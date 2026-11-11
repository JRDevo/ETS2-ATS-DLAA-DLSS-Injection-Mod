// Phase 94 (issue #31): which OTHER mods run with the game, written to dlaa_inject.log.
// A crash that only one user gets is often a mix with another tool: a telemetry plugin (bin\win_x64\plugins), a game mod,
// an overlay or injector DLL. The bug report zip (Insert) already had the DLL list (sysinfo.txt) and the game.log.txt tail, but
// a crash at game close never gets a capture -- so the plain log now has the list too ("other mods:" lines).
// This header holds the pure text helpers (unit test: tests/other_mods_test.cpp); the file / module reads are in inject.cpp
// (namespace OtherMods, a low-priority thread: no work on the render thread, no work at exit).
#pragma once
#include <string>
#include <vector>
#include <cctype>
#include <cstring>

namespace OtherModsText {

inline bool ContainsI(const std::string& s, const char* n) {
    const size_t nl = strlen(n);
    if (!nl || s.size() < nl) return false;
    for (size_t i = 0; i + nl <= s.size(); ++i) {
        size_t k = 0;
        while (k < nl && tolower((unsigned char)s[i + k]) == tolower((unsigned char)n[k])) ++k;
        if (k == nl) return true;
    }
    return false;
}

// The game.log.txt lines that name a mod or a plugin, without the "00:00:14.132 : " time stamp, in file order:
//   "[mods] Active 1 mods (local: 0, workshop: 1)", "[mods] Active workshop mod ID ... (name: ...)", every line with
//   "plugin" in it (the SCS telemetry SDK plugins). Config lines ("uset ...") are left out.
inline std::vector<std::string> ModLines(const std::string& text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        std::string ln = text.substr(i, e - i);
        i = e + 1;
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == ' ')) ln.pop_back();
        const size_t ts = ln.find(" : ");                // "00:00:14.132 : " (the game's time stamp)
        if (ts != std::string::npos && ts <= 16) ln = ln.substr(ts + 3);
        if (ln.empty() || ln.compare(0, 5, "uset ") == 0) continue;
        if (ln.compare(0, 6, "[mods]") == 0 || ContainsI(ln, "plugin")) {
            if (ln.size() > 600) ln = ln.substr(0, 600) + "...";
            out.push_back(ln);
        }
    }
    return out;
}

// true = `s` is not in `seen` yet (and is added): each line is logged once per run.
inline bool FirstTime(std::vector<std::string>& seen, const std::string& s) {
    for (const std::string& x : seen) if (x == s) return false;
    seen.push_back(s);
    return true;
}

}  // namespace OtherModsText
