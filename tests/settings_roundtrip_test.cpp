// Issue #26 test: every tuning menu row survives a game restart, and the Reset row really resets.
// Builds inject.cpp WITH_DLAA into this exe (tests\build_settings_roundtrip_test.bat) and drives the REAL menu code: MenuChange (the
// row's Left / Right), MenuBuildPanel (the value column the user reads), SaveSettings (the Save row), ResetSettings (the Reset hold)
// and LoadConfig (the start). A "game restart" is a fresh process of this exe: nothing in memory carries over, only dlaa.ini.
//
//   settings_roundtrip_test.exe            the whole test (exit 0 = all checks passed)
//   ... --row <MR_ id> --dir <+1|-1>       (child) start, press the row, Save, write the panel
//   ... --show                             (child) start, write the panel
//   ... --reset                            (child) start, press every row once, Reset, write the panel
//
// For each row, Right and Left, flat and VR: the value column after the press must be the same after the restart -- for that row
// AND every other row (a press that changes a neighbour has to save the neighbour too). A press that changes nothing (a greyed
// row, an information row) is counted, not failed. Issue #26 (See-through depth back to "on" after a restart) fails here.
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
    for (int r = 0; r < MR_COUNT; ++r) fwprintf(f, L"%ls\n", s_panel.rows[r].value);
    fclose(f);
}

// rows that are a live A/B switch on purpose (the saved choice is another row): after a restart they are back as they started
static bool SessionOnly(int r) { return r == MR_DLAA; }   // Shift+F11 compare; the Mode row (End) is the saved one

// rows the round trip leaves out: group headers (open / close only), actions and pages, information rows
static bool SkipRow(int r) {
    switch (r) {
    case MR_PICGRP: case MR_MOTGRP: case MR_UPGRP: case MR_VRPGRP: case MR_FRGRP: case MR_PANELGRP: case MR_FPSGRP:
    case MR_FPSREMEAS: case MR_MENUKEYS: case MR_HOTKEYS: case MR_SAVE: case MR_RESET: case MR_CLOSE:
    case MR_OFXR: case MR_VECTORXR:
        return true;
    default:
        return false;
    }
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

static int Parent() {
    if (IniPath().empty()) { printf("FAIL: no dlaa.ini path\n"); return 1; }
    Boot(false);                                       // (row names for the messages)
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
    int row = -1, dir = 1;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--vr")) vr = true;
        else if (!wcscmp(argv[i], L"--show")) show = true;
        else if (!wcscmp(argv[i], L"--reset")) reset = true;
        else if (!wcscmp(argv[i], L"--row") && i + 1 < argc) row = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"--dir") && i + 1 < argc) dir = _wtoi(argv[++i]);
    }
    if (!show && !reset && row < 0) {
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(nullptr, p, MAX_PATH);
        s_exe = p;
        return Parent();
    }
    Boot(vr);
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
