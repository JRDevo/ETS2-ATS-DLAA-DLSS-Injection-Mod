// ShadowLock -- phase 103: the player's OWN vehicle shadow no longer steps 7..20 px per frame while driving (dlaa.ini shadow_lock).
// Pure maths, no D3D: inject.cpp (the "phase 103" block above hkMap: ring fills, slice binds, the viewport shift) and
// tests/shadow_lock_test.cpp use it.
//
// WHY (RenderDoc ATS frames 1890 / 3818 / 5519, scratchpad rdc_shadow / rdc_shadow2, 2026-11-14): the SCS sun shadow cascades are
// NOT texel-snapped. Each frame the cascade fit follows the camera, so the own vehicle -- rigid with the camera -- is rasterised
// into every cascade at a new sub-texel phase and its shadow edge jumps by whole shadow texels (cascade 0 ~ 7.75 mm, cascade 1
// ~ 2 cm; the steps show with the mod off too). DLSS cannot hide it: the road's motion vectors are not the shadow's motion.
//
// THE GAME (D3D11): the cascades are ONE R16_TYPELESS S x S Texture2DArray, 4 slices (S = 3096 at r_sun_shadow_texture_size 4096;
// never hard-coded here), each slice drawn through its own DSV (Texture2DArray view, ArraySize 1). The far map is a separate
// 2S x 2S Texture2D -- left alone. All per-frame constants are written by ONE Map(WRITE_DISCARD) / Unmap per 2 MB DYNAMIC
// constant ring at frame start. The sun light's block sits in a ring at a varying offset; its rows (float4 "c" registers, block
// start = c0):
//     c(14+3k) / c(15+3k) / c(16+3k)  cascade k (0..3) x / y / z rows: VIEW space -> cascade NDC   (k = 4: the far map)
//     c29 = (S, S, 1/S, 1/S)          the cascade size row  ("R" below: every row index is relative to it)
//     c30 = (F, F, 1/F, 1/F)          the far map's size,  c31.y = 100
// uv = (0.5 x + 0.5, 0.5 - 0.5 y); the camera (view origin) is at x = c(14+3k).w, y = c(15+3k).w, so its texel is
// tx = (0.5 x + 0.5) S, ty = (0.5 - 0.5 y) S. A forward VS (81dfa1a2-...) samples the cascades too: its per-draw cb0 has the same
// 5 x 3 rows right before its size row (c13..c27, size c28), but no far-size row after it.
//
// THE FIX, per frame and cascade k: d_k = (dx, dy) texels with frac(camera texel + d_k) = phase (0.5): dx = Wrap(phase - frac(tx))
// in [-0.5, 0.5). (a) the slice is rasterised with its viewport shifted by d_k (TopLeft += d_k, a fractional viewport is legal);
// (b) every reader's rows get the same shift in NDC: x-row.w += 2 dx / S, y-row.w -= 2 dy / S (y is flipped in uv). Any point
// rigid with the vehicle can be the lock point; the camera is the one the constants carry. The own vehicle then lands on the same
// sub-texel phase every frame -> its shadow edge is still. The static world now steps instead (it did before as well: the cascade
// fit was never snapped). EDGE: the shift leaves at most a 1-texel column / row of the slice unrasterised at its border; inside
// cascades 0..2 that is in the 20 % cascade blend band (the next cascade covers it) -- accepted.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ShadowLock {

constexpr int    kCascades  = 4;    // slices locked (row group 4 = the far map, never touched)
constexpr int    kGroups    = 5;    // row groups before the size row: 4 cascades + the far map
constexpr int    kRowsBefore = 15;  // 5 groups x (x, y, z)
constexpr int    kSunRowsAfter = 2; // the sun block: far-size row R + 1, c31 = R + 2 (y = 100)

enum Kind : int { kKindNone = 0, kKindSun = 1, kKindFwd = 2 };
inline const char* KindName(int k) { return k == kKindSun ? "sun" : (k == kKindFwd ? "forward" : "none"); }

inline uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline bool Finite(float f) { return (Bits(f) & 0x7f800000u) != 0x7f800000u; }
inline bool Near(float a, float b, float rel) { return std::fabs(a - b) <= rel * std::fabs(b); }

// (S, S, 1/S, 1/S) -- S itself compared exactly (the game writes the integer size as a float), 1/S within 1e-4 relative
inline bool SizeRow(const float* r, float S) {
    return S > 0.0f && r[0] == S && r[1] == S && Near(r[2], 1.0f / S, 1e-4f) && Near(r[3], 1.0f / S, 1e-4f);
}
// (F, F, 1/F, 1/F) for any F >= 64 (the far map's size)
inline bool FarRow(const float* r) {
    return r[0] >= 64.0f && r[0] == r[1] && Finite(r[0]) && Near(r[2], 1.0f / r[0], 1e-4f) && Near(r[3], 1.0f / r[0], 1e-4f);
}
inline float Len3(const float* r) { return std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]); }

// Is float4 row R of `rows` (nRows float4 rows) a cascade-block size row for cascade size S? Checks, besides the size row:
//   - the 15 rows before it are finite and plausible (|v| < 1e6), every x / y / z row of the 5 groups is non-zero,
//   - the x-row AND y-row scale shrink strictly from group to group (cascades grow: 0.083, 0.029, 0.011, 0.0055, 0.00058 in
//     frame 1890) -- a random (S, S, 1/S, 1/S) row (a texel-size constant of another shader) has no such ladder in front of it,
//   - the camera's cascade NDC is plausible (|x.w|, |y.w| <= 16 for k = 0..3; VR measured 2.04 at cascade 3),
//   - R + 2 rows exist (the window must show what follows); then: R + 1 a far-size row and c31.y = 100 -> kKindSun; R + 1 a
//     far-size row WITHOUT c31.y = 100 -> rejected (unknown layout); anything else -> kKindFwd (the forward VS block).
// strict = false (a block a sun light draw READS, the size row at c29 of its window): a far-size row makes it a sun block whatever
// c31.y holds (night / another light setting); the scans stay strict (c31.y = 100) against false hits in raw ring memory
inline Kind Classify(const float* rows, size_t nRows, size_t R, float S, bool strict = true) {
    if (R < (size_t)kRowsBefore || R + kSunRowsAfter >= nRows) return kKindNone;
    const float* sr = rows + 4 * R;
    if (!SizeRow(sr, S)) return kKindNone;
    if (FarRow(sr - 4)) return kKindNone;   // R is the FAR row of a block whose cascade size is S / 2 (its z-row is never one)
    for (size_t i = R - kRowsBefore; i < R; ++i)
        for (int c = 0; c < 4; ++c) {
            const float v = rows[4 * i + c];
            if (!Finite(v) || std::fabs(v) >= 1e6f) return kKindNone;
        }
    float px = 0.0f, py = 0.0f;
    for (int g = 0; g < kGroups; ++g) {
        const float* x = rows + 4 * (R - kRowsBefore + 3 * g);
        const float sx = Len3(x), sy = Len3(x + 4), sz = Len3(x + 8);
        if (!(sx > 0.0f) || !(sy > 0.0f) || !(sz > 0.0f)) return kKindNone;
        if (g > 0 && !(sx < 0.95f * px && sy < 0.95f * py)) return kKindNone;
        px = sx; py = sy;
        // the camera's cascade NDC: plausibility only. VR (ETS2 2026-11-14 dump) puts it OUTSIDE the outer cascades (c23.w = 2.039:
        // one fit for both eyes) -- the round-1 bound of 2 rejected real blocks there, so the eye-0 block was often never found
        if (g < kCascades && (std::fabs(x[3]) > 16.0f || std::fabs(x[7]) > 16.0f)) return kKindNone;
    }
    if (FarRow(sr + 4)) return (!strict || sr[9] == 100.0f) ? kKindSun : kKindNone;
    return kKindFwd;
}

struct Hit { uint32_t row; int kind; };   // `row` = the size row R, in float4 rows from the start of the scanned data
// Every block in `rows` (nRows float4 rows, 16-byte rows as the constant buffer holds them). Cheap pre-test per row: x and y
// bit-equal to S. Returns the number of hits written (<= maxOut; *more = true when there were more).
inline int Scan(const float* rows, size_t nRows, float S, Hit* out, int maxOut, bool* more = nullptr) {
    if (more) *more = false;
    int n = 0;
    const uint32_t bs = Bits(S);
    for (size_t r = kRowsBefore; r + kSunRowsAfter < nRows; ++r) {
        const float* p = rows + 4 * r;
        if (Bits(p[0]) != bs || Bits(p[1]) != bs) continue;
        const Kind k = Classify(rows, nRows, r, S);
        if (k == kKindNone) continue;
        if (n >= maxOut) { if (more) *more = true; break; }
        out[n++] = Hit{ (uint32_t)r, (int)k };
    }
    return n;
}

// the rows of cascade k relative to the size row R (the same for the sun block and the forward block)
inline size_t XRow(size_t R, int k) { return R - kRowsBefore + 3 * (size_t)k; }
inline size_t YRow(size_t R, int k) { return R - kRowsBefore + 1 + 3 * (size_t)k; }

// the camera's texel position in each cascade (tx, ty in texels of the S x S slice; ty grows downwards like the texture)
struct Cam { double tx[kCascades], ty[kCascades]; };
inline Cam CamTexel(const float* rows, size_t R, float S) {
    Cam c{};
    for (int k = 0; k < kCascades; ++k) {
        const double x = rows[4 * XRow(R, k) + 3], y = rows[4 * YRow(R, k) + 3];
        c.tx[k] = (0.5 * x + 0.5) * (double)S;
        c.ty[k] = (0.5 - 0.5 * y) * (double)S;
    }
    return c;
}
inline double Frac(double v) { return v - std::floor(v); }
inline double Wrap(double v) { return v - std::floor(v + 0.5); }          // -> [-0.5, 0.5)
// the shift that puts texel position t on sub-texel phase `phase` (0..1): frac(t + d) == phase, d in [-0.5, 0.5)
inline double Delta(double t, double phase) { return Wrap(phase - Frac(t)); }

struct Shift { float dx[kCascades], dy[kCascades]; };
inline Shift ShiftFor(const Cam& c, double phase) {
    Shift s{};
    for (int k = 0; k < kCascades; ++k) { s.dx[k] = (float)Delta(c.tx[k], phase); s.dy[k] = (float)Delta(c.ty[k], phase); }
    return s;
}
// the patched .w of a reader's x-row / y-row for a slice rasterised d texels shifted (y flipped: texel y grows as NDC y falls)
inline float PatchX(float w, float dx, float S) { return (float)((double)w + 2.0 * (double)dx / (double)S); }
inline float PatchY(float w, float dy, float S) { return (float)((double)w - 2.0 * (double)dy / (double)S); }
// the BYTE offsets of cascade k's x-row .w / y-row .w for a block whose size row starts at byte `sizeRowByte` (16-aligned) --
// inject.cpp writes the patched values there in the mapped ring
inline uint32_t XWByte(uint32_t sizeRowByte, int k) { return (uint32_t)XRow(sizeRowByte / 16u, k) * 16u + 12u; }
inline uint32_t YWByte(uint32_t sizeRowByte, int k) { return (uint32_t)YRow(sizeRowByte / 16u, k) * 16u + 12u; }
// in place on a copy (tests; inject.cpp writes the same values into the mapped ring), only cascades whose bit is in `mask`
inline void Patch(float* rows, size_t R, float S, const Shift& s, unsigned mask = 0xFu) {
    for (int k = 0; k < kCascades; ++k) {
        if (!(mask & (1u << k))) continue;
        float& xw = rows[4 * XRow(R, k) + 3];
        float& yw = rows[4 * YRow(R, k) + 3];
        xw = PatchX(xw, s.dx[k], S);
        yw = PatchY(yw, s.dy[k], S);
    }
}

// HOW the slice binds of one ring-fill group are paired with its sun blocks, from the STABLE history (VR is not measured yet:
// the cascades may be drawn once per frame or once per eye, with one sun block or one per eye):
//   kModeSingle  1 sun block, each slice bound once per group: d from that block (flat)
//   kModeShared  >= 2 sun blocks (one per eye), each slice bound once: the slices are shared by the eyes -> ONE d for all of
//                them, from the FIRST block of the group (any point rigid with the vehicle works; the first eye's camera is one)
//   kModePerEye  N >= 2 sun blocks and each slice bound N times: the m-th bind of slice k in the group uses block m's own d
//                (paired by order: ring fill order, ascending offset)
//   kModeOff     anything else (no slices, an unstable history, N blocks with another number of binds): nothing is changed
enum Mode : int { kModeOff = 0, kModeSingle = 1, kModeShared = 2, kModePerEye = 3 };
inline const char* ModeName(int m) {
    switch (m) {
        case kModeSingle: return "single (1 sun block, 1 bind per slice)";
        case kModeShared: return "shared (N sun blocks, 1 bind per slice: d from the first block)";
        case kModePerEye: return "per eye (N sun blocks, N binds per slice: paired by order)";
        default:          return "off";
    }
}
inline Mode Decide(int nSun, int bindsPerSlice) {
    if (nSun <= 0 || bindsPerSlice <= 0) return kModeOff;
    if (nSun == 1) return bindsPerSlice == 1 ? kModeSingle : kModeOff;
    if (bindsPerSlice == 1) return kModeShared;
    if (bindsPerSlice == nSun && nSun <= 4) return kModePerEye;
    return kModeOff;
}

// a value that must repeat `need` times in a row before it is trusted (the per-group sun count / binds per slice)
struct Stable {
    int value = -1, run = 0;
    void Add(int v) { if (v == value) { if (run < 1000000) ++run; } else { value = v; run = 1; } }
    bool Ok(int need) const { return run >= need; }
    int Get(int need) const { return Ok(need) ? value : -1; }
};

// The byte spans one ring fill is scanned over: +-pad around each learned block offset (size-row byte offsets) + one discovery
// span [discLo, discLo + discLen), clipped to [0, width), 16-byte aligned, sorted, merged. Returns the count (<= maxOut).
struct Span { uint32_t lo, len; };
inline int Spans(const uint32_t* offs, int nOffs, uint32_t width, uint32_t pad, uint32_t discLo, uint32_t discLen, Span* out,
                 int maxOut) {
    Span tmp[64];
    int n = 0;
    auto add = [&](uint64_t lo, uint64_t hi) {
        if (hi > width) hi = width;
        lo &= ~(uint64_t)15u;
        hi = (hi + 15u) & ~(uint64_t)15u;
        if (hi > width) hi = width & ~15u;
        if (hi <= lo || n >= 64) return;
        tmp[n++] = Span{ (uint32_t)lo, (uint32_t)(hi - lo) };
    };
    for (int i = 0; i < nOffs; ++i) {
        if (offs[i] >= width) continue;
        add(offs[i] > pad ? offs[i] - pad : 0u, (uint64_t)offs[i] + pad);
    }
    if (discLen && discLo < width) add(discLo, (uint64_t)discLo + discLen);
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0 && tmp[j].lo < tmp[j - 1].lo; --j) { const Span t = tmp[j]; tmp[j] = tmp[j - 1]; tmp[j - 1] = t; }
    int m = 0;
    for (int i = 0; i < n; ++i) {
        if (m && tmp[i].lo <= out[m - 1].lo + out[m - 1].len) {
            const uint32_t e = tmp[i].lo + tmp[i].len, pe = out[m - 1].lo + out[m - 1].len;
            if (e > pe) out[m - 1].len = e - out[m - 1].lo;
        } else if (m < maxOut) {
            out[m++] = tmp[i];
        }
    }
    return m;
}

// ---- review round 2 (VR diag drive 2026-11-14): which block is THIS frame's, and which eye ---------------------------------------
// The constant rings are DISCARD-mapped, but the game writes only what this frame needs: the rest of the memory still holds blocks
// of EARLIER frames (stale copies: same layout, older cameras), and the sun block's offset moves with the frame's draw count. The
// mod therefore patches only the block a sun light draw actually READS (at that draw), and picks the camera that sets d from the
// candidates by PREDICTION: the true eye-0 camera of the last frames (read at their first sun light draw) extrapolated by its
// velocity. The nearest candidate within the tolerance is this frame's eye-0 block; stale copies (older cameras) and the other
// eye's block (an IPD away) are farther. No candidate close enough -> the prediction itself (lock quality only: the readers are
// always patched with the d the slices were drawn with, whatever camera chose it).

// FNV-1a of the block rows R-15 .. R+2 (a byte-identical block at the same ring offset as in the ring's previous fill = stale)
inline uint64_t BlockHash(const float* rows, size_t R) {
    const unsigned char* b = reinterpret_cast<const unsigned char*>(rows + 4 * (R - kRowsBefore));
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < (size_t)(kRowsBefore + 1 + kSunRowsAfter) * 16u; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
// the true eye-0 camera of one frame (its first sun light draw), `seq` = that frame's slice-set number
struct Truth { bool ok = false; Cam c{}; double t = 0.0; };   // t = seconds (QPC) of that frame's first sun light draw
inline double Dist0(const Cam& a, const Cam& b) { const double x = a.tx[0] - b.tx[0], y = a.ty[0] - b.ty[0]; return std::sqrt(x * x + y * y); }
// the predicted eye-0 camera at time `now` (seconds) from the last two truths (t1 newer), extrapolated by TIME (frame times vary);
// t1 alone when t0 is missing / too old; false = no truth within maxAge. *speed = |velocity| in cascade-0 texels per second.
inline bool Predict(const Truth& t0, const Truth& t1, double now, Cam* out, double* speed, double maxAge = 0.1) {
    *speed = 0.0;
    if (!t1.ok || now < t1.t || now - t1.t > maxAge) return false;
    Cam p = t1.c;
    const double dt = t1.t - t0.t;
    if (t0.ok && dt > 1e-4 && dt <= maxAge) {
        const double s = (now - t1.t) / dt;
        for (int k = 0; k < kCascades; ++k) {
            p.tx[k] += (t1.c.tx[k] - t0.c.tx[k]) * s;
            p.ty[k] += (t1.c.ty[k] - t0.c.ty[k]) * s;
        }
        *speed = Dist0(t1.c, t0.c) / dt;
    }
    *out = p;
    return true;
}
// a truth this far (cascade-0 texels) from its prediction is a JUMP (teleport, camera switch, cascade refit): the velocity history
// restarts there (no extrapolation across it)
constexpr double kJumpTexels = 8.0;
inline bool IsJump(const Cam& pred, const Cam& truth) { return Dist0(pred, truth) > kJumpTexels; }
// how far (cascade-0 texels) a candidate may be from the prediction: 1 texel + half the motion since the last truth (`motion`), and under half the eye
// separation (eyeSep <= 0: unknown / flat) so the other eye's block is never taken
inline double Tolerance(double motion, double eyeSep) {
    double t = 1.0 + 0.5 * motion;
    if (eyeSep > 0.0 && t > 0.45 * eyeSep) t = 0.45 * eyeSep;
    return t < 0.05 ? 0.05 : t;
}
// the candidate nearest the prediction within `tol`, or -1
inline int Choose(const Cam* cands, int n, const Cam& pred, double tol) {
    int best = -1;
    double bd = tol;
    for (int i = 0; i < n; ++i) { const double d = Dist0(cands[i], pred); if (d < bd) { bd = d; best = i; } }
    return best;
}
// where the camera lands after the shift, relative to the target phase (texels, [-0.5, 0.5)): 0 = locked
inline double LockError(double t, double d, double phase) { return Wrap(Frac(t + d) - phase); }

// ---- round 3: the FORWARD cascade readers are found by their vertex shader, patched at their own draw ---------------------------
// A vertex shader that reads a Texture2DArray (dcl_resource_texture2darray) and declares cb0 with >= 29 registers -- the shape of the
// RenderDoc forward reader 81dfa1a2-48cc605f-e6ef06db-a3a98af0 (cb0[30], t0 = the cascade array) -- or that exact checksum. Pure
// DXBC walk (no D3D); a false positive costs one window check per draw (the block signature decides).
constexpr uint32_t kFwdVsHash[4] = { 0x81dfa1a2u, 0x48cc605fu, 0xe6ef06dbu, 0xa3a98af0u };
inline bool VsReadsCascades(const void* code, size_t len) {
    if (!code || len < 32) return false;
    const unsigned char* b = static_cast<const unsigned char*>(code);
    auto rd = [&](size_t o) { uint32_t v; std::memcpy(&v, b + o, 4); return v; };
    if (rd(0) != 0x43425844u) return false;                      // 'DXBC'
    if (rd(4) == kFwdVsHash[0] && rd(8) == kFwdVsHash[1] && rd(12) == kFwdVsHash[2] && rd(16) == kFwdVsHash[3]) return true;
    const uint32_t nChunks = rd(28);
    if (nChunks > 64 || 32 + (size_t)nChunks * 4 > len) return false;
    for (uint32_t c = 0; c < nChunks; ++c) {
        const size_t off = rd(32 + (size_t)c * 4);
        if (off + 8 > len) return false;
        const uint32_t fourcc = rd(off), size = rd(off + 4);
        if (fourcc != 0x52444853u && fourcc != 0x58454853u) continue;   // 'SHDR' / 'SHEX'
        if (size < 8 || off + 8 + size > len) return false;
        const size_t base = off + 8;
        if ((rd(base) >> 16) != 1u) return false;                // program type 1 = vertex shader
        const uint32_t nTok = size / 4, declared = rd(base + 4), n = declared < nTok ? declared : nTok;
        bool arr = false, cb0 = false;
        for (uint32_t i = 2; i < n;) {
            const uint32_t op = rd(base + (size_t)i * 4), opc = op & 0x7FFu;
            uint32_t ilen = (op >> 24) & 0x7Fu;
            if (opc == 0x35u) { if (i + 1 >= n) return false; ilen = rd(base + (size_t)(i + 1) * 4); }
            if (ilen == 0 || ilen > n - i) return false;
            if (opc == 88u && ((op >> 11) & 0x1Fu) == 8u) arr = true;            // dcl_resource, dimension TEXTURE2DARRAY (8)
            if (opc == 89u && ilen >= 4 && rd(base + (size_t)(i + 2) * 4) == 0u &&  // dcl_constantbuffer cb0[N], N >= 29
                rd(base + (size_t)(i + 3) * 4) >= 29u) cb0 = true;
            i += ilen;
        }
        return arr && cb0;
    }
    return false;
}

}  // namespace ShadowLock
