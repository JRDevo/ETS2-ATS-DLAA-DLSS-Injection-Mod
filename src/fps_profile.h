// FpsProfile -- per-vehicle fps box placement for the VR "Attached to vehicle" mode (fps_vr_lock = 2; src/fps_cab.h phase 47).
// Design: docs/DEVELOPMENT.md "Phase 51".
//
// VEHICLE KEY: a 64-bit fingerprint of the cab, the same after a game restart. FpsCab's lock-on readback lists the cabin-layer
// draws of both eyes (FpsCab Gather: the 16 biggest per eye). The key is made from the kKeyParts BIGGEST of them (union of both
// eyes, by IndexCount), each as the tuple (IndexCount, VB0 stride, index format) -- geometry numbers only: no buffer / shader
// pointers (they change every run), and no start index / base vertex either (offsets into the game's buffers: phase 47e had to
// fall back to IndexCount alone because the keys of the same part changed mid-session). The tuples are put in a canonical order
// (IndexCount descending, then stride, then format) before hashing, so the order the draws came in (eye, draw order, which part
// was the anchor) does not matter. Known risk: the set of the 3 biggest cab draws must be the same at each lock-on -- a big part
// culled (head turned far away at the lock-on) or a different LOD gives another key (= another profile, it starts from the
// current values). Two vehicles whose 3 biggest cab meshes have the same index counts share one profile.
//
// PROFILE: fps_vr_x, fps_vr_y, fps_vr_dist, fps_vr_yaw, fps_vr_pitch, fps_vr_roll, fps_vr_scale; phase 51b: + fps_color,
// fps_brightness, fps_text and the box's CAB POSE (below). dlaa.ini lines, one per vehicle (cap kMax, the least recently used
// dropped); Rewrite updates those lines in place and keeps every other byte:
//   v1 (phase 51):  fps_profile = <16 hex digits> x y dist yaw pitch roll scale
//   v2 (phase 51b): fps_profile = <16 hex digits> x y dist yaw pitch roll scale v2 color brightness text <cab>
//                   <cab> = "-" (none) or "cab" ic stride fmt a00 a01 a02 a03 a10 .. a13 a20 .. a23 pivotx pivoty pivotz yaw0 fw
//                   [+ ic stride fmt a00 .. a23]... (phase 51c: the same box in up to 2 more cab parts)
//   v3 (phase 51f): fps_profile = <16 hex digits> x y dist yaw pitch roll scale v3 color brightness text opacity <cab>
//                   (v2 + fps_opacity after text; written only when an opacity is stored, else the line stays v2)
//                   phase 51d: "cabsq" instead of "cab" = the pose is SQUARE TO THE CAB (CabPose::level: its virtual frame was
//                   turned onto the cab's own axes at the lock, FpsCab CabLevel); "cab" = an older pose built in the play space of
//                   its lock (levelled once at the next lock-on that has a cab frame, then written back as "cabsq")
// A v1 line still loads (colour / brightness / text = -1: "not stored" -> the live global values are kept and stored at the next
// tick); an entry with nothing of v2 is written back as v1. color / brightness / text / opacity = -1 in a v2 / v3 line = not stored
// either (a v2 line has no opacity: -1, the live global value is kept).
// CAB POSE (phase 51b): the box's placement IN THE CAB, independent of the play space: the play -> model map A (3 x 4 affine
// rows) of one rigid cab part (identified by IndexCount, VB0 stride, index format: the numbers of the vehicle key) and the
// LockFrame (pivot, yaw0, fw) the box is built in. Corners = A * BoxPlay(settings, frame) -- the same settings give the same
// cab-space corners at every lock-on, whatever the pose error of the lock-on frame (FpsCab uses it instead of the lock's own
// play -> cab map when that part is among the lock's rigid parts). Written with 17 significant digits (exact round trip).
// Pure (no Windows / D3D): tests/fps_cab_test.cpp.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "fps_pose.h"

namespace FpsProfile {

constexpr int kKeyParts = 3;                             // the N biggest cab draws make the key
constexpr int kMax = 64;                                 // profiles kept (dlaa.ini lines)
constexpr const char* kIniKey = "fps_profile";

struct Part { uint32_t ic = 0, stride = 0, fmt = 0; };
inline bool PartBefore(const Part& a, const Part& b) {   // the canonical order: IndexCount desc, then stride, then format
    if (a.ic != b.ic) return a.ic > b.ic;
    if (a.stride != b.stride) return a.stride < b.stride;
    return a.fmt < b.fmt;
}
inline bool PartSame(const Part& a, const Part& b) { return a.ic == b.ic && a.stride == b.stride && a.fmt == b.fmt; }
inline uint64_t Mix(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33;
    return x;
}
// The key of a cab from its draws (any order, duplicates allowed: the same draw seen by both eyes counts once). 0 = no key (no
// part with an IndexCount). *used = the parts that made it (canonical order), *nUsed their count (log).
inline uint64_t VehicleKey(const Part* parts, int n, Part* used = nullptr, int* nUsed = nullptr) {
    Part top[kKeyParts];
    int m = 0;
    for (int i = 0; i < n; ++i) {
        const Part& p = parts[i];
        if (!p.ic) continue;
        bool dup = false;
        for (int k = 0; k < m; ++k) if (PartSame(top[k], p)) { dup = true; break; }
        if (dup) continue;
        if (m < kKeyParts) top[m++] = p;
        else if (PartBefore(p, top[m - 1])) top[m - 1] = p;
        else continue;
        for (int k = m - 1; k > 0 && PartBefore(top[k], top[k - 1]); --k) { const Part t = top[k]; top[k] = top[k - 1]; top[k - 1] = t; }
    }
    if (nUsed) *nUsed = m;
    if (used) for (int k = 0; k < m; ++k) used[k] = top[k];
    if (!m) return 0;
    uint64_t h = Mix(0x46505350524F4631ull + (uint64_t)m);
    for (int k = 0; k < m; ++k) {
        h = Mix(h ^ ((uint64_t)top[k].ic | ((uint64_t)top[k].stride << 32)));
        h = Mix(h ^ ((uint64_t)top[k].fmt + 0x9E3779B97F4A7C15ull));
    }
    return h ? h : 1;
}

// ---- the placement ---------------------------------------------------------------------------------------------------------
struct Vals {
    float x = 0.0f, y = -25.0f;                          // fps_vr_x / fps_vr_y (-50..50)
    int dist = FpsPose::kDistDefault;                    // fps_vr_dist (cm)
    int yaw = 0, pitch = 0, roll = 0;                    // fps_vr_yaw / _pitch / _roll (degrees)
    float scale = 0.3f;                                  // fps_vr_scale (0.2..3.0)
    int color = -1, bright = -1, text = -1;              // phase 51b: fps_color / fps_brightness / fps_text; -1 = not stored (a v1 line)
    int opacity = -1;                                    // phase 51f: fps_opacity; -1 = not stored (a v1 / v2 line)
    bool operator==(const Vals& o) const {
        return x == o.x && y == o.y && dist == o.dist && yaw == o.yaw && pitch == o.pitch && roll == o.roll && scale == o.scale &&
               color == o.color && bright == o.bright && text == o.text && opacity == o.opacity;
    }
    bool operator!=(const Vals& o) const { return !(*this == o); }
    bool HasLook() const { return color >= 0 || bright >= 0 || text >= 0 || opacity >= 0; }
};
constexpr int kTextModes = 6;                            // fps_text 0..5 (inject.cpp kFpsTextModes)
inline float ClampF(double v, double lo, double hi, float def) { return v != v ? def : (float)(v < lo ? lo : (v > hi ? hi : v)); }
inline Vals Clamped(const Vals& v) {                     // the LoadConfig ranges of the fps_vr_* keys
    Vals o = v;
    o.x = ClampF(v.x, -50.0, 50.0, 0.0f);
    o.y = ClampF(v.y, -50.0, 50.0, -25.0f);
    o.dist = FpsPose::ClampI(v.dist, FpsPose::kDistMin, FpsPose::kDistMax);
    o.yaw = FpsPose::ClampI(v.yaw, -FpsPose::kYawMax, FpsPose::kYawMax);
    o.pitch = FpsPose::ClampI(v.pitch, -FpsPose::kPitchMax, FpsPose::kPitchMax);
    o.roll = FpsPose::ClampI(v.roll, -FpsPose::kRollMax, FpsPose::kRollMax);
    o.scale = ClampF(v.scale, 0.2, 3.0, 0.3f);
    o.color = v.color < 0 ? -1 : FpsPose::ClampColor(v.color);
    o.bright = v.bright < 0 ? -1 : FpsPose::ClampBright(v.bright);
    o.text = v.text < 0 ? -1 : (v.text >= kTextModes ? kTextModes - 1 : v.text);
    o.opacity = v.opacity < 0 ? -1 : FpsPose::ClampOpacity(v.opacity);
    return o;
}

// ---- phase 51b: the box's pose in the cab -----------------------------------------------------------------------------------
// phase 51c: the same box in up to kCabParts cab parts (part 0 = the fields below, parts 1.. = x[]): a later lock-on uses the
// parts that AGREE (FpsCab ApplyCab: the majority), so one moving part among them (the ATS 14:38 truck: the reference was the
// steering wheel, IndexCount 13200) cannot turn the box.
constexpr int kCabParts = 3;
struct CabPart {
    uint32_t ic = 0, stride = 0, fmt = 0;
    double A[3][4] = {};
    bool operator==(const CabPart& o) const {
        if (ic != o.ic || stride != o.stride || fmt != o.fmt) return false;
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) if (A[i][j] != o.A[i][j]) return false;
        return true;
    }
};
struct CabPose {
    uint32_t ic = 0, stride = 0, fmt = 0;                // the cab part whose model space A maps into
    double A[3][4] = {};                                 // play space -> that part's model space (affine rows; row 3 = 0 0 0 1)
    double pivot[3] = { 0.0, 0.0, 0.0 };                 // the FpsCab::LockFrame the box is built in
    double yaw0 = 0.0, fw = 1.6;
    bool level = false;                                  // phase 51d: the frame is square to the cab (line token "cabsq")
    int nx = 0;                                          // phase 51c: more parts (the same box, each in its own model space)
    CabPart x[kCabParts - 1];
    int Parts() const { return 1 + nx; }
    CabPart Part(int i) const {
        if (i > 0) return x[i - 1];
        CabPart p;
        p.ic = ic; p.stride = stride; p.fmt = fmt;
        memcpy(p.A, A, sizeof(A));
        return p;
    }
    void SetPart(int i, const CabPart& p) {
        if (i > 0) { x[i - 1] = p; return; }
        ic = p.ic; stride = p.stride; fmt = p.fmt;
        memcpy(A, p.A, sizeof(A));
    }
    bool operator==(const CabPose& o) const {
        if (ic != o.ic || stride != o.stride || fmt != o.fmt || yaw0 != o.yaw0 || fw != o.fw || nx != o.nx || level != o.level) return false;
        for (int i = 0; i < 3; ++i) {
            if (pivot[i] != o.pivot[i]) return false;
            for (int j = 0; j < 4; ++j) if (A[i][j] != o.A[i][j]) return false;
        }
        for (int i = 0; i < nx; ++i) if (!(x[i] == o.x[i])) return false;
        return true;
    }
    bool operator!=(const CabPose& o) const { return !(*this == o); }
    // drop part i (phase 51c: found moving); false = it was the last one
    bool DropPart(int i) {
        if (i < 0 || i >= Parts()) return Parts() > 0;
        if (nx == 0) return false;
        for (int k = i; k < Parts() - 1; ++k) SetPart(k, Part(k + 1));
        --nx;
        return true;
    }
};
inline bool CabPartOk(const CabPart& p) {
    if (!p.ic) return false;
    double n = 0.0;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) { if (!std::isfinite(p.A[i][j])) return false; if (j < 3) n += p.A[i][j] * p.A[i][j]; }
    return n > 1e-12;
}
inline bool CabPoseOk(const CabPose& c) {
    if (!(c.fw > 0.1 && c.fw < 20.0) || !std::isfinite(c.yaw0) || c.nx < 0 || c.nx > kCabParts - 1) return false;
    for (int i = 0; i < 3; ++i) if (!std::isfinite(c.pivot[i])) return false;
    for (int i = 0; i < c.Parts(); ++i) if (!CabPartOk(c.Part(i))) return false;   // (a zero map = no pose)
    return true;
}

// ---- dlaa.ini line ---------------------------------------------------------------------------------------------------------
// The VALUE part (after '='): v1 or v2 (the header). False = malformed (the line is then left alone). *cab / *hasCab (optional) =
// the v2 cab pose; a v2 line whose cab pose is not usable (not finite, fw out of range, IndexCount 0) loads without one.
inline bool ParseValue(const char* s, uint64_t* key, Vals* v, CabPose* cab = nullptr, bool* hasCab = nullptr) {
    if (hasCab) *hasCab = false;
    if (!s) return false;
    while (*s == ' ' || *s == '\t') ++s;
    char* e = nullptr;
    const unsigned long long k = strtoull(s, &e, 16);
    if (e == s || !k || (*e != ' ' && *e != '\t')) return false;
    double f[7];
    const char* p = e;
    for (int i = 0; i < 7; ++i) {
        char* q = nullptr;
        f[i] = strtod(p, &q);
        if (q == p) return false;
        p = q;
    }
    auto skip = [&p]() { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p; };
    auto word = [&p](const char* w) {                    // the token w, then a blank / the end
        const size_t n = strlen(w);
        if (strncmp(p, w, n) != 0 || (p[n] && p[n] != ' ' && p[n] != '\t' && p[n] != '\r' && p[n] != '\n')) return false;
        p += n;
        return true;
    };
    auto clampI = [](double d) { return (int)(d < -1e6 ? -1e6 : (d > 1e6 ? 1e6 : d)); };
    Vals o;
    skip();
    if (*p && *p != '#' && *p != ';') {                  // phase 51b: the v2 fields
        const bool v3 = word("v3");                      // phase 51f: v3 = v2 + opacity after text
        if (!v3 && !word("v2")) return false;
        double l[4] = { -1.0, -1.0, -1.0, -1.0 };
        for (int i = 0; i < (v3 ? 4 : 3); ++i) {
            char* q = nullptr;
            l[i] = strtod(p, &q);
            if (q == p || l[i] != l[i]) return false;
            p = q;
        }
        o.color = l[0] < 0 ? -1 : clampI(l[0]);
        o.bright = l[1] < 0 ? -1 : clampI(l[1]);
        o.text = l[2] < 0 ? -1 : clampI(l[2]);
        o.opacity = l[3] < 0 ? -1 : clampI(l[3]);
        skip();
        bool sq = false;
        if (word("-")) {
        } else if ((sq = word("cabsq")) || word("cab")) {   // phase 51d: "cabsq" = square to the cab
            double c[20];                                // ic stride fmt, A (12), pivot (3), yaw0, fw
            for (int i = 0; i < 20; ++i) {
                char* q = nullptr;
                c[i] = strtod(p, &q);
                if (q == p) return false;
                p = q;
            }
            CabPose cp;
            const bool idsOk = c[0] >= 1.0 && c[0] < 4294967296.0 && c[1] >= 0.0 && c[1] < 4294967296.0 && c[2] >= 0.0 && c[2] < 4294967296.0;
            if (idsOk) { cp.ic = (uint32_t)c[0]; cp.stride = (uint32_t)c[1]; cp.fmt = (uint32_t)c[2]; }
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) cp.A[i][j] = c[3 + i * 4 + j];
            for (int i = 0; i < 3; ++i) cp.pivot[i] = c[15 + i];
            cp.yaw0 = c[18]; cp.fw = c[19];
            cp.level = sq;
            bool ok = idsOk;
            skip();
            while (word("+")) {                          // phase 51c: "+ ic stride fmt a00 .. a23" per further part
                double d[15];
                for (int i = 0; i < 15; ++i) {
                    char* q = nullptr;
                    d[i] = strtod(p, &q);
                    if (q == p) return false;
                    p = q;
                }
                skip();
                if (cp.nx >= kCabParts - 1) continue;    // (more than kCabParts: ignored)
                CabPart x;
                if (!(d[0] >= 1.0 && d[0] < 4294967296.0 && d[1] >= 0.0 && d[1] < 4294967296.0 && d[2] >= 0.0 && d[2] < 4294967296.0)) { ok = false; continue; }
                x.ic = (uint32_t)d[0]; x.stride = (uint32_t)d[1]; x.fmt = (uint32_t)d[2];
                for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) x.A[i][j] = d[3 + i * 4 + j];
                cp.x[cp.nx++] = x;
            }
            if (ok && CabPoseOk(cp)) { if (cab) *cab = cp; if (hasCab) *hasCab = true; }
        } else return false;
        skip();
        if (*p && *p != '#' && *p != ';') return false;
    }
    o.x = (float)f[0]; o.y = (float)f[1];
    o.dist = clampI(f[2]);
    o.yaw = clampI(f[3]);
    o.pitch = clampI(f[4]);
    o.roll = clampI(f[5]);
    o.scale = (float)f[6];
    *key = (uint64_t)k;
    *v = Clamped(o);
    return true;
}
// v1 when there is nothing of v2 to write (no colour / brightness / text / opacity, no cab pose), else v2 (v3 when an opacity is stored)
inline std::string FormatValue(uint64_t key, const Vals& v, const CabPose* cab = nullptr) {
    char b[2048];
    int o = snprintf(b, sizeof(b), "%016llx %.2f %.2f %d %d %d %d %.2f", (unsigned long long)key, (double)v.x, (double)v.y, v.dist, v.yaw,
                     v.pitch, v.roll, (double)v.scale);
    if (v.HasLook() || cab) {
        if (v.opacity >= 0) o += snprintf(b + o, sizeof(b) - (size_t)o, " v3 %d %d %d %d", v.color, v.bright, v.text, v.opacity);
        else                o += snprintf(b + o, sizeof(b) - (size_t)o, " v2 %d %d %d", v.color, v.bright, v.text);
        if (!cab) {
            o += snprintf(b + o, sizeof(b) - (size_t)o, " -");
        } else {
            o += snprintf(b + o, sizeof(b) - (size_t)o, " %s %u %u %u", cab->level ? "cabsq" : "cab", cab->ic, cab->stride, cab->fmt);
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) o += snprintf(b + o, sizeof(b) - (size_t)o, " %.17g", cab->A[i][j]);
            o += snprintf(b + o, sizeof(b) - (size_t)o, " %.17g %.17g %.17g %.17g %.17g", cab->pivot[0], cab->pivot[1], cab->pivot[2],
                          cab->yaw0, cab->fw);
            for (int k = 0; k < cab->nx && k < kCabParts - 1; ++k) {   // phase 51c
                const CabPart& x = cab->x[k];
                o += snprintf(b + o, sizeof(b) - (size_t)o, " + %u %u %u", x.ic, x.stride, x.fmt);
                for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) o += snprintf(b + o, sizeof(b) - (size_t)o, " %.17g", x.A[i][j]);
            }
        }
    }
    return b;
}
inline std::string FormatLine(uint64_t key, const Vals& v, const CabPose* cab = nullptr) {
    return std::string(kIniKey) + " = " + FormatValue(key, v, cab);
}

// ---- the profiles in memory ------------------------------------------------------------------------------------------------
struct Store {
    struct E { uint64_t key = 0; Vals v; uint64_t used = 0; bool hasCab = false; CabPose cab; };   // phase 51b: + the cab pose
    E e[kMax];
    int n = 0;
    uint64_t clock = 0;                                  // use stamps (the least recently used is dropped at the cap)
    uint64_t evicted[kMax] = {};                         // dropped keys: their lines leave dlaa.ini at the next Rewrite
    int nEvicted = 0;
    int Find(uint64_t key) const { for (int i = 0; i < n; ++i) if (e[i].key == key) return i; return -1; }
    const Vals* Get(uint64_t key) { const int i = Find(key); if (i < 0) return nullptr; e[i].used = ++clock; return &e[i].v; }
    const CabPose* Cab(uint64_t key) const { const int i = Find(key); return i >= 0 && e[i].hasCab ? &e[i].cab : nullptr; }
    // phase 51b: the cab pose of a stored key; true = changed
    bool PutCab(uint64_t key, const CabPose& c) {
        const int i = Find(key);
        if (i < 0 || !CabPoseOk(c)) return false;
        if (e[i].hasCab && e[i].cab == c) return false;
        e[i].hasCab = true; e[i].cab = c;
        return true;
    }
    // true = something changed (a new entry or other values); a new entry has no cab pose; the cab pose is kept
    bool Put(uint64_t key, const Vals& v) {
        if (!key) return false;
        int i = Find(key);
        if (i >= 0) { e[i].used = ++clock; if (e[i].v == v) return false; e[i].v = v; return true; }
        if (n == kMax) {
            int lru = 0;
            for (int k = 1; k < n; ++k) if (e[k].used < e[lru].used) lru = k;
            if (nEvicted < kMax) evicted[nEvicted++] = e[lru].key;
            e[lru] = e[--n];
        }
        for (int k = 0; k < nEvicted; ++k) if (evicted[k] == key) { evicted[k] = evicted[--nEvicted]; break; }
        e[n] = E();
        e[n].key = key; e[n].v = v; e[n].used = ++clock;
        ++n;
        return true;
    }
    // phase 51c: a stored part found MOVING: dropped from the key's cab pose (the last one: the pose is cleared -- the next lock-on
    // stores a fresh one). True = changed.
    bool DropCabPart(uint64_t key, uint32_t ic, uint32_t stride, uint32_t fmt) {
        const int i = Find(key);
        if (i < 0 || !e[i].hasCab) return false;
        CabPose& c = e[i].cab;
        for (int k = 0; k < c.Parts(); ++k) {
            const CabPart p = c.Part(k);
            if (p.ic != ic || p.stride != stride || p.fmt != fmt) continue;
            if (!c.DropPart(k)) e[i].hasCab = false;
            return true;
        }
        return false;
    }
    bool ClearCab(uint64_t key) { const int i = Find(key); if (i < 0 || !e[i].hasCab) return false; e[i].hasCab = false; return true; }
    // a dlaa.ini line at startup (the value part); the first line of a key wins; false = malformed / a duplicate
    bool Load(const char* value) {
        uint64_t k; Vals v; CabPose c; bool hc = false;
        if (!ParseValue(value, &k, &v, &c, &hc) || Find(k) >= 0 || n == kMax) return false;
        e[n] = E();
        e[n].key = k; e[n].v = v; e[n].used = 0; e[n].hasCab = hc;
        if (hc) e[n].cab = c;
        ++n;
        return true;
    }
};

// ---- phase 51d: the binding of the live fps_vr_* values to a vehicle (inject.cpp FpsProfileTick; pure, tests/fps_cab_test.cpp) ----
// Bound = the vehicle the live values belong to (+ phase 47e same-cab aliases). Each Present: Tick with the LIVE mode (VR + box shown +
// fps_vr_lock 2 in the live settings), whether FpsCab is active, the profiles switch, the lock count / key / same-cab flag of FpsCab
// and the live values. Not bound while the live mode is not 2: phase 51d -- the Reset row sets the defaults and mode 0 at once, but
// FpsCab only switches off at its next sync (one Present later, ATS 15:04:46), so the cab state alone let that Present copy the
// defaults into the bound vehicle's profile. A new lock binds its vehicle: a stored profile is loaded (*apply), else the live values
// start it. While bound, a change of the live values goes into the bound keys.
struct Session {
    static constexpr int kBound = 4;
    uint64_t bound[kBound] = {};
    int nBound = 0;
    uint64_t locks = 0;                                  // FpsCab::LockCount handled (~0 = handle the held lock again)
    uint64_t lastKey = 0;                                // phase 51e: the vehicle bound last (kept while unbound: box off / mode 0 / 1)
    enum { kLockNone = 0, kLockLoaded, kLockNew, kLockAlias, kLockKept };
    struct Out {
        int lock = kLockNone;                            // a new lock-on was handled (its vehicle key = key)
        uint64_t key = 0, aliasOf = 0;
        bool applyIt = false;                            // kLockLoaded with values other than the live ones: apply `apply`
        Vals apply;
        int nChanged = 0;                                // keys whose profile changed this tick (new / alias / edits)
        uint64_t changed[kBound + 1] = {};
    };
    // (explicit: the Reset row, profiles switched off -- the next lock-on loads the vehicle's stored values; the unbound ticks below
    // keep lastKey: the FPS row cycling through Off re-binds the same vehicle and keeps the live values, phase 51e)
    void Unbind() { nBound = 0; lastKey = 0; }
    bool IsBound(uint64_t k) const { for (int i = 0; i < nBound; ++i) if (bound[i] == k) return true; return false; }
    static Vals Applied(const Vals& live, const Vals& pv) {   // the live values after a profile is applied (look -1 = keep live)
        Vals o = pv;
        o.color = pv.color >= 0 ? pv.color : live.color;
        o.bright = pv.bright >= 0 ? pv.bright : live.bright;
        o.text = pv.text >= 0 ? pv.text : live.text;
        o.opacity = pv.opacity >= 0 ? pv.opacity : live.opacity;   // phase 51f
        return o;
    }
    void Changed(Out* o, uint64_t k) const {
        for (int i = 0; i < o->nChanged; ++i) if (o->changed[i] == k) return;
        if (o->nChanged < kBound + 1) o->changed[o->nChanged++] = k;
    }
    Out Tick(Store& st, bool mode2Live, bool cabActive, bool profilesOn, uint64_t lockCount, uint64_t key, bool sameCab, Vals live) {
        Out o;
        if (!mode2Live || !cabActive || !profilesOn) {   // (mode 0 / 1 edits, the Reset row and the shared mode never reach a profile)
            nBound = 0;
            locks = lockCount;                           // (switched on later / mode 2 again = the next lock-on binds)
            return o;
        }
        if (lockCount != locks) {
            locks = lockCount;
            if (key) {
                o.key = key;
                if (!IsBound(key) && nBound > 0 && sameCab) {   // a 47e re-lock on the same cab whose biggest draws differ: an alias
                    o.aliasOf = bound[0];
                    if (nBound < kBound) bound[nBound++] = key; else bound[kBound - 1] = key;
                    if (st.Put(key, live)) Changed(&o, key);
                    o.lock = kLockAlias;
                } else if (key == lastKey && nBound == 0 && st.Get(key)) {   // phase 51e: the same vehicle re-bound: live values win
                    bound[0] = key; nBound = 1;
                    if (st.Put(key, live)) Changed(&o, key);
                    o.lock = kLockKept;
                } else if (const Vals* pv = st.Get(key)) {
                    bound[0] = key; nBound = 1; lastKey = key;
                    o.apply = *pv;
                    o.applyIt = *pv != live;
                    live = Applied(live, *pv);
                    o.lock = kLockLoaded;
                } else {
                    bound[0] = key; nBound = 1; lastKey = key;
                    st.Put(key, live);
                    Changed(&o, key);
                    o.lock = kLockNew;
                }
            }
        }
        for (int i = 0; i < nBound; ++i) if (st.Put(bound[i], live)) Changed(&o, bound[i]);   // the menu's edits
        return o;
    }
};

// The dlaa.ini text with the profile lines brought up to date: an fps_profile line of a stored key is replaced in place (its own
// line terminator kept; a later duplicate of the key dropped), a line of an evicted key dropped, every other line (comments,
// other keys, malformed / unknown fps_profile lines) kept byte for byte; stored keys without a line are appended at the end
// (the file's line ending). An empty input gets the WriteIniPreserving header first.
inline std::string Rewrite(const std::string& in, const Store& st) {
    const char* nl = (in.find("\r\n") != std::string::npos) ? "\r\n" : "\n";
    bool seen[kMax] = {};
    std::string out;
    out.reserve(in.size() + 128);
    if (in.empty()) { out += "# dlaa.ini -- settings saved by the ETS2 DLAA injector"; out += nl; }
    const size_t kl = strlen(kIniKey);
    size_t i = 0;
    while (i < in.size()) {
        size_t e = i;
        while (e < in.size() && in[e] != '\n') ++e;
        const size_t rawEnd = e < in.size() ? e + 1 : e;
        size_t pe = e;
        if (pe > i && in[pe - 1] == '\r') --pe;
        size_t s = i;
        while (s < pe && (in[s] == ' ' || in[s] == '\t')) ++s;
        int action = 0, idx = -1;                        // 0 keep, 1 replace by entry idx, 2 drop
        if (s < pe && in[s] != '#' && in[s] != ';') {
            size_t eq = s;
            while (eq < pe && in[eq] != '=') ++eq;
            size_t ke = eq;
            while (ke > s && (in[ke - 1] == ' ' || in[ke - 1] == '\t')) --ke;
            if (eq < pe && ke - s == kl && memcmp(&in[s], kIniKey, kl) == 0) {
                const std::string val(in, eq + 1, pe - eq - 1);
                uint64_t k; Vals v;
                if (ParseValue(val.c_str(), &k, &v)) {
                    const int f = st.Find(k);
                    if (f >= 0) { if (seen[f]) action = 2; else { action = 1; idx = f; seen[f] = true; } }
                    else for (int q = 0; q < st.nEvicted; ++q) if (st.evicted[q] == k) action = 2;
                }
            }
        }
        if (action == 0) out.append(in, i, rawEnd - i);
        else if (action == 1) {
            out += FormatLine(st.e[idx].key, st.e[idx].v, st.e[idx].hasCab ? &st.e[idx].cab : nullptr);
            out.append(in, pe, rawEnd - pe);
        }
        i = rawEnd;
    }
    bool needNl = !out.empty() && out[out.size() - 1] != '\n';
    for (int k = 0; k < st.n; ++k) {
        if (seen[k]) continue;
        if (needNl) { out += nl; needNl = false; }
        out += FormatLine(st.e[k].key, st.e[k].v, st.e[k].hasCab ? &st.e[k].cab : nullptr); out += nl;
    }
    return out;
}

}  // namespace FpsProfile
