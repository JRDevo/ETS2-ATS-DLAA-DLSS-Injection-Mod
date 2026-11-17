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
#include <string>

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
// NoteVs -> phase 114: 'A' / 'B' / 'D' / 'F' when this VS joined that solid VS set by its output signature (0 otherwise)
int  NoteVs(const void* vs, const void* code, size_t len);
void NotePs(const void* ps, const void* code, size_t len);
// phase 114: the DXBC chunk `tag` (payload after its 8-byte header; nullptr + *size 0 when absent / malformed) and the solid VS set
// whose reference output signature (OSGN of 07dfaa00 / 6c157c5f / 3847be86 / e5a4956d) this bytecode carries byte for byte (0 none)
const uint8_t* DxbcChunk(const void* code, size_t len, const char tag[4], uint32_t* size);
int  OsgnSet(const void* code, size_t len);
int  OsgnJoins();                                    // VSs that joined a set by OSGN so far
int  JoinFull();                                     // phase 115r: joins that found their (256-slot) join set full
int  JoinedVs();                                     // phase 115r: VS objects in the join sets now
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
// phase 107b (ETS2): pair C = cranes (pair A's VS 07dfaa00 + PS 57a4e1f1 -> GSolidA), pair D = railings (PS 2d7ed0e6, discard < 0.05;
// VS 3847be86 / 9d2904cd / d87f5280), pair E = the ETS2 street lights (instanced VS d87f5280 + PS d3af2ebb); D and E -> GSolidD.
extern const uint32_t kSolidVsA[4], kSolidPsA[4], kSolidVsB[4], kSolidPsB[4], kSolidVsB2[4];
extern const uint32_t kSolidPsC[4], kSolidVsD[4], kSolidVsD2[4], kSolidVsD3[4], kSolidPsD[4], kSolidPsE[4];
// the solid KIND = which PS copy (PSSolidA..E); several kinds share one GS (SolidGsOf). A replay record carries the GS only
// (SolidReplayKind = 2 + the GS index: 2 GSolidA, 3 GSolidB, 4 GSolidD -- phase 107b: never 1 + the PS kind, which overflowed).
// phase 109e: F = fences / town street lights (VS e5a4956d + PS 2cc0a6fa -> GSolidF), G = PS cfc52d43 (pair A's VSs, discard kept) ->
// GSolidA; pair A's VS set + ccd6b6d1. phase 109d: H = PS e19e0136 (pair A's VSs; road-pole stripes / road signs) -> GSolidA, used
// ONLY by the standalone pole_aa (inject.cpp SolidOnDraw skips it).
// phase 111: pair A's VS set + 2a9f326e (instanced road-pole / delineator band) and 7fcd07d6 (skinned); I = PS 2585159b (pair A's
// VSs, pair A without the gloss: sign / road-furniture atlas) -> GSolidA, wire_widen only.
enum SolidKind { kSolidA = 1, kSolidB = 2, kSolidC = 3, kSolidD = 4, kSolidE = 5, kSolidF = 6, kSolidG = 7, kSolidH = 8, kSolidI = 9 };
constexpr int kSolidKinds = 9, kSolidGsN = 4;
extern const uint32_t kSolidVsA2[4], kSolidVsF[4], kSolidPsF[4], kSolidPsG[4], kSolidPsH[4];
extern const uint32_t kSolidVsA3[4], kSolidVsA4[4], kSolidPsI[4];   // phase 111
extern const uint32_t kSolidVsA5[4];                                 // phase 113: VS bdce664b (Volvo dealership signage, kind I)
int  SolidPsKindOfCode(const void* code, size_t len);   // the solid kind of a PS bytecode by checksum (census / tests), 0 = none
uint32_t PsGeneration();                             // phase 109d: changes with every NotePs (a per-PS kind memo is stale then)
int  SolidPsKind(const void* ps);                    // 0 / kSolidA .. kSolidE, identity only
// the VS set of that kind (A + C share one). Phase 115r (B4): *joined = true when the VS is only an OSGN-JOINED member (not hand-
// listed): the callers dither such a draw only when it is not instanced AND has a fresh phase-80 static verdict
bool IsSolidVs(const void* vs, int kind, bool* joined = nullptr);
int  SolidGsOf(int kind);                            // 0 GSolidA, 1 GSolidB, 2 GSolidD, 3 GSolidF (109e), -1 not a kind
int  SolidReplayKind(int kind);                      // BindReplay's kind for a draw of this solid kind (0 = none)
int  KnownSolid();
// phase 109b: the power lines the GAME widens (forward VS 277d2302 + PS 69dcaf36): identity sets, no shaders of ours. inject.cpp
// stamps their forward record DrawIdRecord::kGsNoA2c (the depth batch without alpha-to-coverage).
extern const uint32_t kGameWireVs[4], kGameWirePs[4];
bool IsGameWirePs(const void* ps);
bool IsGameWireVs(const void* vs);                                   // solid shader objects known (6 VS + 5 PS when all were seen; phase 107b)
const char* SolidGsSource(size_t* len);              // entries GSolidA / GSolidB / GSolidD / GSolidF
const char* SolidPsSource(size_t* len);              // entries PSSolidA .. PSSolidI (the game's PSs + the dither cut)
// Render thread, before the game's G-buffer draw of pair `kind`; true = bound (call Unbind after the draw). frame = dither frame.
// pole (phase 109d) = the stratified per-face keep window (box k = h / W, no floor; minShare unused) for pole_aa.
// phase 113: jitX / jitY = the pass's viewport jitter in screen px (pole mode: its keep window is measured from the un-jittered pixel)
bool BindSolid(ID3D11DeviceContext* ctx, int kind, float vpW, float vpH, float minPx, uint32_t frame, float minShare, bool pole = false,
               float jitX = 0.0f, float jitY = 0.0f);
// phase 64: the widening GS alone for the mod's forward-depth / forward-id replays (DrawIdRecord::SetReplayGs): kind 1 = GSCut (a
// see-through wire: COLOR.w untouched, its extra TEXCOORD7 is ignored by the game / id PS), 2 / 3 / 4 = GSolidA / GSolidB / GSolidD (keep share
// unused there: the replay PS has no dither -> the full band); 0 = unbind. vpW / vpH = the replay viewport, scale = replay px per game
// px (the band width follows the 1/2-resolution forward depth). No PS change. Render thread, inside the mod's own work.
bool BindReplay(ID3D11DeviceContext* ctx, int kind, float vpW, float vpH, float scale);
void SetReplayMinPx(float minPx);                    // the band width the replays use (= the live wire_min_px)
// phase 109r: kind | kReplayPole (= DrawIdRecord::kGsPole) = a pole_aa draw: the solid GS in pole mode, band = this width (pole_min_px)
constexpr int kReplayPole = 0x80;
void SetReplayPoleMinPx(float minPx);


// The HLSL of the geometry shader (entry GSMain, gs_5_0); the harness compiles it directly.
const char* GsSource(size_t* len);                   // entries GSMain (plain) and GSCut (pole hardware)
const char* CutPsSource(size_t* len);                // phase 62: the copy of the pole-hardware PS b97c44a3 (entry PSCut)
void RegisterShaders();                              // SetupThread, before ShaderCache::Start

// Render thread, the game's immediate context, right before the game's draw: true = the GS (+ its constant buffer at GS b0) is
// bound for this draw -> call Unbind after it. `bs` = the draw's blend state (qualifies or not, memoised), (vpW, vpH) = viewport 0
// in pixels, minPx = the band width. False = nothing bound (shader not ready, blend not qualifying, a GS already bound by the game).
bool Bind(ID3D11DeviceContext* ctx, ID3D11BlendState* bs, float vpW, float vpH, float minPx, int psKind);   // psKind = WirePsKind
void Unbind(ID3D11DeviceContext* ctx);
bool PsSwapped();                                    // phase 109a: a Bind / BindSolid put a PS copy in (until Unbind)
bool SolidDead();                                    // phase 109r: the solid shaders failed (BindSolid is false until Reset)
int  BlendMode(ID3D11BlendState* bs);                // 0 = not widened, 1 = SRC_ALPHA / INV_SRC_ALPHA, 2 = SRC_ALPHA / ONE (memo, ref held)
int  SetFull();                                      // shader notes that found the identity set full (should stay 0)
std::string SetFullNames();                          // phase 115r: the sets that overflowed ("solid VS A, wire PS"; "" = none)
void Reset();                                        // device change / shutdown: device objects + memo released

struct Stats { uint64_t bound = 0, boundCut = 0, boundSolid = 0, skipBlend = 0, skipGs = 0, skipNotReady = 0, cbUpdates = 0; };
Stats TakeStats();                                   // returns and clears the window counters (render thread)

} // namespace WireWiden

#endif // WITH_DLAA
