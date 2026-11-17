// phase 103 unit test: own-vehicle shadow grid lock (src/shadow_lock.h, dlaa.ini shadow_lock).
//
// Part A (maths, no D3D), with the REAL constant rows of RenderDoc ATS frame 1890 (sun light PS cb0, ring 18784 @ 301824) and
// the forward VS 81dfa1a2 (cb0, frame 17840):
//   1. the sun block is found in a 2 MB-like ring at an odd row offset (kind sun, size row = c29); the forward block (size row
//      c28, x / y rows c13 / c14) is found as kind forward; both together in one buffer, in order;
//   2. not found: no block, another cascade size, a window that ends right after the size row;
//   3. false hits rejected: a lone (S, S, 1/S, 1/S) row (no shrinking cascade ladder before it), a NaN in the rows, a camera far
//      outside cascade 0, a far-size row without c31.y = 100;
//   4. Delta / Wrap: d in [-0.5, 0.5) and frac(t + d) == phase for t across many texels and phases;
//   5. a drifting camera over 240 frames: after the patch, the camera's texel phase is the same in every cascade every frame;
//   6. the y flip: the patched y-row moves the texel row by +dy (and x by +dx) -- checked through CamTexel of the patched rows;
//   7. the forward block's rows (R - 15 + 3k, R - 14 + 3k) are the ones patched;
//   8. a cascade size other than 3096 (2048 and 1548) is found and locked the same way;
//   9. Decide (pairing mode) and Stable; Spans (the scan windows: clip, align, merge, discovery span);
//  10. raster emulation: points rigid with the vehicle sample the SAME shadow texel every frame with the lock (and not without).
// Part B (WARP): the D3D11 rasteriser with a FRACTIONAL viewport TopLeft (dx, dy) covers exactly the texels the patched reader
//   rows predict -- x and the flipped y (the sign of the whole fix).
// Build: tests\build_shadow_lock_test.bat   Run: build\tests\shadow_lock_test.exe   (exit code 0 = all checks passed)
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
#include "shadow_lock.h"

using Microsoft::WRL::ComPtr;
using namespace ShadowLock;

static int g_fail = 0, g_pass = 0;
static void Check(bool ok, const char* what) {
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL: %s\n", what); }
}

// ATS frame 1890, EID 19329 (deferred sun light, PS 188), cb0 c0..c37 (RenderDoc, rdc_shadow/scan_1890.txt)
static const float kSun[38][4] = {
    { -0.359738f, -0.455794f, -0.81415f, 1 }, { 0.000345146f, 1, -0.000434689f, 20.245f }, { 1, 0.000886407f, 0, -1 },
    { 4.23527e-06f, 0.012271f, -5.33405e-06f, 29.9873f }, { 0, 0, 0, 4.29178f }, { 0, 0, 0, -1 },
    { 0.937865f, 1.30288f, 1.90036f, 1 }, { 7.50912f, 5.77569f, 3.68024f, -1 }, { 7.50912f, 5.77569f, 3.68024f, 1 },
    { 0.64968f, 0.120607f, 0, 0 }, { 0.000513039f, 2.20129e-07f, 0.000913835f, 0.12336f },
    { 0.00109822f, 0.000221414f, -0.000616609f, 1.43174f }, { 0.000354936f, 1.52292e-07f, 0.000632219f, 0.0800495f },
    { 0.000636179f, 0.000128261f, -0.00035719f, 0.910414f },
    { 0.0762325f, -4.09397e-05f, -0.0336609f, -0.404091f },        // c14 cascade 0 x
    { -0.0204121f, 0.0988984f, -0.046348f, -0.556028f },           // c15 cascade 0 y
    { -0.00754604f, -0.00956095f, -0.017078f, 0.639768f },         // c16 cascade 0 z
    { 0.0267482f, -1.43648e-05f, -0.0118108f, -0.425022f }, { -0.00737788f, 0.0357464f, -0.0167523f, -0.603386f },
    { -0.00440399f, -0.00557993f, -0.00996701f, 0.359907f },
    { 0.0101306f, -5.44049e-06f, -0.00447321f, -0.447552f }, { -0.00268581f, 0.0130129f, -0.00609842f, -0.6101f },
    { -0.00210416f, -0.002666f, -0.00476208f, 0.156235f },
    { 0.00500706f, -2.68898e-06f, -0.0022109f, -0.44246f }, { -0.00135479f, 0.00656405f, -0.00307619f, -0.615278f },
    { -0.00115579f, -0.00146441f, -0.00261577f, 0.0634077f },
    { 0.000529576f, -2.84402e-07f, -0.000233837f, -0.467536f }, { -0.000141981f, 0.000687909f, -0.000322383f, -0.644826f },
    { -0.000126809f, -0.00016067f, -0.000286992f, 0.0113018f },
    { 3096, 3096, 0.000322997f, 0.000322997f },                    // c29 cascade size
    { 6192, 6192, 0.000161499f, 0.000161499f },                    // c30 far map size
    { 0.348556f, 100, 0.01f, 1 }, { 0, 0.000821407f, 0, -1 }, { 0, 0, 0.5f, 0.766044f },
    { -1.78814e-07f, 0.54761f, -0.836734f, 4.29178f }, { -1, -1, -1, -1 }, { -1, -1, -1, -1 }, { -1, -1, -1, -1 },
};
// frame 17840, the forward VS 81dfa1a2 (samples the cascade array in its vertex shader), cb0 c0..c31 (rdc_shadow2/fwd_17840.txt)
static const float kFwd[32][4] = {
    { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 }, { 1.07111f, 0, 0, 0 }, { 0, 1.9042f, 0, 0 },
    { 0, 0, 5.00083e-05f, 0.20001f }, { 0, 0, -1, 0 }, { 1.07111f, 0, 0, 0 }, { 0, 1.9042f, 0, 0 }, { 0, 0, -1.0001f, -0.40002f },
    { 0, 0, -1, 0 }, { 1, 0, 1, 0 },
    { 0.0791651f, -0.000356242f, 0.0260233f, 0.31231f }, { 0.0163918f, 0.0986066f, -0.0485153f, -0.582187f },
    { 0.00584277f, -0.00978224f, -0.0179081f, 0.64737f },
    { 0.0277772f, -0.000124997f, 0.00913097f, 0.328682f }, { 0.00592474f, 0.0356409f, -0.0175357f, -0.631498f },
    { 0.00350106f, -0.00586164f, -0.0107308f, 0.357064f },
    { 0.010182f, -4.58189e-05f, 0.00334704f, 0.334963f }, { 0.00225575f, 0.0135697f, -0.00667642f, -0.667965f },
    { 0.0017002f, -0.00284655f, -0.00521111f, 0.139646f },
    { 0.00502635f, -2.26185e-05f, 0.00165227f, 0.330767f }, { 0.00114096f, 0.00686357f, -0.00337694f, -0.675277f },
    { 0.000950353f, -0.00159112f, -0.00291284f, 0.0287037f },
    { 0.000528648f, -2.37891e-06f, 0.000173778f, 0.347587f }, { 0.000120794f, 0.000726651f, -0.000357519f, -0.715048f },
    { 0.000100868f, -0.000168878f, -0.000309162f, 0.00714477f },
    { 3096, 3096, 0.000322997f, 0.000322997f },                    // c28 cascade size
    { 1, 0, 0.2f, 0.9f }, { 1, 0, 0, 0 }, { 1, 1, 1, 1 },
};

// a "ring": nRows float4 rows of plausible clutter (small values, no size rows), blocks copied in at row offsets
static std::vector<float> Ring(size_t nRows, unsigned seed = 1) {
    std::vector<float> v(nRows * 4);
    unsigned s = seed;
    for (float& f : v) { s = s * 1664525u + 1013904223u; f = (float)((s >> 8) % 20000) / 1000.0f - 10.0f; }
    return v;
}
template <size_t N>
static void Put(std::vector<float>& ring, size_t atRow, const float (&blk)[N][4]) {
    std::memcpy(&ring[atRow * 4], blk, sizeof(blk));
}
// a block scaled to cascade size S (the ladder rows stay; the size rows become S / 2S)
template <size_t N>
static void PutSized(std::vector<float>& ring, size_t atRow, const float (&blk)[N][4], size_t sizeRow, float S, bool sun) {
    Put(ring, atRow, blk);
    float* r = &ring[(atRow + sizeRow) * 4];
    r[0] = r[1] = S; r[2] = r[3] = 1.0f / S;
    if (sun) { r[4] = r[5] = 2 * S; r[6] = r[7] = 1.0f / (2 * S); }
}

static void PartA() {
    const float S = 3096.0f;
    // ---- 1. found: sun + forward in one ring, in order ----
    {
        std::vector<float> ring = Ring(8192);
        Put(ring, 1885, kSun);         // sun size row = 1885 + 29 = 1914
        Put(ring, 5003, kFwd);         // forward size row = 5003 + 28 = 5031
        Hit h1[1];
        bool cap = false;
        Check(Scan(ring.data(), 8192, S, h1, 1, &cap) == 1 && cap, "1: a cap of 1 block reports `more` (inject.cpp abstains then)");
        Hit h[8];
        bool more = true;
        const int n = Scan(ring.data(), 8192, S, h, 8, &more);
        Check(n == 2 && !more, "1: exactly two blocks found");
        Check(n >= 1 && h[0].row == 1914 && h[0].kind == kKindSun, "1: sun block at its size row c29, kind sun");
        Check(n >= 2 && h[1].row == 5031 && h[1].kind == kKindFwd, "1: forward block at its size row c28, kind forward");
        Check(XRow(1914, 0) == 1885 + 14 && YRow(1914, 0) == 1885 + 15 && XRow(1914, 3) == 1885 + 23, "1: sun rows c14 / c15 / c23");
        Check(XRow(5031, 0) == 5003 + 13 && YRow(5031, 0) == 5003 + 14 && YRow(5031, 3) == 5003 + 23, "7: forward rows c13 / c14 / c23");
        // the camera texel of frame 1890: cascade 0 x.w = -0.404091 -> tx = (0.5 * -0.404091 + 0.5) * 3096
        const Cam c = CamTexel(ring.data(), 1914, S);
        Check(std::fabs(c.tx[0] - (0.5 * -0.404091 + 0.5) * 3096.0) < 1e-3 && std::fabs(c.ty[0] - (0.5 + 0.5 * 0.556028) * 3096.0) < 1e-3,
              "1: camera texel of cascade 0 from c14.w / c15.w");
    }
    // ---- 2. not found ----
    {
        std::vector<float> ring = Ring(4096, 7);
        Hit h[4];
        Check(Scan(ring.data(), 4096, S, h, 4) == 0, "2: clutter alone: nothing");
        Put(ring, 100, kSun);
        Check(Scan(ring.data(), 4096, 2048.0f, h, 4) == 0, "2: another cascade size: nothing");
        Check(Scan(ring.data(), 100 + 30, S, h, 4) == 0, "2: a window ending right after the size row: nothing (the kind is unknown)");
        Check(Scan(ring.data(), 100 + 32, S, h, 4) == 1 && h[0].kind == kKindSun, "2: ... one row more: found");
    }
    // ---- 3. false hits rejected ----
    {
        std::vector<float> ring = Ring(4096, 3);
        float* r = &ring[500 * 4];
        r[0] = r[1] = S; r[2] = r[3] = 1.0f / S;     // a lone texel-size row: clutter before it, no ladder
        Hit h[4];
        Check(Scan(ring.data(), 4096, S, h, 4) == 0, "3: a lone (S, S, 1/S, 1/S) row is rejected");
        Put(ring, 1000, kSun);
        ring[(1000 + 20) * 4 + 1] = std::numeric_limits<float>::quiet_NaN();
        Check(Scan(ring.data(), 4096, S, h, 4) == 0, "3: a NaN in the cascade rows is rejected");
        Put(ring, 1000, kSun);
        ring[(1000 + 14) * 4 + 3] = 40.0f;           // camera absurdly far outside cascade 0
        Check(Scan(ring.data(), 4096, S, h, 4) == 0, "3: a camera far outside cascade 0 is rejected");
        Put(ring, 1000, kSun);
        ring[(1000 + 31) * 4 + 1] = 99.0f;           // c31.y != 100 with a far-size row
        Check(Scan(ring.data(), 4096, S, h, 4) == 0, "3: a far-size row without c31.y = 100 is rejected (unknown layout)");
        Put(ring, 1000, kSun);
        ring[(1000 + 17) * 4 + 0] = 0.5f;            // cascade 1 x-row larger than cascade 0's: no ladder
        Check(Scan(ring.data(), 4096, S, h, 4) == 0, "3: a cascade ladder that does not shrink is rejected");
        Put(ring, 1000, kSun);
        Check(Scan(ring.data(), 4096, S, h, 4) == 1, "3: ... the untouched block is found again");
    }
    // ---- 4. Delta / Wrap ----
    {
        bool okRange = true, okPhase = true;
        for (int i = 0; i < 20000; ++i) {
            const double t = -50.0 + i * 0.01237, ph = (i % 7) / 7.0;
            const double d = Delta(t, ph);
            if (!(d >= -0.5 && d < 0.5)) okRange = false;
            const double f = Frac(t + d);
            if (std::fabs(f - ph) > 1e-9 && std::fabs(std::fabs(f - ph) - 1.0) > 1e-9) okPhase = false;
        }
        Check(okRange, "4: Delta in [-0.5, 0.5)");
        Check(okPhase, "4: frac(t + Delta(t, phase)) == phase");
        Check(Wrap(0.5) == -0.5 && Wrap(-0.5) == -0.5 && Wrap(0.25) == 0.25 && Wrap(0.75) == -0.25, "4: Wrap edges");
    }
    // ---- 5. + 6. drifting camera: a constant phase after the patch, x and the flipped y ----
    {
        const double phase = 0.5;
        double maxErr = 0.0, maxD = 0.0;
        bool movedX = true, movedY = true;
        std::vector<float> ring = Ring(256, 9);
        for (int f = 0; f < 240; ++f) {
            Put(ring, 10, kSun);
            const size_t R = 10 + 29;
            // the truck drives: the camera's cascade NDC drifts by an irregular sub-texel amount per frame in x and y
            for (int k = 0; k < kCascades; ++k) {
                ring[4 * XRow(R, k) + 3] += (float)(f * 0.000137 / (1 << k) + 0.00003 * std::sin(f * 0.7));
                ring[4 * YRow(R, k) + 3] -= (float)(f * 0.000091 / (1 << k) + 0.00002 * std::cos(f * 0.3));
            }
            const Cam c0 = CamTexel(ring.data(), R, S);
            const Shift s = ShiftFor(c0, phase);
            Patch(ring.data(), R, S, s);
            const Cam c1 = CamTexel(ring.data(), R, S);
            for (int k = 0; k < kCascades; ++k) {
                maxErr = std::fmax(maxErr, std::fabs(Frac(c1.tx[k]) - phase));
                maxErr = std::fmax(maxErr, std::fabs(Frac(c1.ty[k]) - phase));
                maxD = std::fmax(maxD, std::fmax(std::fabs(s.dx[k]), std::fabs(s.dy[k])));
                // 6. the patched rows move the reader's texel by exactly +d (x) and +d (y, through the flip)
                if (std::fabs((c1.tx[k] - c0.tx[k]) - s.dx[k]) > 2e-3) movedX = false;
                if (std::fabs((c1.ty[k] - c0.ty[k]) - s.dy[k]) > 2e-3) movedY = false;
            }
        }
        std::printf("5: drifting camera, 240 frames: max |phase error| %.5f texel, max |d| %.3f texel\n", maxErr, maxD);
        Check(maxErr < 2e-3, "5: camera texel phase constant (0.5) in all cascades after the patch");
        Check(maxD <= 0.5, "5: |d| <= 0.5 texel");
        Check(movedX, "6: patched x-row.w moves the texel column by +dx");
        Check(movedY, "6: patched y-row.w (subtracted: y flip) moves the texel row by +dy");
    }
    // ---- 7. the forward block is patched on ITS rows (c13 / c14 ...), the sun's untouched rows stay ----
    {
        std::vector<float> ring = Ring(256, 5);
        Put(ring, 40, kFwd);
        const size_t R = 40 + 28;
        Shift s{};
        for (int k = 0; k < kCascades; ++k) { s.dx[k] = 0.25f; s.dy[k] = -0.125f; }
        std::vector<float> before = ring;
        Patch(ring.data(), R, S, s);
        int changed = 0;
        bool right = true;
        for (size_t i = 0; i < ring.size(); ++i) if (ring[i] != before[i]) {
            ++changed;
            const size_t row = i / 4, col = i % 4;
            if (col != 3 || row < 40 + 13 || row > 40 + 23 || ((row - (40 + 13)) % 3) == 2) right = false;
        }
        Check(changed == 8 && right, "7: forward block: exactly the 8 x / y .w of cascades 0..3 (c13, c14, c16, c17, ...) change");
        Check(std::fabs(ring[4 * (40 + 13) + 3] - (0.31231f + 2 * 0.25f / S)) < 1e-6f &&
              std::fabs(ring[4 * (40 + 14) + 3] - (-0.582187f - 2 * -0.125f / S)) < 1e-6f, "7: forward c13.w += 2dx/S, c14.w -= 2dy/S");
        std::vector<float> r2 = before;
        Patch(r2.data(), R, S, s, 0x1u);
        int ch2 = 0;
        for (size_t i = 0; i < r2.size(); ++i) if (r2[i] != before[i]) ++ch2;
        Check(ch2 == 2, "7: cascade mask 0x1 patches cascade 0 only");
    }
    // ---- 8. other cascade sizes ----
    for (float S2 : { 2048.0f, 1548.0f }) {
        std::vector<float> ring = Ring(4096, 11);
        PutSized(ring, 300, kSun, 29, S2, true);
        PutSized(ring, 2000, kFwd, 28, S2, false);
        Hit h[4];
        const int n = Scan(ring.data(), 4096, S2, h, 4);
        char what[96];
        std::snprintf(what, sizeof(what), "8: size %.0f: sun + forward found", S2);
        Check(n == 2 && h[0].kind == kKindSun && h[1].kind == kKindFwd, what);
        Check(Scan(ring.data(), 4096, 3096.0f, h, 4) == 0, "8: ... not as 3096");
        const Cam c0 = CamTexel(ring.data(), h[0].row, S2);
        const Shift s = ShiftFor(c0, 0.5);
        Patch(ring.data(), h[0].row, S2, s);
        const Cam c1 = CamTexel(ring.data(), h[0].row, S2);
        std::snprintf(what, sizeof(what), "8: size %.0f: phase 0.5 after the patch", S2);
        Check(std::fabs(Frac(c1.tx[1]) - 0.5) < 2e-3 && std::fabs(Frac(c1.ty[2]) - 0.5) < 2e-3, what);
    }
    // ---- 9. Decide, Stable, Spans ----
    {
        Check(Decide(1, 1) == kModeSingle && Decide(2, 1) == kModeShared && Decide(2, 2) == kModePerEye, "9: Decide single / shared / per eye");
        Check(Decide(0, 1) == kModeOff && Decide(1, 0) == kModeOff && Decide(1, 2) == kModeOff && Decide(2, 3) == kModeOff &&
              Decide(-1, -1) == kModeOff, "9: Decide off for the ambiguous cases");
        Stable st;
        for (int i = 0; i < 7; ++i) st.Add(1);
        Check(!st.Ok(8) && st.Get(8) == -1, "9: Stable not yet after 7");
        st.Add(1);
        Check(st.Ok(8) && st.Get(8) == 1, "9: Stable after 8");
        st.Add(2);
        Check(!st.Ok(8) && st.value == 2 && st.run == 1, "9: Stable restarts on a change");
        Span sp[8];
        const uint32_t offs[3] = { 301824u + 29u * 16u, 310000u, 2097000u };
        const int n = Spans(offs, 3, 2097152u, 32768u, 65536u, 65536u, sp, 8);
        bool aligned = true, sorted = true, inside = true;
        for (int i = 0; i < n; ++i) {
            if ((sp[i].lo & 15u) || (sp[i].len & 15u)) aligned = false;
            if (i && sp[i].lo <= sp[i - 1].lo + sp[i - 1].len) sorted = false;
            if ((uint64_t)sp[i].lo + sp[i].len > 2097152u) inside = false;
        }
        Check(n == 3, "9: Spans: discovery + 2 offsets merged + the end one = 3 spans");
        Check(aligned && sorted && inside, "9: Spans 16-byte aligned, sorted, disjoint, inside the ring");
        Check(n == 3 && sp[1].lo <= offs[0] - 32768u && sp[1].lo + sp[1].len >= offs[1] + 32768u, "9: Spans: the two near offsets in one span");
        Check(n == 3 && sp[2].lo + sp[2].len == 2097152u, "9: Spans: the last one clipped at the ring's end");
    }
    // ---- 11. the byte offsets inject.cpp writes (XWByte / YWByte) = the rows Patch changes ----
    {
        std::vector<float> ring = Ring(512, 13);
        Put(ring, 77, kSun);
        const uint32_t R = 77 + 29, sizeByte = R * 16u;
        Shift s{};
        for (int k = 0; k < kCascades; ++k) { s.dx[k] = 0.3f - 0.1f * k; s.dy[k] = -0.2f + 0.05f * k; }
        std::vector<float> a = ring, bb = ring;
        Patch(a.data(), R, 3096.0f, s, 0xBu);
        unsigned char* base = reinterpret_cast<unsigned char*>(bb.data());
        for (int k = 0; k < kCascades; ++k) {
            if (!(0xBu & (1u << k))) continue;
            float xw, yw;
            std::memcpy(&xw, base + XWByte(sizeByte, k), 4);
            std::memcpy(&yw, base + YWByte(sizeByte, k), 4);
            xw = PatchX(xw, s.dx[k], 3096.0f); yw = PatchY(yw, s.dy[k], 3096.0f);
            std::memcpy(base + XWByte(sizeByte, k), &xw, 4);
            std::memcpy(base + YWByte(sizeByte, k), &yw, 4);
        }
        Check(std::memcmp(a.data(), bb.data(), a.size() * 4) == 0, "11: XWByte / YWByte writes == Patch (mask 0xB)");
        Check(XWByte(sizeByte, 0) == (77u + 14u) * 16u + 12u && YWByte(sizeByte, 3) == (77u + 24u) * 16u + 12u,
              "11: XWByte c14.w / YWByte c24.w of the sun block");
    }
    // ---- 12. stale copies (round 2): a byte-identical block at the same offset = stale; any change = a new block ----
    {
        std::vector<float> ring = Ring(512, 21);
        Put(ring, 100, kSun);
        const uint64_t h0 = BlockHash(ring.data(), 129);
        std::vector<float> same = ring;
        Check(BlockHash(same.data(), 129) == h0, "12: the same bytes -> the same hash (rejected as stale)");
        same[4 * XRow(129, 0) + 3] += 1e-6f;
        Check(BlockHash(same.data(), 129) != h0, "12: a moved camera -> another hash (a fresh block)");
        std::vector<float> other = ring;
        other[4 * 128 + 1] += 0.5f;              // a z-row value of the far map changes (rows R-15 .. R+2 are all hashed)
        Check(BlockHash(other.data(), 129) != h0, "12: any row of the block counts");
    }
    // ---- 13. eye-consistent reference (round 2): two eyes + stale copies, candidates in shuffled order; Predict + Choose take eye
    //          0's block EVERY frame, the lock error of eye 0 stays ~0; "the first block found" flips eyes and wobbles ----
    {
        Truth t0, t1;
        double eyeSep = 0.0;
        int chosenEye0 = 0, frames = 0, noPred = 0;
        double maxErr = 0.0, maxErrNaive = 0.0;
        Cam staleQ[6];
        int staleN = 0;
        unsigned rnd = 7;
        double now = 10.0;
        for (uint64_t seq = 1; seq <= 300; ++seq) {
            now += (1.0 / 90.0) * (1.0 + 0.15 * std::sin(seq * 1.7));   // uneven frame times: the prediction runs on time
            // the truck drives: eye 0 drifts ~0.37 texel / frame with small irregularity; eye 1 is 6.3 texels to the side
            Cam e0{}, e1{};
            for (int k = 0; k < kCascades; ++k) {
                const double sc = 1.0 / (1 << k);
                e0.tx[k] = 900.0 + now * 33.0 * sc + 0.03 * std::sin(seq * 0.9); e0.ty[k] = 700.0 - now * 19.0 * sc;
                e1.tx[k] = e0.tx[k] + 6.3 * sc; e1.ty[k] = e0.ty[k] + 0.8 * sc;
            }
            // candidates this frame: both eyes + up to 3 stale copies of older frames, in a random order
            Cam cand[8];
            int n = 0;
            cand[n++] = e1; cand[n++] = e0;
            for (int i = 0; i < staleN && n < 5; ++i) cand[n++] = staleQ[i];
            for (int i = n - 1; i > 0; --i) { rnd = rnd * 1103515245u + 12345u; const int j = (int)((rnd >> 8) % (unsigned)(i + 1)); const Cam c = cand[i]; cand[i] = cand[j]; cand[j] = c; }
            Cam pred{};
            double speed = 0.0;
            if (Predict(t0, t1, now, &pred, &speed)) {
                const int c = Choose(cand, n, pred, Tolerance(speed * (now - t1.t), eyeSep));
                const Cam ref = c >= 0 ? cand[c] : pred;
                if (c >= 0 && Dist0(cand[c], e0) < 1e-9) ++chosenEye0;
                const Shift d = ShiftFor(ref, 0.5);
                if (seq > 3) {
                    maxErr = std::fmax(maxErr, std::fabs(LockError(e0.tx[0], d.dx[0], 0.5)));
                    maxErr = std::fmax(maxErr, std::fabs(LockError(e0.ty[1], d.dy[1], 0.5)));
                    const Shift dn = ShiftFor(cand[0], 0.5);   // naive: the first candidate in the list
                    maxErrNaive = std::fmax(maxErrNaive, std::fabs(LockError(e0.tx[0], dn.dx[0], 0.5)));
                    ++frames;
                }
            } else ++noPred;
            // the frame's first sun light draw (eye 0) tells the truth; eye 1 the separation
            t0 = t1; t1.ok = true; t1.c = e0; t1.t = now;
            eyeSep = Dist0(e0, e1);
            // the ring memory keeps this frame's blocks: they show up as stale copies 3 frames later
            if (staleN < 6) staleQ[staleN++] = e0; else { for (int i = 0; i < 5; ++i) staleQ[i] = staleQ[i + 1]; staleQ[5] = e0; }
        }
        std::printf("13: 300 frames, 2 eyes + stale copies: eye-0 block chosen %d of %d frames, eye-0 lock error max %.4f texel "
                    "(naive first block: %.3f), frames without a prediction %d\n", chosenEye0, frames + 3 - noPred, maxErr, maxErrNaive, noPred);
        Check(noPred == 1, "13: only the very first frame has no prediction");
        Check(chosenEye0 >= 297, "13: the eye-0 block is the reference every frame (never eye 1, never a stale copy)");
        Check(maxErr < 1e-6, "13: eye 0 sits exactly on the phase every frame");
        Check(maxErrNaive > 0.2, "13: the first-found block would wobble (eye / stale changes)");
        // tolerance: never as wide as half the eye separation
        Check(Tolerance(10.0, 6.3) <= 0.45 * 6.3 + 1e-12 && Tolerance(0.0, 0.0) == 1.0, "13: Tolerance capped under half the eye separation");
        // a prediction needs a recent truth
        Truth a, b; b.ok = true; b.t = 10.0;
        Cam pp{}; double sp = 0.0;
        Check(!Predict(a, b, 10.2, &pp, &sp) && Predict(a, b, 10.05, &pp, &sp) && !Predict(a, Truth{}, 10.01, &pp, &sp) &&
              !Predict(a, b, 9.9, &pp, &sp), "13: Predict only from a truth <= 0.1 s old (and not from the future)");
        // a jump (teleport / camera switch): the truth far from the prediction is one
        Cam j1 = b.c; j1.tx[0] += 20.0;
        Cam j2 = b.c; j2.tx[0] += 3.0;
        Check(IsJump(b.c, j1) && !IsJump(b.c, j2), "13: IsJump at > 8 cascade-0 texels only");
        // extrapolation by time: half the interval -> half the step
        Truth u0, u1; u0.ok = u1.ok = true; u0.t = 1.0; u1.t = 1.02;
        for (int k = 0; k < kCascades; ++k) { u0.c.tx[k] = 100.0; u1.c.tx[k] = 102.0; }
        Check(Predict(u0, u1, 1.03, &pp, &sp) && std::fabs(pp.tx[0] - 103.0) < 1e-9 && std::fabs(sp - 100.0) < 1e-6,
              "13: extrapolation by time (2 texels / 20 ms -> +1 texel after 10 ms, 100 texels / s)");
    }
    // ---- 15. the VR block of the 2026-11-14 dump (c23.w = 2.039: the camera outside cascade 3) is a sun block ----
    {
        std::vector<float> ring = Ring(256, 41);
        Put(ring, 60, kSun);
        float* r = &ring[4 * 60];
        r[4 * 23 + 3] = 2.03905f; r[4 * 20 + 3] = 1.37483f;      // the dump's cascade 2 / 3 x-row .w
        r[4 * 29 + 0] = r[4 * 29 + 1] = 2048.0f; r[4 * 29 + 2] = r[4 * 29 + 3] = 0.000488281f;
        r[4 * 30 + 0] = r[4 * 30 + 1] = 4096.0f; r[4 * 30 + 2] = r[4 * 30 + 3] = 0.000244141f;
        Hit h[4];
        Check(Scan(ring.data(), 256, 2048.0f, h, 4) == 1 && h[0].kind == kKindSun, "15: VR block with the camera outside cascade 3 is found");
    }
    // ---- 14. a block a sun light READS: c31.y need not be 100 (night / another light setting) -- the scans stay strict ----
    {
        std::vector<float> ring = Ring(256, 31);
        Put(ring, 50, kSun);
        ring[4 * (50 + 31) + 1] = 37.0f;
        Check(Classify(ring.data(), 256, 50 + 29, 3096.0f) == kKindNone, "14: strict (scan): c31.y = 37 is rejected");
        Check(Classify(ring.data(), 256, 50 + 29, 3096.0f, false) == kKindSun, "14: at the light draw: still the sun block");
    }
    // ---- 10. raster emulation: the vehicle's shadow texels are the same every frame with the lock ----
    {
        // D3D11: a texel (i, j) is covered when its centre (i + 0.5, j + 0.5) is inside, viewport X = TopLeftX + (x + 1) / 2 * S
        // (i.e. texel position tx + dx). A shadow point P rigid with the vehicle (camera + offset in texels) is "in shadow" when
        // the reader's patched texel floor(tx_P + dx) lies in the slice coverage of the vehicle's caster edge, also rigid.
        const double Sd = 3096.0;
        int changesLocked = 0, changesFree = 0;
        int prevL[16], prevF[16];
        for (int f = 0; f < 300; ++f) {
            const double camTx = 1000.0 + f * 0.3721;                 // the truck drives: ~0.37 texel per frame
            const double edge = camTx + 12.34;                         // the caster's edge (texels, rigid with the camera)
            const double dl = Delta(camTx, 0.5);
            for (int p = 0; p < 16; ++p) {
                const double P = camTx + 12.34 - 2.0 + p * 0.25;          // shadow sample points around the edge, rigid too
                // locked: rasterised at edge + dl (first covered column ceil(edge + dl - 0.5)); read at floor(P + dl)
                const int colL = (int)std::floor(P + dl), firstL = (int)std::ceil(edge + dl - 0.5);
                const int colF = (int)std::floor(P), firstF = (int)std::ceil(edge - 0.5);
                const int inL = colL >= firstL ? 1 : 0, inF = colF >= firstF ? 1 : 0;
                if (f && inL != prevL[p]) ++changesLocked;
                if (f && inF != prevF[p]) ++changesFree;
                prevL[p] = inL; prevF[p] = inF;
            }
            (void)Sd;
        }
        std::printf("10: 300 frames, 16 vehicle-rigid shadow samples: %d flips with the lock, %d without\n", changesLocked, changesFree);
        Check(changesLocked == 0, "10: with the lock no vehicle-rigid shadow sample ever flips");
        Check(changesFree > 50, "10: without it they flip (the bug)");
    }
}

// ---- Part B: WARP, a fractional viewport TopLeft rasterises where the patched reader rows look ----
static const char* kVs =
    "float4 main(float2 p : POSITION) : SV_Position { return float4(p, 0.5, 1); }";
static const char* kPs =
    "float4 main() : SV_Target { return 1; }";

static void PartB() {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        Check(false, "B: WARP device"); return;
    }
    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kVs, std::strlen(kVs), "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(kPs, std::strlen(kPs), "ps", nullptr, nullptr, "main", "ps_5_0", 0, 0, &psb, &err))) {
        Check(false, "B: shader compile"); return;
    }
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    D3D11_INPUT_ELEMENT_DESC ie{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
    ComPtr<ID3D11InputLayout> il;
    dev->CreateInputLayout(&ie, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il);
    const UINT S = 64;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = S; td.MipLevels = td.ArraySize = 1; td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> rt, st;
    dev->CreateTexture2D(&td, nullptr, &rt);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dev->CreateTexture2D(&td, nullptr, &st);
    ComPtr<ID3D11RenderTargetView> rtv;
    dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> rs;
    dev->CreateRasterizerState(&rd, &rs);

    // the caster: the quadrant x >= xe, y >= ye in NDC (y up), i.e. texel columns right of the edge, rows ABOVE it
    const float xe = 0.1234f, ye = -0.2345f;
    const float verts[6][2] = { { xe, ye }, { xe, 3 }, { 3, ye }, { 3, ye }, { xe, 3 }, { 3, 3 } };
    D3D11_BUFFER_DESC bd{ sizeof(verts), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA sd{ verts, 0, 0 };
    ComPtr<ID3D11Buffer> vb;
    dev->CreateBuffer(&bd, &sd, &vb);
    // the reader's rows: cascade NDC = identity on (x, y) + a .w the patch changes; the edge point in texels
    bool allOk = true;
    int cases = 0;
    for (int ix = 0; ix < 5; ++ix)
        for (int iy = 0; iy < 5; ++iy) {
            const float dx = -0.5f + 0.23f * ix, dy = -0.5f + 0.23f * iy;   // in [-0.5, 0.5)
            const float clr[4] = { 0, 0, 0, 0 };
            ctx->ClearRenderTargetView(rtv.Get(), clr);
            ID3D11RenderTargetView* r0 = rtv.Get();
            ctx->OMSetRenderTargets(1, &r0, nullptr);
            D3D11_VIEWPORT vp{ dx, dy, (float)S, (float)S, 0, 1 };
            ctx->RSSetViewports(1, &vp);
            ctx->RSSetState(rs.Get());
            const UINT stride = 8, off = 0;
            ID3D11Buffer* b = vb.Get();
            ctx->IASetVertexBuffers(0, 1, &b, &stride, &off);
            ctx->IASetInputLayout(il.Get());
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vs.Get(), nullptr, 0);
            ctx->PSSetShader(ps.Get(), nullptr, 0);
            ctx->Draw(6, 0);
            ctx->CopyResource(st.Get(), rt.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) { allOk = false; continue; }
            // the reader: a point at NDC (x, y) samples texel floor of uv * S with uv from its PATCHED rows
            //   x' = x + 2 dx / S, y' = y - 2 dy / S, uv = (0.5 x' + 0.5, 0.5 - 0.5 y')
            // the caster covers the point iff the point is in the quadrant (x >= xe, y >= ye): the texel it reads must be lit
            // (away from the edge by > 1 texel: the rasteriser's coverage of the edge texel itself is a half-texel matter)
            for (int py = 0; py < 64; ++py)
                for (int px = 0; px < 64; ++px) {
                    const float x = -1.0f + (px + 0.37f) * (2.0f / 64.0f), y = 1.0f - (py + 0.61f) * (2.0f / 64.0f);
                    const float distX = (x - xe) * 0.5f * S, distY = (y - ye) * 0.5f * S;   // texels from the edge
                    if (std::fabs(distX) < 1.0f || std::fabs(distY) < 1.0f) continue;
                    const float xp = PatchX(x, dx, (float)S), yp = PatchY(y, dy, (float)S);
                    const int tx = (int)std::floor((0.5f * xp + 0.5f) * S), ty = (int)std::floor((0.5f - 0.5f * yp) * S);
                    // the slice border texels are skipped: a shifted viewport is cut at floor(TopLeft + size), so a negative
                    // shift leaves the last column / row unrasterised (the accepted <= 1-texel edge: the cascade blend band)
                    if (tx < 1 || ty < 1 || tx >= (int)S - 1 || ty >= (int)S - 1) continue;
                    const bool lit = ((const uint8_t*)m.pData)[ty * m.RowPitch + tx] > 127;
                    const bool want = x >= xe && y >= ye;
                    if (lit != want) { if (allOk) std::printf("  sample mismatch d=(%.2f,%.2f) px=%d py=%d tx=%d ty=%d lit=%d want=%d\n", dx, dy, px, py, tx, ty, (int)lit, (int)want); allOk = false; }
                    ++cases;
                }
            // and the edge itself: the first covered column / the last covered row sit at the shifted edge
            const double edgeX = (0.5 * xe + 0.5) * S + dx, edgeY = (0.5 - 0.5 * ye) * S + dy;
            const int firstCol = (int)std::ceil(edgeX - 0.5), lastRow = (int)std::ceil(edgeY - 0.5) - 1;
            const uint8_t* row = (const uint8_t*)m.pData + 2 * m.RowPitch;   // a row well inside the quadrant
            if (!(row[firstCol] > 127 && (firstCol == 0 || row[firstCol - 1] < 128))) { std::printf("  col edge d=(%.2f,%.2f) first=%d v=%d prev=%d\n", dx, dy, firstCol, row[firstCol], firstCol ? row[firstCol-1] : -1); allOk = false; }
            const uint8_t* colTop = (const uint8_t*)m.pData + (size_t)lastRow * m.RowPitch;
            const uint8_t* colBelow = (const uint8_t*)m.pData + (size_t)(lastRow + 1) * m.RowPitch;
            if (!(colTop[S - 2] > 127 && colBelow[S - 2] < 128)) { std::printf("  row edge d=(%.2f,%.2f) last=%d top=%d below=%d\n", dx, dy, lastRow, colTop[S-2], colBelow[S-2]); allOk = false; }
            ctx->Unmap(st.Get(), 0);
        }
    std::printf("B: WARP, 25 fractional viewport shifts, %d reader samples compared\n", cases);
    Check(cases > 50000, "B: enough reader samples");
    Check(allOk, "B: the shifted raster and the patched reader rows agree (x and the flipped y), edges at ceil(edge + d - 0.5)");
}

// ---- Part C: the forward-reader vertex shader classifier (VsReadsCascades) on fxc-compiled look-alikes ----
static bool VsFlag(const char* src, const char* target) {
    ComPtr<ID3DBlob> b, err;
    if (FAILED(D3DCompile(src, std::strlen(src), "vs", nullptr, nullptr, "main", target, 0, 0, &b, &err))) {
        std::printf("compile failed: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    return VsReadsCascades(b->GetBufferPointer(), b->GetBufferSize());
}
static void PartC() {
    const char* reader =
        "cbuffer C : register(b0) { float4 c[30]; };"
        "Texture2DArray<float> t0 : register(t0); SamplerState s0 : register(s0);"
        "float4 main(float4 p : POSITION) : SV_Position { float4 r = mul(c[0], p);"
        "  float3 uv = float3(dot(c[13], p), dot(c[14], p), 0); float s = t0.SampleLevel(s0, uv, 0);"
        "  return r * s * c[28].x * c[29].y; }";
    const char* tex2d =
        "cbuffer C : register(b0) { float4 c[30]; };"
        "Texture2D<float> t0 : register(t0); SamplerState s0 : register(s0);"
        "float4 main(float4 p : POSITION) : SV_Position { return mul(c[0], p) * t0.SampleLevel(s0, c[13].xy, 0) * c[29].x; }";
    const char* smallCb =
        "cbuffer C : register(b0) { float4 c[8]; };"
        "Texture2DArray<float> t0 : register(t0); SamplerState s0 : register(s0);"
        "float4 main(float4 p : POSITION) : SV_Position { return mul(c[0], p) * t0.SampleLevel(s0, c[7].xyz, 0); }";
    const char* ps =
        "cbuffer C : register(b0) { float4 c[30]; };"
        "Texture2DArray<float> t0 : register(t0); SamplerState s0 : register(s0);"
        "float4 main(float4 p : SV_Position) : SV_Target { return t0.SampleLevel(s0, c[13].xyz, 0) * c[29]; }";
    Check(VsFlag(reader, "vs_5_0"), "C: a VS reading a Texture2DArray with cb0[30] is a forward-reader candidate");
    Check(!VsFlag(tex2d, "vs_5_0"), "C: a Texture2D VS is not");
    Check(!VsFlag(smallCb, "vs_5_0"), "C: a cb0 under 29 registers is not");
    Check(!VsFlag(ps, "ps_5_0"), "C: a pixel shader is not");
    unsigned char fake[64] = {};
    const uint32_t hdr[5] = { 0x43425844u, kFwdVsHash[0], kFwdVsHash[1], kFwdVsHash[2], kFwdVsHash[3] };
    std::memcpy(fake, hdr, sizeof(hdr));
    Check(VsReadsCascades(fake, sizeof(fake)), "C: the RenderDoc checksum 81dfa1a2-... alone is enough");
    Check(!VsReadsCascades(nullptr, 0) && !VsReadsCascades(fake, 8), "C: garbage is rejected");
}

int main() {
    PartA();
    PartB();
    PartC();
    std::printf("shadow_lock_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
