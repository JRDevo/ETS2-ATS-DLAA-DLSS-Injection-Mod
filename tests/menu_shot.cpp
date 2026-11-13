// phase 46: menu_shot -- offline renders of the REAL tuning menu and fps box (src/menu.cpp, linked as it is) to PNG files for the
// README, without the game. A WARP D3D11 device, a render target over a neutral blurred-road background (generated here, no game
// assets), a hand-filled TuningMenu::Panel, TuningMenu::Draw / DrawWidget, read back, saved with WIC.
//
//   menu_shot.exe <output dir>      writes tuning-menu.png (flat), tuning-menu-vr.png (VR),
//                                   tuning-menu-all-open.png (flat, every row), tuning-menu-all-open-vr.png (VR, every row),
//                                   tuning-menu-vr-performance.png, tuning-menu-picture.png, tuning-menu-hotkeys.png,
//                                   fps-counter.png (with the history graph), fps-counter-colours.png; exit 0 = all written.
//
// THE DATA is ONE table (kRows, below) that mirrors MenuRowId / MenuBuildPanel / kMenuDesc / kMenuGroups / MenuRowMode in
// src/inject.cpp (those need the whole of inject.cpp, so the panel is filled by hand): update the table when a row is added,
// renamed or moved. Each row carries which MODE it belongs to (both / flat-only / VR-only, as MenuRowMode) and whether it is
// normally HIDDEN on this machine (X3D cores, OFXR / VectorXR not found, a split's sub-rows, the attached fps box) -- the all-open
// shots show those ungreyed (as dlaa.ini menu_show_all = 1, but not greyed), the normal shots hide them. The scenario is a session
// with plausible figures (README "Performance"): CPU in blue, GPU in orange, VR GPU = both eyes.
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
#include "lang.h"                                  // phase 74: --lang renders the menu in a language

using TuningMenu::kInkCpu; using TuningMenu::kInkGpu; using TuningMenu::kInkEnd;

// ---- the data table ------------------------------------------------------------------------------------------------------
// Ink marks in the strings: "<c>" CPU blue, "<g>" GPU orange, "</>" end (expanded by Ink()).
// grp: -1 = top level, 0.. = a collapsible group (kGroups); hdr = this row is that group's header.
// mode: M_B = both, M_F = flat only, M_V = VR only (mirrors inject.cpp MenuRowMode). hidden = normally hidden on this machine /
//       state (only the all-open shots show it). value / keys are the flat text; valueVr / keysVr override them in VR (null =
//       same). cost is the VR cost column; costFlat overrides it in flat (null = same).
enum { M_B = 0, M_F = 1, M_V = 2 };
struct E {
    int grp;
    bool hdr;
    int mode;
    bool hidden;
    const wchar_t* name;
    const wchar_t* value;
    const wchar_t* valueVr;
    const wchar_t* keys;
    const wchar_t* keysVr;
    const wchar_t* cost;       // "" = "-"; "<c>0.31</> | <g>3.4</>" with ink marks (VR)
    const wchar_t* costFlat;   // flat cost column (null = same as cost)
    const wchar_t* desc;       // the description (kMenuDesc) -- only the rows a shot selects need one
    const wchar_t* sent;       // the live cost sentence MenuPerfCosts appends
};
// phase 84 / 89d: the groups as the player sees them (kMenuGroups order). Performance + In-game UI are VR-only (their header rows
// carry M_V, so they are hidden in the flat shots, as MenuHeaderHasRows hides them when all their rows are of the other mode).
static const wchar_t* const kGroups[] = { L"Picture", L"Motion and mirrors", L"Frame rate", L"Performance", L"In-game UI",
                                          L"FPS counter", L"Menu panel", L"Compare and test", L"Bug report" };
enum { G_PIC, G_MOT, G_FR, G_VRP, G_UI, G_FPSG, G_PANEL, G_CMP, G_BUG, G_COUNT };
static const E kRows[] = {
    // ---- top level ----
    { -1, false, M_B, false, L"Mode", L"Mix (TAA + DLAA)", nullptr, L"End", nullptr, L"<c>0.31</> | <g>3.4</>", L"<c>0.28</> | <g>1.9</>",
      L"DLAA = clean edges at the game's render size. DLSS = the same, and it upscales when the game's Scaling is below 100 %. "
      L"Mix = the game's own TAA blended with DLAA / DLSS: calmer, a bit softer. off = the game's own picture. Saved at once.",
      L"The game's TAA is running now: the mix is on." },
    { -1, false, M_B, false, L"Mix amount", L"50 % TAA", nullptr, L"dlaa.ini game_taa_mix", nullptr, L"", nullptr, nullptr, nullptr },   // phase 90
    { -1, false, M_B, false, L"Quality preset", L"high", nullptr, L"Alt+F9", nullptr, L"", nullptr, nullptr, nullptr },
    { -1, false, M_B, false, L"DLAA model", L"2 (E)", nullptr, L"Shift+F1 .. Shift+F4", nullptr, L"<c>0.12</> | <g>1.2</>", L"<c>0.10</> | <g>0.6</>", nullptr, nullptr },
    // ---- Picture ----
    { G_PIC, true, M_B, false, L"Picture", L"", nullptr, L"", nullptr, L"", nullptr,
      L"Picture quality: sharpening, texture sharpness, the Glass and fence fix, the Light trail fix and the DLAA area. Right opens "
      L"the group, Left closes it; it starts closed every time the menu opens.", nullptr },
    { G_PIC, false, M_B, false, L"Sharpen strength", L"0.4", nullptr, L"Shift+F7 / Shift+F8", nullptr, L"- | <g>0.12</>", L"- | <g>0.06</>",
      L"Sharpens the picture after DLAA, 0 (off) to 1.0. Raise it if the picture looks soft; too high gives bright halos on edges. "
      L"Costs about 0.1 ms; 0 skips the pass.",
      L"Costs 0.12 GPU per frame now; 0 saves it." },
    { G_PIC, false, M_B, false, L"Sharpness size", L"1.5 px", nullptr, L"Shift+F9 / Shift+F10", nullptr, L"- | <g>0.12</>", L"- | <g>0.06</>", nullptr, nullptr },
    { G_PIC, false, M_B, false, L"Texture sharpness", L"-0.50", nullptr, L"Ctrl+F1 / Ctrl+F2", nullptr, L"<c>0.02</> | -", nullptr, nullptr, nullptr },
    { G_PIC, false, M_B, false, L"Leaves and fences sharpness", L"same (-0.50)", nullptr, L"Ctrl+Shift+F1 / Ctrl+Shift+F2", nullptr, L"<c>0.02</> | -", nullptr, nullptr, nullptr },
    { G_PIC, false, M_B, false, L"Glass and fence fix", L"on", nullptr, L"Ctrl+F3", nullptr, L"<c>0.05</> | <g>0.52</>", L"<c>0.04</> | <g>0.26</>", nullptr, nullptr },
    { G_PIC, false, M_B, false, L"Light trail fix", L"100 %", nullptr, L"dlaa.ini dlss_bias_mask", nullptr, L"- | <g>0.10</>", L"- | <g>0.05</>", nullptr, nullptr },   // phase 49
    { G_PIC, false, M_V, false, L"DLAA area", L"", L"40 %", L"Shift+F5 / Shift+F6", nullptr, L"<c>0.12</> | <g>1.2</>", nullptr, nullptr, nullptr },   // VR only
    // ---- Motion and mirrors ----
    { G_MOT, true, M_B, false, L"Motion and mirrors", L"", nullptr, L"", nullptr, L"", nullptr,
      L"How moving parts and truck mirrors are handled by DLAA. Right opens the group, Left closes it; it starts closed every time "
      L"the menu opens.", nullptr },
    { G_MOT, false, M_B, false, L"Track moving parts", L"on", nullptr, L"Alt+F5", nullptr, L"<c>0.18</> | <g>0.70</>", L"<c>0.15</> | <g>0.35</>", nullptr, nullptr },
    { G_MOT, false, M_B, false, L"Smooth mirrors", L"2 mirrors (largest)", nullptr, L"Alt+F6", nullptr, L"<c>0.08</> | <g>0.34</>", L"<c>0.06</> | <g>0.17</>", nullptr, nullptr },
    { G_MOT, false, M_B, false, L"Mirrors take turns (faster)", L"on", nullptr, L"dlaa.ini mirror_turns", nullptr, L"<c>0.08</> | <g>0.34</>", L"<c>0.06</> | <g>0.17</>", nullptr, nullptr },
    { G_MOT, false, M_F, false, L"Motion blur", L"off", nullptr, L"dlaa.ini motion_blur", nullptr, L"off", nullptr, nullptr, nullptr },   // phase 100: flat only
    // ---- Frame rate ----
    { G_FR, true, M_B, false, L"Frame rate", L"", nullptr, L"", nullptr, L"", nullptr,
      L"Frame rate and CPU: CPU tuning, AMD cache cores, an FPS cap and Low delay (Reflex) for flat, an FPS cap for VR. Right opens "
      L"the group, Left closes it; it starts closed every time the menu opens.", nullptr },
    { G_FR, false, M_B, false, L"CPU tuning", L"off", nullptr, L"dlaa.ini cpu_pcore / cpu_priority / cpu_no_throttle", nullptr, L"", nullptr, nullptr, nullptr },   // phase 54
    { G_FR, false, M_B, true,  L"AMD cache cores (X3D)", L"off", nullptr, L"dlaa.ini cpu_cache_pin", nullptr, L"", nullptr, nullptr, nullptr },   // phase 87 (X3D only)
    { G_FR, false, M_F, false, L"FPS cap", L"off", nullptr, L"dlaa.ini fps_cap", nullptr, L"", nullptr, nullptr, nullptr },   // flat only
    { G_FR, false, M_F, false, L"Low delay (Reflex)", L"off", nullptr, L"dlaa.ini reflex", nullptr, L"", nullptr, nullptr, nullptr },   // flat only
    { G_FR, false, M_V, false, L"FPS cap", L"", L"off", L"dlaa.ini vr_fps_cap", nullptr, L"", nullptr, nullptr, nullptr },   // VR only (VR FPS cap)
    // ---- Performance (VR only) ----
    { G_VRP, true, M_V, false, L"Performance", L"", nullptr, L"", nullptr, L"", nullptr,
      L"VR speed-ups: Turbo, foveated rendering, skipping the hidden lens edges, and whether OFXR Bridge / VectorXR are found. Right "
      L"opens the group, Left closes it; it starts closed every time the menu opens.", nullptr },
    { G_VRP, false, M_V, false, L"Turbo mode", L"", L"off", L"dlaa.ini xr_turbo", nullptr, L"", nullptr, nullptr, nullptr },
    { G_VRP, false, M_V, false, L"Foveated rendering", L"", L"on", L"dlaa.ini vrs", nullptr, L"<c>0.04</> | -", nullptr,
      L"Sharp centre, lower detail at the edge (NVIDIA RTX only). Saves GPU time at high Scaling. VR only. The outer ring gets one "
      L"colour per 2 x 2 pixels; fences, grass and leaves keep full detail. Turn it off if the edge looks blocky.",
      L"CPU 0.04 ms per frame; it saves GPU time in the game's own figure." },
    { G_VRP, false, M_V, false, L"Foveated size", L"", L"80 % (balanced)", L"dlaa.ini vrs_inner / vrs_outer", nullptr, L"<c>0.04</> | -", nullptr, nullptr, nullptr },
    { G_VRP, false, M_V, false, L"Foveated: lighting too", L"", L"scene", L"dlaa.ini vrs_passes", nullptr, L"<c>0.04</> | -", nullptr, nullptr, nullptr },
    { G_VRP, false, M_V, false, L"Foveated strength", L"", L"Normal (2 x 2)", L"dlaa.ini vrs_strength", nullptr, L"<c>0.04</> | -", nullptr, nullptr, nullptr },   // phase 59f
    { G_VRP, false, M_V, false, L"Foveated eye tracking", L"", L"off", L"dlaa.ini vrs_eye", nullptr, L"off", nullptr, nullptr, nullptr },   // phase 59
    { G_VRP, false, M_V, false, L"Skip hidden edges", L"", L"on", L"dlaa.ini xr_hidden_mask", nullptr, L"<c>0.02</> | -", nullptr, nullptr, nullptr },
    { G_VRP, false, M_V, true,  L"OFXR Bridge (VR frame gen)", L"", L"not found", L"dlaa.ini ofxr_bridge", nullptr, L"", nullptr, nullptr, nullptr },   // not present
    { G_VRP, false, M_V, true,  L"VectorXR (VR layer)", L"", L"not found", L"dlaa.ini vectorxr", nullptr, L"", nullptr, nullptr, nullptr },   // phase 56, not present
    // ---- In-game UI (VR only) ----
    { G_UI, true, M_V, false, L"In-game UI", L"", nullptr, L"", nullptr, L"", nullptr,
      L"Where the game's own VR UI floats (HUD, menus). The mod types the change into the game's console for you; needs g_developer "
      L"and g_console 1 in config.cfg. Right opens the group, Left closes it.", nullptr },
    { G_UI, false, M_V, false, L"Layout", L"", L"driving", L"config.cfg", nullptr, L"", nullptr, nullptr, nullptr },
    { G_UI, false, M_V, false, L"Height", L"", L"-0.40", L"y", nullptr, L"", nullptr, nullptr, nullptr },
    { G_UI, false, M_V, false, L"Side", L"", L"0.00", L"x", nullptr, L"", nullptr, nullptr, nullptr },
    { G_UI, false, M_V, false, L"Distance", L"", L"1.00", L"dist", nullptr, L"", nullptr, nullptr, nullptr },
    { G_UI, false, M_V, false, L"Tilt", L"", L"-0.50", L"pitch", nullptr, L"", nullptr, nullptr, nullptr },
    { G_UI, false, M_V, false, L"Turn", L"", L"0.00", L"yaw", nullptr, L"", nullptr, nullptr, nullptr },
    { G_UI, false, M_V, false, L"Size", L"", L"50", L"fov", nullptr, L"", nullptr, nullptr, nullptr },
    { G_UI, false, M_V, false, L"Curve", L"", L"0.0", L"radius", nullptr, L"", nullptr, nullptr, nullptr },
    { G_UI, false, M_V, false, L"Reset to config.cfg", L"", L"-", L"Right", nullptr, L"", nullptr, nullptr, nullptr },
    // ---- FPS counter on / off (top level) ----
    { -1, false, M_B, false, L"FPS counter", L"72 fps + 13.9 ms + graph", nullptr, L"dlaa.ini fps_show / fps_text", nullptr, L"- | <g>0.012</>", nullptr, nullptr, nullptr },
    // ---- FPS counter group ----
    { G_FPSG, true, M_B, false, L"FPS counter", L"", nullptr, L"", nullptr, L"", nullptr,
      L"The fps box's lock, position, size, colour, VR distance and turn. Right opens the group, Left closes it; it starts closed "
      L"every time the menu opens.", nullptr },
    { G_FPSG, false, M_B, false, L"FPS counter X", L"5 %", L"+5.0 %", L"dlaa.ini fps_x", L"dlaa.ini fps_vr_x", L"", nullptr, nullptr, nullptr },
    { G_FPSG, false, M_B, false, L"FPS counter Y", L"5 %", L"+5.0 %", L"dlaa.ini fps_y", L"dlaa.ini fps_vr_y", L"", nullptr, nullptr, nullptr },
    { G_FPSG, false, M_B, false, L"FPS counter size", L"1.0", L"0.3", L"dlaa.ini fps_scale", L"dlaa.ini fps_vr_scale", L"", nullptr, nullptr, nullptr },
    { G_FPSG, false, M_B, false, L"FPS colour", L"white", nullptr, L"dlaa.ini fps_color", nullptr, L"", nullptr, nullptr, nullptr },
    { G_FPSG, false, M_B, false, L"FPS brightness", L"100 %", nullptr, L"dlaa.ini fps_brightness", nullptr, L"", nullptr, nullptr, nullptr },
    { G_FPSG, false, M_B, false, L"FPS opacity", L"100 %", nullptr, L"dlaa.ini fps_opacity", nullptr, L"", nullptr, nullptr, nullptr },   // phase 51f
    { G_FPSG, false, M_B, false, L"FPS graph length", L"12 s", nullptr, L"dlaa.ini fps_graph_secs", nullptr, L"", nullptr, nullptr, nullptr },   // phase 88
    { G_FPSG, false, M_V, false, L"FPS counter lock", L"", L"Follow head", L"dlaa.ini fps_vr_lock", nullptr, L"", nullptr, nullptr, nullptr },
    { G_FPSG, false, M_V, true,  L"Position per vehicle", L"", L"On", L"dlaa.ini fps_profiles", nullptr, L"", nullptr, nullptr, nullptr },   // phase 51b (attached only)
    { G_FPSG, false, M_V, true,  L"Re-measure cab spot", L"", L"-", L"Right: now, Left: defaults", nullptr, L"", nullptr, nullptr, nullptr },   // phase 51c / 51d (attached only)
    { G_FPSG, false, M_V, false, L"FPS turn", L"", L"+0 deg", L"dlaa.ini fps_vr_yaw", nullptr, L"", nullptr, nullptr, nullptr },
    { G_FPSG, false, M_V, false, L"FPS tilt", L"", L"+0 deg", L"dlaa.ini fps_vr_pitch", nullptr, L"", nullptr, nullptr, nullptr },
    { G_FPSG, false, M_V, false, L"FPS rotate", L"", L"+0 deg", L"dlaa.ini fps_vr_roll", nullptr, L"", nullptr, nullptr, nullptr },
    { G_FPSG, false, M_V, false, L"FPS distance", L"", L"0.80 m", L"dlaa.ini fps_vr_dist", nullptr, L"", nullptr, nullptr, nullptr },
    // ---- Menu panel group ----
    { G_PANEL, true, M_B, false, L"Menu panel", L"", nullptr, L"", nullptr, L"", nullptr,
      L"The panel's position, size, depth, facing and the menu keys. Right opens the group, Left closes it; it starts closed every "
      L"time the menu opens.", nullptr },
    { G_PANEL, false, M_B, false, L"Panel position X", L"0 %", L"+0 %", L"dlaa.ini menu_x", L"dlaa.ini menu_vr_x", L"", nullptr, nullptr, nullptr },
    { G_PANEL, false, M_B, false, L"Panel position Y", L"0 %", L"+0 %", L"dlaa.ini menu_y", L"dlaa.ini menu_vr_y", L"", nullptr, nullptr, nullptr },
    { G_PANEL, false, M_B, false, L"Panel size", L"1.0", nullptr, L"dlaa.ini menu_scale", L"dlaa.ini menu_vr_scale", L"", nullptr, nullptr, nullptr },
    { G_PANEL, false, M_B, false, L"Menu keys", L"Delete", nullptr, L"Up / Down / Left / Right", nullptr, L"", nullptr, nullptr, nullptr },
    { G_PANEL, false, M_V, false, L"Panel lock", L"", L"Follow head", L"dlaa.ini menu_vr_lock", nullptr, L"", nullptr, nullptr, nullptr },   // phase 70
    { G_PANEL, false, M_V, false, L"Panel depth", L"", L"1.00 m", L"dlaa.ini menu_vr_dist", nullptr, L"", nullptr, nullptr, nullptr },
    { G_PANEL, false, M_V, false, L"Panel faces you", L"", L"on", L"dlaa.ini menu_vr_face", nullptr, L"", nullptr, nullptr, nullptr },
    // ---- Compare and test ----
    { G_CMP, true, M_B, false, L"Compare and test", L"", nullptr, L"", nullptr, L"", nullptr,
      L"Switches for comparing: DLAA off, the game's own TAA mixed in or side by side, the wire fixes, running DLAA earlier. Nothing "
      L"here is needed for normal driving. Right opens the group, Left closes it.", nullptr },
    { G_CMP, false, M_B, false, L"DLAA off to compare (not saved)", L"on", nullptr, L"Shift+F11", nullptr, L"<c>0.31</> | <g>3.4</>", L"<c>0.28</> | <g>1.9</>", nullptr, nullptr },
    { G_CMP, false, M_B, false, L"Split with game TAA", L"off", nullptr, L"dlaa.ini game_taa", nullptr, L"", nullptr, nullptr, nullptr },
    { G_CMP, false, M_B, true,  L"Split line position", L"50 %", nullptr, L"dlaa.ini game_taa_split", nullptr, L"", nullptr, nullptr, nullptr },   // split off -> hidden
    { G_CMP, false, M_B, true,  L"Split: left | right", L"DLAA | TAA", nullptr, L"dlaa.ini game_taa_pair", nullptr, L"", nullptr, nullptr, nullptr },   // split off -> hidden
    { G_CMP, false, M_B, false, L"Wire fix: motion", L"on", nullptr, L"Alt+F10", nullptr, L"", nullptr, nullptr, nullptr },          // phase 69
    { G_CMP, false, M_B, false, L"Wire fix: depth", L"on", nullptr, L"Alt+F11", nullptr, L"", nullptr, nullptr, nullptr },
    { G_CMP, false, M_B, false, L"Thicker wires (test)", L"off", nullptr, L"Alt+F12", nullptr, L"", nullptr, nullptr, nullptr },
    { G_CMP, false, M_F, false, L"Run DLAA earlier (test)", L"off", nullptr, L"Alt+F7", nullptr, L"same", nullptr, nullptr, nullptr },   // flat only
    // ---- Bug report ----
    { G_BUG, true, M_B, false, L"Bug report", L"", nullptr, L"", nullptr, L"", nullptr,
      L"Bug report: press the capture key twice (or Capture now) where something looks wrong; the mod writes one zip you attach to "
      L"a GitHub issue. Right opens the group, Left closes it.", nullptr },
    { G_BUG, false, M_B, false, L"Send everything", L"off", nullptr, L"dlaa.ini report_all", nullptr, L"", nullptr, nullptr, nullptr },   // phase 89d
    { G_BUG, false, M_B, false, L"Log detail", L"errors", nullptr, L"dlaa.ini log_level", nullptr, L"<c>0.01</> | -", nullptr, nullptr, nullptr },   // phase 39 (moved here)
    { G_BUG, false, M_B, false, L"Rewind before key", L"off", nullptr, L"dlaa.ini rewind_secs", nullptr, L"", nullptr, nullptr, nullptr },
    { G_BUG, false, M_B, false, L"Capture includes", L"Quick", nullptr, L"dlaa.ini capture_mode", nullptr, L"", nullptr, nullptr, nullptr },
    { G_BUG, false, M_B, false, L"Capture key", L"Insert (press twice)", nullptr, L"Right: change", nullptr, L"", nullptr, nullptr, nullptr },
    { G_BUG, false, M_B, false, L"Keep captures", L"all", nullptr, L"dlaa.ini capture_keep", nullptr, L"", nullptr, nullptr, nullptr },
    { G_BUG, false, M_B, false, L"Capture now", L"ready", nullptr, L"Right", nullptr, L"", nullptr, nullptr, nullptr },
    // ---- top level (end) ----
    { -1, false, M_B, false, L"Language", L"auto (English)", nullptr, L"dlaa.ini language", nullptr, L"", nullptr, nullptr, nullptr },   // phase 74
    { -1, false, M_B, false, L"Hotkeys...", L"32 actions", nullptr, L"Right: open", nullptr, L"", nullptr, nullptr, nullptr },   // v0.13.0 phase 50
    { -1, false, M_B, false, L"Save to dlaa.ini", L"-", nullptr, L"Shift+F12", nullptr, L"", nullptr, nullptr, nullptr },
    { -1, false, M_B, false, L"Reset to defaults", L"-", nullptr, L"hold Right 5 s", nullptr, L"", nullptr, nullptr, nullptr },
    { -1, false, M_B, false, L"Close menu", L"-", nullptr, L"Delete", nullptr, L"", nullptr, nullptr, nullptr },
};
constexpr int kNRows = (int)(sizeof(kRows) / sizeof(kRows[0]));
static_assert(kNRows <= TuningMenu::kMaxRows, "menu_shot: more rows than the panel holds");

// the perf block (3 lines): VR (GPU = both eyes, headset 72 Hz) and flat (single eye, GPU-limited)
static const wchar_t kPerf0[]  = L"Limit: headset (72 Hz)      fps 72 (13.9 ms, headset 72 Hz)      frame busy <c>6.1</> | <g>9.8</> ms";
static const wchar_t kPerf1[]  = L"this mod: <c>0.31</> | <g>3.4</> ms -- GPU: NGX 1.2, part motion 0.7, camera 0.4, see-through 0.5, "
                                 L"mirrors 0.3, sharpen 0.1, copies 0.2";
static const wchar_t kPerf2[]  = L"<c>CPU</> | <g>GPU</> ms per frame (GPU: both eyes), other threads 0.4. CPU = the game's render thread; "
                                 L"measuring costs 0.12 ms CPU (taken out).";
static const wchar_t kPerf0F[] = L"Limit: GPU      fps 141 (7.1 ms)      frame busy <c>5.2</> | <g>6.8</> ms";
static const wchar_t kPerf1F[] = L"this mod: <c>0.28</> | <g>1.9</> ms -- GPU: NGX 0.6, part motion 0.35, camera 0.2, see-through 0.26, "
                                 L"mirrors 0.17, sharpen 0.06, copies 0.1";
static const wchar_t kPerf2F[] = L"<c>CPU</> | <g>GPU</> ms per frame, other threads 0.4. CPU = the game's render thread; measuring costs "
                                 L"0.12 ms CPU (taken out).";
static const wchar_t kTitle[]  = L"ETS2 / ATS DLAA & DLSS mod v0.17.0 - tuning menu";
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

// phase 74: menu_shot.exe <dir> <xx_yy.po> <code> renders the main shots in that language (Lang::LoadForTest + its font)
static bool g_lang = false;
static const wchar_t* T(const wchar_t* s) { return s && s[0] && g_lang ? Lang::Tr(s) : s; }

// ---- the panel -----------------------------------------------------------------------------------------------------------
static void Put(wchar_t* dst, size_t cap, const std::wstring& s) { wcsncpy_s(dst, cap, s.c_str(), _TRUNCATE); }

// is a row shown in this mode (vr) and, when not showAll, not hidden on this machine?
static bool RowInShot(const E& e, bool vr, bool showAll) {
    if (e.mode == M_V && !vr) return false;
    if (e.mode == M_F && vr) return false;
    if (e.hidden && !showAll) return false;
    return true;
}

// open[g]: group g shown. selName: the selected row (by name). vr = VR / flat shot; showAll = the all-open shot (every row of
// this mode, ungreyed).
static void BuildPanel(TuningMenu::Panel& p, bool vr, bool showAll, const bool open[G_COUNT], const wchar_t* selName) {
    memset(&p, 0, sizeof(p));
    swprintf(p.title, sizeof(p.title) / sizeof(wchar_t), L"%ls - %ls", kTitle, vr ? L"VR settings" : L"Flat settings");
    Put(p.perf[0], 224, Ink(vr ? kPerf0 : kPerf0F));
    Put(p.perf[1], 224, Ink(vr ? kPerf1 : kPerf1F));
    Put(p.perf[2], 224, Ink(vr ? kPerf2 : kPerf2F));
    swprintf(p.costHead, 24, L"%lcCPU%lc | %lcGPU%lc ms", kInkCpu, kInkEnd, kInkGpu, kInkEnd);
    wcsncpy_s(p.footer, kFooter, _TRUNCATE);
    int nv = 0;
    const E* sel = nullptr;
    for (int i = 0; i < kNRows; ++i) {
        const E& e = kRows[i];
        if (!RowInShot(e, vr, showAll)) continue;                 // mode / machine-hidden filter
        if (e.grp >= 0 && !e.hdr && !open[e.grp]) continue;       // a collapsed group's children are not in the panel
        TuningMenu::Row& r = p.rows[nv];
        if (e.hdr)           swprintf(r.name, 64, L"%lc %ls", open[e.grp] ? (wchar_t)0x25BC : (wchar_t)0x25BA, T(e.name));   // as MenuBuildPanel
        else if (e.grp >= 0) swprintf(r.name, 64, L"    %ls", T(e.name));                                         // (phase 46b)
        else                 wcsncpy_s(r.name, T(e.name), _TRUNCATE);
        const wchar_t* keys = (vr && e.keysVr) ? e.keysVr : e.keys;
        wcsncpy_s(r.keys, keys, _TRUNCATE);
        r.greyed = 0;
        if (e.hdr) {
            int n = 0;
            for (int j = 0; j < kNRows; ++j)
                if (kRows[j].grp == e.grp && !kRows[j].hdr && RowInShot(kRows[j], vr, showAll)) ++n;
            if (open[e.grp]) wcsncpy_s(r.value, T(L"open"), _TRUNCATE); else swprintf(r.value, 96, g_lang ? Lang::TrF(L"closed (%d)") : L"closed (%d)", n);
            swprintf(r.keys, 96, L"%ls: %ls", open[e.grp] ? L"Left" : L"Right", open[e.grp] ? T(L"close") : T(L"open"));
        } else {
            const wchar_t* v = (vr && e.valueVr) ? e.valueVr : e.value;
            wcsncpy_s(r.value, T(v), _TRUNCATE);
        }
        const wchar_t* cost = (!vr && e.costFlat) ? e.costFlat : e.cost;
        Put(r.cost, 32, cost[0] ? Ink(cost) : std::wstring(L"-"));
        if (!wcscmp(e.name, selName)) { p.sel = nv; sel = &e; }
        ++nv;
    }
    p.nRows = nv;
    if (g_lang) {
        wcsncpy_s(p.colHead[0], T(L"Setting"), _TRUNCATE);
        wcsncpy_s(p.colHead[1], T(L"Value"), _TRUNCATE);
        wcsncpy_s(p.colHead[2], T(L"Hotkey"), _TRUNCATE);
    }
    if (sel && sel->desc) {
        if (sel->sent) swprintf(p.desc, 800, L"%ls %ls", T(sel->desc), T(sel->sent));
        else           wcsncpy_s(p.desc, T(sel->desc), _TRUNCATE);
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
static void MenuShot(Gpu& g, const wchar_t* file, bool vr, bool showAll, const bool open[G_COUNT], const wchar_t* sel) {
    TuningMenu::Panel p;
    BuildPanel(p, vr, showAll, open, sel);
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

// v0.13.0 phase 50: the Hotkeys page (mirrors MenuBuildHotkeysPanel + kMenuHkLines in src/inject.cpp: "< Back" + one line per
// action, the Keyboard / Controller columns). The default controller column is "-" everywhere (no controller binding ships). The
// "Sharpen strength +" line waits for a press (the wide bind-mode line).
static void HotkeysShot(Gpu& g, const wchar_t* file) {
    struct L { const wchar_t* name; const wchar_t* kb; const wchar_t* pad; };
    static const L kL[] = {
        { L"Mode (DLAA / DLSS / off)", L"End", L"-" }, { L"Performance profile", L"Alt+F9", L"-" },
        { L"DLAA model 1 (K)", L"Shift+F1", L"-" }, { L"DLAA model 2 (E)", L"Shift+F2", L"-" },
        { L"DLAA model 3 (F)", L"Shift+F3", L"-" }, { L"DLAA model 4 (M)", L"Shift+F4", L"-" },
        { L"Sharpen strength -", L"Shift+F7", L"-" }, { L"Sharpen strength +", L"Shift+F8", L"-" },
        { L"Sharpen width -", L"Shift+F9", L"-" }, { L"Sharpen width +", L"Shift+F10", L"-" },
        { L"Texture sharpness -", L"Ctrl+F1", L"-" }, { L"Texture sharpness +", L"Ctrl+F2", L"-" },
        { L"Cut-out sharpness -", L"Ctrl+Shift+F1", L"-" }, { L"Cut-out sharpness +", L"Ctrl+Shift+F2", L"-" },
        { L"DLAA area (VR) -", L"Shift+F5", L"-" }, { L"DLAA area (VR) +", L"Shift+F6", L"-" },
        { L"Per-part motion vectors", L"Alt+F5", L"-" }, { L"Mirror DLAA on / off", L"Alt+F6", L"-" },
        { L"DLAA before tonemap (flat)", L"Alt+F7", L"-" }, { L"See-through depth", L"Ctrl+F3", L"-" },
        { L"Thin-wire motion (DLAA)", L"Alt+F10", L"-" }, { L"Thin-wire depth", L"Alt+F11", L"-" }, { L"Thin-wire widening", L"Alt+F12", L"-" },
        { L"DLSS upscale", L"Ctrl+F4", L"-" }, { L"DLAA on / off", L"Shift+F11", L"-" },
        { L"Shimmer capture", L"Insert", L"-" }, { L"Save to dlaa.ini", L"Shift+F12", L"-" },
        { L"Menu: open / close", L"Delete", L"-" }, { L"Menu: up", L"Up", L"-" }, { L"Menu: down", L"Down", L"-" },
        { L"Menu: left", L"Left", L"-" }, { L"Menu: right", L"Right", L"-" },
    };
    constexpr int kN = (int)(sizeof(kL) / sizeof(kL[0]));
    static_assert(kN + 1 <= TuningMenu::kMaxRows, "menu_shot: hotkeys page too long");
    const int bindLine = 8;                                        // "Sharpen strength +"
    TuningMenu::Panel p;
    memset(&p, 0, sizeof(p));
    wcsncpy_s(p.title, L"ETS2 / ATS DLAA & DLSS mod v0.17.0 - hotkeys", _TRUNCATE);
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
    // fps-counter.png (phase 88): the default white box over the road picture WITH its history graph -- a 72 Hz headset, mostly on
    // time, a busy patch and two hitches (DrawWidget draws the strip under the text; SetWidgetGraph feeds the columns)
    {
        const float s = 2.0f;
        const int W = (int)(TuningMenu::kWidgetRefW * s) + 30, H = (int)(TuningMenu::kWidgetRefHMax * s) + 26;
        TuningMenu::WidgetGraph gr;
        gr.on = true; gr.gen = 1; gr.targetMs = 13.9f;
        for (int i = 0; i < TuningMenu::kGraphCols; ++i) gr.colMs[i] = 13.4f + 0.5f * (float)((i * 7) % 3);
        gr.colMs[9] = 19.5f; gr.colMs[10] = 18.0f; gr.colMs[11] = 17.2f;   // a busy city
        gr.colMs[24] = 41.0f;                                               // a hitch
        gr.colMs[35] = 27.0f;
        TuningMenu::SetWidgetGraph(gr);
        std::vector<uint32_t> px;
        TuningMenu::SetWidgetColors(255, 255, 255, 8, 8, 8);
        const bool ok = Render(g, W, H, [&](ID3D11DeviceContext* c, ID3D11RenderTargetView* rtv, uint32_t w, uint32_t h) {
            return TuningMenu::DrawWidget(c, rtv, w, h, 15.0f, 13.0f, s, s, TuningMenu::kOutRaw, L"72 fps", L"13.9 ms");
        }, px);
        TuningMenu::ReleaseWidget();
        TuningMenu::SetWidgetGraph(TuningMenu::WidgetGraph());
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
    bool none[G_COUNT] = {}, vrp[G_COUNT] = {}, pic[G_COUNT] = {}, all[G_COUNT];
    vrp[G_VRP] = true; pic[G_PIC] = true;
    for (bool& o : all) o = true;
    if (argc >= 4) {                                                              // phase 74: one language (VR shots)
        wchar_t po[MAX_PATH];
        MultiByteToWideChar(CP_ACP, 0, argv[2], -1, po, MAX_PATH);
        if (!Lang::LoadForTest(po, argv[3])) { printf("cannot read %s\n", argv[2]); return 1; }
        g_lang = true;
        TuningMenu::SetFace(Lang::Face());
        printf("language %s, font %ls%s\n", argv[3], Lang::Face(), Lang::FontMissing() ? " (FONT MISSING: English)" : "");
        std::wstring code(argv[3], argv[3] + strlen(argv[3]));
        MenuShot(g, (L"menu-" + code + L".png").c_str(), /*vr*/true, /*showAll*/false, none, L"Mode");
        MenuShot(g, (L"menu-" + code + L"-all-open.png").c_str(), /*vr*/true, /*showAll*/true, all, L"Mode");
        g.ctx->Release();
        g.dev->Release();
        return g_fail ? 1 : 0;
    }
    // Flat and VR each: the closed main view, and an all-open view that shows every row of that mode ungreyed.
    MenuShot(g, L"tuning-menu.png", /*vr*/false, /*showAll*/false, none, L"Mode");
    MenuShot(g, L"tuning-menu-vr.png", /*vr*/true, /*showAll*/false, none, L"Mode");
    MenuShot(g, L"tuning-menu-all-open.png", /*vr*/false, /*showAll*/true, all, L"Mode");
    MenuShot(g, L"tuning-menu-all-open-vr.png", /*vr*/true, /*showAll*/true, all, L"Mode");
    MenuShot(g, L"tuning-menu-vr-performance.png", /*vr*/true, /*showAll*/false, vrp, L"Foveated rendering");
    MenuShot(g, L"tuning-menu-picture.png", /*vr*/true, /*showAll*/false, pic, L"Sharpen strength");
    HotkeysShot(g, L"tuning-menu-hotkeys.png");                                   // v0.13.0 phase 50: the Hotkeys page
    FpsShots(g);
    g.ctx->Release();
    g.dev->Release();
    printf("%s\n", g_fail ? "FAILED" : "all images written");
    return g_fail ? 1 : 0;
}
