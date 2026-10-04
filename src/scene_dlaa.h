// SceneDlaa -- the per-frame DLAA step for the pre-UI tonemap output (v0.4.2). v0.5.0: one instance
// per VR eye (own NGX feature, textures, CameraMv); NGX init/shutdown itself is shared (dlaa.cpp).
// Owns the private textures (color in, depth twin + R32F depth, motion vectors,
// output) and the CameraMv reprojection (zero MVs when disabled), the depth-convert compute shader, and a DlaaProcessor; runs the
// whole thing inside a save/restore of the compute-stage state so the game's
// next draw never sees our bindings. Compiled only when WITH_DLAA=1.
// v0.5.6: per-stage GPU timestamps, dlss_preset plumbing, optional RCAS sharpen pass after NGX.
// v0.5.7: live preset / sharpness, timing window reset on change, depth convert merged into the MV pass.
// v0.6.4: DLAA AREA. The whole pipeline (copy-in, depth + MV, NGX, sharpen, copy-back) runs on a centred rect
// of the image (dlaa_area % of width and of height, centred on the eye's optical centre); colorIn / R32F depth /
// MV / out / sharp and the NGX feature are CROP-sized, the depth twins stay full-size. At area < 100 the RCAS
// pass always runs and blends the result into the raw frame over a dlaa_area_feather px border.
// OUTSIDE the rect the image is the raw jittered frame (no AA there).
// v0.7.0: DLSS UPSCALING. Run(..., outW, outH) with an output (the game's blit viewport / RT) larger than the
// tonemap in at least one axis: colorIn / R32F depth / MV stay render-size (crop-sized), m_out / m_sharp and
// the NGX target are OUTPUT-size (the crop rect mapped into output space), nothing is copied back over the
// tonemap. After the game's own blit Draw (which bilinear-stretches the raw frame into the eye RT) the caller
// runs Composite(): our VS/PS draws the result into the still-bound RT0 with the feather ramp as alpha.
// outW = outH = 0 (or equal to the tonemap) = the v0.6.5 path, unchanged.
#pragma once
#ifdef WITH_DLAA

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include "dlaa.h"
#include "motion_vectors.h"

// A per-pass copy of the scene depth (v0.5.4): R32G8X24_TYPELESS + depth-plane SRV at scene dims, created lazily.
// The pass FIFO slots own one each; SceneDlaa::Run converts it directly (no own copy).
struct DepthTwin {
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          tex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;   // R32_FLOAT_X8X24_TYPELESS
    uint32_t w = 0, h = 0;
    bool     valid = false;                                 // holds a snapshot not yet consumed
    // Copies `depth` (the live scene depth) into the twin NOW (before the game clears / discards it).
    // v0.5.6: `timing` = wrap the copy in a GPU timestamp pair (shared ring, non-blocking readback), logged
    // as "depth snapshot GPU cost" every 600 timed snapshots.
    bool Snapshot(ID3D11DeviceContext* ctx, ID3D11Texture2D* depth, bool timing = false);
    void Shutdown() { srv.Reset(); tex.Reset(); w = h = 0; valid = false; }
};

class SceneDlaa {
public:
    // Runs DLAA on `tonemap` (R8G8B8A8_UNORM_SRGB, render res, the pre-UI LDR
    // scene) using `sceneDepth` (D32_FLOAT_S8X24_UINT, same dims, reversed-Z)
    // and camera-reprojection motion vectors (CameraMv) when `useMv`, else zero
    // MVs. (jitterX, jitterY) is the sub-pixel offset (pixels,
    // NGX sign convention) the frame was rasterized with; `reset` drops history.
    // On success the AA'd image is copied back over
    // `tonemap`; on any failure `tonemap` is left untouched. ctx must be the
    // immediate context the game is drawing on. Lazily (re)initialises NGX and
    // the private textures on first use / dimension change. Never throws.
    // v0.5.4: `depthTwin` (nullable) = the pass's own pre-clear snapshot (R32G8X24_TYPELESS copy, valid, same
    // dims): it is only converted, `sceneDepth` is not touched. Null / invalid / wrong dims = copy the live
    // `sceneDepth` into our own twin first.
    // `cand` = the candidate record of the pass being blitted (matched against this eye's previous committed
    // record, then committed as the new prev). Consumes `depthTwin->valid`.
    // v0.6.1 `candBad`: the caller classified `cand` as incomplete; CameraMv::Generate reuses the last good
    // reprojection instead of matching (no world-miss, so no forced history reset; see motion_vectors.h).
    // v0.7.0 (outW, outH): full OUTPUT size (the game's blit viewport in the eye RT / backbuffer). Non-zero AND
    // >= the tonemap in both axes AND larger in one = DLSS upscaling: no copy-back; on success the result
    // waits for Composite() (CompositePending()). 0/0 = DLAA in place (v0.6.5 path).
    bool Run(ID3D11DeviceContext* ctx, ID3D11Texture2D* tonemap, ID3D11Texture2D* sceneDepth,
             DepthTwin* depthTwin, const CandidateRecord* cand,
             float jitterX, float jitterY, bool reset, bool useMv, bool mvDebug = false, bool candBad = false,
             uint32_t outW = 0, uint32_t outH = 0);

    // v0.7.0 upscale composite. Call right AFTER the game's blit Draw, with that Draw's OM binding still in
    // place (RT0 = the eye RT / backbuffer). Draws the DLSS result into RT0 at (vpX, vpY) + the output rect
    // origin, size = the output rect: own fullscreen-triangle VS + PS (Load, 1:1), alpha blend with the
    // feather ramp as alpha (1 everywhere at area 100), RGB write mask (RT0's alpha untouched), RT1..7 not
    // written. srgbDecode = the game's blit SRV0 is an _SRGB view: the PS decodes our raw (encoded) texels to
    // linear so the RTV's own encode (if any) reproduces the game's blit exactly. Saves and restores every
    // piece of graphics state it touches. Must run inside the caller's re-entrancy guard (our Draw goes
    // through the hooked Draw). Ends the frame's GPU timing (stage "composite"). Returns false = nothing drawn.
    bool CompositePending() const { return m_compPending; }
    bool Composite(ID3D11DeviceContext* ctx, uint32_t vpX, uint32_t vpY, bool srgbDecode);
    void CancelComposite(ID3D11DeviceContext* ctx);   // pending result dropped (RT changed): timing slot closed
    bool Upscaling() const { return m_up; }           // v0.7.0: the last Ensure built the upscale path
    bool InitFailed() const { return m_failed; }      // v0.7.0: the last Ensure failed (no retry at these dims)
    // v0.7.8: the last Run returned false WITHOUT doing anything and will simply work later: the shader warm-up was
    // not finished, or the unit's (re)build waited for a Present with a free creation budget (one NGX feature create
    // per Present, see DlaaProcessor::CreateBudgetFree). Nothing was touched, nothing is marked failed.
    bool Deferred() const { return m_deferred; }

    // v0.7.8 one-time work moved off the first Run:
    //  RegisterShaders(): the depth-convert / RCAS / MV-debug / composite sources into ShaderCache (setup thread,
    //    before ShaderCache::Start). ShadersReady(): the warm-up is done -- Run returns false (Deferred) until then;
    //    callers test it first so a unit just does not run.
    //  PrewarmNgx(ctx): DlaaProcessor::Prewarm inside a save / restore of the compute stage (render thread, the
    //    game's immediate context, first Present). Returns whether NGX is pinned.
    static void RegisterShaders();
    static bool ShadersReady();
    static bool PrewarmNgx(ID3D11DeviceContext* ctx);
    // v0.7.0 output-space rect of the last Run (origin in the full output, size = m_out / m_sharp / NGX target).
    uint32_t OutRectX() const { return m_oxc; }
    uint32_t OutRectY() const { return m_oyc; }
    uint32_t OutRectW() const { return m_ow; }
    uint32_t OutRectH() const { return m_oh; }
    uint32_t OutFullW() const { return m_up ? m_outFullW : m_fullW; }
    uint32_t OutFullH() const { return m_up ? m_outFullH : m_fullH; }
    const char* QualityName() const { return m_dlaa.QualityName(); }
    // v0.7.0: the texture the last successful Run produced (sharpened or NGX output; MV debug = m_out).
    // Upscale: output-rect-sized, what Composite() draws. Null before the first success.
    ID3D11Texture2D* ResultTex() const { return m_result; }

    void SetEye(int e) { m_eye = e; m_dlaa.SetEye(e); }
    // GPU timing (timestamp queries around Run, read back a few frames later, logged every 600 frames).
    // v0.5.6: one timestamp per stage boundary (copy-in / depth / mv / ngx / sharpen / copy-back).
    void SetTiming(bool on) { m_timing = on; }
    // v0.5.7 (preset / sharpness changed live): drop this eye's per-stage sums and every in-flight sample
    // (they measured the old state), skip the next kTimingWarmup landed samples (feature recreate hitch),
    // then log the next "DLAA GPU cost" line after 240 timed frames instead of 600 (then 600 again).
    void ResetTiming();
    // v0.5.7: restarts the depth-snapshot GPU timer window (shared by all twins).
    static void ResetSnapTiming();

    // v0.5.6 (dlaa.ini, read once at load, before the first Run): process-wide, both eyes.
    // dlss_preset: 0 = driver default, else 'J','K','L','M','E','F'. Returns false for an unknown letter.
    // v0.5.7: also live (Shift+End); both eyes recreate their feature at their next Run (see dlaa.h).
    static bool SetDlssPreset(char letter) { return DlaaProcessor::SetRenderPreset(letter); }
    static const char* DlssPresetName() { return DlaaProcessor::RenderPresetName(); }
    static uint32_t DlssPresetFallbacks() { return DlaaProcessor::PresetFallbacks(); }
    // sharpness 0..1 (RCAS strength after NGX); 0 = pass skipped. v0.5.7: live (Shift+PageUp/PageDown); the
    // RCAS texture/shader always exist now, each eye re-uploads its constant buffer when the value changed.
    static void SetSharpness(float s) { s_sharpness = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s); }
    static float Sharpness() { return s_sharpness; }
    // v0.5.8 RCAS ring-tap radius in texels, 1..4 (default 1.5). The 4 ring taps are bilinear, so a fractional
    // radius averages two neighbours; live (Ctrl+PageUp/PageDown), each eye re-uploads its CB when it changed.
    static void SetSharpRadius(float r) { s_sharpRadius = r < 1.0f ? 1.0f : (r > 4.0f ? 4.0f : r); }
    static float SharpRadius() { return s_sharpRadius; }
    // v0.6.4 DLAA area: percentage 40..100 of width AND height (pixel count ~ area^2; 100 = whole image = the
    // v0.6.3 path). dlaa.ini dlaa_area + live Shift/Ctrl+Home; a change recreates each eye's crop textures and
    // NGX feature at its next Run (history reset). Feather = border blend width in px (0..512, dlaa.ini only).
    static void SetArea(int a) { s_area = a < 40 ? 40 : (a > 100 ? 100 : a); }
    static int  Area() { return s_area; }
    static void SetFeather(int f) { s_feather = f < 0 ? 0 : (f > 512 ? 512 : f); }
    static int  Feather() { return s_feather; }
    // Crop size for a full image W x H at `area` %: each axis round(full * area / 100) to a multiple of 8
    // (at least 8), clamped to the full size; area >= 100 = full size.
    static void CropSize(uint32_t fullW, uint32_t fullH, int area, uint32_t* cw, uint32_t* ch);
    // v0.6.4: this eye's optical centre in uv (0..1, y down), used to centre the rect. Default (0.5, 0.5) until
    // the caller has one (flat: always 0.5, 0.5). The caller only changes it on a re-adopt (> 0.01 move); a
    // resulting rect-origin change resets this eye's DLSS history at the next Run.
    void SetOpticalCentre(float u, float v) { m_cu = u; m_cv = v; }
    float CentreU() const { return m_cu; }
    float CentreV() const { return m_cv; }
    // v0.6.4 rect of the last Run (crop origin / size in the full image). Whole image: 0, 0, full, full.
    uint32_t RectX() const { return m_rx; }
    uint32_t RectY() const { return m_ry; }
    uint32_t RectW() const { return m_w; }
    uint32_t RectH() const { return m_h; }
    uint32_t FullW() const { return m_fullW; }
    uint32_t FullH() const { return m_fullH; }
    // mvDebug (v0.4.1): after DLAA (or instead of it if it failed) the game's blit source is
    // overwritten with a visualization of the MV texture: (0.5 + mv.x/16, 0.5 + mv.y/16,
    // cabin ? 1 : 0). Gray = no motion.

    // Motion-vector candidate collector (the DrawIndexed hook feeds it).
    CameraMv& Mv() { return m_cam; }

    void Shutdown();

    uint64_t Frames() const { return m_frames; }

    // Snapshot accessors (v0.4.2, F10): the private textures NGX consumed. Valid after Run().
    // v0.6.4: all crop-sized (RectW x RectH). v0.7.0 upscale: OutTex is output-rect-sized (OutRectW x OutRectH).
    ID3D11Texture2D* ColorInTex() const { return m_colorIn.Get(); }   // R8G8B8A8_UNORM, pre-AA, jittered
    ID3D11Texture2D* OutTex()     const { return m_out.Get(); }       // R8G8B8A8_UNORM, NGX output
    ID3D11Texture2D* MvTex()      const { return m_mv.Get(); }        // R16G16_FLOAT
    ID3D11Texture2D* DepthTex()   const { return m_depthR32.Get(); }  // R32_FLOAT
    // v0.7.8 preview capture (Ctrl+F10 on the profile / truck-preview screen): SRVs of the same textures and what the
    // last Run did -- MV pass B ran (else zero MVs), the reset finally passed to NGX (incl. forced ones), RCAS ran.
    ID3D11ShaderResourceView* ColorInSrv() const { return m_colorInSrv.Get(); }
    ID3D11ShaderResourceView* DepthSrv()   const { return m_depthR32Srv.Get(); }
    ID3D11ShaderResourceView* MvSrv()      const { return m_mvSrv.Get(); }
    ID3D11ShaderResourceView* NgxOutSrv()  const { return m_outSrv.Get(); }   // NGX output before RCAS (null: no RCAS setup)
    bool LastMvDone() const { return m_lastMvDone; }
    bool LastReset() const { return m_lastReset; }
    bool LastSharpened() const { return m_lastSharpened; }

private:
    // v0.6.4: (fullW, fullH) = tonemap / depth size, (cw, ch) = crop size (= full at area 100), (rx, ry) = crop
    // origin (only logged / stored on a rebuild; an origin change alone does not rebuild).
    // v0.7.0: (outFullW, outFullH) = full output size (0 = no upscale), (ow, oh) = output rect size (= cw, ch
    // without upscale). m_out / m_sharp and the NGX target are ow x oh.
    bool Ensure(ID3D11Device* dev, uint32_t fullW, uint32_t fullH, uint32_t cw, uint32_t ch, uint32_t rx, uint32_t ry,
                uint32_t outFullW, uint32_t outFullH, uint32_t ow, uint32_t oh);
    bool EnsureComposite(ID3D11Device* dev);               // v0.7.0: VS + PS + states + param buffer
    bool EnsureOwnTwin(ID3D11Device* dev);
    void GpuPoll(ID3D11DeviceContext* ctx);
    void GpuBegin(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    void GpuMark(ID3D11DeviceContext* ctx, int stamp);     // timestamp at a stage boundary (1..kStamps-1)
    void GpuEnd(ID3D11DeviceContext* ctx, bool keep = true);   // keep=false: aborted Run, result discarded
    bool EnsureDebugCs(ID3D11Device* dev);
    bool EnsureSharpen(ID3D11Device* dev);                 // RCAS CS + its constant buffer (v0.5.6)
    void DepthConvert(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* twinSrv);   // twin (at rect origin) -> m_depthR32

    DlaaProcessor m_dlaa;
    CameraMv m_cam;

    Microsoft::WRL::ComPtr<ID3D11ComputeShader>       m_depthCs;
    Microsoft::WRL::ComPtr<ID3D11Buffer>              m_depthCb;      // v0.6.4: b0 = rect origin (uint2 + pad), DYNAMIC
    uint32_t                                          m_depthCbX = 0, m_depthCbY = 0;   // origin last written into m_depthCb
    Microsoft::WRL::ComPtr<ID3D11ComputeShader>       m_mvDbgCs;      // MV debug view
    bool                                              m_mvDbgTried = false;

    Microsoft::WRL::ComPtr<ID3D11Texture2D>           m_colorIn;      // R8G8B8A8_UNORM, SRV
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>  m_colorInSrv;

    Microsoft::WRL::ComPtr<ID3D11Texture2D>           m_depthTwin;    // R32G8X24_TYPELESS, SRV
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>  m_depthTwinSrv; // R32_FLOAT_X8X24_TYPELESS

    Microsoft::WRL::ComPtr<ID3D11Texture2D>           m_depthR32;     // R32_FLOAT, SRV+UAV -> NGX
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>  m_depthR32Srv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_depthR32Uav;

    Microsoft::WRL::ComPtr<ID3D11Texture2D>           m_mv;           // R16G16_FLOAT
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>  m_mvSrv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_mvUav;

    Microsoft::WRL::ComPtr<ID3D11Texture2D>           m_out;          // R8G8B8A8_UNORM, UAV + SRV (RCAS input)
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_outUav;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>  m_outSrv;       // v0.5.6: RCAS input

    // v0.5.6 RCAS sharpen: m_out -> m_sharp, then m_sharp is copied back. v0.5.7: always created in Ensure
    // (also at sharpness 0, so the strength can be raised live; 4592x6496 RGBA8 = ~120 MB per eye).
    Microsoft::WRL::ComPtr<ID3D11Texture2D>           m_sharp;        // R8G8B8A8_UNORM, UAV (v0.7.0 upscale: + SRV)
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_sharpUav;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>  m_sharpSrv;     // v0.7.0: composite source (upscale only)
    Microsoft::WRL::ComPtr<ID3D11ComputeShader>       m_sharpCs;
    // b0: strength, radius, (v0.6.4) feather, blend, edge flags (L, T, R, B) -- 32 bytes, DYNAMIC (v0.5.7)
    Microsoft::WRL::ComPtr<ID3D11Buffer>              m_sharpCb;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>        m_sharpSampler; // v0.5.8: linear clamp, bound at s0 for the RCAS dispatch
    // v0.6.4: the 8 floats last written into m_sharpCb (was strength + radius only); re-uploaded on any change.
    static constexpr int                              kSharpCbFloats = 8;
    float                                             m_sharpCbData[kSharpCbFloats] = {};
    bool                                              m_sharpCbValid = false;
    bool                                              m_sharpRes = false;    // m_sharp* + CS + CB valid for m_w x m_h
    static float                                      s_sharpness;
    static float                                      s_sharpRadius;  // v0.5.8: RCAS ring-tap radius in texels (1..4)
    static int                                        s_area;         // v0.6.4 dlaa_area (40..100)
    static int                                        s_feather;      // v0.6.4 dlaa_area_feather (0..512 px)

    // ---- v0.7.0 upscale composite (VS + PS draw into the game's RT0 after its blit) ----
    Microsoft::WRL::ComPtr<ID3D11VertexShader>        m_compVs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>         m_compPs;
    Microsoft::WRL::ComPtr<ID3D11BlendState>          m_compBlend;    // RT0: src-alpha blend, RGB write; RT1..7: no write
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>     m_compRs;       // solid, no cull, no scissor
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState>   m_compDss;      // depth + stencil off
    Microsoft::WRL::ComPtr<ID3D11Buffer>              m_compParams;   // 3 x float4 (Buffer<float4> at t1; no CB slot touched)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>  m_compParamsSrv;
    bool                                              m_compTried = false;   // EnsureComposite failed once: no retry
    bool                                              m_compPending = false; // Run produced a result for Composite()
    ID3D11ShaderResourceView*                         m_compSrc = nullptr;   // m_sharpSrv or m_outSrv (owned by them)
    ID3D11Texture2D*                                  m_result = nullptr;    // ResultTex() (owned by m_out / m_sharp)
    float                                             m_compData[12] = {};   // last written m_compParams
    bool                                              m_compDataValid = false;

    // Timestamps: 0 start, 1 after copy-in, 2 after depth convert, 3 after MV, 4 after NGX evaluate,
    // 5 after sharpen, 6 after copy-back. v0.5.7: with the merged MV+depth pass (MV on and Generate ran)
    // stamp 2 is taken right after stamp 1 (depth ~0 ms) and "mv" carries the merged pass.
    // v0.7.0 upscale: stamp 6 is taken after the composite draw (in Composite, after the game's blit Draw) and
    // `tc` right before it; stage 6 = "composite" = t[6] - tc, and the total skips the game's blit (tc - t[5]).
    static constexpr int kStamps = 7;
    static constexpr uint64_t kTimingWarmup = 30;     // v0.5.7: samples skipped after ResetTiming
    struct GpuQ {
        Microsoft::WRL::ComPtr<ID3D11Query> dis, t[kStamps];
        Microsoft::WRL::ComPtr<ID3D11Query> tc;       // v0.7.0: just before the composite draw (split slots only)
        bool inFlight = false;
        bool keep = true;                 // false = Run aborted, timestamps not meaningful
        bool split = false;               // v0.7.0: stage 6 measured by Composite (tc .. t[6])
    };
    GpuQ     m_q[4];
    int      m_qHead = 0, m_qCur = -1, m_qNext = 0;   // m_qNext = next stamp index not yet ended
    bool     m_timing = false;
    double   m_msSum = 0.0;
    double   m_stSum[kStamps - 1] = {};               // per-stage sums (stamp i+1 - stamp i)
    uint64_t m_msN = 0;                               // samples in the current window
    uint64_t m_msWindow = 600;                        // v0.5.7: 240 for the first window after a change
    uint64_t m_msSkip = 0;                            // v0.5.7: landed samples still to drop (warm-up)

    int      m_eye = 0;
    uint32_t m_w = 0, m_h = 0;     // v0.6.4: CROP size (= full at area 100): every private texture + the NGX feature
    uint32_t m_fullW = 0, m_fullH = 0;   // v0.6.4: full tonemap / depth size the crop belongs to
    uint32_t m_rx = 0, m_ry = 0;   // v0.6.4: crop origin in the full image (even px)
    bool     m_crop = false;       // v0.6.4: m_w x m_h != m_fullW x m_fullH (sub-rect path + border blend)
    bool     m_up = false;         // v0.7.0: DLSS upscaling (output != render); m_out / m_sharp / NGX target = m_ow x m_oh
    uint32_t m_outFullW = 0, m_outFullH = 0;   // v0.7.0: full output size (0 without upscale)
    uint32_t m_ow = 0, m_oh = 0;   // v0.7.0: output rect size (= m_w x m_h without upscale)
    uint32_t m_oxc = 0, m_oyc = 0; // v0.7.0: output rect origin in the full output (= m_rx, m_ry without upscale)
    bool     m_resetPending = false;   // v0.6.4: rebuilt / rect moved -> next Evaluate drops history
    float    m_cu = 0.5f, m_cv = 0.5f; // v0.6.4: optical centre (uv) the rect is centred on
    bool     m_ready  = false;     // NGX + textures valid for m_w x m_h (of m_fullW x m_fullH)
    bool     m_failed = false;     // init failed for these dims; no retry until dims change
    bool     m_deferred = false;   // v0.7.8: the last Run waited (shader warm-up / creation budget), see Deferred()
    bool     m_lastMvDone = false, m_lastReset = false, m_lastSharpened = false;   // v0.7.8 capture info of the last Run
    uint64_t m_frames = 0;
    uint64_t m_evalFails = 0;
};

#endif // WITH_DLAA
