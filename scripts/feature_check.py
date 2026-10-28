#!/usr/bin/env python3
"""feature_check.py (issue #26) -- after a game session: which mod features were ON, and did each one REALLY RUN?

  python scripts/feature_check.py <dlaa_inject.log> [<dlaa.ini>]      (default dlaa.ini = the one next to the log)

Reads only the game's log (and dlaa.ini for keys the log does not state). The proof comes from what the mod wrote while it
ran -- the counters of the periodic stats lines (every 600 eye blits / 10 s / 30 s), the "first ... drawn" lines, the
errors -- never from reading the code. One row per feature:

  PASS      on, and the log shows it ran
  FAIL      on, but no sign it ran (with log_level high enough to see it), or it failed (an error / a failed counter);
            also: OFF, but it still ran
  OFF       off, and it did not run
  AUTO-OFF  on in dlaa.ini / the menu, but the mod switched it off by itself (the log says why)
  NOT USED  on, but the session never reached the part where it runs (no driving, no mirror in view, no glow, ...)
  N/A       does not apply here (a VR-only feature in flat, a flat-only one in VR, not in this build)
  LOW LOG   the log level is too low to see it run: set log_level = 4 in dlaa.ini and play again
  ?         the log has no line about it (older mod version?)

Exit code 1 when a row is FAIL, else 0. Stdlib only.
"""
import os
import re
import sys

# ------------------------------------------------------------------------------------------------------------------------
# log reading

_LINE = re.compile(r"^\[(\d\d:\d\d:\d\d\.\d+)\] (ERROR |WARN  )?(.*)$")
_HEX = re.compile(r"\b(?:0x)?[0-9A-Fa-f]{8,}\b")
_NUM = re.compile(r"[-+]?\d+(?:\.\d+)?")


class Line:
    __slots__ = ("i", "ts", "lvl", "text")

    def __init__(self, i, ts, lvl, text):
        self.i, self.ts, self.lvl, self.text = i, ts, lvl, text


def num(s, default=None):
    try:
        return float(s)
    except (TypeError, ValueError):
        return default


class Log:
    def __init__(self, text):
        self.lines = []
        for raw in text.splitlines():
            m = _LINE.match(raw)
            if m:
                lvl = {"ERROR ": "E", "WARN  ": "W"}.get(m.group(2) or "", "")
                self.lines.append(Line(len(self.lines), m.group(1), lvl, m.group(3)))
            elif self.lines and raw.strip():                     # a wrapped continuation: part of the line above
                self.lines[-1].text += " " + raw.strip()
        self.end = len(self.lines)
        # the start-up block: everything before the first frame (the config lines are written there)
        self.startup_end = self.end
        for ln in self.lines:
            if re.match(r"Present #0\b", ln.text) or ln.text.startswith("game render context"):
                self.startup_end = ln.i
                break

    def grep(self, rx, start=0, lvl=None):
        rx = re.compile(rx) if isinstance(rx, str) else rx
        out = []
        for ln in self.lines[start:]:
            if lvl and ln.lvl != lvl:
                continue
            m = rx.search(ln.text)
            if m:
                out.append((ln, m))
        return out

    def first(self, rx):
        r = self.grep(rx)
        return r[0] if r else (None, None)

    def last(self, rx):
        r = self.grep(rx)
        return r[-1] if r else (None, None)


def read_ini(path):
    """dlaa.ini as {key: value}; None = no such file."""
    vals = {}
    if not path or not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            s = raw.strip()
            if not s or s[0] in "#;[" or "=" not in s:
                continue
            k, v = s.split("=", 1)
            k = k.strip().lower()
            v = v.split("#")[0].strip() if not k.startswith("key_") else v.strip()
            vals[k] = v
    return vals


# ------------------------------------------------------------------------------------------------------------------------
# session facts

class Session:
    def __init__(self, log, ini):
        self.ini_found = ini is not None
        self.log, self.ini = log, ini or {}
        ln, m = log.first(r"=== ETS2/ATS DLAA injector v(\S+) loaded")
        self.version = m.group(1) if m else None
        ln, m = log.first(r"env: game exe = (\S+)")
        self.game = m.group(1) if m else None
        if not self.game:
            ln, m = log.first(r"caller=(eurotrucks2|amtrucks)\.exe")
            self.game = (m.group(1) + ".exe") if m else None

        # log level: the start level, then every change (the menu's Logging row / LogSetLevel)
        ln, m = log.first(r"logging \(phase 39\): log_level=(\d)")
        if m:
            self.level_start = int(m.group(1))
            self.level_note = ""
        else:
            # before phase 39 the log was written only with debug = 1 = everything (level 4 today)
            self.level_start = 4
            self.level_note = " (no log_level line: older mod version, assumed full log)"
        self.level_events = [(0, self.level_start)]
        for ln, m in log.grep(r"(?:Logging: log_level \d -> (\d)|^log level -> (\d))"):
            self.level_events.append((ln.i, int(m.group(1) or m.group(2))))
        self.level_max = max(l for _, l in self.level_events)
        self.level_min = min(l for _, l in self.level_events)
        self.level_end = self.level_events[-1][1]

        # build flags
        self.no_dlaa_build = bool(log.first(r"DLAA build: not compiled in")[0])

        # VR or flat
        ln, m = log.last(r"^launch mode: (VR|FLAT)")
        self.launch = m.group(1) if m else None
        # the mode the mod ran in: the last line that says so wins (a VR launch can fall back to flat)
        self.mode = self.launch                          # None = not known
        vr_seen = False
        rx = re.compile(r"\bmode=(VR|flat)\b|eye blit detected: eye=\d .*\((VR eye texture|flat backbuffer)|"
                        r"^launch mode (FALLBACK|back to VR)|swapchain blit detected .* -> (eye RT|backbuffer|HDR output)")
        for ln, m in log.grep(rx):
            g = next(x for x in m.groups() if x)
            vr = g in ("VR", "VR eye texture", "back to VR", "eye RT")
            vr_seen = vr_seen or vr
            self.mode = "VR" if vr else "FLAT"
        # an OpenXR session the game really ran (the headset showed the game)
        self.xr = vr_seen or bool(log.first(r"^OpenXR session: xr(Begin|Create)Session\(.*-> 0 = XR_SUCCESS")[0])
        # how much of the world the game drew (world passes / blits)
        blits = [0]
        passes = [0]
        for ln, m in log.grep(r"presents=(\d+) passes=(\d+) blits=(\d+)"):
            passes.append(int(m.group(2)))
            blits.append(int(m.group(3)))
        for ln, m in log.grep(r"^FIFO stats @blit (\d+): passes=(\d+)"):
            blits.append(int(m.group(1)))
            passes.append(int(m.group(2)))
        for ln, m in log.grep(r"^Present #\d+: passes=(\d+) blits=(\d+)"):
            passes.append(int(m.group(1)))
            blits.append(int(m.group(2)))
        self.blits, self.passes = max(blits), max(passes)
        self.ended = bool(log.first(r"=== injector unloading")[0])
        # "the world was drawn" is known when a line that is always written at this level says so
        self.world = self.passes > 0 or self.blits > 0
        self.world_known = self.world or self.ended or self.level_max >= 4
        # where the world frames start (the first stats window starts there, not at the start-up)
        ln, m = log.first(r"^(swapchain blit detected|eye blit detected|pass start: )")
        self.world_start = ln.i if ln else log.startup_end
        # the game drew the 3D world on a context the mod did not take (no pass seen): DLAA had nothing to work on
        ln, m = log.first(r"^G-buffer bind on a non-game context")
        self.world_unseen = "%s %s" % (ln.ts, short(ln.text, 90)) if ln and self.passes == 0 else None

    def level_at(self, idx):
        lvl = self.level_start
        for i, l in self.level_events:
            if i <= idx:
                lvl = l
        return lvl


# ------------------------------------------------------------------------------------------------------------------------
# settings: a timeline per key -- ini, the start-up lines, then the live changes (menu rows, hotkeys, Save, Reset, profiles)

def to_bool(v):
    if v is None:
        return None
    s = str(v).strip().lower()
    if s in ("on", "true", "yes"):
        return True
    if s in ("off", "false", "no", "none"):
        return False
    x = num(s.rstrip("%"))
    if x is None:
        return None
    return x != 0


def kv_value(text, key):
    """The value of `key=value` / `key = value` in a line (None = not there). Prose such as "(needs mv_objects=2 and
    mv_fwd_depth=1)" names a condition, not the value: dropped first."""
    text = re.sub(r"\bneeds? [^;)|]*", "", text)
    m = re.search(r"(?<![\w.])" + re.escape(key) + r" ?= ?(-?[\w.]+)", text)
    return m.group(1) if m else None


class Timeline:
    """[(line index, value, where)] -- index -1 = before the log (dlaa.ini)."""

    def __init__(self):
        self.ev = []

    def add(self, i, v, where):
        if v is None:
            return
        # kept in line order; a value equal to the one before it in time is no change (dlaa.ini's value is always kept)
        k = len(self.ev)
        while k > 0 and self.ev[k - 1][0] > i:
            k -= 1
        if k > 0 and self.ev[k - 1][1] == v and self.ev[k - 1][0] >= 0 and i >= 0:
            return
        self.ev.insert(k, (i, v, where))

    def at(self, i):
        v = None
        for j, x, _ in self.ev:
            if j <= i:
                v = x
        return v

    @property
    def final(self):
        return self.ev[-1][1] if self.ev else None

    @property
    def final_where(self):
        return self.ev[-1][2] if self.ev else None

    def ever(self, pred):
        return any(pred(v) for _, v, _ in self.ev)

    def changes(self):
        return [(i, v, w) for i, v, w in self.ev if i > 0]

    def last_change(self):
        ch = [i for i, _, _ in self.ev if i >= 0]
        return ch[-1] if ch else -1


# the mod's defaults for keys that have no start-up line (LoadConfig's globals in src/inject.cpp)
DEFAULTS = {
    "wire_widen": "0", "menu_vr_layer": "1", "preview_dlaa": "1", "flat_multiview": "1", "fps_vr_lock": "0",
    "cpu_cache_pin": "0", "light_redraw_jitter": "1", "jitter_enabled": "1", "mv_enabled": "1", "tex_aniso": "0",
    "mirror_turns": "0", "reflex": "0", "fps_cap": "0", "vr_fps_cap": "0", "xr_hidden_mask": "0", "xr_turbo": "0",
    "vrs": "0", "vrs_passes": "0", "vrs_eye": "0", "fps_show": "0", "tex_lod_bias": "0",
    "cpu_pcore": "0", "cpu_priority": "0", "cpu_no_throttle": "0",
}

_SKIP_STARTUP = re.compile(r"^(hotkeys:|DirectInput|controller )")
_LIVE_KV = re.compile(r"tuning menu|settings saved to dlaa\.ini|settings RESET to defaults|^frame pace \(phase 52\)")


def key_timeline(sess, key, conv, extra=()):
    """The value of dlaa.ini `key` over the session. `extra` = [(regex, func(match) -> value)] for lines that state the key
    without key=value (hotkeys, toggles)."""
    log = sess.log
    tl = Timeline()
    if key in sess.ini:
        tl.add(-1, conv(sess.ini[key]), "dlaa.ini")
    start = None                                 # the start-up lines: the last one wins (the profile / feature lines come
    for ln in log.lines[:log.startup_end]:       # after the plain dlaa.ini echo)
        if _SKIP_STARTUP.match(ln.text):
            continue
        v = kv_value(ln.text, key)
        if v is not None and conv(v) is not None:
            start = (ln.i, conv(v))
        m = re.search(r"set by the profile: (.*?)(?: \||$)", ln.text)
        if m:
            m2 = re.search(r"(?<![\w])" + re.escape(key) + r" (\S+) -> (\S+?)(?:,|$)", m.group(1))
            if m2 and conv(m2.group(2)) is not None:
                start = (ln.i, conv(m2.group(2)))
    if start:
        tl.add(start[0], start[1], "start")
    rxs = [(re.compile(r), f) for r, f in extra]
    for ln in log.lines[log.startup_end:]:
        hit = False
        for rx, f in rxs:
            m = rx.search(ln.text)
            if m:
                v = f(m)
                if v is not None:
                    tl.add(ln.i, v, "live")
                    hit = True
                    break
        if hit:
            continue
        if _LIVE_KV.search(ln.text):
            v = kv_value(ln.text, key)
            if v is not None and conv(v) is not None:
                tl.add(ln.i, conv(v), "live")
        m = re.search(r"(?:changed|set by the profile): (.*?)(?: \||$)", ln.text)
        if m and ln.text.startswith(("perf profile now", "phase 10 ")):
            m2 = re.search(r"(?<![\w])" + re.escape(key) + r" (\S+) -> (\S+?)(?:,|$)", m.group(1))
            if m2 and conv(m2.group(2)) is not None:
                tl.add(ln.i, conv(m2.group(2)), "profile")
    if not tl.ev and key in DEFAULTS and sess.ini_found:          # no dlaa.ini = the user's choice is not known
        tl.add(-1, conv(DEFAULTS[key]), "default")
    return tl


def on_off(word):
    return word.upper().startswith("ON")


# ------------------------------------------------------------------------------------------------------------------------
# evidence

class Ev:
    """What the log says about one feature.
    samples: [(line index, ran)] -- one per stats window (ran = it did work in that window);
    proof: text for the PASS column; fails / autooff: [text]; notused: why it could not run (None = could);
    need: the log level its proof lines need."""

    def __init__(self, need=4):
        self.samples = []
        self.proof = []
        self.fails = []
        self.autooff = []
        self.autooff_at = []
        self.notused = None
        self.na = None
        self.need = need
        self.found_any = False            # any line of this feature at all (for "?")

    def sample(self, i, ran):
        self.samples.append((i, bool(ran)))
        self.found_any = True


def short(text, n=110):
    text = re.sub(r"\s+", " ", text).strip()
    return text if len(text) <= n else text[:n - 3] + "..."


def errs(log, ev, rx, auto=False, lvl=None):
    for ln, m in log.grep(rx):
        if lvl and ln.lvl not in lvl:
            continue
        (ev.autooff if auto else ev.fails).append("%s %s" % (ln.ts, short(ln.text)))
        if auto:
            ev.autooff_at.append(ln.i)
        ev.found_any = True


def main_eye(e):
    return int(e) < 10                           # 0..7 = main views; 10+ = menu preview units; 20+ = mirror units


# --- the features -------------------------------------------------------------------------------------------------------

def st_dlaa(s):
    """DLAA / DLSS on the main view: on = DLAA on, mode not off, not passive."""
    log = s.log
    tl = Timeline()
    mode_ini = s.ini.get("mode")
    on = True
    if mode_ini is not None:
        on = to_bool(mode_ini) is not False
    tl.add(-1, on, "dlaa.ini" if mode_ini is not None else "default")
    for ln in log.lines:
        t = ln.text
        m = re.match(r"DLAA build: (dlaa_off\.txt found|starts ON)", t)
        if m:
            tl.add(ln.i, m.group(1) == "starts ON", "start")
            continue
        m = re.match(r"mode from dlaa\.ini: (\w+)", t)
        if m:
            tl.add(ln.i, m.group(1).upper() != "OFF", "start")
            continue
        m = re.match(r"mode (DLAA|DLSS|OFF) \(mode key", t)
        if m:
            tl.add(ln.i, m.group(1) != "OFF", "live")
            continue
        m = re.match(r"DLAA (ON|OFF) \(", t)
        if m:
            tl.add(ln.i, m.group(1) == "ON", "live")
            continue
        m = re.match(r"passive mode (ON|OFF) ", t)
        if m:
            tl.add(ln.i, m.group(1) == "OFF", "live")
            continue
        if re.match(r"(DLAA|mode) stays OFF", t):
            tl.add(ln.i, False, "live")
            continue
        if t.startswith("settings RESET to defaults"):
            tl.add(ln.i, True, "live")
    return tl


def ev_dlaa(s, tl):
    log = s.log
    ev = Ev(need=3)
    prev = None
    for ln, m in log.grep(r"^FIFO stats @blit \d+: .*?\| dlaa frames ((?:e\d+=\d+ ?)+)"):
        tot = sum(int(x) for x in re.findall(r"e\d+=(\d+)", m.group(1)))
        ev.sample(ln.i, prev is not None and tot > prev or prev is None and tot > 0)
        prev = tot
    if prev:
        ev.proof.append("%d eye frames anti-aliased (FIFO stats 'dlaa frames')" % prev)
    built = [(ln, m) for ln, m in log.grep(r"^DLAA: eye (\d+) unit built in this Run: .* ok=(\d)") if main_eye(m.group(1))]
    for ln, m in built:
        ev.sample(ln.i, m.group(2) == "1")
        if m.group(2) != "1":
            ev.fails.append("%s %s" % (ln.ts, short(ln.text)))
    if built and not prev:
        ev.proof.append("main DLAA unit built and ran its first frame ('unit built ... ok=1')")
    errs(log, ev, r"^DLAA: (Evaluate failed|EvaluateFeature failed|NGX init (FAILED|failed)|DLSS not available|"
                  r"CreateFeature failed|CreateFeature for preset default (ALSO )?FAILED|NGX is initialised on device .* refused)")
    errs(log, ev, r"^(DLAA|mode) stays OFF")
    if s.world_unseen:
        ev.fails.append("the game drew the 3D world, but the mod never took it: " + s.world_unseen)
    elif s.passes > 0 and s.blits == 0 and s.level_max >= 4:
        ev.fails.append("the mod saw %d world passes but never found the blit of the picture to the screen / eye: DLAA had "
                        "nothing to run on" % s.passes)
    return ev


def st_upscale(s):
    log = s.log
    tl = key_timeline(s, "dlss_upscale", lambda v: to_bool(v), extra=[
        (r"^DLSS upscale (ON|OFF) \(", lambda m: m.group(1) == "ON"),
        (r"^DLSS upscale (ACTIVE|OFF) \(?[^(]*?at blit #", lambda m: m.group(1) == "ACTIVE"),
        (r"^mode (DLAA|DLSS) \(mode key", lambda m: m.group(1) == "DLSS"),
        (r"^settings RESET to defaults", lambda m: True)])
    # the saved mode decides at the start (v0.7.2: mode DLSS = upscale on, DLAA = off)
    ln, m = log.first(r"^mode from dlaa\.ini: (DLAA|DLSS)")
    if m:
        tl.ev = [e for e in tl.ev if e[0] < 0 or e[0] > ln.i]
        tl.add(ln.i, m.group(1) == "DLSS", "start")
        tl.ev.sort(key=lambda e: e[0])
    return tl


def ev_upscale(s, tl):
    log = s.log
    ev = Ev(need=3)
    prev = None
    ups = []
    for ln, m in log.grep(r"^FIFO stats @blit \d+: .*?\| dlaa frames ((?:e\d+=\d+ ?)+).*?\bup=(\S+?)[ \]]"):
        tot = sum(int(x) for x in re.findall(r"e\d+=(\d+)", m.group(1)))
        ran = (prev is not None and tot > prev or prev is None and tot > 0) and m.group(2) != "off"
        ev.sample(ln.i, ran)
        prev = tot
        if ran:
            ups.append(m.group(2))
    if ups:
        ev.proof.append("DLSS upscaled %s (FIFO stats 'up=')" % ups[-1])
    for ln, m in log.grep(r"^DLSS upscale ACTIVE at blit #\d+: render \(scene\) (\d+x\d+) -> output (\d+x\d+)"):
        ev.sample(ln.i, True)
        if not ups:
            ev.proof.append("'DLSS upscale ACTIVE' %s -> %s (level 3 proof)" % (m.group(1), m.group(2)))
            ups.append("x")
    for ln, m in log.grep(r"^DLAA: NGX feature created for eye (\d+) -- DLSS (\w+) upscale: render (\d+x\d+)"):
        if main_eye(m.group(1)):
            ev.sample(ln.i, True)
            if not ups:
                ev.proof.append("DLSS %s upscale feature made, render %s" % (m.group(2), m.group(3)))
                ups.append("x")
    r = log.grep(r"^DLSS upscale not applicable \((.*?)\)")
    if r and not ups:
        ev.notused = "DLSS upscale not applicable: " + r[-1][1].group(1) + " (the game renders at the output size)"
    errs(log, ev, r"^DLAA: CreateFeature failed .*->")
    return ev


def st_sharpen(s):
    conv = lambda v: num(v)
    tl = key_timeline(s, "sharpness", conv)
    # the live value of every stats window (Shift+F7 / F8 change it without a key=value line)
    for ln, m in s.log.grep(r"\[profile=\S+ preset=\S+ sharp=([\d.]+)"):
        tl.add(ln.i, num(m.group(1)), "live")
    tl.ev.sort(key=lambda e: e[0])
    return tl


def ev_sharpen(s, tl):
    log = s.log
    ev = Ev(need=4)
    for ln, m in log.grep(r"^DLAA GPU cost eye (\d+): .*?\| sharpen ([\d.]+).*?\[preset=\S+ sharp=([\d.]+) r=[\d.]+ area=(\d+) up=(\S+)"):
        if main_eye(m.group(1)):
            # strength 0 + a DLAA area below 100 % (no upscale): the pass runs as the area border blend only (scene_dlaa.cpp)
            blend_only = num(m.group(3)) == 0 and int(m.group(4)) < 100 and m.group(5) == "off"
            if blend_only:
                continue
            ev.sample(ln.i, num(m.group(2)) > 0)
            if num(m.group(2)) > 0 and not ev.proof:
                ev.proof.append("sharpen pass costs %s ms on the main view (DLAA GPU cost lines)" % m.group(2))
    for ln, m in log.grep(r"^perf eye (\d+) @blit \d+: .* rcas ([\d.]+)/"):
        if not num(tl.at(ln.i), 0):               # no area / strength on this line: proof only while the strength is > 0
            continue
        ev.sample(ln.i, num(m.group(2)) > 0)
        if num(m.group(2)) > 0 and not ev.proof:
            ev.proof.append("RCAS sharpen timed at %s ms per pass (perf eye lines)" % m.group(2))
    if not ev.samples:                           # level 3: the unit was made with a strength > 0 and ran its first frame
        r = [(ln, m) for ln, m in log.grep(r"^DLAA: sharpen \(RCAS\) ready for eye (\d+) -- strength ([\d.]+)")
             if main_eye(m.group(1))]
        ok = log.grep(r"^DLAA: eye (\d) unit built in this Run: .* ok=1")
        if r and ok:
            ev.need = 3
            v = num(r[-1][1].group(2))
            ev.sample(ok[-1][0].i, v > 0)
            if v > 0:
                ev.proof.append("sharpen made at strength %.2f and the unit ran (level 3 proof: first frame only)" % v)
    errs(log, ev, r"^DLAA: sharpen setup failed|^DLAA: RCAS .*(failed|hr=)")
    return ev


def st_pretonemap(s):
    return key_timeline(s, "dlaa_pre_tonemap", to_bool, extra=[
        (r"^DLAA pre-tonemap (ON|OFF) \(", lambda m: m.group(1) == "ON"),
        (r"^phase 4 \(v0\.10\.0\): dlaa_pre_tonemap=(\d)", lambda m: m.group(1) == "1")])


def ev_pretonemap(s, tl):
    log = s.log
    ev = Ev(need=4)
    if s.mode == "VR":
        ev.na = "VR: the eye pictures always use the blit path (flat only)"
    runs_total = fails_total = 0
    for ln, m in log.grep(r"stage=(pre-tonemap|post-tonemap) \(pre-tonemap runs (\d+) \(at the forward leave \d+\), "
                          r"deferred \d+, failed (\d+)"):
        runs, fails = int(m.group(2)), int(m.group(3))
        ev.sample(ln.i, runs > 0)
        runs_total += runs
        fails_total += fails
    if runs_total:
        ev.proof.append("%d passes anti-aliased before the tone-map (FIFO stats 'pre-tonemap runs')" % runs_total)
    if fails_total and not runs_total:
        ev.fails.append("'failed %d' in the pre-tonemap counters, 0 runs" % fails_total)
    elif fails_total:
        ev.proof.append("(but %d failed runs)" % fails_total)
    pre = log.grep(r"^DLAA stage: pre-tonemap \(HDR")
    post = log.grep(r"^DLAA stage: post-tonemap \(blit\) -- (.*?) \(Present")
    if pre and not ev.samples:
        ev.need = 3
        ev.sample(pre[0][0].i, True)
        ev.proof.append("'DLAA stage: pre-tonemap' line (level 3 proof)")
    if post and not runs_total and not pre:
        why = post[-1][1].group(1)
        if s.mode != "VR":
            ev.notused = "the mod chose the blit path: " + short(why, 90)
        ev.found_any = True
    errs(log, ev, r"^DLAA pre-tonemap: WARNING the HDR unit")
    return ev


def st_mv(s):
    return key_timeline(s, "mv_enabled", to_bool, extra=[
        (r"^motion vectors (ON|OFF) ", lambda m: m.group(1) == "ON")])


def ev_mv(s, tl):
    log = s.log
    ev = Ev(need=4)
    # proof only while on: with mv_enabled off (Ctrl+F5) the MV target is still cleared to 0 (the 'mv' span keeps a cost) and
    # the candidates are still collected, so these lines cannot show "off but ran"
    for ln, m in log.grep(r"^DLAA GPU cost eye (\d+): .*?\| mv ([\d.]+)"):
        if main_eye(m.group(1)) and tl.at(ln.i) is not False:
            ev.sample(ln.i, num(m.group(2)) > 0)
    for ln, m in log.grep(r"^MV: frame \d+ pairs this frame world=(\d+) cabin=(\d+)"):
        if tl.at(ln.i) is not False:
            ev.sample(ln.i, int(m.group(1)) + int(m.group(2)) > 0)
    n = sum(1 for _, r in ev.samples if r)
    if n:
        ev.proof.append("motion vectors made in %d stats lines (MV pass GPU time / camera pairs)" % n)
    if not ev.samples:
        r = log.grep(r"^MV: ready -- ")
        if r:
            ev.need = 3
            ev.sample(r[0][0].i, True)
            ev.proof.append("'MV: ready' line")
    errs(log, ev, r"^DLAA: MV create hr=|^MV: .*(FAILED|failed hr)")
    return ev


def st_drawids(s):
    return key_timeline(s, "mv_objects", lambda v: None if num(v) is None else num(v) == 2, extra=[
        (r"^per-draw motion vectors (ON|OFF) \(", lambda m: m.group(1) == "ON")])


def ev_drawids(s, tl):
    log = s.log
    ev = Ev(need=4)
    best = None
    for ln, m in log.grep(r"^MV draw-ids @blit \d+ \| (\w+).*?replay: ([\d.]+) draws/pass.*?paired ([\d.]+) % of ([\d.]+) draws/frame"):
        ran = m.group(1) == "ACTIVE" and num(m.group(2)) > 0
        ev.sample(ln.i, ran)
        if ran:
            best = (m.group(2), m.group(3))
    for ln, m in log.grep(r"^MV draw-ids @blit \d+ \| (\w+)"):
        if m.group(1) != "ACTIVE":
            ev.sample(ln.i, False)
    if best:
        ev.proof.append("%s draws replayed per pass, %s %% paired (MV draw-ids lines)" % best)
    errs(log, ev, r"^(mirror )?draw-ids: draw-id target create hr=|^MV draw-ids: draw-id target create")
    return ev


def st_fwd(s):
    return key_timeline(s, "mv_fwd_depth", to_bool, extra=[
        (r"^MV forward depth (ON|OFF) \(", lambda m: m.group(1) == "ON")])


def ev_fwd(s, tl):
    log = s.log
    ev = Ev(need=4)
    total = 0.0
    for ln, m in log.grep(r"^MV forward depth batch \(v0\.10\.0 phase 10\) over (\d+) passes: ([\d.]+) draws re-drawn per pass"):
        ev.sample(ln.i, int(m.group(1)) > 0 and num(m.group(2)) > 0)
        if int(m.group(1)) > 0:
            total = max(total, num(m.group(2)))
    if not ev.samples:                           # before phase 10: the per-pass line
        for ln, m in log.grep(r"^MV forward depth: per pass over (\d+) passes -- forward draws [\d.]+, recorded for the batch ([\d.]+)"):
            ev.sample(ln.i, int(m.group(1)) > 0 and num(m.group(2)) > 0)
            total = max(total, num(m.group(2)))
    # the GPU time of the re-draw (the 'perf view' "forward depth at 1/2 resolution in N of M passes" is only the resolution
    # choice: it keeps counting after the mod switched the forward depth off)
    for ln, m in log.grep(r"^perf eye \d+ @blit \d+: .*? fwd-depth ([\d.]+)/([\d.]+)"):
        ev.sample(ln.i, num(m.group(1)) > 0)
        if num(m.group(1)) > 0 and not total:
            total = -1
            ev.proof.append("forward-depth re-draw timed on the GPU (%s ms avg, perf eye lines)" % m.group(1))
    if total > 0:
        ev.proof.append("up to %.1f see-through draws re-drawn into the forward depth per pass" % total)
    r = log.grep(r"^MV forward depth: first (BATCH|re-draw)")
    if r and not ev.samples:
        ev.need = 3
        ev.sample(r[0][0].i, True)
        ev.proof.append("'MV forward depth: first %s' line (level 3 proof)" % r[0][1].group(1))
    errs(log, ev, r"^MV forward depth: OFF for this session", auto=True, lvl=("W",))
    errs(log, ev, r"^MV forward depth: OFF for this session|^MV forward depth: target create hr=", lvl=("E",))
    return ev


def st_mirror(s):
    vr = s.mode == "VR"
    on = key_timeline(s, "mirror_dlaa", to_bool, extra=[
        (r"^mirror DLAA (ON|OFF) \(", lambda m: m.group(1) == "ON"),
        (r"^mirror DLAA now (\w+)", lambda m: m.group(1).lower() not in ("off", "game")),
        (r"^mirror units \(v0\.10\.0 phase 2\): mirror_dlaa=(\d)", lambda m: m.group(1) == "1")])
    mk = "mirror_vr_mode" if vr else "mirror_flat_mode"
    mode = key_timeline(s, mk, lambda v: num(v))
    tl = Timeline()
    idx = sorted(set([e[0] for e in on.ev] + [e[0] for e in mode.ev]))
    for i in idx:
        a, b = on.at(i), mode.at(i)
        tl.add(i, None if a is None else (a and (b is None or b != 0)), "start" if i <= s.log.startup_end else "live")
    # the live state of each stats window
    for ln, m in s.log.grep(r"^mirror units @blit \d+: (ACTIVE|inactive|OFF)"):
        tl.add(ln.i, m.group(1) != "OFF", "live")
    tl.ev.sort(key=lambda e: e[0])
    return tl


def ev_mirror(s, tl):
    log = s.log
    ev = Ev(need=4)
    ok_total = fail_total = 0
    any_views = False
    for ln, m in log.grep(r"^mirror units @blit \d+: (\w+).*?\(frames with mirror views (\d+) of (\d+)\)"):
        oks = sum(int(x) for x in re.findall(r"dlaa ok (\d+)", ln.text))
        fls = sum(int(x) for x in re.findall(r"dlaa ok \d+ fail (\d+)", ln.text))
        ev.sample(ln.i, oks > 0)
        ok_total += oks
        fail_total += fls
        if int(m.group(2)) > 0:
            any_views = True
    if ok_total:
        ev.proof.append("%d mirror views anti-aliased (mirror units lines 'dlaa ok')" % ok_total)
    if fail_total:
        ev.fails.append("%d mirror DLAA runs failed ('fail N' in the mirror units lines)" % fail_total)
    if ev.samples and not any_views:
        ev.notused = "no mirror view was drawn in this session"
    r = log.grep(r"^FIFO stats @blit \d+: .*mirror views handled=(\d+) not handled=(\d+)")
    if r and not ev.samples:
        h = max(int(m.group(1)) for _, m in r)
        ev.sample(r[-1][0].i, h > 0)
        if h:
            ev.proof.append("%d mirror views handled (FIFO stats)" % h)
    errs(log, ev, r"^mirror DLAA: (unit \d+ .*Run failed|WARNING unit \d+ could not be built|WARNING unit \d+ .*failed \d+ times)")
    return ev


def st_turns(s):
    return key_timeline(s, "mirror_turns", to_bool, extra=[
        (r"^Mirrors in turns: (ON|OFF)", lambda m: m.group(1) == "ON"),
        (r"^phase 41: mirror_turns=(\d)", lambda m: m.group(1) == "1")])


def ev_turns(s, tl):
    log = s.log
    ev = Ev(need=4)
    held = 0
    for ln, m in log.grep(r"^mirror units @blit \d+: "):
        h = sum(int(x) for x in re.findall(r"\{turns unit \d+: slot \d+, ran \d+ on its turn \+ \d+ forced, held (\d+)", ln.text))
        if "{turns unit" in ln.text:
            ev.sample(ln.i, h > 0)
        held += h
    for ln, m in log.grep(r"^mirror turns @Present \d+: (\d+) ran, (\d+) held"):
        ev.sample(ln.i, int(m.group(2)) > 0)
        held += int(m.group(2))
    if held:
        ev.proof.append("%d mirror frames shown from the last DLAA picture (held on the off turn)" % held)
    return ev


def st_lod(s):
    """On = the EFFECTIVE bias is not 0. tex_lod_bias_cutout alone does nothing: the cut-out bias is 0 while the main bias is 0
    (LodCutEffective, phase 13)."""
    tl = key_timeline(s, "tex_lod_bias", lambda v: None if num(v) is None else num(v) != 0)
    for ln, m in s.log.grep(r"^texture LOD bias (?:now )?(-?[\d.]+) \(|^texture LOD bias: auto term now [-+\d.]+ .*?-> effective (-?[\d.]+)"):
        tl.add(ln.i, num(m.group(1) or m.group(2)) != 0, "live")
    for ln, m in s.log.grep(r"\[profile=\S+ .*? lod=(off|-?[\d.]+)"):
        tl.add(ln.i, m.group(1) != "off" and num(m.group(1)) != 0, "live")
    tl.ev.sort(key=lambda e: e[0])
    return tl


def ev_lod(s, tl):
    log = s.log
    ev = Ev(need=4)
    best = 0
    for ln, m in log.grep(r"subst/frame=solid (\d+) \(full set: bias (-?[\d.]+) aniso \d+\) / alpha-tested (\d+) \(-> [\w-]+ set: bias (-?[\d.]+)"):
        n = (int(m.group(1)) if num(m.group(2)) != 0 else 0) + (int(m.group(3)) if num(m.group(4)) != 0 else 0)
        ev.sample(ln.i, n > 0)
        best = max(best, n)
    for ln, m in log.grep(r" lod=(-?[\d.]+)(?:\(idle\))? aniso=\d+ .*?subst/frame=(\d+) "):   # v0.9.0 .. phase 10: one counter
        n = int(m.group(2)) if num(m.group(1)) != 0 else 0
        ev.sample(ln.i, n > 0)
        best = max(best, n)
    if best:
        ev.proof.append("up to %d texture samplers per frame swapped for the biased ones" % best)
    errs(log, ev, r"^texture LOD bias: CreateSamplerState .* failed")
    return ev


def st_aniso(s):
    return key_timeline(s, "tex_aniso", lambda v: None if num(v) is None else num(v) > 0)


def ev_aniso(s, tl):
    log = s.log
    ev = Ev(need=4)
    best = 0
    for ln, m in log.grep(r"subst/frame=solid (\d+) \(full set: bias -?[\d.]+ aniso (\d+)\)"):
        n = int(m.group(1)) if int(m.group(2)) > 0 else 0
        ev.sample(ln.i, n > 0)
        best = max(best, n)
    for ln, m in log.grep(r" lod=-?[\d.]+(?:\(idle\))? aniso=(\d+) .*?subst/frame=(\d+) "):   # v0.9.0 .. phase 10: one counter
        n = int(m.group(2)) if int(m.group(1)) >= 2 else 0
        ev.sample(ln.i, n > 0)
        best = max(best, n)
    if best:
        ev.proof.append("up to %d samplers per frame use the anisotropic set" % best)
    return ev


def st_lightjit(s):
    return key_timeline(s, "light_redraw_jitter", to_bool)


def ev_lightjit(s, tl):
    log = s.log
    ev = Ev(need=4)
    tot = 0
    noshift = 0
    for ln, m in log.grep(r"^lighting redraws \(phase 55, light_redraw_jitter (\d)\) @blit \d+: (\d+) shifted redraws in (\d+) "
                          r"lighting binds.*?redraws NOT shifted \(viewport not the pass size\) (\d+)"):
        ev.sample(ln.i, int(m.group(2)) > 0)
        tot += int(m.group(2))
        noshift += int(m.group(4))
    if tot:
        ev.proof.append("%d lighting redraws shifted with the jitter" % tot)
    elif ev.samples and not noshift:
        ev.notused = "the lighting pass drew no light-mesh redraw to shift (0 shifted, 0 missed: few lights, daytime?)"
    elif noshift:
        ev.fails.append("%d lighting redraws NOT shifted (viewport not the pass size), 0 shifted" % noshift)
    return ev


def st_jitter(s):
    return key_timeline(s, "jitter_enabled", to_bool, extra=[
        (r"^jitter-only debug (ON|OFF)", lambda m: None)])


def ev_jitter(s, tl):
    log = s.log
    ev = Ev(need=4)
    mism = passes = 0
    for ln, m in log.grep(r"^jitter check @blit \d+: (\d+) of (\d+) passes drew their first G-buffer draw with a different"):
        a, b = int(m.group(1)), int(m.group(2))
        mism, passes = a, b                         # cumulative counters: the last line has the totals
        if tl.at(ln.i) is not False:                # (passes are counted with jitter off too: proof only while on)
            ev.sample(ln.i, b > 0)
    jit = [ln for ln, m in log.grep(r"^pass start: s=\d+ phase=\d+ jitter=\((-?[\d.]+),(-?[\d.]+)\)")
           if num(m.group(1)) != 0 or num(m.group(2)) != 0]
    jit += [ln for ln, m in log.grep(r"^DLAA eval: eye=\d+ .*?NGX jitter=\((-?[\d.]+),(-?[\d.]+)\)")
            if num(m.group(1)) != 0 or num(m.group(2)) != 0]
    for ln in jit[:1]:
        ev.sample(ln.i, True)
        ev.need = 4
    if passes:
        ev.proof.append("%d passes jittered, %d drawn with the wrong shift (expect 0)" % (passes, mism))
        if mism > max(2, passes // 100):
            ev.fails.append("%d of %d passes drew with another jitter than DLSS was told" % (mism, passes))
    elif jit:
        ev.proof.append("'pass start ... jitter=' lines with a non-zero shift")
    return ev


def st_wire(s):
    tl = key_timeline(s, "wire_widen", to_bool, extra=[
        (r"^wire widening (ON|OFF) \(", lambda m: m.group(1) == "ON")])
    for ln, m in s.log.grep(r"^wire widening \(phase 61, (on|off), "):
        tl.add(ln.i, m.group(1) == "on", "live")
    tl.ev.sort(key=lambda e: e[0])
    return tl


def ev_wire(s, tl):
    log = s.log
    ev = Ev(need=4)
    best = 0.0
    for ln, m in log.grep(r"^wire widening \(phase 61, (?:on|off), wire_min_px [\d.]+\) over (\d+) passes: ([\d.]+) wire draws widened per pass \(.*?\) \+ ([\d.]+) solid"):
        v = num(m.group(2)) + num(m.group(3))
        ev.sample(ln.i, int(m.group(1)) > 0 and v > 0)
        best = max(best, num(m.group(2)))
    if best:
        ev.proof.append("up to %.1f wire draws widened per pass" % best)
    r = log.grep(r"^wire widening \(phase 6[13]\): first (wire|solid thin) draw")
    if r and not ev.samples:
        ev.need = 3
        ev.sample(r[0][0].i, True)
        ev.proof.append("'wire widening: first wire draw widened' line (level 3 proof)")
    errs(log, ev, r"^wire widening: .*(failed|hr=0x)")
    errs(log, ev, r"^wire widening \(phase 61\): no wire draw widened yet", auto=True)
    return ev


def st_bias(s):
    return key_timeline(s, "dlss_bias_mask", lambda v: None if num(v) is None else num(v) > 0, extra=[
        (r"^Light glow ghosting fix \(phase 49\): dlss_bias_mask=(\d+)", lambda m: int(m.group(1)) > 0),
        (r"^DLSS bias mask \(phase 49\): dlss_bias_mask=(\d+)", lambda m: int(m.group(1)) > 0)])


def ev_bias(s, tl):
    log = s.log
    ev = Ev(need=4)
    glow = masks = 0
    any_glow = False
    for ln, m in log.grep(r"^DLSS bias mask \(phase 49\) over (\d+) passes: glow draws per pass ([\d.]+) .*?masks (\d+), failed (\d+)"):
        ev.sample(ln.i, int(m.group(3)) > 0)
        masks = max(masks, int(m.group(3)))
        if num(m.group(2)) > 0:
            any_glow = True
        if int(m.group(4)) > 0:
            ev.fails.append("%s %d glow masks failed" % (ln.ts, int(m.group(4))))
    if masks:
        ev.proof.append("%d glow masks given to DLSS" % masks)
    r = log.grep(r"^DLSS bias mask \(phase 49\): first pass with light glows")
    if r and not ev.samples:
        ev.need = 3
        ev.sample(r[0][0].i, True)
        ev.proof.append("'first pass with light glows' line (level 3 proof)")
    if ev.samples and not any_glow and not masks:
        ev.notused = "no light glow was drawn (daytime?)"
    errs(log, ev, r"^DLSS bias mask: (OFF for this session|.*no glow bias)")
    return ev


def st_vrs(s):
    return key_timeline(s, "vrs", to_bool, extra=[
        (r"^Foveated rendering \(VRS\): (ON|OFF)", lambda m: m.group(1) == "ON")])


def ev_vrs(s, tl):
    log = s.log
    ev = Ev(need=4)
    if s.mode == "FLAT":
        ev.na = "VR only"
    tot = 0
    for ln, m in log.grep(r"^VRS @\d+ s \(Present #\d+\): (\d+) scene binds entered .*?draws coarse-capable (\d+)"):
        ev.sample(ln.i, int(m.group(2)) > 0)
        tot += int(m.group(2))
    if tot:
        ev.proof.append("%d draws shaded at the coarse rate (VRS lines)" % tot)
    r = log.grep(r"^VRS: mask built for eye \d")
    if r and not ev.samples:
        ev.need = 3
        ev.sample(r[0][0].i, True)
        ev.proof.append("'VRS: mask built' line (level 3 proof)")
    errs(log, ev, r"^VRS( \(foveated rendering\))?: .*(FAILED|NOT available)")
    return ev


def st_vrspasses(s):
    return key_timeline(s, "vrs_passes", to_bool)


def ev_vrspasses(s, tl):
    log = s.log
    ev = Ev(need=4)
    if s.mode == "FLAT":
        ev.na = "VR only"
    best = 0.0
    for ln, m in log.grep(r"^VRS passes @\d+ s \(vrs_passes=\d.*?\| lighting [\d.]+ / ([\d.]+) / "):
        ev.sample(ln.i, num(m.group(1)) > 0)
        best = max(best, num(m.group(1)))
    if best:
        ev.proof.append("up to %.0f lighting draws per frame at the coarse rate" % best)
    return ev


def st_eye(s):
    return key_timeline(s, "vrs_eye", to_bool, extra=[
        (r"^Foveated eye tracking \(VRS, phase 59\): (ON|OFF)", lambda m: m.group(1) == "ON")])


def ev_eye(s, tl):
    log = s.log
    ev = Ev(need=3)
    if s.mode == "FLAT":
        ev.na = "VR only"
    valid = 0
    real = False
    for ln, m in log.grep(r"^Eye tracking @\d+ s: source ([^;]+);.*?gaze locates \+(\d+): valid \+(\d+)"):
        v = int(m.group(3))
        ev.sample(ln.i, v > 0)
        valid += v
        real = real or m.group(1).startswith(("real", "FAKE"))
    r = log.grep(r"^Eye tracking: FIRST VALID GAZE")
    if r:
        ev.sample(r[0][0].i, True)
    if valid:
        ev.proof.append("%d valid gazes read from the headset" % valid)
    elif r:
        ev.proof.append("'FIRST VALID GAZE' line")
    if not valid and not r:
        if log.grep(r"^Eye tracking: the OpenXR runtime \S+ does not (LIST|list)"):
            ev.notused = "the headset / runtime offers no eye tracking"
    errs(log, ev, r"^Eye tracking: .*FAILED", lvl=("E",))
    return ev


def st_hmask(s):
    return key_timeline(s, "xr_hidden_mask", to_bool, extra=[
        (r"^Hidden area mask \(VR\): (ON|OFF)", lambda m: m.group(1) == "ON")])


def ev_hmask(s, tl):
    log = s.log
    ev = Ev(need=4)
    if s.mode == "FLAT":
        ev.na = "VR only"
    tot = 0
    for ln, m in log.grep(r"^hidden area mask @\d+ s: masks drawn eye 0 (\d+) / eye 1 (\d+)"):
        n = int(m.group(1)) + int(m.group(2))
        ev.sample(ln.i, n > 0)
        tot += n
    if tot:
        ev.proof.append("%d masks drawn (hidden area mask lines)" % tot)
    r = log.grep(r"^hidden area mask: FIRST mask drawn")
    if r and not ev.samples:
        ev.need = 3
        ev.sample(r[0][0].i, True)
        ev.proof.append("'FIRST mask drawn' line (level 3 proof)")
    if not tot and not r and log.grep(r"^hidden area mask: (view \d -- the runtime reports NO hidden area|the OpenXR runtime .* does not offer)"):
        ev.notused = "the runtime has no hidden area for this headset"
    errs(log, ev, r"^hidden area mask: ", lvl=("E",))
    return ev


def st_turbo(s):
    return key_timeline(s, "xr_turbo", to_bool, extra=[
        (r"^Turbo mode: (ON|OFF) \(", lambda m: m.group(1) == "ON"),
        (r"^Turbo mode \(phase 30, from OpenXR Toolkit\): xr_turbo=(\d)", lambda m: m.group(1) == "1")])


def ev_turbo(s, tl):
    log = s.log
    ev = Ev(need=4)
    if s.mode == "FLAT":
        ev.na = "VR only"
    tot = 0
    for ln, m in log.grep(r"^OpenXR turbo stats \(([^)]*)\): (\d+) async xrWaitFrame .*?(\d+) failures"):
        n = int(m.group(2))
        ev.sample(ln.i, n > 0)
        tot += n
        if int(m.group(3)) > 0:
            ev.fails.append("%s %s failures in the turbo stats" % (ln.ts, m.group(3)))
    if tot:
        ev.proof.append("%d frames with the async xrWaitFrame (turbo stats)" % tot)
    r = log.grep(r"^OpenXR turbo: (first async xrWaitFrame #\d+ armed|ON at)")
    if r and not ev.samples:
        ev.need = 3
        ev.sample(r[-1][0].i, True)
        ev.proof.append("'OpenXR turbo: %s' line (level 3 proof)" % short(r[-1][1].group(1), 40))
    errs(log, ev, r"^OpenXR turbo: .*(FAILED|could not be created|not resolved)", lvl=("E",))
    errs(log, ev, r"NOT running: the VR FPS cap is on", auto=True)
    return ev


def st_reflex(s):
    return key_timeline(s, "reflex", lambda v: None if num(v) is None else num(v) > 0, extra=[
        (r"^Reflex: (on \+ boost|on|off) \(reflex=", lambda m: m.group(1) != "off")])


def ev_reflex(s, tl):
    log = s.log
    ev = Ev(need=3)
    if s.mode == "VR":
        ev.na = "flat only (VR: the headset paces the game)"
    for ln, m in log.grep(r"^frame pace \(phase 52\): reflex=(\d) fps_cap=\d+ -> (Reflex (ON|ON \+ boost): NvAPI_D3D_SetSleepMode status 0|Reflex requested but not available|Reflex off)"):
        ev.sample(ln.i, m.group(2).startswith("Reflex ON"))
        if m.group(2).startswith("Reflex ON"):
            ev.proof = ["the driver accepted Reflex (NvAPI_D3D_SetSleepMode OK)"]
    r = [m for ln, m in log.grep(r"\| frame pace \(phase 52\): reflex=\d \((running|not running)\).*?Reflex Sleep ([\d.]+) ms/frame")]
    for ln, m in log.grep(r"\| frame pace \(phase 52\): reflex=\d \((running|not running)\).*?Reflex Sleep ([\d.]+) ms/frame"):
        ev.sample(ln.i, m.group(1) == "running")
    if r and r[-1].group(1) == "running":
        ev.proof.append("'reflex=1 (running)' in the CPU prof lines")
    errs(log, ev, r"^Reflex: NOT available")
    return ev


def st_fpscap(s):
    return key_timeline(s, "fps_cap", lambda v: None if num(v) is None else num(v) > 0, extra=[
        (r"^FPS cap: (\d+) fps.*?\(fps_cap=(\d+)", lambda m: int(m.group(2)) > 0)])


def ev_fpscap(s, tl):
    log = s.log
    ev = Ev(need=4)
    if s.mode == "VR":
        ev.na = "flat only (VR has its own cap)"
    best = 0.0
    for ln, m in log.grep(r"\| frame pace \(phase 52\): reflex=\d[^;]*?fps_cap=(\d+) -> limiter sleep ([\d.]+) ms/frame, Reflex Sleep ([\d.]+) ms/frame \((\d+) flat frames\)"):
        ran = int(m.group(1)) > 0 and int(m.group(4)) > 0 and (num(m.group(2)) > 0 or num(m.group(3)) > 0)
        ev.sample(ln.i, ran)
        if ran:
            best = max(best, num(m.group(2)) + num(m.group(3)))
    if best:
        ev.proof.append("up to %.2f ms per frame slept by the cap (CPU prof lines)" % best)
    for ln, m in log.grep(r"^frame pace \(phase 52\): reflex=\d fps_cap=(\d+) -> .*?fps cap (by the driver|by the own limiter)"):
        if not ev.samples or ev.need == 3:
            ev.need = 3
            ev.sample(ln.i, True)
            ev.proof = ["fps cap set up %s (level 3: set-up only)" % m.group(1 + 1)]
    return ev


def st_vrcap(s):
    return key_timeline(s, "vr_fps_cap", lambda v: None if num(v) is None else num(v) > 0, extra=[
        (r"^VR FPS cap: .*?\(vr_fps_cap=(\d)", lambda m: int(m.group(1)) > 0)])


def ev_vrcap(s, tl):
    log = s.log
    ev = Ev(need=4)
    if s.mode == "FLAT":
        ev.na = "VR only"
    tot = 0
    for ln, m in log.grep(r"vr_fps_cap=(\d) -> ([\d.]+) ms/frame slept before xrWaitFrame \((\d+) of (\d+) waits delayed"):
        ev.sample(ln.i, int(m.group(3)) > 0)
        tot += int(m.group(3))
    if tot:
        ev.proof.append("%d xrWaitFrame calls held back by the cap" % tot)
    return ev


def st_cpu(s):
    tl = Timeline()
    keys = [key_timeline(s, k, to_bool) for k in ("cpu_pcore", "cpu_priority", "cpu_no_throttle")]
    for i in sorted(set(e[0] for t in keys for e in t.ev)):
        v = [t.at(i) for t in keys]
        if all(x is None for x in v):
            continue
        tl.add(i, any(bool(x) for x in v), "start" if i <= s.log.startup_end else "live")
    for ln, m in s.log.grep(r"^CPU tuning: (ON|OFF) \("):
        tl.add(ln.i, m.group(1) == "ON", "live")
    tl.ev.sort(key=lambda e: e[0])
    return tl


def ev_cpu(s, tl):
    log = s.log
    ev = Ev(need=3)
    for ln, m in log.grep(r"^cpu_pcore \(phase 54\): (\d+) P-core logical processors|^cpu_priority \(phase 54\): (?!off)|"
                          r"^cpu_no_throttle \(phase 54\): the render|^CPU tuning: ON \("):
        ev.sample(ln.i, True)
        if not ev.proof:
            ev.proof.append(short(ln.text, 90))
    for ln, m in log.grep(r"^cpu tuning \(phase 54\): off|^CPU tuning: OFF \("):
        ev.sample(ln.i, False)
    for ln, m in log.grep(r"^cpu_pcore \(phase 54\): nothing will be done -- (.*)"):
        ev.notused = "cpu_pcore: " + short(m.group(1), 80)
    return ev


def st_pin(s):
    tl = key_timeline(s, "cpu_cache_pin", to_bool)
    for ln, m in s.log.grep(r"^cpu_cache_pin \(phase 32\): (?!off)"):
        tl.add(ln.i, True, "start")
        break
    tl.ev.sort(key=lambda e: e[0])
    return tl


def ev_pin(s, tl):
    log = s.log
    ev = Ev(need=3)
    for ln, m in log.grep(r"^cpu_cache_pin \(phase 32\): process affinity (0x[0-9a-f]+) -> (0x[0-9a-f]+)"):
        ev.sample(ln.i, m.group(1) != m.group(2))
        ev.proof.append("affinity %s -> %s" % (m.group(1), m.group(2)))
    for ln, m in log.grep(r"^cpu_cache_pin \(phase 32\): (?!L3 |process affinity)(.*)"):
        ev.found_any = True
        if not ev.samples:
            ev.notused = short(m.group(1), 90)
    return ev


def st_fpsbox(s):
    return key_timeline(s, "fps_show", to_bool, extra=[
        (r"^fps box (ON|OFF) \(tuning menu", lambda m: m.group(1) == "ON")])


def ev_fpsbox(s, tl):
    log = s.log
    ev = Ev(need=4)
    tot = 0
    # proof only while on: with fps_show = 0 the box is still drawn while the tuning menu is open
    for ln, m in log.grep(r"^perf @blit \d+: fps-box [\d.]+ avg / [\d.]+ max ms per frame \((\d+) of (\d+) timed frames"):
        if tl.at(ln.i) is not False:
            ev.sample(ln.i, int(m.group(1)) > 0)
            tot += int(m.group(1))
    for ln, m in log.grep(r"^tuning menu: fps box GPU cost [\d.]+ ms per frame \(GpuPerf fps-box: (\d+) of"):
        if tl.at(ln.i) is not False:
            ev.sample(ln.i, int(m.group(1)) > 0)
            tot += int(m.group(1))
    if tot:
        ev.proof.append("drawn on %d timed frames (perf fps-box lines)" % tot)
    r = log.grep(r"^OpenXR overlay: fps box quad")
    if r and not tot:
        ev.sample(r[0][0].i, True)
        ev.proof.append("'OpenXR overlay: fps box quad' lines")
    return ev


def st_fpslock(s):
    return key_timeline(s, "fps_vr_lock", lambda v: None if num(v) is None else num(v) == 2)


def ev_fpslock(s, tl):
    log = s.log
    ev = Ev(need=3)
    if s.mode == "FLAT":
        ev.na = "VR only"
    att = 0
    for ln, m in log.grep(r"^fps box attached to vehicle \(\d+ s\): (\d+) frame\(s\) attached"):
        ev.sample(ln.i, int(m.group(1)) > 0)
        att += int(m.group(1))
        ev.need = 4
    if att:
        ev.proof.append("%d frames attached to the cab" % att)
    r = log.grep(r"^fps box attached to vehicle: (ATTACHED to the cab|LOCKED ON)")
    if r:
        ev.sample(r[0][0].i, True)
        if not att:
            ev.need = 3
            ev.proof.append("'%s' line" % r[0][1].group(1))
    errs(log, ev, r"^fps box attached to vehicle: .*create failed")
    return ev


def st_quads(s):
    return key_timeline(s, "menu_vr_layer", to_bool)


def ev_quads(s, tl):
    log = s.log
    ev = Ev(need=4)
    if s.mode == "FLAT":
        ev.na = "VR only"
    tot = 0
    for ln, m in log.grep(r"^OpenXR overlay: (\d+) xrEndFrame\(s\) with our quads"):
        ev.sample(ln.i, int(m.group(1)) > 0)
        tot += int(m.group(1))
    if tot:
        ev.proof.append("%d frames sent with the panel / fps box quads" % tot)
    r = log.grep(r"^OpenXR overlay: first .* quad|^tuning menu: VR .* drawn into its OpenXR quad image")
    if r and not ev.samples:
        ev.need = 3
        ev.sample(r[0][0].i, True)
        ev.proof.append("'OpenXR overlay: first quad' line (level 3 proof)")
    errs(log, ev, r"^OpenXR overlay: ", lvl=("E",))
    errs(log, ev, r"^OpenXR overlay: .*quad path is off", auto=True, lvl=("W",))
    return ev


def st_preview(s):
    return key_timeline(s, "preview_dlaa", to_bool)


def ev_preview(s, tl):
    log = s.log
    ev = Ev(need=4)
    best = 0
    for ln, m in log.grep(r"^preview DLAA stats: (\d+) preview frames, (\d+) target runs"):
        ev.sample(ln.i, int(m.group(2)) > 0)
        best = max(best, int(m.group(2)))
    if best:
        ev.proof.append("%d truck-preview DLAA runs (preview DLAA stats)" % best)
    r = log.grep(r"^preview DLAA: (running on \d+ target|started \d+ Presents|slot \d+ first Run took .* ok=1)")
    if r and not ev.samples:
        ev.need = 3
        ev.sample(r[0][0].i, True)
        ev.proof.append("'preview DLAA: %s' line (level 3 proof)" % short(r[0][1].group(1), 40))
    if not ev.samples and not log.grep(r"^preview (target|unit|tile|eye|DLAA|depth)", start=log.startup_end):
        ev.notused = "the truck preview (menu / garage / dealer) was never shown"
    errs(log, ev, r"^preview DLAA: target \d+ \(unit \d+\) Run failed")
    errs(log, ev, r"^preview target \d+: .* -- no DLAA there", auto=True)
    return ev


def st_multiview(s):
    return key_timeline(s, "flat_multiview", to_bool)


def ev_multiview(s, tl):
    log = s.log
    ev = Ev(need=4)
    if s.mode == "VR":
        ev.na = "flat only"
    for ln, m in log.grep(r"^flat multi-view @blit \d+: (\d+) views \((\d+) with DLAA\)"):
        ev.sample(ln.i, int(m.group(2)) >= 2)
        ev.proof = ["%s screens, %s with DLAA" % (m.group(1), m.group(2))]
    if not ev.samples:
        if log.grep(r"flat multi-view detected"):
            ev.need = 3
            ev.sample(log.grep(r"flat multi-view detected")[0][0].i, True)
            ev.proof.append("'flat multi-view detected' line")
        else:
            ev.notused = "one screen only (the game's multi-monitor mode was not used)"
    return ev


# name, scope, needs the world drawn, state, evidence
FEATURES = [
    ("DLAA / DLSS (main view)",       "both", True,  st_dlaa,       ev_dlaa),
    ("DLSS upscale",                  "both", True,  st_upscale,    ev_upscale),
    ("Sharpen (RCAS)",                "both", True,  st_sharpen,    ev_sharpen),
    ("DLAA before tone-map (HDR)",    "flat", True,  st_pretonemap, ev_pretonemap),
    ("Motion vectors",                "both", True,  st_mv,         ev_mv),
    ("Per-draw motion vectors",       "both", True,  st_drawids,    ev_drawids),
    ("See-through depth (fwd depth)", "both", True,  st_fwd,        ev_fwd),
    ("Jitter",                        "both", True,  st_jitter,     ev_jitter),
    ("Lighting redraw jitter",        "both", True,  st_lightjit,   ev_lightjit),
    ("Mirror DLAA",                   "both", True,  st_mirror,     ev_mirror),
    ("Mirrors in turns",              "both", True,  st_turns,      ev_turns),
    ("Texture LOD bias",              "both", True,  st_lod,        ev_lod),
    ("Anisotropic filter",            "both", True,  st_aniso,      ev_aniso),
    ("Wire widening",                 "both", True,  st_wire,       ev_wire),
    ("Light glow fix (bias mask)",    "both", True,  st_bias,       ev_bias),
    ("Menu truck-preview DLAA",       "both", False, st_preview,    ev_preview),
    ("Triple screens (multi-view)",   "flat", True,  st_multiview,  ev_multiview),
    ("Foveated (VRS)",                "vr",   True,  st_vrs,        ev_vrs),
    ("Foveated passes (lighting)",    "vr",   True,  st_vrspasses,  ev_vrspasses),
    ("Eye tracking (VRS)",            "vr",   True,  st_eye,        ev_eye),
    ("Hidden area mask",              "vr",   True,  st_hmask,      ev_hmask),
    ("Turbo mode",                    "vr",   False, st_turbo,      ev_turbo),
    ("VR FPS cap",                    "vr",   False, st_vrcap,      ev_vrcap),
    ("Reflex",                        "flat", False, st_reflex,     ev_reflex),
    ("FPS cap (flat)",                "flat", False, st_fpscap,     ev_fpscap),
    ("CPU tuning",                    "both", False, st_cpu,        ev_cpu),
    ("CPU cache pin",                 "both", False, st_pin,        ev_pin),
    ("fps box",                       "both", False, st_fpsbox,     ev_fpsbox),
    ("fps box attached to vehicle",   "vr",   True,  st_fpslock,    ev_fpslock),
    ("VR panel / fps box as quads",   "vr",   False, st_quads,      ev_quads),
]

# the mod version that brought the feature (an older log cannot have it): "not in this mod version" instead of a false FAIL
SINCE = {
    "DLSS upscale": "0.7.0", "Menu truck-preview DLAA": "0.7.8", "See-through depth (fwd depth)": "0.9.0",
    "Texture LOD bias": "0.9.0", "Anisotropic filter": "0.9.0", "Per-draw motion vectors": "0.10.0",
    "DLAA before tone-map (HDR)": "0.10.0", "Mirror DLAA": "0.10.0", "fps box": "0.10.0",
    "VR panel / fps box as quads": "0.10.3", "Turbo mode": "0.11.0", "Foveated (VRS)": "0.11.0", "CPU cache pin": "0.11.0",
    "Foveated passes (lighting)": "0.12.0", "Hidden area mask": "0.12.0", "Mirrors in turns": "0.12.0",
    "Triple screens (multi-view)": "0.12.0", "fps box attached to vehicle": "0.12.0", "Lighting redraw jitter": "0.13.0",
    "Light glow fix (bias mask)": "0.13.0", "Reflex": "0.13.0", "FPS cap (flat)": "0.13.0", "VR FPS cap": "0.13.0",
    "CPU tuning": "0.13.0", "Wire widening": "0.14.0", "Eye tracking (VRS)": "0.14.0",
}


# the mod version that first wrote the line used as proof (older logs: "?" instead of a false FAIL)
PROOF_SINCE = {"fps box": "0.10.3", "Sharpen (RCAS)": "0.7.6"}


def vtuple(v):
    return tuple(int(x) for x in re.findall(r"\d+", v or "")[:3])


# ------------------------------------------------------------------------------------------------------------------------
# verdicts

def fmt_state(v):
    if v is None:
        return "?"
    if v is True:
        return "on"
    if v is False:
        return "off"
    return str(v)


def judge(sess, name, scope, needs_world, tl, ev):
    """-> (verdict, set-to text, evidence text)"""
    # the state as a bool timeline (sharpness: a number -> on when > 0)
    btl = Timeline()
    evs = list(tl.ev) + [(i, False, "auto") for i in ev.autooff_at]
    evs.sort(key=lambda e: e[0])
    for i, v, w in evs:
        b = v if isinstance(v, bool) or v is None else (num(v, 0) > 0)
        btl.add(i, b, w)
    final = btl.final
    ever_on = btl.ever(lambda v: v is True)
    set_to = fmt_state(final)
    if isinstance(tl.final, float) and not isinstance(tl.final, bool):
        set_to = "%s (%g)" % (fmt_state(final), tl.final)
    if btl.final_where == "auto":
        set_to = "off (by the mod)"
    n_ch = len([e for e in btl.ev[1:] if e[0] > sess.log.startup_end and e[2] != "auto"])   # (the first value is no change)
    if n_ch:
        set_to += ", changed %dx" % n_ch
    proof = "; ".join(ev.proof)

    if sess.no_dlaa_build:
        return "N/A", set_to, "this build has no DLAA (WITH_DLAA=OFF)"
    if sess.version and name in SINCE and vtuple(sess.version) < vtuple(SINCE[name]) and not ev.samples:
        return "N/A", "-", "not in mod v%s (added in v%s)" % (sess.version, SINCE[name])
    if ev.na or (scope == "vr" and sess.mode == "FLAT") or (scope == "flat" and sess.mode == "VR"):
        return "N/A", set_to, ev.na or ("VR only" if scope == "vr" else "flat only")

    # ran while it was ON (a window that straddles a switch counts for both sides)
    # a stats line counts the work since the previous line of the same kind (eye 0 / eye 1 lines are separate kinds)
    def kind(i):
        return re.sub(r"\d+\.\d+|\d{2,}", "", sess.log.lines[i].text[:25]) if 0 <= i < sess.log.end else ""

    samples = sorted(ev.samples)
    prev_of_kind = {}

    def state_window(i):
        k = kind(i)
        j = prev_of_kind.get(k, min(sess.world_start, i))
        prev_of_kind[k] = i
        states = {btl.at(i), btl.at(j)}
        states.update(v for x, v, _ in btl.ev if j < x <= i)
        return states

    ran_on = False
    ran_off = None
    on_in_window = False                           # was it on during any stats window at all?
    for i, ran in samples:
        st = state_window(i)
        on_in_window = on_in_window or True in st or None in st
        if not ran:
            continue
        if True in st or None in st:
            ran_on = True
        if st == {False} and ran_off is None:
            ran_off = i
    ran_any = any(r for _, r in samples)

    if ran_off is not None:
        return "FAIL", set_to, "OFF, but it ran (%s %s)" % (sess.log.lines[ran_off].ts, short(sess.log.lines[ran_off].text, 70)) \
            if 0 <= ran_off < sess.log.end else "OFF, but it ran: " + (proof or "work counted while off")
    if final is None and not ever_on:
        if ran_any:
            return "PASS", "on? (no key in log/ini)", proof
        if not ev.found_any:
            return "?", set_to, "no line about it in this log (older mod version?)"
        return "OFF", set_to, "did not run"
    if not ever_on:
        return "OFF", set_to, "did not run" + ("" if ev.found_any or ev.samples else "")
    # it was on (at least for a while)
    if ev.fails:
        more = " (+%d more)" % (len(ev.fails) - 3) if len(ev.fails) > 3 else ""
        return "FAIL", set_to, ("it ran (%s), but: " % proof if ran_on and proof else "") + "; ".join(ev.fails[:3]) + more
    if ev.autooff:
        return "AUTO-OFF", set_to, ("it ran (%s), then " % proof if ran_on else "") + "the mod switched it off: " + ev.autooff[0]
    if ran_on:
        return "PASS", set_to, proof
    if samples and not on_in_window:
        on_at = [sess.log.lines[i].ts for i, v, _ in btl.ev if v is True and 0 <= i < sess.log.end]
        return "NOT USED", set_to, "on only for a short time (%s) that no stats window covered" % (
            "from " + on_at[0] if on_at else "before the world was drawn")
    if ev.notused:
        return "NOT USED", set_to, ev.notused
    if scope == "vr" and sess.mode == "VR" and not sess.xr and sess.level_max >= 3 and sess.version:
        return "NOT USED", set_to, "the VR session never started (the headset did not show the game)"
    if needs_world and sess.world_known and not sess.world:
        if sess.world_unseen:
            return "NOT USED", set_to, "the mod never took the 3D world (see the DLAA row)"
        return "NOT USED", set_to, "no 3D world frame in this log (menu only, no driving?)"
    if sess.level_max < ev.need:
        return "LOW LOG", set_to, "log_level %d is too low to see it run: need log_level = %d" % (sess.level_max, ev.need)
    if not ev.found_any and not ev.samples:
        if sess.version is None:
            return "?", set_to, "line not found (older version?)"
        if name in PROOF_SINCE and vtuple(sess.version) < vtuple(PROOF_SINCE[name]):
            return "?", set_to, "mod v%s writes no line that proves it (from v%s on)" % (sess.version, PROOF_SINCE[name])
        return "FAIL", set_to, "on, but the log has no line that shows it ran"
    last = max(i for i, _ in ev.samples) if ev.samples else -1
    tail = (" -- last line: %s %s" % (sess.log.lines[last].ts, short(sess.log.lines[last].text, 90))) if 0 <= last < sess.log.end else ""
    return "FAIL", set_to, "on, but no sign it ran (every counter 0)" + tail


def error_kinds(log):
    kinds = {}
    for ln in log.lines:
        if ln.lvl not in ("E", "W"):
            continue
        t = _HEX.sub("#", ln.text)
        t = _NUM.sub("N", t)
        t = short(t, 100)
        key = ("ERROR" if ln.lvl == "E" else "WARN", t)
        kinds[key] = kinds.get(key, 0) + 1
    return sorted(kinds.items(), key=lambda kv: (kv[0][0] != "ERROR", -kv[1]))


# work that only happens while the main view's DLAA runs (jitter, motion vectors, the texture bias, ... are its inputs)
NEEDS_DLAA = {
    "DLSS upscale", "Sharpen (RCAS)", "DLAA before tone-map (HDR)", "Motion vectors", "Per-draw motion vectors",
    "See-through depth (fwd depth)", "Jitter", "Lighting redraw jitter", "Mirror DLAA", "Mirrors in turns",
    "Texture LOD bias", "Anisotropic filter", "Light glow fix (bias mask)",
}


def check(log_path, ini_path=None):
    with open(log_path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    if ini_path is None:
        ini_path = os.path.join(os.path.dirname(os.path.abspath(log_path)), "dlaa.ini")
    log = Log(text)
    sess = Session(log, read_ini(ini_path))
    rows = []
    dlaa = None
    for name, scope, world, stf, evf in FEATURES:
        tl = stf(sess)
        ev = evf(sess, tl)
        row = judge(sess, name, scope, world, tl, ev)
        if dlaa is None:
            dlaa = row[0]                          # FEATURES[0] = the main view's DLAA
        elif name in NEEDS_DLAA and row[0] == "FAIL" and dlaa in ("FAIL", "OFF") and not ev.fails \
                and not any(r for _, r in ev.samples) and not row[2].startswith("OFF, but"):
            row = ("NOT USED", row[1], "the main DLAA itself %s, so this could not run" %
                   ("failed (see its row)" if dlaa == "FAIL" else "was off"))
        rows.append((name,) + row)
    return sess, rows, error_kinds(log), ini_path


def report(log_path, ini_path=None, out=sys.stdout):
    sess, rows, kinds, ini_path = check(log_path, ini_path)
    w = out.write
    w("Feature check: %s\n" % log_path)
    w("  mod v%s, %s, %s, log_level %d%s%s\n" % (
        sess.version or "? (no version line)", sess.game or "game ?", sess.mode or "VR/flat not known",
        sess.level_start, (" -> %d" % sess.level_end) if sess.level_end != sess.level_start else "", sess.level_note))
    w("  dlaa.ini: %s\n" % (ini_path if os.path.isfile(ini_path) else "not found (only the log is used)"))
    if sess.world:
        w("  the game drew the world: %d passes, %d %s blits\n" % (sess.passes, sess.blits, "eye" if sess.mode == "VR" else "screen"))
    elif sess.world_unseen:
        w("  the game drew the 3D world, but the mod saw no pass of it -- see the DLAA row\n")
    elif sess.world_known:
        w("  no 3D world frame in this log (menu only?) -- most features cannot run in such a session\n")
    else:
        w("  world frames: not known at this log level\n")
    if not sess.ended:
        w("  NOTE: no 'injector unloading' line -- the game was still running, or it crashed\n")
    w("\n%-9s %-30s %-22s %s\n" % ("VERDICT", "FEATURE", "SET TO (AT THE END)", "EVIDENCE"))
    w("%s\n" % ("-" * 120))
    counts = {}
    for name, verdict, set_to, evid in rows:
        counts[verdict] = counts.get(verdict, 0) + 1
        w("%-9s %-30s %-22s %s\n" % (verdict, name, short(set_to, 22), evid))
    w("\n%s\n" % ", ".join("%d %s" % (counts[k], k) for k in
                            ("PASS", "FAIL", "AUTO-OFF", "NOT USED", "OFF", "N/A", "LOW LOG", "?") if k in counts))
    if sess.level_max < 4:
        w("Log level %d: many proofs are only written at log_level = 4 (debug). Set log_level = 4 in dlaa.ini for a full check.\n"
          % sess.level_max)
    w("\nERROR / WARNING lines (%d kinds):\n" % len(kinds))
    if not kinds:
        w("  none\n")
    for (lvl, t), n in kinds:
        w("  %-5s x%-5d %s\n" % (lvl, n, t))
    return 1 if counts.get("FAIL") else 0


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        sys.stdout.write(__doc__)
        return 2
    log_path = argv[1]
    if not os.path.isfile(log_path):
        sys.stderr.write("feature_check: no such file: %s\n" % log_path)
        return 2
    return report(log_path, argv[2] if len(argv) > 2 else None)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
