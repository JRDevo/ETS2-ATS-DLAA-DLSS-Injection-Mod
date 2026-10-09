// DrawIds implementation (v0.10.0). See draw_ids.h and docs/DLAA_INTEGRATION.md "Per-draw motion vectors (v0.10.0)".
#ifdef WITH_DLAA

#include "draw_ids.h"
#include "gpu_perf.h"
#include "shader_cache.h"
#include "log.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kNone = 0xFFFFFFFFu;

inline uint64_t Mix(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33;
    return x;
}
inline int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

// ---------------------------------------------------------------------------------------------------------------------
// Replay pixel shader: no inputs (an empty input signature links with any vertex shader output), the draw id comes from
// one 256-byte window of an immutable cbuffer (window k holds k), bound per draw with PSSetConstantBuffers1.
// v0.10.0 phase 3: the id goes to both channels; the replay's blend write mask picks one (G-buffer RED, forward GREEN of an
// R16G16_UINT target; an R16_UINT target simply drops GREEN).
// kDrawIdPs-BEGIN (the build validates this block with fxc, ps_5_0 / PSMain)
const char kDrawIdPs[] = R"(
cbuffer DrawIdCB : register(b0) {
    uint4 DrawId;        // x = draw id (0 = no draw)
};
uint2 PSMain() : SV_Target0 {
    return uint2(DrawId.x, DrawId.x);
}
)";
// kDrawIdPs-END

// v0.10.0 phase 10 forward-depth OCCLUSION INIT (DrawIdRecord::InitFwdDepth; dlaa.ini mv_fwd_depth_cull): a full-screen triangle
// over the forward-depth target writes min(2^shift x 2^shift texels of the depth snapshot) * (1 - 1e-6) inside the world range,
// else 0 (far), through SV_Depth -- strictly below the scene depth, so it never wins pass B (fw > dt), but the batched forward
// re-draw that follows (GREATER_EQUAL) is rejected wherever a forward fragment lies behind the scene at all of its pixels.
// kFwdInitShader-BEGIN (the build validates this block with fxc: vs_5_0 / VSMain, ps_5_0 / PSInit0 and PSInit1)
const char kFwdInitShader[] = R"(
Texture2D<float> Snap : register(t0);   // the pass's depth snapshot (R32_FLOAT_X8X24 view, reversed-Z)
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((float)((id << 1) & 2u), (float)(id & 2u));
    return float4(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);
}
float InitDepth(float4 pos, uint shift) {
    uint w, h;
    Snap.GetDimensions(w, h);
    uint2 p = uint2(pos.xy) << shift;
    uint n = 1u << shift;
    float m = 1.0;
    [unroll] for (uint y = 0u; y < 2u; ++y)
        [unroll] for (uint x = 0u; x < 2u; ++x)
            if (x < n && y < n) m = min(m, Snap[min(p + uint2(x, y), uint2(w - 1u, h - 1u))]);
    return (m >= 0.01 && m < 0.9) ? m * 0.999999 : 0.0;
}
float PSInit0(float4 pos : SV_Position) : SV_Depth { return InitDepth(pos, 0u); }
float PSInit1(float4 pos : SV_Position) : SV_Depth { return InitDepth(pos, 1u); }
)";
// kFwdInitShader-END

// ---------------------------------------------------------------------------------------------------------------------
// Per-draw pairing + R table (DrawIdMv, one thread per draw; three entry points of one source):
//  CSGather  : Mc = this draw's MVP (rows 4..7 of its VS cb0 window, from the ring mirror) -> CurM (the next frame's
//              PrevM); P = R_layer * Mc (where the camera alone puts this draw in the previous frame) -> Work.
//  CSMatch   : threads [0, n): each current draw -> the nearest previous draw of its group (origin distance in previous
//              clip space between P and Mp, tie-break the whole-matrix distance, then the draw-order rank in the group);
//              threads [n, n + m): each previous draw -> the nearest current draw of its group (same metric).
//  CSResolve : a current draw is PAIRED only when the choice is mutual and the origin moved less than Params.x (clip
//              units ~ m) against the camera prediction. R_draw = Mp * inverse(Mc). If R_draw moves the draw's on-screen
//              probe points (origin, +-4 on local x / z, +4 on y) less than Params.y px away from the camera R, the draw
//              is STATIC and keeps the camera R (state 2) -- bit-identical to the camera path; else it is a MOVER (state
//              3, its own R). Unpaired (1) / instanced (4) / no MVP (5): the camera R of the draw's layer.
//              TRACK CONTINUITY (ambiguous groups only: more than one draw of the key on a side, or a pointer-free-key
//              group): a mover verdict also needs the previous draw to have been mutually paired itself last frame with
//              a consistent displacement (|d - d_prev| <= Params.z + 25 % of |d_prev|). Without it a culled copy of an
//              identical mesh (grass clump, window, wheel) "moves" to its neighbour's place the frame the neighbour
//              appears; that pair is answered with the camera R (state 1) instead. Identical movers get their own R
//              from their second paired frame on.
// RTab[i*5 + r] = row r of the draw's R, RTab[i*5 + 4] = (state, viewport MinDepth, MaxDepth, deviation px).
// CurM / PrevM: 80 bytes per draw = the MVP (rows) + the track (prev-clip displacement of the origin against the camera
// prediction x, y, w; flags: 1 = mutually paired this frame).
// Cnt dwords: [state] resolve state counts, [8] pairs found through the pointer-free key, [9] track rejects, [10] / [11]
// forward draws paired / movers, [16] / [17] / [18] paired world / far world / cabin consensus voters, [20..25] world and
// [28..33] cabin consensus (state 0 none / 1 consensus / 2 medoid fallback / 3 off, cluster draws, avg and max |consensus -
// medoid| px (float bits), valid candidates, 1 = compared with a fresh medoid), [26] world voters included near draws
// (fewer than Cons.z far ones); pass B adds [64 + c*16 + s] (c 0..6).
// v0.10.0 phase 3 CONSENSUS CAMERA R (in-game log 2026-10-08: the medoid of pass A -- 16 sampled candidate pairs, unique
// meshes first -- picked a MOVING vehicle as the camera reference for seconds, the camera R was off by 1-2 px, and every
// pixel that uses it smeared: instanced grass, unpaired draws, sky, forward wires). The per-draw pairs ARE the evidence:
//  CSPair    : the mutual pair + R_draw = Mp * inverse(Mc) per draw (Work) and the partner index; counts the voters.
//  CSVote    : one 64-thread group per CANDIDATE (Cand[], chosen on the CPU: evenly spaced draws of the layer): how many paired
//              draws of its layer (world voters: origin >= Params2.z m from the camera when at least Cons.z such draws exist
//              -- the own truck / trailer move with the camera) agree with the candidate's R within Params2.y px at their
//              own probe points (ProbeDev(Mp_j, R_cand * Mc_j)).
//  CSPick    : per layer the candidate with the most agreeing draws (tie: the smaller mean deviation). A cluster of >= Cons.z
//              draws replaces the medoid in the solve buffer (SolveRW, Solve[8 + layer].w = its size); else the medoid
//              stays. Static scenery is hundreds of draws, vehicles tens -- a convoy moving together forms a cluster too,
//              but a smaller one.
//  CSResolve then decides static / mover (and writes the track) against that final camera R, and pass B, the debug view
//  and the next frame's prediction read it from the solve buffer.
// v0.10.0 phase 3 FORWARD DRAWS (Tab flag 512): the forward pass's re-drawn draws are appended after the G-buffer draws (one
// id space); CSGather reads their MVPs from the forward record's mirror (t7). They pair only among themselves (own keys) and
// never vote.
// v0.10.0 phase 4 ATTACH (in-game 2026-10-08: a bus plate was a forward MOVER whose own R moved it <= 0.5 px from the camera
// R while the bus body moved many px -- the plate's own pair carried no vehicle motion):
//  CSAttach  : one thread per FORWARD draw, after CSResolve: the nearest G-buffer MOVER (state 3, same layer) whose MVP origin
//              lies within Params2.w metres of the forward draw's origin (view-space distance from the two clip origins; x / y
//              divided by the mover's own |row 0| / |row 3| and |row 1| / |row 3| = P00 / P11) gives the forward draw its R
//              (state 3, RTab .w = -(1 + mover index) = attached), unless the forward draw is a mover itself whose R agrees
//              with the mover's within 0.5 px at its probes (kept). Work [kAttach + i*4] = the mover index or kNone (the dump
//              reads it); Cnt [12] attached, [13] kept (agreed). Params2.w = 0: off. Counts.w = the G-buffer draws.
// v0.10.0 phase 5 INHERIT (in-game 2026-10-08 12:00: plates on moving trucks were WHITE in Ctrl+F6 = recorded draws that are
// UNPAIRED every frame -- no partner group: their keys are new each frame -- so they took the camera R and DLAA rejected their
// history; the phase-4 attach never fired: 0.00/frame):
//  CSInherit1: one thread per draw, after CSResolve: a draw whose OWN pair made it static / mover (state 2 / 3) inserts itself
//              into a hash table of its 16 MVP words (Work kHash, 2n slots, linear probing; the slots are cleared by CSGather;
//              only when Cons.w bit 2 = mv_drawid_twin); a G-buffer MOVER also appends itself to a compact list (kMovCnt /
//              kMovList, the candidates of CSAttach).
//  CSTwin    : a draw still at state 1 (no partner group, oversized group, continuity reject, not mutual) whose MVP is
//              BIT-identical (-0 == +0) to such a draw's (same layer; G-buffer twins before forward ones, then the lowest
//              index) takes the twin's R rows and state -- a plate, a lamp, a mirror arm drawn with the vehicle's own matrix
//              moves exactly like the body. RTab .w = -(1 + kMaxDraws + twin) marks it; Work [kTwin + i*4] = the twin (dump);
//              Cnt [48] twins, [49] of them movers, [52] state-1 draws looked up, [53] forward draws among the twins.
//  CSAttach  : (phase 4, extended) also a G-buffer draw still at state 1 without a twin takes the nearest G-buffer mover's R
//              when the origins lie within Params2.w AND the MVP columns 0..2 (projected rotation / scale) agree within 1e-3
//              (a static draw reappearing at a vehicle's pivot must not move with it); forward draws keep the phase-4 rule;
//              a draw with a twin is never attached. Candidates = the mover list. Cnt [50] G-buffer draws attached.
// v0.10.0 phase 6 RIGID PARENTS (in-game 2026-10-08 12:42: the plates still unpaired after phase 5 carry the bare PROJECTION in
// cb0 rows 4..7 -- column 3 = (0, 0, 0.2, 0): view-space vertices written by the CPU every frame -- so neither a twin nor an
// origin can find their vehicle). R_U = R_P for any node rigidly fixed on P (column-vector convention, see CSParentList), so
// only the PARENT must be found -- the draw the plate sits on, by a vote of the neighbouring pixels in the draw-id target:
//  CSParentList : one group, after CSResolve: every state-1 draw gets a U slot (<= 64, draw order); the vote tables are cleared.
//                 Cnt [54] U's listed, [55] U's over the cap (no vote).
//  CSParentVote : after CSTwin / CSAttach, one thread per ParU.y-th pixel in x and y of the full-size draw-id target: a pixel of
//                 a U (RED id, scene depth; GREEN id where the forward depth won, forward depth) samples the RED ids at +-2 /
//                 +-5 px; a sample within Par.x relative linear depth votes for its draw when that draw is a G-buffer draw
//                 paired by its OWN pair (static / mover) or -- a chain vote -- another U. Cnt [192] votes.
//  CSParentPick1: the real candidate with >= Par.y votes and >= Par.z of the real votes is the parent P: a mover gives U its R
//                 rows and state 3, a static parent state 2 (camera R); RTab .w = -(1 + 2 kMaxDraws + P). It decides over a
//                 phase-5 twin / origin attach of the same draw (Cnt [62] overrides, [193] with another state).
//  CSParentPick2: a U without own evidence (< Par.y real votes) whose chain candidate was resolved by pass 1 takes that U's
//                 root parent (plate text inside an unpaired plate background). Work kParent / kParVT = parent / votes (dump).
//                 Cnt [56] parents, [57] movers, [58] forward, [59] via a chain, [60] no votes at all, [61] no majority,
//                 [194] listed draws left with neither a parent nor a twin / origin attach (camera R).
//  CSTwin skips (Cnt [63]) a draw whose MVP column 3 is (0, 0, c, 0): the projection is shared by every view-space draw.
// v0.10.0 phase 6b INSTANCED PLATES (ParU.x bit 2, dlaa.ini mv_drawid_instanced = 1 default): a close car's licence plate is a
// DrawIndexedInstanced draw -- its cb0 rows 4..7 are not the instances' MVP (flag kFInst), so it has no pair, no twin and no
// origin: it stayed at the camera R and ghosted. When replayed into the draw-id target (mv_drawid_instanced), every kFInst /
// kFNoMvp draw joins the rigid-parent vote exactly like an unpaired draw:
//  CSParentCount: one thread per kParStride-th pixel counts each replayed kFInst / kFNoMvp draw's on-screen RED-id pixels
//                 into kParPix (so the <= 64 vote slots go to the instanced draws that actually cover the screen).
//  CSParentList : the genuine state-1 draws take their slots first (unchanged); kFInst / kFNoMvp draws with pixels fill the rest,
//                 most pixels first (tie: the lower index). Cnt [195] = instanced listed, [197] = instanced candidates with pixels.
//  CSParentVote / Pick: their pixels vote and take a parent exactly like an unpaired draw; a mover parent -> state 3 + its R
//                 (the plate moves with the car), a static parent -> state 2 (camera R), no parent -> the camera R as before.
//                 They are never a parent source while listed (phase 6c: ParSource). Cnt [196] = instanced -> parent.
// v0.10.0 phase 6c MARCHING VOTE (in game 2026-10-08 17:27, log dlaa_inject_ets2_v0100p6b_flat.log: a close car's plate took its
// parent only some of the time -- "no votes 22-33, no majority 18-29" per frame: a plate is a flat patch, its +-2 / +-5 px
// neighbours are mostly the plate itself or its own text draw, so only the thin border ring reached the body; far plates never
// reached 32 votes). Same GPU passes, same frame:
//  CSParentCount: always runs with the parents: it also counts each genuine unpaired draw's RED / GREEN id pixels -> the march
//                 decimation of every listed draw (ParDecim: <= ParU.w marches per pass, spread evenly over the draw).
//  CSParentVote : every sampled pixel of U MARCHES in 8 directions (ParU.y = 2 px steps, up to ParU.z = 32 px) across U's own
//                 pixels and the depth-continuous pixels of draws WITHOUT a motion (plate background / text, other unpaired or
//                 instanced draws); the first draw WITH a motion (ParSource: state 2 / 3 by its own pair, a twin, an origin
//                 attach; never a listed draw in the 1st pass) is that direction's vote when it lies within Par.x = 4 % of
//                 U's linear depth (else the march is depth-rejected); sky / no id / another layer / out of reach = no source.
//  CSParentPick1: winner = the most votes (tie: nearer in depth to U, then the lower index) with >= Par.y = 8 votes and
//                 >= Par.z = 50 % of U's accepted votes. Losers keep their 1st-pass evidence; their tables are cleared.
//  2nd pass     : CSParentVote again (pass flag), only for listed draws still without a parent / twin / origin attach, now
//                 with the listed draws that hold a motion as sources; CSParentPick2 = the same rule, the winner's ROOT
//                 parent is recorded (kParVT bit 31). Reasons for the rest: no majority Cnt [61], all sources depth-rejected
//                 [199], no source within reach [60]. Cnt [200] / [201] / [202] / [203] = 1st-pass marches / marched px /
//                 depth-rejected marches / marches without a source.
// v0.10.0 phase 9 VR COST (first VR log: the per-pixel passes covered the whole 6120x6496 eye while NGX reads only the DLAA-area
// rect, 2448x2600 at dlaa_area 40):
//  Clip     : CSParentCount / CSParentVote run over the G-buffer replay's clip only (DrawIdRecord::SetView: the area rect + a
//             margin; everything outside holds id 0, so a march that leaves it ends there like at the sky). x0 / y0 are aligned
//             down to 4 px, so every sample sits on the same global grid as in phase 8 (ParVoter's decimation is unchanged);
//             CSParentList sizes the indirect vote grid from it. Without a clip it is the whole image = phase 8 exactly.
//  FwdMode.x: 1 = the forward depth (PFwd) and the forward ids (PFwdIds, R16_UINT, t11) are at 1/2 resolution (dlaa.ini
//             mv_fwd_depth_res 2): read at pixel >> 1 (FwdIdAt / FwdDepthAt); 0 = GREEN channel + full-size depth (phase 8).
//  FOLDED   : every dispatch has a fixed GPU floor (+ the barrier the driver puts between dependent dispatches); the per-eye
//             chain had 15. CSPickResolve (one group of 256): the consensus pick (threads 0 / 1, FoldU.x bit 0) -> the layers'
//             final camera R from SolveRW (the solve buffer is a UAV for the whole dispatch, Solve / t5 unbound) -> ResolveDraw
//             for every draw -> Inherit1Draw (bit 1). CSListTwin (one group of 256): ParentListGroup (bit 2; u5 = the vote's
//             indirect args) -> TwinDraw (bit 3) -> AttachDraw (bit 4). Same code and order as the separate entry points, the
//             group's device-memory barriers in place of the dispatch boundaries. gather / match / pair / vote / the pixel count /
//             the vote passes stay separate (each needs every draw or pixel of the step before: a grid-wide dependency).
//             ParentListGroup also ranks the instanced candidates in a compact groupshared list (was: each candidate looped
//             over all n draws in device memory, "p-list 0.2 ms" per eye in VR); more than kInstCap = the old loop.
// v0.10.0 phase 14 STATIC REPLAY SKIP (draw_ids.h step 11): three more Cnt bitmasks, read back with the diagnostics -- dwords
// 384..511 bit i = G-buffer draw i is PAIRED AND STATIC by its own pair (ResolveDraw state 2; nothing later changes a state-2
// G-buffer draw), dwords 512..639 bit p = G-buffer draw p was a rigid parent this pass (ParApply), dwords 640..767 bit i = draw i
// MOVES (StatReset: an own-pair mover, a track reject, a moving twin / origin attach / rigid parent). A draw in neither the static
// nor the reset mask (no partner: new / culled back in, oversized group) is NEUTRAL: it neither builds nor breaks its key's streak
// (30 identical grass clumps, 3 culled at random each frame, would otherwise reset their key every frame) but HOLDS the key (an
// unpaired copy needs its pixels for its own rigid-parent vote). The CPU keeps a per-key static streak from them and skips the
// replay of long-static keys (inject.cpp DidReplay -> DrawIdRecord::SetStaticGate). In such a pass (ParU.x bit 4) the marching
// vote treats a world pixel without a G-buffer id as a STATIC source (kParPseudo, see ParState): the vote's inputs do not depend
// on which static keys were re-drawn this frame.
// kDrawIdCs-BEGIN (the build validates this block with fxc)
const char kDrawIdCs[] = R"(
cbuffer DidCB : register(b0) {
    uint4  Counts;       // x = draws of this pass (n), y = draws of the eye's previous pass (m), z = groups,
                         // w = G-buffer draws of this pass (ids 1..w; v0.10.0 phase 4, CSAttach)
    float4 Params;       // x = max origin displacement (clip units ~ m), y = snap threshold (px), zw = full image (px)
    float4 Params2;      // x = track continuity: displacement change allowed per frame (clip units ~ m)
                         // y = consensus agreement tolerance (px), z = world voters' near filter (m, |w| of the origin)
                         // w = v0.10.0 phase 4 attach radius (m, mv_fwd_attach_m; 0 = off)
    uint4  Cons;         // x = world candidates, y = cabin candidates, z = smallest cluster (draws), w = bit 0 consensus
                         // on, bit 1 the solve buffer holds this frame's medoid (the comparison statistic), bit 2 matrix twins,
                         // v0.10.0 phase 8: bit 3 + L = layer L has NO medoid this frame (dropped: CSPick writes its info rows)
    uint4  Cand[48];     // candidate draw indices: world [0, Cons.x), cabin [Cons.x, Cons.x + Cons.y)
    float4 Par;          // v0.10.0 phase 6: x = vote depth tolerance (relative linear depth), y = smallest winning vote count,
                         // z = smallest winning share, w = vote cap per unpaired draw
    uint4  ParU;         // v0.10.0 phase 6: x = bit 0 rigid parents on, bit 1 harness mutation: the conjugated (row-vector)
                         // formula instead of R_parent, bit 2 (phase 6b) replayed instanced / no-MVP draws take part in the
                         // vote (dlaa.ini mv_drawid_instanced); y = vote grid stride = march step (px); phase 6c: z = march
                         // reach (px), w = march budget per listed draw and pass; v0.10.0 phase 8: x bit 3 = the grid stride
                         // is coarser than 2 px (dlaa.ini mv_vote_res 4): small listed draws vote on the 2 px grid (CSParentVote);
                         // v0.10.0 phase 14: x bit 4 = the static gate skipped draws (id-0 world pixels = a static source)
    float4 Par2;         // v0.10.0 phase 6c: x = edge continuity (relative linear depth between a source pixel and the last
                         // pixel of U's patch before it: an attached part is FLUSH with its parent)
                         // v0.10.0 phase 8: y = a listed draw with fewer counted grid samples is a FINE draw (ParU.x bit 3: it
                         // marches every 2nd pixel with 2 px steps); z / w = march budget of the whole frame in the 1st / 2nd
                         // pass (0 = no frame cap: ParU.w per listed draw and pass, as before)
    uint4  Clip;         // v0.10.0 phase 9 (see the C++ header above): vote grid rect x0, y0 (multiples of 4), x1, y1
    uint4  FwdMode;      //   x = forward depth / ids at 1/2^x resolution (x = 1: the ids in PFwdIds); v0.10.0 phase 18: y = the
                         //   CABIN layer's smallest consensus cluster (dlaa.ini mv_cons_min_cabin; 0 = Cons.z as the world layer)
    uint4  FoldU;        //   x = folded-dispatch bits (pick, inherit, parent list, twins, attach); phase 10: y = listed cap;
                         //   v0.10.0 phase 14: z = asuint(deep-static epsilon, px; mv_replay_static_eps_px); round 4: w bit 0 =
                         //   the previous pass's plate-hold table (UHoldR) is valid; round 6 HARNESS MUTATIONS only: w bit 1 =
                         //   the origin-free rule off, w bit 2 = multi-instance draws listed for the vote again (round-4 rule)
};
ByteAddressBuffer          Mirror : register(t0);   // this pass's copy of the game's VS cbuffer ring(s)
StructuredBuffer<uint4>    Tab    : register(t1);   // [2i] = (mirror byte offset | ~0, group | ~0, flags, rank in group)
                                                    // [2i + 1] = (viewport MinDepth bits, MaxDepth bits, 0, 0)
StructuredBuffer<uint4>    PTab   : register(t2);   // previous draw j: (group | ~0, flags, rank in group, 0)
StructuredBuffer<uint4>    Groups : register(t3);   // (cur list start, cur count, prev list start, prev count)
StructuredBuffer<uint>     Lists  : register(t4);
StructuredBuffer<float4>   Solve  : register(t5);   // pass A: [0..3] R_world rows, [4..7] R_cabin rows
ByteAddressBuffer          PrevM  : register(t6);   // previous pass of this eye: gathered MVPs + tracks (80 B per draw)
RWByteAddressBuffer        CurM   : register(u0);   // this pass: gathered MVPs + tracks (80 B per draw)
RWByteAddressBuffer        Work   : register(u1);   // [i*64] = P_i; [kBestCur + i*16], [kBestPrev + j*16] = matches
RWStructuredBuffer<float4> RTab   : register(u2);
RWByteAddressBuffer        Cnt    : register(u3);
ByteAddressBuffer          MirrorF : register(t7);  // v0.10.0 phase 3: the forward record's ring mirror
RWStructuredBuffer<float4> SolveRW : register(u4);  // v0.10.0 phase 3, CSPick only: CameraMv's solve buffer
Texture2D<uint2>           PIds    : register(t8);  // v0.10.0 phase 6, CSParentVote only: the pass's draw-id target (full size)
Texture2D<float>           PDepth  : register(t9);  //   the scene depth twin (full size)
Texture2D<float>           PFwd    : register(t10); //   the pass's forward depth (unbound = 0: no forward pixel)
RWByteAddressBuffer        ParArgs : register(u5);  // v0.10.0 phase 8, CSParentList only: DispatchIndirect args of the vote
Texture2D<uint>            PFwdIds : register(t11); // v0.10.0 phase 9: the 1/2-resolution forward ids
RWStructuredBuffer<uint4>  UHoldW  : register(u6);  // v0.10.0 phase 14 round 4, CSParentPick2 only: this pass's listed draws
StructuredBuffer<uint4>    UHoldR  : register(t12); //   (IndexCount, centroid, final parent, held age) -- and the previous pass's
// v0.10.0 phase 18 COMPACT VOTE (dlaa.ini mv_vote_compact 1, FoldU.x bit 5): the vote-grid tiles that hold a countable pixel
// (CSParentCount) and the march items of the current vote pass (CSParentCollect -> CSParentMarch). Bytes: [kTileBase..) tiles
// (group x | y << 16), [kItemBase..) items (uint4: x | y << 16, draw | fwd << 31, asuint(depth), slot | step << 16).
// CSParentCollect binds the march args buffer (2 x (groups, 1, 1) at 0 / 16, never this one) at u5 under the name ParArgs.
RWByteAddressBuffer        ParList : register(u7);
static const uint kTileCap = 65536u;                // tiles (beyond: the full vote grid, exactly as before)
static const uint kItemCap = 131072u;               // march items per vote pass (beyond: marched in place, ParMarch)
static const uint kTileBase = 0u;
static const uint kItemBase = kTileCap * 4u;
static const uint kCntTiles = 768u;                 // Cnt dwords: active tiles, march items of pass 1 / 2, items over the cap
static const uint kCntItems = 769u;
static const uint kCntItemOver = 771u;
static const uint kCntTileOver = 772u;              //   passes whose tiles overflowed kTileCap (full grid)

static const uint kNone = 0xFFFFFFFFu;
static const uint kFInst = 1u;
static const uint kFNoMvp = 2u;
static const uint kFCabin = 4u;
static const uint kFLoose = 256u;
static const uint kFFwd = 512u;                     // v0.10.0 phase 3: a forward-pass draw (MVP in MirrorF)
static const uint kMaxDraws = 4096u;
static const uint kBestCur = kMaxDraws * 64u;
static const uint kBestPrev = kMaxDraws * 80u;
static const uint kRd = kMaxDraws * 96u;            // v0.10.0 phase 3: R_draw per paired draw (64 B)
static const uint kPartner = kMaxDraws * 160u;      // partner (previous draw index) per draw, kNone = unpaired
static const uint kVote = kMaxDraws * 164u;         // per candidate: (agreeing draws, deviation sum bits, valid, draw index)
static const uint kAttach = kMaxDraws * 164u + 4096u;   // v0.10.0 phase 4: attached mover index per draw (kNone = none)
static const uint kTwin = kMaxDraws * 168u + 4096u;     // v0.10.0 phase 5: twin (draw index) per draw (kNone = none)
static const uint kHash = kMaxDraws * 172u + 4096u;     // v0.10.0 phase 5: MVP hash table, 2n slots (draw + 1, 0 = empty)
static const uint kMovCnt = kMaxDraws * 180u + 4096u;   // v0.10.0 phase 5: G-buffer movers in the list (16 B)
static const uint kMovList = kMaxDraws * 180u + 4112u;  // v0.10.0 phase 5: G-buffer mover indices (CSAttach candidates)
static const uint kMStride = 80u;                   // CurM / PrevM bytes per draw (MVP 64 + track 16)
static const uint kParSlot = kMaxDraws * 184u + 8192u;  // v0.10.0 phase 6: U slot per draw (kNone = not a listed unpaired draw)
static const uint kParent = kMaxDraws * 188u + 8192u;   // v0.10.0 phase 6: rigid parent (draw index) per draw (kNone = none)
static const uint kParVT = kMaxDraws * 192u + 8192u;    // v0.10.0 phase 6: votes << 16 | total, bit 31 = 2nd pass (dump)
static const uint kParPix = kMaxDraws * 196u + 8192u;   // v0.10.0 phase 6b: on-screen RED-id pixels per instanced / no-MVP draw
static const uint kParU = kMaxDraws * 200u + 8192u;     // v0.10.0 phase 6: [s] = draw of U slot s; [64] U's listed, [65] over
static const uint kParTab = kMaxDraws * 200u + 8704u;   // v0.10.0 phase 6(c): 64 x 192 B vote tables (ParAddVote)
static const uint kParMaxU = 64u;
static const uint kChainBit = 0x80000000u;          // kParVT: won in the 2nd pass (no parent: depth-rejected)
static const uint kParSum = kMaxDraws * 200u + 24576u;  // v0.10.0 phase 14 round 4: per draw (sum x, sum y, n) of its pixels on
                                                        // a 2 px sub-grid (CSParentCount: its centroid -- the plate hold)

// (v0.10.0 phase 14 round 4: one more C++ raw-string split here, MSVC's 16380-byte literal cap): )" R"(
float4x4 LoadM(ByteAddressBuffer b, uint a) {
    return float4x4(asfloat(b.Load4(a)), asfloat(b.Load4(a + 16u)), asfloat(b.Load4(a + 32u)), asfloat(b.Load4(a + 48u)));
}
float4x4 LoadMW(RWByteAddressBuffer b, uint a) {
    return float4x4(asfloat(b.Load4(a)), asfloat(b.Load4(a + 16u)), asfloat(b.Load4(a + 32u)), asfloat(b.Load4(a + 48u)));
}
void StoreM(RWByteAddressBuffer b, uint a, float4x4 m) {
    b.Store4(a, asuint(m[0])); b.Store4(a + 16u, asuint(m[1])); b.Store4(a + 32u, asuint(m[2])); b.Store4(a + 48u, asuint(m[3]));
}
// v0.10.0 phase 14: draw i MOVES this pass (a mover by any route, or a track reject) -> the reset bitmask (Cnt dwords 640..767)
void StatReset(uint i) { uint o; Cnt.InterlockedOr((640u + (i >> 5u)) * 4u, 1u << (i & 31u), o); }
float4x4 LayerR(uint flags) {
    uint b = (flags & kFCabin) != 0u ? 4u : 0u;
    return float4x4(Solve[b], Solve[b + 1u], Solve[b + 2u], Solve[b + 3u]);
}
float4 Col3(float4x4 m) { return float4(m[0][3], m[1][3], m[2][3], m[3][3]); }
bool Finite4(float4x4 m) { return all(isfinite(m[0])) && all(isfinite(m[1])) && all(isfinite(m[2])) && all(isfinite(m[3])); }
// v0.10.0 phase 14 round 6 ORIGIN-FREE MVP (ATS 2026-10-09, the user capture ats_flat_fence_frame1634.rdc: all 79 non-instanced
// roadside grass batches, all 131 instanced grass draws and 37 other draws are drawn CAMERA-RELATIVE (247 of 1554) -- cb0 rows 0..3 = the
// view ROTATION, rows 4..7 = projection * view rotation, column 3 = (~1e-9, ~2e-6, near, ~-1e-7); the camera translation lives in
// the vertices / another cbuffer). Such a draw's matrices carry no position: R = Mp * inverse(Mc) of its own pair is the camera
// ROTATION only. While the camera drove, the draw "moved" against the camera R (the translation's parallax) and became a MOVER with
// that rotation-only R: solid magenta roadside grass in the Ctrl+F6 view, smeared grass under DLSS. FoldU.w bit 1 = HARNESS
// mutation: the rule off.
// Round 7 (first ATS VR run of round 6: "held static 0.0/frame" in every window, 200-220 own-pair movers per frame on a grassy
// roadside): a VR eye's view = T(eye offset) * head view, so a draw relative to the HEAD has column 3 = P * (ex, ey, ez, 1) =
// (p00 ex + p02 ez, p11 ey + p12 ez, p22 ez + p23, w = the origin's view depth in m; column vectors, mul(M, p)). ex = IPD / 2 ~
// 0.032 m gives |x| ~ 0.04-0.05 at p00 1.3-1.5, far over round 6's 1e-4 |near| ~ 1e-5. The off-centre eye projection (p02 / p12
// ~ 0.24 / 0.19) only adds p02 ez ~ 1e-3. Rule: DEPTH part |w| <= kOFreeW and LATERAL part |x|, |y| <= kOFreeXY (|z| > 1e-6: not
// degenerate). kOFreeW 0.03 m: an eye-to-head transform carries a few mm (some headsets ~1-2 cm) of z; round 6's |w| <= 1e-4 |z|
// (1e-5 m) survives no z offset at all and is dropped. kOFreeXY 0.25 clip = 15 cm of lateral offset at p00 1.7 (flat 4K), 17-19 cm
// at the VR eyes' 1.3-1.5, 25 cm at 1.0: any IPD / 2 plus several cm of head offset. A real object whose origin lies within 3 cm
// of the eye plane AND within ~15-25 cm of the eye laterally is only ever a camera-attached draw, which wants the camera R anyway.
// The depth-only / lateral split is read back (Cnt [224..230], OriginDiag) so the next log shows which part held in game.
// Round 7b (ATS flat 17:57 log, round 7 DLL): the camera-relative draws' origin w was 0.028-0.031 m and |x| up to 0.235 -- right
// at the 0.03 m / 0.25 limits, so the grass flipped static <-> mover from frame to frame (origin-free max |w| 0.0296-0.0299,
// min mover |w| 0.0300-0.0307, 30-90 near-origin movers per frame = the smudge stayed). Limits widened to 0.20 m / 0.50 clip:
// a real object's origin within 20 cm of the eye plane AND within ~30-50 cm laterally is still only a camera-attached draw.
static const float kOFreeW = 0.20;    // round 7b: in game the grass sat at |w| 0.028-0.031 m (see below)
static const float kOFreeXY = 0.50;   // round 7b: |x| reached 0.235 in game
bool OriginDepthFree(float4 c) { return abs(c.z) > 1e-6 && abs(c.w) <= kOFreeW; }
bool OriginLatFree(float4 c) { return abs(c.x) <= kOFreeXY && abs(c.y) <= kOFreeXY; }
bool OriginFreeCol(float4 c) {                       // c = MVP column 3
    if ((FoldU.w & 8u) != 0u)                        // FoldU.w bit 3 = HARNESS mutation: the round-6 rule (S15 VR eyes must fail)
        return (FoldU.w & 2u) == 0u && abs(c.z) > 1e-6 && max(abs(c.x), max(abs(c.y), abs(c.w))) <= 1e-4 * abs(c.z);
    return (FoldU.w & 2u) == 0u && OriginDepthFree(c) && OriginLatFree(c);
}
// round 7 diagnostics (not affected by the mutation bits), column 3 c of a PAIRED draw's current MVP: Cnt [224] = the depth part
// held but the lateral part failed, [230] = ~bits of the smallest max(|x|, |y|) among those (the nearest lateral miss; world
// geometry crossing the eye plane lands here too -- a roadside post 7 m to the side has |x| ~ 5 -- so only a miss just over the
// tolerance points at a camera-relative draw); [225..227] = max |x|, |y|, |w| over the draws that pass the whole rule (= the eye
// offset as the game's matrices carry it; float bits: non-negative floats order as uints). Of an own-pair MOVER: [228] = ~bits of
// the smallest |w| (InterlockedMax of the complement = min; 0 = none), [229] = movers with |w| < 0.5 m.
void OriginDiag(float4 c) {
    if (!all(isfinite(c)) || !OriginDepthFree(c)) return;
    uint o;
    if (!OriginLatFree(c)) {
        Cnt.InterlockedAdd(224u * 4u, 1u, o);
        Cnt.InterlockedMax(230u * 4u, ~asuint(max(abs(c.x), abs(c.y))), o);
        return;
    }
    Cnt.InterlockedMax(225u * 4u, asuint(abs(c.x)), o);
    Cnt.InterlockedMax(226u * 4u, asuint(abs(c.y)), o);
    Cnt.InterlockedMax(227u * 4u, asuint(abs(c.w)), o);
}
void OriginMoverDiag(float4 c) {
    if (!all(isfinite(c))) return;
    uint o;
    Cnt.InterlockedMax(228u * 4u, ~asuint(abs(c.w)), o);
    if (abs(c.w) < 0.5) Cnt.InterlockedAdd(229u * 4u, 1u, o);
}
bool OriginFree(float4x4 m) { return OriginFreeCol(Col3(m)); }
uint CandAt(uint c) {
    uint4 v = Cand[c >> 2];
    uint k = c & 3u;
    return k == 0u ? v.x : (k == 1u ? v.y : (k == 2u ? v.z : v.w));
}
float Frob(float4x4 a, float4x4 b) {
    float d = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) { float4 r = a[i] - b[i]; d += dot(r, r); }
    return sqrt(d);
}
// lexicographic (origin distance, matrix distance, rank distance) with a relative tie band on the two distances; the first
// valid candidate always wins (best == kNone)
bool Better(float d0, float dF, uint rk, uint best, float b0, float bF, uint brk) {
    if (best == kNone) return true;
    float t0 = 1e-5 * (1.0 + b0);
    if (d0 < b0 - t0) return true;
    if (d0 > b0 + t0) return false;
    float tF = 1e-5 * (1.0 + bF);
    if (dF < bF - tF) return true;
    if (dF > bF + tF) return false;
    return rk < brk;
}

bool Inv4(float4x4 mm, out float4x4 r) {
    float m[16];
    float inv[16];
    [unroll] for (int i = 0; i < 4; ++i)
        [unroll] for (int j = 0; j < 4; ++j)
            m[i * 4 + j] = mm[i][j];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    r = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    if (!(abs(det) > 1e-30) || !isfinite(det)) return false;
    float id = 1.0 / det;
    [unroll] for (int a = 0; a < 4; ++a)
        [unroll] for (int b = 0; b < 4; ++b)
            r[a][b] = inv[a * 4 + b] * id;
    return true;
}

[numthreads(64, 1, 1)]
void CSGather(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= Counts.x) return;
    uint4 e = Tab[i * 2u];
    float4x4 Mc = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    if ((e.z & (kFInst | kFNoMvp)) == 0u && e.x != kNone) {
        if ((e.z & kFFwd) != 0u) Mc = LoadM(MirrorF, e.x);
        else Mc = LoadM(Mirror, e.x);
    }
    StoreM(CurM, i * kMStride, Mc);
    CurM.Store4(i * kMStride + 64u, uint4(0u, 0u, 0u, 0u));   // track: none (CSResolve writes it for a mutual pair)
    StoreM(Work, i * 64u, mul(LayerR(e.z), Mc));
    // v0.10.0 phase 14 round 6: the instance count of each instanced draw AS THE SHADERS SEE IT (Tab[2i + 1].z >> 24, CPU-capped at
    // 255): Cnt [215] = their sum, [216 + min(n, 7)] = instanced draws with n instances (the harness checks CPU record -> Tab -> GPU)
    if ((e.z & kFInst) != 0u) {
        uint ni = Tab[i * 2u + 1u].z >> 24, oi;
        Cnt.InterlockedAdd(215u * 4u, ni, oi);
        Cnt.InterlockedAdd((216u + min(ni, 7u)) * 4u, 1u, oi);
    }
    // v0.10.0 phase 5: no attach / twin yet (also read by the dump when those passes do not run); the hash slots 2i, 2i + 1
    // of the 2n-slot table; the mover list starts empty
    Work.Store(kAttach + i * 4u, kNone);
    Work.Store(kTwin + i * 4u, kNone);
    Work.Store2(kHash + i * 8u, uint2(0u, 0u));
    if (i == 0u) Work.Store(kMovCnt, 0u);
    // v0.10.0 phase 6: not a listed unpaired draw, no rigid parent (read by the vote and the dump when the passes do not run)
    Work.Store(kParSlot + i * 4u, kNone);
    Work.Store(kParent + i * 4u, kNone);
    Work.Store(kParVT + i * 4u, 0u);
    Work.Store(kParPix + i * 4u, 0u);                    // v0.10.0 phase 6b: on-screen pixel count (CSParentCount fills it)
    Work.Store3(kParSum + i * 12u, uint3(0u, 0u, 0u));   // v0.10.0 phase 14 round 4: + the pixel position sums (centroid)
    if (i == 0u) Work.Store2(kParU + 256u, uint2(0u, 0u));
}

[numthreads(64, 1, 1)]
void CSMatch(uint3 tid : SV_DispatchThreadID) {
    uint t = tid.x;
    uint n = Counts.x, m = Counts.y;
    if (t < n) {
        uint4 e = Tab[t * 2u];
        uint best = kNone, brk = kNone;
        float b0 = 0.0, bF = 0.0;
        if (e.y != kNone && (e.z & (kFInst | kFNoMvp)) == 0u) {
            uint4 g = Groups[e.y];
            float4x4 P = LoadMW(Work, t * 64u);
            float4 pc = Col3(P);
            for (uint k = 0u; k < g.w; ++k) {
                uint j = Lists[g.z + k];
                float4x4 Mp = LoadM(PrevM, j * kMStride);
                float d0 = length(Col3(Mp) - pc);
                if (!isfinite(d0) || (best != kNone && !(d0 <= b0 + 1e-5 * (1.0 + b0)))) continue;
                float dF = Frob(Mp, P);
                uint rk = (uint)abs((int)k - (int)e.w);
                if (Better(d0, dF, rk, best, b0, bF, brk)) { best = j; b0 = d0; bF = dF; brk = rk; }
            }
        }
        Work.Store4(kBestCur + t * 16u, uint4(best, asuint(b0), asuint(bF), brk));
    } else if (t < n + m) {
        uint j = t - n;
        uint4 p = PTab[j];
        uint best = kNone, brk = kNone;
        float b0 = 0.0, bF = 0.0;
        if (p.x != kNone) {
            uint4 g = Groups[p.x];
            float4x4 Mp = LoadM(PrevM, j * kMStride);
            float4 mc = Col3(Mp);
            for (uint k = 0u; k < g.y; ++k) {
                uint i = Lists[g.x + k];
                float4x4 P = LoadMW(Work, i * 64u);
                float d0 = length(Col3(P) - mc);
                if (!isfinite(d0) || (best != kNone && !(d0 <= b0 + 1e-5 * (1.0 + b0)))) continue;
                float dF = Frob(Mp, P);
                uint rk = (uint)abs((int)k - (int)p.z);
                if (Better(d0, dF, rk, best, b0, bF, brk)) { best = i; b0 = d0; bF = dF; brk = rk; }
            }
        }
        Work.Store4(kBestPrev + j * 16u, uint4(best, asuint(b0), asuint(bF), brk));
    }
}

// (v0.10.0 phase 9: one more C++ raw-string split here, MSVC's 16380-byte literal cap): )" R"(
// largest screen distance (px) between the true previous position Mp * p and the camera prediction P * p over the probe
// points. Only an ON-SCREEN probe (|ndc| <= 1.5, w >= 0.05) is compared in screen space; a probe off-screen, at or behind
// the camera plane compares clip x / y divided by max(w, 1): a perspective divide by a tiny w (a probe beside the camera)
// would magnify float noise of the camera solve into "motion" there, while a real mover still shows up in clip units.
float ProbeDev(float4x4 Mp, float4x4 P) {
    // origin, +-4 on the local x / z axes, +4 on y: about a vehicle's half length (a trailer's far end must not hide a
    // motion the origin alone would not show); a larger lever only over-estimates -> "mover" -> its own exact R (safe)
    float4 probes[6] = { float4(0, 0, 0, 1), float4(4, 0, 0, 1), float4(-4, 0, 0, 1),
                         float4(0, 0, 4, 1), float4(0, 0, -4, 1), float4(0, 4, 0, 1) };
    float dev = 0.0;
    [unroll] for (int k = 0; k < 6; ++k) {
        float4 a = mul(Mp, probes[k]);
        float4 b = mul(P, probes[k]);
        float d;
        bool onScreen = a.w > 0.05 && b.w > 0.05;
        float2 na = a.xy / max(a.w, 1e-6), nb = b.xy / max(b.w, 1e-6);
        onScreen = onScreen && all(abs(na) <= 1.5) && all(abs(nb) <= 1.5);
        if (onScreen) d = length((na - nb) * Params.zw * 0.5);
        else d = length((a.xy - b.xy) / max(max(abs(a.w), abs(b.w)), 1.0) * Params.zw * 0.5);
        dev = max(dev, isfinite(d) ? d : 1e9);
    }
    return dev;
}

// v0.10.0 phase 3: the source is split into two C++ raw strings here (MSVC caps one literal at 16380 bytes): )" R"(
// v0.10.0 phase 3: the mutual pair of each draw and its R_draw (kept in Work for the consensus and CSResolve)
[numthreads(64, 1, 1)]
void CSPair(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= Counts.x) return;
    uint4 e = Tab[i * 2u];
    uint partner = kNone;
    if ((e.z & (kFInst | kFNoMvp)) == 0u && e.x != kNone && e.y != kNone && Counts.y != 0u) {
        uint4 b = Work.Load4(kBestCur + i * 16u);
        if (b.x != kNone && b.x < Counts.y && Work.Load(kBestPrev + b.x * 16u) == i && asfloat(b.y) < Params.x) {
            float4x4 Mc = LoadMW(CurM, i * kMStride);
            float4x4 Mp = LoadM(PrevM, b.x * kMStride);
            float4x4 Ic;
            if (Inv4(Mc, Ic)) {
                float4x4 Rd = mul(Mp, Ic);
                if (Finite4(Rd)) {
                    partner = b.x;
                    StoreM(Work, kRd + i * 64u, Rd);
                    uint old;
                    if ((e.z & kFFwd) == 0u) {
                        if ((e.z & kFCabin) != 0u) Cnt.InterlockedAdd(18u * 4u, 1u, old);
                        else {
                            Cnt.InterlockedAdd(16u * 4u, 1u, old);
                            if (abs(Mc[3][3]) >= Params2.z) Cnt.InterlockedAdd(17u * 4u, 1u, old);
                        }
                    }
                }
            }
        }
    }
    Work.Store(kPartner + i * 4u, partner);
}

// consensus voter / candidate class of draw i: 0 world, 1 cabin, 2 none (unpaired, instanced, no MVP, forward, or a world
// draw nearer than the near filter while enough far ones exist)
uint VoterLayer(uint i, bool useFar, out uint partner) {
    uint4 e = Tab[i * 2u];
    partner = Work.Load(kPartner + i * 4u);
    if (partner == kNone || (e.z & (kFInst | kFNoMvp | kFFwd)) != 0u) return 2u;
    bool cab = (e.z & kFCabin) != 0u;
    if (!cab && useFar) return abs(asfloat(CurM.Load(i * kMStride + 60u))) >= Params2.z ? 0u : 2u;   // |MVP row 3 .w| = origin w
    // v0.10.0 phase 14 round 6: an origin-free (camera-relative) draw's own R is the camera rotation only -- never a voter or a
    // candidate (with the far filter on, its origin w ~ 0 already fails it above)
    uint b = i * kMStride;
    if (OriginFreeCol(asfloat(uint4(CurM.Load(b + 12u), CurM.Load(b + 28u), CurM.Load(b + 44u), CurM.Load(b + 60u))))) return 2u;
    return cab ? 1u : 0u;
}

groupshared uint  gVoteN[64];
groupshared float gVoteD[64];
// one group per candidate: the paired draws of its layer whose probes agree with the candidate's R within Params2.y px
[numthreads(64, 1, 1)]
void CSVote(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    uint c = gid.x;
    uint want = c < Cons.x ? 0u : 1u;
    bool useFar = Cnt.Load(17u * 4u) >= Cons.z;
    uint ci = c < Cons.x + Cons.y ? CandAt(c) : kNone;
    uint pc = kNone;
    bool ok = false;
    if (ci < Counts.x) ok = VoterLayer(ci, useFar, pc) == want;
    float4x4 Rc = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    if (ok) Rc = LoadMW(Work, kRd + ci * 64u);
    uint cnt = 0u;
    float sdev = 0.0;
    if (ok) {
        for (uint i = gi; i < Counts.x; i += 64u) {
            uint p;
            if (VoterLayer(i, useFar, p) != want) continue;
            float4x4 Mc = LoadMW(CurM, i * kMStride);
            float4x4 Mp = LoadM(PrevM, p * kMStride);
            float dv = ProbeDev(Mp, mul(Rc, Mc));
            if (dv < Params2.y) { ++cnt; sdev += dv; }
        }
    }
    gVoteN[gi] = cnt;
    gVoteD[gi] = sdev;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 32u; s > 0u; s >>= 1u) {
        if (gi < s) { gVoteN[gi] += gVoteN[gi + s]; gVoteD[gi] += gVoteD[gi + s]; }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0u) Work.Store4(kVote + c * 16u, uint4(gVoteN[0], asuint(gVoteD[0]), ok ? 1u : 0u, ci));
}

// thread 0 = world, 1 = cabin: the winning cluster's R replaces the medoid in the solve buffer (>= Cons.z draws)
// v0.10.0 phase 18: split into PickHead (the choice) / PickDm (the statistic per candidate) / PickTail (the writes), so CSPickPar
// can spread the statistic's loop (64 candidates x 2 probe tests, one thread before) over a group -- same values, same order
// returns (the winning candidate -- kNone = the layer keeps its medoid, Cnt has why --, its cluster size, small = below Cons.z)
uint3 PickHead(uint L) {
    uint c0 = L == 0u ? 0u : Cons.x;
    uint c1 = L == 0u ? Cons.x : Cons.x + Cons.y;
    uint best = kNone, nValid = 0u, bc = 0u;
    float bdev = 0.0;
    for (uint c = c0; c < c1; ++c) {
        uint4 v = Work.Load4(kVote + c * 16u);
        if (v.z == 0u) continue;
        ++nValid;
        float md = v.x != 0u ? asfloat(v.y) / (float)v.x : 0.0;
        if (best == kNone || v.x > bc || (v.x == bc && md < bdev)) { best = c; bc = v.x; bdev = md; }
    }
    uint o = (L == 0u ? 20u : 28u) * 4u;
    Cnt.Store(o + 4u, bc);
    Cnt.Store(o + 16u, nValid);
    if (L == 0u) Cnt.Store(o + 24u, Cnt.Load(17u * 4u) >= Cons.z ? 0u : 1u);   // 1 = too few far draws: near ones voted
    uint3 r = uint3(kNone, bc, 0u);                      // (one return: fxc's X4000 false positive on early returns)
    if (best == kNone) Cnt.Store(o, 0u);
    else if ((Cons.w & 1u) == 0u) Cnt.Store(o, 3u);
    else {
        // v0.10.0 phase 8: a layer WITHOUT a medoid this frame (dropped) takes a smaller cluster (>= 8 draws) rather than keeping
        // the last frame's R; it is still a failed consensus (state 4: the medoid re-arms)
        bool small = bc < ((L == 1u && FwdMode.y != 0u) ? FwdMode.y : Cons.z);   // (phase 18: mv_cons_min_cabin)
        if (small && ((Cons.w & (8u << L)) == 0u || bc < 8u)) { Cnt.Store(o, 2u); r.z = 1u; }
        else r = uint3(best, bc, small ? 1u : 0u);
    }
    return r;
}
// statistic: how far the medoid is from the consensus at the cluster's candidate draws (only with a fresh medoid)
bool PickStatOn(uint L) { return (Cons.w & 2u) != 0u && (Cons.w & (8u << L)) == 0u; }   // (phase 8: not without a medoid)
float PickDm(uint c2, float4x4 Rw, float4x4 Rm) {      // candidate c2's |medoid - consensus| px (-1: not in the winning cluster)
    uint4 v = Work.Load4(kVote + c2 * 16u);
    if (v.z == 0u) return -1.0;
    uint pj = Work.Load(kPartner + v.w * 4u);
    float4x4 Mc = LoadMW(CurM, v.w * kMStride);
    float4x4 Mp = LoadM(PrevM, pj * kMStride);
    if (!(ProbeDev(Mp, mul(Rw, Mc)) < Params2.y)) return -1.0;       // not a member of the winning cluster
    return min(ProbeDev(Mp, mul(Rm, Mc)), 1e6);
}
void PickTail(uint L, uint bc, bool small, uint ci, float4x4 Rw, float dsum, float dmax, uint dn);
void PickLayer(uint L) {
    uint3 h = PickHead(L);
    uint best = h.x, bc = h.y;
    bool small = h.z != 0u;
    if (best == kNone) return;
    uint c0 = L == 0u ? 0u : Cons.x;
    uint c1 = L == 0u ? Cons.x : Cons.x + Cons.y;
    uint ci = Work.Load(kVote + best * 16u + 12u);
    float4x4 Rw = LoadMW(Work, kRd + ci * 64u);
    uint base = L * 4u;
    float4x4 Rm = float4x4(SolveRW[base], SolveRW[base + 1u], SolveRW[base + 2u], SolveRW[base + 3u]);
    float dsum = 0.0, dmax = 0.0;
    uint dn = 0u;
    if (PickStatOn(L)) {
        for (uint c2 = c0; c2 < c1; ++c2) {
            float dm = PickDm(c2, Rw, Rm);
            if (dm < 0.0) continue;
            dsum += dm; dmax = max(dmax, dm); ++dn;
        }
    }
    PickTail(L, bc, small, ci, Rw, dsum, dmax, dn);
}
void PickTail(uint L, uint bc, bool small, uint ci, float4x4 Rw, float dsum, float dmax, uint dn) {
    uint o = (L == 0u ? 20u : 28u) * 4u;
    uint base = L * 4u;
    [unroll] for (uint r = 0u; r < 4u; ++r) SolveRW[base + r] = Rw[r];
    float4 inf = SolveRW[8u + L];
    inf.w = (float)bc;                                                   // the camera R is a consensus of bc draws
    SolveRW[8u + L] = inf;
    // v0.10.0 phase 8: no medoid of this layer this frame (its candidates were dropped: pass A wrote nothing) -> the info rows pass
    // A would write: no pair (z = -1), and for the world layer no ego split (R_ego = the camera R) and InvRow3 = row 3 of the
    // inverse MVP of the winning draw (= row 3 of the inverse projection for any rigid draw: pass B's view distance)
    if ((Cons.w & (8u << L)) != 0u) {
        SolveRW[8u + L] = float4(0.0, 0.0, -1.0, (float)bc);
        SolveRW[10u + L] = float4(0.0, 0.0, 0.0, 0.0);
        if (L == 0u) {
            [unroll] for (uint r2 = 0u; r2 < 4u; ++r2) SolveRW[12u + r2] = Rw[r2];
            float4x4 Mw = LoadMW(CurM, ci * kMStride), iw;
            if (Inv4(Mw, iw) && all(isfinite(iw[3]))) SolveRW[16u] = iw[3];
            SolveRW[17u] = float4(0.0, 0.0, 1.0, 0.0);
        }
    }
    Cnt.Store(o, small ? 4u : 1u);
    Cnt.Store(o + 8u, asuint(dn != 0u ? dsum / (float)dn : 0.0));
    Cnt.Store(o + 12u, asuint(dmax));
    Cnt.Store(o + 20u, dn != 0u ? 1u : 0u);
}
[numthreads(2, 1, 1)]
void CSPick(uint3 tid : SV_DispatchThreadID) {
    if (tid.x < 2u) PickLayer(tid.x);
}
// (v0.10.0 phase 18: one more C++ raw-string split here, MSVC's literal cap): )" R"(
// v0.10.0 phase 18 (mv_fold_dispatch 2): the same pick with the statistic's candidate loop spread over the group -- threads
// 0..127 the world layer (<= kCandWorld candidates), 128..255 the cabin layer; thread 0 of each sums in candidate order (the
// serial loop's order: identical values) and writes. CSPickResolve ran it on ONE thread inside a 256-thread group (in game:
// pick + resolve + inherit 0.15-0.18 ms per eye, ~2200 draws resolved 9 per thread in one group)
groupshared uint  gPkBest[2], gPkBc[2], gPkSmall[2];
groupshared float gPkDm[256];
[numthreads(256, 1, 1)]
void CSPickPar(uint gi : SV_GroupIndex) {
    uint L = gi >> 7u, t = gi & 127u;
    if (t == 0u) {
        uint3 h = PickHead(L);
        gPkBest[L] = h.x; gPkBc[L] = h.y; gPkSmall[L] = h.z;
    }
    GroupMemoryBarrierWithGroupSync();
    uint best = gPkBest[L];
    uint c0 = L == 0u ? 0u : Cons.x;
    uint c1 = L == 0u ? Cons.x : Cons.x + Cons.y;
    uint ci = 0u;
    float4x4 Rw = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    float dm = -1.0;
    if (best != kNone) {
        ci = Work.Load(kVote + best * 16u + 12u);
        Rw = LoadMW(Work, kRd + ci * 64u);
        if (PickStatOn(L) && c0 + t < c1) {
            uint base = L * 4u;
            float4x4 Rm = float4x4(SolveRW[base], SolveRW[base + 1u], SolveRW[base + 2u], SolveRW[base + 3u]);
            dm = PickDm(c0 + t, Rw, Rm);
        }
    }
    gPkDm[gi] = dm;
    GroupMemoryBarrierWithGroupSync();          // every Rm read above happens before PickTail overwrites the layer's rows
    if (t == 0u && best != kNone) {
        float dsum = 0.0, dmax = 0.0;
        uint dn = 0u;
        for (uint k = 0u; k < c1 - c0; ++k) {
            float d = gPkDm[L * 128u + k];
            if (d < 0.0) continue;
            dsum += d; dmax = max(dmax, d); ++dn;
        }
        PickTail(L, gPkBc[L], gPkSmall[L] != 0u, ci, Rw, dsum, dmax, dn);
    }
}

// R = the layer's FINAL camera R (v0.10.0 phase 9: from Solve, or from SolveRW in CSPickResolve)
void ResolveDraw(uint i, float4x4 R) {
    uint4 e = Tab[i * 2u];
    uint4 e1 = Tab[i * 2u + 1u];
    uint state = 1u;
    float dev = 0.0;
    uint old0;
    bool oFree = false;                                 // v0.10.0 phase 14 round 6 (OriginFree): paired, but camera-relative
    if ((e.z & kFInst) != 0u) state = 4u;
    else if ((e.z & kFNoMvp) != 0u || e.x == kNone) state = 5u;
    else {
        uint p = Work.Load(kPartner + i * 4u);
        bool paired = p != kNone && p < Counts.y;
        if (paired) OriginDiag(Col3(LoadMW(CurM, i * kMStride)));   // round 7: the depth-only / lateral split (diagnostics)
        if (paired && (OriginFree(LoadMW(CurM, i * kMStride)) || OriginFree(LoadM(PrevM, p * kMStride)))) {
            // round 6: a camera-relative draw (roadside grass / terrain batches) -- its own R is the camera ROTATION only (see
            // OriginFree). It is world (or cabin) geometry drawn around the camera: STATIC, the layer's camera R. Never a mover,
            // never a skip streak (StatReset below), never a consensus voter (VoterLayer). Cnt [214] counts them.
            oFree = true;
            state = 2u;
        } else if (paired) {
            float4x4 Mc = LoadMW(CurM, i * kMStride);
            float4x4 Mp = LoadM(PrevM, p * kMStride);
            float4x4 P = mul(R, Mc);                    // where the camera alone puts this draw in the previous frame
            // track of this pair (read by the next frame's continuity check): the origin's displacement against the camera
            float4 disp4 = Col3(Mp) - Col3(P);
            float3 disp = float3(disp4.x, disp4.y, disp4.w);
            CurM.Store4(i * kMStride + 64u, uint4(asuint(disp.x), asuint(disp.y), asuint(disp.z), 1u));
            dev = ProbeDev(Mp, P);
            if (dev < Params.y) state = 2u;
            else {
                // ambiguous group (several draws of the key on a side, or the pointer-free key): the previous draw must
                // carry a consistent track of its own (see the header)
                uint4 g = Groups[e.y];
                bool ambiguous = g.y > 1u || g.w > 1u || (e.z & kFLoose) != 0u;
                bool ok = true;
                if (ambiguous) {
                    uint4 pt = PrevM.Load4(p * kMStride + 64u);
                    float3 pd = float3(asfloat(pt.x), asfloat(pt.y), asfloat(pt.z));
                    ok = pt.w == 1u && length(disp - pd) <= Params2.x + 0.25 * length(pd);
                }
                if (ok) { state = 3u; R = LoadMW(Work, kRd + i * 64u); OriginMoverDiag(Col3(Mc)); }
                else { state = 1u; Cnt.InterlockedAdd(36u, 1u, old0); StatReset(i); }   // phase 14: a track reject moves
            }
        }
    }
    uint o = i * 5u;
    RTab[o] = R[0]; RTab[o + 1u] = R[1]; RTab[o + 2u] = R[2]; RTab[o + 3u] = R[3];
    RTab[o + 4u] = float4((float)state, asfloat(e1.x), asfloat(e1.y), dev);
    uint old;
    Cnt.InterlockedAdd(state * 4u, 1u, old);
    // v0.10.0 phase 14: own-pair static G-buffer draw -> the static bitmask (dwords 384..511); a mover -> the reset bitmask.
    // Only a DEEP static draw (dev below FoldU.z px, mv_replay_static_eps_px: world geometry, whose matrices move exactly with the
    // camera) may build a skip streak; a "static" draw with any measurable motion of its own (dev between the epsilon and the snap:
    // a vehicle driving at the player's speed -- 2026-10-09 round 3, its plate flickered with the re-check cadence) counts as
    // MOVING for the gate (it is never skipped) while its R stays the camera R as before. Cnt [210] = such shallow-static draws.
    if (oFree) { StatReset(i); Cnt.InterlockedAdd(214u * 4u, 1u, old); }   // round 6: origin-free (static, never skipped)
    else if (state == 2u && (e.z & kFFwd) == 0u) {
        if (dev < asfloat(FoldU.z)) Cnt.InterlockedOr((384u + (i >> 5u)) * 4u, 1u << (i & 31u), old);
        else { StatReset(i); Cnt.InterlockedAdd(210u * 4u, 1u, old); }
    }
    if (state == 3u) StatReset(i);
    if ((e.z & kFLoose) != 0u && (state == 2u || state == 3u)) Cnt.InterlockedAdd(32u, 1u, old);
    if ((e.z & kFFwd) != 0u && (state == 2u || state == 3u)) {
        Cnt.InterlockedAdd(40u, 1u, old);
        if (state == 3u) Cnt.InterlockedAdd(44u, 1u, old);
    }
}
[numthreads(64, 1, 1)]
void CSResolve(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= Counts.x) return;
    ResolveDraw(i, LayerR(Tab[i * 2u].z));               // the layer's FINAL camera R (consensus, else the medoid)
}
void Inherit1Draw(uint i);
// v0.10.0 phase 18 (mv_fold_dispatch 2): resolve + inherit per draw in ONE wide dispatch (Inherit1Draw reads only its own draw's
// state, which this thread has just written; the twin table / mover list it fills are read by later dispatches only)
[numthreads(64, 1, 1)]
void CSResolveInherit(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= Counts.x) return;
    ResolveDraw(i, LayerR(Tab[i * 2u].z));
    if ((FoldU.x & 2u) != 0u) Inherit1Draw(i);
}

// v0.10.0 phase 5 (the source is split into a third C++ raw string here, MSVC's literal cap): )" R"(
// the 16 MVP words as compared bit-wise (-0 == +0)
uint WordOf(float f) { return f == 0.0 ? 0u : asuint(f); }
uint HashM(float4x4 m) {
    uint h = 2166136261u;
    [unroll] for (uint r = 0u; r < 4u; ++r)
        [unroll] for (uint c = 0u; c < 4u; ++c) { h ^= WordOf(m[r][c]); h *= 16777619u; }
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
bool SameM(float4x4 a, float4x4 b) {
    bool same = true;
    [unroll] for (uint r = 0u; r < 4u; ++r)
        [unroll] for (uint c = 0u; c < 4u; ++c) same = same && WordOf(a[r][c]) == WordOf(b[r][c]);
    return same;
}
// the projected rotation / scale (MVP columns 0..2) of two draws agree within 1e-3 of the column length
bool SameAxes(float4x4 a, float4x4 b) {
    bool same = true;
    [unroll] for (uint c = 0u; c < 3u; ++c) {
        float4 ca = float4(a[0][c], a[1][c], a[2][c], a[3][c]), cb = float4(b[0][c], b[1][c], b[2][c], b[3][c]);
        same = same && length(ca - cb) <= 1e-3 * max(length(cb), 1e-20);
    }
    return same;
}

// v0.10.0 phase 4: a forward draw (plate, decal, window) whose origin lies on a G-buffer mover's origin takes that mover's R
// (see the header). Runs after CSResolve: the G-buffer states and R's in RTab are final. Phase 5: + G-buffer draws still at
// state 1 without a twin (after CSTwin); candidates = the mover list of CSInherit1; the attach column is cleared by CSGather.
void AttachDraw(uint i) {
    uint4 e = Tab[i * 2u];
    if ((e.z & (kFInst | kFNoMvp)) != 0u || e.x == kNone || !(Params2.w > 0.0)) return;
    if (Work.Load(kTwin + i * 4u) != kNone) return;              // phase 5: the matrix twin is exact
    bool fwd = (e.z & kFFwd) != 0u;
    uint o = i * 5u;
    float4 own = RTab[o + 4u];
    uint ownSt = (uint)(own.x + 0.5);
    if (!fwd && ownSt != 1u) return;                             // phase 5: G-buffer draws only without a usable pair
    float4x4 Mc = LoadMW(CurM, i * kMStride);
    float4 oi = Col3(Mc);
    if (!(oi.w > 0.05) || !all(isfinite(oi))) return;
    uint best = kNone;
    float bd = Params2.w;
    uint nMov = min(Work.Load(kMovCnt), Counts.x);
    for (uint q = 0u; q < nMov; ++q) {
        uint j = Work.Load(kMovList + q * 4u);                   // a G-buffer MOVER by its own pair (CSInherit1)
        uint4 ej = Tab[j * 2u];
        if (((ej.z ^ e.z) & kFCabin) != 0u) continue;
        float4x4 Mj = LoadMW(CurM, j * kMStride);
        float4 oj = Col3(Mj);
        if (!(oj.w > 0.05)) continue;
        float n3 = max(length(Mj[3].xyz), 1e-6);
        float sx = max(length(Mj[0].xyz) / n3, 1e-6), sy = max(length(Mj[1].xyz) / n3, 1e-6);
        float dist = length(float3((oi.x - oj.x) / sx, (oi.y - oj.y) / sy, oi.w - oj.w));
        // nearest; equal distances -> the lower index (the list order is not deterministic)
        if (best == kNone ? !(dist < bd) : (dist > bd || (dist == bd && j > best))) continue;
        if (!fwd && !SameAxes(Mc, Mj)) continue;                 // phase 5: a G-buffer draw must share the mover's orientation
        bd = dist; best = j;
    }
    if (best == kNone) return;
    uint ob = best * 5u;
    float4x4 Rj = float4x4(RTab[ob], RTab[ob + 1u], RTab[ob + 2u], RTab[ob + 3u]);
    uint old;
    if (fwd && ownSt == 3u) {                                    // its own pair says "mover": kept when it agrees
        float4x4 Ri = float4x4(RTab[o], RTab[o + 1u], RTab[o + 2u], RTab[o + 3u]);
        if (ProbeDev(mul(Ri, Mc), mul(Rj, Mc)) < 0.5) { Cnt.InterlockedAdd(13u * 4u, 1u, old); return; }
    }
    RTab[o] = Rj[0]; RTab[o + 1u] = Rj[1]; RTab[o + 2u] = Rj[2]; RTab[o + 3u] = Rj[3];
    RTab[o + 4u] = float4(3.0, own.y, own.z, -1.0 - (float)best);
    Work.Store(kAttach + i * 4u, best);
    StatReset(i);                                                // v0.10.0 phase 14: it moves (a mover's R)
    Cnt.InterlockedAdd((fwd ? 12u : 50u) * 4u, 1u, old);
}
[numthreads(64, 1, 1)]
void CSAttach(uint3 tid : SV_DispatchThreadID) {
    if (tid.x < Counts.x) AttachDraw(tid.x);
}

// phase 5: after CSResolve -- own-pair static / mover draws into the twin table, G-buffer movers into the mover list
void Inherit1Draw(uint i) {
    uint4 e = Tab[i * 2u];
    if ((e.z & (kFInst | kFNoMvp)) != 0u || e.x == kNone) return;
    uint st = (uint)(RTab[i * 5u + 4u].x + 0.5);
    if (st != 2u && st != 3u) return;
    if (st == 3u && (e.z & kFFwd) == 0u) {
        uint k;
        Work.InterlockedAdd(kMovCnt, 1u, k);
        if (k < kMaxDraws) Work.Store(kMovList + k * 4u, i);
    }
    if ((Cons.w & 4u) == 0u) return;
    uint T = Counts.x * 2u;
    uint h = HashM(LoadMW(CurM, i * kMStride));
    for (uint k2 = 0u; k2 < T; ++k2) {
        uint orig;
        Work.InterlockedCompareExchange(kHash + ((h + k2) % T) * 4u, 0u, i + 1u, orig);
        if (orig == 0u) break;
    }
}
[numthreads(64, 1, 1)]
void CSInherit1(uint3 tid : SV_DispatchThreadID) {
    if (tid.x < Counts.x) Inherit1Draw(tid.x);
}

// phase 5: a draw still at state 1 takes the R and state of the draw that carries the SAME MVP (see the header)
void TwinDraw(uint i) {
    uint4 e = Tab[i * 2u];
    if ((e.z & (kFInst | kFNoMvp)) != 0u || e.x == kNone) return;
    uint o = i * 5u;
    float4 own = RTab[o + 4u];
    if ((uint)(own.x + 0.5) != 1u) return;
    uint old;
    Cnt.InterlockedAdd(52u * 4u, 1u, old);
    float4x4 M = LoadMW(CurM, i * kMStride);
    if (!Finite4(M)) return;
    // v0.10.0 phase 6: an MVP whose column 3 is exactly (0, 0, c, 0) is the bare PROJECTION (view-space vertices written by the
    // CPU every frame -- the in-game plates): every such draw carries the same "matrix", a twin would be arbitrary
    if (M[0][3] == 0.0 && M[1][3] == 0.0 && M[3][3] == 0.0) { Cnt.InterlockedAdd(63u * 4u, 1u, old); return; }
    uint T = Counts.x * 2u;
    uint h = HashM(M);
    uint best = kNone;
    bool bestFwd = true;
    for (uint k = 0u; k < T; ++k) {
        uint v = Work.Load(kHash + ((h + k) % T) * 4u);
        if (v == 0u) break;                                      // end of the probe chain
        uint j = v - 1u;
        if (j == i || j >= Counts.x) continue;
        uint4 ej = Tab[j * 2u];
        if (((ej.z ^ e.z) & kFCabin) != 0u) continue;
        if (!SameM(M, LoadMW(CurM, j * kMStride))) continue;
        bool jf = (ej.z & kFFwd) != 0u;
        if (best == kNone || (bestFwd && !jf) || (bestFwd == jf && j < best)) { best = j; bestFwd = jf; }
    }
    if (best == kNone) return;
    uint ob = best * 5u;
    float4 inf = RTab[ob + 4u];
    RTab[o] = RTab[ob]; RTab[o + 1u] = RTab[ob + 1u]; RTab[o + 2u] = RTab[ob + 2u]; RTab[o + 3u] = RTab[ob + 3u];
    RTab[o + 4u] = float4(inf.x, own.y, own.z, -1.0 - (float)kMaxDraws - (float)best);
    Work.Store(kTwin + i * 4u, best);
    Cnt.InterlockedAdd(48u * 4u, 1u, old);
    if ((uint)(inf.x + 0.5) == 3u) { Cnt.InterlockedAdd(49u * 4u, 1u, old); StatReset(i); }   // phase 14: a moving twin
    if ((e.z & kFFwd) != 0u) Cnt.InterlockedAdd(53u * 4u, 1u, old);
}
[numthreads(64, 1, 1)]
void CSTwin(uint3 tid : SV_DispatchThreadID) {
    if (tid.x < Counts.x) TwinDraw(tid.x);
}

// v0.10.0 phase 6 RIGID PARENTS (the source is split into a fourth C++ raw string here, MSVC's literal cap): )" R"(
// U = a draw left at state 1 by its own pairing (any IndexCount, G-buffer or forward). Column-vector convention (clip = M * p,
// R = Mp * inverse(Mc)): a node rigidly fixed on a parent body P (MVP_U = MVP_P * O, or view-space vertices V * W_P * O * p
// under a bare projection) has MVP_U_prev = MVP_P_prev * O = R_P * MVP_U_cur, so R_U = R_P exactly -- U's own matrix is not
// needed (in game it is the bare projection). The parent is the draw U SITS ON: a pixel vote over the draw-id target.
// v0.10.0 phase 6c MARCHING VOTE (in game 2026-10-08 17:27: a close car's plate took its parent only some of the time -- a plate
// is a flat patch, its +-2 / +-5 px neighbours were mostly the plate itself or its own text, so only the border ring voted):
// every sampled pixel of U marches outward in 8 directions (ParU.y px steps, up to ParU.z px) across U's own pixels and across
// depth-continuous pixels of draws WITHOUT a motion (other listed draws = plate background / text, unpaired / instanced draws);
// the first draw WITH a motion it meets (ParSource) is that direction's vote when it lies within Par.x relative linear depth of
// U's pixel (else the march is depth-rejected). Sky / no G-buffer id / another layer / a motionless surface at another depth /
// out of reach / off-screen = no source.
static const uint kParTabB = 192u;                  // v0.10.0 phase 6c: vote table bytes per U slot (see ParAddVote)
static const uint kParIter = kParU + 264u;          // v0.10.0 phase 6c: 0 = the 1st pass, 1 = the 2nd (CSParentPick1 sets it)
static const uint kParAnyFine = kParU + 268u;       // v0.10.0 phase 8: listed FINE draws with pixels this pass (CSParentList)
// draw j HAS a motion this frame = a valid parent source: state 2 / 3 (its own pair, a matrix twin, an origin attach, or a
// parent taken in the 1st pick) and, in the 1st pass, not a listed draw (a listed draw may still change its motion in the 1st
// pick). In the 2nd pass a listed draw is a source once it holds a motion -- such a draw never votes in the 2nd pass.
bool ParSource(uint j, uint iter) {
    uint st = (uint)(RTab[j * 5u + 4u].x + 0.5);
    if (st != 2u && st != 3u) return false;
    return iter != 0u || Work.Load(kParSlot + j * 4u) == kNone;
}
// v0.10.0 phase 14 (mv_replay_static_*): a world pixel WITHOUT a G-buffer id in a pass whose replay skipped long-static draws
// (ParU.x bit 4) is the pixel of such a draw -- the vote treats it as what that draw is: a STATIC source, one pseudo candidate
// (kParPseudo) for all of them. The vote then reads the same inputs whether a static neighbour was re-drawn this frame or not
// (in game: a moving car's plate found no parent on the 1-in-4 re-check frames). State 2, R = the layer's camera R.
static const uint kParPseudo = kMaxDraws;
uint ParState(uint j) { return j >= kMaxDraws ? 2u : (uint)(RTab[j * 5u + 4u].x + 0.5); }
// linear depth of a depth value inside draw j's viewport depth range (reversed-Z infinite: ~ near / view distance)
float ZLin(float d, uint j) {
    float4 inf = RTab[j * 5u + 4u];
    return (d - inf.y) / max(inf.z - inf.y, 1e-6);
}

// v0.10.0 phase 6b / 6c: one thread per kParStride-th pixel counts the on-screen pixels of every draw that may be listed into
// kParPix: a replayed instanced / no-MVP draw's RED-id pixels (ParU.x bit 2: CSParentList gives the free vote slots to the
// instanced draws that cover the screen, most pixels first) and (phase 6c) a genuine unpaired draw's RED (G-buffer) or GREEN
// (forward) id pixels -- the march budget of each listed draw (ParDecim).
// v0.10.0 phase 9: forward id / depth of full-image pixel p (GREEN + full-size depth, or the 1/2-size targets at p >> 1)
uint FwdIdAt(int2 p, uint2 ids) { return FwdMode.x != 0u ? PFwdIds[uint2(p) >> FwdMode.x] : ids.y; }
float FwdDepthAt(int2 p) { return PFwd[uint2(p) >> FwdMode.x]; }
// v0.10.0 phase 14 round 6: an instanced draw of MORE THAN ONE instance (a vegetation / clutter batch) is never counted, so never
// listed: it never takes a parent (moving or static), never holds one -- it keeps the camera R (state 4). Only a 1-instance draw (a
// close car's licence plate, phase 6b) and a no-MVP draw may take a parent. (The round-4 rule refused only MOVING parents and only
// above 4 instances: the user capture ats_flat_fence_frame1634.rdc has 31 of its 135 instanced G-buffer draws at 2..4 instances.)
// FoldU.w bit 2 = HARNESS mutation: the round-4 behaviour.
bool ParCountable(uint j) {
    uint4 e = Tab[j * 2u];
    if ((e.z & (kFInst | kFNoMvp)) != 0u)
        return (ParU.x & 4u) != 0u && ((e.z & kFInst) == 0u || (Tab[j * 2u + 1u].z >> 24) <= 1u || (FoldU.w & 4u) != 0u);
    return e.x != kNone && (uint)(RTab[j * 5u + 4u].x + 0.5) == 1u && Work.Load(kPartner + j * 4u) == kNone;
}
// v0.10.0 phase 14 round 4: the centroid sums (x, y, n) of a countable draw over a 2x2 sub-grid of each vote-grid cell (a centroid
// on the 4 px grid jitters by a pixel as a small draw moves over the grid -- the temporal check / plate hold compare centroids)
bool ParSumAt(int2 q) {                                            // (phase 18: true = q holds a countable draw)
    if (q.x >= (int)Clip.z || q.y >= (int)Clip.w) return false;
    uint2 ids = PIds[q];
    uint o0, j = kNone;
    if (ids.x != 0u && ids.x <= Counts.w && ParCountable(ids.x - 1u)) j = ids.x - 1u;
    else {
        uint fy = FwdIdAt(q, ids);
        if (fy > Counts.w && fy <= Counts.x && (Tab[(fy - 1u) * 2u].z & (kFInst | kFNoMvp)) == 0u && ParCountable(fy - 1u)) j = fy - 1u;
    }
    if (j == kNone) return false;
    Work.InterlockedAdd(kParSum + j * 12u, (uint)q.x, o0);
    Work.InterlockedAdd(kParSum + j * 12u + 4u, (uint)q.y, o0);
    Work.InterlockedAdd(kParSum + j * 12u + 8u, 1u, o0);
    return true;
}
// v0.10.0 phase 18 COMPACT VOTE: the count's group (8 x 8 grid samples) is exactly a vote group (same clip origin, same stride);
// a group that saw a countable draw at any of the samples a vote thread reads (its grid sample + the three 2 px sub-samples =
// the centroid sub-grid below) appends itself to the tile list. The listed draws are a subset of the countable ones (CSParentList
// lists after this pass from the same states), so no vote thread outside these tiles could march.
groupshared uint gTileAny;
[numthreads(8, 8, 1)]
void CSParentCount(uint3 tid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    if (gi == 0u) gTileAny = 0u;
    GroupMemoryBarrierWithGroupSync();
    uint st = max(ParU.y, 1u);
    int2 p = int2(Clip.xy) + int2(tid.xy * st);                    // v0.10.0 phase 9: the replay's clip only
    if (p.x < (int)Clip.z && p.y < (int)Clip.w) {
        uint2 ids = PIds[p];
        uint o0;
        if (ids.x != 0u && ids.x <= Counts.w && ParCountable(ids.x - 1u)) Work.InterlockedAdd(kParPix + (ids.x - 1u) * 4u, 1u, o0);
        uint fy = FwdIdAt(p, ids);
        if (fy > Counts.w && fy <= Counts.x) {
            uint j = fy - 1u;
            if ((Tab[j * 2u].z & (kFInst | kFNoMvp)) == 0u && ParCountable(j)) Work.InterlockedAdd(kParPix + j * 4u, 1u, o0);
        }
        int h = (int)max(st / 2u, 1u);                              // phase 14 round 4: centroid sub-grid
        bool a0 = ParSumAt(p);
        bool a1 = ParSumAt(p + int2(h, 0));
        bool a2 = ParSumAt(p + int2(0, h));
        bool a3 = ParSumAt(p + int2(h, h));
        if (a0 || a1 || a2 || a3) InterlockedOr(gTileAny, 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u && gTileAny != 0u && (FoldU.x & 32u) != 0u) {
        uint at;
        Cnt.InterlockedAdd(kCntTiles * 4u, 1u, at);
        if (at < kTileCap) ParList.Store(kTileBase + at * 4u, gid.x | (gid.y << 16));
    }
}

// (v0.10.0 phase 9: one more C++ raw-string split here, MSVC's 16380-byte literal cap): )" R"(
// v0.10.0 phase 6c: march decimation of listed draw u -- with C counted grid samples and 8 marches per sample, only every k-th
// grid sample in x and y marches (k = ceil(sqrt(8 C / ParU.w)), >= 1): at most ~ParU.w marches per pass, spread evenly over
// the draw (no bias towards one part of a large draw)
// v0.10.0 phase 8: `budget` = this draw's marches per pass (the frame cap thins it); a FINE draw (ParFine) marches every 2nd pixel,
// so its sample count is the counted one x (stride / 2)^2
uint ParDecim(uint u, float budget, bool fine) {
    float c = (float)Work.Load(kParPix + u * 4u);
    if (fine) { float r = (float)max(ParU.y, 2u) * 0.5; c *= r * r; }
    return max((uint)ceil(sqrt(8.0 * c / max(budget, 1.0))), 1u);
}
// v0.10.0 phase 8 (dlaa.ini mv_vote_res 4): a listed draw with fewer than Par2.y counted samples on the coarse grid is FINE --
// its pixels march on the 2 px grid with 2 px steps (the far 24x8 px plate keeps its votes), everything else on the coarse grid
bool ParFine(uint u) {
    return (ParU.x & 8u) != 0u && (float)Work.Load(kParPix + u * 4u) < Par2.y;
}

// after CSResolve (+ CSInherit1): every state-1 draw gets a U slot in draw order (deterministic prefix scan, one group); the
// 64 vote tables are cleared ([27] = the 1st pick's parent, kNone). A continuity reject (an identical copy on its first
// paired frame: it HAS a mutual partner) is not a U -- the camera R is the deliberate answer for that one frame.
// v0.10.0 phase 6b: when ParU.x bit 2 is set, the genuine state-1 draws take their slots first (unchanged); the replayed
// instanced / no-MVP draws with on-screen pixels then fill the remaining slots, most pixels first (tie: the lower index).
// v0.10.0 phase 6c: each assigned slot also gets its march decimation ([32], ParDecim); the pass flag starts at 0.
groupshared uint gScan[256];
groupshared uint gNVis;                                     // v0.10.0 phase 6b: visible instanced / no-MVP candidates
groupshared uint gVis, gFine;                               // v0.10.0 phase 8: listed draws with pixels / of them fine
static const uint kInstCap = 1024u;                         // v0.10.0 phase 9: compact candidate list capacity
groupshared uint gInstIdx[kInstCap], gInstPix[kInstCap];
void ParentListGroup(uint gi) {
    uint capU = FoldU.y != 0u ? min(FoldU.y, kParMaxU) : kParMaxU;
    for (uint k = gi; k < kParMaxU * (kParTabB / 4u); k += 256u) {
        uint w = k % (kParTabB / 4u);
        if (w != 32u && w != 42u) Work.Store(kParTab + k * 4u, w == 27u ? kNone : 0u);   // [32] / [42] are written with the slot below
    }
    if (gi == 0u) { gNVis = 0u; Work.Store(kParIter, 0u); }
    GroupMemoryBarrierWithGroupSync();
    uint n = Counts.x;
    uint base = 0u;
    for (uint c = 0u; c < n; c += 256u) {
        uint i = c + gi;
        uint isU = 0u;
        if (i < n) {
            uint4 e = Tab[i * 2u];
            if ((e.z & (kFInst | kFNoMvp)) == 0u && e.x != kNone && (uint)(RTab[i * 5u + 4u].x + 0.5) == 1u &&
                Work.Load(kPartner + i * 4u) == kNone) isU = 1u;
        }
        gScan[gi] = isU;
        GroupMemoryBarrierWithGroupSync();
        for (uint s = 1u; s < 256u; s <<= 1u) {
            uint v = gi >= s ? gScan[gi - s] : 0u;
            GroupMemoryBarrierWithGroupSync();
            gScan[gi] += v;
            GroupMemoryBarrierWithGroupSync();
        }
        uint slot = base + gScan[gi] - isU;
        if (isU != 0u && slot < capU) {
            Work.Store(kParSlot + i * 4u, slot); Work.Store(kParU + slot * 4u, i);
        }
        base += gScan[255];
        GroupMemoryBarrierWithGroupSync();
    }
    uint listed = min(base, capU);
    // v0.10.0 phase 6b: replayed instanced / no-MVP draws (ParU.x bit 2) with on-screen pixels take the remaining slots, most
    // pixels first (tie: the lower draw index). A total order over (pixels desc, index asc) gives every candidate a unique rank,
    // so slot = listed + rank has no collisions. They vote like unpaired draws but are never a source while listed draws in the
    // 1st pass (ParSource).
    // v0.10.0 phase 9: ranked in a compact groupshared list (barriers outside the branches)
    bool doInst = (ParU.x & 4u) != 0u && listed < capU;
    if (gi == 0u) gNVis = 0u;
    GroupMemoryBarrierWithGroupSync();
    if (doInst) {
        for (uint c2 = gi; c2 < n; c2 += 256u) {
            if ((Tab[c2 * 2u].z & (kFInst | kFNoMvp)) == 0u) continue;
            uint pc = Work.Load(kParPix + c2 * 4u);
            if (pc == 0u) continue;
            uint at;
            InterlockedAdd(gNVis, 1u, at);
            if (at < kInstCap) { gInstIdx[at] = c2; gInstPix[at] = pc; }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (doInst) {
        uint free = capU - listed;
        uint nv = gNVis;
        if (nv <= kInstCap) {
            for (uint a = gi; a < nv; a += 256u) {
                uint c2 = gInstIdx[a], pc = gInstPix[a];
                uint rank = 0u;
                for (uint b = 0u; b < nv; ++b) {
                    uint pj = gInstPix[b], j = gInstIdx[b];
                    if (pj > pc || (pj == pc && j < c2)) ++rank;
                }
                if (rank < free) {
                    uint sl = listed + rank;
                    Work.Store(kParSlot + c2 * 4u, sl); Work.Store(kParU + sl * 4u, c2);
                }
            }
        } else {                                            // (list full: the phase-8 loop)
            for (uint c3 = gi; c3 < n; c3 += 256u) {
                if ((Tab[c3 * 2u].z & (kFInst | kFNoMvp)) == 0u) continue;
                uint pc3 = Work.Load(kParPix + c3 * 4u);
                if (pc3 == 0u) continue;
                uint rank3 = 0u;
                for (uint j = 0u; j < n; ++j) {
                    if (j == c3 || (Tab[j * 2u].z & (kFInst | kFNoMvp)) == 0u) continue;
                    uint pj = Work.Load(kParPix + j * 4u);
                    if (pj == 0u) continue;
                    if (pj > pc3 || (pj == pc3 && j < c3)) ++rank3;
                }
                if (rank3 < free) {
                    uint sl3 = listed + rank3;
                    Work.Store(kParSlot + c3 * 4u, sl3); Work.Store(kParU + sl3 * 4u, c3);
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint free0 = listed < capU ? capU - listed : 0u;
    uint total = listed + min(free0, gNVis);
    // v0.10.0 phase 8: every listed draw's march decimation ([32]) and fine flag ([42]) now that the list is complete -- the frame
    // budget (Par2.z, dlaa.ini mv_vote_march_cap) is shared by the listed draws that have pixels (a draw without pixels never
    // marches); the vote dispatches are INDIRECT: no listed draw with pixels = no vote grid at all (ParArgs)
    if (gi == 0u) { gVis = 0u; gFine = 0u; }
    GroupMemoryBarrierWithGroupSync();
    if (gi < total && Work.Load(kParPix + Work.Load(kParU + gi * 4u) * 4u) != 0u) InterlockedAdd(gVis, 1u);
    GroupMemoryBarrierWithGroupSync();
    if (gi < total) {
        uint u = Work.Load(kParU + gi * 4u);
        bool fine = ParFine(u);
        float budget = (float)ParU.w;
        if (Par2.z > 0.0) budget = min(budget, max(Par2.z / (float)max(gVis, 1u), 64.0));
        Work.Store(kParTab + gi * kParTabB + 128u, ParDecim(u, budget, fine));
        Work.Store(kParTab + gi * kParTabB + 168u, fine ? 1u : 0u);
        if (fine && Work.Load(kParPix + u * 4u) != 0u) InterlockedAdd(gFine, 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u) {
        uint free = listed < capU ? capU - listed : 0u;
        uint assigned = min(free, gNVis);
        Work.Store2(kParU + 256u, uint2(listed + assigned, base - listed));
        Cnt.Store(54u * 4u, listed);
        Cnt.Store(55u * 4u, base - listed);
        Cnt.Store(195u * 4u, assigned);     // v0.10.0 phase 6b: instanced / no-MVP draws listed as U
        Cnt.Store(197u * 4u, gNVis);        //   candidates with on-screen pixels (assigned = min(free, this))
        Cnt.Store(205u * 4u, gVis);         // v0.10.0 phase 8: listed draws with pixels (0 = the vote grids are skipped)
        Cnt.Store(206u * 4u, gFine);        //   of them FINE (2 px samples, see CSParentVote)
        Work.Store(kParAnyFine, gFine);
        uint S = max(ParU.y, 1u);
        uint gw = (((Clip.z - Clip.x) + S - 1u) / S + 7u) / 8u, gh = (((Clip.w - Clip.y) + S - 1u) / S + 7u) / 8u;
        // v0.10.0 phase 18 (FoldU.x bit 5): the same args drive CSParentCollect; over the tiles CSParentCount listed (tile mode
        // 1), or the full grid when they did not fit the list (mode 0)
        uint tiles = Cnt.Load(kCntTiles * 4u);
        bool tileMode = (FoldU.x & 32u) != 0u && tiles <= kTileCap;
        if ((FoldU.x & 32u) != 0u && !tileMode) Cnt.Store(kCntTileOver * 4u, 1u);
        Work.Store(kParU + 272u, tileMode ? 1u : 0u);
        ParArgs.Store3(0u, gVis == 0u ? uint3(0u, 1u, 1u) : (tileMode ? uint3(tiles, 1u, 1u) : uint3(gw, gh, 1u)));   // the votes
    }
}
[numthreads(256, 1, 1)]
void CSParentList(uint gi : SV_GroupIndex) {
    ParentListGroup(gi);
}

// vote table of U slot s (kParTabB = 192 B; v0.10.0 phase 6c): [0..7] candidate (draw + 1), [8..15] its votes, [16..23] its
// summed relative depth difference to U (x 16384), [24] accepted votes, [25] depth-rejected marches, [26] marches without a
// source, [27] the parent the 1st pick chose (kNone), [28] marched px (1st pass), [29] / [30] / [31] the 1st pass's accepted
// votes / depth-rejected marches / packed votes, [32] march decimation (CSParentList), [33..40] the march directions that met
// each candidate (bit k = kParDir[k]), [41] the 1st pass's marches without a source; the 2nd pass reuses [0..26] and [33..40]
void ParAddVote(uint base, uint cand, uint dzq, uint dirBit) {
    uint old;
    for (uint k = 0u; k < 8u; ++k) {
        uint orig;
        Work.InterlockedCompareExchange(base + k * 4u, 0u, cand, orig);
        if (orig == 0u || orig == cand) {
            Work.InterlockedAdd(base + 32u + k * 4u, 1u, old);
            Work.InterlockedAdd(base + 64u + k * 4u, dzq, old);
            Work.InterlockedOr(base + 132u + k * 4u, dirBit, old);
            return;
        }
    }
}
static const int2 kParDir[8] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
                                 int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1) };
// v0.10.0 phase 6c: the 8 marches from U's pixel p (depth dp; fwdU = a forward U, whose own pixels carry its GREEN id): each
// steps ParU.y px at a time up to ParU.z px (per axis) and stops at the first pixel that is neither U's own nor a motionless
// draw at U's depth (see the header above); the RED (G-buffer) id there is the candidate
// (v0.10.0 phase 18: one more C++ raw-string split here, MSVC's literal cap): )" R"(
// v0.10.0 phase 18: ONE march direction k (the body of the phase-6c loop): 0 = no source, 1 = a vote for cand (dz = relative depth
// difference), 2 = depth-rejected; px = the px it marched. ParMarch runs the 8 in a row (one thread per sample, as before);
// CSParentMarch runs each on its own thread (the compact vote)
uint ParMarchDir(uint u, int2 p, float zp, uint fu, bool fwdU, uint iter, uint step, uint nStep, uint k,
                 out uint cand, out float dz, out uint px) {
    int2 d = kParDir[k] * (int)step;
    int2 q = p;
    uint res = 0u, s = 1u;
    float zLast = zp;                                               // zLast: the patch pixel before q
    cand = 0u; dz = 0.0;
    [loop] for (; s <= nStep; ++s) {
        q += d;
        if (q.x < 0 || q.y < 0 || q.x >= (int)Params.z || q.y >= (int)Params.w) break;
        uint2 ids = PIds[q];
        if ((fwdU ? FwdIdAt(q, ids) : ids.x) == u + 1u) {            // U's own pixel
            float dq = fwdU ? FwdDepthAt(q) : PDepth[q];
            if (dq > 0.0) zLast = ZLin(dq, u);
            continue;
        }
        uint g = ids.x;
        if (g > Counts.w) break;                                     // (not a G-buffer id)
        uint j = kParPseudo;
        float zq = 0.0;
        if (g == 0u) {                                               // no G-buffer draw here (sky / not replayed) ...
            // ... phase 14: unless the static gate skipped one here (world-range depth): the pseudo static source
            float dq0 = PDepth[q];
            if ((ParU.x & 16u) == 0u || (fu & kFCabin) != 0u || !(dq0 >= 0.01 && dq0 < 0.9)) break;
            zq = ZLin(dq0, u);                                       // (U's layer: U's viewport depth range)
        } else {
            j = g - 1u;
            if (((Tab[j * 2u].z ^ fu) & kFCabin) != 0u) break;       // another depth layer
            zq = ZLin(PDepth[q], j);
        }
        float rel = abs(zq - zp) / max(max(zq, zp), 1e-6);
        bool nearZ = rel <= Par.x;
        if (j == kParPseudo || ParSource(j, iter)) {                 // within Par.x of U AND flush with the patch edge
            bool flush = abs(zq - zLast) <= Par2.x * max(max(zq, zLast), 1e-6);
            res = (nearZ && flush) ? 1u : 2u; cand = j + 1u; dz = rel;
            break;
        }
        if (!nearZ) break;                                           // a motionless surface at another depth: the patch ends
        zLast = zq;
    }
    px = min(s, nStep) * step;
    return res;
}
void ParMarch(uint u, uint slot, int2 p, float dp, bool fwdU, uint iter, uint step) {
    uint base = kParTab + slot * kParTabB;
    if (Work.Load(base + 96u) + Work.Load(base + 100u) + Work.Load(base + 104u) >= (uint)Par.w) return;   // bounds the atomics
    uint fu = Tab[u * 2u].z;
    float zp = ZLin(dp, u);
    step = max(step, 1u);                                           // v0.10.0 phase 8: the slot's own step (fine: 2 px)
    uint nStep = max(ParU.z / step, 1u);
    uint nAcc = 0u, nRej = 0u, nNo = 0u, px = 0u;
    [loop] for (uint k = 0u; k < 8u; ++k) {
        uint cand, pxk;
        float dz;
        uint res = ParMarchDir(u, p, zp, fu, fwdU, iter, step, nStep, k, cand, dz, pxk);
        px += pxk;
        if (res == 1u) { ++nAcc; ParAddVote(base, cand, (uint)(min(dz, 1.0) * 16384.0), 1u << k); }
        else if (res == 2u) ++nRej;
        else ++nNo;
    }
    uint old;
    if (nAcc != 0u) Work.InterlockedAdd(base + 96u, nAcc, old);
    if (nRej != 0u) Work.InterlockedAdd(base + 100u, nRej, old);
    if (nNo != 0u) Work.InterlockedAdd(base + 104u, nNo, old);
    if (iter == 0u) Work.InterlockedAdd(base + 112u, px, old);
}
// does U (slot) march from its pixel p in this pass? every k-th sample of its grid in x and y (ParDecim; v0.10.0 phase 8: the
// grid of a FINE draw is the 2 px grid, else the vote grid ParU.y); in the 2nd pass only a listed draw the 1st pick left without
// a parent and without a twin / origin attach (those hold a motion: 2nd-pass sources)
bool ParVoter(uint u, uint slot, int2 p, uint iter) {
    uint base = kParTab + slot * kParTabB;
    uint k = max(Work.Load(base + 128u), 1u);
    uint2 g = uint2(p) / (Work.Load(base + 168u) != 0u ? 2u : max(ParU.y, 1u));
    if ((g.x % k) != 0u || (g.y % k) != 0u) return false;
    if (iter == 0u) return true;
    return Work.Load(base + 108u) == kNone && Work.Load(kTwin + u * 4u) == kNone && Work.Load(kAttach + u * 4u) == kNone;
}
// v0.10.0 phase 6c (the source is split into a fifth C++ raw string here, MSVC's literal cap): )" R"(
// one thread per ParU.y-th pixel in x and y, dispatched twice (pass flag kParIter; v0.10.0 phase 8: INDIRECT, no grid at all when
// no listed draw has pixels): a pixel owned by a U (RED id = G-buffer U at the scene depth; GREEN id where the forward depth won,
// pass B's rule = forward U at the forward depth) marches (ParMarch).
// v0.10.0 phase 8 (dlaa.ini mv_vote_res 4): the thread owns the 4x4 cell at p. A COARSE listed draw marches from p only, in 4 px
// steps (a quarter of the phase-6c samples, half the steps). When the frame has a FINE listed draw (kParAnyFine) the thread also
// reads the cell's three other 2 px samples, and a fine draw marches from every one of them in 2 px steps -- exactly the phase-6c
// samples of that draw, however thin or sparse it is (a box around its coarse samples missed most of a grass clump's blades)
void ParFrom(uint u, int2 p, float dp, bool fwdU, uint iter, bool sub) {
    uint slot = Work.Load(kParSlot + u * 4u);
    if (slot >= kParMaxU) return;
    bool fine = Work.Load(kParTab + slot * kParTabB + 168u) != 0u;
    if (sub && !fine) return;                                      // a coarse draw: its grid sample only
    if (ParVoter(u, slot, p, iter)) ParMarch(u, slot, p, dp, fwdU, iter, fine ? 2u : max(ParU.y, 1u));
}
[numthreads(8, 8, 1)]
void CSParentVote(uint3 tid : SV_DispatchThreadID) {
    if (Work.Load(kParU + 256u) == 0u) return;
    uint iter = Work.Load(kParIter);
    int2 p0 = int2(Clip.xy) + int2(tid.xy * max(ParU.y, 1u));   // v0.10.0 phase 9: the replay's clip only
    uint nSub = (ParU.y > 2u && Work.Load(kParAnyFine) != 0u) ? 4u : 1u;
    for (uint k = 0u; k < nSub; ++k) {
        int2 p = p0 + int2((int)(k & 1u), (int)(k >> 1)) * 2;
        if (p.x >= (int)Clip.z || p.y >= (int)Clip.w) continue;
        uint2 ids = PIds[p];
        if (ids.x != 0u && ids.x <= Counts.w) ParFrom(ids.x - 1u, p, PDepth[p], false, iter, k != 0u);
        uint fy = FwdIdAt(p, ids);
        if (fy > Counts.w && fy <= Counts.x) {
            float fw = FwdDepthAt(p), dt = PDepth[p];
            if (fw >= 0.01 && fw < 0.9 && fw > dt) ParFrom(fy - 1u, p, fw, true, iter, k != 0u);
        }
    }
}

// (v0.10.0 phase 18: one more C++ raw-string split here, MSVC's 16380-byte literal cap): )" R"(
// v0.10.0 phase 18 COMPACT VOTE (dlaa.ini mv_vote_compact 1). In game the vote cost 0.12-0.18 ms per pass (x 2 passes) per eye for
// ~10k marches per frame while the pixel count over the same grid cost 0.01: the work sat in a few threads -- a thread of a small
// (FINE) plate marched 4 sub-samples x 8 directions x 16 steps in a row (512 dependent texture reads), on a handful of SMs. Now:
// CSParentCollect (over the tiles CSParentCount listed) does exactly CSParentVote's per-pixel tests and appends each march sample
// as an item; CSParentMarch runs one thread per (item, direction) -- the same ParMarchDir, the same vote tables. The vote's
// result depends only on WHICH marches run and what each finds (atomics: order-free counts; the candidate slots of a table fill in
// arrival order as before, ParBest is order-free unless more than 8 candidates arrive -- the existing GPU-order caveat).
void ParEmit(uint u, int2 p, float dp, bool fwdU, uint iter, bool sub) {
    uint slot = Work.Load(kParSlot + u * 4u);
    if (slot >= kParMaxU) return;
    bool fine = Work.Load(kParTab + slot * kParTabB + 168u) != 0u;
    if (sub && !fine) return;                                      // a coarse draw: its grid sample only
    if (!ParVoter(u, slot, p, iter)) return;
    uint step = fine ? 2u : max(ParU.y, 1u);
    uint idx, o;
    Cnt.InterlockedAdd((kCntItems + iter) * 4u, 1u, idx);
    if (idx < kItemCap) {
        ParList.Store4(kItemBase + idx * 16u, uint4((uint)p.x | ((uint)p.y << 16), u | (fwdU ? 0x80000000u : 0u), asuint(dp),
                                                    slot | (step << 16)));
        if ((idx & 7u) == 0u) ParArgs.InterlockedAdd(iter * 16u, 1u, o);   // (u5 = the march args here: 8 items per group)
    } else {
        Cnt.InterlockedAdd(kCntItemOver * 4u, 1u, o);
        ParMarch(u, slot, p, dp, fwdU, iter, step);                // over the cap: marched in place (the phase-8 thread)
    }
}
[numthreads(8, 8, 1)]
void CSParentCollect(uint3 gid : SV_GroupID, uint3 gt : SV_GroupThreadID) {
    if (Work.Load(kParU + 256u) == 0u) return;
    uint iter = Work.Load(kParIter);
    uint2 tile = gid.xy;
    if (Work.Load(kParU + 272u) != 0u) { uint t = ParList.Load(kTileBase + gid.x * 4u); tile = uint2(t & 0xFFFFu, t >> 16); }
    uint2 tid = tile * 8u + gt.xy;
    int2 p0 = int2(Clip.xy) + int2(tid * max(ParU.y, 1u));
    uint nSub = (ParU.y > 2u && Work.Load(kParAnyFine) != 0u) ? 4u : 1u;
    for (uint k = 0u; k < nSub; ++k) {
        int2 p = p0 + int2((int)(k & 1u), (int)(k >> 1)) * 2;
        if (p.x >= (int)Clip.z || p.y >= (int)Clip.w) continue;
        uint2 ids = PIds[p];
        if (ids.x != 0u && ids.x <= Counts.w) ParEmit(ids.x - 1u, p, PDepth[p], false, iter, k != 0u);
        uint fy = FwdIdAt(p, ids);
        if (fy > Counts.w && fy <= Counts.x) {
            float fw = FwdDepthAt(p), dt = PDepth[p];
            if (fw >= 0.01 && fw < 0.9 && fw > dt) ParEmit(fy - 1u, p, fw, true, iter, k != 0u);
        }
    }
}
// 8 items per group, thread = item * 8 + direction; each item's counters are summed in groupshared and added once (as ParMarch)
groupshared uint gMAcc[8], gMRej[8], gMNo[8], gMPx[8];
[numthreads(64, 1, 1)]
void CSParentMarch(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    uint li = gi >> 3u, k = gi & 7u;
    if (k == 0u) { gMAcc[li] = 0u; gMRej[li] = 0u; gMNo[li] = 0u; gMPx[li] = 0u; }
    GroupMemoryBarrierWithGroupSync();
    uint iter = Work.Load(kParIter);
    uint n = min(Cnt.Load((kCntItems + iter) * 4u), kItemCap);
    uint item = gid.x * 8u + li;
    uint base = 0u;
    bool live = item < n;
    if (live) {
        uint4 it = ParList.Load4(kItemBase + item * 16u);
        int2 p = int2((int)(it.x & 0xFFFFu), (int)(it.x >> 16));
        uint u = it.y & 0x7FFFFFFFu;
        bool fwdU = (it.y & 0x80000000u) != 0u;
        uint slot = it.w & 0xFFFFu, step = max(it.w >> 16, 1u);
        base = kParTab + slot * kParTabB;
        // (the atomics bound, per direction here: never reached with the per-draw budget -- ParU.w <= 8192 marches per pass)
        if (Work.Load(base + 96u) + Work.Load(base + 100u) + Work.Load(base + 104u) >= (uint)Par.w) live = false;
        if (live) {
            uint cand, px, o;
            float dz;
            uint res = ParMarchDir(u, p, ZLin(asfloat(it.z), u), Tab[u * 2u].z, fwdU, iter, step, max(ParU.z / step, 1u), k,
                                   cand, dz, px);
            InterlockedAdd(gMPx[li], px, o);
            if (res == 1u) { InterlockedAdd(gMAcc[li], 1u, o); ParAddVote(base, cand, (uint)(min(dz, 1.0) * 16384.0), 1u << k); }
            else if (res == 2u) InterlockedAdd(gMRej[li], 1u, o);
            else InterlockedAdd(gMNo[li], 1u, o);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (k == 0u && item < n) {
        uint old;
        uint nAcc = gMAcc[li], nRej = gMRej[li], nNo = gMNo[li];
        if (nAcc != 0u) Work.InterlockedAdd(base + 96u, nAcc, old);
        if (nRej != 0u) Work.InterlockedAdd(base + 100u, nRej, old);
        if (nNo != 0u) Work.InterlockedAdd(base + 104u, nNo, old);
        if (iter == 0u && nAcc + nRej + nNo != 0u) Work.InterlockedAdd(base + 112u, gMPx[li], old);
    }
}

// U takes its rigid parent P's motion: mover -> P's R rows (= R_U exactly, see above) and state 3; static -> state 2 with the
// camera R of its layer. RTab .w = -(1 + 2 kMaxDraws + P) marks it (Ctrl+F6 green tint, the dump)
void ParApply(uint u, uint p, uint vt) {
    uint o = u * 5u;
    float4 own = RTab[o + 4u];
    uint pst = ParState(p);                                     // (phase 14: kParPseudo = a static parent)
    uint4 e = Tab[u * 2u];
    float4x4 R = LayerR(e.z);
    if (pst == 3u) {
        R = float4x4(RTab[p * 5u], RTab[p * 5u + 1u], RTab[p * 5u + 2u], RTab[p * 5u + 3u]);
        if ((ParU.x & 2u) != 0u) {      // HARNESS MUTATION only: the row-vector formula typed into this column-vector code
            float4x4 Mu = LoadMW(CurM, u * kMStride), Mpc = LoadMW(CurM, p * kMStride);
            float4x4 Mpp = LoadM(PrevM, Work.Load(kPartner + p * 4u) * kMStride);
            float4x4 Iu, Ip;
            if (Inv4(Mu, Iu) && Inv4(Mpc, Ip)) R = mul(mul(mul(Mu, Ip), Mpp), Iu);
        }
    }
    uint old;
    if (Work.Load(kTwin + u * 4u) != kNone || Work.Load(kAttach + u * 4u) != kNone) {   // the vote overrides phase 5
        Cnt.InterlockedAdd(62u * 4u, 1u, old);
        if ((uint)(own.x + 0.5) != pst) Cnt.InterlockedAdd(193u * 4u, 1u, old);
    }
    RTab[o] = R[0]; RTab[o + 1u] = R[1]; RTab[o + 2u] = R[2]; RTab[o + 3u] = R[3];
    RTab[o + 4u] = float4((float)pst, own.y, own.z, -1.0 - 2.0 * (float)kMaxDraws - (float)p);
    Work.Store(kParent + u * 4u, p);
    Work.Store(kParVT + u * 4u, vt);
    Cnt.InterlockedAdd(56u * 4u, 1u, old);
    if ((e.z & (kFInst | kFNoMvp)) != 0u) Cnt.InterlockedAdd(196u * 4u, 1u, old);   // v0.10.0 phase 6b: instanced / no-MVP -> parent
    if ((e.z & (kFInst | kFNoMvp)) != 0u && pst == 3u)   // v0.10.0 phase 8: ... a MOVING one (bitmask, mv_inst_replay)
        Cnt.InterlockedOr((256u + (u >> 5)) * 4u, 1u << (u & 31u), old);
    if (pst == 3u) { Cnt.InterlockedAdd(57u * 4u, 1u, old); StatReset(u); }   // (phase 14: a moving parent = it moves)
    if (p < Counts.w) Cnt.InterlockedOr((512u + (p >> 5u)) * 4u, 1u << (p & 31u), old);   // v0.10.0 phase 14: a rigid parent
    if ((e.z & kFFwd) != 0u) Cnt.InterlockedAdd(58u * 4u, 1u, old);
    if ((vt & kChainBit) != 0u) Cnt.InterlockedAdd(59u * 4u, 1u, old);   // phase 6c: won in the 2nd pass
}
// v0.10.0 phase 6c: do sources a and b carry the SAME motion? both static (state 2 = the camera R of the layer), or both movers
// whose R rows agree within 2e-3 of their size (the rigid parts of one vehicle: body, bumper, lamps -- the vote must not split)
bool ParSameMotion(uint a, uint b) {
    uint sa = ParState(a), sb = ParState(b);                    // (phase 14: the pseudo static source = state 2)
    if (sa != sb) return false;
    if (sa != 3u) return true;
    float4x4 Ra = float4x4(RTab[a * 5u], RTab[a * 5u + 1u], RTab[a * 5u + 2u], RTab[a * 5u + 3u]);
    float4x4 Rb = float4x4(RTab[b * 5u], RTab[b * 5u + 1u], RTab[b * 5u + 2u], RTab[b * 5u + 3u]);
    float4x4 Z = float4x4(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    return Frob(Ra, Rb) <= 2e-3 * max(Frob(Ra, Z), 1e-6);
}
// a direction mask with hits on two OPPOSITE sides of U (left + right, up + down, or a diagonal pair)
bool ParOpposite(uint m) { return (m & 3u) == 3u || (m & 12u) == 12u || (m & 0x90u) == 0x90u || (m & 0x60u) == 0x60u; }
// v0.10.0 phase 6c: the winning MOTION of a vote table. Candidates carrying the same motion pool their votes and directions
// (a car's body, bumper and lamps must not split the vote). A motion met on two OPPOSITE sides of U qualifies (a parent
// surrounds what sits on it); a motion met on one side only qualifies only when it is the only motion met at all (acc = all
// accepted votes: a plate in a body's corner, with the road / sky around the rest) -- a vehicle body touching the top of a
// grass clump that also meets the ground does not. Best = the most pooled votes; a tie -> the candidate nearer in depth to U
// (smaller mean relative depth difference), then the lower draw index. Returns the pooled votes; who = that candidate
// (kNone: no motion qualifies)
// (v0.10.0 phase 14 round 4: one more C++ raw-string split here, MSVC's 16380-byte literal cap): )" R"(
// v0.10.0 phase 14 round 4 TEMPORAL CHECK of a MOVING candidate p for listed draw u (FoldU.w bit 0: last pass's listed-draw table
// UHoldR is valid). Grass standing right in front of a passing car's side is surrounded by the car on screen at nearly its depth,
// exactly like a plate on its body -- the pixel vote alone cannot tell them apart (in game: roadside grass went wobbly / blurry
// while an SUV overtook). Time can: carry u's pixel centroid back by p's motion (pm) and by the camera motion of u's layer (ps);
// when they differ by more than 2 kHoldPx, last pass's table must hold an entry of the same IndexCount / size at pm -- if it has
// one at ps but none at pm, u did not move with p last frame (it is static): p is rejected (Cnt [212]). No / ambiguous evidence =
// the vote decides as before.
static const uint kHoldMax = 3u, kHoldBit = 0x20000000u;
static const float kHoldPx = 6.0, kTempPx = 2.5;
static const uint kParInstClutter = 4u;                 // a draw of more instances is a clutter / vegetation batch (see ParBest)
// a draw's pixel centroid (pixel-index units, float) from its sub-grid sums; packed in quarter pixels (16 bits per axis)
float2 ParCentroid(uint3 sum) { return float2((float)sum.x, (float)sum.y) / (float)max(sum.z, 1u); }
// the draw's viewport sub-pixel jitter (Tab[2u + 1].w, two int16 of 1/16384 px): pixel centroids of two frames are compared minus it
float2 ParJit(uint u) {
    uint w = Tab[u * 2u + 1u].w;
    int jx = (int)(w << 16) >> 16, jy = (int)w >> 16;
    return float2((float)jx, (float)jy) / 16384.0;
}
uint ParPackC(float2 c) { return min((uint)(c.x * 4.0 + 0.5), 0xFFFFu) | (min((uint)(c.y * 4.0 + 0.5), 0xFFFFu) << 16); }
float2 ParUnpackC(uint v) { return float2((float)(v & 0xFFFFu), (float)(v >> 16)) * 0.25; }
float2 ParBackPx(float4x4 R, float2 ndc, float zd) {     // full-image px where R puts this point last frame (-1e9: behind)
    float4 c = mul(R, float4(ndc, zd, 1.0));
    if (!(c.w > 1e-6)) return float2(-1e9, -1e9);
    return float2((c.x / c.w * 0.5 + 0.5) * Params.z - 0.5, (0.5 - c.y / c.w * 0.5) * Params.w - 0.5);
}
bool ParMotionOk(uint u, uint p) {
    if ((FoldU.w & 1u) == 0u || p >= kMaxDraws || ParState(p) != 3u) return true;
    uint3 sum = Work.Load3(kParSum + u * 12u);
    uint pix = sum.z;
    if (pix == 0u) return true;
    float2 cr = ParCentroid(sum);                        // (the raw centroid: its depth; minus the jitter: compared / projected)
    int2 ci = int2(cr + 0.5);
    if (ci.x >= (int)Params.z || ci.y >= (int)Params.w) return true;
    float2 cf = cr - ParJit(u);
    uint4 e = Tab[u * 2u];
    uint ic = Tab[u * 2u + 1u].z & 0xFFFFFFu;
    float dc = (e.z & kFFwd) != 0u ? FwdDepthAt(ci) : PDepth[ci];
    float2 ndc = float2((cf.x + 0.5) / Params.z * 2.0 - 1.0, 1.0 - (cf.y + 0.5) / Params.w * 2.0);
    float4 iu = RTab[u * 5u + 4u], ip = RTab[p * 5u + 4u];
    float2 ps = ParBackPx(LayerR(e.z), ndc, saturate((dc - iu.y) / max(iu.z - iu.y, 1e-6)));
    float2 pm = ParBackPx(float4x4(RTab[p * 5u], RTab[p * 5u + 1u], RTab[p * 5u + 2u], RTab[p * 5u + 3u]), ndc,
                          saturate((dc - ip.y) / max(ip.z - ip.y, 1e-6)));
    // a centroid on the 2 px sub-grid jumps by up to ~1 px as a draw slides over the grid (+ the frame's jitter is removed): below
    // kTempPx of difference between the two predictions the evidence is noise -- the vote decides as before
    if (length(pm - ps) < kTempPx) return true;
    // an entry within kHoldPx of a prediction is evidence for the prediction it lies clearly CLOSER to
    bool atS = false, atM = false;
    for (uint k = 0u; k < kParMaxU; ++k) {
        uint4 h = UHoldR[k];
        uint hic = h.x & 0xFFFFFFu;
        if (hic == 0u || hic != ic || h.w * 2u < pix || pix * 2u < h.w) continue;
        float2 hc = ParUnpackC(h.y);
        float dS = length(hc - ps), dM = length(hc - pm);
        if (dS <= kHoldPx && dS + 0.5 * length(pm - ps) < dM) atS = true;   // clearly nearer the camera prediction
        if (dM <= kHoldPx && dM <= dS) atM = true;
    }
    if (atS && !atM) { uint o; Cnt.InterlockedAdd(212u * 4u, 1u, o); return false; }
    return true;
}
uint ParBest(uint u, uint base, uint acc, out uint who) {
    uint c[8], v[8], m[8];
    [unroll] for (uint i = 0u; i < 8u; ++i) {
        c[i] = Work.Load(base + i * 4u); v[i] = Work.Load(base + 32u + i * 4u); m[i] = Work.Load(base + 132u + i * 4u);
    }
    uint best = 0u;
    float bz = 0.0;
    who = kNone;
    for (uint k = 0u; k < 8u; ++k) {
        if (c[k] == 0u || v[k] == 0u) continue;
        uint d = c[k] - 1u;
        // phase 14 round 4: a multi-instance draw (> kParInstClutter instances: a vegetation / clutter batch -- the roadside grass
        // beside an overtaking SUV) never takes a MOVING parent (a close car's plate is a 1-instance draw); then the temporal check
        // (round 6: a draw of more than 1 instance is no longer listed at all, ParCountable -- the instance test below only acts
        // under the harness mutation "mutinst")
        if (ParState(d) == 3u && ((Tab[u * 2u + 1u].z >> 24) > kParInstClutter || !ParMotionOk(u, d))) {
            if ((Tab[u * 2u + 1u].z >> 24) > kParInstClutter) { uint oi; Cnt.InterlockedAdd(213u * 4u, 1u, oi); }
            continue;
        }
        uint pv = 0u, pm = 0u;
        for (uint q = 0u; q < 8u; ++q) {
            if (c[q] == 0u || v[q] == 0u) continue;
            if (q == k || ParSameMotion(d, c[q] - 1u)) { pv += v[q]; pm |= m[q]; }
        }
        if (!ParOpposite(pm) && pv < acc) continue;
        float z = (float)Work.Load(base + 64u + k * 4u) / (float)v[k];
        if (who == kNone || pv > best || (pv == best && (z < bz || (z == bz && d < who)))) { best = pv; bz = z; who = d; }
    }
    return best;
}
// 1st pick: the motion (ParBest) with >= Par.y pooled votes and >= Par.z of U's accepted votes gives U its parent; otherwise the 1st pass's
// evidence is kept ([29..31]) and the table is cleared for the 2nd pass. Sets the pass flag for the 2nd vote.
void ParPick1(uint s) {
    uint u = Work.Load(kParU + s * 4u);
    uint base = kParTab + s * kParTabB;
    uint acc = Work.Load(base + 96u), rej = Work.Load(base + 100u), nos = Work.Load(base + 104u);
    uint old;
    Cnt.InterlockedAdd(192u * 4u, acc, old);
    Cnt.InterlockedAdd(200u * 4u, acc + rej + nos, old);   // phase 6c: 1st-pass marches, marched px, depth rejects, no source
    Cnt.InterlockedAdd(201u * 4u, Work.Load(base + 112u), old);
    Cnt.InterlockedAdd(202u * 4u, rej, old);
    Cnt.InterlockedAdd(203u * 4u, nos, old);
    uint p;
    uint v = ParBest(u, base, acc, p);
    uint vt = (min(v, 32767u) << 16) | min(acc, 65535u);
    Work.Store(kParVT + u * 4u, vt);
    if (p != kNone && v >= (uint)Par.y && (float)v >= Par.z * (float)acc) {
        Work.Store(base + 108u, p);
        ParApply(u, p, vt);
        return;
    }
    Work.Store(base + 116u, acc); Work.Store(base + 120u, rej); Work.Store(base + 124u, vt); Work.Store(base + 164u, nos);
    for (uint k = 0u; k < 27u; ++k) Work.Store(base + k * 4u, 0u);
    for (uint k2 = 33u; k2 < 41u; ++k2) Work.Store(base + k2 * 4u, 0u);
}
// v0.10.0 phase 8: the listed draws that march again in the 2nd pass share its frame budget (Par2.w, dlaa.ini mv_vote_march_cap)
// -> their decimation ([32]) is recomputed for it (no frame cap: the same per-draw budget as the 1st pass = unchanged)
groupshared uint gRem;
[numthreads(64, 1, 1)]
void CSParentPick1(uint3 tid : SV_DispatchThreadID) {
    uint s = tid.x;
    if (s == 0u) { Work.Store(kParIter, 1u); gRem = 0u; }
    uint nl = Work.Load(kParU + 256u);
    GroupMemoryBarrierWithGroupSync();
    if (s < nl) ParPick1(s);
    DeviceMemoryBarrierWithGroupSync();
    bool again = false;
    uint u2 = 0u;
    if (s < nl) {
        u2 = Work.Load(kParU + s * 4u);
        again = Work.Load(kParTab + s * kParTabB + 108u) == kNone && Work.Load(kTwin + u2 * 4u) == kNone &&
                Work.Load(kAttach + u2 * 4u) == kNone && Work.Load(kParPix + u2 * 4u) != 0u;
        if (again) InterlockedAdd(gRem, 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (again && Par2.w > 0.0) {
        float budget = min((float)ParU.w, max(Par2.w / (float)max(gRem, 1u), 64.0));
        uint b2 = kParTab + s * kParTabB;
        Work.Store(b2 + 128u, ParDecim(u2, budget, Work.Load(b2 + 168u) != 0u));
    }
}
// 2nd pick: a listed draw the 1st pick left without a parent (and without a twin / origin attach) marched again with the listed
// draws that now hold a motion as sources (plate text inside a plate background wider than the march reach); it takes the
// winner's ROOT parent (vt bit 31 = 2nd pass). The rest is counted by reason: some votes but no winner (Cnt [61]), every source
// depth-rejected ([199]; vt bit 31 = this reason when there is no parent), no source within reach ([60]; vt bit 30), no pixel
// of it was sampled at all -- hidden / sub-pixel ([204]; vt bits 31 + 30)
void ParPick2Slot(uint s) {
    if (s >= Work.Load(kParU + 256u)) return;
    uint base = kParTab + s * kParTabB;
    if (Work.Load(base + 108u) != kNone) return;
    uint u = Work.Load(kParU + s * 4u);
    bool kept = Work.Load(kTwin + u * 4u) != kNone || Work.Load(kAttach + u * 4u) != kNone;
    uint acc = Work.Load(base + 96u), rej = Work.Load(base + 100u), nos = Work.Load(base + 104u);
    uint old;
    Cnt.InterlockedAdd(192u * 4u, acc, old);
    uint p;
    uint v = ParBest(u, base, acc, p);
    if (!kept && p != kNone && v >= (uint)Par.y && (float)v >= Par.z * (float)acc) {
        uint root = p < Counts.x ? Work.Load(kParent + p * 4u) : kNone;   // (phase 14: the pseudo source has no root)
        ParApply(u, root != kNone ? root : p, (min(v, 32767u) << 16) | min(acc, 65535u) | kChainBit);
        return;
    }
    uint acc1 = Work.Load(base + 116u), rej1 = Work.Load(base + 120u);
    uint vt = Work.Load(base + 124u);
    if (acc > acc1) vt = (min(v, 32767u) << 16) | min(acc, 65535u);
    if (acc + acc1 != 0u) Cnt.InterlockedAdd(61u * 4u, 1u, old);
    else if (rej + rej1 != 0u) { Cnt.InterlockedAdd(199u * 4u, 1u, old); vt = kChainBit; }
    else if (nos + Work.Load(base + 164u) != 0u) { Cnt.InterlockedAdd(60u * 4u, 1u, old); vt = 0x40000000u; }
    else { Cnt.InterlockedAdd(204u * 4u, 1u, old); vt = 0xC0000000u; }
    Work.Store(kParVT + u * 4u, vt);
    if (!kept) Cnt.InterlockedAdd(194u * 4u, 1u, old);
}
// (v0.10.0 phase 14 round 4: one more C++ raw-string split here, MSVC's 16380-byte literal cap): )" R"(
// v0.10.0 phase 14 round 4 PLATE HOLD (FoldU.w bit 0 = the previous pass's table UHoldR is valid): a listed draw left WITHOUT a
// parent this pass, or with only the pseudo static source (its parent's pixels may simply be missing: a skipped / occluded body),
// keeps last pass's MOVING parent for up to kHoldMax frames in a row. Identity across passes (plates have no key / matrix of their
// own): the same IndexCount, about the same pixel count, and its pixel centroid carried back by the parent's own R lands within
// kHoldPx of last pass's centroid (it moved WITH that parent: a static clump beside a plate does not match); the parent's previous
// index maps to this pass through its mutual pair (kBestPrev + kPartner) and must still be a mover. Every slot then writes its
// result to UHoldW (the next pass's UHoldR).
void ParHold(uint s) {
    uint nl = Work.Load(kParU + 256u);
    uint4 ent = uint4(0u, 0u, kNone, 0u);
    if (s < nl) {
        uint u = Work.Load(kParU + s * 4u);
        uint3 sum = Work.Load3(kParSum + u * 12u);
        uint pix = sum.z;
        uint ic = Tab[u * 2u + 1u].z & 0xFFFFFFu;
        float2 cr = ParCentroid(sum);
        int2 ci = int2(cr + 0.5);
        float2 cf = max(cr - ParJit(u), float2(0.0, 0.0));   // jitter-free (stored and compared)
        uint fin = Work.Load(kParent + u * 4u);
        uint age = 0u;
        bool kept = Work.Load(kTwin + u * 4u) != kNone || Work.Load(kAttach + u * 4u) != kNone;
        // round 6: only a 1-instance draw is ever held (a multi-instance draw is not listed either, see ParCountable), and only when
        // exactly ONE entry of last pass matches it (two candidates = no identity: no hold)
        bool one = (Tab[u * 2u].z & kFInst) == 0u || (Tab[u * 2u + 1u].z >> 24) <= 1u || (FoldU.w & 4u) != 0u;
        if ((FoldU.w & 1u) != 0u && one && !kept && pix != 0u && (fin == kNone || fin == kParPseudo) && Counts.y != 0u &&
            ci.x < (int)Params.z && ci.y < (int)Params.w) {
            uint nMatch = 0u, mPar = kNone, mAge = 0u;
            // this draw's centroid (pixel centre, ndc) and its depth there (its own surface: G-buffer or forward depth)
            bool fwdU = (Tab[u * 2u].z & kFFwd) != 0u;
            float dc = fwdU ? FwdDepthAt(ci) : PDepth[ci];
            float2 ndc = float2((cf.x + 0.5) / Params.z * 2.0 - 1.0, 1.0 - (cf.y + 0.5) / Params.w * 2.0);
            for (uint k = 0u; k < kParMaxU; ++k) {
                uint4 h = UHoldR[k];
                uint hic = h.x & 0xFFFFFFu, hage = h.x >> 24;
                if (hic == 0u || hic != ic || h.z == kNone || h.z == kParPseudo || h.z >= Counts.y || hage >= kHoldMax) continue;
                if (h.w * 2u < pix || pix * 2u < h.w) continue;            // about the same size on screen
                uint i2 = Work.Load(kBestPrev + h.z * 16u);
                if (i2 >= Counts.w || Work.Load(kPartner + i2 * 4u) != h.z || ParState(i2) != 3u) continue;
                // the SAME draw as last pass's entry: its centroid carried back by the held parent's motion lands on the stored one
                // (a plate moves with its body; a static clump next to it does not)
                float4 inf = RTab[i2 * 5u + 4u];
                float zd = saturate((dc - inf.y) / max(inf.z - inf.y, 1e-6));
                float4x4 Rp = float4x4(RTab[i2 * 5u], RTab[i2 * 5u + 1u], RTab[i2 * 5u + 2u], RTab[i2 * 5u + 3u]);
                float4 c = mul(Rp, float4(ndc, zd, 1.0));
                if (!(c.w > 1e-6)) continue;
                float px = (c.x / c.w * 0.5 + 0.5) * Params.z - 0.5, py = (0.5 - c.y / c.w * 0.5) * Params.w - 0.5;
                float2 hc = ParUnpackC(h.y);
                float dM = length(float2(px, py) - hc);
                if (dM > kHoldPx) continue;
                // ... and closer to it than to where the CAMERA motion puts it (a static clump the vote once gave a passing car's
                // motion must not keep it: the two predictions are only ~1 px apart there)
                float4 iu = RTab[u * 5u + 4u];
                float2 ps = ParBackPx(LayerR(Tab[u * 2u].z), ndc, saturate((dc - iu.y) / max(iu.z - iu.y, 1e-6)));
                if (length(ps - float2(px, py)) >= kTempPx && !(dM < length(ps - hc))) continue;   // (distinguishable only)
                ++nMatch; mPar = i2; mAge = hage;
            }
            if (nMatch == 1u) {
                ParApply(u, mPar, Work.Load(kParVT + u * 4u) | kHoldBit);
                fin = mPar; age = mAge + 1u;
                uint ho;
                Cnt.InterlockedAdd(211u * 4u, 1u, ho);
            }
            nMatch = 0u; mAge = 0u;
            // no moving parent to keep and none this pass: a STATIC parent of last pass is kept the same way (identity = its centroid
            // carried back by the camera motion) -- the motion is the camera R either way, only the Ctrl+F6 label stays steady
            if (fin == kNone) {
                float4 iu = RTab[u * 5u + 4u];
                float2 ps = ParBackPx(LayerR(Tab[u * 2u].z), ndc, saturate((dc - iu.y) / max(iu.z - iu.y, 1e-6)));
                for (uint k2 = 0u; k2 < kParMaxU; ++k2) {
                    uint4 h = UHoldR[k2];
                    uint hic = h.x & 0xFFFFFFu, hage = h.x >> 24;
                    if (hic == 0u || hic != ic || h.z == kNone || hage >= kHoldMax || h.w * 2u < pix || pix * 2u < h.w) continue;
                    bool stat = h.z == kParPseudo;
                    if (!stat && h.z < Counts.y) {
                        uint i3 = Work.Load(kBestPrev + h.z * 16u);
                        stat = i3 < Counts.w && Work.Load(kPartner + i3 * 4u) == h.z && ParState(i3) == 2u;
                    }
                    if (!stat || length(ps - ParUnpackC(h.y)) > kHoldPx) continue;
                    ++nMatch; mAge = hage;
                }
                if (nMatch == 1u) {
                    ParApply(u, kParPseudo, Work.Load(kParVT + u * 4u) | kHoldBit);
                    fin = kParPseudo; age = mAge + 1u;
                    uint ho2;
                    Cnt.InterlockedAdd(211u * 4u, 1u, ho2);
                }
            }
        }
        // (IndexCount | held age << 24, centroid, final parent, pixel count)
        ent = uint4(ic | (min(age, 255u) << 24), ParPackC(cf), fin, pix);
    }
    UHoldW[s] = ent;
}
[numthreads(64, 1, 1)]
void CSParentPick2(uint3 tid : SV_DispatchThreadID) {
    ParPick2Slot(tid.x);
    DeviceMemoryBarrierWithGroupSync();
    ParHold(tid.x);                                     // (unbound tables: reads 0 = no entry, writes dropped)
}

// v0.10.0 phase 9 folded dispatches (see the C++ header; a sixth C++ raw string starts here, MSVC's literal cap): )" R"(
groupshared float4 gSolveR[8];
[numthreads(256, 1, 1)]
void CSPickResolve(uint gi : SV_GroupIndex) {
    if ((FoldU.x & 1u) != 0u && gi < 2u) PickLayer(gi);
    DeviceMemoryBarrierWithGroupSync();
    if (gi < 8u) gSolveR[gi] = SolveRW[gi];
    GroupMemoryBarrierWithGroupSync();
    float4x4 Rw = float4x4(gSolveR[0], gSolveR[1], gSolveR[2], gSolveR[3]);
    float4x4 Rc = float4x4(gSolveR[4], gSolveR[5], gSolveR[6], gSolveR[7]);
    for (uint i = gi; i < Counts.x; i += 256u) ResolveDraw(i, (Tab[i * 2u].z & kFCabin) != 0u ? Rc : Rw);
    DeviceMemoryBarrierWithGroupSync();
    if ((FoldU.x & 2u) != 0u)
        for (uint i2 = gi; i2 < Counts.x; i2 += 256u) Inherit1Draw(i2);
}
[numthreads(256, 1, 1)]
void CSListTwin(uint gi : SV_GroupIndex) {
    if ((FoldU.x & 4u) != 0u) ParentListGroup(gi);
    DeviceMemoryBarrierWithGroupSync();
    if ((FoldU.x & 8u) != 0u)
        for (uint i = gi; i < Counts.x; i += 256u) TwinDraw(i);
    DeviceMemoryBarrierWithGroupSync();
    if ((FoldU.x & 16u) != 0u)
        for (uint i2 = gi; i2 < Counts.x; i2 += 256u) AttachDraw(i2);
}
)";
// kDrawIdCs-END

// ---- device objects shared by every DrawIdRecord ---------------------------------------------------------------------
ComPtr<ID3D11PixelShader>        s_ps;
ComPtr<ID3D11DepthStencilState>  s_dss;
ComPtr<ID3D11Buffer>             s_idCb;
ComPtr<ID3D11BlendState>         s_bsR, s_bsG;            // v0.10.0 phase 3: write RED (G-buffer ids) / GREEN (forward ids)
const void*                      s_dev = nullptr;          // identity of the device the objects belong to
// v0.10.0 phase 10: forward-depth occlusion init (InitFwdDepth): own device identity (the ids may be off)
ComPtr<ID3D11VertexShader>       s_initVs;
ComPtr<ID3D11PixelShader>        s_initPs[2];
ComPtr<ID3D11DepthStencilState>  s_initDss;               // depth ALWAYS, write ALL, stencil off
ComPtr<ID3D11RasterizerState>    s_initRs;                // cull none, scissor on
const void*                      s_initDev = nullptr;
bool                             s_initFailed = false;
bool                             s_devFailed = false;

// Everything the replay touches, saved before and restored after (refs held in between).
struct GfxSave {
    static constexpr int kVb = DrawIdRecord::kVb, kCb = DrawIdRecord::kVsCb, kSrv = DrawIdRecord::kVsSrv,
                         kSmp = DrawIdRecord::kVsSmp;
    static constexpr UINT kVpMax = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    bool targets = false;
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11DepthStencilState* dss = nullptr; UINT ref = 0;
    ID3D11BlendState* bs = nullptr; FLOAT bf[4] = {}; UINT mask = 0xFFFFFFFFu;
    ID3D11InputLayout* il = nullptr;
    ID3D11Buffer* vb[kVb] = {}; UINT vbs[kVb] = {}, vbo[kVb] = {};
    ID3D11Buffer* ib = nullptr; DXGI_FORMAT ibf = DXGI_FORMAT_UNKNOWN; UINT ibo = 0;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs = nullptr;
    ID3D11Buffer* cb[kCb] = {}; UINT cbf[kCb] = {}, cbn[kCb] = {};
    ID3D11ShaderResourceView* srv[kSrv] = {};
    ID3D11SamplerState* smp[kSmp] = {};
    ID3D11PixelShader* ps = nullptr;
    ID3D11Buffer* pcb = nullptr; UINT pcbf = 0, pcbn = 0;
    // v0.10.0 phase 10 (psAll: the forward-depth batch / init bind the game's PS resources): every PS slot they can set
    static constexpr int kPCb = DrawIdRecord::kPsCb, kPSrv = DrawIdRecord::kPsSrv, kPSmp = DrawIdRecord::kPsSmp;
    bool psAll = false;
    ID3D11Buffer* pcbA[kPCb] = {}; UINT pcbAf[kPCb] = {}, pcbAn[kPCb] = {};
    ID3D11ShaderResourceView* psrv[kPSrv] = {};
    ID3D11SamplerState* psmp[kPSmp] = {};
    ID3D11GeometryShader* gs = nullptr; ID3D11HullShader* hs = nullptr; ID3D11DomainShader* ds = nullptr;
    ID3D11RasterizerState* rs = nullptr;
    UINT nVp = 0; D3D11_VIEWPORT vp[kVpMax] = {};
    UINT nSc = 0; D3D11_RECT sc[kVpMax] = {};

    void Save(ID3D11DeviceContext1* c, bool withTargets, bool allPs = false) {
        targets = withTargets;
        psAll = allPs;
        if (psAll) {
            c->PSGetConstantBuffers1(0, kPCb, pcbA, pcbAf, pcbAn);
            c->PSGetShaderResources(0, kPSrv, psrv);
            c->PSGetSamplers(0, kPSmp, psmp);
        }
        if (targets) c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        c->OMGetDepthStencilState(&dss, &ref);
        c->OMGetBlendState(&bs, bf, &mask);
        c->IAGetInputLayout(&il);
        c->IAGetVertexBuffers(0, kVb, vb, vbs, vbo);
        c->IAGetIndexBuffer(&ib, &ibf, &ibo);
        c->IAGetPrimitiveTopology(&topo);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->VSGetConstantBuffers1(0, kCb, cb, cbf, cbn);
        c->VSGetShaderResources(0, kSrv, srv);
        c->VSGetSamplers(0, kSmp, smp);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->PSGetConstantBuffers1(0, 1, &pcb, &pcbf, &pcbn);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->RSGetState(&rs);
        nVp = kVpMax; c->RSGetViewports(&nVp, vp); if (nVp > kVpMax) nVp = kVpMax;
        nSc = kVpMax; c->RSGetScissorRects(&nSc, sc); if (nSc > kVpMax) nSc = kVpMax;
    }
    void Restore(ID3D11DeviceContext1* c) {
        if (targets) c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        c->OMSetDepthStencilState(dss, ref);
        c->OMSetBlendState(bs, bf, mask);
        c->IASetInputLayout(il);
        c->IASetVertexBuffers(0, kVb, vb, vbs, vbo);
        c->IASetIndexBuffer(ib, ibf, ibo);
        c->IASetPrimitiveTopology(topo);
        c->VSSetShader(vs, nullptr, 0);
        for (int k = 0; k < kCb; ++k) if (!cb[k] || cbn[k] == 0) { cbf[k] = 0; cbn[k] = 4096; }
        c->VSSetConstantBuffers1(0, kCb, cb, cbf, cbn);
        c->VSSetShaderResources(0, kSrv, srv);
        c->VSSetSamplers(0, kSmp, smp);
        c->PSSetShader(ps, nullptr, 0);
        if (!pcb || pcbn == 0) { pcbf = 0; pcbn = 4096; }
        c->PSSetConstantBuffers1(0, 1, &pcb, &pcbf, &pcbn);
        if (psAll) {
            for (int k = 0; k < kPCb; ++k) if (!pcbA[k] || pcbAn[k] == 0) { pcbAf[k] = 0; pcbAn[k] = 4096; }
            c->PSSetConstantBuffers1(0, kPCb, pcbA, pcbAf, pcbAn);
            c->PSSetShaderResources(0, kPSrv, psrv);
            c->PSSetSamplers(0, kPSmp, psmp);
        }
        c->GSSetShader(gs, nullptr, 0);
        c->HSSetShader(hs, nullptr, 0);
        c->DSSetShader(ds, nullptr, 0);
        c->RSSetState(rs);
        c->RSSetViewports(nVp, nVp ? vp : nullptr);
        c->RSSetScissorRects(nSc, nSc ? sc : nullptr);
        Release();
    }
    template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
    void Release() {
        for (auto& p : rtv) Rel(p);
        Rel(dsv); Rel(dss); Rel(bs); Rel(il);
        for (auto& p : vb) Rel(p);
        Rel(ib); Rel(vs);
        for (auto& p : cb) Rel(p);
        for (auto& p : srv) Rel(p);
        for (auto& p : smp) Rel(p);
        Rel(ps); Rel(pcb); Rel(gs); Rel(hs); Rel(ds); Rel(rs);
        for (auto& p : pcbA) Rel(p);
        for (auto& p : psrv) Rel(p);
        for (auto& p : psmp) Rel(p);
    }
};

float g_maxM = 8.0f;          // dlaa.ini mv_drawid_max_m
float g_snapPx = 0.1f;        // dlaa.ini mv_drawid_snap_px
float g_trackM = 0.25f;       // track continuity: displacement change allowed per frame (clip units ~ m), + 25 %
bool  g_consensus = true;     // v0.10.0 phase 3: dlaa.ini mv_camr_consensus
bool  g_consLogged = false;   // one-time "MV camera R: consensus" line
float g_attachM = 0.5f;       // v0.10.0 phase 4: dlaa.ini mv_fwd_attach_m (0 = no attach, per draw and per pixel)
bool  g_twin = true;          // v0.10.0 phase 5: dlaa.ini mv_drawid_twin (unpaired draws inherit their matrix twin's R)
bool  g_parent = true;        // v0.10.0 phase 6: dlaa.ini mv_drawid_parent (unpaired draws take their rigid parent's R)
int   g_parentMut = 0;        // v0.10.0 phase 6: harness mutations only (bit 0 conjugated formula, bit 1 no depth test; phase 14
                              // round 6: bit 2 origin-free rule off, bit 3 multi-instance draws listed / held again)
bool  g_instancedVote = false;// v0.10.0 phase 6b: replayed instanced / no-MVP draws take part in the rigid-parent vote
                              // (dlaa.ini mv_drawid_instanced; set by DrawIdMv::SetInstanced -- in game it follows the ini key,
                              // the harness turns it on for S8 only so S1-S7 are unchanged)
// v0.10.0 phase 6 vote rules: relative linear-depth tolerance, smallest winning vote count / share, vote cap per unpaired draw,
// grid stride (px; 2 = a quarter of the pixels). Phase 6c (marching vote): 4 % / 8 votes / 50 % (was 2 % / 32 / 60 % for the
// +-2 / +-5 px neighbour samples); the stride is also the march step; march reach (px per axis) and the march budget per
// listed draw and pass (marches; ParDecim thins the grid of a larger draw)
constexpr float kParDepthTol = 0.04f, kParMinVotes = 8.0f, kParMinShare = 0.5f, kParVoteCap = 65535.0f;
constexpr UINT  kParStride = 2u, kParReachPx = 32u, kParMarchBudget = 8192u;
// v0.10.0 phase 6c: edge continuity -- a source pixel must also lie within 1.5 % (relative linear depth) of the last pixel of the
// patch before it (2 px away): an attached part is flush with its parent; a static object standing 0.3 m in front of a vehicle
// (2 % at 13 m: inside the 4 % gate) is not
constexpr float kParEdgeTol = 0.015f;
unsigned g_parReach = 0;      // v0.10.0 phase 6c: harness-only march reach override (0 = kParReachPx)
// v0.10.0 phase 8 performance (dlaa.ini; the fast setting is the default, the old behaviour stays one key away):
//   mv_vote_res        4 = the rigid-parent vote / pixel count sample every 4th pixel in x / y and march in 4 px steps; listed
//                      draws with fewer than kParFineSamples counted samples (far plates, grass) keep every 2nd pixel + 2 px
//                      steps (CSParentVote); 2 = every 2nd pixel, 2 px steps for every draw (phase 6c)
//   mv_vote_march_cap  marches per frame over all listed draws (5/6 for the 1st pass, 1/6 for the 2nd); 0 = no frame cap
//                      (kParMarchBudget per listed draw and pass, phase 6c)
//   mv_cons_cands      consensus candidates of the world layer (the cabin layer takes half, <= 64); 128 = phase 3
UINT  g_voteRes = 4u;
UINT  g_marchCap = 48000u;
UINT  g_consCands = 64u;
//   mv_medoid_drop     a layer whose consensus was used (>= kConsMin draws) in this many landed readbacks in a row drops the old
//                      medoid path (camera candidate copies + pass A); 0 = never (phase 7)
UINT  g_medoidDrop = 120u;
//   mv_inst_replay     0 = every instanced / no-MVP draw replayed every pass; 1 = only keys that took a moving parent lately
int      g_instReplay = 0;
//   mv_parent_max_listed (v0.10.0 phase 10) unpaired / instanced draws listed per pass for the rigid-parent vote (8..64)
UINT     g_parMaxListed = 64u;
bool     g_instProbe = true;           // this replay replays every instanced draw (set per replay by the caller)
uint64_t g_instFrame = 0;              // Present counter (SetFrame)
constexpr uint64_t kInstKeepFrames = 240;   // a key stays "wanted" this many frames after its last moving parent
constexpr int kInstTab = 2048;         // open addressing, key 0 = empty
uint64_t g_instKey[kInstTab] = {};
uint64_t g_instSeen[kInstTab] = {};
int      g_instKnown = 0;
void InstMark(uint64_t key) {
    if (!key) key = 1;
    uint32_t h = (uint32_t)(key ^ (key >> 29)) & (kInstTab - 1);
    for (int p = 0; p < 64; ++p, h = (h + 1) & (kInstTab - 1)) {
        if (g_instKey[h] == key) { g_instSeen[h] = g_instFrame; return; }
        // an empty slot or one that expired long ago takes the key
        if (!g_instKey[h] || g_instSeen[h] + 4 * kInstKeepFrames < g_instFrame) {
            if (!g_instKey[h]) ++g_instKnown;
            g_instKey[h] = key; g_instSeen[h] = g_instFrame;
            return;
        }
    }
}
// v0.10.0 phase 14 STATIC REPLAY SKIP (draw_ids.h step 11): one table for every main unit (both VR eyes feed it; the eyes draw
// the same meshes). Per FullKey: the static streak (frames in a row whose readback had draws of the key paired + static by their
// own pair and none moving; draws without a partner are neutral), the frame of its last verdict, the frame of its last increment (one per frame: two eyes do not count twice), the
// last frame it was a rigid parent. Open addressing (key 0 = empty), no deletion: a slot not seen for kStatExpire frames is reused.
//   mv_replay_static_frames  0 = off; streak needed before a key's replay is skipped (default 6)
//   mv_replay_static_every   a skipped key is still replayed on 1 frame of every `every` (default 4; 1 = never skipped)
unsigned g_statFrames = 6u, g_statEvery = 4u;
//   mv_replay_static_eps_px  (round 3) only a DEEP static draw builds a streak: its own motion within this many px (full image) of
//                            the camera's -- world geometry moves with the camera exactly (float noise); a vehicle driving at the
//                            player's speed is "static" within the 0.1 px snap but wobbles: it is never skipped (default 0.02)
float    g_statEps = 0.02f;
bool     g_staticGate = false;               // DrawIdRecord::SetStaticGate (set per replay by the caller)
uint64_t g_testSkipKey = 0;                  // round 4, HARNESS ONLY: DrawIdRecord::SetTestSkipKey
constexpr uint64_t kStatFresh = 16;         // the last verdict must be at most this many frames old (readbacks stalled: replay)
constexpr uint64_t kStatParentHold = 30;     // a key that was a rigid parent within this many frames is always replayed
constexpr uint64_t kStatQuiet = 60;          // round 3: a key that MOVED (or was only shallow-static) within this many frames is
                                             // never skipped, whatever its streak (vehicles wobble; buildings do not)
constexpr uint64_t kStatExpire = 600;        // a slot not seen for this many frames may take another key
constexpr int      kStatTab = 8192;          // >= 2x the draws of a pass (kMax 4096)
// badTag: the readback that reset it; moved: the last frame + 1 a draw of the key moved / was only shallow-static (0 = never)
struct StatEnt { uint64_t key, seen, inc, parent, moved; uint32_t streak, badTag; };
StatEnt  g_stat[kStatTab] = {};
uint32_t g_statTag = 0;                      // one per fed readback (the "this key had a non-static draw in it" mark)
int      g_statUsed = 0;                     // slots taken since the last clear
uint64_t g_statClears[DrawIdMv::kStatClrN] = {};
constexpr int kStatUnits = 8;                // main units registered for DrawIdMv::PollMain (flat: 1, VR: 2; harness: a few)
DrawIdMv* g_statUnit[kStatUnits] = {};
StatEnt* StatFind(uint64_t key, bool insert) {
    if (!key) key = 1;
    uint32_t h = (uint32_t)(key ^ (key >> 29)) & (kStatTab - 1);
    for (int p = 0; p < 32; ++p, h = (h + 1) & (kStatTab - 1)) {
        StatEnt& e = g_stat[h];
        if (e.key == key) return &e;
        if (!e.key) {
            if (!insert) return nullptr;
            e = StatEnt{}; e.key = key;
            ++g_statUsed;
            return &e;
        }
        if (insert && e.seen + kStatExpire < g_instFrame && e.parent + kStatExpire < g_instFrame) {   // expired: reused
            e = StatEnt{}; e.key = key;
            return &e;
        }
    }
    return nullptr;                                      // probe chain full: the key is never skipped (always replayed)
}
constexpr float kParFineSamples = 64.0f;   // < 64 coarse samples (~1000 px at mv_vote_res 4) = a FINE listed draw
// v0.10.0 phase 9: dlaa.ini mv_fold_dispatch (1 = CSPickResolve / CSListTwin, 0 = the phase-8 chain of separate dispatches)
// v0.10.0 phase 18: 2 (default) = CSListTwin + CSPickPar + the wide CSResolveInherit (see the header)
int   g_foldMode = 2;
// v0.10.0 phase 18: dlaa.ini mv_vote_compact (1 = the rigid-parent vote over the listed tiles, one thread per march direction)
bool  g_voteCompact = true;
// v0.10.0 phase 18: dlaa.ini mv_cons_min_cabin (0 = kConsMin, as the world layer; 8..64): the cabin layer's consensus needs this many
// agreeing draws -- below kConsMin it can be used (and its medoid / candidate copies dropped after mv_medoid_drop healthy readbacks)
UINT  g_consMinCabin = 0u;
constexpr UINT kParTileCap = 65536u, kParItemCap = 131072u;   // = the shader's kTileCap / kItemCap
constexpr UINT kParListBytes = kParTileCap * 4u + kParItemCap * 16u;
// v0.10.0 phase 9: scissor-enabled copies of the game's rasterizer states (DrawIdRecord::ScissorRs). Key ref held (no pointer
// reuse aliasing), copy ref held; dropped with the device objects (ShutdownDevice). A full table = no clip for new states.
struct RsMemo { ID3D11RasterizerState* game; ID3D11RasterizerState* sc; bool had; };
constexpr int kRsMemo = 64;
RsMemo   g_rsMemo[kRsMemo] = {};
int      g_rsMemoN = 0, g_rsMemoLast = -1;
const void* g_rsMemoDev = nullptr;
uint64_t g_rsFails = 0;
void RsMemoClear() {
    for (int i = 0; i < g_rsMemoN; ++i) {
        if (g_rsMemo[i].game) g_rsMemo[i].game->Release();
        if (g_rsMemo[i].sc && g_rsMemo[i].sc != g_rsMemo[i].game) g_rsMemo[i].sc->Release();
        g_rsMemo[i] = RsMemo{};
    }
    g_rsMemoN = 0; g_rsMemoLast = -1; g_rsMemoDev = nullptr;
}

// v0.10.0 phase 3 buffer sizes: DidCB = Counts, Params, Params2, Cons + 48 candidate uint4; Work = P, best matches, R_draw,
// partner, votes (see kDrawIdCs); Cnt = 256 dwords (state counts, consensus, pass B G-buffer + forward pixel counters)
// v0.10.0 phase 6: + Par, ParU; phase 6c: + Par2
constexpr UINT kCbBytes = 64u + 48u * 16u + 96u;     // v0.10.0 phase 9: + Clip, FwdMode, FoldU
// v0.10.0 phase 4: + attach index per draw; phase 5: + twin index per draw, the 2n-slot MVP hash table, the mover list;
// phase 6: + U slot / parent / votes per draw, the U list and 64 vote tables; phase 6b: + the instanced pixel count per draw;
// phase 6c: the vote tables grow to 64 x 192 B (kParTab = kMax * 200 + 8704, end kMax * 200 + 20992)
// phase 14 round 4: + the per-draw centroid sums (kParSum = kMax * 200 + 24576, kMax * 12 B)
constexpr UINT kWorkBytes = (UINT)DrawIdRecord::kMax * 212u + 24576u;
constexpr UINT kCntBytes = 3328u;               // v0.10.0 phase 8: + dwords 256..383 = instanced draws with a moving parent
                                                 // v0.10.0 phase 18: + dwords 768..772 (compact vote: tiles, items, overflows)
                                                // phase 14: + 384..511 own-pair static G-buffer draws, 512..639 rigid parents,
                                                // 640..767 draws that move (a mover by any route / a track reject)
static_assert(256u + (UINT)DrawIdRecord::kMax / 32u <= 384u, "instanced mover bitmask");
static_assert(640u + (UINT)DrawIdRecord::kMax / 32u <= kCntBytes / 4u && (UINT)DrawIdRecord::kMax / 32u == 128u,
              "v0.10.0 phase 14: static / rigid-parent / reset bitmasks (the shader's 384 / 512 / 640 bases)");
static_assert(DrawIdMv::kCandWorld + DrawIdMv::kCandCabin <= 48 * 4, "candidate table");
static_assert((DrawIdMv::kCandWorld + DrawIdMv::kCandCabin) * 16 <= 4096, "vote table");
static_assert(8704u + 64u * 192u <= 24576u, "v0.10.0 phase 6c: 64 vote tables of 192 B (kParTab / kParTabB) fit in Work");

bool CreateCsFromCache(ID3D11Device* dev, ShaderCache::Id id, ID3D11ComputeShader** out) {
    size_t size = 0;
    const void* code = ShaderCache::Code(id, &size);
    if (!code) { Log("MV draw-ids: %s compile failed %s", ShaderCache::Name(id), ShaderCache::Error(id)); return false; }
    const HRESULT hr = dev->CreateComputeShader(code, size, nullptr, out);
    if (FAILED(hr)) { Log("MV draw-ids: %s CreateComputeShader hr=0x%lx", ShaderCache::Name(id), (unsigned long)hr); return false; }
    return true;
}

bool MakeBuf(ID3D11Device* dev, UINT bytes, UINT bind, UINT misc, UINT stride, bool dynamic, ID3D11Buffer** out) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
    bd.BindFlags = bind;
    bd.CPUAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;
    bd.MiscFlags = misc;
    bd.StructureByteStride = stride;
    return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, out));
}

}  // namespace

// =====================================================================================================================
// GpuSpanTimer
// =====================================================================================================================
void GpuSpanTimer::Poll(ID3D11DeviceContext* ctx) {
    for (Q& s : m_q) {
        if (!s.inFlight) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
        if (ctx->GetData(s.dis.Get(), &dd, sizeof(dd), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 a = 0, b = 0;
        if (ctx->GetData(s.t0.Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            ctx->GetData(s.t1.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        s.inFlight = false;
        if (dd.Disjoint || !dd.Frequency || b < a) continue;
        const double ms = (double)(b - a) * 1000.0 / (double)dd.Frequency;
        m_sum += ms;
        if (ms > m_max) m_max = ms;
        ++m_n;
    }
}

int GpuSpanTimer::Begin(ID3D11DeviceContext* ctx) {
    if (m_broken || !ctx) return -1;
    Poll(ctx);
    Q& s = m_q[m_head];
    if (s.inFlight) return -1;
    if (!s.dis) {
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        if (!dev || FAILED(dev->CreateQuery(&qd, &s.dis))) { m_broken = true; return -1; }
        qd.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(dev->CreateQuery(&qd, &s.t0)) || FAILED(dev->CreateQuery(&qd, &s.t1))) { m_broken = true; return -1; }
    }
    ctx->Begin(s.dis.Get());
    ctx->End(s.t0.Get());
    return m_head;
}

void GpuSpanTimer::End(ID3D11DeviceContext* ctx, int slot) {
    if (slot < 0 || slot >= kQ || !ctx) return;
    ctx->End(m_q[slot].t1.Get());
    ctx->End(m_q[slot].dis.Get());
    m_q[slot].inFlight = true;
    m_head = (slot + 1) % kQ;
}

void GpuSpanTimer::Take(double* avgMs, double* maxMs, uint64_t* n) {
    if (avgMs) *avgMs = m_n ? m_sum / (double)m_n : 0.0;
    if (maxMs) *maxMs = m_max;
    if (n) *n = m_n;
    m_sum = 0.0; m_max = 0.0; m_n = 0;
}

void GpuSpanTimer::Reset() {
    for (Q& s : m_q) { s.dis.Reset(); s.t0.Reset(); s.t1.Reset(); s.inFlight = false; }
    m_head = 0; m_broken = false; m_sum = 0.0; m_max = 0.0; m_n = 0;
}

// =====================================================================================================================
// DrawIdRecord
// =====================================================================================================================
struct DrawIdRecord::State {
    ID3D11InputLayout* il;
    ID3D11Buffer* vb[kVb]; UINT vbStride[kVb], vbOff[kVb];
    ID3D11Buffer* ib; DXGI_FORMAT ibFmt; UINT ibOff;
    D3D11_PRIMITIVE_TOPOLOGY topo;
    ID3D11VertexShader* vs;
    ID3D11Buffer* cb[kVsCb]; UINT cbFirst[kVsCb], cbNum[kVsCb];
    ID3D11ShaderResourceView* srv[kVsSrv];
    ID3D11SamplerState* smp[kVsSmp];
    ID3D11RasterizerState* rs;
    UINT nVp, nSc;
    D3D11_VIEWPORT vp[kVp];
    D3D11_RECT sc[kSc];
    UINT ic, inst, si, sinst;
    INT  bv;
    bool instanced;
};

// v0.10.0 phase 10: the pixel-shader state of a forward draw (SetCapturePs), for the batched forward-depth re-draw
struct DrawIdRecord::PsState {
    ID3D11PixelShader* ps;
    ID3D11Buffer* cb[kPsCb]; UINT cbFirst[kPsCb], cbNum[kPsCb];
    ID3D11ShaderResourceView* srv[kPsSrv];
    ID3D11SamplerState* smp[kPsSmp];
};

void DrawIdRecord::ReleasePs(int from, int to) {
    if (!m_ps) return;
    for (int i = from; i < to; ++i) {
        PsState& p = m_ps[i];
        if (p.ps) p.ps->Release();
        for (auto* x : p.cb) if (x) x->Release();
        for (auto* x : p.srv) if (x) x->Release();
        for (auto* x : p.smp) if (x) x->Release();
        memset(&p, 0, sizeof(p));
    }
}

DrawIdRecord::DrawIdRecord() {}
// No COM Release here: the global records die at DLL unload, possibly after the game's device (the refs are the game's
// objects). Owners that outlive nothing (the WARP harness) call Shutdown() before releasing their device.
DrawIdRecord::~DrawIdRecord() {
    delete[] m_st; m_st = nullptr;
    delete[] m_key; m_key = nullptr;
    delete[] m_ps; m_ps = nullptr;                       // v0.10.0 phase 10
}

void DrawIdRecord::ReleaseRange(int from, int to) {
    if (!m_st) return;
    for (int i = from; i < to; ++i) {
        State& s = m_st[i];
        if (s.il) s.il->Release();
        for (auto* p : s.vb) if (p) p->Release();
        if (s.ib) s.ib->Release();
        if (s.vs) s.vs->Release();
        for (auto* p : s.cb) if (p) p->Release();
        for (auto* p : s.srv) if (p) p->Release();
        for (auto* p : s.smp) if (p) p->Release();
        if (s.rs) s.rs->Release();
        memset(&s, 0, sizeof(s));
    }
    ReleasePs(from, to);                                 // v0.10.0 phase 10
}

void DrawIdRecord::Reset() {
    ReleaseRange(m_releasedTo, m_n);
    for (int k = 0; k < m_nRings; ++k) if (m_rings[k].buf) m_rings[k].buf->Release();
    for (Ring& r : m_rings) r = Ring{};
    m_nRings = 0; m_lastRing = -1; m_seals = 0; m_need = 0; m_copiedAny = false; m_flushFail = false;
    m_otherBuf = nullptr;
    m_n = 0; m_replayedTo = 0; m_releasedTo = 0;
    m_full = 0; m_nInst = 0; m_nNoMvp = 0;
    m_cleared = false; m_replayFail = false;
    m_segments = 0; m_replayed = 0; m_skipped = 0;
    m_idBase = kNone; m_overflow = 0;
    m_instGated = 0;                                     // v0.10.0 phase 8
    if (m_staticSkipped) memset(m_staticBits, 0, sizeof(m_staticBits));   // v0.10.0 phase 14
    m_staticSkipped = 0; m_staticGated = false;
    m_depthTo = 0;                                       // v0.10.0 phase 10
    m_clip = false; m_clipRect = D3D11_RECT{ 0, 0, 0, 0 }; m_shift = 0;   // v0.10.0 phase 9 (set again before each replay)
    if (++m_gen == 0) { memset(m_resGen, 0, sizeof(m_resGen)); m_gen = 1; }
    m_resFill = 0;
    memset(m_lastRes, 0, sizeof(m_lastRes));
    memset(m_srvKey, 0, sizeof(m_srvKey));
    memset(m_srvRes, 0, sizeof(m_srvRes));
    m_srvNext = 0;
}

void DrawIdRecord::Shutdown() {
    Reset();
    m_mirrorSrv.Reset(); m_mirror.Reset(); m_mirrorBytes = 0;
}

void DrawIdRecord::NoteRes(const void* p) {
    if (!p) return;
    if (m_resFill >= kResSet * 3 / 4) return;            // full: ReferencesPending answers "yes" (conservative)
    uint32_t h = (uint32_t)((Mix((uint64_t)(uintptr_t)p) >> 40) & (kResSet - 1));
    for (;; h = (h + 1) & (kResSet - 1)) {
        if (m_resGen[h] != m_gen) { m_resGen[h] = m_gen; m_res[h] = p; ++m_resFill; return; }
        if (m_res[h] == p) return;
    }
}

bool DrawIdRecord::ReferencesPending(const void* res) const {
    if (!res || PendingReplay() <= 0) return false;
    if (m_resFill >= kResSet * 3 / 4) return true;
    uint32_t h = (uint32_t)((Mix((uint64_t)(uintptr_t)res) >> 40) & (kResSet - 1));
    for (;; h = (h + 1) & (kResSet - 1)) {
        if (m_resGen[h] != m_gen) return false;
        if (m_res[h] == res) return true;
    }
}

bool DrawIdRecord::HasOpenRing(const void* res) const {
    for (int k = 0; k < m_nRings; ++k)
        if (!m_rings[k].sealed && (const void*)m_rings[k].buf == res) return true;
    return false;
}

UINT DrawIdRecord::AddRing(ID3D11Buffer* cb, UINT off) {
    int k = m_lastRing;
    if (k < 0 || m_rings[k].buf != cb || m_rings[k].sealed) {
        k = -1;
        for (int j = 0; j < m_nRings; ++j)
            if (m_rings[j].buf == cb && !m_rings[j].sealed) { k = j; break; }
        if (k < 0) {
            if ((const void*)cb == m_otherBuf) return kNone;
            D3D11_BUFFER_DESC bd{};
            cb->GetDesc(&bd);
            if (bd.ByteWidth < 65536u || m_nRings >= kRings) { m_otherBuf = cb; return kNone; }
            k = m_nRings++;
            Ring& nr = m_rings[k];
            nr = Ring{};
            nr.buf = cb;
            cb->AddRef();
            nr.bytes = bd.ByteWidth;
            nr.base = (m_need + 255u) & ~255u;           // rings back to back in the mirror, 256-byte aligned bases
            m_need = nr.base + ((bd.ByteWidth + 15u) & ~15u);
            nr.lo = 0xFFFFFFFFu; nr.hi = 0;
        }
        m_lastRing = k;
    }
    Ring& r = m_rings[k];
    if (off + 64u > r.bytes) return kNone;
    if (off < r.lo) r.lo = off;
    if (off + 64u > r.hi) r.hi = off + 64u;
    ++r.draws;
    return r.base + off;
}

bool DrawIdRecord::EnsureMirror(ID3D11DeviceContext* ctx, UINT need) {
    if (m_mirror && m_mirrorSrv && m_mirrorBytes >= need) return true;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    const UINT bytes = (need + 0xFFFFu) & ~0xFFFFu;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.NumElements = bytes / 4;
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    ComPtr<ID3D11Buffer> nb;
    ComPtr<ID3D11ShaderResourceView> nsrv;
    if (!dev || need == 0 || bytes > 64u * 1024u * 1024u ||
        !MakeBuf(dev.Get(), bytes, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, false, &nb) ||
        FAILED(dev->CreateShaderResourceView(nb.Get(), &sd, &nsrv))) {
        static int fails = 0;
        if (fails++ < 3) Log("MV draw-ids: ring mirror (%u bytes, %d ring(s)) create failed", bytes, m_nRings);
        return false;
    }
    if (m_mirror && m_copiedAny && m_mirrorBytes) {
        const D3D11_BOX box{ 0, 0, 0, m_mirrorBytes, 1, 1 };
        ctx->CopySubresourceRegion(nb.Get(), 0, 0, 0, 0, m_mirror.Get(), 0, &box);
    }
    m_mirror = nb; m_mirrorSrv = nsrv; m_mirrorBytes = bytes;
    return true;
}

bool DrawIdRecord::FlushRing(ID3D11DeviceContext* ctx, int k) {
    Ring& r = m_rings[k];
    if (r.hi <= r.lo || !r.buf) return true;
    if (!EnsureMirror(ctx, m_need)) {
        m_flushFail = true;
        for (int j = 0; j < m_nRings; ++j) { m_rings[j].lo = 0xFFFFFFFFu; m_rings[j].hi = 0; }
        return false;
    }
    const D3D11_BOX box{ r.lo, 0, 0, r.hi, 1, 1 };
    ctx->CopySubresourceRegion(m_mirror.Get(), 0, r.base + r.lo, 0, 0, r.buf, 0, &box);
    r.lo = 0xFFFFFFFFu; r.hi = 0;
    m_copiedAny = true;
    return true;
}

void DrawIdRecord::Flush(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    for (int k = 0; k < m_nRings; ++k)
        if (m_rings[k].hi > m_rings[k].lo && !FlushRing(ctx, k)) return;
}

bool DrawIdRecord::OnDiscard(ID3D11DeviceContext* ctx, ID3D11Resource* res) {
    if (!ctx || !res) return false;
    for (int k = 0; k < m_nRings; ++k) {
        Ring& r = m_rings[k];
        if (r.sealed || (ID3D11Resource*)r.buf != res) continue;
        FlushRing(ctx, k);                               // queued before the discard: reads the old contents
        if (r.draws <= 0) return false;
        r.sealed = true;                                 // later draws from this buffer (new contents) -> a new slot
        ++m_seals;
        if (m_lastRing == k) m_lastRing = -1;
        return true;
    }
    return false;
}

bool DrawIdRecord::Record(ID3D11DeviceContext1* ctx, UINT ic, UINT inst, UINT si, INT bv, UINT sinst, bool instanced) {
    if (!ctx) return false;
    if (m_n >= kMax) { ++m_full; return false; }
    if (!m_st) {
        m_st = new (std::nothrow) State[kMax];
        m_key = new (std::nothrow) Key[kMax];
        if (!m_st || !m_key) { delete[] m_st; delete[] m_key; m_st = nullptr; m_key = nullptr; ++m_full; return false; }
        memset(m_st, 0, sizeof(State) * kMax);
    }
    State& s = m_st[m_n];
    ctx->IAGetInputLayout(&s.il);
    ctx->IAGetVertexBuffers(0, kVb, s.vb, s.vbStride, s.vbOff);
    ctx->IAGetIndexBuffer(&s.ib, &s.ibFmt, &s.ibOff);
    ctx->IAGetPrimitiveTopology(&s.topo);
    ctx->VSGetShader(&s.vs, nullptr, nullptr);
    ctx->VSGetConstantBuffers1(0, kVsCb, s.cb, s.cbFirst, s.cbNum);
    ctx->VSGetShaderResources(0, kVsSrv, s.srv);
    ctx->VSGetSamplers(0, kVsSmp, s.smp);
    ctx->RSGetState(&s.rs);
    {
        D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        UINT nv = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&nv, vps);
        if (nv > (UINT)kVp) nv = kVp;
        s.nVp = nv;
        memcpy(s.vp, vps, sizeof(D3D11_VIEWPORT) * nv);
        D3D11_RECT scs[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        UINT ns = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetScissorRects(&ns, scs);
        if (ns > (UINT)kSc) ns = kSc;
        s.nSc = ns;
        memcpy(s.sc, scs, sizeof(D3D11_RECT) * ns);
    }
    s.ic = ic; s.inst = inst; s.si = si; s.sinst = sinst; s.bv = bv; s.instanced = instanced;
    // v0.10.0 phase 10: the pixel-shader state of a forward draw (the batched forward-depth re-draw needs the game's alpha)
    if (m_capPs && !m_ps && m_n == 0) {
        m_ps = new (std::nothrow) PsState[kMax];
        if (m_ps) memset(m_ps, 0, sizeof(PsState) * kMax);
    }
    if (m_capPs && m_ps) {
        PsState& p = m_ps[m_n];
        ctx->PSGetShader(&p.ps, nullptr, nullptr);
        ctx->PSGetConstantBuffers1(0, kPsCb, p.cb, p.cbFirst, p.cbNum);
        ctx->PSGetShaderResources(0, kPsSrv, p.srv);
        ctx->PSGetSamplers(0, kPsSmp, p.smp);
        // pending-resource set (hkMap): the cbuffers, and the SRVs whose view changed since this record's previous draw
        for (int t = 0; t < kPsCb; ++t) if (p.cb[t]) NoteRes(p.cb[t]);
        for (int t = 0; t < kPsSrv; ++t) {
            ID3D11ShaderResourceView* v = p.srv[t];
            if (!v || (m_n > 0 && m_ps[m_n - 1].srv[t] == v)) continue;
            ID3D11Resource* r = nullptr;
            v->GetResource(&r);
            if (r) { NoteRes(r); r->Release(); }         // identity only (the SRV ref keeps it alive)
        }
    }

    Key& k = m_key[m_n];
    k.flags = 0;
    if (instanced) { k.flags |= kFInstanced; ++m_nInst; }
    k.vpMin = s.nVp ? s.vp[0].MinDepth : 0.0f;
    k.inst = instanced ? inst : 1u;                      // v0.10.0 phase 14 round 4 (the clutter-batch rule)
    {                                                    // v0.10.0 phase 14 round 4: the viewport's sub-pixel jitter
        const float ox = s.nVp ? s.vp[0].TopLeftX : 0.0f, oy = s.nVp ? s.vp[0].TopLeftY : 0.0f;
        const float fx = ox - std::floor(ox + 0.5f), fy = oy - std::floor(oy + 0.5f);   // [-0.5, 0.5]
        const int jx = (int)std::lround(fx * 16384.0f), jy = (int)std::lround(fy * 16384.0f);
        k.jit = (UINT)(uint16_t)(int16_t)jx | ((UINT)(uint16_t)(int16_t)jy << 16);
    }
    k.vpMax = s.nVp ? s.vp[0].MaxDepth : 1.0f;
    k.ic = ic; k.cbFirst = s.cbFirst[0]; k.cbNum = s.cb[0] ? s.cbNum[0] : 0;   // v0.10.0 phase 4 (dump; State is released)
    if (k.vpMin >= 0.85f) k.flags |= kFCabin;
    if (m_segments > 0) k.flags |= kFLate;
    if (m_forward) k.flags |= kFFwd;                     // v0.10.0 phase 3
    k.off = kNone;
    if (!instanced && s.cb[0] && s.cbNum[0] * 16u >= 128u) k.off = AddRing(s.cb[0], s.cbFirst[0] * 16u + 64u);
    if (!instanced && k.off == kNone) { k.flags |= kFNoMvp; ++m_nNoMvp; }
    uint64_t f = Mix((uint64_t)(uintptr_t)s.ib + 0x9E3779B97F4A7C15ull);
    f = Mix(f ^ (uint64_t)(uintptr_t)s.vb[0]);
    f = Mix(f ^ ((uint64_t)s.ibOff | ((uint64_t)s.vbOff[0] << 32)));
    f = Mix(f ^ ((uint64_t)ic | ((uint64_t)si << 32)));
    f = Mix(f ^ ((uint64_t)(uint32_t)bv | ((uint64_t)(instanced ? 1 : 0) << 40)));
    f = Mix(f ^ (uint64_t)(uintptr_t)s.vs);
    uint64_t l = Mix(((uint64_t)ic | ((uint64_t)si << 32)) + 0x632BE59BD9B4E019ull);
    l = Mix(l ^ ((uint64_t)(uint32_t)bv | ((uint64_t)(instanced ? 1 : 0) << 40)));
    l = Mix(l ^ (uint64_t)(uintptr_t)s.vs);
    if (m_forward) { f = Mix(f ^ 0x5F0D5F0D5F0D5F0Dull); l = Mix(l ^ 0x5F0D5F0D5F0D5F0Dull); }   // forward keys of their own
    k.full = f ? f : 1;
    k.loose = l ? l : 1;
    uint64_t sh = Mix(((uint64_t)ic | ((uint64_t)inst << 32)) + 0x2545F4914F6CDD1Dull);   // v0.10.0 phase 8: ShapeKey
    sh = Mix(sh ^ (uint64_t)(uintptr_t)s.vs);
    sh = Mix(sh ^ (uint64_t)(uintptr_t)s.il);
    k.shape = sh ? sh : 1;

    // pending-resource set (hkMap): only pointers that changed since the previous draw of this pass
    for (int t = 0; t < kVb; ++t) if (s.vb[t] != m_lastRes[t]) { m_lastRes[t] = s.vb[t]; NoteRes(s.vb[t]); }
    if (s.ib != m_lastRes[4]) { m_lastRes[4] = s.ib; NoteRes(s.ib); }
    for (int t = 0; t < kVsCb; ++t) if (s.cb[t] != m_lastRes[5 + t]) { m_lastRes[5 + t] = s.cb[t]; NoteRes(s.cb[t]); }
    for (int t = 0; t < kVsSrv && 9 + t < 20; ++t) {
        ID3D11ShaderResourceView* v = s.srv[t];
        if (!v || (const void*)v == m_lastRes[9 + t]) continue;
        m_lastRes[9 + t] = v;
        const void* resPtr = nullptr;
        for (int c = 0; c < 4; ++c) if (m_srvKey[c] == v) { resPtr = m_srvRes[c]; break; }
        if (!resPtr) {
            ID3D11Resource* r = nullptr;
            v->GetResource(&r);
            resPtr = r;
            if (r) r->Release();                         // identity only (the SRV ref keeps it alive)
            m_srvKey[m_srvNext] = v; m_srvRes[m_srvNext] = resPtr; m_srvNext = (m_srvNext + 1) & 3;
        }
        NoteRes(resPtr);
    }
    ++m_n;
    return true;
}

// v0.10.0 phase 9 (see draw_ids.h SetView): the 1/2^shift viewport, the clipped / scaled scissor, the scissor-enabled RS copy
void DrawIdRecord::ScaleViewport(D3D11_VIEWPORT* vp, UINT n, UINT shift) {
    if (!vp || !shift) return;
    const float k = 1.0f / (float)(1u << shift);         // exact (a power of two): the replay reproduces the re-draw bit for bit
    for (UINT i = 0; i < n; ++i) { vp[i].TopLeftX *= k; vp[i].TopLeftY *= k; vp[i].Width *= k; vp[i].Height *= k; }
}
bool DrawIdRecord::ScaledScissor(bool rsScissor, const D3D11_RECT* own, const D3D11_RECT* clip, UINT shift, D3D11_RECT* out) {
    constexpr LONG kBig = 0x3FFFFFFF;
    D3D11_RECT r = (rsScissor && own) ? *own : D3D11_RECT{ 0, 0, kBig, kBig };
    if (clip) {
        if (clip->left > r.left) r.left = clip->left;
        if (clip->top > r.top) r.top = clip->top;
        if (clip->right < r.right) r.right = clip->right;
        if (clip->bottom < r.bottom) r.bottom = clip->bottom;
    }
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > kBig) r.right = kBig;
    if (r.bottom > kBig) r.bottom = kBig;
    if (shift) {                                         // the target pixels a full-res rect touches: floor / ceil
        const LONG add = (LONG)(1u << shift) - 1;
        r.left >>= shift; r.top >>= shift;
        r.right = (r.right + add) >> shift; r.bottom = (r.bottom + add) >> shift;
    }
    const bool any = r.left < r.right && r.top < r.bottom;
    if (!any) r = D3D11_RECT{ 0, 0, 0, 0 };
    if (out) *out = r;
    return any;
}
ID3D11RasterizerState* DrawIdRecord::ScissorRs(ID3D11Device* dev, ID3D11RasterizerState* game, bool* hadScissor) {
    if (hadScissor) *hadScissor = false;
    if (!dev) return nullptr;
    if (g_rsMemoDev != (const void*)dev) { RsMemoClear(); g_rsMemoDev = dev; }
    if (g_rsMemoLast >= 0 && g_rsMemo[g_rsMemoLast].game == game) {
        if (hadScissor) *hadScissor = g_rsMemo[g_rsMemoLast].had;
        return g_rsMemo[g_rsMemoLast].sc;
    }
    for (int i = 0; i < g_rsMemoN; ++i)
        if (g_rsMemo[i].game == game) {
            g_rsMemoLast = i;
            if (hadScissor) *hadScissor = g_rsMemo[i].had;
            return g_rsMemo[i].sc;
        }
    D3D11_RASTERIZER_DESC d{};
    if (game) game->GetDesc(&d);
    else {                                               // the D3D11 default state
        d.FillMode = D3D11_FILL_SOLID; d.CullMode = D3D11_CULL_BACK; d.FrontCounterClockwise = FALSE;
        d.DepthBias = 0; d.DepthBiasClamp = 0.0f; d.SlopeScaledDepthBias = 0.0f; d.DepthClipEnable = TRUE;
        d.ScissorEnable = FALSE; d.MultisampleEnable = FALSE; d.AntialiasedLineEnable = FALSE;
    }
    const bool had = d.ScissorEnable != FALSE;
    if (hadScissor) *hadScissor = had;
    ID3D11RasterizerState* sc = nullptr;
    if (had && game) sc = game;
    else {
        d.ScissorEnable = TRUE;
        if (FAILED(dev->CreateRasterizerState(&d, &sc))) sc = nullptr;
    }
    if (!sc) { ++g_rsFails; return nullptr; }
    if (g_rsMemoN >= kRsMemo) {                          // full: not kept (a copy per call would leak) -> no clip for it
        if (sc != game) sc->Release();
        ++g_rsFails;
        return nullptr;
    }
    if (game) game->AddRef();
    g_rsMemo[g_rsMemoN] = { game, sc, had };
    g_rsMemoLast = g_rsMemoN++;
    return sc;
}
uint64_t DrawIdRecord::ScissorRsFails() { return g_rsFails; }

DrawIdRecord::ReplayResult DrawIdRecord::Replay(ID3D11DeviceContext1* ctx, ID3D11RenderTargetView* idRtv,
                                                 ID3D11DepthStencilView* roDsv, bool restoreTargets, bool replayAll) {
    return ReplayImpl(ctx, idRtv, roDsv, restoreTargets, replayAll, false, 0);
}

DrawIdRecord::ReplayResult DrawIdRecord::ReplayForward(ID3D11DeviceContext1* ctx, ID3D11RenderTargetView* idRtv,
                                                        ID3D11DepthStencilView* fwdDsv, bool restoreTargets, UINT idBase) {
    return ReplayImpl(ctx, idRtv, fwdDsv, restoreTargets, true, true, idBase);
}

// v0.10.0 phase 10: the batched forward-depth re-draw (see draw_ids.h)
DrawIdRecord::ReplayResult DrawIdRecord::ReplayDepth(ID3D11DeviceContext1* ctx, ID3D11RenderTargetView* rt0,
                                                      ID3D11DepthStencilView* dsv, ID3D11DepthStencilState* dss,
                                                      ID3D11BlendState* bs, bool restoreTargets) {
    if (!dss || !bs) { ReplayResult rr; m_depthTo = m_n; return rr; }
    return ReplayImpl(ctx, rt0, dsv, restoreTargets, true, true, 0, dss, bs);
}

void DrawIdRecord::DropIds() {
    const int relEnd = (m_capPs && m_depthTo < m_n) ? m_depthTo : m_n;
    ReleaseRange(m_releasedTo, relEnd);
    m_releasedTo = relEnd;
    m_replayedTo = m_n;
    if (++m_gen == 0) { memset(m_resGen, 0, sizeof(m_resGen)); m_gen = 1; }
    m_resFill = 0;
    memset(m_lastRes, 0, sizeof(m_lastRes));
}

DrawIdRecord::ReplayResult DrawIdRecord::ReplayImpl(ID3D11DeviceContext1* ctx, ID3D11RenderTargetView* idRtv,
                                                     ID3D11DepthStencilView* roDsv, bool restoreTargets, bool replayAll,
                                                     bool forward, UINT idBase, ID3D11DepthStencilState* depthDss,
                                                     ID3D11BlendState* depthBs) {
    ReplayResult rr;
    if (!ctx) return rr;
    Flush(ctx);                                          // MVP copies of everything recorded so far (queued first)
    // v0.10.0 phase 10: depth mode (the forward-depth batch) has its own cursor and never touches the id state; the id replay
    // of a PS-capturing record releases only the draws whose depth re-draw already ran
    const bool depth = depthDss != nullptr;
    const int relEnd = (m_capPs && m_depthTo < m_n) ? m_depthTo : m_n;
    if (depth ? PendingDepth() <= 0 : PendingReplay() <= 0) { rr.ok = true; return rr; }
    if (depth && (!idRtv || !roDsv || !m_ps)) {
        m_depthTo = m_n;
        return rr;
    }
    if (!depth && (!idRtv || !roDsv || !s_ps || !s_dss || !s_idCb || !s_bsR || !s_bsG)) {
        m_replayFail = true;
        ReleaseRange(m_releasedTo, relEnd);
        m_releasedTo = relEnd;
        m_replayedTo = m_n;
        ++m_segments;
        return rr;
    }
    if (forward && !depth) {                             // one id space per pass: every forward replay uses the same base
        if (m_idBase == kNone) m_idBase = idBase;
        else if (m_idBase != idBase) m_replayFail = true;
    }
    GfxSave sv;
    sv.Save(ctx, restoreTargets, depth);                 // phase 10: depth mode sets every PS slot of the game's draws
    if (!m_cleared && !depth) {
        if (!forward) {                                  // the forward replay never clears: the G-buffer replay did (both
            const FLOAT zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // channels) and its ids must survive
            ctx->ClearRenderTargetView(idRtv, zero);
        }
        m_cleared = true;
    }
    ctx->OMSetRenderTargets(1, &idRtv, roDsv);
    if (depth) {                                         // v0.10.0 phase 10: the caller's forward-depth states (A2C, mask 0)
        ctx->OMSetDepthStencilState(depthDss, 0);
        ctx->OMSetBlendState(depthBs, nullptr, 0xFFFFFFFFu);
    } else {
        ctx->OMSetDepthStencilState(s_dss.Get(), 0);
        // v0.10.0 phase 9: a forward replay at 1/2 resolution writes its OWN single-channel target (RED)
        ctx->OMSetBlendState((forward && !m_shift) ? s_bsG.Get() : s_bsR.Get(), nullptr, 0xFFFFFFFFu);
        ctx->PSSetShader(s_ps.Get(), nullptr, 0);
    }
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ID3D11Buffer* idCb = s_idCb.Get();

    // per-draw: only what differs from the previously replayed draw (first draw: everything). REVERSE draw order: the game
    // tests depth GREATER, so of two draws at exactly the same depth the FIRST one owns the pixel; with EQUAL the last
    // replayed one wins -- replaying backwards gives the same owner. v0.10.0 phase 3, forward mode: DRAW order (the forward
    // depth is written with GREATER_EQUAL: the LAST draw of a tie owns the pixel).
    const State* cur = nullptr;
    const PsState* curPs = nullptr;                      // v0.10.0 phase 10 (depth mode)
    const int from = depth ? m_depthTo : m_replayedTo;
    const int pend = m_n - from;
    // v0.10.0 phase 14 (mv_replay_static_*): the static gate of the main G-buffer passes (see SetStaticGate)
    const bool statGate = g_staticGate && !forward && !depth && g_statFrames > 0u && g_statEvery > 1u;
    if (statGate) m_staticGated = true;
    // v0.10.0 phase 9 replay view (SetView): every draw through a scissor-enabled copy of its RS, scissor = its own (if its RS had
    // one) n the clip, viewport / scissor at 1/2^shift (forward mode). Off (no clip, no shift) = the phase-8 calls exactly.
    const UINT shift = forward ? m_shift : 0u;
    const bool view = m_clip || shift != 0u;
    ComPtr<ID3D11Device> vdev;
    if (view) ctx->GetDevice(&vdev);
    ID3D11RasterizerState* lastRs = nullptr;
    D3D11_VIEWPORT lastVp[kVp] = {};
    D3D11_RECT lastSc[kVp] = {};
    UINT lastNVp = 0, lastNSc = 0;
    bool viewFirst = true;
    // v0.10.0 phase 8: in a SAMPLED pass (GpuPerf, 1 in 31) the G-buffer replay is split into runs of instanced / no-MVP draws
    // and runs of the others, one timestamp per run boundary (at most kSplitMax per replay; the rest of the replay is then not
    // split) -- "replay-inst*" / "replay-rest*" in the perf eye line (the input for the instanced-replay decision)
    constexpr int kSplitMax = 64;
    const bool split = !forward && !depth && GpuPerf::On() && !GpuPerf::Mirror() && GpuPerf::Sampled(GpuPerf::Pass());
    int splitTok = -1, splitKind = -1, splitN = 0;
    for (int q = 0; q < pend; ++q) {
        const int i = forward ? from + q : m_n - 1 - q;
        const State& s = m_st[i];
        const uint8_t fl = m_key[i].flags;
        if (!replayAll && !(fl & kFLate) && (fl & (kFInstanced | kFNoMvp))) { ++rr.skipped; continue; }
        // v0.10.0 phase 8 (mv_inst_replay 1): outside a probe pass only the instanced keys that took a moving parent lately
        if (replayAll && g_instReplay && !g_instProbe && !forward && !(fl & kFLate) && (fl & (kFInstanced | kFNoMvp)) &&
            !DrawIdMv::InstancedWanted(m_key[i].shape)) { ++rr.skipped; ++m_instGated; continue; }
        // v0.10.0 phase 14: a long-static world draw outside its re-check frame -- no id pixels (= the camera R in pass B); it stays
        // recorded, so its MVP is still gathered and paired (DrawIdMv), which is what keeps its verdict and its partner alive.
        // Never a LATE draw (recorded after an earlier replay of this pass: a forced replay / a G-buffer re-entry): the earlier
        // segment wrote its ids against the depth of that moment, and only the later draws' replay overwrites the ids of what
        // they cover -- a skipped late wall would leave a car's stale id (its motion) on its pixels. (In-game logs: 0
        // multi-segment passes, so this costs nothing there.)
        if (g_testSkipKey && !forward && !depth && m_key[i].full == g_testSkipKey) continue;   // round 4: harness-only hook
        if (statGate && !(fl & (kFInstanced | kFNoMvp | kFCabin | kFLate)) && DrawIdMv::StaticSkip(m_key[i].full)) {
            ++m_staticSkipped; m_staticBits[i >> 5] |= 1u << (i & 31);
            continue;
        }
        const UINT drawId = (forward ? idBase : 0u) + (UINT)i + 1u;
        if (!depth && drawId > (UINT)kMax) { ++rr.skipped; ++m_overflow; continue; }   // no id window (forward mode)
        if (split && splitN <= kSplitMax) {
            const int kind = (fl & (kFInstanced | kFNoMvp)) ? 1 : 0;
            if (kind != splitKind) {
                splitKind = kind;
                if (++splitN > kSplitMax) { GpuPerf::End(ctx, splitTok); splitTok = -1; }
                else splitTok = GpuPerf::Next(ctx, splitTok, kind ? GpuPerf::kReplayInst : GpuPerf::kReplayRest);
            }
        }
        if (!cur || cur->il != s.il) ctx->IASetInputLayout(s.il);
        if (!cur || memcmp(cur->vb, s.vb, sizeof(s.vb)) || memcmp(cur->vbStride, s.vbStride, sizeof(s.vbStride)) ||
            memcmp(cur->vbOff, s.vbOff, sizeof(s.vbOff)))
            ctx->IASetVertexBuffers(0, kVb, s.vb, s.vbStride, s.vbOff);
        if (!cur || cur->ib != s.ib || cur->ibFmt != s.ibFmt || cur->ibOff != s.ibOff) ctx->IASetIndexBuffer(s.ib, s.ibFmt, s.ibOff);
        if (!cur || cur->topo != s.topo) ctx->IASetPrimitiveTopology(s.topo);
        if (!cur || cur->vs != s.vs) ctx->VSSetShader(s.vs, nullptr, 0);
        if (!cur || memcmp(cur->cb, s.cb, sizeof(s.cb)) || memcmp(cur->cbFirst, s.cbFirst, sizeof(s.cbFirst)) ||
            memcmp(cur->cbNum, s.cbNum, sizeof(s.cbNum))) {
            UINT f[kVsCb], nm[kVsCb];
            for (int c = 0; c < kVsCb; ++c) {
                const bool valid = s.cb[c] && s.cbNum[c] != 0;
                f[c] = valid ? s.cbFirst[c] : 0;
                nm[c] = valid ? s.cbNum[c] : 4096;
            }
            ctx->VSSetConstantBuffers1(0, kVsCb, s.cb, f, nm);
        }
        if (!cur || memcmp(cur->srv, s.srv, sizeof(s.srv))) ctx->VSSetShaderResources(0, kVsSrv, s.srv);
        if (!cur || memcmp(cur->smp, s.smp, sizeof(s.smp))) ctx->VSSetSamplers(0, kVsSmp, s.smp);
        if (!view) {
            if (!cur || cur->rs != s.rs) ctx->RSSetState(s.rs);
            if (!cur || cur->nVp != s.nVp || memcmp(cur->vp, s.vp, sizeof(D3D11_VIEWPORT) * s.nVp))
                ctx->RSSetViewports(s.nVp, s.nVp ? s.vp : nullptr);
            if (!cur || cur->nSc != s.nSc || memcmp(cur->sc, s.sc, sizeof(D3D11_RECT) * s.nSc))
                ctx->RSSetScissorRects(s.nSc, s.nSc ? s.sc : nullptr);
        } else {
            bool had = false;
            ID3D11RasterizerState* rsS = ScissorRs(vdev.Get(), s.rs, &had);
            // no copy (creation failed / table full): the draw's own RS, its own scissor scaled, no clip (more pixels, same ids)
            ID3D11RasterizerState* rsUse = rsS ? rsS : s.rs;
            const bool scOn = rsS ? true : had;
            D3D11_VIEWPORT vp[kVp];
            memcpy(vp, s.vp, sizeof(D3D11_VIEWPORT) * s.nVp);
            ScaleViewport(vp, s.nVp, shift);
            D3D11_RECT sc[kVp] = {};
            UINT nSc = 0;
            if (scOn) {
                nSc = s.nVp ? s.nVp : 1u;
                for (UINT k = 0; k < nSc; ++k)
                    ScaledScissor(had, k < s.nSc ? &s.sc[k] : nullptr, rsS && m_clip ? &m_clipRect : nullptr, shift, &sc[k]);
            }
            if (viewFirst || rsUse != lastRs) ctx->RSSetState(rsUse);
            if (viewFirst || lastNVp != s.nVp || memcmp(lastVp, vp, sizeof(D3D11_VIEWPORT) * s.nVp))
                ctx->RSSetViewports(s.nVp, s.nVp ? vp : nullptr);
            if (scOn && (viewFirst || lastNSc != nSc || memcmp(lastSc, sc, sizeof(D3D11_RECT) * nSc)))
                ctx->RSSetScissorRects(nSc, sc);
            viewFirst = false;
            lastRs = rsUse; lastNVp = s.nVp; memcpy(lastVp, vp, sizeof(D3D11_VIEWPORT) * s.nVp);
            if (scOn) { lastNSc = nSc; memcpy(lastSc, sc, sizeof(D3D11_RECT) * nSc); }
        }
        if (depth) {                                     // v0.10.0 phase 10: the game's pixel shader + its resources
            const PsState& p = m_ps[i];
            if (!curPs || curPs->ps != p.ps) ctx->PSSetShader(p.ps, nullptr, 0);
            if (!curPs || memcmp(curPs->cb, p.cb, sizeof(p.cb)) || memcmp(curPs->cbFirst, p.cbFirst, sizeof(p.cbFirst)) ||
                memcmp(curPs->cbNum, p.cbNum, sizeof(p.cbNum))) {
                UINT f[kPsCb], nm[kPsCb];
                for (int c = 0; c < kPsCb; ++c) {
                    const bool valid = p.cb[c] && p.cbNum[c] != 0;
                    f[c] = valid ? p.cbFirst[c] : 0;
                    nm[c] = valid ? p.cbNum[c] : 4096;
                }
                ctx->PSSetConstantBuffers1(0, kPsCb, p.cb, f, nm);
            }
            if (!curPs || memcmp(curPs->srv, p.srv, sizeof(p.srv))) ctx->PSSetShaderResources(0, kPsSrv, p.srv);
            if (!curPs || memcmp(curPs->smp, p.smp, sizeof(p.smp))) ctx->PSSetSamplers(0, kPsSmp, p.smp);
            curPs = &p;
        } else {
            const UINT first = drawId * 16u, num = 16u;
            ctx->PSSetConstantBuffers1(0, 1, &idCb, &first, &num);
        }
        if (s.instanced) ctx->DrawIndexedInstanced(s.ic, s.inst, s.si, s.bv, s.sinst);
        else ctx->DrawIndexed(s.ic, s.si, s.bv);
        cur = &s;
        ++rr.replayed;
    }
    GpuPerf::End(ctx, splitTok);
    sv.Restore(ctx);
    if (depth) {                                         // v0.10.0 phase 10: only the depth cursor moves
        m_depthTo = m_n;
        if (m_replayedTo >= m_n) { ReleaseRange(m_releasedTo, m_n); m_releasedTo = m_n; }
        rr.ok = true;
        return rr;
    }
    m_replayed += rr.replayed;
    m_skipped += rr.skipped;
    ReleaseRange(m_releasedTo, relEnd);
    m_releasedTo = relEnd;
    m_replayedTo = m_n;
    ++m_segments;
    // the pending-resource set restarts (everything recorded so far is replayed)
    if (++m_gen == 0) { memset(m_resGen, 0, sizeof(m_resGen)); m_gen = 1; }
    m_resFill = 0;
    memset(m_lastRes, 0, sizeof(m_lastRes));
    rr.ok = true;
    return rr;
}

uint64_t DrawIdRecord::FullKey(int i) const { return (i >= 0 && i < m_n) ? m_key[i].full : 0; }
uint64_t DrawIdRecord::LooseKey(int i) const { return (i >= 0 && i < m_n) ? m_key[i].loose : 0; }
uint64_t DrawIdRecord::ShapeKey(int i) const { return (i >= 0 && i < m_n) ? m_key[i].shape : 0; }   // v0.10.0 phase 8
UINT     DrawIdRecord::MirrorOff(int i) const { return (i >= 0 && i < m_n) ? m_key[i].off : kNone; }
uint8_t  DrawIdRecord::Flags(int i) const { return (i >= 0 && i < m_n) ? m_key[i].flags : 0; }
float    DrawIdRecord::VpMin(int i) const { return (i >= 0 && i < m_n) ? m_key[i].vpMin : 0.0f; }
float    DrawIdRecord::VpMax(int i) const { return (i >= 0 && i < m_n) ? m_key[i].vpMax : 1.0f; }
UINT     DrawIdRecord::VpJitterPacked(int i) const { return (i >= 0 && i < m_n) ? m_key[i].jit : 0u; }   // phase 14 round 4
UINT     DrawIdRecord::InstanceCount(int i) const { return (i >= 0 && i < m_n) ? m_key[i].inst : 1u; }  // phase 14 round 4
UINT     DrawIdRecord::IndexCount(int i) const { return (i >= 0 && i < m_n) ? m_key[i].ic : 0; }
void     DrawIdRecord::CbWindow(int i, UINT* first, UINT* num) const {
    *first = (i >= 0 && i < m_n) ? m_key[i].cbFirst : 0;
    *num = (i >= 0 && i < m_n) ? m_key[i].cbNum : 0;
}

void DrawIdRecord::RegisterShaders() {
    ShaderCache::Add(ShaderCache::kDrawIdPs, kDrawIdPs, sizeof(kDrawIdPs) - 1, "drawid_ps", "PSMain", "ps_5_0");
    // v0.10.0 phase 10: the forward-depth occlusion init
    ShaderCache::Add(ShaderCache::kFwdInitVs, kFwdInitShader, sizeof(kFwdInitShader) - 1, "fwd_init_vs", "VSMain", "vs_5_0");
    ShaderCache::Add(ShaderCache::kFwdInitPs0, kFwdInitShader, sizeof(kFwdInitShader) - 1, "fwd_init_ps0", "PSInit0", "ps_5_0");
    ShaderCache::Add(ShaderCache::kFwdInitPs1, kFwdInitShader, sizeof(kFwdInitShader) - 1, "fwd_init_ps1", "PSInit1", "ps_5_0");
}

// v0.10.0 phase 10: the occlusion init of a forward-depth target (see draw_ids.h). Device objects per device, lazily.
static bool FwdInitObjects(ID3D11Device* dev) {
    if (!dev) return false;
    if (s_initDev == (const void*)dev) return !s_initFailed && s_initVs && s_initPs[0] && s_initPs[1] && s_initDss && s_initRs;
    if (!ShaderCache::Done()) return false;              // warm-up still running: retried
    s_initVs.Reset(); s_initPs[0].Reset(); s_initPs[1].Reset(); s_initDss.Reset(); s_initRs.Reset();
    s_initDev = dev;
    s_initFailed = true;
    size_t n0 = 0, n1 = 0, n2 = 0;
    const void* c0 = ShaderCache::Code(ShaderCache::kFwdInitVs, &n0);
    const void* c1 = ShaderCache::Code(ShaderCache::kFwdInitPs0, &n1);
    const void* c2 = ShaderCache::Code(ShaderCache::kFwdInitPs1, &n2);
    if (!c0 || !c1 || !c2 || FAILED(dev->CreateVertexShader(c0, n0, nullptr, &s_initVs)) ||
        FAILED(dev->CreatePixelShader(c1, n1, nullptr, &s_initPs[0])) ||
        FAILED(dev->CreatePixelShader(c2, n2, nullptr, &s_initPs[1]))) {
        Log("MV forward depth: occlusion-init shaders unavailable (%s) -- the forward depth starts empty (as mv_fwd_depth_cull 0)",
            ShaderCache::Error(!c0 ? ShaderCache::kFwdInitVs : (!c1 ? ShaderCache::kFwdInitPs0 : ShaderCache::kFwdInitPs1)));
        return false;
    }
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = FALSE;
    dd.StencilReadMask = 0xFF; dd.StencilWriteMask = 0xFF;
    dd.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
    dd.BackFace = dd.FrontFace;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.ScissorEnable = TRUE;
    if (FAILED(dev->CreateDepthStencilState(&dd, &s_initDss)) || FAILED(dev->CreateRasterizerState(&rd, &s_initRs))) {
        Log("MV forward depth: occlusion-init states create failed -- the forward depth starts empty (as mv_fwd_depth_cull 0)");
        return false;
    }
    s_initFailed = false;
    return true;
}

bool DrawIdRecord::InitFwdReady() { return s_initDev && !s_initFailed && s_initVs; }

bool DrawIdRecord::InitFwdDepth(ID3D11DeviceContext1* ctx, ID3D11DepthStencilView* dsv, UINT dsvW, UINT dsvH,
                                ID3D11ShaderResourceView* snapSrv, UINT shift, const D3D11_RECT* clip) {
    if (!ctx || !dsv || !snapSrv || !dsvW || !dsvH) return false;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!FwdInitObjects(dev.Get())) return false;
    shift = shift ? 1u : 0u;
    D3D11_RECT sc{};
    if (!ScaledScissor(false, nullptr, clip, shift, &sc)) return true;   // nothing of the clip on the target: nothing to cull
    if (sc.right > (LONG)dsvW) sc.right = (LONG)dsvW;
    if (sc.bottom > (LONG)dsvH) sc.bottom = (LONG)dsvH;
    GfxSave sv;
    sv.Save(ctx, true, true);
    ID3D11RenderTargetView* noRtv = nullptr;
    ctx->OMSetRenderTargets(1, &noRtv, dsv);
    ctx->OMSetDepthStencilState(s_initDss.Get(), 0);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(s_initVs.Get(), nullptr, 0);
    ctx->PSSetShader(s_initPs[shift].Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &snapSrv);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->RSSetState(s_initRs.Get());
    const D3D11_VIEWPORT vp{ 0.0f, 0.0f, (float)dsvW, (float)dsvH, 0.0f, 1.0f };
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetScissorRects(1, &sc);
    ctx->Draw(3, 0);
    sv.Restore(ctx);
    return true;
}

bool DrawIdRecord::DeviceReady() { return s_ps && s_dss && s_idCb && s_bsR && s_bsG; }

bool DrawIdRecord::InitDevice(ID3D11Device* dev) {
    if (!dev) return false;
    if (s_dev == (const void*)dev) return DeviceReady();
    if (s_dev && s_dev != (const void*)dev) ShutdownDevice();
    if (!ShaderCache::Done()) return false;              // warm-up still running: retried (not failed)
    s_dev = dev;
    size_t size = 0;
    const void* code = ShaderCache::Code(ShaderCache::kDrawIdPs, &size);
    HRESULT hr = E_FAIL;
    if (!code || FAILED(hr = dev->CreatePixelShader(code, size, nullptr, &s_ps))) {
        Log("MV draw-ids: replay pixel shader unavailable (%s, hr=0x%lx) -- per-draw motion vectors off",
            ShaderCache::Error(ShaderCache::kDrawIdPs), (unsigned long)hr);
        s_ps.Reset();
        return false;
    }
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;     // read-only DSV: no depth write
    dd.DepthFunc = D3D11_COMPARISON_EQUAL;               // the draw owns the pixel exactly where its depth IS the depth
    dd.StencilEnable = FALSE;
    dd.StencilReadMask = 0xFF; dd.StencilWriteMask = 0x00;
    dd.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
    dd.BackFace = dd.FrontFace;
    if (FAILED(hr = dev->CreateDepthStencilState(&dd, &s_dss))) {
        Log("MV draw-ids: EQUAL depth state create hr=0x%lx -- per-draw motion vectors off", (unsigned long)hr);
        ShutdownDevice(); s_dev = dev;
        return false;
    }
    // v0.10.0 phase 3: blend states that write only RED (G-buffer ids) / only GREEN (forward ids); no blending
    for (int ch = 0; ch < 2; ++ch) {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = ch == 0 ? D3D11_COLOR_WRITE_ENABLE_RED : D3D11_COLOR_WRITE_ENABLE_GREEN;
        if (FAILED(hr = dev->CreateBlendState(&bd, ch == 0 ? &s_bsR : &s_bsG))) {
            Log("MV draw-ids: id write-mask blend state create hr=0x%lx -- per-draw motion vectors off", (unsigned long)hr);
            ShutdownDevice(); s_dev = dev;
            return false;
        }
    }
    // immutable cbuffer: (kMax + 1) windows of 256 bytes (16 constants), window k holds uint4(k, 0, 0, 0)
    const UINT windows = (UINT)kMax + 1u;
    uint32_t* init = new (std::nothrow) uint32_t[windows * 64u];
    if (!init) { ShutdownDevice(); s_dev = dev; return false; }
    memset(init, 0, sizeof(uint32_t) * windows * 64u);
    for (UINT w = 0; w < windows; ++w) init[w * 64u] = w;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = windows * 256u;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{ init, 0, 0 };
    hr = dev->CreateBuffer(&bd, &sd, &s_idCb);
    delete[] init;
    if (FAILED(hr)) {
        Log("MV draw-ids: id cbuffer (%u bytes) create hr=0x%lx -- per-draw motion vectors off", bd.ByteWidth,
            (unsigned long)hr);
        ShutdownDevice(); s_dev = dev;
        return false;
    }
    return true;
}

void DrawIdRecord::ShutdownDevice() {
    s_ps.Reset(); s_dss.Reset(); s_idCb.Reset(); s_bsR.Reset(); s_bsG.Reset();
    s_dev = nullptr;
    s_initVs.Reset(); s_initPs[0].Reset(); s_initPs[1].Reset(); s_initDss.Reset(); s_initRs.Reset();   // v0.10.0 phase 10
    s_initDev = nullptr; s_initFailed = false;
    RsMemoClear();                                       // v0.10.0 phase 9: the scissor-enabled RS copies
}

// =====================================================================================================================
// DrawIdMv
// =====================================================================================================================
void DrawIdMv::RegisterShaders() {
    ShaderCache::Add(ShaderCache::kDrawIdGather, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_gather", "CSGather", "cs_5_0");
    ShaderCache::Add(ShaderCache::kDrawIdMatch, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_match", "CSMatch", "cs_5_0");
    ShaderCache::Add(ShaderCache::kDrawIdResolve, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_resolve", "CSResolve",
                     "cs_5_0");
    ShaderCache::Add(ShaderCache::kDrawIdPair, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_pair", "CSPair", "cs_5_0");
    ShaderCache::Add(ShaderCache::kDrawIdVote, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_vote", "CSVote", "cs_5_0");
    ShaderCache::Add(ShaderCache::kDrawIdPick, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_pick", "CSPick", "cs_5_0");
    ShaderCache::Add(ShaderCache::kDrawIdAttach, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_attach", "CSAttach", "cs_5_0");
    ShaderCache::Add(ShaderCache::kDrawIdInherit1, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_inherit1", "CSInherit1", "cs_5_0");
    ShaderCache::Add(ShaderCache::kDrawIdTwin, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_twin", "CSTwin", "cs_5_0");
    // v0.10.0 phase 6 (rigid parents)
    ShaderCache::Add(ShaderCache::kDrawIdParentList, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_parent_list", "CSParentList",
                     "cs_5_0");
    ShaderCache::Add(ShaderCache::kDrawIdParentVote, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_parent_vote", "CSParentVote",
                     "cs_5_0");
    ShaderCache::Add(ShaderCache::kDrawIdParentPick1, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_parent_pick1", "CSParentPick1",
                     "cs_5_0");
    ShaderCache::Add(ShaderCache::kDrawIdParentPick2, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_parent_pick2", "CSParentPick2",
                     "cs_5_0");
    // v0.10.0 phase 6b (instanced draws join the vote)
    ShaderCache::Add(ShaderCache::kDrawIdParentCount, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_parent_count", "CSParentCount",
                     "cs_5_0");
    // v0.10.0 phase 9 (folded dispatches)
    ShaderCache::Add(ShaderCache::kDrawIdPickResolve, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_pick_resolve", "CSPickResolve",
                     "cs_5_0");
    ShaderCache::Add(ShaderCache::kDrawIdListTwin, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_list_twin", "CSListTwin", "cs_5_0");
    // v0.10.0 phase 18 (mv_fold_dispatch 2, mv_vote_compact)
    ShaderCache::Add(ShaderCache::kDrawIdPickPar, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_pick_par", "CSPickPar", "cs_5_0");
    ShaderCache::Add(ShaderCache::kDrawIdResInh, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_resolve_inherit", "CSResolveInherit",
                     "cs_5_0");
    ShaderCache::Add(ShaderCache::kDrawIdParentCollect, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_parent_collect",
                     "CSParentCollect", "cs_5_0");
    ShaderCache::Add(ShaderCache::kDrawIdParentMarch, kDrawIdCs, sizeof(kDrawIdCs) - 1, "drawid_parent_march", "CSParentMarch",
                     "cs_5_0");
}

void DrawIdMv::SetFold(int mode) { g_foldMode = mode <= 0 ? 0 : (mode >= 2 ? 2 : 1); }   // v0.10.0 phase 9 / 18
int  DrawIdMv::Fold() { return g_foldMode; }
void DrawIdMv::SetVoteCompact(bool on) { g_voteCompact = on; }   // v0.10.0 phase 18
bool DrawIdMv::VoteCompact() { return g_voteCompact; }
void DrawIdMv::SetConsMinCabin(unsigned n) { g_consMinCabin = n == 0u ? 0u : (n < 8u ? 8u : (n > 64u ? 64u : n)); }
unsigned DrawIdMv::ConsMinCabin() { return g_consMinCabin; }

void DrawIdMv::SetConsensus(bool on) { g_consensus = on; }
bool DrawIdMv::Consensus() { return g_consensus; }
void DrawIdMv::SetAttach(float m) { g_attachM = !(m >= 0.0f) ? 0.0f : (m > 5.0f ? 5.0f : m); }
float DrawIdMv::AttachM() { return g_attachM; }
void DrawIdMv::SetTwin(bool on) { g_twin = on; }
bool DrawIdMv::Twin() { return g_twin; }
void DrawIdMv::SetParent(bool on) { g_parent = on; }        // v0.10.0 phase 6
bool DrawIdMv::Parent() { return g_parent; }
void DrawIdMv::SetParentMutation(int bits) { g_parentMut = bits; }
void DrawIdMv::SetInstanced(bool on) { g_instancedVote = on; }   // v0.10.0 phase 6b
void DrawIdMv::SetParentReach(unsigned px) { g_parReach = px; }   // v0.10.0 phase 6c (harness only)
void DrawIdMv::SetVoteRes(unsigned px) { g_voteRes = px >= 4u ? 4u : 2u; }                       // v0.10.0 phase 8
unsigned DrawIdMv::VoteRes() { return g_voteRes; }
void DrawIdMv::SetMarchCap(unsigned n) { g_marchCap = n == 0u ? 0u : (n < 4096u ? 4096u : (n > 1000000u ? 1000000u : n)); }
unsigned DrawIdMv::MarchCap() { return g_marchCap; }
void DrawIdMv::SetMedoidDrop(unsigned frames) { g_medoidDrop = frames > 100000u ? 100000u : frames; }   // v0.10.0 phase 8
unsigned DrawIdMv::MedoidDrop() { return g_medoidDrop; }
bool DrawIdMv::ConsHealthy(int layer) const {
    return g_medoidDrop != 0u && g_consensus && m_ready && m_consRun[layer & 1] >= g_medoidDrop;
}
void DrawIdMv::SetInstReplay(int mode) { g_instReplay = mode ? 1 : 0; }   // v0.10.0 phase 8
int  DrawIdMv::InstReplay() { return g_instReplay; }
void DrawIdMv::SetParentMaxListed(unsigned n) { g_parMaxListed = n < 8u ? 8u : (n > 64u ? 64u : n); }   // v0.10.0 phase 10
unsigned DrawIdMv::ParentMaxListed() { return g_parMaxListed; }
void DrawIdMv::SetFrame(uint64_t frame) { g_instFrame = frame; }
int  DrawIdMv::InstancedKnown() { return g_instKnown; }
bool DrawIdMv::InstancedWanted(uint64_t key) {
    if (!key) key = 1;
    uint32_t h = (uint32_t)(key ^ (key >> 29)) & (kInstTab - 1);
    for (int p = 0; p < 64; ++p, h = (h + 1) & (kInstTab - 1)) {
        if (!g_instKey[h]) return false;
        if (g_instKey[h] == key) return g_instSeen[h] + kInstKeepFrames >= g_instFrame;
    }
    return false;
}
void DrawIdRecord::SetInstancedProbe(bool probe) { g_instProbe = probe; }
// ---- v0.10.0 phase 14 STATIC REPLAY SKIP (see the StatEnt table above) ----
void DrawIdRecord::SetStaticGate(bool on) { g_staticGate = on; }
void DrawIdRecord::SetTestSkipKey(uint64_t fullKey) { g_testSkipKey = fullKey; }   // round 4 (harness only)
void DrawIdMv::SetReplayStatic(unsigned frames, unsigned every) {
    const unsigned f = frames > 60u ? 60u : frames;
    const unsigned e = every < 1u ? 1u : (every > 16u ? 16u : every);
    if (f != g_statFrames || e != g_statEvery) StaticClear(kStatClrConfig);
    g_statFrames = f; g_statEvery = e;
}
unsigned DrawIdMv::ReplayStaticFrames() { return g_statFrames; }
unsigned DrawIdMv::ReplayStaticEvery() { return g_statEvery; }
void DrawIdMv::SetReplayStaticEps(float px) {           // round 3 (dlaa.ini mv_replay_static_eps_px, 0..0.1)
    const float v = !(px >= 0.0f) ? 0.0f : (px > 0.1f ? 0.1f : px);
    if (v != g_statEps) StaticClear(kStatClrConfig);
    g_statEps = v;
}
float DrawIdMv::ReplayStaticEps() { return g_statEps; }
bool DrawIdMv::StaticSkip(uint64_t key) {
    if (g_statFrames == 0u || g_statEvery <= 1u) return false;
    const StatEnt* e = StatFind(key, false);
    if (!e || e->streak < g_statFrames) return false;
    if (e->seen + kStatFresh < g_instFrame) return false;                   // no recent verdict (readbacks stalled)
    if (e->parent && e->parent + kStatParentHold >= g_instFrame) return false;   // a rigid parent lately: its plates need it
    if (e->moved && e->moved + kStatQuiet > g_instFrame) return false;      // round 3: it moved / wobbled lately (a vehicle)
    // the re-check frame: ~1/every of the static keys each frame, spread by the key's hash
    return (g_instFrame % g_statEvery) != (Mix(key ^ 0x51A7C0DE51A7C0DEull) % g_statEvery);
}
void DrawIdMv::StaticClear(int why) {
    if (!g_statUsed) return;                             // nothing to drop (not counted)
    memset(g_stat, 0, sizeof(g_stat));
    g_statUsed = 0;
    if (why >= 0 && why < kStatClrN) ++g_statClears[why];
}
uint64_t DrawIdMv::StaticClears(int why) { return (why >= 0 && why < kStatClrN) ? g_statClears[why] : 0; }
int DrawIdMv::StaticKeys(int* atThreshold, int* held) {
    int n = 0, at = 0, hd = 0;
    for (const StatEnt& e : g_stat) {
        if (!e.key || e.seen + kStatExpire < g_instFrame) continue;
        ++n;
        if (g_statFrames && e.streak >= g_statFrames) ++at;
        if (e.parent && e.parent + kStatParentHold >= g_instFrame) ++hd;
    }
    if (atThreshold) *atThreshold = at;
    if (held) *held = hd;
    return n;
}
void DrawIdMv::PollMain(ID3D11DeviceContext* ctx) {
    if (!ctx || g_statFrames == 0u || g_statEvery <= 1u) return;
    for (DrawIdMv* u : g_statUnit) if (u && u->m_ready && !u->m_quiet) u->Poll(ctx);
}
void DrawIdMv::SetConsCands(unsigned n) { g_consCands = n < 16u ? 16u : (n > (unsigned)kCandWorld ? (unsigned)kCandWorld : n); }
unsigned DrawIdMv::ConsCands() { return g_consCands; }
bool DrawIdMv::Instanced() { return g_instancedVote; }

void DrawIdMv::SetParams(float maxM, float snapPx) {
    g_maxM = !(maxM >= 0.5f) ? 0.5f : (maxM > 100.0f ? 100.0f : maxM);
    g_snapPx = !(snapPx >= 0.0f) ? 0.0f : (snapPx > 4.0f ? 4.0f : snapPx);
}
float DrawIdMv::MaxM() { return g_maxM; }
float DrawIdMv::SnapPx() { return g_snapPx; }

DrawIdMv::DrawIdMv() {}
DrawIdMv::~DrawIdMv() {
    delete[] m_prevFull; delete[] m_prevLoose; delete[] m_prevFlags;
    delete[] m_curFull; delete[] m_curLoose; delete[] m_curFlags;
    delete[] m_ent; delete[] m_orph;
    delete[] m_curGroup; delete[] m_curRank; delete[] m_curLooseG; delete[] m_prevGroup; delete[] m_prevRank;
    delete[] m_groupData; delete[] m_listData;
    delete[] m_candTmp;
    delete[] m_curBig;
    delete[] m_rbInstIdx; delete[] m_rbInstKey; delete[] m_prepInstIdx; delete[] m_prepInstKey;   // v0.10.0 phase 8
    delete[] m_rbStatKey; delete[] m_prepStatKey;        // v0.10.0 phase 14
    if (m_statReg) for (DrawIdMv*& u : g_statUnit) if (u == this) u = nullptr;
    delete[] m_dump;
}

bool DrawIdMv::Init(ID3D11Device* dev) {
    if (m_ready) return true;
    if (m_failed || !dev) return false;
    if (!ShaderCache::Done()) return false;              // retried
    m_failed = true;                                     // until everything below succeeded
    if (!m_prevFull) {
        m_prevFull = new (std::nothrow) uint64_t[kMax]; m_prevLoose = new (std::nothrow) uint64_t[kMax];
        m_prevFlags = new (std::nothrow) uint8_t[kMax];
        m_curFull = new (std::nothrow) uint64_t[kMax]; m_curLoose = new (std::nothrow) uint64_t[kMax];
        m_curFlags = new (std::nothrow) uint8_t[kMax];
        m_ent = new (std::nothrow) Ent[2 * kMax]; m_orph = new (std::nothrow) Ent[2 * kMax];
        m_curGroup = new (std::nothrow) uint32_t[kMax]; m_curRank = new (std::nothrow) uint32_t[kMax];
        m_curLooseG = new (std::nothrow) uint32_t[kMax];
        m_prevGroup = new (std::nothrow) uint32_t[kMax]; m_prevRank = new (std::nothrow) uint32_t[kMax];
        m_groupData = new (std::nothrow) uint32_t[4 * 2 * kMax]; m_listData = new (std::nothrow) uint32_t[2 * kMax];
        m_candTmp = new (std::nothrow) uint32_t[kMax];
        m_curBig = new (std::nothrow) uint8_t[kMax];     // v0.10.0 phase 5
        m_rbInstIdx = new (std::nothrow) uint32_t[kRb * kRbInst];   // v0.10.0 phase 8
        m_rbInstKey = new (std::nothrow) uint64_t[kRb * kRbInst];
        m_prepInstIdx = new (std::nothrow) uint32_t[kRbInst];
        m_prepInstKey = new (std::nothrow) uint64_t[kRbInst];
    }
    if (!m_prevFull || !m_prevLoose || !m_prevFlags || !m_curFull || !m_curLoose || !m_curFlags || !m_ent || !m_orph ||
        !m_curGroup || !m_curRank || !m_curLooseG || !m_prevGroup || !m_prevRank || !m_groupData || !m_listData ||
        !m_candTmp || !m_curBig || !m_rbInstIdx || !m_rbInstKey || !m_prepInstIdx || !m_prepInstKey) {
        Log("MV draw-ids: out of memory for the pairing tables");
        return false;
    }
    if (!CreateCsFromCache(dev, ShaderCache::kDrawIdGather, &m_csGather) ||
        !CreateCsFromCache(dev, ShaderCache::kDrawIdMatch, &m_csMatch) ||
        !CreateCsFromCache(dev, ShaderCache::kDrawIdResolve, &m_csResolve) ||
        !CreateCsFromCache(dev, ShaderCache::kDrawIdPair, &m_csPair) ||
        !CreateCsFromCache(dev, ShaderCache::kDrawIdVote, &m_csVote) ||
        !CreateCsFromCache(dev, ShaderCache::kDrawIdPick, &m_csPick) ||
        !CreateCsFromCache(dev, ShaderCache::kDrawIdAttach, &m_csAttach) ||
        !CreateCsFromCache(dev, ShaderCache::kDrawIdInherit1, &m_csInherit1) ||
        !CreateCsFromCache(dev, ShaderCache::kDrawIdTwin, &m_csTwin) ||
        !CreateCsFromCache(dev, ShaderCache::kDrawIdParentList, &m_csParList) ||          // v0.10.0 phase 6
        !CreateCsFromCache(dev, ShaderCache::kDrawIdParentVote, &m_csParVote) ||
        !CreateCsFromCache(dev, ShaderCache::kDrawIdParentPick1, &m_csParPick1) ||
        !CreateCsFromCache(dev, ShaderCache::kDrawIdParentPick2, &m_csParPick2) ||
        !CreateCsFromCache(dev, ShaderCache::kDrawIdParentCount, &m_csParCount) ||   // v0.10.0 phase 6b
        !CreateCsFromCache(dev, ShaderCache::kDrawIdPickResolve, &m_csPickRes) ||    // v0.10.0 phase 9
        !CreateCsFromCache(dev, ShaderCache::kDrawIdListTwin, &m_csListTwin)) return false;
    // v0.10.0 phase 18: a missing one only keeps the phase-9 / phase-8 paths (mv_fold_dispatch 1, mv_vote_compact 0)
    if (!CreateCsFromCache(dev, ShaderCache::kDrawIdPickPar, &m_csPickPar) ||
        !CreateCsFromCache(dev, ShaderCache::kDrawIdResInh, &m_csResInh)) {
        m_csPickPar.Reset(); m_csResInh.Reset();
        Log("MV draw-ids: phase-18 pick / resolve shaders unavailable -- mv_fold_dispatch 2 runs as 1");
    }
    if (!CreateCsFromCache(dev, ShaderCache::kDrawIdParentCollect, &m_csParCollect) ||
        !CreateCsFromCache(dev, ShaderCache::kDrawIdParentMarch, &m_csParMarch)) {
        m_csParCollect.Reset(); m_csParMarch.Reset();
        Log("MV draw-ids: phase-18 compact vote shaders unavailable -- the vote runs on the full grid (mv_vote_compact 0)");
    }
    const UINT kM = (UINT)kMax;
    bool ok = MakeBuf(dev, kCbBytes, D3D11_BIND_CONSTANT_BUFFER, 0, 0, true, &m_cb) &&
              MakeBuf(dev, kM * 2u * 16u, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16, true, &m_tab) &&
              MakeBuf(dev, kM * 16u, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16, true, &m_ptab) &&
              MakeBuf(dev, kM * 2u * 16u, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16, true, &m_groups) &&
              MakeBuf(dev, kM * 2u * 4u, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, true, &m_lists);
    for (int k = 0; ok && k < 2; ++k)
        ok = MakeBuf(dev, kM * 80u, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                     D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, false, &m_mvp[k]);
    ok = ok && MakeBuf(dev, kWorkBytes, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, false,
                       &m_work) &&
         MakeBuf(dev, kM * kRStride * 16u, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                 D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16, false, &m_rtab) &&
         MakeBuf(dev, kCntBytes, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, false, &m_cnt);
    if (!ok) { Log("MV draw-ids: pairing buffers create failed"); return false; }
    auto srvStruct = [&](ID3D11Buffer* b, UINT n, ID3D11ShaderResourceView** out) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.FirstElement = 0; sd.Buffer.NumElements = n;
        return SUCCEEDED(dev->CreateShaderResourceView(b, &sd, out));
    };
    auto srvRaw = [&](ID3D11Buffer* b, UINT bytes, ID3D11ShaderResourceView** out) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.NumElements = bytes / 4; sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        return SUCCEEDED(dev->CreateShaderResourceView(b, &sd, out));
    };
    auto uavRaw = [&](ID3D11Buffer* b, UINT bytes, ID3D11UnorderedAccessView** out) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = bytes / 4; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        return SUCCEEDED(dev->CreateUnorderedAccessView(b, &ud, out));
    };
    ok = srvStruct(m_tab.Get(), kM * 2u, &m_tabSrv) && srvStruct(m_ptab.Get(), kM, &m_ptabSrv) &&
         srvStruct(m_groups.Get(), kM * 2u, &m_groupsSrv) && srvStruct(m_lists.Get(), kM * 2u, &m_listsSrv);
    for (int k = 0; ok && k < 2; ++k)
        ok = srvRaw(m_mvp[k].Get(), kM * 80u, &m_mvpSrv[k]) && uavRaw(m_mvp[k].Get(), kM * 80u, &m_mvpUav[k]);
    ok = ok && uavRaw(m_work.Get(), kWorkBytes, &m_workUav) && uavRaw(m_cnt.Get(), kCntBytes, &m_cntUav);
    // v0.10.0 phase 8: DispatchIndirect args of the rigid-parent vote (CSParentList writes them; 0 groups = no listed draw with
    // pixels)
    ok = ok && MakeBuf(dev, 32u, D3D11_BIND_UNORDERED_ACCESS,
                       D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0, false, &m_parArgs) &&
         uavRaw(m_parArgs.Get(), 32u, &m_parArgsUav);
    if (ok) {
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kM * kRStride;
        ok = SUCCEEDED(dev->CreateUnorderedAccessView(m_rtab.Get(), &ud, &m_rtabUav)) &&
             srvStruct(m_rtab.Get(), kM * kRStride, &m_rtabSrv);
    }
    // v0.10.0 phase 14 round 4: the plate-hold tables (64 x uint4 each, ping-pong); a failure only disables the hold
    for (int k = 0; ok && k < 2; ++k) {
        bool hk = MakeBuf(dev, 64u * 16u, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                          D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16, false, &m_uhold[k]) &&
                  srvStruct(m_uhold[k].Get(), 64u, &m_uholdSrv[k]);
        if (hk) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_UNKNOWN;
            ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = 64u;
            hk = SUCCEEDED(dev->CreateUnorderedAccessView(m_uhold[k].Get(), &ud, &m_uholdUav[k]));
        }
        if (!hk) {
            for (int q = 0; q < 2; ++q) { m_uhold[q].Reset(); m_uholdSrv[q].Reset(); m_uholdUav[q].Reset(); }
            Log("MV draw-ids: plate-hold tables create failed -- no plate hold");
            break;
        }
    }
    for (int k = 0; ok && k < kRb; ++k) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kCntBytes; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &m_rb[k]));
    }
    if (!ok) { Log("MV draw-ids: pairing views create failed"); return false; }
    m_failed = false;
    m_ready = true;
    return true;
}

// v0.10.0 phase 18: the compact vote's tile / item list + march args, made on the first pass that wants them; a failure keeps the
// full-grid vote for this unit (one line)
bool DrawIdMv::EnsureParList(ID3D11Device* dev) {
    if (m_parListUav && m_marchArgsUav) return true;
    if (m_parListFailed || !dev || !m_csParCollect || !m_csParMarch) return false;
    m_parListFailed = true;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kParListBytes; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = kParListBytes / 4u; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    bool ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &m_parList)) &&
              SUCCEEDED(dev->CreateUnorderedAccessView(m_parList.Get(), &ud, &m_parListUav));
    bd.ByteWidth = 32u;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    ud.Buffer.NumElements = 8u;
    ok = ok && SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &m_marchArgs)) &&
         SUCCEEDED(dev->CreateUnorderedAccessView(m_marchArgs.Get(), &ud, &m_marchArgsUav));
    if (!ok) {
        m_parList.Reset(); m_parListUav.Reset(); m_marchArgs.Reset(); m_marchArgsUav.Reset();
        Log("MV draw-ids: compact vote buffers create failed -- the vote runs on the full grid for this unit");
        return false;
    }
    m_parListFailed = false;
    return true;
}

void DrawIdMv::Poll(ID3D11DeviceContext* ctx) {
    if (m_dumpPending) DumpPoll(ctx);                    // v0.10.0 phase 4
    for (int k = 0; k < kRb; ++k) {
        if (!m_rbPending[k]) continue;
        D3D11_MAPPED_SUBRESOURCE mp{};
        if (ctx->Map(m_rb[k].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mp) != S_OK) continue;
        uint32_t c[kCntBytes / 4];
        memcpy(c, mp.pData, sizeof(c));
        ctx->Unmap(m_rb[k].Get(), 0);
        m_rbPending[k] = false;
        ++m_stats.rbFrames;
        for (int s = 0; s < 8; ++s) m_stats.st[s] += c[s];
        m_stats.loosePaired += c[8];
        m_stats.trackRejects += c[9];
        m_stats.fwdPaired += c[10];                      // v0.10.0 phase 3
        m_stats.fwdMovers += c[11];
        m_stats.fwdAttached += c[12];                    // v0.10.0 phase 4
        m_stats.fwdAttachKept += c[13];
        m_stats.twins += c[48];                          // v0.10.0 phase 5
        m_stats.twinMovers += c[49];
        m_stats.gbufAttached += c[50];
        m_stats.twinLookups += c[52];
        m_stats.twinFwd += c[53];
        m_stats.parListed += c[54];                      // v0.10.0 phase 6 (rigid parents)
        m_stats.parOver += c[55];
        m_stats.parents += c[56];
        m_stats.parMovers += c[57];
        m_stats.parFwd += c[58];
        m_stats.parChain += c[59];
        m_stats.parNoVotes += c[60];
        m_stats.parNoWinner += c[61];
        m_stats.parOverrode += c[62];
        m_stats.parOverrodeDiff += c[193];
        m_stats.parVotes += c[192];
        m_stats.parLeft += c[194];
        m_stats.parInstListed += c[195];                 // v0.10.0 phase 6b
        m_stats.parInst += c[196];
        m_stats.parInstVis += c[197];
        m_stats.parDepthRej += c[199];                   // v0.10.0 phase 6c (marching vote)
        m_stats.parMarches += c[200];
        m_stats.parMarchPx += c[201];
        m_stats.parRejMarches += c[202];
        m_stats.parNoSrcMarches += c[203];
        m_stats.parNoPixel += c[204];
        m_stats.parHolds += c[211];                      // v0.10.0 phase 14 round 4: plate holds / static-contact vetoes
        m_stats.parVetoes += c[212];
        m_stats.parClutter += c[213];
        m_stats.originFree += c[214];                    // round 6: origin-free paired draws resolved static
        m_stats.instSum += c[215];                       //   instance counts as the shaders saw them (sum, draws per count)
        for (int q = 0; q < 8; ++q) m_stats.instHist[q] += c[216 + q];
        m_stats.originDepthOnly += c[224];               // round 7: the origin-free rule's depth-only / lateral split
        m_stats.originMoverNear += c[229];
        for (int q = 0; q < 3; ++q) {                    //   max |x|, |y|, |w| among the origin-free draws (non-negative float bits)
            float v = 0.0f;
            memcpy(&v, &c[225 + q], 4);
            if (v > m_ofMax[q]) m_ofMax[q] = v;
        }
        if (c[228] != 0u) {                              //   the smallest own-pair mover |w| (stored as the bit complement)
            const uint32_t b = ~c[228];
            float v = 0.0f;
            memcpy(&v, &b, 4);
            if (m_ofMoverMinW < 0.0f || v < m_ofMoverMinW) m_ofMoverMinW = v;
        }
        if (c[230] != 0u) {                              //   the nearest lateral miss (bit complement)
            const uint32_t b = ~c[230];
            float v = 0.0f;
            memcpy(&v, &b, 4);
            if (m_ofMissMin < 0.0f || v < m_ofMissMin) m_ofMissMin = v;
        }
        for (uint32_t q = 0; q < m_rbInstN[k]; ++q) {    // v0.10.0 phase 8 (mv_inst_replay): instanced draws with a moving parent
            const uint32_t i = m_rbInstIdx[k * kRbInst + q];
            if ((c[256 + (i >> 5)] >> (i & 31)) & 1u) { InstMark(m_rbInstKey[k * kRbInst + q]); ++m_stats.instMoverDraws; }
        }
        m_rbInstN[k] = 0;
        m_stats.parVis += c[205];                        // v0.10.0 phase 8
        if (m_rbCompact[k]) {                            // v0.10.0 phase 18 (mv_vote_compact)
            ++m_stats.cvFrames;
            m_stats.cvTiles += c[768]; m_stats.cvGrid += m_rbGrid[k];
            m_stats.cvItems1 += c[769]; m_stats.cvItems2 += c[770];
            m_stats.cvItemOver += c[771]; m_stats.cvTileOver += c[772];
        }
        m_stats.parFine += c[206];
        if (c[205] == 0) ++m_stats.parSkipped;
        m_stats.twinViewSkip += c[63];
        uint64_t px[8] = {};
        for (int q = 0; q < 8; ++q) for (int s = 0; s < 16; ++s) px[q] += c[64 + q * 16 + s];
        m_stats.worldPx += px[0]; m_stats.idPx += px[1]; m_stats.moverPx += px[2]; m_stats.unpairedPx += px[3];
        m_stats.fwdPx += px[4]; m_stats.fwdIdPx += px[5]; m_stats.fwdMoverPx += px[6];
        m_stats.fwdAttachPx += px[7];                    // v0.10.0 phase 4: pass B forward px that took the mover's R
        // v0.10.0 phase 3: consensus camera R per layer
        for (int L = 0; L < 2; ++L) {
            const uint32_t* q = &c[L == 0 ? 20 : 28];
            m_lastCons[L] = (int)q[0];
            m_lastConsN[L] = q[1];
            // v0.10.0 phase 8: consensus health (mv_medoid_drop); a failure after a healthy run re-arms that layer's medoid
            const uint32_t minL = (L == 1 && g_consMinCabin) ? g_consMinCabin : (uint32_t)kConsMin;   // (phase 18)
            if (q[0] == 1 && q[1] >= minL) { if (m_consRun[L] < 0x7FFFFFFFu) ++m_consRun[L]; }
            else {
                if (g_medoidDrop && m_consRun[L] >= g_medoidDrop) ++m_stats.medoidRearms[L];
                m_consRun[L] = 0;
            }
            if (q[0] == 1) {
                ++m_stats.consFrames[L];
                m_stats.consCluster[L] += q[1];
                if (L == 0 && q[6]) ++m_stats.consNearVoters;
                if (q[5]) {
                    float avg = 0.0f;
                    memcpy(&avg, &q[2], 4);
                    if (!(avg >= 0.0f) || avg > 1e6f) avg = 1e6f;
                    ++m_stats.consDisFrames[L];
                    m_stats.consDisSum[L] += avg;
                    if (avg > 1.0f) ++m_stats.consDisOver1[L];
                    if (avg > m_consDisMax[L]) m_consDisMax[L] = avg;
                }
                if (L == 0 && !g_consLogged && !m_quiet) {
                    g_consLogged = true;
                    float avg = 0.0f;
                    memcpy(&avg, &q[2], 4);
                    Log("MV camera R: consensus -- the world camera R is now the largest cluster of per-draw R's that agree "
                        "within %.2f px (first frame: %u draws agree, %u candidates; |consensus - medoid| %.3f px avg at the "
                        "cluster's draws%s); the medoid of pass A is the fallback below %d draws (dlaa.ini mv_camr_consensus)",
                        (double)(g_snapPx > 0.05f ? g_snapPx : 0.05f), q[1], q[4], (double)avg,
                        q[5] ? "" : ", not compared this frame", kConsMin);
                }
            } else if (q[0] == 2 || q[0] == 4) {        // (v0.10.0 phase 8: 4 = a small cluster without a medoid)
                if (q[0] == 4) ++m_stats.consSmall[L];
                ++m_stats.consFallback[L];
                m_stats.consFallCluster[L] += q[1];      // v0.10.0 phase 18 (the cabin layer's cluster size, for the log)
            }
        }
        // v0.10.0 phase 14 (mv_replay_static_*): this pass's verdicts -> the static streak per FullKey. The camera R the verdicts
        // were measured against must be healthy: with the consensus on, a readback whose world layer did not use one (fallback,
        // small cluster, no pick) clears the table instead (every key is replayed again until it re-earns its streak).
        if (m_rbStatN[k] && m_rbStatKey) {
            const uint32_t nS = m_rbStatN[k];
            m_rbStatN[k] = 0;
            if (g_consensus && c[20] != 1) StaticClear(kStatClrCamR);
            else {
                if (++g_statTag == 0) g_statTag = 1;
                const uint32_t tag = g_statTag;
                const uint64_t src = m_rbFrame[k];
                const uint64_t* keys = &m_rbStatKey[(size_t)k * kMax];
                ++m_stats.statFeeds;
                m_stats.statShallow += c[210];           // round 3: static by the snap, but not deep (counted as moving)
                for (uint32_t i = 0; i < nS; ++i) {
                    const uint64_t key = keys[i];
                    if (!key) continue;                  // not a gate candidate (instanced / no MVP / cabin)
                    ++m_stats.statDraws;
                    const uint32_t w = i >> 5, b = 1u << (i & 31);
                    const bool par = (c[512 + w] & b) != 0;    // a rigid parent this pass: its plates need its pixels
                    const bool stat = (c[384 + w] & b) != 0;   // paired + static by its own pair
                    const bool mov = (c[640 + w] & b) != 0;    // moves (any route) / track reject
                    if (!par && !stat && !mov) {         // no partner: neither builds nor breaks the streak, but HOLDS the
                        ++m_stats.statNeutral;           // key (always re-drawn): an unpaired copy needs its pixels for the
                        StatEnt* n0 = StatFind(key, false);   // rigid-parent vote (a part of a moving car that shares a mesh
                        if (n0) n0->parent = src > 0 ? src : 1;   // with parked copies); a key not in the table is never skipped
                        continue;
                    }
                    StatEnt* e = StatFind(key, true);
                    if (!e) continue;                    // probe chain full: this key is never skipped
                    if (par) { e->parent = src > 0 ? src : 1; ++m_stats.statParents; }
                    if (src > e->seen) e->seen = src;
                    if (mov || !stat) {                  // (mov: a mover, a track reject, or round 3: only shallow-static)
                        e->streak = 0; e->badTag = tag;
                        if (mov) { ++m_stats.statMoving; e->moved = src + 1; }   // + the kStatQuiet guard (StaticSkip)
                        continue;
                    }
                    ++m_stats.statStatic;
                    // one increment per source frame (inc = frame + 1: 0 = never): two eyes of one frame count once
                    if (e->badTag != tag && e->inc != src + 1) { if (e->streak < 0xFFFFu) ++e->streak; e->inc = src + 1; }
                }
            }
        }
        if (m_rbHist[k]) {
            ++m_stats.rbHistFrames;
            const uint32_t elig = m_rbElig[k], paired = c[kStStatic] + c[kStMover];
            if (elig >= 16 && paired * 2u < elig) {
                ++m_stats.lowPairFrames;
                // v0.10.0 phase 14 round 5: a pairing collapse re-arms the medoid path (mv_medoid_drop). With both medoids dropped
                // pass A does not run, so CSGather predicts every draw with LAST frame's camera R; when the rotation speed changes
                // abruptly, far draws exceed mv_drawid_max_m and identical copies mis-pair (in game 2026-10-09: up to 31 frames per
                // 600 with 11-26 % paired; they flood the rigid-parent list -> a plate loses its parent). Pass A's fresh medoid
                // is back from the next pass on until the consensus has been healthy for mv_medoid_drop readbacks again.
                for (int L = 0; L < 2; ++L) {
                    if (g_medoidDrop && m_consRun[L] >= g_medoidDrop) ++m_stats.medoidRearms[L];
                    m_consRun[L] = 0;
                }
                if (!m_quiet && (m_lowPairLogs < 10 || (m_stats.lowPairFrames % 1000) == 0)) {
                    ++m_lowPairLogs;
                    Log("MV draw-ids: WARNING paired %u of %u draws (%.0f %%, < 50 %%) in one frame (%u unpaired, %u movers) "
                        "-- these draws use the camera motion vector (low-pair frames so far: %llu)", paired, elig,
                        100.0 * (double)paired / (double)elig, c[kStUnpaired], c[kStMover],
                        (unsigned long long)m_stats.lowPairFrames);
                }
            }
        }
    }
}

uint32_t DrawIdMv::Prepare(ID3D11DeviceContext* ctx, const DrawIdRecord* rec, uint32_t fullW, uint32_t fullH,
                           const DrawIdRecord* fwd, float nearM, bool freshMedoid, uint32_t noMedoid, bool idsBound) {
    m_prepN = 0; m_prepNG = 0; m_prepNF = 0;
    if (m_dumpPrep) { m_dumpPrep = false; m_dumpArm = true; }   // v0.10.0 phase 4: the dumped pass never finished: next one
    if (!ctx || !m_ready) return 0;
    Poll(ctx);
    const int64_t t0 = Qpc();
    if (!rec || !rec->Usable()) {
        m_prevN = 0;
        if (!m_quiet) StaticClear(kStatClrRecord);       // v0.10.0 phase 14: no history -> no static streak either
        return 0;
    }
    const uint32_t nG = (uint32_t)rec->Count();
    // v0.10.0 phase 14 (mv_replay_static_*): main units keep the FullKey of every gate candidate (world, not instanced / no MVP /
    // cabin) of this pass for the readback slot Finish fills; quiet (mirror) units never feed the table
    m_prepStatN = 0;
    if (!m_quiet && g_statFrames > 0u) {
        if (!m_prepStatKey) {
            m_prepStatKey = new (std::nothrow) uint64_t[kMax];
            m_rbStatKey = new (std::nothrow) uint64_t[(size_t)kRb * kMax];
            if (!m_prepStatKey || !m_rbStatKey) { delete[] m_prepStatKey; delete[] m_rbStatKey; m_prepStatKey = m_rbStatKey = nullptr; }
        }
        if (m_prepStatKey && !m_statReg) {               // PollMain may poll this unit's readbacks before its next blit
            for (DrawIdMv*& u : g_statUnit) if (!u) { u = this; m_statReg = true; break; }
        }
        if (m_prepStatKey) {
            const uint8_t notCand = DrawIdRecord::kFInstanced | DrawIdRecord::kFNoMvp | DrawIdRecord::kFCabin;
            for (uint32_t i = 0; i < nG; ++i) m_prepStatKey[i] = (rec->Flags((int)i) & notCand) ? 0ull : rec->FullKey((int)i);
            m_prepStatN = nG;
        }
    }
    // v0.10.0 phase 3: the pass's forward draws (ids nG + 1 ..) -- only a complete forward replay in this id space
    uint32_t nF = 0;
    if (fwd && fwd->Count() > 0) {
        if (fwd->Usable() && fwd->Segments() > 0 && fwd->PendingReplay() == 0 && !fwd->ReplayFailed() &&
            fwd->IdBase() == nG && nG < (uint32_t)kMax) {
            nF = (uint32_t)fwd->Count();
            if (nF > (uint32_t)kMax - nG) nF = (uint32_t)kMax - nG;
        } else {
            ++m_stats.fwdDropped;
        }
    }
    const uint32_t n = nG + nF;
    auto src = [&](uint32_t i) -> const DrawIdRecord* { return i < nG ? rec : fwd; };
    auto idx = [&](uint32_t i) -> int { return i < nG ? (int)i : (int)(i - nG); };
    const uint32_t m = m_prevN;
    const uint8_t kNoPair = DrawIdRecord::kFInstanced | DrawIdRecord::kFNoMvp;
    uint32_t elig = 0;
    // ---- group by the full key; orphans (one side only) by the pointer-free key ----
    uint32_t ne = 0;
    m_prepInstN = 0;                                     // v0.10.0 phase 8 (mv_inst_replay): the G-buffer instanced draws
    for (uint32_t i = 0; i < n; ++i) {
        if (g_instReplay && i < nG && (rec->Flags((int)i) & kNoPair) && m_prepInstN < (uint32_t)kRbInst) {
            m_prepInstIdx[m_prepInstN] = i; m_prepInstKey[m_prepInstN] = rec->ShapeKey((int)i); ++m_prepInstN;
        }
        m_curFull[i] = src(i)->FullKey(idx(i));
        m_curLoose[i] = src(i)->LooseKey(idx(i));
        m_curFlags[i] = (uint8_t)(src(i)->Flags(idx(i)) | (i >= nG ? DrawIdRecord::kFFwd : 0));
        m_curGroup[i] = kNone; m_curRank[i] = 0; m_curLooseG[i] = 0; m_curBig[i] = 0;
        if (m_curFlags[i] & kNoPair) continue;
        ++elig;
        m_ent[ne++] = { m_curFull[i], i };
    }
    for (uint32_t j = 0; j < m; ++j) {
        m_prevGroup[j] = kNone; m_prevRank[j] = 0;
        if (m_prevFlags[j] & kNoPair) continue;
        m_ent[ne++] = { m_prevFull[j], 0x80000000u | j };
    }
    auto byKey = [](const Ent& a, const Ent& b) { return a.key < b.key || (a.key == b.key && a.sideIdx < b.sideIdx); };
    std::sort(m_ent, m_ent + ne, byKey);
    constexpr uint32_t kGroupCap = 1024;                // larger groups: unpaired (drawn > 1024 times: static clutter)
    m_nGroups = 0; m_nList = 0;
    uint32_t no = 0;
    auto emit = [&](const Ent* e, uint32_t a, uint32_t b, bool loose) {
        uint32_t c = 0, p = 0;
        for (uint32_t q = a; q < b; ++q) { if (e[q].sideIdx & 0x80000000u) ++p; else ++c; }
        if (!c || !p) return false;
        if (c > kGroupCap || p > kGroupCap) {
            m_stats.bigGroup += c;
            for (uint32_t q = a; q < b; ++q) if (!(e[q].sideIdx & 0x80000000u)) m_curBig[e[q].sideIdx] = 1;   // phase 5 (dump)
            return true;
        }
        uint32_t* g = &m_groupData[m_nGroups * 4u];
        g[0] = m_nList; g[1] = c;
        uint32_t r = 0;
        for (uint32_t q = a; q < b; ++q) {
            if (e[q].sideIdx & 0x80000000u) continue;
            const uint32_t i = e[q].sideIdx;
            m_listData[m_nList++] = i;
            m_curGroup[i] = m_nGroups; m_curRank[i] = r++; m_curLooseG[i] = loose ? 1u : 0u;
        }
        g[2] = m_nList; g[3] = p;
        r = 0;
        for (uint32_t q = a; q < b; ++q) {
            if (!(e[q].sideIdx & 0x80000000u)) continue;
            const uint32_t j = e[q].sideIdx & 0x7FFFFFFFu;
            m_listData[m_nList++] = j;
            m_prevGroup[j] = m_nGroups; m_prevRank[j] = r++;
        }
        ++m_nGroups;
        return true;
    };
    for (uint32_t a = 0; a < ne;) {
        uint32_t b = a + 1;
        while (b < ne && m_ent[b].key == m_ent[a].key) ++b;
        if (!emit(m_ent, a, b, false)) {
            for (uint32_t q = a; q < b; ++q) {
                const uint32_t si = m_ent[q].sideIdx;
                const uint64_t lk = (si & 0x80000000u) ? m_prevLoose[si & 0x7FFFFFFFu] : m_curLoose[si];
                m_orph[no++] = { lk, si };
            }
        }
        a = b;
    }
    std::sort(m_orph, m_orph + no, byKey);
    for (uint32_t a = 0; a < no;) {
        uint32_t b = a + 1;
        while (b < no && m_orph[b].key == m_orph[a].key) ++b;
        emit(m_orph, a, b, true);
        a = b;
    }
    // ---- upload ----
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(ctx->Map(m_tab.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { m_prevN = 0; return 0; }
    {
        uint32_t* t = (uint32_t*)mp.pData;
        uint64_t looseN = 0, noGroup = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t fl = m_curFlags[i];
            uint32_t f = 0;
            if (fl & DrawIdRecord::kFInstanced) f |= 1u;
            if (fl & DrawIdRecord::kFNoMvp) f |= 2u;
            if (fl & DrawIdRecord::kFCabin) f |= 4u;
            if (m_curLooseG[i]) { f |= 256u; ++looseN; }
            if (fl & DrawIdRecord::kFFwd) f |= 512u;      // v0.10.0 phase 3: the MVP lives in the forward record's mirror
            if (!(fl & kNoPair) && m_curGroup[i] == kNone) ++noGroup;
            const float vmin = src(i)->VpMin(idx(i)), vmax = src(i)->VpMax(idx(i));
            t[i * 8 + 0] = src(i)->MirrorOff(idx(i));
            t[i * 8 + 1] = m_curGroup[i];
            t[i * 8 + 2] = f;
            t[i * 8 + 3] = m_curRank[i];
            memcpy(&t[i * 8 + 4], &vmin, 4);
            memcpy(&t[i * 8 + 5], &vmax, 4);
            // phase 14 round 4: .z = IndexCount (24 bits) | instance count (8 bits, capped), .w = viewport jitter (plate hold)
            t[i * 8 + 6] = (src(i)->IndexCount(idx(i)) & 0xFFFFFFu) | ((src(i)->InstanceCount(idx(i)) < 255u ? src(i)->InstanceCount(idx(i)) : 255u) << 24);
            t[i * 8 + 7] = src(i)->VpJitterPacked(idx(i));
        }
        m_stats.looseDraws += looseN;
        m_stats.noGroup += m != 0 ? noGroup : 0;
    }
    ctx->Unmap(m_tab.Get(), 0);
    if (m) {
        if (FAILED(ctx->Map(m_ptab.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { m_prevN = 0; return 0; }
        uint32_t* t = (uint32_t*)mp.pData;
        for (uint32_t j = 0; j < m; ++j) {
            t[j * 4 + 0] = m_prevGroup[j]; t[j * 4 + 1] = m_prevFlags[j]; t[j * 4 + 2] = m_prevRank[j]; t[j * 4 + 3] = 0;
        }
        ctx->Unmap(m_ptab.Get(), 0);
    }
    if (m_nGroups) {
        if (FAILED(ctx->Map(m_groups.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { m_prevN = 0; return 0; }
        memcpy(mp.pData, m_groupData, (size_t)m_nGroups * 16u);
        ctx->Unmap(m_groups.Get(), 0);
        if (FAILED(ctx->Map(m_lists.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { m_prevN = 0; return 0; }
        memcpy(mp.pData, m_listData, (size_t)m_nList * 4u);
        ctx->Unmap(m_lists.Get(), 0);
    }
    // ---- v0.10.0 phase 3: consensus candidates = evenly spaced G-buffer draws of each layer that have a partner group ----
    uint32_t cand[kCandWorld + kCandCabin] = {};
    uint32_t candW = 0, candC = 0;
    if (m && m_nGroups) {
        uint32_t* tmp = m_candTmp;
        for (int L = 0; L < 2; ++L) {
            uint32_t ne2 = 0;
            for (uint32_t i = 0; i < nG; ++i) {
                const uint8_t fl = m_curFlags[i];
                if ((fl & kNoPair) || m_curGroup[i] == kNone) continue;
                if (((fl & DrawIdRecord::kFCabin) != 0) != (L == 1)) continue;
                tmp[ne2++] = i;
            }
            // v0.10.0 phase 8: dlaa.ini mv_cons_cands (world; the cabin layer half of it, <= kCandCabin)
            const uint32_t capC = g_consCands / 2u < (uint32_t)kCandCabin ? g_consCands / 2u : (uint32_t)kCandCabin;
            const uint32_t cap = L == 0 ? g_consCands : capC;
            const uint32_t take = ne2 < cap ? ne2 : cap;
            for (uint32_t j = 0; j < take; ++j) cand[(L == 0 ? 0 : candW) + j] = tmp[(uint32_t)(((uint64_t)j * ne2) / take)];
            if (L == 0) candW = take; else candC = take;
        }
    }
    if (FAILED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { m_prevN = 0; return 0; }
    {
        const uint32_t counts[4] = { n, m, m_nGroups, nG };   // v0.10.0 phase 4: w = G-buffer draws (CSAttach)
        const float params[4] = { g_maxM, g_snapPx, (float)fullW, (float)fullH };
        const float params2[4] = { g_trackM, g_snapPx > 0.05f ? g_snapPx : 0.05f, nearM, g_attachM };   // w: phase 4
        // Cons.w: bit 0 consensus, bit 1 fresh medoid, bit 2 matrix twins (v0.10.0 phase 5), bits 3 / 4 no world / cabin medoid
        // this frame (v0.10.0 phase 8)
        const uint32_t cons[4] = { candW, candC, (uint32_t)kConsMin,
                                   (g_consensus ? 1u : 0u) | (freshMedoid ? 2u : 0u) | (g_twin ? 4u : 0u) | ((noMedoid & 3u) << 3) };
        memcpy(mp.pData, counts, sizeof(counts));
        memcpy((uint8_t*)mp.pData + 16, params, sizeof(params));
        memcpy((uint8_t*)mp.pData + 32, params2, sizeof(params2));
        memcpy((uint8_t*)mp.pData + 48, cons, sizeof(cons));
        memcpy((uint8_t*)mp.pData + 64, cand, sizeof(cand));
        // v0.10.0 phase 6: Par = (depth tolerance, smallest winning votes, smallest winning share, vote cap), ParU = (bit 0 on,
        // bit 1 the harness' conjugated-formula mutation; grid stride); the harness' "no depth test" mutation widens Par.x.
        // Phase 6c: ParU.z = march reach (px), ParU.w = march budget per listed draw and pass
        const float par[4] = { (g_parentMut & 2) ? 1e30f : kParDepthTol, kParMinVotes, kParMinShare, kParVoteCap };
        // phase 8: ParU.y = the vote grid stride (mv_vote_res), bit 3 = coarser than 2 px (fine draws on the 2 px grid)
        // v0.10.0 phase 14: bit 4 = this pass's G-buffer replay skipped long-static draws (id-0 world pixels = static sources)
        const uint32_t parU[4] = { (g_parent ? 1u : 0u) | ((g_parentMut & 1) ? 2u : 0u) | (g_instancedVote ? 4u : 0u) |
                                       (g_voteRes > kParStride ? 8u : 0u) | (rec->StaticSkipped() > 0 ? 16u : 0u),
                                   g_voteRes, g_parReach ? g_parReach : kParReachPx, kParMarchBudget };   // phase 6b: bit 2 = instanced / no-MVP vote
        static_assert(sizeof(cand) == 48u * 16u, "DidCB layout");
        memcpy((uint8_t*)mp.pData + 64 + sizeof(cand), par, sizeof(par));
        memcpy((uint8_t*)mp.pData + 64 + sizeof(cand) + 16, parU, sizeof(parU));
        // phase 6c: Par2.x edge continuity; phase 8: y = fine-draw threshold (coarse samples), z / w = the frame's march budget
        // in the 1st / 2nd pass (mv_vote_march_cap: 5/6 + 1/6; 0 = no frame cap)
        const float par2[4] = { (g_parentMut & 2) ? 1e30f : kParEdgeTol, kParFineSamples,
                                g_marchCap ? (float)g_marchCap * (5.0f / 6.0f) : 0.0f,
                                g_marchCap ? (float)g_marchCap * (1.0f / 6.0f) : 0.0f };
        memcpy((uint8_t*)mp.pData + 64 + sizeof(cand) + 32, par2, sizeof(par2));
        // v0.10.0 phase 9: Clip = the vote grid / pixel count rect (the G-buffer replay's clip, x0 / y0 down to a multiple of 4: the
        // samples stay on the global grid), FwdMode.x = the forward targets' resolution shift, FoldU.x = the folded-dispatch bits
        uint32_t clip[4] = { 0u, 0u, fullW, fullH };
        if (rec->Clipped()) {
            const D3D11_RECT r = rec->ClipRect();
            const uint32_t x0 = r.left > 0 ? (uint32_t)r.left : 0u, y0 = r.top > 0 ? (uint32_t)r.top : 0u;
            const uint32_t x1 = r.right > 0 ? (uint32_t)r.right : 0u, y1 = r.bottom > 0 ? (uint32_t)r.bottom : 0u;
            clip[0] = (x0 < fullW ? x0 : fullW) & ~3u; clip[1] = (y0 < fullH ? y0 : fullH) & ~3u;
            clip[2] = x1 < fullW ? x1 : fullW;         clip[3] = y1 < fullH ? y1 : fullH;
            if (clip[2] < clip[0]) clip[2] = clip[0];
            if (clip[3] < clip[1]) clip[3] = clip[1];
        }
        const uint32_t fwdMode[4] = { (nF && fwd) ? fwd->Shift() : 0u, g_consMinCabin, 0u, 0u };   // phase 18: y (cabin min)
        const bool pickOn = m && m_nGroups && (candW + candC) && g_consensus;
        const bool twinOn = g_twin && m, attachOn = g_attachM > 0.0f && m;
        const bool parentOn = g_parent && m && idsBound && fullW && fullH;
        // v0.10.0 phase 18: bit 5 = the compact vote (CSParentCount lists its tiles, CSParentList writes the collect args)
        ID3D11Device* devC = nullptr;
        if (parentOn && g_voteCompact && m_csParCollect && m_csParMarch && !m_parListUav && !m_parListFailed) ctx->GetDevice(&devC);
        if (devC) { EnsureParList(devC); devC->Release(); }
        const bool compact = parentOn && g_voteCompact && m_parListUav && m_marchArgsUav;
        const uint32_t fold = (pickOn ? 1u : 0u) | ((twinOn || attachOn) ? 2u : 0u) | (parentOn ? 4u : 0u) | (twinOn ? 8u : 0u) |
                              (attachOn ? 16u : 0u) | (compact ? 32u : 0u);
        m_prepCompact = compact;
        {
            const uint32_t S = g_voteRes, cw = clip[2] - clip[0], chh = clip[3] - clip[1];
            m_prepGrid = ((cw + S - 1u) / S + 7u) / 8u * (((chh + S - 1u) / S + 7u) / 8u);
        }
        uint32_t epsBits = 0;                            // v0.10.0 phase 14: z = the deep-static epsilon (px, float bits)
        memcpy(&epsBits, &g_statEps, 4);
        // phase 14 round 4: w bit 0 = the previous pass's plate-hold table is valid (that pass ran the rigid-parent picks)
        // round 6: bits 1 / 2 = HARNESS mutations (SetParentMutation bits 2 / 3: the origin-free rule off / multi-instance draws
        // listed for the vote again) -- never set by the DLL; round 7: bit 3 = HARNESS mutation (SetParentMutation bit 4: the
        // round-6 origin-free rule, |x|, |y|, |w| <= 1e-4 |z|)
        const uint32_t holdBits = ((parentOn && m_uhold[0] && m_uholdValid[1 - m_ping]) ? 1u : 0u) |
                                  ((g_parentMut & 4) ? 2u : 0u) | ((g_parentMut & 8) ? 4u : 0u) | ((g_parentMut & 16) ? 8u : 0u);
        const uint32_t foldU[4] = { fold, g_parMaxListed, epsBits, holdBits };   // v0.10.0 phase 10: y = the listed-draw cap
        memcpy((uint8_t*)mp.pData + 64 + sizeof(cand) + 48, clip, sizeof(clip));
        memcpy((uint8_t*)mp.pData + 64 + sizeof(cand) + 64, fwdMode, sizeof(fwdMode));
        memcpy((uint8_t*)mp.pData + 64 + sizeof(cand) + 80, foldU, sizeof(foldU));
        static_assert(64u + sizeof(cand) + 96u == kCbBytes, "DidCB layout (v0.10.0 phase 9)");
        memcpy(m_prepClip, clip, sizeof(clip));
        m_prepFold = fold;
        m_prepIds = idsBound;
    }
    ctx->Unmap(m_cb.Get(), 0);
    m_prepW = fullW; m_prepH = fullH;                    // v0.10.0 phase 6: the vote grid (full-size draw-id target)
    // ---- v0.10.0 phase 4 dump: the CPU facts of THIS pass's draws (the GPU side is copied at Finish). Phase 5: every draw --
    // DumpPoll picks the lines once the states are known (unpaired / inherited draws of any IndexCount) ----
    if (m_dumpArm && !m_dumpPending && m_dump) {
        m_dumpArm = false;
        m_dumpPrep = true;
        m_dumpN = 0; m_dumpSkipped = 0;
        m_dumpNG = nG; m_dumpNAll = n; m_dumpM = m; m_dumpW = fullW; m_dumpH = fullH; m_dumpHist = m != 0;
        for (uint32_t i = 0; i < n; ++i) {
            const DrawIdRecord* r = src(i);
            const int k = idx(i);
            const UINT ic = r->IndexCount(k);
            if (m_dumpN >= (uint32_t)kMax) { ++m_dumpSkipped; continue; }
            DumpEnt& d = m_dump[m_dumpN++];
            d.big = m_curBig[i];
            d.i = i; d.ic = ic; r->CbWindow(k, &d.cbFirst, &d.cbNum);
            d.mirrorOff = r->MirrorOff(k);
            d.group = m_curGroup[i];
            d.gCur = d.group != kNone ? m_groupData[d.group * 4u + 1u] : 0u;
            d.gPrev = d.group != kNone ? m_groupData[d.group * 4u + 3u] : 0u;
            d.full = m_curFull[i]; d.loose = m_curLoose[i];
            d.flags = (uint8_t)(m_curFlags[i] | (m_curLooseG[i] ? 0x80 : 0));
        }
    }
    m_prepN = n;
    m_prepNG = nG;
    m_prepNF = nF;
    m_prepCandW = candW;
    m_prepCandC = candC;
    m_prepElig = elig;
    m_prepHist = m != 0;
    if (noMedoid & 1u) ++m_stats.noMedoidFrames[0];      // v0.10.0 phase 8
    if (noMedoid & 2u) ++m_stats.noMedoidFrames[1];
    ++m_stats.frames;
    m_stats.draws += n;
    m_stats.fwdDraws += nF;
    m_stats.instanced += (uint64_t)rec->Instanced();
    m_stats.noMvp += (uint64_t)rec->NoMvp();
    if (m) ++m_stats.histFrames;
    m_stats.cpuTicks += (uint64_t)(Qpc() - t0);
    return n;
}

void DrawIdMv::Dispatch(ID3D11DeviceContext* ctx, const DrawIdRecord* rec, ID3D11ShaderResourceView* solveSrv,
                        const DrawIdRecord* fwd, ID3D11UnorderedAccessView* solveUav, ID3D11ShaderResourceView* idSrv,
                        ID3D11ShaderResourceView* depthSrv, ID3D11ShaderResourceView* fwdDepthSrv,
                        ID3D11ShaderResourceView* fwdIdSrv) {
    if (!m_prepN || !ctx || !rec || !solveSrv) return;
    const int tq = m_pairTimer.Begin(ctx);
    int pt = GpuPerf::Begin(ctx, GpuPerf::kGather);      // v0.10.0 phase 8: one timer per dispatch (Next = one timestamp)
    const UINT zero4[4] = { 0, 0, 0, 0 };
    ctx->ClearUnorderedAccessViewUint(m_cntUav.Get(), zero4);
    ID3D11Buffer* cb = m_cb.Get();
    // t7 = the forward record's mirror (v0.10.0 phase 3; only read for forward draws, so the G-buffer mirror stands in)
    ID3D11ShaderResourceView* fwdMirror = (m_prepNF && fwd && fwd->MirrorSrv()) ? fwd->MirrorSrv() : rec->MirrorSrv();
    ID3D11ShaderResourceView* srvs[8] = { rec->MirrorSrv(), m_tabSrv.Get(), m_ptabSrv.Get(), m_groupsSrv.Get(),
                                          m_listsSrv.Get(), solveSrv, m_mvpSrv[1 - m_ping].Get(), fwdMirror };
    ID3D11UnorderedAccessView* uavs[4] = { m_mvpUav[m_ping].Get(), m_workUav.Get(), m_rtabUav.Get(), m_cntUav.Get() };
    ID3D11ShaderResourceView* nullSrv[8] = {};
    ID3D11UnorderedAccessView* nullUav[5] = {};
    const UINT keep[5] = { (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1 };
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetShaderResources(0, 8, srvs);
    ctx->CSSetUnorderedAccessViews(0, 4, uavs, keep);
    const UINT n = m_prepN, m = m_prevN;
    ctx->CSSetShader(m_csGather.Get(), nullptr, 0);
    ctx->Dispatch((n + 63) / 64, 1, 1);
    if (m && m_nGroups) {
        pt = GpuPerf::Next(ctx, pt, GpuPerf::kMatch);
        ctx->CSSetShader(m_csMatch.Get(), nullptr, 0);
        ctx->Dispatch((n + m + 63) / 64, 1, 1);
    }
    pt = GpuPerf::Next(ctx, pt, GpuPerf::kPair);
    ctx->CSSetShader(m_csPair.Get(), nullptr, 0);
    ctx->Dispatch((n + 63) / 64, 1, 1);
    // v0.10.0 phase 3 consensus: vote (one group per candidate), then pick -- the solve buffer is a UAV for the pick only
    const UINT nCand = m_prepCandW + m_prepCandC;
    const bool pickOn = m && m_nGroups && nCand && solveUav && g_consensus;
    // v0.10.0 phase 9: the folded chain (CSPickResolve, CSListTwin) needs the solve UAV and the same id-target decision Prepare
    // wrote into FoldU; otherwise (and with mv_fold_dispatch 0) the phase-8 chain of separate dispatches runs
    const bool fold = g_foldMode >= 1 && solveUav && m_csPickRes && m_csListTwin && m_prepIds == (idSrv != nullptr) && depthSrv;
    // v0.10.0 phase 18 (mv_fold_dispatch 2): the list + twin + attach group as in phase 9, but the pick (statistic spread over a
    // group) and resolve + inherit (one thread per draw, one wide dispatch) in place of the single-group CSPickResolve
    const bool wide = fold && g_foldMode >= 2 && m_csPickPar && m_csResInh;
    if (pickOn) {
        pt = GpuPerf::Next(ctx, pt, GpuPerf::kVote);
        ctx->CSSetShader(m_csVote.Get(), nullptr, 0);
        ctx->Dispatch(nCand, 1, 1);
    }
    if (wide) {
        if (pickOn) {
            pt = GpuPerf::Next(ctx, pt, GpuPerf::kPick);
            ID3D11ShaderResourceView* noSolve = nullptr;
            ctx->CSSetShaderResources(5, 1, &noSolve);
            ctx->CSSetUnorderedAccessViews(4, 1, &solveUav, keep);
            ctx->CSSetShader(m_csPickPar.Get(), nullptr, 0);
            ctx->Dispatch(1, 1, 1);
            ctx->CSSetUnorderedAccessViews(4, 1, nullUav, keep);
            ctx->CSSetShaderResources(5, 1, &solveSrv);
        }
        pt = GpuPerf::Next(ctx, pt, GpuPerf::kResInh);
        ctx->CSSetShader(m_csResInh.Get(), nullptr, 0);
        ctx->Dispatch((n + 63) / 64, 1, 1);
    } else if (fold) {
        pt = GpuPerf::Next(ctx, pt, GpuPerf::kPickRes);
        ID3D11ShaderResourceView* noSolve = nullptr;
        ctx->CSSetShaderResources(5, 1, &noSolve);
        ctx->CSSetUnorderedAccessViews(4, 1, &solveUav, keep);
        ctx->CSSetShader(m_csPickRes.Get(), nullptr, 0);
        ctx->Dispatch(1, 1, 1);
        ctx->CSSetUnorderedAccessViews(4, 1, nullUav, keep);
        ctx->CSSetShaderResources(5, 1, &solveSrv);
    } else {
        if (pickOn) {
            pt = GpuPerf::Next(ctx, pt, GpuPerf::kPick);
            ID3D11ShaderResourceView* noSolve = nullptr;
            ctx->CSSetShaderResources(5, 1, &noSolve);
            ctx->CSSetUnorderedAccessViews(4, 1, &solveUav, keep);
            ctx->CSSetShader(m_csPick.Get(), nullptr, 0);
            ctx->Dispatch(1, 1, 1);
            ctx->CSSetUnorderedAccessViews(4, 1, nullUav, keep);
            ctx->CSSetShaderResources(5, 1, &solveSrv);
        }
        pt = GpuPerf::Next(ctx, pt, GpuPerf::kResolve);
        ctx->CSSetShader(m_csResolve.Get(), nullptr, 0);
        ctx->Dispatch((n + 63) / 64, 1, 1);
    }
    // v0.10.0 phase 5: unpaired draws inherit their matrix twin's R (CSInherit1 fills the table + the mover list, CSTwin looks
    // up); then (phase 4, extended) forward draws and still-unpaired G-buffer draws on a G-buffer mover's origin take its R.
    // Nothing pairs without a previous pass (m == 0): no sources, no movers -> skipped (CSGather cleared both columns).
    const bool twinOn = g_twin && m != 0;
    const bool attachOn = g_attachM > 0.0f && m != 0;
    // v0.10.0 phase 6: rigid parents need the pass's draw-id target and scene depth (pass B's inputs) and a previous pass
    const bool parentOn = g_parent && m != 0 && idSrv && depthSrv && m_prepW && m_prepH;
    if ((twinOn || attachOn) && !fold) {                 // (v0.10.0 phase 9: folded into CSPickResolve)
        pt = GpuPerf::Next(ctx, pt, GpuPerf::kInherit);
        ctx->CSSetShader(m_csInherit1.Get(), nullptr, 0);
        ctx->Dispatch((n + 63) / 64, 1, 1);
    }
    // v0.10.0 phase 18 (mv_vote_compact): the count lists the vote tiles into m_parList (u7, bound until the votes are done); the
    // march args (2 x (0 groups, 1, 1)) are reset here, CSParentCollect counts them up
    const bool compact = parentOn && m_prepCompact && m_parListUav && m_marchArgsUav && m_csParCollect && m_csParMarch;
    if (compact) {
        static const UINT kMarchArgs0[8] = { 0u, 1u, 1u, 0u, 0u, 1u, 1u, 0u };
        ctx->UpdateSubresource(m_marchArgs.Get(), 0, nullptr, kMarchArgs0, 0, 0);
        ID3D11UnorderedAccessView* lu = m_parListUav.Get();
        ctx->CSSetUnorderedAccessViews(7, 1, &lu, keep);
    }
    if (parentOn) {                                      // v0.10.0 phase 6: the U list (state 1 = unpaired by its own pair)
        // v0.10.0 phase 6b: count each replayed instanced / no-MVP draw's on-screen pixels (needs the id target, t8) so the U
        // list can give the 64 slots to the instanced draws that cover the screen (mv_drawid_instanced 1). Phase 6c: it runs
        // whenever the parents are on -- it also counts each genuine unpaired draw's pixels (its march decimation, ParDecim).
        // v0.10.0 phase 8: on the mv_vote_res grid (4: a quarter of the phase-6c samples)
        // v0.10.0 phase 9: over the replay's clip only (m_prepClip); t11 = the 1/2-resolution forward ids (FwdMode.x 1)
        pt = GpuPerf::Next(ctx, pt, GpuPerf::kParCount);
        ctx->CSSetShaderResources(8, 1, &idSrv);
        ctx->CSSetShaderResources(11, 1, &fwdIdSrv);
        ctx->CSSetShader(m_csParCount.Get(), nullptr, 0);
        const UINT cw = m_prepClip[2] - m_prepClip[0], chh = m_prepClip[3] - m_prepClip[1];
        const UINT cgw = (cw + g_voteRes - 1) / g_voteRes, cgh = (chh + g_voteRes - 1) / g_voteRes;
        if (cgw && cgh) ctx->Dispatch((cgw + 7) / 8, (cgh + 7) / 8, 1);
        ctx->CSSetShaderResources(8, 1, nullSrv);
        ctx->CSSetShaderResources(11, 1, nullSrv);
    }
    if (fold && (parentOn || twinOn || attachOn)) {      // v0.10.0 phase 9: list + twin + attach in one group
        pt = GpuPerf::Next(ctx, pt, GpuPerf::kListTwin);
        ID3D11UnorderedAccessView* argsUav = m_parArgsUav.Get();
        if (parentOn) ctx->CSSetUnorderedAccessViews(5, 1, &argsUav, keep);
        ctx->CSSetShader(m_csListTwin.Get(), nullptr, 0);
        ctx->Dispatch(1, 1, 1);
        if (parentOn) ctx->CSSetUnorderedAccessViews(5, 1, nullUav, keep);
    } else if (!fold) {
        if (parentOn) {
            // v0.10.0 phase 8: CSParentList also writes the DispatchIndirect args of the votes (u5, bound for this dispatch only)
            pt = GpuPerf::Next(ctx, pt, GpuPerf::kParList);
            ID3D11UnorderedAccessView* argsUav = m_parArgsUav.Get();
            ctx->CSSetUnorderedAccessViews(5, 1, &argsUav, keep);
            ctx->CSSetShader(m_csParList.Get(), nullptr, 0);
            ctx->Dispatch(1, 1, 1);
            ctx->CSSetUnorderedAccessViews(5, 1, nullUav, keep);
        }
        if (twinOn) {
            pt = GpuPerf::Next(ctx, pt, GpuPerf::kTwin);
            ctx->CSSetShader(m_csTwin.Get(), nullptr, 0);
            ctx->Dispatch((n + 63) / 64, 1, 1);
        }
        if (attachOn) {
            pt = GpuPerf::Next(ctx, pt, GpuPerf::kAttach);
            ctx->CSSetShader(m_csAttach.Get(), nullptr, 0);
            ctx->Dispatch((n + 63) / 64, 1, 1);
        }
    }
    // v0.10.0 phase 6: the vote over the draw-id target (every kParStride-th pixel in x and y), then the picks; t8..t10 only for
    // these dispatches. Phase 6c: the MARCHING vote runs twice -- vote, pick 1 (sets the pass flag), vote again for the listed
    // draws still without a parent (the listed draws that now hold a motion are sources), pick 2
    // v0.10.0 phase 8: the vote grid is DispatchIndirect (args from CSParentList: no groups when no listed draw has pixels; the
    // picks are 1 group each and always run -- they keep the per-draw reasons / statistics exactly as before)
    if (parentOn) {
        ID3D11ShaderResourceView* psrv[4] = { idSrv, depthSrv, fwdDepthSrv, fwdIdSrv };   // (v0.10.0 phase 9: + t11)
        ctx->CSSetShaderResources(8, 4, psrv);
        for (int pass = 0; pass < 2; ++pass) {
            if (compact) {
                // v0.10.0 phase 18: the march samples of this pass -> items (args from CSParentList: the listed tiles), then one
                // thread per item and direction (args counted by the collect; u5 = the march args for the collect only)
                pt = GpuPerf::Next(ctx, pt, pass == 0 ? GpuPerf::kParCol1 : GpuPerf::kParCol2);
                ID3D11UnorderedAccessView* mu = m_marchArgsUav.Get();
                ctx->CSSetUnorderedAccessViews(5, 1, &mu, keep);
                ctx->CSSetShader(m_csParCollect.Get(), nullptr, 0);
                ctx->DispatchIndirect(m_parArgs.Get(), 0);
                ctx->CSSetUnorderedAccessViews(5, 1, nullUav, keep);
                pt = GpuPerf::Next(ctx, pt, pass == 0 ? GpuPerf::kParVote1 : GpuPerf::kParVote2);
                ctx->CSSetShader(m_csParMarch.Get(), nullptr, 0);
                ctx->DispatchIndirect(m_marchArgs.Get(), pass == 0 ? 0u : 16u);
            } else {
                pt = GpuPerf::Next(ctx, pt, pass == 0 ? GpuPerf::kParVote1 : GpuPerf::kParVote2);
                ctx->CSSetShader(m_csParVote.Get(), nullptr, 0);
                ctx->DispatchIndirect(m_parArgs.Get(), 0);
            }
            pt = GpuPerf::Next(ctx, pt, pass == 0 ? GpuPerf::kParPick1 : GpuPerf::kParPick2);
            ctx->CSSetShader(pass == 0 ? m_csParPick1.Get() : m_csParPick2.Get(), nullptr, 0);
            // v0.10.0 phase 14 round 4: both picks read the previous pass's listed-draw table (t12: the temporal check of a moving
            // candidate); the 2nd pick also runs the plate hold and writes this pass's table (u6)
            const bool hold = m_uholdUav[0] && m_uholdUav[1];
            if (hold) {
                ID3D11ShaderResourceView* hs = m_uholdSrv[1 - m_ping].Get();
                ctx->CSSetShaderResources(12, 1, &hs);
                if (pass == 1) { ID3D11UnorderedAccessView* hu = m_uholdUav[m_ping].Get(); ctx->CSSetUnorderedAccessViews(6, 1, &hu, keep); }
            }
            ctx->Dispatch(1, 1, 1);
            if (hold) {
                ID3D11ShaderResourceView* ns = nullptr;
                ctx->CSSetShaderResources(12, 1, &ns);
                if (pass == 1) {
                    ID3D11UnorderedAccessView* nu = nullptr;
                    ctx->CSSetUnorderedAccessViews(6, 1, &nu, keep);
                    m_uholdValid[m_ping] = true;
                }
            }
        }
        ctx->CSSetShaderResources(8, 4, nullSrv);
    } else {
        m_uholdValid[m_ping] = false;                    // phase 14 round 4: no picks this pass -> no hold table for the next one
    }
    if (compact) ctx->CSSetUnorderedAccessViews(7, 1, nullUav, keep);   // v0.10.0 phase 18
    if (m_dumpPrep) { m_dumpSolve.Reset(); solveSrv->GetResource(&m_dumpSolve); }
    ctx->CSSetShaderResources(0, 8, nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 4, nullUav, keep);
    GpuPerf::End(ctx, pt);
    m_pairTimer.End(ctx, tq);
}

void DrawIdMv::Finish(ID3D11DeviceContext* ctx) {
    if (!m_prepN || !ctx) return;
    // diagnostics readback (non-blocking; the hot path never waits for it)
    for (int k = 0; k < kRb; ++k) {
        if (m_rbPending[k]) continue;
        ctx->CopyResource(m_rb[k].Get(), m_cnt.Get());
        m_rbPending[k] = true;
        m_rbHist[k] = m_prepHist;
        m_rbN[k] = m_prepN;
        m_rbCompact[k] = m_prepCompact;                  // v0.10.0 phase 18
        m_rbGrid[k] = m_prepGrid;
        m_rbElig[k] = m_prepElig;
        m_rbSeq[k] = ++m_rbCtr;
        m_rbInstN[k] = g_instReplay ? m_prepInstN : 0;   // v0.10.0 phase 8 (mv_inst_replay): the instanced draws' shape keys
        for (uint32_t q = 0; q < m_rbInstN[k]; ++q) {
            m_rbInstIdx[k * kRbInst + q] = m_prepInstIdx[q];
            m_rbInstKey[k * kRbInst + q] = m_prepInstKey[q];
        }
        // v0.10.0 phase 14 (mv_replay_static_*): the gate candidates' keys + the frame, for the static streaks (Poll)
        m_rbStatN[k] = (m_prepStatKey && m_rbStatKey && m_prepStatN <= (uint32_t)m_prepNG) ? m_prepStatN : 0;
        if (m_rbStatN[k]) memcpy(&m_rbStatKey[(size_t)k * kMax], m_prepStatKey, (size_t)m_rbStatN[k] * sizeof(uint64_t));
        m_rbFrame[k] = g_instFrame;
        break;
    }
    // v0.10.0 phase 4 dump: this pass's GPU tables -> staging (read by DumpPoll without waiting)
    if (m_dumpPrep) {
        m_dumpPrep = false;
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        const UINT kM = (UINT)kMax;
        // phase 5: + the twin column; phase 6: + the parent and votes columns
        const UINT sizes[4] = { kM * 80u, kM * kRStride * 16u, kM * 84u, 512u };
        bool ok = dev && m_dumpSolve;
        for (int k = 0; ok && k < 4; ++k) {
            if (m_dumpSt[k]) continue;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = sizes[k]; bd.Usage = D3D11_USAGE_STAGING; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &m_dumpSt[k]));
        }
        if (ok) {
            const UINT n = m_prepN;
            D3D11_BOX b{ 0, 0, 0, n * 80u, 1, 1 };
            ctx->CopySubresourceRegion(m_dumpSt[0].Get(), 0, 0, 0, 0, m_mvp[m_ping].Get(), 0, &b);
            b.right = n * kRStride * 16u;
            ctx->CopySubresourceRegion(m_dumpSt[1].Get(), 0, 0, 0, 0, m_rtab.Get(), 0, &b);
            const UINT kRd = kM * 96u, kPartner = kM * 160u, kAttach = kM * 164u + 4096u;   // = the shader's Work layout
            const UINT kTwin = kM * 168u + 4096u;                                           // v0.10.0 phase 5
            b.left = kPartner; b.right = kPartner + n * 4u;
            ctx->CopySubresourceRegion(m_dumpSt[2].Get(), 0, 0, 0, 0, m_work.Get(), 0, &b);
            b.left = kRd; b.right = kRd + n * 64u;
            ctx->CopySubresourceRegion(m_dumpSt[2].Get(), 0, kM * 4u, 0, 0, m_work.Get(), 0, &b);
            b.left = kAttach; b.right = kAttach + n * 4u;
            ctx->CopySubresourceRegion(m_dumpSt[2].Get(), 0, kM * 68u, 0, 0, m_work.Get(), 0, &b);
            b.left = kTwin; b.right = kTwin + n * 4u;
            ctx->CopySubresourceRegion(m_dumpSt[2].Get(), 0, kM * 72u, 0, 0, m_work.Get(), 0, &b);
            const UINT kParent = kM * 188u + 8192u, kParVT = kM * 192u + 8192u;           // v0.10.0 phase 6
            b.left = kParent; b.right = kParent + n * 4u;
            ctx->CopySubresourceRegion(m_dumpSt[2].Get(), 0, kM * 76u, 0, 0, m_work.Get(), 0, &b);
            b.left = kParVT; b.right = kParVT + n * 4u;
            ctx->CopySubresourceRegion(m_dumpSt[2].Get(), 0, kM * 80u, 0, 0, m_work.Get(), 0, &b);
            b.left = 0; b.right = 8u * 16u;               // the camera R rows: world [0..3], cabin [4..7]
            ctx->CopySubresourceRegion(m_dumpSt[3].Get(), 0, 0, 0, 0, m_dumpSolve.Get(), 0, &b);
            m_dumpPending = true;
            m_dumpPolls = 0;
        } else {
            Log("MV draw-ids dump: staging buffers unavailable -- no dump");
        }
        m_dumpSolve.Reset();
    }
    // commit: this pass becomes the eye's previous one
    memcpy(m_prevFull, m_curFull, sizeof(uint64_t) * m_prepN);
    memcpy(m_prevLoose, m_curLoose, sizeof(uint64_t) * m_prepN);
    memcpy(m_prevFlags, m_curFlags, m_prepN);
    m_prevN = m_prepN;
    m_ping = 1 - m_ping;
    m_prepN = 0;
    m_prepNG = 0; m_prepNF = 0; m_prepCandW = 0; m_prepCandC = 0;
}

// ---- v0.10.0 phase 4 DUMP ----------------------------------------------------------------------------------------------
bool DrawIdMv::RequestDump(const char* why) {
    if (DumpBusy()) return false;
    if (!m_dump) {
        m_dump = new (std::nothrow) DumpEnt[kMax];       // v0.10.0 phase 5: every draw of the pass
        if (!m_dump) return false;
    }
    snprintf(m_dumpWhy, sizeof(m_dumpWhy), "%s", why ? why : "");
    m_dumpArm = true;
    return true;
}

// Reads the staged tables of the dumped pass without waiting (retried at the next Prepare) and writes the lines.
void DrawIdMv::DumpPoll(ID3D11DeviceContext* ctx) {
    D3D11_MAPPED_SUBRESOURCE mp[4] = {};
    int mapped = 0;
    for (; mapped < 4; ++mapped) {
        const HRESULT hr = ctx->Map(m_dumpSt[mapped].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mp[mapped]);
        if (hr != S_OK) break;
    }
    if (mapped < 4) {
        for (int k = 0; k < mapped; ++k) ctx->Unmap(m_dumpSt[k].Get(), 0);
        if (++m_dumpPolls > 600) { m_dumpPending = false; Log("MV draw-ids dump: the readback never landed -- dropped"); }
        return;
    }
    m_dumpPending = false;
    ++m_stats.dumps;
    const uint8_t* M = (const uint8_t*)mp[0].pData;           // CurM: 80 B per draw (MVP rows + track)
    const float* RT = (const float*)mp[1].pData;              // RTab: 5 float4 per draw
    // partner [kMax * 4] | R_draw [kMax * 64] | attach [kMax * 4] | twin [kMax * 4] (v0.10.0 phase 5) | parent [kMax * 4] |
    // votes [kMax * 4] (v0.10.0 phase 6)
    const uint8_t* W = (const uint8_t*)mp[2].pData;
    const float* S = (const float*)mp[3].pData;               // camera R rows: world [0..15], cabin [16..31]
    const uint32_t kM = (uint32_t)kMax;
    const uint32_t nF = m_dumpNAll - m_dumpNG;
    // v0.10.0 phase 5: which draws get a line -- every forward draw, the G-buffer draws with IndexCount <= 36 (phase 4) and
    // EVERY draw that came out of the pairing unpaired (any IndexCount) or inherited a twin's / mover's R
    auto col = [&](uint32_t base, uint32_t i) { uint32_t v = 0; memcpy(&v, W + (size_t)base + (size_t)i * 4u, 4); return v; };
    auto stateOf = [&](uint32_t i) { return (uint32_t)(RT[((size_t)i * 5u + 4u) * 4u] + 0.5f); };
    // state 1 before the inheritance: still unpaired, or a twin / a G-buffer origin attach fired
    // (v0.10.0 phase 6: or a rigid parent's -- column kM * 76)
    auto wasUnpaired = [&](uint32_t i) {
        return stateOf(i) == 1u || col(kM * 72u, i) != 0xFFFFFFFFu || (col(kM * 68u, i) != 0xFFFFFFFFu && i < m_dumpNG) ||
               col(kM * 76u, i) != 0xFFFFFFFFu;
    };
    // (v0.10.0 phase 14 round 6: + every MOVER of any IndexCount -- a false mover names itself in the next in-game dump)
    auto selected = [&](const DumpEnt& d) {
        return d.i >= m_dumpNG || d.ic <= 36u || wasUnpaired(d.i) || col(kM * 68u, d.i) != 0xFFFFFFFFu || stateOf(d.i) == 3u;
    };
    uint32_t nSel = 0, nUnp = 0, nTwin = 0, nAtt = 0, nLeft = 0, nPar = 0;
    for (uint32_t q = 0; q < m_dumpN; ++q) {
        const uint32_t i = m_dump[q].i;
        if (wasUnpaired(i)) {
            ++nUnp;
            if (col(kM * 76u, i) != 0xFFFFFFFFu) ++nPar;           // phase 6: the vote decides over a twin / origin attach
            else if (col(kM * 72u, i) != 0xFFFFFFFFu) ++nTwin;
            else if (col(kM * 68u, i) != 0xFFFFFFFFu) ++nAtt;
            else ++nLeft;
        }
        if (selected(m_dump[q])) ++nSel;
    }
    Log("MV draw-ids dump (%s): %u draws in the table (%u G-buffer + %u forward, previous pass %u draws%s) -- the %u forward draws, "
        "the G-buffer draws with IndexCount <= 36, every mover and every unpaired / inherited draw follow (%u lines%s); unpaired after the "
        "pairing %u: %u took their rigid parent's R by the pixel-neighbour vote (mv_drawid_parent %d), %u their matrix twin's R "
        "(mv_drawid_twin %d), %u a G-buffer mover's by origin, %u left unpaired (camera R); attach radius %.2f m, snap %.2f px | "
        "camera R world "
        "rows [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f]", m_dumpWhy, m_dumpNAll,
        m_dumpNG, nF, m_dumpM, m_dumpHist ? "" : ", NO history: nothing can pair", nF, nSel,
        m_dumpSkipped ? " -- more skipped, table cap" : "", nUnp, nPar, (int)g_parent, nTwin, (int)g_twin, nAtt, nLeft,
        (double)g_attachM,
        (double)g_snapPx, S[0], S[1], S[2], S[3], S[4], S[5], S[6], S[7], S[8], S[9], S[10], S[11], S[12], S[13], S[14], S[15]);
    static const char* const kSt[] = { "?", "UNPAIRED (camera R)", "static (camera R)", "MOVER (own R)", "instanced (camera R)",
                                       "no MVP (camera R)" };
    static const char* const kStShort[] = { "?", "unpaired", "static", "mover", "instanced", "no MVP" };
    uint32_t line = 0;
    const double fw = (double)m_dumpW, fh = (double)m_dumpH;
    auto toPx = [&](const double c[4], double* x, double* y) {
        if (!(c[3] > 1e-6)) return false;
        *x = (c[0] / c[3] * 0.5 + 0.5) * fw; *y = (0.5 - c[1] / c[3] * 0.5) * fh;
        return true;
    };
    for (uint32_t q = 0; q < m_dumpN; ++q) {
        const DumpEnt& d = m_dump[q];
        const uint32_t i = d.i;
        if (!selected(d)) continue;
        ++line;
        const uint32_t twin = col(kM * 72u, i);
        const float* m = (const float*)(M + (size_t)i * 80u);    // rows: m[r * 4 + c]
        const float* tr = m + 16;                                 // track: disp x, y, w, flag (bits)
        uint32_t trFlag = 0; memcpy(&trFlag, &tr[3], 4);
        const float* info = RT + ((size_t)i * 5u + 4u) * 4u;
        uint32_t partner = 0, attach = 0;
        memcpy(&partner, W + (size_t)i * 4u, 4);
        memcpy(&attach, W + (size_t)kM * 68u + (size_t)i * 4u, 4);
        const float* rd = (const float*)(W + (size_t)kM * 4u + (size_t)i * 64u);
        const bool cab = (d.flags & DrawIdRecord::kFCabin) != 0;
        const float* cr = S + (cab ? 16 : 0);
        const double o[4] = { m[3], m[7], m[11], m[15] };         // the MVP origin in clip space (column 3)
        double ox = 0, oy = 0;
        const bool oOn = toPx(o, &ox, &oy);
        char pairTxt[160] = "no partner";
        char devTxt[96] = "";
        if (partner != 0xFFFFFFFFu) {
            snprintf(pairTxt, sizeof(pairTxt), "partner %u (prev), origin displacement vs the camera prediction %.3f %.3f w %.3f%s",
                     partner, (double)tr[0], (double)tr[1], (double)tr[2], trFlag == 1u ? "" : " (no track)");
            double a[4], b[4];
            for (int r = 0; r < 4; ++r) {
                a[r] = rd[r * 4] * o[0] + rd[r * 4 + 1] * o[1] + rd[r * 4 + 2] * o[2] + rd[r * 4 + 3] * o[3];
                b[r] = cr[r * 4] * o[0] + cr[r * 4 + 1] * o[1] + cr[r * 4 + 2] * o[2] + cr[r * 4 + 3] * o[3];
            }
            double ax, ay, bx, by;
            if (toPx(a, &ax, &ay) && toPx(b, &bx, &by))
                snprintf(devTxt, sizeof(devTxt), ", R_draw - camR at the origin %.3f px", std::sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by)));
            else snprintf(devTxt, sizeof(devTxt), ", R_draw - camR at the origin: off-screen / behind");
        }
        const uint32_t st = (uint32_t)(info[0] + 0.5f);
        char attTxt[64] = "";
        if (attach != 0xFFFFFFFFu) snprintf(attTxt, sizeof(attTxt), " | ATTACHED to G-buffer mover id %u", attach + 1u);
        // v0.10.0 phase 5: why the pairing left it unpaired, and what it inherited
        char unpTxt[384] = "";
        if (wasUnpaired(i) && st != 4u && st != 5u) {
            const char* why = d.group == kNone ? (d.big ? "oversized group (more than 1024 copies)"
                                                        : (m_dumpHist ? "no partner group (full + loose key new this frame)"
                                                                      : "no previous pass"))
                            : (partner == 0xFFFFFFFFu ? "no mutual partner / displacement over mv_drawid_max_m"
                                                      : "track rejected (continuity guard: identical copies)");
            // v0.10.0 phase 6: the rigid parent (pixel-neighbour vote) decides over a twin / origin attach
            const uint32_t par = col(kM * 76u, i), vt = col(kM * 80u, i);
            if (twin != 0xFFFFFFFFu)
                snprintf(unpTxt, sizeof(unpTxt), " | unpaired: %s | twin -> id %u %s (%s, identical MVP)", why, twin + 1u,
                         twin < m_dumpNG ? "GBUF" : "FWD", par != 0xFFFFFFFFu ? "overridden by the parent" : kStShort[st <= 5u ? st : 0u]);
            else if (attach != 0xFFFFFFFFu)
                snprintf(unpTxt, sizeof(unpTxt), " | unpaired: %s | no twin", why);
            else
                snprintf(unpTxt, sizeof(unpTxt), " | unpaired: %s | no twin, no G-buffer mover within %.2f m with its orientation",
                         why, (double)g_attachM);
            const size_t ul = strlen(unpTxt);
            if (par != 0xFFFFFFFFu && par >= (uint32_t)kMax)    // v0.10.0 phase 14: the pseudo static source (kParPseudo)
                snprintf(unpTxt + ul, sizeof(unpTxt) - ul, " | parent -> static pixels the static gate did not re-draw (votes %u/%u%s)",
                         (vt >> 16) & 0x7FFFu, vt & 0xFFFFu, (vt & 0x80000000u) ? ", 2nd pass" : "");
            else if (par != 0xFFFFFFFFu)
                snprintf(unpTxt + ul, sizeof(unpTxt) - ul, " | parent -> id %u %s (votes %u/%u, %s%s)", par + 1u,
                         par < m_dumpNG ? "GBUF" : "FWD", (vt >> 16) & 0x7FFFu, vt & 0xFFFFu, st == 3u ? "rigid" : "static",
                         (vt & 0x80000000u) ? ", 2nd pass: via a listed neighbour that took its motion first" : "");
            else if (g_parent && m_dumpHist) {   // phase 6c: + the reason (kParVT bits 31 / 30 when no vote was accepted)
                const uint32_t why6c = vt & 0xC0000000u;
                snprintf(unpTxt + ul, sizeof(unpTxt) - ul, " | no parent (best %u of %u votes: %s)",
                         (vt & 0xFFFFu) ? (vt >> 16) & 0x7FFFu : 0u, vt & 0xFFFFu,
                         (vt & 0xFFFFu) ? "no majority, needs >= 8 votes and >= 50 %"
                         : why6c == 0x80000000u ? "depth-rejected, every source within 32 px at another depth"
                         : why6c == 0x40000000u ? "no source within 32 px"
                         : why6c == 0xC0000000u ? "no pixel of it sampled: hidden / sub-pixel" : "not listed, over the 64 cap");
            }
        }
        // v0.10.0 phase 6: column 3 = (0, 0, c, 0) exactly -> rows 4..7 are the bare projection (view-space vertices)
        const bool viewSpace = m[3] == 0.0f && m[7] == 0.0f && m[15] == 0.0f;
        // v0.10.0 phase 14 round 7: column 3 at full precision + the origin-free verdict of the CURRENT matrix (the shader's
        // OriginFreeCol: depth part |w| <= 0.20 m, lateral part |x|, |y| <= 0.50 clip; keep in step with kOFreeW / kOFreeXY)
        const bool ofDepth = std::fabs(o[2]) > 1e-6 && std::fabs(o[3]) <= 0.20;
        const bool ofLat = std::fabs(o[0]) <= 0.50 && std::fabs(o[1]) <= 0.50;
        const char* ofTxt = ofDepth ? (ofLat ? "yes" : "depth only, lateral over 0.50") : "no";
        Log("MV dump %u/%u: id %u %s ic %u flags 0x%02x%s%s | full %016llx loose %016llx | cb0 first %u num %u -> mirror +%u | "
            "group %d (%u cur / %u prev) | origin clip %.3f %.3f %.3f w %.3f%s px %.1f %.1f | col3 x %.4e y %.4e z %.4e w %.4e "
            "origin-free %s | %s%s | probe dev %.3f px%s | state %u "
            "%s%s%s",
            line, nSel, i + 1u, i < m_dumpNG ? "GBUF" : "FWD", d.ic, d.flags & 0x7Fu, cab ? " cabin" : "",
            (d.flags & 0x80) ? " loose-key" : "", (unsigned long long)d.full, (unsigned long long)d.loose, d.cbFirst, d.cbNum,
            d.mirrorOff, d.group == kNone ? -1 : (int)d.group, d.gCur, d.gPrev, o[0], o[1], o[2], o[3],
            viewSpace ? " (view space: MVP = projection only)" : (oOn ? "" : " (behind)"), oOn ? ox : -1.0, oOn ? oy : -1.0,
            o[0], o[1], o[2], o[3], ofTxt, pairTxt,
            devTxt, info[3] < 0.0f ? 0.0 : (double)info[3],
            (st == 2u && partner != 0xFFFFFFFFu && twin == 0xFFFFFFFFu) ? " (snapped to the camera R)" : "", st,
            st <= 5u ? kSt[st] : "?", unpTxt, attTxt);
    }
    for (int k = 0; k < 4; ++k) ctx->Unmap(m_dumpSt[k].Get(), 0);
    Log("MV draw-ids dump (%s): done", m_dumpWhy);
}

#endif // WITH_DLAA
