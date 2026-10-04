# v0.2 capture findings — ETS2 1.61.1.1, flat, DX11

Capture: `captures/ets2_flat_frame1969.rdc` (RenderDoc 1.46, 2026-10-03, flat mode,
3840x2160 window, `r_scale_x 0.75` → scene renders at **2880x2160**).
Scripts that produced this: RenderDoc python, run with `qrenderdoc.exe --python <script>`.
Shaders are stripped (cbuffer vars are `cb0_vN`), so all identification is by
format / size / bind pattern — never by draw index.

## Frame structure (3060 actions)

| Stage | Targets (render res = 2880x2160 here) |
|---|---|
| Shadow maps | 4096² R16_TYPELESS + 2048² x4 cascades (R16_TYPELESS) |
| Env / reflection probes | 512², 256² cubes, R11G11B10 |
| Mirror views (3 small G-buffers) | 512x256, 512x512, 512x1024 — same 4-MRT layout |
| **Main G-buffer** | RT0..2 `R16G16B16A16_FLOAT`, RT3 `R16G16B16A16_UINT`, DS `D32_FLOAT_S8X24_UINT` |
| Lighting | 2 x RGBA16F, reads G-buffer RT0/RT1/RT3 + shadow maps — **not** the depth buffer |
| Forward / transparents | RGBA16F HDR color (`8591`), depth test on the main DS |
| Bloom / exposure | 720x540 → 360x270 chain, 4x2 + 1x1 luminance |
| **Tonemap** | full-screen Draw → `R8G8B8A8_UNORM_SRGB` at render res (`8609`). Reads HDR color, bloom, G-buffer RT0 |
| **Swapchain blit** | Draw reading that SRGB texture → backbuffer (stretch to window size) |
| UI | 5 more DrawIndexed on the backbuffer, then Present |

## Key facts

- **Reversed-Z.** Depth cleared to 0.0, depth func `GREATER`.
- **Depth partitioning.** Near layer (cabin/interior) is drawn with viewport depth
  range **[0.9, 1.0]**, world with **[0.01, 0.9]**, each with its own projection
  z-row. Reprojection must decide the layer by `d >= 0.9`.
  Cabin pixels move with the camera → MV ≈ 0 is a good first guess for them.
- **Scene depth has no SRV bind** (`DepthTarget` only). We must `CopyResource`
  it into our own `R32G8X24_TYPELESS` twin with SRV bind (same type group).
  Depth stays intact until the next frame's clear, so copying at blit time works.
- **No MSAA, no jitter, `r_aa 0`.**
- **Per-draw VS cbuffer** (one ring buffer, D3D11.1 offsets): rows `v0..v3` =
  object→view (ModelView), `v4..v7` = full MVP (rows x, y, z, w). From MVP/MV:
  `P00 = 0.75`, `P11 = 1.3333` (16:9 FOV; the 4:3 render is a horizontal squash).
  No per-frame view/proj cbuffer found yet (VS declares only cb0).
- `r_scale_x 0.75` means the game already upscales 2880→3840 horizontally with a
  plain stretch. DLSS could do this upscale instead of DLAA-at-1.0 (later option).

## Injection plan (replaces "AA the backbuffer at Present")

Hook `ID3D11DeviceContext::Draw` (MinHook, same as Present). Fingerprint the
**swapchain blit**: render target = current backbuffer AND PS SRV slot 0 =
`R8G8B8A8_UNORM_SRGB` texture smaller/equal to the backbuffer. Just before that
draw runs:

1. Copy the scene DS (last DSV seen with the SRV texture's dims, format
   `D32_FLOAT_S8X24_UINT`) → our SRV twin.
2. Run DLAA on the SRV texture (pre-UI, render res) → our UAV texture → copy back.
3. Let the game's blit draw continue. The UI stays sharp.

v0.3.4: the earlier "tonemap RT (SRGB at scene dims)" trigger was dropped because with the sun on screen the game first draws a sun-shaft occlusion MASK into an SRGB scene-size target, which falsely matched; the backbuffer-blit fingerprint avoids that.

## v0.4 implementation (motion vectors) — what was verified and how it is used

Verified in RenderDoc (ETS2 1.61): the main G-buffer pass draws with `DrawIndexed`; VS cbuffer slot 0
of each draw is a window of one big dynamic ring buffer (D3D11.1 offsets); in that window float4 rows
4..7 (byte 64..127) are the full MVP **as rows** (`clip.i = dot(row_i, float4(pos,1))`). Rows 0..3 look
like a model-view matrix but are NOT exactly consistent with the MVP, so the earlier plan
(`ΔView = MV_prev · MV_curr⁻¹` from rows 0..3, then unproject through `P⁻¹`) was dropped. Instead the
**clip-space R method** is implemented: for a static object seen in both frames,
`R = MVP_prev · inverse(MVP_cur)` maps current clip space to previous clip space for all static things
of that depth layer (the world matrix cancels, no separate P needed). Two layers (viewport depth range
cabin [0.9, 1.0] / world [0.01, 0.9]) each get their own R, chosen per pixel from the depth value; the
medoid over up to 8 matched draws rejects moving objects. See `docs/DLAA_INTEGRATION.md` (motion-vector
section) and `src/motion_vectors.cpp`. Status: built and checked on synthetic matrices; needs an
in-game test (log lines `MV:` incl. the `CB-copy check`).

## VR frame structure (F11 trace, ETS2 `-openxr`, Virtual Desktop, v0.4.3)

Per eye scene size = 4592x6496. Per Present frame, for eye 0 then eye 1, in order:

1. `ClearDepthStencilView(scene depth D32S8 4592x6496)`, `OMSetRenderTargets` n=4 (G-buffer, the same 4 RTs and
   the same depth texture for BOTH eyes), `DrawIndexed...`. Viewport depth ranges: [0.9,1] cabin,
   [0.01,0.9] world, plus a [0,0.009] far/sky layer.
2. Lighting (n=2), forward pass into an RGBA16F HDR target (eye 0 and eye 1 use DIFFERENT HDR textures),
   sun-shaft mask into a scene-size SRGB target, 2296x3248 blur ping-pong.
   -> eye 1's G-buffer pass CLEARS AND OVERWRITES eye 0's depth before eye 0 is tonemapped.

Then, later in the same frame:

3. Tonemap eye 0: full-screen `Draw(3)`, RT = scene-size R8G8B8A8_UNORM_SRGB texture T; then STRETCH BLIT `Draw(3)`:
   RT0 = 6120x6496 R8G8B8A8_TYPELESS eye texture E0, PS SRV0 = T.
4. Bloom + tonemap eye 1 into the SAME T, then stretch blit `Draw(3)` into a different 6120x6496 eye texture E1.
5. Final `Draw(4)` x2 into the OpenXR swapchain images (3072x3264 B8G8R8A8_UNORM_SRGB) reading the 6120x6496
   textures; then `Present` of the 2560x1440 mirror window (the mirror is NOT the scene).

**Eye order is FIXED (corrected v0.5.4; the earlier "alternates per frame" note was WRONG).** ETS2 VR never swaps the
eyes. The blit render target of each eye is an OpenXR swapchain image: the FIRST blit of every frame always targets one
of 3 textures {A1,A2,A3} (cycling frame by frame), the SECOND blit always one of 3 other textures {B1,B2,B3}. Proven by
a 12-frame F11 trace + F10 dump in VR. The apparent alternation seen in v0.5.1 (R_world row0 ~ [1,0,+-0.29,+-0.48],
0.48 = 2x0.2425 = difference of the two eyes' asymmetric P[0][2]) came from the Present-boundary bug (frame/pass
bookkeeping keyed to the mirror-window Present, which is not a frame boundary), not from the game: two consecutive
"eye 0" frames of v0.5.3 carried ~1260 px motion vectors = the projection offset between the eyes. Passes and blits
can interleave either as P P B B or P B P B within a frame (both seen); the FIFO handles that. v0.5.4 therefore takes
eye identity from the blit RT (ptr -> eye map; an unseen RT is assigned the blit index within its frame pair) and
keeps depth snapshot + MV candidates per PASS until that blit.

In flat mode the stretch blit's RT is the DXGI backbuffer (3840x2160) and there is 1 eye per frame.

How v0.5.0 uses this: the blit trigger (3/4-vert Draw, SRV0 R8G8B8A8 at scene dims, RT0 backbuffer or
RGBA/BGRA >= scene size and not scene-sized) fires once per eye at steps 3 and 4 (the sun-shaft mask draws and the
OpenXR-swapchain draws do not match); eye 0's depth must be snapshotted in the `ClearDepthStencilView` hook
because step 1 of eye 1 destroys it before step 3; T is shared, so the DLAA output is written back over T right
before each blit reads it. Far/sky layer (d < 0.01) falls out of the reprojection shader's `saturate` as
z_ndc = 0 (infinitely far, rotation only). Unverified in VR until a run confirms the log lines
`eye blit detected: eye=0/1` and `eye depth snapshot`.

## v0.5.2: pass -> blit FIFO instead of Present-based frames

The mirror-window Present is not a reliable frame boundary in VR (v0.5.1 log: blit #0 with `depth=live`, i.e. the other
eye's depth). The reliable structure is the order of GPU work itself: all G-buffer passes of a frame come first, then
all blits, in the same order (`[clear,A][clear,B][blit A][blit B]`, flat `[clear,pass][blit]`). v0.5.2 therefore keeps a
FIFO of passes started at each scene-depth clear; each blit consumes the oldest unconsumed pass and takes eye identity,
jitter and depth source (snapshot taken when the next pass's clear arrived before the blit, else live) from it.
Unverified in VR until a run shows `FIFO stats` with `fifo underflow=0 fifo overflow=0` and `live used` only for the
last pass of each frame.

- **v0.5.3 (proven from an F10 VR snapshot):** the game calls `ID3D11DeviceContext1::DiscardView()` on the scene depth after each eye pass; the content is undefined afterwards (~25% garbage speckle: negative/denormal/NaN in what NGX got), so the depth must be snapshotted BEFORE the discard (hooks: DiscardResource 117, DiscardView 118, DiscardView1 133).

- **v0.5.4:** eye identity is authoritative from the blit render target (see "Eye order is FIXED" above). Each pass slot owns its depth twin and MV candidate record; at the blit the eye's NGX feature / CameraMv matches the slot's record (cur) against the eye's last committed record (prev) and commits it. Alternation hypotheses, the parity formula, VrEyeCheck and `vr_eye_alternate` are gone. Why VrEyeCheck (v0.5.1) never fired: it needed a streak of 4 CONSECUTIVE mismatching R_world readbacks on BOTH eyes, but the wrong pairing happened only every other frame, so every matching frame reset the streak to 0 (and the 2-3 frame old async readbacks interleave good/bad frames further). Diagnostic now: per eye, the count of R_world readbacks with |R[0][3]| > 0.25 (logged in `FIFO stats`; expected ~0).
