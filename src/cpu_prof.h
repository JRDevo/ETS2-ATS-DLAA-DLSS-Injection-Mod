// CpuProf -- phase 31: what the mod costs ON THE GAME'S THREADS (CPU), per hook and per block of mod work.
//
// Why: the user is CPU-limited in VR (72 Hz = 13.9 ms; ETS2 / ATS submit every D3D11 call from ONE render thread and draw the
// world twice). The older figures ("mod CPU ~1.7 ms per eye") were inferred from a handful of QPC timers around single blocks
// (the "cpu:" field of the FIFO stats line, the draw-id record / replay CPU, the forward-depth CPU) -- nothing covered the
// cost of the hooks themselves (thousands of game calls per frame pass through them), the Present / OpenXR work, the tuning
// menu, the log, the GPU timers. This profiler covers ALL of it with one accounting rule, so the parts add up to the frame.
//
// How (exclusive time on a per-thread category stack, one timer read per transition):
//  - every hooked D3D11 / DXGI / OpenXR entry point opens a HOOK scope (CPU_HOOK) -- but only when nothing of ours is open on
//    this thread (depth 0): a hooked call issued BY the mod (our own draws, NGX's context calls, the D3D11 runtime inside the
//    game's Present) is charged to whatever scope issued it, never counted as a game call;
//  - the pass-through of the GAME's call to the original (CPU_ORIG) opens the hook's DRIVER category: that time is the
//    game's own D3D11 / driver cost, NOT the mod's (it would be paid without the mod too);
//  - big blocks of mod work open a BLOCK scope (CpuProf::Block) at any depth: time inside them is charged to the block, not to
//    the hook that ran them (exclusive: a block inside a block charges the inner one, the outer gets only its own part);
//  - a transition reads the TSC once (__rdtsc, ~7-10 ns on Zen 4/5 with an invariant TSC), charges the elapsed ticks to the
//    category on the top of the stack, then pushes / pops. No locks on the hot path.
// phase 33 (ETS2 VR log 2026-10-11 16:02: the "CPU prof" line only ever showed tid 19132 = the game's MAIN thread, which does
// xrWaitFrame / xrBeginFrame and the desktop Present with ~8 D3D11 calls per frame; the RENDER thread -- every draw, all the
// per-draw mod work, xrEndFrame -- was never reported, because each thread wrote only its own line at its own Present):
//  - each thread's accumulators are a HEAP block (never freed: a reporter may read it after the thread is gone), made at the
//    thread's first scope and listed in a global registry (kMaxThreads slots, a lock-free append; a thread beyond the last
//    slot is simply not profiled -- the report says how many);
//  - the counters the reporter reads are single-writer relaxed atomics (a load + a store on the owner thread = the same plain
//    MOVs as before, no LOCK prefix) and CUMULATIVE: the reporter keeps the previous snapshot per slot and prints deltas
//    (a snapshot of another thread may be slightly torn across fields: fine for a statistics line);
//  - FrameMark(kind) (inject.cpp CpuProfFrameMark through g_markFn): Present marks (the presenting thread) and xrEndFrame marks
//    (the game's outermost xrEndFrame = the VR render thread) time each thread's own frames and count the frames of the window;
//    every >= 10 s whichever thread marks first writes the report: one header line + one line per busy thread (inject.cpp
//    CpuProfReport), per-frame figures divided by the VR frame count (xrEndFrame calls) in VR, the Present count in flat;
//  - the per-window maxima (worst frame) are reset by their OWNER at its first mark of a new window (g_winSeq).
//
// Off (dlaa.ini cpu_prof = 0, or the default -1 with debug = 0): every scope is ONE relaxed load + a not-taken branch.
// Header-only (C++17 inline variables): the WARP harnesses that compile scene_dlaa / draw_ids / gpu_perf link nothing new.
#pragma once
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <new>

namespace CpuProf {

// Categories. Order matters: [0, kHookCount) = hook bodies (mod work done inside the hook itself), [kBlockFirst, kModEnd) =
// mod blocks, [kDrvFirst, kDrvFirst + kHookCount) = the game's own call inside each hook (NOT mod time). IsMod(c) = c < kModEnd.
enum Cat : uint8_t {
    // ---- hooks (the mod's bookkeeping inside the hook, outside any block) ----
    kHkPresent = 0, kHkOMSetRT, kHkRSSetVp, kHkDrawIndexed, kHkDrawIdxInst, kHkDraw, kHkPSSetSamplers, kHkPSSetShader,
    kHkOMSetBlend, kHkOMSetDss, kHkMap, kHkDispatch, kHkCopyRes, kHkCopySub, kHkResolve, kHkClearRtv, kHkClearDsv,
    kHkDiscard, kHkCreatePS, kHkXrWait, kHkXrBegin, kHkXrEnd, kHkXrAcquire, kHkXrRelease, kHkXrLocate, kHkXrOther,
    kHkStateSet,           // phase 35 (did_shadow = 1): IA / VS / RS Set hooks + ClearState / ExecuteCommandList / SwapDeviceContextState
    kHookCount,
    // ---- mod blocks ----
    kBlockFirst = kHookCount,
    kBBlit = kBlockFirst,  // HandlePossibleBlit, matched part (eye map, readbacks, the DLAA unit run minus the blocks below)
    kBDlaaRun,             // SceneDlaa::Run minus NGX / MV generation (copies, RCAS, composite, Ensure)
    kBNgx,                 // NGX_D3D11_EVALUATE_DLSS_EXT itself (CPU side of the evaluate: NGX + driver command building)
    kBMvGen,               // camera MV + per-draw MV dispatch chain at the blit (CameraMv / DrawIdMv)
    kBDidRec,              // per-draw MVs: the draw's input-state record (DidOnDraw)
    kBDidReplay,           // per-draw MVs: the G-buffer leave replay (DidOnLeave) + the discard-map early replays
    kBFwd,                 // forward depth: per-draw record (FwdOnDraw) + the batch at the pass end (FwdOnPassEnd)
    kBMvCollect,           // CollectMvCandidate (medoid camera candidates)
    kBObj,                 // v0.9.0 per-object stencil ids (ObjOnDraw / ObjOnBind)
    kBSnap,                // depth snapshot + pass start / clear (OnDepthDiscard, OnSceneDepthClear)
    kBLod,                 // texture LOD bias scope (LodOnDraw / LodOnBind / PSSetSamplers twins)
    kBFrameEnd,            // the frame-end rule (FrameEndOnBind / FrameEndOnDraw: GetResource + GetDesc per bind)
    kBMirror,              // mirror views / units (MirOnBind / MirOnDraw / MirOnDiscard + their DLAA)
    kBMenu,                // tuning menu, fps box, VR late panel, OpenXR quad layers
    kBPreview,             // profile-screen / truck preview DLAA
    kBFrameWork,           // the per-frame work at Present (PresentFrameWork minus the blocks below)
    kBKeys,                // hotkey / controller polling (PollKeys: GetAsyncKeyState per bound key)
    kBGpuPerf,             // GPU timer queries (GpuPerf Begin / Next / End / Poll; on with debug = 1)
    kBLog,                 // Log() (formatting + the queue push)
    kBProf,                // this profiler's own report
    kBVrs,                 // phase 34: VRS bracket enter / leave + the per-draw full-rate rule (vrs.cpp NVAPI calls included)
    kBHmask,               // phase 40: VR hidden area mask (the eye / mesh decision + the depth-only draw at the scene clear)
    kModEnd,
    // ---- outside the mod: the game's own call inside each hook ----
    kDrvFirst = kModEnd,
    kCount = kDrvFirst + kHookCount
};
static_assert(kCount <= 255, "category ids are uint8_t");

inline const char* Name(int c) {
    static const char* const kN[kModEnd] = {
        "Present", "OMSetRT", "RSSetVp", "DrawIdx", "DrawIdxInst", "Draw", "PSSetSamp", "PSSetShader", "OMSetBlend", "OMSetDss",
        "Map", "Dispatch", "CopyRes", "CopySub", "Resolve", "ClearRtv", "ClearDsv", "Discard", "CreatePS", "xrWaitFrame",
        "xrBeginFrame", "xrEndFrame", "xrAcquire", "xrRelease", "xrLocateViews", "xrOther", "StateSet",
        "blit", "dlaa-run", "ngx-eval", "mv-gen", "did-rec", "did-replay", "fwd-depth", "mv-collect", "obj-ids", "snapshot",
        "lod", "frame-end", "mirror", "menu", "preview", "frame-work", "keys", "gpu-timers", "log", "prof", "vrs", "hmask" };
    if (c < 0 || c >= kCount) return "?";
    return c < kModEnd ? kN[c] : kN[c - kDrvFirst];
}
constexpr bool IsMod(int c) { return c < kModEnd; }

constexpr int kMaxDepth = 24;
constexpr int kMaxThreads = 64;      // phase 33: registry slots (threads with any scope; the game has ~10-20 such threads)

// phase 33: what a thread does for the game (role bits, set by the hooks; the report's thread label)
enum Role : uint32_t { kRolePresent = 1, kRoleXrWait = 2, kRoleXrEnd = 4, kRoleTurbo = 8 };
enum Mark : int { kMarkPresent = 0, kMarkXrEnd = 1 };

// A counter the owner thread writes and the reporter reads (relaxed: a plain MOV pair on x64, no LOCK prefix).
typedef std::atomic<uint64_t> Ctr;
inline void Add(Ctr& c, uint64_t d) { c.store(c.load(std::memory_order_relaxed) + d, std::memory_order_relaxed); }
inline uint64_t Get(const Ctr& c) { return c.load(std::memory_order_relaxed); }

// Per-thread accumulators (heap, value-initialised = zero, never freed).
struct Tls {
    // ---- read by the reporter (cumulative since the thread's first scope, except the two window maxima) ----
    Ctr      ticks[kCount];
    Ctr      calls[kCount];
    Ctr      transitions;        // timer reads (the profiler's own cost estimate)
    Ctr      overflows;          // pushes refused because the stack was full (never expected)
    Ctr      frames, frameTicks; // this thread's own frame marks: count + mark-to-mark wall ticks
    Ctr      frameMaxTicks;      // the longest frame of the current window (reset by the owner, see g_winSeq)
    Ctr      frameModMax;        // the most mod time in one frame of the current window
    std::atomic<uint32_t> roles; // Role bits
    uint32_t tid;                // the owner's thread id
    // ---- owner only ----
    uint64_t last;               // TSC of the last transition
    uint64_t mod;                // running sum of the mod categories' ticks (worst-frame tracking)
    uint8_t  stack[kMaxDepth];
    int      depth;
    int      overflow;           // pending pops of refused pushes
    int      markKind;           // -1 none yet; the mark kind that times this thread's frames (xrEndFrame wins over Present)
    uint64_t frameT0;            // TSC of the last frame mark (0 = none yet)
    uint64_t frameModAt;         // `mod` at the last frame mark
    uint32_t winSeen;            // g_winSeq at the last frame mark
    uint32_t onSeen;             // phase 39: g_onSeq at the last frame mark (a live switch-on drops the stale frame start)
};

inline std::atomic<bool>      g_on{false};
inline std::atomic<Tls*>      g_threads[kMaxThreads];   // zero-initialised; [0, min(g_nThreads, kMaxThreads)) used
inline std::atomic<int>       g_nThreads{0};
inline std::atomic<int>       g_unprofiled{0};          // threads that found the registry full (not profiled)
inline std::atomic<uint32_t>  g_winSeq{0};              // the reporter bumps it after each report (owners reset their maxima)
inline std::atomic<void (*)(int)> g_markFn{nullptr};    // inject.cpp CpuProfFrameMark (QPC + the report live there)
inline thread_local Tls*      t_p = nullptr;            // this thread's block (null = not made yet / registry full)
inline thread_local bool      t_noSlot = false;

inline bool On() { return g_on.load(std::memory_order_relaxed); }
// phase 39: the profiler's own cost in its last report (inject.cpp CpuProfReport: the sum of the reported threads' "profiler ~x
// ms/frame"), ms per frame; -1 = no report yet (also right after a live switch-on). The tuning menu's Logging row shows it.
inline std::atomic<double>    g_selfMs{-1.0};
inline std::atomic<uint32_t>  g_onSeq{0};               // phase 39: bumped at each live switch-on (inject.cpp CpuProfSetOn)
inline double SelfMsPerFrame() { return g_selfMs.load(std::memory_order_relaxed); }

// The calling thread's block, made + registered at its first use (cold path; never logs: Log() itself opens a scope).
inline Tls* RegisterSlow() {
    if (t_noSlot) return nullptr;
    const int i = g_nThreads.fetch_add(1, std::memory_order_relaxed);
    if (i >= kMaxThreads) { t_noSlot = true; g_unprofiled.fetch_add(1, std::memory_order_relaxed); return nullptr; }
    Tls* p = new (std::nothrow) Tls();
    if (!p) { t_noSlot = true; g_unprofiled.fetch_add(1, std::memory_order_relaxed); return nullptr; }
    p->tid = (uint32_t)__readgsdword(0x48);              // TEB ClientId.UniqueThread (= GetCurrentThreadId, x64)
    p->markKind = -1;
    t_p = p;
    g_threads[i].store(p, std::memory_order_release);
    return p;
}
inline Tls* TP() { Tls* p = t_p; return p ? p : RegisterSlow(); }
inline void SetRole(uint32_t bit) {
    if (!On()) return;
    Tls* t = TP();
    if (!t) return;
    const uint32_t r = t->roles.load(std::memory_order_relaxed);
    if (!(r & bit)) t->roles.store(r | bit, std::memory_order_relaxed);
}
// A frame boundary of the calling thread (Present / the game's xrEndFrame): inject.cpp CpuProfFrameMark.
inline void FrameMark(int kind) {
    if (!On()) return;
    if (void (*f)(int) = g_markFn.load(std::memory_order_relaxed)) f(kind);
}

// Push / pop with exclusive charging (one TSC read each).
inline void Push(Tls& t, uint8_t c) {
    const uint64_t now = __rdtsc();
    if (t.depth > 0) {
        const uint8_t top = t.stack[t.depth - 1];
        const uint64_t d = now - t.last;
        Add(t.ticks[top], d);
        if (top < kModEnd) t.mod += d;
    }
    t.last = now;
    Add(t.transitions, 1);
    if (t.depth < kMaxDepth) { t.stack[t.depth++] = c; Add(t.calls[c], 1); }
    else { ++t.overflow; Add(t.overflows, 1); }
}
inline void Pop(Tls& t) {
    const uint64_t now = __rdtsc();
    if (t.overflow > 0) { --t.overflow; return; }
    if (t.depth <= 0) return;
    const uint8_t top = t.stack[--t.depth];
    const uint64_t d = now - t.last;
    Add(t.ticks[top], d);
    if (top < kModEnd) t.mod += d;
    t.last = now;
    Add(t.transitions, 1);
}

// A hooked entry point. Active only for an OUTERMOST call on this thread (nothing of ours open): see the header.
struct Hook {
    Tls*    tp = nullptr;
    uint8_t cat = 0;
    explicit Hook(uint8_t c) {
        if (!On()) return;
        Tls* t = TP();
        if (!t || t->depth != 0) return;
        tp = t;
        cat = c;
        Push(*t, c);
    }
    ~Hook() { if (tp) Pop(*tp); }
    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;
};
// The game's own call inside an active hook: charged to that hook's driver category (outside the mod).
struct Orig {
    Tls* tp;
    explicit Orig(const Hook& h) : tp(h.tp) { if (tp) Push(*tp, (uint8_t)(kDrvFirst + h.cat)); }
    ~Orig() { if (tp) Pop(*tp); }
    Orig(const Orig&) = delete;
    Orig& operator=(const Orig&) = delete;
};
// The game's own call forwarded from a helper (no Hook object at hand: ReconcileViewports issues the game's viewports): its
// hook's driver category when `want` and a scope is open on this thread.
struct Drv {
    Tls* tp = nullptr;
    Drv(bool want, uint8_t hookCat) {
        if (!want || !On()) return;
        Tls* t = TP();
        if (!t || t->depth <= 0) return;
        tp = t;
        Push(*t, (uint8_t)(kDrvFirst + hookCat));
    }
    ~Drv() { if (tp) Pop(*tp); }
    Drv(const Drv&) = delete;
    Drv& operator=(const Drv&) = delete;
};
// A block of mod work (any depth).
struct Block {
    Tls* tp;
    explicit Block(uint8_t c) : tp(On() ? TP() : nullptr) { if (tp) Push(*tp, c); }
    ~Block() { if (tp) Pop(*tp); }
    Block(const Block&) = delete;
    Block& operator=(const Block&) = delete;
};

// Cost of one timer transition in TSC ticks (measured once by Calibrate; the report's self-cost estimate).
inline double g_transTicks = 0.0;
inline void Calibrate() {
    static Tls* dummy = new (std::nothrow) Tls();          // (not registered: never reported)
    if (!dummy) return;
    dummy->depth = 0; dummy->overflow = 0;
    const uint64_t t0 = __rdtsc();
    for (int i = 0; i < 4096; ++i) { Push(*dummy, kBProf); Pop(*dummy); }
    const uint64_t t1 = __rdtsc();
    volatile uint64_t sink = Get(dummy->ticks[kBProf]) + Get(dummy->transitions);
    (void)sink;
    g_transTicks = (double)(t1 - t0) / 8192.0;
}

} // namespace CpuProf

// Hook / pass-through helpers (one statement each; `cph` is the hook's scope object)
#define CPU_HOOK(cat)   CpuProf::Hook cph(CpuProf::cat)
#define CPU_ORIG(expr)  do { CpuProf::Orig cpo_(cph); expr; } while (0)
