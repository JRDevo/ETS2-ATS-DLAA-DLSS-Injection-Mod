// Vrs -- phase 34 fixed foveated rendering (NVIDIA VRS through NVAPI). See vrs.h for the design.
//
// The ring pattern (ellipses in NDC around the projection centre, radii in % of the NDC half-size), the 16 x 16 tile mask of
// shading-rate TABLE INDICES, the per-viewport shading-rate lookup table and the three NVAPI calls
// (NvAPI_D3D11_CreateShadingRateResourceView, NvAPI_D3D11_RSSetViewportsPixelShadingRates, NvAPI_D3D11_RSSetShadingRateResourceView)
// are ported from OpenXR Toolkit (XR_APILAYER_MBUCCHIA_toolkit/vrs.cpp and VRS.hlsl), rewritten for this project: the mask is
// filled on the CPU once (no compute shader), the bracket is the game's world scene passes only, and draws that must stay
// sharp switch the table off for themselves. OpenXR Toolkit's notice:
//
// MIT License
//
// Copyright (c) 2021-2022 Matthieu Bucchianeri
// Copyright (c) 2021-2022 Jean-Luc Dupiot - Reality XP
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstring>
#include <vector>
#include "vrs.h"
#include "cpu_prof.h"                                 // the "vrs" row of the phase 31 CPU profiler (header-only)
#ifndef VRS_NO_LOG
#include "log.h"
#else
#define Log(...) ((void)0)
#define LogErr(...) ((void)0)                       // phase 39: the level macros too
#define LogWarn(...) ((void)0)
#define LogDbg(...) ((void)0)
#endif
#ifdef WITH_VRS
#include <nvapi.h>                                    // external/nvapi (NVIDIA NVAPI SDK, MIT), nvapi64.lib linked statically
#endif

namespace Vrs {

bool g_want = false;

namespace {
std::atomic<int> s_on{0}, s_inner{55}, s_outer{80}, s_quality{1}, s_debug{0}, s_passes{0};
int              s_cls = kClsGbuf;                   // the bind kind of the open bracket (render thread; stats per kind)
uint64_t         s_lastLogFrame = 0;                 // the Present count at the last stats line (per-frame figures)
int              s_support = -1;                     // render thread (Probe)
const char*      s_supportText = "not checked yet (VR world scene not seen)";
ID3D11Device*    s_probedDev = nullptr;              // identity: the device the support state belongs to
Stats            s_stats;
uint64_t         s_lastLogMs = 0;
Stats            s_lastLog;                          // the counters at the last stats line (deltas per window)

#ifdef WITH_VRS
// ---- mask cache: (size, centre, radii) -> texture + NVAPI view. Static per key: never rebuilt per frame. ----
struct Mask {
    uint32_t w = 0, h = 0;                           // render-target px
    int      cuQ = 0, cvQ = 0;                       // centre in 1/1000 uv
    int      inner = 0, outer = 0;                   // %
    ID3D11Texture2D*                 tex = nullptr;
    ID3D11NvShadingRateResourceView* view = nullptr;
    uint64_t lastUse = 0;
};
constexpr int kMaxMasks = 8;                         // 2 eyes x a few sizes (DLSS scale changes) / radii presets
Mask             s_masks[kMaxMasks];
uint64_t         s_useSeq = 0;
ID3D11Device*    s_maskDev = nullptr;                // the device the masks were made on

// bracket state (render thread)
ID3D11DeviceContext* s_ctx = nullptr;                // the context the bracket is on (identity)
DWORD            s_tid = 0;                          // ... and its thread
bool             s_hw = false;                       // the viewport rate table is enabled on s_ctx right now
bool             s_bound = false;                    // a shading-rate view is bound on s_ctx
bool             s_excl = false;                     // the last game draw was excluded (full rate)
bool             s_mod = false;                      // the mod's own work is running inside the bracket
NV_D3D11_VIEWPORT_SHADING_RATE_DESC s_vp[NV_MAX_NUM_VIEWPORTS];   // the table, all viewports (the game may set several)
NV_D3D11_VIEWPORT_SHADING_RATE_DESC s_vpOuter[NV_MAX_NUM_VIEWPORTS];   // phase 42: the outer-only table (extra passes)
NV_D3D11_VIEWPORT_SHADING_RATE_DESC* s_vpCur = s_vp; // the table of the open bracket (SetHw binds this one)
int              s_tableKey = -1;                    // quality | debug | upscale step the tables were built for

void ReleaseMasks() {
    for (Mask& m : s_masks) {
        if (m.view) m.view->Release();
        if (m.tex) m.tex->Release();
        m = Mask();
    }
    s_maskDev = nullptr;
}

const char* RateName(NV_PIXEL_SHADING_RATE r) {
    switch (r) {
    case NV_PIXEL_X1_PER_RASTER_PIXEL:      return "1x1";
    case NV_PIXEL_X1_PER_2X1_RASTER_PIXELS: return "2x1";
    case NV_PIXEL_X1_PER_1X2_RASTER_PIXELS: return "1x2";
    case NV_PIXEL_X1_PER_2X2_RASTER_PIXELS: return "2x2";
    case NV_PIXEL_X1_PER_4X4_RASTER_PIXELS: return "4x4";
    default:                                return "?";
    }
}

// The table for this Enter: index 0 = inner, 1 = middle ring, 2 = outer ring (the mask's values); the rest 1 x 1.
void BuildTable(int quality, int debug, float renderOverOutput) {
    const bool upLow = renderOverOutput > 0.0f && renderOverOutput < 0.75f;
    const int key = (quality ? 1 : 0) | (debug ? 2 : 0) | (upLow ? 4 : 0);
    if (key == s_tableKey) return;
    NV_PIXEL_SHADING_RATE mid, out;
    if (debug)      { mid = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS; out = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS; }   // the pattern, on purpose
    else if (upLow) { mid = NV_PIXEL_X1_PER_RASTER_PIXEL;      out = NV_PIXEL_X1_PER_2X1_RASTER_PIXELS; }   // DLSS already upscales
    else            { mid = quality ? NV_PIXEL_X1_PER_RASTER_PIXEL : NV_PIXEL_X1_PER_2X1_RASTER_PIXELS;
                      out = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS; }
    for (NV_D3D11_VIEWPORT_SHADING_RATE_DESC& v : s_vp) {
        v.enableVariablePixelShadingRate = true;
        for (NV_PIXEL_SHADING_RATE& r : v.shadingRateTable) r = NV_PIXEL_X1_PER_RASTER_PIXEL;
        v.shadingRateTable[1] = mid;
        v.shadingRateTable[2] = out;
    }
    // phase 42: the extra passes (lighting) -- the outer ring only; the middle ring stays 1 x 1 whatever vrs_quality says
    for (NV_D3D11_VIEWPORT_SHADING_RATE_DESC& v : s_vpOuter) {
        v.enableVariablePixelShadingRate = true;
        for (NV_PIXEL_SHADING_RATE& r : v.shadingRateTable) r = NV_PIXEL_X1_PER_RASTER_PIXEL;
        v.shadingRateTable[2] = out;
    }
    const bool first = s_tableKey < 0;
    s_tableKey = key;
    Log("VRS: shading-rate table inner 1x1, middle %s, outer %s (vrs_quality=%d%s%s); extra passes (vrs_passes=1): middle 1x1, "
        "outer %s%s", RateName(mid), RateName(out), quality,
        debug ? ", vrs_debug=1: exaggerated to show the pattern" : "",
        upLow ? ", DLSS upscales from below 75 % of the output width: outer one step finer" : "", RateName(out),
        first ? "" : " -- changed");
}

bool NvOk(NvAPI_Status st, const char* what) {
    if (st == NVAPI_OK) return true;
    ++s_stats.nvFails;
    NvAPI_ShortString msg = {};
    NvAPI_GetErrorMessage(st, msg);
    LogErr("VRS: %s FAILED (NvAPI status %d: %s) -- VRS off for this session", what, (int)st, msg);
    return false;
}

Mask* FindOrBuild(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, float cu, float cv, int inner, int outer, int eye) {
    if (s_maskDev != dev) { ReleaseMasks(); s_maskDev = dev; }
    const int cuQ = (int)std::lround(cu * 1000.0f), cvQ = (int)std::lround(cv * 1000.0f);
    for (Mask& m : s_masks) {
        if (m.view && m.w == w && m.h == h && m.cuQ == cuQ && m.cvQ == cvQ && m.inner == inner && m.outer == outer) {
            m.lastUse = ++s_useSeq;
            return &m;
        }
    }
    // the slot for a new mask: a free one, else the least recently used (never bound now: Enter runs after Leave)
    Mask* lru = nullptr;
    for (Mask& m : s_masks) if (!m.view) { lru = &m; break; }
    if (!lru) { lru = &s_masks[0]; for (Mask& m : s_masks) if (m.lastUse < lru->lastUse) lru = &m; }
    // build: one byte per 16 x 16 tile (NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH / _HEIGHT), rounded up
    const uint32_t tw = (w + NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH - 1) / NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH;
    const uint32_t th = (h + NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT - 1) / NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT;
    std::vector<uint8_t> bytes((size_t)tw * th);
    uint32_t counts[3] = {};
    FillMask(bytes.data(), tw, th, w, h, (float)cuQ / 1000.0f, (float)cvQ / 1000.0f, (float)inner / 100.0f,
             (float)outer / 100.0f, counts);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = tw; td.Height = th; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UINT; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    // NOT as initial data: measured on an RTX 5090 (driver 616.92, scratch vrstest / diag) the shading-rate view of a texture
    // created WITH initial data reads index 0 everywhere (no coarse tile at all), while the same bytes written by the GPU
    // (UpdateSubresource, CopyResource or a UAV clear) work. So: an empty texture + one UpdateSubresource on the game's
    // context, before the view is created.
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &tex);
    if (FAILED(hr) || !tex) {
        LogErr("VRS: mask texture %ux%u tiles FAILED (hr=0x%08lX)", tw, th, (unsigned long)hr);
        return nullptr;
    }
    ctx->UpdateSubresource(tex, 0, nullptr, bytes.data(), tw, 0);
    NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC vd{};
    vd.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
    vd.Format = DXGI_FORMAT_R8_UINT;
    vd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
    vd.Texture2D.MipSlice = 0;
    ID3D11NvShadingRateResourceView* view = nullptr;
    if (!NvOk(NvAPI_D3D11_CreateShadingRateResourceView(dev, tex, &vd, &view), "NvAPI_D3D11_CreateShadingRateResourceView") ||
        !view) {
        tex->Release();
        s_support = 0;
        s_supportText = "the driver refused the shading-rate view";
        return nullptr;
    }
    if (lru->view) lru->view->Release();
    if (lru->tex) lru->tex->Release();
    lru->w = w; lru->h = h; lru->cuQ = cuQ; lru->cvQ = cvQ; lru->inner = inner; lru->outer = outer;
    lru->tex = tex; lru->view = view; lru->lastUse = ++s_useSeq;
    ++s_stats.masksBuilt;
    const double all = (double)tw * th;
    Log("VRS: mask built for eye %d: %ux%u px -> %ux%u tiles of 16 px, centre uv (%.3f, %.3f), radii inner %d %% / outer %d %% "
        "of the NDC half-size -> tiles inner %u (%.1f %%), middle %u (%.1f %%), outer %u (%.1f %%); mask #%llu, cached per "
        "size + centre + radii", eye, w, h, tw, th, (double)cuQ / 1000.0, (double)cvQ / 1000.0, inner, outer, counts[0],
        100.0 * counts[0] / all, counts[1], 100.0 * counts[1] / all, counts[2], 100.0 * counts[2] / all,
        (unsigned long long)s_stats.masksBuilt);
    return lru;
}

void SetHw(ID3D11DeviceContext* ctx, bool on) {
    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC d{};
    d.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
    d.numViewports = on ? NV_MAX_NUM_VIEWPORTS : 0;  // 0 = VRS off for every viewport (NVAPI)
    d.pViewports = on ? s_vpCur : nullptr;
    if (!NvOk(NvAPI_D3D11_RSSetViewportsPixelShadingRates(ctx, &d), "NvAPI_D3D11_RSSetViewportsPixelShadingRates")) {
        s_support = 0;
        s_supportText = "the driver refused the shading-rate table";
        s_hw = false;
        return;
    }
    s_hw = on;
    ++s_stats.switches;
}
#endif // WITH_VRS
} // namespace

void Configure(int on, int innerPct, int outerPct, int quality, int debug, int passes) {
    innerPct = innerPct < 10 ? 10 : (innerPct > 150 ? 150 : innerPct);
    outerPct = outerPct < 10 ? 10 : (outerPct > 150 ? 150 : outerPct);
    if (outerPct < innerPct) outerPct = innerPct;
    s_on.store(on ? 1 : 0, std::memory_order_relaxed);
    s_inner.store(innerPct, std::memory_order_relaxed);
    s_outer.store(outerPct, std::memory_order_relaxed);
    s_quality.store(quality ? 1 : 0, std::memory_order_relaxed);
    s_debug.store(debug ? 1 : 0, std::memory_order_relaxed);
    s_passes.store(passes ? 1 : 0, std::memory_order_relaxed);
}
bool On() { return s_on.load(std::memory_order_relaxed) != 0; }
int Passes() { return s_passes.load(std::memory_order_relaxed); }
const char* ClsName(int cls) {
    switch (cls) {
    case kClsGbuf:  return "G-buffer";
    case kClsFwd:   return "forward";
    case kClsLight: return "lighting";
    default:        return "?";
    }
}
int Support() { return s_support; }
const char* SupportText() { return s_supportText; }
Stats& GetStats() { return s_stats; }
void NoteEyeCheck(bool ok) { ++s_stats.eyeChecks; if (!ok) ++s_stats.eyeMisses; }

void FillMask(uint8_t* dst, uint32_t tw, uint32_t th, uint32_t rtW, uint32_t rtH, float cu, float cv, float rInner,
              float rOuter, uint32_t counts[3]) {
    // OpenXR Toolkit VRS.hlsl: tile centre -> uv -> NDC, minus the centre, inside x^2/a^2 + y^2/b^2 <= 1 per ring (a = b = r:
    // circles in NDC; the eye pictures are nearly square). The tile's centre in PIXELS (the last tile may hang over the edge).
    const float ri2 = rInner * rInner, ro2 = rOuter * rOuter;
    uint32_t c[3] = {};
    for (uint32_t ty = 0; ty < th; ++ty) {
        const float v = ((float)ty * 16.0f + 8.0f) / (float)rtH;
        const float dy = 2.0f * (v - cv);
        for (uint32_t tx = 0; tx < tw; ++tx) {
            const float u = ((float)tx * 16.0f + 8.0f) / (float)rtW;
            const float dx = 2.0f * (u - cu);
            const float d2 = dx * dx + dy * dy;
            const uint8_t r = d2 <= ri2 ? 0 : (d2 <= ro2 ? 1 : 2);
            dst[(size_t)ty * tw + tx] = r;
            ++c[r];
        }
    }
    if (counts) { counts[0] = c[0]; counts[1] = c[1]; counts[2] = c[2]; }
}

int Probe(ID3D11Device* dev) {
#ifdef WITH_VRS
    if (s_probedDev == dev && s_support >= 0) return s_support;
    s_probedDev = dev;
    NvAPI_Status st = NvAPI_Initialize();
    if (st != NVAPI_OK) {
        NvAPI_ShortString msg = {};
        NvAPI_GetErrorMessage(st, msg);
        s_support = 0;
        s_supportText = "no NVIDIA driver (NvAPI_Initialize failed)";
        LogErr("VRS (foveated rendering): NOT available -- NvAPI_Initialize failed (status %d: %s; not an NVIDIA GPU / driver)",
            (int)st, msg);
        return s_support;
    }
    NV_D3D1x_GRAPHICS_CAPS caps{};
    st = NvAPI_D3D1x_GetGraphicsCapabilities(dev, NV_D3D1x_GRAPHICS_CAPS_VER, &caps);
    if (st != NVAPI_OK || !caps.bVariablePixelRateShadingSupported) {
        s_support = 0;
        s_supportText = "this GPU has no variable rate shading (needs NVIDIA RTX / GTX 16xx)";
        LogWarn("VRS (foveated rendering): NOT available -- NvAPI_D3D1x_GetGraphicsCapabilities status %d, "
            "bVariablePixelRateShadingSupported %u (needs Turing or newer)", (int)st, (unsigned)caps.bVariablePixelRateShadingSupported);
        return s_support;
    }
    s_support = 1;
    s_supportText = "available";
    Log("VRS (foveated rendering): available -- NVAPI variable pixel rate shading supported (SM %u.%u, CUDA SM %u.%u), tile %ux%u "
        "px; vrs=%d vrs_inner=%d vrs_outer=%d vrs_quality=%d vrs_debug=%d", (unsigned)caps.majorSMVersion,
        (unsigned)caps.minorSMVersion, (unsigned)caps.majorCudaSMVersion, (unsigned)caps.minorCudaSMVersion,
        NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH, NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT, s_on.load(), s_inner.load(), s_outer.load(),
        s_quality.load(), s_debug.load());
    return s_support;
#else
    (void)dev;
    if (s_support < 0) {
        s_support = 0;
        s_supportText = "not built in (no external/nvapi at build time)";
        LogWarn("VRS (foveated rendering): NOT available -- this build has no NVAPI (external/nvapi missing at build time)");
    }
    return s_support;
#endif
}

bool Enter(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, int eye, float cu, float cv, float renderOverOutput, int cls) {
#ifdef WITH_VRS
    if (g_want) Leave(ctx);                            // (never expected: every game bind leaves first)
    if (!s_on.load(std::memory_order_relaxed) || !ctx || !w || !h) return false;
    if (cls < 0 || cls >= kNCls) cls = kClsGbuf;
    if (cls == kClsLight && !s_passes.load(std::memory_order_relaxed)) return false;   // phase 42: vrs_passes = 0
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    dev->Release();                                    // identity only: the context keeps its device alive
    if (Probe(dev) != 1) return false;
    Mask* m = FindOrBuild(dev, ctx, w, h, cu, cv, s_inner.load(std::memory_order_relaxed), s_outer.load(std::memory_order_relaxed),
                          eye);
    if (!m) return false;
    BuildTable(s_quality.load(std::memory_order_relaxed), s_debug.load(std::memory_order_relaxed), renderOverOutput);
    s_vpCur = cls == kClsLight ? s_vpOuter : s_vp;     // phase 42: an extra pass = outer ring only
    s_cls = cls;
    // the Toolkit's order: the viewport table first, then the shading-rate view
    SetHw(ctx, true);
    if (!s_hw) return false;                            // refused (logged, VRS off for the session)
    if (!NvOk(NvAPI_D3D11_RSSetShadingRateResourceView(ctx, m->view), "NvAPI_D3D11_RSSetShadingRateResourceView")) {
        s_support = 0;
        s_supportText = "the driver refused the shading-rate view";
        SetHw(ctx, false);
        return false;
    }
    s_bound = true;
    s_ctx = ctx;
    s_tid = GetCurrentThreadId();
    s_excl = false;
    s_mod = false;
    g_want = true;
    ++s_stats.enters;
    ++s_stats.entersCls[cls];
    if (eye == 0 || eye == 1) ++s_stats.entersEye[eye];
    return true;
#else
    (void)ctx; (void)w; (void)h; (void)eye; (void)cu; (void)cv; (void)renderOverOutput; (void)cls;
    return false;
#endif
}

void Leave(ID3D11DeviceContext* ctx) {
#ifdef WITH_VRS
    if (!g_want) return;
    g_want = false;
    ID3D11DeviceContext* c = s_ctx ? s_ctx : ctx;
    if (s_hw) SetHw(c, false);
    if (s_bound) { NvAPI_D3D11_RSSetShadingRateResourceView(c, nullptr); s_bound = false; }
    s_hw = false; s_mod = false; s_excl = false;
    s_ctx = nullptr;
    s_vpCur = s_vp; s_cls = kClsGbuf;                  // phase 42
    ++s_stats.leaves;
#else
    (void)ctx;
#endif
}

void OnDraw(ID3D11DeviceContext* ctx, bool exclude, int why) {
#ifdef WITH_VRS
    if (!g_want || ctx != s_ctx) return;
    if (exclude) { if (why >= 0 && why < kSkipCount) ++s_stats.drawsSkip[why]; ++s_stats.drawsFullCls[s_cls]; }
    else { ++s_stats.drawsVrs; ++s_stats.drawsVrsCls[s_cls]; }
    s_excl = exclude;
    const bool want = !exclude && !s_mod;
    if (want != s_hw) SetHw(ctx, want);
#else
    (void)ctx; (void)exclude; (void)why;
#endif
}

void OnModWork(bool begin) {
#ifdef WITH_VRS
    if (!g_want || !s_ctx || GetCurrentThreadId() != s_tid) return;
    s_mod = begin;
    if (begin) ++s_stats.modPauses;
    const bool want = !begin && !s_excl;
    if (want != s_hw) {
        CpuProf::Block cpb(CpuProf::kBVrs);           // charged to "vrs", not to the mod block that paused it
        SetHw(s_ctx, want);
    }
#else
    (void)begin;
#endif
}

void Abandon() {
#ifdef WITH_VRS
    g_want = false;
    s_hw = false; s_bound = false; s_mod = false; s_excl = false;
    s_ctx = nullptr;
    s_vpCur = s_vp; s_cls = kClsGbuf;                  // phase 42
#endif
}

void MaybeLogStats(uint64_t frame) {
    const uint64_t now = GetTickCount64();
    if (!s_lastLogMs) { s_lastLogMs = now; s_lastLogFrame = frame; s_lastLog = s_stats; return; }
    if (now - s_lastLogMs < 10000) return;
    const Stats& c = s_stats;
    const Stats& p = s_lastLog;
    const uint64_t vrs = c.drawsVrs - p.drawsVrs, cut = c.drawsSkip[kSkipCutout] - p.drawsSkip[kSkipCutout];
    const uint64_t dep = c.drawsSkip[kSkipDepth] - p.drawsSkip[kSkipDepth], unk = c.drawsSkip[kSkipUnknown] - p.drawsSkip[kSkipUnknown];
    const uint64_t all = vrs + cut + dep + unk;
    LogDbg("VRS @%.0f s (Present #%llu): %llu scene binds entered (eye 0 %llu / eye 1 %llu), draws coarse-capable %llu (%.1f %%), "
        "full rate: cut-out %llu / SV_Depth %llu / unknown shader %llu; mod-work pauses %llu, table switches %llu, eye prediction "
        "misses %llu of %llu checked blits, masks %llu, NVAPI failures %llu; eye sources (entered binds): blit-RT cycle %llu / "
        "forward colour %llu / frame pair %llu / own blit %llu; binds left alone: no certain eye %llu, sources disagree %llu, "
        "no optical centre yet %llu; GPU support: %s",
        (double)(now - s_lastLogMs) / 1000.0, (unsigned long long)frame, (unsigned long long)(c.enters - p.enters),
        (unsigned long long)(c.entersEye[0] - p.entersEye[0]), (unsigned long long)(c.entersEye[1] - p.entersEye[1]),
        (unsigned long long)vrs, all ? 100.0 * (double)vrs / (double)all : 0.0, (unsigned long long)cut, (unsigned long long)dep,
        (unsigned long long)unk, (unsigned long long)(c.modPauses - p.modPauses), (unsigned long long)(c.switches - p.switches),
        (unsigned long long)(c.eyeMisses - p.eyeMisses), (unsigned long long)(c.eyeChecks - p.eyeChecks),
        (unsigned long long)c.masksBuilt, (unsigned long long)c.nvFails,
        (unsigned long long)(c.eyeSrc[1] - p.eyeSrc[1]), (unsigned long long)(c.eyeSrc[2] - p.eyeSrc[2]),
        (unsigned long long)(c.eyeSrc[3] - p.eyeSrc[3]), (unsigned long long)(c.eyeSrc[0] - p.eyeSrc[0]),
        (unsigned long long)(c.bindNoEye - p.bindNoEye), (unsigned long long)(c.bindDisagree - p.bindDisagree),
        (unsigned long long)(c.bindNoCentre - p.bindNoCentre), s_supportText);
    // phase 42: which passes got VRS, per frame (Presents in the window; both eyes together)
    const double fr = frame > s_lastLogFrame ? (double)(frame - s_lastLogFrame) : 0.0;
    auto pf = [&](uint64_t a, uint64_t b) { return fr > 0.0 ? (double)(a - b) / fr : 0.0; };
    LogDbg("VRS passes @%.0f s (vrs_passes=%d; per frame, both eyes, %.0f frames): G-buffer %.2f binds / %.0f coarse-capable draws / "
        "%.0f full-rate draws | forward %.2f / %.0f / %.0f | lighting %.2f / %.0f / %.0f | lighting-stage binds %.2f, of them not "
        "the lighting shape %.2f (left at full rate)", (double)(now - s_lastLogMs) / 1000.0, s_passes.load(std::memory_order_relaxed),
        fr, pf(c.entersCls[kClsGbuf], p.entersCls[kClsGbuf]), pf(c.drawsVrsCls[kClsGbuf], p.drawsVrsCls[kClsGbuf]),
        pf(c.drawsFullCls[kClsGbuf], p.drawsFullCls[kClsGbuf]), pf(c.entersCls[kClsFwd], p.entersCls[kClsFwd]),
        pf(c.drawsVrsCls[kClsFwd], p.drawsVrsCls[kClsFwd]), pf(c.drawsFullCls[kClsFwd], p.drawsFullCls[kClsFwd]),
        pf(c.entersCls[kClsLight], p.entersCls[kClsLight]), pf(c.drawsVrsCls[kClsLight], p.drawsVrsCls[kClsLight]),
        pf(c.drawsFullCls[kClsLight], p.drawsFullCls[kClsLight]), pf(c.litStageBinds, p.litStageBinds),
        pf(c.litShapeMiss, p.litShapeMiss));
    s_lastLogMs = now;
    s_lastLogFrame = frame;
    s_lastLog = s_stats;
}

} // namespace Vrs
