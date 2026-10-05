# DLAA module — design note (v0.4.0)

Two self-contained classes, compiled only with `-DWITH_DLAA=ON`:

- **`DlaaProcessor`** (`src/dlaa.h/.cpp`) — raw NGX on the **native D3D11** path
  (no DX12 interop). DLAA = the DLSS feature created with render dims ==
  output dims (ratio 1.0), `PerfQuality = NVSDK_NGX_PerfQuality_Value_DLAA`,
  and the `DLSS.Hint.Render.Preset.DLAA` hint (left at `Default` = driver's
  pick, currently transformer preset K).
- **`CameraMv`** (`src/motion_vectors.h/.cpp`) — collects per-draw MVPs from the
  G-buffer pass and runs two compute passes that manufacture the MV texture DLSS
  needs (Prism3D has no velocity buffer). HLSL embedded as strings, compiled at
  runtime via `D3DCompile` (d3dcompiler_47, an OS DLL).

Everything is guarded by `#ifdef WITH_DLAA`; with the option OFF the build is
byte-for-byte the v0.1 target.

## Build & runtime prerequisites

```
git clone --depth 1 https://github.com/NVIDIA/DLSS external/DLSS   # the public SDK (git-ignored)
cmake -B build -G "Visual Studio 17 2022" -A x64     # WITH_DLAA defaults ON when external/DLSS exists
```

Never commit the SDK or `nvngx_dlss.dll` (NVIDIA license; `.gitignore` already
blocks them). At runtime copy `nvngx_dlss.dll` from
`<sdk>/lib/Windows_x86_64/rel/` next to `dinput8.dll` — `DlaaProcessor::Init`
adds its own DLL's folder to the NGX search path. DLAA needs an RTX GPU.

## API

```cpp
DlaaProcessor dlaa;
dlaa.Init(device, displayW, displayH);      // NGX init + caps check + feature
dlaa.Evaluate(ctx, colorSRV, depthSRV, mvSRV, outputUAV, DlaaFrameParams{});
dlaa.Shutdown();                            // also: resize = Shutdown + Init

CameraMv mv;
mv.Init(device);                            // shaders + candidate/solve buffers (size independent)
mv.Record(ctx, layer, cbRing, byteOff, key);// from the DrawIndexed hook, G-buffer pass only
mv.Generate(ctx, w, h, cur, depthTwinSrv, depthR32Uav, mvUav, &stats); // at the blit; v0.5.7: also writes the R32F depth
mv.Shutdown();
```

`DlaaFrameParams`: `jitterX/Y` (default 0), `mvScaleX/Y` (default 1), `reset`
(camera cut), `depthInverted` (reversed-Z; flipping it recreates the NGX
feature — it's a creation flag).

Buffer requirements (guide 3.4): color/depth/MV must be SRV-readable; the
output texture must be created with `D3D11_BIND_UNORDERED_ACCESS`. All at
display resolution.

## NGX call sequence (all `NVSDK_NGX_D3D11_*`, verified against the SDK headers)

1. `Init_with_ProjectID(guid, ENGINE_TYPE_CUSTOM, "0.3.1", logDir, device, &fci)`
2. `GetCapabilityParameters` → check `SuperSampling.Available` (log
   `NeedsUpdatedDriver` / min driver / `FeatureInitResult` on failure) → `DestroyParameters`
3. `AllocateParameters` → set `DLSS.Hint.Render.Preset.DLAA`
4. `NGX_D3D11_CREATE_DLSS_EXT` (helper → `CreateFeature`): `InWidth == InTargetWidth`,
   `InHeight == InTargetHeight`, `PerfQuality_Value_DLAA`; flags: `AutoExposure`
   (+ `DepthInverted` when asked). **Not** `MVLowRes` (MVs are display-res),
   **not** `MVJittered`, **not** `IsHDR` (we feed the post-tonemap backbuffer).
   **v0.7.0 DLSS upscaling** (render < output): `InWidth/InHeight` = render, `InTargetWidth/Height` = output,
   PerfQuality from the per-axis geomean ratio (nearest of 1.3 UltraQuality / 1.5 Quality / 1.7 Balanced /
   2.0 Performance / 3.0 UltraPerformance; UltraQuality retried once as Quality), `MVLowRes` set (MV + depth at
   render res, MVs in render pixels, MVScale 1), the `dlss_preset` value on every `DLSS.Hint.Render.Preset.*`.
   The output is composited into the eye RT after the game's blit (README "DLSS upscaling (v0.7.0)").
   **v0.8.0 HDR** (Windows HDR on, flat): the colour input is the game's RGBA16F scene composite (linear, values
   above 1), so the feature is created with `IsHDR` (+ `AutoExposure` as before); `dlaa.ini dlss_hdr = 0` drops
   `IsHDR`. Colour in / out / sharpen textures are RGBA16F for such a unit; LDR units are unchanged. See the v0.8.0
   header comment in `src/inject.cpp` for the HDR blit rule.
5. Per frame: `NGX_D3D11_EVALUATE_DLSS_EXT` (helper → `EvaluateFeature`) with
   color/depth/MV/output as raw `ID3D11Resource*`, jitter, MV scale, reset,
   `InRenderSubrectDimensions = {W,H}`. Sharpness stays 0 (deprecated in DLSS).
6. `ReleaseFeature` → `DestroyParameters` → `Shutdown1(device)`.

Failures log via `Log(...)` and return false — the game keeps presenting raw
frames; we never crash it.

## Motion-vector convention

- **Value:** `mv = prev_pixel_pos − curr_pixel_pos`, i.e. current + mv =
  where the pixel was last frame (DLSS guide 3.6). Units: **pixels** at
  display res, origin top-left, +y down. Texture: `RG16_FLOAT`. Hence
  **`MVScale = (1,1)`** passed to NGX.
- **Source of the matrices (v0.4.0):** only per-draw data exists. Each G-buffer
  `DrawIndexed` has VS cbuffer slot 0 = a window of one big dynamic ring buffer, bound with
  D3D11.1 offsets (`VSSetConstantBuffers1`). In the window, float4 rows 4..7 (byte offset
  64..127) are the full MVP **stored as rows**: `clip.i = dot(row_i, float4(pos,1))`. Rows 0..3
  (a model-view-like matrix) are NOT exactly consistent with the MVP, so they are not used.
  The MVP contains no jitter (our jitter is the viewport shift).
- **Clip-space R method:** for a static object drawn in both frames with `MVP_prev`, `MVP_cur`
  (same object, same world matrix): `R = MVP_prev * inverse(MVP_cur)` maps current clip space to
  previous clip space for everything static in that depth layer (`P_prev V_prev V_cur^-1 P_cur^-1`;
  the world matrix cancels). Two layers, each with its own projection: cabin/interior draws use
  viewport depth range [0.9, 1.0], world draws [0.01, 0.9]. Per pixel: layer = `d >= 0.9 ? cabin :
  world`, `z_ndc = saturate((d - minZ)/(maxZ - minZ))` (sky d == 0 -> z_ndc 0, the infinitely far
  plane in reversed-Z, so rotation-only reprojection falls out), `ndc = ((px+.5)/W*2-1, 1-(py+.5)/H*2)`,
  `c = R * (ndc, z_ndc, 1)`, `ndcPrev = c.xy / c.w`, `prevPix = ((ndcPrev.x+1)/2*W, (1-ndcPrev.y)/2*H)`,
  `mv = prevPix - (px+.5, py+.5)` (DLSS convention, pixels, +y down). MVScale (1,1), `MVJittered` off.
- **Collecting candidates:** `hkDrawIndexed` (context vtable 12), only while the current binding is
  the main G-buffer (4 RTVs + scene DSV), DLAA and MV on, immediate context, not our own work. The
  layer comes from the game's viewport[0].MinDepth (>= 0.85 = cabin). First 64 draws per layer per
  frame; per draw: `VSGetConstantBuffers1(0)` (skip if the window is < 128 bytes or runs past the
  buffer), key = (IB, VB0, IB offset, VB offset, IndexCount, StartIndexLocation, BaseVertexLocation),
  then `CopySubresourceRegion` of the 64 bytes at `firstConstant*16 + 64` from the ring buffer into
  slot k of the per-layer candidate buffer (raw buffer, 2 layers x 64 slots x 64 B, double buffered
  cur/prev, swapped at the blit). This GPU copy is plain buffer->buffer (bind flags do not matter).
  A one-time debug readback logs slot 0's 16 floats (`MV: CB-copy check`); all zeros would mean the
  copy path is dead (the planned fallback, hooking Map/Unmap on the ring and copying on the CPU,
  is NOT implemented).
- **Matching (blit time, CPU):** for each cur candidate find the prev candidate with the same key and
  the same occurrence index (the k-th draw of an identical mesh pairs with the k-th of the previous
  frame). Pairs whose key is unique in both frames are preferred, then larger IndexCount; up to 8 per layer.
- **Pass A** (`kSolveShader`, 2 threads, one per layer): per pair `R_k = Mprev * inverse(Mcur)` with a
  cofactor 4x4 inverse (singular / non-finite R dropped); the layer's R is the **medoid** (smallest
  summed Frobenius distance to the other R_k), which rejects moving objects (cars, wheels). No valid
  pair -> identity. Result in a 10 x float4 structured buffer.
- **Pass B** (`kReprojDepthShader` since v0.5.7, was `kReprojShader`; 8x8 groups): per pixel as above, reading
  the depth twin directly; writes the RG16F MV texture (replaces the zero clear) AND the flattened R32F depth
  for NGX, so `kDepthConvertShader` is skipped. Whenever pass B does not run (MV off, no candidate record,
  CameraMv init failed, Generate failed before dispatch) SceneDlaa runs the old depth convert instead.
- **Reset:** if the WORLD layer had no matched pair (first frame, camera cut, scene change) the frame
  goes to NGX with `reset = true`. HOME toggles MV on/off live (zero MVs when off), `dlaa.ini`
  `mv_enabled` sets the start state.
- **Logs:** first candidate counts per layer, the CB-copy check, every 600 frames pairs per layer /
  average / world-miss count, and the chosen `R_world` (async staging readback; ~identity when the
  camera is still).
- **Caveats:** matching is by geometry key, so a mesh drawn many times (trees) can mis-pair when
  culling changes the draw order (the medoid then decides; unique keys are preferred); movers other
  than the camera still get camera motion if they win the medoid; pixels of one layer reproject with a
  single R, so anything not static in that layer (own wheels in the world layer) ghosts.

## Jitter (viewport offset, v0.3.1)

DLSS assumes the input was rasterized with sub-pixel jitter ([-0.5,+0.5] px,
guide 3.7; Halton, >=8 phases at DLAA ratio). **ETS2/ATS apply none**, so with a
zero jitter DLAA accumulated identical samples and could not anti-alias (v0.3.0:
mechanically fine, image unchanged). v0.3.1 injects the jitter itself without
touching the per-draw MVPs (they live in a dynamic cbuffer ring): D3D11 viewports
take float `TopLeftX/Y`, so shifting the viewport by (jx,jy) pixels shifts
rasterization exactly like a clip-space jitter would.

- **Hook:** `RSSetViewports` (context vtable 44). We keep a copy of the game's
  last viewports. In a *jitter pass* with viewport[0] at scene dims we issue the
  game's viewports with viewport[0].TopLeftX/Y += (jx,jy); otherwise the game's
  unmodified viewports. Re-issued only when the game sets viewports or the state
  (pass / phase) changes.
- **Jitter pass** (decided in `hkOMSetRenderTargets`): bound DSV texture == the
  remembered scene depth AND either 4 RTVs (G-buffer) or exactly 1 RTV that is
  `R16G16B16A16_FLOAT` at scene dims (forward/transparent). The lighting pass
  (2 RTVs + scene DSV), shadow/mirror/probe passes (other DSVs), tonemap and UI
  are never jittered. Nothing is jittered while DLAA is off or `jitter_enabled=0`.
- **Sequence:** Halton(2,3), `jitter_phases` (16) phases, index = phase+1, offset =
  halton - 0.5. Advances once per frame right after our DLAA evaluate (the tonemap
  detection point), so the whole next scene uses the next phase.
- **Sign:** shifting the viewport by +jx moves the image content right by jx px
  (+jy = down). NGX wants the offset matching the projection jitter; reasoning:
  content shifted right means a given pixel centre sampled the scene at -jx. The
  default therefore passes `InJitterOffsetX = -1 * jx`, `InJitterOffsetY = -1 * jy`.
  The sign is not verified against NGX, so `dlaa.ini` has `jitter_sign_x` /
  `jitter_sign_y` (+/-1) to flip it; wrong sign shows up as shimmering/doubling.
  The frame passed to NGX is the one just rendered (phase before the advance).

## Where this plugs in (v0.3.1: the tonemap draw, not Present)

Derived from `docs/CAPTURE_FINDINGS.md`. Hooks (MinHook, addresses from the dummy
device's immediate context vtable) live in `src/inject.cpp`; the DLAA step is
`SceneDlaa::Run` in `src/scene_dlaa.cpp`.

1. `hkOMSetRenderTargets` (vtable 33): `NumViews == 4` and the DSV's texture is
   `D32_FLOAT_S8X24_UINT` (or typeless twin), width >= 1024 -> main scene G-buffer
   pass. Remember that depth texture (ref held) and its dims. The 512-wide mirror
   G-buffers are rejected by the width test.
2. `hkDraw` (vtable 13): call the original, then filter `VertexCount` 3 or 4,
   then `OMGetRenderTargets(1)`: RT0's texture `R8G8B8A8_UNORM_SRGB` with the
   scene depth's dims = the tonemap draw (pre-UI LDR scene, UI not yet drawn, so it
   stays sharp). Handled once per Present-frame; a thread-local flag stops our own
   work re-triggering the hooks.
3. DLAA step (all on the game's immediate context, compute state saved/restored:
   CS shader+instances, SRV 0..15, UAV 0..7, CB 0..13, samplers 0..15):
   - `CopyResource` tonemap (SRGB) -> `colorIn` (UNORM twin, same bytes; DLSS gets
     gamma-encoded LDR, `IsHDR` off).
   - `CopyResource` scene depth -> `R32G8X24_TYPELESS` twin; a tiny compute shader
     reads it via an `R32_FLOAT_X8X24_TYPELESS` SRV and writes an `R32_FLOAT` UAV
     texture, which is what NGX receives (`depthInverted = true`, reversed-Z).
   - Motion vectors: an `RG16_FLOAT` texture written by `CameraMv::Generate` (see "Motion-vector
     convention" above); cleared to 0 each frame only when MV is off (HOME / `mv_enabled=0`).
   - Output: `R8G8B8A8_UNORM` with UAV bind; on Evaluate success `CopyResource`
     output -> game tonemap texture, on failure the game texture is untouched.
   - NGX feature and textures are (re)created lazily when the dims change; a failed
     init is not retried until the dims change.
4. Logging: first tonemap detection, NGX init OK/failed (with reason), first 20
   Evaluate failures, heartbeat every 600 DLAA frames. A file `dlaa_off.txt` next to
   the DLL at startup makes DLAA start OFF. The End key toggles DLAA + jitter live
   (reset=true to NGX on the first frame after re-enabling).
5. `hkPresent` only counts frames and logs; DLAA no longer runs there.

Caveats: the depth partition (cabin layer in viewport range [0.9, 1.0], world in
[0.01, 0.9]) is passed to DLSS as-is; the v0.4 MV pass treats it (layer by `d >= 0.9`). Jitter is the viewport offset above.

## Open questions (v0.2 answers marked in CAPTURE_FINDINGS.md)

- Which depth target is "the" scene depth (Prism3D may keep several; pick by
  size == swapchain + `DXGI_FORMAT_*_TYPELESS` depth format + DSV usage).
- Where the view-proj lives (cbuffer slot/offset fingerprint) and whether the
  game uses reversed-Z → sets `depthInverted`.
- Copy vs. alias for depth: depth is bound as DSV while we want an SRV — likely
  needs a `CopyResource` into a typeless twin per frame.
- Pre-UI injection point (device-context hook) so the HUD stays crisp.
