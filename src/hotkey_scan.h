// HotkeyScan (v0.12.1 phase 50, GitHub issue #16) -- the fresh-press detector of the tuning menu's Hotkeys page bind mode
// (inject.cpp MenuHkBindTick). Pure logic, no Windows calls: the caller reads the keys (GetAsyncKeyState) and the controller
// state (KeySwallow::ControllerState via PollKeys) once per Present and hands them in; tests/hotkey_scan_test.cpp drives it.
//
// The rule: an input binds only on a FRESH DOWN-EDGE after bind mode started. Everything down at Start is the "previous"
// state, so an input already held then (the menu Right that started it, an engaged H-shifter gear = a button held for
// minutes) needs a release AND a new press. Keys are read reliably (GetAsyncKeyState): one previous state.
// The controller needs more care, because a controller button can read UP without being released:
//  - a device slot that goes stale (the game did not read it for 0.5 s) or is cleared (a failed read, Unacquire, a new data
//    format) reads all up; when it is read again its held buttons read down. `fresh` (one bit per device slot whose state is
//    known) tells: while the set of known slots changes, no controller edge counts and every input down in any of the last
//    states stays "held" (adopted); only a Present with the same known set as the one before compares.
//  - a single torn read (the input thread clearing a slot while the render thread reads it) can show a held button up for one
//    Present: a controller edge needs the input UP in the TWO previous compared Presents (a real release-and-press of a gear
//    or a button takes far longer than two frames; a tap shorter than a frame still counts: the tap latch shows it down once).
#pragma once
#include <cstdint>

namespace HotkeyScan {

struct State {
    uint32_t key[8]  = {};                     // keys down at the previous Present (bit vk)
    uint32_t btn1[4] = {}, btn2[4] = {};       // controller buttons down at the previous / the one before (bit = button - 1)
    unsigned pov1 = 0, pov2 = 0;               // D-pad directions the same way (1 up, 2 right, 4 down, 8 left)
    uint32_t fresh = 0;                        // the known controller slots at the previous Present
};
struct Edges {
    uint32_t key[8] = {};                      // fresh key presses this Present
    uint32_t btn[4] = {};                      // fresh controller button presses
    unsigned pov = 0;                          // fresh D-pad directions
    bool     joyCompared = false;              // false = the known-slot set changed: no controller edge could count
};

// Bind mode starts: everything down now is "held".
inline void Start(State& s, const uint32_t key[8], const uint32_t btn[4], unsigned pov, uint32_t fresh) {
    for (int w = 0; w < 8; ++w) s.key[w] = key[w];
    for (int w = 0; w < 4; ++w) s.btn1[w] = s.btn2[w] = btn[w];
    s.pov1 = s.pov2 = pov;
    s.fresh = fresh;
}

// One Present: the fresh presses since the previous one; the state moves on.
inline Edges Step(State& s, const uint32_t key[8], const uint32_t btn[4], unsigned pov, uint32_t fresh) {
    Edges e;
    for (int w = 0; w < 8; ++w) { e.key[w] = key[w] & ~s.key[w]; s.key[w] = key[w]; }
    if (fresh == s.fresh) {
        e.joyCompared = true;
        for (int w = 0; w < 4; ++w) {
            e.btn[w] = btn[w] & ~s.btn1[w] & ~s.btn2[w];
            s.btn2[w] = s.btn1[w];
            s.btn1[w] = btn[w];
        }
        e.pov = pov & ~s.pov1 & ~s.pov2;
        s.pov2 = s.pov1;
        s.pov1 = pov;
    } else {                                   // a slot went stale / came back: whatever was down anywhere stays held
        for (int w = 0; w < 4; ++w) s.btn1[w] = s.btn2[w] = s.btn1[w] | s.btn2[w] | btn[w];
        s.pov1 = s.pov2 = s.pov1 | s.pov2 | pov;
    }
    s.fresh = fresh;
    return e;
}

}  // namespace HotkeyScan
