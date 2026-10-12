// Ofxr -- see ofxr.h. v0.10.0 OFXR Bridge pass-through: module-range detection by name, no D3D, no WITH_DLAA dependency.
#include "ofxr.h"
#include <windows.h>
#include <psapi.h>
#include <cwchar>
#include <cwctype>
#include <cstdio>
#include <mutex>
#include "log.h"

namespace Ofxr {

Range                 g_ranges[kMaxRanges] = {};
std::atomic<int>      g_count{0};
std::atomic<bool>     g_on{true};
std::atomic<uint64_t> g_passed{0};

namespace {
std::mutex            s_scanLock;                 // writers only (setup thread / Present thread); readers never lock
bool                  s_firstScanDone = false;
ULONGLONG             s_startTick = 0;            // first OnPresent
uint64_t              s_lastScanN = 0;
uint64_t              s_lastStatN = 0;
uint64_t              s_lastStatPassed = 0;

wchar_t s_bridgeDir[MAX_PATH] = L"";          // "%LOCALAPPDATA%\OFXR Bridge\" lower-cased; "" when LOCALAPPDATA is unset

// The layer DLL's name is not documented (the tray copies "the OpenXR layer" into %LOCALAPPDATA%\OFXR Bridge\), so a module
// counts when its base name CONTAINS "ofxr" (case-insensitive) or when it was loaded from that folder.
bool IsOfxrModule(const wchar_t* base, const wchar_t* fullPath) {
    wchar_t lo[MAX_PATH];
    int i = 0;
    for (; base[i] && i < MAX_PATH - 1; ++i) lo[i] = (wchar_t)towlower(base[i]);
    lo[i] = 0;
    if (wcsstr(lo, L"ofxr")) return true;
    if (!s_bridgeDir[0] || !fullPath) return false;
    const size_t dl = wcslen(s_bridgeDir);
    for (size_t k = 0; k < dl; ++k)
        if (!fullPath[k] || (wchar_t)towlower(fullPath[k]) != s_bridgeDir[k]) return false;
    return true;
}
} // namespace

void Configure(int mode) {
    g_on.store(mode != 0, std::memory_order_relaxed);
}

void Rescan() {
    if (!g_on.load(std::memory_order_relaxed)) return;
    std::unique_lock<std::mutex> lk(s_scanLock, std::try_to_lock);
    if (!lk.owns_lock()) return;                  // another thread is scanning right now

    if (!s_firstScanDone) {
        s_firstScanDone = true;
        wchar_t base[MAX_PATH];
        const DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
        if (len > 0 && len < MAX_PATH - 40) {
            wchar_t path[MAX_PATH];
            swprintf_s(s_bridgeDir, L"%ls\\OFXR Bridge\\", base);
            for (wchar_t* c = s_bridgeDir; *c; ++c) *c = (wchar_t)towlower(*c);
            swprintf_s(path, L"%ls\\OFXR Bridge\\tray.ini", base);
            if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
                Log("OFXR Bridge: tray.ini found at %ls (the bridge is installed on this PC; armed = the tray app is running)", path);
            else
                Log("OFXR Bridge: not installed (no %ls)", path);
        } else {
            Log("OFXR Bridge: not installed (LOCALAPPDATA not set)");
        }
    }

    LARGE_INTEGER t0, t1, fq;                     // phase 31: the cost of one rescan (first 3 logged)
    QueryPerformanceCounter(&t0);
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    const DWORD count = (needed < sizeof(mods) ? needed : (DWORD)sizeof(mods)) / sizeof(HMODULE);
    int n = g_count.load(std::memory_order_relaxed);
    for (DWORD i = 0; i < count && n < kMaxRanges; ++i) {
        wchar_t name[MAX_PATH], full[MAX_PATH];
        if (!GetModuleBaseNameW(GetCurrentProcess(), mods[i], name, MAX_PATH)) continue;
        if (!GetModuleFileNameExW(GetCurrentProcess(), mods[i], full, MAX_PATH)) full[0] = 0;
        if (!IsOfxrModule(name, full)) continue;
        MODULEINFO mi{};
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi)) || !mi.SizeOfImage) continue;
        const uintptr_t lo = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
        bool known = false;
        for (int k = 0; k < n; ++k) if (g_ranges[k].lo == lo) { known = true; break; }
        if (known) continue;
        g_ranges[n].lo = lo;
        g_ranges[n].hi = lo + mi.SizeOfImage;
        ++n;
        g_count.store(n, std::memory_order_release);   // count written last: readers never see a half-filled range
        Log("OFXR Bridge: layer module %ls loaded @ %p size %u KB -- its D3D11 calls on the game context pass through the "
            "mod's hooks", name, mi.lpBaseOfDll, (unsigned)(mi.SizeOfImage / 1024));
    }
    static int s_timedLogs = 0;
    if (s_timedLogs < 3) {
        ++s_timedLogs;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&fq);
        Log("OFXR Bridge: module rescan took %.3f ms for %lu modules (tid=%lu; phase 31: off the render thread after the first)",
            fq.QuadPart > 0 ? (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart : 0.0, count,
            GetCurrentThreadId());
    }
}

void OnPresent(uint64_t n) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    const ULONGLONG now = GetTickCount64();
    if (!s_startTick) { s_startTick = now; s_lastScanN = n; s_lastStatN = n; }
    const uint64_t interval = (now - s_startTick < 10ull * 60ull * 1000ull) ? 300 : 3000;
    if (n - s_lastScanN >= interval) {
        s_lastScanN = n;
        // phase 31: the rescan runs on a thread-pool thread, not on the render thread. EnumProcessModules + a base name + a full
        // path per loaded module (GetModuleFileNameExW reads the loader list through ReadProcessMemory) is a few hundred
        // microseconds to milliseconds with the ~150-250 modules of a VR session -- a stall of one frame every 300 frames (4 s
        // at 72 fps) for the first 10 minutes, then every 3000. Rescan already serialises itself (s_scanLock try-lock) and
        // publishes the ranges lock-free (count written last), so it may run on any thread; a module found a few ms later
        // than before changes nothing (the cadence was already 300 frames). Falls back to the old inline call when the pool
        // refuses the work item.
        if (!TrySubmitThreadpoolCallback([](PTP_CALLBACK_INSTANCE, void*) { Rescan(); }, nullptr, nullptr)) Rescan();
    }
    if (n - s_lastStatN >= 600) {
        s_lastStatN = n;
        const uint64_t p = g_passed.load(std::memory_order_relaxed);
        if (p > 0 && p != s_lastStatPassed) {
            s_lastStatPassed = p;
            LogDbg("OFXR Bridge: %llu calls passed through so far", (unsigned long long)p);
        }
    }
}

} // namespace Ofxr
