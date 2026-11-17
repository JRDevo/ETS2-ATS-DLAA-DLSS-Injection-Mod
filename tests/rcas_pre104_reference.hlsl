// Phase 104 test reference (tests/sharpen_nis_test.cpp): kRcasShader EXACTLY as it was before phase 104 (phase 101d,
// 12-float RcasCB), copied byte for byte from src/scene_dlaa.cpp. The test runs it next to the current shader: with the
// phase 104 fields at their defaults and Sharpness <= 1 both must give bit-identical pictures. Do not edit.

cbuffer RcasCB : register(b0) {
    float  Sharpness;       // 0..1 user strength (final lobe scale)
    float  Radius;          // v0.5.8 ring-tap radius in texels (1..4), bilinear
    float  Feather;         // v0.6.4 border blend width in px (0 = hard edge)
    float  Blend;           // v0.6.4 1 = blend into Orig at the rect border (area < 100), 0 = off
    float4 EdgeOn;          // v0.6.4 (left, top, right, bottom): 1 = that rect edge is inside the image
    float4 Mode;            // v0.8.0 x: 1 = HDR (linear RGBA16F, sharpen a compressed value), 0 = LDR; phase 81 yz: Orig offset;
                            // phase 101 w: 1 = NIS sharpen instead of RCAS
};
Texture2D<float4>   Src                : register(t0);
Texture2D<float4>   Orig               : register(t1);   // v0.6.4 colorIn (only read when Blend = 1); phase 81: or the scratch
RWTexture2D<float4> Dst                : register(u0);
SamplerState        LinearClampSampler : register(s0);

static const float kRcasLimit = 0.25 - (1.0 / 16.0);

float Luma(float3 c) { return 0.25 * c.r + 0.5 * c.g + 0.25 * c.b; }
float Max3(float3 c) { return max(c.r, max(c.g, c.b)); }
// v0.8.0 reversible tonemapper (HDR only): linear >= 0 -> [0, 1) and back (exact inverse for max3 >= 0).
float3 HdrCompress(float3 c) { return c / (1.0 + max(Max3(c), 0.0)); }
float3 HdrExpand(float3 c)   { return c / max(1.0 - max(Max3(c), 0.0), 1.0e-5); }

// phase 101 NIS SHARPEN (issue #34; dlaa.ini sharpen_type = nis, Mode.w = 1): the sharpen-only path of the NVIDIA Image Scaling
// SDK v1.0.3 (NIS_Scaler.h NVSharpen, MIT licence, see THIRD_PARTY.md), ported to this pass. A 5x5 luma window; a 3x3
// edge map picks the edge direction (0 / 90 / 45 / 135 deg), an unsharp mask runs ACROSS that edge only, its strength and limit
// follow the centre luma (less on bright pixels) and a contrast-ratio test damps it near one-sided edges (halo control). The
// result is added to all three channels. Sharpness 0..1 = the SDK's slider (the SDK constants of NVSharpenUpdateConfig, SDR).
// HDR units run it on the same reversibly compressed values as RCAS. Radius is not used (NIS has a fixed 5x5 footprint).
float NisY(float3 c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }
static const float kNisDetectRatio = 2.0 * 1127.0 / 1024.0;
static const float kNisDetectThres = 64.0 / 1024.0;
static const float kNisMinContrast = 2.0;
static const float kNisRatioNorm   = 1.0 / (10.0 - 2.0);   // 1 / (max - min contrast ratio)
static const float kNisSharpStartY = 0.45;
static const float kNisSharpScaleY = 1.0 / (0.9 - 0.45);   // 1 / (end - start)
static const float kNisEps         = 1.0 / 255.0;
float NisLti(float y0, float y1, float y2, float y3, float y4) {   // CalcLTIFast: 1 = full USM, 0 = none (one-sided edge)
    const float aCont = max(max(y0, y1), y2) - min(min(y0, y1), y2);
    const float bCont = max(max(y2, y3), y4) - min(min(y2, y3), y4);
    const float ratio = max(aCont, bCont) / (min(aCont, bCont) + kNisEps);
    return 1.0 - saturate((ratio - kNisMinContrast) * kNisRatioNorm);
}
float NisUsm(float y0, float y1, float y2, float y3, float y4, float strength, float limit) {   // EvalUSM
    float u = (-0.6001 * y1 + 1.2002 * y2 - 0.6001 * y3) * strength;
    u = clamp(u, -limit, limit);
    return u * NisLti(y0, y1, y2, y3, y4);
}
// p[y][x] = the luma at (x - 2, y - 2) around the pixel. Returns the luma change for the centre.
float NisSharpenY(float p[5][5], float sharpness) {
    // NVSharpenUpdateConfig (SDR): the slider 0..1 -> -0.5..+0.5
    const float s          = sharpness - 0.5;
    const float maxScale   = s >= 0.0 ? 1.25 : 1.75;
    const float minScale   = s >= 0.0 ? 1.25 : 1.0;
    const float limitScale = s >= 0.0 ? 1.25 : 1.0;
    const float strMin = max(0.0, 0.4 + s * minScale * 1.2);
    const float strMax = 1.6 + s * maxScale * 1.8;
    const float limMin = max(0.1, 0.14 + s * limitScale * 0.32);
    const float limMax = 0.5 + s * limitScale * 0.6;
    // GetDirUSM: strength / limit ramp with the centre luma
    const float scaleY   = 1.0 - saturate((p[2][2] - kNisSharpStartY) * kNisSharpScaleY);
    const float strength = scaleY * (strMax - strMin) + strMin;
    // phase 101c (audit): >= 0 -- an HDR centre can have a slightly negative compressed luma (DLSS output), and clamp(u, -l, l)
    // with l < 0 is undefined in HLSL (the SDK's min / max form happened to return l)
    const float limit    = max((scaleY * (limMax - limMin) + limMin) * p[2][2], 0.0);
    float4 usm;
    usm.x = NisUsm(p[0][2], p[1][2], p[2][2], p[3][2], p[4][2], strength, limit);                          // 0 deg (vertical profile)
    usm.y = NisUsm(p[2][0], p[2][1], p[2][2], p[2][3], p[2][4], strength, limit);                          // 90 deg
    usm.z = NisUsm(p[1][1], lerp(p[2][1], p[1][2], 0.5), p[2][2], lerp(p[3][2], p[2][3], 0.5), p[3][3], strength, limit);   // 45
    usm.w = NisUsm(p[3][1], lerp(p[3][2], p[2][1], 0.5), p[2][2], lerp(p[2][3], p[1][2], 0.5), p[1][3], strength, limit);   // 135
    // GetEdgeMap over the centre 3x3 (rows / cols 1..3)
    const float g0   = abs(p[1][1] + p[1][2] + p[1][3] - p[3][1] - p[3][2] - p[3][3]);
    const float g45  = abs(p[2][1] + p[1][1] + p[1][2] - p[3][2] - p[3][3] - p[2][3]);
    const float g90  = abs(p[1][1] + p[2][1] + p[3][1] - p[1][3] - p[2][3] - p[3][3]);
    const float g135 = abs(p[2][1] + p[3][1] + p[3][2] - p[1][2] - p[1][3] - p[2][3]);
    const float g090Max = max(g0, g90),  g090Min = min(g0, g90);
    const float g45Max  = max(g45, g135), g45Min = min(g45, g135);
    if (g090Max + g45Max <= 0.0) return 0.0;
    const float e090 = min(g090Max / (g090Max + g45Max), 1.0);
    const float e45  = 1.0 - e090;
    const bool  c090 = g090Max > g090Min * kNisDetectRatio && g090Max > kNisDetectThres && g090Max > g45Min;
    const bool  c45  = g45Max > g45Min * kNisDetectRatio && g45Max > kNisDetectThres && g45Max > g090Min;
    const bool  is0  = g090Max == g0;
    const bool  is45 = g45Max == g45;
    const float f090 = (c090 && c45) ? e090 : 1.0;
    const float f45  = (c090 && c45) ? e45 : 1.0;
    const float4 wgt = float4((c090 && is0) ? f090 : 0.0, (c090 && !is0) ? f090 : 0.0,
                              (c45 && is45) ? f45 : 0.0, (c45 && !is45) ? f45 : 0.0);
    return dot(usm, wgt);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    Src.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    const int2   p    = int2(id.xy);
    const float2 size = float2(w, h);
    const float2 uv   = (float2(id.xy) + 0.5) / size;
    const bool   hdr  = Mode.x > 0.5;
    //    b
    //  d e f
    //    g
    const float4 e0 = Src.Load(int3(p, 0));
    float3 res;
    // phase 57 (perf): LDR unit at Sharpness 0 (the pass runs only for the border blend): the RCAS lobe is exactly 0,
    // so c = e and the result is saturate(centre) -- skip the 4 ring taps + the limiter maths. Shader-uniform branch
    // (constant buffer). Not taken for HDR: the compress / expand round trip is not bit-exact, so HDR keeps the full path.
    if (!hdr && Sharpness <= 0.0) {
        res = saturate(e0.rgb);
    } else if (Mode.w > 0.5) {                          // phase 101: NIS sharpen (shader-uniform branch, constant buffer)
        if (Sharpness <= 0.0) {
            res = e0.rgb;                               // HDR at 0 (border blend only): unchanged
        } else {
            float ny[5][5];
            float3 mn5 = 1.0e30, mx5 = -1.0e30;           // HDR: the range of the centre + its 4 cross neighbours
            [unroll] for (int y = 0; y < 5; ++y) {
                [unroll] for (int x = 0; x < 5; ++x) {
                    const int2 q = clamp(p + int2(x - 2, y - 2), int2(0, 0), int2((int)w - 1, (int)h - 1));
                    float3 t = Src.Load(int3(q, 0)).rgb;
                    if (hdr) t = HdrCompress(t);
                    ny[y][x] = NisY(t);
                    if ((x == 2 && y >= 1 && y <= 3) || (y == 2 && x >= 1 && x <= 3)) { mn5 = min(mn5, t); mx5 = max(mx5, t); }
                }
            }
            const float3 e = hdr ? HdrCompress(e0.rgb) : e0.rgb;
            float d = NisSharpenY(ny, Sharpness);
            // phase 101c (audit): an Inf / NaN texel (FP16 specular overflow) makes d NaN for every pixel whose 5x5 window holds it
            // (NaN * weight 0 is still NaN) -- a black (LDR) or dark (HDR) 5x5 square. No sharpening there instead.
            if (!(abs(d) < 1.0e30)) d = 0.0;
            // phase 101b (Fable review): HDR anti-overshoot. The expand c / (1 - max3) is very steep near 1, so an unclamped delta on
            // a thin bright feature (lamp, chrome glint at compressed 0.95) became a 26x firefly. Keep the result inside the 5-tap
            // range like RCAS does: max3 stays below 1 (finite expand), no fixed cap (highlights above linear 499 are kept).
            // LDR keeps the SDK behaviour (saturate only).
            res = hdr ? min(HdrExpand(clamp(e + d, mn5, mx5)), 65504.0) : saturate(e + d);
        }
    } else {
        float3 b = Src.SampleLevel(LinearClampSampler, uv + float2(0.0, -Radius) / size, 0).rgb;
        float3 d = Src.SampleLevel(LinearClampSampler, uv + float2(-Radius, 0.0) / size, 0).rgb;
        float3 f = Src.SampleLevel(LinearClampSampler, uv + float2( Radius, 0.0) / size, 0).rgb;
        float3 g = Src.SampleLevel(LinearClampSampler, uv + float2(0.0,  Radius) / size, 0).rgb;
        float4 e = e0;
        if (hdr) {
            e.rgb = HdrCompress(e0.rgb);
            b = HdrCompress(b); d = HdrCompress(d); f = HdrCompress(f); g = HdrCompress(g);
        }
        const float bL = Luma(b), dL = Luma(d), fL = Luma(f), gL = Luma(g);
        const float mn4 = min(min(bL, dL), min(fL, gL));
        const float mx4 = max(max(bL, dL), max(fL, gL));
        // RCAS limiters: the largest negative lobe that keeps the result inside [0,1] given the ring range.
        const float hitMin = mn4 / max(4.0 * mx4, 1.0e-5);
        const float hitMax = (1.0 - mx4) / min(4.0 * mn4 - 4.0, -1.0e-5);
        float lobe = max(-hitMin, hitMax);
        lobe = max(-kRcasLimit, min(lobe, 0.0)) * Sharpness;
        float3 c = (lobe * (b + d + f + g) + e.rgb) / (4.0 * lobe + 1.0);
        // v0.5.9 anti-ringing: keep the result inside the range of the 5 taps. With a wide Radius a bright
        // lamp in one ring tap pushed the dark pixels around it below zero (black, flickering halos).
        const float3 mn5 = min(e.rgb, min(min(b, d), min(f, g)));
        const float3 mx5 = max(e.rgb, max(max(b, d), max(f, g)));
        c = clamp(c, mn5, mx5);
        res = hdr ? min(HdrExpand(c), 65504.0) : saturate(c);   // v0.8.0 HDR: back to linear, no 0..1 clamp
    }
    if (Blend > 0.5) {
        float mask = 1.0;
        if (Feather > 0.0) {
            const float2 lo = float2(p) + 0.5;           // distance to the left / top rect edge (px)
            const float2 hi = size - lo;                 // ... to the right / bottom rect edge
            const float rx = min(EdgeOn.x > 0.5 ? saturate(lo.x / Feather) : 1.0,
                                 EdgeOn.z > 0.5 ? saturate(hi.x / Feather) : 1.0);
            const float ry = min(EdgeOn.y > 0.5 ? saturate(lo.y / Feather) : 1.0,
                                 EdgeOn.w > 0.5 ? saturate(hi.y / Feather) : 1.0);
            mask = smoothstep(0.0, 1.0, rx) * smoothstep(0.0, 1.0, ry);
        }
        // phase 57 (perf): the mask is exactly 1 everywhere except the Feather band at the rect border -> read Orig
        // (a full-res load per pixel) only there; lerp(Orig, res, 1) is res.
        if (mask < 1.0) res = lerp(Orig.Load(int3(p + int2(Mode.yz), 0)).rgb, res, mask);
    }
    Dst[id.xy] = float4(res, e0.a);
}
