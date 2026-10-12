// Vrs -- phase 34: FIXED FOVEATED RENDERING with NVIDIA Variable Rate Shading (D3D11 through NVAPI), VR only.
//
// Why: at 100 % in-game Scaling + DLAA the VR render thread waits ~3.5 ms per frame in xrEndFrame (RTX 5090, 6120 x 6496 per
// eye) = GPU-limited. The outer part of each eye picture is seen through the lens edge, blurred anyway; shading it once per
// 2 x 2 pixels there (the rasteriser still writes every pixel: depth, stencil, coverage stay per pixel) cuts the pixel-shader
// cost of the scene where nobody looks sharp.
//
// What (ideas ported from OpenXR Toolkit's VRS, MIT -- see vrs.cpp; NOT its framework):
//  - a shading-rate MASK per (render-target size, eye centre, radii): one byte per 16 x 16 px tile (NVAPI tile size), the
//    INDEX into a per-viewport shading-rate table -- 0 = inner (1 x 1), 1 = middle ring, 2 = outer ring; rings are circles in
//    NDC around the eye's optical (projection) centre, radii in % of the NDC half-width as the Toolkit's (inner 55, outer 80).
//    Built ONCE on the CPU (a few hundred KB of bytes), uploaded as an R8_UINT texture with initial data, never per frame;
//  - the TABLE decides the rates: inner 1 x 1, middle 2 x 1 (quality 0) or 1 x 1 (quality 1, default), outer 2 x 2 (never
//    4 x 4 by default: DLAA / DLSS sees each coarse block as 4 equal samples; while DLSS upscales from below 75 % of the output
//    width the outer ring is 2 x 1 instead -- a 2 x 2 block there would be 3 x 3 output pixels or more). vrs_debug = 1 shows
//    the pattern on purpose: middle 2 x 2, outer 4 x 4;
//  - Enter (the game binds an eye's world G-buffer or its main forward pass) binds the eye's mask + the table; Leave (the
//    game's next OMSetRenderTargets) unbinds both. Inside, OnDraw switches the table off for a draw that must not be coarse
//    (alpha-tested cut-outs, SV_Depth writers, unknown shaders) and on again for the next normal draw -- only on a change;
//    OnModWork switches it off while the mod's own work runs on that thread (t_inDlaa in inject.cpp) and back on after.
//
// Only on an NVIDIA GPU whose driver reports bVariablePixelRateShadingSupported (Turing / RTX and newer); anywhere else
// (or built without external/nvapi: WITH_VRS undefined) Probe logs once and nothing else ever happens.
//
// Threading: Enter / Leave / OnDraw / OnModWork run on the game's render thread (the thread that issues the scene draws);
// Configure and the reads for the menu may come from any thread (atomics).
#pragma once
#include <d3d11.h>
#include <atomic>
#include <cstdint>

namespace Vrs {

// Inside a bracket (Enter succeeded, Leave not yet called). The hooks test it inline before calling OnDraw / OnModWork.
// Render thread writes; other threads only read it as a hint (OnModWork re-checks the thread id).
extern bool g_want;

// dlaa.ini vrs / vrs_inner / vrs_outer / vrs_quality / vrs_debug (any thread; Enter reads them). Clamped here.
void Configure(int on, int innerPct, int outerPct, int quality, int debug);
bool On();                     // vrs = 1

// Support state: -1 = not probed yet, 0 = not available (reason in SupportText), 1 = available.
int         Support();
const char* SupportText();
// Probes NVAPI + the device's capabilities once per device (logs once). Render thread. Returns Support().
int Probe(ID3D11Device* dev);

// The mask fill (pure function; the offline harness calls it too). dst = tw * th bytes, row-major. (cu, cv) = the eye's
// optical centre in uv of the render target (v down). Radii in NDC units (0.55 = 55 %). Value per tile: 0 inner, 1 middle,
// 2 outer (decided at the tile centre). Returns the tile counts per value in counts[3] (nullable).
void FillMask(uint8_t* dst, uint32_t tw, uint32_t th, uint32_t rtW, uint32_t rtH, float cu, float cv, float rInner,
              float rOuter, uint32_t counts[3]);

// Bracket. Enter: `eye` only for the log / stats; `renderOverOutput` = render width / output width while DLSS upscales
// (1 = no upscale). false = not entered (off, unsupported, mask failed): no state was touched.
bool Enter(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, int eye, float cu, float cv, float renderOverOutput);
void Leave(ID3D11DeviceContext* ctx);
// A game draw inside the bracket: `exclude` = this draw keeps full-rate shading. Render thread, only while g_want.
void OnDraw(ID3D11DeviceContext* ctx, bool exclude, int why);
// The mod's own work starts (true) / ends (false) on the calling thread. Acts only on the bracket's thread while g_want.
void OnModWork(bool begin);
// The game context was abandoned (context change): forget the bracket without touching the old context.
void Abandon();

// Statistics (render thread writes; the log reads).
enum SkipWhy { kSkipCutout = 0, kSkipDepth = 1, kSkipUnknown = 2, kSkipCount = 3 };
struct Stats {
    uint64_t enters = 0, entersEye[2] = {}, leaves = 0;
    uint64_t drawsVrs = 0, drawsSkip[kSkipCount] = {};
    uint64_t modPauses = 0, switches = 0, nvFails = 0;
    uint64_t masksBuilt = 0;
    uint64_t eyeChecks = 0, eyeMisses = 0;
    uint64_t bindNoEye = 0, bindNoCentre = 0;    // scene binds left alone: no certain eye / no optical centre measured yet
    // phase 34b (inject.cpp VrsDecideEye): entered binds per eye source (0 own blit, 1 blit-RT cycle, 2 forward colour,
    // 3 frame pair; a bind may count in several) and binds left alone because two sources disagreed
    uint64_t eyeSrc[4] = {}, bindDisagree = 0;
};
Stats& GetStats();
void NoteEyeCheck(bool ok);    // the blit's eye vs the eye predicted at Enter (inject.cpp)
// Every >= 10 s (called from Enter): one "VRS:" stats line when the log is on.
void MaybeLogStats(uint64_t frame);

} // namespace Vrs
