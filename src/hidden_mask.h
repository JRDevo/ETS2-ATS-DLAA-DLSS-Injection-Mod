// HiddenMask -- phase 40: the VR HIDDEN AREA MASK (dlaa.ini xr_hidden_mask; the OpenXR side is in xr_hook.h, the decision of
// when / which eye in inject.cpp, the block above HmOnSceneClear).
//
// Why: the corners of each eye picture are never seen through the lenses, yet the game shades them in every pass. The OpenXR
// runtime describes them (XR_KHR_visibility_mask, the HIDDEN triangle mesh per view). Drawn into the eye's scene depth right after
// the game clears it, at the NEAREST depth value, every later game draw fails the depth test there (early-Z / Hi-Z): no G-buffer,
// light, sky or forward shading in those pixels.
//
// What this module does (render thread, the game's immediate context, inside the mod's own work):
//  * MapToNdc (pure, no D3D): the runtime's vertices lie on the z = -1 plane of the view (x, y = view tangents); with the fov the
//    game submits for that view (tan L / R / U / D) they map linearly to the picture's NDC:
//        x_ndc = (2 x - (tan R + tan L)) / (tan R - tan L),   y_ndc = (2 y - (tan U + tan D)) / (tan U - tan D)
//    (y up, as D3D NDC; the viewport turns it into pixel rows from the top). The triangle list is expanded (3 vertices per
//    triangle, no index buffer). Checks: finite, the mapped mesh must reach near the picture border (it is the lens corner area)
//    and cover between 0.2 % and 60 % of the picture (Coverage, a 64 x 64 sample grid) -- else it is not used (`why`).
//  * Upload: one immutable vertex buffer per view (float3: NDC x, y + the depth value), rebuilt only when the mesh, the fov or
//    the depth value changes.
//  * Draw: depth only -- no render target, null pixel shader, depth test ALWAYS + write ALL, stencil off (the stencil the game
//    just cleared stays as it is), no cull, viewport = the whole depth texture. Every piece of state it changes is saved and
//    restored: PreviewBlit::SaveState / RestoreState (IA layout + topology, VS / HS / DS / GS / PS + class instances, PS SRV0,
//    RS state + viewports, blend, depth-stencil state + ref, render targets + DSV) + the IA vertex buffer slot 0 here.
// Compiled only with WITH_DLAA (PreviewBlit, ShaderCache).
#pragma once
#ifdef WITH_DLAA

#include <d3d11.h>
#include <cstdint>

namespace HiddenMask {

constexpr uint32_t kMaxTris = 1024;                  // = XrHook::kHiddenMaxIdx / 3

struct MapInfo {
    uint32_t    tris = 0;
    float       ndcMin[2] = {}, ndcMax[2] = {};       // mapped bounds (x, y)
    float       coverage = 0.0f;                     // fraction of the picture covered (0..1)
    const char* why = "";                            // "" = usable, else why not
};
// The hidden mesh (xy = nv vertices (x, y), idx = ni indices, a triangle list) of a view rendered with `fovRad` (L, R, U, D) ->
// out = 3 * tris NDC vertices (x, y), at most kMaxTris triangles. True = usable (info->why == "").
bool MapToNdc(const float* xy, uint32_t nv, const uint32_t* idx, uint32_t ni, const float fovRad[4], float* out, MapInfo* info);
// Fraction of the NDC square [-1, 1]^2 inside the triangles (sample grid `grid` x `grid`, pixel centres).
float Coverage(const float* triXy, uint32_t tris, int grid);

void RegisterShaders();                              // SetupThread, before ShaderCache::Start
bool Ensure(ID3D11Device* dev);                      // shader + states once per device; false = not yet / failed (logged once)
// Vertex buffer `slot` (0 / 1 = OpenXR view) for `tris` NDC triangles at depth `depth`. False = failed (logged).
bool Upload(ID3D11Device* dev, int slot, const float* triXy, uint32_t tris, float depth);
bool HasMesh(int slot);
// Draws mesh `slot` into `dsv` (a w x h depth texture). Saves + restores all state it touches. Caller: t_inDlaa set.
bool Draw(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, uint32_t w, uint32_t h, int slot);
void Reset();                                        // device change: everything released

} // namespace HiddenMask

#endif // WITH_DLAA
