// Phase 44 unit test of src/fps_pose.h: the fps box colour presets, the turn quaternion (fps_vr_yaw / fps_vr_pitch /
// fps_vr_roll) and its sign conventions, the drawn-in fallback's turned corners and the follow-head distance shift.
// No D3D, no game. Build: tests\build_fps_pose_test.bat. Exit code 0 = every check passed.
#include "../src/fps_pose.h"
#include <cstdio>
#include <cmath>
#include <cwchar>

static int g_checks = 0, g_fails = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fails; printf("   FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool Near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

// corner i of TurnedCorners back to picture px
static void Px(const float c[4], double W, double H, double* x, double* y) {
    *x = ((double)c[0] / c[3] + 1.0) * 0.5 * W;
    *y = (1.0 - (double)c[1] / c[3]) * 0.5 * H;
}

int main() {
    using namespace FpsPose;
    // ---- colours
    {
        const Color& w = ColorOf(0);
        CHECK(w.tr == 255 && w.tg == 255 && w.tb == 255 && w.lr == 8 && w.lg == 8 && w.lb == 8, "preset 0 must be today's white / RGB 8 outline");
        for (int i = 0; i < kColorCount; ++i) {
            CHECK(ColorOf(i).name && ColorOf(i).name[0], "preset %d has no name", i);
            CHECK(wcslen(ColorOf(i).name) < 40, "preset %d name too long for a menu value", i);
            for (int j = 0; j < i; ++j) CHECK(wcscmp(ColorOf(i).name, ColorOf(j).name) != 0, "presets %d and %d share a name", i, j);
            // readable: the text differs from its outline by a clear luminance gap
            const Color& c = ColorOf(i);
            const double lt = 0.2126 * c.tr + 0.7152 * c.tg + 0.0722 * c.tb, ll = 0.2126 * c.lr + 0.7152 * c.lg + 0.0722 * c.lb;
            CHECK(std::fabs(lt - ll) > 80.0, "preset %d: text / outline luminance gap %.0f too small", i, std::fabs(lt - ll));
        }
        CHECK(ColorOf(-3).tr == 255 && ColorOf(99).tr == 255, "out-of-range presets fall back to 0");
        CHECK(StepColor(19, 1) == 0 && StepColor(0, -1) == 19 && StepColor(5, 1) == 6, "StepColor wraps");
        CHECK(ClampColor(-1) == 0 && ClampColor(25) == 19 && ClampColor(7) == 7, "ClampColor");
    }
    // ---- opacity (phase 51f)
    {
        CHECK(ClampOpacity(0) == 10 && ClampOpacity(5) == 10 && ClampOpacity(55) == 55 && ClampOpacity(500) == 100, "ClampOpacity 10..100");
        CHECK(StepOpacity(100, 1) == 100 && StepOpacity(100, -1) == 90 && StepOpacity(10, -1) == 10 && StepOpacity(10, 1) == 20, "StepOpacity 10 %% steps, clamped");
    }
    // ---- brightness (phase 48)
    {
        CHECK(ClampBright(0) == 10 && ClampBright(5) == 10 && ClampBright(55) == 55 && ClampBright(500) == 100, "ClampBright 10..100");
        CHECK(StepBright(100, 1) == 100 && StepBright(100, -1) == 90 && StepBright(10, -1) == 10 && StepBright(10, 1) == 20, "StepBright 10 %% steps, clamped");
        for (int i = 0; i < kColorCount; ++i) {
            const Color& c = ColorOf(i);
            const Color d = Dimmed(c, 100);
            CHECK(d.tr == c.tr && d.tg == c.tg && d.tb == c.tb && d.lr == c.lr && d.lg == c.lg && d.lb == c.lb, "preset %d: 100 %% = unchanged", i);
            const Color h = Dimmed(c, 50);
            CHECK(i == 19 || (h.lr == c.lr && h.lg == c.lg && h.lb == c.lb), "preset %d: the outline stays", i);
            const Color m = Dimmed(c, 10);
            CHECK(i == 19 || (m.tr <= (c.tr + 9) / 10 + 1 && m.tg <= (c.tg + 9) / 10 + 1 && m.tb <= (c.tb + 9) / 10 + 1), "preset %d: 10 %% is dark", i);
        }
        const Color w = Dimmed(ColorOf(0), 50);
        CHECK(w.tr == 128 && w.tg == 128 && w.tb == 128, "white at 50 %% = 128, got %d", w.tr);
        const Color bw = Dimmed(ColorOf(19), 50);
        CHECK(bw.tr == 16 && bw.lr == 128 && bw.lg == 128 && bw.lb == 128, "black on white: the white outline dims, text stays");
        CHECK(Dimmed(ColorOf(2), 10).tr == 26 && Dimmed(ColorOf(2), 10).tg == 19 && Dimmed(ColorOf(2), 10).tb == 0, "amber at 10 %%");
    }
    // ---- turn quaternion conventions
    {
        double q[4];
        TurnQuat(0, 0, 0, q);
        CHECK(Near(q[0], 0, 1e-12) && Near(q[1], 0, 1e-12) && Near(q[2], 0, 1e-12) && Near(q[3], 1, 1e-12), "0/0/0 = identity");
        const double z[3] = { 0, 0, 1 }, y[3] = { 0, 1, 0 }, x[3] = { 1, 0, 0 };
        double o[3];
        TurnQuat(90, 0, 0, q); QRotate(q, z, o);
        CHECK(Near(o[0], 1, 1e-9) && Near(o[2], 0, 1e-9), "yaw +90: the front (+z) turns to +x (right), got (%.3f %.3f %.3f)", o[0], o[1], o[2]);
        QRotate(q, x, o);
        CHECK(Near(o[2], -1, 1e-9), "yaw +90: the right edge goes away (-z)");
        TurnQuat(0, 90, 0, q); QRotate(q, z, o);
        CHECK(Near(o[1], 1, 1e-9), "pitch +90: the front faces up, got (%.3f %.3f %.3f)", o[0], o[1], o[2]);
        QRotate(q, y, o);
        CHECK(Near(o[2], -1, 1e-9), "pitch +90: the top edge goes away (-z)");
        TurnQuat(0, 0, 90, q); QRotate(q, x, o);
        CHECK(Near(o[1], -1, 1e-9), "roll +90: +x goes down = clockwise seen from the front, got (%.3f %.3f %.3f)", o[0], o[1], o[2]);
        // composition order: yaw applied in the frame AFTER... local turn = Ry * Rx * Rz: a pitched box then yawed keeps its yaw axis vertical
        TurnQuat(30, 20, 10, q);
        const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        CHECK(Near(n, 1, 1e-12), "TurnQuat is unit");
        double base[4] = { 0, std::sin(0.3), 0, std::cos(0.3) }, c[4], t[4], m[4];
        ComposeTurn(base, 30, 20, 10, c);
        QMul(base, q, m);
        for (int k = 0; k < 4; ++k) t[k] = m[k];
        CHECK(Near(c[0], t[0], 1e-12) && Near(c[1], t[1], 1e-12) && Near(c[2], t[2], 1e-12) && Near(c[3], t[3], 1e-12), "ComposeTurn = base * turn");
        const double ident[4] = { 0, 0, 0, 1 };
        ComposeTurn(ident, 0, 0, 0, c);
        CHECK(Near(c[3], 1, 1e-12), "ComposeTurn with 0/0/0 keeps the base");
    }
    // ---- drawn-in corners
    {
        const double W = 4000, H = 4200, u0 = 1700, v0 = 2000, fx = 1500, fy = 1500, cx = 1900, cy = 1500, bw = 300, bh = 140;
        float c[4][4];
        CHECK(TurnedCorners(W, H, u0, v0, fx, fy, cx, cy, bw, bh, 0, 0, 0, 0.0, c), "flat corners");
        const double ex[4] = { cx - bw / 2, cx + bw / 2, cx - bw / 2, cx + bw / 2 }, ey[4] = { cy - bh / 2, cy - bh / 2, cy + bh / 2, cy + bh / 2 };
        for (int i = 0; i < 4; ++i) {
            double px, py;
            Px(c[i], W, H, &px, &py);
            CHECK(Near(px, ex[i], 1e-3) && Near(py, ey[i], 1e-3), "0/0/0 corner %d = the flat box (%.3f %.3f vs %.3f %.3f)", i, px, py, ex[i], ey[i]);
            CHECK(Near(c[i][2], 0.5 * c[i][3], 1e-6) && c[i][3] > 0, "corner %d: z = 0.5 w, w > 0", i);
        }
        // shift moves every corner
        float cs[4][4];
        TurnedCorners(W, H, u0, v0, fx, fy, cx, cy, bw, bh, 0, 0, 0, 12.0, cs);
        double a1, b1, a2, b2;
        Px(c[0], W, H, &a1, &b1); Px(cs[0], W, H, &a2, &b2);
        CHECK(Near(a2 - a1, 12.0, 1e-3) && Near(b2, b1, 1e-3), "shift adds px to x");
        // roll +90 (clockwise): the top-left corner goes to the top-right of the centre
        CHECK(TurnedCorners(W, H, u0, v0, fx, fy, cx, cy, bw, bh, 0, 0, 90, 0.0, c), "roll corners");
        double px, py;
        Px(c[0], W, H, &px, &py);
        CHECK(px > cx && py < cy, "roll +90: top-left corner -> upper right (%.1f %.1f)", px, py);
        double p1x, p1y, p3x, p3y;
        Px(c[1], W, H, &p1x, &p1y); Px(c[3], W, H, &p3x, &p3y);
        CHECK(Near(std::hypot(p1x - px, p1y - py), bw, 2.0) && std::fabs(p1x - px) < 2.0 && p1y > py,
              "roll +90: the texture's top edge (bw long) now runs DOWN the right side (%.1f, %.1f)", p1x - px, p1y - py);
        // yaw +40: the right edge is farther -> shorter than the left edge
        CHECK(TurnedCorners(W, H, u0, v0, fx, fy, cx, cy, bw, bh, 40, 0, 0, 0.0, c), "yaw corners");
        double l0x, l0y, l2x, l2y, r1x, r1y, r3x, r3y;
        Px(c[0], W, H, &l0x, &l0y); Px(c[2], W, H, &l2x, &l2y); Px(c[1], W, H, &r1x, &r1y); Px(c[3], W, H, &r3x, &r3y);
        CHECK((r3y - r1y) < (l2y - l0y), "yaw +40: right edge %.1f px < left edge %.1f px", r3y - r1y, l2y - l0y);
        CHECK((r1x - l0x) < bw, "yaw +40: narrower than flat");
        // pitch +40: the top edge is farther -> shorter than the bottom edge
        CHECK(TurnedCorners(W, H, u0, v0, fx, fy, cx, cy, bw, bh, 0, 40, 0, 0.0, c), "pitch corners");
        Px(c[0], W, H, &l0x, &l0y); Px(c[1], W, H, &r1x, &r1y); Px(c[2], W, H, &l2x, &l2y); Px(c[3], W, H, &r3x, &r3y);
        CHECK((r1x - l0x) < (r3x - l2x), "pitch +40: top edge %.1f px < bottom edge %.1f px", r1x - l0x, r3x - l2x);
        // edge-on: refused
        CHECK(!TurnedCorners(W, H, u0, v0, fx, fy, cx, cy, 1e6, bh, 90, 0, 0, 0.0, c), "a corner behind the eye is refused");
        CHECK(!TurnedCorners(W, H, u0, v0, 0.0, fy, cx, cy, bw, bh, 0, 0, 0, 0.0, c), "a bad pinhole is refused");
    }
    // ---- follow-head distance shift
    {
        CHECK(Near(ExtraShiftPx(0.064, 1500, 0.8, 0.8), 0.0, 1e-12), "80 cm = no extra shift (as before)");
        CHECK(ExtraShiftPx(0.064, 1500, 0.4, 0.8) > 0.0, "nearer = more inward");
        CHECK(ExtraShiftPx(0.064, 1500, 2.0, 0.8) < 0.0, "farther = less inward");
        CHECK(Near(ExtraShiftPx(0.064, 1500, 0.4, 0.8), 0.032 * 1500 * (2.5 - 1.25), 1e-9), "the shift value");
        CHECK(ExtraShiftPx(0.0, 1500, 0.4, 0.8) == 0.0, "no ipd = no shift");
    }
    // ---- ranges
    CHECK(ClampI(10, kDistMin, kDistMax) == 30 && ClampI(999, kDistMin, kDistMax) == 300 && kDistDefault == 80, "distance range");
    printf("fps_pose_test: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
