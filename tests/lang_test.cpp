// phase 74: lang_test -- src/lang.cpp without the game.
//   lang_test.exe            unit checks: .po parsing (escapes, continuation lines, fuzzy), Tr fallback, TrF placeholder guard,
//                            TrA, the font pick for Latin text; exit 0 = all passed.
//   lang_test.exe fit DIR    every DIR\*.po: the chosen font, and the texts that are much wider than their English in the
//                            menu's fonts (a report for translators, exit 0).
// Build: tests\build_lang_test.bat -> build\tests\lang_test.exe
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include "lang.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static bool WriteFile8(const wchar_t* path, const char* text) {
    FILE* f = _wfopen(path, L"wb");
    if (!f) return false;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    return true;
}

static int Units() {
    const wchar_t* po = L"build\\tests\\lang_test_xx.po";
    const char* text =
        "\xEF\xBB\xBF# test\n"
        "msgid \"\"\nmsgstr \"\"\n\"Language: de_de\\n\"\n\n"
        "msgid \"Mode\"\nmsgstr \"Modus\"\n\n"
        "#, c-format\nmsgid \"%d fps\"\nmsgstr \"%d B/s\"\n\n"
        "#, c-format\nmsgid \"Costs %ls per frame now.\"\nmsgstr \"Kostet %d pro Bild.\"\n\n"   // wrong placeholder
        "#, c-format\nmsgid \"%d %% (%ls)\"\nmsgstr \"%d %% \\\"%ls\\\"\"\n\n"
        "msgid \"\"\n\"Long \"\n\"text\"\nmsgstr \"\"\n\"Langer \"\n\"Text\"\n\n"
        "#, fuzzy\nmsgid \"off\"\nmsgstr \"aus\"\n\n"
        "msgid \"on\"\nmsgstr \"\xC3\xA4n\"\n";                                                 // UTF-8 "än"
    CHECK(WriteFile8(po, text));
    CHECK(Lang::LoadForTest(po, "de_de"));
    CHECK(!wcscmp(Lang::Tr(L"Mode"), L"Modus"));
    CHECK(!wcscmp(Lang::TrF(L"%d fps"), L"%d B/s"));
    CHECK(!wcscmp(Lang::TrF(L"Costs %ls per frame now."), L"Costs %ls per frame now."));   // guard: English
    CHECK(!wcscmp(Lang::Tr(L"Costs %ls per frame now."), L"Kostet %d pro Bild."));        // (Tr itself does not check)
    CHECK(!wcscmp(Lang::TrF(L"%d %% (%ls)"), L"%d %% \"%ls\""));
    CHECK(!wcscmp(Lang::Tr(L"Long text"), L"Langer Text"));
    CHECK(!wcscmp(Lang::Tr(L"off"), L"off"));                                               // fuzzy = not used
    CHECK(!wcscmp(Lang::Tr(L"on"), L"än"));
    CHECK(!wcscmp(Lang::Tr(L"not there"), L"not there"));
    CHECK(!wcscmp(Lang::TrA("Mode"), L"Modus"));
    CHECK(!wcscmp(Lang::TrA("narrow only"), L"narrow only"));
    CHECK(!strcmp(Lang::Active(), "de_de") && !Lang::FontMissing());
    CHECK(!wcscmp(Lang::Face(), L"Segoe UI"));
    const uint32_t g = Lang::Gen();
    CHECK(TR(L"Mode") == Lang::Tr(L"Mode"));                                                // the cached site
    CHECK(Lang::Gen() == g);
    // placeholder guard: a stray % is never accepted, %% is text, the colour marks count
    const wchar_t* bad = L"build\\tests\\lang_test_bad.po";
    CHECK(WriteFile8(bad, "msgid \"%d px\"\nmsgstr \"% d px\"\n\nmsgid \"a\x01%ls\x03\"\nmsgstr \"%ls b\"\n\n"
                          "msgid \"100 %% x\"\nmsgstr \"100 %% y\"\n"));
    CHECK(Lang::LoadForTest(bad, "de_de"));
    CHECK(!wcscmp(Lang::TrF(L"%d px"), L"%d px"));
    CHECK(!wcscmp(Lang::TrF(L"a\x01%ls\x03"), L"a\x01%ls\x03"));
    CHECK(!wcscmp(Lang::TrF(L"100 %% x"), L"100 %% y"));
    CHECK(Lang::Gen() != g);
    printf("%s (%d failed)\n", g_fail ? "FAILED" : "all lang checks passed", g_fail);
    return g_fail ? 1 : 0;
}

// ---- fit report -------------------------------------------------------------------------------------------------------
struct Pair { std::wstring en, tr; char kind = 0; };   // kind: the first letter of the template's "#." note
static std::wstring Unq(const std::string& l) {
    const size_t a = l.find('"'), b = l.rfind('"');
    std::string o;
    if (a == std::string::npos || b <= a) return std::wstring();
    for (size_t i = a + 1; i < b; ++i) {
        if (l[i] == '\\' && i + 1 < b) { ++i; o += l[i] == 'n' ? '\n' : l[i]; } else o += l[i];
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, o.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 1), 0);
    MultiByteToWideChar(CP_UTF8, 0, o.c_str(), -1, &w[0], n);
    if (!w.empty() && !w.back()) w.pop_back();
    return w;
}
static std::vector<Pair> ReadPairs(const wchar_t* path, bool keepEmpty = false) {
    std::vector<Pair> out;
    FILE* f = _wfopen(path, L"rb");
    if (!f) return out;
    std::string line, all;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) all.append(buf, n);
    fclose(f);
    Pair cur;
    int part = 0;
    size_t p = 0;
    auto flush = [&]() { if (!cur.en.empty() && (!cur.tr.empty() || keepEmpty)) out.push_back(cur); cur = Pair(); part = 0; };
    while (p < all.size()) {
        size_t e = all.find('\n', p);
        if (e == std::string::npos) e = all.size();
        line = all.substr(p, e - p);
        p = e + 1;
        if (!line.compare(0, 3, "#. "))         { if (part) flush(); cur.kind = line.size() > 3 ? line[3] : 0; }
        else if (!line.compare(0, 5, "msgid"))  { const char k = part ? 0 : cur.kind; flush(); cur.kind = k; cur.en = Unq(line); part = 1; }
        else if (!line.compare(0, 6, "msgstr")) { cur.tr = Unq(line); part = 2; }
        else if (!line.empty() && line[0] == '"') { if (part == 1) cur.en += Unq(line); else if (part == 2) cur.tr += Unq(line); }
    }
    flush();
    return out;
}
static int Width(HDC dc, const wchar_t* face, float px, const std::wstring& s) {
    HFONT f = CreateFontW(-(int)(px + 0.5f), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
    HGDIOBJ o = SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, o);
    DeleteObject(f);
    return sz.cx;
}
static int Fit(const char* dir8) {
    wchar_t dir[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, dir8, -1, dir, MAX_PATH);
    std::wstring pat = std::wstring(dir) + L"\\*.po";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("no .po files in %s\n", dir8); return 0; }
    HDC dc = CreateCompatibleDC(nullptr);
    std::vector<Pair> tmpl = ReadPairs((std::wstring(dir) + L"\\template.pot").c_str(), true);   // the "#." kinds
    auto kindOf = [&](const std::wstring& en) { for (const Pair& t : tmpl) if (t.en == en) return t.kind; return (char)0; };
    do {
        const std::wstring path = std::wstring(dir) + L"\\" + fd.cFileName;
        char code[16] = "";
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, 5, code, 15, nullptr, nullptr);
        code[5] = 0;
        if (!Lang::LoadForTest(path.c_str(), code)) continue;
        char face[64] = "";
        WideCharToMultiByte(CP_UTF8, 0, Lang::Face(), -1, face, 63, nullptr, nullptr);
        printf("== %s: font %s%s\n", code, face, Lang::FontMissing() ? "  ** FONT MISSING **" : "");
        int over = 0;
        for (const Pair& pr : ReadPairs(path.c_str())) {
            // the columns (menu.cpp, reference px): Cost 104, Value 160, Setting 228 (- the group indent), Hotkey 216; headings in
            // the 12.5 px font; descriptions: 1.3 x the English (4 lines hold it), the rest is shortened or wraps by itself
            const char k = kindOf(pr.en);
            const float px = k == 'D' ? 14.0f : (k == 'H' ? 12.5f : 15.0f);
            const int we = Width(dc, L"Segoe UI", px, pr.en), wt = Width(dc, Lang::Face(), px, pr.tr);
            int lim = 0;
            if (k == 'C') lim = pr.en.size() < 12 ? 100 : 0;                  // "Cost column cell"; (the CPU | GPU heading: none)
            else if (k == 'V') lim = 158;
            else if (k == 'S') lim = 205;
            else if (k == 'K') lim = 212;                                      // the Hotkey column
            else if (k == 'H') lim = pr.en.size() < 12 ? 150 : 0;              // a column heading
            else if (k == 'D') lim = we * 13 / 10 + 40;
            if (lim && we > lim) lim = we + 8;                                 // as wide as the English is fine
            if (!lim) continue;
            if (wt > lim) {
                char e8[160], t8[400];
                WideCharToMultiByte(CP_UTF8, 0, pr.en.c_str(), -1, e8, 159, nullptr, nullptr); e8[159] = 0;
                WideCharToMultiByte(CP_UTF8, 0, pr.tr.c_str(), -1, t8, 399, nullptr, nullptr); t8[399] = 0;
                printf("  TOO WIDE %d px (English %d, limit %d): \"%.60s\" -> \"%.120s\"\n", wt, we, lim, e8, t8);
                ++over;
            }
        }
        printf("  %d too wide\n", over);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    DeleteDC(dc);
    return 0;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 3 && !strcmp(argv[1], "fit")) return Fit(argv[2]);
    return Units();
}
