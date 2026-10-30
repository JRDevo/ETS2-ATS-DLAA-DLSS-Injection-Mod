// LightFs -- phase 78: the lighting pass's FULL-SCREEN draws get the G-buffer's jitter shift (dlaa.ini light_fs_jitter). The
// pure geometry of the fix: inject.cpp (hkDraw -> LightFsEdgeFill) and tests/light_fs_test.cpp (WARP) use it.
//
// A full-screen draw shifted by s with the game's W x H viewport (no overscan: its UV must map 1:1 onto the G-buffer texels)
// loses, by the phase-23 edge rule (the rasterizer cuts the far edge at floor(TopLeft + size)), the LAST column when s.x < 0 and
// the LAST row when s.y < 0. LightFsEdgeRects returns the scissor rects of exactly those pixels: the mod draws them again with the
// game's own (unshifted) viewport, so every pixel is written once (additive light blends stay correct).
#pragma once
#include <cmath>
#include <d3d11.h>

namespace LightFs {

// The pixels the shifted W x H viewport `gv` + (sx, sy) does not rasterise but the unshifted one does, as up to 2 disjoint rects:
// [0] the far column(s), full height of the game's viewport; [1] the far row(s), left of that column (the corner once). Returns
// the count (0 = nothing lost: both shifts >= 0, or a viewport whose far edge does not move across a pixel boundary).
inline int EdgeRects(const D3D11_VIEWPORT& gv, float sx, float sy, D3D11_RECT r[2]) {
    const LONG xa = (LONG)std::floor(gv.TopLeftX + sx + gv.Width), xb = (LONG)std::floor(gv.TopLeftX + gv.Width);
    const LONG ya = (LONG)std::floor(gv.TopLeftY + sy + gv.Height), yb = (LONG)std::floor(gv.TopLeftY + gv.Height);
    const LONG x0 = (LONG)std::floor(gv.TopLeftX), y0 = (LONG)std::floor(gv.TopLeftY);
    int n = 0;
    if (xa < xb) r[n++] = { xa, y0, xb, yb };
    if (ya < yb) r[n++] = { x0, ya, xa < xb ? xa : xb, yb };
    return n;
}

// The edge rect `e` limited to the game's own scissor rect `sc` (when its rasterizer state enables one); false = empty.
inline bool Clip(D3D11_RECT& e, const D3D11_RECT& sc) {
    if (e.left < sc.left) e.left = sc.left;
    if (e.top < sc.top) e.top = sc.top;
    if (e.right > sc.right) e.right = sc.right;
    if (e.bottom > sc.bottom) e.bottom = sc.bottom;
    return e.left < e.right && e.top < e.bottom;
}

}  // namespace LightFs
