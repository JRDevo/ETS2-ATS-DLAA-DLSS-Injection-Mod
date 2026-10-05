// DlaaProcessor implementation. NGX call sequence (verified against the
// public NVIDIA DLSS SDK headers + DLSS Programming Guide, SDK repo
// github.com/NVIDIA/DLSS):
//   Init:      NVSDK_NGX_D3D11_Init_with_ProjectID
//              NVSDK_NGX_D3D11_GetCapabilityParameters -> SuperSampling.Available
//              NVSDK_NGX_D3D11_AllocateParameters
//              set DLSS.Hint.Render.Preset.DLAA
//              NGX_D3D11_CREATE_DLSS_EXT (in==out dims, PerfQuality=DLAA; v0.7.0: in<out = DLSS upscaling,
//              PerfQuality from the ratio, MVLowRes, the preset on every hint parameter)
//   Per frame: NGX_D3D11_EVALUATE_DLSS_EXT
//   Shutdown:  NVSDK_NGX_D3D11_ReleaseFeature, NVSDK_NGX_D3D11_DestroyParameters,
//              NVSDK_NGX_D3D11_Shutdown1
// Requires nvngx_dlss.dll reachable at runtime -- we add our own DLL's folder
// to the NGX search path, so drop it next to dinput8.dll.
#ifdef WITH_DLAA

#include "dlaa.h"
#include "log.h"
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <cmath>
#include <cstdio>

namespace {

// Any stable GUID works for unregistered apps (public API for engines without
// an NVIDIA-assigned AppId). Ours, generated for this project:
constexpr char kProjectId[]     = "8b7e9e5a-2f61-4c8e-9d3a-6f0b1c4d7a52";
constexpr char kEngineVersion[] = "0.3.1";

// NGX itself (Init/Shutdown) is process-wide: with one DlaaProcessor per VR eye it must be
// initialised once and shut down when the LAST processor lets go (v0.5.0). Render thread only.
int                g_ngxRefs = 0;
ID3D11Device*      g_ngxDev  = nullptr;     // device NGX was initialised with (compare/Shutdown1 only)

// v0.5.6: render preset for PerfQuality==DLAA (dlaa.ini dlss_preset). Default (0) = driver's pick.
constexpr int      kMenuEyeTag  = 10;              // eye tags 10.. = menu / truck-preview units (inject.cpp)
unsigned int       g_preset     = NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
char               g_presetName[8] = "default";
// v0.5.7: bumped by every successful SetRenderPreset; a processor whose feature was created with an older
// generation recreates it at its next Evaluate (live Shift+End cycle). Render thread only.
uint32_t           g_presetGen  = 0;
uint32_t           g_presetFallbacks = 0;     // live preset creates that failed -> default

// v0.7.8 creation budget: at most ONE NGX feature create per Present (BeginFrame), so two units (VR: two eyes, two
// preview targets) or two live preset recreates never stack into one long frame. Render thread only.
uint64_t           g_frameNo    = 1;          // advanced by BeginFrame (Present)
uint64_t           g_createFrame = 0;         // g_frameNo of the last CreateFeature
// v0.7.8 NGX pre-warm: one extra NGX reference held for the process life (never released), taken by Prewarm at the
// first Present. NGX therefore never shuts down when the last unit lets go (a rebuild / area / upscale change no
// longer pays the ~0.7 s re-init), and every unit's Init only shares it.
bool               g_ngxPinned  = false;

inline int64_t QpcNow() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
inline double  QpcMs(int64_t a, int64_t b) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    return f.QuadPart > 0 ? (double)(b - a) * 1000.0 / (double)f.QuadPart : 0.0;
}

// v0.7.0: PerfQuality for render -> output. Equal dims = DLAA. Otherwise the per-axis geometric-mean ratio
// sqrt((outW/renderW) * (outH/renderH)) picks the NEAREST of the standard DLSS ratios.
struct QualityChoice { double ratio; NVSDK_NGX_PerfQuality_Value v; const char* name; };
const QualityChoice kQualities[] = {
    // v0.7.6: no UltraQuality row. NGX rejects it (FAIL_UnsupportedParameter, seen in ETS2 and ATS, DLSS
    // 310.7 / 310.9.1): every switch to DLSS cost a failed create + a retry as Quality (~0.5 s hitch per eye).
    { 1.5, NVSDK_NGX_PerfQuality_Value_MaxQuality,       "Quality" },
    { 1.7, NVSDK_NGX_PerfQuality_Value_Balanced,         "Balanced" },
    { 2.0, NVSDK_NGX_PerfQuality_Value_MaxPerf,          "Performance" },
    { 3.0, NVSDK_NGX_PerfQuality_Value_UltraPerformance, "UltraPerformance" },
};
NVSDK_NGX_PerfQuality_Value PickPerfQuality(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    if (rw == ow && rh == oh) return NVSDK_NGX_PerfQuality_Value_DLAA;
    const double r = std::sqrt(((double)ow / (double)rw) * ((double)oh / (double)rh));
    int best = 0;
    for (int i = 1; i < (int)(sizeof(kQualities) / sizeof(kQualities[0])); ++i)
        if (std::fabs(r - kQualities[i].ratio) < std::fabs(r - kQualities[best].ratio)) best = i;
    return kQualities[best].v;
}
const char* PerfQualityName(int v) {
    if (v == NVSDK_NGX_PerfQuality_Value_DLAA) return "DLAA";
    for (const QualityChoice& q : kQualities) if ((int)q.v == v) return q.name;
    return "?";
}

// GetNGXResultAsString returns wchar_t*; squeeze into a narrow buffer for Log.
const char* NgxErr(NVSDK_NGX_Result r) {
    static thread_local char buf[160];
    snprintf(buf, sizeof(buf), "0x%08x (%ls)", (unsigned)r, GetNGXResultAsString(r));
    return buf;
}

// Directory containing THIS dll (dinput8.dll), wide, trailing slash stripped.
bool ThisDllDir(wchar_t* out, DWORD cap) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&ThisDllDir, &self)) return false;
    DWORD n = GetModuleFileNameW(self, out, cap);
    if (n == 0 || n >= cap) return false;
    wchar_t* slash = wcsrchr(out, L'\\');
    if (slash) *slash = 0;
    return true;
}

// NGX itself (process-wide): the first user initialises it, later users share it (+1 reference). v0.7.8: split out
// of Init so the pre-warm can take a reference of its own. false = init failed (logged, no reference taken).
bool NgxAcquire(ID3D11Device* dev, bool quiet) {
    // v0.8.1: NGX is up on ANOTHER device. Happens when the pre-warm ran on a present layer's presenting device (driver
    // frame generation not recognised by name) before the game's render context was adopted. Sharing it would hand NGX a
    // context of a device it was not initialised with. If only the pre-warm pin holds it, NGX is shut down there and
    // initialised on this device (pinned again); with live units on the old device it is refused (they belong there).
    if (g_ngxRefs > 0 && dev && dev != g_ngxDev) {
        if (g_ngxPinned && g_ngxRefs == 1) {
            Log("DLAA: NGX is initialised on device %p (the pre-warm), but a unit needs device %p -- shutting NGX down "
                "on the old device and initialising it on the new one (pinned again)", (void*)g_ngxDev, (void*)dev);
            NVSDK_NGX_D3D11_Shutdown1(g_ngxDev);
            g_ngxRefs = 0; g_ngxDev = nullptr; g_ngxPinned = false;
            if (!NgxAcquire(dev, quiet)) return false;   // fresh init on dev, 1 reference (the caller's)
            ++g_ngxRefs;                                 // + the pin again
            g_ngxPinned = true;
            return true;
        }
        Log("DLAA: NGX is initialised on device %p with %d user(s), a unit asks for device %p -- refused (restart the "
            "game)", (void*)g_ngxDev, g_ngxRefs, (void*)dev);
        return false;
    }
    if (g_ngxRefs > 0) {
        ++g_ngxRefs;                           // NGX already up (other eye / the pre-warm pin): share it
        if (!quiet) Log("DLAA: NGX already initialised, sharing it (%d users%s)", g_ngxRefs,
                        g_ngxPinned ? " incl. the pre-warm pin" : "");
        return true;
    }
    // nvngx_dlss.dll search path = our own folder (game exe folder is searched
    // by default; this lets the snippet sit next to dinput8.dll instead).
    static wchar_t dllDir[MAX_PATH];
    NVSDK_NGX_FeatureCommonInfo fci{};
    const wchar_t* paths[1] = { dllDir };
    if (ThisDllDir(dllDir, MAX_PATH)) {
        fci.PathListInfo.Path   = paths;
        fci.PathListInfo.Length = 1;
    }
    const NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion,
        dllDir, dev, &fci, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) {
        Log("DLAA: NGX init failed %s (non-NVIDIA GPU or driver too old?)", NgxErr(r));
        return false;
    }
    g_ngxDev = dev;
    g_ngxRefs = 1;
    return true;
}

} // namespace

void DlaaProcessor::BeginFrame() { ++g_frameNo; }
void* DlaaProcessor::NgxDevice() { return (void*)g_ngxDev; }
bool DlaaProcessor::CreateBudgetFree() { return g_createFrame != g_frameNo; }

// v0.7.8: see dlaa.h. The pin is taken once; a failed NGX init is not retried (no NVIDIA GPU: Init fails the same way
// later and logs it there).
bool DlaaProcessor::Prewarm(ID3D11Device* dev) {
    static bool tried = false;
    if (tried || !dev) return g_ngxPinned;
    tried = true;
    const bool fresh = g_ngxRefs == 0;
    const int64_t t0 = QpcNow();
    if (!NgxAcquire(dev, true)) {
        Log("DLAA: NGX pre-warm FAILED after %.1f ms (see the line above) -- units initialise NGX themselves", QpcMs(t0, QpcNow()));
        return false;
    }
    g_ngxPinned = true;
    const int64_t t1 = QpcNow();
    // Throwaway feature with the configured preset: the first feature create of a process also loads the DLSS model;
    // after this one every real feature create is only its own (small) cost.
    constexpr uint32_t kSide = 512;
    DlaaProcessor tmp;
    tmp.m_quiet = true;
    tmp.SetEye(-1);
    const bool featOk = tmp.Init(dev, kSide, kSide, kSide, kSide, /*depthInverted=*/true);
    const int64_t t2 = QpcNow();
    tmp.Shutdown();                            // feature + params released; the pin keeps NGX up
    const int64_t t3 = QpcNow();
    Log("DLAA: NGX pre-warm at the first Present: %.1f ms -- NGX %s %.1f ms, throwaway %ux%u DLAA feature (preset=%s) "
        "create %s %.1f ms, release %.1f ms; NGX stays initialised for the process (pinned), later unit creates only "
        "pay their own feature", QpcMs(t0, t3), fresh ? "init" : "share", QpcMs(t0, t1), kSide, kSide, g_presetName,
        featOk ? "OK" : "FAILED", QpcMs(t1, t2), QpcMs(t2, t3));
    return true;
}

bool DlaaProcessor::Init(ID3D11Device* dev, uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH,
                         bool depthInverted, bool hdr) {
    if (m_ngxInited) { Log("DLAA: Init called twice -- call Shutdown first"); return false; }
    if (!dev || !renderW || !renderH || outW < renderW || outH < renderH) { Log("DLAA: bad Init args"); return false; }

    m_dev = dev; m_dev->AddRef();
    m_width = renderW; m_height = renderH;
    m_outW = outW; m_outH = outH;                      // v0.7.0: == render for DLAA
    m_hdr = hdr;                                       // v0.8.0: IsHDR for every (re)create of this feature

    if (!NgxAcquire(m_dev, m_quiet)) { Shutdown(); return false; }
    m_ngxInited = true;

    // Capability check: is the DLSS feature available on this GPU/driver?
    NVSDK_NGX_Parameter* caps = nullptr;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_GetCapabilityParameters(&caps);
    if (NVSDK_NGX_FAILED(r) || !caps) {
        Log("DLAA: GetCapabilityParameters failed %s", NgxErr(r));
        Shutdown();
        return false;
    }
    int available = 0;
    NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (!available) {
        int needsDriver = 0, minMajor = 0, minMinor = 0, initResult = 0;
        NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
        NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minMajor);
        NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minMinor);
        NVSDK_NGX_Parameter_GetI(caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &initResult);
        Log("DLAA: DLSS not available (needsDriver=%d min=%d.%d initResult=0x%x)",
            needsDriver, minMajor, minMinor, (unsigned)initResult);
        NVSDK_NGX_D3D11_DestroyParameters(caps);
        Shutdown();
        return false;
    }
    NVSDK_NGX_D3D11_DestroyParameters(caps);

    r = NVSDK_NGX_D3D11_AllocateParameters(&m_params);
    if (NVSDK_NGX_FAILED(r) || !m_params) {
        Log("DLAA: AllocateParameters failed %s", NgxErr(r));
        Shutdown();
        return false;
    }

    if (!CreateFeature(depthInverted)) { Shutdown(); return false; }
    char sz[96];
    if (!m_quiet)
        Log("DLAA: ready -- %s, preset=%s%s", SizeText(sz, sizeof(sz)), m_eye >= kMenuEyeTag ? "E" : g_presetName,
            m_eye >= kMenuEyeTag ? " (menu units always use E)" :
            g_preset == NVSDK_NGX_DLSS_Hint_Render_Preset_Default ? " (driver pick)" : "");
    return true;
}

const char* DlaaProcessor::QualityName() const { return PerfQualityName(m_perfQ); }

// v0.7.0: DLAA keeps the v0.6.5 wording ("WxH native (DLAA)"); upscaling names render -> output and the mode.
// v0.8.0: " HDR input (IsHDR)" appended for an HDR feature (LDR text unchanged).
const char* DlaaProcessor::SizeText(char* buf, size_t cap) const {
    if (!Upscaling())
        snprintf(buf, cap, "%ux%u native (DLAA)%s", m_width, m_height, m_hdr ? ", HDR input (IsHDR)" : "");
    else
        snprintf(buf, cap, "%ux%u -> %ux%u (DLSS %s, ratio x%.3f / y%.3f, MVLowRes%s)", m_width, m_height, m_outW, m_outH,
                 PerfQualityName(m_perfQ), (double)m_outW / (double)m_width, (double)m_outH / (double)m_height,
                 m_hdr ? ", HDR input (IsHDR)" : "");
    return buf;
}

bool DlaaProcessor::SetRenderPreset(char letter) {
    unsigned int v;
    switch (letter >= 'a' && letter <= 'z' ? (char)(letter - 'a' + 'A') : letter) {
    case 0:   v = NVSDK_NGX_DLSS_Hint_Render_Preset_Default; break;
    case 'J': v = NVSDK_NGX_DLSS_Hint_Render_Preset_J; break;
    case 'K': v = NVSDK_NGX_DLSS_Hint_Render_Preset_K; break;
    case 'L': v = NVSDK_NGX_DLSS_Hint_Render_Preset_L; break;
    case 'M': v = NVSDK_NGX_DLSS_Hint_Render_Preset_M; break;
    case 'E': v = NVSDK_NGX_DLSS_Hint_Render_Preset_E; break;     // deprecated in the SDK, still accepted
    case 'F': v = NVSDK_NGX_DLSS_Hint_Render_Preset_F; break;     // deprecated in the SDK, still accepted
    default:  return false;
    }
    g_preset = v;
    if (v == NVSDK_NGX_DLSS_Hint_Render_Preset_Default) snprintf(g_presetName, sizeof(g_presetName), "default");
    else snprintf(g_presetName, sizeof(g_presetName), "%c", letter >= 'a' ? (char)(letter - 'a' + 'A') : letter);
    ++g_presetGen;                                     // v0.5.7: live features recreate at their next Evaluate
    return true;
}

const char* DlaaProcessor::RenderPresetName() { return g_presetName; }
uint32_t DlaaProcessor::PresetFallbacks() { return g_presetFallbacks; }

bool DlaaProcessor::CreateFeature(bool depthInverted) {
    if (m_feature) { NVSDK_NGX_D3D11_ReleaseFeature(m_feature); m_feature = nullptr; }
    m_presetGen = g_presetGen;                         // also on failure: no per-frame retry storm
    g_createFrame = g_frameNo;                         // v0.7.8: this Present's create budget is used

    // Render preset. Default (0) = driver's pick (currently preset K for DLAA/Quality/Balanced -- see
    // nvsdk_ngx_defs.h). v0.5.6: dlaa.ini dlss_preset. v0.7.0: written to EVERY DLSS hint parameter, so the
    // Shift+F1..F4 model choice applies whatever PerfQuality the render -> output ratio selects (NGX only
    // reads the hint of the feature's own PerfQuality; DLAA reads ..._DLAA exactly as before).
    // v0.7.8: menu / truck-preview units (eye tag >= kMenuEyeTag) always use preset E -- the model keys change the
    // drive only (default/K on the full eye-sized menu picture cost 3.5 ms per eye and smudged).
    const unsigned int preset = m_eye >= kMenuEyeTag ? (unsigned int)NVSDK_NGX_DLSS_Hint_Render_Preset_E : g_preset;
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, preset);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, preset);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, preset);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, preset);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, preset);
    NVSDK_NGX_Parameter_SetUI(m_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, preset);

    const NVSDK_NGX_PerfQuality_Value pq = PickPerfQuality(m_width, m_height, m_outW, m_outH);
    if (CreateFeatureOnce(depthInverted, (int)pq)) return true;
    // v0.7.0: some DLSS builds reject UltraQuality at creation -- one retry with Quality (same dims, same
    // preset hint) before giving up. DLAA / the other modes are not retried (same behaviour as v0.6.5).
    if (pq == NVSDK_NGX_PerfQuality_Value_UltraQuality) {
        Log("DLAA: CreateFeature with PerfQuality UltraQuality failed on eye %d -- retrying with Quality", m_eye);
        return CreateFeatureOnce(depthInverted, (int)NVSDK_NGX_PerfQuality_Value_MaxQuality);
    }
    return false;
}

bool DlaaProcessor::CreateFeatureOnce(bool depthInverted, int perfQuality) {
    if (m_feature) { NVSDK_NGX_D3D11_ReleaseFeature(m_feature); m_feature = nullptr; }

    // Flags: MVs are unjittered; color is the post-tonemap LDR image (not IsHDR). AutoExposure spares us an
    // exposure texture; it's ignored for LDR input anyway. v0.7.0: MVLowRes when render != output (our MV
    // texture and depth stay at render res, MVs in render pixels, MVScale 1); DLAA (equal dims): MVs are
    // "display res" = render res, no MVLowRes, exactly as before.
    // v0.8.0: IsHDR when the colour input is the game's linear RGBA16F HDR scene (Windows HDR on); AutoExposure then
    // supplies the exposure DLSS needs for HDR input. LDR: flags exactly as before.
    int flags = NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    if (depthInverted) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (Upscaling())   flags |= NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (m_hdr)         flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;

    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth            = m_width;   // render size (v0.7.0: == target only for DLAA)
    cp.Feature.InHeight           = m_height;
    cp.Feature.InTargetWidth      = m_outW;
    cp.Feature.InTargetHeight     = m_outH;
    cp.Feature.InPerfQualityValue = (NVSDK_NGX_PerfQuality_Value)perfQuality;
    cp.InFeatureCreateFlags       = flags;

    ID3D11DeviceContext* ctx = nullptr;
    m_dev->GetImmediateContext(&ctx);
    NVSDK_NGX_Result r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &m_feature, m_params, &cp);
    ctx->Release();
    if (NVSDK_NGX_FAILED(r) || !m_feature) {
        Log("DLAA: CreateFeature failed %s (%ux%u -> %ux%u, PerfQuality %s)%s", NgxErr(r), m_width, m_height,
            m_outW, m_outH, PerfQualityName(perfQuality), m_hdr ? " [HDR input: IsHDR flag set]" : "");   // v0.8.0 tag
        m_feature = nullptr;
        return false;
    }
    m_perfQ = perfQuality;
    m_depthInverted = depthInverted;
    return true;
}

bool DlaaProcessor::Evaluate(ID3D11DeviceContext* ctx,
                             ID3D11ShaderResourceView* color,
                             ID3D11ShaderResourceView* depth,
                             ID3D11ShaderResourceView* motionVectors,
                             ID3D11UnorderedAccessView* output,
                             const DlaaFrameParams& p) {
    if (!m_params || !m_dev || !ctx || !color || !depth || !motionVectors || !output) return false;

    // v0.5.7: live preset change (Shift+End) -> recreate with the new hint before this evaluate. Checked
    // before the m_feature test so a feature lost to a failed create comes back at the next change.
    // v0.7.8: one create per Present -- when another unit already created a feature this Present, this unit keeps
    // evaluating with its current (old preset) feature and recreates at its next Present (VR: the two eyes recreate
    // in consecutive frames instead of ~2 x 40 ms in one). A unit without a feature always recreates.
    bool recreated = false;
    if (m_eye >= kMenuEyeTag && m_feature) m_presetGen = g_presetGen;   // menu unit: fixed preset, no recreate
    if (m_presetGen != g_presetGen && (!m_feature || CreateBudgetFree())) {
        if (!CreateFeature(m_depthInverted)) {
            if (g_preset != NVSDK_NGX_DLSS_Hint_Render_Preset_Default) {
                Log("DLAA: CreateFeature for preset %s FAILED on eye %d -- falling back to default", g_presetName, m_eye);
                SetRenderPreset(0);                    // bumps the generation: the other eye follows
                ++g_presetFallbacks;
                if (!CreateFeature(m_depthInverted)) {
                    Log("DLAA: CreateFeature for preset default ALSO FAILED on eye %d -- no DLSS on this eye until the next preset change", m_eye);
                    return false;
                }
            } else {
                Log("DLAA: CreateFeature for preset default FAILED on eye %d -- no DLSS on this eye until the next preset change", m_eye);
                return false;
            }
        }
        recreated = true;
        char sz[96];
        Log("DLAA: ready (eye %d) -- %s, preset=%s%s -- feature recreated for a live preset change",
            m_eye, SizeText(sz, sizeof(sz)), g_presetName,
            g_preset == NVSDK_NGX_DLSS_Hint_Render_Preset_Default ? " (driver pick)" : "");
    }
    if (!m_feature) return false;

    // DepthInverted is a creation flag -- recreate if the caller flipped it.
    if (p.depthInverted != m_depthInverted) {
        Log("DLAA: depthInverted %d -> %d, recreating feature", m_depthInverted, p.depthInverted);
        if (!CreateFeature(p.depthInverted)) return false;
    }

    // NGX D3D11 takes raw resources; views are just how the caller hands them in.
    ID3D11Resource *colorRes = nullptr, *depthRes = nullptr, *mvRes = nullptr, *outRes = nullptr;
    color->GetResource(&colorRes);
    depth->GetResource(&depthRes);
    motionVectors->GetResource(&mvRes);
    output->GetResource(&outRes);

    NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
    ep.Feature.pInColor  = colorRes;
    ep.Feature.pInOutput = outRes;
    ep.Feature.InSharpness = 0.0f;            // sharpening is deprecated in DLSS
    ep.pInDepth          = depthRes;
    ep.pInMotionVectors  = mvRes;
    ep.InJitterOffsetX   = p.jitterX;         // viewport jitter (inject.cpp)
    ep.InJitterOffsetY   = p.jitterY;
    ep.InMVScaleX        = p.mvScaleX;        // MV pass writes pixels -> 1.0
    ep.InMVScaleY        = p.mvScaleY;
    ep.InReset           = (p.reset || recreated) ? 1 : 0;   // a fresh feature has no history anyway
    ep.InRenderSubrectDimensions = { m_width, m_height };   // v0.7.0: render size (output size = the feature's target)

    NVSDK_NGX_Result r = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, m_feature, m_params, &ep);

    colorRes->Release(); depthRes->Release(); mvRes->Release(); outRes->Release();

    if (NVSDK_NGX_FAILED(r)) {
        static uint64_t fails = 0;                       // don't spam the log
        if (fails < 20 || (fails % 600 == 0))
            Log("DLAA: EvaluateFeature failed %s (fail #%llu)%s", NgxErr(r), (unsigned long long)fails,
                m_hdr ? " [HDR input: RGBA16F colour, IsHDR]" : "");   // v0.8.0 tag (LDR line unchanged)
        ++fails;
        return false;
    }
    return true;
}

void DlaaProcessor::Shutdown() {
    if (m_feature) { NVSDK_NGX_D3D11_ReleaseFeature(m_feature); m_feature = nullptr; }
    if (m_params)  { NVSDK_NGX_D3D11_DestroyParameters(m_params); m_params = nullptr; }
    if (m_ngxInited) {
        m_ngxInited = false;
        if (g_ngxRefs > 0 && --g_ngxRefs == 0) {      // last user: shut NGX down
            NVSDK_NGX_D3D11_Shutdown1(g_ngxDev ? g_ngxDev : m_dev);
            g_ngxDev = nullptr;
        }
    }
    if (m_dev)     { m_dev->Release(); m_dev = nullptr; }
    m_width = m_height = 0;
    m_outW = m_outH = 0;
    m_perfQ = 0;
    m_depthInverted = false;
    m_hdr = false;
}

#endif // WITH_DLAA
