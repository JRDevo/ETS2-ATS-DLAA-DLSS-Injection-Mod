# ETS2 & ATS: DLAA & DLSS Injection Mod — development notes

Internals and history of the injector. For installing and using the mod, see the
top-level [`README.md`](../README.md). Companion docs:

- [`DLAA_INTEGRATION.md`](DLAA_INTEGRATION.md) — NGX/DLSS integration, jitter and motion-vector details.
- [`CAPTURE_FINDINGS.md`](CAPTURE_FINDINGS.md) — RenderDoc findings about the ETS2/ATS frame (buffers, passes, VR eye structure).
- [`KEYS.md`](KEYS.md) — the hotkey cheat sheet.

A from-scratch DLAA anti-aliasing injector for Euro Truck Simulator 2 and
American Truck Simulator (Prism3D engine, DirectX 11.1), targeting both flat and
VR (OpenXR, e.g. Virtual Desktop).

> **v0.7.7 (re-bindable hotkeys):** every hotkey is an action with a binding in `dlaa.ini` (`key_mode_cycle`,
> `key_model_1..4`, `key_area_down/up`, `key_sharpen_down/up`, `key_width_down/up`, `key_dlaa_toggle`, `key_save`,
> `key_upscale_toggle`, `key_mv_toggle`, `key_mv_debug`, `key_passive`, `key_jitter_only`, `key_selftest`,
> `key_snapshot`, `key_trace`, `key_jitter_sign`; value `[Shift+][Ctrl+][Alt+]Key` or `none`, defaults = the old keys).
> Same rules as before (down-edge, focused game, exact modifiers, top-level Present); one per-VK edge table
> (`PollKeys`) replaces the F-key-only one. Bad values log a warning and keep the default; duplicate combos warn
> at load. See [`KEYS.md`](KEYS.md).
>
> **v0.7.3 – v0.7.5 (after the status below):** v0.7.3 flat: a blit RT with exactly the backbuffer's size and
> format is the backbuffer (the `GetBuffer(0)` pointer of the last Present is not always the texture the game
> blits into; the blit was taken for a VR eye and the mod stuck in VR mode). v0.7.4: flat / VR comes from the
> launch options at load (`-openxr` / `-oculus` = VR; log `launch mode: VR|FLAT`); flat never
> switches to VR and always uses `dlaa_area` 100 (outside the area the image is the raw jittered frame, which
> flickers on a monitor); VR always ignores the backbuffer (mirror). v0.7.5: the log file is only written with
> `debug = 1` in `dlaa.ini` (`src/log.h`, checked once at the first `Log` call).
>
> **Status: v0.7.2 — real DLSS upscaling (render res < output res) on top of the v0.6.5 per-eye DLAA (VR /
> OpenXR + flat), plus the plain-`End` mode cycle (DLAA → DLSS → off, saved to `dlaa.ini`).** v0.6.5 runs in
> VR; the v0.7.x upscale path is built but has not yet been run in the game.
> With `dlss_upscale = 1` (default) and the game's `r_scale_x` / `r_scale_y` below 1, NGX renders from the
> scene size to the eye texture / backbuffer size and the result is composited into the eye image after the
> game's own blit (see "DLSS upscaling" below). Equal sizes or `dlss_upscale = 0` = the v0.6.5 DLAA
> path, unchanged. One `SceneDlaa` per eye (own NGX DLSS feature, textures, motion-vector state); the eye blit
> is detected by shape (see "VR / per-eye" below). Hooks: `OMSetRenderTargets`, `RSSetViewports`,
> `DrawIndexed`, `Draw`, `ClearDepthStencilView`, the `Discard*` calls, `Present`. Motion vectors are
> camera-reprojection (see "Motion vectors" below).

---

## Why this architecture

Written from scratch against the public NVIDIA DLSS SDK and RenderDoc captures of
the game's own frame. It is built on:

- **NGX / DLSS SDK** (`NVSDK_NGX_*`, preset `DLSS.Hint.Render.Preset.DLAA`) —
  DLAA = DLSS at native scale (ratio 1.0).
- **Reconstructed motion vectors** — the engine has **no native velocity buffer**, so
  MVs are computed. This is the hard 80%, and the part most likely to need work
  after a game update.
- **D3D11 / DXGI hooks** (MinHook) and hotkeys with beeps for tuning; no overlay.
- **No OpenXR calls** — VR is handled by hooking at the **D3D11
  render-target level**, so each eye's pass flows through the same DLAA path.

### Two deliberate choices

1. **Loader = `dinput8.dll` proxy, not a `dxgi.dll` proxy.** A `dxgi.dll` proxy
   must also forward DXGI's internal *ordinal* exports that `d3d11.dll` imports,
   or the game crashes at launch. ETS2/ATS already load `dinput8.dll` (only 5
   simple exports to forward), so it's a clean, low-risk vector. We still get "DXGI injector" behavior by
   hooking `IDXGISwapChain::Present`.

2. **`Present` is the flat path only.** In VR, `Present` carries the desktop
   **mirror** window, *not* the per-eye images submitted to the runtime. DLAA in
   VR must run on the **eye render targets** — a device-context-level hook (v0.5),
   not `Present`. v0.1 deliberately proves the simpler hook first.

---

## Pipeline (target)

```
game frame -> [Present / RT hook]
                 grab: color RT (pre-UI), depth buffer
                 compute: motion vectors   <-- the hard part
                 NGX DLAA (scale 1.0, preset DLAA)
                 write AA'd color back -> HUD on top -> real Present
```

For ETS2/ATS the world is mostly **static** and you fly a camera through it, so
**camera-only reprojected MVs** (depth + prev/cur view-projection) get ~80%
quality on the first pass — only traffic, wheels and your own body ghost. Good
enough for an MVP; dynamic-object MVs are polish.

---

## Build

Requires Visual Studio 2022 (MSVC, x64) + CMake ≥ 3.20. MinHook is fetched automatically by CMake (needs git + internet on first configure).

DLAA is compiled in automatically when `external/DLSS` exists (a clone of the
public NVIDIA SDK, git-ignored for license reasons; tested with DLSS SDK 310.9.1):

```bash
git clone --depth 1 https://github.com/NVIDIA/DLSS external/DLSS
```

Without it the build is the logging-only injector (`-DWITH_DLAA=OFF` forces that).

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/Release/dinput8.dll`.

v0.10.0 WARP harness for the per-draw motion vectors (no game needed): `tests\build_drawid_harness.bat` builds
`build\tests\drawid_harness.exe` with `cl` (Visual Studio 2022) from `tests/drawid_harness.cpp` + the injector's
`motion_vectors.cpp`, `draw_ids.cpp`, `shader_cache.cpp` (phase 7: + `scene_dlaa.cpp` and `tests/fake_dlaa.cpp`, a stand-in
NGX). Run it with no arguments (all scenes) or `[scene 1..12] [snap
px] [debug | nocons | nofwd | noattach | notwin | noinherit | noparent | parentconj | nodepth]`; exit code 0 = every check
passed (77804 checks at v0.10.0 phase 6: S1-S4 46740 + the mirror scene S5 9277 + the consensus / forward-id scene S6 15895 +
the plate / attach / twin scene S7 3687 + the rigid-parent scene S8 2205; 81011 checks at phase 6c: S1-S7 unchanged, S8 2706
(+ the far 24x8 px plate, instanced background + text, bottom-edge plate) and a second S8 run with a 6 px march reach, "S8 short
reach", 2706 (plate text only through the 2nd pass); 81033 checks at phase 7: S1-S8 unchanged + the pre / post-tonemap stage
scene S9 22 (DlaaStage hysteresis, SceneDlaa's two colour sets, the per-pass flow); 81041 checks at phase 8: S1-S9 unchanged
(with the phase-8 defaults) + the pass-slot texture pool S10 8; phase-8 knobs from the 3rd argument on: `legacy` (every phase-8 cut
off = the phase-7 behaviour), `vres=2|4`, `cap=N`, `cands=N`, `drop=N`, `inst=0|1`, and the COMPARISON mode `cmpdump=FILE` (write
every frame's motion vectors) / `cmp=FILE` (compare with them, per pixel: run the reference with `legacy`); every scene also prints
its phase-8 counters and a "WARP perf" line (per-sub-pass timers on WARP = CPU figures, not GPU numbers); 100066 checks at phase 9:
S1-S10 unchanged (bit-identical motion vectors to the phase-8 binary's `cmpdump` with the folded dispatches) + the view unit
checks 10 + S11 16102 (= S6 + two SHADOW pipelines: the DLAA-area clip around an off-centre optical centre, full / 1/2-resolution
forward depth, compared with the whole-image pipeline every frame) + S12 2913 (= S8 + the same shadows); phase-9 knobs: `nofold`
(the phase-8 chain of separate dispatches), `mutclip` / `mutview` (S11 / S12 mutations: the G-buffer replay without its clip / the
1/2-resolution forward re-draw without the halved viewport -- they must FAIL); `debug` = under the D3D11 debug layer; `nocons` /
`nofwd` = S6 with the consensus camera R / the forward ids switched off, `noattach` = S7 with mv_fwd_attach_m = 0, `notwin` =
S7 with mv_drawid_twin = 0, `noinherit` = both (these three also switch the rigid parents off), `noparent` = S8 with
mv_drawid_parent = 0, `parentconj` = S8 with the row-vector formula typed into the column-vector code, `nodepth` = S8 without
the vote's depth test -- mutations, they must FAIL); 100686 checks at phase 10: S11 / S12 gain three BATCH shadows each (+620
checks: the forward depth only recorded during the forward pass and re-drawn in one go after the snapshot, as inject.cpp's
FwdBatch -- full resolution without the occlusion init must be bit-identical to the interleaved re-draw, full / 1/2 resolution WITH
the init (`DrawIdRecord::InitFwdDepth`) must keep every RED id and motion vector and the GREEN ids where the forward depth won);
phase-10 knobs: `listed=N` (mv_parent_max_listed), `medium` / `low` (the per-draw parts of those perf profiles: vote res 4, march
cap 24000, mv_inst_replay 1, listed 64 / 32 -- informational, `inst=1` alone already changes S8's instanced-plate checks).
Phase 14 (static replay skip): `rstatic=E` runs S1-S4 and the new S13 (S1 with every vehicle parked until frame 40, then
accelerating) through the static gate -- skipped draws must own no id pixel, keep the camera R and their state, a skipped MOVER
may keep the camera R for at most 2 frames in a row, and the gate must skip something; S13 also runs in the plain set (111238
checks without rstatic). S6, S7 and S8 (consensus convoy, plates by twin / attach, view-space + instanced plates on a turning
trailer that moves from frame 0) also run gated (their own id / parent / MV checks + "the gate must skip something"). `cmpdump`
without / `cmp` with `rstatic=4`: S1-S4 and S6-S8 0 px differ; S13 785 px in 2 frames (max 0.24 px: the start lag). Run this
comparison after any change to the gate or the vote.
Phase 18 knobs: `fold=0|1|2` (mv_fold_dispatch; `nofold` = 0), `nocompact` (mv_vote_compact 0), `cabmin=N` (mv_cons_min_cabin) and
`hw` (the hardware adapter instead of WARP: the per-scene perf line then shows real GPU ms; the checks still pass on an RTX 5090).
Phase 19: scene 17 = S17 (car, the interior's big draws with their MVP one cb0 row later, the in-game signature) + S17b (car with a
valid interior: one big body draw against a 2-draw swinging tag); knobs `cabsmall=N` (mv_cabin_small, 0 = off) and the mutation
`mutshape` (the MVP-shape rule off, here and in pass A). Full run 134981 checks (133103 + S17 1184 + S17b 694).
What it covers: `docs/DLAA_INTEGRATION.md`, "Per-draw motion vectors
(v0.10.0)" (+ "Phase 3", "Phase 4", "Phase 5", "Phase 6", "Phase 6b", "Phase 6c", "Phase 9", "Phase 10") and "Mirror units (v0.10.0)". The pre-tonemap DLAA (phase 4 job B) is
inject.cpp plumbing around SceneDlaa::Run and is not covered by the harness (game only); phase 7's stage logic and SceneDlaa's
two colour sets are (S9), the forward-leave / discard hooks are not.

The version string in the load-log line comes from `project(... VERSION x.y.z)` in
`CMakeLists.txt` via the `DLAA_INJECTOR_VERSION` compile definition — bump it there
(and in the doc headers) only.

## Install / test

1. Back up first. Locate the game exe folder, e.g.
   `<game>\bin\win_x64\`.
2. Copy `dinput8.dll` there. (Remove any other mod's `dinput8.dll`/`dxgi.dll` first —
   only one input proxy can win.)
3. **Flat test:** launch the game normally. A `dlaa_inject.log` should appear in
   `bin\win_x64\` containing `injector v0.7.2 loaded`, five `hook installed` lines
   (Present, OMSetRenderTargets, RSSetViewports, DrawIndexed, Draw; plus the trace hooks and
   ClearDepthStencilView), `MV: ready`, `MV: first candidates collected`, `MV: CB-copy check` (the 16 floats
   must not be all zero), `jitter pass: ...` lines, `scene depth G-buffer pass`, `swapchain blit detected`,
   `eye blit detected: eye=0`, `DLAA: NGX feature created for eye 0`, and a `DLAA heartbeat` every 600 frames.
   DLAA also needs `nvngx_dlss.dll` (the game folder already ships one; the SDK's is
   in `external/DLSS/lib/Windows_x86_64/rel/`). To test with DLAA bypassed (detect +
   log only) create an empty `dlaa_off.txt` next to `dinput8.dll` before launch (DLAA then
   starts OFF; `Shift+F11` can still switch it on). The log is opened share-read, so
   you can tail it while the game runs.
4. **VR test:** add `-openxr` to the Steam launch options,
   start Virtual Desktop, launch. Confirm `Present` still fires (its resolution is the
   mirror window, expected). With v0.5.0 the log should show `eye blit detected: eye=0` and `eye=1`
   (src = scene size, RT = 6120x6496 eye texture), `eye depth snapshot: eye=0 captured ...`, two
   `NGX feature created for eye N` lines, the `frame #n summary` lines for the first Presents, and
   `DLAA GPU cost eye N` every 600 frames. See "VR / per-eye".

If the game fails to start, the most likely cause is `bin\win_x64\` being
read-only (Steam verify) or an antivirus quarantining an unsigned DLL.

---

## Controls / config

**v0.7.2 hotkeys** (scheme since v0.6.5; v0.7.0 adds `Ctrl+F4`, v0.7.2 adds the plain-`End` mode cycle). Apart
from `End`, every control is an F-key with exactly one modifier (the mod is operated blind in a VR
headset, so the scheme is uniform): **Shift + F-key = user keys**, **Ctrl + F-key = debug keys**. The modifier
must match exactly — a bare F-key, the wrong modifier, or any combination that also holds Alt does nothing. All
keys require the game window to be focused and fire on the key's down-edge. (See [`KEYS.md`](KEYS.md) for a printable
cheat sheet.)

### User keys — Shift + F-key (Ctrl and Alt NOT held)

| Key | Action | Feedback |
|-----|--------|----------|
| `End` (no modifier, v0.7.2) | Mode cycle DLAA → DLSS → off; saved to `dlaa.ini` key `mode` (`1` / `2` / `0`) at once and used at the next start. DLAA = NGX at render res + game stretch (`dlss_upscale` off), DLSS = NGX upscales to the eye texture. The render size is the game's `r_scale`; at 100 % both modes give the same picture | 1 × 1200 Hz = DLAA, 2 × 1200 Hz = DLSS, 300 Hz = off |
| `Shift+F1` | DLAA model 1 = preset `default` (driver pick) | 1 × 880 Hz |
| `Shift+F2` | DLAA model 2 = preset `E` | 2 × 880 Hz |
| `Shift+F3` | DLAA model 3 = preset `F` | 3 × 880 Hz |
| `Shift+F4` | DLAA model 4 = preset `M` | 4 × 880 Hz |
| `Shift+F5` | DLAA area −10 % (10..100; 40..100 before v0.10.0 phase 15, 20..100 until phase 24) | 3 beeps at `300 + 6·area` Hz; low 200 Hz at the 10 limit |
| `Shift+F6` | DLAA area +10 % | 3 beeps at `300 + 6·area` Hz; low 200 Hz at the 100 limit |
| `Shift+F7` | Sharpen strength −0.1 (0..1) | 1 beep at `400 + 800·sharpness` Hz; low 200 Hz at a limit |
| `Shift+F8` | Sharpen strength +0.1 | 1 beep at `400 + 800·sharpness` Hz; low 200 Hz at a limit |
| `Shift+F9` | Sharpen width (RCAS radius) −0.5 (1..4) | 2 beeps at `400 + 250·radius` Hz; low 200 Hz at a limit |
| `Shift+F10` | Sharpen width +0.5 | 2 beeps at `400 + 250·radius` Hz; low 200 Hz at a limit |
| `Shift+F11` | DLAA on/off (jitter + evaluate) | 1200 Hz = ON, 300 Hz = OFF |
| `Shift+F12` | **Save** `dlss_preset` / `dlaa_area` / `sharpness` / `sharp_radius` / `dlss_upscale` to `dlaa.ini` | rising 600 → 900 Hz (120 ms each) on success; one long 200 Hz (400 ms) on failure |

Model select is a **direct** pick (replaces the old cycle): selecting the model that is already active just
re-beeps and does **not** recreate the NGX feature. A real change recreates both eyes' NGX feature and resets
the DLSS history; if NGX cannot create the preset it falls back to `default` (long low tone + 1 beep). Area
changes recreate each eye's crop textures + NGX feature at the next blit (one hitch) and reset its history.
**Save** rewrites `dlaa.ini` in place, preserving every other line (comments, blanks, other keys); it writes to
`dlaa.ini.tmp` and atomically replaces the original, so a failed write cannot destroy the file. After a preset,
area, sharpness or radius change the GPU timing windows restart (the next `DLAA GPU cost eye N` line after 240
timed frames, and one `rate after change: ...` line 300 blits later).

### Debug keys — Ctrl + F-key (Shift and Alt NOT held)

| Key | Action | Feedback |
|-----|--------|----------|
| `Ctrl+F4` | DLSS upscale on/off (v0.7.0; off = DLAA at render res, the v0.6.5 path). Both eyes rebuild their textures + NGX feature at the next blit, history reset | 1200 Hz = ON, 300 Hz = OFF |
| `Ctrl+F5` | Motion vectors on/off (off = zero MVs) | — (log + DLSS history reset) |
| `Ctrl+F6` | MV debug view (blit source = `(0.5+mv.x/16, 0.5+mv.y/16, cabin?1:0)`; gray = still, blue = cabin; needs DLAA on; v0.7.0 upscale: drawn by the composite, render-size MVs scaled up to output size; v0.9.0 `mv_objects=1`: magenta = pixel of a stencil object id with its own R this frame, orange = id without a usable R (camera R used; r4: no trusted non-spinning member this frame, or vetoed: a tagged draw contradicted the id's motion); v0.9.0 r2: cyan = identity rejected by the r2 rules in the last readbacks (stencil id 15 reserved while the view is on, camera R)) | — |
| `Ctrl+F7` | Passive mode on/off (no jitter / MV copies / depth snapshot / evaluate / readbacks; tracking + GPU timers only) | 1 low tone = ON, 2 = OFF |
| `Ctrl+F8` | Jitter-only debug (jitter on, DLAA evaluate skipped = raw jittered image) | — (DLSS history reset) |
| `Ctrl+F9` | Self-test (cycles OFF, the 4 jitter signs, jitter-only; dumps lossless BMP frames to `dlaa_selftest\`; v0.7.0 upscale: the DLAA modes dump the DLSS output at output res, OFF / jitter-only the raw render-res frame) | — |
| `Ctrl+F10` | NGX input snapshot (2 frames to `dlaa_snap\<n>_blit<blit>\` (one folder per press, phase 24b): color-in / out BMP, MV / depth `.bin`, info txt; needs DLAA on; v0.7.0 upscale: `out` = the DLSS output rect at output res) | — |
| `Ctrl+F11` | Frame trace (~2 frames of RT binds / viewports / draws / copies / clears to `dlaa_trace.txt`) | — |
| `Ctrl+F12` | Cycle NGX jitter sign (−1,−1) → (+1,+1) → (+1,−1) → (−1,+1) | — (DLSS history reset) |

The self-test dumps eye 0 only in VR (~89 MB per BMP at 4592×6496); snapshots are several GB (both eyes,
`_e0`/`_e1` suffix). The frame trace also fires once automatically when the Present count reaches
`trace_auto_frame`. Same resource pointer in the trace = same texture.

- **`dlaa.ini`** (optional, next to `dinput8.dll`, `key=value` lines, `#` or `;` comments; written by `Shift+F12` / `End`):

  | key | default | meaning |
  |-----|---------|---------|
  | `jitter_enabled` | `1` | `0` = never jitter (DLAA then only temporally smooths) |
  | `jitter_phases`  | `8` | length of the Halton(2,3) jitter sequence (v0.5.7: was 16) |
  | `jitter_sign_x`  | `-1` | sign applied to the X offset handed to NGX (`-1` or `1`) |
  | `jitter_sign_y`  | `-1` | sign applied to the Y offset handed to NGX (`-1` or `1`) |
  | `mv_enabled`     | `1`  | `0` = zero motion vectors (start state of `Ctrl+F5`) |
  | `mv_near_reject_m` | `30` | float metres; world pairs whose object-origin view depth is below this are excluded from the medoid (own truck/trailer move with the camera); ignored if it would leave no pair |
  | `mv_ego_origin_m` | `8` | float metres 0..30 (v0.6.0); world draws whose object origin is closer than this are ego candidates (own truck exterior) |
  | `mv_ego_pixel_m` | `3` | float metres 0..20 (v0.6.0); world-layer pixels closer than this use the ego reprojection `R_ego`; `0` = off |
  | `mv_sample_shift` | `2` | int 0..4 (v0.6.3); a G-buffer draw is an MV candidate only when `hash(indexCount,startIndex,baseVertex) & ((1<<shift)-1) == 0` (same draws every frame); `0` = every draw (old behaviour), `2` = ~1/4 |
  | `mv_world_slots` | `40` | int 1..128 (v0.6.3); world (layer 0) MV-candidate cap per pass (replaces the old 128 limit) |
  | `mv_cabin_slots` | `24` | int 1..128 (v0.6.3); cabin (layer 1) MV-candidate cap per pass (replaces the old 128 limit) |
  | `gpu_timing` | `-1` | `-1` = GPU timestamp queries around each eye's DLAA work in VR only, `0` = off, `1` = on (flat too); logs avg ms per eye every 600 frames. v0.6.2: also the whole-frame `GPU spans [...]: scene .. ms/frame \| gbuf .. ms/pass` line every 300 frames, with DLAA ON, OFF or passive |
  | `trace_auto_frame` | `0` | auto-run the frame trace once when the Present count reaches this; `0` = off (`Ctrl+F11` always works) |
  | `dlss_preset` | `default` | DLSS render preset for DLAA (`default`, `J`, `K`, `L`, `M`, `E`, `F`); models 1–4 (`Shift+F1`..`F4`) select `default`/`E`/`F`/`M`; written by `Shift+F12`. v0.7.0: applied to every DLSS quality mode (all hint parameters), so it also picks the model while upscaling |
  | `sharpness` | `0.4` | RCAS sharpen strength after DLAA, 0..1 (`0` = pass skipped); `Shift+F7`/`F8` adjust; written by `Shift+F12` |
  | `sharp_radius` | `1.5` | RCAS ring-tap radius in texels, 1..4 (bilinear taps); `Shift+F9`/`F10` adjust (v0.5.8); written by `Shift+F12` |
  | `dlaa_area` | `100` | int 10..100 (v0.6.4; 40..100 before v0.10.0 phase 15, 20..100 until phase 24): DLAA runs on a rect of this % of each eye image's width AND height (pixel count ~ area²), centred on the eye's optical centre; `100` = whole image (v0.6.3 behaviour); `Shift+F5`/`F6` adjust; written by `Shift+F12` |
  | `dlaa_area_feather` | `96` | int 0..512 px (v0.6.4): blend ramp from the raw frame to the DLAA result at the rect edges that lie inside the image; `0` = hard edge (render px; v0.7.0 upscale: scaled per axis to output px) |
  | `dlss_upscale` | `1` | `0`/`1` (v0.7.0): DLSS upscaling from the scene size to the eye texture / backbuffer size whenever that is larger (see "DLSS upscaling"); `0` = always DLAA at render res (v0.6.5 path); `Ctrl+F4` toggles; written by `Shift+F12` |
  | `mode` | *(absent)* | `0`/`1`/`2` (v0.7.2): start mode written by the plain `End` key — `1` DLAA, `2` DLSS, `0` off. When present (and no `dlaa_off.txt`) it wins over `dlss_upscale` at startup. `End` cycles and rewrites it |
  | `beeps` | `1` | `0` = no audible feedback for the hotkeys |
  | `debug` | `0` | v0.7.5: `1` / `true` / `yes` / `on` = write `dlaa_inject.log`; default off (no file is created) |

  The values in use are logged at startup. If the image shimmers or looks doubled
  with DLAA on, try flipping the signs (4 combinations).

## DLSS upscaling (v0.7.0)

The game's render-scale cvars `r_scale_x` / `r_scale_y` shrink only the **scene** texture (what the eye blit
reads); the eye texture (VR) / backbuffer (flat) keeps its size and the game's blit stretches the scene into it
bilinearly. So "scene size vs blit target size" *is* the render scale. With `dlss_upscale = 1` (default) and a
blit target larger than the scene in at least one axis, NGX renders **from the scene size to the blit target
size** (real DLSS super resolution) and the game's bilinear stretch is replaced by the DLSS result. With equal
sizes, or `dlss_upscale = 0` / `Ctrl+F4` off, it is the v0.6.5 DLAA path, unchanged.

Set **both** `r_scale_x` and `r_scale_y` in `Documents\Euro Truck Simulator 2\config.cfg`
(game closed), e.g. `uset r_scale_x "0.667"` and `uset r_scale_y "0.667"`:

| DLSS mode | `r_scale_x` = `r_scale_y` | output / render per axis | pixels rendered |
|-----------|---------------------------|--------------------------|-----------------|
| Quality | `0.667` | 1.5 | 44 % |
| Balanced | `0.58` | 1.72 | 34 % |
| Performance | `0.5` | 2.0 | 25 % |

DLAA without upscaling = `dlss_upscale = 0` (or `Ctrl+F4` off) at any `r_scale`. v0.7.1: VR at
`r_scale_x = r_scale_y = 1` (eye texture == scene size) now gets DLAA too. A scene-sized blit is accepted when its
source is the texture the last tonemap draw (RGBA16F scene -> RGBA8 scene-size RT) wrote and its RT is a different
texture. Not yet run in the game: if the game skips the blit at 100 % (tonemap straight into the eye texture) the
log shows no `eye blit detected` line and DLAA stays off.

The mode is picked automatically from the size ratio (per-axis geometric mean, nearest of 1.3 UltraQuality /
1.5 Quality / 1.7 Balanced / 2.0 Performance / 3.0 UltraPerformance); anisotropic scales work too (e.g.
`r_scale_x 0.75`, `r_scale_y 1` = ratio 1.33 × 1.0, geomean 1.15 → UltraQuality; if NGX rejects UltraQuality
the feature is retried once as Quality). The `Shift+F1..F4` model choice applies in every mode.

- **How it is drawn.** colorIn, depth and motion vectors stay at render size (MVs in render pixels, NGX flag
  `MVLowRes`); the NGX output and the RCAS sharpen run at output size (sharpen width = output texels). Nothing is
  copied back over the scene texture: the game's own blit runs first (that is the picture outside the DLAA area),
  then our fullscreen-triangle VS + PS draws the DLSS result into the same render target, viewport = the DLAA rect
  in output space, alpha-blended with the `dlaa_area_feather` ramp (scaled to output px; alpha 1 everywhere at area
  100), RGB only (the target's alpha is untouched). If the game's blit reads the scene through an `_SRGB` view the
  PS decodes to linear first, so the result matches the game's blit in brightness. Every piece of graphics state
  the draw changes is saved and restored.
- **Jitter.** Halton phases while upscaling = `max(jitter_phases, round(jitter_phases × output/render pixels))`
  (NVIDIA: 8 × ratio²), e.g. 8 → 18 at Quality; jitter stays in render pixels.
- **Fallback.** If NGX cannot create the upscale feature for a size, that size falls back to DLAA at render res
  (log line + one long low tone); `Ctrl+F4` off/on retries.
- **Debug views.** `Ctrl+F6` MV view: render-size MVs scaled up to the output rect and drawn by the composite.
  `Ctrl+F10` snapshot: `color_in` / `mv` / `depth` are render-size (crop), `out` is the DLSS output rect at output
  res (not the blit source); the info txt has `upscale`, `render_size`, `output_size`, `output_rect_origin`,
  `output_rect_size`, `dlss_quality`. `Ctrl+F9` self-test: the DLAA modes dump the DLSS output (output res), OFF /
  jitter-only dump the raw render-res frame, so the files of one run have two sizes.
- **Logs.** `DLSS upscale ACTIVE at blit #N: render (scene) 4592x6496 -> output 6120x6496 at (0,0) in RT 6120x6496
  ... | blit SRV0 view fmt=.. | RTV view fmt=..` (once, and on every change), `DLAA: ready -- 4592x6496 -> 6120x6496
  (DLSS UltraQuality, ratio x1.333 / y1.000, MVLowRes), preset=...`, `DLAA: NGX feature created for eye N -- DLSS
  UltraQuality upscale: render ... -> output ...`, `DLAA upscale rect: eye N render (x,y) WxH ... -> output (x,y)
  WxH ...`, `DLAA upscale composite: eye N first draw -- ...`, `jitter: 11 Halton phases now ...`. The GPU cost line's
  last stage is `composite` (our draw only, the game's blit excluded), and every `[preset=.. area=..]` tag ends in
  `up=4592x6496->6120x6496` (or `up=off`); the `config (...)` line has `dlss_upscale=`.

## DLAA area (v0.6.4)

In the headset only the centre of each eye image is seen sharply; the lens blurs the outer part and some of it is
not visible at all. Every DLAA stage scales with the pixel count, so `dlaa_area` (or `Shift+F5`/`F6` live) limits
the whole pipeline -- copy-in, depth + motion vectors, NGX, sharpen, copy-back -- to a rect of `area` % of the
width and of the height (each rounded to a multiple of 8). At 70 % that is 49 % of the pixels.

- **Centre.** The rect is centred on that eye's optical centre `uv = (0.5 - p/2, 0.5 + q/2)`, with `p` / `q` the
  projection's x / y skew, read from the same async MVP readback that verifies the eye map (mean of the last 16
  readbacks). Logged once per eye: `DLAA area: eye N optical centre uv=(u, v) from p=.. q=..`. Until it is known
  (and always in flat mode) the image centre is used. It is only re-adopted when it moves by more than 0.01 (a rect
  move resets that eye's DLSS history). The rect is clamped into the image; its origin is an even pixel.
- **Border.** At the rect edges that lie inside the image the result is blended into the raw frame over
  `dlaa_area_feather` px (default 96; smoothstep ramp per axis, the two axes multiplied). For this the sharpen pass
  always runs while `area` < 100, also at sharpness 0 (it then only blends). **Outside the rect the image is the raw
  jittered frame** (no anti-aliasing there).
- **Logs / snapshot (`Ctrl+F10`).** `area=N` is part of the `[preset=.. sharp=.. r=..]` tags (GPU cost, GPU spans, rate lines) and of
  the `config (...)` line. `DLAA: NGX feature created for eye N -- WxH at rect origin (x,y) of WxH (area=N%)`.
  The snapshot `color_in` / `mv` / `depth` files are crop-sized; `out` is still the whole blit source (shows the border);
  the info txt has `dlaa_area`, `rect_origin`, `rect_size`, `full_size`, `optical_centre_uv`.

## Jitter

DLSS needs each frame rasterized at a different sub-pixel offset. ETS2/ATS do not
do that, so v0.3.1 injects it: `ID3D11DeviceContext::RSSetViewports` is hooked and,
during the scene G-buffer and forward passes only, the first viewport's
`TopLeftX/Y` is shifted by a Halton(2,3) offset in [-0.5,+0.5) px (D3D11 viewports
take floats, so this jitters rasterization exactly, with no cbuffer patching). The
same offset (times the configured sign) is passed to NGX. v0.5.0: the Halton phase advances once per
Present frame, so both VR eyes use the same phase. See [`DLAA_INTEGRATION.md`](DLAA_INTEGRATION.md).
v0.10.0 phase 23: the shifted world / mirror viewport is issued 1 px larger on the right and bottom (`jitter_overscan`, default
1): with the game's W x H a negative shift left the last pixel column / bottom row unrendered (see "Phase 23" below).
v0.10.0 phase 25: a FULL-SCREEN draw inside a jittered world / mirror pass (3 / 4 vertices, viewport depth range 0..1: the
game's fog triangle in the forward pass, which samples the scene by interpolated UV) is issued with the game's OWN viewport
(`hkDraw`, no shift, no overscan) and the shifted one is re-issued after it. Shifted, it read the neighbour texel over much of the
picture (shift + the phase-23 scale), at every silhouette the neighbour's depth: a dark 1 px line on the horizon and around bushes
(ATS flat 2026-10-10, `Ctrl+F10` color_in: sky 204 / line 25 / hill 120; gone with DLAA off). Log line `jitter (v0.10.0 phase 25)`.

## Motion vectors

The engine has no velocity buffer, but every draw in the main G-buffer pass has its full MVP in
the VS constant buffer (slot 0, bytes 64..127 of the draw's window in a big dynamic ring). The
`DrawIndexed` hook GPU-copies that MVP (and remembers a geometry key) for the first 128 draws of
each depth layer (world = viewport depth 0.01-0.9, cabin = 0.9-1.0). At the blit, draws present in
two consecutive frames are static-object candidates; `R = MVP_prev * inverse(MVP_cur)` maps current
clip space to previous clip space for everything static in that layer (the world matrix cancels).
A compute pass picks the medoid R of up to 16 world / 8 cabin pairs per layer (rejects moving cars/wheels),
a second pass reprojects every pixel using its depth (depth < 0.01, the VR far/sky layer, reprojects as
infinitely far = rotation only). If the world layer has no matched pair the frame is
sent to NGX with reset=true. Logged every 600 frames (`MV:` lines). In VR each eye has its own candidate
lists and is matched only against its own previous frame. Details: [`DLAA_INTEGRATION.md`](DLAA_INTEGRATION.md).

**Ego reprojection (v0.6.0).** The outside of your own truck (mirror housings and glass, hood, wheels) is drawn
in the world layer but moves with the camera, so `R_world` gave it a large parallax vector and the mirrors
flickered while driving. The solve pass now also builds `R_ego`: the medoid of the near world draws (object
origin closer than `mv_ego_origin_m`, default 8 m) whose motion differs from `R_world` (standing still, or
nothing left: `R_ego = R_world`). World-layer pixels closer than `mv_ego_pixel_m` (default 3 m, `0` = off)
use `R_ego`; the INSERT debug view shows them with blue = 0.5 (cabin = 1).

**Static replay skip (v0.10.0 phase 14, `mv_replay_static_frames` 6 / `mv_replay_static_every` 4).** The per-draw id replay
(`DrawIdRecord::Replay` at the G-buffer leave) was 2.1-3.0 ms per frame at 4K in ATS, nearly all of it static scenery. Design:
- *Key:* the draw's `FullKey` (IB, VB0, offsets, counts, base vertex, VS -- the key the GPU pairing groups by first). Identical
  copies share it, so a key is skipped only while none of its draws moves (conservative: a parked truck of the same model as a
  driving one is never skipped). A buffer-pool rotation gives a new key = a fresh streak (6 frames of full replay).
- *Readback:* the verdict is already on the GPU (`ResolveDraw`). Three more bitmasks in DrawIdMv's `Cnt` buffer ride the existing
  non-blocking diagnostics readback (3 staging slots, `DO_NOT_WAIT`): own-pair STATIC (dwords 384..511), RIGID PARENT this pass
  (512..639, `ParApply`), MOVES (640..767: own-pair mover, track reject, moving twin / attach / parent). `Finish` stores the
  pass's keys with the slot; `Poll` (and `DrawIdMv::PollMain`, called right before each gated replay) feeds a global 8192-slot
  table: STATIC raises the streak once per frame (two VR eyes count once), MOVES resets it, a draw in neither (no partner: new,
  culled back in) is neutral -- it neither builds nor breaks the streak but holds the key (an unpaired copy needs its pixels for
  its own rigid-parent vote). A rigid-parent mark holds the key for 30 frames.
- *Deep static only (round 3):* STATIC means own-pair state 2 AND `dev < mv_replay_static_eps_px` (default 0.02 px; `FoldU.z`).
  A draw static by the 0.1 px snap but above the epsilon (a truck followed at the player's speed: near the focus of expansion its
  own motion stays sub-snap but wobbles) sets the MOVES bit (Cnt [210] counts them) and stamps its key "moved"; a key that moved
  within 60 frames is never skipped. In game (2026-10-09 13:16 DLL) such a truck's plate alternated sharp / blurred with the 4-frame
  re-check cadence: its parts were skipped like world geometry and had no own pixels on the frames their motion crossed the snap.
  Harness S14 (vehicle + plate exactly static, then 0.04 px / frame of its own, then accelerating 0.05 px / frame per frame):
  skipped only while exactly static, never after; `cmp` with / without `rstatic=4` bit-identical.
- *The vote does not see the stagger:* in a gated pass (`ParU.x` bit 4) the marching rigid-parent vote counts a world pixel without a
  G-buffer id as a STATIC source (`kParPseudo`, state 2, the layer's camera R) -- what the skipped draw is. Without it the vote's
  inputs changed with the re-check frames (first in-game test, 2026-10-09: a moving SUV's plate lost its parent -- white in Ctrl+F6
  -- on every ~4th frame). Harness `cmpdump` / `cmp` with `rstatic=4`: S1-S4 and S6-S8 (every plate path: own matrix, twin /
  attach, view-space and instanced plates on a turning trailer) bit-identical motion vectors.
- *Rigid-parent vote, round 4 (in game 2026-10-09 13:48 DLL):* (1) a plate lost its parent for ONE frame (white in Ctrl+F6) in a
  frame where many draws were unpaired at once; (3) roadside grass took an overtaking SUV's motion (wobbly / blurry). Both are
  properties of the phase-6 vote, not of the gate: the harness reproduces (3) with the gate off (S8: four grass clumps 10 cm in
  front of the passing truck's side took its motion in 64 clump-frames). Fixes: every pass keeps a 64-entry table of its listed
  draws (IndexCount + instance count, pixel centroid on a 2 px sub-grid minus the viewport jitter, final parent, size;
  `UHoldW` / `UHoldR`). PLATE HOLD (`ParHold`, end of `CSParentPick2`): no parent this pass (or only the pseudo static source) ->
  last pass's parent is kept for up to 3 frames when the centroid, carried back by that parent's own R, lands within 6 px of last
  pass's entry (and nearer than the camera motion puts it when the two differ by >= 2.5 px; a static parent is kept by the camera
  motion -- only the label). TEMPORAL CHECK (`ParMotionOk`, in `ParBest`): a moving candidate is refused when last pass's entry lies
  clearly where the CAMERA motion puts the draw, not the candidate's (only >= 2.5 px apart: below that a 2 px-grid centroid is
  noise). CLUTTER RULE: a draw of > 4 instances never takes a moving parent (close-car plates are 1-instance draws). Stats: "plate
  holds / refused by the temporal check / to clutter batches" in the static-skip field. Harness S8: + the grass beside the truck
  (8-instance batches: 0 mover frames) and the trailer left out of the replay in frame 50 (`DrawIdRecord::SetTestSkipKey`, harness
  only): its 7 plates keep its motion (a mutation without the hold fails all 7).
- *Round 5 (static smear report, 14:47 DLL):* an id-0 world pixel and a pixel of a static draw take the SAME camera R in pass B
  (`kReprojDepthShader`: only a state-3 mover uses its own R; everything else `Solve[base]`, which `CSPick` overwrites with the
  per-draw consensus when one is found -- the log shows the consensus in 598-600 of every 600 frames). Harness `drop=5` (the
  medoid dropped after 5 healthy readbacks, as in game) `cmpdump` / `cmp` with and without `rstatic=4`: bit-identical. The low-pair
  frames of that run (8-31 per 600 in some windows, 11-26 % paired) come from the phase-8 medoid drop: without pass A, `CSGather`
  predicts with LAST frame's camera R, and an abrupt change of the rotation speed pushes far draws over `mv_drawid_max_m` /
  mis-pairs identical copies (harness `jerk=X`, S1-S4 / S13 / S14: a yaw-rate step at frame 60). A low-pair readback now re-arms
  the medoid (counted as a re-arm), so pass A predicts again until the consensus has been healthy for `mv_medoid_drop` readbacks.
- *Round 6 (ATS 15:29 clip, gate OFF, Ctrl+F6: roadside grass / vegetation solid magenta while driving; "smudgy, even the fence"):*
  the magenta was NOT the rigid-parent path -- its pixels carry no green / cyan / orange tint, i.e. state 3 by the draws' OWN pairs
  (log: 200-210 own-pair movers per frame vs 2-4 parent movers in those windows; the 12:36 clip already shows the same magenta tufts
  where the roadside has grass). Root cause, from the user capture `captures/ats_flat_fence_frame1634.rdc` (cb0 of all 1554
  G-buffer draws, RenderDoc replay): ATS draws its roadside grass CAMERA-RELATIVE -- all 79 non-instanced grass batches (PS 12755),
  all 131 instanced grass draws and 37 other draws: cb0 rows 0..3 = the view ROTATION, rows 4..7 = projection * view rotation,
  column 3 = (~1e-9, ~2e-6, near 0.1, ~-1e-7); the camera translation is in the vertices / another cbuffer. Such a draw's own pair
  gives R = the camera rotation only; with the camera driving it deviated from the camera R by the translation's parallax and
  became a MOVER with that rotation-only R (wrong motion vectors on all its pixels; its matrix twins spread it). Fix
  (`OriginFree`): a paired draw whose MVP column 3 is (~0, ~0, near, ~0) is STATIC (the layer's camera R), never a mover, never a
  skip streak, never a consensus voter (Cnt [214], "camera-relative ... held static" in the static-skip field). Also: an instanced
  draw of more than 1 instance is never counted / listed for the vote (no parent, no hold -- the round-4 clutter rule passed the
  31 of 135 ATS instanced draws with 2..4 instances), and the plate hold applies only when exactly one entry of last pass matches.
  The instance count path is verified end to end (Cnt [215..223]: the counts the shaders saw). Harness S15 (roadside: 16
  camera-relative clumps through VSSkin bones, 12 instanced 2 / 3-instance batches + one of 8, a car overtaking 25-55 cm from the
  clumps, an oncoming car, a parked van): every clump pixel exactly the camera motion in all 100 frames; `mutcr` (the rule off)
  fails it with 620891 clump px off by up to 118 px, `mutinst` (round-4 listing) with 389 parent draw-frames and 302 px.
- *Round 7 (first in-game test of round 6, ATS VR, `captures/dlaa_inject_ats_v0100p14r6_vr.log`):* every 'MV draw-ids @blit'
  window shows `camera-relative (origin-free MVP) paired draws held static 0.0/frame` while the own-pair movers climb to 200-220
  per frame on a grassy roadside (`paired 99.9 % of 1272 draws/frame (movers 212.9 ...`); the grass stayed a mover, still smudged.
  Cause (algebra, column vectors, `mul(M, p)`): a VR eye's view = T(eye offset) * head view, and the grass vertices are relative
  to the HEAD, so the MVP column 3 = P * (ex, ey, ez, 1) = (p00 ex + p02 ez, p11 ey + p12 ez, p22 ez + p23, w = ez as view depth).
  With ex = IPD / 2 ~ 0.032 m and p00 1.3-1.5, |x| ~ 0.04-0.05 clip, far over round 6's |x|, |y|, |w| <= 1e-4 |near| (~1e-5). The
  off-centre eye projection (log: `optical centre uv=(0.379, 0.403) from p=0.2425 q=-0.1932`) changes column 2, not column 3 (it
  only adds p02 ez ~ 1e-3). Fix (`OriginFreeCol`, `kOFreeW` / `kOFreeXY`): DEPTH part |w| <= 0.03 m (an eye-to-head transform
  carries a few mm, on some headsets 1-2 cm, of z; the old |w| <= 1e-4 |z| form needs |w| <= 1e-5 m and is dropped) AND LATERAL part
  |x|, |y| <= 0.25 clip (15 cm of lateral offset at p00 1.7, 17-19 cm at the VR eyes' 1.3-1.5, 25 cm at 1.0) AND |z| > 1e-6. An
  object whose origin is within 3 cm of the eye plane and ~15-25 cm of the eye laterally is only ever camera-attached (wants the
  camera R anyway); world geometry crossing the eye plane (a roadside post) fails the lateral part by metres. Diagnostics (Cnt
  [224..230], `OriginDiag` / `OriginMoverDiag`, mutation bits ignored), appended to the static-skip field as `round 7 origin-free
  split: depth ok (|w| <= 0.03 m) but lateral over 0.25 clip X/frame (nearest lateral miss max(|x|,|y|) M), origin-free max |x| A
  |y| B |w| C, own-pair movers with origin |w| < 0.5 m N/frame (min mover |w| W m)`; the Alt+F8 `MV dump` lines also print
  `col3 x .. y .. z .. w .. origin-free yes | depth only, lateral over 0.25 | no` (every mover is dumped). Harness: S15 runs as
  the two VR eyes (scene 16; also in the full run): every draw's view = T(eye offset) * head view, eye offset (-0.032 / +0.032,
  0.002, -0.004) m, a fixed eye per 100-frame run on its own unit (as the DLL: one unit per eye; alternating the sign per frame
  would be an eye swap), the clumps' cb0 = P * T(eye offset) * head rotation. Same criterion as flat: every clump pixel exactly
  the camera motion in all 100 frames (0 of 1357409 / 1354059 px off), 16 origin-free draws per readback, and the read-back
  origin-free max |x| |y| |w| = 0.0216 / 0.002402 / 0.00400012 = p00 |ex|, p11 |ey|, |ez| exactly (flat: the residues 3.5e-9 /
  2e-6 / 1.2e-7). New mutation `mutr6` (the round-6 rule, FoldU.w bit 3): flat S15 passes, both VR eyes FAIL (618928 / 619372
  clump px off by up to 118.9 px, 1568 clump draw-frames a mover, 0.00 origin-free per readback) -- the harness reproduces the
  in-game miss; `mutcr` fails flat (620891 px, 1379 / 2948) and both eyes (1379 / 2948 each); `mutinst` unchanged on flat (389
  parent draw-frames, 302 px) and fails both eyes (397 / 375, 594 / 556 px). Full run 133103 checks (127205 before: + 2 flat S15
  diagnostics checks + 2 x 2948 VR eyes), `rstatic=4` 133115. `cmp` of the round-6 binary's `cmpdump` vs this one (no rstatic):
  S1-S4, S6-S8, S13, S14, S15 0 px differ, max 0.0000 px (the wider rule changes nothing outside the camera-relative draws);
  `cmpdump` without / `cmp` with `rstatic=4` (this binary): S1-S4, S6-S8, S14, S15, S16 0 px; S13 7-778 px in 1 frame (max
  0.09-0.24 px, the start lag; it varies run to run with the non-blocking readback timing, the round-6 binary gives the same range).
  Next VR log: `held static` must be ~200+/frame on a grassy roadside, the own-pair movers back to the vehicle count, `origin-free
  max |x|` ~ p00 * IPD / 2 (~0.04-0.05), `|w|` a few mm; `own-pair movers with origin |w| < 0.5 m` near 0 (only a
  vehicle right beside the eye or a moving cabin part; the mutation shows 16 per frame = every missed clump, min mover |w| =
  |ez|). If the movers stay high: a `nearest lateral miss` just over 0.25 or a min mover |w| over 0.03 m names which threshold to
  move (Alt+F8 then gives the exact column 3 of each missed draw).
- *Round 7b (ATS flat 17:57, round-7 DLL, `captures/dlaa_inject_ats_v0100p14r7_flat.log`):* the rule fired (33-75 held static per
  frame) but 30-90 own-pair movers per frame kept an origin |w| < 0.5 m, with `min mover |w|` 0.0300-0.0307 m and `origin-free max
  |w|` 0.0296-0.0299 m in every window, max |x| up to 0.235: the camera-relative draws' origin sits at w ~ 0.03 m (and x ~ 0.04-0.24),
  right at the round-7 limits, so each grass batch flipped static <-> mover with its frame-to-frame jitter = the smudge stayed. Fix:
  `kOFreeW` 0.03 -> 0.20 m, `kOFreeXY` 0.25 -> 0.50 clip (the dump / log texts follow). Next log: `min mover |w|` well above 0.2 m,
  near-origin movers ~0/frame, held static ~100-250/frame on a grassy roadside.
- *Perf reading:* the per-section GPU timers are timestamp pairs; when the GPU runs out of queued work it waits for the CPU, and that
  wait lands in whichever section brackets it (in-game logs from before this change already show `fwd-replay` 0.01 <-> 6.9 ms and
  `fwd-depth` 0.05 <-> 4.9 ms between 600-pass windows with the same draws and CPU time). Judge the gain by the `replay` figure,
  the frame time and the `GPU spans scene` figure, not by the mod total of one window.
- *Gate + stagger:* `DrawIdRecord::SetStaticGate(true)` around `DidReplay` (main passes only); a world, non-instanced, non-cabin,
  non-late draw whose key has streak >= frames, a verdict <= 16 frames old and no parent hold is skipped unless
  `frame % every == hash(key) % every`. Only the REPLAY is skipped: the draw is still recorded, so its MVP is gathered, paired,
  votes in the consensus and gets its state every frame (that is what keeps its partner and its verdict alive); its pixels hold
  id 0 = the camera R of their layer in pass B, which is what a static draw gets anyway (harness `cmp`: bit-identical motion
  vectors). Late draws (after a forced / segment replay) are never skipped: a stale id of an earlier segment would survive under
  them.
- *Safety:* the table is cleared on `Forget` (history reset, `MvInvalidate`), an unusable record, a readback without a used world
  consensus (with `mv_camr_consensus` on), a FIFO underflow and a change of the two ini values. Stats: the `static skip` field of the `MV draw-ids`
  line; the Ctrl+F6 view shows id-0 world pixels olive while the gate runs. Harness: `rstatic=E` (S1-S4, S6-S8 and S13, parked
  vehicles that start at frame 40: lag <= 2 frames).

## VR / per-eye (v0.5.0)

VR frame structure (ETS2 `-openxr`, Virtual Desktop; facts in [`CAPTURE_FINDINGS.md`](CAPTURE_FINDINGS.md)): per Present
frame both eyes render a G-buffer pass into the SAME depth texture (eye 1 clears eye 0's depth before eye 0
is tonemapped), both tonemap into the SAME scene-size SRGB texture, then each is stretch-blitted into its own
6120x6496 eye texture. The Present'ed 2560x1440 window is only the mirror.

- **Eye detection.** A 3/4-vertex `Draw` whose PS SRV0 is an R8G8B8A8-family texture at exactly the scene
  depth dims, and whose RT0 is the DXGI backbuffer (flat) or an R8G8B8A8/B8G8R8A8-family texture at least
  scene size in both axes (VR eye texture; v0.7.1: scene-sized is allowed when SRV0 is the tonemap output). The Nth match in a Present frame is eye N
  (cap 2); a backbuffer match is flat (one eye, further matches ignored). DLAA runs on the SRV texture just
  before that blit draw, per eye, so each eye texture receives its own AA'd image.
- **Per-eye depth.** `ClearDepthStencilView` is hooked: if the DSV is the scene depth and it was rendered into
  since its last clear, it is copied into eye k's depth twin first (k = number of such clears this frame,
  0 or 1). At eye k's blit that twin is used if captured; otherwise the live scene depth (last eye, flat).
- **Per-eye state.** Each eye has its own NGX feature and textures (about 0.7 GB each at 4592x6496), and its own
  `CameraMv` (candidates recorded per G-buffer pass index, matched only against the same eye's previous
  frame). NGX itself is initialised once and shut down by the last user.
- **Eye identity from the blit render target (v0.5.4).** ETS2 VR does not alternate the eye order (the v0.5.1
  "alternation" was a Present-boundary artefact). The blit RT of each eye is an OpenXR swapchain image: the first
  blit of a frame always targets one of 3 textures, the second one of 3 others. The blit RT pointer is mapped to an
  eye (an unseen RT gets the blit index within its pair, `slot.s & 1`; the map is authoritative afterwards, a later
  opposite-parity arrival is only counted as `rt parity mismatch`). Each eye's NGX feature, textures, history and
  `CameraMv` therefore only ever see one eye. `R_world` readbacks with `|R[0][3]| > 0.25` are counted per eye in the
  `FIFO stats` line (should be ~0). `vr_eye_alternate` was removed (ignored with a log line if present).
- **Pass FIFO (v0.5.2) -- replaces all Present-based frame bookkeeping.** The mirror-window Present is not a
  reliable frame separator in VR, so eye blit #0 sometimes ran with the other eye's live depth. The game's order is
  strictly passes then blits: `[clear,G-buf A][clear,G-buf B][blit A][blit B][clear,C][clear,D][blit C][blit D]`
  (flat: `[clear,pass][blit]`). A pass starts at each scene-depth clear (only if the previous pass rendered into
  the G-buffer; the first pass starts at the first G-buffer bind) and gets a slot `{s, jitter, phase, snap, depth twin, MV candidates}` in
  a ring of 4. If the newest pass has not been blitted when the next clear arrives, its depth is snapshotted into
  its eye's twin first. Every blit consumes the OLDEST unconsumed pass: eye, NGX jitter and depth source (snapshot or
  live) come from the slot. No pass available = `fifo underflow` (DLAA skipped for that blit); more than 2
  outstanding = oldest dropped (`fifo overflow`). Each slot owns its own depth twin
  (the discard/clear snapshot goes there) and MV candidate record; at the blit the eye (from the RT map) matches
  the slot's record against its previous committed one. Jitter phase =
  `(VR ? s>>1 : s) % jitter_phases` (both eyes of a frame share a phase). Log: `FIFO stats @blit N: ...` every 600 blits.
- **Hotkeys** (DLAA on/off, MV, area, MV debug, jitter sign/only) act on both eyes. The `Ctrl+F9` self-test dumps eye 0 only. The `Ctrl+F10` snapshot
  dumps both eyes with an `_e0` / `_e1` suffix (flat: no suffix, as before).
- **Logs.** `eye blit detected: eye=N src WxH -> RT WxH fmt`, `eye depth snapshot: eye=N captured ...`,
  `frame #n summary: ...` (first 5 Presents: G-buffer pass entries, depth clears, eye blits, snapshots),
  `DLAA: NGX feature created for eye N`, heartbeat with per-eye frame counts and snapshot counts every 600
  frames, and `DLAA GPU cost eye N: avg X ms` every 600 frames (timestamp queries, read back late, never
  blocking). `dlaa.ini` `gpu_timing`: -1 (default) = VR only, 0 off, 1 on.
- **Cost.** About 30 MP per eye: expect a heavy GPU hit and ~1.5 GB extra VRAM with both eyes live.

## OFXR Bridge pass-through (v0.10.0)

- OFXR Bridge's D3D11 bridge opens shared textures on the game's device and copies / draws the eye pictures on the game's own immediate
  context (inside `xrReleaseSwapchainImage` / `xrEndFrame`, game render thread). `IsGameCtx` cannot separate those calls from the
  game's, so the filter is the return address, not the context: `src/ofxr.cpp` records the address range of every `ofxr*` module
  (`EnumProcessModules`, rescanned from the setup thread, then every 300 frames for 10 minutes, then every 3000).
- First statement of Draw / DrawIndexed / DrawIndexedInstanced / Dispatch / CopyResource / CopySubresourceRegion(1) /
  ResolveSubresource / OMSetRenderTargets / RSSetViewports / ClearRenderTargetView: `if (Ofxr::FromOfxr(_ReturnAddress()))` -> call the
  original and return, no counting, no blit test. Readers never lock (fixed array, atomic count written last). `ofxr_bridge = 0` disables it.

## Tuning menu overlay (v0.10.0)

- **State** lives in `inject.cpp` (the "TUNING MENU" block after `ToggleMirrors`): `g_menuOpen` (atomic), the selected row, the
  `menu_*` placement. `MenuKeys` runs in `PresentFrameWork` before the other user keys; every row calls the hotkey's own function
  (`CycleMode` / `CycleProfile` got a direction, the Shift+F11 / Ctrl+F4 / Ctrl+F3 toggles became `ToggleDlaa` / `ToggleUpscale` /
  `ToggleFwdDepth`). The navigation keys repeat in `PollKeys` (350 ms, then 70 ms) and fire only while the menu is open; repeats
  act on the number rows only.
- **Panel** (`src/menu.cpp`, `TuningMenu`): the panel text is drawn with GDI (Segoe UI, `ANTIALIASED_QUALITY`) into a 32-bit DIB,
  only when the panel's text or its build scale changes (the whole `Panel` struct is compared), and uploaded with
  `UpdateSubresource` into an R8G8B8A8_UNORM texture. One textured quad (VS from `SV_VertexID`, rect in a constant buffer; PS alpha =
  max(0.88, luminance), SrcAlpha / InvSrcAlpha, RGB write mask), drawn under `t_inDlaa` with a full save / restore of the state it
  touches (the `PreviewBlit` set + VS / PS constant buffer 0 and PS sampler 0). An `_SRGB` view or a float (scRGB) target gets the
  colours decoded to linear first (white = 1.0, SDR white); the HDR10 screen (R10G10B10A2 with `g_hdrOut`) gets them PQ-encoded
  (Rec.2020, white at 200 nits). Device objects are made per device (a new device recreates them); the texture is dropped when the menu closes.
- **Where it draws.** Flat: at Present, into a per-Present RTV of `GetBuffer(0)` (no reference kept, so `ResizeBuffers` stays
  possible), only when the swapchain's device is the game's. Present-layer mode: at the frame boundary, into the bound RT0 when it is
  the game-side backbuffer. VR: `HandlePossibleBlit` notes the eye and its RT (`g_menuBlitEye` / `g_menuBlitRt`), and `hkDraw` draws
  into the still-bound eye RTV right after the game's blit Draw and `RunPendingComposite`, centred on `g_optC[eye]` + `menu_vr_x/y`,
  shifted inward by `menu_vr_depth`; the VR desktop mirror also gets the flat panel at Present (scaled from the eye texture, so the
  texture is built once per change). Closed: one branch per Present and per blit Draw.
- **GPU figures** (`MenuPerfOpen` / `MenuPerfTick` / `MenuPerfClose`): opening the menu switches `GpuPerf` on when dlaa.ini has it
  off (back at the close); every 60 Presents the menu turns `GpuPerf::Stats` back into window sums and shows the difference to its
  last read (`GpuPerf::Restarts` catches a `LogWindow` restart in between), grouped per feature (`MenuCostGroup`, NGX + mirror NGX
  included). `game` = the GPU spans' scene ms/frame minus the mod's total of both eyes. Texts change only at these snapshots, so
  the panel texture is rebuilt at most once a second.
- **FPS box** (`fps_*`, `g_fpsShow`): `FpsTick` (the one fps source: Presents counted, QPC, figures every 0.5 s) runs per Present
  only while the box is on or the menu is open (`MenuPerfTick` copies its figures); `TuningMenu::DrawWidget` draws it after the panel
  at the same two places (`MenuPresentTarget` at Present, `MenuDrawVr` per eye; the eye blit is noted while menu open || box on) from
  its own texture cache (rebuilt only when its text or build scale changes), dropped by `ReleaseWidget` when switched off.
- **Key swallowing** (`src/dinput_wrap.cpp`, `KeySwallow`), only while the menu is open and only for the 5 bound menu keys
  (`MenuSwallowKeys`): `DirectInput8Create` (proxy.cpp) wraps `IDirectInput8A/W`; `CreateDevice` of a keyboard returns a full
  forwarding `IDirectInputDevice8A/W` whose `GetDeviceState` clears the keys' DIK bytes and whose `GetDeviceData` drops their key-down
  entries. The game window (the swapchain's `OutputWindow`, first top-level Present) is subclassed once: `WM_KEYDOWN` /
  `WM_SYSKEYDOWN` / `WM_CHAR` / `WM_SYSCHAR` and `WM_INPUT` keyboard make events of those keys are dropped. Key releases always pass
  (a key held when the menu opened must reach the game as released). The menu itself reads `GetAsyncKeyState`. A DLL unload puts the
  original window procedure back if the window still points at ours.

### Phase 15 (v0.10.0): VR menu screens -- panel, DLAA area, limits

- **Panel / fps box on the VR menu screens.** The VR main menu (before a save is loaded) and the garage / truck dealer screens
  have no world eye blit, so `MenuDrawVr` never ran there (ATS VR log 18:10: menu opened at 18:10:30, `VR detected at blit`
  only at 18:10:49). Their eye pictures are the preview composites: `MenuDrawPreview` draws the panel + fps box through the
  game's own composite RTV (`PvTarget::rtv`) right after the target's flush -- trigger (a) after the game's first
  non-composite Draw / DrawIndexed into it (the VR verts=96 Draw), trigger (b) at the bind that leaves it; not at Present.
  Eye = `PvUnitOf` (the eye map from the preview projection verdicts); an eye not known yet gets no panel that frame (the
  composite order is not usable: the game renders the eyes in either order). With DLAA off / passive the composites are still
  matched and the candidate / layout / verdict readbacks keep running while the panel or the box is up (`PvReadbacksOn`), no
  DLAA runs. Centre = `VrEyeCentre`: the world eye's optical centre, else the preview picture's own (`g_pvOptC`, tag `preview
  DLAA area`: the reference tile's skew mapped through the tile layout, p' = (p + c.x) / h.x), else the image centre.
  `MenuVr()` (rows edit the VR placement) = an eye blit was seen, or a VR launch composited into a non-backbuffer eye picture
  in the last 120 Presents (a VR launch without a headset keeps the flat rows).
- **Limits.** `dlaa_area` 10..100 (was 40..100, then 20..100; `SceneDlaa::kAreaMin`; ini, keys, menu row). `fps_scale` / `fps_vr_scale`
  0.2..3.0 (was 0.5..3.0); the meaning of the scale is unchanged (VR 1 = 8 % of the eye width, so a saved value keeps its
  size), the VR default is 0.3 (was 1.0, ~490 px on a 6120-px eye).
- **Menu units honour `dlaa_area` in VR.** `PreviewFlush` runs the preview units (eye tags 10 / 11) on the same centred rect
  as the world eyes (SceneDlaa's crop path: feather blend, raw picture outside); flat, the VR-without-headset backbuffer route
  and the two preview debug captures keep the whole picture. The tiled depth assembly clears and reduces only inside the union
  of both eyes' rects + 32 px (`PvAsmClip`; a tile pass's eye is only known at its composite); a tile outside it is skipped.
  The per-tile DS copy stays whole (D3D11 copies a depth-stencil resource only whole; DS_P has no SRV).
- **Next VR log, look for:** `tuning menu: VR panel / fps box drawn on a menu / truck-preview eye picture (target order N ->
  eye E, ...)` before `VR detected at blit` (and no `... NOT drawn ...` lines after the first frames); `preview DLAA area: eye
  0 optical centre uv=(0.379, 0.403) ...`; `preview unit 0 (eye tag 10): DLAA area 40% -> rect (...) 2448x2600 of 6120x6496`;
  `DLAA GPU cost eye 10: ... [preset=E ... area=40 up=off rect=2448x2600 of 6120x6496]` (was total 2.32 ms, ngx 1.48 at
  area=100); `preview depth assembly GPU cost: ... | clip ON (dlaa_area 40: clear + reduce inside WxH ...)` (was 0.32-0.39 ms
  per tile pass); `preview depth assembly: ClearView supported`; `DLAA area now 20%` at the new limit.

### Phase 16 (v0.10.0): VR panel drawn last, panel faces the eye

- **Drawn LAST on each eye picture (the "VR late panel", block after `MenuDrawPreview`).** Phase 15 drew the panel / fps box
  at the point the picture is finished for the MOD (world: after the eye blit + DLSS composite; menu screens: after the
  target's flush, trigger (a) = after the verts=96 Draw). The garage / dealer screens draw the game's own menu into the eye
  picture AFTER that, so it covered the panel where they overlap (user: flicker there). Drawing at the bind that leaves the
  picture or at Present is NOT safe: the runtime gets its swapchain image at `xrReleaseSwapchainImage` (before `xrEndFrame`
  and the mirror Present) and the hooks see no OpenXR call. What they do see: the game's eye picture (6120x6496, the blit /
  preview target) is the game's own eye buffer, READ by a final `Draw(4)` into the OpenXR swapchain image
  (CAPTURE_FINDINGS "VR frame structure" step 5). That read is the safe last point. So `MenuDrawVr` / `MenuDrawPreview` now
  only ARM a per-eye record (`MenuLateArm`: the game's RTV of the picture); the hooks watch for a game read of it (PS SRV0 of
  a Draw / DrawIndexed with <= 6 vertices / indices, or the source of CopyResource / CopySubresourceRegion(1)) whose target
  is not the backbuffer and is eye-shaped (the picture's aspect +-25 %, >= 30 % of its height: the swapchain image, not a
  landscape mirror / small effect texture; `MenuLateOnRead`), and count the game draws into the bound picture (`MenuLateOnBind`,
  `MenuLateCountDraw`). Mode "arm point" (start): drawn at the arm point as in phase 15, the record only watches; 8 pictures
  in a row read after the arm -> mode "late": drawn right before that read. A late record replaced by its eye's next arm
  (within 2 Presents) without a read = a miss -> back to the arm point (3 fallbacks, forgiven after 3600 good pictures; then
  the session stays at the arm point: a setup whose eye buffer IS the swapchain image never reads it). A record older than 2
  Presents (screen change / pause) is "stale", not a miss. `menu.cpp` saves / restores PS SRVs 0..15 now (was 0): the late
  draw binds the picture as RT while the game has it bound as SRV. Nothing runs while the menu is closed and the fps box is
  off (`g_menuLateN = 0`).
- **The panel faces the eye** (`menu_vr_face`, default 1, menu row `Panel faces you (VR)`, no key). `MenuFacingGeom`: the
  centre direction C = (tx, ty, 1) from the flat placement (menu_vr_x / y in % of the eye from the optical centre), half size
  = the flat panel's at the optical centre (a = pw / 2 / fx, b = ph / 2 / fy) times |C| (constant angle), turned to face the
  eye (normal = -C / |C|, right axis horizontal, no roll), the 4 corners projected through THIS eye's projection
  (px = u W + fx x / z, py = v H - fy y / z, fx = W / 2 |P00|, fy = H / 2 |P11|) and handed to the menu VS in clip space with
  w = z (`TuningMenu::DrawCorners`, perspective-correct). |P00| / |P11| come from the same verdict MVPs as the optical
  centre (`MvpScale`: |row0 - (row0.row3 / |row3|^2) row3| / |row3|, model scale cancels; median per readback, 16-sample
  window, `g_eyeScale` world / `g_pvEyeScale` menu picture mapped through the tile layout / h), else 0.9 assumed.
  menu_vr_depth stays the per-eye inward px shift (both eyes share the view-space quad, so the stereo depth comes only from
  it, as before). menu_vr_x = menu_vr_y = 0 = the flat panel (drawn by the flat path, pixel-snapped). The fps box stays flat.
- **Next VR log, look for:** `tuning menu (v0.10.0): ... menu_vr_face=1 (the VR panel turns to face the eye)`;
  `DLAA area: eye N projection scale |P00|=... |P11|=... (view about W x H degrees)` (also `preview DLAA area: ...` on the
  menu screens); `tuning menu: VR late panel: first eye picture recorded ...`; `tuning menu: VR late panel (learning): eye
  N's picture is read by the game's Draw (4) into WxH fmt=F ... N game draw(s) went into it after our panel there` (the
  destination should be the swapchain image, ~3060x3248; a draw count > 0 on the garage = the old overlap);
  `tuning menu: VR panel / fps box now drawn LAST on every eye picture ...`; `tuning menu: VR panel / fps box drawn LAST on
  eye N's picture: right before the game's Draw (4) reading it ...`; `tuning menu: VR late panel stats (menu closed): mode
  late ..., missed 0 ..., game draws over the panel 0 ...` (mode late: 0 expected). With the panel moved to the side:
  `tuning menu: VR panel faces the eye (menu_vr_face=1): eye N centre direction yaw +X deg pitch +Y deg, panel W x H deg ...
  projection |P00|=... (measured, world eye)`. Bad signs: `VR late panel MISSED`, `back to the arm point`, `mode at the arm
  point (stays: no reliable read)` (then no read of the picture exists in this setup: a Ctrl+F11 trace on the garage screen
  with the menu open shows what the game does after its menu draws).

### Phase 17 (v0.10.0): menu / garage DLAA starts at once, shader warm-up on several threads + disk cache

- **Why it started ~20 s late** (ATS VR logs `captures/dlaa_inject_ats_v0100p15_vr2.log`, `_p16_vr.log`): load 20:44:48.05,
  preview targets at Present #31 (20:44:53.4), but `shader warm-up: 38 shader(s) ... in 20283.3 ms` only at 20:45:08.34 and
  the menu units' `DLAA: ready` 10 ms later. `PreviewFlush` (`SceneDlaa::ShadersReady`), the preview jitter (`JitterLive`)
  and `CameraMv::Init` all waited for the LAST of 38 shaders compiled in series on one thread (the draw-id set: ~18 s,
  `drawid_parent_pick1` ~7.4 s). Second gate, hidden behind it: on the VR main menu the preview pass had ONE DrawIndexed for
  ~9 s (`DrawIndexed=1, collector skips: sampled=1`, p14r6 / p8 logs) and the MV sampling hash rejected it -- no candidate
  record, so no eye verdict / tile layout (both need a candidate in every pass; first verdict at #599, 20:45:02) and, once
  DLAA ran, a history reset EVERY frame (`Generate` early-out -> `reset = true`). The readback throttle was not a gate: it
  only starts after the layout / eye map has settled.
- **Shader warm-up** (`src/shader_cache.*`): the core group (ids 0..`kCoreLast` = `kPvEdgeFill`: camera MVs, depth, RCAS,
  composite, MV debug, preview blit / depth assembly / edge fill) first, then the menu quad, then the world set; hardware
  threads / 2 workers (2..8, the extra ones below normal priority) take entries in that order; every blob is published at once
  (`Ready(id)`, `Code`), `CoreDone()` after the core group, `Done()` after all. DXBC disk cache next to the game exe:
  `dlaa_shader_cache\<name>_<key>.dxbc`, key = FNV-1a 64 of source / name / entry / profile / flags / compiler version;
  header (magic, version, key, size) + payload hash + "DXBC" checked at load; a bad file is ignored, recompiled, rewritten
  (temp file + rename); a changed shader gets a new key and its old file is deleted. Never on the render thread.
- **Who waits for what now:** menu / truck-preview units (`SceneDlaa::SetMenuUnit`), their viewport jitter (`PvJitterLive`),
  the preview depth assembly, edge fill, preview blit and the texture LOD bias on preview passes: the core group.
  `CameraMv::Init`: its two shaders. World / mirror units, world jitter, LOD bias on world passes: `Done()` as before
  (`CameraMv::InitObjects` now also waits for `Done()` instead of failing). Tuning menu: its two shaders.
- **Preview MV candidates:** the first DrawIndexed of every preview pass is collected even when the sampling hash rejects it
  (`CollectMvCandidate(..., force)`): eye verdict + tile layout get their readbacks from the screen's first frame.
- **Still-camera fallback** (menu units only): a frame whose record has no candidate runs with R = identity (MVs 0) and keeps
  the history; while that lasts a world miss keeps it too; the first solve with pairs takes over without a reset.
- **Tile layout wait:** while the layout of a target order is unknown its flush leaves the picture untouched for at most 8
  flushes per episode (`kPvLayWaitFrames`), then the reference-slot path runs as before (a TILED picture run as untiled =
  one tile's depth stretched + half the jitter, and the verdict resets the history anyway).
- **Start of the game:** the 30-Present "no world G-buffer" wait before a preview pass counts only once a world G-buffer bind
  was ever seen (it delayed the first menu frames by 31 Presents).
- **Next log, look for:** `shader warm-up: N shader slot(s), T worker thread(s) ...`; `shader warm-up: core set ... ready
  after X ms`; `shader warm-up: N from the disk cache, M compiled in X ms on T thread(s) ...` (first start after an update:
  N = 0; later starts: N = 38, well under a second); `preview MV (v0.10.0 phase 17): the first DrawIndexed of a preview pass is
  collected ...`; `preview DLAA: started N Presents / S s after the first preview target (R identity until candidates |
  real camera R from the first run, layout TILED|single after K frames) ... | first eye verdict after E frames | ... held: H
  flush(es) waiting for the tile layout ...`; if the fallback ran: `preview DLAA: unit U (eye tag T) runs with R = identity`
  and `... real camera R takes over at Present #P (S s after the first preview target) ...`.
- **Preview unit vs scene eye unit** (for the garage-door shimmer): same Halton phases (8, one phase per frame for both eyes),
  same preset rule (menu units always E), same RCAS (strength / radius, `preview_sharpen`), same LOD bias sets. Different:
  the picture is the game's 2x2-supersampled tile composite in LDR (post-tonemap, 8-bit), the tile passes are shifted by
  2x the jitter in tile px (+ the edge fix); depth = nearest of each 2x2 tile footprint (tiled) or the reference tile's
  snapshot (untiled), no forward-depth pass; MVs = camera-only medoid R of <= 40 hash-sampled draws of the reference pass
  (no per-draw / per-object MVs, near-reject 0, no ego); no pre-tonemap HDR unit.

### Phase 18 (v0.10.0): VR GPU cost of the per-draw chain -- the two single-thread hot spots

- **Evidence** (ATS VR, Quest 3 / VD, eye 3064x3248, `captures/dlaa_inject_ats_v0100p16_vr.log`): mod GPU 1.41 ms per eye (NGX
  apart), of it `pick+res` 0.15-0.18, `p-vote1` 0.12-0.18 and `p-vote2` 0.08-0.17 in every window, while `p-count` -- the same
  vote grid, more texture reads per thread -- cost 0.012. Both hot spots are LATENCY, not work: `CSPickResolve` (phase 9) resolves
  ~2200 draws in ONE group of 256 threads (9 draws per thread, on one SM); in the vote one thread of a small (FINE) plate marches 4
  sub-samples x 8 directions x 16 steps in a row (512 dependent texture reads) while the rest of the GPU idles. The harness on the
  hardware adapter (`hw`, 960x540) shows the same: `pick+res` 0.044-0.058 ms for ~100-300 draws, S8 `p-vote1` 0.066-0.128 ms.
- **`mv_fold_dispatch = 2` (new default; 1 = phase 9, 0 = phase 8, both unchanged):** the list + twin + attach group stays
  (`list+twin`); the consensus pick runs as `CSPickPar` (the |consensus - medoid| statistic's loop over the candidates, one thread
  each, summed by thread 0 in candidate order -- identical values), then `CSResolveInherit` = resolve + inherit in ONE wide dispatch
  (one thread per draw; `Inherit1Draw` reads only the state its own thread just wrote). GPU sections `pick` + `res+inh`.
- **`mv_vote_compact = 1` (new default; 0 = the phase-8 grid vote):** `CSParentCount` (same 8x8 groups as the vote) appends every
  group that saw a COUNTABLE pixel at any sample a vote thread reads (grid sample + the three 2 px sub-samples) to a tile list -- the
  listed draws are a subset of the countable ones, so no vote thread outside those tiles could march; `CSParentList` writes the
  indirect args over the tiles (more than 65536 tiles: the full grid). Per vote pass `CSParentCollect` (over the tiles) does exactly
  `CSParentVote`'s per-pixel tests and appends each march sample as an item (more than 131072: marched in place, as before);
  `CSParentMarch` runs one thread per (item, direction) through the same `ParMarchDir` into the same vote tables (each item's
  counters summed in groupshared and added once). Sections `p-col1` / `p-col2` (collect) + `p-vote1` / `p-vote2` (the marches).
  Buffers per DrawIdMv unit (made on first use): 2.3 MB tile / item list + 32 B march args; Cnt grows to 3328 B (dwords 768..772).
- **Bit-identical:** harness `cmpdump` of the pre-change binary (HEAD c7bf34c) vs this one, all scenes (S1-S16, 1598 frames): the
  two dump files are BYTE-identical (default knobs, and `legacy`); `cmp` with `fold=1 nocompact` and with `fold=0`: 0 px, max
  0.0000; `rstatic=4` per scene: S1-S4, S6-S8, S14, S15, S16 0 px, S13 24 px in 1 frame (max 0.24 px) -- the base binary against
  itself gives 17 px in 1 frame (max 0.24): the documented readback-timing start lag. Mutations `noparent` / `parentconj` / `nodepth`
  still fail S8; S8 under the D3D11 debug layer: 0 messages. Full suite 133103 / 133103 (WARP).
- **Per-eye cost, before (p16 VR log) / after (expected; the hw harness rows are measured, GPU ms per frame, 960x540):**

  | section | VR before | VR after (expected) | hw S8 before / after | hw S7 before / after |
  |---------|-----------|---------------------|----------------------|----------------------|
  | pick+res -> pick + res+inh | 0.15-0.18 | ~0.01 + ~0.03 | 0.057 / 0.011 + 0.006 | 0.054 / 0.011 + 0.008 |
  | p-vote1 -> p-col1 + p-vote1 | 0.12-0.18 | ~0.01 + 0.01-0.03 | 0.066-0.128 / 0.010 + 0.008-0.015 | 0.092 / 0.010 + 0.012 |
  | p-vote2 -> p-col2 + p-vote2 | 0.08-0.17 | ~0.01 + 0.01-0.02 | 0.031-0.033 / 0.007 + 0.007 | 0.027 / 0.007 + 0.009 |
  | mod total | 1.41 (@21000, Ctrl+F6 on: dbg-view 0.10 of it) | ~1.0-1.1 | 0.274-0.339 / 0.168-0.173 | 0.302 / 0.187 |

  Expected saving 0.27-0.42 ms per eye (both eyes: 0.5-0.8 ms per frame). The "after" VR figures are an estimate from the phase-8
  wide-dispatch numbers (resolve 0.004-0.006, inherit 0.022-0.028 ms at ~1900 draws) and the hw harness; the next VR log decides.
- **Cabin consensus (target 3, diagnostic + opt-in):** the cabin layer never reached the 24-draw consensus (`cabin: consensus 0 /
  fallback N` in every window), so its medoid and its 24 candidate copies per pass (`cand-copy*`, a sampled UPPER bound of 0.09-0.17
  ms) stayed. The `cabin:` field now prints the largest agreeing cluster of the fallback frames; `mv_cons_min_cabin = 8..64` (default
  0 = 24, unchanged) lets a smaller cabin cluster replace the medoid (and `mv_medoid_drop` then drop the cabin copies). Not
  bit-identical when set (the cabin camera R comes from another static cabin draw's exact R: float-rounding differences), so it is
  off until a VR log shows the cluster size.
- **Not changed:** pass B / RCAS / copies (the tonemap texture is typed R8G8B8A8_UNORM_SRGB, fmt 29: no UNORM view, no UAV -- RCAS
  cannot write into it and NGX cannot read it as UNORM, so copy-in / copy-out stay; the depth convert is already inside pass B);
  the depth snapshot (phase 10 job C: the game's depth has DSV binding only, substituting our own depth for it still risks a bind we
  do not see -- 0.067 ms per eye now, the risk is the same); `dbg-view` costs nothing while Ctrl+F6 is off (the section is inside
  `if (mvDebug ...)`; the @21000 window had the view ON since 21:18:31).
- **Next VR log, look for:** `phase 18 (v0.10.0): mv_fold_dispatch=2 mv_vote_compact=1 mv_cons_min_cabin=0 ...`; `perf eye N`:
  `pick` + `res+inh` in place of `pick+res`, `p-col1` / `p-col2` next to `p-vote1` / `p-vote2` (their sum vs the old 0.25-0.35);
  `perf vote @blit N: ... X of Y vote groups per pass hold a countable pixel (P %) ...` (P a few %; items over the cap and tile
  overflows 0); the `cabin: consensus 0 / fallback N (largest agreeing cluster avg C draws there; 24 needed)` field (C >= 8 steady
  = `mv_cons_min_cabin` can be tried).

### Phase 19 (v0.10.0): the car's cabin -- interior draws whose cb0 rows 4..7 are not their MVP, a small cabin layer

- **Evidence** (ATS VR, driving a car, `captures/dlaa_inject_ats_v0100p17_vr.log`, Alt+F8 dumps 21:49:41 / :43 / :49): the cabin layer
  has 7 draws. The four big ones (ic 6168 / 642 / 4614 / 2517, the interior) are state 3 MOVERS (probe dev 10.5-17.4 px, 2.5-2.9 px
  in dump 3) and print `col3 x 1.0000e+00 y a z b w c` in EVERY dump, while the camera-locked ic 18 / ic 6 beside them print
  `col3 x a y b z c w ...` with the same a, b, c (dump 1: 0.11267 / 9.4642e-04 / 0.10092 for ic 6168 / 642 and the pair). A real
  MVP's clip x of an origin cannot stay at exactly 1.0000 through three head poses: that shader keeps its MVP one row later, the
  injector's rows 4..7 read is (a row with .w = 1, MVP rows 0..2). Such a "matrix" still inverts and pairs; its R lives in the wrong
  coordinates, the probe deviation is meaningless, and pass B throws those pixels' vectors far away (harness: 1644 px) = the
  see-through hole in the cabin wall that grows with the head turn. `cabin: consensus 0 / fallback N` in every window: the cabin R
  was pass A's medoid of those 7 pairs -- 4 garbage, 2 identical camera-relative overlays (their own R: the head rotation only) and
  ic 24. (The phase-18 debug frames show the interior flat cabin-blue = static while the head was still.)
- **What pass B does for cabin-depth mover pixels** (`kReprojDepthShader`): a pixel owned by a state-3 draw uses THAT draw's R
  whatever its depth (`didMover`: `c = Rd * (ndc, zd, 1)`, zd from the draw's own viewport range [0.9, 1] for a cabin draw); only a
  non-mover pixel picks `Solve[cabin]` (d >= 0.9) or `Solve[world]`. So the hole sat on the four movers' own pixels; the junk-R
  idea (non-mover cabin pixels with a bad cabin R) is the second, smaller effect below.
- **MVP shape (`NotMvp`, draw_ids.cpp + pass A):** a real MVP's clip-w row 3 is the view axis in object space (|row3.xyz| = the
  object's scale, about the x / y rows' length / p00); the misread row 3 is the clip-z row (|xyz| = the projection's z scale: 0 for
  the infinite world projection, ~0.007 for the cabin's finite one). |row3.xyz| < 0.02 x the longest of rows 0..2 = not an MVP:
  never paired (CSPair), never a voter / candidate, state 5 (no MVP: the layer's camera R; grey in Ctrl+F6), and never a pass-A
  medoid / ego candidate. Cnt [232] / [233] count them (all / cabin); the Alt+F8 lines print `mvp-shape ok | NOT AN MVP (|row3.xyz|
  X, longest row Y)` and every such draw gets a line. Harness mutation `mutshape`.
- **Small cabin layer (`mv_cabin_small`, default 12; 0 = off, 2..64):** fewer paired cabin draws than this (a car; a truck cab has
  dozens) -> the cabin candidates are ranked by the summed IndexCount of the voters that agree with them (CSVote also sums the
  agreeing voters' IndexCount into Work `kVoteW`; a cluster of 1 is enough: the largest real cabin part decides) instead of
  falling back to the medoid. Pass A's medoid is KEPT when it agrees with that cluster within 0.01 px at every member's probes
  (`kCabKeepPx`; no write at all -- S1-S4's 8-draw cab keeps it in every readback); else the cluster's R replaces it (Cnt [28] = 5;
  6 = kept). Not counted as a healthy consensus: the cabin medoid and its candidate copies stay armed. Movers relative to the cab (a
  swinging freshener) keep the normal own-pair mover rule against the better reference. Without the shape rule this rule makes the
  car WORSE: the heaviest cluster is then the garbage dashboard + console pair (harness `mutshape`: the cabin R itself 1805 px off).
- **Harness S17 / S17b** (`drawid_harness.exe 17`; last in the full run so an older binary's full-run `cmpdump` still lines up): a
  car driving 0.5 m / frame over a static world, VR-like head motion inside it (yaw +-0.35 rad, pitch +-0.12 rad, 2 / 1.5 cm head
  bob), a finite reversed-Z cabin projection (0.05..15 m), three camera-relative overlays (projection * head rotation + per-frame
  offsets), the hood as a world-layer draw moving with the car, an oncoming car. S17: the interior's 4 big draws with the MVP in cb0
  rows 5..8 (row 4 = (0, 0, 0, 1); `VSShift` / `PSMain9`), 2 small real parts, a swinging freshener; S17b: one big body draw (1440
  indices) against a 2-draw swinging tag (6 indices each: more draws, far less IndexCount). Checks per frame: every cabin-depth pixel
  = the exact cabin motion (<= 0.05 px + fp16), the freshener / tag their own R, the hood its own, world pixels the camera R, the
  cabin R of the solve buffer vs the exact one on a grid (<= 0.05 px), the draw states (S17 big draws state 5, overlays / small parts
  2, hood 3), the shape count (4 / 0 per readback), the rule used. Result: S17 1184 / 1184 (0 of 13.3 M interior px off, worst
  0.035 px, cabin R worst 0.0003 px), S17b 694 / 694; `fold=0` / `fold=1` / `nocompact` / `legacy` the same; under the debug layer
  0 messages. Mutations: `mutshape` fails S17 (12.8 M of 13.3 M interior px off by up to 1817 px, cabin R off by up to 1805 px, the
  big draws movers); `cabsmall=0` fails both (the medoid = an overlay's rotation-only R, cabin R off by up to 6 px: S17 12.8 M of
  13.3 M interior px off by up to 4.9 px; S17b's body turns mover with its own R, its overlays off by up to 3.9 px).
- **Bit-identical:** the full run's `cmpdump` of S1-S16 (1598 frames) is byte-identical to HEAD 2d14fa7's (rebuilt from `git archive`);
  `rstatic=4` passes (134994; HEAD itself varies 133116-133121 with the readback timing). Full suite 134981 / 134981 (WARP).
- **Next ATS car log, read:** `phase 19 (v0.10.0): mv_cabin_small=12 ...`; in `MV draw-ids @blit`: `cabin: consensus 0 / fallback N
  (...) ...; small cabin layer (mv_cabin_small 12): small-cluster R F frames, last from draw ic X (cluster weight W IndexCount, dev to
  medoid D px) (max dev to medoid M px), medoid agreed K; cabin paired P / voters V per frame; rows 4..7 not an MVP S draws/frame
  (cabin C)` -- expected in the car: C ~ 4 (the interior), paired ~3, voters ~1 (ic 24), X = 24; an Alt+F8 dump shows the four big
  draws `mvp-shape NOT AN MVP ... state 5 no MVP (camera R)` and no cabin movers. In a truck: C 0, `medoid agreed` in most frames.
  Ctrl+F6: the car interior grey (state 5), not magenta, while the head turns; the hole must be gone. If C stays 0 with the
  interior still magenta, the shape rule does not see that layout: the dump's `|row3.xyz|` vs `longest row` says by how much.
- **Risks:** (1) a real MVP of a mesh scaled very unevenly (one axis < 2 % of another) seen along that axis fails the shape rule ->
  camera R for it (a moving one would lose its own motion; none seen so far). (2) The interior draws now take the cabin R, which in
  the car comes from ic 24 alone (the only paired, non-camera-relative cabin voter): if ic 24 is not cab-fixed, the cabin R follows
  it. (3) IndexCount weighting: a big cabin part that moves (a steering wheel while steering) outweighs a few small static voters
  and becomes the cabin R while it moves. (4) The interior's real motion is the cabin R, not its own: correct for a cab-fixed
  interior, wrong for an interior part that moves (doors, wheel) -- those parts' real MVP is not read.

### Phase 20 (v0.10.0): VR late panel -- the eye read accepted by its written rect, one panel per eye per Present

- **Evidence** (ATS VR, Quest 3 / Virtual Desktop, `r_manual_stereo_buffer_scale` 1.0 = eye picture 3064x3248, log
  `captures/dlaa_inject_ats_v0100p16_vr.log`): the phase-16 read test never accepted a read (`game reads of the picture 0`), but
  from Present #8013 (the screen before the world, then the world) it rejected exactly 2 reads per frame "into other targets":
  `CopySubresourceRegion` of each eye picture into ONE 6128x3248 R8G8B8A8_UNORM target (2 x 3064 wide: both eyes side by side).
  The whole target is 2:1, the test wanted the whole target eye-shaped. The garage / menu screens showed no read of any kind
  (also not at stereo 2.0). The same log has one `eye-blit-like draw on a non-game context ... 4 verts, PS SRV0 3064x3248
  fmt=27 tid=...` (a reader of an eye-sized picture on another thread).
- **Acceptance rule** (`MenuLateOnRead` + `MenuLateWrittenRect`): the read is measured by the rectangle it WRITES, not by its
  whole target: copy = the source box (or the whole source subresource) placed at DstX / DstY, clipped to the destination mip;
  CopyResource = the whole destination; Draw / DrawIndexed (<= 6 vertices / indices) = viewport 0 clipped to RT0 (the whole RT0
  without one). Accepted = destination is a 2D texture, NOT the mirror window / backbuffer (pointer, or its size + format), and
  the written rect has >= 50 % of the picture's area with an aspect within 40 % of the picture's (either way, max(a/b, b/a) <=
  1.4). The side-by-side copy above: rect 3064x3248 = 100 %, aspect 1.0 -> accepted. Still rejected: a blur / effect / small
  texture (< 50 %), a landscape mirror copy (aspect), the backbuffer. The small-Draw read now looks at PS SRV 0..7 (was 0; the
  slot is logged). Every distinct destination (resource + verdict, up to 16) is logged ONCE: `read target #N` with size, format,
  array size, aspect, area %, mirror / backbuffer yes / no, the written rect, a layout guess (side-by-side / stacked / eye-sized
  / part of a bigger target), the caller module (`DescribeAddr` of the hook's return address) and the verdict.
- **Once per eye per Present** (all modes): a per-eye stamp (`MenuLateEye::drawnFr`, the Present interval the eye got its panel
  in) blocks any second draw into that eye in the same Present (`dupSkips`). Several distinct pictures of one eye in one Present:
  the panel goes on the eye's LAST one of the frame pattern -- learning: the last arm, late: the last accepted read; "last" =
  the smaller of the previous two Presents' counts (`MenuLatePat`; a pattern alternating 1 / 2 keeps picture 1 every frame instead
  of flickering every other frame; an eye whose count drops below that gets no panel that Present: counted, `noPanel`). Further
  fixes of the duplicates: the same picture re-armed in the same Present AFTER its read keeps its record (phase 16 retired it and
  drew a second panel at the new arm); a picture already recorded for the OTHER eye in the same Present is not recorded again
  (one picture for both eyes / an eye-map flip drew two panels, at both eyes' placements, into one picture; `shared`). A record
  replaced by another picture of the same eye in the same Present is `superseded` (an intermediate picture): not a miss, does not
  reset the learning run.
- **Loading-screen choice: no loading-screen detection, no skip.** The hooks have no reliable loading-screen signal, and the
  loading screen DOES read its picture (the side-by-side copy starts there), so it reaches mode late like the world and gets its
  panel right before that read, once per eye; the pattern rule covers a screen that writes several pictures per eye. Skipping
  the panel there would hide the fps box exactly where the user watches the loading progress.
- **Mode per arm-point category** (`g_menuLateM[2]`: world eye blit 'w' / menu-preview flush 'a' 'b'): learning, fallbacks and
  forgiveness are kept per category, so the garage (no read) no longer throws the world's late mode back to the arm point.
  Unchanged per category: 8 pictures in a row read -> late; a late picture never read before its next arm (in a LATER Present,
  within 2 Presents) = a miss -> back to the arm point, 3 fallbacks -> that category stays there, forgiven after 3600 good ones.
- **Other-context reads** (`MenuLateOnForeignDraw`, hkDraw / hkDrawIndexed non-game branch, <= 6): PS SRV 0..7 compared with the
  recorded pictures through atomics (`g_menuLateTexA`, published by `MenuLateUpdateN`; the other thread never touches
  `g_menuLate`); counted, the first 3 logged with the context type (deferred / immediate), same device as the game's or not, tid,
  caller and RT0. Never a draw point.
- **Stats line** now: `mode world <..>, menu / loading screens <..> | eye pictures N (panel drawn LAST L, at the arm point A;
  skipped S1: not the eye's last picture / read of the frame pattern, S2: the eye had its panel this Present already;
  eye-Presents without a panel P) | game reads of the picture R accepted (+ M into the mirror window / no target and O into other
  targets, rejected; F on another context, never used) | read accepted: target #K = <what> into WxH fmt=F (<layout>, written rect
  wxh, caller <module+off>), N read(s) | missed ..., superseded ..., arm(s) of a picture the other eye had recorded ..., fallbacks
  world a / menu b of 3, game draws over the panel ...`.
- **Next VR log, read:** `tuning menu: VR late panel: read target #0: eye N's picture 3064x3248 read by the game's
  CopySubresourceRegion (0, caller X) into ... 6128x3248 fmt=28 ... written rect 3064x3248 at (0,0) / (3064,0) ... layout:
  side-by-side, both eyes -> ACCEPTED` (caller X = the game exe or the runtime's DLL: says who copies); `VR late panel (learning,
  menu / loading screens)` / `(learning, world): eye N's picture is read ...`; `VR panel / fps box on the world now drawn LAST on
  every eye picture`; `VR panel / fps box drawn LAST on eye N's picture: right before the game's CopySubresourceRegion ...`; the
  stats line with `read accepted: target #0 = CopySubresourceRegion into 6128x3248 ...`, `game draws over the panel 0` in late
  mode, `skipped ... the eye had its panel this Present already` or `arm(s) of a picture the other eye had recorded` > 0
  on the loading screen (= the old duplicates, now blocked; the first ones are logged one by one), `eye-Presents
  without a panel` near 0. Garage: any `read target` line there (slot > 0, or a Draw with a viewport) or `read on ANOTHER
  context` = where the garage picture goes. Bad signs: `MISSED`, `back to the arm point`, `eye N got NO panel in Present` every
  frame.
- **Risks / not verified:** (1) the side-by-side copy is ASSUMED to be the runtime taking the picture (Virtual Desktop's stream
  image, inside `xrReleaseSwapchainImage` / `xrEndFrame` on the game context, like OFXR Bridge); if it is the game's own desktop
  mirror copy made AFTER the release, the late panel lands in the mirror only and vanishes from the headset in the world / on the
  loading screen -- the `read target` caller module tells (then reject that caller). (2) The garage / dealer screens still have
  no accepted read: there the panel stays at the arm point and the game's menu can still cover it (problem (a) remains until the
  next log shows the garage read). (3) The game's own fps meter "duplicating" was assumed to be the same double draw; not proven.
  (4) The pattern rule can drop the panel for one Present when an eye's picture count falls (a screen change): `noPanel` counts it.

### Phase 21 (v0.10.0): mirror_vr_mode 2 handled no mirror at all; Alt+F8 pixel probe (pink shop windows)

- **Mirror evidence** (ATS VR, `captures/dlaa_inject_ats_v0100p19_vr.log`, `mirror_vr_mode 2`, `mirror_vr_views 2`): every
  `mirror units @blit` line says `handled 4, not handled 63072 in total`, all three units `views 0, dlaa ok 0`, and
  `views left to the game as not among the largest ~1444` per window. So every mirror (the car's interior mirror sits on the
  right side of the left eye) showed the raw jittered picture = the flicker there; `mirror_vr_mode 1` was the workaround.
- **Cause:** the phase-9 selection ("the `mirror_vr_views` largest views of the previous frame") was built in `MirBeginView`
  inside `if (fr != g_mirFrame)`, from `g_mirSeen`. But `MirNextFrame` (the Present boundary, `OnPresentBoundary`) runs first:
  it already sets `g_mirFrame = g_frames` (the NEW frame number, `g_frames` was incremented before) and empties `g_mirSeen`.
  So the block never ran in a normal frame: `g_mirSelN` stayed 0 for the whole session and `g_mirSelUsed` (the "first N views
  before the first selection" counter) was never reset -- the first 2 views of the session were handled, every later view
  was left to the game. (The 4 instead of 2: most likely the block ran on a rare frame boundary that did not pass
  `MirNextFrame`, with an empty table -- it reset the counter but built nothing; not proven.) Not VR-specific:
  `mirror_flat_mode 2` had the same bug. The VR timing candidates (two eyes per Present, eye 1's views after eye 0's main pass)
  were not the cause: the mode-1 log shows ~2-3 mirror views per Present for both eyes together (the mirrors are rendered once
  per frame, not per eye).
- **Fix** (`MirSelEndFrame`, inject.cpp MIRROR UNITS): called by `MirNextFrame` BEFORE the frame's table is cleared (and by
  `MirBeginView` when a frame boundary was missed). `g_mirSelUsed` restarts every frame. The selection is learnt from a table
  of view KEYS (size + order among the views of that size in the frame, `MirSeen.sord`), each with the frame it was last seen
  in; a key not seen for 120 frames with mirror views is forgotten (16 keys max). The selection = the `mirror_vr_views`
  largest known keys (area; ties: lower order, then wider). Why not "the largest of the previous frame" only: the mode-1 log
  (`v0100p19_vr2`) shows the game renders some views on some frames only (1024x512 every frame, 2048x512 most frames, a 256x512
  / second 1024x512 now and then) -- a one-frame selection would drop the main mirror on every frame after one that skipped
  it. Frames without mirror views (menus, loading) change nothing. Before the first frame with views has ended, the old
  "first `mirror_vr_views` views of the frame" rule applies (now per frame, counted as `handled before the first selection`).
- **New log lines (mode 2 only):** `mirror units (mirror_vr_mode 2): selection now [2048x512 #0, 1024x512 #0] -- the 2
  largest of the known view keys [...]` on every change (first 24), and after every `mirror units @blit N` line: `mirror units
  mirror_vr_mode 2 selection @blit N: mirror_vr_views 2, selected 2: [WxH #o (in F of W frames), ...] | views matched M
  (handled), not selected U (left to the game), handled before the first selection P | selected entries absent from their
  frame A | selection rebuilt B times, changed C | known view keys K: [WxH #o (n frames, last seen k frames ago), ...]`.
- **Pixel probe (Alt+F8, same key, same dump; "pink shop windows").** In the `Ctrl+F6` view magenta = blue (cabin class,
  d >= 0.9 in pass B's merged depth) + red (mv.x > 0); the `v0100p19_vr2` dumps show ~0 movers, so the windows are NOT movers
  but pixels that pass B classifies as cabin depth (cabin camera R = wrong motion = wobble). Why a facade at 30-100 m reads
  d >= 0.9 is the open question; the probe names the draw and the reason:
  - `RequestMvDump` passes eye 0's optical centre (VR: `g_optC[0]`, image centre until known; flat: image centre) to
    `DrawIdMv::RequestDump(why, u, v)`. At the dumped pass's `Prepare` the centre is packed (x | y << 16) into pass B's
    `FwdMode.w` (`CameraMv::WriteDims`, `DrawIdMv::ProbePacked`; 0 = off, every other pass) and `DispatchReproj` binds a
    3200-byte raw buffer at u4 (`DrawIdMv::ProbeUav`, cleared). `kReprojDepthShader` (end of CSMain, after `MvOut` is written --
    the MVs are unchanged): the pixels of a 5 x 5 grid 48 px apart around the centre write a 128-byte record each (scene depth
    of the pass's twin, the forward depth as read, merged d, G-buffer / forward ids, flags: forward won / id valid / mover R /
    per-pixel attach / cabin / ids on / ego / stencil id / forward >= 0.9 overlay, the id used + its state, the G-buffer id's
    state, R base, z, zd, mv, clip w, view distance, the camera path's mv, ndc). `Finish` copies it to a staging buffer with
    the dump tables; `DumpPoll` logs it with the CPU facts of both draws (`DrawIdMv::ProbeLog`).
  - Every recorded draw now keeps the game's pixel shader (identity: `hkPSSetShader` -> `DrawIdRecord::NoteGamePs`, copied by
    `Record`); `DxbcPsClass` (phase 12 scan at `CreatePixelShader`) also reports a depth output -- `dcl_output` (opcode 0x65)
    of operand type 12 / 38 / 39 = `oDepth` / `oDepthGE` / `oDepthLE` (token values checked against fxc output) -- stored as
    bit 4 of the shader table entry (`kLodPsDepth`), read through `DrawIdRecord::PsInfo` (`DidPsInfo`). The existing
    `MV dump n/N` lines end with ` | vp depth [min, max] | PS <ptr>: no SV_Depth | WRITES SV_Depth (, discard) | unknown (no
    DXBC scan)`; every G-buffer draw whose PS writes SV_Depth gets a line; a new header line counts them (`G-buffer pixel
    shaders ...: N of M draws write SV_Depth (C of them with the cabin viewport) ..., D alpha-tested (discard), U unknown ...;
    V G-buffer draws with the cabin viewport`).
  - **Probe log format:** `MV probe (<why>): eye 0 pass, 25 pixels = 5 x 5 grid 48 px apart centred on px (cx, cy) of the WxH
    image ... | pass B crop at (x0, y0) wxh | draw ids: G G-buffer + F forward ...`, then per pixel `MV probe [r,c] px (x, y)
    (+dx, +dy): scene depth 0.xxxxxx, <forward 0.xxxxxx -> FORWARD won | scene won (forward behind) | no forward depth -> SCENE
    | forward ... >= 0.9 (an overlay at the camera: ignored) -> SCENE>, d 0.xxxxxx = <CABIN layer (d >= 0.9) | world layer |
    sky> | R: <own-mover R of id N (own pair | rigid parent id P | matrix twin id T | attached to G-buffer mover id M) | the
    G-buffer MOVER under the forward pixel (per-pixel attach) | CABIN camera R | WORLD camera R | EGO R> | MV (x, y) px, camera
    path (x, y), clip w W | GBUF id N GBUF ic I xK flags 0x.. <CABIN | world> vp depth [a, b] state S <state> (<source>)
    mvp-shape <ok | NOT AN MVP | no MVP> col3 w .. | PS <ptr> <no SV_Depth | WRITES SV_Depth | ...> | FWD <same, or none>`,
    plus for a cabin-layer pixel a NOTE: `cabin-range depth WITHOUT a G-buffer id` (no recorded G-buffer draw reproduces the
    depth: SV_Depth PS / not a G-buffer draw / depth rewritten after the replay), `cabin-range depth on a WORLD-viewport
    G-buffer draw` (impossible without SV_Depth or a later depth write), or `the G-buffer draw under it has the CABIN viewport
    [0.9, 1]` (the game draws that surface in the cabin layer). A pixel outside pass B's crop: `not computed`. Last: `MV probe
    (<why>): N of 25 pixels computed | cabin layer C (no G-buffer id a, a world-viewport G-buffer draw b, a cabin-viewport
    G-buffer draw c; under a PS writing SV_Depth d) | forward won f | mover R m | mv.x > 0.5 px x`.
- **Verified:** Release build, 0 warnings, fxc validates `kReprojDepthShader`; WARP harness `drawid_harness.exe` 134981 /
  134981 (the probe is off in every scene: `FwdMode.w = 0`, u4 unbound, 4 UAVs as before); a manual S1 run with `HDUMP=40
  HPROBE=1` (new debugging aid; needs `build\tests\dlaa.ini` with `debug = 1`) wrote 25 probe lines (a cabin mover pixel, world
  movers, static world pixels, sky) whose values agree with the dump lines of the same pass; `lod_scope_test` 239 / 239 with 7
  new SV_Depth scan checks (built by hand: `tests\build_lod_scope_test.bat` does not link at HEAD -- it lacks `src\ofxr.cpp`,
  `src\dinput_wrap.cpp`, `dxguid.lib`, `dinput8.lib`; pre-existing, not changed here).
- **Not verified (needs the game):** the mirror selection in game (the next ATS VR log); the probe on real game data (VR
  crop, 1/2-resolution forward depth, forward ids); the SV_Depth flag on the game's shaders (shaders created before the
  `CreatePixelShader` hook read "unknown").
- **Next VR log, read:** `mirror units (mirror_vr_mode 2): selection now [...]` once or a few times, then the `mirror units
  mirror_vr_mode 2 selection @blit` line every 600 blits with `views matched` ~ 2 per frame (the selected keys `in ~F of F
  frames`), `not selected` = the small views only; the `mirror units @blit` line: `handled` growing, the units of the
  selected sizes with `dlaa ok` ~ their views. Pink window: look at it, press Alt+F8, then read the `MV probe` block: the
  pixels on the window should say `CABIN layer`, and the NOTE / GBUF part says which draw (vp depth, PS SV_Depth) put the
  cabin depth there; the `MV draw-ids dump ...: G-buffer pixel shaders` header says whether any G-buffer PS writes SV_Depth.
- **Risks:** the key selection assumes a view's size + order identify the same mirror every frame (true in the mode-1 log;
  a game that swaps the order of two equal-size mirrors between frames would swap their units -- the same as before for the
  unit match). A key that disappears stays selected for up to 120 frames with views (a mirror the game stopped drawing: the
  next largest is not promoted until then). The probe costs nothing outside the dumped pass (one uniform branch on
  `FwdMode.w`).

### Phase 22 (v0.10.0): the real MVP per draw -- the cb0 layout of each vertex shader (shop windows rows 6..9, car interior 5..8)

- **Evidence** ([`captures/shop_window_audit.md`](../captures/shop_window_audit.md), four ATS flat RenderDoc captures, the cb0 rows of
  every G-buffer and forward draw, VS disassembly): the game's vertex shaders do not all keep the MVP in cb0 rows 4..7. Normal world
  draws: rows 0..3 = model-view (row 3 = (0, 0, 0, 1)), rows 4..7 = MVP. The car interior (VS R3109, phase 19): row 4 = (0, 0, 0, 1),
  MVP in rows 5..8. Both shop-window families (fake-interior glass VS R38017 / R38019, reflective windows VS R37723 / R37725):
  r0 = object position + seed, r1 = parameters, r2..r5 = model-view, MVP in rows 6..9 (`dp4 o1.x..w, cb0[6..9], v0`). Phase 19's
  `NotMvp` caught only the car: the windows' misread rows 4..7 = [MV row 2, (0, 0, 0, 1), MVP row 0, MVP row 1] have the clip-y row
  as "row 3" (|xyz| ~ 2), so they paired with that wrong matrix -- static while parked (R = I), a state-3 mover with an R in the
  wrong coordinates as soon as the head moves (simulated: 0.05 deg of yaw -> 3.6 px probe deviation, pass B gives the window ~0 px
  where the truth is -2 px; 0.3 m of driving -> 98 px) = the wobbly pink shop windows in VR.
- **Layout rule = a SHAPE test, first passing block from row 4 up** (`ShapeOk`, kDrawIdCs): a 4-row block is an MVP when its clip-x,
  clip-y and clip-w rows (0, 1, 3) each have an xyz part > 0.02 x the longest of rows 0..2 (the clip-z row may be ~0: the infinite
  reversed-Z projection's (0, 0, 0, near)). A (0, 0, 0, 1) row inside a block fails it, so rows 4..7 fail for both shifted layouts
  (car: block row 0, windows: block row 1), rows 5..8 fail for the windows (block row 0). Checked against the audit's cb0 census
  (all four captures, every G-buffer and forward DrawIndexed): the scan picks rows 4..7 for every normal draw, 5..8 for the 4 car
  interior draws, 6..9 for every window draw (G-buffer and forward) -- exactly the disassembly; the smallest passing ratio min(|row
  0|, |row 1|, |row 3|) / longest is 0.478, the largest failing ratio 0.0 (every rejected block holds the exact (0, 0, 0, 1) row): a
  24x margin to the 0.02 limit. Every G-buffer VS had ONE layout in all four captures (the only VS with mixed results is the
  full-screen fog triangle: a forward `Draw`, never recorded). Why not the anchor ("the block after the (0, 0, 0, 1) row"): it fits
  the three layouts too, but needs rows 0..3 to be a model-view, which neither the harness nor every game shader guarantees; the
  shape test reads only the block itself and is phase 19's test extended by the two clip rows.
- **Mirror:** the recorded cb0 copy holds rows 0 .. min(window, 12) - 1 of every draw (`DrawIdRecord::AddRing` copies from the
  window's row 0; was rows 4..7 only; the mirror always held the whole ring, only the copied byte range grows by 64 B before / 128 B
  after each draw's MVP -- ring ranges are copied contiguously anyway). `MirrorOff` keeps its meaning (row 4); `MirrorRows` = the
  rows mirrored (Tab .z bits 16..19); the dump's `cb0 first N num M` stays the draw's window, `cb0 rows mirrored R` is new.
- **GPU (`CSGather`):** a draw whose VS has a cached start row (Tab .z bits 20..23) tests only that block -- passes = used (code 7),
  fails = no MVP for this draw (state 5 through `NotMvp`; the scan result is reported, not used). A VS not cached yet scans rows
  4..8 and uses the first passing block (codes 1..5), none = rows 4..7, which fail the shape = state 5 (code 6; phase 19's
  fallback). Everything downstream reads the gathered matrix (CurM / PrevM), so pairing, consensus votes / candidates, twins (the
  MVP hash), origin attach, the parent vote's sources, the origin-free rule, forward ids (the forward record's mirror, same scan),
  the static-skip verdicts and the dump's origin / col3 fields all use the found block with no change of their own. `NotMvp` =
  the chosen block fails `ShapeOk` (a degenerate block, rows 0..2 ~0, is left to `Inv4` as before). Per-draw 4-bit codes in Cnt
  dwords 832..1343 (Cnt 3328 -> 5376 B), counters Cnt [236..240] (G-buffer draws by block: rows 4..7 .. 8..11), [241] none,
  [242] cached block failed, [243] forward draws with a shifted block, [244] draws that scanned.
- **Per-VS cache** (`VsLay`, draw_ids.cpp; `DrawIdMv::MvpRow`): keyed by the VS pointer (the layout is a property of the VS, like the
  phase-12 PS discard flag), global (every unit -- both VR eyes, mirror units -- feeds it from its diagnostics readback through
  the per-draw codes; Prepare / Finish keep the draws' VS + cached row per readback slot). The FIRST read-back draw of a VS whose
  scan found a block decides it; a VS whose cached block fails in 8 draws in a row (no draw confirming it in between) that all
  found the same other block switches (a VS pointer reused by another shader; logged `MVP rows of VS ... CHANGED`). Draws that find
  no block never decide a VS. 4096 slots (3072 used at most; full = new VSes keep scanning, counted), cleared on a device change
  (`DrawIdRecord::ShutdownDevice`). Log once per VS: `MV draw-ids: MVP rows r..r+3 for VS <ptr> (layout: normal | +1 | +2 ...) --
  decided from its first read-back draw (... VS layouts known: normal N, +1 A, +2 B, ...)` -- every shifted VS (up to 64), the first
  16 normal ones (the rest are counted in the @blit field). Per-draw cost: one table lookup per change of VS between consecutive
  draws in Prepare, one extra 64-byte load + `ShapeOk` per draw on the GPU (the scan only for a VS not cached yet).
- **Other readers of "rows 4..7" changed:** pass A's camera candidates (inject.cpp, `CandidateRecord` copies) read the MVP at the
  VS's cached row (`VSGetShader` + `MvpRow`, only while that layer's medoid is not dropped), and pass A's `NotMvp` is the same 3-row
  shape (rejects a misread shifted layout while its VS is not cached yet); the forward-depth near-camera check (`FwdNoteMvp`) reads
  at the cached row too. The phase-21 dump / probe text (`mvp-shape`) follows the new rule. NOT changed: the v0.9 stencil-id path
  (`ObjRecord`, `mv_objects = 1`, legacy) still reads rows 4..7.
- **Kept:** phase 19's state-5 fallback (no MVP-shaped block), `mv_cabin_small`.
- **Log:** startup `phase 22 (v0.10.0): MVP layout per vertex shader -- ...`; `MV draw-ids @blit` cabin field: `MVP layout (phase 22):
  normal N / +1 A / +2 B / +3 / +4 / none C draws per frame (none = no MVP-shaped block + the VS's cached block failed: state 5),
  forward draws with a shifted block F, draws that scanned the rows S per frame; VS layouts known: normal .., +1 .., +2 .., undecided
  ..`; Alt+F8 dump: a header `MVP layout (v0.10.0 phase 22) of the G-buffer draws ...: rows 4..7 (normal) N, 5..8 (+1) A, 6..9 (+2)
  B, ... | VS table: ...`, every draw whose MVP is not in rows 4..7 gets a line, and every line says `MVP rows r..r+3 (layout X, the
  VS's cached block | found now: the VS was not cached) , VS <ptr>, cb0 rows mirrored R` (or `NO MVP-shaped block ...` / `the VS's
  cached rows .. FAILED the shape -> no MVP this draw`).
- **Harness** (WARP): S17 now pairs the interior's big draws with their rows-5..8 MVP: they are static cabin draws and cabin voters
  (want state 2, was 5; the shape count 0, the +1 layout count 4 per readback, the per-VS table row 5), and the left door OPENS from
  frame 50 (hinged at its front end) -- a cabin mover with its own motion (48 of 48 frames a mover, 0 of 3.4 M door px off its own
  motion). The pass-A medoid is now a correct cab-fixed draw and is KEPT by the small-cabin rule (S17 check: the rule ran in every
  readback; S17b still needs the replacement). New **S18 shop windows** (`drawid_harness.exe 18`, after S17 in the full run): 12
  facades, 40 posts, 10 boxes, six windows in two families (two G-buffer VSes) on the facades, each window ALSO drawn as a forward
  reflection (a third VS, recorded in the forward record: 1 cm in front of the glass on the odd windows -- the forward id is used --
  and 1 cm behind it on the even ones -- the G-buffer id is used), all with the shop-window cb0 layout (MVP rows 6..9, the audit's
  r0 / r1 / model-view rows), the camera driving 0.3 m / frame and yawing 0.05..0.5 deg / frame with a pitch wobble, an overtaking car.
  Result: 1282 / 1282 -- 0 of 496232 window px off the EXACT camera motion (worst 0.016 px; 148988 via the forward id, 347244 via the
  G-buffer id), 1176 / 1176 window draw-frames static, 0 movers, the +2 count 6 / forward-shifted 6 / none 0 per readback, the
  three window VSes at rows 6..9 in the table, the main VS at 4..7. **Mutation `mutlayout`** (SetParentMutation bit 6 = FoldU.w bit
  5 / pass-A Params.w 2: rows 4..7 + the phase-19 rule everywhere = phase 21): S18 FAILS (122 window draw-frames movers, 840 states
  wrong, 7063 window px off by up to 12.8 px), S17 FAILS (the big draws state 5, the opening door never a mover: 1.66 M of 3.4 M door
  px off by up to 43.8 px, shape count 4, layout count 0). `mutshape` (phase 19's fallback off) no longer fails S17 (the detection
  finds the block; the fallback is unused there) and passes S18; `cabsmall=0` still fails S17 and S17b. S17 / S18 also pass with
  `fold=0`, `fold=1`, `nocompact`, `legacy`, `cabsmall=0` (S18) and under the D3D11 debug layer (0 messages). Full run 136218 /
  136218 (snap 0.1 and 0.02; was 134981: S17 1184 -> 1137 checks, S17b 694 -> 696, + S18 1282), `rstatic=4` 136230 / 136230.
- **Bit-identical:** the full run's `cmpdump` of S1-S16 (1598 frames, the first 3139430400 bytes) is BYTE-identical to HEAD 99a361c's
  (rebuilt from `git archive`); the first difference is S17 frame 3 (the interior now paired). Scene 8 alone: HEAD / HEAD / new
  identical.
- **Not verified (needs the game):** the layout and the per-VS table on real game draws (only the audit's four flat captures were
  replayed through the rule, offline); the VR head-motion case in game; ETS2 (the audit is ATS); the cost on the GPU (WARP numbers
  only; the change adds one 64-byte load + ~10 ALU per draw and ~2 atomics per draw to CSGather).
- **Next VR log, read:** `phase 22 (v0.10.0): MVP layout per vertex shader ...`; `MV draw-ids: MVP rows 6..9 for VS ... (layout: +2)`
  -- expect one line per shop-window G-buffer VS that has been in view (2 in the audit scenes: R38017, R37723; the forward window
  draws are not recorded, so R38019 / R37725 appear only if a forward-depth batch draw uses them) and `MVP rows 5..8 ... (layout: +1)`
  once in the car; no `CHANGED` lines. In `MV draw-ids @blit`: `MVP layout (phase 22): normal N / +1 ~4 (in the car) / +2 ~1-6 (shop
  windows in view) / ... / none ~0`, `draws that scanned the rows` ~0 after the first seconds, and `rows 4..7 not an MVP 0.0
  draws/frame (cabin 0.0)` (phase 19's 4 car draws are now read correctly). Ctrl+F6 with the head moving: the windows no longer
  magenta (static: the world's static colour, olive where the static gate skipped them), the car interior the static cabin tint (not
  grey state 5), a door opening magenta (its own motion). Alt+F8 on a window: `MVP rows 6..9 (layout +2, the VS's cached block)`,
  `mvp-shape ok`, state 2 (static).
- **Risks:** (1) a shader whose cb0 holds another 4x4 with clip-like rows BEFORE its MVP (rows 4..7 an unrelated matrix, MVP at
  8..11) would be read at the wrong block -- not seen in the audit. (2) The first read-back draw decides a VS; a wrong first decision
  (a degenerate matrix that happens to pass elsewhere) needs 8 consecutive contrary draws to switch back; meanwhile that VS's draws
  are state 5 (no MVP, camera R), not a wrong mover. (3) A VS not cached yet scans per draw for the 2-3 frames until its first
  readback lands (correct block, just uncached). (4) Pass A candidates of a VS not yet cached read rows 4..7 for those frames (the
  3-row shape rejects a misread shifted layout). (5) The interior's real MVP now makes car-interior parts that move (doors, wheel)
  movers with their own motion -- correct, but the small-cabin rule's heaviest cluster can now include more draws; a big moving part
  (a steering wheel while steering) is excluded as a mover only when it disagrees with the cluster.

### Phase 23 (v0.10.0): the jitter's unrendered edge column / row (sun haze blinking on the right side)

- **Bug** (Ctrl+F10 depth snapshots, ATS VR, eye 3832x4064): on frames with a negative Halton shift in x the whole last pixel
  column of the scene depth stayed at the clear value (sky), with a negative shift in y the whole bottom row. The game's sun-shaft
  sky mask (1 where depth == 0) and bloom read it as a bright sky line and smeared it toward the sun: the haze blinked on about
  half of the 8 phases.
- **Edge rule, measured** (scratch D3D11 test, WARP and RTX 5090, 64x48 and 3832x4064, full-screen draw, x and y separately):
  the rasterizer cuts the far edge at floor(TopLeft + size) and the near edge by the pixel-centre rule. W x H viewport: any
  shift < 0 (even -0.001) loses the last column / row, any shift > +0.5 the first; safe domain [0, +0.5] -- half the Halton span,
  so a constant bias cannot fit the offsets in ([0, 1) would blink on the left / top edge instead). (W+1) x (H+1) viewport with
  the same top-left: safe domain [-1, +0.5], which holds the whole [-0.5, +0.5) range. Table: `captures/phase23_jitter_edge.md`.
- **Fix:** `ReconcileViewports` adds 1 px to Width / Height of viewport 0 of every jittered world and mirror pass
  (`g_jitterOverscan`, dlaa.ini `jitter_overscan`, default 1; 0 = phase 22). TopLeft = the shift, unchanged, so the NGX jitter
  (sign x shift, inside the DLSS guide's [-0.5, 0.5]), the draw-id jitter key and the replays are unchanged; the picture is scaled
  by (W+1)/W about its top-left corner, identically on every frame (<= 1 px at the right / bottom edge; MVs from the matrices are
  off by MV / W). The preview passes keep W x H and their round 4/5 edge fix (tile shifts up to +-0.875 px).
- **Logs:** startup `jitter (v0.10.0 phase 23): viewport shift in [-0.5, +0.5) px with the jittered world / mirror viewport 1 px
  larger ...`; Ctrl+F10 `info_*.txt`: `viewport_overscan=1` and `depth_edge_sky=col0 a/H, colW-1 b/H, row0 c/W, rowH-1 d/W`
  (texels at 0 per edge of the snapshot depth; a whole edge at 0 = the bug).
- **Harness:** knob `overscan` = all jittered scene viewports (W+1) x (H+1). Default run unchanged 136218 / 136218. With
  `overscan` 136206 / 136215: S1-S3 fail one draw-frame (frame 34, an unpaired culled grass clump next to a spinning wheel within
  ~4 % depth wins a 34/34 rigid-parent vote for the wheel; W x H: 0 of 46, no parent). Not an overscan defect: the same failure
  appears with the W x H viewport when the whole raster is offset by a constant (0.48, 0.57) or (0.05, 0.4) px (a temporary
  `HJOFF` experiment, not kept); offsets (0.25, 0.25), (0.1, 0.1), (-0.3, -0.3) pass. Pairing is identical in both modes
  (`HDUMP=34` dumps). A sub-pixel sensitivity of the phase-6c vote for that scene.
- **Not verified (needs the game):** that the haze no longer blinks; mirrors with the overscan; ETS2.

## Roadmap / history

| Ver  | Goal | Key work |
|------|------|----------|
| v0.1 | **Injection proof** ✅ | dinput8 proxy + Present vtable hook + logging |
| v0.2 | Buffer capture ✅ | RenderDoc capture: see [`CAPTURE_FINDINGS.md`](CAPTURE_FINDINGS.md) |
| v0.3 | DLAA on flat 🔧 (built, needs in-game test) | Draw + OMSetRenderTargets hooks find the tonemap draw; NGX **D3D11** DLAA at scale 1.0 on the pre-UI image, zero MVs, reversed-Z depth via a compute copy |
| v0.3.1 | Jitter + End toggle 🔧 | viewport-offset Halton jitter in the scene passes, offset passed to NGX, live toggle, `dlaa.ini` |
| v0.4 | Motion vectors 🔧 (built, needs in-game test) | per-layer clip-space reprojection from per-draw MVPs (`MVP_prev * inverse(MVP_cur)`, medoid of matched static draws); dynamic objects later |
| v0.5 | VR eyes 🔧 (built, needs in-game test) | per-eye blit trigger + per-eye depth snapshot (ClearDepthStencilView hook) + one DLSS feature / CameraMv per eye; see "VR / per-eye" |
| v0.6 | Overlay | ImGui tuning UI (sharpness, preset, on/off), persisted |
| v0.7 | DLSS upscaling 🔧 (built, needs in-game test) | render at `r_scale` size, NGX to the eye texture size, composited after the game's blit; see "DLSS upscaling"; v0.7.2 adds the plain-`End` mode cycle |

### Phase 24 (v0.10.0): the VR jitter phase parity -- a parked truck wobbled, each eye saw only half of the Halton positions

- **Evidence** (ATS VR 2026-10-10, `E:\OpenXR-EyeCapture\captures\amtrucks_20261010_101035_SBS.mkv`, Ctrl+F10 `dlaa_snap\`): the user
  saw a parked pickup in front wobble slightly, no magenta. The four snapshot frames (passes e0 e1 e1 e0) showed the per-draw motion
  vectors on the truck CORRECT (the previous frame warped by `mv - jitter delta` lands on the current one, best of a 2D search; the
  DLAA output follows the same shift), but the Halton phases were eye 0: 0, 2 and eye 1: 1, 1. `n.phase = (s >> 1) % 8` pairs the
  pass serials (2k, 2k+1) as one frame; the eye order flips between frames (e0 e1 | e1 e0), so once the serial parity is off by one
  (any odd pass: menu, loading) the pairs straddle two frames: each eye gets only the even or only the odd phases (in x all of one
  sign), each twice in a row -- half the sample positions and a 2-frame rhythm on every bright edge.
- **Fix** (`g_vrPhaseOff`, inject.cpp pass start + the blit's eye site): `phase = ((s + off) >> 1) % phases`. The blit knows the eye of
  pass s; two consecutive passes of the SAME eye belong to two frames, so the boundary sits between them and `off = s & 1`. Learned
  again at every such pair (no state to get stuck). Log: `jitter (VR): phase parity -> N at pass s=...` (first 8) and
  `jitter (VR) @blit N: phase parity P, corrections C` every 600 blits. In the 10:47 log: ~150 corrections while the loading
  screens had both passes on eye 0, then stable; Ctrl+F10 at 10:48: eye 0 / eye 1 phases 4 / 4 then 5 / 5. User: the truck is good.
- **Not the mod: pole edges crawl / sparkle in VR** (same run): the user sees it unchanged in passive mode (Ctrl+F7 = no mod work),
  so it is the game's own aliasing of sub-pixel poles; a 2-frame lamp-post check (driving, 8.75 px/frame) showed the per-draw
  motion vectors right within 0.15 px and the output following. Levers: a stronger model (Shift+F1), less RCAS (Shift+F7), the
  texture LOD bias back toward 0 (Ctrl+F2).
- **Static replay skip** (`mv_replay_static_frames = 0` test in the ATS ini): the user "thinks" the ghosting was less. Not proven;
  left off in the ATS ini for now (GPU +0.15..0.3 ms / pass, 72 fps held).
- **RenderDoc is blind to all of this**: under RenderDoc NGX fails (PlatformError) and phase 12 holds the jitter, so a capture has
  no mod work in it.

### The RenderDoc oracle (done in v0.2)

Capture a frame of the unmodified game to learn empirically: which pass writes
the final HDR color, where the depth buffer is, and where the scene is blitted to
the screen / eye texture. Identify buffers by **format/size/usage**, not draw-call index, so the
injector survives game updates that break index-based hooks.

---

## Risks / notes

- **No anti-cheat** in ETS2/ATS single-player → DLL injection is safe. Convoy
  multiplayer / TruckersMP have their own rules — check before using online.
- **NVIDIA redistributables** (`nvngx_dlss.dll`, Streamline `sl.*.dll`) have their
  own license — fetch at build time, don't commit them (see `.gitignore` and `THIRD_PARTY.md`).
- DLAA requires an **RTX** GPU.
- Present is hooked with MinHook trampolines (inline detour on dxgi's Present),
  not the shared swapchain vtable: the Steam overlay (`gameoverlayrenderer64.dll`,
  always injected by Steam) detours dxgi Present and calls back through the
  vtable, which made a vtable hook recurse forever. A thread-local depth guard
  remains as a safety net.
