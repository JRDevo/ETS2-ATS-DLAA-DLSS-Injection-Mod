**I've removed my post from Reddit, you can't be constructive there.**

# ETS2 & ATS: DLAA & DLSS Injection Mod

NVIDIA DLAA and DLSS upscaling for **Euro Truck Simulator 2** and **American Truck
Simulator**, in both flat and VR. It replaces the game's own anti-aliasing with
DLAA (NVIDIA's native-resolution AA) and can optionally upscale with DLSS so the
game renders fewer pixels and the GPU has an easier time.

It installs as a small `dinput8.dll` proxy in the game folder. No launcher, no
account, no in-game menu — a few hotkeys and an optional text config.

**An NVIDIA RTX GPU is required** (DLAA and DLSS only run on RTX cards).

## Features

- **DLAA** — NVIDIA anti-aliasing at the game's render resolution. It removes the shimmer and flicker on
  fences, power lines, road markings and shadows that the game's own AA leaves behind.
- **DLSS upscaling** — render fewer pixels and let DLSS rebuild the full picture, for more frame rate.
- **Flat and VR** — one DLL and one `dlaa.ini` for both. VR runs one pass per eye (OpenXR).
- **Windows HDR** — works with HDR on in flat mode (new in v0.8.0).
- **Menu and truck-preview screens** get anti-aliasing too, not only the drive.
- **Four DLSS models** to pick from, switched live with a key, from light to heavy.
- **Adjustable sharpening** — strength and width, live.
- **DLAA area (VR)** — process only the middle of each eye to save GPU time.
- **Hotkeys with beeps** — made to use blind in a headset; every key can be re-bound in `dlaa.ini`.
- **Bring your own `nvngx_dlss.dll`** — drop in a newer one at any time to get NVIDIA's newer models.
- **Small and simple** — a single `dinput8.dll`. No launcher, no account, no in-game menu. Delete the file
  to uninstall.
- **Debug log** for bug reports (off by default).

## ⬇ Download

**[Download the latest release here](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/releases/latest)**
— get the `.zip` under **Assets**, then follow [Install](#install) below.
(The green **Code** button downloads the source code, not the mod.)

This is an unofficial fan project. It is not affiliated with, or endorsed by, SCS
Software or NVIDIA. Euro Truck Simulator 2 and American Truck Simulator are
trademarks of SCS Software; DLSS and DLAA are trademarks of NVIDIA Corporation.

> Status: v0.8.0. Windows HDR works in flat mode (new in v0.8.0). DLAA has been run in ETS2 (VR) and ATS (flat and VR). The DLSS
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

**Every key can be changed.** Open `dlaa.ini`, find the `key_...` line for the
action and edit the key, e.g. `key_dlaa_toggle = Ctrl+D` (or `none` to turn it
off). Restart the game. Full list: [`docs/KEYS.md`](docs/KEYS.md).

By default every control except `End` is an **F-key with one modifier**, so it can be used
blind in a headset. The game window must be focused. **Shift + F-key = the keys you
use while playing**; Ctrl + F-key are debug keys (see [`docs/KEYS.md`](docs/KEYS.md)
for the full sheet).

| Default key | Action |
|-----|--------|
| `End` | Cycle mode: DLAA → DLSS → off (saved) |
| `Shift+F1`..`F4` | DLAA model / preset: 1 = default (driver pick), 2 = `E`, 3 = `F`, 4 = `M` |
| `Shift+F5` / `F6` | DLAA area − / + 10 % (shrinks the processed region; VR only, flat always uses the whole picture) |
| `Shift+F7` / `F8` | Sharpening strength − / + 0.1 |
| `Shift+F9` / `F10` | Sharpening width − / + 0.5 |
| `Shift+F11` | DLAA on / off |
| `Shift+F12` | Save the current preset / area / sharpening / upscale settings to `dlaa.ini` |

Each key beeps to confirm (pitch tracks the value); a long low tone means "already
at the limit" or "save failed". Turn beeps off with `beeps = 0` in `dlaa.ini`.

## `dlaa.ini`

Optional, placed next to `dinput8.dll`. One `key = value` per line; `#` or `;` start
a comment. It is read once at launch (the `config (...)` line in the log shows what
was used). The full template with every key and its default is in
[`docs/dlaa.ini.sample`](docs/dlaa.ini.sample). The keys most users care about:

| key | default | meaning |
|-----|---------|---------|
| `mode` | *(set by `End`)* | start mode: `1` DLAA, `2` DLSS, `0` off |
| `dlss_upscale` | `1` | `1` = allow DLSS upscaling when the render scale is below 1; `0` = DLAA only |
| `dlss_preset` | `default` | DLSS model: `default`, `J`, `K`, `L`, `M`, `E`, `F` |
| `sharpness` | `0.4` | sharpening after DLAA, `0`..`1` (`0` = off) |
| `sharp_radius` | `1.5` | sharpening width in texels, `1`..`4` |
| `dlaa_area` | `100` | run DLAA on the centre `N` % of each eye image (VR performance), `40`..`100`. VR only: in flat mode the whole picture is always used |
| `preview_dlaa` | `1` | `1` = also anti-alias the profile / truck-preview screen (flat and VR); `0` = leave that screen untouched. VR at eye resolution needs a lot of GPU memory and time there, turn it off if that screen stutters. That screen always uses model 2 (preset E); the model keys change the drive only |
| `jitter_sign_x` / `jitter_sign_y` | `1` | flip if the image shimmers or looks doubled (try the 4 combinations) |
| `dlss_hdr` | `1` | HDR only: `1` = tell DLSS the picture is HDR (normal); `0` = try this if HDR brightness or colours look wrong with DLAA on |
| `beeps` | `1` | `0` = silence the hotkey beeps |
| `key_<action>` | *(see KEYS.md)* | re-bind any hotkey, e.g. `key_dlaa_toggle = Ctrl+D`, or `none` to disable it. Full list and format: [`docs/KEYS.md`](docs/KEYS.md) |
| `debug` | `0` | `1` = write the log file `dlaa_inject.log` (off by default) |

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

This project's own source is under the **PolyForm Noncommercial License 1.0.0** —
free for noncommercial use. See [`LICENSE`](LICENSE).

It builds against two third-party components with their own terms — **MinHook**
(BSD 2-Clause) and the **NVIDIA DLSS / NGX SDK** (proprietary NVIDIA RTX SDK
license). Neither is redistributed by this project, and **no NVIDIA binary is
shipped** — you supply your own `nvngx_dlss.dll`. The sharpening pass follows AMD's
published RCAS method (FidelityFX FSR 1.0, MIT). Details and the required notices
are in [`THIRD_PARTY.md`](THIRD_PARTY.md).
