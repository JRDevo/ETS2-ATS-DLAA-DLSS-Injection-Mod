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
// switched off).
#pragma once
#ifdef WITH_DLAA

#include <d3d11.h>
#include <cstdint>

namespace TuningMenu {

constexpr int   kMaxRows  = 26;                      // (was 24 before the 4 fps box rows)
constexpr int   kPerfLines = 3;                      // the GPU block under the title (small font, one line each)
constexpr float kRefWidth = 760.0f;                  // panel width in reference px (scale 1; was 720 before the GPU cost column)
constexpr float kWidgetRefW = 92.0f;                 // the fps box in reference px (scale 1)
constexpr float kWidgetRefH = 44.0f;

struct Row {
    wchar_t name[48];
    wchar_t value[40];
    wchar_t keys[72];                                // the hotkey(s) of the row + a "VR only" / "flat only" tag
    wchar_t cost[24];                                // the "GPU cost" column ("1.2 ms", "off", "same", "-")
    unsigned char greyed;                            // 1 = not applicable in the current mode (drawn grey)
};
// Fill with memset 0 first: Draw compares the whole struct to decide whether the texture must be rebuilt (the caller keeps
// every text stable between its once-a-second GPU snapshots, so the texture is not rebuilt every frame).
struct Panel {
    wchar_t title[112];
    wchar_t perf[kPerfLines][224];                   // the GPU block (game / fps, the mod's cost per eye)
    wchar_t costHead[24];                            // the 4th column's header ("GPU cost" / "GPU cost (eye 0)")
    Row     rows[kMaxRows];
    int     nRows;
    int     sel;
    wchar_t desc[480];                               // the selected row's description + its cost sentence (word-wrapped, 3 lines)
    wchar_t footer[96];
};

void  RegisterShaders();                             // VS + PS into ShaderCache (setup thread, before ShaderCache::Start)
float RefHeight(int nRows);                          // panel height in reference px
// Draws `p` into `rtv` (a target of rtW x rtH px) with its top-left corner at (x0, y0) px and `scale` px per reference
// px. The texture is rendered at `buildScale` (the GPU scales it by scale / buildScale) so two targets of different
// sizes in one frame (VR eyes + the desktop mirror) do not rebuild it twice per frame. outMode = how the target stores
// colour (OutMode below). False = not drawn (device objects unavailable / shader warm-up still running; logged once).
enum OutMode : int {
    kOutRaw    = 0,                                  // 8-bit UNORM view: the sRGB panel bytes as they are
    kOutLinear = 1,                                  // _SRGB view (the hardware encodes) or scRGB float: decoded to linear (white = 1.0)
    kOutPq     = 2,                                  // HDR10 (R10G10B10A2, PQ / Rec.2020): white at 200 nits, PQ-encoded
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

}  // namespace TuningMenu

#endif // WITH_DLAA
