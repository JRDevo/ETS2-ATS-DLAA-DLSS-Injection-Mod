// Phase 77 unit test of src/cutout_mips.h: coverage-preserving binary alpha mips for cut-out BC3 textures.
// BC3 alpha block decode / binary encode round trip, the detector (soft, coverage, no-loss, shape), a synthetic chain-link fence
// (1- and 2-texel diagonal wires, period 16, 128x2048 like the ATS fence) with exact coverage per mip, colour bytes untouched,
// a mip the game already keeps left alone, NPOT and padded row pitches. When the captured ATS fence alpha mips exist
// (E:\ETS2-DLAA-Injector\captures\ats_flat_fence_frame1634.fence_dump\m0.png .. m11.png, outside git) they are run too, else
// skipped with a note (CI). Phase 107a: anti-aliased cut-outs (the rim test) + the real ETS2 birch / soft textures
// (captures\ets2_cutout_2026-11-14, also skipped when missing). No D3D, no game. Build: tests\build_cutout_mips_test.bat. Exit code 0 = every check passed.
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include "../src/cutout_mips.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fails; printf("   FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace CutoutMips;

// ---- test-side BC3 encoding: alpha = min/max 8-level, nearest index; colour = a recognisable byte pattern ----------------
static void EncodeAlphaAny(const uint8_t a[16], uint8_t* b) {
    int lo = 255, hi = 0;
    for (int t = 0; t < 16; ++t) { lo = std::min<int>(lo, a[t]); hi = std::max<int>(hi, a[t]); }
    if (hi == lo) { if (hi < 255) ++hi; else --lo; }
    b[0] = (uint8_t)hi; b[1] = (uint8_t)lo;
    int p[8] = { hi, lo };
    for (int i = 2; i < 8; ++i) p[i] = ((8 - i) * hi + (i - 1) * lo + 3) / 7;
    uint64_t bits = 0;
    for (int t = 0; t < 16; ++t) {
        int best = 0;
        for (int i = 1; i < 8; ++i) if (std::abs(p[i] - a[t]) < std::abs(p[best] - a[t])) best = i;
        bits |= (uint64_t)best << (3 * t);
    }
    for (int i = 0; i < 6; ++i) b[2 + i] = (uint8_t)(bits >> (8 * i));
}
static uint8_t ColourByte(uint32_t bx, uint32_t by, int i) { return (uint8_t)(bx * 31 + by * 17 + i * 7 + 1); }

// alpha (w x h) -> BC3 blocks with rowPitch (>= blocks * 16); padding bytes 0xCD
static std::vector<uint8_t> EncodeBc3(const std::vector<uint8_t>& al, uint32_t w, uint32_t h, uint32_t rowPitch) {
    const uint32_t bw = Blocks(w), bh = Blocks(h);
    std::vector<uint8_t> out((size_t)rowPitch * bh, 0xCD);
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx) {
            uint8_t a[16];
            for (int t = 0; t < 16; ++t) {
                const uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                a[t] = (x < w && y < h) ? al[(size_t)y * w + x] : 0;
            }
            uint8_t* blk = &out[(size_t)by * rowPitch + bx * 16];
            EncodeAlphaAny(a, blk);
            for (int i = 0; i < 8; ++i) blk[8 + i] = ColourByte(bx, by, i);
        }
    return out;
}
static std::vector<uint8_t> DecodeBc3(const uint8_t* src, uint32_t rowPitch, uint32_t w, uint32_t h) {
    std::vector<uint8_t> al((size_t)w * h);
    uint8_t a[16];
    for (uint32_t by = 0; by < Blocks(h); ++by)
        for (uint32_t bx = 0; bx < Blocks(w); ++bx) {
            DecodeAlphaBlock(src + (size_t)by * rowPitch + bx * 16, a);
            for (int t = 0; t < 16; ++t) {
                const uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                if (x < w && y < h) al[(size_t)y * w + x] = a[t];
            }
        }
    return al;
}
// the game's rule: mip L = box of the mip-0 alpha over the footprint >= 0.5, binary
static std::vector<uint8_t> GameMip(const std::vector<uint8_t>& a0, uint32_t w, uint32_t h, int L) {
    const uint32_t wl = MipDim(w, L), hl = MipDim(h, L);
    std::vector<uint64_t> sum((size_t)wl * hl, 0), cnt((size_t)wl * hl, 0);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = (size_t)((uint64_t)y * hl / h) * wl + (uint32_t)((uint64_t)x * wl / w);
            sum[i] += a0[(size_t)y * w + x]; ++cnt[i];
        }
    std::vector<uint8_t> m((size_t)wl * hl);
    for (size_t i = 0; i < m.size(); ++i) m[i] = 2 * sum[i] >= 255 * cnt[i] ? 255 : 0;
    return m;
}
static uint64_t CountCut(const std::vector<uint8_t>& al) { uint64_t n = 0; for (uint8_t v : al) n += v >= kCut; return n; }
static bool IsBinary(const std::vector<uint8_t>& al) { for (uint8_t v : al) if (v != 0 && v != 255) return false; return true; }

// chain-link: diagonal wires (x + y) % 16 < 1 (1 texel) and (x - y) % 16 < 2 (2 texels)
static std::vector<uint8_t> ChainLink(uint32_t w, uint32_t h) {
    std::vector<uint8_t> a((size_t)w * h, 0);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const bool on = ((x + y) % 16) < 1 || ((x + 1600 - y % 1600) % 16) < 2;
            a[(size_t)y * w + x] = on ? 255 : 0;
        }
    return a;
}

// phase 107a: leaves -- small discs (radius 2.5 .. 5 texels) on a jittered grid, alpha = 255 x clamp(0.5 - d / rim) with d the
// signed distance to the disc's edge: rim = 1 -> an ANTI-ALIASED cut-out (a ~1 texel soft rim around hard 0 / 255, like the SCS
// birch), rim = 12 on 48-texel cells -> a genuinely soft decal (a wide gradient: most soft texels see only one hard side, or none).
static std::vector<uint8_t> Leaves(uint32_t w, uint32_t h, double rim, int cell = 12, double r0 = 2.5, double rr = 2.5) {
    std::vector<uint8_t> a((size_t)w * h, 0);
    uint32_t seed = 777;
    auto rnd = [&seed] { seed = seed * 1664525u + 1013904223u; return (double)(seed >> 8) / 16777216.0; };
    for (uint32_t cy = 0; cy < h / cell; ++cy)
        for (uint32_t cx = 0; cx < w / cell; ++cx) {
            const double ox = cx * cell + 2 + rnd() * (cell - 4), oy = cy * cell + 2 + rnd() * (cell - 4), r = r0 + rnd() * rr;
            const int x0 = std::max(0, (int)(ox - r - rim - 1)), x1 = std::min((int)w - 1, (int)(ox + r + rim + 1));
            const int y0 = std::max(0, (int)(oy - r - rim - 1)), y1 = std::min((int)h - 1, (int)(oy + r + rim + 1));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const double d = std::sqrt((x + 0.5 - ox) * (x + 0.5 - ox) + (y + 0.5 - oy) * (y + 0.5 - oy)) - r;
                    const double v = std::min(1.0, std::max(0.0, 0.5 - d / rim));
                    uint8_t& t = a[(size_t)y * w + x];
                    t = std::max<uint8_t>(t, (uint8_t)std::lround(255.0 * v));
                }
        }
    return a;
}
static std::vector<uint8_t> LoadRaw(const char* path) {
    std::vector<uint8_t> b;
    FILE* f = fopen(path, "rb");
    if (!f) return b;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    b.resize(n > 0 ? (size_t)n : 0);
    if (n > 0 && fread(b.data(), 1, (size_t)n, f) != (size_t)n) b.clear();
    fclose(f);
    return b;
}

// Runs a whole texture through the plan: mip 0 + the game's mips (from `game`, alpha per mip) -> per-mip checks. Prints a table.
struct ChainResult { int patchedMips = 0; double ourMin = 1e9, gameMin = 1e9; };
static ChainResult RunTexture(const char* name, const std::vector<uint8_t>& a0, const std::vector<std::vector<uint8_t>>& game,
                              uint32_t w, uint32_t h, uint32_t mips, uint32_t pad, bool print) {
    ChainResult r;
    const uint32_t rp0 = Blocks(w) * 16 + pad;
    const std::vector<uint8_t> m0 = EncodeBc3(a0, w, h, rp0);
    Plan p;
    Analyse(m0.data(), rp0, w, h, mips, p);
    CHECK(p.reason == kPatched, "%s: reason %s (cov0 %.3f soft %.3f minRatio %.2f)", name, ReasonName(p.reason), p.cov0, p.soft,
          p.minRatio);
    if (p.reason != kPatched) return r;
    if (print) printf("  %s %ux%u mips %u: cov0 %.2f %%, game rule worst ratio %.2f\n    mip  size        game %%   ours %%  target  opaque\n",
                      name, w, h, mips, 100.0 * p.cov0, p.minRatio);
    for (int L = 1; L < (int)mips; ++L) {
        const uint32_t wl = MipDim(w, L), hl = MipDim(h, L), rp = Blocks(wl) * 16 + pad;
        const std::vector<uint8_t> src = EncodeBc3(game[L], wl, hl, rp);
        std::vector<uint8_t> dst(src.size(), 0xEE);
        double gc = -1.0;
        const bool rep = Replace(p, L, src.data(), rp, dst.data(), &gc);
        const std::vector<uint8_t> after = rep ? DecodeBc3(dst.data(), rp, wl, hl) : DecodeBc3(src.data(), rp, wl, hl);
        const uint64_t opaque = CountCut(after);
        const double n = (double)wl * hl;
        if (rep) {
            ++r.patchedMips;
            CHECK(IsBinary(after), "%s mip %d: output not binary", name, L);
            CHECK(std::fabs((double)opaque - (double)p.target[L]) <= 1.0, "%s mip %d: %llu opaque, target %u", name, L,
                  (unsigned long long)opaque, p.target[L]);
            CHECK(std::fabs((double)p.target[L] - p.cov0 * n) <= 1.0, "%s mip %d: target %u vs cov0 x n %.1f", name, L, p.target[L],
                  p.cov0 * n);
            bool colourOk = true, padOk = true;
            for (uint32_t by = 0; by < Blocks(hl); ++by) {
                for (uint32_t bx = 0; bx < Blocks(wl); ++bx)
                    for (int i = 8; i < 16; ++i) colourOk &= dst[(size_t)by * rp + bx * 16 + i] == src[(size_t)by * rp + bx * 16 + i];
                for (uint32_t k = Blocks(wl) * 16; k < rp; ++k) padOk &= dst[(size_t)by * rp + k] == 0xEE;   // padding not written
            }
            CHECK(colourOk, "%s mip %d: colour bytes changed", name, L);
            CHECK(padOk, "%s mip %d: row padding written", name, L);
        } else if (n > 1) {
            CHECK(gc < 0 || gc >= kKeepShare * p.cov0 || p.alpha[L].empty(), "%s mip %d left though the game lost coverage (%.3f)", name, L, gc);
        }
        if (n > 1 && wl >= 4 && hl >= 4) {
            r.ourMin = std::min(r.ourMin, opaque / n / p.cov0);   // replaced mips = cov0 exactly; kept ones >= kKeepShare
            if (gc >= 0) r.gameMin = std::min(r.gameMin, gc / p.cov0);
        }
        if (print) printf("    %2d  %4ux%-5u  %6.2f  %6.2f  %6u  %6llu%s\n", L, wl, hl, gc < 0 ? -1.0 : 100.0 * gc, 100.0 * opaque / n,
                          p.target[L], (unsigned long long)opaque, rep ? "  replaced" : "  game's");
    }
    return r;
}

// ---- WIC PNG reader (the captured fence; R channel = alpha) ------------------------------------------------------------
static bool LoadPngR(const wchar_t* path, std::vector<uint8_t>& out, uint32_t& w, uint32_t& h) {
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    bool ok = false;
    IWICBitmapDecoder* d = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d)) &&
        SUCCEEDED(d->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        UINT W = 0, H = 0;
        cv->GetSize(&W, &H);
        std::vector<uint8_t> px((size_t)W * H * 4);
        if (SUCCEEDED(cv->CopyPixels(nullptr, W * 4, (UINT)px.size(), px.data()))) {
            w = W; h = H; out.resize((size_t)W * H);
            for (size_t i = 0; i < out.size(); ++i) out[i] = px[i * 4];
            ok = true;
        }
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (d) d->Release();
    f->Release();
    return ok;
}

int main() {
    // ---- 1. block codec
    {
        const uint8_t b8[8] = { 200, 100, 0, 0, 0, 0, 0, 0 };   // 8-level, all index 0
        uint8_t a[16];
        DecodeAlphaBlock(b8, a);
        CHECK(a[0] == 200 && a[15] == 200, "8-level index 0");
        uint8_t b6[8] = { 50, 150, 0, 0, 0, 0, 0, 0 };          // 6-level: index 6 = 0, index 7 = 255
        b6[2] = 6 | (7 << 3);
        DecodeAlphaBlock(b6, a);
        CHECK(a[0] == 0 && a[1] == 255 && a[2] == 50, "6-level indices 6 / 7 / 0: %d %d %d", a[0], a[1], a[2]);
        uint32_t seed = 12345;
        bool exact = true;
        for (int it = 0; it < 5000; ++it) {
            seed = seed * 1664525u + 1013904223u;
            const uint16_t m = (uint16_t)(seed >> 8);
            uint8_t blk[8];
            EncodeBinaryBlock(m, blk);
            DecodeAlphaBlock(blk, a);
            for (int t = 0; t < 16; ++t) exact &= a[t] == ((m >> t) & 1 ? 255 : 0);
        }
        CHECK(exact, "binary block round trip not exact");
    }
    // ---- 2. shape rule
    CHECK(ShapeOk(128, 2048, 12, 1), "128x2048 / 12 mips is a candidate");
    CHECK(!ShapeOk(128, 2048, 11, 1), "partial mip chain");
    CHECK(!ShapeOk(128, 2048, 12, 2), "array");
    CHECK(ShapeOk(32, 2048, 12, 1), "phase 114: min dim 32 is a candidate now (was < 64)");
    CHECK(ShapeOk(32, 128, 8, 1) && ShapeOk(32, 16, 6, 1) && ShapeOk(8, 8, 4, 1), "phase 114: the bus seal 32x128, the grille 32x16, 8x8");
    CHECK(!ShapeOk(4, 256, 9, 1) && !ShapeOk(256, 4, 9, 1), "phase 114: min dim < 8 stays the size rule");
    CHECK(!ShapeOk(8192, 64, 14, 1), "> 4096");
    CHECK(ShapeOk(96, 64, 7, 1), "NPOT 96x64 with its full chain");

    // ---- 3. synthetic chain-link fence, 128x2048 (tight and padded row pitch)
    const uint32_t W = 128, H = 2048, MIPS = 12;
    const std::vector<uint8_t> fence = ChainLink(W, H);
    std::vector<std::vector<uint8_t>> game(MIPS);
    game[0] = fence;
    for (int L = 1; L < (int)MIPS; ++L) game[L] = GameMip(fence, W, H, L);
    printf("synthetic chain-link fence (1- and 2-texel wires, period 16):\n");
    const ChainResult cr = RunTexture("chain-link", fence, game, W, H, MIPS, 0, true);
    CHECK(cr.patchedMips >= 4, "chain-link: only %d mips replaced", cr.patchedMips);
    CHECK(cr.ourMin >= kKeepShare - 1e-9, "chain-link: our worst mip keeps %.2f of cov0", cr.ourMin);
    CHECK(cr.gameMin < 0.7, "chain-link: the game rule should lose coverage (worst %.2f)", cr.gameMin);
    RunTexture("chain-link padded pitch", fence, game, W, H, MIPS, 48, false);
    {   // the plan does not depend on the row pitch; the hash neither, and one alpha change changes the hash
        const uint32_t rpA = Blocks(W) * 16, rpB = rpA + 64;
        const std::vector<uint8_t> a = EncodeBc3(fence, W, H, rpA), b = EncodeBc3(fence, W, H, rpB);
        Plan pa, pb;
        Analyse(a.data(), rpA, W, H, MIPS, pa);
        Analyse(b.data(), rpB, W, H, MIPS, pb);
        bool same = pa.reason == pb.reason;
        for (int L = 0; L < (int)MIPS; ++L) same &= pa.alpha[L] == pb.alpha[L];
        CHECK(same, "row pitch changes the plan");
        CHECK(HashAlpha(a.data(), rpA, W, H, MIPS) == HashAlpha(b.data(), rpB, W, H, MIPS), "row pitch changes the hash");
        std::vector<uint8_t> f2 = fence;
        f2[5000] ^= 255;
        const std::vector<uint8_t> c = EncodeBc3(f2, W, H, rpA);
        CHECK(HashAlpha(a.data(), rpA, W, H, MIPS) != HashAlpha(c.data(), rpA, W, H, MIPS), "one texel does not change the hash");
    }
    {   // a mip whose game data keeps the coverage is left alone (dst untouched)
        const uint32_t rp0 = Blocks(W) * 16;
        const std::vector<uint8_t> m0 = EncodeBc3(fence, W, H, rp0);
        Plan p;
        Analyse(m0.data(), rp0, W, H, MIPS, p);
        const uint32_t w1 = MipDim(W, 1), h1 = MipDim(H, 1), rp1 = Blocks(w1) * 16;
        std::vector<uint8_t> keep((size_t)w1 * h1, 0);              // a mip 1 with exactly cov0: the game kept it
        const size_t want = (size_t)std::llround(p.cov0 * w1 * h1);
        for (size_t i = 0; i < want; ++i) keep[(i * 7919) % keep.size()] = 255;
        const std::vector<uint8_t> src = EncodeBc3(keep, w1, h1, rp1);
        std::vector<uint8_t> dst(src.size(), 0xEE);
        double gc = 0;
        CHECK(!Replace(p, 1, src.data(), rp1, dst.data(), &gc), "mip 1 replaced though the game keeps %.3f of %.3f", gc, p.cov0);
        bool untouched = true;
        for (uint8_t v : dst) untouched &= v == 0xEE;
        CHECK(untouched, "kept mip: dst written");
        CHECK(!Replace(p, (int)MIPS - 1, src.data(), rp1, dst.data()), "1x1 mip must stay the game's");
        CHECK(!Replace(p, 0, src.data(), rp1, dst.data()), "mip 0 must stay the game's");
    }

    // ---- 4. rejections
    {
        const uint32_t w = 256, h = 256, mips = 9, rp = Blocks(w) * 16;
        std::vector<uint8_t> grad((size_t)w * h), grey((size_t)w * h, 0), blobs((size_t)w * h, 0), solid((size_t)w * h, 255),
            sparse((size_t)w * h, 0);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                grad[(size_t)y * w + x] = (uint8_t)x;
                if ((x / 8 + y / 8) % 2) grey[(size_t)y * w + x] = 77;                // a 30 % grey mask
                if (x < 77) blobs[(size_t)y * w + x] = 255;                         // one wide band: no loss in the mips
            }
        sparse[1000] = 255;
        struct { const char* n; const std::vector<uint8_t>* a; Reason want; } cases[] = {
            { "soft gradient", &grad, kSoft }, { "30 % grey mask", &grey, kSoft }, { "wide band (no loss)", &blobs, kNoLoss },
            { "solid", &solid, kCoverage }, { "almost empty", &sparse, kCoverage } };
        for (auto& c : cases) {
            const std::vector<uint8_t> m = EncodeBc3(*c.a, w, h, rp);
            Plan p;
            Analyse(m.data(), rp, w, h, mips, p);
            CHECK(p.reason == c.want, "%s: %s, want %s (cov0 %.3f soft %.3f ratio %.2f)", c.n, ReasonName(p.reason), ReasonName(c.want),
                  p.cov0, p.soft, p.minRatio);
            for (int L = 0; L < kMaxMips; ++L) CHECK(p.alpha[L].empty(), "%s: rejected but mip %d built", c.n, L);
        }
    }

    // ---- 5. NPOT (96x40, below the 64 shape rule but Analyse itself must cope) and 96x64
    {
        const uint32_t sizes[][2] = { { 96, 40 }, { 96, 64 }, { 100, 68 } };
        for (auto& s : sizes) {
            const uint32_t w = s[0], h = s[1], mips = FullMips(w, h);
            const std::vector<uint8_t> a = ChainLink(w, h);
            std::vector<std::vector<uint8_t>> g(mips);
            for (int L = 1; L < (int)mips; ++L) g[L] = GameMip(a, w, h, L);
            char nm[64];
            snprintf(nm, sizeof(nm), "NPOT %ux%u", w, h);
            RunTexture(nm, a, g, w, h, mips, 16, false);
        }
    }

    // ---- 6. the captured ATS fence (outside git; CI skips)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const wchar_t* dir = L"E:\\ETS2-DLAA-Injector\\captures\\ats_flat_fence_frame1634.fence_dump\\";
        std::vector<std::vector<uint8_t>> m;
        uint32_t w0 = 0, h0 = 0;
        for (int L = 0; L < 16; ++L) {
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%lsm%d.png", dir, L);
            std::vector<uint8_t> a;
            uint32_t w = 0, h = 0;
            if (!LoadPngR(path, a, w, h)) break;
            if (L == 0) { w0 = w; h0 = h; }
            if (w != MipDim(w0, L) || h != MipDim(h0, L)) { printf("  real fence: m%d.png is %ux%u, unexpected\n", L, w, h); break; }
            m.push_back(a);
        }
        if (m.empty()) printf("NOTE: captured ATS fence not found (%ls) -- real-fence check skipped\n", dir);
        else if (m.size() != FullMips(w0, h0)) CHECK(false, "real fence: %zu mips found, want %u", m.size(), FullMips(w0, h0));
        else {
            printf("captured ATS fence (frame 1634, the game's own mips as uploaded):\n");
            const ChainResult rr = RunTexture("ATS fence", m[0], m, w0, h0, (uint32_t)m.size(), 0, true);
            CHECK(rr.patchedMips >= 4, "real fence: only %d mips replaced", rr.patchedMips);
            CHECK(rr.ourMin >= kKeepShare - 1e-9, "real fence: our worst mip keeps %.2f of cov0", rr.ourMin);
        }
        CoUninitialize();
    }

    // ---- 8. phase 107a: an anti-aliased cut-out qualifies (the rim test), a genuinely soft alpha still does not
    {
        const uint32_t w = 512, h = 256, mips = FullMips(w, h), rp = Blocks(w) * 16;
        const std::vector<uint8_t> aa = Leaves(w, h, 1.0), decal = Leaves(w, h, 12.0, 48, 6.0, 8.0);
        const std::vector<uint8_t> mAa = EncodeBc3(aa, w, h, rp), mDecal = EncodeBc3(decal, w, h, rp);
        Plan pOld, pNew, pDecal, pDecalOld;
        Analyse(mAa.data(), rp, w, h, mips, pOld, 0);
        Analyse(mAa.data(), rp, w, h, mips, pNew);
        Analyse(mDecal.data(), rp, w, h, mips, pDecalOld, 0);
        Analyse(mDecal.data(), rp, w, h, mips, pDecal);
        printf("phase 107a synthetic leaves %ux%u: AA rim raw soft %.1f %% off-rim %.2f %% cov0 %.1f %% -> old rule %s, rim rule %s; "
               "wide-gradient decal raw soft %.1f %% off-rim %.1f %% -> %s\n", w, h, 100 * pNew.softRaw, 100 * pNew.soft, 100 * pNew.cov0,
               ReasonName(pOld.reason), ReasonName(pNew.reason), 100 * pDecal.softRaw, 100 * pDecal.soft, ReasonName(pDecal.reason));
        CHECK(pNew.softRaw > kMaxSoft && pNew.softRaw < kMaxSoftRaw, "AA leaves: the raw soft share %.3f should be between the limits",
              pNew.softRaw);
        CHECK(pOld.reason == kSoft, "AA leaves: the phase-77 rule should call them soft (%s)", ReasonName(pOld.reason));
        CHECK(pNew.reason == kPatched, "AA leaves: the rim rule should patch them (%s, off-rim %.4f, ratio %.2f)", ReasonName(pNew.reason),
              pNew.soft, pNew.minRatio);
        CHECK(pNew.soft <= kMaxSoft, "AA leaves: off-rim soft share %.4f", pNew.soft);
        CHECK(pDecal.softRaw > kMaxSoft && pDecal.softRaw < kMaxSoftRaw, "decal: raw soft share %.3f should be between the limits (the rim "
              "test decides)", pDecal.softRaw);
        CHECK(pDecal.reason == kSoft && pDecalOld.reason == kSoft, "decal (wide gradient): soft under both rules (%s / %s)",
              ReasonName(pDecal.reason), ReasonName(pDecalOld.reason));
        for (int L = 0; L < kMaxMips; ++L) CHECK(pDecal.alpha[L].empty(), "decal: rejected but mip %d built", L);
        // the whole chain: our mips keep the coverage the game's 0.5-threshold box loses
        std::vector<std::vector<uint8_t>> g(mips);
        for (int L = 1; L < (int)mips; ++L) g[L] = GameMip(aa, w, h, L);
        const ChainResult lr = RunTexture("AA leaves", aa, g, w, h, mips, 0, true);
        CHECK(lr.patchedMips >= 3, "AA leaves: only %d mips replaced", lr.patchedMips);
        CHECK(lr.ourMin >= kKeepShare - 1e-9, "AA leaves: our worst mip keeps %.2f of cov0", lr.ourMin);
        // a rim wider than kMaxSoftRaw allows (every texel soft): early stop, soft
        std::vector<uint8_t> grey((size_t)w * h, 100);
        const std::vector<uint8_t> mg = EncodeBc3(grey, w, h, rp);
        Plan pg;
        Analyse(mg.data(), rp, w, h, mips, pg);
        CHECK(pg.reason == kSoft && pg.softRaw > kMaxSoftRaw, "flat grey: soft by the raw share (%s, %.3f)", ReasonName(pg.reason), pg.softRaw);
    }
    // ---- 9. phase 107a, real textures from ETS2 frame 7916 (captures\ets2_cutout_2026-11-14, outside git; CI skips): the birch
    //         leaves (PS 438d1a63, discard; the game's mips lose 35.5 -> 20 % coverage) now qualify; a gloss alpha (PS b67da69f,
    //         no discard) and a blended wire alpha (PS a5aa1c2d) stay soft
    {
        const char* dir = "E:\\ETS2-DLAA-Injector\\captures\\ets2_cutout_2026-11-14\\";
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%sbirch_13758_2048x1024_mip0.bc3", dir);
        const std::vector<uint8_t> b0 = LoadRaw(path);
        const uint32_t w = 2048, h = 1024, mips = 12, rp = Blocks(w) * 16;
        if (b0.size() < (size_t)rp * Blocks(h)) printf("NOTE: real ETS2 textures not found (%s) -- section 9 skipped\n", dir);
        else {
            Plan pOld, p;
            Analyse(b0.data(), rp, w, h, mips, pOld, 0);
            Analyse(b0.data(), rp, w, h, mips, p);
            printf("real birch leaves 2048x1024: raw soft %.2f %% off-rim %.2f %% cov0 %.1f %% -> old rule %s, rim rule %s\n",
                   100 * p.softRaw, 100 * p.soft, 100 * p.cov0, ReasonName(pOld.reason), ReasonName(p.reason));
            CHECK(pOld.reason == kSoft, "birch: the phase-77 rule called it soft (%s)", ReasonName(pOld.reason));
            CHECK(p.reason == kPatched, "birch: the rim rule patches it (%s, off-rim %.4f)", ReasonName(p.reason), p.soft);
            int replaced = 0;
            double worst = 1e9;
            for (int L = 1; L < (int)mips && p.reason == kPatched; ++L) {
                snprintf(path, sizeof(path), "%sbirch_13758_2048x1024_mip%d.bc3", dir, L);
                const std::vector<uint8_t> g = LoadRaw(path);
                const uint32_t wl = MipDim(w, L), hl = MipDim(h, L), rl = Blocks(wl) * 16;
                if (g.size() < (size_t)rl * Blocks(hl)) { CHECK(false, "birch mip %d missing", L); break; }
                std::vector<uint8_t> dst(g.size());
                double gc = 0;
                const bool rep = Replace(p, L, g.data(), rl, dst.data(), &gc);
                const double ours = rep ? Coverage(dst.data(), rl, wl, hl) : gc;
                if (wl >= 4 && hl >= 4) worst = std::min(worst, ours / p.cov0);
                replaced += rep;
                printf("    mip %2d %4ux%-4u game %5.1f %% -> %5.1f %%%s\n", L, wl, hl, 100 * gc, 100 * ours, rep ? "  replaced" : "");
            }
            CHECK(replaced >= 4, "birch: only %d mips replaced", replaced);
            CHECK(worst >= kKeepShare - 1e-9, "birch: worst mip keeps %.2f of cov0", worst);
            const char* soft[] = { "soft_gloss_12291_1024x1024_m11.bc3", "soft_wire_13224_1024x1024_m11.bc3" };
            for (const char* s : soft) {
                snprintf(path, sizeof(path), "%s%s", dir, s);
                const std::vector<uint8_t> m = LoadRaw(path);
                if (m.size() < (size_t)Blocks(1024) * 16 * Blocks(1024)) { CHECK(false, "%s missing", s); continue; }
                Plan q;
                Analyse(m.data(), Blocks(1024) * 16, 1024, 1024, 11, q);
                printf("real %s: raw soft %.2f %% off-rim %.2f %% -> %s\n", s, 100 * q.softRaw, 100 * q.soft, ReasonName(q.reason));
                CHECK(q.reason == kSoft, "%s: a genuinely soft alpha must stay skipped (%s)", s, ReasonName(q.reason));
            }
            // phase 109f: kRimR 2 -> 3. Two leaf textures with 3-4 texel anti-aliased rims: soft at radius 2, patched at kRimR.
            struct Leaf { const char* f; uint32_t w, h, m; } leaves[] = {
                { "leaf_438d1a63_18072_1024x1024_m11.bc3", 1024, 1024, 11 }, { "leaf_a9d39be8_20237_1024x512_m11.bc3", 1024, 512, 11 } };
            CHECK(kRimR == 3, "phase 109f: kRimR is %d, want 3", kRimR);
            for (const Leaf& lf : leaves) {
                snprintf(path, sizeof(path), "%s%s", dir, lf.f);
                const std::vector<uint8_t> m = LoadRaw(path);
                if (m.size() < (size_t)Blocks(lf.w) * 16 * Blocks(lf.h)) { printf("NOTE: %s missing -- 109f leaf check skipped\n", lf.f); continue; }
                Plan q2, q3;
                Analyse(m.data(), Blocks(lf.w) * 16, lf.w, lf.h, lf.m, q2, 2);
                Analyse(m.data(), Blocks(lf.w) * 16, lf.w, lf.h, lf.m, q3);
                printf("real %s: radius 2 %s (off-rim %.2f %%), kRimR %d %s (off-rim %.2f %%, raw %.2f %%, cov0 %.1f %%)\n", lf.f,
                       ReasonName(q2.reason), 100 * q2.soft, kRimR, ReasonName(q3.reason), 100 * q3.soft, 100 * q3.softRaw, 100 * q3.cov0);
                CHECK(q2.reason == kSoft, "%s: radius 2 should still call it soft (%s)", lf.f, ReasonName(q2.reason));
                CHECK(q3.reason == kPatched, "%s: kRimR %d must patch the leaf (%s)", lf.f, kRimR, ReasonName(q3.reason));
            }
        }
    }

    // ---- 7. CPU cost (printed, not checked): hash + Analyse of a patched texture, and of a soft one (the early stop)
    {
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        const uint32_t sz[][2] = { { 128, 2048 }, { 512, 512 }, { 1024, 1024 }, { 2048, 2048 } };
        for (auto& s : sz) {
            const uint32_t w = s[0], h = s[1], rp = Blocks(w) * 16;
            const std::vector<uint8_t> m = EncodeBc3(ChainLink(w, h), w, h, rp);
            std::vector<uint8_t> soft((size_t)w * h);
            for (size_t i = 0; i < soft.size(); ++i) soft[i] = (uint8_t)(i * 7);
            const std::vector<uint8_t> ms = EncodeBc3(soft, w, h, rp);
            Plan p, q;
            QueryPerformanceCounter(&t0);
            const uint64_t hs = HashAlpha(m.data(), rp, w, h, FullMips(w, h));
            Analyse(m.data(), rp, w, h, FullMips(w, h), p);
            QueryPerformanceCounter(&t1);
            const double msP = 1000.0 * (t1.QuadPart - t0.QuadPart) / f.QuadPart;
            QueryPerformanceCounter(&t0);
            Analyse(ms.data(), rp, w, h, FullMips(w, h), q);
            QueryPerformanceCounter(&t1);
            printf("cpu: %ux%u hash + analyse %.2f ms (%s, %.1f KB plan), soft texture %.3f ms (%s) [hash %08x]\n", w, h, msP,
                   ReasonName(p.reason), p.Bytes() / 1024.0, 1000.0 * (t1.QuadPart - t0.QuadPart) / f.QuadPart,
                   ReasonName(q.reason), (unsigned)hs);
        }
    }

    // ---- phase 112r: the draw side's rules -- the slot plan (the PS's discard-alpha slots, else 0..7) and the pending age-out
    {
        CHECK(DrawSlots(0) == 0xFFu, "no discard slots known -> slots 0..7 (the pre-112r fetch)");
        CHECK(DrawSlots(1u << 6) == (1u << 6) && DrawSlots(0x12u) == 0x12u, "known discard slots -> exactly those (t6; t1 + t4)");
        CHECK(DrawSlots(1u << 12) == (1u << 12), "a discard slot above 7 is looked at too (the old fetch never saw t8..)");
        const uint64_t ticks[5] = { 1000, 2000, 30000, 61500, 70000 };
        auto tk = [&](size_t i) { return ticks[i]; };
        CHECK(AgedPrefix(5, 62000, 60000, tk) == 1, "age-out at 62 s: only the 1 s entry is older than 60 s (%zu)",
              AgedPrefix(5, 62000, 60000, tk));
        CHECK(AgedPrefix(5, 85000, 60000, tk) == 2, "age-out at 85 s: the 1 s and 2 s entries (%zu)", AgedPrefix(5, 85000, 60000, tk));
        CHECK(AgedPrefix(5, 200000, 60000, tk) == 5, "age-out much later: every entry -> the pending count returns to 0");
        CHECK(AgedPrefix(5, 50000, 60000, tk) == 0 && AgedPrefix(0, 1000000, 60000, tk) == 0, "nothing old / nothing waiting: none");
        CHECK(AgedPrefix(5, 500, 60000, tk) == 0, "a clock behind the entries (tick wrap guard): none dropped");
        const uint64_t mixed[3] = { 1000, 100000, 2000 };   // (push order is the age order; a young entry stops the prefix)
        CHECK(AgedPrefix(3, 150000, 60000, [&](size_t i) { return mixed[i]; }) == 1, "the prefix stops at the first young entry");
    }

    // ---- phase 113: SPARSE alphas judged on the game's REAL mips, and the late read-back's shape rule ----------------------------
    {
        // a sparse line pattern: 1-texel vertical lines every 64 texels on 1024 x 1024 (cov0 1.56 %, under kCovMin)
        const uint32_t w = 1024, h = 1024, mips = FullMips(w, h), rp = Blocks(w) * 16;
        std::vector<uint8_t> a0((size_t)w * h, 0);
        for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; x += 64) a0[(size_t)y * w + x] = 255;
        const std::vector<uint8_t> b0 = EncodeBc3(a0, w, h, rp);
        auto box = [&](int L) {                          // game mips that KEEP the lines (a max over the footprint: like the real roof
            const uint32_t wl = MipDim(w, L), hl = MipDim(h, L);   //   texture below, whose coverage at the cut rises with the mip)
            std::vector<uint8_t> m((size_t)wl * hl, 0);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    const size_t i = (size_t)((uint64_t)y * hl / h) * wl + (uint32_t)((uint64_t)x * wl / w);
                    m[i] = std::max(m[i], a0[(size_t)y * w + x]);
                }
            return m;
        };
        std::vector<std::vector<uint8_t>> thr(mips), avg(mips);
        const uint8_t* mt[kMaxMips] = {}; const uint8_t* ma[kMaxMips] = {}; uint32_t rps[kMaxMips] = {};
        for (uint32_t L = 1; L < mips; ++L) {
            const uint32_t wl = MipDim(w, (int)L), hl = MipDim(h, (int)L);
            rps[L] = Blocks(wl) * 16;
            thr[L] = EncodeBc3(GameMip(a0, w, h, (int)L), wl, hl, rps[L]);
            avg[L] = EncodeBc3(box((int)L), wl, hl, rps[L]);
            mt[L] = thr[L].data(); ma[L] = avg[L].data();
        }
        Plan p;
        Analyse(b0.data(), rp, w, h, mips, p);
        CHECK(p.reason == kCoverage && SparseCandidate(p) && p.cov0 > 0.015 && p.cov0 < 0.017,
              "sparse lines: the phase-77 rule says coverage (cov0 %.4f) -> a sparse candidate", p.cov0);
        int judgedT = 0, judgedA = 0;
        const double rT = RealMinRatio(mt, rps, w, h, mips, p.cov0, &judgedT), rA = RealMinRatio(ma, rps, w, h, mips, p.cov0, &judgedA);
        printf("   phase 113 sparse lines (cov0 %.2f %%): real mips with the 0.5 threshold -> worst %.2f x cov0 over %d mips; line-keeping "
               "-> %.2f over %d\n", 100.0 * p.cov0, rT, judgedT, rA, judgedA);
        CHECK(rT < kLossGate && judgedT >= 3, "threshold mips LOSE the sparse lines (%.2f over %d mips)", rT, judgedT);
        CHECK(rA >= kLossGate && judgedA >= 3, "line-keeping mips keep them (%.2f over %d mips)", rA, judgedA);
        Plan pa = p;
        CHECK(SparseDecide(pa, rA, w, h) == 1 && pa.reason == kNoLoss, "line-keeping game mips -> no loss (%s)", ReasonName(pa.reason));
        Plan pt = p;
        CHECK(SparseDecide(pt, rT, w, h) == 2, "threshold game mips -> rebuild");
        Plan pr;
        Analyse(b0.data(), rp, w, h, mips, pr, kRimR, kCovMinSparse, true);
        CHECK(pr.reason == kPatched, "the sparse rebuild patches it (%s)", ReasonName(pr.reason));
        double worst = 9.0;
        int rep = 0;
        for (uint32_t L = 1; L < mips && pr.reason == kPatched; ++L) {
            const uint32_t wl = MipDim(w, (int)L), hl = MipDim(h, (int)L);
            if (pr.cov0 * (double)wl * hl < kSparseMinTexels) break;
            std::vector<uint8_t> out(thr[L].size());
            const uint8_t* use = thr[L].data();
            if (Replace(pr, (int)L, thr[L].data(), rps[L], out.data())) { use = out.data(); ++rep; }
            worst = std::min(worst, Coverage(use, rps[L], wl, hl) / pr.cov0);
        }
        CHECK(rep >= 3 && worst >= kKeepShare - 1e-9, "sparse rebuild: %d mips replaced, the worst keeps %.2f of cov0", rep, worst);
        // an empty alpha, a handful of texels, a missing mip
        Plan pe; pe.reason = kCoverage; pe.cov0 = 0.0;
        CHECK(SparseDecide(pe, 1.0, w, h) == 1 && pe.reason == kNoLoss, "an empty alpha has nothing to lose -> no loss");
        Plan pf; pf.reason = kCoverage; pf.cov0 = 2.0 / ((double)w * h);
        CHECK(SparseDecide(pf, 0.0, w, h) == 0 && pf.reason == kCoverage, "two texels that the mips lose: too few to rebuild, kept");
        const uint8_t* gap[kMaxMips] = {};
        CHECK(RealMinRatio(gap, rps, w, h, mips, p.cov0) < 0.0, "a missing real mip -> not judged (-1)");
        Plan pm2 = p;
        CHECK(SparseDecide(pm2, -1.0, w, h) == 0 && pm2.reason == kCoverage, "not judged -> stays coverage (census: sparse)");
        Plan pn; pn.reason = kPatched; pn.cov0 = 0.01;
        CHECK(!SparseCandidate(pn) && SparseDecide(pn, 0.0, w, h) == 0, "only a phase-77 coverage rejection under 2 %% is a candidate");
        // the late read-back's shape rule
        CHECK(LateShape(true, 1024, 1024, 11, 1, 1, true) == kLateOk, "late path: a DEFAULT BC3 1024 x 1024 full chain is read back");
        CHECK(LateShape(true, 1024, 1024, 11, 1, 1, false) == kLateNoWrite, "late path: IMMUTABLE / DYNAMIC -> cannot be patched");
        CHECK(LateShape(true, 1024, 1024, 5, 1, 1, true) == kLateShape && LateShape(true, 1024, 1024, 11, 4, 1, true) == kLateShape &&
              LateShape(false, 1024, 1024, 11, 1, 1, true) == kLateShape && LateShape(true, 8192, 512, 14, 1, 1, true) == kLateShape &&
              LateShape(true, 1024, 1024, 11, 1, 4, true) == kLateShape, "late path: short chain / array / not BC3 / too big / MSAA -> shape");
        CHECK(LateShape(true, 4096, 1024, 13, 1, 1, true, 2048) == kLateShape, "late path: over cutout_max_dim -> shape");
    }

    // ---- phase 113, local only: the three cut-out textures of the 2026-11-15 14:54 census (DDS read back from the GPU, every mip) ----
    {
        const char* dir = "E:\\ETS2-DLAA-Injector\\captures\\census_2026-11-15\\shimmer_20261115_145426\\census\\";
        struct T { const char* pair; const char* what; };
        const T ts[3] = { { "c2ec5608_a9d39be8", "far roof details, cov0 1.55 % (census: sparse)" },
                          { "6d2c4e63_438d1a63", "tree leaves 4096 x 1024 (census: not seen)" },
                          { "8fcd62e3_a9d39be8", "grass 1024 x 1024 (census: not seen)" } };
        for (const T& t : ts) {
            char path[512];
            snprintf(path, sizeof(path), "%s%s\\tex_t6.dds", dir, t.pair);
            FILE* f = fopen(path, "rb");
            if (!f) { printf("NOTE: %s not found -- phase 113 real-texture check skipped\n", path); continue; }
            std::vector<uint8_t> b;
            fseek(f, 0, SEEK_END); b.resize((size_t)ftell(f)); fseek(f, 0, SEEK_SET);
            const bool rd = fread(b.data(), 1, b.size(), f) == b.size();
            fclose(f);
            uint32_t hh = 0, ww = 0, mm = 0, fmt = 0;
            if (!rd || b.size() < 148 || memcmp(b.data(), "DDS ", 4) || memcmp(b.data() + 84, "DX10", 4)) { CHECK(false, "%s: not a DX10 DDS", path); continue; }
            memcpy(&hh, b.data() + 12, 4); memcpy(&ww, b.data() + 16, 4); memcpy(&mm, b.data() + 28, 4); memcpy(&fmt, b.data() + 128, 4);
            CHECK(fmt == 77 || fmt == 78, "%s: BC3 (%u)", t.pair, fmt);
            std::vector<const uint8_t*> mp(kMaxMips, nullptr);
            uint32_t rps[kMaxMips] = {};
            size_t off = 148;
            for (uint32_t L = 0; L < mm && (int)L < kMaxMips; ++L) {
                const uint32_t wl = MipDim(ww, (int)L), hl = MipDim(hh, (int)L), n = Blocks(wl) * 16 * Blocks(hl);
                if (off + n > b.size()) break;
                mp[L] = b.data() + off; rps[L] = Blocks(wl) * 16; off += n;
            }
            Plan p;
            Analyse(mp[0], rps[0], ww, hh, mm, p);
            const char* before = ReasonName(p.reason);
            int dec = -1;
            double ratio = 1.0;
            if (SparseCandidate(p)) {
                ratio = RealMinRatio(mp.data(), rps, ww, hh, mm, p.cov0);
                dec = SparseDecide(p, ratio, ww, hh);
            }
            printf("   phase 113 real %s (%s): %ux%u, %u mips, phase-77 rule %s (cov0 %.2f %%, soft %.2f %%)%s -> %s\n", t.pair, t.what, ww, hh,
                   mm, before, 100.0 * p.cov0, 100.0 * p.soft, dec >= 0 ? " sparse, real mips judged" : "", ReasonName(p.reason));
            if (!strcmp(t.pair, "c2ec5608_a9d39be8")) {
                CHECK(dec == 1 && p.reason == kNoLoss && ratio >= 1.0,
                      "the real sparse roof texture: its box-filtered game mips keep (raise) the coverage -> no loss (ratio %.2f)", ratio);
            } else {
                CHECK(p.reason != kSize, "%s: a decision the census counts (not the shape rule)", t.pair);
            }
        }
    }

    // ---- phase 114: BC1 see-through texels (synthetic) ------------------------------------------------------------------------------
    {
        // two blocks: block 0 in 4-colour mode (c0 > c1: opaque whatever the indices), block 1 in 3-colour mode with 5 texels at index 3
        uint8_t b[16] = {};
        b[0] = 0x00; b[1] = 0xF8; b[2] = 0x1F; b[3] = 0x00;   // c0 0xF800 > c1 0x001F
        b[4] = b[5] = b[6] = b[7] = 0xFF;                      // all index 3 (= the 1/3 colour in 4-colour mode, opaque)
        b[8] = 0x1F; b[9] = 0x00; b[10] = 0x00; b[11] = 0xF8;  // c0 0x001F <= c1 0xF800: 3-colour mode
        const uint32_t idx = 3u | (3u << 2) | (3u << 4) | (3u << 6) | (3u << 30);   // texels 0..3 and 15 see-through
        memcpy(b + 12, &idx, 4);
        CHECK(Bc1SeeThrough(b, 16, 8, 4) == 5, "BC1: 5 see-through texels (3-colour block, index 3); a 4-colour block counts none");
        CHECK(Bc1SeeThrough(b, 16, 4, 4) == 0, "BC1: a 4-colour block alone is opaque");
        CHECK(Bc1SeeThrough(b, 16, 6, 4) == 2, "BC1: texels outside the mip (w 6) are not counted");
        CHECK(Bc1SeeThrough(b, 16, 8, 4, 1) >= 2, "BC1: stops once past stopAt");
        uint8_t eq[8] = { 0x34, 0x12, 0x34, 0x12, 0, 0, 0, 0xC0 };   // c0 == c1 -> 3-colour mode: texel 15 at index 3 is see-through
        CHECK(Bc1SeeThrough(eq, 8, 4, 4) == 1, "BC1: c0 == c1 is the 3-colour mode");
        // phase 115r (P2): the per-block mask scan == the per-texel reference on random blocks, every edge size; the first-hit test
        auto ref = [](const uint8_t* src, uint32_t rpB, uint32_t w, uint32_t h) {
            uint64_t n = 0;
            for (uint32_t by = 0; by < (h + 3) / 4; ++by)
                for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx) {
                    const uint8_t* q = src + (size_t)by * rpB + bx * 8;
                    const uint32_t c0 = q[0] | (q[1] << 8), c1 = q[2] | (q[3] << 8);
                    if (c0 > c1) continue;
                    const uint32_t bits = q[4] | (q[5] << 8) | (q[6] << 16) | ((uint32_t)q[7] << 24);
                    for (int t = 0; t < 16; ++t)
                        if (bx * 4 + (t & 3) < w && by * 4 + (t >> 2) < h && ((bits >> (2 * t)) & 3u) == 3u) ++n;
                }
            return n;
        };
        uint32_t rng = 12345u;
        auto rnd = [&rng] { rng = rng * 1664525u + 1013904223u; return (uint8_t)(rng >> 24); };
        int bad = 0, any = 0;
        for (uint32_t w = 1; w <= 13; ++w)
            for (uint32_t h = 1; h <= 13; ++h) {
                const uint32_t rpB = ((w + 3) / 4) * 8 + 8;   // (a padded pitch)
                std::vector<uint8_t> blk((size_t)rpB * ((h + 3) / 4));
                for (uint8_t& v : blk) v = rnd();
                const uint64_t r = ref(blk.data(), rpB, w, h);
                if (Bc1SeeThrough(blk.data(), rpB, w, h) != r) ++bad;
                if (Bc1AnySeeThrough(blk.data(), rpB, w, h) != (r > 0)) ++bad;
                any += r > 0;
            }
        CHECK(bad == 0 && any > 50, "phase 115r: BC1 mask scan == per-texel reference on 169 random sizes (%d differ, %d with see-through)",
              bad, any);
        uint8_t first[16] = {};                                // a see-through texel in the FIRST block: found without the rest
        first[0] = 0x1F; first[3] = 0xF8; first[4] = 0x03;
        CHECK(Bc1AnySeeThrough(first, 16, 8, 4) && Bc1SeeThroughRows(first, 16, 8, 4, 0, 1, 1) == 1, "BC1: stops at the first texel");
        // phase 115r (P4): the area floor of the game's uploads -- the grille 32 x 16 (512) and the bus seal stay, 16 x 16 / 8 x 8 go
        CHECK(UploadShapeOk(32, 16, 6, 1) && UploadShapeOk(32, 128, 8, 1) && !UploadShapeOk(16, 16, 5, 1) && !UploadShapeOk(8, 8, 4, 1) &&
              ShapeOk(16, 16, 5, 1), "phase 115r: kMinArea 512 keeps the grille + the bus seal, drops 16x16 / 8x8 (the census path keeps them)");
        CHECK(LateShape(true, 16, 16, 5, 1, 1, true, 4096, kMinArea) == kLateShape && LateShape(true, 16, 16, 5, 1, 1, true, 4096, 0) == kLateOk,
              "phase 115r: the late read-back's floor (normal) vs none (census)");
    }
    // ---- phase 115r (P1): the alpha hash in pieces == the whole hash (the late read-back hashes mip 0 while copying it) ----------
    {
        uint32_t rng = 777u;
        const uint32_t w = 52, h = 36, mips = FullMips(w, h), rpB = Blocks(w) * 16 + 32;
        std::vector<uint8_t> m0((size_t)rpB * Blocks(h));
        for (uint8_t& v : m0) { rng = rng * 1664525u + 1013904223u; v = (uint8_t)(rng >> 24); }
        const uint64_t whole = HashAlpha(m0.data(), rpB, w, h, mips);
        uint64_t part = HashAlphaBegin(w, h, mips);
        for (uint32_t by = 0; by < Blocks(h);) {
            const uint32_t k = by % 3 + 1 < Blocks(h) - by ? by % 3 + 1 : Blocks(h) - by;
            part = HashAlphaRows(part, m0.data() + (size_t)by * rpB, rpB, w, k);
            by += k;
        }
        CHECK(part == whole, "phase 115r: HashAlphaBegin + HashAlphaRows in 1..3-row pieces == HashAlpha");
    }
    // ---- phase 115r (A7): no mip judged = not measured (-1), never "no loss" ---------------------------------------------------------
    {
        const uint8_t* none[kMaxMips] = {};
        uint32_t rps0[kMaxMips] = {};
        int j = 7;
        CHECK(RealMinRatio(none, rps0, 8, 8, 4, 0.01, &j) < 0.0 && j == 0, "phase 115r: cov0 x mip-1 texels < 4 -> -1 (was 1.0)");
        Plan ps; ps.reason = kCoverage; ps.cov0 = 0.01;
        CHECK(SparseDecide(ps, RealMinRatio(none, rps0, 8, 8, 4, 0.01), 8, 8) == 0 && ps.reason == kCoverage,
              "phase 115r: ... so SparseDecide keeps kCoverage (no kNoLoss without a measurement)");
    }
    // ---- phase 114, local only: the user's 2026-11-15 evening VR census -- small cut-outs and the BC1 pennant flags ----------------
    {
        const char* dir = "E:\\ETS2-DLAA-Injector\\captures\\census_2026-11-15_evening\\";
        struct T { const char* path; const char* what; int kind; };   // kind 0 = BC3 small (was the size rule), 1 = BC1 opaque
        const T ts[3] = { { "ets2\\shimmer_20261115_172644\\census\\7fcd07d6_c55c787d\\tex_t6.dds", "ETS2 bus window seals 32 x 128", 0 },
                          { "ats\\shimmer_20261115_173946\\census\\9d73eba3_9762bfe0\\tex_t6.dds", "ATS car grille 32 x 16", 0 },
                          { "ats\\shimmer_20261115_173946\\census\\c2ec5608_a9d39be8\\tex_t6.dds", "ATS pennant flags (BC1 256 x 256)", 1 } };
        for (const T& t : ts) {
            char path[512];
            snprintf(path, sizeof(path), "%s%s", dir, t.path);
            FILE* f = fopen(path, "rb");
            if (!f) { printf("NOTE: %s not found -- phase 114 real-texture check skipped\n", path); continue; }
            std::vector<uint8_t> b;
            fseek(f, 0, SEEK_END); b.resize((size_t)ftell(f)); fseek(f, 0, SEEK_SET);
            const bool rd = fread(b.data(), 1, b.size(), f) == b.size();
            fclose(f);
            uint32_t hh = 0, ww = 0, mm = 0, fmt = 0;
            if (!rd || b.size() < 148 || memcmp(b.data(), "DDS ", 4) || memcmp(b.data() + 84, "DX10", 4)) { CHECK(false, "%s: not a DX10 DDS", path); continue; }
            memcpy(&hh, b.data() + 12, 4); memcpy(&ww, b.data() + 16, 4); memcpy(&mm, b.data() + 28, 4); memcpy(&fmt, b.data() + 128, 4);
            const uint32_t bs = t.kind == 1 ? 8u : 16u;
            std::vector<const uint8_t*> mp(kMaxMips, nullptr);
            uint32_t rps[kMaxMips] = {};
            size_t off = 148;
            for (uint32_t L = 0; L < mm && (int)L < kMaxMips; ++L) {
                const uint32_t wl = MipDim(ww, (int)L), hl = MipDim(hh, (int)L), n = Blocks(wl) * bs * Blocks(hl);
                if (off + n > b.size()) break;
                mp[L] = b.data() + off; rps[L] = Blocks(wl) * bs; off += n;
            }
            if (t.kind == 1) {
                CHECK(fmt == 71 || fmt == 72, "%s: BC1 (%u)", t.what, fmt);
                uint64_t see = 0;
                for (uint32_t L = 0; L < mm && mp[L]; ++L) see += Bc1SeeThrough(mp[L], rps[L], MipDim(ww, (int)L), MipDim(hh, (int)L));
                printf("   phase 114 real %s: %ux%u, %u mips, see-through texels on every mip: %llu -> %s\n", t.what, ww, hh, mm,
                       (unsigned long long)see, see ? "a BC1 cut-out" : "opaque (nothing to cut out: census OK)");
                CHECK(see == 0, "%s: no see-through texel on any mip (the discard never fires)", t.what);
                continue;
            }
            CHECK(fmt == 77 || fmt == 78, "%s: BC3 (%u)", t.what, fmt);
            CHECK(ShapeOk(ww, hh, mm, 1), "%s: %ux%u / %u mips passes the phase-114 shape rule (was the size rule)", t.what, ww, hh, mm);
            Plan p;
            Analyse(mp[0], rps[0], ww, hh, mm, p);
            double gameMin = 1e9;                         // the game's REAL coverage per mip vs the cut coverage of mip 0 (mips >= 4 px)
            for (uint32_t L = 1; L < mm && mp[L]; ++L) {
                const uint32_t wl = MipDim(ww, (int)L), hl = MipDim(hh, (int)L);
                if (wl < 4 || hl < 4 || p.covCut <= 0) continue;
                gameMin = (std::min)(gameMin, Coverage(mp[L], rps[L], wl, hl) / p.covCut);
            }
            printf("   phase 114 real %s: %ux%u, %u mips, phase-77 rule -> %s (cov0 %.1f %%, soft %.1f %%, game rule worst %.2f; the game's REAL "
                   "mips keep %.2f x the cut coverage at worst)\n", t.what, ww, hh, mm, ReasonName(p.reason), 100.0 * p.cov0, 100.0 * p.soft,
                   p.minRatio, gameMin > 1e8 ? 1.0 : gameMin);
            CHECK(p.reason != kSize, "%s: now a phase-77 decision, not the size rule (%s)", t.what, ReasonName(p.reason));
            // the bus seal: 64 % coverage (over kCovMax: "coverage", census OK) and its real mips keep it exactly; the grille: patched
            if (strstr(t.what, "bus")) CHECK(p.reason == kCoverage && gameMin >= 0.95, "%s: blocky 64 %% alpha, the game's mips keep it -> census OK", t.what);
            if (strstr(t.what, "grille")) CHECK(p.reason == kPatched, "%s: a binary 38 %% ring pattern the game rule thins -> patched", t.what);
        }
    }

    printf("%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
