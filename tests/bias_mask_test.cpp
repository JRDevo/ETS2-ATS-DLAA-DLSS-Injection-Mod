// phase 49 unit test (WARP, no GPU needed): the GLOW BIAS MASK (inject.cpp GLOW BIAS MASK, dlaa.ini dlss_bias_mask).
//  T1 glow re-draw: two additive draws recorded through DrawIdRecord (forward mode, pixel-shader state captured, as BiasOnDraw) and
//     re-drawn with ReplayDepth into an R11G11B10_FLOAT glow sum (blend ONE + ONE) against a READ-ONLY view of a typed
//     D32_FLOAT_S8X24 scene depth with GREATER_EQUAL (as BiasBatch): the glow behind the scene depth is rejected, overlapping glows
//     add up, the game's PS constant buffer is the one bound at record time (changed after the record).
//  T2 share pass (DrawIdRecord::BiasShare): mask = smoothstep(0.05, 0.5, lum(glow) / lum(colour)) on four bands (no glow, a glow
//     over the night sky, half-half, a faint glow over a bright day background), only inside the clip (outside untouched), and the
//     state it touches is back as it was (the forward colour as RT0 + the DSV, the viewport, PS SRV 0 / 1, the PS, the blend state).
//  T3 crop step (scene_dlaa.cpp kBiasPrepShader, the CS of SceneDlaa::BiasPrep): Cur = the pass mask at the crop origin; Out =
//     strength x max(Cur, last frame's Cur around current + mv); PrevOk 0 = no carry-over.
// Build: tests\build_bias_mask_test.bat -> build\tests\bias_mask_test.exe (exit code 0 = all checks passed).
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "draw_ids.h"
#include "scene_dlaa.h"
#include "shader_cache.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX::PackedVector;

static int g_checks = 0, g_fails = 0;
static void Check(bool ok, const char* fmt, ...) {
    ++g_checks;
    if (ok) return;
    ++g_fails;
    va_list a;
    va_start(a, fmt);
    printf("  FAIL: ");
    vprintf(fmt, a);
    printf("\n");
    va_end(a);
}

struct Env {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext1> ctx1;
};

static ComPtr<ID3D11Texture2D> Tex(Env& E, UINT w, UINT h, DXGI_FORMAT f, UINT bind, const void* init = nullptr, UINT pitch = 0) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA sd{ init, pitch, 0 };
    ComPtr<ID3D11Texture2D> t;
    E.dev->CreateTexture2D(&d, init ? &sd : nullptr, &t);
    return t;
}
// readback of a whole texture into bytes (row-packed: w * bpp)
static std::vector<uint8_t> Read(Env& E, ID3D11Texture2D* t, UINT bpp) {
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> s;
    E.dev->CreateTexture2D(&d, nullptr, &s);
    E.ctx->CopyResource(s.Get(), t);
    D3D11_MAPPED_SUBRESOURCE m{};
    std::vector<uint8_t> out((size_t)d.Width * d.Height * bpp);
    if (SUCCEEDED(E.ctx->Map(s.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < d.Height; ++y) memcpy(&out[(size_t)y * d.Width * bpp], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)d.Width * bpp);
        E.ctx->Unmap(s.Get(), 0);
    }
    return out;
}
static ComPtr<ID3DBlob> Compile(const char* src, const char* entry, const char* prof) {
    ComPtr<ID3DBlob> b, err;
    if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, prof, 0, 0, &b, &err))) {
        printf("compile %s: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "?");
        return nullptr;
    }
    return b;
}

// ---- T1 ------------------------------------------------------------------------------------------------------------------
static const char kGameShaders[] = R"(
cbuffer PsCb : register(b0) { float4 Col; };
float4 VSMain(float3 p : POSITION) : SV_Position { return float4(p, 1.0); }
float4 PSMain(float4 pos : SV_Position) : SV_Target { return Col; }
)";
static const UINT W = 64, H = 32;

// a quad from x0 to x1 (pixels, full height) at depth z
static void Quad(float* v, float x0, float x1, float z) {
    const float l = x0 / (float)W * 2.0f - 1.0f, r = x1 / (float)W * 2.0f - 1.0f;
    const float q[6][3] = { { l, 1, z }, { r, 1, z }, { l, -1, z }, { r, 1, z }, { r, -1, z }, { l, -1, z } };
    memcpy(v, q, sizeof(q));
}

static void T1(Env& E) {
    printf("T1 glow re-draw: ReplayDepth into the R11G11B10 glow sum, read-only scene depth, GREATER_EQUAL, ONE + ONE\n");
    ComPtr<ID3DBlob> vsb = Compile(kGameShaders, "VSMain", "vs_5_0"), psb = Compile(kGameShaders, "PSMain", "ps_5_0");
    Check(vsb && psb, "T1: game shaders compile");
    if (!vsb || !psb) return;
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps; ComPtr<ID3D11InputLayout> il;
    E.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    E.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    const D3D11_INPUT_ELEMENT_DESC ie{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    E.dev->CreateInputLayout(&ie, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il);
    // vertices: quad 0 = the scene's occluder (x 0..24 at depth 0.4), quad 1 = glow A (x 0..32, z 0.5), quad 2 = glow B (x 16..48, z 0.2)
    float v[3][18];
    Quad(v[0], 0, 24, 0.4f); Quad(v[1], 0, 32, 0.5f); Quad(v[2], 16, 48, 0.2f);
    D3D11_BUFFER_DESC bd{ sizeof(v), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA vsd{ v, 0, 0 };
    ComPtr<ID3D11Buffer> vb;
    E.dev->CreateBuffer(&bd, &vsd, &vb);
    const uint16_t idx[18] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17 };
    D3D11_BUFFER_DESC ibd{ sizeof(idx), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA isd{ idx, 0, 0 };
    ComPtr<ID3D11Buffer> ib;
    E.dev->CreateBuffer(&ibd, &isd, &ib);
    D3D11_BUFFER_DESC cbd{ 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    const float colA[4] = { 0.5f, 0.25f, 0.125f, 1.0f }, colB[4] = { 0.25f, 0.5f, 1.0f, 1.0f }, colX[4] = { 8.0f, 8.0f, 8.0f, 1.0f };
    D3D11_SUBRESOURCE_DATA ca{ colA, 0, 0 }, cbb{ colB, 0, 0 };
    ComPtr<ID3D11Buffer> cbA, cbB;
    E.dev->CreateBuffer(&cbd, &ca, &cbA);
    E.dev->CreateBuffer(&cbd, &cbb, &cbB);
    // the scene: typed D32S8 depth (DSV only, as the game's), cleared to 0 (far, reversed-Z); the forward colour RGBA16F
    ComPtr<ID3D11Texture2D> depth = Tex(E, W, H, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D11_BIND_DEPTH_STENCIL);
    ComPtr<ID3D11DepthStencilView> dsv, ro;
    E.dev->CreateDepthStencilView(depth.Get(), nullptr, &dsv);
    D3D11_DEPTH_STENCIL_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; rd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    rd.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
    E.dev->CreateDepthStencilView(depth.Get(), &rd, &ro);
    ComPtr<ID3D11Texture2D> fwd = Tex(E, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11RenderTargetView> fwdRtv;
    E.dev->CreateRenderTargetView(fwd.Get(), nullptr, &fwdRtv);
    E.ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
    // game states: the occluder writes depth (GREATER), the glows test without writing (GREATER) and add (ONE + ONE)
    D3D11_DEPTH_STENCIL_DESC dw{}; dw.DepthEnable = TRUE; dw.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dw.DepthFunc = D3D11_COMPARISON_GREATER;
    D3D11_DEPTH_STENCIL_DESC dt = dw; dt.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ComPtr<ID3D11DepthStencilState> dssW, dssT;
    E.dev->CreateDepthStencilState(&dw, &dssW);
    E.dev->CreateDepthStencilState(&dt, &dssT);
    D3D11_BLEND_DESC ba{}; ba.RenderTarget[0].BlendEnable = TRUE; ba.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    ba.RenderTarget[0].DestBlend = D3D11_BLEND_ONE; ba.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    ba.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; ba.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    ba.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; ba.RenderTarget[0].RenderTargetWriteMask = 7;
    ComPtr<ID3D11BlendState> bsAdd;
    E.dev->CreateBlendState(&ba, &bsAdd);
    D3D11_RASTERIZER_DESC rs{}; rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rsN;
    E.dev->CreateRasterizerState(&rs, &rsN);
    const D3D11_VIEWPORT vp{ 0, 0, (float)W, (float)H, 0.01f, 0.9f };   // the world layer (the bias rule's viewport)
    const UINT stride = 12, off = 0;
    ID3D11Buffer* vbs = vb.Get();
    E.ctx->IASetInputLayout(il.Get());
    E.ctx->IASetVertexBuffers(0, 1, &vbs, &stride, &off);
    E.ctx->IASetIndexBuffer(ib.Get(), DXGI_FORMAT_R16_UINT, 0);
    E.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    E.ctx->VSSetShader(vs.Get(), nullptr, 0);
    E.ctx->PSSetShader(ps.Get(), nullptr, 0);
    E.ctx->RSSetState(rsN.Get());
    E.ctx->RSSetViewports(1, &vp);
    ID3D11RenderTargetView* rtv = fwdRtv.Get();
    E.ctx->OMSetRenderTargets(1, &rtv, dsv.Get());
    // the occluder (G-buffer stand-in)
    E.ctx->OMSetDepthStencilState(dssW.Get(), 0);
    E.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    ID3D11Buffer* c = cbA.Get();
    E.ctx->PSSetConstantBuffers(0, 1, &c);
    E.ctx->DrawIndexed(6, 0, 0);
    // the glows: A once, B twice -- each recorded right after the game's draw (BiasOnDraw)
    DrawIdRecord rec;
    rec.Reset(); rec.SetForward(true); rec.SetCapturePs(true);
    E.ctx->OMSetDepthStencilState(dssT.Get(), 0);
    E.ctx->OMSetBlendState(bsAdd.Get(), nullptr, 0xFFFFFFFFu);
    E.ctx->PSSetConstantBuffers(0, 1, &c);
    E.ctx->DrawIndexed(6, 6, 0);
    Check(rec.Record(E.ctx1.Get(), 6, 1, 6, 0, 0, false), "T1: glow A recorded");
    c = cbB.Get();
    E.ctx->PSSetConstantBuffers(0, 1, &c);
    E.ctx->DrawIndexed(6, 12, 0);
    Check(rec.Record(E.ctx1.Get(), 6, 1, 12, 0, 0, false), "T1: glow B recorded");
    E.ctx->DrawIndexed(6, 12, 0);
    Check(rec.Record(E.ctx1.Get(), 6, 1, 12, 0, 0, false), "T1: glow B (2nd) recorded");
    // the game moves on: another PS constant buffer is bound (the replay must use the recorded ones)
    D3D11_SUBRESOURCE_DATA cx{ colX, 0, 0 };
    ComPtr<ID3D11Buffer> cbX;
    E.dev->CreateBuffer(&cbd, &cx, &cbX);
    c = cbX.Get();
    E.ctx->PSSetConstantBuffers(0, 1, &c);
    // BiasBatch: the glow sum, our states (GREATER_EQUAL without write, ONE + ONE, RGB)
    ComPtr<ID3D11Texture2D> glow = Tex(E, W, H, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11RenderTargetView> glowRtv;
    E.dev->CreateRenderTargetView(glow.Get(), nullptr, &glowRtv);
    const FLOAT zero[4] = {};
    E.ctx->ClearRenderTargetView(glowRtv.Get(), zero);
    D3D11_DEPTH_STENCIL_DESC dg = dt; dg.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    ComPtr<ID3D11DepthStencilState> dssG;
    E.dev->CreateDepthStencilState(&dg, &dssG);
    const DrawIdRecord::ReplayResult rr = rec.ReplayDepth(E.ctx1.Get(), glowRtv.Get(), ro.Get(), dssG.Get(), bsAdd.Get(), true);
    rec.DropIds();
    Check(rr.ok && rr.replayed == 3, "T1: 3 glow draws re-drawn (ok %d, %d)", (int)rr.ok, rr.replayed);
    // the game's binding is back
    ID3D11RenderTargetView* r0 = nullptr; ID3D11DepthStencilView* d0 = nullptr;
    E.ctx->OMGetRenderTargets(1, &r0, &d0);
    Check(r0 == fwdRtv.Get() && d0 == dsv.Get(), "T1: the game's RT0 + DSV are bound again");
    if (r0) r0->Release();
    if (d0) d0->Release();
    ID3D11Buffer* pc = nullptr;
    E.ctx->PSGetConstantBuffers(0, 1, &pc);
    Check(pc == cbX.Get(), "T1: the game's PS cbuffer 0 is bound again");
    if (pc) pc->Release();
    const std::vector<uint8_t> g = Read(E, glow.Get(), 4);
    auto at = [&](UINT x, UINT y, float* out) { XMFLOAT3PK pk; memcpy(&pk, &g[((size_t)y * W + x) * 4], 4); DirectX::XMFLOAT3 f; DirectX::XMStoreFloat3(&f, XMLoadFloat3PK(&pk)); out[0] = f.x; out[1] = f.y; out[2] = f.z; };
    // expected per x band: [0,24) A (B occluded at 16..24), [24,32) A + 2B, [32,48) 2B, [48,64) nothing
    struct Band { UINT x; float e[3]; const char* what; } bands[] = {
        { 8,  { 0.5f, 0.25f, 0.125f }, "A alone" },
        { 20, { 0.5f, 0.25f, 0.125f }, "A, B behind the scene depth (rejected)" },
        { 28, { 1.0f, 1.25f, 2.125f }, "A + 2 B" },
        { 40, { 0.5f, 1.0f, 2.0f },    "2 B (summed)" },
        { 56, { 0.0f, 0.0f, 0.0f },    "nothing" } };
    for (const Band& b : bands) {
        float f[3];
        at(b.x, H / 2, f);
        const bool ok = std::fabs(f[0] - b.e[0]) < 0.02f && std::fabs(f[1] - b.e[1]) < 0.02f && std::fabs(f[2] - b.e[2]) < 0.04f;
        Check(ok, "T1: x=%u %s: glow (%.3f %.3f %.3f), want (%.3f %.3f %.3f)", b.x, b.what, f[0], f[1], f[2], b.e[0], b.e[1], b.e[2]);
    }
    printf("  3 re-drawn; bands checked at x = 8 / 20 / 28 / 40 / 56\n");
}

// ---- T2 ------------------------------------------------------------------------------------------------------------------
static float Smooth(float a, float b, float x) { float t = (x - a) / (b - a); t = t < 0 ? 0 : (t > 1 ? 1 : t); return t * t * (3 - 2 * t); }

static void T2(Env& E) {
    printf("T2 share pass (DrawIdRecord::BiasShare): four bands, the clip, the state restore\n");
    // glow + colour per 16-px band: (glow, colour) grey levels
    const float gv[4] = { 0.0f, 1.0f, 0.25f, 0.1f }, cv[4] = { 1.0f, 1.05f, 1.0f, 5.0f };
    std::vector<uint32_t> gl(W * H);
    std::vector<uint16_t> co(W * H * 4);
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x) {
            const int b = (int)(x / 16);
            XMFLOAT3PK pk(gv[b], gv[b], gv[b]);
            memcpy(&gl[y * W + x], &pk, 4);
            for (int k = 0; k < 3; ++k) co[(y * W + x) * 4 + k] = XMConvertFloatToHalf(cv[b]);
            co[(y * W + x) * 4 + 3] = XMConvertFloatToHalf(1.0f);
        }
    ComPtr<ID3D11Texture2D> glow = Tex(E, W, H, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, gl.data(), W * 4);
    ComPtr<ID3D11Texture2D> fwd = Tex(E, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, co.data(), W * 8);
    ComPtr<ID3D11Texture2D> mask = Tex(E, W, H, DXGI_FORMAT_R8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ComPtr<ID3D11ShaderResourceView> glowSrv, fwdSrv, other;
    ComPtr<ID3D11RenderTargetView> fwdRtv, maskRtv;
    E.dev->CreateShaderResourceView(glow.Get(), nullptr, &glowSrv);
    E.dev->CreateShaderResourceView(fwd.Get(), nullptr, &fwdSrv);
    E.dev->CreateShaderResourceView(mask.Get(), nullptr, &other);
    E.dev->CreateRenderTargetView(fwd.Get(), nullptr, &fwdRtv);
    E.dev->CreateRenderTargetView(mask.Get(), nullptr, &maskRtv);
    const FLOAT half[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
    E.ctx->ClearRenderTargetView(maskRtv.Get(), half);
    // the game's state at the scene depth discard: the forward colour as RT0 (+ a DSV), its viewport, a PS SRV in slot 0
    ComPtr<ID3D11Texture2D> depth = Tex(E, W, H, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D11_BIND_DEPTH_STENCIL);
    ComPtr<ID3D11DepthStencilView> dsv;
    E.dev->CreateDepthStencilView(depth.Get(), nullptr, &dsv);
    ID3D11RenderTargetView* rtv = fwdRtv.Get();
    E.ctx->OMSetRenderTargets(1, &rtv, dsv.Get());
    const D3D11_VIEWPORT gvp{ 3, 4, 50, 20, 0.01f, 0.9f };
    E.ctx->RSSetViewports(1, &gvp);
    ID3D11ShaderResourceView* s0 = glowSrv.Get();         // (any SRV that is not the forward colour)
    E.ctx->PSSetShaderResources(0, 1, &s0);
    D3D11_BLEND_DESC bd{}; bd.RenderTarget[0].RenderTargetWriteMask = 15;
    ComPtr<ID3D11BlendState> gbs;
    E.dev->CreateBlendState(&bd, &gbs);
    const FLOAT bfIn[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
    E.ctx->OMSetBlendState(gbs.Get(), bfIn, 0x0Fu);
    const D3D11_RECT clip{ 0, 0, 56, 24 };
    const bool ok = DrawIdRecord::BiasShare(E.ctx1.Get(), maskRtv.Get(), W, H, glowSrv.Get(), fwdSrv.Get(), &clip);
    Check(ok, "T2: BiasShare ran");
    // state back
    ID3D11RenderTargetView* r0 = nullptr; ID3D11DepthStencilView* d0 = nullptr;
    E.ctx->OMGetRenderTargets(1, &r0, &d0);
    Check(r0 == fwdRtv.Get() && d0 == dsv.Get(), "T2: the forward colour is RT0 again with its DSV");
    if (r0) r0->Release();
    if (d0) d0->Release();
    UINT nvp = 1; D3D11_VIEWPORT vpo{};
    E.ctx->RSGetViewports(&nvp, &vpo);
    Check(nvp == 1 && vpo.TopLeftX == 3 && vpo.Width == 50 && vpo.MaxDepth == 0.9f, "T2: the game's viewport is back");
    ID3D11ShaderResourceView* sr[2] = {};
    E.ctx->PSGetShaderResources(0, 2, sr);
    Check(sr[0] == glowSrv.Get() && sr[1] == nullptr, "T2: PS SRV 0 / 1 are the game's again");
    for (ID3D11ShaderResourceView* p : sr) if (p) p->Release();
    ID3D11BlendState* bso = nullptr; FLOAT bf[4] = {}; UINT sm = 0;
    E.ctx->OMGetBlendState(&bso, bf, &sm);
    Check(bso == gbs.Get() && bf[2] == 0.3f && sm == 0x0Fu, "T2: the game's blend state + factor + mask are back");
    if (bso) bso->Release();
    // values
    const std::vector<uint8_t> m = Read(E, mask.Get(), 1);
    for (int b = 0; b < 4; ++b) {
        const UINT x = (UINT)b * 16u + 5u, y = 10;
        const float gq = b == 3 ? 0.099609375f : gv[b];          // 0.1 in R11G11B10 (6-bit mantissa)
        const float s = gq / (std::fmax)((std::fmax)(XMConvertHalfToFloat(XMConvertFloatToHalf(cv[b])), gq), 1e-6f);
        const float want = gq > 1e-5f ? Smooth(0.05f, 0.5f, s) : 0.0f;
        const float got = m[(size_t)y * W + x] / 255.0f;
        Check(std::fabs(got - want) < 2.5f / 255.0f, "T2: band %d (glow %.2f over colour %.2f): mask %.3f, want %.3f", b, gv[b], cv[b], got, want);
    }
    Check(m[(size_t)28 * W + 5] == 128 && m[(size_t)10 * W + 60] == 128, "T2: outside the clip the mask is untouched (%u %u)",
          m[(size_t)28 * W + 5], m[(size_t)10 * W + 60]);
    printf("  bands: no glow %.3f | night glow %.3f | half %.3f | faint day glow %.3f\n", m[10 * W + 5] / 255.0, m[10 * W + 21] / 255.0,
           m[10 * W + 37] / 255.0, m[10 * W + 53] / 255.0);
}

// ---- T3 ------------------------------------------------------------------------------------------------------------------
static void T3(Env& E) {
    printf("T3 crop step (kBiasPrepShader): crop origin, last frame's glow along the MVs, strength, PrevOk\n");
    size_t sz = 0;
    const void* code = ShaderCache::Code(ShaderCache::kBiasPrep, &sz);
    Check(code != nullptr, "T3: kBiasPrep compiled (%s)", ShaderCache::Error(ShaderCache::kBiasPrep));
    if (!code) return;
    ComPtr<ID3D11ComputeShader> cs;
    E.dev->CreateComputeShader(code, sz, nullptr, &cs);
    const UINT FW = 64, FH = 32, CW = 32, CH = 16, OX = 16, OY = 8;   // the full mask and the crop at (16, 8)
    // the pass mask: one glow pixel at full (16 + 5, 8 + 6) = crop (5, 6), value 1.0
    std::vector<uint8_t> full(FW * FH, 0);
    full[(OY + 6) * FW + OX + 5] = 255;
    // last frame's crop: a glow at crop (20, 10); this frame's MVs: +3 px x, -2 px y everywhere (current + mv = previous)
    std::vector<uint8_t> prev(CW * CH, 0);
    prev[10 * CW + 20] = 204;                                // 0.8
    std::vector<uint32_t> mv(CW * CH);
    const uint32_t mvx = XMConvertFloatToHalf(3.0f), mvy = XMConvertFloatToHalf(-2.0f);
    for (uint32_t& p : mv) p = mvx | (mvy << 16);
    ComPtr<ID3D11Texture2D> tFull = Tex(E, FW, FH, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, full.data(), FW);
    ComPtr<ID3D11Texture2D> tPrev = Tex(E, CW, CH, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE, prev.data(), CW);
    ComPtr<ID3D11Texture2D> tMv = Tex(E, CW, CH, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, mv.data(), CW * 4);
    ComPtr<ID3D11Texture2D> tCur = Tex(E, CW, CH, DXGI_FORMAT_R8_UNORM, D3D11_BIND_UNORDERED_ACCESS);
    ComPtr<ID3D11Texture2D> tOut = Tex(E, CW, CH, DXGI_FORMAT_R8_UNORM, D3D11_BIND_UNORDERED_ACCESS);
    ComPtr<ID3D11ShaderResourceView> sFull, sPrev, sMv;
    ComPtr<ID3D11UnorderedAccessView> uCur, uOut;
    E.dev->CreateShaderResourceView(tFull.Get(), nullptr, &sFull);
    E.dev->CreateShaderResourceView(tPrev.Get(), nullptr, &sPrev);
    E.dev->CreateShaderResourceView(tMv.Get(), nullptr, &sMv);
    E.dev->CreateUnorderedAccessView(tCur.Get(), nullptr, &uCur);
    E.dev->CreateUnorderedAccessView(tOut.Get(), nullptr, &uOut);
    D3D11_BUFFER_DESC cbd{ 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    for (int pass = 0; pass < 2; ++pass) {
        struct { uint32_t x, y, prevOk; float strength; } cb = { OX, OY, pass == 0 ? 1u : 0u, 0.5f };
        D3D11_SUBRESOURCE_DATA cd{ &cb, 0, 0 };
        ComPtr<ID3D11Buffer> b;
        E.dev->CreateBuffer(&cbd, &cd, &b);
        ID3D11Buffer* bb = b.Get();
        ID3D11ShaderResourceView* srv[3] = { sFull.Get(), sPrev.Get(), sMv.Get() };
        ID3D11UnorderedAccessView* uav[2] = { uCur.Get(), uOut.Get() };
        E.ctx->CSSetShader(cs.Get(), nullptr, 0);
        E.ctx->CSSetConstantBuffers(0, 1, &bb);
        E.ctx->CSSetShaderResources(0, 3, srv);
        E.ctx->CSSetUnorderedAccessViews(0, 2, uav, nullptr);
        E.ctx->Dispatch((CW + 7) / 8, (CH + 7) / 8, 1);
        ID3D11UnorderedAccessView* nu[2] = {};
        E.ctx->CSSetUnorderedAccessViews(0, 2, nu, nullptr);
        const std::vector<uint8_t> cur = Read(E, tCur.Get(), 1), out = Read(E, tOut.Get(), 1);
        int curN = 0, outN = 0;
        for (uint8_t v : cur) curN += v ? 1 : 0;
        for (uint8_t v : out) outN += v ? 1 : 0;
        Check(cur[6 * CW + 5] == 255 && curN == 1, "T3/%d: Cur = the pass mask at the crop origin (%u, %d set)", pass, cur[6 * CW + 5], curN);
        Check(std::abs((int)out[6 * CW + 5] - 128) <= 1, "T3/%d: this frame's glow x strength 0.5 (%u)", pass, out[6 * CW + 5]);
        if (pass == 0) {
            // pixel (x, y) reads the 2x2 texels from (x + 3, y - 2) of last frame's crop: (17, 12) -> (20, 10) = the old glow ->
            // 0.8 x 0.5; so do (16, 12), (17, 11), (16, 11) (their footprints hold (20, 10) too): 4 trail pixels + the glow itself
            Check(std::abs((int)out[12 * CW + 17] - 102) <= 1, "T3/0: the trail pixel (its history held the glow) gets 0.8 x 0.5 (%u)",
                  out[12 * CW + 17]);
            Check(outN == 5, "T3/0: 5 pixels set (the glow + its 2x2 history footprint), %d", outN);
        } else {
            Check(out[12 * CW + 17] == 0 && outN == 1, "T3/1: PrevOk 0 = no carry-over (%u, %d set)", out[12 * CW + 17], outN);
        }
    }
}

int main() {
    Env E;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &E.dev, nullptr, &E.ctx)) ||
        FAILED(E.ctx.As(&E.ctx1))) {
        printf("WARP device failed\n");
        return 1;
    }
    DrawIdRecord::RegisterShaders();                      // + the share pass (kBiasNormPs) and the occlusion init's VS
    SceneDlaa::RegisterShaders();                         // + the crop step (kBiasPrep)
    ShaderCache::Start();
    while (!ShaderCache::Done()) Sleep(10);
    T1(E);
    T2(E);
    T3(E);
    printf("bias_mask_test: %d checks, %d failed -> %s\n", g_checks, g_fails, g_fails ? "FAIL" : "PASS");
    return g_fails ? 1 : 0;
}
