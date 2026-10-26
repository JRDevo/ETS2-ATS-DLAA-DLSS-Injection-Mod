// Lang (v0.15.0 phase 74) -- see lang.h. The packed table (RCDATA "LANGPACK", made by scripts/lang_tool.py pack):
//   "DLNG" u32 version (1) u32 nLangs, then per language: char code[8], u32 nameOff (UTF-8 native name), u32 entOff, u32 nEnt;
//   at entOff nEnt x { u64 hash, u32 strOff } sorted by hash; every string UTF-8 with a 0 end. Offsets from the pack start.
//   hash = FNV-1a 64 over the UTF-16 code units of the English text (the same in lang_tool.py).
#include "lang.h"
#include "log.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace Lang {
namespace {

constexpr const char* kEnglish = "en_us";

uint64_t Hash(const wchar_t* s) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; ++s) { h ^= (uint16_t)*s; h *= 1099511628211ull; }
    return h;
}
std::wstring Widen(const char* s, int n = -1) {
    if (!s || !*s || !n) return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, s, n, nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring w((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, n, &w[0], len);
    if (n < 0 && !w.empty() && w.back() == 0) w.pop_back();
    return w;
}

// The printf conversions of `s` in order (+ the ink marks 0x01..0x03), one token each; %% is text. A '%' that starts no valid
// conversion becomes the token "%!" (English never has one, so such a translation never matches). The same in lang_tool.py.
std::wstring Specs(const wchar_t* s) {
    std::wstring out;
    for (size_t i = 0; s[i];) {
        const wchar_t c = s[i];
        if (c >= 1 && c <= 3) { out += c; out += L'\x1F'; ++i; continue; }
        if (c != L'%') { ++i; continue; }
        if (s[i + 1] == L'%') { i += 2; continue; }
        size_t j = i + 1;
        while (s[j] && wcschr(L"-+ #0", s[j])) ++j;
        if (s[j] == L'*') ++j; else while (s[j] >= L'0' && s[j] <= L'9') ++j;
        if (s[j] == L'.') { ++j; if (s[j] == L'*') ++j; else while (s[j] >= L'0' && s[j] <= L'9') ++j; }
        static const wchar_t* const kLen[] = { L"hh", L"h", L"ll", L"l", L"L", L"I64", L"I32", L"I", L"j", L"z", L"t", L"w" };
        for (const wchar_t* m : kLen) { const size_t n = wcslen(m); if (!wcsncmp(s + j, m, n)) { j += n; break; } }
        if (s[j] && wcschr(L"diouxXeEfFgGaAcCsSpn", s[j])) { out.append(s + i, j + 1 - i); out += L'\x1F'; i = j + 1; }
        else { out += L"%!\x1F"; i = j > i + 1 ? j : i + 1; }
    }
    return out;
}

// ---- one language's texts --------------------------------------------------------------------------------------------
struct Table {
    std::string code;                               // "" = English (nothing loaded)
    std::vector<std::pair<uint64_t, uint32_t>> idx; // sorted by hash; uint32_t = offset into arena
    std::vector<wchar_t> arena;
    void Add(uint64_t h, const std::wstring& t) {
        idx.emplace_back(h, (uint32_t)arena.size());
        arena.insert(arena.end(), t.begin(), t.end());
        arena.push_back(0);
    }
    void Finish() {                                 // sort; a later duplicate (the override file) wins
        std::stable_sort(idx.begin(), idx.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<std::pair<uint64_t, uint32_t>> u;
        u.reserve(idx.size());
        for (const auto& e : idx) { if (!u.empty() && u.back().first == e.first) u.back() = e; else u.push_back(e); }
        idx.swap(u);
    }
    const wchar_t* Find(uint64_t h) const {
        auto it = std::lower_bound(idx.begin(), idx.end(), h, [](const auto& a, uint64_t v) { return a.first < v; });
        return it != idx.end() && it->first == h ? &arena[it->second] : nullptr;
    }
};

// ---- the packed table ----------------------------------------------------------------------------------------------------
struct PackLang { std::string code; std::wstring name; uint32_t entOff = 0, nEnt = 0; };
const uint8_t*        g_pack = nullptr;
size_t                g_packSize = 0;
std::vector<PackLang> g_packLangs;
bool                  g_packTried = false;

uint32_t Rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint64_t Rd64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

HMODULE SelfModule() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SelfModule), &m);
    return m;
}
void LoadPack() {
    if (g_packTried) return;
    g_packTried = true;
    HMODULE self = SelfModule();
    HRSRC r = self ? FindResourceW(self, L"LANGPACK", MAKEINTRESOURCEW(10)) : nullptr;
    HGLOBAL g = r ? LoadResource(self, r) : nullptr;
    const uint8_t* p = g ? static_cast<const uint8_t*>(LockResource(g)) : nullptr;
    const size_t n = r ? SizeofResource(self, r) : 0;
    if (!p || n < 12 || memcmp(p, "DLNG", 4) || Rd32(p + 4) != 1) {
        Log("language: no packed translations in this build (English only)");
        return;
    }
    const uint32_t nl = Rd32(p + 8);
    if (12 + (size_t)nl * 20 > n) { LogErr("language: the packed table is damaged (English only)"); return; }
    for (uint32_t i = 0; i < nl; ++i) {
        const uint8_t* e = p + 12 + (size_t)i * 20;
        PackLang l;
        l.code.assign(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 8));
        const uint32_t nameOff = Rd32(e + 8);
        l.entOff = Rd32(e + 12);
        l.nEnt = Rd32(e + 16);
        if (nameOff >= n || l.entOff + (size_t)l.nEnt * 12 > n) { LogErr("language: packed entry %u is damaged", i); continue; }
        l.name = Widen(reinterpret_cast<const char*>(p + nameOff));
        g_packLangs.push_back(l);
    }
    g_pack = p;
    g_packSize = n;
    Log("language: %u packed languages", (unsigned)g_packLangs.size());
}
const PackLang* FindPack(const std::string& code) {
    for (const PackLang& l : g_packLangs) if (l.code == code) return &l;
    return nullptr;
}

// ---- .po files ---------------------------------------------------------------------------------------------------------
std::string PoUnquote(const char* p) {             // the text between the first '"' and the last '"', escapes decoded
    const char* a = strchr(p, '"');
    const char* b = strrchr(p, '"');
    std::string out;
    if (!a || b <= a) return out;
    for (const char* q = a + 1; q < b; ++q) {
        if (*q != '\\' || q + 1 >= b) { out += *q; continue; }
        ++q;
        switch (*q) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            default:  out += *q; break;            // \" \\ and anything else: the character itself
        }
    }
    return out;
}
// Parses a .po file into t (msgid hash -> msgstr); fuzzy entries and empty msgstr are skipped. False = no such file.
bool LoadPo(const wchar_t* path, Table& t, int& added, int& rejected) {
    added = rejected = 0;
    FILE* f = _wfsopen(path, L"rb", _SH_DENYNO);
    if (!f) return false;
    std::string data;
    char buf[16384];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0 && data.size() < (8u << 20)) data.append(buf, got);
    fclose(f);
    if (data.size() >= 3 && !memcmp(data.data(), "\xEF\xBB\xBF", 3)) data.erase(0, 3);
    std::string id, str, ctx;
    int part = 0;                                   // 0 none, 1 msgctxt, 2 msgid, 3 msgstr
    bool fuzzy = false, haveEntry = false;
    auto flush = [&]() {
        if (haveEntry && !id.empty() && !str.empty() && !fuzzy) {
            const std::wstring wid = Widen(id.c_str()), wstr = Widen(str.c_str());
            if (!wid.empty() && !wstr.empty()) { t.Add(Hash(wid.c_str()), wstr); ++added; }
            else ++rejected;
        }
        id.clear(); str.clear(); ctx.clear();
        part = 0; fuzzy = false; haveEntry = false;
    };
    size_t pos = 0;
    while (pos < data.size()) {
        size_t e = data.find('\n', pos);
        if (e == std::string::npos) e = data.size();
        std::string line = data.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        const char* l = line.c_str() + s;
        if (!*l) { if (part == 3) flush(); continue; }
        if (l[0] == '#') {
            if (part == 3) flush();
            if (l[1] == ',' && strstr(l, "fuzzy")) fuzzy = true;
            continue;
        }
        if (!strncmp(l, "msgctxt", 7)) { if (part == 3) flush(); ctx = PoUnquote(l); part = 1; haveEntry = true; }
        else if (!strncmp(l, "msgid_plural", 12)) { part = 0; }
        else if (!strncmp(l, "msgid", 5)) { if (part == 3) flush(); id = PoUnquote(l); part = 2; haveEntry = true; }
        else if (!strncmp(l, "msgstr", 6)) { str = PoUnquote(l); part = 3; }
        else if (l[0] == '"') {
            if (part == 1) ctx += PoUnquote(l); else if (part == 2) id += PoUnquote(l); else if (part == 3) str += PoUnquote(l);
        }
    }
    if (part == 3) flush();
    return true;
}

// ---- the state -----------------------------------------------------------------------------------------------------------
std::string  g_setting = "auto";
std::string  g_wanted = kEnglish, g_activeCode = kEnglish;
bool         g_fontMissing = false;
std::wstring g_face = L"Segoe UI";
Table        g_tab;                                 // the active texts (empty = English)
uint32_t     g_gen = 1;
// what the last Refresh loaded (a new load only when one of these changes)
std::string  g_loadedFor;                           // wanted code of the last load
FILETIME     g_ovTime{};
uint64_t     g_ovSize = ~0ull;
// game.log.txt / config.cfg reading (auto)
FILETIME     g_glTime{};
uint64_t     g_glSize = ~0ull;
std::string  g_gameLang;

bool DllDir(std::wstring& dir) {
    wchar_t p[MAX_PATH];
    const DWORD n = GetModuleFileNameW(SelfModule(), p, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(p, L'\\');
    if (!slash) return false;
    slash[1] = 0;
    dir = p;
    return true;
}
bool FileStamp(const std::wstring& path, FILETIME& t, uint64_t& size) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    t = a.ftLastWriteTime;
    size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return true;
}
bool SameTime(const FILETIME& a, const FILETIME& b) { return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime; }

std::string Lower(std::string s) { for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }
bool ValidCode(const std::string& c) {             // "xx_yy" (letters, 5 characters)
    if (c.size() != 5 || c[2] != '_') return false;
    for (size_t i = 0; i < 5; ++i) if (i != 2 && !(c[i] >= 'a' && c[i] <= 'z')) return false;
    return true;
}

// The game's profile folder (Documents\Euro Truck Simulator 2 or \American Truck Simulator, by the exe name).
bool GameDocs(std::wstring& dir) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    const wchar_t* base = wcsrchr(exe, L'\\');
    base = base ? base + 1 : exe;
    const wchar_t* game = !_wcsicmp(base, L"amtrucks.exe") ? L"American Truck Simulator" : L"Euro Truck Simulator 2";
    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) || !docs) { if (docs) CoTaskMemFree(docs); return false; }
    dir = docs;
    CoTaskMemFree(docs);
    dir += L'\\';
    dir += game;
    dir += L'\\';
    return true;
}
// The value of the LAST `<key> "xx_yy"` in `text` (key includes the trailing space + quote), "" = none.
std::string LastQuoted(const std::string& text, const char* key) {
    std::string out;
    size_t pos = 0;
    const size_t kl = strlen(key);
    while ((pos = text.find(key, pos)) != std::string::npos) {
        pos += kl;
        const size_t e = text.find('"', pos);
        if (e != std::string::npos && e - pos < 16) out = Lower(text.substr(pos, e - pos));
    }
    return out;
}
std::string ReadAll(const std::wstring& path, size_t cap) {
    std::string data;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return data;
    char buf[65536];
    DWORD got = 0;
    while (data.size() < cap && ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) data.append(buf, got);
    CloseHandle(h);
    return data;
}
// auto: the game's language. game.log.txt (this session: "uset g_lang "xx"" when the player set one, else the game's
// "Selected language: xx" line) wins; else config.cfg g_lang, then g_lang_init; else English.
std::string DetectGame() {
    std::wstring docs;
    if (!GameDocs(docs)) return kEnglish;
    const std::wstring gl = docs + L"game.log.txt";
    FILETIME t{};
    uint64_t sz = 0;
    if (FileStamp(gl, t, sz)) {
        if (sz == g_glSize && SameTime(t, g_glTime) && !g_gameLang.empty()) return g_gameLang;
        const std::string log = ReadAll(gl, 64u << 20);
        std::string c = LastQuoted(log, "uset g_lang \"");
        if (!ValidCode(c)) {
            const size_t p = log.rfind("Selected language: ");
            if (p != std::string::npos) {
                c = Lower(log.substr(p + 19, 5));
            }
        }
        if (ValidCode(c)) { g_glTime = t; g_glSize = sz; g_gameLang = c; return c; }
    }
    const std::string cfg = ReadAll(docs + L"config.cfg", 1u << 20);
    std::string c = LastQuoted(cfg, "uset g_lang \"");
    if (!ValidCode(c)) c = LastQuoted(cfg, "uset g_lang_init \"");
    return ValidCode(c) ? c : std::string(kEnglish);
}

// ---- fonts -------------------------------------------------------------------------------------------------------------
// Faces tried in order; Windows ships the first of each list. The Linux names are what Proton prefixes / fontconfig often
// have (protontricks cjkfonts makes the Windows names resolve too).
const wchar_t* const kFacesLatin[] = { L"Segoe UI", L"Tahoma", L"Arial", L"DejaVu Sans", L"Liberation Sans", L"Noto Sans", nullptr };
const wchar_t* const kFacesJa[] = { L"Yu Gothic UI", L"Meiryo UI", L"Meiryo", L"MS UI Gothic", L"MS Gothic", L"Noto Sans CJK JP",
                                    L"Noto Sans JP", L"Source Han Sans JP", L"IPAGothic", L"TakaoGothic", L"VL Gothic", nullptr };
const wchar_t* const kFacesKo[] = { L"Malgun Gothic", L"Gulim", L"Dotum", L"Noto Sans CJK KR", L"Noto Sans KR", L"NanumGothic",
                                    L"UnDotum", nullptr };
const wchar_t* const kFacesZh[] = { L"Microsoft YaHei UI", L"Microsoft YaHei", L"SimHei", L"SimSun", L"Noto Sans CJK SC",
                                    L"Noto Sans SC", L"WenQuanYi Micro Hei", L"WenQuanYi Zen Hei", L"Droid Sans Fallback", nullptr };
const wchar_t* const kFacesZhTw[] = { L"Microsoft JhengHei UI", L"Microsoft JhengHei", L"PMingLiU", L"MingLiU", L"Noto Sans CJK TC",
                                      L"Noto Sans TC", L"WenQuanYi Micro Hei", L"WenQuanYi Zen Hei", L"Droid Sans Fallback", nullptr };
const wchar_t* const kFacesKa[] = { L"Segoe UI", L"Sylfaen", L"DejaVu Sans", L"Noto Sans Georgian", L"Noto Sans", nullptr };
const wchar_t* const* FacesFor(const std::string& code) {
    if (!code.compare(0, 2, "ja")) return kFacesJa;
    if (!code.compare(0, 2, "ko")) return kFacesKo;
    if (code == "zh_tw") return kFacesZhTw;
    if (!code.compare(0, 2, "zh")) return kFacesZh;
    if (!code.compare(0, 2, "ka")) return kFacesKa;
    return kFacesLatin;
}
// The first face that has a glyph for every character of t's texts; else the one missing the fewest when that is <= 1 %;
// else nullptr (the font is missing).
const wchar_t* PickFace(const std::string& code, const Table& t, int& missing, int& chars) {
    std::vector<wchar_t> set(t.arena);
    std::sort(set.begin(), set.end());
    set.erase(std::unique(set.begin(), set.end()), set.end());
    set.erase(std::remove_if(set.begin(), set.end(), [](wchar_t c) { return c < 0x21 || (c >= 0xD800 && c <= 0xDFFF); }), set.end());
    chars = (int)set.size();
    missing = chars;
    if (set.empty()) return FacesFor(code)[0];
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return FacesFor(code)[0];
    std::vector<WORD> gi(set.size());
    const wchar_t* best = nullptr;
    for (const wchar_t* const* f = FacesFor(code); *f; ++f) {
        HFONT h = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, *f);
        if (!h) continue;
        HGDIOBJ old = SelectObject(dc, h);
        int miss = (int)set.size();
        if (GetGlyphIndicesW(dc, set.data(), (int)set.size(), gi.data(), GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR) {
            miss = 0;
            for (WORD g : gi) if (g == 0xFFFF) ++miss;
        }
        SelectObject(dc, old);
        DeleteObject(h);
        if (miss < missing) { missing = miss; best = *f; }
        if (!miss) break;
    }
    DeleteDC(dc);
    return best && missing * 100 <= chars ? best : nullptr;
}

std::unordered_map<std::string, std::wstring> g_widened;   // TrA: the widened English texts (by content)
std::unordered_map<uint64_t, bool>            g_fmtWarned;  // TrF: one log line per rejected format

}  // namespace

uint32_t Gen() { return g_gen; }

const wchar_t* Tr(const wchar_t* en) {
    if (!en || !*en || g_tab.idx.empty()) return en;
    const wchar_t* t = g_tab.Find(Hash(en));
    return t ? t : en;
}
const wchar_t* TrF(const wchar_t* en) {
    const wchar_t* t = Tr(en);
    if (t == en || Specs(en) == Specs(t)) return t;
    const uint64_t h = Hash(en);
    if (!g_fmtWarned[h]) {
        g_fmtWarned[h] = true;
        char e8[200] = "";
        WideCharToMultiByte(CP_UTF8, 0, en, -1, e8, (int)sizeof(e8), nullptr, nullptr);
        e8[sizeof(e8) - 1] = 0;
        LogErr("language %s: the translation of \"%s\" has other %% placeholders than the English text -- English is shown",
               g_activeCode.c_str(), e8);
    }
    return en;
}
const wchar_t* TrA(const char* en) {
    if (!en) return L"";
    auto it = g_widened.find(en);
    if (it == g_widened.end()) it = g_widened.emplace(en, Widen(en)).first;
    return Tr(it->second.c_str());
}

void SetSetting(const char* v) {
    std::string s = Lower(v ? v : "");
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    s.erase(0, b);
    if (s == "en_gb" || s == "english") s = kEnglish;
    g_setting = ValidCode(s) ? s : "auto";
}
const char* Setting() { return g_setting.c_str(); }

void Refresh() {
    LoadPack();
    std::string want = g_setting == "auto" ? DetectGame() : g_setting;
    if (want == "en_gb") want = kEnglish;
    std::wstring dir, ov;
    FILETIME ovT{};
    uint64_t ovS = ~0ull;
    if (want != kEnglish && DllDir(dir)) {
        ov = dir + L"lang\\" + Widen(want.c_str()) + L".po";
        if (!FileStamp(ov, ovT, ovS)) ovS = ~0ull;
    }
    if (want == g_loadedFor && ovS == g_ovSize && (ovS == ~0ull || SameTime(ovT, g_ovTime))) return;   // nothing changed
    g_loadedFor = want;
    g_ovTime = ovT;
    g_ovSize = ovS;
    g_wanted = want;
    Table t;
    t.code = want;
    int packed = 0, added = 0, rejected = 0;
    bool haveOv = false;
    if (want != kEnglish) {
        if (const PackLang* pl = FindPack(want)) {
            for (uint32_t i = 0; i < pl->nEnt; ++i) {
                const uint8_t* e = g_pack + pl->entOff + (size_t)i * 12;
                const uint32_t so = Rd32(e + 8);
                if (so >= g_packSize) continue;
                const char* s = reinterpret_cast<const char*>(g_pack + so);
                t.Add(Rd64(e), Widen(s, (int)strnlen(s, g_packSize - so)));
                ++packed;
            }
        }
        if (ovS != ~0ull) haveOv = LoadPo(ov.c_str(), t, added, rejected);
        t.Finish();
    }
    int missing = 0, chars = 0;
    const wchar_t* face = L"Segoe UI";
    bool fontMissing = false;
    if (!t.idx.empty()) {
        face = PickFace(want, t, missing, chars);
        if (!face) { fontMissing = true; face = L"Segoe UI"; }
    }
    g_fontMissing = fontMissing;
    g_face = face;
    g_activeCode = (t.idx.empty() || fontMissing) ? kEnglish : want;
    if (t.idx.empty() || fontMissing) g_tab = Table(); else g_tab = std::move(t);
    g_fmtWarned.clear();
    ++g_gen;
    char face8[64] = "";
    WideCharToMultiByte(CP_UTF8, 0, face, -1, face8, (int)sizeof(face8), nullptr, nullptr);
    if (want == kEnglish)
        Log("language: English (setting %s)", g_setting.c_str());
    else if (fontMissing)
        LogErr("language %s (setting %s): NO FONT on this PC has its letters (%d of %d characters missing in the best one) -- the "
               "menu shows English. Linux / Proton: install the fonts, see the README section \"Menu languages and fonts\"",
               want.c_str(), g_setting.c_str(), missing, chars);
    else if (!packed && !haveOv)
        Log("language %s (setting %s): no translation in this build -- English", want.c_str(), g_setting.c_str());
    else
        Log("language %s (setting %s): %d packed texts%s, font %s%s", want.c_str(), g_setting.c_str(), packed,
            haveOv ? " + the lang file next to the DLL" : "", face8, missing ? " (a few characters missing)" : "");
    if (haveOv) Log("language %s: lang file next to the DLL: %d texts used, %d not readable", want.c_str(), added, rejected);
}

bool LoadForTest(const wchar_t* poPath, const char* code) {
    Table t;
    int added = 0, rejected = 0;
    if (!LoadPo(poPath, t, added, rejected)) return false;
    t.Finish();
    t.code = code;
    int missing = 0, chars = 0;
    const wchar_t* face = PickFace(code, t, missing, chars);
    g_fontMissing = !face;
    g_face = face ? face : L"Segoe UI";
    g_activeCode = face ? code : kEnglish;
    g_wanted = code;
    if (face) g_tab = std::move(t); else g_tab = Table();
    g_fmtWarned.clear();
    ++g_gen;
    return true;
}

const char* Active() { return g_activeCode.c_str(); }
const char* Wanted() { return g_wanted.c_str(); }
bool FontMissing() { return g_fontMissing; }
const wchar_t* Face() { return g_face.c_str(); }
int Count() { LoadPack(); return 2 + (int)g_packLangs.size(); }
const char* CodeAt(int i) {
    LoadPack();
    if (i <= 0) return "auto";
    if (i == 1) return kEnglish;
    return i - 2 < (int)g_packLangs.size() ? g_packLangs[(size_t)i - 2].code.c_str() : "auto";
}
const wchar_t* NativeName(const char* code) {
    LoadPack();
    if (!code || !strcmp(code, kEnglish) || !strcmp(code, "en_gb")) return L"English";
    for (const PackLang& l : g_packLangs) if (l.code == code && !l.name.empty()) return l.name.c_str();
    return TrA(code);
}

}  // namespace Lang
