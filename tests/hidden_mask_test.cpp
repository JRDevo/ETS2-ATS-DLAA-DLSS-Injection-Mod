// phase 40 unit test (WARP, no GPU needed): the VR hidden area mask.
//  T1 MapToNdc: the runtime's view-tangent vertices land on the exact NDC corners for an asymmetric fov; Coverage of known shapes.
//  T2 MapToNdc rejects: a mesh that never reaches the picture border, one that covers most of the picture, a bad index / fov.
//  T3 HiddenMask::Draw on a D32_FLOAT_S8X24 depth cleared to 0 (reversed Z) with stencil 0x5A: depth 1.0 inside the mask, 0.0
//     outside, the stencil untouched everywhere; and every piece of D3D11 state the draw touches is back as it was (render
//     targets + DSV, viewports, IA layout / topology / vertex buffer 0, VS / PS, RS / blend / depth-stencil state + ref + factor,
//     PS SRV0).
// Build: tests\build_hidden_mask_test.bat -> build\tests\hidden_mask_test.exe (exit code 0 = all checks passed).
#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "hidden_mask.h"
#include "shader_cache.h"
#include "preview_blit.h"

static int g_checks = 0, g_fails = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) { ++g_fails; printf("  FAIL: %s\n", what); }
}
static const float kDeg = 0.017453292519943295f;

// an asymmetric fov (a right eye: more to the right / temporal side)
static const float kFov[4] = { -40.0f * kDeg, 52.0f * kDeg, 44.0f * kDeg, -50.0f * kDeg };

// tangent-space point at NDC (x, y) for kFov (the inverse of the mapping, written independently)
static void TanOf(float x, float y, float* tx, float* ty) {
    const double tl = std::tan((double)kFov[0]), tr = std::tan((double)kFov[1]);
    const double tu = std::tan((double)kFov[2]), td = std::tan((double)kFov[3]);
    *tx = (float)(tl + (x + 1.0) * 0.5 * (tr - tl));
    *ty = (float)(td + (y + 1.0) * 0.5 * (tu - td));
}

static void T1T2() {
    printf("T1 / T2 MapToNdc + Coverage\n");
    // two corner triangles: top-left (-1,1) (0,1) (-1,0) and bottom-right (1,-1) (1,0) (0,-1), given in view tangents
    const float nd[6][2] = { { -1, 1 }, { 0, 1 }, { -1, 0 }, { 1, -1 }, { 1, 0 }, { 0, -1 } };
    float xy[12];
    for (int i = 0; i < 6; ++i) TanOf(nd[i][0], nd[i][1], &xy[2 * i], &xy[2 * i + 1]);
    const uint32_t idx[6] = { 0, 1, 2, 3, 4, 5 };
    float out[12] = {};
    HiddenMask::MapInfo mi;
    const bool ok = HiddenMask::MapToNdc(xy, 6, idx, 6, kFov, out, &mi);
    Check(ok, "T1: corner mesh accepted");
    double maxErr = 0.0;
    for (int i = 0; i < 6; ++i) {
        maxErr = (std::fmax)(maxErr, std::fabs(out[2 * i] - nd[i][0]));
        maxErr = (std::fmax)(maxErr, std::fabs(out[2 * i + 1] - nd[i][1]));
    }
    printf("  mapped max error %.2e, bounds x %+.3f..%+.3f y %+.3f..%+.3f, coverage %.4f (exact 0.25)\n", maxErr, mi.ndcMin[0],
           mi.ndcMax[0], mi.ndcMin[1], mi.ndcMax[1], mi.coverage);
    Check(maxErr < 1e-5, "T1: tangents map onto the exact NDC corners / edge midpoints");
    Check(mi.tris == 2, "T1: 2 triangles");
    Check(std::fabs(mi.coverage - 0.25f) < 0.02f, "T1: coverage of two corner halves-of-quadrants = 0.25");
    // the optical axis (tangent 0, 0) maps to the fov centre: x = -(tr + tl) / (tr - tl)
    {
        const float axy[6] = { 0.0f, 0.0f, xy[0], xy[1], xy[2], xy[3] };
        const uint32_t ai[3] = { 0, 1, 2 };
        float ao[6];
        HiddenMask::MapInfo m2;
        HiddenMask::MapToNdc(axy, 3, ai, 3, kFov, ao, &m2);
        const double tl = std::tan((double)kFov[0]), tr = std::tan((double)kFov[1]);
        const double tu = std::tan((double)kFov[2]), td = std::tan((double)kFov[3]);
        Check(std::fabs(ao[0] - (-(tr + tl) / (tr - tl))) < 1e-5 && std::fabs(ao[1] - (-(tu + td) / (tu - td))) < 1e-5,
              "T1: the view axis lands on the asymmetric fov centre");
    }
    // Coverage of the full square (2 triangles) = 1
    const float full[12] = { -1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1 };
    Check(std::fabs(HiddenMask::Coverage(full, 2, 64) - 1.0f) < 1e-6f, "T1: Coverage(full square) = 1");
    // T2 rejects
    {
        float mid[6];
        TanOf(-0.2f, -0.2f, &mid[0], &mid[1]); TanOf(0.2f, -0.2f, &mid[2], &mid[3]); TanOf(0.0f, 0.2f, &mid[4], &mid[5]);
        const uint32_t si[3] = { 0, 1, 2 };
        float so[6];
        HiddenMask::MapInfo m3;
        Check(!HiddenMask::MapToNdc(mid, 3, si, 3, kFov, so, &m3), "T2: a mesh in the middle (not reaching the border) is rejected");
        printf("  middle mesh: %s\n", m3.why);
    }
    {
        float fx[12];
        for (int i = 0; i < 6; ++i) TanOf(full[2 * i], full[2 * i + 1], &fx[2 * i], &fx[2 * i + 1]);
        const uint32_t fi[6] = { 0, 1, 2, 3, 4, 5 };
        float fo[12];
        HiddenMask::MapInfo m4;
        Check(!HiddenMask::MapToNdc(fx, 6, fi, 6, kFov, fo, &m4), "T2: a mesh over the whole picture is rejected");
        printf("  full mesh: %s (coverage %.3f)\n", m4.why, m4.coverage);
    }
    {
        const uint32_t bad[3] = { 0, 1, 9 };
        float bo[6];
        HiddenMask::MapInfo m5;
        Check(!HiddenMask::MapToNdc(xy, 6, bad, 3, kFov, bo, &m5), "T2: an index out of range is rejected");
        const float badFov[4] = { 0.3f, 0.2f, 0.5f, -0.5f };   // right < left
        Check(!HiddenMask::MapToNdc(xy, 6, idx, 6, badFov, out, &m5), "T2: an unusable fov is rejected");
    }
}

static bool T3(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    printf("T3 depth-only draw + state restore (WARP)\n");
    const UINT W = 320, H = 240;
    // the "scene depth": D32_FLOAT_S8X24, cleared like the game (depth 0 = far, reversed Z) with a stencil pattern
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D* ds = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &ds))) { printf("  depth texture failed\n"); return false; }
    ID3D11DepthStencilView* dsv = nullptr;
    dev->CreateDepthStencilView(ds, nullptr, &dsv);
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0x5A);
    // a "game" state to come back: a colour target + another depth bound, odd viewports, a VB, states, topology
    D3D11_TEXTURE2D_DESC cd = td;
    cd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D* col = nullptr; dev->CreateTexture2D(&cd, nullptr, &col);
    ID3D11RenderTargetView* rtv = nullptr; dev->CreateRenderTargetView(col, nullptr, &rtv);
    ID3D11Texture2D* col2 = nullptr; dev->CreateTexture2D(&cd, nullptr, &col2);
    ID3D11ShaderResourceView* srv = nullptr; dev->CreateShaderResourceView(col2, nullptr, &srv);
    ID3D11Texture2D* ds2 = nullptr; dev->CreateTexture2D(&td, nullptr, &ds2);
    ID3D11DepthStencilView* dsv2 = nullptr; dev->CreateDepthStencilView(ds2, nullptr, &dsv2);
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 256; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    ID3D11Buffer* vbG = nullptr; dev->CreateBuffer(&bd, nullptr, &vbG);
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_WIREFRAME; rd.CullMode = D3D11_CULL_FRONT; rd.ScissorEnable = TRUE;
    ID3D11RasterizerState* rsG = nullptr; dev->CreateRasterizerState(&rd, &rsG);
    D3D11_DEPTH_STENCIL_DESC dd{}; dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL; dd.StencilEnable = TRUE; dd.StencilReadMask = 0xF0; dd.StencilWriteMask = 0x0F;
    dd.FrontFace.StencilFunc = dd.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    dd.FrontFace.StencilPassOp = dd.BackFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;
    dd.FrontFace.StencilFailOp = dd.BackFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = dd.BackFace.StencilDepthFailOp =
        D3D11_STENCIL_OP_KEEP;
    ID3D11DepthStencilState* dssG = nullptr; dev->CreateDepthStencilState(&dd, &dssG);
    D3D11_BLEND_DESC bld{}; bld.RenderTarget[0].BlendEnable = TRUE; bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD; bld.RenderTarget[0].RenderTargetWriteMask = 0x7;
    ID3D11BlendState* bsG = nullptr; dev->CreateBlendState(&bld, &bsG);
    ctx->OMSetRenderTargets(1, &rtv, dsv2);
    D3D11_VIEWPORT vps[2] = { { 3.5f, 2.0f, 100.0f, 50.0f, 0.9f, 1.0f }, { 0.0f, 0.0f, 10.0f, 10.0f, 0.0f, 0.5f } };
    ctx->RSSetViewports(2, vps);
    const UINT strideG = 12, offG = 4;
    ctx->IASetVertexBuffers(0, 1, &vbG, &strideG, &offG);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ctx->RSSetState(rsG);
    const FLOAT bfG[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
    ctx->OMSetBlendState(bsG, bfG, 0x00FF00FFu);
    ctx->OMSetDepthStencilState(dssG, 0x37);
    ctx->PSSetShaderResources(0, 1, &srv);

    // the mesh: the top-left corner triangle (-1,1) (0,1) (-1,0), at the nearest value 1.0
    const float tri[6] = { -1.0f, 1.0f, 0.0f, 1.0f, -1.0f, 0.0f };
    if (!HiddenMask::Ensure(dev)) { printf("  Ensure failed\n"); return false; }
    Check(HiddenMask::Upload(dev, 1, tri, 1, 1.0f), "T3: upload");
    Check(HiddenMask::Draw(ctx, dsv, W, H, 1), "T3: draw");

    // state back?
    ID3D11RenderTargetView* r2[8] = {}; ID3D11DepthStencilView* d2 = nullptr;
    ctx->OMGetRenderTargets(8, r2, &d2);
    Check(r2[0] == rtv && !r2[1] && d2 == dsv2, "T3: render targets + DSV restored");
    for (auto* p : r2) if (p) p->Release();
    if (d2) d2->Release();
    UINT nvp = 8; D3D11_VIEWPORT v2[8] = {};
    ctx->RSGetViewports(&nvp, v2);
    Check(nvp == 2 && !memcmp(v2, vps, sizeof(vps)), "T3: viewports restored");
    ID3D11Buffer* vb2 = nullptr; UINT st2 = 0, of2 = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb2, &st2, &of2);
    Check(vb2 == vbG && st2 == strideG && of2 == offG, "T3: vertex buffer 0 restored");
    if (vb2) vb2->Release();
    D3D11_PRIMITIVE_TOPOLOGY tp; ctx->IAGetPrimitiveTopology(&tp);
    Check(tp == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, "T3: topology restored");
    ID3D11InputLayout* il2 = nullptr; ctx->IAGetInputLayout(&il2);
    Check(il2 == nullptr, "T3: input layout restored (none)");
    if (il2) il2->Release();
    ID3D11VertexShader* vs2 = nullptr; ctx->VSGetShader(&vs2, nullptr, nullptr);
    Check(vs2 == nullptr, "T3: VS restored (none)");
    if (vs2) vs2->Release();
    ID3D11RasterizerState* rs2 = nullptr; ctx->RSGetState(&rs2);
    Check(rs2 == rsG, "T3: RS state restored");
    if (rs2) rs2->Release();
    ID3D11BlendState* bs2 = nullptr; FLOAT bf2[4] = {}; UINT sm2 = 0;
    ctx->OMGetBlendState(&bs2, bf2, &sm2);
    Check(bs2 == bsG && !memcmp(bf2, bfG, sizeof(bfG)) && sm2 == 0x00FF00FFu, "T3: blend state + factor + sample mask restored");
    if (bs2) bs2->Release();
    ID3D11DepthStencilState* ds2s = nullptr; UINT ref2 = 0;
    ctx->OMGetDepthStencilState(&ds2s, &ref2);
    Check(ds2s == dssG && ref2 == 0x37, "T3: depth-stencil state + ref restored");
    if (ds2s) ds2s->Release();
    ID3D11ShaderResourceView* srv2 = nullptr; ctx->PSGetShaderResources(0, 1, &srv2);
    Check(srv2 == srv, "T3: PS SRV0 restored");
    if (srv2) srv2->Release();

    // depth + stencil: read back (staging copy of the D32S8 texture: float depth, uint8 stencil, 24 bits unused per texel)
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* stg = nullptr;
    dev->CreateTexture2D(&sd, nullptr, &stg);
    ctx->CopyResource(stg, ds);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m))) { printf("  map failed\n"); return false; }
    uint32_t in = 0, out = 0, badIn = 0, badOut = 0, badSt = 0, edge = 0;
    for (UINT y = 0; y < H; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + (size_t)y * m.RowPitch;
        for (UINT x = 0; x < W; ++x) {
            float dpt; memcpy(&dpt, row + 8 * x, 4);
            const uint8_t st = row[8 * x + 4];
            if (st != 0x5A) ++badSt;
            const double xn = ((double)x + 0.5) / W * 2.0 - 1.0, yn = 1.0 - ((double)y + 0.5) / H * 2.0;
            const double e = xn - yn + 1.0;                 // < 0 inside the triangle (-1,1) (0,1) (-1,0)
            const double px = 2.0 / (std::fmin)(W, H);
            if (std::fabs(e) < 1.5 * px) { ++edge; continue; }
            if (e < 0.0) { ++in; if (dpt != 1.0f) ++badIn; }
            else         { ++out; if (dpt != 0.0f) ++badOut; }
        }
    }
    ctx->Unmap(stg, 0);
    printf("  inside %u px (depth != 1: %u), outside %u px (depth != 0: %u), edge band skipped %u, stencil changed %u px\n", in,
           badIn, out, badOut, edge, badSt);
    Check(in > 1000 && badIn == 0, "T3: depth 1.0 (nearest, reversed Z) everywhere inside the mask");
    Check(out > 1000 && badOut == 0, "T3: depth untouched (0.0) outside the mask");
    Check(badSt == 0, "T3: stencil untouched everywhere");
    stg->Release(); ds->Release(); dsv->Release(); col->Release(); rtv->Release(); col2->Release(); srv->Release(); ds2->Release();
    dsv2->Release(); vbG->Release(); rsG->Release(); dssG->Release(); bsG->Release();
    return true;
}

int main() {
    T1T2();
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        printf("WARP device failed\n");
        return 1;
    }
    HiddenMask::RegisterShaders();
    ShaderCache::Start();
    while (!ShaderCache::Done()) Sleep(10);
    size_t sz = 0;
    if (!ShaderCache::Code(ShaderCache::kHiddenMaskVs, &sz)) {
        printf("shader: %s\n", ShaderCache::Error(ShaderCache::kHiddenMaskVs));
        return 1;
    }
    if (!T3(dev, ctx)) ++g_fails;
    ctx->Release();
    dev->Release();
    printf("%s: %d / %d checks passed\n", g_fails ? "FAILED" : "PASSED", g_checks - g_fails, g_checks);
    return g_fails ? 1 : 0;
}
