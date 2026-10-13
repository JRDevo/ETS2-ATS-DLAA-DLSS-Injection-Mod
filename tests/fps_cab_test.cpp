// phase 47: fps_cab_test -- the VR fps box ATTACHED TO THE VEHICLE (src/fps_cab.h / fps_cab.cpp, the menu.cpp VSCab draw).
//
// Part A (pure maths): the eye frame of an MVP, play -> model map, the box built in the play space and carried into a part's
//   model space, the MVP-block rules (car layout rows 5..8, camera-relative MVPs), the eye-order check, the moving-part test.
// Part B (WARP D3D11, the REAL DrawIdRecord + FpsCab runtime + menu.cpp): a VR cab scene, both eyes, 300 frames. Every frame the
//   cab SWAYS against the play space (pitch / roll / bounce) and the head moves; the parts' MVPs go through a WRITE_DISCARD
//   cbuffer ring, DrawIdRecord::Record + Flush mirror them, FpsCab::OnBlit picks the reference, TuningMenu::DrawWidgetMvp draws
//   the box with the MVP read on the GPU. The picture is compared with a reference draw of the box at the TRUE corners of the
//   SAME frame (the cab-fixed box projected on the CPU, DrawWidgetCorners): zero lag = the same pixels; the previous frame's
//   truth is measured too (what a 1-frame lag would be off by). Events: the biggest valid part is the STEERING WHEEL, it starts
//   turning at frame 60 (must be found moving and dropped, the box back on the cab); an exterior camera (no cabin layer, the box
//   falls back, the lock is kept); a buffer-pool rotation (new pointers, same shapes: loose keys, no re-lock); a truck change
//   (new parts: a new lock after kRelockBlits). A camera-relative overlay with the biggest IndexCount and a world draw must never
//   carry the box.
// Part D (phase 51d): the box SQUARE TO THE CAB -- a cab yawed / rolled against the play space (the ATS 15:02 truck 47a8), two cabs
//   with different play alignments (the same settings = the same cab spot), an older play-space pose levelled once, the moving
//   wheel dropped from a pose stored at a play-space lock, and the Reset row never reaching a vehicle's profile (FpsProfile::Session).
// Build: tests\build_fps_cab_test.bat -> build\tests\fps_cab_test.exe (exit 0 = all checks passed).
#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "fps_cab.h"
#include "fps_profile.h"
#include "draw_ids.h"
#include "menu.h"
#include "shader_cache.h"

using Microsoft::WRL::ComPtr;
using namespace FpsCab;

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- maths helpers (column vectors) -----------------------------------------------------------------------------------------
static M4 T(double x, double y, double z) { M4 m = Identity(); m.m[0][3] = x; m.m[1][3] = y; m.m[2][3] = z; return m; }
static M4 S(double s) { M4 m = Identity(); m.m[0][0] = m.m[1][1] = m.m[2][2] = s; return m; }
static M4 Rq(const double q[4]) {
    M4 m = Identity();
    for (int c = 0; c < 3; ++c) {
        double v[3] = { 0, 0, 0 }, o[3];
        v[c] = 1.0;
        FpsPose::QRotate(q, v, o);
        for (int r = 0; r < 3; ++r) m.m[r][c] = o[r];
    }
    return m;
}
static void Qax(int axis, double rad, double q[4]) { FpsPose::QAxis(axis, rad, q); }
static M4 Rax(int axis, double rad) { double q[4]; Qax(axis, rad, q); return Rq(q); }
static M4 PoseM(const Pose& p) { return Mul(T(p.p[0], p.p[1], p.p[2]), Rq(p.q)); }
static M4 InvM(const M4& a) { M4 o; Inv(a, &o); return o; }
// OpenXR view space (x right, y up, z back), fov tangents; D3D clip (w = view depth), z in [0, w] (near 0.05, far 50)
static M4 Proj(double tl, double tr, double tu, double td) {
    M4 p{};
    const double n = 0.05, f = 50.0;
    p.m[0][0] = 2.0 / (tr - tl); p.m[0][2] = (tr + tl) / (tr - tl);
    p.m[1][1] = 2.0 / (tu - td); p.m[1][2] = (tu + td) / (tu - td);
    p.m[2][2] = f / (n - f);     p.m[2][3] = n * f / (n - f);
    p.m[3][2] = -1.0;
    return p;
}
static void ToClip(const M4& M, const double p[3], double c[4]) { const double h[4] = { p[0], p[1], p[2], 1.0 }; Apply(M, h, c); }

// ---- the scene ----------------------------------------------------------------------------------------------------------------
constexpr double kHz = 72.0, kIpd = 0.064;
constexpr int kW = 512, kH = 512;
// eye 0 = the RIGHT eye (the DLAA eye order of the mod), eye 1 = the left eye
static const double kTan[2][4] = { { -0.9, 1.1, 1.0, -1.05 }, { -1.1, 0.9, 1.0, -1.05 } };   // L, R, U, D
static double g_steerFrom = 60.0;
static bool g_ownTransforms = false;                    // phase 47d (variant 4): every part its own transform (the ATS 15:06 truck)
// the head in the play space (LOCAL: origin = the seat): small yaw / nod / lean
static Pose Head(double t) {
    Pose h{};
    double qy[4], qx[4];
    Qax(1, 0.10 * std::sin(2 * 3.14159265 * 0.4 * t), qy);
    Qax(0, 0.05 * std::sin(2 * 3.14159265 * 0.3 * t), qx);
    FpsPose::QMul(qy, qx, h.q);
    h.p[0] = 0.02 * std::sin(2 * 3.14159265 * 0.25 * t); h.p[1] = 0.01 * std::sin(2 * 3.14159265 * 0.5 * t); h.p[2] = 0.0;
    return h;
}
static Pose Eye(double t, int e) {                       // e 0 = right (+x), 1 = left
    const Pose h = Head(t);
    const double off[3] = { e == 0 ? 0.5 * kIpd : -0.5 * kIpd, 0.0, 0.0 };
    double o[3];
    FpsPose::QRotate(h.q, off, o);
    Pose p = h;
    for (int i = 0; i < 3; ++i) p.p[i] += o[i];
    return p;
}
// play space -> cab space: the seat in the cab, then the SWAY (pitch 4 deg 2.5 Hz, roll 2 deg 1.7 Hz, bounce 3 cm 4 Hz)
static double g_wheelA = 0.0;                           // phase 51c Part C: the steering wheel's angle (rad)
static bool g_noSway = false;                           // phase 51b Part C: a parked cab (the play -> cab map is the same every frame)
static M4 g_cabTilt = Identity();                      // phase 51d Part D: the cab turned against the play space (play -> cab rotation)
static M4 G(double t) {
    const M4 seat = Mul(T(0.45, 1.25, 0.35), g_cabTilt);
    if (g_noSway) return seat;
    const M4 sway = Mul(T(0.0, 0.03 * std::sin(2 * 3.14159265 * 4.0 * t), 0.0),
                        Mul(Rax(0, 0.0698 * std::sin(2 * 3.14159265 * 2.5 * t)), Rax(2, 0.035 * std::sin(2 * 3.14159265 * 1.7 * t))));
    return Mul(seat, sway);
}
// a part's model space -> cab space: models in cm, x forward / y left / z up (a different handedness than the cab's axes)
static M4 Perm() {
    M4 m{};
    m.m[0][1] = -1.0; m.m[1][2] = 1.0; m.m[2][0] = -1.0; m.m[3][3] = 1.0;
    return Mul(m, S(0.01));
}
enum { PBody, PDash, PWheel, PCar, POverlay, PWorld, PN };
static const char* const kPartName[PN] = { "body", "dashboard", "steering wheel", "car-layout console", "camera overlay", "world" };
static const UINT kIc[PN] = { 2400, 1800, 3000, 1000, 5000, 9000 };
static M4 PartModel(int p, double t, double frame) {
    switch (p) {
    case PBody: return Perm();
    case PDash: return g_ownTransforms ? Mul(T(0.0, 0.9, -0.8), Perm()) : Perm();   // 47c: shares the body's transform (the cab group)
    case PWheel: {
        const double a = g_wheelA +                       // (phase 51c Part C: a set wheel angle)
                         (frame >= g_steerFrom && frame < g_steerFrom + 50.0 ? 0.7 * std::sin(3.14159265 * (frame - g_steerFrom) / 50.0) : 0.0);
        return Mul(Mul(T(0.45, 1.0, -0.2), Rax(2, a)), Perm());   // turns about the cab's z (towards the driver)
    }
    case PCar: return g_ownTransforms ? Mul(T(0.0, 0.8, -0.5), Perm()) : Perm();
    case PWorld: return Mul(T(3.0, 0.0, -20.0), Perm());
    default: return Identity();
    }
}
static M4 View(double t, int e) { return InvM(Mul(G(t), PoseM(Eye(t, e)))); }
static M4 Mvp(int p, double t, double frame, int e) {
    const M4 P = Proj(kTan[e][0], kTan[e][1], kTan[e][2], kTan[e][3]);
    if (p == POverlay) {                                  // camera-relative: projection * head rotation, origin at the eye
        M4 r = View(t, e);
        r.m[0][3] = r.m[1][3] = r.m[2][3] = 0.0;
        return Mul(P, r);
    }
    return Mul(P, Mul(View(t, e), PartModel(p, t, frame)));
}
static XrIn Xr(double t, bool swapped) {             // swapped: the OpenXR views handed in the wrong eye order (FpsCab must notice)
    XrIn x;
    x.ok = true; x.local = true;
    for (int e = 0; e < 2; ++e) { x.eye[e] = Eye(t, swapped ? 1 - e : e); x.fw[e] = kTan[e][1] - kTan[e][0]; }
    return x;
}
// phase 51d: the truth -- the box SQUARE TO THE CAB: built around the seat (the play-space origin at the lock moment t0, LOCAL pivot)
// along the cab's own axes (cab space: x right, y up, -z forward), eye 0's fov width, fixed in the cab -> in cab space
static void BoxCabTruth(const Settings& s, double t0, double out[4][3], double centre[3]) {
    LockFrame lf;
    lf.fw = kTan[0][1] - kTan[0][0];
    double cp[4][3], cc[3];
    BoxPlay(s, lf, cp, cc);
    const M4 G0 = G(t0);
    const double pv[3] = { G0.m[0][3], G0.m[1][3], G0.m[2][3] };   // the seat in the cab at t0
    for (int k = 0; k < 4; ++k) for (int i = 0; i < 3; ++i) out[k][i] = pv[i] + cp[k][i];
    if (centre) for (int i = 0; i < 3; ++i) centre[i] = pv[i] + cc[i];
}
// -> its clip corners in eye e at time t
static void TruthClip(const Settings& s, double t0, double t, int e, float clip[4][4]) {
    double cp[4][3];
    BoxCabTruth(s, t0, cp, nullptr);
    const M4 P = Proj(kTan[e][0], kTan[e][1], kTan[e][2], kTan[e][3]);
    const M4 M = Mul(P, View(t, e));
    for (int k = 0; k < 4; ++k) {
        double c[4];
        ToClip(M, cp[k], c);
        for (int i = 0; i < 4; ++i) clip[k][i] = (float)c[i];
        clip[k][2] = 0.5f * clip[k][3];
    }
}

// ---- Part A -------------------------------------------------------------------------------------------------------------------
static void PartA() {
    printf("Part A: maths\n");
    const double t = 0.3, fr = 0.0;
    // the eye frame of an MVP: C = the eye in model space, k = metres per model unit (cm models: 0.01)
    for (int e = 0; e < 2; ++e) {
        const M4 M = Mvp(PBody, t, fr, e);
        EyeFrame ef;
        CHECK(EyeFrameOf(M, &ef), "EyeFrameOf eye %d", e);
        const M4 eyeInModel = Mul(InvM(PartModel(PBody, t, fr)), Mul(G(t), PoseM(Eye(t, e))));
        double d = 0.0;
        for (int i = 0; i < 3; ++i) d = std::fmax(d, std::fabs(ef.C[i] - eyeInModel.m[i][3]));
        CHECK(d < 1e-6, "eye centre in model space off by %.3g units (eye %d)", d, e);
        CHECK(std::fabs(ef.k - 0.01) < 1e-9, "k = %.6f, want 0.01", ef.k);
        CHECK(std::fabs(ef.p00 * (kTan[e][1] - kTan[e][0]) - 2.0) < 1e-9, "p00 %.6f", ef.p00);
        CHECK(ShapeOk(M) && !OriginFree(M), "body MVP shape / origin-free");
        CHECK(OriginFree(Mvp(POverlay, t, fr, e)), "the camera overlay must be origin-free");
    }
    // play -> model map + the box: at t0 the box seen through any part's MVP = the play-space box projected with the eye pose
    Settings s;
    s.x = 10; s.y = -20; s.distCm = 90; s.yaw = 15; s.pitch = 25; s.roll = -10; s.scale = 0.5;
    EyeFrame ef[2];
    Pose ps[2];
    for (int e = 0; e < 2; ++e) { EyeFrameOf(Mvp(PBody, t, fr, e), &ef[e]); ps[e] = Eye(t, e); }
    M4 A;
    double upm = 0.0, ratio = 0.0;
    bool sw = true;
    CHECK(PlayToModel(ef, ps, 2, &A, &upm, &sw, &ratio), "PlayToModel");
    CHECK(!sw && std::fabs(ratio - 1.0) < 1e-6 && std::fabs(upm - 100.0) < 1e-4, "swapped %d ratio %.6f upm %.4f", sw, ratio, upm);
    {
        Pose swp[2] = { ps[1], ps[0] };
        M4 A2;
        bool sw2 = false;
        PlayToModel(ef, swp, 2, &A2, nullptr, &sw2, nullptr);
        double d = 0.0;
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) d = std::fmax(d, std::fabs(A2.m[i][j] - A.m[i][j]));
        CHECK(sw2 && d < 1e-9, "eye order swap detected %d, map diff %.3g", sw2, d);
    }
    LockFrame lf;
    lf.fw = kTan[0][1] - kTan[0][0];
    double cp[4][3], cc[3];
    BoxPlay(s, lf, cp, cc);
    double maxPx = 0.0;
    for (int part : { PBody, PDash, PCar }) {
        const M4 Tc = Mul(InvM(Mvp(part, t, fr, 0)), Mvp(PBody, t, fr, 0));   // body = the reference
        for (double tt : { t, t + 0.137, t + 0.5 }) {     // later: the cab swayed; the box must be cab-fixed
            for (int e = 0; e < 2; ++e) {
                const M4 truth = Mul(Proj(kTan[e][0], kTan[e][1], kTan[e][2], kTan[e][3]), Mul(View(tt, e), G(t)));
                for (int k = 0; k < 4; ++k) {
                    const double p[4] = { cp[k][0], cp[k][1], cp[k][2], 1.0 };
                    double aref[4], a[4], c[4], w[4];
                    Apply(A, p, aref);
                    Apply(Tc, aref, a);
                    Apply(Mvp(part, tt, fr, e), a, c);
                    Apply(truth, p, w);
                    const double px = std::fabs(c[0] / c[3] - w[0] / w[3]) * kW / 2 + std::fabs(c[1] / c[3] - w[1] / w[3]) * kH / 2;
                    maxPx = std::fmax(maxPx, px);
                }
            }
        }
    }
    CHECK(maxPx < 1e-4, "box through the parts' MVPs vs the cab-fixed truth: %.3g px", maxPx);
    printf("  the box through 3 parts x 3 moments x 2 eyes vs the cab-fixed truth: max %.2g px\n", maxPx);
    // at the lock moment = the mode-1 box (fixed in the play space, projected with the eye pose)
    {
        double m1 = 0.0;
        for (int e = 0; e < 2; ++e) {
            const M4 playClip = Mul(Proj(kTan[e][0], kTan[e][1], kTan[e][2], kTan[e][3]), InvM(PoseM(Eye(t, e))));
            for (int k = 0; k < 4; ++k) {
                const double p[4] = { cp[k][0], cp[k][1], cp[k][2], 1.0 };
                double a[4], c[4], w[4];
                Apply(A, p, a);
                Apply(Mvp(PBody, t, fr, e), a, c);
                Apply(playClip, p, w);
                m1 = std::fmax(m1, std::fabs(c[0] / c[3] - w[0] / w[3]) * kW / 2 + std::fabs(c[1] / c[3] - w[1] / w[3]) * kH / 2);
            }
        }
        CHECK(m1 < 1e-4, "at the lock the attached box = the play-space box: %.3g px", m1);
    }
    // the box size: the mode-1 rule wM = d fw 0.08 scale
    {
        double w = 0.0;
        BoxPlay(s, lf, cp, cc, &w);
        const double e01 = std::sqrt(std::pow(cp[1][0] - cp[0][0], 2) + std::pow(cp[1][1] - cp[0][1], 2) + std::pow(cp[1][2] - cp[0][2], 2));
        CHECK(std::fabs(w - 0.9 * lf.fw * 0.08 * 0.5) < 1e-12 && std::fabs(e01 - w) < 1e-9, "box width %.5f / edge %.5f", w, e01);
        Settings s0;                                      // no turn: faces the pivot, upright, centred on the anchor ray
        BoxPlay(s0, lf, cp, cc);
        CHECK(std::fabs(cp[0][1] - cp[1][1]) < 1e-12 && cp[0][0] < cp[1][0] && cp[0][1] > cp[2][1], "upright, TL left of TR, top above");
        const double dist = std::sqrt(cc[0] * cc[0] + cc[1] * cc[1] + cc[2] * cc[2]);
        CHECK(std::fabs(dist - 0.8) < 1e-12 && cc[2] < 0.0 && cc[1] > 0.0, "anchor 0.8 m ahead, up for fps_vr_y -25 (%.3f %.3f %.3f)",
              cc[0], cc[1], cc[2]);
    }
    // the MVP block: rows 4..7 normally; the car layout (row 4 = 0 0 0 1, the MVP in rows 5..8)
    {
        float rows[8 * 4] = {};
        const M4 M = Mvp(PBody, t, fr, 0);
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) rows[r * 4 + c] = (float)M.m[r][c];
        M4 o;
        CHECK(FindMvpRow(rows, 8, &o) == 4, "normal layout -> rows 4..7");
        float car[8 * 4] = {};
        car[3] = 1.0f;
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) car[(r + 1) * 4 + c] = (float)M.m[r][c];
        CHECK(FindMvpRow(car, 8, &o) == 5, "car layout -> rows 5..8");
        CHECK(FindMvpRow(car, 4, &o) == 0, "rows 5..8 not mirrored -> none");
    }
    // phase 47c: the transform groups -- body / dashboard / car-layout console share one MVP, the wheel (even at rest) has its own
    CHECK(SameTransform(Mvp(PBody, t, fr, 0), Mvp(PDash, t, fr, 0)) && SameTransform(Mvp(PBody, t, fr, 1), Mvp(PCar, t, fr, 1)),
          "cab parts must share the transform");
    CHECK(!SameTransform(Mvp(PBody, t, 0.0, 0), Mvp(PWheel, t, 0.0, 0)), "the wheel at rest must be its own group");
    // the moving-part test: rigid parts agree (0), the wheel turned 20 deg puts the box centre far away
    {
        const M4 Tw = Mul(InvM(Mvp(PWheel, t, 0.0, 0)), Mvp(PBody, t, fr, 0));
        const double p[4] = { cc[0], cc[1], cc[2], 1.0 };
        double aref[4], aw[4], ad[4];
        Apply(A, p, aref);
        Apply(Tw, aref, aw);
        const M4 Td = Mul(InvM(Mvp(PDash, t, fr, 0)), Mvp(PBody, t, fr, 0));
        Apply(Td, aref, ad);
        const double t2 = t + 0.4;
        const double rigid = Disagree(Mvp(PDash, t2, fr, 0), ad, Mvp(PBody, t2, fr, 0), aref);
        const double moved = Disagree(Mvp(PWheel, t2, g_steerFrom + 15.0, 0), aw, Mvp(PBody, t2, fr, 0), aref);
        CHECK(rigid >= 0.0 && rigid < 1e-6, "rigid parts disagree by %.3g m", rigid);
        CHECK(moved > 0.05, "the turned wheel disagrees by only %.3f m", moved);
        printf("  moving-part test: rigid %.2g m, turned wheel %.3f m\n", rigid, moved);
    }
}

// ---- Part B -------------------------------------------------------------------------------------------------------------------
struct Gpu {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext1> ctx1;
    ComPtr<ID3D11Buffer> cb;                              // the cbuffer ring (64 KB, dynamic: WRITE_DISCARD per pass)
    ComPtr<ID3D11Buffer> ib[3];                           // index buffers: the truck, the same truck after a pool rotation, a new truck
    ComPtr<ID3D11Texture2D> rt, rd;
    ComPtr<ID3D11RenderTargetView> rtv;
};
static bool Readback(Gpu& g, std::vector<uint8_t>& px) {
    g.ctx->CopyResource(g.rd.Get(), g.rt.Get());
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(g.ctx->Map(g.rd.Get(), 0, D3D11_MAP_READ, 0, &ms))) return false;
    px.resize((size_t)kW * kH * 4);
    for (int y = 0; y < kH; ++y) memcpy(&px[(size_t)y * kW * 4], (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch, (size_t)kW * 4);
    g.ctx->Unmap(g.rd.Get(), 0);
    return true;
}
// weighted centroid of the pixels that differ from the grey background (the box's text + outline)
static bool Centroid(const std::vector<uint8_t>& px, double* cx, double* cy, double* mass) {
    double sx = 0, sy = 0, sw = 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const uint8_t* p = &px[((size_t)y * kW + x) * 4];
            const double w = std::fabs(p[0] - 128.0) + std::fabs(p[1] - 128.0) + std::fabs(p[2] - 128.0);
            sx += w * x; sy += w * y; sw += w;
        }
    *mass = sw;
    if (sw < 1.0) return false;
    *cx = sx / sw; *cy = sy / sw;
    return true;
}
static void Clear(Gpu& g) { const float c[4] = { 128.f / 255.f, 128.f / 255.f, 128.f / 255.f, 1.0f }; g.ctx->ClearRenderTargetView(g.rtv.Get(), c); }

// variant 0: eye 0 (right) is rendered first in each frame; 1: the left eye first; 2: + the OpenXR views in the wrong order;
// 3 (phase 47b): the Present (Latch) between the two eyes' blits; 4 (phase 47d): no shared transform, a Reset re-lock, back to truck A
static int PartB(int variant) {
    printf("Part B variant %d, 2 eyes x %d frames (%s): WARP -- DrawIdRecord + FpsCab + menu.cpp VSCab\n", variant, variant == 4 ? 640 : 560,
           variant == 0 ? "right eye first" : variant == 1 ? "left eye first" : variant == 2 ? "left eye first, OpenXR views swapped"
           : variant == 3 ? "a Present between the two eyes' blits" : "every part its own transform (ATS): the wheel is the first reference");
    Gpu g;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx)) ||
        FAILED(g.ctx.As(&g.ctx1))) { printf("  WARP device failed\n"); return 1; }
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 65536; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g.dev->CreateBuffer(&bd, nullptr, &g.cb);
        std::vector<uint16_t> idx(20000, 0);
        D3D11_BUFFER_DESC ib{};
        ib.ByteWidth = (UINT)(idx.size() * 2); ib.Usage = D3D11_USAGE_DEFAULT; ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{ idx.data(), 0, 0 };
        for (auto& b : g.ib) g.dev->CreateBuffer(&ib, &sd, &b);
        D3D11_TEXTURE2D_DESC td{};
        td.Width = kW; td.Height = kH; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
        g.dev->CreateTexture2D(&td, nullptr, &g.rt);
        g.dev->CreateRenderTargetView(g.rt.Get(), nullptr, &g.rtv);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        g.dev->CreateTexture2D(&td, nullptr, &g.rd);
    }
    DrawIdRecord ring[4];
    Settings set;                                         // the defaults: x 0, y -25 deg, 80 cm, no turn, scale 0.3
    set.scale = 1.0;                                      // (a bigger box: more pixels to compare)
    SetMonitorEvery(12);                                  // phase 47d: the DLL's default (6 frames)
    SetLockSamples(1);                                    // phase 51b: the truth below is the box of ONE lock frame t0
    SetCabPoseProvider(nullptr);
    g_ownTransforms = variant == 4;
    SetActive(true, 0);
    SetSettings(set);
    const Stats s0 = GetStats();                          // (the totals run on over the variants)
    // phase 47b timeline: 0-29 eye 0 has no usable record (the lock must wait for both eyes), 30-44 the OpenXR views are in a VIEW
    // space (no lock without poses), the steering wheel turns 120-170, exterior camera 190-209, buffer-pool rotation at 230, a
    // LOADING screen 260-299 (eye 0 has no world blit, eye 1 shows other cabin-layer draws, VIEW-space views, and every 3rd frame
    // looks attachable: the decision must not flap), a truck change at 320.
    const int kFrames = variant == 4 ? 640 : 560, kKeysAt = 450, kResetAt = 240, kBackAt = 450, kNoEye0To = 30, kNoXrTo = 45, kExtFrom = 190, kExtTo = 210, kPoolAt = 230, kLoadFrom = 260, kLoadTo = 300,
              kTruckAt = 320;
    g_steerFrom = 120.0;
    const float build = (float)(kW * 0.08 / TuningMenu::kWidgetRefW * set.scale);
    const wchar_t* l1 = L"72 fps";
    const wchar_t* l2 = L"13.9 ms";
    const float ident[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
    std::vector<uint8_t> pxA, pxB, pxC;
    int attached = 0, compared = 0, lagMeasured = 0, badFrames = 0, attachedExt = 0, attachedAfterSteer = 0;
    double maxErr = 0.0, sumLag = 0.0, maxDiffShare = 0.0;
    uint64_t locksBeforeTruck = 0, reacqBeforeExt = 0, locksEarly = 0;
    int firstBadAfterSteer = -1, twoKinds = 0, loadAttachedDraws = 0, staleChecked = 0;
    uint64_t switches0 = 0, switchesLoad0 = 0, switchesLoad = 0;
    int firstLockEyes = -1;
    uint64_t lastLocks = s0.locks;
    std::vector<uint32_t> lockRefs;
    std::vector<uint64_t> lockKeys;                       // phase 51: each lock's vehicle key
    std::vector<int> lockAt;
    int demoteAt = -1;
    const int rem0 = s0.remembered;
    bool firstLockXr = false;
    for (int f = 0; f < kFrames; ++f) {
        const double t = f / kHz;
        const bool ext = f >= kExtFrom && f < kExtTo;     // exterior camera: no cabin layer
        const bool load = f >= kLoadFrom && f < kLoadTo;  // loading screen
        const bool loadAttachable = load && (f % 3 == 0); // (a normal-looking frame in the middle of it)
        const bool truckB = f >= kTruckAt && !(variant == 4 && f >= kBackAt);   // variant 4: back to truck A at kBackAt
        const int ibi = truckB ? 2 : (f >= kPoolAt ? 1 : 0);
        const UINT siAdd = (truckB ? 100u : 0u) + (variant != 4 && f >= kKeysAt ? 300u : 0u);   // the new truck's parts: new loose keys too;
                                                          // phase 47e: from kKeysAt on the start indices change again, the meshes (IndexCounts) do not
        const UINT icAdd = truckB ? 37u : 0u;             // a real other truck is another mesh: other IndexCounts (47e matches by IndexCount)
        if (variant == 4 && f == kResetAt) Reset("test: the lock row toggled");   // a re-lock on the same truck
        if (f == kTruckAt) locksBeforeTruck = GetStats().locks - s0.locks;
        if (f == kExtFrom) reacqBeforeExt = GetStats().reacquires - s0.reacquires;
        if (f == kNoXrTo) locksEarly = GetStats().locks - s0.locks;
        if (f == kLoadFrom) { const Stats q = GetStats(); switchesLoad0 = q.latchOn + q.latchOff; }
        XrIn xr = Xr(t, variant == 2);
        if ((f >= kNoEye0To && f < kNoXrTo) || (load && !loadAttachable)) xr.ok = false;   // VIEW-space views
        uint64_t serials[2] = {};
        bool blitted[2] = {};
        for (int k = 0; k < 2; ++k) {
            const int e = (variant == 1 || variant == 2) ? 1 - k : k;
            const uint64_t serial = (uint64_t)f * 2 + (uint64_t)k;
            serials[e] = serial;
            if (variant == 3 && k == 1) Latch();          // variant 3: the Present comes BETWEEN the two eyes' blits
            DrawIdRecord& rec = ring[serial % 4];
            rec.Reset();
            if (load && !loadAttachable && e == 0) continue;   // loading: no world blit of eye 0 this frame
            const bool noRecord = f < kNoEye0To && e == 0;
            const bool loadScene = load && !loadAttachable;   // eye 1: other cabin-layer draws (the loading picture)
            // the cbuffer ring: one 16-row (256 B) window per part; normal: rows 0..3 model-view, 4..7 MVP; car: row 4 = 0 0 0 1, 5..8 MVP
            D3D11_MAPPED_SUBRESOURCE ms{};
            g.ctx->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
            float* cbf = (float*)ms.pData;
            memset(cbf, 0, 65536);
            for (int p = 0; p < PN; ++p) {
                float* w = cbf + p * 64;
                const M4 M = Mvp(p, t, (double)f, e);
                const M4 MV = Mul(View(t, e), PartModel(p, t, (double)f));
                for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) w[r * 4 + c] = (float)MV.m[r][c];
                const int r0 = p == PCar ? 5 : 4;
                if (p == PCar) { for (int c = 0; c < 4; ++c) w[4 * 4 + c] = c == 3 ? 1.0f : 0.0f; }
                for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) w[(r0 + r) * 4 + c] = (float)M.m[r][c];
            }
            g.ctx->Unmap(g.cb.Get(), 0);
            for (int p = 0; p < PN && !noRecord; ++p) {
                const bool cabin = p != PWorld && !ext;
                D3D11_VIEWPORT vp{ 0.0f, 0.0f, (float)kW, (float)kH, cabin ? 0.9f : 0.0f, cabin ? 1.0f : 0.9f };
                g.ctx->RSSetViewports(1, &vp);
                ID3D11Buffer* cbs = g.cb.Get();
                const UINT first = (UINT)p * 16u, num = 16u;
                g.ctx1->VSSetConstantBuffers1(0, 1, &cbs, &first, &num);
                g.ctx->IASetIndexBuffer(g.ib[ibi].Get(), DXGI_FORMAT_R16_UINT, 0);
                rec.Record(g.ctx1.Get(), kIc[p] + icAdd + (loadScene ? 7u : 0u), 1, (UINT)p * 10u + siAdd + (loadScene ? 500u : 0u), 0, 0, false);
            }
            rec.Flush(g.ctx.Get());
            OnBlit(g.ctx.Get(), e, rec, serial, (uint64_t)f, k == 0, xr);
            blitted[e] = true;
        }
        if (firstLockEyes < 0 && GetStats().locks > s0.locks) { const Stats q = GetStats(); firstLockEyes = q.lockEyes; firstLockXr = q.lockXr; }
        {                                                 // phase 47d: each lock's reference, and when the wheel was found moving
            const Stats q = GetStats();
            if (q.locks != lastLocks && q.state == 2) {
                lastLocks = q.locks; lockRefs.push_back(q.refIc); lockAt.push_back(f); lockKeys.push_back(q.vehicleKey);
                CHECK(q.vehicleKey == VehicleKey() && q.locks == LockCount(), "phase 51: the stats / accessors disagree");
            }
            if (demoteAt < 0 && q.remembered > rem0) demoteAt = f;
        }
        // the late draw of both eye pictures (after both blits, as at the OpenXR release): ONE box per picture -- the attached one on
        // an attached frame (or none), else the lock-1 box (here only counted: it is inject.cpp's drawn-in / quad path)
        const uint64_t passSeq = (uint64_t)f * 2 + 2;
        const bool on = Attached();
        for (int e = 0; e < 2; ++e) {
            Clear(g);
            Place pl;
            int why = 0;
            const bool got = Get(e, passSeq, &pl, &why);
            if (got && !on) ++twoKinds;                    // an attached placement on a fixed frame = two kinds in one frame
            if (on && got && f == 180 && !staleChecked) {  // a pass started since this blit: the mirror may be reused -> never used
                Place p2;
                int w2 = 0;
                ++staleChecked;
                CHECK(!Get(e, serials[e] + 3, &p2, &w2) && w2 == kStale, "a stale pass was not refused (why %d)", w2);
            }
            bool drew = false;
            if (on && got) drew = TuningMenu::DrawWidgetMvp(g.ctx.Get(), g.rtv.Get(), kW, kH, pl.srv, pl.off, pl.corner, ident, build,
                                                             TuningMenu::kOutRaw, l1, l2);
            if (on) NoteDraw(drew, got ? (drew ? 0 : kShader) : why);
            if (ext) { CHECK(!got, "frame %d eye %d exterior: attached placement given (why %d)", f, e, why); if (got) ++attachedExt; }
            if (load && drew) ++loadAttachedDraws;
            if (!drew) continue;
            ++attached;
            Readback(g, pxA);
            // the truth of THIS frame and of the previous frame (what a 1-frame lag would show)
            const Stats st = GetStats();
            const double t0 = (double)st.lockFrame / kHz;
            float clip[4][4];
            TruthClip(set, t0, t, e, clip);
            Clear(g);
            TuningMenu::DrawWidgetCorners(g.ctx.Get(), g.rtv.Get(), kW, kH, clip, build, TuningMenu::kOutRaw, l1, l2);
            Readback(g, pxB);
            double ax, ay, am, bx, by, bm;
            if (!Centroid(pxA, &ax, &ay, &am) || !Centroid(pxB, &bx, &by, &bm)) continue;
            ++compared;
            const double err = std::hypot(ax - bx, ay - by);
            size_t diff = 0, ink = 0;
            for (size_t i = 0; i < pxA.size(); i += 4) {
                const int da = std::abs(pxA[i] - pxB[i]) + std::abs(pxA[i + 1] - pxB[i + 1]) + std::abs(pxA[i + 2] - pxB[i + 2]);
                if (pxB[i] != 128 || pxA[i] != 128) ++ink;
                if (da > 24) ++diff;
            }
            const double share = ink ? (double)diff / (double)ink : 0.0;
            // 47c: the wheel is never a carrier -- exact while it turns; 47d variant 4 (no shared transform): the first lock may take it,
            // until the monitor drops it (+ 2 frames for the draws already placed)
            const bool steerSettling = variant == 4 && f >= g_steerFrom && (demoteAt < 0 || f <= demoteAt + 2);
            if (!steerSettling) {
                maxErr = std::fmax(maxErr, err);
                maxDiffShare = std::fmax(maxDiffShare, share);
                if (err > 0.1 || share > 0.02) {
                    ++badFrames;
                    if (badFrames <= 6) printf("  frame %d eye %d: centroid off by %.3f px, %.1f %% of the box pixels differ (entry %d)\n",
                                               f, e, err, share * 100.0, pl.entry);
                }
            } else if (f >= g_steerFrom + 2 && err > 0.1 && firstBadAfterSteer < 0) firstBadAfterSteer = f;
            if (f >= g_steerFrom + 24) ++attachedAfterSteer;
            if (f > 0 && f - 1 >= (int)st.lockFrame) {
                TruthClip(set, t0, (f - 1) / kHz, e, clip);
                Clear(g);
                TuningMenu::DrawWidgetCorners(g.ctx.Get(), g.rtv.Get(), kW, kH, clip, build, TuningMenu::kOutRaw, l1, l2);
                Readback(g, pxC);
                double cx, cy, cm;
                if (Centroid(pxC, &cx, &cy, &cm)) { sumLag += std::hypot(cx - bx, cy - by); ++lagMeasured; }
            }
        }
        (void)blitted;
        if (variant != 3) Latch();                        // Present: this frame's verdict -> the next frame's box kind
        if (f == kLoadTo - 1) { const Stats q = GetStats(); switchesLoad = q.latchOn + q.latchOff - switchesLoad0; }
        g.ctx->Flush();
        Sleep(1);                                         // (frame pacing: the lagged readbacks land as in a game)
    }
    {
        const Stats q = GetStats();
        switches0 = q.latchOn + q.latchOff - s0.latchOn - s0.latchOff;
    }    Stats st = GetStats();
    st.locks -= s0.locks; st.relocks -= s0.relocks; st.reacquires -= s0.reacquires;
    printf("  attached in %d of %d eye pictures, compared %d: max centroid error %.4f px, max %.2f %% of the box pixels differ; a 1-frame "
           "lag would be off by %.2f px on average\n", attached, kFrames * 2, compared, maxErr, maxDiffShare * 100.0,
           lagMeasured ? sumLag / lagMeasured : 0.0);
    printf("  locks %llu (new after the truck change %llu), re-acquires %llu, parts %d (moving %d), lock frame %llu\n",
           (unsigned long long)st.locks, (unsigned long long)st.relocks, (unsigned long long)st.reacquires, st.entries, st.demoted,
           (unsigned long long)st.lockFrame);
    {                                                     // the logged cab-vs-play-space figure against the scene's own sway
        double cp[4][3], xc[3];                           // (phase 51d: the cab-square box's centre, fixed in the cab)
        BoxCabTruth(set, (double)st.lockFrame / kHz, cp, xc);
        double p0[4];
        ToClip(InvM(G((double)st.lockFrame / kHz)), xc, p0);
        double tmax = 0.0;
        for (int f = (int)st.lockFrame; f < kFrames; ++f) {
            double c[4];
            ToClip(InvM(G(f / kHz)), xc, c);
            tmax = std::fmax(tmax, std::sqrt(std::pow(c[0] - p0[0], 2) + std::pow(c[1] - p0[1], 2) + std::pow(c[2] - p0[2], 2)));
        }
        printf("  the cab against the play space since the last lock: logged max %.2f cm over %d readbacks, the scene's max %.2f cm\n",
               st.devMaxM * 100.0, st.devN, tmax * 100.0);
        CHECK(st.devN >= 8 && st.devMaxM <= tmax + 1e-3 && st.devMaxM >= 0.6 * tmax, "cab-vs-play figure %.4f m (n %d), scene max %.4f m",
              st.devMaxM, st.devN, tmax);
    }
    printf("  attach decisions: %llu switches in all, %llu during the loading screen (40 frames, every 3rd attachable), %d attached boxes drawn in "
           "it; first lock: %d eye(s), poses %s; monitor readbacks %llu; held pictures %llu\n", (unsigned long long)switches0,
           (unsigned long long)switchesLoad, loadAttachedDraws, firstLockEyes, firstLockXr ? "yes" : "NO",
           (unsigned long long)st.monitors, (unsigned long long)st.held);
    CHECK(locksEarly == 0, "a lock was taken with eye 0 absent / without poses (%llu)", (unsigned long long)locksEarly);
    CHECK(firstLockEyes == 2 && firstLockXr, "the first lock used %d eye(s), poses %d", firstLockEyes, (int)firstLockXr);
    CHECK(twoKinds == 0, "%d eye pictures got an attached placement on a fixed frame (two kinds in one frame)", twoKinds);
    CHECK(switchesLoad <= 1 && loadAttachedDraws <= 2 * kOffFrames, "loading screen: %llu switches, %d attached boxes (flicker)",
          (unsigned long long)switchesLoad, loadAttachedDraws);
    CHECK(st.monitors - s0.monitors > 40, "monitor readbacks %llu", (unsigned long long)(st.monitors - s0.monitors));
    CHECK(staleChecked == 1, "stale check not reached");
    CHECK(badFrames == 0, "%d attached eye pictures not at the cab-fixed truth of the same frame", badFrames);
    CHECK(compared > 400, "only %d eye pictures compared", compared);
    CHECK(lagMeasured > 0 && sumLag / lagMeasured > 1.0, "the sway is too small to show a 1-frame lag (%.2f px)", lagMeasured ? sumLag / lagMeasured : 0.0);
    CHECK(attachedExt == 0, "attached %d times with no cabin layer", attachedExt);
    CHECK(attachedAfterSteer > 100, "only %d attached pictures after the wheel started turning", attachedAfterSteer);
    {
        char refs[160];
        int o = 0;
        for (size_t i = 0; i < lockRefs.size() && o < 140; ++i)
            o += snprintf(refs + o, sizeof(refs) - (size_t)o, "%s%u@%d", i ? ", " : "", lockRefs[i], lockAt[i]);
        printf("  lock references (IndexCount@frame): %s; wheel found moving at frame %d (%d frames after it began to turn); remembered "
               "%d; monitor CPU %.1f us per readback\n", o ? refs : "none", demoteAt, demoteAt >= 0 ? demoteAt - (int)g_steerFrom : -1,
               GetStats().remembered, (st.monitors - s0.monitors) ? (GetStats().monitorUs - s0.monitorUs) / (double)(st.monitors - s0.monitors) : 0.0);
    }
    // phase 51: the vehicle key -- every lock has one; truck B differs from truck A; the same truck (a Reset re-lock, the keys
    // changed at kKeysAt, back to truck A) gives the same key again
    for (size_t i = 0; i < lockKeys.size(); ++i) CHECK(lockKeys[i] != 0, "lock %zu has no vehicle key", i);
    if (variant != 4) {
        CHECK(lockKeys.size() == 2 && lockKeys[0] != lockKeys[1], "phase 51: truck A / B keys %016llx / %016llx (want 2 different)",
              lockKeys.size() > 0 ? (unsigned long long)lockKeys[0] : 0ull, lockKeys.size() > 1 ? (unsigned long long)lockKeys[1] : 0ull);
    } else {
        CHECK(lockKeys.size() == 4 && lockKeys[0] == lockKeys[1] && lockKeys[2] != lockKeys[0] && lockKeys[3] == lockKeys[0],
              "phase 51: keys of the 4 locks (A, A after Reset, B, A again) not A A B A");
    }
    if (variant != 4) {                                   // 47c: the cab group shares a transform -- the wheel is never a carrier
        CHECK(switches0 <= 8, "%llu attach switches in all (flapping)", (unsigned long long)switches0);
        CHECK(locksBeforeTruck == 1, "the exterior camera / pool rotation caused a new lock (%llu locks before the truck change)",
              (unsigned long long)locksBeforeTruck);
        // (phase 47e: the keys change again at kKeysAt, the IndexCounts stay -> found by IndexCount, no third lock)
        CHECK(st.relocks == 1 && st.locks == 2, "truck change: relocks %llu locks %llu (want 1 / 2; a third = the IndexCount match failed)", (unsigned long long)st.relocks,
              (unsigned long long)st.locks);
        CHECK(reacqBeforeExt == 0 && st.demoted == 0, "the wheel was a carrier: re-acquires %llu, demoted %d", (unsigned long long)reacqBeforeExt, st.demoted);
        CHECK(st.entries == 3, "the lock holds %d carriers (want the 3 cab-group parts, not the wheel)", st.entries);
        CHECK(firstBadAfterSteer < 0, "the box moved with the wheel (frame %d)", firstBadAfterSteer);
    } else {                                              // 47d: no shared transform -- the first lock takes the wheel (the biggest)
        CHECK(switches0 <= 12, "%llu attach switches in all (flapping)", (unsigned long long)switches0);
        CHECK(lockRefs.size() == 4, "%zu locks (want: first, re-lock after Reset, truck B, back to truck A)", lockRefs.size());
        CHECK(!lockRefs.empty() && lockRefs[0] == kIc[PWheel], "the first lock took IndexCount %u (the scene's biggest is the wheel)",
              lockRefs.empty() ? 0u : lockRefs[0]);
        CHECK(demoteAt >= 0 && demoteAt - (int)g_steerFrom <= 10, "the turning wheel was found moving %d frames after it began",
              demoteAt < 0 ? -1 : demoteAt - (int)g_steerFrom);
        CHECK(lockRefs.size() >= 2 && lockRefs[1] != kIc[PWheel], "the re-lock after the round trip / Reset took the wheel again");
        CHECK(lockRefs.size() >= 4 && lockRefs[3] != kIc[PWheel], "back on truck A the wheel was taken again (the memory was lost)");
        CHECK(GetStats().remembered == 1, "%d parts remembered (want the wheel of truck A only)", GetStats().remembered);
        CHECK(locksBeforeTruck == 2, "locks before the truck change %llu (want 2: first + the Reset)", (unsigned long long)locksBeforeTruck);
    }    SetActive(false, 0);
    Shutdown();
    return 0;
}

// ---- Part P (phase 51): per-vehicle fps box profiles (src/fps_profile.h, pure) ---------------------------------------------
static void PartP() {
    printf("Part P: vehicle key + fps_profile lines\n");
    using namespace FpsProfile;
    // the key: order-independent, duplicates (both eyes) ignored, only the 3 biggest parts count
    const Part cab[6] = { { 52000, 32, 57 }, { 41000, 32, 57 }, { 13200, 40, 57 }, { 900, 32, 57 }, { 300, 24, 57 }, { 36, 32, 57 } };
    const uint64_t k0 = VehicleKey(cab, 6);
    CHECK(k0 != 0, "no key");
    const Part shuffled[9] = { cab[5], cab[2], cab[4], cab[0], cab[2], cab[3], cab[1], cab[0], cab[5] };
    CHECK(VehicleKey(shuffled, 9) == k0, "the same cab in another order / with duplicates gave another key");
    const Part rev[6] = { cab[5], cab[4], cab[3], cab[2], cab[1], cab[0] };
    CHECK(VehicleKey(rev, 6) == k0, "the reversed order gave another key");
    const Part fewSmall[3] = { cab[1], cab[0], cab[2] };
    CHECK(VehicleKey(fewSmall, 3) == k0, "the small parts changed the key (only the 3 biggest count)");
    Part other[6];
    for (int i = 0; i < 6; ++i) other[i] = cab[i];
    other[1].ic = 41037;                                  // another truck: another mesh among the biggest
    CHECK(VehicleKey(other, 6) != k0, "another cab gave the same key");
    other[1] = cab[1]; other[2].stride = 44;
    CHECK(VehicleKey(other, 6) != k0, "another vertex stride gave the same key");
    other[2] = cab[2]; other[0].fmt = 42;
    CHECK(VehicleKey(other, 6) != k0, "another index format gave the same key");
    CHECK(VehicleKey(cab, 0) == 0, "no parts must give no key");
    Part used[kKeyParts];
    int nu = 0;
    VehicleKey(shuffled, 9, used, &nu);
    CHECK(nu == 3 && used[0].ic == 52000 && used[1].ic == 41000 && used[2].ic == 13200, "the key parts are not the 3 biggest");
    // the line: round trip, clamps, malformed
    Vals v;
    v.x = 12.4f; v.y = -7.6f; v.dist = 95; v.yaw = -10; v.pitch = 5; v.roll = 3; v.scale = 0.45f;
    uint64_t kr = 0;
    Vals vr;
    CHECK(ParseValue(FormatValue(0x00ab12cd34ef5678ull, v).c_str(), &kr, &vr) && kr == 0x00ab12cd34ef5678ull && vr == v,
          "the profile line does not read back (%s)", FormatValue(0x00ab12cd34ef5678ull, v).c_str());
    CHECK(ParseValue(" 1f 99 -99 5000 -400 400 999 9.5\r\n", &kr, &vr) && kr == 0x1f && vr.x == 50.0f && vr.y == -50.0f &&
          vr.dist == FpsPose::kDistMax && vr.yaw == -FpsPose::kYawMax && vr.pitch == FpsPose::kPitchMax && vr.roll == FpsPose::kRollMax &&
          vr.scale == 3.0f, "the profile values are not clamped");
    CHECK(!ParseValue("1f 1 2 3 4 5 6", &kr, &vr), "a line with 6 values was accepted");
    CHECK(!ParseValue("zz 1 2 3 4 5 6 7", &kr, &vr), "a line without a hex key was accepted");
    CHECK(!ParseValue("0 1 2 80 4 5 6 0.3", &kr, &vr), "key 0 was accepted");
    CHECK(!ParseValue("1f 1 2 80 4 5 6 0.3 junk", &kr, &vr), "a line with junk after the values was accepted");
    // the store: put / get / LRU at the cap
    Store st;
    CHECK(st.Put(1, v) && !st.Put(1, v), "put of the same values again reported a change");
    Vals v2 = v; v2.dist = 120;
    CHECK(st.Put(1, v2) && st.Get(1) && st.Get(1)->dist == 120, "an update was not stored");
    for (uint64_t k = 2; k <= (uint64_t)kMax; ++k) st.Put(k, v);
    CHECK(st.n == kMax, "the store holds %d (want %d)", st.n, kMax);
    st.Get(1);                                            // key 1 used last -> key 2 is the least recently used
    st.Put(1000, v);
    CHECK(st.n == kMax && st.Find(2) < 0 && st.Find(1) >= 0 && st.Find(1000) >= 0 && st.nEvicted == 1 && st.evicted[0] == 2,
          "the cap did not drop the least recently used profile");
    // the ini rewrite: CRLF, comments and other keys byte for byte, in place, appended, duplicates / evicted dropped
    {
        Store s2;
        s2.Load("00000000000000aa 1 2 80 0 0 0 0.30");      // from the file
        s2.Load("00000000000000bb 1 2 80 0 0 0 0.30");
        Vals va = v;
        s2.Put(0xaa, va);                                 // aa changed
        s2.Put(0xcc, v2);                                 // cc new
        s2.evicted[s2.nEvicted++] = 0xdd;                 // dd dropped at the cap
        const std::string in =
            "# comment = keep\r\nfps_vr_x = 3.0\r\n  fps_profile = 00000000000000aa 1 2 80 0 0 0 0.30 \r\n; fps_profile = 00000000000000aa x\r\n"
            "fps_profile = garbage line\r\nfps_profile = 00000000000000dd 1 2 80 0 0 0 0.30\r\nfps_profile=00000000000000bb 1 2 80 0 0 0 0.30\r\n"
            "fps_profile = 00000000000000aa 9 9 80 0 0 0 0.30\r\nlast_key = 1";
        const std::string want =
            "# comment = keep\r\nfps_vr_x = 3.0\r\n" + FormatLine(0xaa, va) + "\r\n; fps_profile = 00000000000000aa x\r\n"
            "fps_profile = garbage line\r\n" + FormatLine(0xbb, *s2.Get(0xbb)) + "\r\nlast_key = 1\r\n" + FormatLine(0xcc, v2) + "\r\n";
        const std::string out = Rewrite(in, s2);
        CHECK(out == want, "the CRLF rewrite differs:\n---got\n%s\n---want\n%s", out.c_str(), want.c_str());
        CHECK(Rewrite(out, s2) == out, "a second rewrite changed the file");
        const std::string lf = Rewrite("a = 1\nb = 2\n", s2);
        CHECK(lf.find('\r') == std::string::npos && lf.compare(0, 12, "a = 1\nb = 2\n") == 0 && lf.size() > 12, "the LF rewrite is wrong");
        const std::string empty = Rewrite("", s2);
        CHECK(empty.compare(0, 1, "#") == 0 && empty.find(FormatLine(0xcc, v2)) != std::string::npos, "the new-file rewrite is wrong");
        Store none;
        CHECK(Rewrite(in, none) == in, "an empty store changed the file");
    }
}

// ---- Part P2 (phase 51b): the cab pose + the v2 line (pure) ------------------------------------------------------------------
static double MaxCornerDiff(const double a[4][4], const double b[4][4]) {
    double m = 0.0;
    for (int k = 0; k < 4; ++k) {
        if (!(std::fabs(a[k][3]) > 1e-12) || !(std::fabs(b[k][3]) > 1e-12)) return 1e30;
        for (int i = 0; i < 3; ++i) m = std::fmax(m, std::fabs(a[k][i] / a[k][3] - b[k][i] / b[k][3]));
    }
    return m;
}
static void PartP2() {
    printf("Part P2: cab pose round trip, v1 / v2 lines, colour / brightness / text\n");
    using namespace FpsProfile;
    // a lock's play -> model map (the scene's body part at t = 0.3, eye 0 + eye 1 as FpsCab builds it)
    const double t = 0.3;
    EyeFrame ef[2];
    Pose ps[2];
    for (int e = 0; e < 2; ++e) { EyeFrameOf(Mvp(PBody, t, 0.0, e), &ef[e]); ps[e] = Eye(t, e); }
    M4 A;
    double upm = 0.0;
    bool sw = false;
    double ipd = 0.0;
    CHECK(PlayToModel(ef, ps, 2, &A, &upm, &sw, &ipd), "PlayToModel failed");
    LockFrame lf;
    lf.fw = kTan[0][1] - kTan[0][0];
    Settings s;
    s.x = 12.4; s.y = 18.6; s.distCm = 95; s.yaw = -7; s.pitch = 11; s.roll = -2; s.scale = 0.45;
    // 1. settings -> cab pose -> the same corners as the lock's own path (A BoxPlay)
    CabPose cp;
    CabPoseSet(A, lf, kIc[PBody], 32, 57, &cp);
    double want[4][4], got[4][4];
    {
        double pc[4][3], cc[3];
        BoxPlay(s, lf, pc, cc);
        for (int k = 0; k < 4; ++k) { const double h[4] = { pc[k][0], pc[k][1], pc[k][2], 1.0 }; Apply(A, h, want[k]); }
    }
    CabCorners(cp, s, got);
    CHECK(MaxCornerDiff(want, got) < 1e-12, "cab pose corners differ from the lock path by %.3g", MaxCornerDiff(want, got));
    // 2. through the dlaa.ini line (17 digits): the same corners bit for bit
    Vals v;
    v.x = (float)s.x; v.y = (float)s.y; v.dist = s.distCm; v.yaw = s.yaw; v.pitch = s.pitch; v.roll = s.roll; v.scale = (float)s.scale;
    v.color = 7; v.bright = 60; v.text = 4;
    const std::string line = FormatValue(0x7b8a0fae6ab01548ull, v, &cp);
    uint64_t k = 0;
    Vals vr;
    CabPose cr;
    bool hc = false;
    CHECK(ParseValue(line.c_str(), &k, &vr, &cr, &hc) && k == 0x7b8a0fae6ab01548ull && hc, "the v2 line does not parse: %s", line.c_str());
    CHECK(vr == v && vr.color == 7 && vr.bright == 60 && vr.text == 4, "colour / brightness / text / placement did not round-trip");
    CHECK(cr == cp, "the cab pose did not round-trip exactly through the line");
    CabCorners(cr, s, got);
    CHECK(MaxCornerDiff(want, got) == 0.0, "the parsed cab pose gives other corners (%.3g)", MaxCornerDiff(want, got));
    printf("  v2 line: %zu chars\n", line.size());
    // 3. a lock with a 1-frame POSE ERROR (1.5 deg yaw, 1 cm) lands on the stored corners exactly through the stored pose; its own
    //    play -> cab map would not
    {
        Pose bad[2];
        double qe[4];
        Qax(1, 1.5 * 3.14159265358979 / 180.0, qe);
        for (int e = 0; e < 2; ++e) {
            bad[e] = ps[e];
            FpsPose::QRotate(qe, ps[e].p, bad[e].p);
            bad[e].p[0] += 0.01;
            FpsPose::QMul(qe, ps[e].q, bad[e].q);
        }
        // the new lock's reference is the DASHBOARD (another part of the same rigid group): T = dash model <- body model ... the
        // lock's Entry::T of the body = M_body^-1 M_dash (the reference's model -> the body's)
        const double t2 = 0.9;
        EyeFrame ed[2];
        for (int e = 0; e < 2; ++e) EyeFrameOf(Mvp(PDash, t2, 0.0, e), &ed[e]);
        M4 Abad;
        CHECK(PlayToModel(ed, bad, 2, &Abad, nullptr, nullptr, nullptr), "PlayToModel (pose error) failed");
        const M4 Tbody = Mul(InvM(Mvp(PBody, t2, 0.0, 0)), Mvp(PDash, t2, 0.0, 0));   // dash model -> body model
        M4 Aref;
        CHECK(RefMapFromCab(cr, Tbody, &Aref), "RefMapFromCab failed");
        double pc[4][3], cc[3], viaCab[4][4], viaPlay[4][4];
        BoxPlay(s, CabFrame(cr), pc, cc);
        for (int q = 0; q < 4; ++q) {
            const double h[4] = { pc[q][0], pc[q][1], pc[q][2], 1.0 };
            double r[4];
            Apply(Aref, h, r); Apply(Tbody, r, viaCab[q]);           // the box in the body's model space (as BuildCorners carries it)
            Apply(Abad, h, r); Apply(Tbody, r, viaPlay[q]);
        }
        const double dCab = MaxCornerDiff(viaCab, want), dPlay = MaxCornerDiff(viaPlay, want);
        printf("  1-frame pose error (1.5 deg, 1 cm): stored cab pose off by %.2g model units, the lock's own map by %.2f (cm)\n", dCab, dPlay);
        CHECK(dCab < 1e-9, "the stored cab pose did not land on the stored corners (%.3g)", dCab);
        CHECK(dPlay > 1.0, "the pose error should move the play-space path (%.3f)", dPlay);
    }
    // 4. AverageMaps: 11 maps with a +-0.3 deg jitter + 1 outlier (8 deg) -> the outlier dropped, the mean closer than the worst
    {
        M4 maps[12];
        const double jit[12] = { 0.3, -0.3, 0.2, -0.2, 0.1, -0.1, 0.25, -0.25, 0.15, -0.15, 0.0, 8.0 };
        double worst = 0.0;
        for (int f = 0; f < 12; ++f) {
            Pose b2[2];
            double qe[4];
            Qax(1, jit[f] * 3.14159265358979 / 180.0, qe);
            for (int e = 0; e < 2; ++e) { b2[e] = ps[e]; FpsPose::QRotate(qe, ps[e].p, b2[e].p); FpsPose::QMul(qe, ps[e].q, b2[e].q); }
            PlayToModel(ef, b2, 2, &maps[f], nullptr, nullptr, nullptr);
            if (f < 11) {
                double c[4][4], pc[4][3], cc[3];
                BoxPlay(s, lf, pc, cc);
                for (int q = 0; q < 4; ++q) { const double h[4] = { pc[q][0], pc[q][1], pc[q][2], 1.0 }; Apply(maps[f], h, c[q]); }
                worst = std::fmax(worst, MaxCornerDiff(c, want));
            }
        }
        double pc[4][3], cc[3];
        BoxPlay(s, lf, pc, cc);
        const double probe[5][3] = { { pc[0][0], pc[0][1], pc[0][2] }, { pc[1][0], pc[1][1], pc[1][2] }, { pc[2][0], pc[2][1], pc[2][2] },
                                     { pc[3][0], pc[3][1], pc[3][2] }, { cc[0], cc[1], cc[2] } };
        M4 avg;
        double sp = 0.0;
        const int used = AverageMaps(maps, 12, probe, 5, 0.002 * upm, &avg, &sp);
        double c[4][4];
        for (int q = 0; q < 4; ++q) { const double h[4] = { pc[q][0], pc[q][1], pc[q][2], 1.0 }; Apply(avg, h, c[q]); }
        const double d = MaxCornerDiff(c, want);
        printf("  AverageMaps: %d of 12 kept, spread %.3f units; mean off by %.3f, the worst kept single frame by %.3f (cm)\n", used, sp, d, worst);
        CHECK(used == 11, "the outlier was not dropped (%d kept)", used);
        CHECK(d < 0.25 * worst, "the average (%.3f) is not much better than the worst frame (%.3f)", d, worst);
        double det = 0.0;                                 // still a rotation x scale with the same handedness
        {
            const auto& m = avg.m;
            det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                  m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
            const auto& a = A.m;
            const double detA = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                                a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
            CHECK((det > 0.0) == (detA > 0.0) && std::fabs(std::fabs(det) / std::fabs(detA) - 1.0) < 1e-6, "the average changed the scale / handedness");
        }
    }
    // 5. back-compat: an old (v1, 8-field) line loads without colour / cab pose and is written back as v1; a v2 "-" line; bad v2
    {
        Store st;
        CHECK(st.Load("7b8a0fae6ab01548 0.20 14.60 90 0 0 0 0.40"), "the v1 line did not load");
        const Vals* p = st.Get(0x7b8a0fae6ab01548ull);
        CHECK(p && p->color == -1 && p->bright == -1 && p->text == -1 && !p->HasLook() && !st.Cab(0x7b8a0fae6ab01548ull) &&
              p->dist == 90 && p->y == 14.6f, "the v1 line loaded wrong values");
        CHECK(FormatValue(0x7b8a0fae6ab01548ull, *p) == "7b8a0fae6ab01548 0.20 14.60 90 0 0 0 0.40", "a v1 entry is not written as v1");
        const std::string in = "fps_profile = 7b8a0fae6ab01548 0.20 14.60 90 0 0 0 0.40\r\n";
        CHECK(Rewrite(in, st) == in, "an untouched v1 line was changed");
        Vals v2 = *p; v2.color = 3; v2.bright = 50; v2.text = 1;
        st.Put(0x7b8a0fae6ab01548ull, v2);
        CHECK(Rewrite(in, st) == "fps_profile = 7b8a0fae6ab01548 0.20 14.60 90 0 0 0 0.40 v2 3 50 1 -\r\n", "the v2 rewrite: %s",
              Rewrite(in, st).c_str());
        st.PutCab(0x7b8a0fae6ab01548ull, cp);
        const std::string out = Rewrite(in, st);
        Store st2;
        CHECK(st2.Load(out.c_str() + strlen("fps_profile = ")) && st2.Cab(0x7b8a0fae6ab01548ull) && *st2.Cab(0x7b8a0fae6ab01548ull) == cp &&
              st2.Get(0x7b8a0fae6ab01548ull)->text == 1, "the v2 line with a cab pose does not load back");
        CHECK(Rewrite(out, st2) == out, "a second rewrite of the v2 line changed it");
        uint64_t kk; Vals vv; CabPose cc2; bool h2 = true;
        CHECK(ParseValue("1f 1 2 80 0 0 0 0.3 v2 -1 -1 -1 -", &kk, &vv, &cc2, &h2) && !h2 && !vv.HasLook(), "a v2 line without look / cab failed");
        CHECK(ParseValue("1f 1 2 80 0 0 0 0.3 v2 99 5 9 -", &kk, &vv) && vv.color == FpsPose::kColorCount - 1 &&
              vv.bright == FpsPose::kBrightMin && vv.text == kTextModes - 1, "the v2 look values are not clamped");
        CHECK(!ParseValue("1f 1 2 80 0 0 0 0.3 v4 1 1 1 1 -", &kk, &vv), "an unknown version was accepted");
        // phase 51f: v3 = v2 + opacity after text; a v2 line has none (-1: the live value stays); written as v3 only with an opacity
        {
            CHECK(ParseValue("1f 1 2 80 0 0 0 0.3 v2 3 50 1 -", &kk, &vv) && vv.opacity == -1 && vv.color == 3, "51f: a v2 line must load with opacity -1");
            CHECK(ParseValue("1f 1 2 80 0 0 0 0.3 v3 3 50 1 40 -", &kk, &vv) && vv.opacity == 40 && vv.color == 3 && vv.bright == 50 &&
                  vv.text == 1, "51f: a v3 line did not load");
            CHECK(ParseValue("1f 1 2 80 0 0 0 0.3 v3 3 50 1 5 -", &kk, &vv) && vv.opacity == FpsPose::kOpacityMin, "51f: opacity not clamped low");
            CHECK(ParseValue("1f 1 2 80 0 0 0 0.3 v3 3 50 1 900 -", &kk, &vv) && vv.opacity == FpsPose::kOpacityMax, "51f: opacity not clamped high");
            CHECK(ParseValue("1f 1 2 80 0 0 0 0.3 v3 -1 -1 -1 -1 -", &kk, &vv) && !vv.HasLook(), "51f: a v3 line without look failed");
            CHECK(!ParseValue("1f 1 2 80 0 0 0 0.3 v3 3 50 1 -", &kk, &vv), "51f: a v3 line missing its opacity was accepted");
            Vals o3 = vv; o3.color = 2; o3.bright = 70; o3.text = 0; o3.opacity = 60;
            const std::string l3 = FormatValue(0x1full, o3);
            CHECK(l3 == "000000000000001f 1.00 2.00 80 0 0 0 0.30 v3 2 70 0 60 -", "51f: the v3 line is %s", l3.c_str());
            Vals back; uint64_t kb = 0;
            CHECK(ParseValue(l3.c_str(), &kb, &back) && back == o3 && kb == 0x1full, "51f: the v3 line does not round-trip");
            o3.opacity = -1;
            CHECK(FormatValue(0x1full, o3).find(" v2 2 70 0 -") != std::string::npos, "51f: without an opacity the line must stay v2");
            // a cab pose after the opacity
            const std::string l3c = FormatValue(0x1full, back, &cp);
            Vals b2; CabPose c3; bool hc3 = false;
            CHECK(ParseValue(l3c.c_str(), &kb, &b2, &c3, &hc3) && hc3 && b2 == back && c3 == cp, "51f: v3 + cab pose does not round-trip");
            // Session: an applied profile keeps its opacity, an opacity-less one keeps the live value
            Vals pv = back; pv.opacity = 30;
            Vals live = back; live.opacity = 80;
            CHECK(Session::Applied(live, pv).opacity == 30, "51f: Applied lost the profile's opacity");
            pv.opacity = -1;
            CHECK(Session::Applied(live, pv).opacity == 80, "51f: Applied must keep the live opacity for a v2 profile");
            Store ps; Session ss;
            Vals mine = back; mine.opacity = 30;
            ps.Put(0x55ull, mine);
            Session::Out out = ss.Tick(ps, true, true, true, 1, 0x55ull, false, live);
            CHECK(out.lock == Session::kLockLoaded && out.applyIt && out.apply.opacity == 30, "51f: the lock-on did not hand over the stored opacity");
            live.opacity = 50;                             // a menu edit
            out = ss.Tick(ps, true, true, true, 1, 0x55ull, false, live);
            CHECK(out.nChanged == 1 && ps.Get(0x55ull)->opacity == 50, "51f: an opacity edit did not reach the bound profile");
        }
        CHECK(!ParseValue("1f 1 2 80 0 0 0 0.3 v2 1 1 1 cab 5 6", &kk, &vv), "a short cab pose was accepted");
        CHECK(ParseValue("1f 1 2 80 0 0 0 0.3 v2 1 1 1 cab 0 0 0 1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 0 1.6", &kk, &vv, &cc2, &h2) && !h2,
              "a cab pose with IndexCount 0 was used");
        CHECK(FormatLine(1, vv).size() < 1000 && line.size() < 900, "the line is too long for LoadConfig's 1024-byte buffer");
    }
}

// ---- Part C (phase 51b): the runtime lock-on from a STORED CAB POSE / averaged, WARP ------------------------------------------
static FpsProfile::CabPose g_stored;
static bool g_haveStored = false;
static int g_provAsked = 0;
static bool Provider(uint64_t, FpsProfile::CabPose* out) { ++g_provAsked; if (!g_haveStored) return false; *out = g_stored; return true; }
static double g_errDeg = 0.0, g_errDx = 0.0;            // the OpenXR poses' error this frame (the lock-on pose mismatch)
static double (*g_wheelAt)(int) = nullptr;             // phase 51c: the wheel angle per frame of a run
static int g_collapseUntil = 0;                         // phase 51c: the first N frames of a run hand both eyes the same pose (a broken map)
static int RunUntilLock(Gpu& g, DrawIdRecord* ring, int& f, int maxFrames, uint64_t locks0, double (*errAt)(int)) {
    for (int n = 0; n < maxFrames; ++n, ++f) {
        const double t = f / kHz;
        g_errDeg = errAt ? errAt(n) : 0.0;
        if (g_wheelAt) g_wheelA = g_wheelAt(n);
        XrIn xr = Xr(t, false);
        if (n < g_collapseUntil) xr.eye[1] = xr.eye[0];
        double qe[4];
        Qax(1, g_errDeg * 3.14159265358979 / 180.0, qe);
        for (int e = 0; e < 2; ++e) {
            Pose b = xr.eye[e];
            FpsPose::QRotate(qe, xr.eye[e].p, b.p);
            b.p[0] += g_errDx;
            FpsPose::QMul(qe, xr.eye[e].q, b.q);
            xr.eye[e] = b;
        }
        for (int k = 0; k < 2; ++k) {
            const int e = k;
            const uint64_t serial = (uint64_t)f * 2 + (uint64_t)k;
            DrawIdRecord& rec = ring[serial % 4];
            rec.Reset();
            D3D11_MAPPED_SUBRESOURCE ms{};
            g.ctx->Map(g.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
            float* cbf = (float*)ms.pData;
            memset(cbf, 0, 65536);
            for (int p = 0; p < PN; ++p) {
                float* w = cbf + p * 64;
                const M4 M = Mvp(p, t, 0.0, e);
                const M4 MV = Mul(View(t, e), PartModel(p, t, 0.0));
                for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) w[r * 4 + c] = (float)MV.m[r][c];
                const int r0 = p == PCar ? 5 : 4;
                if (p == PCar) { for (int c = 0; c < 4; ++c) w[4 * 4 + c] = c == 3 ? 1.0f : 0.0f; }
                for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) w[(r0 + r) * 4 + c] = (float)M.m[r][c];
            }
            g.ctx->Unmap(g.cb.Get(), 0);
            for (int p = 0; p < PN; ++p) {
                const bool cabin = p != PWorld;
                D3D11_VIEWPORT vp{ 0.0f, 0.0f, (float)kW, (float)kH, cabin ? 0.9f : 0.0f, cabin ? 1.0f : 0.9f };
                g.ctx->RSSetViewports(1, &vp);
                ID3D11Buffer* cbs = g.cb.Get();
                const UINT first = (UINT)p * 16u, num = 16u;
                g.ctx1->VSSetConstantBuffers1(0, 1, &cbs, &first, &num);
                g.ctx->IASetIndexBuffer(g.ib[0].Get(), DXGI_FORMAT_R16_UINT, 0);
                rec.Record(g.ctx1.Get(), kIc[p], 1, (UINT)p * 10u, 0, 0, false);
            }
            rec.Flush(g.ctx.Get());
            OnBlit(g.ctx.Get(), e, rec, serial, (uint64_t)f, k == 0, xr);
        }
        Latch();
        g.ctx->Flush();
        Sleep(1);
        if (locks0 != ~0ull && GetStats().locks > locks0 && GetStats().state == 2) { ++f; return n + 1; }
    }
    return -1;
}
static double ErrNone(int) { return 0.0; }
static double ErrConst(int) { return 1.5; }
static double ErrJitter(int n) {                          // +-0.5 deg frame to frame + one 8-deg glitch (an outlier)
    static const double k[8] = { 0.5, -0.5, 0.3, -0.3, 0.4, -0.4, 0.1, -0.1 };
    return (n % 37) == 20 ? 8.0 : k[n % 8];
}
static int PartC() {
    printf("Part C: lock-on from a stored cab pose / averaged over several readbacks (WARP, a parked cab)\n");
    Gpu g;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx)) ||
        FAILED(g.ctx.As(&g.ctx1))) { printf("  WARP device failed\n"); return 1; }
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 65536; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g.dev->CreateBuffer(&bd, nullptr, &g.cb);
        std::vector<uint16_t> idx(20000, 0);
        D3D11_BUFFER_DESC ib{};
        ib.ByteWidth = (UINT)(idx.size() * 2); ib.Usage = D3D11_USAGE_DEFAULT; ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{ idx.data(), 0, 0 };
        g.dev->CreateBuffer(&ib, &sd, &g.ib[0]);
    }
    DrawIdRecord ring[4];
    g_noSway = true;
    g_ownTransforms = false;
    g_steerFrom = 1e9;
    Settings set;
    set.x = 10.0; set.y = 20.0; set.distCm = 90; set.roll = -2; set.scale = 0.4;
    SetMonitorEvery(12);
    SetCabPoseProvider(Provider);
    g_haveStored = false;
    SetLockSamples(1);
    SetActive(true, 0);
    SetSettings(set);
    // the truth: the box built in the (true) play space, fixed in the parked cab, in the body's model space
    double truth[4][4];
    {
        LockFrame lf;
        lf.fw = kTan[0][1] - kTan[0][0];
        double pc[4][3], cc[3];
        BoxPlay(set, lf, pc, cc);
        const M4 toBody = Mul(InvM(PartModel(PBody, 0.0, 0.0)), G(0.0));
        for (int q = 0; q < 4; ++q) { const double h[4] = { pc[q][0], pc[q][1], pc[q][2], 1.0 }; Apply(toBody, h, truth[q]); }
    }
    int f = 0;
    // 1. the first lock-on (no stored pose, exact poses): placed from the play space; its cab pose is what a profile stores
    int n = RunUntilLock(g, ring, f, 300, GetStats().locks, ErrNone);
    double c1[4][4] = {};
    FpsProfile::CabPose cp;
    CHECK(n > 0 && GetLockInfo().placed == kPlacedPlay && CurrentCabPose(&cp) && EntryCorners(kIc[PBody], c1), "lock 1 failed (%d)", n);
    CHECK(g_provAsked > 0, "the provider was never asked");
    printf("  lock 1 after %d frames: body corners off the truth by %.2g cm (placed %d, ref IndexCount %u)\n", n, MaxCornerDiff(c1, truth),
           GetLockInfo().placed, cp.ic);
    CHECK(MaxCornerDiff(c1, truth) < 1e-3, "the exact lock is off the truth by %.3g cm", MaxCornerDiff(c1, truth));
    // the cab pose through the ini line (as a restart would load it)
    {
        FpsProfile::Vals v;
        uint64_t k = 0;
        bool hc = false;
        FpsProfile::ParseValue(FpsProfile::FormatValue(0x1234, v, &cp).c_str(), &k, &v, &g_stored, &hc);
        CHECK(hc && g_stored == cp, "the runtime cab pose did not round-trip through the line");
        g_haveStored = true;
    }
    // 2. lock-on again with a 1.5-deg / 1-cm POSE ERROR: placed from the stored cab pose -> the same corners exactly
    for (int rep = 0; rep < 2; ++rep) {
        Reset("test: a re-lock");
        g_errDx = 0.01;
        n = RunUntilLock(g, ring, f, 300, GetStats().locks, ErrConst);
        double c2[4][4] = {};
        const LockInfo li = GetLockInfo();
        CHECK(n > 0 && li.placed == kPlacedCab && li.cabIc == cp.ic && EntryCorners(kIc[PBody], c2), "lock with a pose error: placed %d (%d)",
              li.placed, n);
        printf("  re-lock %d with a 1.5 deg / 1 cm pose error, from the stored cab pose: body corners moved %.3g cm (placed %d)\n", rep + 1,
               MaxCornerDiff(c2, c1), li.placed);
        CHECK(MaxCornerDiff(c2, c1) < 1e-9, "the stored-pose lock moved the box by %.3g cm", MaxCornerDiff(c2, c1));
        double cd1[4][4], cd2[4][4];
        CHECK(EntryCorners(kIc[PDash], cd2), "the dashboard is not an entry");
        (void)cd1;
    }
    // 3. the same pose error without a stored pose (the shared mode / first time): the box moves -- what the stored pose fixes
    g_haveStored = false;
    Reset("test: no stored pose");
    n = RunUntilLock(g, ring, f, 300, GetStats().locks, ErrConst);
    double c3[4][4] = {};
    CHECK(n > 0 && GetLockInfo().placed == kPlacedPlay && EntryCorners(kIc[PBody], c3), "lock 3 failed");
    printf("  the same pose error from the play space: moved %.2f cm (51d: only the 1-cm shift -- the box's turn comes from the cab frame)\n",
           MaxCornerDiff(c3, c1));
    CHECK(MaxCornerDiff(c3, c1) > 0.5 && MaxCornerDiff(c3, c1) < 1.0 + 1e-6, "the pose error moved the play-space lock by %.3f cm (want the 1-cm "
          "shift only: 51d levels the turn away)", MaxCornerDiff(c3, c1));
    // 4. a stored pose whose part is missing (another LOD): the play-space path, cabMissing
    g_stored.ic = 77777; g_stored.nx = 0; g_haveStored = true;   // (51c: part 0 only)
    g_errDx = 0.0;
    Reset("test: part missing");
    n = RunUntilLock(g, ring, f, 300, GetStats().locks, ErrNone);
    CHECK(n > 0 && GetLockInfo().placed == kPlacedPlay && GetLockInfo().cabMissing && GetLockInfo().missingIc == 77777, "missing part: placed %d",
          GetLockInfo().placed);
    // PlaceFromCab (the profiles switched on with a lock held) with the real part: placed from it
    g_stored = cp;
    CHECK(PlaceFromCab(cp) && GetLockInfo().placed == kPlacedCab, "PlaceFromCab failed");
    double c4[4][4];
    CHECK(EntryCorners(kIc[PBody], c4) && MaxCornerDiff(c4, c1) < 1e-9, "PlaceFromCab corners off by %.3g", MaxCornerDiff(c4, c1));
    // 5. averaged lock-on (12 readbacks) with a +-0.5-deg jitter and an 8-deg glitch: the glitch dropped, close to the truth
    g_haveStored = false;
    SetLockSamples(kLockSamplesDefault);
    Reset("test: averaged");
    n = RunUntilLock(g, ring, f, 600, GetStats().locks, ErrJitter);
    double c5[4][4] = {};
    const LockInfo li = GetLockInfo();
    CHECK(n > 0 && li.placed == kPlacedPlayAvg && EntryCorners(kIc[PBody], c5), "averaged lock: placed %d (%d)", li.placed, n);
    printf("  averaged lock after %d frames: %d readbacks, %d kept, spread %.1f mm; off the truth by %.2f cm (one 0.5 deg frame: ~0.8 cm)\n",
           n, li.samples, li.samplesUsed, li.spreadMm, MaxCornerDiff(c5, truth));
    CHECK(li.samples == kLockSamplesDefault, "%d readbacks averaged (want %d)", li.samples, kLockSamplesDefault);
    CHECK(MaxCornerDiff(c5, truth) < 0.4, "the averaged lock is %.2f cm off the truth", MaxCornerDiff(c5, truth));
    // ---- phase 51c: the ATS 14:38 truck -- every part its own transform, the STEERING WHEEL the biggest (the reference) ----
    SetLockSamples(1);
    g_ownTransforms = true;
    g_haveStored = false;
    g_wheelA = 0.12;                                      // the wheel turned 7 deg at the first lock-on
    Reset("test 51c: first lock, wheel turned");
    n = RunUntilLock(g, ring, f, 300, GetStats().locks, ErrNone);
    FpsProfile::CabPose cpw;
    double b1[4][4] = {}, d1[4][4] = {};
    CHECK(n > 0 && GetStats().refIc == kIc[PWheel] && CurrentCabPose(&cpw) && EntryCorners(kIc[PBody], b1) && EntryCorners(kIc[PDash], d1),
          "51c lock 1 (ref %u)", GetStats().refIc);
    printf("  51c: first lock on the wheel (ref IndexCount %u): the stored pose holds %d part(s): %u%s%u%s%u\n", GetStats().refIc, cpw.Parts(),
           cpw.Part(0).ic, cpw.Parts() > 1 ? ", " : "", cpw.Parts() > 1 ? cpw.Part(1).ic : 0u, cpw.Parts() > 2 ? ", " : "", cpw.Parts() > 2 ? cpw.Part(2).ic : 0u);
    CHECK(cpw.Parts() == 3, "the stored pose holds %d parts (want 3: wheel, body, dash / console)", cpw.Parts());
    {                                                     // (through the line: 3 parts round-trip)
        FpsProfile::Vals v;
        uint64_t k = 0;
        bool hc = false;
        const std::string ln = FpsProfile::FormatValue(0x47a81f298a9ee69eull, v, &cpw);
        CHECK(FpsProfile::ParseValue(ln.c_str(), &k, &v, &g_stored, &hc) && hc && g_stored == cpw && ln.size() < 1800,
              "the 3-part cab pose did not round-trip (%zu chars)", ln.size());
    }
    g_haveStored = true;
    // later lock-on with the wheel straight: the body + dash agree, the wheel does not -> placed from the majority, the box exactly
    // where it was in the cab (no tilt)
    g_wheelA = 0.0;
    Reset("test 51c: the wheel straight");
    n = RunUntilLock(g, ring, f, 300, GetStats().locks, ErrConst);
    {
        const LockInfo li2 = GetLockInfo();
        double b2[4][4] = {};
        CHECK(n > 0 && li2.placed == kPlacedCab && li2.cabFound == 3 && li2.cabAgree == 2 && li2.cabIc != kIc[PWheel] && EntryCorners(kIc[PBody], b2),
              "51c majority: placed %d found %d agree %d ic %u", li2.placed, li2.cabFound, li2.cabAgree, li2.cabIc);
        printf("  51c: re-lock with the wheel straight + a pose error: %d of %d stored parts agree, placed from IndexCount %u, body corners moved %.2g cm\n",
               li2.cabAgree, li2.cabFound, li2.cabIc, MaxCornerDiff(b2, b1));
        CHECK(MaxCornerDiff(b2, b1) < 1e-3, "the majority placement moved the box %.3g cm (the stored 3 x 4 drops T's float-noise projective row: ~1e-5 cm)", MaxCornerDiff(b2, b1));
        // the wheel-only pose (what 51b stored for the ATS truck): it carries the wheel's old turn into the box -> tilted
        FpsProfile::CabPose only = cpw;
        only.nx = 0;
        if (only.ic != kIc[PWheel]) { for (int i = 0; i < cpw.Parts(); ++i) if (cpw.Part(i).ic == kIc[PWheel]) only.SetPart(0, cpw.Part(i)); }
        CHECK(PlaceFromCab(only), "PlaceFromCab (wheel only)");
        double b3[4][4] = {};
        EntryCorners(kIc[PBody], b3);
        printf("  51c: the same lock from a WHEEL-ONLY pose (51b): body corners off by %.2f cm (the wheel's 7-deg turn baked in)\n", MaxCornerDiff(b3, b1));
        CHECK(MaxCornerDiff(b3, b1) > 1.0, "the wheel-only pose should be off (%.3f cm)", MaxCornerDiff(b3, b1));
        // the wheel turns while attached: the monitor finds it moving -> CabMovedSerial names it (the profile drops it)
        FpsProfile::CabPart mp;
        const uint64_t ms0 = CabMovedSerial(&mp);
        static double (*turn)(int) = [](int k) { return 0.4 * std::sin(k * 0.15); };
        g_wheelAt = turn;
        RunUntilLock(g, ring, f, 80, ~0ull, ErrNone);
        g_wheelAt = nullptr;
        const uint64_t ms1 = CabMovedSerial(&mp);
        CHECK(ms1 > ms0 && mp.ic == kIc[PWheel], "the moving stored wheel part was not reported (serial %llu -> %llu, ic %u)",
              (unsigned long long)ms0, (unsigned long long)ms1, mp.ic);
        FpsProfile::Store st;
        st.Put(1, FpsProfile::Vals());
        st.PutCab(1, cpw);
        CHECK(st.DropCabPart(1, mp.ic, mp.stride, mp.fmt) && st.Cab(1) && st.Cab(1)->Parts() == 2, "DropCabPart (3 -> 2)");
        FpsProfile::CabPose one = only;
        st.PutCab(1, one);
        CHECK(st.DropCabPart(1, mp.ic, mp.stride, mp.fmt) && !st.Cab(1), "DropCabPart of the last part must clear the pose");
        // the next lock-on: the wheel is remembered as moving (by geometry) -> never the reference again
        g_wheelA = 0.0;
        g_haveStored = false;
        Reset("test 51c: after the wheel moved");
        n = RunUntilLock(g, ring, f, 300, GetStats().locks, ErrNone);
        CHECK(n > 0 && GetStats().refIc != kIc[PWheel], "the wheel was taken as the reference again (ref %u)", GetStats().refIc);
    }
    // phase 51c: Remeasure -- the stored pose is ignored, a fresh map taken (remeasured), the settings kept
    g_stored = cpw; g_haveStored = true;
    Remeasure();
    n = RunUntilLock(g, ring, f, 300, GetStats().locks, ErrNone);
    CHECK(n > 0 && GetLockInfo().remeasured && GetLockInfo().placed != kPlacedCab, "Remeasure: placed %d remeasured %d",
          GetLockInfo().placed, (int)GetLockInfo().remeasured);
    // phase 51c: a BROKEN map (both eyes at one spot: IPD ratio ~0) is never locked on
    g_haveStored = false;
    g_collapseUntil = 60;
    Reset("test 51c: broken maps first");
    n = RunUntilLock(g, ring, f, 400, GetStats().locks, ErrNone);
    g_collapseUntil = 0;
    double b6[4][4] = {};
    CHECK(n > 60 && EntryCorners(kIc[PBody], b6) && MaxCornerDiff(b6, b1) < 1e-3, "the lock used a broken map (after %d frames, off %.3f cm)", n,
          MaxCornerDiff(b6, b1));
    printf("  51c: with 60 frames of broken maps the lock waited %d frames; body corners off by %.2g cm\n", n, MaxCornerDiff(b6, b1));
    g_ownTransforms = false;
    g_wheelA = 0.0;
    SetActive(false, 0);
    SetCabPoseProvider(nullptr);
    SetLockSamples(kLockSamplesDefault);
    Shutdown();
    g_noSway = false;
    return 0;
}

// ---- Part D (phase 51d): the box SQUARE TO THE CAB, the moving stored part dropped, Reset never reaching a profile ------------
static M4 TiltM(double yawDeg, double pitchDeg, double rollDeg) {   // play -> cab rotation (the play space turned in the cab)
    const double k = 3.14159265358979 / 180.0;
    return Mul(Rax(1, yawDeg * k), Mul(Rax(0, pitchDeg * k), Rax(2, rollDeg * k)));
}
static double RotAngleDeg(const M4& R) {
    const double c = std::fmax(-1.0, std::fmin(1.0, 0.5 * (R.m[0][0] + R.m[1][1] + R.m[2][2] - 1.0)));
    return std::acos(c) * 180.0 / 3.14159265358979;
}
// the lock's maths for the parked cab at t (g_noSway): A = play -> body model, S = the cab frame (the camera-relative overlay: the
// cab's axes) in the body model; level = A T_Q. Out: the box corners in CAB space, levelled and not.
static bool CabCornersOf(const Settings& s, double t, double lev[4][3], double raw[4][3], double* turnDeg, int* upAxis, bool bodyFrame) {
    EyeFrame eb[2], eo;
    Pose ps[2];
    for (int e = 0; e < 2; ++e) { if (!EyeFrameOf(Mvp(PBody, t, 0.0, e), &eb[e])) return false; ps[e] = Eye(t, e); }
    if (!EyeFrameOf(Mvp(POverlay, t, 0.0, 0), &eo)) return false;
    M4 A;
    if (!PlayToModel(eb, ps, 2, &A, nullptr, nullptr, nullptr)) return false;
    double S[3][3], Q[3][3];
    FrameRot(eb[0], bodyFrame ? eb[0] : eo, S);
    if (!CabLevel(A, S, Q, turnDeg, upAxis)) return false;
    const double pivot[3] = { 0.0, 0.0, 0.0 };
    const M4 Aq = Mul(A, LevelT(Q, pivot));
    LockFrame lf;
    lf.fw = kTan[0][1] - kTan[0][0];
    double cp[4][3], cc[3];
    BoxPlay(s, lf, cp, cc);
    const M4 toCab = PartModel(PBody, t, 0.0);
    for (int k = 0; k < 4; ++k) {
        const double h[4] = { cp[k][0], cp[k][1], cp[k][2], 1.0 };
        double m[4], c[4];
        Apply(Aq, h, m); Apply(toCab, m, c);
        for (int i = 0; i < 3; ++i) lev[k][i] = c[i] / c[3];
        Apply(A, h, m); Apply(toCab, m, c);
        for (int i = 0; i < 3; ++i) raw[k][i] = c[i] / c[3];
    }
    return true;
}
static double MaxDiff3(const double a[4][3], const double b[4][3]) {
    double m = 0.0;
    for (int k = 0; k < 4; ++k) for (int i = 0; i < 3; ++i) m = std::fmax(m, std::fabs(a[k][i] - b[k][i]));
    return m;
}
static void CabToBody(const double in[4][3], double out[4][4]) {   // cab space (m) -> the body's model space (cm), homogeneous
    const M4 inv = InvM(PartModel(PBody, 0.0, 0.0));
    for (int k = 0; k < 4; ++k) { const double h[4] = { in[k][0], in[k][1], in[k][2], 1.0 }; Apply(inv, h, out[k]); }
}
static int PartD() {
    printf("Part D (phase 51d): the box square to the cab; moving stored part dropped; Reset keeps the profiles\n");
    const bool sway0 = g_noSway;
    g_noSway = true;
    // 1. a cab YAWED and ROLLED against the play space (the ATS 15:02 truck: 10.4 deg roll, 4.6 pitch; + a 15-deg recentre yaw)
    const M4 tiltA = TiltM(15.0, 4.6, 10.4), tiltB = TiltM(-20.0, -6.0, 3.0);
    const double angA = RotAngleDeg(tiltA), angB = RotAngleDeg(tiltB);
    {
        g_cabTilt = tiltA;
        Settings s0;                                      // zero turn, straight ahead: the box must face the seat along the cab's -z
        s0.y = 0.0; s0.distCm = 80;
        double lev[4][3], raw[4][3], want[4][3];
        double deg = 0.0;
        int up = -1;
        CHECK(CabCornersOf(s0, 0.3, lev, raw, &deg, &up, false), "the cab-level maths failed");
        BoxCabTruth(s0, 0.3, want, nullptr);
        const double e[3][3] = { { lev[1][0] - lev[0][0], lev[1][1] - lev[0][1], lev[1][2] - lev[0][2] },     // TL -> TR
                                 { lev[0][0] - lev[2][0], lev[0][1] - lev[2][1], lev[0][2] - lev[2][2] },     // BL -> TL
                                 { 0, 0, 0 } };
        double cen[3];
        for (int i = 0; i < 3; ++i) cen[i] = 0.25 * (lev[0][i] + lev[1][i] + lev[2][i] + lev[3][i]);
        printf("  a cab turned %.1f deg (yaw 15, pitch 4.6, roll 10.4) against the play space: levelled %.2f deg (up = cab axis %d); zero-turn "
               "box vs the cab-square truth %.2g m, the play-built box %.3f m off\n", angA, deg, up, MaxDiff3(lev, want), MaxDiff3(raw, want));
        CHECK(std::fabs(deg - angA) < 1e-6 && up == 1, "levelled %.4f deg (want %.4f), up axis %d", deg, angA, up);
        CHECK(MaxDiff3(lev, want) < 1e-9, "the levelled zero-turn box is %.3g m off the cab-square truth", MaxDiff3(lev, want));
        CHECK(std::fabs(e[0][1]) < 1e-12 && std::fabs(e[0][2]) < 1e-12 && e[0][0] > 0.0, "the top edge is not along the cab's x");
        CHECK(std::fabs(e[1][0]) < 1e-12 && std::fabs(e[1][2]) < 1e-12 && e[1][1] > 0.0, "the side edge is not along the cab's y (upright)");
        CHECK(std::fabs(cen[0] - 0.45) < 1e-9 && std::fabs(cen[1] - 1.25) < 1e-9 && std::fabs(cen[2] - (0.35 - 0.8)) < 1e-9,
              "the box is not 0.8 m straight ahead of the seat along the cab's -z (%.4f %.4f %.4f)", cen[0], cen[1], cen[2]);
        CHECK(MaxDiff3(raw, want) > 0.05, "the play-built box should be turned against the cab (%.3f m)", MaxDiff3(raw, want));
        double lev2[4][3], raw2[4][3], deg2 = 0.0;
        int up2 = -1;                                     // the same frame from the body's own axes (z up, x forward: the fallback snap)
        CHECK(CabCornersOf(s0, 0.3, lev2, raw2, &deg2, &up2, true) && up2 == 2 && MaxDiff3(lev2, lev) < 1e-9,
              "the body-axes cab frame (up axis %d) gives another box (%.3g m)", up2, MaxDiff3(lev2, lev));
        // CabTilt (the log's figures) of a pure 10-deg roll: sideways 10, forward 0, yaw 0
        g_cabTilt = TiltM(0.0, 0.0, 10.0);
        EyeFrame eb[2], eo; Pose ps[2]; M4 A;
        for (int k = 0; k < 2; ++k) { EyeFrameOf(Mvp(PBody, 0.3, 0.0, k), &eb[k]); ps[k] = Eye(0.3, k); }
        EyeFrameOf(Mvp(POverlay, 0.3, 0.0, 0), &eo);
        PlayToModel(eb, ps, 2, &A, nullptr, nullptr, nullptr);
        double S[3][3], Q[3][3], sd, fd, yd;
        FrameRot(eb[0], eo, S);
        CabLevel(A, S, Q);
        CabTilt(Q, &sd, &fd, &yd);
        CHECK(std::fabs(std::fabs(sd) - 10.0) < 1e-6 && std::fabs(fd) < 1e-6 && std::fabs(yd) < 1e-6, "CabTilt of a 10-deg roll: %.3f %.3f %.3f", sd, fd, yd);
        // a head-locked draw (bare projection) is never a cab frame
        EyeFrame eh;
        CHECK(EyeFrameOf(Proj(kTan[0][0], kTan[0][1], kTan[0][2], kTan[0][3]), &eh) && HeadLocked(eh) && !HeadLocked(eo), "HeadLocked");
    }
    // 2. the same settings in two cabs with different play alignments = the same spot relative to each cab
    {
        Settings s;
        s.x = 8.0; s.y = 26.2; s.distCm = 95; s.scale = 0.6;
        double la[4][3], ra[4][3], lb[4][3], rb[4][3], da = 0, db = 0;
        g_cabTilt = tiltA;
        const bool okA = CabCornersOf(s, 0.3, la, ra, &da, nullptr, false);
        g_cabTilt = tiltB;
        const bool okB = CabCornersOf(s, 0.7, lb, rb, &db, nullptr, false);
        printf("  two cabs turned %.1f / %.1f deg against the play space, x 8 y 26.2 dist 95: levelled boxes %.2g m apart in cab space, "
               "play-built %.3f m\n", angA, angB, MaxDiff3(la, lb), MaxDiff3(ra, rb));
        CHECK(okA && okB && MaxDiff3(la, lb) < 1e-9, "the same settings land on different cab spots (%.3g m)", MaxDiff3(la, lb));
        CHECK(MaxDiff3(ra, rb) > 0.05, "the play-built boxes should differ (%.3f m)", MaxDiff3(ra, rb));
    }
    // 3. the runtime (WARP): a lock-on in the tilted cab is square to it; an OLDER play-space pose is levelled once, then stays
    Gpu g;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx)) ||
        FAILED(g.ctx.As(&g.ctx1))) { printf("  WARP device failed\n"); return 1; }
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 65536; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g.dev->CreateBuffer(&bd, nullptr, &g.cb);
        std::vector<uint16_t> idx(20000, 0);
        D3D11_BUFFER_DESC ib{};
        ib.ByteWidth = (UINT)(idx.size() * 2); ib.Usage = D3D11_USAGE_DEFAULT; ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{ idx.data(), 0, 0 };
        g.dev->CreateBuffer(&ib, &sd, &g.ib[0]);
    }
    DrawIdRecord ring[4];
    g_cabTilt = tiltA;
    g_ownTransforms = false;
    g_steerFrom = 1e9;
    g_wheelA = 0.0;
    Settings set;
    set.x = 8.0; set.y = 26.2; set.distCm = 95; set.scale = 0.6;
    SetMonitorEvery(12);
    SetCabPoseProvider(Provider);
    g_haveStored = false;
    SetLockSamples(1);
    SetActive(true, 0);
    SetSettings(set);
    int f = 0;
    double want[4][4];
    {
        double wc[4][3];
        BoxCabTruth(set, 0.0, wc, nullptr);
        CabToBody(wc, want);
    }
    int n = RunUntilLock(g, ring, f, 300, GetStats().locks, ErrNone);
    FpsProfile::CabPose cp;
    double c1[4][4] = {};
    LockInfo li = GetLockInfo();
    CHECK(n > 0 && li.placed == kPlacedPlay && li.level && CurrentCabPose(&cp) && cp.level && EntryCorners(kIc[PBody], c1),
          "51d lock 1 in the tilted cab: placed %d level %d", li.placed, (int)li.level);
    printf("  runtime lock in the tilted cab: levelled %.2f deg (cab frame from %d draws, IndexCount sum %llu), body corners off the cab-square "
           "truth by %.2g cm; the cab leans %+.1f sideways / %+.1f forward, turned %+.1f\n", li.turnDeg, li.cabFrameDraws,
           (unsigned long long)li.cabFrameSum, MaxCornerDiff(c1, want), li.tiltSide, li.tiltFore, li.tiltYaw);
    CHECK(std::fabs(li.turnDeg - angA) < 1e-3, "the runtime levelled %.4f deg (want %.4f)", li.turnDeg, angA);
    CHECK(MaxCornerDiff(c1, want) < 1e-4, "the runtime box is %.3g cm off the cab-square truth", MaxCornerDiff(c1, want));
    // an OLDER pose of the same cab, built in the play space of its lock (what every pre-51d line holds): levelled ONCE at the next lock
    FpsProfile::CabPose old = cp;
    {
        EyeFrame eb[2];
        Pose ps[2];
        for (int e = 0; e < 2; ++e) { EyeFrameOf(Mvp(PBody, 0.0, 0.0, e), &eb[e]); ps[e] = Eye(0.0, e); }
        M4 A;
        PlayToModel(eb, ps, 2, &A, nullptr, nullptr, nullptr);
        CabPoseSet(A, CabFrame(cp), cp.ic, cp.stride, cp.fmt, &old);
        old.yaw0 = 0.0; old.level = false;
        uint64_t k = 0;
        FpsProfile::Vals v;
        bool hc = false;
        FpsProfile::CabPose rd;
        const std::string ln = FpsProfile::FormatValue(0x47a81f298a9ee69eull, v, &old);
        CHECK(ln.find(" cab ") != std::string::npos && FpsProfile::ParseValue(ln.c_str(), &k, &v, &rd, &hc) && hc && !rd.level && rd == old,
              "an older 'cab' line does not load as a play-space pose");
    }
    g_stored = old; g_haveStored = true;
    Reset("test 51d: an older pose");
    n = RunUntilLock(g, ring, f, 300, GetStats().locks, ErrConst);
    li = GetLockInfo();
    FpsProfile::CabPose lv;
    double c2[4][4] = {};
    CHECK(n > 0 && li.placed == kPlacedCab && li.leveledOld && li.level && CurrentCabPose(&lv) && lv.level && lv != old &&
          EntryCorners(kIc[PBody], c2), "the older pose was not levelled: placed %d leveledOld %d", li.placed, (int)li.leveledOld);
    printf("  an older play-space pose at the next lock-on (a 1.5-deg pose error): levelled %.2f deg once, body corners vs the cab-square truth "
           "%.2g cm\n", li.turnDeg, MaxCornerDiff(c2, want));
    CHECK(std::fabs(li.turnDeg - angA) < 1e-3 && MaxCornerDiff(c2, want) < 1e-4, "the levelled older pose: %.4f deg, %.3g cm", li.turnDeg,
          MaxCornerDiff(c2, want));
    {                                                     // the levelled pose goes into the line as "cabsq" and reads back as square
        uint64_t k = 0;
        FpsProfile::Vals v;
        bool hc = false;
        FpsProfile::CabPose rd;
        const std::string ln = FpsProfile::FormatValue(0x47a81f298a9ee69eull, v, &lv);
        CHECK(ln.find(" cabsq ") != std::string::npos && FpsProfile::ParseValue(ln.c_str(), &k, &v, &rd, &hc) && hc && rd.level && rd == lv,
              "the levelled pose does not round-trip as cabsq: %s", ln.c_str());
    }
    g_stored = lv;
    Reset("test 51d: the levelled pose");
    n = RunUntilLock(g, ring, f, 300, GetStats().locks, ErrConst);
    li = GetLockInfo();
    double c3[4][4] = {};
    CHECK(n > 0 && li.placed == kPlacedCab && !li.leveledOld && li.turnDeg < 1e-6 && EntryCorners(kIc[PBody], c3) && MaxCornerDiff(c3, c2) < 1e-9,
          "the square pose moved (leveledOld %d, residual %.3g deg, %.3g cm)", (int)li.leveledOld, li.turnDeg, MaxCornerDiff(c3, c2));
    // 4. the ATS 47a8 line: a lock placed from the PLAY SPACE on the steering wheel stores a pose with the wheel; the monitor then finds
    //    it moving -> reported (51c reported only stored-pose locks) and dropped from the stored pose; a later lock-on from a stored pose
    //    that still names the remembered wheel reports it again
    g_ownTransforms = true;
    g_haveStored = false;
    g_wheelA = 0.12;
    Reset("test 51d: wheel first");
    n = RunUntilLock(g, ring, f, 300, GetStats().locks, ErrNone);
    FpsProfile::CabPose cw;
    CHECK(n > 0 && GetLockInfo().placed == kPlacedPlay && CurrentCabPose(&cw) && cw.Parts() == 3, "51d wheel lock: placed %d parts %d",
          GetLockInfo().placed, cw.Parts());
    bool wheelIn = false;
    for (int i = 0; i < cw.Parts(); ++i) if (cw.Part(i).ic == kIc[PWheel]) wheelIn = true;
    CHECK(wheelIn, "the stored pose has no wheel part (the scene: the wheel is the reference)");
    FpsProfile::Store st;
    const uint64_t key = 0x47a81f298a9ee69eull;
    st.Put(key, FpsProfile::Vals());
    st.PutCab(key, cw);
    uint64_t ser = 0;
    {
        FpsProfile::CabPart tmp[16];
        CabMovedSince(&ser, tmp, 16);                     // (skip the reports of the earlier parts)
        ser = CabMovedSerial(nullptr);
    }
    static double (*turn)(int) = [](int k) { return 0.12 + 0.4 * std::sin(k * 0.15); };
    g_wheelAt = turn;
    RunUntilLock(g, ring, f, 80, ~0ull, ErrNone);
    g_wheelAt = nullptr;
    FpsProfile::CabPart mp[16];
    int nm = CabMovedSince(&ser, mp, 16);
    bool rep = false;
    for (int m = 0; m < nm; ++m) if (mp[m].ic == kIc[PWheel]) { rep = true; st.DropCabPart(key, mp[m].ic, mp[m].stride, mp[m].fmt); }
    const FpsProfile::CabPose* left = st.Cab(key);
    bool stillWheel = false;
    if (left) for (int i = 0; i < left->Parts(); ++i) if (left->Part(i).ic == kIc[PWheel]) stillWheel = true;
    const std::string ln = left ? FpsProfile::FormatValue(key, *st.Get(key), left) : std::string();
    printf("  a play-space lock on the wheel: the turning wheel reported %s, the stored pose keeps %d part(s), the line %s the wheel\n",
           rep ? "yes" : "NO", left ? left->Parts() : 0, stillWheel ? "STILL has" : "no longer has");
    CHECK(rep && left && left->Parts() == 2 && !stillWheel, "the moving wheel was not dropped from a pose stored at a play-space lock");
    // the user's line as it is (the wheel first): a stored-pose lock-on skips the remembered wheel and reports it
    g_stored = cw; g_haveStored = true;
    g_wheelA = 0.0;
    Reset("test 51d: stored wheel");
    n = RunUntilLock(g, ring, f, 300, GetStats().locks, ErrNone);
    nm = CabMovedSince(&ser, mp, 16);
    rep = false;
    for (int m = 0; m < nm; ++m) if (mp[m].ic == kIc[PWheel]) rep = true;
    CHECK(n > 0 && GetLockInfo().placed == kPlacedCab && GetLockInfo().cabIc != kIc[PWheel] && rep,
          "a stored pose naming the remembered wheel: placed %d from %u, reported %d", GetLockInfo().placed, GetLockInfo().cabIc, (int)rep);
    g_ownTransforms = false;
    g_wheelA = 0.0;
    g_haveStored = false;
    SetActive(false, 0);
    SetCabPoseProvider(nullptr);
    SetLockSamples(kLockSamplesDefault);
    Shutdown();
    g_cabTilt = Identity();
    g_noSway = sway0;
    // 5. the Reset row never reaches a vehicle's profile (FpsProfile::Session: the inject.cpp FpsProfileTick logic)
    {
        using namespace FpsProfile;
        Store ps;
        Session ss;
        Vals def;                                         // the defaults (the Reset row's fps values)
        def.color = 0; def.bright = FpsPose::kBrightDefault; def.text = 0;
        Vals mine;
        mine.x = 8.0f; mine.y = 26.2f; mine.dist = 95; mine.scale = 0.6f; mine.color = 4; mine.bright = 40; mine.text = 4;
        const uint64_t K = 0x47a81f298a9ee69eull, K2 = 0x7b8a0fae6ab01548ull;
        ps.Put(K, mine);
        Vals other = mine; other.x = 0.2f; other.y = 14.4f; other.dist = 90;
        ps.Put(K2, other);
        Session::Out o = ss.Tick(ps, true, true, true, 1, K, false, def);   // lock-on: the vehicle's profile loaded
        CHECK(o.lock == Session::kLockLoaded && o.applyIt && o.apply == mine && ss.nBound == 1, "the lock-on did not load the profile");
        Vals live = mine;
        o = ss.Tick(ps, true, true, true, 1, K, false, live);
        CHECK(o.nChanged == 0, "an unchanged tick changed a profile");
        live.x = 9.0f;                                     // a menu edit
        o = ss.Tick(ps, true, true, true, 1, K, false, live);
        CHECK(o.nChanged == 1 && o.changed[0] == K && ps.Get(K)->x == 9.0f, "the edit did not reach the bound profile");
        const Vals before = *ps.Get(K);
        // the Reset row: the defaults + mode 0 in the live settings; FpsCab is still active this Present (it syncs one Present later)
        o = ss.Tick(ps, false, true, true, 1, K, false, def);
        CHECK(o.nChanged == 0 && *ps.Get(K) == before && ss.nBound == 0, "the Reset row changed the bound vehicle's profile");
        o = ss.Tick(ps, false, false, true, 1, K, false, def);
        CHECK(o.nChanged == 0 && *ps.Get(K) == before, "the Present after the Reset changed the profile");
        ss.Unbind();                                       // (inject.cpp FpsProfUnbind: the Reset row) -> the next lock-on loads
        // mode 2 again (the user turns the attached box back on): a new lock-on -> the vehicle's own values return
        o = ss.Tick(ps, true, true, true, 2, K, false, def);
        CHECK(o.lock == Session::kLockLoaded && o.applyIt && o.apply == before && *ps.Get(K) == before && *ps.Get(K2) == other,
              "after the Reset the vehicle's values did not come back (x %.1f)", (double)o.apply.x);
        // what phase 51c did (the cab state alone gated the binding): the Reset Present stored the defaults
        Store bug = ps;
        Session sb = ss;
        sb.Tick(bug, true, true, true, 2, K, false, before);
        sb.Tick(bug, true, true, true, 2, K, false, def);
        CHECK(*bug.Get(K) == def, "the old gate should have stored the defaults (the bug reproduction)");
        // the menu's Left on "Re-measure cab spot": THIS vehicle's box to the defaults (an edit), the other vehicle untouched
        o = ss.Tick(ps, true, true, true, 2, K, false, before);
        o = ss.Tick(ps, true, true, true, 2, K, false, def);
        CHECK(o.nChanged == 1 && *ps.Get(K) == def && *ps.Get(K2) == other, "box defaults: K %d, K2 untouched %d", (int)(*ps.Get(K) == def),
              (int)(*ps.Get(K2) == other));
        // the shared mode / mode 1 never bind
        Session s3;
        o = s3.Tick(ps, true, true, false, 5, K, false, def);
        CHECK(o.lock == Session::kLockNone && s3.nBound == 0, "the shared mode bound a vehicle");
        // phase 51e: the FPS row cycling through Off re-binds the same vehicle -> the live values are kept and stored
        {
            Store p2; Session s5;
            Vals a = mine, b = other;
            p2.Put(K, a); p2.Put(K2, b);
            Vals lv = a;                                   // text 4 stored
            o = s5.Tick(p2, true, true, true, 1, K, false, lv);
            CHECK(o.lock == Session::kLockLoaded, "51e: first bind did not load");
            o = s5.Tick(p2, false, true, true, 1, K, false, lv);   // box Off
            o = s5.Tick(p2, false, false, true, 1, K, false, lv);
            lv.text = 1; lv.color = 2; lv.bright = 55;     // edited in the gap / meanwhile
            o = s5.Tick(p2, true, false, true, 1, K, false, lv);   // box On, cab not locked yet: edits unbound
            CHECK(o.nChanged == 0 && p2.Get(K)->text == 4, "51e: an unbound tick wrote a profile");
            o = s5.Tick(p2, true, true, true, 2, K, false, lv);    // re-lock on the same vehicle
            CHECK(o.lock == Session::kLockKept && !o.applyIt && p2.Get(K)->text == 1 && p2.Get(K)->color == 2 && p2.Get(K)->bright == 55 &&
                  s5.nBound == 1, "51e: the re-bind did not keep + store the live values (text %d)", p2.Get(K)->text);
            // another vehicle in between -> A's stored values load again
            o = s5.Tick(p2, true, true, true, 3, K2, false, lv);
            CHECK(o.lock == Session::kLockLoaded && o.apply == b, "51e: B did not load");
            Vals lb = *p2.Get(K2);
            o = s5.Tick(p2, true, true, true, 4, K, false, lb);
            CHECK(o.lock == Session::kLockLoaded && o.applyIt && o.apply.text == 1 && o.apply.x == a.x, "51e: A after B did not load its stored values");
            // box off, then a Reset (Unbind): loads again
            s5.Tick(p2, false, true, true, 4, K, false, def);
            s5.Unbind();
            o = s5.Tick(p2, true, true, true, 5, K, false, def);
            CHECK(o.lock == Session::kLockLoaded, "51e: after Reset the stored values did not load");
            printf("  51e: same-vehicle re-bind keeps + stores the live values; another vehicle / Reset loads\n");
        }
        printf("  Reset: the bound profile kept (x %.1f), the vehicle's values back at the next lock-on; the old gate would have stored the "
               "defaults; Left on Re-measure = this vehicle's box to defaults only\n", (double)before.x);
    }
    return 0;
}

int main() {
    PartA();
    PartP();
    PartP2();                                             // phase 51b
    TuningMenu::RegisterShaders();
    ShaderCache::Start();
    while (!ShaderCache::Done()) Sleep(10);
    if (!ShaderCache::Ready(ShaderCache::kMenuCabVs)) { printf("VSCab not compiled: %s\n", ShaderCache::Error(ShaderCache::kMenuCabVs)); return 1; }
    int r = 0;
    for (int v = 0; v < 5; ++v) r |= PartB(v);
    r |= PartC();                                         // phase 51b
    r |= PartD();                                         // phase 51d
    printf("fps_cab_test: %d checks, %d failed\n", g_checks, g_fail);
    return (r || g_fail) ? 1 : 0;
}
