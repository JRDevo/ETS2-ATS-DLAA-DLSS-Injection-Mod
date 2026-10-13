# ETS2 & ATS: DLAA & DLSS Injection Mod — hotkey cheat sheet

The tables below show the **default** keys. Every key can be changed in `dlaa.ini` (see
"Change a key" at the end). The game window must be focused. The modifiers must match
exactly — with the default `Shift+F11`, Shift+Ctrl+F11 or Shift+Alt+F11 does nothing, and a
key with no modifier (like `End`) only fires when Shift, Ctrl and Alt are all up.

- **Shift + F-key = USER keys** (the ones you use while playing)
- **Ctrl + F-key = DEBUG keys** (diagnostics / capture)
- **Alt + F5 / F6 = A/B switches** (per-draw motion vectors, mirror DLAA)
- **Delete = the tuning menu** (every setting on one panel; the arrow keys work only while it is open)


## The short version

**Daily keys** (the only ones most people ever press):

| Key | What it does |
|-----|--------------|
| `End` | Mode: DLAA → DLSS → off |
| `Alt+F9` | Performance profile: high → medium → low |
| `Shift+F12` | Save the current live settings to `dlaa.ini` |
| `Delete` | Tuning menu: every setting on one panel, arrow keys to change |

Everything else is tuning. The defaults are tuned already; you can ignore the rest.

**Tuning keys** (touch only when something looks wrong): `Shift+F1`..`F4` (model), `Shift+F5`/`F6` (DLAA area, VR),
`Shift+F7`..`F10` (sharpen strength / width), `Shift+F11` (DLAA on / off), `Ctrl+F1`/`F2` (texture sharpness),
`Ctrl+Shift+F1`/`F2` (cut-out sharpness), `Alt+F5` (per-part motion vectors), `Alt+F6` (mirror DLAA),
`Alt+F7` (DLAA before tone-map), `Ctrl+F3` (wire depth), `Ctrl+F4` (DLSS upscale).

**Debug keys** (need `log_level = 4`; diagnostics and bug reports only): `Ctrl+F5`..`Ctrl+F12`, `Alt+F8` (motion dump).

The detailed tables below describe every key; they are grouped by modifier, not by how often you need them.

## The tuning menu

Press **`Delete`** in game: a panel with every setting opens over the picture (two short beeps; one low beep when it
closes). It works in flat and in VR (in VR it shows in both eyes and on the desktop mirror -- also on the VR main
menu and the garage / truck dealer screens, before a save is loaded).

| Key | In the menu |
|-----|-------------|
| `Delete` | Open / close the menu |
| `Up` / `Down` | Select a row (hold to scroll) |
| `Left` / `Right` | Change the selected row. Number rows step and repeat while held; switches, the mode, the profile and the model change once per press. On a group row (`Panel settings`, `FPS counter settings`) `Right` opens the group, `Left` closes it |

- **Two groups keep the list short** (v0.10.3): **Panel settings** (`Panel position X`, `Panel position Y`, `Panel size`,
  `Panel VR depth`, `Panel faces you (VR)`, `Menu keys`) and **FPS counter settings** (`FPS counter lock`, `FPS counter X`,
  `FPS counter Y`, `FPS counter size`; the `FPS counter` on / off row itself stays in the main list, right above the
  group). Both are **closed every time the menu opens** (the value shows `...`): select the group row and press `Right` to
  show its rows (`shown`), `Left` on the group row hides them again. `Up` / `Down` skip the rows of a closed group, and
  the panel gets shorter while a group is closed. If the menu was closed on a row inside a group, it reopens on that
  group's row. The group rows hold no setting (nothing to save or reset).
- **The line at the bottom** names the menu keys by their keyboard key only (`Up / Down select   Left / Right change
  Delete close   keys: Hotkeys...`), whatever wheel buttons are bound too; it is the same on every row (the Reset hold shows
  its bar in the row itself). With long key names the hint is left out so the line fits.

- **Only the 5 menu keys are kept from the game while the menu is open** (v0.13.0): `Delete`, `Up`, `Down`, `Left`,
  `Right` (or whatever they are bound to, with any `Shift` / `Ctrl` / `Alt`, and their wheel buttons) -- the arrow keys
  steer, accelerate and brake in the default ETS2 / ATS layout. Every other key, every other wheel / gamepad button, the
  shifter, steering and pedals reach the game, so **you can keep driving with the menu open**. Only while the
  [Hotkeys page](#bind-a-key-in-the-menu) waits for a key (a few seconds at most) does the game get no keys or buttons at
  all, so the key you bind does nothing in the game. Key and button releases always reach the game; `Alt+F4` always
  works. When the menu is closed every key goes to the game as usual (the arrow keys do nothing for the mod then).
  (v0.10.2 to v0.12.0 kept every key and button from the game while the menu was open; before that only the 5 menu keys.)
- **Bind a key in the menu:** `Hotkeys...` row, `Right`, pick the action, `Right`, press the new key or button, see
  [Bind a key in the menu](#bind-a-key-in-the-menu).
- **Every row names its hotkey**, so you can learn the keys from the menu. The hotkeys keep working while the menu is
  open, and the menu shows their changes at once. Each change logs and beeps like its key.
- The selected row shows a description under the list, in plain words: what the row does, when you would change it, and
  what it costs or saves (the measured figure is added while the GPU timers run). A greyed row does not apply to the
  current mode and says why (`DLAA area`, `Panel VR depth` and `Panel faces you` are VR only; `DLAA before tonemap` is flat
  only; `OFXR Bridge` is greyed when `ofxr_bridge = 0`); it stays in the list. The **Performance profile** description
  lists what `medium` and `low` change; **Save to dlaa.ini** says what it writes.
- **Panel position X / Y, Panel size** move and size the panel (repeat makes it fast); flat and VR each keep their own
  values. **Panel VR depth** (VR only) shifts the panel inward per eye so it sits nearer than infinity: raise it if the
  panel is tiring to look at. **Panel faces you (VR)** (on by default, `menu_vr_face`): moved off the centre, the panel
  turns towards your eye like a real screen and keeps its size from every angle; off = a flat picture. In VR the panel and
  the fps box are drawn on top of the game's own menu (garage / truck dealer screens) once the mod has confirmed where the
  game hands each eye picture on (a few frames after the menu opens). **v0.10.3, `menu_vr_layer = 1` (default):** in VR
  the panel and the fps box are separate OpenXR quad layers the headset's compositor places itself -- steady (no swimming
  with the reprojection), the panel 1 m (default) and the fps box 0.8 m away, never covered by the game's menus. There
  **Panel VR depth** is the panel's real distance, shown in metres (`menu_vr_dist`, 0.30 - 3.00 m in 5 cm steps; the panel
  keeps its size, only its depth changes), and **Panel faces you (VR)** turns the panel -- and the fps box while it
  follows your head -- towards you as a true 3-D layer (off = parallel to your view, a flat picture seen at an angle).
  With `menu_vr_layer = 0` (the panel drawn into the picture; also the automatic fallback when the layers are not
  possible) **Panel VR depth** is the inward shift in px (`menu_vr_depth`) as above.
- **FPS counter** (on / off), **FPS counter X / Y, FPS counter size**: a small live fps box: the fps and the frame time as text with a dark outline, no background
  (`60 fps` / `16.7 ms`, updated twice a second) that stays on screen also while the menu is closed, in flat and in VR
  (both eyes + the desktop mirror). Flat and VR each keep their own position (`fps_show`, `fps_x`, `fps_y`, `fps_scale`,
  `fps_vr_x`, `fps_vr_y`, `fps_vr_scale` in `dlaa.ini`); off, it costs nothing. On, its GPU cost is measured and shown in
  the row's cost column, per frame (every picture it is drawn into that frame together). **FPS counter size** goes from `0.2` to
  `3.0` (flat default `1.0`; VR default `0.3`, a saved `fps_vr_scale` keeps its size).
- **FPS counter lock** (v0.10.3, VR only, greyed in flat): `Follow head` (default) = the fps box moves with the headset;
  `Fixed in cab` = it stays at one spot in the cab, 0.8 m from your seat position, like a cab instrument (`fps_vr_lock` in
  `dlaa.ini`). While fixed, **FPS counter X / Y** turn it around you in degrees (X + = right, Y + = down) and **FPS counter
  size** is its size at 0.8 m. It follows the headset again by itself while the game's VR view poses are not available.
- **Mirror DLAA** (v0.10.2) has seven states, `Left` / `Right` step through them (wrapping around): `off` -> `1 mirror
  (largest)` -> `1 mirror, camera` -> `2 mirrors (largest)` -> `2 mirrors, camera` -> `all mirrors` -> `all, camera
  motion`. `1 mirror` keeps DLAA only on the largest mirror picture (the main mirror next to you), `2 mirrors` on the two
  largest, `all` on every mirror. Without `camera` each moving part (traffic, wheels) moves exactly in the glass (about
  0.4 ms per mirror in VR); with `camera` the mirror uses the camera motion only: the cheapest per mirror (about 0.15 ms),
  but moving traffic can ghost in the glass. The row sets `mirror_dlaa` plus, for the mode you are in, `mirror_vr_mode`
  (VR) or `mirror_flat_mode` (flat), `mirror_vr_views` and `mirror_mv`: `1 mirror` = mode 2 with `mirror_vr_views = 1`,
  `2 mirrors` = mode 2 with `mirror_vr_views = 2` (each with `mirror_mv = parts`, or `camera` for the `, camera` state),
  `all mirrors` = mode 1 with `mirror_mv = parts`, `all, camera motion` = mode 4; `off` = `mirror_dlaa` off (the mode
  stays). `mirror_mv` is one key for VR and flat. Beeps: 1 short beep = 1 mirror, 2 = 2 mirrors, 3 = all; high (1200 Hz)
  = per-part motion, lower (800 Hz) = camera motion only; one low tone = off. `Alt+F6` still switches mirror DLAA on / off
  with the state it had; a `dlaa.ini` / profile value that is none of the seven shows as it is (`3 largest`, `3 largest,
  camera`, `all, every 2nd frame`, `all, 2nd fr., camera`, `off (mode 0)`) and `Left` / `Right` go to the nearest state on
  that side.
- **Mirrors in turns** (phase 41, `mirror_turns`, off by default): `Left` / `Right` switch it. On, each mirror gets its DLAA
  every 2nd frame and shows its last clean picture in between: about half the mirror cost. Works with every Mirror DLAA
  state; greyed while Mirror DLAA is off, and shows `on (mode 3)` with a `dlaa.ini` mirror mode 3 (already in turns).
  Beeps: high = on, low = off. `Shift+F12` / the Save row writes it, the Reset row removes it.
- **Save to dlaa.ini** (or `Shift+F12`) writes the live values, and the panel position once you changed it
  (`menu_x`, `menu_y`, `menu_scale`, `menu_vr_x`, `menu_vr_y`, `menu_vr_scale`, `menu_vr_depth`, `menu_vr_dist`, `menu_vr_face`) and the fps box once you
  changed it (the 7 `fps_*` keys), keeping every other line.
  The `Mode` row saves itself at once, like `End`.
- **Reset to defaults** puts every row back to the fresh-install values, live and in `dlaa.ini` (the keys are removed
  from the file; hotkeys and other lines stay). v0.10.3: **hold `Right` 5 s** on the row (the menu right key, keyboard or
  its wheel button; the time is `bind_hold_ms`): the Hotkey column reads `hold Right 5 s`, fills a progress bar (one
  square per second), then resets once (`reset done` until you let go; press and hold again for another reset). Let go earlier =
  nothing happens; `Left` and a plain tap of `Right` do nothing. It beeps like Save (rising two-tone = done, one long low tone = the file
  could not be written).
- **GPU line and GPU cost column.** Under the title the panel shows what the GPU spends per frame, measured live and
  refreshed once a second: `game` (the game's own GPU frame, without the mod), `this mod` split into its parts (DLAA/DLSS
  itself, per-part motion, camera motion, see-through depth, mirrors, sharpen, copies; in VR one line per eye) and the fps.
  The `GPU cost` column shows what each row costs right now in GPU milliseconds per frame (VR: per eye, eye 0), `off` when
  that feature is off; the description of the selected row says what changes it. `Mode` and `DLAA on / off` show the whole
  mod (what switching it off saves), `DLSS upscale` shows `no extra` (it is the same pass; the game just draws fewer
  pixels), the two texture sharpness rows show `~0 ms` (memory only, no GPU pass), `-` = no cost of its own. The mod's GPU timers switch on while the
  menu is open and off again when it closes (they stay on with `log_level = 4`).
- The menu costs nothing while it is closed.

## Bind a key in the menu

(v0.13.0; GitHub issue #16) Instead of editing `dlaa.ini`, open the menu, select **`Hotkeys...`** (near the bottom, above
`Save to dlaa.ini`) and press `Right`. The panel shows the **Hotkeys page**: `< Back` first, then one line per action --
`Mode`, `Performance profile`, `DLAA model 1` .. `4`, `Sharpen strength -` / `+`, `Sharpen width -` / `+`,
`Texture sharpness -` / `+`, `Cut-out sharpness -` / `+`, `DLAA area (VR) -` / `+`, `Per-part motion vectors`,
`Mirror DLAA on / off`, `DLAA before tonemap (flat)`, `See-through depth`, `DLSS upscale`, `DLAA on / off`,
`Save to dlaa.ini` and the five menu keys (`Menu: open / close`, `up`, `down`, `left`, `right`). The `Keyboard` column
shows the key, the `Controller` column the wheel / gamepad button or D-pad direction (`-` = none). `Up` / `Down` select a
line; `Left` (or `Right` on `< Back`) goes back to the main menu; the open key closes the menu as everywhere.

**Bind:** select a line and press `Right`. The line reads `Press a key or button...  (Left = cancel, Backspace = clear)`.
Press the new key (hold `Shift` / `Ctrl` / `Alt` with it for a combination: `Ctrl` + `F5` binds `Ctrl+F5`) or the new wheel /
gamepad button or D-pad direction. The mod beeps twice (rising, like Save), the line shows `= Ctrl+F5` for 3 seconds, the
action's hotkey is the new key at once and the `key_<action>` line in `dlaa.ini` is rewritten (every other line stays).
The log says what changed, e.g.
`hotkey key_sharpen_up = Shift+F8, Wheel5 (bound on the Hotkeys page: pressed Wheel5; was "Shift+F8")`.

- **Only a new press counts.** A key or button that is already held when you press `Right` -- an H-shifter in gear (the
  shifter is a set of buttons, one held all the time), a pedal key, the `Right` you just pressed -- is ignored until you
  let go of it and press it again. A wheel the game has not read for a moment (it reads as "nothing pressed" then) does
  not count as a release either. So an engaged gear can never be bound by accident (issue #16: with v0.10.2 to v0.12.0's
  hold-to-bind, a player in gear bound the gear to the Mode row). Steering, pedals and other axes never bind. Two keys /
  buttons pressed in the same frame are ignored: press one.
- **Cancel:** the menu `Left` key, `Esc`, or 10 seconds without a press (one low beep, `cancelled` for 3 s); also closing
  the menu or switching to another window.
- **`Backspace` clears** the action's hotkey to `none` (keyboard key and wheel button; one low beep). So `Backspace`
  itself cannot be bound in the menu (it can in `dlaa.ini`); neither can `Esc`.
- **Keyboard and wheel stay separate:** a key replaces the action's keyboard key and keeps its wheel button; a wheel
  button replaces the wheel button and keeps the key (`key_menu = Delete, Wheel10` + pressing `Wheel12` =
  `Delete, Wheel12`). Only keys from the key list in [Change a key](#change-a-key) can be bound (not `Escape`,
  `Caps Lock`, the Windows keys or the mouse).
- **Taken from another hotkey:** if the key (with the same `Shift` / `Ctrl` / `Alt`) or the button already belongs to
  another action, it is taken away from that one (its key or button becomes empty, also written to `dlaa.ini`); the line
  shows `taken from key_...` for 3 seconds and the log names it.
- **The press does nothing else:** while a line waits, no hotkey fires and the game gets no key or button, and the key /
  button you bind with stays hidden from the game until you let it go. (With v0.10.2 to v0.12.0's hold-to-bind the held
  key did its old job once.)
- **The menu's own keys** (`Delete`, `Up`, `Down`, `Left`, `Right`, or whatever they are bound to, also with modifiers,
  and their wheel buttons) cannot be bound on the other lines: there `Up` / `Down` / `Right` do nothing and `Left`
  cancels.
- **The five menu-key lines** (`Menu: open / close`, `up`, `down`, `left`, `right`; the `Menu keys` row in the
  **Panel settings** group shows them and `Right` on it opens the Hotkeys page there): here every key counts, the menu
  keys too -- `Left` binds instead of cancelling and the open key binds instead of closing the menu. To get out without
  binding press `Esc` or `Backspace`, or wait 10 seconds. These lines cannot be cleared, and a press that would take the
  **only** binding away from any of the five menu actions (`key_menu`, `key_menu_up` ... `key_menu_right`) is refused
  (one long low tone, the line keeps waiting): the menu must stay reachable, and `Right` is what opens the groups, the
  Hotkeys page and Reset. To swap two menu keys, give one of them a third key first (`Menu: left` = `Numpad4`, then
  `Menu: right` = `Left`, then `Menu: left` = `Right`).
- `bind_hold_ms` is no longer used for binding; it is only the [Reset row](#the-tuning-menu)'s hold time.

## User keys — Shift + F-key (Ctrl and Alt NOT held)

| Key        | Does                                   | Beep |
|------------|----------------------------------------|------|
| `End` (no modifier) | **Mode**: DLAA → DLSS → off → DLAA … (saved to `dlaa.ini` `mode` at once) | 1 high beep = DLAA, 2 high beeps = DLSS, 1 low beep = off |
| `Shift+F1` | Model 1 = preset `default` (driver)    | 1 beep |
| `Shift+F2` | Model 2 = preset `E`                   | 2 beeps |
| `Shift+F3` | Model 3 = preset `F`                   | 3 beeps |
| `Shift+F4` | Model 4 = preset `M`                   | 4 beeps |
| `Shift+F5` | DLAA area −10 % (down to 10 %)         | 3 quick beeps (pitch rises with area) |
| `Shift+F6` | DLAA area +10 %                        | 3 quick beeps (pitch rises with area) |
| `Shift+F7` | Sharpen strength −0.1                   | 1 beep (pitch rises with strength) |
| `Shift+F8` | Sharpen strength +0.1                   | 1 beep (pitch rises with strength) |
| `Shift+F9` | Sharpen width −0.5                      | 2 beeps (pitch rises with width) |
| `Shift+F10`| Sharpen width +0.5                      | 2 beeps (pitch rises with width) |
| `Shift+F11`| DLAA on / off                          | high 1200 Hz = ON, low 300 Hz = OFF |
| `Shift+F12`| **Save** current settings to `dlaa.ini` | rising two-tone (600→900 Hz) = saved; one long low tone = failed |

Model select is direct: pressing the model that is already active just re-beeps (no change).
`Shift+F12` saves `dlss_preset`, `dlaa_area`, `sharpness`, `sharp_radius`, `dlss_upscale`, `tex_lod_bias`, `tex_lod_bias_cutout` (the `Ctrl+Shift+F1` / `F2` value), `mv_objects` and `mirror_dlaa` (the `Alt+F5` / `Alt+F6` states), `dlaa_pre_tonemap` (the
`Alt+F7` state) and `perf_profile` (the `Alt+F9` state) and leaves every other line of `dlaa.ini` untouched.
v0.10.0: `dlss_preset`, `dlaa_area` and `mirror_dlaa` are only written when you set them yourself (in `dlaa.ini` or with
their own key) -- a value that only comes from the performance profile is not written, so the next profile still moves it.
v0.10.2: the same for `mirror_vr_mode` / `mirror_flat_mode` (set in `dlaa.ini` or with the menu's Mirror DLAA row);
`mirror_vr_views` is written with `mirror_vr_mode`, and with `mirror_flat_mode` when that is `2`; `mirror_mv` (`parts` /
`camera`) is written with either mode. The menu's Reset row removes all four.

## Texture sharpness keys — Ctrl+F1 / Ctrl+F2

| Key        | Does                                   | Beep |
|------------|----------------------------------------|------|
| `Ctrl+F1`  | Texture LOD bias −0.25: sharper textures (road signs, dashboard, GPS, decals), down to −3 | 1 beep (pitch rises as it gets sharper) |
| `Ctrl+F2`  | Texture LOD bias +0.25, back up to 0 (= off) | 1 beep (pitch rises as it gets sharper) |
| `Ctrl+Shift+F1` | Texture LOD bias of the alpha-tested draws (wire-mesh fences, grass, foliage cut-outs) −0.25, down to −3 | 2 beeps (pitch rises as it gets sharper) |
| `Ctrl+Shift+F2` | Texture LOD bias of the alpha-tested draws +0.25, back up to 0 (= the game's own filtering) | 2 beeps (pitch rises as it gets sharper) |

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

## A/B switches — Alt + F5 / Alt + F6 / Alt + F7, dump Alt + F8 (Shift and Ctrl NOT held)

| Key        | Does                                   | Beep |
|------------|----------------------------------------|------|
| `Alt+F5`   | Per-draw motion vectors on / off (`mv_objects` 2 ↔ 0, main view and mirrors). Off = every pixel uses the camera motion of its depth layer | high 1200 Hz = ON, low 300 Hz = OFF; one long low tone = ignored (`mv_objects = 1` in `dlaa.ini`) |
| `Alt+F6`   | Mirror DLAA on / off (each truck / car mirror gets its own DLAA; off = the mirrors are left as the game draws them) | high 1200 Hz = ON, low 300 Hz = OFF |
| `Alt+F7`   | DLAA before / after the game's tone-mapping (flat DLAA mode). ON = the main picture is anti-aliased on the game's own HDR colours before its brightness / bloom / tonemap step (calmer thin bright lines: lane paint, wires, rails); OFF = on the finished 8-bit picture as before v0.10.0. DLSS upscaling, VR and Windows HDR always use the after-tonemap path. OFF takes effect at once; ON after about 60 frames (v0.10.0 phase 7: the before-tonemap unit warms up first, so the switch shows no blur pulse) | high 1200 Hz = ON, low 300 Hz = OFF |
| `Alt+F8`   | Diagnostic dump: writes the per-part motion state of the next frame into the log (`MV dump n/N` lines: licence plates, decals, small parts and -- v0.10.0 phase 5 -- every part that found no partner in the previous frame, with what it inherited; `MV dump fwd-skip` lines: the see-through parts of one frame that get no motion of their own). Needs `debug = 1`; press it next to a truck when asked for a plate report. v0.10.0 phase 21: the same press also writes `MV probe` lines -- a 5 x 5 grid of pixels 48 px apart around the centre of your view (VR: eye 0's optical centre; flat: the screen centre): for each pixel its depth, which surface won, cabin / world layer, the motion it got and the parts under it (`MV probe (...)` header + 25 pixel lines + one summary). Look straight at the problem spot (e.g. a pink shop window in the `Ctrl+F6` view) when you press it | 1 short 900 Hz beep = requested; 1 long low tone = not available (per-draw motion vectors off) |
| `Alt+F9`   | Performance profile: cycles **high -> medium -> low**. high = everything on; medium = for mid-range GPUs (model E, cheaper mirror / see-through / plate work); low = for weak GPUs (+ no mirror DLAA, no see-through depth, VR DLAA area 60 %). A profile only changes the settings you did not set yourself in `dlaa.ini` or with their own key. `Shift+F12` saves it (`perf_profile`) | 1 / 2 / 3 beeps (660 Hz) = high / medium / low |

`Alt+F5` .. `Alt+F7` start at their `dlaa.ini` value (`mv_objects = 2`, `mirror_dlaa = 1`, `dlaa_pre_tonemap = 1` by
default) and `Shift+F12` saves them. While the `Ctrl+F6` view is on, the main picture uses the after-tonemap path (exact
debug colours).

## Debug keys — Ctrl + F-key (Shift and Alt NOT held)

| Key        | Does                                   | Beep |
|------------|----------------------------------------|------|
| `Ctrl+F3`  | Forward depth for the motion vectors on / off (wires / fences / lane paint) | high 1200 Hz = ON, low 300 Hz = OFF |
| `Ctrl+F4`  | DLSS upscale on / off         | high 1200 Hz = ON, low 300 Hz = OFF |
| `Ctrl+F5`  | Motion vectors on / off                | — |
| `Ctrl+F6`  | MV debug view (see below)              | — |
| `Ctrl+F7`  | Passive mode on / off                  | 1 low tone = ON, 2 low tones = OFF |
| `Ctrl+F8`  | Jitter-only debug on / off             | — |
| `Ctrl+F9`  | Self-test (dumps frames: `<mode>_<n>.bmp` = DLAA output, `final_<mode>_<n>.bmp` = the picture on screen, same frame) | — |
| `Ctrl+F10` | NGX input snapshot, each press into its own folder `dlaa_snap\<n>_blit<blit>\` (on the menu / truck-preview screen: a measuring capture, short freeze) | — |
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
**Yellow** (`mv_fwd_depth = 1`) = pixels whose depth came from a see-through world draw (overhead wire,
power cable, fence, lane paint, glass) instead of what lies behind it; those pixels now move with their own distance.
Strong yellow = such a pixel without its own part id (it keeps the camera motion at its own distance); with per-draw
motion vectors most of them show their part's colour with a light yellow tint instead (see above). `Ctrl+F3` switches
the see-through depth -- and with it the see-through part ids -- off and on live.
**Mirrors** (`mirror_dlaa = 1`): every mirror that gets its own DLAA shows the same motion-vector view inside a
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
| 1 beep, 400 Hz at 0 rising to 1000 Hz at −3 | texture LOD bias changed (`Ctrl+F1`/`F2`) |
| 2 beeps, 400 Hz at 0 rising to 1000 Hz at −3 | alpha-tested (fence / grass) texture LOD bias changed (`Ctrl+Shift+F1`/`F2`) |
| 1 high tone (1200 Hz) | DLAA turned ON (`Shift+F11`) |
| 1 mid tone (300 Hz) | DLAA turned OFF (`Shift+F11`) |
| rising two-tone (600 → 900 Hz) | settings saved OK (`Shift+F12`) |
| one long low tone (200 Hz, ~0.4 s) | a setting is already at its limit, OR a save failed |
| long low tone then 1 beep | the chosen DLSS preset could not be created; fell back to `default` |
| 1 low tone / 2 low tones (250 Hz) | passive mode ON / OFF (`Ctrl+F7`) |
| 1 high tone (1200 Hz) / 1 mid tone (300 Hz) after `Alt+F5` / `Alt+F6` / `Alt+F7` | per-draw motion vectors / mirror DLAA / DLAA before the tonemap ON / OFF |
| 1 short 900 Hz beep after `Alt+F8` | per-draw dump requested (lines in the log 2-4 frames later) |
| 1 / 2 / 3 beeps (660 Hz) after `Alt+F9` | performance profile high / medium / low |
| 1 high tone (1200 Hz) / 1 mid tone (300 Hz) after `Ctrl+F4` | DLSS upscale ON / OFF |
| one long low tone with no key pressed | DLSS upscale could not be created for this size; fell back to DLAA |

A single long low 200 Hz tone has two meanings by context: right after a `Shift+F5..F10` or
`Ctrl+F1`/`F2` (or `Ctrl+Shift+F1`/`F2`) press it means "already at the min/max, unchanged"; right after `Shift+F12` it means
the save failed.

## Change a key

Put a `key_<action>` line in `dlaa.ini` (next to `dinput8.dll`, read when the game starts), or bind it in the game:
[Bind a key in the menu](#bind-a-key-in-the-menu). The value is
zero or more modifiers `Shift`, `Ctrl`, `Alt` joined with `+`, then one key (and optionally a wheel / gamepad button after
a comma, see [below](#wheel-gamepad-and-joystick-buttons)). Not case-sensitive, spaces are
ignored. `none` (or an empty value) turns the action off. A value that is not understood is logged
(with `log_level = 2` or more) and the default stays. Write the binding alone on the line, no comment after it.

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
| `key_lod_bias_down` / `key_lod_bias_up` | Texture LOD bias −/+0.25 | `Ctrl+F1` / `Ctrl+F2` |
| `key_lod_cutout_down` / `key_lod_cutout_up` | Texture LOD bias of the alpha-tested draws (`tex_lod_bias_cutout`) −/+0.25 | `Ctrl+Shift+F1` / `Ctrl+Shift+F2` |
| `key_fwd_depth` | Forward depth for the motion vectors on / off | `Ctrl+F3` |
| `key_mv_toggle` | Motion vectors on / off | `Ctrl+F5` |
| `key_mv_debug` | MV debug view | `Ctrl+F6` |
| `key_passive` | Passive mode | `Ctrl+F7` |
| `key_jitter_only` | Jitter-only debug | `Ctrl+F8` |
| `key_selftest` | Self-test | `Ctrl+F9` |
| `key_snapshot` | NGX input snapshot (menu / truck-preview screen: measuring capture) | `Ctrl+F10` |
| `key_trace` | Frame trace | `Ctrl+F11` |
| `key_jitter_sign` | Cycle NGX jitter sign | `Ctrl+F12` |
| `key_mv_drawids` | Per-draw motion vectors on / off (`mv_objects` 2 ↔ 0) | `Alt+F5` |
| `key_mirror_dlaa` | Mirror DLAA on / off | `Alt+F6` |
| `key_pre_tonemap` | Main-picture DLAA before / after the game's tonemap | `Alt+F7` |
| `key_mv_dump` | Per-draw motion-vector dump of the next frame into the log (diagnostic) | `Alt+F8` |
| `key_perf_profile` | Performance profile high -> medium -> low | `Alt+F9` |
| `key_menu` | Tuning menu open / close | `Delete` |
| `key_menu_up` / `key_menu_down` | Tuning menu: select the row above / below (repeats while held; menu open only) | `Up` / `Down` |
| `key_menu_left` / `key_menu_right` | Tuning menu: change the selected row down / up (repeats on the step rows; menu open only) | `Left` / `Right` |

If two actions get the same combination the log warns and both fire. At most one of the user keys (models,
area, sharpen, width, texture LOD bias, save) acts per frame.

### Wheel, gamepad and joystick buttons

Every action can also be bound to a **button or the D-pad of a wheel / gamepad / joystick**, for example to open and drive
the tuning menu from a Thrustmaster wheel. Put a second binding after a **comma**: `key_menu = Delete, Wheel10` opens the
menu with `Delete` **or** wheel button 10. A value may hold one keyboard binding, one controller binding, or one of each.

- `Wheel1` … `Wheel128` = button 1 … 128 of the controller (DirectInput button index N−1 in the game's data format;
  the same N as the game's `bN` button id in `game.log`).
- `WheelUp`, `WheelDown`, `WheelLeft`, `WheelRight` = the D-pad (the first POV hat). A diagonal counts for both of its
  neighbours (up-right = `WheelUp` and `WheelRight`).
- Controller bindings ignore `Shift` / `Ctrl` / `Alt` (held or not does not matter); `Shift+Wheel3` is not accepted.
- Not case-sensitive, spaces ignored. `none` still turns the whole action off. If one part is not understood the whole
  line is ignored (logged) and the default stays.
- A comma right after a `+` or at the start of a binding is the comma **key** (`Shift+,` still works).

**Finding the button number:** set `log_level = 3` (or 4), start the game and press each button once. The first press of every
button / D-pad direction is written to `dlaa_inject.log`, e.g.
`controller 'Thrustmaster T300RS Racing Wheel': button 10 pressed -- bind it in dlaa.ini as Wheel10`
(at most 24 such lines per run). The game's own `game.log` lists the same numbers as `[di8] Button input 'b10'` for the
device. `WheelN` fires on button N of **every** DirectInput controller the game opens: a T300RS wheel and a shifter both
have a button 1, so pick a number your shifter / button box does not use (or check the log lines, they name the device).

If a shifter or a second controller shares button numbers, set `controller_name = T300` in `dlaa.ini` (part of the
device name as `game.log` / the mod log shows it, not case-sensitive). Then only controllers whose name contains that text
are read; every other controller goes to the game untouched (the log says `DirectInput: controller '...' not tapped`).
Empty (the default) = every controller.

Only controllers the game opens through DirectInput are seen (wheels, most joysticks and button boxes; the log lists each
one as `DirectInput: controller device wrapped`). XInput-only gamepads (Xbox controllers) are not.

While the tuning menu is open the game sees no controller button and no D-pad at all (v0.10.2; before only the 5 menu
bindings were hidden): buttons read as released, the D-pad as centred, so the D-pad does not also change the game's camera
or menu. Steering, pedals and other axes still pass. The press that **opens** the menu does reach the game (the menu is
still closed at that moment), as with `Delete`: bind `key_menu` to a button the game does not use. Releases always reach
the game. You can also bind the buttons in the menu itself: [Bind a key in the menu](#bind-a-key-in-the-menu).

```
key_menu = Delete, Wheel10
key_menu_up = Up, WheelUp
key_menu_down = Down, WheelDown
key_menu_left = Left, WheelLeft
key_menu_right = Right, WheelRight
```
