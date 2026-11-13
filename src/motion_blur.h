// MotionBlur -- phase 100 (issue #33): per-object motion blur, FLAT ONLY (dlaa.ini motion_blur, menu row "Motion blur" in Motion and
// mirrors; 0 = off, the default). Pure maths + the HLSL block; scene_dlaa.cpp runs it, tests/motion_blur_test.cpp (WARP) checks it.
//
// WHY: a player asked for object motion blur as a look (issue #33). The mod already has what it needs: per-object motion vectors
// (mv_objects = 2, phase v0.10.0) for every DLAA unit, in RENDER pixels, NGX convention. Blur needs no new data from the game.
// WHERE: SceneDlaa::Run, right after the RCAS sharpen and before the copy-back / composite: the AA'd (+ sharpened) result is smeared
// along each pixel's own motion vector into a scratch of the same size and format, and the scratch goes back instead. The game's UI
// is drawn later, so it never blurs. Only the main view's units get it (inject.cpp SetMotionBlurOk: flat, not VR, not the mirrors,
// not the menu / truck preview). In VR it never runs: blur in a headset makes many players sick.
// HOW (per output pixel o, the CS below):
//   v = mv(o) * scale * shutter (output px; scale = output / render, 1 without DLSS upscaling; shutter = motion_blur / 100). The
//   blur is CENTRED on o: taps at t in (-0.5, 0.5) of v, |v| clamped to MaxLen. Below MinLen the pixel is copied as it is (still
//   picture, the own truck: it moves with the camera, so its vectors are ~0 and it stays sharp).
//   SPEED WEIGHT: a tap whose own vector is much SLOWER than o's is left out (weight 0 at <= 25 % of o's speed, 1 at >= 75 %). The
//   world seen through the windscreen therefore never smears the static A-pillar / dashboard / mirror housings into itself.
//   Taps are POINT reads at WHOLE-PIXEL offsets (round(v * t)), at the same texel the speed weight judged: a bilinear tap at an
//   edge would mix in the static neighbour, and a per-pixel random tap offset loses brightness (measured: a 4 px stripe smeared
//   8 px kept only 85 % of its light). Whole-pixel taps are pure shifts of the picture, so a uniform smear keeps all of it.
//   SHOW (dlaa.ini motion_blur_show / the menu's "+ pink" step): blurred pixels are tinted pink, stronger for a longer smear --
//   a test view so the player can see WHERE the blur works (still things keep their colour).
//   n = clamp(ceil(|v|), 3, MaxTaps) taps: up to MaxTaps px every pixel of the smear is read; beyond, the taps spread out.
// COST: one CS over the result; still pixels return after 2 loads; moving pixels n taps of 2 loads each. GpuPerf section
// "motion-blur", menu cost group MG_MBLUR.
#pragma once
#include <cmath>
#include <cstdint>

namespace MotionBlur {

constexpr int   kStepPct  = 25;                  // menu / dlaa.ini steps: 0, 25, ..., 200 (phase 100b: up to 200 %)
constexpr int   kMaxPct   = 200;                 // 100 % = one frame of motion (a film's 180-degree shutter is 50 %)
constexpr float kMinLenPx = 1.0f;                // below this (output px, after the shutter) a pixel is not blurred
constexpr float kMaxLenFrac = 0.06f;             // the longest smear: 6 % of the output width (phase 100b: was 4 %) ...
constexpr float kMaxLenMinPx = 16.0f;            // ... but at least 16 px
constexpr int   kMaxTaps  = 48;                  // (phase 100b: was 32, for the 200 % smear)

// dlaa.ini / menu value -> a valid step (0..kMaxPct, multiple of kStepPct)
inline int ClampPct(int v) {
    if (v <= 0) return 0;
    if (v >= kMaxPct) return kMaxPct;
    return ((v + kStepPct / 2) / kStepPct) * kStepPct;
}
inline float MaxLen(uint32_t outW) {
    const float m = (float)outW * kMaxLenFrac;
    return m < kMaxLenMinPx ? kMaxLenMinPx : m;
}
// taps for a smear of `len` output px (C++ twin of the shader)
inline int Taps(float len) {
    float n = std::ceil(len);
    if (n < 3.0f) n = 3.0f;
    if (n > (float)kMaxTaps) n = (float)kMaxTaps;
    return (int)n;
}
// weight of a tap moving at spS when the pixel moves at sp (> 0): 0 at <= 25 % of sp, 1 at >= 75 %
inline float SpeedWeight(float spS, float sp) {
    const float r = sp > 1e-4f ? spS / sp : 0.0f;
    float w = (r - 0.25f) * 2.0f;
    return w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
}

// The constant buffer (b0), 3 x float4:
//  [0] output size (x, y), motion-vector texture size (z, w)
//  [1] scale output / render (x, y), shutter 0..1 (z), MaxLen output px (w)
//  [2] MinLen output px (x), MaxTaps (y), z = 1: SHOW (phase 100b test view: blurred pixels tinted pink by smear length), w unused
constexpr int kCbFloats = 12;
constexpr float kShowFullPx = 24.0f;             // SHOW: full pink at a smear of this many output px
inline void FillCb(float p[kCbFloats], uint32_t outW, uint32_t outH, uint32_t mvW, uint32_t mvH, int pct, bool show = false) {
    p[0] = (float)outW; p[1] = (float)outH; p[2] = (float)mvW; p[3] = (float)mvH;
    p[4] = mvW ? (float)outW / (float)mvW : 1.0f; p[5] = mvH ? (float)outH / (float)mvH : 1.0f;
    p[6] = (float)ClampPct(pct) / 100.0f; p[7] = MaxLen(outW);
    p[8] = kMinLenPx; p[9] = (float)kMaxTaps; p[10] = show ? 1.0f : 0.0f; p[11] = 0.0f;
}

// kMotionBlurShader-BEGIN (the build validates this block with fxc: cs_5_0 / CSMain)
inline constexpr char kMotionBlurShader[] = R"(
cbuffer P : register(b0) { float4 Sz; float4 Sc; float4 Lm; };
Texture2D<float4>   Src : register(t0);     // the DLAA result (output size)
Texture2D<float2>   Mv  : register(t1);     // motion vectors (render size, render px)
RWTexture2D<float4> Dst : register(u0);

float2 MvAt(float2 uv) {                     // output px
    const int2 p = clamp(int2(uv * Sz.zw), int2(0, 0), int2(Sz.zw) - 1);
    return Mv.Load(int3(p, 0)) * Sc.xy;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)Sz.x || id.y >= (uint)Sz.y) return;
    const float2 inv = 1.0 / Sz.xy;
    const float2 uv  = (float2(id.xy) + 0.5) * inv;
    const float4 c0  = Src.Load(int3(id.xy, 0));
    const float2 mv  = MvAt(uv);
    const float  sp  = length(mv);
    float2 v   = mv * Sc.z;
    float  len = sp * Sc.z;
    if (!(len >= Lm.x)) { Dst[id.xy] = c0; return; }          // still (or NaN): as it is
    if (len > Sc.w) { v *= Sc.w / len; len = Sc.w; }
    const int  n  = (int)clamp(ceil(len), 3.0, Lm.y);
    const int2 hi = int2(Sz.xy) - 1;
    float3 acc = c0.rgb;
    float  ws  = 1.0;
    [loop] for (int i = 0; i < n; ++i) {
        const float t = ((float)i + 0.5) / (float)n - 0.5;
        const int2  q = clamp(int2(id.xy) + int2(floor(v * t + 0.5)), int2(0, 0), hi);   // whole-pixel shift
        const float r = length(MvAt((float2(q) + 0.5) * inv)) / sp;
        const float w = saturate((r - 0.25) * 2.0);
        acc += Src.Load(int3(q, 0)).rgb * w;
        ws  += w;
    }
    float3 c = acc / ws;
    if (Lm.z > 0.5) {                                          // SHOW (test view): pink by smear length, full at 24 px
        const float l = max(dot(c, float3(0.25, 0.5, 0.25)), 0.05);
        c = lerp(c, float3(1.0, 0.0, 1.0) * (l * 1.5 + 0.1), saturate(len / 24.0) * 0.85);
    }
    Dst[id.xy] = float4(c, c0.a);
}
)";
// kMotionBlurShader-END

}  // namespace MotionBlur
