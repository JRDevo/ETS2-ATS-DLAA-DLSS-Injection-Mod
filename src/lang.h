// Lang (v0.15.0 phase 74) -- the tuning menu in the game's languages.
// gettext style: the English text in the code IS the key. TR(L"Mode") returns the active language's text for "Mode", or
// "Mode" itself when there is none; TRF(L"%d fps") the same for a printf FORMAT, whose translation is used only when it has
// exactly the same conversions (%d, %.1f, %ls ...) in the same order -- else the English one (a player's file can never make
// swprintf read the wrong argument). N_(L"...") / NA_("...") only MARK a text in a table for scripts/lang_tool.py (the
// extractor); the table's user translates it at the use with Tr() / TrA(). TRA("narrow") = TR for a UTF-8 literal.
// Files: lang/<code>.po in the repo (one per game language, the game's own codes: de_de, ru_ru, ko_kr ...; lang/template.pot
// = every English text, made by `python scripts/lang_tool.py extract`). The build packs them into the DLL (RCDATA LANGPACK:
// a hash of each English text + its translation, no English copies). A lang\<code>.po NEXT TO THE DLL wins over the packed
// one, text by text, and is re-read when it changed (at every menu open): players edit it and see the result in the game.
// Language: dlaa.ini `language` = auto (the default: the game's language from its game.log.txt / config.cfg, NOT Windows'
// -- Proton reports English there) or a code. Font: the menu's GDI face is picked per language from fonts Windows ships
// (Segoe UI; Yu Gothic UI / Malgun Gothic / Microsoft YaHei UI for ja / ko / zh) and every character of the language is
// probed (GetGlyphIndicesW); none fits (Linux / Proton without the fonts) = English + FontMissing() (the menu says so).
// Render thread only (the menu builds and Refresh run there); the returned texts stay valid until the next Refresh that
// changes the language or reloads a file (Gen() changes then), so a caller copies them (swprintf into the Panel) at once.
#pragma once
#include <cstdint>

namespace Lang {

uint32_t        Gen();                              // bumped whenever the active texts change (the TR sites re-resolve)
const wchar_t*  Tr(const wchar_t* en);              // the translation of `en`, or `en` (content lookup: any string)
const wchar_t*  TrF(const wchar_t* en);             // the same for a printf format: the conversions must match `en`'s
const wchar_t*  TrA(const char* en);                // a UTF-8 English text (key names, tables) -> the wide translation or
                                                    //   the widened English (kept in a small per-text cache)

struct Site {                                       // one TR(...) in the code: resolved once per Gen()
    uint32_t gen = ~0u;
    const wchar_t* s = nullptr;
    const wchar_t* Get(const wchar_t* en, bool fmt) {
        if (gen != Gen()) { s = fmt ? TrF(en) : Tr(en); gen = Gen(); }
        return s;
    }
};
struct SiteA {
    uint32_t gen = ~0u;
    const wchar_t* s = nullptr;
    const wchar_t* Get(const char* en) {
        if (gen != Gen()) { s = TrA(en); gen = Gen(); }
        return s;
    }
};

// The menu's settings and state.
void            SetSetting(const char* v);          // dlaa.ini language: "auto" or a code (unknown codes = auto)
const char*     Setting();                          // "auto" or the code
void            Refresh();                          // at every menu open: resolve the language, reload changed files, probe
                                                    //   the font (cheap when nothing changed). Logs what it picked.
const char*     Active();                           // the language in use now ("en_us" = English)
const char*     Wanted();                           // the language asked for (Active() differs when its font is missing)
bool            FontMissing();                      // Wanted() has no font on this PC: the menu shows English
const wchar_t*  Face();                             // the GDI face for the menu ("Segoe UI" for English)
int             Count();                            // the languages the menu row cycles: 0 = auto, then English, then the packed ones
const char*     CodeAt(int i);                      // "auto", "en_us", "de_de" ...
const wchar_t*  NativeName(const char* code);       // "Deutsch", L"English" ...; the code itself when unknown
bool            LoadForTest(const wchar_t* poPath, const char* code);   // tests: that .po file active as `code` (no pack)

}  // namespace Lang

#define N_(s)  s
#define NA_(s) s
#define TR(s)  ([]() -> const wchar_t* { static Lang::Site site_; return site_.Get(s, false); }())
#define TRF(s) ([]() -> const wchar_t* { static Lang::Site site_; return site_.Get(s, true); }())
#define TRA(s) ([]() -> const wchar_t* { static Lang::SiteA site_; return site_.Get(s); }())
