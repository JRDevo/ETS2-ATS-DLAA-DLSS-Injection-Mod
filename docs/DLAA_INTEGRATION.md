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
  than the camera still get camera motion if they win the medoid (v0.9.0: unless they carry an object id, see
  "Per-object motion vectors"; v0.10.0: every paired moving draw has its own R, see "Per-draw motion vectors"); pixels of one layer reproject with a
  single R, so anything not static in that layer (own wheels in the world layer) ghosts.

### Per-draw motion vectors (v0.10.0, `mv_objects = 2`, default)

v0.9.0 (`mv_objects = 1`, below) tagged "moving" draws with 15 stencil ids chosen by CPU heuristics (membership, coherence,
static class, ego set, fast join, attach rule). In game (r11 log / video) the road was tagged as a mover for most of a
drive, tags blinked, far trucks ghosted, plates stayed untagged and own-truck parts took ids: the heuristics were the
problem. v0.10.0 replaces the guess with a fact: every pixel learns WHICH G-buffer draw owns it, and every draw gets its
own exact R = MVP_prev · MVP_cur⁻¹. Code: `src/draw_ids.h/.cpp` (record, replay, GPU pairing), the "PER-DRAW MOTION
VECTORS" block in `src/inject.cpp`, pass B in `src/motion_vectors.cpp`. Mode 1 stays compiled for comparison; mode 2
never enables it (no stencil twins, no id table, no membership / static / ego / attach / fast-join logic).

- **Engine facts it relies on** (RenderDoc, `scripts/rdc_drawid_audit.py`, flat capture, answers in
  `captures/v010_handoff.md`): the G-buffer draws read only VS cb0 (the per-frame 2 MB ring, `VSSetConstantBuffers1`
  windows), VS SRVs (bone matrices at t3, instance data at t1) and their VBs / IB -- none of them is written inside the
  pass; no GS / HS / DS / stream-out; one depth-stencil state (GREATER, depth write on, stencil REPLACE 0x0F); two
  rasterizer states (cull back / none, scissor ON); one viewport per draw (world [0.01, 0.9], cabin [0.9, 1.0]); no
  G-buffer PS writes SV_Depth; the scene depth is a typed `D32_FLOAT_S8X24_UINT` texture with DSV binding only (a
  read-only DSV of it is legal); its DiscardView comes after the forward pass. 742 draws (643 DrawIndexed + 99
  DrawIndexedInstanced) in one G-buffer bind segment.
- **Record** (`DrawIdRecord`, one per pass FIFO slot): every world / cabin G-buffer `DrawIndexed` and (new hook, vtable
  20) `DrawIndexedInstanced` keeps its whole input state through ~11 getters (refs held until replayed): input layout,
  VB slots 0..3, IB, topology, VS, VS cb slots 0..3 with first / num constants, VS SRVs 0..7, VS samplers 0..1, RS
  state, viewports (the jittered ones), scissor rects, draw arguments; plus a full geometry key (IB, VB0, offsets,
  counts, base vertex, VS) and a pointer-free key (index count, start index, base vertex, VS), and the byte offset of
  its MVP (cb0 rows 4..7) in the pass's ring mirror (the v0.9.0 r3 ring logic: up to 3 rings, one
  `CopySubresourceRegion` per ring per replay, a WRITE_DISCARD seals a ring). Up to 4096 draws per pass.
- **Replay** at the G-buffer LEAVE (`hkOMSetRenderTargets`, before the game's next targets are bound, so the scene depth
  is final and intact): the pass slot's `R16_UINT` draw-id target (`DepthTwin::idTex`, cleared to 0 = "no draw" at the
  pass's first replay) + a read-only DSV of the scene depth (`READ_ONLY_DEPTH | READ_ONLY_STENCIL`), depth func
  **EQUAL**, no depth write, stencil off, blend off, the recorded VS / IA / cbuffers / RS / viewport / scissor, and our
  pixel shader `kDrawIdPs`: no inputs (an empty input signature links with every VS), it returns the draw id read from
  one 256-byte window of an immutable (4096 + 1) x 256 B cbuffer (`PSSetConstantBuffers1`, window k holds k; no
  per-draw upload). GS / HS / DS are bound null. Per draw only the state that differs from the previously replayed draw
  is set; everything touched is saved first and restored after. Same VS + same inputs + same viewport = the same
  SV_Position.z, so EQUAL passes exactly where that draw owns the pixel: an alpha-tested hole holds another draw's depth
  and fails, an occluded pixel fails, a decal / depth-write-off draw falls to the surface below it. Draw id = record
  index + 1.
  - **Reverse order:** the game tests GREATER, so of two draws at exactly the same depth the FIRST owns the pixel; with
    EQUAL the last replayed draw wins, so the replay runs backwards (WARP harness: 0 wrong pixels with deliberately
    overlapping coplanar parts; forward order: 6.2 M wrong).
  - First-segment `DrawIndexedInstanced` and no-MVP draws are not replayed (id 0 = camera motion; `mv_drawid_instanced =
    1` replays them). Once a pass has replayed (the G-buffer was left and re-entered, or a forced replay), every later
    draw is replayed, instanced too, so a later draw can never leave a stale id under itself.
  - Other replay points (render targets restored): the next pass's depth clear (`StartPass`, before the clear runs) and
    the scene depth discard, for a pass that ended without a G-buffer leave; and `hkMap` WRITE_DISCARD of a buffer a
    recorded-but-not-replayed draw reads (VB, IB, VS cbuffer or a VS SRV's resource; at most 2 per pass, counted as
    `forced`): the discard would hand the replay new contents.
- **Pairing** (`DrawIdMv`, one per eye in `CameraMv`, at the blit, after pass A): the CPU groups this pass's draws with
  the eye's previous pass by the full key; draws whose full key exists on one side only (the game's buffer-pool rotation
  changes the pointers on some frames -- the old "world-miss") regroup by the pointer-free key; groups over 1024 draws a
  side stay unpaired. Three small dispatches (`kDrawIdCs`): `CSGather` (each draw's MVP from the mirror into the eye's
  ping-pong buffer; the camera prediction P = R_layer · MVP_cur), `CSMatch` (each current draw -> the previous draw of its
  group whose origin is nearest to P's origin in previous clip space, tie-break on the whole-matrix distance, then on
  the draw-order rank; the same for each previous draw -> nearest current draw), `CSResolve`:
  - paired only when the choice is **mutual** and the origin moved less than `mv_drawid_max_m` (default 8, clip units ~
    m) against the camera prediction; R_draw = MVP_prev · MVP_cur⁻¹.
  - **static vs mover:** six probe points (origin, ±4 m on the local x / z axes, +4 m on y) through MVP_prev and P; an
    on-screen probe compares screen positions (px), an off-screen probe or one at / behind the camera plane compares
    clip x / y divided by max(w, 1) (a perspective divide by a tiny w would turn float noise into "motion" beside the
    camera; the harness saw 0.1-1.6 px there with ±1 m probes). Below `mv_drawid_snap_px` (default 0.1 px) the draw is
    STATIC and keeps the camera R (bit-identical to the camera path); otherwise it is a MOVER with its own R. The ±4 m
    lever covers a vehicle's half length (±1 m let an 8 m trailer's far end move 0.27 px while snapped); a larger lever
    only over-estimates, i.e. a static thing becomes a "mover" with its own exact R, which is harmless.
  - **track continuity** (ambiguous groups only: more than one draw of the key on a side, or a pointer-free-key group):
    a mover verdict additionally needs the previous draw to have been mutually paired itself in the frame before, with
    a consistent displacement (|d - d_prev| <= 0.25 + 25 % of |d_prev|, clip units). Without it a culled copy of an
    identical mesh (grass clump, window, wheel) "moved" to its neighbour's place the frame the neighbour reappeared
    (harness: 19..2163 px wrong motion vectors); such a pair now gets the camera R. Consequence: identical movers (cars
    of one model, a vehicle's wheels) get their own R from their second paired frame on.
  - unpaired, instanced and no-MVP draws: the camera R of their layer. Nothing here ever resets the DLSS history.
  - R table: `[i*5 + r]` = rows of R, `[i*5 + 4]` = (state 1 unpaired / 2 static / 3 mover / 4 instanced / 5 no MVP,
    viewport MinDepth, MaxDepth, probe deviation px).
- **Pass B** (`kReprojDepthShader`, `ObjInfo.z` = draws, `ObjInfo.w` = G-buffer draws): `id = DrawIds[px]`; a pixel of a
  MOVER draw is reprojected with that draw's R, z_ndc from the draw's own viewport depth range; every other pixel
  (static, unpaired, instanced, sky, no id) takes the unchanged camera path (cabin / world layer, ego). Phase 3: a pixel
  whose forward depth won (`mv_fwd_depth`; a plate, a wire, glass) uses its FORWARD draw id (the GREEN channel, see
  "Phase 3" below) instead of the G-buffer draw behind it; without a forward id: camera path. Coverage counters on a
  1-in-64 grid (world pixels, with an id, of a mover, of an unpaired draw; forward pixels, with a forward id, forward
  movers) are read back asynchronously for the log. Per-draw R subsumes the cabin layer and the old ego rule for every
  paired draw (own-truck parts pair exactly).
- **Debug view** (`Ctrl+F6`, mode 2; phase 3 colours -- the phase-1 "hue per draw" view was unreadable): **magenta** = a
  mover (its own R; a slight hue shift per draw so neighbouring moving parts differ; **pale** magenta = a mover whose own
  motion differs from the camera R's by <= 0.5 px at that pixel); static draws the **flat layer colour** (olive world,
  blue cabin = the camera R); **white** = a draw without a partner in the previous pass (camera R); **grey** = instanced /
  no-MVP (camera R); a pixel whose forward depth won and that has a forward id: its forward draw's state colour with a
  **yellow tint** (a plate on a moving truck: magenta-yellow, a static wire: olive-yellow); forward depth without a
  forward id: strong **yellow** as before. Pixels without an id (sky, unreplayed instanced vegetation, EQUAL misses) keep
  the plain motion-vector colours.
- **Logs** (every 600 blits, eye 0 for the GPU side): `MV draw-ids: ACTIVE ...` / `inactive` (on change), `MV draw-ids:
  read-only view of the scene depth created`, `MV draw-ids: first replay (pass s=..., G-buffer leave): N draws replayed`,
  and the stats line `MV draw-ids @blit N | ACTIVE, instanced hooked | record: D draws/pass (I instanced), F full/pass,
  CPU x ms/pass | replay: R draws/pass, S skipped (instanced / no MVP), multi-segment passes, end-of-pass replays, forced
  (capped), no target, CPU x ms/pass, GPU x ms avg / y max | pairing (eye 0, ...): paired P % of D draws/frame (movers,
  static, unpaired, instanced, no-MVP), via the pointer-free key ..., no partner group, oversized groups, unconfirmed
  tracks, frames < 50 % paired, CPU x ms/frame, GPU x ms | pass B GPU x ms | id coverage C % of world px (movers %,
  unpaired %)`. `MV draw-ids: WARNING paired p of e draws (< 50 %) in one frame` (first 10, then every 1000th) when a
  frame with history paired fewer than half its draws. GPU timers run whenever mode 2 is active (independent of
  `gpu_timing`).
- **Cost (expected, not measured in game).** CPU per G-buffer draw: ~11 getters + their refs at record (~1 µs?), ~5
  state calls + 1 draw at replay; per pass one sort of ~2 x the draw count at the blit. GPU per pass: the G-buffer's
  vertex work again with a one-instruction pixel shader behind an early EQUAL depth test, a buffer copy of the used ring
  range, three dispatches over the draws (~O(group size²) for duplicate meshes), one extra texture load per pixel in
  pass B. Memory: one `R16_UINT` scene-size target per pass slot (4 x 12 MB flat 2880x2160, 4 x 60 MB VR 4592x6496),
  ~5 MB of recorded state. All of it is in the `MV draw-ids @blit` line; target in VR ≤ ~1 ms per eye.
- **Limits.** Depth EQUAL needs the replayed VS to give bit-identical depth (the same VS object, inputs, RS and
  viewport; D3D11 does not formally promise it across different pixel shaders) -- the `id coverage` figure shows it in
  game, a miss only costs the pixel its own motion (camera R). Two coplanar draws in DIFFERENT G-buffer segments: the
  later one wins (the game keeps the earlier; one segment per pass in the capture). Instanced draws (vegetation, small
  props) keep the camera motion (their MVP is not in cb0). Vertex animation / skinning inside a draw is not in its
  MVP (the whole draw moves rigidly). Forward-pass draws get ids since phase 3 only when they are re-drawn into the
  forward depth ("over"-blended, depth test without write, world layer, not a near-camera overlay); other forward draws
  (additive glows, instanced forward draws) keep the camera path where the forward depth won. A G-buffer GS / HS / DS
  would be dropped by the replay (none in the capture; EQUAL then fails gracefully).
- **Synthetic WARP test** (`tests/drawid_harness.cpp`, `tests/build_drawid_harness.bat` -> `build/tests/drawid_harness.exe
  [scene] [snap px]`; 960 x 540; the game's G-buffer layout: 2 MB dynamic cb ring with D3D11.1 offsets, MVP rows 4..7,
  reversed-Z infinite projection, GREATER, stencil REPLACE, scissor on, viewport jitter, world / cabin layers, typed
  D32S8 depth with DSV binding only; the true owner of every pixel goes to an extra target). Drives `DrawIdRecord` /
  `CameraMv::Generate` exactly like inject.cpp. S1 street (120 frames: 30 identical clumps shuffled with 3 culled per
  frame, 10 boxes, vehicle with 4 identical spinning wheels and a plate on its body matrix, an identical vehicle the
  other way with an articulated trailer, alpha-tested foliage in front of the vehicle's lane, instanced vegetation, a
  skinned mover reading VS SRV bones, cabin with overlapping coplanar parts and a rotating wiper, moving + yawing
  camera), S2 = S1 + buffer-pool rotation (VB / IB pointers switch every 7th frame, the ring alternates), S3 = S1 + two
  G-buffer segments + a mid-pass ring WRITE_DISCARD (forced replay), S4 a crawling vehicle that stops. Checks every frame:
  exact draw-id ownership of every pixel, every pixel's MV against its object's true R (movers) / pass A's camera R
  (others), each draw's state against the true probe deviation, state flicker, history resets. Result: **46740 /
  46740** (also at snap 0.02 px = the game's 0.1 px at this harness's 1/5.8 focal); 0 wrong ids of 46.1 M owned pixels
  in S1 / S2 / S4, 1 pixel in S3 (a coplanar tie across the two segments, the limit above); state flicker 0; history
  resets 0; worst MV error 0.002..0.28 px (fp16 storage of large MVs). Mutations caught: continuity guard off (32
  checks, 5853 px), forward-order replay (6.2 M ids), replay without the VS SRVs (363 k ids + MVs). The v0.9.0 r11
  harness (mode 1 + camera path) still passes 23874 / 23874 against these sources.
- **dlaa.ini:** `mv_objects` 0 off / 1 v0.9.0 stencil ids (legacy) / 2 per-draw (default); `mv_drawid_instanced` (1 since phase 6b),
  `mv_drawid_max_m` (8, 0.5..100), `mv_drawid_snap_px` (0.1, 0..4), `mv_camr_consensus` (1, phase 3). The `mv_objects_*`
  keys are mode 1 only, except `mv_objects_hold` (also the camera path's world-miss hold).

#### Phase 3: consensus camera R + forward-pass draw ids (v0.10.0)

The first in-game test of phases 1 + 2 (ETS2 flat, 2026-10-08) showed per-draw pairing at 97-99.9 % and id coverage
84-98 %, but two gaps: (1) the WORLD CAMERA R still came from pass A's medoid (16 sampled candidate pairs, unique meshes
first), which picked a moving vehicle for seconds (`MV: R(moving) ... maxdev=1.27..2.00`, chosen pair |w| = 84 m): the
camera R was off by 1-2 px, so every pixel that uses it -- instanced grass, unpaired draws, the sky, the cabin layer,
forward wires -- smeared, and hundreds of static draws per frame were flagged "mover" (harmless for them, they keep their
own exact R); (2) licence plates, decals and glass are FORWARD-pass draws (no G-buffer id) and ghosted against the truck
body.

- **Consensus camera R** (`DrawIdMv`, three more dispatches of `kDrawIdCs`, GPU only, no new readback): `CSPair` stores
  each draw's mutual partner and R_draw = MVP_prev · MVP_cur⁻¹; `CSVote` runs one 64-thread group per CANDIDATE (up to 128
  world + 64 cabin, chosen on the CPU in `Prepare`: evenly spaced paired draws of the layer) and counts the paired draws
  of its layer whose six probe points agree with the candidate's R within `mv_drawid_snap_px` (min 0.05 px) --
  ProbeDev(MVP_prev_j, R_cand · MVP_cur_j), the same metric as the static test; `CSPick` (one thread per layer) takes the
  candidate with the most agreeing draws (tie: the smaller mean deviation). If that cluster has >= 24 draws, its R is
  written into CameraMv's solve buffer (`Solve[0..3]` world / `[4..7]` cabin, through a UAV; `Solve[8 + layer].w` = the
  cluster size) BEFORE the static / mover verdicts (`CSResolve`) and pass B; else the medoid stays (fallback). Voters are
  paired, non-instanced, MVP-carrying, non-forward draws of the layer; world voters must have their origin >=
  `mv_near_reject_m` (30 m) away (the own truck / trailer move with the camera, the medoid's own near filter) unless fewer
  than 24 such draws exist. Votes are per draw: static scenery is hundreds of draws, vehicles tens; a convoy moving
  together forms a cluster too, but a smaller one (harness S6). Everything that reads the camera R gets it from the solve
  buffer: the static test, pass B, the debug view, the next frame's match prediction, the world-miss hold / reuse path,
  and every mirror unit (the same CameraMv + DrawIdMv code). Track displacements (continuity guard) are now measured
  against the final camera R. `mv_camr_consensus = 0` keeps the medoid (A/B).
- **Forward-pass draw ids:** every forward draw that `FwdOnDraw` re-draws into the forward depth (r5: "over"-blended, depth
  test without depth write, world layer, not near-camera) is also recorded (`DrawIdRecord` in FORWARD mode,
  `g_didFwdRing[slot]`: same full input state, its own ring mirror, keys of their own so forward draws pair only with
  forward draws). While forward ids are possible the pass slot's draw-id target is `R16G16_UINT`: RED = the G-buffer id
  (the G-buffer replay writes RED only, blend write mask), GREEN = the forward id. At the scene depth discard (fallbacks:
  the next pass start; forced before a WRITE_DISCARD of a buffer the pending forward draws read, max 2 per pass)
  `ReplayForward` draws them again into GREEN with the pass's forward-depth target as an EQUAL depth, in DRAW order (the
  forward depth is written with GREATER_EQUAL: the last draw of a tie owns it). Ids continue after the G-buffer draws
  (nG + 1 ..) in ONE table: `DrawIdMv::Prepare` appends the forward draws, `CSGather` reads their MVPs from the forward
  record's mirror (t7). Pass B uses the GREEN id exactly where the forward depth won (fw > dt, world range) -- the plate
  gets the body's motion because the plate draw carries the body's matrix. Alpha: the forward depth exists only where
  the game's alpha-to-coverage kept the pixel; the replay has no alpha but EQUAL only passes where the draw's own depth IS
  the stored forward depth (two exactly coplanar forward draws: the later wins). WARP showed that single-sample
  alpha-to-coverage DITHERS (alpha 0.3 covers some pixels) -- the ids follow the forward depth either way.
  `mv_fwd_depth = 0` / Ctrl+F3 off disables both the forward depth and the forward ids. Mirror views have no forward
  depth and therefore no forward ids.
- **Logs:** `per-draw motion vectors phase 3 (v0.10.0): mv_camr_consensus=1 ...` (config), one-time `MV camera R:
  consensus -- ...`, `MV draw-ids: first FORWARD replay (...)`, `MV forward draw n/20 (pass s=...): IndexCount ...,
  viewport depth ..., depth ... write ... func ..., blend ... src .. dest .. mask .., VS cb0 ... -> RECORDED / skipped:
  ... / instanced` (the first 20 forward draws of one pass: which of them can get an id), the `MV: R ...` line prints
  `camR=consensus (N draws)` / `camR=medoid` (its pairs / pick / |w| fields always describe the medoid), and the `MV
  draw-ids @blit` line gains `camR (eye 0): consensus N / medoid fallback M frames (V with near voters: fewer than 24
  far draws, the own truck may vote), cluster avg C draws, |consensus -
  medoid| avg A / max B px (K compared, F frames > 1 px); cabin ... | forward ids (on): recorded x/pass, replayed y/pass,
  id overflow, forced, no target, CPU, GPU | forward pairing (eye 0): draws/frame in the table, paired, movers, records
  dropped | forward px (forward depth won) with a forward id P % (movers %), instanced forward draws`. `MV camera R:
  WARNING the medoid camera R (pass A) was off by > 1 px in F of K compared frames` when F > 30 in a window (the proof
  that the consensus mattered). Mirror unit lines: `camR consensus N / medoid fallback M frames`.
- **Cost:** CSVote = (candidates) x (paired draws) probe tests (~192 x 700 x 6 mat-vec), CSPair / CSPick tiny; the forward
  replay = the forward draws' vertex work once more (~80-110 draws per pass in the capture); one more 2 bytes per pixel
  per pass slot (`R16G16_UINT`: 4 x 24 MB flat 2880x2160, 4 x 120 MB VR 4592x6496).
- **WARP harness S6** (`drawid_harness.exe 6`): 12 unique cars and a convoy of 30 identical trucks all moving with ONE
  velocity (their R_draw identical) vs ground, 3 boxes, 50 identical posts, 30 identical clumps; a vehicle with a FORWARD
  plate (alpha band) and a forward glass pane (alpha 0.3) on its matrix, a static forward wire gantry. Checks: the solve
  buffer's camera R against the exact camera R on a pixel grid at view depths >= 5 m (<= 0.05 px; measured 0.0006 px),
  the medoid really fooled (|consensus - medoid| avg 1.07 px, > 1 px in 62 of 98 frames), exact ids in both channels
  (GREEN against a truth pass with the same alpha-to-coverage), every pixel's MV against TRUTH (a true mover its exact R,
  everything else the exact camera R), the plate's pixels move exactly with the body. 15895 / 15895; full suite S1-S6
  71912 / 71912 at snap 0.1 and 0.02; S6 under the D3D11 debug layer 15995 / 15995 with 0 messages. Mutations:
  `drawid_harness.exe 6 0.1 nocons` (consensus off: camera R off by up to 279 px at 5 m, instanced / static pixels
  wrong) and `... nofwd` (forward ids off: 0 of ~700 plate pixels right per frame) both FAIL.

#### Phase 4: plates take their vehicle's motion (attach) + DLAA before the tonemap (v0.10.0)

In-game evidence (ETS2 flat, 2026-10-08, `captures/dlaa_inject_ets2_v0100p3_flat.log`, `captures/vid4_truck_1m33.png`): in
Ctrl+F6 a bus right ahead was full magenta, its licence plate PALE magenta + yellow = a forward id, state MOVER, but its own
R moved the plate <= 0.5 px away from the camera R while the body moved many px; on screen the plate stayed jagged (DLSS
history rejected). RenderDoc (`scripts/rdc_fwdcolour_audit.py`, `captures/ets2_flat_frame1969.fwdcolour_audit.txt`): most
small forward draws carry a normal per-object MVP (vehicle decals with the vehicle's matrix, vertices up to 2.8 m from the
pivot), but the game also has forward draws with a camera-only matrix and view-space vertices in a DYNAMIC vertex buffer
(these are the r11 "near-camera" draws: never re-drawn, no id). A plate whose own pair carries no vehicle motion is
consistent only with that kind of draw (or a pair whose matrix did not change); the dump key below settles which.

- **Attach per draw** (`DrawIdMv` `CSAttach`, after `CSResolve`, `mv_fwd_attach_m`, default 0.5 m, 0 = off): a forward
  draw whose MVP origin lies within the radius of a paired G-buffer MOVER's origin (same layer; view-space distance from
  the two clip origins, x / y divided by the mover's |row 0| / |row 3| and |row 1| / |row 3| = P00 / P11) takes that
  mover's R (state 3; the R table's `.w` = -(1 + mover index) marks it) unless the forward draw is a mover itself whose R
  agrees within 0.5 px at its probes (kept). Covers decals with the vehicle's matrix that pair badly (12 identical plates =
  an ambiguous group: the continuity guard answers their first paired frame with the camera R; unpaired frames).
- **Attach per pixel** (pass B, same radius): where the forward depth won and the G-buffer draw under the pixel (RED id =
  the surface the decal sits on) is a mover and the forward surface lies within the radius in front of it (view distance
  through InvRow3; ~2 % of the distance when InvRow3 is unknown), the pixel takes that mover's R (z from the merged depth)
  unless the forward draw is a mover whose own R agrees within 0.5 px there. Covers camera-matrix plates (the origin is
  meaningless) and forward pixels without an id. Counted on the 1-in-64 grid (dword 176 + stripe).
- **Ctrl+F6:** an attached pixel / draw = the mover's state colour with an ORANGE tint (a plate on a moving bus = strong
  magenta-orange). Recognised in the debug shader from pass B's result (the pixel's MV equals the G-buffer mover's
  reprojection there; the scene depth under a forward pixel is not available to that shader).
- **Dump** (`key_mv_dump`, default `Alt+F8`; `debug = 1`): `DrawIdMv::RequestDump` -> the next prepared pass of the main
  scene logs one line per forward draw and per G-buffer draw with IndexCount <= 36: `MV dump n/N: id I GBUF|FWD ic IC flags
  0x.. [cabin] [loose-key] | full ... loose ... | cb0 first F num N -> mirror +O | group G (C cur / P prev) | origin clip x y
  z w px X Y | partner J (prev), origin displacement vs the camera prediction dx dy w dw [(no track)] | R_draw - camR at the
  origin D px | probe dev P px [(snapped to the camera R)] | state S <name> [| ATTACHED to G-buffer mover id M]`, after a
  header line with the table size and the camera R rows. Staging copies of CurM / the R table / Work (partner, R_draw,
  attach) / the camera R at Finish, read with DO_NOT_WAIT in a later Prepare (never blocks).
- **Risk:** a static forward draw (a wire, a sign) that passes within the radius in front of a moving G-buffer surface takes
  the mover's motion on those pixels (the rule cannot tell "on" from "just in front of"); 0.5 m keeps that to contact
  distances. `mv_fwd_attach_m = 0` turns both attaches off.
- **WARP harness S7** (`drawid_harness.exe 7`): 12 vehicles with unique bodies sharing ONE plate mesh (vertices in the body's
  frame, 2 cm in front of the rear face), 4 vehicles with a shared BRACKET plate 0.9 m behind the body (the pixel under it is
  the road: only the per-draw attach can help; two are culled every 13th frame), one vehicle whose plate is drawn with the
  CAMERA VP and world-space vertices rewritten into a DYNAMIC vertex buffer every frame (own R = camera R: only the per-pixel
  attach can help), a static decal on a static board. Every pixel's MV against its owner -- a plate against its BODY's
  expected motion -- from frame 1. 2576 / 2576 (incl. one check that the dump path ran and landed); all plate pixels right incl. the first paired / reappearance frames.
  Mutation `drawid_harness.exe 7 0.1 noattach` FAILS (camera-VP plate 0 of 91835 px right, shared plates 0 of 655 px on
  their first paired frame, bracket plates 0 of 3083 px on first paired / reappearance frames). S6's `nofwd` mutation now
  runs with the attach off (with it on, the per-pixel attach alone gives S6's plate the body's motion).

**DLAA before the tonemap** (`dlaa_pre_tonemap`, default 1; `key_pre_tonemap` Alt+F7, Shift+F12 saves it): in flat DLAA
mode the main scene's DLAA runs IN PLACE on the RGBA16F forward colour at the scene depth discard (`PreTonemapAtDiscard` in
inject.cpp, from `OnDepthDiscard` after the draw-id replays and the depth snapshot), like the mirror units -- an HDR unit
(NGX IsHDR unless `dlss_hdr = 0`), so the game's bloom down-sample, tonemap and UI see the anti-aliased picture.
- RenderDoc: the forward colour is one RGBA16F scene-size texture; the scene depth DiscardView comes right after the forward
  pass WITH the forward RTV still bound; afterwards only a 720x540 bloom down-sample and the tonemap READ it (nothing copies,
  clears or discards it). So no snapshot of the colour is needed; the colour is taken from the RTV bound at the discard and
  must be the forward jitter pass's RT (`g_fwdColorTex`), RGBA16F, scene size, 1 mip, 1 sample.
- Same jitter (the pass slot's), same depth snapshot, same candidates / draw records as the blit path; the blit that
  consumes the slot skips its own Run (`PassSlot::pre`). Exposure: NGX AutoExposure (the game computes its exposure later,
  after the bloom down-sample). Sharpen: RCAS inside Run on the HDR result (v0.8.0 reversible mapping), i.e. before the
  game's tonemap.
- Falls back to the blit path (logged once per reason: `DLAA stage: post-tonemap (blit) -- <why>`): `dlaa_pre_tonemap = 0` /
  Alt+F7 off, VR, flat not confirmed yet, HDR output, DLSS upscaling (the last blit upscaled), DLAA off / jitter-only /
  passive, the Ctrl+F6 view on (exact debug colours after the tonemap), shader warm-up, no blit yet, the forward colour not
  bound / not of the expected shape, no depth snapshot. A deferred HDR unit build leaves that frame un-anti-aliased at the
  blit (no LDR rebuild = no flip-flop); a unit that cannot be built or fails 60 times in a row latches the blit path until
  Alt+F7 is pressed twice. Switching between the two paths rebuilds eye 0's unit (LDR <-> HDR: NGX feature recreate +
  history reset). (Phase 7 replaced the per-frame choice, the frame left alone and the rebuild: see "Phase 7" below.)
- Logs: config `phase 4 (v0.10.0): dlaa_pre_tonemap=1 (Alt+F7 live switch) mv_fwd_attach_m=0.50 ...`; one-time `DLAA stage:
  pre-tonemap (HDR, in place on the RGBA16F scene colour WxH at the scene depth discard; NGX IsHDR=1, AutoExposure, RCAS
  in HDR) ...`, `first DLAA evaluate OK BEFORE THE TONEMAP: eye=0 ...`; the FIFO stats tag `stage=pre-tonemap (pre-tonemap
  runs N, deferred D, failed F, blit skips S; post-tonemap blit runs B)`.

**Phase 7 -- the stage never flips per frame, and a stage change costs no rebuild.** In game (ETS2 flat, log
`captures/dlaa_inject_ets2_v0100p6c_flat.log`) the phase-4 stage flipped 18 times to post-tonemap ("the forward colour is not
bound at the scene depth discard") and 19 times back in 7 minutes, some windows alternating per frame (`pre-tonemap runs 345
... post-tonemap blit runs 255`), with 71 `NGX feature created for eye 0` lines: every flip rebuilt eye 0's unit (a
whole-screen blur / shimmer pulse). The RenderDoc frame (`scripts/rdc_fwdstage_audit.py`, output
`captures/ets2_flat_frame1969.fwdstage_audit.txt`) has exactly three bindings on the scene depth (G-buffer 742 draws, lighting
9, forward 190) and the discard with the forward binding still current -- the failing frames are not in it; the extra
`left its forward binding BEFORE the scene depth discard` log lines (below) name the binding that comes first in game.
- ONE DECISION PER PASS (`PreTonemapPoint`), at the FIRST of (a) the scene depth discard (as phase 4) and (b) the FORWARD
  LEAVE (`FwdOnLeave`, hkOMSetRenderTargets before the game's next bind goes through while the main forward binding is the
  current one): the forward colour is final there and nothing has read it yet, so a pass whose game binds other targets
  before the discard still runs before the tonemap. At the leave the pending draw-id replays and the depth snapshot run
  first (the depth is intact). The forward colour is the pass's own (`PassSlot::fwdTex`, remembered at its forward bind by
  `FwdOnBind`), not a global. A main forward bind of a pass after its decision at the leave is a RE-ENTRY (counted, WARNING
  lines: those draws land on top of the anti-aliased picture).
- HYSTERESIS (`src/dlaa_stage.h`, `DlaaStage`): mode reasons (dlaa_pre_tonemap 0 / Alt+F7 off, DLAA off, jitter-only /
  passive, Ctrl+F6, DLSS upscaling, VR, HDR output, ...) switch to post-tonemap at once. Per-pass conditions (no forward
  colour, no depth snapshot, the HDR run deferred / failed) change the PREFERRED stage only after 60 passes in a row; a single
  such pass runs the blit unit as a FALLBACK (the HDR unit stays). post -> pre needs 60 passes in a row that could run before
  the tonemap; the last 8 of them WARM the HDR unit at the blit (`SceneDlaa::Warm`: the same pass's motion vectors, depth and
  jitter, its forward colour as input, nothing written back), so it takes over with a continuous history. Not warm after 60
  (its build waits for an NGX create budget): the switch waits up to 60 more passes, then goes ahead with a reset.
- BOTH UNITS STAY ALIVE: `SceneDlaa` keeps two colour sets (colorIn / out / sharp + own NGX feature + own history; depth, MV
  textures and the CameraMv state are shared, they belong to the pass). A change of only the colour kind swaps the parked set
  in (`Ensure` -> `SwapParked`) or builds the missing one next to the parked one (`BuildColourSet`); only a size / area /
  upscale change rebuilds (both sets go). `KindIdleRuns(kind)`: evaluations of the unit since that set last evaluated; a set
  that takes over after more than 2 (`kResumeKeepRuns`) drops its stale history once (the blit unit on the first fallback pass
  after a run of pre-tonemap passes, the HDR unit after a fallback run shorter than 60, either after a mode switch). Automatic
  switches cost no reset (the unit that takes over ran / was warmed on the passes before).
- Logs: config `phase 7 (v0.10.0): pre-tonemap stage = ONE decision per pass ...`; `DLAA stage:` lines only when the preferred
  stage or its mode reason changes (`... (stage switch #N, Present #...)`, `the pre-tonemap path was not possible for 60 passes
  in a row (last: <why>) -- the blit unit ran in all of them: no history reset, the HDR unit stays alive`, `pre-tonemap
  possible: switching after 60 passes in a row (the HDR unit warms up during the last 8)`, `switched after the hysteresis;
  the HDR unit was warmed on the last passes: no history reset`); `DLAA: eye 0 now runs its HDR (RGBA16F) / LDR (RGBA8) colour
  set (swapped in with its own history, NGX feature kept ...) -- swap #N, no rebuild` (first 8); `NGX feature created for eye 0
  ... (second colour set: the LDR set stays alive)`; `DLAA pre-tonemap: pass s=... left its forward binding BEFORE the scene
  depth discard (leave #N) -- next binding: n RTV(s), RTV0 fmt F WxH, DSV the scene depth / another depth / none` (first 8);
  `depth snapshot: captured ... at its FORWARD LEAVE` (once); FIFO stats tag `stage=... (pre-tonemap runs N (at the forward
  leave L), deferred, failed; post-tonemap blit runs B (fallbacks while pre-tonemap is preferred F, passes without a decision
  point M); stage switches S (auto A), HDR warm-ups W, resume resets R, colour sets: swaps X builds Y; forward leaves before the
  discard Z, re-entries E, two forward colours T)`, `snapshots: at discard=... at forward leave=...`; `DLAA stage: WARNING N
  automatic stage switches in this stats window (> 2 ...)`.
- Harness S9 (`tests/drawid_harness.cpp` RunStageScene, 22 checks): the DlaaStage hysteresis (start-up, per-pass alternation,
  59-pass runs, failing HDR runs, mode switches, forced switch, 30 % random failures), the REAL `SceneDlaa` with two colour sets
  on WARP (`tests/fake_dlaa.cpp` stands in for NGX: counted features, a per-feature marker in the output) -- no rebuild across
  40 alternating passes, warm = no write-back, take-over without a reset, size change rebuilds both, deferred / failed builds
  leave the other set running -- and a model of the per-pass flow driven with the in-game pass pattern (4319 passes: 2 NGX
  features in total, 6 automatic switches >= 72 passes apart, 0 resets at the switches). The inject.cpp hooks themselves
  (forward leave, discard order) are game-only.
- Not covered by the harness (inject.cpp plumbing; the HDR unit itself is the mirror units' path, in game since phase 2).

#### Phase 5: unpaired draws take the motion of the draw that shares their matrix (v0.10.0)

In-game evidence (ETS2 flat, 2026-10-08, `captures/dlaa_inject_ets2_v0100p4_run2.log`, 33 Alt+F8 dumps while driving): the
phase-4 attach never fired (`took its R 0.00/frame` in every window) and in Ctrl+F6 the plates on moving trucks were WHITE =
recorded draws without a partner in the previous pass, every frame -> camera R -> DLSS history rejected -> jagged plate. The
stats showed `unpaired 2-19 draws/frame` and `no partner group 0.5-12.8/frame` (no draw of the same full OR pointer-free key in
the previous pass: the plate's vertex buffer / ring offsets are new every frame); the dumps never listed them (G-buffer draws
with IndexCount <= 36 only).

- **Matrix twins** (`DrawIdMv` `CSInherit1` + `CSTwin`, after `CSResolve`; dlaa.ini `mv_drawid_twin`, default 1): every draw
  paired by its OWN pair (static or mover) inserts itself into a GPU hash table of its 16 MVP words (2n slots in Work,
  linear probing with InterlockedCompareExchange, cleared by `CSGather`); every draw still at state 1 (no partner group,
  oversized group, continuity reject, no mutual partner; not instanced / no-MVP) looks its MVP up: an entry whose 16 words
  are bit-identical (-0 == +0; same layer; G-buffer twins preferred over forward ones, then the lowest index) is its twin,
  and the draw takes the twin's R rows and state -- a plate / lamp / mirror arm drawn with the vehicle's own matrix moves
  exactly with the body; a sign face on its post's static matrix stays static (camera R). The R table's `.w` = -(1 + 4096 +
  twin) marks it. Sources are only draws paired by their own pair (no chains; `CSTwin` modifies state-1 draws only), so
  the result does not depend on thread order. Exact bit equality, not a tolerance: the game computes a part's MVP from the
  same world matrix in the same code path; a near-identical MVP of a MOVER still lands in the origin fallback below.
- **Origin fallback for G-buffer draws** (`CSAttach`, extended; `mv_fwd_attach_m`): a G-buffer draw still unpaired without a
  twin takes the nearest G-buffer MOVER's R when the origins lie within the radius (the phase-4 view-space metric) AND the
  MVP columns 0..2 (projected rotation / scale) agree within 1e-3 -- the orientation test is new for G-buffer draws: a
  static draw that reappears at a vehicle's pivot (harness: a decoy on the vehicle's path) must not take its motion; a part
  on its own node with the body's orientation passes. Forward draws keep the phase-4 rule; a draw that got a twin is never
  attached by origin. Candidates are a compact list of the G-buffer movers built by `CSInherit1` (no full scan per draw).
  Limit: origins at / behind the camera plane (w <= 0.05) cannot use the fallback -- a long truck alongside the camera has
  its pivot behind the camera: only the twin helps there.
- **Ctrl+F6:** a draw that took its twin's R = the twin's state colour with a **cyan** tint (a plate on a moving truck:
  magenta-cyan; on a parked one: olive-cyan); a G-buffer draw that took a mover's R by origin = the mover's colour with an
  **orange** tint (like the phase-4 forward attach). White = still unpaired (no twin, no mover nearby).
- **Stats** (`MV draw-ids @blit`, new tail): `| twin: inherited N/frame (movers M, forward F) of U unpaired/frame
  (mv_drawid_twin 1); G-buffer origin attach K/frame (mv_fwd_attach_m 0.50, same orientation); left unpaired L/frame`
  (Cnt dwords 48 twins, 49 twins of movers, 50 G-buffer origin attaches, 52 state-1 draws looked up, 53 forward twins).
- **Dump** (Alt+F8): the per-draw dump now keeps the CPU facts of EVERY draw of the dumped pass and, once the GPU states are
  read back, logs every forward draw, every G-buffer draw with IndexCount <= 36 (as before) and EVERY draw that came out of
  the pairing unpaired (any IndexCount) or inherited something, with ` | unpaired: <why> | twin -> id X GBUF|FWD (<state>,
  identical MVP)` / ` | no twin` (+ ` | ATTACHED to G-buffer mover id M`) / ` | no twin, no G-buffer mover within 0.50 m with
  its orientation`; why = `no partner group (full + loose key new this frame)`, `oversized group`, `no mutual partner /
  displacement over mv_drawid_max_m`, `track rejected (continuity guard: identical copies)`. The header line counts them:
  `unpaired after the pairing U: T took their matrix twin's R, A a G-buffer mover's by origin, L left unpaired`. Alt+F8 also
  logs ONE pass of forward draws that get no forward depth / id (inject.cpp `DidFwdSkipDraw`): `MV dump fwd-skip n (pass
  s=...): IndexCount ic, viewport depth a..b, depth on|off write ALL|none func F, stencil on|off, blend on|off src S dest D op
  O | alpha src dest op, A2C 0|1, write mask 0x.., VS cb0 bytes at constant (+num) -> <verdict>` (cap 512) and at the next
  pass start one `MV dump fwd-skip combo k/K: N draws, IndexCount a..b, <state> -> <verdict> (first: line L)` per distinct
  state combination (the in-game "other states" 17-158 draws per pass).
- **Cost:** two more dispatches per eye of n threads; `CSTwin` walks one probe chain per unpaired draw (load factor <= 0.5),
  `CSAttach` scans the mover list (tens..hundreds) instead of all G-buffer draws. Work grows by kMax x 16 B + 8 KB.
- **WARP harness S7** (phase 5 additions; `drawid_harness.exe 7`): draws whose KEYS CHANGE EVERY FRAME (3 vertex buffers in
  rotation, base vertex and startIndex shifted every frame: neither key ever repeats, as the in-game plates) -- a G-buffer
  and a forward plate on the H1 body's matrix, a G-buffer lamp on its own node 0.37 m from that body's pivot (origin
  fallback), a static sign face on its post's matrix (must be state 2 = its static twin), a G-buffer and a forward side plate
  on a 12 m truck overtaking alongside the camera whose pivot is BEHIND the camera plane in every frame (only the twin can
  help), a static decoy that the H1 body's moving pivot passes within 0.5 m (with another orientation: must never take its
  motion) -- plus 12 identical G-buffer plates on the 12 shared-plate vehicles' matrices (2 of them culled every 11th frame:
  ambiguous group, continuity guard). Every plate pixel is checked against its BODY's expected motion; a check per case that
  the camera R alone would be wrong there (the scene exercises it). 3639 / 3639. Mutations: `notwin` FAILS (the overtaking
  truck's G-buffer side plate 0 px right in every frame), `noinherit` (twins + attach off) FAILS, `noattach` FAILS (lamp +
  camera-VP plate); a scratch build without the orientation test FAILS (the decoy takes the body's motion in 2 frames).
  The scene change moved phase 4's static board out of the bracket-vehicle lane: a bracket vehicle drove THROUGH the board
  (its surface 1.4 cm behind the decal); with the new objects (another draw order, other pass-A samples) the decal's pixels
  in front of that moving surface got a wrong motion in one frame -- the phase-4 per-pixel attach risk ("on" vs "just in
  front of" a mover) in a geometry that cannot happen in game; the old scene still passes with the new code.

#### Phase 6: unpaired draws take the motion of the draw they sit on (rigid parents, v0.10.0)

In-game evidence (ETS2 flat, 2026-10-08 12:42, `captures/dlaa_inject_ets2_v0100p5_flat.log`, two Alt+F8 dumps next to a Krone
trailer whose plate stayed WHITE in Ctrl+F6, `captures/vid7_plate_5m28.png`): the twins work for the own truck's parts
(`twin: inherited 11-21/frame`), but the plates are the draws left unpaired (`id 143 GBUF ic 132`, `id 123 GBUF ic 102`, plus
forward draws ic 24 / 6 / 18): `no partner group (full + loose key new this frame) | no twin, no G-buffer mover within 0.50 m`,
and every one of them prints `origin clip 0.000 0.000 0.200 w 0.000` -- column 3 of their cb0 rows 4..7 is EXACTLY (0, 0, 0.2,
0), the column 3 of the game's reversed-Z infinite projection (near 0.2 m). Their rows 4..7 are the bare PROJECTION: the CPU
writes their vertices in VIEW space every frame (a dynamic vertex buffer: the keys are new every frame). No matrix can name
their vehicle, so neither the twin nor the origin test can work.

- **The maths (convention verified in the code):** `LoadM` loads rows 4..7 as matrix rows and every use is `mul(M, p)` with p a
  column (clip = M * p); `R_draw = mul(Mp, inverse(Mc))` maps current clip to previous clip. A node rigidly fixed on a parent
  body P (MVP_U = MVP_P * O, O constant; or view-space vertices V * W_P * O * p under the bare projection) has MVP_U_prev =
  MVP_P_prev * O = R_P * MVP_U_cur, so **R_U = R_P exactly** -- for any offset from the pivot, turning or not. U's own matrix is
  not needed. (The row-vector form `MVP_U_prev = MVP_U_cur * inv(MVP_P_cur) * MVP_P_prev` gives the same map in a row-vector
  code; typed into this column-vector code it is a wrong conjugate -- the harness mutation `parentconj` shows it.)
- **Finding the parent = the draw U sits on** (`DrawIdMv`, GPU, same frame, no readback; dlaa.ini `mv_drawid_parent`, default
  1): `CSParentList` (one group, after `CSResolve`) gives every draw that is at state 1 WITHOUT a mutual partner (no partner
  group, oversized group, displacement over the limit; not the continuity rejects of identical copies, whose camera R for one
  frame is deliberate) a slot (<= 64 per pass, draw order). `CSParentVote` (after `CSTwin` / `CSAttach`) runs one thread per
  2nd pixel in x and y (a quarter of the pixels) over the full-size draw-id target: a pixel whose RED id is a U (scene depth)
  or -- where the forward depth won, pass B's rule -- whose GREEN id is a U (forward depth) samples the RED (G-buffer) ids at
  +-2 and +-5 px in the 4 directions; a sample votes when its draw is in the same layer and its linear depth (z_ndc of its own
  viewport range, ~ near / view distance) is within 2 % of the pixel's, and it is either a G-buffer draw paired by its OWN
  pair (static or mover, R table `.w >= 0`) or another U (a chain vote). Per U a table of 8 candidates in Work
  (InterlockedCompareExchange insert, InterlockedAdd counts) + the real / chain totals; a U whose total reached 65535 stops
  voting. `CSParentPick1`: the real candidate with >= 32 votes and >= 60 % of the U's real votes is its parent -- a mover gives
  U its R rows and state 3, a static parent makes U static (state 2, camera R); R table `.w = -(1 + 2 * 4096 + P)`.
  `CSParentPick2`: a U without own evidence (< 32 real votes) whose chain candidate was resolved by pass 1 (>= 32 chain votes,
  >= 60 %) takes that U's ROOT parent -- plate text drawn inside a plate background that is itself unpaired. The vote decides
  over a phase-5 twin / origin attach of the same draw; no winner keeps the phase-5 chain (twin / origin attach / camera R).
- **Twin guard:** `CSTwin` no longer looks up a draw whose MVP column 3 is exactly (0, 0, c, 0): every view-space draw carries
  the same projection, a "twin" would be arbitrary.
- **Ctrl+F6:** a draw that took its rigid parent's motion = the parent's state colour with a **green** tint (a plate on a
  moving trailer: magenta-green; on a parked one: olive-green); it wins over the cyan / orange tint of the same draw.
- **Stats** (`MV draw-ids @blit`, new tail): `| parent: inherited N/frame (movers M) of U unpaired, no parent K/frame
  (mv_drawid_parent 1; forward F, via an unpaired neighbour C; no votes A, no majority B, over the 64 cap O; avg V votes per
  listed draw); overrode a twin / origin attach R/frame (S with another state); left at the camera R L/frame (+ over the cap);
  twin lookups skipped (MVP = projection only) T/frame` (Cnt dwords 54..63, 192..194). Mirror units: `(... twin t, origin o,
  parent p)`.
- **Dump** (Alt+F8): the unpaired lines end with ` | parent -> id P GBUF (votes V/T, rigid)` (`static` for a parked parent;
  `, via an unpaired neighbour` for a chain) or ` | no parent (best V of T votes)`; the origin of a view-space draw prints
  `(view space: MVP = projection only)`; the header counts `R took their rigid parent's R by the pixel-neighbour vote`.
- **Cost:** the vote is (W/2) x (H/2) threads (flat 2880x2160: 1.56 M; VR 4592x6496: 7.5 M per eye) that read one uint2 id and,
  for a non-zero id, one Work dword; only the pixels of the <= 64 unpaired draws read depth and 8 neighbours. Expected ~0.02-
  0.05 ms per flat eye and ~0.1 ms per VR eye on an RTX 5090 (not measured; it is part of `pairing ... GPU` in the stats
  line); list and picks are one group each. Work grows by kMax x 12 B + 8 KB.
- **Limit:** a STATIC unpaired draw that touches a MOVING draw at the same depth (within 2 %), with the moving draw the
  majority of its depth-consistent neighbours, takes the moving draw's motion for those frames (harness: phase 5's decoy stood
  INSIDE the moving body's volume and took its motion -- moved 3.2 m up, its origin test unchanged). Plates smaller than ~32
  votes (in game roughly beyond 80-100 m) keep the phase-5 chain / camera R.
- **WARP harness S8** (`drawid_harness.exe 8`): a TURNING trailer (yaw up to 0.009 rad / frame) with a G-buffer plate
  background and the plate's text inside it, both written in VIEW space every frame (rows 4..7 = the projection, keys new every
  frame, exactly the in-game plates; the text's samples see only the background = a chain), and a FORWARD plate on its own node
  matrix 6.5 m behind the pivot (no twin, no origin); a parked trailer with a view-space plate (must be static by its parent);
  a static view-space gantry sign placed on the ray through a passing truck's rear top edge at 1.5x its distance (the truck's
  pixels come within 0..5 px of the sign in 23 frames: the depth test must reject them). Every plate pixel against its body's
  exact motion, the sign against the camera R, the parent per draw and frame. 2205 / 2205; mutations `noparent`,
  `parentconj` and `nodepth` FAIL. S1-S7 accept a rigid-parent inheritance where "unpaired" was expected when its state
  matches the object's true motion (the pixel check then verifies the inherited R); full suite 77804 / 77804.

#### Phase 6b: instanced plates join the rigid-parent vote (v0.10.0, `mv_drawid_instanced = 1`, now default)

- **Finding (in game, ETS2 flat):** a close car's licence plate shows GREY in the Ctrl+F6 view = a pixel without a draw id =
  plain camera motion, and it ghosts against the car. It is a `DrawIndexedInstanced` draw: its cb0 rows 4..7 are not the
  instances' MVP (flag `kFInst`), so it has no pair, no twin and no origin, and until phase 6b it was not even replayed
  (`mv_drawid_instanced = 0`), so it had no id at all. The phase-6 vote also excluded `kFInst` / `kFNoMvp` draws.
- **Change:** `mv_drawid_instanced` now defaults to **1** -- every instanced / no-MVP draw is replayed into the draw-id target
  (it already carries `kFInst`, ~180 extra tiny replay draws per pass) -- and such draws now take part in the rigid-parent vote
  exactly like an unpaired draw: their pixels vote for the neighbouring paired G-buffer draw they sit on (same depth gate), and
  `CSParentPick1/2` give them that parent's R and state (mover -> state 3 with the parent's R, the plate moves with the car;
  static -> state 2, camera R). They are **never** a parent source (`ParVoteFrom` / `ParentOk` skip `kFInst` / `kFNoMvp`), since
  they have no own pair. No winning parent = the camera R, as before.
- **Which instanced draws get a vote slot:** the 64 U slots go to the genuine unpaired draws first (phases 5/6, unchanged), then
  to the instanced / no-MVP draws **with the most on-screen pixels** (a new `CSParentCount` pass counts each replayed instanced
  draw's RED-id pixels into `kParPix`; `CSParentList` ranks them by that count, tie = lower index). In game ~180 instanced draws
  exceed the 64 cap, so the pixel count keeps the close plate (many pixels) in and drops the far / zero-pixel vegetation.
- **Pass B / Ctrl+F6 / dump are unchanged** -- they are driven by the draw's RTab state and `.w` marker, which `ParApply` already
  sets: an instanced plate with a mover parent is treated as any parent-inherited mover (green tint, `parent -> id X`).
- **Stats** (`MV draw-ids @blit`, `parent:` field): `instanced/no-MVP with a parent N/frame (of M listed, V with pixels;
  mv_drawid_instanced 1)` (Cnt dwords 195 / 196 / 197). Work grows by another kMax x 4 B (`kParPix`).
- **Limit / risk:** an instanced vegetation clump touching a mover at the same depth (within 2 %) may inherit that mover's motion
  for those frames (the same depth-overlap limit as phase 6). Roadside grass away from traffic votes for the static ground and
  stays still.
- **WARP harness S8** adds an instanced licence plate (`DrawIndexedInstanced`, 1 instance, view-space vertices) on the turning
  trailer -- all its pixels must move with the trailer and `noparent` must fail on it -- and an instanced grass clump next to the
  road that must stay static (camera R).

#### Phase 6c: the marching vote (v0.10.0, part of `mv_drawid_parent = 1`)

- **Finding (in game, ETS2 flat, log `dlaa_inject_ets2_v0100p6b_flat.log`):** plates took a parent only SOME of the time --
  `parent: inherited 12-17 of 13-18 unpaired ... no votes 22-33, no majority 18-29`; a close car's plate was pale / white in most
  Ctrl+F6 frames, green only now and then. A plate is a flat patch: the +-2 / +-5 px samples of its pixels are mostly the plate
  itself or its own text (another instanced draw, not a valid source), so only the thin border ring reached the body -- too few
  votes (< 32) or a split (< 60 %); far / small plates never reached 32.
- **March (`CSParentVote` -> `ParMarch`):** every sampled pixel of a listed draw U (every 2nd pixel in x and y) walks outward in
  **8 directions in 2 px steps up to 32 px** (per axis). It passes over U's own pixels and over pixels of draws that have **no
  motion** this frame while they stay within 4 % (relative linear depth) of U's pixel -- other listed draws (plate background /
  text), unpaired / instanced draws -- and stops at the first draw that **has a motion** (`ParSource`: state 2 / 3 from its own
  pair, a matrix twin or an origin attach; in the 1st pass never a listed draw). That draw is the direction's vote when it lies
  within **4 %** of U's pixel (the spec's gate; 2 % before) AND within **1.5 % of the last patch pixel before it** (edge
  continuity: an attached part is flush with its parent); otherwise the march is **depth-rejected**. Sky / no G-buffer id /
  another depth layer / a motionless surface at another depth / the 32 px reach / the screen edge = **no source**.
- **Winner (`ParBest`):** candidates with the **same motion pool their votes** (both static, or both movers whose R rows agree
  within 2e-3: body, bumper, lamps of one vehicle). A motion qualifies when it was met on **two opposite sides** of U (left +
  right, up + down or a diagonal pair), or when it is the **only** motion met at all (a plate in a body corner). The best motion
  needs **>= 8 pooled votes and >= 50 %** of U's accepted votes; a tie goes to the candidate nearer in depth to U (smaller mean
  relative depth difference), then the lower draw index.
- **2nd pass:** `CSParentPick1` sets a pass flag; `CSParentVote` runs again only for listed draws still without a parent, twin
  or origin attach, now with the listed draws that took a parent in the 1st pick (or hold a twin / attach) as sources, and
  `CSParentPick2` applies the same rule and records the winner's ROOT parent (`kParVT` bit 31 = "2nd pass" in the dump). No race:
  a 2nd-pass voter never holds a motion, so it is never a 2nd-pass source.
- **Cost bound:** `CSParentCount` now always runs with the parents and also counts the genuine unpaired draws' RED / GREEN id
  pixels; `CSParentList` derives a per-draw grid decimation `k = ceil(sqrt(8 * pixels / 8192))`, so every listed draw marches at
  most ~8192 times per pass, spread evenly over the draw. Vote tables grow to 64 x 192 B (Work + 4 KB); DidCB + 16 B (`Par2.x` =
  the 1.5 % edge tolerance).
- **Why the extra rules (WARP evidence):** with the plain spec rule (4 % gate, >= 8, >= 50 %) S1-S3 failed: grass clumps that
  stand 0.3 m in front of / inside a moving vehicle's lane took its motion (2.3 % depth step: inside 4 %; the body above and
  beside them won). The edge-continuity test rejects the 0.3 m step; the opposite-sides-or-unanimous rule rejects a body that only
  touches the top of a clump that also meets the ground (removing it alone brings back the frame-34 failure).
- **Stats** (`MV draw-ids @blit`, `parent:` field): `of U unpaired + I instanced/no-MVP listed, no parent K/frame (...; 2nd pass
  via a listed neighbour C; no votes (no source within 32 px) A, depth-rejected D, no majority B, no pixel sampled (hidden /
  sub-pixel) P, over the 64 cap O; avg V votes per listed draw, marched avg S px over M marches/frame (R depth-rejected, N without
  a source))` (Cnt dwords 199..204). `no
  parent` now counts every listed draw (phase 6 / 6b left the instanced ones out, so `inherited` could exceed `unpaired`).
  Config line: `phase 6c (v0.10.0): rigid-parent vote = MARCHING ...`. Dump: `| parent -> id P GBUF (votes V/T, rigid, 2nd pass: via
  a listed neighbour that took its motion first)`, `| no parent (best V of T votes: no majority, needs >= 8 votes and >= 50 % |
  depth-rejected, every source within 32 px at another depth | no source within 32 px | no pixel of it sampled: hidden /
  sub-pixel | not listed, over the 64 cap)`.
- **Limits / risks:** a plate met by its body on one side only while another motion is also met (e.g. a plate at a body corner
  next to a different vehicle) gets no parent; a fragment of a plate with fewer than 8 accepted marches (a few pixels left
  visible behind an occluder) keeps the camera R; a static object FLUSH (within 1.5 %) with a moving body and surrounded by it
  on two sides takes its motion (the same geometric ambiguity as a plate); 1-px body borders can be stepped over by the 2 px
  march; 2nd pass only for draws with no twin / attach.
- **WARP harness S8** adds a FAR view-space plate 24 x 8 px on a weaving van ~24 m ahead, an INSTANCED plate background with
  INSTANCED text fully inside it on the turning trailer's top band, and an INSTANCED plate on the trailer's BOTTOM edge (the road
  seen under the trailer is ~12 % deeper: those marches are depth-rejected); every plate pixel must move with its body and
  `noparent` fails all of them. A second run "S8 short reach" (6 px reach, harness-only `DrawIdMv::SetParentReach`) makes plate
  text reach its trailer only through the 2nd pass. The phase-6b vote, run on the same scene, gets the instanced text right in
  1 of 98 frames (the new vote: 98 of 98).

#### Phase 8: performance -- every sub-pass timed per eye, and the cuts (v0.10.0)

Goal: <= 1 ms per eye for everything except the NGX evaluation itself, VR (2 eyes, 4592x6496, 72 Hz) included. Measured before
(flat 2880x2160): replay 0.13-0.2 ms, "pairing GPU" 1.0-1.4 ms (the whole per-draw chain in one figure), pass B 0.15 ms, mirrors
~0.3-1.2 ms per frame, forward ids 0.03-4.4 ms (one timer around the forward replay, suspicious).

- **Timers (`src/gpu_perf.h/.cpp`, GpuPerf):** timestamp queries only, ONE `TIMESTAMP_DISJOINT` per Present frame around all of
  them, read 2-4 frames later with `DONOTFLUSH` (never a blocking readback). Each section is tagged with its PASS; the blit that
  consumes the pass names the eye, so a pass whose replay and blit fall in different frames still sums correctly. Sections:
  replay, fwd-replay, snapshot, copy-in, convert, medoid (pass A + the candidate-record copy), gather / match / pair / vote /
  pick / resolve / inherit / p-count / p-list / twin / attach / p-vote1 / p-pick1 / p-vote2 / p-pick2 (one timestamp per
  dispatch: `Next`), pass-B, mv-misc (diagnostic / commit copies), NGX (not in the total), rcas, copy-out (copy-back or the
  upscale composite), dbg-view, mirrors (everything a mirror unit does but its NGX) and mirror-NGX (not in the total). SAMPLED
  (1 pass in 31, every piece bracketed = an upper bound, averaged per sampled pass): cand-copy* (the 40 + 24 per-draw candidate
  copies inside the G-buffer), fwd-depth* (the forward-depth re-draws inside the forward pass), replay-inst* / replay-rest* (the
  replay split into runs of instanced / other draws; a breakdown of "replay", not in the total). On with `debug = 1`
  (`perf_timers = -1`, default) or `perf_timers = 1`; the GPU spans scene figure is measured too while the timers are on.
- **Log, every 600 blits:** `perf eye N @blit B: mod GPU T avg / M max ms per pass (P passes; everything except NGX; NGX x avg,
  mirror NGX y avg) | GPU spans scene S ms/frame (n=K) | ms avg/max: replay a/b fwd-replay a/b ... cand-copy* a/b (n sampled)
  ...` (sections that never ran are left out) and `perf cuts @blit B: ...` (the state of every cut below).
- **Cut 1, `mv_vote_res = 4` (2 = phase 6c):** the rigid-parent pixel count and vote sample every 4th pixel in x and y and march
  in 4 px steps -- a quarter of the samples, half the steps. A listed draw with < 64 counted samples (~1000 px: far plates,
  grass) is FINE: the vote thread then also reads the three other 2 px samples of its 4x4 cell (only while a fine draw exists:
  `kParAnyFine`) and a fine draw marches from all of them in 2 px steps -- exactly the phase-6c samples of that draw. (A first
  version marched fine draws only inside the box of their coarse samples: a sparse grass clump lost most of its blades and took a
  passing vehicle's motion in S1 frame 34; the cell read has no such bias.)
- **Cut 2:** `mv_vote_march_cap = 48000` marches per frame (5/6 for the 1st pass shared by the listed draws with pixels, 1/6 for
  the 2nd pass shared by the draws that march again; each draw's grid decimation follows); `mv_cons_cands = 64` world consensus
  candidates (cabin: half, <= 64; 128 / 64 before); the vote grid is `DispatchIndirect` with args written by CSParentList: no
  listed draw with pixels = no vote grid at all (the 1-group picks still run and keep the per-draw reasons).
- **Cut 3, `mv_medoid_drop = 120` (0 = never):** a layer whose consensus camera R was used (>= 24 draws) in 120 landed readbacks
  in a row stops the old medoid path for the next passes: no camera candidates are copied for it (`CandidateRecord::SetDropped`,
  inject.cpp StartPass; mirror units by their own unit), pass A skips that layer (`Params.z`), and CSPick writes the layer's info
  rows itself (world: R_ego = the camera R, InvRow3 from the winning draw's inverse MVP). A frame without a world medoid decides
  "world miss" from the per-draw pairing (no previous pass / no partner group), not from the dropped candidates. If the
  consensus fails in such a frame, a smaller cluster (>= 8 draws) is used rather than the last frame's R (state 4), and the
  failure re-arms the medoid (the candidates come back from the next pass on; ~3-5 frames of reaction). VR: both eyes must be
  healthy and the eye map settled (the eye verdict reads those candidates). ClassifyRecord ignores a dropped layer.
- **Cut 4, `mv_inst_replay` (default 0 until the VR log shows the cost):** 1 = an instanced / no-MVP draw is replayed only when
  its SHAPE key (index count, instance count, VS, input layout -- the start index / base vertex of a CPU-written plate change
  every frame) took a MOVING rigid parent within the last 240 frames (a bitmask in the diagnostics readback), and every
  instanced draw in every 7th pass (the probe that finds a new plate: <= 7 passes + 2-3 frames readback). The brief's "within
  15 m of a paired mover" is not computable for instanced draws (their cb0 rows 4..7 are not the instances' MVP). The input for
  the decision is `replay-inst*` in the perf eye line.
- **Cut 5, `mirror_vr_mode` (VR only; default 1):** 0 off, 1 every mirror view, 2 only the views with the largest area (the
  main mirrors; re-learnt after 600 frames without one), 3 each unit every 2nd frame (units alternate by index; on its off
  frame the view is neither jittered nor recorded and gets the unit's last AA'd picture copied in -- same size / format only;
  the history continues over the 2-frame gap).
- **Cut 6 (pass B / depth convert):** nothing redundant left to merge: the depth convert is already inside pass B (v0.5.7), the
  convert pass only runs when the MV pass does not, the snapshot must copy the whole D32S8 depth (the game's depth has no SRV
  binding), the coverage counters are ~1 atomic per 64 px. Not changed. The copy-in / copy-out around NGX are now timed
  separately: if they matter in VR, a later step could hand NGX the game's texture directly (format / subrect permitting).
- **Cut 7, `mv_slot_pool = 1` (0 = phase 7):** a pass slot's depth twin / forward depth / draw-id target go back to a process-
  wide pool once its blit consumed the pass (`DepthTwin::ReleaseToPool`), and the next pass takes a matching set before
  creating one: alive sets = passes in flight (flat 1-2, VR 2-3) instead of 4 -- in VR ~0.95 GB less (each set ~477 MB at
  4592x6496), flat ~200 MB. Only pass-slot twins pool (`pooled`; mirror / preview twins never touch it); another size drops
  the pooled sets of the old size.
- **WARP harness:** `legacy` = every cut off; the comparison mode (`cmpdump=` / `cmp=`) shows the defaults give the SAME motion
  vectors as phase 7 except where the consensus picks another of its agreeing candidates (64 instead of 128): max 0.07 px
  difference; `vres` and `cap` alone are bit-identical on S1-S8. S10 checks the pool (2 sets alive for 2 passes in flight over
  200 passes, mirror twins untouched, resize drops). The per-scene "WARP perf" lines are CPU figures (WARP rasterises on the CPU),
  not GPU numbers.

#### Phase 9: VR cost -- the per-pixel work only inside the DLAA area, forward depth at half resolution (v0.10.0)

The first VR log (ATS, Quest 3, eye 6120x6496, `dlaa_area = 40` -> NGX reads a 2448x2600 rect per eye) had the mod at 4.2-4.5 ms
of GPU per eye besides NGX; 2.5 ms of it re-drew the forward pass's depth over the WHOLE eye, the draw-id replays, the
rigid-parent vote and the mirrors made most of the rest. The user's 72 Hz target leaves ~2-3 ms per frame for the mod.

- **Area scissor, `mv_area_scissor = 1` (0 = phase 8):** with `dlaa_area < 100` each pass gets a clip rect at its start
  (`PassViewSetup`): the union of the DLAA rects of every eye the pass may become (flat: the centred rect; VR: both eyes' rects
  around their optical centres -- the eye of a pass is only known at its blit) plus `mv_area_margin` px (default
  `dlaa_area_feather` + 32, the vote's march reach). The G-buffer and forward draw-id replays (`DrawIdRecord::SetView`), the
  forward-depth re-draw and the rigid-parent pixel count / vote (`Clip` in the pairing cbuffer, x0 / y0 aligned to 4 px so the
  samples stay on the global grid) run only inside it; everything outside holds id 0 / forward depth 0 (a march that leaves
  the clip ends there as at the sky). Pass B, copy-in / copy-out, NGX and RCAS already ran on the DLAA rect. A draw is clipped
  through a scissor-enabled copy of its own rasterizer state (`DrawIdRecord::ScissorRs`: identical raster rules, so every
  pixel's depth is unchanged -- depth EQUAL still holds); a draw whose state already scissors keeps its own rect n the clip.
  The depth snapshot stays a whole-texture copy: D3D11 copies a depth-stencil resource only whole, and the game's depth has
  no shader-resource binding to copy a rect with. The pairing / consensus / per-draw verdicts are per DRAW and unaffected; a
  draw whose pixels all lie outside the clip can get another verdict (no pixel = no parent vote) -- it has no pixel NGX reads.
- **Forward depth at half resolution, `mv_fwd_depth_res` (0 = auto: 2 in VR, 1 flat; 1 = phase 8; 2 = half):** the forward-depth
  target is (W+1)/2 x (H+1)/2; the re-draw halves the game's viewport (origin and size, the jitter included) and scissor
  (left / top floor, right / bottom ceil) and binds a same-size R8_UNORM dummy RT0 (an RTV must match the DSV's size;
  alpha-to-coverage still reads the game's pixel shader's output 0, colour writes stay off). The forward ids then live in
  their own half-size R16_UINT target (the full-size id target stays one channel) and the forward replay applies the same
  viewport / scissor transform (`ScaleViewport` / `ScaledScissor`, shared with the re-draw) -- the replayed depth equals the
  re-drawn one bit for bit. Pass B, the vote and the Ctrl+F6 view read the forward depth / ids at pixel >> 1 (`FwdMode` in their
  cbuffers). Trade-off: a forward surface 1 px wide at full resolution can lose pixels or widen to 2 px (WARP S11: 85 % of the
  pixels the forward depth touches keep their exact full-resolution motion, the rest move by up to a few px on a 2-px wire).
- **Mirrors (VR):** `mirror_vr_mode 2` = the `mirror_vr_views` (default 2) largest views of the previous frame keep their unit
  (identified by size + order among the views of that size; the rest are left to the game); `mirror_vr_mode 4` = every view
  keeps DLAA but without per-draw ids (no record / replay / pairing in the mirror units: their camera R is the medoid of their
  camera candidates -- the cheapest DLAA mirror). The per-unit cost is in every `mirror units @blit` line (`GPU: DLAA ... replay`).
- **Folded dispatches, `mv_fold_dispatch = 1` (0 = phase 8):** every dispatch has a fixed GPU floor plus the barrier the driver
  puts between dependent dispatches. `CSPickResolve` (one group of 256) = the consensus pick + resolve + inherit; `CSListTwin`
  (one group of 256) = the rigid-parent list + twin + attach -- same code and order, device-memory barriers in place of the
  dispatch boundaries; 15 dispatches per eye become 11. gather / match / pair / vote / the pixel count / the vote passes stay
  separate (each needs every draw or pixel of the step before). The parent list also ranks the instanced candidates in a
  compact groupshared list (it looped over all draws in device memory: `p-list` 0.2 ms per eye in the VR log).
  Phase 18: the default is now `mv_fold_dispatch = 2` -- `CSListTwin` stays, but the single-group `CSPickResolve` (0.15-0.18 ms
  per eye in VR at ~2200 draws) is replaced by `CSPickPar` + the wide `CSResolveInherit`; and `mv_vote_compact = 1` runs the
  vote passes only over the tiles that hold a countable pixel, one thread per march direction (docs/DEVELOPMENT.md, "Phase 18").
- **Log:** config line `phase 9 (v0.10.0): mv_area_scissor=... mv_fwd_depth_res=... mirror_vr_mode=... mirror_vr_views=...
  mv_fold_dispatch=...`; once `MV area scissor: ACTIVE ... (l,t)-(r,b) of WxH = P % of the image` and `MV forward depth: 1/2
  resolution ...`; every 600 blits `perf view @blit N: area scissor ... passes clipped, rect P % ... | forward depth at 1/2
  resolution in N of M passes | dispatch chain folded ...`; the `perf eye` lines gain `pick+res` and `list+twin` (the folded
  dispatches) in place of `pick resolve inherit p-list twin attach`.
- **WARP harness:** S11 = S6 and S12 = S8 with two SHADOW pipelines each (the clip of a 40 % area around an off-centre optical
  centre -- the crop's edge runs through the vehicles / the turning trailer and its plates -- + a 40 px margin; full and
  half-resolution forward depth): the shadow's ids inside the clip equal the whole-image pipeline's (0 outside), its motion
  vectors inside the crop are bit-identical (max 0.0000 px; half resolution: every pixel the forward depth does not touch);
  every half-resolution forward-depth pixel has a forward id. Mutations `mutclip` / `mutview` fail it. The phase-8 comparison
  file of S1-S10 is reproduced exactly (0 px differ) with the folded dispatches.

#### Phase 10: the forward depth in one batch, performance profiles (v0.10.0)

The phase-9 VR log (ATS, 4592x6496 per eye, area 40) still showed `fwd-depth*` 2.1 / 1.9 ms per eye -- an UPPER bound: every one
of the ~290 forward re-draws per pass was bracketed by its own timestamp pair (serialising the GPU around each) and interleaved
with the game's forward draws. RenderDoc (`scripts/rdc_fwdbatch_audit.py`, flat frame 1969): the 111 qualifying draws use 2 pixel
shaders (~22 / ~38 instructions, one texture sample, no discard) and cost the GAME 0.22 ms at 2880x2160 with its depth culling;
no resource they read (VS / PS cbuffers in the 2 MB ring, VBs, IB, the 13 PS SRVs) is written between them and the end of the
forward pass.

- **One batch per pass (`FwdBatch`, inject.cpp):** `FwdOnDraw` only qualifies (the r5 / r11 rules, unchanged) and RECORDS the draw
  into the pass's forward `DrawIdRecord`, which now also keeps the pixel-shader state (`SetCapturePs`: PS, PS cb 0..3 with D3D11.1
  offsets, SRV 0..15, samplers 0..7; their resources join the hkMap pending set). At the scene depth discard -- after the depth
  snapshot -- (or the forward leave / the next pass start / forced by a WRITE_DISCARD of a pending draw's buffer) the forward depth
  is cleared, optionally initialised from the snapshot (below) and the recorded draws are drawn again in draw order with the
  game's VS / PS and resources, our GREATER_EQUAL + write / alpha-to-coverage + colour mask 0 states, the pass's view (area clip,
  1/2 resolution) and an R8 dummy RT0 (`DrawIdRecord::ReplayDepth`); the forward-id replay of the same record follows (ids on) or
  the record is released (`DropIds`, ids off). The WARP harness shows the batch is bit-identical to the interleaved re-draw.
- **Occlusion init, `mv_fwd_depth_cull = 1` (0 = the target starts empty):** a full-screen triangle (`kFwdInitShader`, SV_Depth,
  depth ALWAYS) writes min(the 2^shift x 2^shift snapshot texels of the pixel) x (1 - 1e-6) where that lies in the world range
  [0.01, 0.9), else 0. It is strictly below the snapshot, so it never wins pass B / the vote (both test `fw > dt` strictly) and
  the Ctrl+F6 yellow stays where a forward surface won; a forward fragment behind the scene at every pixel it covers is rejected
  by the depth test instead of running the game's pixel shader -- it could not have won either. WARP: RED ids and every motion
  vector bit-identical to the interleaved path, GREEN ids identical wherever the forward depth won (S11 / S12 batch shadows).
- **Timing:** the GpuPerf sections `fwd-init` (clear + init) and `fwd-depth` (the re-draws) are EXACT and timed every pass (no
  star any more); `g_fwdBatchTimer` (always on) feeds the `MV forward depth batch` line and the VR budget.
- **VR budget, `mv_fwd_depth_budget_ms = 0.30` (0 = off):** when `mv_fwd_depth` is not set in dlaa.ini and the batch averages
  above the budget per eye pass over 300 timed passes in VR, the forward depth switches off for the session (one line).
  `Ctrl+F3` brings it back and makes it the user's choice.
- **Performance profiles, `perf_profile = high | medium | low` (Alt+F9 cycles, Shift+F12 saves):** a profile only sets the keys
  that are not explicit (set in dlaa.ini, or changed by their own key this session); see docs/dlaa.ini.sample for the table.
  high = the built-in defaults; medium = model E, half-resolution forward depth also in flat, flat mirrors with camera motion
  only (`mirror_flat_mode 4`), march cap 24000, `mv_inst_replay 1`; low = medium + `mirror_dlaa 0`, `mv_fwd_depth 0`,
  `mv_parent_max_listed 32` (new: the rigid-parent vote's listed-draw cap, `FoldU.y` in the pairing cbuffer) and the VR DLAA area
  60 %. Shift+F12 writes `dlss_preset` / `dlaa_area` / `mirror_dlaa` only when explicit.
- **Mirrors:** `mirror_flat_mode` (new, default 1) gives flat the same modes as `mirror_vr_mode`; the VR default is now 2 (the
  two largest mirror pictures with per-draw ids) -- mode 4 (all pictures, camera motion only) removes only the per-draw chain of
  each unit (~40 % of a unit's non-NGX cost by the phase-9 numbers), which is not clearly cheaper than 2 pictures in mode 2.
- **Depth snapshot (not changed):** the game's depth is a typed D32_FLOAT_S8X24_UINT texture with DEPTH_STENCIL binding only (no
  SRV possible). Letting the game render into OUR typeless depth (substituting its 3 DSVs in OMSetRenderTargets / ClearDSV /
  DiscardView, dropping its discard) would save the 0.17 ms per eye copy, but a bind we do not see (a deferred context, a path
  outside the audited frame) would make the game test its G-buffer against an uncleared texture -- scoped in the v0.10.0
  handoff (phase 10, job C), not built.
- **Log:** config line `phase 10 (v0.10.0): perf_profile=... -- set by the profile: ... | kept (set in dlaa.ini): ...`; once
  `MV forward depth: first BATCH ...`; every 1200 passes `MV forward depth batch (v0.10.0 phase 10) over N passes: D draws
  re-drawn per pass ..., occlusion init in I ... | GPU x ms avg / y max per pass ...`; the `perf eye` lines show `fwd-init` and
  `fwd-depth`; the FIFO stats bracket starts with `profile=...`; Alt+F9: `perf profile now ... -- changed: ...`.

#### Phase 11: LOD bias scope (v0.10.0, `tex_lod_bias_scope` / `tex_aniso_scope = solid`, default)

User finding: `tex_lod_bias = -1` makes sign text crisper but gives strong moire on fences and other see-through meshes at a
distance (wires thinner than a pixel sampled from a too-sharp mip). The game draws solid surfaces in the 4-RTV G-buffer pass
and the see-through things (fences, wires, glass, plates, decals) in the 1-RTV RGBA16F forward pass, plus some blended /
alpha-to-coverage draws inside the G-buffer.

- **solid (default):** the v0.9.0 sampler twins are bound only for the draws of the world G-buffer pass and of the menu
  truck-preview passes (one forward-lit pass, solids and see-through mixed) whose current blend state has `BlendEnable` off on
  RT0 and `AlphaToCoverageEnable` off; never in the world forward pass. Mirror views keep the game's samplers in both scopes
  (as since v0.9.0). **all** = the v0.9.0 behaviour. The two keys are independent: each game sampler gets a FULL twin (bias +
  aniso, for the solid draws) and a SEE-THROUGH member (the original with both scopes solid; a bias-only / aniso-only twin when
  one scope is all; the full twin with both all). Load-time keys; Ctrl+F1 / F2 still change only the bias value.
- **Per draw (inject.cpp `LodOnDraw`):** a new `OMSetBlendState` hook (vtable 35) keeps the game's current blend state
  (identity; resynced by one `OMGetBlendState` at every per-draw pass entry); each state object is classified once (`GetDesc`,
  memoised in a pointer hash holding a ref). `hkDrawIndexed` / `hkDrawIndexedInstanced` / `hkDraw`, right before the game's
  draw: a solid draw needs the full set, a see-through draw the see-through set. The bound set stays across consecutive draws of
  the same kind; a change is ONE `PSSetSamplers` over the slots where the two sets differ. `hkPSSetSamplers` binds the bound
  set's member of each new sampler; the OM-bind enter / leave (`LodOnBind` / `LodEnter` / `LodLeave`) is the v0.9.0 one,
  extended by the set choice. With both scopes solid the forward pass gets no scene state at all (zero calls).
- **Cost:** per hooked draw one relaxed atomic load + a pointer compare (a hash probe after a blend-state change); extra D3D
  calls per frame = the set swaps (one `PSSetSamplers` per solid <-> see-through transition inside a G-buffer / preview pass,
  shown in the stats tag) + one `OMGetBlendState` per G-buffer / preview pass entry + one `GetDesc` per new blend state object
  (once per session). Both scopes solid also REMOVE the v0.9.0 enter / leave / per-call twin work of the forward pass.
- **Risks:** a sign or decal drawn WITH blending (or alpha-to-coverage) in the G-buffer loses the bias; an alpha-TESTED fence
  (`clip()` in the pixel shader, no blend, no A2C) in the G-buffer keeps it; `DrawInstanced` / indirect draws (not hooked) use
  the set the previous hooked draw left bound. Without the blend hook the scope falls back to pass level (G-buffer / preview
  biased, forward not).
- **Log:** config line `texture LOD bias (v0.9.0): ... tex_lod_bias_scope=solid tex_aniso_scope=solid -- ...`; FIFO stats tag
  ` lod=-1.00 aniso=0 scope=solid/solid twins=N (see-through M) ... subst/frame=solid S / see-through skipped T (draws; set swaps
  W/frame, slot subst X/frame, blend states a solid / b see-through, resyncs R) enters/frame=E` (scope all/all: the v0.9.0
  `subst/frame=X`).
- **Test:** `tests\build_lod_scope_test.bat` -> `build\tests\lod_scope_test.exe` (inject.cpp without WITH_DLAA in one TU on a
  WARP device, the real `LodOnBind` / hooks; reads the bound PS samplers back after every step): 60 / 60 checks, the four scope
  combinations, read-back twins, blend resync, feature off.

#### Phase 12: alpha-tested cut-outs keep the game's samplers (`tex_lod_bias_scope` / `tex_aniso_scope = opaque`, default)

RenderDoc (`captures/ats_flat_fence_frame1634.rdc`, ATS flat, `scripts/rdc_fence_audit.py`): the fence the user saw with
moire is a wire-mesh fence drawn INSIDE the G-buffer -- 68 draws, opaque blend state (no blending, no A2C), pixel shader
`discard_nz` when `texture alpha * vertex alpha < 0.05` (a 128x2048 BC3 atlas: welded grid, chain link and barbed wire, a full
12-mip chain, sampled anisotropic x16 trilinear, WRAP). Phase 11 called it "solid" and bound the -1 twin (the phase-11 risk).
In that frame 303 of the 1554 G-buffer draws are alpha-tested (grass, leaves, fences; 21 of the 126 pixel shaders discard).

- **Rule:** a G-buffer / preview draw whose current pixel shader contains a `discard` (any of `discard_z` / `discard_nz`: HLSL
  `clip()`) is a CUT-OUT. Scopes per key: **opaque** (new default) = solid draws without discard only; **solid** = the phase-11
  rule (cut-outs biased too; `0` = its old alias); **all** = every draw (v0.9.0; `1`). With opaque / opaque a cut-out draw binds
  the see-through set (= the game's samplers), so there is no extra twin and no extra swap between cut-out and blended draws.
- **How:** a `CreatePixelShader` hook (device vtable 15, installed FIRST in the setup thread) scans the game's DXBC once per
  shader object (`DxbcPsClass`: walks the SHDR / SHEX tokens, `0x0D` = discard, customdata lengths honoured; checked against
  all 126 pixel shaders of the capture: 21 / 105 exactly as RenderDoc's disassembly) into a lock-free pointer table
  (`g_lodPsTab`, any thread inserts, a shader re-created at a reused address overwrites its entry and bumps a generation). A
  `PSSetShader` hook (context vtable 9) keeps the current PS (identity; one `PSGetShader` resync at every per-draw pass entry).
  `LodOnDraw` -> `LodWantSet`: blend solid and PS cut-out -> the cut-out set (memoised per PS pointer + table generation).
  A third sampler member (`LodEntry::val3`, the CUT-OUT twin) exists only when the two scopes mix `solid` and `opaque` with
  both bias and aniso active; otherwise the cut-out set IS the full or the see-through set.
- **Unknown shaders:** a PS created before the hook (the game boots its shaders while the setup thread installs hooks) or not
  parsable counts as "no discard" = the phase-11 behaviour; counted per frame in the stats tag.
- **Cost:** per hooked draw in a per-draw pass + one pointer compare / atomic load (the PS memo); per `PSSetShader` one store;
  per created PS one scan of its bytecode (once). Set swaps only at solid <-> cut-out transitions inside the G-buffer (ATS frame
  1634: 1251 solid / 303 cut-out draws in runs: 12 solid <-> cut-out transitions = 12 extra `PSSetSamplers` per frame).
- **Log:** config line `... tex_lod_bias_scope=opaque tex_aniso_scope=opaque -- ...; v0.10.0 phase 11/12 scope, decided per draw:
  opaque = ...`; hook lines `CreatePixelShader hook installed` (first) and `PSSetShader hook installed`; once `texture LOD bias:
  first alpha-tested draw (pixel shader ... has a discard) in a G-buffer pass -> the see-through set (...)`; FIFO stats tag
  `scope=opaque/opaque twins=N (see-through 0, cut-out 0) ... subst/frame=solid S / see-through skipped T (draws; alpha-tested
  C/frame -> see-through set, unknown-PS draws U/frame, pixel shaders a with discard / b without / c unparsed; set swaps ...)`
  -- T now includes the cut-outs (> 0 wherever grass / fences / trees are on screen).
- **Test:** `lod_scope_test.exe`: 127 / 127 (the 60 phase-11 checks unchanged + 67: the scanner on compiled `clip()` / branch /
  immediate-constant-buffer / plain / vertex shaders and malformed blobs, the hooks, opaque / solid / all and both mixed
  scopes incl. the third twin, the PS resync, a re-created shader at a reused address); `--scan <dir>` classifies dumped DXBC.

#### Phase 12: no jitter while NGX cannot run

The fence capture's session ran under RenderDoc: every NGX init failed (`NVSDK_NGX_Result_FAIL_PlatformError`), no DLAA ever
evaluated (`dlaa frames e0=0`, 4280 x `DLAA eval ... ok=0`) -- and the mod kept jittering the viewport, so the picture just
shook (fine wire patterns most). `JitterLive()` now returns false while `DlaaProcessor::NgxInitFailed()` (the last NGX init
attempt of the process failed, none succeeded since): no viewport jitter (world, preview, mirrors) and no texture LOD bias;
one log line `jitter HELD: NGX could not be initialised ...`. A later successful NGX init brings both back.

#### Phase 13: the alpha-tested draws get a bias of their own (`tex_lod_bias_cutout = -0.5`, default)

User finding after phase 12: the fence moire improved, but the GRASS in front of the fence went soft -- it is an alpha-tested
G-buffer draw too (the same class as the fence: discard PS, opaque blend) and lost the -1 it had before. The fence wants 0,
the grass -1; the PS class cannot tell them apart, so the cut-out class gets a bias VALUE of its own.

- **Rule:** `tex_lod_bias_cutout` (-3..+1, default -0.5; `same` = the phase-12 scope rule) is the MipLODBias of every
  alpha-tested G-buffer / preview draw, independent of `tex_lod_bias` (the solid draws). A numeric value wins over
  `tex_lod_bias_scope` for the cut-out class (the scope still decides the see-through class). The key ABSENT = -0.5 with scope
  opaque (the default) and `same` with scope solid / all, so an ini that asks for the phase-11 / v0.9.0 rule keeps it.
  `tex_lod_bias_cutout = 0` with scope opaque = phase 12 exactly (the cut-out class binds the see-through set = the game's
  samplers, no extra twin). The value is absolute: the `tex_lod_bias_auto` term is NOT added (a cut-out sampled sharper at
  render resolution is what moires). `tex_lod_bias = 0` (effective 0) still turns every bias off, the cut-outs included, so
  the default -0.5 changes nothing for users who never enabled the LOD bias. Aniso follows `tex_aniso_scope` as before.
- **How:** the phase-12 third sampler member (`LodEntry::val3`, `g_lodCutS`, `kLodSetCut`) now carries the cut-out VALUE:
  `LodCutEffective` -> `g_lodCutEff` per frame (a change bumps `g_lodGen` -> the twins are rebuilt at the next scene enter);
  `LodFrameUpdate` compares the three classes' (bias, aniso) as values: the cut-out class binds the full set or the
  see-through set when its values equal theirs, else its own set. `LodOnDraw` / `LodWantSet` / `LodBindSet` / `hkPSSetSamplers`
  / `LodEnter` / `LodLeave` are unchanged (they already handle three sets).
- **Keys:** `Ctrl+F1` / `Ctrl+F2` still change `tex_lod_bias`; NEW `Ctrl+Shift+F1` / `Ctrl+Shift+F2` (`key_lod_cutout_down` /
  `key_lod_cutout_up`) change `tex_lod_bias_cutout` in 0.25 steps within -3..0 (two beeps, the same pitch rule; from `same`
  the first press starts at the value the scope gives and becomes a value of its own); `Shift+F12` writes it (`same` or the
  value). Exact modifiers: `Ctrl+Shift+F1` does not fire `Ctrl+F1` or `Shift+F1`.
- **Cost:** with the default (-1 / -0.5, opaque) a third twin per biased game sampler (once) and a `PSSetSamplers` at each
  cut-out <-> blended transition inside the G-buffer (phase 12 had none there: both were the game's samplers), on top of the
  solid <-> cut-out transitions (ATS frame 1634: 12 per frame).
- **Log:** config line `... tex_lod_bias_scope=opaque tex_aniso_scope=opaque tex_lod_bias_cutout=-0.50 (default) -- ...;
  v0.10.0 phase 13: alpha-tested G-buffer / preview draws ... get tex_lod_bias_cutout as their own bias ...`; the first
  alpha-tested draw line names the set and its bias; key lines `texture LOD bias cut-out (alpha-tested G-buffer / preview
  draws) -0.75 (Ctrl+Shift+F1, ...)`; FIFO stats tag ` lod=-1.00 aniso=0 scope=opaque/opaque cutout=-0.50 twins=N
  (see-through 0, cut-out K) ... subst/frame=solid S (full set: bias -1.00 aniso 0) / alpha-tested C (-> cut-out set: bias
  -0.50 aniso 0, tex_lod_bias_cutout=-0.50) / see-through T (see-through set: bias 0.00 aniso 0) (draws; unknown-PS draws U/frame,
  ...; set swaps W/frame, ...)` -- S, C, T are now disjoint classes (phase 12's T included the cut-outs).
- **Test:** `lod_scope_test.exe`: 233 / 233 (the 127 phase-11/12 checks, run with `same`, + 106: default absent key with scope
  opaque (third set at -0.5, swaps, PSSetSamplers / read-back twin in the cut-out set, class counters, stats tag), absent +
  solid / all (= same), `same` / 0 with opaque (= phase 12, no third twin), a value equal to the bias (full set), a value over
  scope solid, aniso per scope, three distinct sets, the `tex_lod_bias = 0` gate, the step keys (limits, start from `same`, a
  live rebuild, `Ctrl+F2` leaves the value), the default bindings, the dlaa.ini parse + the save text round trip).

### Mirror units (v0.10.0, `mirror_dlaa = 1`, default)

The truck mirrors (ATS: also the car mirrors) are not part of the main picture: the game renders each one as its own
small view every frame, BEFORE the main scene, and the mirror glass in the main scene samples the result. Until v0.10.0
they were left alone ("secondary G-buffer view ... ignored", v0.8.2), so they were the one large area without
anti-aliasing (shimmering edges in the mirror). Each mirror view now gets its own DLAA unit. Code: the "MIRROR UNITS" block
in `src/inject.cpp` (`MirOnBind`, `MirBeginView`, `MirReplay`, `MirEndView`, `MirFlush`, `MirStatsLog`); facts and plan
in `captures/mirror_dlaa_plan.md`.

- **Engine facts** (RenderDoc `ets2_flat_frame1969.rdc`, `scripts/rdc_mirror_audit.py`, the F11 trace, the ATS car logs):
  every mirror view is a complete miniature deferred render: clear -> 4-RTV G-buffer (`RGBA16F` x3 + `RGBA16_UINT`, its
  OWN `D32_FLOAT_S8X24_UINT` depth, world layer [0.01, 0.9] only, ~150-750 draws with the full MVP in VS cb0 rows 4..7
  like the main pass, replay-verified) -> lighting (2 RTVs) -> forward (1 RTV `RGBA16F`, sky [0, 0.009] + world) ->
  `DiscardView` of its depth -> a 3-vertex down-sample of the `RGBA16F` forward colour into a 256² `R11G11B10`. All views
  come before the main scene. Sizes and counts vary: ETS2 flat 512x1024 (x2), 512x512, 512x256; ATS cars 2048x512,
  1024x512. Reflection-probe faces (1 RTV `R11G11B10`, 256², one draw) are not mirrors.
- **Detection** (`MirIsGbuf`, in `hkOMSetRenderTargets` after `InspectDepth`, never through it -- ETS2's 512-wide mirrors
  are below its 1024 px floor): a 4-RTV bind whose RTV view formats are `RGBA16F` x3 + `RGBA16_UINT`, RT0 the depth's
  size, depth `D32_FLOAT_S8X24_UINT` / `R32G8X24_TYPELESS` with 1 sample / 1 mip / 1 slice, NOT the scene depth, both
  sides >= `mirror_min_px`, and not a screen- or scene-shaped view >= 1024 wide (a resolution / Scaling change that
  `InspectDepth` adopts after its 30-frame expiry). The v0.8.2 ranking that protects the scene depth is unchanged; a
  mirror never becomes a main pass.
- **Units.** A VIEW lasts from its G-buffer bind to its depth discard. `MirBeginView` gives it a unit: the unit whose
  last view had the same G-buffer RT0, depth, size and **occurrence** of that pair within the frame (one pooled RT
  rendered twice per frame -- the two 512x1024 mirrors, VR eyes -- becomes two units, so no history alternates between two
  cameras); else the unit that had the same size and the same order among the views of that size LAST frame (re-home: the
  pool moved it to other textures; history reset); else a free unit; else the least recently used one (history reset);
  else the view is left to the game (`not handled`; `mirror_max_views`, default 8, max 16). Each unit owns a `SceneDlaa`
  (log tag "eye 20" + unit), a depth twin with its `R16_UINT` draw-id target, a candidate record, a `DrawIdRecord`, a
  read-only DSV of its mirror depth, its own Halton phase and two GPU span timers. Refs on the game's textures are held
  while a unit is in use (dropped after 120 frames without a view; never released at DLL unload).
- **Jitter:** the view's G-buffer and its forward pass (1 RTV `RGBA16F` of the view's size on the same depth) shift
  viewport 0 by the unit's own Halton phase (`ReconcileViewports`, `g_mirJit`); the lighting pass and the down-sample are
  never shifted. No jitter for a unit that cannot anti-alias (unsupported colour, a size whose build failed, 60 failed
  runs in a row).
- **Motion vectors:** the per-draw path of the main view, per unit. The G-buffer draws (`DrawIndexed` +
  `DrawIndexedInstanced`) go into the unit's camera candidates (world layer) and its `DrawIdRecord`; the G-buffer LEAVE
  (the lighting bind, before the game's bind goes through) replays them into the unit's draw-id target against a
  read-only DSV of the MIRROR depth, depth EQUAL. `hkMap` WRITE_DISCARD of a buffer the open view's pending draws read ->
  forced replay (2 per view) + ring flush / seal. Reason: the own trailer and truck fill a large part of a mirror and
  are static relative to the mirror camera while the world moves; their per-draw R is the identity (zero motion), where a
  camera-only motion vector would ghost them. The unit's camera medoid uses the main `mv_near_reject_m` (the own truck
  never drives it), no ego split; its `CameraMv` / `DrawIdMv` are quiet (no per-unit `MV:` lines, no blocking CB check).
- **DLAA** at the view's `DiscardView` of its depth (`MirEndView` -> `MirFlush`): a pending replay first, the depth
  snapshot into the unit's twin, then `SceneDlaa::Run` IN PLACE on the forward colour, outW = outH = 0 (output = input
  size), area forced to 100 %, an **HDR unit** (`RGBA16F` colour, NGX `IsHDR` unless `dlss_hdr = 0`), the unit's draw
  record -- all before the game's down-sample reads the colour. Fallback ends (logged as "late"): a ClearDSV of the
  unit's depth, the next mirror or scene G-buffer bind, the frame boundary. History reset: the unit's first run, a
  re-home / take-over, a frame in which the unit did not run (throttled mirrors), `MvInvalidate` (DLAA / MV keys),
  `Alt+F6`. Units are self-contained: the eye identity does not matter for them and nothing touches `g_sceneDepth`, the
  pass FIFO, the eye map or the main units.
- **Keys:** `Alt+F6` (`key_mirror_dlaa`) mirror DLAA on / off live (off = the views are left to the game, no jitter; the
  units keep their resources). `Alt+F5` (`key_mv_drawids`) per-draw motion vectors on / off live (`mv_objects` 2 <-> 0,
  main view and mirrors; off drops every draw record and marks the draw ids invalid, so pass B uses the camera path from
  the next blit on; `mv_objects = 1` is not switched). `Shift+F12` also saves `mv_objects` and `mirror_dlaa`.
- **Debug view** (`Ctrl+F6`): each handled mirror shows the motion-vector view (hue per draw, **magenta** movers, white
  unpaired) inside a frame in its unit's colour -- unit 0 red, 1 green, 2 blue, 3 yellow, 4 cyan, 5 orange, 6 white, 7
  purple (units 8-15 the same at half intensity). A mirror with the normal picture and no frame is not handled. The own
  trailer / truck in a mirror should be magenta (own R = identity), passing traffic too, the road and scenery not.
- **Logs:** config line `mirror units (v0.10.0 phase 2): mirror_dlaa=1 mirror_min_px=128 mirror_max_views=8 ...`;
  `mirror view detected: unit U (eye tag 20+U) WxH -- new unit / re-homed / taken over ...` (first 24 assignments);
  `mirror draw-ids: first replay (unit U, mirror G-buffer leave): N draws replayed ...`; `first mirror DLAA evaluate OK:
  unit U (eye tag ...) WxH HDR (RGBA16F, IsHDR=1) ...` (one per unit); `mirror DLAA: ACTIVE -- ...` (once); every 600
  blits `mirror units @blit N: ACTIVE | views/frame ..., handled ..., not handled ... | [unit U (eye 20+U) WxH: views,
  dlaa ok / fail / deferred, resets (re-home, take-over), late, no-colour, jitter, snapshot / missing | candidates/view |
  draw-ids: recorded, replayed, forced, no target, paired %, movers / static / unpaired per frame, id coverage, low-pair
  frames | GPU: DLAA ms avg / max, replay ms] ... | units K, mirror GPU ~x ms per frame`. WARNINGs: a unit failing 60
  runs in a row, a size whose build failed, a unit above 1 ms GPU per view (it keeps running), more views per frame than
  `mirror_max_views`. The FIFO stats line counts `mirror views handled=` / `not handled=`.
- **Cost (expected, not measured in game).** DLAA scales with the pixel count: the ETS2 flat set (~1.4 MP) is about a
  fifth of one 2880x2160 main pass; plus the replay (the mirror G-buffer's vertex work again) and a small MV pass per
  unit. VR renders the views per eye (2 x N units). Memory per unit: its textures (a few MB at 512 px) + its NGX feature +
  ~1.4 MB of recorded draw state. Feature creation is one per Present, so N new units take N frames to warm up.
- **Limits / risks.** Which secondary views are on-screen mirrors and which are dynamic reflections is not proven (plan
  R1): every view of the mirror shape is anti-aliased. Pool rotation that swaps two equal-size mirrors' textures between
  frames would hand a unit the other mirror's view without a reset (pointer keys cannot see it). The mirror forward
  pass's alpha-blended draws (wires, glass) have no forward depth (R5; the main view's `mv_fwd_depth` is not applied to
  mirrors). In VR the occurrence order decides which eye's view a unit gets; if the game swaps the eye order of the
  mirror views between frames, a unit's history alternates between the eyes (per-draw R still keeps the motion vectors
  exact). The texture LOD bias does not apply to mirror passes.
- **Synthetic test:** harness scene S5 (`drawid_harness.exe 5`): a 480x240 mirror view rendered before the 960x540 main
  view every frame, both from one cbuffer ring, each with its own depth / record / replay / snapshot / `CameraMv`; the own
  trailer static relative to the mirror camera, an overtaking car with 4 identical spinning wheels, every 37th mirror
  frame skipped. 9277 / 9277 checks (0 wrong ids, 0 wrong MV pixels; the trailer is a mover with R = identity in every
  frame with history, where the camera R would have moved it 2.26 px at the harness focal). S1-S4 unchanged.

### Per-object motion vectors (v0.9.0, `mv_objects = 1`; legacy since v0.10.0, see above)

The camera R is wrong for everything that moves on its own (AI traffic and its wheels, the own trailer in
the outside views): those pixels got the camera's motion and shimmered / lost detail under DLAA and DLSS.
Per-object MVs need to know, per pixel, which object it shows.

- **Where the ids live — the stencil.** The scene depth is `D32_FLOAT_S8X24_UINT`. RenderDoc audit of the
  flat capture (`scripts/rdc_stencil_audit.py`, numbers in `CAPTURE_FINDINGS.md`): the game clears depth +
  stencil every frame, writes the stencil in the G-buffer with `StencilWriteMask 0x0F` (REPLACE, ref 1 or 2)
  and tests it in the lighting / forward passes with `StencilReadMask 0x0F`. No SRV ever reads it. The upper
  nibble is free: **15 object ids** (1..15, 0 = static).
- **Writing the ids.** New hooks `OMSetDepthStencilState` (vtable 36) and `Map` (14). While the world
  G-buffer pass is bound, every game depth-stencil state is replaced by a cached twin (enter / leave at the
  OM bind, like the LOD sampler twins): a state that REPLACEs the stencil gets `write mask | 0xF0` and
  `ref = id << 4 | game ref & 0x0F`; a state without stencil (or with KEEP) gets stencil on, pass op
  REPLACE, write mask `0xF0`. Static draws write id 0 (clears an id that a later static draw covers); a
  tagged draw gets its id for that one draw (two state calls). The leave binds the game's own state back,
  so the lighting / forward passes see exactly what the engine set; the low nibble is written exactly as the
  game writes it.
- **Recording.** Every world-layer `DrawIndexed` of the pass goes into the pass's `ObjRecord`: identity
  (geometry key + occurrence index), key hash, occurrence index (r2), the byte offset of its MVP in the VS cbuffer
  ring, index count, id. The MVP is not copied per draw: the ring (2 MB dynamic cbuffer) is Map-DISCARDed once per frame
  before the first draw and holds the whole frame's constants, so ONE `CopySubresourceRegion` of the used
  range into a mirror buffer at the pass end replaces ~600 per-draw copies. A DISCARD of the ring while the
  pass still draws (never seen) marks the record torn (no per-object MVs for that pass). r3: up to 3 ring
  buffers per pass, and a DISCARD seals one ring instead of tearing the pass (see "r3" below).
- **At the blit** (`CameraMv::Generate`, after pass A, compute `mv_objects`, one thread per draw): gather
  the MVPs into the eye's ping-pong buffer; pair each draw with the previous pass of the same eye (same
  geometry key; for duplicate meshes the candidate whose origin is nearest to where the camera alone puts
  this one); verdict on 5 probe points (origin, ±2 m on local x / z): `dev` = screen deviation of the real
  previous position from the camera-only prediction (px), `disp` = clip-space distance (~m, plausibility:
  < 6 per frame, else a wrong partner). The biggest tagged draw of each id gives that id's
  `R_id = MVP_prev · MVP_cur⁻¹`. Moving draws (`dev > 0.5 px`, plausible, trusted pairing, origin ≥
  `mv_objects_ego_m`; r2, see below) are appended to a list that eye 0 reads back asynchronously (2-3 frames),
  together with their per-frame displacement; draws filtered by the r2 rules go to a second (reject) list.
- **Ids** (`ObjIds`, CPU, shared by both eyes): moving identities are grouped by rigid motion — join an id
  whose R is equal (Frobenius < 1e-3) or whose motion moves the identity's origin within 1 px of its own (a
  wheel joins its vehicle), else take a free id (`mv_objects_max`, ≤ 15). `mv_objects_hits` (2) consecutive moving
  readbacks with coherent motion to get an id (r2), 8 without to lose it (r6: 8 readbacks NOT DRAWN; see "r6"). Biggest meshes first, so vehicle bodies
  anchor the ids.
- **Pass B** reads the twin's stencil through an `X32_TYPELESS_G8X24_UINT` view (`CopyResource` of the depth
  carries the stencil plane): a world pixel whose id is valid this frame is reprojected with `R_id`, all
  others as before (camera / ego / cabin R). `Ctrl+F6`: id pixels magenta (orange = id without a usable R; r2: cyan =
  an identity rejected by the r2 rules in the last readbacks).
- **Safety.** A G-buffer state that cannot be twinned without changing the game's low nibble, or any state
  bound on the scene depth outside the G-buffer that tests the upper nibble (`read mask & 0xF0` with a real
  compare) or writes it, disables the feature for the session (`WARNING: MV objects DISABLED`). The capture
  has none.
- **Cost (expected, not yet measured in game).** CPU per G-buffer pass: 3 getters + 2 hash lookups per world
  `DrawIndexed` (~0.2-0.4 µs each → ~0.1-0.3 ms for 600 draws, VR ×2 passes) + 2 state calls per tagged
  draw; logged as `obj-ids ms/pass` on the FIFO stats line. GPU per eye: one ring copy (≤ 2 MB, a few µs),
  one 64-thread-group dispatch over the pass's draws, one 12 KB readback copy (eye 0), one extra stencil load
  per pixel in pass B (only while ids are valid); included in the `mv` stage of the DLAA GPU cost line.
- **Logs.** `MV objects: ready`, `MV objects: ACTIVE / inactive`, `MV objects: first stencil-tagged draw`,
  and every 600 blits `MV objects @blit N:` (ids in use, members, waiting, over-budget, recorded / tagged /
  missed draws per pass, CPU ms per pass, twins, torn passes, eye 0 valid ids per frame, paired %, moving
  entries per readback, unusable records, whether the game was seen using the upper nibble).
  - `missed` = world draws of the pass that are NOT recorded (no verdict, no R; they can still be tagged). r2 splits
    it: `no-cb` (no VS constant buffer at slot 0 or a window < 128 bytes: no MVP at rows 4..7), `small-cb` (slot 0 is
    a small, < 64 KB, per-object cbuffer instead of the per-draw ring), `2nd-ring` (slot 0 is another big buffer while
    the pass already mirrors one ring: the engine continued in a second ring buffer mid-pass, or uses two; only one
    ring is mirrored per pass), `full` (2048 draws recorded, or the window lies outside the ring). r3: `2nd-ring` is
    now `extra-ring` (a big buffer while all 3 ring slots of the pass are taken), see "r3" below for the ring fields.
  - `over-budget` = moving identities that passed the hysteresis but found no free id (all `mv_objects_max` ids
    used, and their motion matches no existing id); such an identity retries every readback, so the counter grows
    by (number of homeless movers) per readback. r2 prints the window's increase and the session total.
  - r2: `rejected/readback` per reason (`dup`, `pairing`, `ambiguous`, `own-truck`, `incoherent`), members `dropped`
    at once, `lists-full` (moving / reject list overflowed in a readback), and after the line an `MV objects ids`
    dump: per id in use (at most 8 lines) its member count, the biggest member's IndexCount, its geometry-key dup count
    (and the members' maximum), origin depth (m), speed (the anchor's per-frame displacement vs the camera-only
    prediction in clip units: depth in metres, lateral scaled by the projection's focal factor ~1.2-1.8), the speed
    window min..max since the last dump, readbacks held, jumps (readbacks whose displacement broke coherence) and
    re-anchors. A vehicle reads like "2 members, dup 1, 25 m, speed 0.44 (window 0.44..0.45), jumps 0".
- **r2: false movers in the first in-game test.** Flat ETS2, truck standing still: real traffic was magenta
  (correct), but also grass / vegetation clumps, a regular grid of windows on a block of flats, a row of trees and
  once a part of the own truck. Causes found in the code: (1) identical meshes drawn dozens of times -- only the
  FIRST 8 previous draws of a geometry key (draw order) were pairing candidates, so most copies were paired with a
  neighbouring copy 1-3 m away: plausible (< 6 m/frame), deviation ≫ 0.5 px, "moving", with a wrong R; and the
  identity (key + occurrence index) names a different copy whenever the engine re-orders them. (2) one deviating
  frame pair was enough (no consistency over time). (3) the own-truck exclusion used `mv_ego_origin_m`, which can be
  0 (off). Vertex-animated things (grass wind, flags) cannot cause it: the verdict only compares MVPs. A synthetic
  WARP test (40 identical clumps shuffled every frame, a part rigidly attached to the camera, a mesh jittering ±0.75 m)
  reproduces all three with the v0.9.0 code (all 15 ids taken) and none with r2, while two moving cars and a wheel
  keep their ids and correct motion vectors. The r2 rules:
  - **Duplicates** (`mv_objects_max_dup`, default 3): a geometry key drawn more often than this in the pass or the
    previous pass never gets a verdict or an R (reject reason `dup`).
  - **Trusted pairing**: the nearest previous candidate must BE the identity's own previous occurrence (same key, same
    occurrence index; reason `pairing`), and it must be clearly nearer than the second candidate (second ≥ 1.5 × best
    + 0.25 clip units; reason `ambiguous`). Only a trusted pair gives an id its R in pass B.
  - **Own truck** (`mv_objects_ego_m`, default 8 m, independent of `mv_ego_origin_m`): a draw whose origin view depth
    is below it is never an object (reason `own-truck`).
  - **Temporal coherence** (`mv_objects_hits`, default 2): an identity needs that many consecutive moving readbacks
    whose per-frame displacement agrees with the previous one (|Δd| ≤ 0.15 per frame of gap + 0.35 × |d|, readbacks at
    most 4 frames apart); a jump restarts the count (`incoherent`). A member whose displacement jumps, or that is
    rejected, in 2 readbacks in a row loses its id at once (`dropped`).
  - **Reuse frames** (bad camera record, last good R reused): no "moving" verdicts that frame (the camera R is a
    frame old, every static draw would deviate while the camera turns).
  - **Debug view**: while `Ctrl+F6` is on, stencil id 15 is reserved for identities rejected in the last 4 readbacks
    (drawn cyan; pass B gives them the camera R); the real ids are 1..14 meanwhile.
  - **Own trailer**: NOT excluded. A part moving with the camera (trailer in the chase view, a car driving alongside at
    the same speed) is exactly what needs its own motion vector (its true screen motion is ~0, the camera R says it
    moves), so "moves like the camera" is no reason to reject it. Only the distance rule above applies to the own
    truck; a trailer whose origin is beyond `mv_objects_ego_m` can get an id like any vehicle.
  - **Cost**: the per-pass sort by key hash moved from the commit to the pairing step (same work); per draw one
    occurrence index more in the record; GPU: the same one dispatch, a 18 KB readback instead of 12 KB.
- **r3: second constant-buffer ring, wheels of duplicate vehicles (round-2 in-game log).**
  - **What the "second ring" is (facts from the round-2 log + code).** `missed` was 100 % `2nd-ring` (0 `no-cb`,
    0 `small-cb`, 0 `full`): those draws' VS slot 0 is a buffer ≥ 64 KB that is NOT the first big buffer the pass
    used. It is never a whole pass (the first big buffer seen in a pass becomes its ring, so a pass drawn entirely
    from buffer B would show 0 missed): the switch happens INSIDE the G-buffer pass. The count varies with the scene
    (0 in light windows, 18-452 per pass in heavy ones; world draws per pass up to ~1400) and the r2 `torn` counter
    stayed 0 (no DISCARD of the first ring inside a pass). Consistent with the engine continuing in a further
    dynamic buffer once the 2 MB ring of the frame is full (the shadow / earlier passes use the same ring, so the
    fill point moves with the whole frame's load). Not yet proven: the ring size of buffer B, and whether rings
    rotate between frames -- r3 logs both (below).
  - **Identity does not depend on the ring.** Identity = geometry key (IB, VB0, offsets, counts, base vertex) +
    occurrence index in the pass; the pairing compares the gathered MVP VALUES of this and the previous pass. A
    vehicle whose draws come from ring A in one frame and ring B in the next pairs exactly as before.
  - **Recording up to 3 rings per pass** (`ObjRecord::kRings`). Each distinct big buffer of a pass gets a ring slot
    (one compare per draw for the common "same buffer as the previous draw" case, a loop over ≤ 3 slots otherwise,
    one `GetDesc` per new buffer -- no extra API call per draw, the `VSGetConstantBuffers1` result is reused). Each
    slot keeps its own used byte range and gets its own `CopySubresourceRegion` at the pass end into ONE mirror
    buffer, at a 256-byte aligned per-ring base: a draw's table entry is `base + the game's byte offset`, so the GPU
    side is unchanged (same shader gather, one dispatch, one SRV). The mirror only grows (64 KB steps; growing after
    a ring was already copied in the same pass carries the old contents over with one GPU copy).
  - **DISCARD inside the pass.** `hkMap` checks the pass's open ring slots: a `WRITE_DISCARD` of one of them flushes
    THAT ring (queued before the discard, so it copies the old contents) and SEALS its slot; the draws recorded so
    far keep their MVPs, later draws from the same buffer open a new slot. No pass is torn any more.
  - **Logs.** On `MV objects @blit`: `rings/pass` (and the max), `switches/pass` (ring changes between consecutive
    recorded draws: ~1 = "moved on to the next ring", many = rings used side by side), copied KB and draws per ring
    slot per pass, `ring0-vs-prev-pass same / other / new` (ring 0 of a pass is ring 0 / another ring / no ring of
    the previous pass: rotation between passes / frames), `sealed-in-pass` (r2: `torn`). The first 4 multi-ring
    passes are logged in full: per ring its size, used KB range, draws and first draw index -- ring 0 used up to its
    end at the switch confirms "the first ring was full".
  - **Duplicates default 6 (was 3; r4: back to 3, see "r4").** The grass / window-grid false movers of round 1 are filtered by the TRUSTED
    PAIRING rule, not by the cap: with a complete candidate set a static copy drawn in both passes pairs with itself
    (deviation ~0: never "moving"); a reordered copy's nearest candidate is itself under another occurrence index
    (`pairing`); a copy that appears / vanishes (culling) gives at most a stray readback. The cap only has to stay
    ≤ 8 (8 previous candidates per key are searched; an own occurrence beyond them is never found). 6 lets the 4-6
    identical wheel draws of one bus / truck be judged. Raising it exposed one r2 weakness, fixed in r3: the wait
    list kept an identity over ONE unseen readback, so one of 5 identical windows that was culled every 3rd frame
    (paired with its neighbour when it reappeared, the same constant "displacement" each time) collected 2 coherent
    hits. `mv_objects_hits` readbacks must now be strictly consecutive (as documented in r2).
  - **Followers (wheels of several vehicles of one model).** Duplicate / pairing / ambiguity rejects block NEW ids,
    not membership: a rejected draw whose hub the fresh R of an id (anchor seen in the same readback) moves to within
    1 px of one of its previous same-key draws -- the nearest candidate, or its own previous occurrence (the reject
    entries now carry the origin and both partner origins: 64 B instead of 16 B) -- joins that id as a FOLLOWER after
    `mv_objects_hits` consecutive follows. A follower is tagged (gets the vehicle's R in pass B), never anchors or
    represents the id (it has no trusted R of its own), is kept while it follows (a fresh R it does not follow counts
    as a bad readback, 2 in a row drop it), and goes with the id when no trusted member is left. A trusted member that
    becomes a duplicate (a second truck of the same model appears) stays the same way. Only with a COMPLETE candidate
    set (key drawn ≤ 8 times this and the previous pass: then a static copy always finds itself and is never a
    reject) -- in the synthetic test 40 shuffled grass clumps otherwise "followed" a passing car by coincidence.
  - **Proximity gate.** The loose 1-px join (r2) and the follower rule only join an id whose anchor origin is within
    15 clip units (x, y, w; ~20 m sideways, ~11 m up, 15 m deep in the game's projection): a wheel is next to its
    vehicle. Without it a slow truck 50 m away whose screen motion matched another vehicle's within 1 px joined that
    vehicle's id (synthetic test at low resolution).
  - **Cost.** Per draw: unchanged API calls, one extra compare (ring slot) and one byte stored. Per pass: one copy
    per ring (1-3). Readback 30 KB instead of 18 KB (reject list 256 × 64 B). VRAM: the 4 pass records each hold a
    mirror of the rings they used (2 MB per ring in the capture: ≤ 24 MB with 3 rings everywhere, 8 MB with one).
  - **Synthetic WARP test (r3).** 960 × 540, 18 frames, 5 ring layouts from a fresh state: one ring; wheels in ring B
    (a vehicle split across two rings); rings alternating per frame with an overflow into the other ring mid-pass; 4
    rings (the 4th not mirrored: counted as `extra-ring`); one ring DISCARDed mid-pass with the second half reusing
    the same byte offsets (seal). Scene: the r2 cases plus a bus with 6 identical wheel draws, two trucks of one model
    with 4 identical wheels each (8 draws: duplicates; tandem axles: ambiguous / wrong nearest candidate), 6 identical
    lamps shuffled with one culled at random, 5 identical windows whose first copy is culled every 3rd frame. All
    layouts: both cars, the bus and both trucks get their own ids, all 6 bus wheels and all 8 truck wheels share
    their vehicle's id (the truck wheels as followers), no clump / lamp / window / own-truck part / jittering mesh ever
    gets a real id, static and object pixels (incl. a follower wheel hub) get the expected MVs. Mutation checks: the
    ring base dropped from the table offset fails layouts 2-5; a discard without seal fails the discard layout.
- **r4: distant traffic ghosts and wobbles (round-3 in-game log `captures/dlaa_inject_ets2_v090r3_regression_flat.log`).**
  - **What the log shows.** The camera stood still the whole session (every `R_world` line is identity to 1e-4, translation
    terms <= 3e-4), so every "speed" in the ids dump is real object motion, not a pairing artefact: 0.32 m/frame x 72 fps
    = 83 km/h (AI trucks), 0.51 = 132 km/h (cars). Identical speeds across ids = vehicles at the same speed limit. What
    is NOT normal is the id budget: `over-budget` +261..+7683 per 600 blits (32572 in total; round 2: 0 in every window),
    `ids=15/15`, `moving/readback` 55-123 with `lists-full` in up to 512 of 600 readbacks (the 128-entry moving list
    overflowed: random members "unseen", random waiting identities restarted), `dropped` 26-254 per window. The ids dump
    explains it: PAIRS and TRIPLES of ids with the SAME IndexCount, dup 2-3, the same depth within 0.4-2.6 m and the same
    speed (`id 4: 792 dup 2, 230.3 m, 0.51` + `id 5: 792 dup 2, 230.7 m, 0.51`; `2670 dup 2` at 274.6 / 277.2 m;
    `360 dup 2` at 283.9 / 281.6 m; three `1560 dup 3` ids at 172.9-175.5 m; `2457` + `2247` at 80.7 / 76.9 m) = the
    WHEELS of one vehicle, each anchoring an id of its own. A wheel's R carries its spin (rotation about its own axle), so
    no other wheel / body matches it (Frob) and the 1-px hub test against another axle fails: one vehicle used 2-4 ids,
    15 ids covered 4-5 vehicles, everything else fell back to the camera R (ghosting), and ids changed hands all the
    time (wobble: a vehicle's motion vectors switching between its own and the camera's). Round 2 had the same pattern
    on a small scale (`1560 dup 3` x 3 at 46-48 m) but fewer movers were judged: `max_dup` 3 and the second ring not
    recorded. A wheel can also be its id's representative (biggest tagged draw: at distant LODs a wheel mesh has more
    indices than the body), and a body whose origin lies on a wheel's axle line passes the 1-px test against that wheel's
    R -- then the body's pixels get the spinning R.
  - **Ruled out.** (1) The ring mirror: base 0 / 2 MB for the game's 2 x 2 MB rings, ring 0 = the TAIL of its buffer,
    ring 1 from 0, buffers rotating every frame -- the synthetic test now has exactly this layout (S5): pass.
    (2) Constant-displacement fake movers from two copies of a mesh whose draw order flips: the nearest previous
    candidate of a STATIC copy is always itself (distance ~0), so a flip is a `pairing` reject, never a "moving" verdict
    (test: two copies at 250 m flipping every frame never get an id, also with the r3 code). (3) Id reuse vs stale
    stencil: the stencil of a pass is written with the id table of that pass and pass B uses the R computed from THAT
    pass's own tagged draws, so a reused id cannot meet a stale R (test S6: 0 wrong pixels also with r3's 1-frame reuse).
  - **Rules (r4).**
    - **Spin** (GPU, per draw): how far the draw's local axes turned in the frame beyond the camera's motion,
      `|Mp col_a - R_cam Mc col_a| / |col_a|` (~radians / frame). A vehicle yaws < 0.03, a wheel above ~10 km/h > 0.1;
      limit `kSpinMax` 0.12. A spinning identity never takes a free id, never anchors / re-anchors one and never
      represents its id; it only JOINS an id whose anchor refreshed in the same readback when that motion moves its hub
      to within 1 px (the wheel test of r2). Unjoined it waits (`spin-waits`). An id left with spinning members only is
      freed.
    - **Per-frame representative** (GPU, `CSResolve`): the biggest tagged draw of the id with a trusted, plausible,
      non-spinning pairing THIS frame (r2 / r3: the CPU's biggest tagged draw -- when its pairing failed that frame the
      whole id used the camera R). Table.z bits 8..11 = every draw's id, bit 13 = may not represent (follower /
      spinner). No such draw: the id keeps the camera R for the frame (`ids-without-trusted-member`).
    - **Same-frame veto** (GPU, `CSVeto`): every tagged draw of an id must lie where the id's motion says it was:
      `R_id * origin` within max(1.5 px, 35 % of the id's correction at that point) of one of its key's previous draws.
      Else the id takes the camera R for that frame (debug view: orange). Catches the identity swap of two identical
      vehicles (the ids are 2-3 frames old: after a draw-order swap, one vehicle's body carries the other's id until the
      CPU drops it) -- synthetic test: two identical bodies driving in opposite directions, swapped every 8 frames: r3
      gives 16 wrong-direction pixels, r4 0; with the veto disabled r4 gives the same 16.
    - **Membership re-check** (CPU): a member seen moving while its anchor refreshed must still move like its id (the
      anchor's R moves its origin to within 1.5 px of its own); 2 failures in a row and it leaves the id (it can
      qualify on its own). Before r4 a member that once moved alike (two cars side by side) kept the id's R forever.
    - **Quarantine**: a freed id is not handed out for 3 frames (defensive; `quarantined`).
    - **Defaults**: `mv_objects_max_dup` 3 again; `mv_objects_followers` 0 (new key; 1 = the r3 follower rule, now only
      to "solid" ids whose anchor refreshed coherently in 4 consecutive readbacks).
    - **Moving list**: 256 entries per readback (r3: 128), 128 bytes each (+ spin).
  - **Logs (r4).** `MV objects @blit` gains `r4: vetoed draws/readback`, `ids-without-trusted-member/readback`,
    `spinners`, `spin-joined`, `spin-waits/readback`, `diverged`, `quarantined`, `solid-blocked`; the ids dump shows
    `(n follower(s), m spinning)` and `solid` per id; the config line `mv_objects_followers`.
  - **What the next log should show.** `over-budget` back near 0 in most windows; no pairs / triples of ids with the same
    IndexCount at the same depth (wheels now appear as `spinning` members of their vehicle's id, or not at all);
    `lists-full` 0; `vetoed draws/readback` small (it counts swap events; a large steady value would mean the 35 %
    tolerance is too tight for some vehicle, e.g. articulated trucks -- then raise it); `ids-without-trusted-member`
    small; ids `held` for long runs with `jumps 0`.
  - **Cost.** GPU: two more tiny dispatches per eye (16 threads; one thread per recorded draw), an 8 KB scratch buffer;
    readback 48 KB per eye-0 frame instead of 30 KB. CPU: one more pass over the moving entries (membership re-check).
  - **Synthetic WARP test (r4)** (scratchpad `objtest/obj_mv_test_r4.cpp`, 960 x 540): r3 scene + a distant LOD car
    whose 4 wheel draws (2 keys x 2) have more indices than its body and spin 1.4 rad / frame, body origin ON the rear
    axle; two static copies at 250 m flipping order every frame; two identical bodies (+ unique trailers) driving in
    opposite directions, draw order swapped every 8 frames. Layouts S0-S4 (r3) + S5 = the game's ring layout (2 MB
    DYNAMIC pool of 4 rotating per frame, ring 0 = buffer tail with "shadow" constants before it, ring 1 from 0 with the
    game appending NO_OVERWRITE after the pass). S6 = id churn: 16 movers for 15 ids, one vanishes, one turns erratic
    (dropped while still drawn): every tagged pixel keeps its own motion, no id re-given within 2 frames. Config B
    (max_dup 6 + followers) on S0 / S5: bus wheels (dup 6) join as spinners, truck wheels (dup 8) follow. Result:
    3975 / 3975 checks. The same test on the r3 sources fails the LOD car (wheels anchored 44 frame-ids, 2 ids for one
    car), the swap (16 wrong-direction pixels) and the churn quarantine (an id re-given after 1 frame).
- **r6: sticky tags (round-4 in-game log `captures/dlaa_inject_ets2_v090r4_flat.log`, flat ETS2).** Report: "I still
  see wobbly, smudgy cars in the distance -- almost when the pink stops" (pink = the `Ctrl+F6` magenta tag).
  - **What the log shows.** The r3 shortage is gone (`over-budget=+0` in 38 of 40 windows, +203 / +31 in the other two, `lists-full=0/0`), but the id
    LIFECYCLE churns: `waiting` 42-50, members `dropped` 44..2133 per 600-blit window while `spin-joined` is +1382..+1526 in
    the same windows (dropped and joined again), `diverged` up to +903, `vetoed draws/readback` up to 2.36 (each veto puts a
    whole vehicle on the camera R for a frame), `spin-waits/readback` 30-45, ids with `jumps` 12-27 while their speed window
    is flat (`id 7: ... speed 0.33 (window 0.33..0.33) ... jumps 27`). Every tag loss / gain flips a car's pixels between
    its own R and the camera R; DLSS's history then no longer lines up (smudge), and the next flip back is the wobble.
  - **Causes in the code (r4).** (1) A member counted as SEEN only when it was in the moving list (> 0.5 px vs the camera
    R); otherwise `misses` grew and it was released after 8 readbacks. A car at 250 m driving towards / away from the
    camera, slowing down or merely crossing at a shallow angle sits around 0.5 px: tag off after 8 readbacks, on again
    when it crosses 0.5 px (exactly "when the pink stops"). (2) A member's REJECT sightings (pairing / ambiguous / dup:
    no trusted pair of its own that frame -- reordered identical wheels, a second car of the same model) counted as bad
    readbacks: 2 in a row dropped it, and the next coherent sightings joined it again. (3) Coherence tolerance 0.15 per
    frame + 0.35 |d| in clip units: at 250 m one pixel is ~0.2-0.4 clip units, so ~1 px of displacement noise was a
    "jump". (4) The membership re-check (1.5 px) and the veto (1.5 px / 35 %) fired on small errors; a veto drops the
    whole vehicle to the camera R for that frame. (5) `spin-waits`: a spinning wheel could only join an id whose ANCHOR
    refreshed in the same readback, near the anchor's origin. A far body under 0.5 px never refreshes while its wheels
    (their ±2 m probe points spin) are "moving", and a semi-trailer's wheels are > 15 clip units from a tractor anchor
    seen side-on; the r3 strict-consecutive hysteresis added to `waiting`.
  - **Rules (r6).**
    - **Seen = drawn.** `Update` gets every identity of the read-back pass that was drawn with previous draws of its key
      (paired, trusted or not, moving or not). A member is released only when it was not drawn for 8 readbacks, or when it
      is PROVEN to belong elsewhere. Sub-threshold sightings are counted (`still-seen/readback`).
    - **Proof, not divergence.** A member seen moving is judged against its id's reference R of THE SAME readback: it fits
      within max(1.5 px, 35 % of its own correction); it is a misfit only beyond max(4 px, 50 %) (the veto threshold);
      in between nothing happens. A misfit that fits another id clearly better (own misfit >= 2 x the other fit + 1 px)
      in 2 judged readbacks in a row MOVES there (tag A -> B, no camera-R frames); a non-spinning misfit that fits no id
      takes a free id with its own motion; with no free id it stays unless the id's R is worse than the camera R. Rejected
      sightings of a member only count as seen; with a fresh reference R their hub is judged like the r3 follower test,
      and only "fits another id" moves them (identity swap of two identical vehicles). Own-truck-distance rejects: seen,
      no verdict (a vehicle passing closer than `mv_objects_ego_m` keeps its id now).
    - **Veto records.** `CSVeto` appends each vetoed tagged draw to the reject list (reason 6). A vetoed member that is not
      moving itself (a car that stopped next to the car whose id it had joined: it is never judged by its own motion again)
      leaves to the camera R after 2 such readbacks (`left-to-camera`; before the fix the veto would flip the moving car to
      the camera R every frame from then on -- synthetic case D below: 69 frames instead of 3).
    - **Reference R.** Refreshed from the anchor when it is seen moving coherently, else from the biggest member that is
      (it becomes the anchor). The GPU's per-frame representative now PREFERS the anchor (table bit 14), so the CPU judges
      members against the same draw's R that pass B uses.
    - **Ids are sticky.** An id lives while it has members. One left with spinning / follower members only is kept for 8
      readbacks (a new LOD of its body joins through the wheels: a non-spinning mover whose motion puts one of the id's
      spinning hubs within 1 px), then freed. Quarantine 3 frames as before. An id whose vehicle stops keeps it: its R then
      equals the camera R, so nothing switches.
    - **Coherence** tolerance max(0.15 per frame of gap + 0.35 |d|, 2 px of footprint at the origin's depth; footprint =
      2 w / min(W, H) clip units per px). Only 2 REAL jumps in a row drop a member.
    - **Hysteresis** 2 coherent readbacks, one readback without a sighting forgiven (r2 behaviour) -- but only when the
      identity was NOT DRAWN in it, or was drawn sub-threshold while its correction is < 2 px (a far car hovering around
      0.5 px). Without that guard the r2 hole is open: a copy among identical meshes whose neighbour is culled every 2nd
      frame pairs TRUSTED with the neighbour's previous draw (same occurrence index, unambiguous: the trusted-pairing rule
      does NOT block it) and shows a constant fake displacement every other readback; the synthetic test (case C) gives it
      an id in 194 of 200 frames without the guard, 0 with it.
    - **Spinning parts** join with a reference R up to 2 readbacks old (1 px fresh, 1.5 px older) near ANY member's last
      origin (r4: the anchor's, same readback only).
    - **Veto** only on gross errors: > max(4 px, 50 % of the id's correction) (r4: 1.5 px / 35 %).
    - **Budget** 512 members (r4: 256; round 3 / 4 reached 256).
  - **Telling DLSS about a switch -- not done.** The NGX D3D11 evaluate parameters do have one per-pixel input that could
    soften a switch, `pInBiasCurrentColorMask` (bias towards the current frame), plus the whole-frame `InReset`. Using the
    mask would need a per-pixel "tag changed since last frame" image (the previous pass's stencil) and trades history for
    aliasing / shimmer on exactly those pixels; r6 removes the switches instead. Revisit only if the next log still shows
    `tag-switches/readback` well above the vehicles entering / leaving the view.
  - **Logs (r6).** `MV objects @blit` gains `r6: tag-switches/readback` -- identities drawn in two consecutive read-back
    passes whose stencil tag went 0 -> id (`on`) or id -> 0 (`off`), per compared readback; `id-changes` = id A -> B
    (object R both times) -- THE number to watch (target: near 0; acquisitions of vehicles entering the view show up as
    `on`), `readbacks`, `still-seen/readback`, `moved`, `own-id`, `left-to-camera`, `released`, and `id ages (readbacks)`:
    min / median / max of the ids in use, ids freed in the window, how many of them within 72 readbacks, their mean age.
    The ids dump shows each id's `reference N readback(s) old`. The config line reads `(v0.9.0 r6 sticky tags)`.
  - **What the next log should show.** `tag-switches/readback` near 0 apart from traffic entering / leaving the view
    (r4-style churn would be several per readback); `off` well below `on`; `dropped` and `spin-joined` small and no longer
    equal; `vetoed draws/readback` near 0 (a steady value: articulated vehicles or identity swaps -- check `moved`);
    `spin-waits/readback` lower (wheels of sub-threshold bodies may still wait: such a vehicle has no id of its own);
    id ages in the hundreds or thousands of readbacks, `short-lived` freed ids rare; `over-budget` 0 (sticky ids hold
    longer -- if it climbs, 15 ids are too few for the traffic and the farthest vehicles fall back to the camera R).
  - **Cost.** CPU per readback: one hash lookup per recorded draw (`drawn` list, <= 2048) and one per wait entry, two
    small hash tables for the tag-switch metric (~2 x 2048 inserts / lookups); per-member proximity scans only for
    gross misfits and joins. GPU: the veto writes up to 64 B per vetoed draw. Readback size unchanged.
  - **Synthetic WARP test (r6)** (scratchpad `objtest/obj_mv_test_r6.cpp`, 960 x 540): the r4 scenes S0-S6 + configuration
    B, with tag switches counted per object after its first tag, and S7 "far traffic", 200 frames: A = car at 250 m,
    0.3 m / frame sideways (0.39 px / frame here, ~2 px in a 2880-wide render) with ±0.3 px of position jitter per frame,
    body + cabin + 4 spinning wheels; B = car at 150 m decelerating to a stop (frames 60-120) and standing; C = 3
    identical windows whose first copy is culled every 2nd frame; D = two cars side by side at 60 m, D2 stops dead at
    frame 90. r6: A tagged at frame 6 (all 6 members), B at frame 4, then 0 tag switches and 0 object-R losses for both
    over the 200 frames; C never tagged; D2 out of D1's id 3 frames after it stopped, D1 back on its object R right after.
    The same test on the r5 sources: A 140 tag switches and 8 object-R losses, B loses its id when it stops, D1 9 frames
    without its object R. Mutations of r6 (each fails S7): no `drawn` list (B loses its id), no hysteresis gap guard (C
    gets an id), no coherence footprint floor (A: 176 switches), no veto records (D1: 69 frames on the camera R).
    S0-S5 K bodies now drive 0.8 m / frame from ±30 m (r4: 0.4 from ±14): the 4 px veto floor at 960 x 540 is ~12 px in
    the game's render, and the identity swap must still be a gross error there (0 wrong pixels; the swapped bodies
    change ids, 5 per body in 26 frames). Result: 4860 / 4860 checks.
- **r7: sticky object MVs (round-5 in-game log `captures/dlaa_inject_ets2_v090r6b_flat_driving.log` + video, flat ETS2,
  driving, `Ctrl+F6` view on most of the time).** Report: "the pink flickers often, lots of pop in and out of pink, it
  doesn't stay attached to the cars".
  - **What the log + video show.** Frame-by-frame analysis of the video (3600 frames, the horizon band): ~2000 tag gaps
    of 2-8 frames and 344 pink -> orange -> pink blinks in 60 s; 2993 of 3600 frames had an orange patch somewhere. A
    zoomed 16-frame sequence of a truck passing close by (t = 52.15 s): its trailer BOX cyan (rejected, camera R) in every
    frame, the tractor magenta, the nearest wheel orange (frames 0-3) -> magenta (4-7) -> cyan (8, 14, 15) -- the tag
    is per part and per frame, not per vehicle. The video minute (22:13:15..22:14:13, 600 blits per window):
    `r6: tag-switches/readback` 0.12..0.57 (on 0.06..0.37, off 0.05..0.24) = 4..27 tag losses per second at 72 fps;
    `r4: vetoed draws/readback` 0.08..1.41 and `ids-without-trusted-member/readback` 0.0..1.15 (each = a whole id on the
    camera R for that frame = orange); `r2 rejected/readback: own-truck` 8..37 while driving (0..3 standing in the
    earlier log), `pairing` 7..15, `dup` 2..22; `dropped` 0..73, `released` 32..237, `left-to-camera` 2..130 per window;
    ids with `jumps` 173 / 435 / 905; `over-budget` 0 in every window (the ids are not short).
  - **Causes in the code (r6).** (1) The own-truck rule was distance only: `CSMain` called every trusted "moving" draw
    whose ORIGIN is nearer than `mv_objects_ego_m` (8 m view depth) the own truck (reason 4). A vehicle passing alongside
    has its origin's view depth near 0..8 m: a part that was not a member yet could neither take an id nor join one (CPU
    step 5: `kRejEgo` = "seen, no verdict") -- cyan / camera R while passing. (A member that came within 8 m kept its id
    and could still represent it: sticky membership + `CSResolve` only needs a trusted pairing.) The own truck's exterior
    is what the rule is for: its draws are CAMERA-LOCKED (`Mp * x == Mc * x`), a passing trailer crosses the screen by tens
    of px per frame. (2) No representative / a veto = the camera R for the whole id for that frame: `CSResolve` state -2
    (no trusted member: a second instance of the mesh appeared -> dup / pairing / ambiguous) and `CSVeto` state -3 flip
    the vehicle between its own R and the camera R -- the pink / orange blink and the DLSS smudge. A 1..3-frame-old R of
    the same id is a far better fallback (off by the motion's change over a frame, sub-pixel) than the camera R (off by
    the vehicle's whole relative motion). (3) A part that lost its tag (drop, LOD switch = a new identity) needed
    `mv_objects_hits` (2) coherent readbacks again even when its R matched its vehicle's id exactly.
  - **Rules (r7).**
    - **Own truck = near AND camera-locked** (GPU, `CSMain`): over the 5 probe points, `lockPx` = max screen distance between
      `Mp * x` and `Mc * x` (the point's own motion on screen in the frame, no camera term). A draw is the own truck only
      when its origin is nearer than `mv_objects_ego_m` AND `lockPx <= mv_objects_lock_px` (new key, default 3 px of the
      full render, 0.5..20). A SPINNING draw (r4 spin measure) is judged at its origin only -- its probe points turn with
      it, so the own truck's wheels would never look locked; their hubs are. A near draw that is not locked is a normal
      candidate (moving list, can take / join / anchor an id; a spinning one only joins). Counted per readback:
      `near-locked` (= reason 4, own truck) and `near-accepted`. Same rule per eye in VR.
    - **Hold** (GPU, `CSResolve` + new 4th dispatch `CSHold`): `SlotR` is never cleared between frames, so `CSResolve`
      first copies each id's rows / state as they were at the start of the frame to `SlotR[80 + s*4 ..]` / `[144 + s]`
      (the table grew from 80 to 160 float4). An id in the valid mask without a usable R of its own this frame (no
      trusted representative, partner missing, R not finite) whose previous state was usable (state 0, held or not)
      with a hold count < `mv_objects_hold` (new key, default 3, 0..8; 0 = r6 behaviour) KEEPS its previous rows: state 0,
      value = hold count + 1 (`SlotR[64 + s].z`; a fresh representative resets it to 0). Pass B and the debug view
      (`SlotR[64 + s].x > 0.5`) are unchanged: held pixels stay magenta. An id not in the mask gets state -1 (count
      cleared). `CameraMv` passes `mv_objects_hold` only when the previous `Generate` ran the object dispatch
      (`m_objHoldOk`; cleared by `ObjForget`: `Invalidate`, resets, a frame without it; the table is created zeroed), so a
      hold never uses an R from before a gap.
    - **Veto -> hold, unless the camera fits better.** `CSVeto` no longer writes `SlotR`: each contradicting draw ORs its
      verdict into the readback tail (bit 0 vetoed, bit 1 = the camera R puts this draw nearer to a previous draw of its
      key than the id's R), plus the worst contradiction (px) and a draw index; `CSHold` (16 threads) applies it once per
      id: a fresh R vetoed -> the same hold (last frame's rows, count + 1); an id already held by `CSResolve` stays held;
      else state -3 + camera R as in r4..r6. The bit-1 guard keeps r4's reason for the veto: in an identity swap of two
      identical vehicles driving in OPPOSITE directions the id's R is the other vehicle's motion (worse than the camera
      R), and a stopped car inside a moving car's id is exactly static (the camera R is right for it) -- both still go to
      the camera R. This is stricter than the plain "veto -> hold" the round-5 analysis asked for: a veto hold keeps an R
      that the veto has just proven wrong for one draw, so it only applies where that R is still the better of the two.
      Side effect: all draws of an id test against the same R now (r4..r6: the first veto's write made the later threads
      of that id skip, so `vetoed draws` and the veto records depended on thread order).
    - **Fast join** (CPU, `ObjIds::Update` step 4): a waiting mover on its first sighting (hits >= 1) that is not spinning,
      whose R matches an existing id within `kTightFrob` AND that lies near one of the id's members (`NearMember`), joins
      that id at once; a spinning part joins at once when the id's FRESH reference R (this readback) moves its hub within
      1 px, near a member. Taking a NEW id still needs `mv_objects_hits` coherent readbacks (what keeps mis-paired static
      meshes out). Counted: `fast-joined`. **r9 reverted the non-spinning half** (round 7: a static draw that is a candidate
      for one frame has R = the camera R; once a static draw held an id, every such draw joined it on sight -- the road
      and trees went magenta, one id had 282 members at 659 m); the spinning-part fast join stays, see "r9".
    - **Origins behind the camera.** Near movers bring a new case to the CPU: a passing trailer's origin behind the camera
      (w < 0) while its front is on screen. `OriginPx` / `PrevPx` cannot place such an origin on screen (1e30); the
      membership re-checks (steps 3 and 5) now give no verdict there. Before the fix the synthetic S8b moved a rigid
      trailer out of its tractor's id 2 readbacks after it joined (1e30 read as a gross misfit).
  - **Logs (r7).** The config line reads `(v0.9.0 r7 sticky object MVs)` and gains `mv_objects_lock_px` / `mv_objects_hold`;
    the "MV objects: ready" line gains an `r7:` part; `MV objects @blit` gains `| r7: held/readback=%.2f
    hold-expired/readback=%.2f near-accepted/readback=%.2f near-locked/readback=%.2f fast-joined=+%llu` (held = ids that
    kept their last R in a frame; hold-expired = ids that wanted a hold after a usable frame but had used up
    `mv_objects_hold`; both from eye 0's readback tail, a 208-byte block after the reject list).
  - **What the next log should show.** `near-locked/readback` ~ the own truck's exterior draws (~25 while driving: the
    `own-truck` rejects of round 5 split into this and `near-accepted`); `near-accepted` > 0 only with traffic close by
    (a steady high value while alone on the road = the own truck's exterior sways more than `mv_objects_lock_px` at the
    render resolution -- then raise it); `held/readback` about the round-5 `ids-without-trusted-member` plus part of
    `vetoed`, with `hold-expired` far below it; `tag-switches/readback` lower, `off` well below `on`; `fast-joined` > 0.
    In the `Ctrl+F6` view a passing truck stays magenta (box, tractor and wheels), orange blinks become rare.
  - **Cost.** GPU: one more 16-thread dispatch per eye; the id table 2.5 KB (was 1.25 KB); the readback 208 bytes larger.
    CPU: the fast-join scan (members of nearby ids) only for waiting movers.
  - **Synthetic WARP test (r7)** (scratchpad `objtest/obj_mv_test_r7.cpp` + `r7_scenes.inc`, 960 x 540): S0-S7 + config B
    as in r6, and (200 frames each, moving camera):
    **S8** passing truck (box + rigid tractor + spinning tractor wheel; box origin 30 m -> 2 m -> -2 m, up to ~15 px /
    frame; the own truck = 3 draws exactly locked to the camera at 1.5..3 m + a spinning wheel whose hub is locked):
    box / tractor tagged in every frame after acquisition, 0 camera-R frames, 0 tag switches, the own truck never tagged
    (~4 near-locked draws per frame = the own truck, 66 near-accepted draws = the truck within 8 m). S8 as specified does
    NOT separate r6 from r7 -- the parts are members before they come within 8 m, and a member kept its id there in r6 --
    so **S8b**: an overtaking truck that APPEARS within 8 m (box origin -8 m -> +24 m): r7 tags the tractor at frame 4
    and the box at frame 42 (its first probe point in front of the camera at frame 40), 0 tag switches; with the r6
    own-truck rule (mutant `locked = near`) frames 48 / 102: fails.
    **S9** pairing hiccup (car body at 60 m, a second instance of its mesh 0.1 m away every 7th frame -> the next frame's
    pairing is ambiguous): 0 camera-R frames with hold 3 (28 held frames), 28 with hold 0.
    **S10** LOD switch (car at 80 m, body + rigid cabin; the body changes mesh every 30 frames = a new identity): 19
    untagged body frames over 6 switches (3 per switch = the readback pipeline); without the fast join (mutant) 24: fails.
    Veto guard (mutant: every veto holds, the literal round-5 proposal): the r4 identity swap (S0-S5, two identical bodies
    in opposite directions) gets 16 wrong-direction pixels again (r3's result, as if there were no veto) and S7's stopped
    car D2 gets the moving car's motion for 3 frames; with the guard 0 / 0 as in r6.
    Result: 6863 / 6863 checks. The r2 own-truck part of S0-S5 sways 2 cm at 15 Hz (3 px per frame at its origin, ~16 px
    at its nearest probe point): under r7 that is not camera-locked and it gets an id (`OWN_SWAY=0.02` build: S0 fails
    exactly those 2 own-truck checks); the default build uses 0.5 mm (< 0.4 px).
- **r8: ego set + mask hold (round-6 in-game log `captures/dlaa_inject_ets2_v090r7_flat_driving.log` + video, flat ETS2,
  2026-10-07 17:04-17:07, `Ctrl+F6` view, video analysed frame by frame).**
  - **What improved (r7).** `own-truck` rejects while driving fell from 8..37 to 0..15 per readback; passing vehicles
    keep their tag better.
  - **What the log + video show.** (1) Truck STANDING STILL (ids dump 17:06:52..17:07:33, R_world = identity): ids 3, 4,
    6, 8 (and 9, 10) live the whole time with `depth 1.3 m / 1.3 m / 1.1 m / 0.0 m / 8.0 m`, `speed 0.00`, `dup 1`,
    IndexCount 3480 / 6618 / 864 / 264 / 168, 1..44 members -- the own truck's exterior parts (they all share the truck
    root as local origin, 1.1..1.3 m from the interior camera). `near-locked/readback` 0.00 in every window from
    17:07:00 on, `near-accepted` 0.36..20.3; `ids-without-trusted-member/readback` 3.00..3.02 constant for 5 windows
    (r6: 0..1.15): 4 of 15 ids wasted, orange on the own truck in the `Ctrl+F6` view. (2) Video (debug view, standing,
    38 s): 5 times ALL tagged pixels vanish for exactly one frame (t = 141.62, 151.43, 151.68, 166.75, 166.92 s, pairs
    0.17..0.25 s apart; no orange, no cyan, the frame otherwise identical to its neighbours). (3) Driving (17:06:43,
    17:06:52): `fast-joined` +1176 / +1217 per 600-blit window with `released` 977 / 1258 in the same windows.
  - **Causes.** (1) The r7 own-truck test is a ONE-FRAME test: max screen motion over the 5 probe points <= 3 px. The
    engine's idle shake moves the parts > 0.5 px against R_world (candidates) and swings the probe points at +-2 m by
    more than 3 px at 1.3 m depth: trusted movers -> ids. While driving their pairing fails (symmetric left / right
    copies, a large parallax shift of the camera prediction: `pairing` 7..17 / readback), so they are never
    camera-rejected or judged again -- ids WITHOUT a trusted representative that the r7 hold cannot help (they never had
    a usable R). (2) Not found in the log. The hypothesis that a dropped / extra scene pass became the pairing record is
    CONTRADICTED by the FIFO numbers: `passes - blits` = 60 and `fifo overflow` = 58 in every window from blit 600 to
    9000 (all of them before the first blit, the loading screen) -- no extra pass while playing. Eye 0 `frames` =
    `readbacks` = 600 per 600 blits, `unusable` 0, `paired` 100 % in the standing windows, `MV: frame` world-miss 21 /
    bad 0 / stale-reuse 0 unchanged, all DLAA blits used the depth snapshot: every blit ran the object path with a
    usable record. What the r7 log cannot show (no per-frame data): a frame whose mask went to 0 because its tagged
    draws found no previous draw of their key (the pairing record was not this eye's last frame, or none was committed),
    or a pass whose draws carried no stencil id (id lookups at draw time returned 0). A mask hold covers the first, not
    the second -- hence the diagnostic below.
  - **Rules (r8).**
    - **Ego set** (CPU, `ObjIds::Update` step E, before every other step). The own truck is what stays PUT on screen
      over time, not in one frame. Every near candidate reaches the CPU already (moving entries and reject entries both
      carry `c0` = the origin in current clip space and the view depth); a table (256 entries, open addressing) keeps
      per near identity (origin < `mv_objects_ego_m`, `c0.w` > 0.05) the box of its origin's screen positions (px of the
      full render), the readbacks it was seen in and a flag. While the box stays within 2 x `mv_objects_ego_px` per axis
      (every sighting within `mv_objects_ego_px` of the box centre: new key, default 12, 2..64) the count grows; at
      `mv_objects_ego_rb` sightings (new key, default 24, 4..200) the identity is FLAGGED: its reference becomes a slow
      running mean (1/32 per sighting: the sway oscillates around it, an acceleration nose-lift shifts it slowly), and
      two sightings in a row farther than `mv_objects_ego_px` from it drop the flag (a car that stood next to you drives
      off -- one jolt does not). A sighting outside the box restarts it; a sighting with no screen position (behind / at
      the camera) or no longer near removes the entry (never ego, never counted); unflagged entries not seen for 2 x
      `mv_objects_ego_rb` readbacks and flagged ones not seen for 3600 go. A flagged identity: its members are released
      (`ego-released`; an id left without members is freed in step 8), waiting / follower candidates are dropped, steps
      4 / 5 skip it, and `ObjPrepare` sets **Table.z bit 15** for its draws: `CSMain` never makes it a mover (a trusted
      candidate is a reason-4 reject, so the CPU keeps seeing it and can drop the flag), never lets it represent its id,
      and the CPU leaves it out of the mask and the anchor bit.
    - **Lock pre-filter at the origin** (GPU, `CSMain`). The r7 lock test stays as a cheap first check but at the ORIGIN
      only, for every draw (r7: max over the probe points, origin only for spinning draws): `locked = near && lock0 <=
      mv_objects_lock_px`, default **6** (was 3); an origin that could not be measured (behind the camera) is NOT
      locked. A vehicle passing within 8 m crosses >= 10 px / frame in the game's render at any realistic relative
      speed; the cabin's sway at the origin is a few px.
    - **Mask hold** (CPU `ObjPrepare` + `CSResolve`). A PAIRING FAILURE frame -- the record is unusable (Count 0, ring
      copy pending or failed, no mirror) or it pairs fewer than half the draws of the last GOOD (not held) frame --
      keeps the last good frame's valid-id mask: ids missing from this frame's own mask are added to `Ids.x` (and to the
      mask pass B and the debug view use). They have no representative this frame, so `CSResolve` gives them exactly the
      r7 hold (last frame's rows, hold count + 1, state 0 = magenta), within the same `mv_objects_hold` budget; at most
      `mv_objects_hold` mask-held frames in a row, and a frame that is not held is the new baseline. An unusable record
      has nothing to gather: that frame runs `CSResolve` alone (Counts.x = 0, scratch cleared = every key 0), reads
      nothing back and commits nothing, and the pairing record is cleared (it would be two frames old: an R paired
      against it is two frames of motion), so the next frame pairs nothing and is held the same way. A frame with a good
      record behaves exactly as r7.
    - **Pairing record = the last consumed pass** (already so in r7, verified): the pairing record (`m_objPrevKh` /
      `m_objPrevOcc`, the gathered MVPs) is committed only by `ObjFinish` of the `Generate` that consumed the pass, i.e.
      a pass this eye's blit consumed; a pass the FIFO drops never reaches `CameraMv`.
    - **Mask-dropout diagnostic** (`CameraMv::ObjDiag`): whenever the previous `Generate` had a non-zero mask and this
      one gets 0, does not run the object path, pairs fewer than half of its draws, or holds -- rate-limited (the first
      10, then one per 600).
    - **fast-joined-then-released** (diagnostic, no behaviour change): a member that joined through the r7 fast join and
      was released without being drawn again in a later readback (identity churn: occurrence-index shifts, LOD swaps).
  - **Logs (r8).** Config line `per-object motion vectors (v0.9.0 r8 ego set + mask hold): ... mv_objects_lock_px=%.1f
    mv_objects_hold=%d mv_objects_ego_px=%.1f mv_objects_ego_rb=%d`; the "MV objects: ready" line gains an `r8:` part;
    `MV objects @blit` gains `| r8: ego-set=%d ego-flagged=+%llu ego-dropped=+%llu ego-released=+%llu
    mask-held/readback=%.2f fast-joined-then-released=+%llu` (ego-set = identities flagged now; mask-held = frames whose
    mask was held, eye 0, per readback); the ids dump gains `  ego set (own truck, r8): %d flagged of %d near identities
    tracked | flagged sightings deviated up to %.1f px from their reference (limit %.0f px, mv_objects_ego_px) | best
    unflagged run %d of %d readbacks (mv_objects_ego_rb)`; new line `MV objects: mask dropout #%llu at frame %llu --
    <path> | mask 0x%04x -> 0x%04x | record usable|UNUSABLE|none: %d draws, %d paired (%.0f%%; last good frame %d),
    tagged %u (last committed record %u), previous record %d draws | ids in use %d, members %d, ego set %d | this blit
    consumed pass s=%llu (newest s=%llu, %d unconsumed after it, fifo overflows %llu), pairing record from pass s=%llu |
    mask hold: <what it did>`. Paths: `record unusable: no draw recorded (Count 0)` / `... the ring mirror copy failed`
    / `... ring copy still pending (the pass was never flushed)` / `... no mirror buffer`, `mask 0: no draw of this
    record carries a stencil id (the id lookups at draw time returned 0)`, `mask 0: the N tagged draws have no previous
    draw of their key or may not represent their id`, `pairing collapse: P of N draws paired (< 50% of the last good
    frame's G)[, no previous record (not committed)]`, `fewer than half of the record's draws paired`, `Generate
    early-out: ...`, `a frame without MV generation (Commit)`, `no object record for this pass (no stencil view)`. Hold:
    `HELD 0x.... (frame k of n in a row)`, `HELD without a record (...)`, `none (not a pairing failure: the r7 path)`,
    `off (mv_objects_hold 0)`, `budget used (...)`, `none (no object dispatch in the previous frame)`.
  - **What the next log should show.** `ids-without-trusted-member/readback` near 0 while standing (r7: 3.00); no
    own-truck ids in the dump (depth ~1 m, speed 0.00, dup 1 ids gone; the ids dump's `ego set` line = the own truck's
    near parts, `ego-set` ~ that count, a handful of `ego-flagged` after a camera change, `ego-dropped` rare,
    `ego-released` once per part); `near-locked` higher, `near-accepted` ~0 while alone on the road. The video: no
    one-frame dropouts; if a `mask dropout` line appears, its path says which case it was -- `pairing collapse` /
    `record unusable` with `HELD` = covered (and `mask-held/readback` > 0), `mask 0: no draw of this record carries a
    stencil id` = the tagging side (the hold cannot help: no ids in the stencil), `this blit consumed pass s=X ...
    pairing record from pass s=Y` with Y != X - 1 (flat) = a pass ordering problem after all.
    `fast-joined-then-released` close to `fast-joined` would mean the fast join feeds the churn.
  - **Cost.** CPU per readback: <= 512 hash probes + a 256-entry rehash for the ego table, one probe per member /
    waiting entry once the ego set is non-empty; per frame one probe per recorded draw (only while the set is
    non-empty). GPU: nothing new (a record-less held frame runs one 16-thread dispatch instead of four).
  - **Synthetic WARP test (r8)** (scratchpad `objtest/obj_mv_test_r8.cpp` + `r8_scenes.inc`, 960 x 540; the harness
    renders with a 112 deg FOV, horizontal focal 324 px vs ~1880 px in the game's 2880-wide render): all r7 scenes
    S0-S10 + config B pass unchanged (S8b's box tagged at frame 43, r7: 42 -- at this scale the tractor's origin moves
    3.8 px / frame within 8 m, under the new 6 px origin pre-filter, so for a few frames the CPU reference was not
    refreshed and the box joined by the normal path one readback later; in the game's render that is ~22 px / frame),
    total tag switches 82 as in r7. **S11** idle shake: own truck = 6 camera-attached parts at 1.1..1.3 m, 3 px / 7 Hz
    at the origin, +-10 px at the probe points; 300 frames standing still, 300 driving with a car overtaking 3 m to the
    left (from 2 m ahead, 0.4 m / frame relative = the screen motion of ~18 km/h in the game): own parts never tagged,
    ego set 6 by frame 49 and through the whole drive, 0 dropped; the car tagged 3 frames after it starts moving, 0
    untagged / camera-R frames, 0 wrong MV pixels. **S11b** (9 px / 10 Hz origin shake: up to 7.7 px / frame at the
    origin, through the 6 px pre-filter in some frames): own parts tagged in the first 25 frames only (94 part-frames),
    then the ego set releases them (6), 0 dropped, flagged sightings within 10.1 px of their reference. **S12** dropped
    pass (car at 60 m; every 150th record handed to `Generate` is another draw set -- 4 statics + 30 unseen meshes,
    pairs 12 % -- or, frame 299, empty; the stencil holds the shown pass's tags): hold 3: 0 frames with a tagged car
    pixel missing from the mask, 7 mask-held frames (4 events + the 3 frames after them that pair against the odd / no
    record), the car on its held R in those 7 frames, 0 wrong MV pixels; hold 0: 14 such pixel-frames (7 frames x body +
    cabin). Against the r7 sources (`/DR7_BASELINE`, the same harness): S11 3572 own part-frames tagged, S11b 3576, S12
    (hold 3) 14 -- all three fail. Mutant "no ego set" (the CPU never flags): S11 passes (the origin pre-filter alone
    handles a 3 px origin shake), S11b fails (3215 own part-frames tagged). Result: 9850 / 9850 checks.
- **r9: no fast join, world-miss hold (round-7 in-game log `captures/dlaa_inject_ets2_v090r8_flat_driving.log` + video,
  flat ETS2, 2026-10-07 18:03-18:05, driving, `Ctrl+F6` view, video analysed frame by frame).** Report: "even trees had
  issues".
  - **What the log + video show.** (1) From 0:40 on the whole ROAD surface, the verges and trees are magenta (tagged =
    a vehicle's R). Ids dump 18:05:14: `id 6: 282 member(s) ... depth 658.8 m | speed 0.39`; `ids=15/15 members=447`,
    `over-budget=+1913` / `+464`; `fast-joined` +1063..+2545 per 600-blit window with `released` +1161..+2712 and
    `fast-joined-then-released` +332..+1326 in the same windows; `moving/readback` 55..148; `tag-switches on` 1.0..2.4
    per readback (r6: 0.06..0.37). (2) `world-miss frames 21/7200` (r7: 21/9000, r6: 28/5997): about one frame in 340
    paired NOTHING with the previous pass (`pairs this frame world=0`) and every one reset the DLSS history
    (`scene_dlaa.cpp`: `mvDone && fs.worldMiss -> reset`) -- the whole picture re-accumulated from scratch, a shimmer
    flash on trees and fences every ~5 s. The video shows 6 whole-frame tag dropouts (t = 36.38, 38.58, 38.63, 42.0,
    45.2, 94.57 s), the r8 `mask dropout` line fired once (`pairing collapse: 141 of 530 draws paired`, previous record
    530 draws, HELD): two records with the same draw count, 27 % of the keys matching -- most keys changed at once while
    the content did not (most likely new buffer pointers). (3) The ego set never flagged anything: `0 flagged of 34 near
    identities tracked | ... best unflagged run 1 of 24 readbacks` in every dump, `ego-set=0` in every window; the ids
    dump has the own truck in ids the whole time (`id 1: 44 member(s) ... depth 0.1..0.3 m`, held 564..3042 readbacks;
    `ids 4 / 5 ... depth 1.2..1.6 m speed 0.00`).
  - **Causes.** (1) The r7 FAST JOIN (`Update` step 4) let a non-spinning mover join an id on its first sighting when its
    R equals the id's within `kTightFrob`. A static draw that is a candidate for one frame (the camera R's own float
    error in a turn / under acceleration moves it > 0.5 px) has R = the TRUE camera motion -- equal to every other static
    draw's R to ~1e-5. Once one static draw held an id, every static noise candidate near a member joined it on sight
    and sticky membership kept it. Removing the fast join alone is not enough (synthetic **S13** below): r6's
    `mv_objects_hits` = 2 coherent readbacks does not reject this noise either -- a sub-2-px displacement is always
    "coherent" under the 2-px footprint floor, so a static draw that is a candidate in two readbacks within the forgiven
    gap takes an id and the rest join it by the normal R-equality join. (2) A world miss is treated as a camera cut;
    in game it is a one-frame key change (the solve buffer still holds a one-frame-old camera R that is off by the
    change of the camera motion over a frame, sub-pixel). The object path ran with `camOk = false` on such a frame and
    the dropout diagnostic did not look at it. (3) The ego set tracked the IDENTITY (key + occurrence index) and the
    origin's PROJECTION: symmetric copies of one mesh whose draw order flips name the other copy after each flip (a jump
    of hundreds of px), and an origin far off screen (the truck root under / beside the interior camera, or 0.1..0.3 m
    from the camera plane) moves by tens of px per mm of head motion -- every sighting restarted the box.
  - **Rules (r9).**
    - **No non-spinning fast join** (CPU, step 4): a non-spinning mover joins or takes an id only at `mv_objects_hits`
      coherent readbacks again (r6). The spinning-part fast join (hub test against the id's FRESH reference R, next to a
      member) stays; `fast-joined` now counts only those; `fast-joined-then-released` is gone.
    - **Camera-noise guard** (GPU, `CSMain`): column 2 of an R is where it maps clip `(0, 0, 1, 0)` = the camera ORIGIN.
      A rotation about the camera leaves it put; any motion relative to the world (translation, rotation about a point
      away from the camera) moves it by (relative motion) x focal / near. `devT` = the screen deviation that the column-2
      difference between the draw's R and the camera R explains, at the same probe points as `dev` (clip z there = the
      near term, linearized). A trusted candidate with `devT` below half the moving threshold (0.25 px) moves like the
      world: the camera R is off, not the draw -- reject reason 7 (`kRejNoise`, "camera noise"): never a mover, seen
      (ego set, sticky membership), a waiting run restarts, not cyan in the debug view. A translating mover's deviation
      IS its `devT`; a part spinning about its own origin has `devT` ~ angle x its distance from the camera. Not covered:
      a camera-solve error in TRANSLATION (column 2 itself) -- those candidates behave as in r6.
    - **World-miss hold** (`CameraMv::Generate`): a frame whose camera candidates pair nothing keeps the solve buffer as
      it is (pass A skipped, like the v0.6.1 reuse path) when a good solve exists, for up to `mv_objects_hold` (3)
      frames in a row (`FrameStats::missHeld`); `SceneDlaa::Run` resets the history only for a miss outside that budget
      (or with no good solve since the last `Invalidate`, or `mv_objects_hold = 0`). Applies with `mv_objects = 0` too.
      Trade-off: a real scene switch that changes every key is no longer reset on its first frame (DLSS rejects the old
      history over a few frames). The object path treats the frame as a pairing failure: the r8 mask hold (the last
      good mask stays in `Ids.x`, ids without a fresh representative keep their R through the r7 hold), the frame is
      not a new baseline, and the dropout diagnostic fires on every world-miss frame that follows a frame with ids,
      naming `world-miss: the camera candidates paired nothing with the previous pass (camera R held, frame k of n in a
      row | ...: the history is reset)`.
    - **Fallback pairing** (pass A and the object record; `CameraMv::SetPairFallback`, on -- off only in the synthetic
      test): a draw whose full key finds no previous draw pairs by its LOOSE key (indexCount, startIndex, baseVertex)
      when that loose key is drawn exactly once in both records (its full-key partner would share it: no steal). Pass
      A: such pairs are "not unique" (the medoid prefers full-key pairs). Object record: the partner is the identity's
      own previous draw (rank 0); `ObjRecord::Add` takes the raw key (`ObjRecord::Geo`, from `ObjOnDraw`), `ObjFinish`
      commits this pass's sorted loose keys as the next prev list. Identities stay pointer-based: after a key change the
      new identities are untagged until the CPU takes them in (S14: 3 frames per change), only pairing and the R survive.
    - **Unpaired-draw diagnostic** (first 5 each, a frame whose candidates / object draws paired fewer than half through
      the FULL key): a sample of 6 unpaired draws with ib / vb pointers, offsets, indexCount / startIndex / baseVertex and
      the previous record's draw with the same loose key (how many, its pointers / offsets) -- so the next log shows what
      changed.
    - **Ego set by geometry key** (CPU, step E): one entry per PHYSICAL COPY of a key (256, open addressing on the key,
      duplicates allowed); a sighting updates the nearest copy of its key within 3 x `mv_objects_ego_px` that has no
      sighting in this readback, else it opens a new copy. The position is measured at the copy's reference view depth
      `wr` (its box start, at least 1 m): `(0.5 W x, 0.5 H y, 0.5 W w) / wr` of the clip origin (px-equivalent; an
      on-screen origin moves as projected within ~1.4 x, an off-screen one by its clip-space motion). Box / flag / drop
      rules as r8 per copy (3 axes). The EGO IDENTITIES (512): an identity whose sighting matched a flagged copy (put
      back on every such sighting); a sighting that matches an unflagged copy, opens a new one, has no position or is not
      near takes it out. `IsEgo` (Table.z bit 15, member / waiting / follower release) reads them.
    - **Ego trace** (new dlaa.ini `mv_objects_ego_trace`, default 0): for the first 150 readbacks with a near sighting
      one line with up to 4 of them.
  - **Logs (r9).** Config line `per-object motion vectors (v0.9.0 r9 no fast join, world-miss hold): ...
    mv_objects_ego_rb=%d mv_objects_ego_trace=%d`; the "MV objects: ready" line gains an `r9:` part; `MV objects @blit`
    drops `fast-joined-then-released` and gains `| r9: world-miss-held=+%llu pairs-by-fallback/readback=%.2f
    camera-noise/readback=%.2f ego-ids=%d` (world-miss-held = eye 0 frames answered with the last good camera R;
    pairs-by-fallback = object-record draws paired through the loose key; camera-noise = reason-7 rejects; ego-ids =
    identities in the ego set now); `MV: frame` gains `| r9: world-miss held=%llu (no reset), pairs by fallback key=%llu`
    (and `(R held)` after the pair counts on a held frame); the ids dump's ego line reads `  ego set (own truck, r9 by
    geometry key): %d flagged of %d near copies tracked, %d ego identities | flagged sightings deviated up to %.1f px ...
    | best unflagged run %d of %d readbacks (mv_objects_ego_rb)`; new lines `MV: unpaired-draw sample #N at frame F
    (world-miss by the full key | full-key pairing < 50%): world candidates cur C prev P, paired A by the full key + B by
    the fallback key (on|off) | [ib .. vb .. ibOff .. vbOff .. ic .. si .. bv .. -> prev same loose key: none | n, first
    ib .. vb .. ibOff .. vbOff ..] x6`, `MV objects: unpaired-draw sample #N at frame F[ (world-miss frame)]: D draws
    (previous record P), A paired by the full key + B by the fallback key (on|off) | [...] x6` and `MV objects: ego trace
    #N (readback U, frame F): S near sighting(s), C copies tracked, G flagged, I ego identities | [key %016llx id %016llx
    -> copy K (new copy | new copy (far) | box grows | box restarted | FLAGGED | flagged | flagged, out | flag DROPPED |
    not near | no position | table full, d %.1f px) | pos x y z (projected origin sx, sy px) w %.3f | n N box x lo..hi y
    lo..hi z lo..hi | flag 0|1] x <= 4`.
  - **What the next log should show.** No static ids in the dump (no 200-member ids at hundreds of metres),
    `moving/readback` back to the traffic (single digits alone on the road), `camera-noise/readback` > 0 in turns,
    `tag-switches on` back near r6 (0.06..0.37); `world-miss frames` about as before but `world-miss held` equal to it
    (no reset flash: the picture no longer shimmers every few seconds); the first `MV: unpaired-draw sample` lines say
    whether the pointers changed (same loose key, other ib / vb) -- then `pairs by fallback key` > 0 and world-miss frames
    near 0; the ego line `N flagged of M near copies` with N = the own truck's exterior parts after a few seconds and no
    own-truck ids (depth ~0.1..1.6 m, speed ~0) in the dump. If the ego set still stays empty, `mv_objects_ego_trace = 1`
    shows per sighting why (box restarted / new copy (far) with the positions).
  - **Cost.** GPU: one column compare + 5 probe evaluations per candidate in `CSMain` (candidates only). CPU per pass:
    one more sort of <= 2048 loose keys and, for unpaired draws only, two binary searches; per world-candidate miss a
    128 x 128 loose-key scan (only on misses); the ego set by key costs what the r8 one did plus one identity probe per
    flagged match. `ObjRecord` +32 B per draw (raw key), `CameraMv` +~120 KB (prev loose / position / raw-key lists).
  - **Synthetic WARP test (r9)** (scratchpad `objtest/obj_mv_test_r9.cpp` + `r9_scenes.inc`; the r7 / r8 scenes from
    `r7_scenes_r9.inc` / `r8_scenes_r9.inc`; `build_r9.bat`, `/DR8_BASELINE` against the r8 sources = `build_r9_base8.bat`):
    all r8 scenes S0-S12 + config B pass; **S10** (LOD switch) is the r7 fast-join test and now has the r6 bound: 24
    untagged body frames over 6 switches (r8: 19; bound 4 per switch + 1). Total tag switches 82 as in r8. **S13** noise
    candidates (300 frames, moving camera; the 12 camera candidates carry a per-frame random view rotation of up to
    1.9 mrad roll + 0.4 mrad yaw / pitch -- the camera solve's error; 200 static draws at 70..480 m: mean 19.8, max 119
    of them > 0.5 px from the camera R per frame, different ones each frame; a far vehicle at 300 m, 0.86 px / frame):
    r9 0 static draws tagged, the vehicle tagged at frame 4 and kept, 0 wrong MV pixels, moving entries 1.9 per
    readback; r8 sources: static draws tagged in 292 of 300 frames, 123 distinct (first at frame 8), 22 moving entries
    per readback; r9 WITHOUT the camera-noise guard (an intermediate build): 292 frames, 118 distinct -- the fast join
    is not the main path. **S14** key change, permanent (all buffer pointers change at frames 150 / 300 / 450, content
    the same; a car at 60 m + 40 statics): fallback on 0 world-miss frames, 0 history resets, 162 object draws paired
    through the fallback key, the car untagged 9 frames (3 per change: new identities), 0 wrong MV pixels; fallback off
    3 world-miss frames, all held, 0 resets, car untagged 12 frames, 0 wrong pixels; r8 sources: 3 world-miss frames, 3
    resets, 4 wrong car pixels (pass A wrote the identity R). **S14b** the same for ONE frame per 150 (f % 150 == 149):
    fallback on 0 world-miss frames, 0 resets, 378 fallback pairs, car untagged 4 frames (1 per event: the stencil tag
    comes from the pointer-based identity); fallback off 7 world-miss frames (two per event, the last event one), all
    held, 0 resets; r8: 7 resets, 4 wrong pixels. **S15** flipping copies (the S11b own truck, parts 0 / 1 one mesh 0.9 m
    apart whose draw order swaps every 7 frames): 6 copies flagged and both mirror identities ego at the end of the
    standing phase, own parts tagged only in the first 25 frames (81 part-frames), never after; r8 sources: 4 flagged,
    mirror identities not ego, 824 own part-frames tagged after frame 60. **S15b** origin off screen (4 parts sharing a
    root 1 m ahead, 0.8 m left, 3 m below the camera, ndc y ~ -3.6; +-2 cm depth / +-3 mm lateral bob at 7 Hz: the
    projected origin swings ~28 px, the position measure ~19 px): 4 copies flagged by frame 200, own parts tagged only in
    the first 25 frames; r8: 0 flagged, 1760 own part-frames tagged after frame 60. Result: 13915 / 13915 checks (r8
    sources: all r9 scenes fail, 13879 / 13909).
- **r10: origin gate, attach, ego view-space (round-8 in-game log `captures/dlaa_inject_ets2_v090r9_flat_driving.log` +
  video, flat ETS2, 2026-10-07 19:08-19:12, driving, `Ctrl+F6` view, video analysed frame by frame).**
  - **What improved (r9).** `world-miss frames 3/16800` (r8: 21/7200) and 341 pairs by the fallback key: the 5-second
    whole-screen flashes are gone; the camera-noise guard fires (`camera-noise/readback` 0.1..13.3).
  - **What the log + video show.** (1) Video 2:35-2:48: the whole road, the right verge and its trees magenta for 13 s;
    log 19:11:38..47: `members` 299, `dup` 13, `diverged` 12..28, ids dump `id 3: 28 member(s) ... jumps 1735`, `id 1:
    60 member(s) depth 240 m speed window 0.00..0.35`; `released` 1000..2400 per 600-blit window, all of it long-lived
    churn of batched geometry. (2) User: the licence plate of an AI vehicle is not tagged while its body is -- the plate
    ghosts / smears on a sharp body. (3) `ego set ... 0 flagged of 112 near copies tracked ... best unflagged run 1 of
    24` (copies growing 30 -> 112 over the run), `near-locked` 0..2.5; own-truck ids persist (`id 2: 33 members depth
    0.3 m held 12399 readbacks`, `id 4 depth 0.7 m speed 0.00`, `id 6 depth 1.7 m`).
  - **Causes.** (1) A static draw that is a candidate in 2 coherent readbacks (dev > 0.5 px: float noise of the camera
    R on bumps / turns; with a TRANSLATION part the r9 devT guard does not catch it) has R ~ the camera R, and so has a
    vehicle moving slowly relative to the camera (same-direction traffic: the common highway case) within `kTightFrob`:
    the static draw joins that id by R similarity. Harmless at first, but the vehicle's relative motion changes while
    sticky membership keeps the static members, and the r7 "no verdict when the origin has no screen position" rule
    meant that a road / terrain / tree-batch mesh -- origin anywhere, often behind the camera -- was never proven out.
    (2) A plate is its own draw, the same mesh on every vehicle: `dup` / `pairing` / `ambiguous`, never a member; the r3
    follower rule is off by default (and its hub test is loose). (3) The r9 position measure (clip x / y / w at the
    copy's reference depth >= 1 m, px-equivalent) is ~1880 px per metre at the game's focal: `mv_objects_ego_px` 12 =
    6 mm of motion, below the cabin sway -- every sighting restarted its box or opened a new copy; and an origin behind
    the camera plane (w <= 0.05) had no position at all.
  - **Rules (r10).**
    - **Origin gate** (GPU `CSMain` + CPU): a candidate is a mover / member only while its ORIGIN has a screen position
      inside the viewport plus `mv_objects_origin_margin` (new key, default 1.2 = 20 % beyond the edge, 1.0..3.0): clip
      w > 0.05 and |x / w|, |y / w| <= margin (`ObjCB.Ext.x`). Otherwise reason 8 `kRejOffscreen` (counted, never a
      mover, a waiting run restarts, not cyan in the debug view); an off-screen draw never represents its id; every
      TAGGED draw with an off-screen origin is reported as a reason-8 entry even when it is no candidate (bit 9,
      `Reject::drawnOnly`, not counted as a reject), so a member whose latest sighting is off screen is released
      (`offscreen-released`). Spinning parts: the same (a wheel's origin is its hub). The r7 "no verdict" branches of
      the membership re-check (step 3: the id's motion puts the origin behind the camera; step 5: the hub test has no
      screen position) are releases now. Vehicles' origins are on the vehicle; big static meshes' origins are anywhere.
      Consequence: a vehicle whose origin leaves the margin (a trailer passing alongside within ~2-4 m, a car at the
      screen edge) loses its tag there even if part of its body is still visible.
    - **Minimum own motion** (CPU, step 4): a NON-spinning new mover founds or joins an id only from sightings with
      `dev` (the GPU's max probe deviation, px of the full render) >= `mv_objects_min_px` (new key, default 1.0,
      0.5..4) in each of the `mv_objects_hits` coherent readbacks; a weaker sighting counts as drawn sub-threshold (the
      r6 hysteresis: one forgiven readback when the last correction was small) and `sub-min-px`. The candidate threshold
      (`Params.x` = 0.5 px) stays for sightings; members keep their id as in r6.
    - **Attach** (CPU, step 5b; `mv_objects_attach`, new key, default 1): a dup / pairing / ambiguous reject whose clip
      ORIGIN coincides with a non-spinning member's origin listed in THE SAME readback -- projected <= 0.5 px AND |w
      difference| <= 1 % of w: the same world matrix (a plate, a mirror, a lamp on that vehicle) -- in `mv_objects_hits`
      readbacks in a row with the same member joins that member's id as an ATTACHED member (`attached`): never anchors /
      represents (LUT "may not represent", Table.z bit 13), follows its anchor member's id, is released when its anchor
      member leaves or when both origins are listed but no longer coincide in 2 readbacks in a row; a trusted mover of
      its own becomes a full member. A forward-pass plate (alpha-blended, r5 forward depth) already gets its MV from the
      G-buffer stencil behind it; this rule is for G-buffer plate draws.
    - **Ego set in view space** (GPU `ViewOrigin` + CPU `EgoSee`): each moving / reject entry carries the origin's
      VIEW-SPACE position, recovered from the MVP alone -- row 3 = +-(view row 2), so |row 3.xyz| is the world scale s,
      the skew pe = dot(row 0, row 3) / s^2 and the focal fx = |row 0 - pe row 3| / s (row 1: qe, fy); X = (c0.x - pe
      c0.w) / fx, Y = (c0.y - qe c0.w) / fy, Z = c0.w (exact for a uniformly scaled world matrix; a non-uniform scale
      gives a part-constant affine error, harmless for a persistence test). Valid at / behind the camera plane. The copy
      match is 3 x `mv_objects_ego_m_box` (new key, default 0.05 m, 0.01..0.5), the box 2 x that per axis, the flag drop
      2 sightings beyond it -- the r8 / r9 rules in metres; `mv_objects_ego_px` is no longer used (still parsed).
      `mv_objects_ego_trace` default 1 (first 150 readbacks with a near sighting). Reject entries grow to 80 B.
  - **Logs (r10).** Config line `per-object motion vectors (v0.9.0 r10 origin gate, attach, ego view-space): ...
    mv_objects_hold=%d mv_objects_ego_m_box=%.3f mv_objects_ego_rb=%d mv_objects_ego_trace=%d
    mv_objects_origin_margin=%.2f mv_objects_min_px=%.1f mv_objects_attach=%d -- ...` (`mv_objects_ego_px` dropped); the
    "MV objects: ready" line gains an `r10:` part; `MV objects @blit` gains `| r10: offscreen-rejected/readback=%.2f
    offscreen-released=+%llu sub-min-px/readback=%.2f attached=+%llu attached-members=%d` (offscreen-rejected =
    reason-8 candidates per readback; offscreen-released = members released for an off-screen origin; sub-min-px = new
    non-spinning sightings under `mv_objects_min_px` per readback; attached = attach joins in the window,
    attached-members = attached members now); the ids dump header gains `| r10: origin on screen within %.2f (else no
    mover, members released), new movers from sightings >= %.1f px only, attach on|off`, each id line `(%d
    follower(s), %d spinning, %d attached)`, and the ego line reads `  ego set (own truck, r10 by geometry key, view
    space): %d flagged of %d near copies tracked, %d ego identities | flagged sightings deviated up to %.3f m from their
    reference (box %.3f m per axis, mv_objects_ego_m_box) | best unflagged run %d of %d readbacks (mv_objects_ego_rb)`;
    the ego trace entries read `[key .. id .. -> copy K (what, d %.3f m) | view pos %.3f %.3f %.3f m (projected origin
    %.0f, %.0f px) w %.3f | n N box x %.3f..%.3f y %.3f..%.3f z %.3f..%.3f m | flag 0|1]`.
  - **What the next log should show.** No static ids in the dump (no 28- / 60-member ids at hundreds of metres, no
    `jumps` in the thousands), `released` per window back to the traffic's churn, `offscreen-rejected/readback` > 0
    (the batches that used to get in), `offscreen-released` small after the first windows; `sub-min-px/readback` > 0
    in turns / on bumps; plates: `attached` per window ~ the plated vehicles entering the view, `attached-members` ~ the
    plated vehicles on screen, in the `Ctrl+F6` view the plate magenta with its body. Ego: the trace's `view pos` of
    the own parts constant within a few cm (`box grows` ... `FLAGGED` after 24 sightings), the ego line `N flagged of M
    near copies` with N = the own parts, M about N plus the traffic passing close, and no own-truck ids (depth 0.1..1.7
    m, speed 0.00) in the dump. If the ego set still stays empty, the trace's `d` / `box` values say how much the
    own parts really move in view space (raise `mv_objects_ego_m_box` to that).
  - **Cost.** GPU: one origin test per draw, `ViewOrigin` (3 dots, 2 lengths) per moving / reject entry, reject
    entries +16 B (256 x 16 = 4 KB more per readback), the reject list now also holds the off-screen tagged draws.
    CPU per readback: one origin test per entry, the attach pass (attached members x members linear search, rejects x
    listed anchors), no new per-frame work.
  - **Synthetic WARP test (r10)** (scratchpad `objtest/obj_mv_test_r10.cpp` + `r10_scenes.inc`; the r7 / r8 / r9
    scenes from `r7_scenes_r10.inc` / `r8_scenes_r10.inc` / `r9_scenes_r10.inc`; `build_r10.bat`, `/DR9_BASELINE`
    against the r9 sources = `build_r10_base9.bat`). The regression scenes run with `mv_objects_min_px` 0.5 (the
    harness renders 960 wide with a horizontal focal of 324 px, the game ~1880 px: the game's 1.0 px is 0.17 px here,
    below the 0.5 px candidate threshold -- the rule has no effect at this scale; with 1.0 for every scene, S7 A (0.39
    px / frame here, ~2 px in the game), S8 (the approaching truck), S10 (LOD re-join at 1.2 px / frame) and S17 car 0
    miss their tag deadlines: 21715 / 21724).
    All r9 scenes S0-S15 + config B pass; the origin gate CHANGES four of them by design, so their tag-loss / switch /
    camera-R counts and deadlines only count frames with the part's origin on screen: S7 D1 (its origin leaves the
    margin at frame ~195: released there, 5 frames; was "tag at the end"), S8 box (origin at 2.1 m depth, 4 m to the
    left, beyond the margin from frame 175: 27 frames untagged; the tractor keeps the id), S8b box (origin enters the
    margin at frame 64: tagged at 68, r9: 4 frames after its first probe point was in front of the camera), S12 / S14
    car (origin beyond the right edge from frame ~541: 60 frames; S14 / S14b untagged 9 / 12 / 3 / 3 frames
    without them, r9: 9 / 12 / 4 / 4). Total tag switches 82 as in r9. **S16** slow convoy +
    noisy world (300 frames, moving camera; a car 18 m ahead in the left lane at the camera's speed +-1.5 m, R ~
    identity, ~1.9 px / frame of parallax against the camera R; 200 statics: 40 roadside props 5..200 m, 60 props
    200..480 m, 100 batches with off-screen origins |x / w| or |y / w| 1.6..2.6; every frame 20 random statics get a
    per-draw view-space TRANSLATION error -- props 0.55..0.85 px, batches 1.2..3 px at the worst probe point -- each
    noisy matrix gives two coherent candidate sightings, then 2 clean frames): r10 0 static draws tagged, 4807 sub-min
    sightings, 4765 reason-8 rejects, the car tagged at frame 4 and kept, 0 wrong MV pixels; with `mv_objects_min_px`
    0.5: 58 props / roadside props tagged (own ids), 0 batches; with the margin at 3.0: 43 batches tagged, 0 props; r9
    sources: 62 statics tagged in 296 frames (every class). **S16c** the same without the 2-frame cooldown (two errors
    in a row add up to 1.7 px > 1.0): 3 statics found ids of their own (never the car's: 0 static draw-frames in the
    car's id) -- the residual: a static draw whose own noise twice exceeds `mv_objects_min_px` in coherent readbacks
    still takes an id, and sticky membership keeps it (its R is its own, ~ the camera's). **S17** plates (6 cars, 30..75
    m, 0.15..0.40 m / frame crossing each other, one plate mesh on all 6 = dup 6, plate MVP = body MVP): attach on --
    every plate tagged 1 frame after its car (frame 5 vs 4), always with its own car's id (0 frames with another car's
    id), never lost, 6 attached members, 0 wrong MV pixels (bodies and plates); attach off -- plates never tagged; r9
    sources: plates never tagged. **S18** ego view-space (400 frames driving; 7 own parts at view depth 0.1 / 0.3 / 0.5
    / 1.0 / 1.3 / 1.7 m and 0.2 m BEHIND the camera plane, 4 origins off screen; cabin sway +-2 cm at 1.3 Hz + head bob
    +-2 cm vertical / +-0.5 cm depth at 7 Hz; `mv_objects_ego_px` 2 = the game's 12 px at its focal, for the baseline; a
    car overtaking 3 m to the left from frame 60): all 7 own parts ego at frame 26, 18 own part-frames tagged before
    (the 0.5 m part, whose bob passes the 6 px origin pre-filter), 0 after, no flag dropped; the car tagged at frame 64
    (moving from 61), kept, never ego, 0 wrong MV pixels; r9 sources: 0 flagged, 1577 own part-frames tagged. Also seen
    in the main scenes S0-S5: the LOD car's two rear wheels (their hubs lie on the body origin's line of sight: < 0.5
    px, 0.7 % of w apart at 110 m) attach to its body -- they take the body's motion, as spinning members do. Result:
    21723 / 21723 checks (r9 sources: S0-S15 pass, S16 / S17 / S18 fail, 21711 / 21722).
- **r11: overlay depth, camera-origin, static class (round-9 in-game log
  `captures/dlaa_inject_ets2_v090r10_flat_driving.log` + video, flat ETS2, 2026-10-07 20:05-20:08, driving, `Ctrl+F6`,
  video analysed frame by frame).**
  - **What the log + video show.** (1) Video 1:10-1:43 (log 20:07:16-20:07:49): the road surface, the side-window view
    and the near verge turn BLUE-based in the MV view -- sampled road pixels (125, 124, 253) = colour (0.5, 0.5, 1.0) =
    the `cabin` classification with a zero motion vector; the same road 20 s earlier shows the normal camera-motion
    gradient. Weather / light dependent ("trees had issues", "the road smears"). (2) The ego trace (on by default since
    r10) shows every near sighting at `view pos 0.000 0.000 0.000 m ... w 0.000` (35 per readback) and the ego set
    `239 of 240 near copies` flagged, 251 ego identities; earlier logs had ids at depth 0.0 / 0.3 m. (3) Road and verge
    still magenta-tinted besides the blue; the ids dump shows ids of 39-52 members at 10-196 m with speed 0.28-0.35 and
    0 spinning; `released` 173-585 and `offscreen-released` 62-197 per window. (4) `attached` +1..+23 per window,
    `attached-members` 0..2.
  - **Causes.** (1) A world-layer alpha-blended ("over") forward draw that sits at the camera -- a windscreen overlay
    (rain drops / dirt / glare / a reflection quad) that comes and goes with weather or light -- qualifies for the r5
    depth-only re-draw and writes NEAR depth (>= 0.9) over the whole glass; pass B takes `max(twin, forward)` and calls
    every pixel behind it `cabin` (R_cabin, z remapped into [0.9, 1]). The r5 audit frame (a parking lot) had no such
    overlay. The debug view suppresses the yellow forward tint at d >= 0.9, so it showed blue. (2) Draws whose rows 4..7
    are no object MVP (a batched / instanced draw whose per-instance transform lives elsewhere, an overlay with the
    view-projection only): their clip origin is the camera (w = 0). (3) The static world keeps founding / joining ids
    from occasional coherent candidate sightings (camera-R noise, per-draw float noise) -- nothing remembered that
    those draws are static almost always.
  - **Rules (r11).**
    - **Forward depth in the world range only** (pass B, `kReprojDepthShader`): `f = FwdDepth[src]; if (!(f >= 0.01 &&
      f < 0.9)) f = 0;` before the max (also the debug view's yellow tint). Pixels whose forward depth was >= 0.9 are
      counted on a 1-in-16 grid (every 4th column and row, 64 striped dword counters, u2, async readback; x 16 on the
      CPU = an estimate): `near-range px` in the forward-depth line.
    - **Near-camera forward draws** (inject.cpp `FwdOnDraw`, new key `mv_fwd_min_m`, default 0.5 m, 0.1..5): a
      qualifying draw whose MVP origin (rows 4..7 of VS cbuffer slot 0, the G-buffer layout) lies within it of the
      camera -- view-space length (the CPU copy of `ViewOrigin`, `ObjIds::MvpViewOrigin`), or clip |w|, |x|, |y| all
      below it -- is not re-drawn (`skipped: near-camera`). The MVP is on the GPU only: every qualifying draw copies its
      64 bytes (one `CopySubresourceRegion`, the hook bypassed) into its pass's staging slot (4 slots, 256 draws per
      pass), read back without waiting at a later pass start (`oMap` DO_NOT_WAIT); the verdict is cached per
      forward-draw IDENTITY (geometry key + vertex shader + occurrence index in the pass; 4096-entry cache of near
      identities, cleared when half full). A new near identity is therefore skipped 2-4 passes late (the range rule
      covers those frames when it sits at the near plane); a skipped draw keeps copying, so its verdict follows it. The
      first 5 near identities are logged.
    - **Camera-attached draws** (GPU `CSMain` / `CSVeto`, new key `mv_objects_cam_m`, default 0.3 m, 0.05..2,
      `ObjCB.Ext.y`): the view-space origin within it of the camera (or clip |w|, |x|, |y| all below it) -- reason 9
      `kRejCamOrigin`: never a candidate, mover, representative, ego sighting or attach anchor, never judged by the
      veto; a TAGGED one is reported (bit 9, `drawnOnly`) and its membership released; every such draw is counted in the
      Moving tail (+16 B at `kObjTailCam`). `DrawIndexedInstanced` is not hooked at all, so instanced draws were never
      recorded (no change needed).
    - **Static class** (CPU `ObjIds::Update`, new key `mv_objects_static_rb`, default 24, 8..48, 0 = off): per
      identity a 48-readback history (bit masks, lazily shifted; 8192-slot open-addressing table, <= 4096 live entries,
      compacted every 32 readbacks) of the readbacks in which it was DRAWN (the read-back pass's paired identities) and
      the STATIC ones among them: drawn but no candidate (within 0.5 px of the camera R), a camera-noise reject, or a
      tagged non-candidate off screen. Moving entries and dup / pairing / ambiguous / own-truck / off-screen candidate
      rejects are sightings of non-static or unknown motion; veto records and camera-attached draws give no mark. >=
      `mv_objects_static_rb` sightings in the last 48 with a static share >= 50 % = STATIC CLASS: it neither founds nor
      joins an id (step 4 new movers, the follower rule, the attach rule -- `static-blocked`), and a member that becomes
      static class is released (`static-released`; an id left without members is freed). A vehicle moving relative to
      the world is a candidate in every readback; a parked / stopped one is static class (the camera R is right for it)
      and leaves the class after ~24 readbacks of motion, then re-founds its id through the normal hits.
    - `mv_objects_min_px` default **2.0** (r10: 1.0): a far vehicle below 2 px / frame of own screen motion stays on the
      camera R.
    - **Attach diagnostics** (no behaviour change): one `MV objects: attach diagnostics` line per 600 readbacks.
  - **Logs (r11).** Config lines `per-object motion vectors (v0.9.0 r11 overlay depth, camera-origin, static class): ...
    mv_objects_attach=%d mv_objects_cam_m=%.2f mv_objects_static_rb=%d -- ...` and `forward depth for motion vectors
    (v0.9.0 r5; r11 overlay depth): mv_fwd_depth=%d (%s live switch) mv_fwd_min_m=%.2f -- ...`; `MV objects @blit` gains
    `| r11: cam-origin/readback=%.2f static-class=%d static-blocked/readback=%.2f static-released=+%llu`
    (camera-attached draws per readback; identities in the static class now; new-mover / follower / attach sightings
    refused for it per readback; members released for it in the window); the forward-depth per-pass line reads `...
    skipped: sky / cabin layer %.1f, other states %.1f, near-camera %.1f, no target %.1f | CPU %.3f ms/pass (incl. the
    re-draw calls) | r11: near-camera = MVP origin < %.2f m (mv_fwd_min_m): MVP copies %.1f, verdicts %.1f (near %.1f),
    passes without a readback slot %llu; near-range px %.0f (forward depth >= 0.9, ignored by the MV pass; per frame,
    1-in-16 estimate)`; new `MV forward depth: near-camera draw #%d (v0.9.0 r11, not re-drawn from now on) -- IndexCount
    %u, MVP origin %.3f m from the camera (recovered: view %.3f %.3f %.3f m, clip w %.3f; mv_fwd_min_m %.2f) | blend RT0
    src %d dest %d write mask 0x%x | depth func %d | viewport depth %.3f..%.3f | pass s=%llu` (first 5); new `MV
    objects: attach diagnostics over %llu readbacks (v0.9.0 r11, attach on|off) -- rejects %llu: not dup / pairing /
    ambiguous %llu (own truck %llu, camera noise %llu, origin off screen %llu, other %llu) | dup / pairing / ambiguous
    %llu: already a member %llu, ego %llu, origin off screen %llu, no member listed in the readback %llu, nearest member
    origin > 0.5 px %llu (< 2 px %llu, < 8 px %llu, < 32 px %llu, >= 32 px %llu), within 0.5 px but w differs > 1%%
    %llu, coincided %llu (static class %llu) | listed anchors per readback %.1f | attached members now %d`; the ready
    line and the ids dump header name the r11 rules.
  - **What the next log should show.** In the `Ctrl+F6` view no blue road / verge / trees behind the windscreen in rain
    / glare; `near-range px` > 0 exactly while such an overlay is up, and `near-camera` > 0 once its verdict landed (the
    `near-camera draw` lines name it: IndexCount, blend, viewport, origin distance). `cam-origin/readback` ~ the draws
    the r10 trace showed at view pos 0 (tens); the ego trace without `view pos 0.000 0.000 0.000` sightings and the ego
    line `N flagged of M near copies` with N, M ~ the own truck's parts (not 239 of 240). No static ids in the dump (no
    39-52-member ids at 10-196 m), `static-class` in the thousands, `static-blocked/readback` > 0, `released` per window
    down to the traffic's churn. The attach line says what blocks plates: `no member listed` (their car is not a
    member that readback), `nearest member origin > 0.5 px` (the plate is not on the body's world matrix: not a
    G-buffer twin of the body), `w differs` or `coincided` with few joins (hits not reached in a row).
  - **Risks / limits.** (a) The near-camera verdict assumes the forward draws keep their MVP at rows 4..7 of VS
    cbuffer slot 0 like the G-buffer draws; if a forward shader uses another layout the verdicts are garbage (the
    near-camera lines show the distances). (b) A forward draw whose rows 4..7 are the camera-relative view-projection
    (a batched wire / fence mesh in world coordinates) has its origin AT the camera and is skipped too -- the r5 wire
    fix would be lost for such a batch (the near-camera lines and a drop of `re-drawn` would show it; `mv_fwd_min_m`
    can only go down to 0.1). (c) Under a full-glass overlay at >= 0.9 a wire behind it loses its forward depth until
    the verdict skips the overlay (rule 1a alone drops every forward depth at those pixels). (d) The static class
    releases any member whose own motion stays within 0.5 px of the camera R in >= half of 24+ readbacks -- also a
    part rigidly attached to a moving vehicle when its own deviation is sub-threshold (it then takes the camera R,
    error <= ~0.5 px); a vehicle that stops loses its id after ~24 readbacks and needs ~27 readbacks of motion to get
    it back.
  - **Cost.** GPU: one `ViewOrigin` + length per object draw (camera-attached test) and per tagged draw in the veto,
    pass B one extra texture compare + a 1-in-16 striped atomic where the forward depth is >= 0.9 (none normally),
    16 B more per object readback, a 256 B counter readback per frame. CPU: per qualifying forward draw 3 IA / VS
    getters + one 64-byte copy + one hash lookup; per pass start one DO_NOT_WAIT map of a landed slot (<= 256 x 64 B);
    per object readback one history mark per listed / drawn identity and one class test per member.
  - **Synthetic WARP test (r11)** (scratchpad `objtest/obj_mv_test_r11.cpp` + `r11_scenes.inc`; the r7..r10 scenes
    unchanged; `build_r11.bat`, `/DR10_BASELINE` against the r10 sources = `build_r11_base10.bat`; new command-line
    switches `static N` / `camm X` (regression scenes) and `movingpx X` = `CameraMv::SetMovingPx`, a test-only setter of
    the object candidate threshold `Params.x`, default 0.5, never called by the injector). **S19** windscreen overlay
    (40 frames driving; twin = sky + a road plane; forward depth = a wire band at 10 m every frame + a full-screen quad
    0.2 m ahead in frames 10..29): overlay depth 0.93 (the in-game case) -- 0 of 229064 sampled road pixels cabin,
    merged depth = twin, MVs = the camera R's, near-range counter 20.00 x W x H (= the 20 overlay frames); the wire
    loses its forward depth under the overlay (2740 of 5206 wire samples: rule 1a alone); r10 sources: 120560 road
    samples cabin, MVs off by up to 117 px. Overlay depth 0.455 (inside the world range, so only the re-draw verdict
    helps; the harness applies `ObjIds::CamAttached(MVP, 0.5 m)` to decide the re-draw -- inject.cpp's staging copy /
    readback / identity cache are not exercised): overlay skipped 20 of 20, wire never skipped and keeps its depth (5206
    of 5206), road unchanged; r10 sources: every road sample off. Reported: a VP-only wire batch WOULD be skipped (the
    blind spot, see Risks). **S20** camera-origin draws (300 frames; 10 VP-only, 10 MVP = P, 10 at 0.05..0.23 m; a car
    crossing at 20 m; 3 own parts at 1.0..1.5 m): cam-origin 30.00 per readback, the 30 never tagged / never ego, ego
    copies 3 (the own parts, flagged at frame 26), the car tagged at frame 4 and kept, 0 wrong MV pixels; r10 sources:
    33 ego copies, 8081 camera-origin draw-frames in the ego set. **S21** static class (380 frames, camera 0.1 m /
    frame, min px 2.0; 100 statics 45..180 m with 3-frame 2.2..3.0 px coherent noise bursts, p = 2.5 % per frame each --
    892 bursts; a car 25 m ahead crossing at 0.25 m / frame that stops for 120 frames): defaults -- 16 statics found ids
    in the first 30 frames (no history yet), 0 tag-ons after frame 30, 0 static tagged draw-frames after frame 40; the
    car tagged at frame 4, released 23 frames into its stop (frame 153), re-tagged 27 frames after it moves again, 0
    wrong MV pixels; `mv_objects_static_rb 0`: 37 statics tagged, 29 tag-ons after frame 30, 5712 static tagged
    draw-frames after frame 40, the stopped car keeps its id; r10 sources: as with the rule off (38 / 27 / 5729).
    **Regression (S0-S18 + config B):** with `static 0` (the static class off) all r10 checks pass -- the overlay,
    camera-origin, min-px and attach diagnostics changes alter none of them. With the default 24 the static class
    CHANGES S7 (car B stops: released by design; car A, 0.39 px / frame here, is a static sighting in most readbacks),
    S8 (the tractor, rigid with the moving box but within 0.35 px of the camera R until frame 38), S11 / S11b / S12 (the
    passing / test car), S17 (bodies at 0.65..1.6 px / frame) and S18 (the passing car) -- 32 of 23874 checks, all
    tag-loss / switch / camera-R-after checks; 0 wrong MV pixels in any scene. Cause: scale -- this harness renders with
    a 324 px focal, the game ~1880 px, so its vehicles move ~5.8x fewer px than the same scene in the game, and the
    static class keys on the fixed 0.5 px candidate threshold. With `movingpx 0.086` (= the game's 0.5 px at this scale)
    and the default static class ALL 23874 checks pass (also with `static 0`). Result: 23842 / 23874 at the defaults
    (the 32 scale-changed checks), 23874 / 23874 with `static 0`, 23874 / 23874 with `movingpx 0.086`; r10 sources: S19
    / S20 / S21-defaults fail.
- **Limits.** 15 ids at once (a vehicle and its wheels share one; more than ~10 independently moving
  vehicles on screen leave the rest on the camera R). Wheels get their vehicle's motion, not their spin.
  r2: a mesh drawn more than `mv_objects_max_dup` times is never an object, so wheels of a vehicle model whose wheel
  mesh is drawn 4+ times on screen (and several vehicles of the same model) fall back to the camera R (r3: default 6,
  and such wheels can FOLLOW their vehicle while the wheel mesh is drawn ≤ 8 times; 3+ vehicles of one model with
  4 wheel draws each still fall back; r4: default 3 again, followers off unless `mv_objects_followers = 1`); fast
  vehicles' identical wheels (dup 2-3) can be `ambiguous` (the own motion of ~1 m / frame is larger than the 1.5 x
  margin to the neighbouring wheel) and keep the camera R; identical
  vehicles whose draw order swaps lose their id while it swaps (pairing ≠ identity; r6: the swapped bodies move to each
  other's id after ~3 frames, the veto covers the frames in between); a vehicle passing closer than
  `mv_objects_ego_m` (view depth) loses its id until it is farther away again (r6: only a NEW mover; a vehicle that already
  has its id keeps it; r7: only while it is also camera-locked, i.e. moves less than `mv_objects_lock_px` on screen --
  conversely an own-truck part that sways more than that is treated as a mover; r8: measured at the origin, and a
  vehicle that stays within `mv_objects_ego_m` AND within `mv_objects_ego_px` of one screen position for
  `mv_objects_ego_rb` readbacks -- driving next to you at your speed -- joins the ego set and gets the camera R like the
  own truck until it drifts away; an object whose origin sits at the camera plane (depth 0.0 m in the ids dump) has no
  screen position and is never ego; r9: tracked per copy of its mesh at its reference depth, see "r9"). r6: a vehicle
  that never moves more than 0.5 px against the camera R (far, driving
  straight towards
  / away from the camera) never gets an id; once it has one it keeps it while it is drawn.
  r9: object identities are buffer-pointer based -- when the game's buffers change, pairing survives (fallback key)
  but the tags need ~3 frames to come back; a camera-solve error in translation is not caught by the camera-noise guard.
  r10: a mover is tagged only while its ORIGIN is on screen (viewport + `mv_objects_origin_margin`): a trailer passing
  close alongside or a vehicle at the screen edge loses its tag while its origin is outside even if part of it is
  visible; a static draw whose own noise exceeds `mv_objects_min_px` in coherent readbacks can still found an id of its
  own (never a vehicle's: the attach rule needs the same origin); plates attach 1 readback after their vehicle.
  r11: a vehicle whose own motion stays within 0.5 px of the camera motion in at least half of its last 24..48
  readbacks (stopped, parked, far and slow) is static class and keeps the camera R; after it moves again it needs ~25
  readbacks plus the usual hits to get an id back. Draws whose MVP origin is at the camera (batched / camera-relative
  geometry) are never objects. A forward draw whose origin lies within `mv_fwd_min_m` of the camera is not re-drawn
  (also a camera-relative wire batch, if the game draws one that way).
  `DrawIndexedInstanced` draws (99 in the capture: instanced props / vegetation) are not tagged. Skinned
  characters move with their draw's MVP only (no per-vertex skinning motion). New movers need ~5 frames
  (readback + hysteresis) before they get an id.

### Forward depth for the motion vectors (v0.9.0 r5, `mv_fwd_depth = 1`, default 1)

Symptom (flat ETS2, DLSS 2880x2160 → 3840x2160): overhead / catenary wires (static geometry) leave trails while the
camera moves under them; in DLAA mode they are fine or nearly fine.

- **Not the DLSS setup.** Checked against the SDK rules: MVs are written in render pixels (`mv = (prevUv - uv) *
  FullSize`, FullSize = the scene texture), the feature has `MVLowRes`, MV scale 1 / 1, depth and MVs at render size,
  subrect bases 0 (the inputs are render-sized copies), `InRenderSubrectDimensions` = render size, jitter = the
  viewport shift in render pixels (sign verified by the DLAA self-test). The Halton phase count (11 while upscaling
  x1.333) is not something NGX is told; RCAS runs after NGX on its own target. No unit / scale mismatch.
- **Cause: the wires write no depth.** RenderDoc (`scripts/rdc_forward_depthwrite_audit.py`, numbers in
  `CAPTURE_FINDINGS.md`): 188 of the 190 forward-pass draws test depth but do not write it; the thin wire / cable /
  fence draws are `SRC_ALPHA / INV_SRC_ALPHA` blended. The depth snapshot (scene depth at the discard) therefore holds
  what lies BEHIND a wire: the sky (0 = infinitely far = rotation only, no parallax). Under a bridge the wire is 8-15 m
  away and sweeps across the screen; its pixels got the sky's near-zero motion. DLAA has a fresh sample per output
  pixel and hides most of it; DLSS upscaling leans on history and dilates the MVs by depth (the wire's sky depth never
  wins), so the old wire positions linger.
- **Fix: re-draw them depth-only.** In the world forward pass (1 RTV RGBA16F + scene DSV), after the game's own
  `DrawIndexed`, a draw whose depth-stencil state tests but does not write depth, whose blend state blends RT0 with
  `DestBlend = INV_SRC_ALPHA` ("over" / premultiplied over) and whose viewport is the world layer
  (`MinDepth >= 0.005`, `MaxDepth <= 0.95`) is issued once more with three states swapped for that one call: DSV =
  the pass slot's own forward-depth target (`DepthTwin::fwdDsv`, R32_TYPELESS, cleared to 0 at the pass's first
  re-draw), depth-stencil = depth on / write ALL / `GREATER_EQUAL` / stencil off, blend = alpha-to-coverage with every
  RT write mask 0. The game's RT0 stays bound and receives nothing; the target gets the draw's depth where its pixel
  shader's alpha >= 0.5 (single-sample alpha-to-coverage). The game's RTV + DSV, depth-stencil state + ref and blend
  state + factor + mask are bound back. Pass B then uses `max(snapshot, forward)` per pixel (the nearer surface,
  reversed-Z) for the reprojection AND for the flattened depth NGX reads.
- **Skipped on purpose:** the sky / sun / cloud layer (viewport `[0, 0.009]`: the cloud dome and the sun quad are
  drawn there at 0.2-35 m and must keep the sky's motion), the cabin layer (`[0.9, 1]`: a windscreen must never
  own the world's depth), additive (`DestBlend ONE`: glows, light cones) and multiplicative (`DEST_COLOR / ZERO`)
  blending, everything with depth write on, the G-buffer, the menu / truck-preview passes.
- **Expected side effects (all in the direction of "the visible surface owns the pixel"):** lane paint and road
  decals get their own (road) depth = no change; glass with alpha >= 0.5 owns its pixels (a moving car's glass keeps
  the car's stencil id, so it still moves with the car); thick smoke / dust / rain with alpha >= 0.5 owns its pixels.
  A static wire in front of a tagged moving vehicle keeps that vehicle's object id (the stencil comes from the
  G-buffer) -- rare and small.
- **Cost:** one extra `DrawIndexed` + 6 state calls per qualifying draw (~110 in the audited frame) and one
  `ClearDepthStencilView` per pass, one R32 target per pass slot (4 x 25 MB at 2880x2160). The `MV forward depth`
  log line reports draws / re-draws / skips per pass and the CPU ms.
- **Debug:** `Ctrl+F3` (`key_fwd_depth`) switches it live (A/B), the `Ctrl+F6` MV view tints the pixels whose depth
  came from the forward depth **yellow**. Off for the session (one line) if the scene depth is multisampled or the
  two states cannot be created.
- **r11: near-camera overlays (round-9 log + video).** A world-layer "over"-blended overlay right at the camera --
  windscreen rain drops / dirt / glare / a reflection quad, weather and light dependent -- passes every test above (it
  is in the WORLD layer, not the cabin layer) and wrote near depth (>= 0.9) over the whole glass: pass B then classified
  the road, the verge and the trees seen through it as cabin (blue in the `Ctrl+F6` view, zero motion). Now (1) pass B
  lets the forward depth win only inside the world range [0.01, 0.9) (immediate; the debug tint follows it) and counts
  the near-range pixels (`near-range px`, 1-in-16 estimate); (2) the re-draw skips a qualifying draw whose MVP origin
  lies within `mv_fwd_min_m` (default 0.5 m) of the camera, judged from an asynchronous copy of its MVP and cached per
  draw identity (a new overlay 2-4 passes late; `skipped: near-camera`, the first 5 logged as `MV forward depth:
  near-camera draw`). Limits: the verdict assumes the G-buffer cbuffer layout (MVP at rows 4..7 of VS slot 0); a
  forward batch drawn with the camera-relative view-projection (origin at the camera) is skipped as well; while a
  >= 0.9 overlay is still re-drawn, the forward depth of everything behind it is dropped at those pixels. Details in
  the r11 bullet of "Per-object motion vectors".

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

(v0.10.0 phase 4: in flat DLAA mode the main scene's DLAA now runs earlier, in place on the RGBA16F forward colour at the
scene depth discard -- see "Phase 4" above; the path below stays for DLSS upscaling, VR, HDR output and every frame the
pre-tonemap path does not take.)

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
