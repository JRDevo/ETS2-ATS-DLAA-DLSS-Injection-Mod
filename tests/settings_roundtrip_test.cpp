// Issue #26 test: every tuning menu row survives a game restart, and the Reset row really resets.
// Builds inject.cpp WITH_DLAA into this exe (tests\build_settings_roundtrip_test.bat) and drives the REAL menu code: MenuChange (the
// row's Left / Right), MenuBuildPanel (the value column the user reads), SaveSettings (the Save row), ResetSettings (the Reset hold)
// and LoadConfig (the start). A "game restart" is a fresh process of this exe: nothing in memory carries over, only dlaa.ini.
//
//   settings_roundtrip_test.exe            the whole test (exit 0 = all checks passed)
//   ... --row <MR_ id> --dir <+1|-1>       (child) start, press the row, Save, write the panel
//   ... --show                             (child) start, write the panel
//   ... --reset                            (child) start, press every row once, Reset, write the panel
//   ... --gates <row>                      (child) start, press <row>, then every row in turn: after EACH press the menu must
//                                          agree with the switches the render code reads (below), exit 1 when not
//
// For each row, Right and Left, flat and VR: the value column after the press must be the same after the restart -- for that row
// AND every other row (a press that changes a neighbour has to save the neighbour too). A press that changes nothing (a greyed
// row, an information row) is counted, not failed. Issue #26 (See-through depth back to "on" after a restart) fails here.
// The menu must not lie either (the other half of issue #26: the row read one thing, the code did another). GateCheck compares
// what a row shows (value, greyed) with the gate the render code itself tests -- PreIneligible for the pre-tonemap stage,
// g_fwdCfg && g_fwdOn for the forward-depth batch (FwdLive), the forward-depth replay for Thin-wire depth, XrTurboEffective, ...
// The ini lives next to this exe (PathNextToDll): build\tests\settings_rt\dlaa.ini, written fresh for every case.
#include "../src/inject.cpp"

#include <string>
#include <vector>

// the parts of src/dlaa.cpp inject.cpp calls that tests\fake_dlaa.cpp does not stand in for
static bool s_mvDilate = true;
void DlaaProcessor::SetMvDilate(bool on) { s_mvDilate = on; }
bool DlaaProcessor::MvDilate() { return s_mvDilate; }
bool DlaaProcessor::NgxInitFailed() { return false; }

static int s_checks = 0, s_fails = 0;
#define CHECK(cond, ...) do { ++s_checks; if (!(cond)) { ++s_fails; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// the dlaa.ini a case starts from: a fresh install (no beeps: the tone thread would outlive a short child), and the low profile
// (it switches profile keys off -- mirror DLAA, the see-through depth -- that a row must still be able to switch back on and save)
static const char* const kBaseIni[] = { "beeps = 0\n", "beeps = 0\nperf_profile = low\n" };
static const char* const kBaseName[] = { "fresh install", "perf_profile low" };
static int s_base = 0;
static std::wstring IniPath() { wchar_t p[MAX_PATH]; return PathNextToDll(L"dlaa.ini", p) ? p : L""; }
static std::wstring OutPath() { wchar_t p[MAX_PATH]; return PathNextToDll(L"panel.txt", p) ? p : L""; }

static bool WriteBaseIni() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, IniPath().c_str(), L"wb") != 0 || !f) return false;
    fwrite(kBaseIni[s_base], 1, strlen(kBaseIni[s_base]), f);
    fclose(f);
    return true;
}

// The start as StartInjection does it (no hooks, no threads): dlaa.ini, flat / VR launch, the saved mode.
static void Boot(bool vr) {
    LARGE_INTEGER qf;
    QueryPerformanceFrequency(&qf);
    g_qpcFreq = qf.QuadPart;
    LoadConfig();
    g_menuShowAll = true;                                // phase 84: every row in the panel (p.rows[r] = row r; flat / VR rows hidden otherwise)
    g_launchVr = vr ? 1 : 0;
    g_vrMode = vr;
    g_vrArea = SceneDlaa::Area();
    if (!vr) SceneDlaa::SetArea(100);
    g_dlaaOn = true;
    if (g_iniMode >= 0) {
        g_dlaaOn = g_iniMode != 0;
        if (g_iniMode != 0) g_dlssUpscale = g_iniMode == 2;
    }
}

static TuningMenu::Panel s_panel;
static void BuildPanel() {
    for (bool& o : g_menuGrpOpen) o = true;            // every group open: p.rows[i] is row MR_ i (closed groups leave theirs out)
    MenuBuildPanel(s_panel);
}
static void WritePanel() {
    BuildPanel();
    FILE* f = nullptr;
    if (_wfopen_s(&f, OutPath().c_str(), L"wb") != 0 || !f) { printf("cannot write panel.txt\n"); ExitProcess(3); }
    for (int r = 0; r < MR_COUNT; ++r) fwprintf(f, L"%ls%ls\n", s_panel.rows[r].greyed ? L"(greyed) " : L"", s_panel.rows[r].value);
    fclose(f);
}

// rows that are a live A/B switch on purpose (the saved choice is another row): after a restart they are back as they started
static bool SessionOnly(int r) {                       // Shift+F11 compare; the Mode row (End) is the saved one; phase 83: the game's
    return r == MR_DLAA || (r >= MR_UISET && r <= MR_UIRADIUS);   //   own VR UI cvars (config.cfg is the game's, never dlaa.ini)
}

// rows the round trip leaves out: group headers (open / close only), actions and pages, information rows
static bool SkipRow(int r) {
    switch (r) {
    case MR_PICGRP: case MR_MOTGRP: case MR_CMPGRP: case MR_VRPGRP: case MR_FRGRP: case MR_PANELGRP: case MR_FPSGRP:
    case MR_FPSREMEAS: case MR_MENUKEYS: case MR_HOTKEYS: case MR_SAVE: case MR_RESET: case MR_CLOSE:
    case MR_OFXR: case MR_VECTORXR:
    case MR_UIGRP: case MR_UIRESET:                    // phase 83: a header and an action
    case MR_BUGGRP: case MR_CAPKEY: case MR_CAPNOW:    // phase 89d: a header, the Hotkeys page, an action
        return true;
    default:
        return false;
    }
}

// ---- the menu against the render code's gates -----------------------------------------------------------------------------------
static const char* RowName(int r);
static int GateCheck(const char* after) {
    BuildPanel();
    int bad = 0;
    auto val = [](int r) { return (const wchar_t*)s_panel.rows[r].value; };
    auto grey = [](int r) { return s_panel.rows[r].greyed != 0; };
    auto on = [&](int r) { return !wcsncmp(val(r), L"on", 2); };
    auto off = [&](int r) { return !wcsncmp(val(r), L"off", 3); };
    auto expect = [&](bool ok, int r, const char* rule) {
        if (ok) return;
        ++bad;
        char v[128];
        WideCharToMultiByte(CP_UTF8, 0, val(r), -1, v, sizeof(v), nullptr, nullptr);
        printf("FAIL (%s, after %s): row \"%s\" shows \"%s\"%s -- %s\n", g_vrMode ? "VR" : "flat", after, RowName(r), v,
               grey(r) ? " (greyed)" : "", rule);
    };
    const bool fwd = g_fwdCfg && g_fwdOn.load();       // the forward-depth batch's switches (FwdLive / FwdOnDraw)
    const char* pre = PreIneligible();                 // the pre-tonemap stage decision (its mode reasons the user switches)
    const bool preUser = !(pre && (!strcmp(pre, "dlaa_pre_tonemap = 0") || !strncmp(pre, "pre-tonemap switched off", 24)));
    expect(on(MR_FWD) == fwd, MR_FWD, "Glass and fence fix: the menu and g_fwdCfg && g_fwdOn disagree");
    expect(on(MR_PRETM) == preUser, MR_PRETM, "Run DLAA earlier (test): the menu and PreIneligible() disagree");
    expect(!(on(MR_WIREDEPTH) && !grey(MR_WIREDEPTH)) || (fwd && g_fwdConsOn.load() >= 1), MR_WIREDEPTH,
           "Wire fix: depth reads on, but the forward-depth replay does not run it (See-through depth off)");
    expect(on(MR_WIREDEPTH) == (g_fwdConsOn.load() >= 1), MR_WIREDEPTH, "Wire fix: depth: the menu and g_fwdConsOn disagree");
    expect(on(MR_WIREWIDEN) == (g_wireCfg && g_wireOn.load()), MR_WIREWIDEN, "Thicker wires (test): the menu and WireOnDraw's switch disagree");
    expect(on(MR_WIREMV) == DlaaProcessor::MvDilate(), MR_WIREMV, "Wire fix: motion: the menu and the NGX dilation flag disagree");
    expect(on(MR_DRAWIDS) == (g_objMode == 2 && g_didCfg), MR_DRAWIDS, "Track moving parts: the menu and g_didCfg disagree");
    expect(grey(MR_WIDTH) == (SceneDlaa::Sharpness() <= 0.0f || !MainDlaaRuns()), MR_WIDTH,
           "Sharpness size is live only while Sharpen strength > 0 and the main scene runs DLAA");
    expect(grey(MR_LODCUT) == (g_lodBias == 0.0f), MR_LODCUT, "Leaves and fences sharpness is live only while Texture sharpness is on");
    expect(off(MR_MIRROR) == !(g_mirOn.load() && MirMode() != 0), MR_MIRROR, "Smooth mirrors: the menu and g_mirOn / MirMode() disagree");
    expect(grey(MR_MIRTURNS) || (g_mirOn.load() && MirMode() != 0 && MirMode() != 3), MR_MIRTURNS,
           "Mirrors take turns (faster) is not greyed, but mirror DLAA does not take turns (off / mode 3)");
    expect(off(MR_BIAS) == (SceneDlaa::BiasStrength() == 0) && g_biasCfg == SceneDlaa::BiasStrength(), MR_BIAS,
           "Light trail fix: the menu and SceneDlaa's bias strength disagree");
    expect(off(MR_MODE) == !g_dlaaOn.load() && on(MR_DLAA) == g_dlaaOn.load(), MR_MODE, "Mode / DLAA on-off and g_dlaaOn disagree");
    {   // phase 82: the Game TAA row names the mode the render code reads (g_gameTaa); its share row is live only in mode mix
        // phase 90: the Compare row is the split only (on / off); Mode names Mix exactly while DLAA runs with game_taa mix
        expect(!wcscmp(val(MR_GAMETAA), g_gameTaa == GameTaa::kSplit ? L"on" : L"off") && g_gameTaa == GameTaa::ClampMode(g_gameTaa),
               MR_GAMETAA, "Split with game TAA: the menu and g_gameTaa disagree");
        const bool modeOn = g_dlaaOn.load(), modeMix = modeOn && g_gameTaa == GameTaa::kMix, up = g_dlssUpscale.load();
        const wchar_t* const modeWant = !modeOn ? L"off" : modeMix ? (up ? L"Mix (TAA + DLSS)" : L"Mix (TAA + DLAA)")
                                                                   : (up ? L"DLSS" : L"DLAA");
        expect(!wcscmp(val(MR_MODE), modeWant), MR_MODE, "Mode: the menu and g_dlaaOn / g_dlssUpscale / g_gameTaa disagree");
        expect(grey(MR_TAAMIX) == !GameTaa::ShareUsed(g_gameTaa, g_gameTaaPair), MR_TAAMIX,
               "Game TAA amount is live exactly in mode mix and in split with a Mix side");
        static const wchar_t* const kPair[GameTaa::kPairCount] = { L"DLAA | TAA", L"Mix | TAA", L"DLAA | Mix" };
        expect(!wcscmp(val(MR_TAAPAIR), kPair[GameTaa::ClampPair(g_gameTaaPair)]) && grey(MR_TAAPAIR) == (g_gameTaa != GameTaa::kSplit),
               MR_TAAPAIR, "Split: left | right: the menu and g_gameTaaPair disagree, or not greyed exactly outside split");
        wchar_t pct[16];
        swprintf(pct, 16, L"%d %% TAA", g_gameTaaMix);
        expect(!wcscmp(val(MR_TAAMIX), pct) && g_gameTaaMix % GameTaa::kMixStep == 0, MR_TAAMIX,
               "Game TAA amount: the menu and g_gameTaaMix disagree (or off the 10 % grid after a press)");
        // phase 87: the AMD cache cores row names cpu_cache_pin and is greyed exactly on a CPU the pin cannot act on
        expect(!wcscmp(val(MR_CPUPIN), g_cpuCachePin ? L"on (auto)" : L"off") && grey(MR_CPUPIN) == !CpuPin::Supported(), MR_CPUPIN,
               "AMD cache cores: the menu and cpu_cache_pin / CpuPin::Supported() disagree");
        expect(grey(MR_TAASPLIT) == (g_gameTaa != GameTaa::kSplit), MR_TAASPLIT, "Split line position is live exactly in mode split");
        swprintf(pct, 16, L"%d %%", g_gameTaaSplit);
        expect(!wcscmp(val(MR_TAASPLIT), pct) && g_gameTaaSplit % GameTaa::kSplitStep == 0, MR_TAASPLIT,
               "Split line position: the menu and g_gameTaaSplit disagree (or off the 5 % grid after a press)");
        // phase 83: the In-game UI (VR) rows are live exactly in VR with the game's console on (config.cfg)
        for (int r = MR_UISET; r <= MR_UIRESET; ++r)
            expect(grey(r) == !(g_vrMode && g_vrUiConsole), r, "an In-game UI (VR) row is live exactly in VR with the game's console on");
        // the DLAA quality rows are greyed exactly while the main scene runs no DLAA (MainDlaaRuns)
        for (int r : { MR_MODEL, MR_SHARP, MR_BIAS })
            expect(grey(r) == !MainDlaaRuns(), r, "a DLAA quality row is greyed exactly while the main scene runs no DLAA");
    }
    if (g_vrMode) {
        expect(!wcscmp(val(MR_TURBO), L"on") == XrTurboEffective(), MR_TURBO, "Turbo mode reads \"on\" exactly when XrTurboEffective()");
        for (int r : { MR_VRSSIZE, MR_VRSPASS, MR_VRSSTR, MR_VRSEYE })
            expect(grey(r) || g_vrsOn, r, "a Foveated sub-row is not greyed while Foveated rendering is off");
    }
    return bad;
}

// ---- phase 85: the menu's mode rules (menu_show_all = 0, as the player sees it) ---------------------------------------------------
// the panel row whose name contains `name` (rows are compacted: hidden ones are left out), nullptr = not shown
static const TuningMenu::Row* FindRow(const wchar_t* name) {
    for (int i = 0; i < s_panel.nRows; ++i) if (wcsstr(s_panel.rows[i].name, name)) return &s_panel.rows[i];
    return nullptr;
}
// phase 88: the fps box lines for every FPS counter value (a measured 72 fps / 13.9 ms) -- the line text the box draws, and the
// height it is laid out with. (A phase 88 edit once dropped the line-1 copy: the box said "-- fps" for ever.)
static void FpsBoxLines() {
    const int text0 = g_fpsText, graph0 = g_fpsGraph;
    const bool show0 = g_fpsShow;
    g_fps.have = true; g_fps.fps = 72.0; g_fps.frameMs = 13.9;
    g_fpsShow = true;
    static const wchar_t* const kL1[kFpsTextModes] = { L"72 fps", L"72 fps", L"13.9 ms", L"72", L"72", L"13.9", L"" };
    static const wchar_t* const kL2[kFpsTextModes] = { L"13.9 ms", L"", L"", L"13.9", L"", L"", L"" };
    for (int v = 1; v < kFpsCounterValues; ++v) {
        FpsCounterSet(v);
        FpsSetLines();
        const int t = g_fpsText;
        CHECK(!wcscmp(g_fps.line1, kL1[t]) && !wcscmp(g_fps.line2, kL2[t]), "fps box value %d (fps_text %d): lines '%ls' / '%ls', "
              "expected '%ls' / '%ls'", v, t, g_fps.line1, g_fps.line2, kL1[t], kL2[t]);
        CHECK(FpsGraphShown() == (v % 2 == 0 || v == kFpsCounterValues - 1), "fps box value %d: graph shown %d", v, (int)FpsGraphShown());
        CHECK(FpsCounterValue() == v, "fps box value %d reads back as %d", v, FpsCounterValue());
    }
    g_fpsText = text0; g_fpsGraph = graph0; g_fpsShow = show0;
    g_fps = FpsMeter();
    FpsSetLines();
}

static void MenuRules() {
    FpsBoxLines();                                     // phase 88
    const bool vr0 = g_vrMode, all0 = g_menuShowAll, con0 = g_vrUiConsole;
    const int sel0 = g_menuSel;
    VrUiEnsure();
    g_menuShowAll = false;
    for (int vr = 0; vr <= 1; ++vr) {
        g_vrMode = vr != 0;
        // "closed (N)" counts only the rows shown now (this mode, phase 87: not hidden by their rule; the hide state comes from
        // the previous build, so the panel is built twice)
        for (bool& o : g_menuGrpOpen) o = false;
        g_menuSel = MR_MODE;
        MenuBuildPanel(s_panel);
        MenuBuildPanel(s_panel);
        int nFr = 0;
        for (int r = MR_FRGRP + 1; r <= MR_VRFPSCAP; ++r) if (MenuRowForMode(r)) ++nFr;
        wchar_t want[32];
        swprintf(want, 32, L"closed (%d)", nFr);
        const TuningMenu::Row* fr = FindRow(L"Frame rate");
        CHECK(fr && !wcscmp(fr->value, want) && nFr >= 2, "%s: Frame rate reads \"%ls\", want \"%ls\" (only the shown rows "
              "counted)", vr ? "VR" : "flat", fr ? fr->value : L"(not shown)", want);
        // the mode label in the title, no mode words in the shown row / group titles
        CHECK(wcsstr(s_panel.title, vr ? L"VR settings" : L"Flat settings") != nullptr, "%s: the title names the mode: \"%ls\"",
              vr ? "VR" : "flat", s_panel.title);
        for (bool& o : g_menuGrpOpen) o = true;
        MenuBuildPanel(s_panel);
        MenuBuildPanel(s_panel);
        for (int i = 0; i < s_panel.nRows; ++i) {
            const wchar_t* n = s_panel.rows[i].name;
            const bool word = wcsstr(n, L"(VR)") || wcsstr(n, L"(flat)") || wcsstr(n, L"VR performance") || wcsstr(n, L"VR FPS") ||
                              wcsstr(n, L"Panel VR");
            CHECK(!word, "%s: row \"%ls\" still carries a mode word (the title says the mode)", vr ? "VR" : "flat", n);
            CHECK(!s_panel.rows[i].greyed, "%s: row \"%ls\" is shown GREYED (phase 87: such a row is hidden)", vr ? "VR" : "flat", n);
        }
        CHECK(FindRow(vr ? L"Turbo mode" : L"Quality preset") && FindRow(L"FPS cap"), "%s: the renamed rows are shown", vr ? "VR" : "flat");
        // phase 87: every row greyed with menu_show_all is hidden without it (and the others are shown, if their group is)
        g_menuShowAll = true;
        MenuBuildPanel(s_panel);
        bool greyAll[MR_COUNT] = {};
        {   // the panel lists only the visible rows, in MR order (MenuBuildPanel compacts them): map them back to their ids
            int k = 0;
            for (int r = 0; r < MR_COUNT && k < s_panel.nRows; ++r)
                if (MenuRowVisible(r)) greyAll[r] = s_panel.rows[k++].greyed != 0;
        }
        g_menuShowAll = false;
        MenuBuildPanel(s_panel);
        MenuBuildPanel(s_panel);
        for (int r = 0; r < MR_COUNT; ++r)
            if (greyAll[r]) CHECK(!MenuRowVisible(r), "%s: row \"%s\" is greyed with menu_show_all but not hidden without it", vr ? "VR" : "flat", RowName(r));
    }
    // a row hidden by the mode inside an OPEN group moves the selection (flat -> VR flip on FPS cap (flat), the Frame rate group open)
    g_vrMode = false;
    for (bool& o : g_menuGrpOpen) o = true;
    g_menuSel = MR_FPSCAP;
    MenuBuildPanel(s_panel);
    CHECK(g_menuSel == MR_FPSCAP, "flat: FPS cap stays selected (%d)", g_menuSel);
    g_vrMode = true;
    MenuBuildPanel(s_panel);
    CHECK(g_menuSel != MR_FPSCAP && MenuRowForMode(g_menuSel), "VR: the selection left the hidden flat FPS cap row (now %s)", RowName(g_menuSel));
    // ... but a row hidden only by its COLLAPSED group keeps its place (it shows the header)
    g_menuSel = MR_VRFPSCAP;
    MenuGrpOpen(MR_FRGRP) = false;
    MenuBuildPanel(s_panel);
    CHECK(g_menuSel == MR_VRFPSCAP, "VR: a row in a collapsed group stays selected (%s)", RowName(g_menuSel));
    // phase 87: a row that DISAPPEARS by its rule while selected moves the selection (Sharpness size, Sharpen strength -> 0)
    {
        const float sh0 = SceneDlaa::Sharpness();
        for (bool& o : g_menuGrpOpen) o = true;
        SceneDlaa::SetSharpness(0.5f);
        g_menuSel = MR_WIDTH;
        MenuBuildPanel(s_panel); MenuBuildPanel(s_panel);
        const bool shown = MenuRowVisible(MR_WIDTH);
        SceneDlaa::SetSharpness(0.0f);
        MenuBuildPanel(s_panel); MenuBuildPanel(s_panel);
        CHECK(!shown || (!MenuRowVisible(MR_WIDTH) && g_menuSel != MR_WIDTH), "Sharpen strength 0: Sharpness size hidden and the selection moved (%s)",
              RowName(g_menuSel));
        SceneDlaa::SetSharpness(sh0);
    }
    // the console off: the In-game UI group is hidden (and cannot be opened); on: it opens (VR)
    g_vrUiConsole = false;
    MenuGrpOpen(MR_UIGRP) = false;
    MenuChange(MR_UIGRP, 1, false, 900);
    CHECK(!MenuGrpOpen(MR_UIGRP), "console off: Right does not open the In-game UI group");
    g_menuSel = MR_MODE;
    MenuBuildPanel(s_panel); MenuBuildPanel(s_panel);
    CHECK(!FindRow(L"In-game UI") && !FindRow(L"Height"), "console off: the In-game UI group is hidden");
    g_vrUiConsole = true;
    MenuBuildPanel(s_panel); MenuBuildPanel(s_panel);
    MenuChange(MR_UIGRP, 1, false, 901);
    CHECK(MenuGrpOpen(MR_UIGRP), "console on: Right opens the In-game UI group");
    MenuBuildPanel(s_panel); MenuBuildPanel(s_panel);
    const TuningMenu::Row* ui = FindRow(L"In-game UI");
    CHECK(ui && !ui->greyed && FindRow(L"Height"), "console on (VR): the group is shown and its rows too");
    // review fix (phase 89d): EVERY group header opens with Right and closes with Left (the Bug report header once did nothing)
    for (int gi = 0; gi < kMenuGroupCount; ++gi) {
        const int h = kMenuGroups[gi].header;
        MenuGrpOpen(h) = false;
        MenuChange(h, 1, false, 910 + gi);
        CHECK(MenuGrpOpen(h), "group header \"%s\": Right does not open it", RowName(h));
        MenuChange(h, -1, false, 930 + gi);
        CHECK(!MenuGrpOpen(h), "group header \"%s\": Left does not close it", RowName(h));
    }
    // phase 86: a Game TAA row change asks for the game's r_aa (typed later by VrUiTick, not called here: it would really type), and
    // r_aa is the game's: never written to dlaa.ini
    {
        const int taa0 = g_gameTaa;
        g_raaPending = false;
        g_gameTaa = GameTaa::kOff;                        // phase 90: the row's Right = split on (from off)
        MenuChange(MR_GAMETAA, 1, false, 902);
        CHECK(g_raaPending, "a Game TAA row change requests the game's r_aa");
        g_raaPending = false;
        WriteBaseIni();
        MenuChange(MR_SAVE, 1, false, 903);
        FILE* f = nullptr;
        std::string ini;
        if (_wfopen_s(&f, IniPath().c_str(), L"rb") == 0 && f) { char b2[4096]; size_t n; while ((n = fread(b2, 1, sizeof(b2), f)) > 0) ini.append(b2, n); fclose(f); }
        CHECK(!ini.empty() && ini.find("r_aa") == std::string::npos && ini.find("game_taa") != std::string::npos,
              "Save writes game_taa but never the game's r_aa");
        // phase 90: the Mode row walks off -> DLAA -> DLSS -> Mix (TAA + DLAA) -> Mix (TAA + DLSS) -> off; into / out of Mix asks for
        // r_aa and writes game_taa at once (like mode), and a split is replaced by Mix
        const bool on0 = g_dlaaOn.load(), up0 = g_dlssUpscale.load();
        g_gameTaa = GameTaa::kSplit;
        g_dlaaOn = true; g_dlssUpscale = true;            // DLSS -> Right = Mix (TAA + DLAA)
        g_raaPending = false;
        MenuChange(MR_MODE, 1, false, 904);
        CHECK(g_dlaaOn.load() && !g_dlssUpscale.load() && g_gameTaa == GameTaa::kMix && g_raaPending,
              "Mode Right from DLSS: Mix (TAA + DLAA), game_taa mix (the split replaced), r_aa requested");
        std::string ini2;
        if (_wfopen_s(&f, IniPath().c_str(), L"rb") == 0 && f) { char b2[4096]; size_t n; while ((n = fread(b2, 1, sizeof(b2), f)) > 0) ini2.append(b2, n); fclose(f); }
        CHECK(ini2.find("game_taa = 1") != std::string::npos || ini2.find("game_taa=1") != std::string::npos,
              "Mode into Mix writes game_taa 1 to dlaa.ini at once");
        MenuChange(MR_MODE, 1, false, 905);
        CHECK(g_dlssUpscale.load() && g_gameTaa == GameTaa::kMix, "Mode Right from Mix (TAA + DLAA): Mix (TAA + DLSS)");
        g_raaPending = false;
        MenuChange(MR_MODE, 1, false, 906);
        CHECK(!g_dlaaOn.load() && g_gameTaa == GameTaa::kOff && g_raaPending, "Mode Right from Mix (TAA + DLSS): off, game_taa off");
        MenuChange(MR_MODE, -1, false, 907);
        CHECK(g_dlaaOn.load() && g_dlssUpscale.load() && g_gameTaa == GameTaa::kMix, "Mode Left from off: Mix (TAA + DLSS)");
        g_dlaaOn = on0; g_dlssUpscale = up0;
        g_gameTaa = taa0;
        g_gameTaaExplicit = false;
    }
    g_vrMode = vr0; g_menuShowAll = all0; g_vrUiConsole = con0; g_menuSel = sel0;
    for (bool& h : g_menuHide) h = false;
    for (bool& o : g_menuGrpOpen) o = false;
}

// ---- the parent ----------------------------------------------------------------------------------------------------------------
static std::wstring s_exe;
static bool RunChild(const std::wstring& args) {
    std::wstring cl = L"\"" + s_exe + L"\" " + args;
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cl[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}
static std::vector<std::wstring> ReadPanel() {
    std::vector<std::wstring> v;
    FILE* f = nullptr;
    if (_wfopen_s(&f, OutPath().c_str(), L"rb") != 0 || !f) return v;
    wchar_t line[256];
    while (fgetws(line, 256, f)) {
        size_t l = wcslen(line);
        while (l && (line[l - 1] == L'\n' || line[l - 1] == L'\r')) line[--l] = 0;
        v.push_back(line);
    }
    fclose(f);
    return v;
}
static std::vector<std::wstring> Child(const std::wstring& args) {
    if (!RunChild(args)) return {};
    return ReadPanel();
}
static const char* RowName(int r) {
    static char buf[MR_COUNT][96];
    BuildPanel();
    WideCharToMultiByte(CP_UTF8, 0, s_panel.rows[r].name, -1, buf[r], sizeof(buf[r]), nullptr, nullptr);
    return buf[r];
}
static std::string U8(const std::wstring& w) {
    char b[512];
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, b, sizeof(b), nullptr, nullptr);
    return b;
}

// one case: the press (or two presses, a restart between them) then a restart; the panel must not change across the restart
static void Case(int r, int dir, int presses, const wchar_t* vrArg, const char* what, const std::vector<std::wstring>& fresh,
                 int& changed, int& unchanged) {
    WriteBaseIni();
    std::vector<std::wstring> after;
    for (int i = 0; i < presses; ++i) {                // each press its own start + Save (the second one starts from the saved file)
        wchar_t a[64];
        swprintf(a, 64, L"--row %d --dir %d%ls", r, dir, vrArg);
        after = Child(a);
        if (after.size() != (size_t)MR_COUNT) { CHECK(false, "%s: row \"%s\": the change child failed", what, RowName(r)); return; }
    }
    if (presses == 1 && after == fresh) { ++unchanged; return; }   // the press changed nothing here (greyed / information)
    ++changed;
    const std::vector<std::wstring> back = Child(std::wstring(L"--show") + vrArg);
    if (back.size() != (size_t)MR_COUNT) { CHECK(false, "%s: row \"%s\": the restart child failed", what, RowName(r)); return; }
    const std::vector<std::wstring>& want = SessionOnly(r) ? fresh : after;
    for (int k = 0; k < MR_COUNT; ++k)
        CHECK(want[k] == back[k], "%s: row \"%s\" %s x%d: \"%s\" shows \"%s\" before the restart, \"%s\" after it%s", what, RowName(r),
              dir > 0 ? "Right" : "Left", presses, RowName(k), U8(want[k]).c_str(), U8(back[k]).c_str(),
              SessionOnly(r) ? " (a session-only switch must not be saved)" : " (not saved)");
}

// ---- phase 89 review fixes: the capture / rewind helpers that need no GPU ------------------------------------------------------
static void CaptureRules() {
    // #2 the rewind length does not follow a rolling flat Present rate frame by frame
    const float secs0 = g_rewindSecs; const int frames0 = g_rewindFrames; const bool vr0 = g_vrMode;
    g_vrMode = false;
    g_rewindSecs = 1.0f;
    auto fill = [](float ms) { for (int i = 0; i < Cap::kFt; ++i) Cap::g_ft[i] = ms; Cap::g_ftN = Cap::kFt; Cap::g_ftHead = 0; };
    fill(1000.0f / 72.0f);
    const int n72 = Rewind::TargetFrames();
    CHECK(n72 == 72, "rewind 1 s at 72 fps = %d frames (72)", n72);
    int changes = 0, last = n72;
    for (int k = 0; k < 200; ++k) {                    // +-4 % jitter of the measured rate: never a new length
        fill(1000.0f / (72.0f * (k % 2 ? 1.04f : 0.96f)));
        const int t = Rewind::TargetFrames();
        changes += t != last; last = t;
    }
    CHECK(changes == 0, "rewind length changed %d times under +-4 %% rate noise (the ring would be rebuilt each time)", changes);
    fill(1000.0f / 45.0f);                             // a real new rate: taken after it stayed > 10 % off for ~2 s
    int t = 0;
    for (int k = 0; k < 400; ++k) t = Rewind::TargetFrames();
    CHECK(t == 45, "rewind 1 s after a lasting drop to 45 fps = %d frames (45)", t);
    // #7 StepRewindSecs from the old rewind_frames alone: Left reaches off
    g_rewindSecs = -1.0f; g_rewindFrames = 64;
    fill(1000.0f / 60.0f);
    StepRewindSecs(-1, 1);                             // 64 frames at 60 fps ~ 1.07 s -> nearest 1 s -> Left = 0.5 s
    CHECK(g_rewindSecs == 0.5f, "rewind_frames 64 at 60 fps, Left: %.1f s (0.5)", (double)g_rewindSecs);
    StepRewindSecs(-1, 2);
    CHECK(g_rewindSecs == 0.0f, "Left again: off (%.1f)", (double)g_rewindSecs);
    g_rewindSecs = secs0; g_rewindFrames = frames0; g_vrMode = vr0;
    // #3 a capture's set 0 / the restore without a change does not reset the DLSS history
    Cap::g_base = Cap::Base{ g_dlaaOn.load(), g_jitterOnly.load(), g_mvDebug.load(), g_mvOn.load(), g_objMode, LogLevel(), g_beeps };
    Cap::g_partsSwitched = false;
    for (int level : { 1, 2 }) {
        Cap::g_plan = BugReport::Plan(level, &Cap::g_planN);
        g_resetNext = false;
        Cap::ApplySet(0);
        CHECK(!g_resetNext, "capture_mode %d: set 0 (the player's picture) reset the DLSS history", level);
        CHECK(!Cap::ApplyBase() && !g_resetNext, "capture_mode %d: the restore without a change reset the DLSS history", level);
        Cap::ApplySet(1);                              // phase 89e: set 1 = DLAA off (the second set)
        CHECK(!g_dlaaOn.load() && g_resetNext, "capture_mode %d: the DLAA-off set", level);
        g_resetNext = false;
        CHECK(Cap::ApplyBase() && g_dlaaOn.load() == Cap::g_base.dlaa && g_resetNext, "capture_mode %d: the restore after DLAA off", level);
    }
    // #5 formats: BGRA from the format, unknown formats refused
    CHECK(Cap::IsBgra8(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) && !Cap::IsBgra8(DXGI_FORMAT_R8G8B8A8_UNORM) &&
          !Cap::Convertible(DXGI_FORMAT_R10G10B10A2_UNORM) && !Cap::Convertible(DXGI_FORMAT_R11G11B10_FLOAT) &&
          Cap::Convertible(DXGI_FORMAT_R16G16_FLOAT), "capture formats");
    {   Cap::Pix p; p.w = 1; p.h = 1; p.pitch = 4; p.f = DXGI_FORMAT_B8G8R8A8_UNORM; p.bgra = true; p.px = { 10, 20, 30, 255 };
        std::vector<uint8_t> o;
        CHECK(Cap::ToRgba8(p, o, L"t.png") && o[0] == 30 && o[1] == 20 && o[2] == 10, "BGRA -> RGBA byte order");
        p.f = DXGI_FORMAT_R10G10B10A2_UNORM;
        CHECK(!Cap::ToRgba8(p, o, L"t.png"), "R10G10B10A2 is refused, not mis-read"); }
    {   // phase 89e: the MV picture is auto-ranged (8 px = full red), the HDR plane auto-exposed (0.045 grey -> ~0.18 -> sRGB ~117)
        Cap::Pix p; p.w = 1; p.h = 1; p.pitch = 4; p.f = DXGI_FORMAT_R16G16_FLOAT; p.px = { 0x00, 0x48, 0x00, 0x00 };   // (8.0, 0.0) half
        std::vector<uint8_t> o;
        CHECK(Cap::ToRgba8(p, o, L"mv.png") && o[0] == 255 && o[1] == 128, "MV auto range: 8 px -> red %d green %d (255 / 128)", o[0], o[1]);
        p.pitch = 8; p.f = DXGI_FORMAT_R16G16B16A16_FLOAT;
        p.px = { 0xC3, 0x29, 0xC3, 0x29, 0xC3, 0x29, 0x00, 0x3C };   // 0.045 grey (half 0x29C3), alpha 1
        CHECK(Cap::ToRgba8(p, o, L"c.png") && o[0] > 105 && o[0] < 130, "HDR auto exposure: 0.045 grey -> %d (about 117)", o[0]);
        std::lock_guard<std::mutex> lk(Cap::g_noteMx);
        CHECK(Cap::g_notes.size() >= 2, "the exposure / MV range notes for info.txt");
        Cap::g_notes.clear();
    }
    // #10 the writer's RAM cap
    Cap::g_queuedBytes = Cap::kQueueCap + 1;
    CHECK(Cap::WriterBehind(), "writer behind above the cap");
    Cap::g_queuedBytes = 0;
    CHECK(!Cap::WriterBehind(), "not behind at 0");
}

static int Parent() {
    if (IniPath().empty()) { printf("FAIL: no dlaa.ini path\n"); return 1; }
    Boot(false);                                       // (row names for the messages)
    MenuRules();                                       // phase 85
    CaptureRules();                                    // phase 89 review fixes
    int changed = 0, unchanged = 0;
    for (s_base = 0; s_base < (int)(sizeof(kBaseIni) / sizeof(kBaseIni[0])); ++s_base)
    for (int vr = 0; vr <= 1; ++vr) {
        const wchar_t* vrArg = vr ? L" --vr" : L"";
        char what[64];
        snprintf(what, sizeof(what), "%s, %s", vr ? "VR" : "flat", kBaseName[s_base]);
        // the start, twice: the panel must be the same (no row text that changes between two plain starts)
        WriteBaseIni();
        const std::vector<std::wstring> fresh = Child(std::wstring(L"--show") + vrArg);
        const std::vector<std::wstring> fresh2 = Child(std::wstring(L"--show") + vrArg);
        CHECK(fresh.size() == (size_t)MR_COUNT && fresh == fresh2, "%s: two plain starts give a different panel", what);
        if (fresh.size() != (size_t)MR_COUNT) return 1;

        for (int r = 0; r < MR_COUNT; ++r) {
            if (SkipRow(r)) continue;
            for (int dir = 1; dir >= -1; dir -= 2) {
                Case(r, dir, 1, vrArg, what, fresh, changed, unchanged);   // a press from the start state
                Case(r, dir, 2, vrArg, what, fresh, changed, unchanged);   // a 2nd press after a restart: from the SAVED state (issue #26:
            }                                                              //   a saved off must switch back on, and that saves too)
        }

        // the menu against the gates: from the start state, each row first, then every row in turn (many combinations)
        for (int r = 0; r < MR_COUNT; ++r) {
            if (SkipRow(r)) continue;
            WriteBaseIni();
            wchar_t a[48];
            swprintf(a, 48, L"--gates %d%ls", r, vrArg);
            ++s_checks;
            if (!RunChild(a)) { ++s_fails; printf("  ^ %s: gates after row \"%s\" first\n", what, RowName(r)); }
        }

        // Reset (fresh install only: Reset removes perf_profile): every row pressed once, Saved, then Reset -> the fresh panel,
        // live and after a restart
        if (s_base != 0) continue;
        WriteBaseIni();
        const std::vector<std::wstring> reset = Child(std::wstring(L"--reset") + vrArg);
        const std::vector<std::wstring> back = Child(std::wstring(L"--show") + vrArg);
        CHECK(reset.size() == (size_t)MR_COUNT && back.size() == (size_t)MR_COUNT, "%s: the reset children failed", what);
        if (back.size() == (size_t)MR_COUNT && reset.size() == (size_t)MR_COUNT)
            for (int k = 0; k < MR_COUNT; ++k) {
                CHECK(reset[k] == fresh[k], "%s: Reset leaves \"%s\" at \"%s\" (fresh install: \"%s\")", what, RowName(k),
                      U8(reset[k]).c_str(), U8(fresh[k]).c_str());
                CHECK(back[k] == fresh[k], "%s: after Reset + restart \"%s\" is \"%s\" (fresh install: \"%s\")", what, RowName(k),
                      U8(back[k]).c_str(), U8(fresh[k]).c_str());
            }
    }
    DeleteFileW(IniPath().c_str());
    DeleteFileW(OutPath().c_str());
    CHECK(changed >= 300, "only %d cases changed the panel (the test drives less than it should)", changed);
    printf("settings_roundtrip_test: %d cases changed a value and were checked across a restart, %d presses changed nothing "
           "(greyed / information); %d checks, %d failed\n", changed, unchanged, s_checks, s_fails);
    return s_fails ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    bool vr = false, show = false, reset = false;
    int row = -1, dir = 1, gates = -1;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--vr")) vr = true;
        else if (!wcscmp(argv[i], L"--show")) show = true;
        else if (!wcscmp(argv[i], L"--reset")) reset = true;
        else if (!wcscmp(argv[i], L"--row") && i + 1 < argc) row = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--dir") && i + 1 < argc) dir = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--gates") && i + 1 < argc) gates = _wtoi(argv[++i]);
    }
    if (!show && !reset && row < 0 && gates < 0) {
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        s_exe = p;
        return Parent();
    }
    Boot(vr);
    if (gates >= 0) {                                  // the menu against the gates after every press, four rounds over all rows:
        int bad = GateCheck("the start");              //   one Right, one Left, then every row to its low end and its high end
        MenuChange(gates, 1, false, 1000);             //   (12 presses: Sharpen strength 0 = off, the LOD bias 0, the caps, ...)
        bad += GateCheck(RowName(gates));
        static const int kDir[4] = { 1, -1, -1, 1 }, kPresses[4] = { 1, 1, 12, 12 };
        for (int round = 0; round < 4; ++round)
            for (int r = 0; r < MR_COUNT; ++r) {
                if (SkipRow(r) || SessionOnly(r)) continue;
                for (int i = 0; i < kPresses[round]; ++i) {
                    MenuChange(r, kDir[round], false, 1001);
                    bad += GateCheck(RowName(r));
                }
            }
        return bad ? 1 : 0;
    }
    if (row >= 0) {                                    // the user: Right / Left on the row, then the Save row
        MenuChange(row, dir, false, 1000);
        MenuChange(MR_SAVE, 1, false, 1001);
    } else if (reset) {
        for (int r = 0; r < MR_COUNT; ++r)
            if (!SkipRow(r)) MenuChange(r, 1, false, 1000);
        MenuChange(MR_SAVE, 1, false, 1001);
        ResetSettings(1002);
    }
    WritePanel();
    return 0;
}
