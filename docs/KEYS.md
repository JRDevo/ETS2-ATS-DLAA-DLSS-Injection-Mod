# ETS2 & ATS: DLAA & DLSS Injection Mod — hotkey cheat sheet (v0.10.0)

The tables below show the **default** keys. Every key can be changed in `dlaa.ini` (see
"Change a key" at the end). The game window must be focused. The modifiers must match
exactly — with the default `Shift+F11`, Shift+Ctrl+F11 or Shift+Alt+F11 does nothing, and a
key with no modifier (like `End`) only fires when Shift, Ctrl and Alt are all up.

- **Shift + F-key = USER keys** (the ones you use while playing)
- **Ctrl + F-key = DEBUG keys** (diagnostics / capture)
- **Alt + F5 / F6 = A/B switches** (v0.10.0: per-draw motion vectors, mirror DLAA)
- **Delete = the tuning menu** (v0.10.0: every setting on one panel; the arrow keys work only while it is open)


## The short version

**Daily keys** (the only ones most people ever press):

| Key | What it does |
|-----|--------------|
| `End` | Mode: DLAA → DLSS → off |
| `Alt+F9` | Performance profile: high → medium → low |
| `Shift+F12` | Save the current live settings to `dlaa.ini` |
| `Delete` | Tuning menu: every setting on one panel, arrow keys to change (v0.10.0) |

Everything else is tuning. The defaults are tuned already; you can ignore the rest.

**Tuning keys** (touch only when something looks wrong): `Shift+F1`..`F4` (model), `Shift+F5`/`F6` (DLAA area, VR),
`Shift+F7`..`F10` (sharpen strength / width), `Shift+F11` (DLAA on / off), `Ctrl+F1`/`F2` (texture sharpness),
`Ctrl+Shift+F1`/`F2` (cut-out sharpness), `Alt+F5` (per-part motion vectors), `Alt+F6` (mirror DLAA),
`Alt+F7` (DLAA before tone-map), `Ctrl+F3` (wire depth), `Ctrl+F4` (DLSS upscale).

**Debug keys** (need `debug = 1`; diagnostics and bug reports only): `Ctrl+F5`..`Ctrl+F12`, `Alt+F8` (motion dump).

The detailed tables below describe every key; they are grouped by modifier, not by how often you need them.

## The tuning menu (v0.10.0)

Press **`Delete`** in game: a panel with every setting opens over the picture (two short beeps; one low beep when it
closes). It works in flat and in VR (in VR it shows in both eyes and on the desktop mirror -- also on the VR main
menu and the garage / truck dealer screens, before a save is loaded).

| Key | In the menu |
|-----|-------------|
| `Delete` | Open / close the menu |
| `Up` / `Down` | Select a row (hold to scroll) |
| `Left` / `Right` | Change the selected row. Number rows step and repeat while held; switches, the mode, the profile and the model change once per press |

- **The game does not see these 5 keys while the menu is open** — the arrow keys steer, accelerate and brake in the
  default ETS2 / ATS layout, so the truck does not move while you change a value. When the menu is closed the keys go to
  the game as usual (the arrow keys do nothing for the mod then).
- **Every row names its hotkey**, so you can learn the keys from the menu. The hotkeys keep working while the menu is
  open, and the menu shows their changes at once. Each change logs and beeps like its key.
- The selected row shows a short description under the list. A greyed row does not apply to the current mode
  (`DLAA area` is VR only, `DLAA before tonemap` is flat only); it stays in the list.
- **Panel position X / Y, Panel size** move and size the panel (repeat makes it fast); flat and VR each keep their own
  values. **Panel VR depth** (VR only) shifts the panel inward per eye so it sits nearer than infinity: raise it if the
  panel is tiring to look at.
- **FPS counter** (on / off), **FPS counter X / Y, FPS counter size**: a small live fps box: the fps and the frame time as text with a dark outline, no background
  (`60 fps` / `16.7 ms`, updated twice a second) that stays on screen also while the menu is closed, in flat and in VR
  (both eyes + the desktop mirror). Flat and VR each keep their own position (`fps_show`, `fps_x`, `fps_y`, `fps_scale`,
  `fps_vr_x`, `fps_vr_y`, `fps_vr_scale` in `dlaa.ini`); off, it costs nothing. **FPS counter size** goes from `0.2` to
  `3.0` (flat default `1.0`; VR default `0.3`, a saved `fps_vr_scale` keeps its size).
- **Save to dlaa.ini** (or `Shift+F12`) writes the live values, and the panel position once you changed it
  (`menu_x`, `menu_y`, `menu_scale`, `menu_vr_x`, `menu_vr_y`, `menu_vr_scale`, `menu_vr_depth`) and the fps box once you
  changed it (the 7 `fps_*` keys), keeping every other line.
  The `Mode` row saves itself at once, like `End`.
- **GPU line and GPU cost column.** Under the title the panel shows what the GPU spends per frame, measured live and
  refreshed once a second: `game` (the game's own GPU frame, without the mod), `this mod` split into its parts (DLAA/DLSS
  itself, per-part motion, camera motion, see-through depth, mirrors, sharpen, copies; in VR one line per eye) and the fps.
  The `GPU cost` column shows what each row costs right now in GPU milliseconds per frame (VR: per eye, eye 0), `off` when
  that feature is off; the description of the selected row says what changes it. The mod's GPU timers switch on while the
  menu is open and off again when it closes (they stay on with `debug = 1`).
- The menu costs nothing while it is closed.

## User keys — Shift + F-key (Ctrl and Alt NOT held)

| Key        | Does                                   | Beep |
|------------|----------------------------------------|------|
| `End` (no modifier) | **Mode**: DLAA → DLSS → off → DLAA … (saved to `dlaa.ini` `mode` at once) | 1 high beep = DLAA, 2 high beeps = DLSS, 1 low beep = off |
| `Shift+F1` | Model 1 = preset `default` (driver)    | 1 beep |
| `Shift+F2` | Model 2 = preset `E`                   | 2 beeps |
| `Shift+F3` | Model 3 = preset `F`                   | 3 beeps |
| `Shift+F4` | Model 4 = preset `M`                   | 4 beeps |
| `Shift+F5` | DLAA area −10 % (down to 20 %)         | 3 quick beeps (pitch rises with area) |
| `Shift+F6` | DLAA area +10 %                        | 3 quick beeps (pitch rises with area) |
| `Shift+F7` | Sharpen strength −0.1                   | 1 beep (pitch rises with strength) |
| `Shift+F8` | Sharpen strength +0.1                   | 1 beep (pitch rises with strength) |
| `Shift+F9` | Sharpen width −0.5                      | 2 beeps (pitch rises with width) |
| `Shift+F10`| Sharpen width +0.5                      | 2 beeps (pitch rises with width) |
| `Shift+F11`| DLAA on / off                          | high 1200 Hz = ON, low 300 Hz = OFF |
| `Shift+F12`| **Save** current settings to `dlaa.ini` | rising two-tone (600→900 Hz) = saved; one long low tone = failed |

Model select is direct: pressing the model that is already active just re-beeps (no change).
`Shift+F12` saves `dlss_preset`, `dlaa_area`, `sharpness`, `sharp_radius`, `dlss_upscale` (v0.7.0), `tex_lod_bias`
(v0.9.0), `tex_lod_bias_cutout` (v0.10.0, the `Ctrl+Shift+F1` / `F2` value), `mv_objects` and `mirror_dlaa` (v0.10.0, the `Alt+F5` / `Alt+F6` states), `dlaa_pre_tonemap` (v0.10.0, the
`Alt+F7` state) and `perf_profile` (v0.10.0, the `Alt+F9` state) and leaves every other line of `dlaa.ini` untouched.
v0.10.0: `dlss_preset`, `dlaa_area` and `mirror_dlaa` are only written when you set them yourself (in `dlaa.ini` or with
their own key) -- a value that only comes from the performance profile is not written, so the next profile still moves it.

## Texture sharpness keys — Ctrl+F1 / Ctrl+F2 (new in v0.9.0)

| Key        | Does                                   | Beep |
|------------|----------------------------------------|------|
| `Ctrl+F1`  | Texture LOD bias −0.25: sharper textures (road signs, dashboard, GPS, decals), down to −3 | 1 beep (pitch rises as it gets sharper) |
| `Ctrl+F2`  | Texture LOD bias +0.25, back up to 0 (= off) | 1 beep (pitch rises as it gets sharper) |
| `Ctrl+Shift+F1` | Texture LOD bias of the alpha-tested draws (wire-mesh fences, grass, foliage cut-outs) −0.25, down to −3 (v0.10.0) | 2 beeps (pitch rises as it gets sharper) |
| `Ctrl+Shift+F2` | Texture LOD bias of the alpha-tested draws +0.25, back up to 0 (= the game's own filtering) (v0.10.0) | 2 beeps (pitch rises as it gets sharper) |

The bias only acts while DLAA or DLSS is on (it is what removes the extra shimmer of the sharper
textures) and only on the 3-D world and the menu truck-preview; the UI is never touched. Start
with −0.5 to −1. Save it with `Shift+F12` (`tex_lod_bias` in `dlaa.ini`).
v0.10.0: by default (`tex_lod_bias_scope = opaque`, `tex_aniso_scope = opaque` in `dlaa.ini`) the bias only reaches the
solid surfaces (signs, buildings, road, dashboard); see-through things (fences, wire mesh, wires, glass, plates, decals,
grass / leaf cut-outs) keep the game's filtering, so fences no longer show extra moire at a distance. `solid` = alpha-tested
cut-outs (wire-mesh fences, foliage) get the bias too; `all` = the v0.9.0 behaviour. The keys change only the bias value,
never the scope.
v0.10.0: the alpha-tested draws (wire-mesh fences, chain-link, grass, leaf / bush cut-outs) have a bias of their own,
`tex_lod_bias_cutout` (default −0.5): fences moire with a strong negative bias, grass goes soft without one, and the game
draws both the same way, so they share this value. `Ctrl+Shift+F1` / `Ctrl+Shift+F2` change it (0.25 steps within −3..0,
two beeps); `Ctrl+F1` / `Ctrl+F2` keep changing the bias of the solid surfaces only. 0 = those draws keep the game's own
filtering. It only acts while `tex_lod_bias` is not 0. `Shift+F12` saves it (`tex_lod_bias_cutout` in `dlaa.ini`).

## A/B switches — Alt + F5 / Alt + F6 / Alt + F7, dump Alt + F8 (new in v0.10.0; Shift and Ctrl NOT held)

| Key        | Does                                   | Beep |
|------------|----------------------------------------|------|
| `Alt+F5`   | Per-draw motion vectors on / off (`mv_objects` 2 ↔ 0, main view and mirrors). Off = every pixel uses the camera motion of its depth layer | high 1200 Hz = ON, low 300 Hz = OFF; one long low tone = ignored (`mv_objects = 1` in `dlaa.ini`) |
| `Alt+F6`   | Mirror DLAA on / off (each truck / car mirror gets its own DLAA; off = the mirrors are left as the game draws them) | high 1200 Hz = ON, low 300 Hz = OFF |
| `Alt+F7`   | DLAA before / after the game's tone-mapping (flat DLAA mode). ON = the main picture is anti-aliased on the game's own HDR colours before its brightness / bloom / tonemap step (calmer thin bright lines: lane paint, wires, rails); OFF = on the finished 8-bit picture as before v0.10.0. DLSS upscaling, VR and Windows HDR always use the after-tonemap path. OFF takes effect at once; ON after about 60 frames (v0.10.0 phase 7: the before-tonemap unit warms up first, so the switch shows no blur pulse) | high 1200 Hz = ON, low 300 Hz = OFF |
| `Alt+F8`   | Diagnostic dump: writes the per-part motion state of the next frame into the log (`MV dump n/N` lines: licence plates, decals, small parts and -- v0.10.0 phase 5 -- every part that found no partner in the previous frame, with what it inherited; `MV dump fwd-skip` lines: the see-through parts of one frame that get no motion of their own). Needs `debug = 1`; press it next to a truck when asked for a plate report | 1 short 900 Hz beep = requested; 1 long low tone = not available (per-draw motion vectors off) |
| `Alt+F9`   | Performance profile: cycles **high -> medium -> low** (v0.10.0). high = everything on; medium = for mid-range GPUs (model E, cheaper mirror / see-through / plate work); low = for weak GPUs (+ no mirror DLAA, no see-through depth, VR DLAA area 60 %). A profile only changes the settings you did not set yourself in `dlaa.ini` or with their own key. `Shift+F12` saves it (`perf_profile`) | 1 / 2 / 3 beeps (660 Hz) = high / medium / low |

`Alt+F5` .. `Alt+F7` start at their `dlaa.ini` value (`mv_objects = 2`, `mirror_dlaa = 1`, `dlaa_pre_tonemap = 1` by
default) and `Shift+F12` saves them. While the `Ctrl+F6` view is on, the main picture uses the after-tonemap path (exact
debug colours).

## Debug keys — Ctrl + F-key (Shift and Alt NOT held)

| Key        | Does                                   | Beep |
|------------|----------------------------------------|------|
| `Ctrl+F3`  | Forward depth for the motion vectors on / off (v0.9.0 r5; wires / fences / lane paint) | high 1200 Hz = ON, low 300 Hz = OFF |
| `Ctrl+F4`  | DLSS upscale on / off (v0.7.0)         | high 1200 Hz = ON, low 300 Hz = OFF |
| `Ctrl+F5`  | Motion vectors on / off                | — |
| `Ctrl+F6`  | MV debug view (see below)              | — |
| `Ctrl+F7`  | Passive mode on / off                  | 1 low tone = ON, 2 low tones = OFF |
| `Ctrl+F8`  | Jitter-only debug on / off             | — |
| `Ctrl+F9`  | Self-test (dumps frames)               | — |
| `Ctrl+F10` | NGX input snapshot (on the menu / truck-preview screen: a measuring capture, short freeze) | — |
| `Ctrl+F11` | Frame trace                            | — |
| `Ctrl+F12` | Cycle NGX jitter sign                  | — |

MV debug view (`Ctrl+F6`, needs DLAA or DLSS on): the picture is replaced by the motion vectors —
red = horizontal and green = vertical motion (still pixels are a flat olive), blue added = cabin
(half blue = your own truck's exterior). With the per-draw motion vectors (`mv_objects = 2`, the default since v0.10.0)
only MOVING parts stand out: **magenta** = a part that moves on its own (traffic, wheels, wipers, licence plates on a
moving truck; neighbouring moving parts differ slightly in hue; **pale magenta** = it moves on its own, but by less than
half a pixel against the camera here); everything that stands still is a **flat olive** (world) / **flat blue** (cabin)
-- the road and the scenery must never turn magenta; **white** = a part seen for the first time (nothing to match in the
previous frame: it keeps the camera motion for that frame); **grey** = vegetation / small props drawn in batches (always
the camera motion); a **yellow tint** on top = a see-through part drawn after the scene (licence plate, decal, glass,
overhead wire) that got its own motion (a plate on a moving truck: magenta with a yellow tint); an **orange tint**
(v0.10.0 phase 4) = a see-through part that sits on a moving part and takes that part's motion (a licence plate on a
moving bus: strong magenta with an orange tint -- before phase 4 such a plate could show PALE magenta + yellow and stay
jagged; since phase 5 also a solid part without a partner that sits on a moving part's centre); a **cyan tint** (v0.10.0
phase 5) = a part that found no partner in the previous frame and took the motion of the part drawn with exactly the same
placement (a licence plate on a moving truck: magenta with a cyan tint; on a parked truck: olive with a cyan tint -- before
phase 5 such a plate was WHITE and stayed jagged); a **green tint** (v0.10.0 phase 6) = a part that found no partner and took
the motion of the part it sits on (a licence plate the game rebuilds every frame, on a moving trailer: magenta with a green
tint; on a parked trailer: olive with a green tint -- it wins over cyan / orange on the same part); no tint = sky, or a pixel
no part could claim. With `mv_objects = 1` (the v0.9.0 stencil ids, legacy), moving objects that got their own motion
vectors are tinted **magenta**; **orange** = an object id whose own motion could not be used this
frame (it falls back to the camera motion; v0.9.0 r4: also when one of its parts did not move the way the object
did, e.g. right after two identical cars swapped places in the game's draw order); **cyan** = something that looked like it was moving but was filtered out
(repeated mesh such as grass or windows, unreliable match, your own truck, jumpy motion) and keeps the camera motion.
While this view is on, one of the 15 object ids is used for the cyan marking.
**Yellow** (v0.9.0 r5, `mv_fwd_depth = 1`) = pixels whose depth came from a see-through world draw (overhead wire,
power cable, fence, lane paint, glass) instead of what lies behind it; those pixels now move with their own distance.
Strong yellow = such a pixel without its own part id (it keeps the camera motion at its own distance); with per-draw
motion vectors most of them show their part's colour with a light yellow tint instead (see above). `Ctrl+F3` switches
the see-through depth -- and with it the see-through part ids -- off and on live.
**Mirrors** (v0.10.0, `mirror_dlaa = 1`): every mirror that gets its own DLAA shows the same motion-vector view inside a
coloured frame (one colour per mirror unit: red, green, blue, yellow, cyan, orange, white, purple); your own trailer /
truck in a mirror should be magenta (it moves with the mirror), the road and scenery not. A mirror with the normal
picture and no frame is not handled (see the `mirror units` lines in the log).

## Beep patterns at a glance

| Sound | Means |
|-------|-------|
| 1 / 2 / 3 / 4 beeps at the same pitch (880 Hz) | DLAA model 1 / 2 / 3 / 4 selected (`Shift+F1..F4`) |
| 3 quick beeps, pitch tracks the value | DLAA area changed (`Shift+F5`/`F6`) |
| 1 beep, pitch tracks the value | sharpen strength changed (`Shift+F7`/`F8`) |
| 2 beeps, pitch tracks the value | sharpen width changed (`Shift+F9`/`F10`) |
| 1 beep, 400 Hz at 0 rising to 1000 Hz at −3 | texture LOD bias changed (`Ctrl+F1`/`F2`, v0.9.0) |
| 2 beeps, 400 Hz at 0 rising to 1000 Hz at −3 | alpha-tested (fence / grass) texture LOD bias changed (`Ctrl+Shift+F1`/`F2`, v0.10.0) |
| 1 high tone (1200 Hz) | DLAA turned ON (`Shift+F11`) |
| 1 mid tone (300 Hz) | DLAA turned OFF (`Shift+F11`) |
| rising two-tone (600 → 900 Hz) | settings saved OK (`Shift+F12`) |
| one long low tone (200 Hz, ~0.4 s) | a setting is already at its limit, OR a save failed |
| long low tone then 1 beep | the chosen DLSS preset could not be created; fell back to `default` |
| 1 low tone / 2 low tones (250 Hz) | passive mode ON / OFF (`Ctrl+F7`) |
| 1 high tone (1200 Hz) / 1 mid tone (300 Hz) after `Alt+F5` / `Alt+F6` / `Alt+F7` | per-draw motion vectors / mirror DLAA / DLAA before the tonemap ON / OFF (v0.10.0) |
| 1 short 900 Hz beep after `Alt+F8` | per-draw dump requested (lines in the log 2-4 frames later) |
| 1 / 2 / 3 beeps (660 Hz) after `Alt+F9` | performance profile high / medium / low (v0.10.0) |
| 1 high tone (1200 Hz) / 1 mid tone (300 Hz) after `Ctrl+F4` | DLSS upscale ON / OFF (v0.7.0) |
| one long low tone with no key pressed | DLSS upscale could not be created for this size; fell back to DLAA (v0.7.0) |

A single long low 200 Hz tone has two meanings by context: right after a `Shift+F5..F10` or
`Ctrl+F1`/`F2` (or `Ctrl+Shift+F1`/`F2`) press it means "already at the min/max, unchanged"; right after `Shift+F12` it means
the save failed.

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
| `key_lod_bias_down` / `key_lod_bias_up` | Texture LOD bias −/+0.25 (v0.9.0) | `Ctrl+F1` / `Ctrl+F2` |
| `key_lod_cutout_down` / `key_lod_cutout_up` | Texture LOD bias of the alpha-tested draws (`tex_lod_bias_cutout`) −/+0.25 (v0.10.0) | `Ctrl+Shift+F1` / `Ctrl+Shift+F2` |
| `key_fwd_depth` | Forward depth for the motion vectors on / off (v0.9.0 r5) | `Ctrl+F3` |
| `key_mv_toggle` | Motion vectors on / off | `Ctrl+F5` |
| `key_mv_debug` | MV debug view | `Ctrl+F6` |
| `key_passive` | Passive mode | `Ctrl+F7` |
| `key_jitter_only` | Jitter-only debug | `Ctrl+F8` |
| `key_selftest` | Self-test | `Ctrl+F9` |
| `key_snapshot` | NGX input snapshot (menu / truck-preview screen: measuring capture) | `Ctrl+F10` |
| `key_trace` | Frame trace | `Ctrl+F11` |
| `key_jitter_sign` | Cycle NGX jitter sign | `Ctrl+F12` |
| `key_mv_drawids` | Per-draw motion vectors on / off (`mv_objects` 2 ↔ 0, v0.10.0) | `Alt+F5` |
| `key_mirror_dlaa` | Mirror DLAA on / off (v0.10.0) | `Alt+F6` |
| `key_pre_tonemap` | Main-picture DLAA before / after the game's tonemap (v0.10.0) | `Alt+F7` |
| `key_mv_dump` | Per-draw motion-vector dump of the next frame into the log (v0.10.0 diagnostic) | `Alt+F8` |
| `key_perf_profile` | Performance profile high -> medium -> low (v0.10.0) | `Alt+F9` |
| `key_menu` | Tuning menu open / close (v0.10.0) | `Delete` |
| `key_menu_up` / `key_menu_down` | Tuning menu: select the row above / below (repeats while held; menu open only) (v0.10.0) | `Up` / `Down` |
| `key_menu_left` / `key_menu_right` | Tuning menu: change the selected row down / up (repeats on the step rows; menu open only) (v0.10.0) | `Left` / `Right` |

If two actions get the same combination the log warns and both fire. At most one of the user keys (models,
area, sharpen, width, texture LOD bias, save) acts per frame.
