// Phase 77 unit test of src/cutout_mips.h: coverage-preserving binary alpha mips for cut-out BC3 textures.
// BC3 alpha block decode / binary encode round trip, the detector (soft, coverage, no-loss, shape), a synthetic chain-link fence
// (1- and 2-texel diagonal wires, period 16, 128x2048 like the ATS fence) with exact coverage per mip, colour bytes untouched,
// a mip the game already keeps left alone, NPOT and padded row pitches. When the captured ATS fence alpha mips exist
// (E:\ETS2-DLAA-Injector\captures\ats_flat_fence_frame1634.fence_dump\m0.png .. m11.png, outside git) they are run too, else
// skipped with a note (CI). No D3D, no game. Build: tests\build_cutout_mips_test.bat. Exit code 0 = every check passed.
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
    CHECK(!ShapeOk(32, 2048, 12, 1), "min dim < 64");
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

    printf("%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
