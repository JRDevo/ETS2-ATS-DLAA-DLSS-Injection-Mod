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
constexpr double   kCovMin    = 0.02;    // mip-0 coverage at the cut: 2 .. 60 % (a solid or an almost empty alpha: skip)
constexpr double   kCovMax    = 0.60;
constexpr double   kLossGate  = 0.70;    // the game rule keeps >= 70 % of cov0 on every mip 2..(last >= 4 px): no real loss, skip
constexpr double   kKeepShare = 0.85;    // a mip whose own (game) coverage is >= 85 % of cov0 is left as the game sent it
constexpr uint32_t kMinDim    = 64;      // min(W, H) >= 64
constexpr uint32_t kMaxDim    = 4096;    // W, H <= 4096 (CPU time / scratch memory)
constexpr int      kMaxMips   = 16;

enum Reason { kPatched = 0, kSoft, kCoverage, kNoLoss, kSize, kReasonCount };
inline const char* ReasonName(int r) {
    static const char* k[kReasonCount] = { "patched", "soft alpha", "coverage", "no loss", "size" };
    return r >= 0 && r < kReasonCount ? k[r] : "?";
}

inline uint32_t MipDim(uint32_t d0, int L) { const uint32_t d = d0 >> L; return d ? d : 1u; }
inline uint32_t Blocks(uint32_t d) { return (d + 3) / 4; }
inline uint32_t FullMips(uint32_t w, uint32_t h) { uint32_t m = 1, d = w > h ? w : h; while (d > 1) { d >>= 1; ++m; } return m; }

// The shape rule (Texture2D, BC3 / BC3_SRGB checked by the caller): one array slice, the full mip chain, 64 .. 4096.
inline bool ShapeOk(uint32_t w, uint32_t h, uint32_t mips, uint32_t arraySize) {
    if (arraySize != 1 || w < kMinDim || h < kMinDim || w > kMaxDim || h > kMaxDim) return false;
    return mips == FullMips(w, h) && (int)mips <= kMaxMips;
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
inline uint64_t HashAlpha(const uint8_t* src, uint32_t rowPitch, uint32_t w, uint32_t h, uint32_t mips) {
    uint64_t hsh = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { hsh ^= v; hsh *= 1099511628211ull; };
    mix(((uint64_t)w << 32) | h); mix(mips);
    const uint32_t bw = Blocks(w), bh = Blocks(h);
    for (uint32_t by = 0; by < bh; ++by) {
        const uint8_t* row = src + (size_t)by * rowPitch;
        for (uint32_t bx = 0; bx < bw; ++bx) { uint64_t v; memcpy(&v, row + bx * 16, 8); mix(v); }
    }
    return hsh;
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
    double   soft = 0.0;                   // share of non-binary mip-0 texels (only up to the early stop when soft)
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

// src = mip 0's BC3 data (rowPitch bytes per block row), w x h, mips = the texture's mip count. Fills `out`.
inline void Analyse(const uint8_t* src, uint32_t rowPitch, uint32_t w, uint32_t h, uint32_t mips, Plan& out) {
    out = Plan();
    out.w = w; out.h = h; out.mips = mips;
    if (!src || mips < 2 || (int)mips > kMaxMips || w == 0 || h == 0 || rowPitch < Blocks(w) * 16) { out.reason = kSize; return; }
    // 1. decode mip 0 into texel classes: 0 = cut away, 1 = passes the cut (alpha >= 13), 2 = also >= 128 (the game rule's half)
    const size_t N = (size_t)w * h;
    std::vector<uint8_t> cls(N);
    const uint32_t bw = Blocks(w), bh = Blocks(h);
    const uint64_t softMax = (uint64_t)(kMaxSoft * (double)N);
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
                const uint8_t c = v >= 128 ? 2 : (v >= kCut ? 1 : 0);
                cls[(size_t)y * w + x] = c;
                n13 += c != 0;
            }
        }
        if (soft > softMax) { out.soft = (double)soft / N; out.reason = kSoft; return; }   // early stop (most BC3 alphas)
    }
    out.soft = (double)soft / N;
    out.cov0 = (double)n13 / N;
    if (out.cov0 < kCovMin || out.cov0 > kCovMax) { out.reason = kCoverage; return; }

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
                            if (L == 1) { s13 += cls[k] != 0; s128 += cls[k] == 2; }
                            else        { s13 += c13[L - 1][k]; s128 += c128[L - 1][k]; }
                        }
                    c13[L][(size_t)y * wl + x] = s13; c128[L][(size_t)y * wl + x] = s128;
                }
        } else {
            for (uint32_t y0 = 0; y0 < h; ++y0) {
                const uint32_t y = (uint32_t)((uint64_t)y0 * hl / h);
                for (uint32_t x0 = 0; x0 < w; ++x0) {
                    const uint32_t x = (uint32_t)((uint64_t)x0 * wl / w);
                    const uint8_t c = cls[(size_t)y0 * w + x0];
                    c13[L][(size_t)y * wl + x] += c != 0;
                    c128[L][(size_t)y * wl + x] += c == 2;
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
    if (lastGate < 2) {                                  // (min(W, H) >= 64 makes mips 2..4 >= 4 px; kept for odd callers)
        minRatio = 1e9;
        for (int L = 1; L <= lastGate; ++L) minRatio = (std::min)(minRatio, out.gameRule[L] / out.cov0);
    }
    out.minRatio = minRatio > 1e8 ? 1.0 : minRatio;
    if (out.minRatio >= kLossGate) { out.reason = kNoLoss; return; }
    std::vector<uint8_t>().swap(cls);

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

} // namespace CutoutMips
