// GazeMath -- phase 59: EYE-TRACKED FOVEATION, the pure maths (header-only; xr_hook.cpp, inject.cpp and tests\vrs_gaze_test.cpp).
//
//  * GazeToViewTangents: the gaze direction (-z of the located XR_EXT_eye_gaze_interaction pose) rotated into one VIEW's frame
//    with the inverse of that view's located orientation (canted displays: each view has its own), as view tangents
//    tx = x / -z, ty = y / -z (OpenXR view frame: +x right, +y up, -z forward). False = the gaze points sideways / behind the view.
//    The gaze ray starts between the eyes, the views at the eyes: the direction alone is used (a point at infinity; a fixation
//    0.7 m away is ~2.6 deg off per eye -- far inside the sharp ring).
//  * GazeUv: view tangents -> the ring centre in uv of the eye picture (v down). With the eye's optical centre (cu, cv) = where the
//    view's forward axis lands (inject.cpp VrEyeCentre: uv = 0.5 - 0.5 p, 0.5 + 0.5 q from the projection skew) and its measured
//    projection scale |P00|, |P11| (VrEyeScale): ndc.x = P00 * tx + skew, ndc.y = P11 * ty + skew, so
//        u = cu + 0.5 * |P00| * tx,      v = cv - 0.5 * |P11| * ty     (uv v runs DOWN, ndc y UP)
//    clamped to the picture. tests\vrs_gaze_test.cpp checks it against an OpenXR fov projection, skewed fovs included.
//  * Smoother: per eye, exponential toward the target with time constant `ms` (vrs_eye_smooth); a jump of more than kSaccadeUv
//    (a saccade) is followed four times faster.
//  * EdgeGrow (phase 59d): the sharp rings grow when the gaze is far to the side (the tracker reads short there).
//  * FakeGaze: vrs_eye_fake -- a made-up gaze that circles slowly (radius 0.25 tangent ~ 14 deg, one turn in 6 s), the same
//    downstream path as the real one (a Quest 3 has no eye tracker).
#pragma once
#include <cmath>

namespace GazeMath {

// R(q) v for a unit quaternion q = (x, y, z, w)
inline void QRotate(const double q[4], const double v[3], double o[3]) {
    const double t[3] = { 2.0 * (q[1] * v[2] - q[2] * v[1]), 2.0 * (q[2] * v[0] - q[0] * v[2]), 2.0 * (q[0] * v[1] - q[1] * v[0]) };
    o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

// gazeQ / viewQ: orientations (x, y, z, w) of the gaze pose and of the view, both in the SAME space at the same time.
inline bool GazeToViewTangents(const double gazeQ[4], const double viewQ[4], float* tx, float* ty) {
    const double fwd[3] = { 0.0, 0.0, -1.0 };
    double g[3];
    QRotate(gazeQ, fwd, g);                              // the gaze direction in the space
    const double inv[4] = { -viewQ[0], -viewQ[1], -viewQ[2], viewQ[3] };
    double d[3];
    QRotate(inv, g, d);                                  // ... in the view's own frame
    if (!(d[2] < -1e-3)) return false;
    *tx = (float)(d[0] / -d[2]);
    *ty = (float)(d[1] / -d[2]);
    return true;
}

inline float Clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// (cu, cv) = the eye's optical centre (uv, v down), (sx, sy) = |P00|, |P11| of the eye's projection, (tx, ty) = view tangents.
inline void GazeUv(float cu, float cv, float sx, float sy, float tx, float ty, float* u, float* v) {
    *u = Clamp01(cu + 0.5f * sx * tx);
    *v = Clamp01(cv - 0.5f * sy * ty);
}

// phase 59d: EDGE GROW -- toward the lens edge the eye tracker reads short (issue #21, Varjo Aero: at a full left / right look the
// runtime's own gaze dot stops short of where the eye looks; up / down were fine), so the sharp rings grow with the gaze's SIDEWAYS
// distance from the eye's optical centre: x 1 up to kEdgeFrom uv, then linear to x (1 + kEdgeGrow) at kEdgeFull uv, in kEdgeStep
// steps (a new step = one mask refill; steps keep the refills rare). The Varjo log's full left look: 0.31 uv off.
constexpr float kEdgeFrom = 0.12f, kEdgeFull = 0.28f, kEdgeGrow = 0.30f, kEdgeStep = 0.05f;
// du = gaze u - optical u (uv of the eye picture) -> the ring scale (1 .. 1 + kEdgeGrow)
inline float EdgeGrow(float du) {
    const float d = du < 0.0f ? -du : du;
    if (!(d > kEdgeFrom)) return 1.0f;                   // (NaN: no grow)
    const float t = d >= kEdgeFull ? 1.0f : (d - kEdgeFrom) / (kEdgeFull - kEdgeFrom);
    const float steps = std::floor(t * kEdgeGrow / kEdgeStep + 0.5f);
    return 1.0f + steps * kEdgeStep;
}

// phase 59g: the same steps with HYSTERESIS -- `prev` (-1 = none) is kept until the raw step is 0.75 of a step away from it, so
// a gaze resting on a step border does not flip the radii (one mask refill each) every frame. Returns the step 0 .. 6.
inline int EdgeStep(float du, int prev) {
    const float d = du < 0.0f ? -du : du;
    float raw = 0.0f;                                    // (NaN: 0)
    if (d > kEdgeFrom) raw = (d >= kEdgeFull ? 1.0f : (d - kEdgeFrom) / (kEdgeFull - kEdgeFrom)) * kEdgeGrow / kEdgeStep;
    if (prev >= 0 && std::fabs(raw - (float)prev) < 0.75f) return prev;
    return (int)std::floor(raw + 0.5f);
}

constexpr float kSaccadeUv = 0.06f;                      // a jump this big (uv) is a saccade: followed 4x faster

struct Smoother {
    bool  have = false;
    float u = 0.5f, v = 0.5f;
    // one step toward (tu, tv) after dt seconds with time constant ms (0 = no smoothing)
    void Step(float tu, float tv, double dt, double ms) {
        if (!have) { u = tu; v = tv; have = true; return; }
        double tau = ms / 1000.0;
        const double du = (double)tu - u, dv = (double)tv - v;
        if (std::sqrt(du * du + dv * dv) > kSaccadeUv) tau *= 0.25;
        const double a = (tau <= 0.0 || dt <= 0.0) ? (tau <= 0.0 ? 1.0 : 0.0) : 1.0 - std::exp(-dt / tau);
        u = (float)(u + a * du);
        v = (float)(v + a * dv);
    }
};

// vrs_eye_fake: a circle of radius 0.25 (view tangent) once every 6 s, t in seconds. The same for both views (a gaze at infinity).
inline void FakeGaze(double t, float* tx, float* ty) {
    const double a = 2.0 * 3.141592653589793 * t / 6.0;
    *tx = (float)(0.25 * std::cos(a));
    *ty = (float)(0.25 * std::sin(a));
}

}  // namespace GazeMath
