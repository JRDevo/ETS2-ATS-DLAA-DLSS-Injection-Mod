// FpsPose -- phase 44: the fps box's colour presets (dlaa.ini fps_color) and the pure maths of its VR placement: the distance
// (fps_vr_dist) and the box's own turn angles (fps_vr_yaw / fps_vr_pitch / fps_vr_roll). No D3D, no OpenXR, no game state:
// inject.cpp (the drawn-in fallback), xr_hook.cpp (the quad layer pose) and tests/fps_pose_test.cpp use it.
//
// Turn convention (both paths): the box's local frame is the OpenXR one -- +x right, +y up, +z = its FRONT normal (towards the
// viewer when the box faces him). The turn is applied in that local frame on top of the box's normal facing:
//   q_turn = Ry(+yaw) * Rx(-pitch) * Rz(-roll)
//   yaw   + = the front turns to the RIGHT (its right edge goes away from you)
//   pitch + = the front tilts UP (its top edge goes away from you: lying back, as on a dashboard)
//   roll  + = CLOCKWISE in its own plane as you look at it
// 0 / 0 / 0 = the identity = exactly the facing of phase 28 / 29 / 29b.
#pragma once
#include <cmath>
#include <cstdint>

namespace FpsPose {

// ---- colours (fps_color 0..kColorCount-1) -------------------------------------------------------------------------------
// text = the glyph colour, line = the outline drawn around every glyph (the box has no background: the outline keeps it
// readable over any picture). 0 = white on a near-black outline = the colour of every version before phase 44.
struct Color { const wchar_t* name; uint8_t tr, tg, tb, lr, lg, lb; };
constexpr int kColorCount = 20;
inline const Color& ColorOf(int i) {
    static const Color k[kColorCount] = {
        { L"white",          255, 255, 255,   8,   8,   8 },   // 0: as before phase 44
        { L"yellow",         255, 235,  59,   8,   8,   8 },
        { L"amber",          255, 191,   0,   8,   8,   8 },
        { L"orange",         255, 140,  20,   8,   8,   8 },
        { L"red",            255,  72,  72,   8,   8,   8 },
        { L"pink",           255, 140, 200,   8,   8,   8 },
        { L"magenta",        255,  70, 255,   8,   8,   8 },
        { L"purple",         196, 120, 255,   8,   8,   8 },
        { L"violet",         160, 150, 255,   8,   8,   8 },
        { L"blue",            90, 150, 255,   8,   8,   8 },
        { L"sky blue",       130, 205, 255,   8,   8,   8 },
        { L"cyan",             0, 230, 255,   8,   8,   8 },
        { L"teal",             0, 200, 180,   8,   8,   8 },
        { L"mint",           130, 255, 195,   8,   8,   8 },
        { L"green",           70, 230,  70,   8,   8,   8 },
        { L"lime",           195, 255,  60,   8,   8,   8 },
        { L"olive",          195, 195,  95,   8,   8,   8 },
        { L"gold",           255, 215,   0,   8,   8,   8 },
        { L"silver",         192, 192, 204,   8,   8,   8 },
        { L"black on white",  16,  16,  16, 255, 255, 255 },  // 19: dark text, white outline
    };
    return k[i < 0 || i >= kColorCount ? 0 : i];
}
inline int ClampColor(long long v) { return v < 0 ? 0 : (v >= kColorCount ? kColorCount - 1 : (int)v); }
inline int StepColor(int cur, int dir) { return ((cur + dir) % kColorCount + kColorCount) % kColorCount; }   // wraps

// ---- brightness (fps_brightness 10..100 %, phase 48) ---------------------------------------------------------------------
// The box dims toward black: the glyph colour * pct / 100. A preset whose outline is LIGHTER than its text (19, black on white)
// dims the outline instead, so the box still gets dimmer. 100 = the preset as it is.
constexpr int kBrightMin = 10, kBrightMax = 100, kBrightDefault = 100, kBrightStep = 10;
inline int ClampBright(long long v) { return v < kBrightMin ? kBrightMin : (v > kBrightMax ? kBrightMax : (int)v); }
inline int StepBright(int cur, int dir) { return ClampBright((long long)cur + (long long)kBrightStep * dir); }
inline uint8_t DimByte(int v, int pct) { return (uint8_t)((v * ClampBright(pct) + 50) / 100); }
inline Color Dimmed(const Color& c, int pct) {
    Color o = c;
    const int lt = c.tr + c.tg + c.tb, ll = c.lr + c.lg + c.lb;
    if (ll > lt) { o.lr = DimByte(c.lr, pct); o.lg = DimByte(c.lg, pct); o.lb = DimByte(c.lb, pct); }
    else         { o.tr = DimByte(c.tr, pct); o.tg = DimByte(c.tg, pct); o.tb = DimByte(c.tb, pct); }
    return o;
}

// ---- opacity (fps_opacity 10..100 %, phase 51f) --------------------------------------------------------------------------
// The WHOLE box (glyphs AND outline) fades: the widget texture's alpha * pct / 100 (menu.cpp: the pixel shader's Opt.y), in every
// draw path (all of them blend with that alpha; the OpenXR quad layer is UNPREMULTIPLIED + texture-source-alpha). 100 = as drawn.
constexpr int kOpacityMin = 10, kOpacityMax = 100, kOpacityDefault = 100, kOpacityStep = 10;
inline int ClampOpacity(long long v) { return v < kOpacityMin ? kOpacityMin : (v > kOpacityMax ? kOpacityMax : (int)v); }
inline int StepOpacity(int cur, int dir) { return ClampOpacity((long long)cur + (long long)kOpacityStep * dir); }

// ---- ini ranges --------------------------------------------------------------------------------------------------------
constexpr int kDistMin = 30, kDistMax = 300, kDistDefault = 80;   // fps_vr_dist, cm (80 = the phase 28 0.8 m)
constexpr int kYawMax = 90, kPitchMax = 90, kRollMax = 180;       // degrees, symmetric
inline int ClampI(long long v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : (int)v); }

// ---- quaternions (x, y, z, w), OpenXR convention ----------------------------------------------------------------------
inline void QMul(const double a[4], const double b[4], double o[4]) {
    const double x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    const double y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    const double z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    const double w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}
inline void QAxis(int axis, double rad, double q[4]) {
    q[0] = q[1] = q[2] = 0.0;
    q[axis] = std::sin(0.5 * rad);
    q[3] = std::cos(0.5 * rad);
}
// v' = q v q*
inline void QRotate(const double q[4], const double v[3], double o[3]) {
    const double t[3] = { 2.0 * (q[1] * v[2] - q[2] * v[1]), 2.0 * (q[2] * v[0] - q[0] * v[2]), 2.0 * (q[0] * v[1] - q[1] * v[0]) };
    o[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    o[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    o[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}
// The box's own turn (see the header comment). Angles in degrees.
inline void TurnQuat(double yawDeg, double pitchDeg, double rollDeg, double q[4]) {
    const double kRad = 0.017453292519943295;
    double qy[4], qx[4], qz[4], t[4];
    QAxis(1, yawDeg * kRad, qy);
    QAxis(0, -pitchDeg * kRad, qx);
    QAxis(2, -rollDeg * kRad, qz);
    QMul(qy, qx, t);
    QMul(t, qz, q);
}
inline bool TurnIsZero(int yawDeg, int pitchDeg, int rollDeg) { return !yawDeg && !pitchDeg && !rollDeg; }
// The quad layer orientation: the normal facing `base` (unit quaternion, the box's +z towards the viewer) turned by the box's
// own angles in its local frame: base * turn.
inline void ComposeTurn(const double base[4], int yawDeg, int pitchDeg, int rollDeg, double out[4]) {
    double t[4];
    TurnQuat(yawDeg, pitchDeg, rollDeg, t);
    QMul(base, t, out);
    const double n = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
    if (n > 1e-12) for (int k = 0; k < 4; ++k) out[k] /= n;
}

// ---- the drawn-in fallback: the box's 4 corners in an eye picture -----------------------------------------------------
// Pinhole of the eye picture (W x H px): optical centre (u0, v0) px, focal lengths fx, fy px per unit tangent (y down in the
// picture). The flat box is centred at (cx, cy) px, bw x bh px. It is modelled as a quad PARALLEL to the image plane at unit
// depth through that point (exactly the flat box when not turned), turned about its centre by TurnQuat, and projected back.
// `shift` px is added to every corner's x (the follow-head stereo shift). clip[i] = (x, y, z, w) of the corners top-left,
// top-right, bottom-left, bottom-right in CLIP space (w = the corner's depth, z = 0.5 w: perspective-correct texture, as the
// VR panel's MenuFacingGeom). False = a corner at or behind 0.05 of the centre's depth (turned edge-on into the eye) or not
// finite -- the caller then draws the flat box.
inline bool TurnedCorners(double W, double H, double u0, double v0, double fx, double fy, double cx, double cy, double bw,
                          double bh, int yawDeg, int pitchDeg, int rollDeg, double shift, float clip[4][4]) {
    if (!(W > 1.0) || !(H > 1.0) || !(fx > 1.0) || !(fy > 1.0)) return false;
    double q[4];
    TurnQuat(yawDeg, pitchDeg, rollDeg, q);
    // the centre in the view (OpenXR axes: +x right, +y up, -z forward) at depth 1
    const double C[3] = { (cx - u0) / fx, -(cy - v0) / fy, -1.0 };
    const double a = 0.5 * bw / fx, b = 0.5 * bh / fy;   // half size at depth 1 (the flat box's angle at the axis)
    const double sx[4] = { -1.0, 1.0, -1.0, 1.0 }, sy[4] = { 1.0, 1.0, -1.0, -1.0 };
    for (int i = 0; i < 4; ++i) {
        const double l[3] = { sx[i] * a, sy[i] * b, 0.0 };
        double r[3];
        QRotate(q, l, r);
        const double P[3] = { C[0] + r[0], C[1] + r[1], C[2] + r[2] };
        const double depth = -P[2];
        if (!(depth > 0.05)) return false;
        const double px = u0 + fx * P[0] / depth + shift;
        const double py = v0 - fy * P[1] / depth;
        const double nx = px / W * 2.0 - 1.0, ny = 1.0 - py / H * 2.0;
        clip[i][0] = (float)(nx * depth);
        clip[i][1] = (float)(ny * depth);
        clip[i][2] = (float)(0.5 * depth);
        clip[i][3] = (float)depth;
        for (int k = 0; k < 4; ++k) if (!std::isfinite(clip[i][k])) return false;
    }
    return true;
}

// The follow-head fallback's extra stereo shift (px, + = inward / nearer) for a box at `distM` instead of `refM`: each eye
// sits ipd / 2 beside the head centre, so a point straight ahead at distance d is seen ipd / 2 / d (tangent) towards the nose.
// The fallback's own menu_vr_depth shift stays the base at refM (0.8 m), so fps_vr_dist 80 draws exactly as before.
inline double ExtraShiftPx(double ipdM, double fxPx, double distM, double refM) {
    if (!(ipdM > 0.0) || !(fxPx > 0.0) || !(distM > 0.0) || !(refM > 0.0)) return 0.0;
    return 0.5 * ipdM * fxPx * (1.0 / distM - 1.0 / refM);
}

}  // namespace FpsPose
