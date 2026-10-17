# Contributing

With v0.13.0 the bells and whistles I know of are in, and the ideas left on my list are not worth
their cost. From here on the main work is **performance tuning and fixing what the mod gets
wrong**, and for that I need data from PCs that are not mine. New feature ideas are welcome too:
[open an issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose) with the **Feature request** template and say what it
would do for you in the game. Three ways to help, from easy to hard.

### 1. Send a performance log

The log is the most useful thing you can send. At `log_level = 4` the mod writes its own GPU and
CPU timings every 10 seconds (`perf eye`, `CPU prof`, `mirror units`, `frame pace` lines), so I can
see where the time goes on your hardware and tune for it.

1. Put `log_level = 4` in `dlaa.ini` (next to `dinput8.dll`). The packaged file has `log_level = 1`:
   change the `1` to `4`. The menu's `Logging` row does the same live (`debug`); `Shift+F12` saves it.
2. Start the game, load a save and **drive 2 to 3 minutes** where the fps drop or the problem shows.
   Then **quit the game** (the log is written while the game runs).
3. [Open an issue](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/issues/new/choose) (**Bug report**, also for performance) and attach
   `dlaa_inject.log` and `dlaa.ini` from `bin\win_x64\`. Write:
   - game (ETS2 / ATS), flat or VR (headset and runtime), GPU and driver;
   - **where**: map (base game or a map mod), city, road or depot;
   - **when**: the in-game time of day, the weather, the traffic;
   - your game Scaling (`r_scale_x` in `config.cfg`, or the Graphics menu) and the mod's mode / model;
   - what you saw: the fps, a stutter, a flicker, a ghost, and when it happens (always, in cities,
     at night, in mirrors ...).

"It does not work" or "the fps are low" without the log cannot be investigated. With the log, where
and when, it usually can.

### 2. Capture a flicker, a shimmer or a ghost

Anti-aliasing problems do not survive video compression, so a video rarely shows what is wrong.
The mod has its own capture keys. They need `log_level = 4`.

1. **Is it the mod?** Press `Shift+F11` (DLAA off) and look at the spot again. If the flicker is still
   there, the game draws it that way (its far shadows do flicker on their own); the mod cannot fix
   it. If it goes away with DLAA off, it is the mod's.
2. Switch DLAA back on (`Shift+F11`), **look straight at the spot** and press **`Ctrl+F9`**: the mod
   writes lossless frames into `dlaa_selftest\` (`<mode>_<n>.bmp` = the DLAA output,
   `final_<mode>_<n>.bmp` = the same frame as it reached the screen).
3. Still looking at the spot, press **`Ctrl+F10`**: two consecutive frames of what goes into DLSS
   (colour, depth, motion vectors) go into `dlaa_snap\<n>_blit<blit>\`. On the menu / truck-preview
   screen the same key takes a measuring capture (a short freeze).
4. For a part that smears or a number plate that blinks, also press **`Alt+F8`** next to it: the
   per-part motion state of the next frame goes into the log (`MV dump` lines).
5. Zip `dlaa_selftest\` and `dlaa_snap\` and attach them with the log and `dlaa.ini`. Say what to look
   at (a pole on the left, the shop window, the trailer's plate) and where / when you were.

### 3. Code

- Build: see [Build from source](README.md#build-from-source). The internals, every phase and why it was
  built that way: [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md).
- Branches: `main` holds released versions only; all work goes into `dev`. Open pull requests
  against `dev`.
- Tests: `tests\build_*.bat` build the unit tests without the game (`build\tests\*.exe`, exit code 0
  = pass). GitHub Actions runs them on every push. Pure logic goes into a header with a test
  (`src\fps_pose.h`, `src\frame_pace.h`, `src\hotkey_scan.h` are examples); the DX11 hooks stay
  in `src\inject.cpp`, the OpenXR hooks in `src\xr_hook.cpp`.
- Rules for published text: never name another ETS2 / ATS anti-aliasing mod, VR = OpenXR only,
  game paths as `bin\win_x64\`, say what was tested and what was not.
- Tell me what you tested in game (game, flat / VR, where, when) in the pull request, with a log.
