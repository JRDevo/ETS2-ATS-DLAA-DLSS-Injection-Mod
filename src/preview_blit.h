// PreviewBlit (v0.7.8 round 4) -- format-converting full-target copy for the profile-screen preview DLAA.
// The preview composite target in FLAT is the B8G8R8A8 backbuffer; SceneDlaa works on R8G8B8A8 textures (CopyResource
// cannot convert BGRA <-> RGBA). A fullscreen draw that Loads through an SRV and writes through an RTV does the
// conversion for free (a Texture2D<float4> load returns the logical r, g, b, a whatever the memory order is), so the
// caller copies target -> RGBA scratch with it, runs SceneDlaa in place on the scratch, and copies the result back.
// Views are made by the caller with the UNORM (non-sRGB) format, so the gamma-encoded bytes travel as they are, like
// the world path feeds NGX the encoded bytes of the sRGB tonemap texture.
// The draw changes graphics state: SaveState / RestoreState bracket every piece of it (IA layout / topology,
// VS / HS / DS / GS / PS + class instances, PS SRV0, RS state + viewports, blend, depth-stencil, render targets + DSV).
// Not touched, hence not saved: vertex / index buffers (SV_VertexID only), constant buffers, samplers (Load only),
// scissor rects (our RS state has scissor off), OM UAVs, stream-out, predication. Render thread only, one device.
// Compiled only when WITH_DLAA=1.
#pragma once
#ifdef WITH_DLAA

#include <d3d11.h>
#include <cstdint>

namespace PreviewBlit {
void RegisterShaders();                               // v0.7.8: VS + PS into ShaderCache (setup thread, before Start)
bool Ensure(ID3D11Device* dev);                       // shaders + states (once); false = unusable (logged)
void SaveState(ID3D11DeviceContext* ctx);
void RestoreState(ID3D11DeviceContext* ctx);
// dst RT0 <- src (Load, 1:1, RGB written, alpha untouched, no blending); viewport = (0, 0, w, h). Between SaveState
// and RestoreState only. The PS SRV0 is unbound again afterwards.
void Draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst, uint32_t w, uint32_t h);
}

#endif // WITH_DLAA
