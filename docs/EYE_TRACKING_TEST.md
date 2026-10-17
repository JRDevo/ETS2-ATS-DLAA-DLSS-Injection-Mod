# Eye-tracked foveated rendering — test build (discussion #11)

> **Test build.** Not a normal release. It has everything from v0.13.0, plus the new performance work from `dev`, plus eye tracking.
> I do not have an eye-tracked headset. You are my eyes here. Thank you!

## What it does

Foveated rendering (menu row **Foveated (VRS)**) draws the edge of each eye picture at a lower detail. Until now the sharp area stayed in the centre of the lens.

With **eye tracking on**, the sharp area moves to where you look. Then the sharp area can be smaller, and you get more fps.

It uses the standard OpenXR eye-gaze extension (`XR_EXT_eye_gaze_interaction`). Varjo headsets have it. Other eye-tracked headsets may work too.

## What you need

- NVIDIA RTX GPU (VRS needs it).
- ETS2 or ATS in VR (OpenXR).
- Eye tracking **on** in your headset software (Varjo Base: *System → Eye tracking*). Do the eye calibration first.

## Install

1. Back up your current `dinput8.dll` and `dlaa.ini` in the game's `bin\win_x64` folder.
2. Copy the `dinput8.dll` from this test build over it. Keep your `dlaa.ini`.
3. Open `dlaa.ini` and set these lines (add them if they are not there):

```ini
vrs=1
vrs_eye=1
log_level=4
```

## Test 1 — does the mod get your eye data? (5 minutes)

1. Start the game in VR. Drive for 1 minute.
2. Open the menu (**Delete** key) → **VR performance** group.
3. Look at the row **Foveated eye tracking**. What does it say?
   - `on (gaze)` → good, eye data comes in.
   - `on (waiting for gaze)` → the mod asked for eye data, but none comes yet.
   - `on (no eye tracker)` → the runtime does not offer eye tracking to the game.
   - `on (restart the game)` → you switched it on in the menu. The eye data is asked for only at game start. Restart the game.
   - `on (failed, see log)` → something went wrong. The log says what.
   - `on (no OpenXR session yet)` → VR has not started yet. Wait until you sit in the truck.
4. Quit the game. Send me the log file `dlaa_inject.log` (same folder as the DLL).

## Test 2 — does the sharp area follow your eyes? (5 minutes)

1. In `dlaa.ini` set `vrs_debug=1`. Keep `log_level=4` (`vrs_debug` works only at log level 4). This makes the low-detail area **very blocky on purpose**, so you can see it.
2. Start the game in VR.
3. Record the desktop mirror window (Varjo Base mirror, or the game window) with OBS or the NVIDIA overlay (Alt+F9).
4. While driving, look slowly: left, right, up at the sky, down at the dashboard. Hold each look for 2 seconds.
5. In the recording, the sharp circle must move with your look. The blocks must stay at the edge, away from where you look.

Tell me:
- Does the sharp circle follow your eyes? Yes / no / only sometimes.
- Is it in the correct place, or a bit off (which side)?
- When you blink, does it jump or flicker?
- When you look fast, can you see the blocks for a moment before the circle moves?

Set `vrs_debug=0` again after this test.

## Test 3 — how much fps does it give? (10 minutes)

Same place, same weather, same time of day. Drive the same road for 1 minute each time. Write down the fps (menu → **FPS counter**) or the GPU frame time.

| Run | `vrs` | `vrs_eye` | Foveated size | fps / frame time | Sharp area visible? |
|---|---|---|---|---|---|
| A | 0 | 0 | – | | |
| B | 1 | 0 | 100 % | | |
| C | 1 | 1 | 100 % | | |
| D | 1 | 1 | 60 % | | |
| E | 1 | 1 | the smallest you cannot see | | |

You can change **Foveated size** and **Foveated eye tracking** live in the menu. No restart.

For run E: make **Foveated size** smaller step by step. Stop at the last step where you cannot see the low detail when you drive normally. Tell me that number. This is the most important number for me.

## What to send

- `dlaa_inject.log` from Test 1 and Test 3.
- Your answers from Test 2 (a short video is great).
- The table from Test 3.
- Headset, GPU, game (ETS2 / ATS), in-game scaling, and DLAA or DLSS.

Post it in [discussion #11](https://github.com/JRDevo/ETS2-ATS-DLAA-DLSS-Injection-Mod/discussions/11), or open an issue.

## If something is wrong

- Game does not start, or VR does not start: set `vrs_eye=0`. If it still fails, put back your old `dinput8.dll`. Send me the log.
- Menu row says `no eye tracker` but your headset has eye tracking: send me the log. The first `Eye tracking:` lines say why.
