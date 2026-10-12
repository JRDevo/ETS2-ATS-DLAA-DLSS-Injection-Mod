// v0.12.0 issue #12: FLAT MULTI-VIEW (multi-monitor, dlaa.ini flat_multiview, default 1; 0 = the v0.11 behaviour exactly).
//
// WHY: ATS / ETS2 with r_multimon_mode 2 (multimon_config.sii) on a flat multi-monitor rig render ONE scene pass + ONE
// scene->backbuffer blit per monitor view per Present (issue #12: 3 x 1920x1080 = a 5760x1080 backbuffer, hFOV 50 deg per view,
// blit viewports at x = 0 / 1920 / 3840; 5-screen rigs exist, and the views may differ in size / aspect). Before v0.12 flat meant
// ONE view: every backbuffer blit was eye 0, so the one DLAA unit got the left, centre and right camera in turn -- its MV history
// paired passes 50 deg apart (the reporter's log: "R |x|>0.25: e0=33026/33053", camera R = a 50 deg yaw) and DLAA looked worse
// than off.
// HOW: inject.cpp HandlePossibleBlit feeds every matched flat backbuffer blit's viewport rect to Detector::OnBlit; OnPresentBoundary
// calls Detector::OnPresent. A Present whose 2..kMaxBlits blits have NON-OVERLAPPING viewports is a LAYOUT (no equal sizes, no
// even tiling, no single row assumed); the same layout in kConfirmPresents Presents in a row CONFIRMS it (views = its blit count),
// and from then on a blit's VIEW INDEX = the position of its viewport in the layout sorted by x, then y (stacked rigs) -- not the
// blit order (the order is tracked as a sanity check and logged when it changes). kDropPresents non-empty Presents in a row
// without that layout fall back to one view. Presents without any matched blit (menus, loading screens, a present layer's extra
// Presents) are neutral.
// Views 0..kMaxViews-1 each get their own DLAA unit (g_dlaa[view]: history, MV / camera / per-draw pairing state, reset bit,
// record classification), exactly like the two VR eyes; a layout with MORE views than kMaxViews keeps the first kMaxViews (by
// position) and the rest pass through untouched (OnBlit returns kBeyondCap; logged once at the confirm). The scene pass -> view
// mapping is the pass FIFO (blit k of a Present consumes the oldest unconsumed pass, as in VR), and the Halton phase advances
// once per frame for all views (PhaseOf: (s + off) / n, the VR ">> 1" generalised; off learned from the blit order inside the
// Present, PhaseOffset).
// tests/flat_views_test.cpp checks this file (no D3D).
#pragma once
#include <cstdint>

namespace FlatViews {

constexpr int kMaxViews        = 8;    // views with their own DLAA unit (the per-view arrays in inject.cpp; VR uses 2 of them)
constexpr int kMaxBlits        = 16;   // views a layout may have (blits remembered per Present; beyond kMaxViews: pass-through)
constexpr int kConfirmPresents = 30;   // the same layout in this many Presents in a row -> confirmed
constexpr int kDropPresents    = 10;   // confirmed: this many non-empty Presents in a row without it -> back to one view
constexpr int32_t kTolPx       = 2;    // viewport rects compare within this many pixels (rounding of float viewports)
constexpr int kBeyondCap       = -2;   // OnBlit: a view of the confirmed layout past kMaxViews (left alone)
static_assert(kMaxViews <= kMaxBlits, "a layout can hold every view that has a unit");

struct Rect { int32_t x = 0, y = 0, w = 0, h = 0; };

// views sorted by x, then y (view 0 = the leftmost / topmost); order[k] = the view of the Present's k-th blit
struct Layout {
    int  n = 0;
    Rect v[kMaxBlits];
    int  order[kMaxBlits] = {};
};

inline bool Near(int32_t a, int32_t b) { return a - b <= kTolPx && b - a <= kTolPx; }
inline bool SameRect(const Rect& a, const Rect& b) { return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.w, b.w) && Near(a.h, b.h); }
// two rects share more than the rounding tolerance in BOTH axes
inline bool Overlap(const Rect& a, const Rect& b) {
    return a.x + kTolPx < b.x + b.w && b.x + kTolPx < a.x + a.w && a.y + kTolPx < b.y + b.h && b.y + kTolPx < a.y + a.h;
}
inline bool Before(const Rect& a, const Rect& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; }   // x, then y

// the view a viewport rect is in a layout (-1 = none of them; may be >= kMaxViews)
inline int ViewOf(const Layout& L, const Rect& r) {
    for (int i = 0; i < L.n; ++i)
        if (SameRect(L.v[i], r)) return i;
    return -1;
}

// why the blits of one Present are / are not a layout
enum Why : int { kOk = 0, kTooFew, kTooMany, kBadRect, kOverlap, kWhyN };
inline const char* WhyName(int w) {
    static const char* const k[kWhyN] = { "ok", "fewer than 2 blits", "more blits than a layout holds (16)", "empty viewport",
                                          "overlapping viewports (the same view twice / a full-screen blit)" };
    return w >= 0 && w < kWhyN ? k[w] : "?";
}

// The blits of one Present (viewport rects in blit order) -> a layout: 2..kMaxBlits non-empty rects of which no two overlap
// (two blits at the same rect -- the same view twice -- are NOT a layout). Sizes and positions are otherwise free.
inline int MakeLayout(const Rect* b, int n, Layout* out) {
    if (n < 2) return kTooFew;
    if (n > kMaxBlits) return kTooMany;
    int idx[kMaxBlits];
    for (int i = 0; i < n; ++i) {
        if (b[i].w <= 0 || b[i].h <= 0) return kBadRect;
        idx[i] = i;
    }
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (Overlap(b[i], b[j])) return kOverlap;
    for (int i = 1; i < n; ++i)                            // insertion sort by (x, y) (stable)
        for (int j = i; j > 0 && Before(b[idx[j]], b[idx[j - 1]]); --j) { const int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; }
    Layout L;
    L.n = n;
    for (int i = 0; i < n; ++i) { L.v[i] = b[idx[i]]; L.order[idx[i]] = i; }
    *out = L;
    return kOk;
}
inline bool SameRects(const Layout& a, const Layout& b) {
    if (a.n != b.n) return false;
    for (int i = 0; i < a.n; ++i)
        if (!SameRect(a.v[i], b.v[i])) return false;
    return true;
}
inline bool SameOrder(const Layout& a, const Layout& b) {
    if (a.n != b.n) return false;
    for (int i = 0; i < a.n; ++i)
        if (a.order[i] != b.order[i]) return false;
    return true;
}

// PLACEMENT of one blit (v0.12.0 issue #12, 2nd round): the pixels a draw can touch = viewport 0 (rounded) INTERSECTED with RT0,
// and with scissor rect 0 when the rasterizer state enables the scissor test. The first in-game test (ATS, 3 x 1920x1080 Surround,
// r_scale_x 0.75) showed all three blits with the FULL-backbuffer viewport (0,0 5760x1080, 3 vertices): the views are not
// placed by the viewport. The scissor is the next place a full-screen triangle can be confined by; the diagnostics lines show
// the rest (VS / PS cbuffer 0) if it is not that either.
enum PlaceFlags : int {
    kPlVpNone    = 1,    // no viewport bound: the whole RT
    kPlVpClipped = 2,    // the viewport reaches outside RT0: clipped to it (before v0.12 round 2 the whole RT was used then)
    kPlVpOutside = 4,    // the viewport lies wholly outside RT0 / is empty: the whole RT
    kPlScissor   = 8,    // the scissor test is on and made the rect smaller
    kPlScEmpty   = 16,   // the scissor test is on but its rect misses the viewport (the draw writes nothing): ignored
};
struct Placement { Rect vp; Rect eff; int flags = 0; };
inline Rect Intersect(const Rect& a, const Rect& b) {
    const int32_t x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    const int32_t x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w, y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    Rect r; r.x = x0; r.y = y0; r.w = x1 - x0; r.h = y1 - y0;
    return r;
}
inline bool IsEmpty(const Rect& r) { return r.w <= 0 || r.h <= 0; }
// rtW x rtH = RT0; (vx, vy, vw, vh) = viewport 0 as floats (haveVp false: none bound); scissor rect 0 as D3D11_RECT
// (left, top, right, bottom; scOn = the rasterizer state's ScissorEnable, haveSc = a rect is bound)
inline Placement PlaceBlit(int32_t rtW, int32_t rtH, bool haveVp, double vx, double vy, double vw, double vh,
                           bool scOn, bool haveSc, int32_t sl, int32_t st, int32_t sr, int32_t sb) {
    Placement p;
    Rect rt; rt.w = rtW; rt.h = rtH;
    p.vp = rt;
    if (!haveVp) p.flags |= kPlVpNone;
    else {
        auto rnd = [](double v) { return (int32_t)(v >= 0.0 ? (int64_t)(v + 0.5) : -(int64_t)(-v + 0.5)); };
        Rect v; v.x = rnd(vx); v.y = rnd(vy); v.w = rnd(vw); v.h = rnd(vh);
        const Rect c = Intersect(v, rt);
        if (IsEmpty(v) || IsEmpty(c)) p.flags |= kPlVpOutside;
        else {
            if (c.x != v.x || c.y != v.y || c.w != v.w || c.h != v.h) p.flags |= kPlVpClipped;
            p.vp = c;
        }
    }
    p.eff = p.vp;
    if (scOn && haveSc) {
        Rect s; s.x = sl; s.y = st; s.w = sr - sl; s.h = sb - st;
        const Rect c = Intersect(s, p.vp);
        if (IsEmpty(s) || IsEmpty(c)) p.flags |= kPlScEmpty;
        else if (c.x != p.vp.x || c.y != p.vp.y || c.w != p.vp.w || c.h != p.vp.h) { p.eff = c; p.flags |= kPlScissor; }
    }
    return p;
}

// JITTER PHASE (all views of a frame share one Halton phase, as both VR eyes do): pass s of n views per frame has phase
// ((s + off) / n) % phases, where off makes the frame's FIRST pass a multiple of n. The blit that consumes pass s is the Present's
// k-th blit (k = 0 first) and passes / blits keep their order (the FIFO) -> (s + off) % n == k -> off = (k - s) mod n.
inline int PhaseOffset(int k, uint64_t s, int n) {
    if (n < 1) return 0;
    return (int)(((uint64_t)(k % n) + (uint64_t)n - s % (uint64_t)n) % (uint64_t)n);
}
inline int PhaseOf(uint64_t s, int off, int n, int phases) {
    if (n < 1) n = 1;
    if (phases < 1) phases = 1;
    return (int)(((s + (uint64_t)off) / (uint64_t)n) % (uint64_t)phases);
}
// the Present-local blit index pass s will have (the same relation; the caller maps it to a view through Layout::order)
inline int BlitIndexOf(uint64_t s, int off, int n) {
    if (n < 1) return 0;
    return (int)((s + (uint64_t)off) % (uint64_t)n);
}

// The detection state machine (render thread: OnBlit per matched flat backbuffer blit, OnPresent per Present boundary).
class Detector {
public:
    enum Event : int { kNone = 0, kConfirmed, kDropped, kOrderChanged, kNotDistinct };

    void Reset() { *this = Detector(); }

    // A matched flat backbuffer blit with viewport `r`. Returns its view: 0 while one view; with a confirmed layout the index of
    // its viewport in it (0..kMaxViews-1), kBeyondCap = a view past the cap, -1 = not in the layout (the caller leaves both alone).
    int OnBlit(const Rect& r) {
        const int k = m_cur;
        if (m_cur < kMaxBlits) m_blits[m_cur] = r;
        if (m_cur < 1000000) ++m_cur;
        if (m_views < 2) return 0;
        const int v = ViewOf(m_layout, r);
        if (v < 0) { ++m_unmapped; return -1; }
        if (k < m_layout.n && m_layout.order[k] != v) ++m_orderMismatch;
        if (v >= kMaxViews) { ++m_beyondCap; return kBeyondCap; }
        return v;
    }
    // the Present-local index of the last OnBlit (0 = the first blit of this Present), -1 before any
    int BlitIndex() const { return m_cur - 1; }

    Event OnPresent() {
        const int n = m_cur;
        m_cur = 0;
        if (n == 0) return kNone;                          // no world blit this Present: neutral
        Layout L;
        const int why = n > kMaxBlits ? (int)kTooMany : MakeLayout(m_blits, n, &L);
        m_lastWhy = why;
        m_lastN = n;
        if (m_views >= 2) {
            if (why == kOk && SameRects(L, m_layout)) {
                m_miss = 0;
                if (!SameOrder(L, m_layout)) {
                    for (int i = 0; i < L.n; ++i) m_layout.order[i] = L.order[i];
                    ++m_orderChanges;
                    return kOrderChanged;
                }
                return kNone;
            }
            if (++m_miss >= kDropPresents) {
                m_prevViews = m_views;
                m_views = 1;
                m_miss = 0;
                m_stable = 0;
                ++m_drops;
                return kDropped;
            }
            return kNone;
        }
        if (why == kOk) {
            if (m_stable > 0 && SameRects(L, m_cand)) ++m_stable;
            else m_stable = 1;
            m_cand = L;                                    // (the latest order)
            if (m_stable >= kConfirmPresents) {
                m_layout = m_cand;
                m_views = m_cand.n;
                m_stable = 0;
                m_miss = 0;
                ++m_confirms;
                return kConfirmed;
            }
            return kNone;
        }
        m_stable = 0;
        if (n >= 2 && !m_notDistinctSeen) { m_notDistinctSeen = true; return kNotDistinct; }
        return kNone;
    }

    int Views() const { return m_views; }                  // views per frame (blits per Present); 1 = single view (normal flat)
    int DlaaViews() const { return m_views < kMaxViews ? m_views : kMaxViews; }   // ... of them with a DLAA unit
    const Layout& Current() const { return m_layout; }     // valid while Views() >= 2
    const Layout& Candidate() const { return m_cand; }
    int  Stable() const { return m_stable; }               // Presents in a row with the candidate layout (0 = none)
    // the view the Present's k-th blit had in the last good Present (-1 = unknown; may be >= kMaxViews)
    int  ViewOfBlitIndex(int k) const { return m_views >= 2 && k >= 0 && k < m_layout.n ? m_layout.order[k] : -1; }
    // the pass FIFO depth the views need: views (confirmed), the candidate's blit count while one is being confirmed, else 1
    int  FifoDepth() const { return m_views >= 2 ? m_views : (m_stable > 0 ? m_cand.n : 1); }
    // multi-view confirmed OR a candidate layout seen in the last non-empty Present (single-view-only paths stay off then)
    bool MultiSeen() const { return m_views >= 2 || m_stable > 0; }
    int  PrevViews() const { return m_prevViews; }
    int  LastWhy() const { return m_lastWhy; }             // the last non-empty Present's MakeLayout verdict
    int  LastN() const { return m_lastN; }                 // ... and its blit count
    uint64_t Unmapped() const { return m_unmapped; }       // blits not in the confirmed layout (total)
    uint64_t BeyondCap() const { return m_beyondCap; }     // blits of views past kMaxViews (total)
    uint64_t OrderMismatch() const { return m_orderMismatch; }   // blits whose Present-local order disagreed with the layout's
    uint64_t OrderChanges() const { return m_orderChanges; }
    uint64_t Confirms() const { return m_confirms; }
    uint64_t Drops() const { return m_drops; }

private:
    Rect     m_blits[kMaxBlits];
    int      m_cur = 0;                                    // blits so far in this Present
    int      m_views = 1;
    Layout   m_layout;                                     // the confirmed layout
    Layout   m_cand;                                       // the layout being confirmed
    int      m_stable = 0, m_miss = 0, m_prevViews = 1;
    int      m_lastWhy = kTooFew, m_lastN = 0;
    bool     m_notDistinctSeen = false;
    uint64_t m_unmapped = 0, m_beyondCap = 0, m_orderMismatch = 0, m_orderChanges = 0, m_confirms = 0, m_drops = 0;
};

}  // namespace FlatViews
