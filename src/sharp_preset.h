// SharpPreset -- phase 105b: the menu's "Sharpen preset" row (Picture group; dlaa.ini sharp_preset). Pure logic, no D3D:
// inject.cpp (the row, LoadConfig / SaveSettings / ResetSettings) and tests/settings_roundtrip_test.cpp use it.
//
// WHY: phase 104 gave RCAS two expert knobs (Halo limit = sharp_halo, Sharpen colour = sharp_luma); players found them confusing.
// Phase 112c (the user, 2026-11-15: "keep sharpen neat and tidy, right now it's just so random"): the preset now FULLY defines the
// sharpening -- strength, filter (sharpen_type), size (sharp_radius), halo limit and colour -- and comes FIRST in the sharpen block;
// the five value rows (Sharpen strength, Sharpen type, Sharpness size, Halo limit, Sharpen colour) are shown only with Custom,
// directly under it. Every built-in preset uses RCAS at size 1.5 px (the code defaults: RCAS is the filter the halo / colour
// values were tuned on; NIS has no size, halo or colour knob) -- pick Custom for NIS or another size.
//   Off           strength 0 (the sharpen pass is skipped)
//   Soft          strength 0.4, halo  0 %, colour           (= the fresh-install values: a new install starts on Soft)
//   Balanced      strength 0.8, halo  0 %, colour
//   Crisp         strength 1.2, halo 10 %, brightness only
//   Extra crisp   strength 1.6, halo 20 %, brightness only
//   Custom        nothing is changed: the values are the player's own (the five rows shown)
// A hand change of any of the five values (row, hotkey Shift+F7 / F8 / F9 / F10) while a named preset is active makes it Custom
// (inject.cpp SharpPresetTouched).
// dlaa.ini sharp_preset = off | soft | balanced | crisp | extra | custom. LOAD rule (Resolve): no key -> the preset whose values
// match exactly, else custom (an old dlaa.ini keeps its values); custom -> custom; a named preset -> its values fill the keys the
// file does not set, and every key the file DOES set must agree with it, else custom (a value the player wrote is never changed).
#pragma once
#include <cmath>
#include <cstring>

namespace SharpPreset {

enum : int { kOff = 0, kSoft, kBalanced, kCrisp, kExtra, kCustom, kCount };
constexpr int kNamed = kCustom;                  // presets 0 .. kNamed-1 carry values

constexpr int   kTypeRcas = 0, kTypeNis = 1;     // = SceneDlaa::kSharpRcas / kSharpNis
constexpr float kRadius = 1.5f;                  // every named preset: the code default size
struct Values { float strength; int halo; bool luma; int type; float radius; };
constexpr Values kValues[kNamed] = {
    { 0.0f,  0, false, kTypeRcas, kRadius },     // Off (phase 112c)
    { 0.4f,  0, false, kTypeRcas, kRadius },     // Soft
    { 0.8f,  0, false, kTypeRcas, kRadius },     // Balanced
    { 1.2f, 10, true,  kTypeRcas, kRadius },     // Crisp
    { 1.6f, 20, true,  kTypeRcas, kRadius },     // Extra crisp
};

// the five values a preset sets (what LoadConfig read / what runs now)
struct Now { float strength; int halo; bool luma; int type; float radius; };

inline const char* IniName(int p) {
    static const char* const k[kCount] = { "off", "soft", "balanced", "crisp", "extra", "custom" };
    return k[(p < 0 || p >= kCount) ? kCustom : p];
}
// dlaa.ini text -> preset, -1 = not recognised (case-insensitive; "extra crisp" / "extra_crisp" accepted too)
inline int Parse(const char* s) {
    if (!s) return -1;
    char t[24] = {};
    int n = 0;
    while (*s == ' ' || *s == '\t') ++s;
    for (; *s && *s != '\r' && *s != '\n' && *s != '#' && *s != ';' && n < 23; ++s) {
        char c = *s;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c == ' ' || c == '\t' || c == '_' || c == '-') continue;   // "extra crisp" == "extracrisp"
        t[n++] = c;
    }
    if (!strcmp(t, "extracrisp")) return kExtra;
    for (int p = 0; p < kCount; ++p) if (!strcmp(t, IniName(p))) return p;
    return -1;
}
inline bool SameStrength(float a, float b) { return std::fabs(a - b) < 0.05f; }   // the 0.1 grid of the keys and dlaa.ini
inline bool SameRadius(float a, float b) { return std::fabs(a - b) < 0.05f; }
// the values equal preset p. Strength 0 (Off): the pass is skipped, so halo / colour / type / size do not matter for it
inline bool Matches(int p, const Now& v) {
    if (p < 0 || p >= kNamed) return false;
    const Values& k = kValues[p];
    if (p == kOff) return SameStrength(v.strength, 0.0f);
    return SameStrength(v.strength, k.strength) && v.halo == k.halo && v.luma == k.luma && v.type == k.type &&
           SameRadius(v.radius, k.radius);
}
// (phase 105b signature: strength, halo, colour with the type / size at the presets' own -- tests and old callers)
inline bool Matches(int p, float strength, int halo, bool luma) {
    return Matches(p, Now{ strength, halo, luma, kTypeRcas, kRadius });
}
// the named preset the values match exactly, else kCustom
inline int Match(const Now& v) {
    for (int p = 0; p < kNamed; ++p) if (Matches(p, v)) return p;
    return kCustom;
}
// Left / Right on the row: Off -> Soft -> Balanced -> Crisp -> Extra crisp -> Custom -> Off (wraps both ways)
inline int Next(int p, int dir) {
    if (p < 0 || p >= kCount) p = kCustom;
    return (p + (dir < 0 ? kCount - 1 : 1)) % kCount;
}
// have bits: which of the five keys the file set
enum : unsigned { kHaveStrength = 1, kHaveHalo = 2, kHaveLuma = 4, kHaveType = 8, kHaveRadius = 16 };
// the LOAD rule (see the top). `iniPreset` = Parse() of the key, -2 = no key, -1 = not recognised (treated as no key).
// v = what LoadConfig read (defaults where the key is absent). Returns the preset; *apply = true when the caller must set the
// values to kValues[result] (a named preset filling absent keys).
inline int Resolve(int iniPreset, unsigned have, const Now& v, bool* apply) {
    if (apply) *apply = false;
    if (iniPreset < 0) return Match(v);
    if (iniPreset == kCustom) return kCustom;
    const Values& k = kValues[iniPreset];
    bool agree = !(have & kHaveStrength) || SameStrength(v.strength, k.strength);
    if (iniPreset != kOff)                       // (Off: only the strength counts, the rest is the player's for a later Custom)
        agree = agree && (!(have & kHaveHalo) || v.halo == k.halo) && (!(have & kHaveLuma) || v.luma == k.luma) &&
                (!(have & kHaveType) || v.type == k.type) && (!(have & kHaveRadius) || SameRadius(v.radius, k.radius));
    if (!agree) return kCustom;
    if (apply) *apply = true;
    return iniPreset;
}

} // namespace SharpPreset
