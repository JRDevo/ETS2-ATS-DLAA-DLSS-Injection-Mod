// GroupPreset -- phase 112c: a small "Recommended / Custom" preset for a menu group's expert rows (the user, 2026-11-15: "try and
// make as many preset areas as possible"). Pure logic, no D3D: inject.cpp (the rows, LoadConfig / SaveSettings / ResetSettings)
// and tests/settings_roundtrip_test.cpp use it. The Picture preset (pic_fixes.h, + Off) and the Sharpen preset (sharp_preset.h,
// named levels) keep their own headers: their sets have more than two states.
//
// ONE constexpr table per preset (kDefs below): the dlaa.ini key of the preset, and per value its dlaa.ini key and its Recommended
// value (= the code default: choosing Recommended never changes anything for a player who never touched a row). The rows of the
// values are shown ONLY with Custom, directly under the preset row (hidden, never grey).
//   Recommended  every value at its code default (the row sets them live)
//   Custom       the player's own values (the rows shown); a fresh Custom starts from the values in use
// A hand change of one value (its row; inject.cpp GrpTouched) makes it Custom.
// dlaa.ini <preset key> = recommended | custom. LOAD rule (Resolve): no key -> recommended when every value read (the default where
// a key is absent) is the Recommended one, else custom (an old dlaa.ini keeps its values); custom -> custom; recommended -> recommended
// when the file agrees, else custom (a value the player wrote is never changed). SAVE writes the preset key; the value keys are
// written as before (their own explicit rules). Reset -> Recommended (= the code defaults Reset sets anyway).
#pragma once
#include <cstring>

namespace GroupPreset {

enum : int { kRecommended = 0, kCustom, kCount };

enum : int { kVrs = 0, kPanel, kPresetCount };     // the presets (the Foveated detail rows; the VR panel's depth / facing)
constexpr int kMaxVals = 4;

struct Def {
    const char* key;                               // dlaa.ini key of the preset
    int         n;                                 // values used
    const char* valKey[kMaxVals];                  // dlaa.ini key of each value (documentation, logs, tests)
    int         rec[kMaxVals];                     // the Recommended value (= the code default)
};
// Foveated (Performance group, VR): vrs_inner / vrs_outer (the Foveated size row), vrs_passes (Foveated: lighting too),
// vrs_strength (Foveated strength). NOT vrs (the on / off row, the group's main switch) and NOT vrs_eye (eye tracking: a headset
// capability the player must be able to see and switch). Panel (Menu panel group, VR): menu_vr_depth / menu_vr_dist /
// menu_vr_lock_dist (the Panel depth row: one of them per path) and menu_vr_face (Panel faces you).
constexpr Def kDefs[kPresetCount] = {
    { "vrs_preset",   4, { "vrs_inner", "vrs_outer", "vrs_passes", "vrs_strength" },          { 55, 80, 0, 0 } },
    { "panel_preset", 4, { "menu_vr_depth", "menu_vr_dist", "menu_vr_lock_dist", "menu_vr_face" }, { 12, 100, 80, 1 } },
};

struct Vals { int v[kMaxVals]; };

inline Vals Recommended(int g) {
    Vals r{};
    if (g >= 0 && g < kPresetCount) for (int i = 0; i < kDefs[g].n; ++i) r.v[i] = kDefs[g].rec[i];
    return r;
}
inline bool IsRecommended(int g, const Vals& v) {
    if (g < 0 || g >= kPresetCount) return false;
    for (int i = 0; i < kDefs[g].n; ++i) if (v.v[i] != kDefs[g].rec[i]) return false;
    return true;
}
inline int Match(int g, const Vals& v) { return IsRecommended(g, v) ? kRecommended : kCustom; }
inline const char* IniName(int p) { return p == kRecommended ? "recommended" : "custom"; }
// dlaa.ini text -> preset, -1 = not recognised (case-insensitive; a trailing comment ignored)
inline int Parse(const char* s) {
    if (!s) return -1;
    char t[16] = {};
    int n = 0;
    while (*s == ' ' || *s == '\t') ++s;
    for (; *s && *s != '\r' && *s != '\n' && *s != '#' && *s != ';' && *s != ' ' && *s != '\t' && n < 15; ++s) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        t[n++] = c;
    }
    if (!strcmp(t, "recommended")) return kRecommended;
    if (!strcmp(t, "custom")) return kCustom;
    return -1;
}
// Left / Right on the row: two states, both keys switch
inline int Next(int p) { return p == kRecommended ? kCustom : kRecommended; }
// the LOAD rule (see the top). iniPreset = Parse() of the key, -2 = no key, -1 = not recognised (read as no key)
inline int Resolve(int g, int iniPreset, const Vals& file) {
    if (iniPreset == kCustom) return kCustom;
    return Match(g, file);                         // no key / recommended: Recommended only when the file agrees, else Custom
}

} // namespace GroupPreset
