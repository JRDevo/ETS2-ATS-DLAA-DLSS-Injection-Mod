// phase 83 test: src/vr_ui.h -- the game's VR UI placement typed into its console. Pure logic, no GPU, no game.
//   tests\build_vr_ui_test.bat  ->  build\tests\vr_ui_test.exe   (exit 0 = all checks passed)
// config.cfg parsing (exact names, last line wins, the console switches), the menu steps, the command text, the debounce / queue
// (sent once stable for 0.4 s, one command per changed cvar, nothing when back to the game's value) and the keystroke plan.
#include "../src/vr_ui.h"

#include <cstdio>

static int s_checks = 0, s_fails = 0;
#define CHECK(cond, ...) do { ++s_checks; if (!(cond)) { ++s_fails; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace VrUi;

static const char* const kCfg =
    "# prism3d variable config data\n"
    "uset r_manual_stereo_ui_pitch_game \"-0.5\"\n"
    "uset r_manual_stereo_ui_y_game \"-0.25\"\n"
    "uset r_manual_stereo_ui_x_game \"0.0\"\n"
    "uset r_manual_stereo_ui_y \"-0.0\"\n"
    "uset r_manual_stereo_ui_fov_game \"50\"\n"
    "uset r_manual_stereo_ui_dist_game \"1.0\"\n"
    "uset r_manual_stereo_ui_y_game \"-0.4\"\n"       // a later line wins
    "uset g_developer \"1\"\n"
    "uset g_console_state \"0\"\n"
    "uset g_console \"1\"\n";

static void TestParse() {
    const std::string cfg = kCfg;
    float v = 9.0f;
    CHECK(ParseCfg(cfg, "r_manual_stereo_ui_y_game", &v) && v == -0.4f, "the last y_game line: %.2f", v);
    CHECK(ParseCfg(cfg, "r_manual_stereo_ui_y", &v) && v == 0.0f, "y (menus) is not y_game: %.2f", v);
    CHECK(ParseCfg(cfg, "r_manual_stereo_ui_fov_game", &v) && v == 50.0f, "fov_game");
    CHECK(!ParseCfg(cfg, "r_manual_stereo_ui_radius_game", &v), "a missing cvar");
    CHECK(VarName(kDriving, kY) == "r_manual_stereo_ui_y_game" && VarName(kMenus, kFov) == "r_manual_stereo_ui_fov", "names");
    CHECK(ConsoleEnabled(cfg), "g_developer 1 + g_console 1 = console on");
    CHECK(!ConsoleEnabled("uset g_developer \"1\"\nuset g_console \"0\"\n") && !ConsoleEnabled("uset g_console \"1\"\n") &&
          !ConsoleEnabled(""), "console off / missing");
    CHECK(!ConsoleEnabled("uset g_console_state \"1\"\nuset g_developer \"1\"\n"), "g_console_state is not g_console");
}

static void TestStepFormat() {
    CHECK(Step(kY, -0.4f, 1) == -0.35f && Step(kY, -0.4f, -1) == -0.45f, "y 0.05 steps: %.3f / %.3f", Step(kY, -0.4f, 1), Step(kY, -0.4f, -1));
    CHECK(Step(kY, -0.43f, -1) == -0.45f && Step(kY, -0.43f, 1) == -0.40f, "an off-grid value snaps to the grid");
    CHECK(Step(kY, 3.0f, 1) == 3.0f && Step(kDist, 0.2f, -1) == 0.2f, "clamped");
    CHECK(Step(kFov, 50.0f, 1) == 51.0f && Step(kDist, 1.0f, 1) == 1.1f, "fov 1, dist 0.1");
    CHECK(Format(kY, -0.45f) == "-0.45" && Format(kY, -0.0f) == "0.00" && Format(kFov, 51.0f) == "51" && Format(kRadius, 2.5f) == "2.5",
          "formats: %s %s %s %s", Format(kY, -0.45f).c_str(), Format(kY, -0.0f).c_str(), Format(kFov, 51.0f).c_str(),
          Format(kRadius, 2.5f).c_str());
    CHECK(Command(kDriving, kY, -0.45f) == "uset r_manual_stereo_ui_y_game \"-0.45\"", "command: %s", Command(kDriving, kY, -0.45f).c_str());
}

static void TestDebounce() {
    Debounce d;
    d.Known(kDriving, kY, -0.4f);
    d.Known(kDriving, kX, 0.0f);
    d.Changed(kDriving, kY, -0.45f, 1000);
    CHECK(d.Due(1200).empty() && d.AnyDirty(), "not yet stable (0.2 s)");
    d.Changed(kDriving, kY, -0.5f, 1300);                  // a second press restarts the wait
    CHECK(d.Due(1600).empty(), "the second press restarted the 0.4 s");
    std::vector<Pending> due = d.Due(1700);
    CHECK(due.size() == 1 && due[0].field == kY && due[0].value == -0.5f, "sent once, the last value");
    CHECK(d.Due(5000).empty() && !d.AnyDirty(), "sent only once");
    d.Changed(kDriving, kY, -0.45f, 6000);
    d.Changed(kDriving, kY, -0.5f, 6100);                  // back to what the game has
    CHECK(d.Due(7000).empty(), "back to the game's value: nothing sent");
    d.Changed(kDriving, kY, -0.55f, 8000);
    d.Changed(kDriving, kX, 0.05f, 8000);
    d.Changed(kMenus, kFov, 66.0f, 8000);
    due = d.Due(8500);
    CHECK(due.size() == 3, "one command per changed cvar: %zu", due.size());
}

static void TestPlan() {
    const std::vector<std::string> cmds = { "uset a \"1\"", "uset b \"2\"" };
    const std::vector<KeyStep> p = KeyPlan(cmds);
    CHECK(KeyPlan({}).empty(), "nothing to send: no plan");
    // open: grave down, wait, grave up; then the text; Enter (down, wait, up); ...; close with grave
    CHECK(p.size() > 6 && p[0].kind == kKeyDown && p[0].dik == kDikGrave && p[1].kind == kWait && p[2].kind == kKeyUp &&
          p[2].dik == kDikGrave && p[3].kind == kWait && p[3].ms == kOpenMs, "the console opens first");
    std::string typed;
    int enters = 0, graves = 0;
    for (const KeyStep& s : p) {
        if (s.kind == kChar) typed += s.ch;
        if (s.kind == kKeyDown && s.dik == kDikReturn) ++enters;
        if (s.kind == kKeyDown && s.dik == kDikGrave) ++graves;
    }
    CHECK(typed == "uset a \"1\"uset b \"2\"" && enters == 2 && graves == 2, "the text in order, one Enter per command, console open + close");
    CHECK(p[p.size() - 4].kind == kKeyDown && p[p.size() - 4].dik == kDikGrave, "the console closes last");
    for (size_t i = 0; i + 1 < p.size(); ++i)
        if (p[i].kind == kKeyDown) CHECK(p[i + 1].kind == kWait && p[i + 1].ms >= kKeyMs, "each key is held %d ms", kKeyMs);
    bool sh = true;
    CHECK(DikOfChar('u', &sh) == 0x16 && !sh && DikOfChar('_', &sh) == 0x0C && sh && DikOfChar('"', &sh) == 0x28 && sh &&
          DikOfChar('-', &sh) == 0x0C && !sh && DikOfChar('0', &sh) == 0x0B && DikOfChar('.', &sh) == 0x34 && DikOfChar(' ', &sh) == 0x39,
          "US scan codes (DirectInput-only mode)");
    for (char c : std::string("uset r_manual_stereo_ui_y_game \"-0.45\""))
        CHECK(DikOfChar(c, &sh) != 0, "every command character has a scan code: '%c'", c);
}

// phase 85: the delivery modes (vr_ui_keys, renumbered: 0 SendInput default) and the game.log.txt echo that confirms a command
static void TestDelivery() {
    CHECK(kKeysSendInput == 0 && KeysUseSendInput(0) && KeysUseSendInput(kKeysSendScan) && !KeysUseSendInput(kKeysMixed) &&
          !KeysUseSendInput(kKeysWindow) && !KeysUseSendInput(kKeysDirectInput), "0 / 1 = SendInput, 2 .. 4 = the phase 83 paths");
    CHECK(ClampKeys(-3) == 0 && ClampKeys(2) == 2 && ClampKeys(4) == 4 && ClampKeys(9) == 4, "vr_ui_keys clamped to 0 .. 4");
    for (int m = 0; m < kKeysModes; ++m) CHECK(strcmp(KeysName(m), "?") != 0, "mode %d has a name for the log", m);
    const std::string cmd = "uset r_manual_stereo_ui_y_game \"-0.45\"";
    const std::string log = "00:02:13.004 : [sys] something\n00:02:13.511 : uset r_manual_stereo_ui_y_game \"-0.45\"\r\n";
    CHECK(Echoed(log, cmd), "the echoed line confirms the command");
    CHECK(!Echoed(log, "uset r_manual_stereo_ui_y_game \"-0.4\""), "another value is not confirmed (exact text)");
    CHECK(!Echoed("00:00:01.000 : uset r_manual_stereo_ui_y_game \"-0.45\"5\n", cmd), "a longer value is not confirmed");
    CHECK(!Echoed("00:00:01.000 : xuset r_manual_stereo_ui_y_game \"-0.45\"\n", cmd), "a word ending in it is not confirmed");
    CHECK(Echoed(cmd, cmd) && Echoed("> " + cmd + "\n", cmd), "at the start of the read bytes / after a prompt");
    CHECK(!Echoed("", cmd), "an empty log confirms nothing");
}

// phase 86: the Game TAA row -> the game's r_aa (0 off, 5 TAA): what to type, or nothing
static void TestRaa() {
    CHECK(RaaFor(0) == 0 && RaaFor(1) == 5 && RaaFor(2) == 5, "off -> r_aa 0, mix / split -> r_aa 5");
    CHECK(RaaDecide(1, true, kRaaUnknown) == 5 && RaaDecide(2, true, 0) == 5 && RaaDecide(0, true, 5) == 0 && RaaDecide(0, true, 2) == 0,
          "a row change types the mode's value when the game's differs (or is unknown)");
    CHECK(RaaDecide(1, true, 5) == kRaaUnknown && RaaDecide(2, true, 5) == kRaaUnknown && RaaDecide(0, true, 0) == kRaaUnknown,
          "the game has that value already: nothing typed (mix -> split types nothing)");
    for (int m = 0; m < 3; ++m)
        for (int k : { kRaaUnknown, 0, 2, 5 }) CHECK(RaaDecide(m, false, k) == kRaaUnknown, "console off: nothing typed (mode %d, r_aa %d)", m, k);
    const char q = 34;
    CHECK(RaaCommand(5) == std::string("uset r_aa ") + q + "5" + q && RaaCommand(0) == std::string("uset r_aa ") + q + "0" + q, "the command text");
    const std::string cfg = std::string("uset r_aa ") + q + "5" + q + "\nuset r_aa_x " + q + "3" + q + "\n";
    CHECK(RaaFromCfg(cfg) == 5 && RaaFromCfg(std::string("uset g_lang ") + q + "en_us" + q + "\n") == kRaaUnknown, "config.cfg r_aa (exact name; missing = unknown)");
}

int main() {
    TestRaa();
    TestDelivery();
    TestParse();
    TestStepFormat();
    TestDebounce();
    TestPlan();
    printf("vr_ui_test: %d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
