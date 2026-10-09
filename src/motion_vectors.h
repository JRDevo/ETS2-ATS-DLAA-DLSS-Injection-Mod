// CameraMv -- camera-reprojection motion vectors for ETS2/ATS (v0.4.2).
// The engine has no velocity buffer and no per-frame camera cbuffer, only a
// per-draw MVP in a big dynamic VS constant-buffer ring. Method (details in
// docs/DLAA_INTEGRATION.md):
//   * While the main G-buffer pass runs, DrawIndexed is hooked; for the first N
//     draws per depth layer (world / cabin) we GPU-copy the draw's 64-byte MVP
//     (VS cb slot 0, byte offset 64..127 of the draw's window) into a candidate
//     buffer and remember a geometry key (IB, VB, offsets, counts).
//   * At the swapchain blit: keys present in BOTH this and the previous frame's
//     list are static-object candidates. R = MVP_prev * inverse(MVP_cur) maps
//     current clip space to previous clip space for everything static in that
//     layer (world matrix cancels). A compute pass computes R per pair and picks
//     the medoid per layer (rejects moving objects); a second pass reprojects
//     every pixel (layer from its depth value) into an RG16F MV texture.
//   * v0.6.0: a third matrix R_ego (medoid of the near world draws that move WITH the camera: own truck
//     mirrors / hood / wheels) is used for world-layer pixels closer than mv_ego_pixel_m.
//   MV convention (DLSS): current_pixel + mv = previous_pixel, pixels, origin
//   top-left, MVScale (1,1), not jittered.
// Compiled only when WITH_DLAA=1.
#pragma once
#ifdef WITH_DLAA

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include "draw_ids.h"     // v0.10.0 per-draw motion vectors (DrawIdRecord / DrawIdMv)

// One pass's worth of MV candidates (v0.5.4): a GPU buffer of per-draw MVPs + the CPU geometry keys. Recorded
// per G-buffer PASS (the pass FIFO slot owns one) and committed to an eye's CameraMv at that pass's blit.
class CandidateRecord {
public:
    static constexpr int  kLayers    = 2;      // 0 = world, 1 = cabin
    static constexpr int  kSlots     = 128;    // candidates per layer
    static constexpr UINT kSlotBytes = 64;     // one float4x4
    static constexpr UINT kBytes     = kLayers * kSlots * kSlotBytes;   // 16384

    struct DrawKey {
        const void* ib; const void* vb;
        UINT ibOffset, vbOffset;
        UINT indexCount, startIndex;
        INT  baseVertex;
    };

    bool Init(ID3D11Device* dev);                       // lazily creates the buffer; false on failure
    bool Ready() const { return m_buf != nullptr; }
    void Reset() { for (int l = 0; l < kLayers; ++l) m_count[l] = 0; m_drop = 0; }
    // v0.10.0 phase 8 (dlaa.ini mv_medoid_drop): bit L = layer L is deliberately NOT collected for this pass (the per-draw
    // consensus of that layer is healthy, CameraMv::MedoidDropBits): no copies, no medoid, no world miss from it. Set at the pass
    // start, after Reset; carried by CopyFrom.
    void SetDropped(uint8_t bits) { m_drop = (uint8_t)(bits & 3u); }
    uint8_t DropBits() const { return m_drop; }
    bool Dropped(int layer) const { return ((m_drop >> layer) & 1u) != 0; }
    bool LayerFull(int layer) const { return m_count[layer] >= kSlots; }
    int  Count(int layer) const { return m_count[layer]; }
    const DrawKey& Key(int layer, int i) const { return m_keys[layer][i]; }
    // GPU-copies 64 bytes at `srcByteOffset` of `src` into the next free slot of `layer`, stores `key`.
    void Record(ID3D11DeviceContext* ctx, int layer, ID3D11Buffer* src, UINT srcByteOffset, const DrawKey& key);
    // Becomes a copy of `o` (GPU CopyResource + keys + counts).
    void CopyFrom(ID3D11DeviceContext* ctx, const CandidateRecord& o);
    ID3D11Buffer* Buffer() const { return m_buf.Get(); }
    ID3D11ShaderResourceView* Srv() const { return m_srv.Get(); }
    void Shutdown() { m_srv.Reset(); m_buf.Reset(); Reset(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Buffer>             m_buf;   // raw, 2 layers x 128 slots x 64 B
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    int     m_count[kLayers] = {};
    uint8_t m_drop = 0;                                 // v0.10.0 phase 8 (SetDropped)
    DrawKey m_keys[kLayers][kSlots];
};

// ---- v0.9.0 per-object motion vectors (stencil object ids) ----------------------------------------------------------
// The camera R is wrong for everything that moves on its own (AI traffic, its wheels, the own trailer in the chase view).
// RenderDoc audit (scripts/rdc_stencil_audit.py, docs/CAPTURE_FINDINGS.md): the game writes the scene stencil in the
// G-buffer with StencilWriteMask 0x0F (REPLACE, ref 1 / 2) and reads it in the lighting / forward passes with
// StencilReadMask 0x0F. The UPPER nibble is never read or written by the game -> 15 object ids (1..15, 0 = static).
// While the world G-buffer pass runs every DepthStencilState is swapped for a twin that also writes the upper nibble
// (static draws write 0 = clear stale ids, a tagged draw writes its id), see the "per-object MVs" block in inject.cpp.
// ObjRecord = one G-buffer pass: every world-layer DrawIndexed (identity hash, key hash, MVP byte offset in the game's VS
// constant-buffer ring, index count, id). The ring is written once per frame (Map WRITE_DISCARD) and holds every draw's
// constants for the whole frame, so ONE CopySubresourceRegion of the used range at the pass end replaces a per-draw copy.
// v0.9.0 r3: up to kRings DISTINCT ring buffers per pass (round-2 log: 70-375 world draws per pass came from a second big
// dynamic cbuffer -- most likely the engine continues in another buffer when the first one is full). Each ring has its
// own used range [lo, hi) and its own Flush copy into ONE mirror buffer at a per-ring base offset (base_k + the game's
// byte offset), so a draw's table entry stays a single mirror byte offset and the GPU side (kObjShader) is unchanged:
// one dispatch, one SRV. A WRITE_DISCARD of a recorded ring (hkMap) flushes THAT ring only and SEALS its slot: the draws
// recorded so far keep their (already copied) MVPs, later draws from the same buffer open a new slot.
class ObjRecord {
public:
    static constexpr int kMax = 2048;          // world-layer DrawIndexed recorded per pass (flat capture: 566)
    static constexpr int kIds = 16;            // stencil ids 0..15 (upper nibble), 0 = static (camera / ego R)
    static constexpr int kRings = 3;           // v0.9.0 r3: distinct cbuffer rings mirrored per pass
    ObjRecord() { Reset(); }
    void Reset();                              // a new pass (releases the ring refs)
    // v0.9.0 r2: why a world draw was NOT recorded (the "missed" counter, split so a log can tell the causes apart):
    //   kMissNoCb      no VS constant buffer at slot 0, or its window is < 128 bytes (no MVP at rows 4..7) -- inject.cpp
    //   kMissSmallCb   slot 0 is a small (< 64 KB) buffer: a per-object cbuffer, not the per-draw constant ring
    //   kMissOtherRing slot 0 is another big buffer while all kRings ring slots of the pass are taken (v0.9.0 r3; r2:
    //                  any second big buffer -- only ONE ring was mirrored per pass)
    //   kMissFull      kMax draws already recorded, or the MVP window lies outside the ring
    enum Miss { kMissNone = 0, kMissNoCb, kMissSmallCb, kMissOtherRing, kMissFull, kMissKinds };
    // Render thread, per draw. kMissNone = recorded; else not recorded (the draw may still be tagged).
    // v0.9.0 r2: + occ = the identity's occurrence index (the n-th draw of this geometry key in the pass).
    // v0.9.0 r3: `cb` may be any of up to kRings ring buffers; canRep = false: the draw never represents its id (a
    // "follower" member without a trusted pairing of its own -- its R would be the identity matrix).
    // v0.9.0 r9: `geo` = the draw's raw geometry key (nullable). Kept per draw for the FALLBACK pairing (the loose key
    // indexCount / startIndex / baseVertex without the buffer pointers, used only where the full key finds no previous
    // draw and the loose key is unique on both sides) and for the unpaired-draw diagnostic.
    struct Geo { const void* ib; const void* vb; UINT ibOffset, vbOffset, startIndex; INT baseVertex; };
    Miss Add(ID3D11Buffer* cb, UINT mvpByteOff, uint64_t id, uint64_t keyHash, uint32_t occ, UINT indexCount, int slot,
             bool canRep = true, const Geo* geo = nullptr);
    static uint64_t LooseKey(UINT indexCount, UINT startIndex, INT baseVertex);   // r9: never 0
    // Pass end (G-buffer left) / a new pass: copies [lo, hi) of every ring into the mirror (base + the same byte
    // offsets). Idempotent.
    void Flush(ID3D11DeviceContext* ctx);
    // v0.9.0 r3 (hkMap): `buf` is about to be WRITE_DISCARD-mapped. If it is an open ring of this record: flush that ring
    // (the copy is queued before the discard, so it reads the old contents) and, if it has draws, seal it. true = sealed.
    bool OnDiscard(ID3D11DeviceContext* ctx, ID3D11Resource* buf);
    bool HasOpenRing(const ID3D11Resource* buf) const {
        for (int k = 0; k < m_nRings; ++k)
            if (!m_rings[k].sealed && (const ID3D11Resource*)m_rings[k].buf == buf) return true;
        return false;
    }
    bool Pending() const {
        for (int k = 0; k < m_nRings; ++k) if (m_rings[k].hi > m_rings[k].lo) return true;
        return false;
    }
    int  Count() const { return m_n; }
    bool Usable() const { return m_n > 0 && !m_flushFail && !Pending() && m_mirrorSrv; }
    bool FlushFailed() const { return m_flushFail; }           // v0.9.0 r8 diagnostics (why a record is not Usable)
    bool HasMirror() const { return (bool)m_mirrorSrv; }
    uint64_t Id(int i) const { return m_id[i]; }
    uint64_t KeyHash(int i) const { return m_kh[i]; }
    UINT Off(int i) const { return m_off[i]; }         // v0.9.0 r3: MIRROR byte offset (ring base + the game's offset)
    int  RingOf(int i) const { return m_ringOf[i]; }   // v0.9.0 r3: ring slot of draw i
    UINT Ic(int i) const { return m_ic[i]; }
    uint32_t Occ(int i) const { return m_occ[i]; }      // v0.9.0 r2
    uint64_t LooseKh(int i) const { return m_lkh[i]; }  // v0.9.0 r9: 0 = no raw key recorded (no fallback pairing)
    const Geo& GeoOf(int i) const { return m_geo[i]; }  // v0.9.0 r9 (zeroed when not recorded)
    // draw index of the biggest tagged draw allowed to represent id `slot` this pass (-1 = none). v0.9.0 r4: diagnostics
    // only -- the GPU now picks the representative per frame among ALL tagged draws (CSResolve)
    int  Rep(int slot) const { return m_rep[slot]; }
    // v0.9.0 r4: the draw's stencil id (bits 0..3, 0 = untagged) and bit 7 = it may NOT represent its id (follower /
    // spinning part). The GPU picks each id's representative per frame among its tagged draws (kObjShader CSResolve).
    uint8_t Tag(int i) const { return m_tag[i]; }
    uint32_t Tagged() const { return m_tagged; }
    uint32_t Missed() const { return m_missed; }       // world draws not recorded (all reasons)
    ID3D11ShaderResourceView* MirrorSrv() const { return m_mirrorSrv.Get(); }
    void NoteTagged() { ++m_tagged; }
    void NoteMissed() { ++m_missed; }
    void Shutdown();
    // v0.9.0 r3 diagnostics (stats; the first multi-ring passes are logged by inject.cpp)
    struct RingInfo {
        const void* buf;                       // identity
        UINT bytes;                            // ByteWidth
        UINT base;                             // mirror base offset
        UINT usedLo, usedHi;                   // byte range used by the recorded draws this pass
        UINT copied;                           // bytes copied into the mirror this pass
        int  draws;                            // draws recorded from it
        int  firstDraw;                        // record index of its first draw
        bool sealed;                           // DISCARD-mapped while it had draws (later draws use a new slot)
    };
    int  Rings() const { return m_nRings; }
    RingInfo GetRing(int k) const;
    int  Seals() const { return m_seals; }
    int  Switches() const { return m_switches; }       // ring changes between consecutive recorded draws
                                                       // (1 = "moved on to the next ring", many = used side by side)

private:
    struct Ring {
        ID3D11Buffer* buf;                     // ref held between the slot's first Add and Reset
        UINT bytes, base;
        UINT lo, hi;                           // byte range still to copy (empty when hi <= lo)
        UINT usedLo, usedHi, copied;
        int  draws, firstDraw;
        bool sealed;
    };
    bool FlushRing(ID3D11DeviceContext* ctx, int k);
    bool EnsureMirror(ID3D11DeviceContext* ctx, UINT need);
    Ring     m_rings[kRings] = {};
    int      m_nRings = 0;
    int      m_last = -1;                      // ring slot of the previous Add (the common case: the same buffer)
    UINT     m_need = 0;                       // mirror bytes the open slots need (aligned bases)
    bool     m_copiedAny = false;              // a FlushRing already wrote into the mirror this pass (growth keeps it)
    int      m_seals = 0;
    int      m_switches = 0;
    int      m_n = 0;
    bool     m_flushFail = false;
    uint32_t m_tagged = 0, m_missed = 0;
    int      m_rep[kIds] = {};
    UINT     m_repIc[kIds] = {};
    uint64_t m_id[kMax];
    uint64_t m_kh[kMax];
    UINT     m_off[kMax];
    UINT     m_ic[kMax];
    uint16_t m_occ[kMax];                      // v0.9.0 r2: occurrence index of the identity (saturates at 0xFFFF)
    uint8_t  m_ringOf[kMax];                   // v0.9.0 r3
    uint8_t  m_tag[kMax];                      // v0.9.0 r4: stencil id | 0x80 = may not represent (see Tag)
    uint64_t m_lkh[kMax];                      // v0.9.0 r9: loose key hash (0 = none)
    Geo      m_geo[kMax];                      // v0.9.0 r9: raw geometry key
    const void* m_otherBuf = nullptr;          // v0.9.0 r2: last non-ring cbuffer seen (identity) and its class
    bool     m_otherBig = false;
    Microsoft::WRL::ComPtr<ID3D11Buffer>             m_mirror;     // raw: ring k's bytes at [base_k, base_k + bytes_k)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_mirrorSrv;
    UINT m_mirrorBytes = 0;
};

// Global identity -> stencil id table (render thread). Fed from eye 0's async "moving" readback (2-3 frames old), read at
// every tagged G-buffer draw. Ids are handed to RIGID-MOTION GROUPS: a moving identity joins an id whose R is (nearly)
// equal, or whose motion moves the identity's origin to within 1 px of its own (a wheel joins its vehicle body); else it
// gets a free id. Hysteresis: 2 moving readbacks to get an id, 8 readbacks without being seen moving to lose it.
// v0.9.0 r2 (first in-game test: grass clumps, window grids, tree rows and own-truck parts were tagged as movers):
//  * the GPU verdict only calls a draw "moving" when its pairing is trustworthy -- geometry key drawn at most
//    mv_objects_max_dup times (pass and previous pass), the nearest previous draw IS the identity's own previous
//    occurrence (same key + occurrence index), the nearest candidate is clearly nearer than the second, and the origin is
//    at least mv_objects_ego_m from the camera (own truck). Draws that would have been "moving" but fail one of these come
//    back in a separate REJECT list (reason codes below).
//  * TEMPORAL COHERENCE: an identity needs mv_objects_hits CONSECUTIVE moving readbacks whose per-frame displacement
//    (origin vs the camera-only prediction, ~m/frame) agrees with the previous one (roughly constant velocity: a vehicle
//    moves smoothly, a mis-paired clump jumps). A member whose motion jumps twice in a row, or that is rejected in two
//    readbacks in a row, loses its id at once.
//  * Ctrl+F6 debug view: id 15 is reserved for "rejected in the last readbacks" identities (cyan in the view, camera R in
//    pass B); the real ids are 1..14 while the view is on.
// v0.9.0 r3 FOLLOWERS (wheels of several vehicles of one model, round 2: rejected as duplicates): the duplicate / pairing /
// ambiguity rules block NEW ids, not membership. A rejected draw whose hub the motion of an existing id moves to within
// 1 px of one of its previous candidates (the nearest one, or its own previous occurrence) -- the same loose test that
// lets a wheel join its vehicle -- in mv_objects_hits consecutive readbacks joins that id as a FOLLOWER: it is tagged
// with the id (gets the vehicle's R in pass B), never anchors / represents it (no trusted R of its own), stays while it
// keeps following (a fresh anchor R that it no longer follows counts as a bad readback: 2 in a row drop it), and goes
// with the id when no trusted member is left. A trusted member that later becomes a duplicate stays the same way.
// v0.9.0 r4 (round-3 in-game log: ids exhausted -- over-budget +261..+7683 per 600 blits, 32572 in total, r2: 0 -- distant
// traffic ghosting / wobbling; the per-id dump shows pairs / triples of ids with the SAME IndexCount, dup 2-3, the same depth
// within 0.4-2.6 m and the same speed = the wheels of ONE vehicle, each anchoring its own id):
//  * SPINNING parts (wheels): the GPU measures how much the draw's local axes turned in the frame beyond the camera's
//    motion (Moving::spin, ~radians per frame). A vehicle yaws < 0.03 rad / frame, a wheel above ~10 km/h turns > 0.1. A
//    spinning identity never takes a free id, never anchors or re-anchors one and never represents its id (its R carries
//    the spin: applied to the body it made the body's motion vectors rotate); it can only JOIN an id whose fresh anchor
//    motion moves its hub (origin) to within 1 px of where it really was (the loose join). Unjoined it waits (no id).
//  * QUARANTINE: an id that was freed is not handed out again for kQuarantine frames (defensive: the stencil and the R of
//    an id always come from the same pass, see docs, but a reused id can never meet a stale tag this way either).
//  * FOLLOWERS (r3) are off by default (dlaa.ini mv_objects_followers, 0); when on, only an id whose anchor refreshed
//    coherently in kSolidRb consecutive readbacks ("solid") takes followers.
//  * MEMBERSHIP RE-CHECK: a member (joined because it moved alike) that no longer moves like its id -- the fresh anchor R
//    puts its origin more than 1.5 px from where it really was -- in 2 readbacks in a row leaves the id.
//  * mv_objects_max_dup default 3 again (r3: 6).
//  The per-frame GPU side (CameraMv) adds: the id's representative is the biggest TAGGED draw with a trusted, non-spinning
//  pairing THIS frame (was: the CPU's biggest tagged draw, untrusted or not), and an id is VETOED for the frame (camera R)
//  when one of its tagged draws is not where the id's motion says it was (identity swaps between identical vehicles).
// v0.9.0 r6 STICKY TAGS (round-4 in-game log: distant cars lose and regain their tag -- "wobbly, smudgy cars where the pink
// stops"; every loss / gain flips the car's pixels between the object R and the camera R and DLSS's history no longer
// matches). Before r6 a member stayed only while it was seen MOVING (> 0.5 px vs the camera R): a car far away driving
// towards / away from the camera, or slowing down, dropped under 0.5 px, was "unseen", released after 8 readbacks and
// re-acquired later. Now:
//  * SEEN = drawn and paired in the readback's pass (Update gets the pass's recorded identities), moving or not. A member
//    is released only when it was not drawn for kMissToDrop readbacks, or when it is PROVEN to belong elsewhere: a gross
//    misfit (> max(4 px, 50 % of its own correction) -- the GPU veto's threshold) in 2 judged readbacks in a row, AND a
//    clean fit to another id (it moves there directly, no camera-R gap) or, for a non-spinning member with its own trusted
//    motion, a free id of its own. Rejected sightings (dup / pairing / ambiguous / own truck) only count as seen; they are
//    judged with the hub test against the id's recent R (identity swaps of identical vehicles move to the other id).
//  * An id lives while it has members (spinning ones included, for a re-anchoring body after a LOD switch); its motion
//    reference is refreshed from the anchor or, when the anchor is not moving this readback, the biggest member that is.
//  * Coherence ("jumps") tolerance: max(0.15 per frame of gap + 0.35 |d|, 2 x the pixel footprint at the origin's depth);
//    only a member with 2 REAL jumps in a row is dropped.
//  * Hysteresis: 2 coherent readbacks, one unseen readback in between allowed (r2 behaviour) when the sightings are at most
//    2 frames apart (a copy culled every 3rd frame cannot collect hits).
//  * Spinning parts join with the latest reference R up to 2 readbacks old, near ANY member of the id (trailer wheels).
// v0.9.0 r7 STICKY OBJECT MVs (round-5 in-game log + video, driving: 4..27 tag losses per second, a passing truck's
// parts cyan / magenta / orange frame by frame):
//  * OWN TRUCK = near (origin < mv_objects_ego_m) AND camera-locked (its probe points -- a spinning draw: its origin --
//    move <= mv_objects_lock_px on screen in the frame; GPU). A vehicle passing alongside is near but not locked: a normal
//    mover now (r2..r6: a part that was not a member yet never got an id within 8 m). kRejEgo keeps its meaning (a near +
//    locked draw: seen, no verdict). An origin behind the camera (no screen position) gets no membership verdict.
//  * HOLD (GPU, CameraMv): an id without a usable R of its own this frame keeps its last R for up to mv_objects_hold
//    frames.
//  * FAST JOIN: a mover on its first sighting whose R equals an id's (kTightFrob) next to one of its members -- or a
//    spinning part whose hub that id's fresh reference R moves within 1 px -- joins at once (r6: after mv_objects_hits
//    readbacks; a part that lost its tag or a new LOD of a member was untagged for 2-3 readbacks). Taking a NEW id still
//    needs the hits.
// v0.9.0 r9 (round-7 in-game log + video: road, verges and trees magenta = tagged with a vehicle's id; ego set empty):
//  * NO NON-SPINNING FAST JOIN (r7 reverted): a static draw that is a candidate for ONE frame (camera-R float noise in
//    a turn) has R = the camera R; once any static draw held an id its R matched every other such draw within
//    kTightFrob, so the static world joined it on first sightings (one id: 282 members at 659 m). A non-spinning mover
//    joins or takes an id only after mv_objects_hits coherent readbacks again (r6). The spinning-part fast join stays.
//  * EGO SET BY GEOMETRY KEY: one entry per PHYSICAL COPY of a key (a sighting updates the nearest entry of its key),
//    so symmetric copies whose occurrence order flips (left / right mirror) keep their boxes; the identities that
//    matched a flagged copy are the ego identities (Table.z bit 15). The position is measured at the entry's reference
//    depth (clip x / y and the view depth, px-equivalent), not as the origin's projection: an origin far off screen at
//    ~1 m (the truck root under / beside the interior camera) moved by tens of px per mm of head motion and restarted
//    its box on every sighting (round 7: best unflagged run 1 of 24, 0 flagged of 34).
// v0.9.0 r10 (round-8 in-game log + video: the road, a verge and its trees magenta for 13 s -- static draws joined a
// vehicle id by R similarity and were never proven out; licence plates untagged; ego set still 0 flagged of 112):
//  * ORIGIN GATE: a candidate is a mover / member only while its ORIGIN has a screen position inside the viewport plus
//    mv_objects_origin_margin (clip w > 0.05, |x / w|, |y / w| <= margin; GPU reason 8 kRejOffscreen). A member whose
//    latest sighting is off screen is released (the GPU reports every TAGGED draw with an off-screen origin, candidate
//    or not). Road / terrain / vegetation batches have their origin anywhere; a vehicle's is on the vehicle.
//  * MIN OWN MOTION: a non-spinning mover founds or joins an id only from sightings with dev >= mv_objects_min_px; a
//    sighting below it counts as drawn sub-threshold (r6 hysteresis), members keep their id as in r6.
//  * ATTACH: a dup / pairing / ambiguous reject whose origin coincides (<= 0.5 px, |w| within 1 %) with a non-spinning
//    member's origin of the same readback in mv_objects_hits readbacks in a row joins that member's id as an ATTACHED
//    member (licence plates, mirrors, lamps: the same world matrix); never anchors / represents; released when its
//    anchor member leaves or the origins no longer coincide in 2 readbacks (mv_objects_attach).
//  * EGO SET IN VIEW SPACE: positions are the origin's view-space position in metres (CSMain recovers it from the MVP:
//    focal terms and skew from rows 0 / 1 / 3), the box mv_objects_ego_m_box per axis -- a camera-locked part keeps a
//    constant view-space origin within the cabin sway, a passing vehicle's moves by its relative speed per frame.
// v0.9.0 r11 (round-9 in-game log + video: the road / trees seen through the windscreen cabin-classified -- see the
// forward depth --, the near set flooded by draws whose "MVP" has no object transform, the static world still founding
// / joining ids):
//  * CAMERA-ATTACHED (GPU CSMain, reason 9 kRejCamOrigin): a draw whose view-space origin (ViewOrigin) lies within
//    mv_objects_cam_m (default 0.3 m) of the camera -- or whose clip origin has |w|, |x|, |y| all below it -- has no
//    object transform in rows 4..7 (a batched / instanced draw whose per-instance transform lives elsewhere, an overlay
//    with the view-projection only: its origin IS the camera). Never a candidate, mover, representative, ego sighting
//    or attach anchor; a TAGGED one is reported (bit 9, drawnOnly) so its membership is released; every such draw is
//    counted in the Moving tail (cam-origin per readback). DrawIndexedInstanced is not hooked: never recorded.
//  * STATIC CLASS (CPU, Update): per identity the readbacks of the last 48 in which it was drawn (the `drawn` list:
//    paired in the read-back pass) and the STATIC ones among them (drawn but no candidate, or a camera-noise reject).
//    >= mv_objects_static_rb (default 24) sightings with a static share >= 50 % = static class: it neither founds nor
//    joins an id (new mover, follower, attach), a member that becomes static class is released (static-released).
//    Moving entries and dup / pairing / ambiguous / own-truck / off-screen / veto rejects are sightings of non-static
//    or unknown motion. A stopped vehicle is static class (the camera R is right for it) and leaves the class after
//    ~24 readbacks of motion; a vehicle moving relative to the world is a candidate in every readback.
//  * mv_objects_min_px default 2.0 (r10: 1.0): a far vehicle below 2 px / frame of own motion stays on the camera R.
namespace ObjIds {
struct Moving {                 // one readback entry (CameraMv)
    uint64_t id;
    uint64_t kh;                // v0.9.0 r9: its geometry key hash (ego set by key)
    float    dev, w, ic;        // deviation from the camera R (px), origin view depth (m), index count
    float    c0[4];             // the identity's origin in current clip space (MVP column 3)
    float    R[16];             // its own R = MVP_prev * inverse(MVP_cur), row-major
    float    d[3];              // v0.9.0 r2: origin displacement per frame vs the camera-only prediction (prev clip x, y, w ~ m)
    int      dup;               // v0.9.0 r2: draws of its geometry key in the pass (max of this / previous pass)
    float    spin;              // v0.9.0 r4: rotation of its local axes in the frame beyond the camera's (~rad / frame)
    float    v[3];              // v0.9.0 r10: the origin's view-space position (m; NaN = not recoverable from the MVP)
};
// v0.9.0 r6: kRejVeto = a VETO record (CSVeto): this tagged draw contradicted its id's motion grossly this frame (dev = the
// contradiction in px, a[0] = the nearest previous origin of its key under the id's R). Lets the CPU take a member out that
// stopped (< 0.5 px, never judged by its own motion) while its id moves on -- else the veto would flip the whole id to the
// camera R every frame.
// v0.9.0 r9: kRejNoise = CAMERA NOISE (CSMain): a trusted candidate whose deviation from the camera R is not explained
// by any motion relative to the world (the column-2 / camera-origin test): the camera R is off by a rotation, the draw
// is static. Seen (ego set, sticky membership), never a mover, not marked in the debug view.
// v0.9.0 r10: kRejOffscreen = the ORIGIN has no screen position inside the viewport + mv_objects_origin_margin
// (CSMain): never a mover; a member is released. Also reported for a TAGGED draw that is no candidate
// (Reject::drawnOnly).
// v0.9.0 r11: kRejCamOrigin = CAMERA-ATTACHED (CSMain): the origin lies within mv_objects_cam_m of the camera (no
// object transform in the MVP rows): never a candidate; only reported for a TAGGED draw (drawnOnly) so its membership
// goes.
enum RejectWhy { kRejNone = 0, kRejDup = 1, kRejPair = 2, kRejAmbig = 3, kRejEgo = 4, kRejIncoh = 5, kRejVeto = 6,
                 kRejNoise = 7, kRejOffscreen = 8, kRejCamOrigin = 9, kRejKinds = 10 };
struct Reject {                 // v0.9.0 r2: a draw that would have been "moving" under the v0.9.0 rules, filtered by r2
    uint64_t id;
    uint64_t kh;                // v0.9.0 r9: its geometry key hash (ego set by key)
    int      why;               // RejectWhy (GPU: dup / pair / ambig / ego)
    float    dev, w;
    int      dup;
    // v0.9.0 r3 (follower test): the origin in current clip space and up to two previous-pass origins (prev clip) of
    // same-key draws: a[0] = the candidate nearest to the camera-only prediction, a[1] = the identity's own previous
    // occurrence (valid when ownOk)
    float    c0[4];
    float    a[2][4];
    bool     ownOk;
    float    v[3];              // v0.9.0 r10: the origin's view-space position (m; NaN = not recoverable)
    bool     drawnOnly;         // v0.9.0 r10: reason 8 (r11: or 9) of a tagged draw that was no candidate (not counted)
};
void SetMax(int n);             // 1..15 (dlaa.ini mv_objects_max)
int  Max();                     // ids usable now (v0.9.0 r2: <= 14 while the debug view reserves id 15)
void SetMaxDup(int n);          // v0.9.0 r2: 1..8 (dlaa.ini mv_objects_max_dup; default 3 -- r3: 6, back to 3 in r4)
int  MaxDup();
void SetFollowers(bool on);     // v0.9.0 r4: dlaa.ini mv_objects_followers (default 0 = the r3 follower rule is off)
bool Followers();
constexpr float kSpinMax = 0.12f;   // v0.9.0 r4: Moving::spin above this = a spinning part (never anchors / represents)
void SetEgoM(float m);          // v0.9.0 r2: 0..30 m (dlaa.ini mv_objects_ego_m, default 8)
float EgoM();
void SetHits(int n);            // v0.9.0 r2: 2..8 coherent readbacks before an id (dlaa.ini mv_objects_hits, default 2)
int  Hits();
// v0.9.0 r7 (GPU-side rules, kept here with the other mv_objects_* values): a near draw is the own truck only when its
// probe points moved <= LockPx on screen in the frame (dlaa.ini mv_objects_lock_px, 0.5..20, default 3); an id without
// a usable R of its own keeps its last R for up to Hold() frames in a row (dlaa.ini mv_objects_hold, 0..8, default 3;
// 0 = r6)
// v0.9.0 r8: LockPx is a pre-filter at the ORIGIN only (default 6); the own truck is the EGO SET: a near identity
// (origin < mv_objects_ego_m, on screen) whose origin stayed within EgoPx (dlaa.ini mv_objects_ego_px, 2..64, default
// 12 px of the full render) of its reference in EgoRb readbacks (mv_objects_ego_rb, 4..200, default 24) -- fed from the
// moving AND reject entries of every readback (ObjIds::Update), flag dropped when the origin drifts > EgoPx away.
// IsEgo: the identity is flagged (ObjPrepare sets Table.z bit 15 for it: never a mover / representative on the GPU).
void  SetLockPx(float px);
float LockPx();
void  SetHold(int n);
int   Hold();
void  SetEgoPx(float px);
float EgoPx();
void  SetEgoRb(int n);
int   EgoRb();
bool  IsEgo(uint64_t id);
// v0.9.0 r9 dlaa.ini mv_objects_ego_trace (default 0): 1 = log up to 4 near sightings per readback (key, identity,
// entry, position, box, flag) for the first 150 readbacks that have one ("MV objects: ego trace" lines)
// v0.9.0 r10: default 1 (the round-8 log still had 0 flagged copies; the trace costs 150 lines once)
void  SetEgoTrace(int on);
int   EgoTrace();
// v0.9.0 r10 dlaa.ini keys (see the r10 block above): mv_objects_origin_margin (1.0..3.0, default 1.2 = 20 % beyond the
// viewport edge; the GPU gate, ObjWriteCb Ext.x), mv_objects_min_px (0.5..4, default 1.0), mv_objects_attach (0/1,
// default 1), mv_objects_ego_m_box (0.01..0.5 m, default 0.05: the ego box per axis in view space; mv_objects_ego_px is
// no longer used)
void  SetOriginMargin(float m);
float OriginMargin();
void  SetMinPx(float px);
float MinPx();
void  SetAttach(bool on);
bool  Attach();
void  SetEgoMBox(float m);
float EgoMBox();
// v0.9.0 r11 dlaa.ini keys (see the r11 block above): mv_objects_cam_m (0.05..2 m, default 0.3: the GPU camera-attached
// test, ObjWriteCb Ext.y), mv_objects_static_rb (8..48, default 24: sightings within the last 48 readbacks before an
// identity can be static class; 0 = off). mv_objects_min_px default 2.0.
void  SetCamM(float m);
float CamM();
void  SetStaticRb(int n);
int   StaticRb();
bool  IsStaticClass(uint64_t id);           // r11: the identity is static class as of the last Update
constexpr int kStaticWin = 48;              // r11: readbacks of static-class history
// v0.9.0 r11: the origin of an MVP (16 floats, the cbuffer rows as stored) in VIEW SPACE (m) -- the CPU copy of
// CSMain's ViewOrigin; false = not recoverable. CamAttached: that origin within `m` metres of the camera (view-space
// length; or clip |w|, |x|, |y| all below m) = the r11 camera-attached test of CSMain (objects, mv_objects_cam_m) and
// of inject.cpp's forward re-draw (near-camera, mv_fwd_min_m). *dist = the view-space length (|w| when not
// recoverable).
bool  MvpViewOrigin(const float* mvp, float v[3]);
bool  CamAttached(const float* mvp, float m, float* dist = nullptr);
void SetDebug(bool on);        // v0.9.0 r2: Ctrl+F6 view on -> id 15 marks rejected identities (render thread, per frame)
int  DebugSlot();               // 15 while SetDebug(true), else 0
int  Lookup(uint64_t id);       // 0 = no id (static); DebugSlot() = recently rejected (debug view only)
int  Lookup(uint64_t id, bool* follower);   // v0.9.0 r3: + whether the member is a follower (never represents its id)
// v0.9.0 r6: `id` is the anchor of stencil id `slot` (the GPU prefers it as the id's representative: CPU reference and GPU
// R come from the same draw while it is drawn with a trusted pairing)
bool IsAnchor(uint64_t id, int slot);
// `frame` = the CameraMv frame number the readback belongs to (coherence needs the frame gap).
// v0.9.0 r4: `nowFrame` = the feeding unit's current frame (id quarantine; the readback is 2-3 frames older).
// v0.9.0 r6: `drawn` / `nDrawn` = every identity recorded in the readback's pass that had previous draws of its key (paired,
// trusted or not, moving or not): a member drawn there is SEEN (sticky membership). nullptr / 0 = only the lists count.
void Update(const Moving* e, int n, const Reject* r, int nr, uint64_t frame, float fullW, float fullH, uint64_t nowFrame,
            const uint64_t* drawn = nullptr, int nDrawn = 0);
void Clear();                   // feature off / history invalid
struct Stats {
    int idsInUse = 0, members = 0, waiting = 0;
    uint64_t updates = 0, overBudget = 0, joined = 0, released = 0;
    uint64_t incoherent = 0;    // v0.9.0 r2: moving sightings whose displacement jumped (wait-list restarts + member jumps)
    uint64_t dropped = 0;       // v0.9.0 r2: members dropped at once (2 jumps / 2 rejects in a row)
    int followers = 0;          // v0.9.0 r3: members that are followers now
    uint64_t followJoined = 0;  // v0.9.0 r3: rejected draws that joined an id as followers
    uint64_t followKept = 0;    // v0.9.0 r3: rejected member sightings kept because the hub follows its id
    // v0.9.0 r4
    int spinners = 0;           // members that are spinning parts now (never represent their id)
    uint64_t spinJoined = 0;    // spinning identities that joined an id (hub test)
    uint64_t spinWait = 0;      // spinning sightings that passed the hysteresis but found no id to join (they never take one)
    uint64_t quarantined = 0;   // new-id requests that found only quarantined free ids (counted like over-budget)
    uint64_t solidBlocked = 0;  // follower candidates refused because the id was not solid yet (followers on)
    uint64_t diverged = 0;      // member sightings whose motion no longer matched their id (2 in a row: dropped)
                                // v0.9.0 r6: only GROSS misfits (> max(4 px, 50 % of its correction)) count
    // v0.9.0 r6 (sticky tags)
    uint64_t moved = 0;         // members proven to belong to another id: moved there directly (tag A -> B, no camera R)
    uint64_t ownId = 0;         // members proven to move on their own: took a free id directly (tag A -> B)
    uint64_t leftToCam = 0;     // members proven out with no id to go to (tag -> camera R; a switch)
    uint64_t seenStill = 0;     // member sightings that were drawn but in no list (sub-0.5 px: kept, pre-r6 = a miss)
    uint64_t vetoOut = 0;       // members vetoed while not moving in kProveRb readbacks: out to the camera R (a stopped
                                // car that had joined a moving one; included in leftToCam)
    uint64_t freed = 0;         // ids freed (no member left)
    uint64_t freedShort = 0;    // ... of them held fewer than kShortRb readbacks
    uint64_t freedAgeSum = 0;   // sum of the freed ids' ages (readbacks)
    int ageMin = 0, ageMed = 0, ageMax = 0;   // ages (readbacks held) of the ids in use now
    uint64_t fastJoined = 0;    // v0.9.0 r7: movers that joined an id on their first sighting (included in joined);
                                // r9: spinning parts only (the non-spinning fast join is gone)
    // v0.9.0 r8 ego set (own truck by screen-position persistence)
    int egoSet = 0;             // identities flagged now (gauge)
    int egoTracked = 0;         // near identities in the table now (flagged or not)
    float egoMaxDev = 0.0f;     // largest deviation (px) of a flagged identity from its reference since the last LogIds
    uint64_t egoFlagged = 0;    // identities flagged (persistence reached)
    uint64_t egoDropped = 0;    // flags dropped (origin drifted > mv_objects_ego_px, left the near range or the screen)
    uint64_t egoReleased = 0;   // members released because their identity was flagged (included in released)
    // v0.9.0 r9: the r8 fast-joined-then-released diagnostic is gone with the non-spinning fast join
    int egoIds = 0;             // v0.9.0 r9: identities that matched a flagged copy (Table.z bit 15), gauge
    // v0.9.0 r10 (r10: egoMaxDev is in METRES of view space now)
    uint64_t offscreenReleased = 0;   // members released because their latest sighting had no on-screen origin
    uint64_t subMinPx = 0;      // non-spinning new-mover sightings with dev < mv_objects_min_px (seen, not counted)
    uint64_t attached = 0;      // rejected draws that joined a member's id as ATTACHED members (plates, mirrors)
    int attachedMembers = 0;    // attached members now (gauge)
    // v0.9.0 r11
    int staticClass = 0;        // identities in the static class now (gauge)
    uint64_t staticBlocked = 0; // new-mover / follower / attach sightings refused because the identity is static class
    uint64_t staticReleased = 0;   // members released because their identity became static class (included in released)
    uint64_t camOriginReleased = 0;   // members released for a camera-attached tagged draw (reason 9; in released)
};
constexpr int kShortRb = 72;    // v0.9.0 r6: "short-lived" id = freed within ~1 s of readbacks
Stats GetStats();
void LogIds(uint64_t blit);     // v0.9.0 r2: "MV objects ids" dump (<= 8 ids), restarts the per-id window min / max
}

class CameraMv {
public:
    static constexpr int kLayers = CandidateRecord::kLayers;      // 0 = world, 1 = cabin
    static constexpr int kSlots  = CandidateRecord::kSlots;       // candidates per layer per frame
    static constexpr UINT kSlotBytes = CandidateRecord::kSlotBytes;
    static constexpr int kPairs  = 16;     // max matched pairs per layer (buffer stride)
    static constexpr int kWorldPairs = 16; // world pair budget (v0.4.1: was 8)
    static constexpr int kCabinPairs = 8;  // cabin pair budget (unchanged)
    // v0.6.0 solve buffer (structured float4): [0..3] R_world, [4..7] R_cabin, [8],[9] per-layer info,
    // [10],[11] chosen-pair info, [12..15] R_ego, [16] InvRow3 (row 3 of inverse(MVP_cur) of the chosen world
    // pair), [17] ego info (candidates, used after the static reject, fallback to R_world, |R_ego - R_world|).
    static constexpr int kSolveElems  = 18;
    static constexpr int kSolveFloats = kSolveElems * 4;   // 72
    using DrawKey = CandidateRecord::DrawKey;

    struct FrameStats {                    // result of the last Generate()
        int  pairs[kLayers] = {0, 0};
        bool worldMiss = true;             // world layer had no usable pair
        bool reused = false;               // v0.6.1: no matching / pass A; pass B ran with the last good R
        // v0.9.0 r9: a world miss answered with the last good camera R (pass A skipped, the solve buffer kept) within
        // the mv_objects_hold budget -- NOT a camera cut: the caller must not reset the DLSS history for it
        bool missHeld = false;
    };
    // v0.9.0 r9 FALLBACK PAIRING (default on; off only for the synthetic test): a draw whose full geometry key (buffer
    // pointers, offsets, counts) finds no draw in the previous record pairs by its loose key (indexCount, startIndex,
    // baseVertex) when that loose key is drawn exactly once in both records. Pass A (camera candidates) and the object
    // record both use it.
    static void SetPairFallback(bool on) { s_pairFallback = on; }
    static bool PairFallback() { return s_pairFallback; }
    // v0.9.0 r11 (synthetic test only, default 0.5): the object path's "moving" candidate threshold (px, ObjCB
    // Params.x). The WARP harness renders at ~1/5.8 of the game's focal; this lets it put the static-class / candidate
    // boundary at the game's 0.5 px. The injector never calls it.
    static void SetMovingPx(float px) { s_movingPx = !(px > 0.0f) ? 0.5f : px; }
    static float MovingPx() { return s_movingPx; }

    // Creates shaders + buffers (device only; independent of the render size).
    // Safe to call repeatedly; returns false (and stays unusable) on failure.
    // v0.7.8: the shader bytecode comes from ShaderCache; while its warm-up is still running Init returns false
    // WITHOUT marking itself failed (the next call retries).
    bool Init(ID3D11Device* dev);
    bool Ready() const { return m_ready; }
    // v0.7.8: registers kSolveShader / kReprojDepthShader with ShaderCache (setup thread, before ShaderCache::Start).
    static void RegisterShaders();

    // Blit side --------------------------------------------------------------
    // v0.5.4: `cur` = the candidate record of the pass being blitted; the eye's previous COMMITTED record
    // (m_prev) is "prev". Matches them, runs both compute passes into `mvUav` (RG16F, w x h), then commits
    // `cur` as this eye's new prev. Caller saved/restores CS state. Returns false if the passes could not
    // run (mvUav is then left untouched).
    // v0.5.7: pass B is merged with the depth convert: it reads the depth TWIN (`depthTwinSrv`,
    // R32_FLOAT_X8X24_TYPELESS view, w x h) and writes the flattened depth into `depthR32Uav` (R32F, w x h)
    // as well as the MVs. true <=> both were written; on false NEITHER was touched (all failure returns come
    // before any dispatch) and the caller must run its own depth convert.
    // v0.6.1 `candBad` (the caller classified `cur` as an incomplete record): no matching, no pass A; pass B
    // reprojects with the solve buffer as it is (R of the last good frame), `cur` is NOT committed and prev is
    // marked stale. The first good record after that (prev is then >= 2 frames old) also reuses R once, but
    // is committed. Both: stats->worldMiss = false, stats->reused = true. Only while the solve buffer holds a
    // good solve (pass A with world pairs since the last Invalidate); otherwise the normal path runs.
    // v0.6.4 DLAA area: (w, h) = the crop (dispatch size, = the MV / R32F textures), (x0, y0) = its origin in
    // the full image, (fullW, fullH) = the full image (= the depth twin). Pass B reads the twin at origin + id
    // and computes uv / ndc / the MV pixel scale from the FULL size, so the MVs are unchanged pixel deltas.
    // Whole image: x0 = y0 = 0, fullW = w, fullH = h (identical math to v0.6.3).
    // v0.9.0 per-object MVs: `objs` = this pass's object record (nullptr = off: previews, feature off), `stencilSrv` =
    // the depth twin's X32_TYPELESS_G8X24_UINT view (stencil ids in the upper nibble). Both or neither.
    // v0.9.0 r5 `fwdDepthSrv` (nullable): the pass's FORWARD DEPTH (R32_FLOAT, full size like the twin, reversed-Z, 0 where
    // nothing was drawn): the depth of the forward pass's alpha-blended draws that write no scene depth (wires, cables,
    // fences, lane paint), re-drawn by inject.cpp into their own depth target. Pass B uses max(twin, forward) per pixel
    // (the nearer surface, reversed-Z), so a wire in front of the sky reprojects with its own parallax instead of the
    // sky's rotation-only motion; NGX gets the same merged depth. Null = the twin alone (exactly as before r5).
    // v0.9.0 r11: only a forward depth inside the WORLD range [0.01, 0.9) may win (a world-layer overlay at the camera
    // -- windscreen drops / glare -- wrote >= 0.9 and made everything behind it "cabin"); the near-range pixels are
    // counted.
    // v0.10.0 per-draw MVs (mv_objects = 2): `did` = the pass's draw record (nullptr = off), `didIdSrv` = the pass's draw-id
    // target (R16_UINT, full size like the twin; null = the replay did not complete: the record still becomes this eye's
    // history, pass B uses the camera path). After pass A the record is paired with this eye's previous one on the GPU
    // (DrawIdMv) and pass B reprojects a pixel whose draw is a MOVER with that draw's own R. Never used with `objs`.
    bool Generate(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, uint32_t x0, uint32_t y0,
                  uint32_t fullW, uint32_t fullH, const CandidateRecord& cur,
                  ID3D11ShaderResourceView* depthTwinSrv, ID3D11UnorderedAccessView* depthR32Uav,
                  ID3D11UnorderedAccessView* mvUav, FrameStats* stats, bool candBad = false,
                  const ObjRecord* objs = nullptr, ID3D11ShaderResourceView* stencilSrv = nullptr,
                  ID3D11ShaderResourceView* fwdDepthSrv = nullptr,
                  const DrawIdRecord* did = nullptr, ID3D11ShaderResourceView* didIdSrv = nullptr,
                  const DrawIdRecord* didFwd = nullptr, uint32_t fwdShift = 0, ID3D11ShaderResourceView* fwdIdSrv = nullptr);
    // v0.10.0 phase 9 `fwdShift` (0 / 1): the forward depth (fwdDepthSrv) is at 1/2^fwdShift of the image (dlaa.ini
    // mv_fwd_depth_res 2) and is read at pixel >> fwdShift; with fwdShift 1 the forward ids are not in the GREEN channel of the
    // id target but in `fwdIdSrv` (R16_UINT, the forward depth's size). 0 / null = phase 8 exactly.
    // v0.10.0 phase 3 `didFwd` (nullable): the pass's FORWARD draw record (forward mode), replayed into the GREEN channel of
    // the same draw-id target (R16G16_UINT) against the forward depth; its draws get the ids after the G-buffer draws and
    // are paired with the eye's previous forward draws. Pass B uses a forward id where the forward depth won. The camera R
    // of each layer becomes the CONSENSUS of the per-draw R's when a cluster is found (DrawIdMv, mv_camr_consensus).
    // v0.10.0: the last Generate's draw-id inputs for the MV debug view (0 / null = per-draw MVs were not used) and the
    // eye's pairing unit (statistics, GPU timers).
    uint32_t DidN() const { return m_didN; }
    uint32_t DidNG() const { return m_didN ? m_didNG : 0; }   // v0.10.0 phase 3: G-buffer ids 1..nG, forward ids nG+1..N
    ID3D11ShaderResourceView* DidIdSrv() const { return m_didN ? m_didIdSrv : nullptr; }
    ID3D11ShaderResourceView* DidRSrv() const { return m_didN ? m_did.RSrv() : nullptr; }
    DrawIdMv& DrawIds() { return m_did; }
    const DrawIdMv& DrawIds() const { return m_did; }
    // v0.10.0 phase 8 (dlaa.ini mv_medoid_drop): bit L = this unit's per-draw consensus of layer L is healthy, the camera
    // candidates of that layer need not be collected for its next passes (CandidateRecord::SetDropped). 0 when per-draw MVs /
    // the consensus are off or the unit is not ready.
    uint8_t MedoidDropBits() const {
        return (uint8_t)((m_did.ConsHealthy(0) ? 1u : 0u) | (m_did.ConsHealthy(1) ? 2u : 0u));
    }
    // v0.9.0: this unit feeds ObjIds from its moving readback (eye 0 / flat only, so both eyes share one id table).
    void SetObjFeed(bool on) { m_objFeed = on; }
    // v0.10.0 phase 2 (mirror units): no routine per-unit log lines (R_world / pairs / unpaired samples / first candidates /
    // ego), no one-time CB-copy check (a blocking Map), no DrawIdMv low-pair WARNING lines -- the caller's own stats line
    // reports these units. Default off (the world eyes and the preview units log as before).
    void SetQuiet(bool on) { m_quiet = on; m_did.SetQuiet(on); if (on) m_dbgDone = true; }
    // v0.9.0 MV debug view: the last Generate's stencil view, id table and valid-id mask (0 = no per-object MVs).
    // v0.9.0 r2: also while the debug view reserves an id for rejected identities (ObjDebugSlot, cyan).
    ID3D11ShaderResourceView* ObjStencilSrv() const { return (m_objMask || m_objDbgSlot) ? m_objStencil : nullptr; }
    ID3D11ShaderResourceView* ObjSlotSrv() const { return m_objSlotSrv.Get(); }
    uint32_t ObjMask() const { return m_objMask; }
    uint32_t ObjDebugSlot() const { return m_objDbgSlot; }   // v0.9.0 r2: stencil id of "rejected" identities (0 = none)
    // Cumulative counters (the periodic "MV objects" line reads deltas).
    // v0.9.0 r2: + rejected entries per reason (ObjIds::RejectWhy, GPU reasons), list overflows.
    struct ObjStats {
        uint64_t frames = 0, maskBits = 0, draws = 0, paired = 0, tagged = 0, unusable = 0, rbRead = 0, moving = 0;
        uint64_t rej[ObjIds::kRejKinds] = {};
        uint64_t movFull = 0, rejFull = 0;     // readbacks whose moving / reject list overflowed (entries lost)
        // v0.9.0 r4 (eye 0 readback header): tagged draws that contradicted their id's motion (the id fell back to the
        // camera R that frame), ids with tagged draws but no trusted non-spinning member that frame (camera R too)
        uint64_t vetoDraws = 0, noRepIds = 0;
        // v0.9.0 r6 TAG SWITCHES (eye 0, per readback, the pass vs the previous read-back pass): identities drawn in both
        // whose stencil tag went untagged -> tagged (on), tagged -> untagged (off) or id A -> id B (idChange). on + off =
        // pixels that flipped between the camera R and an object R -- what DLSS sees as a history mismatch.
        uint64_t tagOn = 0, tagOff = 0, tagIdChange = 0, tagCmp = 0;
        // v0.9.0 r7 (eye 0 readback tail): ids that kept their last R this frame (no usable R of their own: no trusted
        // representative, R not finite, or vetoed), ids whose hold ran out (camera R), trusted "moving" near draws
        // (origin < mv_objects_ego_m) that were camera-locked (own truck) / not locked (a vehicle passing close: a mover)
        uint64_t heldIds = 0, holdExpired = 0, nearLocked = 0, nearAccepted = 0;
        // v0.9.0 r8: frames whose valid-id mask was HELD (a pairing failure: unusable record or < 50 % of the last good
        // frame's pairs; the last good frame's ids kept their R through the r7 hold), mask-dropout diagnostics logged
        uint64_t maskHeld = 0, maskDropouts = 0;
        // v0.9.0 r9: world-miss frames answered with the last good camera R (no history reset; counted on every unit,
        // not only with objects), object-record draws paired through the fallback key (their full key had no previous
        // draw)
        uint64_t worldMissHeld = 0, fallbackPairs = 0;
        // v0.9.0 r11: camera-attached draws (origin within mv_objects_cam_m of the camera; eye 0 readback tail)
        uint64_t camOrigin = 0;
    };
    const ObjStats& GetObjStats() const { return m_objStats; }
    // v0.9.0 r11 forward-depth stat: pixels whose FORWARD depth lay in the cabin / near range (>= 0.9; pass B ignores
    // them since r11), counted in pass B on a 1-in-16 pixel grid (x 16: an estimate), read back asynchronously.
    // Cumulative, with the number of frames read back.
    uint64_t FwdNearPx() const { return m_fwdNearPx; }
    uint64_t FwdNearFrames() const { return m_fwdNearFrames; }
    // v0.9.0 r8 diagnostics: the FIFO pass this blit consumes (inject.cpp, before SceneDlaa::Run) -- its pass index,
    // the unconsumed passes after it, the FIFO overflow count and the newest pass. Only printed by the mask-dropout
    // line.
    void SetObjPassInfo(uint64_t s, int outstanding, uint64_t overflows, uint64_t newest) {
        m_objPassS = s; m_objPassOut = outstanding; m_objPassOvf = overflows; m_objPassNewest = newest;
        m_objPassKnown = true;
    }
    // Commits `cur` as the new prev without generating (frame without MV generation). Clears the stale flag.
    void Commit(ID3D11DeviceContext* ctx, const CandidateRecord& cur);
    // Forgets every candidate (toggle, resize): next frame has no history. v0.6.1: also forgets the good
    // solve / stale state and bumps InvalidateCount() (the blit's last-good record bookkeeping keys on it).
    void Invalidate();
    uint64_t InvalidateCount() const { return m_invalidations; }
    uint64_t BadFrames() const { return m_badFrames; }

    void Shutdown();

    // World-layer near filter (v0.4.1): pairs whose object-origin view depth |w|
    // is below this many metres are excluded from the medoid (own truck, trailer).
    void SetNearReject(float m) { m_nearReject = m < 0.0f ? 0.0f : m; }
    float NearReject() const { return m_nearReject; }

    // v0.6.0 ego reprojection: world draws whose object-origin view depth is below EgoOrigin metres are
    // ego candidates (own truck exterior: mirrors, hood, wheels); world-layer pixels closer than EgoPixel
    // metres use R_ego instead of R_world (0 = off, everything world as before).
    void SetEgoOrigin(float m) { m_egoOrigin = m < 0.0f ? 0.0f : (m > 30.0f ? 30.0f : m); }
    void SetEgoPixel(float m)  { m_egoPixel  = m < 0.0f ? 0.0f : (m > 20.0f ? 20.0f : m); }
    float EgoOrigin() const { return m_egoOrigin; }
    float EgoPixel() const  { return m_egoPixel; }

    // v0.7.8 tiled preview pictures (the game renders the truck preview as N tile passes, each one part of the view at
    // the full RT size, and composites them into one target): the candidates / R come from the REFERENCE tile, the MVs
    // must be pixels of the composited picture. Pass B maps a full-picture ndc to reference-tile ndc as
    // ndc * (sx, sy) + (ox, oy) before applying R and maps the result back. Default (1, 1, 0, 0) = untiled (world path).
    void SetTileXf(float sx, float sy, float ox, float oy) { m_tileXf[0] = sx; m_tileXf[1] = sy; m_tileXf[2] = ox; m_tileXf[3] = oy; }

    // v0.6.0 MV debug view: the solve buffer SRV and the dims cbuffer (Size, EgoPixelM; v0.6.4 + Origin, FullSize) of the last
    // Generate(), so the debug shader can redo pass B's ego classification. Only valid after a Generate()
    // that returned true.
    ID3D11ShaderResourceView* SolveSrv() const { return m_solveSrv.Get(); }
    ID3D11Buffer* DimsCb() const { return m_cbDims.Get(); }
    ID3D11ShaderResourceView* FwdIdSrv() const { return m_didN ? m_fwdIds : nullptr; }   // v0.10.0 phase 9 (debug view, t8)

    const FrameStats& Last() const { return m_last; }

    // v0.5.1: every Generate() copies the solve buffer into a ring of 3 staging buffers and polls the
    // oldest ones with MAP_FLAG_DO_NOT_WAIT (never stalls). TakeSolve() returns true once per new
    // readback and fills R_world (row-major 16 floats, 2-3 frames old).
    // v0.6.0: the ring reads the whole solve buffer (m_rLast = kSolveFloats); TakeSolve still hands out R_world.
    bool TakeSolve(float rWorld[16]) {
        if (!m_rNew) return false;
        m_rNew = false;
        for (int i = 0; i < 16; ++i) rWorld[i] = m_rLast[i];
        return true;
    }

    // Snapshot (v0.4.2, F10): blocking readback of the solve buffer (v0.6.0: kSolveFloats = 72 floats:
    // R_world[0..15], R_cabin[16..31], stats[32..47], R_ego[48..63], InvRow3[64..67], ego info[68..71]) as
    // left by the last Generate(). Uses its own staging buffer.
    bool ReadSolveBlocking(ID3D11DeviceContext* ctx, float out[kSolveFloats]);

private:
    void DebugReadback(ID3D11DeviceContext* ctx, const CandidateRecord& cur);
    void SolveReadback(ID3D11DeviceContext* ctx);
    void RingStep(ID3D11DeviceContext* ctx);
    // v0.6.1 shared by the normal and the reuse path of Generate().
    bool WriteDims(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, uint32_t x0, uint32_t y0,
                   uint32_t fullW, uint32_t fullH);
    void DispatchReproj(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, ID3D11ShaderResourceView* depthTwinSrv,
                        ID3D11UnorderedAccessView* depthR32Uav, ID3D11UnorderedAccessView* mvUav);
    void LogFrameLine(const FrameStats& fs, const CandidateRecord& cur);
    // ---- v0.9.0 per-object MVs (see ObjRecord) ----
    bool InitObjects();                         // lazily: shader + buffers (false = feature unavailable for this unit)
    // CPU side: pairs the record with this eye's previous one, uploads the table + constants. Returns the valid-id mask
    // (0 = nothing to do this frame; pass B then ignores the stencil).
    // camOk = the solve buffer's R_world is a real camera solve (else no "moving" verdicts this frame).
    // v0.9.0 r9: worldMiss = this frame's camera candidates paired nothing: treated as a pairing failure (mask hold,
    // the dropout diagnostic names "world-miss"); `missWhy` = what the camera path did about it (held R / history
    // reset)
    uint32_t ObjPrepare(ID3D11DeviceContext* ctx, const ObjRecord* objs, uint32_t fullW, uint32_t fullH, bool camOk,
                        bool worldMiss = false, const char* missWhy = nullptr);
    void ObjDispatch(ID3D11DeviceContext* ctx, const ObjRecord* objs);   // after pass A (camera R), before pass B
    void ObjFinish(ID3D11DeviceContext* ctx, const ObjRecord* objs);     // readback + commit (ping-pong, prev key map)
    // no object history (the next frame pairs nothing; v0.9.0 r7: and holds nothing)
    // v0.9.0 r8: + the dropout diagnostic's "previous mask" (callers that log a dropout call ObjNoRun first)
    void ObjForget() { m_objPrevN = 0; m_objMask = 0; m_objDbgSlot = 0; m_objHoldOk = false; m_objDiagPrevMask = 0; }
    void ObjPollReadback(ID3D11DeviceContext* ctx);
    // v0.9.0 r8: record-less mask hold (unusable record): cbuffer for a CSResolve-only dispatch; true = held
    bool ObjHoldEmpty(ID3D11DeviceContext* ctx, uint32_t fullW, uint32_t fullH);
    // v0.9.0 r8: the rate-limited "MV objects: mask dropout" line (previous blit had ids, this one none / no run /
    // pairing collapse); `why` = the path, `hold` = what the mask hold did (nullptr: derived from the hold state)
    void ObjDiag(const char* why, const ObjRecord* objs, int n, int paired, uint32_t mask, const char* hold);
    void ObjNoRun(const char* why, const ObjRecord* objs);    // r8: a Generate / frame without the object path at all
    // the object cbuffer (Counts, Params, Img, Ids) for draws `n`, prev list length `prevN`, valid-id mask, hold frames
    void ObjWriteCb(void* dst, uint32_t n, uint32_t prevN, bool camOk, uint32_t mask, uint32_t hold, uint32_t fullW,
                    uint32_t fullH) const;

    static constexpr int kRing = 3;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_ring[kRing];
    bool     m_ringPending[kRing] = {};
    uint64_t m_ringSeq[kRing] = {};
    uint64_t m_ringCtr = 0;
    float    m_rLast[kSolveFloats] = {};   // v0.6.0: whole solve buffer (was R_world only)
    bool     m_rNew = false;
    bool     m_egoLogged = false;          // v0.6.0 one-time "ego reprojection active" line

    bool m_ready = false;
    bool m_failed = false;
    uint64_t m_frame = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> m_dev;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csSolve, m_csReproj;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_solve;                  // kSolveElems (18) x float4 structured
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_solveUav;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_solveSrv;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbPairs, m_cbDims;      // dynamic constant buffers (v0.6.4: dims 32 B)
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_stageDbg, m_stageSolve; // staging readbacks
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_stageSnap;              // blocking snapshot readback (on demand)

    CandidateRecord m_prev;                                        // this eye's last committed record

    FrameStats m_last;
    bool     m_loggedFirst = false;
    bool     m_dbgDone = false;
    bool     m_quiet = false;             // v0.10.0 phase 2 SetQuiet (mirror units)
    int      m_solvePending = 0;      // frames until the solve staging copy is read
    uint64_t m_solveFrame = 0;        // frame number the pending staging copy was taken at
    int      m_driveLogs = 0;         // "R_world not identity" lines written (cap 200)
    float    m_nearReject = 30.0f;
    float    m_egoOrigin = 8.0f;      // v0.6.0 mv_ego_origin_m
    float    m_egoPixel = 3.0f;       // v0.6.0 mv_ego_pixel_m (0 = off)
    float    m_tileXf[4] = { 1.0f, 1.0f, 0.0f, 0.0f };   // v0.7.8 full -> reference-tile ndc (SetTileXf)
    uint64_t m_missFrames = 0, m_genFrames = 0;
    uint64_t m_pairSum[kLayers] = {0, 0};
    // ---- v0.6.1 reuse of the last good reprojection on incomplete candidate records ----
    bool     m_solveGood = false;     // solve buffer holds a pass A result WITH world pairs (since Invalidate)
    bool     m_prevStale = false;     // a bad record was skipped: m_prev is >= 2 frames old
    uint64_t m_badFrames = 0;         // bad records answered with the last good R (not committed)
    uint64_t m_staleFrames = 0;       // first good records after a bad one (R reused once more, committed)
    uint64_t m_invalidations = 0;     // Invalidate() calls
    // ---- v0.9.0 r9 world-miss hold + fallback pairing ----
    static bool s_pairFallback;       // SetPairFallback (default true)
    static float s_movingPx;          // v0.9.0 r11 SetMovingPx (default 0.5; synthetic test only)
    int      m_missHoldRun = 0;       // world-miss frames in a row answered with the last good R (<= mv_objects_hold)
    uint64_t m_missHeld = 0;          // world-miss frames held (all units; ObjStats::worldMissHeld is the same count)
    uint64_t m_candFallback = 0;      // camera candidate pairs found through the loose key
    int      m_unpairedLogs = 0;      // "MV: unpaired-draw sample" lines written (first 5)
    int      m_objUnpairedLogs = 0;   // "MV objects: unpaired-draw sample" lines written (first 5)
    // ---- v0.9.0 per-object MVs ----
    static constexpr int kObjMax = ObjRecord::kMax;
    static constexpr int kObjCands = 8;        // prev draws with the same geometry key tried per draw (nearest wins)
    // v0.9.0 r4: 256 moving entries (r3: 128 overflowed in 272-512 of 600 readbacks in busy traffic: random members went
    // "unseen" and random waiting identities restarted -- id churn); 128 B per entry (+ spin)
    static constexpr int kObjMoving = 256;     // moving entries per readback
    static constexpr UINT kObjMovStride = 128; // bytes per moving entry (32 floats; r2: + displacement, r4: + spin), header 16 B
    static constexpr int kObjRej = 256;        // v0.9.0 r2: reject entries per readback (after the moving list)
    // v0.9.0 r3: bytes per reject entry (r2: 16; + origin, 2 partner origins); r10: 80 (+ view-space origin)
    static constexpr UINT kObjRejStride = 80;
    static constexpr UINT kObjRejBase = 16u + (UINT)kObjMoving * kObjMovStride;
    // v0.9.0 r7: a tail after the reject list (cleared per frame with the rest): held-id mask, hold-expired mask,
    // near-locked and near-accepted draws (16 B), then per id CSVeto's verdict bits, worst contradiction (px) and a vetoed
    // draw (3 x 64 B)
    static constexpr UINT kObjTailBase = kObjRejBase + (UINT)kObjRej * kObjRejStride;
    // r11: + 16 B (camera-attached draws at kObjTailCam)
    static constexpr UINT kObjTailBytes = 16u + 3u * 64u + 16u;
    static constexpr UINT kObjTailCam = 16u + 3u * 64u;
    // v0.9.0 r7: SlotR = 16 x 4 rows + 16 states (r4..r6: 80 float4) + a copy of both as of the frame's start
    static constexpr UINT kObjSlotElems = 160;
    static constexpr int kObjRb = 3;           // readback ring depth
    static constexpr float kObjPairMargin = 0.25f;   // v0.9.0 r2: unambiguous pairing: second >= 1.5 x best + this (clip ~ m)
    // v0.9.0 r4: CSVeto absolute tolerance (px; + 35 % of the id's correction). v0.9.0 r6: gross errors only -- 4 px or 50 %
    // (round-4 log: up to 2.4 vetoed draws per readback, each one flipped a whole vehicle to the camera R for a frame)
    static constexpr float kObjVetoPx = 4.0f;
    static constexpr float kObjVetoRel = 0.5f;       // v0.9.0 r6 (r4: 0.35)
    bool m_objFeed = false;                    // feeds ObjIds (eye 0 / flat)
    bool m_objRun = false;                     // ObjPrepare uploaded a usable record this Generate (dispatch + commit)
    int  m_objRunN = 0;                        // its draw count
    float m_objRunW = 0.0f, m_objRunH = 0.0f;  // its full image size
    bool m_objReady = false, m_objFailed = false;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csObj;
    // v0.9.0 r4: per-id representative (CSResolve, 16 threads) and same-frame veto (CSVeto, one thread per draw) + their
    // scratch (raw: [0..15] winner key per id, [16 + i] = partner index of draw i in the previous pass)
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csObjResolve, m_csObjVeto;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csObjHold;              // v0.9.0 r7: applies CSVeto's verdict per id
    // v0.9.0 r7: the last Generate ran the object dispatch (SlotR holds LAST frame's ids / R): only then may this frame
    // hold an R (ObjPrepare passes mv_objects_hold, else 0). Cleared by ObjForget (Invalidate, reset, a frame without
    // the dispatch).
    bool m_objHoldOk = false;
    // v0.9.0 r8 MASK HOLD: the last GOOD frame's (not held) valid-id mask and paired-draw count, the held frames in a
    // row (<= mv_objects_hold), this frame is a record-less hold (CSResolve only, no readback, no commit)
    uint32_t m_objGoodMask = 0;
    int      m_objGoodPaired = 0;
    int      m_objDiagGood = 0;                       // m_objGoodPaired as it was when this frame started (diagnostics)
    int      m_objMaskHoldRun = 0;
    bool     m_objHoldRun = false;
    // v0.9.0 r8 diagnostics: the previous Generate's mask, the last good record's tagged draws, FIFO pass info
    uint32_t m_objDiagPrevMask = 0;
    uint32_t m_objPrevTagged = 0, m_objCurTagged = 0;
    uint64_t m_objPassS = 0, m_objPassOvf = 0, m_objPassNewest = 0, m_objPrevPassS = 0;
    int      m_objPassOut = 0;
    bool     m_objPassKnown = false, m_objPrevPassKnown = false;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_objScratch;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_objScratchUav;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_objMvp[2];                     // gathered MVPs, raw, ping-pong (cur / prev)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_objMvpSrv[2];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_objMvpUav[2];
    int m_objPing = 0;                                                    // index of the CURRENT gather target
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_objTable, m_objPrevList;      // dynamic structured uint4 / uint (CPU -> GPU)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_objTableSrv, m_objPrevListSrv;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_objSlot;                       // structured float4 x kObjSlotElems (r7: 160)
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_objSlotSrv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_objSlotUav;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_objMov;                        // raw: count + kObjMoving entries
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_objMovUav;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_objCb;                         // dynamic cbuffer
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_objRbBuf[kObjRb];              // staging copies of m_objMov
    bool     m_objRbPending[kObjRb] = {};
    uint64_t m_objRbSeq[kObjRb] = {};
    uint64_t m_objRbCtr = 0;
    int      m_objRbN[kObjRb] = {};                                       // draws of the pass each copy belongs to
    float    m_objRbW[kObjRb] = {}, m_objRbH[kObjRb] = {};
    uint64_t m_objRbIds[kObjRb][kObjMax];                                 // that pass's identity ids (curIdx -> id)
    uint64_t m_objRbKh[kObjRb][kObjMax];                                  // v0.9.0 r9: ... their geometry key hashes
    uint16_t m_objRbDup[kObjRb][kObjMax];                                 // v0.9.0 r2: ... their geometry-key dup counts
    uint64_t m_objRbFrame[kObjRb] = {};                                   // v0.9.0 r2: ... the frame (m_frame) of the copy
    // v0.9.0 r6: ... per draw: stencil tag (bits 0..3, 0 = untagged / the debug view's "rejected" id) | 0x10 = paired
    uint8_t  m_objRbInfo[kObjRb][kObjMax];
    uint8_t  m_objCurInfo[kObjMax];                                       // v0.9.0 r6: the same for the pass being prepared
    // v0.9.0 r6 tag-switch metric: identity -> tag of the last two read-back passes (open addressing, generation stamped)
    static constexpr int kSwTab = 4096;                                   // power of two, >= 2 x kObjMax
    struct SwEnt { uint64_t id; uint32_t gen; uint8_t tag; };
    SwEnt    m_swTab[2][kSwTab] = {};
    uint32_t m_swGen[2] = { 0, 0 };
    int      m_swCur = 0;                                                 // table of the last read-back pass
    bool     m_swHave = false;                                            // a previous pass is in m_swTab[m_swCur]
    void     ObjTagSwitches(int rb);                                      // compares readback rb's pass with the last one
    // previous committed record of this eye (CPU): key hash -> contiguous run in the prev list (sorted by key hash)
    int      m_objPrevN = 0;                                              // 0 = no history
    uint64_t m_objPrevKh[kObjMax];                                        // sorted key hashes
    uint32_t m_objPrevIdx[kObjMax];                                       // matching draw index in the prev MVP buffer
    uint16_t m_objPrevOcc[kObjMax];                                       // v0.9.0 r2: its occurrence index
    // v0.9.0 r9 fallback pairing: the previous record's loose keys sorted (+ draw index), each draw's position in the
    // key-hash-sorted prev list (what Table.y points at), its raw geometry key (diagnostic); this pass's loose keys
    // sorted
    uint64_t m_objPrevLkh[kObjMax];
    uint32_t m_objPrevLIdx[kObjMax];
    uint32_t m_objPrevPos[kObjMax];                                       // by draw index
    ObjRecord::Geo m_objPrevGeo[kObjMax];                                 // by draw index
    uint64_t m_objCurLkh[kObjMax];
    uint32_t m_objCurLIdx[kObjMax];
    int      m_objPrevNL = 0, m_objCurNL = 0;                             // entries in the two loose lists
    // v0.9.0 r2: this pass sorted by key hash (ObjPrepare: dup counts; ObjFinish commits it as the next prev map)
    uint64_t m_objCurKh[kObjMax];
    uint32_t m_objCurIdx[kObjMax];
    uint16_t m_objCurDup[kObjMax];                                        // by draw index: draws of its key this pass
    uint16_t m_objDup[kObjMax];                                           // by draw index: max(this, prev pass) dup
    uint32_t m_objMask = 0;                                               // valid-id mask of the last Generate
    uint32_t m_objDbgSlot = 0;                                            // v0.9.0 r2: ObjIds::DebugSlot() of the last Generate
    ID3D11ShaderResourceView* m_objStencil = nullptr;                     // stencil view of the last Generate (no ref)
    ID3D11ShaderResourceView* m_fwdDepth = nullptr;                       // v0.9.0 r5: forward depth of the last Generate (no ref)
    uint32_t m_fwdShift = 0;                                              // v0.10.0 phase 9: its resolution shift (0 / 1)
    ID3D11ShaderResourceView* m_fwdIds = nullptr;                         // v0.10.0 phase 9: 1/2-size forward ids (no ref)
    // v0.9.0 r11 near-range forward-depth counter of pass B (u2: 64 striped dwords) + async readback ring
    static constexpr int  kFwdCntRb = 3;
    static constexpr UINT kFwdCntBytes = 256;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_fwdCnt, m_fwdCntRb[kFwdCntRb];
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_fwdCntUav;
    bool     m_fwdCntPending[kFwdCntRb] = {};
    bool     m_fwdCntFailed = false;
    uint64_t m_fwdNearPx = 0, m_fwdNearFrames = 0;
    bool     FwdCountInit();
    void     FwdCountPoll(ID3D11DeviceContext* ctx);
    bool     m_objLogged = false;
    ObjStats m_objStats;
    // ---- v0.10.0 per-draw MVs ----
    DrawIdMv m_did;                                                       // pairing + R table of this eye
    uint32_t m_didN = 0;                                                  // pass B uses draw ids this Generate (draws)
    uint32_t m_didNG = 0;                                                 // v0.10.0 phase 3: of them G-buffer draws
    ID3D11ShaderResourceView* m_didIdSrv = nullptr;                       // this Generate's draw-id target (no ref)
    bool     m_didInitTried = false;
    bool     m_didStaticGate = false;                                     // v0.10.0 phase 14: the pass's replay ran with the
                                                                          // static gate (FwdMode.z: the Ctrl+F6 view's id-0 olive)
    // prepares the record (CPU grouping + uploads); sets m_didN when pass B can use the ids. Returns the draws prepared.
    // v0.10.0 phase 3: + the forward record and whether pass A wrote this frame's medoid (the consensus statistic).
    uint32_t DidPrepare(ID3D11DeviceContext* ctx, const DrawIdRecord* did, ID3D11ShaderResourceView* idSrv,
                        uint32_t fullW, uint32_t fullH, const DrawIdRecord* didFwd, bool freshMedoid, uint32_t noMedoid = 0);
};

#endif // WITH_DLAA
