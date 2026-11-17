// CutoutMips -- phase 77: COVERAGE-PRESERVING ALPHA MIPS for the game's cut-out (alpha-tested) BC3 textures. No D3D, no game
// state: inject.cpp (the UpdateSubresource1 hook) and tests/cutout_mips_test.cpp use it.
//
// Why: far chain-link fences shimmer and vanish. SCS builds a BC3 texture's alpha mips with a 2x2 box + a 0.5 threshold, binary
// 0 / 255, so a 1-2 texel wire drops out after a few mips: the ATS fence (128x2048, 12 mips) covers 17.5 % at mip 0 (alpha >=
// 0.05, the PS's discard cut), then 16.0, 9.9, 6.7, 6.6, 7.8, 14.1, 6.2, 0, 0, 0, 0 %. The fence PS reuses the alpha after the
// discard (it scales G-buffer RT1), so the output stays BINARY 0 / 255 (no Castano soft alpha scaling): per mip L the K_L =
// round(cov0 x W_L x H_L) texels whose mip-0 footprint is the most covered become 255, the rest 0. Same coverage on every mip
// = the wires thin out into a constant-density dither instead of vanishing.
//
// Flow (inject.cpp): mip 0 arrives first (UpdateSubresource1, DstSubresource 0, whole mip) -> Analyse() decodes its alpha,
// decides (Reason) and builds the binary alpha blocks of every smaller mip -> when mip L arrives, Replace() checks the game's own
// coverage of that mip (>= kKeepShare x cov0 = the game's data stays) and else copies the game's blocks with OUR alpha half.
// The colour half (8 bytes of each 16-byte block) is never touched.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace CutoutMips {

constexpr int      kCut       = 13;      // alpha >= 13 / 255 (0.051) passes the fence PS's "alpha < 0.049988 -> discard"
constexpr int      kSoftLo    = 5;       // "binary" texel: alpha <= 0.02 x 255 ...
constexpr int      kSoftHi    = 153;     // ... or >= 0.6 x 255
constexpr double   kMaxSoft   = 0.03;    // >= 97 % of the mip-0 texels binary, else a soft alpha (glass, gloss, decals): skip
// phase 107a: an ANTI-ALIASED cut-out (SCS tree leaves: the birch's 2048x1024 alpha has 5-6 % soft texels, all of them a 1-3 texel
// rim between alpha 0 and 255) failed the 3 % rule and lost 44 % of its coverage by mip 6. A soft texel now counts as soft only
// when it is NOT on a hard edge: no texel <= kSoftLo AND no texel >= kSoftHi both within kRimR texels (a 7 x 7 window). A rim
// between transparent and opaque always sees both; the inside of a gradient (decal fade, gloss, glass) sees at most one side.
// The rim pass runs only when the raw soft share is between kMaxSoft and kMaxSoftRaw (above that: soft, early stop as before).
// phase 109f: 2 -> 3. The 2026-11-14 town / forest / water dumps (1466 BC3 textures): 9 decisions change, all soft -> patched:
// the leaf textures of the discard PSs 438d1a63 (R18072 / R19440 / R20675, wide 3-4 texel rims) and a9d39be8 (R20237) + 5
// textures of non-discard PSs (a5aa1c2d, 9c062880 masks, one unlabelled 128x64) that phase 77b never patches (no discard draw).
constexpr int      kRimR       = 3;
constexpr double   kMaxSoftRaw = 0.25;   // raw soft share above this: never a cut-out (a rim this wide is a gradient)
constexpr double   kCovMin    = 0.02;    // mip-0 coverage at the cut: 2 .. 60 % (a solid or an almost empty alpha: skip)
constexpr double   kCovMax    = 0.60;
constexpr double   kLossGate  = 0.70;    // the game rule keeps >= 70 % of cov0 on every mip 2..(last >= 4 px): no real loss, skip
constexpr double   kKeepShare = 0.85;    // a mip whose own (game) coverage is >= 85 % of cov0 is left as the game sent it
constexpr uint32_t kMinDim    = 8;       // min(W, H) >= 8 (phase 114: was 64 -- the bus window seals 32 x 128 and a car grille
                                         //   32 x 16 were "shape / size rule" = never analysed; 8 keeps mip 1 >= 4 px, two blocks)
constexpr uint32_t kMaxDim    = 4096;    // W, H <= 4096 (CPU time / scratch memory)
constexpr int      kMaxMips   = 16;
// phase 115r (perf audit P4): kMinDim 8 let every tiny BC3 (decals, UI, 8 x 8 .. 16 x 16) into the uploader's hash / record / worker
// queue. The GAME's uploads now also need W x H >= kMinArea texels. 512 = the smallest real cut-out that needs the fix: the ATS
// Charger grille 32 x 16 (patched: tests\cutout_mips_test.cpp "ATS car grille"); the bus seal 32 x 128 = 4096. Smaller ones are
// analysed only when the CENSUS asks (its late read-back: CensusLate), never for a normal player.
constexpr uint32_t kMinArea   = 512;

enum Reason { kPatched = 0, kSoft, kCoverage, kNoLoss, kSize, kReasonCount };
inline const char* ReasonName(int r) {
    static const char* k[kReasonCount] = { "patched", "soft alpha", "coverage", "no loss", "size" };
    return r >= 0 && r < kReasonCount ? k[r] : "?";
}

inline uint32_t MipDim(uint32_t d0, int L) { const uint32_t d = d0 >> L; return d ? d : 1u; }
inline uint32_t Blocks(uint32_t d) { return (d + 3) / 4; }
inline uint32_t FullMips(uint32_t w, uint32_t h) { uint32_t m = 1, d = w > h ? w : h; while (d > 1) { d >>= 1; ++m; } return m; }

// The shape rule (Texture2D, BC3 / BC3_SRGB checked by the caller): one array slice, the full mip chain, 8 .. 4096 (phase 114: was 64).
inline bool ShapeOk(uint32_t w, uint32_t h, uint32_t mips, uint32_t arraySize) {
    if (arraySize != 1 || w < kMinDim || h < kMinDim || w > kMaxDim || h > kMaxDim) return false;
    return mips == FullMips(w, h) && (int)mips <= kMaxMips;
}
// phase 115r: the shape rule of the game's own uploads (and the normal late read-back): + the area floor kMinArea
inline bool UploadShapeOk(uint32_t w, uint32_t h, uint32_t mips, uint32_t arraySize) {
    return ShapeOk(w, h, mips, arraySize) && (uint64_t)w * h >= kMinArea;
}

// ---- phase 114: BC1 cut-outs -- does a BC1 mip 0 have any see-through texel? ------------------------------------------------
// A BC1 block (8 bytes: c0, c1 as RGB565, 16 x 2-bit indices) is see-through only in its 3-colour mode (c0 <= c1 as integers) at
// index 3. A BC1 texture whose blocks never use that has no alpha at all: a discard on its alpha never fires, so there is no coverage
// to lose (the ATS dealership pennant flags c2ec5608_a9d39be8: a 256 x 256 BC1 at 100 % on every mip). Counts the see-through texels
// of the w x h texels of mip 0 (rowPitch bytes per block row); stops once `stopAt` is passed (0 = count all).
// phase 115r (perf audit P2): per block ONE mask instead of 16 texel tests -- (bits & bits >> 1) keeps the low bit of every 2-bit
// index that is 3, & the valid-texel mask of an edge block (a mip narrower / shorter than 4) -> popcount.
inline uint32_t Bc1ValidMask(uint32_t cols, uint32_t rows) {   // cols / rows = valid texels of the block (1..4)
    uint32_t m = 0;
    for (uint32_t r = 0; r < rows && r < 4; ++r)
        for (uint32_t c = 0; c < cols && c < 4; ++c) m |= 1u << (2 * (r * 4 + c));
    return m;
}
inline int Bc1Pop(uint32_t v) { int n = 0; while (v) { v &= v - 1; ++n; } return n; }
// see-through texels of block rows by0..by1-1 (src = mip 0's start); returns as soon as the count reaches `limit`
inline uint64_t Bc1SeeThroughRows(const uint8_t* src, uint32_t rowPitch, uint32_t w, uint32_t h, uint32_t by0, uint32_t by1,
                                  uint64_t limit = ~0ull) {
    if (!src || w == 0 || h == 0) return 0;
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (by1 > bh) by1 = bh;
    const uint32_t lastCols = w - (bw - 1) * 4, lastRows = h - (bh - 1) * 4;
    const uint32_t full = 0x55555555u, edgeX = Bc1ValidMask(lastCols, 4);
    uint64_t n = 0;
    for (uint32_t by = by0; by < by1; ++by) {
        const uint8_t* row = src + (size_t)by * rowPitch;
        const bool lastRow = by + 1 == bh;
        const uint32_t rowMask = lastRow ? Bc1ValidMask(4, lastRows) : full, rowEdge = lastRow ? Bc1ValidMask(lastCols, lastRows) : edgeX;
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* b = row + (size_t)bx * 8;
            uint32_t c0, c1, bits;
            uint16_t s0, s1;
            memcpy(&s0, b, 2); memcpy(&s1, b + 2, 2); memcpy(&bits, b + 4, 4);   // (little-endian: the block's own byte order)
            c0 = s0; c1 = s1;
            if (c0 > c1) continue;                       // 4-colour mode: every texel opaque
            const uint32_t idx3 = bits & (bits >> 1) & (bx + 1 == bw ? rowEdge : rowMask);
            if (!idx3) continue;
            n += (uint64_t)Bc1Pop(idx3);
            if (n >= limit) return n;
        }
    }
    return n;
}
// stops once `stopAt` is passed (0 = count all)
inline uint64_t Bc1SeeThrough(const uint8_t* src, uint32_t rowPitch, uint32_t w, uint32_t h, uint64_t stopAt = 0) {
    return Bc1SeeThroughRows(src, rowPitch, w, h, 0, (h + 3) / 4, stopAt ? stopAt + 1 : ~0ull);
}
// any see-through texel at all (early-out at the FIRST one: all the census needs)
inline bool Bc1AnySeeThrough(const uint8_t* src, uint32_t rowPitch, uint32_t w, uint32_t h) {
    return Bc1SeeThroughRows(src, rowPitch, w, h, 0, (h + 3) / 4, 1) > 0;
}

// ---- BC3 alpha block (bytes 0..7 of a 16-byte block): a0, a1, 16 x 3-bit indices (texel t = row * 4 + col at bit 3t) -------
inline void DecodeAlphaBlock(const uint8_t* b, uint8_t out[16]) {
    const int a0 = b[0], a1 = b[1];
    uint8_t p[8];
    p[0] = (uint8_t)a0; p[1] = (uint8_t)a1;
    if (a0 > a1) {                                       // 8-level
        for (int i = 2; i < 8; ++i) p[i] = (uint8_t)(((8 - i) * a0 + (i - 1) * a1 + 3) / 7);
    } else {                                             // 6-level + 0 and 255
        for (int i = 2; i < 6; ++i) p[i] = (uint8_t)(((6 - i) * a0 + (i - 1) * a1 + 2) / 5);
        p[6] = 0; p[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int t = 0; t < 16; ++t) out[t] = p[(bits >> (3 * t)) & 7];
}
// A binary block: a0 = 255, a1 = 0 (8-level), index 0 = 255, index 1 = 0 -- exact. mask bit t = texel t opaque.
inline void EncodeBinaryBlock(uint16_t mask, uint8_t* b) {
    uint64_t bits = 0;
    for (int t = 0; t < 16; ++t) if (!(mask & (1u << t))) bits |= (uint64_t)1 << (3 * t);
    b[0] = 255; b[1] = 0;
    for (int i = 0; i < 6; ++i) b[2 + i] = (uint8_t)(bits >> (8 * i));
}

// 64-bit FNV-1a over the alpha halves of a mip (8 bytes per block, word-wise) + its size: the rebuild cache key.
// phase 115r (perf audit P1): the same hash in pieces -- HashAlphaBegin, then HashAlphaRows over block rows in order (the late
// read-back hashes mip 0 a few block rows per Present while it copies them; `rows` = the first of those rows, rowPitch apart)
inline uint64_t HashAlphaBegin(uint32_t w, uint32_t h, uint32_t mips) {
    uint64_t hsh = 1469598103934665603ull;
    hsh ^= ((uint64_t)w << 32) | h; hsh *= 1099511628211ull;
    hsh ^= mips; hsh *= 1099511628211ull;
    return hsh;
}
inline uint64_t HashAlphaRows(uint64_t hsh, const uint8_t* rows, uint32_t rowPitch, uint32_t w, uint32_t nRows) {
    const uint32_t bw = Blocks(w);
    for (uint32_t by = 0; by < nRows; ++by) {
        const uint8_t* row = rows + (size_t)by * rowPitch;
        for (uint32_t bx = 0; bx < bw; ++bx) { uint64_t v; memcpy(&v, row + bx * 16, 8); hsh ^= v; hsh *= 1099511628211ull; }
    }
    return hsh;
}
inline uint64_t HashAlpha(const uint8_t* src, uint32_t rowPitch, uint32_t w, uint32_t h, uint32_t mips) {
    return HashAlphaRows(HashAlphaBegin(w, h, mips), src, rowPitch, w, Blocks(h));
}

// Share of the texels of a BC3 mip (w x h valid texels) whose alpha passes the cut.
inline double Coverage(const uint8_t* src, uint32_t rowPitch, uint32_t w, uint32_t h) {
    const uint32_t bw = Blocks(w), bh = Blocks(h);
    uint64_t n = 0;
    uint8_t a[16];
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            DecodeAlphaBlock(src + (size_t)by * rowPitch + bx * 16, a);
            for (int t = 0; t < 16; ++t) {
                const uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                if (x < w && y < h && a[t] >= kCut) ++n;
            }
        }
    return (double)n / ((double)w * h);
}

// ---- the decision + the rebuilt mips -----------------------------------------------------------------------------------
struct Plan {
    Reason   reason = kSize;
    uint32_t w = 0, h = 0, mips = 0;
    double   cov0 = 0.0;                   // mip-0 coverage at the cut (share)
    double   covCut = -1.0;                // phase 111: the share of DECODED mip-0 texels passing the cut, also for a soft alpha
                                           //   (early stop: over the rows decoded); -1 = not decoded (shape rule). Census only.
    double   soft = 0.0;                   // share of non-binary mip-0 texels NOT on a hard edge (phase 107a; = softRaw when <= kMaxSoft)
    double   softRaw = 0.0;                // share of non-binary mip-0 texels, rims included (only up to the early stop when soft)
    double   minRatio = 1.0;               // the game rule's worst cov_L / cov0 over mips 2..(last >= 4 px)
    float    gameRule[kMaxMips] = {};      // the game rule's coverage per mip (box of mip 0 >= 0.5), share
    uint32_t target[kMaxMips] = {};        // K_L: opaque texels in our mip L
    std::vector<uint8_t> alpha[kMaxMips];  // our alpha halves per mip (8 bytes per block, row-major); empty = leave the game's
    size_t Bytes() const { size_t n = sizeof(*this); for (const auto& a : alpha) n += a.capacity(); return n; }
};

namespace detail {
inline uint32_t Dither16(uint32_t x, uint32_t y) {      // a fixed per-texel tie-break (no top-left bias)
    uint32_t v = x * 0x9E3779B1u ^ (y + 0x7F4A7C15u) * 0x85EBCA77u;
    v ^= v >> 15; v *= 0x2C1B3C6Du; v ^= v >> 12; v *= 0x297A2D39u; v ^= v >> 15;
    return v & 0xFFFFu;
}
}

// The phase-107a rim test: soft texels of mip 0 (alpha bytes, w x h) that have no hard-transparent (<= kSoftLo) AND hard-opaque
// (>= kSoftHi) texel both within r texels (clamped at the borders). Stops counting once `stopAt` is passed.
inline uint64_t CountSoftOffRim(const std::vector<uint8_t>& alp, uint32_t w, uint32_t h, int r, uint64_t stopAt) {
    uint64_t n = 0;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* row = &alp[(size_t)y * w];
        for (uint32_t x = 0; x < w; ++x) {
            const int v = row[x];
            if (v <= kSoftLo || v >= kSoftHi) continue;
            const uint32_t y0 = y >= (uint32_t)r ? y - r : 0, y1 = (std::min)(h - 1, y + (uint32_t)r);
            const uint32_t x0 = x >= (uint32_t)r ? x - r : 0, x1 = (std::min)(w - 1, x + (uint32_t)r);
            bool lo = false, hi = false;
            for (uint32_t yy = y0; yy <= y1 && !(lo && hi); ++yy) {
                const uint8_t* q = &alp[(size_t)yy * w];
                for (uint32_t xx = x0; xx <= x1; ++xx) { lo |= q[xx] <= kSoftLo; hi |= q[xx] >= kSoftHi; }
            }
            if (!(lo && hi) && ++n > stopAt) return n;
        }
    }
    return n;
}

// src = mip 0's BC3 data (rowPitch bytes per block row), w x h, mips = the texture's mip count. Fills `out`.
// rimR (phase 107a): the rim test's radius; 0 = the phase-77 rule (every soft texel counts, kMaxSoft of the raw share).
// covMin / lossKnown (phase 113, the sparse rule below): the lowest mip-0 coverage that is rebuilt (kCovMin = the phase-77 rule), and
// "the game's REAL mips are known to lose coverage" (skips the simulated game-rule gate kLossGate: the real mips decided).
inline void Analyse(const uint8_t* src, uint32_t rowPitch, uint32_t w, uint32_t h, uint32_t mips, Plan& out, int rimR = kRimR,
                    double covMin = kCovMin, bool lossKnown = false) {
    out = Plan();
    out.w = w; out.h = h; out.mips = mips;
    if (!src || mips < 2 || (int)mips > kMaxMips || w == 0 || h == 0 || rowPitch < Blocks(w) * 16) { out.reason = kSize; return; }
    // 1. decode mip 0's alpha (the classes below: >= kCut passes the cut, >= 128 = the game rule's half)
    const size_t N = (size_t)w * h;
    std::vector<uint8_t> alp(N);
    const uint32_t bw = Blocks(w), bh = Blocks(h);
    const uint64_t softMax = (uint64_t)(kMaxSoft * (double)N);
    const uint64_t rawMax = rimR > 0 ? (uint64_t)(kMaxSoftRaw * (double)N) : softMax;
    uint64_t soft = 0, n13 = 0;
    uint8_t a[16];
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            DecodeAlphaBlock(src + (size_t)by * rowPitch + bx * 16, a);
            for (int t = 0; t < 16; ++t) {
                const uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                if (x >= w || y >= h) continue;
                const int v = a[t];
                if (v > kSoftLo && v < kSoftHi) ++soft;
                alp[(size_t)y * w + x] = (uint8_t)v;
                n13 += v >= kCut;
            }
        }
        // early stop (most BC3 alphas): over the raw cap, or (phase 107a, the cost of the higher cap) more than half of the texels
        // soft after the first 1/8 of the rows -- an anti-aliased rim is a thin line, never half of a texture's area
        const uint64_t done = (uint64_t)(std::min)((by + 1) * 4, h) * w;
        if (soft > rawMax || (rimR > 0 && soft > softMax && 8 * (by + 1) >= bh && 2 * soft > done)) {
            out.softRaw = out.soft = (double)soft / (double)done;
            out.covCut = (double)n13 / (double)done;   // phase 111 (census: the thin measure of cut-out draws)
            out.reason = kSoft;
            return;
        }
    }
    out.softRaw = out.soft = (double)soft / N;
    out.covCut = (double)n13 / N;                        // phase 111
    if (soft > softMax) {                                // phase 107a: only an anti-aliased rim may be soft
        const uint64_t off = CountSoftOffRim(alp, w, h, rimR, softMax);
        out.soft = (double)off / N;
        if (off > softMax) { out.reason = kSoft; return; }
    }
    out.cov0 = (double)n13 / N;
    if (out.cov0 < covMin || out.cov0 <= 0.0 || out.cov0 > kCovMax) { out.reason = kCoverage; return; }

    // 2. per mip L: the mip-0 footprint counts (c13 = texels passing the cut, c128 = texels >= 128). Footprint of texel x on
    // a W0 -> W_L axis = [floor(x W0 / W_L), floor((x + 1) W0 / W_L)): a partition of mip 0 for any size (NPOT too). An
    // exactly halved axis (or 1 -> 1) sums the previous mip's 2 (1) cells; otherwise the level is counted from mip 0 directly.
    // (mip 0 itself stays as the 1-byte classes: 4 x less memory than two count arrays)
    std::vector<uint32_t> c13[kMaxMips], c128[kMaxMips];
    int lastGate = 0;                                    // the last mip with min(W_L, H_L) >= 4
    double minRatio = 1e9;
    for (int L = 1; L < (int)mips; ++L) {
        const uint32_t pw = MipDim(w, L - 1), ph = MipDim(h, L - 1), wl = MipDim(w, L), hl = MipDim(h, L);
        const size_t n = (size_t)wl * hl;
        c13[L].assign(n, 0); c128[L].assign(n, 0);
        const bool hx = pw == 2 * wl || (pw == 1 && wl == 1), hy = ph == 2 * hl || (ph == 1 && hl == 1);
        if (hx && hy) {
            const uint32_t sx = pw / wl, sy = ph / hl;
            for (uint32_t y = 0; y < hl; ++y)
                for (uint32_t x = 0; x < wl; ++x) {
                    uint32_t s13 = 0, s128 = 0;
                    for (uint32_t j = 0; j < sy; ++j)
                        for (uint32_t i = 0; i < sx; ++i) {
                            const size_t k = (size_t)(y * sy + j) * pw + (x * sx + i);
                            if (L == 1) { s13 += alp[k] >= kCut; s128 += alp[k] >= 128; }
                            else        { s13 += c13[L - 1][k]; s128 += c128[L - 1][k]; }
                        }
                    c13[L][(size_t)y * wl + x] = s13; c128[L][(size_t)y * wl + x] = s128;
                }
        } else {
            for (uint32_t y0 = 0; y0 < h; ++y0) {
                const uint32_t y = (uint32_t)((uint64_t)y0 * hl / h);
                for (uint32_t x0 = 0; x0 < w; ++x0) {
                    const uint32_t x = (uint32_t)((uint64_t)x0 * wl / w);
                    const uint8_t c = alp[(size_t)y0 * w + x0];
                    c13[L][(size_t)y * wl + x] += c >= kCut;
                    c128[L][(size_t)y * wl + x] += c >= 128;
                }
            }
        }
        // the game rule (box of mip 0 >= 0.5; texels >= 128 stand for the alpha sum: mip 0 is >= 97 % binary here)
        uint64_t kept = 0;
        for (uint32_t y = 0; y < hl; ++y) {
            const uint64_t ly = (uint64_t)(y + 1) * h / hl - (uint64_t)y * h / hl;
            for (uint32_t x = 0; x < wl; ++x) {
                const uint64_t lx = (uint64_t)(x + 1) * w / wl - (uint64_t)x * w / wl;
                kept += 2ull * c128[L][(size_t)y * wl + x] >= lx * ly;
            }
        }
        out.gameRule[L] = (float)((double)kept / n);
        if (wl >= 4 && hl >= 4) {
            lastGate = L;
            if (L >= 2) minRatio = (std::min)(minRatio, out.gameRule[L] / out.cov0);
        }
    }
    if (lastGate < 2) {                                  // (phase 114: min(W, H) 8..15 -> only mip 1 is >= 4 px: judged on it)
        minRatio = 1e9;
        for (int L = 1; L <= lastGate; ++L) minRatio = (std::min)(minRatio, out.gameRule[L] / out.cov0);
    }
    out.minRatio = minRatio > 1e8 ? 1.0 : minRatio;
    if (out.minRatio >= kLossGate && !lossKnown) { out.reason = kNoLoss; return; }
    std::vector<uint8_t>().swap(alp);

    // 3. our mips: exactly K_L = round(cov0 x W_L x H_L) texels opaque, the most covered footprints first (key: c13, then
    // c128, then a fixed dither); the 1x1 mip stays the game's
    std::vector<uint64_t> keys, tmp;
    std::vector<uint8_t> on;
    for (int L = 1; L < (int)mips; ++L) {
        const uint32_t wl = MipDim(w, L), hl = MipDim(h, L);
        const size_t n = (size_t)wl * hl;
        if (n <= 1) continue;
        const uint64_t k = (uint64_t)std::llround(out.cov0 * (double)n);
        out.target[L] = (uint32_t)k;
        keys.resize(n);
        for (uint32_t y = 0; y < hl; ++y)
            for (uint32_t x = 0; x < wl; ++x) {
                const size_t i = (size_t)y * wl + x;
                keys[i] = ((uint64_t)(std::min<uint32_t>)(c13[L][i], 0xFFFFFFu) << 40) |
                          ((uint64_t)(std::min<uint32_t>)(c128[L][i], 0xFFFFFFu) << 16) | detail::Dither16(x, y);
            }
        on.assign(n, 0);
        if (k >= n) std::fill(on.begin(), on.end(), (uint8_t)1);
        else if (k > 0) {
            tmp = keys;
            std::nth_element(tmp.begin(), tmp.begin() + (k - 1), tmp.end(), std::greater<uint64_t>());
            const uint64_t T = tmp[k - 1];
            uint64_t above = 0;
            for (size_t i = 0; i < n; ++i) if (keys[i] > T) { on[i] = 1; ++above; }
            for (size_t i = 0; i < n && above < k; ++i) if (keys[i] == T) { on[i] = 1; ++above; }   // equal keys: raster order
        }
        const uint32_t lbw = Blocks(wl), lbh = Blocks(hl);
        out.alpha[L].resize((size_t)lbw * lbh * 8);
        for (uint32_t by = 0; by < lbh; ++by)
            for (uint32_t bx = 0; bx < lbw; ++bx) {
                uint16_t m = 0;
                for (int t = 0; t < 16; ++t) {
                    const uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                    if (x < wl && y < hl && on[(size_t)y * wl + x]) m |= (uint16_t)(1u << t);   // outside the mip: 0
                }
                EncodeBinaryBlock(m, &out.alpha[L][((size_t)by * lbw + bx) * 8]);
            }
        if (L >= 2) { std::vector<uint32_t>().swap(c13[L - 1]); std::vector<uint32_t>().swap(c128[L - 1]); }
    }
    out.reason = kPatched;
}

// Mip L of a patched texture arrives: true = `dst` (rowPitch bytes per block row, as the source) holds the game's blocks with
// our alpha half; false = leave the game's data (no plan for this mip, or its own coverage is >= kKeepShare x cov0).
// gameCov (optional) = the game's coverage of this mip.
inline bool Replace(const Plan& p, int L, const uint8_t* src, uint32_t rowPitch, uint8_t* dst, double* gameCov = nullptr) {
    if (p.reason != kPatched || L < 1 || L >= (int)p.mips || p.alpha[L].empty() || !src || !dst) return false;
    const uint32_t wl = MipDim(p.w, L), hl = MipDim(p.h, L), bw = Blocks(wl), bh = Blocks(hl);
    if (rowPitch < bw * 16 || p.alpha[L].size() != (size_t)bw * bh * 8) return false;
    const double gc = Coverage(src, rowPitch, wl, hl);
    if (gameCov) *gameCov = gc;
    if (gc >= kKeepShare * p.cov0) return false;
    for (uint32_t by = 0; by < bh; ++by) {
        const uint8_t* s = src + (size_t)by * rowPitch;
        uint8_t* d = dst + (size_t)by * rowPitch;
        memcpy(d, s, (size_t)bw * 16);
        for (uint32_t bx = 0; bx < bw; ++bx) memcpy(d + bx * 16, &p.alpha[L][((size_t)by * bw + bx) * 8], 8);
    }
    return true;
}

// ---- phase 113: SPARSE alphas are judged on the game's REAL mips ---------------------------------------------------------------
// Phase 77 skipped every alpha under kCovMin (2 %) and the census (110b) called it "sparse: the game's mips lose it". The 2026-11-15
// 14:54 capture (census\c2ec5608_a9d39be8\tex_t6.dds, 2048 x 1024, cov0 1.55 %, far roof details) shows the opposite for that one:
// its game mips are box-filtered, the coverage at the 0.05 cut RISES (1.09 / 1.16 / 1.26 / 1.44 / 1.90 x cov0 on mips 1..5) -- no
// loss. A 1-texel line pattern with 0.5-threshold mips would lose it instead. So the worker, which holds the game's own mips, now
// measures them: a sparse texture whose real mips keep >= kLossGate of cov0 is "no loss" (census OK); one whose real mips lose it is
// rebuilt like any cut-out (Analyse with covMin = kCovMinSparse, lossKnown) -- our K_L = round(cov0 x texels) binary mips. An empty
// alpha (cov0 = 0) has nothing to lose. Mips where cov0 x texels < kSparseMinTexels are not judged (one texel more or less there is
// quantisation, not loss) -- also the minimum mip-0 count: a texture with fewer passing texels than that stays with the game.
constexpr double   kCovMinSparse   = 0.0;      // any coverage is rebuilt (the texel count below is the real floor)
constexpr double   kSparseMinTexels = 4.0;     // a mip is judged only where cov0 x its texels >= this
inline bool SparseCandidate(const Plan& p) { return p.reason == kCoverage && p.cov0 < kCovMin; }
// the worst cov_L / cov0 of the game's REAL mips 1..N-1 (mip[L] = its BC3 blocks, rp[L] its row pitch; null = not known), over the
// mips with cov0 x texels >= kSparseMinTexels. *judged = how many mips were measured. -1 = a mip missing, OR (phase 115r, audit A7)
// no mip qualifies: "nothing measured" is not "no loss" (it used to return 1.0 = SparseDecide's kNoLoss without a measurement).
inline double RealMinRatio(const uint8_t* const* mip, const uint32_t* rp, uint32_t w, uint32_t h, uint32_t mips, double cov0,
                           int* judged = nullptr) {
    double worst = 1.0;
    int n = 0;
    if (judged) *judged = 0;
    if (!(cov0 > 0.0)) return 1.0;
    for (uint32_t L = 1; L < mips && (int)L < kMaxMips; ++L) {
        const uint32_t wl = MipDim(w, (int)L), hl = MipDim(h, (int)L);
        if (cov0 * (double)wl * (double)hl < kSparseMinTexels) break;   // smaller mips: quantisation only
        if (!mip || !mip[L] || !rp || rp[L] < Blocks(wl) * 16) return -1.0;
        const double c = Coverage(mip[L], rp[L], wl, hl);
        worst = (std::min)(worst, c / cov0);
        ++n;
    }
    if (judged) *judged = n;
    return n > 0 ? worst : -1.0;
}
// the decision for a sparse candidate `p` (from Analyse with the phase-77 rule): 0 = keep kCoverage (real mips unknown or too few
// texels at mip 0), 1 = no loss (sets p.reason = kNoLoss, p.minRatio), 2 = rebuild (the caller runs Analyse(..., kCovMinSparse, true))
inline int SparseDecide(Plan& p, double realRatio, uint32_t w, uint32_t h) {
    if (!SparseCandidate(p)) return 0;
    if (!(p.cov0 > 0.0)) { p.reason = kNoLoss; p.minRatio = 1.0; return 1; }   // an empty alpha: nothing to lose
    if (realRatio < 0.0) return 0;                                             // a mip missing: not judged
    if (realRatio >= kLossGate) { p.reason = kNoLoss; p.minRatio = realRatio; return 1; }
    if (p.cov0 * (double)w * (double)h < kSparseMinTexels) return 0;         // a handful of texels: stays the game's (census BAD)
    return 2;
}

// ---- phase 113: the LATE path -- a texture the uploader never saw is read back from the GPU ----------------------------------------
// The 2026-11-15 14:54 census: trees (6d2c4e63_438d1a63, 19 % of its checked draws) and grass (8fcd62e3_a9d39be8, 14 %) drew textures
// with no phase-77 decision at all ("not seen by the uploader"), and more waited forever on "worker queue full" (a full queue drops
// the texture; it is never offered again). The game's DEFERRED context has its own UpdateSubresource code in d3d11.dll (measured:
// vtable 48 / ctx1 116 differ from the immediate context's; the mod hooks the immediate one) and CreateTexture2D's initial data is
// not hooked: whatever the path, a texture drawn by a discard PS without a decision is copied to a staging texture once (render
// thread, no stall: mapped DO_NOT_WAIT a few Presents later) and fed to the same mip-0 / mip-L pipeline as a game upload.
// The texture shape the late path accepts: BC3 Texture2D, DEFAULT usage (a late patch is an UpdateSubresource), one slice, no MSAA,
// the phase-77 shape rule. 0 = OK, 1 = not DEFAULT (can never be patched: census "cannot be patched"), 2 = shape / format (census
// "shape / size rule").
enum LateVerdict { kLateOk = 0, kLateNoWrite = 1, kLateShape = 2 };
// phase 115r: minArea = kMinArea for the normal read-back (as the uploader), 0 when the census asks (any kMinDim texture)
inline int LateShape(bool bc3, uint32_t w, uint32_t h, uint32_t mips, uint32_t arraySize, uint32_t samples, bool usageDefault,
                     uint32_t maxDim = kMaxDim, uint32_t minArea = 0) {
    if (!bc3 || samples != 1 || !ShapeOk(w, h, mips, arraySize) || w > maxDim || h > maxDim || (uint64_t)w * h < minArea)
        return kLateShape;
    return usageDefault ? kLateOk : kLateNoWrite;
}

// ---- phase 112r: the draw side's two pure rules (inject.cpp Cutout::OnDraw / AgeOut use them; tests\cutout_mips_test.cpp) ----------
// The t-slots a discard draw's bound views are looked at in: the PS's discard-alpha slots from the DXBC scan (census
// discardTex), or slots 0..7 when the scan knows none (0: not parsed / no texture reaches the discard / a non-DLAA build).
inline uint32_t DrawSlots(uint32_t discardSlots) { return discardSlots ? discardSlots : 0xFFu; }
// How many of the waiting entries, oldest first (`tick(i)` = GetTickCount64 when entry i started waiting, in push order), are
// older than maxAgeMs at `now` -- the prefix the age-out drops. Stops at the first young one (the order is the push order).
template <class TickOf> inline size_t AgedPrefix(size_t n, uint64_t now, uint64_t maxAgeMs, TickOf tick) {
    size_t k = 0;
    while (k < n && now >= tick(k) && now - tick(k) > maxAgeMs) ++k;
    return k;
}

} // namespace CutoutMips
