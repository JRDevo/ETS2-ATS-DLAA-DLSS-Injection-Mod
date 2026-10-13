// FramePace -- phase 52 frame cap + NVIDIA Reflex (flat). See frame_pace.h for the design; the VR FPS cap is in xr_hook.cpp
// (WaitFrameImpl) and uses the maths and the Sleeper from here.
//
// NVAPI (Reflex) only with WITH_NVAPI (external/nvapi at build time, the same SDK and static nvapi64.lib as the phase 34 VRS);
// without it Support() says "not built in" and fps_cap runs on the own limiter.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "frame_pace.h"
#include "log.h"
#ifdef WITH_NVAPI
#include <nvapi.h>                                    // external/nvapi (NVIDIA NVAPI SDK, MIT), nvapi64.lib linked statically
#endif

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002   // Windows 10 1803+ (older SDK headers lack the name)
#endif

namespace FramePace {

namespace {
std::atomic<int>      s_reflex{0}, s_cap{0};
std::atomic<uint32_t> s_cfgSeq{1};                    // Configure bumps it; the render thread applies + logs at the next Present
std::atomic<int>      s_support{-1};
std::atomic<const char*> s_supportText{"not checked yet (no flat frame with Reflex on)"};
std::atomic<bool>     s_reflexRunning{false};
// render thread only (the game's top-level Present)
uint32_t s_appliedSeq = 0;
void*    s_dev = nullptr;                             // identity of the device the sleep mode was set on (no reference held)
bool     s_sleepModeSet = false;                      // NvAPI_D3D_SetSleepMode(low latency on) went through on s_dev
bool     s_wasVr = false;                             // the previous Present was in a VR frame loop
bool     s_vrLogged = false;
uint64_t s_markerId = 0;                              // the Reflex latency-marker frame id (strictly increasing)
bool     s_simStarted = false;                        // SIMULATION_START was sent for s_markerId
int64_t  s_deadline = 0;                              // the own limiter's previous deadline (0 = none)
Sleeper  s_sleeper;
char     s_failText[160] = "";
// the statistics window (TakeWindow; the render thread adds, the log reads)
std::atomic<uint64_t> s_winFrames{0};
std::atomic<int64_t>  s_winLimNs{0}, s_winRefNs{0};

int64_t QpcFreq() {
    static const int64_t f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return (int64_t)q.QuadPart; }();
    return f;
}

#ifdef WITH_NVAPI
const char* NvMsg(NvAPI_Status st, NvAPI_ShortString& msg) {
    msg[0] = 0;
    NvAPI_GetErrorMessage(st, msg);
    return msg;
}
// NVAPI once per process (NvAPI_Initialize is reference counted by the SDK; the phase 34 VRS probe calls it too).
bool NvInit() {
    static int s_init = -1;
    if (s_init >= 0) return s_init == 1;
    const NvAPI_Status st = NvAPI_Initialize();
    s_init = st == NVAPI_OK ? 1 : 0;
    if (!s_init) {
        NvAPI_ShortString msg;
        snprintf(s_failText, sizeof(s_failText), "no NVIDIA driver (NvAPI_Initialize status %d: %s)", (int)st, NvMsg(st, msg));
        s_support.store(0);
        s_supportText.store(s_failText);
    }
    return s_init == 1;
}
// NvAPI_D3D_SetSleepMode on `dev`. Returns the status (NVAPI_OK = 0).
int SetSleepMode(IUnknown* dev, bool lowLatency, bool boost, uint32_t minIntervalUs) {
    NV_SET_SLEEP_MODE_PARAMS p = {};
    p.version = NV_SET_SLEEP_MODE_PARAMS_VER;
    p.bLowLatencyMode = lowLatency ? 1 : 0;
    p.bLowLatencyBoost = boost ? 1 : 0;
    p.minimumIntervalUs = minIntervalUs;
    p.bUseMarkersToOptimize = 0;                      // the markers below are approximate (the hook sees no input / submit)
    return (int)NvAPI_D3D_SetSleepMode(dev, &p);
}
void Marker(IUnknown* dev, NV_LATENCY_MARKER_TYPE type, uint64_t id) {
    NV_LATENCY_MARKER_PARAMS p = {};
    p.version = NV_LATENCY_MARKER_PARAMS_VER;
    p.frameID = id;
    p.markerType = type;
    NvAPI_D3D_SetLatencyMarker(dev, &p);              // (no check: a marker that fails changes nothing)
}
#endif

// The presenting device (a reference only for the call; s_dev keeps the raw pointer as an identity).
ID3D11Device* DeviceOf(IDXGISwapChain* sc) {
    ID3D11Device* d = nullptr;
    if (!sc || FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&d)))) return nullptr;
    return d;
}

// Sleep mode back to the driver's default on the device it was set on (reflex switched off, or VR started).
void SleepModeOff(IUnknown* dev, const char* why) {
#ifdef WITH_NVAPI
    if (s_sleepModeSet && dev) {
        const int st = SetSleepMode(dev, false, false, 0);
        Log("Reflex: OFF (%s) -- NvAPI_D3D_SetSleepMode(low latency off, no interval) status %d", why, st);
    }
#else
    (void)dev; (void)why;
#endif
    s_sleepModeSet = false;
    s_reflexRunning.store(false);
}

// The render thread applies a new configuration (or the same one on a new device). Logs one line.
void Apply(ID3D11Device* dev, uint64_t presentNo) {
    const int reflex = s_reflex.load(), cap = s_cap.load();
    s_deadline = 0;                                   // the own limiter restarts from this frame
    char res[200] = "";
#ifdef WITH_NVAPI
    if (reflex && dev && NvInit() && s_support.load() != 0) {
        const uint32_t us = cap ? IntervalUs(cap) : 0u;
        const int st = SetSleepMode(dev, true, reflex == 2, us);
        if (st == 0) {
            s_sleepModeSet = true;
            s_reflexRunning.store(true);
            s_support.store(1);
            s_supportText.store("available");
            snprintf(res, sizeof(res), "Reflex %s: NvAPI_D3D_SetSleepMode status 0 (OK), NvAPI_D3D_Sleep after each Present; fps cap %s",
                     reflex == 2 ? "ON + boost" : "ON", cap ? "by the driver (minimumIntervalUs)" : "off");
            if (cap) {
                const size_t l = strlen(res);
                snprintf(res + l, sizeof(res) - l, " = %u us", us);
            }
        } else {
            NvAPI_ShortString msg;
            snprintf(s_failText, sizeof(s_failText), "the driver refused NvAPI_D3D_SetSleepMode (status %d: %s)", st,
                     NvMsg((NvAPI_Status)st, msg));
            s_support.store(0);
            s_supportText.store(s_failText);
            s_sleepModeSet = false;
            s_reflexRunning.store(false);
            LogErr("Reflex: NOT available -- %s; reflex stays off for this session%s", s_failText,
                   cap ? ", fps_cap runs on the own limiter" : "");
        }
    } else if (!reflex && s_sleepModeSet) {
        SleepModeOff(static_cast<IUnknown*>(s_dev), "reflex = 0");   // (s_dev is the current device: a new one cleared the flag)
    }
#else
    (void)dev;
    if (reflex && s_support.load() != 0) {
        s_support.store(0);
        s_supportText.store("not built in (no external/nvapi at build time)");
        LogWarn("Reflex: NOT available -- this build has no NVAPI (external/nvapi missing at build time)%s",
                cap ? "; fps_cap runs on the own limiter" : "");
    }
#endif
    if (!res[0])
        snprintf(res, sizeof(res), "Reflex %s; fps cap %s", reflex ? "requested but not available" : "off",
                 cap ? "by the own limiter (high-resolution waitable timer + 0.5 ms spin, deadline-based)" : "off");
    Log("frame pace (phase 52): reflex=%d fps_cap=%d -> %s (flat; Present #%llu)", reflex, cap, res,
        (unsigned long long)presentNo);
}
} // namespace

int64_t NowNs() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const int64_t f = QpcFreq();
    return (c.QuadPart / f) * 1000000000LL + (c.QuadPart % f) * 1000000000LL / f;
}

Sleeper::~Sleeper() {
    if (timer_) CloseHandle(timer_);
}

int64_t Sleeper::Until(int64_t deadlineNs, int64_t spinNs) {
    const int64_t t0 = NowNs();
    if (deadlineNs <= t0) return 0;
    if (!tried_) {
        tried_ = true;
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        hiRes_ = timer_ != nullptr;
        if (!timer_) timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);   // older Windows: ~1-2 ms grain
        Log("frame pace: %s waitable timer for the sleeps (thread %lu)", hiRes_ ? "high-resolution" : timer_ ? "normal (no "
            "high-resolution timer on this Windows: a 2-ms spin)" : "NO (CreateWaitableTimerExW failed: spin only)", GetCurrentThreadId());
    }
    const int64_t spin = hiRes_ ? spinNs : (spinNs > 2000000 ? spinNs : 2000000);
    const int64_t bulk = TimerPartNs(deadlineNs - t0, spin);
    if (timer_ && bulk > 0) {
        LARGE_INTEGER due;
        due.QuadPart = -(bulk / 100);                 // relative, 100-ns units
        if (SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer_, 1000);
    }
    int64_t now = NowNs();
    while (now < deadlineNs) { YieldProcessor(); now = NowNs(); }
    return now - t0;
}

void Configure(int reflex, int fpsCap) {
    s_reflex.store(ClampReflex(reflex));
    s_cap.store(ClampFpsCap(fpsCap));
    s_cfgSeq.fetch_add(1);
}
int Support() { return s_support.load(); }
const char* SupportText() { return s_supportText.load(); }
bool ReflexRunning() { return s_reflexRunning.load(); }

void BeforePresent(IDXGISwapChain* sc, bool vr) {
#ifdef WITH_NVAPI
    // PRESENT_START: right before the game's Present (the driver learns how long the OS / present queue blocks)
    if (vr || !s_sleepModeSet || !s_dev) return;
    ID3D11Device* d = DeviceOf(sc);
    if (d == s_dev) {
        if (!s_simStarted) { Marker(d, SIMULATION_START, s_markerId); s_simStarted = true; }   // (the first frame: no Sleep yet)
        Marker(d, PRESENT_START, s_markerId);
    }
    if (d) d->Release();
#else
    (void)sc; (void)vr;
#endif
}

void AfterPresent(IDXGISwapChain* sc, bool vr, uint64_t presentNo) {
    // VR: the runtime paces the game in xrWaitFrame (vr_fps_cap is xr_hook.cpp's); the desktop mirror's Present is left alone
    if (vr) {
        if (!s_wasVr) {
            s_wasVr = true;
            s_deadline = 0;
            if (s_sleepModeSet) {
                ID3D11Device* d = DeviceOf(sc);
                SleepModeOff(d == s_dev ? d : nullptr, "VR frame loop / present layer");   // (another device: s_dev may be gone -- not touched)
                if (d) d->Release();
            }
            if (!s_vrLogged && (s_reflex.load() || s_cap.load())) {
                s_vrLogged = true;
                Log("frame pace (phase 52): VR (an OpenXR frame loop runs) or a present layer (NVIDIA Smooth Motion) -- Reflex Sleep and fps_cap do "
                    "NOT run (VR: the runtime paces the game in xrWaitFrame, vr_fps_cap is the VR cap; the layer presents its own frames); "
                    "reflex=%d fps_cap=%d stay for plain flat (Present #%llu)",
                    s_reflex.load(), s_cap.load(), (unsigned long long)presentNo);
            }
        }
        return;
    }
    if (s_wasVr) { s_wasVr = false; s_appliedSeq = 0; }   // back to flat: apply again
    const int reflex = s_reflex.load(), cap = s_cap.load();
    const uint32_t seq = s_cfgSeq.load();
    ID3D11Device* dev = nullptr;
    if (reflex || s_sleepModeSet) {
        dev = DeviceOf(sc);
        if (dev && dev != s_dev && s_sleepModeSet) { s_sleepModeSet = false; s_reflexRunning.store(false); }   // a new device
    }
    if (seq != s_appliedSeq || (reflex && dev && dev != s_dev && s_support.load() != 0)) {
        s_appliedSeq = seq;
        Apply(dev, presentNo);
        s_dev = dev;
        s_simStarted = false;
    }
    int64_t limNs = 0, refNs = 0;
#ifdef WITH_NVAPI
    if (s_sleepModeSet && dev && dev == s_dev) {
        Marker(dev, PRESENT_END, s_markerId);
        const int64_t t0 = NowNs();
        NvAPI_D3D_Sleep(dev);                         // the driver sleeps here (low latency / minimumIntervalUs)
        refNs = NowNs() - t0;
        ++s_markerId;
        Marker(dev, SIMULATION_START, s_markerId);    // the next frame's simulation starts now (on this thread at least)
        s_simStarted = true;
    } else
#endif
    if (cap) {
        const int64_t now = NowNs();
        const int64_t dl = NextDeadline(s_deadline, now, IntervalNs(cap));
        s_deadline = dl;
        if (dl > now) limNs = s_sleeper.Until(dl, kSpinNs);
    } else {
        s_deadline = 0;
    }
    if (dev) dev->Release();
    s_winFrames.fetch_add(1, std::memory_order_relaxed);
    if (limNs) s_winLimNs.fetch_add(limNs, std::memory_order_relaxed);
    if (refNs) s_winRefNs.fetch_add(refNs, std::memory_order_relaxed);
}

Window TakeWindow() {
    Window w;
    w.frames = s_winFrames.exchange(0, std::memory_order_relaxed);
    const int64_t l = s_winLimNs.exchange(0, std::memory_order_relaxed), r = s_winRefNs.exchange(0, std::memory_order_relaxed);
    w.limiterMs = w.frames ? (double)l / 1e6 / (double)w.frames : 0.0;
    w.reflexMs = w.frames ? (double)r / 1e6 / (double)w.frames : 0.0;
    return w;
}

} // namespace FramePace
