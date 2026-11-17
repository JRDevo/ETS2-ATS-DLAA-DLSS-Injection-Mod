// Census -- phase 108 "shader census": while the player drives, the mod lists the shaders / draws it does not handle yet that are
// likely to flicker, tints them live (Ctrl+Shift+F5) and keeps a file a developer reads after the drive (dlaa_census.txt) plus a
// dump folder per new suspect (dlaa_census\<vs>_<ps>\). This header = the PURE parts (no D3D, any thread): the DXBC classifier, the
// per-pair verdict, the texts and the DDS header. inject.cpp (src/census_run.h) does the hooks, the tint, the queries and the files.
// Unit test: tests/census_test.cpp (synthetic fxc shaders in CI + the real game bytecode under captures\ when present).
//
// SUSPECT KINDS (a bit each; a draw pair VS + PS can have several):
//   kPixel  "pixel-reader": the PS computes a pixel index from an INTERPOLANT -- a perspective divide of an input register by a
//           component of the same register (vN.yz / vN.x), times a constant-buffer value (the scene size), ftoi / ftou -- and `ld`s
//           a texture at it, NOT from SV_Position. With the phase-23 (W+1) x (H+1) jitter overscan that index is the NEIGHBOUR pixel
//           over most of the picture (x / W px off): the read lands one texel off on alternating jitter phases (RenderDoc audits
//           2026-11-14: the old SCS water forward PS 9a5fca98, the forward PSs 9c062880, b0852ebe, 1a104d6e, a5aa1c2d, 0a3c1386,
//           079791a9, 19f12dc2, 31586e4b, fd5e16e5). Covered (KNOWN) when phase 97's water_ps classified it (the parity
//           checker shaders are drawn without the overscan) or phase 109a made its SV_Position twin (pixel_reader.h).
//   kCutout "cut-out": the PS discards on a value fetched from a texture (alpha test). Covered (phase 110b) unless a texture the
//           pair DREW in the last kCutWindow Presents is unhandled and may lose coverage (CutClassOf / CutTrack below).
//   kThin   "thin": a G-buffer / forward draw whose coverage really FLICKERS under the jitter. Phase 113b: two steps -- the cheap
//           PRE-FILTER (its sampled occlusion footprint is tiny for its triangle count: < kThinPxPerTri pixels per triangle over >=
//           kThinMinSamples samples; a dense solid mesh passes it too), then the JITTER PROBE (src/census_probe.h: the draw re-drawn
//           depth-only at the 8 jitter offsets into the mod's stencil; the share of its pixels covered at ALL 8 = the stable fraction;
//           thin = under 0.35 on enough probes). Only a pair the probe CONFIRMS is thin (Judge's `thin`); a pair still being probed
//           is no suspect. Covered when the pair is a phase-61..66 / 107b wire / solid pair (wire_widen's VS + PS sets).
//   kWater  "water-like": a projective (reflection / refraction) texture lookup -- a `sample` whose address is a perspective-divided
//           interpolant -- together with an `ld` at the interpolated pixel (reads the G-buffer under it) or >= 2 scrolling samples
//           (interpolant + constant). Covered when phase 97 classified the PS or phase 109a made its twin
//           (the pixel read is what moves under the overscan).
//   kVs     "unknown VS": a PS the mod knows (a wire / solid PS of wire_widen) drawn with a VS outside its known VS set (found:
//           the lamp VS ccd6b6d1 with the solid PS b67da69f). Phase 113b: only while the jitter probe confirms the pair is thin (a
//           known PS drawing a big steady surface with a new VS is nothing to fix).
// phase 110b: KNOWN needs the fix to be ON (wire_widen, pole_aa, power_line_fix, pixel_reader_fix, water_overscan_fix: Cover's
// switches), and a cut-out pair is judged over the textures it really draws (CutTrack; CutClassOf: soft alpha / coverage over 60 % /
// no loss = the game's mips keep coverage = OK; queue full / pending = WAIT, never tinted; only an unhandled or sparse alpha = BAD).
// KNOWN = covered by an existing feature, per kind (a5aa1c2d is a known WIRE PS -- its thinness is covered -- but it is still a
// pixel-reader: that kind is not covered by anything). What is NOT counted as a fix: draw ids / plates (motion only, no flicker
// fix), the light redraw / full-screen light jitter (phases 55 / 78: those are lighting-bind Draw / DrawIndexed calls the census
// does not look at), the glow bias mask (a DLSS input mask).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include "water_ps.h"

namespace Census {

enum Kind : uint32_t { kPixel = 1, kCutout = 2, kThin = 4, kWater = 8, kVs = 16, kKindsAll = 31 };
constexpr int kKindN = 5;
inline const char* KindTag(int i) {
    static const char* const k[kKindN] = { "pixel-reader", "cut-out", "thin", "water-like", "unknown-VS" };
    return (i >= 0 && i < kKindN) ? k[i] : "?";
}
// the colour of a kind AS SHOWN ON SCREEN (sRGB bytes / 255): magenta, orange, cyan, blue, yellow. Phase 115: these are display
// colours now -- the census paint (src/census_paint.h) writes them after DLAA and after the game's tonemap, so the bytes on screen ARE
// these (the legend's swatches too). Was linear (1, 0.45, 0) for orange, tinted into the albedo / HDR colour before the lighting and
// the tonemap: the user's pictures showed it as (255, 179, 0), a gold that read as yellow. Orange is now (255, 112, 0): hue 26 deg
// (gold 42, yellow 60); blue (32, 64, 255) stays apart from cyan.
inline void KindColour(int i, float c[3]) {
    static const uint8_t k[kKindN][3] = { {255, 0, 255}, {255, 112, 0}, {0, 255, 255}, {32, 64, 255}, {255, 255, 0} };
    const int j = (i >= 0 && i < kKindN) ? i : 0;
    c[0] = k[j][0] / 255.0f; c[1] = k[j][1] / 255.0f; c[2] = k[j][2] / 255.0f;
}
// sRGB (0..1) -> linear (0..1), the exact sRGB curve (the paint writes linear values into an _SRGB view; the mirrors' in-place tint)
inline float SrgbToLinear(float v) {
    if (v <= 0.04045f) return v / 12.92f;
    return (float)std::pow((v + 0.055) / 1.055, 2.4);
}
// hue in degrees (0..360) of an sRGB colour (tests: orange must read ORANGE, not gold / yellow)
inline double HueDeg(double r, double g, double b) {
    const double mx = (std::max)(r, (std::max)(g, b)), mn = (std::min)(r, (std::min)(g, b)), d = mx - mn;
    if (d <= 0.0) return 0.0;
    double h = mx == r ? std::fmod((g - b) / d, 6.0) : (mx == g ? (b - r) / d + 2.0 : (r - g) / d + 4.0);
    h *= 60.0;
    return h < 0.0 ? h + 360.0 : h;
}

// ---- phase 115: the census PAINT -- a flashing checkerboard, drawn after DLAA (src/census_paint.h) -----------------------------
// The user (VR + flat drives): "I hardly noticed the yellows ... It must be IN YOUR FACE so people capture it." The tint draws no
// colour into the game's targets any more (main world passes): each suspect draw is re-drawn into the census MASK (R32_UINT, the
// pass's depth size, its own render target, the game's depth test) with a code word; at the eye / screen blit the mask is PAINTED
// into the blit source -- after DLAA, after the tonemap -- as a checkerboard of the kind's colour and black, its phase swapped
// kPaintSwapsPerSec times a second. DLAA never sees it (it ran before), so nothing smooths it; the colours are exact screen bytes.
// Mask word: bits 0..2 = kind + 1 (0 = nothing), bit 3 = no depth check, bits 4..31 = the draw's depth (SV_Position.z as float bits,
// low 4 mantissa bits dropped). The paint shows a word with a depth only where the pass's final depth still has that value (a later
// draw in front hid it: no X-ray); a word without one (the VS gives no SV_Position the mask PS can link to, or the draw wrote no
// depth: the game's own test at its draw was the occlusion test) always.
// Phase 115r (audit B8): a draw that wrote NO depth (glass, glow, decals: forward draws) had no depth in its word -> it showed through
// every nearer surface drawn later (cab, mirrors). Its word now carries its depth too, with a TEST mode: shown unless the pass's final
// depth is NEARER (by the game's own depth function: LESS* = nearer is smaller, GREATER* = nearer is larger, reversed Z). z is in
// [0, 1], so float bits 30 / 31 are always 0: bit 31 = test mode, bit 30 = "nearer is larger"; bits 4..29 = the depth as before.
enum MaskMode : int { kMaskModeNone = 0, kMaskModeEqual = 1, kMaskModeLess = 2, kMaskModeGreater = 3 };
constexpr uint32_t kMaskNoDepth = 8u;
constexpr uint32_t kMaskDepthBits = 0x3FFFFFF0u;
constexpr uint32_t kMaskTest = 0x80000000u, kMaskGreater = 0x40000000u;
inline uint32_t MaskFloatBits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
// the mode of a draw: depth written -> EQUAL (its own depth must still be there); no depth write with a LESS* / GREATER* test -> the
// test; anything else (no depth test, ALWAYS / EQUAL / NEVER ...) -> no depth in the word (D3D11_COMPARISON_FUNC values: LESS 2,
// LESS_EQUAL 4, GREATER 5, GREATER_EQUAL 7)
inline int MaskModeOf(bool depthEnable, bool depthWrite, int func) {
    if (!depthEnable) return kMaskModeNone;
    if (depthWrite) return kMaskModeEqual;
    if (func == 2 || func == 4) return kMaskModeLess;
    if (func == 5 || func == 7) return kMaskModeGreater;
    return kMaskModeNone;
}
inline uint32_t MaskWord(int kind, int mode, float z) {   // (mode: MaskMode; a bool true = kMaskModeEqual, false = none)
    const uint32_t code = (uint32_t)(kind + 1) & 7u;
    if (mode <= kMaskModeNone || mode > kMaskModeGreater) return code | kMaskNoDepth;
    const uint32_t zb = (MaskFloatBits(z) & kMaskDepthBits) | code;
    return mode == kMaskModeEqual ? zb : (zb | kMaskTest | (mode == kMaskModeGreater ? kMaskGreater : 0u));
}
inline int MaskKind(uint32_t w) { return (int)(w & 7u) - 1; }           // -1 = nothing
// the paint's rule for one pixel (the HLSL PSPaint does exactly this): shown = a kind and (no depth check, or EQUAL: the depth still
// matches, or TEST: the final depth is not nearer). tol = 2 steps of 16 units of the float bits (the snapshot is the same float;
// rounding slack)
inline bool MaskShows(uint32_t w, bool haveDepth, float finalZ) {
    if (!(w & 7u)) return false;
    if ((w & kMaskNoDepth) || !haveDepth) return true;
    const int64_t a = (int64_t)(w & kMaskDepthBits), b = (int64_t)(MaskFloatBits(finalZ) & kMaskDepthBits);
    if (!(w & kMaskTest)) return (a > b ? a - b : b - a) <= 32;
    return (w & kMaskGreater) ? b <= a + 32 : b + 32 >= a;
}
constexpr double kPaintSwapsPerSec = 4.0;      // the checker swaps (colour <-> black) this many times a second: 2 Hz full on / off
inline int PaintPhase(double sec) { return sec < 0.0 ? 0 : (int)((int64_t)(sec * kPaintSwapsPerSec) & 1); }
// the square's side in blit-source (render) px: ~0.42 % of the picture width, 8..12 px. VR eye 2900 px wide over ~104 deg = 12 px =
// 0.43 deg = ~8 Quest 3 display px; flat 1920 = 8 px (0.4 % of the width), 4K = 12 px. Big enough to read in the headset, small
// enough that a 2 px pole still shows several.
inline int PaintCell(uint32_t width) {
    const int c = (int)std::lround((double)width / 240.0);
    return c < 8 ? 8 : (c > 12 ? 12 : c);
}
// the colour (sRGB 0..1) the paint writes at render pixel (x, y): kind colour or black by the checker, anchored at (ox, oy) (VR: the
// eye's optical centre, so both eyes' squares meet at infinity; flat 0, 0)
inline void PaintColour(int kind, int x, int y, float ox, float oy, int cell, int phase, float out[3]) {
    const int cx = (int)std::floor(((double)x + 0.5 - ox) / cell), cy = (int)std::floor(((double)y + 0.5 - oy) / cell);
    const bool on = ((cx + cy + phase) & 1) == 0;
    float c[3];
    KindColour(kind, c);
    for (int i = 0; i < 3; ++i) out[i] = on ? c[i] : 0.0f;
}

// ---- phase 115: the vertex shader's output layout, for the mask PS that reads SV_Position (the draw's depth) -------------------
// A pixel shader's inputs must match the VS outputs by register and semantic. Measured over every game VS under captures\ (135
// files, 50 distinct): 42 write COLOR0 (float4, all components) to o0 and SV_Position to o1, 8 SV_Position to o0. One mask PS per
// layout (PSMask1 / PSMask2); any other layout = PSMask0 (no inputs: the word has no depth -> painted where the game's test passed).
enum VsLayout : int { kVsLayoutOther = 0, kVsLayoutPos0 = 1, kVsLayoutColPos = 2 };
inline int VsPosLayout(const void* code, size_t len) {
    if (!code || len < 32) return kVsLayoutOther;
    const uint8_t* const b = static_cast<const uint8_t*>(code);
    if (b[0] != 'D' || b[1] != 'X' || b[2] != 'B' || b[3] != 'C') return kVsLayoutOther;
    const uint32_t n = WaterPs::Rd(b, 28);
    if (n > 64 || 32 + (size_t)n * 4 > len) return kVsLayoutOther;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t off = WaterPs::Rd(b, 32 + 4 * (size_t)i);
        if ((size_t)off + 8 > len) return kVsLayoutOther;
        if (memcmp(b + off, "OSGN", 4) != 0) continue;     // (OSG5 = a GS stream signature; OSG1 = min precision: not handled)
        const uint32_t sz = WaterPs::Rd(b, off + 4);
        const uint8_t* const p = b + off + 8;
        if ((size_t)off + 8 + sz > len || sz < 8) return kVsLayoutOther;
        const uint32_t cnt = WaterPs::Rd(p, 0);
        if (cnt == 0 || cnt > 64 || 8 + (size_t)cnt * 24 > sz) return kVsLayoutOther;
        auto el = [&](uint32_t k, const char** name, uint32_t* idx, uint32_t* sv, uint32_t* ct, uint32_t* reg, uint32_t* mask) {
            const uint8_t* e = p + 8 + (size_t)k * 24;
            const uint32_t no = WaterPs::Rd(e, 0);
            *idx = WaterPs::Rd(e, 4); *sv = WaterPs::Rd(e, 8); *ct = WaterPs::Rd(e, 12); *reg = WaterPs::Rd(e, 16); *mask = e[20];
            *name = (no < sz && memchr(p + no, 0, sz - no)) ? (const char*)(p + no) : "";
        };
        const char* nm; uint32_t idx, sv, ct, reg, mask;
        int posReg = -1;
        for (uint32_t k = 0; k < cnt; ++k) {
            el(k, &nm, &idx, &sv, &ct, &reg, &mask);
            if (sv == 1) { posReg = (int)reg; if ((mask & 15u) != 15u) return kVsLayoutOther; }
        }
        if (posReg == 0) return kVsLayoutPos0;
        if (posReg != 1) return kVsLayoutOther;
        // o0 must hold exactly one element: COLOR0, float, all four components (the PS declares float4 COLOR0 : v0)
        int inReg0 = 0;
        bool col0 = false;
        for (uint32_t k = 0; k < cnt; ++k) {
            el(k, &nm, &idx, &sv, &ct, &reg, &mask);
            if (reg != 0) continue;
            ++inReg0;
            col0 = !_stricmp(nm, "COLOR") && idx == 0 && sv == 0 && ct == 3 && (mask & 15u) == 15u;
        }
        return (inReg0 == 1 && col0) ? kVsLayoutColPos : kVsLayoutOther;
    }
    return kVsLayoutOther;
}

// ---- phase 115: captured THIS SESSION = not highlighted until the next game start -------------------------------------------------
// The user: "I captured flags, drove on, and the next same flags were still coloured as uncaptured." A pair dumped or linked by a
// double-Insert capture is hidden from the live view once that capture has finished (memory only: the next start shows it again,
// unless the mod then has a fix for it). capSession = the capture number that saved it (0 = never); while that capture still runs
// it stays highlighted (tinted.png and its own picture.png need it).
inline bool CapturedHidden(int capSession, bool capActive, int capNo) {
    return capSession > 0 && !(capActive && capSession == capNo);
}
// the kind a draw is tinted with when it has several: the first bit in the order above
inline int TintKind(uint32_t kinds) {
    for (int i = 0; i < kKindN; ++i) if (kinds & (1u << i)) return i;
    return -1;
}
inline std::string KindList(uint32_t kinds) {
    std::string s;
    for (int i = 0; i < kKindN; ++i)
        if (kinds & (1u << i)) { if (!s.empty()) s += ", "; s += KindTag(i); }
    return s.empty() ? "-" : s;
}

// the thin PRE-FILTER (per pair, from sampled occlusion queries; phase 113b: the jitter probe decides, src/census_probe.h)
constexpr double   kThinPxPerTri   = 0.5;    // pixels per triangle below this = a thin CANDIDATE (dense meshes pass too)
constexpr uint32_t kThinMinTris    = 64;     // a sample needs at least this many triangles (IndexCount / 3 x instances)
constexpr int      kThinMinSamples = 3;      // samples with pixels > 0 before a verdict
inline bool ThinVerdict(uint64_t pixSum, uint64_t triSum, int samples) {
    return samples >= kThinMinSamples && triSum > 0 && (double)pixSum < kThinPxPerTri * (double)triSum;
}
// phase 111: an occlusion query counts only the samples that SURVIVE a cut-out's discard, so a cut-out draw's pixels per triangle
// read low by its alpha coverage (the 2026-11-15 roof-ladder lattice 0dfced74_180589ca: 0.36 px / tri measured at ~28 % coverage =
// ~1.3 px / tri rasterised). A sampled draw of a cut-out pair whose alpha texture's coverage at the 0.05 cut is known (phase 77's
// mip-0 analysis, `cov` 0..1) counts px / max(cov, kThinCovFloor) towards the thin rule; unknown coverage (< 0, cutout_mips = 0, not
// BC3) or no cut-out: px as measured. Rounded; never below the measured pixels.
constexpr double kThinCovFloor = 0.05;
inline uint64_t ThinPixels(uint64_t px, double cov) {
    if (!(cov > 0.0) || cov >= 1.0) return px;
    const double c = cov < kThinCovFloor ? kThinCovFloor : cov;
    const double v = (double)px / c + 0.5;
    return v >= 1.8e19 ? ~0ull : (uint64_t)v;
}

// ---- the DXBC scan -----------------------------------------------------------------------------------------------------------
struct PsScan {
    uint32_t sum[4] = {};        // the DXBC checksum (RenderDoc's "Shader hash")
    bool     parsed = false;     // a pixel shader whose SHDR / SHEX parsed to the end
    uint32_t kinds = 0;          // structural kinds: kPixel / kCutout / kWater
    uint32_t texDecl = 0;        // t-slots 0..31 declared
    uint32_t discardTex = 0;     // t-slots whose data reaches a discard condition (the alpha source of a cut-out)
    uint32_t pixelTex = 0;       // t-slots read with `ld` at the interpolated pixel
    int      projSamples = 0;    // samples at a perspective-divided interpolant
    int      scrollSamples = 0;  // samples at interpolant + constant (scrolling normals)
    int      pixelLds = 0;       // `ld` at the interpolated pixel
    int      discards = 0;       // discard instructions
    bool     parity = false;     // the pixel index's parity is taken (the phase-97 checker shape)
};

inline bool Checksum(const void* code, size_t len, uint32_t out[4]) {
    memset(out, 0, 16);
    if (!code || len < 20) return false;
    const uint8_t* b = static_cast<const uint8_t*>(code);
    if (b[0] != 'D' || b[1] != 'X' || b[2] != 'B' || b[3] != 'C') return false;
    for (int i = 0; i < 4; ++i) out[i] = WaterPs::Rd(b, 4 + 4 * (size_t)i);
    return true;
}

namespace scan {
constexpr uint8_t kP = 1;     // perspective-divided interpolant
constexpr uint8_t kS = 2;     // ... times a constant (a screen position in pixels)
constexpr uint8_t kI = 4;     // ... as an integer (ftou / ftoi)
constexpr uint8_t kPar = 8;   // ... its parity
constexpr uint8_t kC = 16;    // constant-buffer data
constexpr uint8_t kT = 32;    // data fetched from a texture
constexpr uint8_t kV = 64;    // an interpolant (input register, not SV_Position)
constexpr uint8_t kQ = 128;   // a projective divide of any value by a component of the same register (reflection lookups)
constexpr uint32_t kOpTemp = 0, kOpInput = 1, kOpImm32 = 4, kOpResource = 7, kOpCb = 8;
inline bool IsSample(uint32_t opc) { return opc == 69u || opc == 72u || opc == 73u || opc == 74u; }   // not sample_c / _c_lz
}  // namespace scan

// Pure function (any thread). Reuses phase 97's operand parser (WaterPs::detail).
inline PsScan ScanPs(const void* code, size_t len) {
    using namespace WaterPs::detail;
    using namespace scan;
    PsScan r;
    if (!code || len < 32) return r;
    const uint8_t* const b = static_cast<const uint8_t*>(code);
    if (!Checksum(code, len, r.sum)) return r;
    const uint32_t nChunks = WaterPs::Rd(b, 28);
    if (nChunks > 64 || 32 + (size_t)nChunks * 4 > len) return r;
    size_t base = 0;
    uint32_t n = 0;
    for (uint32_t c = 0; c < nChunks && !n; ++c) {
        const size_t off = WaterPs::Rd(b, 32 + (size_t)c * 4);
        if (off + 8 > len) return r;
        const uint32_t fourcc = WaterPs::Rd(b, off), size = WaterPs::Rd(b, off + 4);
        if (fourcc != 0x52444853u && fourcc != 0x58454853u) continue;   // 'SHDR' / 'SHEX'
        if (size < 8 || off + 8 + size > len) return r;
        base = off + 8;
        if ((WaterPs::Rd(b, base) >> 16) != 0) return r;                // program type 0 = pixel shader
        const uint32_t nTok = size / 4, declared = WaterPs::Rd(b, base + 4);
        n = declared < nTok ? declared : nTok;
    }
    if (!n) return r;
    constexpr int kTemps = 256;
    static_assert(kMaxTemps == kTemps, "temp table");
    uint8_t taint[kTemps][4];
    uint32_t tmask[kTemps][4];                                           // t-slots feeding each temp component
    memset(taint, 0, sizeof(taint));
    memset(tmask, 0, sizeof(tmask));
    uint32_t posReg = 0xFFFFFFFFu;                                       // the SV_Position input register
    for (uint32_t i = 2; i < n;) {
        const uint32_t op = WaterPs::Rd(b, base + (size_t)i * 4), opc = op & 0x7FFu;
        uint32_t ilen = (op >> 24) & 0x7Fu;
        if (opc == 0x35u) {
            if (i + 1 >= n) return r;
            ilen = WaterPs::Rd(b, base + (size_t)(i + 1) * 4);
        }
        if (ilen == 0 || ilen > n - i) return r;
        const uint32_t end = i + ilen;
        if (IsDecl(opc)) {
            if (opc == 88u && ilen >= 3) {                               // dcl_resource tN
                Opnd o;
                if (ParseOpnd(b, base, i + 1, end, o) && o.hasIdx0 && o.idx0 < 32) r.texDecl |= 1u << o.idx0;
            } else if (opc == 100u || opc == 97u) {                      // dcl_input_ps_siv / dcl_input_siv: the system value name
                Opnd o;
                const uint32_t j = ParseOpnd(b, base, i + 1, end, o);
                if (j && j < end && o.hasIdx0 && (WaterPs::Rd(b, base + (size_t)j * 4) & 0xFFFFu) == 1u) posReg = o.idx0;   // POSITION
            }
            i = end;
            continue;
        }
        uint32_t j = i + 1;
        bool ext = (op >> 31) != 0;
        while (ext) {
            if (j >= end) return r;
            ext = (WaterPs::Rd(b, base + (size_t)j * 4) >> 31) != 0;
            ++j;
        }
        Opnd ops[8];
        int no = 0;
        while (j < end && no < 8) {
            j = ParseOpnd(b, base, j, end, ops[no]);
            if (!j) return r;
            ++no;
        }
        if (j != end) return r;
        const int nDest = NoDest(opc) ? 0 : (TwoDest(opc) ? 2 : 1);
        uint8_t srcT[8][4] = {};
        uint32_t srcM[8][4] = {};
        bool srcCb[8] = {}, srcOne[8][4] = {};
        for (int k = nDest; k < no; ++k) {
            const Opnd& o = ops[k];
            for (int cc = 0; cc < 4; ++cc) {
                const int sc = o.comp(cc);
                if (o.type == kOpTemp && o.hasIdx0 && o.idx0 < (uint32_t)kTemps) { srcT[k][cc] = taint[o.idx0][sc]; srcM[k][cc] = tmask[o.idx0][sc]; }
                else if (o.type == kOpCb) { srcT[k][cc] = kC; srcCb[k] = true; }
                else if (o.type == kOpInput && o.hasIdx0 && o.idx0 != posReg) srcT[k][cc] = kV;
                else if (o.type == kOpImm32) srcOne[k][cc] = o.imm[o.nImm == 4 ? cc : 0] == 1u;
            }
        }
        auto any = [&](int k) { uint8_t u = 0; for (int cc = 0; cc < 4; ++cc) u |= srcT[k][cc]; return u; };
        auto anyM = [&](int k) { uint32_t u = 0; for (int cc = 0; cc < 4; ++cc) u |= srcM[k][cc]; return u; };
        if (opc == 13u && no >= 1) {                                     // discard_z / discard_nz
            ++r.discards;
            if (any(0) & kT) { r.kinds |= kCutout; r.discardTex |= anyM(0); }
        }
        const bool fetch = IsFetch(opc);
        uint32_t resSlot = 0xFFFFFFFFu;
        if (fetch) for (int k = nDest; k < no; ++k) if (ops[k].type == kOpResource && ops[k].hasIdx0) { resSlot = ops[k].idx0; break; }
        if ((opc == 45u || opc == 46u) && no >= 3) {                     // ld / ld_ms: operand 1 = the address
            const uint8_t u = any(1);
            if (u & kI) {
                ++r.pixelLds;
                if (resSlot < 32) r.pixelTex |= 1u << resSlot;
            }
        }
        if (IsSample(opc) && no >= 3) {                                  // sample*: operand 1 = the address
            const uint8_t u = any(1);
            if (u & (kP | kS | kQ)) ++r.projSamples;
            else if ((u & kV) && (u & kC)) ++r.scrollSamples;
        }
        if (!nDest) { i = end; continue; }
        uint8_t res[4] = {};
        uint32_t resM[4] = {};
        if (fetch) {
            for (int cc = 0; cc < 4; ++cc) { res[cc] = kT; resM[cc] = resSlot < 32 ? (1u << resSlot) : 0u; }
        } else {
            for (int cc = 0; cc < 4; ++cc) {
                uint8_t u = 0;
                uint32_t m = 0;
                for (int k = nDest; k < no; ++k) {
                    if (IsDot(opc)) { for (int c2 = 0; c2 < 4; ++c2) { u |= srcT[k][c2]; m |= srcM[k][c2]; } }
                    else { u |= srcT[k][cc]; m |= srcM[k][cc]; }
                }
                res[cc] = (uint8_t)(u & ~(kP | kS | kI | kPar));            // the pixel bits are earned below, not inherited by any op
                resM[cc] = m;
                // a pure move / swizzle / saturate / bitfield keeps the pixel bits
                if (opc == 54u || opc == 55u || opc == 1u || opc == 30u || opc == 35u || opc == 0u || opc == 140u)
                    res[cc] = u;
            }
            if (opc == 14u && no == 3 && ops[1].type == kOpInput && ops[2].type == kOpInput && ops[1].hasIdx0 && ops[2].hasIdx0 &&
                ops[1].idx0 == ops[2].idx0 && ops[1].idx0 != posReg) {
                for (int cc = 0; cc < 4; ++cc) res[cc] |= kP;            // div rX, vN.?, vN.? = a perspective divide of an interpolant
            }
            if (opc == 14u) {                                            // div of an already divided / scaled value keeps it
                for (int cc = 0; cc < 4; ++cc) res[cc] |= srcT[1][cc] & (kP | kS);
                if (no == 3 && ops[1].type == ops[2].type && (ops[1].type == kOpTemp || ops[1].type == kOpInput) && ops[1].hasIdx0 &&
                    ops[2].hasIdx0 && ops[1].idx0 == ops[2].idx0 && (any(1) & (kV | kT)))
                    for (int cc = 0; cc < 4; ++cc) res[cc] |= kQ;        // x.xy / x.z of an interpolated / fetched vector
            }
            if (opc == 56u || opc == 50u) {                              // mul / mad: P x (cbuffer value) -> a pixel position
                for (int cc = 0; cc < 4; ++cc) {
                    bool p = false, cb = false;
                    for (int k = 1; k < no && k < 3; ++k) {
                        if (srcT[k][cc] & (kP | kS)) p = true;
                        if (srcCb[k] || (srcT[k][cc] & kC)) cb = true;
                    }
                    if (p && cb) res[cc] |= kS;
                    else if (p) res[cc] |= kP;                           // P x something else stays a projected value
                    if (opc == 50u && (srcT[3][cc] & (kP | kS))) res[cc] |= srcT[3][cc] & (kP | kS);
                }
            }
            if (opc == 0u) {                                             // add: a projected value + an offset stays projected
                for (int cc = 0; cc < 4; ++cc) res[cc] |= (srcT[1][cc] | srcT[2][cc]) & (kP | kS);
            }
            if (opc == 27u || opc == 28u) {                              // ftoi / ftou of a pixel position
                for (int cc = 0; cc < 4; ++cc) if (srcT[1][cc] & kS) res[cc] |= kI;
            }
            if (opc == 1u && no == 3) {                                  // and x, (integer pixel), l(1) -> the parity
                for (int cc = 0; cc < 4; ++cc)
                    if (((srcT[1][cc] & kI) && srcOne[2][cc]) || ((srcT[2][cc] & kI) && srcOne[1][cc])) { res[cc] |= kPar; r.parity = true; }
            }
        }
        for (int d = 0; d < nDest; ++d) {
            const Opnd& o = ops[d];
            if (o.type != kOpTemp || !o.hasIdx0 || o.idx0 >= (uint32_t)kTemps) continue;
            const uint32_t mask = o.selMode == 0 ? o.sel : 0xFu;
            for (int cc = 0; cc < 4; ++cc) if (mask & (1u << cc)) { taint[o.idx0][cc] = res[cc]; tmask[o.idx0][cc] = resM[cc]; }
        }
        i = end;
    }
    r.parsed = true;
    if (r.pixelLds > 0) r.kinds |= kPixel;
    if (r.projSamples > 0 && (r.pixelLds > 0 || r.scrollSamples >= 2)) r.kinds |= kWater;
    return r;
}

// ---- the per-pair verdict ----------------------------------------------------------------------------------------------------
// what the mod's features know about one VS + PS pair (inject.cpp fills it from WaterPs / WireWiden / the cut-out mips)
enum CutReason : int {
    kCutUnknown = 0,     // the texture was never seen by the phase-77 uploader (not a BC3 mip-0 upload)
    kCutOff,             // cutout_mips = 0 (not analysed)
    kCutPatched,         // phase 77: our mips are waiting for its first discard draw
    kCutApplied,         // phase 77b: our mips are in
    kCutNoLoss,          // the game's own mips keep the coverage
    kCutSoft,            // a soft alpha (not a cut-out by the phase-77 / 107a rules)
    kCutCoverage,        // coverage outside 2..60 %
    kCutSize,            // the shape rule (array, partial box, not the full chain, over cutout_max_dim)
    kCutQueue,           // the worker queue was full
    kCutFormat,          // not BC3 (BC1 / BC7 / RGBA cut-outs are not handled by phase 77)
    kCutNoTex,           // no Texture2D bound in the alpha slot
    kCutPending,         // the phase-77 worker has not decided yet
    kCutSparse,          // phase 110b: coverage UNDER 2 % (kCoverage split by cov0): a sparse alpha the game's mips really lose
                         //   (phase 113: only when the game's REAL mips were not seen; else the worker decides on them)
    kCutLate,            // phase 113: never seen by the uploader (or its queue was full): being read back from the GPU for phase 77
    kCutNoWrite,         // phase 113: the texture cannot be patched (not D3D11_USAGE_DEFAULT: IMMUTABLE / DYNAMIC / staging)
    kCutOneMip,          // phase 114: ONE mip level -- no smaller mips that could lose its coverage (mip 0 is drawn at every distance)
    kCutRenderTarget,    // phase 114: a render target the game composes (the ATS plate / sign atlas 2048 x 2048 RGBA8): no stored alpha
    kCutOpaque,          // phase 114: a BC1 texture with no see-through texel (its discard never fires: the pennant flags)
    kCutBc1Alpha,        // phase 114: a BC1 texture WITH see-through texels (1-bit alpha): phase 77 rebuilds BC3 alphas only
    kCutCount
};
inline const char* CutReasonName(int r) {
    static const char* const k[kCutCount] = {
        "not seen by the uploader", "cutout_mips = 0", "patched (waiting for the first discard draw)", "patched (mips applied)",
        "no loss (game mips keep coverage)", "soft alpha (game mips keep coverage)", "coverage over 60 % (game mips keep it)",
        "shape / size rule", "worker queue full (re-checked)", "format not BC3", "no Texture2D in the alpha slot",
        "analysis pending (phase-77 worker)", "coverage under 2 % (sparse: the game's mips lose it)",
        "being read back from the GPU for the cut-out mips (phase 113)", "cannot be patched (not a DEFAULT-usage texture)",
        "one mip level (no smaller mips to lose coverage, phase 114)",
        "a render target the game draws (no stored alpha to rebuild, phase 114)",
        "BC1 without see-through texels (nothing to cut out, phase 114)",
        "BC1 with see-through texels (phase 77 rebuilds BC3 only)" };
    return (r >= 0 && r < kCutCount) ? k[r] : "?";
}
// phase 110b: what a texture's phase-77 decision means for the census. OK = the mod handles it or its game mips keep the coverage
// (evidence: the 2026-11-15 Norway capture -- every soft-alpha and over-60 % texture keeps 100..109 % of its mip-0 coverage at the
// 0.05 discard cut down to the 4 px mip: the game box-filters soft alpha, and a box average only spreads alpha past a low cut);
// WAIT = not decided yet (pending) or not analysed this time (the worker queue was full): re-checked at later draws, never tinted;
// BAD = unhandled and possibly losing coverage: no phase-77 analysis (cutout_mips = 0, not BC3, the shape rule, never seen by the
// uploader) or a sparse alpha under 2 % (1-texel lines: the game's 0.5-threshold mips drop them).
enum CutClass : int { kCutClsOk = 0, kCutClsWait = 1, kCutClsBad = 2 };
inline int CutClassOf(int r) {
    switch (r) {
    case kCutPatched: case kCutApplied: case kCutNoLoss: case kCutSoft: case kCutCoverage: case kCutNoTex: return kCutClsOk;
    case kCutOneMip: case kCutRenderTarget: case kCutOpaque: return kCutClsOk;   // phase 114 (the 2026-11-15 evening VR captures)
    case kCutQueue: case kCutPending: case kCutLate: return kCutClsWait;   // (113: the GPU read-back = a decision is on its way)
    default: return kCutClsBad;   // kCutUnknown, kCutOff, kCutSize, kCutFormat, kCutSparse, kCutNoWrite, kCutBc1Alpha
    }
}
inline bool CutCovered(int r) { return CutClassOf(r) != kCutClsBad; }
// the worse of two slot reasons (a PS with several alpha slots): bad > wait > ok > no texture
inline int CutWorse(int a, int b) {
    auto rank = [](int r) { return r == kCutNoTex ? -1 : CutClassOf(r); };
    return rank(b) > rank(a) ? b : a;
}
// phase 110b: the cut-out state of ONE PAIR over the textures it draws (a tree PS draws hundreds of textures; one texture's reason
// at first sight is not the pair's verdict). Each checked draw notes its texture's reason; the pair is a cut-out SUSPECT while a BAD
// texture was drawn within kCutWindow Presents (live: a texture the loader patches later stops counting after that window, and the
// tint already follows the draw's own texture). Shown = the reason the file prints and Judge uses.
constexpr uint64_t kCutWindow = 300;     // Presents (~5 s at 60 fps)
struct CutTrack {
    uint64_t at[3] = {};                 // per CutClass: Present + 1 of its last check (0 = never)
    int      why[3] = { kCutNoTex, kCutNoTex, kCutNoTex };
    uint64_t checks[3] = {};
    void Note(int r, uint64_t n) {
        const int c = r == kCutNoTex ? kCutClsOk : CutClassOf(r);
        at[c] = n + 1; why[c] = r; ++checks[c];
    }
    bool Recent(int c, uint64_t n) const { return at[c] != 0 && n + 1 >= at[c] && n + 1 - at[c] <= kCutWindow; }
    int Shown(uint64_t n) const {
        if (Recent(kCutClsBad, n)) return why[kCutClsBad];
        int best = -1;                   // else the most recent check (a bad one past the window = the pair was not drawn since)
        for (int c = 0; c < 3; ++c) if (at[c] && (best < 0 || at[c] > at[best])) best = c;
        return best < 0 ? kCutPending : why[best];
    }
};

// ---- phase 111: pairs judged "no fix needed" (or covered by a fix the kind rules do not see) from the user's capture -------------
// Each entry names the kinds it covers, the switch it needs (kKnownFixNone = always) and why, with the evidence. Matched by the full
// VS + PS checksums. Source: captures\census_2026-11-15\shimmer_20261115_104831\ (ETS2 flat, Norway coast road): per pair its
// picture.png (the pair alone tinted), textures, disassembly, and a jitter-locked flicker measure over the 12 consecutive rewind
// frames (the camera nearly still; a pixel whose luma follows the 8 jitter phases -- frame i = frame i + 8 -- but changes inside a
// cycle = flicker the jitter causes): building / house INTERIORS 0.02 - 0.09 (the whole frame 0.035, the delineator post 8.7 max).
// A dense far mesh (a building facade, a car, a terrain strip) has sub-pixel TRIANGLES but no gaps between them: "px per triangle"
// flags it, yet nothing dashes.
enum KnownFix : int { kKnownFixNone = 0, kKnownFixPole = 1 };
struct KnownPair { uint32_t vs[4]; uint32_t ps[4]; uint32_t kinds; int fix; const char* what; };
inline const KnownPair* KnownPairs(int* n) {
    static const KnownPair k[] = {
        { { 0x07dfaa00u, 0xd29866d9u, 0x6d22e7e7u, 0xdbbda59fu }, { 0xc92c7c22u, 0xf70cb3c9u, 0xd2d02d6du, 0x9760efe9u }, kThin, kKnownFixNone,
          "far building facades (atlas of windows / walls): a dense LOD mesh, no gaps; interior stable in the capture" },
        { { 0x9d73eba3u, 0x8758ec57u, 0x87efbcc4u, 0xe2f12efcu }, { 0x5396e5a9u, 0x270c2f1bu, 0xc5aac975u, 0xb1772c23u }, kThin, kKnownFixNone,
          "far industrial building / roof: a dense LOD mesh, no gaps; stable in the capture" },
        { { 0x5ebed025u, 0x92e52f34u, 0xa27693d0u, 0x2d3292ecu }, { 0x7c55aceeu, 0x88c13fd9u, 0xc62d7bc9u, 0x1e000558u }, kThin, kKnownFixNone,
          "far town houses (instanced low-poly LOD boxes, 12..174 indices): solid, a few px each; stable in the capture" },
        { { 0x97d9c72du, 0xdc7ef4e5u, 0x11336708u, 0x188dd9b0u }, { 0x0a1ed38au, 0xc9b6c781u, 0x2cf79f54u, 0x7b8028c2u }, kThin, kKnownFixNone,
          "the window-light forward pass of the far town houses above (same instances; its pixel read has the 109a twin)" },
        { { 0x3c957c7cu, 0x7d255c34u, 0x9f37a129u, 0xdfabb3f4u }, { 0x18e8e355u, 0x8eeac1b5u, 0xd475b889u, 0x06313532u }, kThin, kKnownFixNone,
          "ground / terrain strips (bank path, kerb edge, shore line; rock-grass texture): flat surface, the flicker seen there is "
          "the grass blades over it" },
        { { 0x07dfaa00u, 0xd29866d9u, 0x6d22e7e7u, 0xdbbda59fu }, { 0xad165830u, 0x89d9a42fu, 0x009833bau, 0xe84e35a0u }, kThin, kKnownFixNone,
          "grass-on-ground blend strips (grass albedo + noise mask; shore / boathouse ground): flat surface" },
        { { 0x1ec77518u, 0xf349de3eu, 0xc7581476u, 0xc5f2a9ffu }, { 0xb8636556u, 0xcfac603eu, 0x48c334a1u, 0xf5ac50a0u }, kThin, kKnownFixNone,
          "far AI / parked cars (car body atlas + reflection cube, 2400..3700 indices): a dense mesh, no gaps" },
        { { 0x8ba7d8dau, 0x4472b536u, 0xa56b922cu, 0xd29acdbbu }, { 0xfd5e16e5u, 0x53aa378fu, 0xf90f332bu, 0x42f0b8d5u }, kThin, kKnownFixNone,
          "far car glass / lamp overlay (additive forward, ~1 px per draw; its pixel read has the 109a twin)" },
        { { 0x2f9ff4a3u, 0xa04a3764u, 0x975cb93eu, 0x0c331fd0u }, { 0x438d1a63u, 0x447e789au, 0x21aea10au, 0x407feaceu }, kThin, kKnownFixNone,
          "far grass / hedge cards (cut-out alpha handled by phase 77; ~7 px per draw)" },
        { { 0xc8dddb32u, 0xba382cddu, 0xa6d40471u, 0xba1ebc27u }, { 0x438d1a63u, 0x447e789au, 0x21aea10au, 0x407feaceu }, kThin, kKnownFixNone,
          "flower clumps on grass banks (instanced cards, opaque alpha): no jitter flicker on the flower patch in the capture" },
        { { 0xdb9404adu, 0xf58046a7u, 0xb40372ebu, 0x0bb612cau }, { 0xa761133au, 0xcf396c94u, 0x4c52f1eau, 0xa375d132u }, kThin, kKnownFixPole,
          "forward re-light of the road-pole / delineator reflector band (same 57 instances as VS 2a9f326e + PS e19e0136): it rewrites "
          "the lit colour of the G-buffer band pole_aa dithers (+ a headlight glint term)" },
        // phase 113: the 2026-11-15 14:54 capture by the Volvo Trucks Center (census_2026-11-15\shimmer_20261115_145426). The truck was
        // moving there (same-phase rewind difference 0.23 vs within-cycle 0.10 for the whole frame), so the phase-111 jitter measure
        // reads motion; these three are judged from their shaders, state and coverage instead. None of them draws its own coverage:
        // each is a FORWARD pass with depth write off over a mesh the G-buffer pass already drew (or an additive glow), so a sub-pixel
        // triangle of theirs can only re-colour a pixel that is already there.
        { { 0x4186dac5u, 0xa4ee75cfu, 0xb37e764cu, 0xa4e7601cu }, { 0x9c062880u, 0xd52ad14bu, 0xaf899637u, 0xfd6cd6ffu }, kThin, kKnownFixNone,
          "forward re-light + reflection of opaque meshes (house walls, windows, metal: reads the G-buffer at its pixel, adds the "
          "environment cube x a mask; depth GREATER_EQUAL, no depth write, no blend): it re-colours pixels the G-buffer pass drew, its "
          "pixel read has the 109a twin; ~23 px per draw, 0.12 px per triangle = dense far LOD meshes" },
        { { 0x07dfaa00u, 0xd29866d9u, 0x6d22e7e7u, 0xdbbda59fu }, { 0xf8ac351du, 0x4440200eu, 0x3e8ef573u, 0xa3ab0b2du }, kThin, kKnownFixNone,
          "forward GLASS (lit + fogged, dual-source blend ONE x src + SRC1_COLOR x dst, no depth write): far window / shelter panes, "
          "~6 px per draw; a transparent tint over the frame the G-buffer drew -- a dither would punch holes in the glass" },
        { { 0xfaf4c362u, 0x77cd1912u, 0x02c43a33u, 0x1ef50869u }, { 0xe2128488u, 0x26e9ba1eu, 0xdfd5f2abu, 0xd203960du }, kThin, kKnownFixNone,
          "additive glow sprites (SRC_ALPHA + ONE, texture x vertex colour, alpha squared; no depth write): 0..6 px per draw, the "
          "0.00 px per triangle is most sprites hidden behind geometry, not a thin mesh" },
        // phase 114: the user's 2026-11-15 evening VR captures (captures\census_2026-11-15_evening\; ETS2 in a bus, ATS by a dealership).
        // The jitter probe confirmed these thin (their small parts), but each is either a MOVING vehicle -- phase 80 never dithers a
        // moving draw (a new coverage every frame = no DLSS history), so no solid kind may take it; its motion is the per-draw MVs' job --
        // or a pass that draws no coverage of its own. Kept out of wire_widen on purpose, not forgotten.
        { { 0x07dfaa00u, 0xd29866d9u, 0x6d22e7e7u, 0xdbbda59fu }, { 0x6ff10d0cu, 0x05678a80u, 0x84261808u, 0xafa71defu }, kThin, kKnownFixNone,
          "AI car body / trim PS (ATS; car atlas + reflection cube; picture: the whole oncoming Charger, tinted.png: none of its body): "
          "the thin draws are trims of MOVING cars -- phase 80 keeps moving draws undithered, their motion is the per-draw MVs'" },
        { { 0x07dfaa00u, 0xd29866d9u, 0x6d22e7e7u, 0xdbbda59fu }, { 0xa5577deeu, 0x2174eacau, 0x0198eb19u, 0x60b5feaau }, kThin, kKnownFixNone,
          "truck / trailer chassis atlas PS (ETS2; reefer unit, wheels, frame parts + reflection cube; ~13 px per draw): small parts of "
          "MOVING AI trucks / trailers -- phase 80 keeps moving draws undithered, their motion is the per-draw MVs'" },
        { { 0x4186dac5u, 0xa4ee75cfu, 0xb37e764cu, 0xa4e7601cu }, { 0x68ad6bcbu, 0x553fcd72u, 0xaca9f158u, 0x11bdd316u }, kThin, kKnownFixNone,
          "forward ADDITIVE reflection (SRC_ALPHA + ONE, depth GREATER_EQUAL, no depth write; env cube x a BC4 mask, reads the G-buffer "
          "at its pixel with the 109a twin): it adds light to pixels the G-buffer pass drew -- the same case as 4186dac5_9c062880" },
        { { 0x4a6b7f2au, 0x0ddf2371u, 0x1138e332u, 0x48654163u }, { 0x73c69246u, 0x34e451ebu, 0x7cfea778u, 0x9fc71ec7u }, kThin, kKnownFixNone,
          "ground / terrain strips (ATS; dirt-grass albedo 2048 x 2048 + normal maps, 42..432 indices, ~5 px per draw): narrow ground "
          "pieces lying on ground of the same look -- the 3c957c7c_18e8e355 case; a dither would punch holes in the ground" },
    };
    if (n) *n = (int)(sizeof(k) / sizeof(k[0]));
    return k;
}
inline int KnownPairIndex(const uint32_t vs[4], const uint32_t ps[4]) {
    int n = 0;
    const KnownPair* k = KnownPairs(&n);
    for (int i = 0; i < n; ++i)
        if (!memcmp(k[i].vs, vs, 16) && !memcmp(k[i].ps, ps, 16)) return i;
    return -1;
}

struct Cover {
    int  knownPair = -1;     // phase 111: index into KnownPairs (-1 none)
    int  waterRule = 0;      // WaterPs rule of the PS (0 = not a phase-97 shader)
    bool pixelFix = false;   // phase 109a: the PS has a pixel-reader twin (PixelReader::Make: its pixel loads read at SV_Position)
    bool gameWire = false;   // phase 109b: the game-widened power-line pair (VS 277d2302 + PS 69dcaf36: depth batch without A2C)
    int  wirePsKind = 0;     // WireWiden: 1 plain / 2 cut wire PS
    bool wireVs = false;     // the VS is the wire VS
    int  solidKind = 0;      // WireWiden solid PS kind 1..8 (0 none; 4 = D poles / railings, 8 = H pole stripes / signs)
    bool solidVs = false;    // the VS is in that kind's VS set
    int  cutReason = kCutUnknown;
    // phase 110b: a pair is KNOWN for a kind only while the fix for it is ON (the live switches; default on = the unit tests)
    bool wireOn = true;      // wire_widen (and its Alt+F12 live state): wire pairs + solid kinds A..G
    bool poleOn = true;      // pole_aa (phase 109d): solid kinds D (pole bodies) and H (pole stripes / signs)
    bool powerLineOn = true; // power_line_fix (phase 109b)
    bool prFixOn = true;     // pixel_reader_fix (phase 109a)
    bool waterFixOn = true;  // water_overscan_fix (phase 97)
    bool overscan = true;    // jitter_overscan: 0 = no overscan, a pixel index read from an interpolant is not off by one
};
// kinds of the pair (structural + the pair facts) and which of them a feature covers
struct Verdict {
    uint32_t kinds = 0, covered = 0;
    uint32_t Suspect() const { return kinds & ~covered; }
};
// phase 110b: a fix for this pair's thinness exists AND is on (wire pairs / solid kinds A..G = wire_widen; D also pole_aa; H only
// pole_aa -- SolidOnDraw skips H; the game-widened power line = power_line_fix)
inline bool ThinFixOn(const Cover& c) {
    if (c.wirePsKind && c.wireVs && c.wireOn) return true;
    if (c.gameWire && c.powerLineOn) return true;
    if (c.solidKind && c.solidVs) {
        if (c.solidKind == 8) return c.poleOn;
        if (c.solidKind == 4) return c.wireOn || c.poleOn;
        return c.wireOn;
    }
    return false;
}
// the thin fix this pair WOULD have (for the why text): "" none
inline const char* ThinFixName(const Cover& c) {
    if (c.wirePsKind && c.wireVs) return "wire_widen";
    if (c.gameWire) return "power_line_fix";
    if (c.solidKind && c.solidVs) return c.solidKind == 8 ? "pole_aa" : (c.solidKind == 4 ? "wire_widen / pole_aa" : "wire_widen");
    return "";
}
// `thin` = the pair's thinness is CONFIRMED (phase 113b: the px-per-triangle pre-filter AND the jitter probe; before: the pre-filter)
inline Verdict Judge(uint32_t structural, const Cover& c, bool thin) {
    Verdict v;
    v.kinds = structural & (kPixel | kCutout | kWater);
    if (thin) v.kinds |= kThin;
    // phase 113b: unknown-VS only for a confirmed thin pair (bdce664b_2585159b drew the Volvo Trucks Center's big windows: a known
    // solid PS with a new VS that flickers nowhere -- nothing for wire_widen to fix there)
    if (thin && ((c.wirePsKind && !c.wireVs) || (c.solidKind && !c.solidVs))) v.kinds |= kVs;
    // phase 97: the overscan-exempt checker shaders (rules 1..4; phase 109c's VRS-only rule fixes no read)
    if (c.waterRule && c.waterRule != WaterPs::kRuleVrsOnly && c.waterFixOn) v.covered |= kPixel | kWater;
    if (c.pixelFix && c.prFixOn) v.covered |= kPixel | kWater;   // phase 109a: its index reads at SV_Position under the overscan
    if (!c.overscan) v.covered |= kPixel | kWater;    // no overscan: the interpolated pixel index is the right one
    if (ThinFixOn(c)) v.covered |= kThin;
    if (CutCovered(c.cutReason)) v.covered |= kCutout;
    if (c.knownPair >= 0) {                           // phase 111: the known list (with its switch)
        int n = 0;
        const KnownPair* k = KnownPairs(&n);
        if (c.knownPair < n && (k[c.knownPair].fix == kKnownFixNone || (k[c.knownPair].fix == kKnownFixPole && c.poleOn)))
            v.covered |= k[c.knownPair].kinds;
    }
    v.covered &= v.kinds;
    return v;
}
// phase 113 (the legend's "Some colours mean a fix is switched off" line): the suspect kinds of this pair that a fix of the mod WOULD
// cover if every switch were on (wire_widen, pole_aa, power_line_fix, pixel_reader_fix, water_overscan_fix), + a cut-out whose only
// reason is cutout_mips = 0. 0 = nothing the player can switch on for it.
inline uint32_t FixOffKinds(uint32_t structural, const Cover& c, bool thin) {
    Cover a = c;
    a.wireOn = a.poleOn = a.powerLineOn = a.prFixOn = a.waterFixOn = true;
    const uint32_t sus = Judge(structural, c, thin).Suspect();
    uint32_t k = sus & Judge(structural, a, thin).covered;
    if ((sus & kCutout) && c.cutReason == kCutOff) k |= kCutout;
    return k;
}
// phase 116 (the user: a player with a fix switched off saw those objects flash and captured them -- the fix is known, so that was
// wasted time and noise in the bug reports): the kinds of a draw that are SHOWN (tinted, counted "to capture", captured) = its tint
// kinds minus the kinds a switched-off fix would cover. 0 with k != 0 = the draw is hidden only because a fix is off. A draw that
// also has a kind no fix covers still flashes, in the colour of the kinds left.
inline uint32_t ShownKinds(uint32_t k, uint32_t fixOff) { return k & ~fixOff; }
// phase 116: a pair the bug report's census part captures / the bug icon counts "to capture": a suspect kind no switch would fix
inline bool CapWanted(uint32_t suspect, uint32_t fixOff) { return ShownKinds(suspect, fixOff) != 0; }

// the impact score the census file is sorted by: how often it is on screen x how big (pixels when measured) x the kind's weight
inline double Impact(uint32_t suspect, uint64_t framesSeen, double avgPx, uint64_t draws) {
    if (!suspect) return 0.0;
    double w = 0.0;
    if (suspect & kPixel) w += 3.0;
    if (suspect & kWater) w += 2.0;
    if (suspect & kThin) w += 2.0;
    if (suspect & kCutout) w += 1.0;
    if (suspect & kVs) w += 1.0;
    const double size = avgPx > 0.0 ? 1.0 + avgPx / 1000.0 : 1.0 + (double)draws / (double)(framesSeen ? framesSeen : 1) / 10.0;
    return w * (double)framesSeen * size;
}

// "{ 0x9a5fca98u, 0x48aeb8a1u, 0x0f1346bau, 0x64390c54u }" -- the mod's ChecksumIs constant format (wire_widen.cpp)
inline std::string ConstText(const uint32_t s[4]) {
    char b[80];
    snprintf(b, sizeof(b), "{ 0x%08xu, 0x%08xu, 0x%08xu, 0x%08xu }", s[0], s[1], s[2], s[3]);
    return b;
}
inline std::string Short(const uint32_t s[4]) { char b[12]; snprintf(b, sizeof(b), "%08x", s[0]); return b; }

// ---- DDS (DX10 header) for the texture dumps -------------------------------------------------------------------------------
// bytes per block and block size of a DXGI format (0 = not supported by the dump). DXGI numbers as in dxgiformat.h.
struct FmtInfo { uint32_t bytes = 0, block = 1; };
inline FmtInfo FormatInfo(uint32_t f) {
    switch (f) {
    case 2: case 3: case 4: return { 16, 1 };                                           // R32G32B32A32
    case 10: case 11: case 12: case 13: case 14: case 9: return { 8, 1 };               // R16G16B16A16
    case 16: case 17: case 18: return { 8, 1 };                                         // R32G32
    case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31: case 32: return { 4, 1 };   // R10G10B10A2, R11G11B10, RGBA8
    case 34: case 35: case 36: case 37: case 38: case 33: return { 4, 1 };              // R16G16
    case 39: case 40: case 41: case 42: case 43: return { 4, 1 };                       // R32
    case 48: case 49: case 50: case 51: case 52: return { 2, 1 };                       // R8G8
    case 53: case 54: case 55: case 56: case 57: case 58: return { 2, 1 };              // R16
    case 60: case 61: case 62: case 63: case 64: case 65: return { 1, 1 };              // R8, A8
    case 87: case 88: case 90: case 91: case 92: case 93: return { 4, 1 };              // B8G8R8A8 / X8
    case 70: case 71: case 72: case 79: case 80: case 81: return { 8, 4 };              // BC1, BC4
    case 73: case 74: case 75: case 76: case 77: case 78: return { 16, 4 };             // BC2, BC3
    case 82: case 83: case 84: case 94: case 95: case 96: case 97: case 98: case 99: return { 16, 4 };   // BC5, BC6H, BC7
    default: return {};
    }
}
inline uint32_t MipDim(uint32_t d, uint32_t m) { const uint32_t v = d >> m; return v ? v : 1u; }
// tightly packed row bytes / rows of one mip
inline void MipRows(uint32_t fmt, uint32_t w, uint32_t h, uint32_t* rowBytes, uint32_t* rows) {
    const FmtInfo fi = FormatInfo(fmt);
    const uint32_t bw = (w + fi.block - 1) / fi.block, bh = (h + fi.block - 1) / fi.block;
    *rowBytes = bw * fi.bytes;
    *rows = bh;
}
// the 148-byte header: "DDS " + DDS_HEADER (124) + DDS_HEADER_DXT10 (20)
inline std::vector<uint8_t> DdsHeader(uint32_t fmt, uint32_t w, uint32_t h, uint32_t mips, uint32_t arraySize, bool cube) {
    std::vector<uint8_t> o(148, 0);
    auto put = [&o](size_t at, uint32_t v) { memcpy(&o[at], &v, 4); };
    put(0, 0x20534444u);                         // "DDS "
    put(4, 124);                                 // dwSize
    put(8, 0x1u | 0x2u | 0x4u | 0x1000u | 0x20000u);   // CAPS | HEIGHT | WIDTH | PIXELFORMAT | MIPMAPCOUNT
    put(12, h);
    put(16, w);
    uint32_t rb = 0, rows = 0;
    MipRows(fmt, w, h, &rb, &rows);
    put(20, FormatInfo(fmt).block > 1 ? rb * rows : rb);   // pitch / linear size of mip 0
    put(24, 1);                                  // depth
    put(28, mips);
    put(76, 32);                                 // ddspf.dwSize
    put(80, 0x4u);                               // DDPF_FOURCC
    put(84, 0x30315844u);                        // "DX10"
    put(108, 0x1000u | (mips > 1 ? 0x400008u : 0u));   // TEXTURE | (MIPMAP | COMPLEX)
    put(112, cube ? 0xFE00u : 0u);               // caps2: all cube faces
    put(128, fmt);                               // DXT10: dxgiFormat
    put(132, 3);                                 // D3D10_RESOURCE_DIMENSION_TEXTURE2D
    put(136, cube ? 0x4u : 0u);                  // DDS_RESOURCE_MISC_TEXTURECUBE
    put(140, cube ? arraySize / 6 : arraySize);  // arraySize (cube: number of cubes)
    put(144, 0);
    return o;
}

// ---- phase 110: the census part of a bug report (the capture key pressed twice while the census view is on) -----------------
// WHY a pair is a suspect, in words: one line per suspect kind, with the facts the verdict used (the dump's draw.txt + index.txt)
// phase 113b: probes / thinProbes / stable = the jitter probe's facts (probes < 0: not given)
inline std::string WhyText(const Verdict& v, const Cover& c, const PsScan& s, double pxPerTri, int thinSamples, int probes = -1,
                           int thinProbes = 0, double stable = -1.0) {
    const uint32_t sus = v.Suspect();
    std::string o;
    char b[720];
    char pr[160] = "";
    if (probes >= 0)
        snprintf(pr, sizeof(pr), "; jitter probe: %d of %d probe(s) thin, %.0f %% of its pixels covered at all 8 jitter offsets (thin "
                 "below 35 %%)", thinProbes, probes, stable * 100.0);
    if (sus & kPixel) {
        snprintf(b, sizeof(b), "  why pixel-reader: the PS reads t-slots 0x%x with `ld` at a pixel index computed from an interpolant "
                 "(%d such loads%s) -- one texel off under the jitter overscan; not a phase-97 water shader (water_ps rule %d) and "
                 "no SV_Position twin (phase 109a: pixel_reader_fix off, or PixelReader::Make refused this shape)\n",
                 s.pixelTex, s.pixelLds, s.parity ? ", parity taken" : "", c.waterRule);
        o += b;
    }
    if (sus & kWater) {
        snprintf(b, sizeof(b), "  why water-like: %d projective sample(s) + %d pixel load(s) / %d scrolling sample(s); not classified by "
                 "phase 97 (water_ps rule %d) and no twin\n", s.projSamples, s.pixelLds, s.scrollSamples, c.waterRule);
        o += b;
    }
    if (sus & kCutout) {
        snprintf(b, sizeof(b), "  why cut-out: %d discard(s) on the alpha of t-slots 0x%x; the texture there: %s\n", s.discards,
                 s.discardTex, CutReasonName(c.cutReason));
        o += b;
    }
    if ((sus & kThin) && c.knownPair >= 0) {           // phase 111: a known-list pair whose switch is off
        int n = 0;
        const KnownPair* k = KnownPairs(&n);
        if (c.knownPair < n) {
            snprintf(b, sizeof(b), "  why thin: %.2f px per triangle over %d sample(s)%s; known pair (%s) -- its fix pole_aa is OFF\n",
                     pxPerTri, thinSamples, pr, k[c.knownPair].what);
            o += b;
        }
    } else if (sus & kThin) {
        const char* fx = ThinFixName(c);
        snprintf(b, sizeof(b), "  why thin: %.2f px per triangle over %d sample(s) (candidate below %.2f)%s; %s%s%s (wire PS kind %d%s, "
                 "solid PS kind %d%s)\n", pxPerTri, thinSamples, kThinPxPerTri, pr,
                 *fx ? "its fix " : "not a wire_widen / pole_aa / power-line pair",
                 fx, *fx ? " is OFF" : "", c.wirePsKind, c.wirePsKind ? (c.wireVs ? " with its VS" : " without its VS") : "",
                 c.solidKind, c.solidKind ? (c.solidVs ? " with a VS of its set" : " with a VS outside its set") : "");
        o += b;
    }
    if (sus & kVs) {
        snprintf(b, sizeof(b), "  why unknown-VS: the PS is a known %s PS (kind %d) but this VS is not in its VS set -- add the VS to "
                 "wire_widen.cpp's set after checking its output layout\n", c.wirePsKind ? "wire" : "solid",
                 c.wirePsKind ? c.wirePsKind : c.solidKind);
        o += b;
    }
    if (!sus) o += "  (no suspect kind: every kind of this pair is covered)\n";
    return o;
}

// what the capture does with a suspect pair it sees in its armed frames
enum CapAction : int {
    kCapNone = 0,        // not decided yet
    kCapDump,            // a full dump into this capture (shaders, disassembly, textures, constant buffers, state, picture)
    kCapLink,            // dumped earlier this session and that folder still exists: state + picture here, the rest linked
    kCapTextOnly,        // the dump budget is spent (census_max_mb / the capture's pair cap): state + verdict + picture only
};
inline const char* CapActionName(int a) {
    switch (a) {
    case kCapDump: return "dumped here";
    case kCapLink: return "linked (dumped earlier)";
    case kCapTextOnly: return "text only (dump budget spent)";
    default: return "-";
    }
}
// the decision at the pair's first draw of the capture: an earlier full dump that is still on disk is linked; else a full dump
// while the budget allows (pairs dumped in this capture < maxPairs, the disk budget not spent); else text only
inline int CapDecide(bool dumpedBefore, bool earlierOnDisk, int dumpedThisCapture, int maxPairs, bool budgetOk) {
    if (dumpedBefore && earlierOnDisk) return kCapLink;
    if (dumpedThisCapture >= maxPairs || !budgetOk) return kCapTextOnly;
    return kCapDump;
}
// phase 115: a capture action that SAVED the pair -- a full dump, or a link to its earlier full dump (CapturedHidden: not highlighted
// again this session). Text only (the dump budget was spent) saved no shaders / textures: the pair keeps flashing.
inline bool CapSaves(int action) { return action == kCapDump || action == kCapLink; }
// phase 115: the bug icon's count -- distinct pairs still to capture that were SHOWN (flashing) in the window, and the pairs captured
// this session. shownAt = the Present + 1 a pair was last shown (0 = never), fromN = the window's first Present + 1.
// phase 116: + fixOff = distinct pairs drawn in the window that were NOT shown only because a fix is switched off (fixOffAt = the
// Present + 1 a pair was last hidden for that reason; null = none). A pair also shown in the window counts "to capture" only.
struct BugCount { int toCapture = 0, captured = 0, fixOff = 0; };
inline BugCount CountBug(const uint64_t* shownAt, const int* capSession, size_t n, uint64_t fromN, const uint64_t* fixOffAt = nullptr) {
    BugCount c;
    for (size_t i = 0; i < n; ++i) {
        if (capSession[i] > 0) { ++c.captured; continue; }
        if (shownAt[i] && shownAt[i] >= fromN) ++c.toCapture;
        else if (fixOffAt && fixOffAt[i] && fixOffAt[i] >= fromN) ++c.fixOff;
    }
    return c;
}
// the order of the per-pair pictures (each needs ~8 Presents with the pair alone tinted): the biggest on screen in the armed frames
// first (occlusion pixels), then the impact, then the draws; at most maxPictures
struct CapRank { int pair = -1; uint64_t px = 0; double impact = 0.0; int draws = 0; };
inline std::vector<int> CapPictureOrder(std::vector<CapRank> v, int maxPictures) {
    std::stable_sort(v.begin(), v.end(), [](const CapRank& a, const CapRank& b) {
        if (a.px != b.px) return a.px > b.px;
        if (a.impact != b.impact) return a.impact > b.impact;
        return a.draws > b.draws;
    });
    std::vector<int> o;
    for (const CapRank& r : v) { if ((int)o.size() >= maxPictures) break; o.push_back(r.pair); }
    return o;
}
// one row of the capture's census\index.txt (filled on the render thread, formatted anywhere)
struct CapRow {
    std::string folder;          // "<vs8>_<ps8>" inside the capture's census folder
    std::string vs, ps;          // ConstText of each ("" = a VS created before the hooks)
    std::string kinds;           // the suspect kinds
    int    action = kCapNone;
    std::string link;            // kCapLink: where the full dump is (relative to the game folder)
    uint64_t px = 0;             // occlusion pixels summed over the armed frames
    int    pxQueries = 0, draws = 0;
    std::string picture;         // "picture.png (Present #.., +x.x s after the key)" or why not
    std::string why;             // WhyText
    std::string tex;             // per-slot texture status (incl. late grabs)
};
inline std::string CapIndexText(const std::string& head, const std::vector<CapRow>& rows) {
    std::string s = head;
    char b[512];
    int n = 0;
    for (const CapRow& r : rows) {
        snprintf(b, sizeof(b), "\n== %d  %s\\   %s\n  VS %s\n  PS %s\n  in the armed frames: %d draw(s), %llu px over %d occlusion "
                 "quer%s%s\n  action: %s%s%s\n  picture: %s\n", ++n, r.folder.c_str(), r.kinds.c_str(),
                 r.vs.empty() ? "(created before the mod's hooks: no checksum)" : r.vs.c_str(), r.ps.c_str(), r.draws,
                 (unsigned long long)r.px, r.pxQueries, r.pxQueries == 1 ? "y" : "ies",
                 r.pxQueries ? "" : " (no coverage measured)", CapActionName(r.action), r.link.empty() ? "" : " -> ",
                 r.link.c_str(), r.picture.c_str());
        s += b;
        s += r.why;
        if (!r.tex.empty()) { s += "  textures:\n"; s += r.tex; }
    }
    if (rows.empty()) s += "\n(no suspect pair was drawn in the armed frames -- nothing the census flags was on screen)\n";
    return s;
}

}  // namespace Census

// ---- the tint pixel shader (one colour from a constant buffer at b13; no inputs, links with any VS) ---------------------------
// PSMain = forward draws (RT0 = the HDR colour); PSGbuf (phase 109r) = G-buffer draws: the SCS G-buffer is o0 normal, o1 specular,
// o2 ALBEDO (the bit-exact ports PSSolidA..H in wire_widen.cpp), so the tint goes to SV_Target2 (blend state: RT2 only).
// Phase 115: the MIRRORS keep that in-place tint (PSMain / PSGbuf). The main world passes write the census MASK instead (PSMask0..2:
// one uint word per pixel, see MaskWord above; b13 = uint4(kind + 1, depth check, 0, 0)) and the blit PAINTS it (VSPaint + PSPaint:
// a full-screen triangle over the blit source; t0 = the mask, t1 = the pass's final depth (R32_FLOAT_X8X24 view), b0 = Paint below).
// PSMask2's COLOR0 is declared to match the game VS's o0 (it is never read: the mask PS only needs SV_Position at v1).
// kCensusTintPs-BEGIN (the build validates this block with fxc: ps_5_0 / PSMain, PSGbuf, PSMask0..2, PSPaint; vs_5_0 / VSPaint)
inline constexpr char kCensusTintPs[] = R"(
cbuffer CensusTint : register(b13) { float4 TintColour; };
float4 PSMain() : SV_Target0 { return TintColour; }
float4 PSGbuf() : SV_Target2 { return TintColour; }

cbuffer CensusMask : register(b13) { uint4 MaskCode; };
uint MaskOf(float z) {
    if (MaskCode.y == 0u) return MaskCode.x | 8u;
    uint w = (asuint(z) & 0x3FFFFFF0u) | MaskCode.x;
    return MaskCode.y == 1u ? w : (w | 0x80000000u | (MaskCode.y == 3u ? 0x40000000u : 0u));
}
uint PSMask0() : SV_Target0 { return MaskCode.x | 8u; }
uint PSMask1(float4 pos : SV_Position) : SV_Target0 { return MaskOf(pos.z); }
uint PSMask2(float4 col : COLOR0, float4 pos : SV_Position) : SV_Target0 { return MaskOf(pos.z); }

cbuffer Paint : register(b0) {
    float4 Dim;        // mask w, h, target w, h
    float4 Prm;        // cell px, phase (0 / 1), depth check available (1 / 0), output: 0 = linear (an _SRGB view / float), 1 = sRGB bytes
    float4 Org;        // checker anchor (render px), 0, 0
    float4 Col[5];     // the kinds' screen colours, sRGB 0..1 (Census::KindColour)
};
Texture2D<uint>  CMask  : register(t0);
Texture2D<float> CDepth : register(t1);
float4 VSPaint(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float3 ToLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow(abs((c + 0.055) / 1.055), 2.4); }
float4 PSPaint(float4 p : SV_Position) : SV_Target0 {
    int2 m = int2(floor(p.xy * Dim.xy / Dim.zw));
    uint w = CMask.Load(int3(m, 0));
    uint code = w & 7u;
    if (code == 0u || code > 5u) discard;
    if ((w & 8u) == 0u && Prm.z > 0.5) {
        uint a = w & 0x3FFFFFF0u, b = asuint(CDepth.Load(int3(m, 0))) & 0x3FFFFFF0u;
        if ((w & 0x80000000u) == 0u) { if ((a > b ? a - b : b - a) > 32u) discard; }
        else if ((w & 0x40000000u) != 0u) { if (b > a + 32u) discard; }
        else if (b + 32u < a) discard;
    }
    int2 c = int2(floor((p.xy - Org.xy) / Prm.x));
    bool on = ((c.x + c.y + (int)Prm.y) & 1) == 0;
    float3 col = on ? Col[code - 1u].rgb : float3(0, 0, 0);
    return float4(Prm.w > 0.5 ? col : ToLinear(col), 1);
}
)";
// kCensusTintPs-END
