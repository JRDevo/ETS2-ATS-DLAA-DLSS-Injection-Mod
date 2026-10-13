// FramePace -- phase 52: a built-in FRAME CAP and NVIDIA REFLEX (flat), and the VR FPS CAP's pacing maths (xr_hook.cpp).
//
// Why: ETS2 / ATS have no frame limiter of their own besides vsync. A cap below what the PC can hold keeps the frame time
// steady (no hitch when a city loads), saves power and GPU heat, and in flat Reflex cuts the render queue (lower input lag
// when the GPU is the limit). In VR a fixed HALF (or third) of the headset rate gives the runtime's reprojection (ASW /
// Motion Smoothing / SSW) a steady cadence to fill in: a game that swings between 72 and 50 fps makes the reprojection
// switch in and out (judder); a game locked at 36 gets one synthetic frame between every two real ones, always.
//
// What (dlaa.ini, all 0 = off by default; the tuning menu's "Frame rate" rows switch them live):
//  - reflex = 0 off / 1 on / 2 on + boost. Flat only, NVIDIA only (NVAPI: NvAPI_D3D_SetSleepMode when a setting changes,
//    NvAPI_D3D_Sleep once per frame right after the game's Present returns = the start of the next frame's simulation on the
//    presenting thread; latency markers SIMULATION_START after the Sleep, PRESENT_START / PRESENT_END around Present).
//    A refused NvAPI_D3D_SetSleepMode (not NVIDIA / old driver) is logged once; the cap then falls back to the own limiter.
//  - fps_cap = 0 off, else frames per second (20..1000). Flat only. With Reflex running: the driver limits
//    (minimumIntervalUs, inside its Sleep). Else the OWN limiter after Present: a high-resolution waitable timer for the bulk,
//    a spin for the last 0.5 ms, against a DEADLINE: next = max(prev + interval, now - interval) -- no drift (the deadlines
//    step by exactly one interval while the game keeps up) and no burst (after a slow frame at most one interval is caught up).
//  - vr_fps_cap = 0 off / 2 half / 3 a third of the headset rate. VR only (xr_hook.cpp, the game's xrWaitFrame): the game's
//    real xrWaitFrame is DELAYED until VrWaitTarget below (the last real wait's return + div display periods - a quarter
//    period), then called as always. The runtime then wakes the game at the display slot div periods after the last one;
//    its predicted display time stays the runtime's own (no made-up time, nothing about wait / begin / end order changes).
//    Reflex Sleep never runs in VR (the runtime paces through xrWaitFrame); the flat fps_cap neither.
//
// This header is the PURE part (no Windows, no D3D, no NVAPI): the deadline / target maths and the value rules, checked by
// tests/frame_pace_test.cpp. frame_pace.cpp has the NVAPI calls, the sleeper and the stats.
#pragma once
#include <cstdint>

struct IDXGISwapChain;

namespace FramePace {

// ---- pure maths (tests/frame_pace_test.cpp) ----------------------------------------------------------------------------------
constexpr int     kCapMin = 20, kCapMax = 1000;      // dlaa.ini fps_cap range (0 = off)
constexpr int64_t kSpinNs = 500000;                  // the own limiter spins the last 0.5 ms (the timer sleeps the rest)

inline int ClampFpsCap(long v) { return v <= 0 ? 0 : (v < kCapMin ? kCapMin : (v > kCapMax ? kCapMax : (int)v)); }
inline int ClampReflex(long v) { return v <= 0 ? 0 : (v >= 2 ? 2 : 1); }
inline int ClampVrDiv(long v)  { return v == 2 || v == 3 ? (int)v : 0; }          // 1 = every frame = off
inline int64_t IntervalNs(int fps) { return fps > 0 ? 1000000000LL / fps : 0; }
inline uint32_t IntervalUs(int fps) { return fps > 0 ? (uint32_t)(1000000 / fps) : 0u; }

// The own limiter: the deadline of the frame that just presented. prevDeadline = the previous frame's deadline (0 = none:
// the first frame, or the limiter was off / reset -- no wait). The caller waits until the result when it is in the future.
inline int64_t NextDeadline(int64_t prevDeadline, int64_t now, int64_t interval) {
    if (interval <= 0) return 0;
    if (prevDeadline <= 0) return now;
    const int64_t steady = prevDeadline + interval, floor = now - interval;
    return steady > floor ? steady : floor;
}
// The part of a wait of `remaining` ns the timer sleeps (the rest is spun).
inline int64_t TimerPartNs(int64_t remaining, int64_t spin) { return remaining > spin ? remaining - spin : 0; }

// The menu's FPS cap steps (Left / Right). A dlaa.ini value between two steps goes to the next step in that direction.
constexpr int kCapSteps[] = { 0, 30, 40, 45, 50, 60, 72, 75, 80, 90, 100, 120, 144, 165, 240, 360 };
constexpr int kCapStepN = (int)(sizeof(kCapSteps) / sizeof(kCapSteps[0]));
inline int StepCap(int cur, int dir) {
    if (dir > 0) {
        for (int i = 0; i < kCapStepN; ++i) if (kCapSteps[i] > cur) return kCapSteps[i];
        return kCapSteps[kCapStepN - 1];
    }
    for (int i = kCapStepN - 1; i >= 0; --i) if (kCapSteps[i] < cur) return kCapSteps[i];
    return 0;
}

// VR FPS cap: the headset's frame period. `base` = the smallest period the runtime reported in this run of waits (0 = none):
// a runtime that already halves the rate itself (ASW locked to 45) may report a doubled period -- dividing that again would
// give a quarter. The caller re-seeds (base 0) after a gap in the waits (a new session, maybe at another refresh rate).
inline int64_t VrBasePeriod(int64_t base, int64_t reported) {
    if (reported <= 0) return base;
    return (base <= 0 || reported < base) ? reported : base;
}
// VR FPS cap: when the game's next real xrWaitFrame may be called. lastReturn = the steady time the last real xrWaitFrame
// returned (the runtime woke the game for a display slot then), period = VrBasePeriod, div = 2 / 3. The runtime blocks the
// call until its next wake point: a call a quarter period BEFORE the slot div periods on lands on that slot -- never the one
// before (that wake point is 3/4 of a period in the past: absorbs a late timer and a wobbly wake phase), never the one after.
// 0 = no wait (off, or nothing to anchor on yet).
inline int64_t VrWaitTarget(int64_t lastReturn, int64_t period, int div) {
    if (div < 2 || lastReturn <= 0 || period <= 0) return 0;
    return lastReturn + (int64_t)div * period - period / 4;
}

// ---- the module (frame_pace.cpp; inject.cpp / xr_hook.cpp) -----------------------------------------------------------------
// dlaa.ini reflex / fps_cap (clamped here). Any thread; the render thread applies it at its next Present (logged there).
void Configure(int reflex, int fpsCap);
// Reflex support: -1 = not checked yet (no flat Present with reflex on), 0 = not available (reason in SupportText), 1 = OK.
int         Support();
const char* SupportText();
bool        ReflexRunning();                         // NvAPI_D3D_Sleep is called each frame now (flat, NVIDIA, reflex on)
// The game's top-level Present (render thread). `vr` = an OpenXR frame loop is running (no Reflex Sleep, no flat cap then).
void BeforePresent(IDXGISwapChain* sc, bool vr);
void AfterPresent(IDXGISwapChain* sc, bool vr, uint64_t presentNo);
// Statistics since the previous call (the level-4 "CPU prof" line): frames, own-limiter sleep and Reflex Sleep ms per frame.
struct Window { uint64_t frames; double limiterMs, reflexMs; };
Window TakeWindow();

// Steady clock in ns (QueryPerformanceCounter) -- the limiter's and the VR cap's time base.
int64_t NowNs();
// A precise sleep until `deadlineNs` (NowNs time): a high-resolution waitable timer (Windows 10 1803+, else a normal one
// and a 2-ms spin) for all but the last `spinNs`, then a spin. One sleeper per thread that uses it (not thread-safe).
// Returns the ns actually waited.
class Sleeper {
public:
    ~Sleeper();
    int64_t Until(int64_t deadlineNs, int64_t spinNs);
private:
    void*   timer_ = nullptr;
    bool    tried_ = false, hiRes_ = false;
};

} // namespace FramePace
