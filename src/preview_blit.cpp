// PreviewBlit implementation. See preview_blit.h.
#ifdef WITH_DLAA

#include "preview_blit.h"
#include "shader_cache.h"
#include "log.h"
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

// Fullscreen triangle from SV_VertexID (no input layout, no vertex buffer); the viewport clips it to the target.
// kPvBlitVs-BEGIN (the build validates this block with fxc, vs_5_0 / VSMain)
const char kPvBlitVs[] = R"(
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    const float2 t = float2((id << 1) & 2, id & 2);          // (0,0) (2,0) (0,2)
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}
)";
// kPvBlitVs-END

// One exact Load per pixel (source and target have the same size, viewport at the origin): a plain copy.
// kPvBlitPs-BEGIN (the build validates this block with fxc, ps_5_0 / PSMain)
const char kPvBlitPs[] = R"(
Texture2D<float4> Src : register(t0);

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    return Src.Load(int3(int2(pos.xy), 0));
}
)";
// kPvBlitPs-END

ComPtr<ID3D11VertexShader>      g_vs;
ComPtr<ID3D11PixelShader>       g_ps;
ComPtr<ID3D11BlendState>        g_blend;      // no blending, RGB write mask (alpha untouched)
ComPtr<ID3D11RasterizerState>   g_rs;         // solid, no cull, no scissor
ComPtr<ID3D11DepthStencilState> g_dss;        // depth + stencil off
ID3D11Device*                   g_dev = nullptr;
bool                            g_failed = false;

constexpr UINT kInst = 256;
constexpr UINT kMaxVp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
struct Saved {
    ID3D11InputLayout*        il = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY  topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader*       vs = nullptr; ID3D11ClassInstance* vsI[kInst] = {}; UINT vsN = kInst;
    ID3D11HullShader*         hs = nullptr; ID3D11ClassInstance* hsI[kInst] = {}; UINT hsN = kInst;
    ID3D11DomainShader*       ds = nullptr; ID3D11ClassInstance* dsI[kInst] = {}; UINT dsN = kInst;
    ID3D11GeometryShader*     gs = nullptr; ID3D11ClassInstance* gsI[kInst] = {}; UINT gsN = kInst;
    ID3D11PixelShader*        ps = nullptr; ID3D11ClassInstance* psI[kInst] = {}; UINT psN = kInst;
    ID3D11ShaderResourceView* srv0 = nullptr;
    ID3D11RasterizerState*    rs = nullptr;
    D3D11_VIEWPORT            vp[kMaxVp] = {};
    UINT                      vpN = 0;
    ID3D11BlendState*         bs = nullptr; FLOAT bf[4] = {}; UINT sampleMask = 0xffffffffu;
    ID3D11DepthStencilState*  dss = nullptr; UINT stencilRef = 0;
    ID3D11RenderTargetView*   rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView*   dsv = nullptr;
};
Saved g_saved;

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
void RelInst(ID3D11ClassInstance** a, UINT n) { for (UINT i = 0; i < n && i < kInst; ++i) Rel(a[i]); }

} // namespace

namespace PreviewBlit {

void RegisterShaders() {
    ShaderCache::Add(ShaderCache::kPvBlitVs, kPvBlitVs, sizeof(kPvBlitVs) - 1, "pv_blit_vs", "VSMain", "vs_5_0");
    ShaderCache::Add(ShaderCache::kPvBlitPs, kPvBlitPs, sizeof(kPvBlitPs) - 1, "pv_blit_ps", "PSMain", "ps_5_0");
}

bool Ensure(ID3D11Device* dev) {
    if (g_dev == dev && g_vs && g_ps && g_blend && g_rs && g_dss) return true;
    if (g_failed && g_dev == dev) return false;
    if (!ShaderCache::Done()) return false;               // v0.7.8: warm-up still running (callers gate on it first)
    g_failed = true; g_dev = dev;
    g_vs.Reset(); g_ps.Reset(); g_blend.Reset(); g_rs.Reset(); g_dss.Reset();
    HRESULT hr;
    // v0.7.8: bytecode from ShaderCache (compiled once on its worker thread), only the device objects are made here.
    size_t vsSize = 0, psSize = 0;
    const void* vsCode = ShaderCache::Code(ShaderCache::kPvBlitVs, &vsSize);
    const void* psCode = ShaderCache::Code(ShaderCache::kPvBlitPs, &psSize);
    if (!vsCode) { Log("preview blit VS D3DCompile failed %s", ShaderCache::Error(ShaderCache::kPvBlitVs)); return false; }
    if (FAILED(hr = dev->CreateVertexShader(vsCode, vsSize, nullptr, &g_vs))) {
        Log("preview blit CreateVertexShader hr=0x%lx", hr); return false;
    }
    if (!psCode) { Log("preview blit PS D3DCompile failed %s", ShaderCache::Error(ShaderCache::kPvBlitPs)); return false; }
    if (FAILED(hr = dev->CreatePixelShader(psCode, psSize, nullptr, &g_ps))) {
        Log("preview blit CreatePixelShader hr=0x%lx", hr); return false;
    }
    D3D11_BLEND_DESC bd{};
    bd.IndependentBlendEnable = TRUE;
    for (int i = 0; i < 8; ++i) {
        bd.RenderTarget[i].BlendEnable = FALSE;
        bd.RenderTarget[i].SrcBlend = D3D11_BLEND_ONE;       bd.RenderTarget[i].DestBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[i].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;  bd.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[i].RenderTargetWriteMask = i == 0 ? (D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                                             D3D11_COLOR_WRITE_ENABLE_BLUE) : 0;
    }
    if (FAILED(hr = dev->CreateBlendState(&bd, &g_blend))) { Log("preview blit CreateBlendState hr=0x%lx", hr); return false; }
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(hr = dev->CreateRasterizerState(&rd, &g_rs))) { Log("preview blit CreateRasterizerState hr=0x%lx", hr); return false; }
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = FALSE;
    dd.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
    dd.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
    dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp = dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    dd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
    dd.BackFace = dd.FrontFace;
    if (FAILED(hr = dev->CreateDepthStencilState(&dd, &g_dss))) { Log("preview blit CreateDepthStencilState hr=0x%lx", hr); return false; }
    g_failed = false;
    return true;
}

void SaveState(ID3D11DeviceContext* ctx) {
    Saved& s = g_saved;
    s.vsN = s.hsN = s.dsN = s.gsN = s.psN = kInst;
    ctx->IAGetInputLayout(&s.il);
    ctx->IAGetPrimitiveTopology(&s.topo);
    ctx->VSGetShader(&s.vs, s.vsI, &s.vsN);
    ctx->HSGetShader(&s.hs, s.hsI, &s.hsN);
    ctx->DSGetShader(&s.ds, s.dsI, &s.dsN);
    ctx->GSGetShader(&s.gs, s.gsI, &s.gsN);
    ctx->PSGetShader(&s.ps, s.psI, &s.psN);
    ctx->PSGetShaderResources(0, 1, &s.srv0);
    ctx->RSGetState(&s.rs);
    s.vpN = 0;
    ctx->RSGetViewports(&s.vpN, nullptr);
    if (s.vpN > kMaxVp) s.vpN = kMaxVp;
    if (s.vpN) ctx->RSGetViewports(&s.vpN, s.vp);
    ctx->OMGetBlendState(&s.bs, s.bf, &s.sampleMask);
    ctx->OMGetDepthStencilState(&s.dss, &s.stencilRef);
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, &s.dsv);
}

void RestoreState(ID3D11DeviceContext* ctx) {
    Saved& s = g_saved;
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);                // our SRV off before the game's render targets come back
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, s.dsv);
    ctx->IASetInputLayout(s.il);
    ctx->IASetPrimitiveTopology(s.topo);
    ctx->VSSetShader(s.vs, s.vsI, s.vsN);
    ctx->HSSetShader(s.hs, s.hsI, s.hsN);
    ctx->DSSetShader(s.ds, s.dsI, s.dsN);
    ctx->GSSetShader(s.gs, s.gsI, s.gsN);
    ctx->PSSetShader(s.ps, s.psI, s.psN);
    ctx->PSSetShaderResources(0, 1, &s.srv0);
    ctx->RSSetState(s.rs);
    ctx->RSSetViewports(s.vpN, s.vpN ? s.vp : nullptr);
    ctx->OMSetBlendState(s.bs, s.bf, s.sampleMask);
    ctx->OMSetDepthStencilState(s.dss, s.stencilRef);
    Rel(s.il);
    Rel(s.vs); RelInst(s.vsI, s.vsN);
    Rel(s.hs); RelInst(s.hsI, s.hsN);
    Rel(s.ds); RelInst(s.dsI, s.dsN);
    Rel(s.gs); RelInst(s.gsI, s.gsN);
    Rel(s.ps); RelInst(s.psI, s.psN);
    Rel(s.srv0);
    Rel(s.rs); Rel(s.bs); Rel(s.dss);
    for (ID3D11RenderTargetView*& r : s.rtv) Rel(r);
    Rel(s.dsv);
}

void Draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, uint32_t w, uint32_t h) {
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);                // the previous source may be the resource we now bind as RT
    ID3D11RenderTargetView* rtv = dst;
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width = (float)w; vp.Height = (float)h; vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    const FLOAT bf[4] = { 0, 0, 0, 0 };
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(g_ps.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &src);
    ctx->RSSetState(g_rs.Get());
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetBlendState(g_blend.Get(), bf, 0xffffffffu);
    ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    ctx->Draw(3, 0);                                           // (hooked: passes straight through inside t_inDlaa)
    ctx->PSSetShaderResources(0, 1, &nullSrv);
}

} // namespace PreviewBlit

#endif // WITH_DLAA
