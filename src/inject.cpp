// v0.8.1 -- PRESENT LAYER (driver frame generation, NVIDIA Smooth Motion = NvPresent64.dll). The layer presents its own
//           swapchain from its own D3D11 device on its own thread (real + generated frames); the game renders on another
//           device and its own Present never reaches our dxgi hook. v0.8.0 took the game context from the presenting
//           device, the v0.6.2 filter then threw every game call away (log captures/dlaa_inject_ats_flat_smoothmotion_on.log:
//           caller=NvPresent64.dll, "G-buffer bind on a non-game context ... not adopted", passes=0 blits=0). Now:
//           (a) ADOPTION: scene binds (4 RTVs + D32S8 >= 1024 = G-buffer, or the preview pass shape) on ONE foreign
//           immediate context (>= 60 binds, >= 1 s, >= 30 Presents) while the presenting context has passes=0 blits=0,
//           the Presents come from another thread and not from the game exe -> that context becomes the game context
//           on its own thread, under g_plMx (hkPresent's per-frame work runs under it until then). Soft: nothing was
//           created on the presenting device, so DLAA is NOT forced off (ResetPassTracking's restart rule is for a real
//           device change). Blocked evidence logs one WARNING with the reason. A known present-layer module
//           (nvpresent*) presenting another device while the game context is known never flips it (UpdateGameContext).
//           (b) FRAME BOUNDARY: in present-layer mode hkPresent (layer thread) only counts + logs (g_plPresents; the
//           layer presents generated frames too: ETS2 test ~140/s vs ~70 game frames/s). The per-frame work (hotkeys,
//           trace, NGX pre-warm, preview bookkeeping, NGX create budget, status logs, backbuffer) runs on the game
//           render thread (g_plGameTid, hard-gated: a request from any other thread is dropped + logged) at the
//           FRAME-END RULE (FrameEndOnBind / FrameEndOnDraw): the frame ends at the first bind of a non-screen target
//           after a FINAL draw into the screen target (RT0 with the backbuffer's size / format) -- not a 3-vertex
//           R11G11B10 tile composite (the SDR profile screen composites its 4 tile passes into the backbuffer), and
//           only after non-screen work since the last end. Simulated on both traces (ATS HDR world, ETS2 SDR profile
//           screen): exactly 1 per frame, with and without the runtime's post-Present unbind; the naive "any backbuffer
//           draw" rule gave 4 (profile) / 2 (world). In normal mode the rule only counts ("frame-end rule check" lines).
//           g_frames counts these game frames. Fallbacks while the rule finds nothing for 1 s: the scene depth clear
//           that starts a world pass, else an idle boundary (a game bind after 30 ms without one); both turn the
//           preview path off (PreviewFramesOk), it needs exact frames. The first build used RSSetViewports with 0
//           viewports as an "engine frame-start marker": the test logs showed it is the D3D11 runtime inside Present
//           (caller d3d11.dll+0x196ff5; under the layer 0 on the game context during play, and once from the layer's
//           thread at shutdown, which ran the per-frame work on the wrong thread) -- now a diagnostic line only.
//           (c) BACKBUFFER: size / format = the presenting swapchain's (ETS2 test: the game's target had the same
//           3840x2160 fmt=24, matched by the v0.7.3 desc rule); the identity is the screen target a frame-end boundary
//           ends on. Both are logged.
//           (d) NGX: the pre-warm never runs on the presenting device while Presents come through a known present
//           layer. Round 3 (ETS2 test: waiting for the adoption put its 0.6 s exactly on the first menu truck = a
//           visible jerk): MaybeEarlyPrewarm runs it on the boot screen instead, on the game's render thread inside the
//           game's own OMSetRenderTargets, on the first immediate context that is not the presenting one, of another
//           device, called from inside the game exe image, not on the layer's thread, 10 binds in a row. The adoption
//           line says whether NGX is already on the adopted device (WARNING if not). Fallbacks: no qualifying call =
//           the pre-warm at the first boundary after the adoption; NGX on a wrong device = NgxAcquire re-initialises
//           it on the game device while only the pin holds it.
//           (e) PREVIEW LAYOUT under the layer (round 3, a "jerk" entering the truck screen with full 72/s windows = no
//           stall): the main menu draws the truck as 4 untiled passes, the truck screen as 2x2 tiles. A settled layout
//           was re-checked every 31st frame -- ~70 ms at the menus' 400+ Presents/s in normal mode, ~0.43 s at 72 game
//           frames/s under the layer -- so after a switch the old layout's jitter scale / depth / MVs were applied for up
//           to 0.43 s (smear), then the verdict reset the history. Under a present layer: every 4th frame. Each layout
//           switch logs one "preview layout switch" line (stale frames, resets, picture-depth creation ms, our CPU ms
//           per frame around it).
//           (f) TILED PREVIEW SEAM (round 4, user: a faint black line at the screen's horizontal middle, top to bottom,
//           gone with the mode off). Not layer-specific, a bug of the tiled preview since v0.7.8 round 6: the tile
//           passes' viewport shift is 2x the picture jitter (up to +-0.875 tile px), and a shift beyond +-0.5 px leaves the
//           tile's first / last column (row) unrasterized = the pass's black clear; the left tiles' right column and the
//           right tiles' left column meet at the vertical seam. PvFixTileEdges fills such a column / row from its inner
//           neighbour at the pass's DS discard, before the game's composite. Debug switches (dlaa.ini): preview_edge_fix,
//           preview_tile_jitter, preview_layout_recheck. Apart from (f), normal (no layer) flat and VR are unchanged.
//           (g) SEAM LINE, round 5 (user: the line is still there; log captures/dlaa_inject_ets2_v081r4_linestill.log). That
//           run never left the "NOT a clean tiling -- reference-slot path" verdict (main menu, ~4.6 s of DLAA), no pass was
//           shifted by more than 0.4375 px and no edge-fix line exists: under the D3D11 pixel-centre rule (f) cannot
//           explain the line there. Proven from the logs: the verdict itself was wrong. Slots 0 / 2 fit the 2x2 tiling
//           exactly, slots 1 / 3 with residual ~0.6 and the SAME a.x / b.x in every misread verdict (r2 / r3 / r4 logs);
//           a tile offset only changes MVP rows 0 / 1, so a different row 3 = another INSTANCE of the same mesh (the game
//           culls per tile; the first sampled draw of the right tiles is not the left tiles' one). The main menu therefore
//           ran as untiled (shift scale 1 = the picture moved by half the jitter NGX was told, depth of the top-left tile
//           stretched over the picture, MVs in tile space) and the truck screen flipped TILED <-> untiled with a history
//           reset each time. Fixes: PvSolveLayout pairs the first 16 candidates of every pass by draw key and takes the
//           exact fit (the other passes collect 16 instead of 1); PvFixTileEdges also handles 0 < |shift| <= 0.49 with a
//           ZERO-GUARDED fill (kPvEdgeFillShader: only texels still at the clear colour get the inner neighbour = a no-op
//           on a GPU that follows the centre rule). The line's cause is NOT proven by these: the snapshot key on an
//           RGBA16F (HDR) preview target now runs a SEAM CAPTURE (PvSeam: 8 frames normal + 4 with the preview jitter
//           forced to 0; bands around x = W/2 of the DLAA input / NGX output / our result / the final target, the tile
//           RTs' edge columns before the fix, per-frame "pvseam:" lines + a VERDICT line, BMPs). Diagnostics: composites
//           landing after the target's DLAA ran ("late composites"), the edge-fix stats line every 600 preview frames also
//           untiled. Debug switches: preview_sharpen (0 = no RCAS on preview units; it ran with the world's 0.7 / r 4),
//           preview_seam_capture (the seam capture on 8-bit targets too).
// v0.8.0 -- HDR (flat). With Windows HDR on the game renders a float pipeline and the backbuffer is R10G10B10A2 (HDR10):
//           forward colour F (RGBA16F) -> composite C = F + bloom (RGBA16F, scene size) -> a 3-vertex copy of C into a
//           backbuffer-sized RGBA16F output target O (NOT the backbuffer) -> UI into O -> encode O into the backbuffer.
//           The 8-bit rule (SRV0 RGBA8 at scene size, RT0 = backbuffer) never matched: passes=N blits=0, no NGX (log +
//           frame trace captures/dlaa_trace_ats_flat_hdr_scale125.txt). New HDR rule, only while the backbuffer format
//           is HDR (g_hdrOut: R10G10B10A2 or RGBA16F): F = RTV0 of the forward jitter pass; a draw with RT0 = scene-sized
//           RGBA16F reading F is the composite (g_hdrCompTex = its RT0, no blit); a draw with RT0 = backbuffer-sized
//           RGBA16F (not C) reading C is the HDR blit, handled as a flat backbuffer blit. Told apart by SRV0 IDENTITY,
//           so Scaling 100 % (C and O the same size) is safe. SceneDlaa runs on C as an "HDR unit" (format-driven:
//           RGBA16F colorIn / out / sharp, NGX IsHDR unless dlaa.ini dlss_hdr=0, RCAS on a reversibly compressed value,
//           composite without sRGB decode into O); a format change rebuilds the unit. The profile / truck-preview screen
//           (its composite target is O too) runs in place on RGBA16F the same way. BMP dumps of RGBA16F are
//           tone-compressed; the 8-bit preview capture refuses to start on an RGBA16F target. Logs: "HDR output
//           detected", "HDR scene composite recognised", HDR variants of flat detected / first blit / first evaluate,
//           Present-line tail "hdr-out=1 hdr-blits=N", no-blit WARNING with backbuffer format + sizes. The 8-bit (SDR)
//           paths, flat and VR, are unchanged.
// v0.7.7 -- RE-BINDABLE HOTKEYS. Every control is now an action with a binding read from dlaa.ini (key_mode_cycle,
//           key_model_1..4, key_area_down/up, key_sharpen_down/up, key_width_down/up, key_dlaa_toggle, key_save,
//           key_upscale_toggle, key_mv_toggle, key_mv_debug, key_passive, key_jitter_only, key_selftest,
//           key_snapshot, key_trace, key_jitter_sign). Format "[Shift+][Ctrl+][Alt+]Key" (F1-F24, A-Z, 0-9, Home,
//           End, Insert, Delete, PageUp/PageDown, arrows, Space, Tab, Backspace, Enter, Pause, Numpad keys, OEM
//           punctuation) or "none" = disabled; a bad value logs a warning and keeps the default. Defaults are
//           exactly the v0.7.6 keys. Semantics unchanged: down-edge only, game focused, top-level Present, the
//           modifiers must match EXACTLY (no modifier = none held; Alt is now a usable modifier). The F-key-only
//           g_fWasDown[12] / FKeyPressed and the static End edge are replaced by one per-VK g_vkWasDown table
//           refreshed every Present (PollKeys). The trace key is still consumed before TraceAtPresent. Two
//           actions on the same combo log a warning at load (both fire); the binding table is logged once.
// v0.7.0 -- REAL DLSS UPSCALING (render res < output res). The game's r_scale_x / r_scale_y shrink only the scene
//           texture (the blit SOURCE); the blit RT (VR eye texture / flat backbuffer) keeps its size, so scene size
//           vs blit viewport size IS the render scale. New dlaa.ini dlss_upscale (default 1) + Ctrl+F4 live: when
//           the blit viewport is >= the scene in both axes and larger in one, NGX renders scene res -> viewport
//           res (PerfQuality from the per-axis geomean ratio: 1.3 UltraQuality / 1.5 Quality / 1.7 Balanced /
//           2.0 Performance / 3.0 UltraPerformance; the user preset on every hint parameter; MVLowRes). colorIn /
//           depth / MV stay render-size (crop-sized with dlaa_area), m_out / m_sharp are the crop mapped into
//           output space; RCAS runs at output res; NOTHING is copied back over the scene texture. The game's own
//           blit Draw runs first (bilinear stretch of the raw frame = what stays outside the DLAA area), then
//           hkDraw draws our result into the still-bound RT0 (own fullscreen-triangle VS + PS, viewport = output
//           rect, alpha blend with the feather ramp as alpha, RGB write mask, sRGB decode in the PS when the
//           game's SRV0 is an _SRGB view; full graphics-state save/restore, inside t_inDlaa). Halton phases while
//           upscaling: max(jitter_phases, round(jitter_phases * output/render pixel ratio)). GPU timing: the last
//           stage is "composite" (our draw only). up=WxH->WxH / up=off in the [preset=..] tags and the config line;
//           Shift+F12 also saves dlss_upscale; F10 info has upscale / render_size / output_size; with upscale the
//           F10 "out" and the F9 self-test dumps are the DLSS output (output res), MV debug is scaled to output.
//           Equal sizes or dlss_upscale=0 = the v0.6.5 path unchanged.
// v0.6.5 -- ONE HOTKEY SCHEME. All live controls are now F1..F12 with a single, consistent modifier (the user
//           runs the mod blind in the headset by hotkeys + beeps, and the old mix of End / PageUp / PageDown /
//           Home / Insert / F9-F11 was confusing). USER keys = Shift + F-key (Ctrl and Alt NOT held): F1..F4 =
//           DLAA model 1..4 = preset default / E / F / M (DIRECT select, replaces the Shift+End cycle; 1..4
//           beeps; re-selecting the active model just re-beeps, no recreate); F5/F6 = DLAA area -/+10 %; F7/F8 =
//           sharpen strength -/+0.1; F9/F10 = sharpen width (RCAS radius) -/+0.5; F11 = DLAA on/off; F12 = SAVE
//           dlss_preset / dlaa_area / sharpness / sharp_radius to dlaa.ini (rewrites in place, preserving every
//           other line; via dlaa.ini.tmp + MoveFileEx; rising 600->900 Hz on success, one long 200 Hz on fail).
//           DEBUG keys = Ctrl + F-key (Shift and Alt NOT held): F5 = motion vectors on/off, F6 = MV debug view,
//           F7 = passive mode, F8 = jitter-only debug, F9 = self-test, F10 = NGX input snapshot, F11 = frame
//           trace, F12 = cycle NGX jitter sign. Every control needs its modifier EXACTLY (a bare F-key, or the
//           wrong modifier, does nothing). All the old bindings (End, PageUp/PageDown, Home, Insert, plain
//           F9/F10/F11 and every Shift/Ctrl variant) are removed. One shiftOnly/ctrlOnly test and one
//           g_fWasDown[12] edge table (FKeyPressed) replace the ~8 per-key g_*WasDown globals; the behaviour of
//           each action, its beeps and its down-edge / foreground / top-level-Present rules are unchanged.
// v0.6.4 -- DLAA AREA. In the headset only the centre of each eye image is seen sharply (the lens blurs the outer
//           part), but every DLAA stage (NGX 1.08 ms, MV+depth 0.31, sharpen 0.22, copies 0.14 ms per eye at
//           4592x6496) scales with the pixel count. The whole pipeline now runs on a centred rect of each eye
//           image: dlaa_area % (40..100, default 100 = whole image = v0.6.3 path) of width AND height, each
//           rounded to a multiple of 8, centred on the eye's OPTICAL CENTRE and clamped into the image (origin
//           snapped to even px). colorIn / R32F depth / MV / out / sharp and the NGX feature are crop-sized
//           (CopySubresourceRegion in and back); the depth twins stay full-size and the merged MV+depth pass /
//           the fallback convert read them at origin + id with uv / ndc / MV scale from the FULL size (MVs stay
//           pixel deltas). At area < 100 the RCAS pass always runs (lobe 0 at sharpness 0) and blends into
//           colorIn over dlaa_area_feather px (default 96) at every rect edge that is inside the image. OUTSIDE
//           the rect the image is the raw jittered frame. Optical centre per eye from the v0.5.5 verdict readback:
//           uv = (0.5 - p/2, 0.5 + q/2), p / q = projection x / y skew (mean of the last 16 readbacks, adopted at
//           >= 4 samples, re-adopted only on a > 0.01 move = rect move = that eye's history reset); (0.5, 0.5)
//           until known and in flat mode. Live: Shift+Home = area -10, Ctrl+Home = area +10 (THREE beeps at
//           300 + 6 * area Hz, low tone at 40 / 100); the eyes rebuild their crop resources + NGX feature at the
//           next blit (history reset). Plain Home (neither Shift nor Ctrl) still toggles MVs. area= in the
//           [preset .. sharp .. r ..] tags and the config line; F10 info has area, rect and optical centre.
// v0.6.3 -- MV CANDIDATE COLLECTION THINNED. CollectMvCandidate copied the MVP of up to 128 world + ~77 cabin
//           draws per eye pass (~410 tiny GPU copies/frame); whole-frame timing blamed ~0.5 ms/eye on them. Now
//           each draw is sampled by a stable integer hash of its own (indexCount, startIndex, baseVertex): a draw
//           is a candidate only when (hash & ((1<<mv_sample_shift)-1)) == 0, so the SAME draws are picked every
//           frame (pairing stays stable) and the IB/VB fetch is skipped for the rest. Per-layer caps
//           mv_world_slots / mv_cabin_slots (default 40 / 24) replace the 128-slot LayerFull limit. dlaa.ini:
//           mv_sample_shift (0..4, default 2; 0 = every draw = old behaviour), mv_world_slots, mv_cabin_slots.
//           Flight recorder gains "skips: sampled=N" (draws dropped by the hash); "full=" now counts the caps.
// v0.6.2 -- HOOKS BOUND TO THE GAME'S RENDER CONTEXT. The v0.6.1 flight recorder showed every non-camera-change
//           PASS ANOMALY with foreign-thread OM/RS calls (1..7) and the pass's draws counted as "other": another
//           D3D11 device in the process (OpenXR runtime / compositor / overlay) calls OMSetRenderTargets /
//           RSSetViewports on ITS immediate context, and our hooks (detours on the D3D11 code, so they see every
//           context) only tested GetType() == IMMEDIATE -> g_gbufPass / g_jitterPass / g_gameVp were overwritten
//           mid-pass, the rest of that eye's G-buffer lost its viewport jitter and its MV candidates (one-eye
//           flicker every few seconds). Now the top-level Present fixes the game context (swapchain device ->
//           immediate context, plus its ID3D11DeviceContext1 identity); every context hook passes calls on any
//           other context straight through with no effect on our state (nothing is tracked before the first
//           Present). A device change is adopted after 3 consecutive top-level Presents (tracking reset, DLAA
//           forced OFF: its resources belong to the old device). G-buffer binds / blit-like draws on another
//           immediate context are logged once. Recorder: "foreign-ctx" (OM/RS/Draw/DrawIndexed on other
//           immediate contexts during the pass) and "game-ctx other-thread OM/RS" replace "foreign-thread".
//           F11 trace tags foreign immediate contexts FGN@ptr. NEW: whole-frame GPU spans under gpu_timing, also
//           with DLAA OFF ("GPU spans [...]: scene ms/frame | gbuf ms/pass", every 300 frames, window restarts
//           on any End / Ctrl+End / Shift+End / HOME / sharpness / radius change, first 30 samples skipped).
//           NEW: Ctrl+End = passive mode (no jitter, no candidate copies, no depth snapshot, no DLAA, no eye
//           readbacks; tracking + timers only), 1 low beep ON / 2 low beeps OFF. Plain End / Shift+End need Ctrl
//           NOT held.
// v0.6.1 -- INCOMPLETE CANDIDATE RECORDS no longer reset DLSS. Every ~1 in 100-200 passes the pass's MV record
//           came out short (world=0 cabin=19, world=128 cabin=0 ... vs 128/77): world-miss -> forced history
//           reset on that eye and the next one (prev = the bad record) = one big black flicker. The blit now
//           classifies each record against the eye's last GOOD counts (BAD = a layer below 50 %; none before the
//           first good record after any CameraMv::Invalidate; the 3rd bad in a row becomes the new baseline).
//           Bad: no matching / pass A, pass B with the last good R, record NOT committed (prev marked stale);
//           first good after bad: R reused once more, committed. Neither forces a reset. Flight recorder per
//           pass (DrawIndexed gbuf/other/deferred, G-buffer binds, scene clears, collector skip reasons, pass
//           age, foreign-thread OM/RS calls): one "PASS ANOMALY" line per bad record (cap 80), a "pass
//           sample" reference line at the first good pass once the eye map has 6 RTs, then every 3000 blits.
//           "MV: frame" line gains bad= / stale-reuse=. hkDrawIndexed queries the context type once per call.
// v0.6.0 -- EGO REPROJECTION. The exterior of the player's own truck (mirror housings + glass, hood, wheels) is
//           drawn in the WORLD depth layer, so it got R_world = "static scenery 1-2 m away": a huge parallax MV
//           for something fixed to the camera's vehicle; DLSS fetched history from the wrong place and the
//           mirrors flickered while driving. Pass A now also gets ALL matched world pairs (up to 128) and builds
//           R_ego = medoid of the near draws (origin < mv_ego_origin_m, default 8 m) that do NOT move like
//           R_world (Frobenius >= 0.004; none left / standing still -> R_ego = R_world), plus InvRow3 (row 3 of
//           inverse(MVP_cur) of the chosen world pair -> per-pixel view depth). Pass B reprojects world-layer
//           pixels closer than mv_ego_pixel_m (default 3 m, 0 = off) through R_ego. Solve buffer 12 -> 18 float4.
//           INSERT debug view: ego pixels blue = 0.5. Logged: ego cand/used/fallback/|R_ego-R_world| on the
//           "MV: R frame" lines, one "ego reprojection active" line; F10 info txt has R_ego, InvRow3, ego info.
// v0.5.9 -- Sharpen anti-ringing: RCAS result clamped to the min/max of its 5 taps (wide radius made black,
//           flickering halos around street lamps).
// v0.5.8 -- WIDER SHARPEN. The RCAS ring taps were sampled at exactly +-1 px, invisible on the 2x-supersampled,
//           scaled-down VR eye image. Now the 4 taps are bilinear (linear-clamp sampler at s0) at centre +-Radius
//           texels (b0, keeps the 16-byte CB) so the sharpen works at an adjustable width; the centre tap stays an
//           exact Load. New sharp_radius (float 1..4, default 1.5; dlaa.ini + s_sharpRadius) and Ctrl+PageUp/PageDown
//           change it live in 0.5 steps (TWO beeps at 400 + 250 * radius Hz). Plain PageUp/PageDown need neither
//           Shift nor Ctrl; Shift (sharpness) needs Ctrl NOT held. Radius added to the [preset .. sharp .. r ..] tags.
// v0.5.7 -- LIVE A/B in the headset + one perf trim. (a) Shift+End cycles the DLSS preset default -> E -> F -> M
//           (start = dlaa.ini dlss_preset; outside the list = default on the first press); both eyes recreate
//           their NGX feature at the next evaluate (DlaaProcessor preset generation), history reset; a failed
//           create falls back to default once. (b) Shift+PageUp / Shift+PageDown = sharpness +-0.1 (0..1, 0 =
//           pass skipped); the RCAS texture/shader always exist, the CB is DYNAMIC. Plain End / PageUp /
//           PageDown keep their old functions (Shift not held). (c) Beeps (worker thread, dlaa.ini beeps):
//           preset = N x 880 Hz (1 default, 2 E, 3 F, 4 M), sharpness = 400 + 800 * s Hz (200 Hz at a limit),
//           End = 1200 Hz ON / 300 Hz OFF. (d) Preset + sharpness appended to the "DLAA GPU cost eye" lines and
//           the FIFO stats rate; a change restarts the timing windows (next GPU line after 240 frames) and logs
//           one "rate after change" line 300 blits later (blits 60..300 measured). (e) The depth convert is
//           merged into the MV reprojection pass (kReprojDepthShader writes MV + R32F depth from the twin);
//           the old convert still runs whenever that pass does not. (f) jitter_phases default 16 -> 8.
// v0.5.6 -- MEASURE + two knobs. (a) Per-stage GPU timestamps in SceneDlaa::Run (copy-in / depth / mv / ngx /
//           sharpen / copy-back, "DLAA GPU cost eye N: total ..." every 600 timed frames per eye) and a separate
//           timestamp ring around the depth snapshot copy ("depth snapshot GPU cost", every 600), both under
//           gpu_timing. (b) CPU cost (QueryPerformanceCounter) of the matched blit path and of
//           CollectMvCandidate, plus blit rate / fps, appended to the FIFO stats line. (c) dlaa.ini
//           dlss_preset (default | J K L M E F) -> DLSS.Hint.Render.Preset.DLAA. (d) dlaa.ini sharpness
//           (0..1, default 0.4, 0 = off): FSR1 RCAS compute pass on the NGX output before the copy-back.
//           (e) The v0.5.5 eye-verdict readback is throttled to every 31st clean blit once the eye map is
//           settled (>= 6 entries, all |score| == 6); pending readbacks are still polled every blit.
// v0.5.5 -- Eye map is CORRECTED by the pass's own projection. v0.5.4 assigned an unseen blit RT the pair
//           index (s&1) of its pass and trusted it forever; in the menu/loading phase (double blits, FIFO
//           underflows) that guess slipped, so one of the 3 swapchain images per eye sat on the wrong eye
//           (R |x|>0.25 on 2 of 3 frames per eye, R_world[0][3] = 0.485 = 2x the eye skew) -> wobble. Now
//           each clean VR blit async-reads its pass's MVP candidates (8-deep staging ring, DO_NOT_WAIT);
//           sign of the projection x-skew p = -dot(row0.xyz,row3.xyz)/|row3.xyz|^2 votes the absolute eye,
//           a +-6 score per RT flips the map entry at |score| >= 2 (reset + MvInvalidate). Candidates are
//           now collected in VR even with MV off. FIFO stats line counts verdicts / corrections.
// v0.5.4 -- Eye identity is AUTHORITATIVE from the blit render target. ETS2 VR does NOT alternate the eye
//           order: the blit RT of each eye is an OpenXR swapchain image (the FIRST blit of a frame always
//           targets one of 3 textures A1..A3, the SECOND one of 3 others B1..B3). v0.5.1-v0.5.3 assumed
//           alternation, so every other frame each NGX feature / CameraMv got the opposite eye (strong
//           flicker). Now: a small ptr->eye map (unseen RT = pair index (s&1) of the consumed pass; the map is
//           authoritative afterwards). Every FIFO pass slot owns its depth twin (the discard/clear snapshot
//           goes into the slot) and its MV candidate record; at the blit the eye's NGX/CameraMv uses the
//           slot's data (cur) against the eye's previously committed record (prev). Hypotheses, EyeOfPass
//           parity, VrEyeCheck and vr_eye_alternate removed. Jitter stays per pass.
// v0.5.3 -- VR depth fix: ETS2 calls ID3D11DeviceContext1::DiscardView()/DiscardView1()/DiscardResource() on
//           the scene depth right after each eye's pass; after a discard the content is undefined (in VR ~25%
//           of the depth we handed to NGX was garbage: negative/denormal/NaN speckle). The three discards
//           are hooked (ID3D11DeviceContext1 vtable 117 DiscardResource, 118 DiscardView, 133 DiscardView1)
//           and the newest pass's depth is snapshotted BEFORE the original discard runs. Snapshot-at-clear
//           stays as a fallback. FIFO stats line counts snapshots at discard / at clear / live / discarded
//           without snapshot.
// v0.5.2 -- Present-independent pass FIFO. A "pass" starts at each scene-depth clear (after the previous pass
//           rendered into the G-buffer); each blit consumes the OLDEST unconsumed pass (the game issues
//           [clear,gbuf A][clear,gbuf B][blit A][blit B] ...). The pass carries its eye identity, Halton
//           jitter and depth-snapshot flag, so frame boundaries (mirror-window Present) no longer matter.
//           Eye identity per pass s: (s&1) ^ (alt ? (s>>1)&1 : 0) ^ parity; 4 hypotheses incl. no-alt parity 1.
// v0.5.1 -- VR eye order ALTERNATES every frame (L,R then R,L): pass/blit k maps to eye identity
//           e = k ^ alt(N); everything (NGX feature, textures, history, CameraMv, depth snapshot) is routed
//           per identity. Self-correction from the async R_world readback (|R[0][3]| > 0.25 on both eyes
//           x4 -> next mapping hypothesis). dlaa.ini vr_eye_alternate. trace_auto_frame default 0.
// v0.5.0 -- VR: per-eye DLAA. The swapchain-blit trigger is generalised (3/4-vert Draw, PS SRV0 =
//           R8G8B8A8 at scene dims, RT0 = RGBA/BGRA texture >= scene size that is not scene-sized, or
//           the DXGI backbuffer); the Nth match in a Present frame is eye N (cap 2). The shared scene
//           depth is snapshotted in the ClearDepthStencilView hook (eye 1's G-buffer pass clears eye 0's
//           depth before eye 0 is tonemapped). One SceneDlaa (own NGX feature, textures, CameraMv) per
//           eye; same Halton phase for both eyes, advanced once per Present. F10 dumps _e0/_e1.
// v0.4.3 -- F11 = frame trace probe (VR structure): ~2 frames of RT binds / viewports / draws / copies /
//           resolves / dispatches / clears into dlaa_trace.txt; also auto-runs at Present
//           #trace_auto_frame (dlaa.ini, default 0 = off). Log-only extra hooks:
//           ID3D11DeviceContext vtable 41 Dispatch, 46 CopySubresourceRegion, 47 CopyResource,
//           50 ClearRenderTargetView, 53 ClearDepthStencilView, 57 ResolveSubresource and
//           ID3D11DeviceContext1 vtable 115 CopySubresourceRegion1.
// v0.4.2 -- F10 = lossless snapshot of what goes into NGX (2 consecutive frames into dlaa_snap\:
//           color_in/out BMP, mv/depth raw float .bin, info txt incl. R_world/R_cabin).
// v0.4.1 -- MV robustness: world medoid ignores pairs closer than mv_near_reject_m (own truck),
//           16 world pairs spread over the record order, 128 candidate slots/layer,
//           INSERT = MV debug view, extra "R(moving)" logging while driving.
// v0.4.0 -- camera-reprojection motion vectors: DrawIndexed hook collects per-draw MVPs
//           (GPU copy out of the VS cbuffer ring) in the G-buffer pass, motion_vectors.cpp
//           reprojects per depth layer; HOME toggles MV, dlaa.ini mv_enabled.
// v0.3.4 -- hook point moved to the SWAPCHAIN BLIT (RT0 == backbuffer, SRV0 = scene
//           image); the old "tonemap RT" trigger matched the sun-shaft occlusion mask.
// v0.3.3 -- F9 self-test (lossless BMP frame dumps per jitter-sign mode).
// v0.3.2 -- DLAA with injected sub-pixel jitter (flat, zero motion vectors).
// Hooks (all MinHook, addresses read from a dummy device):
//   IDXGISwapChain::Present                 -- frame counter, hotkeys, logging. Hotkeys (v0.6.5, game window
//       focused; each needs its modifier EXACTLY -- Shift OR Ctrl, never both, never Alt):
//       USER  = Shift+F1..F4 DLAA model 1..4 (preset default/E/F/M, 1..4 beeps), Shift+F5/F6 DLAA area -/+10 %,
//               Shift+F7/F8 sharpen strength -/+0.1, Shift+F9/F10 sharpen width -/+0.5, Shift+F11 DLAA on/off,
//               Shift+F12 save dlss_preset/dlaa_area/sharpness/sharp_radius (v0.7.0 + dlss_upscale) to dlaa.ini
//       DEBUG = Ctrl+F4 DLSS upscale on/off (v0.7.0),
//               Ctrl+F5 MVs on/off, Ctrl+F6 MV debug view, Ctrl+F7 passive mode, Ctrl+F8 jitter-only debug,
//               Ctrl+F9 self-test, Ctrl+F10 NGX input snapshot, Ctrl+F11 frame trace, Ctrl+F12 jitter sign cycle
//   ID3D11DeviceContext::RSSetViewports     -- jitter: shifts the viewport of the
//       scene G-buffer / forward passes by a Halton sub-pixel offset (the game
//       itself renders with NO jitter, so DLAA could not anti-alias without it)
//   ID3D11DeviceContext::OMSetRenderTargets -- spots the main G-buffer pass
//       (4 RTVs + D32_FLOAT_S8X24_UINT depth >= 1024 wide), remembers its depth,
//       and classifies each bind as jitter pass / not
//   ID3D11DeviceContext::DrawIndexed        -- (v0.4) G-buffer pass only: records the draw's MVP
//       (VS cb slot 0, bytes 64..127 of its window) + geometry key for motion vectors
//   ID3D11DeviceContext::Draw               -- spots the SWAPCHAIN BLIT (full-screen
//       3/4-vertex draw whose RT0 is the DXGI backbuffer and whose PS SRV0 is an
//       R8G8B8A8 texture at scene dims = pre-UI LDR scene) and runs DLAA on that
//       SRV texture BEFORE the blit executes (scene_dlaa.cpp).
//   NOTE: VR (OpenXR, v0.5) needs a different blit target -- the eye images do
//   not go to the DXGI backbuffer.
// Findings that drive this: docs/CAPTURE_FINDINGS.md. Wiring: docs/DLAA_INTEGRATION.md.
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cwchar>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <vector>
#include <intrin.h>
#include <MinHook.h>
#include "log.h"
#ifdef WITH_DLAA
#include "scene_dlaa.h"
#include "preview_blit.h"
#include "shader_cache.h"
#endif

namespace {

typedef HRESULT (STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
Present_t             oPresent = nullptr;
std::atomic<uint64_t> g_frames{0};      // top-level Present calls only
std::atomic<uint64_t> g_reentrant{0};   // re-entrant (depth >= 1) calls
std::atomic<uint64_t> g_cuts{0};        // recursion cuts
std::atomic<uint64_t> g_failures{0};    // oPresent failure HRESULTs
thread_local int      t_depth = 0;
// Identity of the swapchain's back buffer 0 (raw pointer, compare only -- NO ref held).
std::atomic<void*>    g_backbuffer{nullptr};
// v0.7.3: its size / format (render thread only: written at Present, read at the blit).
UINT                  g_bbW = 0, g_bbH = 0;
DXGI_FORMAT           g_bbFmt = DXGI_FORMAT_UNKNOWN;
uint64_t              g_cBbByDesc = 0;    // flat blits matched by size / format instead of the pointer
// v0.8.0: the swapchain is an HDR output (R10G10B10A2_UNORM = HDR10 / PQ, R16G16B16A16_FLOAT = scRGB): with Windows HDR
// on the game switches to its float pipeline (scene -> RGBA16F composite -> backbuffer-sized RGBA16F output -> encode
// into the backbuffer) and the 8-bit blit rule can never match. Only while this is true the HDR blit rule
// (HandlePossibleBlit) is evaluated at all; each blit is still decided from its own formats. Render thread (Present).
bool                  g_hdrOut = false;
// v0.8.1: + the TYPELESS twins (a game-side backbuffer learned under a present layer is a plain texture; a real swapchain
// buffer from GetBuffer(0) is never typeless, so the normal path is unchanged).
inline bool IsHdrBackbufferFmt(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R16G16B16A16_FLOAT ||
           f == DXGI_FORMAT_R10G10B10A2_TYPELESS || f == DXGI_FORMAT_R16G16B16A16_TYPELESS;
}

constexpr int kMaxDepth = 4;

// ---- runtime state shared by Present (toggle) and the context hooks -------
// Everything below is touched from the render thread only, except g_dlaaOn.
std::atomic<bool> g_dlaaOn{false};       // DLAA + jitter live switch (Shift+F11)
bool     g_resetNext      = true;        // pass reset=true to NGX on the next evaluate
// v0.7.7: every hotkey is a re-bindable action (dlaa.ini key_*), see KeyBind / PollKeys below. One down-edge
// state per virtual-key code (g_vkWasDown) replaces the F-key-only table of v0.6.5.
bool     g_vkWasDown[256] = {};          // edge detect per VK, refreshed every top-level Present (PollKeys)
std::atomic<bool> g_mvDebug{false};      // Ctrl+F6: blit source replaced by the MV visualization
std::atomic<bool> g_mvOn{true};          // camera-reprojection MVs live switch (Ctrl+F5, dlaa.ini mv_enabled)
std::atomic<bool> g_snapRequest{false};       // set by Present on Ctrl+F10, consumed at the next DLAA blit
std::atomic<bool> g_selfTestRequest{false};   // set by Present on Ctrl+F9, consumed in hkDraw
std::atomic<bool> g_jitterOnly{false};   // Ctrl+F8: jitter stays on, DLAA evaluate skipped
// v0.6.2 (v0.6.5 Ctrl+F7): pure pass-through for cost attribution -- no jitter, no MV candidate copies, no depth snapshot,
// no DLAA evaluate, no eye readbacks; only the pass/blit tracking and the GPU span timers keep running.
std::atomic<bool> g_passive{false};
// v0.6.2: the game's render context (immediate context of the device whose swapchain the top-level Present
// presents). Identity only, no refs held. Set by hkPresent (depth 0); every context hook ignores (passes straight
// through) calls on any other context. g_gameCtx1 = the same context's ID3D11DeviceContext1 interface pointer
// (for the Discard* hooks, which receive an ID3D11DeviceContext1*). Null until the first Present.
std::atomic<ID3D11DeviceContext*> g_gameCtx{nullptr};
std::atomic<void*>                g_gameCtx1{nullptr};
std::atomic<bool> g_gameDeviceChanged{false};   // the game context was switched to a new device (DLAA stays OFF)
inline bool IsGameCtx(ID3D11DeviceContext* c) {
    return c && c == g_gameCtx.load(std::memory_order_acquire);
}
inline bool IsGameCtx1(ID3D11DeviceContext1* c) {
    return c && ((void*)c == g_gameCtx1.load(std::memory_order_acquire) ||
                 static_cast<ID3D11DeviceContext*>(c) == g_gameCtx.load(std::memory_order_acquire));
}
// ---- v0.8.1 present layer (see the header). Before g_plActive the per-frame work runs in hkPresent under g_plMx;
// the adoption (game thread) takes the same lock, so the hand-over of the render-thread state is ordered. Once
// g_plActive is set it never clears, and hkPresent touches nothing but the atomics below and the log.
std::atomic<bool>          g_plActive{false};     // present-layer mode: per-frame work on the game render thread
std::atomic<bool>          g_plSuspect{false};    // a top-level Present came from a known present-layer module
std::atomic<bool>          g_plAdopted{false};    // the game context was adopted from the scene render (evidence)
std::atomic<bool>          g_plBlockLogged{false};// the one "adoption evidence seen but BLOCKED" WARNING was written
std::mutex                 g_plMx;                // hkPresent's per-frame work (until g_plActive) vs the adoption
std::atomic<unsigned long> g_plPresentTid{0};     // thread of the last top-level Present (evidence)
std::atomic<void*>         g_plPresentCaller{nullptr};   // its return address (evidence: which module presents)
std::atomic<uint64_t>      g_plPresents{0};       // top-level Presents while g_plActive (layer thread; generated frames too)
std::atomic<uint64_t>      g_plScDesc{0};         // presenting swapchain: W | H << 20 | format << 40 (0 = unknown)
std::atomic<int64_t>       g_plActiveQpc{0};      // QPC when present-layer mode started (boundary fallback / watchdog)
std::atomic<bool>          g_prewarmDone{false};  // the NGX pre-warm ran (MaybePrewarmNgx, or v0.8.1 the early one)
char                       g_plModule[64] = "";   // present-layer module name (set under g_plMx, log only)
std::atomic<unsigned long> g_plGameTid{0};        // the game render thread in present-layer mode: EVERY boundary runs there
// Game render thread (present-layer mode) / any top-level work otherwise:
uint32_t                   g_plDraws = 0;         // game-context draws since the last game-thread frame boundary
uint64_t                   g_mkMarkers = 0;       // RSSetViewports(0 viewports) on the game context (the D3D11 runtime inside
                                                  // Present, d3d11.dll+0x196ff5 in the v0.8.1 test logs): diagnostic only
uint64_t                   g_plBoundRule = 0;     // game-thread frame boundaries from the frame-end rule (FrameEndOnBind)
uint64_t                   g_plBoundFallback = 0; // ... taken at the scene depth clear (no rule frame end for 1 s)
uint64_t                   g_plBoundIdle = 0;     // ... taken at a game bind after >= 30 ms without any (no rule frame end for 1 s)
bool                       g_plRuleDriving = false; // the last boundary came from the frame-end rule (preview needs exact frames)
enum PlBoundaryKind { kPlbFrameEnd = 0, kPlbSceneClear = 1, kPlbIdle = 2 };
inline bool PresentLayerActive() { return g_plActive.load(std::memory_order_acquire); }
inline uint64_t PlPackDesc(UINT w, UINT h, DXGI_FORMAT f) {
    return (uint64_t)(w & 0xFFFFFu) | ((uint64_t)(h & 0xFFFFFu) << 20) | ((uint64_t)(unsigned)f << 40);
}
inline UINT        PlDescW(uint64_t d) { return (UINT)(d & 0xFFFFFu); }
inline UINT        PlDescH(uint64_t d) { return (UINT)((d >> 20) & 0xFFFFFu); }
inline DXGI_FORMAT PlDescF(uint64_t d) { return (DXGI_FORMAT)(unsigned)(d >> 40); }
void OnFrameStartMarker(ID3D11DeviceContext* ctx, void* caller);    // hkRSSetViewports, game ctx, 0 viewports (diagnostic)
void LayerFrameBoundary(ID3D11DeviceContext* ctx, int kind);        // the game-thread frame boundary (PlBoundaryKind)
void FrameEndOnBind(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs);   // the frame-end rule
void FrameEndOnDraw(ID3D11DeviceContext* ctx, UINT verts, bool indexed);
void FrameEndReset();                                                // the rule's state back to "nothing seen" (adoption)
// v0.8.1: the profile-screen preview path needs exact frame boundaries (per-frame slots / jitter); under a present layer
// it only runs while the frame-end rule drives the frames (fallback boundaries can land mid-frame).
inline bool PreviewFramesOk() { return !g_plActive.load(std::memory_order_relaxed) || g_plRuleDriving; }
// v0.8.1: present-layer mode, this is the game render thread (the only thread a boundary / the frame-end rule runs on).
inline bool PlOnGameThread() {
    const unsigned long t = g_plGameTid.load(std::memory_order_relaxed);
    return t != 0 && t == GetCurrentThreadId();
}
void LayerBoundaryWatchdog();                                        // no boundary for 5 s -> one WARNING
// Game-context Draw / DrawIndexed (not our own work): feeds the frame-end rule and the fallbacks' "draws since the last
// boundary"; every 4096 draws in present-layer mode the watchdog looks at the clock.
inline void CountGameDraw(bool inDlaa, ID3D11DeviceContext* ctx, UINT verts, bool indexed) {
    if (inDlaa) return;
    FrameEndOnDraw(ctx, verts, indexed);
    if ((++g_plDraws & 4095u) == 0 && g_plActive.load(std::memory_order_relaxed)) LayerBoundaryWatchdog();
}
int      g_signX = +1, g_signY = +1;     // dlaa.ini: NGX jitter = sign * viewport shift
                                         // (+1,+1) won the F9 self-test 2026-10-03: least flicker, sharpest
bool     g_jitterEnabled  = true;        // dlaa.ini: jitter_enabled
int      g_phases         = 8;           // dlaa.ini: jitter_phases (v0.5.7: 16 -> 8 = NVIDIA's 8 x ratio for DLAA)
int      g_phase          = 0;           // Halton phase of the NEWEST pass (logging)
float    g_jx = 0.0f, g_jy = 0.0f;       // viewport shift of the NEWEST pass (G-buffer rasterization), px
// v0.7.0 DLSS upscaling (dlaa.ini dlss_upscale, Ctrl+F4 live): render at the scene size (r_scale_x / r_scale_y),
// NGX outputs at the blit RT (viewport) size and the result is composited into the eye RT after the game's blit.
// Only when the blit output is larger than the scene in at least one axis; otherwise the v0.6.5 DLAA path.
std::atomic<bool> g_dlssUpscale{true};
int      g_iniMode        = -1;          // v0.7.2 dlaa.ini `mode` (0 off, 1 DLAA, 2 DLSS; -1 = key absent)
double   g_upAreaRatio    = 1.0;         // output / render pixel count of the last matched blit (1 = no upscale)
char     g_upTag[48]      = "off";       // "WxH->WxH" / "off" for the [preset=.. up=..] log tags (last blit)
char     g_upBlocked[48]  = "";          // "WxH->WxH" whose NGX upscale init failed -> DLAA fallback (Ctrl+F4 ON clears)
int      g_effPhases      = 8;           // v0.7.0: Halton phase count in use (EffectivePhases, logged on change)
// v0.7.10: the live DLAA mode as one word for the log lines, matching the End-key cycle ("off" = DLAA evaluate
// skipped). dlss_upscale decides DLAA (native res) vs DLSS (upscale), same as CycleMode's names.
inline const char* DlaaModeStr() {
#ifdef WITH_DLAA
    if (!g_dlaaOn.load(std::memory_order_relaxed)) return "off";
    return g_dlssUpscale.load(std::memory_order_relaxed) ? "DLSS" : "DLAA";
#else
    return "off(no-DLAA-build)";
#endif
}
// v0.7.0: Halton phases per pass. Upscale: max(jitter_phases, round(jitter_phases * output/render pixel ratio))
// (DLSS guide: 8 x ratio^2), capped at 1024; DLAA: jitter_phases (v0.6.5). Jitter stays in render pixels.
int EffectivePhases() {
    if (g_upAreaRatio <= 1.0) return g_phases;
    const double p = std::floor((double)g_phases * g_upAreaRatio + 0.5);
    const int n = p > 1024.0 ? 1024 : (int)p;
    return n < g_phases ? g_phases : n;
}

// Halton low-discrepancy sequence, 1-based index.
float Halton(int index, int base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) { f /= (float)base; r += f * (float)(index % base); index /= base; }
    return r;
}
// Halton(2,3) jitter of a phase, mapped to [-0.5, +0.5).
void JitterOfPhase(int phase, float* jx, float* jy) {
    *jx = Halton(phase + 1, 2) - 0.5f;
    *jy = Halton(phase + 1, 3) - 0.5f;
}
bool IsForeground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
// ---- v0.7.7 re-bindable hotkeys ---------------------------------------------------------------------------
// Every action has a binding "[Shift+][Ctrl+][Alt+]Key" (dlaa.ini key_<action>, defaults below = the v0.7.6 keys).
// A binding fires on the key's DOWN-EDGE only, game in the foreground, with EXACTLY its modifiers held (a binding
// without modifiers fires only when none of Shift/Ctrl/Alt is down). vk == 0 = disabled ("none").
enum KeyAction {
    KA_MODE_CYCLE, KA_MODEL1, KA_MODEL2, KA_MODEL3, KA_MODEL4, KA_AREA_DOWN, KA_AREA_UP, KA_SHARPEN_DOWN,
    KA_SHARPEN_UP, KA_WIDTH_DOWN, KA_WIDTH_UP, KA_DLAA_TOGGLE, KA_SAVE, KA_UPSCALE_TOGGLE, KA_MV_TOGGLE,
    KA_MV_DEBUG, KA_PASSIVE, KA_JITTER_ONLY, KA_SELFTEST, KA_SNAPSHOT, KA_TRACE, KA_JITTER_SIGN, KA_COUNT
};
enum { MOD_SHIFT_BIT = 1, MOD_CTRL_BIT = 2, MOD_ALT_BIT = 4 };
struct KeyBind {
    const char* ini;          // dlaa.ini key
    const char* def;          // default binding text
    int         vk;           // 0 = disabled
    unsigned    mods;         // MOD_*_BIT
    char        name[48];     // binding as shown in logs
    bool        hit;          // down-edge this Present (PollKeys)
};
KeyBind g_keys[KA_COUNT] = {
    { "key_mode_cycle",      "End",       0, 0, "", false },
    { "key_model_1",         "Shift+F1",  0, 0, "", false },
    { "key_model_2",         "Shift+F2",  0, 0, "", false },
    { "key_model_3",         "Shift+F3",  0, 0, "", false },
    { "key_model_4",         "Shift+F4",  0, 0, "", false },
    { "key_area_down",       "Shift+F5",  0, 0, "", false },
    { "key_area_up",         "Shift+F6",  0, 0, "", false },
    { "key_sharpen_down",    "Shift+F7",  0, 0, "", false },
    { "key_sharpen_up",      "Shift+F8",  0, 0, "", false },
    { "key_width_down",      "Shift+F9",  0, 0, "", false },
    { "key_width_up",        "Shift+F10", 0, 0, "", false },
    { "key_dlaa_toggle",     "Shift+F11", 0, 0, "", false },
    { "key_save",            "Shift+F12", 0, 0, "", false },
    { "key_upscale_toggle",  "Ctrl+F4",   0, 0, "", false },
    { "key_mv_toggle",       "Ctrl+F5",   0, 0, "", false },
    { "key_mv_debug",        "Ctrl+F6",   0, 0, "", false },
    { "key_passive",         "Ctrl+F7",   0, 0, "", false },
    { "key_jitter_only",     "Ctrl+F8",   0, 0, "", false },
    { "key_selftest",        "Ctrl+F9",   0, 0, "", false },
    { "key_snapshot",        "Ctrl+F10",  0, 0, "", false },
    { "key_trace",           "Ctrl+F11",  0, 0, "", false },
    { "key_jitter_sign",     "Ctrl+F12",  0, 0, "", false },
};
inline bool KeyHit(KeyAction a) { return g_keys[a].hit; }
inline const char* KeyName(KeyAction a) { return g_keys[a].name; }

// Parses one binding ("Ctrl+Shift+F5", case-insensitive, spaces ignored). "" / "none" -> vk 0 (disabled), true.
bool ParseBinding(const char* text, int* vkOut, unsigned* modsOut) {
    char buf[64]; int n = 0;
    for (const char* c = text; *c && n < (int)sizeof(buf) - 1; ++c)
        if (*c != ' ' && *c != '\t' && *c != '\r' && *c != '\n') buf[n++] = (char)tolower((unsigned char)*c);
    buf[n] = 0;
    *vkOut = 0; *modsOut = 0;
    if (!n || !strcmp(buf, "none")) return true;
    unsigned mods = 0;
    const char* p = buf;
    for (;;) {                                           // peel leading "shift+" / "ctrl+" / "alt+" (the key stays last)
        if      (!strncmp(p, "shift+", 6)) { mods |= MOD_SHIFT_BIT; p += 6; }
        else if (!strncmp(p, "ctrl+", 5))  { mods |= MOD_CTRL_BIT;  p += 5; }
        else if (!strncmp(p, "alt+", 4))   { mods |= MOD_ALT_BIT;   p += 4; }
        else break;
    }
    if (!*p) return false;
    int vk = 0;
    const size_t len = strlen(p);
    if (len == 1 && p[0] >= 'a' && p[0] <= 'z') vk = 'A' + (p[0] - 'a');
    else if (len == 1 && p[0] >= '0' && p[0] <= '9') vk = p[0];
    else if (p[0] == 'f' && len >= 2 && len <= 3 && isdigit((unsigned char)p[1]) && (len == 2 || isdigit((unsigned char)p[2]))) {
        const int f = atoi(p + 1);
        if (f >= 1 && f <= 24) vk = VK_F1 + f - 1;
    }
    else if (!strncmp(p, "numpad", 6) && len == 7 && isdigit((unsigned char)p[6])) vk = VK_NUMPAD0 + (p[6] - '0');
    else {
        static const struct { const char* name; int vk; } kNames[] = {
            { "home", VK_HOME }, { "end", VK_END }, { "insert", VK_INSERT }, { "delete", VK_DELETE },
            { "pageup", VK_PRIOR }, { "pagedown", VK_NEXT }, { "up", VK_UP }, { "down", VK_DOWN },
            { "left", VK_LEFT }, { "right", VK_RIGHT }, { "space", VK_SPACE }, { "tab", VK_TAB },
            { "backspace", VK_BACK }, { "enter", VK_RETURN }, { "pause", VK_PAUSE },
            { "numpadadd", VK_ADD }, { "numpadsubtract", VK_SUBTRACT }, { "numpadmultiply", VK_MULTIPLY },
            { "numpaddivide", VK_DIVIDE }, { "numpaddecimal", VK_DECIMAL },
            { "[", VK_OEM_4 }, { "]", VK_OEM_6 }, { ";", VK_OEM_1 }, { "'", VK_OEM_7 }, { ",", VK_OEM_COMMA },
            { ".", VK_OEM_PERIOD }, { "/", VK_OEM_2 }, { "\\", VK_OEM_5 }, { "-", VK_OEM_MINUS },
            { "=", VK_OEM_PLUS }, { "`", VK_OEM_3 },
        };
        for (const auto& k : kNames) if (!strcmp(p, k.name)) { vk = k.vk; break; }
    }
    if (!vk) return false;
    *vkOut = vk; *modsOut = mods;
    return true;
}

// Resets every binding to its default (called first in LoadConfig).
void InitKeyBinds() {
    for (KeyBind& k : g_keys) {
        ParseBinding(k.def, &k.vk, &k.mods);
        snprintf(k.name, sizeof(k.name), "%s", k.def);
    }
}
// Applies one dlaa.ini "key_*" line. Unknown key_ names are ignored silently (like every unknown ini key).
void SetKeyBindFromIni(const char* key, const char* value) {
    for (KeyBind& k : g_keys) {
        if (strcmp(key, k.ini)) continue;
        char trimmed[48] = {}; int n = 0;                // trimmed raw text, used as the display name
        const char* s = value;
        while (*s == ' ' || *s == '\t') ++s;
        for (; *s && *s != '\r' && *s != '\n' && n < (int)sizeof(trimmed) - 1; ++s) trimmed[n++] = *s;
        while (n > 0 && (trimmed[n - 1] == ' ' || trimmed[n - 1] == '\t')) trimmed[--n] = 0;
        int vk; unsigned mods;
        if (ParseBinding(trimmed, &vk, &mods)) {
            k.vk = vk; k.mods = mods;
            snprintf(k.name, sizeof(k.name), "%s", vk ? trimmed : "none");
        } else {
            Log("dlaa.ini: %s '%s' not recognised (modifiers Shift/Ctrl/Alt + one key, or none) -- keeping %s",
                k.ini, trimmed, k.name);
        }
        return;
    }
}
// Logs the final binding table once and warns about two actions on the same combo.
void LogKeyBinds() {
    char line[512]; int len = 0;
    for (int i = 0; i < KA_COUNT; ++i) {
        char item[96];
        const int w = snprintf(item, sizeof(item), "%s=%s", g_keys[i].ini + 4, g_keys[i].vk ? g_keys[i].name : "none");
        if (len + w + 2 >= (int)sizeof(line)) { Log("hotkeys: %s", line); len = 0; }
        len += snprintf(line + len, sizeof(line) - (size_t)len, "%s%s", len ? " " : "", item);
    }
    if (len) Log("hotkeys: %s", line);
    for (int i = 0; i < KA_COUNT; ++i)
        for (int j = i + 1; j < KA_COUNT; ++j)
            if (g_keys[i].vk && g_keys[i].vk == g_keys[j].vk && g_keys[i].mods == g_keys[j].mods)
                Log("dlaa.ini: WARNING %s and %s share the binding %s -- both will fire", g_keys[i].ini, g_keys[j].ini,
                    g_keys[i].name);
}
// Once per top-level Present: fills KeyBind::hit. The per-VK was-down state is refreshed every call for every bound
// key, foreground or not, so a key released while the game is not focused is not later seen as a fresh press.
void PollKeys() {
    const bool fg = IsForeground();
    const unsigned held = ((GetAsyncKeyState(VK_SHIFT)   & 0x8000) ? MOD_SHIFT_BIT : 0u) |
                          ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? MOD_CTRL_BIT  : 0u) |
                          ((GetAsyncKeyState(VK_MENU)    & 0x8000) ? MOD_ALT_BIT   : 0u);
    bool down[KA_COUNT];
    for (int i = 0; i < KA_COUNT; ++i) {
        KeyBind& k = g_keys[i];
        k.hit = false;
        down[i] = k.vk && (GetAsyncKeyState(k.vk) & 0x8000) != 0;
        if (down[i] && !g_vkWasDown[k.vk & 0xFF] && fg && held == k.mods) k.hit = true;
    }
    for (int i = 0; i < KA_COUNT; ++i)                   // update after all edges are computed (bindings may share a VK)
        if (g_keys[i].vk) g_vkWasDown[g_keys[i].vk & 0xFF] = down[i];
}

// Writes "module.dll+0xOFFSET" for the module containing addr (or "?" + addr).
void DescribeAddr(void* addr, char* buf, size_t cap) {
    HMODULE mod = nullptr;
    if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(addr), &mod) && mod) {
        wchar_t wpath[MAX_PATH]{};
        if (GetModuleFileNameW(mod, wpath, MAX_PATH)) {
            const wchar_t* base = wcsrchr(wpath, L'\\');
            base = base ? base + 1 : wpath;
            char name[MAX_PATH]{};
            WideCharToMultiByte(CP_UTF8, 0, base, -1, name, (int)sizeof(name) - 1, nullptr, nullptr);
            snprintf(buf, cap, "%s+0x%llx", name,
                     (unsigned long long)((uintptr_t)addr - (uintptr_t)mod));
            return;
        }
    }
    snprintf(buf, cap, "?(%p)", addr);
}
// v0.8.1: base name ("NvPresent64.dll") of the module containing addr; false (out = "") when there is none.
bool ModuleBaseOf(void* addr, char* out, size_t cap) {
    out[0] = 0;
    HMODULE mod = nullptr;
    if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCWSTR>(addr), &mod) || !mod) return false;
    wchar_t wpath[MAX_PATH]{};
    if (!GetModuleFileNameW(mod, wpath, MAX_PATH)) return false;
    const wchar_t* base = wcsrchr(wpath, L'\\');
    base = base ? base + 1 : wpath;
    WideCharToMultiByte(CP_UTF8, 0, base, -1, out, (int)cap - 1, nullptr, nullptr);
    out[cap - 1] = 0;
    return out[0] != 0;
}
// v0.8.1: a known driver frame-generation present layer (NVIDIA Smooth Motion: NvPresent64.dll / NvPresent.dll). It
// presents its OWN swapchain from its own device and thread; the game's render device is never the presenting one.
inline bool IsPresentLayerName(const char* base) { return base && _strnicmp(base, "nvpresent", 9) == 0; }
// v0.8.1: base name of the game exe (compare against a Present caller module).
const char* GameExeBase() {
    static char s_exe[MAX_PATH] = "";
    if (!s_exe[0]) {
        char path[MAX_PATH] = "";
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        const char* b = strrchr(path, '\\');
        strncpy_s(s_exe, b ? b + 1 : path, _TRUNCATE);
    }
    return s_exe;
}

// ---- v0.5.7 audible feedback (the user is in a headset) -----------------------------------------------
// Beep sequences play on a short-lived worker thread (kernel32 Beep blocks for its duration), never on the
// render thread. The worker touches nothing but this tiny mutex-guarded hand-off: while a sequence plays, at
// most ONE more is queued (the newest replaces an older queued one). No D3D, no other shared state.
bool     g_beeps          = true;        // dlaa.ini: beeps (0/1)
struct BeepSeq {
    int   n = 0;                         // tones (<= kMax)
    static constexpr int kMax = 6;
    DWORD freq[kMax] = {};
    DWORD ms[kMax]   = {};
    DWORD gapMs      = 100;              // silence between tones
};
std::mutex g_beepMx;
bool       g_beepBusy = false;           // a worker thread is playing (guarded by g_beepMx)
bool       g_beepHasNext = false;        // g_beepNext is queued (guarded by g_beepMx)
BeepSeq    g_beepNext;

DWORD WINAPI BeepThread(LPVOID param) {
    BeepSeq seq = *static_cast<BeepSeq*>(param);
    delete static_cast<BeepSeq*>(param);
    for (;;) {
        for (int i = 0; i < seq.n; ++i) {
            if (i) Sleep(seq.gapMs);
            Beep(seq.freq[i], seq.ms[i]);
        }
        std::lock_guard<std::mutex> lk(g_beepMx);
        if (!g_beepHasNext) { g_beepBusy = false; return 0; }
        seq = g_beepNext;
        g_beepHasNext = false;
    }
}
// Non-blocking (the lock is only ever held for a few instructions).
void PlayBeeps(const BeepSeq& s) {
    if (!g_beeps || s.n <= 0) return;
    std::lock_guard<std::mutex> lk(g_beepMx);
    if (g_beepBusy) { g_beepNext = s; g_beepHasNext = true; return; }
    BeepSeq* p = new (std::nothrow) BeepSeq(s);
    if (!p) return;
    HANDLE h = CreateThread(nullptr, 0, BeepThread, p, 0, nullptr);
    if (!h) { delete p; return; }
    CloseHandle(h);                                     // detached
    g_beepBusy = true;
}
// `count` equal tones.
void PlayTones(int count, DWORD freq, DWORD ms, DWORD gapMs) {
    BeepSeq s;
    s.n = count < 0 ? 0 : (count > BeepSeq::kMax ? BeepSeq::kMax : count);
    for (int i = 0; i < s.n; ++i) { s.freq[i] = freq; s.ms[i] = ms; }
    s.gapMs = gapMs;
    PlayBeeps(s);
}

void MvInvalidate();                     // defined with the scene state below
void OnPresentBoundary(uint64_t n);      // per-Present frame reset (eye counters, jitter phase)
bool g_logTick = false;                  // v0.7.10: this Present prints the periodic log lines (first frames + every 10 s of wall clock)
void TraceAtPresent(IDXGISwapChain* sc, uint64_t n);   // F11 frame trace, defined below
extern std::atomic<bool> g_traceRequest; // v0.6.5: set by the Ctrl+F11 handler in hkPresent, defined with the trace state below
bool UpdateGameContext(IDXGISwapChain* sc, uint64_t n, void* caller); // v0.6.2: g_gameCtx from the presenting device
                                         // (v0.8.1: true = a present layer took over, present-layer mode is now on)
void LayerBackbufferAtBoundary(ID3D11DeviceContext* ctx, uint64_t n, bool fromRule); // v0.8.1 game-side backbuffer
void ResetSpanWindow();                  // v0.6.2: restart the "GPU spans" averaging window
void TogglePassive(uint64_t n);          // v0.6.2 (v0.6.5 Ctrl+F7)
#ifdef WITH_DLAA
void SelectDlssPreset(int idx, uint64_t n); // v0.6.5 Shift+F1..F4 direct model select (0=default,1=E,2=F,3=M)
void StepSharpness(int dir, uint64_t n); // v0.5.7 (v0.6.5 Shift+F7 -1 / Shift+F8 +1)
void StepSharpRadius(int dir, uint64_t n); // v0.5.8 (v0.6.5 Shift+F9 -1 / Shift+F10 +1)
void StepArea(int dir, uint64_t n);      // v0.6.4 (v0.6.5 Shift+F5 -1 / Shift+F6 +1)
void CycleMode(uint64_t n);              // v0.7.2 plain End: DLAA -> DLSS -> off, saved to dlaa.ini (mode)
void SaveSettings(uint64_t n);           // v0.6.5 Shift+F12: write the 4 live values (v0.7.0: + dlss_upscale) to dlaa.ini
void CheckPresetFallback(uint64_t n);    // v0.5.7: beep + log when a live preset fell back to default
void OnLiveTuningChange();               // v0.5.7; v0.6.2 also DLAA toggle / passive / MV (restarts the GPU spans window)
void PreviewInvalidate();                // v0.7.8 profile-screen preview slots: drop their MV / DLSS history
void PreviewNextFrame(uint64_t n);       // v0.7.8 per-Present preview bookkeeping (slot counter, jitter, stats)
void MaybePrewarmNgx();                  // v0.7.8 NGX pre-warm at the first Present with DLAA on
#endif

// v0.7.10: the periodic log cadence -- by wall clock, not frame count (loading screens Present at 1000+ fps and flooded
// the log). v0.8.1: one function for both frame drivers (hkPresent, or the game-thread boundary under a present layer;
// never both at once, see g_plMx).
bool ComputeLogTick(uint64_t n) {
    static ULONGLONG s_lastLogTick = 0;
    const ULONGLONG nowTick = GetTickCount64();
    const bool tick = n < 5 || nowTick - s_lastLogTick >= 10000;
    if (tick) s_lastLogTick = nowTick;
    return tick;
}

// v0.7.3 / v0.8.0 backbuffer identity + size / format + HDR output state (v0.8.1: one place for both frame drivers).
// bb == nullptr keeps the current identity (only the desc is known). The HDR line is logged on every change (cap), the
// first one is the "HDR seen" line a bug report needs.
void ApplyBackbufferDesc(void* bb, UINT w, UINT h, DXGI_FORMAT f, uint64_t n) {
    if (bb) g_backbuffer.store(bb, std::memory_order_relaxed);
    g_bbW = w; g_bbH = h; g_bbFmt = f;
    const bool hdrNow = IsHdrBackbufferFmt(f);
    if (hdrNow != g_hdrOut) {
        g_hdrOut = hdrNow;
        static int hdrLogs = 0;
        if (hdrLogs++ < 10)
            Log("%s: backbuffer %ux%u fmt=%d (%s) at Present #%llu -- %s", hdrNow ? "HDR output detected" : "HDR output off",
                w, h, (int)f,
                (f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R10G10B10A2_TYPELESS) ? "R10G10B10A2_UNORM, HDR10" :
                (f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16G16B16A16_TYPELESS) ? "R16G16B16A16_FLOAT, scRGB" : "8-bit",
                (unsigned long long)n,
                hdrNow ? "HDR blit rule in use: the world blit is the draw that copies the game's RGBA16F scene "
                         "composite into its backbuffer-sized RGBA16F output target (DLAA/DLSS run on that RGBA16F "
                         "scene, see the config line's dlss_hdr); the 8-bit rule stays for any 8-bit blit"
                       : "8-bit blit rule only (the v0.7.x behaviour)");
    }
}
// Refresh the back buffer identity (cheap; GetBuffer(0) is stable but can change on ResizeBuffers). Release the ref at
// once: the pointer is only compared.
void RefreshBackbufferFromSwapchain(IDXGISwapChain* sc, uint64_t n) {
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
        D3D11_TEXTURE2D_DESC bbd{};                  // v0.7.3: size / format for the flat blit match
        bb->GetDesc(&bbd);
        bb->Release();
        ApplyBackbufferDesc(bb, bbd.Width, bbd.Height, bbd.Format, n);
    }
}

// The per-frame work (v0.8.1: was the body of hkPresent at depth 0). Normal mode: hkPresent, sc = the presenting
// swapchain, under g_plMx. Present-layer mode: the game render thread at the frame boundary (LayerFrameBoundary), sc =
// nullptr, bctx = the game context, fromRule = the boundary is the frame-end rule (between two frames, the frame's last
// target still bound: the game-side backbuffer identity is taken from it).
void PresentFrameWork(IDXGISwapChain* sc, uint64_t n, ID3D11DeviceContext* bctx, bool fromRule) {
#ifdef WITH_DLAA
    MaybePrewarmNgx();                          // v0.7.8: NGX init + first feature on the boot screen (once)
#endif
    {                                           // v0.7.7 re-bindable hotkeys (dlaa.ini key_*, see PollKeys)
        PollKeys();                              // down-edges for this Present (exact modifiers, foreground only)

        // frame trace key. Handled before TraceAtPresent so the request is consumed this same frame.
        if (KeyHit(KA_TRACE)) {
            g_traceRequest = true;
            Log("frame trace requested (%s, Present #%llu)", KeyName(KA_TRACE), (unsigned long long)n);
        }
        TraceAtPresent(sc, n);                   // F11 frame-trace state machine (v0.4.3)
#ifdef WITH_DLAA
        CheckPresetFallback(n);                  // v0.5.7: a live preset create failed -> told by ear
#endif

        // ---- user keys -------------------------------------------------------------------------------
        if (KeyHit(KA_DLAA_TOGGLE)) {            // DLAA on/off (old plain-End behaviour + beeps)
#ifdef WITH_DLAA
            if (g_gameDeviceChanged && !g_dlaaOn.load()) {
                Log("DLAA stays OFF (%s, Present #%llu): the game render device changed, our NGX feature / "
                    "textures / queries belong to the old device -- restart the game", KeyName(KA_DLAA_TOGGLE), (unsigned long long)n);
                PlayTones(1, 200, 400, 0);
            } else {
                const bool on = !g_dlaaOn.load();
                g_dlaaOn = on;
                if (on) { g_resetNext = true; MvInvalidate(); }   // drop DLSS history + stale MV candidates on re-enable
                Log("DLAA %s (%s, Present #%llu)", on ? "ON" : "OFF", KeyName(KA_DLAA_TOGGLE), (unsigned long long)n);
                PlayTones(1, on ? 1200 : 300, 150, 0);
                OnLiveTuningChange();            // v0.6.2: restart the timing windows (GPU spans: on/off compare)
            }
#else
            Log("%s ignored: DLAA not compiled in", KeyName(KA_DLAA_TOGGLE));
#endif
        }
#ifdef WITH_DLAA
        if (KeyHit(KA_MODE_CYCLE)) CycleMode(n); // v0.7.2 mode cycle DLAA -> DLSS -> off (default End)
        if      (KeyHit(KA_MODEL1))       SelectDlssPreset(0, n);  // model 1 = default (driver pick) -- 1 beep
        else if (KeyHit(KA_MODEL2))       SelectDlssPreset(1, n);  // model 2 = E                      -- 2 beeps
        else if (KeyHit(KA_MODEL3))       SelectDlssPreset(2, n);  // model 3 = F                      -- 3 beeps
        else if (KeyHit(KA_MODEL4))       SelectDlssPreset(3, n);  // model 4 = M                      -- 4 beeps
        else if (KeyHit(KA_AREA_DOWN))    StepArea(-1, n);         // DLAA area -10 %
        else if (KeyHit(KA_AREA_UP))      StepArea(+1, n);         // DLAA area +10 %
        else if (KeyHit(KA_SHARPEN_DOWN)) StepSharpness(-1, n);    // sharpen strength -0.1
        else if (KeyHit(KA_SHARPEN_UP))   StepSharpness(+1, n);    // sharpen strength +0.1
        else if (KeyHit(KA_WIDTH_DOWN))   StepSharpRadius(-1, n);  // sharpen width -0.5
        else if (KeyHit(KA_WIDTH_UP))     StepSharpRadius(+1, n);  // sharpen width +0.5
        else if (KeyHit(KA_SAVE))         SaveSettings(n);         // save the 4 live values + dlss_upscale to dlaa.ini

        // ---- debug keys ------------------------------------------------------------------------------
        {
            if (KeyHit(KA_UPSCALE_TOGGLE)) {                         // v0.7.0 DLSS upscale on/off
                const bool on = !g_dlssUpscale.load();
                g_dlssUpscale = on;
                if (on) g_upBlocked[0] = 0;      // a failed upscale size is retried
                g_resetNext = true;              // each eye also rebuilds its textures + NGX feature at its next blit
                Log("DLSS upscale %s (%s, Present #%llu)%s", on ? "ON" : "OFF", KeyName(KA_UPSCALE_TOGGLE), (unsigned long long)n,
                    on ? " -- render (scene) res -> blit RT res whenever the blit RT is larger"
                       : " -- DLAA at render res, copied back over the blit source (v0.6.5 path)");
                PlayTones(1, on ? 1200 : 300, 150, 0);
                OnLiveTuningChange();            // timing windows restart (new pipeline shape)
            }
            if (KeyHit(KA_MV_TOGGLE)) {                         // motion vectors on/off (old HOME)
                const bool on = !g_mvOn.load();
                g_mvOn = on;
                g_resetNext = true;
                MvInvalidate();                  // forget stale candidates either way
                Log("motion vectors %s (%s, Present #%llu)", on ? "ON (camera reprojection)" : "OFF (zero MVs)",
                    KeyName(KA_MV_TOGGLE), (unsigned long long)n);
                OnLiveTuningChange();            // v0.6.2: MV state is part of the GPU spans window key
            }
            if (KeyHit(KA_MV_DEBUG)) {                         // MV debug view (old INSERT)
                const bool on = !g_mvDebug.load();
                g_mvDebug = on;
                Log("MV debug view %s (%s, Present #%llu)", on ? "ON (R=mv.x G=mv.y B=cabin; gray = still)" : "OFF",
                    KeyName(KA_MV_DEBUG), (unsigned long long)n);
            }
            if (KeyHit(KA_PASSIVE)) TogglePassive(n);         // Ctrl+F7 passive mode (old Ctrl+End)
            if (KeyHit(KA_JITTER_ONLY)) {                         // jitter-only debug (old plain PageUp)
                const bool jo = !g_jitterOnly.load();
                g_jitterOnly = jo;
                g_resetNext = true;              // also resets on the way back to DLAA
                Log("jitter-only debug %s (%s, Present #%llu)", jo ? "ON (DLAA evaluate skipped)" : "OFF",
                    KeyName(KA_JITTER_ONLY), (unsigned long long)n);
            }
            if (KeyHit(KA_SELFTEST)) {                         // self-test (old F9)
                g_selfTestRequest = true;
                Log("self-test requested (%s, Present #%llu)", KeyName(KA_SELFTEST), (unsigned long long)n);
            }
            if (KeyHit(KA_SNAPSHOT)) {                         // NGX input snapshot (old F10)
                g_snapRequest = true;
                Log("NGX input snapshot requested (%s, Present #%llu)", KeyName(KA_SNAPSHOT), (unsigned long long)n);
            }
            if (KeyHit(KA_JITTER_SIGN)) {                        // cycle NGX jitter sign (old plain PageDown)
                static const int kCycle[4][2] = { {-1,-1}, {+1,+1}, {+1,-1}, {-1,+1} };
                int cur = 0;
                for (int i = 0; i < 4; ++i)
                    if (kCycle[i][0] == g_signX && kCycle[i][1] == g_signY) cur = i;
                const int nx = (cur + 1) % 4;
                g_signX = kCycle[nx][0]; g_signY = kCycle[nx][1];
                g_resetNext = true;
                PreviewInvalidate();             // v0.7.8: the preview slots drop their history too (own reset epoch)
                Log("jitter sign now X=%+d Y=%+d (%s)", g_signX, g_signY, KeyName(KA_JITTER_SIGN));
            }
        }
#endif
    }
    // DLAA does NOT run here any more: it runs inside hkDraw on the tonemap
    // draw (pre-UI), see above. Present just counts frames for that logic.
    // ---- roadmap ---------------------------------------------------------
    // v0.4: camera-reprojection motion vectors (done, motion_vectors.cpp).
    // v0.5: VR -- eye targets via the same device-context hooks.
    // v0.6: ImGui overlay (HOME / Ctrl+P) for live tuning + preset menu.
    // ----------------------------------------------------------------------
    if (sc) RefreshBackbufferFromSwapchain(sc, n);                       // normal: the presenting swapchain IS the game's
    else if (bctx) LayerBackbufferAtBoundary(bctx, n, fromRule);         // v0.8.1 present layer: the game side
    OnPresentBoundary(n);
}

// v0.8.1: top-level Present in present-layer mode (the layer's own present thread, real AND generated frames). Touches
// nothing but atomics and the log: counts, remembers the presenting swapchain's desc for the game-side backbuffer logic,
// one line every 10 s. The game's frames are counted on the game render thread (LayerFrameBoundary -> g_frames).
HRESULT LayerPresent(IDXGISwapChain* sc, UINT sync, UINT flags, void* caller, unsigned long tid) {
    const uint64_t k = g_plPresents.fetch_add(1, std::memory_order_relaxed);
    DXGI_SWAP_CHAIN_DESC d{};
    const bool haveDesc = SUCCEEDED(sc->GetDesc(&d));
    if (haveDesc)
        g_plScDesc.store(PlPackDesc(d.BufferDesc.Width, d.BufferDesc.Height, d.BufferDesc.Format), std::memory_order_relaxed);
    static std::atomic<ULONGLONG> s_last{0};
    const ULONGLONG now = GetTickCount64();
    ULONGLONG last = s_last.load(std::memory_order_relaxed);
    if (k < 3 || (now - last >= 10000 && s_last.compare_exchange_strong(last, now))) {
        if (k < 3) s_last.store(now, std::memory_order_relaxed);
        char where[MAX_PATH + 32];
        DescribeAddr(caller, where, sizeof(where));
        Log("Present (present layer) #%llu  %ux%u fmt=%d sync=%u flags=0x%x tid=%lu caller=%s -- game frames so far %llu "
            "(counted on the game render thread)", (unsigned long long)k, haveDesc ? d.BufferDesc.Width : 0u,
            haveDesc ? d.BufferDesc.Height : 0u, haveDesc ? (int)d.BufferDesc.Format : 0, sync, flags, tid, where,
            (unsigned long long)g_frames.load(std::memory_order_relaxed));
    }
    ++t_depth;
    const HRESULT hr = oPresent(sc, sync, flags);
    --t_depth;
    if (FAILED(hr)) {
        const uint64_t f = g_failures.fetch_add(1);
        if (f < 20)
            Log("oPresent failed hr=0x%lx (depth=0 tid=%lu, present layer)", (unsigned long)hr, tid);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    const int   depth  = t_depth;
    void* const caller = _ReturnAddress();
    char where[MAX_PATH + 32];
    const unsigned long tid = GetCurrentThreadId();

    if (depth >= kMaxDepth) {                   // safety: break the loop
        const uint64_t c = g_cuts.fetch_add(1);
        g_reentrant.fetch_add(1);
        if (c < 5) {
            DescribeAddr(caller, where, sizeof(where));
            Log("recursion cut at depth %d (tid=%lu caller=%s)", depth, tid, where);
        }
        return S_OK;
    }

    if (depth >= 1) {
        // Re-entered (e.g. via an overlay detour): no DLAA work, pass through.
        const uint64_t r = g_reentrant.fetch_add(1);
        if (r < 20) {
            DescribeAddr(caller, where, sizeof(where));
            Log("re-entrant Present #%llu depth=%d tid=%lu caller=%s",
                (unsigned long long)r, depth, tid, where);
        }
        return oPresent(sc, sync, flags);
    }
    // ---- top level (depth 0) ----
    g_plPresentTid.store(tid, std::memory_order_relaxed);        // v0.8.1: present-layer evidence (who presents)
    g_plPresentCaller.store(caller, std::memory_order_relaxed);
    if (PresentLayerActive()) return LayerPresent(sc, sync, flags, caller, tid);   // v0.8.1: count + log only
    // v0.8.1: the per-frame work runs under g_plMx until present-layer mode starts (the adoption on the game thread
    // takes the same lock, so it never overlaps this). Uncontended in normal operation.
    std::unique_lock<std::mutex> plLock(g_plMx);
    if (g_plActive.load(std::memory_order_relaxed)) {           // adopted while we waited for the lock
        plLock.unlock();
        return LayerPresent(sc, sync, flags, caller, tid);
    }
    const uint64_t n = g_frames.fetch_add(1);
    g_logTick = ComputeLogTick(n);
    if (g_logTick) {                            // first frames + every 10 s
        DescribeAddr(caller, where, sizeof(where));
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d)))
            Log("Present #%llu  %ux%u fmt=%d sync=%u flags=0x%x tid=%lu caller=%s reentrant=%llu",
                (unsigned long long)n, d.BufferDesc.Width, d.BufferDesc.Height,
                (int)d.BufferDesc.Format, sync, flags, tid, where,
                (unsigned long long)g_reentrant.load());
        else
            Log("Present #%llu sync=%u flags=0x%x tid=%lu caller=%s reentrant=%llu",
                (unsigned long long)n, sync, flags, tid, where,
                (unsigned long long)g_reentrant.load());
    }
    // v0.7.10: once, at the first top-level Present -- note if Present is reached through a driver / overlay present
    // layer (not the game exe or dxgi). `where` is "module+0xoffset" (just set by DescribeAddr, since n==0 < 5).
    if (n == 0) {
        char mod[MAX_PATH];
        size_t i = 0;
        for (; where[i] && where[i] != '+' && i < sizeof(mod) - 1; ++i) mod[i] = where[i];
        mod[i] = 0;
        const char* eb = GameExeBase();
        if (_stricmp(mod, eb) != 0 && _stricmp(mod, "dxgi.dll") != 0)
            Log("note: Present is called through %s (driver/overlay present layer), not %s / dxgi.dll -- the "
                "caller= values above are that layer, not the game", mod, eb);
        // v0.8.1: a known frame-generation present layer presents its own swapchain from its own device: the game
        // context taken below is most likely NOT the game's render context. Nothing is device-bound until the scene
        // render shows which context is the game's (adoption) -- the NGX pre-warm waits for it.
        if (IsPresentLayerName(mod)) {
            g_plSuspect.store(true, std::memory_order_relaxed);
            strncpy_s(g_plModule, mod, _TRUNCATE);
            Log("present layer: %s is a known driver frame-generation present layer (NVIDIA Smooth Motion) -- it presents "
                "its own swapchain from its own device and thread (tid=%lu); the game's render context is taken from the "
                "scene render once it is seen (see the 'present layer' lines), the NGX pre-warm waits for it", mod, tid);
        }
    }
    if (!UpdateGameContext(sc, n, caller))      // v0.6.2: before anything else this frame (hotkeys may reset state)
        PresentFrameWork(sc, n, nullptr, false);// v0.8.1: false = a present layer took over this very Present
    plLock.unlock();
    ++t_depth;
    const HRESULT hr = oPresent(sc, sync, flags);
    --t_depth;
    if (n < 5)
        Log("Present #%llu oPresent returned hr=0x%lx", (unsigned long long)n, (unsigned long)hr);
    if (FAILED(hr)) {
        const uint64_t f = g_failures.fetch_add(1);
        if (f < 20)
            Log("oPresent failed hr=0x%lx (depth=%d tid=%lu)", (unsigned long)hr, depth, tid);
    }
    return hr;
}

// ---- scene detection + DLAA ------------------------------------------------
typedef void (STDMETHODCALLTYPE* RSSetViewports_t)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
typedef void (STDMETHODCALLTYPE* Draw_t)(ID3D11DeviceContext*, UINT, UINT);
typedef void (STDMETHODCALLTYPE* OMSetRenderTargets_t)(ID3D11DeviceContext*, UINT,
        ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
Draw_t               oDraw = nullptr;
OMSetRenderTargets_t oOMSetRenderTargets = nullptr;
RSSetViewports_t     oRSSetViewports = nullptr;

// Main scene depth (held ref, replaced when the game hands us a new one).
// Only touched from the render thread, so no lock.
ID3D11Texture2D* g_sceneDepth = nullptr;
UINT             g_sceneW = 0, g_sceneH = 0;
uint64_t         g_depthChanges = 0;
bool             g_tonemapLogged = false;
void*            g_tonemapTex = nullptr;         // v0.7.1: RT of the last tonemap draw (identity only, no ref)
// ---- v0.8.0 HDR blit rule (only evaluated while g_hdrOut; identities only, no refs) ----------------------------------
// HDR frame end (ATS flat, Windows HDR, captures/dlaa_trace_ats_flat_hdr_scale125.txt, frame 3):
//   [4944] OMSetRenderTargets: forward colour F (RGBA16F, scene size) + scene DSV        -> g_fwdColorTex = F
//   [5024] Draw 3 verts: RT0 = C (RGBA16F, scene size), PS SRV0 = F, SRV1 = bloom        -> HDR composite: g_hdrCompTex = C
//   [5028] Draw 3 verts: RT0 = O (RGBA16F, BACKBUFFER size, not the backbuffer), SRV0 = C -> the HDR "blit" (DLAA runs on C)
//   UI into O, then [5041] Draw: backbuffer (R10G10B10A2) <- O (the HDR10 encode; never matched: SRV0 is not C)
// [5024] and [5028] are told apart by the IDENTITY of SRV0, never by sizes: the composite reads the forward colour
// target, the blit reads the composite's RT. So at Scaling 100 % (scene size == backbuffer size, both draws look alike
// by shape) the rule still holds, and so does < 100 % (O larger than C: DLSS upscale + composite into O) and > 100 %
// (O smaller: DLAA at scene res, copied back into C, the game's own [5028] downsamples it).
void*            g_fwdColorTex = nullptr;        // RTV0 texture of the last forward jitter pass bind (1 RTV RGBA16F + scene DSV)
void*            g_hdrCompTex  = nullptr;        // RT0 of the last HDR composite draw (scene-sized RGBA16F, SRV0 = g_fwdColorTex)
bool             g_hdrCompLogged = false;        // "HDR scene composite recognised" logged
uint64_t         g_hdrBlitCount = 0;             // matched HDR blits (log / triage only)
bool             g_dlaaDisabled = false;        // dlaa_off.txt next to our DLL (start state = OFF)
constexpr int    kMaxEyes = 2;
uint64_t         g_dlaaFrames[kMaxEyes] = {};    // frames DLAA actually ran OK, per eye
thread_local bool t_inDlaa = false;              // re-entrancy guard for our own work
// ---- v0.6.1 per-eye record classification (blit, render thread) ---------------------------------------
// Counts of the eye's last GOOD candidate record. A record is BAD when a last-good exists and either layer
// count fell below 50 % of it (incl. 0). Keyed on CameraMv::InvalidateCount(): every Invalidate (DLAA
// re-enable, Ctrl+F5 MV toggle, eye-map correction, resize in SceneDlaa::Ensure, CameraMv::Init) forgets the last-good.
struct EyeRecState {
    bool     have = false;           // a last-good record exists
    int      world = 0, cabin = 0;   // its counts
    int      badRun = 0;             // consecutive bad records since
    uint64_t gen = 0;                // CameraMv::InvalidateCount() when last-good was taken
};
EyeRecState      g_eyeRec[kMaxEyes];
constexpr int    kBadRunMax      = 2;            // the 3rd bad record in a row is accepted as the new baseline
#ifdef WITH_DLAA
SceneDlaa        g_dlaa[kMaxEyes];               // one per eye (flat uses [0] only)
// v0.7.0 upscale composite, armed by HandlePossibleBlit (Run succeeded in upscale mode), executed by hkDraw right
// after the game's blit Draw on the same RT0. Render thread only.
int              g_compEye  = -1;                // eye whose result waits (-1 = none)
void*            g_compRt   = nullptr;           // RT0 resource of that blit (identity; re-checked before drawing)
uint32_t         g_compX = 0, g_compY = 0;       // output (viewport) origin inside RT0
bool             g_compSrgb = false;             // the blit's SRV0 was an _SRGB view
uint64_t         g_compSkipped = 0;              // armed composites dropped because RT0 changed
void MvInvalidate() {
    PreviewInvalidate();
    for (SceneDlaa& d : g_dlaa) d.Mv().Invalidate();
    for (EyeRecState& e : g_eyeRec) e = EyeRecState();    // v0.6.1 (the generation key would catch it too)
}
#else
void MvInvalidate() { for (EyeRecState& e : g_eyeRec) e = EyeRecState(); }
#endif
bool             g_gbufPass = false;             // current OM binding = main G-buffer (4 RTVs + scene DSV)
// v0.6.1: DrawIndexed on NON-immediate contexts (any thread); each pass snapshots the delta.
std::atomic<uint32_t> g_diDeferred{0};
// v0.6.2: OM / RS / Draw / DrawIndexed on IMMEDIATE contexts other than the game's (other devices, any thread;
// counted only once the game context is known) and OM / RS on the game context from a thread other than the
// blit thread. Each pass snapshots both deltas (same scheme as g_diDeferred).
std::atomic<uint32_t> g_foreignCtx{0};
std::atomic<uint32_t> g_gameOtherThread{0};
std::atomic<unsigned long> g_renderTid{0};       // v0.6.1: thread of the last matched blit (0 = none yet)
bool             g_needFirstPass = true;         // v0.6.2: next G-buffer bind starts a pass (start / context change)

// ---- v0.5.2 pass FIFO (render thread) ---------------------------------------------------------
struct PassSlot {
    uint64_t s = 0;                  // pass sequence number
    float    jx = 0.0f, jy = 0.0f;   // viewport shift this pass was rasterized with (0 when jitter disabled)
    int      phase = 0;
    bool     snap = false;           // scene depth of this pass was snapshotted into twin
    bool     consumed = true;
    bool     discarded = false;      // a Discard* of the scene depth was seen since this pass started
#ifdef WITH_DLAA
    DepthTwin       twin;            // this pass's depth snapshot (v0.5.4: per pass, not per eye)
    CandidateRecord cand;            // this pass's MV candidates (committed to the eye's CameraMv at the blit)
#endif
    // ---- v0.6.1 flight recorder: plain counters on the NEWEST slot, reset in StartPass, logged at the blit
    // only for a bad record (PASS ANOMALY) or as a periodic reference (pass sample). Record counts = cand.
    uint32_t diGbuf = 0;             // immediate DrawIndexed while g_gbufPass (all of them, any gate)
    uint32_t diOther = 0;            // immediate DrawIndexed while !g_gbufPass
    uint32_t diDeferred = 0;         // non-immediate DrawIndexed during the pass (frozen at the next StartPass)
    uint32_t defStart = 0;           // g_diDeferred at StartPass
    bool     defClosed = false;      // diDeferred frozen
    uint32_t gbufBinds = 0;          // EVERY qualifying G-buffer bind (4 RTVs + scene DSV), not only transitions
    uint32_t sceneClears = 0;        // ClearDSV of the scene depth incl. the one that started this pass
    uint32_t skSampled = 0;          // v0.6.3 ... rejected by the stable-sampling hash (mv_sample_shift)
    uint32_t skGate = 0;             // G-buffer draws not collected: DLAA off / (MV off and not VR)
    uint32_t skInDlaa = 0;           // ... t_inDlaa set
    uint32_t skNoCtx = 0;            // ... no ID3D11DeviceContext1 / no game viewport / record not ready
    uint32_t skFull = 0;             // ... layer already holds its cap (v0.6.3: mv_world_slots / mv_cabin_slots)
    uint32_t skNoCb = 0;             // ... no VS cbuffer in slot 0
    uint32_t skRange = 0;            // ... cbuffer window < 128 bytes or MVP outside the buffer
    // v0.6.2 (replaces v0.6.1 "foreign-thread OM/RS"): deltas of g_foreignCtx / g_gameOtherThread over the
    // pass, frozen at the next StartPass together with diDeferred (defClosed).
    uint32_t fgnCtx = 0;             // OM/RS/Draw/DrawIndexed on immediate contexts other than the game's
    uint32_t fgnStart = 0;           // g_foreignCtx at StartPass
    uint32_t otherThread = 0;        // OM/RS on the game context from a thread other than the blit thread
    uint32_t otStart = 0;            // g_gameOtherThread at StartPass
    int64_t  tStart = 0;             // QPC at StartPass
    void ResetRecorder(uint32_t deferredNow, uint32_t fgnNow, uint32_t otNow, int64_t now) {
        diGbuf = diOther = diDeferred = 0; defStart = deferredNow; defClosed = false;
        gbufBinds = sceneClears = 0;
        skSampled = skGate = skInDlaa = skNoCtx = skFull = skNoCb = skRange = 0;
        fgnCtx = 0; fgnStart = fgnNow; otherThread = 0; otStart = otNow;
        tStart = now;
    }
};
constexpr int    kRing           = 4;
PassSlot         g_ring[kRing];
uint64_t         g_passSeq       = 0;            // passes started so far (= next s)
uint64_t         g_fifoHead      = 0;            // oldest unconsumed pass (unconsumed = [g_fifoHead, g_passSeq))
uint64_t         g_blitCount     = 0;            // matched blits (incl. underflow / DLAA off)
uint64_t         g_cSnapUsed = 0, g_cLiveUsed = 0, g_cUnderflow = 0, g_cOverflow = 0, g_cMirrorIgnored = 0;
uint64_t         g_cSnapAtDiscard = 0, g_cSnapAtClear = 0, g_cDiscardNoSnap = 0, g_cDiscardSeen = 0;
bool             g_depthDirty    = false;        // G-buffer rendered into the scene depth since its last clear
bool             g_eyeLogged[kMaxEyes] = {};     // "eye blit detected" logged per eye
bool             g_flatMode      = false;        // a blit with RT0 == DXGI backbuffer was seen (flat, 1 eye)
bool             g_vrMode        = false;        // a blit into a non-backbuffer RT was seen
// v0.7.4: mode from the game's launch options, set once at load: 1 = VR (-openvr / -openxr / -oculus on the
// command line), 0 = flat. Flat: dlaa_area is forced to 100 (outside the area the picture is the raw jittered
// frame, which flickers on a monitor); g_vrArea keeps the dlaa.ini value so a save in flat does not lose it.
int              g_launchVr      = -1;
int              g_vrArea        = 100;
// v0.7.9: g_launchVr is only the FIRST GUESS. A VR launch whose headset session never comes up (ATS 22:45: -openxr on
// the command line, the game ran flat on the 3840x2160 backbuffer) never draws an eye texture; its world blits go to
// the backbuffer, which a VR launch ignores as the mirror window -> the passes were jittered and never evaluated.
// After kFlatFallbackBlits consecutive matched backbuffer blits with NO eye-texture blit ever seen in this process the
// mode falls back to flat (FallBackToFlat); a later eye-texture blit switches back (BackToVr).
bool             g_eyeBlitEver   = false;        // a matched blit into a non-backbuffer (eye) RT was ever seen
bool             g_vrFellBack    = false;        // launched VR, now running as flat (the fallback above)
uint32_t         g_bbBlitRun     = 0;            // matched backbuffer blits in a row while VR is not confirmed
constexpr uint32_t kFlatFallbackBlits = 60;
// World viewport jitter is held (no shift, pass jitter 0) while the launch says VR and no eye blit has been seen: the
// passes are not consumed by any DLAA then, a jittered raw picture would just shimmer. A real VR world starts its eye
// blits with its first frame, so only that first frame (both eyes) runs unjittered.
inline bool WorldJitterHeld() { return g_launchVr == 1 && !g_eyeBlitEver; }
int              g_resetEyes     = 0;            // per-eye reset bitmask (fed from g_resetNext at the next blit)
// ---- VR eye identity (v0.5.4): from the blit render target, not from pass order ------------------------
struct EyeMapEntry { void* rt; int eye; int parity; int score; };   // score: v0.5.5 projection verdicts (+ = eye 0)
constexpr int    kEyeMapCap      = 16;
EyeMapEntry      g_eyeMap[kEyeMapCap];
int              g_eyeMapN       = 0;
bool             g_eyeMapLogged  = false;
uint64_t         g_cRtParityMismatch = 0;        // same RT arrived with the opposite pair index
uint64_t         g_rReads[kMaxEyes] = {};        // R_world readbacks per eye
uint64_t         g_rBig[kMaxEyes]   = {};        // ... with |R[0][3]| > 0.25 (wrong-eye pairing signature; expect ~0)
uint64_t         g_snapCount     = 0;            // total depth snapshots taken
// ---- v0.5.5 eye verdict from the pass's projection (async MVP readback) ----------------------------------
uint64_t         g_cVerdicts     = 0;            // readbacks that arrived and were evaluated
uint64_t         g_cVerdictNoVote = 0;           // ... that gave no verdict (vote 0 / no usable MVP)
uint64_t         g_cEyeCorrections = 0;          // eye map entries flipped by verdicts
uint64_t         g_cRbSkipFull   = 0;            // clean blits not read back because the staging ring was full
uint64_t         g_cRbFailed     = 0;            // readbacks dropped (Map failed with something other than STILL_DRAWING)
void*            g_prevBlitRt    = nullptr;      // RT of the previous matched blit (clean-blit filter)
uint64_t         g_lastUnderflowBlit = 0;        // g_blitCount of the most recent FIFO underflow (0 = none)
uint64_t         g_cRbThrottled  = 0;            // v0.5.6: clean blits not read back because the eye map is settled
uint64_t         g_settledClean  = 0;            // v0.5.6: clean blits since the eye map became settled
// ---- v0.5.6 CPU timing (QueryPerformanceCounter, render thread), windowed by the FIFO stats line ----------
int64_t          g_qpcFreq       = 0;            // QueryPerformanceFrequency (set in StartInjection)
int64_t          g_cpuBlitTicks  = 0;            // time inside the matched part of HandlePossibleBlit
uint64_t         g_cpuBlitN      = 0;            // ... number of matched blits timed
int64_t          g_cpuMvTicks    = 0;            // time inside CollectMvCandidate (all calls)
uint64_t         g_cMvRecords    = 0;            // CollectMvCandidate calls that recorded a candidate
uint64_t         g_cpuPass0      = 0;            // g_passSeq at the start of the window
uint64_t         g_rateBlit0     = 0;            // g_blitCount at the start of the rate window (0 = not started)
int64_t          g_rateT0        = 0;            // QPC at the start of the rate window
// ---- v0.5.7 cost per live state: one extra rate line after a preset / sharpness change ----------------------
bool             g_rateChgArm    = false;        // set by a live change (Present), latched at the next blit
uint64_t         g_rateChgBlit   = 0;            // g_blitCount of the first blit after the change (0 = idle)
int64_t          g_rateChgT0     = 0;            // QPC at blit +kRateChgSkip
constexpr uint64_t kRateChgSkip  = 60;           // blits ignored after a change (feature recreate hitch)
constexpr uint64_t kRateChgAt    = 300;          // the line is written at this blit after the change
inline int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

#ifdef WITH_DLAA
// v0.5.7: after a live preset / sharpness change, every timing window restarts so each state gets its own
// numbers: per-stage GPU sums of both eyes (next line after 240 frames), the depth snapshot timer, and one
// "rate after change" line (blits kRateChgSkip..kRateChgAt after the change).
void OnLiveTuningChange() {
    for (SceneDlaa& d : g_dlaa) d.ResetTiming();
    SceneDlaa::ResetSnapTiming();
    g_rateChgArm = true;
    ResetSpanWindow();                                   // v0.6.2 whole-frame GPU spans
}

// v0.6.2 (v0.6.5 Ctrl+F7): passive mode on/off (see g_passive). Leaving it drops the DLSS history and the
// (empty) candidate records collected while passive.
void TogglePassive(uint64_t n) {
    const bool on = !g_passive.load();
    g_passive = on;
    if (!on) { g_resetNext = true; MvInvalidate(); }
    Log("passive mode %s (passive key, Present #%llu)%s", on ? "ON" : "OFF", (unsigned long long)n,
        on ? " -- no jitter / candidate copies / depth snapshot / DLAA; tracking + GPU spans only" : "");
    PlayTones(on ? 1 : 2, 250, 150, 100);                // 1 low beep = ON, 2 low beeps = OFF
    OnLiveTuningChange();
}

// v0.6.5 Shift+F1..F4: DIRECTLY select model idx 0..3 = preset default / E / F / M (replaces the old Shift+End
// cycle). Selecting the already-active model just beeps again (its N-beep code) and does NOT recreate the NGX
// feature. Otherwise both eyes recreate their feature at their next evaluate (dlaa.cpp, generation check), with
// a DLSS history reset. The fallback-to-default handling (CheckPresetFallback) is unchanged.
void SelectDlssPreset(int idx, uint64_t n) {
    if (idx < 0 || idx > 3) return;
    static const char  kLetter[4] = { 0, 'E', 'F', 'M' };
    static const char* kName[4]   = { "default", "E", "F", "M" };
    const char* cur = SceneDlaa::DlssPresetName();
    if (!strcmp(cur, kName[idx])) {                      // already this model: re-beep only, no recreate
        Log("DLSS preset already %s (model key %d, Present #%llu) -- unchanged", kName[idx], idx + 1,
            (unsigned long long)n);
        PlayTones(idx + 1, 880, 120, 100);               // 1 default, 2 E, 3 F, 4 M
        return;
    }
    SceneDlaa::SetDlssPreset(kLetter[idx]);
    g_resetNext = true;                                  // DLSS history of both eyes
    OnLiveTuningChange();
    Log("DLSS preset now %s (model key %d, Present #%llu)", SceneDlaa::DlssPresetName(), idx + 1,
        (unsigned long long)n);
    PlayTones(idx + 1, 880, 120, 100);                   // 1 default, 2 E, 3 F, 4 M
}

// v0.5.7 (v0.6.5 Shift+F7 -1 / Shift+F8 +1): sharpness in 0.1 steps, clamped 0..1 (0 = RCAS pass skipped).
void StepSharpness(int dir, uint64_t n) {
    const float cur = SceneDlaa::Sharpness();
    if (dir > 0 ? cur >= 1.0f : cur <= 0.0f) {
        Log("sharpness now %.1f (sharpen keys, Present #%llu) -- already at the limit, unchanged",
            (double)cur, (unsigned long long)n);
        PlayTones(1, 200, 150, 0);                       // low beep: clamp limit
        return;
    }
    SceneDlaa::SetSharpness(std::floor((cur + 0.1f * (float)dir) * 10.0f + 0.5f) / 10.0f);   // clamps 0..1
    const float s = SceneDlaa::Sharpness();
    OnLiveTuningChange();
    Log("sharpness now %.1f (sharpen keys, Present #%llu)", (double)s, (unsigned long long)n);
    PlayTones(1, (DWORD)(400.0f + 800.0f * s + 0.5f), 150, 0);
}

// v0.5.8 (v0.6.5 Shift+F9 -1 / Shift+F10 +1): RCAS ring-tap radius ("sharpen width") in 0.5 px steps, clamped 1..4.
void StepSharpRadius(int dir, uint64_t n) {
    const float cur = SceneDlaa::SharpRadius();
    if (dir > 0 ? cur >= 4.0f : cur <= 1.0f) {
        Log("sharpen radius now %.1f px (width keys, Present #%llu) -- already at the limit, unchanged",
            (double)cur, (unsigned long long)n);
        PlayTones(1, 200, 150, 0);                       // low beep: clamp limit (same as the strength limit tone)
        return;
    }
    SceneDlaa::SetSharpRadius(std::floor((cur + 0.5f * (float)dir) * 10.0f + 0.5f) / 10.0f);   // clamps 1..4
    const float r = SceneDlaa::SharpRadius();
    OnLiveTuningChange();
    Log("sharpen radius now %.1f px (width keys, Present #%llu)", (double)r, (unsigned long long)n);
    // TWO short beeps (distinguishable from the single strength beep) at 400 + 250 * radius Hz.
    BeepSeq s;
    s.n = 2;
    s.freq[0] = s.freq[1] = (DWORD)(400.0f + 250.0f * r + 0.5f);
    s.ms[0] = s.ms[1] = 100;
    s.gapMs = 80;
    PlayBeeps(s);
}

// v0.6.4 (v0.6.5 Shift+F5 -1 / Shift+F6 +1): DLAA area in 10 % steps, clamped 40..100 (100 = whole image). Each
// eye rebuilds its crop textures + NGX feature at its next blit (Ensure sees the new crop size; one hitch), with
// a DLSS history reset.
void StepArea(int dir, uint64_t n) {
    if (g_launchVr == 0) {                               // v0.7.4: flat = always the whole picture
        Log("DLAA area stays 100%% (area keys, Present #%llu): flat mode always uses the whole picture", (unsigned long long)n);
        PlayTones(1, 200, 150, 0);
        return;
    }
    const int cur = SceneDlaa::Area();
    uint32_t cw = 0, ch = 0;
    if (dir > 0 ? cur >= 100 : cur <= 40) {
        SceneDlaa::CropSize(g_sceneW, g_sceneH, cur, &cw, &ch);
        Log("DLAA area now %d%% (%ux%u of %ux%u per eye, area keys, Present #%llu) -- already at the limit, unchanged",
            cur, cw, ch, g_sceneW, g_sceneH, (unsigned long long)n);
        PlayTones(1, 200, 150, 0);                       // low beep: clamp limit (same as the other limit tones)
        return;
    }
    SceneDlaa::SetArea(cur + 10 * dir);                  // clamps 40..100
    const int a = SceneDlaa::Area();
    SceneDlaa::CropSize(g_sceneW, g_sceneH, a, &cw, &ch);
    g_resetNext = true;                                  // DLSS history of both eyes (the rebuild also resets)
    OnLiveTuningChange();
    Log("DLAA area now %d%% (%ux%u of %ux%u per eye, area keys, Present #%llu)",
        a, cw, ch, g_sceneW, g_sceneH, (unsigned long long)n);
    // THREE short beeps (distinguishable from the 1-beep strength and 2-beep radius codes) at 300 + 6 * area Hz.
    PlayTones(3, (DWORD)(300 + 6 * a), 90, 70);
}

void CheckPresetFallback(uint64_t n) {
    static uint32_t seen = 0;
    const uint32_t fb = SceneDlaa::DlssPresetFallbacks();
    if (fb == seen) return;
    seen = fb;
    Log("DLSS preset fell back to default: NGX could not create the requested preset (see the DLAA: lines above, Present #%llu)",
        (unsigned long long)n);
    BeepSeq s;                                           // long low tone, then the 1-beep "default" code
    s.n = 2; s.freq[0] = 200; s.ms[0] = 400; s.freq[1] = 880; s.ms[1] = 120; s.gapMs = 150;
    PlayBeeps(s);
}

// ---- v0.6.5 Shift+F12: persist the current live values to dlaa.ini --------------------------------------
bool PathNextToDll(const wchar_t* name, wchar_t* path);   // defined below

// Rewrites dlaa.ini preserving everything else. For each of the `nkv` keys: a non-comment line whose key (the
// text before '=', trimmed) equals it is replaced IN PLACE by "key = value" (keeping that line's exact
// terminator); every other line (comments, blanks, other keys) is kept byte-for-byte; a key not present is
// appended at the end. A missing file is created with a one-line '#' header plus the keys. The new text is
// written to dlaa.ini.tmp and MoveFileExW'd over the original, so a failed write cannot destroy the file.
// Returns true only if the temp was fully written AND the replace succeeded.
struct IniKV { const char* key; char val[32]; };
bool WriteIniPreserving(const IniKV* kv, int nkv) {
    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    if (!PathNextToDll(L"dlaa.ini", path) || !PathNextToDll(L"dlaa.ini.tmp", tmp)) return false;

    std::string in;                                      // existing file, read raw (binary) so endings survive
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") == 0 && f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) { in.resize((size_t)sz); in.resize(fread(&in[0], 1, (size_t)sz, f)); }
        fclose(f);
    }
    const char* nl = (in.find("\r\n") != std::string::npos) ? "\r\n" : "\n";   // match the file's ending

    std::string out;
    out.reserve(in.size() + 256);
    bool seen[8] = {};                                   // nkv <= 8
    if (in.empty()) {
        out += "# dlaa.ini -- settings saved by the ETS2 DLAA injector";
        out += nl;
    } else {
        size_t i = 0;
        while (i < in.size()) {
            size_t e = i;
            while (e < in.size() && in[e] != '\n') ++e;  // body = [i, e); e is the '\n' or == size
            const size_t rawEnd = (e < in.size()) ? e + 1 : e;   // raw line incl. the '\n'
            size_t pe = e;
            if (pe > i && in[pe - 1] == '\r') --pe;       // strip a trailing '\r' from the body
            size_t s = i;
            while (s < pe && (in[s] == ' ' || in[s] == '\t')) ++s;
            const bool isComment = (s < pe && (in[s] == '#' || in[s] == ';'));
            int match = -1;
            if (!isComment && s < pe) {
                size_t eq = i;
                while (eq < pe && in[eq] != '=') ++eq;
                if (eq < pe) {                            // the line has an '='
                    size_t ks = i, ke = eq;               // key = [i, eq) trimmed
                    while (ks < ke && (in[ks] == ' ' || in[ks] == '\t')) ++ks;
                    while (ke > ks && (in[ke - 1] == ' ' || in[ke - 1] == '\t')) --ke;
                    for (int k = 0; k < nkv; ++k) {
                        const size_t kl = strlen(kv[k].key);
                        if (ke - ks == kl && memcmp(&in[ks], kv[k].key, kl) == 0) { match = k; break; }
                    }
                }
            }
            if (match >= 0) {                             // replace, keeping this line's own terminator
                out += kv[match].key; out += " = "; out += kv[match].val;
                out.append(in, pe, rawEnd - pe);
                seen[match] = true;
            } else {
                out.append(in, i, rawEnd - i);            // keep byte-for-byte
            }
            i = rawEnd;
        }
    }
    bool needNl = !out.empty() && out[out.size() - 1] != '\n';   // ensure a break before appended keys
    for (int k = 0; k < nkv; ++k) {
        if (seen[k]) continue;
        if (needNl) { out += nl; needNl = false; }
        out += kv[k].key; out += " = "; out += kv[k].val; out += nl;
    }

    FILE* w = nullptr;
    if (_wfopen_s(&w, tmp, L"wb") != 0 || !w) return false;
    const bool wrote = out.empty() ? true : (fwrite(out.data(), 1, out.size(), w) == out.size());
    const bool closed = (fclose(w) == 0);
    if (!wrote || !closed) { _wremove(tmp); return false; }
    if (!MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) { _wremove(tmp); return false; }
    return true;
}

void SaveSettings(uint64_t n) {
    const char* preset = SceneDlaa::DlssPresetName();    // "default" / "E" / "F" / "M" (the name LoadConfig reads)
    const int   area   = g_launchVr == 0 ? g_vrArea : SceneDlaa::Area();   // v0.7.4: flat keeps the VR value in the file
    const float sharp  = SceneDlaa::Sharpness();
    const float radius = SceneDlaa::SharpRadius();
    const int   upscale = g_dlssUpscale.load() ? 1 : 0;   // v0.7.0 (Ctrl+F4)
    IniKV kv[5] = { { "dlss_preset", {} }, { "dlaa_area", {} }, { "sharpness", {} }, { "sharp_radius", {} },
                    { "dlss_upscale", {} } };
    snprintf(kv[0].val, sizeof(kv[0].val), "%s", preset);
    snprintf(kv[1].val, sizeof(kv[1].val), "%d", area);
    snprintf(kv[2].val, sizeof(kv[2].val), "%.1f", (double)sharp);
    snprintf(kv[3].val, sizeof(kv[3].val), "%.1f", (double)radius);
    snprintf(kv[4].val, sizeof(kv[4].val), "%d", upscale);
    if (WriteIniPreserving(kv, 5)) {
        Log("settings saved to dlaa.ini: dlss_preset=%s dlaa_area=%d sharpness=%.1f sharp_radius=%.1f dlss_upscale=%d (save key, Present #%llu)",
            preset, area, (double)sharp, (double)radius, upscale, (unsigned long long)n);
        BeepSeq s;                                       // success: rising two-tone 600 -> 900 Hz, 120 ms each
        s.n = 2; s.freq[0] = 600; s.ms[0] = 120; s.freq[1] = 900; s.ms[1] = 120; s.gapMs = 40;
        PlayBeeps(s);
    } else {
        Log("settings save FAILED (dlss_preset=%s dlaa_area=%d sharpness=%.1f sharp_radius=%.1f dlss_upscale=%d, save key, Present #%llu)",
            preset, area, (double)sharp, (double)radius, upscale, (unsigned long long)n);
        PlayTones(1, 200, 400, 0);                       // failure: one long 200 Hz tone (400 ms)
    }
}

// v0.7.2 plain End: one key walks the three modes and saves the new one to dlaa.ini at once (key `mode`).
//   1 = DLAA (NGX at render res, the game stretches; dlss_upscale off)      -- 1 high beep
//   2 = DLSS (NGX upscales render res -> eye texture; dlss_upscale on)      -- 2 high beeps
//   0 = off  (raw game picture)                                             -- 1 low beep
// The render size itself is the game's r_scale: at 100 % scaling modes 1 and 2 give the same picture.
void CycleMode(uint64_t n) {
    const bool wasOn = g_dlaaOn.load();
    const int cur = !wasOn ? 0 : (g_dlssUpscale.load() ? 2 : 1);
    int next = cur == 0 ? 1 : (cur == 1 ? 2 : 0);
    if (next != 0 && !wasOn && g_gameDeviceChanged) {
        Log("mode stays OFF (mode key, Present #%llu): the game render device changed -- restart the game", (unsigned long long)n);
        PlayTones(1, 200, 400, 0);
        return;
    }
    if (next != 0) {
        g_dlssUpscale = next == 2;
        if (next == 2) g_upBlocked[0] = 0;               // a failed upscale size is retried
        g_resetNext = true;                              // textures + NGX feature rebuild at the next blit
        if (!wasOn) MvInvalidate();                      // stale MV candidates from before the off period
    }
    g_dlaaOn = next != 0;
    IniKV kv[1] = { { "mode", {} } };
    snprintf(kv[0].val, sizeof(kv[0].val), "%d", next);
    const bool saved = WriteIniPreserving(kv, 1);
    Log("mode %s (mode key, Present #%llu)%s", next == 1 ? "DLAA" : (next == 2 ? "DLSS" : "OFF"), (unsigned long long)n,
        saved ? " -- saved to dlaa.ini" : " -- dlaa.ini save FAILED");
    if (next == 0) PlayTones(1, 300, 150, 0);
    else           PlayTones(next, 1200, 150, 100);
    OnLiveTuningChange();
}
#endif
// Adds the time from construction to scope exit to the blit CPU counters (every return path of the matched part).
struct BlitCpuTimer {
    const int64_t t0 = Qpc();
    ~BlitCpuTimer() { g_cpuBlitTicks += Qpc() - t0; ++g_cpuBlitN; }
};

void LogEyeMap() {
    char buf[640]; int n = 0; buf[0] = 0;
    for (int i = 0; i < g_eyeMapN && n < (int)sizeof(buf) - 40; ++i)
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, " [%p->e%d s%+d]", g_eyeMap[i].rt, g_eyeMap[i].eye, g_eyeMap[i].score);
    Log("eye map (%d RTs):%s", g_eyeMapN, buf);
}
// v0.5.5: a projection verdict (absolute eye of the pass that was blitted into `rt`) moves that RT's score;
// at |score| >= 2 the decided eye wins over the first guess. A flip drops both eyes' history (mixed eyes).
void ApplyEyeVerdict(void* rt, int verdictEye, float p) {
    for (int i = 0; i < g_eyeMapN; ++i) {
        EyeMapEntry& e = g_eyeMap[i];
        if (e.rt != rt) continue;
        e.score += verdictEye == 0 ? +1 : -1;
        if (e.score > 6) e.score = 6;
        if (e.score < -6) e.score = -6;
        const int decided = e.score >= 2 ? 0 : (e.score <= -2 ? 1 : e.eye);
        if (decided != e.eye) {
            ++g_cEyeCorrections;
            Log("eye map: RT %p corrected eye %d -> %d by projection verdict (score %d, p=%.4f)",
                rt, e.eye, decided, e.score, (double)p);
            e.eye = decided;
            g_resetNext = true;
            MvInvalidate();
            if (g_cEyeCorrections <= 50) LogEyeMap();
        }
        return;
    }
    // RT not in the map (map was cleared since the readback was queued): ignore.
}
// Eye of a blit render target. Flat (backbuffer) is handled by the caller (eye 0). `pairIdx` = s&1 of the
// consumed pass = blit index within the frame pair; used only as the FIRST GUESS for an unseen RT. Since
// v0.5.5 the async projection verdicts (ApplyEyeVerdict) correct the entry; until they arrive the guess rules.
int EyeOfRT(void* rt, int pairIdx) {
    for (int i = 0; i < g_eyeMapN; ++i) {
        if (g_eyeMap[i].rt != rt) continue;
        if (g_eyeMap[i].parity != pairIdx) {
            if (g_cRtParityMismatch++ < 10)
                Log("eye map: RT %p (eye %d) arrived with pair index %d, first seen with %d (rt parity mismatch #%llu; map kept)",
                    rt, g_eyeMap[i].eye, pairIdx, g_eyeMap[i].parity, (unsigned long long)g_cRtParityMismatch);
        }
        return g_eyeMap[i].eye;
    }
    if (g_eyeMapN >= kEyeMapCap) {
        Log("eye map full (%d): clearing (swapchain recreated?)", g_eyeMapN);
        g_eyeMapN = 0; g_eyeMapLogged = false;
        for (EyeMapEntry& e : g_eyeMap) e.score = 0;
    }
    g_eyeMap[g_eyeMapN] = { rt, pairIdx & 1, pairIdx, 0 };
    ++g_eyeMapN;
    if (g_eyeMapN <= 8)
        Log("eye map: new blit RT %p -> eye %d (pair index %d, entry #%d)", rt, pairIdx & 1, pairIdx, g_eyeMapN);
    if (g_eyeMapN == 6 && !g_eyeMapLogged) { g_eyeMapLogged = true; LogEyeMap(); }
    return pairIdx & 1;
}
#ifdef WITH_DLAA
// v0.5.5 async readback ring: a clean VR blit GPU-copies its pass's candidate record (MVPs) into a staging
// buffer; later blits poll oldest-first with DO_NOT_WAIT (never a blocking Map) and turn it into a verdict.
struct EyeReadback {
    ID3D11Buffer* staging = nullptr;  // CandidateRecord::kBytes, STAGING + CPU read (held for the process life)
    void*         rt = nullptr;       // blit RT the pass went to (identity only)
    int           count[CandidateRecord::kLayers] = {};
    uint64_t      seq = 0;            // g_blitCount at queue time
};
constexpr int    kRbRing         = 8;
EyeReadback      g_rb[kRbRing];
int              g_rbHead        = 0;            // oldest pending
int              g_rbPending     = 0;
bool             g_rbInitTried   = false;
bool             g_rbOk          = false;

bool EnsureEyeReadback(ID3D11DeviceContext* ctx) {
    if (g_rbInitTried) return g_rbOk;
    g_rbInitTried = true;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = CandidateRecord::kBytes;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    g_rbOk = true;
    for (EyeReadback& r : g_rb)
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &r.staging)) || !r.staging) { g_rbOk = false; break; }
    dev->Release();
    if (!g_rbOk) Log("eye verdict: staging ring creation failed (projection eye correction disabled)");
    return g_rbOk;
}

// ---- v0.6.4 optical centre per eye (DLAA area rect centre), from the verdict readbacks ---------------------------
// A point on the optical axis lands at ndc = (-p, -q) (p / q = projection x / y skew of the MVP), so
// uv = (0.5 - p/2, 0.5 + q/2) (uv y down). Each verdict delivers the mean p / q of its winning-side MVPs; the
// eye keeps the last kWin of them and its centre is their mean. Adopted once kOptMinSamples are in; after that
// only RE-adopted when the mean moved by more than 0.01 in u or v (a rect move resets that eye's DLSS history,
// so it must not follow per-readback noise). Constants of the headset -> expect one adopt per eye.
struct OpticalCentre {
    static constexpr int kWin = 16;
    float    p[kWin] = {}, q[kWin] = {};
    int      n = 0, head = 0;                // samples in the window / next write slot
    bool     have = false;                   // a centre has been adopted
    float    u = 0.5f, v = 0.5f;             // adopted centre (uv)
    uint32_t adopts = 0;
};
OpticalCentre    g_optC[kMaxEyes];
constexpr int    kOptMinSamples  = 4;
uint32_t         g_cOptRejected  = 0;            // samples ignored by the sanity check (non-finite / |p|,|q| > 0.6)

void NoteOpticalCentre(int eye, float p, float q) {
    if (eye < 0 || eye >= kMaxEyes) return;
    if (!std::isfinite(p) || !std::isfinite(q) || std::fabs(p) > 0.6f || std::fabs(q) > 0.6f) {
        if (g_cOptRejected++ < 5)
            Log("DLAA area: eye %d optical centre sample ignored (p=%.4f q=%.4f, sanity limit 0.6)", eye, (double)p, (double)q);
        return;
    }
    OpticalCentre& c = g_optC[eye];
    c.p[c.head] = p; c.q[c.head] = q;
    c.head = (c.head + 1) % OpticalCentre::kWin;
    if (c.n < OpticalCentre::kWin) ++c.n;
    if (c.n < kOptMinSamples) return;
    double sp = 0.0, sq = 0.0;
    for (int i = 0; i < c.n; ++i) { sp += c.p[i]; sq += c.q[i]; }
    const double mp = sp / c.n, mq = sq / c.n;
    const float u = (float)(0.5 - 0.5 * mp), v = (float)(0.5 + 0.5 * mq);
    if (c.have && std::fabs(u - c.u) <= 0.01f && std::fabs(v - c.v) <= 0.01f) return;
    c.have = true; c.u = u; c.v = v;
    if (c.adopts++ < 100)
        Log("DLAA area: eye %d optical centre uv=(%.3f, %.3f) from p=%.4f q=%.4f", eye, (double)u, (double)v, mp, mq);
}

// Sign of the projection x-skew of each usable MVP votes the absolute eye (cabin layer first, then world).
// Returns 0 / 1, or -1 for no verdict.
// v0.6.4: *qOut = mean y skew q = -dot(row1.xyz,row3.xyz)/|row3.xyz|^2 over the same MVPs as *pOut (winning side).
int EyeVerdict(const float* data, const int* count, float* pOut, float* qOut, int* votesOut, int* layerVotes) {
    int vote = 0; double pSum[2] = {}, qSum[2] = {}; int pN[2] = {};
    const int layers[2] = { 1, 0 };                      // cabin, world
    for (int li = 0; li < 2; ++li) {
        const int l = layers[li];
        const int n = count[l] < 8 ? count[l] : 8;
        int lv = 0;
        for (int i = 0; i < n; ++i) {
            const float* m = data + (size_t)(l * CandidateRecord::kSlots + i) * 16;
            bool finite = true;
            for (int k = 0; k < 16; ++k) if (!std::isfinite(m[k])) { finite = false; break; }
            if (!finite) continue;
            const float* r0 = m; const float* r1 = m + 4; const float* r3 = m + 12;
            const double d33 = (double)r3[0] * r3[0] + (double)r3[1] * r3[1] + (double)r3[2] * r3[2];
            if (d33 <= 1e-12) continue;
            const double p = -((double)r0[0] * r3[0] + (double)r0[1] * r3[1] + (double)r0[2] * r3[2]) / d33;
            if (std::fabs(p) < 0.03) continue;
            const double q = -((double)r1[0] * r3[0] + (double)r1[1] * r3[1] + (double)r1[2] * r3[2]) / d33;   // v0.6.4
            const int side = p > 0.0 ? 0 : 1;
            lv += side == 0 ? +1 : -1;
            pSum[side] += p; qSum[side] += q; ++pN[side];
        }
        layerVotes[l] = lv;
        vote += lv;
    }
    *votesOut = vote;
    const int eye = vote > 0 ? 0 : (vote < 0 ? 1 : -1);
    *pOut = eye >= 0 && pN[eye] ? (float)(pSum[eye] / pN[eye]) : 0.0f;   // mean p of the winning side
    *qOut = eye >= 0 && pN[eye] ? (float)(qSum[eye] / pN[eye]) : 0.0f;   // v0.6.4: mean q, same MVPs
    return eye;
}

// Poll pending readbacks oldest-first; stop at the first one the GPU has not finished.
void PollEyeReadbacks(ID3D11DeviceContext* ctx) {
    while (g_rbPending > 0) {
        EyeReadback& r = g_rb[g_rbHead];
        D3D11_MAPPED_SUBRESOURCE ms{};
        const HRESULT hr = ctx->Map(r.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) break;
        g_rbHead = (g_rbHead + 1) % kRbRing; --g_rbPending;
        if (FAILED(hr) || !ms.pData) {
            if (SUCCEEDED(hr)) ctx->Unmap(r.staging, 0);
            if (g_cRbFailed++ < 5) Log("eye verdict: readback Map failed hr=0x%08lx (dropped)", (unsigned long)hr);
            continue;
        }
        float p = 0.0f, q = 0.0f; int votes = 0; int lv[CandidateRecord::kLayers] = {};
        const int v = EyeVerdict((const float*)ms.pData, r.count, &p, &q, &votes, lv);
        ctx->Unmap(r.staging, 0);
        ++g_cVerdicts;
        if (g_cVerdicts <= 6)
            Log("eye verdict: RT %p -> eye %d (p=%.4f q=%.4f, votes %d, cabin=%d world=%d) [blit #%llu, candidates cabin=%d world=%d]",
                r.rt, v, (double)p, (double)q, votes, lv[1], lv[0], (unsigned long long)r.seq, r.count[1], r.count[0]);
        if (v < 0) { ++g_cVerdictNoVote; continue; }
        NoteOpticalCentre(v, p, q);                      // v0.6.4: the verdict eye's own projection
        ApplyEyeVerdict(r.rt, v, p);
    }
}

// Queue this pass's candidate record for an async verdict (caller guarantees a clean VR blit).
void QueueEyeReadback(ID3D11DeviceContext* ctx, const CandidateRecord& cand, void* rt) {
    if (!cand.Ready() || !cand.Buffer() || (cand.Count(0) <= 0 && cand.Count(1) <= 0)) return;
    if (g_rbPending >= kRbRing) { ++g_cRbSkipFull; return; }
    EyeReadback& r = g_rb[(g_rbHead + g_rbPending) % kRbRing];
    ctx->CopyResource(r.staging, cand.Buffer());
    r.rt = rt;
    for (int l = 0; l < CandidateRecord::kLayers; ++l) r.count[l] = cand.Count(l);
    r.seq = g_blitCount;
    ++g_rbPending;
}
#endif
int              g_gpuTiming     = -1;           // dlaa.ini gpu_timing: -1 auto (VR only), 0 off, 1 on
// GPU timestamp queries wanted right now (DLAA stages and, v0.5.6, the depth snapshot copy).
inline bool GpuTimingOn() { return g_gpuTiming > 0 || (g_gpuTiming < 0 && g_vrMode); }

// ---- v0.6.2 whole-frame GPU spans (game context, render thread, log only) -----------------------------------
// Two timestamp spans, same pattern as SnapTimer (scene_dlaa.cpp): a small query ring per span type, read with
// D3D11_ASYNC_GETDATA_DONOTFLUSH at the next blits, a span is simply not measured when every slot is busy.
// Active under GpuTimingOn() whatever the DLAA / passive state, so DLAA on/off (Shift+F11) and passive (Ctrl+F7) can be compared in one session.
//  gbuf : per pass, from just after the scene-depth clear that started it (StartPass) to the scene-depth discard,
//         else the next StartPass, else the blit that consumes the pass (before our DLAA work). Contains the game's
//         G-buffer (+ whatever it renders before the discard), our candidate CopySubresourceRegions and the depth
//         snapshot (taken before the span is closed at the discard / next pass start).
//  scene: per frame, from just after the clear of the first pass of a frame (FIFO empty at StartPass) to just
//         after the game's Draw of the frame's last blit (VR: 2nd eye blit, flat: the blit), only when the frame
//         is a clean pair (VR: exactly passes s0, s0+1 then their 2 blits; flat: s0 then its blit; no FIFO
//         underflow / overflow in between) -- otherwise the sample is discarded. Contains both eyes' scene
//         rendering, both DLAA evaluates (when on) and the first eye blit.
struct SpanRing {
    static constexpr int kN = 8;
    struct Q {
        ID3D11Query* dis = nullptr; ID3D11Query* t0 = nullptr; ID3D11Query* t1 = nullptr;
        bool open = false, inFlight = false; uint32_t gen = 0;
    };
    Q    q[kN];
    bool broken = false;              // query creation failed: this span type stays unmeasured
    void Release() {
        for (Q& s : q) {
            if (s.dis) s.dis->Release();
            if (s.t0) s.t0->Release();
            if (s.t1) s.t1->Release();
            s = Q();
        }
    }
    // Returns the slot to End() later, or -1 (no free slot / no queries): span not measured.
    int Begin(ID3D11DeviceContext* ctx, uint32_t gen) {
        if (broken) return -1;
        int i = 0;
        while (i < kN && (q[i].open || q[i].inFlight)) ++i;
        if (i == kN) return -1;
        Q& s = q[i];
        if (!s.dis) {
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            bool ok = dev && SUCCEEDED(dev->CreateQuery(&qd, &s.dis));
            qd.Query = D3D11_QUERY_TIMESTAMP;
            ok = ok && SUCCEEDED(dev->CreateQuery(&qd, &s.t0)) && SUCCEEDED(dev->CreateQuery(&qd, &s.t1));
            if (dev) dev->Release();
            if (!ok) { Release(); broken = true; Log("GPU spans: query creation failed (span timing off)"); return -1; }
        }
        ctx->Begin(s.dis);
        ctx->End(s.t0);
        s.open = true; s.gen = gen;
        return i;
    }
    // keep=false: the span is closed (so the slot can land and be reused) but its sample is discarded.
    void End(ID3D11DeviceContext* ctx, int i, bool keep) {
        if (i < 0 || i >= kN || !q[i].open) return;
        Q& s = q[i];
        ctx->End(s.t1);
        ctx->End(s.dis);
        s.open = false; s.inFlight = true;
        if (!keep) s.gen = 0;          // never a live generation
    }
    // Lands finished samples (never blocks); only samples of generation `gen` count, the first `*skip` are dropped.
    void Poll(ID3D11DeviceContext* ctx, uint32_t gen, uint32_t* skip, double* sum, uint32_t* n) {
        for (Q& s : q) {
            if (!s.inFlight) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
            if (ctx->GetData(s.dis, &dd, sizeof(dd), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            UINT64 a = 0, b = 0;
            if (ctx->GetData(s.t0, &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                ctx->GetData(s.t1, &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            s.inFlight = false;
            if (s.gen != gen || dd.Disjoint || !dd.Frequency || b < a) continue;
            if (*skip > 0) { --*skip; continue; }
            *sum += (double)(b - a) * 1000.0 / (double)dd.Frequency;
            ++*n;
        }
    }
};
SpanRing         g_gbufRing, g_sceneRing;
uint32_t         g_spanGen        = 1;           // window generation (0 = discarded sample)
int              g_gbufSpan       = -1;          // open gbuf span slot of the NEWEST pass (-1 = none)
uint64_t         g_gbufSpanS      = 0;           // ... that pass
int              g_sceneSpan      = -1;          // open scene span slot (-1 = none)
uint64_t         g_sceneS0 = 0, g_sceneUnd0 = 0, g_sceneOvf0 = 0;   // its first pass, underflow/overflow counts
bool             g_spanPendGbuf   = false;       // begin the gbuf span after the game's clear runs
bool             g_spanPendScene  = false;       // begin the scene span after the game's clear runs
uint64_t         g_spanPendS      = 0;           // pass the pending begins belong to
bool             g_sceneEndAfterDraw = false;    // close the scene span right after the current blit's Draw
constexpr uint32_t kSpanWarmup    = 30;          // landed samples dropped after a window restart
constexpr uint32_t kSpanEvery     = 300;         // scene samples per log line (or 2x that many gbuf samples)
double           g_spanSceneSum = 0.0, g_spanGbufSum = 0.0;
uint32_t         g_spanSceneN = 0, g_spanGbufN = 0, g_spanSceneSkip = kSpanWarmup, g_spanGbufSkip = kSpanWarmup;
uint32_t         g_spanSceneDropped = 0;         // scene spans discarded in the window (no clean pairing)
// Window key: any change restarts the window (DLAA on/off / passive / preset / MV / sharpness / radius call
// ResetSpanWindow via OnLiveTuningChange; the key also catches the Ctrl+F9 self-test and Ctrl+F8 jitter-only).
struct SpanKey {
    bool dlaa = false, passive = false, jo = false, mv = false; char preset[12] = {}; float sharp = 0.0f, r = 0.0f;
    int  area = 100;                                 // v0.6.4 DLAA area %
    char up[48] = {};                                // v0.7.0 "WxH->WxH" / "off" (g_upTag)
    bool operator==(const SpanKey& o) const {
        return dlaa == o.dlaa && passive == o.passive && jo == o.jo && mv == o.mv && sharp == o.sharp && r == o.r &&
               area == o.area && !strcmp(preset, o.preset) && !strcmp(up, o.up);
    }
};
SpanKey          g_spanKey;
SpanKey CurrentSpanKey() {
    SpanKey k;
    k.dlaa = g_dlaaOn.load(std::memory_order_relaxed);
    k.passive = g_passive.load(std::memory_order_relaxed);
    k.jo = g_jitterOnly.load(std::memory_order_relaxed);
    k.mv = g_mvOn.load(std::memory_order_relaxed);
#ifdef WITH_DLAA
    strncpy_s(k.preset, SceneDlaa::DlssPresetName(), _TRUNCATE);
    k.sharp = SceneDlaa::Sharpness();
    k.r = SceneDlaa::SharpRadius();
    k.area = SceneDlaa::Area();
#else
    strncpy_s(k.preset, "n/a", _TRUNCATE);
#endif
    strncpy_s(k.up, g_upTag, _TRUNCATE);
    return k;
}
void ResetSpanWindow() {
    g_spanSceneSum = g_spanGbufSum = 0.0;
    g_spanSceneN = g_spanGbufN = 0;
    g_spanSceneSkip = g_spanGbufSkip = kSpanWarmup;
    g_spanSceneDropped = 0;
    if (++g_spanGen == 0) g_spanGen = 1;             // in-flight / open spans now belong to the old window
    g_spanKey = CurrentSpanKey();
}
// Pass start (inside StartPass, BEFORE the game's clear runs): close the previous pass's gbuf span (its depth
// snapshot, if any, was just taken), and arm the begins (issued by SpanBeginPending after the clear).
// `frameStart` = no unconsumed pass existed = first pass of a frame.
void SpanOnPassStart(ID3D11DeviceContext* ctx, uint64_t s, bool frameStart) {
    if (g_gbufSpan >= 0) { g_gbufRing.End(ctx, g_gbufSpan, true); g_gbufSpan = -1; }
    if (frameStart && g_sceneSpan >= 0) {          // previous frame never reached a clean last blit
        g_sceneRing.End(ctx, g_sceneSpan, false); g_sceneSpan = -1; ++g_spanSceneDropped;
    }
    const bool on = GpuTimingOn();
    g_spanPendGbuf = on;
    g_spanPendScene = on && frameStart;
    g_spanPendS = s;
}
// After the game's clear (hkClearDSV) or right after the first pass's G-buffer bind (hkOMSetRenderTargets).
void SpanBeginPending(ID3D11DeviceContext* ctx) {
    if (g_spanPendGbuf) {
        g_spanPendGbuf = false;
        g_gbufSpan = g_gbufRing.Begin(ctx, g_spanGen);
        g_gbufSpanS = g_spanPendS;
    }
    if (g_spanPendScene) {
        g_spanPendScene = false;
        g_sceneSpan = g_sceneRing.Begin(ctx, g_spanGen);
        g_sceneS0 = g_spanPendS; g_sceneUnd0 = g_cUnderflow; g_sceneOvf0 = g_cOverflow;
    }
}
// Scene-depth discard of the newest pass (after its snapshot, before the game's discard): the pass's gbuf span ends.
void SpanOnDiscard(ID3D11DeviceContext* ctx) {
    if (g_gbufSpan >= 0 && g_gbufSpanS + 1 == g_passSeq) { g_gbufRing.End(ctx, g_gbufSpan, true); g_gbufSpan = -1; }
}
// Matched blit that consumed pass `s` (called after the FIFO advanced, before any DLAA work).
void SpanOnBlit(ID3D11DeviceContext* ctx, uint64_t s) {
    if (g_gbufSpan >= 0 && g_gbufSpanS == s) { g_gbufRing.End(ctx, g_gbufSpan, true); g_gbufSpan = -1; }
    if (g_sceneSpan < 0 || g_fifoHead != g_passSeq) return;        // more passes of this frame still to blit
    const uint64_t passes = g_vrMode ? 2 : (g_flatMode ? 1 : 0);
    if (passes && s == g_sceneS0 + passes - 1 && g_passSeq == g_sceneS0 + passes &&
        g_cUnderflow == g_sceneUnd0 && g_cOverflow == g_sceneOvf0) {
        g_sceneEndAfterDraw = true;                                 // closed in hkDraw after the game's Draw
    } else {
        g_sceneRing.End(ctx, g_sceneSpan, false); g_sceneSpan = -1; ++g_spanSceneDropped;
    }
}
// FIFO underflow at a blit: the pairing is broken, drop the open scene span.
void SpanOnUnderflow(ID3D11DeviceContext* ctx) {
    if (g_sceneSpan >= 0) { g_sceneRing.End(ctx, g_sceneSpan, false); g_sceneSpan = -1; ++g_spanSceneDropped; }
}
// hkDraw, right after the game's Draw of the blit that SpanOnBlit marked.
void SpanAfterDraw(ID3D11DeviceContext* ctx) {
    g_sceneEndAfterDraw = false;
    if (g_sceneSpan >= 0) { g_sceneRing.End(ctx, g_sceneSpan, true); g_sceneSpan = -1; }
}
// Every matched blit: land finished samples, log every kSpanEvery scene samples (or 2x that many gbuf samples
// when frames never pair cleanly).
void SpanPollAndLog(ID3D11DeviceContext* ctx) {
    if (!(CurrentSpanKey() == g_spanKey)) ResetSpanWindow();   // safety net (self-test, jitter-only, ...)
    g_gbufRing.Poll(ctx, g_spanGen, &g_spanGbufSkip, &g_spanGbufSum, &g_spanGbufN);
    g_sceneRing.Poll(ctx, g_spanGen, &g_spanSceneSkip, &g_spanSceneSum, &g_spanSceneN);
    if (g_spanSceneN < kSpanEvery && g_spanGbufN < 2 * kSpanEvery) return;
    const SpanKey& k = g_spanKey;
    Log("GPU spans [dlaa=%s passive=%d preset=%s sharp=%.1f r=%.1f area=%d up=%s mv=%d jo=%d]: scene %.2f ms/frame | gbuf %.2f ms/pass | "
        "(n=%u, gbuf n=%u, scene spans dropped=%u, %s) | Present #%llu",
        k.dlaa ? "ON" : "OFF", (int)k.passive, k.preset, (double)k.sharp, (double)k.r, k.area, k.up, (int)k.mv, (int)k.jo,
        g_spanSceneN ? g_spanSceneSum / g_spanSceneN : 0.0, g_spanGbufN ? g_spanGbufSum / g_spanGbufN : 0.0,
        g_spanSceneN, g_spanGbufN, g_spanSceneDropped, g_vrMode ? "VR" : "flat",
        (unsigned long long)g_frames.load());
    g_spanSceneSum = g_spanGbufSum = 0.0;
    g_spanSceneN = g_spanGbufN = 0;
    g_spanSceneDropped = 0;
}
// Game context switched to a new device: the queries belong to the old one.
void SpanReleaseAll() {
    g_gbufRing.Release(); g_sceneRing.Release();
    g_gbufRing.broken = g_sceneRing.broken = false;
    g_gbufSpan = g_sceneSpan = -1;
    g_spanPendGbuf = g_spanPendScene = false;
    g_sceneEndAfterDraw = false;
    ResetSpanWindow();
}

// ---- v0.4.3 F11 frame trace ---------------------------------------------------------
// Records every interesting D3D11 event for ~2 frames into memory, writes dlaa_trace.txt at
// the end. Log-only: every hook passes straight through to the original.
bool PathNextToDll(const wchar_t* name, wchar_t* path);   // defined below

typedef void (STDMETHODCALLTYPE* Dispatch_t)(ID3D11DeviceContext*, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE* CopyResource_t)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
typedef void (STDMETHODCALLTYPE* CopySubresourceRegion_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
        ID3D11Resource*, UINT, const D3D11_BOX*);
typedef void (STDMETHODCALLTYPE* CopySubresourceRegion1_t)(ID3D11DeviceContext1*, ID3D11Resource*, UINT, UINT, UINT, UINT,
        ID3D11Resource*, UINT, const D3D11_BOX*, UINT);
typedef void (STDMETHODCALLTYPE* DiscardResource_t)(ID3D11DeviceContext1*, ID3D11Resource*);
typedef void (STDMETHODCALLTYPE* DiscardView_t)(ID3D11DeviceContext1*, ID3D11View*);
typedef void (STDMETHODCALLTYPE* DiscardView1_t)(ID3D11DeviceContext1*, ID3D11View*, const D3D11_RECT*, UINT);
typedef void (STDMETHODCALLTYPE* ResolveSubresource_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT);
typedef void (STDMETHODCALLTYPE* ClearRTV_t)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*);
typedef void (STDMETHODCALLTYPE* ClearDSV_t)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
typedef void (STDMETHODCALLTYPE* DrawIndexed_t)(ID3D11DeviceContext*, UINT, UINT, INT);
Dispatch_t                oDispatch = nullptr;
CopyResource_t            oCopyResource = nullptr;
CopySubresourceRegion_t   oCopySubresourceRegion = nullptr;
CopySubresourceRegion1_t  oCopySubresourceRegion1 = nullptr;
DiscardResource_t         oDiscardResource = nullptr;
DiscardView_t             oDiscardView = nullptr;
DiscardView1_t            oDiscardView1 = nullptr;
ResolveSubresource_t      oResolveSubresource = nullptr;
ClearRTV_t                oClearRTV = nullptr;
ClearDSV_t                oClearDSV = nullptr;
DrawIndexed_t             oDrawIndexed = nullptr;

constexpr size_t kTraceMaxLines = 20000;
int                  g_traceAutoFrame = 0;    // dlaa.ini trace_auto_frame (0 = off)
bool                 g_traceAutoDone = false;
std::atomic<bool>    g_traceRequest{false};
std::atomic<int>     g_traceState{0};           // 0 idle, 1 armed (starts at next Present), 2 recording
std::mutex           g_traceMx;
std::vector<std::string> g_traceLines;
uint64_t             g_traceIdx = 0, g_traceDropped = 0;
int                  g_traceFrame = 0;          // Presents seen since the trace started
uint64_t             g_traceStartPresent = 0;
const char*          g_traceCause = "";
struct TraceAgg {
    int kind = 0;                               // 0 none, 1 DrawIndexed run, 2 Dispatch run
    ID3D11DeviceContext* ctx = nullptr;
    void* rt0 = nullptr; void* dsv = nullptr;
    uint64_t count = 0, sum = 0; int frame = 0;
    std::string head;                           // context tag
    std::string tail;                           // " | RT0=... | DSV=..." (DrawIndexed only)
    UINT lx = 0, ly = 0, lz = 0;
};
TraceAgg g_agg;

inline bool TraceOn() { return !t_inDlaa && g_traceState.load(std::memory_order_relaxed) == 2; }

struct LB {                                     // tiny line builder
    char b[4096]; int n = 0;
    LB() { b[0] = 0; }
    void add(const char* fmt, ...) {
        if (n >= (int)sizeof(b) - 1) return;
        va_list ap; va_start(ap, fmt);
        const int w = vsnprintf(b + n, sizeof(b) - (size_t)n, fmt, ap);
        va_end(ap);
        if (w > 0) n = (n + w < (int)sizeof(b)) ? n + w : (int)sizeof(b) - 1;
    }
};

void TraceAddLocked(const std::string& s) {
    if (g_traceLines.size() >= kTraceMaxLines) { ++g_traceDropped; return; }
    g_traceLines.push_back(s);
}
void TraceFlushAggLocked() {
    if (!g_agg.kind) return;
    char t[128];
    if (g_agg.kind == 1) snprintf(t, sizeof(t), " DrawIndexed x %llu (indices total %llu)",
                                  (unsigned long long)g_agg.count, (unsigned long long)g_agg.sum);
    else snprintf(t, sizeof(t), " Dispatch x %llu (last groups %u,%u,%u)",
                  (unsigned long long)g_agg.count, g_agg.lx, g_agg.ly, g_agg.lz);
    char pre[48];
    snprintf(pre, sizeof(pre), "[%llu] f=%d ", (unsigned long long)g_traceIdx++, g_agg.frame);
    TraceAddLocked(std::string(pre) + g_agg.head + t + g_agg.tail);
    g_agg = TraceAgg();
}
void TraceEmit(const LB& b) {
    std::lock_guard<std::mutex> lk(g_traceMx);
    if (g_traceState.load() != 2) return;
    TraceFlushAggLocked();
    char pre[48];
    snprintf(pre, sizeof(pre), "[%llu] f=%d ", (unsigned long long)g_traceIdx++, g_traceFrame);
    TraceAddLocked(std::string(pre) + b.b);
}
// v0.6.2: IMM = the game's render context, FGN = any other immediate context (another device), DEF = deferred.
void CtxTag(LB& b, ID3D11DeviceContext* c) {
    b.add("%s@%p", IsGameCtx(c) ? "IMM" : (c->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? "FGN" : "DEF"), (void*)c);
}

// "<ptr>{Tex2D WxH arr mip fmt ms bind misc}" for any resource.
void DescRes(LB& b, ID3D11Resource* r) {
    b.add("%p", (void*)r);
    if (!r) return;
    D3D11_RESOURCE_DIMENSION dim{}; r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        ID3D11Texture2D* t = nullptr;
        if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) {
            D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d); t->Release();
            b.add("{Tex2D %ux%u arr=%u mip=%u fmt=%d ms=%u bind=0x%x misc=0x%x}",
                  d.Width, d.Height, d.ArraySize, d.MipLevels, (int)d.Format, d.SampleDesc.Count, d.BindFlags, d.MiscFlags);
        }
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        ID3D11Buffer* t = nullptr;
        if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Buffer), (void**)&t)) && t) {
            D3D11_BUFFER_DESC d{}; t->GetDesc(&d); t->Release();
            b.add("{Buffer bytes=%u bind=0x%x}", d.ByteWidth, d.BindFlags);
        }
    } else b.add("{dim=%d}", (int)dim);
}
void DescRtv(LB& b, ID3D11RenderTargetView* v) {
    if (!v) { b.add("null"); return; }
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    DescRes(b, r);
    if (r) r->Release();
    D3D11_RENDER_TARGET_VIEW_DESC d{}; v->GetDesc(&d);
    b.add(" view{dim=%d fmt=%d", (int)d.ViewDimension, (int)d.Format);
    if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) b.add(" mip=%u", d.Texture2D.MipSlice);
    else if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY)
        b.add(" mip=%u slice=%u n=%u", d.Texture2DArray.MipSlice, d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize);
    else if (d.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY)
        b.add(" slice=%u n=%u", d.Texture2DMSArray.FirstArraySlice, d.Texture2DMSArray.ArraySize);
    b.add("}");
}
void DescDsv(LB& b, ID3D11DepthStencilView* v) {
    if (!v) { b.add("null"); return; }
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    DescRes(b, r);
    if (r) r->Release();
    D3D11_DEPTH_STENCIL_VIEW_DESC d{}; v->GetDesc(&d);
    b.add(" view{dim=%d fmt=%d flags=0x%x", (int)d.ViewDimension, (int)d.Format, d.Flags);
    if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D) b.add(" mip=%u", d.Texture2D.MipSlice);
    else if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY)
        b.add(" mip=%u slice=%u n=%u", d.Texture2DArray.MipSlice, d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize);
    else if (d.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY)
        b.add(" slice=%u n=%u", d.Texture2DMSArray.FirstArraySlice, d.Texture2DMSArray.ArraySize);
    b.add("}");
}
void DescSrv(LB& b, ID3D11ShaderResourceView* v) {
    if (!v) { b.add("null"); return; }
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    DescRes(b, r);
    if (r) r->Release();
    D3D11_SHADER_RESOURCE_VIEW_DESC d{}; v->GetDesc(&d);
    b.add(" view{dim=%d fmt=%d", (int)d.ViewDimension, (int)d.Format);
    if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY)
        b.add(" mip=%u/%u slice=%u n=%u", d.Texture2DArray.MostDetailedMip, d.Texture2DArray.MipLevels,
              d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize);
    else if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
        b.add(" mip=%u/%u", d.Texture2D.MostDetailedMip, d.Texture2D.MipLevels);
    else if (d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY)
        b.add(" slice=%u n=%u", d.Texture2DMSArray.FirstArraySlice, d.Texture2DMSArray.ArraySize);
    b.add("}");
}

void TraceOM(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!TraceOn()) return;
    LB b; CtxTag(b, ctx); b.add(" OMSetRenderTargets n=%u", n);
    for (UINT i = 0; i < n && i < 8; ++i) {
        b.add(" | RTV%u=", i);
        DescRtv(b, rtvs ? rtvs[i] : nullptr);
    }
    b.add(" | DSV="); DescDsv(b, dsv);
    TraceEmit(b);
}
void TraceVp(ID3D11DeviceContext* ctx, UINT n, const D3D11_VIEWPORT* v) {
    if (!TraceOn()) return;
    LB b; CtxTag(b, ctx); b.add(" RSSetViewports n=%u", n);
    if (v && n) b.add(" vp0=(x=%.2f y=%.2f w=%.2f h=%.2f minZ=%.3f maxZ=%.3f)",
                      v->TopLeftX, v->TopLeftY, v->Width, v->Height, v->MinDepth, v->MaxDepth);
    TraceEmit(b);
}
void TraceDraw(ID3D11DeviceContext* ctx, UINT verts, UINT start) {
    if (!TraceOn()) return;
    LB b; CtxTag(b, ctx); b.add(" Draw verts=%u start=%u", verts, start);
    if (verts == 3 || verts == 4) {
        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        b.add(" | RT0="); DescRtv(b, rtv);
        if (rtv) rtv->Release();
        ID3D11ShaderResourceView* srv[4] = {};
        ctx->PSGetShaderResources(0, 4, srv);
        for (int i = 0; i < 4; ++i) {
            b.add(" | PS_SRV%d=", i); DescSrv(b, srv[i]);
            if (srv[i]) srv[i]->Release();
        }
    }
    TraceEmit(b);
}
void TraceDrawIndexed(ID3D11DeviceContext* ctx, UINT indexCount) {
    if (!TraceOn()) return;
    ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);
    void* rt0 = nullptr; void* dsvRes = nullptr;
    LB rb;
    if (rtv) { ID3D11Resource* r = nullptr; rtv->GetResource(&r); if (r) { rt0 = r; DescRes(rb, r); r->Release(); } rtv->Release(); }
    if (dsv) { ID3D11Resource* r = nullptr; dsv->GetResource(&r); if (r) { dsvRes = r; r->Release(); } dsv->Release(); }
    std::lock_guard<std::mutex> lk(g_traceMx);
    if (g_traceState.load() != 2) return;
    if (g_agg.kind == 1 && g_agg.ctx == ctx && g_agg.rt0 == rt0 && g_agg.dsv == dsvRes) {
        ++g_agg.count; g_agg.sum += indexCount; return;
    }
    TraceFlushAggLocked();
    g_agg.kind = 1; g_agg.ctx = ctx; g_agg.rt0 = rt0; g_agg.dsv = dsvRes;
    g_agg.count = 1; g_agg.sum = indexCount; g_agg.frame = g_traceFrame;
    LB h; CtxTag(h, ctx);
    g_agg.head = h.b;
    LB t; t.add(" | RT0=%s | DSV=%p", rt0 ? rb.b : "null", dsvRes);
    g_agg.tail = t.b;
}
void TraceDispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) {
    if (!TraceOn()) return;
    std::lock_guard<std::mutex> lk(g_traceMx);
    if (g_traceState.load() != 2) return;
    if (g_agg.kind == 2 && g_agg.ctx == ctx) { ++g_agg.count; g_agg.lx = x; g_agg.ly = y; g_agg.lz = z; return; }
    TraceFlushAggLocked();
    g_agg.kind = 2; g_agg.ctx = ctx; g_agg.count = 1; g_agg.frame = g_traceFrame;
    g_agg.lx = x; g_agg.ly = y; g_agg.lz = z;
    LB h; CtxTag(h, ctx);
    g_agg.head = h.b;
}
void AddBox(LB& b, const D3D11_BOX* bx) {
    if (bx) b.add(" box=(l=%u t=%u f=%u r=%u b=%u bk=%u)", bx->left, bx->top, bx->front, bx->right, bx->bottom, bx->back);
    else b.add(" box=none");
}
void TraceCopyRes(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src) {
    if (!TraceOn()) return;
    LB b; CtxTag(b, ctx); b.add(" CopyResource dst="); DescRes(b, dst); b.add(" src="); DescRes(b, src);
    TraceEmit(b);
}
void TraceCopySub(ID3D11DeviceContext* ctx, const char* name, ID3D11Resource* dst, UINT dsub, UINT dx, UINT dy, UINT dz,
                  ID3D11Resource* src, UINT ssub, const D3D11_BOX* box, UINT flags, bool hasFlags) {
    if (!TraceOn()) return;
    LB b; CtxTag(b, ctx); b.add(" %s dst=", name); DescRes(b, dst);
    b.add(" dstSub=%u dstXYZ=(%u,%u,%u) src=", dsub, dx, dy, dz); DescRes(b, src);
    b.add(" srcSub=%u", ssub); AddBox(b, box);
    if (hasFlags) b.add(" copyFlags=0x%x", flags);
    TraceEmit(b);
}
void TraceResolve(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT dsub, ID3D11Resource* src, UINT ssub, DXGI_FORMAT f) {
    if (!TraceOn()) return;
    LB b; CtxTag(b, ctx); b.add(" ResolveSubresource dst="); DescRes(b, dst);
    b.add(" dstSub=%u src=", dsub); DescRes(b, src); b.add(" srcSub=%u fmt=%d", ssub, (int)f);
    TraceEmit(b);
}
void TraceClearRtv(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* v, const FLOAT* c) {
    if (!TraceOn()) return;
    LB b; CtxTag(b, ctx); b.add(" ClearRenderTargetView "); DescRtv(b, v);
    if (c) b.add(" color=(%.3f,%.3f,%.3f,%.3f)", c[0], c[1], c[2], c[3]);
    TraceEmit(b);
}
void TraceClearDsv(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* v, UINT fl, FLOAT d, UINT8 s) {
    if (!TraceOn()) return;
    LB b; CtxTag(b, ctx); b.add(" ClearDepthStencilView "); DescDsv(b, v);
    b.add(" flags=0x%x depth=%.3f stencil=%u", fl, d, (unsigned)s);
    TraceEmit(b);
}

void STDMETHODCALLTYPE hkDispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) {
    TraceDispatch(ctx, x, y, z);
    oDispatch(ctx, x, y, z);
}
void STDMETHODCALLTYPE hkCopyResource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src) {
    TraceCopyRes(ctx, dst, src);
    oCopyResource(ctx, dst, src);
}
void STDMETHODCALLTYPE hkCopySubresourceRegion(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT dsub, UINT dx, UINT dy,
        UINT dz, ID3D11Resource* src, UINT ssub, const D3D11_BOX* box) {
    TraceCopySub(ctx, "CopySubresourceRegion", dst, dsub, dx, dy, dz, src, ssub, box, 0, false);
    oCopySubresourceRegion(ctx, dst, dsub, dx, dy, dz, src, ssub, box);
}
void STDMETHODCALLTYPE hkCopySubresourceRegion1(ID3D11DeviceContext1* ctx, ID3D11Resource* dst, UINT dsub, UINT dx, UINT dy,
        UINT dz, ID3D11Resource* src, UINT ssub, const D3D11_BOX* box, UINT flags) {
    TraceCopySub(ctx, "CopySubresourceRegion1", dst, dsub, dx, dy, dz, src, ssub, box, flags, true);
    oCopySubresourceRegion1(ctx, dst, dsub, dx, dy, dz, src, ssub, box, flags);
}
void STDMETHODCALLTYPE hkResolveSubresource(ID3D11DeviceContext* ctx, ID3D11Resource* dst, UINT dsub, ID3D11Resource* src,
        UINT ssub, DXGI_FORMAT f) {
    TraceResolve(ctx, dst, dsub, src, ssub, f);
    oResolveSubresource(ctx, dst, dsub, src, ssub, f);
}
void STDMETHODCALLTYPE hkClearRTV(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* v, const FLOAT* c) {
    TraceClearRtv(ctx, v, c);
    oClearRTV(ctx, v, c);
}
#ifdef WITH_DLAA
// ---- v0.7.8 profile / truck-preview screen DLAA (flat + VR) ----------------------------------------------------
// That screen has no world G-buffer. FLAT (frame trace dlaa_trace_profilescreen.txt), per frame, up to 4 times:
// clear RT_P (R11G11B10_FLOAT, backbuffer size) + DS_P (D32_FLOAT_S8X24_UINT), OMSetRenderTargets n=1 (RT_P + DS_P),
// viewport MinDepth 0.01 / MaxDepth 0.9, ~207 DrawIndexed of the truck, DiscardView DS_P, then a 3-vertex Draw onto the
// backbuffer with RT_P as PS SRV0 (the composite): pass, composite, pass, composite, ..., then the UI DrawIndexed.
// VR (dlaa_trace_profilescreen_vr_real.txt): per eye 4 passes into 4 DIFFERENT eye-sized RT_k sharing ONE DS_P, ALL
// passes first, then 4 composites (PS SRV0 = RT_3 .. RT_0) into the eye RT, then a Draw verts=96; eye 1 repeats it.
// The 4 passes are LAYERS the composites blend together (every layer carries visible edges), so DLAA runs ONCE PER
// COMPOSITE TARGET on the finished LDR composite, like the world path (round 4; rounds 1-3 ran one float DLAA per layer,
// 1.8-2.5 s NGX creation each):
//  * Slots = preview passes keyed by the bind order within the frame (kPvSlots = 8). Each remembers its RT_k; a composite
//    (3-vertex Draw, PS SRV0 = RT_k, RT0 = a target) takes the OLDEST not-yet-composited slot whose RT == SRV0.
//  * Targets = the RT0 of matched composites, indexed by the order of their first composite in the frame (kPtMax = 2:
//    flat = the backbuffer, VR = the two eye RTs). Round 5: the DLAA UNITS (one SceneDlaa each: own CameraMv history,
//    own NGX feature) belong to the EYE, not to the composite order -- flat: unit 0; VR: the eye of the target's RT,
//    from a small RT map corrected by projection verdicts (see PvEyeEntry). Round 4 tied unit k to "the k-th
//    composite target of the frame", and the game renders the two eyes in either order: each unit's history and MV
//    prev then came from the OTHER eye every swapped frame (VR log 20:37: R_world = the other eye's projection,
//    |R[0][3]| ~0.97, in every sample of both units) -- ghosting with preset K ("default").
//  * Reference pass: depth + MVs of a target come from the LOWEST slot composited into it (flat 0; VR 0 and 4). That
//    mapping is learned at every Present (g_ptRefMask); from the next frame on only the reference slots snapshot depth
//    (at DiscardView, before the next pass clears the shared DS) and collect MV candidates, the other slots' twins are
//    freed. The first frame of a screen therefore has no depth and runs nothing.
//  * A target is pending after its first matched composite. Its DLAA runs once, before the first of: (a) a Draw /
//    DrawIndexed on the game context while it is bound that is not a matched composite (flat: the UI, VR: the verts=96
//    Draw), (b) an OMSetRenderTargets that binds something else that is not a preview pass, (c) Present.
//  * Formats: the target is LDR 8-bit. R8G8B8A8 (VR eye RT, typeless): SceneDlaa runs in place (CopyResource, exactly the
//    world path). B8G8R8A8 (flat backbuffer): PreviewBlit copies it into an R8G8B8A8_TYPELESS scratch through an SRV / RTV
//    pair in the UNORM (non-sRGB) format (the draw's load / store converts the byte order, the gamma-encoded bytes travel
//    unchanged), SceneDlaa runs in place on the scratch, and a second PreviewBlit draw writes it back.
//    v0.8.0: with HDR output the target is the game's backbuffer-sized R16G16B16A16_FLOAT output target: SceneDlaa runs
//    in place as an HDR unit (RGBA16F colour, NGX IsHDR), exactly like the RGBA8 route; the Ctrl+F10 capture is 8-bit only.
// Nothing here touches g_ring / g_passSeq / g_fifoHead / g_dlaa. Render thread only.
constexpr int      kPvSlots       = 8;
constexpr int      kPtMax         = 2;
constexpr uint64_t kPvQuietFrames = 30;          // no world G-buffer bind for this many Presents before a bind can count
struct PvSlot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> rt;  // RT_k this frame's pass of the slot rendered into (ref dropped at Present)
    bool      composited = false;                // its composite Draw was handled this frame
    PassSlot  ps;                                // twin + cand (and the collector's skip counters, otherwise unused)
    // v0.7.8 preview capture (Ctrl+F10): the viewport decisions ReconcileViewports made while this pass was bound (full
    // preview-sized viewport 0 only): last applied shift, how often shifted / issued unshifted. Reset at the slot's bind.
    float     capShX = 0.0f, capShY = 0.0f;
    int       capShN = 0, capUnshN = 0;
    uint32_t  diCount = 0;                       // round 6: DrawIndexed calls while this pass was bound (diagnostics)
    D3D11_VIEWPORT compVp{};                     // round 6: viewport 0 of its composite Draw (layout log: where it lands)
    // v0.8.1 round 4: the viewport shift ReconcileViewports applied to this pass (tile px; reset at the slot's bind), for
    // the tile edge fix-up at its DS discard (PvFixTileEdges).
    float     shX = 0.0f, shY = 0.0f;
    bool      shOn = false;
    D3D11_VIEWPORT capVp{};                      // v0.8.1 round 5 seam capture: the game's last viewport 0 in this pass
};
struct PvTarget {                                // per frame, indexed by composite order (reset at Present)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex; // this frame's composite target (ref dropped at Present)
    bool      pending = false;                   // composited into, DLAA not run yet this frame
    int       curRef = 99;                       // lowest slot composited into it this frame
    int       slotMask = 0;                      // v0.7.8 capture: every slot composited into it this frame
    int       unit = -1;                         // round 5: unit (= eye) it was given this frame (-1 = not resolved / none)
    int       flushes = 0;                       // v0.8.1 round 5: PreviewFlush calls on it this frame (DLAA attempts)
    int       maskAtFlush = 0;                   // ... slotMask at the first of them
};
struct PvUnit {                                  // round 5: one DLAA unit per EYE (flat: unit 0), persistent
    SceneDlaa dl;                                // instance tag (SetEye) 10 + unit in the logs ("eye 10" = eye 0)
    uint64_t  lastFrame = 0, epoch = 0, runs = 0;
    int       lastRef = -1;                      // reference slot of the last successful Run (log only: VR 0 or 4)
    int       lastOrder = -1;                    // composite order of its target at the last successful Run (log only)
    bool      inited = false, unsupported = false;
    // BGRA path (flat backbuffer): UNORM views of the target + an RGBA8 scratch SceneDlaa runs on
    Microsoft::WRL::ComPtr<ID3D11Texture2D>           viewsOf;   // texture the target views below belong to
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>  tgtSrv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>    tgtRtv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>           scratch;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>  scratchSrv;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>    scratchRtv;
    UINT      sw = 0, sh = 0;                    // scratch size
};
PvSlot           g_pv[kPvSlots];
PvTarget         g_pt[kPtMax];
PvUnit           g_pu[kPtMax];
bool             g_previewPass  = false;         // current OM binding is a preview pass (jitter shift, MV collection)
int              g_pvCount      = 0;             // preview passes bound this frame (reset at Present)
int              g_ptCount      = 0;             // composite targets seen this frame
int              g_ptPendingMask = 0;            // bit idx = target idx is pending
int              g_ptBoundIdx   = -1;            // pending target that is the current RT0 (-1 none)
int              g_ptRefMask    = 0;             // learned reference slots (bit = slot); 0 = not learned yet
int              g_pvCur        = -1;            // slot of the newest preview pass (draw / discard target; -1 none)
Microsoft::WRL::ComPtr<ID3D11Texture2D> g_pvDs;   // DS_P of the current frame (shared by the passes; ref dropped at Present)
int              g_previewDlaa  = 1;             // dlaa.ini preview_dlaa (0 = the preview path never activates)
// v0.8.1 round 4 (black seam line on the TILED truck screen; see PvFixTileEdges). Debug switches, dlaa.ini:
int              g_pvEdgeFix    = 1;             // preview_edge_fix: 1 = fill the tile edge a > 0.5 px shift leaves uncovered
uint64_t         g_pvEdgeFixes  = 0;             // tile edge columns / rows filled in total (PvFixTileEdges)
uint64_t         g_pvEdgeGuarded = 0;            // round 5: zero-guarded edge fills dispatched in total (PvFixTileEdges)
int              g_pvTileJitter = 1;             // preview_tile_jitter: 0 = TILED passes get no viewport shift and NGX jitter 0
int              g_pvLayRecheck = 0;             // preview_layout_recheck: settled-layout re-check period in frames (0 = auto:
                                                 // 4 under a present layer, 31 otherwise)
// v0.8.1 round 5 (seam line, see PvSeam): debug switches + one diagnostic counter
int              g_pvSharpen    = 1;             // preview_sharpen: 0 = no RCAS on preview units (the world keeps it)
int              g_pvSeamAlways = 0;             // preview_seam_capture: 1 = the snapshot key runs the seam capture on 8-bit
                                                 // preview targets too (RGBA16F / HDR targets always get it)
uint64_t         g_pvLateComps  = 0;             // composites into a target AFTER its DLAA ran in the same frame
int              g_pvCaptureAt  = 0;             // dlaa.ini preview_capture_at (debug): Ctrl+F10 capture N Presents after the
bool             g_pvCaptureAtDone = false;      //   preview DLAA first ran (0 = off); once per session
uint64_t         g_pvFirstRunPresent = 0;        // Present of the first preview frame with a successful unit run
int64_t          g_pvT0         = 0;             // QPC / Present index at the start of the 600-frame fps window
uint64_t         g_pvF0         = 0;
UINT             g_pvW = 0, g_pvH = 0;           // RT_k size = the viewport size the jitter shift applies to
float            g_pvJx = 0.0f, g_pvJy = 0.0f;   // viewport shift of the whole frame (advances per Present, all passes share it)
uint64_t         g_pvEpoch      = 1;             // bumped by PreviewInvalidate: targets reset their DLSS history on a mismatch
uint64_t         g_lastGbufFrame = 0;            // g_frames at the last world G-buffer bind (4 RTVs + scene DSV)
bool             g_pvDetectLogged = false, g_pvRunLogged = false;
bool             g_pvHdrTarget  = false;         // v0.8.0: the last flushed preview target was RGBA16F (HDR output)
int              g_pvOkFrame    = 0;             // targets that ran OK this frame
uint64_t         g_pvFrames     = 0, g_pvRunsTotal = 0;   // frames with >= 1 OK target / OK runs in total

// v0.8.1 round 3: the CPU time of our preview work per frame (PreviewFlush, PreviewOnDiscard incl. the tile depth assembly
// and depth snapshots, PreviewNextFrame incl. the layout readback / verdict) and one line per tile-layout switch with
// the frames around it, so a test log tells a mod-side cost (resets, resource creation, a stale layout) from the game's.
int64_t          g_pvCpuTicks   = 0;             // this frame (outermost scopes only)
double           g_pvCpuRing[4] = {};            // the last 4 finished frames, ms
int              g_pvCpuHead    = 0;
uint64_t         g_pvInvalidates = 0;            // PreviewInvalidate calls (= preview DLSS history resets)
double           g_pvCreateMs   = 0.0;           // time in picture-depth (re)creation (PvEnsureAsm), summed
thread_local int t_pvCpuDepth   = 0;
struct PvCpuScope {
    int64_t t0;
    PvCpuScope() : t0(t_pvCpuDepth++ == 0 ? Qpc() : 0) {}
    ~PvCpuScope() { if (--t_pvCpuDepth == 0) g_pvCpuTicks += Qpc() - t0; }
};
struct PvSwitchRep {                             // armed by PvSolveLayout on a layout change, logged 8 frames later
    bool     armed = false;
    int      g = 0;
    bool     fromKnown = false, fromTiled = false, toTiled = false;
    uint64_t frame = 0, queued = 0, prevQueued = 0;
    double   before[4] = {};
    double   after[8] = {};
    int      nAfter = 0;
    uint64_t inv0 = 0;
    double   create0 = 0.0;
};
PvSwitchRep      g_pvSw;
int              g_pvSwLogs = 0;

void PreviewInvalidate() {
    for (PvUnit& u : g_pu) u.dl.Mv().Invalidate();
    ++g_pvEpoch;
    ++g_pvInvalidates;                           // v0.8.1 round 3 (switch report)
}

// ---- round 5: VR eye of a preview target (RT map + projection verdicts) ------------------------------------------
// Same principle as the world path (EyeOfRT + EyeVerdict): the eye comes from the target RT pointer, never from the
// order (the eye RTs rotate, 3 per eye; the eye order changes between frames). The map is learned from the MVPs: at
// Present, when both targets of the frame have a reference-slot candidate record, the first <= 8 world MVPs of each are
// GPU-copied into one small staging buffer and read back a few Presents later (DO_NOT_WAIT, never a stall). Projection
// x skew p = -dot(row0.xyz, row3.xyz) / |row3.xyz|^2 (EyeVerdict's p); the target with the LARGER mean p is eye 0. (The
// world rule "p > 0 = eye 0" does not hold on this screen: the 20:37 log's MVPs give p ~ +1.49 and ~ +0.52.)
// An RT not decided yet is left untouched (no DLAA, no history) -- a handful of frames on entering the screen; after
// kPvGuessFrames without any verdict it falls back to the composite order (the round 4 behaviour). A decided RT keeps a
// score (+1 per eye-0 verdict, -1 per eye-1, clamped +-6); a flip needs |score| >= 2 against it and drops both units'
// history (PreviewInvalidate). Two targets of one frame mapped to the same unit: the second one is left untouched.
// Render thread only; flat (one target, launch mode flat) never uses any of it.
struct PvEyeEntry { void* rt; int eye; int score; uint64_t firstFrame; };   // eye -1 = not decided yet
constexpr int      kPvEyeCap      = 16;
constexpr int      kPvRbRing      = 4;
constexpr int      kPvRbMvps      = 8;           // MVPs read back per target (world layer, record order)
constexpr uint64_t kPvGuessFrames = 30;          // no verdict for this many Presents -> composite-order guess
PvEyeEntry       g_pvEye[kPvEyeCap];
int              g_pvEyeN       = 0;
struct PvEyeReadback {
    ID3D11Buffer* staging = nullptr;             // 2 x kPvRbMvps x 64 bytes, STAGING + CPU read (held for the process life)
    void*         rt[2] = {};                    // target RT of each half (identity only)
    int           n[2] = {};                     // MVPs in each half
    uint64_t      frame = 0;                     // Present it was queued at
};
PvEyeReadback    g_pvRb[kPvRbRing];
int              g_pvRbHead = 0, g_pvRbPending = 0;
bool             g_pvRbInitTried = false, g_pvRbOk = false;
uint64_t         g_pvSettledN   = 0;             // Presents since the map settled (readback throttle)
int              g_pvUnitMask   = 0;             // units given to a target this frame (reset at Present)
uint64_t         g_pvVerdicts = 0, g_pvNoVote = 0, g_pvCorrections = 0, g_pvRbThrottled = 0;
uint64_t         g_pvOrderSwaps = 0;             // targets whose unit != composite order (the round 4 mix-up, per run)
uint64_t         g_pvConflicts = 0, g_pvUndecided = 0;   // targets left untouched: unit taken / eye unknown

int PvEyeFind(void* rt) {
    for (int i = 0; i < g_pvEyeN; ++i) if (g_pvEye[i].rt == rt) return i;
    return -1;
}

// v0.7.9: a preview target that is the backbuffer (pointer, or its exact size + format) is never a VR eye texture --
// a VR launch whose headset never came up composites the preview into the backbuffer (ATS 22:45, 3840x2160), and the
// eye map route then left it untouched for 30 frames and could never get a verdict (one target). Real VR eye RTs are
// eye-sized (6120x6496 here), not the mirror window's size.
bool PvIsBackbufferTarget(ID3D11Texture2D* tex) {
    if (!tex) return false;
    if ((void*)tex == g_backbuffer.load(std::memory_order_relaxed)) return true;
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    // v0.8.0: with HDR output the preview composites into the game's backbuffer-sized RGBA16F output target (encoded into
    // the backbuffer afterwards) -- the backbuffer's stand-in, same flat route.
    if (g_hdrOut && g_bbW && td.Width == g_bbW && td.Height == g_bbH && td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
        return true;
    return g_bbW && td.Width == g_bbW && td.Height == g_bbH && td.Format == g_bbFmt;
}

// Unit of target `idx` for this frame (resolved once per frame), or -1 = leave the target untouched this frame.
int PvUnitOf(int idx, uint64_t fr) {
    PvTarget& t = g_pt[idx];
    if (t.unit >= 0) return t.unit;
    int u = idx;                                         // flat / not a VR launch: the order (one target)
    if (g_launchVr == 1 && !PvIsBackbufferTarget(t.tex.Get())) {   // v0.7.9: a backbuffer target takes the flat route
        void* const rt = t.tex.Get();
        int e = PvEyeFind(rt);
        if (e < 0) {
            if (g_pvEyeN >= kPvEyeCap) {
                Log("preview eye map full (%d RTs): cleared", g_pvEyeN);
                g_pvEyeN = 0;
            }
            e = g_pvEyeN++;
            g_pvEye[e] = { rt, -1, 0, fr };
            if (g_pvEyeN <= 8)
                Log("preview eye map: new target RT %p (composite order %d, entry #%d) -- eye from the next projection verdict",
                    rt, idx, g_pvEyeN);
        }
        PvEyeEntry& en = g_pvEye[e];
        if (en.eye < 0 && fr - en.firstFrame > kPvGuessFrames) {
            en.eye = idx;
            static int guessLogs = 0;
            if (guessLogs++ < 8)
                Log("preview eye map: RT %p got no projection verdict in %llu Presents -> eye %d by composite order (guess)",
                    rt, (unsigned long long)kPvGuessFrames, idx);
        }
        if (en.eye < 0) { ++g_pvUndecided; return -1; }
        u = en.eye;
        if ((g_pvUnitMask >> u) & 1) {                   // the frame's other target already has this eye: map conflict
            if (g_pvConflicts++ < 10)
                Log("preview eye map: target %d (RT %p) maps to eye %d, already used this frame -- left untouched (conflict #%llu)",
                    idx, rt, u, (unsigned long long)g_pvConflicts);
            return -1;
        }
        if (u != idx) ++g_pvOrderSwaps;
    }
    g_pvUnitMask |= 1 << u;
    t.unit = u;
    return u;
}

bool PvEnsureEyeReadback(ID3D11DeviceContext* ctx) {
    if (g_pvRbInitTried) return g_pvRbOk;
    g_pvRbInitTried = true;
    Microsoft::WRL::ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 2 * kPvRbMvps * CandidateRecord::kSlotBytes;
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    g_pvRbOk = true;
    for (PvEyeReadback& r : g_pvRb)
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &r.staging)) || !r.staging) { g_pvRbOk = false; break; }
    if (!g_pvRbOk) Log("preview eye verdict: staging ring creation failed (VR preview targets fall back to the composite order)");
    return g_pvRbOk;
}

// Mean projection x skew over the finite MVPs of one half (n x 16 floats, rows). false = no usable MVP.
bool PvMeanSkew(const float* data, int n, double* pOut) {
    double sum = 0.0; int cnt = 0;
    for (int i = 0; i < n; ++i) {
        const float* m = data + (size_t)i * 16;
        bool finite = true;
        for (int k = 0; k < 16; ++k) if (!std::isfinite(m[k])) { finite = false; break; }
        if (!finite) continue;
        const float* r0 = m; const float* r3 = m + 12;
        const double d33 = (double)r3[0] * r3[0] + (double)r3[1] * r3[1] + (double)r3[2] * r3[2];
        if (d33 <= 1e-12) continue;
        sum += -((double)r0[0] * r3[0] + (double)r0[1] * r3[1] + (double)r0[2] * r3[2]) / d33;
        ++cnt;
    }
    if (!cnt) return false;
    *pOut = sum / cnt;
    return true;
}

void PvApplyVerdict(void* rt, int eye, double p) {
    const int i = PvEyeFind(rt);
    if (i < 0) return;                                   // map cleared since the readback was queued
    PvEyeEntry& e = g_pvEye[i];
    if (e.eye < 0) {                                     // first verdict decides an undecided RT directly
        e.eye = eye;
        e.score = eye == 0 ? 1 : -1;
        static int logs = 0;
        if (logs++ < 8) Log("preview eye map: RT %p -> eye %d by projection verdict (p=%.4f)", rt, eye, p);
        return;
    }
    e.score += eye == 0 ? 1 : -1;
    if (e.score > 6) e.score = 6;
    if (e.score < -6) e.score = -6;
    const int decided = e.score >= 2 ? 0 : (e.score <= -2 ? 1 : e.eye);
    if (decided != e.eye) {
        ++g_pvCorrections;
        if (g_pvCorrections <= 20)
            Log("preview eye map: RT %p corrected eye %d -> %d by projection verdict (score %d, p=%.4f) -- both units' "
                "history dropped", rt, e.eye, decided, e.score, p);
        e.eye = decided;
        PreviewInvalidate();
    }
}

// Poll pending readbacks oldest-first (stop at the first the GPU has not finished) and turn each into a verdict.
void PvPollEyeReadbacks(ID3D11DeviceContext* ctx) {
    while (g_pvRbPending > 0) {
        PvEyeReadback& r = g_pvRb[g_pvRbHead];
        D3D11_MAPPED_SUBRESOURCE ms{};
        const HRESULT hr = ctx->Map(r.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) break;
        g_pvRbHead = (g_pvRbHead + 1) % kPvRbRing; --g_pvRbPending;
        if (FAILED(hr) || !ms.pData) {
            if (SUCCEEDED(hr)) ctx->Unmap(r.staging, 0);
            continue;
        }
        double p[2] = {};
        bool ok[2];
        for (int i = 0; i < 2; ++i) ok[i] = PvMeanSkew((const float*)ms.pData + (size_t)i * kPvRbMvps * 16, r.n[i], &p[i]);
        ctx->Unmap(r.staging, 0);
        ++g_pvVerdicts;
        if (!ok[0] || !ok[1] || r.rt[0] == r.rt[1] || std::fabs(p[0] - p[1]) < 0.05) {
            if (g_pvNoVote++ < 5)
                Log("preview eye verdict: no vote (Present #%llu, p=%.4f/%.4f usable=%d/%d, RTs %p / %p)",
                    (unsigned long long)r.frame, p[0], p[1], (int)ok[0], (int)ok[1], r.rt[0], r.rt[1]);
            continue;
        }
        const int hi = p[0] > p[1] ? 0 : 1;              // larger x skew = eye 0
        if (g_pvVerdicts <= 6)
            Log("preview eye verdict (Present #%llu): order 0 RT %p p=%.4f, order 1 RT %p p=%.4f -> eye 0 = order %d",
                (unsigned long long)r.frame, r.rt[0], p[0], r.rt[1], p[1], hi);
        PvApplyVerdict(r.rt[hi], 0, p[hi]);
        PvApplyVerdict(r.rt[1 - hi], 1, p[1 - hi]);
    }
}

// Present (before the frame's targets are reset): queue this frame's two reference records for a verdict. Once the
// map is settled (>= 2 RTs, every score saturated) only every 31st frame (coprime to the 3-image RT cycle).
void PvQueueEyeReadback(ID3D11DeviceContext* ctx, uint64_t n) {
    if (g_ptCount != 2 || g_pvRbPending >= kPvRbRing) return;
    const CandidateRecord* rec[2] = {};
    int cnt[2] = {};
    for (int i = 0; i < 2; ++i) {
        const int ref = g_pt[i].curRef;
        if (!g_pt[i].tex || ref < 0 || ref >= kPvSlots) return;
        const CandidateRecord& c = g_pv[ref].ps.cand;
        if (!c.Ready() || !c.Buffer() || c.Count(0) <= 0) return;
        rec[i] = &c;
        cnt[i] = c.Count(0) < kPvRbMvps ? c.Count(0) : kPvRbMvps;
    }
    if (g_pt[0].tex.Get() == g_pt[1].tex.Get()) return;
    bool settled = g_pvEyeN >= 2;
    for (int i = 0; settled && i < g_pvEyeN; ++i)
        if (g_pvEye[i].score != 6 && g_pvEye[i].score != -6) settled = false;
    if (!settled) g_pvSettledN = 0;
    else if (g_pvSettledN++ % 31 != 0) { ++g_pvRbThrottled; return; }
    if (!PvEnsureEyeReadback(ctx)) return;
    PvEyeReadback& r = g_pvRb[(g_pvRbHead + g_pvRbPending) % kPvRbRing];
    for (int i = 0; i < 2; ++i) {
        const D3D11_BOX box{ 0, 0, 0, (UINT)cnt[i] * CandidateRecord::kSlotBytes, 1, 1 };   // layer 0 = the record's first slots
        ctx->CopySubresourceRegion(r.staging, 0, (UINT)(i * kPvRbMvps) * CandidateRecord::kSlotBytes, 0, 0,
                                   rec[i]->Buffer(), 0, &box);
        r.rt[i] = g_pt[i].tex.Get();
        r.n[i] = cnt[i];
    }
    r.frame = n;
    ++g_pvRbPending;
}

// ---- v0.7.8 round 6: TILED preview pictures --------------------------------------------------------------------------
// Proven by the Ctrl+F10 capture (VR 21:48): every composite target is built from N TILE passes. Each pass renders a part
// of the view at the FULL RT size -- its first MVP = the reference pass's with row0 += bx * row3 / row1 += by * row3
// (slot 1: x 6120 px = 2 ndc, slot 2: y 2 ndc, slot 3: both; rows 2 / 3 equal, same draw, same viewport shift) = 2x2
// tiles = the game's 2x supersampling -- and the composites shrink them into the target. Rounds 4 / 5 gave NGX the depth
// of the reference tile stretched over the whole picture (a corner of the backdrop: the depth BMP had no truck) and MVs
// from R in reference-tile clip space (the capture: "MVs do NOT reduce the error", mean 18-44 px with R ~ identity).
//  * Layout (PvLayout per composite order = group): the FIRST candidate MVP of every pass of the target (the same draw
//    in every pass) is read back asynchronously at Present (no stall); per pass a least-squares fit
//    row0 = ax * ref row0 + bx * ref row3, row1 = ay * ref row1 + by * ref row3, rows 2 / 3 equal, i.e. tile ndc =
//    a * reference-tile ndc + b. Tile k shows reference ndc x in [(-1 - bx) / ax, (1 - bx) / ax]; the PICTURE is the
//    union of the tiles: reference ndc = h * picture ndc + c (h = half extent, c = centre of the union). Accepted as
//    tiled only if every fit is exact (relative residual < 1e-3), a > 0 and the tile areas add up to the union
//    (0.98..1.02); otherwise ("untiled": one pass, depth slices, anything else) the round 5 reference-slot path runs
//    unchanged. Rechecked every Present until 8 identical results in a row, then every 31st; a change drops the history.
//    Where each tile lands is thus derived from the projections (the composites themselves draw full-target viewports,
//    logged as "composite vp"); the capture's depth / colour edge alignment verifies it on the real picture.
//  * Depth: at each tile pass's DiscardView (the shared DS still holds that pass) the DS is copied into ONE shared tile
//    twin and kPvDepthAsmShader reduces it into the group's picture-size R32F depth at the tile's rect: each picture pixel
//    takes the MAX (reversed-Z = NEAREST) of the tile texels it covers (2x2 here). Nearest, because a thin foreground
//    silhouette (the truck's edges, light against the backdrop) must keep the truck's depth and motion rather than the
//    backdrop's -- the usual closest-depth choice for temporal AA; the colour of such a pixel is mostly the truck too
//    (2 of its 4 samples or more). Complete once every tile of the layout went through this frame; NGX then gets it,
//    otherwise the target is left untouched this frame (logged as asm-miss).
//  * Motion: CameraMv::SetTileXf(hx, hy, cx, cy): pass B maps picture ndc into reference-tile ndc before R and back after
//    it (R_picture = S^-1 R_tile S), so the MVs are pixels of the composited picture.
//  * Jitter: a viewport shift of s tile pixels is s / (a * h) picture pixels after the composite's shrink (the tile's
//    texel spans 1 / (a * h) picture pixel; here a = 1, h = 2). Round 5 told NGX s while the picture moved s / 2. Now the
//    tile passes get g_pvJx * a * hx (g_pvJy * a * hy) and NGX gets g_pvJx / g_pvJy: the picture carries exactly the full
//    Halton offsets NGX is told (+-0.5 px), as in the world path.
// Render thread only.
template <class T> using PcPtr = Microsoft::WRL::ComPtr<T>;

// Compute-stage save / restore around our own dispatches outside SceneDlaa (the slots they use).
struct PvCsSave {
    ID3D11ComputeShader*       cs = nullptr;
    ID3D11ClassInstance*       inst[256] = {};
    UINT                       nInst = 256;
    ID3D11ShaderResourceView*  srv[7] = {};
    ID3D11UnorderedAccessView* uav = nullptr;
    ID3D11Buffer*              cb = nullptr;
    ID3D11SamplerState*        smp = nullptr;
    void Save(ID3D11DeviceContext* c) {
        c->CSGetShader(&cs, inst, &nInst);
        c->CSGetShaderResources(0, 7, srv);
        c->CSGetUnorderedAccessViews(0, 1, &uav);
        c->CSGetConstantBuffers(0, 1, &cb);
        c->CSGetSamplers(0, 1, &smp);
    }
    void Restore(ID3D11DeviceContext* c) {
        const UINT keep = (UINT)-1;
        c->CSSetShader(cs, inst, nInst);
        c->CSSetShaderResources(0, 7, srv);
        c->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
        c->CSSetConstantBuffers(0, 1, &cb);
        c->CSSetSamplers(0, 1, &smp);
        if (cs) cs->Release();
        for (UINT i = 0; i < nInst && i < 256; ++i) if (inst[i]) inst[i]->Release();
        for (auto* p : srv) if (p) p->Release();
        if (uav) uav->Release();
        if (cb) cb->Release();
        if (smp) smp->Release();
    }
};

bool PvSameKey(const CandidateRecord::DrawKey& a, const CandidateRecord::DrawKey& b) {
    return a.ib == b.ib && a.vb == b.vb && a.ibOffset == b.ibOffset && a.vbOffset == b.vbOffset &&
           a.indexCount == b.indexCount && a.startIndex == b.startIndex && a.baseVertex == b.baseVertex;
}

// kPvDepthAsmShader-BEGIN (the build validates this block with fxc)
const char kPvDepthAsmShader[] = R"(
cbuffer AsmCB : register(b0) {
    float4 Map;        // tile ndc = picture ndc * Map.xy + Map.zw (x, y)
    uint2  FullSize;   // picture (composite target) size
    uint2  TileSize;   // tile pass RT / DS size
    uint2  RectOrg;    // picture pixel rect the tile covers (dispatch origin, size)
    uint2  RectSize;
};
Texture2D<float>   TileDepth : register(t0);   // the tile pass's pre-discard depth (R32_FLOAT_X8X24 view of the twin)
RWTexture2D<float> FullDepth : register(u0);   // picture depth, cleared to 0 (far) at the frame's first tile

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= RectSize.x || id.y >= RectSize.y) return;
    uint2 p = RectOrg + id.xy;
    if (p.x >= FullSize.x || p.y >= FullSize.y) return;
    float2 fs = float2(FullSize);
    float2 ts = float2(TileSize);
    // the picture pixel's footprint [p, p + 1) -> picture ndc -> tile ndc -> tile texels
    float fx0 = float(p.x) / fs.x * 2.0 - 1.0, fx1 = float(p.x + 1) / fs.x * 2.0 - 1.0;
    float fy0 = 1.0 - float(p.y) / fs.y * 2.0, fy1 = 1.0 - float(p.y + 1) / fs.y * 2.0;
    float kx0 = fx0 * Map.x + Map.z, kx1 = fx1 * Map.x + Map.z;
    float ky0 = fy0 * Map.y + Map.w, ky1 = fy1 * Map.y + Map.w;
    float u0 = (min(kx0, kx1) + 1.0) * 0.5 * ts.x, u1 = (max(kx0, kx1) + 1.0) * 0.5 * ts.x;
    float v0 = (1.0 - max(ky0, ky1)) * 0.5 * ts.y, v1 = (1.0 - min(ky0, ky1)) * 0.5 * ts.y;
    int x0 = max((int)floor(u0 + 1e-3), 0), x1 = min((int)ceil(u1 - 1e-3), (int)TileSize.x);
    int y0 = max((int)floor(v0 + 1e-3), 0), y1 = min((int)ceil(v1 - 1e-3), (int)TileSize.y);
    if (x1 <= x0 || y1 <= y0) return;           // not covered by this tile
    x1 = min(x1, x0 + 4);
    y1 = min(y1, y0 + 4);
    float d = 0.0;
    [loop] for (int y = y0; y < y1; ++y)
        [loop] for (int x = x0; x < x1; ++x)
            d = max(d, TileDepth[int2(x, y)]);  // reversed-Z: max = the nearest surface
    FullDepth[p] = max(FullDepth[p], d);        // tiles meeting inside a pixel: nearest of both
}
)";
// kPvDepthAsmShader-END

void PvAsmRegisterShaders() {
    ShaderCache::Add(ShaderCache::kPvDepthAsm, kPvDepthAsmShader, sizeof(kPvDepthAsmShader) - 1, "pv_depth_asm", "CSMain", "cs_5_0");
}

struct PvTileFit { float ax = 1.0f, bx = 0.0f, ay = 1.0f, by = 0.0f; };   // tile ndc = a * reference-tile ndc + b
// v0.8.1 round 5: the layout fit compares the first kPvLayCands world candidates of every pass with the reference
// pass's (pairs of the SAME draw key, the exact fit wins), not only the two first candidates. The round 3 / 4 test logs
// classified the main menu as "NOT a clean tiling" in every verdict and the truck screen in about half of them: slots 0
// and 2 fit the 2x2 tiling exactly, slots 1 and 3 (the right column) with residual ~0.6 and an IDENTICAL a.x / b.x. The
// tile offset only changes rows 0 / 1 of the MVP, so row 3 (view depth) of every tile equals the reference's for the
// same object; slots 1 / 3 had a different row 3 -> their first sampled draw is ANOTHER INSTANCE of the same mesh (same
// IB / VB / offsets / counts, other world matrix: the game culls per tile, so the instance drawn first in the left tiles
// is not drawn in the right ones). The misread verdict ran the main menu on the untiled path (viewport shift scale 1 =
// the picture moved by half the jitter NGX was told; depth of the top-left tile stretched over the whole picture; MVs
// in tile space) and flipped the truck screen between both paths (a history reset at every flip).
constexpr int    kPvLayCands    = 16;
struct PvLayout {
    bool      known = false, tiled = false;
    int       slotMask = 0, ref = -1, nTiles = 0;
    PvTileFit t[kPvSlots];                       // per slot (the reference slot: 1, 0)
    float     hx = 1.0f, hy = 1.0f, cx = 0.0f, cy = 0.0f;   // reference-tile ndc = h * picture ndc + c
    UINT      W = 0, H = 0, Wt = 0, Ht = 0;      // picture (target) size, tile RT size
    int       same = 0;                          // identical verdicts in a row
    double    resid = 0.0, area = 0.0;           // worst fit residual, tile area / union area
    int       mRef[kPvSlots] = {}, mSlot[kPvSlots] = {};   // v0.8.1 round 5: candidate pair of the fit (log only)
};
PvLayout         g_pvLay[kPtMax];
int              g_pvSlotGroup[kPvSlots] = { -1, -1, -1, -1, -1, -1, -1, -1 };   // composite order of each slot (last frame)
struct PvLayRb {
    ID3D11Buffer* staging = nullptr;             // kPtMax x kPvSlots x kPvLayCands x 64 bytes (round 5: first MVPs of every pass)
    int      groups = 0;
    int      mask[kPtMax] = {}, ref[kPtMax] = {};
    bool     ok[kPtMax] = {};                    // every pass of the group had at least one world candidate
    UINT     W[kPtMax] = {}, H[kPtMax] = {}, Wt = 0, Ht = 0;
    uint64_t frame = 0;
    uint64_t prevFrame = 0;                      // v0.8.1 round 3: frame of the readback queued before this one (0 = none)
    int      n[kPtMax][kPvSlots] = {};           // v0.8.1 round 5: MVPs copied per pass + their draw keys
    CandidateRecord::DrawKey key[kPtMax][kPvSlots][kPvLayCands] = {};
};
constexpr int    kPvLayRing     = 4;
PvLayRb          g_pvLayRb[kPvLayRing];
int              g_pvLayHead = 0, g_pvLayPending = 0;
bool             g_pvLayInitTried = false, g_pvLayOk = false;
uint64_t         g_pvLaySettledN = 0, g_pvLayThrottled = 0, g_pvLayChanges = 0;
uint64_t         g_pvLayLastQueued = 0;          // v0.8.1 round 3: frame of the last queued layout readback
struct PvAsm {                                   // the picture depth of one group
    PcPtr<ID3D11Texture2D>           tex;        // R32_FLOAT, picture size, SRV + UAV
    PcPtr<ID3D11UnorderedAccessView> uav;
    PcPtr<ID3D11ShaderResourceView>  srv;
    UINT      w = 0, h = 0;
    int       frameMask = 0;                     // tiles reduced into it this frame (reset at Present)
    DepthTwin twin;                              // what SceneDlaa::Run gets (tex / srv above; valid = complete)
};
PvAsm            g_pvAsm[kPtMax];
DepthTwin        g_pvTileTwin;                   // the shared copy of the tile pass's DS (one tile at a time)
PcPtr<ID3D11ComputeShader> g_pvAsmCs;
PcPtr<ID3D11Buffer>        g_pvAsmCb;
bool             g_pvAsmFailed = false;
uint64_t         g_pvAsmMiss = 0, g_pvAsmTilesWin = 0;
// Round 6 diagnostics: preview flushes whose reference pass had no MV candidate (VR: the first ~14 s on the menu).
uint64_t         g_pvNoCandFlushes = 0;
bool             g_pvNoCandLogged = false, g_pvCandBackLogged = false;

// GPU cost of the depth assembly (shared DS copy + reduce per tile pass): non-blocking timestamp pairs, averaged and
// logged next to the preview stats line.
struct PvAsmTimer {
    struct Q { PcPtr<ID3D11Query> dis, t0, t1; bool busy = false; };
    Q        q[8];
    int      head = 0;
    bool     broken = false;
    double   sum = 0.0;
    uint64_t n = 0;
    int Begin(ID3D11DeviceContext* ctx) {
        if (broken) return -1;
        Q& s = q[head];
        if (s.busy) return -1;
        if (!s.dis) {
            PcPtr<ID3D11Device> dev;
            ctx->GetDevice(&dev);
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            if (!dev || FAILED(dev->CreateQuery(&qd, &s.dis))) { broken = true; return -1; }
            qd.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(dev->CreateQuery(&qd, &s.t0)) || FAILED(dev->CreateQuery(&qd, &s.t1))) { broken = true; return -1; }
        }
        ctx->Begin(s.dis.Get());
        ctx->End(s.t0.Get());
        const int i = head;
        head = (head + 1) % 8;
        return i;
    }
    void End(ID3D11DeviceContext* ctx, int i) {
        if (i < 0) return;
        ctx->End(q[i].t1.Get());
        ctx->End(q[i].dis.Get());
        q[i].busy = true;
    }
    void Poll(ID3D11DeviceContext* ctx) {
        for (Q& s : q) {
            if (!s.busy) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
            if (ctx->GetData(s.dis.Get(), &dd, sizeof(dd), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            UINT64 a = 0, b = 0;
            if (ctx->GetData(s.t0.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                ctx->GetData(s.t1.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
            s.busy = false;
            if (dd.Disjoint || !dd.Frequency || b < a) continue;
            sum += (double)(b - a) * 1000.0 / (double)dd.Frequency;
            ++n;
        }
    }
};
PvAsmTimer       g_pvAsmTimer;

// Viewport-shift factor of a tile pass (picture pixels -> tile pixels): a * h of its group's layout; 1 when untiled.
void PvShiftScale(int slot, float* kx, float* ky) {
    *kx = 1.0f; *ky = 1.0f;
    if (slot < 0 || slot >= kPvSlots) return;
    const int g = g_pvSlotGroup[slot];
    if (g < 0 || g >= kPtMax || !g_pvLay[g].tiled || !((g_pvLay[g].slotMask >> slot) & 1)) return;
    *kx = g_pvLay[g].t[slot].ax * g_pvLay[g].hx;
    *ky = g_pvLay[g].t[slot].ay * g_pvLay[g].hy;
    if (!g_pvTileJitter) { *kx = 0.0f; *ky = 0.0f; }     // v0.8.1 round 4 debug: dlaa.ini preview_tile_jitter = 0
}

// row t = a * u + b * v (4-vectors), least squares. false = degenerate. *res = |t - a u - b v| / |t|.
bool PvFitRow(const float* u, const float* v, const float* t, double* a, double* b, double* res) {
    double uu = 0, uv = 0, vv = 0, ut = 0, vt = 0, tt = 0;
    for (int i = 0; i < 4; ++i) {
        uu += (double)u[i] * u[i]; uv += (double)u[i] * v[i]; vv += (double)v[i] * v[i];
        ut += (double)u[i] * t[i]; vt += (double)v[i] * t[i]; tt += (double)t[i] * t[i];
    }
    const double det = uu * vv - uv * uv;
    if (!(std::fabs(det) > 1e-12 * uu * vv) || !std::isfinite(det)) return false;
    *a = (ut * vv - vt * uv) / det;
    *b = (uu * vt - uv * ut) / det;
    double r = 0;
    for (int i = 0; i < 4; ++i) { const double e = t[i] - *a * u[i] - *b * v[i]; r += e * e; }
    *res = std::sqrt(r) / (std::sqrt(tt) > 1e-9 ? std::sqrt(tt) : 1e-9);
    return std::isfinite(*a) && std::isfinite(*b);
}

// One group's verdict from a readback (data = kPtMax x kPvSlots x kPvLayCands MVPs, 16 floats each, rows).
// v0.8.1 round 5: per pass, every (reference candidate i, pass candidate j) pair with the same draw key is fitted; the
// pair with the smallest residual (fit + rows 2 / 3) is the pass's fit. Same instance -> exact (1e-8 in the logs); another
// instance of the mesh -> residual ~0.6, never chosen while the same instance is among the candidates. The first pair
// (0, 0) alone is the round 4 verdict.
void PvSolveLayout(int g, const PvLayRb& r, const float* data, uint64_t now) {
    PvLayout L;
    L.known = true; L.slotMask = r.mask[g]; L.ref = r.ref[g];
    L.W = r.W[g]; L.H = r.H[g]; L.Wt = r.Wt; L.Ht = r.Ht;
    auto mvp = [&](int s, int i) { return data + ((size_t)(g * kPvSlots + s) * kPvLayCands + (size_t)i) * 16; };
    bool fitOk = true;
    double worst = 0.0, areaSum = 0.0;
    double xmin = 1e30, xmax = -1e30, ymin = 1e30, ymax = -1e30;
    char why[96] = "";
    for (int s = 0; s < kPvSlots; ++s) {
        if (!((L.slotMask >> s) & 1)) continue;
        ++L.nTiles;
        double ax = 1, bx = 0, ay = 1, by = 0;
        if (s != L.ref) {
            double best = 1e30;
            int pairs = 0;
            for (int i = 0; i < r.n[g][L.ref]; ++i)
                for (int j = 0; j < r.n[g][s]; ++j) {
                    if (!PvSameKey(r.key[g][L.ref][i], r.key[g][s][j])) continue;
                    ++pairs;
                    const float* M0 = mvp(L.ref, i);
                    const float* M = mvp(s, j);
                    double fax, fbx, fay, fby, rx, ry;
                    if (!PvFitRow(M0, M0 + 12, M, &fax, &fbx, &rx) || !PvFitRow(M0 + 4, M0 + 12, M + 4, &fay, &fby, &ry))
                        continue;
                    double d23 = 0, n23 = 1e-9;          // rows 2 / 3 must be the reference's
                    for (int k = 8; k < 16; ++k) {
                        d23 = (std::max)(d23, (double)std::fabs(M[k] - M0[k]));
                        n23 = (std::max)(n23, (double)std::fabs(M0[k]));
                    }
                    const double res = (std::max)((std::max)(rx, ry), d23 / n23);
                    if (res < best) { best = res; ax = fax; bx = fbx; ay = fay; by = fby; L.mRef[s] = i; L.mSlot[s] = j; }
                }
            if (best >= 1e29) {
                fitOk = false;
                snprintf(why, sizeof(why), pairs ? "slot %d: degenerate fit" : "slot %d: no draw in common with the reference "
                         "among the first %d candidates", s, kPvLayCands);
                continue;
            }
            worst = (std::max)(worst, best);
        }
        if (!(ax > 1e-6) || !(ay > 1e-6)) { fitOk = false; snprintf(why, sizeof(why), "slot %d: a <= 0", s); continue; }
        L.t[s].ax = (float)ax; L.t[s].bx = (float)bx; L.t[s].ay = (float)ay; L.t[s].by = (float)by;
        const double x0 = (-1.0 - bx) / ax, x1 = (1.0 - bx) / ax, y0 = (-1.0 - by) / ay, y1 = (1.0 - by) / ay;
        xmin = (std::min)(xmin, x0); xmax = (std::max)(xmax, x1); ymin = (std::min)(ymin, y0); ymax = (std::max)(ymax, y1);
        areaSum += (x1 - x0) * (y1 - y0);
    }
    L.resid = worst;
    if (fitOk && worst >= 1e-3) { fitOk = false; snprintf(why, sizeof(why), "fit residual %.2e >= 1e-3", worst); }
    if (L.nTiles > 1 && fitOk) {
        L.hx = (float)((xmax - xmin) * 0.5); L.cx = (float)((xmax + xmin) * 0.5);
        L.hy = (float)((ymax - ymin) * 0.5); L.cy = (float)((ymax + ymin) * 0.5);
        const double ua = (xmax - xmin) * (ymax - ymin);
        L.area = areaSum / (ua > 1e-12 ? ua : 1e-12);
        L.tiled = L.area > 0.98 && L.area < 1.02 && L.W && L.H;
        if (!L.tiled) snprintf(why, sizeof(why), "tile area / union area %.4f (gaps or overlap)", L.area);
    }
    PvLayout& cur = g_pvLay[g];
    bool same = cur.known && cur.tiled == L.tiled && cur.slotMask == L.slotMask && cur.ref == L.ref && cur.W == L.W &&
                cur.H == L.H && cur.Wt == L.Wt && cur.Ht == L.Ht;
    if (same && L.tiled) {
        same = std::fabs(cur.hx - L.hx) < 1e-3f && std::fabs(cur.hy - L.hy) < 1e-3f && std::fabs(cur.cx - L.cx) < 1e-3f &&
               std::fabs(cur.cy - L.cy) < 1e-3f;
        for (int s = 0; same && s < kPvSlots; ++s)
            if ((L.slotMask >> s) & 1)
                same = std::fabs(cur.t[s].ax - L.t[s].ax) < 1e-3f && std::fabs(cur.t[s].bx - L.t[s].bx) < 1e-3f &&
                       std::fabs(cur.t[s].ay - L.t[s].ay) < 1e-3f && std::fabs(cur.t[s].by - L.t[s].by) < 1e-3f;
    }
    if (same) { if (cur.same < 1000000) ++cur.same; return; }
    L.same = 1;
    if (g_pvSwLogs < 20) {                               // v0.8.1 round 3: report the frames around this switch
        g_pvSw = PvSwitchRep();
        g_pvSw.armed = true; g_pvSw.g = g;
        g_pvSw.fromKnown = cur.known; g_pvSw.fromTiled = cur.tiled; g_pvSw.toTiled = L.tiled;
        g_pvSw.frame = now; g_pvSw.queued = r.frame; g_pvSw.prevQueued = r.prevFrame;
        for (int i = 0; i < 4; ++i) g_pvSw.before[i] = g_pvCpuRing[(g_pvCpuHead + i) % 4];   // oldest first
        g_pvSw.inv0 = g_pvInvalidates; g_pvSw.create0 = g_pvCreateMs;
    }
    cur = L;
    ++g_pvLayChanges;
    PreviewInvalidate();                                 // depth / MV / jitter space changed: no history across it
    if (g_pvLayChanges > 20) return;
    char buf[1100]; size_t bl = 0; buf[0] = 0;
    for (int s = 0; s < kPvSlots && bl + 160 < sizeof(buf); ++s) {
        if (!((L.slotMask >> s) & 1)) continue;
        const D3D11_VIEWPORT& v = g_pv[s].compVp;
        const int w = snprintf(buf + bl, sizeof(buf) - bl, " | slot %d a=(%.4f,%.4f) b=(%+.4f,%+.4f) cand %d/%d of %d composite "
                               "vp=(%.0f,%.0f %.0fx%.0f)", s, (double)L.t[s].ax, (double)L.t[s].ay, (double)L.t[s].bx,
                               (double)L.t[s].by, L.mRef[s], L.mSlot[s], r.n[g][s], (double)v.TopLeftX, (double)v.TopLeftY,
                               (double)v.Width, (double)v.Height);
        if (w > 0) bl += (size_t)w;
    }
    Log("preview tile layout (target order %d, Present #%llu): %s -- %d pass(es) (slot mask 0x%x, ref slot %d) of %ux%u -> "
        "picture %ux%u | picture ndc -> ref-tile ndc: x*%.4f%+.4f, y*%.4f%+.4f | tile ndc = a*ref + b:%s | worst fit residual "
        "%.2e, tile area / picture area %.4f%s%s", g, (unsigned long long)now,
        L.tiled ? "TILED -- depth assembled from every tile (nearest), MVs + jitter in picture space"
                : (L.nTiles <= 1 ? "untiled (one pass) -- reference-slot path" : "NOT a clean tiling -- reference-slot path"),
        L.nTiles, L.slotMask, L.ref, L.Wt, L.Ht, L.W, L.H, (double)L.hx, (double)L.cx, (double)L.hy, (double)L.cy, buf,
        L.resid, L.area, why[0] ? " | " : "", why);
}

bool PvEnsureLayoutReadback(ID3D11DeviceContext* ctx) {
    if (g_pvLayInitTried) return g_pvLayOk;
    g_pvLayInitTried = true;
    PcPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kPtMax * kPvSlots * kPvLayCands * CandidateRecord::kSlotBytes;   // round 5: 16 KB
    bd.Usage = D3D11_USAGE_STAGING;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    g_pvLayOk = true;
    for (PvLayRb& r : g_pvLayRb)
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &r.staging)) || !r.staging) { g_pvLayOk = false; break; }
    if (!g_pvLayOk) Log("preview tile layout: staging ring creation failed (the reference-slot path stays)");
    return g_pvLayOk;
}

void PvPollLayoutReadbacks(ID3D11DeviceContext* ctx, uint64_t now) {
    while (g_pvLayPending > 0) {
        PvLayRb& r = g_pvLayRb[g_pvLayHead];
        D3D11_MAPPED_SUBRESOURCE ms{};
        const HRESULT hr = ctx->Map(r.staging, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) break;
        g_pvLayHead = (g_pvLayHead + 1) % kPvLayRing; --g_pvLayPending;
        if (FAILED(hr) || !ms.pData) { if (SUCCEEDED(hr)) ctx->Unmap(r.staging, 0); continue; }
        for (int g = 0; g < r.groups && g < kPtMax; ++g)
            if (r.ok[g]) PvSolveLayout(g, r, (const float*)ms.pData, now);
        ctx->Unmap(r.staging, 0);
    }
}

// Present (before the frame's targets are reset): the first MVP of every pass of every target, for PvSolveLayout.
void PvQueueLayoutReadback(ID3D11DeviceContext* ctx, uint64_t n) {
    if (g_ptCount <= 0 || g_pvLayPending >= kPvLayRing) return;
    bool settled = true;
    for (int i = 0; i < g_ptCount && i < kPtMax; ++i)
        if (!g_pvLay[i].known || g_pvLay[i].same < 8) settled = false;
    if (!settled) g_pvLaySettledN = 0;
    // v0.8.1 round 3: a settled layout is re-checked every 31st frame -- ~70 ms at the 400+ Presents/s of the menus in
    // normal mode, but ~0.43 s at the 72 game frames/s counted under a present layer (ETS2 test): after the game switched
    // the picture between 4 tiles and 4 untiled passes (main menu <-> truck screen) the OLD layout was applied for up to
    // 0.43 s (tile jitter scale, depth and MVs of the wrong picture: a visible smear) before the verdict and its reset.
    // Under a present layer: every 4th frame (~55 ms at 72/s, the normal-mode time). Normal mode / VR unchanged.
    else if (g_pvLaySettledN++ % (g_pvLayRecheck > 0 ? (unsigned)g_pvLayRecheck
                                                      : (g_plActive.load(std::memory_order_relaxed) ? 4u : 31u)) != 0) {
        ++g_pvLayThrottled;                              // (v0.8.1 round 4: dlaa.ini preview_layout_recheck overrides)
        return;
    }
    if (!PvEnsureLayoutReadback(ctx)) return;
    PvLayRb& r = g_pvLayRb[(g_pvLayHead + g_pvLayPending) % kPvLayRing];
    bool any = false;
    r.groups = g_ptCount < kPtMax ? g_ptCount : kPtMax;
    r.Wt = g_pvW; r.Ht = g_pvH; r.frame = n; r.prevFrame = g_pvLayLastQueued;
    for (int i = 0; i < r.groups; ++i) {
        const PvTarget& t = g_pt[i];
        r.ok[i] = false; r.mask[i] = t.slotMask; r.ref[i] = t.curRef; r.W[i] = r.H[i] = 0;
        for (int s = 0; s < kPvSlots; ++s) r.n[i][s] = 0;
        if (!t.tex || t.curRef < 0 || t.curRef >= kPvSlots || !t.slotMask) continue;
        D3D11_TEXTURE2D_DESC td{};
        t.tex->GetDesc(&td);
        r.W[i] = td.Width; r.H[i] = td.Height;
        // round 5: every pass needs >= 1 world candidate; the key pairing (same draw) is the solver's job now
        bool ok = true;
        for (int s = 0; s < kPvSlots && ok; ++s) {
            if (!((t.slotMask >> s) & 1)) continue;
            const CandidateRecord& c = g_pv[s].ps.cand;
            ok = c.Ready() && c.Buffer() && c.Count(0) > 0;
        }
        if (!ok) continue;
        for (int s = 0; s < kPvSlots; ++s) {
            if (!((t.slotMask >> s) & 1)) continue;
            const CandidateRecord& c = g_pv[s].ps.cand;
            const int nc = c.Count(0) < kPvLayCands ? c.Count(0) : kPvLayCands;   // layer 0 = the buffer's first slots
            const D3D11_BOX box{ 0, 0, 0, (UINT)nc * CandidateRecord::kSlotBytes, 1, 1 };
            ctx->CopySubresourceRegion(r.staging, 0, (UINT)((i * kPvSlots + s) * kPvLayCands) * CandidateRecord::kSlotBytes,
                                       0, 0, c.Buffer(), 0, &box);
            for (int k = 0; k < nc; ++k) r.key[i][s][k] = c.Key(0, k);
            r.n[i][s] = nc;
        }
        r.ok[i] = true;
        any = true;
    }
    if (any) { ++g_pvLayPending; g_pvLayLastQueued = n; }
}

bool PvEnsureAsm(ID3D11DeviceContext* ctx, PvAsm& A, const PvLayout& L, int g) {
    if (g_pvAsmFailed) return false;
    PcPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    if (!g_pvAsmCs) {
        size_t size = 0;
        const void* code = ShaderCache::Code(ShaderCache::kPvDepthAsm, &size);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 48;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (!code || FAILED(dev->CreateComputeShader(code, size, nullptr, &g_pvAsmCs)) ||
            FAILED(dev->CreateBuffer(&bd, nullptr, &g_pvAsmCb))) {
            g_pvAsmFailed = true;
            Log("preview depth assembly: shader / constant buffer unavailable (%s) -- tiled pictures stay untouched",
                ShaderCache::Error(ShaderCache::kPvDepthAsm));
            return false;
        }
    }
    if (A.tex && A.w == L.W && A.h == L.H) return true;
    A.tex.Reset(); A.uav.Reset(); A.srv.Reset(); A.twin.Shutdown();
    const int64_t tc0 = Qpc();                           // v0.8.1 round 3 (switch report)
    D3D11_TEXTURE2D_DESC td{};
    td.Width = L.W; td.Height = L.H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    HRESULT hr;
    if (FAILED(hr = dev->CreateTexture2D(&td, nullptr, &A.tex)) ||
        FAILED(hr = dev->CreateUnorderedAccessView(A.tex.Get(), nullptr, &A.uav)) ||
        FAILED(hr = dev->CreateShaderResourceView(A.tex.Get(), nullptr, &A.srv))) {
        Log("preview depth assembly: %ux%u R32F create hr=0x%lx", L.W, L.H, (unsigned long)hr);
        A.tex.Reset(); A.uav.Reset(); A.srv.Reset();
        return false;
    }
    A.w = L.W; A.h = L.H;
    if (g_qpcFreq > 0) g_pvCreateMs += (double)(Qpc() - tc0) * 1000.0 / (double)g_qpcFreq;
    A.twin.tex = A.tex; A.twin.srv = A.srv; A.twin.w = L.W; A.twin.h = L.H; A.twin.valid = false;
    Log("preview depth assembly: picture depth %ux%u R32F created for target order %d (%.0f MB)", L.W, L.H, g,
        (double)L.W * (double)L.H * 4.0 / (1024.0 * 1024.0));
    return true;
}

// DiscardView of tile pass `slot` of group `g` (the shared DS holds it): copy + reduce into the picture depth.
void PvAssembleTile(ID3D11DeviceContext* ctx, int g, int slot) {
    const PvLayout& L = g_pvLay[g];
    PvAsm& A = g_pvAsm[g];
    if (!PvEnsureAsm(ctx, A, L, g)) return;
    t_inDlaa = true;
    const int tq = g_pvAsmTimer.Begin(ctx);
    if (!A.frameMask) {
        const FLOAT zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // far (reversed-Z) where no tile writes
        ctx->ClearUnorderedAccessViewFloat(A.uav.Get(), zero);
    }
    const bool ok = g_pvTileTwin.Snapshot(ctx, g_pvDs.Get(), false);
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (ok && SUCCEEDED(ctx->Map(g_pvAsmCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) {
        const PvTileFit& f = L.t[slot];
        const double Ax = (double)f.ax * L.hx, Bx = (double)f.ax * L.cx + f.bx;
        const double Ay = (double)f.ay * L.hy, By = (double)f.ay * L.cy + f.by;
        // the tile's picture-pixel rect (+1 px margin; the shader rejects pixels outside the tile)
        const double xf0 = (-1.0 - Bx) / Ax, xf1 = (1.0 - Bx) / Ax, yf0 = (-1.0 - By) / Ay, yf1 = (1.0 - By) / Ay;
        auto clampPx = [](double v, UINT lim) { return v < 0.0 ? 0u : (v > (double)lim ? lim : (UINT)v); };
        const UINT px0 = clampPx(std::floor((xf0 + 1.0) * 0.5 * L.W) - 1.0, L.W);
        const UINT px1 = clampPx(std::ceil((xf1 + 1.0) * 0.5 * L.W) + 1.0, L.W);
        const UINT py0 = clampPx(std::floor((1.0 - yf1) * 0.5 * L.H) - 1.0, L.H);
        const UINT py1 = clampPx(std::ceil((1.0 - yf0) * 0.5 * L.H) + 1.0, L.H);
        struct { float map[4]; UINT fw, fh, tw, th, ox, oy, rw, rh; } cb =
            { { (float)Ax, (float)Ay, (float)Bx, (float)By }, L.W, L.H, g_pvTileTwin.w, g_pvTileTwin.h, px0, py0,
              px1 > px0 ? px1 - px0 : 0u, py1 > py0 ? py1 - py0 : 0u };
        memcpy(mp.pData, &cb, sizeof(cb));
        ctx->Unmap(g_pvAsmCb.Get(), 0);
        if (cb.rw && cb.rh) {
            PvCsSave s;
            s.Save(ctx);
            ID3D11ShaderResourceView* srv = g_pvTileTwin.srv.Get();
            ID3D11ShaderResourceView* nullSrv = nullptr;
            ID3D11UnorderedAccessView* uav = A.uav.Get();
            ID3D11UnorderedAccessView* nullUav = nullptr;
            ID3D11Buffer* cbp = g_pvAsmCb.Get();
            const UINT keep = (UINT)-1;
            ctx->CSSetShader(g_pvAsmCs.Get(), nullptr, 0);
            ctx->CSSetConstantBuffers(0, 1, &cbp);
            ctx->CSSetShaderResources(0, 1, &srv);
            ctx->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
            ctx->Dispatch((cb.rw + 7) / 8, (cb.rh + 7) / 8, 1);
            ctx->CSSetShaderResources(0, 1, &nullSrv);
            ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
            s.Restore(ctx);
        }
        A.frameMask |= 1 << slot;
        ++g_pvAsmTilesWin;
        if (A.frameMask == L.slotMask) A.twin.valid = true;   // every tile is in: NGX may use it this frame
    }
    g_pvAsmTimer.End(ctx, tq);
    t_inDlaa = false;
    static int logs = 0;
    if (logs < 2 || (!ok && logs < 6)) {
        ++logs;
        Log("preview depth assembly: tile slot %d of target order %d %s", slot, g,
            ok ? "reduced into the picture depth" : "FAILED (DS copy)");
    }
}

bool PvIsRgba8(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}
bool PvIsBgra8(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}

// ---- v0.7.8 Ctrl+F10 preview capture (profile / truck-preview screen) -------------------------------------------------
// Diagnostic only: proves where the preview DLAA loses its effect (VR: "white flicker on straight truck edges, no
// difference DLAA on / off"). Started by the snapshot key while preview units run (the world F10 snapshot still takes
// the key in the world). Per preview unit: 1 priming frame (edge / depth tile counts -> the crop, prev copies) + 8
// measured consecutive frames. Every frame, right after the unit's Run (inside the flush, before the game's next draw):
//   * the target (= our copied-back result) is copied into a capture texture (the target is still bound as RT0, so it
//     cannot be read through an SRV), then ONE compute dispatch (kPvStatsShader) accumulates whole-image sums into a raw
//     buffer with 64-bit atomics: |out-in|, frame-to-frame |in-prevIn| / |out-prevOut|, motion-compensated versions
//     (prev sampled at p + mv, the DLSS convention), the warp error |warp(prevOut)-in| vs |prevOut-in|, all also on edge
//     pixels (luma gradient of the input > kPcEdgeThr) and on "truck edges" (edge + non-far depth), depth coverage,
//     edges sitting on far depth, MV statistics;
//   * the solve buffer (R_world) and the first MVP of EVERY pass composited into the target (all slots collect
//     candidates while the capture runs) are GPU-copied; the viewport shift each pass actually got is recorded by
//     ReconcileViewports (PvSlot::capSh*);
//   * 1536x1536 crops of in / out / depth / mv are copied into staging textures.
// At the Present of the same frame the target is copied again and compared with our result (pixels changed by the
// game's later draws). Nothing waits on the GPU until the end, except one tiny tile-count readback after the priming
// frame (crop choice). At the end: one blocking readback per unit, the "pvcap:" log lines, the BMPs in
// <dll folder>\dlaa_selftest\preview_<unit>_<frame>_{in,out,depth,mv}.bmp, a SUMMARY line per unit, everything released.
// VRAM while it runs: 3 full-size RGBA8 textures per unit (VR 6120x6496: ~480 MB per unit). Render thread only.
bool WriteBmp(const wchar_t* path, const uint8_t* src, UINT pitch, UINT w, UINT h);   // self-test section below
float HalfToFloat(uint16_t h);                                                         // F10 snapshot section below

// kPvStatsShader-BEGIN (the build validates this block with fxc)
const char kPvStatsShader[] = R"(
cbuffer StatsCB : register(b0) {
    uint2 Size;       // target size (px)
    uint  Base;       // byte offset of this frame's counter block in Acc
    uint  Flags;      // 1 = prev textures valid, 2 = "later draws" compare (Out vs PrevOut only), 4 = tile pass, 8 = Ngx bound
    float EdgeThr;    // luma gradient (central differences, x + y) above which a pixel is an edge
    float FarDepth;   // depth <= this = far plane (reversed-Z: the preview DS clears to 0, geometry is >= 0.01)
    uint  TileBase;   // byte offset of the tile counters (tile pass: 2 uints per 256x256 tile = edges, non-far)
    uint  TilesX;
};
Texture2D<float4> In      : register(t0);
Texture2D<float4> Out     : register(t1);
Texture2D<float4> PrevIn  : register(t2);
Texture2D<float4> PrevOut : register(t3);
Texture2D<float>  Depth   : register(t4);
Texture2D<float2> Mv      : register(t5);
Texture2D<float4> Ngx     : register(t6);   // NGX output before RCAS (unbound = 0: then Flags bit 3 is clear)
SamplerState      Lin     : register(s0);
RWByteAddressBuffer Acc   : register(u0);

#define NC 27
groupshared uint gs[NC];
groupshared uint gsMax;
groupshared uint gsInvMin;

uint Diff(float4 a, float4 b) { return (uint)(dot(abs(a.rgb - b.rgb), float3(1.0, 1.0, 1.0)) * 255.0 + 0.5); }
float Luma(int2 p) {
    int2 q = clamp(p, int2(0, 0), int2(Size) - int2(1, 1));
    return dot(In.Load(int3(q, 0)).rgb, float3(0.299, 0.587, 0.114));
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex, uint3 gid : SV_GroupID) {
    if (gi < NC) gs[gi] = 0;
    if (gi == 0) { gsMax = 0; gsInvMin = 0; }
    GroupMemoryBarrierWithGroupSync();

    uint v[NC];
    [unroll] for (int k = 0; k < NC; ++k) v[k] = 0;
    uint mvq = 0;
    bool inside = id.x < Size.x && id.y < Size.y;
    if (inside) {
        int2 p = int2(id.xy);
        int3 l = int3(p, 0);
        if ((Flags & 2u) != 0u) {
            uint d = Diff(Out.Load(l), PrevOut.Load(l));
            v[0] = 1; v[1] = d; v[2] = d > 3u ? 1u : 0u;
        } else {
            float g = abs(Luma(p + int2(1, 0)) - Luma(p - int2(1, 0))) + abs(Luma(p + int2(0, 1)) - Luma(p - int2(0, 1)));
            bool edge = g > EdgeThr;
            bool far = Depth.Load(l) <= FarDepth;
            if ((Flags & 4u) != 0u) {
                v[0] = edge ? 1u : 0u;
                v[1] = far ? 0u : 1u;
            } else {
                float4 cin  = In.Load(l);
                float4 cout = Out.Load(l);
                uint e  = edge ? 1u : 0u;
                uint te = (edge && !far) ? 1u : 0u;
                v[0] = 1;
                v[1] = Diff(cout, cin);
                v[2] = e;
                v[3] = e * v[1];
                float2 mv = Mv.Load(l);
                float m = length(mv);
                if (isnan(m)) m = 0;
                mvq = (uint)min(m * 16.0, 4.0e9);
                if ((Flags & 1u) != 0u) {
                    float4 pin  = PrevIn.Load(l);
                    float4 pout = PrevOut.Load(l);
                    float2 uv   = (float2(p) + 0.5 + mv) / float2(Size);   // DLSS: current + mv = previous
                    float4 win  = PrevIn.SampleLevel(Lin, uv, 0);
                    float4 wout = PrevOut.SampleLevel(Lin, uv, 0);
                    v[4]  = Diff(cin, pin);   v[5]  = Diff(cout, pout);
                    v[6]  = e * v[4];         v[7]  = e * v[5];
                    v[8]  = Diff(wout, cin);  v[9]  = Diff(pout, cin);
                    v[10] = e * v[8];         v[11] = e * v[9];
                    v[12] = Diff(win, cin);   v[13] = Diff(wout, cout);
                    v[14] = e * v[12];        v[15] = e * v[13];
                    v[23] = te * v[13];
                }
                v[16] = far ? 0u : 1u;
                v[17] = (edge && far) ? 1u : 0u;
                v[18] = mvq;
                v[19] = m > 0.5 ? 1u : 0u;
                v[20] = far ? 0u : mvq;
                v[21] = te;
                v[22] = te * v[1];
                if ((Flags & 8u) != 0u) {                       // what RCAS did to the NGX output
                    v[24] = Diff(cout, Ngx.Load(l));
                    v[25] = e * v[24];
                }
                if (edge) {                                     // round 6: a depth edge within 2 px of this colour edge?
                    float dmin = 1e9, dmax = -1e9;
                    [loop] for (int yy = -2; yy <= 2; ++yy)
                        [loop] for (int xx = -2; xx <= 2; ++xx) {
                            int2 q = clamp(p + int2(xx, yy), int2(0, 0), int2(Size) - int2(1, 1));
                            float dq = Depth.Load(int3(q, 0));
                            dmin = min(dmin, dq); dmax = max(dmax, dq);
                        }
                    v[26] = (dmax - dmin) > 0.05 * max(dmax, 1e-6) ? 1u : 0u;   // >= 5 % reversed-Z step = a silhouette
                }
            }
        }
    }
    [unroll] for (int k2 = 0; k2 < NC; ++k2) if (v[k2] != 0u) InterlockedAdd(gs[k2], v[k2]);
    if (inside && (Flags & 6u) == 0u) { InterlockedMax(gsMax, mvq); InterlockedMax(gsInvMin, 0xFFFFFFFFu - mvq); }
    GroupMemoryBarrierWithGroupSync();

    if ((Flags & 4u) != 0u) {
        if (gi < 2u && gs[gi] != 0u) {
            uint tile = (gid.y * 16u / 256u) * TilesX + (gid.x * 16u / 256u);
            Acc.InterlockedAdd(TileBase + tile * 8u + gi * 4u, gs[gi]);
        }
        return;
    }
    if (gi < NC) {
        uint val = gs[gi];
        if (val != 0u) {
            uint addr = Base + gi * 8u;
            uint orig;
            Acc.InterlockedAdd(addr, val, orig);
            if (orig + val < orig) Acc.InterlockedAdd(addr + 4u, 1u);   // 64-bit sum: carry into the high word
        }
    } else if (gi == NC) {
        Acc.InterlockedMax(Base + NC * 8u, gsMax);
    } else if (gi == NC + 1) {
        Acc.InterlockedMax(Base + NC * 8u + 4u, gsInvMin);
    }
}
)";
// kPvStatsShader-END

constexpr int   kPcMeasured  = 8;                // measured frames (after 1 priming frame)
constexpr UINT  kPcBlock     = 256;              // bytes per counter block: 27 x 64-bit sums, max, inverse min
constexpr UINT  kPcLaterBase = kPcMeasured * kPcBlock;          // "later draws" blocks follow the frame blocks
constexpr UINT  kPcTileBase  = 2 * kPcMeasured * kPcBlock;      // then the tile counters (priming frame)
constexpr UINT  kPcTile      = 256;              // px per crop-choice tile (multiple of the 16 px thread group)
constexpr UINT  kPcCrop      = 1536;             // crop side (px)
constexpr float kPcEdgeThr   = 0.20f;
constexpr float kPcFarDepth  = 0.0005f;
constexpr UINT  kPcSolveBytes = CameraMv::kSolveFloats * 4;

struct PvCapFrame {                              // CPU-side facts of one measured frame of one unit
    bool     ran = false, havePrev = false, mvDone = false, reset = false, rcas = false, solve = false, later = false;
    bool     worldMiss = false, tiled = false;
    float    tx[4] = { 1.0f, 1.0f, 0.0f, 0.0f };     // round 6: picture -> reference-tile ndc (TileXf) used this frame
    int      order = -1, ref = -1, pairs = 0, nSlots = 0;
    uint64_t present = 0;
    float    njx = 0.0f, njy = 0.0f;
    int      slot[kPvSlots] = {};
    float    shX[kPvSlots] = {}, shY[kPvSlots] = {};
    int      shN[kPvSlots] = {}, unshN[kPvSlots] = {}, candW[kPvSlots] = {};
    bool     mvp[kPvSlots] = {}, sameDraw[kPvSlots] = {};
};
struct PvCapUnit {
    bool     used = false, failed = false, tiles = false;
    UINT     w = 0, h = 0, tilesX = 0, tilesY = 0, accBytes = 0;
    PcPtr<ID3D11Texture2D>           prevIn, outTex[2];          // R8G8B8A8_TYPELESS, full size
    PcPtr<ID3D11ShaderResourceView>  prevInSrv, outSrv[2];       // UNORM views (the bytes as they are)
    int      cur = 0;                                            // outTex[cur] = this frame's, [cur ^ 1] = previous
    PcPtr<ID3D11Buffer>              acc, accStage, solveStage, mvpStage;
    PcPtr<ID3D11UnorderedAccessView> accUav;
    int      lastF = -1;                                         // capture frame of the last run
    uint64_t lastPresent = 0;
    bool     cropChosen = false;
    UINT     cx = 0, cy = 0, cw = 0, ch = 0;
    PcPtr<ID3D11Texture2D>           crop[kPcMeasured][4];       // staging: in, out, depth, mv
    bool     laterPending = false;
    int      laterF = -1;
    PcPtr<ID3D11Texture2D>           laterTex;                   // this frame's target (ref until Present)
    PvCapFrame fr[kPcMeasured];
};
struct PvCap {
    int      f = 0;                              // 0 = priming frame, 1..kPcMeasured = measured
    int      idle = 0;                           // Presents in a row without any unit run (abort at 60)
    uint64_t start = 0;
    PcPtr<ID3D11ComputeShader> cs;
    PcPtr<ID3D11Buffer>        cb;
    PcPtr<ID3D11SamplerState>  smp;
    PvCapUnit u[kPtMax];
    wchar_t  dir[MAX_PATH] = {};
};
bool   g_pvCapActive = false;
PvCap* g_pvCap = nullptr;                        // heap: only while a capture runs

void PvCapRegisterShaders() {
    ShaderCache::Add(ShaderCache::kPvStats, kPvStatsShader, sizeof(kPvStatsShader) - 1, "pv_stats", "CSMain", "cs_5_0");
}

void PvCapDispatch(ID3D11DeviceContext* ctx, PvCapUnit& c, UINT flags, UINT base, ID3D11ShaderResourceView* in,
                   ID3D11ShaderResourceView* out, ID3D11ShaderResourceView* prevIn, ID3D11ShaderResourceView* prevOut,
                   ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* mv, ID3D11ShaderResourceView* ngx = nullptr) {
    PvCap& P = *g_pvCap;
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(ctx->Map(P.cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) return;
    struct { UINT w, h, base, flags; float edgeThr, farDepth; UINT tileBase, tilesX; } cbd =
        { c.w, c.h, base, flags, kPcEdgeThr, kPcFarDepth, kPcTileBase, c.tilesX };
    memcpy(mp.pData, &cbd, sizeof(cbd));
    ctx->Unmap(P.cb.Get(), 0);
    PvCsSave s;
    s.Save(ctx);
    ID3D11ShaderResourceView* srvs[7] = { in, out, prevIn, prevOut, depth, mv, ngx };
    ID3D11ShaderResourceView* nulls[7] = {};
    ID3D11UnorderedAccessView* uav = c.accUav.Get();
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ID3D11Buffer* cb = P.cb.Get();
    ID3D11SamplerState* smp = P.smp.Get();
    const UINT keep = (UINT)-1;
    ctx->CSSetShader(P.cs.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetSamplers(0, 1, &smp);
    ctx->CSSetShaderResources(0, 7, srvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
    ctx->Dispatch((c.w + 15) / 16, (c.h + 15) / 16, 1);
    ctx->CSSetShaderResources(0, 7, nulls);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
    s.Restore(ctx);
}

bool PvCapMakeColor(ID3D11Device* dev, UINT w, UINT h, PcPtr<ID3D11Texture2D>& tex, PcPtr<ID3D11ShaderResourceView>& srv) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    return SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex)) && SUCCEEDED(dev->CreateShaderResourceView(tex.Get(), &sd, &srv));
}

bool PvCapMakeBuffer(ID3D11Device* dev, UINT bytes, bool raw, PcPtr<ID3D11Buffer>& buf) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    if (raw) {
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    } else {
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    }
    return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &buf));
}

// The unit's capture resources (first run in the capture). false = not possible (logged; the unit is skipped).
bool PvCapEnsureUnit(ID3D11DeviceContext* ctx, ID3D11Device* dev, PvCapUnit& c, int ui, UINT w, UINT h) {
    if (c.acc) return c.w == w && c.h == h;
    c.w = w; c.h = h;
    c.tilesX = (w + kPcTile - 1) / kPcTile;
    c.tilesY = (h + kPcTile - 1) / kPcTile;
    c.accBytes = (kPcTileBase + c.tilesX * c.tilesY * 8 + 15) & ~15u;
    bool ok = PvCapMakeColor(dev, w, h, c.prevIn, c.prevInSrv) && PvCapMakeColor(dev, w, h, c.outTex[0], c.outSrv[0]) &&
              PvCapMakeColor(dev, w, h, c.outTex[1], c.outSrv[1]) && PvCapMakeBuffer(dev, c.accBytes, true, c.acc) &&
              PvCapMakeBuffer(dev, c.accBytes, false, c.accStage) &&
              PvCapMakeBuffer(dev, kPcMeasured * kPcSolveBytes, false, c.solveStage) &&
              PvCapMakeBuffer(dev, kPcMeasured * kPvSlots * CandidateRecord::kSlotBytes, false, c.mvpStage);
    if (ok) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = c.accBytes / 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        ok = SUCCEEDED(dev->CreateUnorderedAccessView(c.acc.Get(), &ud, &c.accUav));
    }
    if (!ok) {
        Log("pvcap: unit %d capture resources for %ux%u could not be created (VRAM?) -- unit skipped", ui, w, h);
        return false;
    }
    const UINT zero[4] = { 0, 0, 0, 0 };
    ctx->ClearUnorderedAccessViewUint(c.accUav.Get(), zero);
    Log("pvcap: unit %d capture resources created: 3 x %ux%u RGBA8 (%.0f MB VRAM until the capture ends) + counters", ui, w, h,
        3.0 * (double)w * (double)h * 4.0 / (1024.0 * 1024.0));
    return true;
}

void PvCapStart(ID3D11DeviceContext* ctx, uint64_t n) {
    PcPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    size_t size = 0;
    const void* code = ShaderCache::Code(ShaderCache::kPvStats, &size);
    if (!dev || !code) { Log("pvcap: not started -- stats shader unavailable (%s)", ShaderCache::Error(ShaderCache::kPvStats)); return; }
    PvCap* P = new (std::nothrow) PvCap();
    if (!P) return;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 32;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateComputeShader(code, size, nullptr, &P->cs)) || FAILED(dev->CreateBuffer(&bd, nullptr, &P->cb)) ||
        FAILED(dev->CreateSamplerState(&sd, &P->smp)) || !PathNextToDll(L"dlaa_selftest\\", P->dir)) {
        Log("pvcap: not started -- shader / constant buffer / sampler / path creation failed");
        delete P;
        return;
    }
    CreateDirectoryW(P->dir, nullptr);
    P->start = n;
    g_pvCap = P;
    g_pvCapActive = true;
    Log("pvcap: started (snapshot key on the preview screen, Present #%llu) -- 1 priming + %d measured frames per preview "
        "unit; edge = input luma gradient > %.2f, far = depth <= %.4f; levels below are mean |difference| per colour "
        "channel in 8-bit steps (0..255)", (unsigned long long)n, kPcMeasured, (double)kPcEdgeThr, (double)kPcFarDepth);
}


// Right after unit `ui`'s successful Run on target `tg` (composite order `order`) in this frame `fr`.
void PvCapAfterRun(ID3D11DeviceContext* ctx, int ui, int order, PvTarget& tg, int ref, float njx, float njy, bool bgra,
                   UINT w, UINT h, uint64_t fr) {
    PvCap& P = *g_pvCap;
    PvCapUnit& c = P.u[ui];
    PvUnit& pu = g_pu[ui];
    SceneDlaa& dl = pu.dl;
    if (c.failed || c.laterPending) return;
    PcPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    ID3D11Texture2D* outSrc = bgra ? pu.scratch.Get() : tg.tex.Get();
    if (!dev || !outSrc || !dl.ColorInTex() || !dl.ColorInSrv() || !dl.DepthSrv() || !dl.MvSrv()) { c.failed = true; return; }
    t_inDlaa = true;
    if (!PvCapEnsureUnit(ctx, dev.Get(), c, ui, w, h)) { c.failed = true; t_inDlaa = false; return; }
    const int f = P.f;
    c.used = true;
    ctx->CopyResource(c.outTex[c.cur].Get(), outSrc);    // our result as it sits in the target now
    if (f == 0) {
        PvCapDispatch(ctx, c, 4u, 0, dl.ColorInSrv(), nullptr, nullptr, nullptr, dl.DepthSrv(), nullptr);
        c.tiles = true;
    } else {
        const int m = f - 1;
        PvCapFrame& F = c.fr[m];
        F.ran = true;
        F.havePrev = c.lastF == f - 1 && c.lastPresent + 1 == fr;
        ID3D11ShaderResourceView* const ngx = dl.LastSharpened() ? dl.NgxOutSrv() : nullptr;   // RCAS ran: its input
        PvCapDispatch(ctx, c, (F.havePrev ? 1u : 0u) | (ngx ? 8u : 0u), (UINT)m * kPcBlock, dl.ColorInSrv(),
                      c.outSrv[c.cur].Get(), c.prevInSrv.Get(), c.outSrv[c.cur ^ 1].Get(), dl.DepthSrv(), dl.MvSrv(), ngx);
        F.present = fr; F.order = order; F.ref = ref; F.njx = njx; F.njy = njy;
        F.mvDone = dl.LastMvDone(); F.reset = dl.LastReset(); F.rcas = dl.LastSharpened();
        F.pairs = dl.Mv().Last().pairs[0]; F.worldMiss = dl.Mv().Last().worldMiss;
        if (order >= 0 && order < kPtMax && g_pvLay[order].tiled) {
            const PvLayout& lay = g_pvLay[order];
            F.tiled = true; F.tx[0] = lay.hx; F.tx[1] = lay.hy; F.tx[2] = lay.cx; F.tx[3] = lay.cy;
        }
        if (F.mvDone && dl.Mv().SolveSrv()) {                // R of THIS frame (the solve buffer is rewritten every frame)
            PcPtr<ID3D11Resource> sres;
            dl.Mv().SolveSrv()->GetResource(&sres);
            const D3D11_BOX box{ 0, 0, 0, kPcSolveBytes, 1, 1 };
            if (sres) { ctx->CopySubresourceRegion(c.solveStage.Get(), 0, (UINT)m * kPcSolveBytes, 0, 0, sres.Get(), 0, &box); F.solve = true; }
        }
        const CandidateRecord& rc = g_pv[ref].ps.cand;
        int k = 0;
        for (int s = 0; s < kPvSlots; ++s) {
            if (!((tg.slotMask >> s) & 1)) continue;
            const PvSlot& sl = g_pv[s];
            F.slot[k] = s; F.shX[k] = sl.capShX; F.shY[k] = sl.capShY; F.shN[k] = sl.capShN; F.unshN[k] = sl.capUnshN;
            const CandidateRecord& cr = sl.ps.cand;
            F.candW[k] = cr.Ready() ? cr.Count(0) : 0;
            if (cr.Ready() && cr.Buffer() && cr.Count(0) > 0) {
                const D3D11_BOX box{ 0, 0, 0, CandidateRecord::kSlotBytes, 1, 1 };   // first world MVP
                ctx->CopySubresourceRegion(c.mvpStage.Get(), 0, (UINT)(m * kPvSlots + k) * CandidateRecord::kSlotBytes, 0, 0,
                                           cr.Buffer(), 0, &box);
                F.mvp[k] = true;
                F.sameDraw[k] = rc.Ready() && rc.Count(0) > 0 && PvSameKey(cr.Key(0, 0), rc.Key(0, 0));
            }
            ++k;
        }
        F.nSlots = k;
        if (c.cropChosen) {
            const D3D11_BOX box{ c.cx, c.cy, 0, c.cx + c.cw, c.cy + c.ch, 1 };
            ID3D11Texture2D* src[4] = { dl.ColorInTex(), c.outTex[c.cur].Get(), dl.DepthTex(), dl.MvTex() };
            for (int q = 0; q < 4; ++q)
                if (c.crop[m][q] && src[q]) ctx->CopySubresourceRegion(c.crop[m][q].Get(), 0, 0, 0, 0, src[q], 0, &box);
        }
        if (!bgra) { c.laterTex = tg.tex; c.laterF = m; c.laterPending = true; }   // compared again at Present (VR)
    }
    ctx->CopyResource(c.prevIn.Get(), dl.ColorInTex());
    c.lastF = f; c.lastPresent = fr;
    c.cur ^= 1;                                          // [cur ^ 1] = this frame's result from now on
    t_inDlaa = false;
}

// After the priming frame: blocking readback of the tile counts, crop = the 1536 px window with the most edge pixels.
void PvCapChooseCrop(ID3D11DeviceContext* ctx, PcPtr<ID3D11Device>& dev, PvCapUnit& c, int ui) {
    ctx->CopyResource(c.accStage.Get(), c.acc.Get());
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(ctx->Map(c.accStage.Get(), 0, D3D11_MAP_READ, 0, &mp))) { Log("pvcap: unit %d tile readback failed -- no crops", ui); return; }
    const uint32_t* t = (const uint32_t*)((const uint8_t*)mp.pData + kPcTileBase);
    const UINT wt = (kPcCrop / kPcTile) < c.tilesX ? kPcCrop / kPcTile : c.tilesX;
    const UINT ht = (kPcCrop / kPcTile) < c.tilesY ? kPcCrop / kPcTile : c.tilesY;
    uint64_t totE = 0, totNf = 0, best = 0;
    UINT bx = 0, by = 0;
    for (UINT i = 0; i < c.tilesX * c.tilesY; ++i) { totE += t[i * 2]; totNf += t[i * 2 + 1]; }
    for (UINT y0 = 0; y0 + ht <= c.tilesY; ++y0)
        for (UINT x0 = 0; x0 + wt <= c.tilesX; ++x0) {
            uint64_t s = 0;
            for (UINT y = y0; y < y0 + ht; ++y)
                for (UINT x = x0; x < x0 + wt; ++x) s += t[(y * c.tilesX + x) * 2];
            if (s > best) { best = s; bx = x0; by = y0; }
        }
    ctx->Unmap(c.accStage.Get(), 0);
    c.cw = kPcCrop < c.w ? kPcCrop : c.w;
    c.ch = kPcCrop < c.h ? kPcCrop : c.h;
    c.cx = bx * kPcTile; if (c.cx > c.w - c.cw) c.cx = c.w - c.cw;
    c.cy = by * kPcTile; if (c.cy > c.h - c.ch) c.cy = c.h - c.ch;
    const DXGI_FORMAT fmts[4] = { DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R32_FLOAT,
                                  DXGI_FORMAT_R16G16_FLOAT };
    bool ok = true;
    for (int m = 0; m < kPcMeasured && ok; ++m)
        for (int q = 0; q < 4 && ok; ++q) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = c.cw; td.Height = c.ch; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = fmts[q]; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &c.crop[m][q]));
        }
    if (!ok) {
        for (auto& row : c.crop) for (auto& p : row) p.Reset();
        Log("pvcap: unit %d crop staging creation failed -- numbers only, no BMPs", ui);
        return;
    }
    c.cropChosen = true;
    const double px = (double)c.w * (double)c.h;
    Log("pvcap: unit %d crop %ux%u at (%u,%u) of %ux%u = the window with the most edge pixels (%llu of %llu edge px in the "
        "image, %.2f%% of all px are edges; non-far depth covers %.1f%% of the image)", ui, c.cw, c.ch, c.cx, c.cy, c.w, c.h,
        (unsigned long long)best, (unsigned long long)totE, px > 0 ? 100.0 * (double)totE / px : 0.0,
        px > 0 ? 100.0 * (double)totNf / px : 0.0);
}

void PvCapFinish(ID3D11DeviceContext* ctx, bool aborted);

// Present (after the frame's flushes): the "later draws" compare, the crop choice after the priming frame, next frame.
void PvCapAtPresent(ID3D11DeviceContext* ctx, uint64_t n) {
    PvCap& P = *g_pvCap;
    bool any = false;
    for (int ui = 0; ui < kPtMax; ++ui) {
        PvCapUnit& c = P.u[ui];
        if (c.used && c.lastF == P.f) any = true;        // ran since the last advance
        if (!c.laterPending) continue;
        c.laterPending = false;
        t_inDlaa = true;
        ctx->CopyResource(c.outTex[c.cur].Get(), c.laterTex.Get());   // [cur] is free until the next run
        PvCapDispatch(ctx, c, 2u, kPcLaterBase + (UINT)c.laterF * kPcBlock, nullptr, c.outSrv[c.cur].Get(), nullptr,
                      c.outSrv[c.cur ^ 1].Get(), nullptr, nullptr);
        t_inDlaa = false;
        c.fr[c.laterF].later = true;
        c.laterTex.Reset();
    }
    if (!any) {
        if (++P.idle >= 60) PvCapFinish(ctx, true);
        return;
    }
    P.idle = 0;
    if (P.f == 0) {
        PcPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        for (int ui = 0; ui < kPtMax; ++ui)
            if (dev && P.u[ui].used && P.u[ui].tiles && !P.u[ui].failed) PvCapChooseCrop(ctx, dev, P.u[ui], ui);
    }
    if (++P.f > kPcMeasured) PvCapFinish(ctx, false);
}

// Mean projection x / y skew of one MVP (EyeVerdict's p / q).
void PvSkew(const float* m, double* p, double* q) {
    const double d33 = (double)m[12] * m[12] + (double)m[13] * m[13] + (double)m[14] * m[14];
    *p = d33 > 1e-12 ? -((double)m[0] * m[12] + (double)m[1] * m[13] + (double)m[2] * m[14]) / d33 : 0.0;
    *q = d33 > 1e-12 ? -((double)m[4] * m[12] + (double)m[5] * m[13] + (double)m[6] * m[14]) / d33 : 0.0;
}

bool PvCapWriteCrop(ID3D11DeviceContext* ctx, ID3D11Texture2D* st, int kind, const wchar_t* path, UINT w, UINT h,
                    float* dMin, float* dMax) {
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &mp))) return false;
    bool ok;
    if (kind < 2) {
        ok = WriteBmp(path, (const uint8_t*)mp.pData, mp.RowPitch, w, h);
    } else {
        std::vector<uint8_t> rgba((size_t)w * h * 4);
        if (kind == 2) {                                 // depth: far = dark blue, else min..max of the crop -> 40..255
            float lo = 1e9f, hi = -1e9f;
            for (UINT y = 0; y < h; ++y) {
                const float* r = (const float*)((const uint8_t*)mp.pData + (size_t)y * mp.RowPitch);
                for (UINT x = 0; x < w; ++x) if (r[x] > kPcFarDepth) { if (r[x] < lo) lo = r[x]; if (r[x] > hi) hi = r[x]; }
            }
            *dMin = lo; *dMax = hi;
            const float span = hi > lo ? hi - lo : 1.0f;
            for (UINT y = 0; y < h; ++y) {
                const float* r = (const float*)((const uint8_t*)mp.pData + (size_t)y * mp.RowPitch);
                for (UINT x = 0; x < w; ++x) {
                    uint8_t* o = &rgba[((size_t)y * w + x) * 4];
                    if (r[x] <= kPcFarDepth) { o[0] = 0; o[1] = 0; o[2] = 64; }
                    else { const uint8_t g = (uint8_t)(40.0f + 215.0f * (r[x] - lo) / span); o[0] = o[1] = o[2] = g; }
                    o[3] = 255;
                }
            }
        } else {                                         // mv: R = 128 + 32 * mv.x, G = 128 + 32 * mv.y, B = |mv| > 0.5 px
            for (UINT y = 0; y < h; ++y) {
                const uint16_t* r = (const uint16_t*)((const uint8_t*)mp.pData + (size_t)y * mp.RowPitch);
                for (UINT x = 0; x < w; ++x) {
                    const float mx = HalfToFloat(r[x * 2]), my = HalfToFloat(r[x * 2 + 1]);
                    auto q8 = [](float v) { v = 128.0f + 32.0f * v; return (uint8_t)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v)); };
                    uint8_t* o = &rgba[((size_t)y * w + x) * 4];
                    o[0] = q8(mx); o[1] = q8(my); o[2] = (mx * mx + my * my > 0.25f) ? 255 : 0; o[3] = 255;
                }
            }
        }
        ok = WriteBmp(path, rgba.data(), w * 4, w, h);
    }
    ctx->Unmap(st, 0);
    return ok;
}

void PvCapFinish(ID3D11DeviceContext* ctx, bool aborted) {
    PvCap& P = *g_pvCap;
    t_inDlaa = true;
    for (int ui = 0; ui < kPtMax; ++ui) {
        PvCapUnit& c = P.u[ui];
        if (!c.used || c.failed || !c.acc) continue;
        ctx->CopyResource(c.accStage.Get(), c.acc.Get());
        D3D11_MAPPED_SUBRESOURCE ma{}, ms{}, mm{};
        if (FAILED(ctx->Map(c.accStage.Get(), 0, D3D11_MAP_READ, 0, &ma))) { Log("pvcap: unit %d counter readback failed", ui); continue; }
        const bool haveS = SUCCEEDED(ctx->Map(c.solveStage.Get(), 0, D3D11_MAP_READ, 0, &ms));
        const bool haveM = SUCCEEDED(ctx->Map(c.mvpStage.Get(), 0, D3D11_MAP_READ, 0, &mm));
        const uint32_t* A = (const uint32_t*)ma.pData;
        // averages for the summary (frames with a previous frame only for the temporal numbers)
        double sOutIn = 0, sOutInE = 0, sFlIn = 0, sFlOut = 0, sMcIn = 0, sMcOut = 0, sMcInE = 0, sMcOutE = 0, sMcOutT = 0;
        double sWarp = 0, sUnwarp = 0, sWarpE = 0, sUnwarpE = 0, sLater = 0, sNf = 0, sEdgeFar = 0, sMv = 0, sMvMax = 0;
        double sFlInE = 0, sFlOutE = 0, sRcas = 0, sRcasE = 0, sAlign = 0;
        int nTiled = 0;
        int nF = 0, nP = 0, nL = 0, nReset = 0, nRcas = 0, nMv = 0;
        for (int m = 0; m < kPcMeasured; ++m) {
            const PvCapFrame& F = c.fr[m];
            if (!F.ran) { Log("pvcap: unit %d frame %d: unit did not run in this capture frame", ui, m); continue; }
            const uint32_t* B = A + (size_t)m * kPcBlock / 4;
            auto S = [&](int k) { return (double)(((uint64_t)B[k * 2 + 1] << 32) | B[k * 2]); };
            const double N = S(0), Ne = S(2), Nt = S(21);
            auto L = [](double sum, double cnt) { return cnt > 0 ? sum / (3.0 * cnt) : 0.0; };
            const double mvMax = (double)B[54] / 16.0, mvMin = (double)(0xFFFFFFFFu - B[55]) / 16.0;
            char later[96] = "n/a (flat target or no Present compare)";
            if (F.later) {
                const uint32_t* Lb = A + (size_t)(kPcLaterBase + m * kPcBlock) / 4;
                auto SL = [&](int k) { return (double)(((uint64_t)Lb[k * 2 + 1] << 32) | Lb[k * 2]); };
                const double ln = SL(0);
                const double pct = ln > 0 ? 100.0 * SL(2) / ln : 0.0;
                snprintf(later, sizeof(later), "%.2f%% px changed, mean %.3f", pct, L(SL(1), ln));
                sLater += pct; ++nL;
            }
            Log("pvcap: unit %d frame %d (Present #%llu, target order %d, ref slot %d): out-in %.3f (edges %.3f, truck-edges %.3f) | "
                "after later draws: %s | depth non-far %.1f%%, edge px %.2f%% of image, edge px on far depth %.1f%%, colour edges with a "
                "depth edge within 2 px %.1f%% | "
                "mv mean %.3f max %.2f min %.3f px, >0.5 px on %.1f%%, mean on non-far %.3f px | reset=%d mvDone=%d (pairs %d%s) "
                "rcas=%d (RCAS changed the NGX output by %.3f, edges %.3f) ngx jitter=(%+.4f,%+.4f) prev=%d | tiled=%d picture->ref-tile "
                "ndc x*%.3f%+.3f y*%.3f%+.3f", ui, m,
                (unsigned long long)F.present, F.order, F.ref,
                L(S(1), N), L(S(3), Ne), L(S(22), Nt), later, N > 0 ? 100.0 * S(16) / N : 0.0, N > 0 ? 100.0 * Ne / N : 0.0,
                Ne > 0 ? 100.0 * S(17) / Ne : 0.0, Ne > 0 ? 100.0 * S(26) / Ne : 0.0, N > 0 ? S(18) / 16.0 / N : 0.0, mvMax, mvMin,
                N > 0 ? 100.0 * S(19) / N : 0.0, S(16) > 0 ? S(20) / 16.0 / S(16) : 0.0, (int)F.reset, (int)F.mvDone, F.pairs,
                F.worldMiss ? ", WORLD MISS" : "", (int)F.rcas, L(S(24), N), L(S(25), Ne), (double)F.njx, (double)F.njy,
                (int)F.havePrev, (int)F.tiled, (double)F.tx[0], (double)F.tx[2], (double)F.tx[1], (double)F.tx[3]);
            if (F.havePrev)
                Log("pvcap: unit %d frame %d temporal: raw flicker in->out %.3f->%.3f (edges %.3f->%.3f) | motion-compensated "
                    "flicker in->out %.3f->%.3f (edges %.3f->%.3f, truck-edges out %.3f) | warp error warped/unwarped "
                    "%.3f/%.3f (edges %.3f/%.3f)", ui, m, L(S(4), N), L(S(5), N), L(S(6), Ne), L(S(7), Ne), L(S(12), N),
                    L(S(13), N), L(S(14), Ne), L(S(15), Ne), L(S(23), Nt), L(S(8), N), L(S(9), N), L(S(10), Ne), L(S(11), Ne));
            // passes of this target: viewport shift applied, candidates, game projection offset vs the reference slot
            {
                char buf[1400]; size_t bl = 0; buf[0] = 0;
                const float* refM = nullptr;
                for (int k = 0; k < F.nSlots; ++k)
                    if (F.slot[k] == F.ref && F.mvp[k] && haveM)
                        refM = (const float*)((const uint8_t*)mm.pData + (size_t)(m * kPvSlots + k) * CandidateRecord::kSlotBytes);
                for (int k = 0; k < F.nSlots && bl + 200 < sizeof(buf); ++k) {
                    int wr = snprintf(buf + bl, sizeof(buf) - bl, " | slot %d shift=(%+.4f,%+.4f) shifted=%d unshifted=%d cand=%d",
                                      F.slot[k], (double)F.shX[k], (double)F.shY[k], F.shN[k], F.unshN[k], F.candW[k]);
                    if (wr > 0) bl += (size_t)wr;
                    if (F.mvp[k] && haveM && refM) {
                        const float* M = (const float*)((const uint8_t*)mm.pData + (size_t)(m * kPvSlots + k) * CandidateRecord::kSlotBytes);
                        double p, q, pr, qr;
                        PvSkew(M, &p, &q); PvSkew(refM, &pr, &qr);
                        double rd[4] = {};
                        for (int r = 0; r < 4; ++r)
                            for (int cc = 0; cc < 4; ++cc) {
                                const double d = std::fabs((double)M[r * 4 + cc] - (double)refM[r * 4 + cc]);
                                if (d > rd[r]) rd[r] = d;
                            }
                        wr = snprintf(buf + bl, sizeof(buf) - bl, " same-draw=%d proj-offset vs ref=(%+.3f,%+.3f) px rowdiff=%.3g/%.3g/%.3g/%.3g",
                                      (int)F.sameDraw[k], -(p - pr) * c.w / 2.0, (q - qr) * c.h / 2.0, rd[0], rd[1], rd[2], rd[3]);
                        if (wr > 0) bl += (size_t)wr;
                    }
                }
                Log("pvcap: unit %d frame %d passes (%d):%s", ui, m, F.nSlots, buf);
            }
            if (F.solve && haveS) {
                const float* R = (const float*)((const uint8_t*)ms.pData + (size_t)m * kPcSolveBytes);
                Log("pvcap: unit %d frame %d R_world [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] "
                    "[%.5f %.5f %.5f %.5f] (pairs %.0f valid %.0f pick %.0f)", ui, m, R[0], R[1], R[2], R[3], R[4], R[5], R[6],
                    R[7], R[8], R[9], R[10], R[11], R[12], R[13], R[14], R[15], R[32], R[33], R[34]);
            }
            ++nF;
            sAlign += Ne > 0 ? 100.0 * S(26) / Ne : 0.0; nTiled += F.tiled ? 1 : 0;
            sOutIn += L(S(1), N); sOutInE += L(S(3), Ne); sRcas += L(S(24), N); sRcasE += L(S(25), Ne);
            sNf += N > 0 ? 100.0 * S(16) / N : 0.0; sEdgeFar += Ne > 0 ? 100.0 * S(17) / Ne : 0.0;
            sMv += N > 0 ? S(18) / 16.0 / N : 0.0; if (mvMax > sMvMax) sMvMax = mvMax;
            nReset += F.reset ? 1 : 0; nRcas += F.rcas ? 1 : 0; nMv += F.mvDone ? 1 : 0;
            if (F.havePrev) {
                ++nP;
                sFlIn += L(S(4), N); sFlOut += L(S(5), N); sFlInE += L(S(6), Ne); sFlOutE += L(S(7), Ne);
                sMcIn += L(S(12), N); sMcOut += L(S(13), N); sMcInE += L(S(14), Ne); sMcOutE += L(S(15), Ne);
                sMcOutT += L(S(23), Nt);
                sWarp += L(S(8), N); sUnwarp += L(S(9), N); sWarpE += L(S(10), Ne); sUnwarpE += L(S(11), Ne);
            }
        }
        if (haveM) ctx->Unmap(c.mvpStage.Get(), 0);
        if (haveS) ctx->Unmap(c.solveStage.Get(), 0);
        ctx->Unmap(c.accStage.Get(), 0);
        // crops -> BMP
        int files = 0;
        if (c.cropChosen) {
            static const char* kKind[4] = { "in", "out", "depth", "mv" };
            for (int m = 0; m < kPcMeasured; ++m) {
                if (!c.fr[m].ran) continue;
                float dLo = 0, dHi = 0;
                for (int q = 0; q < 4; ++q) {
                    if (!c.crop[m][q]) continue;
                    wchar_t name[64];
                    swprintf_s(name, L"preview_%d_%d_%S.bmp", ui, m, kKind[q]);
                    wchar_t path[MAX_PATH];
                    wcscpy_s(path, P.dir); wcscat_s(path, name);
                    if (PvCapWriteCrop(ctx, c.crop[m][q].Get(), q, path, c.cw, c.ch, &dLo, &dHi)) ++files;
                    else Log("pvcap: FAILED to write preview_%d_%d_%s.bmp", ui, m, kKind[q]);
                }
                if (m == 0)
                    Log("pvcap: unit %d crop files: depth BMP = far plane dark blue, else grey 40..255 over depth %.5f..%.5f; "
                        "mv BMP = R 128+32*mv.x, G 128+32*mv.y (px), B 255 where |mv| > 0.5 px", ui, (double)dLo, (double)dHi);
            }
        }
        const double k1 = nF ? 1.0 / nF : 0.0, kp = nP ? 1.0 / nP : 0.0;
        const double mcIn = sMcInE * kp, mcOut = sMcOutE * kp;
        Log("pvcap SUMMARY unit %d (eye tag %d): %d measured frames (%d with a previous frame), %d BMPs written%s | DLAA changes "
            "the picture by %.3f levels/channel (edges %.3f) | flicker on edge px, motion-compensated: input %.3f, output %.3f -> "
            "output is %s (%.0f%% of input) | raw frame-to-frame on edge px: input %.3f, output %.3f | MV check (out[n-1] warped "
            "by MV vs not, against in[n]): all %.3f vs %.3f, edges %.3f vs %.3f -> MVs %s | truck-edge output flicker %.3f | "
            "%s | depth: %.1f%% of px non-far, %.1f%% of edge px sit on far depth, %.1f%% of colour edges have a depth edge within "
            "2 px (depth aligned with the picture) | tiled picture path in %d/%d frames | mv mean %.3f px, max %.2f px | NGX reset in "
            "%d/%d frames, RCAS ran %d/%d (changed the NGX output by %.3f, edges %.3f), MV pass ran %d/%d", ui, 10 + ui, nF, nP, files, aborted ? " (ABORTED: units stopped running)" : "",
            sOutIn * k1, sOutInE * k1, mcIn, mcOut, mcOut < mcIn ? "MORE stable" : "NOT more stable",
            mcIn > 0 ? 100.0 * mcOut / mcIn : 0.0, sFlInE * kp, sFlOutE * kp, sWarp * kp, sUnwarp * kp, sWarpE * kp,
            sUnwarpE * kp, sWarpE < sUnwarpE ? "reduce the error (they fit)" : "do NOT reduce the error",
            sMcOutT * kp, nL ? "" : "later draws: not measured", sNf * k1, sEdgeFar * k1, sAlign * k1, nTiled, nF, sMv * k1, sMvMax, nReset, nF, nRcas,
            nF, sRcas * k1, sRcasE * k1, nMv, nF);
        if (nL) Log("pvcap SUMMARY unit %d: the game's draws after our DLAA changed %.2f%% of the target's pixels on average "
                    "(before Present)", ui, sLater / nL);
    }
    t_inDlaa = false;
    Log("pvcap: done (%s) -- files in dlaa_selftest\\, capture resources released", aborted ? "aborted" : "complete");
    delete g_pvCap;
    g_pvCap = nullptr;
    g_pvCapActive = false;
}

// ---- v0.8.1 round 5: preview SEAM capture (the snapshot key on an RGBA16F / HDR preview target) ----------------------
// The open question of the centre line (a faint dark column at the picture's horizontal middle, top to bottom, gone with
// the mode off) in ONE user run: is it already in what DLAA gets (game side: the tile passes, our viewport shift, the
// game's composite), made by NGX (depth / MVs / the jitter it is told) or by RCAS -- and does it need our jitter at all?
// Started like the 8-bit capture (snapshot key / dlaa.ini preview_capture_at while preview units run) when the target is
// RGBA16F, or on any target with dlaa.ini preview_seam_capture = 1. Records kSmA (8) consecutive frames with the normal
// jitter (one full Halton cycle), then kSmB (4) with the preview jitter forced to 0 (viewport shift 0 AND NGX told 0): the
// same numbers without any shift of ours. Per frame, target order 0 only:
//   * every pass's DS discard (its RT is complete), BEFORE the tile edge fix: the tile RT's 32 first / 32 last columns and
//     2 first / 2 last rows (staging), the viewport shift it got and the game's own viewport 0;
//   * right after the target's DLAA Run: a band of 128 columns (x = W/2 - 64 .. W/2 + 63, full height) of the DLAA input
//     (colorIn = the composited picture), the NGX output (before RCAS), our result in the target (after RCAS), the depth
//     and the MVs NGX got; at the frame end the same band of the target again (FINAL: after the game's later draws, UI);
//   * layout state, shift scale, jitter, reset / RCAS / MV flags, DLAA runs and late composites of the target.
// Nothing waits on the GPU while it runs (each copy goes into its own staging texture); at the end ONE blocking readback
// (a one-time hitch), "pvseam:" log lines, BMPs in dlaa_selftest\ (seam<N>_<frame>_bands.bmp = IN | NGX | OUT | FINAL |
// depth | MV, seam<N>_<frame>_tiles.bmp = each pass's first / last 32 tile columns, texels exactly 0 in magenta; N = the
// capture number of the session), released.
// Staging memory while it runs: ~14 MB per frame at 3840x2160 RGBA16F (~170 MB for the 12 frames). Render thread only.
constexpr int  kSmA = 8, kSmB = 4, kSmFrames = kSmA + kSmB;
constexpr UINT kSmHalf     = 64;                 // band = picture columns [W/2 - 64, W/2 + 64)
constexpr UINT kSmTileCols = 32;                 // tile RT edge strips: columns [0, 32) and [Wt - 32, Wt)
constexpr UINT kSmTileRows = 2;                  // ... rows [0, 2) and [Ht - 2, Ht)
constexpr int  kSmSlots    = 4;                  // passes recorded per frame (the frame's first 4)
enum { kSmIn = 0, kSmNgx, kSmOut, kSmFin, kSmDepth, kSmMv, kSmBands };
struct PvSmFrame {
    bool        ran = false;
    uint64_t    present = 0;
    bool        known = false, tiled = false, jitterOff = false, reset = false, rcas = false, mvDone = false;
    float       kx = 1.0f, ky = 1.0f, jx = 0.0f, jy = 0.0f, njx = 0.0f, njy = 0.0f;
    int         ref = -1, maskAtRun = 0, maskEnd = 0, flushes = 0;
    UINT        bx = 0, bw = 0;                  // band origin / width in the picture
    PcPtr<ID3D11Texture2D> band[kSmBands];
    DXGI_FORMAT bandFmt[kSmBands] = {};
    bool        tile[kSmSlots] = {}, shOn[kSmSlots] = {};
    float       shX[kSmSlots] = {}, shY[kSmSlots] = {};
    D3D11_VIEWPORT vp[kSmSlots] = {};
    PcPtr<ID3D11Texture2D> tL[kSmSlots], tR[kSmSlots], tT[kSmSlots], tB[kSmSlots];
    DXGI_FORMAT tileFmt = DXGI_FORMAT_UNKNOWN;
    UINT        tw = 0, th = 0;                  // tile RT size
};
struct PvSeam {
    int      id = 0;                             // capture number this session (file names seam<id>_<frame>_...)
    int      f = 0;                              // frame being recorded (0 .. kSmFrames - 1)
    int      idle = 0;                           // frame ends in a row without a run of target 0 (abort at 60)
    uint64_t start = 0;
    UINT     W = 0, H = 0;                       // picture (target) size at the start
    wchar_t  dir[MAX_PATH] = {};
    PvSmFrame fr[kSmFrames];
};
bool    g_pvSmActive = false;
PvSeam* g_pvSm = nullptr;                        // heap: only while a seam capture runs

// Copies the rect (x, y, w, h) of `src` (clamped to it) into a NEW staging texture `out` of its format.
bool PvSmStage(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, UINT x, UINT y, UINT w, UINT h, PcPtr<ID3D11Texture2D>& out,
               DXGI_FORMAT* fmt) {
    out.Reset();
    if (!src) return false;
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);
    if (sd.SampleDesc.Count != 1 || x >= sd.Width || y >= sd.Height || !w || !h) return false;
    if (w > sd.Width - x) w = sd.Width - x;
    if (h > sd.Height - y) h = sd.Height - y;
    PcPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = sd.Format; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (!dev || FAILED(dev->CreateTexture2D(&td, nullptr, &out)) || !out) { out.Reset(); return false; }
    const D3D11_BOX b{ x, y, 0, x + w, y + h, 1 };
    ctx->CopySubresourceRegion(out.Get(), 0, 0, 0, 0, src, 0, &b);
    if (fmt) *fmt = sd.Format;
    return true;
}

void PvSmFinish(ID3D11DeviceContext* ctx, bool aborted);

void PvSmStart(ID3D11DeviceContext* ctx, uint64_t n) {
    if (!g_pt[0].tex) { Log("pvseam: not started -- no preview target this frame (Present #%llu)", (unsigned long long)n); return; }
    PvSeam* P = new (std::nothrow) PvSeam();
    if (!P) return;
    if (!PathNextToDll(L"dlaa_selftest\\", P->dir)) { delete P; Log("pvseam: not started -- no output path"); return; }
    CreateDirectoryW(P->dir, nullptr);
    D3D11_TEXTURE2D_DESC td{};
    g_pt[0].tex->GetDesc(&td);
    static int captures = 0;
    P->id = ++captures;
    P->W = td.Width; P->H = td.Height; P->start = n;
    g_pvSm = P;
    g_pvSmActive = true;
    (void)ctx;
    Log("pvseam: capture %d started (snapshot key / preview_capture_at on the preview screen, Present #%llu) -- target order 0 %ux%u "
        "fmt=%d; %d frames with the normal jitter, then %d with the preview jitter forced to 0; band x=%u..%u (seam between "
        "x=%u and x=%u), tile edges of the first %d passes before the edge fix; staging ~%.0f MB until the end",
        P->id, (unsigned long long)n, td.Width, td.Height, (int)td.Format, kSmA, kSmB,
        td.Width / 2 > kSmHalf ? td.Width / 2 - kSmHalf : 0u, td.Width / 2 + kSmHalf - 1, td.Width / 2 - 1, td.Width / 2,
        kSmSlots, (double)kSmFrames * (6.0 * 2.0 * kSmHalf * td.Height * 8.0 +
                                       kSmSlots * (2.0 * kSmTileCols * td.Height + 2.0 * kSmTileRows * td.Width) * 4.0) /
                  (1024.0 * 1024.0));
}

// PreviewOnDiscard of pass `slot` (its RT complete, before PvFixTileEdges and the game's composite).
void PvSmOnPass(ID3D11DeviceContext* ctx, int slot) {
    PvSeam& P = *g_pvSm;
    if (P.f >= kSmFrames || slot < 0 || slot >= kSmSlots) return;
    PvSmFrame& F = P.fr[P.f];
    PvSlot& s = g_pv[slot];
    if (!s.rt || F.tile[slot]) return;
    D3D11_TEXTURE2D_DESC d{};
    s.rt->GetDesc(&d);
    if (d.Width < kSmTileCols * 2 || d.Height < kSmTileRows * 2) return;
    t_inDlaa = true;
    const bool ok = PvSmStage(ctx, s.rt.Get(), 0, 0, kSmTileCols, d.Height, F.tL[slot], &F.tileFmt) &&
                    PvSmStage(ctx, s.rt.Get(), d.Width - kSmTileCols, 0, kSmTileCols, d.Height, F.tR[slot], nullptr) &&
                    PvSmStage(ctx, s.rt.Get(), 0, 0, d.Width, kSmTileRows, F.tT[slot], nullptr) &&
                    PvSmStage(ctx, s.rt.Get(), 0, d.Height - kSmTileRows, d.Width, kSmTileRows, F.tB[slot], nullptr);
    t_inDlaa = false;
    F.tile[slot] = ok;
    F.tw = d.Width; F.th = d.Height;
    F.shX[slot] = s.shX; F.shY[slot] = s.shY; F.shOn[slot] = s.shOn; F.vp[slot] = s.capVp;
}

// Right after unit `ui`'s successful Run on target order `idx` (`out` = where our result sits now).
void PvSmAfterRun(ID3D11DeviceContext* ctx, int ui, int idx, PvTarget& tg, int ref, float njx, float njy,
                  ID3D11Texture2D* out, uint64_t fr) {
    PvSeam& P = *g_pvSm;
    if (idx != 0 || P.f >= kSmFrames) return;
    PvSmFrame& F = P.fr[P.f];
    if (F.ran) return;                                   // first run of the frame only (more runs: counted at the frame end)
    SceneDlaa& dl = g_pu[ui].dl;
    D3D11_TEXTURE2D_DESC td{};
    tg.tex->GetDesc(&td);
    F.bw = td.Width < 2 * kSmHalf ? td.Width : 2 * kSmHalf;
    F.bx = td.Width / 2 > kSmHalf ? td.Width / 2 - kSmHalf : 0u;
    t_inDlaa = true;
    ID3D11Texture2D* src[kSmBands] = { dl.ColorInTex(), dl.OutTex(), out, nullptr, dl.DepthTex(), dl.MvTex() };
    for (int b = 0; b < kSmBands; ++b)
        if (src[b]) PvSmStage(ctx, src[b], F.bx, 0, F.bw, td.Height, F.band[b], &F.bandFmt[b]);
    t_inDlaa = false;
    F.ran = true; F.present = fr; F.ref = ref;
    const PvLayout& lay = g_pvLay[idx];
    F.known = lay.known; F.tiled = lay.tiled;
    PvShiftScale(ref, &F.kx, &F.ky);
    F.jx = g_pvJx; F.jy = g_pvJy; F.njx = njx; F.njy = njy;
    F.reset = dl.LastReset(); F.rcas = dl.LastSharpened(); F.mvDone = dl.LastMvDone();
    F.jitterOff = P.f >= kSmA;
    F.maskAtRun = tg.slotMask;
}

// Frame end (PreviewNextFrame, after the flushes, before the targets reset): the FINAL band, then the next frame.
void PvSmAtFrameEnd(ID3D11DeviceContext* ctx, uint64_t n) {
    PvSeam& P = *g_pvSm;
    if (P.f >= kSmFrames) { PvSmFinish(ctx, false); return; }
    PvSmFrame& F = P.fr[P.f];
    if (!F.ran) {                                        // no run of target 0 this frame: its pass strips are dropped
        F = PvSmFrame();
        if (++P.idle >= 60) PvSmFinish(ctx, true);
        return;
    }
    P.idle = 0;
    F.maskEnd = g_pt[0].slotMask; F.flushes = g_pt[0].flushes;
    if (g_pt[0].tex) {
        D3D11_TEXTURE2D_DESC td{};
        g_pt[0].tex->GetDesc(&td);
        t_inDlaa = true;
        PvSmStage(ctx, g_pt[0].tex.Get(), F.bx, 0, F.bw, td.Height, F.band[kSmFin], &F.bandFmt[kSmFin]);
        t_inDlaa = false;
    }
    if (++P.f == kSmA)
        Log("pvseam: %d frames with the normal jitter recorded (Present #%llu) -- the next %d frames run with the preview "
            "jitter forced to 0 (viewport shift 0, NGX told 0)", kSmA, (unsigned long long)n, kSmB);
    if (P.f >= kSmFrames) PvSmFinish(ctx, false);
}

// ---- readback side (PvSmFinish) ----
float PvSmF11(uint32_t v, int manBits) {             // unsigned R11G11B10 component: 5-bit exponent, 6 / 5-bit mantissa
    const uint32_t e = (v >> manBits) & 31u, m = v & ((1u << manBits) - 1u);
    const float mf = (float)m / (float)(1u << manBits);
    if (e == 0) return std::ldexp(mf, -14);
    if (e == 31) return 0.0f;                            // inf / NaN: not a colour here
    return std::ldexp(1.0f + mf, (int)e - 15);
}
// Texel x of a mapped row -> rgb (linear float formats or 0..1 for UNORM). *flt = float format (tone-compress for viewing);
// *zero = all three channels exactly 0 (the clear colour). false = format not handled.
bool PvSmTexel(DXGI_FORMAT f, const uint8_t* row, UINT x, float rgb[3], bool* flt, bool* zero) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: {
        const uint16_t* p = (const uint16_t*)row + (size_t)x * 4;
        for (int c = 0; c < 3; ++c) rgb[c] = HalfToFloat(p[c]);
        *flt = true; *zero = !(p[0] & 0x7FFF) && !(p[1] & 0x7FFF) && !(p[2] & 0x7FFF);
        return true;
    }
    case DXGI_FORMAT_R11G11B10_FLOAT: {
        const uint32_t v = ((const uint32_t*)row)[x];
        rgb[0] = PvSmF11(v & 0x7FFu, 6); rgb[1] = PvSmF11((v >> 11) & 0x7FFu, 6); rgb[2] = PvSmF11(v >> 22, 5);
        *flt = true; *zero = v == 0;
        return true;
    }
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
        const uint8_t* p = row + (size_t)x * 4;
        const bool bgr = f == DXGI_FORMAT_B8G8R8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_UNORM ||
                         f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        rgb[0] = p[bgr ? 2 : 0] / 255.0f; rgb[1] = p[1] / 255.0f; rgb[2] = p[bgr ? 0 : 2] / 255.0f;
        *flt = false; *zero = !p[0] && !p[1] && !p[2];
        return true;
    }
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
        const uint32_t v = ((const uint32_t*)row)[x];
        rgb[0] = (v & 1023u) / 1023.0f; rgb[1] = ((v >> 10) & 1023u) / 1023.0f; rgb[2] = ((v >> 20) & 1023u) / 1023.0f;
        *flt = false; *zero = (v & 0x3FFFFFFFu) == 0;
        return true;
    }
    case DXGI_FORMAT_R32_FLOAT: {
        rgb[0] = ((const float*)row)[x]; rgb[1] = rgb[2] = 0.0f;
        *flt = true; *zero = rgb[0] == 0.0f;
        return true;
    }
    case DXGI_FORMAT_R16G16_FLOAT: {
        const uint16_t* p = (const uint16_t*)row + (size_t)x * 2;
        rgb[0] = HalfToFloat(p[0]); rgb[1] = HalfToFloat(p[1]); rgb[2] = 0.0f;
        *flt = true; *zero = false;
        return true;
    }
    default:
        return false;
    }
}
// Display luma: linear float colour tone-compressed L / (1 + L) (HDR highlights do not swamp the means), UNORM as stored.
inline float PvSmLuma(const float rgb[3], bool flt) {
    float l = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
    if (!(l > 0.0f)) return 0.0f;                        // negative / NaN -> 0
    return flt ? l / (1.0f + l) : l;
}
uint8_t PvSmView(float v, bool flt) {                    // one channel -> 8-bit for the BMPs (float: x / (1 + x), sRGB)
    static uint8_t lut[4096];                            // sRGB encode of 0..1 (the WriteBmpRgba16f mapping), built once
    static bool lutReady = false;
    if (!lutReady) {
        for (int i = 0; i < 4096; ++i) {
            const double y = (double)i / 4095.0;
            const double s = y <= 0.0031308 ? 12.92 * y : 1.055 * std::pow(y, 1.0 / 2.4) - 0.055;
            lut[i] = (uint8_t)(s <= 0.0 ? 0 : (s >= 1.0 ? 255 : (int)(s * 255.0 + 0.5)));
        }
        lutReady = true;
    }
    if (!(v > 0.0f)) return 0;
    if (flt) return lut[(int)(v / (1.0f + v) * 4095.0f + 0.5f) & 4095];
    return (uint8_t)(v >= 1.0f ? 255 : (int)(v * 255.0f + 0.5f));
}

// One colour band, mapped: luma plane (bw x h), column means and the seam numbers.
struct PvSmBandStats {
    bool  ok = false, flt = false;
    std::vector<float> lum;                          // bw x h display luma
    std::vector<float> col;                          // column means
    float dip = 0.0f;                                // worst local dip at the seam columns (fraction, < 0 = darker)
    int   dipX = -1;                                 // its band column
    float noise = 0.0f;                              // RMS of the same local dip over the columns away from the seam
    float darkRows = 0.0f, darkRowsTyp = 0.0f;       // fraction of rows darker than both neighbours by > 3 % at dipX / typical
    bool  sig = false;                               // dip < -max(1 %, 3 x noise)
};
void PvSmBand(const D3D11_MAPPED_SUBRESOURCE& m, DXGI_FORMAT f, UINT bw, UINT h, UINT seam, PvSmBandStats& S) {
    S.lum.assign((size_t)bw * h, 0.0f);
    S.col.assign(bw, 0.0f);
    float rgb[3]; bool flt = false, zero = false;
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
        for (UINT x = 0; x < bw; ++x) {
            if (!PvSmTexel(f, row, x, rgb, &flt, &zero)) return;
            const float l = PvSmLuma(rgb, flt);
            S.lum[(size_t)y * bw + x] = l;
            S.col[x] += l;
        }
    }
    for (UINT x = 0; x < bw; ++x) S.col[x] /= (float)(h ? h : 1);
    S.ok = true; S.flt = flt;
    const int d = 3;                                     // neighbours 3 columns either side
    auto local = [&](int x) {
        const float nb = 0.5f * (S.col[x - d] + S.col[x + d]);
        return nb > 1e-6f ? S.col[x] / nb - 1.0f : 0.0f;
    };
    auto darkFrac = [&](int x) {
        int rows = 0, dark = 0;
        for (UINT y = 0; y < h; ++y) {
            const float* r = &S.lum[(size_t)y * bw];
            const float nb = 0.5f * (r[x - d] + r[x + d]);
            if (nb < 0.02f) continue;                    // too dark to tell
            ++rows;
            if (r[x] < 0.97f * nb) ++dark;
        }
        return rows ? (float)dark / (float)rows : 0.0f;
    };
    const int s = (int)seam;
    S.dip = 1e9f;
    for (int x = s - 3; x <= s + 2; ++x) {
        if (x - d < 0 || x + d >= (int)bw) continue;
        const float v = local(x);
        if (v < S.dip) { S.dip = v; S.dipX = x; }
    }
    if (S.dipX < 0) { S.dip = 0.0f; return; }
    double sq = 0.0, dsum = 0.0;
    int nq = 0, nd = 0;
    for (int x = d + 3; x < (int)bw - d - 3; ++x) {
        if (x > s - 12 && x < s + 12) continue;
        const float v = local(x);
        sq += (double)v * v; ++nq;
        if ((x & 3) == 0) { dsum += darkFrac(x); ++nd; }
    }
    S.noise = nq ? (float)std::sqrt(sq / nq) : 0.0f;
    S.darkRows = darkFrac(S.dipX);
    S.darkRowsTyp = nd ? (float)(dsum / nd) : 0.0f;
    S.sig = S.dip < -(std::max)(0.01f, 3.0f * S.noise);
}

void PvSmFinish(ID3D11DeviceContext* ctx, bool aborted) {
    PvSeam& P = *g_pvSm;
    t_inDlaa = true;
    static const char* kBandName[kSmBands] = { "IN", "NGX", "OUT", "FINAL", "depth", "mv" };
    struct Acc { int n = 0, sig = 0; double dip = 0.0; };
    Acc acc[2][4];                                       // [phase A / B][IN NGX OUT FINAL]
    float worstZero = 0.0f; int wzF = -1, wzSlot = -1; const char* wzEdge = ""; float wzShift = 0.0f;
    float worstZeroSmall = 0.0f;                         // ... on an edge whose shift was <= 0.49 px (the open case)
    int frames = 0, bmps = 0;
    for (int m = 0; m < kSmFrames; ++m) {
        PvSmFrame& F = P.fr[m];
        if (!F.ran) continue;
        ++frames;
        const UINT h = P.H, bw = F.bw, seam = P.W / 2 - F.bx;   // band column of picture x = W/2 (the seam's right side)
        // --- colour bands ---
        PvSmBandStats S[4];
        D3D11_MAPPED_SUBRESOURCE mp[kSmBands] = {};
        bool mapped[kSmBands] = {};
        for (int b = 0; b < kSmBands; ++b)
            if (F.band[b]) mapped[b] = SUCCEEDED(ctx->Map(F.band[b].Get(), 0, D3D11_MAP_READ, 0, &mp[b]));
        for (int b = 0; b < 4; ++b)
            if (mapped[b]) PvSmBand(mp[b], F.bandFmt[b], bw, h, seam, S[b]);
        // BMP: IN | NGX | OUT | FINAL | depth | mv, 4 px grey gaps
        {
            const UINT gap = 4, W6 = bw * kSmBands + gap * (kSmBands - 1);
            std::vector<uint8_t> img((size_t)W6 * h * 4, 96);
            float dLo = 1e30f, dHi = -1e30f;
            if (mapped[kSmDepth])
                for (UINT y = 0; y < h; ++y)
                    for (UINT x = 0; x < bw; ++x) {
                        float rgb[3]; bool flt, zero;
                        if (!PvSmTexel(F.bandFmt[kSmDepth], (const uint8_t*)mp[kSmDepth].pData + (size_t)y * mp[kSmDepth].RowPitch,
                                       x, rgb, &flt, &zero)) break;
                        if (rgb[0] > 0.0f) { dLo = (std::min)(dLo, rgb[0]); dHi = (std::max)(dHi, rgb[0]); }
                    }
            for (int b = 0; b < kSmBands; ++b) {
                if (!mapped[b]) continue;
                for (UINT y = 0; y < h; ++y) {
                    const uint8_t* row = (const uint8_t*)mp[b].pData + (size_t)y * mp[b].RowPitch;
                    uint8_t* o = &img[((size_t)y * W6 + (size_t)b * (bw + gap)) * 4];
                    for (UINT x = 0; x < bw; ++x, o += 4) {
                        float rgb[3]; bool flt = false, zero = false;
                        if (!PvSmTexel(F.bandFmt[b], row, x, rgb, &flt, &zero)) break;
                        if (b == kSmDepth) {             // far (0) = dark blue, else grey over the band's depth range
                            if (rgb[0] <= 0.0f) { o[0] = 0; o[1] = 0; o[2] = 64; }
                            else { const uint8_t g = (uint8_t)(40.0f + 215.0f * (rgb[0] - dLo) / (dHi > dLo ? dHi - dLo : 1.0f)); o[0] = o[1] = o[2] = g; }
                        } else if (b == kSmMv) {         // R = 128 + 32 * mv.x, G = 128 + 32 * mv.y (px)
                            auto q8 = [](float v) { v = 128.0f + 32.0f * v; return (uint8_t)(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v)); };
                            o[0] = q8(rgb[0]); o[1] = q8(rgb[1]); o[2] = 0;
                        } else {
                            for (int c = 0; c < 3; ++c) o[c] = PvSmView(rgb[c], flt);
                        }
                        o[3] = 255;
                    }
                }
            }
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%sseam%d_%d_bands.bmp", P.dir, P.id, m);
            if (WriteBmp(path, img.data(), W6 * 4, W6, h)) ++bmps;
        }
        for (int b = 0; b < kSmBands; ++b) if (mapped[b]) ctx->Unmap(F.band[b].Get(), 0);
        // --- tile edges (before the edge fix) ---
        char tbuf[1600]; size_t tl = 0; tbuf[0] = 0;
        {
            const UINT sw = kSmTileCols * 2 + 2, gap = 8;
            const UINT TW = sw * kSmSlots + gap * (kSmSlots - 1), th = F.th;
            std::vector<uint8_t> img((size_t)TW * (th ? th : 1) * 4, 96);
            for (int s = 0; s < kSmSlots; ++s) {
                if (!F.tile[s]) continue;
                D3D11_MAPPED_SUBRESOURCE ml{}, mr{}, mt{}, mb{};
                const bool okL = SUCCEEDED(ctx->Map(F.tL[s].Get(), 0, D3D11_MAP_READ, 0, &ml));
                const bool okR = SUCCEEDED(ctx->Map(F.tR[s].Get(), 0, D3D11_MAP_READ, 0, &mr));
                const bool okT = SUCCEEDED(ctx->Map(F.tT[s].Get(), 0, D3D11_MAP_READ, 0, &mt));
                const bool okB = SUCCEEDED(ctx->Map(F.tB[s].Get(), 0, D3D11_MAP_READ, 0, &mb));
                // zero fraction + mean luma of strip column `c` (0..31) over th rows
                auto colStat = [&](const D3D11_MAPPED_SUBRESOURCE& mm, UINT c, float* z, float* l) {
                    UINT nz = 0; double sl = 0.0;
                    for (UINT y = 0; y < th; ++y) {
                        float rgb[3]; bool flt = false, zero = false;
                        if (!PvSmTexel(F.tileFmt, (const uint8_t*)mm.pData + (size_t)y * mm.RowPitch, c, rgb, &flt, &zero)) break;
                        nz += zero ? 1u : 0u; sl += PvSmLuma(rgb, flt);
                    }
                    *z = th ? 100.0f * nz / th : 0.0f; *l = th ? (float)(sl / th) : 0.0f;
                };
                auto rowZero = [&](const D3D11_MAPPED_SUBRESOURCE& mm, UINT r) {
                    UINT nz = 0;
                    const uint8_t* row = (const uint8_t*)mm.pData + (size_t)r * mm.RowPitch;
                    for (UINT x = 0; x < F.tw; ++x) {
                        float rgb[3]; bool flt = false, zero = false;
                        if (!PvSmTexel(F.tileFmt, row, x, rgb, &flt, &zero)) break;
                        nz += zero ? 1u : 0u;
                    }
                    return F.tw ? 100.0f * nz / F.tw : 0.0f;
                };
                float z0 = 0, l0 = 0, z1 = 0, l1 = 0, zN2 = 0, lN2 = 0, zN1 = 0, lN1 = 0;
                if (okL) { colStat(ml, 0, &z0, &l0); colStat(ml, 1, &z1, &l1); }
                if (okR) { colStat(mr, kSmTileCols - 2, &zN2, &lN2); colStat(mr, kSmTileCols - 1, &zN1, &lN1); }
                const float zT = okT ? rowZero(mt, 0) : 0.0f, zB = okB ? rowZero(mb, kSmTileRows - 1) : 0.0f;
                struct { float z; const char* e; float sh; } edges[4] = {
                    { z0, "first column", F.shX[s] }, { zN1, "last column", F.shX[s] }, { zT, "first row", F.shY[s] },
                    { zB, "last row", F.shY[s] } };
                for (auto& e : edges) {
                    if (e.z > worstZero) { worstZero = e.z; wzF = m; wzSlot = s; wzEdge = e.e; wzShift = e.sh; }
                    if (std::fabs(e.sh) <= 0.49f && e.z > worstZeroSmall) worstZeroSmall = e.z;
                }
                const D3D11_VIEWPORT& v = F.vp[s];
                const int w = snprintf(tbuf + tl, sizeof(tbuf) - tl, " | slot %d vp=(%.2f,%.2f %.0fx%.0f) shift=(%+.4f,%+.4f)%s: "
                                       "col0 %.1f%% zero L=%.4f, col1 %.1f%% L=%.4f, col%u %.1f%% L=%.4f, col%u %.1f%% zero L=%.4f, "
                                       "row0 %.1f%% zero, row%u %.1f%% zero", s, (double)v.TopLeftX, (double)v.TopLeftY,
                                       (double)v.Width, (double)v.Height, (double)F.shX[s], (double)F.shY[s],
                                       F.shOn[s] ? "" : " (not shifted)", (double)z0, (double)l0, (double)z1, (double)l1,
                                       F.tw - 2, (double)zN2, (double)lN2, F.tw - 1, (double)zN1, (double)lN1, (double)zT,
                                       F.th - 1, (double)zB);
                if (w > 0 && tl + (size_t)w < sizeof(tbuf)) tl += (size_t)w;
                // BMP strip: first 32 columns | 2 px gap | last 32 columns; texels exactly 0 = magenta
                auto paint = [&](const D3D11_MAPPED_SUBRESOURCE& mm, UINT xo) {
                    for (UINT y = 0; y < th; ++y) {
                        const uint8_t* row = (const uint8_t*)mm.pData + (size_t)y * mm.RowPitch;
                        uint8_t* o = &img[((size_t)y * TW + (size_t)s * (sw + gap) + xo) * 4];
                        for (UINT x = 0; x < kSmTileCols; ++x, o += 4) {
                            float rgb[3]; bool flt = false, zero = false;
                            if (!PvSmTexel(F.tileFmt, row, x, rgb, &flt, &zero)) break;
                            if (zero) { o[0] = 255; o[1] = 0; o[2] = 255; }
                            else for (int c = 0; c < 3; ++c) o[c] = PvSmView(rgb[c], flt);
                            o[3] = 255;
                        }
                    }
                };
                if (okL) paint(ml, 0);
                if (okR) paint(mr, kSmTileCols + 2);
                if (okL) ctx->Unmap(F.tL[s].Get(), 0);
                if (okR) ctx->Unmap(F.tR[s].Get(), 0);
                if (okT) ctx->Unmap(F.tT[s].Get(), 0);
                if (okB) ctx->Unmap(F.tB[s].Get(), 0);
            }
            if (th) {
                wchar_t path[MAX_PATH];
                swprintf_s(path, L"%sseam%d_%d_tiles.bmp", P.dir, P.id, m);
                if (WriteBmp(path, img.data(), TW * 4, TW, th)) ++bmps;
            }
        }
        // --- verdict of this frame ---
        const char* where = "no dark column at the seam in this frame";
        if (S[0].sig)       where = "the dark column is ALREADY IN THE DLAA INPUT (game side: tile passes / our viewport shift / composite)";
        else if (S[1].sig)  where = "NOT in the input -- CREATED BY NGX (depth / MV / jitter side)";
        else if (S[2].sig)  where = "not in the input nor the NGX output -- created by RCAS (preview_sharpen=0 removes it)";
        else if (S[3].sig)  where = "only after our DLAA -- created by the game's later draws (UI / encode)";
        auto bandTxt = [&](const PvSmBandStats& b, char* o, size_t on) {
            if (!b.ok) { snprintf(o, on, "n/a"); return; }
            snprintf(o, on, "%+.2f%% at x=%u%s (dark rows %.0f%% vs %.0f%% typical, noise %.2f%%)", 100.0 * b.dip,
                     F.bx + (UINT)(b.dipX < 0 ? 0 : b.dipX), b.sig ? " SIGNIFICANT" : "", 100.0 * b.darkRows,
                     100.0 * b.darkRowsTyp, 100.0 * b.noise);
        };
        char bt[4][160];
        for (int b = 0; b < 4; ++b) bandTxt(S[b], bt[b], sizeof(bt[b]));
        Log("pvseam: frame %d%s (game frame #%llu) layout=%s ref slot %d, shift scale (%.2f,%.2f) | jitter (%+.4f,%+.4f) picture px, "
            "NGX told (%+.4f,%+.4f) | reset=%d rcas=%d mv=%d | DLAA runs %d, composited before the run 0x%x, by the frame end "
            "0x%x | seam dip (worst of x=W/2-3..W/2+2 vs the columns 3 px either side): IN %s | NGX %s | OUT %s | FINAL %s -> %s",
            m, F.jitterOff ? " [jitter forced 0]" : "", (unsigned long long)F.present,
            !F.known ? "unknown" : (F.tiled ? "TILED" : "untiled/reference-slot"), F.ref, (double)F.kx, (double)F.ky,
            (double)F.jx, (double)F.jy, (double)F.njx, (double)F.njy, (int)F.reset, (int)F.rcas, (int)F.mvDone, F.flushes,
            F.maskAtRun, F.maskEnd, bt[0], bt[1], bt[2], bt[3], where);
        {   // the 8 columns around the seam, per stage
            char cb[900]; size_t cl = 0; cb[0] = 0;
            for (int b = 0; b < 4; ++b) {
                if (!S[b].ok) continue;
                int w = snprintf(cb + cl, sizeof(cb) - cl, "%s%s:", b ? " | " : "", kBandName[b]);
                if (w > 0) cl += (size_t)w;
                for (int x = (int)seam - 4; x < (int)seam + 4 && cl + 16 < sizeof(cb); ++x) {
                    w = snprintf(cb + cl, sizeof(cb) - cl, " %.4f", (x >= 0 && x < (int)bw) ? (double)S[b].col[x] : 0.0);
                    if (w > 0) cl += (size_t)w;
                }
            }
            Log("pvseam: frame %d column mean luma x=%u..%u (seam between %u and %u; HDR values as L/(1+L)): %s", m,
                F.bx + seam - 4, F.bx + seam + 3, F.bx + seam - 1, F.bx + seam, cb);
        }
        Log("pvseam: frame %d tile passes (%ux%u fmt=%d; edge texels EXACTLY 0 = never rasterized, measured before the edge "
            "fix)%s", m, F.tw, F.th, (int)F.tileFmt, tl ? tbuf : " | none recorded");
        const int ph = F.jitterOff ? 1 : 0;
        for (int b = 0; b < 4; ++b)
            if (S[b].ok) { ++acc[ph][b].n; acc[ph][b].sig += S[b].sig ? 1 : 0; acc[ph][b].dip += S[b].dip; }
    }
    // --- summary + verdict ---
    char ph[2][400];
    for (int p = 0; p < 2; ++p) {
        size_t l = 0; ph[p][0] = 0;
        for (int b = 0; b < 4; ++b) {
            const int w = snprintf(ph[p] + l, sizeof(ph[p]) - l, "%s%s significant %d/%d mean dip %+.2f%%", b ? ", " : "",
                                   kBandName[b], acc[p][b].sig, acc[p][b].n, acc[p][b].n ? 100.0 * acc[p][b].dip / acc[p][b].n : 0.0);
            if (w > 0) l += (size_t)w;
        }
    }
    Log("pvseam SUMMARY (capture %d): %d frame(s) recorded%s, %d BMPs | jitter on (%d frames): %s | jitter forced 0: %s | "
        "tile edge texels exactly 0 before the edge fix: worst %.1f%% (frame %d slot %d %s, shift %+.4f tile px), worst on an "
        "edge shifted <= 0.49 px: %.1f%%", P.id, frames, aborted ? " (ABORTED: target 0 stopped running)" : "", bmps, acc[0][0].n, ph[0], ph[1],
        (double)worstZero, wzF, wzSlot, wzEdge, (double)wzShift, (double)worstZeroSmall);
    const char* verdict;
    auto half = [&](int p, int b) { return acc[p][b].n > 0 && acc[p][b].sig * 2 >= acc[p][b].n; };
    if (half(0, 0)) {
        verdict = half(1, 0) ? "the dark column is IN THE DLAA INPUT, also with our jitter forced to 0 -> the game's own picture "
                               "(tile passes / composite) has it; NGX / RCAS at most amplify it (compare the IN / NGX / OUT dips)"
                             : "the dark column is IN THE DLAA INPUT only while we jitter -> our viewport shift creates it at the "
                               "tile edges (see the tile lines: edge texels left at 0, and which pass / shift)";
    } else if (half(0, 1)) {
        verdict = half(1, 1) ? "the input is clean; NGX creates the dark column, also without jitter -> depth / MV side"
                             : "the input is clean; NGX creates the dark column only while jittered -> the jitter NGX is told vs "
                               "what the picture got (shift scale / layout) is the suspect";
    } else if (half(0, 2)) {
        verdict = "input and NGX output are clean; RCAS (the sharpen after NGX) creates the dark column -> dlaa.ini preview_sharpen=0";
    } else if (half(0, 3)) {
        verdict = "our result is clean; the dark column appears only after our DLAA (the game's later draws)";
    } else {
        verdict = "no dark seam column measured in this capture -- capture again while the line is visible on screen";
    }
    auto meanDip = [&](int b) { return acc[0][b].n ? 100.0 * acc[0][b].dip / acc[0][b].n : 0.0; };
    Log("pvseam VERDICT (capture %d): %s | mean seam dip with jitter: IN %+.2f%% -> NGX %+.2f%% -> OUT %+.2f%% -> FINAL "
        "%+.2f%%%s", P.id, verdict, meanDip(0), meanDip(1), meanDip(2), meanDip(3), worstZeroSmall > 1.0f ? " | NOTE: tile "
        "edges shifted by <= 0.49 px were left at the clear colour (the GPU drops the fractional edge column) -- the "
        "zero-guarded edge fill covers that case" : "");
    t_inDlaa = false;
    Log("pvseam: capture %d done (%s) -- BMPs in dlaa_selftest\\ (seam%d_<frame>_bands.bmp: IN | NGX | OUT | FINAL | depth | "
        "mv, %u px each, the seam at x=%u inside every band; seam%d_<frame>_tiles.bmp: per pass its first | last %u tile "
        "columns, magenta = texel exactly 0), staging released", P.id, aborted ? "aborted" : "complete", P.id, 2 * kSmHalf,
        kSmHalf, P.id, kSmTileCols);
    delete g_pvSm;
    g_pvSm = nullptr;
    g_pvSmActive = false;
}

// BGRA target: UNORM (non-sRGB, DXGI_FORMAT_B8G8R8A8_UNORM) SRV + RTV on the target and an R8G8B8A8_TYPELESS scratch of
// its size with UNORM views (rebuilt when the target texture / size changes). false = not possible (logged once).
// Round 5: the views / scratch belong to the unit `t` (flat: unit 0), `tex` is this frame's target.
bool PvPrepareBgra(ID3D11Device* dev, PvUnit& t, ID3D11Texture2D* tex, int idx, const D3D11_TEXTURE2D_DESC& td) {
    if (t.viewsOf.Get() != tex) {
        t.tgtSrv.Reset(); t.tgtRtv.Reset(); t.viewsOf = tex;
        if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE) || !(td.BindFlags & D3D11_BIND_RENDER_TARGET)) {
            Log("preview target %d: BGRA target %ux%u lacks SHADER_RESOURCE / RENDER_TARGET bind (bind flags 0x%x) -- no DLAA there",
                idx, td.Width, td.Height, (unsigned)td.BindFlags);
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        HRESULT hr;
        if (FAILED(hr = dev->CreateShaderResourceView(tex, &sd, &t.tgtSrv)) ||
            FAILED(hr = dev->CreateRenderTargetView(tex, &rd, &t.tgtRtv))) {
            Log("preview target %d: UNORM views of the BGRA target failed hr=0x%lx (target fmt %d, bind 0x%x) -- no DLAA there",
                idx, (unsigned long)hr, (int)td.Format, (unsigned)td.BindFlags);
            t.tgtSrv.Reset(); t.tgtRtv.Reset();
            return false;
        }
    }
    if (!t.tgtSrv || !t.tgtRtv) return false;
    if (!t.scratch || t.sw != td.Width || t.sh != td.Height) {
        t.scratch.Reset(); t.scratchSrv.Reset(); t.scratchRtv.Reset();
        D3D11_TEXTURE2D_DESC sdsc{};
        sdsc.Width = td.Width; sdsc.Height = td.Height;
        sdsc.MipLevels = 1; sdsc.ArraySize = 1;
        sdsc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
        sdsc.SampleDesc.Count = 1;
        sdsc.Usage = D3D11_USAGE_DEFAULT;
        sdsc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        HRESULT hr;
        if (FAILED(hr = dev->CreateTexture2D(&sdsc, nullptr, &t.scratch)) ||
            FAILED(hr = dev->CreateShaderResourceView(t.scratch.Get(), &sd, &t.scratchSrv)) ||
            FAILED(hr = dev->CreateRenderTargetView(t.scratch.Get(), &rd, &t.scratchRtv))) {
            Log("preview target %d: RGBA8 scratch %ux%u create hr=0x%lx -- no DLAA there", idx, td.Width, td.Height, (unsigned long)hr);
            t.scratch.Reset(); t.scratchSrv.Reset(); t.scratchRtv.Reset();
            return false;
        }
        t.sw = td.Width; t.sh = td.Height;
    }
    return true;
}

// Runs target `idx`'s DLAA now (it is pending): see the block comment. Always clears `pending`.
// Round 5: the work is done by the target's EYE unit (PvUnitOf); a target whose eye is not known yet (or that collides
// with the frame's other target) is left untouched this frame.
void PreviewFlush(ID3D11DeviceContext* ctx, int idx) {
    PvCpuScope cpuScope;                                 // v0.8.1 round 3 (per-frame CPU of our preview work)
    PvTarget& tg = g_pt[idx];
    if (!tg.pending) return;
    tg.pending = false;
    if (tg.flushes++ == 0) tg.maskAtFlush = tg.slotMask; // v0.8.1 round 5 (late-composite check, seam capture)
    g_ptPendingMask &= ~(1 << idx);
    if (g_ptBoundIdx == idx) g_ptBoundIdx = -1;
    if (!tg.tex) return;
    if (!g_dlaaOn.load(std::memory_order_relaxed) || g_passive.load(std::memory_order_relaxed)) return;
    if (!SceneDlaa::ShadersReady()) return;              // v0.7.8 warm-up still running: untouched, nothing built yet
    const int ref = tg.curRef;
    if (ref < 0 || ref >= kPvSlots) return;
    PvSlot& rs = g_pv[ref];
    const uint64_t fr = g_frames.load(std::memory_order_relaxed);
    const int ui = PvUnitOf(idx, fr);
    if (ui < 0) return;                                  // VR eye of this RT not known yet / map conflict: untouched
    PvUnit& t = g_pu[ui];
    if (t.unsupported) return;
    if (g_jitterOnly.load(std::memory_order_relaxed)) {  // jitter-only debug: no evaluate, keep the MV history rolling
        t_inDlaa = true;
        if (t.dl.Mv().Ready() && rs.ps.cand.Ready()) t.dl.Mv().Commit(ctx, rs.ps.cand);
        t_inDlaa = false;
        return;
    }
    {   // Round 6 diagnostics: the reference pass collected no MV candidate (VR logs: the first ~14 s on the menu)
        const CandidateRecord& rc = rs.ps.cand;
        if (!rc.Ready() || rc.Count(0) + rc.Count(1) == 0) {
            if (++g_pvNoCandFlushes == 120 && !g_pvNoCandLogged) {
                g_pvNoCandLogged = true;
                Log("preview MV: the reference pass (slot %d) collected no candidate in 120 flushes -- this pass: DrawIndexed=%u, "
                    "collector skips: sampled=%u no-record/ctx/viewport=%u full=%u no-cb=%u cb-range=%u, record ready=%d "
                    "world=%d cabin=%d | dlaa=%d passive=%d mv=%d", ref, rs.diCount, rs.ps.skSampled, rs.ps.skNoCtx,
                    rs.ps.skFull, rs.ps.skNoCb, rs.ps.skRange, (int)rc.Ready(), rc.Ready() ? rc.Count(0) : 0,
                    rc.Ready() ? rc.Count(1) : 0, (int)g_dlaaOn.load(), (int)g_passive.load(), (int)g_mvOn.load());
            }
        } else if (g_pvNoCandLogged && !g_pvCandBackLogged) {
            g_pvCandBackLogged = true;
            Log("preview MV: first candidates after %llu candidate-less flushes (slot %d: world=%d cabin=%d, DrawIndexed=%u)",
                (unsigned long long)g_pvNoCandFlushes, ref, rc.Count(0), rc.Count(1), rs.diCount);
        }
    }
    // Round 6: the depth NGX gets. A TILED group (PvLayout): the picture depth assembled from every tile pass of this frame;
    // otherwise (untiled, or the layout not known yet) the reference pass's own pre-discard snapshot, as in round 5.
    const PvLayout& lay = g_pvLay[idx];
    const bool tiled = lay.tiled;
    DepthTwin* depth = nullptr;
    if (tiled) {
        PvAsm& A = g_pvAsm[idx];
        if (lay.slotMask != tg.slotMask || lay.ref != ref || !A.twin.valid || A.frameMask != lay.slotMask) {
            static int missLogs = 0;
            if (missLogs++ < 10)
                Log("preview target %d: tiled picture depth incomplete (tiles 0x%x of 0x%x, this frame's passes 0x%x, ref %d vs %d) "
                    "-- frame left untouched (asm-miss #%llu)", idx, A.frameMask, lay.slotMask, tg.slotMask, ref, lay.ref,
                    (unsigned long long)(g_pvAsmMiss + 1));
            ++g_pvAsmMiss;
            return;
        }
        depth = &A.twin;
    } else {
        if (!rs.ps.snap || !rs.ps.twin.valid) {          // no pre-discard depth of the reference pass
            static int warned = 0;
            if (g_ptRefMask != 0 && warned++ < 5)        // (the first frame of a screen has none by design: nothing learned yet)
                Log("preview target %d: reference slot %d has no depth snapshot at the flush -- frame left untouched", idx, ref);
            return;
        }
        depth = &rs.ps.twin;
    }
    D3D11_TEXTURE2D_DESC td{};
    tg.tex->GetDesc(&td);
    if (tiled && (td.Width != depth->w || td.Height != depth->h)) { ++g_pvAsmMiss; return; }
    const bool rgba = PvIsRgba8(td.Format), bgra = PvIsBgra8(td.Format);
    // v0.8.0: RGBA16F = the HDR output target (Windows HDR on): SceneDlaa runs in place as an HDR unit (like RGBA8).
    const bool f16 = SceneDlaa::IsHdrFormat(td.Format);
    if ((!rgba && !bgra && !f16) || td.SampleDesc.Count != 1) {
        t.unsupported = true;
        Log("preview target %d: %ux%u fmt=%d samples=%u is not an 8-bit RGBA / BGRA or RGBA16F target -- no DLAA there", idx,
            td.Width, td.Height, (int)td.Format, td.SampleDesc.Count);
        return;
    }
    if (f16 != g_pvHdrTarget) {
        g_pvHdrTarget = f16;
        static int hdrLogs = 0;
        if (hdrLogs++ < 4)
            Log("preview target %d: %ux%u fmt=%d -- %s", idx, td.Width, td.Height, (int)td.Format,
                f16 ? "RGBA16F (HDR output): preview DLAA runs in place as an HDR unit (RGBA16F colour, NGX IsHDR unless "
                      "dlss_hdr=0); the snapshot key runs the seam capture (pvseam) on it, the 8-bit measuring capture "
                      "does not start here"
                    : "8-bit again: preview DLAA on the LDR path");
    }
    Microsoft::WRL::ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return;
    if (bgra && (!PreviewBlit::Ensure(dev.Get()) || !PvPrepareBgra(dev.Get(), t, tg.tex.Get(), idx, td))) { t.unsupported = true; return; }
    if (!t.inited) {
        t.inited = true;
        t.dl.SetEye(10 + ui);                            // log tag "eye 10 / 11" = preview unit 0 / 1 (round 5: = eye 0 / 1)
        t.dl.Mv().SetNearReject(0.0f);                   // the truck is close: no near-reject (the solve shader would bypass it anyway)
        t.dl.Mv().SetEgoOrigin(0.0f);                    // no ego split: the whole truck is one rigid object
        t.dl.Mv().SetEgoPixel(0.0f);
        t.dl.SetOpticalCentre(0.5f, 0.5f);
    }
    // Cost visibility: the instance's own non-blocking GPU timestamp queries ("DLAA GPU cost eye 10 / 11" lines, every
    // 600 timed frames). Flat: only with gpu_timing = 1 (the flat default stays as before); VR: also in auto mode.
    t.dl.SetTiming(g_gpuTiming > 0 || (g_gpuTiming < 0 && g_launchVr == 1));
    t.dl.SetNoSharpen(!g_pvSharpen);                     // v0.8.1 round 5: dlaa.ini preview_sharpen = 0 (A/B)
    // Round 5: no reset on a reference-slot change any more -- the unit is the eye, so slot 0 vs 4 only reflects which eye
    // the game rendered first this frame (round 4 reset on it, but its units were order-bound, see the block comment).
    const bool reset = t.runs == 0 || t.lastFrame + 1 != fr || t.epoch != g_pvEpoch;
    // v0.8.1 round 4 debug (dlaa.ini preview_tile_jitter = 0): a TILED target's passes are not shifted, so NGX is told 0.
    const bool noTileJit = !g_pvTileJitter && idx >= 0 && idx < kPtMax && g_pvLay[idx].tiled;
    const float njx = noTileJit ? 0.0f : (float)g_signX * g_pvJx, njy = noTileJit ? 0.0f : (float)g_signY * g_pvJy;
    const int64_t t0 = t.runs == 0 ? Qpc() : 0;          // wall time of the first Run (NGX create + textures)
    const int areaNow = SceneDlaa::Area();               // preview targets always use the whole image (no dlaa_area crop)
    if (areaNow != 100) SceneDlaa::SetArea(100);
    // Round 6: MVs in picture space (pass B maps picture ndc into the reference tile's clip space and back).
    if (tiled) t.dl.Mv().SetTileXf(lay.hx, lay.hy, lay.cx, lay.cy);
    else       t.dl.Mv().SetTileXf(1.0f, 1.0f, 0.0f, 0.0f);
    t_inDlaa = true;
    bool ok = false;
    if (rgba || f16) {                                   // v0.8.0: RGBA16F in place too (HDR unit)
        ok = t.dl.Run(ctx, tg.tex.Get(), nullptr, depth, &rs.ps.cand, njx, njy, reset, g_mvOn.load(), g_mvDebug.load(), false, 0, 0);
    } else {
        PreviewBlit::SaveState(ctx);
        PreviewBlit::Draw(ctx, t.tgtSrv.Get(), t.scratchRtv.Get(), td.Width, td.Height);          // target -> RGBA8 scratch
        ok = t.dl.Run(ctx, t.scratch.Get(), nullptr, depth, &rs.ps.cand, njx, njy, reset, g_mvOn.load(), g_mvDebug.load(), false, 0, 0);
        if (ok) PreviewBlit::Draw(ctx, t.scratchSrv.Get(), t.tgtRtv.Get(), td.Width, td.Height);  // result -> target
        PreviewBlit::RestoreState(ctx);
    }
    t_inDlaa = false;
    if (areaNow != 100) SceneDlaa::SetArea(areaNow);
    if (!ok && t.dl.Deferred()) return;                  // v0.7.8: built at a later Present (one NGX create per Present)
    t.epoch = g_pvEpoch;
    if (t0) {
        static int firstLogs = 0;
        if (firstLogs++ < 8)
            Log("preview unit %d (eye tag %d, target order %d): %ux%u fmt=%d (%s), reference slot %d, DLAA unit created (first Run %.1f ms, ok=%d, Present #%llu)",
                ui, 10 + ui, idx, td.Width, td.Height, (int)td.Format,
                bgra ? "BGRA: copied through an RGBA8 scratch" : (f16 ? "RGBA16F (HDR): in place" : "RGBA8: in place"), ref,
                g_qpcFreq > 0 ? (double)(Qpc() - t0) * 1000.0 / (double)g_qpcFreq : 0.0, (int)ok, (unsigned long long)fr);
    }
    if (ok) {
        // MV prev of this run = the unit's record committed at its previous run (slot t.lastRef, order t.lastOrder); cur =
        // slot `ref`. Same unit = same eye, whatever the slot / order (round 5).
        if (t.runs < 2 || (t.lastOrder >= 0 && t.lastOrder != idx && g_pvOrderSwaps <= 4))
            Log("preview DLAA eval: unit %d (eye tag %d) target order %d Present #%llu viewport shift=(%+.4f,%+.4f) NGX "
                "jitter=(%+.4f,%+.4f) reset=%d ref slot %d (MV prev from slot %d, order %d) depth=snapshot", ui, 10 + ui, idx,
                (unsigned long long)fr, g_pvJx, g_pvJy, njx, njy, (int)reset, ref, t.lastRef, t.lastOrder);
        t.lastFrame = fr; t.lastRef = ref; t.lastOrder = idx; ++t.runs; ++g_pvOkFrame; ++g_pvRunsTotal;
        if (g_pvCapActive && !f16) PvCapAfterRun(ctx, ui, idx, tg, ref, njx, njy, bgra, td.Width, td.Height, fr);   // Ctrl+F10 capture (v0.8.0: 8-bit only)
        if (g_pvSmActive) PvSmAfterRun(ctx, ui, idx, tg, ref, njx, njy, bgra ? t.scratch.Get() : tg.tex.Get(), fr);   // v0.8.1 round 5
    } else {
        static int fails = 0;
        if (fails++ < 10)
            Log("preview DLAA: target %d (unit %d) Run failed (init failed=%d, Present #%llu) -- frame left untouched", idx,
                ui, (int)t.dl.InitFailed(), (unsigned long long)fr);
    }
}

// Called at the end of every top-level Present's pre-work (OnPresentBoundary): (c) flushes pending targets, logs, learns
// the reference slots, then starts the next frame (counters, ref drop, jitter for the next frame). The jitter phase is
// per Present here (no G-buffer pass advances g_jx / g_jy on this screen; in VR one Present covers both eyes); the
// viewport shift of every preview pass and the NGX jitter both read g_pvJx / g_pvJy, which only change here, so the
// blended result of a frame is consistently jittered.
// v0.8.1 round 3: the finished frame's preview CPU time into the ring; the armed layout-switch report collects the 8
// frames after the switch and is logged then (one line per switch, 20 at most).
void PvFrameCpuDone(uint64_t n) {
    const double ms = g_qpcFreq > 0 ? (double)g_pvCpuTicks * 1000.0 / (double)g_qpcFreq : 0.0;
    g_pvCpuTicks = 0;
    g_pvCpuRing[g_pvCpuHead] = ms;
    g_pvCpuHead = (g_pvCpuHead + 1) % 4;
    if (!g_pvSw.armed) return;
    g_pvSw.after[g_pvSw.nAfter++] = ms;
    if (g_pvSw.nAfter < 8) return;
    g_pvSw.armed = false;
    ++g_pvSwLogs;
    double mx = 0.0; int mi = 0;
    for (int i = 0; i < 8; ++i) if (g_pvSw.after[i] > mx) { mx = g_pvSw.after[i]; mi = i; }
    const PvSwitchRep& w = g_pvSw;
    const uint64_t staleMax = w.prevQueued ? (w.frame - w.prevQueued) : (w.frame - w.queued);
    Log("preview layout switch (target order %d, %s -> %s, verdict at game frame #%llu): from a readback queued at #%llu "
        "(%llu frame(s) latency), the readback before it at #%llu -> the old layout was applied to at most %llu frame(s) "
        "of the new picture (jitter scale / depth / MVs of the wrong layout there) | preview DLSS history resets %llu | "
        "picture depth created %.1f ms | our preview CPU ms per frame, 4 before: %.2f %.2f %.2f %.2f | 8 after: %.2f %.2f "
        "%.2f %.2f %.2f %.2f %.2f %.2f (max %.2f at +%d) | %s (frame #%llu)", w.g,
        !w.fromKnown ? "unknown" : (w.fromTiled ? "TILED" : "untiled"), w.toTiled ? "TILED" : "untiled",
        (unsigned long long)w.frame, (unsigned long long)w.queued, (unsigned long long)(w.frame - w.queued),
        (unsigned long long)w.prevQueued, (unsigned long long)staleMax,
        (unsigned long long)(g_pvInvalidates - w.inv0), g_pvCreateMs - w.create0,
        w.before[0], w.before[1], w.before[2], w.before[3], w.after[0], w.after[1], w.after[2], w.after[3], w.after[4],
        w.after[5], w.after[6], w.after[7], mx, mi,
        g_pvLayRecheck > 0 ? "settled layouts re-checked every preview_layout_recheck-th frame (dlaa.ini)"
        : g_plActive.load(std::memory_order_relaxed) ? "present layer: settled layouts re-checked every 4th frame"
                                                     : "settled layouts re-checked every 31st frame",
        (unsigned long long)n);
}

void PreviewNextFrame(uint64_t n) {
    PvFrameCpuDone(n);                                   // v0.8.1 round 3: the frame that just ended
    PvCpuScope cpuScope;
    if (g_ptPendingMask) {
        ID3D11DeviceContext* c = g_gameCtx.load(std::memory_order_acquire);
        for (int i = 0; i < kPtMax; ++i)
            if (c) PreviewFlush(c, i); else g_pt[i].pending = false;
        g_ptPendingMask = 0;
    }
    if (g_pvOkFrame > 0) {
        ++g_pvFrames;
        if (!g_pvRunLogged) {
            g_pvRunLogged = true;
            Log("preview DLAA: running on %d target(s) (Present #%llu) -- one %s DLAA per composite target, depth + MV from "
                "the reference slot", g_pvOkFrame, (unsigned long long)n,
                g_pvHdrTarget ? "HDR (RGBA16F)" : "LDR");   // v0.8.0
        }
        if (!g_pvT0) { g_pvT0 = Qpc(); g_pvF0 = n; }
        if (g_pvFrames % 600 == 0) {
            const double secs = g_qpcFreq > 0 ? (double)(Qpc() - g_pvT0) / (double)g_qpcFreq : 0.0;
            char refs[96];
            size_t rl = 0;
            refs[0] = 0;
            for (int i = 0; i < g_ptCount && i < kPtMax && rl + 24 < sizeof(refs); ++i) {
                const int w = snprintf(refs + rl, sizeof(refs) - rl, " t%d:slot%d", i, g_pt[i].curRef);
                if (w > 0) rl += (size_t)w;
            }
            // Round 5 eye map: order!=eye = target runs whose eye unit differed from the composite order (each one was a
            // cross-eye history mix in round 4); untouched = targets skipped (eye unknown yet / conflict).
            Log("preview DLAA stats: %llu preview frames, %llu target runs, %d target(s) ran this frame, reference slots [%s ], "
                "jitter phase %d/%d, %.1f Presents/s over the last window (Present #%llu) | eye map: %d RTs, verdicts=%llu "
                "no-vote=%llu corrections=%llu throttled=%llu, order!=eye=%llu, untouched: unknown-eye=%llu conflict=%llu",
                (unsigned long long)g_pvFrames, (unsigned long long)g_pvRunsTotal, g_pvOkFrame, refs,
                (int)(n % (uint64_t)(g_phases > 0 ? g_phases : 1)), g_phases, secs > 0.0 ? (double)(n - g_pvF0) / secs : 0.0,
                (unsigned long long)n, g_pvEyeN, (unsigned long long)g_pvVerdicts, (unsigned long long)g_pvNoVote,
                (unsigned long long)g_pvCorrections, (unsigned long long)g_pvRbThrottled, (unsigned long long)g_pvOrderSwaps,
                (unsigned long long)g_pvUndecided, (unsigned long long)g_pvConflicts);
            // Round 6: the tiled-picture depth assembly cost (GPU, non-blocking timestamps) and its health.
            ID3D11DeviceContext* const tc = g_gameCtx.load(std::memory_order_acquire);
            if (tc) g_pvAsmTimer.Poll(tc);
            if (g_pvAsmTilesWin > 0 || g_pvAsmMiss > 0) {
                const double per = g_pvAsmTimer.n ? g_pvAsmTimer.sum / (double)g_pvAsmTimer.n : 0.0;
                const double tpf = (double)g_pvAsmTilesWin / 600.0;
                Log("preview depth assembly GPU cost: %.3f ms per tile pass (shared DS copy + reduce, %llu timed), %.2f tile "
                    "passes per frame -> %.2f ms per frame | asm-miss (target left untouched) %llu in total | layout readbacks "
                    "throttled %llu, layout changes %llu", per, (unsigned long long)g_pvAsmTimer.n, tpf, per * tpf,
                    (unsigned long long)g_pvAsmMiss, (unsigned long long)g_pvLayThrottled, (unsigned long long)g_pvLayChanges);
                g_pvAsmTimer.sum = 0.0; g_pvAsmTimer.n = 0; g_pvAsmTilesWin = 0;
            }
            // v0.8.1 (round 5: every window, also untiled -- the round 4 test never printed it on the main menu)
            Log("preview tile edge fix (v0.8.1): %llu uncovered tile edge column(s) / row(s) filled in total, %llu "
                "zero-guarded fills (shift <= 0.49 px, round 5) | late composites (after the target's DLAA) %llu | layout "
                "changes %llu, now %s | dlaa.ini preview_edge_fix=%d preview_tile_jitter=%d preview_layout_recheck=%d "
                "preview_sharpen=%d", (unsigned long long)g_pvEdgeFixes, (unsigned long long)g_pvEdgeGuarded,
                (unsigned long long)g_pvLateComps, (unsigned long long)g_pvLayChanges,
                !g_pvLay[0].known ? "unknown" : (g_pvLay[0].tiled ? "TILED" : "untiled"), g_pvEdgeFix, g_pvTileJitter,
                g_pvLayRecheck, g_pvSharpen);
            g_pvT0 = Qpc(); g_pvF0 = n;
        }
    }
    if (g_pvOkFrame > 0 && !g_pvFirstRunPresent) g_pvFirstRunPresent = n;
    {   // v0.7.8 Ctrl+F10 on the preview screen: measuring capture (see PvCap); the world F10 snapshot is untouched.
        // dlaa.ini preview_capture_at = N (debug): the same capture once, N Presents after the preview DLAA first ran.
        ID3D11DeviceContext* c = g_gameCtx.load(std::memory_order_acquire);
        const bool autoCap = g_pvCaptureAt > 0 && !g_pvCaptureAtDone && g_pvFirstRunPresent &&
                             n >= g_pvFirstRunPresent + (uint64_t)g_pvCaptureAt;
        if (c && g_pvCapActive) PvCapAtPresent(c, n);
        else if (c && g_pvSmActive) {                    // v0.8.1 round 5 seam capture (before the targets reset)
            g_snapRequest = false;                       // a key press while it runs does not queue a second one
            PvSmAtFrameEnd(c, n);
        }
        else if (c && g_pvOkFrame > 0 && (g_snapRequest.load(std::memory_order_relaxed) || autoCap)) {
            if (autoCap) {
                g_pvCaptureAtDone = true;
                Log("pvcap: automatic start (dlaa.ini preview_capture_at=%d, preview DLAA first ran at Present #%llu)",
                    g_pvCaptureAt, (unsigned long long)g_pvFirstRunPresent);
            }
            g_snapRequest = false;
            // v0.8.0: the measuring capture is 8-bit only. v0.8.1 round 5: an RGBA16F (HDR) target -- or any target with
            // dlaa.ini preview_seam_capture = 1 -- gets the seam capture instead (PvSeam).
            if (g_pvHdrTarget || g_pvSeamAlways) PvSmStart(c, n);
            else PvCapStart(c, n);
        }
    }
    if (g_ptCount > 0) {                                 // learn the reference slots (lowest composited slot per target)
        int mask = 0;
        for (int i = 0; i < g_ptCount && i < kPtMax; ++i)
            if (g_pt[i].curRef < kPvSlots) mask |= 1 << g_pt[i].curRef;
        if (mask != g_ptRefMask) {
            static int logs = 0;
            if (logs++ < 20)
                Log("preview reference slots mask 0x%x -> 0x%x (%d target(s), Present #%llu)", g_ptRefMask, mask, g_ptCount,
                    (unsigned long long)n);
            g_ptRefMask = mask;
        }
        // Round 6: the group (= composite order) each slot went into, for the next frame's tile-pass depth assembly.
        for (int s = 0; s < kPvSlots; ++s) g_pvSlotGroup[s] = -1;
        for (int i = 0; i < g_ptCount && i < kPtMax; ++i)
            for (int s = 0; s < kPvSlots; ++s)
                if ((g_pt[i].slotMask >> s) & 1) g_pvSlotGroup[s] = i;
        // Depth twins: only the reference slots of UNTILED groups keep one (a tiled group uses g_pvTileTwin + the picture
        // depth). Candidate records stay for every slot (16 KB each): round 6 reads every pass's first candidate.
        for (int i = 0; i < kPvSlots; ++i) {
            const int grp = g_pvSlotGroup[i];
            const bool tiledSlot = grp >= 0 && g_pvLay[grp].tiled;
            if (!((g_ptRefMask >> i) & 1) || tiledSlot) g_pv[i].ps.twin.Shutdown();
        }
    }
    // Round 6 tile layout: finished readbacks first, then this frame's passes (flat and VR).
    if ((g_pvLayPending > 0 || g_ptCount > 0) && g_dlaaOn.load(std::memory_order_relaxed) &&
        !g_passive.load(std::memory_order_relaxed)) {
        ID3D11DeviceContext* c = g_gameCtx.load(std::memory_order_acquire);
        if (c) {
            t_inDlaa = true;
            if (g_pvLayPending > 0) PvPollLayoutReadbacks(c, n);
            PvQueueLayoutReadback(c, n);
            t_inDlaa = false;
        }
    }
    for (PvAsm& a : g_pvAsm) { a.frameMask = 0; a.twin.valid = false; }
    // Round 5 VR eye map: finished verdicts first, then this frame's two reference records (see PvEyeEntry).
    if (g_launchVr == 1 && (g_pvRbPending > 0 || g_ptCount == 2) && g_dlaaOn.load(std::memory_order_relaxed) &&
        !g_passive.load(std::memory_order_relaxed)) {
        ID3D11DeviceContext* c = g_gameCtx.load(std::memory_order_acquire);
        if (c) {
            t_inDlaa = true;
            if (g_pvRbPending > 0) PvPollEyeReadbacks(c);
            PvQueueEyeReadback(c, n);
            t_inDlaa = false;
        }
    }
    g_pvCount = 0; g_pvOkFrame = 0; g_pvCur = -1;
    g_ptCount = 0; g_ptPendingMask = 0; g_ptBoundIdx = -1; g_pvUnitMask = 0;
    for (PvSlot& s : g_pv) { s.rt.Reset(); s.composited = false; }
    for (PvTarget& t : g_pt) {
        t.tex.Reset(); t.pending = false; t.curRef = 99; t.unit = -1; t.slotMask = 0;
        t.flushes = 0; t.maskAtFlush = 0;                // v0.8.1 round 5
    }
    g_pvDs.Reset();
    if (g_jitterEnabled) JitterOfPhase((int)((n + 1) % (uint64_t)(g_phases > 0 ? g_phases : 1)), &g_pvJx, &g_pvJy);
    else { g_pvJx = 0.0f; g_pvJy = 0.0f; }
    // v0.8.1 round 5 seam capture, second part: the preview jitter forced to 0 (viewport shift 0 AND NGX told 0, both read
    // these two values) -- the same numbers without any shift of ours.
    if (g_pvSmActive && g_pvSm && g_pvSm->f >= kSmA) { g_pvJx = 0.0f; g_pvJy = 0.0f; }
}

// hkOMSetRenderTargets (game context, not t_inDlaa): sets g_previewPass. `gbuf` = this bind is the world G-buffer
// (4 RTVs + scene DSV). Preview pass = preview_dlaa on, n == 1, DSV set, RTV0 R11G11B10_FLOAT, RT size == DSV size and
// >= 1024 wide (flat: the backbuffer size; VR: the eye size), single-sample, DSV D32_FLOAT_S8X24 / R32G8X24_TYPELESS,
// and no world G-buffer bind within the last kPvQuietFrames Presents (so it never fires in the game world). A new
// (non-redundant) preview bind takes the next slot of the frame and records its RT; passes beyond kPvSlots are
// ignored (no jitter, no DLAA).
void PreviewOnBind(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv, bool gbuf) {
    const uint64_t fr = g_frames.load(std::memory_order_relaxed);
    if (gbuf) { g_lastGbufFrame = fr; g_pvCur = -1; g_pvCount = 0; }
    bool pv = false;
    if (n == 1 && rtvs && rtvs[0] && dsv && g_previewDlaa &&
        !g_gameDeviceChanged.load(std::memory_order_relaxed) && fr - g_lastGbufFrame > kPvQuietFrames &&
        PreviewFramesOk()) {                                     // v0.8.1: exact frames needed (present layer)
        Microsoft::WRL::ComPtr<ID3D11Resource> rres, dres;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> rt, ds;
        rtvs[0]->GetResource(&rres);
        dsv->GetResource(&dres);
        if (rres) rres.As(&rt);
        if (dres) dres.As(&ds);
        if (rt && ds) {
            D3D11_TEXTURE2D_DESC rd{}, dd{};
            rt->GetDesc(&rd);
            ds->GetDesc(&dd);
            const bool ok = rd.Format == DXGI_FORMAT_R11G11B10_FLOAT && rd.Width >= 1024 &&
                            rd.SampleDesc.Count == 1 && dd.Width == rd.Width && dd.Height == rd.Height &&
                            dd.SampleDesc.Count == 1 &&
                            (dd.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || dd.Format == DXGI_FORMAT_R32G8X24_TYPELESS);
            if (ok) {
                if (g_previewPass && g_pvCur >= 0 && rt.Get() == g_pv[g_pvCur].rt.Get() && ds.Get() == g_pvDs.Get()) {
                    pv = true;                                   // redundant rebind inside the same pass: same slot
                } else if (g_pvCount < kPvSlots) {
                    const int slot = g_pvCount++;
                    PvSlot& s = g_pv[slot];
                    s.ps.cand.Reset();
                    s.ps.twin.valid = false;
                    s.ps.snap = false;
                    s.rt = rt; s.composited = false;
                    s.capShX = s.capShY = 0.0f; s.capShN = s.capUnshN = 0;   // v0.7.8 capture: viewport decisions of this pass
                    s.shX = s.shY = 0.0f; s.shOn = false;                     // v0.8.1 round 4: the shift of this pass
                    s.capVp = D3D11_VIEWPORT{};                               // v0.8.1 round 5 seam capture
                    s.diCount = 0;                                            // round 6: per-pass collector diagnostics
                    s.ps.skSampled = s.ps.skNoCtx = s.ps.skFull = s.ps.skNoCb = s.ps.skRange = 0;
                    g_pvDs = ds;
                    g_pvW = rd.Width; g_pvH = rd.Height;
                    g_pvCur = slot;
                    pv = true;
                    if (!g_pvDetectLogged) {
                        g_pvDetectLogged = true;
                        Log("preview pass detected (profile / truck-preview screen): RT %ux%u fmt=%d (R11G11B10_FLOAT) + DSV %ux%u "
                            "fmt=%d, backbuffer %ux%u fmt=%d (%s), no world G-buffer bind for %llu Presents (Present #%llu) -- "
                            "one DLAA per composite target, up to %d slots per frame",
                            rd.Width, rd.Height, (int)rd.Format, dd.Width, dd.Height, (int)dd.Format, g_bbW, g_bbH, (int)g_bbFmt,
                            (rd.Width == g_bbW && rd.Height == g_bbH) ? "RT = backbuffer size" : "RT is eye-sized",
                            (unsigned long long)(fr - g_lastGbufFrame), (unsigned long long)fr, kPvSlots);
                    }
                } else {
                    g_pvCur = -1;                                // beyond the slot cap: ignored (neither jittered nor collected)
                    static int capLogs = 0;
                    if (capLogs++ < 3)
                        Log("preview pass beyond the %d-slot cap ignored (Present #%llu)", kPvSlots, (unsigned long long)fr);
                }
            }
        }
    }
    g_previewPass = pv;
}

// Trigger (b), from hkOMSetRenderTargets after PreviewOnBind while some target is pending: a bind that is not a preview
// pass and does not bind the pending target itself flushes that target. Also tracks which pending target is the
// current RT0 (g_ptBoundIdx), so the per-Draw / DrawIndexed trigger (a) is a plain integer test. (The DLAA runs after
// the game's bind went through; it saves / restores whatever it changes, so the game's new binding is what comes back.)
void PreviewOnBindTargets(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs) {
    Microsoft::WRL::ComPtr<ID3D11Resource> res;
    if (n >= 1 && rtvs && rtvs[0]) rtvs[0]->GetResource(&res);
    int bound = -1;
    for (int i = 0; i < kPtMax; ++i)
        if ((g_ptPendingMask >> i & 1) && res && res.Get() == (ID3D11Resource*)g_pt[i].tex.Get()) bound = i;
    if (!g_previewPass)
        for (int i = 0; i < kPtMax; ++i)
            if ((g_ptPendingMask >> i & 1) && i != bound) PreviewFlush(ctx, i);
    g_ptBoundIdx = (bound >= 0 && (g_ptPendingMask >> bound & 1)) ? bound : -1;
}

// DiscardResource / DiscardView / DiscardView1 on the game context: the preview depth (one DS shared by all the
// passes) is snapshotted into the twin of the pass just drawn (g_pvCur) right before the discard destroys it (same
// as the VR snapshot-at-discard) -- only for a learned reference slot. g_pvCur keeps pointing at the newest preview pass
// until the next preview bind.
// ---- v0.8.1 round 4: TILED preview, uncovered tile edge = the black seam line --------------------------------------------
// The truck screen draws its picture as 2x2 tiles; every tile pass renders a QUARTER of the picture into the full tile RT
// (2x supersampled: 1 tile px = 0.5 picture px) and the game's composite shrinks it into its quadrant. To move the
// PICTURE by the NGX jitter (g_pvJx, g_pvJy in [-0.5, 0.5) picture px) PvShiftScale scales the tile pass's viewport shift
// by a * h = 2: up to +-0.875 tile px. A viewport moved right by sx > 0.5 px no longer contains the centre (0.5) of tile
// column 0, and one moved left by sx <= -0.5 no longer contains the centre of the last column (W - 0.5 < W + sx): that
// whole column is never rasterized and keeps the pass's clear colour, (0,0,0,0) = black. Same for rows with sy. Untiled
// passes (scale 1, |shift| <= 0.4375) never lose a whole pixel. With the 8 Halton phases sx = 2 * jx = 0, -0.5, +0.5,
// -0.75, +0.25, -0.25, +0.75, -0.875: 3 of 8 frames lose the tiles' RIGHT column, 1 of 8 their LEFT column. The right
// column of the left tiles and the left column of the right tiles meet at picture x = W/2: after the 2:1 composite a
// half-black picture column at the screen's horizontal middle in half of the frames -> DLSS integrates a faint dark line
// top to bottom (user, round 3 test: exactly there, gone with the mode off). sy = 2 * jy gives the same at the
// horizontal seam (4 of 8 frames, top / bottom rows) and at the screen edges. Fix: at the pass's DS discard (its RT is
// complete, the composite follows) each uncovered edge column / row is replaced by its inner neighbour (2 small copies
// through a 1-px scratch strip; a copy inside one subresource is not used). An edge shift of a fraction of a tile pixel
// stays (sub-pixel, invisible). dlaa.ini preview_edge_fix = 0 turns it off (A/B).
// v0.8.1 round 5: ZERO-GUARDED fill for the smaller shifts. The rule above (a pixel is rasterized when its CENTRE lies
// inside the shifted viewport -> |shift| < 0.5 never loses a column) is the D3D11 rule, but the round 4 test log has the
// line in a run where no pass was shifted by 0.5 or more (untiled verdict, scale 1, |shift| <= 0.4375). So for
// 0 < |shift| <= 0.49 the edge column / row on the side the viewport moved away from is no longer trusted: a small compute
// pass (kPvEdgeFillShader, from the shader cache) copies that edge + its inner neighbour into a 2-texel strip and writes
// back the edge with every texel that is EXACTLY the clear colour (0,0,0) replaced by its inner neighbour. Rendered
// R11G11B10 texels are practically never exactly 0 (and a true black one gets its neighbour = no visible change), so on a
// GPU that follows the rule this changes nothing; on one that drops the fractional edge it removes the black column.
// The seam capture (pvseam, snapshot key) logs the zero texels of every tile edge BEFORE this fix = the proof either way.
// |shift| > 0.49 keeps the unconditional copy above (provably uncovered). Both off with dlaa.ini preview_edge_fix = 0.
// kPvEdgeFillShader-BEGIN (the build validates this block with fxc)
const char kPvEdgeFillShader[] = R"(
cbuffer EdgeCB : register(b0) {
    uint Count;      // texels along the edge (tile RT height for a column, width for a row)
    uint Axis;       // 0 = column (Strip is 2 x Count), 1 = row (Strip is Count x 2)
    uint EdgeIdx;    // 0 / 1: which of the 2 strip texels is the edge; the other one is its inner neighbour
    uint Pad;
};
Texture2D<float4>   Strip : register(t0);
RWTexture2D<float4> Dst   : register(u0);   // 1 x Count (column) / Count x 1 (row), the tile RT's format

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= Count) return;
    const int i = (int)id.x;
    const int e = (int)EdgeIdx, n = 1 - (int)EdgeIdx;
    const int2 pe = Axis == 0 ? int2(e, i) : int2(i, e);
    const int2 pn = Axis == 0 ? int2(n, i) : int2(i, n);
    const float3 ce = Strip.Load(int3(pe, 0)).rgb;
    const float3 cn = Strip.Load(int3(pn, 0)).rgb;
    Dst[Axis == 0 ? int2(0, i) : int2(i, 0)] = float4(all(ce == 0.0) ? cn : ce, 1.0);
}
)";
// kPvEdgeFillShader-END

void PvEdgeRegisterShaders() {
    ShaderCache::Add(ShaderCache::kPvEdgeFill, kPvEdgeFillShader, sizeof(kPvEdgeFillShader) - 1, "pv_edge_fill", "CSMain", "cs_5_0");
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> g_pvEdgeCol, g_pvEdgeRow;   // 1 x H and W x 1 scratch strips (tile RT format)
bool             g_pvEdgeFailed = false;
// round 5 zero-guarded fill: 2-texel strips (SRV) + 1-texel results (UAV) per axis, its CS + constant buffer
PcPtr<ID3D11Texture2D>           g_pvEgStrip[2], g_pvEgOut[2];    // [0] column (2 x H / 1 x H), [1] row (W x 2 / W x 1)
PcPtr<ID3D11ShaderResourceView>  g_pvEgStripSrv[2];
PcPtr<ID3D11UnorderedAccessView> g_pvEgOutUav[2];
PcPtr<ID3D11ComputeShader>       g_pvEgCs;
PcPtr<ID3D11Buffer>              g_pvEgCb;
bool             g_pvEgFailed = false;

// The zero-guarded fill's resources for a tile RT of `d` (axis 0 = column, 1 = row). false = not available (logged once
// and off for the session; a shader-cache warm-up still running is a plain false without the log).
bool PvEnsureEdgeGuard(ID3D11DeviceContext* ctx, const D3D11_TEXTURE2D_DESC& d, int axis) {
    if (g_pvEgFailed) return false;
    PcPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!dev) return false;
    HRESULT hr = S_OK;
    const char* what = "";
    if (!g_pvEgCs) {
        if (!ShaderCache::Done()) return false;
        size_t size = 0;
        const void* code = ShaderCache::Code(ShaderCache::kPvEdgeFill, &size);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (!code) { hr = E_FAIL; what = "shader"; }
        else if (FAILED(hr = dev->CreateComputeShader(code, size, nullptr, &g_pvEgCs))) what = "compute shader";
        else if (FAILED(hr = dev->CreateBuffer(&bd, nullptr, &g_pvEgCb))) what = "constant buffer";
    }
    const UINT sw = axis == 0 ? 2u : d.Width, sh = axis == 0 ? d.Height : 2u;
    const UINT ow = axis == 0 ? 1u : d.Width, oh = axis == 0 ? d.Height : 1u;
    if (SUCCEEDED(hr) && g_pvEgStrip[axis]) {
        D3D11_TEXTURE2D_DESC e{};
        g_pvEgStrip[axis]->GetDesc(&e);
        if (e.Width != sw || e.Height != sh || e.Format != d.Format) {
            g_pvEgStrip[axis].Reset(); g_pvEgStripSrv[axis].Reset(); g_pvEgOut[axis].Reset(); g_pvEgOutUav[axis].Reset();
        }
    }
    if (SUCCEEDED(hr) && !g_pvEgStrip[axis]) {
        D3D11_TEXTURE2D_DESC td{};
        td.MipLevels = 1; td.ArraySize = 1; td.Format = d.Format; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
        td.Width = sw; td.Height = sh; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(hr = dev->CreateTexture2D(&td, nullptr, &g_pvEgStrip[axis])) ||
            FAILED(hr = dev->CreateShaderResourceView(g_pvEgStrip[axis].Get(), nullptr, &g_pvEgStripSrv[axis]))) {
            what = "strip texture / SRV";
        } else {
            td.Width = ow; td.Height = oh; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(hr = dev->CreateTexture2D(&td, nullptr, &g_pvEgOut[axis])) ||
                FAILED(hr = dev->CreateUnorderedAccessView(g_pvEgOut[axis].Get(), nullptr, &g_pvEgOutUav[axis])))
                what = "result texture / typed UAV";
        }
    }
    if (FAILED(hr)) {
        g_pvEgFailed = true;
        g_pvEgStrip[axis].Reset(); g_pvEgStripSrv[axis].Reset(); g_pvEgOut[axis].Reset(); g_pvEgOutUav[axis].Reset();
        Log("preview tile edge fix: the zero-guarded fill for shifts below 0.5 px is unavailable (%s, hr=0x%lx, tile RT fmt=%d, "
            "%s) -- only the > 0.49 px copy stays", what, (unsigned long)hr, (int)d.Format,
            ShaderCache::Error(ShaderCache::kPvEdgeFill));
        return false;
    }
    return true;
}

void PvFixTileEdges(ID3D11DeviceContext* ctx, PvSlot& s) {
    if (!g_pvEdgeFix || !s.shOn || !s.rt) return;
    const bool fl = s.shX > 0.49f, fr = s.shX < -0.49f, ft = s.shY > 0.49f, fb = s.shY < -0.49f;
    // round 5: the side a smaller shift moved the viewport away from (zero-guarded fill only)
    constexpr float kEps = 1e-4f;
    const bool gl = !fl && s.shX > kEps, gr = !fr && s.shX < -kEps, gt = !ft && s.shY > kEps, gb = !fb && s.shY < -kEps;
    if (!(fl || fr || ft || fb || gl || gr || gt || gb)) return;
    D3D11_TEXTURE2D_DESC d{};
    s.rt->GetDesc(&d);
    if (d.Width < 4 || d.Height < 4 || d.SampleDesc.Count != 1) return;
    ID3D11Texture2D* const rt = s.rt.Get();
    if ((fl || fr || ft || fb) && !g_pvEdgeFailed) {
        auto ensure = [&](Microsoft::WRL::ComPtr<ID3D11Texture2D>& t, UINT w, UINT h) {
            if (t) {
                D3D11_TEXTURE2D_DESC e{};
                t->GetDesc(&e);
                if (e.Width == w && e.Height == h && e.Format == d.Format) return true;
                t.Reset();
            }
            Microsoft::WRL::ComPtr<ID3D11Device> dev;
            ctx->GetDevice(&dev);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = d.Format;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            return dev && SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &t)) && t;
        };
        if (((fl || fr) && !ensure(g_pvEdgeCol, 1, d.Height)) || ((ft || fb) && !ensure(g_pvEdgeRow, d.Width, 1))) {
            g_pvEdgeFailed = true;
            Log("preview tile edge fix: scratch strip create failed (tile RT %ux%u fmt=%d) -- off for this session", d.Width,
                d.Height, (int)d.Format);
        } else {
            t_inDlaa = true;
            auto col = [&](UINT from, UINT to) {         // column `from` -> column `to` (all rows)
                const D3D11_BOX b{ from, 0, 0, from + 1, d.Height, 1 };
                ctx->CopySubresourceRegion(g_pvEdgeCol.Get(), 0, 0, 0, 0, rt, 0, &b);
                ctx->CopySubresourceRegion(rt, 0, to, 0, 0, g_pvEdgeCol.Get(), 0, nullptr);
            };
            auto row = [&](UINT from, UINT to) {         // row `from` -> row `to` (all columns)
                const D3D11_BOX b{ 0, from, 0, d.Width, from + 1, 1 };
                ctx->CopySubresourceRegion(g_pvEdgeRow.Get(), 0, 0, 0, 0, rt, 0, &b);
                ctx->CopySubresourceRegion(rt, 0, 0, to, 0, g_pvEdgeRow.Get(), 0, nullptr);
            };
            if (fl) col(1, 0);                           // columns first, then rows (the rows carry the fixed corners)
            if (fr) col(d.Width - 2, d.Width - 1);
            if (ft) row(1, 0);
            if (fb) row(d.Height - 2, d.Height - 1);
            t_inDlaa = false;
            g_pvEdgeFixes += (fl ? 1 : 0) + (fr ? 1 : 0) + (ft ? 1 : 0) + (fb ? 1 : 0);
            static int logs = 0;
            if (logs++ < 3)
                Log("preview tile edge fix: pass slot %d shifted (%+.4f,%+.4f) tile px on a %ux%u tile RT -- %s%s%s%s left "
                    "uncovered (cleared black) -> filled from the inner neighbour before the game's composite (dlaa.ini "
                    "preview_edge_fix=0 turns this off)", g_pvCur, (double)s.shX, (double)s.shY, d.Width, d.Height,
                    fl ? "left column " : "", fr ? "right column " : "", ft ? "top row " : "", fb ? "bottom row " : "");
        }
    }
    const bool gcol = (gl || gr) && PvEnsureEdgeGuard(ctx, d, 0);
    const bool grow = (gt || gb) && PvEnsureEdgeGuard(ctx, d, 1);
    if (!gcol && !grow) return;
    t_inDlaa = true;
    PvCsSave cs;
    cs.Save(ctx);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11UnorderedAccessView* nullUav = nullptr;
    const UINT keep = (UINT)-1;
    ID3D11Buffer* cb = g_pvEgCb.Get();
    ctx->CSSetShader(g_pvEgCs.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &cb);
    int n = 0;
    auto fill = [&](int axis, UINT edge) {               // edge column (axis 0) / row (axis 1) `edge`, inner = its neighbour
        const UINT len = axis == 0 ? d.Height : d.Width;
        const UINT lo = edge == 0 ? 0u : edge - 1;       // the strip holds [lo, lo + 2): edge 0 -> idx 0, last -> idx 1
        const D3D11_BOX b = axis == 0 ? D3D11_BOX{ lo, 0, 0, lo + 2, d.Height, 1 } : D3D11_BOX{ 0, lo, 0, d.Width, lo + 2, 1 };
        ctx->CopySubresourceRegion(g_pvEgStrip[axis].Get(), 0, 0, 0, 0, rt, 0, &b);
        D3D11_MAPPED_SUBRESOURCE mp{};
        if (FAILED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) return;
        const UINT c[4] = { len, (UINT)axis, edge == 0 ? 0u : 1u, 0u };
        memcpy(mp.pData, c, sizeof(c));
        ctx->Unmap(cb, 0);
        ID3D11ShaderResourceView* srv = g_pvEgStripSrv[axis].Get();
        ID3D11UnorderedAccessView* uav = g_pvEgOutUav[axis].Get();
        ctx->CSSetShaderResources(0, 1, &srv);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
        ctx->Dispatch((len + 63) / 64, 1, 1);
        ctx->CSSetShaderResources(0, 1, &nullSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
        ctx->CopySubresourceRegion(rt, 0, axis == 0 ? edge : 0, axis == 0 ? 0 : edge, 0, g_pvEgOut[axis].Get(), 0, nullptr);
        ++n;
    };
    if (gcol && gl) fill(0, 0);                          // columns first, then rows (as above)
    if (gcol && gr) fill(0, d.Width - 1);
    if (grow && gt) fill(1, 0);
    if (grow && gb) fill(1, d.Height - 1);
    cs.Restore(ctx);
    t_inDlaa = false;
    g_pvEdgeGuarded += (uint64_t)n;
    static int glogs = 0;
    if (n && glogs++ < 2)
        Log("preview tile edge fix (zero-guarded, v0.8.1 round 5): pass slot %d shifted (%+.4f,%+.4f) tile px -- %s%s%s%s: "
            "texels still at the clear colour (0,0,0) are replaced by their inner neighbour, rendered ones stay",
            g_pvCur, (double)s.shX, (double)s.shY, (gcol && gl) ? "left column " : "", (gcol && gr) ? "right column " : "",
            (grow && gt) ? "top row " : "", (grow && gb) ? "bottom row " : "");
}

void PreviewOnDiscard(ID3D11DeviceContext* ctx, ID3D11Resource* res) {
    PvCpuScope cpuScope;                                 // v0.8.1 round 3
    if (t_inDlaa || !res || g_pvCur < 0 || (ID3D11Resource*)g_pvDs.Get() != res) return;
    if (g_pvSmActive) PvSmOnPass(ctx, g_pvCur);          // v0.8.1 round 5 seam capture: the tile edges BEFORE the fix
    PvFixTileEdges(ctx, g_pv[g_pvCur]);                  // v0.8.1 round 4: before any early return below (see there)
    if (!g_dlaaOn.load(std::memory_order_relaxed) || g_jitterOnly.load(std::memory_order_relaxed) ||
        g_passive.load(std::memory_order_relaxed)) return;
    // Round 6: a pass of a TILED group goes into the group's picture depth (every tile, see PvLayout); the reference
    // slot's own snapshot below is only the untiled / not-yet-known path.
    const int grp = g_pvSlotGroup[g_pvCur];
    if (grp >= 0 && grp < kPtMax && g_pvLay[grp].tiled && ((g_pvLay[grp].slotMask >> g_pvCur) & 1)) {
        if (SceneDlaa::ShadersReady() && !((g_pvAsm[grp].frameMask >> g_pvCur) & 1)) PvAssembleTile(ctx, grp, g_pvCur);
        return;
    }
    if (!((g_ptRefMask >> g_pvCur) & 1)) return;
    PvSlot& s = g_pv[g_pvCur];
    if (s.ps.snap) return;
    t_inDlaa = true;
    const bool ok = s.ps.twin.Snapshot(ctx, g_pvDs.Get(), false);
    t_inDlaa = false;
    s.ps.snap = ok;
    static int logs = 0;
    if (logs < 2 || (!ok && logs < 6)) {
        ++logs;
        Log("preview depth snapshot slot %d: %s (before the DS_P discard)", g_pvCur, ok ? "captured" : "FAILED");
    }
}
#endif

// ---- v0.5.2 pass FIFO ----------------------------------------------------------------------
// Starts a new G-buffer pass. Called from the scene-depth clear (before the game's clear runs, so the scene
// depth still holds the previous pass) or, for the very first pass, from the first G-buffer bind.
// If the newest existing pass has not been blitted yet, its depth is about to be destroyed: snapshot it
// into that pass's eye twin first.
void StartPass(ID3D11DeviceContext* ctx, bool allowSnapshot) {
    if (allowSnapshot && g_passSeq > g_fifoHead && !g_flatMode) {
        PassSlot& prev = g_ring[(g_passSeq - 1) % kRing];
#ifdef WITH_DLAA
        if (!prev.snap && g_dlaaOn.load(std::memory_order_relaxed) && !g_jitterOnly.load(std::memory_order_relaxed) &&
            !g_passive.load(std::memory_order_relaxed) && g_sceneDepth) {
            t_inDlaa = true;
            const bool ok = prev.twin.Snapshot(ctx, g_sceneDepth, GpuTimingOn());
            t_inDlaa = false;
            prev.snap = ok;
            if (ok) ++g_cSnapAtClear;
            if (ok && g_snapCount++ == 0)
                Log("depth snapshot: captured for pass s=%llu before the scene depth clear (first occurrence)",
                    (unsigned long long)prev.s);
            else if (!ok && g_snapCount == 0)
                Log("depth snapshot: FAILED (pass s=%llu will use live depth)", (unsigned long long)prev.s);
        }
#else
        (void)ctx; (void)prev;
#endif
    }
    // v0.6.2 GPU spans: the previous pass's gbuf span ends here (after its snapshot above, before the game's
    // clear); the new pass's spans begin after the clear (SpanBeginPending). No unconsumed pass = frame start.
    SpanOnPassStart(ctx, g_passSeq, g_fifoHead == g_passSeq);
    // v0.6.1: the previous newest pass ends here -> freeze its deferred-context DrawIndexed delta.
    // v0.6.2: same for the foreign-context and game-context-other-thread counters.
    const uint32_t defNow = g_diDeferred.load(std::memory_order_relaxed);
    const uint32_t fgnNow = g_foreignCtx.load(std::memory_order_relaxed);
    const uint32_t otNow  = g_gameOtherThread.load(std::memory_order_relaxed);
    if (g_passSeq) {
        PassSlot& last = g_ring[(g_passSeq - 1) % kRing];
        if (!last.defClosed) {
            last.diDeferred = defNow - last.defStart;
            last.fgnCtx = fgnNow - last.fgnStart;
            last.otherThread = otNow - last.otStart;
            last.defClosed = true;
        }
    }
    const uint64_t s = g_passSeq++;
    if (s == 0)   // v0.7.10: first scene pass recognised this process -- the G-buffer / scene render path is now tracked
        Log("first scene pass recognised: s=0 at Present #%llu (the 3D scene render is being tracked)",
            (unsigned long long)g_frames.load(std::memory_order_relaxed));
    PassSlot& n = g_ring[s % kRing];
    n.s = s;
    n.ResetRecorder(defNow, fgnNow, otNow, Qpc());
    const int phases = EffectivePhases();                 // v0.7.0: more phases while upscaling
    if (phases != g_effPhases) {
        Log("jitter: %d Halton phases now (jitter_phases %d, upscale pixel ratio %.3f)", phases, g_phases, g_upAreaRatio);
        g_effPhases = phases;
    }
    n.phase = (int)((g_vrMode ? (s >> 1) : s) % (uint64_t)phases);
    if (g_jitterEnabled && !WorldJitterHeld()) JitterOfPhase(n.phase, &n.jx, &n.jy);   // v0.7.9: held until VR is real
    else { n.jx = 0.0f; n.jy = 0.0f; }
    n.snap = false;
    n.discarded = false;
    n.consumed = false;
#ifdef WITH_DLAA
    n.twin.valid = false;
    n.cand.Reset();
#endif
    g_phase = n.phase; g_jx = n.jx; g_jy = n.jy;          // ReconcileViewports uses these for this pass
    while (g_passSeq - g_fifoHead > 2) { ++g_fifoHead; ++g_cOverflow; }   // fifo overflow: drop the oldest
    static int logged = 0;
    if (logged < 12) {
        ++logged;
        Log("pass start: s=%llu phase=%d jitter=(%+.4f,%+.4f) outstanding=%d mode=%s",
            (unsigned long long)s, n.phase, n.jx, n.jy, (int)(g_passSeq - g_fifoHead),
            g_vrMode ? "VR" : (g_flatMode ? "flat" : "undetected"));
    }
}

// v0.6.1: called for EVERY immediate-context ClearDSV (was: only while g_depthDirty) so the flight recorder
// also counts the scene-depth clears that do not start a pass.
void OnSceneDepthClear(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* v) {
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    const bool match = r && r == (ID3D11Resource*)g_sceneDepth;
    if (r) r->Release();
    if (!match) return;
    // A clear only starts a pass if the previous pass actually rendered into the G-buffer.
    if (g_depthDirty) {
        g_depthDirty = false;
        // v0.8.1: present-layer FALLBACK frame boundary (only while the frame-end rule found no frame end for 1 s):
        // the clear that starts a world pass = a new frame in flat. Runs the per-frame work before the pass starts.
        if (g_plActive.load(std::memory_order_relaxed)) LayerFrameBoundary(ctx, kPlbSceneClear);
        StartPass(ctx, true);
    }
    if (g_passSeq) ++g_ring[(g_passSeq - 1) % kRing].sceneClears;   // lands on the pass it started, if any
}


void STDMETHODCALLTYPE hkClearDSV(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* v, UINT fl, FLOAT d, UINT8 s) {
    TraceClearDsv(ctx, v, fl, d, s);
    // v0.6.2: only the game's render context is tracked; every other context passes straight through.
    if (!IsGameCtx(ctx)) { oClearDSV(ctx, v, fl, d, s); return; }
    if (v && !t_inDlaa && g_sceneDepth)
        OnSceneDepthClear(ctx, v);
    oClearDSV(ctx, v, fl, d, s);
    if (g_spanPendGbuf || g_spanPendScene) SpanBeginPending(ctx);   // v0.6.2: spans start after the clear
}

void TraceDiscard(ID3D11DeviceContext* ctx, const char* name, ID3D11Resource* res, ID3D11View* view, bool isScene, bool snapped) {
    if (!TraceOn()) return;
    LB b; CtxTag(b, ctx); b.add(" %s ", name);
    if (view) b.add("view-> ");
    DescRes(b, res);
    if (isScene) b.add(" [SCENE DEPTH%s]", snapped ? ", snapshotted before discard" : "");
    TraceEmit(b);
}
// v0.5.3: the game discards the scene depth right after each eye pass; snapshot it before the discard runs.
// Returns true when `res` is the scene depth; `snapped` = a snapshot was taken by this call.
// v0.6.2: the caller passes only calls on the game context (IsGameCtx1).
bool OnDepthDiscard(ID3D11DeviceContext1* ctx, ID3D11Resource* res, bool* snapped) {
    *snapped = false;
    if (t_inDlaa || !res || !g_sceneDepth || res != (ID3D11Resource*)g_sceneDepth) return false;
    ++g_cDiscardSeen;
    if (g_passSeq <= g_fifoHead) return true;                       // no unconsumed pass
    PassSlot& slot = g_ring[(g_passSeq - 1) % kRing];
    slot.discarded = true;
#ifdef WITH_DLAA
    if (!slot.snap && g_dlaaOn.load(std::memory_order_relaxed) && !g_jitterOnly.load(std::memory_order_relaxed) &&
        !g_passive.load(std::memory_order_relaxed)) {
        t_inDlaa = true;
        const bool ok = slot.twin.Snapshot(ctx, g_sceneDepth, GpuTimingOn());
        t_inDlaa = false;
        if (ok) {
            slot.snap = true; *snapped = true; ++g_cSnapAtDiscard;
            if (g_snapCount++ == 0)
                Log("depth snapshot: captured for pass s=%llu at the scene depth discard (first occurrence)",
                    (unsigned long long)slot.s);
        }
    }
#endif
    SpanOnDiscard(ctx);                                             // v0.6.2: gbuf span ends (snapshot included)
    return true;
}
// v0.6.2: calls on any context other than the game's pass straight through (only the F11 trace sees them).
void STDMETHODCALLTYPE hkDiscardResource(ID3D11DeviceContext1* ctx, ID3D11Resource* res) {
    bool snapped = false;
    const bool scene = IsGameCtx1(ctx) && OnDepthDiscard(ctx, res, &snapped);
#ifdef WITH_DLAA
    if (g_pvCur >= 0 && IsGameCtx1(ctx)) PreviewOnDiscard(ctx, res);   // v0.7.8 profile-screen preview depth
#endif
    TraceDiscard(ctx, "DiscardResource", res, nullptr, scene, snapped);
    oDiscardResource(ctx, res);
}
void STDMETHODCALLTYPE hkDiscardView(ID3D11DeviceContext1* ctx, ID3D11View* view) {
    const bool game = IsGameCtx1(ctx);
    if (!game && !TraceOn()) { oDiscardView(ctx, view); return; }
    ID3D11Resource* r = nullptr;
    if (view) view->GetResource(&r);
    bool snapped = false;
    const bool scene = game && OnDepthDiscard(ctx, r, &snapped);
#ifdef WITH_DLAA
    if (game && g_pvCur >= 0) PreviewOnDiscard(ctx, r);                // v0.7.8 profile-screen preview depth
#endif
    TraceDiscard(ctx, "DiscardView", r, view, scene, snapped);
    if (r) r->Release();
    oDiscardView(ctx, view);
}
void STDMETHODCALLTYPE hkDiscardView1(ID3D11DeviceContext1* ctx, ID3D11View* view, const D3D11_RECT* rects, UINT n) {
    const bool game = IsGameCtx1(ctx);
    if (!game && !TraceOn()) { oDiscardView1(ctx, view, rects, n); return; }
    ID3D11Resource* r = nullptr;
    if (view) view->GetResource(&r);
    bool snapped = false;
    const bool scene = game && OnDepthDiscard(ctx, r, &snapped);
#ifdef WITH_DLAA
    if (game && g_pvCur >= 0) PreviewOnDiscard(ctx, r);                // v0.7.8 profile-screen preview depth
#endif
    TraceDiscard(ctx, "DiscardView1", r, view, scene, snapped);
    if (r) r->Release();
    oDiscardView1(ctx, view, rects, n);
}

// Ends the trace: flush the aggregate, write the file (outside the lock).
void TraceFinish() {
    std::vector<std::string> lines;
    uint64_t dropped;
    {
        std::lock_guard<std::mutex> lk(g_traceMx);
        TraceFlushAggLocked();
        g_traceState.store(0);
        lines.swap(g_traceLines);
        dropped = g_traceDropped;
    }
    wchar_t path[MAX_PATH];
    if (!PathNextToDll(L"dlaa_trace.txt", path)) { Log("trace: cannot resolve DLL path"); return; }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) { Log("trace: cannot open dlaa_trace.txt for writing"); return; }
    char hdr[640];
    snprintf(hdr, sizeof(hdr), "# dlaa_trace v0.4.3  trigger=%s  startedAtPresent=#%llu  frames=%d  events=%llu  dropped=%llu (cap %u)\n"
                               "# [index] f=<frame since start> <IMM|FGN|DEF>@<context ptr> <event>; same ptr = same texture\n"
                               "# IMM = game render context, FGN = other immediate context (not tracked, v0.6.2), DEF = deferred\n",
             g_traceCause, (unsigned long long)g_traceStartPresent, g_traceFrame,
             (unsigned long long)lines.size(), (unsigned long long)dropped, (unsigned)kTraceMaxLines);
    fputs(hdr, f);
    for (const std::string& s : lines) { fwrite(s.data(), 1, s.size(), f); fputc('\n', f); }
    if (dropped) fprintf(f, "# TRUNCATED: %llu further events dropped\n", (unsigned long long)dropped);
    fclose(f);
    Log("trace: wrote dlaa_trace.txt (%llu lines%s)", (unsigned long long)lines.size(), dropped ? ", TRUNCATED" : "");
}

// Called from hkPresent (top-level only, render thread; v0.8.1 present layer: the game-thread boundary, sc = nullptr).
// `n` = this Present's number. The trace key (v0.6.5
// Ctrl+F11) is read in hkPresent's unified hotkey block and sets g_traceRequest, consumed below the same frame.
void TraceAtPresent(IDXGISwapChain* sc, uint64_t n) {
    const int st = g_traceState.load();
    if (st == 1) {                               // armed: recording starts at THIS Present
        {
            std::lock_guard<std::mutex> lk(g_traceMx);
            g_traceLines.clear(); g_traceLines.reserve(4096);
            g_traceIdx = 0; g_traceDropped = 0; g_traceFrame = 0; g_agg = TraceAgg();
            g_traceStartPresent = n;
            g_traceState.store(2);
        }
        Log("trace: started (Present #%llu, %s)", (unsigned long long)n, g_traceCause);
        LB b; b.add("IMM Present (trace start boundary, #%llu)", (unsigned long long)n);
        TraceEmit(b);
    } else if (st == 2) {
        LB b;
        if (sc) b.add("IMM Present #%llu ending frame %d", (unsigned long long)n, g_traceFrame);
        else    b.add("IMM frame boundary #%llu ending frame %d (present layer: frame-end rule / scene clear / idle, "
                      "the game's own Present is not hooked)", (unsigned long long)n, g_traceFrame);   // v0.8.1
        ID3D11Texture2D* bb = nullptr;
        if (sc && SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
            b.add(" backbuffer="); DescRes(b, bb); bb->Release();
        }
        TraceEmit(b);
        if (++g_traceFrame >= 12) TraceFinish();   // 12 frames: enough to see the VR eye-texture rings
    }
    if (g_traceAutoFrame > 0 && !g_traceAutoDone && n >= (uint64_t)g_traceAutoFrame) {
        g_traceAutoDone = true;
        if (g_traceState.load() == 0) { g_traceCause = "auto"; g_traceState.store(1); }
    }
    if (g_traceRequest.exchange(false) && g_traceState.load() == 0) {
        g_traceCause = "trace key";
        g_traceState.store(1);
    }
}

// OMSetRenderTargets helper: remember the main G-buffer depth.
// `res` is the DSV's resource; borrowed (caller releases).
void InspectDepth(ID3D11Resource* res) {
    if (res == g_sceneDepth) return;                           // same as remembered
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex) {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        // The texture may be typeless with a D32_FLOAT_S8X24_UINT DSV on top.
        const bool fmtOk = d.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
                           d.Format == DXGI_FORMAT_R32G8X24_TYPELESS;
        if (fmtOk && d.Width >= 1024 && d.SampleDesc.Count == 1) {
            if (g_sceneDepth) g_sceneDepth->Release();
            g_sceneDepth = tex; tex = nullptr;                  // keep our ref
            g_sceneW = d.Width; g_sceneH = d.Height;
            if (g_depthChanges++ < 10)
                Log("scene depth G-buffer pass: %ux%u fmt=%d (change #%llu)",
                    d.Width, d.Height, (int)d.Format, (unsigned long long)g_depthChanges);
        }
        if (tex) tex->Release();
    }
}

// ---- jitter via viewport offset ----------------------------------------------
// The game's last requested viewports, and what we last really issued.
D3D11_VIEWPORT g_gameVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
UINT           g_gameVpN      = 0;
bool           g_jitterPass   = false;     // current OM binding is a jitter pass
bool           g_vpShifted    = false;     // last issued viewports carried a shift
float          g_vpShiftX = 0.0f, g_vpShiftY = 0.0f;   // ...and which one
int            g_passLogged   = 0;         // bit0 = G-buffer logged, bit1 = forward logged

// Jitter only while DLAA is live and the jitter is enabled in dlaa.ini (v0.6.2: and not in passive mode).
bool JitterLive() {
#ifdef WITH_DLAA
    // v0.7.8: no viewport jitter before the shader warm-up is done (no DLAA unit can run yet, a jittered raw frame would
    // just shimmer). Only the first seconds after the DLL load.
    if (!ShaderCache::Done()) return false;
#endif
    return g_dlaaOn.load(std::memory_order_relaxed) && g_jitterEnabled && !g_passive.load(std::memory_order_relaxed);
}

// ---- v0.6.2 calls on contexts other than the game's (any thread): counted + sanity-logged, never tracked -------
// Nothing here touches the render-thread state except reading g_sceneW/H for a one-shot log line.
std::atomic<bool> g_fgnGbufLogged{false};
std::atomic<bool> g_fgnBlitLogged{false};
// Counts the call when `ctx` is an immediate context and the game context is known. Returns that condition.
inline bool CountForeign(ID3D11DeviceContext* ctx) {
    if (!g_gameCtx.load(std::memory_order_relaxed) || ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;
    g_foreignCtx.fetch_add(1, std::memory_order_relaxed);
    return true;
}
void PlNoteEvidence(ID3D11DeviceContext* ctx, int kind, UINT dw, UINT dh);   // v0.8.1, defined with the game context
void MaybeEarlyPrewarm(ID3D11DeviceContext* ctx, void* caller);                // v0.8.1 round 3, ditto
// OMSetRenderTargets on a foreign context: a bind that passes the scene-depth test (4 RTVs + D32S8 >= 1024 wide,
// single-sample = InspectDepth) is logged once and NOT adopted on its own. v0.8.1: until a context was adopted from a
// present layer, such binds -- and the profile-screen preview pass shape (1 RTV R11G11B10_FLOAT + a D32S8 DSV of the
// RT's size, see PreviewOnBind) -- are present-layer evidence (PlNoteEvidence; adoption needs many of them on ONE
// context plus the presenting side's conditions).
void NoteForeignOM(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) {
    if (!CountForeign(ctx) || !dsv || (n != 4 && n != 1)) return;
    // Evidence is collected until an adoption, and only while the game context has rendered no scene pass: once it
    // does (the normal case) no adoption is possible, nothing to collect. g_passSeq: racy read, a 0-test only.
    const bool collect = !g_plAdopted.load(std::memory_order_relaxed) && g_passSeq == 0;
    if (!collect && (n != 4 || g_fgnGbufLogged.load(std::memory_order_relaxed))) return;   // v0.6.2 behaviour
    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    if (!res) return;
    D3D11_TEXTURE2D_DESC d{};
    bool depthOk = false;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex) {
        tex->GetDesc(&d);
        tex->Release();
        const bool fmtOk = d.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || d.Format == DXGI_FORMAT_R32G8X24_TYPELESS;
        depthOk = fmtOk && d.Width >= 1024 && d.SampleDesc.Count == 1;
    }
    res->Release();
    if (!depthOk) return;
    int kind = 0;                                            // 1 = G-buffer, 2 = preview pass shape
    if (n == 4) {
        kind = 1;
    } else if (collect && rtvs && rtvs[0]) {
        ID3D11Resource* rres = nullptr;
        rtvs[0]->GetResource(&rres);
        if (rres) {
            ID3D11Texture2D* rt = nullptr;
            if (SUCCEEDED(rres->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&rt)) && rt) {
                D3D11_TEXTURE2D_DESC rd{};
                rt->GetDesc(&rd);
                rt->Release();
                if (rd.Format == DXGI_FORMAT_R11G11B10_FLOAT && rd.SampleDesc.Count == 1 && rd.Width == d.Width &&
                    rd.Height == d.Height) kind = 2;
            }
            rres->Release();
        }
    }
    if (!kind) return;
    if (kind == 1 && !g_fgnGbufLogged.exchange(true))
        Log("G-buffer bind on a non-game context %p (game %p): depth %ux%u fmt=%d tid=%lu -- not adopted%s",
            (void*)ctx, (void*)g_gameCtx.load(), d.Width, d.Height, (int)d.Format, GetCurrentThreadId(),
            collect ? " (yet: v0.8.1 collects present-layer evidence, see the 'present layer' lines)" : "");
    if (collect) PlNoteEvidence(ctx, kind, d.Width, d.Height);
}
// Draw on a foreign context: a 3/4-vertex draw whose PS SRV0 is an R8G8B8A8 texture at the scene dims (= what
// our blit trigger looks for) is logged once (our DLAA only runs on game-context blits).
void NoteForeignDraw(ID3D11DeviceContext* ctx, UINT verts) {
    if (!CountForeign(ctx) || verts - 3u > 1u || g_fgnBlitLogged.load(std::memory_order_relaxed)) return;
    const UINT sw = g_sceneW, sh = g_sceneH;                 // racy read, log only
    if (!sw) return;
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    srv->Release();
    if (!res) return;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex) {
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        tex->Release();
        const bool fmtOk = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || d.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                           d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
        if (fmtOk && d.Width == sw && d.Height == sh && !g_fgnBlitLogged.exchange(true))
            Log("eye-blit-like draw on a non-game context %p (game %p): %u verts, PS SRV0 %ux%u fmt=%d tid=%lu -- ignored",
                (void*)ctx, (void*)g_gameCtx.load(), verts, d.Width, d.Height, (int)d.Format, GetCurrentThreadId());
    }
    res->Release();
}
// OM / RS on the game context from a thread other than the blit thread (v0.6.1's "foreign-thread" signal, kept
// as a separate counter: if anomalies persist with foreign-ctx=0, the game context itself is shared).
inline void CountGameOtherThread() {
    const unsigned long rt = g_renderTid.load(std::memory_order_relaxed);
    if (rt && GetCurrentThreadId() != rt) g_gameOtherThread.fetch_add(1, std::memory_order_relaxed);
}

// Issues the right viewports for the current state: the game's unmodified ones,
// or the first one shifted by (g_jx,g_jy) in a jitter pass at scene dims. Does
// nothing if that is exactly what was last issued (no call spam).
void ReconcileViewports(ID3D11DeviceContext* ctx, bool gameJustSet) {
    if (g_gameVpN == 0) return;
    const bool worldShift = g_jitterPass && JitterLive() && !WorldJitterHeld() && g_sceneW &&   // v0.7.9: see g_eyeBlitEver
                            (UINT)g_gameVp[0].Width == g_sceneW && (UINT)g_gameVp[0].Height == g_sceneH;
    // v0.7.8: the profile-screen preview pass shifts viewport 0 too (when its size is the preview RT's size, g_pvW x
    // g_pvH), by the frame's own jitter (g_pvJx / g_pvJy: no G-buffer pass advances g_jx / g_jy on that screen).
#ifdef WITH_DLAA
    const bool pvShift = !worldShift && g_previewPass && JitterLive() && g_pvW &&
                         (UINT)g_gameVp[0].Width == g_pvW && (UINT)g_gameVp[0].Height == g_pvH;
    // Round 6: a tile pass is shrunk by a * h into the picture -- its shift is scaled up so the PICTURE moves by exactly
    // (g_pvJx, g_pvJy), the offset NGX is told (PvShiftScale; 1 for an untiled pass).
    float pvKx = 1.0f, pvKy = 1.0f;
    if (pvShift) PvShiftScale(g_pvCur, &pvKx, &pvKy);
    const float sx = pvShift ? g_pvJx * pvKx : g_jx, sy = pvShift ? g_pvJy * pvKy : g_jy;
    if (pvShift && g_pvCur >= 0) { PvSlot& ss = g_pv[g_pvCur]; ss.shX = sx; ss.shY = sy; ss.shOn = true; }   // round 4
    // v0.7.8 preview capture: record the decision for the current preview pass (proof the 4 passes get the same shift).
    if ((g_pvCapActive || g_pvSmActive) && g_previewPass && g_pvCur >= 0) {
        PvSlot& cs = g_pv[g_pvCur];
        if (gameJustSet) cs.capVp = g_gameVp[0];         // v0.8.1 round 5 seam capture: the game's own viewport 0
        if (g_pvW && (UINT)g_gameVp[0].Width == g_pvW && (UINT)g_gameVp[0].Height == g_pvH) {
            if (pvShift) { cs.capShX = sx; cs.capShY = sy; ++cs.capShN; } else ++cs.capUnshN;
        }
    }
#else
    const bool pvShift = false;
    const float sx = g_jx, sy = g_jy;
#endif
    const bool shift = worldShift || pvShift;
    if (!gameJustSet && shift == g_vpShifted &&
        (!shift || (g_vpShiftX == sx && g_vpShiftY == sy))) return;
    if (shift) {
        D3D11_VIEWPORT v[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        memcpy(v, g_gameVp, g_gameVpN * sizeof(D3D11_VIEWPORT));
        v[0].TopLeftX += sx;
        v[0].TopLeftY += sy;
        oRSSetViewports(ctx, g_gameVpN, v);
    } else {
        oRSSetViewports(ctx, g_gameVpN, g_gameVp);
    }
    g_vpShifted = shift; g_vpShiftX = sx; g_vpShiftY = sy;
}

void STDMETHODCALLTYPE hkRSSetViewports(ID3D11DeviceContext* ctx, UINT n, const D3D11_VIEWPORT* vps) {
    TraceVp(ctx, n, vps);
    if (!IsGameCtx(ctx)) {                         // v0.6.2: other contexts never touch g_gameVp / ReconcileViewports
        CountForeign(ctx);
        oRSSetViewports(ctx, n, vps);
        return;
    }
    // v0.8.1: 0 viewports on the game context. The v0.8.1 test logs show the D3D11 runtime issues it inside Present
    // (caller d3d11.dll, also on the layer's thread at shutdown): counted + its caller logged once, NEVER a frame boundary.
    if (n == 0 && !t_inDlaa) OnFrameStartMarker(ctx, _ReturnAddress());
    if (t_inDlaa || !vps || n == 0) {
        oRSSetViewports(ctx, n, vps);
        return;
    }
    CountGameOtherThread();                        // v0.6.2 flight recorder
    if (n >D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
        n = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    memcpy(g_gameVp, vps, n * sizeof(D3D11_VIEWPORT));
    g_gameVpN = n;
    ReconcileViewports(ctx, true);
}

void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D11DeviceContext* ctx, UINT n,
                                            ID3D11RenderTargetView* const* rtvs,
                                            ID3D11DepthStencilView* dsv) {
    TraceOM(ctx, n, rtvs, dsv);
    if (!IsGameCtx(ctx)) {                         // v0.6.2: other contexts never touch g_gbufPass / g_jitterPass
        MaybeEarlyPrewarm(ctx, _ReturnAddress());   // v0.8.1: NGX pre-warm on the game's thread before the adoption
        NoteForeignOM(ctx, n, rtvs, dsv);
        oOMSetRenderTargets(ctx, n, rtvs, dsv);
        return;
    }
    if (t_inDlaa) {
        oOMSetRenderTargets(ctx, n, rtvs, dsv);
        return;
    }
    // v0.8.1: the frame-end rule (both modes; under a present layer it runs the game-thread frame boundary right here,
    // before this bind -- the previous frame's last target is still bound), then the IDLE fallback boundary (no rule
    // frame end for 1 s and no boundary of any kind for 30 ms). LayerFrameBoundary returns at once unless that holds.
    FrameEndOnBind(ctx, n, rtvs);
    if (g_plActive.load(std::memory_order_relaxed)) LayerFrameBoundary(ctx, kPlbIdle);
    CountGameOtherThread();                        // v0.6.2 flight recorder
    // Jitter-pass test: the bound DSV is the scene depth AND either
    //  (a) 4 RTVs (G-buffer), or
    //  (b) exactly 1 RTV that is R16G16B16A16_FLOAT at scene dims (forward /
    //      transparent pass). The lighting pass (2 RTVs) and every shadow /
    //      mirror / probe pass (other DSVs) are never jittered.
    // InspectDepth runs first so a scene depth discovered in this very call counts.
    bool pass = false;
    if (dsv) {
        ID3D11Resource* dres = nullptr;
        dsv->GetResource(&dres);
        if (dres) {
            if (n == 4) InspectDepth(dres);
            if (dres == g_sceneDepth) {
                if (n == 4) {
                    pass = true;
                    if (!(g_passLogged & 1)) { g_passLogged |= 1; Log("jitter pass: G-buffer (4 RTVs + scene DSV) detected on the game context %p", (void*)ctx); }
                } else if (n == 1 && rtvs && rtvs[0]) {
                    ID3D11Resource* rres = nullptr;
                    rtvs[0]->GetResource(&rres);
                    if (rres) {
                        ID3D11Texture2D* rt = nullptr;
                        if (SUCCEEDED(rres->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&rt)) && rt) {
                            D3D11_TEXTURE2D_DESC d{};
                            rt->GetDesc(&d);
                            pass = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
                                   d.Width == g_sceneW && d.Height == g_sceneH;
                            if (pass && !(g_passLogged & 2)) { g_passLogged |= 2; Log("jitter pass: forward (1 RTV RGBA16F + scene DSV) detected"); }
                            if (pass) g_fwdColorTex = (void*)rres;   // v0.8.0 HDR blit rule: the forward colour (identity)
                            rt->Release();
                        }
                        rres->Release();
                    }
                }
            }
            dres->Release();
        }
    }
    oOMSetRenderTargets(ctx, n, rtvs, dsv);
    g_jitterPass = pass;
    const bool wasGbuf = g_gbufPass;
    g_gbufPass = pass && n == 4;
#ifdef WITH_DLAA
    PreviewOnBind(n, rtvs, dsv, g_gbufPass);       // v0.7.8 profile-screen preview pass (flat + VR), sets g_previewPass
    if (g_ptPendingMask) PreviewOnBindTargets(ctx, n, rtvs);   // v0.7.8 round 4 trigger (b) + bound-target tracking
#endif
    if (g_gbufPass) {
        // The very first pass: its clear ran before the scene depth was known, so start it here.
        // v0.6.2: also the first pass after a game-context change (g_needFirstPass, was g_passSeq == 0).
        if (!wasGbuf && g_needFirstPass) {
            g_needFirstPass = false;
            StartPass(ctx, false);
            SpanBeginPending(ctx);                    // no clear follows: the spans start at this bind
        }
        g_depthDirty = true;                          // scene depth now holds G-buffer content
        ++g_ring[(g_passSeq - 1) % kRing].gbufBinds;  // v0.6.1 (g_passSeq >= 1 here): every qualifying bind
    }
    ReconcileViewports(ctx, false);       // no-op unless the shift state changed
}


#ifdef WITH_DLAA
// ---- F9 self-test: lossless frame dumps for judging the NGX jitter sign -----------
bool PathNextToDll(const wchar_t* name, wchar_t* path);   // defined below

struct TestMode { const char* name; bool dlaaOn; bool jitOnly; int sx, sy; };
const TestMode kModes[] = {
    { "off",        false, false,  0,  0 },
    { "sgn_m1_m1",  true,  false, -1, -1 },
    { "sgn_p1_p1",  true,  false, +1, +1 },
    { "sgn_p1_m1",  true,  false, +1, -1 },
    { "sgn_m1_p1",  true,  false, -1, +1 },
    { "jitonly",    true,  true,   0,  0 },
};
constexpr int kNumModes = (int)(sizeof(kModes) / sizeof(kModes[0]));
constexpr int kSettleFrames = 90;
constexpr int kDumpFrames   = 4;

bool  g_testRunning = false;
int   g_testMode = 0, g_testSettle = 0, g_testDumped = 0;
bool  g_savedOn = false, g_savedJo = false; int g_savedSx = -1, g_savedSy = -1;
wchar_t g_testDir[MAX_PATH] = {};
ID3D11Texture2D* g_staging = nullptr;
UINT g_stagW = 0, g_stagH = 0;
DXGI_FORMAT g_stagFmt = DXGI_FORMAT_UNKNOWN;   // v0.8.0: the staging twin must match the format too (8-bit vs RGBA16F)

void ApplyTestMode(int i) {
    const TestMode& m = kModes[i];
    g_dlaaOn = m.dlaaOn;
    g_jitterOnly = m.jitOnly;
    if (m.dlaaOn && !m.jitOnly) { g_signX = m.sx; g_signY = m.sy; }
    if (m.dlaaOn) g_resetNext = true;
    g_testSettle = kSettleFrames;
    g_testDumped = 0;
    Log("self-test: mode %d/%d '%s' (dlaa=%d jitter-only=%d sign=%+d,%+d), settling %d frames",
        i + 1, kNumModes, m.name, (int)m.dlaaOn, (int)m.jitOnly, g_signX, g_signY, kSettleFrames);
}

// Writes an uncompressed 24-bit bottom-up BMP from RGBA rows (bytes as-is, alpha dropped).
bool WriteBmp(const wchar_t* path, const uint8_t* src, UINT pitch, UINT w, UINT h) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) return false;
    const UINT rowBytes = (w * 3 + 3) & ~3u;
    const uint32_t imgSize = rowBytes * h, off = 54, fileSize = off + imgSize;
    uint8_t hdr[54] = {};
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &fileSize, 4); memcpy(hdr + 10, &off, 4);
    const uint32_t dib = 40; memcpy(hdr + 14, &dib, 4);
    const int32_t iw = (int32_t)w, ih = (int32_t)h; memcpy(hdr + 18, &iw, 4); memcpy(hdr + 22, &ih, 4);
    const uint16_t planes = 1, bpp = 24; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &imgSize, 4);
    bool ok = fwrite(hdr, 1, 54, f) == 54;
    uint8_t* row = (uint8_t*)calloc(1, rowBytes);
    if (!row) { fclose(f); return false; }
    for (UINT y = 0; ok && y < h; ++y) {
        const uint8_t* s = src + (size_t)(h - 1 - y) * pitch;
        for (UINT x = 0; x < w; ++x) {
            row[x * 3 + 0] = s[x * 4 + 2];
            row[x * 3 + 1] = s[x * 4 + 1];
            row[x * 3 + 2] = s[x * 4 + 0];
        }
        ok = fwrite(row, 1, rowBytes, f) == rowBytes;
    }
    free(row);
    fclose(f);
    return ok;
}

float HalfToFloat(uint16_t h);   // F10 snapshot section below
bool IsRgbaBgraFamily(DXGI_FORMAT f);   // blit section below

// v0.8.0: RGBA16F (HDR) rows -> 8-bit RGBA for the BMP writers. Linear HDR does not fit 8 bits, so every channel is
// tone-compressed for VIEWING only: x / (1 + x) (negative -> 0), then sRGB-encoded (4096-entry LUT, built once). The
// same mapping for every dump, so self-test / snapshot frames stay comparable with each other (not with 8-bit dumps).
bool WriteBmpRgba16f(const wchar_t* path, const uint8_t* src, UINT pitch, UINT w, UINT h) {
    static uint8_t lut[4096];
    static bool lutReady = false;
    if (!lutReady) {
        for (int i = 0; i < 4096; ++i) {
            const double y = (double)i / 4095.0;
            const double s = y <= 0.0031308 ? 12.92 * y : 1.055 * std::pow(y, 1.0 / 2.4) - 0.055;
            lut[i] = (uint8_t)(s <= 0.0 ? 0 : (s >= 1.0 ? 255 : (int)(s * 255.0 + 0.5)));
        }
        lutReady = true;
        Log("BMP dumps of RGBA16F (HDR) textures are tone-compressed for viewing: x/(1+x) per channel, sRGB-encoded");
    }
    uint8_t* rgba = (uint8_t*)malloc((size_t)w * h * 4);
    if (!rgba) return false;
    for (UINT y = 0; y < h; ++y) {
        const uint16_t* s = (const uint16_t*)(src + (size_t)y * pitch);
        uint8_t* o = rgba + (size_t)y * w * 4;
        for (UINT x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                float v = HalfToFloat(s[x * 4 + c]);
                v = v > 0.0f ? v / (1.0f + v) : 0.0f;          // NaN -> 0 as well (the compare is false)
                o[x * 4 + c] = lut[(int)(v * 4095.0f + 0.5f) & 4095];
            }
            o[x * 4 + 3] = 255;
        }
    }
    const bool ok = WriteBmp(path, rgba, w * 4, w, h);
    free(rgba);
    return ok;
}

bool DumpFrame(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const wchar_t* path) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    // v0.8.0: 8-bit RGBA family (bytes as-is, the v0.7.x dump) or RGBA16F (tone-compressed, see above); anything else
    // would write garbage -> refused with a log line.
    const bool f16 = d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!f16 && !IsRgbaBgraFamily(d.Format)) {
        Log("BMP dump skipped: texture %ux%u fmt=%d is neither 8-bit RGBA / BGRA nor RGBA16F", d.Width, d.Height, (int)d.Format);
        return false;
    }
    // (8-bit: the staging twin is reused across the R8G8B8A8 views of one typeless group exactly as before; only an
    // 8-bit <-> RGBA16F change recreates it.)
    if (!g_staging || g_stagW != d.Width || g_stagH != d.Height || (g_stagFmt == DXGI_FORMAT_R16G16B16A16_FLOAT) != f16) {
        if (g_staging) { g_staging->Release(); g_staging = nullptr; }
        ID3D11Device* dev = nullptr;
        tex->GetDevice(&dev);
        if (!dev) return false;
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MipLevels = 1; sd.ArraySize = 1;
        const HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &g_staging);
        dev->Release();
        if (FAILED(hr) || !g_staging) { Log("self-test: staging create failed hr=0x%lx", (unsigned long)hr); g_staging = nullptr; return false; }
        g_stagW = d.Width; g_stagH = d.Height; g_stagFmt = d.Format;
    }
    ctx->CopyResource(g_staging, tex);
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &mp))) { Log("self-test: Map failed"); return false; }
    const bool ok = f16 ? WriteBmpRgba16f(path, (const uint8_t*)mp.pData, mp.RowPitch, d.Width, d.Height)   // v0.8.0
                        : WriteBmp(path, (const uint8_t*)mp.pData, mp.RowPitch, d.Width, d.Height);
    ctx->Unmap(g_staging, 0);
    return ok;
}

void FinishSelfTest() {
    g_dlaaOn = g_savedOn; g_jitterOnly = g_savedJo; g_signX = g_savedSx; g_signY = g_savedSy;
    g_resetNext = true;
    g_testRunning = false;
    Log("self-test done (restored dlaa=%d jitter-only=%d sign=%+d,%+d)",
        (int)g_savedOn, (int)g_savedJo, g_signX, g_signY);
}

// Called once per frame from the tonemap-detection point, after DLAA ran / was skipped.
// `phase`, `vx`, `vy`, `njx`, `njy` are the values of the frame just rendered.
void SelfTestStep(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, int phase,
                  float vx, float vy, float njx, float njy) {
    if (!g_testRunning) {
        if (!g_selfTestRequest.exchange(false)) return;
        if (g_gameDeviceChanged) { Log("self-test refused: the game render device changed (DLAA stays OFF)"); return; }
        g_savedOn = g_dlaaOn.load(); g_savedJo = g_jitterOnly.load();
        g_savedSx = g_signX; g_savedSy = g_signY;
        if (!PathNextToDll(L"dlaa_selftest\\", g_testDir)) { Log("self-test: cannot resolve DLL path"); return; }
        CreateDirectoryW(g_testDir, nullptr);
        D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
        wchar_t ip[MAX_PATH]; wcscpy_s(ip, g_testDir); wcscat_s(ip, L"info.txt");
        FILE* f = nullptr;
        if (_wfopen_s(&f, ip, L"w") == 0 && f) {
            SYSTEMTIME t; GetLocalTime(&t);
            fprintf(f, "dims=%ux%u fmt=%d\ntimestamp=%04d-%02d-%02d %02d:%02d:%02d\n"
                       "settle_frames=%d dump_frames=%d phases=%d jitter_enabled=%d\n"
                       "file | phase | viewport shift (x,y) | NGX jitter passed (x,y)\n",
                    d.Width, d.Height, (int)d.Format, t.wYear, t.wMonth, t.wDay,
                    t.wHour, t.wMinute, t.wSecond, kSettleFrames, kDumpFrames, g_phases, (int)g_jitterEnabled);
            fclose(f);
        }
        g_testRunning = true; g_testMode = 0;
        Log("self-test: started, dumping to dlaa_selftest\\ (%ux%u); saved state dlaa=%d jo=%d sign=%+d,%+d",
            d.Width, d.Height, (int)g_savedOn, (int)g_savedJo, g_savedSx, g_savedSy);
        ApplyTestMode(0);
        return;
    }
    if (g_testSettle > 0) { --g_testSettle; return; }

    const TestMode& m = kModes[g_testMode];
    char nm[64];
    snprintf(nm, sizeof(nm), "%s_%d.bmp", m.name, g_testDumped);
    wchar_t wn[64]; MultiByteToWideChar(CP_UTF8, 0, nm, -1, wn, 64);
    wchar_t path[MAX_PATH]; wcscpy_s(path, g_testDir); wcscat_s(path, wn);
    const bool ok = DumpFrame(ctx, tex, path);
    Log("self-test: %s %s", ok ? "wrote" : "FAILED to write", nm);
    // jitter actually applied this frame: none in OFF; NGX value only when DLAA evaluated
    const bool jit = m.dlaaOn && g_jitterEnabled;
    wchar_t ip[MAX_PATH]; wcscpy_s(ip, g_testDir); wcscat_s(ip, L"info.txt");
    FILE* f = nullptr;
    if (_wfopen_s(&f, ip, L"a") == 0 && f) {
        if (!jit) fprintf(f, "%s | phase=n/a | shift=none | ngx=none\n", nm);
        else if (m.jitOnly) fprintf(f, "%s | phase=%d | shift=(%+.4f,%+.4f) | ngx=not passed (DLAA skipped)\n", nm, phase, vx, vy);
        else fprintf(f, "%s | phase=%d | shift=(%+.4f,%+.4f) | ngx=(%+.4f,%+.4f)\n", nm, phase, vx, vy, njx, njy);
        fclose(f);
    }
    if (++g_testDumped >= kDumpFrames) {
        if (++g_testMode >= kNumModes) FinishSelfTest();
        else ApplyTestMode(g_testMode);
    }
}

// ---- F10 snapshot: exactly what NGX received, lossless, 2 consecutive frames ------------
struct SnapInfo {
    uint64_t frame; int phase; float vx, vy, njx, njy; bool reset, mvOn; bool depthSnap;
};

float HalfToFloat(uint16_t h) {
    const uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF, u;
    if (e == 0) {
        if (m == 0) u = sign;
        else { e = 1; while (!(m & 0x400)) { m <<= 1; --e; } m &= 0x3FF; u = sign | ((e + 112) << 23) | (m << 13); }
    } else if (e == 31) u = sign | 0x7F800000u | (m << 13);
    else u = sign | ((e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &u, 4); return f;
}

// Reads `tex` (R32_FLOAT or R16G16_FLOAT) back and writes uint32 w, h + w*h*comps float32.
bool WriteFloatBin(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, int comps, const wchar_t* path) {
    D3D11_TEXTURE2D_DESC d{}; tex->GetDesc(&d);
    ID3D11Device* dev = nullptr; tex->GetDevice(&dev);
    if (!dev) return false;
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.MiscFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MipLevels = 1; sd.ArraySize = 1;
    ID3D11Texture2D* st = nullptr;
    const HRESULT hr = dev->CreateTexture2D(&sd, nullptr, &st);
    dev->Release();
    if (FAILED(hr) || !st) return false;
    ctx->CopyResource(st, tex);
    D3D11_MAPPED_SUBRESOURCE mp{};
    bool ok = SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &mp));
    if (ok) {
        FILE* f = nullptr;
        ok = _wfopen_s(&f, path, L"wb") == 0 && f;
        if (ok) {
            const uint32_t hdr[2] = { d.Width, d.Height };
            ok = fwrite(hdr, 4, 2, f) == 2;
            float* row = (float*)malloc((size_t)d.Width * comps * sizeof(float));
            ok = ok && row;
            for (UINT y = 0; ok && y < d.Height; ++y) {
                const uint8_t* s = (const uint8_t*)mp.pData + (size_t)y * mp.RowPitch;
                if (d.Format == DXGI_FORMAT_R32_FLOAT) memcpy(row, s, (size_t)d.Width * 4);
                else for (UINT i = 0; i < d.Width * (UINT)comps; ++i) row[i] = HalfToFloat(((const uint16_t*)s)[i]);
                ok = fwrite(row, sizeof(float) * comps, d.Width, f) == d.Width;
            }
            free(row);
            fclose(f);
        }
        ctx->Unmap(st, 0);
    }
    st->Release();
    return ok;
}

int g_snapLeft = 0, g_snapIdx = 0;           // blits still to dump / next file index
wchar_t g_snapDir[MAX_PATH] = {};

// Called right after SceneDlaa::Run succeeded (same place as SelfTestStep). v0.5.2: per blit. A snapshot is
// 2 frames worth of blits (VR 4, flat 2); files are numbered by blit index, VR suffix _e<eye identity>.
void SnapshotStep(ID3D11DeviceContext* ctx, int eye, ID3D11Texture2D* blitTex, const SnapInfo& si) {
    if (g_snapLeft == 0) {
        if (!g_snapRequest.exchange(false)) return;
        if (!PathNextToDll(L"dlaa_snap\\", g_snapDir)) { Log("snapshot: cannot resolve DLL path"); return; }
        CreateDirectoryW(g_snapDir, nullptr);
        g_snapLeft = 2 * (g_vrMode ? kMaxEyes : 1); g_snapIdx = 0;
    }
    SceneDlaa& dl = g_dlaa[eye];
    const int i = g_snapIdx;
    char sfx[8] = "";
    if (g_vrMode) snprintf(sfx, sizeof(sfx), "_e%d", eye);
    wchar_t path[MAX_PATH]; char nm[96];
    auto P = [&](const char* base, const char* ext) {
        snprintf(nm, sizeof(nm), "%s_%d%s.%s", base, i, sfx, ext);
        wchar_t wn[96]; MultiByteToWideChar(CP_UTF8, 0, nm, -1, wn, 96);
        wcscpy_s(path, g_snapDir); wcscat_s(path, wn);
    };
    P("color_in", "bmp");
    Log("snapshot: %s %s", DumpFrame(ctx, dl.ColorInTex(), path) ? "wrote" : "FAILED to write", nm);
    P("out", "bmp");
    Log("snapshot: %s %s", DumpFrame(ctx, blitTex, path) ? "wrote" : "FAILED to write", nm);
    P("mv", "bin");
    Log("snapshot: %s %s", WriteFloatBin(ctx, dl.MvTex(), 2, path) ? "wrote" : "FAILED to write", nm);
    P("depth", "bin");
    Log("snapshot: %s %s", WriteFloatBin(ctx, dl.DepthTex(), 1, path) ? "wrote" : "FAILED to write", nm);

    float sol[CameraMv::kSolveFloats] = {};      // v0.6.0: 72 floats (R_ego, InvRow3, ego info appended)
    const bool haveSolve = si.mvOn && dl.Mv().ReadSolveBlocking(ctx, sol);
    const CameraMv::FrameStats& fs = dl.Mv().Last();
    P("info", "txt");
    FILE* f = nullptr;
    const bool ok = _wfopen_s(&f, path, L"w") == 0 && f;
    if (ok) {
        fprintf(f, "frame=%llu\neye=%d\nhalton_phase=%d of %d\nviewport_shift=(%+.5f,%+.5f)\nngx_jitter_passed=(%+.5f,%+.5f)\n"
                   "reset_flag=%d (as passed in; Run() may force reset on world-miss)\nmv_enabled=%d\n"
                   "world_pairs=%d\ncabin_pairs=%d\nworld_miss=%d\ndepth_source=%s\n",
                (unsigned long long)si.frame, eye, si.phase, g_phases, si.vx, si.vy, si.njx, si.njy,
                (int)si.reset, (int)si.mvOn, fs.pairs[0], fs.pairs[1], (int)fs.worldMiss,
                si.depthSnap ? "per-eye snapshot (taken before the scene depth clear)" : "live scene depth");
        // v0.6.4 DLAA area: color_in / mv / depth are crop-sized (the rect); out is the whole blit source.
        fprintf(f, "dlaa_area=%d%% feather=%d px\nrect_origin=(%u,%u)\nrect_size=%ux%u\nfull_size=%ux%u\n"
                   "optical_centre_uv=(%.4f,%.4f)%s\n",
                SceneDlaa::Area(), SceneDlaa::Feather(), dl.RectX(), dl.RectY(), dl.RectW(), dl.RectH(),
                dl.FullW(), dl.FullH(), (double)dl.CentreU(), (double)dl.CentreV(),
                (!g_vrMode || g_optC[eye].have) ? "" : " (not known yet: image centre)");
        // v0.7.0 DLSS upscaling: render = the scene (blit source) size, output = the blit viewport size. With
        // upscale=1 "out" is the DLSS output of the output rect (what the composite draws), not the blit source.
        fprintf(f, "upscale=%d\nrender_size=%ux%u\noutput_size=%ux%u\noutput_rect_origin=(%u,%u)\noutput_rect_size=%ux%u\n"
                   "dlss_quality=%s\nout_file=%s\n",
                (int)dl.Upscaling(), dl.FullW(), dl.FullH(), dl.OutFullW(), dl.OutFullH(),
                dl.OutRectX(), dl.OutRectY(), dl.OutRectW(), dl.OutRectH(), dl.QualityName(),
                dl.Upscaling() ? "DLSS output rect (output res, before the composite)" : "whole blit source after DLAA");
        // v0.8.0: HDR unit = RGBA16F colour (the BMPs are tone-compressed for viewing, see the log)
        fprintf(f, "hdr_unit=%d\nngx_is_hdr=%d\n", (int)dl.IsHdr(), (int)(dl.IsHdr() && SceneDlaa::HdrLinear()));
        if (haveSolve) {
            for (int m = 0; m < 2; ++m) {
                fprintf(f, "R_%s (row-major, 16 floats):\n", m == 0 ? "world" : "cabin");
                for (int r = 0; r < 4; ++r)
                    fprintf(f, "  %.7g %.7g %.7g %.7g\n", sol[m*16+r*4], sol[m*16+r*4+1], sol[m*16+r*4+2], sol[m*16+r*4+3]);
            }
            // v0.6.0 ego reprojection: solve elements 12..15 (R_ego), 16 (InvRow3), 17 (ego info)
            fprintf(f, "R_ego (row-major, 16 floats; mv_ego_origin_m=%.1f mv_ego_pixel_m=%.1f):\n",
                    (double)dl.Mv().EgoOrigin(), (double)dl.Mv().EgoPixel());
            for (int r = 0; r < 4; ++r)
                fprintf(f, "  %.7g %.7g %.7g %.7g\n", sol[48+r*4], sol[48+r*4+1], sol[48+r*4+2], sol[48+r*4+3]);
            fprintf(f, "InvRow3 (row 3 of inverse(MVP_cur) of the chosen world pair; 1/dot(InvRow3,(ndc,z,1)) = view depth m):\n"
                       "  %.7g %.7g %.7g %.7g\n", sol[64], sol[65], sol[66], sol[67]);
            fprintf(f, "ego_info: candidates=%.0f used=%.0f fallback_to_R_world=%.0f |R_ego-R_world|=%.6g\n",
                    sol[68], sol[69], sol[70], sol[71]);
        } else fprintf(f, "R_world/R_cabin/R_ego: not available (MV off or readback failed)\n");
        fclose(f);
    }
    Log("snapshot: %s %s", ok ? "wrote" : "FAILED to write", nm);
    ++g_snapIdx;
    if (--g_snapLeft == 0) Log("snapshot done (dlaa_snap dir, blit %llu was the last)", (unsigned long long)si.frame);
}
#endif

// Called by hkDraw before the game's draw. Everything acquired is released on all paths.
// v0.5.0 trigger (3/4-vertex Draw): PS SRV0 = Texture2D in the R8G8B8A8 family at EXACTLY the scene
// depth dims, and RT0 = either the DXGI backbuffer (flat) or a Texture2D in the R8G8B8A8 / B8G8R8A8
// family that is >= scene size in both axes and not scene-sized (VR eye texture; rejects the
// sun-shaft mask -> 2296x3248 blur draws and the OpenXR swapchain images). v0.5.2: every match consumes the
// oldest unconsumed pass of the FIFO (its slot gives eye identity, jitter, depth source). A backbuffer match is flat.
#ifdef WITH_DLAA
// ---- v0.6.1 record classification + flight-recorder log (blit, render thread) ------------------------------
uint32_t         g_cAnomalyLogs  = 0;            // PASS ANOMALY lines written (cap kAnomalyLogCap)
constexpr uint32_t kAnomalyLogCap = 80;
bool             g_sampleDone    = false;        // first "pass sample" line written
uint64_t         g_nextSampleBlit = 0;           // next "pass sample" at this g_blitCount
constexpr uint64_t kSampleEvery  = 3000;
int64_t          g_eyeLastBlitT[kMaxEyes] = {};  // QPC of the previous matched blit per eye

void LogPassRecord(const char* prefix, int eye, const PassSlot& slot, int lgW, int lgC, int64_t now,
                   double eyeIvMs, const char* note) {
    const double qf = g_qpcFreq > 0 ? (double)g_qpcFreq : 1.0;
    const double ageMs = slot.tStart ? (double)(now - slot.tStart) * 1000.0 / qf : 0.0;
    const uint32_t deferred = slot.defClosed ? slot.diDeferred
                                             : g_diDeferred.load(std::memory_order_relaxed) - slot.defStart;
    // v0.6.2: foreign-ctx = OM/RS/Draw/DrawIndexed on immediate contexts other than the game's (ignored by the
    // hooks); game-ctx other-thread = OM/RS on the game context from a thread other than the blit thread.
    const uint32_t fgn = slot.defClosed ? slot.fgnCtx : g_foreignCtx.load(std::memory_order_relaxed) - slot.fgnStart;
    const uint32_t oth = slot.defClosed ? slot.otherThread
                                        : g_gameOtherThread.load(std::memory_order_relaxed) - slot.otStart;
    Log("%s eye=%d s=%llu blit #%llu: records world=%d cabin=%d (last good %d/%d) | DrawIndexed gbuf=%u other=%u "
        "deferred=%u | gbuf binds=%u scene clears=%u | skips: sampled=%u gate=%u indlaa=%u noctx=%u full=%u nocb=%u range=%u | "
        "pass age %.2f ms, eye blit interval %.2f ms | snap=%d discarded=%d | Present #%llu | foreign-ctx OM/RS/Draw/DI=%u "
        "game-ctx other-thread OM/RS=%u scene-depth changes=%llu%s",
        prefix, eye, (unsigned long long)slot.s, (unsigned long long)g_blitCount, slot.cand.Count(0), slot.cand.Count(1),
        lgW, lgC, slot.diGbuf, slot.diOther, deferred, slot.gbufBinds, slot.sceneClears,
        slot.skSampled, slot.skGate, slot.skInDlaa, slot.skNoCtx, slot.skFull, slot.skNoCb, slot.skRange,
        ageMs, eyeIvMs, (int)slot.snap, (int)slot.discarded, (unsigned long long)g_frames.load(), fgn, oth,
        (unsigned long long)g_depthChanges, note);
}

// Classifies the slot's candidate record for `eye` (definition: EyeRecState) and logs anomalies / samples.
// Returns true = BAD (SceneDlaa::Run reuses the last good reprojection). `gen` = the eye's
// CameraMv::InvalidateCount() right now. Only meaningful while candidates are being collected.
bool ClassifyRecord(int eye, const PassSlot& slot, uint64_t gen, int64_t now, double eyeIvMs) {
    if (!g_dlaaOn.load(std::memory_order_relaxed) || !(g_mvOn.load(std::memory_order_relaxed) || g_vrMode) ||
        g_passive.load(std::memory_order_relaxed))
        return false;                                     // collection gate closed: empty records by design
    EyeRecState& st = g_eyeRec[eye];
    if (st.have && st.gen != gen) st = EyeRecState();     // CameraMv invalidated since (resize, Init, ...)
    const int w = slot.cand.Count(0), c = slot.cand.Count(1);
    const int lgW = st.world, lgC = st.cabin;
    const bool rawBad = st.have && ((lgW > 0 && 2 * w < lgW) || (lgC > 0 && 2 * c < lgC));
    // A persistent drop (e.g. a camera / view change) must not freeze R forever: the (kBadRunMax+1)th bad
    // record in a row becomes the new baseline (it is then handled as "first good after bad").
    const bool forced = rawBad && st.badRun >= kBadRunMax;
    const bool bad = rawBad && !forced;
    if (bad) {
        ++st.badRun;
    } else {
        st.have = true; st.world = w; st.cabin = c; st.badRun = 0; st.gen = gen;
    }
    if (rawBad) {
        if (g_cAnomalyLogs < kAnomalyLogCap) {
            ++g_cAnomalyLogs;
            LogPassRecord("PASS ANOMALY", eye, slot, lgW, lgC, now, eyeIvMs,
                          forced ? " -- accepted as the new baseline (3rd bad record in a row)" :
                          (g_cAnomalyLogs == kAnomalyLogCap ? " (anomaly log cap reached)" : ""));
        }
    } else if ((g_eyeMapN >= 6 || g_flatMode) && (!g_sampleDone || g_blitCount >= g_nextSampleBlit)) {
        g_sampleDone = true;
        g_nextSampleBlit = g_blitCount + kSampleEvery;
        LogPassRecord("pass sample", eye, slot, lgW, lgC, now, eyeIvMs, "");
    }
    return bad;
}
#endif

bool IsRgbaBgraFamily(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_UNORM ||
           f == DXGI_FORMAT_R8G8B8A8_TYPELESS || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}

// v0.7.9 launch-mode fallback (see g_eyeBlitEver). Both switches drop the outstanding passes (they belong to the old
// mode's pairing), every DLSS / MV history and the per-mode detection, exactly like a fresh start in the new mode.
void DropPassesForModeSwitch() {
    while (g_fifoHead < g_passSeq) g_ring[g_fifoHead++ % kRing].consumed = true;
    g_flatMode = false; g_vrMode = false;                // re-detected at the next matched blit
    g_eyeMapN = 0; g_eyeMapLogged = false; g_prevBlitRt = nullptr; g_lastUnderflowBlit = 0;
    for (EyeMapEntry& e : g_eyeMap) e.score = 0;
    for (bool& b : g_eyeLogged) b = false;
    g_resetNext = true;
    MvInvalidate();
#ifdef WITH_DLAA
    g_pvEyeN = 0;                                        // preview eye map: rebuilt for the new mode
#endif
}

// Launched VR, but nothing but backbuffer blits: behave exactly as a FLAT launch from now on (backbuffer blits run
// DLAA, dlaa_area forced to 100 with the VR value kept for Shift+F12, the area keys give the low beep, the preview
// uses the flat route).
void FallBackToFlat(const D3D11_TEXTURE2D_DESC& bd) {
    g_launchVr = 0;
    g_vrFellBack = true;
#ifdef WITH_DLAA
    g_vrArea = SceneDlaa::Area();
    SceneDlaa::SetArea(100);
#endif
    DropPassesForModeSwitch();
    Log("launch mode FALLBACK: the launch options say VR, but no VR eye texture was ever drawn and the last %u world blits "
        "went to the backbuffer (%ux%u fmt=%d, Present #%llu) -- running as FLAT from now on (backbuffer blits get DLAA, "
        "dlaa_area 100 [VR value %d kept], area keys locked, preview: flat route); an eye-texture blit later switches back "
        "to VR", g_bbBlitRun, bd.Width, bd.Height, (int)bd.Format, (unsigned long long)g_frames.load(), g_vrArea);
}

// Fallen back to flat, and now an eye-texture blit: the headset session came up late. Back to the VR launch behaviour.
void BackToVr(const D3D11_TEXTURE2D_DESC& bd) {
    g_launchVr = 1;
    g_vrFellBack = false;
    g_eyeBlitEver = true;
    g_bbBlitRun = 0;
#ifdef WITH_DLAA
    SceneDlaa::SetArea(g_vrArea);
#endif
    DropPassesForModeSwitch();
    Log("launch mode back to VR: an eye-texture blit (RT %ux%u fmt=%d, Present #%llu) after the flat fallback -- VR "
        "behaviour again (dlaa_area %d, the backbuffer is the mirror window); this blit is skipped", bd.Width, bd.Height,
        (int)bd.Format, (unsigned long long)g_frames.load(), g_vrArea);
}

void HandlePossibleBlit(ID3D11DeviceContext* ctx, UINT vertexCount) {
    // 1. RT0: the backbuffer (flat) or an eye-sized RGBA/BGRA texture (VR).
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return;
    D3D11_RENDER_TARGET_VIEW_DESC rvd{};                 // v0.7.0: RTV view format (upscale composite log)
    rtv->GetDesc(&rvd);
    ID3D11Resource* rres = nullptr;
    rtv->GetResource(&rres);
    rtv->Release();
    if (!rres) return;
    void* const rtPtr = (void*)rres;                     // identity only (no ref kept)
    bool isBackbuffer = rtPtr == g_backbuffer.load(std::memory_order_relaxed);
    D3D11_TEXTURE2D_DESC bd{};
    bool rtOk = false;
    {
        ID3D11Texture2D* rt = nullptr;
        if (SUCCEEDED(rres->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&rt)) && rt) {
            rt->GetDesc(&bd);
            rt->Release();
            // v0.7.3: flat. The pointer from GetBuffer(0) at the last Present is not always the texture the game
            // blits into (ATS flat: other pointers after a hitch / swapchain change) -> the blit was taken for a
            // VR eye texture and the mod switched to VR mode for good. While VR is not known, an RT with exactly
            // the backbuffer's size and format is the backbuffer. (VR eye textures differ from the mirror window.)
            if (!isBackbuffer && !g_vrMode && g_bbW && bd.Width == g_bbW && bd.Height == g_bbH &&
                bd.Format == g_bbFmt && bd.SampleDesc.Count == 1) {
                isBackbuffer = true;
                if (g_cBbByDesc++ == 0)
                    Log("flat: blit RT %p is not GetBuffer(0) %p but has the backbuffer's size / format (%ux%u fmt=%d) -- "
                        "treated as the backbuffer", rtPtr, g_backbuffer.load(std::memory_order_relaxed),
                        bd.Width, bd.Height, (int)bd.Format);
            }
            if (isBackbuffer) rtOk = true;                       // flat: exactly as before
            else rtOk = IsRgbaBgraFamily(bd.Format) && bd.SampleDesc.Count == 1 &&
                        bd.Width >= g_sceneW && bd.Height >= g_sceneH;
        }
    }
    rres->Release();
    // v0.8.0 HDR candidate: HDR output (g_hdrOut) and a single-sample RGBA16F RT0 of the scene or the backbuffer size
    // (the only two shapes the HDR rule below can match; the bloom chain's small RTs never fetch SRV0). Only then the HDR
    // rule is looked at; every other draw takes exactly the v0.7.x path (rtOk), and an HDR candidate that is not matched
    // by the HDR rule falls through to it as well.
    const bool hdrCand = g_hdrOut && g_sceneW && bd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && bd.SampleDesc.Count == 1 &&
                         ((bd.Width == g_sceneW && bd.Height == g_sceneH) || (bd.Width == g_bbW && bd.Height == g_bbH));
    if (!rtOk && !hdrCand) return;
    // v0.7.1: a scene-sized RT is no longer rejected here (VR at r_scale 1 x 1: eye RT == scene size, so the
    // eye blit never matched and VR got no DLAA). It is told apart from other scene-sized passes below.
    const bool rtSceneSized = !isBackbuffer && bd.Width == g_sceneW && bd.Height == g_sceneH;

    // 2. PS SRV slot 0 must be an R8G8B8A8-family Texture2D at scene dims.
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return;
    D3D11_SHADER_RESOURCE_VIEW_DESC svd{};              // v0.7.0: SRV0 view format (_SRGB -> composite decodes)
    srv->GetDesc(&svd);
    ID3D11Resource* sres = nullptr;
    srv->GetResource(&sres);
    srv->Release();
    if (!sres) return;
    void* const srcPtr = (void*)sres;                    // identity only (no ref kept)
    // v0.8.0 HDR rule (see g_fwdColorTex for the frame structure). Decided by SRV0's identity, before any QI / GetDesc:
    //  (a) RT0 scene-sized RGBA16F reading the forward colour target = the HDR composite ([5024]): remember its RT, no blit;
    //  (b) RT0 backbuffer-sized RGBA16F that is not the composite, reading the composite's RT = the HDR blit ([5028]).
    bool hdrBlit = false;
    if (hdrCand) {
        if (g_fwdColorTex && srcPtr == g_fwdColorTex && rtPtr != srcPtr && bd.Width == g_sceneW && bd.Height == g_sceneH) {
            if (!g_hdrCompLogged) {
                g_hdrCompLogged = true;
                Log("HDR scene composite recognised: Draw (%u verts) RT0 %p %ux%u fmt=%d reads the forward scene colour %p "
                    "(scene %ux%u) -- its RT0 is the source of the HDR blit (Present #%llu)", vertexCount, rtPtr, bd.Width,
                    bd.Height, (int)bd.Format, srcPtr, g_sceneW, g_sceneH,
                    (unsigned long long)g_frames.load(std::memory_order_relaxed));
            }
            g_hdrCompTex = rtPtr;
            sres->Release();
            return;
        }
        hdrBlit = g_hdrCompTex && srcPtr == g_hdrCompTex && rtPtr != g_hdrCompTex && g_bbW &&
                  bd.Width == g_bbW && bd.Height == g_bbH;
        if (!hdrBlit && !rtOk) { sres->Release(); return; }
    }
    ID3D11Texture2D* tex = nullptr;
    sres->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    sres->Release();
    if (!tex) return;

    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    if (hdrBlit) {
        // v0.8.0: the composite is scene-sized RGBA16F by construction (rule (a)); checked on the texture itself anyway.
        // The HDR output target is the flat screen path: treated exactly like a backbuffer blit from here on (eye 0,
        // flat mode, VR launch -> mirror / flat fallback counting). VR + HDR is not a game mode.
        if (d.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || d.Width != g_sceneW || d.Height != g_sceneH ||
            d.SampleDesc.Count != 1) {
            tex->Release();
            return;
        }
        isBackbuffer = true;
    } else {
        // v0.7.1: scene-sized RT. The tonemap draw (RGBA16F scene -> RGBA8 scene-size RT, right before the blit) names
        // the scene colour texture; a scene-sized blit must read exactly that texture and write a different one.
        if (rtSceneSized) {
            if (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && d.Width == g_sceneW && d.Height == g_sceneH) {
                g_tonemapTex = rtPtr;
                tex->Release();
                return;
            }
            if (srcPtr != g_tonemapTex || rtPtr == g_tonemapTex) { tex->Release(); return; }
        }
        const bool fmtOk = d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                           d.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                           d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
        if (!fmtOk || d.Width != g_sceneW || d.Height != g_sceneH || d.SampleDesc.Count != 1) {
            tex->Release();
            return;
        }
    }

    // Matched. v0.7.4: the launch options say which mode this is (g_launchVr, set at load). Flat launch: only the
    // backbuffer counts, the mod never switches to VR. VR launch: the backbuffer is always the mirror window.
    // v0.7.9: the launch options are only the first guess -- a VR launch without any eye-texture blit falls back to
    // flat after kFlatFallbackBlits backbuffer blits in a row, and an eye-texture blit after that switches back to VR.
    if (!isBackbuffer && g_vrFellBack) { BackToVr(bd); tex->Release(); return; }
    if (g_launchVr == 0 && !isBackbuffer) { tex->Release(); return; }
    if (isBackbuffer && (g_vrMode || g_launchVr == 1)) {
        if (g_launchVr == 1 && !g_eyeBlitEver && ++g_bbBlitRun >= kFlatFallbackBlits) {
            FallBackToFlat(bd);                          // this blit is skipped; the next one runs as flat
        } else {
            ++g_cMirrorIgnored;
        }
        tex->Release();
        return;
    }
    if (!isBackbuffer && !g_eyeBlitEver) {
        g_eyeBlitEver = true;                            // VR is real: the world jitter is no longer held
        if (g_bbBlitRun)
            Log("VR confirmed at blit: an eye-texture blit (RT %ux%u fmt=%d) after %u backbuffer blit(s) -- the fallback to "
                "flat is off for this session", bd.Width, bd.Height, (int)bd.Format, g_bbBlitRun);
    }
    if (isBackbuffer) {
        if (!g_flatMode) { // v0.7.10: first time flat is confirmed -- one line so the log shows flat was detected (VR logs below)
            if (hdrBlit)   // v0.8.0: the HDR rule matched (RT0 = the game's RGBA16F output target, not the backbuffer)
                Log("flat detected at blit: HDR blit (RGBA16F output target %ux%u fmt=%d, backbuffer %ux%u fmt=%d, scene "
                    "composite %ux%u fmt=%d) at Present #%llu", bd.Width, bd.Height, (int)bd.Format, g_bbW, g_bbH,
                    (int)g_bbFmt, d.Width, d.Height, (int)d.Format, (unsigned long long)g_frames.load(std::memory_order_relaxed));
            else
                Log("flat detected at blit: backbuffer blit (RT %ux%u fmt=%d) at Present #%llu",
                    bd.Width, bd.Height, (int)bd.Format, (unsigned long long)g_frames.load(std::memory_order_relaxed));
        }
        g_flatMode = true;
    }
    else if (!g_vrMode) {
        g_vrMode = true; g_flatMode = false;
        Log("VR detected at blit: eye identity from the blit render target (RT map)");
    }
    if (hdrBlit) ++g_hdrBlitCount;                       // v0.8.0 (matched and not skipped as mirror / fallback)
#ifdef WITH_DLAA
    // v0.7.0 DLSS upscale decision. Output = the blit's viewport 0 inside RT0 (rounded; the whole RT if the
    // viewport is missing or not inside it). Upscale only with dlss_upscale on (Ctrl+F4) AND output >= the scene
    // in both axes AND larger in one; else the v0.6.5 DLAA path. The scene vs RT size IS the render scale
    // (r_scale_x / r_scale_y shrink only the scene texture).
    uint32_t upX = 0, upY = 0, upW = 0, upH = 0;         // output rect in RT0 (upW = 0: no upscale)
    const bool srgbDecode = svd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || svd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    {
        uint32_t ox = 0, oy = 0, ow = bd.Width, oh = bd.Height;
        D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        UINT nvp = 0;
        ctx->RSGetViewports(&nvp, nullptr);
        if (nvp > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE) nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        if (nvp) ctx->RSGetViewports(&nvp, vps);
        bool vpUsed = false;
        if (nvp >= 1) {
            const double x = std::floor((double)vps[0].TopLeftX + 0.5), y = std::floor((double)vps[0].TopLeftY + 0.5);
            const double w = std::floor((double)vps[0].Width + 0.5),    h = std::floor((double)vps[0].Height + 0.5);
            if (x >= 0.0 && y >= 0.0 && w >= 1.0 && h >= 1.0 && x + w <= (double)bd.Width && y + h <= (double)bd.Height) {
                ox = (uint32_t)x; oy = (uint32_t)y; ow = (uint32_t)w; oh = (uint32_t)h;
                vpUsed = true;
            }
        }
        const bool sizeUp = ow >= d.Width && oh >= d.Height && (ow > d.Width || oh > d.Height);
        char sizes[48];
        snprintf(sizes, sizeof(sizes), "%ux%u->%ux%u", d.Width, d.Height, ow, oh);
        const bool blocked = !strcmp(sizes, g_upBlocked);  // NGX could not create this upscale: DLAA fallback
        const bool up = g_dlssUpscale.load(std::memory_order_relaxed) && sizeUp && !blocked;
        if (up) { upX = ox; upY = oy; upW = ow; upH = oh; }
        g_upAreaRatio = up ? ((double)ow * (double)oh) / ((double)d.Width * (double)d.Height) : 1.0;
        char tag[48];
        if (up) snprintf(tag, sizeof(tag), "%s", sizes);
        else    snprintf(tag, sizeof(tag), "off");
        static bool upLogged = false;
        if (!upLogged || strcmp(tag, g_upTag) != 0) {
            upLogged = true;
            strncpy_s(g_upTag, tag, _TRUNCATE);
            Log("DLSS upscale %s at blit #%llu: render (scene) %ux%u -> output %ux%u at (%u,%u) in RT %ux%u fmt=%d "
                "(viewport %s: %.1f,%.1f %.1fx%.1f) | blit SRV0 view fmt=%d%s | RTV view fmt=%d%s | dlss_upscale=%d",
                up ? "ACTIVE" : (!sizeUp ? "not applicable (output not larger than the scene)"
                                         : (blocked ? "OFF (NGX init failed for this size: DLAA fallback)"
                                                    : "OFF (dlss_upscale=0 / upscale key)")),
                (unsigned long long)g_blitCount + 1, d.Width, d.Height, ow, oh, ox, oy, bd.Width, bd.Height, (int)bd.Format,
                vpUsed ? "used" : (nvp ? "ignored, not inside RT0" : "none bound"),
                nvp ? (double)vps[0].TopLeftX : 0.0, nvp ? (double)vps[0].TopLeftY : 0.0,
                nvp ? (double)vps[0].Width : 0.0, nvp ? (double)vps[0].Height : 0.0,
                (int)svd.Format, srgbDecode ? " (_SRGB: composite decodes to linear)" : " (not sRGB: raw)",
                (int)rvd.Format, (rvd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || rvd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
                                     ? " (_SRGB: hardware encodes)" : " (not sRGB)",
                (int)g_dlssUpscale.load());
        }
    }
#endif
    ++g_blitCount;
    if (g_blitCount == 1)   // v0.7.10: first scene->screen blit matched this process (the copy DLAA/DLSS hooks into)
        Log("first blit matched: blit #1 at Present #%llu, RT %ux%u fmt=%d, scene source %ux%u fmt=%d (%s)",
            (unsigned long long)g_frames.load(std::memory_order_relaxed), bd.Width, bd.Height, (int)bd.Format,
            d.Width, d.Height, (int)d.Format,
            hdrBlit ? "flat HDR: RGBA16F scene composite -> RGBA16F output target"   // v0.8.0
                    : (isBackbuffer ? "flat backbuffer" : "VR eye"));
    else if (hdrBlit && g_hdrBlitCount == 1)   // v0.8.0: HDR switched on after 8-bit blits (Windows HDR toggled in game)
        Log("first HDR blit matched: blit #%llu at Present #%llu, RT %ux%u fmt=%d, scene source %ux%u fmt=%d (flat HDR: "
            "RGBA16F scene composite -> RGBA16F output target)", (unsigned long long)g_blitCount,
            (unsigned long long)g_frames.load(std::memory_order_relaxed), bd.Width, bd.Height, (int)bd.Format,
            d.Width, d.Height, (int)d.Format);
    BlitCpuTimer cpuT;                                   // v0.5.6: CPU cost of everything below (all return paths)
    if (g_rateBlit0 == 0) { g_rateBlit0 = g_blitCount; g_rateT0 = cpuT.t0; }
#ifdef WITH_DLAA
    // v0.5.7: one rate line kRateChgAt blits after a live preset / sharpness change, measured over blits
    // kRateChgSkip..kRateChgAt (the recreate hitch right after the change is not counted). A newer change
    // restarts the window.
    if (g_rateChgArm) { g_rateChgArm = false; g_rateChgBlit = g_blitCount; g_rateChgT0 = 0; }
    if (g_rateChgBlit) {
        const uint64_t since = g_blitCount - g_rateChgBlit;
        if (since == kRateChgSkip) {
            g_rateChgT0 = cpuT.t0;
        } else if (since >= kRateChgAt) {
            const double qf   = g_qpcFreq > 0 ? (double)g_qpcFreq : 1.0;
            const double secs = g_rateChgT0 ? (double)(cpuT.t0 - g_rateChgT0) / qf : 0.0;
            const double bps  = secs > 0.0 ? (double)(kRateChgAt - kRateChgSkip) / secs : 0.0;
            Log("rate after change: blits/s=%.1f fps=%.1f [preset=%s sharp=%.1f r=%.1f area=%d up=%s]",
                bps, g_vrMode ? bps / 2.0 : bps, SceneDlaa::DlssPresetName(), (double)SceneDlaa::Sharpness(),
                (double)SceneDlaa::SharpRadius(), SceneDlaa::Area(), g_upTag);
            g_rateChgBlit = 0;
        }
    }
#endif
    void* const prevRt = g_prevBlitRt;                  // v0.5.5 clean-blit filter (same RT twice in a row = menu)
    g_prevBlitRt = rtPtr;
    (void)prevRt;
    const uint64_t frame = g_frames.load();
    if (!g_tonemapLogged) {
        g_tonemapLogged = true;
        Log("swapchain blit detected on the game context %p: src %ux%u fmt=%d -> %s %ux%u (verts=%u, scene depth %ux%u)%s",
            (void*)ctx, d.Width, d.Height, (int)d.Format,
            hdrBlit ? "HDR output target" : (isBackbuffer ? "backbuffer" : "eye RT"), bd.Width, bd.Height,   // v0.8.0
            vertexCount, g_sceneW, g_sceneH,
            g_dlaaOn.load() ? "" : " -- DLAA currently OFF (dlaa_off.txt / DLAA toggle key), not evaluating");
    }
    if (g_blitCount % 600 == 0) {
        // v0.5.6 CPU / rate window: everything since the previous stats line (600 blits).
        const double qf = g_qpcFreq > 0 ? (double)g_qpcFreq : 1.0;
        const uint64_t wPasses = g_passSeq - g_cpuPass0;
        const double cpuBlitMs = g_cpuBlitN ? (double)g_cpuBlitTicks * 1000.0 / qf / (double)g_cpuBlitN : 0.0;
        const double cpuMvMs   = wPasses ? (double)g_cpuMvTicks * 1000.0 / qf / (double)wPasses : 0.0;
        const double recPass   = wPasses ? (double)g_cMvRecords / (double)wPasses : 0.0;
        const double wSecs     = (double)(cpuT.t0 - g_rateT0) / qf;
        const double blitsPerS = wSecs > 0.0 ? (double)(g_blitCount - g_rateBlit0) / wSecs : 0.0;
        const double fps       = g_vrMode ? blitsPerS / 2.0 : blitsPerS;   // VR: 2 eye blits per frame
#ifdef WITH_DLAA
        const char*  stPreset  = SceneDlaa::DlssPresetName();             // v0.5.7: state the rate belongs to
        const double stSharp   = (double)SceneDlaa::Sharpness();
        const double stRadius  = (double)SceneDlaa::SharpRadius();        // v0.5.8
        const int    stArea    = SceneDlaa::Area();                       // v0.6.4
#else
        const char*  stPreset  = "n/a";
        const double stSharp   = 0.0;
        const double stRadius  = 0.0;
        const int    stArea    = 100;
#endif
        Log("FIFO stats @blit %llu: passes=%llu blits=%llu depth snapshot used=%llu live used=%llu fifo underflow=%llu "
            "fifo overflow=%llu mirror ignored=%llu | snapshots: at discard=%llu at clear=%llu live used=%llu discarded-without-snapshot=%llu (scene depth discards seen=%llu) | eye map: %s entries=%d rt-parity-mismatch=%llu verdicts=%llu no-vote=%llu corrections=%llu rb-skipped-full=%llu rb-failed=%llu rb-throttled=%llu | R |x|>0.25: e0=%llu/%llu e1=%llu/%llu | dlaa frames e0=%llu e1=%llu | "
            "cpu: blit %.3f ms/blit, mv-collect %.3f ms/pass (%.0f records/pass) | rate: blits/s=%.1f fps=%.1f (%s) [preset=%s sharp=%.1f r=%.1f area=%d up=%s] | Present #%llu",
            (unsigned long long)g_blitCount, (unsigned long long)g_passSeq, (unsigned long long)g_blitCount,
            (unsigned long long)g_cSnapUsed, (unsigned long long)g_cLiveUsed, (unsigned long long)g_cUnderflow,
            (unsigned long long)g_cOverflow, (unsigned long long)g_cMirrorIgnored,
            (unsigned long long)g_cSnapAtDiscard, (unsigned long long)g_cSnapAtClear, (unsigned long long)g_cLiveUsed,
            (unsigned long long)g_cDiscardNoSnap, (unsigned long long)g_cDiscardSeen,
            g_vrMode ? "RT map" : "flat", g_eyeMapN, (unsigned long long)g_cRtParityMismatch,
            (unsigned long long)g_cVerdicts, (unsigned long long)g_cVerdictNoVote, (unsigned long long)g_cEyeCorrections,
            (unsigned long long)g_cRbSkipFull, (unsigned long long)g_cRbFailed, (unsigned long long)g_cRbThrottled,
            (unsigned long long)g_rBig[0], (unsigned long long)g_rReads[0],
            (unsigned long long)g_rBig[1], (unsigned long long)g_rReads[1],
            (unsigned long long)g_dlaaFrames[0], (unsigned long long)g_dlaaFrames[1],
            cpuBlitMs, cpuMvMs, recPass, blitsPerS, fps,
            g_vrMode ? "VR: fps = blits/s / 2" : "flat: fps = blits/s", stPreset, stSharp, stRadius, stArea,
            g_upTag, (unsigned long long)frame);
        g_cpuBlitTicks = 0; g_cpuBlitN = 0; g_cpuMvTicks = 0; g_cMvRecords = 0; g_cpuPass0 = g_passSeq;
        g_rateBlit0 = g_blitCount; g_rateT0 = cpuT.t0;
    }

    // FIFO: this blit belongs to the OLDEST unconsumed pass.
    if (g_fifoHead >= g_passSeq) {
        g_lastUnderflowBlit = g_blitCount;
        if (g_cUnderflow++ < 5)
            Log("fifo underflow: blit #%llu has no unconsumed pass (skipping DLAA for it)", (unsigned long long)g_blitCount);
        SpanOnUnderflow(ctx);                            // v0.6.2: no clean frame pairing
        tex->Release();
        return;
    }
    PassSlot& slot = g_ring[g_fifoHead % kRing];
    slot.consumed = true;
    ++g_fifoHead;
    // v0.6.2 GPU spans: this pass's gbuf span ends (if no discard ended it) before any of our work; the frame's
    // scene span is marked to close after the game's Draw of this blit when it is the frame's clean last blit.
    SpanOnBlit(ctx, slot.s);
    SpanPollAndLog(ctx);
    const bool passive = g_passive.load(std::memory_order_relaxed);
    (void)passive;
    // Eye identity is authoritative from the blit render target (flat backbuffer = eye 0).
    int eye = isBackbuffer ? 0 : EyeOfRT(rtPtr, (int)(slot.s & 1));
    if (eye < 0 || eye >= kMaxEyes) eye = 0;
    if (slot.discarded && !slot.snap) {
        static int warned = 0;
        ++g_cDiscardNoSnap;
        if (warned++ < 5)
            Log("WARNING: scene depth was discarded before pass s=%llu was snapshotted (depth discarded without snapshot, DLAA %s)",
                (unsigned long long)slot.s, g_dlaaOn.load() ? "on" : "off");
    }
    if (!g_eyeLogged[eye]) {
        g_eyeLogged[eye] = true;
        Log("eye blit detected: eye=%d (pass s=%llu) src %ux%u -> RT %ux%u fmt=%d (%s, Present #%llu)",
            eye, (unsigned long long)slot.s, d.Width, d.Height, bd.Width, bd.Height, (int)bd.Format,
            hdrBlit ? "flat HDR output target" : (isBackbuffer ? "flat backbuffer" : "VR eye texture"),   // v0.8.0
            (unsigned long long)frame);
    }
#ifdef WITH_DLAA
    // v0.5.5: VR only -- collect finished projection verdicts (may correct the RT map for the NEXT blits),
    // then queue this pass's MVPs for an async verdict if the blit is clean (not the same RT as the previous
    // blit, no FIFO underflow within the last 4 blits). Run() below does not modify slot.cand.
    // v0.6.2: none of it in passive mode (the readback is a GPU copy; the records are empty anyway).
    if (!isBackbuffer && !passive) {
        t_inDlaa = true;
        if (EnsureEyeReadback(ctx)) {
            PollEyeReadbacks(ctx);
            const bool clean = rtPtr != prevRt && (g_lastUnderflowBlit == 0 || g_blitCount - g_lastUnderflowBlit > 4);
            if (clean) {
                // v0.5.6: once settled (>= 6 RTs, every score saturated at +-6) read back only every 31st clean
                // blit (31 is coprime to the 6-image cycle, so every RT keeps being checked).
                bool settled = g_eyeMapN >= 6;
                for (int i = 0; settled && i < g_eyeMapN; ++i)
                    if (g_eyeMap[i].score != 6 && g_eyeMap[i].score != -6) settled = false;
                if (!settled) g_settledClean = 0;
                if (!settled || g_settledClean++ % 31 == 0) QueueEyeReadback(ctx, slot.cand, rtPtr);
                else ++g_cRbThrottled;
            }
        }
        t_inDlaa = false;
    }
    SceneDlaa& dl = g_dlaa[eye];
    dl.SetEye(eye);
    // v0.6.1: classify this pass's candidate record before anything consumes it (anomaly / sample log).
    g_renderTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const double eyeIvMs = g_eyeLastBlitT[eye] && g_qpcFreq > 0
                               ? (double)(cpuT.t0 - g_eyeLastBlitT[eye]) * 1000.0 / (double)g_qpcFreq : 0.0;
    g_eyeLastBlitT[eye] = cpuT.t0;
    const bool candBad = ClassifyRecord(eye, slot, dl.Mv().InvalidateCount(), cpuT.t0, eyeIvMs);
    // We run between the game's blit state setup and its Draw. Do NOT use
    // SwapDeviceContextState here: NGX evaluates fine once, then fails with
    // PlatformError and the device is removed (tested 2026-10-03). NGX and our
    // depth pass only touch the CS stage (saved/restored in SceneDlaa::Run),
    // and CopyResource does not change bindings.
    // v0.7.0 upscale: Run leaves the scene texture untouched and only ARMS the composite; the graphics-state
    // draw into RT0 happens in hkDraw after the game's own blit Draw (SceneDlaa::Composite, full save/restore).
    // Everything frame-specific comes from the pass slot (jitter, eye, depth snapshot), not from Present timing.
    const float stVx = slot.jx, stVy = slot.jy;
    const float stNx = (float)g_signX * slot.jx, stNy = (float)g_signY * slot.jy;
    ID3D11Texture2D* stTex = tex;          // v0.7.0: what the self-test dumps (upscale + evaluated: the DLSS output)
    if (passive) {
        // v0.6.2 passive mode: no DLAA, no commit (nothing was collected); history reset on leaving (TogglePassive).
    } else if (g_dlaaOn.load() && g_jitterOnly.load()) {
        if (dl.Mv().Ready()) dl.Mv().Commit(ctx, slot.cand);         // keep this eye's MV candidate history rolling
    } else if (g_dlaaOn.load() && !SceneDlaa::ShadersReady()) {
        // v0.7.8: the one-time shader warm-up (ShaderCache worker thread) is still running: this blit passes through
        // untouched, the eye's unit is built at a later blit. (Only possible in the first seconds after the DLL load.)
        static uint64_t waits = 0;
        if (waits++ == 0)
            Log("DLAA waits for the shader warm-up (eye %d, blit #%llu) -- frames pass through untouched until it is done",
                eye, (unsigned long long)g_blitCount);
    } else if (g_dlaaOn.load()) {
        // A global reset request (g_resetNext) applies to every eye: latch it at the next blit.
        if (g_resetNext) { g_resetEyes = (1 << kMaxEyes) - 1; g_resetNext = false; }
        const bool resetUsed = (g_resetEyes & (1 << eye)) != 0;
        g_resetEyes &= ~(1 << eye);
        // Jitter this pass was rasterized with (viewport shift * sign) goes to NGX.
        const float njx = stNx, njy = stNy;
        // Depth: this pass's own snapshot twin (taken at the discard / next clear) or live depth.
        const bool useSnap = slot.snap && slot.twin.valid;
        if (useSnap) ++g_cSnapUsed; else ++g_cLiveUsed;
        dl.SetTiming(GpuTimingOn());
        // v0.6.4 DLAA area: rect centre = this eye's adopted optical centre (VR), (0.5, 0.5) until known / flat.
        // NoteOpticalCentre only changes it on a > 0.01 re-adopt; Run resets the eye's history if the rect moved.
        if (isBackbuffer || !g_optC[eye].have) dl.SetOpticalCentre(0.5f, 0.5f);
        else dl.SetOpticalCentre(g_optC[eye].u, g_optC[eye].v);
        t_inDlaa = true;
        const bool ok = dl.Run(ctx, tex, g_sceneDepth, &slot.twin, &slot.cand, njx, njy, resetUsed, g_mvOn.load(),
                               g_mvDebug.load(), candBad, upW, upH);
        t_inDlaa = false;
        static bool s_firstEvalLogged = false;   // v0.7.10: one line the first time NGX actually processed a frame
        static bool s_firstHdrEvalLogged = false;   // v0.8.0: and once more for the first HDR unit (HDR toggled later)
        if (ok && (!s_firstEvalLogged || (dl.IsHdr() && !s_firstHdrEvalLogged))) {
            s_firstEvalLogged = true;
            if (dl.IsHdr()) s_firstHdrEvalLogged = true;
            Log("first DLAA/DLSS evaluate OK: eye=%d mode=%s %s%s at Present #%llu (NGX processed a frame)",
                eye, DlaaModeStr(), dl.Upscaling() ? "upscaling" : "native-res",
                !dl.IsHdr() ? "" : (SceneDlaa::HdrLinear() ? " HDR (RGBA16F, NGX IsHDR)"          // v0.8.0
                                                           : " HDR (RGBA16F, dlss_hdr=0: no IsHDR)"),
                (unsigned long long)g_frames.load(std::memory_order_relaxed));
        }
        // v0.7.0: arm the composite for the Draw that follows (hkDraw, after the game's blit).
        if (dl.CompositePending()) {
            g_compEye = eye; g_compRt = rtPtr; g_compX = upX; g_compY = upY; g_compSrgb = srgbDecode;
        }
        // v0.7.0: NGX / texture init failed for the UPSCALE sizes -> block exactly these sizes (both eyes) so the
        // next blits rebuild as DLAA at render res instead of staying un-anti-aliased. Long low tone. Ctrl+F4 ON
        // (or a size change) retries.
        if (upW && !ok && !dl.Deferred() && dl.Upscaling() && dl.InitFailed()) {
            snprintf(g_upBlocked, sizeof(g_upBlocked), "%ux%u->%ux%u", dl.FullW(), dl.FullH(), upW, upH);
            Log("DLSS upscale init FAILED on eye %d for %s (see the DLAA: lines above) -- falling back to DLAA at render "
                "res for this size (toggling the upscale key off/on retries)", eye, g_upBlocked);
            PlayTones(1, 200, 400, 0);
        }
        // v0.7.0 upscale: the snapshot "out" and the self-test dump the DLSS output (output rect size), since the
        // blit source itself is no longer modified.
        ID3D11Texture2D* const outTex = (ok && dl.Upscaling() && dl.ResultTex()) ? dl.ResultTex() : tex;
        if (ok) stTex = outTex;
        if (ok) {
            t_inDlaa = true;
            SnapshotStep(ctx, eye, outTex, SnapInfo{ g_blitCount, slot.phase, stVx, stVy, njx, njy, resetUsed, g_mvOn.load(), useSnap });
            t_inDlaa = false;
        }
        if (g_dlaaFrames[eye] < 4 && !(!ok && dl.Deferred()))   // v0.7.8: a deferred build is not a failed eval
            Log("DLAA eval: eye=%d pass s=%llu phase=%d viewport shift=(%+.4f,%+.4f) NGX jitter=(%+.4f,%+.4f) depth=%s ok=%d",
                eye, (unsigned long long)slot.s, slot.phase, stVx, stVy, njx, njy, useSnap ? "snapshot" : "live", (int)ok);
        if (ok) {
            ++g_dlaaFrames[eye];
            float rw[16];
            if (dl.Mv().TakeSolve(rw)) {
                ++g_rReads[eye];
                if (std::fabs(rw[3]) > 0.25f) {
                    if (g_rBig[eye]++ < 5)
                        Log("R_world |R[0][3]|=%.3f > 0.25 on eye %d (wrong-eye pairing signature; blit #%llu)",
                            (double)std::fabs(rw[3]), eye, (unsigned long long)g_blitCount);
                }
            }
        }
    }
    if (eye == 0) {                        // self-test dumps eye 0 only (once per frame)
        t_inDlaa = true;                   // our own work: keep context hooks passive
        SelfTestStep(ctx, stTex, slot.phase, stVx, stVy, stNx, stNy);
        t_inDlaa = false;
    }

#endif
    tex->Release();
}

#ifdef WITH_DLAA
// v0.7.0: the armed upscale composite, right after the game's blit Draw (hkDraw). RT0 must still be the blit's
// render target (it is: nothing runs between HandlePossibleBlit, the game's Draw and this); otherwise the result
// is dropped for this frame. Runs inside t_inDlaa so the composite's own RSSetViewports / Draw pass straight
// through our hooks.
void RunPendingComposite(ID3D11DeviceContext* ctx) {
    const int eye = g_compEye;
    g_compEye = -1;
    if (eye < 0 || eye >= kMaxEyes) return;
    SceneDlaa& dl = g_dlaa[eye];
    t_inDlaa = true;
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    ID3D11Resource* res = nullptr;
    if (rtv) { rtv->GetResource(&res); rtv->Release(); }
    const bool same = res && (void*)res == g_compRt;
    if (res) res->Release();
    if (same) {
        dl.Composite(ctx, g_compX, g_compY, g_compSrgb);
    } else {
        dl.CancelComposite(ctx);
        if (g_compSkipped++ < 5)
            Log("DLAA upscale composite skipped on eye %d: RT0 changed between the blit and its Draw (#%llu)", eye,
                (unsigned long long)g_compSkipped);
    }
    t_inDlaa = false;
}
#endif

#ifdef WITH_DLAA
// v0.7.8 profile-screen preview: the game's composite Draw (3 vertices, PS SRV0 = the RT_k of a slot, RT0 = the
// backbuffer in flat or the eye RT in VR). Flat order is pass, composite, pass, composite; VR order is 4 passes, then 4
// composites in reverse RT order, then the same for eye 1 (see the block comment above PvSlot). The slot is the OLDEST
// slot of this frame whose RT == PS SRV0 and that is not composited yet. Round 4: nothing is evaluated here any more;
// the composite only marks its slot done, finds / creates the frame's index of its RT0 (the composite TARGET, max
// kPtMax), lowers that target's reference slot and leaves the target pending: its single DLAA runs later, from
// PreviewFlush, once the composites into it are finished. RT0 other than the backbuffer / a Texture2D of the preview
// RT's size: not a composite. Returns true when this Draw is a matched preview composite (the caller then must not treat
// it as the "something else drew into the pending target" trigger).
bool PreviewComposite(ID3D11DeviceContext* ctx) {
    if (g_pvCount <= 0 || !g_pvDs) return false;
    if (!g_dlaaOn.load(std::memory_order_relaxed) || g_passive.load(std::memory_order_relaxed)) return false;
    // PS SRV0 -> the oldest uncomposited slot of the frame with that RT.
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return false;
    ID3D11Resource* sres = nullptr;
    srv->GetResource(&sres);
    srv->Release();
    if (!sres) return false;
    int slot = -1;
    for (int i = 0; i < g_pvCount && i < kPvSlots; ++i)
        if (!g_pv[i].composited && (ID3D11Resource*)g_pv[i].rt.Get() == sres) { slot = i; break; }
    sres->Release();
    if (slot < 0) return false;
    // RT0: the backbuffer (pointer, or the size-and-format rule of HandlePossibleBlit) or a Texture2D with the preview
    // RT's width / height (the VR eye RT, any format).
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return false;
    Microsoft::WRL::ComPtr<ID3D11Resource> rres;
    rtv->GetResource(&rres);
    rtv->Release();
    if (!rres) return false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    rres.As(&tex);
    if (!tex || (ID3D11Resource*)tex.Get() == (ID3D11Resource*)g_pv[slot].rt.Get()) return false;   // (RT_k is never its own target)
    D3D11_TEXTURE2D_DESC bd{};
    tex->GetDesc(&bd);
    const bool target = (void*)tex.Get() == g_backbuffer.load(std::memory_order_relaxed) ||
                        (g_bbW && bd.Width == g_bbW && bd.Height == g_bbH && bd.Format == g_bbFmt && bd.SampleDesc.Count == 1) ||
                        (bd.Width == g_pvW && bd.Height == g_pvH && bd.SampleDesc.Count == 1);
    if (!target) return false;
    g_pv[slot].composited = true;                        // this pass's composite is handled now
    int idx = -1;
    for (int i = 0; i < g_ptCount; ++i)
        if (g_pt[i].tex.Get() == tex.Get()) { idx = i; break; }
    if (idx < 0 && g_ptCount < kPtMax) {
        idx = g_ptCount++;
        g_pt[idx].tex = tex;
        g_pt[idx].curRef = 99;
        g_pt[idx].slotMask = 0;
    }
    if (idx < 0) return true;                            // a third target in one frame: composite recognised, no DLAA for it
    PvTarget& t = g_pt[idx];
    if (t.flushes > 0) {                                 // v0.8.1 round 5: this part lands AFTER the target's DLAA ran
        ++g_pvLateComps;
        static int lateLogs = 0;
        if (lateLogs++ < 3)
            Log("preview target %d: composite of slot %d AFTER the target's DLAA already ran this frame (composited before it: "
                "0x%x, Present #%llu) -- that part is not anti-aliased and the target runs again with a history reset", idx,
                slot, t.maskAtFlush, (unsigned long long)g_frames.load(std::memory_order_relaxed));
    }
    if (slot < t.curRef) t.curRef = slot;
    t.slotMask |= 1 << slot;                             // v0.7.8: all passes of this target (round 6: the tile layout)
    if (g_gameVpN > 0) g_pv[slot].compVp = g_gameVp[0]; // round 6: where the game's composite draws it (layout log)
    t.pending = true;
    g_ptPendingMask |= 1 << idx;
    g_ptBoundIdx = idx;                                  // this target is the current RT0 and now pending
    return true;
}
#endif

// Draw: detect the swapchain / eye blit and run DLAA on its source texture before it executes.
// v0.7.0: in upscale mode the DLSS result is drawn into the blit RT right AFTER the game's blit Draw.
void STDMETHODCALLTYPE hkDraw(ID3D11DeviceContext* ctx, UINT vertexCount, UINT startVertex) {
    TraceDraw(ctx, vertexCount, startVertex);
    if (!IsGameCtx(ctx)) {                         // v0.6.2: other contexts are counted / sanity-logged only
        NoteForeignDraw(ctx, vertexCount);
        oDraw(ctx, vertexCount, startVertex);
        return;
    }
    CountGameDraw(t_inDlaa, ctx, vertexCount, false);   // v0.8.1: frame-end rule + present-layer bookkeeping
#ifdef WITH_DLAA
    // v0.7.8 profile-screen preview: a matched composite only marks its target pending; any other Draw while a pending
    // target is the current RT0 (trigger (a): flat UI, VR verts=96 Draw) runs that target's DLAA before the Draw.
    {
        const bool pvMatched = vertexCount == 3 && g_pvCount > 0 && !t_inDlaa && PreviewComposite(ctx);
        if (!pvMatched && g_ptBoundIdx >= 0 && !t_inDlaa) PreviewFlush(ctx, g_ptBoundIdx);
    }
#endif
    if (vertexCount - 3u <= 1u && !t_inDlaa && g_sceneDepth && g_backbuffer.load(std::memory_order_relaxed)) {
        HandlePossibleBlit(ctx, vertexCount);
    }
    oDraw(ctx, vertexCount, startVertex);
#ifdef WITH_DLAA
    if (g_compEye >= 0 && !t_inDlaa) RunPendingComposite(ctx);   // v0.7.0 (before the scene span closes)
#endif
    // v0.6.2: scene span ends after the game's Draw of the frame's last blit (never on a Draw of our own / NGX work)
    if (g_sceneEndAfterDraw && !t_inDlaa) SpanAfterDraw(ctx);
}

#ifdef WITH_DLAA
// v0.7.8: NGX pre-warm at the first top-level Present with DLAA on (normally Present #0, the boot screen, where the
// one-time ~0.7 s of NGX init + first feature create is invisible; it used to land on the first DLAA unit, i.e. the
// VR main menu). Render thread, the game's immediate context, never after a game device change. See
// DlaaProcessor::Prewarm. DLAA off at boot: done at the first Present after it is switched on.
// v0.8.1: while the Presents come through a known present layer and no game context was adopted yet, the game context
// is the layer's presenting device, not the game's: NGX would be initialised (and pinned) on the wrong device. The
// pre-warm then waits until the adoption (it runs at the first game-thread frame boundary after it) or until the
// presenting context renders a scene pass itself (= it IS the game's).
void MaybePrewarmNgx() {
    if (g_prewarmDone.load(std::memory_order_acquire) || !g_dlaaOn.load(std::memory_order_relaxed) ||
        g_gameDeviceChanged.load(std::memory_order_relaxed)) return;
    ID3D11DeviceContext* const c = g_gameCtx.load(std::memory_order_acquire);
    if (!c) return;
    if (g_plSuspect.load(std::memory_order_relaxed) && !g_plActive.load(std::memory_order_relaxed) && g_passSeq == 0) {
        static bool deferLogged = false;
        if (!deferLogged) {
            deferLogged = true;
            Log("DLAA: NGX pre-warm deferred -- the Presents come through the present layer %s, whose device is not the "
                "game's render device; NGX is initialised on the game device: early, on the game's render thread at its "
                "first draws (MaybeEarlyPrewarm), else once its render context is adopted",
                g_plModule[0] ? g_plModule : "?");
        }
        return;
    }
    g_prewarmDone.store(true, std::memory_order_release);
    t_inDlaa = true;                                   // NGX's own context calls pass straight through our hooks
    SceneDlaa::PrewarmNgx(c);
    t_inDlaa = false;
}
#endif

// Top-level Present boundary (called from hkPresent before oPresent). v0.5.2: nothing here drives frame logic
// any more (passes/blits are matched by the FIFO); logging only.
void LogFrameEndCheck(uint64_t n);      // v0.8.1, defined with the frame-end rule
void OnPresentBoundary(uint64_t n) {
#ifdef WITH_DLAA
    PreviewNextFrame(n);                               // v0.7.8 profile-screen preview: log, reset slots, next jitter
    DlaaProcessor::BeginFrame();                       // v0.7.8: a fresh NGX feature-create budget for the next frame
#endif
    // v0.7.10: also every 10 s (g_logTick, the Present line's cadence) so a single periodic line shows whether the scene /
    // blits ever started and the live DLAA state. mode = the detection (VR/flat/undetected); dlaa / dlaa-mode = the
    // live DLAA switch + its mode; ngx-feature = eye-0's NGX feature exists (-1 = logging-only build).
    if (n < 6 || g_logTick) {
#ifdef WITH_DLAA
        const int ngxFeature = (int)g_dlaa[0].FeatureReady();
#else
        const int ngxFeature = -1;
#endif
        // v0.8.0: " | hdr-out=1 hdr-blits=N" appended only while the output is HDR (the 8-bit line is unchanged).
        char hdrTail[64] = "";
        if (g_hdrOut) snprintf(hdrTail, sizeof(hdrTail), " | hdr-out=1 hdr-blits=%llu", (unsigned long long)g_hdrBlitCount);
        // v0.8.1: " | present-layer: ..." appended only in present-layer mode (n = game frames, see LayerFrameBoundary).
        char plTail[192] = "";
        if (g_plActive.load(std::memory_order_relaxed))
            snprintf(plTail, sizeof(plTail), " | present-layer: game frames counted on the game thread (frame-end=%llu "
                     "scene-clear=%llu idle=%llu), layer presents=%llu, backbuffer %ux%u fmt=%d",
                     (unsigned long long)g_plBoundRule, (unsigned long long)g_plBoundFallback,
                     (unsigned long long)g_plBoundIdle, (unsigned long long)g_plPresents.load(std::memory_order_relaxed),
                     g_bbW, g_bbH, (int)g_bbFmt);
        Log("Present #%llu: passes=%llu blits=%llu outstanding=%d mode=%s eye-map-entries=%d | dlaa=%s dlaa-mode=%s ngx-feature=%d%s%s",
            (unsigned long long)n, (unsigned long long)g_passSeq, (unsigned long long)g_blitCount,
            (int)(g_passSeq - g_fifoHead), g_vrMode ? "VR" : (g_flatMode ? "flat" : "undetected"), g_eyeMapN,
            g_dlaaOn.load(std::memory_order_relaxed) ? "on" : "off", DlaaModeStr(), ngxFeature, hdrTail, plTail);
        LogFrameEndCheck(n);                           // v0.8.1: does the frame-end rule fire once per frame?
    }

    // v0.7.10: one-shot triage warnings when the 3D scene never showed up. After ~1800 Presents AND ~30 s wall-clock
    // from the first Present: warn once if no scene pass has started, or (separately) if passes started but no blit
    // was ever matched. The first-time lines (first scene pass / first blit) make a later recovery visible, so these
    // are never repeated. Per-frame cost is an integer compare until the window opens; Qpc only runs in that window.
    static int64_t s_firstPresentQpc = 0;
    if (n == 0) s_firstPresentQpc = Qpc();
    static bool s_warnedNoPass = false, s_warnedNoBlit = false;
    if (n >= 1800 && (!s_warnedNoPass || !s_warnedNoBlit) && g_qpcFreq > 0 && s_firstPresentQpc != 0 &&
        (double)(Qpc() - s_firstPresentQpc) / (double)g_qpcFreq >= 30.0) {
        if (!s_warnedNoPass && g_passSeq == 0) {
            s_warnedNoPass = true;
            Log("WARNING: no scene pass seen after %llu Presents / ~30 s -- the 3D scene render has not been "
                "recognised. Likely still in the menu / a loading screen (load a save and drive), or an unsupported "
                "game version / render path, or another injector or driver layer owns the device.%s",
                (unsigned long long)n,
                (g_plSuspect.load(std::memory_order_relaxed) && !g_plActive.load(std::memory_order_relaxed))   // v0.8.1
                    ? " The Presents come through a present layer and no foreign scene render was adopted yet (see the "
                      "'present layer' / 'G-buffer bind on a non-game context' lines; none = no 3D scene rendered yet)."
                    : "");
        } else if (!s_warnedNoBlit && g_passSeq >= 600 && g_blitCount == 0) {   // 600 passes: the first blit follows the first pass by a frame or two
            s_warnedNoBlit = true;
            // v0.8.0: + backbuffer format and scene / backbuffer sizes and the HDR rule's state, so an unsupported
            // pipeline can be identified from the log alone.
            Log("WARNING: %llu scene pass(es) seen but no blit matched after %llu Presents / ~30 s -- the scene "
                "renders but the final scene->screen copy we hook was not recognised (unsupported render path, or "
                "another layer intercepting the blit). DLAA/DLSS cannot run without a matched blit. | backbuffer %ux%u "
                "fmt=%d (%s), scene %ux%u, launch mode %s | HDR rule: forward colour %s, HDR composite %s",
                (unsigned long long)g_passSeq, (unsigned long long)n, g_bbW, g_bbH, (int)g_bbFmt,
                g_hdrOut ? "HDR output: HDR blit rule active" : "8-bit output: 8-bit blit rule only", g_sceneW, g_sceneH,
                g_launchVr == 1 ? "VR" : (g_launchVr == 0 ? "flat" : "unknown"),
                g_fwdColorTex ? "seen" : "not seen", g_hdrCompTex ? "seen" : "not seen");
        }
    }
}

#ifdef WITH_DLAA
// ---- motion-vector candidate collection (G-buffer pass only) ------------------------
// The game binds a window of one big dynamic ring buffer as VS cbuffer slot 0 (D3D11.1
// offsets); float4 rows 4..7 of the window are the draw's MVP. For the first N draws per
// depth layer we GPU-copy those 64 bytes into CameraMv's candidate buffer and remember a
// geometry key; the blit-time pass pairs equal keys across two frames (motion_vectors.cpp).
ID3D11DeviceContext*   g_ctx1Owner = nullptr;       // context g_ctx1 was queried from (compare only)
ID3D11DeviceContext1*  g_ctx1 = nullptr;            // cached QI (ref held for the process life)

// v0.6.3 stable thinning (dlaa.ini, read once in LoadConfig; see CollectMvCandidate):
int g_mvSampleShift = 2;                            // 0..4; a draw is a candidate only when (hash & ((1<<s)-1)) == 0 (0 = every draw)
int g_mvWorldSlots  = 40;                           // world (layer 0) cap, 1..CandidateRecord::kSlots
int g_mvCabinSlots  = 24;                           // cabin (layer 1) cap, 1..CandidateRecord::kSlots

// v0.6.1: `ps` = the newest pass slot (caller guarantees g_passSeq > 0); every early return counts its reason.
// v0.7.8: `worldCap` > 0 overrides the world-layer cap (the preview's non-reference tile passes only need their FIRST
// candidate: the tile-layout readback compares it with the reference pass's first one).
void CollectMvCandidate(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex, PassSlot& ps,
                        int worldCap = 0) {
    // v0.5.4: candidates go to the record of the NEWEST pass (the one being rendered); the eye is only known
    // at the blit, where the record is matched/committed against that eye's CameraMv.
    CandidateRecord& mv = ps.cand;
    // v0.6.3 stable sampling: decide from the draw's OWN parameters, before any D3D call, so the SAME draws are
    // sampled every frame (no draw order / counters -> stable frame-to-frame pairing). Cheap integer hash of
    // (indexCount, startIndex, baseVertex) with a murmur3-style finaliser (good low bits); a draw is a candidate
    // only when (hash & mask) == 0. mv_sample_shift 0 -> mask 0 -> every draw passes (old behaviour).
    uint32_t h = (uint32_t)indexCount;
    h = h * 0x85ebca6bu + (uint32_t)startIndex;
    h = h * 0xc2b2ae35u + (uint32_t)baseVertex;
    h ^= h >> 16; h *= 0x85ebca6bu; h ^= h >> 13; h *= 0xc2b2ae35u; h ^= h >> 16;
    const uint32_t mask = (1u << g_mvSampleShift) - 1u;
    if ((h & mask) != 0u) { ++ps.skSampled; return; }

    if (!mv.Ready()) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (dev) { mv.Init(dev); dev->Release(); }
        if (!mv.Ready()) { ++ps.skNoCtx; return; }
    }
    if (g_ctx1Owner != ctx) {
        if (g_ctx1) { g_ctx1->Release(); g_ctx1 = nullptr; }
        ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&g_ctx1);
        g_ctx1Owner = ctx;
    }
    if (!g_ctx1 || g_gameVpN == 0) { ++ps.skNoCtx; return; }
    const int layer = g_gameVp[0].MinDepth >= 0.85f ? 1 : 0;     // cabin [0.9,1.0] vs world [0.01,0.9]
    // v0.6.3: per-layer caps replace the kSlots (128) LayerFull limit (both clamped 1..kSlots in LoadConfig).
    if (mv.Count(layer) >= (layer == 0 ? (worldCap > 0 ? worldCap : g_mvWorldSlots) : g_mvCabinSlots)) { ++ps.skFull; return; }

    ID3D11Buffer* cb = nullptr;
    UINT first = 0, num = 0;
    g_ctx1->VSGetConstantBuffers1(0, 1, &cb, &first, &num);
    if (!cb) { ++ps.skNoCb; return; }
    const UINT byteOff = first * 16 + 64;
    D3D11_BUFFER_DESC bd{};
    cb->GetDesc(&bd);
    if (num * 16 >= 128 && byteOff + 64 <= bd.ByteWidth) {
        CandidateRecord::DrawKey key{};
        ID3D11Buffer* ib = nullptr; DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN; UINT ibOff = 0;
        ID3D11Buffer* vb = nullptr; UINT vbStride = 0, vbOff = 0;
        ctx->IAGetIndexBuffer(&ib, &ibFmt, &ibOff);
        ctx->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOff);
        key.ib = ib; key.vb = vb; key.ibOffset = ibOff; key.vbOffset = vbOff;
        key.indexCount = indexCount; key.startIndex = startIndex; key.baseVertex = baseVertex;
        if (ib) ib->Release();
        if (vb) vb->Release();
        mv.Record(ctx, layer, cb, byteOff, key);
        ++g_cMvRecords;                                          // v0.5.6 stats
    } else {
        ++ps.skRange;
    }
    cb->Release();
}

#endif

void STDMETHODCALLTYPE hkDrawIndexed(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex) {
    TraceDrawIndexed(ctx, indexCount);
    // v0.6.1: every call is counted on the newest pass (flight recorder).
    // v0.6.2: only the game context is tracked (identity compare, no GetType); for any other context the type is
    // queried once: deferred -> g_diDeferred, other immediate -> g_foreignCtx (once the game context is known).
    const bool gameCtx = IsGameCtx(ctx);
    if (!gameCtx) {
        if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
            g_diDeferred.fetch_add(1, std::memory_order_relaxed);   // any thread
        else if (g_gameCtx.load(std::memory_order_relaxed))
            g_foreignCtx.fetch_add(1, std::memory_order_relaxed);
    } else {
        CountGameDraw(t_inDlaa, ctx, 0, true);     // v0.8.1: frame-end rule + present-layer bookkeeping
    }
    if (gameCtx && g_passSeq) {
        PassSlot& ps = g_ring[(g_passSeq - 1) % kRing];
        if (!g_gbufPass) {
            ++ps.diOther;
        } else {
            ++ps.diGbuf;
#ifdef WITH_DLAA
            // v0.5.5: VR collects candidates even with MV off (the projection eye verdict reads them).
            if (t_inDlaa) {
                ++ps.skInDlaa;
            } else if (!g_dlaaOn.load(std::memory_order_relaxed) ||
                       !(g_mvOn.load(std::memory_order_relaxed) || g_vrMode) ||
                       g_passive.load(std::memory_order_relaxed)) {   // v0.6.2 passive: no candidate copies
                ++ps.skGate;
            } else {
                const int64_t t0 = Qpc();                        // v0.5.6: CPU cost of the collector
                CollectMvCandidate(ctx, indexCount, startIndex, baseVertex, ps);
                g_cpuMvTicks += Qpc() - t0;
            }
#endif
        }
    }
#ifdef WITH_DLAA
    // v0.7.8 profile-screen preview: the truck's draws feed the current slot's own candidate record (same
    // CollectMvCandidate, same gates as the world path; the viewport MinDepth 0.01 makes it the world layer).
    // v0.7.8 round 4: only for the learned reference slots (the others' candidates are never used).
    // Round 5: VR collects them even with MV off (the preview eye verdict reads them), like the world path.
    // Ctrl+F10 preview capture: every slot while it runs (it compares the passes' projections; nothing else uses them).
    // Round 6 (tiled pictures): every pass collects -- the reference slot its full record, the other passes their FIRST
    // candidate only (the tile-layout readback compares it with the reference pass's first one, PvLayout). Independent
    // of the MV key (the layout is needed for the depth too).
    // v0.8.1 round 5: the other passes collect their first kPvLayCands (16) -- the layout pairs candidates of the same
    // instance, the first one of a tile is often another instance of the reference's first mesh (see PvLayout).
    if (g_previewPass && g_pvCur >= 0 && !t_inDlaa && IsGameCtx(ctx)) {
        ++g_pv[g_pvCur].diCount;                                  // round 6 diagnostics (no-candidate report)
        if (g_dlaaOn.load(std::memory_order_relaxed) && !g_passive.load(std::memory_order_relaxed)) {
            const bool full = ((g_ptRefMask >> g_pvCur) & 1) || g_pvCapActive;
            CollectMvCandidate(ctx, indexCount, startIndex, baseVertex, g_pv[g_pvCur].ps, full ? 0 : kPvLayCands);
        }
    }
    // Trigger (a): a DrawIndexed (flat: the UI) while a pending composite target is the current RT0 runs its DLAA first.
    if (g_ptBoundIdx >= 0 && !t_inDlaa && IsGameCtx(ctx)) PreviewFlush(ctx, g_ptBoundIdx);
#endif
    oDrawIndexed(ctx, indexCount, startIndex, baseVertex);
}

// ---- v0.6.2 game render context (hkPresent, depth 0) -------------------------------------------------------------
void*            g_gameDev       = nullptr;      // device of g_gameCtx (identity; Present thread only; v0.8.1 + the adoption, under g_plMx)
void*            g_candDev       = nullptr;      // another device that is presenting (identity)
int              g_candPresents  = 0;            // ... consecutive top-level Presents from it
uint32_t         g_cCandLogs     = 0;            // "Present from another device" lines (cap 5)
uint32_t         g_cCtxChanges   = 0;            // "game render context changed" lines (cap 5)
constexpr int    kCtxSwitchPresents = 3;         // a new device is adopted after this many consecutive Presents

// A switch to a new game context = back to the state of a fresh start (nothing tracked until the new context's
// first G-buffer bind). Runs on the thread that presents the NEW device (= its render thread). Our device-bound
// resources (NGX features, SceneDlaa / pass twins / candidate buffers, eye-verdict staging ring, self-test
// staging) belong to the OLD device and are not recreated: DLAA is forced OFF for the rest of the session.
// v0.8.1: split -- ResetPassTrackingCore is the tracking reset alone (also used by the present-layer adoption, where
// nothing was created on the old device and DLAA stays as it is); ResetPassTracking adds the forced OFF + its line.
void ResetPassTrackingCore(bool forceDlaaOff);
void ResetPassTracking(uint64_t n) {
    ResetPassTrackingCore(true);
    Log("game context change: pass tracking reset (scene depth, G-buffer/jitter pass flags, viewports, FIFO head -> "
        "%llu, first-pass detection, flat/VR mode, eye map, MV history); DLAA forced OFF (Present #%llu)",
        (unsigned long long)g_passSeq, (unsigned long long)n);
}
void ResetPassTrackingCore(bool forceDlaaOff) {
    if (g_sceneDepth) { g_sceneDepth->Release(); g_sceneDepth = nullptr; }   // rediscovered by InspectDepth
    g_sceneW = g_sceneH = 0;
    g_tonemapTex = nullptr;
    g_fwdColorTex = nullptr; g_hdrCompTex = nullptr; g_hdrCompLogged = false;   // v0.8.0 HDR blit rule
    g_gbufPass = false; g_jitterPass = false; g_depthDirty = false;
    g_gameVpN = 0; g_vpShifted = false; g_vpShiftX = g_vpShiftY = 0.0f;
    while (g_fifoHead < g_passSeq) g_ring[g_fifoHead++ % kRing].consumed = true;   // drop outstanding passes
    g_needFirstPass = true;                                    // next G-buffer bind starts a pass
    g_flatMode = false; g_vrMode = false;                      // re-detected at the next matched blit
    g_eyeMapN = 0; g_eyeMapLogged = false; g_prevBlitRt = nullptr; g_lastUnderflowBlit = 0;
    for (EyeMapEntry& e : g_eyeMap) e.score = 0;
    for (bool& b : g_eyeLogged) b = false;
    g_tonemapLogged = false; g_passLogged = 0;                 // detection lines are logged again
    g_renderTid.store(0, std::memory_order_relaxed);
    if (forceDlaaOff) {
        g_dlaaOn = false;
        g_gameDeviceChanged = true;
    }
    g_resetNext = true;
    MvInvalidate();
    SpanReleaseAll();
#ifdef WITH_DLAA
    // Eye-verdict staging ring is old-device (v0.8.1 adoption: never created there -- passes=0 blits=0 -- so it stays
    // untried and is created on the adopted device at its first use).
    if (forceDlaaOff) { g_rbInitTried = true; g_rbOk = false; g_rbPending = 0; }
    if (g_ctx1) { g_ctx1->Release(); g_ctx1 = nullptr; }
    g_ctx1Owner = nullptr;
    if (g_staging) { g_staging->Release(); g_staging = nullptr; g_stagW = g_stagH = 0; g_stagFmt = DXGI_FORMAT_UNKNOWN; }
#endif
}

// Called by hkPresent at depth 0 before anything else: the presenting device's immediate context is the game's
// render context. Identity only: every ref taken here is released before returning.
// v0.8.1: `caller` = the Present's return address. Returns true when a known present layer (IsPresentLayerName)
// presents ANOTHER device while a game context is known: that device is never adopted (the layer would take the place
// of the game's context and every game call would be filtered out); present-layer mode starts instead, the game context
// stays, and the caller skips this Present's per-frame work (it moves to the game render thread).
bool UpdateGameContext(IDXGISwapChain* sc, uint64_t n, void* caller) {
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) || !dev) return false;   // not a D3D11 swapchain
    if ((void*)dev == g_gameDev) {
        g_candDev = nullptr; g_candPresents = 0;
        dev->Release();
        return false;
    }
    if (g_gameDev) {
        char mod[MAX_PATH];
        if (ModuleBaseOf(caller, mod, sizeof(mod)) && IsPresentLayerName(mod)) {   // v0.8.1, see above
            g_plSuspect.store(true, std::memory_order_relaxed);
            strncpy_s(g_plModule, mod, _TRUNCATE);
            g_plActiveQpc.store(Qpc(), std::memory_order_relaxed);
            // The game render thread: the thread of the last matched blit if there was one, else claimed by the first
            // game-context bind from a thread other than this (the layer's) one (FrameEndOnBind).
            g_plGameTid.store(g_renderTid.load(std::memory_order_relaxed), std::memory_order_relaxed);
            g_plActive.store(true, std::memory_order_release);   // caller holds g_plMx: ordered against the adoption
            Log("present layer appeared: Present #%llu from another D3D11 device %p through %s (tid=%lu) while the game "
                "render context %p (device %p) is known -- that device is NOT adopted (it only presents); present-layer "
                "mode: the game context stays, the per-frame work moves to the game render thread (tid=%lu, 0 = claimed "
                "at its next bind; frame-end rule), this thread only counts its Presents", (unsigned long long)n, (void*)dev,
                mod, GetCurrentThreadId(), (void*)g_gameCtx.load(std::memory_order_relaxed), g_gameDev,
                g_plGameTid.load(std::memory_order_relaxed));
            dev->Release();
            return true;
        }
    }
    if (g_gameDev) {                                           // another device presents: adopt only if it persists
        if ((void*)dev != g_candDev) {
            g_candDev = dev; g_candPresents = 0;
            if (g_cCandLogs < 5) {
                ++g_cCandLogs;
                Log("Present #%llu from another D3D11 device %p (game device %p, tid=%lu) -- adopted only after %d "
                    "consecutive top-level Presents", (unsigned long long)n, (void*)dev, g_gameDev,
                    GetCurrentThreadId(), kCtxSwitchPresents);
            }
        }
        if (++g_candPresents < kCtxSwitchPresents) { dev->Release(); return false; }
    }
    ID3D11DeviceContext* ic = nullptr;
    dev->GetImmediateContext(&ic);
    if (!ic) { dev->Release(); return false; }
    ID3D11DeviceContext1* ic1 = nullptr;
    ic->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ic1);   // once per change
    ID3D11DeviceContext* const old = g_gameCtx.load(std::memory_order_relaxed);
    const bool first = g_gameDev == nullptr;
    g_gameCtx1.store((void*)ic1, std::memory_order_release);
    g_gameCtx.store(ic, std::memory_order_release);
    void* const oldDev = g_gameDev;
    g_gameDev = dev;
    g_candDev = nullptr; g_candPresents = 0;
    if (first) {
        Log("game render context = %p (device %p, ID3D11DeviceContext1 %p, Present #%llu tid=%lu)",
            (void*)ic, (void*)dev, (void*)ic1, (unsigned long long)n, GetCurrentThreadId());
    } else {
        if (g_cCtxChanges < 5) {
            ++g_cCtxChanges;
            Log("game render context changed %p -> %p (device %p -> %p, ID3D11DeviceContext1 %p, Present #%llu tid=%lu)",
                (void*)old, (void*)ic, oldDev, (void*)dev, (void*)ic1, (unsigned long long)n, GetCurrentThreadId());
        }
        ResetPassTracking(n);
    }
    if (ic1) ic1->Release();
    ic->Release();
    dev->Release();
    return false;
}

// ---- v0.8.1 present layer: the game's render context from the scene render -----------------------------------------
// Evidence (foreign immediate contexts, any thread, under g_plEvMx): scene binds (NoteForeignOM) on ONE context. Ready
// at >= kPlMinBinds binds over >= kPlMinSecs and >= kPlMinPresents top-level Presents since its first bind. Then the
// presenting side must look like a present layer: its context rendered no scene pass and matched no blit, the last
// top-level Present came from another thread and not from the game exe. Otherwise ONE warning names the reason (the
// conditions are re-checked every 64 binds; a later adoption is still possible).
struct PlEvidence {
    ID3D11DeviceContext* ctx = nullptr;  // candidate (identity only)
    unsigned long tid = 0;               // thread of its first bind
    uint32_t gbuf = 0, pv = 0;           // G-buffer / preview-pass-shape binds
    uint32_t otherTid = 0;               // ... of them from another thread than `tid`
    int64_t  t0 = 0;                     // QPC at its first bind
    uint64_t presents0 = 0;              // g_frames (top-level Presents so far) at its first bind
    UINT     dw = 0, dh = 0;             // scene depth size of the last bind (log)
};
std::mutex         g_plEvMx;
PlEvidence         g_plEv;
constexpr uint32_t kPlMinBinds    = 60;
constexpr double   kPlMinSecs     = 1.0;
constexpr uint64_t kPlMinPresents = 30;
// Game render thread from here on (present-layer mode):
int64_t            g_plLastRuleQpc     = 0;      // QPC of the last frame boundary from the frame-end rule (0 = none yet)
int64_t            g_plLastBoundaryQpc = 0;      // QPC of the last frame boundary of any kind
int                g_plFallbackKind    = -1;     // fallback kind driving the frames right now (-1 = none / the rule)

void AdoptFromPresentLayer(ID3D11DeviceContext* ctx, const PlEvidence& ev, const char* pmod, unsigned long ptid);

void PlNoteEvidence(ID3D11DeviceContext* ctx, int kind, UINT dw, UINT dh) {
    PlEvidence ev;
    const unsigned long tid = GetCurrentThreadId();
    {
        std::lock_guard<std::mutex> lk(g_plEvMx);
        if (g_plAdopted.load(std::memory_order_relaxed)) return;
        if (g_plEv.ctx != ctx) {                         // a new candidate: the evidence starts over
            g_plEv = PlEvidence();
            g_plEv.ctx = ctx; g_plEv.tid = tid; g_plEv.t0 = Qpc();
            g_plEv.presents0 = g_frames.load(std::memory_order_relaxed);
        }
        if (kind == 1) ++g_plEv.gbuf; else ++g_plEv.pv;
        if (tid != g_plEv.tid) ++g_plEv.otherTid;
        g_plEv.dw = dw; g_plEv.dh = dh;
        const uint32_t binds = g_plEv.gbuf + g_plEv.pv;
        if (binds < kPlMinBinds) return;
        if (g_plBlockLogged.load(std::memory_order_relaxed) && (binds & 63u) != 0) return;
        if (g_qpcFreq <= 0 || (double)(Qpc() - g_plEv.t0) / (double)g_qpcFreq < kPlMinSecs) return;
        if (g_frames.load(std::memory_order_relaxed) - g_plEv.presents0 < kPlMinPresents) return;
        ev = g_plEv;
    }
    // The presenting side. g_passSeq / g_blitCount are written by the presenting context's hooks (other thread): plain
    // aligned 64-bit reads, only tested for 0 (re-tested under g_plMx in the adoption).
    const uint64_t passes = g_passSeq, blits = g_blitCount;
    const unsigned long ptid = g_plPresentTid.load(std::memory_order_relaxed);
    char pmod[MAX_PATH];
    ModuleBaseOf(g_plPresentCaller.load(std::memory_order_relaxed), pmod, sizeof(pmod));
    const char* block = nullptr;
    if (passes || blits)                     block = "the presenting context renders scene passes / blits itself";
    else if (ptid == tid)                    block = "the top-level Presents come from the scene's own render thread";
    else if (!_stricmp(pmod, GameExeBase())) block = "the top-level Presents are made by the game exe itself";
    if (block) {
        if (!g_plBlockLogged.exchange(true))
            Log("WARNING: present layer: adoption evidence seen but adoption BLOCKED -- %s | candidate context %p (tid=%lu): "
                "%u G-buffer + %u preview-pass binds (depth %ux%u) over %.1f s / %llu Presents; presenting context %p "
                "passes=%llu blits=%llu, last top-level Present from %s tid=%lu -- the game context is left as it is",
                block, (void*)ctx, tid, ev.gbuf, ev.pv, ev.dw, ev.dh,
                g_qpcFreq > 0 ? (double)(Qpc() - ev.t0) / (double)g_qpcFreq : 0.0,
                (unsigned long long)(g_frames.load(std::memory_order_relaxed) - ev.presents0),
                (void*)g_gameCtx.load(std::memory_order_relaxed), (unsigned long long)passes, (unsigned long long)blits,
                pmod[0] ? pmod : "?", ptid);
        return;
    }
    AdoptFromPresentLayer(ctx, ev, pmod, ptid);
}

// ---- v0.8.1 round 3: EARLY NGX pre-warm under a present layer known by name ------------------------------------------
// The round-2 test (ETS2, Smooth Motion): the pre-warm waited for the adoption, and the adoption needs scene / preview
// binds, so its ~0.6 s landed exactly when the menu truck first appeared (a visible jerk). Normal mode runs it on the
// boot screen. Here: before the adoption, the game's own D3D11 calls already arrive on a foreign immediate context; the
// first one that qualifies hosts the pre-warm, on its own thread, inside its OMSetRenderTargets (CS state saved /
// restored by SceneDlaa::PrewarmNgx, t_inDlaa set so NGX's calls pass straight through; no D3DCompile: NGX only).
// Qualifies = a present layer known by name (g_plSuspect), not adopted yet, DLAA on, no pre-warm yet; an IMMEDIATE
// context that is not the presenting one, of a device that is not the presenting one; the call comes from code INSIDE
// the game exe image (the engine's renderer -- keeps the layer's / overlays' own devices out); the thread is not the
// layer's present thread; and kEpMinBinds such binds on the same context + thread in a row. A wrong guess is caught at
// the adoption (WARNING, NgxAcquire then moves NGX). Without a qualifying call the post-adoption pre-warm stays.
#ifdef WITH_DLAA
std::mutex           g_epMx;                     // candidate bookkeeping (foreign calls: any thread)
ID3D11DeviceContext* g_epCand    = nullptr;      // candidate context (identity)
unsigned long        g_epCandTid = 0;            // ... and its thread
uint32_t             g_epBinds   = 0;            // qualifying binds in a row on it
std::atomic<bool>    g_epClaimed{false};         // one attempt per process
std::atomic<bool>    g_epDone{false};            // the early pre-warm ran (g_ep* below are valid; release / acquire)
void*                g_epDev = nullptr;          // its device / context / thread (log, adoption check)
void*                g_epCtx = nullptr;
unsigned long        g_epTid = 0;
constexpr uint32_t   kEpMinBinds = 10;

// The return address lies inside the game exe's image (its renderer issues the D3D11 calls).
bool InGameExe(void* addr) {
    static std::atomic<uintptr_t> s_base{0}, s_end{0};
    uintptr_t base = s_base.load(std::memory_order_acquire);
    if (!base) {
        HMODULE m = GetModuleHandleW(nullptr);
        if (!m) return false;
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)m;
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const uint8_t*)m + dos->e_lfanew);
        s_end.store((uintptr_t)m + nt->OptionalHeader.SizeOfImage, std::memory_order_relaxed);
        s_base.store((uintptr_t)m, std::memory_order_release);
        base = (uintptr_t)m;
    }
    const uintptr_t a = (uintptr_t)addr;
    return a >= base && a < s_end.load(std::memory_order_relaxed);
}

// hkOMSetRenderTargets, foreign-context branch (before the game's bind goes through).
void MaybeEarlyPrewarm(ID3D11DeviceContext* ctx, void* caller) {
    if (g_epClaimed.load(std::memory_order_relaxed) || t_inDlaa) return;
    if (!g_plSuspect.load(std::memory_order_relaxed) || g_plActive.load(std::memory_order_relaxed) ||
        g_plAdopted.load(std::memory_order_relaxed)) return;
    if (!g_dlaaOn.load(std::memory_order_relaxed) || g_gameDeviceChanged.load(std::memory_order_relaxed) ||
        g_prewarmDone.load(std::memory_order_acquire)) return;
    ID3D11DeviceContext* const pres = g_gameCtx.load(std::memory_order_acquire);   // the presenting device's context
    if (!pres || ctx == pres || !InGameExe(caller)) return;
    const unsigned long tid = GetCurrentThreadId();
    if (tid == g_plPresentTid.load(std::memory_order_relaxed)) return;
    if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return;
    uint32_t binds;
    {
        std::lock_guard<std::mutex> lk(g_epMx);
        if (ctx != g_epCand || tid != g_epCandTid) { g_epCand = ctx; g_epCandTid = tid; g_epBinds = 0; }
        binds = ++g_epBinds;
    }
    if (binds < kEpMinBinds) return;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    void* const presDev = g_gameDev;                     // racy read (set once at Present #0): a compare only
    if ((void*)dev == presDev || g_epClaimed.exchange(true)) { dev->Release(); return; }
    char where[MAX_PATH + 32];
    DescribeAddr(caller, where, sizeof(where));
    const int64_t t0 = Qpc();
    t_inDlaa = true;                                     // NGX's own context calls pass straight through our hooks
    const bool ok = SceneDlaa::PrewarmNgx(ctx);          // CS state saved / restored; NGX init + throwaway feature
    t_inDlaa = false;
    const double ms = g_qpcFreq > 0 ? (double)(Qpc() - t0) * 1000.0 / (double)g_qpcFreq : 0.0;
    g_epDev = (void*)dev; g_epCtx = (void*)ctx; g_epTid = tid;
    g_prewarmDone.store(true, std::memory_order_release);   // MaybePrewarmNgx (Present / boundary) does not run it again
    g_epDone.store(true, std::memory_order_release);
    Log("DLAA: EARLY NGX pre-warm (present layer %s) ran before the adoption on the game's render thread: context %p, "
        "device %p, tid=%lu, %.1f ms (%s) -- picked as an immediate context that is not the presenting one (%p, device "
        "%p), %u binds in a row called from %s, not on the layer's present thread tid=%lu (Present #%llu)",
        g_plModule[0] ? g_plModule : "?", (void*)ctx, (void*)dev, tid, ms,
        ok ? "NGX pinned on this device" : "FAILED, see the line above -- units initialise NGX themselves",
        (void*)pres, presDev, binds, where, g_plPresentTid.load(std::memory_order_relaxed),
        (unsigned long long)g_frames.load(std::memory_order_relaxed));
    dev->Release();                                      // identity only (the game holds its device)
}
#else
void MaybeEarlyPrewarm(ID3D11DeviceContext*, void*) {}
#endif

// The adoption, on the scene's render thread inside its OMSetRenderTargets (the bind itself still passes through as
// foreign; the next one is a game-context bind). try_lock: the game thread never waits on the presenting thread (a busy
// lock = retried at the next scene bind). Nothing was created on the presenting device (passes=0 blits=0 re-checked
// under the lock), so this is the tracking reset only: DLAA keeps its state, no "restart the game". The NGX pre-warm, if
// it already ran on the presenting device (a layer not recognised by name), is moved by NgxAcquire at the first unit.
void AdoptFromPresentLayer(ID3D11DeviceContext* ctx, const PlEvidence& ev, const char* pmod, unsigned long ptid) {
    std::unique_lock<std::mutex> lk(g_plMx, std::try_to_lock);
    if (!lk.owns_lock() || g_plAdopted.load(std::memory_order_relaxed)) return;
    if (g_passSeq || g_blitCount) return;                // the presenting context started rendering the scene meanwhile
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return;
    ID3D11DeviceContext1* ic1 = nullptr;
    ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ic1);
    ID3D11DeviceContext* const oldCtx = g_gameCtx.load(std::memory_order_relaxed);
    void* const oldDev = g_gameDev;
    const uint64_t n = g_frames.load(std::memory_order_relaxed);
    const unsigned long tid = GetCurrentThreadId();
    // The presenting swapchain's desc as the normal path last saw it (until LayerPresent refreshes it).
    if (g_bbW) g_plScDesc.store(PlPackDesc(g_bbW, g_bbH, g_bbFmt), std::memory_order_relaxed);
    ResetPassTrackingCore(false);
    g_gameCtx1.store((void*)ic1, std::memory_order_release);
    g_gameCtx.store(ctx, std::memory_order_release);
    g_gameDev = dev;
    g_candDev = nullptr; g_candPresents = 0;
    if (!g_plModule[0]) strncpy_s(g_plModule, pmod, _TRUNCATE);
    const int64_t now = Qpc();
    g_plLastRuleQpc = 0; g_plLastBoundaryQpc = 0; g_plFallbackKind = -1; g_plRuleDriving = false;
    g_plDraws = 0;
    g_plGameTid.store(tid, std::memory_order_relaxed);   // every frame boundary runs on this thread from now on
    FrameEndReset();                                     // the rule's state was the presenting context's until now
    g_plActiveQpc.store(now, std::memory_order_relaxed);
    g_plAdopted.store(true, std::memory_order_relaxed);
    g_plActive.store(true, std::memory_order_release);   // hkPresent: count + log only from now on
    const double secs = g_qpcFreq > 0 ? (double)(now - ev.t0) / (double)g_qpcFreq : 0.0;
    Log("present layer detected: top-level Presents come from %s (tid=%lu; presenting device %p, swapchain %ux%u fmt=%d) "
        "while the scene renders on immediate context %p of device %p (tid=%lu): %u G-buffer + %u preview-pass binds "
        "(depth %ux%u, %u from another thread) over %.1f s and %llu Presents; the presenting context rendered passes=0 "
        "blits=0", pmod[0] ? pmod : "?", ptid, oldDev, g_bbW, g_bbH, (int)g_bbFmt, (void*)ctx, (void*)dev, tid, ev.gbuf,
        ev.pv, ev.dw, ev.dh, ev.otherTid, secs, (unsigned long long)(n - ev.presents0));
#ifdef WITH_DLAA
    // v0.8.1 round 3: is NGX already on the adopted device (the early pre-warm picked it at the boot screen)?
    void* const ngxDev = DlaaProcessor::NgxDevice();
    const bool early = g_epDone.load(std::memory_order_acquire);
    const char* ngxNote = !ngxDev ? "NGX not initialised yet: pre-warm at the first game-thread frame boundary (on the "
                                    "adopted device)"
                        : ngxDev == (void*)dev ? (early ? "NGX is ALREADY on the adopted device (early pre-warm on the "
                                                          "game's render thread, same device) -- nothing to redo"
                                                        : "NGX is already on the adopted device")
                        : "NGX is on ANOTHER device: re-initialised on the adopted device at the first DLAA unit (only "
                          "the pre-warm pin holds it), see the WARNING below";
    if (ngxDev && ngxDev != (void*)dev)
        Log("WARNING: present layer: NGX was initialised on device %p (%s), but the adopted game device is %p -- NGX moves "
            "to the adopted device at the first DLAA unit (line 'DLAA: NGX is initialised on device ... shutting NGX down "
            "on the old device'); that unit pays the NGX init (~0.6 s) once", ngxDev,
            early && g_epDev == ngxDev ? "the EARLY pre-warm's guess" : "the presenting device", (void*)dev);
#else
    const char* ngxNote = "no DLAA build";
#endif
    Log("present layer: game render context ADOPTED %p -> %p (device %p -> %p, ID3D11DeviceContext1 %p) on tid=%lu at "
        "Present #%llu -- nothing was created on the presenting device, so DLAA is NOT forced off (dlaa=%s, mode %s); the "
        "presenting device is never taken back; %s", (void*)oldCtx, (void*)ctx, oldDev, (void*)dev, (void*)ic1, tid,
        (unsigned long long)n, g_dlaaOn.load(std::memory_order_relaxed) ? "on" : "off", DlaaModeStr(), ngxNote);
    lk.unlock();
    if (ic1) ic1->Release();                             // identity only (the game holds its device / context)
    dev->Release();
    // A hook call on the old (presenting) context that passed IsGameCtx just before the switch may still be finishing
    // on the layer's thread; give it 1 ms before this thread carries on as the game context (once per process).
    Sleep(1);
}

// hkRSSetViewports, game context, 0 viewports, not our own work (see there). DIAGNOSTIC ONLY since the v0.8.1 tests: the
// D3D11 runtime issues it inside Present (caller d3d11.dll+0x196ff5 in both test logs; under a present layer it never
// reaches the game context during play, and at shutdown it came from the layer's thread). Counted; its caller is logged
// once per mode.
void OnFrameStartMarker(ID3D11DeviceContext* ctx, void* caller) {
    ++g_mkMarkers;
    const bool pl = g_plActive.load(std::memory_order_acquire);
    static std::atomic<bool> s_loggedNormal{false}, s_loggedLayer{false};
    std::atomic<bool>& logged = pl ? s_loggedLayer : s_loggedNormal;
    if (!logged.exchange(true)) {
        char where[MAX_PATH + 32];
        DescribeAddr(caller, where, sizeof(where));
        Log("frame-start marker (RSSetViewports with 0 viewports on the game context %p) first seen%s: caller=%s tid=%lu "
            "-- diagnostic only (issued by the D3D11 runtime inside Present; never used as a frame boundary)", (void*)ctx,
            pl ? " in present-layer mode" : "", where, GetCurrentThreadId());
    }
}

// ---- v0.8.1 frame-end rule (game context, not our own work). Under a present layer it is the game-thread frame
// boundary; in normal mode it only counts (LogFrameEndCheck), so any normal log validates it on every screen.
// The frame's last target is the game-side backbuffer, the SCREEN TARGET: an RT0 with exactly the backbuffer's size /
// format (g_bbW / g_bbH / g_bbFmt; under a present layer the presenting swapchain's -- the ETS2 test log showed the game's
// target has the same 3840x2160 fmt=24). A frame ENDS at the first bind of a non-screen target (or of no RT) after the
// screen target received a FINAL draw. A final draw:
//  - is not a profile-screen tile composite (a 3-vertex Draw whose PS SRV0 view is R11G11B10_FLOAT: in SDR the preview
//    composites each of its 4 tile passes straight into the backbuffer, 4 times a frame), and
//  - comes after at least one draw into a non-screen target since the last frame end (so the quads some world frames
//    draw into the backbuffer right after Present, or a layer's own unbind between frames, never make a 2nd end).
// Simulated on both traces (counted per Present): captures/dlaa_trace_ats_flat_hdr_scale125.txt (ATS HDR world: encode
// -> Present -> ~700 quads into the backbuffer -> shadow bind) and the ETS2 SDR profile-screen trace (4 tile
// composites + UI into the backbuffer -> next frame's first tile bind): exactly 1 per frame in both, with and without the
// runtime's post-Present unbind. The naive rule (any backbuffer draw arms) gave 4 / frame (profile) and 2 / frame (world).
bool     g_feRtScreen   = false;    // the current RT0 is the screen target
bool     g_feArmed      = false;    // a final draw into the screen target happened (after non-screen work)
bool     g_feNonScreen  = false;    // a draw into a non-screen target since the last frame end
void*    g_feScreenRt   = nullptr;  // identity of the last bound screen target (no ref)
uint64_t g_feEnds       = 0;        // frame ends found by the rule
uint64_t g_feFinalDraws = 0;        // final draws into the screen target
uint64_t g_feTileDraws  = 0;        // tile composites into the screen target (not final)

void FrameEndOnBind(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs) {
    const bool pl = g_plActive.load(std::memory_order_relaxed);
    if (pl) {
        if (g_plGameTid.load(std::memory_order_relaxed) == 0) {   // mid-session activation: claim the game thread
            const unsigned long me = GetCurrentThreadId();
            unsigned long expect = 0;
            if (me != g_plPresentTid.load(std::memory_order_relaxed) &&
                g_plGameTid.compare_exchange_strong(expect, me))
                Log("present layer: game render thread = tid=%lu (the first game-context bind after present-layer mode "
                    "started, not the layer's thread tid=%lu)", me, g_plPresentTid.load(std::memory_order_relaxed));
        }
        if (!PlOnGameThread()) return;                   // the rule's state belongs to the game render thread
    }
    bool screen = false;
    void* rtPtr = nullptr;
    if (n >= 1 && rtvs && rtvs[0] && g_bbW) {
        ID3D11Resource* r = nullptr;
        rtvs[0]->GetResource(&r);
        if (r) {
            rtPtr = (void*)r;                            // identity only (no ref kept)
            ID3D11Texture2D* t = nullptr;
            if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) {
                D3D11_TEXTURE2D_DESC d{};
                t->GetDesc(&d);
                t->Release();
                screen = d.Width == g_bbW && d.Height == g_bbH && d.Format == g_bbFmt && d.SampleDesc.Count == 1;
            }
            r->Release();
        }
    }
    if (!screen && g_feArmed) {                          // the frame ended: before this bind reaches the context
        g_feArmed = false;
        g_feNonScreen = false;
        ++g_feEnds;
        if (pl) LayerFrameBoundary(ctx, kPlbFrameEnd);
    }
    g_feRtScreen = screen;
    if (screen) g_feScreenRt = rtPtr;
}

void FrameEndReset() {
    g_feRtScreen = false; g_feArmed = false; g_feNonScreen = false; g_feScreenRt = nullptr;
}

void FrameEndOnDraw(ID3D11DeviceContext* ctx, UINT verts, bool indexed) {
    if (g_plActive.load(std::memory_order_relaxed) && !PlOnGameThread()) return;
    if (!g_feRtScreen) { g_feNonScreen = true; return; }
    if (!indexed && verts == 3) {                        // profile-screen tile composite? (not a final draw)
        ID3D11ShaderResourceView* srv = nullptr;
        ctx->PSGetShaderResources(0, 1, &srv);
        bool tile = false;
        if (srv) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            srv->GetDesc(&sd);
            srv->Release();
            tile = sd.Format == DXGI_FORMAT_R11G11B10_FLOAT;
        }
        if (tile) { ++g_feTileDraws; return; }
    }
    ++g_feFinalDraws;
    if (g_feNonScreen) g_feArmed = true;
}

// Every status-line tick (OnPresentBoundary, g_logTick). Present-layer mode: one line per ~10 s -- rule frame ends per
// second vs world passes per second vs the layer's Presents (rule = passes in the world, 1 per frame; anything else
// means the rule misses or doubles frames). Normal mode: rule frame ends per top-level Present, 6 lines and then only
// when it is off 1.00 by more than 5 % (30 lines at most); not for a VR launch (the backbuffer is the mirror window).
void LogFrameEndCheck(uint64_t n) {
    static uint64_t s_ends = 0, s_n = 0, s_rule = 0, s_sc = 0, s_idle = 0, s_pass = 0, s_lp = 0, s_fin = 0, s_tile = 0;
    static int64_t  s_t = 0;
    static int      s_lines = 0, s_devLines = 0;
    const bool pl = g_plActive.load(std::memory_order_relaxed);
    const int64_t now = Qpc();
    const uint64_t lp = g_plPresents.load(std::memory_order_relaxed);
    if (pl && s_t && g_qpcFreq > 0 && s_lines < 400) {
        const double secs = (double)(now - s_t) / (double)g_qpcFreq;
        if (secs > 0.5) {
            ++s_lines;
            const uint64_t dr = g_plBoundRule - s_rule, dp = g_passSeq - s_pass, dl = lp - s_lp;
            Log("frame-end rule check (present layer, %.1f s): rule frame ends %llu = %.1f/s | fallback boundaries scene-clear "
                "%llu idle %llu | world passes %llu = %.1f/s | layer presents %llu = %.1f/s | screen target %ux%u fmt=%d: final "
                "draws %llu, tile composites %llu -- expected: rule = 1 per game frame (in the world = passes/s), fallbacks 0",
                secs, (unsigned long long)dr, (double)dr / secs, (unsigned long long)(g_plBoundFallback - s_sc),
                (unsigned long long)(g_plBoundIdle - s_idle), (unsigned long long)dp, (double)dp / secs,
                (unsigned long long)dl, (double)dl / secs, g_bbW, g_bbH, (int)g_bbFmt,
                (unsigned long long)(g_feFinalDraws - s_fin), (unsigned long long)(g_feTileDraws - s_tile));
        }
    } else if (!pl && n >= 6 && n > s_n && g_launchVr != 1) {
        const uint64_t de = g_feEnds - s_ends, dn = n - s_n;
        const double ratio = (double)de / (double)dn;
        const bool dev = ratio < 0.95 || ratio > 1.05;
        if (s_lines < 6 || (dev && s_devLines < 30)) {
            if (s_lines < 6) ++s_lines; else ++s_devLines;
            Log("frame-end rule check (normal mode, count only): %llu frame end(s) over %llu top-level Presents = %.2f per "
                "Present%s | screen target %ux%u fmt=%d: final draws %llu, tile composites %llu", (unsigned long long)de,
                (unsigned long long)dn, ratio, dev ? " -- NOT 1.00: the rule would miss / double frames on this screen"
                                                   : " (1.00 = the rule finds every frame)", g_bbW, g_bbH, (int)g_bbFmt,
                (unsigned long long)(g_feFinalDraws - s_fin), (unsigned long long)(g_feTileDraws - s_tile));
        }
    }
    s_ends = g_feEnds; s_n = n; s_rule = g_plBoundRule; s_sc = g_plBoundFallback; s_idle = g_plBoundIdle;
    s_pass = g_passSeq; s_lp = lp; s_fin = g_feFinalDraws; s_tile = g_feTileDraws; s_t = now;
}

// The game-thread frame boundary in present-layer mode: the per-frame work hkPresent does in normal mode, with
// n = g_frames (game frames from here on). Runs ONLY on the game render thread (g_plGameTid; a request from any other
// thread is logged and dropped -- v0.8.1 test: the runtime's RSSetViewports(0) reached the game context from the layer's
// thread at shutdown). kind:
//  kPlbFrameEnd   the frame-end rule (FrameEndOnBind): once per game frame, between frames. The only kind that lets the
//                 preview path run (PreviewFramesOk) and that refreshes the game-side backbuffer identity.
//  kPlbSceneClear the scene depth clear that starts a world pass (= a flat world frame), only while the rule found no
//                 frame end for 1 s (since the last one, or since present-layer mode started).
//  kPlbIdle       a game-context bind when, besides that, no boundary of any kind came for 30 ms (screens where the rule
//                 fails): keeps hotkeys, the NGX create budget and the status lines alive. May land mid-frame, which is
//                 harmless for those (the preview path is off then).
void LayerFrameBoundary(ID3D11DeviceContext* ctx, int kind) {
    static const char* const kKind[] = { "frame-end rule", "scene clear", "idle" };
    if (!PlOnGameThread()) {
        static std::atomic<int> s_offLogs{0};             // (not while the game thread is still unclaimed: mid-session)
        if (g_plGameTid.load(std::memory_order_relaxed) != 0 && s_offLogs.fetch_add(1) < 3)
            Log("frame boundary (%s) requested on tid=%lu, which is not the game render thread tid=%lu -- dropped (the "
                "per-frame work runs on the game render thread only)", kKind[kind], GetCurrentThreadId(),
                g_plGameTid.load(std::memory_order_relaxed));
        return;
    }
    const int64_t now = Qpc();
    if (kind == kPlbFrameEnd) {
        g_plLastRuleQpc = now;
        if (g_plFallbackKind >= 0) {
            g_plFallbackKind = -1;
            static int backLogs = 0;
            if (backLogs++ < 8)
                Log("frame boundary: the frame-end rule is back (game frame #%llu) -- the per-frame work runs there again, "
                    "preview DLAA allowed again", (unsigned long long)g_frames.load(std::memory_order_relaxed));
        }
        g_plRuleDriving = true;
        ++g_plBoundRule;
    } else {
        if (g_qpcFreq <= 0) return;
        const int64_t ref = g_plLastRuleQpc ? g_plLastRuleQpc : g_plActiveQpc.load(std::memory_order_relaxed);
        if (!ref || now - ref < g_qpcFreq) return;                       // the rule drives (or may still start)
        if (kind == kPlbIdle && g_plLastBoundaryQpc && now - g_plLastBoundaryQpc < g_qpcFreq * 3 / 100) return;
        if (g_plDraws == 0) return;
        if (g_plFallbackKind != kind) {
            g_plFallbackKind = kind;
            static int fbLogs = 0;
            if (fbLogs++ < 8)
                Log("frame boundary FALLBACK (%s): the frame-end rule found no frame end for 1 s (screen target %ux%u fmt=%d: "
                    "%llu final draws, %llu tile composites so far%s) -- the per-frame work now runs %s (game frame #%llu); "
                    "the profile-screen preview DLAA is off until the rule is back", kKind[kind], g_bbW, g_bbH, (int)g_bbFmt,
                    (unsigned long long)g_feFinalDraws, (unsigned long long)g_feTileDraws,
                    g_feFinalDraws == 0 ? "; 0 final draws = the game-side backbuffer does not have this size / format" : "",
                    kind == kPlbSceneClear ? "at the scene depth clear that starts a world pass (once per world frame)"
                                           : "at a game-context bind after >= 30 ms without a boundary (hotkeys, NGX create "
                                             "budget and status lines stay alive)",
                    (unsigned long long)g_frames.load(std::memory_order_relaxed));
        }
        g_plRuleDriving = false;
        if (kind == kPlbSceneClear) ++g_plBoundFallback; else ++g_plBoundIdle;
    }
    g_plDraws = 0;
    g_plLastBoundaryQpc = now;
    const uint64_t n = g_frames.fetch_add(1);
    static bool s_logged = false;
    if (!s_logged) {
        s_logged = true;
        Log("frame boundary: present-layer mode -- the per-frame work (hotkeys, frame trace, NGX pre-warm, preview "
            "bookkeeping, NGX create budget, status lines, backbuffer) runs on the game render thread tid=%lu, first at %s "
            "on the game context %p; 'Present #' in the lines below = game frames (#%llu now); the layer's own Presents "
            "(tid=%lu) only count (%llu so far)", GetCurrentThreadId(),
            kind == kPlbFrameEnd ? "the frame-end rule (first bind of a non-screen target after the frame's final draw)"
                                 : (kind == kPlbSceneClear ? "the scene depth clear (fallback)" : "a game bind (idle fallback)"),
            (void*)ctx, (unsigned long long)n, g_plPresentTid.load(std::memory_order_relaxed),
            (unsigned long long)g_plPresents.load(std::memory_order_relaxed));
    }
    g_logTick = ComputeLogTick(n);
    PresentFrameWork(nullptr, n, ctx, kind == kPlbFrameEnd);
}

// Every 4096 game draws in present-layer mode (CountGameDraw): one WARNING when no frame boundary came for 5 s.
void LayerBoundaryWatchdog() {
    static bool warned = false;
    if (warned || g_qpcFreq <= 0 || !PlOnGameThread()) return;
    const int64_t ref = g_plLastBoundaryQpc ? g_plLastBoundaryQpc : g_plActiveQpc.load(std::memory_order_relaxed);
    if (!ref || (double)(Qpc() - ref) / (double)g_qpcFreq < 5.0) return;
    warned = true;
    Log("WARNING: present-layer mode: no game-thread frame boundary for 5 s while the game context keeps drawing (%u draws "
        "since the last one; frame-end rule: %llu final draws, %llu tile composites, %llu frame ends in total) -- hotkeys, "
        "the NGX create budget, preview bookkeeping and the status lines are paused", g_plDraws,
        (unsigned long long)g_feFinalDraws, (unsigned long long)g_feTileDraws, (unsigned long long)g_feEnds);
}

// The game-side backbuffer under a present layer (game render thread, every frame boundary). Its size / format is the
// presenting swapchain's (LayerPresent -> g_plScDesc; the ETS2 test log showed the game's target with the same 3840x2160
// fmt=24) -- applied whenever that changes; this is also the frame-end rule's screen target. At a frame-end boundary the
// identity is the screen target the frame just ended on (still bound), so the blit / preview identity tests hit it.
void LayerBackbufferAtBoundary(ID3D11DeviceContext* ctx, uint64_t n, bool fromRule) {
    (void)ctx;
    const uint64_t sd = g_plScDesc.load(std::memory_order_relaxed);
    if (sd && (PlDescW(sd) != g_bbW || PlDescH(sd) != g_bbH || PlDescF(sd) != g_bbFmt)) {
        ApplyBackbufferDesc(nullptr, PlDescW(sd), PlDescH(sd), PlDescF(sd), n);
        static int scLogs = 0;
        if (scLogs++ < 5)
            Log("present layer: backbuffer size / format from the presenting swapchain %ux%u fmt=%d (game frame #%llu) -- the "
                "frame-end rule's screen target and the blit rules use it", PlDescW(sd), PlDescH(sd), (int)PlDescF(sd),
                (unsigned long long)n);
    }
    if (!fromRule || !g_feScreenRt || g_feScreenRt == g_backbuffer.load(std::memory_order_relaxed)) return;
    g_backbuffer.store(g_feScreenRt, std::memory_order_relaxed);
    static int idLogs = 0;
    if (idLogs++ < 5)
        Log("present layer: game-side backbuffer = %p (the screen target the frame just ended on, %ux%u fmt=%d = the presenting "
            "swapchain's size / format; game frame #%llu)", g_feScreenRt, g_bbW, g_bbH, (int)g_bbFmt, (unsigned long long)n);
}

// Full path of `name` next to this DLL.
bool PathNextToDll(const wchar_t* name, wchar_t* path) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&PathNextToDll, &self)) return false;
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return false;
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - path), name);
    return true;
}

// True if "dlaa_off.txt" sits next to this DLL.
bool DlaaOffFilePresent() {
    wchar_t path[MAX_PATH];
    return PathNextToDll(L"dlaa_off.txt", path) &&
           GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// Optional dlaa.ini next to the DLL: key=value lines (# or ; comments).
//   jitter_sign_x / jitter_sign_y  -1 or +1 (default +1, see g_signX; template: docs/dlaa.ini.sample)
//   jitter_enabled                 0/1 (default 1)
//   jitter_phases                  Halton phase count (default 8 since v0.5.7, was 16; NVIDIA: 8 x render ratio)
//   mv_enabled                     0/1 camera-reprojection motion vectors (default 1; Ctrl+F5 toggles)
//   gpu_timing                     -1 auto (VR only, default) / 0 off / 1 on: timestamp queries around each
//                                  eye's DLAA work, logged as avg ms every 600 frames (v0.5.6: per stage, plus
//                                  the depth snapshot copy every 600 snapshots; v0.6.2: also the whole-frame
//                                  "GPU spans" scene/gbuf line every 300 frames, with DLAA ON or OFF)
//   trace_auto_frame               Present # that auto-triggers the frame trace (Ctrl+F11; default 0 = off)
//   vr_eye_alternate               REMOVED in v0.5.4 (eye identity comes from the blit render target); ignored
//   mv_near_reject_m               float metres (default 30): world pairs closer than this are
//                                  excluded from the medoid (own truck/trailer move with the camera)
//   mv_ego_origin_m                float metres 0..30 (default 8, v0.6.0): world draws whose object origin is closer
//                                  than this are ego candidates (own truck exterior) for R_ego
//   mv_ego_pixel_m                 float metres 0..20 (default 3, v0.6.0; 0 = off): world-layer pixels closer than
//                                  this are reprojected with R_ego instead of R_world
//   mv_sample_shift                int 0..4 (default 2, v0.6.3): a G-buffer draw is an MV candidate only when
//                                  (hash(indexCount,startIndex,baseVertex) & ((1<<s)-1)) == 0 (0 = every draw)
//   mv_world_slots                 int 1..128 (default 40, v0.6.3): world (layer 0) candidate cap per pass
//   mv_cabin_slots                 int 1..128 (default 24, v0.6.3): cabin (layer 1) candidate cap per pass
//   preview_dlaa                   0/1 (default 1, v0.7.8): DLAA on the profile / truck-preview screen (flat and VR);
//                                  0 = that path never activates (the screen stays un-anti-aliased as before v0.7.8)
//   preview_capture_at             int >= 0 (default 0 = off, v0.7.8 debug): one Ctrl+F10-style preview capture this many
//                                  Presents after the preview DLAA first runs (v0.8.1: the seam capture on an RGBA16F target)
//   preview_sharpen                0/1 (default 1, v0.8.1 debug): 0 = no RCAS sharpen on the preview units (world unchanged)
//   preview_seam_capture           0/1 (default 0, v0.8.1 debug): 1 = the snapshot key runs the seam capture (pvseam) on
//                                  8-bit preview targets too (RGBA16F / HDR targets always get it)
//   dlss_preset                  default | J | K | L | M | E | F (case-insensitive, default "default" = driver
//                                  pick): DLSS render preset for DLAA, applied at NGX feature creation (v0.5.6).
//                                  Start state; v0.6.5 Shift+F1..F4 select default / E / F / M live (Shift+F12 saves)
//   sharpness                      float 0..1 (default 0.4; 0 = off): FSR1 RCAS sharpen strength after NGX (v0.5.6).
//                                  Start state; v0.6.5: Shift+F7/F8 change it live in 0.1 steps (the
//                                  sharpen texture now always exists; Shift+F12 saves)
//   sharp_radius                   float 1..4 (default 1.5): RCAS ring-tap radius in texels, bilinear (v0.5.8).
//                                  Start state; v0.6.5 Shift+F9/F10 change it live in 0.5 steps (Shift+F12 saves)
//   dlaa_area                      int 40..100 (default 100 = whole image, v0.6.4): DLAA runs on a rect of this % of
//                                  the eye image's width AND height, centred on the eye's optical centre. Start
//                                  state; v0.6.5 Shift+F5/F6 change it live in 10 % steps (Shift+F12 saves)
//   dlaa_area_feather              int 0..512 px (default 96, v0.6.4): blend ramp at the rect edges inside the image
//                                  (render px; v0.7.0 upscale: scaled per axis to output px)
//   dlss_upscale                   0/1 (default 1, v0.7.0): DLSS upscaling from the scene size (r_scale_x / r_scale_y)
//                                  to the blit RT size whenever that is larger; 0 = always DLAA at render res (the
//                                  v0.6.5 path). Start state; Ctrl+F4 toggles live (Shift+F12 saves)
//   dlss_hdr                       0/1 (default 1, v0.8.0): with Windows HDR on (the game's RGBA16F pipeline) NGX is told
//                                  the colour is linear HDR (IsHDR); 0 = the float values are passed as display-referred
//                                  0..1 (no IsHDR). No effect on the 8-bit (SDR) path. Read once at load
//   key_<action>                  v0.7.7 re-bindable hotkeys, "[Shift+][Ctrl+][Alt+]Key" or none; see docs/KEYS.md and
//                                  g_keys[] (key_mode_cycle, key_model_1..4, key_area_down/up, key_sharpen_down/up,
//                                  key_width_down/up, key_dlaa_toggle, key_save, key_upscale_toggle, key_mv_toggle,
//                                  key_mv_debug, key_passive, key_jitter_only, key_selftest, key_snapshot, key_trace,
//                                  key_jitter_sign)
//   beeps                          0/1 (default 1): v0.5.7 audible feedback for the Shift+F-key user controls
//                                  (model select, area, sharpen strength/width, DLAA on/off, save) and the
//                                  Ctrl+F7 passive / v0.7.0 Ctrl+F4 upscale toggles (kernel32 Beep on a worker thread)

// v0.7.10: "a.b.c.d" file version from a module's PE version resource, for the startup "env:" lines. "no version info"
// if the resource is absent, "unavailable" on a read error. Startup only (GetFileVersionInfo is not on any hot path).
void FileVersionStrW(const wchar_t* path, char* out, size_t cap) {
    out[0] = 0;
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (!size) { snprintf(out, cap, "no version info"); return; }
    std::vector<unsigned char> buf(size);
    if (!GetFileVersionInfoW(path, 0, size, buf.data())) { snprintf(out, cap, "unavailable"); return; }
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (VerQueryValueW(buf.data(), L"\\", (void**)&ffi, &len) && ffi && len)
        snprintf(out, cap, "%u.%u.%u.%u",
                 (unsigned)HIWORD(ffi->dwFileVersionMS), (unsigned)LOWORD(ffi->dwFileVersionMS),
                 (unsigned)HIWORD(ffi->dwFileVersionLS), (unsigned)LOWORD(ffi->dwFileVersionLS));
    else
        snprintf(out, cap, "no version info");
}

// v0.7.10: find the nvngx_dlss.dll the loader would use, in the order NGX init relies on (NgxAcquire adds our own DLL's
// folder to the search path, and Windows searches the game exe folder by default): our folder first, then the game
// exe folder. true + full path in out, or false if neither has it. Startup only.
bool FindNvngxDlss(wchar_t* out, DWORD cap) {
    wchar_t dir[MAX_PATH];
    // (a) next to dinput8.dll
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&FindNvngxDlss, &self) && self) {
        const DWORD n = GetModuleFileNameW(self, dir, MAX_PATH);
        if (n && n < MAX_PATH) {
            wchar_t* slash = wcsrchr(dir, L'\\');
            if (slash) {
                *(slash + 1) = 0;
                wchar_t cand[MAX_PATH];
                if (_snwprintf_s(cand, _TRUNCATE, L"%snvngx_dlss.dll", dir) > 0 &&
                    GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES) { wcscpy_s(out, cap, cand); return true; }
            }
        }
    }
    // (b) next to the game exe
    const DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (n && n < MAX_PATH) {
        wchar_t* slash = wcsrchr(dir, L'\\');
        if (slash) {
            *(slash + 1) = 0;
            wchar_t cand[MAX_PATH];
            if (_snwprintf_s(cand, _TRUNCATE, L"%snvngx_dlss.dll", dir) > 0 &&
                GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES) { wcscpy_s(out, cap, cand); return true; }
        }
    }
    return false;
}

void LoadConfig() {
    InitKeyBinds();                                      // v0.7.7: defaults first, key_* lines below override
    wchar_t path[MAX_PATH];
    FILE* f = nullptr;
    if (PathNextToDll(L"dlaa.ini", path)) _wfopen_s(&f, path, L"r");
    const bool haveIni = f != nullptr;
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char* eq = strchr(line, '=');
            if (!eq || line[0] == '#' || line[0] == ';') continue;
            *eq = 0;
            char* key = line;
            while (*key == ' ' || *key == '\t') ++key;
            for (char* e = eq - 1; e >= key && (*e == ' ' || *e == '\t'); --e) *e = 0;
            const long v = strtol(eq + 1, nullptr, 10);
            if (!strncmp(key, "key_", 4)) { SetKeyBindFromIni(key, eq + 1); continue; }   // v0.7.7 hotkeys
            if      (!strcmp(key, "jitter_sign_x"))  g_signX = v < 0 ? -1 : 1;
            else if (!strcmp(key, "jitter_sign_y"))  g_signY = v < 0 ? -1 : 1;
            else if (!strcmp(key, "jitter_enabled")) g_jitterEnabled = v != 0;
            else if (!strcmp(key, "mv_enabled"))     g_mvOn = v != 0;
            else if (!strcmp(key, "mv_near_reject_m")) {
#ifdef WITH_DLAA
                const double m = strtod(eq + 1, nullptr);
                for (SceneDlaa& d : g_dlaa)
                    d.Mv().SetNearReject((float)(m < 0.0 ? 0.0 : (m > 1000.0 ? 1000.0 : m)));
#endif
            }
            else if (!strcmp(key, "mv_ego_origin_m")) {
#ifdef WITH_DLAA
                const double m = strtod(eq + 1, nullptr);
                for (SceneDlaa& d : g_dlaa)
                    d.Mv().SetEgoOrigin((float)(m < 0.0 ? 0.0 : (m > 30.0 ? 30.0 : m)));
#endif
            }
            else if (!strcmp(key, "mv_ego_pixel_m")) {
#ifdef WITH_DLAA
                const double m = strtod(eq + 1, nullptr);
                for (SceneDlaa& d : g_dlaa)
                    d.Mv().SetEgoPixel((float)(m < 0.0 ? 0.0 : (m > 20.0 ? 20.0 : m)));
#endif
            }
            else if (!strcmp(key, "mv_sample_shift")) {
#ifdef WITH_DLAA
                g_mvSampleShift = v < 0 ? 0 : (v > 4 ? 4 : (int)v);
#endif
            }
            else if (!strcmp(key, "mv_world_slots")) {
#ifdef WITH_DLAA
                g_mvWorldSlots = v < 1 ? 1 : (v > CandidateRecord::kSlots ? CandidateRecord::kSlots : (int)v);
#endif
            }
            else if (!strcmp(key, "mv_cabin_slots")) {
#ifdef WITH_DLAA
                g_mvCabinSlots = v < 1 ? 1 : (v > CandidateRecord::kSlots ? CandidateRecord::kSlots : (int)v);
#endif
            }
            else if (!strcmp(key, "gpu_timing"))     g_gpuTiming = v < 0 ? -1 : (v ? 1 : 0);
#ifdef WITH_DLAA
            else if (!strcmp(key, "preview_dlaa"))   g_previewDlaa = v != 0 ? 1 : 0;   // v0.7.8
            else if (!strcmp(key, "preview_capture_at")) g_pvCaptureAt = v < 0 ? 0 : (int)v;   // v0.7.8 debug
            else if (!strcmp(key, "preview_edge_fix"))   g_pvEdgeFix = v != 0 ? 1 : 0;           // v0.8.1 debug
            else if (!strcmp(key, "preview_tile_jitter")) g_pvTileJitter = v != 0 ? 1 : 0;       // v0.8.1 debug
            else if (!strcmp(key, "preview_layout_recheck")) g_pvLayRecheck = v < 0 ? 0 : (v > 600 ? 600 : (int)v);
            else if (!strcmp(key, "preview_sharpen"))      g_pvSharpen = v != 0 ? 1 : 0;         // v0.8.1 round 5 debug
            else if (!strcmp(key, "preview_seam_capture")) g_pvSeamAlways = v != 0 ? 1 : 0;      // v0.8.1 round 5 debug
#endif
            else if (!strcmp(key, "trace_auto_frame")) g_traceAutoFrame = v < 0 ? 0 : (int)v;
            else if (!strcmp(key, "vr_eye_alternate")) Log("dlaa.ini: vr_eye_alternate is ignored since v0.5.4 (eye identity comes from the blit render target)");
            else if (!strcmp(key, "jitter_phases"))  g_phases = v < 1 ? 8 : (v > 256 ? 256 : (int)v);
            else if (!strcmp(key, "beeps"))          g_beeps = v != 0;
            else if (!strcmp(key, "dlss_upscale"))   g_dlssUpscale = v != 0;   // v0.7.0
            else if (!strcmp(key, "mode"))           g_iniMode = (v >= 0 && v <= 2) ? (int)v : -1;   // v0.7.2 (End)
            else if (!strcmp(key, "dlss_preset")) {
                // The only string-valued key: first token after '=', case-insensitive.
                char tok[16] = {}; int n = 0;
                const char* s = eq + 1;
                while (*s == ' ' || *s == '\t') ++s;
                while (*s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n' && *s != '#' && *s != ';' &&
                       n < (int)sizeof(tok) - 1)
                    tok[n++] = (char)tolower((unsigned char)*s++);
#ifdef WITH_DLAA
                const bool okPreset = !strcmp(tok, "default") ? SceneDlaa::SetDlssPreset(0)
                                    : (n == 1 && SceneDlaa::SetDlssPreset(tok[0]));
                if (!okPreset)
                    Log("dlaa.ini: dlss_preset '%s' not recognised (use default, J, K, L, M, E or F) -- keeping %s",
                        tok, SceneDlaa::DlssPresetName());
#endif
            }
            else if (!strcmp(key, "sharpness")) {
#ifdef WITH_DLAA
                SceneDlaa::SetSharpness((float)strtod(eq + 1, nullptr));   // clamped to 0..1
#endif
            }
            else if (!strcmp(key, "sharp_radius")) {
#ifdef WITH_DLAA
                SceneDlaa::SetSharpRadius((float)strtod(eq + 1, nullptr)); // clamped to 1..4
#endif
            }
            else if (!strcmp(key, "dlaa_area")) {
#ifdef WITH_DLAA
                SceneDlaa::SetArea(v < 40 ? 40 : (v > 100 ? 100 : (int)v));   // v0.6.4, clamped to 40..100
#endif
            }
            else if (!strcmp(key, "dlaa_area_feather")) {
#ifdef WITH_DLAA
                SceneDlaa::SetFeather(v < 0 ? 0 : (v > 512 ? 512 : (int)v));  // v0.6.4, clamped to 0..512
#endif
            }
            else if (!strcmp(key, "dlss_hdr")) {
#ifdef WITH_DLAA
                SceneDlaa::SetHdrLinear(v != 0);                               // v0.8.0, HDR units only
#endif
            }
        }
        fclose(f);
    }
#ifdef WITH_DLAA
    const float nearRej = g_dlaa[0].Mv().NearReject();
    const float egoOrigin = g_dlaa[0].Mv().EgoOrigin();
    const float egoPixel = g_dlaa[0].Mv().EgoPixel();
    const int   mvShift = g_mvSampleShift, mvWorldSlots = g_mvWorldSlots, mvCabinSlots = g_mvCabinSlots;
    const char* preset = SceneDlaa::DlssPresetName();
    const float sharp = SceneDlaa::Sharpness();
    const float sharpRadius = SceneDlaa::SharpRadius();
    const int   area = SceneDlaa::Area(), feather = SceneDlaa::Feather();
    const int   previewDlaa = g_previewDlaa;
    const int   dlssHdr = SceneDlaa::HdrLinear() ? 1 : 0;   // v0.8.0
#else
    const float nearRej = 30.0f;
    const float egoOrigin = 8.0f;
    const float egoPixel = 3.0f;
    const int   mvShift = 2, mvWorldSlots = 40, mvCabinSlots = 24;
    const char* preset = "n/a";
    const float sharp = 0.0f;
    const float sharpRadius = 1.5f;
    const int   area = 100, feather = 96;
    const int   previewDlaa = 0;
    const int   dlssHdr = 0;
#endif
    Log("config (%s): jitter_enabled=%d jitter_phases=%d jitter_sign_x=%d jitter_sign_y=%d mv_enabled=%d mv_near_reject_m=%.1f mv_ego_origin_m=%.1f mv_ego_pixel_m=%.1f mv_sample_shift=%d mv_world_slots=%d mv_cabin_slots=%d gpu_timing=%d trace_auto_frame=%d dlss_preset=%s sharpness=%.2f sharp_radius=%.1f dlaa_area=%d dlaa_area_feather=%d dlss_upscale=%d beeps=%d preview_dlaa=%d dlss_hdr=%d",
        haveIni ? "dlaa.ini" : "no dlaa.ini, defaults", (int)g_jitterEnabled, g_phases, g_signX, g_signY,
        (int)g_mvOn.load(), nearRej, (double)egoOrigin, (double)egoPixel, mvShift, mvWorldSlots, mvCabinSlots,
        g_gpuTiming, g_traceAutoFrame, preset, (double)sharp, (double)sharpRadius, area, feather,
        (int)g_dlssUpscale.load(), (int)g_beeps, previewDlaa, dlssHdr);
#ifdef WITH_DLAA
    if (g_pvEdgeFix != 1 || g_pvTileJitter != 1 || g_pvLayRecheck != 0 || g_pvSharpen != 1 || g_pvSeamAlways != 0)
        Log("dlaa.ini: preview_edge_fix=%d preview_tile_jitter=%d preview_layout_recheck=%d preview_sharpen=%d "
            "preview_seam_capture=%d (debug; defaults 1 / 1 / 0 = auto / 1 / 0)", g_pvEdgeFix, g_pvTileJitter, g_pvLayRecheck,
            g_pvSharpen, g_pvSeamAlways);   // v0.8.1 round 4 + 5 debug switches
    if (g_pvCaptureAt > 0)
        Log("dlaa.ini: preview_capture_at=%d (debug) -- one preview capture (as Ctrl+F10) %d Presents after the preview DLAA "
            "first runs; files in dlaa_selftest\\, pvcap: lines in this log", g_pvCaptureAt, g_pvCaptureAt);
#endif
    // v0.7.10: triage environment, logged once at load. The game exe + its file version, and the nvngx_dlss.dll the
    // loader would use (same search order as NGX init) + its file version. Cheap, startup only. config.cfg is NOT read.
    {
        wchar_t exeW[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, exeW, MAX_PATH);
        const wchar_t* exeBaseW = wcsrchr(exeW, L'\\');
        exeBaseW = exeBaseW ? exeBaseW + 1 : exeW;
        char exeBase[MAX_PATH] = "";
        WideCharToMultiByte(CP_UTF8, 0, exeBaseW, -1, exeBase, (int)sizeof(exeBase) - 1, nullptr, nullptr);
        char exeVer[64];
        FileVersionStrW(exeW, exeVer, sizeof(exeVer));
        Log("env: game exe = %s (file version %s)", exeBase, exeVer);

        wchar_t ngxW[MAX_PATH];
        if (FindNvngxDlss(ngxW, MAX_PATH)) {
            char ngxVer[64];
            FileVersionStrW(ngxW, ngxVer, sizeof(ngxVer));
            char ngxPath[MAX_PATH] = "";
            WideCharToMultiByte(CP_UTF8, 0, ngxW, -1, ngxPath, (int)sizeof(ngxPath) - 1, nullptr, nullptr);
            Log("env: nvngx_dlss.dll = %s (file version %s)", ngxPath, ngxVer);
        } else {
            Log("env: nvngx_dlss.dll NOT found next to dinput8.dll or the game exe -- DLAA/DLSS init will fail unless "
                "it is elsewhere on the DLL search path");
        }
    }
    LogKeyBinds();
}

// Create + enable one MinHook detour; logs the outcome.
bool HookFn(const char* name, void* target, void* hook, void** orig) {
    char where[MAX_PATH + 32];
    DescribeAddr(target, where, sizeof(where));
    MH_STATUS st;
    if ((st = MH_CreateHook(target, hook, orig)) != MH_OK) {
        Log("MH_CreateHook(%s) failed: %s (target=%s)", name, MH_StatusToString(st), where);
        return false;
    }
    if ((st = MH_EnableHook(target)) != MH_OK) {
        Log("MH_EnableHook(%s) failed: %s (target=%s)", name, MH_StatusToString(st), where);
        return false;
    }
    Log("%s hook installed (MinHook) target=%s hook=%p", name, where, hook);
    return true;
}

DWORD WINAPI SetupThread(LPVOID) {
#ifdef WITH_DLAA
    // v0.7.8: compile every embedded shader once, on ShaderCache's own worker thread, while the game boots (was a
    // D3DCompile per unit inside its first Run on the render thread: ~2 s freeze per CameraMv). Not under the
    // loader lock here (StartInjection only spawned this thread).
    CameraMv::RegisterShaders();
    SceneDlaa::RegisterShaders();
    PreviewBlit::RegisterShaders();
    PvCapRegisterShaders();                              // Ctrl+F10 preview capture metrics shader
    PvAsmRegisterShaders();                              // round 6 tiled preview: picture depth assembly
    PvEdgeRegisterShaders();                             // v0.8.1 round 5: zero-guarded tile edge fill
    ShaderCache::Start();
#endif
    // Create a throwaway device+swapchain purely to read the shared
    // IDXGISwapChain vtable, then read the Present address (vtable index 8).
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"dlaa_dummy_wnd";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
                              0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount       = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow      = hwnd;
    scd.SampleDesc.Count  = 1;
    scd.Windowed          = TRUE;
    scd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain*      sc  = nullptr;
    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL    fl{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &scd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr) || !sc) {
        Log("dummy swapchain create failed hr=0x%lx", hr);
        if (hwnd) DestroyWindow(hwnd);
        return 0;
    }

    void** vtbl    = *reinterpret_cast<void***>(sc);
    void*  present = vtbl[8];                    // IDXGISwapChain::Present (dxgi code)

    // Hook the dxgi/d3d11 functions themselves with MinHook trampolines; the
    // vtables are never written. Dummy objects can be released afterwards.
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MH_Initialize failed: %s", MH_StatusToString(st));
    } else {
        HookFn("Present", present, reinterpret_cast<void*>(&hkPresent),
               reinterpret_cast<void**>(&oPresent));
        // ID3D11DeviceContext vtable: DrawIndexed = 12, Draw = 13, OMSetRenderTargets = 33, RSSetViewports = 44.
        void** cvtbl = *reinterpret_cast<void***>(ctx);
        HookFn("OMSetRenderTargets", cvtbl[33], reinterpret_cast<void*>(&hkOMSetRenderTargets),
               reinterpret_cast<void**>(&oOMSetRenderTargets));
        HookFn("RSSetViewports", cvtbl[44], reinterpret_cast<void*>(&hkRSSetViewports),
               reinterpret_cast<void**>(&oRSSetViewports));
        HookFn("DrawIndexed", cvtbl[12], reinterpret_cast<void*>(&hkDrawIndexed),
               reinterpret_cast<void**>(&oDrawIndexed));
        // v0.4.3 trace-only hooks (log-only, pass through).
        HookFn("Dispatch", cvtbl[41], reinterpret_cast<void*>(&hkDispatch),
               reinterpret_cast<void**>(&oDispatch));
        HookFn("CopySubresourceRegion", cvtbl[46], reinterpret_cast<void*>(&hkCopySubresourceRegion),
               reinterpret_cast<void**>(&oCopySubresourceRegion));
        HookFn("CopyResource", cvtbl[47], reinterpret_cast<void*>(&hkCopyResource),
               reinterpret_cast<void**>(&oCopyResource));
        HookFn("ClearRenderTargetView", cvtbl[50], reinterpret_cast<void*>(&hkClearRTV),
               reinterpret_cast<void**>(&oClearRTV));
        HookFn("ClearDepthStencilView", cvtbl[53], reinterpret_cast<void*>(&hkClearDSV),
               reinterpret_cast<void**>(&oClearDSV));
        HookFn("ResolveSubresource", cvtbl[57], reinterpret_cast<void*>(&hkResolveSubresource),
               reinterpret_cast<void**>(&oResolveSubresource));
        ID3D11DeviceContext1* ctx1 = nullptr;
        if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&ctx1)) && ctx1) {
            void** c1vtbl = *reinterpret_cast<void***>(ctx1);   // ID3D11DeviceContext1: CopySubresourceRegion1 = 115
            HookFn("CopySubresourceRegion1", c1vtbl[115], reinterpret_cast<void*>(&hkCopySubresourceRegion1),
                   reinterpret_cast<void**>(&oCopySubresourceRegion1));
            // v0.5.3 (indices from d3d11_1.h ID3D11DeviceContext1Vtbl): DiscardResource 117, DiscardView 118, DiscardView1 133.
            HookFn("DiscardResource", c1vtbl[117], reinterpret_cast<void*>(&hkDiscardResource),
                   reinterpret_cast<void**>(&oDiscardResource));
            HookFn("DiscardView", c1vtbl[118], reinterpret_cast<void*>(&hkDiscardView),
                   reinterpret_cast<void**>(&oDiscardView));
            HookFn("DiscardView1", c1vtbl[133], reinterpret_cast<void*>(&hkDiscardView1),
                   reinterpret_cast<void**>(&oDiscardView1));
            ctx1->Release();
        } else Log("ID3D11DeviceContext1 unavailable on the dummy context: CopySubresourceRegion1 / Discard* not hooked");
        HookFn("Draw", cvtbl[13], reinterpret_cast<void*>(&hkDraw),
               reinterpret_cast<void**>(&oDraw));
    }

    if (sc)  sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (hwnd) DestroyWindow(hwnd);
    return 0;
}

} // namespace

// Version comes from CMake project(... VERSION) via the DLAA_INJECTOR_VERSION compile
// definition (see CMakeLists.txt). The fallback only matters for a non-CMake compile and
// is deliberately not a real version number so a missing definition is obvious in the log.
#ifndef DLAA_INJECTOR_VERSION
#define DLAA_INJECTOR_VERSION "0.0.0-dev"
#endif

void StartInjection() {
    Log("=== ETS2/ATS DLAA injector v%s loaded (pid=%lu) ===", DLAA_INJECTOR_VERSION, GetCurrentProcessId());
    {
        LARGE_INTEGER qf;
        QueryPerformanceFrequency(&qf);
        g_qpcFreq = qf.QuadPart;
    }
    LoadConfig();
    {                                                    // v0.7.4: flat or VR, from the launch options
        std::wstring cl = GetCommandLineW();
        for (wchar_t& c : cl) c = (wchar_t)towlower(c);
        g_launchVr = (cl.find(L"-openvr") != std::wstring::npos || cl.find(L"-openxr") != std::wstring::npos ||
                      cl.find(L"-oculus") != std::wstring::npos) ? 1 : 0;
#ifdef WITH_DLAA
        g_vrArea = SceneDlaa::Area();
        if (g_launchVr == 0) SceneDlaa::SetArea(100);
        Log("launch mode: %s (from the launch options)%s", g_launchVr ? "VR" : "FLAT",
            g_launchVr ? "" : (g_vrArea < 100 ? " -- dlaa_area forced to 100 (the dlaa.ini value is used in VR only)" : ""));
#else
        Log("launch mode: %s (from the launch options)", g_launchVr ? "VR" : "FLAT");
#endif
    }
    g_effPhases = g_phases;                             // v0.7.0: "jitter: N Halton phases" logs only on a change
    JitterOfPhase(0, &g_jx, &g_jy);
#ifdef WITH_DLAA
    g_dlaaDisabled = DlaaOffFilePresent();
    g_dlaaOn = !g_dlaaDisabled;
    Log("DLAA build: %s", g_dlaaDisabled ? "dlaa_off.txt found -- starts OFF (use the mode / DLAA toggle key in-game to enable)"
                                         : "starts ON (the DLAA toggle key switches it; dlaa_off.txt next to the DLL = start OFF)");
    if (g_iniMode >= 0 && !g_dlaaDisabled) {             // v0.7.2: the mode saved by the End key wins over dlss_upscale
        g_dlaaOn = g_iniMode != 0;
        if (g_iniMode != 0) g_dlssUpscale = g_iniMode == 2;
        Log("mode from dlaa.ini: %s (%s cycles DLAA -> DLSS -> off and saves it)",
            g_iniMode == 1 ? "DLAA" : (g_iniMode == 2 ? "DLSS" : "OFF"), KeyName(KA_MODE_CYCLE));
    }
#else
    Log("DLAA build: not compiled in (WITH_DLAA=OFF) -- detect + log only");
#endif
    CreateThread(nullptr, 0, SetupThread, nullptr, 0, nullptr);
}

// v0.7.10: one line at process exit so the log always ends with a verdict (did the scene / blits ever run, which mode)
// instead of just stopping. Called from DllMain's DLL_PROCESS_DETACH. Loader-lock safe: only plain-global / atomic
// reads and Log -- no D3D, no NGX, no thread waits. processTerminating = DllMain's lpReserved != null (process exit);
// either way we only log and do nothing else.
void OnProcessDetach(bool processTerminating) {
    LogExiting().store(true, std::memory_order_relaxed);   // never block on the log mutex from here on
    // v0.8.1: + the present-layer verdict, only when that mode was on (presents= then counts game frames).
    char plTail[200] = "";
    if (g_plActive.load(std::memory_order_relaxed))
        snprintf(plTail, sizeof(plTail), " present-layer=%s (%s) layer-presents=%llu boundaries frame-end=%llu scene-clear=%llu "
                 "idle=%llu", g_plModule[0] ? g_plModule : "?",
                 g_plAdopted.load(std::memory_order_relaxed) ? "game context adopted" : "game context kept",
                 (unsigned long long)g_plPresents.load(std::memory_order_relaxed), (unsigned long long)g_plBoundRule,
                 (unsigned long long)g_plBoundFallback, (unsigned long long)g_plBoundIdle);
    Log("=== injector unloading (%s): presents=%llu passes=%llu blits=%llu mode=%s dlaa=%s dlaa-mode=%s%s ===",
        processTerminating ? "process exit" : "DLL unload",
        (unsigned long long)g_frames.load(std::memory_order_relaxed),
        (unsigned long long)g_passSeq, (unsigned long long)g_blitCount,
        g_vrMode ? "VR" : (g_flatMode ? "flat" : "undetected"),
        g_dlaaOn.load(std::memory_order_relaxed) ? "on" : "off", DlaaModeStr(), plTail);
}
