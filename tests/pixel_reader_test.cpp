// phase 109a unit test: the forward pixel readers' twin (src/pixel_reader.h, inject.cpp PrOnDraw, dlaa.ini pixel_reader_fix).
//
// 1. The DXBC checksum: every fxc blob of this test and (locally) every game blob under the capture folders verifies; one changed
//    byte does not.
// 2. PixelReader::Make on fxc look-alikes: the audited index chain (u = v.yz / v.x * cbA.xy * cbB.xy / cbB.zw, ftoi, ld) -> twin;
//    the same read from SV_Position, a plain shader -> none; the phase-97 parity reader -> parity; an index with an offset added,
//    an index used in arithmetic -> unproven shape (no twin).
// 3. WARP, per twin (look-alike in CI; every game pixel-reader blob found locally, the 10 audited ones must give a twin):
//    a. the twin is accepted by CreatePixelShader;
//    b. BIT-EXACT: game shader vs twin at the game's W x H viewport shifted by the 8 Halton phases (+ a perspective quad and
//       varying interpolants): 0 differing words;
//    c. THE FIX: with every other interpolant constant (full-screen triangle), the twin under the phase-23 (W+1) x (H+1) viewport
//       = the game shader at W x H (every pixel both draw); MUTATION: the game shader under the overscan differs (the bug).
// pixel_reader_test.exe <folder> prints Make's verdict for every *.dxbc in the folder. Exit code 0 = all checks passed.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "pixel_reader.h"

static int g_fail = 0, g_checks = 0, g_mutHits = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::vector<uint8_t> Compile(const char* src, const char* entry, const char* target) {
    ID3DBlob* b = nullptr; ID3DBlob* e = nullptr;
    const HRESULT hr = D3DCompile(src, strlen(src), "pr", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &e);
    if (FAILED(hr)) { std::printf("compile %s failed: %s\n", entry, e ? (const char*)e->GetBufferPointer() : "?"); if (e) e->Release(); return {}; }
    if (e) e->Release();
    std::vector<uint8_t> v((const uint8_t*)b->GetBufferPointer(), (const uint8_t*)b->GetBufferPointer() + b->GetBufferSize());
    b->Release();
    return v;
}
static std::vector<uint8_t> Load(const std::string& p) {
    std::vector<uint8_t> v;
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "rb") || !f) return v;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0) { v.resize((size_t)n); if (fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear(); }
    fclose(f);
    return v;
}
static std::string Disasm(const std::vector<uint8_t>& c) {
    ID3DBlob* d = nullptr;
    if (FAILED(D3DDisassemble(c.data(), c.size(), 0, nullptr, &d)) || !d) return "";
    std::string s((const char*)d->GetBufferPointer(), d->GetBufferSize());
    d->Release();
    return s;
}

// ---- look-alikes ---------------------------------------------------------------------------------------------------------------
static const char* kLook = R"(
cbuffer C : register(b0) { float4 c[8]; };
Texture2D<float4> t10 : register(t10);
Texture2D<float4> t11 : register(t11);
Texture2D<float4> t6 : register(t6);
SamplerState s0 : register(s0);
struct I { float4 col : COLOR0; float4 pos : SV_Position; float4 t0 : TEXCOORD0; float2 uv : TEXCOORD4; float3 t6v : TEXCOORD6; };
// the audited chain (0a3c1386 / a5aa1c2d / 1a104d6e ...): u = v.yz / v.x; pix = ftoi(u * cb0[4].xy * (1 / cb0[5].zw) * cb0[5].xy)
float4 Reader(I i) : SV_Target {
    float2 u = i.t6v.yz / i.t6v.x;
    float2 k = (1.0 / c[5].zw) * c[5].xy;
    int2 p = int2(k * (u * c[4].xy));
    float3 a = t10.Load(int3(p, 0)).xyz;
    float3 b = t11.Load(int3(p, 0)).xyz;
    float4 tex = t6.Sample(s0, i.uv);
    return float4(a * tex.xyz * i.col.xyz + b * i.t0.xyz, i.col.w * tex.w);
}
// the same read from SV_Position: nothing to fix
float4 PosReader(I i) : SV_Target {
    int2 p = int2(i.pos.xy);
    return t10.Load(int3(p, 0)) + i.col;
}
// the phase-97 parity reader
float4 Parity(I i) : SV_Target {
    float2 u = i.t6v.yz / i.t6v.x;
    float2 pix = u * c[4].xy * ((1.0 / c[5].zw) * c[5].xy);
    uint par = (uint(pix.x) + uint(pix.y)) & 1;
    int2 p = int2(pix) + int2(par, 0);
    return t10.Load(int3(p, 0));
}
// an offset before the conversion: not the proven shape
float4 Offset(I i) : SV_Target {
    float2 u = i.t6v.yz / i.t6v.x;
    int2 p = int2(u * c[5].xy + c[6].xy);
    return t10.Load(int3(p, 0)) * i.col;
}
// the index used in arithmetic (a neighbour read): not proven
float4 Neighbour(I i) : SV_Target {
    float2 u = i.t6v.yz / i.t6v.x;
    int2 p = int2(u * c[4].xy * ((1.0 / c[5].zw) * c[5].xy));
    return t10.Load(int3(p, 0)) + t10.Load(int3(p + int2(1, 0), 0));
}
// a plain shader
float4 Plain(I i) : SV_Target { return t6.Sample(s0, i.uv) * i.col; }
)";

// ---- WARP --------------------------------------------------------------------------------------------------------------------
struct Gpu {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    bool Init() {
        return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx));
    }
};

struct SigEl { std::string name; uint32_t idx = 0, sv = 0, reg = 0, mask = 0; };
static std::vector<SigEl> Isgn(const std::vector<uint8_t>& b) {
    std::vector<SigEl> out;
    const uint32_t n = WaterPs::Rd(b.data(), 28);
    for (uint32_t c = 0; c < n; ++c) {
        const size_t off = WaterPs::Rd(b.data(), 32 + 4 * (size_t)c);
        if (WaterPs::Rd(b.data(), off) != 0x4E475349u) continue;
        const size_t base = off + 8;
        const uint32_t ne = WaterPs::Rd(b.data(), base);
        for (uint32_t e = 0; e < ne; ++e) {
            const size_t p = base + 8 + 24 * (size_t)e;
            SigEl s;
            s.name = (const char*)&b[base + WaterPs::Rd(b.data(), p)];
            s.idx = WaterPs::Rd(b.data(), p + 4); s.sv = WaterPs::Rd(b.data(), p + 8); s.reg = WaterPs::Rd(b.data(), p + 16);
            s.mask = b[p + 20];
            out.push_back(s);
        }
    }
    return out;
}
static uint32_t DeclaredTex(const std::vector<uint8_t>& b) {   // t-slots of the PS (Census::ScanPs' texDecl)
    return Census::ScanPs(b.data(), b.size()).texDecl;
}

// a VS whose output signature = the PS's input signature (register order, sizes from the masks); pReg gets the game's projected
// position (w, 0.5 (x + w), 0.5 (w - y)); `vary` = the other outputs vary over the screen, else constants; `persp` = a quad with
// a different w per vertex (else a full-screen triangle, w = 1)
static std::string MakeVs(const std::vector<SigEl>& sig, int pReg, bool vary, bool persp) {
    std::string s = "struct O {\n";
    for (size_t i = 0; i < sig.size(); ++i) {
        int n = 0;
        for (int k = 0; k < 4; ++k) if (sig[i].mask & (1u << k)) ++n;
        char l[160];
        snprintf(l, sizeof(l), "  float%d m%zu : %s%u;\n", n, i, sig[i].name.c_str(), sig[i].idx);
        s += l;
    }
    s += "};\nO main(uint id : SV_VertexID) {\n  O o;\n";
    if (persp) {
        s += "  float2 q[6] = { float2(-1.3, -1.3), float2(1.3, -1.3), float2(-1.3, 1.3), float2(-1.3, 1.3), float2(1.3, -1.3), float2(1.3, 1.3) };\n"
             "  float wv[6] = { 1.0, 2.5, 1.7, 1.7, 2.5, 3.1 };\n  float w = wv[id]; float4 cp = float4(q[id] * w, 0.5 * w, w);\n";
    } else {
        s += "  float2 q[3] = { float2(-1, -1), float2(3, -1), float2(-1, 3) };\n  float4 cp = float4(q[id], 0.5, 1.0);\n";
    }
    for (size_t i = 0; i < sig.size(); ++i) {
        int n = 0;
        for (int k = 0; k < 4; ++k) if (sig[i].mask & (1u << k)) ++n;
        char l[256];
        if (sig[i].sv == 1) snprintf(l, sizeof(l), "  o.m%zu = cp;\n", i);
        else if ((int)sig[i].reg == pReg)
            snprintf(l, sizeof(l), "  o.m%zu = (float%d)float4(cp.w, 0.5 * (cp.x + cp.w), 0.5 * (cp.w - cp.y), cp.w);\n", i, n);
        else if (vary)
            snprintf(l, sizeof(l), "  o.m%zu = (float%d)(float4(0.31, 0.47, 0.59, 0.83) + 0.2 * float4(cp.x, cp.y, cp.x * cp.y, cp.x - cp.y) / cp.w + %zu * 0.01);\n", i, n, i);
        else
            snprintf(l, sizeof(l), "  o.m%zu = (float%d)(float4(0.31, 0.47, 0.59, 0.83) + %zu * 0.01);\n", i, n, i);
        s += l;
    }
    s += "  return o;\n}\n";
    return s;
}

struct Run {
    static constexpr UINT W = 61, H = 37;
    Gpu& g;
    ID3D11Texture2D* rt = nullptr; ID3D11RenderTargetView* rtv = nullptr; ID3D11Texture2D* st = nullptr;
    ID3D11ShaderResourceView* srv[32] = {};
    ID3D11Buffer* cb = nullptr;
    ID3D11SamplerState* smp = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    explicit Run(Gpu& gg) : g(gg) {}
    bool Init(uint32_t texMask, int cbA, int cbB) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &rt)) || FAILED(g.dev->CreateRenderTargetView(rt, nullptr, &rtv))) return false;
        d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g.dev->CreateTexture2D(&d, nullptr, &st))) return false;
        for (int s = 0; s < 32; ++s) {
            if (!(texMask & (1u << s))) continue;
            std::vector<float> px((size_t)W * H * 4);
            for (UINT y = 0; y < H; ++y)
                for (UINT x = 0; x < W; ++x) {
                    float* p = &px[((size_t)y * W + x) * 4];
                    p[0] = 0.05f + 0.013f * (float)x + 0.001f * (float)s;   // every texel different: a misread shows
                    p[1] = 0.07f + 0.017f * (float)y;
                    p[2] = 0.3f + 0.011f * (float)((x * 7 + y * 13 + s) % 29);
                    p[3] = 0.5f + 0.01f * (float)((x + y) % 7);
                }
            D3D11_TEXTURE2D_DESC td = d;
            td.Usage = D3D11_USAGE_DEFAULT; td.CPUAccessFlags = 0; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sd{ px.data(), W * 16, 0 };
            ID3D11Texture2D* t = nullptr;
            if (FAILED(g.dev->CreateTexture2D(&td, &sd, &t))) return false;
            g.dev->CreateShaderResourceView(t, nullptr, &srv[s]);
            t->Release();
        }
        std::vector<float> c(4096);
        for (size_t i = 0; i < c.size(); ++i) c[i] = 0.25f + 0.5f * (float)((i * 37) % 17) / 17.0f;
        if (cbA >= 0) { c[cbA * 4 + 0] = 1; c[cbA * 4 + 1] = 1; c[cbA * 4 + 2] = 1; c[cbA * 4 + 3] = 1; }
        if (cbB >= 0) { c[cbB * 4 + 0] = (float)W; c[cbB * 4 + 1] = (float)H; c[cbB * 4 + 2] = 1; c[cbB * 4 + 3] = 1; }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (UINT)(c.size() * 4); bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA cd{ c.data(), 0, 0 };
        if (FAILED(g.dev->CreateBuffer(&bd, &cd, &cb))) return false;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
        return SUCCEEDED(g.dev->CreateSamplerState(&sd, &smp)) && SUCCEEDED(g.dev->CreateRasterizerState(&rd, &rs));
    }
    ~Run() {
        for (auto* s : srv) if (s) s->Release();
        if (rs) rs->Release();
        if (cb) cb->Release(); if (smp) smp->Release(); if (st) st->Release(); if (rtv) rtv->Release(); if (rt) rt->Release();
    }
    // draw once: the picture as raw words; sentinel where nothing was drawn
    std::vector<uint32_t> Draw(ID3D11VertexShader* vs, ID3D11PixelShader* ps, float sx, float sy, bool over, UINT verts) {
        const float clr[4] = { -12345.0f, -12345.0f, -12345.0f, -12345.0f };
        g.ctx->ClearRenderTargetView(rtv, clr);
        g.ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{ sx, sy, (float)W + (over ? 1.0f : 0.0f), (float)H + (over ? 1.0f : 0.0f), 0.0f, 1.0f };
        g.ctx->RSSetViewports(1, &vp);
        g.ctx->RSSetState(rs);
        g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g.ctx->IASetInputLayout(nullptr);
        g.ctx->VSSetShader(vs, nullptr, 0);
        g.ctx->PSSetShader(ps, nullptr, 0);
        ID3D11Buffer* cbs[4] = { cb, cb, cb, cb };
        g.ctx->PSSetConstantBuffers(0, 4, cbs);
        g.ctx->PSSetShaderResources(0, 32, srv);
        ID3D11SamplerState* ss[16];
        for (auto& x : ss) x = smp;
        g.ctx->PSSetSamplers(0, 16, ss);
        g.ctx->Draw(verts, 0);
        g.ctx->CopyResource(st, rt);
        std::vector<uint32_t> out((size_t)W * H * 4);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            for (UINT y = 0; y < H; ++y) memcpy(&out[(size_t)y * W * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, W * 16);
            g.ctx->Unmap(st, 0);
        }
        return out;
    }
};
static const float kHalton[8][2] = { { 0.0f, -0.16666667f }, { -0.25f, 0.16666667f }, { 0.25f, -0.38888889f }, { -0.375f, -0.05555556f },
                                     { 0.125f, 0.27777778f }, { -0.125f, -0.27777778f }, { 0.375f, 0.05555556f }, { -0.4375f, 0.38888889f } };

// the WARP checks for one game / look-alike shader and its twin; returns false when the setup itself failed
static bool Equivalence(Gpu& g, const char* name, const std::vector<uint8_t>& game, const PixelReader::Twin& tw, bool strictFix) {
    ID3D11PixelShader* psG = nullptr; ID3D11PixelShader* psT = nullptr;
    if (FAILED(g.dev->CreatePixelShader(game.data(), game.size(), nullptr, &psG))) { CHECK(false, "%s: the game blob is refused", name); return false; }
    const HRESULT hr = g.dev->CreatePixelShader(tw.code.data(), tw.code.size(), nullptr, &psT);
    CHECK(SUCCEEDED(hr), "%s: CreatePixelShader refused the twin (0x%08lx)", name, (unsigned long)hr);
    if (FAILED(hr)) { psG->Release(); return false; }
    const std::vector<SigEl> sig = Isgn(game);
    Run r(g);
    if (!r.Init(DeclaredTex(game), tw.cbA, tw.cbB)) { CHECK(false, "%s: WARP setup", name); psG->Release(); psT->Release(); return false; }
    auto vsOf = [&](bool vary, bool persp) -> ID3D11VertexShader* {
        const std::string src = MakeVs(sig, tw.pReg, vary, persp);
        const std::vector<uint8_t> c = Compile(src.c_str(), "main", "vs_5_0");
        ID3D11VertexShader* v = nullptr;
        if (c.empty() || FAILED(g.dev->CreateVertexShader(c.data(), c.size(), nullptr, &v))) { std::printf("%s", src.c_str()); return nullptr; }
        return v;
    };
    ID3D11VertexShader* vsVary = vsOf(true, true);
    ID3D11VertexShader* vsFlat = vsOf(false, false);
    CHECK(vsVary && vsFlat, "%s: test VS", name);
    uint64_t diffB = 0, diffC = 0, mutC = 0, drawn = 0, drawnB = 0;
    float sentF = -12345.0f; uint32_t sentU; memcpy(&sentU, &sentF, 4);
    if (vsVary && vsFlat) {
        for (const auto& s : kHalton) {
            // b: bit-exact at the game's W x H viewport (perspective quad, varying interpolants)
            const std::vector<uint32_t> a = r.Draw(vsVary, psG, s[0], s[1], false, 6), bt = r.Draw(vsVary, psT, s[0], s[1], false, 6);
            for (size_t i = 0; i < a.size(); ++i) { diffB += a[i] != bt[i]; drawnB += (i % 4 == 0) && a[i] != sentU; }
            // c: the twin under the overscan = the game at W x H (other interpolants constant); the game under the overscan differs
            const std::vector<uint32_t> ref = r.Draw(vsFlat, psG, s[0], s[1], false, 3);
            const std::vector<uint32_t> tov = r.Draw(vsFlat, psT, s[0], s[1], true, 3);
            const std::vector<uint32_t> gov = r.Draw(vsFlat, psG, s[0], s[1], true, 3);
            for (size_t p = 0; p < ref.size() / 4; ++p) {
                if (ref[p * 4] == sentU && ref[p * 4 + 1] == sentU) continue;   // not drawn at W x H (the phase-23 edge)
                ++drawn;
                bool dt = false, dg = false;
                for (int k = 0; k < 4; ++k) { dt |= ref[p * 4 + k] != tov[p * 4 + k]; dg |= ref[p * 4 + k] != gov[p * 4 + k]; }
                diffC += dt; mutC += dg;
            }
        }
    }
    std::printf("  %s: ftoi patched %d, pixel lds %d, v%d SV_Position%s, P = v%d%s, cbA row %d, cbB row %d | b: %llu words differ "
                "(8 phases, W x H) | c: twin@overscan vs game@WxH %llu of %llu px differ, game@overscan %llu px differ (the bug)\n",
                name, tw.ftois, tw.lds, tw.posReg, tw.dclAdded ? " (dcl inserted)" : " (dcl existed)", tw.pReg,
                tw.pRegOnlyDiv ? "" : " (also read elsewhere)", tw.cbA, tw.cbB, (unsigned long long)diffB, (unsigned long long)diffC,
                (unsigned long long)drawn, (unsigned long long)mutC);
    CHECK(drawnB >= 8ull * (Run::W - 1) * (Run::H - 1), "%s: the bit-exact check drew too few pixels (%llu)", name, (unsigned long long)drawnB);
    CHECK(diffB == 0, "%s: the twin is not bit-exact with the game shader at W x H (%llu words)", name, (unsigned long long)diffB);
    if (strictFix && tw.pRegOnlyDiv) {
        CHECK(drawn > 1000, "%s: too few pixels compared (%llu)", name, (unsigned long long)drawn);
        CHECK(diffC == 0, "%s: under the overscan the twin does not read like the game at W x H (%llu px)", name, (unsigned long long)diffC);
    }
    if (mutC > 0) ++g_mutHits;
    if (vsVary) vsVary->Release();
    if (vsFlat) vsFlat->Release();
    psG->Release(); psT->Release();
    return true;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--disasm") != 0) {                // folder scan
        WIN32_FIND_DATAA fd;
        std::string dir = argv[1]; if (dir.back() != '\\' && dir.back() != '/') dir += "\\";
        HANDLE h = FindFirstFileA((dir + "*.dxbc").c_str(), &fd);
        int n = 0, ok = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::vector<uint8_t> b = Load(dir + fd.cFileName);
                const PixelReader::Twin t = PixelReader::Make(b.data(), b.size());
                ++n; ok += t.why == PixelReader::kOk;
                if (t.why != PixelReader::kNone)
                    std::printf("%-40s %08x %-34s ftoi %d lds %d (census %d) pos v%d P v%d cbA %d cbB %d%s\n", fd.cFileName, t.sum[0],
                                PixelReader::WhyName(t.why), t.ftois, t.lds, t.scanLds, t.posReg, t.pReg, t.cbA, t.cbB,
                                PixelReader::ChecksumOk(b.data(), b.size()) ? "" : " CHECKSUM MISMATCH");
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        std::printf("%d blobs, %d twins\n", n, ok);
        return 0;
    }
    const bool disasm = argc > 1;

    // ---- 1. the checksum on fxc output ----------------------------------------------------------------------------------------
    const std::vector<uint8_t> reader = Compile(kLook, "Reader", "ps_5_0"), pos = Compile(kLook, "PosReader", "ps_5_0"),
                               par = Compile(kLook, "Parity", "ps_5_0"), off = Compile(kLook, "Offset", "ps_5_0"),
                               nb = Compile(kLook, "Neighbour", "ps_5_0"), plain = Compile(kLook, "Plain", "ps_5_0");
    CHECK(!reader.empty() && !pos.empty() && !par.empty() && !off.empty() && !nb.empty() && !plain.empty(), "look-alikes compile");
    if (g_fail) { std::printf("%d checks, %d failed\n", g_checks, g_fail); return 1; }
    for (const auto* v : { &reader, &pos, &par, &off, &nb, &plain }) {
        CHECK(PixelReader::ChecksumOk(v->data(), v->size()), "fxc blob checksum (%zu bytes)", v->size());
        std::vector<uint8_t> x = *v;
        x[x.size() / 2] ^= 0x10;
        CHECK(!PixelReader::ChecksumOk(x.data(), x.size()), "a changed byte must fail the checksum");
    }
    // both tail branches of the MD5 padding: blobs of every size mod 64 (an appended unknown chunk keeps the blob valid)
    {
        int lens[64] = {};
        for (int pad = 0; pad < 64; pad += 4) {
            std::vector<uint8_t> x = plain;
            const uint32_t n = WaterPs::Rd(x.data(), 28);
            // a new chunk 'TEST' of `pad` bytes at the end: chunk table + 4 (shift every chunk by 4), header + data
            std::vector<uint8_t> y(x.begin(), x.begin() + 32 + 4 * n);
            const uint32_t newOff = (uint32_t)(x.size() + 4);
            y.insert(y.end(), 4, 0);
            memcpy(&y[32 + 4 * n], &newOff, 4);
            for (uint32_t c = 0; c < n; ++c) { uint32_t o = WaterPs::Rd(x.data(), 32 + 4 * c) + 4; memcpy(&y[32 + 4 * c], &o, 4); }
            y.insert(y.end(), x.begin() + 32 + 4 * n, x.end());
            const uint32_t tag = 0x54534554u, sz = (uint32_t)pad;
            y.insert(y.end(), (const uint8_t*)&tag, (const uint8_t*)&tag + 4);
            y.insert(y.end(), (const uint8_t*)&sz, (const uint8_t*)&sz + 4);
            y.insert(y.end(), (size_t)pad, 0x5A);
            const uint32_t total = (uint32_t)y.size(), cnt = n + 1;
            memcpy(&y[24], &total, 4); memcpy(&y[28], &cnt, 4);
            uint32_t c4[4];
            PixelReader::Checksum(y.data(), y.size(), c4);
            memcpy(&y[4], c4, 16);
            ++lens[(y.size() - 20) % 64];
            Gpu gg;
            if (gg.Init()) {
                ID3D11PixelShader* p = nullptr;
                const HRESULT hr = gg.dev->CreatePixelShader(y.data(), y.size(), nullptr, &p);
                CHECK(SUCCEEDED(hr), "the runtime accepts our checksum (tail %zu bytes mod 64)", (y.size() - 20) % 64);
                if (p) p->Release();
                gg.ctx->Release(); gg.dev->Release();
            }
        }
        int tails56 = 0, tailsLow = 0;
        for (int i = 0; i < 64; ++i) { if (lens[i] && i >= 56) ++tails56; if (lens[i] && i < 56) ++tailsLow; }
        CHECK(tails56 > 0 && tailsLow > 0, "both padding branches exercised (%d / %d)", tails56, tailsLow);
    }

    // ---- 2. Make on the look-alikes ---------------------------------------------------------------------------------------------
    const PixelReader::Twin tr = PixelReader::Make(reader.data(), reader.size());
    CHECK(tr.why == PixelReader::kOk, "Reader: twin expected (%s)", PixelReader::WhyName(tr.why));
    CHECK(tr.ftois == 1 && tr.lds == 2 && tr.scanLds == 2, "Reader: 1 ftoi / 2 lds (%d / %d / census %d)", tr.ftois, tr.lds, tr.scanLds);
    CHECK(tr.posReg == 1 && tr.dclAdded, "Reader: SV_Position v1, dcl inserted (v%d, %d)", tr.posReg, (int)tr.dclAdded);
    CHECK(tr.cbA == 4 && tr.cbB == 5, "Reader: factor rows 4 / 5 (%d / %d)", tr.cbA, tr.cbB);
    CHECK(PixelReader::ChecksumOk(tr.code.data(), tr.code.size()), "Reader twin checksum");
    if (disasm) std::printf("---- Reader\n%s\n---- twin\n%s\n", Disasm(reader).c_str(), Disasm(tr.code).c_str());
    {
        const std::string d = Disasm(tr.code);
        CHECK(d.find("dcl_input_ps_siv linear noperspective v1.xy, position") != std::string::npos, "twin disassembly: the position dcl");
        CHECK(d.find("ftoi r0.xy, v1.xyxx") != std::string::npos || d.find(", v1.xyxx") != std::string::npos, "twin disassembly: ftoi from v1.xy");
        const PixelReader::Twin again = PixelReader::Make(tr.code.data(), tr.code.size());
        CHECK(again.why == PixelReader::kNone, "the twin itself is no pixel reader any more (%s)", PixelReader::WhyName(again.why));
    }
    const PixelReader::Twin tp = PixelReader::Make(pos.data(), pos.size());
    CHECK(tp.why == PixelReader::kNone, "PosReader: nothing to fix (%s)", PixelReader::WhyName(tp.why));
    const PixelReader::Twin tq = PixelReader::Make(par.data(), par.size());
    CHECK(tq.why == PixelReader::kParity || tq.why == PixelReader::kShape, "Parity: no twin (%s)", PixelReader::WhyName(tq.why));
    const PixelReader::Twin to = PixelReader::Make(off.data(), off.size());
    CHECK(to.why != PixelReader::kOk, "Offset: no twin (%s)", PixelReader::WhyName(to.why));
    const PixelReader::Twin tn = PixelReader::Make(nb.data(), nb.size());
    CHECK(tn.why == PixelReader::kShape, "Neighbour: unproven shape (%s)", PixelReader::WhyName(tn.why));
    const PixelReader::Twin tl = PixelReader::Make(plain.data(), plain.size());
    CHECK(tl.why == PixelReader::kNone, "Plain: none (%s)", PixelReader::WhyName(tl.why));
    {
        const PixelReader::Twin tg = PixelReader::Make(reader.data(), reader.size() - 7);
        CHECK(tg.why == PixelReader::kBadBlob, "truncated blob: bad (%s)", PixelReader::WhyName(tg.why));
        std::vector<uint8_t> x = reader; x[100] ^= 1;
        const PixelReader::Twin tx = PixelReader::Make(x.data(), x.size());
        CHECK(tx.why != PixelReader::kOk, "a blob with a wrong checksum gets no twin (%s)", PixelReader::WhyName(tx.why));
    }

    // ---- 3. WARP ---------------------------------------------------------------------------------------------------------------
    Gpu g;
    CHECK(g.Init(), "WARP device");
    if (g.dev) {
        std::printf("look-alike:\n");
        Equivalence(g, "Reader (look-alike)", reader, tr, true);
        CHECK(g_mutHits == 1, "mutation: the look-alike under the overscan must misread");
        // local: the game's own pixel readers
        const char* dirs[] = { "E:\\ETS2-DLAA-Injector\\captures\\phase109_2026-11-14\\", "E:\\ETS2-DLAA-Injector\\captures\\census_2026-11-14\\",
                               "E:\\ETS2-DLAA-Injector\\captures\\" };
        const char* audited[] = { "9a5fca98", "9c062880", "b0852ebe", "1a104d6e", "a5aa1c2d", "0a3c1386", "079791a9", "19f12dc2",
                                  "31586e4b", "fd5e16e5" };
        bool seen[10] = {};
        int games = 0, twins = 0;
        std::vector<uint32_t> done;
        for (const char* dir : dirs) {
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA((std::string(dir) + "ps_*.dxbc").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                const std::vector<uint8_t> b = Load(std::string(dir) + fd.cFileName);
                if (b.size() < 32) continue;
                ++games;
                CHECK(PixelReader::ChecksumOk(b.data(), b.size()), "game blob %s: checksum", fd.cFileName);
                const PixelReader::Twin t = PixelReader::Make(b.data(), b.size());
                const uint32_t s0 = t.sum[0];
                bool dup = false;
                for (uint32_t d : done) dup |= d == s0;
                if (dup) continue;
                done.push_back(s0);
                char h8[12]; snprintf(h8, sizeof(h8), "%08x", s0);
                for (int a = 0; a < 10; ++a) if (!strcmp(h8, audited[a])) {
                    seen[a] = true;
                    CHECK(t.why == PixelReader::kOk, "audited pixel reader %s: twin expected (%s)", h8, PixelReader::WhyName(t.why));
                }
                if (t.why == PixelReader::kNone) continue;
                std::printf("game %s: %s\n", h8, PixelReader::WhyName(t.why));
                if (t.why != PixelReader::kOk) continue;
                ++twins;
                CHECK(t.cbA >= 0 && t.cbB >= 0, "%s: the factor rows were found (%d / %d)", h8, t.cbA, t.cbB);
                Equivalence(g, h8, b, t, true);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        int nSeen = 0;
        for (bool s : seen) nSeen += s;
        if (!games) std::printf("NOTE: no game shaders under captures\\ -- the game-bytecode checks are skipped (CI)\n");
        else {
            std::printf("game blobs %d, distinct pixel-reader twins %d, audited readers found %d of 10\n", games, twins, nSeen);
            CHECK(nSeen >= 2, "at least two audited readers expected in the local captures (%d)", nSeen);
            // mutation: the game shaders themselves misread under the overscan (a read whose result saturates may not show)
            std::printf("game shaders whose picture changes under the overscan (the bug): %d of %d twins\n", g_mutHits - 1, twins);
            CHECK(g_mutHits - 1 >= twins / 2, "mutation: too few game shaders misread under the overscan (%d of %d)", g_mutHits - 1, twins);
        }
        g.ctx->Release(); g.dev->Release();
    }
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
