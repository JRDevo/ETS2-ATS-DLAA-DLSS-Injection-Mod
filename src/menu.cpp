// TuningMenu implementation (v0.10.0). See menu.h.
#ifdef WITH_DLAA

#include "menu.h"
#include "shader_cache.h"
#include "log.h"
#include <windows.h>
#include <wrl/client.h>
#include <wincodec.h>                                // phase 74: SavePanelPng (the language dump)
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <mutex>                                    // phase 115r: RenderBug's fit memo
#include <string>
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
// phase 88: the history graph -- the columns as last handed over (under g_wGraphLock: SetWidgetGraph may run on another thread)
// and what the cached texture shows (on / gen; the strip = the rows from g_wTextH down)
SRWLOCK                          g_wGraphLock = SRWLOCK_INIT;
TuningMenu::WidgetGraph          g_wGraph;
bool                             g_wLastGraphOn = false;
uint32_t                         g_wLastGraphGen = 0;
// phase 115: the census BUG ICON under the fps box (VR: SetWidgetBug; "" = none) -- the lines as handed over (under g_wGraphLock) and
// those of the cached texture
wchar_t                          g_wBug1[32] = {}, g_wBug2[32] = {}, g_wBug3[32] = {};   // (116: + the "fix is off" line)
wchar_t                          g_wLastBug1[32] = {}, g_wLastBug2[32] = {}, g_wLastBug3[32] = {};
uint32_t                         g_wTextH = 0;

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
    g_wTextH = 0;
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
constexpr float kDescGap = 6.0f,  kDescH = 84.0f;   // kDescLines lines (phase 74: 4, was 3 = 64: longer languages)
constexpr int   kDescLines = 4;
constexpr float kFootGap = 2.0f,  kFootH = 18.0f, kBottom = 4.0f;
constexpr float kNameX0 = 14.0f,  kNameX1 = 256.0f;   // phase 74: names / values 20 / 16 px wider, the hotkeys 36 px narrower
constexpr float kValX0  = 260.0f, kValX1  = 420.0f;   //   (longer languages; the hotkey texts are short)
constexpr float kKeyX0  = 424.0f, kKeyX1  = 640.0f;
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

// phase 74: the panel's face follows the menu language (TuningMenu::SetFace: Segoe UI, or a CJK face); the fps box keeps Segoe UI
wchar_t g_face[64] = L"Segoe UI";
HFONT MakeFont(float px, int weight, const wchar_t* face = g_face) {
    HFONT f = CreateFontW(-(int)std::lround(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
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

// GDI -> out (RGBA, w x h) in `face`. False = GDI failed (logged once). Touches no global but the fail flag, so the language dump
// (phase 74) can run it on its own thread.
bool RenderPanel(const TuningMenu::Panel& p, float s, uint32_t w, uint32_t h, const wchar_t* face, std::vector<uint32_t>& out) {
    static bool s_failLogged = false;
    Dib d;
    if (!DibBegin(d, w, h, "panel", s_failLogged)) return false;
    HDC dc = d.dc;
    HFONT fBody  = MakeFont(15.0f * s, FW_NORMAL, face);
    HFONT fTitle = MakeFont(17.0f * s, FW_BOLD, face);
    HFONT fSmall = MakeFont(12.5f * s, FW_NORMAL, face);
    HFONT fDesc  = MakeFont(14.0f * s, FW_NORMAL, face);
    HFONT fPerf  = MakeFont(12.0f * s, FW_NORMAL, face);
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
    DibFinish(d, w, h, out);
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
// phase 88: tight = the history graph sits under the text (WidgetTextRefH): one line in y 2..22 instead of the centred 12..32.
// phase 91: the pixels go to `out` and the colours are arguments (were g_wPixels / g_wText / g_wLine, the mask a static): the status
// box paints from two threads (the eyes on the render thread, the desktop mirror at Present), each into its own buffer, no shared state.
bool RenderWidget(const wchar_t* line1, const wchar_t* line2, float s, uint32_t w, uint32_t h, bool tight, std::vector<uint32_t>& out,
                  COLORREF textCol, COLORREF lineCol) {
    static std::atomic<bool> s_failLogged{ false };
    bool failLogged = s_failLogged.load(std::memory_order_relaxed);
    std::vector<uint32_t> maskPx;
    HFONT fBig   = MakeFont(15.0f * s, FW_BOLD, L"Segoe UI");
    HFONT fSmall = MakeFont(12.0f * s, FW_NORMAL, L"Segoe UI");
    HFONT stock  = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    auto pick = [&](HFONT f) { return f ? f : stock; };
    auto px = [&](float v) { return (LONG)std::lround(v * s); };
    const float W = (float)w / s;                            // reference px
    const LONG o = std::max<LONG>(1, (LONG)std::lround(1.0f * s));   // outline offset in device px
    // One DIB: mask = true draws the outline and the text all in white, false the colour version.
    auto paint = [&](bool mask, std::vector<uint32_t>& dst) -> bool {
        Dib d;
        if (!DibBegin(d, w, h, "fps box", failLogged)) return false;
        HDC dc = d.dc;
        HGDIOBJ oldFont = SelectObject(dc, pick(fBig));
        SetBkMode(dc, TRANSPARENT);                          // the DIB starts black (zeroed)
        auto line = [&](HFONT f, const wchar_t* t, float y0, float y1) {
            if (!t || !t[0]) return;
            SelectObject(dc, pick(f));
            const UINT fl = DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
            SetTextColor(dc, mask ? RGB(255, 255, 255) : lineCol);   // phase 44: fps_color's outline (was RGB 8,8,8)
            for (LONG dy = -o; dy <= o; dy += o)
                for (LONG dx = -o; dx <= o; dx += o) {
                    if (!dx && !dy) continue;
                    RECT r{ px(3.0f) + dx, px(y0) + dy, px(W - 3.0f) + dx, px(y1) + dy };
                    DrawTextW(dc, t, -1, &r, fl);
                }
            SetTextColor(dc, mask ? RGB(255, 255, 255) : textCol);   // phase 44: fps_color's text (was white)
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
            line(fBig, has1 ? line1 : line2, tight ? 2.0f : 12.0f, tight ? 22.0f : 32.0f);
        }
        SelectObject(dc, oldFont);
        DibFinish(d, w, h, dst);
        return true;
    };
    const bool ok = paint(false, out) && paint(true, maskPx) && maskPx.size() == out.size();
    if (failLogged) s_failLogged.store(true, std::memory_order_relaxed);
    if (ok)
        for (size_t i = 0, e = out.size(); i < e; ++i)
            out[i] = (out[i] & 0x00FFFFFFu) | ((maskPx[i] & 0xFFu) << 24);   // A = the mask's red (RGBA: R is the low byte)
    if (fBig) DeleteObject(fBig);
    if (fSmall) DeleteObject(fSmall);
    return ok;
}

// phase 115: the census BUG ICON -- GDI -> out (RGBA, w x h): a dark rounded plate (alpha ~67 %), a bug DRAWN with GDI shapes (no
// font glyph, no emoji: legs, antennae, a red body with a white rim, a dark head, a back line and spots) on the left in a square as
// tall as the plate, and two lines right of it: line1 (bold, yellow) "3 to capture", line2 (white) "5 captured", outlined like the
// fps box. The font shrinks (13 -> 7 reference px) until both lines fit the column, so nothing is cut. Same colour / mask DIB pair as
// RenderWidget (A = the mask's red). False = GDI failed.
// phase 116: line3 (null / "" = none) = a small third line under them ("2: fix is off", light grey, kBug3Less px smaller than line 1)
// in the same box: the three rows share the height in proportion to their fonts, and the fit loop counts all three.
constexpr float kBug3Less = 2.5f;
bool RenderBug(const wchar_t* line1, const wchar_t* line2, float s, uint32_t w, uint32_t h, std::vector<uint32_t>& out,
               const wchar_t* line3 = nullptr) {
    const bool has3 = line3 && line3[0];
    static std::atomic<bool> s_failLogged{ false };
    bool failLogged = s_failLogged.load(std::memory_order_relaxed);
    const float W = (float)w / s, H = (float)h / s;          // reference px
    auto px = [&](float v) { return (LONG)std::lround(v * s); };
    const LONG o = std::max<LONG>(1, (LONG)std::lround(1.0f * s));
    const float ix = 2.0f, iy = 2.0f, is = H - 4.0f;          // the icon square
    const float tx0 = ix + is + 3.0f, tx1 = W - 3.0f;         // the text column
    // the font size that fits both lines (measured at this scale). Phase 115r (P5): memoised -- the key is the scale, the size and the
    // two lines with every digit as '0' (Segoe UI's digits are tabular: "12 to capture" fits as "34 to capture"), so a count change
    // costs no measuring (up to 12 x 2 font creations before)
    float fs = 13.0f;
    struct FitMemo { float s = 0.0f; uint32_t w = 0, h = 0; std::wstring a, b, c; float fs = 0.0f; };
    static std::mutex s_fitMx;
    static FitMemo s_fit[4];
    static unsigned s_fitNext = 0;
    auto norm = [](const wchar_t* t) {
        std::wstring r = t ? t : L"";
        for (wchar_t& c : r) if (c >= L'0' && c <= L'9') c = L'0';
        return r;
    };
    const std::wstring n1 = norm(line1), n2 = norm(line2), n3 = norm(line3);
    bool memoHit = false;
    {
        std::lock_guard<std::mutex> lk(s_fitMx);
        for (const FitMemo& m : s_fit)
            if (m.fs > 0.0f && m.s == s && m.w == w && m.h == h && m.a == n1 && m.b == n2 && m.c == n3) { fs = m.fs; memoHit = true; break; }
    }
    if (!memoHit) {
        HDC dc = CreateCompatibleDC(nullptr);
        for (; dc && fs > 7.0f; fs -= 0.5f) {
            HFONT fb = MakeFont(fs * s, FW_BOLD, L"Segoe UI"), fn = MakeFont((fs - 1.0f) * s, FW_SEMIBOLD, L"Segoe UI");
            SIZE a{}, b{};
            HGDIOBJ old = SelectObject(dc, fb ? fb : GetStockObject(DEFAULT_GUI_FONT));
            if (line1 && line1[0]) GetTextExtentPoint32W(dc, line1, (int)wcslen(line1), &a);
            SelectObject(dc, fn ? fn : GetStockObject(DEFAULT_GUI_FONT));
            if (line2 && line2[0]) GetTextExtentPoint32W(dc, line2, (int)wcslen(line2), &b);
            SIZE c3{};                                       // phase 116: the third line (its own smaller font)
            HFONT f3 = has3 ? MakeFont((fs - kBug3Less) * s, FW_SEMIBOLD, L"Segoe UI") : nullptr;
            if (has3) {
                SelectObject(dc, f3 ? f3 : GetStockObject(DEFAULT_GUI_FONT));
                GetTextExtentPoint32W(dc, line3, (int)wcslen(line3), &c3);
            }
            SelectObject(dc, old);
            if (fb) DeleteObject(fb);
            if (fn) DeleteObject(fn);
            if (f3) DeleteObject(f3);
            const float need = (float)(std::max)((std::max)(a.cx, b.cx), c3.cx) / s + 2.0f;
            const float tall = has3 ? 3.0f * fs - 1.0f - kBug3Less : 2.0f * fs;
            if (need <= tx1 - tx0 && tall <= H + 2.0f) break;
        }
        if (dc) DeleteDC(dc);
        if (dc) {
            std::lock_guard<std::mutex> lk(s_fitMx);
            s_fit[s_fitNext++ & 3u] = FitMemo{ s, w, h, n1, n2, n3, fs };
        }
    }
    HFONT fBig = MakeFont(fs * s, FW_BOLD, L"Segoe UI"), fSmall = MakeFont((fs - 1.0f) * s, FW_SEMIBOLD, L"Segoe UI");
    HFONT fTiny = has3 ? MakeFont((fs - kBug3Less) * s, FW_SEMIBOLD, L"Segoe UI") : nullptr;   // phase 116
    HFONT stock = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    auto pick = [&](HFONT f) { return f ? f : stock; };
    std::vector<uint32_t> maskPx;
    auto paint = [&](bool mask, std::vector<uint32_t>& dst) -> bool {
        Dib d;
        if (!DibBegin(d, w, h, "bug icon", failLogged)) return false;
        HDC dc = d.dc;
        HGDIOBJ oldFont = SelectObject(dc, pick(fBig));
        SetBkMode(dc, TRANSPARENT);
        auto brush = [&](COLORREF c, COLORREF m) { return CreateSolidBrush(mask ? m : c); };
        auto pen = [&](COLORREF c, float wd) { return CreatePen(PS_SOLID, std::max<int>(1, (int)std::lround(wd * s)), mask ? RGB(255, 255, 255) : c); };
        // the plate
        {
            HBRUSH b = brush(RGB(16, 16, 20), RGB(170, 170, 170));
            HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, 0, 0, (int)w + 1, (int)h + 1, px(7.0f), px(7.0f));
            SelectObject(dc, ob); SelectObject(dc, op);
            DeleteObject(b);
        }
        // the bug (u, v in 0..1 of the icon square)
        auto X = [&](float u) { return (int)std::lround((ix + u * is) * s); };
        auto Y = [&](float v) { return (int)std::lround((iy + v * is) * s); };
        auto seg = [&](float u0, float v0, float u1, float v1) { MoveToEx(dc, X(u0), Y(v0), nullptr); LineTo(dc, X(u1), Y(v1)); };
        {
            HPEN lp = pen(RGB(235, 235, 235), 1.6f);
            HGDIOBJ op = SelectObject(dc, lp);
            const float legs[3][4] = { { 0.33f, 0.50f, 0.06f, 0.36f }, { 0.30f, 0.66f, 0.03f, 0.66f }, { 0.33f, 0.82f, 0.07f, 0.97f } };
            for (const auto& l : legs) { seg(l[0], l[1], l[2], l[3]); seg(1.0f - l[0], l[1], 1.0f - l[2], l[3]); }
            seg(0.44f, 0.22f, 0.30f, 0.03f); seg(0.56f, 0.22f, 0.70f, 0.03f);   // antennae
            SelectObject(dc, op);
            DeleteObject(lp);
        }
        {
            HPEN rim = pen(RGB(255, 255, 255), 1.2f);
            HBRUSH body = brush(RGB(232, 36, 28), RGB(255, 255, 255));
            HGDIOBJ op = SelectObject(dc, rim), ob = SelectObject(dc, body);
            Ellipse(dc, X(0.27f), Y(0.34f), X(0.73f), Y(0.99f));
            SelectObject(dc, ob);
            DeleteObject(body);
            HBRUSH head = brush(RGB(40, 40, 44), RGB(255, 255, 255));
            ob = SelectObject(dc, head);
            Ellipse(dc, X(0.36f), Y(0.15f), X(0.64f), Y(0.41f));
            SelectObject(dc, ob); SelectObject(dc, op);
            DeleteObject(head);
            DeleteObject(rim);
            if (!mask) {                                     // the back line + the spots (inside the body: the mask is white there already)
                HPEN bl = CreatePen(PS_SOLID, std::max<int>(1, (int)std::lround(1.1f * s)), RGB(20, 20, 20));
                HBRUSH sp = CreateSolidBrush(RGB(20, 20, 20));
                op = SelectObject(dc, bl);
                seg(0.5f, 0.42f, 0.5f, 0.97f);
                ob = SelectObject(dc, sp);
                HGDIOBJ np = SelectObject(dc, GetStockObject(NULL_PEN));
                const float spots[4][2] = { { 0.39f, 0.58f }, { 0.61f, 0.58f }, { 0.39f, 0.80f }, { 0.61f, 0.80f } };
                for (const auto& p : spots) Ellipse(dc, X(p[0] - 0.055f), Y(p[1] - 0.055f), X(p[0] + 0.055f), Y(p[1] + 0.055f));
                SelectObject(dc, np); SelectObject(dc, ob); SelectObject(dc, op);
                DeleteObject(sp);
                DeleteObject(bl);
            }
        }
        // the two lines, outlined like the fps box
        auto line = [&](HFONT f, const wchar_t* t, float y0, float y1, COLORREF col) {
            if (!t || !t[0]) return;
            SelectObject(dc, pick(f));
            const UINT fl = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX;
            SetTextColor(dc, mask ? RGB(255, 255, 255) : RGB(8, 8, 8));
            for (LONG dy = -o; dy <= o; dy += o)
                for (LONG dx = -o; dx <= o; dx += o) {
                    if (!dx && !dy) continue;
                    RECT r{ px(tx0) + dx, px(y0) + dy, px(tx1) + dx, px(y1) + dy };
                    DrawTextW(dc, t, -1, &r, fl);
                }
            SetTextColor(dc, mask ? RGB(255, 255, 255) : col);
            RECT r{ px(tx0), px(y0), px(tx1), px(y1) };
            DrawTextW(dc, t, -1, &r, fl);
        };
        if (!has3) {
            line(fBig, line1, 1.0f, 0.5f * H + 0.5f, RGB(255, 222, 0));
            line(fSmall, line2, 0.5f * H - 0.5f, H - 1.0f, RGB(255, 255, 255));
        } else {                                             // phase 116: three rows, heights in proportion to their fonts
            const float f1 = fs, f2 = fs - 1.0f, f3 = fs - kBug3Less, k = (H - 2.0f) / (f1 + f2 + f3);
            const float y1 = 1.0f + f1 * k, y2 = y1 + f2 * k;
            line(fBig, line1, 1.0f, y1 + 0.5f, RGB(255, 222, 0));
            line(fSmall, line2, y1 - 0.5f, y2 + 0.5f, RGB(255, 255, 255));
            line(fTiny, line3, y2 - 0.5f, H - 1.0f, RGB(190, 200, 210));
        }
        SelectObject(dc, oldFont);
        DibFinish(d, w, h, dst);
        return true;
    };
    const bool ok = paint(false, out) && paint(true, maskPx) && maskPx.size() == out.size();
    if (failLogged) s_failLogged.store(true, std::memory_order_relaxed);
    if (ok)
        for (size_t i = 0, e = out.size(); i < e; ++i)
            out[i] = (out[i] & 0x00FFFFFFu) | ((maskPx[i] & 0xFFu) << 24);
    if (fBig) DeleteObject(fBig);
    if (fSmall) DeleteObject(fSmall);
    if (fTiny) DeleteObject(fTiny);
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
    if (!RenderPanel(p, buildScale, w, h, g_face, g_pixels)) return false;
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
// phase 88: the history graph strip into px (w x rows, RGBA; R = the low byte), s px per reference px. 3-px margins left / right,
// 2 px at the bottom; one bar per column from the bottom up (green / yellow / red against targetMs, the top = 2 x targetMs), then
// the target line (half-way up) over the bars. Plain pixel writes, no GDI. Phase 88c (VR test 2026-11-01): no dark background any
// more -- the game shows through; the target line is white with a dark edge above and below, so it reads on sky and on road.
void PaintGraph(uint32_t* px, uint32_t w, uint32_t rows, float s, const TuningMenu::WidgetGraph& g) {
    std::fill(px, px + (size_t)w * rows, 0u);
    const LONG m = std::max<LONG>(1, std::lround(3.0f * s)), b = std::max<LONG>(1, std::lround(2.0f * s));
    const LONG x0 = m, x1 = (LONG)w - m, y1 = (LONG)rows - b;   // the strip: x0..x1, 0..y1 (y down)
    if (x1 - x0 < 4 || y1 < 4 || !(g.targetMs > 0.0f)) return;
    auto rgba = [](uint32_t r, uint32_t gg, uint32_t bb, uint32_t a) { return r | (gg << 8) | (bb << 16) | (a << 24); };
    const uint32_t green = rgba(70, 205, 95, 255), yellow = rgba(245, 200, 50, 255), red = rgba(235, 60, 50, 255);
    const float top = 2.0f * g.targetMs;
    for (LONG x = x0; x < x1; ++x) {
        // the columns under this pixel (more than one when the strip is narrower than kGraphCols px: a small fps_scale): the slowest
        const int c0 = (int)((int64_t)(x - x0) * TuningMenu::kGraphCols / (x1 - x0));
        const int c1 = (std::max)(c0 + 1, (int)((int64_t)(x + 1 - x0) * TuningMenu::kGraphCols / (x1 - x0)));
        float ms = 0.0f;
        for (int c = c0; c < c1 && c < TuningMenu::kGraphCols; ++c) ms = (std::max)(ms, g.colMs[c]);
        if (!(ms > 0.0f)) continue;
        const float f = ms >= top ? 1.0f : ms / top;
        const LONG hgt = std::max<LONG>(1, std::lround(f * (float)y1));
        const uint32_t col = ms <= 1.1f * g.targetMs ? green : (ms <= 1.5f * g.targetMs ? yellow : red);
        for (LONG y = y1 - hgt; y < y1; ++y) px[(size_t)y * w + x] = col;
    }
    const LONG lt = std::max<LONG>(1, std::lround(0.75f * s)), ly = y1 / 2, edge = std::max<LONG>(1, std::lround(0.5f * s));
    // "over" what is there (straight alpha): c at alpha a
    auto over = [&](uint32_t& p, uint32_t c, uint32_t a) {
        const uint32_t pa = p >> 24, oa = a + pa * (255 - a) / 255;
        auto ch = [&](int sh) { const uint32_t pc = (p >> sh) & 0xFF, cc = (c >> sh) & 0xFF;
                                return oa ? (cc * a + pc * pa * (255 - a) / 255) / oa : 0u; };
        p = rgba(ch(0), ch(8), ch(16), oa);
    };
    for (LONG y = ly - edge; y < ly + lt + edge && y < y1; ++y) {
        if (y < 0) continue;
        const bool core = y >= ly && y < ly + lt;
        for (LONG x = x0; x < x1; ++x)
            over(px[(size_t)y * w + x], core ? rgba(255, 255, 255, 0) : rgba(0, 0, 0, 0), core ? 235u : 150u);
    }
}

// The fps box texture for the two lines at `buildScale`: rebuilt only when a line or the scale changed. phase 88: + the history
// graph strip under the text (graph on): a new column repaints and uploads only the strip's rows.
bool EnsureWidgetTexture(ID3D11Device* dev, ID3D11DeviceContext* ctx, const wchar_t* line1, const wchar_t* line2, float buildScale) {
    wchar_t l1[kWLine], l2[kWLine];
    CopyLine(l1, line1);
    CopyLine(l2, line2);
    static TuningMenu::WidgetGraph s_g;                      // (render thread only; 190 bytes)
    wchar_t b1[32], b2[32], b3[32];                          // phase 115: the bug icon strip under it (VR); 116: + line 3
    AcquireSRWLockShared(&g_wGraphLock);
    s_g = g_wGraph;
    wcscpy_s(b1, g_wBug1); wcscpy_s(b2, g_wBug2); wcscpy_s(b3, g_wBug3);
    ReleaseSRWLockShared(&g_wGraphLock);
    const bool bugOn = b1[0] || b2[0];
    const bool same = g_wTex && g_wHave && std::fabs(g_wBuild - buildScale) < 0.001f && !wcscmp(l1, g_wLast1) && !wcscmp(l2, g_wLast2) &&
        g_wLastText == g_wText && g_wLastLine == g_wLine && g_wLastGraphOn == s_g.on &&   // phase 44: + the colours; 88: + graph on
        !wcscmp(b1, g_wLastBug1) && !wcscmp(b2, g_wLastBug2) && !wcscmp(b3, g_wLastBug3);   // phase 115: + the bug lines
    if (same && (!s_g.on || s_g.gen == g_wLastGraphGen)) return true;
    // phase 88: the text block as tall as its lines (graph on), the strip right under it; phase 115: the bug strip under both
    const float refT = TuningMenu::WidgetTextRefH(l1, l2, s_g.on);
    const uint32_t w = (uint32_t)std::lround(TuningMenu::kWidgetRefW * buildScale);
    const uint32_t ht = (uint32_t)std::lround(refT * buildScale);
    const uint32_t hg = s_g.on ? (uint32_t)std::lround((refT + TuningMenu::kWidgetGraphH) * buildScale) : ht;
    const uint32_t h = bugOn ? (uint32_t)std::lround((refT + (s_g.on ? TuningMenu::kWidgetGraphH : 0.0f) + TuningMenu::kWidgetBugH) *
                                                     buildScale) : hg;
    if (w < 8 || h < 8 || w > 4096 || h > 4096 || hg > h) return false;
    if (same && g_wTextH == ht && g_wTexH == h && g_wPixels.size() == (size_t)w * h) {   // phase 88: only a new column
        uint32_t* const strip = g_wPixels.data() + (size_t)w * ht;
        PaintGraph(strip, w, hg - ht, buildScale, s_g);
        const D3D11_BOX box{ 0, ht, 0, w, hg, 1 };
        ctx->UpdateSubresource(g_wTex.Get(), 0, &box, strip, w * 4u, 0);
        g_wLastGraphGen = s_g.gen;
        return true;
    }
    if (!ht) g_wPixels.clear();                              // phase 88: graph only (no line): no text rows
    else if (!RenderWidget(l1, l2, buildScale, w, ht, s_g.on, g_wPixels, g_wText, g_wLine)) return false;
    if (h > ht) {
        g_wPixels.resize((size_t)w * h);
        if (hg > ht) PaintGraph(g_wPixels.data() + (size_t)w * ht, w, hg - ht, buildScale, s_g);
    }
    if (h > hg) {                                            // phase 115: the bug icon strip
        std::vector<uint32_t> bug;
        if (!RenderBug(b1, b2, buildScale, w, h - hg, bug, b3) || bug.size() != (size_t)w * (h - hg)) return false;
        std::copy(bug.begin(), bug.end(), g_wPixels.begin() + (size_t)w * hg);
    }
    if (!UploadTexture(dev, ctx, g_wTex, g_wSrv, g_wTexW, g_wTexH, w, h, g_wPixels, "fps box")) return false;
    g_wBuild = buildScale;
    memcpy(g_wLast1, l1, sizeof(l1));
    memcpy(g_wLast2, l2, sizeof(l2));
    g_wLastText = g_wText;
    g_wLastLine = g_wLine;
    g_wLastGraphOn = s_g.on;
    g_wLastGraphGen = s_g.gen;
    wcscpy_s(g_wLastBug1, b1); wcscpy_s(g_wLastBug2, b2); wcscpy_s(g_wLastBug3, b3);
    g_wTextH = ht;
    g_wHave = true;
    return true;
}

// One textured quad of `srv` into `rtv` from `cb` (its rect / corners filled by the caller; the options are set here), every
// touched state saved and restored (SaveState / RestoreState). The device objects must exist (Ensure).
// phase 47: vsCab != null = that vertex shader (VSCab) with `vsSrv` at VS t0 instead of VSMain (saved / restored as well).
void DrawCb(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t rtW, uint32_t rtH, CbData& cb, int outMode,
            ID3D11ShaderResourceView* srv, bool textureAlpha, ID3D11VertexShader* vsCab = nullptr,
            ID3D11ShaderResourceView* vsSrv = nullptr, float alphaOverride = -1.0f) {
    const bool overlay = (outMode & TuningMenu::kOutOverlay) != 0;   // v0.10.3 phase 29: blend off, RGBA written
    outMode &= ~TuningMenu::kOutOverlay;
    cb.opt[0] = (float)(outMode < TuningMenu::kOutRaw ? TuningMenu::kOutRaw : (outMode > TuningMenu::kOutPq ? TuningMenu::kOutPq : outMode));
    cb.opt[1] = textureAlpha ? (alphaOverride >= 0.0f ? alphaOverride : g_wOpacity) : 0.88f;   // panel: its 88 % background; fps box:
                                                             // fps_opacity (phase 51f); phase 91: the status box passes its own 1.0
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
              float w, float h, int outMode, ID3D11ShaderResourceView* srv, bool textureAlpha, float alphaOverride = -1.0f) {
    const float px0 = std::floor(x0 + 0.5f), py0 = std::floor(y0 + 0.5f);   // whole pixels: 1:1 texels stay sharp
    CbData cb{};
    cb.rect[0] = px0 / (float)rtW * 2.0f - 1.0f;
    cb.rect[1] = 1.0f - py0 / (float)rtH * 2.0f;
    cb.rect[2] = (px0 + w) / (float)rtW * 2.0f - 1.0f;
    cb.rect[3] = 1.0f - (py0 + h) / (float)rtH * 2.0f;
    cb.opt[3] = 0.0f;                                        // the rect
    DrawCb(ctx, rtv, rtW, rtH, cb, outMode, srv, textureAlpha, nullptr, nullptr, alphaOverride);
}

} // namespace

namespace TuningMenu {

void RegisterShaders() {
    ShaderCache::Add(ShaderCache::kMenuVs, kMenuVs, sizeof(kMenuVs) - 1, "menu_vs", "VSMain", "vs_5_0");
    ShaderCache::Add(ShaderCache::kMenuPs, kMenuPs, sizeof(kMenuPs) - 1, "menu_ps", "PSMain", "ps_5_0");
    ShaderCache::Add(ShaderCache::kMenuCabVs, kMenuVs, sizeof(kMenuVs) - 1, "menu_cab_vs", "VSCab", "vs_5_0");   // phase 47
}

// phase 74: the language dump -- `p` rendered at `scale` in `face` (GDI, as Draw would) and saved as a PNG (WIC). Any thread
// (COM is initialised here); no device objects.
bool SavePanelPng(const Panel& p, const wchar_t* face, float scale, const wchar_t* path) {
    const uint32_t w = (uint32_t)std::lround(kRefWidth * scale), h = (uint32_t)std::lround(RefHeight(p.nRows) * scale);
    std::vector<uint32_t> px;
    if (w < 16 || h < 16 || !RenderPanel(p, scale, w, h, face, px)) return false;
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> f;
        ComPtr<IWICStream> st;
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> fr;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;   // (the PNG encoder takes BGR; 32bpp RGBA it refuses)
        std::vector<BYTE> bgr((size_t)w * h * 3);
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            const uint32_t v = px[i];                        // R, G, B, A in memory
            bgr[i * 3 + 0] = (BYTE)(v >> 16); bgr[i * 3 + 1] = (BYTE)(v >> 8); bgr[i * 3 + 2] = (BYTE)v;
        }
        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
             SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path, GENERIC_WRITE)) &&
             SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
             SUCCEEDED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
             SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
             IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR) &&
             SUCCEEDED(fr->WritePixels(h, w * 3u, (UINT)bgr.size(), bgr.data())) &&
             SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (SUCCEEDED(ci)) CoUninitialize();
    return ok;
}

void SetFace(const wchar_t* face) {
    if (!face || !face[0] || !wcscmp(face, g_face)) return;
    wcsncpy_s(g_face, face, _TRUNCATE);
    g_haveLast = false;                                  // the next Draw rebuilds the panel texture in the new face
}

float RefHeight(int nRows) {
    if (nRows < 0) nRows = 0;
    if (nRows > kMaxRows) nRows = kMaxRows;
    return kRowsY + kRowH * (float)nRows + kDescGap + kDescH + kFootGap + kFootH + kBottom;
}

// v0.10.3 phase 29c: measured at scale 1 (the small font = 13 px there): the widest case relative to the panel -- the size is
// rounded up from 12.5 and the hinting widens small text (Segoe UI: the default footer is 430 px at 13 px, 403 at 50 px / 4),
// so what fits here fits at every larger build scale (VR quad: 3.2 x menu_vr_scale, phase 70). The room = the panel width - both margins - the licence
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

// phase 89e: the bug report's status box -- the fps box's look (outlined text, no background) at kStatusRefW x kWidgetRefH, always
// white on a black outline at full opacity (not the fps_color / fps_opacity of the box). Own texture cache keyed on the lines + scale.
// phase 91: ONE cache slot per caller (kStatusEyes / kStatusFlat). There was one cache for both, and the two build at different scales
// (the eyes W / 1920 x 1.1, the desktop mirror H / 1080 x 1.25 x 2), so with both on EVERY call rebuilt it -- GDI + texture create +
// upload, 1.58 ms/frame measured while the capture ran. Each slot owns its texture and pixels; RenderWidget paints into the slot's
// buffer with explicit colours and the opacity goes to the draw as an argument, so the eyes (render thread) and the mirror (Present)
// share no mutable state here any more (they swapped the fps box's g_wPixels / g_wText / g_wLine / g_wOpacity under each other).
namespace {
struct StatusSlot {
    ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; uint32_t texW = 0, texH = 0;
    float build = 0.0f; wchar_t last1[64] = {}, last2[64] = {}; std::vector<uint32_t> px;
};
StatusSlot g_status[TuningMenu::kStatusSlots];
}
bool DrawStatus(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t rtW, uint32_t rtH, float x0, float y0,
                float scale, float buildScale, int outMode, const wchar_t* line1, const wchar_t* line2, int slot) {
    if (!ctx || !rtv || !rtW || !rtH || !(scale > 0.0f) || !(buildScale > 0.0f) || slot < 0 || slot >= kStatusSlots) return false;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev || !Ensure(dev.Get())) return false;
    StatusSlot& c = g_status[slot];
    wchar_t l1[64], l2[64];
    wcsncpy_s(l1, line1 ? line1 : L"", _TRUNCATE);
    wcsncpy_s(l2, line2 ? line2 : L"", _TRUNCATE);
    if (!c.tex || std::fabs(c.build - buildScale) > 0.001f || wcscmp(l1, c.last1) || wcscmp(l2, c.last2)) {
        const uint32_t w = (uint32_t)std::lround(kStatusRefW * buildScale), h = (uint32_t)std::lround(kWidgetRefH * buildScale);
        if (w < 8 || h < 8 || w > 8192 || h > 4096) return false;
        if (!RenderWidget(l1, l2, buildScale, w, h, false, c.px, RGB(255, 255, 255), RGB(8, 8, 8)) ||
            !UploadTexture(dev.Get(), ctx, c.tex, c.srv, c.texW, c.texH, w, h, c.px, "status box")) {
            c.tex.Reset(); c.srv.Reset(); c.texW = c.texH = 0;   // (no half-updated cache: the next call tries again)
            return false;
        }
        c.build = buildScale;
        wcscpy_s(c.last1, l1); wcscpy_s(c.last2, l2);
    }
    const float k = scale / c.build;
    DrawQuad(ctx, rtv, rtW, rtH, x0, y0, (float)c.texW * k, (float)c.texH * k, outMode, c.srv.Get(), true, 1.0f);
    return true;
}

// phase 112b: the census legend. Phase 113 reference layout (scale 1): kLgPad all round; the title (bold 16 px) in a kLgTitleH row;
// kLgGap; one kLgLineH row per plain line (14 px, white); kLgKeyGap; the colour-key row (kLgKeyH): per entry a kLgSw square swatch,
// kLgSwGap, its word (bold 13 px, in its colour), kLgKeySep to the next entry. The width = the widest of title / lines / key row,
// MEASURED at the build scale (GDI's hinting widens small text, so the box is sized from the very glyphs it draws: nothing is cut at
// any scale). Background RGB 16, 17, 20 at kLgBgA / 255.
namespace {
constexpr float kLgPad = 10.0f, kLgTitleH = 24.0f, kLgGap = 4.0f, kLgLineH = 20.0f, kLgSw = 10.0f, kLgSwGap = 4.0f,
                kLgKeySep = 14.0f, kLgKeyGap = 6.0f, kLgKeyH = 19.0f;
constexpr uint32_t kLgBgA = 192;
struct LegendFonts {
    HFONT title = nullptr, word = nullptr, text = nullptr;
    explicit LegendFonts(float s) : title(MakeFont(16.0f * s, FW_BOLD)), word(MakeFont(13.0f * s, FW_BOLD)),
                                    text(MakeFont(14.0f * s, FW_NORMAL)) {}
    ~LegendFonts() { for (HFONT f : { title, word, text }) if (f) DeleteObject(f); }
    bool ok() const { return title && word && text; }
};
int LegendKeys(const TuningMenu::Legend& lg) { return lg.n < 0 ? 0 : (lg.n > TuningMenu::kLegendLines ? TuningMenu::kLegendLines : lg.n); }
int LegendText(const TuningMenu::Legend& lg) {
    int n = 0;
    for (int i = 0; i < TuningMenu::kLegendText; ++i) n += lg.line[i][0] ? 1 : 0;
    return n;
}
float LegendRefH(const TuningMenu::Legend& lg) {
    return 2.0f * kLgPad + kLgTitleH + kLgGap + kLgLineH * (float)LegendText(lg) + (LegendKeys(lg) ? kLgKeyGap + kLgKeyH : 0.0f);
}
// device px of `t` in font f (0 = empty / GDI failed)
LONG TextW(HDC dc, HFONT f, const wchar_t* t) {
    if (!t || !t[0] || !f) return 0;
    SelectObject(dc, f);
    SIZE z{};
    return GetTextExtentPoint32W(dc, t, (int)wcsnlen(t, 256), &z) ? z.cx : 0;
}
// the widths at scale s (device px): each key word (wordPx[], for the row's layout) and the whole box (reference px, rounded up)
bool LegendMeasure(const TuningMenu::Legend& lg, float s, const LegendFonts& f, float* wordPx, float* refW) {
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;
    HGDIOBJ old = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    LONG other = TextW(dc, f.title, lg.title);
    for (int i = 0; i < TuningMenu::kLegendText; ++i) other = (std::max)(other, TextW(dc, f.text, lg.line[i]));
    float row = 0.0f;
    for (int i = 0; i < LegendKeys(lg); ++i) {
        wordPx[i] = (float)TextW(dc, f.word, lg.word[i]);
        row += (kLgSw + kLgSwGap) * s + wordPx[i] + (i + 1 < LegendKeys(lg) ? kLgKeySep * s : 0.0f);
    }
    SelectObject(dc, old);
    DeleteDC(dc);
    *refW = std::ceil(((std::max)(row, (float)other) + 2.0f * s) / s + 2.0f * kLgPad);   // (+2 px: the last glyph's overhang)
    return true;
}
// GDI -> out (RGBA, w x h): the colour pass on the background, the coverage pass (text + swatches white on black) -> alpha
bool RenderLegend(const TuningMenu::Legend& lg, float s, uint32_t w, uint32_t h, const float* wordPx, std::vector<uint32_t>& out) {
    static std::atomic<bool> s_failLogged{ false };
    bool failLogged = s_failLogged.load(std::memory_order_relaxed);
    LegendFonts f(s);
    if (!f.ok()) return false;
    auto px = [&](float v) { return (LONG)std::lround(v * s); };
    const COLORREF bg = RGB(16, 17, 20), white = RGB(240, 240, 240);
    std::vector<uint32_t> maskPx;
    auto paint = [&](bool mask, std::vector<uint32_t>& dst) -> bool {
        Dib d;
        if (!DibBegin(d, w, h, "census legend", failLogged)) return false;
        HDC dc = d.dc;
        if (!mask) {
            HBRUSH b = CreateSolidBrush(bg);
            RECT all{ 0, 0, (LONG)w, (LONG)h };
            if (b) { FillRect(dc, &all, b); DeleteObject(b); }
        }
        HGDIOBJ oldFont = SelectObject(dc, f.text);
        SetBkMode(dc, TRANSPARENT);
        const UINT fl = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX;
        auto text = [&](HFONT fo, COLORREF c, const wchar_t* t, LONG x0, float y0, float y1) {
            if (!t || !t[0]) return;
            SelectObject(dc, fo);
            SetTextColor(dc, mask ? RGB(255, 255, 255) : c);
            RECT r{ x0, px(y0), (LONG)w, px(y1) };
            DrawTextW(dc, t, -1, &r, fl);
        };
        const LONG x0 = px(kLgPad);
        float y = kLgPad;
        text(f.title, white, lg.title, x0, y, y + kLgTitleH);
        y += kLgTitleH + kLgGap;
        for (int i = 0; i < TuningMenu::kLegendText; ++i) {
            if (!lg.line[i][0]) continue;
            text(f.text, white, lg.line[i], x0, y, y + kLgLineH);
            y += kLgLineH;
        }
        if (LegendKeys(lg)) {
            y += kLgKeyGap;
            float x = (float)x0;
            for (int i = 0; i < LegendKeys(lg); ++i) {
                const COLORREF c = (COLORREF)(lg.rgb[i] & 0xFFFFFFu);
                const float sy = y + 0.5f * (kLgKeyH - kLgSw);
                RECT sw{ (LONG)std::lround(x), px(sy), (LONG)std::lround(x) + px(kLgSw), px(sy) + px(kLgSw) };
                HBRUSH b = CreateSolidBrush(mask ? RGB(255, 255, 255) : c);
                if (b) { FillRect(dc, &sw, b); DeleteObject(b); }
                x += (kLgSw + kLgSwGap) * s;
                text(f.word, c, lg.word[i], (LONG)std::lround(x), y, y + kLgKeyH);
                x += wordPx[i] + kLgKeySep * s;
            }
        }
        SelectObject(dc, oldFont);
        DibFinish(d, w, h, dst);
        return true;
    };
    const bool ok = paint(false, out) && paint(true, maskPx) && maskPx.size() == out.size();
    if (failLogged) s_failLogged.store(true, std::memory_order_relaxed);
    if (ok)
        for (size_t i = 0, e = out.size(); i < e; ++i) {
            const uint32_t m = maskPx[i] & 0xFFu;
            out[i] = (out[i] & 0x00FFFFFFu) | ((kLgBgA + (255u - kLgBgA) * m / 255u) << 24);
        }
    return ok;
}
struct LegendSlot {
    ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; uint32_t texW = 0, texH = 0;
    float build = 0.0f; bool have = false; TuningMenu::Legend last{}; std::vector<uint32_t> px;
    const ID3D11Device* dev = nullptr;               // the device the texture belongs to (a new one: rebuilt)
};
LegendSlot g_legendSlot[TuningMenu::kStatusSlots];
}  // namespace

bool LegendRefSize(const Legend& lg, float* w, float* h) {
    constexpr float s = 2.0f;
    LegendFonts f(s);
    float col[TuningMenu::kLegendLines] = {}, rw = 0.0f;
    if (!f.ok() || !LegendMeasure(lg, s, f, col, &rw)) return false;
    if (w) *w = rw;
    if (h) *h = LegendRefH(lg);
    return true;
}

bool DrawLegend(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t rtW, uint32_t rtH, float x, float y, float ax,
                float ay, float scale, float buildScale, float maxW, float maxH, int outMode, const Legend& lg, int slot,
                float* drawnH) {
    if (!ctx || !rtv || !rtW || !rtH || !(scale > 0.0f) || !(buildScale > 0.0f) || slot < 0 || slot >= kStatusSlots) return false;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev || !Ensure(dev.Get())) return false;
    LegendSlot& c = g_legendSlot[slot];
    if (!c.tex || !c.have || c.dev != dev.Get() || std::fabs(c.build - buildScale) > 0.001f || memcmp(&c.last, &lg, sizeof(lg))) {
        if (c.dev != dev.Get()) { c.tex.Reset(); c.srv.Reset(); c.texW = c.texH = 0; }
        LegendFonts f(buildScale);
        float col[TuningMenu::kLegendLines] = {}, rw = 0.0f;
        if (!f.ok() || !LegendMeasure(lg, buildScale, f, col, &rw)) return false;
        const uint32_t w = (uint32_t)std::lround(rw * buildScale), h = (uint32_t)std::lround(LegendRefH(lg) * buildScale);
        if (w < 8 || h < 8 || w > 8192 || h > 4096) return false;
        if (!RenderLegend(lg, buildScale, w, h, col, c.px) ||
            !UploadTexture(dev.Get(), ctx, c.tex, c.srv, c.texW, c.texH, w, h, c.px, "census legend")) {
            c.tex.Reset(); c.srv.Reset(); c.texW = c.texH = 0; c.have = false;   // (the next call tries again)
            return false;
        }
        c.build = buildScale;
        c.last = lg;
        c.have = true;
        c.dev = dev.Get();
    }
    float k = scale / c.build;
    float dw = (float)c.texW * k, dh = (float)c.texH * k;
    if (maxW > 0.0f && dw > maxW) { const float r = maxW / dw; k *= r; dw *= r; dh *= r; }
    if (maxH > 0.0f && dh > maxH) { const float r = maxH / dh; k *= r; dw *= r; dh *= r; }
    float x0 = x - ax * dw, y0 = y - ay * dh;
    if (x0 > (float)rtW - dw) x0 = (float)rtW - dw;      // kept inside the target
    if (y0 > (float)rtH - dh) y0 = (float)rtH - dh;
    if (x0 < 0.0f) x0 = 0.0f;
    if (y0 < 0.0f) y0 = 0.0f;
    DrawQuad(ctx, rtv, rtW, rtH, x0, y0, dw, dh, outMode, c.srv.Get(), true, 1.0f);
    if (drawnH) *drawnH = dh;
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

void SetWidgetGraph(const WidgetGraph& g) {                  // phase 88
    AcquireSRWLockExclusive(&g_wGraphLock);
    g_wGraph = g;
    ReleaseSRWLockExclusive(&g_wGraphLock);
}

// phase 115: the bug icon box (flat; the VR eyes when the fps box is off) -- own cache per slot (kStatusEyes / kStatusFlat), rebuilt
// when a line or the build scale changes (the count changes at most twice a second).
namespace {
struct BugSlot {
    ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; uint32_t texW = 0, texH = 0;
    float build = 0.0f; wchar_t last1[32] = {}, last2[32] = {}, last3[32] = {}; std::vector<uint32_t> px;
};
BugSlot g_bug[TuningMenu::kStatusSlots];
}
bool DrawBug(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t rtW, uint32_t rtH, float x0, float y0,
             float scale, float buildScale, int outMode, const wchar_t* line1, const wchar_t* line2, int slot, const wchar_t* line3) {
    if (!ctx || !rtv || !rtW || !rtH || !(scale > 0.0f) || !(buildScale > 0.0f) || slot < 0 || slot >= kStatusSlots) return false;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev || !Ensure(dev.Get())) return false;
    BugSlot& c = g_bug[slot];
    wchar_t l1[32], l2[32], l3[32];
    wcsncpy_s(l1, line1 ? line1 : L"", _TRUNCATE);
    wcsncpy_s(l2, line2 ? line2 : L"", _TRUNCATE);
    wcsncpy_s(l3, line3 ? line3 : L"", _TRUNCATE);
    if (!c.tex || std::fabs(c.build - buildScale) > 0.001f || wcscmp(l1, c.last1) || wcscmp(l2, c.last2) || wcscmp(l3, c.last3)) {
        const uint32_t w = (uint32_t)std::lround(kBugRefW * buildScale), h = (uint32_t)std::lround(kBugRefH * buildScale);
        if (w < 8 || h < 8 || w > 8192 || h > 4096) return false;
        if (!RenderBug(l1, l2, buildScale, w, h, c.px, l3) || !UploadTexture(dev.Get(), ctx, c.tex, c.srv, c.texW, c.texH, w, h, c.px, "bug icon")) {
            c.tex.Reset(); c.srv.Reset(); c.texW = c.texH = 0;
            return false;
        }
        c.build = buildScale;
        wcscpy_s(c.last1, l1); wcscpy_s(c.last2, l2); wcscpy_s(c.last3, l3);
    }
    const float k = scale / c.build;
    DrawQuad(ctx, rtv, rtW, rtH, x0, y0, (float)c.texW * k, (float)c.texH * k, outMode, c.srv.Get(), true, 1.0f);
    return true;
}
bool RenderBugPng(const wchar_t* line1, const wchar_t* line2, float scale, bool inWidget, std::vector<uint32_t>& out, uint32_t* w,
                  uint32_t* h, const wchar_t* line3) {
    *w = (uint32_t)std::lround((inWidget ? kWidgetRefW : kBugRefW) * scale);
    *h = (uint32_t)std::lround((inWidget ? kWidgetBugH : kBugRefH) * scale);
    return *w >= 8 && *h >= 8 && RenderBug(line1, line2, scale, *w, *h, out, line3);
}
void SetWidgetBug(const wchar_t* line1, const wchar_t* line2, const wchar_t* line3) {
    AcquireSRWLockExclusive(&g_wGraphLock);
    wcsncpy_s(g_wBug1, line1 ? line1 : L"", _TRUNCATE);
    wcsncpy_s(g_wBug2, line2 ? line2 : L"", _TRUNCATE);
    wcsncpy_s(g_wBug3, line3 ? line3 : L"", _TRUNCATE);
    ReleaseSRWLockExclusive(&g_wGraphLock);
}
bool WidgetBugOn() {
    AcquireSRWLockShared(&g_wGraphLock);
    const bool on = g_wBug1[0] || g_wBug2[0];
    ReleaseSRWLockShared(&g_wGraphLock);
    return on;
}

float WidgetTextRefH(const wchar_t* line1, const wchar_t* line2, bool graphOn) {
    if (!graphOn) return kWidgetRefH;                        // (no graph: the box keeps its size, one line centred)
    const int n = (line1 && line1[0] ? 1 : 0) + (line2 && line2[0] ? 1 : 0);
    return n >= 2 ? kWidgetText2H : (n == 1 ? kWidgetText1H : 0.0f);
}

float WidgetRefH(const wchar_t* line1, const wchar_t* line2) {
    AcquireSRWLockShared(&g_wGraphLock);
    const bool on = g_wGraph.on;
    const bool bug = g_wBug1[0] || g_wBug2[0];               // phase 115: the bug icon strip (VR)
    ReleaseSRWLockShared(&g_wGraphLock);
    return WidgetTextRefH(line1, line2, on) + (on ? kWidgetGraphH : 0.0f) + (bug ? kWidgetBugH : 0.0f);
}

void ReleaseWidget() {
    DropWidget();
    g_wPixels.clear();
    g_wPixels.shrink_to_fit();
}

}  // namespace TuningMenu

#endif // WITH_DLAA
