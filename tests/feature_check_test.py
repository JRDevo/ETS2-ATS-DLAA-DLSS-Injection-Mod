#!/usr/bin/env python3
"""feature_check_test.py -- unit tests for scripts/feature_check.py (issue #26).

  python tests/feature_check_test.py      exit 0 = every test passed

Each test writes a small synthetic dlaa_inject.log built from the mod's real line formats (src/inject.cpp, xr_hook.cpp,
vrs.cpp, frame_pace.cpp, fps_cab.cpp, gpu_perf.cpp) and checks the verdict of the rows it is about.
"""
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "scripts"))
import feature_check as fc  # noqa: E402


def stamp(lines):
    """['text', 'WARN  text', ...] -> the log file text with a rising [hh:mm:ss.mmm] stamp per line."""
    out = []
    for n, t in enumerate(lines):
        s = n * 0.25
        out.append("[12:%02d:%06.3f] %s" % (int(s // 60) % 60, s % 60, t))
    return "\n".join(out) + "\n"


def head(version="0.15.1", level=4, launch="VR", extra=()):
    lv = {1: "errors", 2: "warnings", 3: "info", 4: "debug"}[level]
    return [
        "real dinput8 loaded @ 00007FFAE66F0000 (DirectInput8Create=00007FFAE67011E0)",
        "=== ETS2/ATS DLAA injector v%s loaded (pid=1234) ===" % version,
        "logging (phase 39): log_level=%d (%s) from dlaa.ini log_level -- 0 off, 1 errors, 2 warnings, 3 info, 4 debug" % (level, lv),
        "config (dlaa.ini): jitter_enabled=1 jitter_phases=8 mv_enabled=1 sharpness=0.60 sharp_radius=1.5 dlaa_area=50 "
        "dlss_upscale=0 preview_dlaa=1",
        "lighting redraws (phase 55): light_redraw_jitter=1 (the lighting pass's mesh redraws ...)",
        "DLSS bias mask (phase 49): dlss_bias_mask=0 % (off: NGX gets no mask)",
        "per-draw motion vectors (v0.10.0): mv_objects=2 mv_drawid_instanced=1 -- every world G-buffer draw is replayed",
        "per-draw motion vectors phase 3 (v0.10.0): mv_camr_consensus=1; forward-pass draw ids off (needs mv_objects=2 and mv_fwd_depth=1)",
        "phase 4 (v0.10.0): dlaa_pre_tonemap=1 (Alt+F7 live switch) mv_fwd_attach_m=0.50",
        "phase 34: VRS foveated rendering vrs=1 vrs_inner=35 vrs_outer=60 vrs_quality=1 vrs_debug=0 vrs_passes=0 (world scene binds only) vrs_strength=2 vrs_eye=0",
        "phase 41: mirror_turns=0 (dlaa.ini)",
        "mirror units (v0.10.0 phase 2): mirror_dlaa=0 mirror_min_px=128 mirror_max_views=8 (Alt+F6 live switch)",
        "forward depth for motion vectors (v0.9.0 r5; r11 overlay depth): mv_fwd_depth=1 (Ctrl+F3 live switch) mv_fwd_min_m=0.50",
        "texture LOD bias (v0.9.0): tex_lod_bias=0.00 tex_lod_bias_auto=0 tex_aniso=16 tex_lod_bias_cutout=-0.75 -- world scene",
        "hotkeys: mode_cycle=End wire_widen=Alt+F12 menu=Delete",
        "cpu_cache_pin (phase 32): off (cpu_cache_pin=0) -- process affinity untouched",
        "cpu tuning (phase 54): off (cpu_pcore=0 cpu_priority=0 cpu_no_throttle=0) -- nothing touched",
        "launch mode: %s (from the launch options)" % launch,
        "DLAA build: starts ON (the DLAA toggle key switches it; dlaa_off.txt next to the DLL = start OFF)",
        "mode from dlaa.ini: DLAA (End cycles DLAA -> DLSS -> off and saves it)",
        "hidden area mask (phase 40): xr_hidden_mask=1 (ON: XR_KHR_visibility_mask is added at the game's xrCreateInstance)",
        "Eye tracking (phase 59, experimental): vrs_eye=0 vrs_eye_fake=0 vrs_eye_smooth=20 ms (vrs=1) -- off",
        "Turbo mode (phase 30, from OpenXR Toolkit): xr_turbo=1 (ON: the game does not wait for the headset in xrWaitFrame)",
        "frame rate (phase 52): reflex=0 (off) fps_cap=0 (off) vr_fps_cap=0 (off)",
        "fps box (v0.10.0): fps_show=1 (on: shown also while the menu is closed), fps_text=1, fps_vr_lock=0 (follow the head)",
    ] + list(extra) + [
        "Present #0  2560x1440 fmt=87 sync=0 flags=0x200 tid=1 caller=amtrucks.exe+0x2c123a reentrant=0",
    ]


def fifo(blit, passes, e0, e1, stage="post-tonemap", pre_runs=0, pre_failed=0, up="off", sharp="0.6", lod="0.00", solid=699,
         mode="VR"):
    rate = "VR: fps = blits/s / 2" if mode == "VR" else "flat: fps = blits/s"
    return ("FIFO stats @blit %d: passes=%d blits=%d depth snapshot used=%d live used=0 fifo underflow=0 fifo overflow=0 "
            "mirror ignored=0 secondary gbuf ignored=0 mirror views handled=0 not handled=%d | eye map: RT map entries=6 | "
            "R |x|>0.25: e0=1/147 e1=1/146 | dlaa frames e0=%d e1=%d | cpu: blit 0.204 ms/blit | rate: blits/s=245.8 fps=122.9 (%s) "
            "[profile=high preset=default sharp=%s r=1.5 area=50 up=%s lod=%s aniso=16 scope=opaque/opaque cutout=-0.75 twins=3 "
            "subst/frame=solid %d (full set: bias %s aniso 16) / alpha-tested 207 (-> see-through set: bias 0.00 aniso 0, "
            "tex_lod_bias_cutout=-0.75) / see-through 133 (see-through set: bias 0.00 aniso 0) (draws) enters/frame=1.9 "
            "stage=%s (pre-tonemap runs %d (at the forward leave 0), deferred 0, failed %d; post-tonemap blit runs 415)] | Present #1706"
            % (blit, passes, blit, passes, passes, e0, e1, rate, sharp, up, lod, solid, lod, stage, pre_runs, pre_failed))


def vr_world(blits=(600, 1200), dlaa=True):
    out = ["OpenXR session: xrBeginSession(0000000000000001, view configuration 2) by the game -> 0 = XR_SUCCESS (tid=1)",
           "eye blit detected: eye=0 (pass s=1) src 4136x5848 -> RT 4136x5848 fmt=29 (VR eye texture, Present #1114)"]
    done = 0
    for b in blits:
        n = b // 2 if dlaa else 0
        out.append(fifo(b, b // 2, n, n))
        out.append("jitter check @blit %d: 0 of %d passes drew their first G-buffer draw with a different viewport shift than "
                   "the jitter DLSS is told (expect 0); passes started by a scene depth clear: 3" % (b, b // 2))
        out.append("lighting redraws (phase 55, light_redraw_jitter 1) @blit %d: 232 shifted redraws in 309 lighting binds (0.4 "
                   "per blit), 232 runs; back to the game's viewport at a Draw 0, a non-qualifying DrawIndexed 0, a depth-stencil "
                   "state 232; redraws NOT shifted (viewport not the pass size) 0; cached states: depth-stencil 2, rasterizer 1" % b)
        out.append("MV draw-ids @blit %d | ACTIVE, instanced hooked | record: 323 draws/pass (97.5 instanced), 0.0 full/pass | "
                   "replay: 209 draws/pass, 0 skipped | pairing (eye 0, 154 frames, 153 read back): paired 95.1 %% of 339 "
                   "draws/frame (movers 0.0)" % b)
        out.append("MV forward depth batch (v0.10.0 phase 10) over 300 passes: 5.0 draws re-drawn per pass in 1.00 segments, "
                   "occlusion init in 300 (no snapshot for it 0; mv_fwd_depth_cull 1), forced 0, failed 0 | CPU 0.010 ms/pass")
        out.append("DLAA GPU cost eye 0: total 0.98 ms | copy-in 0.01 | depth 0.00 | mv 0.29 | ngx 0.61 | sharpen 0.04 | "
                   "copy-back 0.02 (avg over 600) [preset=default sharp=0.6 r=1.5 area=50 up=off rect=2448x2600 of 6120x6496]")
        out.append("perf @blit %d: fps-box 0.009 avg / 7.337 max ms per frame (1265 of 1265 timed frames; per frame, both eyes / "
                   "every target together; not in the eye lines' mod total)" % b)
        done += 1
    out += [
        "VRS @10 s (Present #1219): 4000 scene binds entered (eye 0 2000 / eye 1 2000), draws coarse-capable 50000 (80.0 %), "
        "full rate: cut-out 0 / SV_Depth 0 / unknown shader 0",
        "hidden area mask @10 s: masks drawn eye 0 300 / eye 1 300; passes without: no certain eye 0",
        "OpenXR turbo stats (last 30 s): 1910 async xrWaitFrame (1910 in all), 1911 made-up frame states, 1910 xrBeginFrame "
        "skipped, 1 second-poll blocks, 1-s timeouts 0 at xrEndFrame / 0 at xrWaitFrame, 0 failures | phase 33: 1 first waits",
        "OpenXR overlay: 900 xrEndFrame(s) with our quads -- quad images drawn 10, reused 890",
        "preview DLAA stats: 600 preview frames, 1199 target runs, 2 target(s) ran this frame",
    ]
    return out


def unload(passes, blits, mode="VR", dlaa="on"):
    return "=== injector unloading (DLL unload): presents=2155 passes=%d blits=%d mode=%s dlaa=%s dlaa-mode=DLAA ===" % (
        passes, blits, mode, dlaa)


class Base(unittest.TestCase):
    def tmpdir(self):
        d = tempfile.mkdtemp(prefix="feature_check_test_")
        self.addCleanup(shutil.rmtree, d, True)
        return d

    def run_log(self, lines, ini=None):
        d = self.tmpdir()
        path = os.path.join(d, "dlaa_inject.log")
        with open(path, "w", encoding="utf-8") as f:
            f.write(stamp(lines))
        ini_path = os.path.join(d, "dlaa.ini")
        if ini is not None:
            with open(ini_path, "w", encoding="utf-8") as f:
                f.write(ini)
        sess, rows, kinds, _ = fc.check(path, ini_path)
        return sess, {r[0]: r[1:] for r in rows}, kinds

    def verdict(self, rows, name, want):
        self.assertIn(name, rows)
        got = rows[name]
        self.assertEqual(got[0], want, "%s: %s (set to %s; %s)" % (name, got[0], got[1], got[2]))


class TestPass(Base):
    def test_vr_session_runs(self):
        sess, rows, kinds = self.run_log(head() + vr_world() + [unload(600, 1200)], ini="wire_widen = 0\n")
        self.assertEqual(sess.mode, "VR")
        self.assertTrue(sess.world)
        for name in ("DLAA / DLSS (main view)", "Sharpen (RCAS)", "Motion vectors", "Per-draw motion vectors",
                     "See-through depth (fwd depth)", "Jitter", "Lighting redraw jitter", "Anisotropic filter",
                     "Menu truck-preview DLAA", "Foveated (VRS)", "Hidden area mask", "Turbo mode", "fps box",
                     "VR panel / fps box as quads"):
            self.verdict(rows, name, "PASS")

    def test_level3_proof(self):
        lines = head(level=3) + [
            "OpenXR session: xrBeginSession(0000000000000001, view configuration 2) by the game -> 0 = XR_SUCCESS (tid=1)",
            "VRS: mask built for eye 1: 4136x5848 px -> 259x366 tiles of 16 px, centre uv (0.621, 0.403)",
            "hidden area mask: FIRST mask drawn -- pass s=26, eye 0 = OpenXR view 1 (eye sources: RT-cycle)",
            "DLAA: eye 0 unit built in this Run: 40.0 ms (textures + NGX feature 30.0 ms, MV init + first eval 10.0 ms), ok=1",
            unload(876, 1390)]
        sess, rows, _ = self.run_log(lines, ini="")
        self.verdict(rows, "Foveated (VRS)", "PASS")
        self.verdict(rows, "Hidden area mask", "PASS")
        self.verdict(rows, "DLAA / DLSS (main view)", "PASS")


class TestFail(Base):
    def test_on_but_counters_zero(self):
        world = [l for l in vr_world() if not l.startswith("VRS @")] + [
            "VRS @12 s (Present #1219): 0 scene binds entered (eye 0 0 / eye 1 0), draws coarse-capable 0 (0.0 %), full rate"]
        sess, rows, _ = self.run_log(head() + world + [unload(600, 1200)], ini="")
        self.verdict(rows, "Foveated (VRS)", "FAIL")

    def test_off_but_ran(self):
        world = vr_world() + [
            "wire widening (phase 61, off, wire_min_px 1.00) over 600 passes: 4.0 wire draws widened per pass (1.0 of them pole "
            "hardware, phase 62) + 2.0 solid G-buffer draws dithered (phase 63), skipped: blend 0, game GS 0",
            "wire widening (phase 61, off, wire_min_px 1.00) over 600 passes: 4.0 wire draws widened per pass (1.0 of them pole "
            "hardware, phase 62) + 2.0 solid G-buffer draws dithered (phase 63), skipped: blend 0, game GS 0"]
        sess, rows, _ = self.run_log(head() + world + [unload(600, 1200)], ini="wire_widen = 0\n")
        self.verdict(rows, "Wire widening", "FAIL")
        self.assertIn("OFF, but it ran", rows["Wire widening"][2])

    def test_error_line_fails(self):
        world = vr_world() + ["ERROR DLAA: EvaluateFeature failed 0xbad00005 (fail #1)"]
        sess, rows, kinds = self.run_log(head() + world + [unload(600, 1200)], ini="")
        self.verdict(rows, "DLAA / DLSS (main view)", "FAIL")
        self.assertTrue(any(k[0] == "ERROR" for k, _ in kinds))

    def test_exit_code(self):
        d = self.tmpdir()
        path = os.path.join(d, "dlaa_inject.log")
        world = [l for l in vr_world() if not l.startswith("OpenXR turbo stats")] + [
            "OpenXR turbo stats (last 30 s): 0 async xrWaitFrame (0 in all), 0 made-up frame states, 0 xrBeginFrame skipped, "
            "0 second-poll blocks, 1-s timeouts 0 at xrEndFrame / 0 at xrWaitFrame, 0 failures"]
        with open(path, "w", encoding="utf-8") as f:
            f.write(stamp(head() + world + [unload(600, 1200)]))
        with open(os.devnull, "w") as null:
            self.assertEqual(fc.report(path, os.path.join(d, "dlaa.ini"), out=null), 1)


class TestOff(Base):
    def test_off_and_idle(self):
        sess, rows, _ = self.run_log(head() + vr_world() + [unload(600, 1200)], ini="")
        self.verdict(rows, "Mirror DLAA", "OFF")
        self.verdict(rows, "Light glow fix (bias mask)", "OFF")
        self.verdict(rows, "Eye tracking (VRS)", "OFF")
        self.verdict(rows, "Texture LOD bias", "OFF")          # the cut-out bias alone does nothing (bias 0)

    def test_switched_off_mid_session(self):
        # pre-tonemap runs in the first window, then Alt+F7 switches it off: the window that straddles the switch is fine,
        # the next one must show 0 runs
        lines = head(launch="FLAT") + [
            "eye blit detected: eye=0 (pass s=60) src 3840x2160 -> RT 3840x2160 fmt=87 (flat backbuffer, Present #1301)",
            fifo(600, 600, 600, 0, stage="pre-tonemap", pre_runs=590, mode="flat"),
            "DLAA pre-tonemap OFF (Alt+F7, Present #1400) -- the blit path from the next frame",
            fifo(1200, 1200, 1200, 0, stage="post-tonemap", pre_runs=30, mode="flat"),
            fifo(1800, 1800, 1800, 0, stage="post-tonemap", pre_runs=0, mode="flat"),
            unload(1800, 1800, mode="flat")]
        sess, rows, _ = self.run_log(lines, ini="")
        self.assertEqual(sess.mode, "FLAT")
        self.verdict(rows, "DLAA before tone-map (HDR)", "PASS")
        self.assertTrue(rows["DLAA before tone-map (HDR)"][1].startswith("off"))
        # ... and a run in a window fully after the switch = FAIL
        lines[-2] = fifo(1800, 1800, 1800, 0, stage="post-tonemap", pre_runs=12, mode="flat")
        sess, rows, _ = self.run_log(lines, ini="")
        self.verdict(rows, "DLAA before tone-map (HDR)", "FAIL")

    def test_auto_off(self):
        world = vr_world() + [
            "WARN  MV forward depth: OFF for this session (v0.10.0 phase 10 VR budget) -- the batched forward-depth re-draw "
            "measured 0.302 ms avg / 5.225 max per eye pass over 300 timed passes"]
        sess, rows, _ = self.run_log(head() + world + [unload(600, 1200)], ini="")
        self.verdict(rows, "See-through depth (fwd depth)", "AUTO-OFF")


class TestNotApplicable(Base):
    def test_flat_only_in_vr(self):
        sess, rows, _ = self.run_log(head() + vr_world() + [unload(600, 1200)], ini="")
        self.verdict(rows, "Reflex", "N/A")
        self.verdict(rows, "FPS cap (flat)", "N/A")
        self.verdict(rows, "DLAA before tone-map (HDR)", "N/A")
        self.verdict(rows, "Triple screens (multi-view)", "N/A")

    def test_vr_only_in_flat(self):
        lines = head(launch="FLAT") + [
            "eye blit detected: eye=0 (pass s=60) src 3840x2160 -> RT 3840x2160 fmt=87 (flat backbuffer, Present #1301)",
            fifo(600, 600, 600, 0, mode="flat"), unload(600, 600, mode="flat")]
        sess, rows, _ = self.run_log(lines, ini="")
        for name in ("Foveated (VRS)", "Hidden area mask", "Turbo mode", "VR FPS cap", "fps box attached to vehicle"):
            self.verdict(rows, name, "N/A")

    def test_older_version(self):
        lines = head(version="0.10.0") + vr_world() + [unload(600, 1200)]
        sess, rows, _ = self.run_log(lines, ini="")
        self.verdict(rows, "Wire widening", "N/A")
        self.verdict(rows, "Eye tracking (VRS)", "N/A")


class TestNotUsed(Base):
    def test_menu_only(self):
        lines = head() + ["Present #1: passes=0 blits=0 outstanding=0 mode=undetected eye-map-entries=0 | dlaa=on dlaa-mode=DLAA "
                          "ngx-feature=0", unload(0, 0, mode="undetected")]
        sess, rows, _ = self.run_log(lines, ini="")
        self.assertFalse(sess.world)
        self.verdict(rows, "DLAA / DLSS (main view)", "NOT USED")
        self.verdict(rows, "Motion vectors", "NOT USED")
        self.verdict(rows, "Turbo mode", "NOT USED")             # no OpenXR session: the headset never showed the game


class TestLowLog(Base):
    def test_level_too_low(self):
        lines = head(level=3) + [
            "OpenXR session: xrBeginSession(0000000000000001, view configuration 2) by the game -> 0 = XR_SUCCESS (tid=1)",
            unload(876, 1390)]
        sess, rows, _ = self.run_log(lines, ini="")
        self.assertEqual(sess.level_max, 3)
        for name in ("Motion vectors", "Per-draw motion vectors", "Jitter", "Turbo mode"):
            self.verdict(rows, name, "LOW LOG")
        self.assertIn("log_level = 4", rows["Motion vectors"][2])

    def test_level_raised_in_menu(self):
        lines = head(level=1) + ["log level -> 4 (debug)"] + vr_world() + [unload(600, 1200)]
        sess, rows, _ = self.run_log(lines, ini="")
        self.assertEqual(sess.level_max, 4)
        self.verdict(rows, "Motion vectors", "PASS")


class TestParsing(Base):
    def test_prose_is_not_a_value(self):
        # "(needs mv_objects=2 and mv_fwd_depth=1)" after "mv_objects=0" must not turn the per-draw MVs on
        extra = ["per-draw motion vectors (v0.10.0): mv_objects=0 mv_drawid_instanced=1 -- off (mv_objects=0)",
                 "per-draw motion vectors phase 3 (v0.10.0): mv_camr_consensus=1; forward-pass draw ids off (needs "
                 "mv_objects=2 and mv_fwd_depth=1)"]
        lines = head(extra=extra) + vr_world() + [unload(600, 1200)]
        lines = [l for l in lines if not l.startswith("MV draw-ids @blit")]
        sess, rows, _ = self.run_log(lines, ini="")
        self.verdict(rows, "Per-draw motion vectors", "OFF")

    def test_hotkey_line_ignored(self):
        sess, rows, _ = self.run_log(head() + vr_world() + [unload(600, 1200)], ini="")
        self.assertNotEqual(rows["Wire widening"][1], "Alt+F12")

    def test_error_kinds_counted(self):
        world = vr_world() + ["WARN  VR eye blit #2 re-draws the same eye texture as the previous blit of Present #1114",
                              "WARN  VR eye blit #4 re-draws the same eye texture as the previous blit of Present #1115"]
        sess, rows, kinds = self.run_log(head() + world + [unload(600, 1200)], ini="")
        counts = {k[1]: n for k, n in kinds}
        self.assertTrue(any(n == 2 and t.startswith("VR eye blit") for t, n in counts.items()), counts)


if __name__ == "__main__":
    unittest.main(verbosity=1)
