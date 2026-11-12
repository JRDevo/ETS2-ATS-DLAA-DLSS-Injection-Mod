// phase 78 unit test (WARP, no GPU needed): the lighting pass's FULL-SCREEN draws with the G-buffer's jitter shift
// (src/light_fs.h, inject.cpp hkDraw -> LightFsEdgeFill, dlaa.ini light_fs_jitter).
//
// The game's deferred sun light is a full-screen Draw(3): it fetches the G-buffer with `ld` at ftoi(uv * size) and rebuilds the
// position from uv. The mod now issues it with viewport 0 = the game's W x H shifted by the pass's jitter s, no overscan, then
// re-draws the far column / row a negative shift loses with the game's own viewport + a scissor (LightFs::EdgeRects). This test
// runs exactly that sequence on WARP with a full-screen triangle whose PS does what the game's does (point fetch at ftoi(uv * W),
// additive blend ONE / ONE, writes the texel it read + 1 + uv * size) and checks, for the 8 Halton phases and edge shifts:
//   1. every pixel written EXACTLY ONCE (additive count 1: no hole, no double light at the re-drawn edge)
//   2. every pixel read ITS OWN G-buffer texel (ftoi(uv * W) = p for |s| < 0.5)
//   3. uv * size = p + 0.5 - s in the shifted area (the ray matches the jittered G-buffer texel), p + 0.5 at the re-drawn edge
// and two mutations that must FAIL the checks (proof the test sees the defects the design avoids):
//   M1 no edge fill: a negative shift leaves the last column / row unwritten (the phase-23 edge rule, on WARP)
//   M2 the (W+1) x (H+1) overscan viewport (phase 23's world viewport): wrong texel toward the far edge
// plus LightFs::EdgeRects / Clip on fixed cases. Exit code 0 = all passed.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "light_fs.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const char* kHlsl = R"(
Texture2D<uint> g : register(t0);
cbuffer C : register(b0) { float2 size; float2 pad; };
void VS(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 PS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    int2 t = int2(uv * size);                      // ftoi(uv * size), as the game's R188
    uint v = g.Load(int3(t, 0));
    return float4((float)v, 1, uv * size);
}
)";

struct Gpu {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11BlendState* add = nullptr;
    ID3D11RasterizerState* rsGame = nullptr;       // the "game's" state: no scissor
    ID3D11RasterizerState* rsTwin = nullptr;       // the mod's scissor twin of it
};

static ID3DBlob* Compile(const char* entry, const char* target) {
    ID3DBlob* b = nullptr;
    ID3DBlob* e = nullptr;
    if (FAILED(D3DCompile(kHlsl, std::strlen(kHlsl), "light_fs_test", nullptr, nullptr, entry, target, 0, 0, &b, &e))) {
        std::printf("compile %s: %s\n", entry, e ? (const char*)e->GetBufferPointer() : "?");
        std::exit(2);
    }
    if (e) e->Release();
    return b;
}

static bool Init(Gpu& g) {
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx)))
        return false;
    ID3DBlob* v = Compile("VS", "vs_5_0");
    ID3DBlob* p = Compile("PS", "ps_5_0");
    g.dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &g.vs);
    g.dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &g.ps);
    v->Release(); p->Release();
    D3D11_BUFFER_DESC bd{ 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    g.dev->CreateBuffer(&bd, nullptr, &g.cb);
    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = bl.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].SrcBlendAlpha = bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOp = bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g.dev->CreateBlendState(&bl, &g.add);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    g.dev->CreateRasterizerState(&rd, &g.rsGame);
    rd.ScissorEnable = TRUE;                       // = inject.cpp LightFsScissorRs
    g.dev->CreateRasterizerState(&rd, &g.rsTwin);
    return g.vs && g.ps && g.cb && g.add && g.rsGame && g.rsTwin;
}

enum Mode { kMod, kNoFill, kOverscan };

// One full-screen draw as the mod issues it; returns the RGBA32F result (x = texel read, y = count, zw = uv * size).
static std::vector<float> Run(Gpu& g, int W, int H, float sx, float sy, Mode mode, int* edgeDraws) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R32_UINT; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<UINT> ids((size_t)W * H);
    for (int i = 0; i < W * H; ++i) ids[i] = (UINT)i;
    D3D11_SUBRESOURCE_DATA sd{ ids.data(), (UINT)W * 4, 0 };
    ID3D11Texture2D* gbuf = nullptr;
    g.dev->CreateTexture2D(&td, &sd, &gbuf);
    ID3D11ShaderResourceView* srv = nullptr;
    g.dev->CreateShaderResourceView(gbuf, nullptr, &srv);
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* rt = nullptr;
    g.dev->CreateTexture2D(&td, nullptr, &rt);
    ID3D11RenderTargetView* rtv = nullptr;
    g.dev->CreateRenderTargetView(rt, nullptr, &rtv);
    const float zero[4] = { 0, 0, 0, 0 };
    g.ctx->ClearRenderTargetView(rtv, zero);
    const float cbv[4] = { (float)W, (float)H, 0, 0 };
    g.ctx->UpdateSubresource(g.cb, 0, nullptr, cbv, 0, 0);
    g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g.ctx->OMSetBlendState(g.add, nullptr, 0xffffffff);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetShader(g.vs, nullptr, 0);
    g.ctx->PSSetShader(g.ps, nullptr, 0);
    g.ctx->PSSetShaderResources(0, 1, &srv);
    g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
    g.ctx->RSSetState(g.rsGame);
    const D3D11_VIEWPORT game{ 0, 0, (float)W, (float)H, 0, 1 };
    D3D11_VIEWPORT v = game;
    v.TopLeftX += sx; v.TopLeftY += sy;
    if (mode == kOverscan) { v.Width += 1; v.Height += 1; }
    g.ctx->RSSetViewports(1, &v);
    g.ctx->Draw(3, 0);
    *edgeDraws = 0;
    if (mode == kMod) {                            // = inject.cpp LightFsEdgeFill
        D3D11_RECT r[2];
        const int n = LightFs::EdgeRects(game, sx, sy, r);
        if (n) {
            g.ctx->RSSetViewports(1, &game);
            g.ctx->RSSetState(g.rsTwin);
            for (int i = 0; i < n; ++i) { g.ctx->RSSetScissorRects(1, &r[i]); g.ctx->Draw(3, 0); ++*edgeDraws; }
            g.ctx->RSSetState(g.rsGame);
        }
    }
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    g.dev->CreateTexture2D(&td, nullptr, &st);
    g.ctx->CopyResource(st, rt);
    std::vector<float> out((size_t)W * H * 4);
    D3D11_MAPPED_SUBRESOURCE m{};
    g.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
    for (int y = 0; y < H; ++y) std::memcpy(&out[(size_t)y * W * 4], (const char*)m.pData + (size_t)y * m.RowPitch, (size_t)W * 16);
    g.ctx->Unmap(st, 0);
    ID3D11ShaderResourceView* nul = nullptr;
    g.ctx->PSSetShaderResources(0, 1, &nul);
    g.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    st->Release(); rtv->Release(); rt->Release(); srv->Release(); gbuf->Release();
    return out;
}

struct Res { int holes = 0, doubles = 0, wrongTexel = 0, badUv = 0, edgePx = 0; };

static Res Check(const std::vector<float>& o, int W, int H, float sx, float sy, bool modEdge) {
    Res r;
    D3D11_RECT e[2];
    const D3D11_VIEWPORT game{ 0, 0, (float)W, (float)H, 0, 1 };
    const int ne = modEdge ? LightFs::EdgeRects(game, sx, sy, e) : 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* p = &o[((size_t)y * W + x) * 4];
            if (p[1] == 0.0f) { ++r.holes; continue; }
            if (p[1] != 1.0f) { ++r.doubles; continue; }
            if (p[0] != (float)(y * W + x)) ++r.wrongTexel;
            bool edge = false;
            for (int i = 0; i < ne; ++i) edge |= x >= e[i].left && x < e[i].right && y >= e[i].top && y < e[i].bottom;
            r.edgePx += edge;
            const float ex = x + 0.5f - (edge ? 0.0f : sx), ey = y + 0.5f - (edge ? 0.0f : sy);
            if (std::fabs(p[2] - ex) > 2e-3f || std::fabs(p[3] - ey) > 2e-3f) ++r.badUv;
        }
    return r;
}

static float Halton(int i, int b) {
    float f = 1, r = 0;
    for (; i > 0; i /= b) { f /= b; r += f * (i % b); }
    return r;
}

int main() {
    // ---- LightFs::EdgeRects / Clip, fixed cases ----
    {
        const D3D11_VIEWPORT gv{ 0, 0, 64, 48, 0, 1 };
        D3D11_RECT r[2];
        CHECK(LightFs::EdgeRects(gv, 0.0f, 0.0f, r) == 0, "no shift: no edge");
        CHECK(LightFs::EdgeRects(gv, 0.49f, 0.25f, r) == 0, "positive shifts: no edge");
        CHECK(LightFs::EdgeRects(gv, -0.001f, 0.3f, r) == 1 && r[0].left == 63 && r[0].right == 64 && r[0].top == 0 &&
              r[0].bottom == 48, "x < 0: last column, full height");
        CHECK(LightFs::EdgeRects(gv, 0.2f, -0.4f, r) == 1 && r[0].left == 0 && r[0].right == 64 && r[0].top == 47 &&
              r[0].bottom == 48, "y < 0: last row, full width");
        CHECK(LightFs::EdgeRects(gv, -0.3f, -0.3f, r) == 2 && r[0].left == 63 && r[1].left == 0 && r[1].right == 63 &&
              r[1].top == 47, "both < 0: column + row left of it (corner once)");
        const D3D11_VIEWPORT off{ 100, 20, 64, 48, 0, 1 };
        CHECK(LightFs::EdgeRects(off, -0.2f, -0.2f, r) == 2 && r[0].left == 163 && r[0].top == 20 && r[0].bottom == 68 &&
              r[1].left == 100 && r[1].right == 163 && r[1].top == 67, "offset viewport");
        D3D11_RECT c{ 63, 0, 64, 48 };
        const D3D11_RECT sc1{ 0, 10, 64, 20 }, sc2{ 0, 0, 60, 48 };
        CHECK(LightFs::Clip(c, sc1) && c.top == 10 && c.bottom == 20, "clip to the game's scissor");
        D3D11_RECT c2{ 63, 0, 64, 48 };
        CHECK(!LightFs::Clip(c2, sc2), "clip: empty");
    }
    Gpu g;
    if (!Init(g)) { std::printf("WARP device / state creation failed\n"); return 2; }
    // the 8 Halton(2, 3) phases - 0.5 (the mod's jitter_phases = 8: |s| <= 0.4375) + edge shifts. Limit: the rasterizer snaps the
    // viewport / vertices to 1/256 px (D3D11 8-bit sub-pixel precision), so |s| >= 0.5 - 1/512 rounds to 0.5 and ftoi(uv * W)
    // reads the neighbour (measured: +-0.499 fails on WARP); Halton(2 / 3) - 0.5 stays inside up to 128 phases.
    std::vector<std::pair<float, float>> sh;
    for (int i = 1; i <= 8; ++i) sh.push_back({ Halton(i, 2) - 0.5f, Halton(i, 3) - 0.5f });
    sh.push_back({ -0.49f, -0.49f }); sh.push_back({ 0.49f, 0.49f }); sh.push_back({ -0.001f, 0.0f });
    sh.push_back({ 0.0f, -0.001f }); sh.push_back({ 0.0f, 0.0f });
    const int sizes[][2] = { { 64, 48 }, { 37, 29 }, { 320, 240 } };
    int m1Holes = 0, m2Wrong = 0, edgeDraws = 0, runs = 0;
    for (const auto& s : sizes) {
        const int W = s[0], H = s[1];
        for (const auto& j : sh) {
            int ed = 0;
            const Res r = Check(Run(g, W, H, j.first, j.second, kMod, &ed), W, H, j.first, j.second, true);
            edgeDraws += ed; ++runs;
            CHECK(r.holes == 0 && r.doubles == 0 && r.wrongTexel == 0 && r.badUv == 0,
                  "%dx%d shift (%+.4f,%+.4f): holes %d doubles %d wrong texel %d bad uv %d (edge px %d)", W, H, j.first, j.second,
                  r.holes, r.doubles, r.wrongTexel, r.badUv, r.edgePx);
            const int expEdge = (j.first < 0 ? H : 0) + (j.second < 0 ? (j.first < 0 ? W - 1 : W) : 0);
            CHECK(r.edgePx == expEdge, "%dx%d shift (%+.4f,%+.4f): edge px %d, expected %d", W, H, j.first, j.second, r.edgePx,
                  expEdge);
            // M1: without the edge fill a negative shift leaves holes (the reason for LightFsEdgeFill)
            int d = 0;
            const Res r1 = Check(Run(g, W, H, j.first, j.second, kNoFill, &d), W, H, j.first, j.second, false);
            if (j.first < 0 || j.second < 0) CHECK(r1.holes == expEdge, "M1 %dx%d (%+.4f,%+.4f): holes %d, expected %d", W, H,
                                                   j.first, j.second, r1.holes, expEdge);
            m1Holes += r1.holes;
            // M2: the overscan viewport reads the wrong texel toward the far edge (the reason for "no overscan")
            const Res r2 = Check(Run(g, W, H, j.first, j.second, kOverscan, &d), W, H, j.first, j.second, false);
            m2Wrong += r2.wrongTexel;
        }
    }
    CHECK(m1Holes > 0, "mutation M1 (no edge fill) found no hole");
    CHECK(m2Wrong > 0, "mutation M2 (overscan viewport) read no wrong texel");
    std::printf("light_fs_test: %d runs (3 sizes x %zu shifts), %d edge re-draws; mutations: M1 %d holes, M2 %d wrong texels; "
                "%s (%d failures)\n", runs, sh.size(), edgeDraws, m1Holes, m2Wrong, g_fail ? "FAILED" : "all passed", g_fail);
    return g_fail ? 1 : 0;
}
