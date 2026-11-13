// phase 100 unit test (WARP, no GPU needed): flat motion blur (src/motion_blur.h, scene_dlaa.cpp MbRun, dlaa.ini motion_blur).
//
// Part A (maths, no D3D): ClampPct (the 25 % grid), MaxLen, Taps, SpeedWeight, FillCb.
// Part B (WARP, the real HLSL of MotionBlur::kMotionBlurShader): a 64 x 32 picture with a white vertical stripe on black,
//   1. still (all vectors 0): the output is the input, bit for bit;
//   2. moving 8 px / frame at 100 %: the stripe is smeared sideways, the picture's sum is kept, far pixels stay black;
//   3. the SPEED WEIGHT: a static white right half next to a moving black left half -- no white bleeds into the moving side
//      (the windscreen frame / dashboard case), the static side is untouched;
//   4. DLSS upscale: half-size vectors of half the length give the same picture as case 2 (the output / render scale);
//   5. huge vectors are clamped to MaxLen (a pixel MaxLen + 2 px from the stripe stays black), NaN vectors leave the pixel as is;
//   6. motion_blur 50 %: the smear is half as wide as at 100 %.
// Build: tests\build_motion_blur_test.bat   Run: build\tests\motion_blur_test.exe   (exit code 0 = all checks passed)
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include "motion_blur.h"

using Microsoft::WRL::ComPtr;

static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const char* what) {
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL: %s\n", what); }
}

struct Gpu { ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; ComPtr<ID3D11ComputeShader> cs;
             ComPtr<ID3D11SamplerState> lin; ComPtr<ID3D11Buffer> cb; };

static ComPtr<ID3D11Texture2D> Tex(Gpu& g, UINT w, UINT h, DXGI_FORMAT f, const void* data, UINT pitch, UINT bind) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA sd{ data, pitch, 0 };
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(g.dev->CreateTexture2D(&d, data ? &sd : nullptr, &t))) return nullptr;
    return t;
}

// one dispatch: src (outW x outH RGBA32F), mv (mvW x mvH RG32F, render px) at `pct` -> the output pixels
static std::vector<float> Run(Gpu& g, const std::vector<float>& src, uint32_t outW, uint32_t outH, const std::vector<float>& mv,
                              uint32_t mvW, uint32_t mvH, int pct, bool show = false) {
    std::vector<float> out;
    ComPtr<ID3D11Texture2D> ts = Tex(g, outW, outH, DXGI_FORMAT_R32G32B32A32_FLOAT, src.data(), outW * 16, D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11Texture2D> tm = Tex(g, mvW, mvH, DXGI_FORMAT_R32G32_FLOAT, mv.data(), mvW * 8, D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11Texture2D> td = Tex(g, outW, outH, DXGI_FORMAT_R32G32B32A32_FLOAT, nullptr, 0, D3D11_BIND_UNORDERED_ACCESS);
    if (!ts || !tm || !td) { Check(false, "textures"); return out; }
    ComPtr<ID3D11ShaderResourceView> ss, sm;
    ComPtr<ID3D11UnorderedAccessView> ud;
    g.dev->CreateShaderResourceView(ts.Get(), nullptr, &ss);
    g.dev->CreateShaderResourceView(tm.Get(), nullptr, &sm);
    g.dev->CreateUnorderedAccessView(td.Get(), nullptr, &ud);
    float cb[MotionBlur::kCbFloats];
    MotionBlur::FillCb(cb, outW, outH, mvW, mvH, pct, show);
    g.ctx->UpdateSubresource(g.cb.Get(), 0, nullptr, cb, 0, 0);
    ID3D11ShaderResourceView* srv[2] = { ss.Get(), sm.Get() };
    ID3D11UnorderedAccessView* uav = ud.Get();
    ID3D11Buffer* b = g.cb.Get();
    ID3D11SamplerState* s = g.lin.Get();
    g.ctx->CSSetShader(g.cs.Get(), nullptr, 0);
    g.ctx->CSSetShaderResources(0, 2, srv);
    g.ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    g.ctx->CSSetConstantBuffers(0, 1, &b);
    g.ctx->CSSetSamplers(0, 1, &s);
    g.ctx->Dispatch((outW + 7) / 8, (outH + 7) / 8, 1);
    ID3D11UnorderedAccessView* nu = nullptr;
    g.ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
    D3D11_TEXTURE2D_DESC d{};
    td->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> st;
    g.dev->CreateTexture2D(&d, nullptr, &st);
    g.ctx->CopyResource(st.Get(), td.Get());
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &mp))) { Check(false, "map"); return out; }
    out.resize((size_t)outW * outH * 4);
    for (uint32_t y = 0; y < outH; ++y) std::memcpy(&out[(size_t)y * outW * 4], (const char*)mp.pData + (size_t)y * mp.RowPitch, outW * 16);
    g.ctx->Unmap(st.Get(), 0);
    return out;
}

constexpr uint32_t W = 64, H = 32;
static float R(const std::vector<float>& p, uint32_t x, uint32_t y) { return p[((size_t)y * W + x) * 4]; }
static std::vector<float> Stripe(uint32_t x0, uint32_t x1) {      // white columns [x0, x1) on black, alpha 1
    std::vector<float> p((size_t)W * H * 4, 0.0f);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            float* q = &p[((size_t)y * W + x) * 4];
            const float v = (x >= x0 && x < x1) ? 1.0f : 0.0f;
            q[0] = q[1] = q[2] = v; q[3] = 1.0f;
        }
    return p;
}
static std::vector<float> Mv(uint32_t w, uint32_t h, float mx, float my) {
    std::vector<float> m((size_t)w * h * 2);
    for (size_t i = 0; i < (size_t)w * h; ++i) { m[2 * i] = mx; m[2 * i + 1] = my; }
    return m;
}
static double RowSum(const std::vector<float>& p, uint32_t y) { double s = 0; for (uint32_t x = 0; x < W; ++x) s += R(p, x, y); return s; }
static int LitCols(const std::vector<float>& p, uint32_t y) { int n = 0; for (uint32_t x = 0; x < W; ++x) n += R(p, x, y) > 0.01f; return n; }

int main() {
    // ---- Part A: maths ----
    Check(MotionBlur::ClampPct(-5) == 0 && MotionBlur::ClampPct(0) == 0 && MotionBlur::ClampPct(10) == 0, "ClampPct low");
    Check(MotionBlur::ClampPct(13) == 25 && MotionBlur::ClampPct(30) == 25 && MotionBlur::ClampPct(40) == 50, "ClampPct grid");
    Check(MotionBlur::ClampPct(100) == 100 && MotionBlur::ClampPct(130) == 125 && MotionBlur::ClampPct(75) == 75, "ClampPct high");
    Check(MotionBlur::ClampPct(200) == 200 && MotionBlur::ClampPct(260) == 200, "ClampPct 200 % top (phase 100b)");
    Check(MotionBlur::MaxLen(64) == 16.0f && std::fabs(MotionBlur::MaxLen(2560) - 153.6f) < 1e-3f, "MaxLen");
    Check(MotionBlur::Taps(1.0f) == 3 && MotionBlur::Taps(10.0f) == 10 && MotionBlur::Taps(500.0f) == MotionBlur::kMaxTaps, "Taps");
    Check(MotionBlur::SpeedWeight(0.0f, 8.0f) == 0.0f && MotionBlur::SpeedWeight(2.0f, 8.0f) == 0.0f, "SpeedWeight slow = 0");
    Check(MotionBlur::SpeedWeight(6.0f, 8.0f) == 1.0f && MotionBlur::SpeedWeight(16.0f, 8.0f) == 1.0f, "SpeedWeight fast = 1");
    Check(std::fabs(MotionBlur::SpeedWeight(4.0f, 8.0f) - 0.5f) < 1e-6f, "SpeedWeight half");
    {
        float p[MotionBlur::kCbFloats];
        MotionBlur::FillCb(p, 3840, 2160, 1920, 1080, 50);
        Check(p[0] == 3840 && p[2] == 1920 && p[4] == 2.0f && p[5] == 2.0f && p[6] == 0.5f && p[8] == MotionBlur::kMinLenPx && p[10] == 0.0f, "FillCb");
        MotionBlur::FillCb(p, 3840, 2160, 3840, 2160, 200, true);
        Check(p[6] == 2.0f && p[10] == 1.0f, "FillCb 200 % + show");
    }

    // ---- Part B: the shader on WARP ----
    Gpu g;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx))) {
        std::printf("FAIL: no WARP device\n");
        return 1;
    }
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3DCompile(MotionBlur::kMotionBlurShader, std::strlen(MotionBlur::kMotionBlurShader), "motion_blur_test", nullptr,
                          nullptr, "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err))) {
        std::printf("FAIL: compile: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return 1;
    }
    g.dev->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g.cs);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g.dev->CreateSamplerState(&sd, &g.lin);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = MotionBlur::kCbFloats * 4; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g.dev->CreateBuffer(&bd, nullptr, &g.cb);
    if (!g.cs || !g.lin || !g.cb) { std::printf("FAIL: D3D objects\n"); return 1; }

    const std::vector<float> stripe = Stripe(30, 34);     // 4 px wide
    // 1. still
    {
        const std::vector<float> o = Run(g, stripe, W, H, Mv(W, H, 0, 0), W, H, 100);
        Check(o.size() == stripe.size() && std::memcmp(o.data(), stripe.data(), o.size() * 4) == 0, "1. still picture unchanged");
    }
    // 2. moving 8 px at 100 %
    std::vector<float> o2;
    {
        o2 = Run(g, stripe, W, H, Mv(W, H, 8, 0), W, H, 100);
        const uint32_t y = H / 2;
        std::printf("case 2 row:");
        for (uint32_t x = 22; x < 42; ++x) std::printf(" %.2f", R(o2, x, y));
        std::printf("  sum %.3f\n", RowSum(o2, y));
        Check(std::fabs(RowSum(o2, y) - 4.0) < 0.02, "2. smear keeps the row's brightness (4 px of white)");
        Check(LitCols(o2, y) >= 9 && LitCols(o2, y) <= 13, "2. stripe smeared to about 4 + 8 px");
        Check(R(o2, 31, y) < 0.95f && R(o2, 31, y) > 0.2f, "2. stripe centre dimmed");
        Check(R(o2, 10, y) == 0.0f && R(o2, 55, y) == 0.0f, "2. far pixels stay black");
        Check(R(o2, 31, 0) == R(o2, 31, y), "2. vertical stripe smears the same on every row");
    }
    // 3. speed weight: left half moving (black), right half static (white)
    {
        const std::vector<float> half = Stripe(32, W);
        std::vector<float> mv = Mv(W, H, 0, 0);
        for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < 32; ++x) mv[2 * ((size_t)y * W + x)] = 12.0f;
        const std::vector<float> o = Run(g, half, W, H, mv, W, H, 100);
        bool noBleed = true, rightSame = true;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                if (x < 32 && R(o, x, y) != 0.0f) noBleed = false;
                if (x >= 32 && R(o, x, y) != 1.0f) rightSame = false;
            }
        Check(noBleed, "3. no static white bleeds into the moving side");
        Check(rightSame, "3. the static side is untouched");
    }
    // 4. DLSS upscale: vectors at half size and half length (render px) = case 2
    {
        const std::vector<float> o = Run(g, stripe, W, H, Mv(W / 2, H / 2, 4, 0), W / 2, H / 2, 100);
        float md = 0.0f;
        for (size_t i = 0; i < o.size() && i < o2.size(); ++i) md = (std::max)(md, std::fabs(o[i] - o2[i]));
        Check(o.size() == o2.size() && md < 1e-5f, "4. upscale scale (output / render) gives the native picture");
    }
    // 5. clamp + NaN
    {
        const std::vector<float> o = Run(g, stripe, W, H, Mv(W, H, 1000, 0), W, H, 100);
        // MaxLen(64) = 16 px centred: at most 8 px each side of the stripe (+1 for the bilinear tap)
        Check(R(o, 30 - 10, H / 2) == 0.0f && R(o, 34 + 10, H / 2) == 0.0f, "5. huge vectors clamped to MaxLen");
        bool finite = true;
        for (float v : o) finite = finite && std::isfinite(v);
        Check(finite, "5. huge vectors: finite output");
        const float qn = std::numeric_limits<float>::quiet_NaN();
        const std::vector<float> on = Run(g, stripe, W, H, Mv(W, H, qn, qn), W, H, 100);
        Check(on.size() == stripe.size() && std::memcmp(on.data(), stripe.data(), on.size() * 4) == 0, "5. NaN vectors: pixel as is");
    }
    // 6. 50 %: half the smear
    {
        const std::vector<float> o = Run(g, stripe, W, H, Mv(W, H, 8, 0), W, H, 50);
        const uint32_t y = H / 2;
        Check(LitCols(o, y) >= 5 && LitCols(o, y) < LitCols(o2, y), "6. 50 % smears less than 100 %");
        Check(std::fabs(RowSum(o, y) - 4.0) < 0.02, "6. 50 % keeps the row's brightness");
    }
    // 7. phase 100b: 200 % smears more than 100 %; SHOW tints blurred pixels pink, still pixels keep their colour
    {
        const std::vector<float> o = Run(g, stripe, W, H, Mv(W, H, 8, 0), W, H, 200);
        Check(LitCols(o, H / 2) > LitCols(o2, H / 2) && std::fabs(RowSum(o, H / 2) - 4.0) < 0.02, "7. 200 % smears more, keeps the light");
        const std::vector<float> s = Run(g, stripe, W, H, Mv(W, H, 16, 0), W, H, 200, true);
        const float* px = &s[((size_t)(H / 2) * W + 10) * 4];      // a black pixel far from the stripe, moving 32 px
        Check(px[0] > 0.05f && px[2] > 0.05f && px[1] < 0.01f, "7. show: a blurred pixel turns pink");
        const std::vector<float> st = Run(g, stripe, W, H, Mv(W, H, 0, 0), W, H, 200, true);
        Check(std::memcmp(st.data(), stripe.data(), st.size() * 4) == 0, "7. show: still pixels keep their colour");
    }

    std::printf("%s: %d / %d checks passed\n", g_fail ? "FAILED" : "PASSED", g_pass, g_pass + g_fail);
    return g_fail ? 1 : 0;
}
