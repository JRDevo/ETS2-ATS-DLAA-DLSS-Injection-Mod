// Phase 101 test (issue #34): the NIS sharpen path of the sharpen pass (kRcasShader in src/scene_dlaa.cpp, Mode.w = 1).
// Runs the REAL shader source (read from src/scene_dlaa.cpp between its BEGIN / END markers) on WARP, no GPU needed:
//   - a flat picture stays flat (NIS and RCAS, LDR and HDR)
//   - a soft (blurred) vertical edge gets steeper with NIS: the dark side darker, the bright side brighter, nothing outside 0..1 (LDR)
//   - a soft horizontal edge too (the edge map picks the other direction)
//   - HDR (compressed values): finite, no NaN / Inf, a bright 20.0 highlight next to 0.05 stays finite and below 65504
//   - Sharpness 0 = unchanged; NIS differs from RCAS on the edge; more Sharpness = a steeper edge
// phase 104 (stronger, tunable RCAS): Sharpness 0..2, halo limit (Rcas2.x), brightness-only (Rcas2.y); the CB is 16 floats:
//   - Sharpness <= 1, halo 0, colour: BIT-IDENTICAL to the pre-104 shader (tests/rcas_pre104_reference.hlsl), RCAS and NIS, LDR + HDR
//   - halo h: the result leaves the 5-tap range by at most h * that range (and does leave it: the overshoot is the point)
//   - Sharpness 2 steepens a soft edge more than 1; brightness-only = equal per-channel changes; NIS holds 1 above 1; HDR finite
// Build: tests\build_sharpen_nis_test.bat   Run: build\tests\sharpen_nis_test.exe (exit 0 = all checks passed)
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
static int s_checks = 0, s_fails = 0;
#define CHECK(cond, ...) do { ++s_checks; if (!(cond)) { ++s_fails; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::string ReadRepoFile(const char* rel) {
    std::ifstream f(rel, std::ios::binary);
    if (!f) { std::ifstream g(std::string("../../") + rel, std::ios::binary); std::stringstream s; s << g.rdbuf(); f.close(); return s.str(); }
    std::stringstream s; s << f.rdbuf();
    return s.str();
}
static std::string ShaderSource() { return ReadRepoFile("src/scene_dlaa.cpp"); }

struct Gpu { ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx; ComPtr<ID3D11ComputeShader> cs; ComPtr<ID3D11SamplerState> smp; };

// Runs the pass on a W x H RGBA16F picture; cb = the 16 floats of RcasCB (phase 104: was 12). cs = another shader (the pre-104
// reference; it reads only the first 12 floats), nullptr = the current one.
static std::vector<float> Run(Gpu& g, int W, int H, const std::vector<float>& rgba, const float cb[16], ID3D11ComputeShader* cs = nullptr) {
    D3D11_TEXTURE2D_DESC td{}; td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{ rgba.data(), (UINT)(W * 16), 0 };
    ComPtr<ID3D11Texture2D> src, dst, st; g.dev->CreateTexture2D(&td, &sd, &src);
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS; g.dev->CreateTexture2D(&td, nullptr, &dst);
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; g.dev->CreateTexture2D(&td, nullptr, &st);
    ComPtr<ID3D11ShaderResourceView> srv; g.dev->CreateShaderResourceView(src.Get(), nullptr, &srv);
    ComPtr<ID3D11UnorderedAccessView> uav; g.dev->CreateUnorderedAccessView(dst.Get(), nullptr, &uav);
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cd{ cb, 0, 0 };
    ComPtr<ID3D11Buffer> buf; g.dev->CreateBuffer(&bd, &cd, &buf);
    ID3D11ShaderResourceView* srvs[2] = { srv.Get(), srv.Get() };
    ID3D11UnorderedAccessView* u = uav.Get(); ID3D11Buffer* b = buf.Get(); ID3D11SamplerState* s = g.smp.Get();
    g.ctx->CSSetShader(cs ? cs : g.cs.Get(), nullptr, 0);
    g.ctx->CSSetShaderResources(0, 2, srvs); g.ctx->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
    g.ctx->CSSetConstantBuffers(0, 1, &b); g.ctx->CSSetSamplers(0, 1, &s);
    g.ctx->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
    g.ctx->CopyResource(st.Get(), dst.Get());
    D3D11_MAPPED_SUBRESOURCE mp{}; g.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &mp);
    std::vector<float> out((size_t)W * H * 4);
    for (int y = 0; y < H; ++y) memcpy(&out[(size_t)y * W * 4], (const char*)mp.pData + (size_t)y * mp.RowPitch, (size_t)W * 16);
    g.ctx->Unmap(st.Get(), 0);
    return out;
}

static std::vector<float> Picture(int W, int H, bool vertical, float lo, float hi) {
    std::vector<float> p((size_t)W * H * 4);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        const int c = vertical ? x : y, n = vertical ? W : H;
        const float t = 0.5f * (1.0f + std::tanh((c - n / 2 + 0.5f) / 2.0f));   // a soft (blurred) edge in the middle
        const float v = lo + (hi - lo) * t;
        float* q = &p[((size_t)y * W + x) * 4]; q[0] = q[1] = q[2] = v; q[3] = 1.0f;
    }
    return p;
}

static float At(const std::vector<float>& p, int W, int x, int y) { return p[((size_t)y * W + x) * 4 + 1]; }
// the steepest step between two neighbours along row y (vertical edge) or column x (horizontal edge)
static float Slope(const std::vector<float>& p, int W, int H, bool vertical) {
    float m = 0;
    if (vertical) for (int x = 1; x < W; ++x) m = std::fmax(m, At(p, W, x, H / 2) - At(p, W, x - 1, H / 2));
    else          for (int y = 1; y < H; ++y) m = std::fmax(m, At(p, W, W / 2, y) - At(p, W, W / 2, y - 1));
    return m;
}

// phase 101c (audit): a CPU reference of NVSharpen (NIS SDK v1.0.3 NIS_Scaler.h, sharpen-only, SDR config), written from the
// SDK's structure -- NOT copied from the shader -- so a swapped direction, a dropped LTI or a wrong constant in the HLSL shows
// up as a per-pixel mismatch. p[y][x] = luma at (x - 2, y - 2).
namespace Ref {
static float Lti(const float y[5]) {
    const float aMin = std::fmin(std::fmin(y[0], y[1]), y[2]), aMax = std::fmax(std::fmax(y[0], y[1]), y[2]);
    const float bMin = std::fmin(std::fmin(y[2], y[3]), y[4]), bMax = std::fmax(std::fmax(y[2], y[3]), y[4]);
    const float ac = aMax - aMin, bc = bMax - bMin;
    const float ratio = std::fmax(ac, bc) / (std::fmin(ac, bc) + 1.0f / 255.0f);
    const float t = (ratio - 2.0f) * (1.0f / (10.0f - 2.0f));
    return 1.0f - std::fmin(std::fmax(t, 0.0f), 1.0f);
}
static float Usm(const float y[5], float str, float lim) {
    float u = (-0.6001f * y[1] + 1.2002f * y[2] - 0.6001f * y[3]) * str;
    u = std::fmin(lim, std::fmax(-lim, u));
    return u * Lti(y);
}
static float SharpenY(const float p[5][5], float sharpness) {
    const float s = sharpness - 0.5f;
    const float MaxScale = s >= 0 ? 1.25f : 1.75f, MinScale = s >= 0 ? 1.25f : 1.0f, LimitScale = s >= 0 ? 1.25f : 1.0f;
    const float sMin = std::fmax(0.0f, 0.4f + s * MinScale * 1.2f), sMax = 1.6f + s * MaxScale * 1.8f;
    const float lMin = std::fmax(0.1f, 0.14f + s * LimitScale * 0.32f), lMax = 0.5f + s * LimitScale * 0.6f;
    const float scaleY = 1.0f - std::fmin(std::fmax((p[2][2] - 0.45f) / (0.9f - 0.45f), 0.0f), 1.0f);
    const float str = scaleY * (sMax - sMin) + sMin, lim = (scaleY * (lMax - lMin) + lMin) * p[2][2];
    float d0[5], d90[5], d45[5], d135[5];
    for (int i = 0; i < 5; ++i) { d0[i] = p[i][2]; d90[i] = p[2][i]; }
    d45[0] = p[1][1]; d45[1] = 0.5f * (p[2][1] + p[1][2]); d45[2] = p[2][2]; d45[3] = 0.5f * (p[3][2] + p[2][3]); d45[4] = p[3][3];
    d135[0] = p[3][1]; d135[1] = 0.5f * (p[3][2] + p[2][1]); d135[2] = p[2][2]; d135[3] = 0.5f * (p[2][3] + p[1][2]); d135[4] = p[1][3];
    const float u0 = Usm(d0, str, lim), u90 = Usm(d90, str, lim), u45 = Usm(d45, str, lim), u135 = Usm(d135, str, lim);
    // GetEdgeMap(p, 1, 1)
    auto P = [&](int r, int c) { return p[1 + r][1 + c]; };
    const float g0   = std::fabs(P(0,0) + P(0,1) + P(0,2) - P(2,0) - P(2,1) - P(2,2));
    const float g45  = std::fabs(P(1,0) + P(0,0) + P(0,1) - P(2,1) - P(2,2) - P(1,2));
    const float g90  = std::fabs(P(0,0) + P(1,0) + P(2,0) - P(0,2) - P(1,2) - P(2,2));
    const float g135 = std::fabs(P(1,0) + P(2,0) + P(2,1) - P(0,1) - P(0,2) - P(1,2));
    const float a = std::fmax(g0, g90), ai = std::fmin(g0, g90), b = std::fmax(g45, g135), bi = std::fmin(g45, g135);
    if (a + b == 0.0f) return 0.0f;
    const float e090 = std::fmin(a / (a + b), 1.0f), e45 = 1.0f - e090;
    const float ratio = 2.0f * 1127.0f / 1024.0f, thr = 64.0f / 1024.0f;
    const bool c090 = a > ai * ratio && a > thr && a > bi, c45 = b > bi * ratio && b > thr && b > ai;
    const float f090 = (c090 && c45) ? e090 : 1.0f, f45 = (c090 && c45) ? e45 : 1.0f;
    const float w0 = (c090 && a == g0) ? f090 : 0.0f, w90 = (c090 && a != g0) ? f090 : 0.0f;
    const float w45 = (c45 && b == g45) ? f45 : 0.0f, w135 = (c45 && b != g45) ? f45 : 0.0f;
    return u0 * w0 + u90 * w90 + u45 * w45 + u135 * w135;
}
}  // namespace Ref

int main() {
    const std::string src = ShaderSource();
    const size_t a = src.find("const char kRcasShader[] = R\"(");
    const size_t e = a == std::string::npos ? a : src.find(")\";", a);
    if (a == std::string::npos || e == std::string::npos) { printf("FAIL: kRcasShader not found in src/scene_dlaa.cpp (run from the repo root)\n"); return 1; }
    const std::string hlsl = src.substr(a + 30, e - (a + 30));
    Gpu g;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx))) {
        printf("FAIL: no WARP device\n"); return 1;
    }
    ComPtr<ID3DBlob> code, err;
    if (FAILED(D3DCompile(hlsl.data(), hlsl.size(), "rcas", nullptr, nullptr, "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        printf("FAIL: compile: %s\n", err ? (const char*)err->GetBufferPointer() : "?"); return 1;
    }
    g.dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g.cs);
    D3D11_SAMPLER_DESC sm{}; sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sm.MaxLOD = D3D11_FLOAT32_MAX;
    g.dev->CreateSamplerState(&sm, &g.smp);

    const int W = 32, H = 32;
    auto cbOf = [](float sharp, bool hdr, bool nis) {
        static float cb[16];
        const float v[16] = { sharp, 1.5f, 0, 0, 0, 0, 0, 0, hdr ? 1.0f : 0.0f, 0, 0, nis ? 1.0f : 0.0f, 0, 0, 0, 0 };
        memcpy(cb, v, sizeof(cb)); return (const float*)cb;
    };
    // 1. flat stays flat
    for (int hdr = 0; hdr < 2; ++hdr) for (int nis = 0; nis < 2; ++nis) {
        const float lv = hdr ? 3.0f : 0.5f;
        auto in = Picture(W, H, true, lv, lv);
        auto out = Run(g, W, H, in, cbOf(0.8f, hdr != 0, nis != 0));
        float md = 0; for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) md = std::fmax(md, std::fabs(At(out, W, x, y) - lv));
        CHECK(md < 1e-3f * (hdr ? lv : 1.0f), "flat %s %s: changed by %.5f", hdr ? "HDR" : "LDR", nis ? "NIS" : "RCAS", md);
    }
    // 2. soft vertical / horizontal edge, LDR NIS: steeper, inside 0..1
    for (int vert = 0; vert < 2; ++vert) {
        auto in = Picture(W, H, vert != 0, 0.2f, 0.8f);
        auto out = Run(g, W, H, in, cbOf(0.5f, false, true));
        auto px = [&](const std::vector<float>& p, int c) { return vert ? At(p, W, c, H / 2) : At(p, W, W / 2, c); };
        const float inSlope = Slope(in, W, H, vert != 0), outSlope = Slope(out, W, H, vert != 0);
        CHECK(outSlope > inSlope + 0.01f, "%s edge: NIS did not steepen it (slope %.4f -> %.4f)", vert ? "vertical" : "horizontal", inSlope, outSlope);
        float mn = 1, mx = 0; for (float v : out) { mn = std::fmin(mn, v); mx = std::fmax(mx, v); }
        CHECK(mn >= 0.0f && mx <= 1.0f, "%s edge LDR: out of 0..1 (%.4f .. %.4f)", vert ? "vertical" : "horizontal", mn, mx);
        CHECK(std::fabs(px(out, 2) - px(in, 2)) < 1e-3f && std::fabs(px(out, W - 3) - px(in, W - 3)) < 1e-3f, "%s edge: far parts changed", vert ? "vertical" : "horizontal");
    }
    // 3. more sharpness = steeper; 0 = unchanged; NIS != RCAS
    {
        auto in = Picture(W, H, true, 0.2f, 0.8f);
        auto o0 = Run(g, W, H, in, cbOf(0.0f, false, true));
        auto o3 = Run(g, W, H, in, cbOf(0.3f, false, true));
        auto o9 = Run(g, W, H, in, cbOf(0.9f, false, true));
        auto rc = Run(g, W, H, in, cbOf(0.9f, false, false));
        float d0 = 0; for (size_t i = 0; i < in.size(); ++i) d0 = std::fmax(d0, std::fabs(o0[i] - in[i]));
        CHECK(d0 < 1e-5f, "sharpness 0: changed by %.6f", d0);
        const float s3 = Slope(o3, W, H, true), s9 = Slope(o9, W, H, true);
        CHECK(s9 > s3, "sharpness 0.9 not steeper than 0.3 (%.4f vs %.4f)", s9, s3);
        float dr = 0; for (size_t i = 0; i < in.size(); ++i) dr = std::fmax(dr, std::fabs(o9[i] - rc[i]));
        CHECK(dr > 1e-3f, "NIS and RCAS gave the same picture (Mode.w ignored?)");
    }
    // 4. HDR: finite, bounded, still sharpens
    {
        auto in = Picture(W, H, true, 0.05f, 20.0f);
        auto out = Run(g, W, H, in, cbOf(1.0f, true, true));
        bool fin = true; float mx = 0;
        for (float v : out) { if (!std::isfinite(v)) fin = false; mx = std::fmax(mx, v); }
        CHECK(fin && mx < 65504.0f, "HDR: non-finite or too large output (max %.1f)", mx);
        auto in2 = Picture(W, H, true, 0.5f, 2.0f);
        auto out2 = Run(g, W, H, in2, cbOf(0.6f, true, true));
        CHECK(Slope(out2, W, H, true) > Slope(in2, W, H, true), "HDR edge: NIS did not steepen it (%.4f -> %.4f)", Slope(in2, W, H, true), Slope(out2, W, H, true));
        float mn = 1e9f; for (float v : out2) mn = std::fmin(mn, v);
        CHECK(mn >= 0.0f, "HDR: negative output %.4f", mn);
    }
    // 5. phase 101b (Fable review): HDR thin bright feature -- a 2 px stripe at 19.0 on 0.5 must not become a firefly (it did:
    //    x26 to 499 before the 5-tap clamp), and a flat 1000.0 sky / sun stays 1000 (no fixed 499 cap)
    {
        std::vector<float> in((size_t)W * H * 4);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const float v = (x == W / 2 || x == W / 2 + 1) ? 19.0f : 0.5f;
            float* q = &in[((size_t)y * W + x) * 4]; q[0] = q[1] = q[2] = v; q[3] = 1.0f;
        }
        for (float sh : { 0.4f, 1.0f }) {
            auto out = Run(g, W, H, in, cbOf(sh, true, true));
            float mx = 0; for (float v : out) mx = std::fmax(mx, v);
            CHECK(mx <= 19.0f * 1.001f, "HDR thin bright stripe at sharpness %.1f: overshoot to %.1f (input max 19.0)", sh, mx);
        }
        auto flat = Picture(W, H, true, 1000.0f, 1000.0f);
        auto outF = Run(g, W, H, flat, cbOf(0.8f, true, true));
        float md = 0; for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) md = std::fmax(md, std::fabs(At(outF, W, x, y) - 1000.0f));
        CHECK(md < 1.0f, "HDR flat 1000.0: changed by %.2f (a fixed highlight cap?)", md);
    }
    // 6. a soft DIAGONAL edge (the 45 / 135 directions of the edge map) gets steeper too, in both orientations
    for (int flip = 0; flip < 2; ++flip) {
        std::vector<float> in((size_t)W * H * 4);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const float s = (float)(flip ? (x + (H - 1 - y)) : (x + y)) - (float)(W - 1);
            const float v = 0.2f + 0.6f * 0.5f * (1.0f + std::tanh(s / 3.0f));
            float* q = &in[((size_t)y * W + x) * 4]; q[0] = q[1] = q[2] = v; q[3] = 1.0f;
        }
        auto out = Run(g, W, H, in, cbOf(0.6f, false, true));
        auto diag = [&](const std::vector<float>& p) {   // steepest step along the line across the edge
            float m = 0;
            for (int i = 1; i < W; ++i) {
                const int y0 = flip ? H - i : i - 1, y1 = flip ? H - 1 - i : i;
                m = std::fmax(m, At(p, W, i, y1) - At(p, W, i - 1, y0));
            }
            return m;
        };
        CHECK(diag(out) > diag(in) + 0.005f, "diagonal edge (%s): NIS did not steepen it (%.4f -> %.4f)", flip ? "135" : "45", diag(in), diag(out));
    }
    // 7. phase 101c (audit): per-pixel match with the CPU reference (LDR) on pictures that hit every branch: random blocks
    //    (straight 0 / 90 edges), random diagonal stripes (45 / 135), one-sided steps (LTI damping), fine noise (thresholds)
    {
        unsigned rng = 12345u;
        auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return (float)((rng >> 8) & 0xFFFF) / 65535.0f; };
        int worst = 0; float maxErr = 0; long long compared = 0, sharpened = 0;
        for (int kind = 0; kind < 4; ++kind) for (int rep = 0; rep < 3; ++rep) {
            std::vector<float> in((size_t)W * H * 4);
            const float bx = 3 + 5 * rnd(), lo = 0.1f + 0.3f * rnd(), hi = 0.5f + 0.5f * rnd();
            for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
                float v;
                if (kind == 0)      v = (((int)(x / bx) + (int)(y / bx)) & 1) ? hi : lo;                      // blocks
                else if (kind == 1) v = (((int)((x + y) / bx)) & 1) ? hi : lo;                                // 45 stripes
                else if (kind == 2) v = (((int)((x - y + 64) / bx)) & 1) ? hi : lo;                           // 135 stripes
                else                v = lo + (hi - lo) * rnd();                                               // noise
                if (kind < 3) v += 0.03f * (rnd() - 0.5f);   // a little texture so LTI sees both sides
                v = std::fmin(std::fmax(v, 0.0f), 1.0f);
                float* q = &in[((size_t)y * W + x) * 4]; q[0] = q[1] = q[2] = v; q[3] = 1.0f;
            }
            for (float sh : { 0.2f, 0.5f, 0.9f }) {
                auto out = Run(g, W, H, in, cbOf(sh, false, true));
                for (int y = 2; y < H - 2; ++y) for (int x = 2; x < W - 2; ++x) {
                    float p[5][5];
                    for (int j = 0; j < 5; ++j) for (int i = 0; i < 5; ++i) {
                        const float* q = &in[((size_t)(y + j - 2) * W + (x + i - 2)) * 4];
                        p[j][i] = 0.2126f * q[0] + 0.7152f * q[1] + 0.0722f * q[2];
                    }
                    const float ctr = At(in, W, x, y);
                    const float ref = std::fmin(std::fmax(ctr + Ref::SharpenY(p, sh), 0.0f), 1.0f);
                    const float dif = std::fabs(At(out, W, x, y) - ref);
                    ++compared; if (std::fabs(ref - ctr) > 1e-4f) ++sharpened;
                    if (dif > maxErr) { maxErr = dif; worst = kind; }
                }
            }
        }
        CHECK(maxErr < 2e-4f, "CPU reference: max per-pixel error %.6f (picture kind %d) -- the shader is not NVSharpen", maxErr, worst);
        CHECK(sharpened > compared / 10, "CPU reference: only %lld of %lld pixels sharpened -- pictures too weak to test", sharpened, compared);
    }
    // 8. phase 101c (audit): an Inf texel must not blank its 5x5 neighbourhood (NaN * 0 weight = NaN before the guard)
    for (int hdr = 0; hdr < 2; ++hdr) {
        auto in = Picture(W, H, true, 0.3f, 0.6f);
        float* q = &in[((size_t)(H / 2) * W + W / 2) * 4]; q[0] = q[1] = q[2] = INFINITY;
        auto out = Run(g, W, H, in, cbOf(0.6f, hdr != 0, true));
        int bad = 0;
        for (int y = H / 2 - 2; y <= H / 2 + 2; ++y) for (int x = W / 2 - 2; x <= W / 2 + 2; ++x) {
            if (x == W / 2 && y == H / 2) continue;
            const float v = At(out, W, x, y), o = At(in, W, x, y);
            if (!std::isfinite(v) || std::fabs(v - o) > 0.2f) ++bad;
        }
        CHECK(bad == 0, "%s: an Inf texel spoiled %d of its 24 neighbours", hdr ? "HDR" : "LDR", bad);
    }
    // ---- phase 104 --------------------------------------------------------------------------------------------------------------
    ComPtr<ID3D11ComputeShader> refCs;
    {
        const std::string ref = ReadRepoFile("tests/rcas_pre104_reference.hlsl");
        ComPtr<ID3DBlob> rc, re;
        if (ref.empty() || FAILED(D3DCompile(ref.data(), ref.size(), "rcas_pre104", nullptr, nullptr, "CSMain", "cs_5_0",
                                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &rc, &re))) {
            printf("FAIL: tests/rcas_pre104_reference.hlsl: %s\n", re ? (const char*)re->GetBufferPointer() : "not found");
            return 1;
        }
        g.dev->CreateComputeShader(rc->GetBufferPointer(), rc->GetBufferSize(), nullptr, &refCs);
    }
    auto cb104 = [](float sharp, float radius, bool hdr, bool nis, float halo, bool luma) {
        static float cb[16];
        const float v[16] = { sharp, radius, 0, 0, 0, 0, 0, 0, hdr ? 1.0f : 0.0f, 0, 0, nis ? 1.0f : 0.0f, halo, luma ? 1.0f : 0.0f, 0, 0 };
        memcpy(cb, v, sizeof(cb)); return (const float*)cb;
    };
    unsigned rng2 = 777u;
    auto rnd2 = [&]() { rng2 = rng2 * 1664525u + 1013904223u; return (float)((rng2 >> 8) & 0xFFFF) / 65535.0f; };
    auto colourPic = [&](float scale) {   // coloured blocks + noise + a soft edge: the channels differ, every branch is hit
        std::vector<float> in((size_t)W * H * 4);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            float* q = &in[((size_t)y * W + x) * 4];
            const float t = 0.5f * (1.0f + std::tanh((x - W / 2 + 0.5f) / 1.5f));
            for (int c = 0; c < 3; ++c) {
                const float blk = (((x / (3 + c)) + (y / (4 + c))) & 1) ? 0.15f : 0.0f;
                q[c] = scale * std::fmin(std::fmax(0.1f + 0.6f * t * (0.6f + 0.2f * c) + blk + 0.1f * rnd2(), 0.0f), 1.0f);
            }
            q[3] = 1.0f;
        }
        return in;
    };
    auto midRange = [&](std::vector<float> in) {   // into 0.25..0.75: saturate must not hide an overshoot or a channel change
        for (size_t i = 0; i < in.size(); i += 4) for (int c = 0; c < 3; ++c) in[i + c] = 0.25f + 0.5f * in[i + c];
        return in;
    };
    // 9. (a) Sharpness <= 1, halo 0, colour: bit-identical to the pre-104 shader -- RCAS (3 radii) and NIS, LDR and HDR
    {
        long long px = 0, diff = 0;
        for (int hdr = 0; hdr < 2; ++hdr) for (int nis = 0; nis < 2; ++nis) for (float rad : { 1.0f, 1.5f, 2.5f }) {
            if (nis && rad != 1.5f) continue;   // NIS does not read Radius
            auto in = colourPic(hdr ? 12.0f : 1.0f);
            for (int k = 0; k <= 10; ++k) {
                const float sh = 0.1f * (float)k;
                auto now = Run(g, W, H, in, cb104(sh, rad, hdr != 0, nis != 0, 0.0f, false));
                auto old = Run(g, W, H, in, cb104(sh, rad, hdr != 0, nis != 0, 0.0f, false), refCs.Get());
                for (size_t i = 0; i < now.size(); ++i) { ++px; if (memcmp(&now[i], &old[i], 4) != 0) ++diff; }
            }
        }
        CHECK(diff == 0, "Sharpness <= 1, halo 0, colour: %lld of %lld values differ from the pre-104 shader", diff, px);
        printf("phase 104 (a): %lld values compared with the pre-104 shader, %lld differ\n", px, diff);
    }
    // 9. (b) halo h: LDR, Radius 1 (the ring taps are exact texels, so the CPU knows the 5-tap range): the result stays inside
    //    [mn5 - h * range, mx5 + h * range] and does overshoot somewhere
    for (float h : { 0.1f, 0.25f, 0.5f }) for (float sh : { 1.0f, 2.0f }) {
        const auto in = midRange(colourPic(1.0f));
        auto out = Run(g, W, H, in, cb104(sh, 1.0f, false, false, h, false));
        float worst = -1, over = 0;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) for (int c = 0; c < 3; ++c) {
            auto T = [&](int dx, int dy) { const int xx = std::min(std::max(x + dx, 0), W - 1), yy = std::min(std::max(y + dy, 0), H - 1);
                                           return in[((size_t)yy * W + xx) * 4 + c]; };
            const float t[5] = { T(0, 0), T(0, -1), T(-1, 0), T(1, 0), T(0, 1) };
            float mn = t[0], mx = t[0]; for (float v : t) { mn = std::fmin(mn, v); mx = std::fmax(mx, v); }
            const float o = out[((size_t)y * W + x) * 4 + c], r = h * (mx - mn);
            worst = std::fmax(worst, std::fmax(o - (mx + r), (mn - r) - o));
            over  = std::fmax(over, std::fmax(o - mx, mn - o));
        }
        CHECK(worst < 1e-5f, "halo %.2f sharpness %.1f: the result leaves the 5-tap range by more than h * range (by %.6f)", h, sh, worst);
        CHECK(over > 1e-3f, "halo %.2f sharpness %.1f: no overshoot past the 5-tap range at all (max %.6f)", h, sh, over);
        printf("phase 104 (b): halo %.2f sharpness %.1f: max overshoot past the 5-tap range %.4f, past the halo limit %.6f\n", h, sh, over, worst);
    }
    // 9. (c) Sharpness 2 steepens a soft edge more than 1 (halo 25 %); at halo 0 it is not flatter
    {
        auto in = Picture(W, H, true, 0.2f, 0.8f);
        const float s1 = Slope(Run(g, W, H, in, cb104(1.0f, 1.5f, false, false, 0.25f, false)), W, H, true);
        const float s2 = Slope(Run(g, W, H, in, cb104(2.0f, 1.5f, false, false, 0.25f, false)), W, H, true);
        CHECK(s2 > s1 + 0.005f, "sharpness 2 not steeper than 1 at halo 25 %% (%.4f vs %.4f)", s2, s1);
        const float z1 = Slope(Run(g, W, H, in, cb104(1.0f, 1.5f, false, false, 0.0f, false)), W, H, true);
        const float z2 = Slope(Run(g, W, H, in, cb104(2.0f, 1.5f, false, false, 0.0f, false)), W, H, true);
        CHECK(z2 >= z1 - 1e-6f, "sharpness 2 flatter than 1 at halo 0 (%.4f vs %.4f)", z2, z1);
        printf("phase 104 (c): soft edge slope in %.4f; halo 25 %%: S1 %.4f S2 %.4f; halo 0: S1 %.4f S2 %.4f\n",
               Slope(in, W, H, true), s1, s2, z1, z2);
    }
    // 9. (d) brightness-only: every channel moves by the same amount (LDR inside 0.25..0.75: saturate never bites), the luma
    //    stays inside the 5 taps' luma range +- the halo (Radius 1 = exact taps); colour mode moves the channels differently
    for (float h : { 0.0f, 0.25f }) for (float sh : { 1.0f, 2.0f }) {
        const auto in = midRange(colourPic(1.0f));
        auto lu = Run(g, W, H, in, cb104(sh, 1.0f, false, false, h, true));
        auto co = Run(g, W, H, in, cb104(sh, 1.0f, false, false, h, false));
        auto Y = [](const float* q) { return 0.25f * q[0] + 0.5f * q[1] + 0.25f * q[2]; };
        long long sharp = 0, equal = 0; float coSpread = 0, lumaOut = -1;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const size_t i = ((size_t)y * W + x) * 4;
            float d[3], dc[3];
            for (int c = 0; c < 3; ++c) { d[c] = lu[i + c] - in[i + c]; dc[c] = co[i + c] - in[i + c]; }
            coSpread = std::fmax(coSpread, std::fmax(std::fabs(dc[0] - dc[1]), std::fabs(dc[1] - dc[2])));
            float mnL = 1e9f, mxL = -1e9f;
            static const int kOff[5][2] = { { 0, 0 }, { 0, -1 }, { -1, 0 }, { 1, 0 }, { 0, 1 } };
            for (const auto& o : kOff) {
                const int xx = std::min(std::max(x + o[0], 0), W - 1), yy = std::min(std::max(y + o[1], 0), H - 1);
                const float l = Y(&in[((size_t)yy * W + xx) * 4]);
                mnL = std::fmin(mnL, l); mxL = std::fmax(mxL, l);
            }
            const float yo = Y(&lu[i]), r = h * (mxL - mnL);
            lumaOut = std::fmax(lumaOut, std::fmax(yo - (mxL + r), (mnL - r) - yo));
            if (std::fabs(d[0]) + std::fabs(d[1]) + std::fabs(d[2]) < 3e-4f) continue;
            ++sharp;
            if (std::fabs(d[0] - d[1]) < 2e-5f && std::fabs(d[1] - d[2]) < 2e-5f) ++equal;
        }
        CHECK(sharp > (long long)(W * H) / 10 && equal == sharp, "brightness only (halo %.2f, S %.1f): %lld of %lld sharpened pixels "
              "have equal per-channel changes (want all)", h, sh, equal, sharp);
        CHECK(lumaOut < 1e-5f, "brightness only (halo %.2f, S %.1f): the luma leaves the 5-tap luma range +- halo by %.6f", h, sh, lumaOut);
        CHECK(coSpread > 1e-3f, "colour mode: the channels all moved alike (spread %.6f) -- the picture does not test it", coSpread);
        printf("phase 104 (d): brightness only, halo %.2f S %.1f: %lld of %lld sharpened pixels equal per channel; luma past the "
               "limit %.6f; colour mode channel spread %.4f\n", h, sh, equal, sharp, lumaOut, coSpread);
    }
    // 9. (e) NIS holds 1 above 1 (bit-identical to 1); HDR RCAS at 2 with halo 50 %: finite, >= 0, < 65504, no firefly
    {
        for (int hdr = 0; hdr < 2; ++hdr) {
            auto in = colourPic(hdr ? 12.0f : 1.0f);
            auto n1 = Run(g, W, H, in, cb104(1.0f, 1.5f, hdr != 0, true, 0.5f, true));
            auto n2 = Run(g, W, H, in, cb104(2.0f, 1.5f, hdr != 0, true, 0.5f, true));
            CHECK(memcmp(n1.data(), n2.data(), n1.size() * 4) == 0, "%s NIS: sharpness 2 differs from 1 (the slider must hold at 1)",
                  hdr ? "HDR" : "LDR");
        }
        const auto edge = Picture(W, H, true, 0.05f, 20.0f);
        std::vector<float> stripe((size_t)W * H * 4);
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
            const float v = (x == W / 2 || x == W / 2 + 1) ? 19.0f : 0.5f;
            float* q = &stripe[((size_t)y * W + x) * 4]; q[0] = q[1] = q[2] = v; q[3] = 1.0f;
        }
        for (int luma = 0; luma < 2; ++luma) for (int pic = 0; pic < 2; ++pic) {
            auto out = Run(g, W, H, pic ? stripe : edge, cb104(2.0f, 1.5f, true, false, 0.5f, luma != 0));
            bool fin = true; float mn = 1e9f, mx = 0;
            for (float v : out) { if (!std::isfinite(v)) fin = false; mn = std::fmin(mn, v); mx = std::fmax(mx, v); }
            CHECK(fin && mn >= 0.0f && mx < 65504.0f, "HDR RCAS sharpness 2 halo 50 %% (%s, %s): min %.4f max %.1f finite %d",
                  pic ? "stripe" : "edge", luma ? "brightness" : "colour", mn, mx, (int)fin);
            // the top of the halo stops halfway to compressed 1 = at most 2 L + 1 for the brightest tap L (20 here)
            CHECK(mx <= 41.0f * 1.001f, "HDR RCAS sharpness 2 halo 50 %% (%s, %s): firefly %.1f (input max 20, cap 2 L + 1 = 41)",
                  pic ? "stripe" : "edge", luma ? "brightness" : "colour", mx);
            printf("phase 104 (e): HDR RCAS S2 halo 50 %% %s %s: out %.4f .. %.2f\n", pic ? "stripe" : "edge",
                   luma ? "brightness" : "colour", mn, mx);
        }
    }
    printf("sharpen_nis_test: %d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
