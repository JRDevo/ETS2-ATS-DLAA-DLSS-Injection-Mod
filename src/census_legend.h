// phase 112b: the census LEGEND's text -- what the census view ("Show unfixed shaders") shows and how to save a flickering object
// for a fix. Phase 113 (the user: "make it explain what is happening in plain words for a player"): no per-colour jargon any more --
// a title that says what a colour means, three plain lines (the colours are kinds of problem, you do not need to know which; press the
// capture key twice to save it; saved objects are not saved twice + how to switch the colours off), one more line ONLY while a draw
// is coloured because a fix of the mod is switched off, and a small colour key on one row (one word per colour). Pure (no D3D, no
// inject.cpp state): inject.cpp (src/census_run.h, CensusLegendNow) fills it at Present with the player's own key names;
// tests/menu_shot.cpp renders the same text to a PNG. The colours are the tint table's own (Census::KindColour).
#pragma once
#ifdef WITH_DLAA
#include <cwchar>
#include <algorithm>
#include "menu.h"
#include "lang.h"
#include "census.h"

// capKey / viewKey = KeyNameUi of the capture key (pressed twice) and of key_census_view, widened; null / "" / "none" = unbound
// (the lines then name the menu rows instead). fixOff = objects are NOT coloured because a fix of the mod is switched off (phase 116:
// the bug icon's "N: fix is off" count > 0; phase 113 coloured them and said "some colours mean a fix is switched off").
inline void CensusLegendFill(TuningMenu::Legend& lg, const wchar_t* capKey, const wchar_t* viewKey, bool fixOff = false) {
    lg = TuningMenu::Legend{};
    auto bound = [](const wchar_t* k) { return k && k[0] && wcscmp(k, L"none") != 0; };
    const size_t kT = sizeof(lg.title) / sizeof(lg.title[0]), kL = sizeof(lg.line[0]) / sizeof(lg.line[0][0]);
    swprintf(lg.title, kT, L"%ls", TR(L"Census: flashing colours = no fix in the mod yet"));   // (phase 115: the checkerboard)
    swprintf(lg.line[0], kL, L"%ls", TR(L"Each colour is a different kind of problem (thin, see-through, water, ...). You do not need to know which."));
    if (bound(capKey)) swprintf(lg.line[1], kL, TRF(L"Something coloured? Press %ls twice. That saves it so it can be fixed."), capKey);
    else               swprintf(lg.line[1], kL, L"%ls", TR(L"Something coloured? Bug report > Capture now saves it so it can be fixed."));
    // phase 115: a saved (captured) object stops flashing for the rest of the session; the bug icon counts what is left here
    if (bound(viewKey)) swprintf(lg.line[2], kL, TRF(L"Saved objects stop flashing until the next game start. Colours off: %ls."), viewKey);
    else                swprintf(lg.line[2], kL, L"%ls", TR(L"Saved objects stop flashing until the next game start. Colours off: Bug report > Show unfixed shaders."));
    swprintf(lg.line[3], kL, L"%ls", TR(L"The bug icon counts what is left to capture here."));
    if (fixOff) swprintf(lg.line[4], kL, L"%ls", TR(L"Objects with a fix that is switched off are not coloured: see Picture preset > Custom."));
    // the small colour key, in the order the user asked for: thin / see-through / pixel / water / new (census kinds 2, 1, 0, 3, 4)
    static_assert(Census::kKindN <= TuningMenu::kLegendLines, "one key entry per census kind");
    const int order[Census::kKindN] = { 2, 1, 0, 3, 4 };
    const wchar_t* const word[Census::kKindN] = { TR(L"thin"), TR(L"see-through"), TR(L"pixel"), TR(L"water"), TR(L"new") };
    lg.n = Census::kKindN;
    for (int i = 0; i < Census::kKindN; ++i) {
        float c[3];
        Census::KindColour(order[i], c);
        auto b = [](float v) { return (uint32_t)(v <= 0.0f ? 0 : (v >= 1.0f ? 255 : (int)(v * 255.0f + 0.5f))); };
        lg.rgb[i] = b(c[0]) | (b(c[1]) << 8) | (b(c[2]) << 16);   // COLORREF
        swprintf(lg.word[i], sizeof(lg.word[i]) / sizeof(lg.word[i][0]), L"%ls", word[i]);
    }
}

// phase 115: the bug icon's two lines -- "3 to capture" (distinct objects still flashing here in the last ~1 s) and "5 captured" (saved
// by a double-Insert capture this session). cap = the buffers' size in characters.
// phase 116: + l3, a small third line "2: fix is off" (distinct objects in the last ~1 s NOT coloured only because a fix of the mod is
// switched off); "" when that count is 0 (the icon keeps its two lines).
inline void CensusBugLines(int toCapture, int captured, wchar_t* l1, wchar_t* l2, size_t cap, int fixOff = 0, wchar_t* l3 = nullptr) {
    swprintf(l1, cap, TRF(L"%d to capture"), toCapture < 0 ? 0 : toCapture);
    swprintf(l2, cap, TRF(L"%d captured"), captured < 0 ? 0 : captured);
    if (l3) {
        if (fixOff > 0) swprintf(l3, cap, TRF(L"%d: fix is off"), fixOff);
        else l3[0] = 0;
    }
}

// phase 115: the FLAT bug icon's top-left corner (px) -- a top corner away from the fps box (fpsCx = the fps box's centre x when it is
// shown: right of the middle -> the left corner, else the right one; the status box and the legend are top centre), moved under the
// open menu panel (panel = x0, y0, x1, y1 px; x1 <= x0 = no panel) when that corner is behind it, and kept inside the picture.
inline void FlatBugPlace(float W, float H, float bw, float bh, float m, bool fpsShown, float fpsCx, const float panel[4], float* x,
                         float* y) {
    *x = (fpsShown && fpsCx > 0.5f * W) ? m : W - bw - m;
    *y = m;
    if (panel && panel[2] > panel[0] && *x < panel[2] && *x + bw > panel[0] && *y < panel[3] && *y + bh > panel[1]) *y = panel[3] + m;
    if (*y > H - bh - m) *y = H - bh - m;
    if (*x < 0.0f) *x = 0.0f;
    if (*y < 0.0f) *y = 0.0f;
}

// ---- phase 114: the legend + status box in VR -- ONE stereo placement for both eye pictures ---------------------------------------
// The user's VR clips (2026-11-15 evening, "seeing duplicates"): with the menu open the legend sat over the eye-picture panel's centre
// (u W + menu_vr_x W); with ATS's menu_vr_x = -18 and the Quest 3 optical centres u 0.379 / 0.621 the RIGHT eye's box started left of
// its picture and DrawLegend pushed it back in -- in that eye only. The two pictures then had 20..22 px of UNCROSSED disparity (beyond
// infinity): double vision. Fix: the box's offset from each eye's optical centre is the SAME dx for both eyes, clamped once so the box
// fits BOTH pictures; the only difference between the eyes is the depth shift.
namespace LegendVr {
// px towards the nose, in one eye picture, of a head-locked point straight ahead at distM (fx = that eye's focal length in px):
// half the eye separation seen at that distance. 0.063 m, 1000 px, 0.8 m -> 39.4 px.
inline float DepthShiftPx(double ipdM, double fxPx, double distM) {
    if (!(ipdM > 0.0) || !(fxPx > 0.0) || !(distM > 0.0)) return 0.0f;
    return (float)(0.5 * ipdM * fxPx / distM);
}
// the box (width bw px) centred at uPx[e] + dx + sh[e] in eye e (uPx = the optical centre in px, sh = the signed depth shift): the dx
// nearest `want` that keeps it inside [0, W] in both eyes. No room in both (box wider than the overlap) = the middle of the range.
inline float SharedDx(float want, float bw, float W, const float uPx[2], const float sh[2]) {
    float lo = -1e30f, hi = 1e30f;
    for (int e = 0; e < 2; ++e) {
        const float c = uPx[e] + sh[e];
        lo = (std::max)(lo, 0.5f * bw - c);
        hi = (std::min)(hi, W - 0.5f * bw - c);
    }
    if (lo > hi) return 0.5f * (lo + hi);
    return want < lo ? lo : (want > hi ? hi : want);
}
// phase 115r (audit B12): the widest box that fits BOTH pictures with one shared dx: W minus the distance between the two eyes'
// anchors (uPx + sh). A wider legend used to fall back to SharedDx's middle and be clamped per eye by DrawLegend -> a different
// disparity in each eye (double vision). The caller shrinks the legend to this width.
inline float OverlapW(float W, const float uPx[2], const float sh[2]) {
    const float d = (uPx[0] + sh[0]) - (uPx[1] + sh[1]);
    const float o = W - (d < 0.0f ? -d : d);
    return o > 0.0f ? o : 0.0f;
}
// the disparity (px, eye A centre minus eye B centre, each from its own optical centre) of the box as drawn in the two pictures --
// DrawLegend's own per-eye clamp included (the bug) -- for the tests: x0 = clamp(centre - bw / 2, 0, W - bw)
inline float DrawnCentre(float centre, float bw, float W) {
    float x0 = centre - 0.5f * bw;
    if (x0 > W - bw) x0 = W - bw;
    if (x0 < 0.0f) x0 = 0.0f;
    return x0 + 0.5f * bw;
}
}  // namespace LegendVr

#endif // WITH_DLAA
