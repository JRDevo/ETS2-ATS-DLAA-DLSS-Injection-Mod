// DlaaStage -- v0.10.0 phase 7: WHERE the main flat scene's DLAA runs (pre-tonemap HDR unit vs post-tonemap blit unit),
// decided ONCE per pass with hysteresis. Pure logic (no D3D): inject.cpp feeds one Decide() per pass and reports the outcome
// of the HDR run; the WARP harness (tests\drawid_harness.cpp, scene 9) drives it with scripted pass sequences.
//
// WHY (in-game log captures/dlaa_inject_ets2_v0100p6c_flat.log): phase 4 picked the stage per frame; 18 flips to post and 19
// back in 7 minutes, some windows alternating inside a second, and every flip rebuilt eye 0's NGX unit (history reset = a
// whole-screen blur / shimmer pulse). Now:
//  * MODE reasons (dlaa_pre_tonemap 0 / Alt+F7 off, DLAA off, Ctrl+F6, DLSS upscaling, VR, HDR output, ...) switch to the
//    post-tonemap stage AT ONCE (the user asked for it / the debug colours must be exact). The blit unit's history is then
//    stale: the caller resets that unit once (SceneDlaa::KindIdleRuns).
//  * PER-PASS conditions (no forward colour / no depth snapshot / the HDR run failed) never switch the preferred stage for a
//    single pass: while the preferred stage is pre-tonemap such a pass runs the blit unit as a FALLBACK (the HDR unit stays
//    alive, its history kept); only kHyst consecutive passes of the other condition switch the preferred stage (logged once).
//  * pre -> post after kHyst fallback passes: the blit unit ran in every one of them, so its history is continuous (no reset).
//  * post -> pre after kHyst pre-possible passes: the last kWarm of them WARM the HDR unit at the blit (SceneDlaa::Warm, same
//    motion vectors / depth / jitter, result discarded), so it switches with a continuous history (no reset). If the HDR unit
//    is not warm by then (its build waits for an NGX create budget), the switch waits up to kHyst more passes, then goes ahead
//    (the caller resets a stale unit).
// Both units stay alive (SceneDlaa keeps two colour sets: SceneDlaa::Ensure / Warm); only a size change destroys them.
#pragma once
#include <cstdint>

struct DlaaStageIn {
    const char* ineligible = nullptr;   // a MODE reason (non-null = post-tonemap at once)
    bool        possible = false;       // this pass can run the HDR unit before the tonemap (forward colour + depth snapshot)
    bool        hdrWarm = false;        // the HDR unit is built and evaluated within the last pass (SceneDlaa::KindIdleRuns <= 1)
};

struct DlaaStageOut {
    bool runPre = false;       // run the HDR unit now (report the result with OnPreRun)
    bool fallback = false;     // the preferred stage is pre-tonemap but this pass runs the blit unit
    bool warmHdr = false;      // the blit should warm the HDR unit after its own run (a post -> pre switch is coming)
    int  switched = 0;         // +1 switched to pre-tonemap, -1 switched to post-tonemap, 0 no switch
    bool modeSwitch = false;   // the switch was a MODE change (immediate), not the hysteresis
    bool forced = false;       // post -> pre went ahead without a warm HDR unit (after 2 x kHyst)
};

class DlaaStage {
public:
    static constexpr int kHyst = 60;    // consecutive passes of the other condition before the preferred stage switches
    static constexpr int kWarm = 8;     // the last passes before a post -> pre switch warm the HDR unit

    // 1 pre-tonemap, 0 post-tonemap, -1 nothing decided yet (counts as post-tonemap)
    int Preferred() const { return m_pref; }
    int Streak() const { return m_streak; }

    DlaaStageOut Decide(const DlaaStageIn& in) {
        DlaaStageOut o;
        ++m_passes;
        if (in.ineligible) {
            m_streak = 0;
            if (m_pref == 1) { o.switched = -1; o.modeSwitch = true; ++m_switches; ++m_totalSwitches; }
            m_pref = 0;
            return o;
        }
        if (m_pref == 1) {
            if (in.possible) { o.runPre = true; return o; }   // the streak is updated by OnPreRun
            o.fallback = true;
            ++m_fallbacks;
            Miss(o);
            return o;
        }
        // preferred post-tonemap (or nothing decided yet): count passes that could run before the tonemap
        if (m_pref < 0) m_pref = 0;
        if (!in.possible) { m_streak = 0; return o; }
        ++m_streak;
        if (m_streak >= kHyst && (in.hdrWarm || m_streak >= 2 * kHyst)) {
            o.forced = !in.hdrWarm;
            m_pref = 1; m_streak = 0;
            o.switched = 1; o.runPre = true;
            ++m_switches; ++m_autoSwitches; ++m_totalSwitches;
            return o;
        }
        if (m_streak >= kHyst - kWarm) o.warmHdr = true;   // the kWarm passes before the switch
        return o;
    }
    // The HDR run of a runPre pass: ok = it evaluated. A failed / deferred run is a fallback pass (the blit unit runs) and
    // counts towards the switch to post-tonemap like a pass without a forward colour. Returns the switch (-1) or 0.
    int OnPreRun(bool ok) {
        if (ok) { m_streak = 0; return 0; }
        ++m_fallbacks;
        DlaaStageOut o;
        Miss(o);
        return o.switched;
    }
    // Alt+F7 / a context reset: start over (the next eligible stretch decides again, post-tonemap first).
    void Restart() { m_pref = -1; m_streak = 0; }

    // stats (window: the caller reads and clears them with the FIFO stats line)
    uint64_t switches() const { return m_switches; }
    uint64_t autoSwitches() const { return m_autoSwitches; }
    uint64_t fallbacks() const { return m_fallbacks; }
    uint64_t passes() const { return m_passes; }
    void ClearWindow() { m_switches = m_autoSwitches = m_fallbacks = m_passes = 0; }
    uint64_t totalSwitches() const { return m_totalSwitches; }

private:
    void Miss(DlaaStageOut& o) {
        if (m_pref != 1) return;
        if (++m_streak >= kHyst) {
            m_pref = 0; m_streak = 0;
            o.switched = -1;
            ++m_switches; ++m_autoSwitches; ++m_totalSwitches;
        }
    }
    int      m_pref = -1;
    int      m_streak = 0;
    uint64_t m_switches = 0, m_autoSwitches = 0, m_fallbacks = 0, m_passes = 0;
    uint64_t m_totalSwitches = 0;
};
