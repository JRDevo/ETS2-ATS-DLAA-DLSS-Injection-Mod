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
// Build: tests\build_fps_cab_test.bat -> build\tests\fps_cab_test.exe (exit 0 = all checks passed).
#include <windows.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "fps_cab.h"
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
static M4 G(double t) {
    const M4 seat = T(0.45, 1.25, 0.35);
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
        const double a = frame >= g_steerFrom && frame < g_steerFrom + 50.0 ? 0.7 * std::sin(3.14159265 * (frame - g_steerFrom) / 50.0) : 0.0;
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
// the truth: the box built in the play space at the lock moment t0 (as FpsCab does: LOCAL pivot, eye 0's fov width), fixed in the
// cab from then on -> its clip corners in eye e at time t
static void TruthClip(const Settings& s, double t0, double t, int e, float clip[4][4]) {
    LockFrame lf;
    lf.fw = kTan[0][1] - kTan[0][0];
    double cp[4][3], cc[3];
    BoxPlay(s, lf, cp, cc);
    const M4 P = Proj(kTan[e][0], kTan[e][1], kTan[e][2], kTan[e][3]);
    const M4 M = Mul(P, Mul(View(t, e), G(t0)));
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
    printf("Part B variant %d, 2 eyes x %d frames (%s): WARP -- DrawIdRecord + FpsCab + menu.cpp VSCab\n", variant, variant == 4 ? 640 : 480,
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
    g_ownTransforms = variant == 4;
    SetActive(true, 0);
    SetSettings(set);
    const Stats s0 = GetStats();                          // (the totals run on over the variants)
    // phase 47b timeline: 0-29 eye 0 has no usable record (the lock must wait for both eyes), 30-44 the OpenXR views are in a VIEW
    // space (no lock without poses), the steering wheel turns 120-170, exterior camera 190-209, buffer-pool rotation at 230, a
    // LOADING screen 260-299 (eye 0 has no world blit, eye 1 shows other cabin-layer draws, VIEW-space views, and every 3rd frame
    // looks attachable: the decision must not flap), a truck change at 320.
    const int kFrames = variant == 4 ? 640 : 480, kResetAt = 240, kBackAt = 450, kNoEye0To = 30, kNoXrTo = 45, kExtFrom = 190, kExtTo = 210, kPoolAt = 230, kLoadFrom = 260, kLoadTo = 300,
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
        const UINT siAdd = truckB ? 100u : 0u;            // the new truck's parts: new loose keys too
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
                rec.Record(g.ctx1.Get(), kIc[p] + (loadScene ? 7u : 0u), 1, (UINT)p * 10u + siAdd + (loadScene ? 500u : 0u), 0, 0, false);
            }
            rec.Flush(g.ctx.Get());
            OnBlit(g.ctx.Get(), e, rec, serial, (uint64_t)f, k == 0, xr);
            blitted[e] = true;
        }
        if (firstLockEyes < 0 && GetStats().locks > s0.locks) { const Stats q = GetStats(); firstLockEyes = q.lockEyes; firstLockXr = q.lockXr; }
        {                                                 // phase 47d: each lock's reference, and when the wheel was found moving
            const Stats q = GetStats();
            if (q.locks != lastLocks && q.state == 2) { lastLocks = q.locks; lockRefs.push_back(q.refIc); lockAt.push_back(f); }
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
        LockFrame lf;
        lf.fw = kTan[0][1] - kTan[0][0];
        double cp[4][3], cc[3];
        BoxPlay(set, lf, cp, cc);
        const M4 G0 = G((double)st.lockFrame / kHz);
        double tmax = 0.0;
        for (int f = (int)st.lockFrame; f < kFrames; ++f) {
            const M4 back = Mul(InvM(G(f / kHz)), G0);
            double c[4];
            ToClip(back, cc, c);
            tmax = std::fmax(tmax, std::sqrt(std::pow(c[0] - cc[0], 2) + std::pow(c[1] - cc[1], 2) + std::pow(c[2] - cc[2], 2)));
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
    if (variant != 4) {                                   // 47c: the cab group shares a transform -- the wheel is never a carrier
        CHECK(switches0 <= 8, "%llu attach switches in all (flapping)", (unsigned long long)switches0);
        CHECK(locksBeforeTruck == 1, "the exterior camera / pool rotation caused a new lock (%llu locks before the truck change)",
              (unsigned long long)locksBeforeTruck);
        CHECK(st.relocks == 1 && st.locks == 2, "truck change: relocks %llu locks %llu (want 1 / 2)", (unsigned long long)st.relocks,
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

int main() {
    PartA();
    TuningMenu::RegisterShaders();
    ShaderCache::Start();
    while (!ShaderCache::Done()) Sleep(10);
    if (!ShaderCache::Ready(ShaderCache::kMenuCabVs)) { printf("VSCab not compiled: %s\n", ShaderCache::Error(ShaderCache::kMenuCabVs)); return 1; }
    int r = 0;
    for (int v = 0; v < 5; ++v) r |= PartB(v);
    printf("fps_cab_test: %d checks, %d failed\n", g_checks, g_fail);
    return (r || g_fail) ? 1 : 0;
}
