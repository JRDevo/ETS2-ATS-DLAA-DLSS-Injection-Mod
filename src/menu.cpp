// TuningMenu implementation (v0.10.0). See menu.h.
#ifdef WITH_DLAA

#include "menu.h"
#include "shader_cache.h"
#include "log.h"
#include <windows.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// One quad from SV_VertexID (triangle strip, no input layout / vertex buffer); the constant buffer gives its rect in NDC,
// or (v0.10.0 phase 16, Opt.w = 1) its 4 corners in CLIP space (the VR panel that faces the eye: a perspective quad, w = the
// corner's view depth, so the rasterizer interpolates the texture coordinates perspective-correct).
// kMenuVs-BEGIN (the build validates this block with fxc, vs_5_0 / VSMain)
const char kMenuVs[] = R"(
cbuffer MenuCb : register(b0) {
    float4 Rect;                                     // x0, y0 (top), x1, y1 (bottom) in NDC
    float4 Opt;                                      // x: 1 = decode sRGB -> linear, y: panel opacity, w: 1 = Corner[] (clip space)
    float4 Corner[4];                                // phase 16: top-left, top-right, bottom-left, bottom-right (Opt.w = 1)
                                                     // phase 47 (VSCab): the same corners in a cab draw's MODEL space
    uint4  Cab;                                      // phase 47 (VSCab): x = byte offset of that draw's MVP (4 rows) in CabMirror
    float4 Map;                                      // phase 47 (VSCab): picture NDC = the draw's NDC * Map.xy + Map.zw
};
struct VOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VOut VSMain(uint id : SV_VertexID) {
    const float2 t = float2((float)(id & 1u), (float)(id >> 1));   // (0,0) (1,0) (0,1) (1,1)
    VOut o;
    if (Opt.w > 0.5) o.pos = Corner[id & 3u];
    else             o.pos = float4(lerp(Rect.x, Rect.z, t.x), lerp(Rect.y, Rect.w, t.y), 0.0, 1.0);
    o.uv = t;
    return o;
}
// phase 47: the fps box ATTACHED TO THE VEHICLE (src/fps_cab.h). CabMirror = the draw-id record's copy of the game's cbuffer
// ring(s) for THIS eye's pass; the reference cab draw's MVP (rows = clip x, y, z, w; clip = M p) is read at Cab.x, so the box's
// model-space corners land where that draw's geometry lands in this very picture. A block that is not MVP-shaped (DrawIdMv
// ShapeOk) or not finite puts every corner at one point outside the picture (nothing drawn).
ByteAddressBuffer CabMirror : register(t0);
VOut VSCab(uint id : SV_VertexID) {
    const float2 t = float2((float)(id & 1u), (float)(id >> 1));
    VOut o;
    o.uv = t;
    const uint a = Cab.x;
    const float4 r0 = asfloat(CabMirror.Load4(a)), r1 = asfloat(CabMirror.Load4(a + 16u));
    const float4 r2 = asfloat(CabMirror.Load4(a + 32u)), r3 = asfloat(CabMirror.Load4(a + 48u));
    const float4 p = Corner[id & 3u];
    const float4 c = float4(dot(r0, p), dot(r1, p), dot(r2, p), dot(r3, p));
    const float l0 = length(r0.xyz), l1 = length(r1.xyz), l3 = length(r3.xyz);
    const float lm = max(l0, max(l1, length(r2.xyz)));
    const bool ok = all(isfinite(c)) && lm > 1e-20 && l0 > 0.02 * lm && l1 > 0.02 * lm && l3 > 0.02 * lm;
    o.pos = ok ? float4(c.xy * Map.xy + Map.zw * c.w, 0.5 * c.w, c.w) : float4(2.0, 2.0, 0.5, 1.0);
    return o;
}
)";
// kMenuVs-END

// Panel texel (linear sampler) -> rgb; Opt.z = 0 (panel): alpha = max(opacity, luminance) so the text is opaque, the
// background 88 %. Opt.z = 1 (the fps box, outlined text): alpha = the texture's alpha (the black background has 0) * Opt.y
// (phase 51f: fps_opacity -- text AND outline fade together; every path blends straight alpha, so this one place is enough).
// kMenuPs-BEGIN (the build validates this block with fxc, ps_5_0 / PSMain)
const char kMenuPs[] = R"(
cbuffer MenuCb : register(b0) {
    float4 Rect;
    float4 Opt;                                      // x: 0 raw, 1 sRGB -> linear, 2 HDR10 (PQ, Rec.2020, 200 nits); y: opacity; z: 0 panel rule, 1 texture alpha
};
Texture2D<float4> PanelTex : register(t0);
SamplerState      Lin      : register(s0);
float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    const float4 t = PanelTex.Sample(Lin, uv);
    float3 c = t.rgb;
    const float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    const float a = (Opt.z > 0.5) ? t.a * Opt.y : max(Opt.y, lum);
    if (Opt.x > 0.5) c = pow(max(c, 0.0), 2.2);
    if (Opt.x > 1.5) {
        const float3x3 toBt2020 = float3x3(0.6274, 0.3293, 0.0433,
                                           0.0691, 0.9195, 0.0114,
                                           0.0164, 0.0880, 0.8956);
        const float3 L = max(mul(toBt2020, c), 0.0) * (200.0 / 10000.0);
        const float3 Lp = pow(L, 0.1593017578125);
        c = pow((0.8359375 + 18.8515625 * Lp) / (1.0 + 18.6875 * Lp), 78.84375);
    }
    return float4(c, a);
}
)";
// kMenuPs-END

struct CbData { float rect[4]; float opt[4]; float corner[4][4]; uint32_t cab[4]; float map[4]; };   // (phase 16: + the 4
                                                                                                     //  clip-space corners;
                                                                                                     //  phase 47: + cab / map)
static_assert(sizeof(CbData) == 128, "MenuCb layout");

ComPtr<ID3D11VertexShader>      g_vs;
ComPtr<ID3D11VertexShader>      g_vsCab;     // phase 47: VSCab (the fps box attached to the vehicle), made on its first use
bool                            g_vsCabFailed = false;
ComPtr<ID3D11PixelShader>       g_ps;
ComPtr<ID3D11Buffer>            g_cb;
ComPtr<ID3D11SamplerState>      g_smp;
ComPtr<ID3D11BlendState>        g_blend;     // SrcAlpha / InvSrcAlpha, RGB write mask (alpha untouched)
ComPtr<ID3D11BlendState>        g_blendOv;   // v0.10.3 phase 29 (kOutOverlay): blend off, RGBA written as the shader outputs it
ComPtr<ID3D11RasterizerState>   g_rs;        // solid, no cull, no scissor
ComPtr<ID3D11DepthStencilState> g_dss;       // depth + stencil off
ID3D11Device*                   g_dev = nullptr;
bool                            g_failed = false;

ComPtr<ID3D11Texture2D>          g_tex;      // the panel picture (R8G8B8A8_UNORM)
ComPtr<ID3D11ShaderResourceView> g_srv;
uint32_t                         g_texW = 0, g_texH = 0;
float                            g_texBuild = 0.0f;   // build scale of g_tex
TuningMenu::Panel                g_last;              // the panel g_tex shows (compared byte for byte)
bool                             g_haveLast = false;
std::vector<uint32_t>            g_pixels;            // RGBA upload buffer (kept for the next rebuild)

// the fps box (DrawWidget; outlined text, no background): its own texture cache, keyed on its two lines + the build scale
constexpr size_t                 kWLine = 24;         // characters per cached line (incl. the terminator)
ComPtr<ID3D11Texture2D>          g_wTex;
ComPtr<ID3D11ShaderResourceView> g_wSrv;
uint32_t                         g_wTexW = 0, g_wTexH = 0;
float                            g_wBuild = 0.0f;
wchar_t                          g_wLast1[kWLine] = {}, g_wLast2[kWLine] = {};
bool                             g_wHave = false;
// phase 44 (dlaa.ini fps_color): the text / outline colours of the fps box (SetWidgetColors) and those of the cached texture
COLORREF                         g_wText = RGB(255, 255, 255), g_wLine = RGB(8, 8, 8);
COLORREF                         g_wLastText = 0, g_wLastLine = 0;
float                            g_wOpacity = 1.0f;   // phase 51f (fps_opacity): the box's alpha factor, applied by the shader (no rebuild)
std::vector<uint32_t>            g_wPixels;

constexpr UINT kInst = 256;
// PS SRV slots saved / restored (phase 16: was slot 0 only). The VR panel may now be drawn right before the game's own Draw
// that READS the eye picture (the late draw point): binding that picture as our render target unbinds it from every SRV slot
// it sits in, so every slot the game's next draw may read must come back.
constexpr UINT kSrvSave = 16;
constexpr UINT kMaxVp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
struct Saved {
    ID3D11InputLayout*        il = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY  topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader*       vs = nullptr; ID3D11ClassInstance* vsI[kInst] = {}; UINT vsN = kInst;
    ID3D11HullShader*         hs = nullptr; ID3D11ClassInstance* hsI[kInst] = {}; UINT hsN = kInst;
    ID3D11DomainShader*       ds = nullptr; ID3D11ClassInstance* dsI[kInst] = {}; UINT dsN = kInst;
    ID3D11GeometryShader*     gs = nullptr; ID3D11ClassInstance* gsI[kInst] = {}; UINT gsN = kInst;
    ID3D11PixelShader*        ps = nullptr; ID3D11ClassInstance* psI[kInst] = {}; UINT psN = kInst;
    ID3D11Buffer*             vsCb0 = nullptr;
    ID3D11ShaderResourceView* vsSrv0 = nullptr;      // phase 47: VSCab reads its mirror at VS t0
    bool                      vsSrvSaved = false;
    ID3D11Buffer*             psCb0 = nullptr;
    ID3D11ShaderResourceView* srv[kSrvSave] = {};
    ID3D11SamplerState*       smp0 = nullptr;
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

void SaveState(ID3D11DeviceContext* ctx, bool vsSrv = false) {
    Saved& s = g_saved;
    s.vsN = s.hsN = s.dsN = s.gsN = s.psN = kInst;
    s.vsSrvSaved = vsSrv;                                    // phase 47: only the VSCab draw binds a VS SRV
    if (vsSrv) ctx->VSGetShaderResources(0, 1, &s.vsSrv0);
    ctx->IAGetInputLayout(&s.il);
    ctx->IAGetPrimitiveTopology(&s.topo);
    ctx->VSGetShader(&s.vs, s.vsI, &s.vsN);
    ctx->HSGetShader(&s.hs, s.hsI, &s.hsN);
    ctx->DSGetShader(&s.ds, s.dsI, &s.dsN);
    ctx->GSGetShader(&s.gs, s.gsI, &s.gsN);
    ctx->PSGetShader(&s.ps, s.psI, &s.psN);
    ctx->VSGetConstantBuffers(0, 1, &s.vsCb0);
    ctx->PSGetConstantBuffers(0, 1, &s.psCb0);
    ctx->PSGetShaderResources(0, kSrvSave, s.srv);
    ctx->PSGetSamplers(0, 1, &s.smp0);
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
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtv, s.dsv);
    ctx->IASetInputLayout(s.il);
    ctx->IASetPrimitiveTopology(s.topo);
    ctx->VSSetShader(s.vs, s.vsI, s.vsN);
    ctx->HSSetShader(s.hs, s.hsI, s.hsN);
    ctx->DSSetShader(s.ds, s.dsI, s.dsN);
    ctx->GSSetShader(s.gs, s.gsI, s.gsN);
    ctx->PSSetShader(s.ps, s.psI, s.psN);
    ctx->VSSetConstantBuffers(0, 1, &s.vsCb0);
    if (s.vsSrvSaved) { ctx->VSSetShaderResources(0, 1, &s.vsSrv0); Rel(s.vsSrv0); s.vsSrvSaved = false; }   // phase 47
    ctx->PSSetConstantBuffers(0, 1, &s.psCb0);
    ctx->PSSetShaderResources(0, kSrvSave, s.srv);
    ctx->PSSetSamplers(0, 1, &s.smp0);
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
    Rel(s.vsCb0); Rel(s.psCb0);
    for (ID3D11ShaderResourceView*& v : s.srv) Rel(v);
    Rel(s.smp0);
    Rel(s.rs); Rel(s.bs); Rel(s.dss);
    for (ID3D11RenderTargetView*& r : s.rtv) Rel(r);
    Rel(s.dsv);
}

void DropTexture() {
    g_tex.Reset(); g_srv.Reset();
    g_texW = g_texH = 0; g_texBuild = 0.0f;
    g_haveLast = false;
}

void DropWidget() {
    g_wTex.Reset(); g_wSrv.Reset();
    g_wTexW = g_wTexH = 0; g_wBuild = 0.0f;
    g_wHave = false;
}

// Shaders + states for `dev` (once per device; a new device drops the old one's objects and both textures).
bool Ensure(ID3D11Device* dev) {
    if (g_dev == dev && g_vs && g_ps && g_cb && g_smp && g_blend && g_blendOv && g_rs && g_dss) return true;
    if (g_failed && g_dev == dev) return false;
    // warm-up still running: try again next frame. v0.10.0 phase 17: the two menu shaders are compiled right after the core
    // group (~2.1 s), no longer after the whole per-draw set (~20 s): the panel works on the first menu screens.
    if (!ShaderCache::Ready(ShaderCache::kMenuVs) || !ShaderCache::Ready(ShaderCache::kMenuPs)) return false;
    g_failed = true; g_dev = dev;
    g_vsCab.Reset(); g_vsCabFailed = false;                  // phase 47 (made again on its first use on this device)
    g_vs.Reset(); g_ps.Reset(); g_cb.Reset(); g_smp.Reset(); g_blend.Reset(); g_blendOv.Reset(); g_rs.Reset(); g_dss.Reset();
    DropTexture();
    DropWidget();
    HRESULT hr;
    size_t vsSize = 0, psSize = 0;
    const void* vsCode = ShaderCache::Code(ShaderCache::kMenuVs, &vsSize);
    const void* psCode = ShaderCache::Code(ShaderCache::kMenuPs, &psSize);
    if (!vsCode) { LogErr("tuning menu VS D3DCompile failed %s", ShaderCache::Error(ShaderCache::kMenuVs)); return false; }
    if (FAILED(hr = dev->CreateVertexShader(vsCode, vsSize, nullptr, &g_vs))) {
        LogErr("tuning menu CreateVertexShader hr=0x%lx", hr); return false;
    }
    if (!psCode) { LogErr("tuning menu PS D3DCompile failed %s", ShaderCache::Error(ShaderCache::kMenuPs)); return false; }
    if (FAILED(hr = dev->CreatePixelShader(psCode, psSize, nullptr, &g_ps))) {
        LogErr("tuning menu CreatePixelShader hr=0x%lx", hr); return false;
    }
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(CbData);
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(hr = dev->CreateBuffer(&cbd, nullptr, &g_cb))) { LogErr("tuning menu CreateBuffer hr=0x%lx", hr); return false; }
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0.0f; sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(hr = dev->CreateSamplerState(&sd, &g_smp))) { LogErr("tuning menu CreateSamplerState hr=0x%lx", hr); return false; }
    D3D11_BLEND_DESC bd{};
    bd.IndependentBlendEnable = TRUE;
    for (int i = 0; i < 8; ++i) {
        bd.RenderTarget[i].BlendEnable = i == 0 ? TRUE : FALSE;
        bd.RenderTarget[i].SrcBlend = D3D11_BLEND_SRC_ALPHA;  bd.RenderTarget[i].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[i].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;   bd.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[i].RenderTargetWriteMask = i == 0 ? (D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN |
                                                             D3D11_COLOR_WRITE_ENABLE_BLUE) : 0;
    }
    if (FAILED(hr = dev->CreateBlendState(&bd, &g_blend))) { LogErr("tuning menu CreateBlendState hr=0x%lx", hr); return false; }
    D3D11_BLEND_DESC bo{};                                   // v0.10.3 phase 29: the overlay image (kOutOverlay)
    bo.RenderTarget[0].BlendEnable = FALSE;
    bo.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;       bo.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
    bo.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bo.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;  bo.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bo.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bo.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(hr = dev->CreateBlendState(&bo, &g_blendOv))) { LogErr("tuning menu CreateBlendState (overlay) hr=0x%lx", hr); return false; }
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(hr = dev->CreateRasterizerState(&rd, &g_rs))) { LogErr("tuning menu CreateRasterizerState hr=0x%lx", hr); return false; }
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
    if (FAILED(hr = dev->CreateDepthStencilState(&dd, &g_dss))) { LogErr("tuning menu CreateDepthStencilState hr=0x%lx", hr); return false; }
    g_failed = false;
    return true;
}

// ---- panel layout (reference px, scale 1) ------------------------------------------------------------------------
// 26 rows at scale 1: 693 px = 64 % of a 1080p picture (was 21 rows x 21 px = 640 px before the 4 fps box rows; phase 16:
// + the VR facing row).
constexpr float kTitleY = 8.0f,  kTitleH = 24.0f;
constexpr float kPerfY  = 33.0f, kPerfLineH = 15.0f; // TuningMenu::kPerfLines lines (the GPU block)
constexpr float kHeadY  = 82.0f, kHeadH  = 18.0f;
constexpr float kRowsY  = 102.0f, kRowH  = 19.0f;
// v0.10.3 phase 29d: kDescH = 3 FULL lines of the 14 px description font at every build scale (was 54: one line of Segoe UI at
// 14 px is 19 px (tmHeight = DrawText's line step), so the 3rd line ended at 57 and was cut off above the footer). The line step
// rounds per scale (7 px font: 10 px = 20 ref px a line at 0.5; 20 px: 28 = 20 ref px at 1.4) and both rect edges round, so 3
// lines need up to 62 ref px; 64 fits them at every scale 0.30 .. 5.00 for every row count (measured with GDI). RenderPanel also
// draws only as many WHOLE lines as fit (<= kDescLines) and ellipsises what does not: never a cut or 4th line over the footer.
constexpr float kDescGap = 6.0f,  kDescH = 64.0f;   // kDescLines lines
constexpr int   kDescLines = 3;
constexpr float kFootGap = 2.0f,  kFootH = 18.0f, kBottom = 4.0f;
constexpr float kNameX0 = 14.0f,  kNameX1 = 236.0f;
constexpr float kValX0  = 240.0f, kValX1  = 384.0f;
constexpr float kKeyX0  = 388.0f, kKeyX1  = 640.0f;
constexpr float kCostX0 = 644.0f, kCostX1 = 748.0f;
// v0.10.3 phase 29c: the footer and the licence line share the bottom line (the footer left, the licence right-aligned); the
// footer ends kFootGapX left of the licence text (it used to run under it: "overflows" in VR). TuningMenu::FooterFits tests it.
constexpr float kFootX0 = 14.0f, kFootGapX = 12.0f;
const wchar_t kLicence[] = L"(c) JRDevo - All rights reserved - github.com/JRDevo";
static_assert(kPerfY + kPerfLineH * (float)TuningMenu::kPerfLines <= kHeadY, "tuning menu: GPU block overlaps the header");

const COLORREF kBg       = RGB(20, 20, 24);
const COLORREF kSelBg    = RGB(46, 46, 56);
const COLORREF kLine     = RGB(64, 64, 74);
const COLORREF kText     = RGB(236, 236, 236);
const COLORREF kKeyText  = RGB(178, 178, 188);
const COLORREF kSelText  = RGB(255, 214, 64);
const COLORREF kGrey     = RGB(112, 112, 122);
const COLORREF kSelGrey  = RGB(168, 148, 72);
const COLORREF kHead     = RGB(140, 140, 152);
const COLORREF kDesc     = RGB(214, 214, 220);
const COLORREF kPerf     = RGB(150, 210, 160);   // the perf block (soft green: measured figures, not settings)
// phase 45: the CPU / GPU inks (TuningMenu::kInkCpu / kInkGpu marks): Okabe-Ito sky blue and orange -- a pair that stays apart
// for protan / deutan / tritan vision (no red / green), both bright on the dark panel. Greyed rows: halfway to the background.
const COLORREF kInkCpuC  = RGB(86, 180, 233);
const COLORREF kInkGpuC  = RGB(230, 159, 0);
COLORREF Dim(COLORREF c) {
    return RGB((GetRValue(c) + GetRValue(kBg)) / 2, (GetGValue(c) + GetGValue(kBg)) / 2, (GetBValue(c) + GetBValue(kBg)) / 2);
}

HFONT MakeFont(float px, int weight) {
    HFONT f = CreateFontW(-(int)std::lround(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    return f;
}

// A top-down 32-bit DIB selected into a memory DC: GDI draws into it, DibFinish turns it into RGBA and frees it.
struct Dib { HDC dc = nullptr; HBITMAP bmp = nullptr; HGDIOBJ oldBmp = nullptr; void* bits = nullptr; };
bool DibBegin(Dib& d, uint32_t w, uint32_t h, const char* what, bool& failLogged) {
    d = Dib();
    d.dc = CreateCompatibleDC(nullptr);
    if (!d.dc) {
        if (!failLogged) { failLogged = true; LogErr("tuning menu: %s CreateCompatibleDC failed (%lu)", what, GetLastError()); }
        return false;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = (LONG)w;
    bi.bmiHeader.biHeight = -(LONG)h;                      // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    d.bmp = CreateDIBSection(d.dc, &bi, DIB_RGB_COLORS, &d.bits, nullptr, 0);
    if (!d.bmp || !d.bits) {
        if (!failLogged) { failLogged = true; LogErr("tuning menu: %s CreateDIBSection %ux%u failed (%lu)", what, w, h, GetLastError()); }
        if (d.bmp) DeleteObject(d.bmp);
        DeleteDC(d.dc);
        d = Dib();
        return false;
    }
    d.oldBmp = SelectObject(d.dc, d.bmp);
    return true;
}
// GDI flushed -> out (R, G, B, A = 255, w x h); the DIB and the DC are freed (the caller has put its fonts back already).
void DibFinish(Dib& d, uint32_t w, uint32_t h, std::vector<uint32_t>& out) {
    GdiFlush();
    out.resize((size_t)w * h);
    const uint32_t* src = static_cast<const uint32_t*>(d.bits);   // B, G, R, X in memory = 0xXXRRGGBB
    for (size_t i = 0, e = (size_t)w * h; i < e; ++i) {
        const uint32_t v = src[i];
        out[i] = 0xFF000000u | ((v & 0xFFu) << 16) | (v & 0xFF00u) | ((v >> 16) & 0xFFu);   // -> R, G, B, A
    }
    SelectObject(d.dc, d.oldBmp);
    DeleteObject(d.bmp);
    DeleteDC(d.dc);
    d = Dib();
}

// v0.10.3 phase 29d: the description as DrawText lays it out (word-wrapped) in a box w px wide and at most maxH px high (whole
// lines, the font selected in dc): `t` itself when it fits, else the longest prefix ending at a word (binary search, then back
// to its last space) + U+2026 "..." that fits, built in out[cap]. Only at a texture rebuild, so the few measurements cost nothing.
constexpr UINT kDescFlags = DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX;
const wchar_t* DescFit(HDC dc, const wchar_t* t, LONG w, LONG maxH, wchar_t* out, size_t cap) {
    auto fits = [&](const wchar_t* s, int len) {
        RECT r{ 0, 0, w, 0 };
        DrawTextW(dc, s, len, &r, kDescFlags | DT_CALCRECT);
        return r.bottom <= maxH;
    };
    const int len = (int)wcsnlen(t, cap - 2);
    if (fits(t, len)) return t;
    auto build = [&](int n) {                                // t[0..n) without trailing blanks / commas + "..."
        while (n > 0 && (t[n - 1] == L' ' || t[n - 1] == L',' || t[n - 1] == L';')) --n;
        wmemcpy(out, t, (size_t)n);
        out[n] = L'\u2026';
        out[n + 1] = 0;
        return n + 1;
    };
    int lo = 0, hi = len;                                    // the longest prefix that fits (the empty one assumed)
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (fits(out, build(mid))) lo = mid; else hi = mid - 1;
    }
    int cut = lo;
    while (cut > 0 && t[cut] != L' ') --cut;                 // end at a word (a prefix of one long word: keep the cut)
    build(cut > 0 ? cut : lo);
    return out;
}

// GDI -> g_pixels (RGBA, w x h). False = GDI failed (logged once).
bool RenderPanel(const TuningMenu::Panel& p, float s, uint32_t w, uint32_t h) {
    static bool s_failLogged = false;
    Dib d;
    if (!DibBegin(d, w, h, "panel", s_failLogged)) return false;
    HDC dc = d.dc;
    HFONT fBody  = MakeFont(15.0f * s, FW_NORMAL);
    HFONT fTitle = MakeFont(17.0f * s, FW_BOLD);
    HFONT fSmall = MakeFont(12.5f * s, FW_NORMAL);
    HFONT fDesc  = MakeFont(14.0f * s, FW_NORMAL);
    HFONT fPerf  = MakeFont(12.0f * s, FW_NORMAL);
    HFONT stock  = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    auto pick = [&](HFONT f) { return f ? f : stock; };
    HGDIOBJ oldFont = SelectObject(dc, pick(fBody));
    SetBkMode(dc, TRANSPARENT);

    auto px = [&](float v) { return (LONG)std::lround(v * s); };
    auto fill = [&](COLORREF c, float x0, float y0, float x1, float y1) {
        RECT r{ px(x0), px(y0), px(x1), px(y1) };
        HBRUSH b = CreateSolidBrush(c);
        if (b) { FillRect(dc, &r, b); DeleteObject(b); }
    };
    auto text = [&](HFONT f, COLORREF c, const wchar_t* t, float x0, float y0, float x1, float y1, UINT fl) {
        if (!t || !t[0]) return;
        SelectObject(dc, pick(f));
        SetTextColor(dc, c);
        RECT r{ px(x0), px(y0), px(x1), px(y1) };
        DrawTextW(dc, t, -1, &r, fl | DT_NOPREFIX);
    };
    const float W = (float)w / s, H = (float)h / s;          // reference px
    const UINT one = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
    // phase 45: single-line text with ink marks (TuningMenu::kInkCpu / kInkGpu / kInkEnd): the segments are drawn left to right,
    // each in its colour (base = the text's own colour; dim = a greyed row: the inks dimmed too); the last visible segment ends
    // in "..." when the box is too narrow. A text without marks = text() (same flags).
    auto inkText = [&](HFONT f, COLORREF base, const wchar_t* t, float x0, float y0, float x1, float y1, bool dim) {
        if (!t || !t[0]) return;
        bool marks = false;
        for (const wchar_t* q = t; *q; ++q) if (*q >= TuningMenu::kInkCpu && *q <= TuningMenu::kInkEnd) { marks = true; break; }
        if (!marks) { text(f, base, t, x0, y0, x1, y1, one); return; }
        SelectObject(dc, pick(f));
        LONG x = px(x0);
        const LONG right = px(x1), top = px(y0), bottom = px(y1);
        COLORREF cur = base;
        const wchar_t* seg = t;
        for (const wchar_t* q = t;; ++q) {
            const wchar_t ch = *q;
            const bool mark = ch >= TuningMenu::kInkCpu && ch <= TuningMenu::kInkEnd;
            if (!mark && ch) continue;
            const int len = (int)(q - seg);
            if (len > 0 && x < right) {
                SetTextColor(dc, cur);
                RECT r{ x, top, right, bottom };
                DrawTextW(dc, seg, len, &r, one | DT_NOPREFIX);
                SIZE sz{};
                if (GetTextExtentPoint32W(dc, seg, len, &sz)) x += sz.cx;
            }
            if (!ch) break;
            cur = ch == TuningMenu::kInkCpu ? (dim ? Dim(kInkCpuC) : kInkCpuC)
                : ch == TuningMenu::kInkGpu ? (dim ? Dim(kInkGpuC) : kInkGpuC) : base;
            seg = q + 1;
        }
    };
    fill(kBg, 0.0f, 0.0f, W, H);
    fill(kLine, 0.0f, 0.0f, W, 1.0f); fill(kLine, 0.0f, H - 1.0f, W, H);   // 1-px frame
    fill(kLine, 0.0f, 0.0f, 1.0f, H); fill(kLine, W - 1.0f, 0.0f, W, H);
    text(fTitle, kText, p.title, 12.0f, kTitleY, W - 12.0f, kTitleY + kTitleH, one);
    for (int l = 0; l < TuningMenu::kPerfLines; ++l) {
        const float y = kPerfY + kPerfLineH * (float)l;
        inkText(fPerf, kPerf, p.perf[l], 14.0f, y, W - 14.0f, y + kPerfLineH, false);   // phase 45: CPU / GPU inks
    }
    // v0.12.1 phase 50: the Hotkeys page names its columns itself (colHead; "" = the main menu's)
    text(fSmall, kHead, p.colHead[0][0] ? p.colHead[0] : L"Setting", kNameX0 + 14.0f, kHeadY, kNameX1, kHeadY + kHeadH, one);
    text(fSmall, kHead, p.colHead[1][0] ? p.colHead[1] : L"Value", kValX0, kHeadY, kValX1, kHeadY + kHeadH, one);
    text(fSmall, kHead, p.colHead[2][0] ? p.colHead[2] : L"Hotkey", kKeyX0, kHeadY, kKeyX1, kHeadY + kHeadH, one);
    inkText(fSmall, kHead, p.costHead[0] ? p.costHead : L"Cost", kCostX0, kHeadY, kCostX1, kHeadY + kHeadH, false);
    fill(kLine, 10.0f, kRowsY - 2.0f, W - 10.0f, kRowsY - 1.0f);
    const int n = p.nRows < 0 ? 0 : (p.nRows > TuningMenu::kMaxRows ? TuningMenu::kMaxRows : p.nRows);
    for (int i = 0; i < n; ++i) {
        const TuningMenu::Row& r = p.rows[i];
        const float y0 = kRowsY + kRowH * (float)i, y1 = y0 + kRowH;
        const bool sel = i == p.sel;
        if (sel) fill(kSelBg, 4.0f, y0, W - 4.0f, y1);
        const COLORREF cName = sel ? (r.greyed ? kSelGrey : kSelText) : (r.greyed ? kGrey : kText);
        const COLORREF cKey  = sel ? (r.greyed ? kSelGrey : kSelText) : (r.greyed ? kGrey : kKeyText);
        if (sel) text(fBody, cName, L">", kNameX0, y0, kNameX0 + 14.0f, y1, one);
        text(fBody, cName, r.name, kNameX0 + 14.0f, y0, kNameX1, y1, one);
        if (r.wide) {                                        // v0.12.1 phase 50: one text over Value + Hotkey + Cost
            text(fBody, cName, r.value, kValX0, y0, kCostX1, y1, one);
            continue;
        }
        text(fBody, cName, r.value, kValX0, y0, kValX1, y1, one);
        text(fBody, cKey, r.keys, kKeyX0, y0, kKeyX1, y1, one);
        inkText(fBody, cKey, r.cost, kCostX0, y0, kCostX1, y1, r.greyed != 0);   // phase 45: "<cpu> | <gpu>" in blue / orange
    }
    const float rowsEnd = kRowsY + kRowH * (float)n;
    fill(kLine, 10.0f, rowsEnd + 4.0f, W - 10.0f, rowsEnd + 5.0f);
    const float descY = rowsEnd + kDescGap;
    if (p.desc[0]) {
        // v0.10.3 phase 29d: the description gets as many WHOLE lines as its box holds (kDescLines at every scale, see kDescH),
        // the box ends at the last one; a text that needs more lines is cut at a word and ends in "..." (DescFit)
        SelectObject(dc, pick(fDesc));
        SetTextColor(dc, kDesc);
        TEXTMETRICW tm{};
        RECT r{ px(14.0f), px(descY), px(W - 14.0f), px(descY + kDescH) };
        const LONG lineH = GetTextMetricsW(dc, &tm) && tm.tmHeight > 0 ? tm.tmHeight : (r.bottom - r.top) / kDescLines;
        LONG lines = lineH > 0 ? (r.bottom - r.top) / lineH : 0;
        if (lines > kDescLines) lines = kDescLines;
        if (lines > 0) {
            r.bottom = r.top + lines * lineH;
            wchar_t fit[sizeof(p.desc) / sizeof(p.desc[0]) + 2];
            DrawTextW(dc, DescFit(dc, p.desc, r.right - r.left, r.bottom - r.top, fit, sizeof(fit) / sizeof(fit[0])), -1, &r,
                      kDescFlags);
        }
    }
    const float footY = descY + kDescH + kFootGap;
    // v0.10.3 phase 29c: the footer is clipped (end ellipsis) kFootGapX left of the licence text, never under it
    SelectObject(dc, pick(fSmall));
    SIZE lic{};
    if (!GetTextExtentPoint32W(dc, kLicence, (int)(sizeof(kLicence) / sizeof(kLicence[0]) - 1), &lic)) lic.cx = 0;
    const float footX1 = (std::max)(W * 0.25f, W - kFootX0 - (float)lic.cx / s - kFootGapX);
    text(fSmall, kHead, p.footer, kFootX0, footY, footX1, footY + kFootH, one);
    // licence line, bottom right: the GitHub page is the only place this mod exists
    text(fSmall, kHead, kLicence, W * 0.5f, footY, W - kFootX0, footY + kFootH, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
    DibFinish(d, w, h, g_pixels);
    if (fBody) DeleteObject(fBody);
    if (fTitle) DeleteObject(fTitle);
    if (fSmall) DeleteObject(fSmall);
    if (fDesc) DeleteObject(fDesc);
    if (fPerf) DeleteObject(fPerf);
    return true;
}

// The fps box: GDI -> g_wPixels (RGBA, w x h). Reference layout (scale 1, 92 x 44): line 1 (bold 15 px) in y 4..24, line 2
// (12 px) in y 24..40, both centred. NO background, NO frame: the text is white with a near-black outline (every line drawn
// 8 times offset by +-o px, o = max(1, round(buildScale)), then the text on top). Two DIBs with the same GDI calls: (a) the
// colour (black background, RGB 8,8,8 outline, white text) and (b) the mask (black background, outline + text all white);
// the texture is RGB from (a), A = the red channel of (b) (the coverage: antialiased edges = partial alpha). The shader
// draws it with the texture's alpha as it is. False = GDI failed (logged once).
bool RenderWidget(const wchar_t* line1, const wchar_t* line2, float s, uint32_t w, uint32_t h) {
    static bool s_failLogged = false;
    static std::vector<uint32_t> s_mask;
    HFONT fBig   = MakeFont(15.0f * s, FW_BOLD);
    HFONT fSmall = MakeFont(12.0f * s, FW_NORMAL);
    HFONT stock  = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    auto pick = [&](HFONT f) { return f ? f : stock; };
    auto px = [&](float v) { return (LONG)std::lround(v * s); };
    const float W = (float)w / s;                            // reference px
    const LONG o = std::max<LONG>(1, (LONG)std::lround(1.0f * s));   // outline offset in device px
    // One DIB: mask = true draws the outline and the text all in white, false the colour version.
    auto paint = [&](bool mask, std::vector<uint32_t>& out) -> bool {
        Dib d;
        if (!DibBegin(d, w, h, "fps box", s_failLogged)) return false;
        HDC dc = d.dc;
        HGDIOBJ oldFont = SelectObject(dc, pick(fBig));
        SetBkMode(dc, TRANSPARENT);                          // the DIB starts black (zeroed)
        auto line = [&](HFONT f, const wchar_t* t, float y0, float y1) {
            if (!t || !t[0]) return;
            SelectObject(dc, pick(f));
            const UINT fl = DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
            SetTextColor(dc, mask ? RGB(255, 255, 255) : g_wLine);   // phase 44: fps_color's outline (was RGB 8,8,8)
            for (LONG dy = -o; dy <= o; dy += o)
                for (LONG dx = -o; dx <= o; dx += o) {
                    if (!dx && !dy) continue;
                    RECT r{ px(3.0f) + dx, px(y0) + dy, px(W - 3.0f) + dx, px(y1) + dy };
                    DrawTextW(dc, t, -1, &r, fl);
                }
            SetTextColor(dc, mask ? RGB(255, 255, 255) : g_wText);   // phase 44: fps_color's text (was white)
            RECT r{ px(3.0f), px(y0), px(W - 3.0f), px(y1) };
            DrawTextW(dc, t, -1, &r, fl);
        };
        // fps_text: ONE line (the other empty) = drawn in the big font, centred on the two-line layout's centre (y 4..40 -> 22),
        // so the (transparent) box keeps its size and centre and the text does not jump.
        const bool has1 = line1 && line1[0], has2 = line2 && line2[0];
        if (has1 && has2) {
            line(fBig, line1, 4.0f, 24.0f);
            line(fSmall, line2, 24.0f, 40.0f);
        } else {
            line(fBig, has1 ? line1 : line2, 12.0f, 32.0f);
        }
        SelectObject(dc, oldFont);
        DibFinish(d, w, h, out);
        return true;
    };
    const bool ok = paint(false, g_wPixels) && paint(true, s_mask) && s_mask.size() == g_wPixels.size();
    if (ok)
        for (size_t i = 0, e = g_wPixels.size(); i < e; ++i)
            g_wPixels[i] = (g_wPixels[i] & 0x00FFFFFFu) | ((s_mask[i] & 0xFFu) << 24);   // A = the mask's red (RGBA: R is the low byte)
    s_mask.clear();
    s_mask.shrink_to_fit();
    if (fBig) DeleteObject(fBig);
    if (fSmall) DeleteObject(fSmall);
    return ok;
}

// (Re)creates `tex` / `srv` as a w x h R8G8B8A8_UNORM texture when the size changed, then uploads `px`. False = create failed.
bool UploadTexture(ID3D11Device* dev, ID3D11DeviceContext* ctx, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11ShaderResourceView>& srv,
                   uint32_t& texW, uint32_t& texH, uint32_t w, uint32_t h, const std::vector<uint32_t>& px, const char* what) {
    if (!tex || texW != w || texH != h) {
        tex.Reset(); srv.Reset();
        texW = texH = 0;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr;
        if (FAILED(hr = dev->CreateTexture2D(&td, nullptr, &tex)) ||
            FAILED(hr = dev->CreateShaderResourceView(tex.Get(), nullptr, &srv))) {
            static int logs = 0;
            if (logs++ < 3) LogErr("tuning menu: %s texture %ux%u create failed hr=0x%lx", what, w, h, hr);
            tex.Reset(); srv.Reset();
            return false;
        }
        texW = w; texH = h;
    }
    ctx->UpdateSubresource(tex.Get(), 0, nullptr, px.data(), w * 4u, 0);
    return true;
}

// The panel texture for `p` at `buildScale`: rebuilt only when the text or the scale changed.
bool EnsureTexture(ID3D11Device* dev, ID3D11DeviceContext* ctx, const TuningMenu::Panel& p, float buildScale) {
    if (g_tex && g_haveLast && std::fabs(g_texBuild - buildScale) < 0.001f && !memcmp(&g_last, &p, sizeof(p))) return true;
    const uint32_t w = (uint32_t)std::lround(TuningMenu::kRefWidth * buildScale);
    const uint32_t h = (uint32_t)std::lround(TuningMenu::RefHeight(p.nRows) * buildScale);
    if (w < 16 || h < 16 || w > 8192 || h > 8192) return false;
    if (!RenderPanel(p, buildScale, w, h)) return false;
    if (!UploadTexture(dev, ctx, g_tex, g_srv, g_texW, g_texH, w, h, g_pixels, "panel")) return false;
    g_texBuild = buildScale;
    memcpy(&g_last, &p, sizeof(p));
    g_haveLast = true;
    return true;
}

// Copies `src` into a cache line (at most kWLine - 1 characters; null = empty).
void CopyLine(wchar_t (&dst)[kWLine], const wchar_t* src) {
    size_t i = 0;
    if (src) for (; src[i] && i + 1 < kWLine; ++i) dst[i] = src[i];
    dst[i] = 0;
}
// The fps box texture for the two lines at `buildScale`: rebuilt only when a line or the scale changed.
bool EnsureWidgetTexture(ID3D11Device* dev, ID3D11DeviceContext* ctx, const wchar_t* line1, const wchar_t* line2, float buildScale) {
    wchar_t l1[kWLine], l2[kWLine];
    CopyLine(l1, line1);
    CopyLine(l2, line2);
    if (g_wTex && g_wHave && std::fabs(g_wBuild - buildScale) < 0.001f && !wcscmp(l1, g_wLast1) && !wcscmp(l2, g_wLast2) &&
        g_wLastText == g_wText && g_wLastLine == g_wLine)    // phase 44: + the colours
        return true;
    const uint32_t w = (uint32_t)std::lround(TuningMenu::kWidgetRefW * buildScale);
    const uint32_t h = (uint32_t)std::lround(TuningMenu::kWidgetRefH * buildScale);
    if (w < 8 || h < 8 || w > 4096 || h > 4096) return false;
    if (!RenderWidget(l1, l2, buildScale, w, h)) return false;
    if (!UploadTexture(dev, ctx, g_wTex, g_wSrv, g_wTexW, g_wTexH, w, h, g_wPixels, "fps box")) return false;
    g_wBuild = buildScale;
    memcpy(g_wLast1, l1, sizeof(l1));
    memcpy(g_wLast2, l2, sizeof(l2));
    g_wLastText = g_wText;
    g_wLastLine = g_wLine;
    g_wHave = true;
    return true;
}

// One textured quad of `srv` into `rtv` from `cb` (its rect / corners filled by the caller; the options are set here), every
// touched state saved and restored (SaveState / RestoreState). The device objects must exist (Ensure).
// phase 47: vsCab != null = that vertex shader (VSCab) with `vsSrv` at VS t0 instead of VSMain (saved / restored as well).
void DrawCb(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t rtW, uint32_t rtH, CbData& cb, int outMode,
            ID3D11ShaderResourceView* srv, bool textureAlpha, ID3D11VertexShader* vsCab = nullptr,
            ID3D11ShaderResourceView* vsSrv = nullptr) {
    const bool overlay = (outMode & TuningMenu::kOutOverlay) != 0;   // v0.10.3 phase 29: blend off, RGBA written
    outMode &= ~TuningMenu::kOutOverlay;
    cb.opt[0] = (float)(outMode < TuningMenu::kOutRaw ? TuningMenu::kOutRaw : (outMode > TuningMenu::kOutPq ? TuningMenu::kOutPq : outMode));
    cb.opt[1] = textureAlpha ? g_wOpacity : 0.88f;           // panel: its 88 % background; fps box: fps_opacity (phase 51f)
    cb.opt[2] = textureAlpha ? 1.0f : 0.0f;                  // 1 = the fps box: alpha comes from the texture
    ctx->UpdateSubresource(g_cb.Get(), 0, nullptr, &cb, 0, 0);
    SaveState(ctx, vsCab != nullptr);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ID3D11RenderTargetView* rt = rtv;
    ctx->OMSetRenderTargets(1, &rt, nullptr);
    D3D11_VIEWPORT vp{};
    vp.Width = (float)rtW; vp.Height = (float)rtH; vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    const FLOAT bf[4] = { 0, 0, 0, 0 };
    ID3D11Buffer* cbs = g_cb.Get();
    ID3D11SamplerState* smp = g_smp.Get();
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(vsCab ? vsCab : g_vs.Get(), nullptr, 0);
    if (vsCab) ctx->VSSetShaderResources(0, 1, &vsSrv);     // phase 47: the record's cbuffer mirror
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(g_ps.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &cbs);
    ctx->PSSetConstantBuffers(0, 1, &cbs);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->RSSetState(g_rs.Get());
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetBlendState(overlay ? g_blendOv.Get() : g_blend.Get(), bf, 0xffffffffu);
    ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    ctx->Draw(4, 0);                                         // (hooked: passes straight through inside t_inDlaa)
    RestoreState(ctx);
}

// One textured quad of `srv` (w x h px, top-left at x0, y0 rounded to whole pixels) into `rtv` (DrawCb).
void DrawQuad(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t rtW, uint32_t rtH, float x0, float y0,
              float w, float h, int outMode, ID3D11ShaderResourceView* srv, bool textureAlpha) {
    const float px0 = std::floor(x0 + 0.5f), py0 = std::floor(y0 + 0.5f);   // whole pixels: 1:1 texels stay sharp
    CbData cb{};
    cb.rect[0] = px0 / (float)rtW * 2.0f - 1.0f;
    cb.rect[1] = 1.0f - py0 / (float)rtH * 2.0f;
    cb.rect[2] = (px0 + w) / (float)rtW * 2.0f - 1.0f;
    cb.rect[3] = 1.0f - (py0 + h) / (float)rtH * 2.0f;
    cb.opt[3] = 0.0f;                                        // the rect
    DrawCb(ctx, rtv, rtW, rtH, cb, outMode, srv, textureAlpha);
}

} // namespace

namespace TuningMenu {

void RegisterShaders() {
    ShaderCache::Add(ShaderCache::kMenuVs, kMenuVs, sizeof(kMenuVs) - 1, "menu_vs", "VSMain", "vs_5_0");
    ShaderCache::Add(ShaderCache::kMenuPs, kMenuPs, sizeof(kMenuPs) - 1, "menu_ps", "PSMain", "ps_5_0");
    ShaderCache::Add(ShaderCache::kMenuCabVs, kMenuVs, sizeof(kMenuVs) - 1, "menu_cab_vs", "VSCab", "vs_5_0");   // phase 47
}

float RefHeight(int nRows) {
    if (nRows < 0) nRows = 0;
    if (nRows > kMaxRows) nRows = kMaxRows;
    return kRowsY + kRowH * (float)nRows + kDescGap + kDescH + kFootGap + kFootH + kBottom;
}

// v0.10.3 phase 29c: measured at scale 1 (the small font = 13 px there): the widest case relative to the panel -- the size is
// rounded up from 12.5 and the hinting widens small text (Segoe UI: the default footer is 430 px at 13 px, 403 at 50 px / 4),
// so what fits here fits at every larger build scale (VR quad 1.6+). The room = the panel width - both margins - the licence
// text - the gap, as RenderPanel lays the line out. No GDI = "no".
bool FooterFits(const wchar_t* footer) {
    if (!footer || !footer[0]) return true;
    constexpr float kM = 1.0f;
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HFONT f = MakeFont(12.5f * kM, FW_NORMAL);
    if (!f) { DeleteDC(dc); return false; }
    HGDIOBJ old = SelectObject(dc, f);
    SIZE a{}, b{};
    const bool ok = GetTextExtentPoint32W(dc, footer, (int)wcslen(footer), &a) &&
                    GetTextExtentPoint32W(dc, kLicence, (int)(sizeof(kLicence) / sizeof(kLicence[0]) - 1), &b);
    SelectObject(dc, old);
    DeleteObject(f);
    DeleteDC(dc);
    return ok && (float)a.cx / kM <= kRefWidth - 2.0f * kFootX0 - (float)b.cx / kM - kFootGapX;
}

bool Draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t rtW, uint32_t rtH, float x0, float y0,
          float scale, float buildScale, int outMode, const Panel& p) {
    if (!ctx || !rtv || !rtW || !rtH || !(scale > 0.0f) || !(buildScale > 0.0f)) return false;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev || !Ensure(dev.Get())) return false;
    if (!EnsureTexture(dev.Get(), ctx, p, buildScale)) return false;
    const float k = scale / g_texBuild;
    DrawQuad(ctx, rtv, rtW, rtH, x0, y0, (float)g_texW * k, (float)g_texH * k, outMode, g_srv.Get(), false);
    return true;
}

bool DrawCorners(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t rtW, uint32_t rtH, const float clip[4][4],
                 float buildScale, int outMode, const Panel& p) {
    if (!ctx || !rtv || !rtW || !rtH || !clip || !(buildScale > 0.0f)) return false;
    for (int i = 0; i < 4; ++i) {
        for (int k = 0; k < 4; ++k)
            if (!std::isfinite(clip[i][k])) return false;
        if (!(clip[i][3] > 0.0f)) return false;              // every corner in front of the eye
    }
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev || !Ensure(dev.Get())) return false;
    if (!EnsureTexture(dev.Get(), ctx, p, buildScale)) return false;
    CbData cb{};
    memcpy(cb.corner, clip, sizeof(cb.corner));
    cb.opt[3] = 1.0f;                                        // the corners
    DrawCb(ctx, rtv, rtW, rtH, cb, outMode, g_srv.Get(), false);
    return true;
}

void ReleaseTargets() {
    DropTexture();
    g_pixels.clear();
    g_pixels.shrink_to_fit();
}

bool DrawWidget(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t rtW, uint32_t rtH, float x0, float y0,
                float scale, float buildScale, int outMode, const wchar_t* line1, const wchar_t* line2) {
    if (!ctx || !rtv || !rtW || !rtH || !(scale > 0.0f) || !(buildScale > 0.0f)) return false;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev || !Ensure(dev.Get())) return false;
    if (!EnsureWidgetTexture(dev.Get(), ctx, line1, line2, buildScale)) return false;
    const float k = scale / g_wBuild;
    DrawQuad(ctx, rtv, rtW, rtH, x0, y0, (float)g_wTexW * k, (float)g_wTexH * k, outMode, g_wSrv.Get(), true);
    return true;
}

// phase 44: the fps box turned (fps_vr_yaw / pitch / roll) in the drawn-in VR fallback -- DrawCorners with the box's texture.
bool DrawWidgetCorners(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t rtW, uint32_t rtH, const float clip[4][4],
                       float buildScale, int outMode, const wchar_t* line1, const wchar_t* line2) {
    if (!ctx || !rtv || !rtW || !rtH || !clip || !(buildScale > 0.0f)) return false;
    for (int i = 0; i < 4; ++i) {
        for (int k = 0; k < 4; ++k)
            if (!std::isfinite(clip[i][k])) return false;
        if (!(clip[i][3] > 0.0f)) return false;              // every corner in front of the eye
    }
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev || !Ensure(dev.Get())) return false;
    if (!EnsureWidgetTexture(dev.Get(), ctx, line1, line2, buildScale)) return false;
    CbData cb{};
    memcpy(cb.corner, clip, sizeof(cb.corner));
    cb.opt[3] = 1.0f;                                        // the corners
    DrawCb(ctx, rtv, rtW, rtH, cb, outMode, g_wSrv.Get(), true);
    return true;
}

// phase 47: the fps box attached to the vehicle -- VSCab places `model` (4 model-space corners of the reference cab draw) with
// that draw's MVP read on the GPU from `mirror` at `mvpByteOff` (see fps_cab.h). Texture / states as DrawWidget.
bool DrawWidgetMvp(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t rtW, uint32_t rtH, ID3D11ShaderResourceView* mirror,
                   uint32_t mvpByteOff, const float model[4][4], const float map[4], float buildScale, int outMode,
                   const wchar_t* line1, const wchar_t* line2) {
    if (!ctx || !rtv || !rtW || !rtH || !mirror || !model || !map || !(buildScale > 0.0f) || (mvpByteOff & 3u)) return false;
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k)
            if (!std::isfinite(model[i][k])) return false;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev || !Ensure(dev.Get())) return false;
    if (!g_vsCab) {
        if (g_vsCabFailed || !ShaderCache::Ready(ShaderCache::kMenuCabVs)) return false;
        size_t n = 0;
        const void* code = ShaderCache::Code(ShaderCache::kMenuCabVs, &n);
        HRESULT hr = E_FAIL;
        if (!code || FAILED(hr = dev->CreateVertexShader(code, n, nullptr, &g_vsCab))) {
            g_vsCabFailed = true;
            LogErr("tuning menu: fps box (attached to vehicle) VS not available -- %s hr=0x%lx", code ? "CreateVertexShader failed"
                   : ShaderCache::Error(ShaderCache::kMenuCabVs), (unsigned long)hr);
            return false;
        }
    }
    if (!EnsureWidgetTexture(dev.Get(), ctx, line1, line2, buildScale)) return false;
    CbData cb{};
    memcpy(cb.corner, model, sizeof(cb.corner));
    cb.opt[3] = 1.0f;
    cb.cab[0] = mvpByteOff;
    memcpy(cb.map, map, sizeof(cb.map));
    DrawCb(ctx, rtv, rtW, rtH, cb, outMode, g_wSrv.Get(), true, g_vsCab.Get(), mirror);
    return true;
}

void SetWidgetColors(uint8_t tr, uint8_t tg, uint8_t tb, uint8_t lr, uint8_t lg, uint8_t lb) {
    g_wText = RGB(tr, tg, tb);                               // the cache compares them (EnsureWidgetTexture): a change rebuilds once
    g_wLine = RGB(lr, lg, lb);
}

void SetWidgetOpacity(float a) { g_wOpacity = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a); }

void ReleaseWidget() {
    DropWidget();
    g_wPixels.clear();
    g_wPixels.shrink_to_fit();
}

}  // namespace TuningMenu

#endif // WITH_DLAA
