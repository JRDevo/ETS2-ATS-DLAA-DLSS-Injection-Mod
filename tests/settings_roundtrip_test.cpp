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
    g_slDrawHooksOk = true;                              // phase 105a: the start installs the shadow lock's draw hooks (no device here)
    g_didShadowOn = true;                                // phase 109r: ... and did_shadow's VSSetShader hook (ShadowLockUsable needs it)
    g_censusReady = true;                                // phase 110: ... and the census (CensusStart: the PS hooks) -- its row works live
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
    // phase 112c: the sharpen block -- the five value rows only with Sharpen preset Custom (each with its own rule on top)
    const bool shC = g_sharpPreset == SharpPreset::kCustom;
    expect(grey(MR_WIDTH) == (SceneDlaa::Sharpness() <= 0.0f || !MainDlaaRuns() || SceneDlaa::SharpType() == SceneDlaa::kSharpNis || !shC),
           MR_WIDTH, "Sharpness size is live only while Sharpen strength > 0, the main scene runs DLAA, the type is RCAS and the preset is Custom");
    expect(grey(MR_SHARPTYPE) == (!MainDlaaRuns() || !shC), MR_SHARPTYPE,   // phase 101d: also shown at Sharpen strength 0
           "Sharpen type is live whenever the main scene runs DLAA and the preset is Custom (also at Sharpen strength 0)");
    expect(grey(MR_SHARP) == (!MainDlaaRuns() || !shC), MR_SHARP, "Sharpen strength is live exactly with DLAA and the preset Custom");
    for (int r : { MR_SHARPHALO, MR_SHARPLUMA })      // phase 104: RCAS-only, also shown at Sharpen strength 0 (the 101d rule);
        expect(grey(r) == (!MainDlaaRuns() || SceneDlaa::SharpType() == SceneDlaa::kSharpNis ||   // 105b: only with preset Custom
                           !shC), r,
               "Halo limit / Sharpen colour are live exactly while the main scene runs DLAA with RCAS and the preset is Custom "
               "(also at Sharpen strength 0)");
    {   // phase 105b / 112c: the Sharpen preset row -- shown with DLAA (any type), its name, and a named preset = all its values
        expect(grey(MR_SHARPPRESET) == !MainDlaaRuns(), MR_SHARPPRESET,
               "Sharpen preset is live exactly while the main scene runs DLAA (also at Sharpen strength 0 and with NIS)");
        static const wchar_t* const kP[SharpPreset::kCount] = { L"Off", L"Soft", L"Balanced", L"Crisp", L"Extra crisp", L"Custom" };
        const int sp = g_sharpPreset;
        expect(sp >= 0 && sp < SharpPreset::kCount && !wcscmp(val(MR_SHARPPRESET), kP[sp]), MR_SHARPPRESET,
               "Sharpen preset: the menu and g_sharpPreset disagree");
        expect(sp == SharpPreset::kCustom || SharpPreset::Matches(sp, SharpNow()), MR_SHARPPRESET,
               "Sharpen preset: a named preset is shown, but the sharpen values are not its values (a hand change must make "
               "it Custom)");
    }
    {   // phase 112c: the group presets -- the rows name g_grpPreset; Recommended = exactly the defaults; detail rows only with Custom
        const int gv = g_grpPreset[GroupPreset::kVrs], gp = g_grpPreset[GroupPreset::kPanel];
        expect(!wcscmp(val(MR_VRSPRESET), gv == GroupPreset::kCustom ? L"Custom" : L"Recommended"), MR_VRSPRESET,
               "Foveated preset: the menu and g_grpPreset disagree");
        expect(!wcscmp(val(MR_PANELPRESET), gp == GroupPreset::kCustom ? L"Custom" : L"Recommended"), MR_PANELPRESET,
               "Panel preset: the menu and g_grpPreset disagree");
        expect(gv == GroupPreset::kCustom || GroupPreset::IsRecommended(GroupPreset::kVrs, GrpRead(GroupPreset::kVrs)), MR_VRSPRESET,
               "Foveated preset Recommended, but a Foveated value is not the default");
        expect(gp == GroupPreset::kCustom || GroupPreset::IsRecommended(GroupPreset::kPanel, GrpRead(GroupPreset::kPanel)), MR_PANELPRESET,
               "Panel preset Recommended, but a panel depth / facing value is not the default");
        for (int r : { MR_VRSSIZE, MR_VRSPASS, MR_VRSSTR })
            expect(gv == GroupPreset::kCustom || grey(r), r, "a Foveated detail row is shown with Foveated preset Recommended");
        for (int r : { MR_DEPTH, MR_FACE })
            expect(gp == GroupPreset::kCustom || grey(r), r, "a Panel detail row is shown with Panel preset Recommended");
    }
    // phase 105a: Steady own shadow -- the value is the player's choice, the render state follows it (now or at the next slice
    // set: g_slWant), and the row is hidden exactly without its hooks or while the main scene runs no DLAA
    expect(!wcscmp(val(MR_SHADOW), g_slUser ? L"on" : L"off") && (g_slWant >= 0 ? g_slWant : g_slCfg) == g_slUser &&
           (!g_slUser || g_slOn), MR_SHADOW, "Steady own shadow: the menu, g_slUser and the lock's pending / live state disagree");
    // phase 112: the fix rows are shown with Custom only (and, like the preset row, only while the main scene runs DLAA)
    const bool picCustom = g_picPreset == PicFixes::kCustom && MainDlaaRuns();
    expect(grey(MR_SHADOW) == (!picCustom || !ShadowLockUsable()), MR_SHADOW,
           "Steady own shadow is live exactly while the main scene runs DLAA and its draw hooks are in (and Picture fixes is Custom)");
    {   // phase 112: Picture fixes -- the row names g_picPreset; Standard / Off = exactly their values (live); the fix rows hidden
        // outside Custom; each fix row shows its switch
        static const wchar_t* const kP[PicFixes::kCount] = { L"Recommended", L"Off", L"Custom" };   // (112c: was Standard)
        const int pp = g_picPreset;
        expect(pp >= 0 && pp < PicFixes::kCount && !wcscmp(val(MR_PICFIX), kP[pp]) && grey(MR_PICFIX) == !MainDlaaRuns(), MR_PICFIX,
               "Picture fixes: the menu and g_picPreset disagree (or the row is not shown exactly while the main scene runs DLAA)");
        const PicFixes::Set now = PicFixRead();
        // (phase 112r: Off keeps dlaa_mv_dilate at whatever it was -- OffOver(now); every other fix must be exactly Off's)
        expect(pp == PicFixes::kCustom || PicFixes::Equal(now, PicFixes::LiveOver(pp, g_picCustom, now)), MR_PICFIX,
               "Picture fixes: Standard / Off is shown, but a fix switch is not at that preset's value (a hand change must make it Custom)");
        for (int r : { MR_FWD, MR_BIAS, MR_POLE, MR_WIREMV, MR_WIREWIDEN, MR_FXLIGHT, MR_FXROOF, MR_FXCUTOUT })   // 112c: + Glass and fence
            if (!picCustom) expect(grey(r), r, "a Picture fix row is shown while the preset is not Custom");
        expect(grey(MR_BIAS) == !picCustom, MR_BIAS, "Light trail fix: shown exactly with Custom while DLAA runs");
        expect(grey(MR_WIREMV) == !picCustom && grey(MR_WIREWIDEN) == !picCustom && grey(MR_FXLIGHT) == !picCustom &&   // (+ DLAA runs)
               grey(MR_FXROOF) == !picCustom && grey(MR_FXCUTOUT) == !picCustom && grey(MR_FWD) == !picCustom, MR_PICFIX,
               "the always-usable fix rows are shown exactly with Custom");
        expect(grey(MR_FXPOWER) == (!picCustom || !fwd), MR_FXPOWER, "Power line fix: shown with Custom while the Glass and fence fix runs");
        expect(grey(MR_FXREADER) == (!picCustom || !g_jitterOverscan) && grey(MR_FXWATER) == (!picCustom || !g_jitterOverscan),
               MR_FXREADER, "Lit edge fix / Water block fix: shown with Custom while jitter_overscan is on");
        expect(grey(MR_FXOUTSIDE) == (!picCustom || !g_vrMode || SceneDlaa::Area() >= 100), MR_FXOUTSIDE,
               "Outside the DLAA area: shown with Custom in VR below DLAA area 100 %");
        expect(on(MR_FXPOWER) == (g_powerLineCfg != 0) && on(MR_FXREADER) == (g_prFixCfg != 0) && on(MR_FXLIGHT) == (g_lightRedrawCfg != 0) &&
               on(MR_FXROOF) == (g_lightFsCfg != 0) && on(MR_FXWATER) == (g_waterFixCfg != 0) && on(MR_FXCUTOUT) == (g_cutoutCfg != 0),
               MR_PICFIX, "a phase 112 fix row and the switch its render code reads disagree");
        expect(off(MR_FXOUTSIDE) == (g_outsideAaCfg == 0) && g_outsideAaCfg == SceneDlaa::OutsideAaMode(), MR_FXOUTSIDE,
               "Outside the DLAA area: the menu, outside_aa and SceneDlaa disagree");
        expect(on(MR_POLE) == (g_poleCfg != 0), MR_POLE, "Thin pole fix: the menu and pole_aa disagree");
    }
    {   // phase 104: the value column names what the shader gets (Rcas2.x / .y)
        wchar_t h[16];
        swprintf(h, 16, L"%d %%", SceneDlaa::SharpHalo());
        expect(!wcscmp(val(MR_SHARPHALO), h) && SceneDlaa::SharpHalo() % 5 == 0, MR_SHARPHALO,
               "Halo limit: the menu and SceneDlaa::SharpHalo disagree (or off the 5 % grid after a press)");
        expect(!wcscmp(val(MR_SHARPLUMA), SceneDlaa::SharpLuma() ? L"brightness only" : L"colour"), MR_SHARPLUMA,
               "Sharpen colour: the menu and SceneDlaa::SharpLuma disagree");
    }
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
        for (int r : { MR_MODEL })                      // (phase 112: Light trail fix is a Picture fix; 112c: Sharpen strength is a
                                                        //  Sharpen preset Custom row: checked above)
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
        const int spB = g_sharpPreset;
        g_sharpPreset = SharpPreset::kCustom;              // (112c: the sharpen value rows show with Custom only)
        for (bool& o : g_menuGrpOpen) o = true;
        SceneDlaa::SetSharpness(0.5f);
        g_menuSel = MR_WIDTH;
        MenuBuildPanel(s_panel); MenuBuildPanel(s_panel);
        const bool shown = MenuRowVisible(MR_WIDTH);
        SceneDlaa::SetSharpness(0.0f);
        MenuBuildPanel(s_panel); MenuBuildPanel(s_panel);
        CHECK(!shown || (!MenuRowVisible(MR_WIDTH) && g_menuSel != MR_WIDTH), "Sharpen strength 0: Sharpness size hidden and the selection moved (%s)",
              RowName(g_menuSel));
        // phase 101d (the VR test 2026-11-13): at Sharpen strength 0 the Sharpen type row must still SHOW, flat and VR -- it was
        // hidden, so a player with sharpening off could not find it
        for (int vrm = 0; vrm < 2; ++vrm) {
            g_vrMode = vrm != 0;
            MenuBuildPanel(s_panel); MenuBuildPanel(s_panel);
            CHECK(!MainDlaaRuns() || MenuRowVisible(MR_SHARPTYPE), "%s, Sharpen strength 0: the Sharpen type row is hidden",
                  vrm ? "VR" : "flat");
            // phase 104: the same for the RCAS-only Halo limit / Sharpen colour rows (type RCAS; phase 105b: preset Custom) and the
            // phase 105b Sharpen preset row (RCAS: shown at strength 0 too -- the user's ETS2 dlaa.ini has sharpness 0.0)
            const int tyR = SceneDlaa::SharpType(), spR = g_sharpPreset;
            SceneDlaa::SetSharpType(SceneDlaa::kSharpRcas);
            g_sharpPreset = SharpPreset::kCustom;
            MenuBuildPanel(s_panel); MenuBuildPanel(s_panel);
            CHECK(!MainDlaaRuns() || (MenuRowVisible(MR_SHARPHALO) && MenuRowVisible(MR_SHARPLUMA) && MenuRowVisible(MR_SHARPPRESET)),
                  "%s, Sharpen strength 0, RCAS, preset Custom: the Sharpen preset / Halo limit / Sharpen colour rows are hidden",
                  vrm ? "VR" : "flat");
            // ... a named preset hides all five value rows (112c), the preset row stays
            for (int np = SharpPreset::kOff; np < SharpPreset::kNamed; ++np) {
                g_sharpPreset = np;
                MenuBuildPanel(s_panel); MenuBuildPanel(s_panel);
                CHECK(!MainDlaaRuns() || (!MenuRowVisible(MR_SHARPHALO) && !MenuRowVisible(MR_SHARPLUMA) && !MenuRowVisible(MR_SHARP) &&
                                          !MenuRowVisible(MR_SHARPTYPE) && !MenuRowVisible(MR_WIDTH) && MenuRowVisible(MR_SHARPPRESET)),
                      "%s, preset %s: the five sharpen value rows must hide, Sharpen preset must show", vrm ? "VR" : "flat",
                      SharpPreset::IniName(np));
            }
            // ... and with NIS (Custom) the RCAS-only rows hide; the preset and the type rows stay (112c: the preset sets RCAS)
            g_sharpPreset = SharpPreset::kCustom;
            SceneDlaa::SetSharpType(SceneDlaa::kSharpNis);
            MenuBuildPanel(s_panel); MenuBuildPanel(s_panel);
            CHECK(!MenuRowVisible(MR_SHARPHALO) && !MenuRowVisible(MR_SHARPLUMA) &&
                  (!MainDlaaRuns() || (MenuRowVisible(MR_SHARPPRESET) && MenuRowVisible(MR_SHARPTYPE))),
                  "%s, Sharpen type NIS: Halo limit / Sharpen colour must hide, Sharpen preset / type must show", vrm ? "VR" : "flat");
            // phase 105a: Steady own shadow is shown (hooks in, DLAA on), on and off, flat and VR -- phase 112: with Picture fixes
            // Custom; Standard / Off hide every fix row, the Picture fixes row itself is always shown
            const int slR = g_slUser, ppR = g_picPreset;
            for (int u = 0; u < 2; ++u) {
                g_slUser = u;
                g_picPreset = PicFixes::kCustom;
                MenuBuildPanel(s_panel); MenuBuildPanel(s_panel);
                CHECK(!MainDlaaRuns() || MenuRowVisible(MR_SHADOW), "%s: Steady own shadow (%s) is hidden with Custom", vrm ? "VR" : "flat",
                      u ? "on" : "off");
                CHECK(!MainDlaaRuns() || (MenuRowVisible(MR_PICFIX) && MenuRowVisible(MR_WIREMV) && MenuRowVisible(MR_FXCUTOUT)),
                      "%s, Picture fixes Custom: the preset row / fix rows hidden", vrm ? "VR" : "flat");
                for (int p = PicFixes::kStandard; p <= PicFixes::kOff; ++p) {
                    g_picPreset = p;
                    MenuBuildPanel(s_panel); MenuBuildPanel(s_panel);
                    bool any = false;
                    for (int r = MR_PICFIX + 1; r <= MR_FXCUTOUT; ++r) any = any || MenuRowVisible(r);
                    CHECK((!MainDlaaRuns() || MenuRowVisible(MR_PICFIX)) && !any, "%s, Picture fixes %s: the preset row must show, "
                          "every fix row must hide", vrm ? "VR" : "flat", PicFixes::IniName(p));
                }
            }
            g_slUser = slR; g_picPreset = ppR;
            SceneDlaa::SetSharpType(tyR);
            g_sharpPreset = spR;
            MenuBuildPanel(s_panel); MenuBuildPanel(s_panel);
        }
        g_vrMode = true;
        // phase 101: NIS has no size -- Sharpness size hides while Sharpen type is NIS, comes back with RCAS
        const int ty0 = SceneDlaa::SharpType();
        SceneDlaa::SetSharpness(0.5f);
        SceneDlaa::SetSharpType(SceneDlaa::kSharpNis);
        MenuBuildPanel(s_panel); MenuBuildPanel(s_panel);
        const bool nisHides = !MenuRowVisible(MR_WIDTH);
        SceneDlaa::SetSharpType(SceneDlaa::kSharpRcas);
        MenuBuildPanel(s_panel); MenuBuildPanel(s_panel);
        CHECK(!shown || (nisHides && MenuRowVisible(MR_WIDTH)), "Sharpen type NIS hides Sharpness size, RCAS shows it again");
        g_sharpPreset = spB;
        SceneDlaa::SetSharpType(ty0);
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
    // phase 112: a startup-only switch reads "on (next start)" until the restart, plain "on" after it -- the same choice
    auto plain = [](std::wstring s) { const std::wstring t = L" (next start)"; const size_t p = s.find(t); return p == std::wstring::npos ? s : s.erase(p, t.size()); };
    for (int k = 0; k < MR_COUNT; ++k)
        CHECK(plain(want[k]) == plain(back[k]), "%s: row \"%s\" %s x%d: \"%s\" shows \"%s\" before the restart, \"%s\" after it%s", what, RowName(r),
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

// ---- phase 105b: the Sharpen preset (live rules) + phase 105a: Steady own shadow (live switch) -------------------------------------
static void PresetRules() {
    using namespace SharpPreset;
    // the load rule (sharp_preset.h Resolve): no key = the matching preset, else custom; a named preset fills the absent keys.
    // Phase 112c: + Off, and the presets set sharpen_type (RCAS) and sharp_radius (1.5) too
    bool ap = false;
    auto N = [](float st, int h, bool l, int ty = kTypeRcas, float r = 1.5f) { return Now{ st, h, l, ty, r }; };
    const unsigned S = kHaveStrength, H = kHaveHalo, L = kHaveLuma, T = kHaveType, R = kHaveRadius;
    CHECK(Resolve(-2, 0, N(0.4f, 0, false), &ap) == kSoft && !ap, "no key, defaults -> Soft (a fresh install)");
    CHECK(Resolve(-2, S, N(0.8f, 0, false), &ap) == kBalanced && !ap, "no key, sharpness 0.8 -> Balanced");
    CHECK(Resolve(-2, S | H | L, N(1.2f, 10, true), &ap) == kCrisp, "no key, 1.2 / 10 / brightness -> Crisp");
    CHECK(Resolve(-2, S | H | L, N(1.6f, 20, true), &ap) == kExtra, "no key, 1.6 / 20 / brightness -> Extra crisp");
    CHECK(Resolve(-2, S, N(0.6f, 0, false), &ap) == kCustom, "no key, sharpness 0.6 (the user's ATS) -> Custom");
    CHECK(Resolve(-2, S, N(0.0f, 0, false), &ap) == kOff, "no key, sharpness 0.0 -> Off (112c)");
    CHECK(Resolve(-2, S | T, N(0.0f, 0, false, kTypeNis), &ap) == kOff, "no key, sharpness 0.0 with NIS -> Off (the rest does not run)");
    CHECK(Resolve(kCustom, S | R | T | H | L, N(0.0f, 0, false), &ap) == kCustom,
          "the user's ETS2 (sharpness 0.0, sharp_preset = custom) -> Custom");
    CHECK(Resolve(-2, S | H, N(1.2f, 10, false), &ap) == kCustom, "no key, 1.2 / 10 / colour -> Custom (not quite Crisp)");
    CHECK(Resolve(-2, S | T, N(0.8f, 0, false, kTypeNis), &ap) == kCustom, "no key, Balanced values with NIS -> Custom (112c: type)");
    CHECK(Resolve(-2, S | R, N(0.8f, 0, false, kTypeRcas, 2.5f), &ap) == kCustom, "no key, Balanced values at size 2.5 -> Custom (112c)");
    CHECK(Resolve(-1, S, N(0.8f, 0, false), &ap) == kBalanced, "an unknown sharp_preset is read as no key");
    CHECK(Resolve(kCustom, S, N(0.4f, 0, false), &ap) == kCustom && !ap, "sharp_preset custom stays custom on Soft values");
    CHECK(Resolve(kExtra, 0, N(0.4f, 0, false), &ap) == kExtra && ap, "sharp_preset extra without values -> applied");
    CHECK(Resolve(kCrisp, S, N(1.2f, 0, false), &ap) == kCrisp && ap, "sharp_preset crisp + sharpness 1.2 -> Crisp, rest filled");
    CHECK(Resolve(kCrisp, S, N(0.5f, 0, false), &ap) == kCustom && !ap, "sharp_preset crisp + sharpness 0.5 -> custom (kept)");
    CHECK(Resolve(kCrisp, S | T, N(1.2f, 10, true, kTypeNis), &ap) == kCustom, "sharp_preset crisp + sharpen_type nis -> custom (112c)");
    CHECK(Resolve(kOff, S | T | R, N(0.0f, 30, true, kTypeNis, 3.0f), &ap) == kOff && ap, "sharp_preset off + strength 0 -> Off (the rest kept)");
    CHECK(Parse("extra") == kExtra && Parse(" Extra Crisp ") == kExtra && Parse("BALANCED") == kBalanced && Parse("crisp # c") == kCrisp &&
          Parse("off") == kOff && Parse("nope") == -1, "sharp_preset parsing");
    CHECK(Next(kOff, -1) == kCustom && Next(kCustom, 1) == kOff && Next(kCrisp, 1) == kExtra && Next(kOff, 1) == kSoft,
          "the row wraps both ways (Off first)");
    for (int p = 0; p < kCount; ++p) CHECK(Parse(IniName(p)) == p, "sharp_preset %s reads back", IniName(p));
    for (int p = 0; p < kNamed; ++p)
        CHECK(kValues[p].type == kTypeRcas && kValues[p].radius == 1.5f, "every built-in sharpen preset is RCAS at 1.5 px (%s)", IniName(p));

    // live: a named preset sets ALL the sharpening (112c: + type RCAS, size 1.5); a hand change of any of the five makes it Custom
    const float s0 = SceneDlaa::Sharpness(), r0 = SceneDlaa::SharpRadius();
    const int h0 = SceneDlaa::SharpHalo(), t0 = SceneDlaa::SharpType(), p0 = g_sharpPreset;
    const bool l0 = SceneDlaa::SharpLuma();
    SceneDlaa::SetSharpType(SceneDlaa::kSharpNis);
    SceneDlaa::SetSharpRadius(2.5f);
    g_sharpPreset = kSoft;
    MenuChange(MR_SHARPPRESET, 1, false, 2000);
    MenuChange(MR_SHARPPRESET, 1, false, 2001);
    CHECK(g_sharpPreset == kCrisp && Matches(kCrisp, SharpNow()) && SceneDlaa::SharpRadius() == 1.5f &&
          SceneDlaa::SharpType() == SceneDlaa::kSharpRcas,
          "Soft, Right x2 -> Crisp with ALL its values (type RCAS, size 1.5): %.1f / %d / %d / %s / %.1f", (double)SceneDlaa::Sharpness(),
          SceneDlaa::SharpHalo(), (int)SceneDlaa::SharpLuma(), SceneDlaa::SharpTypeName(), (double)SceneDlaa::SharpRadius());
    MenuChange(MR_SHARP, 1, false, 2002);
    CHECK(g_sharpPreset == kCustom && std::fabs(SceneDlaa::Sharpness() - 1.3f) < 0.01f, "Crisp + Sharpen strength Right -> Custom 1.3");
    g_sharpPreset = kCrisp;
    StepSharpness(-1, 2003);                           // (the Shift+F7 hotkey's function)
    CHECK(g_sharpPreset == kCustom, "a preset + the Sharpen strength hotkey -> Custom");
    g_sharpPreset = kBalanced;
    MenuChange(MR_SHARPHALO, 1, false, 2004);
    CHECK(g_sharpPreset == kCustom, "a preset + Halo limit -> Custom");
    g_sharpPreset = kBalanced;
    MenuChange(MR_SHARPLUMA, 1, false, 2005);
    CHECK(g_sharpPreset == kCustom, "a preset + Sharpen colour -> Custom");
    g_sharpPreset = kBalanced;
    StepSharpRadius(1, 2006);                          // (the Shift+F10 hotkey's function / the Sharpness size row)
    CHECK(g_sharpPreset == kCustom, "a preset + Sharpness size -> Custom (112c)");
    g_sharpPreset = kBalanced;
    MenuChange(MR_SHARPTYPE, 1, false, 2007);
    CHECK(g_sharpPreset == kCustom && SceneDlaa::SharpType() == SceneDlaa::kSharpNis, "a preset + Sharpen type -> Custom, NIS (112c)");
    MenuChange(MR_SHARPTYPE, 1, false, 2008);
    SceneDlaa::SetSharpness(0.7f); SceneDlaa::SetSharpHalo(15); SceneDlaa::SetSharpLuma(true);
    g_sharpPreset = kCustom;
    MenuChange(MR_SHARPPRESET, -1, false, 2009);       // Custom, Left -> Extra crisp
    MenuChange(MR_SHARPPRESET, 1, false, 2010);        // Extra crisp, Right -> Custom: phase 112r -- the KEPT Custom set comes back
    CHECK(g_sharpPreset == kCustom && std::fabs(SceneDlaa::Sharpness() - 0.7f) < 0.01f && SceneDlaa::SharpHalo() == 15 &&
          SceneDlaa::SharpLuma(), "Custom -> Extra crisp -> Custom: the kept Custom values back (112r; 112c kept Extra crisp's)");
    MenuChange(MR_SHARPPRESET, 1, false, 2011);        // Custom, Right -> Off: strength 0, the rest kept
    CHECK(g_sharpPreset == kOff && SceneDlaa::Sharpness() == 0.0f && SceneDlaa::SharpHalo() == 15, "Custom, Right -> Off: strength 0 (112c)");
    g_sharpCustomHave = false;
    g_sharpPreset = kSoft;
    SharpPresetApply(kSoft);
    StepSharpness(-1, 2012);                           // 0.4 -> 0.3
    for (int i = 0; i < 30; ++i) StepSharpness(-1, 2013);   // down to 0, then at the limit: no change, no beep path
    CHECK(SceneDlaa::Sharpness() == 0.0f && g_sharpPreset == kCustom, "strength 0 by hand -> Custom");
    SceneDlaa::SetSharpness(s0); SceneDlaa::SetSharpRadius(r0); SceneDlaa::SetSharpHalo(h0); SceneDlaa::SetSharpLuma(l0);
    SceneDlaa::SetSharpType(t0); g_sharpPreset = p0;

    // phase 105a: the live switch from a start with shadow_lock = 0 AND shadow_lock_diag = 0 (no learning yet) -- the row turns the
    // learning on and the lock reaches the render code at the next shadow slice set
    const int cfg0 = g_slCfg, user0 = g_slUser, want0 = g_slWant;
    const bool on0 = g_slOn, hooks0 = g_slDrawHooksOk;
    g_slCfg = 0; g_slUser = 0; g_slWant = -1; g_slOn = false; g_slDrawHooksOk = false;
    MenuChange(MR_SHADOW, 1, false, 2100);
    CHECK(g_slCfg == 0 && g_slWant < 0 && !g_slOn, "Steady own shadow without its draw hooks: nothing switches");
    g_slDrawHooksOk = true;
    MenuChange(MR_SHADOW, 1, false, 2101);
    CHECK(g_slUser == 1 && g_slWant == 1 && g_slOn, "Steady own shadow Right from off (diag 0): on, learning started, switch pending");
    SlOnContextReset();
    SlOnBind(0, 2048);                                 // the next cascade slice bind = a new slice set: the switch applies
    SlOnBind(-1, 0);
    CHECK(g_slCfg == 1 && g_slWant < 0, "the pending switch reaches the render code at the next slice set");
    MenuChange(MR_SHADOW, -1, false, 2102);
    SlOnContextReset();
    SlOnBind(0, 2048);
    SlOnBind(-1, 0);
    CHECK(g_slUser == 0 && g_slCfg == 0 && g_slOn, "Steady own shadow Left: off (the diagnostics keep running)");
    ToggleShadowLock(2103);                            // the key: the same path, the same choice
    CHECK(g_slUser == 1 && g_slWant == 1, "the key_shadow_lock key switches the same choice");
    SlOnContextReset();
    g_slCfg = cfg0; g_slUser = user0; g_slWant = want0; g_slOn = on0; g_slDrawHooksOk = hooks0;
    // phase 109r: the row needs did_shadow's VSSetShader hook and (once known) the NO_OVERWRITE constant-buffer map -- hidden otherwise
    {
        const bool ds0 = g_didShadowOn, hk0 = g_slDrawHooksOk;
        const int no0 = g_slNoOvw;
        g_slDrawHooksOk = true; g_didShadowOn = true; g_slNoOvw = -1;
        CHECK(ShadowLockUsable(), "Steady own shadow usable: draw hooks + did_shadow, NO_OVERWRITE not known yet");
        g_slNoOvw = 0;
        CHECK(!ShadowLockUsable(), "Steady own shadow NOT usable without the NO_OVERWRITE map (row hidden)");
        g_slNoOvw = 1; g_didShadowOn = false;
        CHECK(!ShadowLockUsable(), "Steady own shadow NOT usable without did_shadow's VSSetShader hook (row hidden)");
        g_didShadowOn = ds0; g_slDrawHooksOk = hk0; g_slNoOvw = no0;
    }
}

// ---- phase 105b / 105a: what a start makes of a dlaa.ini (child processes: LoadConfig runs once per process) ----------------------
static bool WriteIni(const char* text) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, IniPath().c_str(), L"wb") != 0 || !f) return false;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    return true;
}
static std::string ReadIni() {
    std::string s;
    FILE* f = nullptr;
    if (_wfopen_s(&f, IniPath().c_str(), L"rb") == 0 && f) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n); fclose(f); }
    return s;
}
static void IniRules() {
    struct C { const char* ini; const wchar_t* preset; const wchar_t* strength; const wchar_t* halo; const wchar_t* colour; };
    static const C kC[] = {
        { "beeps = 0\n",                                              L"Soft",        L"0.4", L"0 %",  L"colour" },
        { "beeps = 0\nsharpness = 0.8\n",                             L"Balanced",    L"0.8", L"0 %",  L"colour" },
        { "beeps = 0\nsharpness = 1.2\nsharp_halo = 10\nsharp_luma = 1\n", L"Crisp",  L"1.2", L"10 %", L"brightness only" },
        { "beeps = 0\nsharpness = 0.6\n",                             L"Custom",      L"0.6", L"0 %",  L"colour" },   // the user's ATS
        { "beeps = 0\nsharpness = 0.0\nsharpen_type = rcas\n",        L"Off",         L"0.0 (off)", L"0 %", L"colour" },   // 112c: Off
        // the user's ETS2 dlaa.ini (2026-11-15): sharp_preset = custom, sharpness 0.0, rcas, 1.5, halo 0, luma 0 -> Custom kept
        { "beeps = 0\nsharpness = 0.0\nsharp_radius = 1.5\nsharpen_type = rcas\nsharp_halo = 0\nsharp_luma = 0\nsharp_preset = custom\n",
                                                                      L"Custom",      L"0.0 (off)", L"0 %", L"colour" },
        { "beeps = 0\nsharpness = 0.8\nsharpen_type = nis\n",          L"Custom",      L"0.8", L"0 %",  L"colour" },   // 112c: NIS = Custom
        { "beeps = 0\nsharp_preset = balanced\nsharp_radius = 2.5\n",   L"Custom",      L"0.4", L"0 %",  L"colour" },   // 112c: size differs
                                                                      //   (custom: the file's values, the absent keys their defaults)
        { "beeps = 0\nsharp_preset = extra\n",                        L"Extra crisp", L"1.6", L"20 %", L"brightness only" },
        { "beeps = 0\nsharp_preset = crisp\nsharpness = 0.5\n",       L"Custom",      L"0.5", L"0 %",  L"colour" },
        { "beeps = 0\nsharp_preset = custom\nsharpness = 0.4\n",      L"Custom",      L"0.4", L"0 %",  L"colour" },
    };
    auto strip = [](std::wstring s) { const std::wstring g = L"(greyed) "; return s.compare(0, g.size(), g) ? s : s.substr(g.size()); };
    for (const C& c : kC) {
        for (int vr = 0; vr < 2; ++vr) {
            WriteIni(c.ini);
            const std::vector<std::wstring> p = Child(vr ? L"--show --vr" : L"--show");
            if (p.size() != (size_t)MR_COUNT) { CHECK(false, "ini case child failed: %s", c.ini); continue; }
            const bool ok = strip(p[MR_SHARPPRESET]) == c.preset && strip(p[MR_SHARP]) == c.strength &&
                            strip(p[MR_SHARPHALO]) == c.halo && strip(p[MR_SHARPLUMA]) == c.colour;
            CHECK(ok, "%s: dlaa.ini \"%s\" -> preset \"%s\" strength \"%s\" halo \"%s\" colour \"%s\" (want %ls / %ls / %ls / %ls)",
                  vr ? "VR" : "flat", c.ini, U8(p[MR_SHARPPRESET]).c_str(), U8(p[MR_SHARP]).c_str(), U8(p[MR_SHARPHALO]).c_str(),
                  U8(p[MR_SHARPLUMA]).c_str(), c.preset, c.strength, c.halo, c.colour);
            // the preset row is shown (RCAS, DLAA on) -- also at strength 0
            CHECK(p[MR_SHARPPRESET].compare(0, 9, L"(greyed) ") != 0, "%s: dlaa.ini \"%s\": the Sharpen preset row is hidden",
                  vr ? "VR" : "flat", c.ini);
        }
    }
    // an old dlaa.ini (no sharp_preset) keeps its values through a Save, which now names the preset
    WriteIni("beeps = 0\nsharpness = 0.6\n");
    RunChild(L"--row " + std::to_wstring((int)MR_SAVE) + L" --dir 1");
    const std::string saved = ReadIni();
    CHECK(saved.find("sharpness = 0.6") != std::string::npos && saved.find("sharp_preset = custom") != std::string::npos,
          "an old dlaa.ini with sharpness 0.6: Save keeps 0.6 and writes sharp_preset = custom");
    // phase 105a: shadow_lock -- default on, 0 = off; Save writes it
    WriteIni("beeps = 0\n");
    std::vector<std::wstring> p = Child(L"--show");
    CHECK(p.size() == (size_t)MR_COUNT && strip(p[MR_SHADOW]) == L"on", "no shadow_lock key: Steady own shadow is on (the default)");
    WriteIni("beeps = 0\nshadow_lock = 0\nshadow_lock_diag = 0\n");
    p = Child(L"--show --vr");
    CHECK(p.size() == (size_t)MR_COUNT && p[MR_SHADOW] == L"off", "shadow_lock = 0, diag 0 (VR): Steady own shadow shows off (not hidden)");
    RunChild(L"--row " + std::to_wstring((int)MR_SHADOW) + L" --dir 1 --vr");
    CHECK(ReadIni().find("shadow_lock = 1") != std::string::npos, "Steady own shadow Right + Save writes shadow_lock = 1");
    WriteBaseIni();
}

// phase 110: the census switched LIVE (row / key, in this process), hidden when it cannot work, the bug report's census part
static void CensusLiveRules() {
    const bool ready0 = g_censusReady;
    const int cfg0 = g_censusCfg;
    // not usable (no PS hooks): the row is hidden (greyed flag = hidden without menu_show_all) and nothing switches
    g_censusReady = false; g_censusCfg = 0; g_censusOn = false; CensusRun::g_view = false;
    MenuChange(MR_CENSUS, 1, false, 2300);
    CHECK(g_censusCfg == 0 && !g_censusOn && !CensusViewOn(), "Show unfixed shaders without the PS hooks: nothing switches");
    BuildPanel();
    CHECK(s_panel.rows[MR_CENSUS].greyed != 0, "Show unfixed shaders is hidden without the PS hooks");
    CensusToggleView(2301);
    CHECK(!g_censusOn, "the census key without the PS hooks: nothing switches");
    // usable: Right on (live, no restart), Left off; the key switches the same state
    g_censusReady = true;
    MenuChange(MR_CENSUS, 1, false, 2302);
    BuildPanel();
    CHECK(g_censusCfg == 1 && g_censusOn && CensusViewOn() && CensusRun::g_codeAll.load(), "Show unfixed shaders Right: census + view on, live");
    CHECK(!wcscmp(s_panel.rows[MR_CENSUS].value, L"on") && !s_panel.rows[MR_CENSUS].greyed, "the row shows on (shown)");
    MenuChange(MR_CENSUS, 1, false, 2303);
    CHECK(g_censusCfg == 1 && g_censusOn, "Right again keeps it on");
    MenuChange(MR_CENSUS, -1, false, 2304);
    BuildPanel();
    CHECK(g_censusCfg == 0 && !g_censusOn && !CensusViewOn() && !wcscmp(s_panel.rows[MR_CENSUS].value, L"off"), "Left: census + view off");
    CensusToggleView(2305);
    CHECK(g_censusCfg == 1 && CensusViewOn(), "key_census_view: on (the row's state)");
    BuildPanel();
    CHECK(!wcscmp(s_panel.rows[MR_CENSUS].value, L"on"), "the row follows the key");
    // nothing tints while off: the tint decision needs the view (and is held while a capture runs its sets)
    CensusToggleView(2306);
    CHECK(!CensusRun::g_view.load() && !CensusViewOn(), "key again: view off (no tint)");
    // phase 112b: the colour legend box -- on every switch-on (key or row) for kLegendSecs, again while the menu sits on the row,
    // never while the view is off; the footer names the player's OWN capture / census keys (or the menu rows when unbound)
    {
        TuningMenu::Legend lg;
        CHECK(!CensusLegendNow(lg), "legend: view off = no legend");
        const KeyBind kc0 = g_keys[KA_SHIMMER_CAP], kv0 = g_keys[KA_CENSUS_VIEW];
        g_keys[KA_SHIMMER_CAP].vk = VK_INSERT; g_keys[KA_SHIMMER_CAP].mods = 0; g_keys[KA_SHIMMER_CAP].joy = 0;
        g_keys[KA_CENSUS_VIEW].vk = VK_DELETE; g_keys[KA_CENSUS_VIEW].mods = 0; g_keys[KA_CENSUS_VIEW].joy = 0;
        wchar_t capN[96], viewN[96];
        MultiByteToWideChar(CP_UTF8, 0, KeyNameUi(KA_SHIMMER_CAP), -1, capN, 96);
        MultiByteToWideChar(CP_UTF8, 0, KeyNameUi(KA_CENSUS_VIEW), -1, viewN, 96);
        CensusToggleView(2307);
        CHECK(CensusViewOn() && CensusLegendNow(lg), "legend: shows when the key switches the view on");
        // phase 113: plain words -- the title, three lines, a small colour key (thin / see-through / pixel / water / new)
        CHECK(!wcscmp(lg.title, L"Census: flashing colours = no fix in the mod yet") && lg.n == 5, "legend: title + 5 key entries (got %d)", lg.n);
        CHECK(!wcscmp(lg.line[0], L"Each colour is a different kind of problem (thin, see-through, water, ...). You do not need to know which."),
              "legend line 1: \"%ls\"", lg.line[0]);
        CHECK(!wcscmp(lg.word[0], L"thin") && !wcscmp(lg.word[1], L"see-through") && !wcscmp(lg.word[2], L"pixel") &&
              !wcscmp(lg.word[3], L"water") && !wcscmp(lg.word[4], L"new"), "legend: the key words in the asked order");
        // phase 115: the screen colours of the paint (orange = 255, 112, 0: the old 255, 115, 0 legend swatch showed as gold in the game)
        CHECK(lg.rgb[0] == RGB(0, 255, 255) && lg.rgb[1] == RGB(255, 112, 0) && lg.rgb[2] == RGB(255, 0, 255) &&
              lg.rgb[3] == RGB(32, 64, 255) && lg.rgb[4] == RGB(255, 255, 0), "legend: swatch colours = the paint's screen colours (Census::KindColour)");
        std::wstring want0 = std::wstring(L"Something coloured? Press ") + capN + L" twice. That saves it so it can be fixed.";
        std::wstring want1 = std::wstring(L"Saved objects stop flashing until the next game start. Colours off: ") + viewN + L".";
        CHECK(want0 == lg.line[1] && want1 == lg.line[2], "legend lines follow the bindings: \"%ls\" / \"%ls\"", lg.line[1], lg.line[2]);
        CHECK(!wcscmp(lg.line[3], L"The bug icon counts what is left to capture here."), "phase 115: the bug icon line (\"%ls\")", lg.line[3]);
        CHECK(!lg.line[4][0], "legend: no \"fix is switched off\" line while no object is hidden for that reason");
        {   // phase 116: objects NOT coloured because a fix is OFF (the bug icon's count > 0) -> the extra line
            const int off0 = CensusRun::g_bugFixOff.load();
            const wchar_t* kFixOff = L"Objects with a fix that is switched off are not coloured: see Picture preset > Custom.";
            CensusRun::g_bugFixOff = 2;
            TuningMenu::Legend lf;
            CHECK(CensusLegendNow(lf) && !wcscmp(lf.line[4], kFixOff),
                  "legend: the fix-off line while the bug icon counts such objects (\"%ls\")", lf.line[4]);
            // phase 116 (audit S2): the line is LATCHED -- the count blinks to 0 and back as the player drives, but the line is held
            // for kLegendSecs after it last read > 0, so it does not flicker
            CensusRun::g_bugFixOff = 0;
            CHECK(CensusLegendNow(lf) && !wcscmp(lf.line[4], kFixOff),
                  "legend: the fix-off line is held after the count drops to 0 (latched, no flicker)");
            // the latch clears only when the census view is switched off; back on with the count at 0 = no line
            CensusRun::g_view = false;
            CHECK(!CensusViewOn() && !CensusLegendNow(lf), "legend: the view off clears the fix-off latch");
            CensusRun::g_view = true;
            CHECK(CensusViewOn() && CensusLegendNow(lf) && !lf.line[4][0],
                  "legend: no fix-off line after the view went off and back with the count at 0 (latch reset)");
            CensusRun::g_bugFixOff = off0;
        }
        const int capState0 = Cap::g_state.load();
        Cap::g_state = 0;                                 // (an earlier test's "Bug report done / FAILED" line would show)
        g_picNoteUntil = 0;                               // (and an earlier Picture preset note)
        CaptureAtPresent(2308);
        CHECK(g_censusLegendOn && g_capStatusOn && !g_capL1[0] && !g_capL2[0] && !memcmp(&g_censusLegend, &lg, sizeof(lg)),
              "CaptureAtPresent: legend on, no status lines, the box text published (on %d status %d \"%ls\")", (int)g_censusLegendOn,
              (int)g_capStatusOn, g_capL1);
        Cap::g_state = 1;
        CaptureAtPresent(2308);
        CHECK(!g_censusLegendOn, "no legend while a capture runs (the status box has the word)");
        Cap::g_state = 0;
        CaptureAtPresent(2308);
        // a mark note (status box) does not cut the legend short any more
        snprintf(CensusRun::g_legendMsg, sizeof(CensusRun::g_legendMsg), "MARK 1 saved: 3 suspects on screen");
        CensusRun::g_markNoteUntil = CensusRun::NowSec() + 4.0;
        wchar_t l1[64], l2[64];
        CHECK(CensusStatus(l1, l2, 64) && wcsstr(l1, L"MARK 1") && CensusLegendNow(lg), "mark note in the status box + the legend stays");
        CensusRun::g_markNoteUntil = -1.0;
        CHECK(!CensusStatus(l1, l2, 64), "the old two-line legend is gone from the status box");
        CensusRun::g_legendUntil = CensusRun::NowSec() - 1.0;   // (15 s later)
        CHECK(!CensusLegendNow(lg), "legend: gone after its time");
        CaptureAtPresent(2309);
        CHECK(!g_censusLegendOn && g_capStatusOn == (g_censusBugOn && !g_vrMode), "CaptureAtPresent: legend off again (phase 115: the bug "
              "icon keeps the overlay flag on while the view is on, flat)");
        // phase 114: a double-Insert capture that FINISHES (running / writing -> done) shows the legend again for kLegendSecs
        Cap::g_state = 2;
        CaptureAtPresent(2309);
        CHECK(!g_censusLegendOn, "phase 114: no legend while the capture writes");
        Cap::g_state = 3;
        CaptureAtPresent(2309);
        CHECK(CensusLegendNow(lg) && g_censusLegendOn, "phase 114: the legend again once the capture finished (view on)");
        CensusRun::g_legendUntil = CensusRun::NowSec() - 1.0;
        Cap::g_state = 0;
        CaptureAtPresent(2309);
        CHECK(!g_censusLegendOn, "phase 114: ... for its 15 s only (no capture finished since)");
        Cap::g_state = capState0;
        {   // phase 114: the VR stereo placement (the user's ATS clips: menu open, menu_vr_x -18, Quest 3 centres u 0.379 / 0.621)
            float rw = 0.0f, rh = 0.0f;
            CHECK(TuningMenu::LegendRefSize(lg, &rw, &rh) && rw > 100.0f, "legend: measured (%.0f x %.0f ref px)", rw, rh);
            const float W = 3448.0f;                      // the ATS VR eye picture width of the evening log
            float ls = W / 1920.0f * 1.25f, bw = rw * ls;
            if (bw > 0.8f * W) bw = 0.8f * W;
            const float uPx[2] = { 0.379f * W, 0.621f * W };   // eye 0 = the RIGHT eye (nasal side: u < 0.5)
            const float in[2] = { -1.0f, 1.0f };
            const float want = -18.0f / 100.0f * W;
            // OLD: menu_vr_depth 12 px each, per-eye clamp in DrawLegend -> the offset from each optical centre differs
            const float shOld[2] = { 12.0f * in[0], 12.0f * in[1] };
            float off[2];
            for (int e = 0; e < 2; ++e) off[e] = LegendVr::DrawnCentre(uPx[e] + want + shOld[e], bw, W) - uPx[e];
            const float dispOld = (off[1] - off[0]) - (shOld[1] - shOld[0]);   // the error vs the intended crossed disparity
            // NEW: one dx for both eyes, depth of a point at 0.8 m (ipd 63 mm, fx = W / 2 x |P00| 1.0)
            const float d = LegendVr::DepthShiftPx(0.063, 0.5 * W, 0.8);
            const float shNew[2] = { d * in[0], d * in[1] };
            const float dx = LegendVr::SharedDx(want, bw, W, uPx, shNew);
            float offN[2];
            for (int e = 0; e < 2; ++e) offN[e] = LegendVr::DrawnCentre(uPx[e] + dx + shNew[e], bw, W) - uPx[e];
            const float dispNew = (offN[1] - offN[0]) - (shNew[1] - shNew[0]);
            std::printf("   phase 114 VR legend (box %.0f px in a %.0f px eye): OLD disparity error %+.1f px (one eye clamped), NEW %+.2f px; "
                        "intended depth 0.8 m = %.1f px inward per eye (menu_vr_depth was 12)\n", bw, W, dispOld, dispNew, d);
            CHECK(std::fabs(dispOld) > 10.0f, "the old placement double-images the legend (%.1f px error) -- the bug reproduced", dispOld);
            CHECK(std::fabs(dispNew) < 0.01f, "the new placement: the two eyes differ by the depth shift only (%.3f px)", dispNew);
            for (int e = 0; e < 2; ++e) {
                const float c = uPx[e] + dx + shNew[e];
                CHECK(c - 0.5f * bw >= -0.01f && c + 0.5f * bw <= W + 0.01f, "the box fits eye %d's picture (%.1f .. %.1f)", e, c - 0.5f * bw,
                      c + 0.5f * bw);
            }
            CHECK(d > 30.0f && d < 80.0f, "0.8 m at a 1724 px focal length: %.1f px inward per eye", d);
            const float dc = LegendVr::SharedDx(0.0f, bw, W, uPx, shNew);
            CHECK(std::fabs(dc) < 0.01f, "menu closed (want 0): centred on the optical centres, no clamp needed (%.2f)", dc);
            CHECK(std::fabs(LegendVr::SharedDx(0.0f, 2.0f * W, W, uPx, shNew) - LegendVr::SharedDx(5.0f, 2.0f * W, W, uPx, shNew)) < 0.01f,
                  "a box wider than both pictures: one fixed dx (the middle), the same in both eyes");
        }
        const bool open0 = g_menuOpen.load();
        const int sel0 = g_menuSel, page0 = g_menuPage;
        g_menuOpen = true; g_menuSel = MR_CENSUS; g_menuPage = kMenuPageMain;
        CHECK(CensusLegendNow(lg), "legend: shown again while the menu is on the Show unfixed shaders row");
        g_menuSel = MR_CAPKEY;
        CHECK(!CensusLegendNow(lg), "legend: not on another row");
        g_menuOpen = open0; g_menuSel = sel0; g_menuPage = page0;
        // re-binding: the next switch-on shows the new names; unbound = the menu rows
        g_keys[KA_SHIMMER_CAP].vk = 0; g_keys[KA_CENSUS_VIEW].vk = 0;
        MenuChange(MR_CENSUS, -1, false, 2310);
        MenuChange(MR_CENSUS, 1, false, 2311);
        CHECK(CensusLegendNow(lg) && wcsstr(lg.line[1], L"Capture now") && wcsstr(lg.line[2], L"Show unfixed shaders"),
              "legend: the row switches it on too; unbound keys name the menu rows (\"%ls\" / \"%ls\")", lg.line[1], lg.line[2]);
        g_keys[KA_SHIMMER_CAP] = kc0; g_keys[KA_CENSUS_VIEW] = kv0;
        CensusToggleView(2312);
        CHECK(!CensusViewOn() && !CensusLegendNow(lg), "legend: off with the view");
    }
    // phase 115: the BUG ICON -- only while the census view is on. Flat: its own box (g_censusBugOn arms the Present overlay; no strip
    // in the fps box). VR: a strip of the fps box's own texture (every fps box placement) -- or, with the fps box off, its own box in
    // the eye pictures (g_capStatusOn arms them). The lines carry the counts ("3 to capture" / "5 captured").
    {
        const bool vr0 = g_vrMode, fps0 = g_fpsShow;
        const int capState0 = Cap::g_state.load();
        Cap::g_state = 0;
        const int to0 = CensusRun::g_bugToCapture.load(), cap0 = CensusRun::g_bugCaptured.load(), off0 = CensusRun::g_bugFixOff.load();
        CensusRun::g_bugToCapture = 3; CensusRun::g_bugCaptured = 5; CensusRun::g_bugFixOff = 0;
        g_picNoteUntil = 0;
        for (int vr = 0; vr < 2; ++vr) {
            g_vrMode = vr != 0;
            g_fpsShow = true;
            if (CensusViewOn()) CensusToggleView(2320);
            CaptureAtPresent(2320);
            CHECK(!g_censusBugOn && !g_bugL1[0] && !TuningMenu::WidgetBugOn() && !g_capStatusOn,
                  "%s: view off = no bug icon (box %d, strip %d, overlay %d)", vr ? "VR" : "flat", (int)g_censusBugOn,
                  (int)TuningMenu::WidgetBugOn(), (int)g_capStatusOn);
            CensusToggleView(2321);
            CensusRun::g_legendUntil = CensusRun::NowSec() - 1.0;   // (the switch-on legend aside: the icon alone)
            CaptureAtPresent(2321);
            CHECK(g_censusBugOn && !wcscmp(g_bugL1, L"3 to capture") && !wcscmp(g_bugL2, L"5 captured") && !g_bugL3[0],
                  "%s: view on = the bug icon with its counts (\"%ls\" / \"%ls\"; no fix-off line at 0)", vr ? "VR" : "flat", g_bugL1, g_bugL2);
            {   // phase 116: the third line "N: fix is off" while objects are hidden because a fix is off (flat box and VR strip alike)
                CensusRun::g_bugFixOff = 4;
                CaptureAtPresent(2321);
                CHECK(!wcscmp(g_bugL3, L"4: fix is off") && g_censusBugOn, "%s: the fix-off line (\"%ls\")", vr ? "VR" : "flat", g_bugL3);
                if (vr) {
                    const float hb = TuningMenu::WidgetRefH(g_fps.line1, g_fps.line2);
                    CHECK(TuningMenu::WidgetBugOn() && hb <= TuningMenu::kWidgetRefHMax + 0.01f,
                          "VR: the strip keeps its size with the third line (%.0f ref px)", hb);
                }
                CensusRun::g_bugFixOff = 0;
                CaptureAtPresent(2321);
                CHECK(!g_bugL3[0], "%s: the fix-off line goes at 0", vr ? "VR" : "flat");
            }
            if (!vr) CHECK(g_capStatusOn && !TuningMenu::WidgetBugOn(), "flat: its own box at Present (overlay on), not in the fps box");
            else {
                const float h1 = TuningMenu::WidgetRefH(g_fps.line1, g_fps.line2);
                CHECK(TuningMenu::WidgetBugOn() && !g_capStatusOn,
                      "VR + fps box: a strip of the fps box (the box's own placement / layer / depth), no extra eye overlay");
                TuningMenu::SetWidgetBug(nullptr, nullptr);
                const float h0 = TuningMenu::WidgetRefH(g_fps.line1, g_fps.line2);
                CHECK(std::fabs(h1 - h0 - TuningMenu::kWidgetBugH) < 0.01f && h1 <= TuningMenu::kWidgetRefHMax + 0.01f,
                      "VR: the strip makes the fps box %.0f ref px taller (%.0f -> %.0f; the quad image holds %.0f)", h1 - h0, h0, h1,
                      TuningMenu::kWidgetRefHMax);
                CaptureAtPresent(2321);
                g_fpsShow = false;
                CaptureAtPresent(2322);
                CHECK(g_capStatusOn && g_censusBugOn, "VR, fps box off: its own box in the eye pictures (overlay armed)");
                g_fpsShow = true;
            }
            CensusToggleView(2323);
            CaptureAtPresent(2323);
            CHECK(!g_censusBugOn && !TuningMenu::WidgetBugOn(), "%s: off again with the view", vr ? "VR" : "flat");
        }
        // the flat corner: away from the fps box, under an open menu panel that covers it
        {
            float x = 0.0f, y = 0.0f;
            const float none[4] = { 0, 0, 0, 0 }, panelTR[4] = { 1500, 10, 1900, 900 };
            FlatBugPlace(1920, 1080, 160, 42, 16, true, 60, none, &x, &y);
            CHECK(x == 1920 - 160 - 16 && y == 16, "flat: fps box left -> top-right corner (%.0f, %.0f)", x, y);
            FlatBugPlace(1920, 1080, 160, 42, 16, true, 1800, none, &x, &y);
            CHECK(x == 16 && y == 16, "flat: fps box right -> top-left corner (%.0f, %.0f)", x, y);
            FlatBugPlace(1920, 1080, 160, 42, 16, false, 0, panelTR, &x, &y);
            CHECK(x == 1920 - 160 - 16 && y == 916, "flat: under the open menu panel that covers the corner (%.0f, %.0f)", x, y);
        }
        CensusRun::g_bugToCapture = to0; CensusRun::g_bugCaptured = cap0; CensusRun::g_bugFixOff = off0;
        g_vrMode = vr0; g_fpsShow = fps0;
        Cap::g_state = capState0;
        CaptureAtPresent(2324);
    }
    // the bug report's census part: view OFF = nothing (the capture is exactly as before)
    CensusRun::g_cap = CensusRun::CapRun{};
    CensusOnCapture(2310, L"shimmer_20261115_000000\\");
    CHECK(!CensusRun::g_cap.active && !CensusCaptureBusy(2310), "capture with the view off: no census part");
    // view ON: a MARK + the armed frames + the folder inside the capture
    CensusSetLive(true, 2311, "test");
    const size_t marks0 = CensusRun::g_marks.size();
    CensusOnCapture(2312, L"shimmer_20261115_000000\\");
    const CensusRun::CapRun& c = CensusRun::g_cap;
    const std::wstring tail = L"dlaa_selftest\\shimmer_20261115_000000\\census\\";
    CHECK(c.active && c.armFrom == 2313 && c.armTo == 2312 + CensusRun::kCapArmFrames && c.stage == 0, "capture with the view on: armed "
          "Presents #2313..#%llu (got %d %llu..%llu)", (unsigned long long)(2312 + CensusRun::kCapArmFrames), (int)c.active,
          (unsigned long long)c.armFrom, (unsigned long long)c.armTo);
    CHECK(c.dir.size() > tail.size() && c.dir.compare(c.dir.size() - tail.size(), tail.size(), tail) == 0 &&
          c.rel == "dlaa_selftest\\shimmer_20261115_000000\\census\\", "the census part goes inside the capture's folder");
    CHECK(CensusRun::g_marks.size() == marks0 + 1 && c.mark == (int)CensusRun::g_marks.size() &&
          CensusRun::g_marks.back().pic == "dlaa_selftest\\shimmer_20261115_000000\\census\\tinted.png", "a MARK with the capture's tinted.png");
    CHECK(CensusCaptureBusy(2400), "the bug report zip waits for the census part");
    CHECK(!CensusCaptureBusy(2312 + CensusRun::kCapDeadline + 301), "... but not past its deadline");
    CensusOnCapture(2320, L"shimmer_20261115_000001\\");
    CHECK(CensusRun::g_cap.no == c.no && CensusRun::g_cap.key == 2312, "a second capture while the first census part runs: left alone");
    // the capture's own frames are untinted
    const bool tr0 = g_testRunning, ts0 = g_testShimmer;
    g_testRunning = true; g_testShimmer = true;
    CHECK(CensusRun::TintHeld(), "the capture's sets hold the tint");
    g_testRunning = false;
    CHECK(!CensusRun::TintHeld(), "no capture: the tint is not held");
    g_testRunning = tr0; g_testShimmer = ts0;
    // CaptureRequest (the Capture now row / the key's second press) names the folder at the key and starts the census part
    CensusRun::g_cap = CensusRun::CapRun{};
    CaptureRequest(2330, true, "test");
    CHECK(!wcsncmp(Cap::g_nextSub, L"shimmer_", 8) && CensusRun::g_cap.active &&
          CensusRun::g_cap.rel == std::string("dlaa_selftest\\") + U8(Cap::g_nextSub) + "census\\",
          "CaptureRequest with the view on: the folder name is fixed at the key and the census part writes into it");
    g_selfTestRequest = false; g_selfTestShimmer = false; Cap::g_fromMenu = false; Cap::g_nextSub[0] = 0;
    CensusRun::g_cap = CensusRun::CapRun{};
    CensusRun::g_picReq.clear();
    for (int i = 0; i < 200 && CensusRun::g_wPending.load() > 0; ++i) Sleep(10);   // (the marks' file jobs) then remove what they made
    {
        wchar_t p[MAX_PATH];
        if (PathNextToDll(L"dlaa_selftest", p)) BugReport::DeleteTree(p);
        if (PathNextToDll(L"dlaa_census.txt", p)) DeleteFileW(p);
    }
    // phase 111 (VR): the row is SHOWN in the VR panel (menu_show_all off, both modes), the key switches it in VR, the tint cap is per
    // world pass (half the per-Present cap in VR: both eye passes get the same budget), and VR pictures wait for eye 0's blit
    {
        const bool vr0 = g_vrMode, all0 = g_menuShowAll;
        g_menuShowAll = false;
        for (int vrm = 0; vrm < 2; ++vrm) {
            g_vrMode = vrm != 0;
            g_censusReady = true;
            if (g_censusOn) CensusToggleView(2500);
            CensusToggleView(2501);
            BuildPanel();
            CHECK(g_censusOn && CensusViewOn() && MenuRowVisible(MR_CENSUS), "%s: key_census_view switches the census on and the row is shown",
                  vrm ? "VR" : "flat");
            CHECK(CensusRun::TintCapPerPass() == (vrm ? CensusRun::kTintMaxPerFrame / 2 : CensusRun::kTintMaxPerFrame),
                  "%s: tint cap per world pass %d", vrm ? "VR" : "flat", CensusRun::TintCapPerPass());
            CensusToggleView(2502);
            CHECK(!g_censusOn && !CensusViewOn(), "%s: the key switches it off again", vrm ? "VR" : "flat");
        }
        // VR pictures: a due request is NOT served from the back buffer for 3 Presents (eye 0's blit takes it); no device here, so
        // only the queue rule is checked
        g_vrMode = true;
        CensusRun::g_picReq.clear();
        CensusRun::g_picReq.push_back({ L"x.png", 2600 });
        CensusRun::g_picEyeBad = false;
        CensusRun::PicAtPresent(nullptr, nullptr, 2601);     // (no swap chain: present-layer; due + 1 < due + 3: kept for the eye)
        CHECK(CensusRun::g_picReq.size() == 1, "VR: a due picture waits for eye 0's blit (not dropped at Present)");
        CensusRun::PicAtPresent(nullptr, nullptr, 2603);     // 3 Presents late and still no blit, no back buffer: dropped
        CHECK(CensusRun::g_picReq.empty(), "VR: a picture no eye blit served within 3 Presents is dropped without a back buffer");
        g_vrMode = false;
        CensusRun::g_picReq.push_back({ L"x.png", 2600 });
        CensusRun::PicAtPresent(nullptr, nullptr, 2600);
        CHECK(CensusRun::g_picReq.empty(), "flat present-layer: a due picture is dropped at once (no back buffer)");
        g_vrMode = vr0; g_menuShowAll = all0;
    }
    // (restore: the rest of the test runs with the start state)
    g_censusCfg = cfg0; g_censusReady = ready0;
    if (g_censusOn) { g_censusOn = false; CensusRun::g_view = false; }
}

// phase 108: the shader census keys: defaults, clamps, the two hotkeys; phase 110: census = the menu row (Save / Reset)
static void CensusRules() {
    struct C { const char* ini; int cfg, dump, mb; };
    static const C kC[] = {
        // phase 109r: census and census_dump default to 0 (a dev tool: the dump tint flashed suspects for every player)
        { "beeps = 0\n",                                                     0, 0, 500 },
        { "beeps = 0\ncensus = 1\n",                                         1, 0, 500 },
        { "beeps = 0\ncensus = 1\ncensus_dump = 1\ncensus_max_mb = 64\n",    1, 1, 64 },
        { "beeps = 0\ncensus = 0\ncensus_dump = 0\n",                        0, 0, 500 },
        { "beeps = 0\ncensus_max_mb = 5\n",                                  0, 0, 16 },
        { "beeps = 0\ncensus_max_mb = 99999\n",                              0, 0, 4096 },
    };
    for (const C& c : kC) {
        WriteIni(c.ini);
        g_censusCfg = 0; g_censusDumpCfg = 0; g_censusMaxMb = 500;   // (the static defaults; LoadConfig runs once per process in game)
        LoadConfig();
        CHECK(g_censusCfg == c.cfg && g_censusDumpCfg == c.dump && g_censusMaxMb == c.mb, "census keys \"%s\" -> census %d dump %d max_mb %d "
              "(want %d %d %d)", c.ini, g_censusCfg, g_censusDumpCfg, g_censusMaxMb, c.cfg, c.dump, c.mb);
    }
    WriteIni("beeps = 0\n");
    LoadConfig();
    CHECK(g_keys[KA_CENSUS_VIEW].vk == VK_F5 && g_keys[KA_CENSUS_VIEW].mods == (MOD_SHIFT_BIT | MOD_CTRL_BIT), "key_census_view default Ctrl+Shift+F5");
    CHECK(g_keys[KA_CENSUS_MARK].vk == VK_F6 && g_keys[KA_CENSUS_MARK].mods == (MOD_SHIFT_BIT | MOD_CTRL_BIT), "key_census_mark default Ctrl+Shift+F6");
    CHECK(MenuHkLineOf(KA_CENSUS_VIEW) > 0 && MenuHkLineOf(KA_CENSUS_MARK) > 0, "the census keys are on the Hotkeys page");
    WriteIni("beeps = 0\nkey_census_view = none\nkey_census_mark = Alt+F1\n");
    LoadConfig();
    CHECK(g_keys[KA_CENSUS_VIEW].vk == 0 && g_keys[KA_CENSUS_MARK].vk == VK_F1 && g_keys[KA_CENSUS_MARK].mods == MOD_ALT_BIT,
          "key_census_view = none / key_census_mark = Alt+F1 read");
    // phase 110: census = the menu row "Show unfixed shaders" -- Save writes it, Reset removes it; census_dump / census_max_mb stay
    // startup-only keys (Save and Reset keep them)
    WriteIni("beeps = 0\ncensus = 1\ncensus_dump = 1\ncensus_max_mb = 64\n");
    std::vector<std::wstring> pn = Child(L"--show");
    CHECK(pn.size() == (size_t)MR_COUNT && pn[MR_CENSUS] == L"on", "census = 1: Show unfixed shaders shows on (not hidden)");
    RunChild(L"--row " + std::to_wstring((int)MR_SAVE) + L" --dir 1");
    std::string ini = ReadIni();
    CHECK(ini.find("census = 1") != std::string::npos && ini.find("census_max_mb = 64") != std::string::npos &&
          ini.find("census_dump = 1") != std::string::npos, "Save writes census = 1 and keeps census_dump / census_max_mb");
    RunChild(L"--row " + std::to_wstring((int)MR_CENSUS) + L" --dir -1");   // Left + Save
    ini = ReadIni();
    CHECK(ini.find("census = 0") != std::string::npos, "Show unfixed shaders Left + Save writes census = 0");
    RunChild(L"--row " + std::to_wstring((int)MR_CENSUS) + L" --dir 1 --vr");   // Right + Save (VR: the row is in both modes)
    ini = ReadIni();
    CHECK(ini.find("census = 1") != std::string::npos, "Show unfixed shaders Right + Save (VR) writes census = 1");
    Child(L"--reset");
    ini = ReadIni();
    CHECK(ini.find("census =") == std::string::npos, "Reset removes census (default 0)");
    CHECK(ini.find("census_max_mb = 64") != std::string::npos && ini.find("census_dump = 1") != std::string::npos,
          "Reset keeps census_dump / census_max_mb (startup-only keys)");
    WriteBaseIni();
    g_censusCfg = 0; g_censusDumpCfg = 0; g_censusMaxMb = 500;
    LoadConfig();
    CensusLiveRules();
}

// ---- phase 109: pixel_reader_fix / power_line_fix (startup only), pole_aa (menu row Thin pole fix + key_pole_aa, Save / Reset),
// pole_min_px (startup only, 1.0 .. 4.0) ------------------------------------------------------------------------------------------
static void Phase109Rules() {
    struct C { const char* ini; int pr, pl, pole; float px; };
    static const C kC[] = {
        { "beeps = 0\n",                                                        1, 1, 0, 1.0f },   // (phase 113: default 1.0)
        { "beeps = 0\npixel_reader_fix = 0\npower_line_fix = 0\n",              0, 0, 0, 1.0f },
        { "beeps = 0\npole_aa = 1\npole_min_px = 2.25\n",                       1, 1, 1, 2.25f },
        { "beeps = 0\npole_min_px = 0.2\n",                                     1, 1, 0, 1.0f },
        { "beeps = 0\npole_min_px = 9\n",                                       1, 1, 0, 4.0f },
    };
    for (const C& c : kC) {
        WriteIni(c.ini);
        g_prFixCfg = 1; g_powerLineCfg = 1; g_poleCfg = 0; g_poleMinPx = 1.0f;   // (the static defaults; LoadConfig runs once per process)
        LoadConfig();
        CHECK(g_prFixCfg == c.pr && g_powerLineCfg == c.pl && g_poleCfg == c.pole && std::fabs(g_poleMinPx - c.px) < 1e-6f,
              "phase 109 keys \"%s\" -> pixel_reader_fix %d power_line_fix %d pole_aa %d pole_min_px %.2f (want %d %d %d %.2f)", c.ini,
              g_prFixCfg, g_powerLineCfg, g_poleCfg, (double)g_poleMinPx, c.pr, c.pl, c.pole, (double)c.px);
    }
    WriteIni("beeps = 0\n");
    LoadConfig();
    CHECK(g_keys[KA_POLE_AA].vk == VK_F4 && g_keys[KA_POLE_AA].mods == (MOD_SHIFT_BIT | MOD_CTRL_BIT), "key_pole_aa default Ctrl+Shift+F4");
    CHECK(MenuHkLineOf(KA_POLE_AA) > 0, "key_pole_aa is on the Hotkeys page");
    WriteIni("beeps = 0\nkey_pole_aa = Alt+F2\n");
    LoadConfig();
    CHECK(g_keys[KA_POLE_AA].vk == VK_F2 && g_keys[KA_POLE_AA].mods == MOD_ALT_BIT, "key_pole_aa = Alt+F2 read");
    // the row: without the PS hooks it refuses (and is hidden); with them it switches pole_aa both ways
    g_poleCfg = 0;
    const bool hooked = g_lodPsHooked.load();
    g_lodPsHooked.store(false);
    MenuChange(MR_POLE, 1, false, 2200);
    CHECK(g_poleCfg == 0, "Thin pole fix without the PS hooks: nothing switches");
    g_lodPsHooked.store(true);
    MenuChange(MR_POLE, 1, false, 2201);
    CHECK(g_poleCfg == 1, "Thin pole fix Right: on");
    MenuChange(MR_POLE, -1, false, 2202);
    CHECK(g_poleCfg == 0, "Thin pole fix Left: off");
    g_lodPsHooked.store(hooked);
    // Save writes pole_aa (the row's state); Reset removes it (default 0); the startup-only keys stay in both
    WriteIni("beeps = 0\npole_aa = 1\npixel_reader_fix = 0\npower_line_fix = 0\npole_min_px = 2\n");
    RunChild(L"--row " + std::to_wstring((int)MR_SAVE) + L" --dir 1");
    std::string ini = ReadIni();
    CHECK(ini.find("pole_aa = 1") != std::string::npos, "Save writes pole_aa = 1");
    CHECK(ini.find("pixel_reader_fix = 0") != std::string::npos && ini.find("power_line_fix = 0") != std::string::npos &&
          ini.find("pole_min_px = 2") != std::string::npos, "Save keeps pixel_reader_fix / power_line_fix / pole_min_px");
    Child(L"--reset");
    ini = ReadIni();
    CHECK(ini.find("pole_aa = 1") == std::string::npos, "Reset removes pole_aa (default 0)");
    // phase 112: pixel_reader_fix / power_line_fix are Picture fixes now -- Reset = Picture fixes Standard removes them too
    CHECK(ini.find("pixel_reader_fix") == std::string::npos && ini.find("power_line_fix") == std::string::npos &&
          ini.find("pole_min_px = 2") != std::string::npos,
          "Reset removes pixel_reader_fix / power_line_fix (Picture fixes Standard), keeps pole_min_px (startup only)");
    WriteBaseIni();
    g_prFixCfg = 1; g_powerLineCfg = 1; g_poleCfg = 0; g_poleMinPx = 1.5f;
    LoadConfig();
}

// ---- phase 112: Picture fixes -- the preset logic, Off live, the key (incl. the Custom round trip), a hand change -> Custom, the
// hidden rows, Save / load / Reset, the user's two dlaa.ini files (their fix keys) -------------------------------------------------
static bool AllAt(const PicFixes::Set& want) {
    const PicFixes::Set s = PicFixRead();
    return PicFixes::Equal(s, want) && SceneDlaa::BiasStrength() == want.v[PicFixes::kBias] &&
           SceneDlaa::OutsideAaMode() == want.v[PicFixes::kOutsideAa] && (g_slWant >= 0 ? g_slWant : g_slCfg) == want.v[PicFixes::kShadow];
}
// ---- phase 112r: audit fixes -- mv_fwd_depth asked for + Recommended (VR budget guard, Save), the kept Custom sharpen set, the kept
// Recommended copy after a Quality preset change, key_picture_preset over the old name --------------------------------------------
static void Phase112rRules() {
    using namespace PicFixes;
    // 1. dlaa.ini mv_fwd_depth = 1 with Recommended (the user's ETS2 / ATS files): explicit, the VR budget guard does NOT switch it off
    const bool vr0 = g_vrMode;
    const float bud0 = g_fwdBudgetMs;
    WriteIni("beeps = 0\nperf_profile = high\nmv_fwd_depth = 1\n");
    LoadConfig();
    CHECK(g_picPreset == kStandard && g_fwdAsked && g_profExplicit[PK_FWD_DEPTH] && g_fwdCfg,
          "mv_fwd_depth = 1 + perf_profile high: Recommended, and the line is the player's request (asked %d explicit %d)",
          (int)g_fwdAsked, (int)g_profExplicit[PK_FWD_DEPTH]);
    g_vrMode = true; g_fwdBudgetMs = 0.30f; g_fwdAutoOff = false; g_fwdOn = true;
    FwdGuardJudge(1.0, 2.0, 300);                      // a window far over the budget
    CHECK(g_fwdOn.load() && !g_fwdAutoOff, "mv_fwd_depth = 1 + Recommended: the VR budget does not switch the Glass and fence fix off");
    StepPicFixes(1, 3100);                             // Recommended -> Off -> Custom -> Recommended: still the player's request
    StepPicFixes(1, 3101);
    StepPicFixes(1, 3102);
    CHECK(g_picPreset == kStandard && g_profExplicit[PK_FWD_DEPTH], "Recommended -> Off -> Custom -> Recommended keeps mv_fwd_depth explicit");
    // without the line: the profile's value, the guard trips (its log line stays true: not set anywhere, not Custom)
    WriteIni("beeps = 0\nperf_profile = high\n");
    LoadConfig();
    CHECK(g_picPreset == kStandard && !g_fwdAsked && !g_profExplicit[PK_FWD_DEPTH], "no mv_fwd_depth line: Recommended, not explicit");
    g_vrMode = true; g_fwdBudgetMs = 0.30f; g_fwdAutoOff = false; g_fwdOn = true;
    FwdGuardJudge(1.0, 2.0, 300);
    CHECK(!g_fwdOn.load() && g_fwdAutoOff, "no mv_fwd_depth line + Recommended: the VR budget switches it off (the guard is armed)");
    ToggleFwdDepth(3103);                              // Ctrl+F3: the player's choice now -> Custom, explicit
    CHECK(g_fwdOn.load() && !g_fwdAutoOff && g_fwdAsked && g_picPreset == kCustom, "Ctrl+F3 after the auto-off: on, asked, Custom");
    StepPicFixes(1, 3104);                             // Custom -> Recommended (the row)
    CHECK(g_picPreset == kStandard && g_profExplicit[PK_FWD_DEPTH], "Ctrl+F3, then the row back to Recommended: still explicit (asked)");
    g_vrMode = vr0; g_fwdBudgetMs = bud0; g_fwdAutoOff = false;
    // Save keeps the line with Recommended (112c removed it: the next start re-armed the guard)
    WriteIni("beeps = 0\nperf_profile = high\nmv_fwd_depth = 1\n");
    RunChild(L"--row " + std::to_wstring((int)MR_SAVE) + L" --dir 1");
    std::string ini = ReadIni();
    CHECK(ini.find("picture_preset = recommended") != std::string::npos && ini.find("mv_fwd_depth = 1") != std::string::npos,
          "Save with mv_fwd_depth = 1 + Recommended: picture_preset = recommended and the mv_fwd_depth line kept");
    WriteIni("beeps = 0\nperf_profile = high\n");
    RunChild(L"--row " + std::to_wstring((int)MR_SAVE) + L" --dir 1");
    ini = ReadIni();
    CHECK(ini.find("mv_fwd_depth") == std::string::npos, "Save without the line + Recommended: no mv_fwd_depth line (the preset's)");
    WriteBaseIni();
    LoadConfig();

    // 2. the kept Custom sharpen set: Custom -> the named presets -> Custom brings the player's values back (112c: Extra crisp's)
    {
        g_sharpPreset = SharpPreset::kCustom; g_sharpCustomHave = false;
        SceneDlaa::SetSharpness(0.7f); SceneDlaa::SetSharpHalo(15); SceneDlaa::SetSharpLuma(true);
        SceneDlaa::SetSharpType(SceneDlaa::kSharpNis); SceneDlaa::SetSharpRadius(2.5f);
        StepSharpPreset(1, 3110);                       // Custom -> Off
        CHECK(g_sharpPreset == SharpPreset::kOff && SceneDlaa::Sharpness() < 0.05f, "sharpen preset Custom, Right: Off (strength 0)");
        for (int i = 0; i < 4; ++i) StepSharpPreset(1, 3111 + i);   // Soft .. Extra crisp
        CHECK(g_sharpPreset == SharpPreset::kExtra && std::fabs(SceneDlaa::Sharpness() - 1.6f) < 0.05f, "... Extra crisp");
        StepSharpPreset(1, 3116);                       // -> Custom: the kept set
        CHECK(g_sharpPreset == SharpPreset::kCustom && std::fabs(SceneDlaa::Sharpness() - 0.7f) < 0.05f && SceneDlaa::SharpHalo() == 15 &&
              SceneDlaa::SharpLuma() && SceneDlaa::SharpType() == SceneDlaa::kSharpNis && std::fabs(SceneDlaa::SharpRadius() - 2.5f) < 0.05f,
              "sharpen preset back to Custom: the player's own five values again (%.1f, halo %d, type %s, size %.1f)",
              (double)SceneDlaa::Sharpness(), SceneDlaa::SharpHalo(), SceneDlaa::SharpTypeName(), (double)SceneDlaa::SharpRadius());
        StepSharpPreset(-1, 3117);                      // Custom, Left: Extra crisp (the Custom set is kept again)
        StepSharpPreset(1, 3118);
        CHECK(g_sharpPreset == SharpPreset::kCustom && std::fabs(SceneDlaa::Sharpness() - 0.7f) < 0.05f, "Custom -> Left -> Right: kept");
        g_sharpPreset = SharpPreset::kSoft; g_sharpCustomHave = false;
        SharpPresetApply(SharpPreset::kSoft);
    }

    // 3. Off over Recommended, a Quality preset change, then Custom: the kept copy of Recommended follows the Quality preset
    {
        const int prof0 = g_profile;
        char ch[256];
        g_profile = 0; ApplyProfile(true, ch, sizeof(ch));
        g_picPreset = kStandard; g_picBack = kStandard; g_picCustom = Standard();
        PicFixApplyLive(Standard(), 3120, "test");
        StepPicFixes(1, 3121);                          // Recommended -> Off
        g_profile = 2; ApplyProfile(true, ch, sizeof(ch));   // Quality preset low: Recommended's Glass and fence fix 0
        CHECK(g_picCustom.v[kFwd] == 0, "Off over Recommended + Quality preset low: the kept set follows Recommended (mv_fwd_depth 0)");
        StepPicFixes(1, 3122);                          // Off -> Custom
        CHECK(g_picPreset == kCustom && PicFixRead().v[kFwd] == 0, "... then Custom: the Glass and fence fix off (not the stale 1)");
        g_profile = prof0; ApplyProfile(true, ch, sizeof(ch));
        WriteBaseIni();
        LoadConfig();
    }

    // 4. key_picture_preset wins over the old key_picture_fixes, in either order in the file
    {
        const KeyBind k0 = g_keys[KA_PIC_FIXES];
        g_picKeyNewSeen = false;
        SetKeyBindFromIni("key_picture_preset", "Alt+F3");
        SetKeyBindFromIni("key_picture_fixes", "Alt+F2");
        CHECK(g_keys[KA_PIC_FIXES].vk == VK_F3, "key_picture_preset then key_picture_fixes: the new key wins (Alt+F3)");
        g_picKeyNewSeen = false;
        SetKeyBindFromIni("key_picture_fixes", "Alt+F2");
        SetKeyBindFromIni("key_picture_preset", "Alt+F3");
        CHECK(g_keys[KA_PIC_FIXES].vk == VK_F3, "key_picture_fixes then key_picture_preset: the new key wins (Alt+F3)");
        WriteIni("beeps = 0\nkey_picture_preset = Alt+F3\nkey_picture_fixes = Alt+F2\n");
        LoadConfig();
        CHECK(g_keys[KA_PIC_FIXES].vk == VK_F3, "dlaa.ini with both keys: key_picture_preset wins");
        g_keys[KA_PIC_FIXES] = k0;
        g_picKeyNewSeen = false;
        WriteBaseIni();
        LoadConfig();
    }
}

static void PicFixRules() {
    using namespace PicFixes;
    // the table: Recommended (was Standard) = the code defaults, Off = everything off; pole_aa / wire_widen not in Recommended yet;
    // phase 112c: cutout_mips IS (1), and mv_fwd_depth = the Quality preset's (g_stdFwd: high 1, low 0)
    const int sf0 = g_stdFwd;
    g_stdFwd = 1;
    const Set st = Standard(), of = Off();
    CHECK(st.v[kBias] == 100 && st.v[kShadow] == 1 && st.v[kPole] == 0 && st.v[kWireMv] == 1 && st.v[kWireDepth] == 1 &&
          st.v[kWireWiden] == 0 && st.v[kPowerLine] == 1 && st.v[kPixelReader] == 1 && st.v[kLightRedraw] == 1 && st.v[kLightFs] == 1 &&
          st.v[kWater] == 1 && st.v[kOutsideAa] == 2 && st.v[kCutout] == 1 && st.v[kFwd] == 1, "Picture preset Recommended = the code defaults");
    bool allOff = true;
    for (int f = 0; f < kFixCount; ++f) allOff = allOff && of.v[f] == 0;
    CHECK(allOff && !InStandard(kPole) && !InStandard(kWireWiden) && InStandard(kCutout) && InStandard(kShadow) && InStandard(kFwd),
          "Picture preset Off = every fix 0; pole_aa / wire_widen are not in Recommended yet, cutout_mips and mv_fwd_depth are");
    g_stdFwd = 0;
    CHECK(Standard().v[kFwd] == 0 && !InStandard(kFwd), "Quality preset low: Recommended has the Glass and fence fix off (g_stdFwd 0)");
    g_stdFwd = 1;
    // parsing, the row cycle, the key cycle, the load rule
    CHECK(Parse("standard") == kStandard && Parse("Recommended") == kRecommended && Parse(" OFF # x") == kOff && Parse("Custom") == kCustom &&
          Parse("nope") == -1 && Parse("") == -1 && !strcmp(IniName(kRecommended), "recommended"),
          "picture_preset parsing (112c: recommended; the old value standard still read)");
    for (int p = 0; p < kCount; ++p) CHECK(Parse(IniName(p)) == p, "picture_fixes %s reads back", IniName(p));
    CHECK(Next(kStandard, 1) == kOff && Next(kOff, 1) == kCustom && Next(kCustom, 1) == kStandard && Next(kStandard, -1) == kCustom,
          "the row: Standard -> Off -> Custom -> Standard, Left the other way");
    CHECK(KeyNext(kStandard, kStandard) == kOff && KeyNext(kOff, kStandard) == kStandard && KeyNext(kCustom, kCustom) == kOff &&
          KeyNext(kOff, kCustom) == kCustom, "the key: Standard <-> Off, Custom -> Off -> Custom");
    Set cu = st; cu.v[kWireWiden] = 1;                 // a custom set (112c: the user's cutout_mips = 1 is Recommended now)
    int back = -1;
    CHECK(Resolve(-2, st, &back) == kStandard && back == kStandard, "no key, Standard values -> Standard");
    CHECK(Resolve(-2, cu, &back) == kCustom && back == kCustom, "no key, wire_widen 1 -> Custom");
    CHECK(Resolve(-1, cu, &back) == kCustom, "an unknown picture_fixes reads as no key");
    CHECK(Resolve(kStandard, cu, &back) == kCustom, "picture_fixes standard + a differing fix key -> Custom (the value kept)");
    CHECK(Resolve(kOff, cu, &back) == kOff && back == kCustom, "picture_fixes off + a custom set -> Off, the key returns to Custom");
    CHECK(Resolve(kOff, st, &back) == kOff && back == kStandard, "picture_fixes off, no fix keys -> Off, the key returns to Standard");
    CHECK(Resolve(kCustom, st, &back) == kCustom && back == kCustom, "picture_fixes custom stays custom on Standard values");
    CHECK(Resolve(-2, of, &back) == kCustom, "no key, every fix 0 by hand -> Custom (Off is never guessed)");

    // live: Standard -> Off (every switch off, at once), the census sees the fixes off (its "only while on" rule)
    const int p0 = g_picPreset, b0 = g_picBack;
    const Set c0 = g_picCustom, live0 = PicFixRead();
    const bool hk0 = g_lodPsHooked.load(), ov0 = g_jitterOverscan != 0;
    g_lodPsHooked.store(true);
    g_picPreset = kStandard; g_picBack = kStandard; g_picCustom = st;
    PicFixApplyLive(st, 3000, "test");
    CHECK(AllAt(st), "Standard: every switch at its Standard value");
    const uint32_t mStd = CensusRun::FixMaskNow();
    MenuChange(MR_PICFIX, 1, false, 3001);             // Right: Off
    // phase 112r: Off keeps dlaa_mv_dilate (an NGX creation flag: switching it recreates every DLSS feature) -- here at Standard's 1
    CHECK(g_picPreset == kOff && AllAt(OffOver(st)) && DlaaProcessor::MvDilate() && g_fwdConsOn.load() == 0 && !g_wireOn.load() &&
          g_poleCfg == 0 && g_lightRedrawCfg == 0 && g_lightFsCfg == 0 && g_waterFixCfg == 0 && g_prFixCfg == 0 && g_powerLineCfg == 0,
          "Picture fixes Right from Standard: Off, every fix switch off live (Wire fix: motion kept: no NGX recreate)");
    CHECK(OffKeeps(kWireMv) && OffOver(of).v[kWireMv] == 0 && OffOver(st).v[kWireMv] == 1 && !OffKeeps(kBias),
          "phase 112r: OffOver keeps only dlaa_mv_dilate at the values in use");
    const uint32_t mOff = CensusRun::FixMaskNow();
    CHECK((mStd & 28u) == 28u && (mOff & 31u) == 0u, "the census fix mask: power / reader / water on with Standard (%x), every fix off "
          "with Off (%x) -- the pairs those fixes cover are suspects again", mStd, mOff);
    MenuChange(MR_PICFIX, 1, false, 3002);             // Right: Custom (a fresh Custom set = the Standard values)
    CHECK(g_picPreset == kCustom && AllAt(st), "Off, Right: Custom with the kept set (here the Standard values)");
    MenuChange(MR_PICFIX, 1, false, 3003);             // Right: Standard
    CHECK(g_picPreset == kStandard && AllAt(st), "Custom, Right: Standard");
    // a hand change of one fix (each row / key path) makes it Custom and keeps the other values
    struct H { int row; const char* what; int fix; };
    static const H kH[] = { { MR_BIAS, "Light trail fix", kBias }, { MR_SHADOW, "Steady own shadow", kShadow },
                            { MR_POLE, "Thin pole fix", kPole }, { MR_WIREMV, "Wire fix: motion", kWireMv },
                            { MR_WIREDEPTH, "Wire fix: depth", kWireDepth }, { MR_WIREWIDEN, "Thicker wires", kWireWiden },
                            { MR_FXPOWER, "Power line fix", kPowerLine }, { MR_FXREADER, "Lit edge fix", kPixelReader },
                            { MR_FXLIGHT, "Lit surface fix", kLightRedraw }, { MR_FXROOF, "Roof shadow fix", kLightFs },
                            { MR_FXWATER, "Water block fix", kWater }, { MR_FXOUTSIDE, "Outside the DLAA area", kOutsideAa },
                            { MR_FXCUTOUT, "Far fence and leaf fix", kCutout }, { MR_FWD, "Glass and fence fix", kFwd } };   // 112c: + Glass and fence
    // phase 112r: a custom set the Off cases return to (wire widening on, Light trail 50 %) -- a change from Off goes on top of it
    Set kept = st; kept.v[kWireWiden] = 1; kept.v[kBias] = 50;
    for (const H& h : kH)
        for (int from : { (int)kStandard, (int)kOff }) {
            for (int back : { (int)kStandard, (int)kCustom }) {
                if (from == kStandard && back == kCustom) continue;
                g_picPreset = from; g_picBack = back; g_picCustom = kept;
                PicFixApplyLive(Live(from, st), 3010, "test");
                const Set before = PicFixRead();
                MenuChange(h.row, from == kOff ? 1 : -1, false, 3011);   // (Light trail fix: Left at 0 % would hit its end)
                const Set after = PicFixRead();
                // Standard: exactly that fix changed. Off (112r): the set Off returns to (Standard / the kept Custom set), with
                // that fix changed from the value the player saw (Off's)
                const Set base = from == kOff ? Live(back, kept) : before;
                int diffs = 0;
                for (int f = 0; f < kFixCount; ++f) if (f != h.fix) diffs += after.v[f] != base.v[f];
                CHECK(g_picPreset == kCustom && diffs == 0 && after.v[h.fix] != before.v[h.fix],
                      "%s (%s, Off returns to %s): a hand change -> Custom = %s with only that fix changed (%d others differ)", h.what,
                      IniName(from), IniName(back), from == kOff ? "the set Off returns to" : "the values shown", diffs);
            }
        }
    g_picCustom = st;
    // the fixes' own hotkeys (their functions) do the same; with the menu closed a note shows
    g_picPreset = kStandard; PicFixApplyLive(st, 3020, "test");
    g_picNoteUntil = 0;
    ToggleWireWiden(3021);
    wchar_t l1[64], l2[64];
    CHECK(g_picPreset == kCustom && g_wireOn.load() && PicFixStatus(l1, l2, 64) && wcsstr(l1, L"Custom") && wcsstr(l2, L"Thicker wires"),
          "the wire widening hotkey from Standard: Custom, wire_widen on, the note \"%ls / %ls\"", l1, l2);
    for (void (*fn)(uint64_t) : { &ToggleMvDilate, &ToggleFwdConservative, &ToggleShadowLock, &ToggleFwdDepth }) {   // 112c: + Ctrl+F3
        g_picPreset = kStandard; PicFixApplyLive(st, 3022, "test");
        fn(3023);
        CHECK(g_picPreset == kCustom, "a fix hotkey from Standard -> Custom");
    }
    g_picPreset = kOff; g_picBack = kStandard; PicFixApplyLive(of, 3024, "test");
    TogglePoleAa(3025, "test");
    CHECK(g_picPreset == kCustom && g_poleCfg == 1 && g_biasCfg == 100,
          "key_pole_aa from Off (phase 112r): Custom = the set Off returns to (Recommended) + the pole fix");
    // the key: Standard <-> Off; Custom -> Off -> the SAME Custom values
    g_picPreset = kStandard; g_picBack = kStandard; PicFixApplyLive(st, 3030, "test");
    PicFixKey(3031);
    CHECK(g_picPreset == kOff && AllAt(OffOver(st)) && PicFixStatus(l1, l2, 64) && wcsstr(l1, L"Off"), "key from Standard: Off (note \"%ls\")", l1);
    PicFixKey(3032);
    CHECK(g_picPreset == kStandard && AllAt(st), "key from Off: Standard again");
    g_picPreset = kStandard; PicFixApplyLive(st, 3033, "test");
    MenuChange(MR_WIREWIDEN, 1, false, 3034);          // a Custom set: wire widening on, Light trail 75 %, pole on
    MenuChange(MR_BIAS, -1, false, 3035);
    MenuChange(MR_POLE, 1, false, 3036);
    const Set mine = PicFixRead();
    CHECK(g_picPreset == kCustom && mine.v[kWireWiden] == 1 && mine.v[kBias] == 75 && mine.v[kPole] == 1, "the test's Custom set");
    PicFixKey(3037);
    CHECK(g_picPreset == kOff && AllAt(OffOver(mine)), "key from Custom: Off");
    PicFixKey(3038);
    CHECK(g_picPreset == kCustom && AllAt(mine), "key again: the SAME Custom values back (wire widening, 75 %%, pole)");
    MenuChange(MR_PICFIX, -1, false, 3039);            // Custom, Left: Off; Left again: Standard; Right x2: Custom = mine
    MenuChange(MR_PICFIX, -1, false, 3040);
    CHECK(g_picPreset == kStandard && AllAt(st), "the row from Custom, Left x2: Standard");
    MenuChange(MR_PICFIX, 1, false, 3041);
    MenuChange(MR_PICFIX, 1, false, 3042);
    CHECK(g_picPreset == kCustom && AllAt(mine), "the row back to Custom: the kept Custom values");
    // the key and the row on the Hotkeys page; the default binding is free (no other action has it)
    CHECK(g_keys[KA_PIC_FIXES].vk == VK_F7 && g_keys[KA_PIC_FIXES].mods == (MOD_SHIFT_BIT | MOD_CTRL_BIT) && MenuHkLineOf(KA_PIC_FIXES) > 0,
          "key_picture_preset default Ctrl+Shift+F7, on the Hotkeys page");
    {   // phase 112c: the old key name key_picture_fixes is still read (a binding written before the rename keeps working)
        const KeyBind k0 = g_keys[KA_PIC_FIXES];
        SetKeyBindFromIni("key_picture_fixes", "Alt+F2");
        CHECK(g_keys[KA_PIC_FIXES].vk == VK_F2 && g_keys[KA_PIC_FIXES].mods == MOD_ALT_BIT && !strcmp(g_keys[KA_PIC_FIXES].ini, "key_picture_preset"),
              "key_picture_fixes = Alt+F2 binds key_picture_preset");
        g_keys[KA_PIC_FIXES] = k0;
    }
    // phase 112c: the Glass and fence fix in the preset -- Recommended leaves it to the Quality preset (not explicit), Custom owns it,
    // Off keeps it off through a Quality preset change
    g_fwdAsked = false;                                // (phase 112r: no mv_fwd_depth request -- the hotkey loop above made one)
    g_picPreset = kStandard; PicFixApplyLive(st, 3043, "test");
    CHECK(!g_profExplicit[PK_FWD_DEPTH] && g_fwdCfg && g_fwdOn.load(), "Recommended: the Glass and fence fix on, not explicit (the profile's)");
    g_picPreset = kOff; PicFixApplyLive(of, 3044, "test");
    {
        const int prof0 = g_profile;
        g_profile = 0;
        char ch[256];
        ApplyProfile(true, ch, sizeof(ch));
        CHECK(!g_fwdCfg && !g_fwdOn.load(), "Off: a Quality preset change does not turn the Glass and fence fix back on (%s)", ch);
        g_profile = 2;
        ApplyProfile(true, ch, sizeof(ch));
        CHECK(g_stdFwd == 0, "Quality preset low: Recommended's Glass and fence fix is off (g_stdFwd)");
        g_picPreset = kOff;
        MenuChange(MR_PICFIX, -1, false, 3045);        // Off, Left: Recommended
        CHECK(g_picPreset == kStandard && !g_fwdCfg, "Recommended at Quality preset low: the Glass and fence fix off");
        g_profile = prof0;
        ApplyProfile(true, ch, sizeof(ch));
        CHECK(g_stdFwd == kProf[prof0 < 0 || prof0 > 2 ? 0 : prof0].fwd && (g_fwdCfg != 0) == (g_stdFwd != 0),
              "back at the start's Quality preset: Recommended follows it");
    }
    for (int a = 0; a < KA_COUNT; ++a)
        for (int b = a + 1; b < KA_COUNT; ++b)
            if (g_keys[a].vk && g_keys[a].vk == g_keys[b].vk && g_keys[a].mods == g_keys[b].mods)
                CHECK(false, "two default hotkeys share one binding: %s and %s (%s)", g_keys[a].ini, g_keys[b].ini, g_keys[a].name);
    g_picPreset = p0; g_picBack = b0; g_picCustom = c0;
    PicFixApplyLive(live0, 3050, "test");
    g_stdFwd = sf0;
    g_lodPsHooked.store(hk0); g_jitterOverscan = ov0 ? 1 : 0;

    // dlaa.ini: what a start makes of it (children: LoadConfig once per process), Save, Reset
    auto strip = [](std::wstring s) { const std::wstring g = L"(greyed) "; return s.compare(0, g.size(), g) ? s : s.substr(g.size()); };
    struct C { const char* ini; const wchar_t* preset; const wchar_t* wire; const wchar_t* cutout; const wchar_t* bias; };
    static const C kC[] = {
        { "beeps = 0\n",                                                           L"Recommended", L"off", L"on",  L"100 %" },
        { "beeps = 0\npicture_preset = off\n",                                     L"Off",      L"off", L"off", L"off" },
        { "beeps = 0\npicture_preset = custom\n",                                  L"Custom",   L"off", L"on",  L"100 %" },
        { "beeps = 0\npicture_preset = recommended\nwire_widen = 1\n",             L"Custom",   L"on (experimental)", L"on", L"100 %" },
        { "beeps = 0\npicture_preset = bogus\n",                                   L"Recommended", L"off", L"on", L"100 %" },
        { "beeps = 0\ndlss_bias_mask = 100\nshadow_lock = 1\npole_aa = 0\n",        L"Recommended", L"off", L"on", L"100 %" },
        // phase 112c: the OLD key / value still read; the new key wins over the old one in the same file
        { "beeps = 0\npicture_fixes = off\n",                                      L"Off",      L"off", L"off", L"off" },
        { "beeps = 0\npicture_fixes = standard\n",                                 L"Recommended", L"off", L"on", L"100 %" },
        { "beeps = 0\npicture_fixes = off\npicture_preset = custom\n",              L"Custom",   L"off", L"on",  L"100 %" },
        { "beeps = 0\npicture_preset = custom\npicture_fixes = off\n",              L"Custom",   L"off", L"on",  L"100 %" },
        // the user's ETS2 / ATS dlaa.ini (2026-11-15): their fix keys -- cutout_mips 1 is Recommended now (112c), mv_fwd_depth 1 at
        // perf_profile high too -> Recommended
        { "beeps = 0\nperf_profile = high\ndlss_bias_mask = 100\ncutout_mips = 1\nwire_widen = 0\nshadow_lock = 1\npole_aa = 0\nmv_fwd_depth = 1\n",
                                                                                   L"Recommended", L"off", L"on", L"100 %" },
        { "beeps = 0\nperf_profile = high\nmv_fwd_depth = 1\nwire_widen = 0\ncutout_mips = 1\ndlss_bias_mask = 100\nshadow_lock = 1\n",
                                                                                   L"Recommended", L"off", L"on", L"100 %" },
        // the Glass and fence fix against the Quality preset: low = off in Recommended; a written 1 at low = Custom
        { "beeps = 0\nperf_profile = low\n",                                       L"Recommended", L"off", L"on", L"100 %" },
        { "beeps = 0\nperf_profile = low\nmv_fwd_depth = 1\n",                      L"Custom",   L"off", L"on",  L"100 %" },
        { "beeps = 0\nmv_fwd_depth = 0\n",                                          L"Custom",   L"off", L"on",  L"100 %" },
        // Off at the start: every fix off, the startup-only cut-out mips too (the key / row later: from the next start)
        { "beeps = 0\npicture_preset = off\ncutout_mips = 1\n",                     L"Off",      L"off", L"off", L"off" },
    };
    for (const C& c : kC)
        for (int vr = 0; vr < 2; ++vr) {
            WriteIni(c.ini);
            const std::vector<std::wstring> p = Child(vr ? L"--show --vr" : L"--show");
            if (p.size() != (size_t)MR_COUNT) { CHECK(false, "picture fixes ini case child failed: %s", c.ini); continue; }
            CHECK(strip(p[MR_PICFIX]) == c.preset && strip(p[MR_WIREWIDEN]) == c.wire && strip(p[MR_FXCUTOUT]) == c.cutout &&
                  strip(p[MR_BIAS]) == c.bias, "%s: dlaa.ini \"%s\" -> Picture fixes \"%s\" wires \"%s\" cut-out \"%s\" light trail \"%s\" "
                  "(want %ls / %ls / %ls / %ls)", vr ? "VR" : "flat", c.ini, U8(p[MR_PICFIX]).c_str(), U8(p[MR_WIREWIDEN]).c_str(),
                  U8(p[MR_FXCUTOUT]).c_str(), U8(p[MR_BIAS]).c_str(), c.preset, c.wire, c.cutout, c.bias);
            const bool custom = !wcscmp(c.preset, L"Custom");
            CHECK(p[MR_PICFIX].compare(0, 9, L"(greyed) ") != 0 && (p[MR_WIREWIDEN].compare(0, 9, L"(greyed) ") != 0) == custom &&
                  (p[MR_FWD].compare(0, 9, L"(greyed) ") != 0) == custom,
                  "%s: dlaa.ini \"%s\": the Picture preset row shown, the fix rows (+ Glass and fence) shown exactly with Custom",
                  vr ? "VR" : "flat", c.ini);
            if (strstr(c.ini, "perf_profile = low") && !custom)
                CHECK(strip(p[MR_FWD]) == L"off", "%s: Quality preset low + Recommended: the Glass and fence fix reads off (%s)", vr ? "VR" : "flat",
                      U8(p[MR_FWD]).c_str());
        }
    // Save: Standard removes every fix key (and writes picture_fixes); Custom writes all of them; Off over Custom keeps them
    WriteIni("beeps = 0\nshadow_lock = 1\ndlss_bias_mask = 100\npixel_reader_fix = 1\n");
    RunChild(L"--row " + std::to_wstring((int)MR_SAVE) + L" --dir 1");
    std::string ini = ReadIni();
    bool anyFix = false;
    for (int f = 0; f < kFixCount; ++f) anyFix = anyFix || ini.find(std::string(kDefs[f].key) + " =") != std::string::npos;
    CHECK(ini.find("picture_preset = recommended") != std::string::npos && ini.find("picture_fixes") == std::string::npos && !anyFix,
          "Save with Recommended: picture_preset = recommended, no picture_fixes, every fix key removed (the preset carries them)");
    RunChild(L"--row " + std::to_wstring((int)MR_WIREWIDEN) + L" --dir 1");   // a hand change + Save
    ini = ReadIni();
    bool allFix = true;
    for (int f = 0; f < kFixCount; ++f) allFix = allFix && ini.find(std::string(kDefs[f].key) + " =") != std::string::npos;
    CHECK(ini.find("picture_preset = custom") != std::string::npos && allFix && ini.find("wire_widen = 1") != std::string::npos &&
          ini.find("mv_fwd_depth = 1") != std::string::npos,
          "Thicker wires Right + Save: picture_preset = custom and every fix key written (wire_widen = 1, mv_fwd_depth = 1)");
    RunChild(L"--row " + std::to_wstring((int)MR_PICFIX) + L" --dir -1");      // Custom, Left = Off (over Custom) + Save
    ini = ReadIni();
    CHECK(ini.find("picture_preset = off") != std::string::npos && ini.find("wire_widen = 1") != std::string::npos,
          "Custom -> Off + Save: picture_preset = off, the Custom set stays in the file (the key returns to it)");
    std::vector<std::wstring> pn = Child(L"--show");
    CHECK(pn.size() == (size_t)MR_COUNT && strip(pn[MR_PICFIX]) == L"Off" && strip(pn[MR_WIREWIDEN]) == L"off",
          "the start with picture_fixes = off over a Custom set: Off, wires off");
    RunChild(L"--key");                                // the key at that start: back to the Custom set, Save
    ini = ReadIni();
    pn = Child(L"--show");
    CHECK(pn.size() == (size_t)MR_COUNT && strip(pn[MR_PICFIX]) == L"Custom" && strip(pn[MR_WIREWIDEN]) == L"on (experimental)" &&
          ini.find("picture_preset = custom") != std::string::npos, "key_picture_preset at an Off start over a Custom set: the Custom set back");
    Child(L"--reset");
    ini = ReadIni();
    anyFix = false;
    for (int f = 0; f < kFixCount; ++f) anyFix = anyFix || ini.find(std::string(kDefs[f].key) + " =") != std::string::npos;
    pn = Child(L"--show");
    CHECK(ini.find("picture_preset") == std::string::npos && !anyFix && pn.size() == (size_t)MR_COUNT && strip(pn[MR_PICFIX]) == L"Recommended",
          "Reset: picture_preset and every fix key removed -> Recommended at the next start");
    // phase 112c: an old dlaa.ini with picture_fixes = custom: Save writes picture_preset and REMOVES the old key
    WriteIni("beeps = 0\npicture_fixes = custom\nwire_widen = 1\n");
    RunChild(L"--row " + std::to_wstring((int)MR_SAVE) + L" --dir 1");
    ini = ReadIni();
    CHECK(ini.find("picture_preset = custom") != std::string::npos && ini.find("picture_fixes") == std::string::npos,
          "an old picture_fixes line: Save writes picture_preset = custom and removes picture_fixes");
    // a startup-only switch: cutout_mips set live reads "(next start)" until the restart (112c: on by default -> Right = off)
    WriteIni("beeps = 0\npicture_preset = custom\n");
    pn = Child(L"--row " + std::to_wstring((int)MR_FXCUTOUT) + L" --dir 1");
    CHECK(pn.size() == (size_t)MR_COUNT && pn[MR_FXCUTOUT] == L"off (next start)", "Far fence and leaf fix Right: \"off (next start)\" (%s)",
          pn.size() == (size_t)MR_COUNT ? U8(pn[MR_FXCUTOUT]).c_str() : "?");
    pn = Child(L"--show");
    CHECK(pn.size() == (size_t)MR_COUNT && pn[MR_FXCUTOUT] == L"off", "... and \"off\" after the restart");
    // Off switches it off for the next start too
    WriteIni("beeps = 0\n");
    pn = Child(L"--key");
    pn = Child(L"--show");
    CHECK(pn.size() == (size_t)MR_COUNT && strip(pn[MR_FXCUTOUT]) == L"off", "key_picture_preset (Off) + Save: the far fence fix off at the next start");
    WriteBaseIni();
}

// ---- phase 112c: the group presets (src/group_preset.h) -- Foveated preset / Panel preset: the table = the code defaults, the load
// rule, the row (Recommended <-> Custom keeps the Custom values), hidden detail rows, Save / load / Reset, the user's VRS / panel keys
static void GroupPresetRules() {
    using namespace GroupPreset;
    CHECK(Parse("recommended") == kRecommended && Parse(" Custom # x") == kCustom && Parse("standard") == -1 && Parse("") == -1,
          "group preset parsing");
    for (int g = 0; g < kPresetCount; ++g) {
        const Vals rec = Recommended(g);
        Vals c = rec; c.v[0] += 5;
        CHECK(Resolve(g, -2, rec) == kRecommended && Resolve(g, -1, rec) == kRecommended && Resolve(g, kRecommended, rec) == kRecommended &&
              Resolve(g, -2, c) == kCustom && Resolve(g, kRecommended, c) == kCustom && Resolve(g, kCustom, rec) == kCustom,
              "%s: the load rule (no key / recommended = Recommended only with the default values; custom stays)", kDefs[g].key);
        CHECK(Parse(IniName(kRecommended)) == kRecommended && Parse(IniName(kCustom)) == kCustom && Next(kRecommended) == kCustom &&
              Next(kCustom) == kRecommended, "%s: names and the row cycle", kDefs[g].key);
    }
    // the table = the Reset row's / the code's defaults (a fresh process: the globals' initial values = what Reset sets)
    CHECK(kDefs[kVrs].rec[0] == 55 && kDefs[kVrs].rec[1] == 80 && kDefs[kVrs].rec[2] == 0 && kDefs[kVrs].rec[3] == 0 &&
          kDefs[kPanel].rec[0] == 12 && kDefs[kPanel].rec[1] == 100 && kDefs[kPanel].rec[2] == 80 && kDefs[kPanel].rec[3] == 1,
          "the group preset tables = the code defaults (vrs 55 / 80 / 0 / 0, panel 12 / 100 / 80 / 1)");
    // live: Custom values -> Recommended (the defaults) -> Custom (the same values back)
    const int gp0[2] = { g_grpPreset[0], g_grpPreset[1] };
    const Vals v0[2] = { GrpRead(0), GrpRead(1) }, c0[2] = { g_grpCustom[0], g_grpCustom[1] };
    const Vals mine[2] = { Vals{ { 45, 70, 1, 2 } }, Vals{ { 20, 300, 120, 0 } } };
    const int rows[2] = { MR_VRSPRESET, MR_PANELPRESET };
    for (int g = 0; g < kPresetCount; ++g) {
        GrpApplyRaw(g, mine[g]);
        g_grpPreset[g] = kCustom;
        MenuChange(rows[g], 1, false, 3200);
        CHECK(g_grpPreset[g] == kRecommended && IsRecommended(g, GrpRead(g)), "%s: Custom, Right -> Recommended, the defaults live", kDefs[g].key);
        MenuChange(rows[g], -1, false, 3201);
        const Vals back = GrpRead(g);
        CHECK(g_grpPreset[g] == kCustom && !memcmp(&back, &mine[g], sizeof(back)), "%s: Recommended, Left -> Custom: the SAME values back",
              kDefs[g].key);
    }
    for (int g = 0; g < kPresetCount; ++g) { g_grpPreset[g] = gp0[g]; g_grpCustom[g] = c0[g]; GrpApplyRaw(g, v0[g]); }
    // dlaa.ini: what a VR start makes of it, and which rows show
    auto strip = [](std::wstring s) { const std::wstring g = L"(greyed) "; return s.compare(0, g.size(), g) ? s : s.substr(g.size()); };
    struct C { const char* ini; const wchar_t* vrs; const wchar_t* panel; };
    static const C kC[] = {
        { "beeps = 0\nvrs = 1\n",                                                             L"Recommended", L"Recommended" },
        { "beeps = 0\nvrs = 1\nvrs_preset = custom\n",                                         L"Custom",      L"Recommended" },
        { "beeps = 0\nvrs = 1\nvrs_preset = recommended\nvrs_strength = 2\n",                  L"Custom",      L"Recommended" },
        { "beeps = 0\nvrs = 1\nvrs_inner = 55\nvrs_outer = 80\nvrs_passes = 0\n",              L"Recommended", L"Recommended" },
        { "beeps = 0\nvrs = 1\npanel_preset = custom\n",                                        L"Recommended", L"Custom" },
        { "beeps = 0\nvrs = 1\nmenu_vr_depth = 12\nmenu_vr_face = 1\nmenu_vr_dist = 100\n",     L"Recommended", L"Recommended" },
        // the user's ETS2 (VRS 45 / 70, lighting too, strength 2; panel distance 300 m) and ATS (70 / 95, strength 2; panel 100)
        { "beeps = 0\nvrs = 1\nvrs_inner = 45\nvrs_outer = 70\nvrs_passes = 1\nvrs_strength = 2\nmenu_vr_depth = 12\n"
          "menu_vr_dist = 300\nmenu_vr_face = 1\nmenu_vr_lock = 1\nmenu_vr_lock_dist = 80\n",                    L"Custom", L"Custom" },
        { "beeps = 0\nvrs = 1\nvrs_inner = 70\nvrs_outer = 95\nvrs_passes = 0\nvrs_strength = 2\nmenu_vr_depth = 12\n"
          "menu_vr_dist = 100\nmenu_vr_face = 1\nmenu_vr_lock = 1\nmenu_vr_lock_dist = 80\n",                    L"Custom", L"Recommended" },
    };
    for (const C& c : kC) {
        WriteIni(c.ini);
        const std::vector<std::wstring> p = Child(L"--show --vr");
        if (p.size() != (size_t)MR_COUNT) { CHECK(false, "group preset ini case child failed: %s", c.ini); continue; }
        CHECK(strip(p[MR_VRSPRESET]) == c.vrs && strip(p[MR_PANELPRESET]) == c.panel, "dlaa.ini \"%s\" -> Foveated preset \"%s\", Panel "
              "preset \"%s\" (want %ls / %ls)", c.ini, U8(p[MR_VRSPRESET]).c_str(), U8(p[MR_PANELPRESET]).c_str(), c.vrs, c.panel);
        const bool vc = !wcscmp(c.vrs, L"Custom"), pc = !wcscmp(c.panel, L"Custom");
        CHECK((p[MR_PANELPRESET].compare(0, 9, L"(greyed) ") != 0) && (p[MR_DEPTH].compare(0, 9, L"(greyed) ") != 0) == pc &&
              (p[MR_FACE].compare(0, 9, L"(greyed) ") != 0) == pc, "dlaa.ini \"%s\": Panel preset shown, Panel depth / faces you shown "
              "exactly with Custom", c.ini);
        (void)vc;                                        // (the Foveated rows also need NVAPI VRS: hidden in a test process)
    }
    // Save writes both keys; Reset removes them
    WriteIni("beeps = 0\nvrs_strength = 2\n");
    RunChild(L"--row " + std::to_wstring((int)MR_SAVE) + L" --dir 1");
    std::string ini = ReadIni();
    CHECK(ini.find("vrs_preset = custom") != std::string::npos && ini.find("panel_preset = recommended") != std::string::npos,
          "Save writes vrs_preset = custom (vrs_strength 2) and panel_preset = recommended");
    Child(L"--reset");
    ini = ReadIni();
    CHECK(ini.find("vrs_preset") == std::string::npos && ini.find("panel_preset") == std::string::npos, "Reset removes vrs_preset / panel_preset");
    const std::vector<std::wstring> pr = Child(L"--show --vr");
    CHECK(pr.size() == (size_t)MR_COUNT && strip(pr[MR_VRSPRESET]) == L"Recommended" && strip(pr[MR_PANELPRESET]) == L"Recommended",
          "after Reset: both presets Recommended");
    WriteBaseIni();
}

// --rows-at <dlaa.ini>: which of the new rows a player with that dlaa.ini sees (flat and VR, as the menu shows them: hidden rows
// left out, every group open). Copies the file next to the exe; prints the result; exit 1 when a new row is hidden.
static int RowsAt(const wchar_t* src) {
    if (!CopyFileW(src, IniPath().c_str(), FALSE)) { printf("cannot copy %ls\n", src); return 2; }
    int bad = 0;
    for (int vr = 0; vr < 2; ++vr) {
        wchar_t a[64];
        swprintf(a, 64, L"--visible%ls", vr ? L" --vr" : L"");
        const std::vector<std::wstring> v = Child(a);
        if (v.size() != (size_t)MR_COUNT) { printf("child failed\n"); return 2; }
        for (int r : { (int)MR_PICFIX, (int)MR_SHARPPRESET, (int)MR_SHARPHALO, (int)MR_SHARPLUMA, (int)MR_SHADOW, (int)MR_SHARP,
                       (int)MR_SHARPTYPE, (int)MR_WIREWIDEN, (int)MR_POLE, (int)MR_FXCUTOUT, (int)MR_FXOUTSIDE, (int)MR_FWD,   // 112c: +
                       (int)MR_VRSPRESET, (int)MR_VRSSIZE, (int)MR_PANELPRESET, (int)MR_DEPTH }) {                          //   presets
            printf("%s: %-26s %s\n", vr ? "VR  " : "flat", RowName(r), U8(v[r]).c_str());
            if ((r == MR_SHARPPRESET || r == MR_PICFIX) && v[r].compare(0, 6, L"hidden") == 0) ++bad;
        }
    }
    DeleteFileW(IniPath().c_str());
    return bad ? 1 : 0;
}

// phase 115r (the audit fixes): the pure rules (legend overlap, joined-VS trust, late stride / feed rows) + on WARP: the phase-77
// decision survives a why-table collision (kWhyTag), the late read-back feeds at most kLateFeedBytes per Present (a 2048 x 2048 BC3:
// several Presents, then the pipeline), and a copy whose texture got a new upload meanwhile is dropped (A3)
static void Phase115rRules() {
    // B12: a legend wider than the two eyes' overlap is shrunk to it -> one shared dx fits BOTH pictures (no per-eye clamp)
    {
        const float W = 3448.0f, uPx[2] = { 0.379f * W, 0.621f * W }, sh[2] = { -40.0f, 40.0f };
        const float ov = LegendVr::OverlapW(W, uPx, sh);
        CHECK(std::fabs(ov - (W - std::fabs((uPx[0] + sh[0]) - (uPx[1] + sh[1])))) < 0.01f && ov < 0.8f * W,
              "phase 115r: OverlapW = W - the anchors' distance (%.0f of %.0f px, under the old 0.8 W cap)", ov, W);
        const float bw = 0.98f * ov;                       // (inject.cpp: min(0.8 W, 0.98 x overlap))
        const float dx = LegendVr::SharedDx(-0.18f * W, bw, W, uPx, sh);
        bool fits = true;
        for (int e = 0; e < 2; ++e) {
            const float c = uPx[e] + dx + sh[e];
            fits = fits && c - 0.5f * bw >= -0.01f && c + 0.5f * bw <= W + 0.01f;
        }
        CHECK(fits, "phase 115r: a legend shrunk to the overlap fits both eye pictures with one dx (no per-eye clamp = no double image)");
    }
    // B4: an OSGN-joined VS only for a non-instanced draw with a fresh static verdict; hand-listed VSs as before
    CHECK(JoinedVsTrusted(false, true, 0) && JoinedVsTrusted(false, false, 3) && JoinedVsTrusted(true, false, 1) &&
          !JoinedVsTrusted(true, true, 0) && !JoinedVsTrusted(true, true, 1) && !JoinedVsTrusted(true, false, 0) &&
          !JoinedVsTrusted(true, false, 3), "phase 115r: joined VS -> instanced refused, no fresh static verdict refused");
    // P3 / P1 pure rules
    CHECK(Cutout::LateStrideNext(4, false) == 8 && Cutout::LateStrideNext(32, false) == 64 && Cutout::LateStrideNext(64, false) == 64 &&
          Cutout::LateStrideNext(64, true) == 4, "phase 115r: the late stride backs off 4 -> 64 while idle, back to 4 at a start");
    CHECK(Cutout::LateFeedRows(Cutout::kLateFeedBytes, 16384, 1024, true) == 128 && Cutout::LateFeedRows(100, 16384, 9, true) == 1 &&
          Cutout::LateFeedRows(100, 16384, 9, false) == 0 && Cutout::LateFeedRows(1 << 20, 64, 3, true) == 3,
          "phase 115r: feed rows = the budget's whole rows (one at least on an untouched budget), never past the mip");
    // WARP
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        CHECK(false, "phase 115r: WARP device");
        return;
    }
    auto bc3 = [&](UINT w, UINT h) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 0; d.ArraySize = 1; d.Format = DXGI_FORMAT_BC3_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Texture2D* t = nullptr;
        dev->CreateTexture2D(&d, nullptr, &t);
        return t;
    };
    // B1: the why table is a cache -- a colliding texture taking the slot no longer makes a decided texture "unknown"
    {
        ID3D11Texture2D* a = bc3(256, 256);
        ID3D11Texture2D* b = bc3(256, 256);
        if (a && b) {
            { std::lock_guard<std::mutex> lk(Cutout::g_mx); Cutout::NoteWhy(a, Census::kCutPatched, 0.25); }
            const uint32_t sl = Cutout::WhySlot(a);
            { std::lock_guard<std::mutex> lk(Cutout::g_mx); Cutout::g_whyRes[sl] = b; Cutout::g_whyVal[sl] = (uint8_t)Census::kCutNoLoss; }
            double cov = -1.0;
            const int w = Cutout::CensusWhy(a, &cov);
            CHECK(w == Census::kCutPatched && std::fabs(cov - 0.25) < 0.01, "phase 115r (B1): after a table collision the texture's own tag "
                  "still says patched (%d, cov %.2f) -- no 'not seen' again, no second late read-back", w, cov);
            ID3D11Texture2D* c = bc3(128, 128);
            CHECK(c && Cutout::CensusWhy(c) == Census::kCutUnknown, "phase 115r: a texture never decided reads unknown");
            if (c) c->Release();
            { std::lock_guard<std::mutex> lk(Cutout::g_mx); Cutout::g_whyRes[Cutout::WhySlot(a)] = nullptr; }
        }
        if (a) a->Release();
        if (b) b->Release();
    }
    // P1: a 2048 x 2048 BC3 (12 mips, 5.6 MB) read back: never more than kLateFeedBytes (+ the one row an untouched budget allows)
    // per Present, one job at a time, then into the pipeline
    {
        ID3D11Texture2D* t = bc3(2048, 2048);
        CHECK(t != nullptr, "phase 115r: a 2048 x 2048 BC3");
        if (t) {
            D3D11_TEXTURE2D_DESC d{};
            t->GetDesc(&d);
            const uint64_t fed0 = Cutout::g_lateSt.fed;
            uint64_t n = 1000000;
            g_frames.store(n);
            const int st = Cutout::LateStart(ctx, t, d, Census::kCutUnknown, n, false);
            CHECK(st == Census::kCutLate && Cutout::g_late.size() == 1, "phase 115r: the read-back started (%d)", st);
            int polls = 0, moving = 0;
            uint64_t maxStep = 0;
            auto pos = [&]() -> uint64_t {                 // bytes copied so far (from the feed position)
                if (Cutout::g_late.empty()) return Cutout::LateBytes(d);
                const Cutout::LateJob& j = Cutout::g_late[0];
                uint64_t b = 0;
                for (UINT L = 0; L < j.mip; ++L)
                    b += (uint64_t)CutoutMips::Blocks(CutoutMips::MipDim(d.Width, (int)L)) *
                         CutoutMips::Blocks(CutoutMips::MipDim(d.Height, (int)L)) * 16;
                return b + (uint64_t)j.row * CutoutMips::Blocks(CutoutMips::MipDim(d.Width, (int)j.mip)) * 16;
            };
            const uint64_t gave0 = Cutout::g_lateSt.gaveUp;
            while (!Cutout::g_late.empty() && polls < 200) {
                ctx->Flush();
                Sleep(5);                                  // (a Present apart: WARP finishes the copy on its own threads)
                const uint64_t before = pos();
                g_frames.store(++n);
                Cutout::LatePoll(ctx);
                ++polls;
                const uint64_t step = pos() - before;
                if (step) ++moving;
                if (step > maxStep) maxStep = step;
            }
            printf("   phase 115r late feed: 2048x2048 BC3 (%.1f MB) fed in %d Presents (of %d polls), largest step %.2f MB, longest poll "
                   "%.2f ms\n", (double)Cutout::LateBytes(d) / 1048576.0, moving, polls, (double)maxStep / 1048576.0, Cutout::g_lateFeedMaxMs);
            CHECK(Cutout::g_late.empty() && Cutout::g_lateSt.fed == fed0 + 1 && Cutout::g_lateSt.gaveUp == gave0,
                  "phase 115r: the job finished and was fed (polls %d)", polls);
            CHECK(maxStep <= Cutout::kLateFeedBytes + 16384 && moving >= 3, "phase 115r: at most kLateFeedBytes per Present (largest %llu B), "
                  "spread over %d Presents", (unsigned long long)maxStep, moving);
            int why = -1;
            { std::lock_guard<std::mutex> lk(Cutout::g_mx); why = Cutout::WhyOf(t); }
            CHECK(why == Census::kCutPending || why == Census::kCutNoLoss || why == Census::kCutCoverage || why == Census::kCutSparse,
                  "phase 115r: the fed texture is in the pipeline (reason %d)", why);
            // A3: a new upload while the copy waits -> the job is dropped (stale)
            { std::lock_guard<std::mutex> lk(Cutout::g_mx); Cutout::NoteWhy(t, Census::kCutUnknown); }
            const uint64_t stale0 = Cutout::g_lateStale;
            g_frames.store(++n);
            CHECK(Cutout::LateStart(ctx, t, d, Census::kCutUnknown, n, false) == Census::kCutLate, "phase 115r: started again");
            const uint64_t seq = 987654321ull;
            t->SetPrivateData(Cutout::kSeqTag, sizeof(seq), &seq);   // (what OnMip0 does for a game upload)
            g_frames.store(++n);
            Cutout::LatePoll(ctx);
            CHECK(Cutout::g_late.empty() && Cutout::g_lateStale == stale0 + 1, "phase 115r (A3): a copy older than the texture's last upload "
                  "is dropped, never fed");
            t->Release();
        }
    }
    ctx->Release();
    dev->Release();
}

static int Parent() {
    if (IniPath().empty()) { printf("FAIL: no dlaa.ini path\n"); return 1; }
    Boot(false);                                       // (row names for the messages)
    MenuRules();                                       // phase 85
    CaptureRules();                                    // phase 89 review fixes
    PresetRules();                                     // phase 105b / 105a
    IniRules();
    CensusRules();                                     // phase 108
    Phase109Rules();                                   // phase 109
    PicFixRules();                                     // phase 112
    Phase112rRules();                                  // phase 112r
    GroupPresetRules();                                // phase 112c
    Phase115rRules();                                  // phase 115r
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
    bool vr = false, show = false, reset = false, visible = false, key = false;
    int row = -1, dir = 1, gates = -1;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--vr")) vr = true;
        else if (!wcscmp(argv[i], L"--rows-at") && i + 1 < argc) {   // phase 105: the new rows at a player's dlaa.ini
            wchar_t p[MAX_PATH];
            GetModuleFileNameW(nullptr, p, MAX_PATH);
            s_exe = p;
            Boot(false);
            return RowsAt(argv[i + 1]);
        }
        else if (!wcscmp(argv[i], L"--visible")) visible = true;
        else if (!wcscmp(argv[i], L"--show")) show = true;
        else if (!wcscmp(argv[i], L"--reset")) reset = true;
        else if (!wcscmp(argv[i], L"--key")) key = true;   // phase 112: key_picture_fixes once, then Save
        else if (!wcscmp(argv[i], L"--row") && i + 1 < argc) row = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--dir") && i + 1 < argc) dir = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--gates") && i + 1 < argc) gates = _wtoi(argv[++i]);
    }
    if (visible) {                                     // (child of --rows-at) the menu as the player sees it: hidden rows marked
        Boot(vr);
        g_menuShowAll = false;
        for (bool& o : g_menuGrpOpen) o = true;
        MenuBuildPanel(s_panel);
        MenuBuildPanel(s_panel);                       // (the hide state comes from the previous build)
        FILE* f = nullptr;
        if (_wfopen_s(&f, OutPath().c_str(), L"wb") != 0 || !f) return 3;
        int k = 0;
        for (int r = 0; r < MR_COUNT; ++r) {
            const bool shown = MenuRowVisible(r);
            fwprintf(f, L"%ls%ls\n", shown ? L"shown: " : L"hidden", shown && k < s_panel.nRows ? s_panel.rows[k].value : L"");
            if (shown) ++k;
        }
        fclose(f);
        return 0;
    }
    if (key) {
        Boot(vr);
        PicFixKey(1000);
        MenuChange(MR_SAVE, 1, false, 1001);
        WritePanel();
        return 0;
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
