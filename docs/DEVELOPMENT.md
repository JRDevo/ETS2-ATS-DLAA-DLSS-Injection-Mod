# ETS2 / ATS DLAA Injector — development notes

Internals and history of the injector. For installing and using the mod, see the
top-level [`README.md`](../README.md). Companion docs:

- [`DLAA_INTEGRATION.md`](DLAA_INTEGRATION.md) — NGX/DLSS integration, jitter and motion-vector details.
- [`CAPTURE_FINDINGS.md`](CAPTURE_FINDINGS.md) — RenderDoc findings about the ETS2/ATS frame (buffers, passes, VR eye structure).
- [`KEYS.md`](KEYS.md) — the hotkey cheat sheet.

A from-scratch DLAA anti-aliasing injector for Euro Truck Simulator 2 and
American Truck Simulator (Prism3D engine, DirectX 11.1), targeting both flat and
VR (Virtual Desktop / SteamVR).

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
> launch options at load (`-openvr` / `-openxr` / `-oculus` = VR; log `launch mode: VR|FLAT`); flat never
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
- **No OpenVR/OpenXR calls** — VR is handled by hooking at the **D3D11
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
4. **VR test:** add `-openvr` (or `-openxr` for VDXR) to the Steam launch options,
   start Virtual Desktop + SteamVR, launch. Confirm `Present` still fires (its resolution is the
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
| `Shift+F5` | DLAA area −10 % (40..100) | 3 beeps at `300 + 6·area` Hz; low 200 Hz at the 40 limit |
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
| `Ctrl+F6` | MV debug view (blit source = `(0.5+mv.x/16, 0.5+mv.y/16, cabin?1:0)`; gray = still, blue = cabin; needs DLAA on; v0.7.0 upscale: drawn by the composite, render-size MVs scaled up to output size) | — |
| `Ctrl+F7` | Passive mode on/off (no jitter / MV copies / depth snapshot / evaluate / readbacks; tracking + GPU timers only) | 1 low tone = ON, 2 = OFF |
| `Ctrl+F8` | Jitter-only debug (jitter on, DLAA evaluate skipped = raw jittered image) | — (DLSS history reset) |
| `Ctrl+F9` | Self-test (cycles OFF, the 4 jitter signs, jitter-only; dumps lossless BMP frames to `dlaa_selftest\`; v0.7.0 upscale: the DLAA modes dump the DLSS output at output res, OFF / jitter-only the raw render-res frame) | — |
| `Ctrl+F10` | NGX input snapshot (2 frames to `dlaa_snap\`: color-in / out BMP, MV / depth `.bin`, info txt; needs DLAA on; v0.7.0 upscale: `out` = the DLSS output rect at output res) | — |
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
  | `dlaa_area` | `100` | int 40..100 (v0.6.4): DLAA runs on a rect of this % of each eye image's width AND height (pixel count ~ area²), centred on the eye's optical centre; `100` = whole image (v0.6.3 behaviour); `Shift+F5`/`F6` adjust; written by `Shift+F12` |
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
