// census_paint.h -- phase 115: the census view's PAINT, a flashing checkerboard drawn into the blit source AFTER DLAA.
//
// WHY (the user, VR + flat drives after phase 114): "I hardly noticed the yellows. Flags were yellow and I thought they were just
// normal flags until I toggled it off and on. It must be IN YOUR FACE so people capture it." Until phase 114 the census tinted a
// suspect draw by drawing it again with one colour at 70 % into the game's own targets: the G-buffer ALBEDO (then lit by the sun, the
// fog, the exposure, the tonemap) or the forward HDR colour -- a dim, lit, gold-ish wash that DLAA then averaged with its history.
// HOW (phase 115):
//   1. During the world pass every suspect draw is re-drawn into the census MASK (src/census_run.h MaskTint): an R32_UINT target of
//      the pass's depth size, bound ALONE (the game's colour targets untouched) with the game's depth buffer and a twin of its depth
//      state (EQUAL for a draw that wrote depth). The mask PS writes one word: the kind + the draw's depth (Census::MaskWord).
//   2. At the eye / screen blit -- after DLAA ran (before the tonemap: the HDR unit; or at this blit: the copy-back) and after the
//      game's tonemap -- Paint() draws one full-screen triangle into the blit SOURCE: where the mask has a word whose depth still
//      matches the pass's final depth, the kind's colour or black by a screen-space checker, the phase swapped 4 times a second.
//      The game's blit then scales it to the eye / back buffer like the rest of the picture.
//   DLAA never sees the checker (it ran before), so nothing smooths it or trails it: the squares are exactly the kind's screen colour
//   and exactly black, in both VR eyes with the same phase (the phase is taken once per Present).
// Pure D3D11 (no inject.cpp state): tests/census_test.cpp runs it on WARP. All state it touches is saved and restored.
#pragma once
#include <d3d11.h>
#include <cstdint>
#include <cstring>
#include "census.h"

namespace CensusPaint {

struct Params {
    uint32_t maskW = 0, maskH = 0;   // the mask (= the pass's depth) size
    uint32_t dstW = 0, dstH = 0;     // the blit source size
    int      cell = 8;               // checker square side, dst px (Census::PaintCell)
    int      phase = 0;              // 0 / 1 (Census::PaintPhase)
    bool     depth = false;          // a final-depth SRV is bound (t1): words with a depth are checked against it
    float    ox = 0.0f, oy = 0.0f;   // checker anchor in dst px (VR: the optical centre)
};
struct CbData { float dim[4]; float prm[4]; float org[4]; float col[5][4]; };
static_assert(sizeof(CbData) == 128, "PSPaint's cbuffer Paint");
inline void FillCb(CbData& cb, const Params& p, bool srgbBytes) {
    memset(&cb, 0, sizeof(cb));
    cb.dim[0] = (float)p.maskW; cb.dim[1] = (float)p.maskH; cb.dim[2] = (float)p.dstW; cb.dim[3] = (float)p.dstH;
    cb.prm[0] = (float)(p.cell > 0 ? p.cell : 8); cb.prm[1] = (float)(p.phase & 1); cb.prm[2] = p.depth ? 1.0f : 0.0f;
    cb.prm[3] = srgbBytes ? 1.0f : 0.0f;
    cb.org[0] = p.ox; cb.org[1] = p.oy;
    for (int k = 0; k < Census::kKindN; ++k) { Census::KindColour(k, cb.col[k]); cb.col[k][3] = 1.0f; }
}
// The view format to paint a texture of format f with, and whether the shader must write sRGB-encoded values (an 8-bit / 10-bit
// UNORM view: the bytes ARE the screen colour) or linear ones (an _SRGB view encodes them; a float target holds linear light).
// DXGI_FORMAT_UNKNOWN = not paintable.
inline DXGI_FORMAT ViewFormat(DXGI_FORMAT f, bool* srgbBytes) {
    *srgbBytes = true;
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: *srgbBytes = false; return f;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: *srgbBytes = false; return f;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: *srgbBytes = false; return f;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT: *srgbBytes = false; return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R11G11B10_FLOAT: *srgbBytes = false; return f;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: *srgbBytes = false; return f;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// device objects + a small cache of render target views on the blit sources (each view holds its texture: a cached pointer can
// never be a reused address; Release() drops them -- the view off / a device change)
struct Kit {
    ID3D11Device*           dev = nullptr;
    ID3D11VertexShader*     vs = nullptr;
    ID3D11PixelShader*      ps = nullptr;
    ID3D11Buffer*           cb = nullptr;
    ID3D11RasterizerState*  rs = nullptr;
    ID3D11DepthStencilState* dss = nullptr;
    ID3D11BlendState*       bs = nullptr;
    bool                    failed = false;
    CbData                  last{};
    bool                    lastValid = false;
    struct Rt { ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr; bool srgbBytes = true; uint64_t used = 0; };
    static constexpr int    kRts = 8;
    Rt                      rt[kRts];
    uint64_t                tick = 0;
    uint64_t                paints = 0, refused = 0;

    void ReleaseViews() {
        for (Rt& r : rt) { if (r.rtv) r.rtv->Release(); if (r.tex) r.tex->Release(); r = Rt{}; }
    }
    void Release() {
        ReleaseViews();
        if (vs) vs->Release(); if (ps) ps->Release(); if (cb) cb->Release();
        if (rs) rs->Release(); if (dss) dss->Release(); if (bs) bs->Release();
        vs = nullptr; ps = nullptr; cb = nullptr; rs = nullptr; dss = nullptr; bs = nullptr;
        dev = nullptr; failed = false; lastValid = false;
    }
    // vsCode / psCode = VSPaint / PSPaint bytecode (ShaderCache, or D3DCompile in the test)
    bool Ensure(ID3D11Device* d, const void* vsCode, size_t vsLen, const void* psCode, size_t psLen) {
        if (dev && dev != d) Release();
        if (vs && ps && cb && rs && dss && bs) return true;
        if (failed || !d || !vsCode || !psCode) return false;
        dev = d;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(CbData); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = FALSE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        D3D11_BLEND_DESC bl{};
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                                   D3D11_COLOR_WRITE_ENABLE_BLUE;   // (the game's alpha stays)
        if (FAILED(d->CreateVertexShader(vsCode, vsLen, nullptr, &vs)) || FAILED(d->CreatePixelShader(psCode, psLen, nullptr, &ps)) ||
            FAILED(d->CreateBuffer(&bd, nullptr, &cb)) || FAILED(d->CreateRasterizerState(&rd, &rs)) ||
            FAILED(d->CreateDepthStencilState(&dd, &dss)) || FAILED(d->CreateBlendState(&bl, &bs))) {
            Release();
            failed = true;
            return false;
        }
        return true;
    }
    // phase 115r (audit B11): a view not painted through for kViewStale paints is dropped (it held its texture -- an old swap chain /
    // eye texture -- until the view went off)
    static constexpr uint64_t kViewStale = 600;
    void DropStale() {
        for (Rt& r : rt)
            if (r.rtv && tick - r.used > kViewStale) { r.rtv->Release(); if (r.tex) r.tex->Release(); r = Rt{}; }
    }
    ID3D11RenderTargetView* ViewFor(ID3D11Texture2D* t, bool* srgbBytes) {
        ++tick;
        if ((tick & 63u) == 0) DropStale();
        for (Rt& r : rt) if (r.tex == t && r.rtv) { r.used = tick; *srgbBytes = r.srgbBytes; return r.rtv; }
        D3D11_TEXTURE2D_DESC td{};
        t->GetDesc(&td);
        if (!(td.BindFlags & D3D11_BIND_RENDER_TARGET) || td.SampleDesc.Count != 1 || td.ArraySize != 1) return nullptr;
        bool sb = true;
        const DXGI_FORMAT vf = ViewFormat(td.Format, &sb);
        if (vf == DXGI_FORMAT_UNKNOWN) return nullptr;
        D3D11_RENDER_TARGET_VIEW_DESC v{};
        v.Format = vf; v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; v.Texture2D.MipSlice = 0;
        ID3D11RenderTargetView* rtv = nullptr;
        if (FAILED(dev->CreateRenderTargetView(t, &v, &rtv)) || !rtv) return nullptr;
        Rt* slot = &rt[0];
        for (Rt& r : rt) { if (!r.rtv) { slot = &r; break; } if (r.used < slot->used) slot = &r; }
        if (slot->rtv) slot->rtv->Release();
        if (slot->tex) slot->tex->Release();
        t->AddRef();
        *slot = Rt{ t, rtv, sb, tick };
        *srgbBytes = sb;
        return rtv;
    }
};

// Paints the mask into `dst` (see the top). mask = the R32_UINT mask's SRV, depth = the pass's final depth as R32_FLOAT (null = no
// depth check: every word shows). Every touched state is saved and restored (render targets with the UAVs kept, shaders + class
// instances, PS t0..t15 / b0, the input layout + topology, rasterizer state + viewports, blend + factor + mask, depth-stencil + ref).
// False = not painted (no view on dst: its format / bind flags; the kit not ready).
inline bool Paint(ID3D11DeviceContext* ctx, Kit& kit, ID3D11Texture2D* dst, ID3D11ShaderResourceView* mask,
                  ID3D11ShaderResourceView* depth, Params p) {
    if (!ctx || !dst || !mask || !kit.vs || !kit.ps) return false;
    bool srgbBytes = true;
    ID3D11RenderTargetView* const rtv = kit.ViewFor(dst, &srgbBytes);
    if (!rtv) { ++kit.refused; return false; }
    if (!depth) p.depth = false;
    CbData cbd;
    FillCb(cbd, p, srgbBytes);
    if (!kit.lastValid || memcmp(&cbd, &kit.last, sizeof(cbd)) != 0) {
        ctx->UpdateSubresource(kit.cb, 0, nullptr, &cbd, 0, 0);
        kit.last = cbd;
        kit.lastValid = true;
    }
    // ---- save
    ID3D11RenderTargetView* gRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* gDsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, gRtv, &gDsv);
    ID3D11VertexShader* gVs = nullptr; ID3D11ClassInstance* gVsCi[8] = {}; UINT nVsCi = 8;
    ctx->VSGetShader(&gVs, gVsCi, &nVsCi);
    ID3D11PixelShader* gPs = nullptr; ID3D11ClassInstance* gPsCi[8] = {}; UINT nPsCi = 8;
    ctx->PSGetShader(&gPs, gPsCi, &nPsCi);
    ID3D11HullShader* gHs = nullptr; ID3D11ClassInstance* gHsCi[8] = {}; UINT nHsCi = 8;
    ctx->HSGetShader(&gHs, gHsCi, &nHsCi);
    ID3D11DomainShader* gDs = nullptr; ID3D11ClassInstance* gDsCi[8] = {}; UINT nDsCi = 8;
    ctx->DSGetShader(&gDs, gDsCi, &nDsCi);
    ID3D11GeometryShader* gGs = nullptr; ID3D11ClassInstance* gGsCi[8] = {}; UINT nGsCi = 8;
    ctx->GSGetShader(&gGs, gGsCi, &nGsCi);
    ID3D11ShaderResourceView* gSrv[16] = {};
    ctx->PSGetShaderResources(0, 16, gSrv);
    ID3D11Buffer* gCb = nullptr;
    ctx->PSGetConstantBuffers(0, 1, &gCb);
    ID3D11InputLayout* gIl = nullptr;
    ctx->IAGetInputLayout(&gIl);
    D3D11_PRIMITIVE_TOPOLOGY gTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&gTopo);
    ID3D11RasterizerState* gRs = nullptr;
    ctx->RSGetState(&gRs);
    D3D11_VIEWPORT gVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT nVp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&nVp, gVp);
    ID3D11BlendState* gBs = nullptr; FLOAT gBf[4] = {}; UINT gMask = 0;
    ctx->OMGetBlendState(&gBs, gBf, &gMask);
    ID3D11DepthStencilState* gDss = nullptr; UINT gRef = 0;
    ctx->OMGetDepthStencilState(&gDss, &gRef);
    // ---- the paint (binding dst as a target unbinds it as the game's blit input t0: restored below with the saved views)
    ID3D11RenderTargetView* ours = rtv;
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &ours, nullptr, 1, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
    const D3D11_VIEWPORT vp{ 0.0f, 0.0f, (float)p.dstW, (float)p.dstH, 0.0f, 1.0f };
    ID3D11ShaderResourceView* srv[2] = { mask, p.depth ? depth : nullptr };
    ID3D11Buffer* cb = kit.cb;
    const FLOAT bf[4] = { 0, 0, 0, 0 };
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(kit.vs, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(kit.ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 2, srv);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->RSSetState(kit.rs);
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetBlendState(kit.bs, bf, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(kit.dss, 0);
    ctx->Draw(3, 0);
    ++kit.paints;
    // ---- restore (the targets first: the saved t0 may be dst itself)
    UINT nrt = 0;
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) if (gRtv[i]) nrt = i + 1;
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(nrt, nrt ? gRtv : nullptr, gDsv, nrt, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
    ctx->VSSetShader(gVs, nVsCi ? gVsCi : nullptr, nVsCi);
    ctx->PSSetShader(gPs, nPsCi ? gPsCi : nullptr, nPsCi);
    ctx->HSSetShader(gHs, nHsCi ? gHsCi : nullptr, nHsCi);
    ctx->DSSetShader(gDs, nDsCi ? gDsCi : nullptr, nDsCi);
    ctx->GSSetShader(gGs, nGsCi ? gGsCi : nullptr, nGsCi);
    ctx->PSSetShaderResources(0, 16, gSrv);
    ctx->PSSetConstantBuffers(0, 1, &gCb);
    ctx->IASetInputLayout(gIl);
    ctx->IASetPrimitiveTopology(gTopo);
    ctx->RSSetState(gRs);
    ctx->RSSetViewports(nVp, nVp ? gVp : nullptr);        // (none bound before = none after)
    ctx->OMSetBlendState(gBs, gBf, gMask);
    ctx->OMSetDepthStencilState(gDss, gRef);
    // ---- release the getters' references
    for (auto* r : gRtv) if (r) r->Release();
    if (gDsv) gDsv->Release();
    if (gVs) gVs->Release();
    if (gPs) gPs->Release();
    if (gHs) gHs->Release();
    if (gDs) gDs->Release();
    if (gGs) gGs->Release();
    for (UINT i = 0; i < 8; ++i) {
        if (i < nVsCi && gVsCi[i]) gVsCi[i]->Release();
        if (i < nPsCi && gPsCi[i]) gPsCi[i]->Release();
        if (i < nHsCi && gHsCi[i]) gHsCi[i]->Release();
        if (i < nDsCi && gDsCi[i]) gDsCi[i]->Release();
        if (i < nGsCi && gGsCi[i]) gGsCi[i]->Release();
    }
    for (auto* s : gSrv) if (s) s->Release();
    if (gCb) gCb->Release();
    if (gIl) gIl->Release();
    if (gRs) gRs->Release();
    if (gBs) gBs->Release();
    if (gDss) gDss->Release();
    return true;
}

}  // namespace CensusPaint
