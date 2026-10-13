// Phase 52 unit test of src/frame_pace.h (the pure part): the flat limiter's deadline rule (no drift, no burst, no wait when
// the game is slower than the cap), the menu's FPS cap steps and the value clamps, and the VR FPS cap's wait target driven
// through a small model of a runtime's xrWaitFrame (wakes the game at fixed display slots). No Windows timer, no D3D, no
// NVAPI, no game. Build: tests\build_frame_pace_test.bat. Exit code 0 = every check passed.
#include "../src/frame_pace.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fails; printf("   FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

using FramePace::NextDeadline;

// A tiny deterministic random source (the same numbers on every run / compiler).
static uint32_t g_rng = 12345u;
static int64_t Rand(int64_t lo, int64_t hi) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return lo + (int64_t)((g_rng >> 8) % (uint32_t)(hi - lo + 1));
}

// The flat limiter as inject.cpp / frame_pace.cpp run it: the game works `work[k]` ns, presents, then waits until the deadline
// (an exact sleep here: the real one overshoots a few us). Returns the present-return times.
static std::vector<int64_t> RunFlat(const std::vector<int64_t>& work, int64_t interval) {
    std::vector<int64_t> t;
    int64_t now = 1000000000LL, dl = 0;
    for (int64_t w : work) {
        now += w;                                         // the frame's work + its Present
        dl = NextDeadline(dl, now, interval);
        if (dl > now) now = dl;                           // the limiter's sleep
        t.push_back(now);
    }
    return t;
}

int main() {
    const int64_t I = FramePace::IntervalNs(60);          // 16.666 ms

    // ---- 1. basics ----
    CHECK(NextDeadline(0, 5000, 0) == 0, "interval 0 = off: no deadline");
    CHECK(NextDeadline(0, 5000, I) == 5000, "first frame: deadline = now (no wait)");
    CHECK(NextDeadline(1000, 2000, I) == 1000 + I, "fast frame: the previous deadline + one interval");
    CHECK(NextDeadline(1000, 1000 + 5 * I, I) == 1000 + 4 * I, "far behind: now - interval (no wait, at most one interval of catch-up)");
    CHECK(FramePace::TimerPartNs(3000000, FramePace::kSpinNs) == 2500000 && FramePace::TimerPartNs(400000, FramePace::kSpinNs) == 0,
          "timer part: all but the last 0.5 ms; nothing under 0.5 ms");
    CHECK(FramePace::IntervalNs(60) == 16666666 && FramePace::IntervalUs(60) == 16666 && FramePace::IntervalUs(0) == 0,
          "intervals");

    // ---- 2. a fast game is held at exactly the cap, without drift (10000 frames, jittery work under the interval) ----
    {
        std::vector<int64_t> w;
        for (int k = 0; k < 10000; ++k) w.push_back(Rand(I / 5, I * 9 / 10));
        const std::vector<int64_t> t = RunFlat(w, I);
        bool exact = true;
        for (size_t k = 1; k < t.size(); ++k) if (t[k] - t[k - 1] != I) { exact = false; break; }
        CHECK(exact, "fast game: every frame exactly one interval after the previous one");
        const int64_t total = t.back() - t.front();
        CHECK(total == (int64_t)(t.size() - 1) * I, "no drift over 10000 frames: %lld ns off", (long long)(total - (int64_t)(t.size() - 1) * I));
    }

    // ---- 3. a game slower than the cap is never delayed ----
    {
        std::vector<int64_t> w(500, I * 3 / 2);
        int64_t now = 0, dl = 0, waited = 0;
        for (int64_t x : w) {
            now += x;
            dl = NextDeadline(dl, now, I);
            if (dl > now) { waited += dl - now; now = dl; }
        }
        CHECK(waited == 0, "slow game (1.5 intervals per frame): waited %lld ns in all", (long long)waited);
    }

    // ---- 4. one hitch: at most ONE interval is caught up afterwards (no burst), then the cadence is exact again ----
    {
        std::vector<int64_t> w(200, I / 2);
        w[100] = 4 * I;                                   // a 66-ms hitch (a city loads)
        const std::vector<int64_t> t = RunFlat(w, I);
        int64_t debt = 0;                                 // how much shorter than an interval the frames after the hitch came
        int shortFrames = 0;
        for (size_t k = 101; k < t.size(); ++k) {
            const int64_t d = t[k] - t[k - 1];
            if (d < I) { debt += I - d; ++shortFrames; }
        }
        CHECK(debt <= I, "after a hitch: %lld ns caught up (at most one interval)", (long long)debt);
        CHECK(shortFrames <= 2, "after a hitch: %d short frames", shortFrames);
        bool steady = true;
        for (size_t k = 110; k < t.size(); ++k) if (t[k] - t[k - 1] != I) steady = false;
        CHECK(steady, "after a hitch: the exact cadence again");
    }

    // ---- 5. a frame a little late, the next one early: the average stays at the cap (jitter absorbed) ----
    {
        std::vector<int64_t> w;
        for (int k = 0; k < 1000; ++k) w.push_back(k % 2 ? I / 3 : I + I / 10);   // every 2nd frame 10 % over the interval
        const std::vector<int64_t> t = RunFlat(w, I);
        const double avg = (double)(t.back() - t.front()) / (double)(t.size() - 1);
        CHECK(avg > (double)I * 0.999 && avg < (double)I * 1.06, "late / early pairs: average %.3f ms (cap %.3f ms)", avg / 1e6, (double)I / 1e6);
    }

    // ---- 6. clamps and the menu steps ----
    CHECK(FramePace::ClampFpsCap(0) == 0 && FramePace::ClampFpsCap(-5) == 0 && FramePace::ClampFpsCap(5) == FramePace::kCapMin &&
          FramePace::ClampFpsCap(60) == 60 && FramePace::ClampFpsCap(99999) == FramePace::kCapMax, "fps_cap clamp");
    CHECK(FramePace::ClampReflex(0) == 0 && FramePace::ClampReflex(1) == 1 && FramePace::ClampReflex(2) == 2 &&
          FramePace::ClampReflex(7) == 2 && FramePace::ClampReflex(-1) == 0, "reflex clamp");
    CHECK(FramePace::ClampVrDiv(0) == 0 && FramePace::ClampVrDiv(1) == 0 && FramePace::ClampVrDiv(2) == 2 &&
          FramePace::ClampVrDiv(3) == 3 && FramePace::ClampVrDiv(4) == 0, "vr_fps_cap clamp");
    CHECK(FramePace::StepCap(0, 1) == 30 && FramePace::StepCap(30, 1) == 40 && FramePace::StepCap(59, 1) == 60 &&
          FramePace::StepCap(360, 1) == 360 && FramePace::StepCap(500, 1) == 360, "FPS cap steps up");
    CHECK(FramePace::StepCap(30, -1) == 0 && FramePace::StepCap(61, -1) == 60 && FramePace::StepCap(0, -1) == 0 &&
          FramePace::StepCap(500, -1) == 360, "FPS cap steps down");
    {
        bool up = true;
        for (int i = 1; i < FramePace::kCapStepN; ++i) if (FramePace::kCapSteps[i] <= FramePace::kCapSteps[i - 1]) up = false;
        CHECK(up && FramePace::kCapSteps[0] == 0, "the step list starts at off and rises");
    }

    // ---- 7. VR: the base period keeps the smallest one reported ----
    {
        int64_t b = 0;
        b = FramePace::VrBasePeriod(b, 0);          CHECK(b == 0, "no period yet");
        b = FramePace::VrBasePeriod(b, 13888888);   CHECK(b == 13888888, "first period");
        b = FramePace::VrBasePeriod(b, 27777777);   CHECK(b == 13888888, "a doubled period (runtime at half rate) is not taken");
        b = FramePace::VrBasePeriod(b, 11111111);   CHECK(b == 11111111, "a shorter period (90 Hz) is taken");
        CHECK(FramePace::VrWaitTarget(0, 13888888, 2) == 0 && FramePace::VrWaitTarget(5, 0, 2) == 0 &&
              FramePace::VrWaitTarget(5, 13888888, 0) == 0 && FramePace::VrWaitTarget(5, 13888888, 1) == 0, "no target when off / unanchored");
    }

    // ---- 8. VR: a model runtime -- xrWaitFrame wakes the game at the first wake point (phase + k * P) at or after the call;
    //         the game works `work`, then calls xrWaitFrame after the cap's sleep (VrWaitTarget; the timer may wake late).
    //         The game's frames must land exactly every div display slots while it is fast enough, and never on a slot
    //         earlier than the cap allows. ----
    for (int hz : { 72, 80, 90, 120 }) {
        for (int div : { 2, 3 }) {
            const int64_t P = 1000000000LL / hz, phase = P / 3;
            auto wake = [&](int64_t call) {               // the runtime's next wake point at or after `call`
                const int64_t k = (call - phase + P - 1) / P;
                return phase + (k < 0 ? 0 : k) * P;
            };
            int64_t ret = wake(5 * P), base = 0;
            base = FramePace::VrBasePeriod(base, P);
            int ok = 0, n = 0, tooEarly = 0;
            for (int f = 0; f < 3000; ++f) {
                const int64_t work = Rand(P / 10, (int64_t)div * P - P / 2);   // fast enough for the cap (also a 1-period-long frame)
                const int64_t late = Rand(0, P / 5);                           // the timer wakes up to 0.2 periods late
                int64_t call = ret + work;
                const int64_t target = FramePace::VrWaitTarget(ret, base, div);
                if (target > call) call = target + late;
                const int64_t r2 = wake(call);
                const int64_t slots = (r2 - ret + P / 2) / P;
                ++n;
                if (slots == div) ++ok;
                if (slots < div) ++tooEarly;
                ret = r2;
            }
            CHECK(ok == n && tooEarly == 0, "VR %d Hz 1/%d: %d of %d frames on every %d-th slot, %d too early", hz, div, ok, n, div, tooEarly);
        }
    }

    // ---- 9. VR: a game slower than the cap is not delayed (the target is already past when it calls) ----
    {
        const int64_t P = 1000000000LL / 72;
        int64_t ret = 1000 * P, delayed = 0;
        for (int f = 0; f < 500; ++f) {
            const int64_t call = ret + 2 * P + P / 10;    // a game that needs a bit more than 2 periods
            if (FramePace::VrWaitTarget(ret, P, 2) > call) ++delayed;
            ret += 3 * P;
        }
        CHECK(delayed == 0, "VR slow game: %lld waits delayed", (long long)delayed);
    }

    printf("%s: %d / %d checks passed\n", g_fails ? "FAILED" : "PASSED", g_checks - g_fails, g_checks);
    return g_fails ? 1 : 0;
}
