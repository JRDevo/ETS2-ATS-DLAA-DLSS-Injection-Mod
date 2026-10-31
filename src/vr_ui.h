// VrUi -- phase 83: the game's in-game VR UI placement, live from the mod's menu (group "In-game UI", VR only). Pure logic, no D3D / no
// Win32: inject.cpp feeds it, dinput_wrap.cpp delivers the keys, tests\vr_ui_test.cpp drives it.
//
// The game places its VR UI with console variables (config.cfg, `uset <var> "<value>"`): r_manual_stereo_ui_<field>_game while
// driving and r_manual_stereo_ui_<field> on the menu screens, fields y (height), x (side), dist (distance), pitch (tilt), yaw (turn),
// fov (size) and radius (curve). Typing `uset r_manual_stereo_ui_y_game "-0.4"` into the game's console moves the HUD at once (user
// test, ATS). The mod has no other way to set a game cvar, so it TYPES the command into the console: open it (the console key),
// type the text, Enter, close it -- KeyPlan builds that step list, the render thread plays it (inject.cpp VrUiTick) and dinput_wrap
// delivers each step to the game (phase 85: SendInput by default, KeySwallow::SendScanKey / SendUnicodeChar; see kKeys*). Values come from config.cfg (start, and when the group opens)
// and are never written there or to dlaa.ini: the game saves its own cvars when it exits.
// The console needs g_developer "1" and g_console "1" in config.cfg (ConsoleEnabled); without them the rows are greyed.
#pragma once
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace VrUi {

enum Field : int { kY = 0, kX, kDist, kPitch, kYaw, kFov, kRadius, kFieldCount };
enum Set : int { kDriving = 0, kMenus = 1, kSetCount = 2 };
struct FieldDef { const char* key; float step, lo, hi, def; int decimals; };
// steps / ranges for the menu (the game accepts any float; these keep a press meaningful); def = the game's own default
constexpr FieldDef kFields[kFieldCount] = {
    { "y",      0.05f, -3.0f,   3.0f,  0.0f, 2 },
    { "x",      0.05f, -3.0f,   3.0f,  0.0f, 2 },
    { "dist",   0.10f,  0.2f,  10.0f,  1.0f, 2 },
    { "pitch",  0.05f, -1.6f,   1.6f,  0.0f, 2 },
    { "yaw",    0.05f, -3.15f,  3.15f, 0.0f, 2 },
    { "fov",    1.0f,  10.0f, 120.0f, 50.0f, 0 },
    { "radius", 0.5f,   0.0f,  20.0f,  0.0f, 1 },
};
// the cvar of a field in a set: r_manual_stereo_ui_<key>_game (driving) / r_manual_stereo_ui_<key> (menus)
inline std::string VarName(int set, int field) {
    std::string s = "r_manual_stereo_ui_";
    s += kFields[field].key;
    if (set == kDriving) s += "_game";
    return s;
}

// config.cfg: the value of the LAST `uset <name> "<value>"` line (exact name: `..._y` never matches `..._y_game`); false = none
inline bool ParseCfg(const std::string& text, const std::string& name, float* out) {
    const std::string key = "uset " + name + " \"";
    bool got = false;
    size_t pos = 0;
    while ((pos = text.find(key, pos)) != std::string::npos) {
        const bool lineStart = pos == 0 || text[pos - 1] == '\n' || text[pos - 1] == '\r';
        pos += key.size();
        if (!lineStart) continue;
        const size_t e = text.find('"', pos);
        if (e == std::string::npos || e - pos > 24) continue;
        char buf[32] = {};
        memcpy(buf, text.data() + pos, e - pos);
        char* end = nullptr;
        const float v = strtof(buf, &end);
        if (end != buf && std::isfinite(v)) { *out = v; got = true; }
    }
    return got;
}
// the console is usable: g_developer "1" and g_console "1"
inline bool ConsoleEnabled(const std::string& cfg) {
    float d = 0.0f, c = 0.0f;
    return ParseCfg(cfg, "g_developer", &d) && ParseCfg(cfg, "g_console", &c) && d != 0.0f && c != 0.0f;
}

inline float Clamp(int field, float v) { const FieldDef& f = kFields[field]; return v < f.lo ? f.lo : (v > f.hi ? f.hi : v); }
// one step from v (snapped to the step grid, so a config.cfg value like -0.43 goes to -0.45 / -0.40)
inline float Step(int field, float v, int dir) {
    const float s = kFields[field].step;
    const float g = std::floor(v / s + 0.5f) * s;
    float n = std::fabs(g - v) > s * 0.01f ? (dir > 0 ? (g > v ? g : g + s) : (g < v ? g : g - s)) : g + (dir > 0 ? s : -s);
    const float q = std::pow(10.0f, (float)kFields[field].decimals);   // to the row's decimals: -0.45f, not -0.45000002
    n = std::floor(n * q + 0.5f) / q;
    return Clamp(field, n);
}
// the value as typed into the console ("-0.45", "50", "2.5")
inline std::string Format(int field, float v) {
    char b[32];
    snprintf(b, sizeof(b), "%.*f", kFields[field].decimals, (double)v);
    if (!strcmp(b, "-0") || !strcmp(b, "-0.0") || !strcmp(b, "-0.00")) snprintf(b, sizeof(b), "%.*f", kFields[field].decimals, 0.0);
    return b;
}
inline std::string Command(int set, int field, float v) { return "uset " + VarName(set, field) + " \"" + Format(field, v) + "\""; }

// ---- Debounce + queue: a changed value is sent once it has been stable for kStableMs, one command per changed cvar ----
constexpr uint64_t kStableMs = 400;
struct Pending { int set = 0, field = 0; float value = 0.0f; };
class Debounce {
public:
    // a menu press (or Reset) set a new value now
    void Changed(int set, int field, float v, uint64_t nowMs) { Slot& s = m_s[set][field]; s.want = v; s.dirty = true; s.at = nowMs; }
    // the value the game has (config.cfg at start / a command sent)
    void Known(int set, int field, float v) { Slot& s = m_s[set][field]; s.sent = v; s.haveSent = true; }
    // every value stable for kStableMs that differs from what the game has: returned once (and taken as sent)
    std::vector<Pending> Due(uint64_t nowMs) {
        std::vector<Pending> out;
        for (int a = 0; a < kSetCount; ++a)
            for (int f = 0; f < kFieldCount; ++f) {
                Slot& s = m_s[a][f];
                if (!s.dirty || nowMs - s.at < kStableMs) continue;
                s.dirty = false;
                if (s.haveSent && Format(f, s.sent) == Format(f, s.want)) continue;   // back where it was: nothing to send
                s.sent = s.want; s.haveSent = true;
                out.push_back({ a, f, s.want });
            }
        return out;
    }
    bool AnyDirty() const { for (const auto& r : m_s) for (const Slot& s : r) if (s.dirty) return true; return false; }
private:
    struct Slot { float want = 0.0f, sent = 0.0f; bool dirty = false, haveSent = false; uint64_t at = 0; };
    Slot m_s[kSetCount][kFieldCount];
};

// ---- the keystroke plan: what the render thread plays, step by step ----
// DirectInput scan codes (DIK_*) and virtual keys of the keys used
constexpr uint8_t kDikGrave = 0x29, kDikReturn = 0x1C;
constexpr uint8_t kVkGrave = 0xC0 /* VK_OEM_3 */, kVkReturn = 0x0D;
enum StepKind : int { kKeyDown = 0, kKeyUp, kChar, kWait };
struct KeyStep { int kind = kWait; uint8_t dik = 0, vk = 0; char ch = 0; int ms = 0; };
// how the keys reach the game (dlaa.ini vr_ui_keys; phase 85 renumbered: the phase 83 modes never reached the console, ATS binds it
// on `sys.keyboard` = raw input / window messages):
//   0 SendInput (default): the console key and Enter by scan code (KEYEVENTF_SCANCODE), the text as Unicode characters
//     (KEYEVENTF_UNICODE = VK_PACKET -> WM_CHAR) -- real keyboard input, the game sees it as the player's keys
//   1 SendInput scan codes: also the text as scan codes on the game's keyboard layout (VkKeyScanExW; for a game that reads the
//     characters from raw input, where Unicode events carry no key)
//   2 the phase 83 mix: console key + Enter as DirectInput events on the game's keyboard device, the text as posted WM_CHAR
//   3 posted window messages only, 4 DirectInput events only (letters as US-layout scan codes)
// SendInput goes to the FOREGROUND window: every step waits for the game window to have the focus (all modes).
// Each step carries the key for every path; the player (inject.cpp / dinput_wrap.cpp) delivers what the mode says.
constexpr int kKeysSendInput = 0, kKeysSendScan = 1, kKeysMixed = 2, kKeysWindow = 3, kKeysDirectInput = 4, kKeysModes = 5;
inline const char* KeysName(int m) {
    switch (m) {
    case kKeysSendInput:   return "SendInput: console key + Enter by scan code, the text as Unicode characters";
    case kKeysSendScan:    return "SendInput: everything by scan code (the game's keyboard layout)";
    case kKeysMixed:       return "console key + Enter as DirectInput events, the text as posted WM_CHAR";
    case kKeysWindow:      return "posted window messages only";
    case kKeysDirectInput: return "DirectInput events only";
    default:               return "?";
    }
}
inline bool KeysUseSendInput(int m) { return m == kKeysSendInput || m == kKeysSendScan; }
inline int  ClampKeys(long v) { return v <= 0 ? 0 : (v >= kKeysModes - 1 ? kKeysModes - 1 : (int)v); }

// ---- delivery confirmation: the game echoes each executed console command into Documents\<game>\game.log.txt as
// "<time> : uset <var> "<value>"" (seen for the config.cfg exec at start; typed commands assumed the same -- the log line says
// "NOT confirmed" when it never shows up). `log` = the bytes appended since the batch started.
inline bool Echoed(const std::string& log, const std::string& cmd) {
    size_t pos = 0;
    while ((pos = log.find(cmd, pos)) != std::string::npos) {
        const size_t e = pos + cmd.size();
        const bool endOk = e == log.size() || log[e] == '\r' || log[e] == '\n' || log[e] == ' ';
        const bool startOk = pos == 0 || log[pos - 1] == ' ' || log[pos - 1] == '>' || log[pos - 1] == '\n' || log[pos - 1] == ']';
        if (endOk && startOk) return true;
        pos = e;
    }
    return false;
}
constexpr uint64_t kConfirmMs = 2000;   // the echo must show up within this after the batch's last key

// ---- phase 86: the Game TAA row switches the GAME's anti-aliasing (cvar r_aa: 0 = off, 5 = TAA, as captured; the game's TAA tuning /
// sharpening cvars are never touched). Same delivery as the In-game UI rows (typed into the console, debounced, confirmed in
// game.log.txt), in flat AND VR, only when the console is on, only on a row change (never at startup by itself), only when the game's
// value as the mod knows it (config.cfg at start / menu open, then what the mod typed) differs.
constexpr int kRaaOff = 0, kRaaTaa = 5, kRaaUnknown = -1;
inline int RaaFor(int gameTaaMode) { return gameTaaMode == 0 ? kRaaOff : kRaaTaa; }   // game_taa 0 off -> r_aa 0; mix / split -> 5
// the r_aa value to type for the row's mode, or kRaaUnknown = type nothing (console off, or the game has that value already)
inline int RaaDecide(int gameTaaMode, bool console, int known) {
    if (!console) return kRaaUnknown;
    const int want = RaaFor(gameTaaMode);
    return known == want ? kRaaUnknown : want;
}
inline std::string RaaCommand(int v) { char b[32]; snprintf(b, sizeof(b), "uset r_aa \"%d\"", v); return b; }
// config.cfg's r_aa (kRaaUnknown = not there)
inline int RaaFromCfg(const std::string& cfg) { float v = 0.0f; return ParseCfg(cfg, "r_aa", &v) ? (int)std::lround(v) : kRaaUnknown; }
constexpr int kKeyMs = 50;        // a key held this long (several game input polls at 60-90 fps)
constexpr int kOpenMs = 150;      // after the console key: the console opens
constexpr int kAfterEnterMs = 100;
// one console session for the commands: open, (text, Enter) per command, close
inline std::vector<KeyStep> KeyPlan(const std::vector<std::string>& cmds) {
    std::vector<KeyStep> p;
    if (cmds.empty()) return p;
    auto key = [&](uint8_t dik, uint8_t vk) {
        KeyStep d; d.kind = kKeyDown; d.dik = dik; d.vk = vk; p.push_back(d);
        KeyStep w; w.kind = kWait; w.ms = kKeyMs; p.push_back(w);
        KeyStep u; u.kind = kKeyUp; u.dik = dik; u.vk = vk; p.push_back(u);
    };
    auto wait = [&](int ms) { KeyStep w; w.kind = kWait; w.ms = ms; p.push_back(w); };
    key(kDikGrave, kVkGrave);
    wait(kOpenMs);
    for (const std::string& c : cmds) {
        for (char ch : c) { KeyStep s; s.kind = kChar; s.ch = ch; p.push_back(s); }
        wait(kKeyMs);
        key(kDikReturn, kVkReturn);
        wait(kAfterEnterMs);
    }
    key(kDikGrave, kVkGrave);
    wait(kKeyMs);
    return p;
}
// US-layout scan code of a character for the DirectInput-only mode (0 = none) and whether it needs Shift
inline uint8_t DikOfChar(char c, bool* shift) {
    *shift = false;
    static const char* const kRow1 = "1234567890-";       // DIK 0x02..0x0C
    static const char* const kQ = "qwertyuiop", * const kA = "asdfghjkl", * const kZ = "zxcvbnm";
    if (c >= 'A' && c <= 'Z') { *shift = true; c = (char)(c - 'A' + 'a'); }
    if (c == '_') { *shift = true; return 0x0C; }
    if (c == '"') { *shift = true; return 0x28; }
    if (c == ' ') return 0x39;
    if (c == '.') return 0x34;
    if (const char* q = strchr(kRow1, c)) return (uint8_t)(0x02 + (q - kRow1));
    if (const char* q = strchr(kQ, c)) return (uint8_t)(0x10 + (q - kQ));
    if (const char* q = strchr(kA, c)) return (uint8_t)(0x1E + (q - kA));
    if (const char* q = strchr(kZ, c)) return (uint8_t)(0x2C + (q - kZ));
    return 0;
}
constexpr uint64_t kUserQuietMs = 2000;   // no keys typed by the player for this long before the mod types (console / chat in use)

}  // namespace VrUi
