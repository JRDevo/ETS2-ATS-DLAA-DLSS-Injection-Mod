// Phase 59 unit test: EYE-TRACKED FOVEATION maths. (1) vrs.cpp FillMaskRuns (the per-row refill of the gaze mask) gives every
// byte and tile count of FillMask (the phase 34 per-tile fill) -- picture sizes, centres in / at / outside the picture, radii
// from tiny to 150 %, inner = outer -- and how much faster it is at an ATS VR eye size. (2) gaze_math.h GazeUv: view tangents ->
// the ring centre in uv, checked against an OpenXR fov projection (skewed / asymmetric fovs): the optical centre and |P00| /
// |P11| are derived from an MVP the way inject.cpp's EyeVerdict / MvpScale do (rotated model-view, so only the projection
// survives). (3) GazeToViewTangents signs (+x right, +y up) and canted views. (4) the Smoother and FakeGaze.
// No D3D device, no NVAPI (vrs.cpp is compiled without WITH_VRS), no game. Build: tests\build_vrs_gaze_test.bat. Exit code 0 =
// every check passed.
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../src/vrs.h"
#include "../src/gaze_math.h"

static int g_checks = 0, g_fails = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fails; if (g_fails <= 40) { printf("   FAIL: "); printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint32_t g_rng = 2026u;
static double Rnd(double lo, double hi) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return lo + (hi - lo) * (double)(g_rng >> 8) / 16777216.0;
}

// ---- (1) FillMaskRuns == FillMask ----
static void CompareFill(uint32_t w, uint32_t h, float cu, float cv, float ri, float ro, const char* tag) {
    const uint32_t tw = (w + 15) / 16, th = (h + 15) / 16;
    std::vector<uint8_t> a((size_t)tw * th, 0xAA), b((size_t)tw * th, 0x55);
    uint32_t ca[3] = {}, cb[3] = {};
    Vrs::FillMask(a.data(), tw, th, w, h, cu, cv, ri, ro, ca);
    Vrs::FillMaskRuns(b.data(), tw, th, w, h, cu, cv, ri, ro, cb);
    size_t diff = 0, first = 0;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) { if (!diff) first = i; ++diff; }
    CHECK(diff == 0, "%s: %ux%u centre (%.4f, %.4f) radii %.3f / %.3f -- %zu bytes differ (first at tile %zu,%zu: %d vs %d)", tag, w, h,
          (double)cu, (double)cv, (double)ri, (double)ro, diff, diff ? first % tw : 0, diff ? first / tw : 0, diff ? a[first] : 0,
          diff ? b[first] : 0);
    CHECK(ca[0] == cb[0] && ca[1] == cb[1] && ca[2] == cb[2], "%s: counts %u/%u/%u vs %u/%u/%u", tag, ca[0], ca[1], ca[2], cb[0], cb[1], cb[2]);
}

// ---- (2) the projection the game renders an eye with (OpenXR fov, right-handed, -z forward), as a row-major MVP ----
struct Fov { double l, r, u, d; };                           // radians (l, d negative)
static void Projection(const Fov& f, double P[16]) {
    const double tl = std::tan(f.l), tr = std::tan(f.r), tu = std::tan(f.u), td = std::tan(f.d);
    memset(P, 0, sizeof(double) * 16);
    P[0] = 2.0 / (tr - tl);  P[2] = (tr + tl) / (tr - tl);   // clip.x = P00 x + P02 z  (ndc = P00 x/-z - P02)
    P[5] = 2.0 / (tu - td);  P[6] = (tu + td) / (tu - td);
    P[10] = -1.0; P[11] = -0.05;                             // (depth: irrelevant here)
    P[14] = -1.0;                                            // clip.w = -z
}
// row-major 4x4 product C = A B
static void Mul(const double A[16], const double B[16], double C[16]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) { double s = 0; for (int k = 0; k < 4; ++k) s += A[i * 4 + k] * B[k * 4 + j]; C[i * 4 + j] = s; }
}
// a rigid model-view (rotation about a random axis + translation)
static void RandomRigid(double M[16]) {
    double ax[3] = { Rnd(-1, 1), Rnd(-1, 1), Rnd(-1, 1) };
    const double n = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
    for (double& a : ax) a /= n;
    const double ang = Rnd(-3.0, 3.0), c = std::cos(ang), s = std::sin(ang), t = 1 - c;
    memset(M, 0, sizeof(double) * 16);
    M[0] = t * ax[0] * ax[0] + c;         M[1] = t * ax[0] * ax[1] - s * ax[2]; M[2] = t * ax[0] * ax[2] + s * ax[1];
    M[4] = t * ax[0] * ax[1] + s * ax[2]; M[5] = t * ax[1] * ax[1] + c;         M[6] = t * ax[1] * ax[2] - s * ax[0];
    M[8] = t * ax[0] * ax[2] - s * ax[1]; M[9] = t * ax[1] * ax[2] + s * ax[0]; M[10] = t * ax[2] * ax[2] + c;
    M[3] = Rnd(-50, 50); M[7] = Rnd(-50, 50); M[11] = Rnd(-50, 50); M[15] = 1.0;
}
// inject.cpp EyeVerdict (p, q) + MvpScale (|P00|, |P11|) of one MVP, then VrEyeCentre's uv = (0.5 - 0.5 p, 0.5 + 0.5 q)
static void MeasureLikeTheMod(const double m[16], float* cu, float* cv, float* sx, float* sy) {
    const double* r0 = m; const double* r1 = m + 4; const double* r3 = m + 12;
    const double d33 = r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2];
    const double p = -(r0[0] * r3[0] + r0[1] * r3[1] + r0[2] * r3[2]) / d33;
    const double q = -(r1[0] * r3[0] + r1[1] * r3[1] + r1[2] * r3[2]) / d33;
    double a = 0, b = 0;
    for (int i = 0; i < 3; ++i) {
        const double t0 = r0[i] + p * r3[i], t1 = r1[i] + q * r3[i];   // row - (row.row3 / |row3|^2) row3
        a += t0 * t0; b += t1 * t1;
    }
    *sx = (float)std::sqrt(a / d33); *sy = (float)std::sqrt(b / d33);
    *cu = (float)(0.5 - 0.5 * p); *cv = (float)(0.5 + 0.5 * q);
}

static void QAxis(double ax, double ay, double az, double deg, double q[4]) {
    const double h = deg * 3.141592653589793 / 360.0, s = std::sin(h);
    q[0] = ax * s; q[1] = ay * s; q[2] = az * s; q[3] = std::cos(h);
}
static void QMul(const double a[4], const double b[4], double o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

int main() {
    // ---- (1) the fills ----
    printf("1. FillMaskRuns vs FillMask\n");
    CompareFill(4592, 6496, 0.5f, 0.5f, 0.55f, 0.80f, "ATS eye centred");
    CompareFill(4592, 6496, 0.43f, 0.52f, 0.55f, 0.80f, "ATS right eye optical");
    CompareFill(6120, 6496, 0.57f, 0.50f, 0.40f, 0.65f, "narrow");
    CompareFill(1000, 1000, 0.5f, 0.5f, 0.10f, 0.10f, "inner = outer, small");
    CompareFill(1000, 1000, 0.5f, 0.5f, 1.50f, 1.50f, "150 % (all inside)");
    CompareFill(1000, 1000, 0.0f, 0.0f, 0.55f, 0.80f, "corner centre");
    CompareFill(1000, 1000, 1.0f, 1.0f, 0.55f, 0.80f, "far corner centre");
    CompareFill(1000, 1000, -0.3f, 0.5f, 0.55f, 0.80f, "centre left of the picture");
    CompareFill(1000, 1000, 1.4f, 0.5f, 0.55f, 0.80f, "centre right of the picture");
    CompareFill(1000, 1000, 0.5f, -2.0f, 0.55f, 0.80f, "centre far above (nothing inside)");
    CompareFill(17, 33, 0.5f, 0.5f, 0.55f, 0.80f, "tiny picture");
    CompareFill(16, 16, 0.5f, 0.5f, 0.55f, 0.80f, "one tile");
    for (int k = 0; k < 4000; ++k) {
        const uint32_t w = 16 + (uint32_t)Rnd(0, 7000), h = 16 + (uint32_t)Rnd(0, 7000);
        const float ri = (float)Rnd(0.1, 1.5);
        const float ro = ri + (float)Rnd(0.0, 0.6);
        CompareFill(w, h, (float)Rnd(-0.2, 1.2), (float)Rnd(-0.2, 1.2), ri, ro > 1.5f ? 1.5f : ro, "random");
    }
    // exact-boundary hunt: centres on tile-centre grid points (ties between neighbours), radii hitting tile centres
    for (int k = 0; k < 2000; ++k) {
        const uint32_t w = 4592, h = 6496;
        const float cu = (float)((16.0 * (int)Rnd(0, 287) + 8.0) / w), cv = (float)((16.0 * (int)Rnd(0, 406) + (k & 1 ? 8.0 : 16.0)) / h);
        const float ri = (float)(2.0 * 16.0 * (int)Rnd(1, 80) / w);
        CompareFill(w, h, cu, cv, ri, ri + (float)Rnd(0, 0.3), "tie grid");
    }
    {   // timing at the ATS eye size (the in-game refill adds one UpdateSubresource of tw * th bytes)
        const uint32_t w = 4592, h = 6496, tw = (w + 15) / 16, th = (h + 15) / 16;
        std::vector<uint8_t> buf((size_t)tw * th);
        LARGE_INTEGER f, t0, t1, t2;
        QueryPerformanceFrequency(&f);
        const int n = 400;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < n; ++i) Vrs::FillMask(buf.data(), tw, th, w, h, 0.4f + 0.0005f * i, 0.5f, 0.55f, 0.8f, nullptr);
        QueryPerformanceCounter(&t1);
        for (int i = 0; i < n; ++i) Vrs::FillMaskRuns(buf.data(), tw, th, w, h, 0.4f + 0.0005f * i, 0.5f, 0.55f, 0.8f, nullptr);
        QueryPerformanceCounter(&t2);
        const double a = (double)(t1.QuadPart - t0.QuadPart) * 1e6 / f.QuadPart / n, b = (double)(t2.QuadPart - t1.QuadPart) * 1e6 / f.QuadPart / n;
        printf("   %ux%u px = %ux%u tiles (%u bytes): FillMask %.1f us, FillMaskRuns %.1f us per fill (%.0fx)\n", w, h, tw, th, tw * th, a,
               b, b > 0 ? a / b : 0.0);
        CHECK(b < a, "FillMaskRuns is not faster (%.1f vs %.1f us)", b, a);
    }

    // ---- (2) GazeUv against the projection ----
    printf("2. GazeUv (view tangents -> uv) against an OpenXR fov projection\n");
    const double kD = 3.141592653589793 / 180.0;
    const Fov fixed[] = { { -45 * kD, 45 * kD, 45 * kD, -45 * kD },        // symmetric
                          { -52 * kD, 42 * kD, 45 * kD, -50 * kD },        // a left eye (more outward = left)
                          { -42 * kD, 52 * kD, 45 * kD, -50 * kD },        // a right eye
                          { -30 * kD, 60 * kD, 35 * kD, -55 * kD } };      // strongly skewed
    double maxErr = 0.0;
    for (int k = 0; k < 600; ++k) {
        const Fov f = k < 4 ? fixed[k] : Fov{ Rnd(-60, -25) * kD, Rnd(25, 60) * kD, Rnd(25, 60) * kD, Rnd(-60, -25) * kD };
        double P[16], M[16], MVP[16];
        Projection(f, P);
        RandomRigid(M);
        Mul(P, M, MVP);
        float cu, cv, sx, sy;
        MeasureLikeTheMod(MVP, &cu, &cv, &sx, &sy);
        const double tl = std::tan(f.l), tr = std::tan(f.r), tu = std::tan(f.u), td = std::tan(f.d);
        if (k < 4) {
            CHECK(std::fabs(cu - (-tl / (tr - tl))) < 1e-5 && std::fabs(cv - (tu / (tu - td))) < 1e-5,
                  "fov %d: optical centre (%.5f, %.5f) vs the fov centre (%.5f, %.5f)", k, (double)cu, (double)cv, -tl / (tr - tl), tu / (tu - td));
            CHECK(std::fabs(sx - 2.0 / (tr - tl)) < 1e-5 && std::fabs(sy - 2.0 / (tu - td)) < 1e-5, "fov %d: scale", k);
        }
        for (int j = 0; j < 20; ++j) {
            const float tx = (float)Rnd(tl * 0.95, tr * 0.95), ty = (float)Rnd(td * 0.95, tu * 0.95);
            float u, v;
            GazeMath::GazeUv(cu, cv, sx, sy, tx, ty, &u, &v);
            const double ue = ((double)tx - tl) / (tr - tl), ve = (tu - (double)ty) / (tu - td);   // where the point lands (v down)
            const double e = std::fmax(std::fabs(u - ue), std::fabs(v - ve));
            if (e > maxErr) maxErr = e;
            CHECK(e < 1e-5, "fov (%.1f %.1f %.1f %.1f) tangents (%.3f, %.3f): uv (%.5f, %.5f) vs projected (%.5f, %.5f)", f.l / kD,
                  f.r / kD, f.u / kD, f.d / kD, (double)tx, (double)ty, (double)u, (double)v, ue, ve);
        }
    }
    printf("   600 fovs x 20 gazes: max error %.2e uv\n", maxErr);
    {   // clamping: a gaze outside the picture stays on its edge
        float u, v;
        GazeMath::GazeUv(0.5f, 0.5f, 1.0f, 1.0f, 3.0f, -3.0f, &u, &v);
        CHECK(u == 1.0f && v == 1.0f, "clamp right / down: (%.3f, %.3f)", (double)u, (double)v);
        GazeMath::GazeUv(0.5f, 0.5f, 1.0f, 1.0f, -3.0f, 3.0f, &u, &v);
        CHECK(u == 0.0f && v == 0.0f, "clamp left / up: (%.3f, %.3f)", (double)u, (double)v);
    }

    // ---- (3) gaze -> view tangents ----
    printf("3. GazeToViewTangents (OpenXR: +x right, +y up, -z forward; a +angle about +y turns LEFT, about +x UP)\n");
    {
        const double id[4] = { 0, 0, 0, 1 };
        double g[4];
        float tx, ty;
        QAxis(0, 1, 0, 10, g);                                   // look 10 deg left
        CHECK(GazeMath::GazeToViewTangents(g, id, &tx, &ty) && std::fabs(tx + std::tan(10 * kD)) < 1e-6 && std::fabs(ty) < 1e-6,
              "10 deg left -> tx %.4f ty %.4f", (double)tx, (double)ty);
        QAxis(1, 0, 0, 12, g);                                   // look 12 deg up
        CHECK(GazeMath::GazeToViewTangents(g, id, &tx, &ty) && std::fabs(ty - std::tan(12 * kD)) < 1e-6 && std::fabs(tx) < 1e-6,
              "12 deg up -> tx %.4f ty %.4f", (double)tx, (double)ty);
        float u, v;
        GazeMath::GazeUv(0.5f, 0.5f, 1.0f, 1.0f, tx, ty, &u, &v);
        CHECK(v < 0.5f, "looking up moves the centre UP in the picture (v %.3f)", (double)v);
        double vq[4];
        QAxis(0, 1, 0, -15, vq);                                 // a view canted 15 deg to the right, the gaze straight ahead
        CHECK(GazeMath::GazeToViewTangents(id, vq, &tx, &ty) && std::fabs(tx + std::tan(15 * kD)) < 1e-6,
              "canted right view, gaze ahead -> tx %.4f (expected %.4f: left of that view's axis)", (double)tx, -std::tan(15 * kD));
        QAxis(0, 1, 0, 20, vq);                                  // head turned 20 deg left, eyes also 20 deg left -> centred
        CHECK(GazeMath::GazeToViewTangents(vq, vq, &tx, &ty) && std::fabs(tx) < 1e-6 && std::fabs(ty) < 1e-6, "same orientation -> 0");
        double h[4], e[4], ge[4];                                // head pitched down 10, eyes 5 right within the head
        QAxis(1, 0, 0, -10, h);
        QAxis(0, 1, 0, -5, e);
        QMul(h, e, ge);
        CHECK(GazeMath::GazeToViewTangents(ge, h, &tx, &ty) && std::fabs(tx - std::tan(5 * kD)) < 1e-6 && std::fabs(ty) < 1e-6,
              "eyes 5 deg right in a pitched head -> tx %.4f ty %.4f", (double)tx, (double)ty);
        QAxis(0, 1, 0, 120, g);                                  // behind / sideways: rejected
        CHECK(!GazeMath::GazeToViewTangents(g, id, &tx, &ty), "a gaze 120 deg to the side is rejected");
    }

    // ---- (4) smoother + fake gaze ----
    printf("4. Smoother, FakeGaze\n");
    {
        GazeMath::Smoother s;
        s.Step(0.5f, 0.5f, 0.0, 20.0);
        CHECK(s.u == 0.5f && s.v == 0.5f, "first step takes the target");
        s.Step(0.52f, 0.5f, 0.014, 20.0);                        // small move (fixation): partial
        const double a = 1.0 - std::exp(-0.014 / 0.020);
        CHECK(std::fabs(s.u - (0.5 + 0.02 * a)) < 1e-6, "small move: %.5f (expected %.5f)", (double)s.u, 0.5 + 0.02 * a);
        GazeMath::Smoother s2;
        s2.Step(0.5f, 0.5f, 0.0, 20.0);
        s2.Step(0.7f, 0.5f, 0.014, 20.0);                        // saccade: tau / 4
        const double a2 = 1.0 - std::exp(-0.014 / 0.005);
        CHECK(std::fabs(s2.u - (0.5 + 0.2 * a2)) < 1e-6, "saccade: %.5f (expected %.5f)", (double)s2.u, 0.5 + 0.2 * a2);
        GazeMath::Smoother s3;
        s3.Step(0.5f, 0.5f, 0.0, 0.0);
        s3.Step(0.3f, 0.6f, 0.014, 0.0);
        CHECK(s3.u == 0.3f && s3.v == 0.6f, "smooth 0 = the raw gaze");
        GazeMath::Smoother s4;                                   // converges
        s4.Step(0.5f, 0.5f, 0.0, 100.0);
        for (int i = 0; i < 200; ++i) s4.Step(0.6f, 0.4f, 0.011, 100.0);
        CHECK(std::fabs(s4.u - 0.6f) < 1e-4 && std::fabs(s4.v - 0.4f) < 1e-4, "converges (%.5f, %.5f)", (double)s4.u, (double)s4.v);
        float tx, ty;
        GazeMath::FakeGaze(0.0, &tx, &ty);
        CHECK(std::fabs(tx - 0.25f) < 1e-6 && std::fabs(ty) < 1e-6, "fake gaze at t = 0");
        GazeMath::FakeGaze(1.5, &tx, &ty);
        CHECK(std::fabs(tx) < 1e-6 && std::fabs(ty - 0.25f) < 1e-6, "fake gaze at t = 1.5 s (a quarter turn: up)");
    }

    printf("%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
