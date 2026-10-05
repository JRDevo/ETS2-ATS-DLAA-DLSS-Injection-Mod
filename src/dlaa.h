// DlaaProcessor -- NGX/DLSS "DLAA" (DLSS at native scale, ratio 1.0) on the
// native D3D11 path. Self-contained: knows nothing about how buffers are
// captured (that's v0.2). Compiled only when WITH_DLAA=1 (CMake -DWITH_DLAA=ON).
// v0.7.0: also real DLSS upscaling (render dims < output dims), see Init.
#pragma once
#ifdef WITH_DLAA

#include <d3d11.h>
#include <cstdint>

// Forward-declare NGX types so this header stays cheap to include.
struct NVSDK_NGX_Parameter;
struct NVSDK_NGX_Handle;

// Per-frame inputs for Evaluate. Camera matrices are NOT here -- they feed the
// motion-vector pass (see motion_vectors.h); NGX itself only sees the MV texture.
struct DlaaFrameParams {
    // Sub-pixel jitter applied to this frame's projection, in PIXELS, range
    // [-0.5,+0.5], same axes/direction as motion vectors (origin top-left).
    // ETS2/ATS do NOT jitter their projection; since v0.3.1 inject.cpp jitters the
    // viewport itself (Halton) and passes that offset here (sign configurable).
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    // Multiplied into MV texture samples to reach pixel space. Our MV pass
    // already writes pixels, so (1,1). (NGX treats 0.0 as 1.0 anyway.)
    float mvScaleX = 1.0f;
    float mvScaleY = 1.0f;
    // True on camera cut / teleport / map load: drops DLSS history.
    bool reset = false;
    // True if the depth buffer is reversed-Z (1.0 = near). This is an NGX
    // *creation* flag; flipping it at runtime recreates the feature (logged).
    bool depthInverted = false;
};

class DlaaProcessor {
public:
    // NGX init + capability check + create the DLSS feature. Uses the device's immediate context.
    // Returns false (and logs why) if NGX/DLSS is unavailable -- never throws.
    // On swapchain resize: Shutdown() then Init() with the new dims.
    // v0.7.0: render dims (renderW x renderH = InWidth/InHeight = InRenderSubrectDimensions) and output dims
    // (outW x outH = InTargetWidth/Height). Equal = DLAA exactly as before (PerfQuality DLAA). Output larger =
    // real DLSS upscaling: PerfQuality picked from the per-axis geomean ratio (nearest of 1.3 UltraQuality /
    // 1.5 Quality / 1.7 Balanced / 2.0 Performance / 3.0 UltraPerformance) and MVLowRes set (MV + depth stay
    // at render res, MVs in render pixels). The user's render preset goes on EVERY DLSS hint parameter.
    // v0.8.0: hdr = the colour input is LINEAR HDR (the game's RGBA16F scene with Windows HDR on): the feature is
    // created with NVSDK_NGX_DLSS_Feature_Flags_IsHDR (AutoExposure stays on: no exposure texture). A creation flag:
    // SceneDlaa rebuilds the unit (Shutdown + Init) whenever its HDR state changes. false = exactly the LDR path.
    bool Init(ID3D11Device* dev, uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH,
              bool depthInverted = false, bool hdr = false);

    // Runs DLAA / DLSS. color/depth/motionVectors must be SRV-capable textures at
    // render resolution; output must be UAV-capable at output resolution (guide 3.4). Views are
    // only used to reach the underlying ID3D11Resource. Returns false on
    // failure (logged, rate-limited) -- caller then presents the raw frame.
    bool Evaluate(ID3D11DeviceContext* ctx,
                  ID3D11ShaderResourceView* color,
                  ID3D11ShaderResourceView* depth,
                  ID3D11ShaderResourceView* motionVectors,
                  ID3D11UnorderedAccessView* output,
                  const DlaaFrameParams& p);

    // Release feature + params + NGX. Safe to call twice / without Init.
    void Shutdown();

    bool IsReady() const { return m_feature != nullptr; }

    // v0.5.6: DLAA render preset (dlaa.ini dlss_preset), process-wide (both eyes use it). `letter` = 0 for
    // the driver default, else one of 'J','K','L','M','E','F' (case-insensitive). Applied at the next
    // CreateFeature. Returns false (and keeps the current preset) for anything else.
    // v0.5.7: may be called live (render thread, Shift+End). Every successful call bumps a process-wide
    // generation; each processor's next Evaluate sees the mismatch and recreates its feature with the new
    // hint (history reset). If that create fails, the preset falls back to default once (PresetFallbacks()++).
    static bool SetRenderPreset(char letter);
    static const char* RenderPresetName();     // "default" or "K" etc.
    static uint32_t PresetFallbacks();         // v0.5.7: live preset changes that failed and fell back to default

    void SetEye(int e) { m_eye = e; }          // v0.5.7: log tag only

    // v0.7.8 NGX pre-warm, called once at the first Present (render thread, the game's immediate context, its device):
    // initialises NGX, PINS it (one reference held for the process life, so it never shuts down and is never
    // re-initialised) and creates + releases a throwaway 512x512 DLAA feature with the configured preset, so the
    // one-time cost (~0.7 s: NGX init + first feature / model load) lands on the boot screen instead of the first
    // DLAA unit. Logs the split (init / feature). Later calls do nothing; returns whether NGX is pinned.
    static bool Prewarm(ID3D11Device* dev);
    // v0.8.1: the device NGX is initialised on (identity, nullptr = not initialised). Log only. If a unit later asks
    // for another device while only the pre-warm pin holds NGX, NGX moves to that device (present-layer adoption).
    static void* NgxDevice();
    // v0.7.8 creation budget: at most one NGX feature create per Present. BeginFrame() is called once per Present
    // (after that Present's own work); CreateBudgetFree() = no feature was created since. Every CreateFeature uses
    // it; a live preset recreate waits for a free Present (the old feature keeps evaluating), and SceneDlaa defers a
    // unit (re)build to the next Present.
    static void BeginFrame();
    static bool CreateBudgetFree();

    // v0.7.0: "DLAA", "UltraQuality", "Quality", "Balanced", "Performance", "UltraPerformance" -- the
    // PerfQuality value the current feature was created with (log / snapshot only).
    const char* QualityName() const;
    bool Upscaling() const { return m_outW != m_width || m_outH != m_height; }
    bool Hdr() const { return m_hdr; }         // v0.8.0: the feature was created with IsHDR

private:
    bool CreateFeature(bool depthInverted);   // (re)creates m_feature
    bool CreateFeatureOnce(bool depthInverted, int perfQuality);   // v0.7.0: one NGX create attempt
    const char* SizeText(char* buf, size_t cap) const;   // v0.7.0: "WxH native (DLAA)" / "WxH -> WxH (DLSS Q)"

    ID3D11Device*        m_dev      = nullptr;
    NVSDK_NGX_Parameter* m_params   = nullptr;
    NVSDK_NGX_Handle*    m_feature  = nullptr;
    uint32_t             m_width    = 0;       // render (input) size
    uint32_t             m_height   = 0;
    uint32_t             m_outW     = 0;       // v0.7.0: output (target) size; == render for DLAA
    uint32_t             m_outH     = 0;
    int                  m_perfQ    = 0;       // v0.7.0: NVSDK_NGX_PerfQuality_Value of m_feature (int: header stays NGX-free)
    bool                 m_depthInverted = false;
    bool                 m_hdr          = false;   // v0.8.0: IsHDR creation flag (set by Init, kept across recreates)
    bool                 m_ngxInited    = false;
    uint32_t             m_presetGen    = 0;   // preset generation m_feature was (last tried to be) created with
    int                  m_eye          = -1;
    bool                 m_quiet        = false;   // v0.7.8 pre-warm instance: no "ready" / "sharing" lines (failures still logged)
};

#endif // WITH_DLAA
