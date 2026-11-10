// OutsideAa -- phase 81: "outside AA" (dlaa.ini outside_aa). The pixels OUTSIDE the DLAA area rect get an unjittered, edge-smoothed
// value instead of the raw jittered frame. Pure geometry + the HLSL block; scene_dlaa.cpp runs it, tests/outside_aa_test.cpp (WARP)
// checks it. Both builds include it (the maths is plain C++; the HLSL is only a string).
//
// WHY (measured on a VR video, still head, dlaa_area 50, DLSS Quality 2072x2928 -> 2761x2928 per rect): 89 % of the strong-flicker
// pixels (thin dark edges against a bright sky: bridge railings and undersides, sign gantries, street-light heads, sign rims) lie
// OUTSIDE the AA rect; inside it the same railing is steady to 0.01 px. Outside the rect the picture is the raw frame, and the mod's
// sub-pixel viewport jitter (inject.cpp ReconcileViewports: viewport 0 TopLeft += (g_jx, g_jy), in [-0.5, +0.5) px, the 8 Halton
// phases) moves ALL of it every frame -- worse than the game without the mod, which does not jitter at all.
// HOW (per output pixel o outside the rect's fully-AA'd interior):
//   1. UNJITTER. The jittered viewport puts the scene point that belongs at render position X at X + s (s = the pass's viewport
//      shift; jitter_overscan's (W+1)/W scale is the same every frame and is also what DLSS sees, so it stays). Resampling the raw
//      frame at X + s therefore gives the picture of a shift-0 frame. The render position of o: X = o_c * render / output (o_c = o's
//      centre; in place render = output). In uv: uv = o_c / output + s / render (Params[1]). s = NGX jitter * jitter sign (the mod
//      tells NGX sign * shift, inject.cpp `njx = g_signX * slot.jx`; ShiftFromNgx undoes it). The kernel is a 5-tap Catmull-Rom
//      (9-tap bilinear-optimised bicubic minus its 4 tiny corner taps, renormalised) clamped into the 2x2 texels around the sample:
//      plain bilinear would be sharp at fraction 0 and blurred at 0.5, i.e. the sharpness would pulse with the jitter phase (its own
//      flicker); Catmull-Rom keeps the passband nearly flat over the phase.
//   2. EDGE AA (outside_aa = 2): FXAA 3.11 "console" on the unjittered value (4 diagonal bilinear lumas, 2 + 2 taps along the edge,
//      the two-tap fallback when the 4-tap blend leaves the local luma range). It softens the stair steps of thin dark-on-sky edges.
//      Luma: encoded LDR as is; linear LDR (an _SRGB view) and HDR (reversibly compressed l / (1 + l)) through sqrt, so the FXAA
//      thresholds see a perceptual scale in all three.
// WHERE (the feather): the rect's own blend ramp (dlaa_area_feather, scene_dlaa.cpp kRcasShader / kCompositePs: per axis the min
// of saturate(distance to an inside edge / feather), smoothstep'd, the axes multiplied) decides. Its complement is covered: every
// pixel whose mask is < 1 gets the outside value, and the DLAA result is blended over it with the mask as before, so the seam is
//   final = mask * DLAA + (1 - mask) * outsideAA      (was: mask * DLAA + (1 - mask) * raw jittered)
// Interior() = the pixels where the mask is exactly 1 (left alone, no cost); Bands() = its complement as <= 4 boxes.
// The two paths (scene_dlaa.cpp):
//   * DLAA in place (v0.6.5 copy-back, VR world eyes with dlss_upscale 0, the VR truck-preview units): CSMain reads the game's
//     colour (a UNORM / SRGB / RGBA16F SRV) BEFORE the copy-back and writes a full-size scratch; RCAS's border blend takes its
//     "raw" input from that scratch (instead of colorIn); the 4 outer bands are copied back next to the rect.
//   * DLSS upscale (v0.7.0 composite, the user's VR config): after the game's blit has bilinear-stretched the raw frame into the
//     eye RT, PSMain draws the bands (render -> output mapping, jitter in RENDER px converted to uv) into RT0 with alpha 1, then the
//     composite draws the DLSS result with the feather as alpha on top. The source is the blit's own SRV0 (its _SRGB decode or not
//     = exactly what the game's blit wrote).
// COST (estimate, not measured): 5 bilinear + 4 point fetches per pixel (mode 1), + 8 bilinear (mode 2), on 75 % of the pixels at
// dlaa_area 50; the in-place path also copies the outer bands back. GpuPerf section "outside-aa" measures it per eye.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace OutsideAa {

struct Box { int32_t x0, y0, x1, y1; };          // [x0, x1) x [y0, y1), image px

inline bool Empty(const Box& b) { return b.x1 <= b.x0 || b.y1 <= b.y0; }

// The feather ramp of one pixel exactly as the shaders compute it (float maths): rect-local position l (px, 0-based), rect size
// n, feather f (px, 0 = hard edge), lo / hi = that rect edge lies inside the image (an edge on the image border is not feathered).
inline float AxisRamp(int l, int n, float f, bool lo, bool hi) {
    if (!(f > 0.0f)) return 1.0f;
    const float a = (float)l + 0.5f, b = (float)n - a;
    auto sat = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    const float ra = lo ? sat(a / f) : 1.0f, rb = hi ? sat(b / f) : 1.0f;
    return ra < rb ? ra : rb;
}
inline float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }   // HLSL smoothstep(0, 1, t) for t in [0, 1]
// mask at image pixel (x, y) for the rect `r`; edges = { left, top, right, bottom } inside the image. Outside the rect: 0.
inline float Mask(int x, int y, const Box& r, float fx, float fy, const bool edges[4]) {
    if (x < r.x0 || x >= r.x1 || y < r.y0 || y >= r.y1) return 0.0f;
    return Smooth(AxisRamp(x - r.x0, r.x1 - r.x0, fx, edges[0], edges[2])) *
           Smooth(AxisRamp(y - r.y0, r.y1 - r.y0, fy, edges[1], edges[3]));
}

// One axis of Interior: the rect-local pixels l with l + 0.5 >= f (feathered low edge) and n - (l + 0.5) >= f (high edge).
inline void InteriorAxis(int32_t r0, int32_t n, float f, bool lo, bool hi, int32_t* i0, int32_t* i1) {
    int32_t a = r0, b = r0 + n;
    if (f > 0.0f) {
        if (lo) a = r0 + (int32_t)std::ceil((double)f - 0.5);
        if (hi) b = r0 + (int32_t)std::floor((double)n - 0.5 - (double)f) + 1;
    }
    if (a < r0) a = r0;
    if (b > r0 + n) b = r0 + n;
    if (b < a) b = a;                               // feather wider than half the rect: no interior
    *i0 = a; *i1 = b;
}
// The pixels where the feather mask is exactly 1 (the pass leaves them alone). Feather 0 / no edge inside = the rect itself.
inline Box Interior(const Box& r, float fx, float fy, const bool edges[4]) {
    Box b{};
    InteriorAxis(r.x0, r.x1 - r.x0, fx, edges[0], edges[2], &b.x0, &b.x1);
    InteriorAxis(r.y0, r.y1 - r.y0, fy, edges[1], edges[3], &b.y0, &b.y1);
    if (Empty(b)) b = Box{ r.x0, r.y0, r.x0, r.y0 };
    return b;
}
// The complement of `in` inside [0, w) x [0, h) as up to 4 disjoint boxes: top and bottom full width, left and right between them.
// Returns the count (empty boxes are left out). An empty `in` = the whole image (1 box).
inline int Bands(uint32_t w, uint32_t h, const Box& in, Box out[4]) {
    int n = 0;
    if (Empty(in)) { out[n++] = Box{ 0, 0, (int32_t)w, (int32_t)h }; return w && h ? 1 : 0; }
    const Box c[4] = {
        { 0, 0, (int32_t)w, in.y0 },                         // top
        { 0, in.y1, (int32_t)w, (int32_t)h },                // bottom
        { 0, in.y0, in.x0, in.y1 },                          // left
        { in.x1, in.y0, (int32_t)w, in.y1 } };               // right
    for (const Box& b : c) if (!Empty(b)) out[n++] = b;
    return n;
}
// The 4 OUTER bands (outside the rect itself) -- the in-place path copies these back next to the rect.
inline int OuterBands(uint32_t w, uint32_t h, const Box& rect, Box out[4]) { return Bands(w, h, rect, out); }

// ---- phase 93: the bands clipped to the VR visibility mask ----
// WHY (Fable audit 2026-11-09, ATS VR 72 Hz): the outside-AA bands cost 0.207 ms per eye (GpuPerf "outside-aa") and cover the whole
// picture outside the rect -- the lens corners included, which the headset never shows (XR_KHR_visibility_mask's HIDDEN mesh, the
// one phase 40 already draws into the scene depth). Pixels there are never seen, so not drawing outside AA over them changes
// nothing in the headset (they keep the raw / stretched frame, finite values).
// HOW: VisRows = per image row, a CONSERVATIVE visible x-span [lo, hi): the row's centre line is cut exactly against every hidden
// triangle (the covered intervals merged only where they really overlap -- a float gap between two triangles counts as visible),
// the span is the hull of what is left (min .. max visible x), widened by `margin` px on both sides and over +- margin rows (the
// compositor filters across a few pixels; nothing between two row centres is lost). ClipBox = the bounding box of the visible spans
// inside a box (empty = skip). SplitClip cuts each band into kClipStrips pieces along its long side first, so the curved lens edge
// trims each piece on its own. Everything is in the image's pixel grid (NDC y up -> row 0 at the top, as MapToNdc / the viewport).
constexpr int kClipStrips = 8;                   // pieces per band (<= 4 bands -> <= 32 boxes)
constexpr int kClipMarginPx = 8;                 // the "few px" of safety around the visible area
struct VisRows {
    uint32_t w = 0, h = 0;
    std::vector<int32_t> lo, hi;                 // per row; lo >= hi = no visible pixel in this row
    uint32_t visRows = 0;                        // rows with a visible span (log)
};
// The hidden triangles (3 * tris vertices, (x, y) NDC each) of the view -> the visible spans of a w x h image. False = no usable
// input (the caller clips nothing).
inline bool BuildVisRows(const float* tri, uint32_t tris, uint32_t w, uint32_t h, int margin, VisRows* out) {
    if (!out) return false;
    *out = VisRows();
    if (!tri || !tris || !w || !h || w > 65536 || h > 65536) return false;
    out->w = w; out->h = h;
    std::vector<int32_t> lo(h), hi(h);
    std::vector<std::pair<float, float>> iv;
    iv.reserve(tris);
    for (uint32_t r = 0; r < h; ++r) {
        const float y = 1.0f - 2.0f * ((float)r + 0.5f) / (float)h;
        iv.clear();
        for (uint32_t k = 0; k < tris; ++k) {
            const float* v = tri + 6 * k;
            if (y < (std::min)((std::min)(v[1], v[3]), v[5]) || y > (std::max)((std::max)(v[1], v[3]), v[5])) continue;
            float xmn = 1e30f, xmx = -1e30f;
            int hits = 0;
            for (int e = 0; e < 3; ++e) {
                // the edge's endpoints in a fixed order (by y, then x), so two triangles sharing it compute the same x
                float ax = v[2 * e], ay = v[2 * e + 1], bx = v[2 * ((e + 1) % 3)], by = v[2 * ((e + 1) % 3) + 1];
                if (by < ay || (by == ay && bx < ax)) { std::swap(ax, bx); std::swap(ay, by); }
                if (y < ay || y > by) continue;
                const float x = by > ay ? ax + (y - ay) * (bx - ax) / (by - ay) : ax;
                if (by == ay) { xmn = (std::min)(xmn, (std::min)(ax, bx)); xmx = (std::max)(xmx, (std::max)(ax, bx)); }
                else { xmn = (std::min)(xmn, x); xmx = (std::max)(xmx, x); }
                ++hits;
            }
            if (hits && xmx > xmn) iv.emplace_back(xmn, xmx);   // (a zero-width crossing hides nothing)
        }
        std::sort(iv.begin(), iv.end());
        // visible = [-1, 1] minus the union; the hull of the visible part: its first and last point
        float vmin = 2.0f, vmax = -2.0f;
        float cur = -1.0f;                               // everything left of `cur` is decided
        for (const auto& p : iv) {
            if (p.first > cur) {                         // a visible gap (cur, p.first)
                vmin = (std::min)(vmin, cur);
                vmax = (std::max)(vmax, (std::min)(p.first, 1.0f));
            }
            if (p.second > cur) cur = p.second;
            if (cur >= 1.0f) break;
        }
        if (cur < 1.0f) { vmin = (std::min)(vmin, cur); vmax = 1.0f; }
        if (vmax > vmin && vmax > -1.0f && vmin < 1.0f) {
            // NDC -> pixel columns: centre c + 0.5 = (x + 1) * w / 2
            const double fx0 = ((double)vmin + 1.0) * 0.5 * (double)w, fx1 = ((double)vmax + 1.0) * 0.5 * (double)w;
            lo[r] = (int32_t)std::floor(fx0) - margin;
            hi[r] = (int32_t)std::ceil(fx1) + margin;
        } else {
            lo[r] = INT32_MAX; hi[r] = INT32_MIN;
        }
    }
    // +- margin rows, then the image bounds
    out->lo.assign(h, INT32_MAX);
    out->hi.assign(h, INT32_MIN);
    for (uint32_t r = 0; r < h; ++r) {
        if (lo[r] >= hi[r]) continue;
        const int64_t a = (int64_t)r - margin, b = (int64_t)r + margin;
        for (int64_t q = a < 0 ? 0 : a; q <= b && q < (int64_t)h; ++q) {
            out->lo[(size_t)q] = (std::min)(out->lo[(size_t)q], lo[r]);
            out->hi[(size_t)q] = (std::max)(out->hi[(size_t)q], hi[r]);
        }
    }
    for (uint32_t r = 0; r < h; ++r) {
        int32_t& l = out->lo[r];
        int32_t& u = out->hi[r];
        if (l >= u) { l = 0; u = 0; continue; }
        if (l < 0) l = 0;
        if (u > (int32_t)w) u = (int32_t)w;
        if (l >= u) { l = 0; u = 0; continue; }
        ++out->visRows;
    }
    return true;
}
// The bounding box of the visible pixels of `b` (empty Box = none visible). No VisRows (w == 0): `b` itself.
inline Box ClipBox(const VisRows& v, const Box& b) {
    if (!v.w || !v.h || Empty(b)) return b;
    Box o{ INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
    const int32_t y0 = b.y0 < 0 ? 0 : b.y0, y1 = b.y1 > (int32_t)v.h ? (int32_t)v.h : b.y1;
    for (int32_t y = y0; y < y1; ++y) {
        const int32_t l = (std::max)(v.lo[(size_t)y], b.x0), u = (std::min)(v.hi[(size_t)y], b.x1);
        if (l >= u) continue;
        if (y < o.y0) o.y0 = y;
        o.y1 = y + 1;
        if (l < o.x0) o.x0 = l;
        if (u > o.x1) o.x1 = u;
    }
    if (o.x0 >= o.x1 || o.y0 >= o.y1) return Box{ b.x0, b.y0, b.x0, b.y0 };
    return o;
}
// Bands -> up to 4 * kClipStrips clipped boxes (empty ones left out). No VisRows: the bands as they are. *area / *clipped = the
// pixels of the bands / of the boxes returned (the log's skipped share).
inline int SplitClip(const VisRows& v, const Box* bands, int n, Box* out, uint64_t* area, uint64_t* clipped) {
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const Box& b = bands[i];
        if (Empty(b)) continue;
        if (area) *area += (uint64_t)(b.x1 - b.x0) * (uint64_t)(b.y1 - b.y0);
        if (!v.w) { out[m++] = b; if (clipped) *clipped += (uint64_t)(b.x1 - b.x0) * (uint64_t)(b.y1 - b.y0); continue; }
        const bool wide = (b.x1 - b.x0) >= (b.y1 - b.y0);
        const int32_t len = wide ? b.x1 - b.x0 : b.y1 - b.y0;
        for (int s = 0; s < kClipStrips; ++s) {
            const int32_t a0 = (int32_t)((int64_t)len * s / kClipStrips), a1 = (int32_t)((int64_t)len * (s + 1) / kClipStrips);
            if (a1 <= a0) continue;
            const Box piece = wide ? Box{ b.x0 + a0, b.y0, b.x0 + a1, b.y1 } : Box{ b.x0, b.y0 + a0, b.x1, b.y0 + a1 };
            const Box c = ClipBox(v, piece);
            if (Empty(c)) continue;
            out[m++] = c;
            if (clipped) *clipped += (uint64_t)(c.x1 - c.x0) * (uint64_t)(c.y1 - c.y0);
        }
    }
    return m;
}
// The union bounding box of boxes (the in-place CS dispatch covers it once; empty if n == 0).
inline Box UnionBox(const Box* b, int n) {
    Box o{ INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
    for (int i = 0; i < n; ++i) {
        if (Empty(b[i])) continue;
        o.x0 = (std::min)(o.x0, b[i].x0); o.y0 = (std::min)(o.y0, b[i].y0);
        o.x1 = (std::max)(o.x1, b[i].x1); o.y1 = (std::max)(o.y1, b[i].y1);
    }
    if (o.x0 >= o.x1 || o.y0 >= o.y1) return Box{ 0, 0, 0, 0 };
    return o;
}

// The viewport shift a pass was rasterised with, from the jitter NGX was told (inject.cpp: NGX jitter = jitter_sign * shift).
inline float ShiftFromNgx(float ngx, int sign) { return sign < 0 ? -ngx : ngx; }

// Catmull-Rom weights of the 4 taps around a sample at fraction f (C++ twin of the shader; the test checks them).
inline void CatmullRomW(float f, float w[4]) {
    w[0] = f * (-0.5f + f * (1.0f - 0.5f * f));
    w[1] = 1.0f + f * f * (-2.5f + 1.5f * f);
    w[2] = f * (0.5f + f * (2.0f - 1.5f * f));
    w[3] = f * f * (-0.5f + 0.5f * f);
}

// Params[] of the shader (Buffer<float4> at t1), 5 x float4:
//  [0] the interior box (output px; CSMain leaves those pixels alone)
//  [1] uv map: 1 / output size (x, y), shift / render size (z, w) -> uv = o_c * xy + zw
//  [2] render texel size (1 / render size) xy, render size zw
//  [3] x = mode (1 unjitter, 2 + edge AA), y = luma (0 encoded LDR, 1 linear LDR, 2 HDR), z = 1: re-encode linear -> sRGB on the
//      way out (CSMain only: an _SRGB-only colour texture read through its _SRGB view, written back as bytes), w unused
//  [4] xy = the output image's origin in the render target (PSMain: SV_Position - origin = output pixel), zw = phase 93: CSMain's
//      dispatch origin (image px; 0 = the dispatch covers the image from (0, 0)) -- set after FillParams by the in-place path
constexpr int kParamFloats = 20;
inline void FillParams(float p[kParamFloats], const Box& interior, uint32_t outW, uint32_t outH, uint32_t renderW, uint32_t renderH,
                       float shiftX, float shiftY, int mode, int luma, bool encode, float originX, float originY) {
    p[0] = (float)interior.x0; p[1] = (float)interior.y0; p[2] = (float)interior.x1; p[3] = (float)interior.y1;
    p[4] = outW ? 1.0f / (float)outW : 0.0f; p[5] = outH ? 1.0f / (float)outH : 0.0f;
    p[6] = renderW ? shiftX / (float)renderW : 0.0f; p[7] = renderH ? shiftY / (float)renderH : 0.0f;
    p[8] = renderW ? 1.0f / (float)renderW : 0.0f; p[9] = renderH ? 1.0f / (float)renderH : 0.0f;
    p[10] = (float)renderW; p[11] = (float)renderH;
    p[12] = (float)mode; p[13] = (float)luma; p[14] = encode ? 1.0f : 0.0f; p[15] = 0.0f;
    p[16] = originX; p[17] = originY; p[18] = 0.0f; p[19] = 0.0f;
}

// The shader. One block, two entries: CSMain (in place: Src = the game's colour, Dst = the full-size scratch, s0 = linear clamp)
// and PSMain (upscale: Src = the game's blit SRV0, drawn per band with the composite's VS). Dst (u0) is not referenced by PSMain,
// so ps_5_0 does not see it.
// kOutsideAaShader-BEGIN (the build validates this block with fxc: cs_5_0 / CSMain and ps_5_0 / PSMain)
inline constexpr char kOutsideAaShader[] = R"(
Texture2D<float4>   Src    : register(t0);
Buffer<float4>      Params : register(t1);
RWTexture2D<float4> Dst    : register(u0);
SamplerState        Lin    : register(s0);

static const float kEdgeRel   = 0.125;     // FXAA 3.11 console defaults: relative / absolute contrast threshold, edge sharpness
static const float kEdgeMin   = 0.05;
static const float kEdgeSharp = 8.0;

float3 Tap(float2 uv) { return Src.SampleLevel(Lin, uv, 0.0).rgb; }

float LumaOf(float3 c, float lm) {
    float l = max(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.0);   // RCAS's luma
    if (lm > 1.5) l = l / (1.0 + l);                           // HDR: reversible compression into 0..1
    return lm > 0.5 ? sqrt(l) : l;                             // linear (sRGB view / HDR): ~perceptual
}

// 5-tap Catmull-Rom at uv (render texture of `size`, texel = 1 / size), clamped into the 2x2 texels around it (no ringing).
float3 CatmullRom5(float2 uv, float2 size, float2 texel) {
    const float2 sp  = uv * size;
    const float2 t1  = floor(sp - 0.5) + 0.5;
    const float2 f   = sp - t1;
    const float2 w0  = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1  = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2  = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3  = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;                                // >= 1 for f in [0, 1]
    const float2 u0  = (t1 - 1.0) * texel;
    const float2 u12 = (t1 + w2 / w12) * texel;
    const float2 u3  = (t1 + 2.0) * texel;
    const float wa = w12.x * w0.y, wb = w0.x * w12.y, wc = w12.x * w12.y, wd = w3.x * w12.y, we = w12.x * w3.y;
    float3 c = Tap(float2(u12.x, u0.y)) * wa + Tap(float2(u0.x, u12.y)) * wb + Tap(u12) * wc +
               Tap(float2(u3.x, u12.y)) * wd + Tap(float2(u12.x, u3.y)) * we;
    c /= (wa + wb + wc + wd + we);
    const int2 i  = int2(t1 - 0.5);
    const int2 hi = int2(size) - 1;
    const float3 a = Src.Load(int3(clamp(i, int2(0, 0), hi), 0)).rgb;
    const float3 b = Src.Load(int3(clamp(i + int2(1, 0), int2(0, 0), hi), 0)).rgb;
    const float3 d = Src.Load(int3(clamp(i + int2(0, 1), int2(0, 0), hi), 0)).rgb;
    const float3 e = Src.Load(int3(clamp(i + int2(1, 1), int2(0, 0), hi), 0)).rgb;
    return clamp(c, min(min(a, b), min(d, e)), max(max(a, b), max(d, e)));
}

// FXAA 3.11 console on the unjittered grid (offsets in render texels around uv). cM = the Catmull-Rom value at uv.
float3 EdgeAa(float2 uv, float3 cM, float2 texel, float lm) {
    const float lNw = LumaOf(Tap(uv + float2(-0.5, -0.5) * texel), lm);
    const float lSw = LumaOf(Tap(uv + float2(-0.5,  0.5) * texel), lm);
    const float lNe = LumaOf(Tap(uv + float2( 0.5, -0.5) * texel), lm) + 1.0 / 384.0;
    const float lSe = LumaOf(Tap(uv + float2( 0.5,  0.5) * texel), lm);
    const float lM  = LumaOf(cM, lm);
    const float lMax = max(max(lNw, lSw), max(lNe, lSe));
    const float lMin = min(min(lNw, lSw), min(lNe, lSe));
    if (max(lMax, lM) - min(lMin, lM) < max(kEdgeMin, lMax * kEdgeRel)) return cM;
    const float dSwNe = lSw - lNe, dSeNw = lSe - lNw;
    const float2 dir = float2(dSwNe + dSeNw, dSwNe - dSeNw);
    const float dl = length(dir);
    if (dl < 1.0e-6) return cM;
    const float2 d1 = dir / dl;
    const float3 n1 = Tap(uv - d1 * 0.5 * texel);
    const float3 p1 = Tap(uv + d1 * 0.5 * texel);
    const float2 d2 = clamp(d1 / max(min(abs(d1.x), abs(d1.y)) * kEdgeSharp, 1.0e-4), -2.0, 2.0);
    const float3 n2 = Tap(uv - d2 * 2.0 * texel);
    const float3 p2 = Tap(uv + d2 * 2.0 * texel);
    const float3 a = n1 + p1;
    const float3 b = (n2 + p2) * 0.25 + a * 0.25;
    const float lB = LumaOf(b, lm);
    return (lB < lMin || lB > lMax) ? a * 0.5 : b;
}

float3 OutsideValue(float2 oc) {
    const float4 m = Params[1];
    const float4 r = Params[2];
    const float4 k = Params[3];
    const float2 uv = oc * m.xy + m.zw;
    float3 c = CatmullRom5(uv, r.zw, r.xy);
    if (k.x > 1.5) c = EdgeAa(uv, c, r.xy, k.y);
    return c;
}

float3 LinearToSrgb(float3 c) {
    c = saturate(c);
    return (c <= 0.0031308) ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    Dst.GetDimensions(w, h);
    const uint2 q = id.xy + (uint2)Params[4].zw;   // phase 93: the dispatch origin (the visible-area clip; 0 = the whole image)
    if (q.x >= w || q.y >= h) return;
    const float4 ib = Params[0];
    const float2 p  = float2(q);
    if (p.x >= ib.x && p.x < ib.z && p.y >= ib.y && p.y < ib.w) return;   // feather mask 1: DLAA only, nothing to do
    float3 c = OutsideValue(p + 0.5);
    if (Params[3].z > 0.5) c = LinearToSrgb(c);
    Dst[q] = float4(c, Src.Load(int3(q, 0)).a);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    return float4(OutsideValue(pos.xy - Params[4].xy), 1.0);
}
)";
// kOutsideAaShader-END

}  // namespace OutsideAa
