# ETS2 / ATS DLAA Injector — hotkey cheat sheet (v0.7.7)

The tables below show the **default** keys. Every key can be changed in `dlaa.ini` (see
"Change a key" at the end). The game window must be focused. The modifiers must match
exactly — with the default `Shift+F11`, Shift+Ctrl+F11 or Shift+Alt+F11 does nothing, and a
key with no modifier (like `End`) only fires when Shift, Ctrl and Alt are all up.

- **Shift + F-key = USER keys** (the ones you use while playing)
- **Ctrl + F-key = DEBUG keys** (diagnostics / capture)

## User keys — Shift + F-key (Ctrl and Alt NOT held)

| Key        | Does                                   | Beep |
|------------|----------------------------------------|------|
| `End` (no modifier) | **Mode**: DLAA → DLSS → off → DLAA … (saved to `dlaa.ini` `mode` at once) | 1 high beep = DLAA, 2 high beeps = DLSS, 1 low beep = off |
| `Shift+F1` | Model 1 = preset `default` (driver)    | 1 beep |
| `Shift+F2` | Model 2 = preset `E`                   | 2 beeps |
| `Shift+F3` | Model 3 = preset `F`                   | 3 beeps |
| `Shift+F4` | Model 4 = preset `M`                   | 4 beeps |
| `Shift+F5` | DLAA area −10 %                        | 3 quick beeps (pitch rises with area) |
| `Shift+F6` | DLAA area +10 %                        | 3 quick beeps (pitch rises with area) |
| `Shift+F7` | Sharpen strength −0.1                   | 1 beep (pitch rises with strength) |
| `Shift+F8` | Sharpen strength +0.1                   | 1 beep (pitch rises with strength) |
| `Shift+F9` | Sharpen width −0.5                      | 2 beeps (pitch rises with width) |
| `Shift+F10`| Sharpen width +0.5                      | 2 beeps (pitch rises with width) |
| `Shift+F11`| DLAA on / off                          | high 1200 Hz = ON, low 300 Hz = OFF |
| `Shift+F12`| **Save** current settings to `dlaa.ini` | rising two-tone (600→900 Hz) = saved; one long low tone = failed |

Model select is direct: pressing the model that is already active just re-beeps (no change).
`Shift+F12` saves `dlss_preset`, `dlaa_area`, `sharpness`, `sharp_radius`, `dlss_upscale` (v0.7.0) and leaves every other
line of `dlaa.ini` untouched.

## Debug keys — Ctrl + F-key (Shift and Alt NOT held)

| Key        | Does                                   | Beep |
|------------|----------------------------------------|------|
| `Ctrl+F4`  | DLSS upscale on / off (v0.7.0)         | high 1200 Hz = ON, low 300 Hz = OFF |
| `Ctrl+F5`  | Motion vectors on / off                | — |
| `Ctrl+F6`  | MV debug view                          | — |
| `Ctrl+F7`  | Passive mode on / off                  | 1 low tone = ON, 2 low tones = OFF |
| `Ctrl+F8`  | Jitter-only debug on / off             | — |
| `Ctrl+F9`  | Self-test (dumps frames)               | — |
| `Ctrl+F10` | NGX input snapshot                     | — |
| `Ctrl+F11` | Frame trace                            | — |
| `Ctrl+F12` | Cycle NGX jitter sign                  | — |

## Beep patterns at a glance

| Sound | Means |
|-------|-------|
| 1 / 2 / 3 / 4 beeps at the same pitch (880 Hz) | DLAA model 1 / 2 / 3 / 4 selected (`Shift+F1..F4`) |
| 3 quick beeps, pitch tracks the value | DLAA area changed (`Shift+F5`/`F6`) |
| 1 beep, pitch tracks the value | sharpen strength changed (`Shift+F7`/`F8`) |
| 2 beeps, pitch tracks the value | sharpen width changed (`Shift+F9`/`F10`) |
| 1 high tone (1200 Hz) | DLAA turned ON (`Shift+F11`) |
| 1 mid tone (300 Hz) | DLAA turned OFF (`Shift+F11`) |
| rising two-tone (600 → 900 Hz) | settings saved OK (`Shift+F12`) |
| one long low tone (200 Hz, ~0.4 s) | a setting is already at its limit, OR a save failed |
| long low tone then 1 beep | the chosen DLSS preset could not be created; fell back to `default` |
| 1 low tone / 2 low tones (250 Hz) | passive mode ON / OFF (`Ctrl+F7`) |
| 1 high tone (1200 Hz) / 1 mid tone (300 Hz) after `Ctrl+F4` | DLSS upscale ON / OFF (v0.7.0) |
| one long low tone with no key pressed | DLSS upscale could not be created for this size; fell back to DLAA (v0.7.0) |

A single long low 200 Hz tone has two meanings by context: right after a `Shift+F5..F10` press it
means "already at the min/max, unchanged"; right after `Shift+F12` it means the save failed.

## Change a key

Put a `key_<action>` line in `dlaa.ini` (next to `dinput8.dll`, read when the game starts). The value is
zero or more modifiers `Shift`, `Ctrl`, `Alt` joined with `+`, then one key. Not case-sensitive, spaces are
ignored. `none` (or an empty value) turns the action off. A value that is not understood is logged
(with `debug = 1`) and the default stays. Write the binding alone on the line, no comment after it.

```
key_dlaa_toggle = Ctrl+D
key_mode_cycle = none
key_save = Ctrl+Alt+S
```

Keys: `F1`-`F24`, `A`-`Z`, `0`-`9`, `Home`, `End`, `Insert`, `Delete`, `PageUp`, `PageDown`, `Up`, `Down`,
`Left`, `Right`, `Space`, `Tab`, `Backspace`, `Enter`, `Pause`, `Numpad0`-`Numpad9`, `NumpadAdd`,
`NumpadSubtract`, `NumpadMultiply`, `NumpadDivide`, `NumpadDecimal`, and the punctuation keys
``[ ] ; ' , . / \ - = ` ``.

| `dlaa.ini` key | Action | Default |
|----------------|--------|---------|
| `key_mode_cycle` | Mode DLAA → DLSS → off | `End` |
| `key_model_1` … `key_model_4` | DLAA model 1..4 | `Shift+F1` … `Shift+F4` |
| `key_area_down` / `key_area_up` | DLAA area −/+10 % | `Shift+F5` / `Shift+F6` |
| `key_sharpen_down` / `key_sharpen_up` | Sharpen strength −/+0.1 | `Shift+F7` / `Shift+F8` |
| `key_width_down` / `key_width_up` | Sharpen width −/+0.5 | `Shift+F9` / `Shift+F10` |
| `key_dlaa_toggle` | DLAA on / off | `Shift+F11` |
| `key_save` | Save settings to `dlaa.ini` | `Shift+F12` |
| `key_upscale_toggle` | DLSS upscale on / off | `Ctrl+F4` |
| `key_mv_toggle` | Motion vectors on / off | `Ctrl+F5` |
| `key_mv_debug` | MV debug view | `Ctrl+F6` |
| `key_passive` | Passive mode | `Ctrl+F7` |
| `key_jitter_only` | Jitter-only debug | `Ctrl+F8` |
| `key_selftest` | Self-test | `Ctrl+F9` |
| `key_snapshot` | NGX input snapshot | `Ctrl+F10` |
| `key_trace` | Frame trace | `Ctrl+F11` |
| `key_jitter_sign` | Cycle NGX jitter sign | `Ctrl+F12` |

If two actions get the same combination the log warns and both fire. At most one of the user keys (models,
area, sharpen, width, save) acts per frame.
