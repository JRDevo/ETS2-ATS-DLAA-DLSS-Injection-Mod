// Phase 41 unit test of src/mirror_turns.h (mirrors in turns): the per-view run / hold decision, the history-gap rule and the
// slot balance, driven by a small frame simulation of the mirror units the way inject.cpp's MirBeginView / MirFlush /
// MirTurnsEndFrame use them. No D3D, no game. Build: tests\build_mirror_turns_test.bat. Exit code 0 = every check passed.
#include "../src/mirror_turns.h"
#include <cstdio>
#include <cstdint>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fails; printf("   FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct SimUnit {
    uint32_t w, h;
    MirTurns::UnitState st;
    bool     everRun = false, heldPrev = false;
    uint64_t lastRun = 0, heldFrame = 0;
    int      runs = 0, holds = 0, maxGap = 0, gapResets = 0;
};

// One simulated frame: every unit in `inView` gets a view; returns the pixels that ran the DLAA path. forceCut / debug as
// inject.cpp passes them; resetMask / epochMask = units with a pending reset / an epoch change this frame.
static uint64_t Frame(std::vector<SimUnit>& U, uint64_t fr, uint32_t inView, bool forceCut, uint32_t resetMask, uint32_t epochMask,
                      uint32_t noResultMask, bool debug, uint32_t* ranMask, int* whyOut) {
    uint64_t px = 0;
    *ranMask = 0;
    for (size_t i = 0; i < U.size(); ++i) {
        SimUnit& u = U[i];
        if (!(inView & (1u << i))) continue;
        u.st.resetPending = !u.everRun || (resetMask & (1u << i));
        u.st.epochStale = (epochMask & (1u << i)) != 0;
        u.st.haveResult = u.everRun && !(noResultMask & (1u << i));
        u.st.lastRunFrame = u.lastRun;
        u.st.runs = (uint64_t)u.runs;
        int why = -1;
        const bool run = MirTurns::Runs(fr, u.st, forceCut, debug, &why);
        if (whyOut) whyOut[i] = why;
        if (run) {
            // MirFlush: the history is kept over the gap? (the first run always resets)
            if (u.everRun) {
                const bool keep = MirTurns::GapKeepsHistory(fr, u.lastRun, true, u.heldFrame + 1 == fr);
                if (!keep) ++u.gapResets;
                const int gap = (int)(fr - u.lastRun);
                if (gap > u.maxGap) u.maxGap = gap;
            }
            u.lastRun = fr; u.everRun = true; ++u.runs;
            *ranMask |= 1u << i;
            px += (uint64_t)u.w * u.h;
        } else {
            // a hold: only ever right after a run (never 2 frames stale)
            CHECK(u.everRun && u.lastRun + 1 == fr, "frame %llu unit %zu held although its last run was frame %llu",
                  (unsigned long long)fr, i, (unsigned long long)u.lastRun);
            u.heldFrame = fr; ++u.holds;
        }
    }
    return px;
}

static void Balance(std::vector<SimUnit>& U) {
    std::vector<uint64_t> a;
    for (auto& u : U) a.push_back((uint64_t)u.w * u.h);
    std::vector<int> s(U.size());
    MirTurns::AssignSlots(a.data(), (int)U.size(), s.data());
    for (size_t i = 0; i < U.size(); ++i) U[i].st.slot = s[i];
}

int main() {
    printf("phase 41 mirrors in turns: unit test of src/mirror_turns.h\n");

    // ---- 1. slot balance: ETS2 flat 512x1024 x2 + 512x512 + 512x256; ATS VR 2048x512 + 1024x512 (+ 256x512) ----
    {
        const uint64_t a[4] = { 512 * 1024, 512 * 1024, 512 * 512, 512 * 256 };
        int s[4] = { -1, -1, -1, -1 };
        MirTurns::AssignSlots(a, 4, s);
        CHECK(s[0] != s[1], "the two 512x1024 mirrors share slot %d", s[0]);
        uint64_t sum[2] = { 0, 0 };
        for (int i = 0; i < 4; ++i) sum[s[i]] += a[i];
        const uint64_t hi = sum[0] > sum[1] ? sum[0] : sum[1], lo = sum[0] > sum[1] ? sum[1] : sum[0];
        CHECK(hi * 100 <= (hi + lo) * 60, "ETS2 set balance %llu / %llu px", (unsigned long long)sum[0], (unsigned long long)sum[1]);
        printf("   ETS2 flat set: slots %d %d %d %d, %.2f / %.2f MP per slot (all %.2f MP)\n", s[0], s[1], s[2], s[3],
               (double)sum[0] / 1e6, (double)sum[1] / 1e6, (double)(sum[0] + sum[1]) / 1e6);
        const uint64_t b[3] = { 2048 * 512, 1024 * 512, 256 * 512 };
        int t[3];
        MirTurns::AssignSlots(b, 3, t);
        CHECK(t[0] != t[1], "ATS 2048x512 and 1024x512 share slot %d", t[0]);
        CHECK(t[1] == t[2], "ATS: the small 256x512 goes with the lighter 1024x512 (slots %d %d %d)", t[0], t[1], t[2]);
        const uint64_t c[2] = { 1000, 1000 };
        int q[2];
        MirTurns::AssignSlots(c, 2, q);
        CHECK(q[0] == 0 && q[1] == 1, "equal areas: index order onto slots 0 / 1 (got %d %d)", q[0], q[1]);
        int one = -1;
        MirTurns::AssignSlots(c, 1, &one);
        CHECK(one == 0, "a single unit: slot 0 (got %d)", one);
        CHECK(MirTurns::PickSlot(10, 5, 1, 1) == 1 && MirTurns::PickSlot(5, 10, 1, 1) == 0 && MirTurns::PickSlot(5, 5, 2, 1) == 1 &&
              MirTurns::PickSlot(5, 5, 1, 1) == 0, "PickSlot: lighter, then fewer, then 0");
    }

    // ---- 2. the history-gap rule ----
    CHECK(MirTurns::GapKeepsHistory(11, 10, false, false), "gap 1 keeps (turns off)");
    CHECK(MirTurns::GapKeepsHistory(12, 10, true, false), "gap 2 keeps with turns on");
    CHECK(!MirTurns::GapKeepsHistory(12, 10, false, false), "gap 2 resets with turns off (no hold between: as before phase 41)");
    CHECK(MirTurns::GapKeepsHistory(12, 10, false, true), "gap 2 keeps after a hold (turns just switched off)");
    CHECK(!MirTurns::GapKeepsHistory(13, 10, true, true), "gap 3 resets");
    CHECK(!MirTurns::GapKeepsHistory(10, 10, true, false), "the same frame is no gap of 1");

    // ---- 3. frame simulation: 4 ETS2 mirrors, 240 frames, events on the way ----
    {
        std::vector<SimUnit> U = { { 512, 1024 }, { 512, 1024 }, { 512, 512 }, { 512, 256 } };
        Balance(U);
        const uint64_t all = 512 * 1024 * 2 + 512 * 512 + 512 * 256;
        uint64_t slotPx[2] = { 0, 0 };
        for (auto& u : U) slotPx[u.st.slot] += (uint64_t)u.w * u.h;
        const uint64_t peakTurn = slotPx[0] > slotPx[1] ? slotPx[0] : slotPx[1];
        uint64_t pxSum = 0, frames = 0, peakSteady = 0;
        int steadyFrames = 0, steadyBad = 0;
        for (uint64_t fr = 100; fr < 340; ++fr) {
            uint32_t inView = 0xF;
            if (fr == 200) inView &= ~(1u << 1);              // unit 1 not rendered this frame (the game skips a view now and then)
            const bool cut = fr == 260 || fr == 261;          // a camera cut noted at frame 260 (MirTurnsNoteCut: this frame + next)
            const uint32_t reset = fr == 150 ? (1u << 2) : 0u;   // unit 2 re-homed (history reset)
            const uint32_t epoch = fr == 180 ? 0xFu : 0u;        // MirInvalidate (a DLAA key)
            const uint32_t noRes = fr == 300 ? (1u << 3) : 0u;   // unit 3 without a result of its size
            const bool debug = fr == 320;
            uint32_t ran = 0;
            int why[4] = { -1, -1, -1, -1 };
            const uint64_t px = Frame(U, fr, inView, cut, reset, epoch, noRes, debug, &ran, why);
            pxSum += px; ++frames;
            if (fr == 100) CHECK(ran == 0xF, "the first frame runs every unit (ran mask %x)", ran);
            if (fr == 150) CHECK(ran & (1u << 2), "a reset makes unit 2 run out of turn (why %d)", why[2]);
            if (fr == 180) CHECK(ran == 0xF, "an epoch change makes every unit run (ran mask %x)", ran);
            if (fr == 201) CHECK(ran & (1u << 1), "unit 1 runs right after its missing frame (why %d)", why[1]);
            if (fr == 260 || fr == 261) CHECK(ran == 0xF, "frame %llu: a camera cut runs every unit (ran mask %x)",
                                              (unsigned long long)fr, ran);
            if (fr == 300) CHECK(ran & (1u << 3), "no result to show makes unit 3 run (why %d)", why[3]);
            if (fr == 320) CHECK(ran == 0xF, "the jitter-only debug key: no unit holds (ran mask %x)", ran);
            // steady frames (no event in this or the last frame): exactly one slot runs
            const bool event = fr <= 101 || (fr >= 150 && fr <= 151) || (fr >= 180 && fr <= 181) || (fr >= 200 && fr <= 202) ||
                               (fr >= 260 && fr <= 262) || (fr >= 300 && fr <= 301) || (fr >= 320 && fr <= 321);
            if (!event) {
                ++steadyFrames;
                if (px > peakSteady) peakSteady = px;
                uint32_t slotMask = 0;
                for (int i = 0; i < 4; ++i) if (((fr + (uint64_t)U[i].st.slot) & 1u) == 0) slotMask |= 1u << i;
                if (ran != slotMask) {
                    ++steadyBad;
                    if (steadyBad <= 3) printf("   frame %llu: ran %x, the turn's slot mask %x\n", (unsigned long long)fr, ran, slotMask);
                }
            }
        }
        CHECK(steadyBad == 0, "%d of %d steady frames ran another set than the turn's slot", steadyBad, steadyFrames);
        CHECK(peakSteady <= peakTurn, "steady peak %llu px above the larger slot %llu px", (unsigned long long)peakSteady,
              (unsigned long long)peakTurn);
        int gapResets = 0, maxGap = 0, holds = 0, runs = 0;
        for (auto& u : U) { gapResets += u.gapResets; if (u.maxGap > maxGap) maxGap = u.maxGap; holds += u.holds; runs += u.runs; }
        CHECK(gapResets == 0, "%d runs lost their history over the gap", gapResets);
        CHECK(maxGap == 2, "the longest gap between two runs of a unit is %d frames (want 2)", maxGap);
        const double share = (double)pxSum / (double)(all * frames);
        CHECK(share > 0.45 && share < 0.60, "share of the mirror pixels processed %.3f (want about half)", share);
        printf("   ETS2 4 mirrors, %llu frames: %d runs, %d holds, %.1f %% of the all-mirrors pixels processed per frame on average, "
               "steady peak %.2f MP of %.2f MP, longest gap %d frames, history resets over gaps %d\n", (unsigned long long)frames,
               runs, holds, 100.0 * share, (double)peakSteady / 1e6, (double)all / 1e6, maxGap, gapResets);
    }

    // ---- 4. one mirror (mode 2, mirror_vr_views 1): every 2nd frame; a hold never follows a hold ----
    {
        std::vector<SimUnit> U = { { 2048, 512 } };
        Balance(U);
        int runs = 0;
        for (uint64_t fr = 7; fr < 107; ++fr) {
            uint32_t ran = 0;
            Frame(U, fr, 1u, false, 0, 0, 0, false, &ran, nullptr);
            if (ran) ++runs;
        }
        CHECK(runs == 50 || runs == 51, "one mirror over 100 frames ran %d times (want 50 / 51)", runs);
        CHECK(U[0].maxGap == 2 && U[0].gapResets == 0, "one mirror: longest gap %d, history resets %d", U[0].maxGap, U[0].gapResets);
    }

    // ---- 5. a slot moved by a re-balance (a mirror appeared): never 2 frames stale ----
    {
        std::vector<SimUnit> U = { { 1024, 512 }, { 1024, 512 } };
        Balance(U);
        for (uint64_t fr = 10; fr < 60; ++fr) {
            if (fr == 31) U[0].st.slot ^= 1;                  // MirTurnsEndFrame moved unit 0 to the other slot
            if (fr == 44) U[1].st.slot ^= 1;
            uint32_t ran = 0;
            Frame(U, fr, 3u, false, 0, 0, 0, false, &ran, nullptr);
        }
        CHECK(U[0].maxGap <= 2 && U[1].maxGap <= 2 && U[0].gapResets == 0 && U[1].gapResets == 0,
              "slot moves: longest gaps %d / %d, resets %d / %d", U[0].maxGap, U[1].maxGap, U[0].gapResets, U[1].gapResets);
    }

    printf("%s: %d / %d checks passed\n", g_fails ? "FAILED" : "PASSED", g_checks - g_fails, g_checks);
    return g_fails ? 1 : 0;
}
