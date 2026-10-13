---
name: Bug report
about: Something doesn't work (crash, no DLAA/DLSS, visual glitch, VR problem, low fps)
title: "[Bug] "
labels: bug
---

<!-- The log has errors only by default. FIRST put the line  log_level = 4  in dlaa.ini (next to dinput8.dll; the packaged -->
<!-- file has  log_level = 1  -- change the 1 to 4; create the file if you have none). The old  debug = 1  does NOT work -->
<!-- while a log_level line exists. -->
<!-- Then start the game, load a save, drive 2-3 minutes where the problem shows, QUIT the game, and collect the files. -->
<!-- The log is written while the game runs; it covers the 3D scene only if you drove. -->
<!-- See README: "Reporting a problem (what to upload)" and "Contributing (logs, flicker captures, code)". -->

## Environment

- Game (ETS2 / ATS) and game version:
- Mod version (line `=== ETS2/ATS DLAA injector v... loaded ===` near the top of `dlaa_inject.log`):
- Flat or VR (the log line `launch mode: FLAT` / `launch mode: VR` says what the mod detected):
- Game launch options (Steam > Properties > Launch options, e.g. `-openxr`):
- VR headset and runtime (e.g. Quest 3 via Virtual Desktop / VDXR), if VR:
- GPU and driver version:
- `nvngx_dlss.dll` version (right-click > Properties > Details):
- Mode in use (`End` key: DLAA = 1 beep / DLSS = 2 beeps / off) and model (`Shift+F1`..`F4`):
- Game Scaling (`r_scale_x` in `config.cfg`, or the Graphics menu), in VR also the headset's render resolution:
- Other mods / injectors in the folder (ReShade, other dinput8/dxgi DLLs):
- Steam overlay on or off:

## Where and when

- Map (base game, DLC region, or a map mod), city / road / depot:
- In-game time of day:
- Weather and traffic:
- Happens: always / in cities / at night / in mirrors / after a truck change / ...:

## Attachments

All files are in the game's `bin\win_x64\` folder, next to `dinput8.dll`.

- [ ] `dlaa_inject.log` (**required** — written with detail only when `dlaa.ini` has `log_level = 4`; a tiny or missing file = that line is missing)
- [ ] `dlaa.ini` (**required** — your settings)
- [ ] For a flicker, shimmer or ghost: `dlaa_selftest\` folder, zipped (`Ctrl+F9` while looking at the spot; lossless frames) and `dlaa_snap\` folder, zipped (`Ctrl+F10`; what goes into DLSS). First check with `Shift+F11` (DLAA off) that the problem goes away without the mod.
- [ ] `dlaa_trace.txt` (optional, `Ctrl+F11` in game; only when asked)
- [ ] Screenshot / short video (video compression hides AA detail; the `Ctrl+F9` frames are better)
- [ ] `config.cfg` lines `r_scale_x`, `r_scale_y`, `r_manual_stereo_buffer_scale` (or the whole file), from `Documents\Euro Truck Simulator 2\` or `Documents\American Truck Simulator\`

Privacy: the log may contain folder paths with your Windows user name; check before uploading.
Do **not** attach `nvngx_dlss.dll` or any other NVIDIA DLL.

## Steps to reproduce

1.
2.
3.

## Expected behavior

## Actual behavior
