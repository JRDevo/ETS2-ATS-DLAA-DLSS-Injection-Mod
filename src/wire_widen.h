// WireWiden -- phase 61: far power / catenary wires drawn at least `wire_min_px` wide, faded to keep their look.
//
// Why: ETS2 / ATS draw a wire as a FLAT strip lying face-up (its normal points to the sky; captures\*.wire_audit.txt). Seen from
// the cab it is edge-on: 0.04 - 0.12 px thin at 170 m, ~1 px at 30 m. The rasterizer then draws it only where it covers a pixel
// centre -- dashes -- and the dashes move every frame (camera motion + the mod's jitter), which DLAA cannot hold while driving
// (phase 60 gave the wire its own depth / motion; the colour itself stayed dashed).
//
// What: a geometry shader between the game's wire VS and PS (same signature in and out, every attribute passed through). A
// triangle whose height h over its longest edge is below W = `min px` on screen is replaced by a BAND W wide from the middle of its
// shortest edge (the strip's end) to its far vertex, with its COLOR.w (the wire PS multiplies its alpha by it: alpha = tex.a *
// COLOR.w) scaled so the two triangles of a ribbon quad (two parallel bands h / 2 apart) together cover h / W: k = h / (2 W) for
// additive blending, the exact overlap solution for over blending (INV_SRC_ALPHA; see kWireGs). The wire is one continuous W-wide band with the energy of the thin strip: every column has a
// pixel, nothing to dash, the same darkness on average. Triangles at or above W pass through unchanged (near wires as before).
// Motion vectors: the forward-depth / forward-id replays draw the thin strip without this GS; with mv_fwd_conservative >= 1 (phase
// 60, default 1) their conservative raster covers the band's pixels, with 0 the band pixels between the dashes keep the sky's motion.
//
// Which draws: the game's wire VS (DXBC checksum 4186dac5-a4ee75cf-b37e764c-a4e7601c, the same in ETS2 and ATS) with one of the
// wire pixel shaders whose alpha is tex.a * COLOR.w (phase 61: a5aa1c2d, the long wires; phase 62: + the pole hardware b97c44a3,
// which discards alpha < 0.05 (cedce1aa / 3cc84c95: same cut-off, other colour maths, 1 draw each: not copied, not widened) -- far away their texture alpha is only 0.05 .. 0.2, so ANY fade would drop them: for
// b97c44a3 the mod binds its own instruction-for-instruction copy of that PS (kWireCutPs) whose cut-off runs on the unscaled alpha
// and only then multiplies by k; scripts\rdc_wire_sig_scan.py / rdc_thin_draw_scan.py / rdc_tex_alpha_mips.py),
// noted at CreateVertexShader / CreatePixelShader; RT0 blend SRC_ALPHA * src + (INV_SRC_ALPHA | ONE) * dst (the alpha really scales the result); the world
// layer of the main forward pass (inject.cpp decides that). The same PS also draws other see-through objects: only their
// sub-pixel triangles change, energy-preserving.
// Compiled only with WITH_DLAA (ShaderCache).
#pragma once
#ifdef WITH_DLAA

#include <d3d11.h>
#include <cstddef>
#include <cstdint>

namespace WireWiden {

// The DXBC checksum (bytes 4..19 of the bytecode, as 4 little-endian dwords = RenderDoc's "Shader hash") is `want`.
bool ChecksumIs(const void* code, size_t len, const uint32_t want[4]);
extern const uint32_t kWireVs[4];
extern const uint32_t kWirePs[4];
// phase 62: every pixel shader with the wire signature whose alpha is tex.a * COLOR.w -- PLAIN (no discard) and CUT (discards alpha
// < 0.05: the pole hardware, jumper loops / wire ends / insulators, ATS PS b97c44a3).
constexpr int kWirePsPlainN = 2, kWirePsCutN = 1;
extern const uint32_t kWirePsPlain[kWirePsPlainN][4];
extern const uint32_t kWirePsCut[kWirePsCutN][4];
int PsKind(const void* code, size_t len);            // 0 = not a wire PS, 1 = plain, 2 = cut (discard: the mod's copy draws it)

// Any thread (the device's Create*Shader hooks): remember / forget a created shader object by its bytecode.
void NoteVs(const void* vs, const void* code, size_t len);
void NotePs(const void* ps, const void* code, size_t len);
bool IsWireVs(const void* vs);                       // identity only
int  WirePsKind(const void* ps);                     // 0 / 1 plain / 2 cut (identity only)
int  KnownVs();                                      // wire shader objects currently known (log)
int  KnownPs();                                      // plain + cut
int  KnownPsCut();
// phase 63: SOLID thin geometry in the G-buffer (no alpha to fade): pair A = far high-voltage wires (VS 07dfaa00 + PS b67da69f),
// pair B = lattice towers (VS 6c157c5f + PS 34860eed). Their thin triangles become the same W-wide band, and the mod's copy of the
// game's G-buffer PS keeps each band pixel with probability k (noise that changes every frame): DLAA averages it into a faint
// continuous line (dithered coverage, as TAA renders dithered transparency).
// phase 66: pair B has a second VS, the instanced street lights (a484ef85: the same output signature as 6c157c5f, byte for byte).
extern const uint32_t kSolidVsA[4], kSolidPsA[4], kSolidVsB[4], kSolidPsB[4], kSolidVsB2[4];
int  SolidPsKind(const void* ps);                    // 0 / 1 (A) / 2 (B), identity only
bool IsSolidVs(const void* vs, int kind);
int  KnownSolid();                                   // solid shader objects known (3 VS + 2 PS when all were seen; phase 66)
const char* SolidGsSource(size_t* len);              // entries GSolidA / GSolidB
const char* SolidPsSource(size_t* len);              // entries PSSolidA / PSSolidB (the game's PSs + the dither cut)
// Render thread, before the game's G-buffer draw of pair `kind`; true = bound (call Unbind after the draw). frame = dither frame.
bool BindSolid(ID3D11DeviceContext* ctx, int kind, float vpW, float vpH, float minPx, uint32_t frame, float minShare);
// phase 64: the widening GS alone for the mod's forward-depth / forward-id replays (DrawIdRecord::SetReplayGs): kind 1 = GSCut (a
// see-through wire: COLOR.w untouched, its extra TEXCOORD7 is ignored by the game / id PS), 2 / 3 = GSolidA / GSolidB (keep share
// unused there: the replay PS has no dither -> the full band); 0 = unbind. vpW / vpH = the replay viewport, scale = replay px per game
// px (the band width follows the 1/2-resolution forward depth). No PS change. Render thread, inside the mod's own work.
bool BindReplay(ID3D11DeviceContext* ctx, int kind, float vpW, float vpH, float scale);
void SetReplayMinPx(float minPx);                    // the band width the replays use (= the live wire_min_px)


// The HLSL of the geometry shader (entry GSMain, gs_5_0); the harness compiles it directly.
const char* GsSource(size_t* len);                   // entries GSMain (plain) and GSCut (pole hardware)
const char* CutPsSource(size_t* len);                // phase 62: the copy of the pole-hardware PS b97c44a3 (entry PSCut)
void RegisterShaders();                              // SetupThread, before ShaderCache::Start

// Render thread, the game's immediate context, right before the game's draw: true = the GS (+ its constant buffer at GS b0) is
// bound for this draw -> call Unbind after it. `bs` = the draw's blend state (qualifies or not, memoised), (vpW, vpH) = viewport 0
// in pixels, minPx = the band width. False = nothing bound (shader not ready, blend not qualifying, a GS already bound by the game).
bool Bind(ID3D11DeviceContext* ctx, ID3D11BlendState* bs, float vpW, float vpH, float minPx, int psKind);   // psKind = WirePsKind
void Unbind(ID3D11DeviceContext* ctx);
int  BlendMode(ID3D11BlendState* bs);                // 0 = not widened, 1 = SRC_ALPHA / INV_SRC_ALPHA, 2 = SRC_ALPHA / ONE (memo, ref held)
int  SetFull();                                      // shader notes that found the identity set full (should stay 0)
void Reset();                                        // device change / shutdown: device objects + memo released

struct Stats { uint64_t bound = 0, boundCut = 0, boundSolid = 0, skipBlend = 0, skipGs = 0, skipNotReady = 0, cbUpdates = 0; };
Stats TakeStats();                                   // returns and clears the window counters (render thread)

} // namespace WireWiden

#endif // WITH_DLAA
