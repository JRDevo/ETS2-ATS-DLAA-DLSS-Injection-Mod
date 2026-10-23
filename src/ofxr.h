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
//
// Phase 56 (VectorXR): the same mechanism now covers a second layer, VectorXR (https://github.com/DienerTech/vectorxr, DienerTech
// LLC): Depth / Pivot / Quadviews / Turbo / Frame Generation / On-Screen Display. Its D3D11 frame-generation bridge and OSD also
// run on the GAME's immediate context (UpdateSubresource, compute dispatches), and its frame generation presents from its own
// thread, so the problem and the cure are the same as for OFXR Bridge. Each recorded range carries a `kind`; the two layers have
// separate on/off switches (ini `ofxr_bridge`, `vectorxr`) and FromOfxr() answers for either.
#pragma once
#include <cstdint>
#include <atomic>

namespace Ofxr {

// Ranges recorded (up to kMaxRanges) and the call counter; exposed so FromOfxr / NotePassed can stay inline.
constexpr int kMaxRanges = 8;
enum Kind : int { kKindOfxr = 1, kKindVectorXr = 2 };   // bit values: g_kinds is a mask of the kinds detected so far
struct Range { uintptr_t lo; uintptr_t hi; int kind; };
extern Range                 g_ranges[kMaxRanges];
extern std::atomic<int>      g_count;     // written last by Rescan
extern std::atomic<bool>     g_on;        // ofxr_bridge != 0
extern std::atomic<bool>     g_onVxr;     // phase 56: vectorxr != 0 (ini default on)
extern std::atomic<bool>     g_anyOn;     // g_on || g_onVxr -- the one flag FromOfxr reads
extern std::atomic<int>      g_kinds;     // phase 56: bitmask of Kind values recorded so far
extern std::atomic<uint64_t> g_passed;

// `ofxr_bridge` ini value: 1 (default) = detect + pass-through, 0 = off (no scanning, FromOfxr always false).
void Configure(int mode);
// Phase 56: `vectorxr` ini value: 1 (default) = detect + pass-through for the VectorXR layer, 0 = off.
void ConfigureVectorXr(int mode);
// Enumerate the process modules; record every module ModuleKind() accepts (OFXR: base name contains "ofxr" or loaded from the
// OFXR Bridge folder; VectorXR: base name contains "vectorxr" or loaded from %LOCALAPPDATA%\VectorXR\), when its kind is on.
void Rescan();
// Cadence driver: call once per top-level Present / game frame (n = the mod's frame number). Rescans every 300 frames for
// the first 10 minutes, then every 3000; logs the pass-through counter every 600 frames while it changes.
void OnPresent(uint64_t n);

// True when retAddr lies inside an OFXR Bridge or VectorXR layer module (name kept from v0.10.0). Inline-fast: both off /
// no ranges -> false after two relaxed loads.
inline bool FromOfxr(void* retAddr) {
    if (!g_anyOn.load(std::memory_order_relaxed)) return false;
    const int c = g_count.load(std::memory_order_acquire);
    const uintptr_t a = reinterpret_cast<uintptr_t>(retAddr);
    for (int i = 0; i < c; ++i)
        if (a >= g_ranges[i].lo && a < g_ranges[i].hi) return true;
    return false;
}
inline void NotePassed() { g_passed.fetch_add(1, std::memory_order_relaxed); }
inline uint64_t PassedCalls() { return g_passed.load(std::memory_order_relaxed); }
inline bool Detected() { return g_count.load(std::memory_order_relaxed) > 0; }
inline bool DetectedOfxr() { return (g_kinds.load(std::memory_order_relaxed) & kKindOfxr) != 0; }
inline bool DetectedVectorXr() { return (g_kinds.load(std::memory_order_relaxed) & kKindVectorXr) != 0; }

} // namespace Ofxr
