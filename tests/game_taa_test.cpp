// phase 82 test: src/game_taa.h -- the game's own TAA together with the mod (dlaa.ini game_taa / game_taa_mix). Pure logic, no GPU.
//   tests\build_game_taa_test.bat  ->  build\tests\game_taa_test.exe   (exit 0 = all checks passed)
// What it pins down: the DXBC checksum test (RenderDoc's "Shader hash" dword order), the live window and the jitter hold, the jitter
// read from the TAA resolve's constants (rows 8 / 11; anything that is not those constants is refused), the per-pass decision (never
// the mod's DLAA on the TAA'd frame: every case the mix cannot take is TAA only), the menu steps and the composite's mix.
#include "../src/game_taa.h"

#include <cstdio>
#include <cstring>

static int s_checks = 0, s_fails = 0;
#define CHECK(cond, ...) do { ++s_checks; if (!(cond)) { ++s_fails; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace GameTaa;

static void TestChecksum() {
    // a DXBC header with the resolve's checksum (bytes 4..19, little-endian dwords) + 4 more bytes
    unsigned char b[24] = { 'D', 'X', 'B', 'C' };
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 4; ++k) b[4 + 4 * i + k] = (unsigned char)(kResolvePs[i] >> (8 * k));
    CHECK(ChecksumIs(b, sizeof(b), kResolvePs), "the resolve checksum is recognised");
    CHECK(!ChecksumIs(b, sizeof(b), kCompressPs), "the resolve bytecode is not the compress pass");
    CHECK(b[4] == 0x9e && b[5] == 0x19 && b[6] == 0x3e && b[7] == 0x6b, "dword 0 = 6b3e199e stored little-endian");
    CHECK(!ChecksumIs(b, 19, kResolvePs), "too short");
    b[0] = 'X';
    CHECK(!ChecksumIs(b, sizeof(b), kResolvePs), "not DXBC");
    CHECK(!ChecksumIs(nullptr, 24, kResolvePs), "null");
}

static void TestLiveHold() {
    CHECK(!Live(0, 100), "never seen = not live");
    CHECK(Live(100, 100) && Live(100, 100 + kLiveFrames), "live through the window");
    CHECK(!Live(100, 101 + kLiveFrames), "not live after it");
    CHECK(!Live(200, 100), "a later stamp than now (counter reset) = not live");
    CHECK(!Hold(kOff, 100, 100), "mode 0 never holds the mod's jitter");
    for (int m = kMix; m < kModeCount; ++m) {
        CHECK(Hold(m, 100, 110), "mode %d holds while live", m);
        CHECK(!Hold(m, 0, 110), "mode %d: no resolve ever = the mod's own jitter", m);
        CHECK(!Hold(m, 100, 100 + kLiveFrames + 1), "mode %d: resolve gone = the mod's own jitter again", m);
    }
    CHECK(!Hold(7, 100, 100) && !Hold(-1, 100, 100), "out-of-range modes read as off");
}

static void FillCb(float* f, float jx, float jy, float w, float h) {
    memset(f, 0, 64 * sizeof(float));
    f[32] = jx; f[33] = jy; f[34] = 0.1f; f[35] = 0.1f;     // row 8 (capture 15407: -5e-05, -6e-05, 0.1, 0.1)
    f[40] = f[41] = f[42] = f[43] = 1.0f;                    // row 10
    f[44] = w; f[45] = h; f[46] = 1.0f; f[47] = 1.0f;        // row 11 = the target size
}

static void TestParseJitter() {
    float f[64], sx = 9.0f, sy = 9.0f;
    FillCb(f, -5e-05f, -6e-05f, 3840.0f, 2160.0f);
    CHECK(ParseJitter(f, 64, 3840, 2160, &sx, &sy), "the capture's constants are accepted");
    CHECK(sx > -0.193f && sx < -0.191f && sy > -0.130f && sy < -0.129f, "UV -> px: (%.4f, %.4f), want (-0.192, -0.1296)", sx, sy);
    FillCb(f, 1.2e-4f, -1.3e-4f, 3840.0f, 2160.0f);
    CHECK(ParseJitter(f, 64, 3840, 2160, &sx, &sy) && sx > 0.46f && sy < -0.28f, "frame 24955-like jitter");
    CHECK(!ParseJitter(f, 64, 2560, 1440, &sx, &sy), "row 11 not the target size = not the TAA constants");
    CHECK(!ParseJitter(f, 47, 3840, 2160, &sx, &sy), "too few floats (row 11 needed)");
    FillCb(f, 0.01f, 0.0f, 3840.0f, 2160.0f);
    CHECK(!ParseJitter(f, 64, 3840, 2160, &sx, &sy), "38 px is no TAA jitter");
    FillCb(f, 0.0f, 0.0f, 3840.0f, 2160.0f);
    f[32] = std::nanf("");
    CHECK(!ParseJitter(f, 64, 3840, 2160, &sx, &sy), "NaN refused");
    FillCb(f, 0.0f, 0.0f, 0.0f, 0.0f);
    CHECK(!ParseJitter(f, 64, 3840, 2160, &sx, &sy), "an all-zero (stale / unwritten) region refused");
    CHECK(!ParseJitter(nullptr, 64, 3840, 2160, &sx, &sy) && !ParseJitter(f, 64, 0, 0, &sx, &sy), "null / no size");
    FillCb(f, 2e-4f, 2e-4f, 1920.0f, 1080.0f);                 // render scale < 100 %: the resolve runs at the render size
    CHECK(ParseJitter(f, 64, 1920, 1080, &sx, &sy) && sx > 0.38f && sx < 0.39f, "render-size target (DLSS case)");
}

static void TestDecide() {
    PassIn in;
    const char* why = "x";
    in.mode = kOff; in.hold = true; in.resolveSeen = in.jitterOk = in.rawOk = in.preHdr = true;
    CHECK(Decide(in, &why) == kActNone && !why, "mode 0 = the mod as before, whatever was seen");
    in.mode = kMix; in.hold = false;
    CHECK(Decide(in, &why) == kActNone && !why, "not held (no live resolve) = the mod as before");
    in.hold = true;
    CHECK(Decide(in, &why) == kActHdr && !why, "flat DLAA pre-tonemap -> the HDR path");
    in.preHdr = false;
    CHECK(Decide(in, &why) == kActLdr && !why, "DLSS / VR / post-tonemap -> the re-issued tonemap path");
    in.mode = kSplit;
    CHECK(Decide(in, &why) == kActLdr, "split uses the same paths");
    // every case the mix cannot take is TAA only, never kActNone (= the mod's DLAA on the TAA'd frame)
    const int kMixModes[2] = { kMix, kSplit };
    for (int m : kMixModes) {
        in.mode = m; in.preHdr = true;
        int code = -1;
        in.resolveSeen = false; in.jitterOk = true; in.rawOk = true;
        CHECK(Decide(in, &why, &code) == kActTaaOnly && why && code == kWhyNoResolve, "mode %d, no resolve in the pass -> TAA only (%s)",
              m, why ? why : "?");
        in.resolveSeen = true; in.jitterOk = false;
        CHECK(Decide(in, &why, &code) == kActTaaOnly && why && code == kWhyNoJitter, "mode %d, no game jitter -> TAA only", m);
        in.jitterOk = true; in.rawOk = false;
        CHECK(Decide(in, &why, &code) == kActTaaOnly && why && code == kWhyNoRaw, "mode %d, no raw scene -> TAA only", m);
        in.rawOk = true;
    }
    in.mode = 9;
    CHECK(Decide(in, nullptr) == kActNone, "an out-of-range mode reads as off (and why may be null)");
}

static void TestMenuSteps() {
    CHECK(NextMode(kOff, 1) == kMix && NextMode(kMix, 1) == kSplit && NextMode(kSplit, 1) == kOff, "Right cycles off -> mix -> split -> off");
    CHECK(NextMode(kOff, -1) == kSplit && NextMode(kSplit, -1) == kMix, "Left goes back");
    CHECK(MigrateMode(0) == kOff && MigrateMode(1) == kMix && MigrateMode(2) == kOff && MigrateMode(3) == kSplit && MigrateMode(7) == kOff,
          "first-build values: 2 (TAA only) -> off, 3 (split) -> split");
    CHECK(StepSplit(50, 1) == 55 && StepSplit(50, -1) == 45 && StepSplit(100, 1) == 100 && StepSplit(0, -1) == 0 && StepSplit(52, 1) == 55,
          "split position: 5 %% steps, clamped, snapped");
    CHECK(SplitX(0.0f, 3840.0f, 50) == 1920.0f && SplitX(100.0f, 1000.0f, 25) == 350.0f && SplitX(0.0f, 2000.0f, 0) == 0.0f,
          "split column = x0 + width x %%");
    CHECK(StepMix(50, 1) == 60 && StepMix(50, -1) == 40, "10 %% steps");
    CHECK(StepMix(100, 1) == 100 && StepMix(0, -1) == 0, "clamped at the ends (no wrap)");
    CHECK(StepMix(45, 1) == 50 && StepMix(45, -1) == 40, "an off-grid dlaa.ini value snaps to the grid");
    CHECK(StepMix(95, 1) == 100 && StepMix(5, -1) == 0, "off-grid near the ends");
    CHECK(ClampMix(-5) == 0 && ClampMix(150) == 100 && ClampMode(3) == kOff && ClampMode(2) == kSplit, "clamps");
    CHECK(!strcmp(ModeName(kOff), "off") && !strcmp(ModeName(kMix), "mix") && !strcmp(ModeName(kSplit), "split"), "mode names");
}

static void TestSplitOverlay() {
    CHECK(SplitLineWidth(1080) == 2.0f && SplitLineWidth(2160) == 4.0f && SplitLineWidth(720) == 2.0f && SplitLineWidth(1440) == 3.0f,
          "the red line: 2 px per 1080 px of height, at least 2");
    const Label l = PackLabel("DLAA (OFF)");
    CHECK(l.n == 10 && (l.p[0] & 31u) == 1u && ((l.p[0] >> 5) & 31u) == 2u && (l.p[1] & 31u) == 0u && ((l.p[1] >> 5) & 31u) == 8u &&
          (l.p[2] & 31u) == 7u && ((l.p[2] >> 5) & 31u) == 9u, "DLAA (OFF) packs D L A A | space ( O F | F )");
    const Label m = PackLabel("MIX 50%");
    CHECK(m.n == 7 && (m.p[0] & 31u) == 10u && ((m.p[0] >> 5) & 31u) == 11u && ((m.p[0] >> 10) & 31u) == 12u &&
          ((m.p[1]) & 31u) == 14u + 5u && ((m.p[1] >> 5) & 31u) == 14u && ((m.p[1] >> 10) & 31u) == 13u, "MIX 50%% packs M I X | 5 0 %%");
    CHECK(PackLabel("TAA").n == 3 && PackLabel("taa").p[0] == PackLabel("TAA").p[0] && PackLabel(nullptr).n == 0, "case, null");
    CHECK(PackLabel("DLSS (OFF)").p[0] < (1u << 20), "a packed part is exact in a float (20 bits)");
    char b[16];
    SideLabel(kSideMod, false, true, true, 50, b, sizeof(b));   CHECK(!strcmp(b, "DLAA"), "mod side: %s", b);
    SideLabel(kSideMod, true, false, true, 50, b, sizeof(b));   CHECK(!strcmp(b, "DLSS (OFF)"), "DLSS that did not run: %s", b);
    SideLabel(kSideTaa, false, true, false, 50, b, sizeof(b));  CHECK(!strcmp(b, "TAA (OFF)"), "TAA not running: %s", b);
    SideLabel(kSideMix, false, true, true, 30, b, sizeof(b));   CHECK(!strcmp(b, "MIX 30%"), "mix side: %s", b);
    SideLabel(kSideMix, false, false, true, 30, b, sizeof(b));  CHECK(!strcmp(b, "MIX (OFF)"), "mix without the mod: %s", b);
    int ls = -1, rs = -1;
    PairSides(kPairDlaaTaa, &ls, &rs); CHECK(ls == kSideMod && rs == kSideTaa, "DLAA | TAA");
    PairSides(kPairMixTaa, &ls, &rs);  CHECK(ls == kSideMix && rs == kSideTaa, "Mix | TAA");
    PairSides(kPairDlaaMix, &ls, &rs); CHECK(ls == kSideMod && rs == kSideMix, "DLAA | Mix");
}

static void TestMix() {
    Mix x = MixOf(kMix, 50);
    CHECK(x.modShare > 0.499f && x.modShare < 0.501f && !x.split, "mix 50 %% = half the mod");
    x = MixOf(kMix, 100);
    CHECK(x.modShare == 0.0f, "TAA share 100 %% = none of the mod");
    x = MixOf(kMix, 0);
    CHECK(x.modShare == 1.0f, "TAA share 0 %% = all the mod");
    x = MixOf(kSplit, 30);
    CHECK(x.modShare == 1.0f && x.split && x.rightShare == 0.0f, "split DLAA | TAA: the mod left, TAA right");
    x = MixOf(kSplit, 30, kPairMixTaa);
    CHECK(x.modShare > 0.699f && x.modShare < 0.701f && x.rightShare == 0.0f, "split Mix | TAA: the mix left (70 %% mod), TAA right");
    x = MixOf(kSplit, 30, kPairDlaaMix);
    CHECK(x.modShare == 1.0f && x.rightShare > 0.699f && x.rightShare < 0.701f, "split DLAA | Mix: the mod left, the mix right");
    CHECK(ShareUsed(kMix, 0) && !ShareUsed(kSplit, kPairDlaaTaa) && ShareUsed(kSplit, kPairMixTaa) && ShareUsed(kSplit, kPairDlaaMix) &&
          !ShareUsed(kOff, kPairMixTaa), "the share row is live in mix and in a split with a Mix side");
    CHECK(NextPair(0, 1) == 1 && NextPair(2, 1) == 0 && NextPair(0, -1) == 2 && ClampPair(5) == 0, "pair steps");
    x = MixOf(kOff, 30);
    CHECK(x.modShare == 1.0f && !x.split, "off: the composite as before");
}

// The first in-game test (ATS flat): the resolve's cb0 rotates over 3 x 2 MB rings, its offset moves 813824 -> 812800, each ring is
// filled by ONE Map / Unmap per frame BEFORE the resolve binds it (and on another thread). Simulated here frame by frame, plus a VR
// frame (two resolves at two offsets of the same ring) and an offset jump beyond the window (it grows, a later frame reads).
static void Copy(void* d, const void* s, size_t n) { memcpy(d, s, n); }
static unsigned char s_ring[3][2u * 1024u * 1024u];
static void TestShadowRotation() {
    const uint32_t kW = 2u * 1024u * 1024u;
    CbShadow sh;
    float out[64];
    auto off = [](int f) -> uint32_t { return 813824u - (uint32_t)(f % 7) * 256u; };
    auto fill = [&](int f, uint32_t o, float j) {
        float c[64];
        memset(c, 0, sizeof(c));
        c[32] = j; c[33] = -j; c[44] = 3840.0f; c[45] = 2160.0f;
        memcpy(&s_ring[f % 3][o], c, sizeof(c));
    };
    int got = 0, misses = 0;
    for (int f = 0; f < 60; ++f) {
        unsigned char* const r = s_ring[f % 3];
        const float j = 1e-5f * (float)(f % 9 + 1);
        fill(f, off(f), j);
        // the game: one Map / Unmap of this frame's ring (all its constants written), then the render thread draws
        if (sh.Watched(r)) { sh.OnMap(r, r); sh.OnUnmap(r, Copy); }
        sh.Learn(r, kW, off(f));
        if (sh.Read(r, off(f), (uint64_t)f, out, 64)) {
            ++got;
            float sx = 0, sy = 0;
            CHECK(ParseJitter(out, 64, 3840, 2160, &sx, &sy) && out[32] == j, "frame %d: the copy holds THIS frame's jitter", f);
        } else {
            ++misses;
        }
    }
    CHECK(misses == 3 && got == 57, "3 rotating rings: only each ring's first frame misses (got %d, missed %d)", got, misses);
    CHECK(!sh.Read(s_ring[59 % 3], off(59), 60, out, 64), "no fresh upload since the last read -> no jitter (never a stale one)");
    {   // VR: two eyes' resolves in one frame, the same ring at two offsets
        unsigned char* const r = s_ring[60 % 3];
        fill(60, 700000u, 2e-5f);
        fill(60, 700256u, 3e-5f);
        sh.OnMap(r, r); sh.OnUnmap(r, Copy);
        sh.Learn(r, kW, 700000u);
        bool grown = false;
        bool ok = sh.Read(r, 700000u, 60, out, 64, &grown);
        if (!ok && grown) { sh.OnMap(r, r); sh.OnUnmap(r, Copy); ok = sh.Read(r, 700000u, 60, out, 64); }   // (the offset jumped 110 KB)
        CHECK(ok && out[32] == 2e-5f, "VR eye 0 reads its offset");
        sh.Learn(r, kW, 700256u);
        CHECK(sh.Read(r, 700256u, 60, out, 64) && out[32] == 3e-5f, "VR eye 1 reads its offset of the same upload");
    }
    {   // an offset far outside the window: that frame misses, the window grows / follows, a later frame there reads
        unsigned char* r = s_ring[61 % 3];
        fill(61, 100000u, 4e-5f);
        sh.OnMap(r, r); sh.OnUnmap(r, Copy);
        bool grown = false;
        sh.Learn(r, kW, 100000u);
        CHECK(!sh.Read(r, 100000u, 61, out, 64, &grown) && grown, "an offset outside the window misses (the range now includes it)");
        r = s_ring[64 % 3];
        fill(64, 100000u, 5e-5f);
        sh.OnMap(r, r); sh.OnUnmap(r, Copy);
        sh.Learn(r, kW, 100000u);
        CHECK(sh.Read(r, 100000u, 64, out, 64) && out[32] == 5e-5f, "the window follows the new place");
    }
    {   // game test 2: offsets far apart in use at the same time (12 KB / 186 KB / 327 KB / 368 KB): once seen, every one is covered
        const uint32_t offs[4] = { 12032u, 185856u, 326656u, 368384u };
        for (uint32_t o : offs) sh.Learn(s_ring[1], kW, o);
        int ok = 0;
        for (int k = 0; k < 4; ++k) {
            const int f = 70 + 3 * k;                    // ring 1 each time
            fill(f, offs[k], 7e-5f + 1e-6f * (float)k);
            sh.OnMap(s_ring[1], s_ring[1]); sh.OnUnmap(s_ring[1], Copy);
            sh.Learn(s_ring[1], kW, offs[k]);
            if (sh.Read(s_ring[1], offs[k], (uint64_t)f, out, 64) && out[32] == 7e-5f + 1e-6f * (float)k) ++ok;
        }
        CHECK(ok == 4, "the window spans the range of the offsets in use: %d of 4 read", ok);
        CHECK(sh.Span() <= CbShadow::kMaxSpan && sh.Span() >= 368384u - 12032u, "span %u within the cap and over the range", sh.Span());
    }
    {   // UpdateSubresource of a watched buffer
        unsigned char src[512];
        float c[64] = {};
        c[32] = 6e-5f; c[44] = 3840.0f; c[45] = 2160.0f;
        memcpy(src + 256, c, sizeof(c));
        sh.Learn(s_ring[0], kW, 100256u);
        CHECK(sh.OnUpdate(s_ring[0], src, 100000u, 512u, Copy), "an UpdateSubresource of a watched buffer is copied");
        CHECK(sh.Read(s_ring[0], 100256u, 65, out, 64) && out[32] == 6e-5f, "... and read at the resolve's offset");
    }
    CHECK(!sh.Watched(nullptr) && !sh.OnUpdate(&got, &got, 0, 4, Copy), "an unwatched buffer is ignored");
}

int main() {
    TestShadowRotation();
    TestChecksum();
    TestLiveHold();
    TestParseJitter();
    TestDecide();
    TestMenuSteps();
    TestMix();
    TestSplitOverlay();
    printf("game_taa_test: %d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
