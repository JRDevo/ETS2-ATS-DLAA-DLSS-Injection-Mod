// phase 54: CPU tuning (dlaa.ini cpu_pcore / cpu_priority / cpu_no_throttle, all default 0 = off; the menu's one "CPU tuning" row
// switches the three together, live). Extends phase 32 (cpu_pin.h). Each item logs once at start what it did or why it did nothing.
//
//  cpu_pcore       Intel hybrid CPUs: the game's main thread and the render (Present) thread PREFER the P-cores through
//                  SetThreadSelectedCpuSets (a soft preference, not a hard affinity). Other threads are left alone. Does nothing
//                  without two efficiency classes, with more than one processor group, or when the process affinity is already
//                  narrower than the system's: cpu_cache_pin (or a launcher) WINS then, the cpu sets are not touched.
//  cpu_priority    1 = ABOVE_NORMAL_PRIORITY_CLASS for the process. Never HIGH / REALTIME; never lowers a higher class.
//  cpu_no_throttle 1 = opt out of EcoQoS: ProcessPowerThrottling (EXECUTION_SPEED off) for the process and for the two threads above,
//                  plus IGNORE_TIMER_RESOLUTION off so timeBeginPeriod is honoured while minimized / occluded.
//
// Threading: Start() runs at DllMain time from StartInjection (kernel32 only, no loads, no locks, before any other thread of the
// mod exists). Everything else (OnPresent, SetLive) runs on the render thread at Present (the first Present, and the menu), so no
// lock is needed. The two thread handles are kept (never closed) so a live switch-off can undo the cpu sets.
#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <cstdint>
#include "log.h"
#include "cpu_tuning_pick.h"

namespace CpuTuning {

struct Cfg { int pcore = 0, priority = 0, noThrottle = 0; };

#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

struct State {
    Cfg      cfg;
    bool     started = false;
    bool     topoRead = false;
    DWORD    origPriority = 0;          // the priority class found at Start (restored when cpu_priority goes off)
    bool     prioSet = false;
    bool     procThrottleSet = false;
    CpuTuningPick::Pick pick;           // topology + verdict
    HWND     wnd = nullptr;             // the swap chain's output window (seen by OnPresent)
    bool     threadsReady = false;      // the thread handles are open
    HANDLE   hRender = nullptr, hMain = nullptr;
    DWORD    tidRender = 0, tidMain = 0;
    bool     setsApplied = false;       // SetThreadSelectedCpuSets is currently in force on the handles
    bool     thrApplied = false;        // thread power throttling opt-out is in force
};
inline State g_s;

// ---- process-level pieces (kernel32 only) ----------------------------------------------------------------------------------
inline void ApplyPriority(bool on, const char* who) {
    HANDLE p = GetCurrentProcess();
    if (on) {
        if (!g_s.prioSet) g_s.origPriority = GetPriorityClass(p);
        if (g_s.origPriority == HIGH_PRIORITY_CLASS || g_s.origPriority == REALTIME_PRIORITY_CLASS ||
            g_s.origPriority == ABOVE_NORMAL_PRIORITY_CLASS) {
            Log("cpu_priority (phase 54, %s): nothing done -- the process is already at priority class 0x%lx (never lowered, never HIGH / REALTIME)",
                who, (unsigned long)g_s.origPriority);
            return;
        }
        if (g_s.prioSet) return;
        if (SetPriorityClass(p, ABOVE_NORMAL_PRIORITY_CLASS)) {
            g_s.prioSet = true;
            Log("cpu_priority (phase 54, %s): priority class 0x%lx -> ABOVE_NORMAL (0x%lx)", who, (unsigned long)g_s.origPriority,
                (unsigned long)ABOVE_NORMAL_PRIORITY_CLASS);
        } else LogErr("cpu_priority (phase 54, %s): SetPriorityClass failed (error %lu) -- unchanged", who, GetLastError());
    } else if (g_s.prioSet) {
        if (SetPriorityClass(p, g_s.origPriority))
            Log("cpu_priority (phase 54, %s): off -- priority class back to 0x%lx", who, (unsigned long)g_s.origPriority);
        else LogErr("cpu_priority (phase 54, %s): restoring the priority class failed (error %lu)", who, GetLastError());
        g_s.prioSet = false;
    }
}

inline void ApplyProcessThrottle(bool on, const char* who) {
    PROCESS_POWER_THROTTLING_STATE st = {};
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    if (on) {
        if (g_s.procThrottleSet) return;
        st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        st.StateMask = 0;       // control bit set + state bit clear = "opt out": no EcoQoS, timer resolution honoured
        if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof(st))) {
            g_s.procThrottleSet = true;
            Log("cpu_no_throttle (phase 54, %s): process opted out of EcoQoS (EXECUTION_SPEED) and of ignoring the timer resolution", who);
            return;
        }
        const DWORD e1 = GetLastError();
        st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;       // Windows before the timer flag existed
        if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof(st))) {
            g_s.procThrottleSet = true;
            Log("cpu_no_throttle (phase 54, %s): process opted out of EcoQoS (EXECUTION_SPEED); the timer-resolution flag was refused (error %lu, old Windows)",
                who, (unsigned long)e1);
        } else LogErr("cpu_no_throttle (phase 54, %s): SetProcessInformation(ProcessPowerThrottling) failed (error %lu) -- unchanged",
                      who, GetLastError());
    } else if (g_s.procThrottleSet) {
        st.ControlMask = 0; st.StateMask = 0;                            // control bits clear = the system manages it again
        if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof(st)))
            Log("cpu_no_throttle (phase 54, %s): off -- process power throttling back to system-managed", who);
        else LogErr("cpu_no_throttle (phase 54, %s): restoring ProcessPowerThrottling failed (error %lu)", who, GetLastError());
        g_s.procThrottleSet = false;
    }
}

// The CPU topology -> g_s.pick (also logged). Once.
inline void ReadTopology() {
    if (g_s.topoRead) return;
    g_s.topoRead = true;
    ULONG len = 0;
    GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
    std::vector<CpuTuningPick::CpuSetEntry> sets;
    if (len) {
        std::vector<unsigned char> buf(len);
        if (GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf.data(), len, &len, GetCurrentProcess(), 0)) {
            for (ULONG off = 0; off < len;) {
                const SYSTEM_CPU_SET_INFORMATION* e = (const SYSTEM_CPU_SET_INFORMATION*)(buf.data() + off);
                if (e->Size == 0) break;
                if (e->Type == CpuSetInformation)
                    sets.push_back({ e->CpuSet.Id, e->CpuSet.Group, e->CpuSet.EfficiencyClass });
                off += e->Size;
            }
        } else LogErr("cpu_pcore (phase 54): GetSystemCpuSetInformation failed (error %lu)", GetLastError());
    }
    DWORD_PTR procMask = 0, sysMask = 0;
    const bool haveAff = GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask) != 0;
    const bool narrowed = haveAff && procMask != sysMask;
    g_s.pick = CpuTuningPick::PickPCores(sets, narrowed);
    const CpuTuningPick::Pick& p = g_s.pick;
    Log("cpu_pcore (phase 54): %u logical processors, %d efficiency classes, highest class %d has %d, the rest %d; process affinity 0x%llx of system 0x%llx",
        (unsigned)sets.size(), p.classCount, p.pClass, p.pCount, p.otherCount, (unsigned long long)procMask, (unsigned long long)sysMask);
}

// ---- thread-level pieces (render thread: first Present and the menu) -------------------------------------------------------
// The earliest-created thread of the process (the fallback main thread).
inline DWORD EarliestThread() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te = {}; te.dwSize = sizeof(te);
    DWORD best = 0; ULONGLONG bestT = ~0ull;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME c, x, k, u;
        if (GetThreadTimes(h, &c, &x, &k, &u)) {
            const ULONGLONG t = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
            if (t < bestT) { bestT = t; best = te.th32ThreadID; }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return best;
}

inline void ThreadPowerOptOut(HANDLE h, bool on) {
    if (!h) return;
    PROCESS_POWER_THROTTLING_STATE st = {};     // (same layout as THREAD_POWER_THROTTLING_STATE)
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = on ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    st.StateMask = 0;
    if (!SetThreadInformation(h, ThreadPowerThrottling, &st, sizeof(st)))
        LogErr("cpu_no_throttle (phase 54): SetThreadInformation(ThreadPowerThrottling) failed (error %lu)", GetLastError());
}

inline void ApplyThreads(const char* who) {
    if (!g_s.threadsReady) return;
    HANDLE hs[2] = { g_s.hRender, g_s.hMain };
    const char* names[2] = { "render", "main" };
    const bool wantSets = g_s.cfg.pcore != 0 && g_s.pick.verdict == CpuTuningPick::kOk;
    if (wantSets != g_s.setsApplied) {
        for (int i = 0; i < 2; ++i) {
            if (!hs[i]) continue;
            BOOL ok;
            if (wantSets) ok = SetThreadSelectedCpuSets(hs[i], (PULONG)g_s.pick.ids.data(), (ULONG)g_s.pick.ids.size());
            else          ok = SetThreadSelectedCpuSets(hs[i], nullptr, 0);
            if (ok) Log("cpu_pcore (phase 54, %s): %s thread (tid %lu) %s", who, names[i],
                        (unsigned long)(i == 0 ? g_s.tidRender : g_s.tidMain),
                        wantSets ? "now prefers the P-cores (soft cpu set)" : "cpu set preference removed");
            else LogErr("cpu_pcore (phase 54, %s): SetThreadSelectedCpuSets on the %s thread failed (error %lu)", who, names[i], GetLastError());
        }
        g_s.setsApplied = wantSets;
    } else if (g_s.cfg.pcore && !wantSets)
        Log("cpu_pcore (phase 54, %s): nothing done -- %s", who, CpuTuningPick::Why(g_s.pick.verdict));
    const bool wantThr = g_s.cfg.noThrottle != 0;
    if (wantThr != g_s.thrApplied) {
        for (int i = 0; i < 2; ++i) ThreadPowerOptOut(hs[i], wantThr);
        g_s.thrApplied = wantThr;
        Log("cpu_no_throttle (phase 54, %s): render + main thread %s", who,
            wantThr ? "opted out of EcoQoS" : "power throttling back to system-managed");
    }
}

// Opens the two handles (once) on the thread that presents. The main thread = the thread that created the game window (the
// swap chain's output window): it is the one pumping the window's messages, and unlike "the first thread of the process" it
// cannot be a loader stub. No window (or one of another process) -> the earliest-created thread of the process.
inline void OpenThreads() {
    if (g_s.threadsReady) return;
    g_s.tidRender = GetCurrentThreadId();
    DWORD winPid = 0, tidMain = 0;
    const char* how = "thread that created the game window";
    if (g_s.wnd) tidMain = GetWindowThreadProcessId(g_s.wnd, &winPid);
    if (!tidMain || winPid != GetCurrentProcessId()) { tidMain = EarliestThread(); how = "earliest-created thread (no usable window)"; }
    g_s.tidMain = tidMain;
    const DWORD acc = THREAD_SET_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION;
    g_s.hRender = OpenThread(acc, FALSE, g_s.tidRender);
    g_s.hMain = (tidMain && tidMain != g_s.tidRender) ? OpenThread(acc, FALSE, tidMain) : nullptr;
    g_s.threadsReady = true;
    Log("cpu_tuning (phase 54): render thread tid %lu, main thread tid %lu (%s)%s", (unsigned long)g_s.tidRender, (unsigned long)tidMain, how,
        tidMain == g_s.tidRender ? " -- the same thread" : "");
}

// True while the Present hook still has to call OnPresent (started, thread work wanted, not done yet): one flag read per Present.
inline bool NeedPresent() { return g_s.started && !g_s.threadsReady && (g_s.cfg.pcore || g_s.cfg.noThrottle); }

// Called from the hook at the first Present after something wants thread work; the thread work happens ONCE.
inline void OnPresent(HWND wnd) {
    if (!g_s.started) return;
    if (!g_s.wnd) g_s.wnd = wnd;
    if (g_s.threadsReady || (!g_s.cfg.pcore && !g_s.cfg.noThrottle)) return;
    OpenThreads();
    ApplyThreads("first Present");
}

// ---- entry points ------------------------------------------------------------------------------------------------------------
// At DllMain time from StartInjection, after cpu_cache_pin (so a narrowed affinity is visible). Kernel32 only, no locks.
inline void Start(const Cfg& c) {
    g_s.cfg = c;
    g_s.started = true;
    if (!c.pcore && !c.priority && !c.noThrottle) {
        Log("cpu tuning (phase 54): off (cpu_pcore=0 cpu_priority=0 cpu_no_throttle=0) -- nothing touched");
        return;
    }
    if (c.pcore) {
        ReadTopology();
        if (g_s.pick.verdict == CpuTuningPick::kOk)
            Log("cpu_pcore (phase 54): %d P-core logical processors (class %d) will be preferred by the main + render thread from the first Present",
                g_s.pick.pCount, g_s.pick.pClass);
        else Log("cpu_pcore (phase 54): nothing will be done -- %s", CpuTuningPick::Why(g_s.pick.verdict));
    } else Log("cpu_pcore (phase 54): off (cpu_pcore=0)");
    if (c.priority) ApplyPriority(true, "start"); else Log("cpu_priority (phase 54): off (cpu_priority=0) -- priority untouched");
    if (c.noThrottle) {
        ApplyProcessThrottle(true, "start");
        Log("cpu_no_throttle (phase 54): the render + main thread are opted out at the first Present");
    } else Log("cpu_no_throttle (phase 54): off (cpu_no_throttle=0) -- power throttling untouched");
}

// Live switch (the menu row; runs on the Present thread). Applies and undoes everything.
inline void SetLive(const Cfg& c) {
    if (!g_s.started) return;
    g_s.cfg = c;
    if (c.pcore) ReadTopology();
    ApplyPriority(c.priority != 0, "menu");
    ApplyProcessThrottle(c.noThrottle != 0, "menu");
    if (!g_s.threadsReady && (c.pcore || c.noThrottle)) OpenThreads();
    ApplyThreads("menu");
}

} // namespace CpuTuning
