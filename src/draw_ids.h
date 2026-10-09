// DrawIds -- v0.10.0 exact per-draw motion vectors (dlaa.ini mv_objects = 2, the default). Design note:
// docs/DLAA_INTEGRATION.md "Per-draw motion vectors (v0.10.0)"; engine facts: scripts/rdc_drawid_audit.py.
//
// Why: v0.9.0 tagged "moving" draws with 15 stencil ids chosen by CPU heuristics (membership, coherence, static class,
// ego set ...). In game the heuristics tagged the road, blinked, and missed plates. This replaces the guess with a fact:
//   1. RECORD (DrawIdRecord, one per pass FIFO slot): every world / cabin G-buffer DrawIndexed(Instanced) keeps its whole
//      input state (IA, VS + cbuffers with D3D11.1 offsets, VS SRVs / samplers, RS state, viewports, scissors, draw
//      arguments; refs held) and the byte offset of its MVP (VS cb0 rows 4..7) in a mirror of the game's cbuffer ring(s).
//   2. REPLAY at the G-buffer LEAVE, while the scene depth is final and intact: the same VS / inputs / RS / viewport (the
//      same jitter) with OUR pixel shader into an R16_UINT draw-id target, depth func EQUAL against a READ-ONLY view of
//      the game's scene depth. Same vertex shader + same inputs = the same SV_Position.z, so EQUAL passes exactly where
//      that draw owns the pixel (alpha-tested pixels hold another draw's depth and fail). Draw id = record index + 1.
//   3. PAIR on the GPU in the same frame (DrawIdMv, one per eye / CameraMv): each draw with the eye's previous pass by
//      geometry key (pointer-free fallback key after the game's buffer-pool rotation), nearest origin, MUTUAL nearest,
//      R_draw = MVP_prev * inverse(MVP_cur). Static draws (R_draw within mv_drawid_snap_px of the camera R) keep the
//      camera R exactly; unpaired / instanced / no-MVP draws keep the camera R of their layer.
//   4. Pass B (CameraMv) reads id -> R per pixel.
// v0.10.0 phase 3:
//   5. CONSENSUS CAMERA R (dlaa.ini mv_camr_consensus, default on): the per-draw pairs vote (DrawIdMv CSPair / CSVote /
//      CSPick) -- the largest cluster of paired draws whose R's agree within the snap threshold at their own probe points
//      (static scenery: hundreds of draws) becomes the camera R of its layer in CameraMv's solve buffer, the medoid of the
//      sampled candidates (it picked moving vehicles in game) only the fallback (< kConsMin draws).
//   6. FORWARD-PASS DRAW IDS: a DrawIdRecord in forward mode records the forward draws inject.cpp re-draws into the forward
//      depth (plates, decals, glass, wires) and replays them into the GREEN channel of an R16G16_UINT draw-id target against
//      the forward depth (EQUAL, draw order); their ids follow the G-buffer ids in one table, pass B uses them where the
//      forward depth won.
// v0.10.0 phase 4:
//   7. ATTACH (dlaa.ini mv_fwd_attach_m, default 0.5 m; 0 = off): a forward draw whose MVP origin lies on a G-buffer MOVER's
//      origin (within the radius) takes that mover's R (DrawIdMv CSAttach); pass B does the same per PIXEL where the forward
//      surface sits within the radius of the G-buffer mover under it (a plate on a bus whose own pair carries no motion).
//   8. DUMP (inject.cpp key_mv_dump): RequestDump() logs, for the next prepared pass, every forward draw and every G-buffer
//      draw with IndexCount <= 36: keys, cb0 window, MVP origin, partner, displacement, R_draw - camera R at the origin,
//      state, attach (diagnostic readback, non-blocking, a few frames late).
// v0.10.0 phase 5:
//   9. INHERIT: a draw left UNPAIRED (no partner group -- its keys are new every frame --, oversized group, continuity reject)
//      whose MVP is bit-identical to a draw paired by its own pair (its matrix TWIN: a plate / lamp drawn with the vehicle's
//      matrix) takes the twin's R and state (DrawIdMv CSInherit1 / CSTwin; dlaa.ini mv_drawid_twin, default on); a G-buffer
//      draw still unpaired without a twin takes a G-buffer mover's R when its origin lies within mv_fwd_attach_m of the
//      mover's AND it shares the mover's orientation (CSAttach). The dump lists every unpaired / inherited draw.
// v0.10.0 phase 6:
//  10. RIGID PARENTS: a draw left unpaired by its own pairing (in game: plates whose vertices the CPU writes in VIEW space every
//      frame -- their cb0 rows 4..7 are the bare projection, so no twin / origin can name their vehicle) takes the R of the draw
//      it SITS ON: R_U = R_parent exactly for any node rigidly fixed on the parent (column-vector convention, see kDrawIdCs).
//      The parent is found by a GPU vote of the neighbouring pixels in the draw-id target (+-2 / +-5 px, same linear depth
//      within 2 %, a G-buffer draw paired by its own pair; >= 32 votes and >= 60 %), through an unpaired neighbour that found
//      one (plate text inside an unpaired plate background); a static parent makes the draw static (DrawIdMv CSParentList /
//      CSParentVote / CSParentPick1 / CSParentPick2; dlaa.ini mv_drawid_parent, default on). It decides over a twin / origin
//      attach; no winner keeps the phase-5 chain.
// v0.10.0 phase 6b:
//  10b. INSTANCED PLATES (dlaa.ini mv_drawid_instanced, now default on): a close car's licence plate is a DrawIndexedInstanced
//      draw (flag kFInst, no MVP). It is now replayed into the draw-id target and joins the rigid-parent vote like an unpaired
//      draw (CSParentCount gives the 64 vote slots to the instanced draws that cover the screen, most pixels first; they take
//      the R / state of the draw they sit on and are never a parent source). No winning parent = the camera R, as before.
// v0.10.0 phase 6c:
//  10c. MARCHING VOTE: every sampled pixel of a listed draw marches in 8 directions (2 px steps, up to 32 px) across its own
//      pixels and depth-continuous pixels of draws without a motion (plate background / text, other unpaired / instanced
//      draws) to the first draw WITH a motion (own pair, twin, origin attach) -- that direction's vote (within 4 % linear
//      depth, else depth-rejected). Winner: >= 8 votes and >= 50 % (tie: nearer in depth). A 2nd pass lets the listed draws
//      still without a parent take the motion of listed draws that found one. Marches per draw are capped (grid decimation).
// v0.10.0 phase 14 STATIC REPLAY SKIP (dlaa.ini mv_replay_static_frames, default 6; 0 = off; mv_replay_static_every, default 4):
//  11. The G-buffer replay (step 2) of a draw whose geometry key (FullKey: the key the pairing tries first) was PAIRED AND DEEP
//      STATIC by its own pair (own motion within mv_replay_static_eps_px of the camera's -- round 3: a vehicle at the player's
//      speed is static by the snap but not deep, and never skipped; no motion of the key for 60 frames) in the last
//      mv_replay_static_frames frames in a row is skipped, except on its re-check frame (frame %
//      every == hash(key) % every: ~1/every of them each frame). Only the replay is skipped: the draw is still RECORDED (its MVP
//      is gathered, paired with the previous pass, votes in the consensus, gets its state), so it never loses its partner; its
//      pixels hold id 0 = the camera R of their layer in pass B -- exactly what a static draw gets anyway. The verdicts come
//      back to the CPU with the diagnostics readback (Cnt bitmasks: own-pair static; moving -- an own-pair mover, a track reject,
//      a moving twin / attach / rigid parent; rigid parent this pass -- those keys are always replayed, their plates need their
//      pixels). A moving draw resets its key's streak; a draw with no partner (new, culled back in) neither builds nor breaks
//      it but HOLDS its key (an unpaired copy needs its pixels for its own rigid-parent vote); the table is cleared when the
//      camera R is not healthy (history forgotten, a readback without a used world consensus, an unusable record, a FIFO
//      underflow). Cabin, instanced / no-MVP, forward and late draws are never skipped. In the rigid-parent vote a world pixel
//      without a G-buffer id in a gated pass counts as a STATIC source (kParPseudo: what the skipped draw is), so the vote reads
//      the same as with the full replay whichever static keys were re-drawn this frame (fix of the 2026-10-09 in-game test: a
//      moving SUV's plate lost its parent on the 1-in-4 re-check frames). DrawIdRecord::SetStaticGate (per replay, main
//      G-buffer passes only), DrawIdMv::StaticSkip.
//      Round 4 (rigid-parent vote, independent of the gate): every pass keeps a 64-entry table of its listed draws (IndexCount,
//      jitter-free pixel centroid, final parent, size; UHoldW / UHoldR ping-pong). PLATE HOLD: a listed draw left without a parent
//      (or only the pseudo static source) keeps last pass's parent for up to 3 frames when its centroid, carried back by that
//      parent's motion, lands on last pass's entry (and, where the two motions differ by >= 2.5 px, nearer than the camera motion
//      puts it). TEMPORAL CHECK: a MOVING candidate is refused when last pass's entry sits where the camera motion, not the
//      candidate's, puts the draw (>= 2.5 px apart). CLUTTER RULE: a draw of more than 4 instances (a vegetation batch) never takes
//      a MOVING parent -- grass right in front of an overtaking car is surrounded by it on screen like a plate by its body.
//      Round 6 (ATS: roadside grass solid magenta while driving, independent of the gate): ORIGIN-FREE MVPs -- ATS draws its
//      roadside grass (non-instanced and instanced) CAMERA-RELATIVE: cb0 rows 4..7 = projection * view rotation, column 3 =
//      (~0, ~0, near, ~0), the position in the vertices / another cbuffer. Their own pair's R is the camera ROTATION only, so they
//      became movers with a motion missing the camera's translation. A paired draw with such an MVP is now STATIC (the layer's
//      camera R), never a mover, never a skip streak, never a consensus voter (shader OriginFree). The round-4 clutter rule is
//      replaced: an instanced draw of more than ONE instance is never listed for the vote (no parent, no hold); the plate hold
//      needs exactly one matching entry.
// Compiled only when WITH_DLAA=1. Render thread only (the game's immediate context).
#pragma once
#ifdef WITH_DLAA

#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstdint>

// Non-blocking GPU timestamp span (same pattern as SceneDlaa's SnapTimer): Begin / End around GPU work, polled with
// DONOTFLUSH; Take() returns the window average since the last Take.
class GpuSpanTimer {
public:
    int  Begin(ID3D11DeviceContext* ctx);        // slot for End(), -1 = this span is not timed (all slots in flight)
    void End(ID3D11DeviceContext* ctx, int slot);
    void Poll(ID3D11DeviceContext* ctx);
    // window statistics since the last Take (n = landed samples; avg / max in ms); restarts the window
    void Take(double* avgMs, double* maxMs, uint64_t* n);
    void Reset();                                // drops the queries (device / context change)
private:
    struct Q { Microsoft::WRL::ComPtr<ID3D11Query> dis, t0, t1; bool inFlight = false; };
    static constexpr int kQ = 6;
    Q        m_q[kQ];
    int      m_head = 0;
    bool     m_broken = false;
    double   m_sum = 0.0, m_max = 0.0;
    uint64_t m_n = 0;
};

class DrawIdRecord {
public:
    static constexpr int kMax = 4096;            // G-buffer draws per pass (flat capture: 742; VR heavy scenes ~1500)
    static constexpr int kVb = 4, kVsCb = 4, kVsSrv = 8, kVsSmp = 2, kVp = 2, kSc = 2;
    static constexpr int kRings = 3;             // distinct VS cbuffer rings mirrored per pass (v0.9.0 r3 finding)
    // per-draw flags
    enum : uint8_t {
        kFInstanced = 1,     // DrawIndexedInstanced: rows 4..7 are not the instances' MVP -> camera R
        kFNoMvp     = 2,     // no usable cb0 window (null / < 128 bytes / not a ring) -> camera R
        kFCabin     = 4,     // viewport MinDepth >= 0.85 (cabin layer, R_cabin)
        kFLate      = 8,     // recorded after the pass's first replay (always replayed, see Replay)
        kFFwd       = 16,    // v0.10.0 phase 3: a FORWARD-pass draw (a record in forward mode, see SetForward)
    };

    DrawIdRecord();
    ~DrawIdRecord();
    DrawIdRecord(const DrawIdRecord&) = delete;
    DrawIdRecord& operator=(const DrawIdRecord&) = delete;

    void Reset();                                // a new pass: releases every held ref
    // v0.10.0 phase 3: FORWARD mode (inject.cpp g_didFwdRing): every recorded draw carries kFFwd and keys of their own (a
    // forward draw only pairs with forward draws); replayed with ReplayForward. Set once, before the first Record.
    void SetForward(bool on) { m_forward = on; }
    bool Forward() const { return m_forward; }
    // ---- v0.10.0 phase 10 FORWARD-DEPTH BATCH (inject.cpp FwdBatch; was: an interleaved re-draw after every game draw) ----
    // SetCapturePs(true) (a forward record, before its first Record): every Record also keeps the draw's PIXEL-shader state
    // (PS, PS cbuffers 0..kPsCb-1 with D3D11.1 offsets, PS SRVs 0..kPsSrv-1, samplers 0..kPsSmp-1; refs held, their resources join
    // the pending-resource set of ReferencesPending). ReplayDepth then draws the draws recorded since its last call AGAIN, in draw
    // order, with the game's VS / IA / RS / viewport AND its pixel shader + resources (alpha-to-coverage needs the game's alpha),
    // into `dsv` with `rt0` as RT0 and the caller's depth-stencil / blend states, through the record's view (SetView: clip and
    // 1/2^shift exactly like the id replay). It does not touch the id cursor: ReplayForward of the same draws follows (ids), or
    // DropIds() releases them when the ids are off. Saves / restores everything it touches (incl. every PS slot it sets).
    static constexpr int kPsCb = 4, kPsSrv = 16, kPsSmp = 8;
    void SetCapturePs(bool on) { m_capPs = on; }
    bool CapturePs() const { return m_capPs; }
    int  PendingDepth() const { return m_n - m_depthTo; }
    // (ReplayDepth: declared after ReplayResult below)
    void DropIds();                              // no id replay for the draws already depth-replayed: released, cursor moved
    // Occlusion init of a forward-depth target (dlaa.ini mv_fwd_depth_cull): one full-screen triangle writes, per target pixel,
    // min over its 2^shift x 2^shift texels of `snapSrv` (the pass's depth snapshot, R32_FLOAT_X8X24 view, reversed-Z: min = the
    // farthest) times (1 - 1e-6) when that value lies in the world range [0.01, 0.9), else 0, through SV_Depth (depth func
    // ALWAYS). Strictly below the snapshot, so it never "wins" pass B / the vote (fw > dt) -- it only lets the GPU reject forward
    // fragments that are behind the scene at all of their full-resolution pixels (they could not win either). `clip` (full-image
    // px, nullable) is scaled like the re-draw's scissor. Saves / restores every state it touches. false = not available.
    static bool InitFwdDepth(ID3D11DeviceContext1* ctx, ID3D11DepthStencilView* dsv, UINT dsvW, UINT dsvH,
                             ID3D11ShaderResourceView* snapSrv, UINT shift, const D3D11_RECT* clip);
    static bool InitFwdReady();
    void Shutdown();                             // + the mirror buffer
    // Render thread, one G-buffer draw (call before or after the game's draw: the bound state is the draw's). Captures
    // the full input state with ~11 getters. false = not recorded (full).
    bool Record(ID3D11DeviceContext1* ctx, UINT indexCount, UINT instanceCount, UINT startIndex, INT baseVertex,
                UINT startInstance, bool instanced);
    int  Count() const { return m_n; }
    int  PendingReplay() const { return m_n - m_replayedTo; }
    // hkMap (WRITE_DISCARD): true when a draw that still waits for its replay reads `res` (VS cbuffer, VB, IB or a VS SRV's
    // resource) -- the caller replays first (the discard would hand the replay new contents)
    bool ReferencesPending(const void* res) const;
    bool HasOpenRing(const void* res) const;
    // Copies the recorded byte range of every ring into the mirror (idempotent). Replay calls it.
    void Flush(ID3D11DeviceContext* ctx);
    // `res` is about to be WRITE_DISCARD-mapped: if it is an open ring, flush it (queued before the discard) and seal it.
    bool OnDiscard(ID3D11DeviceContext* ctx, ID3D11Resource* res);

    // Replays the draws recorded since the last replay into `idRtv` (R16_UINT, cleared to 0 at the pass's first replay)
    // with `roDsv` (read-only depth + stencil view of the scene depth) bound, depth EQUAL, our PS. Saves and restores every
    // piece of state it touches (restoreTargets: also the OM render targets -- a replay in the middle of the pass; at the
    // G-buffer leave the game binds its next targets itself). replayAll: also instanced / no-MVP draws of the first
    // segment (dlaa.ini mv_drawid_instanced). The replayed draws' refs are released. Must run inside the caller's
    // re-entrancy guard (our draws go through the hooked functions).
    struct ReplayResult { int replayed = 0, skipped = 0; bool ok = false; };
    // v0.10.0 phase 3: writes the RED channel only (blend write mask), so on an R16G16_UINT target the forward ids in GREEN
    // are never touched; the first replay's clear zeroes both channels.
    ReplayResult Replay(ID3D11DeviceContext1* ctx, ID3D11RenderTargetView* idRtv, ID3D11DepthStencilView* roDsv,
                        bool restoreTargets, bool replayAll);
    // v0.10.0 phase 10: the batched forward-depth re-draw (see SetCapturePs above)
    ReplayResult ReplayDepth(ID3D11DeviceContext1* ctx, ID3D11RenderTargetView* rt0, ID3D11DepthStencilView* dsv,
                             ID3D11DepthStencilState* dss, ID3D11BlendState* bs, bool restoreTargets);
    // v0.10.0 phase 8 (dlaa.ini mv_inst_replay 1): with replayAll, an instanced / no-MVP draw is replayed only when its
    // pointer-free key took a MOVING rigid parent in the vote lately (DrawIdMv::InstancedWanted) -- every draw is replayed in a
    // PROBE pass (the caller sets it per replay: every 7th pass of the main view -- odd, so both VR eyes probe; always for mirror
    // units). 0 = all, every pass.
    static void SetInstancedProbe(bool probe);
    int  GatedDraws() const { return m_instGated; }      // instanced draws not replayed this pass (mv_inst_replay 1)
    // v0.10.0 phase 14 (dlaa.ini mv_replay_static_*): the caller turns the static gate on around the replays of the MAIN
    // G-buffer passes only (off for mirror units / the harness unless asked): a world, non-instanced, non-cabin draw whose key
    // DrawIdMv::StaticSkip calls static is not replayed (no id pixels; it stays recorded and paired). Not counted in `skipped`.
    static void SetStaticGate(bool on);
    // HARNESS ONLY (v0.10.0 phase 14 round 4): the draws with this FullKey are left out of the G-buffer replay (0 = none) -- a body
    // whose pixels are missing for one frame (the plate-hold test); not counted as static skips. Never set by the DLL
    static void SetTestSkipKey(uint64_t fullKey);
    int  StaticSkipped() const { return m_staticSkipped; }   // draws the static gate skipped this pass
    bool StaticGated() const { return m_staticGated; }       // a G-buffer replay of this pass ran with the gate on
    bool StaticSkippedAt(int i) const {                       // draw i was skipped by the gate (harness id checks)
        return i >= 0 && i < m_n && ((m_staticBits[i >> 5] >> (i & 31)) & 1u) != 0;
    }
    // v0.10.0 phase 3, forward mode: replays the pending forward draws into the GREEN channel of `idRtv` (R16G16_UINT, never
    // cleared here: the G-buffer replay of the same pass cleared it) with `fwdDsv` (the pass's forward-depth target) bound,
    // depth EQUAL, in DRAW order (the forward depth is written with GREATER_EQUAL: the last draw owns a tie). Draw id =
    // idBase + record index + 1 (idBase = the G-buffer record's draw count: one id space with the G-buffer draws); draws
    // whose id would exceed kMax are skipped. Every replay of a pass must use the same idBase (else ReplayFailed()).
    ReplayResult ReplayForward(ID3D11DeviceContext1* ctx, ID3D11RenderTargetView* idRtv, ID3D11DepthStencilView* fwdDsv,
                               bool restoreTargets, UINT idBase);
    UINT IdBase() const { return m_idBase; }             // forward mode: the idBase of this pass's replays (~0u: none)
    bool Cleared() const { return m_cleared; }           // the id target was cleared this pass (a replay ran)
    bool ReplayFailed() const { return m_replayFail; }   // a replay of this pass could not run (ids incomplete)
    int  Segments() const { return m_segments; }         // replays this pass
    int  ReplayedDraws() const { return m_replayed; }
    int  SkippedDraws() const { return m_skipped; }
    int  OverflowDraws() const { return m_overflow; }    // forward mode: skipped because idBase + index + 1 > kMax

    // ---- v0.10.0 phase 9 REPLAY VIEW (VR cost: NGX only reads the DLAA-area rect) ----
    // clip (full-image px, nullptr = none): every replayed draw is scissored to (its own scissor when its RS enables one) n clip;
    // a draw whose RS has no scissor is replayed through a scissor-enabled copy of that RS (ScissorRs: same raster rules, so the
    // depth of every pixel is unchanged). shift (forward mode only, 0 / 1): the target is 1/2^shift of the image in x and y --
    // viewport and scissor are scaled exactly like inject.cpp's forward-depth re-draw (ScaleViewport / ScaledScissor), and the
    // id target is then its OWN single-channel R16_UINT texture (the forward ids are written to RED, not GREEN). Set before each
    // Replay / ReplayForward of a pass (Reset clears it); DrawIdMv reads Clipped / ClipRect (the vote grid) and Shift at the
    // blit. A record that never gets a view (mirror units, preview, the harness' phase-8 scenes) replays exactly as phase 8.
    void SetView(const D3D11_RECT* clip, UINT shift) {
        m_clip = clip != nullptr; m_clipRect = clip ? *clip : D3D11_RECT{ 0, 0, 0, 0 }; m_shift = shift ? 1u : 0u;
    }
    bool       Clipped() const { return m_clip; }
    D3D11_RECT ClipRect() const { return m_clipRect; }
    UINT       Shift() const { return m_shift; }
    // shared with inject.cpp's forward-depth re-draw (the replay must reproduce its depth bit for bit): the viewport of a
    // 1/2^shift target (origin and size halved; depth range unchanged), the effective scissor of a draw (its own when its RS
    // enables one, else unbounded) n clip (nullptr = none), mapped to the 1/2^shift target (left / top floor, right / bottom
    // ceil). false = empty (nothing of the draw can be drawn).
    static void ScaleViewport(D3D11_VIEWPORT* vp, UINT n, UINT shift);
    static bool ScaledScissor(bool rsScissor, const D3D11_RECT* own, const D3D11_RECT* clip, UINT shift, D3D11_RECT* out);
    // the game's rasterizer state with ScissorEnable on (the state itself when it already has it; a copy created once per state
    // object and kept, ref held on both). *hadScissor = the game's state enabled the scissor. nullptr = could not be created (the
    // caller replays / re-draws without the clip). Render thread only.
    static ID3D11RasterizerState* ScissorRs(ID3D11Device* dev, ID3D11RasterizerState* game, bool* hadScissor);
    static uint64_t ScissorRsFails();            // copies that could not be created (stats)

    // ---- pairing inputs (CameraMv / DrawIdMv at the blit) ----
    uint64_t FullKey(int i) const;               // IB, VB0, offsets, counts, base vertex, VS, instanced
    uint64_t LooseKey(int i) const;              // indexCount, startIndex, baseVertex, VS (no buffer pointers)
    // v0.10.0 phase 8 (mv_inst_replay): indexCount, instance count, VS, input layout -- no start index / base vertex: a plate whose
    // vertices the CPU writes into a dynamic buffer every frame keeps its shape key while its loose key changes every frame
    uint64_t ShapeKey(int i) const;
    UINT     MirrorOff(int i) const;             // byte offset of the MVP (rows 4..7) in the mirror, 0xFFFFFFFF = none
    uint8_t  Flags(int i) const;
    float    VpMin(int i) const;
    float    VpMax(int i) const;
    // v0.10.0 phase 14 round 4: the fractional part of the draw's viewport origin (the per-frame sub-pixel jitter), packed as two
    // int16 of 1/16384 px (x low, y high) -- pixel centroids of different frames are compared jitter-free (the plate hold)
    UINT     VpJitterPacked(int i) const;
    UINT     InstanceCount(int i) const;         // v0.10.0 phase 14 round 4: 1 for DrawIndexed
    UINT     IndexCount(int i) const;            // v0.10.0 phase 4 (dump): the draw's IndexCount
    void     CbWindow(int i, UINT* first, UINT* num) const;   // v0.10.0 phase 4 (dump): VS cb0 first / num constants
    // v0.10.0 phase 21 (dump / pixel probe): the game's pixel shader of draw i (identity only, never dereferenced; null = none /
    // unknown). inject.cpp hkPSSetShader keeps the game's current PS in NoteGamePs; every Record copies it. PsInfo = the DXBC
    // flags of a shader through the lookup inject.cpp registers (SetPsInfoFn): bit 0 scanned, bit 1 discard (cut-out), bit 2
    // writes SV_Depth (oDepth / oDepthGE / oDepthLE); 0 = unknown (created before the hook, not parsable, no lookup: harness)
    const void* Ps(int i) const;
    enum : uint8_t { kPsScanned = 1, kPsDiscard = 2, kPsDepth = 4 };
    typedef uint8_t (*PsInfoFn)(const void* ps);
    static void NoteGamePs(const void* ps) { s_gamePs = ps; }
    static void SetPsInfoFn(PsInfoFn fn) { s_psInfo = fn; }
    static uint8_t PsInfo(const void* ps) { return s_psInfo ? s_psInfo(ps) : 0; }
    ID3D11ShaderResourceView* MirrorSrv() const { return m_mirrorSrv.Get(); }
    bool Usable() const { return m_n > 0 && !m_flushFail && !FlushPending() && m_mirrorSrv; }
    int  Instanced() const { return m_nInst; }
    int  NoMvp() const { return m_nNoMvp; }
    int  Full() const { return m_full; }         // draws not recorded (kMax reached)
    int  Rings() const { return m_nRings; }
    int  Seals() const { return m_seals; }

    // ---- device objects shared by all records: our PS, the EQUAL depth state, the immutable draw-id cbuffer ----
    static void RegisterShaders();               // setup thread, before ShaderCache::Start
    static bool InitDevice(ID3D11Device* dev);   // lazily; false = not (yet) available (shaders still compiling / failed)
    static bool DeviceReady();
    static void ShutdownDevice();                // device change

private:
    ReplayResult ReplayImpl(ID3D11DeviceContext1* ctx, ID3D11RenderTargetView* idRtv, ID3D11DepthStencilView* dsv,
                            bool restoreTargets, bool replayAll, bool forward, UINT idBase,
                            ID3D11DepthStencilState* depthDss = nullptr, ID3D11BlendState* depthBs = nullptr);   // phase 10:
                                                 // depthDss != null = the forward-depth batch (ReplayDepth)
    struct PsState;                              // v0.10.0 phase 10: per-draw pixel-shader state (draw_ids.cpp)
    void ReleasePs(int from, int to);
    struct Ring {
        ID3D11Buffer* buf;                       // ref held between the slot's first use and Reset
        UINT bytes, base, lo, hi;                // [lo, hi) still to copy
        int  draws;
        bool sealed;
    };
    struct State;                                // per-draw captured state (draw_ids.cpp)
    struct Key { uint64_t full, loose; UINT off; uint8_t flags; float vpMin, vpMax; UINT ic, cbFirst, cbNum;   // v0.10.0 phase 4: + ic / cb0 (dump)
                 uint64_t shape;                 // v0.10.0 phase 8: ShapeKey
                 UINT jit, inst;                 // v0.10.0 phase 14 round 4: VpJitterPacked, InstanceCount
                 const void* ps; };              // v0.10.0 phase 21: the game's PS at Record (identity; dump / probe)
    static const void* s_gamePs;                 // v0.10.0 phase 21: NoteGamePs
    static PsInfoFn    s_psInfo;                 //   SetPsInfoFn
    bool FlushPending() const {
        for (int k = 0; k < m_nRings; ++k) if (m_rings[k].hi > m_rings[k].lo) return true;
        return false;
    }
    UINT AddRing(ID3D11Buffer* cb, UINT mvpByteOff);   // mirror byte offset or 0xFFFFFFFF
    bool FlushRing(ID3D11DeviceContext* ctx, int k);
    bool EnsureMirror(ID3D11DeviceContext* ctx, UINT need);
    void ReleaseRange(int from, int to);
    void NoteRes(const void* p);                 // pending-resource set (hkMap check)
    State*   m_st = nullptr;                     // kMax entries, allocated on first Record
    Key*     m_key = nullptr;
    int      m_n = 0, m_replayedTo = 0, m_releasedTo = 0;
    int      m_full = 0, m_nInst = 0, m_nNoMvp = 0;
    bool     m_cleared = false, m_replayFail = false;
    int      m_segments = 0, m_replayed = 0, m_skipped = 0;
    bool     m_forward = false;                  // v0.10.0 phase 3: forward mode (see SetForward)
    UINT     m_idBase = 0xFFFFFFFFu;             // forward mode: idBase of this pass's replays
    int      m_overflow = 0;
    int      m_instGated = 0;                    // v0.10.0 phase 8 (mv_inst_replay)
    int      m_staticSkipped = 0;                // v0.10.0 phase 14 (mv_replay_static_*): draws skipped by the static gate
    bool     m_staticGated = false;              //   a replay of this pass ran with the gate on
    uint32_t m_staticBits[kMax / 32] = {};       //   bit i = draw i skipped (StaticSkippedAt)
    bool     m_capPs = false;                    // v0.10.0 phase 10: SetCapturePs
    PsState* m_ps = nullptr;                     //   kMax entries, allocated on the first Record with m_capPs
    int      m_depthTo = 0;                      //   ReplayDepth cursor (draws [m_depthTo, m_n) wait for their depth re-draw)
    bool     m_clip = false;                     // v0.10.0 phase 9: replay view (SetView)
    D3D11_RECT m_clipRect = { 0, 0, 0, 0 };
    UINT     m_shift = 0;
    Ring     m_rings[kRings] = {};
    int      m_nRings = 0, m_lastRing = -1, m_seals = 0;
    UINT     m_need = 0;
    bool     m_copiedAny = false, m_flushFail = false;
    const void* m_otherBuf = nullptr;            // last cb0 that is not a ring (< 64 KB), identity
    Microsoft::WRL::ComPtr<ID3D11Buffer>             m_mirror;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_mirrorSrv;
    UINT     m_mirrorBytes = 0;
    // pending-resource set: open addressing, generation stamped (cleared at every replay)
    static constexpr int kResSet = 2048;
    const void* m_res[kResSet] = {};
    uint32_t m_resGen[kResSet] = {};
    uint32_t m_gen = 1;
    int      m_resFill = 0;
    const void* m_lastRes[20] = {};              // last pointer per captured slot (dedupe before the set): 0..3 VB,
                                                 // 4 IB, 5..8 VS cb, 9..16 VS SRV
    const void* m_srvKey[4] = {};                // SRV -> resource cache (identity only)
    const void* m_srvRes[4] = {};
    int      m_srvNext = 0;
};

// Per-eye pairing + R table (one per CameraMv). See the header comment.
class DrawIdMv {
public:
    static constexpr int  kMax = DrawIdRecord::kMax;
    static constexpr UINT kRStride = 5;          // float4 per draw in the R table: 4 rows of R + (state, vpMin, vpMax, dev)
    // R table states (RTab[i*5+4].x)
    enum { kStUnpaired = 1, kStStatic = 2, kStMover = 3, kStInstanced = 4, kStNoMvp = 5 };
    static void RegisterShaders();
    // dlaa.ini: mv_drawid_max_m (largest plausible origin displacement per frame, clip units ~ m), mv_drawid_snap_px
    static void SetParams(float maxM, float snapPx);
    static float MaxM();
    static float SnapPx();
    // v0.10.0 phase 3, dlaa.ini mv_camr_consensus (default on): the camera R of each layer = the largest cluster of agreeing
    // per-draw R's (see the kDrawIdCs header comment in draw_ids.cpp); off = the medoid of CameraMv's pass A as before
    static void SetConsensus(bool on);
    static bool Consensus();
    // v0.10.0 phase 4, dlaa.ini mv_fwd_attach_m (0..5 m, default 0.5; 0 = off): the attach radius of forward draws / forward
    // pixels to the G-buffer mover they sit on (CSAttach per draw; CameraMv pass B and the Ctrl+F6 view per pixel)
    static void SetAttach(float m);
    static float AttachM();
    // v0.10.0 phase 5, dlaa.ini mv_drawid_twin (default on): unpaired draws inherit the R / state of their matrix twin
    static void SetTwin(bool on);
    static bool Twin();
    // v0.10.0 phase 6, dlaa.ini mv_drawid_parent (default on): unpaired draws take the R / state of their rigid parent (the
    // pixel-neighbour vote); SetParentMutation = harness mutations only (bit 0: the conjugated row-vector formula instead of
    // R_parent, bit 1: no depth test in the vote; phase 14 round 6: bit 2 = the origin-free rule off, bit 3 = multi-instance draws
    // listed for the vote / held again, the round-4 behaviour; round 7: bit 4 = the round-6 origin-free rule) -- never set by the DLL
    static void SetParent(bool on);
    static bool Parent();
    static void SetParentMutation(int bits);
    // v0.10.0 phase 6b, dlaa.ini mv_drawid_instanced (default on): replayed instanced / no-MVP draws (a close car's licence
    // plate is a DrawIndexedInstanced draw) take part in the rigid-parent vote like unpaired draws -- they take the R / state of
    // the draw they sit on (never a parent source). Off = they keep the camera R as before.
    static void SetInstanced(bool on);
    static bool Instanced();
    // v0.10.0 phase 6c: the march reach of the rigid-parent vote (px per axis; 0 = the default 32). HARNESS ONLY (a short reach
    // makes plate text need the 2nd pass) -- never set by the DLL
    static void SetParentReach(unsigned px);
    // v0.10.0 phase 8 performance (dlaa.ini, fast setting = default): mv_vote_res (4: the rigid-parent vote and its pixel count on
    // every 4th pixel in x / y with 4 px march steps, small listed draws keep the 2 px grid; 2: every 2nd pixel, phase 6c),
    // mv_vote_march_cap (marches per frame over all listed draws; 0 = per-draw budget only, phase 6c), mv_cons_cands (world
    // consensus candidates; the cabin layer half of it, <= kCandCabin; 128 = phase 3)
    static void SetVoteRes(unsigned px);
    static unsigned VoteRes();
    static void SetMarchCap(unsigned n);
    static unsigned MarchCap();
    static void SetConsCands(unsigned n);
    static unsigned ConsCands();
    // v0.10.0 phase 8 (dlaa.ini mv_inst_replay): 0 = every instanced / no-MVP draw is replayed every pass (phase 6b); 1 = only the
    // ones whose pointer-free key took a MOVING rigid parent in the last kInstKeepFrames frames (a car's licence plate), all of
    // them in a probe pass (every 7th; that is how a new plate is found). SetFrame: the Present counter (the keep window).
    static void SetInstReplay(int mode);
    static int  InstReplay();
    // v0.10.0 phase 10 (dlaa.ini mv_parent_max_listed, 8..64, default 64; perf_profile low: 32): how many unpaired / instanced
    // draws one pass lists for the rigid-parent vote (the vote tables / marches scale with it). The genuine unpaired draws keep
    // their draw-order priority, the instanced ones fill the rest by on-screen pixels (as with 64); the rest = "over the cap".
    static void SetParentMaxListed(unsigned n);
    static unsigned ParentMaxListed();
    static void SetFrame(uint64_t frame);
    static bool InstancedWanted(uint64_t looseKey);
    static int  InstancedKnown();                 // keys in the "took a moving parent lately" table
    // ---- v0.10.0 phase 14 STATIC REPLAY SKIP (see the header comment, step 11) ----
    // dlaa.ini mv_replay_static_frames (0 = off, 1..60; default 6): a key whose draws were paired and static by their own pair (none
    // moving; draws without a partner do not count either way) in this many frames in a row is not replayed; mv_replay_static_every (1..16; default 4): such a key is still replayed on 1
    // frame of every `every` (staggered by the key's hash), 1 = every frame (= off). A change clears the table.
    static void SetReplayStatic(unsigned frames, unsigned every);
    static unsigned ReplayStaticFrames();
    static unsigned ReplayStaticEvery();
    // round 3 (dlaa.ini mv_replay_static_eps_px, 0..0.1, default 0.02): only a DEEP static draw (its own motion within this many px
    // of the camera's: world geometry) builds a streak; a draw static within mv_drawid_snap_px but not deep (a vehicle at the
    // player's speed) counts as moving for the gate, and a key that moved in the last 60 frames is never skipped
    static void SetReplayStaticEps(float px);
    static float ReplayStaticEps();
    // DrawIdRecord::ReplayImpl (gate on): true = do NOT replay the draw with this FullKey this frame
    static bool StaticSkip(uint64_t fullKey);
    // forget every streak (everything is replayed again for mv_replay_static_frames frames); why = kStatClr*
    enum { kStatClrHistory = 0, kStatClrCamR, kStatClrRecord, kStatClrFifo, kStatClrConfig, kStatClrN };
    static void StaticClear(int why);
    static uint64_t StaticClears(int why);
    // keys in the table seen lately (*atThreshold: of them at / over the streak threshold, *held: held by a rigid-parent mark)
    static int  StaticKeys(int* atThreshold, int* held);
    // polls the diagnostics readbacks of every main (non-quiet) unit now (non-blocking) -- inject.cpp DidReplay calls it right
    // before a gated replay, so a verdict that landed since the last blit (a parked car that started) counts one frame earlier
    static void PollMain(ID3D11DeviceContext* ctx);
    static constexpr int kCandWorld = 128, kCandCabin = 64;   // consensus candidates per layer (CPU-chosen, evenly spaced)
    static constexpr int kConsMin = 24;                       // smallest cluster that may replace the medoid (draws)

    DrawIdMv();
    ~DrawIdMv();
    DrawIdMv(const DrawIdMv&) = delete;
    DrawIdMv& operator=(const DrawIdMv&) = delete;

    bool Init(ID3D11Device* dev);                // lazily; false = not available (retried while shaders compile)
    bool Ready() const { return m_ready; }
    // CPU: groups this pass's draws with the eye's previous pass (full key, then the pointer-free key for the orphans),
    // uploads the tables. Returns the draw count (0 = nothing to do: no / unusable record -> history forgotten).
    // v0.10.0 phase 3: `fwd` (nullable) = the pass's FORWARD record; its draws are appended after the G-buffer draws (ids
    // nG + 1 ..) when it is usable, was replayed with idBase == rec->Count() and nothing failed (else nF = 0). `nearM` =
    // the world voters' near filter (mv_near_reject_m); `freshMedoid` = pass A wrote this frame's medoid into the solve
    // buffer (the |consensus - medoid| statistic is only taken then).
    // v0.10.0 phase 8: `noMedoid` bit L = layer L has NO medoid this frame (its camera candidates were dropped, see
    // ConsHealthy): pass A wrote nothing for it, so a consensus found here also writes that layer's info rows (world: R_ego =
    // the camera R, InvRow3 from the winning draw); no consensus = the solve buffer keeps the last frame's R of that layer.
    // v0.10.0 phase 9: `idsBound` = Dispatch will get the pass's draw-id target (the rigid-parent bit of the folded dispatch)
    uint32_t Prepare(ID3D11DeviceContext* ctx, const DrawIdRecord* rec, uint32_t fullW, uint32_t fullH,
                     const DrawIdRecord* fwd = nullptr, float nearM = 30.0f, bool freshMedoid = true, uint32_t noMedoid = 0,
                     bool idsBound = true);
    // GPU: gather (MVPs from the mirror) + match (nearest, mutual) + pair + consensus (vote, pick: writes the camera R into
    // the solve buffer through `solveUav` when a cluster is found) + resolve (R per draw) -- after pass A (needs the camera
    // solve buffer of this frame). `solveUav` null = no consensus (the medoid stays).
    // v0.10.0 phase 6: idSrv / depthSrv / fwdDepthSrv = the pass's draw-id target, scene depth twin and forward depth (pass B's
    // inputs, full size) for the rigid-parent vote; null id / depth = no vote this pass (the phase-5 chain only)
    // v0.10.0 phase 9: the pixel count + vote grid cover only the G-buffer record's replay clip (Clipped / ClipRect: the
    // replayed region, everything outside holds id 0); a forward record replayed at 1/2 resolution (Shift 1) has its forward
    // depth (fwdDepthSrv) and its forward ids (fwdIdSrv, R16_UINT, nullable otherwise) at 1/2 size -- read at pixel >> 1.
    // Fewer dispatches (dlaa.ini mv_fold_dispatch): 1 (phase 9) = pick + resolve + inherit run as ONE single-group dispatch
    // (CSPickResolve) and the rigid-parent list + twin + attach as another (CSListTwin); 0 = the phase-8 chain; 2 (phase 18,
    // default) = the list + twin + attach group of 1, the pick with its statistic spread over a group (CSPickPar) and resolve +
    // inherit as ONE wide dispatch (CSResolveInherit) -- the single group resolved ~2200 draws 9 per thread (0.15-0.18 ms / eye).
    // v0.10.0 phase 18 (dlaa.ini mv_vote_compact, default 1): the rigid-parent vote runs over the vote-grid tiles that hold a
    // countable pixel (CSParentCount lists them) and marches one thread per (sample, direction) (CSParentCollect + CSParentMarch);
    // 0 = the phase-8 vote grid (one thread marches a sample's 8 directions in a row). Same marches, same vote tables.
    void Dispatch(ID3D11DeviceContext* ctx, const DrawIdRecord* rec, ID3D11ShaderResourceView* solveSrv,
                  const DrawIdRecord* fwd = nullptr, ID3D11UnorderedAccessView* solveUav = nullptr,
                  ID3D11ShaderResourceView* idSrv = nullptr, ID3D11ShaderResourceView* depthSrv = nullptr,
                  ID3D11ShaderResourceView* fwdDepthSrv = nullptr, ID3D11ShaderResourceView* fwdIdSrv = nullptr);
    static void SetFold(int mode);                // v0.10.0 phase 9 (dlaa.ini mv_fold_dispatch 0 / 1 / 2; phase 18: 2 = default)
    static int  Fold();
    static void SetVoteCompact(bool on);          // v0.10.0 phase 18 (dlaa.ini mv_vote_compact)
    static bool VoteCompact();
    // v0.10.0 phase 18 (dlaa.ini mv_cons_min_cabin, 0 = kConsMin as the world layer, else 8..64): the cabin layer's consensus
    // camera R needs this many agreeing draws. In game the cabin never reached kConsMin (24) -- its medoid and its 24 candidate
    // copies per pass stayed; the 'cabin: consensus' field of the MV draw-ids line shows the largest cluster it had
    static void SetConsMinCabin(unsigned n);
    static unsigned ConsMinCabin();
    // v0.10.0 phase 19 (dlaa.ini mv_cabin_small, 0 = off, default 12, else 2..64): a cabin layer with fewer PAIRED draws than this
    // (a car: 7 cabin draws, 3 of them camera-relative overlays whose own R is the head rotation only) ranks its candidates by
    // the summed IndexCount of the voters that agree with them (a cluster of 1 is enough: the largest real cabin part decides)
    // and uses that R when pass A's medoid disagrees with it at any member (> 0.01 px); else the medoid stays (bit-identical)
    static void SetCabinSmall(unsigned n);
    static unsigned CabinSmall();
    // v0.10.0 phase 19: the harness mutation "the MVP-shape rule off" (SetParentMutation bit 5) -- pass A reads it too
    static bool MvpShapeOff();
    // after pass B: commits the prepared pass as this eye's previous one, queues the diagnostics readback
    void Finish(ID3D11DeviceContext* ctx);
    void Forget() { m_prevN = 0; m_prepN = 0; m_consRun[0] = m_consRun[1] = 0;     // no history (the next pass pairs nothing;
                    m_uholdValid[0] = m_uholdValid[1] = false;      // v0.10.0 phase 8: the consensus health restarts too;
                    if (!m_quiet) StaticClear(kStatClrHistory); }  // phase 14: a main unit's lost history also forgets every
                                                 // static streak; round 4: and the plate-hold table)
    // v0.10.0 phase 8: the prepared pass has a previous pass and at least one partner group (the draw-id pairing can produce a
    // camera R: a world "miss" in the sense of pass A is decided by this when the world medoid is dropped)
    bool PreparedPairable() const { return m_prepN && m_prepHist && m_nGroups > 0; }
    // v0.10.0 phase 8 (dlaa.ini mv_medoid_drop, frames; 0 = never): the consensus of layer L was used with >= kConsMin draws in
    // the last N landed readbacks in a row -> the old medoid path (camera candidate copies, pass A) may stop for that layer; the
    // first readback without it re-arms the medoid (counted in Stats::medoidRearms)
    static void SetMedoidDrop(unsigned frames);
    static unsigned MedoidDrop();
    bool ConsHealthy(int layer) const;
    // v0.10.0 phase 4: log the per-draw state of the NEXT prepared pass (forward draws + G-buffer draws with IndexCount
    // <= 36), see the header. The lines land 2-4 frames later (non-blocking readback). false = a dump is still in flight.
    // v0.10.0 phase 21 PIXEL PROBE: probeU / probeV in [0, 1] (uv of the full image; < 0 = no probe) -- pass B of the dumped
    // pass also records, for a 5 x 5 grid of pixels kProbeStep px apart around that point, its inputs and decisions (scene /
    // forward depth, which won, the layer, the R it took, the draw ids, the final MV); DumpPoll logs one 'MV probe' line per
    // pixel with the facts of both draws (CameraMv binds the probe buffer through ProbePacked / ProbeUav)
    bool RequestDump(const char* why, float probeU = -1.0f, float probeV = -1.0f);
    static constexpr int  kProbeN = 25, kProbeStep = 48;
    static constexpr UINT kProbeStride = 128u;   // bytes per probe pixel record (pass B, kReprojDepthShader)
    // pass B of the prepared pass: the probe centre packed for the shader (x | y << 16), 0 = no probe this pass
    uint32_t ProbePacked() const { return m_dumpPrep ? m_probePacked : 0u; }
    // pass B (CameraMv::DispatchReproj): the probe buffer, cleared, for u4 (null = none); x0 / y0 / w / h = pass B's crop
    ID3D11UnorderedAccessView* ProbeUav(ID3D11DeviceContext* ctx, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h);
    bool DumpBusy() const { return m_dumpArm || m_dumpPrep || m_dumpPending; }
    void SetQuiet(bool on) { m_quiet = on; }     // v0.10.0 phase 2 (mirror units): no low-pair WARNING lines (counted only)
    uint32_t PreparedN() const { return m_prepN; }
    uint32_t PreparedG() const { return m_prepNG; }  // v0.10.0 phase 3: G-buffer draws of the prepared pass (ids 1..nG)
    ID3D11ShaderResourceView*  RSrv() const { return m_rtabSrv.Get(); }
    ID3D11UnorderedAccessView* CntUav() const { return m_cntUav.Get(); }   // pass B coverage counters (u3)

    struct Stats {
        uint64_t frames = 0;                     // Prepare calls with a usable record
        uint64_t draws = 0, instanced = 0, noMvp = 0;
        uint64_t looseDraws = 0;                 // cur draws grouped through the pointer-free key
        uint64_t noGroup = 0, bigGroup = 0;      // eligible draws without any partner candidate / in an oversized group
        uint64_t histFrames = 0;                 // frames that had a previous pass to pair with
        // diagnostics readback (GPU counters, 2-3 frames late; eye's frames that landed)
        uint64_t rbFrames = 0, rbHistFrames = 0;
        uint64_t st[8] = {};                     // resolve state counts (kSt*)
        uint64_t loosePaired = 0;
        uint64_t trackRejects = 0;               // mover verdicts of ambiguous groups without a consistent track
        uint64_t worldPx = 0, idPx = 0, moverPx = 0, unpairedPx = 0;   // pass B, 1-in-64 grid
        uint64_t lowPairFrames = 0;              // landed frames with history whose paired share was < 50 %
        uint64_t cpuTicks = 0;                   // QPC ticks in Prepare
        // v0.10.0 phase 3: consensus camera R per layer (0 world, 1 cabin; read back with the diagnostics, frames that landed)
        uint64_t consFrames[2] = {}, consFallback[2] = {}, consCluster[2] = {};
        uint64_t consFallCluster[2] = {};        // v0.10.0 phase 18: the largest agreeing cluster in the fallback frames (sum)
        uint64_t consDisFrames[2] = {};          // consensus frames that had a fresh medoid to compare with
        double   consDisSum[2] = {};             // sum of the per-frame avg |consensus - medoid| (px)
        uint64_t consDisOver1[2] = {};           // frames whose avg |consensus - medoid| > 1 px
        uint64_t consNearVoters = 0;             // world consensus frames whose voters had to include near (< mv_near_reject_m)
                                                 // draws: fewer than kConsMin far ones (the own truck may vote then)
        // v0.10.0 phase 3: forward draws (prepared) + pass B forward pixels (1-in-64 grid)
        uint64_t fwdDraws = 0, fwdDropped = 0;   // forward draws in the tables / passes whose forward record was not usable
        uint64_t fwdPaired = 0, fwdMovers = 0;   // forward draws paired (static + mover) / movers (read back)
        uint64_t fwdPx = 0, fwdIdPx = 0, fwdMoverPx = 0;
        // v0.10.0 phase 4 attach: forward draws that took a G-buffer mover's R / kept their own (agreed) -- read back;
        // pass B forward pixels that took the mover's R under them (1-in-64 grid)
        uint64_t fwdAttached = 0, fwdAttachKept = 0, fwdAttachPx = 0;
        uint64_t dumps = 0;                      // dumps written
        // v0.10.0 phase 5 (read back): state-1 draws looked up for a twin, twins found (of them: movers, forward draws),
        // G-buffer state-1 draws attached to a mover by origin
        uint64_t twinLookups = 0, twins = 0, twinMovers = 0, twinFwd = 0, gbufAttached = 0;
        // v0.10.0 phase 6 (read back): unpaired draws listed for the vote / over the 64 cap, parents found (movers, forward,
        // via an unpaired neighbour), listed draws without any vote / without a winner, parents that replaced a twin / origin
        // attach (with another state), votes cast, twin lookups skipped because the MVP is the bare projection (view space)
        uint64_t parListed = 0, parOver = 0, parents = 0, parMovers = 0, parFwd = 0, parChain = 0;
        uint64_t parNoVotes = 0, parNoWinner = 0, parOverrode = 0, parOverrodeDiff = 0, parVotes = 0, twinViewSkip = 0;
        uint64_t parLeft = 0;                    // listed draws left with neither a parent nor a twin / origin attach
        // (phase 6c: parChain = parents won in the 2nd pass, through a listed neighbour that took its motion in the 1st)
        // v0.10.0 phase 6b (read back): instanced / no-MVP draws listed as U (parInstListed, of parInstVis candidates with
        // on-screen pixels) and the ones that took a rigid parent (parInst)
        uint64_t parInstListed = 0, parInst = 0, parInstVis = 0;
        // v0.10.0 phase 6c (read back, marching vote): listed draws left without a parent because every source within reach
        // was at another depth (parDepthRej; parNoVotes = no source within reach at all, parNoWinner = votes but no winner),
        // 1st-pass marches / marched px (avg = parMarchPx / parMarches) / depth-rejected marches / marches without a source
        uint64_t parDepthRej = 0, parMarches = 0, parMarchPx = 0, parRejMarches = 0, parNoSrcMarches = 0;
        uint64_t parNoPixel = 0;                 // listed draws with no sampled pixel at all (hidden / sub-pixel)
        // v0.10.0 phase 8 (read back): listed draws with pixels, of them FINE (2 px grid), frames whose vote grids were skipped
        // (no listed draw with pixels: the indirect args were 0)
        uint64_t parVis = 0, parFine = 0, parSkipped = 0;
        // v0.10.0 phase 8: the medoid of a layer re-armed (its consensus failed after being healthy), prepared frames without a
        // world / cabin medoid
        uint64_t medoidRearms[2] = {}, noMedoidFrames[2] = {};
        uint64_t instMoverDraws = 0;
        uint64_t consSmall[2] = {};              // frames without a medoid whose camera R came from a cluster of 8..23 draws             // instanced / no-MVP draws that took a MOVING parent (read back; mv_inst_replay)
        // v0.10.0 phase 14 (read back, mv_replay_static_*): readbacks that fed the static table, G-buffer world draws judged in
        // them, of them paired + static by their own pair / moving (reset their key) / neutral (no partner), rigid-parent marks
        uint64_t statFeeds = 0, statDraws = 0, statStatic = 0, statParents = 0, statMoving = 0, statNeutral = 0;
        uint64_t statShallow = 0;                // round 3: static within the snap but not deep (counted among the moving)
        // round 4 (read back): listed draws that kept last pass's moving parent (plate hold) / votes whose moving candidate was
        // vetoed by flush contact with the static world (grass beside a passing car)
        uint64_t parHolds = 0, parVetoes = 0;
        uint64_t parClutter = 0;                 // round 4: moving candidates refused to a multi-instance (clutter) draw
        // round 6 (read back): paired draws with an ORIGIN-FREE (camera-relative) MVP resolved static (their own R is the camera
        // rotation only); the instanced draws' instance counts as the shaders saw them: sum, and draws per count (index 7 = 7+)
        uint64_t originFree = 0, instSum = 0, instHist[8] = {};
        // round 7 (read back, Cnt [224] / [229]): paired draws whose MVP origin passed the DEPTH part of the origin-free rule
        // (|w| <= 0.20 m) but failed the LATERAL part (|x| or |y| > 0.50 clip); own-pair movers with their origin |w| < 0.5 m
        uint64_t originDepthOnly = 0, originMoverNear = 0;
        // v0.10.0 phase 18 (read back, mv_vote_compact): readbacks of a compact vote, vote-grid tiles listed (sum) / of the grid
        // (sum), march items of the 1st / 2nd pass (sum), items over the cap (marched in place), passes whose tiles overflowed
        uint64_t cvFrames = 0, cvTiles = 0, cvGrid = 0, cvItems1 = 0, cvItems2 = 0, cvItemOver = 0, cvTileOver = 0;
        // v0.10.0 phase 19 (read back): small cabin layer (mv_cabin_small) -- readbacks whose cabin R came from the heaviest
        // cluster / that kept the medoid (it agreed); the last such winner's IndexCount, weight (summed IndexCount) and its
        // max |cluster R - medoid| px at the members (-1 = not compared); cabin voters (paired, not camera-relative) / paired
        // cabin draws (sums); G-buffer draws whose cb0 rows 4..7 are not an MVP (MVP-shape rule; of them cabin) -- sums
        uint64_t cabSmallUsed = 0, cabSmallKept = 0;
        uint32_t cabSmallIc = 0, cabSmallW = 0;
        float    cabSmallDev = -1.0f;
        uint64_t cabVoters = 0, cabPaired = 0, notMvp = 0, notMvpCabin = 0;
    };
    const Stats& GetStats() const { return m_stats; }
    // v0.10.0 phase 3: largest per-frame avg |consensus - medoid| (px, layer 0 / 1) since the last call (restarts it)
    float TakeConsDisMax(int layer) { const float v = m_consDisMax[layer & 1]; m_consDisMax[layer & 1] = 0.0f; return v; }
    // v0.10.0 phase 19: largest |small-cluster cabin R - medoid| px of the readbacks that used it, since the last call (restarts it)
    float TakeCabSmallDevMax() { const float v = m_cabSmallDevMax; m_cabSmallDevMax = 0.0f; return v; }
    // v0.10.0 phase 14 round 7: since the last call (restarts it): o[0..2] = the largest |x|, |y|, |w| of MVP column 3 among the
    // paired draws that pass the whole origin-free rule (0 = none), o[3] = the smallest origin |w| of an own-pair mover (-1 = no
    // mover), o[4] = the smallest max(|x|, |y|) among the paired draws that passed the DEPTH part but failed the lateral one (-1 =
    // none; the nearest lateral miss)
    void TakeOriginDiag(float o[5]) {
        for (int q = 0; q < 3; ++q) { o[q] = m_ofMax[q]; m_ofMax[q] = 0.0f; }
        o[3] = m_ofMoverMinW; m_ofMoverMinW = -1.0f;
        o[4] = m_ofMissMin; m_ofMissMin = -1.0f;
    }
    // v0.10.0 phase 3: the last landed readback's consensus verdict per layer (0 none, 1 consensus, 2 medoid fallback) and
    // cluster size -- the harness reads it
    int LastConsState(int layer) const { return m_lastCons[layer & 1]; }
    uint32_t LastConsCluster(int layer) const { return m_lastConsN[layer & 1]; }
    GpuSpanTimer& PairTimer() { return m_pairTimer; }
    GpuSpanTimer& PassBTimer() { return m_passBTimer; }

private:
    void Poll(ID3D11DeviceContext* ctx);
    bool m_ready = false, m_failed = false;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csGather, m_csMatch, m_csResolve;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csPair, m_csVote, m_csPick;   // v0.10.0 phase 3 (consensus camera R)
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csAttach;                       // v0.10.0 phase 4 (attach)
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csInherit1, m_csTwin;           // v0.10.0 phase 5 (matrix twins)
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csParList, m_csParVote, m_csParPick1, m_csParPick2;   // v0.10.0 phase 6
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csParCount;                     // v0.10.0 phase 6b (instanced pixel count)
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csPickRes, m_csListTwin;       // v0.10.0 phase 9 (folded dispatches)
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csPickPar, m_csResInh;         // v0.10.0 phase 18 (mv_fold_dispatch 2)
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csParCollect, m_csParMarch;    // v0.10.0 phase 18 (mv_vote_compact)
    // v0.10.0 phase 18 compact vote: tiles + march items (raw, ~2.3 MB, made on first use) and the march DispatchIndirect args
    // (2 x (groups, 1, 1); reset from the CPU each pass, counted up by CSParentCollect)
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_parList, m_marchArgs;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_parListUav, m_marchArgsUav;
    bool m_parListFailed = false;
    bool EnsureParList(ID3D11Device* dev);
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_parArgs;                         // v0.10.0 phase 8: DispatchIndirect args (votes)
    // v0.10.0 phase 14 round 4 PLATE HOLD: the listed draws' results of this pass / the previous one (64 x uint4, ping-pong with
    // m_ping); valid = that pass ran the rigid-parent picks (Dispatch), cleared by Forget
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_uhold[2];
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_uholdSrv[2];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_uholdUav[2];
    bool m_uholdValid[2] = {};
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_parArgsUav;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cb;                              // dynamic cbuffer
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_tab, m_ptab, m_groups, m_lists; // dynamic structured (CPU -> GPU)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_tabSrv, m_ptabSrv, m_groupsSrv, m_listsSrv;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_mvp[2];                          // gathered MVPs, raw, ping-pong
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_mvpSrv[2];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_mvpUav[2];
    int m_ping = 0;                                                         // index of THIS pass's gather target
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_work, m_rtab, m_cnt;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_workUav, m_rtabUav, m_cntUav;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_rtabSrv;
    static constexpr int kRb = 3;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_rb[kRb];
    bool     m_rbPending[kRb] = {};
    bool     m_rbHist[kRb] = {};
    uint32_t m_rbN[kRb] = {}, m_rbElig[kRb] = {};
    uint64_t m_rbSeq[kRb] = {};
    // v0.10.0 phase 8 (mv_inst_replay 1): the instanced draws of the pass behind each readback slot (index, pointer-free key)
    static constexpr int kRbInst = 512;
    uint32_t* m_rbInstIdx = nullptr;             // kRb * kRbInst
    uint64_t* m_rbInstKey = nullptr;
    uint32_t  m_rbInstN[kRb] = {};
    uint32_t* m_prepInstIdx = nullptr;           // the prepared pass's instanced draws (kRbInst; copied at Finish)
    uint64_t* m_prepInstKey = nullptr;
    uint32_t  m_prepInstN = 0;
    // v0.10.0 phase 14 (mv_replay_static_*): the FullKey of every G-buffer draw of the pass behind each readback slot (0 = not a
    // gate candidate: instanced / no MVP / cabin), the slot's Present frame; the prepared pass's keys (copied at Finish).
    // Allocated on first use (main units only: never for a quiet mirror unit).
    uint64_t* m_rbStatKey = nullptr;             // kRb * kMax
    uint32_t  m_rbStatN[kRb] = {};
    uint64_t  m_rbFrame[kRb] = {};
    uint64_t* m_prepStatKey = nullptr;           // kMax
    uint32_t  m_prepStatN = 0;
    bool      m_statReg = false;                 // registered for PollMain (the destructor unregisters)
    uint64_t m_rbCtr = 0;
    // previous committed pass of this eye (CPU keys)
    uint32_t m_prevN = 0;
    uint64_t* m_prevFull = nullptr;
    uint64_t* m_prevLoose = nullptr;
    uint8_t*  m_prevFlags = nullptr;
    // this pass (Prepare -> Finish)
    uint32_t m_prepN = 0, m_prepElig = 0;
    uint32_t m_prepNG = 0, m_prepNF = 0;        // v0.10.0 phase 3: G-buffer / forward draws of the prepared pass
    uint32_t m_prepCandW = 0, m_prepCandC = 0;  // consensus candidates (world / cabin) uploaded for this pass
    uint32_t m_prepW = 0, m_prepH = 0;          // v0.10.0 phase 6: full image size of the prepared pass (the vote grid)
    uint32_t m_prepClip[4] = {};                // v0.10.0 phase 9: the vote grid rect (x0 aligned to 4, y0 aligned, x1, y1)
    uint32_t m_prepFold = 0;                    // v0.10.0 phase 9: FoldU.x of the prepared pass
    bool     m_prepCompact = false;             // v0.10.0 phase 18: the prepared pass runs the compact vote (FoldU.x bit 5)
    uint32_t m_prepGrid = 0;                    // v0.10.0 phase 18: vote-grid groups of the prepared pass (stats)
    uint32_t m_rbGrid[kRb] = {};                // v0.10.0 phase 18: ... of the pass behind each readback slot
    bool     m_rbCompact[kRb] = {};
    bool     m_prepIds = false;                 // v0.10.0 phase 9: Prepare was told the id target will be bound
    bool     m_prepHist = false;
    float    m_consDisMax[2] = {};
    float    m_cabSmallDevMax = 0.0f;           // v0.10.0 phase 19 (TakeCabSmallDevMax)
    float    m_ofMax[3] = {};                   // v0.10.0 phase 14 round 7 (TakeOriginDiag)
    float    m_ofMoverMinW = -1.0f;
    float    m_ofMissMin = -1.0f;
    uint32_t m_consRun[2] = {};                 // v0.10.0 phase 8: landed readbacks in a row with a used consensus (>= kConsMin)
    int      m_lastCons[2] = {};
    uint32_t m_lastConsN[2] = {};
    uint32_t* m_candTmp = nullptr;              // kMax: consensus candidate scratch (Prepare)
    uint64_t* m_curFull = nullptr;
    uint64_t* m_curLoose = nullptr;
    uint8_t*  m_curFlags = nullptr;
    // grouping scratch
    struct Ent { uint64_t key; uint32_t sideIdx; };
    Ent*      m_ent = nullptr;                  // 2 * kMax
    Ent*      m_orph = nullptr;                 // 2 * kMax
    uint32_t* m_curGroup = nullptr;             // kMax
    uint32_t* m_curRank = nullptr;
    uint32_t* m_curLooseG = nullptr;            // 1 = grouped by the loose key
    uint8_t*  m_curBig = nullptr;               // v0.10.0 phase 5 (dump): 1 = in an oversized group (never paired)
    uint32_t* m_prevGroup = nullptr;
    uint32_t* m_prevRank = nullptr;
    uint32_t* m_groupData = nullptr;            // 4 * 2 * kMax
    uint32_t* m_listData = nullptr;             // 2 * kMax
    uint32_t  m_nGroups = 0, m_nList = 0;
    int       m_lowPairLogs = 0;
    bool      m_quiet = false;
    Stats     m_stats;
    GpuSpanTimer m_pairTimer, m_passBTimer;
    // v0.10.0 phase 4 dump (RequestDump): CPU facts of the chosen draws at Prepare, GPU tables copied at Finish, logged by Poll
    // (phase 5: every draw of the pass is kept -- kMax entries --; DumpPoll chooses the lines once the states are known)
    struct DumpEnt { uint32_t i, ic, cbFirst, cbNum, mirrorOff, group, gCur, gPrev; uint64_t full, loose; uint8_t flags, big;
                     const void* ps; uint8_t psInfo; float vpMin, vpMax; uint32_t inst; };   // v0.10.0 phase 21
    void DumpPoll(ID3D11DeviceContext* ctx);
    DumpEnt*  m_dump = nullptr;                 // kMax entries, allocated by the first RequestDump
    uint32_t  m_dumpN = 0, m_dumpNG = 0, m_dumpNAll = 0, m_dumpM = 0, m_dumpW = 0, m_dumpH = 0, m_dumpSkipped = 0;
    bool      m_dumpArm = false, m_dumpPrep = false, m_dumpPending = false, m_dumpHist = false;
    int       m_dumpPolls = 0;
    char      m_dumpWhy[64] = {};
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_dumpSt[5];            // CurM, RTab, Work (partner | R_draw | attach | twin), Solve,
                                                                 // v0.10.0 phase 21: the probe records
    // v0.10.0 phase 21 pixel probe (RequestDump probeU / probeV): the centre asked for, the packed centre of the prepared pass,
    // pass B's crop, the probe buffer (raw UAV, kProbeN * kProbeStride bytes) and whether pass B wrote it this pass
    float     m_probeU = -1.0f, m_probeV = -1.0f;
    uint32_t  m_probePacked = 0, m_probeCx = 0, m_probeCy = 0, m_probeCrop[4] = {};
    bool      m_probeBound = false, m_probeStaged = false, m_probeFailed = false;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_probeBuf;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_probeUav;
    void ProbeLog(const void* const* mapped);   // DumpPoll: the 'MV probe' lines
    Microsoft::WRL::ComPtr<ID3D11Resource> m_dumpSolve;          // the solve buffer of the dumped pass (Dispatch)
};

#endif // WITH_DLAA
