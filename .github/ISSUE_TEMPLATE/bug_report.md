---
name: Bug report
about: Something doesn't work (crash, no DLAA/DLSS, visual glitch, VR problem)
title: "[Bug] "
labels: bug
---

<!-- The log is OFF by default. FIRST put the line  debug = 1  in dlaa.ini (next to dinput8.dll; create the file if you have none). -->
<!-- Then start the game, reproduce the problem, QUIT the game, and collect the files. The log is written while the game runs. -->
<!-- See README: "Reporting a problem (what to upload)". -->

## Environment

- Game (ETS2 / ATS) and game version:
- Mod version (line `=== ETS2/ATS DLAA injector v... loaded ===` near the top of `dlaa_inject.log`):
- Flat or VR (the log line `launch mode: FLAT` / `launch mode: VR` says what the mod detected):
- Game launch options (Steam > Properties > Launch options, e.g. `-openxr`):
- VR headset and runtime (e.g. Quest 3 via Virtual Desktop / VDXR), if VR:
- GPU and driver version:
- `nvngx_dlss.dll` version (right-click > Properties > Details):
- Mode in use (`End` key: DLAA = 1 beep / DLSS = 2 beeps / off) and model (`Shift+F1`..`F4`):
- Other mods / injectors in the folder (ReShade, other dinput8/dxgi DLLs):
- Steam overlay on or off:

## Attachments

All files are in the game's `bin\win_x64\` folder, next to `dinput8.dll`.

- [ ] `dlaa_inject.log` (**required** — only written when `dlaa.ini` has `debug = 1`; no file = that line is missing)
- [ ] `dlaa.ini` (**required** — your settings)
- [ ] `dlaa_trace.txt` (optional, `Ctrl+F11` in game)
- [ ] `dlaa_selftest\` folder, zipped (optional, `Ctrl+F9`; best for AA-quality problems)
- [ ] `dlaa_snap\` folder, zipped (optional, `Ctrl+F10`)
- [ ] Screenshot / short video (video compression hides AA detail)
- [ ] `config.cfg` lines `r_scale_x`, `r_scale_y`, `r_manual_stereo_buffer_scale` (or the whole file), from `Documents\Euro Truck Simulator 2\` or `Documents\American Truck Simulator\`

Privacy: the log may contain folder paths with your Windows user name; check before uploading.
Do **not** attach `nvngx_dlss.dll` or any other NVIDIA DLL.

## Steps to reproduce

1.
2.
3.

## Expected behavior

## Actual behavior
