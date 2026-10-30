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
#include <cmath>
#include <cstdint>

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
//  [4] xy = the output image's origin in the render target (PSMain: SV_Position - origin = output pixel), zw unused
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
    if (id.x >= w || id.y >= h) return;
    const float4 ib = Params[0];
    const float2 p  = float2(id.xy);
    if (p.x >= ib.x && p.x < ib.z && p.y >= ib.y && p.y < ib.w) return;   // feather mask 1: DLAA only, nothing to do
    float3 c = OutsideValue(p + 0.5);
    if (Params[3].z > 0.5) c = LinearToSrgb(c);
    Dst[id.xy] = float4(c, Src.Load(int3(id.xy, 0)).a);
}

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    return float4(OutsideValue(pos.xy - Params[4].xy), 1.0);
}
)";
// kOutsideAaShader-END

}  // namespace OutsideAa
