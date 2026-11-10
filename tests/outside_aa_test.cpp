// phase 81 unit test (WARP, no GPU needed): "outside AA" (src/outside_aa.h, scene_dlaa.cpp, dlaa.ini outside_aa).
//
// Part A (maths, no D3D): the feather interior / bands against a brute-force evaluation of the shaders' feather mask (every pixel
// whose mask is < 1 lies in exactly one band, every interior pixel has mask 1, bands and interior tile the image), the Catmull-Rom
// weights (sum 1, exact at fraction 0, reproduce a linear ramp), ShiftFromNgx, FillParams.
// Part B (WARP, the real HLSL of OutsideAa::kOutsideAaShader): frames are RASTERISED with the mod's jittered viewport (viewport 0
// TopLeft += s, (W+1) x (H+1) = jitter_overscan, as inject.cpp ReconcileViewports does) for the 8 Halton phases, so the sign
// convention is checked against what D3D really does with a viewport shift -- not against our own formula:
//   B1 in place (CSMain, RGBA16F / HDR luma): the unjittered outside matches the shift-0 frame (mean error < 15 % of the raw
//      frame's jitter error, max error small), the interior is never written; MUTATION: the opposite sign must be WORSE than raw.
//   B2 in place, R8G8B8A8_UNORM, mode 2 (+ edge AA) on the same smooth scene: still close to the shift-0 frame.
//   B3 thin dark slanted lines on a bright sky (aliased, sub-pixel: the reported case): per-pixel spread over the 8 phases, raw vs
//      mode 1 vs mode 2 (printed; mode 2 must be steadier than raw, mode 1 not much worse -- unjitter cannot fix sub-pixel aliasing).
//   B4 upscale (PSMain drawn per band with an output origin, render 72x80 -> output 96x80 = the user's x1.333 / x1.0): the outside
//      matches the analytic shift-0 picture far better than the game's bilinear stretch of the jittered frame; interior + the RT
//      margin never written, every band pixel written; MUTATION: the opposite sign must fail.
// Exit code 0 = all passed.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "outside_aa.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

using OutsideAa::Box;

// ---- Part A ------------------------------------------------------------------------------------------------------------------
static bool In(const Box& b, int x, int y) { return x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1; }

static void PartA() {
    const int W = 41, H = 33;
    const Box rects[] = { { 8, 6, 30, 25 }, { 0, 0, 20, 33 }, { 5, 0, 41, 12 }, { 10, 10, 13, 14 }, { 0, 0, 41, 33 } };
    const float feathers[] = { 0.0f, 0.5f, 1.0f, 2.5f, 3.0f, 4.75f, 7.3333f, 20.0f };
    int cases = 0;
    for (const Box& r : rects)
        for (float fx : feathers)
            for (int em = 0; em < 16; ++em) {
                const bool e[4] = { (em & 1) != 0 && r.x0 > 0, (em & 2) != 0 && r.y0 > 0, (em & 4) != 0 && r.x1 < W,
                                    (em & 8) != 0 && r.y1 < H };
                const float fy = fx * 0.75f;                 // a different feather per axis (upscale: output px per axis)
                const Box in = OutsideAa::Interior(r, fx, fy, e);
                Box bands[4];
                const int nb = OutsideAa::Bands(W, H, in, bands);
                bool ok = true;
                for (int y = 0; y < H && ok; ++y)
                    for (int x = 0; x < W && ok; ++x) {
                        const float m = OutsideAa::Mask(x, y, r, fx, fy, e);
                        int nIn = 0;
                        for (int b = 0; b < nb; ++b) nIn += In(bands[b], x, y) ? 1 : 0;
                        const bool inI = In(in, x, y);
                        if (inI == (nIn == 1) || nIn > 1) { ok = false; CHECK(false, "tiling (%d,%d) interior %d bands %d", x, y, inI, nIn); }
                        if (m < 1.0f && inI) { ok = false; CHECK(false, "mask %.6f < 1 inside the interior at (%d,%d) f=%.3f", m, x, y, fx); }
                        if (inI && m != 1.0f) { ok = false; CHECK(false, "interior pixel (%d,%d) mask %.6f", x, y, m); }
                        // tight: a rect pixel left out of the interior has a raw (pre-smoothstep) ramp below 1 on some axis
                        if (In(r, x, y) && !inI) {
                            const float ax = OutsideAa::AxisRamp(x - r.x0, r.x1 - r.x0, fx, e[0], e[2]);
                            const float ay = OutsideAa::AxisRamp(y - r.y0, r.y1 - r.y0, fy, e[1], e[3]);
                            if (!(ax < 1.0f || ay < 1.0f)) { ok = false; CHECK(false, "interior not tight at (%d,%d)", x, y); }
                        }
                    }
                CHECK(ok, "rect (%d,%d,%d,%d) f=%.3f edges %d", r.x0, r.y0, r.x1, r.y1, fx, em);
                ++cases;
            }
    // no feather / no inside edge: interior = the rect
    { const bool e[4] = { true, true, true, true }; const Box r{ 8, 6, 30, 25 }; const Box in = OutsideAa::Interior(r, 0.0f, 0.0f, e);
      CHECK(in.x0 == 8 && in.y0 == 6 && in.x1 == 30 && in.y1 == 25, "feather 0 interior"); }
    { const bool e[4] = { true, true, true, true }; const Box r{ 8, 6, 30, 25 }; const Box in = OutsideAa::Interior(r, 96.0f, 96.0f, e);
      Box b[4]; CHECK(OutsideAa::Bands(W, H, in, b) == 1 && b[0].x1 == W && b[0].y1 == H, "feather wider than the rect: whole image"); }
    // Catmull-Rom weights
    for (int i = 0; i <= 16; ++i) {
        const float f = (float)i / 16.0f;
        float w[4];
        OutsideAa::CatmullRomW(f, w);
        CHECK(std::fabs(w[0] + w[1] + w[2] + w[3] - 1.0f) < 1e-6f, "CR weights sum at f=%.3f", f);
        CHECK(std::fabs(-w[0] + w[2] + 2.0f * w[3] - f) < 1e-6f, "CR reproduces a linear ramp at f=%.3f", f);
        CHECK(w[1] + w[2] >= 1.0f - 1e-6f, "w12 >= 1 at f=%.3f (the shader divides by it)", f);
    }
    { float w[4]; OutsideAa::CatmullRomW(0.0f, w); CHECK(w[0] == 0.0f && w[1] == 1.0f && w[2] == 0.0f && w[3] == 0.0f, "CR at f=0 = the texel"); }
    CHECK(OutsideAa::ShiftFromNgx(0.25f, 1) == 0.25f && OutsideAa::ShiftFromNgx(0.25f, -1) == -0.25f, "ShiftFromNgx");
    float p[OutsideAa::kParamFloats];
    OutsideAa::FillParams(p, Box{ 1, 2, 3, 4 }, 200, 100, 150, 100, 0.3f, -0.2f, 2, 1, true, 7.0f, 9.0f);
    CHECK(p[0] == 1 && p[3] == 4 && std::fabs(p[4] - 1.0f / 200) < 1e-9f && std::fabs(p[6] - 0.3f / 150) < 1e-9f &&
          std::fabs(p[7] + 0.2f / 100) < 1e-9f && p[10] == 150 && p[12] == 2 && p[13] == 1 && p[14] == 1 && p[16] == 7 && p[17] == 9,
          "FillParams layout");
    std::printf("part A: %d feather cases\n", cases);
}

// ---- Part C (phase 93, no D3D): the bands clipped to a visibility mask ---------------------------------------------------------
// A lens-like hidden mesh (the square minus a 64-gon of radius 0.93, as quads between the polygon and its radial projection onto the
// square border + the corner wedge where two border points lie on different sides; shared edges in both orders) at several
// image sizes and rects. Brute force: a pixel is VISIBLE when its centre lies in no triangle (edges included, the Coverage rule).
// Checks: every visible pixel outside the feather interior lies in some clip box (the clip never drops a shown pixel), the boxes stay
// inside the bands, the clip skips a real share of the band pixels, no mask = the bands unchanged.
static bool InTri(const float* v, float px, float py) {
    auto E = [](float ax, float ay, float bx, float by, float x, float y) { return (bx - ax) * (y - ay) - (by - ay) * (x - ax); };
    const float e0 = E(v[0], v[1], v[2], v[3], px, py), e1 = E(v[2], v[3], v[4], v[5], px, py), e2 = E(v[4], v[5], v[0], v[1], px, py);
    return (e0 >= 0.0f && e1 >= 0.0f && e2 >= 0.0f) || (e0 <= 0.0f && e1 <= 0.0f && e2 <= 0.0f);
}
static void PartC() {
    std::vector<float> tri;
    const int N = 64;
    const float R = 0.93f;
    for (int i = 0; i < N; ++i) {
        const double a0 = 2.0 * 3.14159265358979 * i / N, a1 = 2.0 * 3.14159265358979 * (i + 1) / N;
        const float p0x = (float)(R * std::cos(a0)), p0y = (float)(R * std::sin(a0) * 0.97);   // slightly elliptic
        const float p1x = (float)(R * std::cos(a1)), p1y = (float)(R * std::sin(a1) * 0.97);
        auto proj = [](float x, float y, float* qx, float* qy) {
            const float m = std::fabs(x) > std::fabs(y) ? std::fabs(x) : std::fabs(y);
            *qx = x / m; *qy = y / m;
        };
        float q0x, q0y, q1x, q1y;
        proj(p0x, p0y, &q0x, &q0y);
        proj(p1x, p1y, &q1x, &q1y);
        const float t0[6] = { p0x, p0y, p1x, p1y, q1x, q1y }, t1[6] = { p0x, p0y, q1x, q1y, q0x, q0y };
        tri.insert(tri.end(), t0, t0 + 6);
        tri.insert(tri.end(), t1, t1 + 6);
        if (std::fabs(q0x) != std::fabs(q1x) && (std::fabs(q0x) == 1.0f) != (std::fabs(q1x) == 1.0f)) {   // around a corner
            const float cx = std::fabs(q0x) == 1.0f ? q0x : q1x, cy = std::fabs(q0y) == 1.0f ? q0y : q1y;
            const float t2[6] = { q0x, q0y, q1x, q1y, cx, cy };
            tri.insert(tri.end(), t2, t2 + 6);
        }
    }
    const uint32_t tris = (uint32_t)(tri.size() / 6);
    struct Case { uint32_t w, h; Box rect; float f; };
    const Case cases[] = { { 320, 360, { 80, 90, 240, 270 }, 24.0f }, { 257, 301, { 64, 75, 192, 225 }, 0.0f },
                           { 400, 300, { 130, 60, 290, 250 }, 12.5f }, { 96, 128, { 20, 30, 70, 100 }, 6.0f } };
    for (const Case& c : cases) {
        OutsideAa::VisRows v;
        CHECK(OutsideAa::BuildVisRows(tri.data(), tris, c.w, c.h, OutsideAa::kClipMarginPx, &v), "BuildVisRows %ux%u", c.w, c.h);
        const bool e[4] = { true, true, true, true };
        const Box in = OutsideAa::Interior(c.rect, c.f, c.f, e);
        Box bands[4], clip[4 * OutsideAa::kClipStrips];
        const int nb = OutsideAa::Bands(c.w, c.h, in, bands);
        uint64_t area = 0, kept = 0;
        const int nc = OutsideAa::SplitClip(v, bands, nb, clip, &area, &kept);
        int missed = 0, outsideBands = 0;
        for (int i = 0; i < nc; ++i) {
            bool inside = false;
            for (int k = 0; k < nb; ++k)
                inside = inside || (clip[i].x0 >= bands[k].x0 && clip[i].x1 <= bands[k].x1 && clip[i].y0 >= bands[k].y0 &&
                                    clip[i].y1 <= bands[k].y1);
            if (!inside) ++outsideBands;
        }
        uint64_t visBand = 0;
        for (uint32_t y = 0; y < c.h; ++y)
            for (uint32_t x = 0; x < c.w; ++x) {
                if (In(in, (int)x, (int)y)) continue;
                const float px = 2.0f * ((float)x + 0.5f) / (float)c.w - 1.0f, py = 1.0f - 2.0f * ((float)y + 0.5f) / (float)c.h;
                bool hidden = false;
                for (uint32_t k = 0; k < tris && !hidden; ++k) hidden = InTri(&tri[6 * k], px, py);
                if (hidden) continue;
                ++visBand;
                bool covered = false;
                for (int i = 0; i < nc && !covered; ++i) covered = In(clip[i], (int)x, (int)y);
                if (!covered) ++missed;
            }
        CHECK(missed == 0, "%ux%u: %d visible band pixels outside every clip box", c.w, c.h, missed);
        CHECK(outsideBands == 0, "%ux%u: %d clip boxes not inside a band", c.w, c.h, outsideBands);
        CHECK(kept <= area && kept >= visBand && (c.w < 200 || kept * 10 < area * 9), "%ux%u: kept %llu of %llu band px (visible "
              "%llu)", c.w, c.h, (unsigned long long)kept, (unsigned long long)area, (unsigned long long)visBand);
        std::printf("part C %ux%u: %d boxes, %.1f %% of the band pixels skipped (visible band pixels %llu, kept %llu of %llu)\n",
                    c.w, c.h, nc, area ? 100.0 * (double)(area - kept) / (double)area : 0.0, (unsigned long long)visBand,
                    (unsigned long long)kept, (unsigned long long)area);
        // no mask: the bands as they are
        OutsideAa::VisRows none;
        Box same[4 * OutsideAa::kClipStrips];
        const int ns = OutsideAa::SplitClip(none, bands, nb, same, nullptr, nullptr);
        bool eq = ns == nb;
        for (int i = 0; i < ns && eq; ++i)
            eq = same[i].x0 == bands[i].x0 && same[i].y0 == bands[i].y0 && same[i].x1 == bands[i].x1 && same[i].y1 == bands[i].y1;
        CHECK(eq, "%ux%u: no mask must leave the bands unchanged", c.w, c.h);
    }
    // degenerate input: nothing usable -> no table (the caller draws the bands whole)
    OutsideAa::VisRows v0;
    CHECK(!OutsideAa::BuildVisRows(nullptr, 0, 100, 100, 8, &v0) && v0.w == 0, "BuildVisRows without triangles");
}

// ---- Part B (WARP) -----------------------------------------------------------------------------------------------------------
static const char* kSceneHlsl = R"(
cbuffer C : register(b0) { float2 size; float kind; float pad; };
void VS(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 t : TEXCOORD0) {
    const float2 u = float2((id << 1) & 2, id & 2);
    pos = float4(u.x * 2.0 - 1.0, 1.0 - u.y * 2.0, 0.0, 1.0);
    t = float2((pos.x + 1.0) * 0.5, (1.0 - pos.y) * 0.5) * size;      // scene position (px of the unshifted W x H frame)
}
float F(float2 t) {
    if (kind < 0.5) {
        const float k = 6.28318530718;
        return 0.5 + 0.2 * sin(k * t.x / 9.7 + 0.3) + 0.15 * sin(k * t.y / 13.1) + 0.1 * sin(k * (t.x + t.y) / 17.3);
    }
    const float d = abs(dot(t - float2(40.0, 30.0), normalize(float2(1.0, -0.21))));   // a thin, nearly vertical line
    const float d2 = abs(dot(t - float2(20.0, 50.0), normalize(float2(0.17, 1.0))));   // a thin, nearly horizontal line
    return (d < 0.35 || d2 < 0.3) ? 0.1 : 0.9;
}
float4 PS(float4 pos : SV_Position, float2 t : TEXCOORD0) : SV_Target { const float v = F(t); return float4(v, v, v, 1.0); }
)";
static float SceneF(float tx, float ty) {             // C++ twin of F (kind 0)
    const float k = 6.28318530718f;
    return 0.5f + 0.2f * std::sin(k * tx / 9.7f + 0.3f) + 0.15f * std::sin(k * ty / 13.1f) + 0.1f * std::sin(k * (tx + ty) / 17.3f);
}

static float Halton(int i, int b) { float f = 1.0f, r = 0.0f; while (i > 0) { f /= (float)b; r += f * (float)(i % b); i /= b; } return r; }

struct Gpu {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11VertexShader* sceneVs = nullptr;
    ID3D11PixelShader* scenePs = nullptr;
    ID3D11ComputeShader* oaaCs = nullptr;
    ID3D11PixelShader* oaaPs = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11Buffer* params = nullptr;
    ID3D11ShaderResourceView* paramsSrv = nullptr;
    ID3D11SamplerState* lin = nullptr;
    ID3D11RasterizerState* rs = nullptr;
};

static ID3DBlob* Compile(const char* src, const char* entry, const char* target, UINT flags) {
    ID3DBlob* b = nullptr;
    ID3DBlob* e = nullptr;
    if (FAILED(D3DCompile(src, std::strlen(src), "outside_aa_test", nullptr, nullptr, entry, target, flags, 0, &b, &e))) {
        std::printf("compile %s: %s\n", entry, e ? (const char*)e->GetBufferPointer() : "?");
        std::exit(2);
    }
    return b;
}

struct Tex {
    ID3D11Texture2D* t = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
    void Release() { if (uav) uav->Release(); if (srv) srv->Release(); if (rtv) rtv->Release(); if (t) t->Release(); *this = Tex(); }
};

static Tex MakeTex(Gpu& g, UINT w, UINT h, DXGI_FORMAT f, bool uav) {
    Tex x; x.w = w; x.h = h; x.f = f;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = f; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
    if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &x.t))) { std::printf("texture create failed\n"); std::exit(2); }
    g.dev->CreateRenderTargetView(x.t, nullptr, &x.rtv);
    g.dev->CreateShaderResourceView(x.t, nullptr, &x.srv);
    if (uav) g.dev->CreateUnorderedAccessView(x.t, nullptr, &x.uav);
    return x;
}

static float HalfToFloat(uint16_t h) {
    const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp((float)(m | 1024u), (int)e - 25);
    return s ? -v : v;
}

static std::vector<float> ReadR(Gpu& g, const Tex& x) {          // red channel as float
    D3D11_TEXTURE2D_DESC td{};
    x.t->GetDesc(&td);
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    g.dev->CreateTexture2D(&td, nullptr, &st);
    g.ctx->CopyResource(st, x.t);
    D3D11_MAPPED_SUBRESOURCE mp{};
    g.ctx->Map(st, 0, D3D11_MAP_READ, 0, &mp);
    std::vector<float> out((size_t)x.w * x.h);
    for (UINT y = 0; y < x.h; ++y) {
        const uint8_t* row = (const uint8_t*)mp.pData + (size_t)y * mp.RowPitch;
        for (UINT i = 0; i < x.w; ++i)
            out[(size_t)y * x.w + i] = x.f == DXGI_FORMAT_R16G16B16A16_FLOAT ? HalfToFloat(((const uint16_t*)row)[i * 4])
                                                                               : (float)row[i * 4] / 255.0f;
    }
    g.ctx->Unmap(st, 0);
    st->Release();
    return out;
}

// The mod's jittered world pass: viewport 0 = (s, (W+1) x (H+1)) (jitter_overscan 1).
static void RenderRaw(Gpu& g, const Tex& rt, float sx, float sy, int kind) {
    const float c[4] = { (float)rt.w, (float)rt.h, (float)kind, 0.0f };
    g.ctx->UpdateSubresource(g.cb, 0, nullptr, c, 0, 0);
    D3D11_VIEWPORT vp{ sx, sy, (float)rt.w + 1.0f, (float)rt.h + 1.0f, 0.0f, 1.0f };
    g.ctx->OMSetRenderTargets(1, &rt.rtv, nullptr);
    g.ctx->RSSetViewports(1, &vp);
    g.ctx->RSSetState(g.rs);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(g.sceneVs, nullptr, 0);
    g.ctx->VSSetConstantBuffers(0, 1, &g.cb);
    g.ctx->PSSetShader(g.scenePs, nullptr, 0);
    g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
    g.ctx->Draw(3, 0);
    ID3D11RenderTargetView* none = nullptr;
    g.ctx->OMSetRenderTargets(1, &none, nullptr);
}

static void SetParams(Gpu& g, const float* p) {
    D3D11_MAPPED_SUBRESOURCE mp{};
    g.ctx->Map(g.params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp);
    std::memcpy(mp.pData, p, sizeof(float) * OutsideAa::kParamFloats);
    g.ctx->Unmap(g.params, 0);
}

static void RunCs(Gpu& g, const Tex& src, const Tex& dst, const float* p) {
    SetParams(g, p);
    const float zero[4] = { 0, 0, 0, 0 };
    g.ctx->ClearUnorderedAccessViewFloat(dst.uav, zero);           // sentinel 0 (every scene value is >= 0.05)
    ID3D11ShaderResourceView* srv[2] = { src.srv, g.paramsSrv };
    g.ctx->CSSetShader(g.oaaCs, nullptr, 0);
    g.ctx->CSSetShaderResources(0, 2, srv);
    g.ctx->CSSetUnorderedAccessViews(0, 1, &dst.uav, nullptr);
    g.ctx->CSSetSamplers(0, 1, &g.lin);
    g.ctx->Dispatch((dst.w + 7) / 8, (dst.h + 7) / 8, 1);
    ID3D11ShaderResourceView* ns[2] = {};
    ID3D11UnorderedAccessView* nu = nullptr;
    g.ctx->CSSetShaderResources(0, 2, ns);
    g.ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
}

// scene_dlaa.cpp's upscale order: one Draw(3) per band, viewport = the band offset by the output origin.
static void RunPs(Gpu& g, const Tex& src, const Tex& rt, const float* p, const Box* bands, int nb, float ox, float oy) {
    SetParams(g, p);
    const float zero[4] = { 0, 0, 0, 0 };
    g.ctx->ClearRenderTargetView(rt.rtv, zero);
    g.ctx->OMSetRenderTargets(1, &rt.rtv, nullptr);
    g.ctx->RSSetState(g.rs);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->VSSetShader(g.sceneVs, nullptr, 0);                      // (its TEXCOORD is unused by PSMain)
    g.ctx->VSSetConstantBuffers(0, 1, &g.cb);
    g.ctx->PSSetShader(g.oaaPs, nullptr, 0);
    ID3D11ShaderResourceView* srv[2] = { src.srv, g.paramsSrv };
    g.ctx->PSSetShaderResources(0, 2, srv);
    g.ctx->PSSetSamplers(0, 1, &g.lin);
    for (int b = 0; b < nb; ++b) {
        D3D11_VIEWPORT vp{ ox + (float)bands[b].x0, oy + (float)bands[b].y0, (float)(bands[b].x1 - bands[b].x0),
                           (float)(bands[b].y1 - bands[b].y0), 0.0f, 1.0f };
        g.ctx->RSSetViewports(1, &vp);
        g.ctx->Draw(3, 0);
    }
    ID3D11ShaderResourceView* ns[2] = {};
    g.ctx->PSSetShaderResources(0, 2, ns);
    ID3D11RenderTargetView* none = nullptr;
    g.ctx->OMSetRenderTargets(1, &none, nullptr);
}

// phase 93 B5: CSMain with a dispatch origin (Params[4].zw, the in-place visibility clip) writes exactly the pixels of the
// full-image dispatch inside the dispatched box (bit for bit) and nothing outside it.
static void CsOrigin(Gpu& g) {
    const UINT W = 97, H = 83;
    Tex raw = MakeTex(g, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, false), a = MakeTex(g, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true),
        b = MakeTex(g, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    RenderRaw(g, raw, 0.31f, -0.22f, 0);
    const bool e[4] = { true, true, true, true };
    const Box in = OutsideAa::Interior(Box{ 25, 20, 70, 60 }, 4.0f, 4.0f, e);
    float p[OutsideAa::kParamFloats];
    OutsideAa::FillParams(p, in, W, H, W, H, 0.31f, -0.22f, 2, 2, false, 0.0f, 0.0f);
    RunCs(g, raw, a, p);                                        // the whole image (origin 0)
    const std::vector<float> fa = ReadR(g, a);
    const UINT ox = 13, oy = 9, gx = 6, gy = 5;                 // a box (13, 9) .. (61, 49), partly over the interior
    p[18] = (float)ox; p[19] = (float)oy;
    SetParams(g, p);
    const float zero[4] = { 0, 0, 0, 0 };
    g.ctx->ClearUnorderedAccessViewFloat(b.uav, zero);
    ID3D11ShaderResourceView* srv[2] = { raw.srv, g.paramsSrv };
    g.ctx->CSSetShader(g.oaaCs, nullptr, 0);
    g.ctx->CSSetShaderResources(0, 2, srv);
    g.ctx->CSSetUnorderedAccessViews(0, 1, &b.uav, nullptr);
    g.ctx->CSSetSamplers(0, 1, &g.lin);
    g.ctx->Dispatch(gx, gy, 1);
    ID3D11ShaderResourceView* ns[2] = {};
    ID3D11UnorderedAccessView* nu = nullptr;
    g.ctx->CSSetShaderResources(0, 2, ns);
    g.ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
    const std::vector<float> fb = ReadR(g, b);
    int diff = 0, stray = 0, written = 0;
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x) {
            const bool inBox = x >= ox && x < ox + gx * 8 && y >= oy && y < oy + gy * 8;
            const float va = fa[(size_t)y * W + x], vb = fb[(size_t)y * W + x];
            if (inBox) { if (std::memcmp(&va, &vb, sizeof(float)) != 0) ++diff; if (vb != 0.0f) ++written; }
            else if (vb != 0.0f) ++stray;
        }
    CHECK(diff == 0 && stray == 0 && written > 0, "CS origin: %d pixels differ from the full dispatch inside the box, %d written "
          "outside it, %d written inside", diff, stray, written);
    std::printf("B5 CS origin: %d pixels written inside the box, identical to the full dispatch\n", written);
    raw.Release(); a.Release(); b.Release();
}

static void Phase(int i, float* sx, float* sy) { *sx = Halton(i + 1, 2) - 0.5f; *sy = Halton(i + 1, 3) - 0.5f; }

// B1 / B2: in place. Errors against the shift-0 frame over the outside pixels (3 px from the image border).
// selfRef: the reference is the SAME mode run on the shift-0 frame (edge AA blurs smooth texture by design: what must hold is
// that the result does not move with the phase), else the shift-0 frame itself.
static void InPlace(Gpu& g, DXGI_FORMAT fmt, int mode, int luma, const char* name, double maxMeanRatio, double maxAbs,
                    bool selfRef = false) {
    const UINT W = 96, H = 80;
    const Box rect{ 24, 20, 72, 60 };
    const bool e[4] = { true, true, true, true };
    const Box in = OutsideAa::Interior(rect, 6.0f, 6.0f, e);
    Tex raw0 = MakeTex(g, W, H, fmt, false), raw = MakeTex(g, W, H, fmt, false), dst = MakeTex(g, W, H, fmt, true);
    RenderRaw(g, raw0, 0.0f, 0.0f, 0);
    std::vector<float> r0 = ReadR(g, raw0);
    const std::vector<float> rawRef = r0;
    if (selfRef) {
        float p0[OutsideAa::kParamFloats];
        OutsideAa::FillParams(p0, in, W, H, W, H, 0.0f, 0.0f, mode, luma, false, 0.0f, 0.0f);
        RunCs(g, raw0, dst, p0);
        const std::vector<float> o0 = ReadR(g, dst);
        for (size_t i = 0; i < r0.size(); ++i) if (o0[i] != 0.0f) r0[i] = o0[i];   // interior keeps the frame (never compared)
    }
    double worst = 0.0;
    for (int ph = 0; ph < 8; ++ph) {
        float sx, sy;
        Phase(ph, &sx, &sy);
        RenderRaw(g, raw, sx, sy, 0);
        const std::vector<float> rs = ReadR(g, raw);
        const int sign = (ph & 1) ? -1 : 1;                         // the NGX jitter as inject.cpp makes it, both signs
        const float ngxX = (float)sign * sx, ngxY = (float)sign * sy;
        float p[OutsideAa::kParamFloats];
        OutsideAa::FillParams(p, in, W, H, W, H, OutsideAa::ShiftFromNgx(ngxX, sign), OutsideAa::ShiftFromNgx(ngxY, sign), mode, luma,
                              false, 0.0f, 0.0f);
        RunCs(g, raw, dst, p);
        const std::vector<float> o = ReadR(g, dst);
        OutsideAa::FillParams(p, in, W, H, W, H, -sx, -sy, mode, luma, false, 0.0f, 0.0f);   // MUTATION: the opposite sign
        RunCs(g, raw, dst, p);
        const std::vector<float> om = ReadR(g, dst);
        double eRaw = 0.0, eOaa = 0.0, eMut = 0.0, mx = 0.0;
        int n = 0, interiorWritten = 0;
        for (UINT y = 0; y < H; ++y)
            for (UINT x = 0; x < W; ++x) {
                const size_t k = (size_t)y * W + x;
                if (In(in, (int)x, (int)y)) { if (o[k] != 0.0f) ++interiorWritten; continue; }
                if (x < 3 || y < 3 || x >= W - 3 || y >= H - 3) continue;
                eRaw += std::fabs(rs[k] - rawRef[k]); eOaa += std::fabs(o[k] - r0[k]); eMut += std::fabs(om[k] - r0[k]);
                mx = std::fmax(mx, std::fabs(o[k] - r0[k]));
                ++n;
            }
        eRaw /= n; eOaa /= n; eMut /= n;
        std::printf("  %s phase %d shift (%+.4f, %+.4f) sign %+d: mean |raw - ref| %.4f, |outside AA - ref| %.4f (max %.4f), "
                    "opposite sign %.4f\n", name, ph, sx, sy, sign, eRaw, eOaa, mx, eMut);
        CHECK(interiorWritten == 0, "%s: %d interior pixels written", name, interiorWritten);
        if (eRaw > 0.004) {
            CHECK(eOaa < maxMeanRatio * eRaw, "%s phase %d: outside AA error %.4f not < %.2f x raw %.4f", name, ph, eOaa, maxMeanRatio, eRaw);
            CHECK(eMut > eRaw, "%s phase %d: the OPPOSITE sign (%.4f) is not worse than raw (%.4f) -- the test cannot see a sign error",
                  name, ph, eMut, eRaw);
        }
        CHECK(mx < maxAbs, "%s phase %d: max error %.4f >= %.4f", name, ph, mx, maxAbs);
        worst = std::fmax(worst, eRaw > 0.0 ? eOaa / eRaw : 0.0);
    }
    std::printf("%s: worst mean error ratio outside AA / raw = %.3f\n", name, worst);
    raw0.Release(); raw.Release(); dst.Release();
}

// B3: per-pixel spread (max - min over the 8 phases) of the thin-line scene, outside pixels. Raw vs mode 1 vs mode 2.
static void ThinLines(Gpu& g) {
    const UINT W = 96, H = 80;
    const Box rect{ 60, 50, 90, 76 };                             // the lines run outside it
    const bool e[4] = { true, true, true, true };
    const Box in = OutsideAa::Interior(rect, 4.0f, 4.0f, e);
    Tex raw = MakeTex(g, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, false), dst = MakeTex(g, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, true);
    std::vector<float> mn[3], mxv[3];
    for (int k = 0; k < 3; ++k) { mn[k].assign((size_t)W * H, 9.0f); mxv[k].assign((size_t)W * H, -9.0f); }
    for (int ph = 0; ph < 8; ++ph) {
        float sx, sy;
        Phase(ph, &sx, &sy);
        RenderRaw(g, raw, sx, sy, 1);
        std::vector<float> v[3];
        v[0] = ReadR(g, raw);
        for (int mode = 1; mode <= 2; ++mode) {
            float p[OutsideAa::kParamFloats];
            OutsideAa::FillParams(p, in, W, H, W, H, sx, sy, mode, 0, false, 0.0f, 0.0f);
            RunCs(g, raw, dst, p);
            v[mode] = ReadR(g, dst);
        }
        for (int k = 0; k < 3; ++k)
            for (size_t i = 0; i < v[k].size(); ++i) { mn[k][i] = std::fmin(mn[k][i], v[k][i]); mxv[k][i] = std::fmax(mxv[k][i], v[k][i]); }
    }
    double spread[3] = {};
    int n = 0;
    for (UINT y = 3; y < H - 3; ++y)
        for (UINT x = 3; x < W - 3; ++x) {
            if (In(in, (int)x, (int)y)) continue;
            const size_t i = (size_t)y * W + x;
            bool flat = true;
            for (int k = 0; k < 3; ++k) flat = flat && mxv[k][i] - mn[k][i] < 0.01f;
            if (flat) continue;                                    // flat sky
            for (int k = 0; k < 3; ++k) spread[k] += mxv[k][i] - mn[k][i];
            ++n;
        }
    for (int k = 0; k < 3; ++k) spread[k] /= (n ? n : 1);
    std::printf("thin lines (aliased, %d pixels near the lines): mean spread over the 8 phases -- raw %.4f, mode 1 (unjitter) %.4f, "
                "mode 2 (+ edge AA) %.4f\n", n, spread[0], spread[1], spread[2]);
    CHECK(spread[2] < spread[0], "thin lines: mode 2 (%.4f) not steadier than raw (%.4f)", spread[2], spread[0]);
    // Unjitter alone cannot steady a SUB-PIXEL line: which pixels it covers changes with the phase (aliasing), and resampling
    // moves that pattern back but does not fill it in -- measured here ~8 % above raw. It must at least not be much worse.
    CHECK(spread[1] < 1.2 * spread[0], "thin lines: mode 1 (%.4f) much less steady than raw (%.4f)", spread[1], spread[0]);
    raw.Release(); dst.Release();
}

// B4: upscale. Render 72x80 -> output 96x80, drawn at origin (8, 4) of a 107x86 RT.
static void Upscale(Gpu& g) {
    const UINT RW = 72, RH = 80, OW = 96, OH = 80, OX = 8, OY = 4;
    const Box rect{ 24, 20, 72, 60 };                              // output px
    const bool e[4] = { true, true, true, true };
    const float fx = 6.0f * (float)OW / (float)RW, fy = 6.0f;      // dlaa_area_feather scaled to output px per axis (Composite)
    const Box in = OutsideAa::Interior(rect, fx, fy, e);
    Box bands[4];
    const int nb = OutsideAa::Bands(OW, OH, in, bands);
    Tex raw = MakeTex(g, RW, RH, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
    Tex rt = MakeTex(g, OW + OX + 3, OH + OY + 2, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
    std::vector<float> ref((size_t)OW * OH);
    for (UINT y = 0; y < OH; ++y)
        for (UINT x = 0; x < OW; ++x) {
            const float X = ((float)x + 0.5f) * (float)RW / (float)OW, Y = ((float)y + 0.5f) * (float)RH / (float)OH;   // render px
            ref[(size_t)y * OW + x] = SceneF(X * (float)RW / (float)(RW + 1), Y * (float)RH / (float)(RH + 1));      // overscan scale
        }
    double worst = 0.0;
    for (int ph = 0; ph < 8; ++ph) {
        float sx, sy;
        Phase(ph, &sx, &sy);
        RenderRaw(g, raw, sx, sy, 0);
        const std::vector<float> rs = ReadR(g, raw);
        float p[OutsideAa::kParamFloats];
        OutsideAa::FillParams(p, in, OW, OH, RW, RH, sx, sy, 1, 2, false, (float)OX, (float)OY);
        RunPs(g, raw, rt, p, bands, nb, (float)OX, (float)OY);
        const std::vector<float> o = ReadR(g, rt);
        OutsideAa::FillParams(p, in, OW, OH, RW, RH, -sx, -sy, 1, 2, false, (float)OX, (float)OY);   // MUTATION
        RunPs(g, raw, rt, p, bands, nb, (float)OX, (float)OY);
        const std::vector<float> om = ReadR(g, rt);
        double eBlit = 0.0, eOaa = 0.0, eMut = 0.0, mx = 0.0;
        int n = 0, interiorWritten = 0, marginWritten = 0, bandMissing = 0;
        for (UINT y = 0; y < rt.h; ++y)
            for (UINT x = 0; x < rt.w; ++x) {
                const float v = o[(size_t)y * rt.w + x];
                const int ox = (int)x - (int)OX, oy = (int)y - (int)OY;
                if (ox < 0 || oy < 0 || ox >= (int)OW || oy >= (int)OH) { if (v != 0.0f) ++marginWritten; continue; }
                if (In(in, ox, oy)) { if (v != 0.0f) ++interiorWritten; continue; }
                if (v == 0.0f) ++bandMissing;
                if (ox < 4 || oy < 3 || ox >= (int)OW - 4 || oy >= (int)OH - 3) continue;
                // the game's blit: bilinear of the jittered frame at uv = o_c / output
                const float X = ((float)ox + 0.5f) * (float)RW / (float)OW - 0.5f, Y = ((float)oy + 0.5f) * (float)RH / (float)OH - 0.5f;
                const int x0 = (int)std::floor(X), y0 = (int)std::floor(Y);
                const float ax = X - (float)x0, ay = Y - (float)y0;
                auto at = [&](int xx, int yy) { xx = xx < 0 ? 0 : (xx >= (int)RW ? (int)RW - 1 : xx); yy = yy < 0 ? 0 : (yy >= (int)RH ? (int)RH - 1 : yy);
                                                 return rs[(size_t)yy * RW + xx]; };
                const float blit = (at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax) * (1 - ay) + (at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax) * ay;
                const float r = ref[(size_t)oy * OW + ox];
                eBlit += std::fabs(blit - r); eOaa += std::fabs(v - r); eMut += std::fabs(om[(size_t)y * rt.w + x] - r);
                mx = std::fmax(mx, std::fabs(v - r));
                ++n;
            }
        eBlit /= n; eOaa /= n; eMut /= n;
        std::printf("  upscale phase %d shift (%+.4f, %+.4f): mean |game blit - ref| %.4f, |outside AA - ref| %.4f (max %.4f), "
                    "opposite sign %.4f\n", ph, sx, sy, eBlit, eOaa, mx, eMut);
        CHECK(interiorWritten == 0 && marginWritten == 0 && bandMissing == 0, "upscale phase %d: interior %d / margin %d written, "
              "%d band pixels missing", ph, interiorWritten, marginWritten, bandMissing);
        CHECK(eOaa < 0.25 * eBlit, "upscale phase %d: outside AA %.4f not < 0.25 x the game's stretch %.4f", ph, eOaa, eBlit);
        CHECK(mx < 0.03, "upscale phase %d: max error %.4f", ph, mx);
        CHECK(eMut > 2.0 * eOaa + 0.002, "upscale phase %d: the opposite sign (%.4f) is not clearly worse (%.4f)", ph, eMut, eOaa);
        worst = std::fmax(worst, eOaa);
    }
    std::printf("upscale: worst mean error %.4f\n", worst);
    raw.Release(); rt.Release();
}

int main() {
    PartA();
    PartC();                                                         // phase 93
    Gpu g;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx))) {
        std::printf("WARP device failed\n");
        return 2;
    }
    const UINT o3 = D3DCOMPILE_OPTIMIZATION_LEVEL3;                  // the runtime / fxc /O3 flags
    ID3DBlob* b = Compile(kSceneHlsl, "VS", "vs_5_0", 0);
    g.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g.sceneVs); b->Release();
    b = Compile(kSceneHlsl, "PS", "ps_5_0", 0);
    g.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g.scenePs); b->Release();
    b = Compile(OutsideAa::kOutsideAaShader, "CSMain", "cs_5_0", o3);
    g.dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g.oaaCs); b->Release();
    b = Compile(OutsideAa::kOutsideAaShader, "PSMain", "ps_5_0", o3);
    g.dev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g.oaaPs); b->Release();
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g.dev->CreateBuffer(&bd, nullptr, &g.cb);
    bd.ByteWidth = sizeof(float) * OutsideAa::kParamFloats; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g.dev->CreateBuffer(&bd, nullptr, &g.params);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.FirstElement = 0; sd.Buffer.NumElements = OutsideAa::kParamFloats / 4;
    g.dev->CreateShaderResourceView(g.params, &sd, &g.paramsSrv);
    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.ComparisonFunc = D3D11_COMPARISON_NEVER; smp.MaxLOD = D3D11_FLOAT32_MAX;
    g.dev->CreateSamplerState(&smp, &g.lin);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    g.dev->CreateRasterizerState(&rd, &g.rs);

    InPlace(g, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 2, "B1 in place RGBA16F mode 1", 0.15, 0.03);
    InPlace(g, DXGI_FORMAT_R8G8B8A8_UNORM, 2, 0, "B2 in place RGBA8 mode 2 (vs mode 2 at shift 0)", 0.35, 0.08, true);
    ThinLines(g);
    Upscale(g);
    CsOrigin(g);                                                     // phase 93

    std::printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "all passed", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
