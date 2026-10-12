// Phase 41: MIRRORS IN TURNS (dlaa.ini mirror_turns, default 0 = off; the tuning menu's "Mirrors in turns" row).
//
// WHY: mirror DLAA is the second-biggest GPU item of the mod after DLAA itself (README Performance: 1.3 ms own motion vectors +
// ids + 0.4 ms NGX for 4 mirror views in flat; VR about 0.4 ms per mirror with per-part motion). Every piece of that work is PER
// VIEW (draw-id replay, depth snapshot, the per-draw pairing chain, pass B, copy-in, NGX, copy-out) and the game renders each
// mirror ONCE per frame, also in VR (DEVELOPMENT.md phase 21: ~2-3 mirror views per Present for both eyes together) -- so there
// is no per-eye pair to share one DLAA between; the only cut left is time: a mirror does not need a new DLAA picture every frame.
//
// HOW: each mirror unit (inject.cpp MirUnit) has a TURN SLOT 0 / 1; on a frame fr a unit whose slot's turn it is ((fr + slot)
// even) runs its full DLAA path, the other slot RE-SHOWS ITS LAST DLAA OUTPUT (one CopyResource into the game's mirror colour
// before the game down-samples it): stale by one frame, but clean. Its view is not jittered and nothing is recorded for it.
// Why the held picture and not the raw one: the raw picture of a skipped frame is the game's aliased one -- every 2nd frame
// aliased = a 36 Hz edge flicker over the whole glass (the phase-21 log had exactly that: "the raw jittered picture = the
// flicker"); the held picture keeps every edge clean and only moving things in the glass update at half rate (the own trailer /
// cab, the largest part of a truck mirror, are static relative to the mirror camera and do not judder at all).
// A unit RUNS out of turn (forced) when holding would be wrong or stale by more than one frame:
//   - its first run, a pending reset (new / taken-over view, MirInvalidate epoch: DLAA / MV keys, mirror keys, menu row);
//   - it did not run in the previous frame (it held then, or had no view): never 2 frames old;
//   - no last output of exactly this size to show;
//   - a camera cut: the main view's DLAA history reset, or a mirror unit's own Run reset on its own (no world pair) -- every
//     unit runs on that frame and the next (MirTurnsNoteCut);
//   - the jitter-only debug key (nothing is evaluated then anyway).
// SLOTS are balanced by pixel area (largest first onto the lighter slot) whenever the set of mirrors in view changes, so each
// frame does about half the mirror work -- the PEAK per frame drops, not only the average (what a fixed VR frame budget needs).
// TEMPORAL HISTORY: every piece of a unit's history lives in the unit and only moves when the unit RUNS -- the Halton phase
// (MirUnit::phaseCtr counts jittered views; a held view is not jittered), the previous camera record / matrices and the per-draw
// record pairing (CameraMv / DrawIdMv commit inside Run), NGX's history (its feature evaluates only on a run). So the motion
// vectors of a run point back to the unit's LAST RUN (2 frames back after a hold), the jitter sequence continues per run, and
// the history is kept over that 2-frame gap (a gap of 2 is no reset while turns are on, or when the frame between was a hold).
// The WARP harness scene S20 (tests/drawid_harness.cpp) checks the motion vectors over such gaps; tests/mirror_turns_test.cpp
// checks this file's decisions.
#pragma once
#include <cstdint>

namespace MirTurns {

// why a view runs / holds (stats, log)
enum Why : int {
    kTurn = 0,        // its slot's turn
    kHold,            // not its turn: shows its last output
    kForceFirst,      // first run / reset pending / epoch changed
    kForceGap,        // it did not run in the previous frame
    kForceNoResult,   // no last output of this size to show
    kForceCut,        // a camera cut (main view reset / a mirror's own reset) this frame or the last
    kForceDebug,      // the jitter-only debug key
    kWhyN
};
inline const char* WhyName(int w) {
    static const char* const k[kWhyN] = { "turn", "held", "first / reset", "no run last frame", "no output to show", "camera cut",
                                          "debug" };
    return w >= 0 && w < kWhyN ? k[w] : "?";
}

// What the decision needs to know about one unit at its view's begin.
struct UnitState {
    uint64_t lastRunFrame = 0;     // frame of its last successful run
    uint64_t runs = 0;             // successful runs so far
    bool     resetPending = true;  // a new / re-homed / taken-over view, or the first run
    bool     epochStale = false;   // MirInvalidate since its last run
    bool     haveResult = false;   // a last DLAA output of exactly this view's size exists
    int      slot = 0;             // its turn slot (0 / 1)
};

// true = the view runs the full DLAA path on frame fr; false = it re-shows its last output. *why = the reason (Why).
// forceCut = a camera cut covers this frame; debug = the jitter-only key.
inline bool Runs(uint64_t fr, const UnitState& s, bool forceCut, bool debug, int* why) {
    int w;
    if (((fr + (uint64_t)(s.slot & 1)) & 1u) == 0)           w = kTurn;
    else if (debug)                                          w = kForceDebug;
    else if (s.runs == 0 || s.resetPending || s.epochStale)  w = kForceFirst;
    else if (s.lastRunFrame + 1 != fr)                       w = kForceGap;
    else if (!s.haveResult)                                  w = kForceNoResult;
    else if (forceCut)                                       w = kForceCut;
    else                                                     w = kHold;
    if (why) *why = w;
    return w != kHold;
}

// The history of a run on frame fr is kept over the gap since the unit's last run when the gap is 1 frame, or 2 frames with
// turns on (or the frame between was the unit's hold: turns just switched off). Longer gaps restart it (as before phase 41).
inline bool GapKeepsHistory(uint64_t fr, uint64_t lastRunFrame, bool turnsOn, bool heldLastFrame) {
    if (fr == lastRunFrame + 1) return true;
    return fr == lastRunFrame + 2 && (turnsOn || heldLastFrame);
}

// The lighter slot for one new unit of `area` px next to units already placed (sum0 / sum1 px, n0 / n1 units): the slot with
// less area; equal area -> fewer units; equal -> 0.
inline int PickSlot(uint64_t sum0, uint64_t sum1, int n0, int n1) {
    if (sum0 != sum1) return sum0 < sum1 ? 0 : 1;
    if (n0 != n1) return n0 < n1 ? 0 : 1;
    return 0;
}

// Balanced slots for n units (largest area first onto the lighter slot; equal areas keep the index order): slotOut[i] for
// area[i]. n <= 32.
inline void AssignSlots(const uint64_t* area, int n, int* slotOut) {
    if (n <= 0) return;
    if (n > 32) n = 32;
    int order[32];
    for (int i = 0; i < n; ++i) {
        int j = i;
        while (j > 0 && area[order[j - 1]] < area[i]) { order[j] = order[j - 1]; --j; }
        order[j] = i;
    }
    uint64_t sum[2] = { 0, 0 };
    int cnt[2] = { 0, 0 };
    for (int k = 0; k < n; ++k) {
        const int i = order[k];
        const int s = PickSlot(sum[0], sum[1], cnt[0], cnt[1]);
        slotOut[i] = s;
        sum[s] += area[i];
        ++cnt[s];
    }
}

}  // namespace MirTurns
