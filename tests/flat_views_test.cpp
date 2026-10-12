// v0.12.0 issue #12 unit test of src/flat_views.h (flat multi-view / multi-monitor: 1, 2, 3, 5, 10 views, unequal sizes, stacked): the
// layout from one Present's blit viewports, the view index from the viewport (x then y, not the blit order), the confirm / drop state machine,
// views past the unit cap, and the shared jitter phase of all views of a frame. No D3D, no game. Build: tests/build_flat_views_test.bat (exit 0 = pass).
#include "../src/flat_views.h"
#include <cstdio>
#include <cstdint>
#include <vector>

static int g_checks = 0, g_fails = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fails; printf("   FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

using FlatViews::Rect;
using FlatViews::Layout;
using FlatViews::Detector;

static Rect R(int32_t x, int32_t y, int32_t w, int32_t h) { Rect r; r.x = x; r.y = y; r.w = w; r.h = h; return r; }

// the issue #12 setup: 3 x 1920x1080 side by side in a 5760x1080 backbuffer
static const Rect kL = R(0, 0, 1920, 1080), kC = R(1920, 0, 1920, 1080), kRt = R(3840, 0, 1920, 1080);

// one Present with the given blits (in blit order); returns the event, views[] = what OnBlit returned per blit
static Detector::Event Present(Detector& d, const Rect* b, int n, int* views = nullptr) {
    for (int i = 0; i < n; ++i) {
        const int v = d.OnBlit(b[i]);
        if (views) views[i] = v;
    }
    return d.OnPresent();
}

static void TestMakeLayout() {
    printf("MakeLayout / ViewOf\n");
    Layout L;
    { const Rect b[3] = { kL, kC, kRt };
      CHECK(FlatViews::MakeLayout(b, 3, &L) == FlatViews::kOk, "3 side-by-side viewports are a layout");
      CHECK(L.n == 3 && L.v[0].x == 0 && L.v[1].x == 1920 && L.v[2].x == 3840, "sorted by x");
      CHECK(L.order[0] == 0 && L.order[1] == 1 && L.order[2] == 2, "order = blit order when the blits go left to right"); }
    { const Rect b[3] = { kC, kL, kRt };                     // centre screen drawn first
      CHECK(FlatViews::MakeLayout(b, 3, &L) == FlatViews::kOk, "centre-first order is a layout");
      CHECK(L.v[0].x == 0 && L.v[1].x == 1920 && L.v[2].x == 3840, "views still sorted by x (left = 0)");
      CHECK(L.order[0] == 1 && L.order[1] == 0 && L.order[2] == 2, "order: blit 0 = view 1, blit 1 = view 0, blit 2 = view 2 (got %d %d %d)",
            L.order[0], L.order[1], L.order[2]);
      CHECK(FlatViews::ViewOf(L, kC) == 1 && FlatViews::ViewOf(L, kL) == 0 && FlatViews::ViewOf(L, kRt) == 2, "ViewOf by viewport");
      CHECK(FlatViews::ViewOf(L, R(1921, 0, 1919, 1080)) == 1, "ViewOf within the pixel tolerance (float viewport rounding)");
      CHECK(FlatViews::ViewOf(L, R(0, 0, 5760, 1080)) == -1, "the whole backbuffer is none of the views");
      CHECK(FlatViews::ViewOf(L, R(960, 0, 1920, 1080)) == -1, "a shifted viewport is none of the views"); }
    { const Rect b[3] = { R(0, 0, 1920, 1080), R(1980, 0, 1920, 1080), R(3960, 0, 1920, 1080) };   // bezel compensation gaps
      CHECK(FlatViews::MakeLayout(b, 3, &L) == FlatViews::kOk, "viewports with gaps between them are a layout"); }
    { const Rect b[2] = { kL, kC };
      CHECK(FlatViews::MakeLayout(b, 2, &L) == FlatViews::kOk && L.n == 2, "2 views are a layout"); }
    { const Rect b[1] = { kL };
      CHECK(FlatViews::MakeLayout(b, 1, &L) == FlatViews::kTooFew, "one blit is not a layout"); }
    { Rect b[FlatViews::kMaxBlits + 1];
      for (int i = 0; i <= FlatViews::kMaxBlits; ++i) b[i] = R(i * 100, 0, 100, 100);
      CHECK(FlatViews::MakeLayout(b, FlatViews::kMaxBlits + 1, &L) == FlatViews::kTooMany, "17 blits: more than a layout holds");
      CHECK(FlatViews::MakeLayout(b, FlatViews::kMaxBlits, &L) == FlatViews::kOk && L.n == 16, "16 blits: a layout (past the unit cap)"); }
    { // 5 screens of DIFFERENT widths (a wide centre, narrow outer pair), drawn centre-first: no equal widths / even tiling assumed
      const Rect b[5] = { R(1280, 0, 2560, 1080), R(0, 0, 640, 1080), R(640, 0, 640, 1080), R(3840, 0, 640, 1080), R(4480, 0, 1280, 1080) };
      CHECK(FlatViews::MakeLayout(b, 5, &L) == FlatViews::kOk && L.n == 5, "5 unequal-width views are a layout");
      CHECK(L.v[0].x == 0 && L.v[1].x == 640 && L.v[2].x == 1280 && L.v[3].x == 3840 && L.v[4].x == 4480, "5 views sorted by x");
      CHECK(L.order[0] == 2 && L.order[1] == 0 && L.order[2] == 1 && L.order[3] == 3 && L.order[4] == 4, "5-view order (got %d %d %d %d %d)",
            L.order[0], L.order[1], L.order[2], L.order[3], L.order[4]);
      CHECK(FlatViews::ViewOf(L, b[0]) == 2 && FlatViews::ViewOf(L, b[4]) == 4, "ViewOf on unequal widths"); }
    { // stacked: 2 x 2 (x ties broken by y)
      const Rect b[4] = { R(1920, 1080, 1920, 1080), R(0, 1080, 1920, 1080), R(1920, 0, 1920, 1080), R(0, 0, 1920, 1080) };
      CHECK(FlatViews::MakeLayout(b, 4, &L) == FlatViews::kOk, "a 2 x 2 stacked rig is a layout");
      CHECK(L.v[0].x == 0 && L.v[0].y == 0 && L.v[1].x == 0 && L.v[1].y == 1080 && L.v[2].x == 1920 && L.v[2].y == 0 &&
            L.v[3].x == 1920 && L.v[3].y == 1080, "sorted by x, then y");
      CHECK(L.order[0] == 3 && L.order[1] == 1 && L.order[2] == 2 && L.order[3] == 0, "stacked order (got %d %d %d %d)", L.order[0],
            L.order[1], L.order[2], L.order[3]); }
    { const Rect b[2] = { R(0, 0, 1920, 1080), R(0, 1080, 1920, 1080) };
      CHECK(FlatViews::MakeLayout(b, 2, &L) == FlatViews::kOk && L.v[0].y == 0, "same x, different y: two views, top first"); }
    { const Rect b[2] = { kL, kL };
      CHECK(FlatViews::MakeLayout(b, 2, &L) == FlatViews::kOverlap, "the same viewport twice is not a layout (same view twice)"); }
    { const Rect b[2] = { R(0, 0, 5760, 1080), R(0, 0, 5760, 1080) };
      CHECK(FlatViews::MakeLayout(b, 2, &L) == FlatViews::kOverlap, "two full-backbuffer blits are not a layout"); }
    { const Rect b[2] = { R(0, 0, 2000, 1080), R(1920, 0, 1920, 1080) };
      CHECK(FlatViews::MakeLayout(b, 2, &L) == FlatViews::kOverlap, "overlapping viewports (80 px) are not a layout"); }
    { const Rect b[2] = { R(0, 0, 1921, 1080), R(1920, 0, 1920, 1080) };
      CHECK(FlatViews::MakeLayout(b, 2, &L) == FlatViews::kOk, "1 px overlap is rounding, still a layout"); }
    { const Rect b[2] = { kL, R(1920, 0, 0, 1080) };
      CHECK(FlatViews::MakeLayout(b, 2, &L) == FlatViews::kBadRect, "an empty viewport is not a layout"); }
}

static void TestPlaceBlit() {
    printf("PlaceBlit (round 2: viewport clip + scissor)\n");
    using FlatViews::PlaceBlit;
    using FlatViews::Placement;
    // the first in-game test: full-backbuffer viewport, views confined by the scissor (the hypothesis this round tests)
    Placement p = PlaceBlit(5760, 1080, true, 0.0, 0.0, 5760.0, 1080.0, true, true, 1920, 0, 3840, 1080);
    CHECK(p.eff.x == 1920 && p.eff.y == 0 && p.eff.w == 1920 && p.eff.h == 1080 && (p.flags & FlatViews::kPlScissor),
          "full viewport + scissor (1920..3840) -> the centre screen");
    p = PlaceBlit(5760, 1080, true, 0.0, 0.0, 5760.0, 1080.0, false, true, 1920, 0, 3840, 1080);
    CHECK(p.eff.x == 0 && p.eff.w == 5760 && p.flags == 0, "scissor rect bound but the test off -> the whole viewport");
    p = PlaceBlit(5760, 1080, true, 0.0, 0.0, 5760.0, 1080.0, true, true, 0, 0, 8192, 8192);
    CHECK(p.eff.w == 5760 && !(p.flags & FlatViews::kPlScissor), "a scissor larger than the RT changes nothing");
    p = PlaceBlit(5760, 1080, true, 0.0, 0.0, 5760.0, 1080.0, true, true, 6000, 0, 7000, 1080);
    CHECK(p.eff.w == 5760 && (p.flags & FlatViews::kPlScEmpty), "a scissor outside the viewport is ignored (flagged)");
    p = PlaceBlit(5760, 1080, true, 3840.0, 0.0, 1920.0, 1080.0, false, false, 0, 0, 0, 0);
    CHECK(p.eff.x == 3840 && p.eff.w == 1920 && p.flags == 0, "a per-screen viewport is used as it is");
    p = PlaceBlit(5760, 1080, true, 3840.0, 0.0, 2000.0, 1080.0, false, false, 0, 0, 0, 0);
    CHECK(p.eff.x == 3840 && p.eff.w == 1920 && (p.flags & FlatViews::kPlVpClipped), "a viewport reaching past RT0 is clipped (not the whole RT)");
    p = PlaceBlit(5760, 1080, true, -10.0, 0.0, 1930.0, 1080.0, false, false, 0, 0, 0, 0);
    CHECK(p.eff.x == 0 && p.eff.w == 1920 && (p.flags & FlatViews::kPlVpClipped), "a negative-x viewport is clipped");
    p = PlaceBlit(5760, 1080, true, 6000.0, 0.0, 100.0, 100.0, false, false, 0, 0, 0, 0);
    CHECK(p.eff.x == 0 && p.eff.w == 5760 && (p.flags & FlatViews::kPlVpOutside), "a viewport outside RT0 -> the whole RT (flagged)");
    p = PlaceBlit(5760, 1080, false, 0, 0, 0, 0, false, false, 0, 0, 0, 0);
    CHECK(p.eff.w == 5760 && (p.flags & FlatViews::kPlVpNone), "no viewport -> the whole RT");
    p = PlaceBlit(5760, 1080, true, 1919.6, 0.0, 1920.4, 1080.0, true, true, 1920, 0, 3840, 1080);
    CHECK(p.eff.x == 1920 && p.eff.w == 1920, "float viewport rounding + scissor");
    // three scissored full-screen blits make a 3-view layout through the detector
    Detector d;
    const int sx[3][2] = { { 0, 1920 }, { 1920, 3840 }, { 3840, 5760 } };
    int at = -1;
    for (int pr = 0; pr < 40 && at < 0; ++pr) {
        for (int k = 0; k < 3; ++k)
            d.OnBlit(PlaceBlit(5760, 1080, true, 0.0, 0.0, 5760.0, 1080.0, true, true, sx[k][0], 0, sx[k][1], 1080).eff);
        if (d.OnPresent() == Detector::kConfirmed) at = pr + 1;
    }
    CHECK(at == FlatViews::kConfirmPresents && d.Views() == 3, "scissor-placed views confirm as 3 views");
    // without the scissor (the round-1 rule) the same blits are 3 full-screen rects: not a layout
    Detector d2;
    for (int pr = 0; pr < 40; ++pr) {
        for (int k = 0; k < 3; ++k) d2.OnBlit(PlaceBlit(5760, 1080, true, 0.0, 0.0, 5760.0, 1080.0, false, false, 0, 0, 0, 0).eff);
        d2.OnPresent();
    }
    CHECK(d2.Views() == 1 && d2.LastWhy() == FlatViews::kOverlap, "3 unscissored full-screen blits stay single-view (the round-1 log)");
}

static void TestPhase() {
    printf("PhaseOffset / PhaseOf\n");
    for (int n = 1; n <= FlatViews::kMaxBlits; ++n) {
        for (uint64_t base = 0; base < 7; ++base) {          // the first pass of frame 0 has serial `base`
            int offSeen = -1;
            bool offConst = true, shared = true, steps = true;
            int prevPhase = -1;
            for (int f = 0; f < 40; ++f) {
                int ph = -1;
                for (int k = 0; k < n; ++k) {
                    const uint64_t s = base + (uint64_t)(f * n + k);
                    const int off = FlatViews::PhaseOffset(k, s, n);
                    if (offSeen < 0) offSeen = off; else if (off != offSeen) offConst = false;
                    if (FlatViews::BlitIndexOf(s, offSeen, n) != k) shared = false;   // the pass -> blit index prediction
                    const int p = FlatViews::PhaseOf(s, offSeen, n, 8);
                    if (ph < 0) ph = p; else if (p != ph) shared = false;
                }
                if (prevPhase >= 0 && ph != (prevPhase + 1) % 8) steps = false;
                prevPhase = ph;
            }
            CHECK(offConst, "n=%d base=%llu: the learned offset is the same at every blit", n, (unsigned long long)base);
            CHECK(shared, "n=%d base=%llu: all views of a frame share one phase", n, (unsigned long long)base);
            CHECK(steps, "n=%d base=%llu: the phase steps by one per frame (each view sees all 8 Halton phases in order)", n,
                  (unsigned long long)base);
        }
    }
    // n = 2 is the VR formula: phase = ((s + off) >> 1) % phases
    bool vrSame = true;
    for (uint64_t s = 0; s < 64; ++s)
        for (int off = 0; off < 2; ++off)
            if (FlatViews::PhaseOf(s, off, 2, 8) != (int)(((s + (uint64_t)off) >> 1) % 8)) vrSame = false;
    CHECK(vrSame, "n=2 matches the VR phase-parity formula");
    // n = 1 is the single-view formula: phase = s % phases
    bool flatSame = true;
    for (uint64_t s = 0; s < 64; ++s) if (FlatViews::PhaseOf(s, 0, 1, 8) != (int)(s % 8)) flatSame = false;
    CHECK(flatSame, "n=1 matches the single-view formula");
    // a wrong offset still steps every view through all phases (it only splits the frame across two phases)
    bool wrongOk = true;
    for (int k = 0; k < 3; ++k) {
        int prev = -1;
        for (int f = 0; f < 24; ++f) {
            const int p = FlatViews::PhaseOf((uint64_t)(f * 3 + k), 2, 3, 8);
            if (prev >= 0 && p != (prev + 1) % 8) wrongOk = false;
            prev = p;
        }
    }
    CHECK(wrongOk, "with a wrong offset each view still advances one phase per frame");
}

static void TestDetectorSingle() {
    printf("Detector: single screen\n");
    Detector d;
    const Rect full = R(0, 0, 2560, 1440);
    bool ok = true;
    for (int p = 0; p < 200; ++p) {
        int v = -9;
        if (Present(d, &full, 1, &v) != Detector::kNone) ok = false;
        if (v != 0) ok = false;
    }
    CHECK(ok, "one blit per Present: no event, every blit is view 0");
    CHECK(d.Views() == 1 && d.FifoDepth() == 1 && !d.MultiSeen(), "stays single-view (FIFO depth 1, not multi)");
    // a single screen with two identical blits (the same view twice) never becomes multi-view
    Detector d2;
    const Rect two[2] = { full, full };
    int notDistinct = 0, confirms = 0;
    for (int p = 0; p < 100; ++p) {
        const Detector::Event e = Present(d2, two, 2);
        if (e == Detector::kNotDistinct) ++notDistinct;
        if (e == Detector::kConfirmed) ++confirms;
    }
    CHECK(notDistinct == 1 && confirms == 0 && d2.Views() == 1, "two blits at the same viewport: one 'not distinct' note, no confirm");
}

static void TestDetectorTriple() {
    printf("Detector: triple screen confirm / map / drop\n");
    Detector d;
    const Rect lcr[3] = { kL, kC, kRt };
    for (int p = 0; p < FlatViews::kConfirmPresents - 1; ++p) {
        int v[3];
        CHECK(Present(d, lcr, 3, v) == Detector::kNone, "Present %d: no event before the confirm", p);
        CHECK(v[0] == 0 && v[1] == 0 && v[2] == 0, "Present %d: before the confirm every blit is view 0 (old behaviour)", p);
        CHECK(d.MultiSeen() && d.FifoDepth() == 3, "Present %d: candidate seen -> FIFO depth 3", p);
    }
    CHECK(Present(d, lcr, 3) == Detector::kConfirmed, "confirmed at Present kConfirmPresents");
    CHECK(d.Views() == 3 && d.FifoDepth() == 3 && d.MultiSeen(), "3 views");
    { int v[3]; Present(d, lcr, 3, v);
      CHECK(v[0] == 0 && v[1] == 1 && v[2] == 2, "left / centre / right -> views 0 / 1 / 2 (got %d %d %d)", v[0], v[1], v[2]);
      CHECK(d.OrderMismatch() == 0, "no order mismatch"); }
    // centre drawn first from now on: the views follow the viewport, not the order; one order-changed note
    const Rect clr[3] = { kC, kL, kRt };
    { int v[3]; const Detector::Event e = Present(d, clr, 3, v);
      CHECK(v[0] == 1 && v[1] == 0 && v[2] == 2, "centre-first blits -> views 1 / 0 / 2 (got %d %d %d)", v[0], v[1], v[2]);
      CHECK(e == Detector::kOrderChanged, "order change noted");
      CHECK(d.OrderMismatch() == 2, "the 2 blits whose order disagreed are counted (got %llu)", (unsigned long long)d.OrderMismatch()); }
    { int v[3]; const Detector::Event e = Present(d, clr, 3, v);
      CHECK(e == Detector::kNone && v[0] == 1 && v[1] == 0 && v[2] == 2, "the new order is the layout's order now"); }
    // empty Presents (menu, loading, a present layer's extra Present) are neutral
    for (int p = 0; p < 50; ++p) CHECK(Present(d, nullptr, 0) == Detector::kNone, "empty Present %d neutral", p);
    CHECK(d.Views() == 3, "still 3 views after empty Presents");
    // a blit outside the layout: -1 (left alone by the caller), counted
    { const Rect odd[3] = { kL, kC, R(0, 0, 5760, 1080) };
      int v[3]; Present(d, odd, 3, v);
      CHECK(v[2] == -1 && d.Unmapped() == 1, "a full-width blit is not one of the views (-1, counted)"); }
    CHECK(Present(d, lcr, 3) == Detector::kOrderChanged || d.Views() == 3, "a good Present resets the miss count");
    // back to one screen: dropped after kDropPresents Presents in a row
    const Rect full = R(0, 0, 5760, 1080);
    for (int p = 0; p < FlatViews::kDropPresents - 1; ++p) {
        int v = 0;
        CHECK(Present(d, &full, 1, &v) == Detector::kNone, "miss %d: no drop yet", p);
        CHECK(v == -1, "miss %d: the full-width blit is not a view while 3 views are confirmed", p);
    }
    CHECK(Present(d, &full, 1) == Detector::kDropped, "dropped at miss kDropPresents");
    CHECK(d.Views() == 1 && d.PrevViews() == 3 && !d.MultiSeen() && d.FifoDepth() == 1, "back to single view");
    { int v = -9; Present(d, &full, 1, &v); CHECK(v == 0, "single view again: view 0"); }
}

static void TestDetectorInterrupt() {
    printf("Detector: interrupted confirm, layout change, 2 views, too many blits\n");
    Detector d;
    const Rect lcr[3] = { kL, kC, kRt };
    const Rect one = R(0, 0, 5760, 1080);
    for (int p = 0; p < 20; ++p) Present(d, lcr, 3);
    Present(d, &one, 1);                                      // one single-blit Present breaks the run
    CHECK(d.Stable() == 0 && !d.MultiSeen(), "a single-blit Present resets the confirm run");
    int at = -1;
    for (int p = 0; p < 40 && at < 0; ++p) if (Present(d, lcr, 3) == Detector::kConfirmed) at = p + 1;
    CHECK(at == FlatViews::kConfirmPresents, "confirm needs kConfirmPresents in a row again (got %d)", at);
    // a missing-then-back layout inside the drop window keeps the views
    for (int p = 0; p < FlatViews::kDropPresents - 1; ++p) Present(d, &one, 1);
    Present(d, lcr, 3);
    for (int p = 0; p < FlatViews::kDropPresents - 1; ++p) Present(d, &one, 1);
    CHECK(d.Views() == 3, "misses interrupted by a good Present never drop");
    // a different 3-view layout (another resolution): drop, then confirm the new one
    const Rect big[3] = { R(0, 0, 2560, 1440), R(2560, 0, 2560, 1440), R(5120, 0, 2560, 1440) };
    int drops = 0, confirms = 0;
    for (int p = 0; p < FlatViews::kDropPresents + FlatViews::kConfirmPresents + 5; ++p) {
        const Detector::Event e = Present(d, big, 3);
        if (e == Detector::kDropped) ++drops;
        if (e == Detector::kConfirmed) confirms++;
    }
    CHECK(drops == 1 && confirms == 1 && d.Views() == 3 && d.Current().v[1].x == 2560, "new layout: dropped once, confirmed once");
    // 2 views
    Detector d2;
    const Rect lr[2] = { R(0, 0, 1920, 1080), R(1920, 0, 1920, 1080) };
    for (int p = 0; p < FlatViews::kConfirmPresents; ++p) Present(d2, lr, 2);
    { int v[2]; const Rect rl[2] = { lr[1], lr[0] }; Present(d2, rl, 2, v);
      CHECK(d2.Views() == 2 && v[0] == 1 && v[1] == 0, "2-view layout, right-first blits -> views 1 / 0"); }
    // more blits than kMaxBlits in one Present never confirms
    Detector d3;
    Rect many[20];
    for (int i = 0; i < 20; ++i) many[i] = R(i * 100, 0, 100, 100);
    for (int p = 0; p < 40; ++p) Present(d3, many, 20);
    CHECK(d3.Views() == 1 && d3.LastWhy() == FlatViews::kTooMany, "20 blits per Present: not a layout");
}

static void TestDetectorFiveAndCap() {
    printf("Detector: 5 unequal views, views past the cap\n");
    // 5 screens of different widths and heights, centre drawn first
    const Rect five[5] = { R(1280, 0, 2560, 1440), R(0, 180, 640, 1080), R(640, 180, 640, 1080), R(3840, 180, 640, 1080), R(4480, 0, 1280, 1440) };
    Detector d;
    int at = -1;
    for (int p = 0; p < 40 && at < 0; ++p) if (Present(d, five, 5) == Detector::kConfirmed) at = p + 1;
    CHECK(at == FlatViews::kConfirmPresents && d.Views() == 5 && d.DlaaViews() == 5 && d.FifoDepth() == 5, "5 views confirmed");
    { int v[5]; Present(d, five, 5, v);
      CHECK(v[0] == 2 && v[1] == 0 && v[2] == 1 && v[3] == 3 && v[4] == 4, "5 views by position (got %d %d %d %d %d)", v[0], v[1], v[2],
            v[3], v[4]);
      for (int k = 0; k < 5; ++k) CHECK(d.ViewOfBlitIndex(k) == v[k], "ViewOfBlitIndex(%d)", k); }
    // 10 views (past the 8-unit cap): views 8 and 9 (the two rightmost) -> kBeyondCap, the rest mapped
    Rect ten[10];
    for (int i = 0; i < 10; ++i) ten[9 - i] = R(i * 576, 0, 576, 1080);   // drawn right to left
    Detector d2;
    for (int p = 0; p < FlatViews::kConfirmPresents; ++p) Present(d2, ten, 10);
    CHECK(d2.Views() == 10 && d2.DlaaViews() == FlatViews::kMaxViews && d2.FifoDepth() == 10, "10 views: %d with a unit", d2.DlaaViews());
    { int v[10]; Present(d2, ten, 10, v);
      bool ok = true;
      for (int k = 0; k < 10; ++k) {
          const int pos = 9 - k;                           // blit k is at x = pos * 576
          const int want = pos >= FlatViews::kMaxViews ? FlatViews::kBeyondCap : pos;
          if (v[k] != want) ok = false;
      }
      CHECK(ok, "10 views: positions 0..7 mapped, 8 / 9 past the cap (got %d %d %d ...)", v[0], v[1], v[2]);
      CHECK(d2.BeyondCap() == 2 && d2.Unmapped() == 0, "2 blits past the cap per Present, none unmapped"); }
    // 1 view through the same API: nothing changes
    Detector d1;
    const Rect one = R(0, 0, 1920, 1080);
    for (int p = 0; p < 100; ++p) Present(d1, &one, 1);
    CHECK(d1.Views() == 1 && d1.DlaaViews() == 1 && d1.ViewOfBlitIndex(0) == -1, "1 view: single view");
}

// ---- round 5: blit <-> pass pairing (PickPass / AdvanceHead / PermLearner) ------------------------------------------------------
// A frame simulator with inject.cpp's bookkeeping: passes start (StartPass: framePos, frame), a tonemap draw reads a pass's forward
// colour into an LDR RT (FvNoteTonemap: the NEWEST unconsumed pass with that forward colour takes the RT, every other pass loses it),
// a blit samples an RT and picks its pass (PickPass), consumes it and advances the head (AdvanceHead). Passes run in `passView`
// order (pass i of the frame renders view passView[i]); they are processed in chunks of `chunk` passes: the chunk's passes, then their
// tonemaps (pass order), then their blits in a scrambled order. Forward colours and LDR RTs come from pools of `chunk` textures
// re-used by every chunk (ATS: pass R re-uses pass L's forward colour and tonemap RT inside one frame).
struct SimPass { uint64_t s; bool consumed; uint64_t frame; int pos; const void* toneRt; const void* fwd; int view; };
struct PairSim {
    std::vector<SimPass> ring;                   // indexed by serial (no wrap needed in a test)
    uint64_t head = 0, seq = 0;
    int maxOutstanding = 0;
    uint64_t picks[4] = {}, wrong = 0;
    FlatViews::PermLearner perm;
    char pool[64];                               // texture identities: &pool[i]
    void Tonemap(const void* fwd, const void* rt) {
        SimPass* owner = nullptr;
        for (uint64_t q = seq; q-- > head;) if (!ring[q].consumed && ring[q].fwd == fwd) { owner = &ring[q]; break; }
        for (uint64_t q = head; q < seq; ++q) if (&ring[q] != owner && ring[q].toneRt == rt) ring[q].toneRt = nullptr;
        if (owner) owner->toneRt = rt;
    }
    // one frame; dropLink = blit indices (bitmask) whose tonemap link is "missed" (SRV identity unknown: src = nullptr)
    void Frame(uint64_t f, int n, const int* passView, const int* blitOfChunk, int chunk, uint32_t dropLink) {
        int blitK = 0;
        for (int c0 = 0; c0 < n; c0 += chunk) {
            const int cn = n - c0 < chunk ? n - c0 : chunk;
            for (int i = 0; i < cn; ++i) {        // passes of the chunk
                SimPass p{ seq, false, f, c0 + i, nullptr, &pool[i], passView[c0 + i] };
                ring.push_back(p); ++seq;
            }
            const int out = (int)(seq - head);
            if (out > maxOutstanding) maxOutstanding = out;
            for (int i = 0; i < cn; ++i) Tonemap(&pool[i], &pool[32 + i]);   // tonemaps, pass order, RT i of the pool
            for (int b = 0; b < cn; ++b, ++blitK) {   // blits, scrambled inside the chunk
                bool fits = true;                    // a short last chunk: the blits go in pass order there
                for (int bb = 0; bb < cn; ++bb) if (blitOfChunk[bb] >= cn) fits = false;
                const int i = fits ? blitOfChunk[b] : b;   // which pass of the chunk this blit shows
                const void* src = (dropLink >> blitK) & 1u ? nullptr : (const void*)&pool[32 + i];
                FlatViews::PassInfo win[64];
                int wn = 0;
                for (uint64_t q = head; q < seq && wn < 64; ++q, ++wn) {
                    win[wn].s = ring[q].s; win[wn].consumed = ring[q].consumed; win[wn].frame = ring[q].frame;
                    win[wn].pos = ring[q].pos; win[wn].toneRt = ring[q].toneRt;
                }
                int how = 0;
                const int w = FlatViews::PickPass(win, wn, src, f, perm.PosOfBlit(blitK), &how);
                ++picks[how];
                if (w < 0) { ++wrong; continue; }
                SimPass& took = ring[head + (uint64_t)w];
                if (took.view != passView[c0 + i]) ++wrong;      // the blit shows view passView[c0 + i]
                if (how == FlatViews::kPickLink) perm.Note(blitK, took.pos);
                took.consumed = true;
                head = FlatViews::AdvanceHead(head, seq, [&](uint64_t q) { return ring[q].consumed; });
            }
        }
        perm.OnFrame(n);
    }
};

static void TestPairing(const char* name, int n, const int* passView, const int* blitOfChunk, int chunk) {
    printf("Pairing: %s (%d views, chunks of %d)\n", name, n, chunk);
    PairSim sim;
    for (uint64_t f = 1; f <= 20; ++f) sim.Frame(f, n, passView, blitOfChunk, chunk, 0u);
    CHECK(sim.wrong == 0, "%s: every blit took the pass of its own view by the tonemap link (wrong %llu)", name, (unsigned long long)sim.wrong);
    CHECK(sim.picks[FlatViews::kPickLink] == (uint64_t)(20 * n), "%s: all %d blits linked (got %llu)", name, 20 * n,
          (unsigned long long)sim.picks[FlatViews::kPickLink]);
    CHECK(sim.head == sim.seq, "%s: nothing left outstanding at the frame end", name);
    CHECK(sim.maxOutstanding == chunk, "%s: %d passes outstanding at once (got %d) -- the ring needs views + 1 slots", name, chunk,
          sim.maxOutstanding);
    CHECK(sim.perm.Known(), "%s: the pass order became known", name);
    // the FIFO would have been wrong whenever the blit order differs from the pass order inside a chunk
    // links missing from now on: the learned permutation must pick the same passes
    PairSim& s2 = sim;
    const uint64_t perm0 = s2.picks[FlatViews::kPickPerm];
    for (uint64_t f = 21; f <= 30; ++f) s2.Frame(f, n, passView, blitOfChunk, chunk, 0xFFFFu);
    CHECK(s2.wrong == 0, "%s: without links the learned order still pairs every blit right (wrong %llu)", name, (unsigned long long)s2.wrong);
    CHECK(s2.picks[FlatViews::kPickPerm] - perm0 == (uint64_t)(10 * n), "%s: %d blits by the learned order", name, 10 * n);
    // and with neither: FIFO (wrong for scrambled orders -- what the old code always did)
    PairSim s3;
    for (uint64_t f = 1; f <= 5; ++f) s3.Frame(f, n, passView, blitOfChunk, chunk, 0xFFFFu);
    bool scrambled = false;
    for (int b = 0; b < chunk && b < n; ++b) if (blitOfChunk[b] != b) scrambled = true;
    CHECK(s3.picks[FlatViews::kPickFifo] == (uint64_t)(5 * n) && (!scrambled || s3.wrong > 0),
          "%s: no link, no learned order -> FIFO (%llu wrong pairs, the round-1..4 bug when the orders differ)", name,
          (unsigned long long)s3.wrong);
}

static void TestPairingAll() {
    // ATS (RenderDoc frame 1176): passes L C R; chunk 1 = passes L, C, tonemaps L, C, blits C, L; chunk 2 = pass R, tonemap, blit R
    { const int pv[3] = { 0, 1, 2 }; const int bo[2] = { 1, 0 }; TestPairing("ATS triple (passes L C R, blits C L R)", 3, pv, bo, 2); }
    { const int pv[3] = { 0, 1, 2 }; const int bo[3] = { 0, 1, 2 }; TestPairing("pass, blit, pass, blit (chunk 1)", 3, pv, bo, 1); }
    { const int pv[5] = { 2, 0, 4, 1, 3 }; const int bo[5] = { 3, 0, 4, 2, 1 }; TestPairing("5 views, scrambled, all passes first", 5, pv, bo, 5); }
    { const int pv[5] = { 4, 3, 2, 1, 0 }; const int bo[2] = { 1, 0 }; TestPairing("5 views, chunks of 2, RTs re-used", 5, pv, bo, 2); }
    { const int pv[8] = { 7, 2, 5, 0, 3, 6, 1, 4 }; const int bo[8] = { 5, 2, 7, 0, 6, 1, 3, 4 };
      TestPairing("8 views, scrambled, all passes first", 8, pv, bo, 8); }
    { const int pv[8] = { 0, 1, 2, 3, 4, 5, 6, 7 }; const int bo[3] = { 2, 0, 1 }; TestPairing("8 views, chunks of 3, RTs re-used", 8, pv, bo, 3); }
    // AdvanceHead with a gap: passes 0..3, 1 and 2 consumed first -> head stays 0; 0 consumed -> head jumps to 3
    { bool c[4] = { false, true, true, false };
      auto f = [&](uint64_t q) { return c[q]; };
      CHECK(FlatViews::AdvanceHead(0, 4, f) == 0, "AdvanceHead: an older pass still outstanding holds the head");
      c[0] = true;
      CHECK(FlatViews::AdvanceHead(0, 4, f) == 3, "AdvanceHead: then it jumps past every consumed pass");
      c[3] = true;
      CHECK(FlatViews::AdvanceHead(0, 4, f) == 4, "AdvanceHead: up to seq"); }
    // PermLearner: incomplete frames do not count, a change restarts the count
    { FlatViews::PermLearner p;
      for (int f = 0; f < FlatViews::kPermConfirm - 1; ++f) { p.Note(0, 1); p.Note(1, 0); p.Note(2, 2); p.OnFrame(3); }
      CHECK(!p.Known(), "PermLearner: not known before kPermConfirm frames");
      p.Note(0, 1); p.Note(1, 0); p.OnFrame(3);          // incomplete
      CHECK(!p.Known(), "PermLearner: an incomplete frame does not count");
      p.Note(0, 1); p.Note(1, 0); p.Note(2, 2);
      CHECK(p.OnFrame(3) && p.Known() && p.PosOfBlit(0) == 1 && p.BlitOfPos(1) == 0 && p.BlitOfPos(2) == 2, "PermLearner: known");
      p.Note(0, 0); p.Note(1, 0); p.Note(2, 2); p.OnFrame(3);   // not a permutation: ignored
      CHECK(p.Known(), "PermLearner: a non-permutation frame is ignored");
      p.Note(0, 2); p.Note(1, 0); p.Note(2, 1); p.OnFrame(3);   // a different permutation
      CHECK(!p.Known() && p.Changes() == 1, "PermLearner: a different complete frame restarts the count"); }
}

int main() {
    TestPairingAll();
    TestMakeLayout();
    TestPlaceBlit();
    TestPhase();
    TestDetectorSingle();
    TestDetectorTriple();
    TestDetectorInterrupt();
    TestDetectorFiveAndCap();
    printf("\nflat_views_test: %d checks, %d failed -> %s\n", g_checks, g_fails, g_fails ? "FAIL" : "PASS");
    return g_fails ? 1 : 0;
}
