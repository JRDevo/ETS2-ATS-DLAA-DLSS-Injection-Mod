> ## THIS GITHUB PAGE IS THE ONLY PLACE THIS MOD EXISTS
> **`https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod` is the only source of truth for this mod: the only place it is
> published, the only place it is documented, and the only place to download it (its [Releases](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases) page).**
>
> - There is **no website** for this mod. There **never will be** one. No Discord, no Patreon, no Nexus, no Steam Workshop, no app store.
> - **DO NOT DOWNLOAD THIS MOD FROM ANYWHERE ELSE.** Any other site, mirror, "launcher", "installer", video link or forum attachment that
>   offers it is not mine and may carry anything. If it did not come from this GitHub page, do not run it.
> - Updates, release notes, the hotkey list and the troubleshooting guide live here and only here. If someone tells you to get the mod
>   somewhere else, they are wrong.

**I've removed my post from Reddit, you can't be constructive there.**

# ETS2 & ATS: DLAA & DLSS Injection Mod

An **ETS2 DLSS mod** and **ATS DLSS mod**: NVIDIA DLAA and DLSS upscaling for
**Euro Truck Simulator 2 (ETS2)** and **American Truck Simulator (ATS)**, on a flat
screen and in VR, with Windows HDR support. It replaces the game's own anti-aliasing with
DLAA (NVIDIA's native-resolution AA) and can optionally upscale with DLSS so the
game renders fewer pixels and the GPU has an easier time.

It installs as a small `dinput8.dll` proxy in the game folder. No launcher, no
account — a few hotkeys, an in-game tuning menu (`Delete`) and an optional text config.

**An NVIDIA RTX GPU is required** (DLAA and DLSS only run on RTX cards).

**Menu:** [Features](#features) · [Download](#-download) · [Requirements](#requirements) · [Install](#install) · [Modes](#modes--the-end-key) · [Flat or VR](#flat-or-vr) · [OFXR Bridge](#ofxr-bridge-vr-frame-generation) · [Render scale](#how-render-scale-works) · [Models](#models-presets--which-one-to-use) · [Controls](#controls) · [Tuning menu](#the-tuning-menu) · [`dlaa.ini`](#dlaaini) · [Performance](#performance) · [Troubleshooting](#troubleshooting) · [Multiplayer](#multiplayer) · [Build](#build-from-source) · [Report a problem](#reporting-a-problem-what-to-upload) · [License](#license-and-credits)

## Features

- **DLAA** — NVIDIA anti-aliasing at the game's render resolution. It removes the shimmer and flicker on
  fences, power lines, road markings and shadows that the game's own AA leaves behind.
- **DLSS upscaling** — render fewer pixels and let DLSS rebuild the full picture, for more frame rate.
- **Flat and VR** — one DLL and one `dlaa.ini` for both. VR runs one pass per eye (OpenXR).
- **In-game tuning menu** — new in v0.10.0: press `Delete` for a panel with every setting, its value, its hotkey
  and a short description; `Up` / `Down` select, `Left` / `Right` change. Flat and VR (both eyes); the game does
  not see those keys while the menu is open. See [The tuning menu](#the-tuning-menu).
- **Windows HDR** — works with HDR on in flat mode (new in v0.8.0).
- **NVIDIA Smooth Motion** — works with the driver's frame generation switched on, in flat mode
  (new in v0.8.1).
- **OFXR Bridge (VR frame generation)** — runs together with the bridge, which adds in-between frames to the
  finished eye pictures (new in v0.10.0, see [OFXR Bridge](#ofxr-bridge-vr-frame-generation)).
- **Drivable cars (ATS)** — DLAA / DLSS also runs while you drive a car (new in v0.8.2).
- **Menu and truck-preview screens** get anti-aliasing too, not only the drive.
- **Four DLSS models** to pick from, switched live with a key, from light to heavy.
- **Exact motion for moving traffic** — new in v0.10.0: every part of the 3-D world is matched with the
  previous frame, so traffic, wheels, trailers and licence plates get their own motion vectors and stay
  sharp under DLAA / DLSS instead of ghosting; everything static keeps the camera motion, which is taken from
  the scenery as a whole (a moving truck can no longer pass for the camera). See-through parts drawn after
  the scene (plates, decals, glass, wires) get their own motion too, and parts the game rebuilds every frame
  (licence plates) take the motion of the vehicle they sit on. On by default (`mv_objects = 2`).
- **Mirrors anti-aliased too** — new in v0.10.0: every truck / car mirror gets its own DLAA with its own
  jitter and motion vectors (your trailer in the mirror stays sharp), on by default (`mirror_dlaa = 1`,
  `Alt+F6` switches it live).
- **DLAA on the game's HDR colours** — new in v0.10.0: in flat DLAA mode the main picture is anti-aliased
  before the game's own tone-mapping, the input NGX is made for, so thin bright lines shimmer less. On by
  default (`dlaa_pre_tonemap = 1`, `Alt+F7` switches it live); DLSS upscaling, VR and Windows HDR keep the
  end-of-frame path.
- **Sharper texture text** — texture LOD bias, new in v0.9.0: road signs, dashboard, GPS and truck decals
  get sharper textures while DLAA / DLSS keeps them from shimmering. Off by default, adjust live with
  `Ctrl+F1` / `Ctrl+F2`.
- **Adjustable sharpening** — strength and width, live.
- **DLAA area (VR)** — process only the middle of each eye to save GPU time.
- **Hotkeys with beeps** — made to use blind in a headset; every key can be re-bound in `dlaa.ini`.
- **Bring your own `nvngx_dlss.dll`** — drop in a newer one at any time to get NVIDIA's newer models.
- **Small and simple** — a single `dinput8.dll`. No launcher, no account. Delete the file
  to uninstall.
- **Debug log** for bug reports (off by default).

## ⬇ Download

**[Download the latest release here](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest)**
— get the `.zip` under **Assets**, then follow [Install](#install) below.
(The green **Code** button downloads the source code, not the mod.)

This is an unofficial fan project. It is not affiliated with, or endorsed by, SCS
Software or NVIDIA. Euro Truck Simulator 2 and American Truck Simulator are
trademarks of SCS Software; DLSS and DLAA are trademarks of NVIDIA Corporation.

> Status: v0.10.0. Exact per-part motion vectors for moving traffic and DLAA on the truck / car mirrors (both new in v0.10.0, on by default; first tested in ETS2 flat, the camera motion is now taken from the scenery as a whole and licence plates / decals / glass get their own motion; plates that take their vehicle's motion and DLAA before the game's tonemap in flat DLAA mode were tested in ETS2 flat; parts that take the motion of the part drawn with the same placement were tested in ETS2 flat; the newest round -- licence plates the game rebuilds every frame take the motion of the vehicle part they sit on -- is not yet tested in game). Sharper texture text with the texture LOD bias (since v0.9.0, off by default). Works while you drive a car in ATS (since v0.8.2). Works with NVIDIA Smooth Motion on in flat mode (new in v0.8.1). Windows HDR works in flat mode (since v0.8.0). DLAA has been run in ETS2 (VR) and ATS (flat and VR). The DLSS
> *upscaling* path (render below native, reconstruct up) runs in VR but is still
> being tuned. Treat upscaling as experimental.

---

## Requirements

- Euro Truck Simulator 2 or American Truck Simulator (64-bit, DirectX 11).
- An **NVIDIA RTX** GPU with a current driver.
- `nvngx_dlss.dll` — the NVIDIA DLSS runtime. **This mod does not ship it**; see
  Install below.
- For VR: an OpenXR runtime (for example Virtual Desktop), launched with the game's `-openxr`
  option.

## Install

1. **Make a backup** of the game's `bin\win_x64\` folder first.
2. [Download the latest release zip](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest)
   and unzip it.
3. Find the game's executable folder, e.g.
   `...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\`
   (or `...\American Truck Simulator\bin\win_x64\`).
4. **Remove any other proxy mod first.** Only one `dinput8.dll` / `dxgi.dll` proxy
   can load — delete any other mod's `dinput8.dll` or `dxgi.dll` from that
   folder before continuing.
5. Copy **`dinput8.dll`** into `bin\win_x64\`. Optionally also copy **`dlaa.ini`**
   there to change settings or keys (see below) — it is not required.
6. **Add the NVIDIA DLSS runtime yourself.** This mod ships **no** NVIDIA files.
   Put your own **`nvngx_dlss.dll`** in the same `bin\win_x64\` folder, next to
   `dinput8.dll`. You can get it from:
   - the NVIDIA DLSS SDK on GitHub — <https://github.com/NVIDIA/DLSS>, file
     `lib/Windows_x86_64/rel/nvngx_dlss.dll`, **or**
   - any game that already ships `nvngx_dlss.dll` (copy that file).

   You can drop in a **newer** `nvngx_dlss.dll` at any time to update the DLSS model
   — just replace the file.
7. Launch the game. DLAA starts on. No log file is written unless you set
   `debug = 1` in `dlaa.ini` (see Troubleshooting).

To uninstall, delete `dinput8.dll` (and `dlaa.ini`) from `bin\win_x64\`.

## Modes — the `End` key

Press **`End`** in game (no modifier) to cycle the mode. The choice is saved to
`dlaa.ini` immediately and used again next launch.

| Mode | What it does | Beep |
|------|--------------|------|
| **DLAA** | NVIDIA AA at the game's render resolution; the game stretches the result to the output size | 1 high beep |
| **DLSS** | NVIDIA reconstructs from the render resolution **up** to the output size (upscaling) | 2 high beeps |
| **off** | the game's own picture, no injection | 1 low beep |

At 100 % render scale DLAA and DLSS produce the same picture; the difference only
shows once you render below native (see render scale below).

## Flat or VR

The mod reads the game's launch options at start. With `-openxr` or `-oculus` it runs in VR mode
(one DLAA / DLSS pass per eye; the desktop mirror window is left alone). Without them it runs in flat mode.
The log says which: `launch mode: VR` or `launch mode: FLAT`. One `dlaa.ini` serves both.

If the launch options say VR but no headset session starts (the game then runs on the monitor), the mod
notices after about one second of driving and switches to flat mode by itself (`launch mode FALLBACK` in the log).

## OFXR Bridge (VR frame generation)

[OFXR Bridge](https://github.com/djules75/OFXR-Bridge) is a separate program that adds in-between frames in VR: it takes the finished
picture of each eye and generates the frames between them from optical flow. From v0.10.0 the mod runs together with it. The mod
anti-aliases / upscales each eye as usual; the bridge then works on the finished eye pictures. It needs no motion vectors from
the mod. The mod only makes sure the bridge's own copies inside the game are left alone (they are not counted as game drawing).

- **Order:** install the mod as usual. Start `OFXRBridgeTray.exe` and arm it **before** you launch the game with `-openxr`.
  Nothing to change in `dlaa.ini`.
- **Never run the game as administrator.** The bridge installs a per-user layer, and Windows ignores per-user layers for a
  program that runs as administrator, so the bridge would silently do nothing.
- **Switch off:** `ofxr_bridge = 0` in `dlaa.ini` switches the mod's handling off (default 1). It does nothing when the bridge is not
  there.
- **In the log** (`debug = 1`): `OFXR Bridge: layer module ... loaded` when the bridge's layer shows up in the game (it loads when the
  game starts its VR session, so it can appear a little after start), `OFXR Bridge: tray.ini found` / `not installed`, and
  `OFXR Bridge: N calls passed through so far`.

Status: v0.10.0 — the handling is built but not yet tested in a headset with the bridge armed; report what you see with `debug = 1`.

## How render scale works

The game renders the 3-D scene at `r_scale_x` × `r_scale_y` of the output size and
then stretches it up. In **DLSS** mode this injector replaces that stretch with DLSS
reconstruction, so lowering the render scale buys performance with little quality
loss.

Set **both** `r_scale_x` and `r_scale_y` (game closed) in
`Documents\Euro Truck Simulator 2\config.cfg` (or `Documents\American Truck
Simulator\config.cfg`), e.g. `uset r_scale_x "0.667"` and `uset r_scale_y "0.667"`:

| DLSS mode | `r_scale_x` = `r_scale_y` | pixels rendered |
|-----------|---------------------------|-----------------|
| Quality | `0.667` | ~44 % |
| Balanced | `0.58` | ~34 % |
| Performance | `0.5` | ~25 % |

The injector picks the matching DLSS quality level automatically from the ratio.
In **DLAA** mode the render scale just sets the resolution DLAA runs at (no
upscaling).

**VR:** the per-eye output size is set by the game's VR supersampling cvar
`r_manual_stereo_buffer_scale` (and your headset/runtime resolution); `r_scale_x` /
`r_scale_y` then shrink the render below that eye size for DLSS to reconstruct back
up to it.

## Models (presets) — which one to use

A "model" (NVIDIA calls it a *render preset*) is the neural network that cleans
the picture. All of them remove jagged edges; they differ in how sharp and stable
the result is and in how much GPU time they cost. Switch live with
`Shift+F1`..`F4` (1 to 4 beeps), look for a few seconds, then save your choice
with `Shift+F12`.

| Key | Model | In one line | Cost |
|-----|-------|-------------|------|
| `Shift+F1` | **1** (default, preset `K`) | Best detail | High |
| `Shift+F2` | **2** (preset `E`) | Good picture, fast. **Start here** | Low |
| `Shift+F3` | **3** (preset `F`) | Smoothest edges, but can smear moving detail | Low |
| `Shift+F4` | **4** (preset `M`) | Very heavy. Flat screen only | Very high |

### Recommended settings

**Scaling** is the game's own slider: *Options → Graphics → Scaling*. **Stereo
buffer scale** is VR only and is not in the menu: with the game closed, set
`uset r_manual_stereo_buffer_scale "1.5"` (for example) in `config.cfg`.

| Your setup | Mode (`End`) | Model | Scaling (in game) | Stereo buffer scale |
|------------|--------------|-------|-------------------|---------------------|
| **Flat**, strong card | DLAA | 1 | 100 % | not used |
| **Flat**, need more frame rate | DLSS | 1 or 2 | 75 % (or 50 %) | not used |
| **VR**, top-end card | DLAA | 2 | 100 % (75 % if the frame rate drops) | 2.0 |
| **VR**, mid-range card | DLSS | 2 | 75 % | 1.0 to 1.5 |

- **Flat:** do not set Scaling above 100 %. The mod already removes the jagged
  edges, so extra pixels only cost frame rate.
- **VR:** the stereo buffer scale is what makes the picture sharp; `1.0` is the
  headset's own resolution, higher is sharper and heavier. Raise it as far as
  your card allows, then use Scaling below 100 % with **DLSS** mode to win the
  frame rate back.
- **Scaling below 100 % only makes sense in DLSS mode.** In DLAA mode the game
  just stretches the smaller picture and it looks soft.
- The slider has fixed steps. Values in between (for example `0.667`) can be set
  in `config.cfg`, see *How render scale works* above.
- These are starting points. Only the "VR, top-end card" row is measured: model 2
  holds a steady 72 fps at stereo buffer scale 2.0 with `r_scale_x` 0.75. Watch
  your own frame rate and move one step up or down.

How to pick a model:

1. Start with **model 2**. Drive for a minute. Watch thin things in the distance:
   power lines, fences, lamp posts, road markings.
2. Frame rate is still fine and you want more detail? Try **model 1**. If the
   frame rate drops (in VR: stutter or reprojection), go back to 2 — or, in VR,
   make the DLAA area smaller with `Shift+F5` so model 1 only works on the middle
   of the view.
3. Still shimmer on model 2? Try **model 3**. If the road or fences look smeared
   while moving, go back to 2.
4. Press `Shift+F12` to save.

What to expect:

- **No model makes the picture sharper than what the game renders.** They remove
  crawling and flicker on edges. If the picture is soft, raise the render scale
  (see above) or the sharpening (`Shift+F7` / `F8`); do not look for a "sharper
  model".
- **Cost grows with resolution.** As an example, on a top-end card at a very high
  VR resolution (about 4600 × 6500 per eye) we measured per eye: model 2 and 3
  about 1 ms, model 1 about 2.5 ms, model 4 about 9 ms. At 72 Hz a frame has
  14 ms in total, for both eyes and the game. On a flat 1440p or 4K screen all
  of them are much cheaper.
- **Things that move on their own** (traffic, wheels, wipers) can show a little
  ghosting with any model. The mod knows how the camera moves, not how each
  object moves.
- **A model change takes one short hitch** (a few hundredths of a second) while
  the new network loads.
- **The menu and truck-preview screens always use model 2.** The model keys only
  change the picture while driving.
- Which models exist depends on the `nvngx_dlss.dll` you installed. `E` and `F`
  are older models that newer DLSS files may drop one day; if a model cannot be
  created the mod falls back to model 1 by itself. More letters (`J`, `L`) can be
  set with `dlss_preset` in `dlaa.ini`.
- The same model is used in **DLAA** and **DLSS** mode. The mode (`End` key)
  decides *what resolution* the model works from; the model decides *how* it
  cleans the picture.

## Controls

**You only need three keys** — or the `Delete` menu, which has every setting on one panel. Everything else is tuning,
and the defaults are tuned already.

### Daily keys

| Key | What it does |
|-----|--------------|
| `End` | Mode: DLAA → DLSS → off (saved at once) |
| `Alt+F9` | Performance profile: high → medium → low. high = everything on; medium = for mid-range GPUs (model E, cheaper mirror / see-through / plate work); low = for weak GPUs (also no mirror DLAA, no see-through depth, VR DLAA area 60 %) |
| `Shift+F12` | Save the current live settings to `dlaa.ini` |
| `Delete` | Tuning menu (v0.10.0): every setting with its hotkey; arrow keys select and change, see [The tuning menu](#the-tuning-menu) |

Everything else is tuning. The defaults are tuned already; you can ignore the rest.

**Every key can be changed.** Open `dlaa.ini`, find the `key_<action>` line for the
action and edit the key, e.g. `key_dlaa_toggle = Ctrl+D` (or `none` to turn it
off). Restart the game. Full list: [`docs/KEYS.md`](docs/KEYS.md).

By default every control except `End` and the menu keys (`Delete`, arrows) is an **F-key with one modifier**, so it can be used
blind in a headset. The game window must be focused. Each key beeps to confirm (pitch tracks
the value); a long low tone means "already at the limit" or "save failed". Turn beeps off with
`beeps = 0` in `dlaa.ini`.

### Tuning keys

<details>
<summary>Show the tuning keys</summary>

| Default key | What it changes, and when you would touch it |
|-----|--------|
| `Shift+F1`..`F4` | DLAA model / preset: 1 = default (driver pick), 2 = `E`, 3 = `F`, 4 = `M`. Touch it if the picture shows ghosting or smearing and you want to try another model |
| `Shift+F5` / `F6` | DLAA area − / + 10 %, `20`..`100` % (shrinks the processed region; VR only, flat always uses the whole picture). Touch it in VR to trade picture quality for GPU time |
| `Shift+F7` / `F8` | Sharpening strength − / + 0.1. Touch it if the picture looks too soft or too crunchy |
| `Shift+F9` / `F10` | Sharpening width − / + 0.5. Touch it together with strength, only if you changed the sharpening |
| `Shift+F11` | DLAA on / off. Touch it to compare with and without |
| `Ctrl+F1` / `F2` | Texture sharpness: LOD bias − / + 0.25 on the solid surfaces (road signs, dashboard, GPS), `−3`..`0`; only while DLAA / DLSS is on. Touch it if sign and dashboard text looks soft; try −0.5 to −1 (new in v0.9.0) |
| `Ctrl+Shift+F1` / `F2` | Cut-out sharpness: LOD bias − / + 0.25 of the see-through draws (wire-mesh fences, grass, leaf cut-outs), `−3`..`0`. Touch it if fences moire or grass looks soft (new in v0.10.0) |
| `Alt+F5` | Per-part motion vectors on / off. Touch it only to compare traffic ghosting with and without |
| `Alt+F6` | Mirror DLAA on / off. Touch it if the mirrors look wrong or cost too much GPU time |
| `Alt+F7` | DLAA before / after the game's tone-mapping (flat mode). Touch it only to compare thin bright lines (lane paint, wires, rails) |
| `Ctrl+F3` | Wire depth (forward depth for the motion vectors) on / off. Touch it only to compare wires, fences and lane paint |
| `Ctrl+F4` | DLSS upscale on / off. Touch it to force DLAA-only while the render scale is below 100 % |

</details>

### Debug keys

<details>
<summary>Show the debug keys</summary>

These need `debug = 1` in `dlaa.ini`. They are for diagnostics and bug reports; you can ignore them when playing.

| Default key | Action |
|-----|--------|
| `Ctrl+F5` | Motion vectors on / off |
| `Ctrl+F6` | Motion-vector debug view (needs DLAA or DLSS on) |
| `Ctrl+F7` | Passive mode on / off |
| `Ctrl+F8` | Jitter-only debug on / off |
| `Ctrl+F9` | Self-test (dumps frames) |
| `Ctrl+F10` | NGX input snapshot |
| `Ctrl+F11` | Frame trace |
| `Ctrl+F12` | Cycle NGX jitter sign |
| `Alt+F8` | Per-part motion dump of the next frame into the log |

</details>

The full sheet with every beep pattern is in [`docs/KEYS.md`](docs/KEYS.md).

## The tuning menu

New in v0.10.0. Press **`Delete`** in game and a panel with every setting opens over the picture: name, current
value, the hotkey for it, and for the selected row a short description. **`Up` / `Down`** select a row,
**`Left` / `Right`** change it, `Delete` closes it again (two short beeps on open, one low beep on close).

- **Flat:** the panel sits near the top-left corner of the screen.
- **VR:** the panel shows in both eyes, centred in front of you, and on the desktop mirror window -- also on the
  VR main menu and the garage / truck dealer screens, before a save is loaded.
- **The game does not see `Delete` and the arrow keys while the menu is open** (they steer and drive in the default
  layout), so the truck does not react while you tune. Closed, the keys belong to the game again.
- Every row calls the same function as its hotkey: the hotkeys keep working while the menu is open, and every change
  beeps and logs exactly like its key. Number rows repeat while you hold `Left` / `Right`.
- **Move the panel:** the `Panel position X`, `Panel position Y` and `Panel size` rows (and `Panel VR depth` in VR,
  which brings the panel nearer). Flat and VR keep their own position (`menu_x`, `menu_y`, `menu_scale`,
  `menu_vr_x`, `menu_vr_y`, `menu_vr_scale`, `menu_vr_depth`, `menu_vr_face` in `dlaa.ini`).
- **VR: the panel faces you.** Move it to the side or up / down and it turns towards your eye like a real screen,
  keeping its size from every angle (`Panel faces you (VR)` row, `menu_vr_face`, on by default; off = a flat picture).
  On the garage / truck dealer screens the panel and the fps box are drawn on top of the game's own menu, not under it
  (the mod finds where the game hands each eye picture to the headset in the first frames after the menu opens).
- **Live fps counter:** the `FPS counter` row switches on a small fps box: the fps and the frame time as text with a dark outline, no background (`60 fps` /
  `16.7 ms`, updated twice a second) that stays on screen also when the menu is closed (flat, and in VR in both eyes).
  `FPS counter X`, `FPS counter Y` and `FPS counter size` move it; flat and VR keep their own position (`fps_show`,
  `fps_x`, `fps_y`, `fps_scale`, `fps_vr_x`, `fps_vr_y`, `fps_vr_scale` in `dlaa.ini`). Off, it costs nothing.
  The size goes from `0.2` to `3.0`; in VR it starts at `0.3` (small but readable in the headset).
- **Save to dlaa.ini** (the same as `Shift+F12`) writes the live values, and the panel position / fps box once you
  changed them.
- **Where your GPU time goes:** under the title, a GPU line shows the game's own GPU time per frame, what this mod costs
  (split into DLAA/DLSS itself, per-part motion, camera motion, see-through depth, mirrors, sharpen and copies; in VR per
  eye) and the fps. The `GPU cost` column shows each row's share right now (`off` = that feature is off and costs
  nothing). All figures are GPU milliseconds per frame (VR: per eye), measured live and refreshed once a second. The
  mod's timers switch on while the menu is open and off again when it closes (unless `debug = 1`).
- Different keys: `key_menu`, `key_menu_up`, `key_menu_down`, `key_menu_left`, `key_menu_right` in `dlaa.ini`
  (see [`docs/KEYS.md`](docs/KEYS.md)).

## `dlaa.ini`

Optional, placed next to `dinput8.dll`. One `key = value` per line; `#` or `;` start
a comment. It is read once at launch (the `config (...)` line in the log shows what
was used). The full template with every key and its default is in
[`docs/dlaa.ini.sample`](docs/dlaa.ini.sample). The keys most users care about:

**The five settings that matter:** `mode`, `perf_profile`, `dlss_preset`, `dlaa_area` (VR), `debug`.
Everything else has a tuned default; see [`docs/dlaa.ini.sample`](docs/dlaa.ini.sample) for the full list.

The table below lists the commonly used keys:

| key | default | meaning |
|-----|---------|---------|
| `mode` | *(set by `End`)* | start mode: `1` DLAA, `2` DLSS, `0` off |
| `dlss_upscale` | `1` | `1` = allow DLSS upscaling when the render scale is below 1; `0` = DLAA only |
| `dlss_preset` | `default` | DLSS model: `default`, `J`, `K`, `L`, `M`, `E`, `F` |
| `sharpness` | `0.4` | sharpening after DLAA, `0`..`1` (`0` = off) |
| `sharp_radius` | `1.5` | sharpening width in texels, `1`..`4` |
| `dlaa_area` | `100` | run DLAA on the centre `N` % of each eye image (VR performance), `20`..`100`. VR only: in flat mode the whole picture is always used. Also used for the truck on the VR main menu / garage screens |
| `mv_objects` | `2` | motion vectors for moving traffic (v0.10.0): `2` = every moving part gets its own exact motion (default), `0` = camera motion only (`1` = the older v0.9.0 method, for comparison) |
| `mirror_dlaa` | `1` | DLAA on the truck / car mirrors (v0.10.0): `1` = each mirror gets its own DLAA (default), `0` = mirrors are left as the game draws them. `Alt+F6` switches it live, `Shift+F12` saves |
| `dlaa_pre_tonemap` | `1` | DLAA before the game's tonemap (v0.10.0, flat DLAA mode): `1` = on the game's HDR colours (default), `0` = on the finished picture at the end of the frame. `Alt+F7` switches it live, `Shift+F12` saves |
| `tex_lod_bias` | `0` | sharper texture text (v0.9.0): texture mip LOD bias for the 3-D world while DLAA / DLSS runs, `-3`..`1`, `0` = off. Try `-0.5` to `-1`; more negative = sharper but more shimmer on fine patterns. `Ctrl+F1` / `F2` change it live, `Shift+F12` saves |
| `tex_lod_bias_auto` | `0` | `1` = while DLSS upscales, add log2(render width / output width) on top of `tex_lod_bias` (e.g. 75 % render width → −0.42) |
| `tex_aniso` | `0` | anisotropic filtering for the world textures: `0` = the game's own setting; `2`..`16` = at least this level (trilinear textures become anisotropic) |
| `preview_dlaa` | `1` | `1` = also anti-alias the profile / truck-preview screen (flat and VR); `0` = leave that screen untouched. VR at eye resolution needs a lot of GPU memory and time there, turn it off if that screen stutters. That screen always uses model 2 (preset E); the model keys change the drive only |
| `jitter_sign_x` / `jitter_sign_y` | `1` | flip if the image shimmers or looks doubled (try the 4 combinations) |
| `dlss_hdr` | `1` | HDR only: `1` = tell DLSS the picture is HDR (normal); `0` = try this if HDR brightness or colours look wrong with DLAA on |
| `beeps` | `1` | `0` = silence the hotkey beeps |
| `menu_x` / `menu_y` / `menu_scale` | `2` / `8` / `1.0` | tuning menu panel, flat (v0.10.0): left / top edge in % of the picture, size `0.5`..`2.0`. The menu's Panel rows change them live |
| `menu_vr_x` / `menu_vr_y` / `menu_vr_scale` / `menu_vr_depth` | `0` / `-5` / `1.0` / `12` | tuning menu panel, VR (v0.10.0): centre offset from the eye's centre in % (`-50`..`50`, negative y = up), size (1 = 54 % of the eye width), inward shift per eye in px |
| `menu_vr_face` | `1` | tuning menu panel, VR (v0.10.0): `1` = the panel turns to face your eye when moved off the centre (a tilted 3-D panel, same size from every angle), `0` = a flat picture. The menu's `Panel faces you (VR)` row switches it live |
| `fps_show` | `0` | live fps box (v0.10.0): `1` = a small fps / frame-time box stays on screen, also while the menu is closed. The menu's `FPS counter` row switches it live |
| `fps_x` / `fps_y` / `fps_scale` | `1` / `1` / `1.0` | fps box, flat (v0.10.0): left / top edge in % of the picture (`0`..`95`), size `0.2`..`3.0` (1 = 92 x 44 px at 1080 p) |
| `fps_vr_x` / `fps_vr_y` / `fps_vr_scale` | `0` / `-25` / `0.3` | fps box, VR (v0.10.0): centre offset from the eye's centre in % (`-50`..`50`, negative y = up), size `0.2`..`3.0` (1 = 8 % of the eye width) |
| `key_<action>` | *(see KEYS.md)* | re-bind any hotkey, e.g. `key_dlaa_toggle = Ctrl+D`, or `none` to disable it. Full list and format: [`docs/KEYS.md`](docs/KEYS.md) |
| `debug` | `0` | `1` = write the log file `dlaa_inject.log` (off by default) |

## Performance

Measured with the mod's own GPU timers (`debug = 1` writes a `perf eye` line to the log every 10 seconds). ETS2 flat,
render 2880x2160 (Scaling 75 % of 3840x2160), model E, sharpen on, texture LOD bias -1.0, 4 mirrors, dense traffic.
The game held its 72 fps cap the whole time.

| Part of the mod | GPU ms per frame | Switch it off with |
|---|---|---|
| DLAA itself (NVIDIA NGX, model E) | 1.5 - 1.7 | `End` (mode off) |
| Mirrors: own motion vectors + ids, 4 views | 1.3 | `mirror_dlaa = 0` / `Alt+F6` |
| Mirrors: DLAA (NGX) on the 4 views | 0.4 | `mirror_dlaa = 0` / `Alt+F6` |
| Depth for wires, fences, plates (forward redraw) | 0.5 | `mv_fwd_depth = 0` / `Ctrl+F3` |
| Per-part motion vectors: redraw into the id picture | 0.12 | `mv_objects = 0` / `Alt+F5` |
| Per-part motion vectors: pairing, camera consensus, plates | 0.7 | `mv_objects = 0` / `Alt+F5` |
| Motion-vector pass, depth copy, sharpen, copies | 0.6 | -- |
| CPU, all of it | 0.3 | -- |

### What changes the cost

| Setting | Effect on frame time | Notes |
|---|---|---|
| Render scale (`Scaling` in the game, `r_scale_x/y`) | biggest lever: everything scales with pixels | DLSS mode renders lower and upscales; DLAA mode = native |
| Model (`Shift+F1..F4`, `dlss_preset`) | 1 = 2.4 ms, 2 (E) = 1.0 ms, 3 (F) = 1.0 ms, 4 (M) = 9.4 ms per eye at 4592x6496 | measured in VR; E is the default |
| DLAA area (`dlaa_area`, `Shift+F5/F6`, VR only) | 40 % = about 0.45 ms per eye instead of 1.0 | NGX runs on the centre only; the edge keeps the raw picture. `20`..`100`; also applies to the VR main menu / garage truck picture |
| Mirror DLAA (`mirror_dlaa`, `Alt+F6`) | 1.7 ms per frame flat with 4 mirrors | the second-biggest item after DLAA itself |
| Per-part motion vectors (`mv_objects`, `Alt+F5`) | about 0.8 ms per frame flat | 0 = camera-only motion: traffic ghosts again |
| Static redraw skip (`mv_replay_static_frames`, `mv_replay_static_every`) | saves most of the per-part `replay` (2-3 ms at 4K in ATS before it) | on by default; `mv_replay_static_frames = 0` = redraw everything every frame |
| Wire / plate depth (`mv_fwd_depth`, `Ctrl+F3`) | 0.5 ms per frame flat | off: wires and fences smear when you move |
| Sharpen (`sharpness`, `Shift+F7/F8`) | 0.1 ms | 0 = the pass is skipped |
| Texture LOD bias (`tex_lod_bias`, `Ctrl+F1/F2`) | none measurable | sharper textures can shimmer more on fine meshes |
| DLAA before the tone-map (`dlaa_pre_tonemap`, `Alt+F7`) | none measurable | same NGX work, different input |
| Debug log + timers (`debug = 1`) | 0.1 - 0.3 ms CPU | `debug = 0` for play, 1 for bug reports |
| Colour view (`Ctrl+F6`) | small | debug only |
| Stereo buffer scale 2.0 (VR, `r_manual_stereo_buffer_scale`) | 4x the pixels of 1.0 | the user setting that decides most of the VR cost |

Everything except DLAA itself adds up to about 4 ms per frame on this card in flat. VRAM: about 410 MB for the
per-frame textures in flat (pooled), roughly 2.5x that in VR.

**Static redraw skip (v0.10.0):** the per-part motion vectors redraw every world object once more each frame to learn which
object owns which pixel; for road, buildings, trees and parked trucks that only ever says "it did not move". An object that was
static for `mv_replay_static_frames` frames in a row (default 6) is now redrawn only on 1 frame of every `mv_replay_static_every`
(default 4), staggered; its pixels keep the camera motion, exactly as before. Expected: the `replay` figure of the `perf eye` line
drops to roughly a third (measure it: not yet timed in game). A parked vehicle that starts moving is picked up within `every`
frames at most (4 = 1/18 s at 72 fps), usually after 1-2. Only objects that move exactly with the camera count (within
`mv_replay_static_eps_px`, default 0.02 px): a truck you follow at the same speed is never skipped. `mv_replay_static_frames = 0`
switches it off.

VR: not measured yet with v0.10. Both eyes run the whole chain at 4592x6496 each (stereo buffer scale 2.0), so expect
roughly double. `mirror_vr_mode` (0 off, 1 all mirrors, 2 only the two largest, 3 every mirror every 2nd frame) and
`mirror_dlaa = 0` are the first knobs to turn if VR drops below the headset rate.

**Test rig** (all numbers above): AMD Ryzen 9 9950X3D, 62 GB RAM, NVIDIA GeForce RTX 5090 (driver 32.0.16.1692),
Windows 11 Pro, 3840x2160 monitor; VR = Meta Quest 3 over Virtual Desktop (VDXR); ATS 1.61, ETS2 current.

## Troubleshooting

- **The log is off by default.** Put `debug = 1` in `dlaa.ini` (next to `dinput8.dll`)
  and start the game again: `dlaa_inject.log` is then written next to the DLL. It
  records the version, which hooks installed, and whether DLAA/DLSS started. It is
  opened share-read, so you can tail it while the game runs. Check it first.
- **Game won't start:** the `bin\win_x64\` folder may be read-only (run a Steam
  "verify files" and it reverts), or antivirus may have quarantined the unsigned
  DLL. A `dxgi.dll`/`dinput8.dll` from another mod still present will also conflict.
- **No DLAA, or a crash referencing NGX/DLSS:** `nvngx_dlss.dll` is missing or too
  old — add or update it (see Install step 6).
- **Windows HDR:** supported from v0.8.0 in flat mode (driving and the menu / truck-preview screen). Older
  versions do nothing with HDR on (the log shows `blits=0`): update. If brightness or colours change when you
  switch DLAA on and off in HDR, put `dlss_hdr = 0` in `dlaa.ini` and report it.
- **NVIDIA Smooth Motion:** supported from v0.8.1 in flat mode (driving and the menu / truck-preview
  screen). Older versions do nothing with Smooth Motion on (the log shows `passes=0 blits=0` and
  `caller=NvPresent64.dll`): update. Smooth Motion only adds frames on the monitor; it does nothing for
  the headset picture in VR, so switch it off for VR.
- **OFXR Bridge (VR frame generation):** supported from v0.10.0 — see the section above; older versions may double-process the
  bridge's copies.
- **Faint dark line down the middle of the menu / truck-preview screen:** fixed in v0.8.1: update.
- **No DLAA / DLSS while you drive a car (ATS):** fixed in v0.8.2: update. Older versions work in the menu
  but do nothing in the car (the log shows many `fifo underflow` lines).
- **No log file with `debug = 1`:** the line must not start with `#`. From v0.8.2 the packaged `dlaa.ini`
  has a plain `debug = 0` line: change the 0 to 1.
- **Start with DLAA off** (detect + log only): put an empty file named
  `dlaa_off.txt` next to `dinput8.dll` before launch. `Shift+F11` / `End` still
  switch it on.
- **Shimmering / doubled image:** flip `jitter_sign_x` / `jitter_sign_y` in
  `dlaa.ini` (4 combinations).
- **Uninstall:** delete `dinput8.dll` (and `dlaa.ini`).

## Multiplayer

ETS2/ATS single-player has no anti-cheat, so this works fine there. **TruckersMP and
Convoy multiplayer have their own rules about third-party DLLs — check what each
allows before using it online.**

## Build from source

Visual Studio 2022 (MSVC, x64) and CMake ≥ 3.20. MinHook is fetched automatically by
CMake (needs git + internet on the first configure). DLAA is compiled in when the
NVIDIA DLSS SDK is present:

```bash
git clone --depth 1 https://github.com/NVIDIA/DLSS external/DLSS
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `build/Release/dinput8.dll`. Without `external/DLSS` the build is a
logging-only injector (no DLAA). `scripts/package.ps1` builds Release and assembles
the release zip. Architecture and internals: [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md).

## Reporting a problem (what to upload)

If something goes wrong, [open an issue](../../issues/new/choose) (**New issue** >
**Bug report**) and attach the files below. Without the log I usually can't tell what
happened.

**Steps first:** put `debug = 1` in `dlaa.ini` (the log is off without it), start the game, reproduce the problem, then **quit the game** and
only then collect the files. Load a save and drive for about a minute before quitting, so the log covers the 3D scene and not just the menu. The log is written while the game runs, so grab it after
the game has exited (or at least after the problem has happened).

**Files to attach** — all of them live in the game's `bin\win_x64\` folder, next to
`dinput8.dll`, e.g.
`...\steamapps\common\Euro Truck Simulator 2\bin\win_x64\` or
`...\steamapps\common\American Truck Simulator\bin\win_x64\`:

- **`dlaa_inject.log`** — **required, always.** It only exists when `dlaa.ini` has
  `debug = 1`; if there is no log file, that line is missing.
- **`dlaa.ini`** — **required** (your settings).
- Only when asked, or for visual problems:
  - **`dlaa_trace.txt`** — frame trace, written after you press `Ctrl+F11` in game.
  - **`dlaa_selftest\`** folder — `Ctrl+F9` dumps lossless BMPs there. Zip the folder.
  - **`dlaa_snap\`** folder — `Ctrl+F10` writes an NGX input snapshot there. Zip the folder.
- A **screenshot or short video** of the problem. Video compression hides anti-aliasing
  detail, so for AA-quality problems the `Ctrl+F9` self-test files are far more useful.
- **Game `config.cfg`** lines `r_scale_x`, `r_scale_y` and
  `r_manual_stereo_buffer_scale`, or just the whole file — from
  `Documents\Euro Truck Simulator 2\config.cfg` or
  `Documents\American Truck Simulator\config.cfg`.

**Write in the issue:**

- Game (ETS2 or ATS) and game version.
- Mod version — near the top of the log:
  `=== ETS2/ATS DLAA injector v<version> loaded ... ===`.
- Flat or VR (the log line `launch mode: FLAT` / `launch mode: VR` shows what the mod
  detected) and your game launch options; if VR, the headset and runtime (e.g. Quest 3
  via Virtual Desktop / VDXR).
- GPU and driver version.
- `nvngx_dlss.dll` version (right-click > Properties > Details).
- Mode in use (`End` key: DLAA / DLSS / off) and the model (`Shift+F1`..`F4`).
- Other mods or injectors in the folder (ReShade, any other
  `dinput8.dll` / `dxgi.dll`).
- Steam overlay on or off.

**Privacy:** the log can contain folder paths with your Windows user name. Open it and
check (or edit it) before uploading.

**Never attach `nvngx_dlss.dll` or any other NVIDIA DLL** — their license doesn't allow
redistribution. Just give the version number.

## License and credits

**This mod** — [PolyForm Noncommercial License 1.0.0](LICENSE.md), Copyright (c) 2026 JRDevo.

- Free to use, copy, change and share for **noncommercial** purposes.
- **No commercial use** (no selling, no paid bundles) without written permission.
- If you share it, include the license and the copyright notice.
- No warranty: you use it at your own risk.

**How it was made** -- the ideas, the direction, every in-game test round and the verdicts are JRDevo's. The code, shaders and docs were written by Claude (Anthropic): Claude Fable 5.1 as
the lead, with Claude Opus 5.5, Opus 4.8 and Sonnet 5.5 agents doing the building, from v0.1 to today.
JRDevo gains nothing from this mod except a fixed problem and a shared solution: no money, no donations,
no paid versions. Made with love.

**Third-party parts** — each has its own terms, see [`THIRD_PARTY.md`](THIRD_PARTY.md):

| Part | License | Shipped with this mod? |
|------|---------|------------------------|
| MinHook (function hooking) | BSD 2-Clause | Compiled into `dinput8.dll` |
| NVIDIA DLSS / NGX SDK | NVIDIA RTX SDK license (proprietary) | Only the small SDK link stub inside `dinput8.dll`. **No NVIDIA DLL** — you supply your own `nvngx_dlss.dll` |
| RCAS sharpening method (AMD FidelityFX FSR 1.0) | MIT | Own implementation of the published method |

Unofficial fan project: not affiliated with, or endorsed by, SCS Software, NVIDIA or AMD.
