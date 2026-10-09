// v0.10.0 phase 11 unit test: texture LOD bias scope (tex_lod_bias_scope / tex_aniso_scope) on a WARP device.
// Builds inject.cpp WITHOUT WITH_DLAA into this exe (tests\build_lod_scope_test.bat) and drives the LOD code through its
// real entry points: LodFrameUpdate, LodOnBind (the OM-bind decision), hkOMSetBlendState, hkPSSetSamplers, hkDrawIndexed,
// hkDraw. The "original" functions are the context's own vtable entries (no MinHook). After every step the PS sampler
// slots actually bound on the context are read back (PSGetSamplers + GetDesc) and compared with the expected set.
// phase 12: + the cut-out rule (hkCreatePixelShader / hkPSSetShader, DxbcPsClass, scope opaque | solid | all, the third
// "cut-out" sampler member for mixed scopes, a re-created shader at a reused address). The test pixel shaders are compiled
// here with D3DCompile (a test program, not the game's render thread).
// phase 13: + tex_lod_bias_cutout (the cut-out class's own bias = the third twin set with a VALUE; `same` / key absent with
// scope solid / all = the phase-12 rule; off while tex_lod_bias is 0), the Ctrl+Shift+F1 / F2 step (StepLodCutout), the
// dlaa.ini parse (LoadConfig on a temporary dlaa.ini next to the exe) and the three-class stats tag. Sections 1-13 run with
// tex_lod_bias_cutout = same (= the phase-12 behaviour they were written for).
// Exit code 0 = all checks passed.
// `lod_scope_test.exe --scan <dir>`: prints DxbcPsClass of every *.dxbc file in <dir> (0 unknown, 1 no discard, 2 discard)
// -- checks the scanner against real game shaders dumped from a RenderDoc capture (scripts/rdc_fence_audit.py).
#include "../src/inject.cpp"

#include <cfloat>
#include <string>
#include <d3dcompiler.h>

static int s_checks = 0, s_fails = 0;
#define CHECK(cond, ...) do { ++s_checks; if (!(cond)) { ++s_fails; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); \
                              printf("\n"); } } while (0)

struct Bound { float bias; UINT aniso; D3D11_FILTER filter; bool null; };
static Bound BoundAt(ID3D11DeviceContext* c, UINT slot) {
    ID3D11SamplerState* s = nullptr;
    c->PSGetSamplers(slot, 1, &s);
    Bound b{0.0f, 0, D3D11_FILTER_MIN_MAG_MIP_POINT, s == nullptr};
    if (s) {
        D3D11_SAMPLER_DESC d{};
        s->GetDesc(&d);
        b.bias = d.MipLODBias; b.aniso = d.MaxAnisotropy; b.filter = d.Filter;
        s->Release();
    }
    return b;
}
static ID3D11SamplerState* BoundPtr(ID3D11DeviceContext* c, UINT slot) {
    ID3D11SamplerState* s = nullptr;
    c->PSGetSamplers(slot, 1, &s);
    if (s) s->Release();                                  // identity only (still bound = alive)
    return s;
}

// The game's OM binds as the injector classifies them (hkOMSetRenderTargets sets these, then calls LodOnBind).
static void BindGbuf(ID3D11DeviceContext* c)    { g_jitterPass = true;  g_gbufPass = true;  LodOnBind(c); }
static void BindForward(ID3D11DeviceContext* c) { g_jitterPass = true;  g_gbufPass = false; LodOnBind(c); }
static void BindOther(ID3D11DeviceContext* c)   { g_jitterPass = false; g_gbufPass = false; LodOnBind(c); }

// phase 12: scopes as ints (kLodScopeOpaque / kLodScopeSolid / kLodScopeAll); the phase-11 sections still pass bools
// (false = solid, true = all).
// phase 13: + the tex_lod_bias_cutout mode / value (default `same` = the phase-12 rule the sections 1-13 test).
static void Configure(float bias, int aniso, int lodScope, int anisoScope, int cutMode = kLodCutSame, float cutBias = -0.5f) {
    if (g_lodInScene.load()) { printf("FAIL: Configure inside scene state\n"); ++s_fails; }
    g_lodBias = bias; g_lodAniso = aniso; g_lodScope = lodScope; g_anisoScope = anisoScope;
    g_lodCutMode = cutMode; g_lodCutBias = cutBias;
    g_lodFlushReq = true;                                 // scopes are load-time in game; here: rebuild the twin cache
    LodFrameUpdate(0);
}
static void Configure(float bias, int aniso, bool lodAll, bool anisoAll) {
    Configure(bias, aniso, lodAll ? kLodScopeAll : kLodScopeSolid, anisoAll ? kLodScopeAll : kLodScopeSolid);
}

static ID3DBlob* CompilePs(const char* src) {
    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    const HRESULT hr = D3DCompile(src, strlen(src), "test", nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                  &code, &err);
    if (FAILED(hr)) printf("D3DCompile failed: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
    if (err) err->Release();
    return code;
}

static int ScanDir(const char* dir) {
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%s\\*.dxbc", dir);
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("no *.dxbc in %s\n", dir); return 2; }
    do {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        FILE* f = nullptr;
        if (fopen_s(&f, path, "rb") != 0 || !f) continue;
        std::vector<uint8_t> b;
        uint8_t buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
        fclose(f);
        printf("%s %d\n", fd.cFileName, DxbcPsClass(b.data(), b.size()));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && !strcmp(argv[1], "--scan")) return ScanDir(argv[2]);
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        printf("WARP device creation failed\n");
        return 2;
    }
    void** vt = *reinterpret_cast<void***>(ctx);
    oPSSetSamplers   = reinterpret_cast<PSSetSamplers_t>(vt[10]);
    oOMSetBlendState = reinterpret_cast<OMSetBlendState_t>(vt[35]);
    oDrawIndexed     = reinterpret_cast<decltype(oDrawIndexed)>(vt[12]);
    oDraw            = reinterpret_cast<decltype(oDraw)>(vt[13]);
    g_lodHooked.store(true);
    g_lodBlendHooked.store(true);
    g_gameCtx.store(ctx);
    g_dlaaOn.store(true);                                 // JitterLive (no WITH_DLAA: DLAA on, jitter on, not passive)
    g_jitterEnabled = true;

    // The game's samplers: A trilinear, B anisotropic x4, C full point (never biased), D comparison (never biased).
    D3D11_SAMPLER_DESC sd{};
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = FLT_MAX; sd.MaxAnisotropy = 1; sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    ID3D11SamplerState *A = nullptr, *B = nullptr, *C = nullptr, *D = nullptr;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; dev->CreateSamplerState(&sd, &A);
    sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 4; dev->CreateSamplerState(&sd, &B);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; sd.MaxAnisotropy = 1; dev->CreateSamplerState(&sd, &C);
    sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT; sd.ComparisonFunc = D3D11_COMPARISON_LESS;
    dev->CreateSamplerState(&sd, &D);
    // Blend states: opaque, alpha blend on RT0, alpha-to-coverage (no blend), blend only on RT1 (counts as solid).
    D3D11_BLEND_DESC bd{};
    for (auto& r : bd.RenderTarget) {
        r.SrcBlend = r.SrcBlendAlpha = D3D11_BLEND_ONE; r.DestBlend = r.DestBlendAlpha = D3D11_BLEND_ZERO;
        r.BlendOp = r.BlendOpAlpha = D3D11_BLEND_OP_ADD; r.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    ID3D11BlendState *bsOpaque = nullptr, *bsBlend = nullptr, *bsA2c = nullptr, *bsRt1 = nullptr;
    dev->CreateBlendState(&bd, &bsOpaque);
    D3D11_BLEND_DESC b2 = bd;
    b2.RenderTarget[0].BlendEnable = TRUE; b2.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    b2.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    dev->CreateBlendState(&b2, &bsBlend);
    D3D11_BLEND_DESC b3 = bd; b3.AlphaToCoverageEnable = TRUE;
    dev->CreateBlendState(&b3, &bsA2c);
    D3D11_BLEND_DESC b4 = bd; b4.IndependentBlendEnable = TRUE; b4.RenderTarget[1].BlendEnable = TRUE;
    dev->CreateBlendState(&b4, &bsRt1);
    if (!A || !B || !C || !D || !bsOpaque || !bsBlend || !bsA2c || !bsRt1) { printf("state creation failed\n"); return 2; }
    const float f4[4] = {};
    auto Blend = [&](ID3D11BlendState* bs) { hkOMSetBlendState(ctx, bs, f4, 0xFFFFFFFFu); };
    auto DrawI = [&]() { hkDrawIndexed(ctx, 3, 0, 0); };
    auto Set = [&](UINT slot, ID3D11SamplerState* s) { hkPSSetSamplers(ctx, slot, 1, &s); };
    auto Orig = [&](UINT slot, ID3D11SamplerState* s, const char* what) {
        CHECK(BoundPtr(ctx, slot) == s, "%s: slot %u is not the game's own sampler", what, slot);
    };
    auto Biased = [&](UINT slot, float bias, UINT aniso, D3D11_FILTER f, const char* what) {
        const Bound b = BoundAt(ctx, slot);
        CHECK(!b.null && b.bias == bias && b.aniso == aniso && b.filter == f, "%s: slot %u bias %.2f aniso %u filter 0x%x, "
              "expected %.2f / %u / 0x%x", what, slot, b.bias, b.aniso, (unsigned)b.filter, bias, aniso, (unsigned)f);
    };
    const D3D11_FILTER TRI = D3D11_FILTER_MIN_MAG_MIP_LINEAR, ANI = D3D11_FILTER_ANISOTROPIC;
    UINT triAniso = 0;                                    // the runtime's MaxAnisotropy for A (trilinear: not kept as 1)
    { D3D11_SAMPLER_DESC ad{}; A->GetDesc(&ad); triAniso = ad.MaxAnisotropy; }

    // ---- 1. default scope solid / solid, bias -1, no aniso ---------------------------------------------------------
    Configure(-1.0f, 0, false, false);
    CHECK(g_lodOn && g_lodSplit && g_lodSeeTrivial, "solid/solid: on %d split %d trivial %d", g_lodOn, g_lodSplit, g_lodSeeTrivial);
    { ID3D11SamplerState* s4[4] = {A, B, C, D}; hkPSSetSamplers(ctx, 0, 4, s4); }   // before the pass (state leak)
    Blend(bsBlend);                                                                    // ... and a blended state
    BindGbuf(ctx);
    CHECK(g_lodInScene.load() && g_lodPerDraw.load(), "G-buffer: scene %d perDraw %d", (int)g_lodInScene.load(), (int)g_lodPerDraw.load());
    Orig(0, A, "G-buffer enter with a blended state");                                // no bind at all at the enter
    DrawI();
    Orig(0, A, "blended draw"); Orig(1, B, "blended draw");
    Blend(bsOpaque); DrawI();
    Biased(0, -1.0f, triAniso, TRI, "opaque draw"); Biased(1, -1.0f, 4, ANI, "opaque draw");
    Orig(2, C, "opaque draw (point)"); Orig(3, D, "opaque draw (comparison)");
    const uint64_t sw0 = g_lodSwaps;
    DrawI(); DrawI(); DrawI();
    CHECK(g_lodSwaps == sw0, "consecutive opaque draws swapped %llu times", (unsigned long long)(g_lodSwaps - sw0));
    Set(0, B);                                                                         // new sampler while the full set is bound
    Biased(0, -1.0f, 4, ANI, "PSSetSamplers in the full set");
    Blend(bsA2c); DrawI();
    Orig(0, B, "alpha-to-coverage draw"); Orig(1, B, "alpha-to-coverage draw");
    Set(0, A);                                                                         // new sampler while the see set is bound
    Orig(0, A, "PSSetSamplers in the see-through set");
    Blend(bsRt1); DrawI();
    Biased(0, -1.0f, triAniso, TRI, "blend only on RT1 = solid");
    Blend(nullptr); hkDraw(ctx, 3, 0);
    Biased(0, -1.0f, triAniso, TRI, "default blend state (Draw)");
    {                                                                                  // the game reads a twin back, binds it
        ID3D11SamplerState* tw = BoundPtr(ctx, 0);
        CHECK(tw != A, "twin expected in slot 0");
        Blend(bsBlend);
        Set(0, tw);                                                                    // the set changes at the next draw
        Biased(0, -1.0f, triAniso, TRI, "read-back twin re-bound (full set still bound)");
        DrawI();
        Orig(0, A, "read-back twin re-bound, blended draw");
        Blend(bsOpaque); DrawI();
        Biased(0, -1.0f, triAniso, TRI, "read-back twin, opaque draw");
    }
    BindForward(ctx);                                                                  // G-buffer -> forward (no other bind)
    CHECK(!g_lodInScene.load() && !g_lodPerDraw.load(), "forward pass: scene %d perDraw %d", (int)g_lodInScene.load(), (int)g_lodPerDraw.load());
    Orig(0, A, "forward pass"); Orig(1, B, "forward pass"); Orig(2, C, "forward pass"); Orig(3, D, "forward pass");
    DrawI();
    Orig(0, A, "forward opaque draw");
    BindOther(ctx);
    Orig(0, A, "lighting pass");
    Blend(bsOpaque);
    BindGbuf(ctx);                                                                     // opaque state at the enter
    Biased(0, -1.0f, triAniso, TRI, "G-buffer enter with an opaque state");
    BindGbuf(ctx);                                                                     // redundant re-bind
    Biased(0, -1.0f, triAniso, TRI, "redundant G-buffer re-bind");
    {                                                                                  // blend set by the game while not
        g_gameCtx.store(nullptr);                                                      // tracked (e.g. before the adoption):
        BindOther(ctx);                                                                // the per-draw entry resyncs it
        oOMSetBlendState(ctx, bsBlend, f4, 0xFFFFFFFFu);
        g_gameCtx.store(ctx);
        BindGbuf(ctx);
        Orig(0, A, "resync at the enter (blended state set untracked)");
        DrawI();
        Orig(0, A, "resync: blended draw");
    }
    BindOther(ctx);
    Orig(0, A, "leave"); Orig(1, B, "leave");
    CHECK(g_lodDrawSolid > 0 && g_lodDrawSee > 0, "counters solid %llu see %llu", (unsigned long long)g_lodDrawSolid,
          (unsigned long long)g_lodDrawSee);
    {
        char tag[1200];
        LodStatsTag(tag, sizeof(tag));
        printf("solid/solid tag:%s\n", tag);
        CHECK(strstr(tag, "subst/frame=solid") && strstr(tag, "/ see-through ") && strstr(tag, "scope=solid/solid") &&
              strstr(tag, "cutout=same"), "stats tag format");
    }

    // ---- 2. scope all / all = v0.9.0 behaviour ---------------------------------------------------------------------
    Configure(-1.0f, 0, true, true);
    CHECK(g_lodOn && !g_lodSplit, "all/all: split %d", g_lodSplit);
    Blend(bsBlend);
    BindGbuf(ctx);
    CHECK(g_lodInScene.load() && !g_lodPerDraw.load(), "all/all G-buffer: no per-draw");
    DrawI();
    Biased(0, -1.0f, triAniso, TRI, "all/all blended G-buffer draw"); Biased(1, -1.0f, 4, ANI, "all/all blended G-buffer draw");
    BindForward(ctx);
    DrawI();
    Biased(0, -1.0f, triAniso, TRI, "all/all forward draw");
    Set(0, B);
    Biased(0, -1.0f, 4, ANI, "all/all forward PSSetSamplers");
    BindOther(ctx);
    Orig(0, B, "all/all leave"); Orig(1, B, "all/all leave");
    {
        char tag[1200];
        LodStatsTag(tag, sizeof(tag));
        printf("all/all tag:%s\n", tag);
        CHECK(strstr(tag, "subst/frame=") && !strstr(tag, "/ see-through "), "all/all stats tag format");
    }

    // ---- 3. tex_lod_bias_scope = all, tex_aniso_scope = solid, aniso 16 --------------------------------------------
    Set(0, A);                                                                         // outside: pass-through
    Configure(-1.0f, 16, true, false);
    CHECK(g_lodSplit && !g_lodSeeTrivial, "lod all / aniso solid: split %d trivial %d", g_lodSplit, g_lodSeeTrivial);
    Blend(bsOpaque);
    BindGbuf(ctx);
    DrawI();
    Biased(0, -1.0f, 16, ANI, "lod all/aniso solid: opaque");
    Blend(bsBlend); DrawI();
    Biased(0, -1.0f, triAniso, TRI, "lod all/aniso solid: blended (bias only)");
    Biased(1, -1.0f, 4, ANI, "lod all/aniso solid: blended slot 1 (bias only)");
    BindForward(ctx);
    CHECK(g_lodInScene.load() && !g_lodPerDraw.load(), "lod all/aniso solid: forward in scene, not per draw");
    Blend(bsOpaque); DrawI();
    Biased(0, -1.0f, triAniso, TRI, "lod all/aniso solid: forward (bias only)");
    BindOther(ctx);
    Orig(0, A, "lod all/aniso solid leave"); Orig(1, B, "lod all/aniso solid leave");

    // ---- 4. tex_lod_bias_scope = solid, tex_aniso_scope = all, aniso 16 --------------------------------------------
    Configure(-1.0f, 16, false, true);
    Blend(bsBlend);
    BindGbuf(ctx);
    DrawI();
    Biased(0, 0.0f, 16, ANI, "lod solid/aniso all: blended (aniso only)");
    Blend(bsOpaque); DrawI();
    Biased(0, -1.0f, 16, ANI, "lod solid/aniso all: opaque");
    BindForward(ctx);
    Biased(0, 0.0f, 16, ANI, "lod solid/aniso all: forward (aniso only)");
    BindOther(ctx);
    Orig(0, A, "lod solid/aniso all leave");

    // ---- 5. bias 0 + aniso with lod scope solid / aniso scope all = no split ------------------------------------------
    Configure(0.0f, 8, false, true);
    CHECK(g_lodOn && !g_lodSplit, "bias 0 aniso all: split %d", g_lodSplit);
    Blend(bsBlend);
    BindForward(ctx);
    Biased(0, 0.0f, 8, ANI, "bias 0 aniso all: forward");
    BindOther(ctx);

    // ---- 6. feature off -> nothing bound, cache flushed -------------------------------------------------------------
    Configure(0.0f, 0, false, false);
    CHECK(!g_lodOn && g_lodFill == 0 && g_lodBsFill == 0, "off: on %d fill %u bs %u", g_lodOn, g_lodFill, g_lodBsFill);
    BindGbuf(ctx);
    CHECK(!g_lodInScene.load(), "off: no scene state");
    BindOther(ctx);

    // ==== phase 12: the cut-out (alpha-tested) rule ================================================================
    oCreatePixelShader = reinterpret_cast<CreatePixelShader_t>((*reinterpret_cast<void***>(dev))[15]);
    oPSSetShader       = reinterpret_cast<PSSetShader_t>(vt[9]);
    g_lodPsHooked.store(true);
    const char* kPlain = "Texture2D t : register(t0); SamplerState s : register(s0);\n"
                         "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return t.Sample(s, uv); }";
    const char* kClip  = "Texture2D t : register(t0); SamplerState s : register(s0);\n"
                         "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
                         "  float4 c = t.Sample(s, uv); clip(c.a - 0.05); return c; }";
    const char* kBranch = "Texture2D t : register(t0); SamplerState s : register(s0); cbuffer C : register(b0) { float4 k; };\n"
                          "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"
                          "  float4 c = t.Sample(s, uv); [branch] if (k.x > 0.5) { if (c.a < k.y) discard; } return c * k.z; }";
    const char* kIcb   = "static const float w[8] = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};\n"   // an immediate constant
                         "Texture2D t : register(t0); SamplerState s : register(s0); cbuffer C : register(b0) { uint4 k; };\n"
                         "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target {\n"   // buffer = customdata
                         "  float4 c = t.Sample(s, uv) * w[k.x & 7]; if (c.a < 0.05) discard; return c; }";
    ID3DBlob *bPlain = CompilePs(kPlain), *bClip = CompilePs(kClip), *bBranch = CompilePs(kBranch), *bIcb = CompilePs(kIcb);
    CHECK(bPlain && bClip && bBranch && bIcb, "test pixel shaders compiled");
    if (!bPlain || !bClip || !bBranch || !bIcb) { printf("FAILED: %d / %d\n", s_checks - s_fails, s_checks); return 1; }
    // DxbcPsClass itself
    CHECK(DxbcPsClass(bPlain->GetBufferPointer(), bPlain->GetBufferSize()) == 1, "scan: plain PS = 1");
    CHECK(DxbcPsClass(bClip->GetBufferPointer(), bClip->GetBufferSize()) == 2, "scan: clip() PS = 2");
    CHECK(DxbcPsClass(bBranch->GetBufferPointer(), bBranch->GetBufferSize()) == 2, "scan: discard in a branch = 2");
    CHECK(DxbcPsClass(bIcb->GetBufferPointer(), bIcb->GetBufferSize()) == 2, "scan: discard after an immediate constant buffer = 2");
    CHECK(DxbcPsClass(bClip->GetBufferPointer(), 31) == 0 && DxbcPsClass(nullptr, 100) == 0, "scan: short / null = 0");
    {
        std::vector<uint8_t> bad((const uint8_t*)bClip->GetBufferPointer(),
                                 (const uint8_t*)bClip->GetBufferPointer() + bClip->GetBufferSize());
        bad[0] = 'X';
        CHECK(DxbcPsClass(bad.data(), bad.size()) == 0, "scan: wrong magic = 0");
        ID3DBlob* vs = nullptr;
        const char* kVs = "float4 main(float4 p : POSITION) : SV_Position { return p; }";
        if (SUCCEEDED(D3DCompile(kVs, strlen(kVs), "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs, nullptr)) && vs) {
            CHECK(DxbcPsClass(vs->GetBufferPointer(), vs->GetBufferSize()) == 0, "scan: a vertex shader = 0");
            vs->Release();
        }
    }
    // shaders through the CreatePixelShader hook (+ one created behind its back = unknown)
    ID3D11PixelShader *psPlain = nullptr, *psClip = nullptr, *psBranch = nullptr, *psUnknown = nullptr;
    const uint32_t cut0 = g_lodPsCreCut.load(), plain0 = g_lodPsCrePlain.load();
    CHECK(SUCCEEDED(hkCreatePixelShader(dev, bPlain->GetBufferPointer(), bPlain->GetBufferSize(), nullptr, &psPlain)) && psPlain,
          "hook: create plain");
    CHECK(SUCCEEDED(hkCreatePixelShader(dev, bClip->GetBufferPointer(), bClip->GetBufferSize(), nullptr, &psClip)) && psClip,
          "hook: create clip");
    CHECK(SUCCEEDED(hkCreatePixelShader(dev, bBranch->GetBufferPointer(), bBranch->GetBufferSize(), nullptr, &psBranch)) && psBranch,
          "hook: create branch");
    CHECK(SUCCEEDED(hkCreatePixelShader(dev, bClip->GetBufferPointer(), bClip->GetBufferSize(), nullptr, nullptr)),
          "hook: validation-only create (null out pointer) passes through");
    dev->CreatePixelShader(bClip->GetBufferPointer(), bClip->GetBufferSize(), nullptr, &psUnknown);   // not seen by the hook
    CHECK(g_lodPsCreCut.load() == cut0 + 2 && g_lodPsCrePlain.load() == plain0 + 1, "hook counters cut +%u plain +%u",
          g_lodPsCreCut.load() - cut0, g_lodPsCrePlain.load() - plain0);
    CHECK(LodPsLookup(psPlain) == 1 && LodPsLookup(psClip) == 2 && LodPsLookup(psBranch) == 2 && LodPsLookup(psUnknown) == 0 &&
          LodPsLookup(nullptr) == 1, "lookup plain %d clip %d branch %d unknown %d null %d", LodPsLookup(psPlain),
          LodPsLookup(psClip), LodPsLookup(psBranch), LodPsLookup(psUnknown), LodPsLookup(nullptr));
    auto Ps = [&](ID3D11PixelShader* ps) { hkPSSetShader(ctx, ps, nullptr, 0); };

    // ---- 7. default opaque / opaque, bias -1: cut-outs keep the game's samplers ---------------------------------------
    { ID3D11SamplerState* s2[2] = {A, B}; hkPSSetSamplers(ctx, 0, 2, s2); }
    Configure(-1.0f, 0, kLodScopeOpaque, kLodScopeOpaque);
    CHECK(g_lodOn && g_lodSplit && g_lodSeeTrivial && g_lodCutSet == kLodSetSee, "opaque/opaque: on %d split %d trivial %d "
          "cutSet %d", g_lodOn, g_lodSplit, g_lodSeeTrivial, (int)g_lodCutSet);
    Blend(bsOpaque); Ps(psClip);
    BindGbuf(ctx);
    Orig(0, A, "opaque/opaque: G-buffer enter with an alpha-tested PS"); Orig(1, B, "opaque/opaque: enter, slot 1");
    const uint64_t cutD0 = g_lodDrawCut, seeD0 = g_lodDrawSee, solD0 = g_lodDrawSolid, unk0 = g_lodDrawUnk;
    DrawI();                                                                            // cut 1
    Orig(0, A, "opaque/opaque: alpha-tested draw"); Orig(1, B, "opaque/opaque: alpha-tested draw slot 1");
    Ps(psPlain); DrawI();                                                               // solid 1
    Biased(0, -1.0f, triAniso, TRI, "opaque/opaque: opaque PS without discard");
    Biased(1, -1.0f, 4, ANI, "opaque/opaque: plain slot 1");
    Ps(psBranch); DrawI();                                                              // cut 2
    Orig(0, A, "opaque/opaque: discard in a branch");
    const uint64_t swc = g_lodSwaps;
    DrawI(); Ps(psClip); DrawI(); DrawI();                                              // cut 3, 4, 5
    CHECK(g_lodSwaps == swc, "consecutive cut-out draws swapped %llu times", (unsigned long long)(g_lodSwaps - swc));
    Blend(bsBlend); DrawI();                                                            // see 1 (same members: no swap)
    CHECK(g_lodSwaps == swc, "cut-out -> blended (same members) swapped %llu times", (unsigned long long)(g_lodSwaps - swc));
    Orig(0, A, "opaque/opaque: blended + alpha-tested");
    Blend(bsOpaque); Ps(psUnknown); DrawI();                                            // solid 2 (unknown)
    Biased(0, -1.0f, triAniso, TRI, "opaque/opaque: unknown PS = no discard (phase-11 behaviour)");
    Ps(nullptr); DrawI();                                                               // solid 3
    Biased(0, -1.0f, triAniso, TRI, "opaque/opaque: no PS = solid");
    Set(0, B); Ps(psClip); DrawI();                                                     // cut 6
    Orig(0, B, "opaque/opaque: new sampler, then a cut-out draw");
    Set(0, A);                                                                          // PSSetSamplers in the cut-out set
    Orig(0, A, "opaque/opaque: PSSetSamplers while the cut-out / see-through set is bound");
    CHECK(g_lodDrawCut - cutD0 == 6 && g_lodDrawUnk - unk0 == 1, "counters: cut-out %llu (want 6) unknown %llu (want 1)",
          (unsigned long long)(g_lodDrawCut - cutD0), (unsigned long long)(g_lodDrawUnk - unk0));
    CHECK(g_lodDrawSee - seeD0 == 1 && g_lodDrawSolid - solD0 == 3, "counters: see-through %llu (want 1 blended; phase 13: the "
          "6 alpha-tested are their own class) solid %llu (want 3)", (unsigned long long)(g_lodDrawSee - seeD0),
          (unsigned long long)(g_lodDrawSolid - solD0));
    {                                                                                   // PS + blend set untracked: resync
        g_gameCtx.store(nullptr);
        BindOther(ctx);
        oPSSetShader(ctx, psClip, nullptr, 0);
        oOMSetBlendState(ctx, bsOpaque, f4, 0xFFFFFFFFu);
        g_gameCtx.store(ctx);
        g_lodPsCur = psPlain;                                                           // stale on purpose
        BindGbuf(ctx);
        Orig(0, A, "resync at the enter: the alpha-tested PS bound untracked");
        DrawI();
        Orig(0, A, "resync: alpha-tested draw");
    }
    BindOther(ctx);
    Orig(0, A, "opaque/opaque leave"); Orig(1, B, "opaque/opaque leave");
    {
        char tag[1200];
        LodStatsTag(tag, sizeof(tag));
        printf("opaque/opaque tag:%s\n", tag);
        CHECK(strstr(tag, "scope=opaque/opaque") && strstr(tag, "/ see-through ") && strstr(tag, "alpha-tested") &&
              strstr(tag, "-> see-through set: bias 0.00") && strstr(tag, "unknown-PS draws"), "opaque/opaque stats tag format");
    }

    // ---- 8. solid / solid (phase 11): cut-outs are biased again --------------------------------------------------------
    Configure(-1.0f, 0, kLodScopeSolid, kLodScopeSolid);
    CHECK(g_lodCutSet == kLodSetFull, "solid/solid: cut set %d", (int)g_lodCutSet);
    Blend(bsOpaque); Ps(psClip);
    BindGbuf(ctx);
    DrawI();
    Biased(0, -1.0f, triAniso, TRI, "solid/solid: alpha-tested draw biased (phase 11)");
    Blend(bsBlend); DrawI();
    Orig(0, A, "solid/solid: blended");
    BindOther(ctx);
    {
        char tag[1200];
        LodStatsTag(tag, sizeof(tag));
        printf("solid/solid tag:%s\n", tag);
        CHECK(strstr(tag, "scope=solid/solid") && strstr(tag, "-> full set"), "solid/solid stats tag format");
    }

    // ---- 9. mixed: lod solid / aniso opaque, aniso 16 -> a cut-out twin of its own (bias only) -------------------------
    Configure(-1.0f, 16, kLodScopeSolid, kLodScopeOpaque);
    CHECK(g_lodCutSet == kLodSetCut && g_lodSplit, "lod solid/aniso opaque: cut set %d split %d", (int)g_lodCutSet, g_lodSplit);
    Blend(bsOpaque); Ps(psPlain);
    BindGbuf(ctx);
    DrawI();
    Biased(0, -1.0f, 16, ANI, "lod solid/aniso opaque: plain = full");
    Biased(1, -1.0f, 16, ANI, "lod solid/aniso opaque: plain slot 1");
    Ps(psClip); DrawI();
    Biased(0, -1.0f, triAniso, TRI, "lod solid/aniso opaque: cut-out = bias only");
    Biased(1, -1.0f, 4, ANI, "lod solid/aniso opaque: cut-out slot 1 = bias only");
    Blend(bsBlend); DrawI();
    Orig(0, A, "lod solid/aniso opaque: blended = original"); Orig(1, B, "lod solid/aniso opaque: blended slot 1");
    Blend(bsOpaque); DrawI();
    Biased(0, -1.0f, triAniso, TRI, "lod solid/aniso opaque: back to the cut-out set");
    Set(0, B);
    Biased(0, -1.0f, 4, ANI, "lod solid/aniso opaque: PSSetSamplers in the cut-out set (bias-only twin of B)");
    CHECK(g_lodCutTwinN >= 2, "cut-out twins %u", g_lodCutTwinN);
    BindOther(ctx);
    Orig(0, B, "lod solid/aniso opaque leave"); Orig(1, B, "lod solid/aniso opaque leave slot 1");
    Set(0, A);

    // ---- 10. mixed: lod opaque / aniso solid -> cut-out = aniso only ---------------------------------------------------
    Configure(-1.0f, 16, kLodScopeOpaque, kLodScopeSolid);
    CHECK(g_lodCutSet == kLodSetCut, "lod opaque/aniso solid: cut set %d", (int)g_lodCutSet);
    Blend(bsOpaque); Ps(psClip);
    BindGbuf(ctx);
    Biased(0, 0.0f, 16, ANI, "lod opaque/aniso solid: enter with a cut-out = aniso only");
    DrawI();
    Biased(0, 0.0f, 16, ANI, "lod opaque/aniso solid: cut-out = aniso only");
    Ps(psPlain); DrawI();
    Biased(0, -1.0f, 16, ANI, "lod opaque/aniso solid: plain = full");
    BindForward(ctx);
    Orig(0, A, "lod opaque/aniso solid: forward = original");
    BindOther(ctx);

    // ---- 11. lod all / aniso opaque: cut-out = the see-through member (bias only), no third twin -----------------------
    Configure(-1.0f, 16, kLodScopeAll, kLodScopeOpaque);
    CHECK(g_lodCutSet == kLodSetSee, "lod all/aniso opaque: cut set %d", (int)g_lodCutSet);
    Blend(bsOpaque); Ps(psClip);
    BindGbuf(ctx);
    DrawI();
    Biased(0, -1.0f, triAniso, TRI, "lod all/aniso opaque: cut-out = bias only (the see-through member)");
    CHECK(g_lodCutTwinN == 0, "lod all/aniso opaque: no cut-out twin of its own (%u)", g_lodCutTwinN);
    Ps(psPlain); DrawI();
    Biased(0, -1.0f, 16, ANI, "lod all/aniso opaque: plain = full");
    BindOther(ctx);

    // ---- 12. all / all: no split, cut-outs biased (v0.9.0) ---------------------------------------------------------------
    Configure(-1.0f, 0, kLodScopeAll, kLodScopeAll);
    CHECK(!g_lodSplit, "all/all: split %d", g_lodSplit);
    Ps(psClip);
    BindGbuf(ctx);
    DrawI();
    Biased(0, -1.0f, triAniso, TRI, "all/all: alpha-tested draw biased");
    BindOther(ctx);

    // ---- 13. PS table: a re-created shader at a reused address, the memo, an unparsable re-creation ----------------------
    {
        ID3D11PixelShader* fake = reinterpret_cast<ID3D11PixelShader*>((uintptr_t)0x7ff012345670ull);
        CHECK(LodPsLookup(fake) == 0, "fake pointer unknown");
        LodPsNote(fake, 2);
        g_lodPsCur = fake;
        CHECK(LodCurPsClass() == 2, "memo: cut-out");
        const uint32_t fill = g_lodPsFill.load();
        LodPsNote(fake, 1);                                                             // the same address, a new shader
        CHECK(LodCurPsClass() == 1 && g_lodPsFill.load() == fill, "re-created at the same address: class %d, fill +%u",
              LodCurPsClass(), g_lodPsFill.load() - fill);
        LodPsNote(fake, 0);
        CHECK(LodCurPsClass() == 0, "unparsable re-creation = unknown");
        g_lodPsCur = nullptr;
    }

    // ==== phase 13: tex_lod_bias_cutout = the cut-out class's own bias (the third twin set with a value) ================
    g_beeps = false;                                                                    // StepLodCutout below: no Beep threads
    auto Tag = [&](const char* what) -> std::string {
        char tag[1200];
        LodStatsTag(tag, sizeof(tag));
        printf("%s tag:%s\n", what, tag);
        return std::string(tag);
    };
    auto Has = [](const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; };

    // ---- 14. key absent, scope opaque/opaque (the new default), bias -1: cut-outs -0.50 in a third set -------------------
    { ID3D11SamplerState* s2[2] = {A, B}; hkPSSetSamplers(ctx, 0, 2, s2); }
    Configure(-1.0f, 0, kLodScopeOpaque, kLodScopeOpaque, kLodCutAbsent, -0.5f);
    CHECK(!LodCutFollows() && g_lodCutEff == -0.5f && g_lodCutSet == kLodSetCut && g_lodSplit && g_lodSeeTrivial && g_lodOn,
          "cutout default: follows %d eff %.2f cutSet %d split %d trivial %d on %d", (int)LodCutFollows(), g_lodCutEff,
          (int)g_lodCutSet, g_lodSplit, g_lodSeeTrivial, g_lodOn);
    {
        char t[16]; LodCutoutText(t, sizeof(t));
        CHECK(!strcmp(t, "-0.50"), "cutout default: text '%s' (want -0.50)", t);
    }
    Blend(bsOpaque); Ps(psClip);
    BindGbuf(ctx);
    Biased(0, -0.5f, triAniso, TRI, "cutout default: G-buffer enter with an alpha-tested PS");
    Biased(1, -0.5f, 4, ANI, "cutout default: enter, slot 1");
    {
        const uint64_t c0 = g_lodDrawCut, so0 = g_lodDrawSolid, se0 = g_lodDrawSee;
        DrawI();                                                                        // cut 1
        Biased(0, -0.5f, triAniso, TRI, "cutout default: alpha-tested draw");
        const uint64_t sw = g_lodSwaps;
        DrawI();                                                                        // cut 2
        CHECK(g_lodSwaps == sw, "cutout default: consecutive cut-out draws swapped %llu times", (unsigned long long)(g_lodSwaps - sw));
        Ps(psPlain); DrawI();                                                           // solid 1
        Biased(0, -1.0f, triAniso, TRI, "cutout default: plain = full -1"); Biased(1, -1.0f, 4, ANI, "cutout default: plain slot 1");
        CHECK(g_lodSwaps == sw + 1, "cutout default: cut-out -> solid = 1 swap (%llu)", (unsigned long long)(g_lodSwaps - sw));
        Ps(psBranch); DrawI();                                                          // cut 3
        Biased(0, -0.5f, triAniso, TRI, "cutout default: discard in a branch");
        Set(0, B);                                                                      // PSSetSamplers in the cut-out set
        Biased(0, -0.5f, 4, ANI, "cutout default: PSSetSamplers while the cut-out set is bound (B's cut-out twin)");
        Set(0, A);
        Biased(0, -0.5f, triAniso, TRI, "cutout default: PSSetSamplers A while the cut-out set is bound");
        Blend(bsBlend); DrawI();                                                        // see 1
        Orig(0, A, "cutout default: blended draw"); Orig(1, B, "cutout default: blended draw slot 1");
        Blend(bsOpaque); DrawI();                                                       // cut 4
        Biased(0, -0.5f, triAniso, TRI, "cutout default: back to the cut-out set");
        {                                                                               // the game reads a cut-out twin back
            ID3D11SamplerState* tw = BoundPtr(ctx, 0);
            CHECK(tw != A, "cutout default: cut-out twin expected in slot 0");
            Set(0, tw);
            Biased(0, -0.5f, triAniso, TRI, "cutout default: read-back cut-out twin re-bound (cut-out set bound)");
            Ps(psPlain); DrawI();                                                       // solid 2
            Biased(0, -1.0f, triAniso, TRI, "cutout default: read-back cut-out twin -> its original's full twin");
            Blend(bsBlend); DrawI();                                                    // see 2
            Orig(0, A, "cutout default: read-back cut-out twin -> its original in the see-through set");
        }
        CHECK(g_lodDrawCut - c0 == 4 && g_lodDrawSolid - so0 == 2 && g_lodDrawSee - se0 == 2, "cutout default: classes cut %llu "
              "(want 4) solid %llu (want 2) see %llu (want 2)", (unsigned long long)(g_lodDrawCut - c0),
              (unsigned long long)(g_lodDrawSolid - so0), (unsigned long long)(g_lodDrawSee - se0));
    }
    CHECK(g_lodCutTwinN >= 2 && g_lodSeeTwinN == 0, "cutout default: cut-out twins %u (want >= 2) see-through twins %u",
          g_lodCutTwinN, g_lodSeeTwinN);
    BindForward(ctx);
    CHECK(!g_lodInScene.load(), "cutout default: forward pass = no scene state");
    Orig(0, A, "cutout default: forward pass"); Orig(1, B, "cutout default: forward pass slot 1");
    BindOther(ctx);
    {
        const std::string t = Tag("cutout default");
        CHECK(Has(t, "cutout=-0.50") && Has(t, "subst/frame=solid ") && Has(t, "(full set: bias -1.00 aniso 0)") &&
              Has(t, "alpha-tested ") && Has(t, "(-> cut-out set: bias -0.50 aniso 0, tex_lod_bias_cutout=-0.50)") &&
              Has(t, "/ see-through ") && Has(t, "(see-through set: bias 0.00 aniso 0)") && Has(t, "cut-out 2)"),
              "cutout default: stats tag format");
    }

    // ---- 15. key absent + scope solid / all = `same` (phase 12: cut-outs get tex_lod_bias) --------------------------------
    Configure(-1.0f, 0, kLodScopeSolid, kLodScopeSolid, kLodCutAbsent, -0.5f);
    CHECK(LodCutFollows() && g_lodCutEff == -1.0f && g_lodCutSet == kLodSetFull, "absent + solid: follows %d eff %.2f cutSet %d",
          (int)LodCutFollows(), g_lodCutEff, (int)g_lodCutSet);
    { char t[16]; LodCutoutText(t, sizeof(t)); CHECK(!strcmp(t, "same"), "absent + solid: text '%s' (want same)", t); }
    Blend(bsOpaque); Ps(psClip);
    BindGbuf(ctx);
    DrawI();
    Biased(0, -1.0f, triAniso, TRI, "absent + solid: alpha-tested = full (phase 12 solid)");
    CHECK(g_lodCutTwinN == 0, "absent + solid: no cut-out twin (%u)", g_lodCutTwinN);
    BindOther(ctx);
    Configure(-1.0f, 0, kLodScopeAll, kLodScopeAll, kLodCutAbsent, -0.5f);
    CHECK(LodCutFollows() && g_lodCutSet == kLodSetFull && !g_lodSplit, "absent + all: follows %d cutSet %d split %d",
          (int)LodCutFollows(), (int)g_lodCutSet, g_lodSplit);
    Ps(psClip);
    BindGbuf(ctx);
    DrawI();
    Biased(0, -1.0f, triAniso, TRI, "absent + all: alpha-tested biased (v0.9.0)");
    BindOther(ctx);

    // ---- 16. `same` and 0 with scope opaque = phase 12 exactly (cut-outs keep the game's samplers, no third twin) ----------
    for (int k = 0; k < 2; ++k) {
        const char* what = k == 0 ? "same + opaque" : "0 + opaque";
        Configure(-1.0f, 0, kLodScopeOpaque, kLodScopeOpaque, k == 0 ? kLodCutSame : kLodCutValue, 0.0f);
        CHECK(g_lodCutEff == 0.0f && g_lodCutSet == kLodSetSee && g_lodSplit && g_lodSeeTrivial, "%s: eff %.2f cutSet %d split %d "
              "trivial %d", what, g_lodCutEff, (int)g_lodCutSet, g_lodSplit, g_lodSeeTrivial);
        Blend(bsOpaque); Ps(psClip);
        BindGbuf(ctx);
        Orig(0, A, what); Orig(1, B, what);
        DrawI();
        Orig(0, A, what);
        Ps(psPlain); DrawI();
        Biased(0, -1.0f, triAniso, TRI, what);
        Ps(psClip); Blend(bsBlend); DrawI();
        Orig(0, A, what);
        const uint64_t sw = g_lodSwaps;
        Blend(bsOpaque); DrawI();
        CHECK(g_lodSwaps == sw, "%s: blended -> alpha-tested swapped %llu times (same set)", what, (unsigned long long)(g_lodSwaps - sw));
        CHECK(g_lodCutTwinN == 0, "%s: no cut-out twin (%u)", what, g_lodCutTwinN);
        BindOther(ctx);
    }
    {
        const std::string t = Tag("0 + opaque");
        CHECK(Has(t, "cutout=0.00") && Has(t, "(-> see-through set: bias 0.00 aniso 0, tex_lod_bias_cutout=0.00)"),
              "0 + opaque: stats tag format");
    }

    // ---- 17. a value equal to tex_lod_bias -> the full set (no third twin) ------------------------------------------------
    Configure(-1.0f, 0, kLodScopeOpaque, kLodScopeOpaque, kLodCutValue, -1.0f);
    CHECK(g_lodCutSet == kLodSetFull && g_lodSplit, "value = bias: cutSet %d split %d", (int)g_lodCutSet, g_lodSplit);
    Blend(bsOpaque); Ps(psClip);
    BindGbuf(ctx);
    DrawI();
    Biased(0, -1.0f, triAniso, TRI, "value = bias: alpha-tested = full");
    CHECK(g_lodCutTwinN == 0, "value = bias: no cut-out twin (%u)", g_lodCutTwinN);
    Blend(bsBlend); DrawI();
    Orig(0, A, "value = bias: blended");
    BindOther(ctx);

    // ---- 18. a value wins over scope solid for the cut-out class ------------------------------------------------------------
    Configure(-1.0f, 0, kLodScopeSolid, kLodScopeSolid, kLodCutValue, -0.25f);
    CHECK(!LodCutFollows() && g_lodCutEff == -0.25f && g_lodCutSet == kLodSetCut, "value + solid: eff %.2f cutSet %d", g_lodCutEff,
          (int)g_lodCutSet);
    Blend(bsOpaque); Ps(psClip);
    BindGbuf(ctx);
    DrawI();
    Biased(0, -0.25f, triAniso, TRI, "value + solid: alpha-tested -0.25"); Biased(1, -0.25f, 4, ANI, "value + solid: slot 1");
    Ps(psPlain); DrawI();
    Biased(0, -1.0f, triAniso, TRI, "value + solid: plain -1");
    Blend(bsBlend); DrawI();
    Orig(0, A, "value + solid: blended");
    BindOther(ctx);

    // ---- 19. aniso per tex_aniso_scope (opaque: none on cut-outs; solid: cut-outs get it) ------------------------------------
    Configure(-1.0f, 16, kLodScopeOpaque, kLodScopeOpaque, kLodCutValue, -0.5f);
    CHECK(g_lodCutSet == kLodSetCut, "aniso opaque: cutSet %d", (int)g_lodCutSet);
    Blend(bsOpaque); Ps(psClip);
    BindGbuf(ctx);
    DrawI();
    Biased(0, -0.5f, triAniso, TRI, "aniso opaque: alpha-tested = bias only"); Biased(1, -0.5f, 4, ANI, "aniso opaque: slot 1");
    Ps(psPlain); DrawI();
    Biased(0, -1.0f, 16, ANI, "aniso opaque: plain = full");
    BindOther(ctx);
    Configure(-1.0f, 16, kLodScopeOpaque, kLodScopeSolid, kLodCutValue, -0.5f);
    Blend(bsOpaque); Ps(psClip);
    BindGbuf(ctx);
    DrawI();
    Biased(0, -0.5f, 16, ANI, "aniso solid: alpha-tested = -0.5 + aniso 16"); Biased(1, -0.5f, 16, ANI, "aniso solid: slot 1");
    Blend(bsBlend); DrawI();
    Orig(0, A, "aniso solid: blended = original");
    BindOther(ctx);

    // ---- 20. three distinct sets: lod all / aniso opaque, aniso 16, cutout -0.5 ---------------------------------------------
    Configure(-1.0f, 16, kLodScopeAll, kLodScopeOpaque, kLodCutValue, -0.5f);
    CHECK(g_lodCutSet == kLodSetCut && g_lodSplit && !g_lodSeeTrivial, "three sets: cutSet %d split %d trivial %d",
          (int)g_lodCutSet, g_lodSplit, g_lodSeeTrivial);
    Blend(bsOpaque); Ps(psPlain);
    BindGbuf(ctx);
    DrawI();
    Biased(0, -1.0f, 16, ANI, "three sets: plain = -1 + aniso 16");
    Ps(psClip); DrawI();
    Biased(0, -0.5f, triAniso, TRI, "three sets: alpha-tested = -0.5");
    Blend(bsBlend); DrawI();
    Biased(0, -1.0f, triAniso, TRI, "three sets: blended = -1 (see-through, bias only)");
    Blend(bsOpaque); DrawI();
    Biased(0, -0.5f, triAniso, TRI, "three sets: alpha-tested again");
    CHECK(g_lodCutTwinN >= 1 && g_lodSeeTwinN >= 1, "three sets: cut-out twins %u see-through twins %u", g_lodCutTwinN, g_lodSeeTwinN);
    BindForward(ctx);
    Biased(0, -1.0f, triAniso, TRI, "three sets: forward = the see-through set");
    BindOther(ctx);
    Orig(0, A, "three sets: leave"); Orig(1, B, "three sets: leave slot 1");

    // ---- 21. tex_lod_bias 0 switches the cut-out bias off too ---------------------------------------------------------------
    Configure(0.0f, 0, kLodScopeOpaque, kLodScopeOpaque, kLodCutValue, -0.5f);
    CHECK(!g_lodOn && g_lodCutEff == 0.0f, "bias 0: on %d cut eff %.2f", g_lodOn, g_lodCutEff);
    Blend(bsOpaque); Ps(psClip);
    BindGbuf(ctx);
    CHECK(!g_lodInScene.load(), "bias 0: no scene state");
    BindOther(ctx);
    Configure(0.0f, 8, kLodScopeOpaque, kLodScopeOpaque, kLodCutValue, -0.5f);
    CHECK(g_lodOn && g_lodCutEff == 0.0f && g_lodCutSet == kLodSetSee, "bias 0 + aniso: on %d cut eff %.2f cutSet %d", g_lodOn,
          g_lodCutEff, (int)g_lodCutSet);
    BindGbuf(ctx);
    DrawI();
    Orig(0, A, "bias 0 + aniso: alpha-tested = original");
    Ps(psPlain); DrawI();
    Biased(0, 0.0f, 8, ANI, "bias 0 + aniso: plain = aniso 8 only");
    BindOther(ctx);

    // ---- 22. Ctrl+Shift+F1 / F2 (StepLodCutout): steps, limits, `same` start, a live rebuild ---------------------------------
    Configure(-1.0f, 0, kLodScopeOpaque, kLodScopeOpaque, kLodCutAbsent, -0.5f);
    {
        const uint32_t gen0 = g_lodGen;
        StepLodCutout(-1, 0);
        CHECK(g_lodCutMode == kLodCutValue && g_lodCutBias == -0.75f, "step down: mode %d value %.2f (want 2 / -0.75)", g_lodCutMode,
              g_lodCutBias);
        CHECK(g_lodBias == -1.0f, "step down: tex_lod_bias untouched (%.2f)", g_lodBias);
        LodFrameUpdate(0);                                                              // next frame: no flush request here
        CHECK(g_lodCutEff == -0.75f && g_lodGen != gen0, "step down: cut eff %.2f gen bumped %d", g_lodCutEff, (int)(g_lodGen != gen0));
        Blend(bsOpaque); Ps(psClip);
        BindGbuf(ctx);                                                                  // the enter rebuilds the twins
        Biased(0, -0.75f, triAniso, TRI, "step down: rebuilt cut-out twin at the next enter");
        Ps(psPlain); DrawI();
        Biased(0, -1.0f, triAniso, TRI, "step down: plain still -1");
        BindOther(ctx);
        StepLodCutout(+1, 0); StepLodCutout(+1, 0); StepLodCutout(+1, 0);
        CHECK(g_lodCutBias == 0.0f, "step up x3: %.2f (want 0)", g_lodCutBias);
        StepLodCutout(+1, 0);
        CHECK(g_lodCutBias == 0.0f, "step up at the limit: %.2f (want 0)", g_lodCutBias);
        g_lodCutBias = -3.0f;
        StepLodCutout(-1, 0);
        CHECK(g_lodCutBias == -3.0f, "step down at the limit: %.2f (want -3)", g_lodCutBias);
        g_lodCutBias = 0.6f;                                                            // a + value from dlaa.ini only steps down
        StepLodCutout(+1, 0);
        CHECK(g_lodCutBias == 0.6f, "step up from +0.6: %.2f (unchanged)", g_lodCutBias);
        StepLodCutout(-1, 0);
        CHECK(g_lodCutBias == 0.25f, "step down from +0.6: %.2f (want 0.25)", g_lodCutBias);
    }
    Configure(-1.0f, 0, kLodScopeOpaque, kLodScopeOpaque, kLodCutSame, -0.5f);
    StepLodCutout(-1, 0);
    CHECK(g_lodCutMode == kLodCutValue && g_lodCutBias == -0.25f, "same + opaque, step down: mode %d value %.2f (want 2 / -0.25)",
          g_lodCutMode, g_lodCutBias);
    Configure(-1.0f, 0, kLodScopeSolid, kLodScopeSolid, kLodCutAbsent, -0.5f);
    StepLodCutout(-1, 0);
    CHECK(g_lodCutMode == kLodCutValue && g_lodCutBias == -1.25f, "absent + solid, step down: mode %d value %.2f (want 2 / -1.25)",
          g_lodCutMode, g_lodCutBias);
    Configure(-1.0f, 0, kLodScopeOpaque, kLodScopeOpaque, kLodCutValue, -0.5f);
    StepLodBias(+1, 0);
    LodFrameUpdate(0);
    CHECK(g_lodBias == -0.75f && g_lodCutBias == -0.5f && g_lodCutEff == -0.5f, "Ctrl+F2 leaves the cut-out value: bias %.2f cut "
          "%.2f eff %.2f", g_lodBias, g_lodCutBias, g_lodCutEff);

    // ---- 23. the default key bindings (exact modifiers: Ctrl+Shift+F1 is not Ctrl+F1 / Shift+F1) ------------------------------
    InitKeyBinds();
    CHECK(g_keys[KA_LOD_CUT_DOWN].vk == VK_F1 && g_keys[KA_LOD_CUT_DOWN].mods == (MOD_CTRL_BIT | MOD_SHIFT_BIT) &&
          g_keys[KA_LOD_CUT_UP].vk == VK_F2 && g_keys[KA_LOD_CUT_UP].mods == (MOD_CTRL_BIT | MOD_SHIFT_BIT),
          "key_lod_cutout_down / up defaults: vk %d mods %u / vk %d mods %u", g_keys[KA_LOD_CUT_DOWN].vk,
          g_keys[KA_LOD_CUT_DOWN].mods, g_keys[KA_LOD_CUT_UP].vk, g_keys[KA_LOD_CUT_UP].mods);
    CHECK(!strcmp(g_keys[KA_LOD_CUT_DOWN].ini, "key_lod_cutout_down") && !strcmp(g_keys[KA_LOD_CUT_UP].ini, "key_lod_cutout_up"),
          "key table order: %s / %s", g_keys[KA_LOD_CUT_DOWN].ini, g_keys[KA_LOD_CUT_UP].ini);
    {
        int dup = 0;
        for (int i = 0; i < KA_COUNT; ++i)
            for (int j = i + 1; j < KA_COUNT; ++j)
                if (g_keys[i].vk && g_keys[i].vk == g_keys[j].vk && g_keys[i].mods == g_keys[j].mods) ++dup;
        CHECK(dup == 0, "default bindings: %d shared combos", dup);
    }

    // ---- 24. dlaa.ini: parse (LoadConfig on a temporary dlaa.ini next to this exe) + the save round trip -------------------------
    {
        wchar_t ini[MAX_PATH];
        CHECK(PathNextToDll(L"dlaa.ini", ini), "dlaa.ini path");
        const DWORD attr = GetFileAttributesW(ini);
        if (attr != INVALID_FILE_ATTRIBUTES) {
            printf("SKIP section 24: a dlaa.ini already exists next to the test exe\n");
        } else {
            struct Case { const char* text; int mode; float value; };
            const Case cases[] = {
                { "tex_lod_bias_cutout = -0.75\n", kLodCutValue, -0.75f },
                { "tex_lod_bias_cutout=same\n", kLodCutSame, -0.5f },
                { "  tex_lod_bias_cutout = SAME ; the phase-12 rule\n", kLodCutSame, -0.5f },
                { "tex_lod_bias_cutout = -5\n", kLodCutValue, -3.0f },
                { "tex_lod_bias_cutout = 2\n", kLodCutValue, 1.0f },
                { "tex_lod_bias_cutout = 0\n", kLodCutValue, 0.0f },
                { "tex_lod_bias_cutout = abc\n", kLodCutAbsent, -0.5f },     // not recognised: the default stays
                { "tex_lod_bias_cutout = -0.5x\n", kLodCutAbsent, -0.5f },
                { "tex_lod_bias = -1\n", kLodCutAbsent, -0.5f },             // key absent
            };
            for (const Case& c : cases) {
                FILE* f = nullptr;
                if (_wfopen_s(&f, ini, L"wb") != 0 || !f) { CHECK(false, "cannot write dlaa.ini"); break; }
                fputs(c.text, f);
                fclose(f);
                g_lodCutMode = kLodCutAbsent; g_lodCutBias = -0.5f;
                LoadConfig();
                CHECK(g_lodCutMode == c.mode && g_lodCutBias == c.value, "ini '%.*s': mode %d value %.2f (want %d / %.2f)",
                      (int)strcspn(c.text, "\n"), c.text, g_lodCutMode, g_lodCutBias, c.mode, c.value);
            }
            // save -> load: the text SaveSettings writes (LodCutoutText; SaveSettings / WriteIniPreserving are WITH_DLAA-only)
            for (int k = 0; k < 2; ++k) {
                g_lodScope = kLodScopeOpaque;
                g_lodCutMode = k == 0 ? kLodCutValue : kLodCutSame; g_lodCutBias = -1.25f;
                char val[32];
                LodCutoutText(val, sizeof(val));
                FILE* f = nullptr;
                if (_wfopen_s(&f, ini, L"wb") != 0 || !f) { CHECK(false, "cannot write dlaa.ini"); break; }
                fprintf(f, "tex_lod_bias_cutout = %s\r\n", val);
                fclose(f);
                g_lodCutMode = kLodCutAbsent; g_lodCutBias = -0.5f;
                LoadConfig();
                CHECK(k == 0 ? (g_lodCutMode == kLodCutValue && g_lodCutBias == -1.25f) : g_lodCutMode == kLodCutSame,
                      "save round trip %d: mode %d value %.2f", k, g_lodCutMode, g_lodCutBias);
            }
            _wremove(ini);
        }
    }

    psPlain->Release(); psClip->Release(); psBranch->Release(); if (psUnknown) psUnknown->Release();
    bPlain->Release(); bClip->Release(); bBranch->Release(); bIcb->Release();

    printf("%s: %d / %d checks passed\n", s_fails ? "FAILED" : "PASSED", s_checks - s_fails, s_checks);
    return s_fails ? 1 : 0;
}
