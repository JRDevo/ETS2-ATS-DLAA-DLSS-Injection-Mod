// SceneDlaa implementation. See scene_dlaa.h and docs/DLAA_INTEGRATION.md.
#ifdef WITH_DLAA

#include "scene_dlaa.h"
#include "shader_cache.h"
#include "log.h"
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

// Scene depth is D32_FLOAT_S8X24_UINT with no SRV bind, so we copy it into an
// R32G8X24_TYPELESS twin and read the depth plane through an
// R32_FLOAT_X8X24_TYPELESS SRV; NGX wants a plain single-channel float
// texture, so this pass flattens it into R32_FLOAT. Values are untouched
// (reversed-Z, 1.0 = near; DLSS gets DepthInverted at feature creation).
// v0.6.4 DLAA area: DepthOut is crop-sized (dispatch = crop), the twin is full-size and read at Origin + id
// (b0). Whole image: Origin 0 and DepthOut = DepthIn size (same as v0.6.3).
// kDepthConvertShader-BEGIN (the build validates this block with fxc)
const char kDepthConvertShader[] = R"(
cbuffer ConvCB : register(b0) {
    uint2 Origin;        // v0.6.4: crop origin in the full-size depth twin (px)
    uint2 ConvPad;
};
Texture2D<float>   DepthIn  : register(t0);
RWTexture2D<float> DepthOut : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    DepthOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    DepthOut[id.xy] = DepthIn[id.xy + Origin];
}
)";
// kDepthConvertShader-END

// MV debug view (INSERT key): color = (0.5 + mv.x/16, 0.5 + mv.y/16, cabin ? 1 : 0).
// Gray = no motion. Cabin = depth >= 0.9 (same split as the reprojection pass).
// v0.6.0: blue = 0.5 for ego pixels (world layer, view depth < EgoPixelM -> R_ego). The classification is
// redone here from CameraMv's solve buffer (t2, InvRow3 = Solve[16]) and its dims cbuffer (b0, same layout
// as pass B); both are bound only when the merged MV pass ran this frame, otherwise unbound (reads 0 = no ego).
// v0.6.4: crop-sized like the MV texture; uv / ndc from the crop origin + full size (same cbuffer as pass B).
// v0.7.0: the dispatch covers the OUTPUT texture (Out = m_out, output-rect-sized when upscaling); each output
// pixel reads the render-size MV / depth texel it covers (src = floor((id + 0.5) * render / output)). Without
// upscaling Out and MvTex have the same size, so src == id exactly (v0.6.5 output unchanged).
// kMvDebugShader-BEGIN (the build validates this block with fxc)
const char kMvDebugShader[] = R"(
cbuffer DimsCB : register(b0) {
    float2 Size;
    float  EgoPixelM;
    float  _pad;
    float2 Origin;       // v0.6.4: crop origin in the full image (px)
    float2 FullSize;     // v0.6.4: full image size (px)
};
Texture2D<float2>        MvTex    : register(t0);
Texture2D<float>         DepthTex : register(t1);
StructuredBuffer<float4> Solve    : register(t2);
RWTexture2D<float4>      Out      : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint w, h, ow, oh;
    MvTex.GetDimensions(w, h);
    Out.GetDimensions(ow, oh);
    if (id.x >= ow || id.y >= oh) return;
    const uint2 src = min(uint2((float2(id.xy) + 0.5) * (float2(w, h) / float2(ow, oh))), uint2(w - 1, h - 1));
    float2 mv = MvTex[src];
    float  d  = DepthTex[src];
    float  cls = (d >= 0.9) ? 1.0 : 0.0;
    if (d < 0.9 && d >= 0.01 && EgoPixelM > 0.0) {
        float4 ir = Solve[16];
        if (any(ir != 0.0)) {
            float2 uv   = (float2(src) + Origin + 0.5) / FullSize;
            float2 ndc  = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
            float  z    = saturate((d - 0.01) / 0.89);
            float  iw   = dot(ir, float4(ndc, z, 1.0));
            float  dist = (abs(iw) > 1e-9) ? 1.0 / iw : 1e9;
            if (dist > 0.0 && dist < EgoPixelM) cls = 0.5;
        }
    }
    Out[id.xy] = float4(saturate(0.5 + mv.x / 16.0), saturate(0.5 + mv.y / 16.0), cls, 1.0);
}
)";
// kMvDebugShader-END

// v0.5.6 sharpen: AMD FidelityFX FSR1 RCAS (robust contrast-adaptive sharpening), compute version.
// Works directly on the gamma-encoded NGX output (R8G8B8A8_UNORM bytes, no linearisation). 5-tap cross;
// the negative lobe is limited by the min/max of the 4 ring taps (no clipping past the ring's range),
// clamped to the RCAS limit 0.25 - 1/16 and scaled by the user strength. One lobe for all channels, from
// a green-weighted luma (0.25 r + 0.5 g + 0.25 b). No noise-removal branch. Alpha = centre tap.
// v0.5.8: the 4 ring taps are taken at centre +- Radius texels (b0) with bilinear filtering through a
// linear-clamp sampler (s0), so a fractional radius averages two neighbours and the supersampled eye image
// is sharpened at a visible width; the centre tap stays an exact Load. Clamp addressing replaces the manual
// edge clamps. Strength + radius come from the constant buffer (b0), never recompiled.
// v0.6.4 DLAA area border blend (Blend = 1 only when the crop is smaller than the image; this pass then ALWAYS
// runs, at Sharpness 0 the lobe is 0 and c = the NGX colour exactly): result = lerp(Orig, sharpened, mask),
// Orig = colorIn (t1, the raw jittered frame = what surrounds the rect). mask = PRODUCT of a per-axis ramp:
// per axis min over its two edges of saturate(distance to that edge / Feather), each axis ramp smoothstep'd;
// an edge that coincides with the image border (EdgeOn = 0) is not feathered (ramp 1). Feather 0 = hard edge.
// Blend = 0: the v0.6.3 pass, same output (Orig unbound, never read).
// v0.8.0 HDR unit (Mode.x = 1; RGBA16F linear input, values above 1): RCAS's limiters assume 0..1, and sharpening the
// linear value rings hard around highlights. Every tap is first mapped through the REVERSIBLE tonemapper
// c / (1 + max3(c)) (into 0..1, the one AMD recommends for running FSR / RCAS on HDR input), the unchanged RCAS math
// (lobe + 5-tap min/max clamp) runs on those values, and the result is mapped back with c / (1 - max3(c)). No 0..1
// saturate in HDR (the 5-tap clamp already bounds the result); the border blend lerps in linear space. Mode.x = 0:
// exactly the LDR code path of v0.7.x.
// kRcasShader-BEGIN (the build validates this block with fxc)
const char kRcasShader[] = R"(
cbuffer RcasCB : register(b0) {
    float  Sharpness;       // 0..1 user strength (final lobe scale)
    float  Radius;          // v0.5.8 ring-tap radius in texels (1..4), bilinear
    float  Feather;         // v0.6.4 border blend width in px (0 = hard edge)
    float  Blend;           // v0.6.4 1 = blend into Orig at the rect border (area < 100), 0 = off
    float4 EdgeOn;          // v0.6.4 (left, top, right, bottom): 1 = that rect edge is inside the image
    float4 Mode;            // v0.8.0 x: 1 = HDR (linear RGBA16F, sharpen a compressed value), 0 = LDR; yzw unused
};
Texture2D<float4>   Src                : register(t0);
Texture2D<float4>   Orig               : register(t1);   // v0.6.4 colorIn (only read when Blend = 1)
RWTexture2D<float4> Dst                : register(u0);
SamplerState        LinearClampSampler : register(s0);

static const float kRcasLimit = 0.25 - (1.0 / 16.0);

float Luma(float3 c) { return 0.25 * c.r + 0.5 * c.g + 0.25 * c.b; }
float Max3(float3 c) { return max(c.r, max(c.g, c.b)); }
// v0.8.0 reversible tonemapper (HDR only): linear >= 0 -> [0, 1) and back (exact inverse for max3 >= 0).
float3 HdrCompress(float3 c) { return c / (1.0 + max(Max3(c), 0.0)); }
float3 HdrExpand(float3 c)   { return c / max(1.0 - max(Max3(c), 0.0), 1.0e-5); }

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
    float3 res = hdr ? min(HdrExpand(c), 65504.0) : saturate(c);   // v0.8.0 HDR: back to linear, no 0..1 clamp
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
        res = lerp(Orig.Load(int3(p, 0)).rgb, res, mask);
    }
    Dst[id.xy] = float4(res, e.a);
}
)";
// kRcasShader-END

// v0.7.0 upscale composite: fullscreen triangle from SV_VertexID (no input layout, no vertex buffer). The
// viewport (the output rect inside the game's RT0) clips it to exactly the rect.
// kCompositeVs-BEGIN (the build validates this block with fxc, vs_5_0 / VSMain)
const char kCompositeVs[] = R"(
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    const float2 t = float2((id << 1) & 2, id & 2);          // (0,0) (2,0) (0,2)
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}
)";
// kCompositeVs-END

// v0.7.0 upscale composite PS: one exact Load per output pixel (our texture is viewport-sized, so 1:1). The
// texels are the raw encoded bytes (UNORM view, like the scene texture's bytes); when the game's blit reads
// the scene through an _SRGB SRV, SrgbDecode = 1 linearises them the same way, so the game's own RTV (bound,
// untouched) re-encodes exactly like it does for the game's blit. Alpha = border feather ramp (v0.6.4 rule:
// per axis min over its two edges of saturate(distance / Feather), smoothstep'd, the axes multiplied; an edge
// on the image border (EdgeOn = 0) is not feathered; Blend = 0 or Feather = 0 -> alpha 1).
// Params (t1, Buffer<float4>; no constant-buffer slot is touched): [0] = RT-space origin of the rect (px) and
// its size, [1] = feather x / y (output px), Blend, SrgbDecode, [2] = EdgeOn (left, top, right, bottom).
// kCompositePs-BEGIN (the build validates this block with fxc, ps_5_0 / PSMain)
const char kCompositePs[] = R"(
Texture2D<float4> Src    : register(t0);
Buffer<float4>    Params : register(t1);

float3 SrgbToLinear(float3 c) {
    return (c <= 0.04045) ? c / 12.92 : pow(abs((c + 0.055) / 1.055), 2.4);   // (UNORM texels: never negative)
}

float4 PSMain(float4 pos : SV_Position) : SV_Target {
    const float4 p0 = Params[0];
    const float4 p1 = Params[1];
    const float4 e  = Params[2];
    const float2 size = p0.zw;
    const int2   ip   = clamp(int2(pos.xy - p0.xy), int2(0, 0), int2(size) - 1);
    float3 c = Src.Load(int3(ip, 0)).rgb;
    if (p1.w > 0.5) c = SrgbToLinear(c);
    float a = 1.0;
    if (p1.z > 0.5) {
        const float2 lo = float2(ip) + 0.5;                  // distance to the left / top rect edge (px)
        const float2 hi = size - lo;                         // ... to the right / bottom rect edge
        float rx = 1.0, ry = 1.0;
        if (p1.x > 0.0) rx = min(e.x > 0.5 ? saturate(lo.x / p1.x) : 1.0, e.z > 0.5 ? saturate(hi.x / p1.x) : 1.0);
        if (p1.y > 0.0) ry = min(e.y > 0.5 ? saturate(lo.y / p1.y) : 1.0, e.w > 0.5 ? saturate(hi.y / p1.y) : 1.0);
        a = smoothstep(0.0, 1.0, rx) * smoothstep(0.0, 1.0, ry);
    }
    return float4(c, a);
}
)";
// kCompositePs-END

// v0.5.6: GPU cost of the per-pass depth snapshot (DepthTwin::Snapshot's CopyResource). One small ring for
// all twins (render thread only), polled non-blocking at the next timed snapshot.
struct SnapTimer {
    struct Q { ComPtr<ID3D11Query> dis, t0, t1; bool inFlight = false; bool stale = false; };
    Q        q[4];
    int      head = 0;
    bool     broken = false;          // query creation failed: timing off for good
    double   msSum = 0.0, msLast = 0.0;
    uint64_t n = 0;

    void Poll(ID3D11DeviceContext* ctx) {
        for (Q& s : q) {
            if (!s.inFlight) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
            if (ctx->GetData(s.dis.Get(), &dd, sizeof(dd), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            UINT64 a = 0, b = 0;
            const bool got = ctx->GetData(s.t0.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                             ctx->GetData(s.t1.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
            if (!got) continue;
            s.inFlight = false;
            if (dd.Disjoint || !dd.Frequency) continue;
            if (s.stale) { s.stale = false; continue; }   // v0.5.7: queued before a window reset
            msLast = (double)(b - a) * 1000.0 / (double)dd.Frequency;
            msSum += msLast;
            if (++n % 600 == 0) {
                Log("depth snapshot GPU cost: avg %.2f ms (over the last 600 timed snapshots, last %.2f ms)",
                    msSum / 600.0, msLast);
                msSum = 0.0;
            }
        }
    }
    // v0.5.7: new window (after a live preset / sharpness change); in-flight samples are dropped.
    void ResetWindow() {
        msSum = 0.0; n = 0;
        for (Q& s : q) if (s.inFlight) s.stale = true;
    }
    // Returns the slot to End() after the copy, or -1 (all slots busy / no queries): copy untimed.
    int Begin(ID3D11DeviceContext* ctx) {
        if (broken) return -1;
        Poll(ctx);
        Q& s = q[head];
        if (s.inFlight) return -1;
        if (!s.dis) {
            ComPtr<ID3D11Device> dev;
            ctx->GetDevice(&dev);
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            if (!dev || FAILED(dev->CreateQuery(&qd, &s.dis))) { broken = true; return -1; }
            qd.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(dev->CreateQuery(&qd, &s.t0)) || FAILED(dev->CreateQuery(&qd, &s.t1))) { broken = true; return -1; }
        }
        ctx->Begin(s.dis.Get());
        ctx->End(s.t0.Get());
        return head;
    }
    void End(ID3D11DeviceContext* ctx, int slot) {
        if (slot < 0) return;
        ctx->End(q[slot].t1.Get());
        ctx->End(q[slot].dis.Get());
        q[slot].inFlight = true;
        q[slot].stale = false;
        head = (head + 1) % 4;
    }
};
SnapTimer g_snapTimer;

// Everything the compute stage can carry that we or NGX might disturb.
struct CsState {
    ID3D11ComputeShader*       cs = nullptr;
    ID3D11ClassInstance*       inst[256] = {};
    UINT                       nInst = 256;
    ID3D11ShaderResourceView*  srv[16] = {};
    ID3D11UnorderedAccessView* uav[8] = {};
    ID3D11Buffer*              cb[14] = {};
    ID3D11SamplerState*        smp[16] = {};

    void Save(ID3D11DeviceContext* ctx) {
        ctx->CSGetShader(&cs, inst, &nInst);
        ctx->CSGetShaderResources(0, 16, srv);
        ctx->CSGetUnorderedAccessViews(0, 8, uav);
        ctx->CSGetConstantBuffers(0, 14, cb);
        ctx->CSGetSamplers(0, 16, smp);
    }
    void Restore(ID3D11DeviceContext* ctx) {
        // (UINT)-1 = keep each UAV's current append/consume counter.
        const UINT keep[8] = { (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1,
                               (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1 };
        ctx->CSSetShader(cs, inst, nInst);
        ctx->CSSetShaderResources(0, 16, srv);
        ctx->CSSetUnorderedAccessViews(0, 8, uav, keep);
        ctx->CSSetConstantBuffers(0, 14, cb);
        ctx->CSSetSamplers(0, 16, smp);
        if (cs) cs->Release();
        for (UINT i = 0; i < nInst; ++i) if (inst[i]) inst[i]->Release();
        for (auto* p : srv) if (p) p->Release();
        for (auto* p : uav) if (p) p->Release();
        for (auto* p : cb)  if (p) p->Release();
        for (auto* p : smp) if (p) p->Release();
    }
};

// v0.7.0: every piece of graphics-pipeline state the upscale composite draw changes. Saved right before, put
// back right after (refs released), so the game's next draw sees exactly its own state. NOT touched by the
// composite and therefore not saved: vertex / index buffers (null input layout, SV_VertexID only), every
// constant-buffer slot (D3D11.1 offsets would not survive a legacy save/restore -- params come from an SRV),
// samplers (Load only), PS SRV slots 2..127, scissor rects (our RS state has scissor off), render targets /
// DSV / OM UAVs (we draw into the game's own, still-bound RT0), stream-out targets, predication.
struct GfxState {
    static constexpr UINT kInst = 256;
    ID3D11InputLayout*        il = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY  topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader*       vs = nullptr; ID3D11ClassInstance* vsI[kInst] = {}; UINT vsN = kInst;
    ID3D11HullShader*         hs = nullptr; ID3D11ClassInstance* hsI[kInst] = {}; UINT hsN = kInst;
    ID3D11DomainShader*       ds = nullptr; ID3D11ClassInstance* dsI[kInst] = {}; UINT dsN = kInst;
    ID3D11GeometryShader*     gs = nullptr; ID3D11ClassInstance* gsI[kInst] = {}; UINT gsN = kInst;
    ID3D11PixelShader*        ps = nullptr; ID3D11ClassInstance* psI[kInst] = {}; UINT psN = kInst;
    ID3D11ShaderResourceView* psSrv[2] = {};
    ID3D11RasterizerState*    rs = nullptr;
    D3D11_VIEWPORT            vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT                      vpN = 0;
    ID3D11BlendState*         bs = nullptr; FLOAT bf[4] = {}; UINT sampleMask = 0xffffffffu;
    ID3D11DepthStencilState*  dss = nullptr; UINT stencilRef = 0;

    void Save(ID3D11DeviceContext* ctx) {
        ctx->IAGetInputLayout(&il);
        ctx->IAGetPrimitiveTopology(&topo);
        ctx->VSGetShader(&vs, vsI, &vsN);
        ctx->HSGetShader(&hs, hsI, &hsN);
        ctx->DSGetShader(&ds, dsI, &dsN);
        ctx->GSGetShader(&gs, gsI, &gsN);
        ctx->PSGetShader(&ps, psI, &psN);
        ctx->PSGetShaderResources(0, 2, psSrv);
        ctx->RSGetState(&rs);
        vpN = 0;
        ctx->RSGetViewports(&vpN, nullptr);                  // count first
        if (vpN > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
            vpN = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        if (vpN) ctx->RSGetViewports(&vpN, vp);
        ctx->OMGetBlendState(&bs, bf, &sampleMask);
        ctx->OMGetDepthStencilState(&dss, &stencilRef);
    }
    template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
    static void RelInst(ID3D11ClassInstance** a, UINT n) { for (UINT i = 0; i < n && i < kInst; ++i) Rel(a[i]); }
    void Restore(ID3D11DeviceContext* ctx) {
        ctx->IASetInputLayout(il);
        ctx->IASetPrimitiveTopology(topo);
        ctx->VSSetShader(vs, vsI, vsN);
        ctx->HSSetShader(hs, hsI, hsN);
        ctx->DSSetShader(ds, dsI, dsN);
        ctx->GSSetShader(gs, gsI, gsN);
        ctx->PSSetShader(ps, psI, psN);
        ctx->PSSetShaderResources(0, 2, psSrv);
        ctx->RSSetState(rs);
        ctx->RSSetViewports(vpN, vpN ? vp : nullptr);      // (hooked: passes straight through inside the guard)
        ctx->OMSetBlendState(bs, bf, sampleMask);
        ctx->OMSetDepthStencilState(dss, stencilRef);
        Rel(il);
        Rel(vs); RelInst(vsI, vsN);
        Rel(hs); RelInst(hsI, hsN);
        Rel(ds); RelInst(dsI, dsN);
        Rel(gs); RelInst(gsI, gsN);
        Rel(ps); RelInst(psI, psN);
        Rel(psSrv[0]); Rel(psSrv[1]);
        Rel(rs); Rel(bs); Rel(dss);
    }
};

// v0.7.0: round(a * num / den) in integers (exact, no float drift at 6000+ px).
uint32_t ScaleRound(uint32_t a, uint32_t num, uint32_t den) {
    if (!den) return a;
    return (uint32_t)(((uint64_t)a * (uint64_t)num * 2u + (uint64_t)den) / (2u * (uint64_t)den));
}

HRESULT MakeTex(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind,
                ID3D11Texture2D** out) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    return dev->CreateTexture2D(&td, nullptr, out);
}

// v0.6.4: crop origin on one axis -- centred on `c` (uv), clamped into the image, snapped DOWN to an even pixel
// (stays inside: 0 <= origin <= full - size). size >= full -> 0.
uint32_t RectOrigin(uint32_t full, uint32_t size, float c) {
    if (size >= full) return 0;
    double o = std::floor((double)c * (double)full - 0.5 * (double)size + 0.5);
    if (!(o >= 0.0)) o = 0.0;                                   // also NaN
    if (o > (double)(full - size)) o = (double)(full - size);
    return (uint32_t)o & ~1u;
}

} // namespace

// v0.7.8: see scene_dlaa.h.
void SceneDlaa::RegisterShaders() {
    ShaderCache::Add(ShaderCache::kDepthConvert, kDepthConvertShader, sizeof(kDepthConvertShader) - 1, "depth_convert",
                     "CSMain", "cs_5_0");
    ShaderCache::Add(ShaderCache::kRcas, kRcasShader, sizeof(kRcasShader) - 1, "rcas_sharpen", "CSMain", "cs_5_0");
    ShaderCache::Add(ShaderCache::kCompositeVs, kCompositeVs, sizeof(kCompositeVs) - 1, "composite_vs", "VSMain", "vs_5_0");
    ShaderCache::Add(ShaderCache::kCompositePs, kCompositePs, sizeof(kCompositePs) - 1, "composite_ps", "PSMain", "ps_5_0");
    ShaderCache::Add(ShaderCache::kMvDebug, kMvDebugShader, sizeof(kMvDebugShader) - 1, "mv_debug", "CSMain", "cs_5_0");
}

bool SceneDlaa::ShadersReady() { return ShaderCache::Done(); }

bool SceneDlaa::PrewarmNgx(ID3D11DeviceContext* ctx) {
    if (!ctx) return false;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    CsState saved;                                     // NGX may bind compute state while creating the feature
    saved.Save(ctx);
    const bool ok = DlaaProcessor::Prewarm(dev.Get());
    saved.Restore(ctx);
    return ok;
}

void SceneDlaa::CropSize(uint32_t fullW, uint32_t fullH, int area, uint32_t* cw, uint32_t* ch) {
    if (area >= 100) { *cw = fullW; *ch = fullH; return; }
    if (area < 1) area = 1;
    // round(full * area / 100) to the nearest multiple of 8: (full * area + 400) / 800 * 8, at least 8.
    uint32_t w = (uint32_t)(((uint64_t)fullW * (uint64_t)area + 400u) / 800u) * 8u;
    uint32_t h = (uint32_t)(((uint64_t)fullH * (uint64_t)area + 400u) / 800u) * 8u;
    if (w < 8) w = 8;
    if (h < 8) h = 8;
    *cw = w > fullW ? fullW : w;
    *ch = h > fullH ? fullH : h;
}

bool SceneDlaa::Ensure(ID3D11Device* dev, uint32_t fullW, uint32_t fullH, uint32_t w, uint32_t h,
                       uint32_t rx, uint32_t ry, uint32_t outFullW, uint32_t outFullH, uint32_t ow, uint32_t oh, bool hdr) {
    const bool same = w == m_w && h == m_h && fullW == m_fullW && fullH == m_fullH &&
                      outFullW == m_outFullW && outFullH == m_outFullH && ow == m_ow && oh == m_oh &&   // v0.7.0
                      hdr == m_hdr;                                                                       // v0.8.0
    if (m_ready && same) return true;
    if (m_failed && same) return false;     // already failed at these dims
    // v0.7.8: a (re)build creates an NGX feature (~10-40 ms with textures): at most one per Present, so two units
    // never build in the same frame. Nothing is torn down here; the next Present retries.
    if (!DlaaProcessor::CreateBudgetFree()) { m_deferred = true; return false; }

    // Dims changed (or first use, or v0.6.4 the DLAA area changed, or v0.7.0 upscale on/off / output size):
    // tear down and rebuild everything.
    m_dlaa.Shutdown();
    m_cam.Invalidate();                    // candidates belong to the old size
    m_colorIn.Reset(); m_colorInSrv.Reset();
    m_depthTwin.Reset(); m_depthTwinSrv.Reset();      // own twin is created lazily (EnsureOwnTwin)
    m_depthR32.Reset(); m_depthR32Srv.Reset(); m_depthR32Uav.Reset();
    m_mv.Reset(); m_mvSrv.Reset(); m_mvUav.Reset();
    m_out.Reset(); m_outUav.Reset(); m_outSrv.Reset();
    m_sharp.Reset(); m_sharpUav.Reset(); m_sharpSrv.Reset(); m_sharpRes = false;
    m_compPending = false; m_compSrc = nullptr; m_result = nullptr; m_compDataValid = false;   // v0.7.0
    m_ready = false; m_failed = true;     // flipped to ready only on full success
    m_w = w; m_h = h;
    m_fullW = fullW; m_fullH = fullH;     // v0.6.4
    m_rx = rx; m_ry = ry;
    m_crop = w != fullW || h != fullH;
    m_hdr = hdr;                          // v0.8.0: colour textures RGBA16F + IsHDR feature
    m_up = outFullW != 0;                 // v0.7.0 (the caller passes 0 unless output > render)
    m_outFullW = outFullW; m_outFullH = outFullH;
    m_ow = ow; m_oh = oh;
    m_oxc = m_oyc = 0xFFFFFFFFu;          // v0.7.0: "unknown" -> Run logs the rect pair after every rebuild
    m_resetPending = true;                // v0.6.4: a rebuilt feature starts without history (explicit reset)
    for (GpuQ& q : m_q) q = GpuQ();

    HRESULT hr;
    // Depth-convert compute shader (created once, kept across resizes; v0.7.8 bytecode from ShaderCache).
    if (!m_depthCs) {
        size_t size = 0;
        const void* code = ShaderCache::Code(ShaderCache::kDepthConvert, &size);
        if (!code) {
            Log("DLAA: depth shader D3DCompile failed %s", ShaderCache::Error(ShaderCache::kDepthConvert));
            return false;
        }
        hr = dev->CreateComputeShader(code, size, nullptr, &m_depthCs);
        if (FAILED(hr)) { Log("DLAA: depth CreateComputeShader hr=0x%lx", hr); return false; }
    }
    if (!m_depthCb) {
        // v0.6.4: rect origin for the convert pass (DYNAMIC, rewritten in DepthConvert when the origin moved).
        const uint32_t cb[4] = { 0, 0, 0, 0 };
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(cb);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SUBRESOURCE_DATA sd{ cb, 0, 0 };
        hr = dev->CreateBuffer(&bd, &sd, &m_depthCb);
        if (FAILED(hr)) { Log("DLAA: depth convert constant buffer create hr=0x%lx", hr); return false; }
        m_depthCbX = m_depthCbY = 0;
    }

    // colorIn: UNORM twin of the SRGB tonemap texture (same typeless group,
    // CopyResource preserves the gamma-encoded bytes).
    // v0.8.0 HDR unit: every colour texture (colorIn, m_out, m_sharp) is R16G16B16A16_FLOAT, the game's own HDR scene
    // format (CopyResource in / out stays a plain same-format copy; NGX, RCAS and the composite read / write float4).
    const DXGI_FORMAT colorFmt = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(hr = MakeTex(dev, w, h, colorFmt, D3D11_BIND_SHADER_RESOURCE, &m_colorIn)) ||
        FAILED(hr = dev->CreateShaderResourceView(m_colorIn.Get(), nullptr, &m_colorInSrv))) {
        Log("DLAA: colorIn create hr=0x%lx", hr); return false;
    }

    // Flattened R32F depth handed to NGX.
    if (FAILED(hr = MakeTex(dev, w, h, DXGI_FORMAT_R32_FLOAT,
                            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &m_depthR32)) ||
        FAILED(hr = dev->CreateShaderResourceView(m_depthR32.Get(), nullptr, &m_depthR32Srv)) ||
        FAILED(hr = dev->CreateUnorderedAccessView(m_depthR32.Get(), nullptr, &m_depthR32Uav))) {
        Log("DLAA: depth R32F create hr=0x%lx", hr); return false;
    }

    // Motion vectors (RG16F): written by CameraMv, or cleared to 0 when MV is off.
    if (FAILED(hr = MakeTex(dev, w, h, DXGI_FORMAT_R16G16_FLOAT,
                            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &m_mv)) ||
        FAILED(hr = dev->CreateShaderResourceView(m_mv.Get(), nullptr, &m_mvSrv)) ||
        FAILED(hr = dev->CreateUnorderedAccessView(m_mv.Get(), nullptr, &m_mvUav))) {
        Log("DLAA: MV create hr=0x%lx", hr); return false;
    }

    // Output (UAV, written by NGX), copied back over the game texture. v0.5.6: with sharpening it is also
    // the RCAS input (SRV) and m_sharp is what gets copied back. v0.5.7: SRV always (sharpness is live).
    // v0.7.0: OUTPUT-rect-sized (ow x oh; = w x h without upscale). It stays R8G8B8A8_UNORM (NGX creates its
    // own UAV on it); the composite reads it through the UNORM SRV and decodes sRGB in the PS when needed.
    if (FAILED(hr = MakeTex(dev, ow, oh, colorFmt,
                            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &m_out)) ||
        FAILED(hr = dev->CreateUnorderedAccessView(m_out.Get(), nullptr, &m_outUav))) {
        Log("DLAA: output create hr=0x%lx", hr); return false;
    }
    if (m_up && FAILED(hr = dev->CreateShaderResourceView(m_out.Get(), nullptr, &m_outSrv))) {
        Log("DLAA: output SRV create hr=0x%lx", hr); return false;   // v0.7.0: the composite needs it
    }

    // v0.5.6 sharpen target + shader. v0.5.7: created even at sharpness 0 (Shift+PageUp can raise it live);
    // the pass itself is skipped while the strength is 0. A failure here only disables sharpening.
    // v0.7.0 upscale: output-rect-sized, + SRV (composite source); RCAS runs at output res (radius in output
    // texels) and never blends (the composite does the border feather).
    hr = S_OK;
    if (EnsureSharpen(dev) &&
        (m_outSrv || SUCCEEDED(hr = dev->CreateShaderResourceView(m_out.Get(), nullptr, &m_outSrv))) &&
        SUCCEEDED(hr = MakeTex(dev, ow, oh, colorFmt,
                               D3D11_BIND_UNORDERED_ACCESS | (m_up ? D3D11_BIND_SHADER_RESOURCE : 0u), &m_sharp)) &&
        SUCCEEDED(hr = dev->CreateUnorderedAccessView(m_sharp.Get(), nullptr, &m_sharpUav)) &&
        (!m_up || SUCCEEDED(hr = dev->CreateShaderResourceView(m_sharp.Get(), nullptr, &m_sharpSrv)))) {
        m_sharpRes = true;
        if (m_up)
            Log("DLAA: sharpen (RCAS) ready for eye %d -- strength %.2f r=%.1f output px%s, %ux%u target created "
                "(upscale: runs at output res; the DLAA area border feather is done by the composite)", m_eye,
                s_sharpness, s_sharpRadius, s_sharpness > 0.0f ? "" : " (pass skipped while 0)", ow, oh);
        else if (m_crop)
            Log("DLAA: sharpen (RCAS) ready for eye %d -- strength %.2f r=%.1f px, %ux%u target created; DLAA area "
                "border blend over %d px (pass always runs while area < 100)", m_eye, s_sharpness, s_sharpRadius,
                w, h, s_feather);
        else
            Log("DLAA: sharpen (RCAS) ready for eye %d -- strength %.2f r=%.1f px%s, %ux%u target created", m_eye,
                s_sharpness, s_sharpRadius,
                s_sharpness > 0.0f ? "" : " (pass skipped while 0; Shift+PageUp raises it)", w, h);
    } else {
        Log("DLAA: sharpen setup failed hr=0x%lx for eye %d -- sharpening OFF (DLAA continues)%s", (unsigned long)hr,
            m_eye, (m_crop && !m_up) ? "; DLAA area border blend OFF too (hard rect edge)" : "");
        m_sharp.Reset(); m_sharpUav.Reset(); m_sharpSrv.Reset();
        if (!m_up) m_outSrv.Reset();          // v0.7.0: upscale keeps it (composite source)
    }

    // Reversed-Z scene depth -> create the feature with DepthInverted from the start.
    // v0.7.0: render w x h -> output ow x oh (equal = DLAA).
    // v0.8.0: an HDR unit sets IsHDR unless dlaa.ini dlss_hdr = 0 (then the float values go in as display-referred).
    const bool ngxHdr = hdr && s_hdrLinear;
    if (!m_dlaa.Init(dev, w, h, ow, oh, /*depthInverted=*/true, ngxHdr)) {
        Log("DLAA: NGX init FAILED for eye %d %ux%u -> %ux%u%s (see preceding DLAA lines for the reason)", m_eye, w, h,
            ow, oh, hdr ? (ngxHdr ? " [HDR unit: RGBA16F colour, IsHDR=1]" : " [HDR unit: RGBA16F colour, IsHDR=0 (dlss_hdr=0)]") : "");
        return false;
    }
    // v0.8.0: HDR units append their colour format / IsHDR state (the LDR lines are unchanged).
    const char* const hdrTag = !hdr ? "" : (ngxHdr ? ", HDR: RGBA16F colour, IsHDR=1"
                                                   : ", HDR: RGBA16F colour, IsHDR=0 (dlaa.ini dlss_hdr=0)");
    if (m_up)
        Log("DLAA: NGX feature created for eye %d -- DLSS %s upscale: render %ux%u at rect origin (%u,%u) of %ux%u -> "
            "output %ux%u of %ux%u (area=%d%%), textures created, depthInverted=1, MVLowRes=1%s", m_eye,
            m_dlaa.QualityName(), w, h, rx, ry, fullW, fullH, ow, oh, outFullW, outFullH, s_area, hdrTag);
    else
        Log("DLAA: NGX feature created for eye %d -- %ux%u at rect origin (%u,%u) of %ux%u (area=%d%%), textures "
            "created, depthInverted=1%s", m_eye, w, h, rx, ry, fullW, fullH, s_area, hdrTag);
    m_ready = true; m_failed = false;
    return true;
}

// v0.7.0: composite VS + PS (compiled once), its states and the params buffer. A failure is logged once and
// the upscale result is then never drawn (the eye keeps the game's bilinear stretch of the raw frame).
bool SceneDlaa::EnsureComposite(ID3D11Device* dev) {
    if (m_compVs && m_compPs && m_compBlend && m_compRs && m_compDss && m_compParamsSrv) return true;
    if (m_compTried) return false;
    m_compTried = true;
    HRESULT hr;
    // v0.7.8: bytecode from ShaderCache (Run only gets here once its warm-up is done).
    size_t vsSize = 0, psSize = 0;
    const void* vsCode = ShaderCache::Code(ShaderCache::kCompositeVs, &vsSize);
    const void* psCode = ShaderCache::Code(ShaderCache::kCompositePs, &psSize);
    if (!vsCode) {
        Log("DLAA: composite VS D3DCompile failed %s", ShaderCache::Error(ShaderCache::kCompositeVs));
        return false;
    }
    if (FAILED(hr = dev->CreateVertexShader(vsCode, vsSize, nullptr, &m_compVs))) {
        Log("DLAA: composite CreateVertexShader hr=0x%lx", hr); return false;
    }
    if (!psCode) {
        Log("DLAA: composite PS D3DCompile failed %s", ShaderCache::Error(ShaderCache::kCompositePs));
        return false;
    }
    if (FAILED(hr = dev->CreatePixelShader(psCode, psSize, nullptr, &m_compPs))) {
        Log("DLAA: composite CreatePixelShader hr=0x%lx", hr); return false;
    }
    // RT0: out = src * a + dst * (1 - a) on RGB (a = 1 -> exactly src); alpha channel NOT written (the eye RT's
    // alpha stays what the game's blit wrote). RT1..7 (if the game had more bound): nothing written.
    D3D11_BLEND_DESC bd{};
    bd.AlphaToCoverageEnable  = FALSE;
    bd.IndependentBlendEnable = TRUE;
    bd.RenderTarget[0].BlendEnable    = TRUE;
    bd.RenderTarget[0].SrcBlend       = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    for (int i = 1; i < 8; ++i) {
        bd.RenderTarget[i].BlendEnable    = FALSE;
        bd.RenderTarget[i].SrcBlend       = D3D11_BLEND_ONE;
        bd.RenderTarget[i].DestBlend      = D3D11_BLEND_ZERO;
        bd.RenderTarget[i].BlendOp        = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[i].SrcBlendAlpha  = D3D11_BLEND_ONE;
        bd.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[i].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[i].RenderTargetWriteMask = 0;
    }
    if (FAILED(hr = dev->CreateBlendState(&bd, &m_compBlend))) { Log("DLAA: composite CreateBlendState hr=0x%lx", hr); return false; }
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = FALSE;
    rd.MultisampleEnable = FALSE;
    rd.AntialiasedLineEnable = FALSE;
    if (FAILED(hr = dev->CreateRasterizerState(&rd, &m_compRs))) { Log("DLAA: composite CreateRasterizerState hr=0x%lx", hr); return false; }
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
    if (FAILED(hr = dev->CreateDepthStencilState(&dd, &m_compDss))) { Log("DLAA: composite CreateDepthStencilState hr=0x%lx", hr); return false; }
    // Params: 3 x float4 in a DYNAMIC buffer read through a Buffer<float4> SRV (t1).
    D3D11_BUFFER_DESC pb{};
    pb.ByteWidth = sizeof(m_compData);
    pb.Usage = D3D11_USAGE_DYNAMIC;
    pb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    pb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(hr = dev->CreateBuffer(&pb, nullptr, &m_compParams))) { Log("DLAA: composite params buffer hr=0x%lx", hr); return false; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.FirstElement = 0;
    sd.Buffer.NumElements = (UINT)(sizeof(m_compData) / 16);
    if (FAILED(hr = dev->CreateShaderResourceView(m_compParams.Get(), &sd, &m_compParamsSrv))) {
        Log("DLAA: composite params SRV hr=0x%lx", hr); return false;
    }
    m_compDataValid = false;
    Log("DLAA: upscale composite ready for eye %d (VS + PS, alpha-blend feather, RGB write mask)", m_eye);
    return true;
}

bool SceneDlaa::EnsureDebugCs(ID3D11Device* dev) {
    if (m_mvDbgCs) return true;
    if (m_mvDbgTried) return false;
    m_mvDbgTried = true;
    size_t size = 0;                                   // v0.7.8: bytecode from ShaderCache
    const void* code = ShaderCache::Code(ShaderCache::kMvDebug, &size);
    if (!code) {
        Log("DLAA: MV debug shader D3DCompile failed %s", ShaderCache::Error(ShaderCache::kMvDebug));
        return false;
    }
    HRESULT hr = dev->CreateComputeShader(code, size, nullptr, &m_mvDbgCs);
    if (FAILED(hr)) { Log("DLAA: MV debug CreateComputeShader hr=0x%lx", hr); return false; }
    return true;
}

// v0.5.6: RCAS compute shader (compiled once, kept across resizes) + its constant buffer (strength).
float SceneDlaa::s_sharpness = 0.4f;     // dlaa.ini sharpness (default 0.4; 0 = off)
float SceneDlaa::s_sharpRadius = 1.5f;   // v0.5.8 dlaa.ini sharp_radius (default 1.5; clamp 1..4)
int   SceneDlaa::s_area = 100;           // v0.6.4 dlaa.ini dlaa_area (default 100 = whole image; clamp 40..100)
int   SceneDlaa::s_feather = 96;         // v0.6.4 dlaa.ini dlaa_area_feather (default 96 px; clamp 0..512)
bool  SceneDlaa::s_hdrLinear = true;     // v0.8.0 dlaa.ini dlss_hdr (default 1 = IsHDR for HDR units)

bool SceneDlaa::EnsureSharpen(ID3D11Device* dev) {
    HRESULT hr;
    if (!m_sharpCs) {
        size_t size = 0;                               // v0.7.8: bytecode from ShaderCache
        const void* code = ShaderCache::Code(ShaderCache::kRcas, &size);
        if (!code) {
            Log("DLAA: RCAS shader D3DCompile failed %s", ShaderCache::Error(ShaderCache::kRcas));
            return false;
        }
        hr = dev->CreateComputeShader(code, size, nullptr, &m_sharpCs);
        if (FAILED(hr)) { Log("DLAA: RCAS CreateComputeShader hr=0x%lx", hr); return false; }
    }
    if (!m_sharpCb) {
        // v0.5.7: DYNAMIC (was immutable) -- the strength changes live; Run re-uploads it (Map WRITE_DISCARD)
        // whenever s_sharpness differs from what this eye last wrote (m_sharpCbVal). v0.5.8: radius too.
        // v0.6.4: 32 bytes (+ feather, blend, edge flags); Run re-uploads whenever any of the 8 floats changed.
        // v0.8.0: + float4 Mode (HDR flag), written by Run before the first dispatch like every other field.
        const float cb[kSharpCbFloats] = { s_sharpness, s_sharpRadius, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                           0.0f, 0.0f, 0.0f, 0.0f };
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(cb);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SUBRESOURCE_DATA sd{ cb, 0, 0 };
        hr = dev->CreateBuffer(&bd, &sd, &m_sharpCb);
        if (FAILED(hr)) { Log("DLAA: RCAS constant buffer create hr=0x%lx", hr); return false; }
        memcpy(m_sharpCbData, cb, sizeof(cb));
        m_sharpCbValid = true;
    }
    if (!m_sharpSampler) {
        // v0.5.8: bilinear ring taps with clamp addressing (replaces the shader's manual edge clamps).
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MinLOD = 0.0f; sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr = dev->CreateSamplerState(&sd, &m_sharpSampler);
        if (FAILED(hr)) { Log("DLAA: RCAS sampler create hr=0x%lx", hr); return false; }
    }
    return true;
}

// v0.5.7: depth twin -> flattened R32F (m_depthR32) for NGX. Used whenever the merged MV pass did not run.
// v0.6.4: the twin is read at the rect origin (b0, rewritten only when the origin moved).
void SceneDlaa::DepthConvert(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* twinSrv) {
    if (m_depthCbX != m_rx || m_depthCbY != m_ry) {
        D3D11_MAPPED_SUBRESOURCE mp{};
        if (SUCCEEDED(ctx->Map(m_depthCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) {
            const uint32_t cb[4] = { m_rx, m_ry, 0, 0 };
            memcpy(mp.pData, cb, sizeof(cb));
            ctx->Unmap(m_depthCb.Get(), 0);
            m_depthCbX = m_rx; m_depthCbY = m_ry;
        } else {
            static int warned = 0;                     // stale origin this frame (depth off by the move), retried next
            if (warned++ < 5) Log("DLAA: depth convert constant buffer Map failed on eye %d", m_eye);
        }
    }
    ctx->CSSetShader(m_depthCs.Get(), nullptr, 0);
    ID3D11ShaderResourceView*  srv = twinSrv;
    ID3D11UnorderedAccessView* uav = m_depthR32Uav.Get();
    ID3D11Buffer*              cb  = m_depthCb.Get();  // b0 (CsState saves CB slots 0..13)
    const UINT keep = (UINT)-1;
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetShaderResources(0, 1, &srv);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
    ctx->Dispatch((m_w + 7) / 8, (m_h + 7) / 8, 1);
    // Unbind so NGX can read the R32F texture as an SRV (no SRV+UAV overlap).
    ID3D11ShaderResourceView*  nullSrv = nullptr;
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ctx->CSSetShaderResources(0, 1, &nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
}

// v0.5.7: see scene_dlaa.h. Render thread (called from Present, never inside Run).
void SceneDlaa::ResetTiming() {
    m_msSum = 0.0;
    for (double& s : m_stSum) s = 0.0;
    m_msN = 0;
    m_msWindow = 240;
    m_msSkip = kTimingWarmup;
    for (GpuQ& q : m_q) if (q.inFlight) q.keep = false;   // measured the old preset / strength
}

void SceneDlaa::ResetSnapTiming() { g_snapTimer.ResetWindow(); }

// Own depth twin: only needed for the live-depth fallback (no per-pass snapshot), so created on first use.
// v0.6.4: always FULL size (a CopyResource of the scene depth), whatever the DLAA area.
bool SceneDlaa::EnsureOwnTwin(ID3D11Device* dev) {
    if (m_depthTwin) return true;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    HRESULT hr;
    if (FAILED(hr = MakeTex(dev, m_fullW, m_fullH, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_SHADER_RESOURCE, &m_depthTwin)) ||
        FAILED(hr = dev->CreateShaderResourceView(m_depthTwin.Get(), &sd, &m_depthTwinSrv))) {
        Log("DLAA: own depth twin create hr=0x%lx", (unsigned long)hr);
        m_depthTwin.Reset(); m_depthTwinSrv.Reset();
        return false;
    }
    return true;
}

bool DepthTwin::Snapshot(ID3D11DeviceContext* ctx, ID3D11Texture2D* depth, bool timing) {
    if (!ctx || !depth) return false;
    D3D11_TEXTURE2D_DESC td{};
    depth->GetDesc(&td);
    if (!tex || w != td.Width || h != td.Height) {
        Shutdown();
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        if (!dev) return false;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        HRESULT hr;
        if (FAILED(hr = MakeTex(dev.Get(), td.Width, td.Height, DXGI_FORMAT_R32G8X24_TYPELESS,
                                D3D11_BIND_SHADER_RESOURCE, &tex)) ||
            FAILED(hr = dev->CreateShaderResourceView(tex.Get(), &sd, &srv))) {
            Log("DLAA: pass depth twin create hr=0x%lx (%ux%u)", (unsigned long)hr, td.Width, td.Height);
            Shutdown();
            return false;
        }
        w = td.Width; h = td.Height;
    }
    const int tq = timing ? g_snapTimer.Begin(ctx) : -1;   // v0.5.6: GPU cost of this copy
    ctx->CopyResource(tex.Get(), depth);               // same typeless group, never bound as SRV+DSV at once
    g_snapTimer.End(ctx, tq);
    valid = true;
    return true;
}

// ---- GPU timing (optional, non-blocking) ----------------------------------------------
// v0.5.6: kStamps timestamps per Run (stage boundaries), all read with DONOTFLUSH; a slot is only
// evaluated once every query in it has landed.
void SceneDlaa::GpuPoll(ID3D11DeviceContext* ctx) {
    for (GpuQ& q : m_q) {
        if (!q.inFlight) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
        if (ctx->GetData(q.dis.Get(), &dd, sizeof(dd), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 ts[kStamps] = {};
        bool got = true;
        for (int i = 0; i < kStamps && got; ++i)
            got = ctx->GetData(q.t[i].Get(), &ts[i], sizeof(ts[i]), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        UINT64 tc = 0;                                 // v0.7.0: start of the composite draw (split slots)
        if (got && q.split)
            got = ctx->GetData(q.tc.Get(), &tc, sizeof(tc), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (!got) continue;
        q.inFlight = false;
        if (!q.keep || dd.Disjoint || !dd.Frequency) continue;
        if (m_msSkip > 0) { --m_msSkip; continue; }    // v0.5.7: warm-up after a preset / sharpness change
        const double toMs = 1000.0 / (double)dd.Frequency;
        if (!q.split) {
            for (int i = 0; i < kStamps - 1; ++i)
                m_stSum[i] += ts[i + 1] >= ts[i] ? (double)(ts[i + 1] - ts[i]) * toMs : 0.0;
            m_msSum += ts[kStamps - 1] >= ts[0] ? (double)(ts[kStamps - 1] - ts[0]) * toMs : 0.0;
        } else {
            // v0.7.0: the last stage = our composite only (tc .. t[6]); the game's blit Draw between t[5] and tc
            // is not ours and is left out of the stage and of the total.
            for (int i = 0; i < kStamps - 2; ++i)
                m_stSum[i] += ts[i + 1] >= ts[i] ? (double)(ts[i + 1] - ts[i]) * toMs : 0.0;
            const double comp = ts[kStamps - 1] >= tc ? (double)(ts[kStamps - 1] - tc) * toMs : 0.0;
            m_stSum[kStamps - 2] += comp;
            m_msSum += (ts[kStamps - 2] >= ts[0] ? (double)(ts[kStamps - 2] - ts[0]) * toMs : 0.0) + comp;
        }
        if (++m_msN >= m_msWindow) {
            const double nW = (double)m_msN;
            char up[64];                               // v0.7.0 tag: up=WxH->WxH / up=off
            if (m_up) snprintf(up, sizeof(up), "%ux%u->%ux%u", m_fullW, m_fullH, m_outFullW, m_outFullH);
            else      snprintf(up, sizeof(up), "off");
            Log("DLAA GPU cost eye %d: total %.2f ms | copy-in %.2f | depth %.2f | mv %.2f | ngx %.2f | sharpen %.2f | %s %.2f (avg over %llu) [preset=%s sharp=%.1f r=%.1f area=%d up=%s]",
                m_eye, m_msSum / nW, m_stSum[0] / nW, m_stSum[1] / nW, m_stSum[2] / nW,
                m_stSum[3] / nW, m_stSum[4] / nW, m_up ? "composite" : "copy-back", m_stSum[5] / nW, (unsigned long long)m_msN,
                DlssPresetName(), (double)s_sharpness, (double)s_sharpRadius, s_area, up);
            m_msSum = 0.0;
            for (double& s : m_stSum) s = 0.0;
            m_msN = 0;
            m_msWindow = 600;                          // back to the regular cadence after a 240 window
        }
    }
}

void SceneDlaa::GpuBegin(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    m_qCur = -1;
    GpuQ& q = m_q[m_qHead];
    if (q.inFlight) return;                            // all slots busy: skip this frame
    if (!q.dis) {
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (FAILED(dev->CreateQuery(&qd, &q.dis))) { m_timing = false; return; }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        for (auto& t : q.t)
            if (FAILED(dev->CreateQuery(&qd, &t))) { q = GpuQ(); m_timing = false; return; }
        if (FAILED(dev->CreateQuery(&qd, &q.tc))) { q = GpuQ(); m_timing = false; return; }   // v0.7.0
    }
    q.split = false;
    ctx->Begin(q.dis.Get());
    ctx->End(q.t[0].Get());
    m_qCur = m_qHead;
    m_qNext = 1;
}

void SceneDlaa::GpuMark(ID3D11DeviceContext* ctx, int stamp) {
    if (m_qCur < 0 || stamp < m_qNext || stamp >= kStamps) return;
    GpuQ& q = m_q[m_qCur];
    while (m_qNext < stamp) ctx->End(q.t[m_qNext++].Get());   // (never skipped in practice; keeps the order)
    ctx->End(q.t[m_qNext++].Get());
}

void SceneDlaa::GpuEnd(ID3D11DeviceContext* ctx, bool keep) {
    if (m_qCur < 0) return;
    GpuQ& q = m_q[m_qCur];
    while (m_qNext < kStamps) ctx->End(q.t[m_qNext++].Get());  // aborted Run: end the rest so the slot can land
    ctx->End(q.dis.Get());
    q.inFlight = true;
    q.keep = keep;
    m_qHead = (m_qHead + 1) % 4;
    m_qCur = -1;
}

bool SceneDlaa::Run(ID3D11DeviceContext* ctx, ID3D11Texture2D* tonemap, ID3D11Texture2D* sceneDepth,
                    DepthTwin* depthTwin, const CandidateRecord* cand, float jitterX, float jitterY, bool reset, bool useMv, bool mvDebug, bool candBad,
                    uint32_t outW, uint32_t outH) {
    if (m_compPending) CancelComposite(ctx);           // v0.7.0: never left open (Composite always follows Run)
    m_deferred = false;
    if (!ShaderCache::Done()) { m_deferred = true; return false; }   // v0.7.8: warm-up running (callers gate first)
    D3D11_TEXTURE2D_DESC td{};
    tonemap->GetDesc(&td);
    // v0.8.0: the colour format decides the unit kind. R8G8B8A8 family = LDR (the v0.7.x path, unchanged), RGBA16F =
    // HDR unit. Anything else is not something the callers hand in; refused (once logged) instead of a format-mismatched
    // CopyResource that would silently do nothing.
    const bool hdr = IsHdrFormat(td.Format);
    if (!hdr && td.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && td.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
        td.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) {
        if (!m_badFmtLogged) {
            m_badFmtLogged = true;
            Log("DLAA: eye %d colour texture %ux%u fmt=%d is neither R8G8B8A8 nor R16G16B16A16_FLOAT -- not processed",
                m_eye, td.Width, td.Height, (int)td.Format);
        }
        return false;
    }

    // v0.6.4 DLAA area: crop size from the area (recomputed every Run, cheap; a change rebuilds in Ensure),
    // origin centred on this eye's optical centre, clamped into the image, even pixels. Area 100 = whole image,
    // origin (0,0) = the v0.6.3 path. Outside the rect the game image stays the raw jittered frame.
    uint32_t cw = td.Width, ch = td.Height;
    CropSize(td.Width, td.Height, s_area, &cw, &ch);
    const uint32_t rx = RectOrigin(td.Width, cw, m_cu);
    const uint32_t ry = RectOrigin(td.Height, ch, m_cv);

    // v0.7.0 upscale: only when the output is >= the tonemap in both axes and larger in one (else v0.6.5 path).
    // Output rect = the crop mapped into output space: size = round(crop * out / full) per axis (depends on the
    // crop size only, so a rect move never rebuilds), origin = round(origin * out / full), clamped so the rect
    // stays inside the output. Area 100 -> (0, 0, outW, outH) exactly.
    const bool up = outW && outH && outW >= td.Width && outH >= td.Height && (outW > td.Width || outH > td.Height);
    uint32_t ofw = 0, ofh = 0, ow = cw, oh = ch, oxc = rx, oyc = ry;
    if (up) {
        ofw = outW; ofh = outH;
        ow = ScaleRound(cw, outW, td.Width);  if (ow > outW) ow = outW;
        oh = ScaleRound(ch, outH, td.Height); if (oh > outH) oh = outH;
        oxc = ScaleRound(rx, outW, td.Width); if (oxc > outW - ow) oxc = outW - ow;
        oyc = ScaleRound(ry, outH, td.Height); if (oyc > outH - oh) oyc = outH - oh;
    }

    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    // v0.7.8: a Run that (re)builds the unit is timed and logged (proof that no build costs more than its textures +
    // its own NGX feature any more: shaders come from ShaderCache, NGX is pre-warmed).
    const bool building = !(m_ready && cw == m_w && ch == m_h && td.Width == m_fullW && td.Height == m_fullH &&
                            ofw == m_outFullW && ofh == m_outFullH && ow == m_ow && oh == m_oh && hdr == m_hdr);
    LARGE_INTEGER runT0{}, ensT1{};
    if (building) QueryPerformanceCounter(&runT0);
    if (!dev || !Ensure(dev.Get(), td.Width, td.Height, cw, ch, rx, ry, ofw, ofh, ow, oh, hdr)) return false;
    if (building) QueryPerformanceCounter(&ensT1);
    if (up && (oxc != m_oxc || oyc != m_oyc)) {
        static int outLogs = 0;                        // v0.7.0: both rects, once per change (cap)
        if (outLogs++ < 40)
            Log("DLAA upscale rect: eye %d render (%u,%u) %ux%u of %ux%u -> output (%u,%u) %ux%u of %ux%u", m_eye,
                rx, ry, cw, ch, td.Width, td.Height, oxc, oyc, ow, oh, outW, outH);
    }
    m_oxc = oxc; m_oyc = oyc;                          // (an output move follows the render rect move below)
    if (rx != m_rx || ry != m_ry) {
        // The optical centre was (re-)adopted: same crop size, new origin. DLSS history belongs to the old
        // pixel window -> treated like a resize for the history (reset), textures / feature kept.
        static int moveLogs = 0;                       // both eyes; a stable headset logs 1 move per eye
        if (moveLogs++ < 100)
            Log("DLAA area: eye %d rect moved (%u,%u) -> (%u,%u), %ux%u of %ux%u, centre uv=(%.3f, %.3f) -- history reset",
                m_eye, m_rx, m_ry, rx, ry, m_w, m_h, m_fullW, m_fullH, (double)m_cu, (double)m_cv);
        m_rx = rx; m_ry = ry;
        m_resetPending = true;
    }
    if (m_resetPending) { reset = true; m_resetPending = false; }

    if (m_timing) { GpuPoll(ctx); GpuBegin(dev.Get(), ctx); }

    CsState saved;
    saved.Save(ctx);

    // a. color in (SRGB -> UNORM twin, bytes preserved). v0.6.4: only the rect when the area is < 100.
    if (m_crop) {
        const D3D11_BOX box{ m_rx, m_ry, 0, m_rx + m_w, m_ry + m_h, 1 };
        ctx->CopySubresourceRegion(m_colorIn.Get(), 0, 0, 0, 0, tonemap, 0, &box);
    } else {
        ctx->CopyResource(m_colorIn.Get(), tonemap);
    }
    if (m_timing) GpuMark(ctx, 1);                     // after copy-in

    // b. depth -> typeless twin (the R32F flatten happens in c. when the merged MV pass runs, else here)
    // v0.6.4: the twin is full-size; the convert / MV pass reads it at the rect origin.
    ID3D11ShaderResourceView* twinSrv = nullptr;
    if (depthTwin && depthTwin->valid && depthTwin->srv && depthTwin->w == m_fullW && depthTwin->h == m_fullH) {
        twinSrv = depthTwin->srv.Get();                // the pass's pre-clear snapshot
    } else if (sceneDepth && EnsureOwnTwin(dev.Get())) {
        ctx->CopyResource(m_depthTwin.Get(), sceneDepth);
        twinSrv = m_depthTwinSrv.Get();
    }
    if (depthTwin) depthTwin->valid = false;           // snapshot consumed (or never usable)
    if (!twinSrv) { saved.Restore(ctx); if (m_timing) GpuEnd(ctx, /*keep=*/false); return false; }
    const UINT keep = (UINT)-1;
    ID3D11ShaderResourceView*  nullSrv = nullptr;
    ID3D11UnorderedAccessView* nullUav = nullptr;

    // c. motion vectors: camera reprojection (per depth layer), or zeros.
    // v0.5.7: with MVs the reprojection pass also flattens the twin into m_depthR32 (one pass instead of two).
    // m_depthR32 is ALWAYS rewritten from this frame's twin before NGX: by the merged pass when Generate
    // returns true, otherwise by DepthConvert -- MV off (HOME), no `cand`, CameraMv init failed, or any
    // Generate failure (it only fails before dispatching anything, see motion_vectors.h).
    // GPU stamps: merged path -> stamp 2 right after stamp 1 (depth ~0 ms), "mv" = merged pass (+ the
    // fallback convert if Generate failed); old path -> depth = convert pass, mv = clear / commit only.
    bool mvDone = false;
    if (useMv && cand && m_cam.Init(dev.Get())) {
        if (m_timing) GpuMark(ctx, 2);                 // (merged) nothing between stamps 1 and 2
        CameraMv::FrameStats fs;
        mvDone = m_cam.Generate(ctx, m_w, m_h, m_rx, m_ry, m_fullW, m_fullH, *cand, twinSrv,
                                m_depthR32Uav.Get(), m_mvUav.Get(), &fs, candBad);
        if (!mvDone) DepthConvert(ctx, twinSrv);       // Generate wrote nothing: depth must not stay stale
        // No world pair = scene change / first frame / camera cut: history is unusable.
        // v0.6.1: a bad record (and the first good one after it) reuses the last good R: fs.worldMiss is false
        // there, so no reset is forced (an incomplete candidate record is not a camera cut).
        if (mvDone && fs.worldMiss) reset = true;
        if (!mvDone) reset = true;
    } else {
        DepthConvert(ctx, twinSrv);
        if (m_timing) GpuMark(ctx, 2);                 // after depth convert
        if (m_cam.Ready() && cand)
            m_cam.Commit(ctx, *cand);       // keep the eye's candidate history rolling
    }
    if (!mvDone) {
        const float zero[4] = { 0, 0, 0, 0 };
        ctx->ClearUnorderedAccessViewFloat(m_mvUav.Get(), zero);
    }
    if (m_timing) GpuMark(ctx, 3);                     // after MV

    // d. evaluate: injected viewport jitter, depth is reversed-Z
    DlaaFrameParams p;
    p.jitterX = jitterX;
    p.jitterY = jitterY;
    p.reset = reset;
    p.depthInverted = true;
    const bool ok = m_dlaa.Evaluate(ctx, m_colorInSrv.Get(), m_depthR32Srv.Get(),
                                    m_mvSrv.Get(), m_outUav.Get(), p);
    if (m_timing) GpuMark(ctx, 4);                     // after NGX evaluate

    // d1. v0.5.6 RCAS sharpen: m_out -> m_sharp (skipped entirely when sharpness is 0 / setup failed).
    // v0.5.7: strength is live; re-upload the constant buffer when it changed since this eye last wrote it.
    // v0.5.8: radius is live too, so re-upload when either the strength or the radius changed.
    // v0.6.4 DLAA area: with a crop the pass ALWAYS runs (also at sharpness 0: lobe 0 = NGX colour unchanged) and
    // blends into colorIn over the border (edges on the image border are not feathered). Whole image: the CB
    // carries feather/blend/edges = 0 and the pass is skipped at sharpness 0, exactly as before.
    // v0.7.0 upscale: m_out / m_sharp are output-sized, colorIn is not -> never blend here (the composite
    // feathers against the game's own stretched frame); the pass runs only for sharpening (skipped at 0).
    const float sharpNow  = s_sharpness;
    const float radiusNow = s_sharpRadius;
    const bool  blend     = m_crop && !m_up;
    const float cbNow[kSharpCbFloats] = {
        sharpNow, radiusNow, blend ? (float)s_feather : 0.0f, blend ? 1.0f : 0.0f,
        (blend && m_rx > 0) ? 1.0f : 0.0f,                       // left edge inside the image
        (blend && m_ry > 0) ? 1.0f : 0.0f,                       // top
        (blend && m_rx + m_w < m_fullW) ? 1.0f : 0.0f,           // right
        (blend && m_ry + m_h < m_fullH) ? 1.0f : 0.0f,           // bottom
        m_hdr ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };                 // v0.8.0 Mode: HDR unit (compressed-value RCAS)
    bool sharpOn = m_sharpRes && (sharpNow > 0.0f || blend);
    if (ok && sharpOn && (!m_sharpCbValid || memcmp(cbNow, m_sharpCbData, sizeof(cbNow)) != 0)) {
        D3D11_MAPPED_SUBRESOURCE mp{};
        if (SUCCEEDED(ctx->Map(m_sharpCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) {
            memcpy(mp.pData, cbNow, sizeof(cbNow));
            ctx->Unmap(m_sharpCb.Get(), 0);
            memcpy(m_sharpCbData, cbNow, sizeof(cbNow));
            m_sharpCbValid = true;
        } else {
            sharpOn = false;                           // stale strength/radius in the CB: skip this frame, retry next
            static int warned = 0;
            if (warned++ < 5) Log("DLAA: RCAS constant buffer Map failed on eye %d -- frame not sharpened", m_eye);
        }
    }
    const bool sharpened = ok && sharpOn;
    m_lastMvDone = mvDone; m_lastReset = reset; m_lastSharpened = sharpened;   // v0.7.8 preview capture (log only)
    if (sharpened) {
        // UAVs first: NGX may have left m_out's UAV bound, and an SRV of a resource still bound as a UAV
        // would be silently nulled by the runtime. Clearing all 8 slots (CsState restores them) avoids that.
        ID3D11UnorderedAccessView* suav[8] = { m_sharpUav.Get() };
        const UINT skeep[8] = { (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1 };
        // v0.6.4: t1 = colorIn (border blend) only with a crop; whole image binds t0 alone as before.
        ID3D11ShaderResourceView*  ssrv[2] = { m_outSrv.Get(), m_colorInSrv.Get() };
        const UINT                 nSrv = blend ? 2u : 1u;
        ID3D11ShaderResourceView*  snull[2] = {};
        ID3D11Buffer*              scb  = m_sharpCb.Get();          // b0 (CsState saves CB slots 0..13)
        ID3D11SamplerState*        ssmp = m_sharpSampler.Get();     // s0, bilinear clamp (CsState saves s0..15)
        ctx->CSSetShader(m_sharpCs.Get(), nullptr, 0);
        ctx->CSSetUnorderedAccessViews(0, 8, suav, skeep);
        ctx->CSSetShaderResources(0, nSrv, ssrv);
        ctx->CSSetConstantBuffers(0, 1, &scb);
        ctx->CSSetSamplers(0, 1, &ssmp);
        ctx->Dispatch((m_ow + 7) / 8, (m_oh + 7) / 8, 1);   // v0.7.0: output rect (= m_w x m_h without upscale)
        ID3D11SamplerState* nullSmp = nullptr;
        ctx->CSSetShaderResources(0, nSrv, snull);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
        ctx->CSSetSamplers(0, 1, &nullSmp);
    }
    if (m_timing) GpuMark(ctx, 5);                     // after sharpen

    if (ok) {
        ID3D11Texture2D* res = sharpened ? m_sharp.Get() : m_out.Get();
        m_result = res;                                // v0.7.0: ResultTex() (self-test / snapshot)
        if (m_up) {
            // v0.7.0 upscale: nothing goes back into the tonemap; Composite() draws `res` into the eye RT after
            // the game's blit. Composite needs the SRV + its shaders; without them the result is dropped.
            m_compSrc = sharpened ? m_sharpSrv.Get() : m_outSrv.Get();
            m_compPending = m_compSrc != nullptr && EnsureComposite(dev.Get());
        } else {
            // UNORM -> SRGB twin, bytes preserved. v0.6.4: crop -> into the rect only.
            if (m_crop) ctx->CopySubresourceRegion(tonemap, 0, m_rx, m_ry, 0, res, 0, nullptr);
            else        ctx->CopyResource(tonemap, res);
        }
        ++m_frames;
    } else if (m_evalFails++ < 20) {
        Log("DLAA: Evaluate failed (#%llu), game frame left untouched",
            (unsigned long long)m_evalFails);
    }
    // After copy-back (MV debug view below is not timed per stage). v0.7.0: with a pending composite, stamp 6
    // is taken by Composite() after our draw (stage "composite").
    if (m_timing && !m_compPending) GpuMark(ctx, 6);

    // d2. MV debug view: overwrite the blit source with the MV visualization.
    // v0.7.0 upscale: written into m_out (output-sized, scaled from the render-size MV / depth) and drawn by the
    // composite instead of the DLSS result (also when Evaluate failed).
    if (mvDebug && EnsureDebugCs(dev.Get())) {
        // v0.6.0: t2 = solve buffer, b0 = CameraMv dims cbuffer (EgoPixelM) -- only when pass B ran this frame.
        ID3D11ShaderResourceView*  dsrv[3] = { m_mvSrv.Get(), m_depthR32Srv.Get(), mvDone ? m_cam.SolveSrv() : nullptr };
        ID3D11Buffer*              dcb = mvDone ? m_cam.DimsCb() : nullptr;
        ID3D11UnorderedAccessView* duav = m_outUav.Get();
        ID3D11ShaderResourceView*  dnull[3] = {};
        ctx->CSSetShader(m_mvDbgCs.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 3, dsrv);
        ctx->CSSetConstantBuffers(0, 1, &dcb);
        ctx->CSSetUnorderedAccessViews(0, 1, &duav, &keep);
        ctx->Dispatch((m_ow + 7) / 8, (m_oh + 7) / 8, 1);  // v0.7.0: output rect (= m_w x m_h without upscale)
        ctx->CSSetShaderResources(0, 3, dnull);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
        if (m_up) {
            // v0.7.0: the composite draws the visualization; a failed Evaluate left stamp 6 already taken.
            m_result = m_out.Get();
            m_compSrc = m_outSrv.Get();
            if (!m_compPending && m_compSrc && EnsureComposite(dev.Get())) {
                m_compPending = true;
                if (m_timing) GpuEnd(ctx);             // stamp 6 was taken above: close this slot as before
            }
        } else if (m_crop) ctx->CopySubresourceRegion(tonemap, 0, m_rx, m_ry, 0, m_out.Get(), 0, nullptr);   // v0.6.4: rect only
        else        ctx->CopyResource(tonemap, m_out.Get());
    }

    // e. put the compute stage back exactly as the game had it
    saved.Restore(ctx);
    // v0.7.0: a pending composite keeps the timing slot open (Composite / CancelComposite close it).
    if (m_timing && !m_compPending) GpuEnd(ctx);
    if (building) {                                    // v0.7.8 build cost (CPU wall time on the render thread)
        static int buildLogs = 0;
        if (buildLogs++ < 40) {
            LARGE_INTEGER t{}, f{};
            QueryPerformanceCounter(&t);
            QueryPerformanceFrequency(&f);
            const double toMs = f.QuadPart > 0 ? 1000.0 / (double)f.QuadPart : 0.0;
            Log("DLAA: eye %d unit built in this Run: %.1f ms (textures + NGX feature %.1f ms, MV init + first eval %.1f ms), ok=%d",
                m_eye, (double)(t.QuadPart - runT0.QuadPart) * toMs, (double)(ensT1.QuadPart - runT0.QuadPart) * toMs,
                (double)(t.QuadPart - ensT1.QuadPart) * toMs, (int)ok);
        }
    }
    return ok;
}

// v0.7.0: see scene_dlaa.h. Runs after the game's blit Draw (render thread, inside the caller's re-entrancy
// guard). Graphics state only; the compute stage is not touched. The game's OM binding (RT0 = eye RT) is used
// as is -- never rebound -- so nothing about the eye RT's bind flags / view formats matters here.
bool SceneDlaa::Composite(ID3D11DeviceContext* ctx, uint32_t vpX, uint32_t vpY, bool srgbDecode) {
    if (!m_compPending) return false;
    m_compPending = false;
    if (!m_compSrc || !m_compVs || !m_compPs || !m_compParamsSrv) { if (m_timing) GpuEnd(ctx, false); return false; }
    // v0.8.0 HDR unit: our texels are linear RGBA16F and RT0 is the game's float output target -- never an sRGB decode
    // (an HDR blit's SRV0 view is RGBA16F, so the caller passes false anyway; this only makes it explicit).
    if (m_hdr) srgbDecode = false;

    // Params: RT-space rect origin + size, feather (output px per axis), blend, sRGB decode, edge flags.
    // Feather: dlaa_area_feather is in render px; scaled per axis to output px (same image fraction as DLAA).
    // Edges: the rect edge is feathered only when it lies inside the output image (v0.6.4 rule).
    const bool blend = m_crop;
    const float fx = blend ? (float)s_feather * (float)m_outFullW / (float)m_fullW : 0.0f;
    const float fy = blend ? (float)s_feather * (float)m_outFullH / (float)m_fullH : 0.0f;
    const float data[12] = {
        (float)(vpX + m_oxc), (float)(vpY + m_oyc), (float)m_ow, (float)m_oh,
        fx, fy, blend ? 1.0f : 0.0f, srgbDecode ? 1.0f : 0.0f,
        (blend && m_oxc > 0) ? 1.0f : 0.0f,                       // left edge inside the image
        (blend && m_oyc > 0) ? 1.0f : 0.0f,                       // top
        (blend && m_oxc + m_ow < m_outFullW) ? 1.0f : 0.0f,       // right
        (blend && m_oyc + m_oh < m_outFullH) ? 1.0f : 0.0f };     // bottom
    if (!m_compDataValid || memcmp(data, m_compData, sizeof(data)) != 0) {
        D3D11_MAPPED_SUBRESOURCE mp{};
        if (FAILED(ctx->Map(m_compParams.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) {
            static int warned = 0;
            if (warned++ < 5) Log("DLAA: composite params Map failed on eye %d -- frame keeps the game's stretch", m_eye);
            if (m_timing) GpuEnd(ctx, false);
            return false;
        }
        memcpy(mp.pData, data, sizeof(data));
        ctx->Unmap(m_compParams.Get(), 0);
        memcpy(m_compData, data, sizeof(data));
        m_compDataValid = true;
    }

    if (m_timing && m_qCur >= 0) {                     // composite stage starts here (after the game's blit)
        GpuQ& q = m_q[m_qCur];
        ctx->End(q.tc.Get());
        q.split = true;
    }

    GfxState saved;
    saved.Save(ctx);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = (float)(vpX + m_oxc);
    vp.TopLeftY = (float)(vpY + m_oyc);
    vp.Width    = (float)m_ow;
    vp.Height   = (float)m_oh;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11ShaderResourceView* srv[2] = { m_compSrc, m_compParamsSrv.Get() };
    const FLOAT blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // unused (no BLEND_FACTOR in the state)

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_compVs.Get(), nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(m_compPs.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 2, srv);
    ctx->RSSetState(m_compRs.Get());
    ctx->RSSetViewports(1, &vp);                       // (hooked: passes straight through inside the guard)
    ctx->OMSetBlendState(m_compBlend.Get(), blendFactor, 0xffffffffu);
    ctx->OMSetDepthStencilState(m_compDss.Get(), 0);
    ctx->Draw(3, 0);                                   // (hooked: passes straight through inside the guard)

    saved.Restore(ctx);

    if (m_timing && m_qCur >= 0) { GpuMark(ctx, kStamps - 1); GpuEnd(ctx); }   // stage "composite" ends
    static bool logged[2] = {};
    const int le = m_eye == 1 ? 1 : 0;
    if (!logged[le]) {
        logged[le] = true;
        Log("DLAA upscale composite: eye %d first draw -- %ux%u at RT (%u,%u), feather %.1f x %.1f px%s, sRGB decode %d%s",
            m_eye, m_ow, m_oh, vpX + m_oxc, vpY + m_oyc, (double)fx, (double)fy, blend ? "" : " (area 100: alpha 1)",
            (int)srgbDecode, m_hdr ? " (HDR: linear RGBA16F into the game's float output target)" : "");
    }
    return true;
}

void SceneDlaa::CancelComposite(ID3D11DeviceContext* ctx) {
    if (!m_compPending) return;
    m_compPending = false;
    if (m_timing) GpuEnd(ctx, /*keep=*/false);
}

void SceneDlaa::Shutdown() {
    m_dlaa.Shutdown();
    m_cam.Shutdown();
    m_mvDbgCs.Reset(); m_mvDbgTried = false;
    m_sharp.Reset(); m_sharpUav.Reset(); m_sharpSrv.Reset(); m_outSrv.Reset(); m_sharpRes = false;
    m_sharpCs.Reset(); m_sharpCb.Reset(); m_sharpSampler.Reset(); m_sharpCbValid = false;
    m_depthCb.Reset(); m_depthCbX = m_depthCbY = 0;
    m_compVs.Reset(); m_compPs.Reset(); m_compBlend.Reset(); m_compRs.Reset(); m_compDss.Reset();   // v0.7.0
    m_compParams.Reset(); m_compParamsSrv.Reset(); m_compTried = false; m_compPending = false;
    m_compSrc = nullptr; m_result = nullptr; m_compDataValid = false;
    for (GpuQ& q : m_q) q = GpuQ();
    m_qHead = 0; m_qCur = -1; m_qNext = 0;
    m_depthTwin.Reset(); m_depthTwinSrv.Reset();
    m_ready = false; m_failed = false; m_w = m_h = 0;
    m_fullW = m_fullH = 0; m_rx = m_ry = 0; m_crop = false; m_resetPending = false;
    m_hdr = false;                                     // v0.8.0
    m_up = false; m_outFullW = m_outFullH = 0; m_ow = m_oh = 0; m_oxc = m_oyc = 0;
}

#endif // WITH_DLAA
