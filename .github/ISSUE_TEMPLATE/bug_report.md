---
name: Bug report
about: Something doesn't work (crash, no DLAA/DLSS, visual glitch, VR problem, low fps)
title: "[Bug] "
labels: bug
---

<!-- BEFORE you write: in dlaa.ini (next to dinput8.dll) change  log_level = 1  to  log_level = 4 . -->
<!-- Start the game, load a save, drive 1-2 minutes where the problem shows, QUIT the game, then attach the files below. -->

## What happens

<!-- What you see, and when (night, rain, in mirrors, after a truck change, ...). -->

- Map location (map mod if any, city / road / depot):

## Setup

- Game (ETS2 / ATS):
- Flat or VR (if VR: headset and how it connects, e.g. Quest 3 via Virtual Desktop):
- GPU and driver version:
- Other graphics mods (ReShade, OptiScaler, other DLLs in `bin\win_x64\`):

## Files (drag them into this box)

All in the game's `bin\win_x64\` folder.

- [ ] `dlaa_inject.log` (**required**, made with `log_level = 4`; no file = the mod did not load, say so)
- [ ] `dlaa.ini` (**required**)
- [ ] A screenshot or short video (optional)

Do **not** attach `nvngx_dlss.dll` or any other NVIDIA DLL.
