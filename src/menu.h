// TuningMenu (v0.10.0) -- the in-game tuning panel (Delete opens / closes, arrows select / change).
// This module only DRAWS: inject.cpp owns the menu state (open, selected row, the values, the keys) and fills a Panel
// each frame the menu is open; Draw renders it with GDI into a 32-bit DIB (only when the panel's text or its build scale
// changed), uploads it into an R8G8B8A8_UNORM texture (UpdateSubresource on the game's context) and draws it as ONE
// textured quad over the given render target (alpha = max(0.88, luminance), RGB write mask only).
// The draw changes graphics state: it saves and restores every piece it touches (IA layout / topology, VS / HS / DS /
// GS / PS + class instances, VS + PS constant buffer 0, PS SRVs 0..15 (phase 16: was 0), PS sampler 0, RS state + viewports, blend,
// depth-stencil, render targets + DSV) -- the PreviewBlit pattern plus the constant buffers and the sampler. The caller
// sets t_inDlaa around Draw so our own Draw call passes straight through the hooks. Render thread only, one device at a
// time: a new device drops every object of the old one (they are recreated lazily). Compiled only when WITH_DLAA=1.
// DrawWidget (v0.10.0) draws the small live fps box (two lines: "60 fps" / "16.7 ms") through the same shaders, states and
// save / restore, from its OWN texture cache (rebuilt only when one of its two lines or its build scale changes; the panel's
// cache is separate). It has NO background and NO frame: white text with a dark (near-black) outline, so it reads over any
// picture (the texture carries its own alpha, the shader uses it as it is). ReleaseWidget drops that texture (the fps box
// switched off). Phase 44: the text / outline colours are set by SetWidgetColors (dlaa.ini fps_color presets).
#pragma once
#ifdef WITH_DLAA

#include <d3d11.h>
#include <cstdint>

namespace TuningMenu {

constexpr int   kMaxRows  = 60;                      // (phase 52: 50 -> 56 -> 60 at the 51b merge, + the Frame rate group header and its 3 rows; was 24 before the 4 fps box rows, 26 before the Reset row, 27 before v0.10.2 Menu keys,
                                                     //  28 before the v0.10.3 phase 28 FPS counter lock row, 29 before the phase 29c
                                                     //  group header rows, 31 before the phase 30 Turbo mode row, 32 before the
                                                     //  phase 34 Foveated rows, 34 before the phase 39 Logging row, 35 before the
                                                     //  phase 41 Mirrors in turns row, 36 before the phase 40 Hidden area mask row,
                                                     //  37 before the phase 42 Foveated passes row, 38 before the 4 phase 43 category headers, 42 before the
                                                     //  5 phase 44 fps box rows (colour, distance, turn, tilt, rotate), 48 before the
                                                     //  phase 49 Light glow ghosting fix row, 50 before the phase 51b Position per vehicle row; a Panel holds only the VISIBLE rows,
                                                     //  nRows <= this; phase 52: + Frame rate group (4), phase 54: + CPU tuning)
constexpr int   kPerfLines = 3;                      // the perf block under the title (small font, one line each)
// phase 45: INK MARKS in the perf lines, the cost column cells and its header: kInkCpu / kInkGpu switch the text colour to the
// CPU blue / the GPU orange (blue / orange, not red / green: readable with the common colour-vision deficiencies), kInkEnd back
// to the line's own colour. Control characters, never drawn; a text without them draws exactly as before. (Build the strings
// with %lc -- an "" literal followed by a hex digit would swallow it.)
constexpr wchar_t kInkCpu = 0x01, kInkGpu = 0x02, kInkEnd = 0x03;
constexpr float kRefWidth = 760.0f;                  // panel width in reference px (scale 1; was 720 before the GPU cost column)
constexpr float kWidgetRefW = 92.0f;                 // the fps box in reference px (scale 1)
constexpr float kWidgetRefH = 44.0f;

struct Row {
    wchar_t name[48];
    wchar_t value[72];                               // (v0.12.1 phase 50: was 40 -- the wide bind-mode line needs ~60)
    wchar_t keys[72];                                // the hotkey(s) of the row + a "VR only" / "flat only" tag
    wchar_t cost[32];                                // the cost column: "<cpu> | <gpu>" with ink marks (phase 45), "off", "same", "-"
    unsigned char greyed;                            // 1 = not applicable in the current mode (drawn grey)
    unsigned char wide;                              // v0.12.1 phase 50: 1 = `value` spans the Value, Hotkey and Cost columns (keys /
                                                     //   cost not drawn): the Hotkeys page's "Press a key or button..." line
};
// Fill with memset 0 first: Draw compares the whole struct to decide whether the texture must be rebuilt (the caller keeps
// every text stable between its once-a-second GPU snapshots, so the texture is not rebuilt every frame).
struct Panel {
    wchar_t title[112];
    wchar_t perf[kPerfLines][224];                   // the perf block (Limit / fps / frame busy, the mod's CPU | GPU, the legend)
    wchar_t costHead[24];                            // the 4th column's header ("CPU | GPU ms" with ink marks, phase 45)
    wchar_t colHead[3][24];                          // v0.12.1 phase 50: the first 3 column headers; "" = "Setting" / "Value" /
                                                     //   "Hotkey" (the Hotkeys page: "Action" / "Keyboard" / "Controller")
    Row     rows[kMaxRows];
    int     nRows;
    int     sel;
    wchar_t desc[480];                               // the selected row's description + its cost sentence (word-wrapped, 3 lines)
    wchar_t footer[96];
};

void  RegisterShaders();                             // VS + PS into ShaderCache (setup thread, before ShaderCache::Start)
float RefHeight(int nRows);                          // panel height in reference px
// v0.10.3 phase 29c: does `footer` fit on the footer line LEFT of the licence line (which shares it, right-aligned)? Measured
// with GDI in the panel's small font (no device objects; a memory DC for the call). The caller shortens the text if not; Draw
// clips the footer at the licence line anyway (end ellipsis), so a wrong answer never overlaps the two.
bool  FooterFits(const wchar_t* footer);
// Draws `p` into `rtv` (a target of rtW x rtH px) with its top-left corner at (x0, y0) px and `scale` px per reference
// px. The texture is rendered at `buildScale` (the GPU scales it by scale / buildScale) so two targets of different
// sizes in one frame (VR eyes + the desktop mirror) do not rebuild it twice per frame. outMode = how the target stores
// colour (OutMode below). False = not drawn (device objects unavailable / shader warm-up still running; logged once).
enum OutMode : int {
    kOutRaw    = 0,                                  // 8-bit UNORM view: the sRGB panel bytes as they are
    kOutLinear = 1,                                  // _SRGB view (the hardware encodes) or scRGB float: decoded to linear (white = 1.0)
    kOutPq     = 2,                                  // HDR10 (R10G10B10A2, PQ / Rec.2020): white at 200 nits, PQ-encoded
    // v0.10.3 phase 29: OR-ed into any of the above = an OVERLAY image (the OpenXR quad layer's swapchain image): blend OFF,
    // RGBA written as the shader outputs it (colour + alpha: the panel max(0.88, luminance), the fps box its own outline
    // alpha), so the compositor gets the alpha. Without the flag every draw is as before (blend, RGB write mask).
    kOutOverlay = 0x100,
};
bool  Draw(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t rtW, uint32_t rtH, float x0, float y0,
           float scale, float buildScale, int outMode, const Panel& p);
// v0.10.0 phase 16: the panel as a PERSPECTIVE quad (the VR panel that faces the eye): `clip` = its 4 corners in clip space
// (top-left, top-right, bottom-left, bottom-right; w = the corner's view depth > 0, so the texture is mapped perspective-
// correct). Texture, states and save / restore as Draw. False = not drawn (a corner behind the eye / not finite, or as Draw).
bool  DrawCorners(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t rtW, uint32_t rtH, const float clip[4][4],
                  float buildScale, int outMode, const Panel& p);
void  ReleaseTargets();                              // drops the panel texture (menu closed: nothing kept but the shaders)
// The fps box (kWidgetRefW x kWidgetRefH reference px, outlined text on nothing) with its top-left corner at (x0, y0) px:
// line1 bold 15 px, line2 12 px (reference px), centred. Same arguments as Draw; the strings are copied into the cache key (at most 23 characters each).
bool  DrawWidget(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t rtW, uint32_t rtH, float x0, float y0,
                 float scale, float buildScale, int outMode, const wchar_t* line1, const wchar_t* line2);
void  ReleaseWidget();                               // drops the fps box texture (the box switched off)
// phase 44: the fps box as a turned quad (fps_vr_yaw / fps_vr_pitch / fps_vr_roll, the drawn-in VR fallback): `clip` = its 4
// corners in clip space as DrawCorners (top-left, top-right, bottom-left, bottom-right; w > 0). Texture / states as DrawWidget.
bool  DrawWidgetCorners(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t rtW, uint32_t rtH, const float clip[4][4],
                        float buildScale, int outMode, const wchar_t* line1, const wchar_t* line2);
// phase 47: the fps box ATTACHED TO THE VEHICLE (fps_vr_lock = 2, src/fps_cab.h): `model` = its 4 corners (top-left, top-right,
// bottom-left, bottom-right; homogeneous) in the model space of a cab draw; the vertex shader reads that draw's CURRENT MVP from
// `mirror` (a raw buffer SRV: the draw-id record's cbuffer mirror of this eye's pass) at byte `mvpByteOff` (4 rows = clip x, y, z,
// w) and draws the box where the draw's geometry is in this picture. map = (sx, sy, ox, oy): picture NDC = the draw's NDC * s + o
// ((1, 1, 0, 0) = the whole picture). Texture / states as DrawWidget (+ VS SRV slot 0 saved / restored). False = not drawn.
bool  DrawWidgetMvp(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, uint32_t rtW, uint32_t rtH, ID3D11ShaderResourceView* mirror,
                    uint32_t mvpByteOff, const float model[4][4], const float map[4], float buildScale, int outMode,
                    const wchar_t* line1, const wchar_t* line2);
// phase 44 (dlaa.ini fps_color): the fps box's text and outline colours (sRGB bytes). Default white text, RGB 8,8,8 outline (the
// colours before phase 44). A change rebuilds the box texture once (the cache key includes them).
void  SetWidgetColors(uint8_t tr, uint8_t tg, uint8_t tb, uint8_t lr, uint8_t lg, uint8_t lb);
// phase 51f (dlaa.ini fps_opacity): the fps box's alpha factor 0..1 (text AND outline); read by the pixel shader per draw, so a change
// needs no texture rebuild. Applies to every draw path (flat, VR drawn-in, OpenXR quad layer image, attached to vehicle).
void  SetWidgetOpacity(float a);

}  // namespace TuningMenu

#endif // WITH_DLAA
