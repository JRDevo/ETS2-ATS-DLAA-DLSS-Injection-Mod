// Ofxr -- v0.10.0: OFXR Bridge (https://github.com/djules75/OFXR-Bridge) compatibility.
//
// OFXR Bridge is an OpenXR API layer (VR frame generation from optical flow). Its "D3D11 bridge" opens shared textures on the
// GAME's D3D11 device and copies / draws the game's finished eye pictures on the game's immediate context, inside
// xrReleaseSwapchainImage / xrEndFrame on the game's render thread. Those calls reach the mod's D3D11 hooks like any game call:
// IsGameCtx() cannot tell them apart (same context), so a layer copy / blit draw could be counted as a game draw, mistaken for
// the eye blit, or feed the frame-end / pass bookkeeping.
//
// How: the layer's code lives in its own module (a base name containing "ofxr", or any module loaded from
// %LOCALAPPDATA%\OFXR Bridge\). Every hook takes its return address; when that address is inside such a module the call
// passes straight through to the original with no bookkeeping. The module ranges are rescanned
// from the setup thread and then on a Present cadence (the layer is loaded late, when the game creates its OpenXR instance).
// Readers never lock: a fixed array of ranges plus an atomic count written last.
#pragma once
#include <cstdint>
#include <atomic>

namespace Ofxr {

// Ranges recorded (up to kMaxRanges) and the call counter; exposed so FromOfxr / NotePassed can stay inline.
constexpr int kMaxRanges = 8;
struct Range { uintptr_t lo; uintptr_t hi; };
extern Range                 g_ranges[kMaxRanges];
extern std::atomic<int>      g_count;     // written last by Rescan
extern std::atomic<bool>     g_on;        // ofxr_bridge != 0
extern std::atomic<uint64_t> g_passed;

// `ofxr_bridge` ini value: 1 (default) = detect + pass-through, 0 = off (no scanning, FromOfxr always false).
void Configure(int mode);
// Enumerate the process modules; record every module whose base name starts with "ofxr" (case-insensitive).
void Rescan();
// Cadence driver: call once per top-level Present / game frame (n = the mod's frame number). Rescans every 300 frames for
// the first 10 minutes, then every 3000; logs the pass-through counter every 600 frames while it changes.
void OnPresent(uint64_t n);

// True when retAddr lies inside an OFXR module. Inline-fast: off / no ranges -> false after two relaxed loads.
inline bool FromOfxr(void* retAddr) {
    if (!g_on.load(std::memory_order_relaxed)) return false;
    const int c = g_count.load(std::memory_order_acquire);
    const uintptr_t a = reinterpret_cast<uintptr_t>(retAddr);
    for (int i = 0; i < c; ++i)
        if (a >= g_ranges[i].lo && a < g_ranges[i].hi) return true;
    return false;
}
inline void NotePassed() { g_passed.fetch_add(1, std::memory_order_relaxed); }
inline uint64_t PassedCalls() { return g_passed.load(std::memory_order_relaxed); }
inline bool Detected() { return g_count.load(std::memory_order_relaxed) > 0; }

} // namespace Ofxr
