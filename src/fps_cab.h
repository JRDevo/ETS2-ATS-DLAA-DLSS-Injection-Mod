// FpsCab -- phase 47: the VR fps box ATTACHED TO THE VEHICLE (dlaa.ini fps_vr_lock = 2, the menu's FPS counter lock row
// "Attached to vehicle"). Design: docs/DEVELOPMENT.md "Phase 47".
//
// Why it is not done in the play space: OpenXR knows the head in the play space (the seat), not the cab. The cab's sway
// (braking pitch, bumps, hills, the game's head / seat physics) exists only in the rendered pictures: the game draws its cab
// interior as a CABIN LAYER (viewport depth [0.9, 1]) whose MVPs carry eye-in-cab = seat * sway(t) * head(t). A CPU copy of
// that motion is 2-3 frames late (phase 44: worse than nothing above ~4 Hz). So the box is drawn INTO each eye picture and
// placed ON THE GPU from a cab draw's CURRENT MVP:
//   1. LOCK (once, CPU, a lagged readback is fine: it is the picture of ONE moment t0): the biggest cabin-layer draws of both
//      eyes' draw records (DrawIdRecord: every G-buffer draw's cb0 rows mirrored GPU-side) are copied to a staging buffer at
//      the blit of frame t0, together with the eyes' OpenXR poses of t0. From a draw's MVP M (column vectors, clip = M p,
//      rows = clip x, y, z, w) the eye's frame in that draw's MODEL space follows exactly (EyeFrameOf): the camera centre C
//      (rows x, y, w of M vanish there), right / up / forward directions and the game metres per model unit. With the eye's
//      play-space pose that gives the affine map play space -> model space AT t0 (PlayToModel). The box is built in the play
//      space exactly as mode 1 / the quad layer build it (BoxPlay: fps_vr_x / fps_vr_y degrees at fps_vr_dist around the seat,
//      facing it, its own turn) and mapped into the model space: four model-space corners a_k -- the box is now a sticker in
//      the cab. Every other usable cab draw c gets the same corners in ITS model space: a_c = M_c(t0)^-1 M_ref(t0) a_ref
//      (= model_c^-1 model_ref: exact, the view / projection cancel), so any of them can carry the box later (re-acquire).
//      Phase 47e: a part is also found again by its IndexCount alone (one cab draw with it) when the game changes its buffers / shader
//      (keys), and a re-lock on the same cab (cab change / every carrier moving) keeps the box's spot (A carried over via a shared IndexCount).
//   2. DRAW (every frame, every eye, ZERO lag): a vertex shader (menu.cpp kMenuVs / VSCab) reads the reference draw's MVP of
//      THIS eye's pass from the record's GPU mirror (byte offset known on the CPU at the blit) and outputs clip = M(t) a_k --
//      the box moves with the cab in the very picture the cab is in, and the runtime's reprojection treats it like the
//      dashboard. No depth test (the eye picture has no depth; the box is drawn over the finished picture as the other drawn-in
//      paths).
//   3. MONITOR (CPU, lagged, every ~0.5 s): the parts' predicted box centres M_c(t) a_c must agree (a rigid cab); a part that
//      disagrees with the largest agreeing group (by summed IndexCount: the cab shell, dashboard, seats) is a MOVING part (the
//      steering wheel turned, a door, a wiper) and is never used again for this lock. The same readback measures how far the
//      cab moved against the play space since the lock (log level 4, per 10 s).
//      (Phase 47e: the lock's entries are matched by full key, else loose key, else the unique IndexCount.)
// The maths below is pure (no D3D, double precision; tests/fps_cab_test.cpp); the runtime (fps_cab.cpp, WITH_DLAA) owns the
// candidate scan, the staging readbacks, the state and the logs. inject.cpp calls OnBlit / Get, menu.cpp draws.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include "fps_pose.h"
#include "fps_profile.h"                                // phase 51b: FpsProfile::CabPose

namespace FpsCab {

// ---- 4x4 matrices: row r = clip component r (x, y, z, w); clip = M p (column vectors, the draw_ids.cpp convention) -------
struct M4 { double m[4][4]; };
inline M4 Identity() { M4 r{}; for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0; return r; }
inline M4 FromRows(const float* r) {                    // 16 floats, row after row (the cbuffer layout of cb0 rows)
    M4 o;
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) o.m[i][j] = (double)r[i * 4 + j];
    return o;
}
inline M4 Mul(const M4& a, const M4& b) {
    M4 o{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            o.m[i][j] = s;
        }
    return o;
}
inline void Apply(const M4& a, const double p[4], double o[4]) {
    double t[4];
    for (int i = 0; i < 4; ++i) t[i] = a.m[i][0] * p[0] + a.m[i][1] * p[1] + a.m[i][2] * p[2] + a.m[i][3] * p[3];
    for (int i = 0; i < 4; ++i) o[i] = t[i];
}
inline bool Finite(const M4& a) {
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) if (!std::isfinite(a.m[i][j])) return false;
    return true;
}
// Gauss-Jordan with partial pivoting. False = singular / not finite.
inline bool Inv(const M4& a, M4* out) {
    double w[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) w[i][j] = j < 4 ? a.m[i][j] : (j - 4 == i ? 1.0 : 0.0);
    double scale = 0.0;
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) scale = std::fmax(scale, std::fabs(a.m[i][j]));
    if (!(scale > 0.0) || !std::isfinite(scale)) return false;
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r) if (std::fabs(w[r][c]) > std::fabs(w[p][c])) p = r;
        if (!(std::fabs(w[p][c]) > 1e-14 * scale)) return false;
        if (p != c) for (int j = 0; j < 8; ++j) { const double t = w[p][j]; w[p][j] = w[c][j]; w[c][j] = t; }
        const double d = 1.0 / w[c][c];
        for (int j = 0; j < 8; ++j) w[c][j] *= d;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = w[r][c];
            if (f != 0.0) for (int j = 0; j < 8; ++j) w[r][j] -= f * w[c][j];
        }
    }
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) out->m[i][j] = w[i][j + 4];
    return Finite(*out);
}

// ---- which cb0 block is an MVP (the draw_ids.cpp phase 22 / round 7 rules, on the CPU) -----------------------------------
constexpr double kShapeMin = 0.02;                       // draw_ids.cpp kShapeMin
constexpr double kOFreeW = 0.20, kOFreeXY = 0.50;        // draw_ids.cpp round 7b (camera-relative draws)
inline double Len3(const double* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
// clip x, y and w rows each > 2 % of the longest of rows 0..2 (a (0, 0, 0, 1) row inside the block fails it)
inline bool ShapeOk(const M4& m) {
    const double l0 = Len3(m.m[0]), l1 = Len3(m.m[1]), l2 = Len3(m.m[2]), l3 = Len3(m.m[3]);
    const double lm = std::fmax(l0, std::fmax(l1, l2));
    return lm > 1e-20 && l0 > kShapeMin * lm && l1 > kShapeMin * lm && l3 > kShapeMin * lm;
}
// a CAMERA-RELATIVE MVP (projection * view rotation, the origin at the eye: column 3 ~ (0, 0, near, ~0)) -- its own motion is
// the head's rotation only (the car's camera-locked cabin overlays, roadside grass): never a cab reference
inline bool OriginFree(const M4& m) {
    const double x = m.m[0][3], y = m.m[1][3], z = m.m[2][3], w = m.m[3][3];
    return std::fabs(z) > 1e-6 && std::fabs(w) <= kOFreeW && std::fabs(x) <= kOFreeXY && std::fabs(y) <= kOFreeXY;
}
// rows4 = cb0 rows 4, 5, ... of a draw (nRows rows of 4 floats). The first 4-row block from row 4 up (4..8) with the SHAPE of an
// MVP (DrawIdMv CSGather's scan); 0 = none. *out = that block.
inline int FindMvpRow(const float* rows4, int nRows, M4* out) {
    for (int r = 4; r <= 8; ++r) {
        if (r - 4 + 4 > nRows) break;
        const M4 b = FromRows(rows4 + (r - 4) * 4);
        if (Finite(b) && ShapeOk(b)) { if (out) *out = b; return r; }
    }
    return 0;
}

// phase 47c: two draws of one pass carry the same model transform (rigidly fixed to each other): their MVPs are equal within
// a tight relative tolerance (the game writes the same matrix: bit-identical in practice; 1e-5 of the largest entry allows float
// noise of a recomputed product).
inline bool SameTransform(const M4& a, const M4& b, double tol = 1e-5) {
    double s = 0.0, d = 0.0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) { s = std::fmax(s, std::fabs(a.m[i][j])); d = std::fmax(d, std::fabs(a.m[i][j] - b.m[i][j])); }
    return s > 0.0 && d <= tol * s;
}

// ---- the eye in a draw's model space ------------------------------------------------------------------------------------
// From an MVP M = P V W (P a perspective projection whose clip-w row is the view depth): C = the eye (camera centre) in model
// units (rows x, y, w of M vanish there), F = the forward direction (clip w grows along row 3), R / U = the right / up directions
// (rows 0 / 1 without their forward part), k = game metres per model unit (|row 3 xyz|), p00 = the projection's x scale
// (tan R - tan L = 2 / p00). All directions unit length in model units. False = degenerate / not finite.
struct EyeFrame { double C[3], R[3], U[3], F[3]; double k = 0.0, p00 = 0.0; bool ok = false; };
inline bool EyeFrameOf(const M4& m, EyeFrame* e) {
    *e = EyeFrame();
    if (!Finite(m)) return false;
    const double* r0 = m.m[0];
    const double* r1 = m.m[1];
    const double* r3 = m.m[3];
    const double k = Len3(r3);
    if (!(k > 1e-9)) return false;
    double F[3] = { r3[0] / k, r3[1] / k, r3[2] / k };
    const double d0 = r0[0] * F[0] + r0[1] * F[1] + r0[2] * F[2];
    double R[3] = { r0[0] - d0 * F[0], r0[1] - d0 * F[1], r0[2] - d0 * F[2] };
    const double lr = Len3(R);
    if (!(lr > 1e-9 * k)) return false;
    for (double& v : R) v /= lr;
    const double d1f = r1[0] * F[0] + r1[1] * F[1] + r1[2] * F[2];
    double U[3] = { r1[0] - d1f * F[0], r1[1] - d1f * F[1], r1[2] - d1f * F[2] };
    const double d1r = U[0] * R[0] + U[1] * R[1] + U[2] * R[2];
    for (int i = 0; i < 3; ++i) U[i] -= d1r * R[i];
    const double lu = Len3(U);
    if (!(lu > 1e-9 * k)) return false;
    for (double& v : U) v /= lu;
    // C: [r0; r1; r3] C = -(m03, m13, m33) (Cramer)
    const double a[3][3] = { { r0[0], r0[1], r0[2] }, { r1[0], r1[1], r1[2] }, { r3[0], r3[1], r3[2] } };
    const double b[3] = { -r0[3], -r1[3], -r3[3] };
    auto det3 = [](const double x[3][3]) {
        return x[0][0] * (x[1][1] * x[2][2] - x[1][2] * x[2][1]) - x[0][1] * (x[1][0] * x[2][2] - x[1][2] * x[2][0]) +
               x[0][2] * (x[1][0] * x[2][1] - x[1][1] * x[2][0]);
    };
    const double D = det3(a);
    if (!(std::fabs(D) > 1e-30) || !std::isfinite(D)) return false;
    for (int c = 0; c < 3; ++c) {
        double t[3][3];
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) t[i][j] = j == c ? b[i] : a[i][j];
        e->C[c] = det3(t) / D;
    }
    memcpy(e->R, R, sizeof(R)); memcpy(e->U, U, sizeof(U)); memcpy(e->F, F, sizeof(F));
    e->k = k;
    e->p00 = lr / k;
    e->ok = std::isfinite(e->C[0]) && std::isfinite(e->C[1]) && std::isfinite(e->C[2]) && e->p00 > 1e-6;
    return e->ok;
}

// ---- play space (the game's OpenXR reference space) ---------------------------------------------------------------------
struct Pose { double p[3]; double q[4]; };              // OpenXR: position (m), orientation (x, y, z, w); view +x right, +y up, -z fwd
inline void QRot(const double q[4], const double v[3], double o[3]) { FpsPose::QRotate(q, v, o); }
inline void QRotInv(const double q[4], const double v[3], double o[3]) {
    const double c[4] = { -q[0], -q[1], -q[2], q[3] };
    FpsPose::QRotate(c, v, o);
}
// The affine map play space -> model space of one draw at the moment its MVPs were taken: ef[i] = the eye frame of that draw in
// eye i, pose[i] = that eye's play-space pose then; n = 1 or 2. Rotation: a play vector v is v_view = q^-1 v in the eye's view
// axes and R v_view.x + U v_view.y - F v_view.z in the model (any handedness of the model space). Scale: with two eyes, model
// units per play metre = |C0 - C1| / |p0 - p1| (the game's eye separation against the headset's: the picture's own metric) when
// that is within 2x of 1 / k, else 1 / k. Origin: the eyes' midpoints match. *swapped = the two poses were handed in the
// wrong order (the eye to the right in the model is the eye to the right in the play space) -- they are used swapped.
// *ipdRatio = (|C0 - C1| k) / |p0 - p1| (1 = the game renders with the headset's IPD; 0 = one eye).
inline bool PlayToModel(const EyeFrame* ef, const Pose* poseIn, int n, M4* A, double* unitsPerM, bool* swapped, double* ipdRatio) {
    if (n < 1 || n > 2 || !ef[0].ok || (n == 2 && !ef[1].ok)) return false;
    Pose pose[2] = { poseIn[0], n == 2 ? poseIn[1] : poseIn[0] };
    bool sw = false;
    double upm = 1.0 / ef[0].k, ratio = 0.0;
    if (n == 2) {
        const double dC[3] = { ef[0].C[0] - ef[1].C[0], ef[0].C[1] - ef[1].C[1], ef[0].C[2] - ef[1].C[2] };
        const double dp[3] = { pose[0].p[0] - pose[1].p[0], pose[0].p[1] - pose[1].p[1], pose[0].p[2] - pose[1].p[2] };
        const double x0[3] = { 1.0, 0.0, 0.0 };
        double right[3];
        QRot(pose[0].q, x0, right);
        const double sm = dC[0] * ef[0].R[0] + dC[1] * ef[0].R[1] + dC[2] * ef[0].R[2];
        const double sp = dp[0] * right[0] + dp[1] * right[1] + dp[2] * right[2];
        if (std::fabs(sm) * ef[0].k > 0.01 && std::fabs(sp) > 0.01 && (sm > 0.0) != (sp > 0.0)) {
            const Pose t = pose[0]; pose[0] = pose[1]; pose[1] = t;
            sw = true;
        }
        const double lm = Len3(dC), lp = Len3(dp);
        if (lp > 0.03 && lm > 0.0) {
            ratio = lm * ef[0].k / lp;
            if (ratio > 0.5 && ratio < 2.0) upm = lm / lp;
        }
    }
    // rotation (eye 0's frame and pose)
    double Rm[3][3];
    for (int c = 0; c < 3; ++c) {
        double v[3] = { 0.0, 0.0, 0.0 }, w[3];
        v[c] = 1.0;                                     // play basis vector c
        QRotInv(pose[0].q, v, w);                       // in eye 0's view axes
        for (int r = 0; r < 3; ++r) Rm[r][c] = ef[0].R[r] * w[0] + ef[0].U[r] * w[1] - ef[0].F[r] * w[2];
    }
    double Cm[3], Pp[3];
    for (int i = 0; i < 3; ++i) {
        Cm[i] = n == 2 ? 0.5 * (ef[0].C[i] + ef[1].C[i]) : ef[0].C[i];
        Pp[i] = n == 2 ? 0.5 * (pose[0].p[i] + pose[1].p[i]) : pose[0].p[i];
    }
    M4 o = Identity();
    for (int r = 0; r < 3; ++r) {
        double t = Cm[r];
        for (int c = 0; c < 3; ++c) { o.m[r][c] = upm * Rm[r][c]; t -= upm * Rm[r][c] * Pp[c]; }
        o.m[r][3] = t;
    }
    *A = o;
    if (unitsPerM) *unitsPerM = upm;
    if (swapped) *swapped = sw;
    if (ipdRatio) *ipdRatio = ratio;
    return Finite(o);
}

// ---- the box in the play space (as the mode-1 quad layer builds it) -------------------------------------------------------
struct Settings {
    double x = 0, y = -25;                                 // fps_vr_x / fps_vr_y: degrees around the seat (+ right, + down)
    int distCm = FpsPose::kDistDefault;                 // fps_vr_dist
    int yaw = 0, pitch = 0, roll = 0;                   // fps_vr_yaw / _pitch / _roll (the box's own turn)
    double scale = 0.3;                                 // fps_vr_scale (1 = 8 % of the eye's fov width at the distance)
    bool operator==(const Settings& o) const {
        return x == o.x && y == o.y && distCm == o.distCm && yaw == o.yaw && pitch == o.pitch && roll == o.roll && scale == o.scale;
    }
    bool operator!=(const Settings& o) const { return !(*this == o); }
};
// LOCAL space: the anchor turns around the space origin (the seat), forward = -z (pivot 0, yaw0 0); any other space: around the
// eyes' midpoint at the lock, forward = the head's yaw then (the mode-1 pivot rule). fw = tan R - tan L of the eye's fov.
struct LockFrame { double pivot[3] = { 0.0, 0.0, 0.0 }; double yaw0 = 0.0; double fw = 1.6; };
constexpr double kAspect = 44.0 / 92.0;                  // TuningMenu::kWidgetRefH / kWidgetRefW (the box texture)
// out[k] = corner k (top-left, top-right, bottom-left, bottom-right: the order of TuningMenu's Corner[]) in the play space,
// *centre = the box centre. The box faces the pivot (+z of its local frame towards it, +y up, no roll), then its own turn
// (FpsPose::TurnQuat in the local frame) -- the OvBuildQuadLocked fixed-box pose, the same size wM = d fw 0.08 scale.
inline void BoxPlay(const Settings& s, const LockFrame& l, double out[4][3], double centre[3], double* widthM = nullptr) {
    const double kRad = 0.017453292519943295;
    const double d = (double)s.distCm / 100.0;
    const double wM = d * l.fw * 0.08 * s.scale, hM = wM * kAspect;
    const double yaw = l.yaw0 + (double)s.x * kRad, pitch = -(double)s.y * kRad;
    const double dir[3] = { std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch) };
    const double c[3] = { l.pivot[0] + d * dir[0], l.pivot[1] + d * dir[1], l.pivot[2] + d * dir[2] };
    const double nrm[3] = { -dir[0], -dir[1], -dir[2] };                    // the box's +z: back to the pivot
    double right[3] = { nrm[2], 0.0, -nrm[0] };                            // up(0,1,0) x n
    double lr = Len3(right);
    if (!(lr > 1e-6)) { right[0] = std::cos(yaw); right[1] = 0.0; right[2] = std::sin(yaw); lr = 1.0; }
    for (double& v : right) v /= lr;
    const double up[3] = { nrm[1] * right[2] - nrm[2] * right[1], nrm[2] * right[0] - nrm[0] * right[2],
                           nrm[0] * right[1] - nrm[1] * right[0] };         // n x right
    double tq[4];
    FpsPose::TurnQuat((double)s.yaw, (double)s.pitch, (double)s.roll, tq);
    const double sx[4] = { -1.0, 1.0, -1.0, 1.0 }, sy[4] = { 1.0, 1.0, -1.0, -1.0 };
    for (int k = 0; k < 4; ++k) {
        const double lo[3] = { sx[k] * 0.5 * wM, sy[k] * 0.5 * hM, 0.0 };
        double r[3];
        FpsPose::QRotate(tq, lo, r);
        for (int i = 0; i < 3; ++i) out[k][i] = c[i] + right[i] * r[0] + up[i] * r[1] + nrm[i] * r[2];
    }
    if (centre) { centre[0] = c[0]; centre[1] = c[1]; centre[2] = c[2]; }
    if (widthM) *widthM = wM;
}

// ---- phase 51b: the box's CAB POSE (FpsProfile::CabPose, stored per vehicle) ----------------------------------------------
// The play -> part-model map A of one rigid cab part + the LockFrame: corners = A BoxPlay(settings). Stored once per vehicle, it
// places the box at a later lock-on WITHOUT that lock's own play -> cab map (which carries the lock frame's pose error: the
// located / submitted pose correction C changes by up to ~1 deg from frame to frame in the ATS 13:41 log): the reference's map
// is A_ref = T_j^-1 A_j (T_j = the lock's reference -> part j transform, Entry::T: exact for a rigid pair, no pose involved).
inline M4 CabM(const FpsProfile::CabPose& c) {
    M4 o = Identity();
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) o.m[i][j] = c.A[i][j];
    return o;
}
inline LockFrame CabFrame(const FpsProfile::CabPose& c) {
    LockFrame l;
    for (int i = 0; i < 3; ++i) l.pivot[i] = c.pivot[i];
    l.yaw0 = c.yaw0; l.fw = c.fw;
    return l;
}
inline void CabPoseSet(const M4& A, const LockFrame& lf, uint32_t ic, uint32_t stride, uint32_t fmt, FpsProfile::CabPose* out) {
    *out = FpsProfile::CabPose();
    out->ic = ic; out->stride = stride; out->fmt = fmt;
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) out->A[i][j] = A.m[i][j];
    for (int i = 0; i < 3; ++i) out->pivot[i] = lf.pivot[i];
    out->yaw0 = lf.yaw0; out->fw = lf.fw;
}
// the box corners (homogeneous) + centre in the cab pose's part model space for settings s
inline void CabCorners(const FpsProfile::CabPose& c, const Settings& s, double out[4][4], double centre[4] = nullptr) {
    double cp[4][3], cc[3];
    BoxPlay(s, CabFrame(c), cp, cc);
    const M4 A = CabM(c);
    for (int k = 0; k < 4; ++k) { const double p[4] = { cp[k][0], cp[k][1], cp[k][2], 1.0 }; Apply(A, p, out[k]); }
    if (centre) { const double p[4] = { cc[0], cc[1], cc[2], 1.0 }; Apply(A, p, centre); }
}
// the lock reference's play -> model map from a cab pose of part j (Tj: the reference's model space -> part j's). False = singular.
inline bool RefMapFromCab(const FpsProfile::CabPose& c, const M4& Tj, M4* Aref) {
    M4 inv;
    if (!Inv(Tj, &inv)) return false;
    *Aref = Mul(inv, CabM(c));
    return Finite(*Aref);
}
// The lock-on map from SEVERAL readbacks (phase 51b: one frame's map carries that frame's pose error). A[0..n-1]: the play ->
// reference-model maps of n lock readbacks (the same reference part). Each map's images of the probe points (the box corners in
// the play space) are compared with the per-coordinate median image; a map further than max(3 x the median distance, tol)
// (model units) is an OUTLIER (a frame with a pose glitch / fast head turn) and dropped; the rest are averaged (entries), the
// linear part made orthogonal again (Gram-Schmidt on the columns, the maps' mean column length: a rotation x scale, any
// handedness). Returns the maps used (0 = none: *out unchanged); *spread = the largest distance of a used map (model units).
constexpr int kMaxLockSamples = 16;
inline int AverageMaps(const M4* A, int n, const double (*probe)[3], int np, double tol, M4* out, double* spread = nullptr) {
    if (n < 1 || np < 1) return 0;
    if (n > kMaxLockSamples) n = kMaxLockSamples;
    if (np > 8) np = 8;
    double img[kMaxLockSamples][8][3];
    for (int f = 0; f < n; ++f)
        for (int p = 0; p < np; ++p) {
            const double h[4] = { probe[p][0], probe[p][1], probe[p][2], 1.0 };
            double o[4];
            Apply(A[f], h, o);
            for (int i = 0; i < 3; ++i) img[f][p][i] = o[i];
        }
    auto median = [](double* v, int m) {                  // (insertion sort: m <= 16)
        for (int a = 1; a < m; ++a) for (int b = a; b > 0 && v[b] < v[b - 1]; --b) { const double t = v[b]; v[b] = v[b - 1]; v[b - 1] = t; }
        return (m & 1) ? v[m / 2] : 0.5 * (v[m / 2 - 1] + v[m / 2]);
    };
    double med[8][3];
    for (int p = 0; p < np; ++p)
        for (int i = 0; i < 3; ++i) {
            double v[kMaxLockSamples];
            for (int f = 0; f < n; ++f) v[f] = img[f][p][i];
            med[p][i] = median(v, n);
        }
    double d[kMaxLockSamples], ds[kMaxLockSamples];
    for (int f = 0; f < n; ++f) {
        double m = 0.0;
        for (int p = 0; p < np; ++p) {
            const double t[3] = { img[f][p][0] - med[p][0], img[f][p][1] - med[p][1], img[f][p][2] - med[p][2] };
            m = std::fmax(m, Len3(t));
        }
        d[f] = ds[f] = m;
    }
    const double lim = std::fmax(3.0 * median(ds, n), tol);
    double sum[3][4] = {}, sp = 0.0, len = 0.0;
    int used = 0;
    for (int f = 0; f < n; ++f) {
        if (!(d[f] <= lim) || !Finite(A[f])) continue;
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) sum[i][j] += A[f].m[i][j];
        for (int j = 0; j < 3; ++j) { const double col[3] = { A[f].m[0][j], A[f].m[1][j], A[f].m[2][j] }; len += Len3(col); }
        sp = std::fmax(sp, d[f]);
        ++used;
    }
    if (!used) return 0;
    M4 o = Identity();
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) o.m[i][j] = sum[i][j] / used;
    len /= 3.0 * used;                                  // (the maps' own column length: averaging rotations shortens the columns)
    double c[3][3];
    for (int j = 0; j < 3; ++j) for (int i = 0; i < 3; ++i) c[j][i] = o.m[i][j];
    for (int j = 0; j < 3; ++j) {
        for (int k = 0; k < j; ++k) {
            const double dt = c[j][0] * c[k][0] + c[j][1] * c[k][1] + c[j][2] * c[k][2];
            for (int i = 0; i < 3; ++i) c[j][i] -= dt * c[k][i];
        }
        const double l = Len3(c[j]);
        if (!(l > 1e-12)) return 0;
        for (int i = 0; i < 3; ++i) c[j][i] /= l;
    }
    for (int j = 0; j < 3; ++j) for (int i = 0; i < 3; ++i) o.m[i][j] = c[j][i] * len;
    if (!Finite(o)) return 0;
    *out = o;
    if (spread) *spread = sp;
    return used;
}

// ---- phase 51d: the box SQUARE TO THE CAB (not to the play space) ------------------------------------------------------------
// BoxPlay builds the box upright in the play space and facing its -z. The play space is the headset's LOCAL space: gravity-up, its
// -z where the user recentred. The cab is not always aligned with it -- the ATS 15:02 truck 47a8: the stored play -> part maps of
// IndexCount 12246 / 8325 are turned 11.4 deg (10.4 deg about the cab's forward axis, 4.6 about its side axis) and the steering
// wheel's tilt axis (a cab side axis by construction: the wheel / 12246 relative turn is 49 deg about x only) lies 10.4 deg off the
// play horizontal; the box looked rolled against the dashboard with every turn at 0. So the box is built in a frame made from the
// CAB's own axes: the rotation shared by the largest summed IndexCount of the lock's cab draws (camera-relative ones included --
// their MVP's translation is gone, their rotation is still P V W: the cab's axes), its up axis the one nearest the play up (+y in
// the game's models: then forward = -z), turned onto the box's frame about the pivot. The orientation then comes from the MVPs
// alone (exact, no pose); only the pivot (the seat) and the scale come from the play -> cab map.
//
// S = the rotation that takes model space b's axes into model space a's, from one eye's frames in both (the same pass: the
// view / projection cancel): S = R_a R_b^T + U_a U_b^T + F_a F_b^T (column i = b's axis i in a's coordinates).
inline void FrameRot(const EyeFrame& a, const EyeFrame& b, double S[3][3]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) S[i][j] = a.R[i] * b.R[j] + a.U[i] * b.U[j] + a.F[i] * b.F[j];
}
// two draws of one eye's pass with the same model ROTATION (their eye frames' axes agree within tol; the translation may differ:
// separate nodes of one rigid cab, or camera-relative)
inline bool SameRot(const EyeFrame& a, const EyeFrame& b, double tol = 0.0035) {
    double d = 0.0;
    for (int i = 0; i < 3; ++i) {
        d = std::fmax(d, std::fabs(a.R[i] - b.R[i]));
        d = std::fmax(d, std::fabs(a.U[i] - b.U[i]));
        d = std::fmax(d, std::fabs(a.F[i] - b.F[i]));
    }
    return d <= tol;
}
// an MVP that is the bare projection (no view rotation: a HEAD-LOCKED draw -- its axes are the eye's, never the cab's)
inline bool HeadLocked(const EyeFrame& e) {
    for (int k = 0; k < 3; ++k) {                       // R, U, F each exactly on a model axis
        const double* v = k == 0 ? e.R : (k == 1 ? e.U : e.F);
        const double m = std::fmax(std::fabs(v[0]), std::fmax(std::fabs(v[1]), std::fabs(v[2])));
        if (!(std::fabs(m - 1.0) < 1e-8)) return false;
    }
    return true;
}
constexpr double kCabLevelMaxDeg = 30.0;                 // a cab frame further from the box's frame than this is not trusted
// The box's frame square to the cab: A = the map virtual frame -> reference model (the play space of a lock, or a stored pose's
// frame), S = the cab's axes in the reference model (FrameRot(ref, cab)). Q (columns: right, up, back in the virtual frame) = the
// cab's axes there: up = the cab axis nearest the virtual up; back = the cab's +z when up is its +y (the game's models: y up, -z
// forward), else the axis nearest the virtual back; right = up x back. *turnDeg = Q's angle (0 = already square), *upAxis = the
// cab axis taken as up (1 = +y). False = degenerate or turned more than kCabLevelMaxDeg (*turnDeg still set).
inline bool CabLevel(const M4& A, const double S[3][3], double Q[3][3], double* turnDeg = nullptr, int* upAxis = nullptr) {
    M4 inv;
    if (!Inv(A, &inv)) return false;
    double w[3][3];                                     // w[i] = the cab's axis i in the virtual frame (unit)
    for (int i = 0; i < 3; ++i) {
        for (int r = 0; r < 3; ++r) w[i][r] = inv.m[r][0] * S[0][i] + inv.m[r][1] * S[1][i] + inv.m[r][2] * S[2][i];
        const double l = Len3(w[i]);
        if (!(l > 1e-12)) return false;
        for (double& v : w[i]) v /= l;
    }
    int iu = 0;
    double su = 1.0, bu = -2.0;
    for (int i = 0; i < 3; ++i) { const double d = w[i][1]; if (std::fabs(d) > bu) { bu = std::fabs(d); iu = i; su = d >= 0.0 ? 1.0 : -1.0; } }
    double up[3] = { su * w[iu][0], su * w[iu][1], su * w[iu][2] }, back[3];
    if (iu == 1 && su > 0.0) { for (int r = 0; r < 3; ++r) back[r] = w[2][r]; }
    else {
        int ib = iu == 0 ? 1 : 0;
        double sb = 1.0, bb = -2.0;
        for (int i = 0; i < 3; ++i) { if (i == iu) continue; const double d = w[i][2]; if (std::fabs(d) > bb) { bb = std::fabs(d); ib = i; sb = d >= 0.0 ? 1.0 : -1.0; } }
        for (int r = 0; r < 3; ++r) back[r] = sb * w[ib][r];
    }
    const double lu = Len3(up);
    for (double& v : up) v /= lu;
    const double d = back[0] * up[0] + back[1] * up[1] + back[2] * up[2];
    for (int r = 0; r < 3; ++r) back[r] -= d * up[r];
    const double lb = Len3(back);
    if (!(lb > 1e-9)) return false;
    for (double& v : back) v /= lb;
    const double right[3] = { up[1] * back[2] - up[2] * back[1], up[2] * back[0] - up[0] * back[2], up[0] * back[1] - up[1] * back[0] };
    for (int r = 0; r < 3; ++r) { Q[r][0] = right[r]; Q[r][1] = up[r]; Q[r][2] = back[r]; }
    const double c = std::fmax(-1.0, std::fmin(1.0, 0.5 * (Q[0][0] + Q[1][1] + Q[2][2] - 1.0)));
    const double deg = std::acos(c) * 57.29577951308232;
    if (turnDeg) *turnDeg = deg;
    if (upAxis) *upAxis = iu;
    return std::isfinite(deg) && deg <= kCabLevelMaxDeg;
}
// T_Q: the rotation Q about the pivot (box frame -> virtual frame); the levelled map = A T_Q (corners = A T_Q BoxPlay(settings)).
inline M4 LevelT(const double Q[3][3], const double pivot[3]) {
    M4 t = Identity();
    for (int r = 0; r < 3; ++r) {
        double s = pivot[r];
        for (int c = 0; c < 3; ++c) { t.m[r][c] = Q[r][c]; s -= Q[r][c] * pivot[c]; }
        t.m[r][3] = s;
    }
    return t;
}
// For the log: the cab against the former box frame (Q's columns right, up, back): the cab up leaning sideways (+ = to the right) /
// forward (+ = forward), the cab forward turned (+ = to the right), degrees.
inline void CabTilt(const double Q[3][3], double* side, double* fore, double* yaw) {
    const double k = 57.29577951308232;
    *side = std::atan2(Q[0][1], Q[1][1]) * k;
    *fore = std::atan2(-Q[2][1], Q[1][1]) * k;
    *yaw = std::atan2(-Q[0][2], Q[2][2]) * k;
}

// ---- consensus / stats helpers ------------------------------------------------------------------------------------------
// Where part c (MVP Mc, its box centre ac in its model space, homogeneous) puts the box centre now, expressed in part d's model
// space and compared with d's own centre ad: the distance in game metres (k of d's eye frame now). Rigid parts: 0. -1 = not
// computable.
inline double Disagree(const M4& Mc, const double ac[4], const M4& Md, const double ad[4]) {
    M4 inv;
    EyeFrame ed;
    if (!Inv(Md, &inv) || !EyeFrameOf(Md, &ed)) return -1.0;
    double p[4], x[4];
    Apply(Mc, ac, p);
    Apply(inv, p, x);
    if (!(std::fabs(x[3]) > 1e-12) || !(std::fabs(ad[3]) > 1e-12)) return -1.0;
    double s = 0.0;
    for (int i = 0; i < 3; ++i) { const double t = x[i] / x[3] - ad[i] / ad[3]; s += t * t; }
    return std::sqrt(s) * ed.k;
}
// The play-space position of model point a (homogeneous) seen through MVP M by an eye at `pose`, game metres scaled by
// `playPerGame` (1 / (units per m * k at the lock): ~1). False = not computable.
inline bool PlayOf(const M4& M, const double a[4], const Pose& pose, double playPerGame, double out[3]) {
    EyeFrame e;
    if (!EyeFrameOf(M, &e) || !(std::fabs(a[3]) > 1e-12)) return false;
    const double X[3] = { a[0] / a[3] - e.C[0], a[1] / a[3] - e.C[1], a[2] / a[3] - e.C[2] };
    const double s = e.k * playPerGame;
    const double v[3] = { (X[0] * e.R[0] + X[1] * e.R[1] + X[2] * e.R[2]) * s, (X[0] * e.U[0] + X[1] * e.U[1] + X[2] * e.U[2]) * s,
                          -(X[0] * e.F[0] + X[1] * e.F[1] + X[2] * e.F[2]) * s };
    double w[3];
    QRot(pose.q, v, w);
    for (int i = 0; i < 3; ++i) out[i] = pose.p[i] + w[i];
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

}  // namespace FpsCab

// ---- the runtime (fps_cab.cpp; the DLL and tests/fps_cab_test.cpp) --------------------------------------------------------
#ifdef WITH_DLAA
#include <d3d11.h>
class DrawIdRecord;
namespace FpsCab {

constexpr int kCand = 16;                               // cab draws per eye in a snapshot / the anchor table (biggest first)
// The OpenXR side of one frame, per DLAA eye index (0 = the picture of OpenXR view 1 normally; the lock checks the order).
struct XrIn {
    bool ok = false;                                    // poses usable (tracked, fresh, not a VIEW space)
    bool local = false;                                 // the layer space is LOCAL (origin = the seat)
    Pose eye[2] = {};
    double fw[2] = { 1.6, 1.6 };                        // tan R - tan L of each eye's fov
};
// Why Get gave no attached placement. Phase 47b: whether the frame shows the attached box at all is decided ONCE per frame
// (Latch, at Present, with hysteresis), never per eye picture: attached = the attached box in both eyes (an eye that cannot
// have it this frame gets NO box); not attached = the lock-1 box for both eyes (the quad layer with menu_vr_layer = 1, else
// drawn in) -- exactly one box per eye picture, never two kinds in one frame.
enum Why : int {
    kOk = 0, kInactive, kNoBlit, kNoRecord, kLocking, kNoCab, kNotFound, kShader, kSettling, kStale, kWhyCount
};
const char* WhyName(int why);

void SetActive(bool on, uint64_t frame);                // mode 2 + box shown + VR; a switch on = a fresh lock
void Reset(const char* why);                            // forget the lock (menu Reset, the lock row toggled)
void SetSettings(const Settings& s);                    // fps_vr_*; a change rebuilds the corners from the lock frame
bool Active();
// Render thread, at eye `eye`'s blit: that pass's draw record (Usable = flushed mirror), the pass serial, the Present count and
// whether this pass starts a VR frame (the phase-24 parity: a snapshot takes both eyes of ONE frame -- this pass and serial + 1).
void OnBlit(ID3D11DeviceContext* ctx, int eye, DrawIdRecord& rec, uint64_t serial, uint64_t frame, bool frameStart, const XrIn& xr);
// The box for eye `eye` now: the record's cbuffer mirror (raw SRV, a reference kept until the eye's next blit), the byte offset of
// the reference draw's MVP block in it, its four model-space corners. False -> *why.
// Phase 47b: passSeq = the passes started so far (inject.cpp g_passSeq): the eye's pass must be one of the two newest (a pass
// that started since may already have reset its record slot / overwritten the mirror -- a stale MVP is never used), and the
// eye must have had its blit since the last Latch or the one before (this frame; a Present between the eyes tolerated).
struct Place { ID3D11ShaderResourceView* srv = nullptr; uint32_t off = 0; float corner[4][4] = {}; int entry = -1; };
bool Get(int eye, uint64_t passSeq, Place* out, int* why);
// Phase 47b, once per frame (inject.cpp MenuOverlaySet, at Present): this frame's verdict -- both eyes had a blit with the
// attached placement available -- into the hysteresis: Attached() turns on after kOnFrames such frames in a row, off when
// kOffFrames of the last 8 frames were without (in a row or scattered: no flapping). Attached() is what the next frame shows (the drawn-in attached box) and what the quad layer
// leaves out (MenuOverlaySet: no fps quad while attached).
void Latch();
bool Attached();
constexpr int kOnFrames = 8, kOffFrames = 3;
void NoteDraw(bool attached, int why);                  // per eye picture drawn while Attached() (attached, or held = no box)
void Shutdown();                                        // device change / exit: every D3D object dropped
// Test hooks (tests/fps_cab_test.cpp): the monitor snapshot period in eye blits (both eyes; default 12 = 6 frames ~0.08 s at 72 Hz)
void SetMonitorEvery(int blits);
struct Stats { int state; int entries; int demoted; uint64_t reacquires, relocks, locks, attached, fallback; int refEntry[2];
               uint64_t lockFrame; double devMaxM; int devN;   // devMaxM: the cab vs the play space (log window / lock)
               int lockEyes; bool lockXr; uint64_t monitors, latchOn, latchOff, held, lockRefusals; bool latched;
               uint32_t refIc; int remembered; double monitorUs;   // phase 47d
               uint64_t vehicleKey; bool sameCab;                    // phase 51 (fps profiles)
               int placed; };                                        // phase 51b (GetLockInfo().placed)
Stats GetStats();
// Phase 51 (per-vehicle fps box profiles, src/fps_profile.h): the locks made so far (a change = a new lock-on), the current lock's
// vehicle key (0 = no lock) and whether that lock is a phase 47e re-lock on the same cab (the box kept its spot).
uint64_t LockCount();
uint64_t VehicleKey();
bool LockSameCab();
// Phase 51b: where the current lock's play -> cab map came from.
enum Placed : int {
    kPlacedNone = 0,                                    // no lock
    kPlacedPlay,                                        // the play space, one lock readback (SetLockSamples(1) / a lone sample)
    kPlacedPlayAvg,                                     // the play space, averaged over several lock readbacks (AverageMaps)
    kPlacedCab,                                         // a stored cab pose (FpsProfile::CabPose: the provider / PlaceFromCab)
    kPlacedSameCab,                                     // a phase 47e re-lock on the same cab (the previous lock's map carried over)
};
struct LockInfo {
    int placed = kPlacedNone;
    int samples = 0, samplesUsed = 0;                   // kPlacedPlayAvg: readbacks taken / kept (outliers dropped)
    double spreadMm = 0.0;                              // kPlacedPlayAvg: the largest distance of a kept readback's box from the median
    uint32_t cabIc = 0;                                 // kPlacedCab: the stored pose's part
    bool cabMissing = false;                            // a stored pose was offered but its part is not among the lock's rigid parts
    uint32_t missingIc = 0;
    int cabFound = 0, cabAgree = 0;                     // phase 51c: stored parts found in the lock / in the agreeing majority
    bool cabAmbiguous = false;                          // phase 51c: the found stored parts disagree, no majority (-> the play space)
    bool remeasured = false;                            // phase 51c: this lock-on was asked for by Remeasure
    // phase 51d: the box square to the cab (CabLevel)
    bool level = false;                                 // the box is built square to the cab
    bool leveledOld = false;                            // kPlacedCab: an older (play-space) stored pose was levelled at this lock-on
    double turnDeg = 0.0;                               // how far the box frame was turned onto the cab (0 = it was square already)
    double tiltSide = 0.0, tiltFore = 0.0, tiltYaw = 0.0;   // the cab against the box's former frame (CabTilt)
    int cabFrameDraws = 0;                              // the draws sharing the cab rotation (0 = no cab frame: not levelled)
    uint32_t cabFrameIc = 0;                            // the biggest of them
    uint64_t cabFrameSum = 0;                           // their summed IndexCount
    bool cabFrameRejected = false;                      // a cab frame was found but turned more than kCabLevelMaxDeg
};
LockInfo GetLockInfo();
// The current lock's cab pose: the stored one verbatim when the lock was placed from it, else the lock's own map in its reference
// part's model space (entry 0). False = no lock.
bool CurrentCabPose(FpsProfile::CabPose* out);
// Re-place the current lock from a stored cab pose (its part must be one of the lock's rigid, not moving parts): true = placed (the
// box corners rebuilt; GetLockInfo().placed = kPlacedCab). Used when the per-vehicle profiles are switched on with a lock held.
bool PlaceFromCab(const FpsProfile::CabPose& c);
// The stored cab pose of a vehicle key, asked at each lock-on readback (render thread). nullptr = none (the shared mode).
using CabPoseFn = bool (*)(uint64_t key, FpsProfile::CabPose* out);
void SetCabPoseProvider(CabPoseFn fn);
// Phase 51c: drop the lock and take the next one from a fresh averaged play -> cab map (the stored pose not used; the menu's
// "Re-measure cab spot" row). The profile then replaces the vehicle's stored pose with it (GetLockInfo().remeasured).
void Remeasure();
// Phase 51c: counts the times a part of the stored cab pose was found MOVING by the monitor (*part = the last one): the profile
// drops that part from the vehicle's pose.
uint64_t CabMovedSerial(FpsProfile::CabPart* part);
// Phase 51d: every cab part reported moving since *serial (up to cap, oldest first; *serial advanced): a part of ANY lock (also one
// placed from the play space, whose pose the profile stored from the lock: the ATS 15:02 wheel 13200 stayed in the line) and a
// stored part the lock-on skipped as a remembered moving part. The profile drops each from the bound vehicle's pose.
int CabMovedSince(uint64_t* serial, FpsProfile::CabPart* out, int cap);
// Readbacks averaged for a lock-on from the play space (1 = the single-frame lock of phase 47; default kLockSamplesDefault).
constexpr int kLockSamplesDefault = 12;
void SetLockSamples(int n);
// Test hook: the box corners in the model space of the lock entry with IndexCount ic. False = no such entry.
bool EntryCorners(uint32_t ic, double out[4][4]);

}  // namespace FpsCab
#endif
