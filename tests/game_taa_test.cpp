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
    in.mode = kTaaOnly;
    CHECK(Decide(in, &why) == kActTaaOnly && !why, "TAA only skips the mod's main-scene DLAA (no fallback reason)");
    // every case the mix cannot take is TAA only, never kActNone (= the mod's DLAA on the TAA'd frame)
    const int kMixModes[2] = { kMix, kSplit };
    for (int m : kMixModes) {
        in.mode = m; in.preHdr = true;
        in.resolveSeen = false; in.jitterOk = true; in.rawOk = true;
        CHECK(Decide(in, &why) == kActTaaOnly && why, "mode %d, no resolve in the pass -> TAA only (%s)", m, why ? why : "?");
        in.resolveSeen = true; in.jitterOk = false;
        CHECK(Decide(in, &why) == kActTaaOnly && why, "mode %d, no game jitter -> TAA only", m);
        in.jitterOk = true; in.rawOk = false;
        CHECK(Decide(in, &why) == kActTaaOnly && why, "mode %d, no raw scene -> TAA only", m);
        in.rawOk = true;
    }
    in.mode = 9;
    CHECK(Decide(in, nullptr) == kActNone, "an out-of-range mode reads as off (and why may be null)");
}

static void TestMenuSteps() {
    CHECK(NextMode(kOff, 1) == kMix && NextMode(kMix, 1) == kTaaOnly && NextMode(kTaaOnly, 1) == kSplit && NextMode(kSplit, 1) == kOff,
          "Right cycles off -> mix -> TAA only -> split -> off");
    CHECK(NextMode(kOff, -1) == kSplit && NextMode(kSplit, -1) == kTaaOnly, "Left goes back");
    CHECK(StepMix(50, 1) == 60 && StepMix(50, -1) == 40, "10 %% steps");
    CHECK(StepMix(100, 1) == 100 && StepMix(0, -1) == 0, "clamped at the ends (no wrap)");
    CHECK(StepMix(45, 1) == 50 && StepMix(45, -1) == 40, "an off-grid dlaa.ini value snaps to the grid");
    CHECK(StepMix(95, 1) == 100 && StepMix(5, -1) == 0, "off-grid near the ends");
    CHECK(ClampMix(-5) == 0 && ClampMix(150) == 100 && ClampMode(4) == kOff && ClampMode(2) == kTaaOnly, "clamps");
    CHECK(!strcmp(ModeName(kOff), "off") && !strcmp(ModeName(kMix), "mix") && !strcmp(ModeName(kTaaOnly), "TAA only") &&
          !strcmp(ModeName(kSplit), "split"), "mode names");
}

static void TestMix() {
    Mix x = MixOf(kMix, 50);
    CHECK(x.modShare > 0.499f && x.modShare < 0.501f && !x.split, "mix 50 %% = half the mod");
    x = MixOf(kMix, 100);
    CHECK(x.modShare == 0.0f, "TAA share 100 %% = none of the mod");
    x = MixOf(kMix, 0);
    CHECK(x.modShare == 1.0f, "TAA share 0 %% = all the mod");
    x = MixOf(kSplit, 30);
    CHECK(x.modShare == 1.0f && x.split, "split: the mod at full share left of the middle");
    x = MixOf(kOff, 30);
    CHECK(x.modShare == 1.0f && !x.split, "off: the composite as before");
}

int main() {
    TestChecksum();
    TestLiveHold();
    TestParseJitter();
    TestDecide();
    TestMenuSteps();
    TestMix();
    printf("game_taa_test: %d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
