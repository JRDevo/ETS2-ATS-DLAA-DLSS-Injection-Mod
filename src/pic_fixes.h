// PicFixes -- phase 112: the menu's "Picture preset" row (Picture group; dlaa.ini picture_preset; key_picture_preset). Pure logic, no
// D3D: inject.cpp (the row, the key, LoadConfig / SaveSettings / ResetSettings) and tests/settings_roundtrip_test.cpp use it.
// Phase 112c (the user, 2026-11-15): renamed "Picture fixes" -> "Picture preset", "Standard" -> "Recommended" (dlaa.ini picture_preset
// = recommended | off | custom; the OLD key picture_fixes and the old value standard are still read; Save writes the new key and
// removes the old one; key_picture_fixes is read as key_picture_preset). The Glass and fence fix (mv_fwd_depth) joined the set (its
// Recommended value = the Quality preset's: high / medium on, low off -- g_stdFwd), and the Far fence and leaf fix (cutout_mips) is in
// Recommended now (user confirmed far fences OK 2026-11-01).
//
// WHY (the user, 2026-11-15): "There are so many fixes and menu items now -- thin pole fix, wire widening, this fix, that fix. It's
// getting overwhelming. There needs to be a standard: this improves this; if you have issues, disable it. Good for live comparison
// in game." So the extra fixes the mod adds against flicker / ghosting / shimmer artefacts are ONE preset:
//   Recommended every fix at its TESTED default (= the code defaults: nothing changes for a player who never touched one)
//   Off       every fix in the set off, LIVE, to compare (DLAA / DLSS itself stays on: this is about the extra fixes, not Mode)
//   Custom    the player's own values: the individual fix rows are shown (hidden otherwise) and set one by one
// A hand change of one fix (its row, its own hotkey, or a value in dlaa.ini that differs from Standard) makes it Custom. The key
// (key_picture_fixes) switches Standard <-> Off; from Custom it goes to Off and the next press returns to the SAME Custom values.
//
// NOT in the set (core settings, outside the preset): Mode, the DLAA model, Quality preset (perf_profile -- it owns Smooth mirrors,
// Track moving parts, and gives the Glass and fence fix its Recommended value), sharpening (its own Sharpen preset), texture sharpness, DLAA area,
// motion blur (an effect, not a fix), frame rate / VR / performance / logging, the census. docs/DEVELOPMENT.md "Phase 112" has the
// whole inventory with the reason per key.
//
// dlaa.ini picture_fixes = standard | off | custom. LOAD rule (Resolve): no key -> standard when every fix value in the file (the
// default where a key is absent) is the Standard one, else custom (an old dlaa.ini keeps its values); custom -> custom; standard ->
// standard when the file agrees, else custom (a value the player wrote is never changed); off -> off, and the values in the file are
// the set the key returns to (standard when they match it, else custom). SAVE writes picture_fixes and, only for a Custom set
// (Custom, or Off on top of Custom), every fix key; Standard removes the fix keys (the preset carries them). Reset -> Standard.
#pragma once
#include <cstring>

namespace PicFixes {

enum : int { kStandard = 0, kOff, kCustom, kCount };
constexpr int kRecommended = kStandard;          // phase 112c: the player-facing name (dlaa.ini "recommended"; "standard" still read)

// the fixes in the preset (the order of the menu's fix rows)
enum : int {
    kBias = 0,      // dlss_bias_mask       phase 49   Light trail fix (lamp / car-light glow trails)            row + 25 % steps
    kShadow,        // shadow_lock          phase 103  Steady own shadow                                          row + key_shadow_lock
    kPole,          // pole_aa              phase 109d Thin pole fix                                              row + key_pole_aa
    kWireMv,        // dlaa_mv_dilate       phase 60   Wire fix: motion                                           row + key_mv_dilate
    kWireDepth,     // mv_fwd_conservative  phase 60   Wire fix: depth (0 / 1 / 2)                                row + key_mv_conservative
    kWireWiden,     // wire_widen           phase 61   Thicker wires                                              row + key_wire_widen
    kPowerLine,     // power_line_fix       phase 109b Power line fix                                             row (phase 112)
    kPixelReader,   // pixel_reader_fix     phase 109a Lit edge fix (twins built at the start)                    row (phase 112)
    kLightRedraw,   // light_redraw_jitter  phase 55   Lit surface fix (tent canopy / glow redraws)               row (phase 112)
    kLightFs,       // light_fs_jitter      phase 78   Roof shadow fix (full-screen sun light)                    row (phase 112)
    kWater,         // water_overscan_fix   phase 97   Water block fix (the new SCS water)                        row (phase 112)
    kOutsideAa,     // outside_aa           phase 81   Edge outside the DLAA area (VR, 0 / 1 / 2)                 row (phase 112)
    kCutout,        // cutout_mips          phase 77   Far fence and leaf fix -- STARTUP ONLY (texture uploads)   row (phase 112)
    kFwd,           // mv_fwd_depth         v0.9.0 r5  Glass and fence fix (Recommended = the Quality preset's)   row + key_fwd_depth (112c)
    kFixCount
};

// ONE table. `standard` = the value in Standard, `off` = the value in Off. A fix whose standard == off is NOT in Standard yet (an
// experimental opt-in: off in both, set it in Custom). Moving a fix into Standard = its `standard` value here (one line).
//   pole_aa and wire_widen move into Standard after the user's flat + VR test (pole_aa: standard 1; wire_widen: standard 1).
//   cutout_mips: 0 since phase 77 (the 77b car sparkles); phase 112c: 1 = in Recommended (far fences confirmed OK by the user
//   2026-11-01; the risk is loader stutter on streaming bursts, which the 109r wait budget caps; still needs a restart to change).
//   mv_fwd_depth: its `standard` here is the high profile's; the live Recommended value is g_stdFwd (the Quality preset's).
// live = the row / the preset switch it while the game runs; cutout_mips is the only one that waits for the next start.
struct Def { const char* key; int standard; int off; int hi; bool live; };
constexpr Def kDefs[kFixCount] = {
    { "dlss_bias_mask",      100, 0, 100, true  },
    { "shadow_lock",           1, 0,   1, true  },
    { "pole_aa",               0, 0,   1, true  },   // experimental opt-in: moves into Standard after the user's flat + VR test
    { "dlaa_mv_dilate",        1, 0,   1, true  },
    { "mv_fwd_conservative",   1, 0,   2, true  },
    { "wire_widen",            0, 0,   1, true  },   // experimental opt-in: moves into Standard after the user's flat + VR test
    { "power_line_fix",        1, 0,   1, true  },
    { "pixel_reader_fix",      1, 0,   1, true  },   // live off / on while its twins exist (built at the start unless Custom 0)
    { "light_redraw_jitter",   1, 0,   1, true  },
    { "light_fs_jitter",       1, 0,   1, true  },
    { "water_overscan_fix",    1, 0,   1, true  },
    { "outside_aa",            2, 0,   2, true  },
    { "cutout_mips",           1, 0,   1, false },   // phase 112c: in Recommended (code default 1); startup only: textures patched at load
    { "mv_fwd_depth",          1, 0,   1, true  },   // phase 112c: Recommended = g_stdFwd (the Quality preset: high / medium 1, low 0)
};
// phase 112c: the Glass and fence fix's Recommended value = the active Quality preset's (inject.cpp ApplyProfile sets it; 1 = high)
inline int g_stdFwd = 1;
inline int StandardOf(int f) { return f == kFwd ? (g_stdFwd ? 1 : 0) : ((f >= 0 && f < kFixCount) ? kDefs[f].standard : 0); }

struct Set { int v[kFixCount]; };

inline int Clamp(int f, int v) { const int hi = (f >= 0 && f < kFixCount) ? kDefs[f].hi : 1; return v < 0 ? 0 : (v > hi ? hi : v); }
inline Set Standard() { Set s{}; for (int f = 0; f < kFixCount; ++f) s.v[f] = StandardOf(f); return s; }
inline Set Off()      { Set s{}; for (int f = 0; f < kFixCount; ++f) s.v[f] = kDefs[f].off; return s; }
inline bool Equal(const Set& a, const Set& b) { for (int f = 0; f < kFixCount; ++f) if (a.v[f] != b.v[f]) return false; return true; }
inline bool InStandard(int f) { return f >= 0 && f < kFixCount && StandardOf(f) != kDefs[f].off; }
// the values the live switches follow for preset p (`custom` = the Custom set)
inline Set Live(int p, const Set& custom) { return p == kOff ? Off() : (p == kCustom ? custom : Standard()); }
// phase 112r (perf audit): fixes the LIVE Off switch leaves as they are. dlaa_mv_dilate (Wire fix: motion) is an NGX creation flag:
// switching it recreates every DLSS feature (46 ms per unit flat, 100+ ms in VR, + a history reset) -- on every Recommended <-> Off
// press. Off keeps its current value; it is switched in Custom (its row / key_mv_dilate). Off() itself stays "every fix 0" (Save,
// the table, the tests); OffOver() is what the row / key / a start with picture_preset = off apply.
inline bool OffKeeps(int f) { return f == kWireMv; }
inline Set OffOver(const Set& cur) { Set s = Off(); for (int f = 0; f < kFixCount; ++f) if (OffKeeps(f)) s.v[f] = cur.v[f]; return s; }
// Live() for a preset SWITCH: `cur` = the values in use now (Off keeps its OffKeeps fixes at them)
inline Set LiveOver(int p, const Set& custom, const Set& cur) { return p == kOff ? OffOver(cur) : Live(p, custom); }
// the preset the values match: Standard, else Custom (an all-off set is Custom too: Off is a mode, never guessed from values)
inline int Match(const Set& s) { return Equal(s, Standard()) ? kStandard : kCustom; }

inline const char* IniName(int p) {
    static const char* const k[kCount] = { "recommended", "off", "custom" };
    return k[(p < 0 || p >= kCount) ? kCustom : p];
}
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
    for (int p = 0; p < kCount; ++p) if (!strcmp(t, IniName(p))) return p;
    if (!strcmp(t, "standard")) return kStandard;  // phase 112c: the old name of Recommended
    return -1;
}
// Left / Right on the row: Recommended -> Off -> Custom -> Standard (wraps both ways)
inline int Next(int p, int dir) {
    if (p < 0 || p >= kCount) p = kCustom;
    return (p + (dir < 0 ? kCount - 1 : 1)) % kCount;
}
// the key: Standard / Custom -> Off; Off -> the preset it came from (`back`: Standard or Custom)
inline int KeyNext(int p, int back) { return p == kOff ? (back == kCustom ? kCustom : kStandard) : kOff; }
// the LOAD rule (see the top). `iniPreset` = Parse() of the key, -2 = no key, -1 = not recognised (read as no key). `file` = the fix
// values LoadConfig read (the default where a key is absent). *back = the preset Off returns to (Standard / Custom).
inline int Resolve(int iniPreset, const Set& file, int* back) {
    const int m = Match(file);
    if (back) *back = m;
    if (iniPreset == kOff) return kOff;
    if (iniPreset == kCustom) { if (back) *back = kCustom; return kCustom; }
    return m;                                    // no key / standard: Standard only when the file agrees, else Custom
}

} // namespace PicFixes
