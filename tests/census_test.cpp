// phase 108 unit test: the shader census's pure parts (src/census.h) -- the DXBC classifier, the per-pair verdict with the mod's
// known shader lists (src/wire_widen.cpp's checksum sets, src/water_ps.h), the thin rule, the texts and the DDS header.
//
// 1. SYNTHETIC (CI): fxc look-alikes compiled here with D3DCompile -- a pixel-reader (interpolant / w * cb -> int -> Load), the same
//    read from SV_Position (NOT a reader), the phase-97 parity reader (a reader, covered by water_ps), an alpha-test cut-out (slot
//    found), a dither discard (NOT a cut-out), a water-like PS (projective sample + pixel read), a plain PS (nothing).
// 2. REAL GAME BYTECODE (local only, skipped when absent: E:\ETS2-DLAA-Injector\captures\census_2026-11-14\ and captures\*.dxbc,
//    RenderDoc dumps of ETS2 / ATS 2026-10 .. 11): the ten audited pixel-readers (9a5fca98, 9c062880, b0852ebe, 1a104d6e,
//    a5aa1c2d, 0a3c1386, 079791a9, 19f12dc2, 31586e4b, fd5e16e5) classify pixel-reader; every known wire / solid pair (wire_widen's
//    VS + PS sets) has its thinness covered and no unknown-VS kind, the solid pairs are not suspects at all (pair D's cut-out only
//    while its alpha texture is not patched); the lamp VS ccd6b6d1 with the solid PS b67da69f = unknown-VS; every game PS parses.
// Optional: census_test.exe <folder> prints the classification of every *.dxbc in it. Exit code 0 = all checks passed.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
#include <array>
#include "census.h"
#include "census_probe.h"
#include "census_paint.h"
#include "pixel_reader.h"
#include "wire_widen.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const char* kShaders = R"(
cbuffer C : register(b0) { float4 c[28]; };
Texture2D t0 : register(t0);
Texture2D t6 : register(t6);
Texture2D t1 : register(t1);
Texture2D t2 : register(t2);
Texture2D<float4> g0 : register(t10);
Texture2D<float4> g1 : register(t12);
Texture2D<uint4>  g3 : register(t15);
SamplerState s0 : register(s0);

// the audited forward shape (9a5fca98 / 9c062880 ...): pixel = v5.yz / v5.x * cb * (cb / cb) -> ftoi -> ld
float4 Reader(float4 pos : SV_Position, float3 v5 : TEXCOORD5, float3 n : TEXCOORD4) : SV_Target {
    float2 pix = v5.yz / v5.x * c[23].xy * (c[24].xy / c[24].zw);
    int2 ip = int2(pix);
    float4 a = g0.Load(int3(ip, 0));
    float4 b = g1.Load(int3(ip, 0));
    return float4(a.xyz * b.xyz + n, 1);
}
// the same G-buffer read from SV_Position: not a pixel-reader (SV_Position is the rasterised pixel)
float4 PosReader(float4 pos : SV_Position, float3 n : TEXCOORD4) : SV_Target {
    float4 a = g0.Load(int3(int2(pos.xy), 0));
    return float4(a.xyz + n, 1);
}
// phase 97's parity reader (185d5b7a look-alike): a pixel-reader with the parity (covered by water_ps)
float4 Parity(float4 pos : SV_Position, float3 v7 : TEXCOORD7, float3 n : TEXCOORD4) : SV_Target {
    float2 sp = v7.yz / v7.x;
    float2 pix = sp * c[23].xy * (c[24].xy / c[24].zw);
    uint par = (uint(pix.x) + uint(pix.y) + uint(c[27].x)) & 1;
    float2 offW = par ? float2(0, 0) : float2(c[27].y, 0);
    int2 cw = int2((sp * c[23].xy + offW) * (c[24].xy / c[24].zw));
    float4 water = g0.Load(int3(cw, 0));
    uint flag = g3.Load(int3(cw, 0)).z;
    return float4(lerp(n, water.xyz, (flag & 16) ? 1.0 : 0.0), 1);
}
// alpha test on t6 (the railings' PS 2d7ed0e6 shape)
float4 Cutout(float4 pos : SV_Position, float2 uv : TEXCOORD0, float3 n : TEXCOORD1) : SV_Target {
    float4 a = t6.Sample(s0, uv);
    clip(a.a - 0.05);
    return float4(a.rgb * n, 1);
}
// phase 112r: colour t0 + normal map t2 + the alpha mask t1 -- only t1 reaches the discard (Cutout::OnDraw's slot mask: the colour /
// normal textures of a fence draw are not looked at)
float4 Cutout3(float4 pos : SV_Position, float2 uv : TEXCOORD0, float3 n : TEXCOORD1) : SV_Target {
    float4 col = t0.Sample(s0, uv);
    float3 nm = t2.Sample(s0, uv).xyz * 2 - 1;
    float a = t1.Sample(s0, uv * 0.5).a;
    clip(a - 0.5);
    return float4(col.rgb * dot(nm, n), 1);
}
// a screen dither discard (no texture): not a cut-out
float4 Dither(float4 pos : SV_Position, float3 n : TEXCOORD1) : SV_Target {
    if (frac(pos.x * 0.5 + pos.y * 0.5) < c[2].x) discard;
    return float4(n, 1);
}
// water-like: a projective reflection lookup + the G-buffer under the pixel
float4 Water(float4 pos : SV_Position, float3 v5 : TEXCOORD5, float4 r : TEXCOORD2, float3 n : TEXCOORD4) : SV_Target {
    float2 pix = v5.yz / v5.x * c[23].xy;
    float4 bed = g0.Load(int3(int2(pix), 0));
    float2 ruv = r.xy / r.w;
    float3 refl = t0.Sample(s0, ruv).rgb;
    return float4(lerp(bed.rgb, refl, 0.5) + n, 1);
}
// nothing: a lit textured surface
float4 Plain(float4 pos : SV_Position, float2 uv : TEXCOORD0, float3 n : TEXCOORD1) : SV_Target {
    return float4(t0.Sample(s0, uv * c[3].xy + c[3].zw).rgb * saturate(dot(n, c[4].xyz)), 1);
}
)";

static std::vector<uint8_t> Compile(const char* entry) {
    ID3DBlob* b = nullptr; ID3DBlob* e = nullptr;
    if (FAILED(D3DCompile(kShaders, std::strlen(kShaders), "census_test", nullptr, nullptr, entry, "ps_5_0", 0, 0, &b, &e))) {
        std::printf("compile %s: %s\n", entry, e ? (const char*)e->GetBufferPointer() : "?");
        std::exit(2);
    }
    if (e) e->Release();
    std::vector<uint8_t> v((const uint8_t*)b->GetBufferPointer(), (const uint8_t*)b->GetBufferPointer() + b->GetBufferSize());
    b->Release();
    return v;
}
static bool ReadFile(const std::string& p, std::vector<uint8_t>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, p.c_str(), "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    const bool ok = n > 0 && fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    return ok;
}

// the pair facts as census_run.h fills them (WireWiden's sets with these fake object pointers)
static int SolidPsByCode(const std::vector<uint8_t>& c) {
    return WireWiden::SolidPsKindOfCode(c.data(), c.size());   // (phase 109d / e: kinds A .. H, as census_run.h)
}
static Census::Verdict JudgePair(const std::vector<uint8_t>& vs, const std::vector<uint8_t>& ps, bool thin, int cutReason) {
    static uintptr_t s_next = 0x100000;
    void* vsObj = (void*)(s_next += 64);
    void* psObj = (void*)(s_next += 64);
    WireWiden::NoteVs(vsObj, vs.data(), vs.size());
    WireWiden::NotePs(psObj, ps.data(), ps.size());
    const Census::PsScan sc = Census::ScanPs(ps.data(), ps.size());
    Census::Cover c;
    c.waterRule = WaterPs::Classify(ps.data(), ps.size()).rule;
    c.pixelFix = (sc.kinds & Census::kPixel) && PixelReader::Make(ps.data(), ps.size()).why == PixelReader::kOk;   // as census_run.h
    c.wirePsKind = WireWiden::PsKind(ps.data(), ps.size());
    c.wireVs = c.wirePsKind && WireWiden::IsWireVs(vsObj);
    c.solidKind = SolidPsByCode(ps);
    c.solidVs = c.solidKind && WireWiden::IsSolidVs(vsObj, c.solidKind);
    c.gameWire = WireWiden::ChecksumIs(ps.data(), ps.size(), WireWiden::kGameWirePs) && WireWiden::IsGameWireVs(vsObj);   // phase 109b
    c.cutReason = cutReason;
    return Census::Judge(sc.kinds, c, thin);
}

static void Synthetic() {
    using namespace Census;
    std::printf("1. synthetic shaders\n");
    {
        const auto b = Compile("Reader");
        const PsScan s = ScanPs(b.data(), b.size());
        CHECK(s.parsed, "Reader parsed");
        CHECK(s.kinds == kPixel, "Reader kinds %s", KindList(s.kinds).c_str());
        CHECK(s.pixelTex == ((1u << 10) | (1u << 12)), "Reader pixel slots 0x%x", s.pixelTex);
        CHECK(!s.parity, "Reader parity");
        CHECK(WaterPs::Classify(b.data(), b.size()).rule == 0, "Reader is not a phase-97 shader");
        Cover c;
        const Verdict v = Judge(s.kinds, c, false);
        CHECK(v.Suspect() == kPixel, "Reader suspect %s", KindList(v.Suspect()).c_str());
        // phase 109a: the look-alike's index chain is the audited one -> a twin -> the pixel read is KNOWN
        c.pixelFix = PixelReader::Make(b.data(), b.size()).why == PixelReader::kOk;
        CHECK(c.pixelFix, "Reader: phase 109a twin");
        CHECK(Judge(s.kinds, c, false).Suspect() == 0, "Reader with its twin: covered (suspect %s)", KindList(Judge(s.kinds, c, false).Suspect()).c_str());
    }
    {
        const auto b = Compile("PosReader");
        const PsScan s = ScanPs(b.data(), b.size());
        CHECK(s.parsed && s.kinds == 0 && s.pixelLds == 0, "PosReader kinds %s lds %d", KindList(s.kinds).c_str(), s.pixelLds);
    }
    {
        const auto b = Compile("Parity");
        const PsScan s = ScanPs(b.data(), b.size());
        const int rule = WaterPs::Classify(b.data(), b.size()).rule;
        CHECK((s.kinds & kPixel) && s.parity, "Parity reader: kinds %s parity %d", KindList(s.kinds).c_str(), (int)s.parity);
        CHECK(rule == WaterPs::kRuleCheckerRead, "Parity reader water rule %d", rule);
        Cover c; c.waterRule = rule;
        const Verdict v = Judge(s.kinds, c, false);
        CHECK(v.Suspect() == 0 && (v.covered & kPixel), "Parity reader covered (suspect %s)", KindList(v.Suspect()).c_str());
    }
    {
        const auto b = Compile("Cutout");
        const PsScan s = ScanPs(b.data(), b.size());
        CHECK(s.kinds == kCutout && s.discardTex == (1u << 6) && s.discards == 1, "Cutout kinds %s slots 0x%x", KindList(s.kinds).c_str(), s.discardTex);
        // phase 112r: the slot mask Cutout::OnDraw fetches -- colour t0 / normal t2 are NOT in it, the alpha mask t1 is
        const auto b3 = Compile("Cutout3");
        const PsScan s3 = ScanPs(b3.data(), b3.size());
        CHECK((s3.kinds & kCutout) && s3.discardTex == (1u << 1) && (s3.texDecl & 7u) == 7u,
              "Cutout3 (colour t0, normal t2, alpha t1): discard slots 0x%x (want 0x2), declared 0x%x", s3.discardTex, s3.texDecl);
        Cover c;
        for (int r = 0; r < kCutCount; ++r) {
            c.cutReason = r;
            // phase 110b: handled / pending / the game's mips keep coverage = not a suspect; only unhandled or sparse alphas are
            const bool cov = r == kCutPatched || r == kCutApplied || r == kCutNoLoss || r == kCutSoft || r == kCutCoverage ||
                             r == kCutQueue || r == kCutPending || r == kCutNoTex || r == kCutLate ||   // (113: the GPU read-back = WAIT)
                             r == kCutOneMip || r == kCutRenderTarget || r == kCutOpaque;   // (114)
            CHECK((Judge(s.kinds, c, false).Suspect() == 0) == cov, "Cutout reason %d (%s) covered %d", r, CutReasonName(r), (int)cov);
        }
    }
    {
        const auto b = Compile("Dither");
        const PsScan s = ScanPs(b.data(), b.size());
        CHECK(s.parsed && s.discards == 1 && s.kinds == 0, "Dither: discards %d kinds %s", s.discards, KindList(s.kinds).c_str());
    }
    {
        const auto b = Compile("Water");
        const PsScan s = ScanPs(b.data(), b.size());
        CHECK(s.kinds == (kPixel | kWater) && s.projSamples >= 1, "Water kinds %s proj %d", KindList(s.kinds).c_str(), s.projSamples);
    }
    {
        const auto b = Compile("Plain");
        const PsScan s = ScanPs(b.data(), b.size());
        CHECK(s.parsed && s.kinds == 0 && s.projSamples == 0, "Plain kinds %s proj %d scroll %d", KindList(s.kinds).c_str(), s.projSamples,
              s.scrollSamples);
        CHECK(s.texDecl == 1u, "Plain declared textures 0x%x", s.texDecl);
    }
    // garbage / truncated / a VS
    {
        const auto b = Compile("Reader");
        for (size_t cut : { (size_t)0, (size_t)16, (size_t)40, b.size() / 2 }) {
            const PsScan s = ScanPs(b.data(), cut);
            CHECK(!s.parsed || s.kinds == 0 || cut == b.size(), "truncated %zu parsed %d", cut, (int)s.parsed);
        }
        std::vector<uint8_t> junk(4096);
        for (size_t i = 0; i < junk.size(); ++i) junk[i] = (uint8_t)(i * 2654435761u >> 13);
        CHECK(!ScanPs(junk.data(), junk.size()).parsed, "garbage parsed");
        ID3DBlob* vb = nullptr; ID3DBlob* e = nullptr;
        const char* vsrc = "float4 main(float4 p : POSITION) : SV_Position { return p; }";
        if (SUCCEEDED(D3DCompile(vsrc, strlen(vsrc), "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vb, &e))) {
            CHECK(!ScanPs(vb->GetBufferPointer(), vb->GetBufferSize()).parsed, "a VS parsed as a PS");
            vb->Release();
        }
        if (e) e->Release();
    }
    // the verdict rules
    {
        Cover c;
        c.wirePsKind = 1; c.wireVs = false;
        CHECK(Judge(0, c, true).Suspect() == (kVs | kThin), "wire PS + other VS + thin = unknown VS (+ thin)");
        CHECK(Judge(0, c, false).Suspect() == 0, "phase 113b: wire PS + other VS, NOT thin = nothing (unknown-VS needs a confirmed thin draw)");
        c.wireVs = true;
        CHECK(Judge(0, c, true).Suspect() == 0, "wire pair thin covered");
        Cover d;
        d.solidKind = 1; d.solidVs = false;
        CHECK(Judge(0, d, true).Suspect() == (kVs | kThin), "solid PS + other VS thin = %s", KindList(Judge(0, d, true).Suspect()).c_str());
        Cover e;
        CHECK(Judge(kWater, e, false).Suspect() == kWater, "water-like suspect");
        e.waterRule = 2;
        CHECK(Judge(kWater | kPixel, e, false).Suspect() == 0, "water rule covers pixel + water");
        Cover f;                                                  // phase 109c: the VRS-only rule fixes no read
        f.waterRule = WaterPs::kRuleVrsOnly;
        CHECK(Judge(kWater | kPixel, f, false).Suspect() == (kWater | kPixel), "VRS-only water rule covers nothing");
        Cover g;                                                  // phase 109a: a twin covers the pixel read (and the water-like ld)
        g.pixelFix = true;
        CHECK(Judge(kWater | kPixel | kCutout, g, false).Suspect() == kCutout, "pixel-reader twin covers pixel + water-like");
        CHECK(Judge(kVs, Cover(), false).kinds == 0, "kVs is never structural");
        Cover h; h.gameWire = true;                               // phase 109b: the power-line pair's thinness is KNOWN
        CHECK(Judge(0, h, true).Suspect() == 0, "game-widened power line: thin covered");
    }
    // thin rule, impact, texts
    CHECK(!ThinVerdict(10, 1000, 2), "thin needs 3 samples");
    CHECK(ThinVerdict(100, 1000, 3), "0.1 px / triangle = thin");
    CHECK(!ThinVerdict(600, 1000, 5), "0.6 px / triangle = not thin");
    CHECK(Impact(kPixel, 100, 0, 100) > Impact(kCutout, 100, 0, 100), "pixel-reader weighs more than cut-out");
    CHECK(Impact(kThin, 1000, 0, 0) > Impact(kThin, 10, 0, 0), "more frames = more impact");
    CHECK(Impact(0, 1000, 5000, 9) == 0.0, "no suspect = no impact");
    {
        const uint32_t s[4] = { 0x9a5fca98u, 0x48aeb8a1u, 0x0f1346bau, 0x64390c54u };
        CHECK(ConstText(s) == "{ 0x9a5fca98u, 0x48aeb8a1u, 0x0f1346bau, 0x64390c54u }", "ConstText %s", ConstText(s).c_str());
        CHECK(Short(s) == "9a5fca98", "Short");
        CHECK(KindList(kPixel | kVs) == "pixel-reader, unknown-VS", "KindList %s", KindList(kPixel | kVs).c_str());
        CHECK(TintKind(kThin | kWater) == 2 && TintKind(kVs) == 4 && TintKind(0) == -1, "TintKind");
    }
    // DDS header + row sizes
    {
        uint32_t rb, rows;
        MipRows(77 /* BC3_UNORM */, 128, 2048, &rb, &rows);
        CHECK(rb == 32 * 16 && rows == 512, "BC3 128x2048 rows %u x %u", rb, rows);
        MipRows(77, 2, 1, &rb, &rows);
        CHECK(rb == 16 && rows == 1, "BC3 2x1 = one block (%u x %u)", rb, rows);
        MipRows(28 /* RGBA8 */, 100, 7, &rb, &rows);
        CHECK(rb == 400 && rows == 7, "RGBA8 rows");
        CHECK(FormatInfo(10).bytes == 8 && FormatInfo(98).block == 4 && FormatInfo(0).bytes == 0, "FormatInfo");
        const std::vector<uint8_t> h = DdsHeader(77, 128, 2048, 12, 1, false);
        uint32_t v[37];
        memcpy(v, h.data(), sizeof(v));
        CHECK(h.size() == 148 && v[0] == 0x20534444u && v[1] == 124 && v[3] == 2048 && v[4] == 128 && v[7] == 12, "DDS header basics");
        CHECK(v[21] == 0x30315844u && v[32] == 77 && v[33] == 3 && v[35] == 1, "DDS DX10 part");
        const std::vector<uint8_t> hc = DdsHeader(28, 64, 64, 1, 6, true);
        memcpy(v, hc.data(), sizeof(v));
        CHECK(v[34] == 4 && v[35] == 1 && v[28] == 0xFE00u, "DDS cube");
    }
    CHECK(std::string(CutReasonName(kCutCount - 1)).size() > 3 && std::string(CutReasonName(kCutCount)) == "?", "cut reason names");
}

static std::string g_capDir = "E:\\ETS2-DLAA-Injector\\captures\\";
static bool Load(const char* name, std::vector<uint8_t>& out) {
    if (ReadFile(g_capDir + "census_2026-11-14\\" + name, out)) return true;
    return ReadFile(g_capDir + name, out);
}

static void RealBytecode() {
    using namespace Census;
    std::vector<uint8_t> probe;
    if (!Load("ps_9a5fca98.dxbc", probe)) {
        std::printf("2. real game bytecode: SKIPPED (no %scensus_2026-11-14\\ -- local game data, not in git)\n", g_capDir.c_str());
        return;
    }
    std::printf("2. real game bytecode (%s)\n", g_capDir.c_str());
    static const char* const kReaders[] = { "9a5fca98", "9c062880", "b0852ebe", "1a104d6e", "a5aa1c2d", "0a3c1386", "079791a9",
                                            "19f12dc2", "31586e4b", "fd5e16e5" };
    for (const char* h : kReaders) {
        std::vector<uint8_t> b;
        const std::string nm = std::string("ps_") + h + ".dxbc";
        if (!Load(nm.c_str(), b)) { CHECK(false, "%s missing", nm.c_str()); continue; }
        const PsScan s = ScanPs(b.data(), b.size());
        CHECK(s.parsed && (s.kinds & kPixel) && s.pixelTex != 0, "%s: pixel-reader expected, kinds %s", h, KindList(s.kinds).c_str());
        CHECK(WaterPs::Classify(b.data(), b.size()).rule == 0, "%s is not a phase-97 shader", h);
        const PixelReader::Twin t = PixelReader::Make(b.data(), b.size());   // phase 109a: every audited reader gets its twin
        CHECK(t.why == PixelReader::kOk, "%s: phase 109a twin expected (%s)", h, PixelReader::WhyName(t.why));
        Cover c; c.pixelFix = t.why == PixelReader::kOk;
        CHECK(!(Judge(s.kinds, c, false).Suspect() & kPixel), "%s: the pixel read is KNOWN with the twin", h);
    }
    // the old SCS water forward PS is water-like too
    {
        std::vector<uint8_t> b;
        Load("ps_9a5fca98.dxbc", b);
        CHECK(ScanPs(b.data(), b.size()).kinds & kWater, "9a5fca98 water-like");
    }
    // the known pairs (wire_widen's VS + PS sets): thinness covered, no unknown VS; the expected leftover kinds
    struct KP { const char* vs; const char* ps; uint32_t left; uint32_t leftPatched; };
    static const KP kKnown[] = {
        { "vs_4186dac5", "ps_a5aa1c2d", 0, 0 },                       // long wires: the pixel read is phase 109a's twin
        { "vs_4186dac5", "ps_1a104d6e", 0, 0 },
        { "vs_4186dac5", "ps_b97c44a3", kCutout, 0 },                 // pole hardware: its alpha test (pixel read: 109a twin)
        { "vs_07dfaa00", "ps_b67da69f", 0, 0 },                       // pair A
        { "vs_6c157c5f", "ps_34860eed", 0, 0 },                       // pair B
        { "vs_a484ef85", "ps_34860eed", 0, 0 },                       // pair B (instanced street lights)
        { "vs_07dfaa00", "ps_57a4e1f1", 0, 0 },                       // pair C
        { "vs_3847be86", "ps_2d7ed0e6", kCutout, 0 },                 // pair D (alpha test on t6)
        { "vs_9d2904cd", "ps_2d7ed0e6", kCutout, 0 },
        { "vs_d87f5280", "ps_2d7ed0e6", kCutout, 0 },
        { "vs_d87f5280", "ps_d3af2ebb", 0, 0 },                       // pair E
        { "vs_277d2302", "ps_69dcaf36", 0, 0 },                       // phase 109b: the game-widened power lines
    };
    int known = 0;
    for (const KP& k : kKnown) {
        std::vector<uint8_t> vs, ps;
        if (!Load((std::string(k.vs) + ".dxbc").c_str(), vs) || !Load((std::string(k.ps) + ".dxbc").c_str(), ps)) {
            CHECK(false, "%s / %s missing", k.vs, k.ps);
            continue;
        }
        ++known;
        for (int patched = 0; patched < 2; ++patched)
            for (int thin = 0; thin < 2; ++thin) {
                const Verdict v = JudgePair(vs, ps, thin != 0, patched ? kCutApplied : kCutOff);
                CHECK(!(v.Suspect() & (kThin | kVs)), "%s + %s: thin / VS must be covered (suspect %s)", k.vs, k.ps, KindList(v.Suspect()).c_str());
                const uint32_t want = patched ? k.leftPatched : k.left;
                CHECK(v.Suspect() == want, "%s + %s (thin %d, alpha %s): suspect %s, want %s", k.vs, k.ps, thin, patched ? "patched" : "off",
                      KindList(v.Suspect()).c_str(), KindList(want).c_str());
            }
    }
    std::printf("   known pairs checked: %d\n", known);
    // the lamp VS with pair A's PS (found 2026-11-14 as an unknown VS): phase 109e put ccd6b6d1 into pair A's VS set -> no suspect,
    // its thinness covered; pair F (e5a4956d + 2cc0a6fa) and kind G (07dfaa00 + cfc52d43) likewise
    {
        std::vector<uint8_t> vs, ps;
        if (Load("vs_ccd6b6d1.dxbc", vs) && Load("ps_b67da69f.dxbc", ps)) {
            const Verdict v = JudgePair(vs, ps, true, kCutOff);
            CHECK(v.Suspect() == 0 && (v.covered & kThin), "ccd6b6d1 + b67da69f: KNOWN since phase 109e, got suspect %s",
                  KindList(v.Suspect()).c_str());
        } else CHECK(false, "vs_ccd6b6d1 / ps_b67da69f missing");
        std::vector<uint8_t> vf, pf, va, pg;
        if (Load("vs_e5a4956d.dxbc", vf) && Load("ps_2cc0a6fa.dxbc", pf) && Load("vs_07dfaa00.dxbc", va) && Load("ps_cfc52d43.dxbc", pg)) {
            const Verdict f = JudgePair(vf, pf, true, kCutOff), g = JudgePair(va, pg, true, kCutApplied);
            CHECK(f.Suspect() == 0, "pair F e5a4956d + 2cc0a6fa thin: KNOWN (phase 109e), got %s", KindList(f.Suspect()).c_str());
            CHECK(g.Suspect() == 0, "kind G 07dfaa00 + cfc52d43 thin (alpha patched): KNOWN (phase 109e), got %s", KindList(g.Suspect()).c_str());
        }
    }
    // every game PS of the corpus parses; count the kinds
    WIN32_FIND_DATAA fd;
    int n = 0, parsed = 0, kinds[kKindN] = {};
    for (const std::string dir : { g_capDir + "census_2026-11-14\\", g_capDir }) {
        HANDLE h = FindFirstFileA((dir + "ps_*.dxbc").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::vector<uint8_t> b;
            if (!ReadFile(dir + fd.cFileName, b)) continue;
            ++n;
            const PsScan s = ScanPs(b.data(), b.size());
            if (s.parsed) ++parsed;
            else CHECK(false, "%s did not parse", fd.cFileName);
            for (int k = 0; k < kKindN; ++k) if (s.kinds & (1u << k)) ++kinds[k];
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::printf("   corpus: %d game pixel shaders, %d parsed; pixel-reader %d, cut-out %d, water-like %d\n", n, parsed, kinds[0], kinds[1],
                kinds[3]);
    // the CreatePixelShader-time cost of one scan (informational: once per distinct bytecode, on the game's loader thread)
    {
        std::vector<uint8_t> b;
        if (Load("ps_9c062880.dxbc", b)) {
            LARGE_INTEGER f, t0, t1;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&t0);
            uint32_t acc = 0;
            for (int i = 0; i < 2000; ++i) acc += ScanPs(b.data(), b.size()).kinds;
            QueryPerformanceCounter(&t1);
            std::printf("   ScanPs: %.2f us per call (9c062880, %zu bytes; %u)\n",
                        1e6 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart / 2000.0, b.size(), acc & 1u);
        }
    }
}

static void ScanFolder(const char* dir) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((std::string(dir) + "\\*.dxbc").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { std::printf("no *.dxbc in %s\n", dir); return; }
    do {
        std::vector<uint8_t> b;
        if (!ReadFile(std::string(dir) + "\\" + fd.cFileName, b)) continue;
        const Census::PsScan s = Census::ScanPs(b.data(), b.size());
        if (!s.parsed) continue;
        std::printf("%-44s %08x %-32s ld@pixel %d (0x%x) proj %d scroll %d discard %d (0x%x) parity %d water_ps %d\n", fd.cFileName, s.sum[0],
                    Census::KindList(s.kinds).c_str(), s.pixelLds, s.pixelTex, s.projSamples, s.scrollSamples, s.discards, s.discardTex,
                    (int)s.parity, WaterPs::Classify(b.data(), b.size()).rule);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// ---- phase 110: the capture part's pure helpers + the create-time cost of the always-on classification ----------------------
static void Phase110() {
    using namespace Census;
    // WhyText: one line per SUSPECT kind, none for covered kinds
    {
        Cover c; c.waterRule = 0; c.cutReason = kCutSparse;    // phase 110b: soft alpha is KNOWN now; sparse is a real suspect
        PsScan s; s.pixelTex = 0x1400; s.pixelLds = 2; s.discards = 1; s.discardTex = 0x40;
        const Verdict v = Judge(kPixel | kCutout, c, false);
        const std::string w = WhyText(v, c, s, 0.0, 0);
        CHECK(w.find("why pixel-reader") != std::string::npos && w.find("0x1400") != std::string::npos, "WhyText pixel-reader line");
        CHECK(w.find("why cut-out") != std::string::npos && w.find("coverage under 2 %") != std::string::npos, "WhyText cut-out line + reason");
        CHECK(w.find("why thin") == std::string::npos && w.find("why water") == std::string::npos, "WhyText: no lines for absent kinds");
        Cover cp = c; cp.cutReason = kCutPatched;          // covered cut-out: no line for it
        const std::string w2 = WhyText(Judge(kPixel | kCutout, cp, false), cp, s, 0.0, 0);
        CHECK(w2.find("why cut-out") == std::string::npos && w2.find("why pixel-reader") != std::string::npos, "WhyText skips a covered kind");
        Cover ct; ct.wirePsKind = 1; ct.wireVs = false;    // a wire PS with a foreign VS: unknown-VS (+ thin: not a wire pair)
        const std::string w3 = WhyText(Judge(0, ct, true), ct, PsScan{}, 0.21, 5);
        CHECK(w3.find("why unknown-VS") != std::string::npos && w3.find("why thin") != std::string::npos &&
              w3.find("0.21 px per triangle over 5") != std::string::npos, "WhyText thin + unknown-VS: %s", w3.c_str());
        Cover cw; cw.wirePsKind = 1; cw.wireVs = true;     // a known wire pair: thin covered -> nothing suspect
        CHECK(WhyText(Judge(0, cw, true), cw, PsScan{}, 0.2, 5).find("no suspect kind") != std::string::npos, "WhyText: all covered");
    }
    // CapDecide: link an earlier dump still on disk, else dump within budget, else text only
    CHECK(CapDecide(false, false, 0, 48, true) == kCapDump, "CapDecide: new pair -> dump");
    CHECK(CapDecide(true, true, 0, 48, true) == kCapLink, "CapDecide: dumped earlier, on disk -> link");
    CHECK(CapDecide(true, true, 48, 48, false) == kCapLink, "CapDecide: a link needs no budget");
    CHECK(CapDecide(true, false, 0, 48, true) == kCapDump, "CapDecide: dumped earlier but deleted (capture_keep) -> dump again");
    CHECK(CapDecide(false, false, 48, 48, true) == kCapTextOnly, "CapDecide: 48 pairs dumped -> text only");
    CHECK(CapDecide(false, false, 3, 48, false) == kCapTextOnly, "CapDecide: disk budget spent -> text only");
    CHECK(std::string(CapActionName(kCapLink)).find("linked") == 0 && std::string(CapActionName(99)) == "-", "CapActionName");
    // CapPictureOrder: biggest coverage first, then impact, then draws; capped
    {
        std::vector<CapRank> r = { { 0, 100, 5.0, 3 }, { 1, 900, 1.0, 1 }, { 2, 100, 9.0, 1 }, { 3, 0, 50.0, 9 }, { 4, 100, 9.0, 7 } };
        const std::vector<int> o = CapPictureOrder(r, 16);
        CHECK(o.size() == 5 && o[0] == 1 && o[1] == 4 && o[2] == 2 && o[3] == 0 && o[4] == 3, "CapPictureOrder: px, impact, draws");
        const std::vector<int> o2 = CapPictureOrder(r, 2);
        CHECK(o2.size() == 2 && o2[0] == 1 && o2[1] == 4, "CapPictureOrder: capped at maxPictures");
        CHECK(CapPictureOrder({}, 16).empty(), "CapPictureOrder: none");
    }
    // CapIndexText: every row listed with its folder, kinds, coverage, action, link, picture, why, textures
    {
        CapRow a; a.folder = "9c062880_1a104d6e"; a.vs = "{ 0x9c062880u, 1, 2, 3 }"; a.ps = "{ 0x1a104d6eu, 4, 5, 6 }";
        a.kinds = "pixel-reader"; a.action = kCapDump; a.px = 12345; a.pxQueries = 3; a.draws = 3; a.picture = "picture.png (x)";
        a.why = "  why pixel-reader: test\n"; a.tex = "  t0: saved\n";
        CapRow b; b.folder = "novs_b67da69f"; b.ps = "{ 0xb67da69fu, 0, 0, 0 }"; b.kinds = "unknown-VS"; b.action = kCapLink;
        b.link = "dlaa_census\\novs_b67da69f\\"; b.picture = "not taken";
        const std::string t = CapIndexText("# head\n", { a, b });
        CHECK(t.compare(0, 7, "# head\n") == 0, "CapIndexText: head first");
        CHECK(t.find("== 1  9c062880_1a104d6e\\") != std::string::npos && t.find("12345 px over 3 occlusion queries") != std::string::npos &&
              t.find("dumped here") != std::string::npos && t.find("why pixel-reader: test") != std::string::npos &&
              t.find("  textures:\n  t0: saved") != std::string::npos, "CapIndexText: row 1");
        CHECK(t.find("== 2  novs_b67da69f\\") != std::string::npos && t.find("created before the mod's hooks") != std::string::npos &&
              t.find("linked (dumped earlier) -> dlaa_census\\novs_b67da69f\\") != std::string::npos &&
              t.find("(no coverage measured)") != std::string::npos, "CapIndexText: row 2 (link, no VS, no coverage)");
        CHECK(CapIndexText("# h\n", {}).find("no suspect pair was drawn") != std::string::npos, "CapIndexText: empty capture says so");
    }
    // the create-time cost of the census's classification, now done for EVERY created shader (phase 110: the row can switch the
    // census on live): per distinct bytecode, on the game's loader thread -- informational, over the local game corpus
    {
        std::vector<std::vector<uint8_t>> corpus;
        for (const std::string dir : { g_capDir + "census_2026-11-14\\", g_capDir }) {
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA((dir + "*.dxbc").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do { std::vector<uint8_t> b; if (ReadFile(dir + fd.cFileName, b)) corpus.push_back(std::move(b)); } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        if (corpus.empty()) { std::printf("   phase 110 create-time cost: SKIPPED (no local game bytecode)\n"); return; }
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        size_t bytes = 0;
        uint32_t acc = 0;
        const int reps = 50;
        QueryPerformanceCounter(&t0);
        for (int r = 0; r < reps; ++r)
            for (const std::vector<uint8_t>& b : corpus) {
                uint32_t sum[4];
                acc += Checksum(b.data(), b.size(), sum) ? sum[0] & 1u : 0u;
                const PsScan s = ScanPs(b.data(), b.size());
                acc += s.kinds + (uint32_t)WaterPs::Classify(b.data(), b.size()).rule;
                if (s.kinds & kPixel) acc += PixelReader::Make(b.data(), b.size()).why;
                acc += (uint32_t)WireWiden::PsKind(b.data(), b.size()) + (uint32_t)WireWiden::SolidPsKindOfCode(b.data(), b.size());
                acc += WireWiden::ChecksumIs(b.data(), b.size(), WireWiden::kGameWirePs) ? 1u : 0u;
                if (r == 0) bytes += b.size();
            }
        QueryPerformanceCounter(&t1);
        const double us = 1e6 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart / (double)(reps * corpus.size());
        std::printf("   phase 110 create-time classification: %.2f us per shader (ScanPs + water_ps + pixel reader + wire / solid kind; "
                    "%zu game shaders, %.1f KB average bytecode; %u)\n", us, corpus.size(), (double)bytes / 1024.0 / corpus.size(), acc & 1u);
        CHECK(us < 500.0, "the create-time classification stays well under 0.5 ms per shader (%.2f us)", us);
    }
}

// ---- phase 110b: the verdict noise fix (cut-out per drawn texture, live; fixes count only while ON) ----------------------------
static void Phase110b() {
    using namespace Census;
    std::printf("4. phase 110b verdict rules\n");
    // the reason classes
    {
        const int ok[] = { kCutPatched, kCutApplied, kCutNoLoss, kCutSoft, kCutCoverage, kCutNoTex };
        const int wait[] = { kCutQueue, kCutPending };
        const int bad[] = { kCutUnknown, kCutOff, kCutSize, kCutFormat, kCutSparse };
        for (int r : ok) CHECK(CutClassOf(r) == kCutClsOk, "class OK: %s", CutReasonName(r));
        for (int r : wait) CHECK(CutClassOf(r) == kCutClsWait, "class WAIT: %s", CutReasonName(r));
        for (int r : bad) CHECK(CutClassOf(r) == kCutClsBad, "class BAD: %s", CutReasonName(r));
        for (int r = 0; r < kCutCount; ++r) CHECK(std::strcmp(CutReasonName(r), "?") != 0, "every reason has a name (%d)", r);
        CHECK(CutWorse(kCutApplied, kCutSparse) == kCutSparse && CutWorse(kCutSparse, kCutApplied) == kCutSparse, "CutWorse: bad wins");
        CHECK(CutWorse(kCutQueue, kCutSoft) == kCutQueue && CutWorse(kCutNoTex, kCutSoft) == kCutSoft, "CutWorse: wait > ok > no texture");
    }
    // one pair, several textures: the pair is a cut-out suspect only while a BAD texture was drawn within kCutWindow
    {
        Cover c;
        CutTrack t;
        CHECK(t.Shown(0) == kCutPending, "a fresh pair is pending (never a suspect at creation -- the 110a bug)");
        c.cutReason = t.Shown(0);
        CHECK(Judge(kCutout, c, false).Suspect() == 0, "fresh pair: not a suspect");
        uint64_t n = 1000;
        t.Note(kCutApplied, n); t.Note(kCutSoft, n); t.Note(kCutPatched, n); t.Note(kCutQueue, n);
        c.cutReason = t.Shown(n);
        CHECK(CutClassOf(c.cutReason) != kCutClsBad && Judge(kCutout, c, false).Suspect() == 0, "mixed OK + waiting textures = KNOWN (%s)",
              CutReasonName(c.cutReason));
        t.Note(kCutSparse, n + 1);                       // one sparse texture drawn
        t.Note(kCutApplied, n + 2);                      // ... and patched ones after it in the same frames
        c.cutReason = t.Shown(n + 2);
        CHECK(c.cutReason == kCutSparse && Judge(kCutout, c, false).Suspect() == kCutout, "a drawn sparse texture makes the pair a suspect");
        CHECK(t.Shown(n + 1 + kCutWindow) == kCutSparse, "... for kCutWindow Presents");
        // live: that texture is not drawn any more (the loader patched it, or it left the screen) -> KNOWN again after the window
        t.Note(kCutApplied, n + 2 + kCutWindow);
        c.cutReason = t.Shown(n + 2 + kCutWindow);
        CHECK(c.cutReason == kCutApplied && Judge(kCutout, c, false).Suspect() == 0, "live: the bad texture aged out -> KNOWN (%s)",
              CutReasonName(c.cutReason));
        CHECK(t.checks[kCutClsOk] == 5 && t.checks[kCutClsWait] == 1 && t.checks[kCutClsBad] == 1, "check counts %llu %llu %llu",
              (unsigned long long)t.checks[0], (unsigned long long)t.checks[1], (unsigned long long)t.checks[2]);
        // a pair whose LAST check was bad and that is not drawn any more keeps the bad verdict (the file shows the last state)
        CutTrack u;
        u.Note(kCutApplied, 10); u.Note(kCutOff, 20);
        CHECK(u.Shown(20 + 5 * kCutWindow) == kCutOff, "not drawn since its bad texture: stays bad");
        // live patch: waiting -> patched -> applied, never a suspect on the way
        CutTrack w;
        w.Note(kCutPending, 5);
        CHECK(w.Shown(5) == kCutPending, "pending shown");
        w.Note(kCutQueue, 6);
        CHECK(w.Shown(6) == kCutQueue && CutCovered(w.Shown(6)), "queue full = waiting, not a suspect");
        w.Note(kCutPatched, 7);
        CHECK(w.Shown(7) == kCutPatched, "patched shown once the worker is done (the waiting one is older)");
        w.Note(kCutApplied, 8);
        CHECK(w.Shown(8) == kCutApplied, "applied at the first discard draw");
    }
    // fixes count only while ON
    {
        Cover c; c.wirePsKind = 1; c.wireVs = true;
        c.wireOn = false;
        CHECK(Judge(0, c, true).Suspect() == kThin, "wire pair, wire_widen off: thin is a REAL suspect");
        c.wireOn = true;
        CHECK(Judge(0, c, true).Suspect() == 0, "wire pair, wire_widen on: KNOWN");
        Cover d; d.solidKind = 4; d.solidVs = true; d.wireOn = false; d.poleOn = false;
        CHECK(Judge(0, d, true).Suspect() == kThin, "kind D, both fixes off: thin suspect");
        d.poleOn = true;
        CHECK(Judge(0, d, true).Suspect() == 0, "kind D, pole_aa on: KNOWN");
        d.poleOn = false; d.wireOn = true;
        CHECK(Judge(0, d, true).Suspect() == 0, "kind D, wire_widen on: KNOWN");
        Cover h; h.solidKind = 8; h.solidVs = true; h.wireOn = true; h.poleOn = false;
        CHECK(Judge(0, h, true).Suspect() == kThin, "kind H is pole_aa's only: wire_widen does not cover it");
        h.poleOn = true;
        CHECK(Judge(0, h, true).Suspect() == 0, "kind H, pole_aa on: KNOWN");
        Cover a; a.solidKind = 1; a.solidVs = true; a.poleOn = true; a.wireOn = false;
        CHECK(Judge(0, a, true).Suspect() == kThin, "kind A: pole_aa does not cover it");
        Cover g; g.gameWire = true; g.powerLineOn = false;
        CHECK(Judge(0, g, true).Suspect() == kThin, "power line, power_line_fix off: suspect");
        Cover p; p.pixelFix = true; p.prFixOn = false;
        CHECK(Judge(kPixel, p, false).Suspect() == kPixel, "twin but pixel_reader_fix off: suspect");
        Cover w; w.waterRule = 2; w.waterFixOn = false;
        CHECK(Judge(kPixel | kWater, w, false).Suspect() == (kPixel | kWater), "water rule but water_overscan_fix off: suspect");
        Cover o; o.overscan = false;
        CHECK(Judge(kPixel | kWater, o, false).Suspect() == 0, "jitter_overscan 0: a pixel read is not off by one");
        CHECK(std::string(ThinFixName(h)) == "pole_aa" && std::string(ThinFixName(Cover())).empty(), "ThinFixName");
        PsScan s;
        Verdict v = Judge(0, c, true); (void)v;
        c.wireOn = false;
        const std::string wt = WhyText(Judge(0, c, true), c, s, 0.1, 20);
        CHECK(wt.find("its fix wire_widen is OFF") != std::string::npos, "WhyText names the fix that is off: %s", wt.c_str());
    }
    // the user's 2026-11-15 Norway drive (dlaa_census.txt: 31 suspects, ETS2 flat, cutout_mips 1, wire_widen 0, pole_aa 0,
    // power_line_fix / pixel_reader_fix / water_overscan_fix 1): replayed with the 110b rule -> 16 real suspects (15 thin + 1
    // unknown-VS only), every one of the 16 cut-out-only pairs is KNOWN
    {
        struct Row { const char* id; uint32_t st; bool thin; int water; bool twin; int wirePs; bool wireVs; int solid; bool solidVs; int cut; };
        const Row rows[] = {
        { "25fc1eee_a9d39be8", kCutout, false, 0, false, 0, false, 0, false, kCutPatched },
        { "8fcd62e3_a9d39be8", kCutout, false, 0, false, 0, false, 0, false, kCutQueue },
        { "3847be86_2d7ed0e6", kCutout, false, 0, false, 0, false, 4, true, kCutSoft },
        { "2e01f9a5_a9d39be8", kCutout, false, 0, false, 0, false, 0, false, kCutApplied },
        { "2a9f326e_e19e0136", 0, true, 0, false, 0, false, 8, false, kCutPending },
        { "0dfced74_180589ca", kCutout, true, 0, false, 0, false, 0, false, kCutSoft },
        { "6d2c4e63_438d1a63", kCutout, false, 0, false, 0, false, 0, false, kCutPatched },
        { "07dfaa00_c92c7c22", 0, true, 0, false, 0, false, 0, false, kCutPending },
        { "c8dddb32_438d1a63", kCutout, true, 0, false, 0, false, 0, false, kCutCoverage },
        { "97d9c72d_0a1ed38a", kPixel, true, 0, true, 0, false, 0, false, kCutPending },
        { "3c957c7c_18e8e355", 0, true, 0, false, 0, false, 0, false, kCutPending },
        { "4186dac5_b97c44a3", kPixel|kCutout, false, 0, true, 2, true, 0, false, kCutCoverage },
        { "5ebed025_7c55acee", 0, true, 0, false, 0, false, 0, false, kCutPending },
        { "1ec77518_b8636556", 0, true, 0, false, 0, false, 0, false, kCutPending },
        { "50cde715_438d1a63", kCutout, false, 0, false, 0, false, 0, false, kCutPatched },
        { "db9404ad_a761133a", kPixel, true, 0, true, 0, false, 0, false, kCutPending },
        { "07dfaa00_ad165830", 0, true, 0, false, 0, false, 0, false, kCutPending },
        { "9d73eba3_3c3197f0", kCutout, false, 0, false, 0, false, 0, false, kCutSoft },
        { "07dfaa00_2585159b", 0, true, 0, false, 0, false, 0, false, kCutPending },
        { "9d73eba3_5396e5a9", 0, true, 0, false, 0, false, 0, false, kCutPending },
        { "2f9ff4a3_438d1a63", kCutout, true, 0, false, 0, false, 0, false, kCutPatched },
        { "8ba7d8da_fd5e16e5", kPixel, true, 0, true, 0, false, 0, false, kCutPending },
        { "07dfaa00_7770320b", kCutout, false, 0, false, 0, false, 0, false, kCutPatched },
        { "d87f5280_2d7ed0e6", kCutout, true, 0, false, 0, false, 4, true, kCutSoft },
        { "07dfaa00_c55c787d", kCutout, false, 0, false, 0, false, 0, false, kCutCoverage },
        { "0dfced74_a710b6a2", kCutout, false, 0, false, 0, false, 0, false, kCutSoft },
        { "5e03b1ca_41c3064e", kCutout, false, 0, false, 0, false, 0, false, kCutNoLoss },
        { "9d2904cd_2d7ed0e6", kCutout, false, 0, false, 0, false, 4, true, kCutSoft },
        { "7fcd07d6_b67da69f", 0, false, 0, false, 0, false, 1, false, kCutPending },
        { "07dfaa00_cfc52d43", kCutout, false, 0, false, 0, false, 7, true, kCutSoft },
        { "c2ec5608_a9d39be8", kCutout, false, 0, false, 0, false, 0, false, kCutNoLoss },
        };
        int sus = 0, thin = 0, vs = 0, cut = 0;
        for (const Row& r : rows) {
            Cover c;
            c.waterRule = r.water; c.pixelFix = r.twin; c.wirePsKind = r.wirePs; c.wireVs = r.wireVs; c.solidKind = r.solid;
            c.solidVs = r.solidVs; c.cutReason = r.cut;
            c.wireOn = false; c.poleOn = false;                   // the user's dlaa.ini
            const uint32_t s = Judge(r.st, c, r.thin).Suspect();
            if (s) ++sus;
            if (s & kThin) ++thin;
            if (s & kVs) ++vs;
            if (s & kCutout) ++cut;
        }
        static_assert(sizeof(rows) / sizeof(rows[0]) == 31, "the 31 suspects of the 2026-11-15 file");
        // (phase 113b: 16 / 15 / 2 -> 15 / 15 / 1: the lamp-VS pair 7fcd07d6_b67da69f was unknown-VS without being thin -- unknown-VS
        // now needs a confirmed thin pair)
        CHECK(sus == 15 && thin == 15 && vs == 1 && cut == 0, "Norway replay: %d suspects (thin %d, unknown-VS %d, cut-out %d), want 15 / 15 / 1 / 0",
              sus, thin, vs, cut);
        std::printf("   Norway 2026-11-15 replay: 31 suspects before -> %d (thin %d, unknown-VS %d, cut-out %d)\n", sus, thin, vs, cut);
    }
}

// phase 111: the 16 suspects of the 2026-11-15 Norway capture, triaged (DEVELOPMENT.md "Phase 111"): the known-pair list, the
// alpha-coverage correction of the thin measure, pair A's VS set + 2a9f326e / 7fcd07d6, kind I (PS 2585159b)
static const Census::KnownPair* KnownByShort(uint32_t vs0, uint32_t ps0, int* idx) {
    int n = 0;
    const Census::KnownPair* k = Census::KnownPairs(&n);
    for (int i = 0; i < n; ++i) if (k[i].vs[0] == vs0 && k[i].ps[0] == ps0) { if (idx) *idx = i; return &k[i]; }
    if (idx) *idx = -1;
    return nullptr;
}
static void Phase111() {
    using namespace Census;
    std::printf("phase 111: known pairs, alpha-corrected thin measure, new VS set entries + kind I\n");
    // ThinPixels
    CHECK(ThinPixels(100, -1.0) == 100 && ThinPixels(100, 1.0) == 100 && ThinPixels(100, 0.0) == 100, "ThinPixels: no / unknown coverage = as measured");
    CHECK(ThinPixels(100, 0.5) == 200 && ThinPixels(28, 0.28) == 100, "ThinPixels: px / coverage");
    CHECK(ThinPixels(10, 0.001) == 200, "ThinPixels: coverage floor kThinCovFloor (0.05)");
    // the roof-ladder lattice 0dfced74_180589ca: 0.36 px / tri measured at 28 % coverage -> ~1.3 px / tri: not thin any more
    {
        const uint64_t tris = 25 * 1000, px = 9000;      // 25 samples, 0.36 px per triangle
        CHECK(ThinVerdict(px, tris, 25), "180589ca measured: thin");
        uint64_t corr = 0;
        for (int i = 0; i < 25; ++i) corr += ThinPixels(px / 25, 0.28);
        CHECK(!ThinVerdict(corr, tris, 25), "180589ca alpha-corrected (%.2f px / tri): not thin", (double)corr / (double)tris);
        // the flower cards (alpha 100 % opaque) and the pole body (97 %) stay thin
        uint64_t f = 0, d = 0;
        for (int i = 0; i < 31; ++i) { f += ThinPixels(120, 1.0); d += ThinPixels(150, 0.97); }
        CHECK(ThinVerdict(f, 31 * 1000, 31) && ThinVerdict(d, 31 * 1000, 31), "opaque-alpha cut-outs stay thin (0.12 / 0.15 px per triangle)");
    }
    // the known list: full checksums match, a different second dword does not; every entry has a reason; kinds = thin only
    {
        int n = 0;
        const KnownPair* k = KnownPairs(&n);
        CHECK(n == 18, "18 known pairs (11 of phase 111 + 3 of phase 113 + 4 of phase 114) (%d)", n);
        for (int i = 0; i < n; ++i) {
            CHECK(KnownPairIndex(k[i].vs, k[i].ps) == i, "KnownPairIndex finds entry %d", i);
            CHECK(k[i].what && strlen(k[i].what) > 20 && k[i].kinds == kThin, "entry %d: reason text, thin only", i);
            uint32_t v2[4]; memcpy(v2, k[i].vs, 16); v2[1] ^= 1u;
            CHECK(KnownPairIndex(v2, k[i].ps) == -1, "entry %d: a different VS checksum is not known", i);
        }
        // its switch: the forward reflector re-light needs pole_aa
        int ri = -1;
        KnownByShort(0xdb9404adu, 0xa761133au, &ri);
        Cover c; c.knownPair = ri; c.pixelFix = true;
        c.poleOn = false;
        CHECK(Judge(kPixel, c, true).Suspect() == kThin, "a761133a: thin suspect while pole_aa is off");
        const std::string wt = WhyText(Judge(kPixel, c, true), c, PsScan(), 0.1, 34);
        CHECK(wt.find("known pair") != std::string::npos && wt.find("pole_aa is OFF") != std::string::npos, "WhyText of a known pair: %s", wt.c_str());
        c.poleOn = true;
        CHECK(Judge(kPixel, c, true).Suspect() == 0, "a761133a: known with pole_aa on");
        int bi = -1;
        KnownByShort(0x07dfaa00u, 0xc92c7c22u, &bi);
        Cover b; b.knownPair = bi; b.wireOn = b.poleOn = false;
        CHECK(Judge(0, b, true).Suspect() == 0, "the far building is known with every fix off");
        b.cutReason = kCutOff;
        CHECK(Judge(kCutout, b, true).Suspect() == kCutout, "a known-list entry covers thin only, never a BAD cut-out");
    }
    // the new VS set entries / kind I by checksum (fake bytecode headers: ChecksumIs reads bytes 4..19)
    {
        auto fake = [](const uint32_t s[4]) { std::vector<uint8_t> b(64, 0); memcpy(b.data(), "DXBC", 4); memcpy(b.data() + 4, s, 16); return b; };
        const std::vector<uint8_t> a3 = fake(WireWiden::kSolidVsA3), a4 = fake(WireWiden::kSolidVsA4), pi = fake(WireWiden::kSolidPsI);
        static int objA3, objA4, objPi;
        WireWiden::NoteVs(&objA3, a3.data(), a3.size());
        WireWiden::NoteVs(&objA4, a4.data(), a4.size());
        WireWiden::NotePs(&objPi, pi.data(), pi.size());
        CHECK(WireWiden::IsSolidVs(&objA3, WireWiden::kSolidH) && WireWiden::IsSolidVs(&objA4, WireWiden::kSolidA) &&
              WireWiden::IsSolidVs(&objA3, WireWiden::kSolidI), "2a9f326e / 7fcd07d6 are in pair A's VS set (kinds A / C / G / H / I)");
        CHECK(!WireWiden::IsSolidVs(&objA3, WireWiden::kSolidD), "... not in pair D's set");
        CHECK(WireWiden::SolidPsKind(&objPi) == WireWiden::kSolidI && WireWiden::SolidPsKindOfCode(pi.data(), pi.size()) == WireWiden::kSolidI,
              "PS 2585159b = kind I");
        CHECK(WireWiden::SolidGsOf(WireWiden::kSolidI) == 0 && WireWiden::SolidReplayKind(WireWiden::kSolidI) == 2, "kind I -> GSolidA, replay 2");
        Cover c; c.solidKind = WireWiden::kSolidI; c.solidVs = true; c.wireOn = false; c.poleOn = true;
        CHECK(Judge(0, c, true).Suspect() == kThin, "kind I: thin suspect while wire_widen is off (pole_aa does not cover it)");
        c.wireOn = true;
        CHECK(Judge(0, c, true).Suspect() == 0, "kind I: known with wire_widen on");
    }
    // the real capture (local only): every dumped pair's checksums -> the known / kind facts used below
    {
        const std::string dir = g_capDir + "census_2026-11-15\\shimmer_20261115_104831\\census\\";
        std::vector<uint8_t> v, q;
        if (!ReadFile(dir + "07dfaa00_c92c7c22\\vs.dxbc", v)) std::printf("   (the 2026-11-15 capture is not here: real-checksum checks skipped)\n");
        else {
            static const char* const kKnownDirs[] = { "07dfaa00_c92c7c22", "9d73eba3_5396e5a9", "5ebed025_7c55acee", "97d9c72d_0a1ed38a",
                "3c957c7c_18e8e355", "07dfaa00_ad165830", "1ec77518_b8636556", "8ba7d8da_fd5e16e5", "2f9ff4a3_438d1a63", "c8dddb32_438d1a63",
                "db9404ad_a761133a" };
            for (const char* d : kKnownDirs) {
                uint32_t vs[4], ps[4];
                const bool ok = ReadFile(dir + d + "\\vs.dxbc", v) && ReadFile(dir + d + "\\ps.dxbc", q) && Checksum(v.data(), v.size(), vs) &&
                                Checksum(q.data(), q.size(), ps);
                CHECK(ok && KnownPairIndex(vs, ps) >= 0, "%s: the real bytecode matches its known-list entry", d);
            }
            CHECK(ReadFile(dir + "07dfaa00_2585159b\\ps.dxbc", q) && WireWiden::SolidPsKindOfCode(q.data(), q.size()) == WireWiden::kSolidI,
                  "real PS 2585159b = kind I");
            uint32_t s2[4] = {}, s7[4] = {};
            CHECK(ReadFile(dir + "2a9f326e_e19e0136\\vs.dxbc", v) && Checksum(v.data(), v.size(), s2) && !memcmp(s2, WireWiden::kSolidVsA3, 16),
                  "real VS 2a9f326e = kSolidVsA3");
            CHECK(ReadFile(dir + "7fcd07d6_b67da69f\\vs.dxbc", v) && Checksum(v.data(), v.size(), s7) && !memcmp(s7, WireWiden::kSolidVsA4, 16),
                  "real VS 7fcd07d6 = kSolidVsA4");
        }
    }
    // the Norway replay (the 31 suspects of the 2026-11-15 file) with phase 111: the user's ini (wire_widen 0, pole_aa 0) and with
    // both fixes on. Columns: id, structural, measured px / tri, thin samples, alpha coverage of the drawn texture (-1 none / unknown),
    // twin, wire PS, wire VS, solid kind, VS in its set (phase 111 sets), cut-out reason.
    {
        struct Row { const char* id; uint32_t st; double pxt; int samples; double cov; bool twin; int wirePs; bool wireVs; int solid; bool solidVs; int cut; };
        const Row rows[] = {
        { "25fc1eee_a9d39be8", kCutout, 188.22, 16, -1, false, 0, false, 0, false, kCutPatched },
        { "8fcd62e3_a9d39be8", kCutout, 4.20, 42, -1, false, 0, false, 0, false, kCutQueue },
        { "3847be86_2d7ed0e6", kCutout, 1.0, 0, -1, false, 0, false, 4, true, kCutSoft },
        { "2e01f9a5_a9d39be8", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutApplied },
        { "2a9f326e_e19e0136", 0, 0.10, 34, -1, false, 0, false, 8, true, kCutPending },
        { "0dfced74_180589ca", kCutout, 0.36, 25, 0.28, false, 0, false, 0, false, kCutSoft },
        { "6d2c4e63_438d1a63", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutPatched },
        { "07dfaa00_c92c7c22", 0, 0.05, 3, -1, false, 0, false, 0, false, kCutPending },
        { "c8dddb32_438d1a63", kCutout, 0.12, 31, 1.0, false, 0, false, 0, false, kCutCoverage },
        { "97d9c72d_0a1ed38a", kPixel, 0.28, 34, -1, true, 0, false, 0, false, kCutPending },
        { "3c957c7c_18e8e355", 0, 0.30, 4, -1, false, 0, false, 0, false, kCutPending },
        { "4186dac5_b97c44a3", kPixel | kCutout, 1.0, 0, -1, true, 2, true, 0, false, kCutCoverage },
        { "5ebed025_7c55acee", 0, 0.15, 35, -1, false, 0, false, 0, false, kCutPending },
        { "1ec77518_b8636556", 0, 0.14, 40, -1, false, 0, false, 0, false, kCutPending },
        { "50cde715_438d1a63", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutPatched },
        { "db9404ad_a761133a", kPixel, 0.10, 34, -1, true, 0, false, 0, false, kCutPending },
        { "07dfaa00_ad165830", 0, 0.17, 31, -1, false, 0, false, 0, false, kCutPending },
        { "9d73eba3_3c3197f0", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutSoft },
        { "07dfaa00_2585159b", 0, 0.08, 20, -1, false, 0, false, 9, true, kCutPending },
        { "9d73eba3_5396e5a9", 0, 0.28, 20, -1, false, 0, false, 0, false, kCutPending },
        { "2f9ff4a3_438d1a63", kCutout, 0.11, 18, -1, false, 0, false, 0, false, kCutPatched },
        { "8ba7d8da_fd5e16e5", kPixel, 0.04, 9, -1, true, 0, false, 0, false, kCutPending },
        { "07dfaa00_7770320b", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutPatched },
        { "d87f5280_2d7ed0e6", kCutout, 0.15, 34, 0.97, false, 0, false, 4, true, kCutSoft },
        { "07dfaa00_c55c787d", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutCoverage },
        { "0dfced74_a710b6a2", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutSoft },
        { "5e03b1ca_41c3064e", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutNoLoss },
        { "9d2904cd_2d7ed0e6", kCutout, 1.0, 0, -1, false, 0, false, 4, true, kCutSoft },
        { "7fcd07d6_b67da69f", 0, 1.0, 0, -1, false, 0, false, 1, true, kCutPending },
        { "07dfaa00_cfc52d43", kCutout, 1.0, 0, -1, false, 0, false, 7, true, kCutSoft },
        { "c2ec5608_a9d39be8", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutNoLoss },
        };
        static_assert(sizeof(rows) / sizeof(rows[0]) == 31, "the 31 suspects of the 2026-11-15 file");
        for (int pass = 0; pass < 2; ++pass) {
            int sus = 0, thin = 0, vs = 0, cut = 0, known = 0;
            std::string left;
            for (const Row& r : rows) {
                Cover c;
                c.pixelFix = r.twin; c.wirePsKind = r.wirePs; c.wireVs = r.wireVs; c.solidKind = r.solid; c.solidVs = r.solidVs; c.cutReason = r.cut;
                unsigned v0 = 0, p0 = 0;
                sscanf_s(r.id, "%8x_%8x", &v0, &p0);
                int ki = -1;
                KnownByShort(v0, p0, &ki);
                c.knownPair = ki;
                known += ki >= 0;
                c.wireOn = c.poleOn = pass == 1;              // pass 0 = the user's dlaa.ini, 1 = wire_widen + pole_aa on
                const uint64_t tris = 1000ull * (uint64_t)(r.samples > 0 ? r.samples : 1);
                uint64_t px = 0;
                for (int i = 0; i < r.samples; ++i) px += ThinPixels((uint64_t)(r.pxt * 1000.0 + 0.5), r.cov);
                const bool isThin = ThinVerdict(px, tris, r.samples);
                const uint32_t s = Judge(r.st, c, isThin).Suspect();
                if (s) { ++sus; left += std::string(" ") + r.id; }
                if (s & kThin) ++thin;
                if (s & kVs) ++vs;
                if (s & kCutout) ++cut;
            }
            CHECK(known == 11, "11 of the 31 pairs are on the known list (%d)", known);
            if (pass == 0)
                CHECK(sus == 4 && thin == 4 && vs == 0 && cut == 0, "Norway replay (user's ini): %d suspects (thin %d, VS %d, cut %d), want 4 / 4 / 0 / 0", sus, thin, vs, cut);
            else
                CHECK(sus == 0, "Norway replay (wire_widen + pole_aa on): %d suspects, want 0", sus);
            std::printf("   Norway 2026-11-15 replay, phase 111, %s: 31 -> %d suspects (thin %d, unknown-VS %d, cut-out %d):%s\n",
                        pass ? "wire_widen 1 + pole_aa 1" : "the user's ini (wire_widen 0, pole_aa 0)", sus, thin, vs, cut, left.c_str());
        }
    }
}

// phase 113: the 2026-11-15 14:54 capture by the Volvo Trucks Center (census_2026-11-15\shimmer_20261115_145426): the VS bdce664b
// joins pair A's set, three forward pairs join the known list, the cut-out reasons of the late read-back / sparse rule, the "a fix
// is switched off" rule of the legend, and the replay of that file's 11 suspects
static void Phase113() {
    using namespace Census;
    std::printf("phase 113: Volvo Trucks Center capture -- VS bdce664b, known forward pairs, late read-back + sparse reasons, replay\n");
    // the new cut-out reasons
    CHECK(CutClassOf(kCutLate) == kCutClsWait && CutClassOf(kCutNoWrite) == kCutClsBad && CutClassOf(kCutUnknown) == kCutClsBad,
          "late read-back = WAIT (never tinted), cannot be patched = BAD, never seen = BAD (until the read-back starts)");
    CHECK(!strcmp(CutReasonName(kCutLate), "being read back from the GPU for the cut-out mips (phase 113)") &&
          !strcmp(CutReasonName(kCutNoWrite), "cannot be patched (not a DEFAULT-usage texture)"), "the new reasons have names");
    CHECK(CutWorse(kCutApplied, kCutLate) == kCutLate && CutWorse(kCutLate, kCutNoWrite) == kCutNoWrite, "worse: bad > wait > ok");
    // VS bdce664b in pair A's VS set (fake bytecode header)
    {
        auto fake = [](const uint32_t s[4]) { std::vector<uint8_t> b(64, 0); memcpy(b.data(), "DXBC", 4); memcpy(b.data() + 4, s, 16); return b; };
        const std::vector<uint8_t> a5 = fake(WireWiden::kSolidVsA5);
        static int objA5;
        WireWiden::NoteVs(&objA5, a5.data(), a5.size());
        CHECK(WireWiden::IsSolidVs(&objA5, WireWiden::kSolidI) && WireWiden::IsSolidVs(&objA5, WireWiden::kSolidA) &&
              !WireWiden::IsSolidVs(&objA5, WireWiden::kSolidD), "bdce664b: in pair A's VS set (kind I covers it), not pair D's");
        Cover c; c.solidKind = WireWiden::kSolidI; c.solidVs = true; c.wireOn = false;
        CHECK((Judge(0, c, false).Suspect() & kVs) == 0, "bdce664b + 2585159b: no longer unknown-VS");
        CHECK(Judge(0, c, false).Suspect() == 0, "bdce664b + 2585159b at 0.60 px per triangle (not thin): no suspect, wire_widen off");
    }
    // the three forward pairs on the known list
    {
        int i1 = -1, i2 = -1, i3 = -1;
        KnownByShort(0x4186dac5u, 0x9c062880u, &i1);
        KnownByShort(0x07dfaa00u, 0xf8ac351du, &i2);
        KnownByShort(0xfaf4c362u, 0xe2128488u, &i3);
        CHECK(i1 >= 0 && i2 >= 0 && i3 >= 0, "4186dac5_9c062880 / 07dfaa00_f8ac351d / faf4c362_e2128488 are on the known list");
        int n = 0;
        const KnownPair* k = KnownPairs(&n);
        for (int i : { i1, i2, i3 })
            if (i >= 0) CHECK(k[i].kinds == kThin && k[i].fix == kKnownFixNone, "known entry %d: thin, no switch needed", i);
        Cover c; c.knownPair = i1; c.pixelFix = true; c.wireOn = c.poleOn = false;
        CHECK(Judge(kPixel, c, true).Suspect() == 0, "4186dac5_9c062880: thin known + its pixel read twinned -> covered with every switch off");
        c.prFixOn = false;
        CHECK(Judge(kPixel, c, true).Suspect() == kPixel, "... but pixel_reader_fix off makes its pixel read a suspect again");
        CHECK(FixOffKinds(kPixel, c, true) == kPixel, "... and that is a fix the player switched off (the legend says so)");
    }
    // FixOffKinds (the legend line)
    {
        Cover d; d.solidKind = WireWiden::kSolidD; d.solidVs = true; d.wireOn = d.poleOn = false;
        CHECK(FixOffKinds(0, d, true) == kThin, "a pole body with wire_widen + pole_aa off: thin, a fix is OFF");
        d.poleOn = true;
        CHECK(FixOffKinds(0, d, true) == 0 && Judge(0, d, true).Suspect() == 0, "pole_aa on: covered, nothing off");
        Cover u;                                          // a thin pair the mod has no fix for
        CHECK(FixOffKinds(0, u, true) == 0 && Judge(0, u, true).Suspect() == kThin, "a thin pair without a fix: suspect, but no fix is off");
        Cover o; o.cutReason = kCutOff;
        CHECK(FixOffKinds(kCutout, o, false) == kCutout, "cutout_mips = 0: the cut-out fix is off");
        o.cutReason = kCutUnknown;
        CHECK(FixOffKinds(kCutout, o, false) == 0, "a texture never seen: no switch would help (the late read-back is the fix)");
    }
    // the real capture (local only): VS bdce664b's checksum, the three known pairs' bytecode
    {
        const std::string dir = g_capDir + "census_2026-11-15\\shimmer_20261115_145426\\census\\";
        std::vector<uint8_t> v, q;
        if (!ReadFile(dir + "bdce664b_2585159b\\vs.dxbc", v)) std::printf("   (the 14:54 capture is not here: real-checksum checks skipped)\n");
        else {
            uint32_t s5[4] = {};
            CHECK(Checksum(v.data(), v.size(), s5) && !memcmp(s5, WireWiden::kSolidVsA5, 16), "real VS bdce664b = kSolidVsA5");
            for (const char* d : { "4186dac5_9c062880", "07dfaa00_f8ac351d", "faf4c362_e2128488" }) {
                uint32_t vs[4], ps[4];
                const bool ok = ReadFile(dir + d + "\\vs.dxbc", v) && ReadFile(dir + d + "\\ps.dxbc", q) && Checksum(v.data(), v.size(), vs) &&
                                Checksum(q.data(), q.size(), ps);
                CHECK(ok && KnownPairIndex(vs, ps) >= 0, "%s: the real bytecode matches its known-list entry", d);
            }
        }
    }
    // the replay: the 11 suspects of the 14:54 file under the phase-113 rules. Cut-out reasons as the census now answers them: the
    // two "not seen" textures start their GPU read-back at the census's own check (kCutLate, WAIT), the sparse roof texture's real
    // mips keep its coverage (kCutNoLoss: cutout_mips_test, the real DDS). Columns as Phase111's replay.
    {
        struct Row { const char* id; uint32_t st; double pxt; int samples; double cov; bool twin; int wirePs; bool wireVs; int solid; bool solidVs; int cut; };
        const Row rows[] = {
        { "8fcd62e3_a9d39be8", kCutout, 13.03, 41, -1, false, 0, false, 0, false, kCutLate },
        { "6d2c4e63_438d1a63", kCutout, 26.85, 30, -1, false, 0, false, 0, false, kCutLate },
        { "c2ec5608_a9d39be8", kCutout, 4.30, 14, -1, false, 0, false, 0, false, kCutNoLoss },
        { "d87f5280_2d7ed0e6", kCutout, 0.13, 34, -1, false, 0, false, 4, true, kCutSoft },
        { "db9404ad_a761133a", kPixel, 0.10, 34, -1, true, 0, false, 0, false, kCutPending },
        { "2a9f326e_e19e0136", 0, 0.10, 34, -1, false, 0, false, 8, true, kCutPending },
        { "07dfaa00_2585159b", 0, 0.12, 21, -1, false, 0, false, 9, true, kCutPending },
        { "bdce664b_2585159b", 0, 0.60, 17, -1, false, 0, false, 9, true, kCutPending },
        { "4186dac5_9c062880", kPixel, 0.12, 8, -1, true, 0, false, 0, false, kCutPending },
        { "07dfaa00_f8ac351d", 0, 0.25, 18, -1, false, 0, false, 0, false, kCutPending },
        { "faf4c362_e2128488", 0, 0.00, 9, -1, false, 0, false, 0, false, kCutPending },
        };
        static_assert(sizeof(rows) / sizeof(rows[0]) == 11, "the 11 suspects of the 14:54 file");
        const char* const passName[3] = { "the user's ini at the capture (wire_widen 0, pole_aa 0)", "pole_aa 1 (the user's ini now)",
                                          "wire_widen 1 + pole_aa 1" };
        const int want[3] = { 4, 1, 0 };
        for (int pass = 0; pass < 3; ++pass) {
            int sus = 0, fixOff = 0;
            std::string left;
            for (const Row& r : rows) {
                Cover c;
                c.pixelFix = r.twin; c.wirePsKind = r.wirePs; c.wireVs = r.wireVs; c.solidKind = r.solid; c.solidVs = r.solidVs; c.cutReason = r.cut;
                unsigned v0 = 0, p0 = 0;
                sscanf_s(r.id, "%8x_%8x", &v0, &p0);
                int ki = -1;
                KnownByShort(v0, p0, &ki);
                c.knownPair = ki;
                c.wireOn = pass == 2;
                c.poleOn = pass >= 1;
                const uint64_t tris = 1000ull * (uint64_t)(r.samples > 0 ? r.samples : 1);
                uint64_t px = 0;
                for (int i = 0; i < r.samples; ++i) px += ThinPixels((uint64_t)(r.pxt * 1000.0 + 0.5), r.cov);
                const bool isThin = ThinVerdict(px, tris, r.samples);
                const uint32_t s = Judge(r.st, c, isThin).Suspect();
                if (s) { ++sus; left += std::string(" ") + r.id; }
                if (FixOffKinds(r.st, c, isThin)) ++fixOff;
            }
            CHECK(sus == want[pass], "14:54 replay, %s: %d suspects, want %d (%s)", passName[pass], sus, want[pass], left.c_str());
            CHECK(fixOff == sus, "14:54 replay: every remaining suspect is one whose fix is switched off (%d of %d)", fixOff, sus);
            std::printf("   14:54 replay, phase 113, %s: 11 -> %d suspects:%s (all %d of them a fix that is switched off)\n", passName[pass], sus,
                        left.c_str(), fixOff);
        }
    }
}

// ---- phase 113b: the jitter probe (src/census_probe.h) on WARP ---------------------------------------------------------------------
// Real D3D11 rasterisation of synthetic meshes under the mod's 8 Halton offsets: thin posts / wires / 57 posts in one draw vs DENSE
// solid meshes (a facade of 0.125 px^2 triangles, a forest band, far boxes, glass panes) that the px-per-triangle rule flagged. Per
// mesh: the old pre-filter (px per triangle at the game's phase), the per-phase coverage WOBBLE (one occlusion count per jitter
// phase: (max - min) / mean) and the probe's STABLE FRACTION (pixels covered at all 8 offsets / the mean coverage). The game's state
// after Run must be exactly what it was.
namespace P113b {
struct V2 { float x, y; };
struct Mesh {
    const char* name;
    std::vector<V2> v;
    std::vector<uint32_t> ix;
    int expect;                                  // CensusProbe::kSteady / kThin, 0 = not asserted (reported only)
    bool overlap = false;                        // triangles over each other (the game's own count has them twice)
    void Quad(V2 a, V2 b, V2 c, V2 d) {          // a b c d around
        const uint32_t o = (uint32_t)v.size();
        v.push_back(a); v.push_back(b); v.push_back(c); v.push_back(d);
        const uint32_t q[6] = { o, o + 1, o + 2, o, o + 2, o + 3 };
        ix.insert(ix.end(), q, q + 6);
    }
    void Rect(float x0, float y0, float x1, float y1) { Quad({ x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 }); }
    void Grid(float x0, float y0, float x1, float y1, int nx, int ny) {   // a rectangle of nx x ny quads (no gaps, shared edges)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const float ax = x0 + (x1 - x0) * i / nx, bx = x0 + (x1 - x0) * (i + 1) / nx;
                const float ay = y0 + (y1 - y0) * j / ny, by = y0 + (y1 - y0) * (j + 1) / ny;
                Rect(ax, ay, bx, by);
            }
    }
    void Line(V2 a, V2 b, float t) {             // a quad from a to b, t px thick
        const float dx = b.x - a.x, dy = b.y - a.y, l = std::sqrt(dx * dx + dy * dy);
        const float nx = dy / l * t * 0.5f, ny = -dx / l * t * 0.5f;   // (the Rect winding: front-facing under the default cull)
        Quad({ a.x + nx, a.y + ny }, { b.x + nx, b.y + ny }, { b.x - nx, b.y - ny }, { a.x - nx, a.y - ny });
    }
    void Tri(V2 a, V2 b, V2 c) {
        const uint32_t o = (uint32_t)v.size();
        v.push_back(a); v.push_back(b); v.push_back(c);
        ix.push_back(o); ix.push_back(o + 1); ix.push_back(o + 2);
    }
};
static uint32_t s_rng = 12345u;
static float Rnd() { s_rng = s_rng * 1664525u + 1013904223u; return (float)((s_rng >> 8) & 0xFFFFFF) / 16777216.0f; }
static std::vector<Mesh> Meshes() {
    using namespace CensusProbe;
    std::vector<Mesh> m;
    { Mesh a{ "post 0.4 px x 60 px (a far road post)", {}, {}, kThin }; a.Rect(100.3f, 40.0f, 100.7f, 100.0f); m.push_back(a); }
    { Mesh a{ "post 1.0 px x 60 px", {}, {}, kThin }; a.Rect(120.2f, 40.0f, 121.2f, 100.0f); m.push_back(a); }
    { Mesh a{ "pole 3.0 px x 60 px (a near pole)", {}, {}, kSteady }; a.Rect(140.4f, 40.0f, 143.4f, 100.0f); m.push_back(a); }
    { Mesh a{ "57 posts 0.3..1.2 px in ONE draw (delineator row)", {}, {}, kThin };
      for (int i = 0; i < 57; ++i) { const float x = 10.0f + i * 8.3f + Rnd(), w = 0.3f + 0.9f * Rnd(), h = 10.0f + 50.0f * Rnd();
                                     a.Rect(x, 150.0f, x + w, 150.0f + h); }
      m.push_back(a); }
    { Mesh a{ "power line 0.35 px, 400 px at 3 deg", {}, {}, kThin }; a.Line({ 50.0f, 230.3f }, { 450.0f, 251.3f }, 0.35f); m.push_back(a); }
    { Mesh a{ "power line 0.35 px, 300 px at 30 deg", {}, {}, kThin }; a.Line({ 60.0f, 260.0f }, { 320.0f, 410.0f }, 0.35f); m.push_back(a); }
    { Mesh a{ "4 sagging power lines 0.5 px (40 segments each)", {}, {}, kThin };
      for (int w = 0; w < 4; ++w) for (int s = 0; s < 40; ++s) {
          auto y = [&](int k) { const float t = k / 40.0f; return 110.0f + w * 7.3f + 30.0f * 4.0f * t * (1.0f - t); };
          a.Line({ 20.0f + s * 11.0f, y(s) }, { 20.0f + (s + 1) * 11.0f, y(s + 1) }, 0.5f); }
      m.push_back(a); }
    { Mesh a{ "dense facade 120 x 60 px, 0.125 px^2 triangles", {}, {}, kSteady }; a.Grid(200.3f, 40.6f, 320.3f, 100.6f, 240, 120); m.push_back(a); }
    { Mesh a{ "dense facade drawn twice (overlapping layers)", {}, {}, kSteady }; a.Grid(200.3f, 40.6f, 320.3f, 100.6f, 120, 60);
      a.Grid(200.3f, 40.6f, 320.3f, 100.6f, 97, 53); a.overlap = true; m.push_back(a); }
    { Mesh a{ "far forest band 300 x 8 px + 4 px crowns, fine mesh", {}, {}, kSteady }; a.Grid(100.2f, 330.4f, 400.2f, 338.4f, 600, 16);
      for (int i = 0; i < 75; ++i) { const float x = 100.2f + i * 4.0f; a.Tri({ x, 330.4f }, { x + 2.0f, 326.4f - 1.5f * Rnd() }, { x + 4.0f, 330.4f }); }
      m.push_back(a); }
    { Mesh a{ "glass panes 12 x (18 x 24 px), 3 px gaps", {}, {}, kSteady };
      for (int j = 0; j < 2; ++j) for (int i = 0; i < 6; ++i) a.Grid(340.5f + i * 21.0f, 120.5f + j * 27.0f, 358.5f + i * 21.0f, 144.5f + j * 27.0f, 36, 48);
      m.push_back(a); }
    { Mesh a{ "40 far houses 3..6 px (12 triangles each)", {}, {}, kSteady };
      for (int i = 0; i < 40; ++i) { const float x = 20.0f + i * 12.0f + Rnd(), y = 360.0f + 8.0f * Rnd(), s = 3.0f + 3.0f * Rnd();
                                     a.Grid(x, y, x + s, y + s * 0.8f, 2, 3); }
      m.push_back(a); }
    { Mesh a{ "8 x 10 px block (the 7fcd07d6 far billboard)", {}, {}, kSteady }; a.Grid(430.3f, 300.7f, 438.3f, 310.7f, 20, 20); m.push_back(a); }
    { Mesh a{ "window lattice: 1 px frames, 20 px cells", {}, {}, 0 };
      for (int i = 0; i <= 6; ++i) a.Rect(20.4f + i * 20.0f, 280.3f, 21.4f + i * 20.0f, 340.3f);
      for (int j = 0; j <= 3; ++j) a.Rect(20.4f, 280.3f + j * 20.0f, 141.4f, 281.3f + j * 20.0f);
      a.overlap = true; m.push_back(a); }
    return m;
}
static const char* kVs = R"(
cbuffer C : register(b0) { float4 s; };
float4 main(float2 p : POSITION) : SV_Position { return float4(p.x * s.x + s.z, p.y * s.y + s.w, 0.5, 1.0); }
)";
static const char* kPs = R"(float4 main() : SV_Target { return float4(1, 1, 1, 1); })";
}  // namespace P113b

static void Phase113b() {
    using namespace P113b;
    using namespace CensusProbe;
    std::printf("phase 113b: the jitter probe -- stable fraction vs px per triangle vs per-phase wobble (WARP)\n");
    // the pure parts
    {
        float x, y;
        Offset(0, &x, &y);
        CHECK(x == 0.0f && std::fabs(y + 1.0f / 6.0f) < 1e-6f, "Offset(0) = the mod's phase 0 (0, -0.1667)");
        Offset(7, &x, &y);
        CHECK(std::fabs(x + 0.4375f) < 1e-6f && std::fabs(y - (8.0f / 9.0f - 0.5f)) < 1e-6f, "Offset(7) = (-0.4375, +0.3889)");
        Result r; r.sum8 = 8 * 100; r.all = 90;
        CHECK(std::fabs(r.Stable() - 0.9) < 1e-9 && r.Verdict() == kSteady, "100 px mean, 90 stable = steady");
        r.all = 20;
        CHECK(r.Verdict() == kThin, "20 %% stable = thin");
        r.sum8 = 8 * 7; r.all = 0;
        CHECK(r.Verdict() == kSmall, "7 px mean = too small (no evidence)");
        PairProbe p;
        Result st; st.sum8 = 800; st.all = 80;
        Result th; th.sum8 = 800; th.all = 10;
        Result sm; sm.sum8 = 16; sm.all = 0;
        for (int i = 0; i < 20; ++i) p.Add(sm);
        CHECK(p.Verdict() == kMeasuring && p.tiny == 20 && p.probes == 0, "small probes only: measuring for ever (never tinted)");
        p.Add(th); p.Add(th);
        CHECK(p.Verdict() == kMeasuring, "2 counted probes: still measuring");
        p.Add(st);
        CHECK(p.Verdict() == kThin, "2 of 3 thin: THIN");
        for (int i = 0; i < 6; ++i) p.Add(st);
        CHECK(p.Verdict() == kSteady && p.thin == 2 && p.probes == 9, "2 of 9 thin (< 25 %%): steady");
        PairProbe q;
        q.Add(th); q.Add(st); q.Add(st); q.Add(st);
        CHECK(q.Verdict() == kSteady, "1 thin of 4: steady (one probe is never enough)");
        // the key table: ordinals per pass, a new pass re-uses the slots, verdicts, freshness, a replaced key
        static KeyTable kt;
        const int a0 = kt.Resolve(3, 600, 0, 0, 0, 0, 0, 10), a1 = kt.Resolve(3, 600, 0, 0, 0, 0, 0, 10);
        const int b0 = kt.Resolve(3, 600, 0, 0, 0, 0, 0, 11), b1 = kt.Resolve(3, 600, 0, 0, 0, 0, 0, 11);
        CHECK(a0 >= 0 && a1 >= 0 && a0 != a1 && a0 == b0 && a1 == b1, "the same mesh twice in a pass: two ordinals, the same next pass");
        const int e1 = kt.Resolve(3, 600, 0, 0, 0, 0, 1, 11);
        CHECK(e1 >= 0 && e1 != a0 && e1 != a1, "flat: another world pass (position 1) = another draw");
        // phase 115r (B3): in VR the pass position is not in the key -- eye 1's pass (the next pass serial) finds eye 0's slot
        CHECK(KeyPos(true, 1) == 0 && KeyPos(true, 0) == 0 && KeyPos(false, 1) == 1, "KeyPos: VR 0 for both eyes, flat = the position");
        const int v0 = kt.Resolve(7, 90, 3, 0, 0, 0, KeyPos(true, 0), 40), v1 = kt.Resolve(7, 90, 3, 0, 0, 0, KeyPos(true, 1), 41);
        CHECK(v0 >= 0 && v0 == v1, "VR: both eyes' draws of a mesh share one slot = one verdict (eye 0 no longer eats the probe)");
        int last = 0;
        for (int i = 0; i < kOrdMax + 1; ++i) last = kt.Resolve(4, 36, 12, 0, 0, 0, 0, 20);
        CHECK(last == -1, "more than kOrdMax draws with the same key in a pass: no verdict");
        Result tr; tr.sum8 = 400; tr.all = 5;
        kt.Note(a0, kt.s[a0].key, tr, 1000);
        CHECK(kt.Thin(a0) && kt.Fresh(a0, 1000) && kt.Fresh(a0, 1000 + kKeyFresh) && !kt.Fresh(a0, 1001 + kKeyFresh) && !kt.Thin(a1),
              "a thin probe: that draw thin + fresh for kKeyFresh Presents");
        kt.Note(a1, 12345, tr, 1000);
        CHECK(!kt.Thin(a1), "a note for a replaced key is dropped");
        // phase 115r (B6): two LIVE draws whose keys share a bucket both keep their slots + verdicts (direct-mapped: they evicted each
        // other every pass and never kept one); a third live one gets no slot; a stale one is evicted
        {
            static KeyTable k2;
            int pairs[3] = { -1, -1, -1 };
            uint32_t ics[3] = {};
            const int bk0 = (int)(DrawKey(11, 100, 0, 0, 0, 0, 0, 0) & (uint64_t)(kKeySlots - 2));
            int found = 1;
            ics[0] = 100;
            for (uint32_t ic = 101; ic < 2000000 && found < 3; ++ic)
                if ((int)(DrawKey(11, ic, 0, 0, 0, 0, 0, 0) & (uint64_t)(kKeySlots - 2)) == bk0) ics[found++] = ic;
            CHECK(found == 3, "three keys in one bucket found");
            bool both = true;
            for (uint64_t pass = 100; pass < 110; ++pass) {   // A and B drawn every pass, alternately first
                const int kx = k2.Resolve(11, ics[pass & 1], 0, 0, 0, 0, 0, pass), ky = k2.Resolve(11, ics[(pass + 1) & 1], 0, 0, 0, 0, 0, pass);
                if (pass == 100) { pairs[pass & 1] = kx; pairs[(pass + 1) & 1] = ky; }
                both = both && kx >= 0 && ky >= 0 && kx == pairs[pass & 1] && ky == pairs[(pass + 1) & 1];
                if (pass == 100) { k2.Note(pairs[0], k2.s[pairs[0]].key, tr, 5); }
            }
            CHECK(both && pairs[0] != pairs[1] && k2.Thin(pairs[0]), "two live keys of one bucket keep their own slots every pass (+ the verdict)");
            CHECK(k2.Resolve(11, ics[2], 0, 0, 0, 0, 0, 109) == -1, "a third live key of the bucket: no slot (no eviction of a live one)");
            const int late = k2.Resolve(11, ics[2], 0, 0, 0, 0, 0, 109 + kKeyKeepPasses + 1);
            CHECK(late == pairs[0] || late == pairs[1], "... once a way is stale (kKeyKeepPasses), it is taken");
        }
    }
    // WARP
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        CHECK(false, "WARP device");
        return;
    }
    const UINT W = 512, H = 448;
    auto comp = [](const char* src, const char* prof, ID3DBlob** out) {
        ID3DBlob* err = nullptr;
        const HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", prof, 0, 0, out, &err);
        if (err) { std::printf("%s\n", (const char*)err->GetBufferPointer()); err->Release(); }
        return SUCCEEDED(hr);
    };
    ID3DBlob *vsb = nullptr, *psb = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11InputLayout* il = nullptr;
    ID3D11Buffer* cb = nullptr;
    ID3D11Texture2D *rt = nullptr, *ds = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11DepthStencilState* gameDss = nullptr;
    bool ok = comp(kVs, "vs_5_0", &vsb) && comp(kPs, "ps_5_0", &psb);
    ok = ok && SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs)) &&
         SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps));
    const D3D11_INPUT_ELEMENT_DESC ied = { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    ok = ok && SUCCEEDED(dev->CreateInputLayout(&ied, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il));
    {
        const float c[4] = { 2.0f / W, -2.0f / H, -1.0f, 1.0f };
        D3D11_BUFFER_DESC bd{ 16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA sd{ c, 0, 0 };
        ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, &sd, &cb));
        D3D11_TEXTURE2D_DESC td{ W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
        ok = ok && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &rt)) && SUCCEEDED(dev->CreateRenderTargetView(rt, nullptr, &rtv));
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ok = ok && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &ds)) && SUCCEEDED(dev->CreateDepthStencilView(ds, nullptr, &dsv));
        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&dd, &gameDss));
    }
    CHECK(ok, "WARP resources (VS, PS, layout, targets)");
    if (!ok) return;
    D3D11_QUERY_DESC qd{ D3D11_QUERY_OCCLUSION, 0 };
    ID3D11Query *qSum = nullptr, *qAll = nullptr, *qGame = nullptr, *qPh[kPhases] = {};
    dev->CreateQuery(&qd, &qSum); dev->CreateQuery(&qd, &qAll); dev->CreateQuery(&qd, &qGame);
    for (auto*& q : qPh) dev->CreateQuery(&qd, &q);
    auto get = [&](ID3D11Query* q) { UINT64 v = 0; while (ctx->GetData(q, &v, sizeof(v), 0) == S_FALSE) {} return (uint64_t)v; };
    Kit kit[1];
    const int cur = 3;                                    // the game's current jitter phase (its viewport carries this shift)
    float sx, sy;
    Offset(cur, &sx, &sy);
    std::vector<Mesh> ms = Meshes();
    int thinOk = 0, steadyOk = 0, oldThinSteady = 0, wobbleMiss = 0;
    std::printf("   %-52s %6s %6s %8s %7s %7s  %s\n", "mesh", "tris", "px/tri", "mean px", "wobble", "stable", "verdict (old px/tri rule)");
    for (const Mesh& m : ms) {
        ID3D11Buffer *vb = nullptr, *ib = nullptr;
        D3D11_BUFFER_DESC bd{ (UINT)(m.v.size() * sizeof(V2)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA sd{ m.v.data(), 0, 0 };
        dev->CreateBuffer(&bd, &sd, &vb);
        bd.ByteWidth = (UINT)(m.ix.size() * 4); bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        sd.pSysMem = m.ix.data();
        dev->CreateBuffer(&bd, &sd, &ib);
        // the "game": its targets, a depth-tested draw at its jittered viewport
        const float clr[4] = { 0, 0, 0, 0 };
        ctx->ClearRenderTargetView(rtv, clr);
        ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        ctx->OMSetRenderTargets(1, &rtv, dsv);
        ctx->OMSetDepthStencilState(gameDss, 7);
        const D3D11_VIEWPORT gvp{ sx, sy, (float)W, (float)H, 0.0f, 1.0f };
        ctx->RSSetViewports(1, &gvp);
        UINT stride = sizeof(V2), off = 0;
        ctx->IASetInputLayout(il); ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
        ctx->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0); ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, nullptr, 0); ctx->VSSetConstantBuffers(0, 1, &cb); ctx->PSSetShader(ps, nullptr, 0);
        const UINT ic = (UINT)m.ix.size();
        ctx->Begin(qGame);
        ctx->DrawIndexed(ic, 0, 0);
        ctx->End(qGame);
        const uint64_t gamePx = get(qGame);
        auto redraw = [&] { ctx->DrawIndexed(ic, 0, 0); };
        auto clear = [&](Kit& k) { ctx->ClearDepthStencilView(k.dsv, D3D11_CLEAR_STENCIL, 1.0f, 0); };
        // run 1: per-phase counts (test mode) + all; run 2: the game's path (one query over the 8 passes)
        const bool r1 = Run(ctx, kit, 1, sx, sy, nullptr, qAll, redraw, clear, qPh);
        uint64_t ph[kPhases], mn = ~0ull, mx = 0, sum = 0;
        for (int k = 0; k < kPhases; ++k) { ph[k] = get(qPh[k]); sum += ph[k]; mn = ph[k] < mn ? ph[k] : mn; mx = ph[k] > mx ? ph[k] : mx; }
        const uint64_t all1 = get(qAll);
        const bool r2 = Run(ctx, kit, 1, sx, sy, qSum, qAll, redraw, clear);
        Result res; res.sum8 = get(qSum); res.all = get(qAll);
        CHECK(r1 && r2 && res.sum8 == sum && res.all == all1, "%s: the one-query sum = the per-phase counts (%llu / %llu), I %llu / %llu", m.name,
              (unsigned long long)res.sum8, (unsigned long long)sum, (unsigned long long)res.all, (unsigned long long)all1);
        // the probe counts a pixel ONCE per offset; the game's depth-tested draw counts overlapping triangles at equal depth again
        CHECK(ph[cur] <= gamePx && (ph[cur] > 0) == (gamePx > 0) && (m.overlap || ph[cur] == gamePx), "%s: the probe at the game's own "
              "phase covers the game's pixels, each once (%llu vs %llu)", m.name, (unsigned long long)ph[cur], (unsigned long long)gamePx);
        // the game's state is back
        {
            ID3D11RenderTargetView* r0 = nullptr; ID3D11DepthStencilView* d0 = nullptr;
            ctx->OMGetRenderTargets(1, &r0, &d0);
            ID3D11DepthStencilState* s0 = nullptr; UINT ref = 0;
            ctx->OMGetDepthStencilState(&s0, &ref);
            D3D11_VIEWPORT v0{}; UINT nv = 1;
            ctx->RSGetViewports(&nv, &v0);
            ID3D11PixelShader* p0 = nullptr;
            ctx->PSGetShader(&p0, nullptr, nullptr);
            CHECK(r0 == rtv && d0 == dsv && s0 == gameDss && ref == 7 && nv == 1 && v0.TopLeftX == sx && v0.TopLeftY == sy && p0 == ps,
                  "%s: the game's targets, depth-stencil state + ref, viewport and PS are restored", m.name);
            if (r0) r0->Release();
            if (d0) d0->Release();
            if (s0) s0->Release();
            if (p0) p0->Release();
        }
        const double tris = (double)ic / 3.0, pxTri = (double)gamePx / tris;
        const double mean = (double)sum / kPhases, wob = mean > 0 ? (double)(mx - mn) / mean : 0.0;
        const bool oldThin = pxTri < Census::kThinPxPerTri;
        const int v = res.Verdict();
        std::printf("   %-52s %6.0f %6.2f %8.1f %7.3f %7.3f  %s (%s)\n", m.name, tris, pxTri, mean, wob, res.Stable(), VerdictName(v),
                    oldThin ? "thin" : "not thin");
        if (m.expect) {
            CHECK(v == m.expect, "%s: %s, want %s (stable %.3f)", m.name, VerdictName(v), VerdictName(m.expect), res.Stable());
            if (m.expect == kThin && v == kThin) ++thinOk;
            if (m.expect == kSteady && v == kSteady) { ++steadyOk; if (oldThin) ++oldThinSteady; }
            if (m.expect == kThin && wob < 0.25) ++wobbleMiss;   // what a per-phase wobble rule (thin above 0.25) would miss
        }
        vb->Release(); ib->Release();
    }
    std::printf("   -> %d thin + %d steady meshes judged right (%d of the steady ones were thin by px per triangle); a per-phase coverage "
                "wobble rule (thin above 0.25) would miss %d of the thin ones\n", thinOk, steadyOk, oldThinSteady, wobbleMiss);
    CHECK(oldThinSteady >= 4, "the px-per-triangle rule flags dense steady meshes (%d) that the probe clears", oldThinSteady);
    CHECK(wobbleMiss >= 2, "the wobble idea misses many-post / sloped thin draws (%d) -- why the probe measures per pixel", wobbleMiss);
    CHECK(kit[0].creates == 1, "one stencil buffer for every probe of one size (%d made)", kit[0].creates);
    kit[0].Release();
    for (auto* q : qPh) q->Release();
    qSum->Release(); qAll->Release(); qGame->Release();
    gameDss->Release(); dsv->Release(); ds->Release(); rtv->Release(); rt->Release(); cb->Release(); il->Release();
    vs->Release(); ps->Release(); vsb->Release(); psb->Release();
    ctx->Release(); dev->Release();
}

// phase 113b: the three 2026-11-15 census files replayed with the jitter probe in the loop. The captures hold no per-draw jitter
// data, so each thin CANDIDATE gets the probe outcome its geometry predicts from the WARP cases above (column `pr`): 'T' thin (the
// delineator posts d87f5280 / 2a9f326e and their reflector band db9404ad -- 0.3..1.2 px posts: stable 18 %), 'S' steady (dense
// facades / houses / cars / terrain / grass and flower cards / glass panes / the hill-forest band / the 8 x 10 px billboard: stable
// 62..98 %), 'U' unknown (not enough known about its mesh: the in-game probe decides; a pair being measured is no suspect). Run once
// WITH the known list and once WITHOUT it (which of the 14 entries the probe makes unnecessary).
struct P113bRow { const char* id; uint32_t st; double pxt; int samples; double cov; bool twin; int wirePs; bool wireVs; int solid; bool solidVs; int cut; char pr; };
static void Phase113bReplay(const char* name, const P113bRow* rows, int nRows, const char* const* passName, const int* wire, const int* pole,
                            int nPass, const int* want, const int* wantNoKnown) {
    using namespace Census;
    for (int pass = 0; pass < nPass; ++pass)
        for (int useKnown = 1; useKnown >= 0; --useKnown) {
            int sus = 0, cand = 0, cleared = 0, measuring = 0;
            std::string left;
            for (int i = 0; i < nRows; ++i) {
                const P113bRow& r = rows[i];
                Cover c;
                c.pixelFix = r.twin; c.wirePsKind = r.wirePs; c.wireVs = r.wireVs; c.solidKind = r.solid; c.solidVs = r.solidVs; c.cutReason = r.cut;
                unsigned v0 = 0, p0 = 0;
                sscanf_s(r.id, "%8x_%8x", &v0, &p0);
                int ki = -1;
                KnownByShort(v0, p0, &ki);
                c.knownPair = useKnown ? ki : -1;
                c.wireOn = wire[pass] != 0;
                c.poleOn = pole[pass] != 0;
                const uint64_t tris = 1000ull * (uint64_t)(r.samples > 0 ? r.samples : 1);
                uint64_t px = 0;
                for (int k = 0; k < r.samples; ++k) px += ThinPixels((uint64_t)(r.pxt * 1000.0 + 0.5), r.cov);
                const bool pre = ThinVerdict(px, tris, r.samples);
                // the probe: a PairProbe fed like the in-game one (5 probes of the predicted kind; 'U' = 1 probe: still measuring)
                CensusProbe::PairProbe pp;
                CensusProbe::Result thinR; thinR.sum8 = 8 * 400; thinR.all = 70;      // 18 % stable (the 57-post row on WARP)
                CensusProbe::Result steadyR; steadyR.sum8 = 8 * 400; steadyR.all = 340;   // 85 %
                const int nProbe = r.pr == 'U' ? 1 : 5;
                for (int k = 0; k < nProbe; ++k) pp.Add(r.pr == 'T' ? thinR : steadyR);
                const bool thin = pre && pp.Verdict() == CensusProbe::kThin;
                if (pre) { ++cand; if (pp.Verdict() == CensusProbe::kSteady) ++cleared; if (pp.Verdict() == CensusProbe::kMeasuring) ++measuring; }
                const uint32_t s = Judge(r.st, c, thin).Suspect();
                if (s) { ++sus; left += std::string(" ") + r.id; }
            }
            const int w = useKnown ? want[pass] : wantNoKnown[pass];
            CHECK(sus == w, "%s, %s, %s: %d suspects, want %d (%s)", name, passName[pass], useKnown ? "known list" : "NO known list", sus, w,
                  left.c_str());
            std::printf("   %s, %s, %s: %d rows -> %d suspects:%s | %d px-per-triangle candidates: %d cleared by the probe, %d left to the "
                        "in-game probe\n", name, passName[pass], useKnown ? "with the known list" : "WITHOUT the known list", nRows, sus,
                        left.empty() ? " none" : left.c_str(), cand, cleared, measuring);
        }
}
static void Phase113bReplays() {
    using namespace Census;
    std::printf("phase 113b: the 2026-11-15 census files replayed with the jitter probe (predicted per mesh, see the comment)\n");
    // 10:48 Norway (31 suspects): the Phase111 rows + the probe prediction
    {
        static const P113bRow rows[] = {
        { "25fc1eee_a9d39be8", kCutout, 188.22, 16, -1, false, 0, false, 0, false, kCutPatched, 'S' },
        { "8fcd62e3_a9d39be8", kCutout, 4.20, 42, -1, false, 0, false, 0, false, kCutQueue, 'S' },
        { "3847be86_2d7ed0e6", kCutout, 1.0, 0, -1, false, 0, false, 4, true, kCutSoft, 'S' },
        { "2e01f9a5_a9d39be8", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutApplied, 'S' },
        { "2a9f326e_e19e0136", 0, 0.10, 34, -1, false, 0, false, 8, true, kCutPending, 'T' },
        { "0dfced74_180589ca", kCutout, 0.36, 25, 0.28, false, 0, false, 0, false, kCutSoft, 'U' },
        { "6d2c4e63_438d1a63", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutPatched, 'S' },
        { "07dfaa00_c92c7c22", 0, 0.05, 3, -1, false, 0, false, 0, false, kCutPending, 'S' },
        { "c8dddb32_438d1a63", kCutout, 0.12, 31, 1.0, false, 0, false, 0, false, kCutCoverage, 'S' },
        { "97d9c72d_0a1ed38a", kPixel, 0.28, 34, -1, true, 0, false, 0, false, kCutPending, 'S' },
        { "3c957c7c_18e8e355", 0, 0.30, 4, -1, false, 0, false, 0, false, kCutPending, 'S' },
        { "4186dac5_b97c44a3", kPixel | kCutout, 1.0, 0, -1, true, 2, true, 0, false, kCutCoverage, 'S' },
        { "5ebed025_7c55acee", 0, 0.15, 35, -1, false, 0, false, 0, false, kCutPending, 'S' },
        { "1ec77518_b8636556", 0, 0.14, 40, -1, false, 0, false, 0, false, kCutPending, 'S' },
        { "50cde715_438d1a63", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutPatched, 'S' },
        { "db9404ad_a761133a", kPixel, 0.10, 34, -1, true, 0, false, 0, false, kCutPending, 'T' },
        { "07dfaa00_ad165830", 0, 0.17, 31, -1, false, 0, false, 0, false, kCutPending, 'S' },
        { "9d73eba3_3c3197f0", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutSoft, 'S' },
        { "07dfaa00_2585159b", 0, 0.08, 20, -1, false, 0, false, 9, true, kCutPending, 'U' },
        { "9d73eba3_5396e5a9", 0, 0.28, 20, -1, false, 0, false, 0, false, kCutPending, 'S' },
        { "2f9ff4a3_438d1a63", kCutout, 0.11, 18, -1, false, 0, false, 0, false, kCutPatched, 'S' },
        { "8ba7d8da_fd5e16e5", kPixel, 0.04, 9, -1, true, 0, false, 0, false, kCutPending, 'U' },
        { "07dfaa00_7770320b", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutPatched, 'S' },
        { "d87f5280_2d7ed0e6", kCutout, 0.15, 34, 0.97, false, 0, false, 4, true, kCutSoft, 'T' },
        { "07dfaa00_c55c787d", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutCoverage, 'S' },
        { "0dfced74_a710b6a2", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutSoft, 'S' },
        { "5e03b1ca_41c3064e", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutNoLoss, 'S' },
        { "9d2904cd_2d7ed0e6", kCutout, 1.0, 0, -1, false, 0, false, 4, true, kCutSoft, 'S' },
        { "7fcd07d6_b67da69f", 0, 1.0, 0, -1, false, 0, false, 1, true, kCutPending, 'S' },
        { "07dfaa00_cfc52d43", kCutout, 1.0, 0, -1, false, 0, false, 7, true, kCutSoft, 'S' },
        { "c2ec5608_a9d39be8", kCutout, 1.0, 0, -1, false, 0, false, 0, false, kCutNoLoss, 'S' },
        };
        static_assert(sizeof(rows) / sizeof(rows[0]) == 31, "the 31 suspects of the 10:48 file");
        const char* const pn[3] = { "wire_widen 0 + pole_aa 0 (the ini then)", "pole_aa 1 (the ini now)", "wire_widen 1 + pole_aa 1" };
        const int wire[3] = { 0, 0, 1 }, pole[3] = { 0, 1, 1 };
        // known list: before 113b 4 / 0 / 0 (phase 111); now the 3 posts / band pairs at pole_aa 0, none with pole_aa on
        // (without the list the reflector-band re-light db9404ad_a761133a stays: a thin band no fix recognises by its shaders)
        const int want[3] = { 3, 0, 0 }, wantNo[3] = { 3, 1, 1 };
        Phase113bReplay("10:48 Norway", rows, 31, pn, wire, pole, 3, want, wantNo);
    }
    // 14:54 Volvo Trucks Center (11 suspects): the Phase113 rows + the probe prediction
    {
        static const P113bRow rows[] = {
        { "8fcd62e3_a9d39be8", kCutout, 13.03, 41, -1, false, 0, false, 0, false, kCutLate, 'S' },
        { "6d2c4e63_438d1a63", kCutout, 26.85, 30, -1, false, 0, false, 0, false, kCutLate, 'S' },
        { "c2ec5608_a9d39be8", kCutout, 4.30, 14, -1, false, 0, false, 0, false, kCutNoLoss, 'S' },
        { "d87f5280_2d7ed0e6", kCutout, 0.13, 34, -1, false, 0, false, 4, true, kCutSoft, 'T' },
        { "db9404ad_a761133a", kPixel, 0.10, 34, -1, true, 0, false, 0, false, kCutPending, 'T' },
        { "2a9f326e_e19e0136", 0, 0.10, 34, -1, false, 0, false, 8, true, kCutPending, 'T' },
        { "07dfaa00_2585159b", 0, 0.12, 21, -1, false, 0, false, 9, true, kCutPending, 'U' },
        { "bdce664b_2585159b", 0, 0.60, 17, -1, false, 0, false, 9, true, kCutPending, 'S' },
        { "4186dac5_9c062880", kPixel, 0.12, 8, -1, true, 0, false, 0, false, kCutPending, 'S' },
        { "07dfaa00_f8ac351d", 0, 0.25, 18, -1, false, 0, false, 0, false, kCutPending, 'S' },
        { "faf4c362_e2128488", 0, 0.00, 9, -1, false, 0, false, 0, false, kCutPending, 'U' },
        };
        const char* const pn[3] = { "wire_widen 0 + pole_aa 0 (the ini then)", "pole_aa 1 (the ini now)", "wire_widen 1 + pole_aa 1" };
        const int wire[3] = { 0, 0, 1 }, pole[3] = { 0, 1, 1 };
        const int want[3] = { 3, 0, 0 }, wantNo[3] = { 3, 1, 1 };   // (phase 113: 4 / 1 / 0)
        Phase113bReplay("14:54 Volvo", rows, 11, pn, wire, pole, 3, want, wantNo);
    }
    // 16:33 Volvo Trucks Center, the user's screenshot (12 suspects of the game's dlaa_census.txt at 16:34, wire_widen 0, pole_aa 1):
    // #1 the building's windows / glass doors (bdce664b_2585159b: 18 444 px avg -- big panes: steady), #2 the hill-forest band + the
    // water-edge strip (9d73eba3_38542285: a band 5..10 px tall: steady), #3 a solid 8 x 10 px far billboard (7fcd07d6_b67da69f, its
    // picture.png), the rest 2..111 px avg with no picture of their mesh: 'U'
    {
        static const P113bRow rows[] = {
        { "bdce664b_2585159b", 0, 0.05, 16, -1, false, 0, false, 9, true, kCutPending, 'S' },
        { "9d73eba3_38542285", 0, 0.29, 39, -1, false, 0, false, 0, false, kCutPending, 'S' },
        { "7fcd07d6_b67da69f", 0, 0.15, 38, -1, false, 0, false, 1, true, kCutPending, 'S' },
        { "4186dac5_b0852ebe", kPixel, 0.19, 22, -1, true, 0, false, 0, false, kCutPending, 'U' },
        { "50cde715_438d1a63", kCutout, 0.07, 19, -1, false, 0, false, 0, false, kCutPatched, 'U' },
        { "686f11d3_64e9f0d9", 0, 0.01, 25, -1, false, 0, false, 0, false, kCutPending, 'U' },
        { "07dfaa00_a5577dee", 0, 0.03, 37, -1, false, 0, false, 0, false, kCutPending, 'U' },
        { "d71a9909_8430facf", kPixel, 0.03, 21, -1, true, 0, false, 0, false, kCutPending, 'U' },
        { "3847be86_fa191fed", 0, 0.02, 18, -1, false, 0, false, 0, false, kCutPending, 'U' },
        { "1ec77518_536fcff3", 0, 0.06, 34, -1, false, 0, false, 0, false, kCutPending, 'U' },
        { "0dfced74_62263b8a", 0, 0.05, 30, -1, false, 0, false, 0, false, kCutPending, 'U' },
        { "3847be86_0656c913", 0, 0.02, 34, -1, false, 0, false, 0, false, kCutPending, 'U' },
        };
        const char* const pn[1] = { "wire_widen 0 + pole_aa 1 (the user's screenshot)" };
        const int wire[1] = { 0 }, pole[1] = { 1 };
        const int want[1] = { 0 }, wantNo[1] = { 0 };          // (phase 113: 12)
        Phase113bReplay("16:33 Volvo", rows, 12, pn, wire, pole, 1, want, wantNo);
    }
}

// phase 114: the user's 2026-11-15 evening VR captures (captures\census_2026-11-15_evening\: ETS2 17:26 in a bus, 1 capture; ATS
// 17:39..17:49 by a dealership, 5 captures) -- every suspect of the two dlaa_census.txt files and of the six captures' index.txt.
// Each row: the reason the file printed (BEFORE) and the reason phase 114 gives the same texture (AFTER, from the evidence: the dumped
// texture, its bind flags / mips / format), the in-game jitter probe verdict ('T' THIN in the file, 'S' steady / none), whether the
// pair is a phase-114 known entry. Rows whose texture was never dumped keep their BEFORE reason (07dfaa00_cfc52d43 in ETS2: the
// "shape / size rule" texture is unknown; the two BC1 forward pairs 07dfaa00_7770320b / 102d6301_18de7f8b: opaque or not is decided
// in the game by the new BC1 check) -- the replay does not guess for them.
struct P114Row { const char* id; uint32_t st; int solid; bool solidVs; bool twin; int cutBefore; int cutAfter; char pr; };
static int P114Count(const P114Row* rows, int n, bool after, bool wire, bool pole, std::string* left) {
    using namespace Census;
    int sus = 0;
    for (int i = 0; i < n; ++i) {
        const P114Row& r = rows[i];
        Cover c;
        c.solidKind = r.solid; c.solidVs = r.solidVs; c.pixelFix = r.twin;
        c.cutReason = after ? r.cutAfter : r.cutBefore;
        c.wireOn = wire; c.poleOn = pole;
        unsigned v0 = 0, p0 = 0;
        sscanf_s(r.id, "%8x_%8x", &v0, &p0);
        int ki = -1;
        KnownByShort(v0, p0, &ki);
        int nk = 0;
        KnownPairs(&nk);
        if (!after && ki >= 14) ki = -1;                 // BEFORE = the phase-113 list (14 entries; the 4 of phase 114 come after)
        c.knownPair = ki;
        const bool thin = r.pr == 'T';
        const uint32_t s = Judge(r.st, c, thin).Suspect();
        if (s) { ++sus; if (left) *left += std::string(" ") + r.id + "(" + KindList(s) + ")"; }
    }
    return sus;
}
static void Phase114() {
    using namespace Census;
    std::printf("phase 114: the 2026-11-15 evening VR captures (ETS2 bus, ATS dealership)\n");
    // the new reasons
    CHECK(CutClassOf(kCutOneMip) == kCutClsOk && CutClassOf(kCutRenderTarget) == kCutClsOk && CutClassOf(kCutOpaque) == kCutClsOk,
          "one mip / render target / opaque BC1: OK (no coverage a mip could lose, no stored alpha, nothing cut out)");
    CHECK(CutClassOf(kCutBc1Alpha) == kCutClsBad && CutClassOf(kCutFormat) == kCutClsBad, "a BC1 WITH see-through texels stays BAD");
    CHECK(std::strstr(CutReasonName(kCutOpaque), "BC1") && std::strstr(CutReasonName(kCutRenderTarget), "render target") &&
          std::strstr(CutReasonName(kCutOneMip), "one mip"), "the new reasons have their names");
    CHECK(CutWorse(kCutOpaque, kCutBc1Alpha) == kCutBc1Alpha && CutWorse(kCutRenderTarget, kCutPending) == kCutPending,
          "CutWorse ranks the new reasons with their class");
    // the four phase-114 known entries
    {
        int iCar = -1, iChassis = -1, iAdd = -1, iTerrain = -1;
        KnownByShort(0x07dfaa00u, 0x6ff10d0cu, &iCar);
        KnownByShort(0x07dfaa00u, 0xa5577deeu, &iChassis);
        KnownByShort(0x4186dac5u, 0x68ad6bcbu, &iAdd);
        KnownByShort(0x4a6b7f2au, 0x73c69246u, &iTerrain);
        CHECK(iCar >= 14 && iChassis >= 14 && iAdd >= 14 && iTerrain >= 14, "the 4 phase-114 pairs are on the known list (after the 14)");
        int n = 0;
        const KnownPair* k = KnownPairs(&n);
        for (int i : { iCar, iChassis, iAdd, iTerrain })
            if (i >= 0) CHECK(k[i].kinds == kThin && k[i].fix == kKnownFixNone, "phase-114 known entry %d: thin only, no switch", i);
        if (iCar >= 0) CHECK(std::strstr(k[iCar].what, "phase 80") != nullptr, "the car entry says why it is not dithered (phase 80)");
        Cover c; c.knownPair = iCar; c.cutReason = kCutBc1Alpha;
        CHECK(Judge(kCutout, c, true).Suspect() == kCutout, "a known thin entry never covers a cut-out");
    }
    // the replays
    static const P114Row ets2[] = {
        // the ETS2 17:28 file (4 suspects, wire_widen 0, pole_aa 1) + the 17:26 capture's 07dfaa00_c55c787d
        { "7fcd07d6_c55c787d", kCutout, 0, false, false, kCutSize, kCutCoverage, 'S' },      // bus window seals 32 x 128 (64 %: game mips keep it)
        { "07dfaa00_cfc52d43", kCutout, 7, true, false, kCutSize, kCutSize, 'S' },           // texture of the BAD draws not dumped
        { "07dfaa00_a5577dee", 0, 0, false, false, kCutPending, kCutPending, 'T' },          // truck / trailer chassis: known (phase 80)
        { "4186dac5_8c187c6b", kPixel | kCutout, 0, false, true, kCutFormat, kCutRenderTarget, 'S' },   // render-target atlas
        { "07dfaa00_c55c787d", kCutout, 0, false, false, kCutSize, kCutCoverage, 'S' },      // bus seals with the static VS (same textures)
    };
    static const P114Row ats[] = {
        // the ATS 17:49 file (16 suspects, wire_widen 0, pole_aa 0) + the captures' c2ec5608_a9d39be8 (pennant flags, 17:39)
        { "07dfaa00_cfc52d43", kCutout, 7, true, false, kCutFormat, kCutRenderTarget, 'S' },  // the 2048 RGBA8 render-target atlas
        { "115d4014_9026a063", 0, 0, false, false, kCutPending, kCutPending, 'T' },          // untextured vertex-colour mesh: not identified
        { "6c157c5f_34860eed", 0, 2, true, false, kCutPending, kCutPending, 'T' },           // pair B (fences, towers): wire_widen
        { "07dfaa00_6ff10d0c", 0, 0, false, false, kCutPending, kCutPending, 'T' },          // AI car body: known (phase 80)
        { "4186dac5_68ad6bcb", kPixel, 0, false, true, kCutPending, kCutPending, 'T' },      // forward additive reflection: known
        { "3847be86_36af737f", 0, 0, false, false, kCutPending, kCutPending, 'T' },          // grey concrete with pair D's VS: no port yet
        { "2a9f326e_e19e0136", 0, 8, true, false, kCutPending, kCutPending, 'T' },           // delineator band: pole_aa
        { "db9404ad_a761133a", kPixel, 0, false, true, kCutPending, kCutPending, 'T' },      // its re-light: pole_aa (known)
        { "4a6b7f2a_73c69246", 0, 0, false, false, kCutPending, kCutPending, 'T' },          // terrain strips: known
        { "6c53c441_34860eed", 0, 2, false, false, kCutPending, kCutPending, 'T' },          // unknown VS: the OSGN join decides in game
        { "9d73eba3_9762bfe0", kCutout, 0, false, false, kCutSize, kCutPatched, 'S' },       // car grille 32 x 16: patched by phase 77 now
        { "9d2904cd_d3af2ebb", 0, 5, true, false, kCutPending, kCutPending, 'T' },           // pair E: wire_widen
        { "07dfaa00_7770320b", kCutout, 0, false, false, kCutFormat, kCutFormat, 'S' },      // BC1, never dumped: decided in game
        { "102d6301_18de7f8b", kCutout, 0, false, false, kCutFormat, kCutFormat, 'S' },      // BC1, never dumped: decided in game
        { "4186dac5_8c187c6b", kPixel | kCutout, 0, false, true, kCutFormat, kCutRenderTarget, 'S' },   // render-target atlas
        { "0dfced74_c447bb43", kCutout, 0, false, false, kCutSoft, kCutSoft, 'T' },          // 0 px average, not in a capture
        { "c2ec5608_a9d39be8", kCutout, 0, false, false, kCutFormat, kCutOpaque, 'S' },      // pennant flags: opaque BC1 256 x 256
    };
    struct Run { const char* name; const P114Row* rows; int n; bool wire, pole; int before, after; };
    const Run runs[] = {
        { "ETS2 17:26 bus (wire_widen 0, pole_aa 1: the ini)", ets2, 5, false, true, 5, 1 },
        { "ETS2 17:26 bus, wire_widen 1 + pole_aa 1", ets2, 5, true, true, 5, 1 },
        { "ATS 17:39..17:49 (wire_widen 0, pole_aa 0: the ini)", ats, 17, false, false, 17, 10 },
        { "ATS, pole_aa 1", ats, 17, false, true, 15, 8 },
        { "ATS, wire_widen 1 + pole_aa 1", ats, 17, true, true, 13, 6 },
    };
    for (const Run& r : runs) {
        std::string lb, la;
        const int b = P114Count(r.rows, r.n, false, r.wire, r.pole, &lb), a = P114Count(r.rows, r.n, true, r.wire, r.pole, &la);
        std::printf("   %s: %d rows -> BEFORE %d suspects, AFTER %d:%s\n", r.name, r.n, b, a, la.empty() ? " none" : la.c_str());
        CHECK(b == r.before && a == r.after, "%s: before %d (want %d), after %d (want %d)", r.name, b, r.before, a, r.after);
    }
}

// ---- phase 115: the in-your-face census view -- the mask + the flashing checkerboard painted after DLAA, captured-this-session, the
// bug icon's count, the kind colours (measured on a rendered sample), the VS layout the mask PS links to
namespace P115 {
static const char* kVsCol = R"(
cbuffer C : register(b0) { float4 s; };
struct O { float4 col : COLOR0; float4 pos : SV_Position; };
O main(float3 p : POSITION) { O o; o.col = float4(p, 1); o.pos = float4(p.x * s.x + s.z, p.y * s.y + s.w, p.z, 1.0); return o; }
)";
static const char* kVsPos = R"(
cbuffer C : register(b0) { float4 s; };
struct O { float4 pos : SV_Position; float4 t : TEXCOORD0; };
O main(float3 p : POSITION) { O o; o.pos = float4(p.x * s.x + s.z, p.y * s.y + s.w, p.z, 1.0); o.t = float4(p, 1); return o; }
)";
static const char* kVsTex = R"(
cbuffer C : register(b0) { float4 s; };
struct O { float2 t : TEXCOORD0; float4 pos : SV_Position; };
O main(float3 p : POSITION) { O o; o.t = p.xy; o.pos = float4(p.x * s.x + s.z, p.y * s.y + s.w, p.z, 1.0); return o; }
)";
static const char* kVsCol3 = R"(
cbuffer C : register(b0) { float4 s; };
struct O { float3 col : COLOR0; float4 pos : SV_Position; };
O main(float3 p : POSITION) { O o; o.col = p; o.pos = float4(p.x * s.x + s.z, p.y * s.y + s.w, p.z, 1.0); return o; }
)";
static const char* kPsGame = R"(float4 main() : SV_Target { return float4(0.2, 0.3, 0.4, 1); })";
static std::vector<uint8_t> Comp(const char* src, const char* entry, const char* prof) {
    ID3DBlob *b = nullptr, *e = nullptr;
    std::vector<uint8_t> out;
    if (SUCCEEDED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, prof, 0, 0, &b, &e)))
        out.assign((const uint8_t*)b->GetBufferPointer(), (const uint8_t*)b->GetBufferPointer() + b->GetBufferSize());
    else if (e) std::printf("%s\n", (const char*)e->GetBufferPointer());
    if (b) b->Release();
    if (e) e->Release();
    return out;
}
struct Px { uint8_t r, g, b, a; };
}  // namespace P115

static void Phase115() {
    using namespace Census;
    using namespace P115;
    std::printf("phase 115: the census view in your face -- mask + flashing checkerboard after DLAA, captured this session, bug icon\n");
    // ---- the kind colours (screen bytes): orange must read ORANGE (the user saw the old linear (1, 0.45, 0) as gold / yellow)
    {
        const uint8_t want[5][3] = { {255, 0, 255}, {255, 112, 0}, {0, 255, 255}, {32, 64, 255}, {255, 255, 0} };
        double hue[5];
        for (int k = 0; k < kKindN; ++k) {
            float c[3];
            KindColour(k, c);
            CHECK(std::lround(c[0] * 255) == want[k][0] && std::lround(c[1] * 255) == want[k][1] && std::lround(c[2] * 255) == want[k][2],
                  "kind %d screen colour", k);
            hue[k] = HueDeg(c[0], c[1], c[2]);
        }
        const double oldGold = HueDeg(1.0, 179.0 / 255.0, 0.0);   // what the user's tinted.png showed for the old orange
        std::printf("  hues: magenta %.0f, orange %.0f (the old orange on screen: %.0f = gold), cyan %.0f, blue %.0f, yellow %.0f deg\n",
                    hue[0], hue[1], oldGold, hue[2], hue[3], hue[4]);
        CHECK(hue[1] >= 20.0 && hue[1] <= 32.0, "orange hue %.1f in 20..32 deg (orange, not gold 38..50 / yellow 60)", hue[1]);
        CHECK(oldGold > 38.0, "the old on-screen orange (255,179,0) was gold: %.1f deg", oldGold);
        for (int a = 0; a < kKindN; ++a)
            for (int b = a + 1; b < kKindN; ++b) {
                double d = std::fabs(hue[a] - hue[b]);
                if (d > 180.0) d = 360.0 - d;
                CHECK(d >= 25.0, "kinds %d / %d are %0.f deg apart (>= 25)", a, b, d);
            }
        CHECK(std::fabs(SrgbToLinear(1.0f) - 1.0f) < 1e-6f && std::fabs(SrgbToLinear(112.0f / 255.0f) - 0.162f) < 0.002f &&
              SrgbToLinear(0.0f) == 0.0f, "sRGB -> linear");
    }
    // ---- the mask word, the paint rule, phase, cell, colour
    {
        const uint32_t w = MaskWord(1, true, 0.37f), wn = MaskWord(2, false, 0.37f);
        CHECK(MaskKind(w) == 1 && MaskKind(wn) == 2 && MaskKind(0) == -1 && !(w & kMaskNoDepth) && (wn & kMaskNoDepth), "mask words");
        CHECK(MaskShows(w, true, 0.37f) && !MaskShows(w, true, 0.36f) && !MaskShows(w, true, 0.38f), "a depth word shows only at its depth");
        CHECK(MaskShows(w, true, std::nextafter(0.37f, 1.0f)), "one float step = rounding slack");
        CHECK(MaskShows(MaskWord(0, true, 1e-5f), true, 1e-5f) && !MaskShows(MaskWord(0, true, 1e-5f), true, 1.06e-5f),
              "the far depth range keeps its relative precision (6 %% apart = hidden)");
        CHECK(MaskShows(wn, true, 0.9f) && MaskShows(w, false, 0.9f) && !MaskShows(0, false, 0.f), "no depth / no snapshot = shown");
        // phase 115r (B8): the TEST modes of a draw without depth write -- shown unless the final depth is NEARER
        const uint32_t tl = MaskWord(3, kMaskModeLess, 0.5f), tg = MaskWord(3, kMaskModeGreater, 0.5f);
        CHECK(MaskKind(tl) == 3 && MaskKind(tg) == 3 && (tl & kMaskTest) && !(tl & kMaskGreater) && (tg & kMaskTest) && (tg & kMaskGreater),
              "test words keep the kind; bit 31 test, bit 30 greater");
        CHECK(MaskShows(tl, true, 0.5f) && MaskShows(tl, true, 0.9f) && !MaskShows(tl, true, 0.3f),
              "LESS (nearer = smaller): shown at / behind its depth, hidden by a nearer (smaller) final depth");
        CHECK(MaskShows(tg, true, 0.5f) && MaskShows(tg, true, 0.1f) && !MaskShows(tg, true, 0.8f),
              "GREATER (reversed Z: nearer = larger): shown at / behind, hidden by a nearer (larger) final depth");
        CHECK(MaskShows(tg, true, std::nextafter(0.5f, 1.0f)) && MaskShows(tl, true, std::nextafter(0.5f, 0.0f)), "one float step = slack");
        CHECK(MaskWord(1, true, 0.37f) == MaskWord(1, kMaskModeEqual, 0.37f) && MaskWord(1, false, 0.37f) == MaskWord(1, kMaskModeNone, 0.37f),
              "a bool true = EQUAL, false = no depth (the old callers)");
        CHECK(MaskModeOf(true, true, 4) == kMaskModeEqual && MaskModeOf(true, false, 4) == kMaskModeLess &&
              MaskModeOf(true, false, 7) == kMaskModeGreater && MaskModeOf(true, false, 8) == kMaskModeNone && MaskModeOf(false, true, 4) == kMaskModeNone,
              "MaskModeOf: depth write -> EQUAL; LESS_EQUAL / GREATER_EQUAL without write -> the test; ALWAYS / no depth -> none");
        CHECK(!(MaskWord(0, kMaskModeEqual, 1.0f) & (kMaskTest | kMaskGreater)), "z = 1.0 never sets bits 30 / 31 (the mode bits are free)");
        CHECK(PaintPhase(0.1) == 0 && PaintPhase(0.3) == 1 && PaintPhase(0.6) == 0 && PaintPhase(0.8) == 1, "4 swaps a second");
        int swaps = 0;
        for (int i = 1; i <= 1000; ++i) swaps += PaintPhase(i / 1000.0) != PaintPhase((i - 1) / 1000.0);
        CHECK(swaps == 4, "the checker swaps %d times in 1 s (want 4)", swaps);
        CHECK(PaintCell(1920) == 8 && PaintCell(2900) == 12 && PaintCell(3840) == 12 && PaintCell(2064) == 9 && PaintCell(640) == 8,
              "cell sizes 8..12 px (1920 %d, 2064 %d, 2900 %d, 3840 %d)", PaintCell(1920), PaintCell(2064), PaintCell(2900), PaintCell(3840));
        float a[3], b[3];
        PaintColour(1, 0, 0, 0, 0, 8, 0, a);
        PaintColour(1, 8, 0, 0, 0, 8, 0, b);
        CHECK(a[0] == 1.0f && std::lround(a[1] * 255) == 112 && b[0] == 0 && b[1] == 0 && b[2] == 0, "checker: colour / black neighbours");
        PaintColour(1, 0, 0, 0, 0, 8, 1, a);
        CHECK(a[0] == 0.0f, "phase 1 = inverted");
        PaintColour(1, 3, 5, 4, 4, 8, 0, a);
        CHECK(a[0] == 0.0f, "anchored at the optical centre: the square left of it is the other colour");
    }
    // ---- captured this session + the bug icon's count
    {
        CHECK(!CapturedHidden(0, false, 0) && !CapturedHidden(0, true, 3), "never captured: shown");
        CHECK(CapturedHidden(2, false, 2) && CapturedHidden(2, true, 3), "captured by a finished capture: hidden");
        CHECK(!CapturedHidden(3, true, 3), "captured by the capture still running: shown (tinted.png, its picture.png)");
        CHECK(CapSaves(kCapDump) && CapSaves(kCapLink) && !CapSaves(kCapTextOnly) && !CapSaves(kCapNone), "dump / link save it, text only not");
        const uint64_t shown[6] = { 0, 100, 160, 200, 201, 199 };
        const int cs[6] = { 0, 0, 0, 1, 0, 2 };
        BugCount c = CountBug(shown, cs, 6, 160);
        CHECK(c.toCapture == 2 && c.captured == 2, "count: shown in the window and not captured (%d), captured this session (%d)",
              c.toCapture, c.captured);
        c = CountBug(shown, cs, 6, 300);
        CHECK(c.toCapture == 0 && c.captured == 2, "nothing shown lately: 0 to capture, captured stays");
    }
    // ---- phase 116: a draw whose ONLY kinds a switched-off fix covers is not shown, not counted "to capture", not captured; the bug
    // icon counts it as "fix is off". A draw that also has a kind no fix covers still flashes (in that kind's colour) and counts.
    {
        // the per-draw decision as census_run.h Slow makes it (view on, not captured, the tint cap free): returns the tint kinds
        auto draw = [](uint32_t k, uint32_t fixOff, uint64_t n, uint64_t& shownAt, uint64_t& fixOffAt) -> uint32_t {
            if (k && !ShownKinds(k, fixOff)) { fixOffAt = n + 1; return 0; }
            if (k) { shownAt = n + 1; return ShownKinds(k, fixOff); }
            return 0;
        };
        // A: a pole body with wire_widen + pole_aa off (its only kind: thin, a switched-off fix covers it)
        Cover a; a.solidKind = WireWiden::kSolidD; a.solidVs = true; a.wireOn = a.poleOn = false;
        const uint32_t susA = Judge(0, a, true).Suspect(), offA = FixOffKinds(0, a, true);
        // B: the same pole body + a cut-out whose texture no switch would help (kCutUnknown: no fix covers it)
        Cover b = a; b.cutReason = kCutUnknown;
        const uint32_t susB = Judge(kCutout, b, true).Suspect(), offB = FixOffKinds(kCutout, b, true);
        CHECK(susA == kThin && offA == kThin, "A: suspect thin, all of it fix-off (sus %x off %x)", susA, offA);
        CHECK(susB == (kThin | kCutout) && offB == kThin, "B: suspect thin + cut-out, only thin fix-off (sus %x off %x)", susB, offB);
        uint64_t shown[3] = {}, off[3] = {};
        const int cs[3] = { 0, 0, 0 };
        const uint32_t tA = draw(susA, offA, 99, shown[0], off[0]);
        const uint32_t tB = draw(susB, offB, 99, shown[1], off[1]);
        CHECK(tA == 0 && shown[0] == 0 && off[0] == 100, "A: not tinted (tint %x), noted as hidden by a fix that is off", tA);
        CHECK(tB == kCutout && shown[1] == 100 && off[1] == 0, "B: tinted in the cut-out colour only (tint %x), shown", tB);
        CHECK(!CapWanted(susA, offA) && CapWanted(susB, offB), "capture list: A not captured, B captured");
        // C: a pair with some draws hidden (fix off) and some shown in the same window: counted "to capture" only
        shown[2] = 120; off[2] = 110;
        // the Present count as census_run.h feeds it: "to capture" only for CapWanted pairs, fix-off only while a kind is fix-off
        uint64_t sh[3], of[3];
        const uint32_t sus[3] = { susA, susB, susB }, fo[3] = { offA, offB, offB };
        for (int i = 0; i < 3; ++i) { sh[i] = CapWanted(sus[i], fo[i]) ? shown[i] : 0; of[i] = (sus[i] & fo[i]) ? off[i] : 0; }
        BugCount c = CountBug(sh, cs, 3, 90, of);
        CHECK(c.toCapture == 2 && c.fixOff == 1 && c.captured == 0, "bug icon: A = fix is off (%d), B + C = to capture (%d)", c.fixOff,
              c.toCapture);
        c = CountBug(sh, cs, 3, 200, of);
        CHECK(c.toCapture == 0 && c.fixOff == 0, "outside the window: no fix-off line (%d)", c.fixOff);
        c = CountBug(sh, cs, 3, 90);
        CHECK(c.fixOff == 0 && c.toCapture == 2, "no fix-off array (old callers): no fix-off count");
        // the fix switched back ON: A is covered (no suspect, no count)
        a.wireOn = a.poleOn = true;
        CHECK(Judge(0, a, true).Suspect() == 0 && FixOffKinds(0, a, true) == 0, "A with its fix on: known, nothing to show or count");
        // a fix-off pair the player captured earlier this session stays in "captured"
        const int cs2[1] = { 4 };
        const uint64_t s1[1] = { 0 }, o1[1] = { 100 };
        c = CountBug(s1, cs2, 1, 90, o1);
        CHECK(c.captured == 1 && c.fixOff == 0 && c.toCapture == 0, "captured earlier: counted captured, not fix-off");
    }
    // ---- the VS layout the mask PS links to (synthetic VSs, then the game's own under captures\)
    {
        const std::vector<uint8_t> vc = Comp(kVsCol, "main", "vs_5_0"), vp = Comp(kVsPos, "main", "vs_5_0"), vt = Comp(kVsTex, "main", "vs_5_0"),
                                   v3 = Comp(kVsCol3, "main", "vs_5_0");
        CHECK(VsPosLayout(vc.data(), vc.size()) == kVsLayoutColPos, "COLOR0 float4 o0 + SV_Position o1 -> PSMask2");
        CHECK(VsPosLayout(vp.data(), vp.size()) == kVsLayoutPos0, "SV_Position o0 -> PSMask1");
        CHECK(VsPosLayout(vt.data(), vt.size()) == kVsLayoutOther, "TEXCOORD0 at o0 -> PSMask0 (no depth)");
        CHECK(VsPosLayout(v3.data(), v3.size()) == kVsLayoutOther, "COLOR0 float3 at o0 -> PSMask0 (the PS's float4 would not link)");
        const std::vector<uint8_t> pg = Comp(kPsGame, "main", "ps_5_0");
        CHECK(VsPosLayout(pg.data(), pg.size()) == kVsLayoutOther && VsPosLayout(nullptr, 0) == kVsLayoutOther && VsPosLayout("DXBCx", 5) == 0,
              "a PS / garbage -> 0");
        // the game's VSs (local)
        int n[3] = {}, files = 0;
        std::vector<std::string> dirs = { g_capDir, g_capDir + "census_2026-11-14\\", g_capDir + "phase109_2026-11-14\\" };
        for (const std::string& d : dirs) {
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA((d + "vs_*.dxbc").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                std::vector<uint8_t> b;
                if (!ReadFile(d + fd.cFileName, b)) continue;
                ++files;
                ++n[VsPosLayout(b.data(), b.size())];
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        if (files) {
            std::printf("  game VS files %d: PSMask2 (COLOR0 + SV_Position) %d, PSMask1 (SV_Position o0) %d, other %d\n", files, n[2], n[1], n[0]);
            CHECK(n[0] == 0 && n[1] > 0 && n[2] > 0, "every game VS links to a depth mask PS (other %d)", n[0]);
        } else std::printf("  (no game VS files under %s: skipped)\n", g_capDir.c_str());
    }
    // ---- WARP: the real mask PSs + the real paint (CensusPaint::Paint) on a G-buffer-like pass
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    bool dbg = SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG, &fl, 1, D3D11_SDK_VERSION, &dev,
                                           nullptr, &ctx));
    if (!dbg && FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        CHECK(false, "WARP device");
        return;
    }
    ID3D11InfoQueue* iq = nullptr;
    if (dbg) dev->QueryInterface(__uuidof(ID3D11InfoQueue), (void**)&iq);
    std::printf("  WARP %s\n", iq ? "with the D3D11 debug layer (linkage errors are checked)" : "without the debug layer (linkage not checked)");
    auto errors = [&](bool print = true) -> int {
        if (!iq) return 0;
        int e = 0;
        const UINT64 nm = iq->GetNumStoredMessages();
        for (UINT64 i = 0; i < nm; ++i) {
            SIZE_T len = 0;
            iq->GetMessage(i, nullptr, &len);
            std::vector<char> buf(len);
            D3D11_MESSAGE* m = (D3D11_MESSAGE*)buf.data();
            if (SUCCEEDED(iq->GetMessage(i, m, &len)) && (m->Severity == D3D11_MESSAGE_SEVERITY_ERROR || m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION)) {
                ++e;
                if (print) std::printf("    D3D11 error: %.*s\n", (int)m->DescriptionByteLength, m->pDescription);
            }
        }
        iq->ClearStoredMessages();
        return e;
    };
    const UINT W = 256, H = 192;
    const std::string src = kCensusTintPs;
    const std::vector<uint8_t> vsCol = Comp(kVsCol, "main", "vs_5_0"), vsPos = Comp(kVsPos, "main", "vs_5_0"),
        psGame = Comp(kPsGame, "main", "ps_5_0"), m0 = Comp(src.c_str(), "PSMask0", "ps_5_0"), m1 = Comp(src.c_str(), "PSMask1", "ps_5_0"),
        m2 = Comp(src.c_str(), "PSMask2", "ps_5_0"), pvs = Comp(src.c_str(), "VSPaint", "vs_5_0"), pps = Comp(src.c_str(), "PSPaint", "ps_5_0");
    CHECK(!vsCol.empty() && !vsPos.empty() && !m0.empty() && !m1.empty() && !m2.empty() && !pvs.empty() && !pps.empty(), "shaders compile");
    if (vsCol.empty() || m2.empty() || pps.empty()) return;
    ID3D11VertexShader *vCol = nullptr, *vPos = nullptr;
    ID3D11PixelShader *pGame = nullptr, *pm[3] = {};
    dev->CreateVertexShader(vsCol.data(), vsCol.size(), nullptr, &vCol);
    dev->CreateVertexShader(vsPos.data(), vsPos.size(), nullptr, &vPos);
    dev->CreatePixelShader(psGame.data(), psGame.size(), nullptr, &pGame);
    dev->CreatePixelShader(m0.data(), m0.size(), nullptr, &pm[0]);
    dev->CreatePixelShader(m1.data(), m1.size(), nullptr, &pm[1]);
    dev->CreatePixelShader(m2.data(), m2.size(), nullptr, &pm[2]);
    ID3D11InputLayout* il = nullptr;
    const D3D11_INPUT_ELEMENT_DESC ied = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    dev->CreateInputLayout(&ied, 1, vsCol.data(), vsCol.size(), &il);
    ID3D11Buffer* cb = nullptr;
    {
        const float c[4] = { 2.0f / W, -2.0f / H, -1.0f, 1.0f };
        D3D11_BUFFER_DESC bd{ 16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA sd{ c, 0, 0 };
        dev->CreateBuffer(&bd, &sd, &cb);
    }
    // the game's targets: a colour target + D32_FLOAT_S8X24 depth (typeless: a DSV + the snapshot SRV R32_FLOAT_X8X24)
    auto tex = [&](DXGI_FORMAT f, UINT bind, ID3D11Texture2D** t) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.Format = f; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = bind;
        return SUCCEEDED(dev->CreateTexture2D(&td, nullptr, t));
    };
    ID3D11Texture2D *gcol = nullptr, *gdep = nullptr, *mtex = nullptr;
    ID3D11RenderTargetView *gcolRtv = nullptr, *mRtv = nullptr;
    ID3D11DepthStencilView* gdsv = nullptr;
    ID3D11ShaderResourceView *depSrv = nullptr, *mSrv = nullptr;
    bool ok = tex(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, &gcol) && SUCCEEDED(dev->CreateRenderTargetView(gcol, nullptr, &gcolRtv)) &&
              tex(DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, &gdep) &&
              tex(DXGI_FORMAT_R32_UINT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &mtex) &&
              SUCCEEDED(dev->CreateRenderTargetView(mtex, nullptr, &mRtv)) && SUCCEEDED(dev->CreateShaderResourceView(mtex, nullptr, &mSrv));
    if (ok) {
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
        dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
        ok = SUCCEEDED(dev->CreateDepthStencilView(gdep, &dd, &gdsv)) && SUCCEEDED(dev->CreateShaderResourceView(gdep, &sd, &depSrv));
    }
    CHECK(ok, "targets");
    if (!ok) return;
    ID3D11DepthStencilState *dssWrite = nullptr, *dssEqual = nullptr, *dssTest = nullptr;
    {
        D3D11_DEPTH_STENCIL_DESC d{};
        d.DepthEnable = TRUE; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; d.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;   // reversed-Z
        dev->CreateDepthStencilState(&d, &dssWrite);
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; d.DepthFunc = D3D11_COMPARISON_EQUAL;
        dev->CreateDepthStencilState(&d, &dssEqual);
        d.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        dev->CreateDepthStencilState(&d, &dssTest);
    }
    // quads (x0, y0, x1, y1, z)
    auto quad = [&](float x0, float y0, float x1, float y1, float z) {
        const float v[6][3] = { {x0, y0, z}, {x1, y0, z}, {x0, y1, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z} };
        ID3D11Buffer* vb = nullptr;
        D3D11_BUFFER_DESC bd{ sizeof(v), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA sd{ v, 0, 0 };
        dev->CreateBuffer(&bd, &sd, &vb);
        const UINT stride = 12, off = 0;
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &off);
        ctx->Draw(6, 0);
        vb->Release();
    };
    auto codeCb = [&](int kind, int check) {
        const uint32_t v[4] = { (uint32_t)kind + 1u, (uint32_t)check, 0, 0 };
        ID3D11Buffer* b = nullptr;
        D3D11_BUFFER_DESC bd{ 16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA sd{ v, 0, 0 };
        dev->CreateBuffer(&bd, &sd, &b);
        return b;
    };
    const D3D11_VIEWPORT vp{ 0, 0, (float)W, (float)H, 0, 1 };
    const FLOAT clr[4] = { 0, 0, 0, 0 };
    ctx->ClearRenderTargetView(gcolRtv, clr);
    ctx->ClearRenderTargetView(mRtv, clr);
    ctx->ClearDepthStencilView(gdsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);   // reversed-Z: far = 0
    ctx->IASetInputLayout(il);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->RSSetViewports(1, &vp);
    // the "game" pass: A = an orange cut-out suspect (writes depth 0.5, the COLOR0 VS); its mask re-draw (EQUAL, PSMask2)
    ID3D11Buffer* cbA = codeCb(1, 1);
    ID3D11Buffer* cbC = codeCb(2, 0);
    ID3D11Buffer* cbD = codeCb(3, 1);
    auto gameDraw = [&](ID3D11VertexShader* v, ID3D11DepthStencilState* s, float x0, float y0, float x1, float y1, float z) {
        ctx->OMSetRenderTargets(1, &gcolRtv, gdsv);
        ctx->VSSetShader(v, nullptr, 0);
        ctx->PSSetShader(pGame, nullptr, 0);
        ctx->OMSetDepthStencilState(s, 0);
        quad(x0, y0, x1, y1, z);
    };
    auto maskDraw = [&](ID3D11VertexShader* v, ID3D11PixelShader* p, ID3D11Buffer* code, ID3D11DepthStencilState* s, float x0, float y0, float x1,
                        float y1, float z) {
        ctx->OMSetRenderTargets(1, &mRtv, gdsv);
        ctx->VSSetShader(v, nullptr, 0);
        ctx->PSSetShader(p, nullptr, 0);
        ctx->PSSetConstantBuffers(13, 1, &code);
        ctx->OMSetDepthStencilState(s, 0);
        quad(x0, y0, x1, y1, z);
    };
    errors();
    if (iq) {                                             // the check has teeth: the COLOR0 mask PS on the SV_Position-o0 VS is refused
        iq->SetMuteDebugOutput(TRUE);
        maskDraw(vPos, pm[2], cbC, dssTest, 0, 0, 1, 1, 0.3f);
        maskDraw(vCol, pm[1], cbC, dssTest, 0, 0, 1, 1, 0.3f);
        const int e = errors(false);
        iq->SetMuteDebugOutput(FALSE);
        CHECK(e >= 2, "the debug layer reports a mismatched mask PS (%d errors: the linkage check is real)", e);
        ctx->ClearRenderTargetView(mRtv, clr);            // (the refused draws are undefined)
    }
    gameDraw(vCol, dssWrite, 16, 16, 176, 112, 0.5f);
    maskDraw(vCol, pm[2], cbA, dssEqual, 16, 16, 176, 112, 0.5f);
    CHECK(errors() == 0, "PSMask2 links to a COLOR0 + SV_Position VS (debug layer: no error)");
    // D = a blue water-like suspect with the SV_Position-o0 VS (PSMask1), depth 0.6
    gameDraw(vPos, dssWrite, 16, 130, 80, 180, 0.6f);
    maskDraw(vPos, pm[1], cbD, dssEqual, 16, 130, 80, 180, 0.6f);
    CHECK(errors() == 0, "PSMask1 links to an SV_Position-o0 VS");
    // B = a later, NEARER plain draw over A's right part (depth 0.75): A must not show there (no X-ray)
    gameDraw(vCol, dssWrite, 96, 64, 176, 112, 0.75f);
    // C = a cyan thin suspect drawn WITHOUT depth write (forward), PSMask0: shown where the game's test passed
    gameDraw(vCol, dssTest, 184, 120, 240, 180, 0.3f);
    maskDraw(vCol, pm[0], cbC, dssTest, 184, 120, 240, 180, 0.3f);
    CHECK(errors() == 0, "PSMask0 (no inputs) links to any VS");
    // phase 115r (B8): E = a yellow suspect drawn WITHOUT depth write but with a depth test (GREATER_EQUAL, reversed Z) and a VS the
    // mask PS links to: its word carries its depth in the TEST mode; a later NEARER plain draw over E's right part hides it there
    ID3D11Buffer* cbE = codeCb(4, kMaskModeGreater);
    gameDraw(vCol, dssTest, 96, 130, 170, 180, 0.3f);
    maskDraw(vCol, pm[2], cbE, dssTest, 96, 130, 170, 180, 0.3f);
    gameDraw(vCol, dssWrite, 140, 130, 170, 180, 0.8f);
    // the blit source the paint draws into: R8G8B8A8_TYPELESS (the game's tonemap target), grey; the game's blit state bound
    ID3D11Texture2D* blit = nullptr;
    ID3D11RenderTargetView* blitRtvU = nullptr;
    ID3D11ShaderResourceView* blitSrv = nullptr;
    ok = tex(DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &blit);
    if (ok) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{}; rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        ok = SUCCEEDED(dev->CreateRenderTargetView(blit, &rd, &blitRtvU)) && SUCCEEDED(dev->CreateShaderResourceView(blit, &sd, &blitSrv));
    }
    CHECK(ok, "blit source");
    if (!ok) return;
    CensusPaint::Kit kit;
    CHECK(kit.Ensure(dev, pvs.data(), pvs.size(), pps.data(), pps.size()), "paint kit");
    auto readback = [&](ID3D11Texture2D* t, std::vector<Px>& out) {
        D3D11_TEXTURE2D_DESC td{};
        t->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        ID3D11Texture2D* st = nullptr;
        out.assign((size_t)W * H, Px{});
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &st))) return;
        ctx->CopyResource(st, t);
        D3D11_MAPPED_SUBRESOURCE mp{};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &mp))) {
            for (UINT y = 0; y < H; ++y) memcpy(&out[(size_t)y * W], (const uint8_t*)mp.pData + (size_t)y * mp.RowPitch, W * 4);
            ctx->Unmap(st, 0);
        }
        st->Release();
    };
    auto paintOnce = [&](int phase, float ox, float oy, bool depth, std::vector<Px>& out, bool checkState) {
        const FLOAT grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
        ctx->ClearRenderTargetView(blitRtvU, grey);
        // the game's blit state: RT0 = its colour target, t0 = the blit source, a viewport, its depth state
        ctx->OMSetRenderTargets(1, &gcolRtv, nullptr);
        ctx->PSSetShaderResources(0, 1, &blitSrv);
        ctx->PSSetShader(pGame, nullptr, 0);
        ctx->VSSetShader(vPos, nullptr, 0);
        const D3D11_VIEWPORT gv{ 3, 4, 100, 50, 0, 1 };
        ctx->RSSetViewports(1, &gv);
        ctx->OMSetDepthStencilState(dssTest, 7);
        CensusPaint::Params p;
        p.maskW = W; p.maskH = H; p.dstW = W; p.dstH = H; p.cell = PaintCell(W); p.phase = phase; p.depth = depth; p.ox = ox; p.oy = oy;
        const bool painted = CensusPaint::Paint(ctx, kit, blit, mSrv, depth ? depSrv : nullptr, p);
        CHECK(painted, "painted");
        if (checkState) {
            ID3D11RenderTargetView* r = nullptr; ID3D11DepthStencilView* d = nullptr;
            ctx->OMGetRenderTargets(1, &r, &d);
            ID3D11ShaderResourceView* s0 = nullptr;
            ctx->PSGetShaderResources(0, 1, &s0);
            ID3D11PixelShader* ps = nullptr; ctx->PSGetShader(&ps, nullptr, nullptr);
            ID3D11VertexShader* vs = nullptr; ctx->VSGetShader(&vs, nullptr, nullptr);
            D3D11_VIEWPORT v{}; UINT nv = 1; ctx->RSGetViewports(&nv, &v);
            ID3D11DepthStencilState* ds = nullptr; UINT ref = 0; ctx->OMGetDepthStencilState(&ds, &ref);
            CHECK(r == gcolRtv && !d && s0 == blitSrv && ps == pGame && vs == vPos && nv == 1 && v.TopLeftX == 3 && v.Width == 100 &&
                  ds == dssTest && ref == 7, "the game's blit state is back after the paint (RT0, t0 = the blit source, shaders, viewport, DSS)");
            for (auto* o : { (IUnknown*)r, (IUnknown*)d, (IUnknown*)s0, (IUnknown*)ps, (IUnknown*)vs, (IUnknown*)ds }) if (o) o->Release();
        }
        ctx->PSSetShaderResources(0, 1, std::array<ID3D11ShaderResourceView*, 1>{}.data());
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
        readback(blit, out);
    };
    std::vector<Px> a, b;
    paintOnce(0, 0, 0, true, a, true);
    paintOnce(1, 0, 0, true, b, false);
    auto at = [&](const std::vector<Px>& v, int x, int y) { return v[(size_t)y * W + x]; };
    auto is = [](Px p, int r, int g, int bl) { return std::abs(p.r - r) <= 1 && std::abs(p.g - g) <= 1 && std::abs(p.b - bl) <= 1; };
    const int cell = PaintCell(W);
    // A's visible part: (20, 20) cell (2, 2) -> colour at phase 0; (28, 20) cell (3, 2) -> black
    const Px o1 = at(a, 20, 20), o2 = at(a, 28, 20), o3 = at(b, 20, 20), o4 = at(b, 28, 20);
    std::printf("  rendered: orange square (%d, %d, %d) hue %.1f deg, black square (%d, %d, %d); phase 1: (%d, %d, %d) / (%d, %d, %d)\n",
                o1.r, o1.g, o1.b, HueDeg(o1.r / 255.0, o1.g / 255.0, o1.b / 255.0), o2.r, o2.g, o2.b, o3.r, o3.g, o3.b, o4.r, o4.g, o4.b);
    CHECK(is(o1, 255, 112, 0) && is(o2, 0, 0, 0), "a cut-out suspect = exactly orange (255, 112, 0) and black squares");
    CHECK(is(o3, 0, 0, 0) && is(o4, 255, 112, 0), "phase 1: every square swapped (full on / off)");
    {
        const Px g = at(a, 120, 80), g2 = at(a, 200, 40), g3 = at(a, 10, 10);
        CHECK(is(g, 128, 128, 128) || is(g, 127, 127, 127), "A under the nearer draw B: not painted (no X-ray) (%d %d %d)", g.r, g.g, g.b);
        CHECK((is(g2, 128, 128, 128) || is(g2, 127, 127, 127)) && (is(g3, 128, 128, 128) || is(g3, 127, 127, 127)), "outside: untouched");
    }
    // without the depth snapshot every word shows (the covered part of A is painted too)
    {
        std::vector<Px> c;
        paintOnce(0, 0, 0, false, c, false);
        const Px g = at(c, 120, 80);
        CHECK(is(g, 255, 112, 0) || is(g, 0, 0, 0), "no depth snapshot: the covered part shows too (%d %d %d)", g.r, g.g, g.b);
    }
    // C (no depth word) cyan / black, D (PSMask1) blue / black
    {
        int cy = 0, bk = 0, bl = 0;
        for (int y = 122; y < 178; ++y) for (int x = 186; x < 238; ++x) { const Px p = at(a, x, y); cy += is(p, 0, 255, 255); bk += is(p, 0, 0, 0); }
        CHECK(cy > 1000 && bk > 1000 && cy + bk == 52 * 56, "the forward suspect without a depth word: cyan %d + black %d squares", cy, bk);
        for (int y = 132; y < 178; ++y) for (int x = 18; x < 78; ++x) bl += is(at(a, x, y), 32, 64, 255);
        CHECK(bl > 1000, "the SV_Position-o0 VS's suspect (PSMask1, its depth checked): blue squares %d", bl);
    }
    // phase 115r (B8): E (no depth write, test mode) shows where nothing nearer came later, and NOT under the later nearer draw
    {
        int ye = 0, bke = 0, hid = 0;
        for (int y = 132; y < 178; ++y) for (int x = 98; x < 138; ++x) { const Px p = at(a, x, y); ye += is(p, 255, 255, 0); bke += is(p, 0, 0, 0); }
        for (int y = 132; y < 178; ++y) for (int x = 142; x < 168; ++x) { const Px p = at(a, x, y); hid += is(p, 128, 128, 128) || is(p, 127, 127, 127); }
        CHECK(ye > 500 && bke > 500 && ye + bke == 40 * 46, "phase 115r: a no-depth-write suspect (test mode) shows in the open: yellow %d + black %d", ye, bke);
        CHECK(hid == 26 * 46, "phase 115r: ... and NOT under a later nearer draw (glass / glow over the cab: no show-through), %d of %d grey",
              hid, 26 * 46);
    }
    // the square size: along row 20 inside A (x 16..95: B covers 96+) the colour runs are cell px long
    {
        int runs = 0, okRuns = 0, len = 0;
        bool cur = at(a, 16, 20).r > 128;
        for (int x = 16; x < 96; ++x) {
            const bool c = at(a, x, 20).r > 128;
            if (c == cur) { ++len; continue; }
            ++runs; okRuns += (len == cell || runs == 1);
            cur = c; len = 1;
        }
        CHECK(runs >= 8 && okRuns == runs, "squares of %d px (%d runs)", cell, runs);
    }
    // the anchor (VR: the optical centre) moves the squares
    {
        std::vector<Px> c;
        paintOnce(0, 4, 0, true, c, false);
        CHECK(is(at(a, 17, 20), 255, 112, 0) && is(at(c, 17, 20), 0, 0, 0), "anchor x 4: (17,20) moves into the square before it -> black");
    }
    // an _SRGB-typed blit source: the shader writes linear, the view encodes -> the same bytes
    {
        ID3D11Texture2D* s = nullptr;
        ID3D11RenderTargetView* sr = nullptr;
        if (tex(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &s) &&
            SUCCEEDED(dev->CreateRenderTargetView(s, nullptr, &sr))) {
            ctx->ClearRenderTargetView(sr, clr);
            CensusPaint::Params p;
            p.maskW = W; p.maskH = H; p.dstW = W; p.dstH = H; p.cell = cell; p.depth = true;
            CHECK(CensusPaint::Paint(ctx, kit, s, mSrv, depSrv, p), "painted (_SRGB)");
            std::vector<Px> c;
            readback(s, c);
            CHECK(is(at(c, 20, 20), 255, 112, 0), "_SRGB target: the same orange bytes (%d %d %d)", at(c, 20, 20).r, at(c, 20, 20).g, at(c, 20, 20).b);
        }
        if (sr) sr->Release();
        if (s) s->Release();
    }
    CHECK(errors() == 0, "no D3D11 error in the paint");
    kit.Release();
    for (IUnknown* u : std::initializer_list<IUnknown*>{ vCol, vPos, pGame, pm[0], pm[1], pm[2], il, cb, gcol, gdep, mtex, gcolRtv, mRtv, gdsv, depSrv,
                                                         mSrv, dssWrite, dssEqual, dssTest, cbA, cbC, cbD, blit, blitRtvU, blitSrv })
        if (u) u->Release();
    if (iq) iq->Release();
    ctx->Release();
    dev->Release();
}

// phase 115r (audit B5 / A1): MaskTint's binds, re-draw and restore must pass the mod's hooks UNTRACKED (t_inDlaa.Quiet), like Tint and
// Probe -- else hkOMSetDepthStencilState records the census twin as the game's state. MaskTint needs the whole hook set (no unit test):
// this reads src\census_run.h and checks that every OMSet* / PSSet* / redraw() in MaskTint's body sits between Quiet(true) and the
// restore of the previous value.
static void Phase115r() {
    std::printf("\n== phase 115r: MaskTint binds go through Quiet (source check) ==\n");
    FILE* f = nullptr;
    if (fopen_s(&f, "src\\census_run.h", "rb") != 0 || !f) { std::printf("  (src\\census_run.h not found from here: skipped)\n"); return; }
    std::string src;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) src.append(buf, n);
    fclose(f);
    // the CI checkout has CRLF (autocrlf): drop the CRs so "\n}\n" finds the end of the function in both
    src.erase(std::remove(src.begin(), src.end(), '\r'), src.end());
    const size_t b = src.find("bool MaskTint(ID3D11DeviceContext* ctx");
    CHECK(b != std::string::npos, "MaskTint found in census_run.h");
    if (b == std::string::npos) return;
    const size_t e = src.find("\n}\n", b);
    const std::string body = src.substr(b, e - b);
    const size_t q0 = body.find("t_inDlaa.Quiet(true)"), q1 = body.find("t_inDlaa.Quiet(wasQuiet)");
    CHECK(q0 != std::string::npos && q1 != std::string::npos && q0 < q1, "MaskTint: Quiet(true) ... Quiet(previous value)");
    int calls = 0, outside = 0;
    for (const char* k : { "ctx->OMSet", "ctx->PSSet", "m_c1->PSSet", "redraw()" }) {
        for (size_t p = body.find(k); p != std::string::npos; p = body.find(k, p + 1)) {
            ++calls;
            if (p < q0 || p > q1) ++outside;
        }
    }
    CHECK(calls >= 10 && outside == 0, "MaskTint: all %d binds / re-draws are inside the Quiet window (%d outside)", calls, outside);
}

int main(int argc, char** argv) {
    if (argc > 1) { ScanFolder(argv[1]); return 0; }
    Synthetic();
    RealBytecode();
    Phase110();
    Phase110b();
    Phase111();
    Phase113();
    Phase113b();
    Phase113bReplays();
    Phase114();
    Phase115();
    Phase115r();
    std::printf("census_test: %d checks, %d failed -> %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
