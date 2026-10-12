// HiddenMask implementation -- phase 40. See hidden_mask.h.
#ifdef WITH_DLAA

#include "hidden_mask.h"
#include <cmath>
#include <wrl/client.h>
#ifndef HM_NO_D3D
#include "preview_blit.h"
#include "shader_cache.h"
#include "log.h"
#endif

using Microsoft::WRL::ComPtr;

namespace HiddenMask {

// ---- the pure part (no D3D; tests can build it alone with HM_NO_D3D) --------------------------------------------------------
namespace {
inline float Edge(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}
}

float Coverage(const float* t, uint32_t tris, int grid) {
    if (!t || !tris || grid < 1) return 0.0f;
    uint32_t in = 0;
    for (int gy = 0; gy < grid; ++gy) {
        const float py = -1.0f + (2.0f * (float)gy + 1.0f) / (float)grid;
        for (int gx = 0; gx < grid; ++gx) {
            const float px = -1.0f + (2.0f * (float)gx + 1.0f) / (float)grid;
            for (uint32_t k = 0; k < tris; ++k) {
                const float* v = t + 6 * k;
                const float e0 = Edge(v[0], v[1], v[2], v[3], px, py);
                const float e1 = Edge(v[2], v[3], v[4], v[5], px, py);
                const float e2 = Edge(v[4], v[5], v[0], v[1], px, py);
                if ((e0 >= 0.0f && e1 >= 0.0f && e2 >= 0.0f) || (e0 <= 0.0f && e1 <= 0.0f && e2 <= 0.0f)) {
                    // (a degenerate triangle has all three 0 only on its line: counted, harmless for a sanity figure)
                    ++in;
                    break;
                }
            }
        }
    }
    return (float)in / (float)(grid * grid);
}

bool MapToNdc(const float* xy, uint32_t nv, const uint32_t* idx, uint32_t ni, const float fovRad[4], float* out, MapInfo* info) {
    MapInfo dummy;
    MapInfo& m = info ? *info : dummy;
    m = MapInfo();
    if (!xy || !idx || !out || !nv || !ni) { m.why = "empty mesh"; return false; }
    if (ni % 3 != 0) { m.why = "index count not a multiple of 3"; return false; }
    const uint32_t tris = ni / 3;
    if (tris > kMaxTris) { m.why = "too many triangles"; return false; }
    const double tl = std::tan((double)fovRad[0]), tr = std::tan((double)fovRad[1]);
    const double tu = std::tan((double)fovRad[2]), td = std::tan((double)fovRad[3]);
    const double sw = tr - tl, sh = tu - td;
    if (!(sw > 0.05 && sw < 40.0) || !(sh > 0.05 && sh < 40.0) || !std::isfinite(sw) || !std::isfinite(sh)) {
        m.why = "unusable fov";
        return false;
    }
    float mn[2] = { 1e30f, 1e30f }, mx[2] = { -1e30f, -1e30f };
    for (uint32_t k = 0; k < ni; ++k) {
        const uint32_t i = idx[k];
        if (i >= nv) { m.why = "index out of range"; return false; }
        const double x = (2.0 * (double)xy[2 * i] - (tr + tl)) / sw;
        const double y = (2.0 * (double)xy[2 * i + 1] - (tu + td)) / sh;
        if (!std::isfinite(x) || !std::isfinite(y)) { m.why = "vertex not finite"; return false; }
        out[2 * k] = (float)x;
        out[2 * k + 1] = (float)y;
        mn[0] = (std::fmin)(mn[0], (float)x); mx[0] = (std::fmax)(mx[0], (float)x);
        mn[1] = (std::fmin)(mn[1], (float)y); mx[1] = (std::fmax)(mx[1], (float)y);
    }
    m.tris = tris;
    m.ndcMin[0] = mn[0]; m.ndcMin[1] = mn[1]; m.ndcMax[0] = mx[0]; m.ndcMax[1] = mx[1];
    m.coverage = Coverage(out, tris, 64);
    // the hidden area is the lens border: the mesh reaches the picture edge somewhere, and stays in a sane range
    const float reach = (std::fmax)((std::fmax)(-mn[0], mx[0]), (std::fmax)(-mn[1], mx[1]));
    if (reach < 0.9f) { m.why = "the mesh does not reach the picture border (not in view tangent space?)"; return false; }
    if (reach > 8.0f) { m.why = "the mesh reaches far outside the picture (not in view tangent space?)"; return false; }
    if (m.coverage < 0.002f) { m.why = "covers less than 0.2 % of the picture"; return false; }
    if (m.coverage > 0.60f) { m.why = "covers more than 60 % of the picture"; return false; }
    return true;
}

#ifndef HM_NO_D3D
// ---- the D3D part ----------------------------------------------------------------------------------------------------------
namespace {
// Depth-only: the vertex is already in NDC with the depth value as z (no constant buffer, no pixel shader).
// kHmVs-BEGIN (the build validates this block with fxc, vs_5_0 / VSMain)
const char kHmVs[] = R"(
float4 VSMain(float3 p : POSITION) : SV_Position {
    return float4(p, 1.0);
}
)";
// kHmVs-END

ComPtr<ID3D11VertexShader>      g_vs;
ComPtr<ID3D11InputLayout>       g_il;
ComPtr<ID3D11BlendState>        g_blend;      // colour writes off (no render target is bound anyway)
ComPtr<ID3D11RasterizerState>   g_rs;         // solid, no cull, depth clip on, no scissor
ComPtr<ID3D11DepthStencilState> g_dss;        // depth ALWAYS + write ALL, stencil off
ID3D11Device*                   g_dev = nullptr;
bool                            g_failed = false;
struct Mesh { ComPtr<ID3D11Buffer> vb; uint32_t verts = 0; };
Mesh                            g_mesh[2];
}

void RegisterShaders() {
    ShaderCache::Add(ShaderCache::kHiddenMaskVs, kHmVs, sizeof(kHmVs) - 1, "hidden_mask_vs", "VSMain", "vs_5_0");
}

void Reset() {
    g_vs.Reset(); g_il.Reset(); g_blend.Reset(); g_rs.Reset(); g_dss.Reset();
    for (Mesh& m : g_mesh) { m.vb.Reset(); m.verts = 0; }
    g_dev = nullptr;
    g_failed = false;
}

bool Ensure(ID3D11Device* dev) {
    if (!dev) return false;
    if (g_dev == dev && g_vs && g_il && g_blend && g_rs && g_dss) return true;
    if (g_failed && g_dev == dev) return false;
    if (!ShaderCache::Ready(ShaderCache::kHiddenMaskVs)) return false;   // the warm-up is still running: next time
    if (g_dev != dev) Reset();
    g_failed = true; g_dev = dev;
    size_t vsSize = 0;
    const void* vsCode = ShaderCache::Code(ShaderCache::kHiddenMaskVs, &vsSize);
    HRESULT hr;
    if (!vsCode) { LogErr("hidden area mask: VS compile failed %s -- no mask", ShaderCache::Error(ShaderCache::kHiddenMaskVs)); return false; }
    if (FAILED(hr = dev->CreateVertexShader(vsCode, vsSize, nullptr, &g_vs))) {
        LogErr("hidden area mask: CreateVertexShader hr=0x%lx -- no mask", hr); return false;
    }
    const D3D11_INPUT_ELEMENT_DESC el = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    if (FAILED(hr = dev->CreateInputLayout(&el, 1, vsCode, vsSize, &g_il))) {
        LogErr("hidden area mask: CreateInputLayout hr=0x%lx -- no mask", hr); return false;
    }
    D3D11_BLEND_DESC bd{};
    for (int i = 0; i < 8; ++i) {
        bd.RenderTarget[i].BlendEnable = FALSE;
        bd.RenderTarget[i].SrcBlend = D3D11_BLEND_ONE;      bd.RenderTarget[i].DestBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[i].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[i].RenderTargetWriteMask = 0;
    }
    if (FAILED(hr = dev->CreateBlendState(&bd, &g_blend))) { LogErr("hidden area mask: CreateBlendState hr=0x%lx", hr); return false; }
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(hr = dev->CreateRasterizerState(&rd, &g_rs))) { LogErr("hidden area mask: CreateRasterizerState hr=0x%lx", hr); return false; }
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = FALSE;
    dd.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
    dd.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
    dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    dd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    dd.BackFace = dd.FrontFace;
    if (FAILED(hr = dev->CreateDepthStencilState(&dd, &g_dss))) { LogErr("hidden area mask: CreateDepthStencilState hr=0x%lx", hr); return false; }
    g_failed = false;
    return true;
}

bool Upload(ID3D11Device* dev, int slot, const float* t, uint32_t tris, float depth) {
    if (slot < 0 || slot > 1 || !dev) return false;
    Mesh& m = g_mesh[slot];
    m.vb.Reset();
    m.verts = 0;
    if (!t || !tris || tris > kMaxTris) return false;
    static float v[3 * 3 * kMaxTris];                 // (render thread only)
    const uint32_t n = 3 * tris;
    for (uint32_t k = 0; k < n; ++k) { v[3 * k] = t[2 * k]; v[3 * k + 1] = t[2 * k + 1]; v[3 * k + 2] = depth; }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = n * 3 * sizeof(float);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = v;
    const HRESULT hr = dev->CreateBuffer(&bd, &sd, &m.vb);
    if (FAILED(hr) || !m.vb) {
        m.vb.Reset();
        LogErr("hidden area mask: vertex buffer for view %d (%u triangles) FAILED hr=0x%lx", slot, tris, hr);
        return false;
    }
    m.verts = n;
    return true;
}

bool HasMesh(int slot) { return slot >= 0 && slot <= 1 && g_mesh[slot].vb && g_mesh[slot].verts; }

bool Draw(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, uint32_t w, uint32_t h, int slot) {
    if (!ctx || !dsv || !w || !h || !HasMesh(slot) || !g_vs) return false;
    PreviewBlit::SaveState(ctx);
    ID3D11Buffer* vb0 = nullptr;                      // (not in SaveState: the blit draws from SV_VertexID)
    UINT stride0 = 0, off0 = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb0, &stride0, &off0);
    ctx->OMSetRenderTargets(0, nullptr, dsv);
    D3D11_VIEWPORT vp{};
    vp.Width = (float)w; vp.Height = (float)h; vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    const FLOAT bf[4] = { 0, 0, 0, 0 };
    ID3D11Buffer* vb = g_mesh[slot].vb.Get();
    const UINT stride = 3 * sizeof(float), off = 0;
    ctx->IASetInputLayout(g_il.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
    ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(nullptr, nullptr, 0);
    ctx->RSSetState(g_rs.Get());
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetBlendState(g_blend.Get(), bf, 0xffffffffu);
    ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    ctx->Draw(g_mesh[slot].verts, 0);                 // (hooked: passes straight through inside t_inDlaa)
    ctx->IASetVertexBuffers(0, 1, &vb0, &stride0, &off0);
    if (vb0) vb0->Release();
    PreviewBlit::RestoreState(ctx);
    return true;
}
#endif // HM_NO_D3D

} // namespace HiddenMask

#endif // WITH_DLAA
