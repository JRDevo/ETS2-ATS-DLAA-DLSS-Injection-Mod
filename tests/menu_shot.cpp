// phase 46: menu_shot -- offline renders of the REAL tuning menu and fps box (src/menu.cpp, linked as it is) to PNG files for the
// README, without the game. A WARP D3D11 device, a render target over a neutral blurred-road background (generated here, no game
// assets), a hand-filled TuningMenu::Panel, TuningMenu::Draw / DrawWidget, read back, saved with WIC.
//
//   menu_shot.exe <output dir>      writes tuning-menu.png, tuning-menu-vr-performance.png, tuning-menu-picture.png,
//                                   tuning-menu-hotkeys.png (v0.13.0 phase 50), fps-counter.png, fps-counter-colours.png;
//                                   exit 0 = all written.
//
// THE DATA is ONE table (kRows, below) that mirrors MenuBuildPanel / kMenuDesc / MenuPerfLines in src/inject.cpp (those need the
// whole of inject.cpp, so the panel is filled by hand): update the table when a row is added or renamed. The scenario is a
// VR session at the headset's 72 Hz with plausible figures (README "Performance"): CPU in blue, GPU in orange.
// Build: tests\build_menu_shot.bat -> build\tests\menu_shot.exe.
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include "menu.h"
#include "shader_cache.h"
#include "fps_pose.h"

using TuningMenu::kInkCpu; using TuningMenu::kInkGpu; using TuningMenu::kInkEnd;

// ---- the data table ------------------------------------------------------------------------------------------------------
// Ink marks in the strings: "\x01" CPU blue, "\x02" GPU orange, "\x03" end (written C / G / E below, expanded by Ink()).
// grp: -1 = top level, 0.. = a collapsible group (kGroups); hdr = this row is that group's header.
struct E {
    int grp;
    bool hdr;
    const wchar_t* name;
    const wchar_t* value;
    const wchar_t* keys;
    const wchar_t* cost;       // "" = "-"; "<c>0.31</> | <g>3.4</>" with ink marks
    bool greyed;
    const wchar_t* desc;       // the description (kMenuDesc) -- only the rows a shot selects need one
    const wchar_t* sent;       // the live cost sentence MenuPerfCosts appends
};
static const wchar_t* const kGroups[] = { L"Picture", L"Motion and mirrors", L"Upscaling and VR", L"VR performance",
                                          L"Frame rate and CPU", L"Panel settings", L"FPS counter settings" };   // phase 52: + Frame rate
enum { G_PIC, G_MOT, G_UP, G_VRP, G_FR, G_PANEL, G_FPSG, G_COUNT };
static const E kRows[] = {
    { -1, false, L"Mode", L"DLAA", L"End", L"<c>0.31</> | <g>3.4</>", false,
      L"DLAA = clean edges at the game's render size. DLSS = the same, and it upscales when the game's Scaling is below 100 %. "
      L"off = the game's own picture: use it to compare. Saved at once.",
      L"The whole mod costs 0.31 ms CPU + 3.4 ms GPU per frame now; off saves most of it." },
    { -1, false, L"Performance profile", L"high", L"Alt+F9", L"", false, nullptr, nullptr },
    { -1, false, L"DLAA model", L"2 (E)", L"Shift+F1 .. Shift+F4", L"<c>0.12</> | <g>1.2</>", false, nullptr, nullptr },
    { G_PIC, true, L"Picture", L"", L"", L"", false,
      L"Picture quality: sharpening, texture sharpness, the DLAA area and the see-through depth. Right opens the group, Left closes "
      L"it; it starts closed every time the menu opens.", nullptr },
    { G_PIC, false, L"Sharpen strength", L"0.4", L"Shift+F7 / Shift+F8", L"- | <g>0.12</>", false,
      L"Sharpens the picture after DLAA, 0 (off) to 1.0. Raise it if the picture looks soft; too high gives bright halos on edges. "
      L"Costs about 0.1 ms; 0 skips the pass.",
      L"Costs 0.12 GPU per frame now; 0 saves it." },
    { G_PIC, false, L"Sharpen width", L"1.5 px", L"Shift+F9 / Shift+F10", L"- | <g>0.12</>", false, nullptr, nullptr },
    { G_PIC, false, L"Texture sharpness", L"-0.50", L"Ctrl+F1 / Ctrl+F2", L"<c>0.02</> | -", false, nullptr, nullptr },
    { G_PIC, false, L"Cut-out texture sharpness", L"same (-0.50)", L"Ctrl+Shift+F1 / Ctrl+Shift+F2", L"<c>0.02</> | -", false, nullptr, nullptr },
    { G_PIC, false, L"DLAA area (VR)", L"40 %", L"Shift+F5 / Shift+F6", L"<c>0.12</> | <g>1.2</>", false, nullptr, nullptr },
    { G_PIC, false, L"DLAA before tonemap (flat)", L"on", L"Alt+F7  (flat only)", L"-", true, nullptr, nullptr },
    { G_PIC, false, L"See-through depth", L"on", L"Ctrl+F3", L"<c>0.05</> | <g>0.52</>", false, nullptr, nullptr },
    { G_PIC, false, L"Light glow ghosting fix", L"100 %", L"dlaa.ini dlss_bias_mask", L"", false, nullptr, nullptr },   // phase 49
    { G_MOT, true, L"Motion and mirrors", L"", L"", L"", false, nullptr, nullptr },
    { G_MOT, false, L"Per-part motion vectors", L"on", L"Alt+F5", L"<c>0.18</> | <g>0.70</>", false, nullptr, nullptr },
    { G_MOT, false, L"Mirror DLAA", L"2 mirrors (largest)", L"Alt+F6", L"<c>0.08</> | <g>0.34</>", false, nullptr, nullptr },
    { G_MOT, false, L"Mirrors in turns", L"on", L"dlaa.ini mirror_turns", L"<c>0.08</> | <g>0.34</>", false, nullptr, nullptr },
    { G_UP, true, L"Upscaling and VR", L"", L"", L"", false, nullptr, nullptr },
    { G_UP, false, L"DLSS upscale", L"off", L"Ctrl+F4", L"off", false, nullptr, nullptr },
    { G_UP, false, L"DLAA on / off", L"on", L"Shift+F11", L"<c>0.31</> | <g>3.4</>", false, nullptr, nullptr },
    { G_UP, false, L"OFXR Bridge (VR frame gen)", L"not found", L"dlaa.ini ofxr_bridge", L"", false, nullptr, nullptr },
    { G_VRP, true, L"VR performance", L"", L"", L"", false,
      L"VR speed-ups: Turbo, foveated rendering and the hidden area mask. Right opens the group, Left closes it; it starts closed "
      L"every time the menu opens.", nullptr },
    { G_VRP, false, L"Turbo mode (VR)", L"off", L"dlaa.ini xr_turbo", L"", false, nullptr, nullptr },
    { G_VRP, false, L"Foveated (VRS)", L"on", L"dlaa.ini vrs", L"<c>0.04</> | -", false,
      L"Sharp centre, lower detail at the edge (NVIDIA RTX only). Saves GPU time at high Scaling. VR only. The outer ring is shaded "
      L"once per 2 x 2 pixels; fences, grass and leaves keep full detail. Turn it off if the edge looks blocky.",
      L"CPU 0.04 ms per frame; it saves GPU time in the game's own figure." },
    { G_VRP, false, L"Foveated size", L"80 % (balanced)", L"dlaa.ini vrs_inner / vrs_outer", L"<c>0.04</> | -", false, nullptr, nullptr },
    { G_VRP, false, L"Foveated passes", L"scene", L"dlaa.ini vrs_passes", L"<c>0.04</> | -", false, nullptr, nullptr },
    { G_VRP, false, L"Hidden area mask (VR)", L"on", L"dlaa.ini xr_hidden_mask", L"<c>0.02</> | -", false, nullptr, nullptr },
    { G_FR, true, L"Frame rate and CPU", L"", L"", L"", false, nullptr, nullptr },                                       // phase 52
    { G_FR, false, L"Reflex (flat)", L"off", L"dlaa.ini reflex  (flat only)", L"", true, nullptr, nullptr },
    { G_FR, false, L"FPS cap (flat)", L"off", L"dlaa.ini fps_cap  (flat only)", L"", true, nullptr, nullptr },
    { G_FR, false, L"VR FPS cap", L"off", L"dlaa.ini vr_fps_cap", L"", false, nullptr, nullptr },
    { G_FR, false, L"CPU tuning", L"off", L"dlaa.ini cpu_pcore / cpu_priority / cpu_no_throttle", L"", false, nullptr, nullptr },   // phase 54
    { G_PANEL, true, L"Panel settings", L"", L"", L"", false, nullptr, nullptr },
    { G_PANEL, false, L"Panel position X", L"+0 %", L"dlaa.ini menu_vr_x", L"", false, nullptr, nullptr },
    { G_PANEL, false, L"Panel position Y", L"+0 %", L"dlaa.ini menu_vr_y", L"", false, nullptr, nullptr },
    { G_PANEL, false, L"Panel size", L"1.0", L"dlaa.ini menu_vr_scale", L"", false, nullptr, nullptr },
    { G_PANEL, false, L"Panel VR depth", L"1.00 m", L"dlaa.ini menu_vr_dist", L"", false, nullptr, nullptr },
    { G_PANEL, false, L"Panel faces you (VR)", L"on", L"dlaa.ini menu_vr_face", L"", false, nullptr, nullptr },
    { G_PANEL, false, L"Menu keys", L"Delete", L"Up / Down / Left / Right", L"", false, nullptr, nullptr },
    { -1, false, L"FPS counter", L"72 fps + 13.9 ms", L"dlaa.ini fps_show / fps_text", L"- | <g>0.012</>", false, nullptr, nullptr },
    { G_FPSG, true, L"FPS counter settings", L"", L"", L"", false, nullptr, nullptr },
    { G_FPSG, false, L"FPS counter lock", L"Follow head", L"dlaa.ini fps_vr_lock", L"", false, nullptr, nullptr },
    { G_FPSG, false, L"Position per vehicle", L"On", L"dlaa.ini fps_profiles", L"", true, nullptr, nullptr },   // phase 51b (greyed: lock = Follow head)
    { G_FPSG, false, L"Re-measure cab spot", L"-", L"Right: now, Left: defaults", L"", true, nullptr, nullptr },   // phase 51c / 51d (greyed: lock = Follow head)
    { G_FPSG, false, L"FPS counter X", L"+5 %", L"dlaa.ini fps_vr_x", L"", false, nullptr, nullptr },
    { G_FPSG, false, L"FPS counter Y", L"+5 %", L"dlaa.ini fps_vr_y", L"", false, nullptr, nullptr },
    { G_FPSG, false, L"FPS turn", L"+0 deg", L"dlaa.ini fps_vr_yaw", L"", false, nullptr, nullptr },
    { G_FPSG, false, L"FPS tilt", L"+0 deg", L"dlaa.ini fps_vr_pitch", L"", false, nullptr, nullptr },
    { G_FPSG, false, L"FPS rotate", L"+0 deg", L"dlaa.ini fps_vr_roll", L"", false, nullptr, nullptr },
    { G_FPSG, false, L"FPS counter size", L"0.3", L"dlaa.ini fps_vr_scale", L"", false, nullptr, nullptr },
    { G_FPSG, false, L"FPS colour", L"white", L"dlaa.ini fps_color", L"", false, nullptr, nullptr },
    { G_FPSG, false, L"FPS brightness", L"100 %", L"dlaa.ini fps_brightness", L"", false, nullptr, nullptr },
    { G_FPSG, false, L"FPS opacity", L"100 %", L"dlaa.ini fps_opacity", L"", false, nullptr, nullptr },   // phase 51f
    { G_FPSG, false, L"FPS distance (VR)", L"0.80 m", L"dlaa.ini fps_vr_dist", L"", false, nullptr, nullptr },
    { -1, false, L"Logging", L"errors", L"dlaa.ini log_level", L"<c>0.01</> | -", false, nullptr, nullptr },
    { -1, false, L"Hotkeys...", L"28 actions", L"Right: open", L"", false, nullptr, nullptr },   // v0.13.0 phase 50
    { -1, false, L"Save to dlaa.ini", L"-", L"Shift+F12", L"", false, nullptr, nullptr },
    { -1, false, L"Reset to defaults", L"-", L"hold Right 5 s", L"", false, nullptr, nullptr },
    { -1, false, L"Close menu", L"-", L"Delete", L"", false, nullptr, nullptr },
};
constexpr int kNRows = (int)(sizeof(kRows) / sizeof(kRows[0]));
static_assert(kNRows <= TuningMenu::kMaxRows, "menu_shot: more rows than the panel holds");

static const wchar_t kPerf0[] = L"Limit: headset (72 Hz)      fps 72 (13.9 ms, headset 72 Hz)      frame busy <c>6.1</> | <g>9.8</> ms";
static const wchar_t kPerf1[] = L"this mod: <c>0.31</> | <g>3.4</> ms -- GPU: NGX 1.2, part motion 0.7, camera 0.4, see-through 0.5, "
                                L"mirrors 0.3, sharpen 0.1, copies 0.2";
static const wchar_t kPerf2[] = L"<c>CPU</> | <g>GPU</> ms per frame (GPU: both eyes), other threads 0.4. CPU = the game's render thread; "
                                L"measuring costs 0.12 ms CPU (taken out).";
static const wchar_t kTitle[] = L"ETS2 / ATS DLAA & DLSS mod v0.13.0 - tuning menu";
static const wchar_t kFooter[] = L"Up / Down select   Left / Right change   Delete close   keys: Hotkeys...";

// Ink shorthand in the table strings: "<c>0.31</>" = CPU blue, "<g>3.4</>" = GPU orange (menu.h kInkCpu / kInkGpu / kInkEnd).
static std::wstring Ink(const wchar_t* s) {
    std::wstring o;
    for (size_t i = 0; s[i]; ++i) {
        if (!wcsncmp(s + i, L"<c>", 3))       { o += kInkCpu; i += 2; }
        else if (!wcsncmp(s + i, L"<g>", 3))  { o += kInkGpu; i += 2; }
        else if (!wcsncmp(s + i, L"</>", 3))  { o += kInkEnd; i += 2; }
        else o += s[i];
    }
    return o;
}

// ---- the panel -----------------------------------------------------------------------------------------------------------
static void Put(wchar_t* dst, size_t cap, const std::wstring& s) { wcsncpy_s(dst, cap, s.c_str(), _TRUNCATE); }

// open[g]: group g shown. selName: the selected row (by name).
static void BuildPanel(TuningMenu::Panel& p, const bool open[G_COUNT], const wchar_t* selName) {
    memset(&p, 0, sizeof(p));
    wcsncpy_s(p.title, kTitle, _TRUNCATE);
    Put(p.perf[0], 224, Ink(kPerf0));
    Put(p.perf[1], 224, Ink(kPerf1));
    Put(p.perf[2], 224, Ink(kPerf2));
    swprintf(p.costHead, 24, L"%lcCPU%lc | %lcGPU%lc ms", kInkCpu, kInkEnd, kInkGpu, kInkEnd);
    wcsncpy_s(p.footer, kFooter, _TRUNCATE);
    int nv = 0;
    const E* sel = nullptr;
    for (int i = 0; i < kNRows; ++i) {
        const E& e = kRows[i];
        if (e.grp >= 0 && !e.hdr && !open[e.grp]) continue;       // a collapsed group's children are not in the panel
        TuningMenu::Row& r = p.rows[nv];
        if (e.hdr)          swprintf(r.name, 48, L"%lc %ls", open[e.grp] ? (wchar_t)0x25BC : (wchar_t)0x25BA, e.name);   // as MenuBuildPanel
        else if (e.grp >= 0) swprintf(r.name, 48, L"    %ls", e.name);                                         // (phase 46b)
        else                wcsncpy_s(r.name, e.name, _TRUNCATE);
        wcsncpy_s(r.keys, e.keys, _TRUNCATE);
        r.greyed = e.greyed ? 1 : 0;
        if (e.hdr) {
            int n = 0;
            for (int j = 0; j < kNRows; ++j) if (kRows[j].grp == e.grp && !kRows[j].hdr) ++n;
            if (open[e.grp]) wcsncpy_s(r.value, L"open", _TRUNCATE); else swprintf(r.value, 40, L"closed (%d)", n);
            wcsncpy_s(r.keys, open[e.grp] ? L"Left: close" : L"Right: open", _TRUNCATE);
        } else {
            wcsncpy_s(r.value, e.value, _TRUNCATE);
        }
        Put(r.cost, 32, e.cost[0] ? Ink(e.cost) : std::wstring(L"-"));
        if (!wcscmp(e.name, selName)) { p.sel = nv; sel = &e; }
        ++nv;
    }
    p.nRows = nv;
    if (sel && sel->desc) {
        if (sel->sent) swprintf(p.desc, 480, L"%ls %ls", sel->desc, sel->sent);
        else           wcsncpy_s(p.desc, sel->desc, _TRUNCATE);
    }
}

// ---- background: a dark, blurred road-like picture (no game assets) ----------------------------------------------------------
static float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
static float Smooth(float a, float b, float x) { const float t = Clamp01((x - a) / (b - a)); return t * t * (3 - 2 * t); }
static std::vector<uint32_t> Background(int W, int H) {
    std::vector<uint32_t> px((size_t)W * H);
    const float hz = 0.40f * H;
    uint32_t seed = 12345;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float fx = (float)x / W, fy = (float)y;
            float r, g, b;
            if (fy < hz) {                                         // dusk sky, darker towards the top
                const float t = fy / hz;
                r = 0.10f + 0.16f * t; g = 0.12f + 0.15f * t; b = 0.17f + 0.15f * t;
            } else {                                               // asphalt, darker towards the bottom
                const float t = (fy - hz) / (H - hz);
                r = g = 0.17f - 0.08f * t; b = 0.18f - 0.08f * t;
            }
            // far hills / tree line (soft)
            const float tl = hz - 0.05f * H * (0.55f + 0.45f * std::sin(fx * 13.0f) * std::sin(fx * 5.0f + 1.0f));
            const float tree = 1.0f - Smooth(tl - 10.0f, tl + 10.0f, fy);
            const float inGround = Smooth(hz - 6.0f, hz + 6.0f, fy);
            const float tm = tree * (1.0f - inGround);
            r = r * (1 - tm) + 0.05f * tm; g = g * (1 - tm) + 0.10f * tm; b = b * (1 - tm) + 0.07f * tm;
            // the road: a wedge from the vanishing point (fx 0.5, hz), lighter than the verge, a dashed centre line
            if (fy > hz) {
                const float t = (fy - hz) / (H - hz);
                const float half = 0.04f + 0.55f * t;
                const float d = std::fabs(fx - 0.5f);
                const float road = 1.0f - Smooth(half - 0.02f * (0.2f + t), half + 0.02f * (0.2f + t), d);
                r += road * 0.07f; g += road * 0.07f; b += road * 0.075f;
                const float line = 1.0f - Smooth(0.003f + 0.01f * t, 0.012f + 0.02f * t, d);
                const float dash = Smooth(0.45f, 0.55f, std::fabs(std::sin(40.0f * std::pow(1.0f - t, 2.2f) + 0.8f)));
                const float lm = line * dash * road * 0.45f;
                r += lm; g += lm; b += lm * 0.8f;
                const float edge = 1.0f - Smooth(0.006f + 0.012f * t, 0.016f + 0.03f * t, std::fabs(d - half * 0.9f));
                r += edge * road * 0.12f; g += edge * road * 0.12f; b += edge * road * 0.12f;
            }
            seed = seed * 1664525u + 1013904223u;
            const float nz = ((float)(seed >> 24) / 255.0f - 0.5f) * 0.02f;
            const float v[3] = { r + nz, g + nz, b + nz };
            uint32_t c[3];
            for (int k = 0; k < 3; ++k) c[k] = (uint32_t)(Clamp01(v[k]) * 255.0f + 0.5f);
            px[(size_t)y * W + x] = c[0] | (c[1] << 8) | (c[2] << 16) | 0xFF000000u;   // RGBA
        }
    }
    return px;
}

// ---- D3D11 + WIC ---------------------------------------------------------------------------------------------------------
struct Gpu { ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr; };

// a W x H render target filled with the background; `draw` renders into it; returns the RGBA pixels read back
template <class F>
static bool Render(Gpu& g, int W, int H, F draw, std::vector<uint32_t>& out) {
    std::vector<uint32_t> bg = Background(W, H);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA sd{ bg.data(), (UINT)(W * 4), 0 };
    ID3D11Texture2D* rt = nullptr;
    if (FAILED(g.dev->CreateTexture2D(&td, &sd, &rt))) { printf("CreateTexture2D failed\n"); return false; }
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(g.dev->CreateRenderTargetView(rt, nullptr, &rtv))) { rt->Release(); return false; }
    const bool ok = draw(g.ctx, rtv, (uint32_t)W, (uint32_t)H);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* st = nullptr;
    bool good = false;
    if (ok && SUCCEEDED(g.dev->CreateTexture2D(&td, nullptr, &st))) {
        g.ctx->CopyResource(st, rt);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(g.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            out.resize((size_t)W * H);
            for (int y = 0; y < H; ++y) memcpy(&out[(size_t)y * W], (const char*)m.pData + (size_t)y * m.RowPitch, (size_t)W * 4);
            g.ctx->Unmap(st, 0);
            good = true;
        }
    }
    if (st) st->Release();
    rtv->Release(); rt->Release();
    return good;
}

// GDI text on the opaque pixels (labels of the colour strip)
static void LabelText(std::vector<uint32_t>& px, int W, int H, int cx, int y, const wchar_t* s) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ ob = SelectObject(dc, bmp);
    uint32_t* d = (uint32_t*)bits;
    for (size_t i = 0; i < px.size(); ++i) {                      // RGBA -> BGRA
        const uint32_t v = px[i];
        d[i] = ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF);
    }
    HFONT f = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, TA_CENTER | TA_TOP);
    SetTextColor(dc, RGB(8, 8, 8));
    TextOutW(dc, cx + 1, y + 1, s, (int)wcslen(s));
    SetTextColor(dc, RGB(225, 225, 230));
    TextOutW(dc, cx, y, s, (int)wcslen(s));
    GdiFlush();
    for (size_t i = 0; i < px.size(); ++i) {
        const uint32_t v = d[i];
        px[i] = ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF) | 0xFF000000u;
    }
    SelectObject(dc, of); DeleteObject(f); SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
}

static bool SavePng(const wchar_t* path, const std::vector<uint32_t>& rgba, int W, int H) {
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac)))) return false;
    IWICStream* stream = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
    bool ok = false;
    std::vector<uint8_t> bgr((size_t)W * H * 3);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        const uint32_t v = rgba[i];
        bgr[i * 3 + 0] = (uint8_t)((v >> 16) & 0xFF); bgr[i * 3 + 1] = (uint8_t)((v >> 8) & 0xFF); bgr[i * 3 + 2] = (uint8_t)(v & 0xFF);
    }
    if (SUCCEEDED(fac->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
        SUCCEEDED(fr->Initialize(nullptr))) {
        WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(fr->SetSize((UINT)W, (UINT)H)) && SUCCEEDED(fr->SetPixelFormat(&pf)) &&
            SUCCEEDED(fr->WritePixels((UINT)H, (UINT)(W * 3), (UINT)bgr.size(), bgr.data())) && SUCCEEDED(fr->Commit()) &&
            SUCCEEDED(enc->Commit()))
            ok = true;
    }
    if (fr) fr->Release(); if (enc) enc->Release(); if (stream) stream->Release(); fac->Release();
    return ok;
}

static std::wstring g_dir;
static int g_fail = 0;
static void Save(const wchar_t* name, const std::vector<uint32_t>& px, int W, int H) {
    const std::wstring path = g_dir + L"\\" + name;
    const bool ok = SavePng(path.c_str(), px, W, H);
    wprintf(L"  %ls %dx%d %ls\n", name, W, H, ok ? L"written" : L"FAILED");
    if (!ok) ++g_fail;
}

// ---- the shots ---------------------------------------------------------------------------------------------------------------
static void MenuShot(Gpu& g, const wchar_t* file, const bool open[G_COUNT], const wchar_t* sel) {
    TuningMenu::Panel p;
    BuildPanel(p, open, sel);
    const float scale = 1.6f;                                      // the existing README picture is the panel at about 1.6x
    const int margin = 10;
    const int W = (int)std::ceil(TuningMenu::kRefWidth * scale) + 2 * margin;
    const int H = (int)std::ceil(TuningMenu::RefHeight(p.nRows) * scale) + 2 * margin;
    std::vector<uint32_t> px;
    const bool ok = Render(g, W, H, [&](ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h) {
        return TuningMenu::Draw(c, rtv, w, h, (float)margin, (float)margin, scale, scale, TuningMenu::kOutRaw, p);
    }, px);
    TuningMenu::ReleaseTargets();
    if (!ok) { wprintf(L"  %ls: render FAILED\n", file); ++g_fail; return; }
    Save(file, px, W, H);
}

// v0.13.0 phase 50: the Hotkeys page (mirrors MenuBuildHotkeysPanel in src/inject.cpp: "< Back" + one line per action, the
// Keyboard / Controller columns), the "Sharpen strength +" line waiting for a press (the wide bind-mode line).
static void HotkeysShot(Gpu& g, const wchar_t* file) {
    struct L { const wchar_t* name; const wchar_t* kb; const wchar_t* pad; };
    static const L kL[] = {
        { L"Mode (DLAA / DLSS / off)", L"End", L"-" }, { L"Performance profile", L"Alt+F9", L"-" },
        { L"DLAA model 1 (K)", L"Shift+F1", L"-" }, { L"DLAA model 2 (E)", L"Shift+F2", L"-" },
        { L"DLAA model 3 (F)", L"Shift+F3", L"-" }, { L"DLAA model 4 (M)", L"Shift+F4", L"-" },
        { L"Sharpen strength -", L"Shift+F7", L"Wheel7" }, { L"Sharpen strength +", L"Shift+F8", L"Wheel8" },
        { L"Sharpen width -", L"Shift+F9", L"-" }, { L"Sharpen width +", L"Shift+F10", L"-" },
        { L"Texture sharpness -", L"Ctrl+F1", L"-" }, { L"Texture sharpness +", L"Ctrl+F2", L"-" },
        { L"Cut-out sharpness -", L"Ctrl+Shift+F1", L"-" }, { L"Cut-out sharpness +", L"Ctrl+Shift+F2", L"-" },
        { L"DLAA area (VR) -", L"Shift+F5", L"-" }, { L"DLAA area (VR) +", L"Shift+F6", L"-" },
        { L"Per-part motion vectors", L"Alt+F5", L"-" }, { L"Mirror DLAA on / off", L"Alt+F6", L"-" },
        { L"DLAA before tonemap (flat)", L"Alt+F7", L"-" }, { L"See-through depth", L"Ctrl+F3", L"-" },
        { L"DLSS upscale", L"Ctrl+F4", L"-" }, { L"DLAA on / off", L"Shift+F11", L"-" }, { L"Save to dlaa.ini", L"Shift+F12", L"-" },
        { L"Menu: open / close", L"Delete", L"Wheel10" }, { L"Menu: up", L"Up", L"WheelUp" }, { L"Menu: down", L"Down", L"WheelDown" },
        { L"Menu: left", L"Left", L"WheelLeft" }, { L"Menu: right", L"Right", L"WheelRight" },
    };
    constexpr int kN = (int)(sizeof(kL) / sizeof(kL[0]));
    static_assert(kN + 1 <= TuningMenu::kMaxRows, "menu_shot: hotkeys page too long");
    const int bindLine = 8;                                        // "Sharpen strength +"
    TuningMenu::Panel p;
    memset(&p, 0, sizeof(p));
    wcsncpy_s(p.title, L"ETS2 / ATS DLAA & DLSS mod v0.13.0 - hotkeys", _TRUNCATE);
    Put(p.perf[0], 224, Ink(kPerf0));
    Put(p.perf[1], 224, Ink(kPerf1));
    Put(p.perf[2], 224, Ink(kPerf2));
    wcsncpy_s(p.colHead[0], L"Action", _TRUNCATE);
    wcsncpy_s(p.colHead[1], L"Keyboard", _TRUNCATE);
    wcsncpy_s(p.colHead[2], L"Controller", _TRUNCATE);
    wcsncpy_s(p.costHead, L" ", _TRUNCATE);
    wcsncpy_s(p.rows[0].name, L"< Back", _TRUNCATE);
    wcsncpy_s(p.rows[0].value, L"-", _TRUNCATE);
    wcsncpy_s(p.rows[0].keys, L"Left / Right: main menu", _TRUNCATE);
    for (int i = 0; i < kN; ++i) {
        TuningMenu::Row& r = p.rows[i + 1];
        wcsncpy_s(r.name, kL[i].name, _TRUNCATE);
        wcsncpy_s(r.value, kL[i].kb, _TRUNCATE);
        wcsncpy_s(r.keys, kL[i].pad, _TRUNCATE);
    }
    TuningMenu::Row& b = p.rows[bindLine];
    b.wide = 1;
    wcsncpy_s(b.value, L"Press a key or button...  (Left = cancel, Backspace = clear)", _TRUNCATE);
    p.nRows = kN + 1;
    p.sel = bindLine;
    wcsncpy_s(p.desc, L"Press ONE new key (Shift / Ctrl / Alt with it make a combination) or controller button / D-pad direction. "
              L"Keys / buttons already held now (a shifter gear) are ignored until released; axes never bind. Left or Esc cancels, "
              L"Backspace clears to none, 10 s without a press cancels.", _TRUNCATE);
    wcsncpy_s(p.footer, L"press the new key / button   Left cancel   Backspace clear", _TRUNCATE);
    const float scale = 1.6f;
    const int margin = 10;
    const int W = (int)std::ceil(TuningMenu::kRefWidth * scale) + 2 * margin;
    const int H = (int)std::ceil(TuningMenu::RefHeight(p.nRows) * scale) + 2 * margin;
    std::vector<uint32_t> px;
    const bool ok = Render(g, W, H, [&](ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h) {
        return TuningMenu::Draw(c, rtv, w, h, (float)margin, (float)margin, scale, scale, TuningMenu::kOutRaw, p);
    }, px);
    TuningMenu::ReleaseTargets();
    if (!ok) { wprintf(L"  %ls: render FAILED\n", file); ++g_fail; return; }
    Save(file, px, W, H);
}

static void FpsShots(Gpu& g) {
    // fps-counter.png: the default white box over the road picture
    {
        const float s = 1.25f;
        const int W = (int)(TuningMenu::kWidgetRefW * s) + 30, H = (int)(TuningMenu::kWidgetRefH * s) + 26;
        std::vector<uint32_t> px;
        TuningMenu::SetWidgetColors(255, 255, 255, 8, 8, 8);
        const bool ok = Render(g, W, H, [&](ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h) {
            return TuningMenu::DrawWidget(c, rtv, w, h, 15.0f, 13.0f, s, s, TuningMenu::kOutRaw, L"72 fps", L"13.9 ms");
        }, px);
        TuningMenu::ReleaseWidget();
        if (ok) Save(L"fps-counter.png", px, W, H); else { wprintf(L"  fps-counter.png: render FAILED\n"); ++g_fail; }
    }
    // fps-counter-colours.png: the 20 presets (src/fps_pose.h), 5 per row, each with its name
    {
        const float s = 1.1f;
        const int cols = 5, rows = 4, cw = 150, ch = 84;
        const int W = cols * cw, H = rows * ch;
        std::vector<uint32_t> px;
        const bool ok = Render(g, W, H, [&](ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h) {
            for (int i = 0; i < FpsPose::kColorCount; ++i) {
                const FpsPose::Color& k = FpsPose::ColorOf(i);
                TuningMenu::SetWidgetColors(k.tr, k.tg, k.tb, k.lr, k.lg, k.lb);
                const float x = (float)((i % cols) * cw) + (cw - TuningMenu::kWidgetRefW * s) * 0.5f;
                const float y = (float)((i / cols) * ch) + 6.0f;
                if (!TuningMenu::DrawWidget(c, rtv, w, h, x, y, s, s, TuningMenu::kOutRaw, L"72 fps", L"13.9 ms")) return false;
            }
            return true;
        }, px);
        TuningMenu::ReleaseWidget();
        if (!ok) { wprintf(L"  fps-counter-colours.png: render FAILED\n"); ++g_fail; return; }
        for (int i = 0; i < FpsPose::kColorCount; ++i)
            LabelText(px, W, H, (i % cols) * cw + cw / 2, (i / cols) * ch + 6 + (int)(TuningMenu::kWidgetRefH * s) + 3,
                      FpsPose::ColorOf(i).name);
        Save(L"fps-counter-colours.png", px, W, H);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: menu_shot <output dir>\n"); return 2; }
    {
        wchar_t w[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, argv[1], -1, w, MAX_PATH);
        g_dir = w;
        while (!g_dir.empty() && (g_dir.back() == L'\\' || g_dir.back() == L'/')) g_dir.pop_back();
        CreateDirectoryW(g_dir.c_str(), nullptr);
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Gpu g;
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx))) {
        printf("WARP device failed\n");
        return 1;
    }
    TuningMenu::RegisterShaders();
    ShaderCache::Start();
    while (!ShaderCache::Done()) Sleep(10);
    if (!ShaderCache::Ready(ShaderCache::kMenuVs) || !ShaderCache::Ready(ShaderCache::kMenuPs)) {
        printf("menu shaders not compiled: %s %s\n", ShaderCache::Error(ShaderCache::kMenuVs), ShaderCache::Error(ShaderCache::kMenuPs));
        return 1;
    }
    printf("menu_shot -> %ls\n", g_dir.c_str());
    bool none[G_COUNT] = {}, vrp[G_COUNT] = {}, pic[G_COUNT] = {};
    vrp[G_VRP] = true; pic[G_PIC] = true;
    MenuShot(g, L"tuning-menu.png", none, L"Mode");
    MenuShot(g, L"tuning-menu-vr-performance.png", vrp, L"Foveated (VRS)");
    MenuShot(g, L"tuning-menu-picture.png", pic, L"Sharpen strength");
    bool all[G_COUNT]; for (bool& o : all) o = true;
    MenuShot(g, L"tuning-menu-all-open.png", all, L"Mode");                       // phase 46b: every group open
    HotkeysShot(g, L"tuning-menu-hotkeys.png");                                   // v0.13.0 phase 50: the Hotkeys page
    FpsShots(g);
    g.ctx->Release();
    g.dev->Release();
    printf("%s\n", g_fail ? "FAILED" : "all images written");
    return g_fail ? 1 : 0;
}
