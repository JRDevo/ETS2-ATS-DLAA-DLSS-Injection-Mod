// v0.12.1 phase 50 unit test (GitHub issue #16): the tuning menu's Hotkeys page.
//  A. src/hotkey_scan.h -- the bind mode's fresh-press detector: an input held when bind mode starts (an engaged H-shifter
//     gear) never binds, also not when its controller slot goes stale / is cleared / comes back or one read is torn; a real
//     release + press does, once.
//  B. src/dinput_wrap.cpp (compiled into this TU) -- what the open menu hides from the game (KeySwallow::SetMenuHide): only
//     the menu inputs (keyboard DIK bytes / buffered entries, controller buttons / D-pad), everything in bind mode, nothing
//     when closed; ControllerState's known-slot mask.
// No D3D, no game. Build: tests\build_hotkey_scan_test.bat. Exit code 0 = every check passed.
#include "../src/hotkey_scan.h"
#include "../src/dinput_wrap.cpp"                  // the anonymous-namespace filters (KbTap, JoyTap, HidesVk ...) are tested directly
#include <cstdio>

// dinput_wrap.cpp's links into inject.cpp
bool ControllerNameAllowed(const char*) { return true; }
const char* ControllerNameFilter() { return ""; }

static int g_checks = 0, g_fails = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fails; printf("   FAIL (line %d): ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- A. HotkeyScan --------------------------------------------------------------------------------------------------------
struct In {                                        // one Present's input
    uint32_t key[8] = {}, btn[4] = {};
    unsigned pov = 0;
    uint32_t fresh = 1;                            // slot 0 known
    In& K(int vk, bool d = true) { if (d) key[vk >> 5] |= 1u << (vk & 31); else key[vk >> 5] &= ~(1u << (vk & 31)); return *this; }
    In& B(int b1, bool d = true) { const int i = b1 - 1; if (d) btn[i >> 5] |= 1u << (i & 31); else btn[i >> 5] &= ~(1u << (i & 31)); return *this; }
};
static int Count(const HotkeyScan::Edges& e) {
    int c = 0;
    for (uint32_t w : e.key) for (; w; w &= w - 1) ++c;
    for (uint32_t w : e.btn) for (; w; w &= w - 1) ++c;
    for (unsigned p = e.pov; p; p &= p - 1) ++c;
    return c;
}
static bool HasBtn(const HotkeyScan::Edges& e, int b1) { const int i = b1 - 1; return (e.btn[i >> 5] >> (i & 31)) & 1u; }
static bool HasKey(const HotkeyScan::Edges& e, int vk) { return (e.key[vk >> 5] >> (vk & 31)) & 1u; }
static HotkeyScan::Edges Step(HotkeyScan::State& s, const In& in) { return HotkeyScan::Step(s, in.key, in.btn, in.pov, in.fresh); }

static void TestScan() {
    printf("A. HotkeyScan (src/hotkey_scan.h)\n");
    const int kGear = 5, kMenuRight = VK_RIGHT;
    // A1: the menu Right that starts bind mode is held: no edge while held, none at the release, one at the next press
    {
        HotkeyScan::State s;
        In in; in.K(kMenuRight);
        HotkeyScan::Start(s, in.key, in.btn, in.pov, in.fresh);
        int n = 0;
        for (int f = 0; f < 30; ++f) n += Count(Step(s, in));
        CHECK(n == 0, "A1 held Right gave %d edges", n);
        in.K(kMenuRight, false);
        CHECK(Count(Step(s, in)) == 0, "A1 release gave an edge");
        in.K(kMenuRight);
        const HotkeyScan::Edges e = Step(s, in);
        CHECK(Count(e) == 1 && HasKey(e, kMenuRight), "A1 the new press of Right must be the one edge");
        CHECK(Count(Step(s, in)) == 0, "A1 held after the press: no second edge");
    }
    // A2 (issue #16): a shifter gear held at the start never binds -- held for minutes, its slot stale for a while (reads all
    // up), cleared and read again, a torn read
    {
        HotkeyScan::State s;
        In in; in.B(kGear);
        HotkeyScan::Start(s, in.key, in.btn, in.pov, in.fresh);
        int n = 0;
        for (int f = 0; f < 600; ++f) n += Count(Step(s, in));                 // 10 s held at 60 fps
        CHECK(n == 0, "A2 held gear gave %d edges", n);
        In stale; stale.fresh = 0;                                               // the slot goes stale: all up, unknown
        for (int f = 0; f < 40; ++f) n += Count(Step(s, stale));
        CHECK(n == 0, "A2 stale slot gave %d edges", n);
        for (int f = 0; f < 40; ++f) n += Count(Step(s, in));                  // read again: the gear reads down
        CHECK(n == 0, "A2 slot back with the gear held gave %d edges (must be adopted as held)", n);
        In torn; torn.fresh = 1;                                                 // one torn read: known slot, gear up
        n += Count(Step(s, torn));
        n += Count(Step(s, in));
        CHECK(n == 0, "A2 a single torn read gave %d edges", n);
        In gone; gone.fresh = 0;                                                 // a stale flicker of one Present
        n += Count(Step(s, gone));
        n += Count(Step(s, in));
        for (int f = 0; f < 10; ++f) n += Count(Step(s, in));
        CHECK(n == 0, "A2 a one-Present stale flicker gave %d edges", n);
        // a tap of another button while the gear stays held: that button is the one edge
        In tap = in; tap.B(3);
        const HotkeyScan::Edges e = Step(s, tap);
        CHECK(Count(e) == 1 && HasBtn(e, 3), "A2 a fresh tap of button 3 with the gear held must be the one edge");
        n = 0;
        for (int f = 0; f < 10; ++f) n += Count(Step(s, in));
        CHECK(n == 0, "A2 after the tap: %d edges", n);
        // the gear really released (out of gear) and engaged again: now it is a fresh press, once
        In neutral; neutral.fresh = 1;
        for (int f = 0; f < 20; ++f) n += Count(Step(s, neutral));
        CHECK(n == 0, "A2 neutral gave %d edges", n);
        const HotkeyScan::Edges g = Step(s, in);
        CHECK(Count(g) == 1 && HasBtn(g, kGear), "A2 the gear engaged again after a real release must be one edge");
    }
    // A3: a second controller appears (a slot becomes known) with a button held: adopted, no edge; a later press on it binds
    {
        HotkeyScan::State s;
        In in;
        HotkeyScan::Start(s, in.key, in.btn, in.pov, in.fresh);
        In two; two.fresh = 3; two.B(40);
        int n = 0;
        for (int f = 0; f < 10; ++f) n += Count(Step(s, two));
        CHECK(n == 0, "A3 a new slot's held button gave %d edges", n);
        In rel; rel.fresh = 3;
        for (int f = 0; f < 3; ++f) n += Count(Step(s, rel));
        const HotkeyScan::Edges e = Step(s, two);
        CHECK(n == 0 && Count(e) == 1 && HasBtn(e, 40), "A3 button 40 pressed after its release must be the one edge");
    }
    // A4: D-pad -- held at the start ignored; a new direction is an edge; a diagonal from centre = two edges (the caller
    // ignores two at once)
    {
        HotkeyScan::State s;
        In in; in.pov = 1;                                                       // up held
        HotkeyScan::Start(s, in.key, in.btn, in.pov, in.fresh);
        int n = 0;
        for (int f = 0; f < 10; ++f) n += Count(Step(s, in));
        CHECK(n == 0, "A4 held D-pad up gave %d edges", n);
        In c; for (int f = 0; f < 3; ++f) n += Count(Step(s, c));
        In right; right.pov = 2;
        const HotkeyScan::Edges e = Step(s, right);
        CHECK(n == 0 && Count(e) == 1 && e.pov == 2, "A4 D-pad right must be the one edge");
        for (int f = 0; f < 3; ++f) Step(s, c);
        In diag; diag.pov = 1 | 2;
        CHECK(Count(Step(s, diag)) == 2, "A4 a diagonal from centre = two edges");
    }
    // A5: keys are read reliably: Escape / Backspace / F5 presses are single edges; Ctrl itself is not scanned (the caller's list)
    {
        HotkeyScan::State s;
        In in;
        HotkeyScan::Start(s, in.key, in.btn, in.pov, in.fresh);
        In esc; esc.K(VK_ESCAPE);
        const HotkeyScan::Edges e = Step(s, esc);
        CHECK(Count(e) == 1 && HasKey(e, VK_ESCAPE), "A5 Escape edge");
        In both; both.K(VK_ESCAPE).K(VK_F5);
        const HotkeyScan::Edges f5 = Step(s, both);
        CHECK(Count(f5) == 1 && HasKey(f5, VK_F5), "A5 F5 edge with Escape still held");
    }
    // A6: a press that lands exactly when the known-slot set changes is not taken (held from then on); the next press is
    {
        HotkeyScan::State s;
        In in;
        HotkeyScan::Start(s, in.key, in.btn, in.pov, in.fresh);
        In both; both.fresh = 3; both.B(7);
        CHECK(Count(Step(s, both)) == 0, "A6 a press at a slot-set change must not count");
        CHECK(!Step(s, both).btn[0], "A6 held after it: no edge");
        In up; up.fresh = 3;
        Step(s, up); Step(s, up);
        const HotkeyScan::Edges e = Step(s, both);
        CHECK(Count(e) == 1 && HasBtn(e, 7), "A6 the next press counts");
    }
}

// ---- B. KeySwallow filters ------------------------------------------------------------------------------------------------
static void TestHide() {
    printf("B. KeySwallow::SetMenuHide + the device / window filters (src/dinput_wrap.cpp)\n");
    using namespace KeySwallow;
    const int vks[6]  = { VK_DELETE, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 0 };
    const int joys[6] = { 10, 0, 0, kJoyPovLeft, 0, 0 };            // menu open = Wheel10, menu left = WheelLeft
    KbTap kb;
    // B1 keyboard state: only the menu keys' DIK bytes are cleared
    SetMenuHide(kHideMenuInputs, vks, joys, 5);
    BYTE st[256];
    memset(st, 0x80, sizeof(st));
    kb.OnState(S_OK, 256, st);
    CHECK(st[0xD3] == 0 && st[0xC8] == 0 && st[0xD0] == 0 && st[0xCB] == 0 && st[0xCD] == 0, "B1 a menu key (Delete / arrows) reached the game");
    CHECK(st[0x11] == 0x80 && st[0x01] == 0x80 && st[0x1E] == 0x80 && st[0x48] == 0x80, "B1 W / Escape / A / Numpad8 must pass");
    int cleared = 0;
    for (int i = 0; i < 256; ++i) cleared += st[i] == 0;
    CHECK(cleared == 5, "B1 %d key bytes cleared (5 expected)", cleared);
    CHECK(HidesVk(HideMode(), VK_UP) && !HidesVk(HideMode(), 'W') && !HidesVk(HideMode(), VK_ESCAPE), "B1 window-message VK test");
    // B2 buffered keyboard: the DOWN entries of hidden keys are dropped, releases and other keys pass
    {
        DIDEVICEOBJECTDATA d[4] = {};
        d[0].dwOfs = 0xC8; d[0].dwData = 0x80;      // Up down: dropped
        d[1].dwOfs = 0xC8; d[1].dwData = 0x00;      // Up up: passes
        d[2].dwOfs = 0x11; d[2].dwData = 0x80;      // W down: passes
        d[3].dwOfs = 0xD3; d[3].dwData = 0x80;      // Delete down: dropped
        DWORD n = 4;
        kb.OnData(S_OK, sizeof(DIDEVICEOBJECTDATA), d, &n);
        CHECK(n == 2 && d[0].dwOfs == 0xC8 && d[0].dwData == 0 && d[1].dwOfs == 0x11, "B2 buffered filter kept %lu entries", (unsigned long)n);
    }
    // B3 bind mode hides every key; closed hides none
    SetMenuHide(kHideAll, nullptr, nullptr, 0);
    memset(st, 0x80, sizeof(st));
    kb.OnState(S_OK, 256, st);
    cleared = 0;
    for (int i = 0; i < 256; ++i) cleared += st[i] == 0;
    CHECK(cleared == 256, "B3 bind mode: %d of 256 key bytes cleared", cleared);
    CHECK(HidesVk(HideMode(), 'W'), "B3 bind mode: W hidden from window messages");
    SetMenuHide(kHideNone, vks, joys, 5);
    memset(st, 0x80, sizeof(st));
    kb.OnState(S_OK, 256, st);
    cleared = 0;
    for (int i = 0; i < 256; ++i) cleared += st[i] == 0;
    CHECK(cleared == 0 && !HidesVk(HideMode(), VK_UP), "B3 closed: %d key bytes cleared", cleared);
    // B4 controller state: the menu's button / D-pad direction hidden, the shifter gear and the axes pass
    {
        JoyTap jt("test wheel");
        CHECK(jt.slot != nullptr, "B4 no joystick slot");
        DIJOYSTATE2 js;
        memset(&js, 0, sizeof(js));
        js.lX = 12345;
        js.rgbButtons[4] = 0x80;                     // Wheel5 = the shifter gear
        js.rgbButtons[9] = 0x80;                     // Wheel10 = the menu open button
        js.rgdwPOV[0] = 27000;                       // D-pad left = the menu left
        SetMenuHide(kHideMenuInputs, vks, joys, 5);
        jt.OnState(S_OK, sizeof(js), &js);
        CHECK(js.rgbButtons[4] == 0x80, "B4 the shifter gear must reach the game while the menu is open");
        CHECK(js.rgbButtons[9] == 0, "B4 the menu's own button reached the game");
        CHECK(js.rgdwPOV[0] == 0xFFFFFFFFu, "B4 the menu's D-pad direction reached the game");
        CHECK(js.lX == 12345, "B4 an axis was touched");
        js.rgdwPOV[0] = 0;                           // D-pad up: not a menu input here
        jt.OnState(S_OK, sizeof(js), &js);
        CHECK(js.rgdwPOV[0] == 0, "B4 D-pad up must pass");
        // bind mode: every button and the D-pad hidden, axes pass
        SetMenuHide(kHideAll, nullptr, nullptr, 0);
        js.rgbButtons[4] = 0x80; js.rgdwPOV[0] = 9000;
        jt.OnState(S_OK, sizeof(js), &js);
        CHECK(js.rgbButtons[4] == 0 && js.rgdwPOV[0] == 0xFFFFFFFFu && js.lX == 12345, "B4 bind mode must hide every button + the D-pad only");
        // buffered: the gear's down entry passes in menu-inputs mode, the menu button's is dropped
        SetMenuHide(kHideMenuInputs, vks, joys, 5);
        DIDEVICEOBJECTDATA d[3] = {};
        d[0].dwOfs = DIJOFS_BUTTON(4); d[0].dwData = 0x80;
        d[1].dwOfs = DIJOFS_BUTTON(9); d[1].dwData = 0x80;
        d[2].dwOfs = DIJOFS_BUTTON(9); d[2].dwData = 0x00;
        DWORD n = 3;
        jt.OnData(S_OK, sizeof(DIDEVICEOBJECTDATA), d, &n);
        CHECK(n == 2 && d[0].dwOfs == DIJOFS_BUTTON(4) && d[1].dwOfs == DIJOFS_BUTTON(9) && d[1].dwData == 0,
              "B4 buffered: %lu entries kept (gear down + menu button up expected)", (unsigned long)n);
        // B5 ControllerState: the slot is known after a read; a failed read (cleared) makes it unknown
        uint32_t b[4]; unsigned pov = 0; uint32_t fresh = 0;
        ControllerState(b, &pov, &fresh);
        const uint32_t bit = 1u << (int)(jt.slot - g_joy);
        CHECK((fresh & bit) != 0, "B5 a slot just read must be known (fresh 0x%x)", fresh);
        CHECK((b[0] >> 4) & 1u, "B5 the tap must still see the gear held (state before the hiding)");
        jt.OnState(DIERR_INPUTLOST, sizeof(js), &js);
        ControllerState(b, &pov, &fresh);
        CHECK((fresh & bit) == 0 && !((b[0] >> 4) & 1u), "B5 a failed read must make the slot unknown and all up");
    }
    SetMenuHide(kHideNone, nullptr, nullptr, 0);
}

int main() {
    TestScan();
    TestHide();
    printf("hotkey_scan_test: %d checks, %d failed -> %s\n", g_checks, g_fails, g_fails ? "FAIL" : "PASS");
    return g_fails ? 1 : 0;
}
