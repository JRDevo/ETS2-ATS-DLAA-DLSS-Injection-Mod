// phase 97 unit test: the new SCS water's checkerboard under the jitter (src/water_ps.h, inject.cpp WaterOnDrawBegin / End,
// dlaa.ini water_overscan_fix).
//
// 1. WaterPs::Classify on fxc-compiled look-alikes of the game's three water shaders (captures/ets2_pond_water_audit.md: the
//    G-buffer checker writer c369f136, the forward reader 185d5b7a, the material-flag draw 4b5eb7ed) -> the structural rules match;
//    and on look-alikes of the shader shapes that must NOT match (screen-space UV fetches without a parity, an SV_Position checker,
//    a loaded G-buffer flag bit, a constant float colour, a vertex shader, truncated / garbage blobs) -> none. A known checksum
//    patched into a non-matching blob -> the hash fallback.
// 2. WARP: the checker maths itself. A perspective quad whose PS computes the game's cell (interpolant.yz / interpolant.x * W x H,
//    ftou) and writes it per pixel:
//      - W x H viewport shifted by each of the 8 Halton phases: the cell is the rasterised pixel everywhere (0 mismatches)
//      - + the edge re-draw (LightFs::EdgeRects, unshifted viewport + scissor) for negative shifts: every pixel written, cell = pixel
//      - MUTATION: the phase-23 (W+1) x (H+1) viewport: the parity is wrong in the predicted rectangles (the issue #30 blocks)
// Optional: water_ps_test.exe <folder> classifies every *.dxbc in the folder and prints the matches (false-positive scan of
// captured game shaders); water_ps_test.exe --disasm prints the look-alikes' disassembly. Exit code 0 = all checks passed.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "light_fs.h"
#include "water_ps.h"

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// ---- shader look-alikes (the game's disassembly, rewritten as HLSL) -------------------------------------------------------------
static const char* kShaders = R"(
cbuffer C : register(b0) { float4 c[28]; };
Texture2D t1 : register(t1);
Texture2D t2 : register(t2);
Texture2D<float4> g0 : register(t12);
Texture2D<float4> g1 : register(t13);
Texture2D<uint4>  g3 : register(t15);
Texture2D<float>  dep : register(t3);
SamplerState s0 : register(s0);

struct GbOut { float4 o0 : SV_Target0; float4 o1 : SV_Target1; float4 o2 : SV_Target2; uint4 o3 : SV_Target3; };

// c369f136: pix = v5.yz / v5.x * cb0[17].xy; discard on (ftou(pix.x) + ftou(pix.y) + ftou(cb0[20].x)) & 1
GbOut Writer(float4 pos : SV_Position, float4 uv : TEXCOORD4, float3 v5 : TEXCOORD5) {
    float2 pix = v5.yz / v5.x * c[17].xy;
    uint par = (uint(pix.x) + uint(pix.y) + uint(c[20].x)) & 1;
    if (par == 0) discard;
    GbOut o;
    o.o0 = float4(normalize(t1.Sample(s0, uv.xy).xyz + t2.Sample(s0, uv.zw).xyz), v5.x);
    o.o1 = 0; o.o2 = 0; o.o3 = 0;
    return o;
}

// 185d5b7a: the same parity from v7, then ld of the water cell (own pixel or +1 in x) and the bed cell by that parity
float4 Reader(float4 pos : SV_Position, float3 v7 : TEXCOORD7, float3 n : TEXCOORD4) : SV_Target {
    float2 sp = v7.yz / v7.x;
    float2 pix = sp * c[23].xy * (c[24].xy / c[24].zw);
    uint par = (uint(pix.x) + uint(pix.y) + uint(c[27].x)) & 1;
    float2 offW = par ? float2(0, 0) : float2(c[27].y, 0);
    float2 offB = par ? float2(c[27].y, 0) : float2(0, 0);
    int2 cw = int2((sp * c[23].xy + offW) * (c[24].xy / c[24].zw));
    int2 cb = int2((sp * c[23].xy + offB) * (c[24].xy / c[24].zw));
    float4 water = g0.Load(int3(cw, 0));
    float bed = g0.Load(int3(cb, 0)).w;
    uint flag = g3.Load(int3(cw, 0)).z;
    float a = (flag & 16) ? saturate(bed - water.w) : 0;
    return float4(lerp(n, water.xyz, a), 1);
}

// 4b5eb7ed: no inputs, RT3 = the water material id
GbOut Flag() {
    GbOut o;
    o.o0 = 0; o.o1 = 0; o.o2 = 0; o.o3 = uint4(0, 0, 3, 8064);
    return o;
}

// ---- must NOT match ----
// soft particle: projected UV -> ftoi -> ld of the depth, no parity
float4 SoftParticle(float4 pos : SV_Position, float3 v1 : TEXCOORD1, float4 col : COLOR0) : SV_Target {
    int2 p = int2(v1.yz / v1.x * c[17].xy);
    float d = dep.Load(int3(p, 0));
    return col * saturate((d - v1.x) * c[3].x);
}
// screen-space decal: projected UV sampled (no integer pixel, no parity)
float4 Decal(float4 pos : SV_Position, float3 v1 : TEXCOORD1) : SV_Target {
    float2 uv = v1.yz / v1.x;
    return t1.Sample(s0, uv * c[17].zw);
}
// the usual dither checker: SV_Position (correct under any viewport) -- no perspective divide of an interpolant
float4 PosChecker(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    uint par = (uint(pos.x) + uint(pos.y)) & 1;
    if (par != 0) discard;
    return t1.Sample(s0, uv);
}
// a G-buffer flag bit loaded at the projected pixel, then & 1 + discard (the loaded value is data, not the pixel's parity)
float4 LoadedBit(float4 pos : SV_Position, float3 v1 : TEXCOORD1) : SV_Target {
    int2 p = int2(v1.yz / v1.x * c[17].xy);
    uint f = g3.Load(int3(p, 0)).w;
    if ((f & 1) != 0) discard;
    return float4(1, 0, 0, 1);
}
// constant float colour (a debug / mask PS)
float4 ConstColour() : SV_Target { return float4(1, 0.5, 0, 1); }
// a vertex shader
float4 SomeVs(float4 p : POSITION) : SV_Position { return mul(p, float4x4(c[0], c[1], c[2], c[3])); }
)";

static ID3DBlob* CompileSrc(const char* src, const char* entry, const char* target) {
    ID3DBlob* b = nullptr;
    ID3DBlob* e = nullptr;
    if (FAILED(D3DCompile(src, std::strlen(src), "water_ps_test", nullptr, nullptr, entry, target, 0, 0, &b, &e))) {
        std::printf("compile %s: %s\n", entry, e ? (const char*)e->GetBufferPointer() : "?");
        std::exit(2);
    }
    if (e) e->Release();
    return b;
}

static std::string Disasm(ID3DBlob* b) {
    ID3DBlob* d = nullptr;
    std::string s;
    if (SUCCEEDED(D3DDisassemble(b->GetBufferPointer(), b->GetBufferSize(), 0, nullptr, &d)) && d) {
        s.assign((const char*)d->GetBufferPointer(), d->GetBufferSize());
        d->Release();
    }
    return s;
}

static void ClassifyCases() {
    struct Case { const char* entry; const char* target; int want; };
    const Case cases[] = {
        { "Writer", "ps_5_0", WaterPs::kRuleCheckerWrite },
        { "Reader", "ps_5_0", WaterPs::kRuleCheckerRead },
        { "Flag", "ps_5_0", WaterPs::kRuleFlag },
        { "SoftParticle", "ps_5_0", WaterPs::kRuleNone },
        { "Decal", "ps_5_0", WaterPs::kRuleNone },
        { "PosChecker", "ps_5_0", WaterPs::kRuleNone },
        { "LoadedBit", "ps_5_0", WaterPs::kRuleNone },
        { "ConstColour", "ps_5_0", WaterPs::kRuleNone },
        { "SomeVs", "vs_5_0", WaterPs::kRuleNone },
        { "Writer", "ps_4_0", WaterPs::kRuleCheckerWrite },   // SHDR (SM4) as well as SHEX
    };
    ID3DBlob* soft = nullptr;
    for (const Case& k : cases) {
        ID3DBlob* b = CompileSrc(kShaders, k.entry, k.target);
        const WaterPs::Result r = WaterPs::Classify(b->GetBufferPointer(), b->GetBufferSize());
        CHECK(r.rule == k.want, "%s (%s): rule %d (%s), want %d (%s)\n%s", k.entry, k.target, r.rule, WaterPs::RuleName(r.rule),
              k.want, WaterPs::RuleName(k.want), Disasm(b).c_str());
        CHECK(r.parsed == (k.target[0] == 'p'), "%s: parsed %d", k.entry, (int)r.parsed);
        CHECK(r.known < 0, "%s: a fresh compile is not one of the game's checksums", k.entry);
        std::printf("  %-13s %-6s -> %s\n", k.entry, k.target, WaterPs::RuleName(r.rule));
        if (!std::strcmp(k.entry, "SoftParticle")) { soft = b; soft->AddRef(); }
        b->Release();
    }
    // hash fallback: a known checksum on a blob the structural rules reject
    if (soft) {
        std::vector<uint8_t> v((const uint8_t*)soft->GetBufferPointer(), (const uint8_t*)soft->GetBufferPointer() + soft->GetBufferSize());
        for (int k = 0; k < 3; ++k) {
            std::memcpy(v.data() + 4, WaterPs::kKnown[k], 16);
            const WaterPs::Result r = WaterPs::Classify(v.data(), v.size());
            CHECK(r.rule == WaterPs::kRuleHash && r.known == k, "hash fallback %d: rule %d known %d", k, r.rule, r.known);
        }
        char t[40];
        WaterPs::SumText(WaterPs::kKnown[0], t, sizeof(t));
        CHECK(!std::strcmp(t, "c369f136-109f6918-1886484d-8c1e6ede"), "SumText: %s", t);
        // truncations of a valid blob never crash and never match the checker rules
        ID3DBlob* w = CompileSrc(kShaders, "Writer", "ps_5_0");
        std::vector<uint8_t> wv((const uint8_t*)w->GetBufferPointer(), (const uint8_t*)w->GetBufferPointer() + w->GetBufferSize());
        size_t shexEnd = wv.size();                      // the code chunk's end (later chunks, e.g. STAT, are never read)
        for (uint32_t c = 0, nc = WaterPs::Rd(wv.data(), 28); c < nc; ++c) {
            const uint32_t off = WaterPs::Rd(wv.data(), 32 + 4 * (size_t)c), fcc = WaterPs::Rd(wv.data(), off);
            if (fcc == 0x58454853u || fcc == 0x52444853u) shexEnd = off + 8 + (size_t)WaterPs::Rd(wv.data(), off + 4);
        }
        for (size_t cut = 0; cut < wv.size(); cut += 7) {
            const WaterPs::Result r = WaterPs::Classify(wv.data(), cut);
            CHECK(r.rule == WaterPs::kRuleNone || cut >= shexEnd, "truncated at %zu (code ends at %zu): rule %d", cut, shexEnd, r.rule);
        }
        // garbage in the code tokens: no crash (any result)
        srand(97);
        for (int it = 0; it < 2000; ++it) {
            std::vector<uint8_t> g = wv;
            for (int k = 0; k < 8; ++k) g[32 + (size_t)rand() % (g.size() - 32)] = (uint8_t)rand();
            (void)WaterPs::Classify(g.data(), g.size());
        }
        w->Release();
        soft->Release();
    }
    CHECK(WaterPs::Classify(nullptr, 0).rule == WaterPs::kRuleNone, "null");
}

// ---- WARP: the checker maths --------------------------------------------------------------------------------------------------
static const char* kGeo = R"(
cbuffer C : register(b0) { float2 size; float2 pad; };
struct V { float4 pos : SV_Position; float3 sp : TEXCOORD5; };
// a perspective quad larger than the screen (w 1 at the top, 3 at the bottom); sp = (w, (x * 0.5 + 0.5 w), (-y * 0.5 + 0.5 w)) = the
// game's v5 (sp.yz / sp.x = the [0, 1] position across the viewport, y down)
V VS(uint id : SV_VertexID) {
    float2 q = float2(id & 1, id >> 1);                  // (0,0) (1,0) (0,1) (1,1), triangle strip
    float w = lerp(1.0, 3.0, q.y);
    float2 ndc = lerp(float2(-1.3, 1.3), float2(1.3, -1.3), q);
    V o;
    o.pos = float4(ndc * w, 0.5 * w, w);
    o.sp = float3(w, o.pos.x * 0.5 + 0.5 * w, -o.pos.y * 0.5 + 0.5 * w);
    return o;
}
uint PS(V i) : SV_Target {
    uint2 cell = uint2(i.sp.yz / i.sp.x * size);         // the water shaders' cell (ftou)
    return cell.x | (cell.y << 16);
}
)";

struct Gpu {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    ID3D11RasterizerState* rsTwin = nullptr;
    ID3D11Texture2D* rt = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* st = nullptr;
};
constexpr UINT kW = 160, kH = 120;

static bool Init(Gpu& g) {
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx))) return false;
    ID3DBlob* v = CompileSrc(kGeo, "VS", "vs_5_0");
    ID3DBlob* p = CompileSrc(kGeo, "PS", "ps_5_0");
    bool ok = SUCCEEDED(g.dev->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &g.vs)) &&
              SUCCEEDED(g.dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &g.ps));
    v->Release(); p->Release();
    const float sz[4] = { (float)kW, (float)kH, 0, 0 };
    D3D11_BUFFER_DESC bd{ 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA sd{ sz, 0, 0 };
    ok = ok && SUCCEEDED(g.dev->CreateBuffer(&bd, &sd, &g.cb));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(g.dev->CreateRasterizerState(&rd, &g.rs));
    rd.ScissorEnable = TRUE;
    ok = ok && SUCCEEDED(g.dev->CreateRasterizerState(&rd, &g.rsTwin));
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW; td.Height = kH; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32_UINT; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ok = ok && SUCCEEDED(g.dev->CreateTexture2D(&td, nullptr, &g.rt)) && SUCCEEDED(g.dev->CreateRenderTargetView(g.rt, nullptr, &g.rtv));
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ok = ok && SUCCEEDED(g.dev->CreateTexture2D(&td, nullptr, &g.st));
    return ok;
}

// mode 0 = W x H shifted, 1 = + the edge re-draw, 2 = (W+1) x (H+1) shifted (phase 23). Returns the RT0 contents.
static std::vector<uint32_t> Render(Gpu& g, float sx, float sy, int mode) {
    const float clr[4] = { 65535.0f, 0, 0, 0 };          // R32_UINT gets 0x0000FFFF: cell x 65535 = "not written"
    g.ctx->ClearRenderTargetView(g.rtv, clr);
    g.ctx->OMSetRenderTargets(1, &g.rtv, nullptr);
    g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g.ctx->IASetInputLayout(nullptr);
    g.ctx->VSSetShader(g.vs, nullptr, 0);
    g.ctx->PSSetShader(g.ps, nullptr, 0);
    g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
    g.ctx->RSSetState(g.rs);
    const D3D11_VIEWPORT game{ 0, 0, (float)kW, (float)kH, 0, 1 };
    D3D11_VIEWPORT v = game;
    v.TopLeftX += sx; v.TopLeftY += sy;
    if (mode == 2) { v.Width += 1.0f; v.Height += 1.0f; }
    g.ctx->RSSetViewports(1, &v);
    g.ctx->Draw(4, 0);
    if (mode == 1) {                                     // inject.cpp WaterOnDrawEnd: the lost far column / row, unshifted
        D3D11_RECT r[2];
        const int nr = LightFs::EdgeRects(game, sx, sy, r);
        if (nr) {
            g.ctx->RSSetViewports(1, &game);
            g.ctx->RSSetState(g.rsTwin);
            for (int i = 0; i < nr; ++i) { g.ctx->RSSetScissorRects(1, &r[i]); g.ctx->Draw(4, 0); }
        }
    }
    g.ctx->CopyResource(g.st, g.rt);
    std::vector<uint32_t> out(kW * kH);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(g.ctx->Map(g.st, 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < kH; ++y) std::memcpy(&out[y * kW], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, kW * 4);
        g.ctx->Unmap(g.st, 0);
    }
    return out;
}

static float Halton(int i, int b) {
    float f = 1.0f, r = 0.0f;
    while (i > 0) { f /= (float)b; r += f * (float)(i % b); i /= b; }
    return r;
}

static void WarpCases() {
    Gpu g;
    if (!Init(g)) { std::printf("WARP device / state creation failed\n"); ++g_fail; return; }
    auto written = [](uint32_t v) { return (v & 0xFFFFu) < kW && (v >> 16) < kH; };
    for (int i = 1; i <= 8; ++i) {
        const float sx = Halton(i, 2) - 0.5f, sy = Halton(i, 3) - 0.5f;
        // W x H shifted (the phase-97 water viewport): cell = pixel wherever the shifted viewport rasterises
        {
            const std::vector<uint32_t> o = Render(g, sx, sy, 0);
            int bad = 0, holes = 0;
            for (UINT y = 0; y < kH; ++y)
                for (UINT x = 0; x < kW; ++x) {
                    const uint32_t v = o[y * kW + x];
                    if (!written(v)) { ++holes; continue; }
                    if ((v & 0xFFFFu) != x || (v >> 16) != y) ++bad;
                }
            const int lost = (sx < 0 ? (int)kH : 0) + (sy < 0 ? (int)kW : 0) - (sx < 0 && sy < 0 ? 1 : 0);
            CHECK(bad == 0, "phase %d (%+.4f,%+.4f) W x H: %d pixels whose cell is not the pixel", i, sx, sy, bad);
            CHECK(holes == lost, "phase %d W x H: %d unwritten pixels, the edge rule predicts %d", i, holes, lost);
        }
        // + the edge re-draw: every pixel written, cell = pixel
        {
            const std::vector<uint32_t> o = Render(g, sx, sy, 1);
            int bad = 0, holes = 0;
            for (UINT y = 0; y < kH; ++y)
                for (UINT x = 0; x < kW; ++x) {
                    const uint32_t v = o[y * kW + x];
                    if (!written(v)) ++holes;
                    else if ((v & 0xFFFFu) != x || (v >> 16) != y) ++bad;
                }
            CHECK(bad == 0 && holes == 0, "phase %d + edge re-draw: %d wrong cells, %d holes", i, bad, holes);
        }
        // MUTATION: the overscan viewport -- the parity is wrong where exactly one axis is past (0.5 - s) * size
        {
            const std::vector<uint32_t> o = Render(g, sx, sy, 2);
            int badPar = 0, predicted = 0, disagree = 0;
            for (UINT y = 0; y < kH; ++y)
                for (UINT x = 0; x < kW; ++x) {
                    const uint32_t v = o[y * kW + x];
                    if (!written(v)) continue;
                    const uint32_t cx = v & 0xFFFFu, cy = v >> 16;
                    if (((cx + cy) & 1u) != ((x + y) & 1u)) ++badPar;
                    const int px = (int)std::floor(((double)x + 0.5 - sx) * kW / (kW + 1.0));
                    const int py = (int)std::floor(((double)y + 0.5 - sy) * kH / (kH + 1.0));
                    if (((px + py) & 1) != (int)((x + y) & 1u)) ++predicted;
                    if (px != (int)cx || py != (int)cy) ++disagree;
                }
            // the model is exact up to the rasterizer's 1/256 px snapping and fp32 interpolation: pixels within a hair of a cell
            // boundary may land either side (at most a few rows / columns)
            CHECK(std::abs(badPar - predicted) <= (int)(kW + kH) && badPar > (int)(kW * kH) / 5 && disagree <= 2 * (int)(kW + kH),
                  "phase %d (%+.4f,%+.4f) (W+1) x (H+1): %d wrong-parity pixels, the model predicts %d (cells off the model: %d)",
                  i, sx, sy, badPar, predicted, disagree);
            std::printf("  phase %d shift (%+.4f,%+.4f): overscan -> %5.1f %% of the pixels read the wrong checker cell; W x H -> 0\n",
                        i, sx, sy, 100.0 * badPar / (kW * kH));
        }
    }
    if (g.st) g.st->Release();
    if (g.rtv) g.rtv->Release();
    if (g.rt) g.rt->Release();
    if (g.rsTwin) g.rsTwin->Release();
    if (g.rs) g.rs->Release();
    if (g.cb) g.cb->Release();
    if (g.ps) g.ps->Release();
    if (g.vs) g.vs->Release();
    if (g.ctx) g.ctx->Release();
    if (g.dev) g.dev->Release();
}

static int ScanFolder(const char* dir) {
    std::string pat = std::string(dir) + "\\*.dxbc";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { std::printf("no *.dxbc in %s\n", dir); return 2; }
    int n = 0, ps = 0, hit = 0;
    do {
        const std::string p = std::string(dir) + "\\" + fd.cFileName;
        FILE* f = nullptr;
        if (fopen_s(&f, p.c_str(), "rb") || !f) continue;
        std::vector<uint8_t> v;
        uint8_t buf[65536];
        size_t k;
        while ((k = fread(buf, 1, sizeof(buf), f)) > 0) v.insert(v.end(), buf, buf + k);
        fclose(f);
        ++n;
        const WaterPs::Result r = WaterPs::Classify(v.data(), v.size());
        if (r.parsed) ++ps;
        if (r.rule) {
            ++hit;
            char t[40];
            WaterPs::SumText(r.sum, t, sizeof(t));
            std::printf("MATCH %s: %s (%s)%s\n", fd.cFileName, WaterPs::RuleName(r.rule), t, r.known >= 0 ? " known" : "");
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::printf("%d files, %d pixel shaders parsed, %d matched\n", n, ps, hit);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && !std::strcmp(argv[1], "--disasm")) {   // the look-alikes' disassembly (compare with the game's, see the audit)
        for (const char* e : { "Writer", "Reader", "Flag" }) {
            ID3DBlob* b = CompileSrc(kShaders, e, "ps_5_0");
            std::printf("---- %s\n%s\n", e, Disasm(b).c_str());
            b->Release();
        }
        return 0;
    }
    if (argc > 1) return ScanFolder(argv[1]);
    std::printf("classifier:\n");
    ClassifyCases();
    std::printf("WARP checker maths (%ux%u):\n", kW, kH);
    WarpCases();
    if (g_fail) { std::printf("%d check(s) FAILED\n", g_fail); return 1; }
    std::printf("water_ps_test: all checks passed\n");
    return 0;
}
