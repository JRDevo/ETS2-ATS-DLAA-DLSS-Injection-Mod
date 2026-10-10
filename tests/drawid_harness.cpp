// v0.10.0 WARP harness for the per-draw motion vectors (mv_objects = 2): DrawIdRecord (record + replay with depth EQUAL),
// DrawIdMv (GPU pairing, R per draw) and CameraMv pass B, end to end on a software device, against ground truth.
//
// A synthetic "game" renders a G-buffer pass the way ETS2 does (VS cb0 = a 2 MB dynamic ring bound with D3D11.1 offsets,
// MVP in rows 4..7, reversed-Z infinite projection, depth GREATER, stencil REPLACE 0x0F, scissor on, viewport jitter,
// world [0.01, 0.9] and cabin [0.9, 1.0] depth layers, D32_FLOAT_S8X24_UINT depth with DSV binding only) and writes the
// TRUE owner of every pixel (object id, draw index) into an extra target. The harness drives the injector code exactly
// like inject.cpp: Record per draw, Replay at the G-buffer leave (read-only DSV), forced replay + ring seal on a mid-pass
// WRITE_DISCARD, depth snapshot, then CameraMv::Generate with the record and the draw-id target. Checks per frame:
//   * id ownership: every pixel of a replayed draw carries that draw's id (exact), unreplayed instanced draws carry 0
//   * MV: every pixel's motion vector equals the reprojection through ITS object's true R = MVP_prev * inverse(MVP_cur)
//     (static objects: = the camera R; first frame / new objects / instanced: the camera R) within tolerance
//   * draw states (R table): static objects never "mover", moving objects "mover" every frame after their first
//     (no tag flicker), and no history reset (world miss without hold) after the first frame
// Scenes: S1 street (static copies, 30 identical shuffled + culled clumps, a vehicle with 4 identical spinning wheels and a
// plate on the body matrix, a trailer, an identical second vehicle the other way, alpha-tested foliage in front of the
// vehicle, instanced vegetation, a skinned (VS SRV) mover, cabin dashboard + wiper), S2 = S1 with buffer-pool rotation
// (VB / IB / ring pointers change), S3 = S1 with a two-segment G-buffer pass (instanced vegetation in segment 2 over a
// segment-1 box) and a mid-pass WRITE_DISCARD of the ring (forced replay), S4 slow / stopping vehicle, S5 (v0.10.0 phase 2)
// a mirror view rendered before the main view each frame with its own depth / record / CameraMv (a mirror unit), the own
// trailer static relative to the mirror camera, every 37th mirror frame skipped (see RunMirrorScene), S6 (v0.10.0 phase 3)
// the CONSENSUS camera R and FORWARD-pass draw ids: a convoy of 30 identical trucks + 12 unique cars all moving together
// (the medoid of pass A samples the unique cars and picks the convoy's motion as the camera R) against a larger static
// scene; a vehicle with a forward-pass licence plate (alpha band) and a forward glass pane (alpha < 0.5: no forward depth),
// a static forward wire (no depth write) -- see RunConvoyScene. Extra arguments "nocons" / "nofwd" run S6 with the consensus
// / the forward ids switched off (mutations: S6 must FAIL then). S7 (v0.10.0 phase 4) LICENCE PLATES take their body's motion:
// 12 vehicles sharing one plate mesh (H2), 4 bracket plates 0.9 m behind their body (per-draw origin attach only), a plate
// drawn with the camera VP and world-space dynamic vertices (H1: per-pixel attach only), a static decal -- see RunPlateScene;
// "noattach" runs S7 with mv_fwd_attach_m = 0 (mutation: S7 must FAIL then). v0.10.0 phase 5: S7 also has draws whose KEYS
// CHANGE EVERY FRAME (a new vertex buffer + ring-shifted startIndex / baseVertex: never paired, as the in-game plates) that carry
// their vehicle's matrix -- a G-buffer and a forward plate on the H1 body, 12 identical G-buffer plates on the 12 shared-plate
// vehicles (2 of them culled every 11th frame), a G-buffer and a forward side plate on a truck OV overtaking alongside the camera
// (its pivot BEHIND the camera plane: only the matrix twin can help), a static sign face on its post's matrix (must stay
// static), a G-buffer lamp on its own node 0.37 m from the H1 body's pivot (origin fallback) and a static decoy on the H1 body's path (the H1 body's pivot
// passes within 0.5 m with another orientation: must NOT take the H1 body's motion). "notwin" (mv_drawid_twin = 0) and "noinherit" (twin
// and attach off) must FAIL S7 (since phase 6 they also switch the rigid parents off: they test phase 5 on its own).
// v0.10.0 phase 6: S8 RIGID PARENTS (see RunParentScene): plates whose vertices are written in VIEW space every frame (rows
// 4..7 = the bare projection, keys new every frame -- the in-game plates of 2026-10-08 12:42) and a forward plate on its own
// node matrix, 6.5 m behind the pivot of a TURNING trailer, plate text inside an unpaired plate background (chain), a parked
// trailer's plate (static parent), a static sign next to a passing truck at ~1.5x its depth (depth test). S1-S7 accept a
// rigid-parent inheritance where "unpaired" was expected when its state matches the object's true motion (the per-pixel MV
// check then verifies the inherited R). "noparent" / "parentconj" (the row-vector formula in this column-vector code) /
// "nodepth" must FAIL S8.
// v0.10.0 phase 6b: S8 also has an INSTANCED (DrawIndexedInstanced, 1 instance) view-space plate on the turning trailer -- it
// has no own matrix, so only the rigid-parent vote gives it the trailer's R (its pixels must move with the trailer; "noparent"
// must fail on it) -- and an INSTANCED roadside grass clump away from traffic that must stay static (camera R). DrawIdMv::
// SetInstanced(true) + replayAll are enabled for S8 only, so S1-S7 are unchanged.
// v0.10.0 phase 6c (MARCHING vote): S8 also has a FAR 24x8 px view-space plate on a weaving van, an INSTANCED background with
// INSTANCED text fully inside it, and an INSTANCED plate on the trailer's bottom edge (road below at another depth); scene 8 runs
// twice -- the second time ("S8 short reach") with a 6 px march reach so the plate text needs the vote's 2nd pass.
// v0.10.0 phase 7: S9 THE PRE / POST-TONEMAP STAGE (see RunStageScene): the DlaaStage hysteresis (src/dlaa_stage.h, pure logic),
// the real SceneDlaa with its two colour sets on WARP (tests/fake_dlaa.cpp stands in for NGX: counted features, a marker per
// feature in the output) and a model of inject.cpp's per-pass flow driven with the in-game pass pattern of the phase-6c log.
// v0.10.0 phase 14: "rstatic=E" (E = mv_replay_static_every, 2..16; mv_replay_static_frames 6) runs S1-S4, S6-S8 and S13 with the
// STATIC REPLAY SKIP of inject.cpp DidReplay (DrawIdRecord::SetStaticGate + DrawIdMv::PollMain before the replays, SetFrame per
// frame): a skipped draw must own NO id pixels, its pixels must still get the camera R (MV check), its state must still be right
// (it stays recorded and paired), and a draw that is a MOVER while skipped (a parked vehicle that just started: S13 parks every
// vehicle until frame 40) may keep the camera R for at most kLagMax frames in a row ("lag"); the gate must skip something.
// S13 also runs without rstatic (a plain scene: everything must pass as in S1).
// Round 4: S8 also has four 8-instance INSTANCED grass batches 10 cm in front of the passing truck's side (they must never take its
// motion: the clutter rule / temporal check), and in frame 50 the turning trailer is left out of the replay (SetTestSkipKey): its 7
// plates must keep its motion through the plate hold.
// Round 6: S15 ROADSIDE (see RunRoadsideScene): grass drawn CAMERA-RELATIVE as ATS does (projection * view rotation in cb0, the
// position in the vertices), non-instanced and instanced (2 / 3 / 8 instances), beside an overtaking car, an oncoming car and a
// parked van: every clump pixel must keep the exact camera motion; the instance counts the shaders saw are checked end to end.
// "mutcr" (the origin-free rule off) must FAIL S15; "mutinst" (multi-instance draws listed again, the round-4 rule) fails it too.
// v0.10.0 phase 19: S17 / S17b CAR (see RunCarScene; scene 17, last in the full run): the interior's big draws keep their MVP one
// cb0 row later (S17, the ATS car signature) / a valid interior with one big body draw (S17b), camera-relative overlays, VR head
// motion -- every interior pixel the exact cabin motion. "mutshape" (the MVP-shape rule off) must FAIL S17, "cabsmall=0"
// (mv_cabin_small off) both.
// v0.10.0 phase 22: the MVP LAYOUT detection (DrawIdMv CSGather) reads the real MVP wherever the VS keeps it: S17's interior draws
// now pair with their rows-5..8 MVP (cabin voters, static; the left door opens from frame 50 and must get its own motion), and
// S18 SHOP WINDOWS (see RunShopScene; scene 18, after S17 in the full run): window glass + forward reflection draws with the MVP in
// cb0 rows 6..9 must keep the camera motion while the camera drives and yaws. "mutlayout" (the detection off = rows 4..7, phase
// 21) must FAIL S17 and S18; "mutshape" alone no longer fails S17 (the detection finds the block, the shape fallback is unused).
// Build: tests\build_drawid_harness.bat (cl, WARP; output in build\tests). Run: build\tests\drawid_harness.exe [scene]
// [snap px] [debug | nocons | nofwd | noattach | notwin | noinherit | noparent | parentconj | nodepth]. Exit code 0 = all
// checks passed.
#define WITH_DLAA 1
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <random>
#include <memory>
#include "motion_vectors.h"
#include "draw_ids.h"
#include "shader_cache.h"
#include "scene_dlaa.h"            // v0.10.0 phase 7: S9 (the real SceneDlaa, tests/fake_dlaa.cpp for NGX)
#include "gpu_perf.h"              // v0.10.0 phase 8: per-sub-pass timers (WARP figures)
#include "dlaa_stage.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr int W = 960, H = 540;
bool g_debugLayer = false;
bool g_hw = false;        // v0.10.0 phase 18: "hw" = the hardware adapter (GPU timings in the perf lines; the checks are WARP-tuned)
int  g_vpOver = 0;        // v0.10.0 phase 23 "overscan": the jittered scene viewports are (W+1) x (H+1) like the DLL's jitter_overscan=1
unsigned g_rstatic = 0;     // v0.10.0 phase 14: "rstatic=E" -- S1-S4 / S13 replay with the static gate (0 = off)
double   g_yawJerk = 0.0;   // phase 14 round 5 "jerk=X": S1-S4 / S13 / S14 camera yaw rate + X rad / frame from frame 60
constexpr int kLagMax = 2;  // most frames in a row a skipped MOVER may keep the camera R (readback latency + the stagger)
// v0.10.0 phase 8: GPU perf timers on WARP (gpu_perf.h) -- every scene frame is one "pass" of eye 0; the per-scene line is a WARP
// figure (software rasteriser on the CPU), NOT a GPU number: it only shows that every section is timed and which ones dominate
uint64_t g_perfTag = 0;
void PerfFrame() {
    ++g_perfTag;
    GpuPerf::SetFrame(g_perfTag);
    GpuPerf::SetPass(g_perfTag);
    GpuPerf::NoteEye(g_perfTag, 0);
}
// the phase-8 counters of a scene's DrawIdMv + the WARP perf line (restarts the perf window)
void PrintPhase8(ID3D11DeviceContext* ctx, const DrawIdMv::Stats& ds, const char* tag) {
    printf("   %s phase 8: frames without a world / cabin medoid %llu / %llu (medoid re-armed %llu / %llu); rigid-parent vote: listed "
           "draws with pixels %llu, of them fine %llu, frames whose vote grid was skipped %llu, 1st-pass marches %llu (%.0f / frame)\n",
           tag, (unsigned long long)ds.noMedoidFrames[0], (unsigned long long)ds.noMedoidFrames[1],
           (unsigned long long)ds.medoidRearms[0], (unsigned long long)ds.medoidRearms[1], (unsigned long long)ds.parVis,
           (unsigned long long)ds.parFine, (unsigned long long)ds.parSkipped, (unsigned long long)ds.parMarches,
           ds.rbFrames ? (double)ds.parMarches / (double)ds.rbFrames : 0.0);
    if (ds.cvFrames)                                     // v0.10.0 phase 18
        printf("   %s compact vote: %llu passes, tiles %.1f of %.1f vote groups per pass, march items %.1f / %.1f per pass (1st / "
               "2nd), items over the cap %llu, tile overflows %llu\n", tag, (unsigned long long)ds.cvFrames,
               (double)ds.cvTiles / (double)ds.cvFrames, (double)ds.cvGrid / (double)ds.cvFrames,
               (double)ds.cvItems1 / (double)ds.cvFrames, (double)ds.cvItems2 / (double)ds.cvFrames,
               (unsigned long long)ds.cvItemOver, (unsigned long long)ds.cvTileOver);
    if (DrawIdMv::InstReplay())
        printf("   %s mv_inst_replay: instanced draws with a moving parent %llu (read back), instanced keys wanted %d\n", tag,
               (unsigned long long)ds.instMoverDraws, DrawIdMv::InstancedKnown());
    for (int k = 0; k < 8; ++k) { ctx->Flush(); GpuPerf::Poll(ctx); Sleep(2); }
    GpuPerf::EyeStats st;
    if (GpuPerf::Stats(0, &st)) {
        char buf[2048];
        int o = snprintf(buf, sizeof(buf), g_hw ? "   %s HW perf (hardware adapter, GPU ms): mod %.3f ms/frame avg (%llu frames) |"
                                                : "   %s WARP perf (CPU-rasterised, not GPU numbers): mod %.3f ms/frame avg (%llu frames) |",
                         tag, st.total, (unsigned long long)st.passes);
        for (int c = 0; c < GpuPerf::kCount && o < (int)sizeof(buf) - 48; ++c)
            if (st.n[c]) o += snprintf(buf + o, sizeof(buf) - (size_t)o, " %s %.3f", GpuPerf::Name(c), st.avg[c]);
        printf("%s\n", buf);
    }
    GpuPerf::Restart();
}
// v0.10.0 phase 8 COMPARISON MODE: "cmpdump=FILE" writes every scene frame's MV texture (R16G16_FLOAT rows) to FILE, "cmp=FILE"
// reads the same frames back and compares per pixel -- run the reference with "legacy" (the phase-7 behaviour) and the candidate
// with the new defaults: every optimisation off / on must give the same MVs within tolerance (counted, printed at the end)
FILE*    g_cmpOut = nullptr;
FILE*    g_cmpIn = nullptr;
uint64_t g_cmpFrames = 0, g_cmpPx = 0, g_cmpOver = 0, g_cmpOverFrames = 0, g_cmpShort = 0;
double   g_cmpMax = 0.0;
constexpr double kCmpTolPx = 0.05;
float HalfF(uint16_t h) {
    const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 31u, m = h & 1023u;
    float v = e == 0 ? (float)m * (1.0f / 16777216.0f) : (e == 31 ? 65504.0f : ldexpf(1.0f + (float)m / 1024.0f, (int)e - 15));
    return s ? -v : v;
}
void CmpFrame(const D3D11_MAPPED_SUBRESOURCE& mp, int w, int h) {
    if (!g_cmpOut && !g_cmpIn) return;
    std::vector<uint32_t> row((size_t)w), ref((size_t)w);
    uint64_t over = 0;
    for (int y = 0; y < h; ++y) {
        memcpy(row.data(), (const uint8_t*)mp.pData + (size_t)y * mp.RowPitch, (size_t)w * 4);
        if (g_cmpOut) fwrite(row.data(), 4, (size_t)w, g_cmpOut);
        if (g_cmpIn) {
            if (fread(ref.data(), 4, (size_t)w, g_cmpIn) != (size_t)w) { ++g_cmpShort; return; }
            for (int x = 0; x < w; ++x) {
                if (row[x] == ref[x]) { ++g_cmpPx; continue; }
                const double dx = HalfF((uint16_t)(row[x] & 0xFFFF)) - HalfF((uint16_t)(ref[x] & 0xFFFF));
                const double dy = HalfF((uint16_t)(row[x] >> 16)) - HalfF((uint16_t)(ref[x] >> 16));
                const double d = sqrt(dx * dx + dy * dy);
                if (d > g_cmpMax) g_cmpMax = d;
                if (d > kCmpTolPx) ++over;
                ++g_cmpPx;
            }
        }
    }
    ++g_cmpFrames;
    g_cmpOver += over;
    if (over) ++g_cmpOverFrames;
}
ComPtr<ID3D11InfoQueue> g_iq;
// prints the debug layer's stored messages (errors / warnings / corruption); returns how many there were
int DrainDebugLayer(const char* where) {
    if (!g_iq) return 0;
    const UINT64 n = g_iq->GetNumStoredMessages();
    int bad = 0;
    static int printed = 0;
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(g_iq->GetMessage(i, nullptr, &len)) || !len) continue;
        std::vector<char> buf(len);
        D3D11_MESSAGE* m = (D3D11_MESSAGE*)buf.data();
        if (FAILED(g_iq->GetMessage(i, m, &len))) continue;
        if (m->Severity > D3D11_MESSAGE_SEVERITY_WARNING) continue;    // INFO / MESSAGE: ignored
        ++bad;
        if (printed++ < 20) printf("   DEBUG LAYER (%s) sev %d id %d: %.*s\n", where, (int)m->Severity, (int)m->ID,
                                   (int)m->DescriptionByteLength, m->pDescription);
    }
    g_iq->ClearStoredMessages();
    return bad;
}

// ---- double matrices (column vectors: clip = M * p; rows of M = the MVP rows the game stores) ----------------------
struct M4 { double m[4][4]; };
M4 Mul(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { double s = 0; for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j]; r.m[i][j] = s; }
    return r;
}
M4 I4() { M4 r{}; for (int i = 0; i < 4; ++i) r.m[i][i] = 1; return r; }
M4 T(double x, double y, double z) { M4 r = I4(); r.m[0][3] = x; r.m[1][3] = y; r.m[2][3] = z; return r; }
M4 S(double x, double y, double z) { M4 r = I4(); r.m[0][0] = x; r.m[1][1] = y; r.m[2][2] = z; return r; }
M4 RotY(double a) { M4 r = I4(); r.m[0][0] = cos(a); r.m[0][2] = sin(a); r.m[2][0] = -sin(a); r.m[2][2] = cos(a); return r; }
M4 RotX(double a) { M4 r = I4(); r.m[1][1] = cos(a); r.m[1][2] = -sin(a); r.m[2][1] = sin(a); r.m[2][2] = cos(a); return r; }
M4 RotZ(double a) { M4 r = I4(); r.m[0][0] = cos(a); r.m[0][1] = -sin(a); r.m[1][0] = sin(a); r.m[1][1] = cos(a); return r; }
bool Inv(const M4& a, M4& out) {
    double m[16], inv[16];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) m[i * 4 + j] = a.m[i][j];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(fabs(det) > 1e-300)) return false;
    for (int i = 0; i < 16; ++i) out.m[i / 4][i % 4] = inv[i] / det;
    return true;
}
void Xf(const M4& a, const double v[4], double o[4]) { for (int i = 0; i < 4; ++i) o[i] = a.m[i][0] * v[0] + a.m[i][1] * v[1] + a.m[i][2] * v[2] + a.m[i][3] * v[3]; }

// reversed-Z infinite projection (as the game): clip = (fx x, fy y, near, z)
const double kAspect = (double)W / H, kF = 1.2;
M4 Proj(double nearM) { M4 r{}; r.m[0][0] = kF / kAspect; r.m[1][1] = kF; r.m[2][3] = nearM; r.m[3][2] = 1; return r; }
const double kNearWorld = 0.1, kNearCabin = 0.05;

// ---- geometry ------------------------------------------------------------------------------------------------------
struct Vtx { float p[3]; float uv[2]; float bone; };
struct Mesh { UINT si = 0, ic = 0; INT bv = 0; };
struct Geo {
    std::vector<Vtx> v;
    std::vector<uint32_t> i;
    Mesh Begin() { Mesh m; m.si = (UINT)i.size(); m.bv = (INT)v.size(); return m; }
    void End(Mesh& m) { m.ic = (UINT)i.size() - m.si; }
    // triangle with front = clockwise seen from outside in the D3D LH convention (see the comment in Box)
    void Tri(int base, int a, int b, int c, const double outward[3]) {
        const float* pa = v[base + a].p; const float* pb = v[base + b].p; const float* pc = v[base + c].p;
        const double e1[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] }, e2[3] = { pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] };
        const double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        const double d = n[0] * outward[0] + n[1] * outward[1] + n[2] * outward[2];
        // D3D (LH, y up, viewport y flip), FrontCounterClockwise FALSE: cross(p1 - p0, p2 - p0) points OUTWARD for a front face
        if (d >= 0) { i.push_back(a); i.push_back(b); i.push_back(c); }
        else { i.push_back(a); i.push_back(c); i.push_back(b); }
    }
    // box centred at (cx, cy, cz) with half sizes; bone = vertex attribute (skinned VS)
    Mesh Box(float hx, float hy, float hz, float cx = 0, float cy = 0, float cz = 0, float bone = 0) {
        Mesh m = Begin();
        const int base = (int)v.size();
        static const int faces[6][3] = { {0, 1, 2}, {0, 1, 2}, {1, 2, 0}, {1, 2, 0}, {2, 0, 1}, {2, 0, 1} };
        for (int f = 0; f < 6; ++f) {
            const int ax = faces[f][2], u = faces[f][0], w = faces[f][1];
            const float s = (f & 1) ? -1.0f : 1.0f;
            const float h[3] = { hx, hy, hz }, c[3] = { cx, cy, cz };
            const int fb = (int)v.size() - base;
            for (int k = 0; k < 4; ++k) {
                Vtx x{};
                float q[3];
                q[ax] = s * h[ax];
                q[u] = ((k == 1 || k == 2) ? 1.0f : -1.0f) * h[u];
                q[w] = ((k >= 2) ? 1.0f : -1.0f) * h[w];
                for (int a = 0; a < 3; ++a) x.p[a] = c[a] + q[a];
                x.uv[0] = (k == 1 || k == 2) ? 1.0f : 0.0f; x.uv[1] = (k >= 2) ? 1.0f : 0.0f;
                x.bone = bone;
                v.push_back(x);
            }
            double outward[3] = { 0, 0, 0 }; outward[ax] = s;
            // quad corners k: 0 (-u,-w), 1 (+u,-w), 2 (+u,+w), 3 (-u,+w)
            Tri(base, fb + 0, fb + 1, fb + 2, outward);
            Tri(base, fb + 0, fb + 2, fb + 3, outward);
        }
        // indices were pushed relative to base (base vertex = m.bv)
        End(m);
        return m;
    }
    // v0.10.0 phase 19 (S17): n boxes in ONE mesh (36 n indices: a big draw), box k centred at (cx, cy, cz) + k (dx, dy, dz)
    Mesh Boxes(int n, float hx, float hy, float hz, float dx, float dy, float dz, float cx, float cy, float cz) {
        Mesh m = Begin();
        for (int b = 0; b < n; ++b) {
            const Mesh one = Box(hx, hy, hz, cx + dx * b, cy + dy * b, cz + dz * b);
            for (UINT q = one.si; q < one.si + one.ic; ++q) i[q] += (uint32_t)(one.bv - m.bv);   // rebase onto this mesh
        }
        End(m);
        return m;
    }
    // v0.10.0 phase 4: vertical quad in the xy plane at (0, cy, cz) (a plate in its body's frame), uv 0..1
    Mesh QuadZ(float hx, float hy, float cy, float cz) {
        Mesh m = Begin();
        const float pts[4][2] = { {-hx, cy - hy}, {hx, cy - hy}, {hx, cy + hy}, {-hx, cy + hy} };
        for (int k = 0; k < 4; ++k) { Vtx x{}; x.p[0] = pts[k][0]; x.p[1] = pts[k][1]; x.p[2] = cz; x.uv[0] = (k == 1 || k == 2) ? 1.f : 0.f; x.uv[1] = k >= 2 ? 0.f : 1.f; v.push_back(x); }
        const uint32_t idx[6] = { 0, 1, 2, 0, 2, 3 };
        for (uint32_t q : idx) i.push_back(q);
        End(m);
        return m;
    }
    // vertical quad in the xy plane (double-sided use: CULL_NONE), uv 0..1
    Mesh Quad(float hx, float hy, float cy = 0) {
        Mesh m = Begin();
        const float pts[4][2] = { {-hx, cy - hy}, {hx, cy - hy}, {hx, cy + hy}, {-hx, cy + hy} };
        for (int k = 0; k < 4; ++k) { Vtx x{}; x.p[0] = pts[k][0]; x.p[1] = pts[k][1]; x.p[2] = 0; x.uv[0] = (k == 1 || k == 2) ? 1.f : 0.f; x.uv[1] = k >= 2 ? 0.f : 1.f; v.push_back(x); }
        const uint32_t idx[6] = { 0, 1, 2, 0, 2, 3 };
        for (uint32_t q : idx) i.push_back(q);
        End(m);
        return m;
    }
    // horizontal ground quad (y = 0)
    Mesh Ground(float h) {
        Mesh m = Begin();
        const float pts[4][2] = { {-h, -h}, {h, -h}, {h, h}, {-h, h} };
        for (int k = 0; k < 4; ++k) { Vtx x{}; x.p[0] = pts[k][0]; x.p[1] = 0; x.p[2] = pts[k][1]; x.uv[0] = pts[k][0]; x.uv[1] = pts[k][1]; v.push_back(x); }
        const uint32_t idx[6] = { 0, 1, 2, 0, 2, 3 };
        for (uint32_t q : idx) i.push_back(q);
        End(m);
        return m;
    }
    // grass clump: two crossed vertical quads
    Mesh Clump(float hx, float hy) {
        Mesh m = Begin();
        for (int q = 0; q < 2; ++q) {
            const int b = (int)v.size() - m.bv;
            for (int k = 0; k < 4; ++k) {
                Vtx x{};
                const float s = (k == 1 || k == 2) ? hx : -hx, y = k >= 2 ? 2 * hy : 0.0f;
                x.p[0] = q ? 0 : s; x.p[2] = q ? s : 0; x.p[1] = y;
                x.uv[0] = (k == 1 || k == 2) ? 1.f : 0.f; x.uv[1] = k >= 2 ? 0.f : 1.f;
                v.push_back(x);
            }
            const uint32_t idx[6] = { 0, 1, 2, 0, 2, 3 };
            for (uint32_t t : idx) i.push_back(b + t);
        }
        End(m);
        return m;
    }
};

// ---- shaders (test only: compiled here with D3DCompile) ----------------------------------------------------------------
const char kGameShaders[] = R"(
cbuffer CB0 : register(b0) { float4 Rows[16]; };   // rows 4..7 = MVP (instanced: VP), row 8 = (object id + 1, draw + 1, alpha test, 0)
StructuredBuffer<float4> Bones : register(t3);       // skinned VS only
struct VIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; float bone : TEXCOORD1; };
struct VInI { float3 pos : POSITION; float2 uv : TEXCOORD0; float bone : TEXCOORD1; float3 off : TEXCOORD4; };
struct VOut { float4 col : COLOR0; float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 Clip(float3 p) { float4 q = float4(p, 1.0); return float4(dot(Rows[4], q), dot(Rows[5], q), dot(Rows[6], q), dot(Rows[7], q)); }
VOut VSMain(VIn i) { VOut o; o.pos = Clip(i.pos); o.uv = i.uv; o.col = float4(i.uv, 0, 1); return o; }
VOut VSSkin(VIn i) { VOut o; o.pos = Clip(i.pos + Bones[(uint)i.bone].xyz); o.uv = i.uv; o.col = float4(i.uv, 1, 1); return o; }
VOut VSInst(VInI i) { VOut o; o.pos = Clip(i.pos + i.off); o.uv = i.uv; o.col = float4(i.uv, 0.5, 1); return o; }
// v0.10.0 phase 19 (S17): the ATS car interior's layout -- row 4 = (0, 0, 0, 1), the MVP in rows 5..8, the truth in row 9 -- so
// the injector's rows 4..7 read is NOT the MVP (one row early)
VOut VSShift(VIn i) {
    VOut o; float4 q = float4(i.pos, 1.0);
    o.pos = float4(dot(Rows[5], q), dot(Rows[6], q), dot(Rows[7], q), dot(Rows[8], q)); o.uv = i.uv; o.col = float4(i.uv, 0.25, 1);
    return o;
}
struct POut { uint2 truth : SV_Target0; float4 col : SV_Target1; };
POut PSMain(VOut i) {
    if (Rows[8].z > 0.5) {                 // alpha test: holes in a checker pattern
        int2 c = (int2)floor(i.uv * 6.0);
        if (((c.x + c.y) & 1) != 0) discard;
    }
    POut o; o.truth = uint2((uint)Rows[8].x, (uint)Rows[8].y); o.col = i.col; return o;
}
POut PSMain9(VOut i) { POut o; o.truth = uint2((uint)Rows[9].x, (uint)Rows[9].y); o.col = i.col; return o; }   // phase 19 (S17)
// v0.10.0 phase 22 (S18): the ATS shop-window layout (captures/shop_window_audit.md) -- r0 = object position + seed, r1 = parameters,
// r2..r5 = model-view (r5 = (0, 0, 0, 1)), the MVP in rows 6..9, the truth in row 10. Three vertex shaders as in game (G-buffer glass
// family A / family B, the forward reflection draws): three VS pointers in the per-VS layout table.
float4 Clip6(float3 p) { float4 q = float4(p, 1.0); return float4(dot(Rows[6], q), dot(Rows[7], q), dot(Rows[8], q), dot(Rows[9], q)); }
VOut VSShift2(VIn i) { VOut o; o.pos = Clip6(i.pos); o.uv = i.uv; o.col = float4(i.uv, 0.75, 1); return o; }
VOut VSShift2B(VIn i) { VOut o; o.pos = Clip6(i.pos); o.uv = i.uv; o.col = float4(0.2, i.uv, 1); return o; }
VOut VSShift2F(VIn i) { VOut o; o.pos = Clip6(i.pos); o.uv = i.uv; o.col = float4(i.uv.yx, 0.9, 1); return o; }
POut PSMain10(VOut i) { POut o; o.truth = uint2((uint)Rows[10].x, (uint)Rows[10].y); o.col = i.col; return o; }
float4 PSFwd10(VOut i) : SV_Target0 { return float4(i.col.rgb, 1.0); }   // opaque: the forward re-draw writes its whole depth
// v0.10.0 phase 3 (S6) forward pass: Rows[8].z = 0 opaque, 1 = a band with alpha 0.25 (plate), 2 = alpha 0.3 everywhere (glass)
float FwdAlpha(VOut i) {
    if (Rows[8].z > 1.5) return 0.3;
    if (Rows[8].z > 0.5 && i.uv.x > 0.4 && i.uv.x < 0.6) return 0.25;
    return 1.0;
}
float4 PSFwd(VOut i) : SV_Target0 { return float4(i.col.rgb, FwdAlpha(i)); }
// truth of the forward depth's owner: drawn with the SAME alpha-to-coverage state as the re-draw (RT0 alpha decides the
// coverage of every target; WARP's single-sample alpha-to-coverage DITHERS -- alpha 0.3 covers some pixels -- so a fixed
// "alpha >= 0.5" model would be wrong), RT1 = (object + 1, forward index + 1)
struct PFT { float4 col : SV_Target0; uint2 truth : SV_Target1; };
PFT PSFwdTruth(VOut i) {
    PFT o;
    o.col = float4(0.0, 0.0, 0.0, FwdAlpha(i));
    o.truth = uint2((uint)Rows[8].x, (uint)Rows[8].y);
    return o;
}
)";

ComPtr<ID3DBlob> Compile(const char* entry, const char* profile) {
    ComPtr<ID3DBlob> code, err;
    if (FAILED(D3DCompile(kGameShaders, sizeof(kGameShaders) - 1, "game", nullptr, nullptr, entry, profile,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
        printf("game shader %s failed: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "?");
        return nullptr;
    }
    return code;
}

// ---- objects ----------------------------------------------------------------------------------------------------------
enum Kind { kStatic, kMover, kCabinStatic, kCabinMover, kInstanced };
struct Obj {
    const char* name;
    Kind kind;
    int mesh;            // index into the mesh table
    int vs;              // 0 main, 1 skinned, 2 instanced
    bool cullNone;
    bool alpha;
    int instances;       // instanced draws
    int group;           // statistics group (vehicle parts, clumps, ...)
};

struct Env {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11DeviceContext1> ctx1;
    ComPtr<ID3D11Buffer> vb[2], ib[2], instVb[2];         // pool copies (rotation)
    ComPtr<ID3D11Buffer> ring[2];                         // 2 MB dynamic cbuffers, alternating per frame
    ComPtr<ID3D11Buffer> bones;
    ComPtr<ID3D11ShaderResourceView> bonesSrv;
    ComPtr<ID3D11VertexShader> vsMain, vsSkin, vsInst;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11VertexShader> vsShift;                   // v0.10.0 phase 19 (S17): the MVP in cb0 rows 5..8
    ComPtr<ID3D11PixelShader> ps9;                        //   + the truth in row 9
    ComPtr<ID3D11VertexShader> vsShift2, vsShift2B, vsShift2F;   // v0.10.0 phase 22 (S18): the MVP in cb0 rows 6..9
    ComPtr<ID3D11PixelShader> ps10, psFwd10;                     //   + the truth in row 10
    ComPtr<ID3D11InputLayout> il, ilInst;
    ComPtr<ID3D11RasterizerState> rsBack, rsNone;
    ComPtr<ID3D11DepthStencilState> dss;
    ComPtr<ID3D11Texture2D> depth, truth, color, twin, dOut, mv, idTex, scratchRt;
    ComPtr<ID3D11DepthStencilView> dsv, roDsv;
    ComPtr<ID3D11RenderTargetView> truthRtv, colorRtv, idRtv, scratchRtv;
    ComPtr<ID3D11ShaderResourceView> twinSrv, idSrv;
    ComPtr<ID3D11UnorderedAccessView> dUav, mvUav;
    ComPtr<ID3D11Texture2D> stMv, stDepth, stTruth, stId;
    ComPtr<ID3D11Buffer> stR;
    std::vector<Mesh> meshes;
};

bool MakeTex(ID3D11Device* dev, UINT w, UINT h, DXGI_FORMAT f, UINT bind, ID3D11Texture2D** out, bool staging = false) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = f; td.SampleDesc.Count = 1;
    td.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    td.BindFlags = staging ? 0 : bind;
    td.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;
    return SUCCEEDED(dev->CreateTexture2D(&td, nullptr, out));
}

// fp16 decode
float H2F(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}

// v0.10.0 phase 6: a draw the pairing left unpaired may take its RIGID PARENT's R and state by the pixel-neighbour vote (R table
// .w <= -(1 + 2 * kMax)). Then the expectation is the object's TRUE motion (the true probe deviation tdev against the camera R:
// a mover, static, or either inside the snap band) instead of "unpaired"; the per-pixel MV check verifies the inherited R.
bool ParentMarked(float w) { return w < -(2.0f * (float)DrawIdMv::kMax) - 0.5f; }
void ParentWant(double tdev, float snapPx, int* want, int* alt) {
    if (tdev >= snapPx * 1.5 + 0.02) { *want = DrawIdMv::kStMover; *alt = -1; }
    else if (tdev <= snapPx * 0.5) { *want = DrawIdMv::kStStatic; *alt = -1; }
    else { *want = DrawIdMv::kStStatic; *alt = DrawIdMv::kStMover; }
}

// scene parameters
struct SceneCfg {
    const char* name;
    int frames;
    bool rotatePool;     // S2: VB / IB pool copy switches every 7th frame (pointers change), ring pool alternates anyway
    bool segments;       // S3: G-buffer left + re-entered mid-pass; instanced vegetation in segment 2 over a box
    bool midDiscard;     // S3: the ring is WRITE_DISCARDed mid-pass (forced replay + ring seal)
    bool slowVehicle;    // S4: the vehicle crawls and stops
    bool startVehicle = false;   // S13 (v0.10.0 phase 14): every vehicle / the skinned mover parked until frame 40, then driving
    // S14 (phase 14 round 3): every other vehicle parked; vehicle A (+ its plate) 90 m out, EXACTLY static until frame kMatchShallow,
    // then its own motion drifts 0.04 px / frame off the camera's (inside the 0.1 px snap: "static", but not DEEP static -- a truck
    // at the player's speed), from frame kMatchAccel on it accelerates by 0.05 px / frame per frame (a mover)
    bool matchVehicle = false;
};
constexpr int kMatchShallow = 30, kMatchAccel = 90;

struct Totals { int checks = 0, fails = 0; };

// ---- the scene ----------------------------------------------------------------------------------------------------------
// mesh table indices
enum { MGround, MBox0, MBoxEnd = MBox0 + 10, MClump = MBoxEnd, MBody, MWheel, MPlate, MTrailer, MFoliage, MInst, MSkin,
       MDash, MWiper, MCover, MCab0, MCab1, MCab2,
       MPost, MWire, MPlateF, MGlass, MTruck, MCar0, MCarEnd = MCar0 + 12,   // v0.10.0 phase 3 (S6)
       MPCar0 = MCarEnd, MPCarB0 = MPCar0 + 12, MPlateS = MPCarB0 + 4, MPlateB, // v0.10.0 phase 4 (S7)
       MPlateGS, MSignPost, MOV,                                                // v0.10.0 phase 5 (S7)
       MTrl6, MTrl6P,                                                           // v0.10.0 phase 6 (S8)
       MFarVan,                                                                 // v0.10.0 phase 6c (S8)
       MClumpCR0, MClumpCREnd = MClumpCR0 + 16,                                 // v0.10.0 phase 14 round 6 (S15)
       MS17Hood, MS17Dash, MS17Console, MS17DoorL, MS17DoorR, MS17Real0, MS17Real1,   // v0.10.0 phase 19 (S17)
       MS17Fresh, MS17Fresh2, MS17Ovl0, MS17OvlEnd = MS17Ovl0 + 3, MS17Body,
       MS18Facade, MS18WinA, MS18WinB,                                          // v0.10.0 phase 22 (S18)
       MCount };

struct DrawSpec {
    int obj;             // object index
    M4  mvp;             // the matrix the draw's cb0 rows 4..7 carry (instanced: VP)
    int segment;         // 0 / 1 (S3)
};

int RunScene(Env& E, const SceneCfg& sc, float snapPx, Totals& tot) {
    ID3D11DeviceContext* ctx = E.ctx.Get();
    ID3D11DeviceContext1* ctx1 = E.ctx1.Get();
    printf("\n===== %s (%d frames, snap %.3f px) =====\n", sc.name, sc.frames, (double)snapPx);
    DrawIdMv::SetParams(8.0f, snapPx);
    CameraMv cam;
    if (!cam.Init(E.dev.Get())) { printf("CameraMv init failed\n"); return 1; }
    cam.SetEgoPixel(0.0f);
    cam.SetEgoOrigin(0.0f);
    CandidateRecord cand;
    cand.Init(E.dev.Get());
    DrawIdRecord rec;

    // ---- objects ----
    std::vector<Obj> objs;
    auto add = [&](const char* n, Kind k, int mesh, int vs, bool cn, bool alpha, int inst, int group) {
        objs.push_back({ n, k, mesh, vs, cn, alpha, inst, group }); return (int)objs.size() - 1; };
    const int oGround = add("ground", kStatic, MGround, 0, true, false, 0, 0);
    int oBox[10];
    for (int k = 0; k < 10; ++k) oBox[k] = add("static box", kStatic, MBox0 + k, 0, false, false, 0, 1);
    int oClump[30];
    for (int k = 0; k < 30; ++k) oClump[k] = add("grass clump (30 identical)", kStatic, MClump, 0, true, false, 0, 2);
    const int oBodyA = add("vehicle A body", kMover, MBody, 0, false, false, 0, 3);
    int oWheelA[4];
    for (int k = 0; k < 4; ++k) oWheelA[k] = add("vehicle A wheel (4 identical, spinning)", kMover, MWheel, 0, false, false, 0, 4);
    const int oPlateA = add("vehicle A plate (body matrix)", kMover, MPlate, 0, true, false, 0, 5);
    const int oTrailer = add("trailer", kMover, MTrailer, 0, false, false, 0, 6);
    const int oBodyB = add("vehicle B body (same model)", kMover, MBody, 0, false, false, 0, 7);
    int oWheelB[4];
    for (int k = 0; k < 4; ++k) oWheelB[k] = add("vehicle B wheel", kMover, MWheel, 0, false, false, 0, 7);
    const int oFoliage = add("alpha-tested foliage (static)", kStatic, MFoliage, 0, true, true, 0, 8);
    const int oInst0 = add("instanced vegetation", kInstanced, MInst, 2, true, false, 12, 9);
    const int oInst1 = add("instanced vegetation", kInstanced, MInst, 2, true, false, 8, 9);
    const int oSkin = add("skinned mover (VS SRV bones)", kMover, MSkin, 1, false, false, 0, 10);
    const int oDash = add("cabin dashboard", kCabinStatic, MDash, 0, false, false, 0, 11);
    const int oWiper = add("cabin wiper (moving)", kCabinMover, MWiper, 0, true, false, 0, 12);
    int oPillar[3];                                     // more static cabin draws (the camera's cabin R is their medoid)
    for (int k = 0; k < 3; ++k) oPillar[k] = add("cabin static part", kCabinStatic, MDash, 0, false, false, 0, 11);
    int oCab[3];                                        // cabin parts with their own meshes
    for (int k = 0; k < 3; ++k) oCab[k] = add("cabin static part (unique mesh)", kCabinStatic, MCab0 + k, 0, false, false, 0, 11);
    const int oCoverInst = sc.segments ? add("segment-2 instanced cover", kInstanced, MInst, 2, true, false, 6, 13) : -1;
    const int nObj = (int)objs.size();
    (void)oGround;

    std::mt19937 rng(777);
    std::vector<M4> mvpPrev(nObj), mvpCur(nObj);
    std::vector<bool> drawnPrev(nObj, false), drawnCur(nObj, false);
    std::vector<int> stPrev(nObj, -1), firstFrame(nObj, -1), wantPrev(nObj, -1), wantAltPrev(nObj, -1), idBad(nObj, 0);
    std::vector<int> drawnRun(nObj, 0);                 // consecutive frames drawn, ending at this frame
    std::vector<long long> covTot(nObj, 0);
    long long throughFoliage = 0;                       // vehicle-A pixels with a foliage pixel in their 3x3 neighbourhood
    // objects that share their mesh (= geometry key) with another object: their pairing may be ambiguous
    auto dupGroup = [&](int o) { int c = 0; for (int q = 0; q < (int)objs.size(); ++q) if (objs[q].mesh == objs[o].mesh) ++c; return c > 1; };
    std::vector<int> flick(nObj, 0), wrongState(nObj, 0), objPxFail(nObj, 0), objFramesBad(nObj, 0);
    int resets = 0, idMismatch = 0, idChecked = 0, mvFails = 0, mvChecked = 0, camBad = 0, debugMsgs = 0;
    long long recTicks = 0, recN = 0, repTicks = 0, repN = 0;  // CPU cost of Record / the final Replay (QPC)
    double worstMv[16] = {};
    uint64_t pairedSum = 0, eligSum = 0;
    int checks = 0, fails = 0;
    // v0.10.0 phase 14 (rstatic=E): draws the static gate skipped / replayed, per object: frames a skipped draw was a mover (lag),
    // the longest run of such frames, pixels
    long long statSkipTot = 0, statRepTot = 0, lagPxTot = 0;
    std::vector<int> lagRun(nObj, 0), lagMaxRun(nObj, 0), lagFrames(nObj, 0);
    // phase 14 round 3 (S14): vehicle A's lateral drift (m) and its body / plate draws skipped while exactly static / after it began
    // to drift (+ kLagMax frames for the readback): the latter must stay 0
    double matchX = 0.0;
    int matchSkipDeep = 0, matchSkipLate = 0;
    if (g_rstatic) DrawIdMv::StaticClear(DrawIdMv::kStatClrConfig);   // no streaks from the previous scene

    GpuPerf::Restart();
    for (int f = 0; f < sc.frames; ++f) {
        PerfFrame();
        // v0.10.0 phase 14: inject.cpp OnPresentBoundary (SetFrame) + DidReplay (PollMain, the gate around every replay of the pass)
        if (g_rstatic) { DrawIdMv::SetFrame((uint64_t)f + 1); DrawIdMv::PollMain(ctx); DrawIdRecord::SetStaticGate(true); }
        // ---- camera: driving forward 0.5 m / frame with a gentle yaw ----
        const M4 camPose = Mul(T(0.0, 1.6, 0.5 * f), RotY(0.003 * f + (f >= 60 ? g_yawJerk * (f - 59) : 0.0)));
        M4 V; Inv(camPose, V);
        const M4 Pw = Proj(kNearWorld), Pc = Proj(kNearCabin);
        const M4 VPw = Mul(Pw, V);
        // ---- world matrices ----
        std::vector<M4> world(nObj, I4());
        world[oGround] = T(0, 0, 100);
        for (int k = 0; k < 10; ++k) world[oBox[k]] = T((k & 1) ? 7.0 : -7.0, 0.0, 30.0 + 15.0 * k);
        for (int k = 0; k < 30; ++k) world[oClump[k]] = T(-4.0 + 1.6 * (k % 6), 0.0, 22.0 + 3.0 * (k / 6));
        // vehicle A: drives along +z at 0.8 m / frame from z = 25 (S4: crawls 0.05 m / frame, stops at frame 60)
        // v0.10.0 phase 14 S13: fm = the "driving" frame count -- 0 until frame 40 (parked, 20 m ahead of the camera then), so the
        // static replay gate skips the parked vehicles' draws and must pick them up when they start
        const int fm = sc.startVehicle ? std::max(0, f - 40) : (sc.matchVehicle ? 0 : f);
        // phase 14 round 3 S14: vehicle A's own screen motion relative to a static world point at its place -- 0, then 0.04 px /
        // frame (static by the 0.1 px snap, not deep), then + 0.05 px / frame per frame: a lateral drift whose per-frame step u
        // gives that many px at A's view distance (ndc x = P[0][0] x / depth; px = ndc * W / 2)
        if (sc.matchVehicle && f >= kMatchShallow) {
            const double targetPx = 0.04 + (f >= kMatchAccel ? 0.05 * (double)(f - kMatchAccel + 1) : 0.0);
            const double depthA = 90.0 - 0.5 * f;
            matchX += targetPx / (Proj(kNearWorld).m[0][0] * (double)W * 0.5 / depthA);
        }
        // distance driven after fm frames at speed v (m / frame): S13 accelerates at 0.03 m / frame^2 from rest (an instant jump to
        // full speed breaks the pairing's track continuity of the duplicate-mesh vehicles -- with or without the gate); else v * fm
        auto drive = [&](double v) {
            if (!sc.startVehicle) return v * fm;
            const double a = 0.03, tf = v / a;
            return fm < tf ? 0.5 * a * fm * fm : 0.5 * a * tf * tf + v * (fm - tf);
        };
        double zA = sc.slowVehicle ? 40.0 + 0.05 * std::min(f, 60) : (sc.startVehicle ? 40.0 + drive(0.62) : 12.0 + 0.62 * f);
        if (sc.matchVehicle) zA = 90.0;
        const double xA = 2.5 + matchX;
        const double spinA = sc.slowVehicle ? 0.05 / 0.45 * std::min(f, 60) : (sc.startVehicle ? drive(0.62) / 0.45 :
                                                                              (sc.matchVehicle ? 0.0 : 0.62 / 0.45 * f));
        const M4 bodyA = Mul(T(xA, 0.0, zA), RotY(0.02 * sin(0.05 * fm)));
        world[oBodyA] = bodyA;
        const double wheelPos[4][3] = { {-1.0, 0.45, 1.6}, {1.0, 0.45, 1.6}, {-1.0, 0.45, -1.6}, {1.0, 0.45, -1.6} };
        for (int k = 0; k < 4; ++k)
            world[oWheelA[k]] = Mul(bodyA, Mul(T(wheelPos[k][0], wheelPos[k][1], wheelPos[k][2]), RotX(spinA)));
        world[oPlateA] = Mul(bodyA, T(0.0, 0.6, -2.31));                    // rear plate on the body matrix
        // vehicle B: same model, coming the other way at 1.0 m / frame
        const double zB = 140.0 - drive(1.0);
        const M4 bodyB = Mul(T(-2.5, 0.0, zB), RotY(3.14159265358979));
        world[oBodyB] = bodyB;
        // trailer: hitched 3 m behind vehicle B (away from the camera), articulating
        world[oTrailer] = Mul(bodyB, Mul(T(0.0, 0.0, -3.0), Mul(RotY(0.15 * sin(0.07 * fm)), T(0.0, 0.0, -4.0))));
        for (int k = 0; k < 4; ++k)
            world[oWheelB[k]] = Mul(bodyB, Mul(T(wheelPos[k][0], wheelPos[k][1], wheelPos[k][2]),
                                               RotX(sc.startVehicle ? drive(1.0) / 0.45 : (sc.matchVehicle ? 0.0 : 1.0 / 0.45 * f))));
        // foliage: a static alpha-tested panel between the camera lane and vehicle A's lane, ahead
        world[oFoliage] = T(1.4, 0.0, 34.5);
        // skinned mover: drifts sideways at 0.3 m / frame (S13: from frame 40; at 65 m so the camera does not pass it first)
        world[oSkin] = T(-8.0 + drive(0.1), 0.0, sc.startVehicle ? 65.0 : 45.0);
        // cabin: dashboard fixed in camera space; wiper rotating in camera space
        world[oDash] = Mul(camPose, T(0.0, -0.45, 0.9));
        world[oWiper] = Mul(camPose, Mul(T(-0.2, -0.25, 1.3), RotZ(0.05 * f)));    // constant angular speed
        for (int k = 0; k < 3; ++k) world[oPillar[k]] = Mul(camPose, T(-0.9 + 0.9 * k, 0.55, 1.0));
        world[oCab[0]] = Mul(camPose, T(-1.1, 0.1, 1.2)); world[oCab[1]] = Mul(camPose, T(1.1, 0.1, 1.2));
        world[oCab[2]] = Mul(camPose, T(0.5, -0.35, 0.8));
        // ---- MVPs ----
        for (int o = 0; o < nObj; ++o) {
            const Kind k = objs[o].kind;
            if (k == kInstanced) mvpCur[o] = VPw;                              // rows 4..7 = VP (not the instances' MVP)
            else if (k == kCabinStatic || k == kCabinMover) mvpCur[o] = Mul(Pc, Mul(V, world[o]));
            else mvpCur[o] = Mul(Pw, Mul(V, world[o]));
        }
        // ---- draw order: shuffled clumps, 3 random clumps culled; vehicle A / B bodies swap order every 8 frames ----
        std::vector<int> order;
        for (int o = 0; o < nObj; ++o) if (o != oCoverInst) order.push_back(o);
        std::vector<int> clumps(oClump, oClump + 30);
        std::shuffle(clumps.begin(), clumps.end(), rng);
        std::vector<bool> culled(nObj, false);
        for (int k = 0; k < 3; ++k) culled[clumps[k]] = true;
        {
            int ci = 0;
            for (int& o : order) if (objs[o].group == 2) o = clumps[ci++];
        }
        if ((f / 8) & 1) {
            auto a = std::find(order.begin(), order.end(), oBodyA), b = std::find(order.begin(), order.end(), oBodyB);
            std::iter_swap(a, b);
        }
        order.erase(std::remove_if(order.begin(), order.end(), [&](int o) { return culled[o]; }), order.end());
        // S3: segment 2 = the second half of the order + the cover instanced draw at the end
        int segSplit = (int)order.size();
        if (sc.segments) { segSplit = (int)order.size() / 2; order.push_back(oCoverInst); }
        for (int o = 0; o < nObj; ++o) drawnCur[o] = std::find(order.begin(), order.end(), o) != order.end();
        for (int o = 0; o < nObj; ++o) drawnRun[o] = drawnCur[o] ? drawnRun[o] + 1 : 0;

        // ---- pool selection ----
        const int pool = sc.rotatePool ? ((f / 7) & 1) : 0;
        ID3D11Buffer* ring = E.ring[f & 1].Get();
        // ---- cbuffer windows: draw d at offset d * 256 (16 constants); the mid-pass discard rewrites from the split on --
        const int nd = (int)order.size();
        const int discardAt = sc.midDiscard ? nd / 3 : -1;
        auto fillRing = [&](int from, int to, UINT baseOff) {
            D3D11_MAPPED_SUBRESOURCE mp{};
            if (FAILED(ctx->Map(ring, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { printf("ring map failed\n"); exit(2); }
            float* p = (float*)mp.pData;
            memset(p, 0, 2u << 20);
            for (int d = from; d < to; ++d) {
                const int o = order[d];
                float* w = p + (baseOff + (UINT)d * 256u) / 4;
                for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) w[(4 + r) * 4 + c] = (float)mvpCur[o].m[r][c];
                w[8 * 4 + 0] = (float)(o + 1); w[8 * 4 + 1] = (float)(d + 1); w[8 * 4 + 2] = objs[o].alpha ? 1.0f : 0.0f;
            }
            ctx->Unmap(ring, 0);
        };
        // the second half of the ring after a mid-pass discard uses windows at +1 MB (new contents)
        const UINT kAfterDiscard = 1u << 20;
        fillRing(0, discardAt >= 0 ? discardAt : nd, 0);

        // ---- G-buffer pass ----
        const FLOAT zero[4] = { 0, 0, 0, 0 };
        ctx->ClearDepthStencilView(E.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
        ctx->ClearRenderTargetView(E.truthRtv.Get(), zero);
        ctx->ClearRenderTargetView(E.colorRtv.Get(), zero);
        ID3D11RenderTargetView* rtvs[2] = { E.truthRtv.Get(), E.colorRtv.Get() };
        ctx->OMSetRenderTargets(2, rtvs, E.dsv.Get());
        ctx->OMSetDepthStencilState(E.dss.Get(), 1);
        // jittered viewports (Halton 2/3), as the injector shifts them
        auto halton = [](int i, int b) { double r = 0, fct = 1; while (i > 0) { fct /= b; r += fct * (i % b); i /= b; } return r; };
        const float jx = (float)(halton(f % 8 + 1, 2) - 0.5), jy = (float)(halton(f % 8 + 1, 3) - 0.5);
        const D3D11_VIEWPORT vpW = { jx, jy, (float)(W + g_vpOver), (float)(H + g_vpOver), 0.01f, 0.9f };
        const D3D11_VIEWPORT vpC = { jx, jy, (float)(W + g_vpOver), (float)(H + g_vpOver), 0.9f, 1.0f };
        const D3D11_RECT sc0 = { 0, 0, W, H };
        ctx->RSSetScissorRects(1, &sc0);
        ctx->PSSetShader(E.ps.Get(), nullptr, 0);
        rec.Reset();
        cand.Reset();
        cand.SetDropped(cam.MedoidDropBits());          // v0.10.0 phase 8: inject.cpp StartPass (mv_medoid_drop)
        bool inSegment = true;
        int lateFrom = -1;                              // first draw recorded after a replay of this pass (always replayed)
        UINT curBase = 0;
        for (int d = 0; d < nd; ++d) {
            const int o = order[d];
            if (d == discardAt) {
                // hkMap (inject.cpp DidOnDiscardMap): pending draws that read the ring are replayed first, the ring flushed
                // + sealed; then the game discards it and writes the rest at new offsets
                if (rec.PendingReplay() > 0 && rec.ReferencesPending(ring)) {
                    const auto rr = rec.Replay(ctx1, E.idRtv.Get(), E.roDsv.Get(), true, false);
                    if (!rr.ok) { printf("   FAIL forced replay\n"); ++fails; }
                    if (lateFrom < 0) lateFrom = d;
                } else { printf("   FAIL frame %d: the ring is not a pending draw's resource before the discard\n", f); ++fails; }
                rec.OnDiscard(ctx, ring);
                fillRing(d, nd, kAfterDiscard);
                curBase = kAfterDiscard;
            }
            if (sc.segments && d == segSplit) {
                // leave the G-buffer (inject.cpp: DidOnLeave replays before the game's next bind), bind something else,
                // come back
                rec.Replay(ctx1, E.idRtv.Get(), E.roDsv.Get(), false, false);
                if (lateFrom < 0) lateFrom = d;
                ID3D11RenderTargetView* other = E.scratchRtv.Get();
                ctx->OMSetRenderTargets(1, &other, nullptr);
                ctx->ClearRenderTargetView(other, zero);
                ctx->OMSetRenderTargets(2, rtvs, E.dsv.Get());
                inSegment = true;
            }
            const Obj& ob = objs[o];
            const Mesh& m = E.meshes[ob.mesh];
            const bool cabin = ob.kind == kCabinStatic || ob.kind == kCabinMover;
            ctx->RSSetViewports(1, cabin ? &vpC : &vpW);
            ctx->RSSetState(ob.cullNone ? E.rsNone.Get() : E.rsBack.Get());
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const UINT stride = sizeof(Vtx), off0 = 0;
            ID3D11Buffer* vbs[2] = { E.vb[pool].Get(), E.instVb[pool].Get() };
            const UINT strides[2] = { stride, 12 }, offs[2] = { 0, 0 };
            ctx->IASetVertexBuffers(0, 2, vbs, strides, offs);
            (void)off0;
            ctx->IASetIndexBuffer(E.ib[pool].Get(), DXGI_FORMAT_R32_UINT, 0);
            ID3D11ShaderResourceView* vsSrv[4] = { nullptr, nullptr, nullptr, ob.vs == 1 ? E.bonesSrv.Get() : nullptr };
            ctx->VSSetShaderResources(0, 4, vsSrv);
            if (ob.vs == 2) { ctx->IASetInputLayout(E.ilInst.Get()); ctx->VSSetShader(E.vsInst.Get(), nullptr, 0); }
            else { ctx->IASetInputLayout(E.il.Get()); ctx->VSSetShader(ob.vs == 1 ? E.vsSkin.Get() : E.vsMain.Get(), nullptr, 0); }
            const UINT first = (curBase + (UINT)d * 256u) / 16u, num = 16u;
            ctx1->VSSetConstantBuffers1(0, 1, &ring, &first, &num);
            ctx1->PSSetConstantBuffers1(0, 1, &ring, &first, &num);
            const bool inst = ob.kind == kInstanced;
            // the injector's hooks: camera candidates (world / cabin layers, non-instanced) + the draw record
            if (!inst) {
                CandidateRecord::DrawKey k{};
                k.ib = E.ib[pool].Get(); k.vb = E.vb[pool].Get(); k.indexCount = m.ic; k.startIndex = m.si; k.baseVertex = m.bv;
                const int layer = cabin ? 1 : 0;
                if (cand.Count(layer) < CandidateRecord::kSlots && !cand.Dropped(layer)) cand.Record(ctx, layer, ring, first * 16u + 64u, k);
            }
            const UINT sinst = o == oInst1 ? 12u : (o == oCoverInst ? 20u : 0u);
            LARGE_INTEGER q0, q1; QueryPerformanceCounter(&q0);
            rec.Record(ctx1, m.ic, inst ? (UINT)ob.instances : 1u, m.si, m.bv, inst ? sinst : 0u, inst);
            QueryPerformanceCounter(&q1); recTicks += q1.QuadPart - q0.QuadPart; ++recN;
            if (inst) ctx->DrawIndexedInstanced(m.ic, ob.instances, m.si, m.bv, sinst);
            else ctx->DrawIndexed(m.ic, m.si, m.bv);
        }
        (void)inSegment;
        // ---- G-buffer leave: replay, mirror flush ----
        LARGE_INTEGER r0, r1; QueryPerformanceCounter(&r0);
        const auto rr = rec.Replay(ctx1, E.idRtv.Get(), E.roDsv.Get(), false, false);
        QueryPerformanceCounter(&r1); repTicks += r1.QuadPart - r0.QuadPart; repN += rr.replayed;
        if (!rr.ok) { printf("   FAIL replay (frame %d)\n", f); ++fails; }
        if (g_rstatic) { DrawIdRecord::SetStaticGate(false); statSkipTot += rec.StaticSkipped(); statRepTot += rec.ReplayedDraws(); }
        if (g_rstatic && sc.matchVehicle) {             // phase 14 round 3 (S14): vehicle A's body / plate
            for (int d = 0; d < nd; ++d) {
                if ((order[d] != oBodyA && order[d] != oPlateA) || !rec.StaticSkippedAt(d)) continue;
                if (f < kMatchShallow) ++matchSkipDeep;
                else if (f > kMatchShallow + kLagMax) {
                    ++matchSkipLate;
                    if (matchSkipLate <= 3) printf("   FAIL frame %d: %s skipped although it drifts off the camera motion\n", f, objs[order[d]].name);
                }
            }
        }
        ID3D11RenderTargetView* nullRtv = nullptr;
        ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
        // ---- depth snapshot + MV generation (the blit) ----
        ctx->CopyResource(E.twin.Get(), E.depth.Get());
        CameraMv::FrameStats fs;
        // v0.10.0 phase 8 debugging aid: environment HDUMP=<frame> requests a DrawIdMv dump of that S1-S4 frame (needs a
        // build\tests\dlaa.ini with debug = 1; the lines land in build\tests\dlaa_inject.log)
        {
            char ev[16] = {};
            if (GetEnvironmentVariableA("HDUMP", ev, sizeof(ev)) && atoi(ev) == f) {
                // v0.10.0 phase 21: HPROBE=1 adds the pixel probe around the image centre ('MV probe' lines)
                char pv[16] = {};
                const bool probe = GetEnvironmentVariableA("HPROBE", pv, sizeof(pv)) && atoi(pv) != 0;
                cam.DrawIds().RequestDump("harness HDUMP", probe ? 0.5f : -1.0f, probe ? 0.5f : -1.0f);
            }
        }
        const bool ok = cam.Generate(ctx, W, H, 0, 0, W, H, cand, E.twinSrv.Get(), E.dUav.Get(), E.mvUav.Get(), &fs, false,
                                     nullptr, nullptr, nullptr, &rec, rec.ReplayFailed() ? nullptr : E.idSrv.Get());
        if (!ok) { printf("   FAIL Generate (frame %d)\n", f); ++fails; }
        if (f >= 2 && fs.worldMiss && !fs.missHeld) ++resets;
        if (f >= 1 && cam.DidN() == 0) { printf("   FAIL frame %d: pass B ran without draw ids\n", f); ++fails; }

        // ---- readbacks ----
        ctx->CopyResource(E.stMv.Get(), E.mv.Get());
        ctx->CopyResource(E.stDepth.Get(), E.dOut.Get());
        ctx->CopyResource(E.stTruth.Get(), E.truth.Get());
        ctx->CopyResource(E.stId.Get(), E.idTex.Get());
        if (cam.DidRSrv()) {
            ComPtr<ID3D11Resource> rres;
            cam.DidRSrv()->GetResource(&rres);
            const D3D11_BOX box{ 0, 0, 0, (UINT)nd * DrawIdMv::kRStride * 16u, 1, 1 };
            ctx->CopySubresourceRegion(E.stR.Get(), 0, 0, 0, 0, rres.Get(), 0, &box);
        }
        D3D11_MAPPED_SUBRESOURCE mMv{}, mD{}, mT{}, mI{}, mR{};
        ctx->Map(E.stMv.Get(), 0, D3D11_MAP_READ, 0, &mMv);
        CmpFrame(mMv, W, H);                            // v0.10.0 phase 8 comparison mode
        ctx->Map(E.stDepth.Get(), 0, D3D11_MAP_READ, 0, &mD);
        ctx->Map(E.stTruth.Get(), 0, D3D11_MAP_READ, 0, &mT);
        ctx->Map(E.stId.Get(), 0, D3D11_MAP_READ, 0, &mI);
        ctx->Map(E.stR.Get(), 0, D3D11_MAP_READ, 0, &mR);
        const float* Rt = (const float*)mR.pData;

        // per draw: state from the R table; expected class per object
        std::vector<int> stCur(nObj, -1);
        std::vector<char> parMark(nObj, 0);                 // v0.10.0 phase 6: took a rigid parent's R / state
        int paired = 0, elig = 0;
        for (int d = 0; d < nd; ++d) {
            const int o = order[d];
            const int st = (int)(Rt[(d * 5 + 4) * 4] + 0.5f);
            stCur[o] = st;
            parMark[o] = ParentMarked(Rt[(d * 5 + 4) * 4 + 3]) ? 1 : 0;
            if (objs[o].kind != kInstanced) { ++elig; if (st == 2 || st == 3) ++paired; }
        }
        if (f >= 1) { pairedSum += (uint64_t)paired; eligSum += (uint64_t)elig; }
        // exact camera R per layer (double) and the camera R pass A produced (GPU, read back: test only). Mode 2 decides
        // "static" against the GPU's camera R and pass B uses it for every non-mover pixel, so the expectations below use
        // it; how far it is from the exact one is pass A's quality, reported separately.
        M4 RcamX[2], RcamG[2];
        {
            static M4 VprevX;
            if (f == 0) VprevX = V;
            M4 Pinv, Vinv; Inv(Pw, Pinv); Inv(V, Vinv);
            RcamX[0] = Mul(Pw, Mul(VprevX, Mul(Vinv, Pinv)));
            RcamX[1] = I4();                            // cabin: camera-attached candidates -> identity
            VprevX = V;
            float sv[CameraMv::kSolveFloats];
            if (!cam.ReadSolveBlocking(ctx, sv)) { printf("   FAIL solve readback\n"); ++fails; }
            for (int l = 0; l < 2; ++l)
                for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) RcamG[l].m[r][c] = sv[l * 16 + r * 4 + c];
            double e = 0;
            for (int l = 0; l < 2; ++l) for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
                e = std::max(e, std::fabs(RcamG[l].m[r][c] - RcamX[l].m[r][c]));
            if (f >= 1 && e > 1e-3) ++camBad;
        }
        std::vector<int> wantCur(nObj, -1), wantAlt(nObj, -1);
        for (int o = 0; o < nObj; ++o) {
            if (!drawnCur[o]) continue;
            const Kind k = objs[o].kind;
            const bool hist = f >= 1 && drawnPrev[o];
            const bool cabinO = k == kCabinStatic || k == kCabinMover;
            int want = -1, alt = -1;                    // the state this draw must have (alt: also accepted)
            double tdev = 0.0;                          // true probe deviation of the object's motion from the camera's (px)
            if (k != kInstanced && (hist || (f >= 1 && parMark[o]))) {
                const M4 P = Mul(RcamG[cabinO ? 1 : 0], mvpCur[o]);
                const double probes[6][4] = { {0,0,0,1}, {4,0,0,1}, {-4,0,0,1}, {0,0,4,1}, {0,0,-4,1}, {0,4,0,1} };
                for (auto& q : probes) {
                    double a[4], b[4];
                    Xf(mvpPrev[o], q, a); Xf(P, q, b);
                    double dv;
                    const bool on = a[3] > 0.05 && b[3] > 0.05 && std::fabs(a[0] / a[3]) <= 1.5 && std::fabs(a[1] / a[3]) <= 1.5 &&
                                    std::fabs(b[0] / b[3]) <= 1.5 && std::fabs(b[1] / b[3]) <= 1.5;
                    if (on) dv = std::hypot((a[0] / a[3] - b[0] / b[3]) * W * 0.5, (a[1] / a[3] - b[1] / b[3]) * H * 0.5);
                    else dv = std::hypot((a[0] - b[0]) * W * 0.5, (a[1] - b[1]) * H * 0.5) / std::max(std::max(std::fabs(a[3]), std::fabs(b[3])), 1.0);
                    tdev = std::max(tdev, dv);
                }
            }
            if (k == kInstanced) want = DrawIdMv::kStInstanced;
            else if (!hist) want = DrawIdMv::kStUnpaired;
            else if (tdev >= snapPx * 1.5 + 0.02) want = DrawIdMv::kStMover;
            else if (tdev <= snapPx * 0.5) want = DrawIdMv::kStStatic;
            else { want = DrawIdMv::kStStatic; alt = DrawIdMv::kStMover; }   // inside the band around the snap threshold
            // identical copies (ambiguous group): a mover verdict needs the previous draw's own track, i.e. the object must
            // have been drawn (and paired) in the frame before the previous one too; on its first paired frame -> camera R
            if (dupGroup(o) && drawnRun[o] == 2) {
                if (want == DrawIdMv::kStMover) want = DrawIdMv::kStUnpaired;
                else if (alt == DrawIdMv::kStMover) alt = DrawIdMv::kStUnpaired;
            }
            if (f >= 1 && parMark[o] && want == DrawIdMv::kStUnpaired) ParentWant(tdev, snapPx, &want, &alt);   // phase 6
            wantCur[o] = want; wantAlt[o] = alt;
            ++checks;
            if (stCur[o] != want && stCur[o] != alt) {
                ++wrongState[o]; ++fails;
                if (wrongState[o] <= 3) {
                    int dd = -1;
                    for (int q = 0; q < nd; ++q) if (order[q] == o) dd = q;
                    printf("   FAIL frame %d: %s #%d state %d, want %d (alt %d), true dev %.3f px, GPU dev %.3f px\n", f, objs[o].name,
                           o, stCur[o], want, alt, tdev, dd >= 0 ? (double)Rt[(dd * 5 + 4) * 4 + 3] : -1.0);
                    static int solveDumps = 0;
                    if (want == DrawIdMv::kStStatic && solveDumps++ < 3) {
                        float sv[CameraMv::kSolveFloats];
                        if (cam.ReadSolveBlocking(ctx, sv)) {
                            const M4& X = RcamX[0];
                            printf("      R_world GPU vs exact (row-major):\n");
                            for (int r = 0; r < 4; ++r)
                                printf("      %+.6f %+.6f %+.6f %+.6f | %+.6f %+.6f %+.6f %+.6f\n", sv[r * 4], sv[r * 4 + 1], sv[r * 4 + 2],
                                       sv[r * 4 + 3], X.m[r][0], X.m[r][1], X.m[r][2], X.m[r][3]);
                            printf("      solve info [8] %.3f %.3f %.3f %.3f [10] %.3f %.3f %.3f %.3f\n", sv[32], sv[33], sv[34], sv[35],
                                   sv[40], sv[41], sv[42], sv[43]);
                            double c0[4]; const double org[4] = { 0, 0, 0, 1 }; Xf(mvpCur[o], org, c0);
                            printf("      object origin clip (%.3f %.3f %.3f %.3f)\n", c0[0], c0[1], c0[2], c0[3]);
                        }
                    }
                }
            }
            // flicker: the state changed between two frames whose expected state did not
            if (f >= 2 && drawnPrev[o] && stPrev[o] >= 0 && stCur[o] != stPrev[o] && want == wantPrev[o] && alt < 0 &&
                wantAltPrev[o] < 0 && k != kInstanced) ++flick[o];
        }
        // ---- per pixel ----
        // expected R per object: a mover's own true R, every other draw the camera R pass A produced for its layer
        const M4* Rcam = RcamG;
        std::vector<M4> Rexp(nObj, I4());
        std::vector<int> layerOf(nObj, 0);
        for (int o = 0; o < nObj; ++o) {
            const Kind k = objs[o].kind;
            layerOf[o] = (k == kCabinStatic || k == kCabinMover) ? 1 : 0;
            if (k != kInstanced && f >= 1 && (drawnPrev[o] || parMark[o]) && stCur[o] == DrawIdMv::kStMover) {
                M4 ic; Inv(mvpCur[o], ic);
                Rexp[o] = Mul(mvpPrev[o], ic);         // a mover: its own exact R
            } else Rexp[o] = Rcam[layerOf[o]];          // static (snapped), unpaired, instanced: the camera R
        }
        std::vector<int> pxOk(nObj, 0), pxAll(nObj, 0), lagPx(nObj, 0);
        const double tolStatic = 0.03, tolMover = 0.03 + snapPx;
        for (int y = 0; y < H; ++y) {
            const uint16_t* mvRow = (const uint16_t*)((const uint8_t*)mMv.pData + y * mMv.RowPitch);
            const float* dRow = (const float*)((const uint8_t*)mD.pData + y * mD.RowPitch);
            const uint32_t* tRow = (const uint32_t*)((const uint8_t*)mT.pData + y * mT.RowPitch);
            const uint16_t* iRow = (const uint16_t*)((const uint8_t*)mI.pData + y * mI.RowPitch);
            for (int x = 0; x < W; ++x) {
                const uint32_t tObj = tRow[x * 2], tDraw = tRow[x * 2 + 1];
                if (!tObj) continue;
                const int o = (int)tObj - 1, d = (int)tDraw - 1;
                // id ownership: replayed draws own exactly their pixels; first-segment instanced draws were not replayed
                // (v0.10.0 phase 14: nor the draws the static gate skipped -- they own no id pixel at all)
                const bool skippedS = g_rstatic != 0 && rec.StaticSkippedAt(d);
                const bool replayed = !skippedS && (objs[o].kind != kInstanced || (lateFrom >= 0 && d >= lateFrom));
                const uint32_t wantId = replayed ? tDraw : 0u;
                ++idChecked;
                if (iRow[x] != wantId) {
                    ++idMismatch; ++idBad[o];
                    if (idMismatch <= 5) printf("   FAIL id at (%d,%d) frame %d: %u, want %u (%s)\n", x, y, f, iRow[x], wantId, objs[o].name);
                }
                // v0.10.0 phase 14: a skipped draw that is a MOVER this frame keeps the camera R (the gate learns it 1-2 frames
                // late) -- counted as lag (bounded below), not as an MV failure; a skipped static draw is checked as usual
                if (skippedS && stCur[o] == DrawIdMv::kStMover) { ++lagPx[o]; ++lagPxTot; continue; }
                // motion vector vs the object's true R
                const double dd = dRow[x];
                const bool cab = dd >= 0.9;
                const double z = cab ? (dd - 0.9) / 0.1 : (dd - 0.01) / 0.89;
                const double u = (x + 0.5) / W, v = (y + 0.5) / H;
                const double p4[4] = { u * 2 - 1, 1 - v * 2, std::min(1.0, std::max(0.0, z)), 1.0 };
                double c[4];
                Xf(Rexp[o], p4, c);
                double ex = 0, ey = 0;
                if (c[3] > 1e-6) { ex = ((c[0] / c[3]) * 0.5 + 0.5 - u) * W; ey = ((0.5 - (c[1] / c[3]) * 0.5) - v) * H; }
                const double gx = H2F(mvRow[x * 2]), gy = H2F(mvRow[x * 2 + 1]);
                const double err = std::sqrt((gx - ex) * (gx - ex) + (gy - ey) * (gy - ey));
                const bool mover = objs[o].kind == kMover || objs[o].kind == kCabinMover;
                // fp16 storage of the MV: half-ulp of |mv| (~1/2048 relative) on top
                const double tol = (mover ? tolMover : tolStatic) + std::max(std::fabs(ex), std::fabs(ey)) / 1024.0;
                ++mvChecked; ++pxAll[o]; ++covTot[o];
                if ((o == oBodyA || o == oPlateA || (o >= oWheelA[0] && o <= oWheelA[3])) && x > 0 && y > 0 && x < W - 1 && y < H - 1) {
                    bool nf = false;
                    for (int yy = -1; yy <= 1 && !nf; ++yy) {
                        const uint32_t* tr = (const uint32_t*)((const uint8_t*)mT.pData + (y + yy) * mT.RowPitch);
                        for (int xx = -1; xx <= 1; ++xx) if (tr[(x + xx) * 2] == (uint32_t)(oFoliage + 1)) { nf = true; break; }
                    }
                    if (nf) ++throughFoliage;
                }
                const int g = objs[o].group;
                if (err > worstMv[g]) worstMv[g] = err;            // largest error per group over all pixels
                if (err <= tol) ++pxOk[o];
                else ++mvFails;
            }
        }
        for (int o = 0; o < nObj; ++o) {                                  // v0.10.0 phase 14: lag runs per object
            if (lagPx[o]) { ++lagFrames[o]; if (++lagRun[o] > lagMaxRun[o]) lagMaxRun[o] = lagRun[o]; }
            else lagRun[o] = 0;
        }
        for (int o = 0; o < nObj; ++o) {
            if (!pxAll[o]) continue;
            ++checks;
            if (pxOk[o] * 1000 < pxAll[o] * 995) {                       // < 99.5 % of the object's pixels right
                ++fails; ++objFramesBad[o];
                if (objFramesBad[o] <= 2)
                    printf("   FAIL frame %d: %s #%d MV right on %d of %d px (state %d)\n", f, objs[o].name, o, pxOk[o], pxAll[o], stCur[o]);
            }
        }
        ctx->Unmap(E.stMv.Get(), 0); ctx->Unmap(E.stDepth.Get(), 0); ctx->Unmap(E.stTruth.Get(), 0);
        ctx->Unmap(E.stId.Get(), 0); ctx->Unmap(E.stR.Get(), 0);
        if (f == 0) {
            std::vector<int> cov(nObj, 0);
            int tot0 = 0;
            for (int o = 0; o < nObj; ++o) { cov[o] = pxAll[o]; tot0 += pxAll[o]; }
            printf("   frame 0 coverage: %d px owned; vehicle A body %d, wheel %d, plate %d, trailer %d, B body %d, clump %d, "
                   "foliage %d, instanced %d, skinned %d, dash %d, wiper %d, box %d\n", tot0, cov[oBodyA], cov[oWheelA[0]],
                   cov[oPlateA], cov[oTrailer], cov[oBodyB], cov[oClump[0]], cov[oFoliage], cov[oInst0], cov[oSkin], cov[oDash],
                   cov[oWiper], cov[oBox[0]]);
        }
        if (g_debugLayer) { const int dl = DrainDebugLayer(sc.name); ++checks; if (dl) { ++fails; debugMsgs += dl; } }
        // ---- commit this frame as the next one's previous ----
        for (int o = 0; o < nObj; ++o) { mvpPrev[o] = mvpCur[o]; drawnPrev[o] = drawnCur[o]; stPrev[o] = drawnCur[o] ? stCur[o] : -1;
                                         wantPrev[o] = wantCur[o]; wantAltPrev[o] = wantAlt[o]; }
    }
    rec.Shutdown();
    checks += 3;
    // a draw of a LATER G-buffer segment that ties (exactly equal depth) with an earlier segment's draw wins the replay, the
    // game's GREATER test keeps the earlier one (documented limit; within one segment the reverse-order replay matches the
    // game). The multi-segment scene tolerates such pixels up to 1e-5 of the owned pixels.
    const int idTol = sc.segments ? idChecked / 100000 : 0;
    if (idMismatch > idTol) { ++fails; }
    else if (idMismatch) printf("   (%d id mismatches = cross-segment coplanar ties, tolerated: <= %d)\n", idMismatch, idTol);
    if (resets) { ++fails; printf("   FAIL history resets after frame 1: %d\n", resets); }
    int flickTot = 0;
    for (int o = 0; o < nObj; ++o) flickTot += flick[o];
    if (flickTot) { ++fails; printf("   FAIL draw state flicker (state changes between consecutive frames): %d\n", flickTot); }
    for (int o = 0; o < nObj; ++o) if (idBad[o]) printf("   id mismatches on %s #%d: %d px\n", objs[o].name, o, idBad[o]);
    if (g_rstatic) {                                    // v0.10.0 phase 14: the static replay skip
        int worstLag = 0, worstO = -1, lagObjs = 0;
        for (int o = 0; o < nObj; ++o) {
            if (lagFrames[o]) ++lagObjs;
            if (lagMaxRun[o] > worstLag) { worstLag = lagMaxRun[o]; worstO = o; }
        }
        int keysAt = 0, held = 0;
        const int keys = DrawIdMv::StaticKeys(&keysAt, &held);
        printf("   static replay skip (mv_replay_static_frames %u, every %u): skipped %lld of %lld G-buffer draws (%.1f %%), "
               "table %d keys (%d at the streak); skipped movers (lag): %d objects, %lld px, longest run %d frames%s%s (limit %d); "
               "table clears so far: history %llu, camera R %llu, record %llu\n",
               DrawIdMv::ReplayStaticFrames(), DrawIdMv::ReplayStaticEvery(), statSkipTot, statSkipTot + statRepTot,
               (statSkipTot + statRepTot) ? 100.0 * (double)statSkipTot / (double)(statSkipTot + statRepTot) : 0.0, keys, keysAt,
               lagObjs, lagPxTot, worstLag, worstO >= 0 ? " on " : "", worstO >= 0 ? objs[worstO].name : "", kLagMax,
               (unsigned long long)DrawIdMv::StaticClears(DrawIdMv::kStatClrHistory),
               (unsigned long long)DrawIdMv::StaticClears(DrawIdMv::kStatClrCamR),
               (unsigned long long)DrawIdMv::StaticClears(DrawIdMv::kStatClrRecord));
        checks += 2;
        if (statSkipTot == 0) { ++fails; printf("   FAIL static replay skip: the gate never skipped a draw (the scene must exercise it)\n"); }
        if (worstLag > kLagMax) { ++fails; printf("   FAIL static replay skip: a skipped mover kept the camera R for %d frames in a row\n", worstLag); }
        if (sc.matchVehicle) {                          // phase 14 round 3: deep static skipped, drifting never
            printf("   S14 vehicle A body / plate skipped: %d draw-frames while exactly static (frames < %d), %d after it began to "
                   "drift (frames > %d: must be 0)\n", matchSkipDeep, kMatchShallow, matchSkipLate, kMatchShallow + kLagMax);
            checks += 2;
            if (matchSkipDeep == 0) { ++fails; printf("   FAIL S14: the exactly static vehicle was never skipped (the scene must exercise it)\n"); }
            if (matchSkipLate != 0) { ++fails; }
        }
    }
    printf("   id ownership: %d of %d owned pixels wrong; MV: %d of %d pixels outside tolerance; paired %.1f %% of the "
           "non-instanced draws (frames >= 1); state flicker %d; history resets %d\n", idMismatch, idChecked, mvFails,
           mvChecked, eligSum ? 100.0 * (double)pairedSum / (double)eligSum : 0.0, flickTot, resets);
    printf("   pixels over the scene: vehicle A body %lld, wheels %lld %lld %lld %lld, plate %lld, trailer %lld, B body %lld, "
           "skinned %lld, wiper %lld, clumps(first) %lld, foliage %lld, instanced %lld; vehicle-A pixels at / through the "
           "alpha-tested foliage %lld\n", covTot[oBodyA], covTot[oWheelA[0]], covTot[oWheelA[1]], covTot[oWheelA[2]],
           covTot[oWheelA[3]], covTot[oPlateA], covTot[oTrailer], covTot[oBodyB], covTot[oSkin], covTot[oWiper], covTot[oClump[0]],
           covTot[oFoliage], covTot[oInst0] + covTot[oInst1], throughFoliage);
    { LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
      printf("   CPU: Record %.2f us / draw (%lld draws), final Replay %.2f us / replayed draw (%lld; WARP's own draw cost included)\n",
             recN ? 1e6 * (double)recTicks / (double)qf.QuadPart / (double)recN : 0.0, recN,
             repN ? 1e6 * (double)repTicks / (double)qf.QuadPart / (double)repN : 0.0, repN); }
    printf("   camera R (pass A) off the exact one by > 1e-3 in %d frames (pass A's fallback pairing on pool-rotation frames; "
           "per-draw pairs do not depend on it)\n", camBad);
    if (g_debugLayer) printf("   debug layer: %d error / warning messages\n", debugMsgs);
    printf("   worst MV error per group (px): ground %.3f box %.3f clump %.3f bodyA %.3f wheelA %.3f plate %.3f trailer %.3f "
           "B %.3f foliage %.3f inst %.3f skin %.3f dash %.3f wiper %.3f cover %.3f\n", worstMv[0], worstMv[1], worstMv[2],
           worstMv[3], worstMv[4], worstMv[5], worstMv[6], worstMv[7], worstMv[8], worstMv[9], worstMv[10], worstMv[11],
           worstMv[12], worstMv[13]);
    const DrawIdMv::Stats& ds = cam.DrawIds().GetStats();
    PrintPhase8(ctx, ds, "");
    printf("   DrawIdMv: %llu frames, %llu draws, %llu via the pointer-free key, %llu without a partner group\n",
           (unsigned long long)ds.frames, (unsigned long long)ds.draws, (unsigned long long)ds.looseDraws,
           (unsigned long long)ds.noGroup);
    printf("   camera R consensus (v0.10.0 phase 3): world %llu frames / medoid fallback %llu (cluster avg %.1f draws), cabin %llu "
           "/ %llu; |consensus - medoid| avg %.3f px\n", (unsigned long long)ds.consFrames[0],
           (unsigned long long)ds.consFallback[0], ds.consFrames[0] ? (double)ds.consCluster[0] / (double)ds.consFrames[0] : 0.0,
           (unsigned long long)ds.consFrames[1], (unsigned long long)ds.consFallback[1],
           ds.consDisFrames[0] ? ds.consDisSum[0] / (double)ds.consDisFrames[0] : 0.0);
    // v0.10.0 phase 19 (informational): the small-cabin rule in this truck cab of 8 draws -- the medoid must be KEPT (bit-identical)
    printf("   phase 19: small cabin layer (mv_cabin_small %u): medoid kept %llu / cluster R used %llu readbacks (max dev to medoid %.4f "
           "px), cabin paired %.2f / voters %.2f per readback, rows 4..7 not an MVP %llu\n", DrawIdMv::CabinSmall(),
           (unsigned long long)ds.cabSmallKept, (unsigned long long)ds.cabSmallUsed, (double)cam.DrawIds().TakeCabSmallDevMax(),
           ds.rbFrames ? (double)ds.cabPaired / (double)ds.rbFrames : 0.0, ds.rbFrames ? (double)ds.cabVoters / (double)ds.rbFrames : 0.0,
           (unsigned long long)ds.notMvp);
    printf("   %s: %d / %d checks passed\n", fails ? "FAILED" : "passed", checks - fails, checks);
    tot.checks += checks; tot.fails += fails;
    return fails;
}

// ---- S5: a MIRROR view + the main view (v0.10.0 phase 2 mirror units) -------------------------------------------------
// The game renders every truck mirror as its own small G-buffer view BEFORE the main one: own depth, own draw record, own
// CameraMv (= one "mirror unit" in inject.cpp, MirUnit). S5 renders, every frame, a 480x240 backward-looking mirror view
// (camera rigidly attached to the moving truck) and then the 960x540 main view; both read their MVPs from ONE per-frame
// cbuffer ring (the mirror's windows from 1 MB on, one WRITE_DISCARD per frame) and each has its own record / replay at its
// G-buffer leave (read-only DSV of ITS depth) / depth snapshot / CameraMv + DrawIdMv -- the unit split of inject.cpp, with
// the two units interleaved in one frame. The own TRAILER and the cab side are STATIC relative to the mirror camera while
// the world moves: their per-draw R must be the identity (zero motion vectors), where the camera R would move them by many
// pixels (checked: "the scene exercises it"). An overtaking car (unique mesh) with 4 identical spinning wheels passes
// through both views. Every 37th frame the mirror is NOT rendered (a throttled mirror): its next frame pairs with the last
// RENDERED one (inject.cpp resets that unit's DLSS history then; the motion vectors must still be exact for the gap).
// Checks per view and frame as S1 (exact id ownership, MV per pixel vs the object's true R, draw states, flicker, no world
// miss) + the trailer is a mover with R = I in the mirror. No mirror-image flip (the classes do not care about handedness).
struct View {
    int w = 0, h = 0;
    ComPtr<ID3D11Texture2D> depth, truth, color, twin, dOut, mv, idTex, stMv, stDepth, stTruth, stId;
    ComPtr<ID3D11DepthStencilView> dsv, roDsv;
    ComPtr<ID3D11RenderTargetView> truthRtv, colorRtv, idRtv;
    ComPtr<ID3D11ShaderResourceView> twinSrv, idSrv;
    ComPtr<ID3D11UnorderedAccessView> dUav, mvUav;
};
bool MakeView(Env& E, View& v, int w, int h) {
    ID3D11Device* d = E.dev.Get();
    v.w = w; v.h = h;
    if (!MakeTex(d, w, h, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D11_BIND_DEPTH_STENCIL, &v.depth) ||
        FAILED(d->CreateDepthStencilView(v.depth.Get(), nullptr, &v.dsv))) return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dv.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
    if (FAILED(d->CreateDepthStencilView(v.depth.Get(), &dv, &v.roDsv))) return false;
    if (!MakeTex(d, w, h, DXGI_FORMAT_R32G32_UINT, D3D11_BIND_RENDER_TARGET, &v.truth) ||
        FAILED(d->CreateRenderTargetView(v.truth.Get(), nullptr, &v.truthRtv))) return false;
    if (!MakeTex(d, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, &v.color) ||
        FAILED(d->CreateRenderTargetView(v.color.Get(), nullptr, &v.colorRtv))) return false;
    if (!MakeTex(d, w, h, DXGI_FORMAT_R16_UINT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &v.idTex) ||
        FAILED(d->CreateRenderTargetView(v.idTex.Get(), nullptr, &v.idRtv)) ||
        FAILED(d->CreateShaderResourceView(v.idTex.Get(), nullptr, &v.idSrv))) return false;
    if (!MakeTex(d, w, h, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_SHADER_RESOURCE, &v.twin)) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    if (FAILED(d->CreateShaderResourceView(v.twin.Get(), &sd, &v.twinSrv))) return false;
    if (!MakeTex(d, w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &v.dOut) ||
        FAILED(d->CreateUnorderedAccessView(v.dOut.Get(), nullptr, &v.dUav))) return false;
    if (!MakeTex(d, w, h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &v.mv) ||
        FAILED(d->CreateUnorderedAccessView(v.mv.Get(), nullptr, &v.mvUav))) return false;
    return MakeTex(d, w, h, DXGI_FORMAT_R16G16_FLOAT, 0, &v.stMv, true) &&
           MakeTex(d, w, h, DXGI_FORMAT_R32_FLOAT, 0, &v.stDepth, true) &&
           MakeTex(d, w, h, DXGI_FORMAT_R32G32_UINT, 0, &v.stTruth, true) &&
           MakeTex(d, w, h, DXGI_FORMAT_R16_UINT, 0, &v.stId, true);
}
M4 ProjA(double nearM, double aspect) { M4 r{}; r.m[0][0] = kF / aspect; r.m[1][1] = kF; r.m[2][3] = nearM; r.m[3][2] = 1; return r; }

int RunMirrorScene(Env& E, float snapPx, Totals& tot) {
    const int frames = 110, skipEvery = 37;
    printf("\n===== S5 mirror view + main view, two units interleaved (%d frames, snap %.3f px) =====\n", frames, (double)snapPx);
    DrawIdMv::SetParams(8.0f, snapPx);
    // ---- objects (world space; the truck drives along +z) ----
    std::vector<Obj> objs;
    auto add = [&](const char* n, Kind k, int mesh, int vs, bool cn, int inst, int group) {
        objs.push_back({ n, k, mesh, vs, cn, false, inst, group }); return (int)objs.size() - 1; };
    const int oGround = add("ground", kStatic, MGround, 0, true, 0, 0);
    int oBox[10];
    for (int k = 0; k < 10; ++k) oBox[k] = add("static box", kStatic, MBox0 + k, 0, false, 0, 1);
    int oClump[12];
    for (int k = 0; k < 12; ++k) oClump[k] = add("grass clump (12 identical)", kStatic, MClump, 0, true, 0, 2);
    const int oCab = add("own cab (rigid with both cameras)", kMover, MBody, 0, false, 0, 3);
    const int oTrailer = add("own trailer (rigid with the mirror camera)", kMover, MTrailer, 0, false, 0, 4);
    const int oCar = add("overtaking car (unique mesh)", kMover, MSkin, 0, false, 0, 5);
    int oWheel[4];
    for (int k = 0; k < 4; ++k) oWheel[k] = add("car wheel (4 identical, spinning)", kMover, MWheel, 0, false, 0, 6);
    const int oInst = add("instanced vegetation", kInstanced, MInst, 2, true, 12, 7);
    const int nObj = (int)objs.size();
    (void)oGround;
    auto dupGroup = [&](int o) { int c = 0; for (int q = 0; q < nObj; ++q) if (objs[q].mesh == objs[o].mesh) ++c; return c > 1; };

    // ---- two units: [0] mirror (480x240, looking back), [1] main (960x540, looking forward) ----
    struct Unit {
        const char* name; View v; CameraMv cam; CandidateRecord cand; DrawIdRecord rec; UINT ringBase = 0;
        std::vector<M4> mvpPrev, mvpCur; std::vector<bool> drawnPrev, drawnCur; std::vector<int> stPrev, stCur, wantPrev,
        wantAltPrev, drawnRun, flick, wrongState, objFramesBad, idBad;
        int frames = 0, resets = 0, idMismatch = 0, idChecked = 0, mvFails = 0, mvChecked = 0, phase = 0;
        uint64_t pairedSum = 0, eligSum = 0; double worstMv[8] = {}; bool rendered = false, renderedPrev = false;
        long long trailerPx = 0, trailerCamPxSum = 0; double trailerCamMvSum = 0.0; int trailerMoverFrames = 0;
        int trailerFrames = 0;
        int sinceSkip = 100;                            // rendered frames since the last skipped one (mirror)
    };
    std::unique_ptr<Unit> U[2] = { std::unique_ptr<Unit>(new Unit), std::unique_ptr<Unit>(new Unit) };
    U[0]->name = "mirror 480x240"; U[1]->name = "main 960x540";
    U[0]->ringBase = 1u << 20; U[1]->ringBase = 0;
    if (!MakeView(E, U[0]->v, 480, 240) || !MakeView(E, U[1]->v, W, H)) { printf("view textures failed\n"); return 1; }
    for (auto& u : U) {
        if (!u->cam.Init(E.dev.Get())) { printf("CameraMv init failed\n"); return 1; }
        u->cam.SetEgoPixel(0.0f); u->cam.SetEgoOrigin(0.0f);
        u->cam.SetQuiet(true);                          // as inject.cpp's mirror units (also exercises the quiet flag)
        u->cand.Init(E.dev.Get());
        u->mvpPrev.assign(nObj, I4()); u->mvpCur.assign(nObj, I4()); u->drawnPrev.assign(nObj, false);
        u->drawnCur.assign(nObj, false);
        for (auto* vec : { &u->stPrev, &u->stCur, &u->wantPrev, &u->wantAltPrev }) vec->assign(nObj, -1);
        for (auto* vec : { &u->drawnRun, &u->flick, &u->wrongState, &u->objFramesBad, &u->idBad }) vec->assign(nObj, 0);
    }
    int checks = 0, fails = 0;
    const FLOAT zero[4] = { 0, 0, 0, 0 };
    const D3D11_RECT sc0big = { 0, 0, 4096, 4096 };
    std::mt19937 rng(4242);
    GpuPerf::Restart();
    for (int f = 0; f < frames; ++f) {
        PerfFrame();
        // ---- truck + cameras ----
        const M4 truck = Mul(T(0.0, 0.0, 0.5 * f), RotY(0.002 * f));
        const M4 camMain = Mul(truck, T(0.4, 1.9, 1.2));
        const M4 camMirror = Mul(truck, Mul(T(-1.45, 2.0, 1.4), RotY(3.14159265358979)));
        // ---- world matrices ----
        std::vector<M4> world(nObj, I4());
        world[oGround] = T(0, 0, 100);
        for (int k = 0; k < 10; ++k) world[oBox[k]] = T((k & 1) ? 7.0 : -7.0, 0.0, -60.0 + 25.0 * k);
        for (int k = 0; k < 12; ++k) world[oClump[k]] = T(((k & 1) ? 5.5 : -5.5) + 0.3 * (k % 3), 0.0, -45.0 + 9.0 * k);
        world[oCab] = truck;
        world[oTrailer] = Mul(truck, T(0.0, 0.0, -8.5));
        const M4 car = T(-3.5, 0.0, -25.0 + 0.8 * f);
        world[oCar] = car;
        const double wheelPos[4][3] = { {-1.0, 0.45, 1.6}, {1.0, 0.45, 1.6}, {-1.0, 0.45, -1.6}, {1.0, 0.45, -1.6} };
        for (int k = 0; k < 4; ++k)
            world[oWheel[k]] = Mul(car, Mul(T(wheelPos[k][0], wheelPos[k][1], wheelPos[k][2]), RotX(0.8 / 0.45 * f)));
        // ---- per view: MVPs, draw order (clumps shuffled, 2 culled per view) ----
        U[0]->rendered = (f % skipEvery) != skipEvery - 1;     // the throttled mirror: not rendered every 37th frame
        U[1]->rendered = true;
        for (auto& up : U) { if (up->rendered) ++up->sinceSkip; else up->sinceSkip = 0; }
        std::vector<int> order[2];
        for (int vi = 0; vi < 2; ++vi) {
            Unit& u = *U[vi];
            M4 V; Inv(vi == 0 ? camMirror : camMain, V);
            const M4 P = ProjA(kNearWorld, (double)u.v.w / u.v.h);
            for (int o = 0; o < nObj; ++o)
                u.mvpCur[o] = objs[o].kind == kInstanced ? Mul(P, V) : Mul(P, Mul(V, world[o]));
            std::vector<int>& ord = order[vi];
            for (int o = 0; o < nObj; ++o) ord.push_back(o);
            std::vector<int> cl(oClump, oClump + 12);
            std::shuffle(cl.begin(), cl.end(), rng);
            { int ci = 0; for (int& o : ord) if (objs[o].group == 2) o = cl[ci++]; }
            std::vector<bool> culled(nObj, false);
            culled[cl[0]] = culled[cl[1]] = true;
            ord.erase(std::remove_if(ord.begin(), ord.end(), [&](int o) { return culled[o]; }), ord.end());
            for (int o = 0; o < nObj; ++o) u.drawnCur[o] = u.rendered && std::find(ord.begin(), ord.end(), o) != ord.end();
            for (int o = 0; o < nObj; ++o) u.drawnRun[o] = u.drawnCur[o] ? u.drawnRun[o] + 1 : (u.rendered ? 0 : u.drawnRun[o]);
        }
        // ---- ONE ring for the frame: main windows from 0, mirror windows from 1 MB ----
        ID3D11Buffer* ring = E.ring[f & 1].Get();
        {
            D3D11_MAPPED_SUBRESOURCE mp{};
            if (FAILED(E.ctx->Map(ring, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { printf("ring map failed\n"); return 1; }
            float* p = (float*)mp.pData;
            memset(p, 0, 2u << 20);
            for (int vi = 0; vi < 2; ++vi) {
                Unit& u = *U[vi];
                for (int d = 0; d < (int)order[vi].size(); ++d) {
                    const int o = order[vi][d];
                    float* w = p + (u.ringBase + (UINT)d * 256u) / 4;
                    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) w[(4 + r) * 4 + c] = (float)u.mvpCur[o].m[r][c];
                    w[8 * 4 + 0] = (float)(o + 1); w[8 * 4 + 1] = (float)(d + 1); w[8 * 4 + 2] = 0.0f;
                }
            }
            E.ctx->Unmap(ring, 0);
        }
        // ---- render: mirror view first (its DLAA unit runs at its own depth discard), then the main view ----
        auto halton = [](int i, int b) { double r = 0, fct = 1; while (i > 0) { fct /= b; r += fct * (i % b); i /= b; } return r; };
        CameraMv::FrameStats fsv[2];
        for (int vi = 0; vi < 2; ++vi) {
            Unit& u = *U[vi];
            if (!u.rendered) continue;
            ID3D11DeviceContext* ctx = E.ctx.Get();
            ctx->ClearDepthStencilView(u.v.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
            ctx->ClearRenderTargetView(u.v.truthRtv.Get(), zero);
            ctx->ClearRenderTargetView(u.v.colorRtv.Get(), zero);
            ID3D11RenderTargetView* rtvs[2] = { u.v.truthRtv.Get(), u.v.colorRtv.Get() };
            ctx->OMSetRenderTargets(2, rtvs, u.v.dsv.Get());
            ctx->OMSetDepthStencilState(E.dss.Get(), 1);
            // its own jitter: the unit's own Halton phase (inject.cpp: MirUnit::phaseCtr; the main pass: the FIFO's)
            const int ph = u.phase++ % 8;
            const float jx = (float)(halton(ph + 1, 2) - 0.5), jy = (float)(halton(ph + 1, 3) - 0.5);
            const D3D11_VIEWPORT vp = { jx, jy, (float)(u.v.w + g_vpOver), (float)(u.v.h + g_vpOver), 0.01f, 0.9f };
            ctx->RSSetScissorRects(1, &sc0big);
            ctx->PSSetShader(E.ps.Get(), nullptr, 0);
            u.rec.Reset();
            u.cand.Reset();
            u.cand.SetDropped(u.cam.MedoidDropBits());  // v0.10.0 phase 8 (mv_medoid_drop)
            for (int d = 0; d < (int)order[vi].size(); ++d) {
                const int o = order[vi][d];
                const Obj& ob = objs[o];
                const Mesh& m = E.meshes[ob.mesh];
                ctx->RSSetViewports(1, &vp);
                ctx->RSSetState(ob.cullNone ? E.rsNone.Get() : E.rsBack.Get());
                ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D11Buffer* vbs[2] = { E.vb[0].Get(), E.instVb[0].Get() };
                const UINT strides[2] = { (UINT)sizeof(Vtx), 12 }, offs[2] = { 0, 0 };
                ctx->IASetVertexBuffers(0, 2, vbs, strides, offs);
                ctx->IASetIndexBuffer(E.ib[0].Get(), DXGI_FORMAT_R32_UINT, 0);
                ID3D11ShaderResourceView* vsSrv[4] = {};
                ctx->VSSetShaderResources(0, 4, vsSrv);
                if (ob.vs == 2) { ctx->IASetInputLayout(E.ilInst.Get()); ctx->VSSetShader(E.vsInst.Get(), nullptr, 0); }
                else { ctx->IASetInputLayout(E.il.Get()); ctx->VSSetShader(E.vsMain.Get(), nullptr, 0); }
                const UINT first = (u.ringBase + (UINT)d * 256u) / 16u, num = 16u;
                E.ctx1->VSSetConstantBuffers1(0, 1, &ring, &first, &num);
                E.ctx1->PSSetConstantBuffers1(0, 1, &ring, &first, &num);
                const bool inst = ob.kind == kInstanced;
                if (!inst) {                            // inject.cpp: CollectMvCandidate into the unit's record
                    CandidateRecord::DrawKey k{};
                    k.ib = E.ib[0].Get(); k.vb = E.vb[0].Get(); k.indexCount = m.ic; k.startIndex = m.si; k.baseVertex = m.bv;
                    if (u.cand.Count(0) < CandidateRecord::kSlots && !u.cand.Dropped(0)) u.cand.Record(ctx, 0, ring, first * 16u + 64u, k);
                }
                u.rec.Record(E.ctx1.Get(), m.ic, inst ? (UINT)ob.instances : 1u, m.si, m.bv, 0u, inst);
                if (inst) ctx->DrawIndexedInstanced(m.ic, ob.instances, m.si, m.bv, 0);
                else ctx->DrawIndexed(m.ic, m.si, m.bv);
            }
            // G-buffer leave: replay into ITS id target against a read-only DSV of ITS depth (inject.cpp MirReplay / DidReplay)
            const auto rr = u.rec.Replay(E.ctx1.Get(), u.v.idRtv.Get(), u.v.roDsv.Get(), false, false);
            ++checks;
            if (!rr.ok || rr.replayed <= 0) { ++fails; printf("   FAIL %s frame %d: replay ok=%d replayed=%d\n", u.name, f, (int)rr.ok, rr.replayed); }
            ID3D11RenderTargetView* nullRtv = nullptr;
            ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
            // its depth discard: snapshot + MV generation of ITS unit (inject.cpp MirEndView -> MirFlush -> SceneDlaa::Run)
            ctx->CopyResource(u.v.twin.Get(), u.v.depth.Get());
            const bool ok = u.cam.Generate(ctx, u.v.w, u.v.h, 0, 0, u.v.w, u.v.h, u.cand, u.v.twinSrv.Get(), u.v.dUav.Get(),
                                           u.v.mvUav.Get(), &fsv[vi], false, nullptr, nullptr, nullptr, &u.rec,
                                           u.rec.ReplayFailed() ? nullptr : u.v.idSrv.Get());
            ++checks;
            if (!ok) { ++fails; printf("   FAIL %s Generate (frame %d)\n", u.name, f); }
            if (u.frames >= 2 && fsv[vi].worldMiss && !fsv[vi].missHeld) ++u.resets;
            if (u.frames >= 1) { ++checks; if (u.cam.DidN() == 0) { ++fails; printf("   FAIL %s frame %d: pass B without draw ids\n", u.name, f); } }
        }
        // ---- checks per rendered view ----
        for (int vi = 0; vi < 2; ++vi) {
            Unit& u = *U[vi];
            if (!u.rendered) continue;
            ID3D11DeviceContext* ctx = E.ctx.Get();
            const int VW = u.v.w, VH = u.v.h;
            const int nd = (int)order[vi].size();
            ctx->CopyResource(u.v.stMv.Get(), u.v.mv.Get());
            ctx->CopyResource(u.v.stDepth.Get(), u.v.dOut.Get());
            ctx->CopyResource(u.v.stTruth.Get(), u.v.truth.Get());
            ctx->CopyResource(u.v.stId.Get(), u.v.idTex.Get());
            if (u.cam.DidRSrv()) {
                ComPtr<ID3D11Resource> rres;
                u.cam.DidRSrv()->GetResource(&rres);
                const D3D11_BOX box{ 0, 0, 0, (UINT)nd * DrawIdMv::kRStride * 16u, 1, 1 };
                ctx->CopySubresourceRegion(E.stR.Get(), 0, 0, 0, 0, rres.Get(), 0, &box);
            }
            float sv[CameraMv::kSolveFloats];
            if (!u.cam.ReadSolveBlocking(ctx, sv)) { ++fails; printf("   FAIL %s solve readback\n", u.name); }
            M4 Rcam;
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) Rcam.m[r][c] = sv[r * 4 + c];
            D3D11_MAPPED_SUBRESOURCE mMv{}, mD{}, mT{}, mI{}, mR{};
            ctx->Map(u.v.stMv.Get(), 0, D3D11_MAP_READ, 0, &mMv);
            CmpFrame(mMv, (int)u.v.w, (int)u.v.h);      // v0.10.0 phase 8 comparison mode
            ctx->Map(u.v.stDepth.Get(), 0, D3D11_MAP_READ, 0, &mD);
            ctx->Map(u.v.stTruth.Get(), 0, D3D11_MAP_READ, 0, &mT);
            ctx->Map(u.v.stId.Get(), 0, D3D11_MAP_READ, 0, &mI);
            ctx->Map(E.stR.Get(), 0, D3D11_MAP_READ, 0, &mR);
            const float* Rt = (const float*)mR.pData;
            const bool hist0 = u.frames >= 1;           // a previous RENDERED pass of this unit exists
            for (int o = 0; o < nObj; ++o) u.stCur[o] = -1;
            int paired = 0, elig = 0;
            std::vector<char> parMark(nObj, 0);             // v0.10.0 phase 6: took a rigid parent's R / state
            for (int d = 0; d < nd; ++d) {
                const int o = order[vi][d];
                const int st = (int)(Rt[(d * 5 + 4) * 4] + 0.5f);
                u.stCur[o] = st;
                parMark[o] = ParentMarked(Rt[(d * 5 + 4) * 4 + 3]) ? 1 : 0;
                if (objs[o].kind != kInstanced) { ++elig; if (st == 2 || st == 3) ++paired; }
            }
            if (hist0) { u.pairedSum += (uint64_t)paired; u.eligSum += (uint64_t)elig; }
            std::vector<int> wantCur(nObj, -1), wantAlt(nObj, -1);
            for (int o = 0; o < nObj; ++o) {
                if (!u.drawnCur[o]) continue;
                const Kind k = objs[o].kind;
                const bool hist = hist0 && u.drawnPrev[o];
                int want = -1, alt = -1;
                double tdev = 0.0;
                if (k != kInstanced && (hist || (hist0 && parMark[o]))) {
                    const M4 P = Mul(Rcam, u.mvpCur[o]);
                    const double probes[6][4] = { {0,0,0,1}, {4,0,0,1}, {-4,0,0,1}, {0,0,4,1}, {0,0,-4,1}, {0,4,0,1} };
                    for (auto& q : probes) {
                        double a[4], b[4];
                        Xf(u.mvpPrev[o], q, a); Xf(P, q, b);
                        double dv;
                        const bool on = a[3] > 0.05 && b[3] > 0.05 && std::fabs(a[0] / a[3]) <= 1.5 && std::fabs(a[1] / a[3]) <= 1.5 &&
                                        std::fabs(b[0] / b[3]) <= 1.5 && std::fabs(b[1] / b[3]) <= 1.5;
                        if (on) dv = std::hypot((a[0] / a[3] - b[0] / b[3]) * VW * 0.5, (a[1] / a[3] - b[1] / b[3]) * VH * 0.5);
                        else dv = std::hypot((a[0] - b[0]) * VW * 0.5, (a[1] - b[1]) * VH * 0.5) / std::max(std::max(std::fabs(a[3]), std::fabs(b[3])), 1.0);
                        tdev = std::max(tdev, dv);
                    }
                }
                if (k == kInstanced) want = DrawIdMv::kStInstanced;
                else if (!hist) want = DrawIdMv::kStUnpaired;
                else if (tdev >= snapPx * 1.5 + 0.02) want = DrawIdMv::kStMover;
                else if (tdev <= snapPx * 0.5) want = DrawIdMv::kStStatic;
                else { want = DrawIdMv::kStStatic; alt = DrawIdMv::kStMover; }
                if (dupGroup(o) && u.drawnRun[o] == 2) {
                    if (want == DrawIdMv::kStMover) want = DrawIdMv::kStUnpaired;
                    else if (alt == DrawIdMv::kStMover) alt = DrawIdMv::kStUnpaired;
                }
                // the first two passes after a skipped mirror frame: an identical copy's displacement vs its previous one is
                // not consistent (2 frames of motion, then 1) -> the track rule keeps it on the camera R (unpaired) there
                if (dupGroup(o) && u.sinceSkip <= 2 && want == DrawIdMv::kStMover && alt < 0) alt = DrawIdMv::kStUnpaired;
                if (hist0 && parMark[o] && want == DrawIdMv::kStUnpaired) ParentWant(tdev, snapPx, &want, &alt);   // phase 6
                wantCur[o] = want; wantAlt[o] = alt;
                ++checks;
                if (u.stCur[o] != want && u.stCur[o] != alt) {
                    ++u.wrongState[o]; ++fails;
                    if (u.wrongState[o] <= 3)
                        printf("   FAIL %s frame %d: %s #%d state %d, want %d (alt %d), true dev %.3f px\n", u.name, f, objs[o].name,
                               o, u.stCur[o], want, alt, tdev);
                }
                if (u.drawnPrev[o] && u.stPrev[o] >= 0 && u.stCur[o] != u.stPrev[o] && want == u.wantPrev[o] && alt < 0 &&
                    u.wantAltPrev[o] < 0 && k != kInstanced && u.renderedPrev) ++u.flick[o];
            }
            // the own trailer in the mirror: a mover whose own R is the identity (it rides with the camera)
            if (vi == 0 && hist0 && u.drawnCur[oTrailer] && u.drawnPrev[oTrailer]) {
                ++u.trailerFrames;
                if (u.stCur[oTrailer] == DrawIdMv::kStMover) ++u.trailerMoverFrames;
            }
            // expected R per object: a mover's own true R (vs its last RENDERED pass), every other draw the GPU camera R
            std::vector<M4> Rexp(nObj, I4());
            for (int o = 0; o < nObj; ++o) {
                if (objs[o].kind != kInstanced && hist0 && (u.drawnPrev[o] || parMark[o]) && u.stCur[o] == DrawIdMv::kStMover) {
                    M4 ic; Inv(u.mvpCur[o], ic);
                    Rexp[o] = Mul(u.mvpPrev[o], ic);
                } else Rexp[o] = Rcam;
            }
            std::vector<int> pxOk(nObj, 0), pxAll(nObj, 0);
            const double tolStatic = 0.03, tolMover = 0.03 + snapPx;
            for (int y = 0; y < VH; ++y) {
                const uint16_t* mvRow = (const uint16_t*)((const uint8_t*)mMv.pData + y * mMv.RowPitch);
                const float* dRow = (const float*)((const uint8_t*)mD.pData + y * mD.RowPitch);
                const uint32_t* tRow = (const uint32_t*)((const uint8_t*)mT.pData + y * mT.RowPitch);
                const uint16_t* iRow = (const uint16_t*)((const uint8_t*)mI.pData + y * mI.RowPitch);
                for (int x = 0; x < VW; ++x) {
                    const uint32_t tObj = tRow[x * 2], tDraw = tRow[x * 2 + 1];
                    if (!tObj) continue;
                    const int o = (int)tObj - 1;
                    const uint32_t wantId = objs[o].kind != kInstanced ? tDraw : 0u;
                    ++u.idChecked;
                    if (iRow[x] != wantId) {
                        ++u.idMismatch; ++u.idBad[o];
                        if (u.idMismatch <= 5) printf("   FAIL %s id at (%d,%d) frame %d: %u, want %u (%s)\n", u.name, x, y, f, iRow[x], wantId, objs[o].name);
                    }
                    const double dd = dRow[x];
                    const double z = (dd - 0.01) / 0.89;
                    const double uu = (x + 0.5) / VW, vv = (y + 0.5) / VH;
                    const double p4[4] = { uu * 2 - 1, 1 - vv * 2, std::min(1.0, std::max(0.0, z)), 1.0 };
                    double c[4];
                    Xf(Rexp[o], p4, c);
                    double ex = 0, ey = 0;
                    if (c[3] > 1e-6) { ex = ((c[0] / c[3]) * 0.5 + 0.5 - uu) * VW; ey = ((0.5 - (c[1] / c[3]) * 0.5) - vv) * VH; }
                    const double gx = H2F(mvRow[x * 2]), gy = H2F(mvRow[x * 2 + 1]);
                    const double err = std::sqrt((gx - ex) * (gx - ex) + (gy - ey) * (gy - ey));
                    const bool mover = objs[o].kind == kMover;
                    const double tol = (mover ? tolMover : tolStatic) + std::max(std::fabs(ex), std::fabs(ey)) / 1024.0;
                    ++u.mvChecked; ++pxAll[o];
                    const int g = objs[o].group;
                    if (err > u.worstMv[g]) u.worstMv[g] = err;
                    if (err <= tol) ++pxOk[o]; else ++u.mvFails;
                    if (vi == 0 && o == oTrailer && hist0 && u.drawnPrev[o]) {
                        // the counterfactual: what the camera R alone would give this pixel (the v0.9.0 / camera-only path)
                        double cc[4];
                        Xf(Rcam, p4, cc);
                        if (cc[3] > 1e-6) {
                            const double cx = ((cc[0] / cc[3]) * 0.5 + 0.5 - uu) * VW, cy = ((0.5 - (cc[1] / cc[3]) * 0.5) - vv) * VH;
                            u.trailerCamMvSum += std::sqrt(cx * cx + cy * cy); ++u.trailerCamPxSum;
                        }
                        ++u.trailerPx;
                    }
                }
            }
            for (int o = 0; o < nObj; ++o) {
                if (!pxAll[o]) continue;
                ++checks;
                if (pxOk[o] * 1000 < pxAll[o] * 995) {
                    ++fails; ++u.objFramesBad[o];
                    if (u.objFramesBad[o] <= 2)
                        printf("   FAIL %s frame %d: %s #%d MV right on %d of %d px (state %d)\n", u.name, f, objs[o].name, o,
                               pxOk[o], pxAll[o], u.stCur[o]);
                }
            }
            ctx->Unmap(u.v.stMv.Get(), 0); ctx->Unmap(u.v.stDepth.Get(), 0); ctx->Unmap(u.v.stTruth.Get(), 0);
            ctx->Unmap(u.v.stId.Get(), 0); ctx->Unmap(E.stR.Get(), 0);
            if (u.frames == 0) {
                printf("   %s frame 0 coverage: cab %d px, trailer %d px, car %d px, wheel %d px, box0 %d px, ground %d px\n", u.name,
                       pxAll[oCab], pxAll[oTrailer], pxAll[oCar], pxAll[oWheel[0]], pxAll[oBox[0]], pxAll[oGround]);
            }
            // commit this rendered pass as the unit's previous one
            for (int o = 0; o < nObj; ++o) {
                u.mvpPrev[o] = u.mvpCur[o]; u.drawnPrev[o] = u.drawnCur[o]; u.stPrev[o] = u.drawnCur[o] ? u.stCur[o] : -1;
                u.wantPrev[o] = wantCur[o]; u.wantAltPrev[o] = wantAlt[o];
            }
            ++u.frames;
        }
        for (auto& up : U) up->renderedPrev = up->rendered;
        if (g_debugLayer) { const int dl = DrainDebugLayer("S5"); ++checks; if (dl) ++fails; }
    }
    for (int vi = 0; vi < 2; ++vi) {
        Unit& u = *U[vi];
        checks += 2;
        if (u.idMismatch) { ++fails; }
        if (u.resets) { ++fails; printf("   FAIL %s history resets after its frame 1: %d\n", u.name, u.resets); }
        int flickTot = 0;
        for (int o = 0; o < nObj; ++o) flickTot += u.flick[o];
        ++checks;
        if (flickTot) { ++fails; printf("   FAIL %s draw state flicker: %d\n", u.name, flickTot); }
        printf("   %s: %d rendered frames; id ownership %d of %d owned pixels wrong; MV %d of %d pixels outside tolerance; "
               "paired %.1f %% of the non-instanced draws; flicker %d; resets %d\n", u.name, u.frames, u.idMismatch, u.idChecked,
               u.mvFails, u.mvChecked, u.eligSum ? 100.0 * (double)u.pairedSum / (double)u.eligSum : 0.0, flickTot, u.resets);
        printf("   %s worst MV error per group (px): ground %.3f box %.3f clump %.3f cab %.3f trailer %.3f car %.3f wheel %.3f inst %.3f\n",
               u.name, u.worstMv[0], u.worstMv[1], u.worstMv[2], u.worstMv[3], u.worstMv[4], u.worstMv[5], u.worstMv[6], u.worstMv[7]);
        u.rec.Shutdown();
    }
    {   // the mirror's own trailer: a mover (R = I) in every frame with history, visible, and the camera R would be wrong there
        Unit& m = *U[0];
        const double camMv = m.trailerCamPxSum ? m.trailerCamMvSum / (double)m.trailerCamPxSum : 0.0;
        checks += 3;
        if (m.trailerFrames < 60 || m.trailerMoverFrames != m.trailerFrames) {
            ++fails; printf("   FAIL mirror: trailer mover frames %d of %d\n", m.trailerMoverFrames, m.trailerFrames);
        }
        if (m.trailerPx < 100000) { ++fails; printf("   FAIL mirror: the trailer covers only %lld px over the scene\n", m.trailerPx); }
        if (!(camMv > 2.0)) { ++fails; printf("   FAIL mirror: the camera R would move the trailer by only %.2f px (scene does not exercise it)\n", camMv); }
        printf("   mirror trailer: mover (own R = identity) in %d of %d frames with history, %lld px over the scene; the camera R "
               "alone would move those pixels by %.2f px on average (= the ghosting per-draw R removes)\n", m.trailerMoverFrames,
               m.trailerFrames, m.trailerPx, camMv);
        const DrawIdMv::Stats& ds = m.cam.DrawIds().GetStats();
        PrintPhase8(E.ctx.Get(), ds, m.name);
        printf("   mirror DrawIdMv: %llu frames, %llu draws, %llu via the pointer-free key, %llu without a partner group\n",
               (unsigned long long)ds.frames, (unsigned long long)ds.draws, (unsigned long long)ds.looseDraws,
               (unsigned long long)ds.noGroup);
        printf("   %s camera R consensus: world %llu frames / medoid fallback %llu (cluster avg %.1f draws)\n", m.name,
               (unsigned long long)ds.consFrames[0], (unsigned long long)ds.consFallback[0],
               ds.consFrames[0] ? (double)ds.consCluster[0] / (double)ds.consFrames[0] : 0.0);
    }
    printf("   %s: %d / %d checks passed\n", fails ? "FAILED" : "passed", checks - fails, checks);
    tot.checks += checks; tot.fails += fails;
    return fails;
}

}  // namespace

// ---- S6 (v0.10.0 phase 3): CONSENSUS camera R + FORWARD-pass draw ids -------------------------------------------------
// The in-game failure: pass A's medoid (16 sampled candidate pairs, unique meshes first) picked a MOVING vehicle as the
// camera reference for seconds -> the camera R was off by 1-2 px and every pixel that uses it smeared. S6 builds exactly that
// trap: 12 unique cars and a convoy of 30 identical trucks all move with ONE velocity (0.06, 0, 2.0) m / frame (so every one
// of them has the same R_draw), while the static scene is ground, 3 boxes, 50 identical posts and 30 identical clumps. The
// medoid's unique-first sample is mostly cars -> wrong. The consensus (largest cluster of agreeing per-draw R's, far voters)
// must pick the static cluster: the solve buffer's camera R is checked against the exact camera R in pixels (grid of ndc
// points at view depths >= 5 m, every frame with history, <= 0.05 px).
// Forward pass (after the G-buffer replay, as the game: 1 RTV + the scene depth tested, no depth write, "over" blending):
// a licence PLATE on the moving vehicle's matrix (an alpha band < 0.5 in the middle: no forward depth there), a GLASS pane
// with alpha 0.3 (never any forward depth), a static WIRE gantry (no depth write). Each is re-drawn into a forward-depth
// target (GREATER_EQUAL, alpha-to-coverage, colour mask 0) and recorded into a forward-mode DrawIdRecord, replayed at the
// "depth discard" into the GREEN channel of the R16G16_UINT id target -- the inject.cpp path. Checks: exact id ownership in
// both channels (G against a truth pass with discard alpha < 0.5, last-writer-wins), the MV of every pixel against its
// owner (the forward owner where the forward depth won), the plate's pixels move exactly with the body, draw states.
struct S6Res {
    ComPtr<ID3D11Texture2D> id2, stId2, fwd, stFwd, fTruth, stFTruth, fTruthDepth, stTwin, scratch;
    ComPtr<ID3D11RenderTargetView> id2Rtv, fTruthRtv, scratchRtv;
    ComPtr<ID3D11ShaderResourceView> id2Srv, fwdSrv;
    ComPtr<ID3D11DepthStencilView> fwdDsv, fTruthDsv;
    ComPtr<ID3D11DepthStencilState> dssFwdGame, dssFwdRe;
    ComPtr<ID3D11BlendState> bsOver, bsA2C, bsA2CAll;
    ComPtr<ID3D11PixelShader> psFwd, psFwdTruth;
};

// ---- v0.10.0 phase 9: a SHADOW pipeline next to a scene's main (phase-8) pipeline. The same draws are recorded into its own
// records and replayed with the DLAA-area clip (DrawIdRecord::SetView, as inject.cpp's DidReplay / DidFwdReplay); the forward
// draws are re-drawn into its own forward depth exactly like inject.cpp's FwdOnDraw (the clip through a scissor-enabled RS copy;
// shift 1 = a 1/2-size target, viewport / scissor halved, a dummy RT0 of that size, the forward ids in their own R16_UINT target);
// its own CameraMv runs pass B over the CROP only (as SceneDlaa::Run at dlaa_area < 100). Compared with the main pipeline every
// frame: ids inside the clip identical, outside 0; MVs inside the crop within 0.07 px of the main pipeline (shift 1: pixels the
// forward depth touches in either pipeline are counted separately -- the 1/2-resolution trade-off); per-draw states.
// S11 = S6 (convoy, forward wire / plate / glass) and S12 = S8 (rigid parents, vote) with two shadows each: full / 1/2 resolution.
// NOT covered: inject.cpp itself (PassViewSetup's rect for VR eyes, the game's own RS states) -- in-game log only.
int g_shadowMut = 0;   // v0.10.0 phase 9 harness mutations (must FAIL S11 / S12): 1 "mutclip" = the G-buffer replay without its clip,
                       // 2 "mutview" = the 1/2-resolution forward re-draw without the halved viewport (its replay keeps it)
// v0.10.0 phase 10: a BATCH shadow models the new inject.cpp path -- the forward draws are only RECORDED (with their pixel-shader
// state, DrawIdRecord::SetCapturePs) and re-drawn in one go after the depth snapshot (FwdBatch: clear, optional occlusion init
// from the snapshot = DrawIdRecord::InitFwdDepth, ReplayDepth, then the forward-id replay). Without the init (cull 0) it must be
// IDENTICAL to the main (interleaved) pipeline; with the init (cull 1) the forward depth differs only where a forward fragment
// lies behind the scene, so RED ids and every MV must still be identical and GREEN ids where the forward depth won.
struct Shadow {
    const char* name = "";
    bool batch = false, cull = false;                   // v0.10.0 phase 10
    ID3D11DepthStencilState* dssRe = nullptr;           //   the re-draw states, taken from the scene at the first forward draw
    ID3D11BlendState* bsA2C = nullptr;
    long long cullInit = 0, gSkipNoWin = 0, gDiffNoWin = 0;
    UINT shift = 0, W = 0, H = 0, fw = 0, fh = 0;
    D3D11_RECT clip{};
    UINT cx = 0, cy = 0, cw = 0, ch = 0;
    ComPtr<ID3D11Texture2D> id, fwd, fwdId, dummy, mv, dOut, stId, stFwd, stFwdId, stMv, stMainId;
    ComPtr<ID3D11RenderTargetView> idRtv, fwdIdRtv, dummyRtv;
    ComPtr<ID3D11ShaderResourceView> idSrv, fwdSrv, fwdIdSrv;
    ComPtr<ID3D11DepthStencilView> fwdDsv;
    ComPtr<ID3D11UnorderedAccessView> mvUav, dUav;
    ComPtr<ID3D11Buffer> stR;
    DrawIdRecord rec, fwdRec;
    CameraMv cam;
    CandidateRecord cand;
    bool fwdCleared = false;
    long long idPx = 0, idBadIn = 0, idBadOut = 0, gPx = 0, gBadIn = 0, gBadOut = 0, hfPx = 0, hfNoId = 0, hfOut = 0;
    long long mvPx = 0, mvOver = 0, fPx = 0, fWithin007 = 0, fWithin025 = 0, fOver1 = 0, stateCmp = 0, stateDiff = 0;
    long long clipPx = 0;
    double mvMax = 0.0, fMax = 0.0;
    int frames = 0, badFrames = 0, edgeFrames = 0, genFails = 0, replayFails = 0;

    bool Init(Env& E, UINT w, UINT h, int areaPct, int margin, UINT sh, const char* nm, float cu = 0.5f, float cv = 0.5f,
              bool batchMode = false, bool cullMode = false) {
        ID3D11Device* d = E.dev.Get();
        name = nm; shift = sh; W = w; H = h;
        batch = batchMode; cull = batchMode && cullMode;
        fw = (w + (1u << sh) - 1u) >> sh; fh = (h + (1u << sh) - 1u) >> sh;
        uint32_t x = 0, y = 0, cww = 0, chh = 0;
        SceneDlaa::CropRect(w, h, areaPct, cu, cv, &x, &y, &cww, &chh);   // (cu, cv): the eye's optical centre
        cx = x; cy = y; cw = cww; ch = chh;
        clip.left = (LONG)cx - margin; clip.top = (LONG)cy - margin;
        clip.right = (LONG)(cx + cw) + margin; clip.bottom = (LONG)(cy + ch) + margin;
        if (clip.left < 0) clip.left = 0;
        if (clip.top < 0) clip.top = 0;
        if (clip.right > (LONG)w) clip.right = (LONG)w;
        if (clip.bottom > (LONG)h) clip.bottom = (LONG)h;
        const DXGI_FORMAT idf = sh ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R16G16_UINT;
        bool ok = MakeTex(d, w, h, idf, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &id) &&
                  SUCCEEDED(d->CreateRenderTargetView(id.Get(), nullptr, &idRtv)) &&
                  SUCCEEDED(d->CreateShaderResourceView(id.Get(), nullptr, &idSrv)) &&
                  MakeTex(d, w, h, idf, 0, &stId, true) &&
                  MakeTex(d, w, h, DXGI_FORMAT_R16G16_UINT, 0, &stMainId, true) &&
                  MakeTex(d, fw, fh, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, &fwd) &&
                  MakeTex(d, fw, fh, DXGI_FORMAT_R32_TYPELESS, 0, &stFwd, true) &&
                  MakeTex(d, cw, ch, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &mv) &&
                  SUCCEEDED(d->CreateUnorderedAccessView(mv.Get(), nullptr, &mvUav)) &&
                  MakeTex(d, cw, ch, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &dOut) &&
                  SUCCEEDED(d->CreateUnorderedAccessView(dOut.Get(), nullptr, &dUav)) &&
                  MakeTex(d, cw, ch, DXGI_FORMAT_R16G16_FLOAT, 0, &stMv, true);
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{}; dd.Format = DXGI_FORMAT_D32_FLOAT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        ok = ok && SUCCEEDED(d->CreateDepthStencilView(fwd.Get(), &dd, &fwdDsv)) &&
             SUCCEEDED(d->CreateShaderResourceView(fwd.Get(), &sd, &fwdSrv));
        if (ok && sh) {
            ok = MakeTex(d, fw, fh, DXGI_FORMAT_R16_UINT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &fwdId) &&
                 SUCCEEDED(d->CreateRenderTargetView(fwdId.Get(), nullptr, &fwdIdRtv)) &&
                 SUCCEEDED(d->CreateShaderResourceView(fwdId.Get(), nullptr, &fwdIdSrv)) &&
                 MakeTex(d, fw, fh, DXGI_FORMAT_R16_UINT, 0, &stFwdId, true) &&
                 MakeTex(d, fw, fh, DXGI_FORMAT_R8_UNORM, D3D11_BIND_RENDER_TARGET, &dummy) &&
                 SUCCEEDED(d->CreateRenderTargetView(dummy.Get(), nullptr, &dummyRtv));
        }
        if (ok && !sh && batch)                          // v0.10.0 phase 10: the batch's RT0 at full resolution too
            ok = MakeTex(d, fw, fh, DXGI_FORMAT_R8_UNORM, D3D11_BIND_RENDER_TARGET, &dummy) &&
                 SUCCEEDED(d->CreateRenderTargetView(dummy.Get(), nullptr, &dummyRtv));
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (UINT)DrawIdMv::kMax * DrawIdMv::kRStride * 16u; bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ok = ok && SUCCEEDED(d->CreateBuffer(&bd, nullptr, &stR));
        ok = ok && cam.Init(d);
        if (!ok) { printf("   shadow %s: resources failed\n", nm); return false; }
        cam.SetEgoPixel(0.0f);
        cam.SetEgoOrigin(0.0f);
        cand.Init(d);
        fwdRec.SetForward(true);
        fwdRec.SetCapturePs(batch);                      // v0.10.0 phase 10
        return true;
    }
    void Shutdown() { rec.Shutdown(); fwdRec.Shutdown(); }
    void BeginFrame() {
        rec.Reset(); fwdRec.Reset(); cand.Reset();
        cand.SetDropped(cam.MedoidDropBits());
        fwdCleared = false;
    }
    void OnGbufDraw(ID3D11DeviceContext* ctx, ID3D11DeviceContext1* ctx1, ID3D11Buffer* ring, int d,
                    const CandidateRecord::DrawKey* key, UINT ic, UINT inst, UINT si, INT bv, bool instanced) {
        if (key && cand.Count(0) < CandidateRecord::kSlots && !cand.Dropped(0)) cand.Record(ctx, 0, ring, (UINT)d * 256u + 64u, *key);
        rec.Record(ctx1, ic, inst, si, bv, 0u, instanced);
    }
    void AfterGbuf(ID3D11DeviceContext1* ctx1, ID3D11DepthStencilView* roDsv, bool replayAll) {
        rec.SetView(g_shadowMut == 1 ? nullptr : &clip, 0);
        if (!rec.Replay(ctx1, idRtv.Get(), roDsv, false, replayAll).ok) ++replayFails;
    }
    // inject.cpp FwdOnDraw's re-draw (v0.10.0 phase 9 view) with the game's forward state bound; then DidFwdOnDraw's record
    void OnFwdDraw(ID3D11DeviceContext* ctx, ID3D11DeviceContext1* ctx1, ID3D11DepthStencilState* dssRe, ID3D11BlendState* bsA2C,
                   UINT ic, UINT si, INT bv) {
        if (batch) {                                     // v0.10.0 phase 10 (inject.cpp FwdOnDraw -> DidFwdOnDraw): record only
            this->dssRe = dssRe; this->bsA2C = bsA2C;
            fwdRec.Record(ctx1, ic, 1, si, bv, 0, false);
            return;
        }
        ComPtr<ID3D11Device> dev;
        ctx->GetDevice(&dev);
        ID3D11RenderTargetView* grtv = nullptr; ID3D11DepthStencilView* gdsv = nullptr;
        ctx->OMGetRenderTargets(1, &grtv, &gdsv);
        ID3D11DepthStencilState* gdss = nullptr; UINT gref = 0;
        ctx->OMGetDepthStencilState(&gdss, &gref);
        ID3D11BlendState* gbs = nullptr; FLOAT gbf[4] = {}; UINT gmask = 0;
        ctx->OMGetBlendState(&gbs, gbf, &gmask);
        if (!fwdCleared) {
            ctx->ClearDepthStencilView(fwdDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
            if (shift) { const FLOAT z[4] = { 0, 0, 0, 0 }; ctx->ClearRenderTargetView(fwdIdRtv.Get(), z); }
            fwdCleared = true;
        }
        ID3D11RasterizerState* grs = nullptr;
        D3D11_VIEWPORT gvp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        D3D11_RECT gsc[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        UINT gnv = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE, gns = gnv;
        ctx->RSGetState(&grs);
        ctx->RSGetViewports(&gnv, gvp);
        ctx->RSGetScissorRects(&gns, gsc);
        bool had = false;
        ID3D11RasterizerState* rsS = DrawIdRecord::ScissorRs(dev.Get(), grs, &had);
        const UINT nv = gnv < (UINT)DrawIdRecord::kVp ? gnv : (UINT)DrawIdRecord::kVp;
        const UINT ns = gns < (UINT)DrawIdRecord::kSc ? gns : (UINT)DrawIdRecord::kSc;
        D3D11_VIEWPORT vp[DrawIdRecord::kVp];
        memcpy(vp, gvp, sizeof(D3D11_VIEWPORT) * nv);
        if (g_shadowMut != 2) DrawIdRecord::ScaleViewport(vp, nv, shift);
        ctx->RSSetViewports(nv, nv ? vp : nullptr);
        if (rsS || had) {
            D3D11_RECT sc[DrawIdRecord::kVp] = {};
            const UINT nsc = nv ? nv : 1u;
            for (UINT k = 0; k < nsc; ++k)
                DrawIdRecord::ScaledScissor(had, k < ns ? &gsc[k] : nullptr, rsS ? &clip : nullptr, shift, &sc[k]);
            ctx->RSSetScissorRects(nsc, sc);
        }
        if (rsS) ctx->RSSetState(rsS);
        ID3D11RenderTargetView* rt0 = shift ? dummyRtv.Get() : grtv;
        ctx->OMSetRenderTargets(1, &rt0, fwdDsv.Get());
        ctx->OMSetDepthStencilState(dssRe, 0);
        ctx->OMSetBlendState(bsA2C, nullptr, 0xFFFFFFFFu);
        ctx->DrawIndexed(ic, si, bv);
        ctx->OMSetRenderTargets(1, &grtv, gdsv);
        ctx->OMSetDepthStencilState(gdss, gref);
        ctx->OMSetBlendState(gbs, gbf, gmask);
        ctx->RSSetState(grs);
        ctx->RSSetViewports(gnv, gnv ? gvp : nullptr);
        ctx->RSSetScissorRects(gns, gns ? gsc : nullptr);
        if (grs) grs->Release();
        if (grtv) grtv->Release();
        if (gdsv) gdsv->Release();
        if (gdss) gdss->Release();
        if (gbs) gbs->Release();
        fwdRec.Record(ctx1, ic, 1, si, bv, 0, false);
    }
    // v0.10.0 phase 10 (inject.cpp FwdBatch, after the snapshot): clear, occlusion init, the batched re-draw, the forward ids
    void AfterTwin(ID3D11DeviceContext* ctx, ID3D11DeviceContext1* ctx1, ID3D11ShaderResourceView* twinSrv, UINT nG, int nf) {
        if (!batch || !nf) return;
        ctx->ClearDepthStencilView(fwdDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        if (shift) { const FLOAT z[4] = { 0, 0, 0, 0 }; ctx->ClearRenderTargetView(fwdIdRtv.Get(), z); }
        if (cull) {
            if (DrawIdRecord::InitFwdDepth(ctx1, fwdDsv.Get(), fw, fh, twinSrv, shift, &clip)) ++cullInit;
            else ++replayFails;
        }
        fwdRec.SetView(&clip, shift);
        const auto dr = fwdRec.ReplayDepth(ctx1, dummyRtv.Get(), fwdDsv.Get(), dssRe, bsA2C, true);
        if (!dr.ok || dr.replayed != nf) ++replayFails;
        const auto fr = fwdRec.ReplayForward(ctx1, shift ? fwdIdRtv.Get() : idRtv.Get(), fwdDsv.Get(), true, nG);
        if (!fr.ok || fr.replayed != nf) ++replayFails;
    }
    void AfterFwd(ID3D11DeviceContext1* ctx1, UINT nG, int nf) {
        if (!nf || batch) return;
        fwdRec.SetView(&clip, shift);
        const auto fr = fwdRec.ReplayForward(ctx1, shift ? fwdIdRtv.Get() : idRtv.Get(), fwdDsv.Get(), true, nG);
        if (!fr.ok || fr.replayed != nf) ++replayFails;
    }
    bool Generate(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* twinSrv, bool useFwd) {
        CameraMv::FrameStats fs;
        const bool ok = cam.Generate(ctx, cw, ch, cx, cy, W, H, cand, twinSrv, dUav.Get(), mvUav.Get(), &fs, false, nullptr, nullptr,
                                     fwdSrv.Get(), &rec, idSrv.Get(), useFwd ? &fwdRec : nullptr, shift,
                                     shift ? fwdIdSrv.Get() : nullptr);
        if (!ok) ++genFails;
        return ok;
    }
    // main*: the main pipeline's mapped MV (W x H R16G16_FLOAT), forward depth (R32), depth twin (R32G8X24), truth (R32G32_UINT),
    // its R table (nTab draws) and its id target (GPU, R16G16_UINT); mover[o] = object o is a mover (truth id o + 1)
    int Compare(ID3D11DeviceContext* ctx, int f, const D3D11_MAPPED_SUBRESOURCE& mMv, const D3D11_MAPPED_SUBRESOURCE& mF,
                const D3D11_MAPPED_SUBRESOURCE& mTw, const D3D11_MAPPED_SUBRESOURCE& mT, const float* Rt, int nTab,
                ID3D11Texture2D* mainId2, const std::vector<char>& mover) {
        ++frames;
        ctx->CopyResource(stId.Get(), id.Get());
        ctx->CopyResource(stMainId.Get(), mainId2);
        ctx->CopyResource(stFwd.Get(), fwd.Get());
        if (shift) ctx->CopyResource(stFwdId.Get(), fwdId.Get());
        ctx->CopyResource(stMv.Get(), mv.Get());
        const int nS = (int)cam.DidN();
        if (cam.DidRSrv() && nS) {
            ComPtr<ID3D11Resource> rres;
            cam.DidRSrv()->GetResource(&rres);
            const D3D11_BOX box{ 0, 0, 0, (UINT)nS * DrawIdMv::kRStride * 16u, 1, 1 };
            ctx->CopySubresourceRegion(stR.Get(), 0, 0, 0, 0, rres.Get(), 0, &box);
        }
        D3D11_MAPPED_SUBRESOURCE sI{}, sM{}, sF{}, sFI{}, sV{}, sR{};
        ctx->Map(stId.Get(), 0, D3D11_MAP_READ, 0, &sI);
        ctx->Map(stMainId.Get(), 0, D3D11_MAP_READ, 0, &sM);
        ctx->Map(stFwd.Get(), 0, D3D11_MAP_READ, 0, &sF);
        if (shift) ctx->Map(stFwdId.Get(), 0, D3D11_MAP_READ, 0, &sFI);
        ctx->Map(stMv.Get(), 0, D3D11_MAP_READ, 0, &sV);
        ctx->Map(stR.Get(), 0, D3D11_MAP_READ, 0, &sR);
        long long bad0 = idBadIn + idBadOut + gBadIn + gBadOut + hfNoId + hfOut + mvOver;
        // v0.10.0 phase 10: the occlusion init's value of a forward-depth pixel (the same float maths as kFwdInitShader)
        auto initAt = [&](UINT x, UINT y) -> float {
            float m = 1.0f;
            const UINT n = 1u << shift;
            for (UINT j = 0; j < n; ++j)
                for (UINT i = 0; i < n; ++i) {
                    const UINT px = (x << shift) + i < W ? (x << shift) + i : W - 1, py = (y << shift) + j < H ? (y << shift) + j : H - 1;
                    const float t = ((const float*)((const uint8_t*)mTw.pData + py * mTw.RowPitch))[px * 2];
                    if (t < m) m = t;
                }
            return (m >= 0.01f && m < 0.9f) ? m * 0.999999f : 0.0f;
        };
        // ids: inside the clip = the main pipeline's, outside 0 (RED; GREEN too at full resolution)
        for (UINT y = 0; y < H; ++y) {
            const uint16_t* a = (const uint16_t*)((const uint8_t*)sI.pData + y * sI.RowPitch);
            const uint16_t* b = (const uint16_t*)((const uint8_t*)sM.pData + y * sM.RowPitch);
            const bool yin = (LONG)y >= clip.top && (LONG)y < clip.bottom;
            for (UINT x = 0; x < W; ++x) {
                const bool in = yin && (LONG)x >= clip.left && (LONG)x < clip.right;
                const uint16_t r = shift ? a[x] : a[x * 2], g = shift ? 0 : a[x * 2 + 1];
                const uint16_t mr = b[x * 2], mg = b[x * 2 + 1];
                if (in) { ++clipPx; if (mr) ++idPx; if (r != mr) ++idBadIn; }
                else if (r) ++idBadOut;
                if (!shift) {
                    // v0.10.0 phase 10 cull: a GREEN id only counts where the main pipeline's forward depth won (fw > dt) -- an
                    // occluded forward fragment's id (main) is not drawn at all with the init, and no reader uses it
                    bool gWon = true;
                    if (cull && in) {
                        const float mfw = ((const float*)((const uint8_t*)mF.pData + y * mF.RowPitch))[x];
                        const float dt = ((const float*)((const uint8_t*)mTw.pData + y * mTw.RowPitch))[x * 2];
                        gWon = mfw >= 0.01f && mfw < 0.9f && mfw > dt;
                        if (!gWon) { ++gSkipNoWin; if (g != mg) ++gDiffNoWin; }
                    }
                    if (in && gWon) { if (mg) ++gPx; if (g != mg) ++gBadIn; }
                    else if (!in && g) ++gBadOut;
                }
            }
        }
        // 1/2 resolution: every forward-depth pixel inside the (halved) clip has a forward id; nothing outside it
        if (shift) {
            const LONG l = clip.left >> 1, t = clip.top >> 1, r = (clip.right + 1) >> 1, b = (clip.bottom + 1) >> 1;
            for (UINT y = 0; y < fh; ++y) {
                const float* dr = (const float*)((const uint8_t*)sF.pData + y * sF.RowPitch);
                const uint16_t* ir = (const uint16_t*)((const uint8_t*)sFI.pData + y * sFI.RowPitch);
                for (UINT x = 0; x < fw; ++x) {
                    const bool in = (LONG)y >= t && (LONG)y < b && (LONG)x >= l && (LONG)x < r;
                    if (dr[x] > 0.0f && !(cull && in && dr[x] == initAt(x, y))) {   // (phase 10: the init is no forward pixel)
                        if (!in) ++hfOut;
                        else { ++hfPx; if (!ir[x]) ++hfNoId; }
                    } else if (ir[x] && !in) ++hfOut;
                }
            }
        }
        // MVs inside the crop: the shadow vs the main pipeline (forward-touched pixels apart at 1/2 resolution)
        for (UINT y = 0; y < ch; ++y) {
            const uint16_t* sv = (const uint16_t*)((const uint8_t*)sV.pData + y * sV.RowPitch);
            const UINT py = cy + y;
            const uint16_t* mv = (const uint16_t*)((const uint8_t*)mMv.pData + py * mMv.RowPitch);
            const float* mfr = (const float*)((const uint8_t*)mF.pData + py * mF.RowPitch);
            const float* twr = (const float*)((const uint8_t*)mTw.pData + py * mTw.RowPitch);
            const float* sfr = (const float*)((const uint8_t*)sF.pData + (py >> shift) * sF.RowPitch);
            for (UINT x = 0; x < cw; ++x) {
                const UINT px = cx + x;
                const double dx = (double)H2F(sv[x * 2]) - (double)H2F(mv[px * 2]);
                const double dy = (double)H2F(sv[x * 2 + 1]) - (double)H2F(mv[px * 2 + 1]);
                const double dd = std::sqrt(dx * dx + dy * dy);
                const float dt = twr[px * 2];
                float mf = mfr[px], sf = sfr[px >> shift];
                if (!(mf >= 0.01f && mf < 0.9f)) mf = 0.0f;
                if (!(sf >= 0.01f && sf < 0.9f)) sf = 0.0f;
                const bool fwdTouched = shift && (mf > dt || sf > dt);
                if (!fwdTouched) {
                    ++mvPx;
                    if (dd > 0.07) ++mvOver;
                    if (dd > mvMax) mvMax = dd;
                } else {
                    ++fPx;
                    if (dd <= 0.07) ++fWithin007;
                    if (dd <= 0.25) ++fWithin025;
                    if (dd > 1.0) ++fOver1;
                    if (dd > fMax) fMax = dd;
                }
            }
        }
        // per-draw states (shadow vs main)
        if (nS == nTab) {
            const float* Rs = (const float*)sR.pData;
            for (int i = 0; i < nTab; ++i) {
                ++stateCmp;
                if ((int)(Rs[(i * 5 + 4) * 4] + 0.5f) != (int)(Rt[(i * 5 + 4) * 4] + 0.5f)) ++stateDiff;
            }
        }
        // the scene exercises the edge: a mover with pixels inside AND outside the crop (truth target)
        {
            std::vector<int> inPx(mover.size(), 0), outPx(mover.size(), 0);
            for (UINT y = 0; y < H; ++y) {
                const uint32_t* tr = (const uint32_t*)((const uint8_t*)mT.pData + y * mT.RowPitch);
                for (UINT x = 0; x < W; ++x) {
                    const uint32_t o = tr[x * 2];
                    if (!o || o > mover.size() || !mover[o - 1]) continue;
                    const bool in = x >= cx && x < cx + cw && y >= cy && y < cy + ch;
                    if (in) ++inPx[o - 1]; else ++outPx[o - 1];
                }
            }
            for (size_t o = 0; o < mover.size(); ++o) if (inPx[o] >= 20 && outPx[o] >= 20) { ++edgeFrames; break; }
        }
        ctx->Unmap(stId.Get(), 0); ctx->Unmap(stMainId.Get(), 0); ctx->Unmap(stFwd.Get(), 0);
        if (shift) ctx->Unmap(stFwdId.Get(), 0);
        ctx->Unmap(stMv.Get(), 0); ctx->Unmap(stR.Get(), 0);
        const bool bad = idBadIn + idBadOut + gBadIn + gBadOut + hfNoId + hfOut + mvOver != bad0;
        if (bad) {
            ++badFrames;
            if (badFrames <= 3)
                printf("   FAIL shadow %s frame %d: ids in/out %lld/%lld, GREEN in/out %lld/%lld, 1/2-res forward without id %lld / "
                       "outside %lld, MV px > 0.07 %lld (totals so far)\n", name, f, idBadIn, idBadOut, gBadIn, gBadOut, hfNoId, hfOut,
                       mvOver);
        }
        (void)f;
        return bad ? 1 : 0;
    }
    // end of the scene: the summary + the scene-level checks (returns the failed checks; *checks += the checks made)
    int Finish(int* checks) {
        int fl = 0;
        *checks += 3;
        if (replayFails || genFails) { ++fl; printf("   FAIL shadow %s: %d replay / %d Generate failures\n", name, replayFails, genFails); }
        if (edgeFrames * 2 < frames) {
            ++fl; printf("   FAIL shadow %s: a mover crossed the crop's edge in only %d of %d frames (the scene must exercise it)\n",
                         name, edgeFrames, frames);
        }
        if (!(idPx > 0 && mvPx > 0)) { ++fl; printf("   FAIL shadow %s: nothing compared (ids %lld, MV px %lld)\n", name, idPx, mvPx); }
        if (shift) {
            *checks += 1;
            // the 1/2-resolution trade-off: forward-touched pixels may differ (a wire 1-2 px wide moves by up to 1 px); the bulk
            // must still agree
            if (!(fPx > 0 && fWithin025 * 10 >= fPx * 6)) {
                ++fl; printf("   FAIL shadow %s: forward-touched px within 0.25 px of the full-resolution path: %lld of %lld\n", name,
                             fWithin025, fPx);
            }
        }
        printf("   shadow %s (clip (%ld,%ld)-(%ld,%ld), crop (%u,%u) %ux%u of %ux%u, forward depth %ux%u): ids inside the clip %lld px "
               "compared, %lld wrong, %lld outside set%s; MV (crop, %s) %lld px, %lld > 0.07 px, max %.4f px",
               name, (long)clip.left, (long)clip.top, (long)clip.right, (long)clip.bottom, cx, cy, cw, ch, W, H, fw, fh, idPx, idBadIn,
               idBadOut, shift ? "" : "", shift ? "pixels the forward depth does not touch" : "every pixel", mvPx, mvOver, mvMax);
        if (!shift) printf("; GREEN (forward ids) %lld px, %lld wrong inside, %lld outside", gPx, gBadIn, gBadOut);
        else printf("; 1/2-res forward depth px %lld (inside the halved clip), without an id %lld, outside %lld; forward-touched px "
                    "%lld: within 0.07 px %lld (%.1f %%), within 0.25 px %lld (%.1f %%), > 1 px %lld, max %.3f px",
                    hfPx, hfNoId, hfOut, fPx, fWithin007, fPx ? 100.0 * (double)fWithin007 / (double)fPx : 0.0, fWithin025,
                    fPx ? 100.0 * (double)fWithin025 / (double)fPx : 0.0, fOver1, fMax);
        if (batch) printf("; BATCH%s (occlusion init %lld frames; GREEN where the forward depth lost: %lld px, %lld differ -- "
                          "not compared)", cull ? " + cull" : "", cullInit, gSkipNoWin, gDiffNoWin);
        printf("; per-draw states differing from the main pipeline %lld of %lld; a mover crossed the crop edge in %d of %d frames; "
               "clip = %.1f %% of the image\n", stateDiff, stateCmp, edgeFrames, frames,
               frames ? 100.0 * (double)clipPx / ((double)frames * W * H) : 0.0);
        return fl;
    }
};

int RunConvoyScene(Env& E, float snapPx, Totals& tot, bool mutNoCons, bool mutNoFwd, Shadow* const* sh = nullptr, int nSh = 0) {
    const int frames = 100;
    printf("\n===== S6 consensus camera R (convoy + unique cars moving together vs the static scene) + forward-pass ids (plate, "
           "glass, wire) (%d frames, snap %.3f px%s%s) =====\n", frames, (double)snapPx, mutNoCons ? ", MUTATION: consensus off" : "",
           mutNoFwd ? ", MUTATION: forward ids off" : "");
    ID3D11Device* dev = E.dev.Get();
    ID3D11DeviceContext* ctx = E.ctx.Get();
    ID3D11DeviceContext1* ctx1 = E.ctx1.Get();
    DrawIdMv::SetParams(8.0f, snapPx);
    DrawIdMv::SetConsensus(!mutNoCons);
    // v0.10.0 phase 4: the "nofwd" mutation isolates the forward-id path -- with the attach on, pass B gives the plate (2 cm in
    // front of its body) the body's R even without forward ids, so the mutation would pass
    const float attachSaved = DrawIdMv::AttachM();
    if (mutNoFwd) DrawIdMv::SetAttach(0.0f);
    View v;
    S6Res r;
    if (!MakeView(E, v, W, H)) { printf("view textures failed\n"); return 1; }
    {
        bool ok = MakeTex(dev, W, H, DXGI_FORMAT_R16G16_UINT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &r.id2) &&
                  SUCCEEDED(dev->CreateRenderTargetView(r.id2.Get(), nullptr, &r.id2Rtv)) &&
                  SUCCEEDED(dev->CreateShaderResourceView(r.id2.Get(), nullptr, &r.id2Srv)) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R16G16_UINT, 0, &r.stId2, true) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, &r.fwd) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32_TYPELESS, 0, &r.stFwd, true) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32G32_UINT, D3D11_BIND_RENDER_TARGET, &r.fTruth) &&
                  SUCCEEDED(dev->CreateRenderTargetView(r.fTruth.Get(), nullptr, &r.fTruthRtv)) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32G32_UINT, 0, &r.stFTruth, true) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_D32_FLOAT, D3D11_BIND_DEPTH_STENCIL, &r.fTruthDepth) &&
                  SUCCEEDED(dev->CreateDepthStencilView(r.fTruthDepth.Get(), nullptr, &r.fTruthDsv)) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32G8X24_TYPELESS, 0, &r.stTwin, true) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, &r.scratch) &&
                  SUCCEEDED(dev->CreateRenderTargetView(r.scratch.Get(), nullptr, &r.scratchRtv));
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{}; dd.Format = DXGI_FORMAT_D32_FLOAT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        ok = ok && SUCCEEDED(dev->CreateDepthStencilView(r.fwd.Get(), &dd, &r.fwdDsv)) &&
             SUCCEEDED(dev->CreateShaderResourceView(r.fwd.Get(), &sd, &r.fwdSrv));
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; ds.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        ds.StencilEnable = FALSE; ds.StencilReadMask = 0xFF; ds.StencilWriteMask = 0xFF;
        ds.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
        ds.BackFace = ds.FrontFace;
        ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&ds, &r.dssFwdGame));
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;            // inject.cpp g_fwdDss: GREATER_EQUAL, write ALL
        ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&ds, &r.dssFwdRe));
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &r.bsOver));
        bd.AlphaToCoverageEnable = TRUE;                            // inject.cpp g_fwdBs: alpha-to-coverage, mask 0
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].RenderTargetWriteMask = 0;
        ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &r.bsA2C));
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;   // the truth pass: same coverage, writes
        ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &r.bsA2CAll));
        ComPtr<ID3DBlob> pf = Compile("PSFwd", "ps_5_0"), pt = Compile("PSFwdTruth", "ps_5_0");
        ok = ok && pf && pt && SUCCEEDED(dev->CreatePixelShader(pf->GetBufferPointer(), pf->GetBufferSize(), nullptr, &r.psFwd)) &&
             SUCCEEDED(dev->CreatePixelShader(pt->GetBufferPointer(), pt->GetBufferSize(), nullptr, &r.psFwdTruth));
        if (!ok) { printf("S6 resources failed\n"); return 1; }
    }
    CameraMv cam;
    if (!cam.Init(dev)) { printf("CameraMv init failed\n"); return 1; }
    cam.SetEgoPixel(0.0f);
    cam.SetEgoOrigin(0.0f);
    CandidateRecord cand;
    cand.Init(dev);
    DrawIdRecord rec, fwdRec;
    fwdRec.SetForward(true);

    // ---- objects ----
    std::vector<Obj> objs;
    std::vector<char> isFwd;
    auto add = [&](const char* n, Kind k, int mesh, bool cn, int inst, int group, bool fwd = false, int alphaMode = 0) {
        objs.push_back({ n, k, mesh, k == kInstanced ? 2 : 0, cn, alphaMode != 0, inst, group }); isFwd.push_back(fwd ? 1 : 0);
        return (int)objs.size() - 1; };
    const int oGround = add("ground", kStatic, MGround, true, 0, 0);
    int oBox[3];
    for (int k = 0; k < 3; ++k) oBox[k] = add("static box", kStatic, MBox0 + k, false, 0, 1);
    int oPost[50];
    for (int k = 0; k < 50; ++k) oPost[k] = add("roadside post (50 identical)", kStatic, MPost, false, 0, 2);
    int oClump[30];
    for (int k = 0; k < 30; ++k) oClump[k] = add("grass clump (30 identical)", kStatic, MClump, true, 0, 3);
    int oTruck[30];
    for (int k = 0; k < 30; ++k) oTruck[k] = add("convoy truck (30 identical, one velocity)", kMover, MTruck, false, 0, 4);
    int oCar[12];
    for (int k = 0; k < 12; ++k) oCar[k] = add("car (12 unique meshes, the convoy's velocity)", kMover, MCar0 + k, false, 0, 5);
    const int oBody = add("vehicle body (plate carrier)", kMover, MBody, false, 0, 6);
    const int oInst = add("instanced vegetation", kInstanced, MInst, true, 12, 7);
    const int oPlate = add("FORWARD plate (body matrix, alpha band)", kMover, MPlateF, true, 0, 8, true, 1);
    const int oGlass = add("FORWARD glass (alpha 0.3: no forward depth)", kMover, MGlass, true, 0, 9, true, 2);
    const int oWire = add("FORWARD wire gantry (static, no depth write)", kStatic, MWire, true, 0, 10, true, 0);
    const int nObj = (int)objs.size();
    (void)oGround; (void)oInst;
    auto alphaMode = [&](int o) { return o == oPlate ? 1 : (o == oGlass ? 2 : 0); };
    auto dupGroup = [&](int o) { int c = 0; for (int q = 0; q < nObj; ++q) if (objs[q].mesh == objs[o].mesh) ++c; return c > 1; };

    std::vector<M4> mvpPrev(nObj, I4()), mvpCur(nObj, I4());
    std::vector<bool> drawnPrev(nObj, false);
    std::vector<int> stPrev(nObj, -1), stCur(nObj, -1), wantPrev(nObj, -1), wantAltPrev(nObj, -1), drawnRun(nObj, 0);
    std::vector<int> flick(nObj, 0), wrongState(nObj, 0), objFramesBad(nObj, 0);
    int checks = 0, fails = 0, idMis = 0, idMisG = 0, mvFails = 0, debugMsgs = 0, camRBad = 0, consFrames = 0;
    long long rsSkip = 0, rsRep = 0;                    // v0.10.0 phase 14 (rstatic): draws the static gate skipped / replayed
    long long idTies = 0;                               // phase 14: exact-depth ties lost by a skipped draw (counted)
    if (g_rstatic && !nSh) DrawIdMv::StaticClear(DrawIdMv::kStatClrConfig);
    long long idChecked = 0, idCheckedG = 0, mvChecked = 0, plateFwdPx = 0, plateBandPx = 0, glassPx = 0, glassIdPx = 0;
    long long wireFwdPx = 0, plateOkPx = 0, plateFwdPx2 = 0;
    double worstCamPx = 0.0, worstMv[12] = {};
    uint64_t pairedSum = 0, eligSum = 0;
    std::mt19937 rng(606);
    const FLOAT zero[4] = { 0, 0, 0, 0 };
    const D3D11_RECT sc0 = { 0, 0, W, H };
    M4 VprevX = I4();
    GpuPerf::Restart();
    for (int f = 0; f < frames; ++f) {
        PerfFrame();
        // ---- camera: 0.5 m / frame, gentle yaw ----
        const M4 camPose = Mul(T(0.0, 1.6, 0.5 * f), RotY(0.002 * f));
        M4 V; Inv(camPose, V);
        const M4 Pw = Proj(kNearWorld);
        const M4 VPw = Mul(Pw, V);
        std::vector<M4> world(nObj, I4());
        world[oGround] = T(0, 0, 100);
        for (int k = 0; k < 3; ++k) world[oBox[k]] = T((k & 1) ? 12.0 : -12.0, 0.0, 80.0 + 60.0 * k);
        for (int k = 0; k < 50; ++k) world[oPost[k]] = T((k & 1) ? 6.5 : -6.5, 0.0, 40.0 + 12.0 * (k / 2));
        for (int k = 0; k < 30; ++k) world[oClump[k]] = T(((k & 1) ? 9.0 : -9.0) + 0.4 * (k % 3), 0.0, 60.0 + 5.0 * (k / 2));
        // the convoy + the cars: ONE velocity (0.06, 0, 2.0) m / frame -> the same R_draw for all 42
        const double cx = 0.06 * f, cz = 2.0 * f;
        for (int k = 0; k < 30; ++k) world[oTruck[k]] = T(-3.8 + cx, 0.0, 45.0 + 13.0 * k + cz);
        for (int k = 0; k < 12; ++k) world[oCar[k]] = T(3.8 + cx, 0.0, 38.0 + 15.0 * k + cz);
        // the plate carrier: ahead in the camera lane, own speed + a small yaw
        const M4 body = Mul(T(0.3, 0.0, 0.5 * f + 10.0 + 2.0 * sin(0.05 * f)), RotY(0.03 * sin(0.07 * f)));
        world[oBody] = body;
        world[oPlate] = Mul(body, T(0.0, 0.9, -2.32));          // 2 cm in front of the rear face (toward the camera)
        world[oGlass] = Mul(body, T(0.0, 1.75, -2.33));
        world[oWire] = T(0.0, 6.0, 70.0);                       // a static gantry across the road
        for (int o = 0; o < nObj; ++o) mvpCur[o] = objs[o].kind == kInstanced ? VPw : Mul(Pw, Mul(V, world[o]));
        // ---- draw order: G-buffer (static and moving interleaved, shuffled), then the forward draws ----
        std::vector<int> order, forder;
        for (int o = 0; o < nObj; ++o) { if (isFwd[o]) forder.push_back(o); else order.push_back(o); }
        std::shuffle(order.begin(), order.end(), rng);
        std::vector<bool> drawnCur(nObj, true);
        for (int o = 0; o < nObj; ++o) drawnRun[o] = drawnCur[o] ? drawnRun[o] + 1 : 0;
        const int nd = (int)order.size(), nf = (int)forder.size();
        ID3D11Buffer* ring = E.ring[f & 1].Get();
        {
            D3D11_MAPPED_SUBRESOURCE mp{};
            if (FAILED(ctx->Map(ring, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { printf("ring map failed\n"); return 1; }
            float* p = (float*)mp.pData;
            memset(p, 0, 2u << 20);
            for (int d = 0; d < nd + nf; ++d) {
                const int o = d < nd ? order[d] : forder[d - nd];
                float* w = p + (UINT)d * 64u;
                for (int rr = 0; rr < 4; ++rr) for (int c = 0; c < 4; ++c) w[(4 + rr) * 4 + c] = (float)mvpCur[o].m[rr][c];
                // row 8: object + 1, draw (G-buffer: index + 1; forward: forward index + 1), alpha mode
                w[8 * 4 + 0] = (float)(o + 1); w[8 * 4 + 1] = (float)((d < nd ? d : d - nd) + 1);
                w[8 * 4 + 2] = (float)alphaMode(o);
            }
            ctx->Unmap(ring, 0);
        }
        auto halton = [](int i, int b) { double q = 0, fct = 1; while (i > 0) { fct /= b; q += fct * (i % b); i /= b; } return q; };
        const float jx = (float)(halton(f % 8 + 1, 2) - 0.5), jy = (float)(halton(f % 8 + 1, 3) - 0.5);
        const D3D11_VIEWPORT vpW = { jx, jy, (float)(W + g_vpOver), (float)(H + g_vpOver), 0.01f, 0.9f };
        auto bindDraw = [&](int o, int d) {
            const Obj& ob = objs[o];
            ctx->RSSetViewports(1, &vpW);
            ctx->RSSetScissorRects(1, &sc0);
            ctx->RSSetState(ob.cullNone ? E.rsNone.Get() : E.rsBack.Get());
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11Buffer* vbs[2] = { E.vb[0].Get(), E.instVb[0].Get() };
            const UINT strides[2] = { sizeof(Vtx), 12 }, offs[2] = { 0, 0 };
            ctx->IASetVertexBuffers(0, 2, vbs, strides, offs);
            ctx->IASetIndexBuffer(E.ib[0].Get(), DXGI_FORMAT_R32_UINT, 0);
            ID3D11ShaderResourceView* vsSrv[4] = {};
            ctx->VSSetShaderResources(0, 4, vsSrv);
            if (ob.kind == kInstanced) { ctx->IASetInputLayout(E.ilInst.Get()); ctx->VSSetShader(E.vsInst.Get(), nullptr, 0); }
            else { ctx->IASetInputLayout(E.il.Get()); ctx->VSSetShader(E.vsMain.Get(), nullptr, 0); }
            const UINT first = (UINT)d * 16u, num = 16u;
            ctx1->VSSetConstantBuffers1(0, 1, &ring, &first, &num);
            ctx1->PSSetConstantBuffers1(0, 1, &ring, &first, &num);
        };
        // ---- G-buffer ----
        ctx->ClearDepthStencilView(v.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
        ctx->ClearRenderTargetView(v.truthRtv.Get(), zero);
        ctx->ClearRenderTargetView(v.colorRtv.Get(), zero);
        ID3D11RenderTargetView* rtvs[2] = { v.truthRtv.Get(), v.colorRtv.Get() };
        ctx->OMSetRenderTargets(2, rtvs, v.dsv.Get());
        ctx->OMSetDepthStencilState(E.dss.Get(), 1);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        ctx->PSSetShader(E.ps.Get(), nullptr, 0);
        rec.Reset(); fwdRec.Reset(); cand.Reset();
        cand.SetDropped(cam.MedoidDropBits());          // v0.10.0 phase 8 (mv_medoid_drop)
        for (int q = 0; q < nSh; ++q) sh[q]->BeginFrame();   // v0.10.0 phase 9 (S11)
        for (int d = 0; d < nd; ++d) {
            const int o = order[d];
            const Obj& ob = objs[o];
            const Mesh& m = E.meshes[ob.mesh];
            bindDraw(o, d);
            const bool inst = ob.kind == kInstanced;
            CandidateRecord::DrawKey k{};
            if (!inst) {
                k.ib = E.ib[0].Get(); k.vb = E.vb[0].Get(); k.indexCount = m.ic; k.startIndex = m.si; k.baseVertex = m.bv;
                if (cand.Count(0) < CandidateRecord::kSlots && !cand.Dropped(0)) cand.Record(ctx, 0, ring, (UINT)d * 256u + 64u, k);
            }
            rec.Record(ctx1, m.ic, inst ? (UINT)ob.instances : 1u, m.si, m.bv, 0u, inst);
            for (int q = 0; q < nSh; ++q)
                sh[q]->OnGbufDraw(ctx, ctx1, ring, d, inst ? nullptr : &k, m.ic, inst ? (UINT)ob.instances : 1u, m.si, m.bv, inst);
            if (inst) ctx->DrawIndexedInstanced(m.ic, ob.instances, m.si, m.bv, 0);
            else ctx->DrawIndexed(m.ic, m.si, m.bv);
        }
        // G-buffer leave: the replay (RED channel of the two-channel target)
        if (g_rstatic && !nSh) {                        // v0.10.0 phase 14 (rstatic): inject.cpp DidReplay's static gate
            DrawIdMv::SetFrame((uint64_t)f + 1); DrawIdMv::PollMain(ctx); DrawIdRecord::SetStaticGate(true);
        }
        if (!rec.Replay(ctx1, r.id2Rtv.Get(), v.roDsv.Get(), false, false).ok) { printf("   FAIL replay (frame %d)\n", f); ++fails; }
        DrawIdRecord::SetStaticGate(false);
        if (g_rstatic && !nSh) { rsSkip += rec.StaticSkipped(); rsRep += rec.ReplayedDraws(); }
        for (int q = 0; q < nSh; ++q) sh[q]->AfterGbuf(ctx1, v.roDsv.Get(), false);
        // ---- forward pass: the game's draw (scene depth tested, no write, "over") + the inject.cpp re-draw + record ----
        ID3D11RenderTargetView* crt = v.colorRtv.Get();
        bool fwdCleared = false;
        for (int k = 0; k < nf; ++k) {
            const int o = forder[k];
            const Mesh& m = E.meshes[objs[o].mesh];
            bindDraw(o, nd + k);
            ctx->OMSetRenderTargets(1, &crt, v.dsv.Get());
            ctx->OMSetDepthStencilState(r.dssFwdGame.Get(), 0);
            ctx->OMSetBlendState(r.bsOver.Get(), nullptr, 0xFFFFFFFFu);
            ctx->PSSetShader(r.psFwd.Get(), nullptr, 0);
            ctx->DrawIndexed(m.ic, m.si, m.bv);
            // FwdOnDraw: the forward-depth re-draw (target cleared to 0 = far at the pass's first re-draw)
            if (!fwdCleared) { ctx->ClearDepthStencilView(r.fwdDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0); fwdCleared = true; }
            ctx->OMSetRenderTargets(1, &crt, r.fwdDsv.Get());
            ctx->OMSetDepthStencilState(r.dssFwdRe.Get(), 0);
            ctx->OMSetBlendState(r.bsA2C.Get(), nullptr, 0xFFFFFFFFu);
            ctx->DrawIndexed(m.ic, m.si, m.bv);
            ctx->OMSetRenderTargets(1, &crt, v.dsv.Get());
            ctx->OMSetDepthStencilState(r.dssFwdGame.Get(), 0);
            ctx->OMSetBlendState(r.bsOver.Get(), nullptr, 0xFFFFFFFFu);
            for (int q = 0; q < nSh; ++q) sh[q]->OnFwdDraw(ctx, ctx1, r.dssFwdRe.Get(), r.bsA2C.Get(), m.ic, m.si, m.bv);
            fwdRec.Record(ctx1, m.ic, 1, m.si, m.bv, 0, false);      // DidFwdOnDraw
        }
        // truth of the forward depth's owner (test only: the re-draw's alpha-to-coverage + GREATER_EQUAL = the last of a tie
        // wins, into an own depth)
        ctx->ClearRenderTargetView(r.fTruthRtv.Get(), zero);
        ctx->ClearDepthStencilView(r.fTruthDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        {
            ID3D11RenderTargetView* tr[2] = { r.scratchRtv.Get(), r.fTruthRtv.Get() };
            for (int k = 0; k < nf; ++k) {
                const int o = forder[k];
                const Mesh& m = E.meshes[objs[o].mesh];
                bindDraw(o, nd + k);
                ctx->OMSetRenderTargets(2, tr, r.fTruthDsv.Get());
                ctx->OMSetDepthStencilState(r.dssFwdRe.Get(), 0);
                ctx->OMSetBlendState(r.bsA2CAll.Get(), nullptr, 0xFFFFFFFFu);
                ctx->PSSetShader(r.psFwdTruth.Get(), nullptr, 0);
                ctx->DrawIndexed(m.ic, m.si, m.bv);
            }
        }
        // ---- the scene depth discard: forward replay (GREEN channel, EQUAL against the forward depth), snapshot ----
        if (!mutNoFwd) {
            ctx->OMSetRenderTargets(1, &crt, v.dsv.Get());
            const auto fr = fwdRec.ReplayForward(ctx1, r.id2Rtv.Get(), r.fwdDsv.Get(), true, (UINT)rec.Count());
            if (!fr.ok || fr.replayed != nf) { printf("   FAIL forward replay (frame %d): ok %d replayed %d\n", f, (int)fr.ok, fr.replayed); ++fails; }
            for (int q = 0; q < nSh; ++q) sh[q]->AfterFwd(ctx1, (UINT)sh[q]->rec.Count(), nf);
        }
        ID3D11RenderTargetView* nullRtv = nullptr;
        ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
        ctx->CopyResource(v.twin.Get(), v.depth.Get());
        if (!mutNoFwd)                                    // v0.10.0 phase 10 batch shadows (after the snapshot, as FwdBatch)
            for (int q = 0; q < nSh; ++q) sh[q]->AfterTwin(ctx, ctx1, v.twinSrv.Get(), (UINT)sh[q]->rec.Count(), nf);
        CameraMv::FrameStats fs;
        const bool ok = cam.Generate(ctx, W, H, 0, 0, W, H, cand, v.twinSrv.Get(), v.dUav.Get(), v.mvUav.Get(), &fs, false,
                                     nullptr, nullptr, r.fwdSrv.Get(), &rec, r.id2Srv.Get(), mutNoFwd ? nullptr : &fwdRec);
        for (int q = 0; q < nSh; ++q) sh[q]->Generate(ctx, v.twinSrv.Get(), !mutNoFwd);
        if (!ok) { printf("   FAIL Generate (frame %d)\n", f); ++fails; }
        if (f >= 1 && cam.DidN() == 0) { printf("   FAIL frame %d: pass B ran without draw ids\n", f); ++fails; }
        if (f >= 1 && !mutNoFwd && cam.DidN() != (uint32_t)(nd + nf)) {
            printf("   FAIL frame %d: %u draws in the per-draw table, want %d (forward draws dropped)\n", f, cam.DidN(), nd + nf); ++fails;
        }
        const int nTab = (int)cam.DidN();
        // ---- readbacks ----
        ctx->CopyResource(v.stMv.Get(), v.mv.Get());
        ctx->CopyResource(v.stDepth.Get(), v.dOut.Get());
        ctx->CopyResource(v.stTruth.Get(), v.truth.Get());
        ctx->CopyResource(r.stId2.Get(), r.id2.Get());
        ctx->CopyResource(r.stFwd.Get(), r.fwd.Get());
        ctx->CopyResource(r.stFTruth.Get(), r.fTruth.Get());
        ctx->CopyResource(r.stTwin.Get(), v.twin.Get());
        if (cam.DidRSrv() && nTab) {
            ComPtr<ID3D11Resource> rres;
            cam.DidRSrv()->GetResource(&rres);
            const D3D11_BOX box{ 0, 0, 0, (UINT)nTab * DrawIdMv::kRStride * 16u, 1, 1 };
            ctx->CopySubresourceRegion(E.stR.Get(), 0, 0, 0, 0, rres.Get(), 0, &box);
        }
        D3D11_MAPPED_SUBRESOURCE mMv{}, mD{}, mT{}, mI{}, mR{}, mF{}, mFT{}, mTw{};
        ctx->Map(v.stMv.Get(), 0, D3D11_MAP_READ, 0, &mMv);
        if (!nSh) CmpFrame(mMv, W, H);                  // v0.10.0 phase 8 comparison mode (phase 9: not in the shadow scenes)
        ctx->Map(v.stDepth.Get(), 0, D3D11_MAP_READ, 0, &mD);
        ctx->Map(v.stTruth.Get(), 0, D3D11_MAP_READ, 0, &mT);
        ctx->Map(r.stId2.Get(), 0, D3D11_MAP_READ, 0, &mI);
        ctx->Map(E.stR.Get(), 0, D3D11_MAP_READ, 0, &mR);
        ctx->Map(r.stFwd.Get(), 0, D3D11_MAP_READ, 0, &mF);
        ctx->Map(r.stFTruth.Get(), 0, D3D11_MAP_READ, 0, &mFT);
        ctx->Map(r.stTwin.Get(), 0, D3D11_MAP_READ, 0, &mTw);
        const float* Rt = (const float*)mR.pData;
        // draw index in the R table: G-buffer draw d -> d, forward draw k -> nd + k
        std::vector<int> tabIdx(nObj, -1);
        for (int d = 0; d < nd; ++d) tabIdx[order[d]] = d;
        for (int k = 0; k < nf; ++k) tabIdx[forder[k]] = nd + k;
        int paired = 0, elig = 0;
        std::vector<char> parMark(nObj, 0);                 // v0.10.0 phase 6: took a rigid parent's R / state
        for (int o = 0; o < nObj; ++o) {
            const int ti = tabIdx[o];
            stCur[o] = (ti >= 0 && ti < nTab) ? (int)(Rt[(ti * 5 + 4) * 4] + 0.5f) : -1;
            parMark[o] = (ti >= 0 && ti < nTab && ParentMarked(Rt[(ti * 5 + 4) * 4 + 3])) ? 1 : 0;
            if (objs[o].kind != kInstanced && ti < nTab) { ++elig; if (stCur[o] == 2 || stCur[o] == 3) ++paired; }
        }
        if (f >= 1) { pairedSum += (uint64_t)paired; eligSum += (uint64_t)elig; }
        // ---- the camera R: GPU (solve buffer, = the consensus when found) vs exact, in pixels ----
        M4 RcamG, RcamX;
        {
            if (f == 0) VprevX = V;
            M4 Pinv, Vinv; Inv(Pw, Pinv); Inv(V, Vinv);
            RcamX = Mul(Pw, Mul(VprevX, Mul(Vinv, Pinv)));
            VprevX = V;
            float sv[CameraMv::kSolveFloats];
            if (!cam.ReadSolveBlocking(ctx, sv)) { printf("   FAIL solve readback\n"); ++fails; }
            for (int rr = 0; rr < 4; ++rr) for (int c = 0; c < 4; ++c) RcamG.m[rr][c] = sv[rr * 4 + c];
            if (sv[35] > 0.5f) ++consFrames;                    // Solve[8].w = consensus cluster size
            double worst = 0.0;
            const double zs[4] = { 0.0, kNearWorld / 200.0, kNearWorld / 40.0, kNearWorld / 5.0 };   // inf, 200 m, 40 m, 5 m
            for (double zq : zs)
                for (int gy = 0; gy < 7; ++gy) for (int gx = 0; gx < 7; ++gx) {
                    const double p4[4] = { -0.9 + 0.3 * gx, -0.9 + 0.3 * gy, zq, 1.0 };
                    double a[4], b[4];
                    Xf(RcamX, p4, a); Xf(RcamG, p4, b);
                    if (!(a[3] > 1e-9) || !(b[3] > 1e-9)) { worst = 1e9; continue; }
                    const double dx = (a[0] / a[3] - b[0] / b[3]) * W * 0.5, dy = (a[1] / a[3] - b[1] / b[3]) * H * 0.5;
                    worst = std::max(worst, std::sqrt(dx * dx + dy * dy));
                }
            if (f >= 2) {                                        // frame 0: no history; frame 1: the first pairs
                ++checks;
                if (!(worst <= 0.05)) {
                    ++fails; ++camRBad;
                    if (camRBad <= 3) printf("   FAIL frame %d: camera R off the exact camera R by %.3f px (consensus %s)\n", f,
                                             worst, sv[35] > 0.5f ? "used" : "NOT used");
                }
                worstCamPx = std::max(worstCamPx, worst);
            }
        }
        // ---- draw states (expected from the true probe deviation against the GPU camera R) ----
        std::vector<int> wantCur(nObj, -1), wantAlt(nObj, -1);
        for (int o = 0; o < nObj; ++o) {
            const Kind k = objs[o].kind;
            const bool hist = f >= 1 && drawnPrev[o];
            int want = -1, alt = -1;
            double tdev = 0.0;
            if (k != kInstanced && (hist || (f >= 1 && parMark[o]))) {
                const M4 P = Mul(RcamG, mvpCur[o]);
                const double probes[6][4] = { {0,0,0,1}, {4,0,0,1}, {-4,0,0,1}, {0,0,4,1}, {0,0,-4,1}, {0,4,0,1} };
                for (auto& q : probes) {
                    double a[4], b[4];
                    Xf(mvpPrev[o], q, a); Xf(P, q, b);
                    double dv;
                    const bool on = a[3] > 0.05 && b[3] > 0.05 && std::fabs(a[0] / a[3]) <= 1.5 && std::fabs(a[1] / a[3]) <= 1.5 &&
                                    std::fabs(b[0] / b[3]) <= 1.5 && std::fabs(b[1] / b[3]) <= 1.5;
                    if (on) dv = std::hypot((a[0] / a[3] - b[0] / b[3]) * W * 0.5, (a[1] / a[3] - b[1] / b[3]) * H * 0.5);
                    else dv = std::hypot((a[0] - b[0]) * W * 0.5, (a[1] - b[1]) * H * 0.5) / std::max(std::max(std::fabs(a[3]), std::fabs(b[3])), 1.0);
                    tdev = std::max(tdev, dv);
                }
            }
            if (k == kInstanced) want = DrawIdMv::kStInstanced;
            else if (!hist) want = DrawIdMv::kStUnpaired;
            else if (tdev >= snapPx * 1.5 + 0.02) want = DrawIdMv::kStMover;
            else if (tdev <= snapPx * 0.5) want = DrawIdMv::kStStatic;
            else { want = DrawIdMv::kStStatic; alt = DrawIdMv::kStMover; }
            if (dupGroup(o) && drawnRun[o] == 2) {
                if (want == DrawIdMv::kStMover) want = DrawIdMv::kStUnpaired;
                else if (alt == DrawIdMv::kStMover) alt = DrawIdMv::kStUnpaired;
            }
            if (f >= 1 && parMark[o] && want == DrawIdMv::kStUnpaired) ParentWant(tdev, snapPx, &want, &alt);   // phase 6
            wantCur[o] = want; wantAlt[o] = alt;
            if (isFwd[o] && mutNoFwd) continue;               // no forward draws in the table: nothing to compare
            ++checks;
            if (stCur[o] != want && stCur[o] != alt) {
                ++wrongState[o]; ++fails;
                if (wrongState[o] <= 2)
                    printf("   FAIL frame %d: %s #%d state %d, want %d (alt %d), true dev %.3f px\n", f, objs[o].name, o, stCur[o],
                           want, alt, tdev);
            }
            if (f >= 2 && drawnPrev[o] && stPrev[o] >= 0 && stCur[o] != stPrev[o] && want == wantPrev[o] && alt < 0 &&
                wantAltPrev[o] < 0 && k != kInstanced) ++flick[o];
        }
        // ---- per pixel: ids (both channels) and the MV against the pixel's owner ----
        // S6 expects TRUTH (not the GPU's own verdicts): a true mover its own exact R, everything else the EXACT camera R
        // (so a wrong camera R or a missing forward id fails here); inside the snap band either is accepted
        std::vector<M4> Rexp(nObj, I4()), Ralt(nObj, I4());
        std::vector<char> hasAlt(nObj, 0);
        for (int o = 0; o < nObj; ++o) {
            M4 Rtrue = RcamX;
            if (objs[o].kind != kInstanced && f >= 1 && (drawnPrev[o] || parMark[o])) {   // phase 6: + a rigid parent's R
                M4 ic; Inv(mvpCur[o], ic); Rtrue = Mul(mvpPrev[o], ic);
            }
            const int w = wantCur[o], a = wantAlt[o];
            if (w == DrawIdMv::kStMover) Rexp[o] = Rtrue;
            else Rexp[o] = RcamX;
            if (a == DrawIdMv::kStMover || (w == DrawIdMv::kStStatic && a >= 0)) { Ralt[o] = Rtrue; hasAlt[o] = 1; }
        }
        std::vector<int> pxOk(nObj, 0), pxAll(nObj, 0);
        const int nG = rec.Count();
        for (int y = 0; y < H; ++y) {
            const uint16_t* mvRow = (const uint16_t*)((const uint8_t*)mMv.pData + y * mMv.RowPitch);
            const float* dRow = (const float*)((const uint8_t*)mD.pData + y * mD.RowPitch);
            const uint32_t* tRow = (const uint32_t*)((const uint8_t*)mT.pData + y * mT.RowPitch);
            const uint16_t* iRow = (const uint16_t*)((const uint8_t*)mI.pData + y * mI.RowPitch);
            const float* fRow = (const float*)((const uint8_t*)mF.pData + y * mF.RowPitch);
            const uint32_t* ftRow = (const uint32_t*)((const uint8_t*)mFT.pData + y * mFT.RowPitch);
            const float* twRow = (const float*)((const uint8_t*)mTw.pData + y * mTw.RowPitch);
            for (int x = 0; x < W; ++x) {
                const uint32_t tObj = tRow[x * 2], tDraw = tRow[x * 2 + 1];
                const uint32_t fObj = ftRow[x * 2], fIdx = ftRow[x * 2 + 1];
                const float dt = twRow[x * 2];                   // R32 depth (+ stencil dword)
                float fw = fRow[x];
                if (!(fw >= 0.01f && fw < 0.9f)) fw = 0.0f;
                const bool fwWin = fw > dt;
                // id ownership: RED = the G-buffer owner (instanced draws are not replayed), GREEN = the forward-depth owner
                if (tObj) {
                    const int o = (int)tObj - 1;
                    // (v0.10.0 phase 14 rstatic: a draw the static gate skipped owns no id pixel)
                    const bool skippedT = g_rstatic && rec.StaticSkippedAt((int)tDraw - 1);
                    const uint32_t want = (objs[o].kind != kInstanced && !skippedT) ? tDraw : 0u;
                    ++idChecked;
                    // (phase 14: an exact-depth tie lost by the skipped owner -- the id of a replayed draw drawn after it: counted)
                    const uint32_t got = iRow[x * 2];
                    if (got != want && skippedT && got > tDraw && !rec.StaticSkippedAt((int)got - 1)) ++idTies;
                    else if (got != want) { ++idMis; if (idMis <= 5) printf("   FAIL id R at (%d,%d) frame %d: %u, want %u (%s)\n", x, y, f, got, want, objs[o].name); }
                }
                if (!mutNoFwd) {
                    const uint32_t wantG = fObj ? (uint32_t)nG + fIdx : 0u;
                    ++idCheckedG;
                    if (iRow[x * 2 + 1] != wantG) {
                        ++idMisG;
                        if (idMisG <= 5) printf("   FAIL id G at (%d,%d) frame %d: %u, want %u\n", x, y, f, iRow[x * 2 + 1], wantG);
                    }
                }
                if ((int)fObj - 1 == oGlass) ++glassIdPx;
                // the pixel's owner for the motion vector
                int o = -1;
                if (fwWin && fObj) o = (int)fObj - 1;
                else if (tObj && !fwWin) o = (int)tObj - 1;
                if (o < 0) continue;
                if (o == oPlate) ++plateFwdPx;
                if (o == oWire) ++wireFwdPx;
                const double dd = dRow[x];
                const double z = (dd - 0.01) / 0.89;
                const double u = (x + 0.5) / W, vv = (y + 0.5) / H;
                const double p4[4] = { u * 2 - 1, 1 - vv * 2, std::min(1.0, std::max(0.0, z)), 1.0 };
                double c[4];
                Xf(Rexp[o], p4, c);
                double ex = 0, ey = 0;
                if (c[3] > 1e-6) { ex = ((c[0] / c[3]) * 0.5 + 0.5 - u) * W; ey = ((0.5 - (c[1] / c[3]) * 0.5) - vv) * H; }
                const double gx = H2F(mvRow[x * 2]), gy = H2F(mvRow[x * 2 + 1]);
                double err = std::sqrt((gx - ex) * (gx - ex) + (gy - ey) * (gy - ey));
                const bool mover = objs[o].kind == kMover;
                double tol = (mover ? 0.03 + snapPx : 0.03) + std::max(std::fabs(ex), std::fabs(ey)) / 1024.0;
                if (hasAlt[o]) {                                 // inside the snap band: the other answer is right too
                    double ca[4]; Xf(Ralt[o], p4, ca);
                    double ax = 0, ay = 0;
                    if (ca[3] > 1e-6) { ax = ((ca[0] / ca[3]) * 0.5 + 0.5 - u) * W; ay = ((0.5 - (ca[1] / ca[3]) * 0.5) - vv) * H; }
                    const double ea = std::sqrt((gx - ax) * (gx - ax) + (gy - ay) * (gy - ay));
                    const double ta = 0.03 + snapPx + std::max(std::fabs(ax), std::fabs(ay)) / 1024.0;
                    if (ea / ta < err / tol) { err = ea; tol = ta; }
                }
                ++mvChecked; ++pxAll[o];
                const int g = objs[o].group;
                if (err > worstMv[g]) worstMv[g] = err;
                if (err <= tol) ++pxOk[o]; else ++mvFails;
                // the plate moves exactly with the body: its pixels against the BODY's true R (from frame 2 on)
                if (o == oPlate && f >= 2) {
                    ++plateFwdPx2;
                    M4 ic; Inv(mvpCur[oBody], ic);
                    const M4 Rb = Mul(mvpPrev[oBody], ic);
                    double cb[4]; Xf(Rb, p4, cb);
                    double bx = 0, by = 0;
                    if (cb[3] > 1e-6) { bx = ((cb[0] / cb[3]) * 0.5 + 0.5 - u) * W; by = ((0.5 - (cb[1] / cb[3]) * 0.5) - vv) * H; }
                    if (std::hypot(gx - bx, gy - by) <= 0.03 + snapPx + std::max(std::fabs(bx), std::fabs(by)) / 1024.0) ++plateOkPx;
                }
            }
        }
        // plate alpha band pixels: no forward depth there (the body's G-buffer id + R)
        for (int y = 0; y < H; ++y) {
            const uint32_t* tRow = (const uint32_t*)((const uint8_t*)mT.pData + y * mT.RowPitch);
            const uint32_t* ftRow = (const uint32_t*)((const uint8_t*)mFT.pData + y * mFT.RowPitch);
            for (int x = 0; x < W; ++x) if (tRow[x * 2] == (uint32_t)(oBody + 1) && !ftRow[x * 2]) ++plateBandPx;
        }
        (void)glassPx;
        for (int o = 0; o < nObj; ++o) {
            if (!pxAll[o]) continue;
            ++checks;
            if (pxOk[o] * 1000 < pxAll[o] * 995) {
                ++fails; ++objFramesBad[o];
                if (objFramesBad[o] <= 2)
                    printf("   FAIL frame %d: %s #%d MV right on %d of %d px (state %d)\n", f, objs[o].name, o, pxOk[o], pxAll[o], stCur[o]);
            }
        }
        if (nSh) {                                      // v0.10.0 phase 9 (S11): the shadow pipelines vs this one
            std::vector<char> moverObj(nObj, 0);
            for (int o = 0; o < nObj; ++o) moverObj[o] = objs[o].kind == kMover ? 1 : 0;
            for (int q = 0; q < nSh; ++q) { ++checks; fails += sh[q]->Compare(ctx, f, mMv, mF, mTw, mT, Rt, nTab, r.id2.Get(), moverObj); }
        }
        ctx->Unmap(v.stMv.Get(), 0); ctx->Unmap(v.stDepth.Get(), 0); ctx->Unmap(v.stTruth.Get(), 0);
        ctx->Unmap(r.stId2.Get(), 0); ctx->Unmap(E.stR.Get(), 0); ctx->Unmap(r.stFwd.Get(), 0);
        ctx->Unmap(r.stFTruth.Get(), 0); ctx->Unmap(r.stTwin.Get(), 0);
        if (g_debugLayer) { const int dl = DrainDebugLayer("S6"); ++checks; if (dl) { ++fails; debugMsgs += dl; } }
        for (int o = 0; o < nObj; ++o) { mvpPrev[o] = mvpCur[o]; drawnPrev[o] = drawnCur[o]; stPrev[o] = stCur[o];
                                         wantPrev[o] = wantCur[o]; wantAltPrev[o] = wantAlt[o]; }
    }
    rec.Shutdown(); fwdRec.Shutdown();
    for (int q = 0; q < nSh; ++q) { fails += sh[q]->Finish(&checks); sh[q]->Shutdown(); }   // v0.10.0 phase 9 (S11)
    // ---- scene-level checks ----
    const DrawIdMv::Stats& ds = cam.DrawIds().GetStats();
    PrintPhase8(ctx, ds, "");
    const double disAvg = ds.consDisFrames[0] ? ds.consDisSum[0] / (double)ds.consDisFrames[0] : 0.0;
    const double disMax = (double)cam.DrawIds().TakeConsDisMax(0);
    if (g_rstatic && !nSh) {                            // v0.10.0 phase 14: the gate must have skipped the static scenery
        printf("   static replay skip (every %u): skipped %lld of %lld G-buffer draws; exact-depth ties lost by a skipped draw: %lld px\n",
               g_rstatic, rsSkip, rsSkip + rsRep, idTies);
        checks += 2;
        if (idTies > 4 * frames) { ++fails; printf("   FAIL static replay skip: %lld tie px (bound %d)\n", idTies, 4 * frames); }
        if (rsSkip == 0) { ++fails; printf("   FAIL static replay skip: the gate never skipped a draw\n"); }
    }
    checks += 7;
    if (idMis) { ++fails; }
    if (idMisG) { ++fails; }
    int flickTot = 0;
    for (int o = 0; o < nObj; ++o) flickTot += flick[o];
    if (flickTot) { ++fails; printf("   FAIL draw state flicker: %d\n", flickTot); }
    // the scene must really exercise the trap: the medoid was wrong by > 0.5 px on average where the consensus was compared
    if (!mutNoCons && !(disAvg > 0.5)) { ++fails; printf("   FAIL the medoid was NOT fooled (|consensus - medoid| avg %.3f px): the scene does not exercise the consensus\n", disAvg); }
    if (!mutNoCons && consFrames < frames - 3) { ++fails; printf("   FAIL consensus used in only %d of %d frames\n", consFrames, frames); }
    if (!mutNoFwd && plateOkPx * 1000 < plateFwdPx2 * 995) {
        ++fails; printf("   FAIL the plate moves with the body on only %lld of %lld px (frames >= 2)\n", plateOkPx, plateFwdPx2);
    }
    if (!mutNoFwd && !(plateFwdPx > 3000 && wireFwdPx > 3000)) {
        ++fails; printf("   FAIL forward coverage too small: plate %lld px, wire %lld px\n", plateFwdPx, wireFwdPx);
    }
    printf("   camera R vs exact: worst %.4f px over the checked frames (limit 0.05), bad frames %d; consensus (Solve[8].w) in %d of "
           "%d frames; |consensus - medoid| avg %.3f px, max %.3f px over %llu compared frames (> 1 px: %llu) -- the medoid alone "
           "would have been that far off\n", worstCamPx, camRBad, consFrames, frames, disAvg, disMax,
           (unsigned long long)ds.consDisFrames[0], (unsigned long long)ds.consDisOver1[0]);
    printf("   consensus stats: world consensus %llu / fallback %llu frames, cluster avg %.1f draws; cabin %llu / %llu\n",
           (unsigned long long)ds.consFrames[0], (unsigned long long)ds.consFallback[0],
           ds.consFrames[0] ? (double)ds.consCluster[0] / (double)ds.consFrames[0] : 0.0,
           (unsigned long long)ds.consFrames[1], (unsigned long long)ds.consFallback[1]);
    printf("   ids: RED %d of %lld owned px wrong, GREEN %d of %lld px wrong; forward pixels (forward depth won): plate %lld (%lld from "
           "frame 2 move exactly with the body), wire %lld; glass (alpha 0.3) forward-depth px %lld (WARP's alpha-to-coverage dither; ids follow the forward depth); body px without a forward "
           "surface (incl. the plate's alpha band) %lld\n", idMis, idChecked, idMisG, idCheckedG, plateFwdPx, plateOkPx, wireFwdPx,
           glassIdPx, plateBandPx);
    printf("   MV: %d of %lld pixels outside tolerance; paired %.1f %%; state flicker %d; forward draws: %llu in the tables, "
           "paired %llu, movers %llu (read back)\n", mvFails, mvChecked,
           eligSum ? 100.0 * (double)pairedSum / (double)eligSum : 0.0, flickTot, (unsigned long long)ds.fwdDraws,
           (unsigned long long)ds.fwdPaired, (unsigned long long)ds.fwdMovers);
    printf("   worst MV error per group (px): ground %.3f box %.3f post %.3f clump %.3f convoy %.3f car %.3f body %.3f inst %.3f "
           "plate %.3f glass %.3f wire %.3f\n", worstMv[0], worstMv[1], worstMv[2], worstMv[3], worstMv[4], worstMv[5],
           worstMv[6], worstMv[7], worstMv[8], worstMv[9], worstMv[10]);
    if (g_debugLayer) printf("   debug layer: %d error / warning messages\n", debugMsgs);
    printf("   %s: %d / %d checks passed\n", fails ? "FAILED" : "passed", checks - fails, checks);
    DrawIdMv::SetConsensus(true);
    DrawIdMv::SetAttach(attachSaved);
    tot.checks += checks; tot.fails += fails;
    return fails;
}

// ---- S7 (v0.10.0 phase 4): LICENCE PLATES take the motion of the body they sit on -------------------------------------------
// In game (2026-10-08, Ctrl+F6) a bus plate was a forward MOVER whose own R moved it <= 0.5 px from the camera R while the
// body moved many px: the plate's own pair carried no vehicle motion. The attach (DrawIdMv CSAttach per draw, pass B per
// pixel) gives a forward draw / pixel the R of the G-buffer mover it sits on. Scene (camera 0.5 m / frame, gentle yaw, a
// static scene of 54 draws for the consensus):
//  * 12 vehicles with UNIQUE body meshes and ONE SHARED plate mesh (vertices in the body's frame, 2 cm in front of the rear
//    face) -- H2: the 12 identical plates form one ambiguous group (continuity guard: state 1 on their first paired frame);
//  * 4 vehicles with a shared BRACKET plate 0.9 m behind the rear face (the pixel under it is the road: only the per-draw
//    origin attach can help); two of them are culled every 13th frame (reappearing plates);
//  * 1 vehicle (H1) whose plate is drawn with the CAMERA VP in cb0 rows 4..7 and its vertices pre-transformed into world
//    space in a DYNAMIC vertex buffer every frame (its own R = the camera R: static) -- only the per-pixel attach can help;
//  * a static forward decal on a static box (must keep the exact camera R; never attached).
// Checks: the camera R (consensus) exact; every pixel's MV against its owner -- a plate pixel against its BODY's expected
// motion (the body's own R when the body is a mover, else the camera R), from frame 1; G-buffer objects as in S6; attach
// counters > 0. "noattach" (mv_fwd_attach_m = 0) must FAIL.
int RunPlateScene(Env& E, float snapPx, Totals& tot, bool mutNoAttach, bool mutNoTwin) {
    const int frames = 100;
    printf("\n===== S7 plates take their body's motion: 12 vehicles share one plate mesh (H2), 4 bracket plates (origin attach), a "
           "camera-VP plate with world-space vertices (H1), a static decal; phase 5: plates whose keys change every frame on their "
           "vehicle's matrix (twins), 12 identical G-buffer plates, a truck alongside the camera, a static sign face, a lamp node, "
           "a decoy (%d frames, snap %.3f px%s%s) =====\n", frames, (double)snapPx, mutNoAttach ? ", MUTATION: attach off" : "",
           mutNoTwin ? ", MUTATION: twins off" : "");
    ID3D11Device* dev = E.dev.Get();
    ID3D11DeviceContext* ctx = E.ctx.Get();
    ID3D11DeviceContext1* ctx1 = E.ctx1.Get();
    DrawIdMv::SetParams(8.0f, snapPx);
    DrawIdMv::SetConsensus(true);
    const float attachSaved = DrawIdMv::AttachM();
    DrawIdMv::SetAttach(mutNoAttach ? 0.0f : 0.5f);
    const bool twinSaved = DrawIdMv::Twin();                     // v0.10.0 phase 5
    DrawIdMv::SetTwin(!mutNoTwin);
    View v;
    S6Res r;
    if (!MakeView(E, v, W, H)) { printf("view textures failed\n"); return 1; }
    ComPtr<ID3D11Buffer> dynVb;                                   // H1: the plate's world-space vertices, rewritten every frame
    {
        bool ok = MakeTex(dev, W, H, DXGI_FORMAT_R16G16_UINT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &r.id2) &&
                  SUCCEEDED(dev->CreateRenderTargetView(r.id2.Get(), nullptr, &r.id2Rtv)) &&
                  SUCCEEDED(dev->CreateShaderResourceView(r.id2.Get(), nullptr, &r.id2Srv)) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, &r.fwd) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32_TYPELESS, 0, &r.stFwd, true) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32G32_UINT, D3D11_BIND_RENDER_TARGET, &r.fTruth) &&
                  SUCCEEDED(dev->CreateRenderTargetView(r.fTruth.Get(), nullptr, &r.fTruthRtv)) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32G32_UINT, 0, &r.stFTruth, true) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_D32_FLOAT, D3D11_BIND_DEPTH_STENCIL, &r.fTruthDepth) &&
                  SUCCEEDED(dev->CreateDepthStencilView(r.fTruthDepth.Get(), nullptr, &r.fTruthDsv)) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32G8X24_TYPELESS, 0, &r.stTwin, true) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, &r.scratch) &&
                  SUCCEEDED(dev->CreateRenderTargetView(r.scratch.Get(), nullptr, &r.scratchRtv));
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{}; dd.Format = DXGI_FORMAT_D32_FLOAT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        ok = ok && SUCCEEDED(dev->CreateDepthStencilView(r.fwd.Get(), &dd, &r.fwdDsv)) &&
             SUCCEEDED(dev->CreateShaderResourceView(r.fwd.Get(), &sd, &r.fwdSrv));
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; ds.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        ds.StencilEnable = FALSE; ds.StencilReadMask = 0xFF; ds.StencilWriteMask = 0xFF;
        ds.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
        ds.BackFace = ds.FrontFace;
        ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&ds, &r.dssFwdGame));
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&ds, &r.dssFwdRe));
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &r.bsOver));
        bd.AlphaToCoverageEnable = TRUE;
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].RenderTargetWriteMask = 0;
        ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &r.bsA2C));
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &r.bsA2CAll));
        ComPtr<ID3DBlob> pf = Compile("PSFwd", "ps_5_0"), pt = Compile("PSFwdTruth", "ps_5_0");
        ok = ok && pf && pt && SUCCEEDED(dev->CreatePixelShader(pf->GetBufferPointer(), pf->GetBufferSize(), nullptr, &r.psFwd)) &&
             SUCCEEDED(dev->CreatePixelShader(pt->GetBufferPointer(), pt->GetBufferSize(), nullptr, &r.psFwdTruth));
        D3D11_BUFFER_DESC vd{}; vd.ByteWidth = 4 * sizeof(Vtx); vd.Usage = D3D11_USAGE_DYNAMIC; vd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = ok && SUCCEEDED(dev->CreateBuffer(&vd, nullptr, &dynVb));
        if (!ok) { printf("S7 resources failed\n"); return 1; }
    }
    // v0.10.0 phase 5: draws whose KEYS CHANGE EVERY FRAME (the in-game plates: no partner group, ever). kDyn quads (vertices in
    // their object's frame), each replicated in 16 slots of 3 vertex buffers + an index buffer with 8 copies of the quad's
    // indices: frame f draws from VB f % 3 at slot (5 f + d) % 16 (base vertex) with startIndex ((3 f) % 8) * 6, so neither the
    // full key (VB pointer, offsets, base vertex) nor the pointer-free key (IndexCount, startIndex, baseVertex, VS) of a draw
    // ever repeats in consecutive frames.
    enum { D_GP1, D_FP1, D_LAMP, D_SIGN, D_GP2, D_FP2, D_DECOY, kDyn };
    ComPtr<ID3D11Buffer> dynPool[3], dynIb;
    {
        std::vector<Vtx> one;
        auto quad = [&](float cx, float cy, float cz, float ux, float uz, float vy) {   // centre, half axes (ux, 0, uz) / (0, vy, 0)
            for (int k = 0; k < 4; ++k) {
                const float su = (k == 1 || k == 2) ? 1.f : -1.f, sv = k >= 2 ? 1.f : -1.f;
                Vtx x{};
                x.p[0] = cx + su * ux; x.p[1] = cy + sv * vy; x.p[2] = cz + su * uz;
                x.uv[0] = (k == 1 || k == 2) ? 1.f : 0.f; x.uv[1] = k >= 2 ? 0.f : 1.f;
                one.push_back(x);
            }
        };
        quad(-0.8f, 0.75f, -2.32f, 0.25f, 0.0f, 0.12f);  // D_GP1: H1 body frame, rear face (z = -2.3) left of the H1 plate
        quad(0.8f, 0.75f, -2.32f, 0.25f, 0.0f, 0.12f);   // D_FP1: H1 body frame, rear face right
        quad(-0.3f, 1.55f, -2.22f, 0.3f, 0.0f, 0.08f);   // D_LAMP: node frame (body * T(0.3, 0.2, -0.1)) -> body (0, 1.75, -2.32)
        quad(0.0f, 2.6f, -0.13f, 0.6f, 0.0f, 0.4f);      // D_SIGN: post frame, 3 cm in front of the post
        quad(-1.22f, 1.2f, 9.0f, 0.0f, 0.4f, 0.25f);     // D_GP2: OV frame, inner side face (x = -1.2), 9 m ahead of the pivot
        quad(-1.22f, 1.2f, 10.5f, 0.0f, 0.4f, 0.25f);    // D_FP2: OV frame, 10.5 m ahead of the pivot
        // D_DECOY: its own (static, axis-aligned) frame; v0.10.0 phase 6: 3.2..3.8 m above its origin (was 0..0.6 m: the decoy
        // then stood INSIDE the H1 body's volume when the body passed it, at the body's depth, and the rigid-parent vote gave it the
        // body's motion -- a static object touching a moving one at the same depth is a documented limit of the vote). Its origin
        // (the phase-5 test: the body's pivot passes within 0.5 m) is unchanged.
        quad(0.0f, 3.5f, 0.0f, 0.2f, 0.0f, 0.3f);
        std::vector<Vtx> pool;
        for (int s = 0; s < 16; ++s) pool.insert(pool.end(), one.begin(), one.end());
        std::vector<uint32_t> idx;
        for (int s = 0; s < 8; ++s) for (uint32_t q : { 0u, 1u, 2u, 0u, 2u, 3u }) idx.push_back(q);
        D3D11_BUFFER_DESC bd{}; bd.Usage = D3D11_USAGE_DEFAULT;
        bd.ByteWidth = (UINT)(pool.size() * sizeof(Vtx)); bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{ pool.data(), 0, 0 };
        for (int k = 0; k < 3; ++k) if (FAILED(dev->CreateBuffer(&bd, &sd, &dynPool[k]))) { printf("S7 dyn VB failed\n"); return 1; }
        bd.ByteWidth = (UINT)(idx.size() * 4); bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        sd.pSysMem = idx.data();
        if (FAILED(dev->CreateBuffer(&bd, &sd, &dynIb))) { printf("S7 dyn IB failed\n"); return 1; }
    }
    CameraMv cam;
    if (!cam.Init(dev)) { printf("CameraMv init failed\n"); return 1; }
    cam.SetEgoPixel(0.0f);
    cam.SetEgoOrigin(0.0f);
    CandidateRecord cand;
    cand.Init(dev);
    DrawIdRecord rec, fwdRec;
    fwdRec.SetForward(true);

    // ---- objects ----
    std::vector<Obj> objs;
    std::vector<char> isFwd;
    std::vector<int> bodyOf;                                     // forward plate -> its body object (-1: none)
    auto add = [&](const char* n, Kind k, int mesh, bool cn, int group, bool fwd = false, int body = -1) {
        objs.push_back({ n, k, mesh, 0, cn, false, 0, group }); isFwd.push_back(fwd ? 1 : 0); bodyOf.push_back(body);
        return (int)objs.size() - 1; };
    const int oGround = add("ground", kStatic, MGround, true, 0);
    int oBox[3];
    for (int k = 0; k < 3; ++k) oBox[k] = add("static box", kStatic, MBox0 + k, false, 1);
    int oPost[50];
    for (int k = 0; k < 50; ++k) oPost[k] = add("roadside post (50 identical)", kStatic, MPost, false, 2);
    int oCarS[12], oPlS[12], oCarB[4], oPlB[4];
    for (int k = 0; k < 12; ++k) oCarS[k] = add("vehicle (12 unique bodies)", kMover, MPCar0 + k, false, 3);
    for (int k = 0; k < 4; ++k) oCarB[k] = add("bracket vehicle (4 unique bodies)", kMover, MPCarB0 + k, false, 4);
    const int oBodyH = add("H1 vehicle body", kMover, MBody, false, 5);
    for (int k = 0; k < 12; ++k) oPlS[k] = add("FORWARD shared plate (12 vehicles, one mesh)", kMover, MPlateS, true, 6, true, oCarS[k]);
    for (int k = 0; k < 4; ++k) oPlB[k] = add("FORWARD bracket plate (0.9 m behind the body)", kMover, MPlateB, true, 7, true, oCarB[k]);
    const int oPlH = add("FORWARD H1 plate (camera VP, world-space dynamic vertices)", kMover, MPlateF, true, 8, true, oBodyH);
    const int oBoard = add("static sign board", kStatic, MBox0 + 5, false, 1);
    const int oSign = add("FORWARD static decal on the static board", kStatic, MGlass, true, 9, true);
    // v0.10.0 phase 5 (twins): draws whose keys change every frame (dynOf >= 0, see the dyn pool) on their vehicle's matrix
    const int oGP1 = add("G-BUFFER plate on the H1 body (its matrix, keys new every frame)", kMover, 0, true, 11, false, oBodyH);
    const int oFP1 = add("FORWARD plate on the H1 body (its matrix, keys new every frame)", kMover, 0, true, 12, true, oBodyH);
    const int oLamp = add("G-buffer lamp on its own node 0.37 m from the H1 body's pivot (keys new every frame)", kMover, 0, true, 13,
                          false, oBodyH);
    int oGPS[12];
    for (int k = 0; k < 12; ++k) oGPS[k] = add("G-BUFFER shared plate (12 identical, the vehicle's matrix)", kMover, MPlateGS, true, 14, false, oCarS[k]);
    const int oSPost = add("static sign post", kStatic, MSignPost, false, 15);
    const int oFace = add("static sign face (the post's matrix, keys new every frame)", kStatic, 0, true, 16, false, oSPost);
    const int oOV = add("truck OV overtaking alongside the camera (pivot behind the camera plane)", kMover, MOV, false, 17);
    const int oGP2 = add("G-BUFFER side plate on OV (OV's matrix, keys new every frame)", kMover, 0, true, 18, false, oOV);
    const int oFP2 = add("FORWARD side plate on OV (OV's matrix, keys new every frame)", kMover, 0, true, 19, true, oOV);
    const int oDecoy = add("static decoy on the H1 body's path (keys new every frame)", kStatic, 0, true, 20);
    const int nObj = (int)objs.size();
    std::vector<int> dynOf(nObj, -1);
    dynOf[oGP1] = D_GP1; dynOf[oFP1] = D_FP1; dynOf[oLamp] = D_LAMP; dynOf[oFace] = D_SIGN; dynOf[oGP2] = D_GP2;
    dynOf[oFP2] = D_FP2; dynOf[oDecoy] = D_DECOY;
    (void)oGround;
    std::vector<M4> mvpPrev(nObj, I4()), mvpCur(nObj, I4());     // the TRUE object transforms (H1 plate: its body's frame)
    std::vector<bool> drawnPrev(nObj, false);
    std::vector<int> stCur(nObj, -1), drawnRun(nObj, 0), objFramesBad(nObj, 0);
    int checks = 0, fails = 0, camRBad = 0, consFrames = 0, mvFails = 0, debugMsgs = 0;
    long long rsSkip = 0, rsRep = 0;                    // v0.10.0 phase 14 (rstatic): draws the static gate skipped / replayed
    if (g_rstatic) DrawIdMv::StaticClear(DrawIdMv::kStatClrConfig);
    // v0.10.0 phase 14 (rstatic): exact-depth ties lost by a skipped draw (see the per-pixel check): the frame's id readback, the
    // tie pixels in total and the most in one frame (bounded: an intersection line between a mover and a static draw)
    int tieFrame = -1, tieMaxFrame = 0;
    long long tiePx = 0;
    ComPtr<ID3D11Texture2D> tieSt;
    D3D11_MAPPED_SUBRESOURCE tieMap{};
    constexpr int kTieMaxFrame = 8;
    long long mvChecked = 0, grpPx[24] = {}, grpOk[24] = {}, grpPx1[24] = {}, grpOk1[24] = {};
    double worstCamPx = 0.0, worstMv[24] = {};
    // v0.10.0 phase 5: pixels the camera R alone would get wrong (the scene exercises the inheritance), the frames the decoy was
    // within 0.5 m of the H1 body's moving pivot, OV's pivot behind the camera plane, state failures of the sign face / decoy
    long long camWrong[24] = {};
    int decoyNear = 0, ovBehind = 0, faceBad = 0, decoyBad = 0, faceChecked = 0;
    std::mt19937 rng(707);
    const FLOAT zero[4] = { 0, 0, 0, 0 };
    const D3D11_RECT sc0 = { 0, 0, W, H };
    M4 VprevX = I4();
    GpuPerf::Restart();
    for (int f = 0; f < frames; ++f) {
        PerfFrame();
        const M4 camPose = Mul(T(0.0, 1.6, 0.5 * f), RotY(0.002 * f));
        M4 V; Inv(camPose, V);
        const M4 Pw = Proj(kNearWorld);
        const M4 VPw = Mul(Pw, V);
        std::vector<M4> world(nObj, I4());
        world[oGround] = T(0, 0, 100);
        for (int k = 0; k < 3; ++k) world[oBox[k]] = T((k & 1) ? 12.0 : -12.0, 0.0, 80.0 + 60.0 * k);
        for (int k = 0; k < 50; ++k) world[oPost[k]] = T((k & 1) ? 9.5 : -9.5, 0.0, 40.0 + 12.0 * (k / 2));
        // shared-plate vehicles: left lane with the camera's speed (+ a wobble; the "bus ahead"), right lane faster
        for (int k = 0; k < 6; ++k)
            world[oCarS[k]] = Mul(T(-3.6, 0.0, 14.0 + 10.0 * k + 0.5 * f + 1.5 * sin(0.05 * f + k)), RotY(0.02 * sin(0.07 * f + k)));
        for (int k = 6; k < 12; ++k) world[oCarS[k]] = T(3.6, 0.0, 18.0 + 10.0 * (k - 6) + 0.8 * f);
        for (int k = 0; k < 4; ++k) world[oCarB[k]] = T((k & 1) ? 6.8 : -6.8, 0.0, 25.0 + 14.0 * k + 0.3 * f);
        const M4 bodyH = Mul(T(0.3, 0.0, 0.5 * f + 10.0 + 2.0 * sin(0.05 * f)), RotY(0.03 * sin(0.07 * f)));
        world[oBodyH] = bodyH;
        for (int k = 0; k < 12; ++k) world[oPlS[k]] = world[oCarS[k]];      // vertices in the body's frame
        for (int k = 0; k < 4; ++k) world[oPlB[k]] = world[oCarB[k]];
        const M4 plateH = Mul(bodyH, T(0.0, 1.0, -2.32));                 // 2 cm in front of the body's rear face
        world[oPlH] = plateH;
        world[oBoard] = T(-9.3, 0.0, 48.0);                                // phase 5: out of the bracket lane (x -6.8)
        world[oSign] = Mul(world[oBoard], T(0.0, 1.75, -1.02));             // 2 cm in front of the board's face
        // v0.10.0 phase 5: two plates on the H1 body's matrix, a lamp on its own node 0.37 m from the body's pivot
        world[oGP1] = bodyH; world[oFP1] = bodyH;
        world[oLamp] = Mul(bodyH, T(0.3, 0.2, -0.1));
        for (int k = 0; k < 12; ++k) world[oGPS[k]] = world[oCarS[k]];
        world[oSPost] = T(-6.0, 0.0, 70.0);
        world[oFace] = world[oSPost];
        // OV: 12 m long, pivot at its rear face 4..1 m BEHIND the camera, overtaking at +0.03 m / frame with a lateral sway
        const M4 ov = Mul(T(3.6 + 0.15 * sin(0.07 * f), 0.0, 0.5 * f - 4.0 + 0.03 * f), RotY(0.01 * sin(0.05 * f)));
        world[oOV] = ov; world[oGP2] = ov; world[oFP2] = ov;
        world[oDecoy] = T(0.3, 0.0, 40.5);                                 // the H1 body's pivot passes it at frames 60 / 61
        for (int o = 0; o < nObj; ++o) mvpCur[o] = Mul(Pw, Mul(V, world[o]));
        // bracket vehicles 0 and 2 (+ plates) are culled every 13th frame: their plates reappear
        std::vector<bool> drawnCur(nObj, true);
        if (f % 13 == 12) for (int k : { 0, 2 }) { drawnCur[oCarB[k]] = false; drawnCur[oPlB[k]] = false; }
        // v0.10.0 phase 5: shared-plate vehicles 2 and 9 (+ both their plates) are culled every 11th frame
        if (f % 11 == 10) for (int k : { 2, 9 }) { drawnCur[oCarS[k]] = false; drawnCur[oPlS[k]] = false; drawnCur[oGPS[k]] = false; }
        for (int o = 0; o < nObj; ++o) drawnRun[o] = drawnCur[o] ? drawnRun[o] + 1 : 0;
        // H1: world-space vertices into the dynamic VB, cb0 rows 4..7 = the camera VP
        {
            const Mesh& pm = E.meshes[MPlateF];
            (void)pm;
            D3D11_MAPPED_SUBRESOURCE mp{};
            if (FAILED(ctx->Map(dynVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { printf("dyn VB map failed\n"); return 1; }
            Vtx* vv = (Vtx*)mp.pData;
            const float pts[4][2] = { {-0.5f, -0.25f}, {0.5f, -0.25f}, {0.5f, 0.25f}, {-0.5f, 0.25f} };
            for (int q = 0; q < 4; ++q) {
                const double p4[4] = { pts[q][0], pts[q][1], 0.0, 1.0 };
                double w4[4]; Xf(plateH, p4, w4);
                Vtx x{}; x.p[0] = (float)w4[0]; x.p[1] = (float)w4[1]; x.p[2] = (float)w4[2];
                x.uv[0] = (q == 1 || q == 2) ? 1.f : 0.f; x.uv[1] = q >= 2 ? 0.f : 1.f;
                vv[q] = x;
            }
            ctx->Unmap(dynVb.Get(), 0);
        }
        std::vector<int> order, forder;
        for (int o = 0; o < nObj; ++o) { if (!drawnCur[o]) continue; if (isFwd[o]) forder.push_back(o); else order.push_back(o); }
        std::shuffle(order.begin(), order.end(), rng);
        const int nd = (int)order.size(), nf = (int)forder.size();
        ID3D11Buffer* ring = E.ring[f & 1].Get();
        {
            D3D11_MAPPED_SUBRESOURCE mp{};
            if (FAILED(ctx->Map(ring, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { printf("ring map failed\n"); return 1; }
            float* p = (float*)mp.pData;
            memset(p, 0, 2u << 20);
            for (int d = 0; d < nd + nf; ++d) {
                const int o = d < nd ? order[d] : forder[d - nd];
                const M4& m = o == oPlH ? VPw : mvpCur[o];               // H1: the camera VP, the motion is in the vertices
                float* w = p + (UINT)d * 64u;
                for (int rr = 0; rr < 4; ++rr) for (int c = 0; c < 4; ++c) w[(4 + rr) * 4 + c] = (float)m.m[rr][c];
                w[8 * 4 + 0] = (float)(o + 1); w[8 * 4 + 1] = (float)((d < nd ? d : d - nd) + 1); w[8 * 4 + 2] = 0.0f;
            }
            ctx->Unmap(ring, 0);
        }
        auto halton = [](int i, int b) { double q = 0, fct = 1; while (i > 0) { fct /= b; q += fct * (i % b); i /= b; } return q; };
        const float jx = (float)(halton(f % 8 + 1, 2) - 0.5), jy = (float)(halton(f % 8 + 1, 3) - 0.5);
        const D3D11_VIEWPORT vpW = { jx, jy, (float)(W + g_vpOver), (float)(H + g_vpOver), 0.01f, 0.9f };
        auto bindDraw = [&](int o, int d) {
            const Obj& ob = objs[o];
            ctx->RSSetViewports(1, &vpW);
            ctx->RSSetScissorRects(1, &sc0);
            ctx->RSSetState(ob.cullNone ? E.rsNone.Get() : E.rsBack.Get());
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11Buffer* vbs[2] = { o == oPlH ? dynVb.Get() : (dynOf[o] >= 0 ? dynPool[f % 3].Get() : E.vb[0].Get()),
                                     E.instVb[0].Get() };
            const UINT strides[2] = { sizeof(Vtx), 12 }, offs[2] = { 0, 0 };
            ctx->IASetVertexBuffers(0, 2, vbs, strides, offs);
            ctx->IASetIndexBuffer(dynOf[o] >= 0 ? dynIb.Get() : E.ib[0].Get(), DXGI_FORMAT_R32_UINT, 0);
            ID3D11ShaderResourceView* vsSrv[4] = {};
            ctx->VSSetShaderResources(0, 4, vsSrv);
            ctx->IASetInputLayout(E.il.Get());
            ctx->VSSetShader(E.vsMain.Get(), nullptr, 0);
            const UINT first = (UINT)d * 16u, num = 16u;
            ctx1->VSSetConstantBuffers1(0, 1, &ring, &first, &num);
            ctx1->PSSetConstantBuffers1(0, 1, &ring, &first, &num);
        };
        auto drawArgs = [&](int o, UINT* ic, UINT* si, INT* bv) {
            if (dynOf[o] >= 0) {                                         // v0.10.0 phase 5: keys new every frame
                *ic = 6; *si = (UINT)(((3 * f) % 8) * 6);
                *bv = (INT)((((5 * f + dynOf[o]) % 16) * kDyn + dynOf[o]) * 4);
                return;
            }
            const Mesh& m = E.meshes[objs[o].mesh];
            *ic = m.ic; *si = m.si; *bv = o == oPlH ? 0 : m.bv;           // the dynamic VB holds only the plate's 4 vertices
        };
        // ---- G-buffer ----
        ctx->ClearDepthStencilView(v.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
        ctx->ClearRenderTargetView(v.truthRtv.Get(), zero);
        ctx->ClearRenderTargetView(v.colorRtv.Get(), zero);
        ID3D11RenderTargetView* rtvs[2] = { v.truthRtv.Get(), v.colorRtv.Get() };
        ctx->OMSetRenderTargets(2, rtvs, v.dsv.Get());
        ctx->OMSetDepthStencilState(E.dss.Get(), 1);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        ctx->PSSetShader(E.ps.Get(), nullptr, 0);
        rec.Reset(); fwdRec.Reset(); cand.Reset();
        cand.SetDropped(cam.MedoidDropBits());          // v0.10.0 phase 8 (mv_medoid_drop)
        for (int d = 0; d < nd; ++d) {
            const int o = order[d];
            UINT ic, si; INT bv; drawArgs(o, &ic, &si, &bv);
            bindDraw(o, d);
            CandidateRecord::DrawKey k{};
            k.ib = dynOf[o] >= 0 ? dynIb.Get() : E.ib[0].Get(); k.vb = dynOf[o] >= 0 ? dynPool[f % 3].Get() : E.vb[0].Get();
            k.indexCount = ic; k.startIndex = si; k.baseVertex = bv;
            if (cand.Count(0) < CandidateRecord::kSlots && !cand.Dropped(0)) cand.Record(ctx, 0, ring, (UINT)d * 256u + 64u, k);
            rec.Record(ctx1, ic, 1u, si, bv, 0u, false);
            ctx->DrawIndexed(ic, si, bv);
        }
        if (g_rstatic) { DrawIdMv::SetFrame((uint64_t)f + 1); DrawIdMv::PollMain(ctx); DrawIdRecord::SetStaticGate(true); }
        if (!rec.Replay(ctx1, r.id2Rtv.Get(), v.roDsv.Get(), false, false).ok) { printf("   FAIL replay (frame %d)\n", f); ++fails; }
        DrawIdRecord::SetStaticGate(false);
        if (g_rstatic) { rsSkip += rec.StaticSkipped(); rsRep += rec.ReplayedDraws(); }
        // ---- forward pass (game draw + forward-depth re-draw + record), forward truth, forward replay ----
        ID3D11RenderTargetView* crt = v.colorRtv.Get();
        bool fwdCleared = false;
        for (int k = 0; k < nf; ++k) {
            const int o = forder[k];
            UINT ic, si; INT bv; drawArgs(o, &ic, &si, &bv);
            bindDraw(o, nd + k);
            ctx->OMSetRenderTargets(1, &crt, v.dsv.Get());
            ctx->OMSetDepthStencilState(r.dssFwdGame.Get(), 0);
            ctx->OMSetBlendState(r.bsOver.Get(), nullptr, 0xFFFFFFFFu);
            ctx->PSSetShader(r.psFwd.Get(), nullptr, 0);
            ctx->DrawIndexed(ic, si, bv);
            if (!fwdCleared) { ctx->ClearDepthStencilView(r.fwdDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0); fwdCleared = true; }
            ctx->OMSetRenderTargets(1, &crt, r.fwdDsv.Get());
            ctx->OMSetDepthStencilState(r.dssFwdRe.Get(), 0);
            ctx->OMSetBlendState(r.bsA2C.Get(), nullptr, 0xFFFFFFFFu);
            ctx->DrawIndexed(ic, si, bv);
            ctx->OMSetRenderTargets(1, &crt, v.dsv.Get());
            ctx->OMSetDepthStencilState(r.dssFwdGame.Get(), 0);
            ctx->OMSetBlendState(r.bsOver.Get(), nullptr, 0xFFFFFFFFu);
            fwdRec.Record(ctx1, ic, 1, si, bv, 0, false);
        }
        ctx->ClearRenderTargetView(r.fTruthRtv.Get(), zero);
        ctx->ClearDepthStencilView(r.fTruthDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        {
            ID3D11RenderTargetView* tr[2] = { r.scratchRtv.Get(), r.fTruthRtv.Get() };
            for (int k = 0; k < nf; ++k) {
                const int o = forder[k];
                UINT ic, si; INT bv; drawArgs(o, &ic, &si, &bv);
                bindDraw(o, nd + k);
                ctx->OMSetRenderTargets(2, tr, r.fTruthDsv.Get());
                ctx->OMSetDepthStencilState(r.dssFwdRe.Get(), 0);
                ctx->OMSetBlendState(r.bsA2CAll.Get(), nullptr, 0xFFFFFFFFu);
                ctx->PSSetShader(r.psFwdTruth.Get(), nullptr, 0);
                ctx->DrawIndexed(ic, si, bv);
            }
        }
        ctx->OMSetRenderTargets(1, &crt, v.dsv.Get());
        {
            const auto fr = fwdRec.ReplayForward(ctx1, r.id2Rtv.Get(), r.fwdDsv.Get(), true, (UINT)rec.Count());
            if (!fr.ok || fr.replayed != nf) { printf("   FAIL forward replay (frame %d): ok %d replayed %d\n", f, (int)fr.ok, fr.replayed); ++fails; }
        }
        ID3D11RenderTargetView* nullRtv = nullptr;
        ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
        ctx->CopyResource(v.twin.Get(), v.depth.Get());
        if (f == 50 && !cam.DrawIds().RequestDump("harness S7 frame 50")) { printf("   FAIL dump request refused\n"); ++fails; }
        CameraMv::FrameStats fs;
        const bool ok = cam.Generate(ctx, W, H, 0, 0, W, H, cand, v.twinSrv.Get(), v.dUav.Get(), v.mvUav.Get(), &fs, false,
                                     nullptr, nullptr, r.fwdSrv.Get(), &rec, r.id2Srv.Get(), &fwdRec);
        if (!ok) { printf("   FAIL Generate (frame %d)\n", f); ++fails; }
        if (f >= 1 && cam.DidN() != (uint32_t)(nd + nf)) {
            printf("   FAIL frame %d: %u draws in the per-draw table, want %d\n", f, cam.DidN(), nd + nf); ++fails;
        }
        const int nTab = (int)cam.DidN();
        ctx->CopyResource(v.stMv.Get(), v.mv.Get());
        ctx->CopyResource(v.stDepth.Get(), v.dOut.Get());
        ctx->CopyResource(v.stTruth.Get(), v.truth.Get());
        ctx->CopyResource(r.stFwd.Get(), r.fwd.Get());
        ctx->CopyResource(r.stFTruth.Get(), r.fTruth.Get());
        ctx->CopyResource(r.stTwin.Get(), v.twin.Get());
        if (cam.DidRSrv() && nTab) {
            ComPtr<ID3D11Resource> rres;
            cam.DidRSrv()->GetResource(&rres);
            const D3D11_BOX box{ 0, 0, 0, (UINT)nTab * DrawIdMv::kRStride * 16u, 1, 1 };
            ctx->CopySubresourceRegion(E.stR.Get(), 0, 0, 0, 0, rres.Get(), 0, &box);
        }
        D3D11_MAPPED_SUBRESOURCE mMv{}, mD{}, mT{}, mR{}, mF{}, mFT{}, mTw{};
        ctx->Map(v.stMv.Get(), 0, D3D11_MAP_READ, 0, &mMv);
        CmpFrame(mMv, W, H);                            // v0.10.0 phase 8 comparison mode
        ctx->Map(v.stDepth.Get(), 0, D3D11_MAP_READ, 0, &mD);
        ctx->Map(v.stTruth.Get(), 0, D3D11_MAP_READ, 0, &mT);
        ctx->Map(E.stR.Get(), 0, D3D11_MAP_READ, 0, &mR);
        ctx->Map(r.stFwd.Get(), 0, D3D11_MAP_READ, 0, &mF);
        ctx->Map(r.stFTruth.Get(), 0, D3D11_MAP_READ, 0, &mFT);
        ctx->Map(r.stTwin.Get(), 0, D3D11_MAP_READ, 0, &mTw);
        const float* Rt = (const float*)mR.pData;
        std::vector<int> tabIdx(nObj, -1);
        for (int d = 0; d < nd; ++d) tabIdx[order[d]] = d;
        for (int k = 0; k < nf; ++k) tabIdx[forder[k]] = nd + k;
        for (int o = 0; o < nObj; ++o) {
            const int ti = tabIdx[o];
            stCur[o] = (ti >= 0 && ti < nTab) ? (int)(Rt[(ti * 5 + 4) * 4] + 0.5f) : -1;
        }
        // ---- v0.10.0 phase 5 state checks: the sign face is its static post's twin (state 2); the decoy never takes the H1 body's R ----
        if (f >= 1 && drawnCur[oFace] && drawnPrev[oSPost] && drawnCur[oSPost] && !mutNoTwin) {
            ++checks; ++faceChecked;
            if (stCur[oFace] != DrawIdMv::kStStatic) {
                ++fails; ++faceBad;
                if (faceBad <= 2) printf("   FAIL frame %d: the static sign face is state %d (want 2: its post's twin)\n", f, stCur[oFace]);
            }
        }
        if (f >= 1 && drawnCur[oDecoy]) {
            const int ti = tabIdx[oDecoy];
            const float w = (ti >= 0 && ti < nTab) ? Rt[(ti * 5 + 4) * 4 + 3] : 0.0f;
            ++checks;
            if (stCur[oDecoy] == DrawIdMv::kStMover) {                   // phase 6: a static parent / twin (.w < 0) is fine
                ++fails; ++decoyBad;
                if (decoyBad <= 2) printf("   FAIL frame %d: the static decoy took a mover's R (state %d, .w %.1f)\n", f, stCur[oDecoy], (double)w);
            }
            const double dz = world[oBodyH].m[2][3] - world[oDecoy].m[2][3], dx = world[oBodyH].m[0][3] - world[oDecoy].m[0][3];
            if (std::sqrt(dx * dx + dz * dz) < 0.5 && stCur[oBodyH] == DrawIdMv::kStMover) ++decoyNear;
        }
        {
            const double org[4] = { 0, 0, 0, 1 };
            double c[4]; Xf(Mul(V, world[oOV]), org, c);
            if (c[2] < 0.0) ++ovBehind;                                   // view z of OV's pivot (LH, +z forward)
        }
        // ---- the camera R: GPU (consensus) vs exact ----
        M4 RcamG, RcamX;
        {
            if (f == 0) VprevX = V;
            M4 Pinv, Vinv; Inv(Pw, Pinv); Inv(V, Vinv);
            RcamX = Mul(Pw, Mul(VprevX, Mul(Vinv, Pinv)));
            VprevX = V;
            float sv[CameraMv::kSolveFloats];
            if (!cam.ReadSolveBlocking(ctx, sv)) { printf("   FAIL solve readback\n"); ++fails; }
            for (int rr = 0; rr < 4; ++rr) for (int c = 0; c < 4; ++c) RcamG.m[rr][c] = sv[rr * 4 + c];
            if (sv[35] > 0.5f) ++consFrames;
            double worst = 0.0;
            const double zs[4] = { 0.0, kNearWorld / 200.0, kNearWorld / 40.0, kNearWorld / 5.0 };
            for (double zq : zs)
                for (int gy = 0; gy < 7; ++gy) for (int gx = 0; gx < 7; ++gx) {
                    const double p4[4] = { -0.9 + 0.3 * gx, -0.9 + 0.3 * gy, zq, 1.0 };
                    double a[4], b[4];
                    Xf(RcamX, p4, a); Xf(RcamG, p4, b);
                    if (!(a[3] > 1e-9) || !(b[3] > 1e-9)) { worst = 1e9; continue; }
                    const double dx = (a[0] / a[3] - b[0] / b[3]) * W * 0.5, dy = (a[1] / a[3] - b[1] / b[3]) * H * 0.5;
                    worst = std::max(worst, std::sqrt(dx * dx + dy * dy));
                }
            if (f >= 2) {
                ++checks;
                if (!(worst <= 0.05)) { ++fails; ++camRBad; if (camRBad <= 3) printf("   FAIL frame %d: camera R off by %.3f px\n", f, worst); }
                worstCamPx = std::max(worstCamPx, worst);
            }
        }
        // ---- expected motion per G-buffer object (S6 rules: a true mover its own R, else the camera R; snap band: both) ----
        std::vector<M4> Rexp(nObj, RcamX), Ralt(nObj, RcamX);
        std::vector<char> hasAlt(nObj, 0);
        for (int o = 0; o < nObj; ++o) {
            if (isFwd[o] || bodyOf[o] >= 0) continue;                    // phase 5: G-buffer plates follow their body (below)
            const bool hist = f >= 1 && drawnPrev[o];
            if (!hist) continue;                                  // first frame / reappearing: no partner -> camera R
            const M4 P = Mul(RcamG, mvpCur[o]);
            const double probes[6][4] = { {0,0,0,1}, {4,0,0,1}, {-4,0,0,1}, {0,0,4,1}, {0,0,-4,1}, {0,4,0,1} };
            double tdev = 0.0;
            for (auto& q : probes) {
                double a[4], b[4];
                Xf(mvpPrev[o], q, a); Xf(P, q, b);
                double dv;
                const bool on = a[3] > 0.05 && b[3] > 0.05 && std::fabs(a[0] / a[3]) <= 1.5 && std::fabs(a[1] / a[3]) <= 1.5 &&
                                std::fabs(b[0] / b[3]) <= 1.5 && std::fabs(b[1] / b[3]) <= 1.5;
                if (on) dv = std::hypot((a[0] / a[3] - b[0] / b[3]) * W * 0.5, (a[1] / a[3] - b[1] / b[3]) * H * 0.5);
                else dv = std::hypot((a[0] - b[0]) * W * 0.5, (a[1] - b[1]) * H * 0.5) / std::max(std::max(std::fabs(a[3]), std::fabs(b[3])), 1.0);
                tdev = std::max(tdev, dv);
            }
            M4 ic; Inv(mvpCur[o], ic);
            const M4 Rtrue = Mul(mvpPrev[o], ic);
            if (tdev >= snapPx * 1.5 + 0.02) Rexp[o] = Rtrue;
            else if (tdev > snapPx * 0.5) { Ralt[o] = Rtrue; hasAlt[o] = 1; }
        }
        // a plate moves with its body (forward or, phase 5, G-buffer): the body's expectation; the static decal: the exact
        // camera R
        for (int o = 0; o < nObj; ++o) {
            if (bodyOf[o] < 0) continue;
            Rexp[o] = Rexp[bodyOf[o]]; Ralt[o] = Ralt[bodyOf[o]]; hasAlt[o] = hasAlt[bodyOf[o]];
        }
        // ---- per pixel ----
        std::vector<int> pxOk(nObj, 0), pxAll(nObj, 0);
        int tiePxFrame = 0;                             // v0.10.0 phase 14 (rstatic): exact-depth tie px of this frame
        for (int y = 0; y < H; ++y) {
            const uint16_t* mvRow = (const uint16_t*)((const uint8_t*)mMv.pData + y * mMv.RowPitch);
            const float* dRow = (const float*)((const uint8_t*)mD.pData + y * mD.RowPitch);
            const uint32_t* tRow = (const uint32_t*)((const uint8_t*)mT.pData + y * mT.RowPitch);
            const float* fRow = (const float*)((const uint8_t*)mF.pData + y * mF.RowPitch);
            const uint32_t* ftRow = (const uint32_t*)((const uint8_t*)mFT.pData + y * mFT.RowPitch);
            const float* twRow = (const float*)((const uint8_t*)mTw.pData + y * mTw.RowPitch);
            for (int x = 0; x < W; ++x) {
                const uint32_t tObj = tRow[x * 2], fObj = ftRow[x * 2];
                const float dt = twRow[x * 2];
                float fw = fRow[x];
                if (!(fw >= 0.01f && fw < 0.9f)) fw = 0.0f;
                const bool fwWin = fw > dt;
                int o = -1;
                if (fwWin && fObj) o = (int)fObj - 1;
                else if (tObj && !fwWin) o = (int)tObj - 1;
                if (o < 0 || f < 1) continue;
                const double dd = dRow[x];
                const double z = (dd - 0.01) / 0.89;
                const double u = (x + 0.5) / W, vv = (y + 0.5) / H;
                const double p4[4] = { u * 2 - 1, 1 - vv * 2, std::min(1.0, std::max(0.0, z)), 1.0 };
                auto mvOf = [&](const M4& Rm, double* ex, double* ey) {
                    double c[4]; Xf(Rm, p4, c);
                    *ex = 0; *ey = 0;
                    if (c[3] > 1e-6) { *ex = ((c[0] / c[3]) * 0.5 + 0.5 - u) * W; *ey = ((0.5 - (c[1] / c[3]) * 0.5) - vv) * H; }
                };
                double ex, ey; mvOf(Rexp[o], &ex, &ey);
                const double gx = H2F(mvRow[x * 2]), gy = H2F(mvRow[x * 2 + 1]);
                double err = std::hypot(gx - ex, gy - ey);
                double tol = 0.03 + snapPx + std::max(std::fabs(ex), std::fabs(ey)) / 1024.0;
                if (hasAlt[o]) {
                    double ax, ay; mvOf(Ralt[o], &ax, &ay);
                    const double ea = std::hypot(gx - ax, gy - ay), ta = 0.03 + snapPx + std::max(std::fabs(ax), std::fabs(ay)) / 1024.0;
                    if (ea / ta < err / tol) { err = ea; tol = ta; }
                }
                ++mvChecked; ++pxAll[o];
                const int g = objs[o].group;
                if (err > worstMv[g]) worstMv[g] = err;
                bool good = err <= tol;
                // v0.10.0 phase 14 (rstatic): an EXACT-DEPTH TIE lost by a skipped draw. Where a static draw S and a later-drawn
                // draw M have the same depth (their intersection line), the game keeps S (GREATER: the first one wins) and the full
                // replay gives S the pixel (reverse order); with S skipped, M's id (and motion) stays there. Classified exactly
                // (S skipped, the pixel's id = a replayed draw drawn after S), counted, not an MV failure -- a documented limit.
                if (!good && g_rstatic && !fwWin && tabIdx[o] >= 0 && tabIdx[o] < nd && rec.StaticSkippedAt(tabIdx[o])) {
                    if (tieFrame != f) {                                  // one id readback per frame, only when needed
                        tieFrame = f;
                        if (!tieSt) MakeTex(dev, W, H, DXGI_FORMAT_R16G16_UINT, 0, &tieSt, true);
                        ctx->CopyResource(tieSt.Get(), r.id2.Get());
                        tieMap = {};
                        if (FAILED(ctx->Map(tieSt.Get(), 0, D3D11_MAP_READ, 0, &tieMap))) tieMap.pData = nullptr;
                    }
                    uint32_t idr = 0;
                    if (tieMap.pData) idr = ((const uint16_t*)((const uint8_t*)tieMap.pData + y * tieMap.RowPitch))[x * 2];
                    if (idr >= 1 && (int)idr <= nd && (int)idr - 1 > tabIdx[o] && !rec.StaticSkippedAt((int)idr - 1)) {
                        good = true; ++tiePx; ++tiePxFrame;
                    }
                }
                if (good) ++pxOk[o]; else ++mvFails;
                ++grpPx[g]; if (good) ++grpOk[g];
                if (g >= 10) {                                            // phase 5: would the camera R alone be wrong here?
                    double cx, cy; mvOf(RcamG, &cx, &cy);
                    double ex2, ey2; mvOf(Rexp[o], &ex2, &ey2);
                    if (std::hypot(cx - ex2, cy - ey2) > 0.03 + snapPx + std::max(std::fabs(ex2), std::fabs(ey2)) / 1024.0) ++camWrong[g];
                }
                if (f == 1 || drawnRun[o] == 2) { ++grpPx1[g]; if (good) ++grpOk1[g]; }   // the frames only the attach gets right
            }
        }
        for (int o = 0; o < nObj; ++o) {
            if (!pxAll[o]) continue;
            ++checks;
            if (pxOk[o] * 1000 < pxAll[o] * 995) {
                ++fails; ++objFramesBad[o];
                if (objFramesBad[o] <= 2)
                    printf("   FAIL frame %d: %s #%d MV right on %d of %d px (state %d)\n", f, objs[o].name, o, pxOk[o], pxAll[o], stCur[o]);
            }
        }
        if (tieMap.pData) { ctx->Unmap(tieSt.Get(), 0); tieMap.pData = nullptr; }   // v0.10.0 phase 14 (rstatic)
        if (tiePxFrame > tieMaxFrame) tieMaxFrame = tiePxFrame;
        ctx->Unmap(v.stMv.Get(), 0); ctx->Unmap(v.stDepth.Get(), 0); ctx->Unmap(v.stTruth.Get(), 0);
        ctx->Unmap(E.stR.Get(), 0); ctx->Unmap(r.stFwd.Get(), 0); ctx->Unmap(r.stFTruth.Get(), 0); ctx->Unmap(r.stTwin.Get(), 0);
        if (g_debugLayer) { const int dl = DrainDebugLayer("S7"); ++checks; if (dl) { ++fails; debugMsgs += dl; } }
        for (int o = 0; o < nObj; ++o) { mvpPrev[o] = mvpCur[o]; drawnPrev[o] = drawnCur[o]; }
    }
    rec.Shutdown(); fwdRec.Shutdown();
    const DrawIdMv::Stats& ds = cam.DrawIds().GetStats();
    PrintPhase8(ctx, ds, "");
    if (g_rstatic) {                                    // v0.10.0 phase 14: the gate must have skipped the static scenery
        printf("   static replay skip (every %u): skipped %lld of %lld G-buffer draws; exact-depth ties lost by a skipped draw: %lld px "
               "(most in one frame %d, limit %d)\n", g_rstatic, rsSkip, rsSkip + rsRep, tiePx, tieMaxFrame, kTieMaxFrame);
        checks += 2;
        if (rsSkip == 0) { ++fails; printf("   FAIL static replay skip: the gate never skipped a draw\n"); }
        if (tieMaxFrame > kTieMaxFrame) { ++fails; printf("   FAIL static replay skip: %d tie px in one frame\n", tieMaxFrame); }
    }
    checks += 4;
    if (consFrames < frames - 3) { ++fails; printf("   FAIL consensus used in only %d of %d frames\n", consFrames, frames); }
    if (!(grpPx[6] > 3000 && grpPx[7] > 1000 && grpPx[8] > 3000 && grpPx[9] > 500)) {
        ++fails; printf("   FAIL plate coverage too small: shared %lld, bracket %lld, H1 %lld, decal %lld px\n", grpPx[6], grpPx[7],
                        grpPx[8], grpPx[9]);
    }
    // phase 5: the shared / bracket plates carry their body's matrix, so the twin takes them before the origin attach; a forward
    // draw took a body's R per draw either way (twin, or attach with the twins off)
    if (!(mutNoAttach && mutNoTwin) && !(ds.fwdAttached + ds.twinFwd > 0)) {
        ++fails; printf("   FAIL no forward draw ever took a body's R per draw (attach %llu, twin %llu)\n",
                        (unsigned long long)ds.fwdAttached, (unsigned long long)ds.twinFwd);
    }
    if (!mutNoAttach && !(ds.fwdAttachPx > 0)) { ++fails; printf("   FAIL no forward pixel was ever attached in pass B\n"); }
    // ---- v0.10.0 phase 5 scene checks ----
    checks += 6;
    if (!(grpPx[11] > 1500 && grpPx[12] > 1500 && grpPx[13] > 500 && grpPx[14] > 1500 && grpPx[16] > 3000 && grpPx[18] > 3000 &&
          grpPx[19] > 3000)) {
        ++fails; printf("   FAIL phase-5 coverage too small: H1-body plates %lld / %lld, lamp %lld, 12 G-buffer plates %lld, sign face %lld, "
                        "OV plates %lld / %lld px\n", grpPx[11], grpPx[12], grpPx[13], grpPx[14], grpPx[16], grpPx[18], grpPx[19]);
    }
    if (!(camWrong[11] > 300 && camWrong[12] > 300 && camWrong[13] > 100 && camWrong[14] > 100 && camWrong[18] > 300)) {
        ++fails; printf("   FAIL the scene does not exercise the inheritance (px the camera R would get wrong: H1-body plates %lld / %lld, "
                        "lamp %lld, 12 G-buffer plates %lld, OV G-buffer plate %lld)\n", camWrong[11], camWrong[12], camWrong[13],
                        camWrong[14], camWrong[18]);
    }
    if (decoyNear < 1) { ++fails; printf("   FAIL the H1 body's moving pivot never passed within 0.5 m of the decoy (%d frames)\n", decoyNear); }
    if (ovBehind < frames - 1) { ++fails; printf("   FAIL OV's pivot was behind the camera plane in only %d of %d frames\n", ovBehind, frames); }
    if (!mutNoTwin && !(ds.twins > 0 && ds.twinMovers > 0)) {
        ++fails; printf("   FAIL no unpaired draw ever took its matrix twin's R (twins %llu, movers %llu)\n", (unsigned long long)ds.twins,
                        (unsigned long long)ds.twinMovers);
    }
    if (!mutNoAttach && !(ds.gbufAttached > 0)) { ++fails; printf("   FAIL no G-buffer draw was ever attached by origin (the lamp)\n"); }
    ++checks;                                                     // the Alt+F8 dump path ran and landed (lines: dlaa_inject.log)
    if (ds.dumps != 1) { ++fails; printf("   FAIL the per-draw dump requested at frame 50 landed %llu times (want 1)\n", (unsigned long long)ds.dumps); }
    printf("   camera R vs exact: worst %.4f px (limit 0.05), bad frames %d; consensus in %d of %d frames\n", worstCamPx, camRBad,
           consFrames, frames);
    printf("   plate px right (all frames / first paired + reappearance frames): shared 12 %lld/%lld (%lld/%lld), bracket %lld/%lld "
           "(%lld/%lld), H1 camera-VP %lld/%lld (%lld/%lld), static decal %lld/%lld; vehicles %lld/%lld\n",
           grpOk[6], grpPx[6], grpOk1[6], grpPx1[6], grpOk[7], grpPx[7], grpOk1[7], grpPx1[7], grpOk[8], grpPx[8], grpOk1[8],
           grpPx1[8], grpOk[9], grpPx[9], grpOk[3] + grpOk[4] + grpOk[5], grpPx[3] + grpPx[4] + grpPx[5]);
    printf("   attach: forward draws attached per draw %llu, kept (agreed) %llu, forward px attached in pass B %llu (1-in-64 grid); "
           "forward draws paired %llu, movers %llu (read back)\n", (unsigned long long)ds.fwdAttached,
           (unsigned long long)ds.fwdAttachKept, (unsigned long long)ds.fwdAttachPx, (unsigned long long)ds.fwdPaired,
           (unsigned long long)ds.fwdMovers);
    printf("   MV: %d of %lld pixels outside tolerance; worst MV error per group (px): ground %.3f box %.3f post %.3f vehicle %.3f "
           "bracket vehicle %.3f H1 body %.3f shared plate %.3f bracket plate %.3f H1 plate %.3f decal %.3f\n", mvFails, mvChecked,
           worstMv[0], worstMv[1], worstMv[2], worstMv[3], worstMv[4], worstMv[5], worstMv[6], worstMv[7], worstMv[8], worstMv[9]);
    printf("   phase 5 px right (all frames / first paired + reappearance frames) [px the camera R alone gets wrong]: H1-body G-buffer "
           "plate %lld/%lld [%lld], H1-body forward plate %lld/%lld [%lld], lamp node %lld/%lld [%lld], 12 G-buffer plates %lld/%lld "
           "(%lld/%lld) [%lld], sign face %lld/%lld, OV G-buffer plate %lld/%lld [%lld], OV forward plate %lld/%lld [%lld], H1 body %lld/%lld, "
           "OV %lld/%lld\n", grpOk[11], grpPx[11], camWrong[11], grpOk[12], grpPx[12], camWrong[12], grpOk[13], grpPx[13],
           camWrong[13], grpOk[14], grpPx[14], grpOk1[14], grpPx1[14], camWrong[14], grpOk[16], grpPx[16], grpOk[18], grpPx[18],
           camWrong[18], grpOk[19], grpPx[19], camWrong[19], grpOk[5], grpPx[5], grpOk[17], grpPx[17]);
    printf("   phase 5 inheritance (read back): twins %llu (movers %llu, forward %llu) of %llu unpaired draws looked up; G-buffer "
           "draws attached by origin %llu; sign face static in %d of %d frames; decoy: the H1 body's pivot within 0.5 m in %d frames, "
           "took a mover's R %d times; OV's pivot behind the camera in %d of %d frames; worst MV error: H1-body plates %.3f / %.3f, lamp "
           "%.3f, G-buffer plates %.3f, sign face %.3f, OV plates %.3f / %.3f, decoy %.3f\n", (unsigned long long)ds.twins,
           (unsigned long long)ds.twinMovers, (unsigned long long)ds.twinFwd, (unsigned long long)ds.twinLookups,
           (unsigned long long)ds.gbufAttached, faceChecked - faceBad, faceChecked, decoyNear, decoyBad, ovBehind, frames,
           worstMv[11], worstMv[12], worstMv[13], worstMv[14], worstMv[16], worstMv[18], worstMv[19], worstMv[20]);
    if (g_debugLayer) printf("   debug layer: %d error / warning messages\n", debugMsgs);
    printf("   %s: %d / %d checks passed\n", fails ? "FAILED" : "passed", checks - fails, checks);
    DrawIdMv::SetAttach(attachSaved);
    DrawIdMv::SetTwin(twinSaved);
    tot.checks += checks; tot.fails += fails;
    return fails;
}

// ---- v0.10.0 phase 6: S8 RIGID PARENTS -----------------------------------------------------------------------------------
// The in-game plates still unpaired after phase 5 carry the bare PROJECTION in cb0 rows 4..7: the CPU writes their vertices in
// VIEW space every frame (keys new every frame, no matrix names the vehicle). S8 reproduces exactly that on a TURNING trailer
// (yaw changes every frame, so a plate 6.5 m behind the pivot sweeps sideways): a G-buffer plate background and the plate's
// text inside it (both view-space, keys new every frame; the text's neighbours are only the background = a chain through an
// unpaired neighbour), a FORWARD plate drawn with its own node matrix (MVP_trailer * O, O = 6.5 m behind the pivot -- neither
// a twin nor an origin can name the trailer), a parked trailer with a view-space plate (must come out STATIC), and a static
// view-space gantry sign placed so that a passing truck's silhouette comes within 0..5 px of it at ~1.5x the truck's depth
// (must NOT take the truck's motion: the vote's depth test). Truth: every plate moves exactly with its body (R_plate = R_body
// for a rigidly fixed node); the sign is static. Mutations: "noparent" (mv_drawid_parent 0), "parentconj" (the row-vector
// formula MVP_U * inv(MVP_P) * MVP_P_prev * inv(MVP_U) typed into the column-vector code), "nodepth" (no depth test in the
// vote) -- each must FAIL S8.
// v0.10.0 phase 6c (MARCHING vote): + a FAR view-space plate 24x8 px on a weaving van ~24 m ahead, an INSTANCED plate background
// with INSTANCED text fully inside it on the turning trailer's free top band (two instanced draws: the text marches through the
// background), and an INSTANCED plate on the turning trailer's BOTTOM edge (the road below it, seen under the trailer, is ~12 % deeper: those marches
// are depth-rejected, the body above / beside must still win). Every plate pixel must move with its body; "noparent" fails them.
// The roadside grass moved to x = -12.5 (clear of the far van). reachPx > 0 (the "S8 short reach" run): the march reach is cut
// to reachPx so the plate text inside its background can only find the trailer through the 2nd pass (via the background).
int RunParentScene(Env& E, float snapPx, Totals& tot, int mut, unsigned reachPx = 0, Shadow* const* sh = nullptr, int nSh = 0) {
    const int frames = 100;
    printf("\n===== S8 rigid parents: view-space plate + text (chain) and a node-matrix forward plate on a TURNING trailer, a "
           "parked trailer's view-space plate, a static view-space sign next to a passing truck; phase 6c: far 24x8 px plate, "
           "instanced background + text, plate on the trailer's bottom edge (%d frames, snap %.3f px%s%s) =====\n",
           frames, (double)snapPx, mut == 1 ? ", MUTATION: parents off" : (mut == 2 ? ", MUTATION: conjugated formula" :
                                                                         (mut == 3 ? ", MUTATION: no depth test" : "")),
           reachPx ? ", SHORT march reach (2nd pass)" : "");
    DrawIdMv::SetParentReach(reachPx);                  // v0.10.0 phase 6c: 0 = the default reach
    ID3D11Device* dev = E.dev.Get();
    ID3D11DeviceContext* ctx = E.ctx.Get();
    ID3D11DeviceContext1* ctx1 = E.ctx1.Get();
    DrawIdMv::SetParams(8.0f, snapPx);
    DrawIdMv::SetConsensus(true);
    const float attachSaved = DrawIdMv::AttachM();
    const bool twinSaved = DrawIdMv::Twin(), parentSaved = DrawIdMv::Parent(), instSaved = DrawIdMv::Instanced();
    DrawIdMv::SetAttach(0.5f);
    DrawIdMv::SetTwin(true);
    DrawIdMv::SetParent(mut != 1);
    DrawIdMv::SetParentMutation(mut == 2 ? 1 : (mut == 3 ? 2 : 0));
    DrawIdMv::SetInstanced(true);                       // v0.10.0 phase 6b: instanced draws join the vote in S8 (S1-S7 leave it off)
    View v;
    S6Res r;
    if (!MakeView(E, v, W, H)) { printf("view textures failed\n"); return 1; }
    {
        bool ok = MakeTex(dev, W, H, DXGI_FORMAT_R16G16_UINT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &r.id2) &&
                  SUCCEEDED(dev->CreateRenderTargetView(r.id2.Get(), nullptr, &r.id2Rtv)) &&
                  SUCCEEDED(dev->CreateShaderResourceView(r.id2.Get(), nullptr, &r.id2Srv)) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, &r.fwd) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32_TYPELESS, 0, &r.stFwd, true) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32G32_UINT, D3D11_BIND_RENDER_TARGET, &r.fTruth) &&
                  SUCCEEDED(dev->CreateRenderTargetView(r.fTruth.Get(), nullptr, &r.fTruthRtv)) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32G32_UINT, 0, &r.stFTruth, true) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_D32_FLOAT, D3D11_BIND_DEPTH_STENCIL, &r.fTruthDepth) &&
                  SUCCEEDED(dev->CreateDepthStencilView(r.fTruthDepth.Get(), nullptr, &r.fTruthDsv)) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32G8X24_TYPELESS, 0, &r.stTwin, true) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, &r.scratch) &&
                  SUCCEEDED(dev->CreateRenderTargetView(r.scratch.Get(), nullptr, &r.scratchRtv));
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{}; dd.Format = DXGI_FORMAT_D32_FLOAT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        ok = ok && SUCCEEDED(dev->CreateDepthStencilView(r.fwd.Get(), &dd, &r.fwdDsv)) &&
             SUCCEEDED(dev->CreateShaderResourceView(r.fwd.Get(), &sd, &r.fwdSrv));
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; ds.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        ds.StencilEnable = FALSE; ds.StencilReadMask = 0xFF; ds.StencilWriteMask = 0xFF;
        ds.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
        ds.BackFace = ds.FrontFace;
        ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&ds, &r.dssFwdGame));
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&ds, &r.dssFwdRe));
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &r.bsOver));
        bd.AlphaToCoverageEnable = TRUE;
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].RenderTargetWriteMask = 0;
        ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &r.bsA2C));
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &r.bsA2CAll));
        ComPtr<ID3DBlob> pf = Compile("PSFwd", "ps_5_0"), pt = Compile("PSFwdTruth", "ps_5_0");
        ok = ok && pf && pt && SUCCEEDED(dev->CreatePixelShader(pf->GetBufferPointer(), pf->GetBufferSize(), nullptr, &r.psFwd)) &&
             SUCCEEDED(dev->CreatePixelShader(pt->GetBufferPointer(), pt->GetBufferSize(), nullptr, &r.psFwdTruth));
        if (!ok) { printf("S8 resources failed\n"); return 1; }
    }
    // draws whose KEYS CHANGE EVERY FRAME (as S7): kDyn quads, 16 slots in 3 DYNAMIC vertex buffers rewritten every frame (the
    // view-space vertices move every frame) + an index buffer with 8 copies of the quad's indices
    enum { Q_BG, Q_TX, Q_FP, Q_PP, Q_SIGN, Q_INSTP, Q_GRASS,          // v0.10.0 phase 6b: Q_INSTP / Q_GRASS = instanced draws
           Q_FAR, Q_TBG, Q_TTX, Q_BOT,                                // v0.10.0 phase 6c
           Q_GN0, Q_GN1, Q_GN2, Q_GN3, kDyn };                       // phase 14 round 4: instanced grass beside the passing truck
    // half sizes (m): the text sits inside the background with > 5 px of background around it (its +-2 / +-5 px samples see
    // only the background = a chain through an unpaired neighbour). Phase 6c: the far plate is 24 x 8 px at ~24 m (focal
    // 324 px); the trailer's instanced text lies fully inside its instanced background; the bottom plate's lower edge is the
    // trailer's bottom edge (y = 0.2)
    const float quadHalf[kDyn][2] = { { 0.7f, 0.4f }, { 0.3f, 0.1f }, { 0.35f, 0.15f }, { 0.6f, 0.3f }, { 1.0f, 0.6f },
                                      { 0.6f, 0.35f }, { 0.5f, 0.5f },
                                      { 0.889f, 0.296f }, { 0.9f, 0.2f }, { 0.5f, 0.1f }, { 0.5f, 0.15f },
                                      { 0.35f, 0.35f }, { 0.35f, 0.35f }, { 0.35f, 0.35f }, { 0.35f, 0.35f } };
    ComPtr<ID3D11Buffer> dynPool[3], dynIb;
    {
        D3D11_BUFFER_DESC bd{}; bd.Usage = D3D11_USAGE_DYNAMIC; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = (UINT)(16 * kDyn * 4 * sizeof(Vtx)); bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        for (int k = 0; k < 3; ++k) if (FAILED(dev->CreateBuffer(&bd, nullptr, &dynPool[k]))) { printf("S8 dyn VB failed\n"); return 1; }
        std::vector<uint32_t> idx;
        for (int s = 0; s < 8; ++s) for (uint32_t q : { 0u, 1u, 2u, 0u, 2u, 3u }) idx.push_back(q);
        D3D11_BUFFER_DESC ib{}; ib.Usage = D3D11_USAGE_DEFAULT; ib.ByteWidth = (UINT)(idx.size() * 4); ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{ idx.data(), 0, 0 };
        if (FAILED(dev->CreateBuffer(&ib, &sd, &dynIb))) { printf("S8 dyn IB failed\n"); return 1; }
    }
    CameraMv cam;
    if (!cam.Init(dev)) { printf("CameraMv init failed\n"); return 1; }
    cam.SetEgoPixel(0.0f);
    cam.SetEgoOrigin(0.0f);
    CandidateRecord cand;
    cand.Init(dev);
    DrawIdRecord rec, fwdRec;
    fwdRec.SetForward(true);

    // ---- objects ----
    std::vector<Obj> objs;
    std::vector<char> isFwd, viewSpace, instObj;        // v0.10.0 phase 6b: instObj = drawn with DrawIndexedInstanced
    std::vector<int> bodyOf, dynOf;
    auto add = [&](const char* n, Kind k, int mesh, bool cn, int group, bool fwd = false, int body = -1, int dyn = -1,
                   bool vs = false, bool inst = false) {
        objs.push_back({ n, k, mesh, 0, cn, false, 0, group }); isFwd.push_back(fwd ? 1 : 0); bodyOf.push_back(body);
        dynOf.push_back(dyn); viewSpace.push_back(vs ? 1 : 0); instObj.push_back(inst ? 1 : 0);
        return (int)objs.size() - 1; };
    const int oGround = add("ground", kStatic, MGround, true, 0);
    int oBox[10];                                                // 10 unique static meshes: pass A's medoid needs unique keys
    for (int k = 0; k < 10; ++k) oBox[k] = add("static box (10 unique)", kStatic, MBox0 + k, false, 1);
    int oPost[50];
    for (int k = 0; k < 50; ++k) oPost[k] = add("roadside post (50 identical)", kStatic, MPost, false, 2);
    const int oTrl = add("TURNING trailer (pivot 6.5 m ahead of its rear face)", kMover, MTrl6, false, 3);
    const int oBg = add("G-BUFFER plate background, VIEW-SPACE vertices (MVP = projection), keys new every frame", kMover, 0, true, 4,
                        false, oTrl, Q_BG, true);
    const int oTx = add("G-BUFFER plate text inside the background, VIEW-SPACE (chain: its neighbours are the background)", kMover, 0,
                        true, 5, false, oTrl, Q_TX, true);
    const int oFp = add("FORWARD plate on its own NODE matrix (MVP_trailer * O, 6.5 m behind the pivot), keys new every frame",
                        kMover, 0, true, 6, true, oTrl, Q_FP, false);
    const int oPark = add("PARKED trailer", kStatic, MTrl6P, false, 7);   // its own mesh (no identical-copy group with the trailer)
    const int oPp = add("parked trailer's G-buffer plate, VIEW-SPACE (must come out static)", kStatic, 0, true, 8, false, oPark, Q_PP,
                        true);
    const int oTruck = add("passing truck", kMover, MTruck, false, 9);
    const int oSign = add("static gantry sign, VIEW-SPACE, next to the passing truck at ~1.5x its depth (must stay static)", kStatic, 0,
                          true, 10, false, -1, Q_SIGN, true);
    // v0.10.0 phase 6b: an INSTANCED (DrawIndexedInstanced, 1 instance) view-space plate on the turning trailer -- it has no own
    // matrix (kFInst), so only the rigid-parent vote can give it the trailer's motion; and an INSTANCED roadside grass clump,
    // away from any mover, that must stay static (camera R).
    const int oInstP = add("INSTANCED G-buffer plate on the trailer (DrawIndexedInstanced, view-space; must take the trailer's R)",
                           kMover, 0, true, 11, false, oTrl, Q_INSTP, true, true);
    const int oGrass = add("INSTANCED roadside grass (DrawIndexedInstanced; must stay static / camera R)", kStatic, 0, true, 12,
                           false, -1, Q_GRASS, true, true);   // cn = true (cullNone) like the other view-space quads
    // v0.10.0 phase 6c: the marching vote's cases (groups 13..17)
    const int oFar = add("far van (weaving, ~24 m ahead, left lane)", kMover, MFarVan, false, 13);
    const int oFarP = add("FAR plate 24x8 px on the far van, VIEW-SPACE G-buffer (must take the van's R)", kMover, 0, true, 14,
                          false, oFar, Q_FAR, true);
    const int oTkBg = add("INSTANCED plate background on the trailer's top band, VIEW-SPACE (must take the trailer's R)", kMover, 0, true,
                          15, false, oTrl, Q_TBG, true, true);
    const int oTkTx = add("INSTANCED plate text fully inside that background, VIEW-SPACE (must take the trailer's R)", kMover, 0,
                          true, 16, false, oTrl, Q_TTX, true, true);
    const int oBot = add("INSTANCED plate on the trailer's BOTTOM edge, road below at another depth (must take the trailer's R)",
                         kMover, 0, true, 17, false, oTrl, Q_BOT, true, true);
    // v0.10.0 phase 14 round 4: four INSTANCED grass clumps on the roadside right beside the passing truck's left face (x 2.35;
    // the clumps end at x 2.25, 10 cm off it), standing on the static ground -- the truck passes them within a few px at their
    // depth. Grass is never ON the truck: they must keep the camera R in every frame (with rstatic also while the ground under
    // them is skipped = the pseudo static source)
    int oGn[4];
    for (int k = 0; k < 4; ++k)
        oGn[k] = add("INSTANCED grass beside the passing truck (must stay static / camera R)", kStatic, 0, true, 18, false, -1,
                     Q_GN0 + k, true, true);
    const int nObj = (int)objs.size();
    (void)oGround;
    std::vector<M4> mvpPrev(nObj, I4()), mvpCur(nObj, I4());
    std::vector<bool> drawnPrev(nObj, false);
    std::vector<int> stCur(nObj, -1), objFramesBad(nObj, 0);
    int checks = 0, fails = 0, camRBad = 0, consFrames = 0, mvFails = 0, debugMsgs = 0;
    long long mvChecked = 0, grpPx[24] = {}, grpOk[24] = {}, camWrong[24] = {};      // phase 6c: 16 -> 24 groups
    double worstCamPx = 0.0, worstMv[24] = {};
    // per-draw state checks: frames with the rigid-parent mark + the right parent, state failures
    int parOk[24] = {}, parFrames[24] = {}, signNear = 0, signMover = 0, parkMover = 0, parkStatic = 0, parkFrames = 0;
    int grassMover = 0;                                 // v0.10.0 phase 6b: frames the instanced grass was a MOVER (want 0)
    int grassNearMover = 0, grassNearFrames = 0;        // phase 14 round 4: grass beside the truck as a MOVER (want 0) / truck near
    constexpr int kHoldFrame = 50;                      // phase 14 round 4: the trailer is not replayed in this frame
    int holdOk = 0, holdChecked = 0;
    long long s8Skip = 0, s8Rep = 0;                    // v0.10.0 phase 14 (rstatic): draws the static gate skipped / replayed
    if (g_rstatic && !nSh) DrawIdMv::StaticClear(DrawIdMv::kStatClrConfig);
    double yawRate = 0.0;
    std::mt19937 rng(808);
    const FLOAT zero[4] = { 0, 0, 0, 0 };
    const D3D11_RECT sc0 = { 0, 0, W, H };
    M4 VprevX = I4();
    const M4 Pw = Proj(kNearWorld);
    GpuPerf::Restart();
    for (int f = 0; f < frames; ++f) {
        PerfFrame();
        const M4 camPose = Mul(T(0.0, 1.6, 0.5 * f), RotY(0.001 * f));
        M4 V; Inv(camPose, V);
        std::vector<M4> world(nObj, I4());
        world[oGround] = T(0, 0, 100);
        for (int k = 0; k < 10; ++k) world[oBox[k]] = T((k & 1) ? 14.0 + k : -14.0 - k, 0.0, 60.0 + 25.0 * k);
        for (int k = 0; k < 50; ++k) world[oPost[k]] = T((k & 1) ? 9.5 : -9.5, 0.0, 40.0 + 12.0 * (k / 2));
        // the trailer: ~12 m ahead (rear face), weaving and TURNING (yaw up to +-0.15 rad, ~0.009 rad / frame)
        const double yaw = 0.15 * sin(0.06 * f);
        if (f > 0) yawRate = std::max(yawRate, std::fabs(yaw - 0.15 * sin(0.06 * (f - 1))));
        const M4 trl = Mul(T(-0.5 + 0.8 * sin(0.04 * f), 0.0, 0.5 * f + 18.5 + 1.0 * sin(0.05 * f)), RotY(yaw));
        world[oTrl] = trl;
        world[oBg] = Mul(trl, T(-0.5, 1.0, -6.52));         // the plate nodes: 2 / 3 cm behind the rear face (z = -6.5)
        world[oTx] = Mul(trl, T(-0.5, 1.0, -6.53));
        world[oFp] = Mul(trl, T(0.75, 1.0, -6.52));
        world[oPark] = T(12.0, 0.0, 75.0);
        world[oPp] = Mul(world[oPark], T(0.0, 1.0, -6.42));        // its rear face is at z = -6.4
        world[oTruck] = T(3.6, 0.0, 0.5 * f + 14.0 + 0.25 * f);           // rear face 5 m behind the pivot
        world[oSign] = T(5.4, 4.35, 31.0);                                  // on the ray from the camera through the truck's
                                                                            // rear top edge at frame 20, at 1.5x its distance
        world[oInstP] = Mul(trl, T(0.0, 2.1, -6.52));   // v0.10.0 phase 6b: upper rear face of the trailer, clear of oBg / oFp
        world[oGrass] = T(-12.5, 0.8, 26.0);            // v0.10.0 phase 6b: roadside, above the static ground, far left of traffic
                                                        // (phase 6c: x -9 -> -12.5, clear of the far van's lane)
        // v0.10.0 phase 6c: the far van ~24 m ahead in the left lane (weaving, z-oscillating, yawing: its own R), its 24x8 px plate;
        // the instanced background + text on the trailer's free top band (y 2.52..2.92); the plate on its bottom edge
        const M4 farVan = Mul(T(-7.6 + 0.3 * sin(0.2 * f), 0.0, 0.5 * f + 26.5 + 1.0 * sin(0.05 * f)), RotY(0.06 * sin(0.07 * f)));
        world[oFar] = farVan;
        world[oFarP] = Mul(farVan, T(0.0, 1.4, -2.52));
        world[oTkBg] = Mul(trl, T(0.0, 2.72, -6.52));
        world[oTkTx] = Mul(trl, T(0.0, 2.72, -6.53));
        world[oBot] = Mul(trl, T(0.6, 0.35, -6.52));
        for (int k = 0; k < 4; ++k) world[oGn[k]] = T(1.9, 0.35, 24.0 + 7.0 * k);   // phase 14 round 4 (static, beside the truck)
        for (int o = 0; o < nObj; ++o) mvpCur[o] = Mul(Pw, Mul(V, world[o]));
        // the dyn quads: view-space vertices (V * world * corner) or node-frame vertices (forward plate)
        const int slotBase = (5 * f) % 16;
        {
            D3D11_MAPPED_SUBRESOURCE mp{};
            if (FAILED(ctx->Map(dynPool[f % 3].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { printf("dyn VB map failed\n"); return 1; }
            Vtx* vv = (Vtx*)mp.pData;
            for (int o = 0; o < nObj; ++o) {
                const int q = dynOf[o];
                if (q < 0) continue;
                const M4 toV = Mul(V, world[o]);
                const int base = (((slotBase + q) % 16) * kDyn + q) * 4;
                for (int c = 0; c < 4; ++c) {
                    const double su = (c == 1 || c == 2) ? 1.0 : -1.0, sv = c >= 2 ? 1.0 : -1.0;
                    const double p4[4] = { su * quadHalf[q][0], sv * quadHalf[q][1], 0.0, 1.0 };
                    double w4[4] = { p4[0], p4[1], p4[2], 1.0 };
                    if (viewSpace[o]) Xf(toV, p4, w4);
                    Vtx x{}; x.p[0] = (float)w4[0]; x.p[1] = (float)w4[1]; x.p[2] = (float)w4[2];
                    x.uv[0] = (c == 1 || c == 2) ? 1.f : 0.f; x.uv[1] = c >= 2 ? 0.f : 1.f;
                    vv[base + c] = x;
                }
            }
            ctx->Unmap(dynPool[f % 3].Get(), 0);
        }
        std::vector<int> order, forder;
        for (int o = 0; o < nObj; ++o) { if (isFwd[o]) forder.push_back(o); else order.push_back(o); }
        std::shuffle(order.begin(), order.end(), rng);
        const int nd = (int)order.size(), nf = (int)forder.size();
        ID3D11Buffer* ring = E.ring[f & 1].Get();
        {
            D3D11_MAPPED_SUBRESOURCE mp{};
            if (FAILED(ctx->Map(ring, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { printf("ring map failed\n"); return 1; }
            float* p = (float*)mp.pData;
            memset(p, 0, 2u << 20);
            for (int d = 0; d < nd + nf; ++d) {
                const int o = d < nd ? order[d] : forder[d - nd];
                const M4& m = viewSpace[o] ? Pw : mvpCur[o];               // view space: rows 4..7 = the bare projection
                float* w = p + (UINT)d * 64u;
                for (int rr = 0; rr < 4; ++rr) for (int c = 0; c < 4; ++c) w[(4 + rr) * 4 + c] = (float)m.m[rr][c];
                w[8 * 4 + 0] = (float)(o + 1); w[8 * 4 + 1] = (float)((d < nd ? d : d - nd) + 1); w[8 * 4 + 2] = 0.0f;
            }
            ctx->Unmap(ring, 0);
        }
        auto halton = [](int i, int b) { double q = 0, fct = 1; while (i > 0) { fct /= b; q += fct * (i % b); i /= b; } return q; };
        const float jx = (float)(halton(f % 8 + 1, 2) - 0.5), jy = (float)(halton(f % 8 + 1, 3) - 0.5);
        const D3D11_VIEWPORT vpW = { jx, jy, (float)(W + g_vpOver), (float)(H + g_vpOver), 0.01f, 0.9f };
        auto bindDraw = [&](int o, int d) {
            const Obj& ob = objs[o];
            ctx->RSSetViewports(1, &vpW);
            ctx->RSSetScissorRects(1, &sc0);
            ctx->RSSetState(ob.cullNone ? E.rsNone.Get() : E.rsBack.Get());
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11Buffer* vbs[2] = { dynOf[o] >= 0 ? dynPool[f % 3].Get() : E.vb[0].Get(), E.instVb[0].Get() };
            const UINT strides[2] = { sizeof(Vtx), 12 }, offs[2] = { 0, 0 };
            ctx->IASetVertexBuffers(0, 2, vbs, strides, offs);
            ctx->IASetIndexBuffer(dynOf[o] >= 0 ? dynIb.Get() : E.ib[0].Get(), DXGI_FORMAT_R32_UINT, 0);
            ID3D11ShaderResourceView* vsSrv[4] = {};
            ctx->VSSetShaderResources(0, 4, vsSrv);
            ctx->IASetInputLayout(E.il.Get());
            ctx->VSSetShader(E.vsMain.Get(), nullptr, 0);
            const UINT first = (UINT)d * 16u, num = 16u;
            ctx1->VSSetConstantBuffers1(0, 1, &ring, &first, &num);
            ctx1->PSSetConstantBuffers1(0, 1, &ring, &first, &num);
        };
        auto drawArgs = [&](int o, UINT* ic, UINT* si, INT* bv) {
            if (dynOf[o] >= 0) {                                         // keys new every frame (VB, startIndex, baseVertex)
                *ic = 6; *si = (UINT)(((3 * f) % 8) * 6);
                *bv = (INT)((((slotBase + dynOf[o]) % 16) * kDyn + dynOf[o]) * 4);
                return;
            }
            const Mesh& m = E.meshes[objs[o].mesh];
            *ic = m.ic; *si = m.si; *bv = m.bv;
        };
        // ---- G-buffer ----
        ctx->ClearDepthStencilView(v.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
        ctx->ClearRenderTargetView(v.truthRtv.Get(), zero);
        ctx->ClearRenderTargetView(v.colorRtv.Get(), zero);
        ID3D11RenderTargetView* rtvs[2] = { v.truthRtv.Get(), v.colorRtv.Get() };
        ctx->OMSetRenderTargets(2, rtvs, v.dsv.Get());
        ctx->OMSetDepthStencilState(E.dss.Get(), 1);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        ctx->PSSetShader(E.ps.Get(), nullptr, 0);
        rec.Reset(); fwdRec.Reset(); cand.Reset();
        cand.SetDropped(cam.MedoidDropBits());          // v0.10.0 phase 8 (mv_medoid_drop)
        for (int q = 0; q < nSh; ++q) sh[q]->BeginFrame();   // v0.10.0 phase 9 (S12)
        for (int d = 0; d < nd; ++d) {
            const int o = order[d];
            UINT ic, si; INT bv; drawArgs(o, &ic, &si, &bv);
            bindDraw(o, d);
            CandidateRecord::DrawKey k{};
            k.ib = dynOf[o] >= 0 ? dynIb.Get() : E.ib[0].Get(); k.vb = dynOf[o] >= 0 ? dynPool[f % 3].Get() : E.vb[0].Get();
            k.indexCount = ic; k.startIndex = si; k.baseVertex = bv;
            if (cand.Count(0) < CandidateRecord::kSlots && !cand.Dropped(0)) cand.Record(ctx, 0, ring, (UINT)d * 256u + 64u, k);
            const bool inst = instObj[o] != 0;                          // v0.10.0 phase 6b: instanced plate / grass
            // phase 14 round 4: the grass beside the truck is an 8-instance batch (vegetation; the instances overlap here)
            const UINT nInst = (o >= oGn[0] && o <= oGn[3]) ? 8u : 1u;
            rec.Record(ctx1, ic, nInst, si, bv, 0u, inst);
            for (int q = 0; q < nSh; ++q) sh[q]->OnGbufDraw(ctx, ctx1, ring, d, &k, ic, nInst, si, bv, inst);
            if (inst) ctx->DrawIndexedInstanced(ic, nInst, si, bv, 0u);
            else ctx->DrawIndexed(ic, si, bv);
        }
        // v0.10.0 phase 6b: replayAll = true so the instanced draws are replayed into the id target (they get an id and a vote)
        DrawIdMv::SetFrame((uint64_t)f);                // v0.10.0 phase 8 (mv_inst_replay: keep window, probe every 7th pass)
        DrawIdRecord::SetInstancedProbe((f % 7) == 0);
        if (g_rstatic && !nSh) { DrawIdMv::PollMain(ctx); DrawIdRecord::SetStaticGate(true); }   // v0.10.0 phase 14 (rstatic)
        // phase 14 round 4: at kHoldFrame the TRAILER is left out of the replay (its pixels missing for one frame): its plates must
        // keep its motion through the plate hold
        if (f == kHoldFrame && !nSh) for (int d = 0; d < nd; ++d) if (order[d] == oTrl) DrawIdRecord::SetTestSkipKey(rec.FullKey(d));
        if (!rec.Replay(ctx1, r.id2Rtv.Get(), v.roDsv.Get(), false, true).ok) { printf("   FAIL replay (frame %d)\n", f); ++fails; }
        DrawIdRecord::SetStaticGate(false);
        DrawIdRecord::SetTestSkipKey(0);
        if (g_rstatic && !nSh) { s8Skip += rec.StaticSkipped(); s8Rep += rec.ReplayedDraws(); }
        for (int q = 0; q < nSh; ++q) sh[q]->AfterGbuf(ctx1, v.roDsv.Get(), true);
        DrawIdRecord::SetInstancedProbe(true);
        // ---- forward pass (game draw + forward-depth re-draw + record), forward truth, forward replay ----
        ID3D11RenderTargetView* crt = v.colorRtv.Get();
        bool fwdCleared = false;
        for (int k = 0; k < nf; ++k) {
            const int o = forder[k];
            UINT ic, si; INT bv; drawArgs(o, &ic, &si, &bv);
            bindDraw(o, nd + k);
            ctx->OMSetRenderTargets(1, &crt, v.dsv.Get());
            ctx->OMSetDepthStencilState(r.dssFwdGame.Get(), 0);
            ctx->OMSetBlendState(r.bsOver.Get(), nullptr, 0xFFFFFFFFu);
            ctx->PSSetShader(r.psFwd.Get(), nullptr, 0);
            ctx->DrawIndexed(ic, si, bv);
            if (!fwdCleared) { ctx->ClearDepthStencilView(r.fwdDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0); fwdCleared = true; }
            ctx->OMSetRenderTargets(1, &crt, r.fwdDsv.Get());
            ctx->OMSetDepthStencilState(r.dssFwdRe.Get(), 0);
            ctx->OMSetBlendState(r.bsA2C.Get(), nullptr, 0xFFFFFFFFu);
            ctx->DrawIndexed(ic, si, bv);
            ctx->OMSetRenderTargets(1, &crt, v.dsv.Get());
            ctx->OMSetDepthStencilState(r.dssFwdGame.Get(), 0);
            ctx->OMSetBlendState(r.bsOver.Get(), nullptr, 0xFFFFFFFFu);
            for (int q = 0; q < nSh; ++q) sh[q]->OnFwdDraw(ctx, ctx1, r.dssFwdRe.Get(), r.bsA2C.Get(), ic, si, bv);
            fwdRec.Record(ctx1, ic, 1, si, bv, 0, false);
        }
        ctx->ClearRenderTargetView(r.fTruthRtv.Get(), zero);
        ctx->ClearDepthStencilView(r.fTruthDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0);
        {
            ID3D11RenderTargetView* tr[2] = { r.scratchRtv.Get(), r.fTruthRtv.Get() };
            for (int k = 0; k < nf; ++k) {
                const int o = forder[k];
                UINT ic, si; INT bv; drawArgs(o, &ic, &si, &bv);
                bindDraw(o, nd + k);
                ctx->OMSetRenderTargets(2, tr, r.fTruthDsv.Get());
                ctx->OMSetDepthStencilState(r.dssFwdRe.Get(), 0);
                ctx->OMSetBlendState(r.bsA2CAll.Get(), nullptr, 0xFFFFFFFFu);
                ctx->PSSetShader(r.psFwdTruth.Get(), nullptr, 0);
                ctx->DrawIndexed(ic, si, bv);
            }
        }
        ctx->OMSetRenderTargets(1, &crt, v.dsv.Get());
        {
            const auto fr = fwdRec.ReplayForward(ctx1, r.id2Rtv.Get(), r.fwdDsv.Get(), true, (UINT)rec.Count());
            if (!fr.ok || fr.replayed != nf) { printf("   FAIL forward replay (frame %d): ok %d replayed %d\n", f, (int)fr.ok, fr.replayed); ++fails; }
            for (int q = 0; q < nSh; ++q) sh[q]->AfterFwd(ctx1, (UINT)sh[q]->rec.Count(), nf);
        }
        ID3D11RenderTargetView* nullRtv = nullptr;
        ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
        ctx->CopyResource(v.twin.Get(), v.depth.Get());
        for (int q = 0; q < nSh; ++q) sh[q]->AfterTwin(ctx, ctx1, v.twinSrv.Get(), (UINT)sh[q]->rec.Count(), nf);   // phase 10
        if (f == 40 && mut == 0 && !cam.DrawIds().RequestDump("harness S8 frame 40")) { printf("   FAIL dump request refused\n"); ++fails; }
        CameraMv::FrameStats fs;
        const bool ok = cam.Generate(ctx, W, H, 0, 0, W, H, cand, v.twinSrv.Get(), v.dUav.Get(), v.mvUav.Get(), &fs, false,
                                     nullptr, nullptr, r.fwdSrv.Get(), &rec, r.id2Srv.Get(), &fwdRec);
        for (int q = 0; q < nSh; ++q) sh[q]->Generate(ctx, v.twinSrv.Get(), true);
        if (!ok) { printf("   FAIL Generate (frame %d)\n", f); ++fails; }
        if (f >= 1 && cam.DidN() != (uint32_t)(nd + nf)) {
            printf("   FAIL frame %d: %u draws in the per-draw table, want %d\n", f, cam.DidN(), nd + nf); ++fails;
        }
        const int nTab = (int)cam.DidN();
        ctx->CopyResource(v.stMv.Get(), v.mv.Get());
        ctx->CopyResource(v.stDepth.Get(), v.dOut.Get());
        ctx->CopyResource(v.stTruth.Get(), v.truth.Get());
        ctx->CopyResource(r.stFwd.Get(), r.fwd.Get());
        ctx->CopyResource(r.stFTruth.Get(), r.fTruth.Get());
        ctx->CopyResource(r.stTwin.Get(), v.twin.Get());
        if (cam.DidRSrv() && nTab) {
            ComPtr<ID3D11Resource> rres;
            cam.DidRSrv()->GetResource(&rres);
            const D3D11_BOX box{ 0, 0, 0, (UINT)nTab * DrawIdMv::kRStride * 16u, 1, 1 };
            ctx->CopySubresourceRegion(E.stR.Get(), 0, 0, 0, 0, rres.Get(), 0, &box);
        }
        D3D11_MAPPED_SUBRESOURCE mMv{}, mD{}, mT{}, mR{}, mF{}, mFT{}, mTw{};
        ctx->Map(v.stMv.Get(), 0, D3D11_MAP_READ, 0, &mMv);
        if (!nSh) CmpFrame(mMv, W, H);                  // v0.10.0 phase 8 comparison mode (phase 9: not in the shadow scenes)
        ctx->Map(v.stDepth.Get(), 0, D3D11_MAP_READ, 0, &mD);
        ctx->Map(v.stTruth.Get(), 0, D3D11_MAP_READ, 0, &mT);
        ctx->Map(E.stR.Get(), 0, D3D11_MAP_READ, 0, &mR);
        ctx->Map(r.stFwd.Get(), 0, D3D11_MAP_READ, 0, &mF);
        ctx->Map(r.stFTruth.Get(), 0, D3D11_MAP_READ, 0, &mFT);
        ctx->Map(r.stTwin.Get(), 0, D3D11_MAP_READ, 0, &mTw);
        const float* Rt = (const float*)mR.pData;
        std::vector<int> tabIdx(nObj, -1);
        for (int d = 0; d < nd; ++d) tabIdx[order[d]] = d;
        for (int k = 0; k < nf; ++k) tabIdx[forder[k]] = nd + k;
        std::vector<int> parentOf(nObj, -1);                     // the rigid parent's table index (-1: none)
        for (int o = 0; o < nObj; ++o) {
            const int ti = tabIdx[o];
            stCur[o] = (ti >= 0 && ti < nTab) ? (int)(Rt[(ti * 5 + 4) * 4] + 0.5f) : -1;
            const float w = (ti >= 0 && ti < nTab) ? Rt[(ti * 5 + 4) * 4 + 3] : 0.0f;
            if (ParentMarked(w)) parentOf[o] = (int)(-(double)w - 1.0 - 2.0 * DrawIdMv::kMax + 0.5);
        }
        // ---- per-draw state checks (frame 2 on: the bodies are paired by their own pairs) ----
        if (f >= 2) {
            auto parentCheck = [&](int o, int body, int wantSt) {
                const int g = objs[o].group;
                ++parFrames[g];
                if (stCur[o] == wantSt && parentOf[o] == tabIdx[body]) ++parOk[g];
            };
            parentCheck(oBg, oTrl, DrawIdMv::kStMover);
            parentCheck(oTx, oTrl, DrawIdMv::kStMover);
            parentCheck(oFp, oTrl, DrawIdMv::kStMover);
            parentCheck(oInstP, oTrl, DrawIdMv::kStMover);   // v0.10.0 phase 6b: the instanced plate moves with the trailer
            ++parkFrames;
            if (stCur[oPp] == DrawIdMv::kStMover) ++parkMover;
            // (phase 14: with the static gate the parked trailer may be skipped -- its id-less pixels are the pseudo static source,
            // index kMax: the same static parent motion)
            if (stCur[oPp] == DrawIdMv::kStStatic && (parentOf[oPp] == tabIdx[oPark] || (g_rstatic && parentOf[oPp] == DrawIdMv::kMax)))
                ++parkStatic;
            if (stCur[oSign] == DrawIdMv::kStMover) ++signMover;
            if (stCur[oGrass] == DrawIdMv::kStMover) ++grassMover;   // v0.10.0 phase 6b: instanced grass must never be a mover
            if (f == kHoldFrame && !nSh) {               // phase 14 round 4: the plates kept the trailer through the hold
                for (int o : { oBg, oTx, oFp, oInstP, oTkBg, oTkTx, oBot }) {
                    ++holdChecked;
                    if (stCur[o] == DrawIdMv::kStMover && parentOf[o] == tabIdx[oTrl]) ++holdOk;
                    else printf("   FAIL frame %d (trailer not replayed): %s state %d parent %d, want the trailer (%d)\n", f,
                                objs[o].name, stCur[o], parentOf[o], tabIdx[oTrl]);
                }
            }
            for (int k = 0; k < 4; ++k)                       // phase 14 round 4: nor the grass beside the passing truck
                if (stCur[oGn[k]] == DrawIdMv::kStMover) {
                    ++grassNearMover;
                    if (grassNearMover <= 3) printf("   FAIL frame %d: grass clump %d beside the truck took a MOVER's motion (parent: %s)\n", f, k, (parentOf[oGn[k]] >= 0 && parentOf[oGn[k]] < nd) ? objs[order[parentOf[oGn[k]]]].name : "?");
                }
        }
        {   // phase 14 round 4: the truck within 5 px of a grass clump (truth target): the scene exercises it
            bool nearG = false;
            for (int y = 5; y < H - 5 && !nearG; ++y) {
                const uint32_t* tRow = (const uint32_t*)((const uint8_t*)mT.pData + y * mT.RowPitch);
                for (int x = 5; x < W - 5 && !nearG; ++x) {
                    const uint32_t tv = tRow[x * 2];
                    if (tv < (uint32_t)(oGn[0] + 1) || tv > (uint32_t)(oGn[3] + 1)) continue;
                    for (int dy = -5; dy <= 5 && !nearG; ++dy) {
                        const uint32_t* t2 = (const uint32_t*)((const uint8_t*)mT.pData + (y + dy) * mT.RowPitch);
                        for (int dx = -5; dx <= 5; ++dx) if (t2[(x + dx) * 2] == (uint32_t)(oTruck + 1)) { nearG = true; break; }
                    }
                }
            }
            if (nearG) ++grassNearFrames;
        }
        // the truck's pixels within 5 px of the sign's (truth target): the scene exercises the depth test
        {
            bool nearT = false;
            for (int y = 5; y < H - 5 && !nearT; ++y) {
                const uint32_t* tRow = (const uint32_t*)((const uint8_t*)mT.pData + y * mT.RowPitch);
                for (int x = 5; x < W - 5 && !nearT; ++x) {
                    if (tRow[x * 2] != (uint32_t)(oSign + 1)) continue;
                    for (int dy = -5; dy <= 5 && !nearT; ++dy) {
                        const uint32_t* t2 = (const uint32_t*)((const uint8_t*)mT.pData + (y + dy) * mT.RowPitch);
                        for (int dx = -5; dx <= 5; ++dx) if (t2[(x + dx) * 2] == (uint32_t)(oTruck + 1)) { nearT = true; break; }
                    }
                }
            }
            if (nearT) ++signNear;
        }
        // ---- the camera R: GPU (consensus) vs exact ----
        M4 RcamG, RcamX;
        {
            if (f == 0) VprevX = V;
            M4 Pinv, Vinv; Inv(Pw, Pinv); Inv(V, Vinv);
            RcamX = Mul(Pw, Mul(VprevX, Mul(Vinv, Pinv)));
            VprevX = V;
            float sv[CameraMv::kSolveFloats];
            if (!cam.ReadSolveBlocking(ctx, sv)) { printf("   FAIL solve readback\n"); ++fails; }
            for (int rr = 0; rr < 4; ++rr) for (int c = 0; c < 4; ++c) RcamG.m[rr][c] = sv[rr * 4 + c];
            if (sv[35] > 0.5f) ++consFrames;
            double worst = 0.0;
            const double zs[4] = { 0.0, kNearWorld / 200.0, kNearWorld / 40.0, kNearWorld / 5.0 };
            for (double zq : zs)
                for (int gy = 0; gy < 7; ++gy) for (int gx = 0; gx < 7; ++gx) {
                    const double p4[4] = { -0.9 + 0.3 * gx, -0.9 + 0.3 * gy, zq, 1.0 };
                    double a[4], b[4];
                    Xf(RcamX, p4, a); Xf(RcamG, p4, b);
                    if (!(a[3] > 1e-9) || !(b[3] > 1e-9)) { worst = 1e9; continue; }
                    const double dx = (a[0] / a[3] - b[0] / b[3]) * W * 0.5, dy = (a[1] / a[3] - b[1] / b[3]) * H * 0.5;
                    worst = std::max(worst, std::sqrt(dx * dx + dy * dy));
                }
            if (f >= 2) {
                ++checks;
                if (!(worst <= 0.05)) { ++fails; ++camRBad; if (camRBad <= 3) printf("   FAIL frame %d: camera R off by %.3f px\n", f, worst); }
                worstCamPx = std::max(worstCamPx, worst);
            }
        }
        // ---- expected motion: TRUTH (a true mover its own exact R, else the exact camera R; snap band: both); a plate moves
        // exactly with its body (rigid node: R_plate = R_body); the first frame / a draw without history: the camera R ----
        std::vector<M4> Rexp(nObj, RcamX), Ralt(nObj, RcamX);
        std::vector<char> hasAlt(nObj, 0);
        for (int o = 0; o < nObj; ++o) {
            if (bodyOf[o] >= 0 || f < 1 || !drawnPrev[o]) continue;
            const M4 P = Mul(RcamG, mvpCur[o]);
            const double probes[6][4] = { {0,0,0,1}, {4,0,0,1}, {-4,0,0,1}, {0,0,4,1}, {0,0,-4,1}, {0,4,0,1} };
            double tdev = 0.0;
            for (auto& q : probes) {
                double a[4], b[4];
                Xf(mvpPrev[o], q, a); Xf(P, q, b);
                double dv;
                const bool on = a[3] > 0.05 && b[3] > 0.05 && std::fabs(a[0] / a[3]) <= 1.5 && std::fabs(a[1] / a[3]) <= 1.5 &&
                                std::fabs(b[0] / b[3]) <= 1.5 && std::fabs(b[1] / b[3]) <= 1.5;
                if (on) dv = std::hypot((a[0] / a[3] - b[0] / b[3]) * W * 0.5, (a[1] / a[3] - b[1] / b[3]) * H * 0.5);
                else dv = std::hypot((a[0] - b[0]) * W * 0.5, (a[1] - b[1]) * H * 0.5) / std::max(std::max(std::fabs(a[3]), std::fabs(b[3])), 1.0);
                tdev = std::max(tdev, dv);
            }
            M4 ic; Inv(mvpCur[o], ic);
            const M4 Rtrue = Mul(mvpPrev[o], ic);
            if (tdev >= snapPx * 1.5 + 0.02) Rexp[o] = Rtrue;
            else if (tdev > snapPx * 0.5) { Ralt[o] = Rtrue; hasAlt[o] = 1; }
        }
        for (int o = 0; o < nObj; ++o) {
            if (bodyOf[o] < 0) continue;
            Rexp[o] = Rexp[bodyOf[o]]; Ralt[o] = Ralt[bodyOf[o]]; hasAlt[o] = hasAlt[bodyOf[o]];
        }
        if (f == kHoldFrame && !nSh) { Rexp[oTrl] = RcamX; hasAlt[oTrl] = 0; }   // phase 14 round 4: not replayed this frame
        // ---- per pixel ----
        std::vector<double> errObj(nObj, 0.0);
        std::vector<int> pxOk(nObj, 0), pxAll(nObj, 0);
        for (int y = 0; y < H; ++y) {
            const uint16_t* mvRow = (const uint16_t*)((const uint8_t*)mMv.pData + y * mMv.RowPitch);
            const float* dRow = (const float*)((const uint8_t*)mD.pData + y * mD.RowPitch);
            const uint32_t* tRow = (const uint32_t*)((const uint8_t*)mT.pData + y * mT.RowPitch);
            const float* fRow = (const float*)((const uint8_t*)mF.pData + y * mF.RowPitch);
            const uint32_t* ftRow = (const uint32_t*)((const uint8_t*)mFT.pData + y * mFT.RowPitch);
            const float* twRow = (const float*)((const uint8_t*)mTw.pData + y * mTw.RowPitch);
            for (int x = 0; x < W; ++x) {
                const uint32_t tObj = tRow[x * 2], fObj = ftRow[x * 2];
                const float dt = twRow[x * 2];
                float fw = fRow[x];
                if (!(fw >= 0.01f && fw < 0.9f)) fw = 0.0f;
                const bool fwWin = fw > dt;
                int o = -1;
                if (fwWin && fObj) o = (int)fObj - 1;
                else if (tObj && !fwWin) o = (int)tObj - 1;
                if (o < 0 || f < 1) continue;
                const double dd = dRow[x];
                const double z = (dd - 0.01) / 0.89;
                const double u = (x + 0.5) / W, vv = (y + 0.5) / H;
                const double p4[4] = { u * 2 - 1, 1 - vv * 2, std::min(1.0, std::max(0.0, z)), 1.0 };
                auto mvOf = [&](const M4& Rm, double* ex, double* ey) {
                    double c[4]; Xf(Rm, p4, c);
                    *ex = 0; *ey = 0;
                    if (c[3] > 1e-6) { *ex = ((c[0] / c[3]) * 0.5 + 0.5 - u) * W; *ey = ((0.5 - (c[1] / c[3]) * 0.5) - vv) * H; }
                };
                double ex, ey; mvOf(Rexp[o], &ex, &ey);
                const double gx = H2F(mvRow[x * 2]), gy = H2F(mvRow[x * 2 + 1]);
                double err = std::hypot(gx - ex, gy - ey);
                double tol = 0.03 + snapPx + std::max(std::fabs(ex), std::fabs(ey)) / 1024.0;
                if (hasAlt[o]) {
                    double ax, ay; mvOf(Ralt[o], &ax, &ay);
                    const double ea = std::hypot(gx - ax, gy - ay), ta = 0.03 + snapPx + std::max(std::fabs(ax), std::fabs(ay)) / 1024.0;
                    if (ea / ta < err / tol) { err = ea; tol = ta; }
                }
                ++mvChecked; ++pxAll[o];
                const int g = objs[o].group;
                if (err > worstMv[g]) worstMv[g] = err;
                if (err > errObj[o]) errObj[o] = err;                     // (phase 14 round 4: this frame's worst per object)
                const bool good = err <= tol;
                if (good) ++pxOk[o]; else ++mvFails;
                ++grpPx[g]; if (good) ++grpOk[g];
                double cx, cy; mvOf(RcamG, &cx, &cy);                    // would the camera R alone be wrong here?
                if (std::hypot(cx - ex, cy - ey) > tol) ++camWrong[g];
            }
        }
        for (int o = 0; o < nObj; ++o) {
            if (!pxAll[o]) continue;
            ++checks;
            if (pxOk[o] * 1000 < pxAll[o] * 995) {
                ++fails; ++objFramesBad[o];
                if (objFramesBad[o] <= 2)
                    printf("   FAIL frame %d: %s #%d MV right on %d of %d px (state %d, parent %d; worst error %.3f px)\n", f,
                           objs[o].name, o, pxOk[o], pxAll[o], stCur[o], parentOf[o], errObj[o]);
            }
        }
        // v0.10.0 phase 6c: the new plates take their body as the rigid parent (the body's own state) in every frame they show
        // >= 8 px (the truck's plate can be partly hidden by the trailer); the per-pixel check above verifies their motion
        if (f >= 2) {
            const int pc[4][2] = { { oFarP, oFar }, { oTkBg, oTrl }, { oTkTx, oTrl }, { oBot, oTrl } };
            for (auto& q : pc) {
                const int o = q[0], body = q[1], g = objs[o].group;
                if (pxAll[o] < 8) continue;
                ++parFrames[g];
                if ((stCur[body] == DrawIdMv::kStMover || stCur[body] == DrawIdMv::kStStatic) && stCur[o] == stCur[body] &&
                    parentOf[o] == tabIdx[body]) ++parOk[g];
                else if (parFrames[g] - parOk[g] <= 2)
                    printf("   frame %d: %s #%d state %d parent %d (want state %d parent %d), %d px\n", f, objs[o].name, o, stCur[o],
                           parentOf[o], stCur[body], tabIdx[body], pxAll[o]);
            }
        }
        if (nSh) {                                      // v0.10.0 phase 9 (S12): the shadow pipelines vs this one
            std::vector<char> moverObj(nObj, 0);
            for (int o = 0; o < nObj; ++o) moverObj[o] = objs[o].kind == kMover ? 1 : 0;
            for (int q = 0; q < nSh; ++q) { ++checks; fails += sh[q]->Compare(ctx, f, mMv, mF, mTw, mT, Rt, nTab, r.id2.Get(), moverObj); }
        }
        ctx->Unmap(v.stMv.Get(), 0); ctx->Unmap(v.stDepth.Get(), 0); ctx->Unmap(v.stTruth.Get(), 0);
        ctx->Unmap(E.stR.Get(), 0); ctx->Unmap(r.stFwd.Get(), 0); ctx->Unmap(r.stFTruth.Get(), 0); ctx->Unmap(r.stTwin.Get(), 0);
        if (g_debugLayer) { const int dl = DrainDebugLayer("S8"); ++checks; if (dl) { ++fails; debugMsgs += dl; } }
        for (int o = 0; o < nObj; ++o) { mvpPrev[o] = mvpCur[o]; drawnPrev[o] = true; }
    }
    rec.Shutdown(); fwdRec.Shutdown();
    for (int q = 0; q < nSh; ++q) { fails += sh[q]->Finish(&checks); sh[q]->Shutdown(); }   // v0.10.0 phase 9 (S12)
    if (g_rstatic && !nSh) {                            // v0.10.0 phase 14: the gate must have skipped the static scenery
        printf("   static replay skip (every %u): skipped %lld of %lld G-buffer draws\n", g_rstatic, s8Skip, s8Skip + s8Rep);
        ++checks;
        if (s8Skip == 0) { ++fails; printf("   FAIL static replay skip: the gate never skipped a draw\n"); }
    }
    const DrawIdMv::Stats& ds = cam.DrawIds().GetStats();
    PrintPhase8(ctx, ds, "");
    // ---- scene checks ----
    checks += 16;       // v0.10.0 phase 6b: + 5 (instanced plate coverage / exercise / parent, grass not-mover, instanced counter)
    if (consFrames < frames - 3) { ++fails; printf("   FAIL consensus used in only %d of %d frames\n", consFrames, frames); }
    if (!(grpPx[4] > 3000 && grpPx[5] > 600 && grpPx[6] > 1500 && grpPx[8] > 800 && grpPx[10] > 3000)) {
        ++fails; printf("   FAIL coverage too small: plate background %lld, text %lld, forward plate %lld, parked plate %lld, sign %lld px\n",
                        grpPx[4], grpPx[5], grpPx[6], grpPx[8], grpPx[10]);
    }
    if (!(camWrong[4] > grpPx[4] / 2 && camWrong[5] > grpPx[5] / 2 && camWrong[6] > grpPx[6] / 2)) {
        ++fails; printf("   FAIL the scene does not exercise the parents (px the camera R would get wrong: background %lld of %lld, text %lld "
                        "of %lld, forward plate %lld of %lld)\n", camWrong[4], grpPx[4], camWrong[5], grpPx[5], camWrong[6], grpPx[6]);
    }
    if (!(yawRate > 0.005)) { ++fails; printf("   FAIL the trailer does not turn (%.4f rad / frame)\n", yawRate); }
    const int need = (frames - 2) * 95 / 100;
    if (parOk[4] < need) { ++fails; printf("   FAIL the plate background took the trailer's R in only %d of %d frames\n", parOk[4], parFrames[4]); }
    if (parOk[5] < need) { ++fails; printf("   FAIL the plate text took the trailer's R (chain) in only %d of %d frames\n", parOk[5], parFrames[5]); }
    if (parOk[6] < need) { ++fails; printf("   FAIL the forward plate took the trailer's R in only %d of %d frames\n", parOk[6], parFrames[6]); }
    if (parkMover != 0) { ++fails; printf("   FAIL the parked trailer's plate was a MOVER in %d frames\n", parkMover); }
    if (parkStatic * 2 < parkFrames) { ++fails; printf("   FAIL the parked trailer's plate took its static parent in only %d of %d frames\n", parkStatic, parkFrames); }
    if (signMover != 0 || signNear < 3) {
        ++fails; printf("   FAIL the static sign was a MOVER in %d frames (the truck within 5 px of it in %d frames, want >= 3)\n", signMover, signNear);
    }
    // v0.10.0 phase 6b: the instanced plate (on the trailer) and the instanced grass (roadside)
    if (!(grpPx[11] > 1000 && grpPx[12] > 500)) {
        ++fails; printf("   FAIL instanced coverage too small: plate %lld, grass %lld px\n", grpPx[11], grpPx[12]);
    }
    if (!(camWrong[11] > grpPx[11] / 2)) {
        ++fails; printf("   FAIL the instanced plate does not exercise the parent (px the camera R would get wrong: %lld of %lld)\n",
                        camWrong[11], grpPx[11]);
    }
    if (parOk[11] < need) {
        ++fails; printf("   FAIL the INSTANCED plate took the trailer's R in only %d of %d frames (state / parent per frame)\n",
                        parOk[11], parFrames[11]);
    }
    if (grassMover != 0) { ++fails; printf("   FAIL the instanced grass was a MOVER in %d frames (must stay static / camera R)\n", grassMover); }
    checks += 2;                                        // phase 14 round 4
    printf("   grass beside the passing truck: the truck within 5 px of it in %d frames, grass a MOVER in %d clump-frames (want 0)\n",
           grassNearFrames, grassNearMover);
    if (grassNearMover != 0) ++fails;
    if (grassNearFrames < 10) { ++fails; printf("   FAIL the truck passed within 5 px of the grass in only %d frames\n", grassNearFrames); }
    if (!nSh) {                                         // phase 14 round 4: the plate hold
        ++checks;
        printf("   plate hold (the trailer not replayed in frame %d): %d of %d plates kept its motion\n", kHoldFrame, holdOk, holdChecked);
        if (holdChecked == 0 || holdOk != holdChecked) ++fails;
    }
    if (mut != 1 && !(ds.parInst > 0)) {
        ++fails; printf("   FAIL no instanced / no-MVP draw took a rigid parent (parInst %llu)\n", (unsigned long long)ds.parInst);
    }
    // phase 6c: the plate text now finds the trailer in the 1st pass (it marches through its background); the 2nd pass ("chain")
    // is required only in the short-reach run, where the text cannot leave its background
    if (mut != 1 && !(ds.parents > 0 && ds.parMovers > 0 && (reachPx == 0 || ds.parChain > 0) && ds.parFwd > 0)) {
        ++fails; printf("   FAIL parent counters: parents %llu, movers %llu, chain (2nd pass) %llu, forward %llu\n",
                        (unsigned long long)ds.parents, (unsigned long long)ds.parMovers, (unsigned long long)ds.parChain,
                        (unsigned long long)ds.parFwd);
    }
    // v0.10.0 phase 6c: the far plate, the instanced background + text, the bottom-edge plate
    checks += 6;
    if (!(grpPx[14] > 1500 && grpPx[15] > 1500 && grpPx[16] > 500 && grpPx[17] > 1500)) {
        ++fails; printf("   FAIL phase-6c coverage too small: far plate %lld, top-band background %lld, text %lld, bottom plate %lld px\n",
                        grpPx[14], grpPx[15], grpPx[16], grpPx[17]);
    }
    if (!(camWrong[14] > grpPx[14] / 2 && camWrong[15] > grpPx[15] / 2 && camWrong[16] > grpPx[16] / 2 &&
          camWrong[17] > grpPx[17] / 2)) {
        ++fails; printf("   FAIL the phase-6c plates do not exercise the vote (px the camera R would get wrong: far %lld of %lld, top-band "
                        "background %lld of %lld, text %lld of %lld, bottom %lld of %lld)\n", camWrong[14], grpPx[14], camWrong[15],
                        grpPx[15], camWrong[16], grpPx[16], camWrong[17], grpPx[17]);
    }
    const char* const n6c[4] = { "far 24x8 px plate", "instanced top-band background", "instanced top-band text", "bottom-edge plate" };
    for (int q = 0; q < 4; ++q) {
        const int g = 14 + q;
        if (parFrames[g] < 60 || parOk[g] * 100 < parFrames[g] * 95) {
            ++fails; printf("   FAIL the %s took its body as rigid parent in only %d of %d visible frames (want >= 95 %% of >= 60)\n",
                            n6c[q], parOk[g], parFrames[g]);
        }
    }
    // the far plate really is ~24 x 8 px: its pixel count per frame (truth) averages 150..260
    const double farAvg = parFrames[14] ? (double)grpPx[14] / (double)(frames - 1) : 0.0;
    if (!(farAvg > 150.0 && farAvg < 260.0)) { ++fails; printf("   FAIL the far plate averages %.1f px per frame (want ~192 = 24 x 8)\n", farAvg); }
    if (mut == 0) {
        ++checks;
        if (ds.dumps != 1) { ++fails; printf("   FAIL the per-draw dump requested at frame 40 landed %llu times (want 1)\n", (unsigned long long)ds.dumps); }
    }
    printf("   camera R vs exact: worst %.4f px (limit 0.05), bad frames %d; consensus in %d of %d frames; trailer yaw rate up to %.4f "
           "rad / frame\n", worstCamPx, camRBad, consFrames, frames, yawRate);
    printf("   px right [px the camera R alone gets wrong]: plate background %lld/%lld [%lld], plate text %lld/%lld [%lld], forward node "
           "plate %lld/%lld [%lld], parked plate %lld/%lld, sign %lld/%lld; trailer %lld/%lld, truck %lld/%lld, parked trailer %lld/%lld; "
           "INSTANCED plate %lld/%lld [%lld], INSTANCED grass %lld/%lld\n",
           grpOk[4], grpPx[4], camWrong[4], grpOk[5], grpPx[5], camWrong[5], grpOk[6], grpPx[6], camWrong[6], grpOk[8], grpPx[8],
           grpOk[10], grpPx[10], grpOk[3], grpPx[3], grpOk[9], grpPx[9], grpOk[7], grpPx[7],
           grpOk[11], grpPx[11], camWrong[11], grpOk[12], grpPx[12]);
    printf("   rigid parent (frames 2..%d, right parent + state): background %d/%d, text (chain) %d/%d, forward plate %d/%d, INSTANCED "
           "plate %d/%d; parked plate static by its parent %d/%d (mover %d); sign: mover %d frames, the truck within 5 px in %d frames; "
           "instanced grass mover %d frames\n", frames - 1,
           parOk[4], parFrames[4], parOk[5], parFrames[5], parOk[6], parFrames[6], parOk[11], parFrames[11], parkStatic, parkFrames,
           parkMover, signMover, signNear, grassMover);
    printf("   phase 6c px right [px the camera R alone gets wrong]: FAR 24x8 plate %lld/%lld [%lld] (avg %.1f px / frame), INSTANCED top-band "
           "background %lld/%lld [%lld], INSTANCED top-band text %lld/%lld [%lld], BOTTOM-edge plate %lld/%lld [%lld], far van %lld/%lld; "
           "rigid parent + state (frames with >= 8 px): far %d/%d, background %d/%d, text %d/%d, bottom %d/%d\n",
           grpOk[14], grpPx[14], camWrong[14], farAvg, grpOk[15], grpPx[15], camWrong[15], grpOk[16], grpPx[16], camWrong[16],
           grpOk[17], grpPx[17], camWrong[17], grpOk[13], grpPx[13], parOk[14], parFrames[14], parOk[15], parFrames[15],
           parOk[16], parFrames[16], parOk[17], parFrames[17]);
    printf("   parents (read back): listed %llu, over the cap %llu, parents %llu (movers %llu, forward %llu, chain %llu), no votes %llu, "
           "no majority %llu, overrode a twin / attach %llu, left at the camera R %llu, avg votes %.1f per listed draw, twin lookups "
           "skipped (projection-only MVP) %llu; instanced/no-MVP listed %llu (of %llu with pixels), took a parent %llu\n",
           (unsigned long long)ds.parListed, (unsigned long long)ds.parOver,
           (unsigned long long)ds.parents, (unsigned long long)ds.parMovers, (unsigned long long)ds.parFwd,
           (unsigned long long)ds.parChain, (unsigned long long)ds.parNoVotes, (unsigned long long)ds.parNoWinner,
           (unsigned long long)ds.parOverrode, (unsigned long long)ds.parLeft,
           ds.parListed ? (double)ds.parVotes / (double)ds.parListed : 0.0, (unsigned long long)ds.twinViewSkip,
           (unsigned long long)ds.parInstListed, (unsigned long long)ds.parInstVis, (unsigned long long)ds.parInst);
    printf("   phase 6c marching (read back): 2nd-pass parents %llu, no votes (no source within reach) %llu, depth-rejected %llu, no "
           "majority %llu, no pixel sampled %llu; 1st-pass marches %llu (avg %.1f px marched; %llu depth-rejected, %llu without a "
           "source)\n",
           (unsigned long long)ds.parChain, (unsigned long long)ds.parNoVotes, (unsigned long long)ds.parDepthRej,
           (unsigned long long)ds.parNoWinner, (unsigned long long)ds.parNoPixel, (unsigned long long)ds.parMarches,
           ds.parMarches ? (double)ds.parMarchPx / (double)ds.parMarches : 0.0, (unsigned long long)ds.parRejMarches,
           (unsigned long long)ds.parNoSrcMarches);
    printf("   MV: %d of %lld pixels outside tolerance; worst MV error per group (px): ground %.3f box %.3f post %.3f trailer %.3f "
           "background %.3f text %.3f forward plate %.3f parked trailer %.3f parked plate %.3f truck %.3f sign %.3f inst-plate %.3f "
           "inst-grass %.3f far-van %.3f far-plate %.3f top-bg %.3f top-text %.3f bottom-plate %.3f\n", mvFails,
           mvChecked, worstMv[0], worstMv[1], worstMv[2], worstMv[3], worstMv[4], worstMv[5], worstMv[6], worstMv[7], worstMv[8],
           worstMv[9], worstMv[10], worstMv[11], worstMv[12], worstMv[13], worstMv[14], worstMv[15], worstMv[16], worstMv[17]);
    if (g_debugLayer) printf("   debug layer: %d error / warning messages\n", debugMsgs);
    printf("   %s: %d / %d checks passed\n", fails ? "FAILED" : "passed", checks - fails, checks);
    DrawIdMv::SetAttach(attachSaved);
    DrawIdMv::SetTwin(twinSaved);
    DrawIdMv::SetParent(parentSaved);
    DrawIdMv::SetParentMutation(0);
    DrawIdMv::SetInstanced(instSaved);                  // v0.10.0 phase 6b
    DrawIdMv::SetParentReach(0);                        // v0.10.0 phase 6c
    tot.checks += checks; tot.fails += fails;
    return fails;
}

// ---- S9 (v0.10.0 phase 7): the pre / post-tonemap STAGE -- one decision per pass + hysteresis (DlaaStage, pure logic), the
// two colour sets of SceneDlaa kept alive (real src/scene_dlaa.cpp on WARP with tests/fake_dlaa.cpp standing in for NGX), and
// a model of inject.cpp's per-pass flow (PreTonemapPoint + the blit) driven with the in-game pass pattern of the phase-6c log.
// What it proves: no per-pass flip, a stage switch costs no history reset (auto switches), no NGX feature is rebuilt after the
// first two (one LDR + one HDR), a size change still rebuilds, deferred / failed builds leave the other unit running.
// NOT covered: inject.cpp itself (the forward leave / discard hooks, the game's binding order) -- in-game log only.
namespace FakeNgx {
extern int creates, destroys, evals, resets;
extern bool budgetBusy, failHdrInit;
extern uintptr_t lastFeature;
extern bool lastReset, lastHdr;
float Marker(uintptr_t id);
}

static float HalfToFloat(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}
// red channel of texel (x, y) of a DEFAULT RGBA8_UNORM / RGBA16_FLOAT texture
static float TexelR(Env& E, ID3D11Texture2D* t, UINT x, UINT y) {
    D3D11_TEXTURE2D_DESC d{};
    t->GetDesc(&d);
    ComPtr<ID3D11Texture2D> st;
    if (!MakeTex(E.dev.Get(), d.Width, d.Height, d.Format, 0, &st, true)) return -1.0f;
    E.ctx->CopyResource(st.Get(), t);
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(E.ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &mp))) return -1.0f;
    float v = -1.0f;
    const uint8_t* row = (const uint8_t*)mp.pData + (size_t)y * mp.RowPitch;
    if (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) v = HalfToFloat(((const uint16_t*)row)[x * 4]);
    else v = (float)row[x * 4] / 255.0f;
    E.ctx->Unmap(st.Get(), 0);
    return v;
}

// v0.10.0 phase 8 S10: the pass-slot texture pool (DepthTwin::ReleaseToPool; inject.cpp mv_slot_pool). VR-like pattern: two
// passes in flight (eye 0, eye 1), each slot of a 4-slot ring takes its depth twin / forward depth / draw-id target at first use
// and gives them back after "its blit" -> only 2 sets may ever be alive; a mirror-like twin (not pooled) never takes or gives
// back; a size change drops the pooled sets of the old size.
int RunPoolScene(Env& E, Totals& tot) {
    printf("\nS10 pass-slot texture pool (mv_slot_pool): 4 slots, 2 passes in flight, 200 passes, then a resolution change\n");
    int checks = 0, fails = 0;
    auto check = [&](bool ok, const char* what) { ++checks; if (!ok) { ++fails; printf("   FAIL %s\n", what); } };
    ID3D11DeviceContext* ctx = E.ctx.Get();
    DepthTwin::PoolShutdown();
    const DepthTwin::PoolStats s0 = DepthTwin::GetPoolStats();
    DepthTwin slot[4];
    for (DepthTwin& s : slot) s.pooled = true;
    DepthTwin mirror;                                   // not pooled (a mirror unit's twin)
    ComPtr<ID3D11Texture2D> depthA, depthB, mirDepth;
    MakeTex(E.dev.Get(), 320, 200, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D11_BIND_DEPTH_STENCIL, &depthA);
    MakeTex(E.dev.Get(), 400, 240, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D11_BIND_DEPTH_STENCIL, &depthB);
    MakeTex(E.dev.Get(), 128, 256, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D11_BIND_DEPTH_STENCIL, &mirDepth);
    int maxAlive = 0;
    bool texOk = true;
    for (int p = 0; p < 200; ++p) {
        DepthTwin& s = slot[p % 4];
        const bool okIds = s.EnsureIds(E.dev.Get(), 320, 200, true);
        const bool okFwd = s.EnsureFwd(E.dev.Get(), 320, 200);
        const bool okSnap = s.Snapshot(ctx, depthA.Get());
        texOk = texOk && okIds && okFwd && okSnap && s.idW == 320 && s.fwdW == 320 && s.w == 320;
        mirror.Snapshot(ctx, mirDepth.Get());           // must neither take from nor drop the pool
        const DepthTwin::PoolStats st = DepthTwin::GetPoolStats();
        if (st.depthAlive > maxAlive) maxAlive = st.depthAlive;
        if (p >= 1) slot[(p - 1) % 4].ReleaseToPool();  // the previous pass's blit (2 passes in flight)
    }
    slot[199 % 4].ReleaseToPool();
    const DepthTwin::PoolStats s1 = DepthTwin::GetPoolStats();
    check(texOk, "every pass got its three textures at the right size");
    check(maxAlive == 2, "at most 2 depth twins alive with 2 passes in flight");
    check(s1.depthAlive == 2 && s1.fwdAlive == 2 && s1.idAlive == 2, "2 sets alive at the end (all idle in the pool)");
    check(s1.depthPooled == 2 && s1.fwdPooled == 2 && s1.idPooled == 2, "both sets back in the pool");
    check(s1.created - s0.created == 6, "6 textures created for 200 passes (2 sets x 3)");
    check(mirror.tex && mirror.w == 128, "the mirror twin kept its own texture");
    // resolution change: the pooled sets of the old size are dropped when a slot looks for its new size
    slot[0].Snapshot(ctx, depthB.Get());
    slot[0].EnsureIds(E.dev.Get(), 400, 240, true);
    slot[0].EnsureFwd(E.dev.Get(), 400, 240);
    const DepthTwin::PoolStats s2 = DepthTwin::GetPoolStats();
    check(slot[0].w == 400 && slot[0].idW == 400 && slot[0].fwdW == 400, "new size after a resolution change");
    check(s2.depthAlive == 1 && s2.fwdAlive == 1 && s2.idAlive == 1 && s2.depthPooled == 0,
          "the old-size sets were released (1 set alive, nothing pooled)");
    printf("   pool: max depth twins alive %d over 200 passes (4 slots), created %llu, reused %llu, %.2f MB alive after the change\n",
           maxAlive, (unsigned long long)(s2.created - s0.created), (unsigned long long)(s2.reused - s0.reused), s2.mbAlive);
    for (DepthTwin& s : slot) { s.ReleaseToPool(); }
    DepthTwin::PoolShutdown();
    printf("   %s: %d / %d checks passed\n", fails ? "FAILED" : "passed", checks - fails, checks);
    tot.checks += checks; tot.fails += fails;
    return fails;
}

int RunStageScene(Env& E, Totals& tot) {
    int checks = 0, fails = 0;
    auto CK = [&](bool ok, const char* what) { ++checks; if (!ok) { ++fails; printf("   FAIL %s\n", what); } };
    char msg[640];
    const int H = DlaaStage::kHyst, Wm = DlaaStage::kWarm;

    // ---------------- A. DlaaStage (pure logic) ----------------
    {   // A1 start-up: post-tonemap first; the HDR unit warms on the last kWarm passes; exactly one switch at pass kHyst
        DlaaStage st;
        int warms = 0, sw = 0, firstPre = -1, firstWarm = -1;
        bool warm = false;
        for (int i = 1; i <= 300; ++i) {
            DlaaStageIn in; in.possible = true; in.hdrWarm = warm;
            const DlaaStageOut o = st.Decide(in);
            if (o.warmHdr) { ++warms; warm = true; if (firstWarm < 0) firstWarm = i; }
            if (o.switched) { ++sw; if (firstPre < 0) firstPre = i; }
            if (o.runPre) st.OnPreRun(true);
        }
        snprintf(msg, sizeof(msg), "A1 start-up: switch at pass %d (want %d), %d switch(es), %d warm passes from %d (want %d from %d)",
                 firstPre, H, sw, warms, firstWarm, Wm, H - Wm);
        CK(firstPre == H && sw == 1 && warms == Wm && firstWarm == H - Wm && st.Preferred() == 1, msg);
        // A2 per-pass alternation (the in-game "post #11162, pre #11164, post #11166" pattern): never a switch
        int sw2 = 0, fb = 0, pre = 0;
        for (int i = 0; i < 2000; ++i) {
            DlaaStageIn in; in.possible = (i & 1) == 0; in.hdrWarm = true;
            const DlaaStageOut o = st.Decide(in);
            if (o.switched) ++sw2;
            if (o.fallback) ++fb;
            if (o.runPre) { ++pre; st.OnPreRun(true); }
        }
        snprintf(msg, sizeof(msg), "A2 alternating passes: %d switch(es) (want 0), %d fallbacks %d pre runs (want 1000 / 1000)", sw2, fb, pre);
        CK(sw2 == 0 && fb == 1000 && pre == 1000 && st.Preferred() == 1, msg);
        // A3 runs of kHyst-1 failures never switch; kHyst in a row switch exactly once, at the kHyst-th
        int sw3 = 0, at = -1;
        { DlaaStageIn in; in.possible = true; in.hdrWarm = true; const DlaaStageOut o = st.Decide(in); if (o.runPre) st.OnPreRun(true); }
        for (int r = 0; r < 10; ++r)
            for (int i = 0; i < H; ++i) {
                DlaaStageIn in; in.possible = i == H - 1; in.hdrWarm = true;
                const DlaaStageOut o = st.Decide(in);
                if (o.switched) ++sw3;
                if (o.runPre) st.OnPreRun(true);
            }
        for (int i = 1; i <= H + 30; ++i) {
            DlaaStageIn in; in.possible = false;
            const DlaaStageOut o = st.Decide(in);
            if (o.switched) { ++sw3; if (at < 0) at = i; if (o.switched != -1 || o.modeSwitch) at = -100; }
        }
        snprintf(msg, sizeof(msg), "A3 %d-pass failure runs: %d switch(es) (want 1) at failing pass %d (want %d), preferred %d",
                 H - 1, sw3, at, H, st.Preferred());
        CK(sw3 == 1 && at == H && st.Preferred() == 0, msg);
    }
    {   // A4 the HDR run itself fails / waits kHyst times in a row -> switch once (OnPreRun returns -1 exactly once)
        DlaaStage st;
        for (int i = 0; i < H; ++i) { DlaaStageIn in; in.possible = true; in.hdrWarm = true; const DlaaStageOut o = st.Decide(in); if (o.runPre) st.OnPreRun(true); }
        int neg = 0, at = -1;
        for (int i = 1; i <= H + 10; ++i) {
            DlaaStageIn in; in.possible = true; in.hdrWarm = true;
            const DlaaStageOut o = st.Decide(in);
            if (o.runPre && st.OnPreRun(false) < 0) { ++neg; if (at < 0) at = i; }
        }
        snprintf(msg, sizeof(msg), "A4 failing HDR runs: %d switch(es) to post (want 1) at %d (want %d)", neg, at, H);
        CK(neg == 1 && at == H && st.Preferred() == 0, msg);
    }
    {   // A5 mode reasons switch at once (once); back to eligible without a warm HDR unit: forced switch after 2 x kHyst
        DlaaStage st;
        for (int i = 0; i < H; ++i) { DlaaStageIn in; in.possible = true; in.hdrWarm = true; const DlaaStageOut o = st.Decide(in); if (o.runPre) st.OnPreRun(true); }
        int modeSw = 0, otherSw = 0;
        for (int i = 0; i < 100; ++i) {
            DlaaStageIn in; in.ineligible = "Ctrl+F6";
            const DlaaStageOut o = st.Decide(in);
            if (o.switched == -1 && o.modeSwitch && i == 0) ++modeSw; else if (o.switched) ++otherSw;
        }
        int at = -1; bool forced = false;
        for (int i = 1; i <= 3 * H; ++i) {
            DlaaStageIn in; in.possible = true; in.hdrWarm = false;
            const DlaaStageOut o = st.Decide(in);
            if (o.switched && at < 0) { at = i; forced = o.forced; }
            if (o.runPre) st.OnPreRun(true);
        }
        snprintf(msg, sizeof(msg), "A5 mode switch %d (want 1, immediate), others %d (want 0); forced switch at %d (want %d, forced %d)",
                 modeSw, otherSw, at, 2 * H, (int)forced);
        CK(modeSw == 1 && otherSw == 0 && at == 2 * H && forced, msg);
    }
    {   // A6 random 30 % failing passes over 20000 passes: no switch at all
        DlaaStage st;
        std::mt19937 rng(7);
        int sw = 0;
        for (int i = 0; i < 20000; ++i) {
            DlaaStageIn in; in.possible = i < H ? true : (rng() % 10) >= 3; in.hdrWarm = true;
            const DlaaStageOut o = st.Decide(in);
            if (o.switched && i >= H) ++sw;
            if (o.runPre) st.OnPreRun(true);
        }
        snprintf(msg, sizeof(msg), "A6 30 %% random failing passes: %d switch(es) after start-up (want 0)", sw);
        CK(sw == 0, msg);
    }

    // ---------------- B. SceneDlaa: two colour sets (fake NGX) ----------------
    const float sharpSaved = SceneDlaa::Sharpness();
    SceneDlaa::SetSharpness(0.0f);
    const int c0 = FakeNgx::creates, d0 = FakeNgx::destroys;
    auto mk = [&](UINT w, UINT h, DXGI_FORMAT f, UINT bind, ComPtr<ID3D11Texture2D>* t) { return MakeTex(E.dev.Get(), w, h, f, bind, t->ReleaseAndGetAddressOf()); };
    ComPtr<ID3D11Texture2D> ldr, hdr, dep;
    auto makeSet = [&](UINT w, UINT h) {
        mk(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, &ldr);
        mk(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, &hdr);
        mk(w, h, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_DEPTH_STENCIL, &dep);
    };
    makeSet(64, 48);
    {
        SceneDlaa sd;
        sd.SetEye(0);
        auto run = [&](ID3D11Texture2D* t, bool reset = false) { return sd.Run(E.ctx.Get(), t, dep.Get(), nullptr, nullptr, 0.25f, -0.25f, reset, false); };
        bool ok = run(ldr.Get());
        const uintptr_t idL = FakeNgx::lastFeature;
        snprintf(msg, sizeof(msg), "B1 first LDR run: ok %d, creates %d (want 1), reset %d (want 1, fresh), copy-back %.3f (want %.3f)",
                 (int)ok, FakeNgx::creates - c0, (int)FakeNgx::lastReset, TexelR(E, ldr.Get(), 3, 3), FakeNgx::Marker(idL));
        CK(ok && FakeNgx::creates - c0 == 1 && FakeNgx::lastReset && !FakeNgx::lastHdr && sd.KindReady(false) && !sd.ParkedValid() &&
           std::fabs(TexelR(E, ldr.Get(), 3, 3) - FakeNgx::Marker(idL)) < 0.01f, msg);
        for (int i = 0; i < 3; ++i) ok = run(ldr.Get()) && ok;
        CK(ok && FakeNgx::creates - c0 == 1 && !FakeNgx::lastReset, "B2 LDR runs keep their feature and history");
        // warm: the HDR set is built next to the LDR one, evaluates on the same pass, writes nothing back
        const FLOAT q[4] = { 0.25f, 0.25f, 0.25f, 1.0f };
        ComPtr<ID3D11RenderTargetView> hdrRtv;
        E.dev->CreateRenderTargetView(hdr.Get(), nullptr, &hdrRtv);
        E.ctx->ClearRenderTargetView(hdrRtv.Get(), q);
        ok = sd.Warm(E.ctx.Get(), hdr.Get(), 0.25f, -0.25f);
        const uintptr_t idH = FakeNgx::lastFeature;
        snprintf(msg, sizeof(msg), "B3 first warm: ok %d, creates %d (want 2), set builds %llu (want 1), parked HDR %d, fresh reset %d, "
                 "HDR texture untouched %.3f (want 0.25)", (int)ok, FakeNgx::creates - c0, (unsigned long long)sd.SetBuilds(),
                 (int)(sd.ParkedValid() && sd.ParkedHdr()), (int)FakeNgx::lastReset, TexelR(E, hdr.Get(), 3, 3));
        CK(ok && FakeNgx::creates - c0 == 2 && sd.SetBuilds() == 1 && sd.ParkedValid() && sd.ParkedHdr() && FakeNgx::lastHdr &&
           FakeNgx::lastReset && idH != idL && std::fabs(TexelR(E, hdr.Get(), 3, 3) - 0.25f) < 0.01f, msg);
        for (int i = 0; i < 3; ++i) { ok = run(ldr.Get()) && ok; ok = sd.Warm(E.ctx.Get(), hdr.Get(), 0.0f, 0.0f) && ok; }
        snprintf(msg, sizeof(msg), "B4 run + warm x3: creates %d (want 2), last warm reset %d (want 0), idle HDR %llu (want 0) LDR %llu (want 1)",
                 FakeNgx::creates - c0, (int)FakeNgx::lastReset, (unsigned long long)sd.KindIdleRuns(true),
                 (unsigned long long)sd.KindIdleRuns(false));
        CK(ok && FakeNgx::creates - c0 == 2 && !FakeNgx::lastReset && FakeNgx::lastFeature == idH && sd.KindIdleRuns(true) == 0 &&
           sd.KindIdleRuns(false) == 1, msg);
        CK(!sd.Warm(E.ctx.Get(), ldr.Get(), 0.0f, 0.0f), "B5 Warm with the ACTIVE kind does nothing");
        // the switch: HDR takes over by a swap -- same feature, no reset, its result written into the HDR texture
        ok = run(hdr.Get());
        snprintf(msg, sizeof(msg), "B6 HDR take-over: ok %d, creates %d (want 2), swaps %llu (want 1), feature %llu (want %llu), reset %d "
                 "(want 0), HDR result %.3f (want %.3f)", (int)ok, FakeNgx::creates - c0, (unsigned long long)sd.SetSwaps(),
                 (unsigned long long)FakeNgx::lastFeature, (unsigned long long)idH, (int)FakeNgx::lastReset,
                 TexelR(E, hdr.Get(), 3, 3), FakeNgx::Marker(idH));
        CK(ok && FakeNgx::creates - c0 == 2 && sd.SetSwaps() == 1 && FakeNgx::lastFeature == idH && !FakeNgx::lastReset &&
           sd.IsHdr() && sd.ParkedValid() && !sd.ParkedHdr() && std::fabs(TexelR(E, hdr.Get(), 3, 3) - FakeNgx::Marker(idH)) < 0.01f, msg);
        for (int i = 0; i < 5; ++i) ok = run(hdr.Get()) && ok;
        CK(ok && sd.KindIdleRuns(false) == 7 && sd.KindIdleRuns(true) == 0, "B7 KindIdleRuns counts the other set's evaluations (LDR: 1 + 6 HDR runs)");
        ok = run(ldr.Get());
        snprintf(msg, sizeof(msg), "B8 LDR fallback pass: swaps %llu (want 2), feature %llu (want %llu), LDR result %.3f (want %.3f)",
                 (unsigned long long)sd.SetSwaps(), (unsigned long long)FakeNgx::lastFeature, (unsigned long long)idL,
                 TexelR(E, ldr.Get(), 3, 3), FakeNgx::Marker(idL));
        CK(ok && sd.SetSwaps() == 2 && FakeNgx::lastFeature == idL && !FakeNgx::lastHdr &&
           std::fabs(TexelR(E, ldr.Get(), 3, 3) - FakeNgx::Marker(idL)) < 0.01f, msg);
        int bad = 0;
        for (int i = 0; i < 40; ++i) {
            const bool h = (i & 1) == 0;
            if (!run(h ? hdr.Get() : ldr.Get())) ++bad;
            if (FakeNgx::lastFeature != (h ? idH : idL) || FakeNgx::lastReset) ++bad;
        }
        snprintf(msg, sizeof(msg), "B9 40 alternating passes: creates %d (want 2), destroys %d (want 0), swaps %llu (want 42), bad %d",
                 FakeNgx::creates - c0, FakeNgx::destroys - d0, (unsigned long long)sd.SetSwaps(), bad);
        CK(bad == 0 && FakeNgx::creates - c0 == 2 && FakeNgx::destroys == d0 && sd.SetSwaps() == 42, msg);
        // a size change takes both sets
        makeSet(32, 24);
        ok = run(ldr.Get());
        snprintf(msg, sizeof(msg), "B10 size change: creates %d (want 3), destroys %d (want 2), parked %d (want 0)",
                 FakeNgx::creates - c0, FakeNgx::destroys - d0, (int)sd.ParkedValid());
        CK(ok && FakeNgx::creates - c0 == 3 && FakeNgx::destroys - d0 == 2 && !sd.ParkedValid() && FakeNgx::lastReset, msg);
        // a deferred build of the other set leaves the running one untouched
        FakeNgx::budgetBusy = true;
        ok = run(hdr.Get());
        const bool deferredOk = !ok && sd.Deferred() && sd.KindReady(false) && !sd.ParkedValid() && FakeNgx::creates - c0 == 3;
        FakeNgx::budgetBusy = false;
        ok = run(ldr.Get()) && FakeNgx::lastFeature != idL;
        CK(deferredOk && ok && FakeNgx::creates - c0 == 3, "B11 deferred HDR build: false + Deferred(), the LDR set keeps running");
        ok = run(hdr.Get());
        CK(ok && FakeNgx::creates - c0 == 4 && sd.ParkedValid() && !sd.ParkedHdr() && FakeNgx::lastReset,
           "B12 the HDR set built when the budget is free (LDR parked, fresh reset)");
        // a failed build of the other kind: no retry at these sizes, the other set keeps running
        makeSet(48, 32);
        ok = run(ldr.Get());
        const int c5 = FakeNgx::creates;
        FakeNgx::failHdrInit = true;
        const bool f1 = !run(hdr.Get()) && sd.InitFailed() && sd.KindReady(false);
        const bool l1 = run(ldr.Get()) && !sd.InitFailed() && !FakeNgx::lastHdr;
        const bool f2 = !run(hdr.Get()) && sd.InitFailed();
        const bool w1 = !sd.Warm(E.ctx.Get(), hdr.Get(), 0.0f, 0.0f);
        FakeNgx::failHdrInit = false;
        snprintf(msg, sizeof(msg), "B13 failed HDR build: refused %d, LDR keeps running %d, no retry %d, no warm %d, creates after %d (want 0)",
                 (int)f1, (int)l1, (int)f2, (int)w1, FakeNgx::creates - c5);
        CK(ok && f1 && l1 && f2 && w1 && FakeNgx::creates == c5, msg);
        SceneDlaa fresh;
        CK(!fresh.Warm(E.ctx.Get(), hdr.Get(), 0.0f, 0.0f), "B14 Warm without a preceding Run does nothing");
        sd.Shutdown();
        fresh.Shutdown();
        snprintf(msg, sizeof(msg), "B15 Shutdown releases every feature: creates %d destroys %d", FakeNgx::creates - c0, FakeNgx::destroys - d0);
        CK(FakeNgx::creates - c0 == FakeNgx::destroys - d0, msg);
    }

    // ---------------- C. the inject.cpp flow, in-game pass pattern (phase-6c log) ----------------
    {
        makeSet(64, 48);
        SceneDlaa u;
        u.SetEye(0);
        DlaaStage st;
        const int cc0 = FakeNgx::creates;
        // (passes, mode) blocks: 1 = pre-tonemap possible, 0 = not possible, 2 = per-pass alternation, 3 = a mode reason
        const int pat[][2] = { {600, 1}, {72, 0}, {600, 1}, {40, 2}, {19, 0}, {581, 1}, {255, 0}, {345, 1}, {30, 0}, {30, 1},
                               {30, 0}, {30, 1}, {59, 0}, {200, 1}, {300, 3}, {600, 1}, {8, 0}, {120, 2}, {400, 1} };
        int pass = 0, lastSwitch = -1000, minGap = 1 << 30, switches = 0, autoSw = 0, switchResets = 0, resets = 0, alterResets = 0;
        int preRuns = 0, blits = 0, warms = 0, bad = 0, expectAuto = 0;
        {   // expected automatic switches: a run of >= kHyst passes of the other condition flips the preferred stage; the
            // alternating blocks never reach kHyst in a row; a mode block switches to post (not automatic)
            int ep = 0;
            for (const auto& b : pat) {
                if (b[1] == 3) ep = 0;
                else if (b[1] == 0 && ep == 1 && b[0] >= H) { ++expectAuto; ep = 0; }
                else if (b[1] == 1 && ep == 0 && b[0] >= H) { ++expectAuto; ep = 1; }
            }
        }
        for (const auto& b : pat) {
            for (int i = 0; i < b[0]; ++i, ++pass) {
                DlaaStageIn in;
                if (b[1] == 3) in.ineligible = "Ctrl+F6";
                in.possible = b[1] == 1 || (b[1] == 2 && (i & 1));
                in.hdrWarm = u.KindIdleRuns(true) <= 1;
                const int r0 = FakeNgx::resets;
                const DlaaStageOut o = st.Decide(in);
                bool pre = false;
                if (o.runPre) {
                    const uint64_t idle = u.KindIdleRuns(true);
                    const bool rs = idle != UINT64_MAX && idle > 2;
                    pre = u.Run(E.ctx.Get(), hdr.Get(), dep.Get(), nullptr, nullptr, 0.0f, 0.0f, rs, false);
                    st.OnPreRun(pre);
                    if (pre) ++preRuns; else ++bad;
                }
                if (!pre) {
                    const uint64_t idle = u.KindIdleRuns(false);
                    const bool rs = idle != UINT64_MAX && idle > 2;
                    if (!u.Run(E.ctx.Get(), ldr.Get(), dep.Get(), nullptr, nullptr, 0.0f, 0.0f, rs, false)) ++bad;
                    ++blits;
                    if (o.warmHdr && u.Warm(E.ctx.Get(), hdr.Get(), 0.0f, 0.0f)) ++warms;
                }
                const int dr = FakeNgx::resets - r0;
                resets += dr;
                if (b[1] == 2) alterResets += dr;
                if (o.switched) {
                    ++switches;
                    if (!o.modeSwitch) { ++autoSw; switchResets += dr; }
                    if (pass - lastSwitch < minGap) minGap = pass - lastSwitch;
                    lastSwitch = pass;
                }
            }
        }
        snprintf(msg, sizeof(msg), "C %d passes: NGX features created %d (want 2), stage switches %d (auto %d, want %d = 6), min passes "
                 "between switches %d (want >= %d), resets at auto switches %d (want 0), resets in the alternating blocks %d (want <= 2), "
                 "resets total %d, pre runs %d, blit runs %d, warm-ups %d, failed runs %d", pass, FakeNgx::creates - cc0, switches,
                 autoSw, expectAuto, minGap, H, switchResets, alterResets, resets, preRuns, blits, warms, bad);
        printf("   %s\n", msg);
        CK(FakeNgx::creates - cc0 == 2 && autoSw == expectAuto && minGap >= H && switchResets == 0 && alterResets <= 2 && bad == 0, msg);
        u.Shutdown();
    }
    SceneDlaa::SetSharpness(sharpSaved);
    if (g_debugLayer) { const int dl = DrainDebugLayer("S9"); ++checks; if (dl) { ++fails; printf("   FAIL debug layer: %d messages\n", dl); } }
    printf("   %s: %d / %d checks passed\n", fails ? "FAILED" : "passed", checks - fails, checks);
    tot.checks += checks; tot.fails += fails;
    return fails;
}

// v0.10.0 phase 9: DrawIdRecord's view helpers (shared with inject.cpp's forward-depth re-draw)
int RunViewUnit(Env& E, Totals& tot) {
    printf("\n===== S11/S12 unit: DrawIdRecord::ScissorRs / ScaledScissor / ScaleViewport =====\n");
    int checks = 0, fails = 0;
    auto expect = [&](bool c, const char* what) { ++checks; if (!c) { ++fails; printf("   FAIL %s\n", what); } };
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_FRONT; rd.FrontCounterClockwise = TRUE;
    rd.DepthBias = 3; rd.SlopeScaledDepthBias = 0.5f; rd.DepthClipEnable = FALSE; rd.ScissorEnable = FALSE;
    ComPtr<ID3D11RasterizerState> noSc, withSc;
    E.dev->CreateRasterizerState(&rd, &noSc);
    rd.ScissorEnable = TRUE;
    E.dev->CreateRasterizerState(&rd, &withSc);
    bool had = true;
    ID3D11RasterizerState* c = DrawIdRecord::ScissorRs(E.dev.Get(), noSc.Get(), &had);
    D3D11_RASTERIZER_DESC cd{};
    if (c) c->GetDesc(&cd);
    expect(c && c != noSc.Get() && !had, "a no-scissor state gets its own copy");
    expect(c && cd.ScissorEnable && cd.CullMode == D3D11_CULL_FRONT && cd.FrontCounterClockwise && cd.DepthBias == 3 &&
           cd.SlopeScaledDepthBias == 0.5f && !cd.DepthClipEnable, "the copy differs only in ScissorEnable");
    expect(DrawIdRecord::ScissorRs(E.dev.Get(), noSc.Get(), &had) == c, "the copy is kept (one per state)");
    expect(DrawIdRecord::ScissorRs(E.dev.Get(), withSc.Get(), &had) == withSc.Get() && had, "a scissor state is used as it is");
    ID3D11RasterizerState* dn = DrawIdRecord::ScissorRs(E.dev.Get(), nullptr, &had);
    D3D11_RASTERIZER_DESC dd{};
    if (dn) dn->GetDesc(&dd);
    expect(dn && dd.ScissorEnable && dd.CullMode == D3D11_CULL_BACK && dd.DepthClipEnable && !had, "the default state's copy");
    const D3D11_RECT own = { 10, 20, 900, 500 }, clip = { 100, 7, 1000, 301 };
    D3D11_RECT o{};
    expect(DrawIdRecord::ScaledScissor(true, &own, &clip, 0, &o) && o.left == 100 && o.top == 20 && o.right == 900 && o.bottom == 301,
           "own n clip");
    expect(DrawIdRecord::ScaledScissor(false, &own, &clip, 0, &o) && o.left == 100 && o.top == 7 && o.right == 1000 && o.bottom == 301,
           "no own scissor: the clip");
    expect(DrawIdRecord::ScaledScissor(true, &own, &clip, 1, &o) && o.left == 50 && o.top == 10 && o.right == 450 && o.bottom == 151,
           "1/2 size: floor / ceil");
    const D3D11_RECT farR = { 950, 0, 990, 10 };
    expect(!DrawIdRecord::ScaledScissor(true, &own, &farR, 0, &o) && o.right == 0, "empty");
    D3D11_VIEWPORT vp = { 0.25f, -0.375f, 961.0f, 540.0f, 0.01f, 0.9f };
    DrawIdRecord::ScaleViewport(&vp, 1, 1);
    expect(vp.TopLeftX == 0.125f && vp.TopLeftY == -0.1875f && vp.Width == 480.5f && vp.Height == 270.0f && vp.MinDepth == 0.01f &&
           vp.MaxDepth == 0.9f, "1/2-size viewport");
    printf("   %s: %d / %d checks passed\n", fails ? "FAILED" : "passed", checks - fails, checks);
    tot.checks += checks; tot.fails += fails;
    return fails;
}

// ---- S15: ROADSIDE (v0.10.0 phase 14 round 6) ---------------------------------------------------------------------------
// ATS 2026-10-09 (Ctrl+F6 video: the roadside grass and vegetation SOLID MAGENTA while driving, smeared under DLSS; the user capture
// captures/ats_flat_fence_frame1634.rdc): ATS draws its roadside grass CAMERA-RELATIVE -- cb0 rows 4..7 = projection * view ROTATION
// (column 3 = (~0, ~0, near, ~0)), the vertices relative to this frame's camera. Such a draw's own pair gives R = the camera rotation
// only, and with the camera driving it was a "mover" with that R. S15 draws a roadside the same way along a straight road the camera
// drives (0.5 m / frame, slow yaw): 16 NON-instanced camera-relative clumps (VSSkin: the clump's position relative to the camera is a
// per-frame bone, as the game's offset), 12 INSTANCED camera-relative batches of 2 and 3 instances + one of 8, a car overtaking
// alongside the right-hand clumps (25..55 cm from them) with a node-matrix G-buffer plate and a 1-instance INSTANCED plate (world VP:
// phase 6b), an oncoming car passing the left-hand clumps, a parked van among the right-hand clumps, the ground + 10 unique boxes + 40
// identical posts (the consensus). Checks: every clump pixel has the exact CAMERA motion in every frame (0 px off), no clump draw is
// ever a mover or takes / holds a parent, the instance counts the shaders saw (Cnt 215..223) are exactly the recorded 1 / 2 / 3 / 8,
// the cars' pixels move with them. Mutations (scene 15, 3rd argument): "mutcr" = the origin-free rule off (S15 must FAIL), "mutinst" =
// multi-instance draws listed / held again (the round-4 rule; reported), "mutr6" = the round-6 origin-free rule (the VR eyes must
// FAIL, flat passes).
// Round 7 (ATS VR run of round 6: the rule never fired, "held static 0.0/frame"): eye = 1 / 2 runs S15 as VR eye 0 / eye 1 --
// every draw's view = T(eye offset) * head view with the eye offset (-0.032 / +0.032, 0.002, -0.004) m, and the camera-relative
// clumps (vertices relative to the HEAD, as the game's) get cb0 = projection * T(eye offset) * head view ROTATION, so their MVP
// column 3 = P * (eye offset, 1), not (0, 0, near, 0). Each eye is its own 100-frame run with its own CameraMv / DrawIdMv (the DLL
// keeps one unit per eye and pairs eye 0 with eye 0); the offset is fixed per run (alternating the sign per frame would be an eye
// swap the DLL never sees). Same pass criterion as flat (every clump pixel exactly the camera motion, every frame), plus the round-7
// diagnostics: the read-back max |x| / |y| / |w| among the origin-free draws equal the eye offset's p00 |ex|, p11 |ey|, |ez| (flat:
// the float residues only), and the nearest lateral miss of the depth-ok draws (posts / the car crossing the eye plane) lies over
// the 0.25 tolerance.
int g_s15Mut = 0;
int RunRoadsideScene(Env& E, float snapPx, Totals& tot, int mut, int eye) {
    const int frames = 100;
    const double eyeX = eye == 1 ? -0.032 : (eye == 2 ? 0.032 : 0.0), eyeY = eye ? 0.002 : 0.0, eyeZ = eye ? -0.004 : 0.0;
    printf("\n===== S15 roadside%s: camera-relative grass (16 non-instanced clumps + 13 instanced batches of 2 / 3 / 8), a car "
           "overtaking alongside the clumps (plate + 1-instance instanced plate), an oncoming car, a parked van (%d frames, snap "
           "%.3f px%s) =====\n", eye == 1 ? " VR EYE 0 (eye offset -0.032 0.002 -0.004 m)" : (eye == 2 ? " VR EYE 1 (eye offset "
           "+0.032 0.002 -0.004 m)" : ""), frames, (double)snapPx, (mut & 4) ? ", MUTATION: origin-free rule off" :
           ((mut & 8) ? ", MUTATION: multi-instance draws listed for the vote (round-4 rule)" :
           ((mut & 16) ? ", MUTATION: the round-6 origin-free rule (|x|, |y|, |w| <= 1e-4 |z|)" : "")));
    ID3D11Device* dev = E.dev.Get();
    ID3D11DeviceContext* ctx = E.ctx.Get();
    ID3D11DeviceContext1* ctx1 = E.ctx1.Get();
    DrawIdMv::SetParams(8.0f, snapPx);
    DrawIdMv::SetConsensus(true);
    const float attachSaved = DrawIdMv::AttachM();
    const bool twinSaved = DrawIdMv::Twin(), parentSaved = DrawIdMv::Parent(), instSaved = DrawIdMv::Instanced();
    DrawIdMv::SetAttach(0.5f);
    DrawIdMv::SetTwin(true);
    DrawIdMv::SetParent(true);
    DrawIdMv::SetInstanced(true);
    DrawIdMv::SetParentMutation(mut);
    CameraMv cam;
    if (!cam.Init(dev)) { printf("CameraMv init failed\n"); return 1; }
    cam.SetEgoPixel(0.0f);
    cam.SetEgoOrigin(0.0f);
    CandidateRecord cand;
    cand.Init(dev);
    DrawIdRecord rec;
    constexpr int kCR = 16, kInstSlots = 64;
    ComPtr<ID3D11Buffer> bones, instVb;
    ComPtr<ID3D11ShaderResourceView> bonesSrv;
    {
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = kCR * 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 16;
        D3D11_BUFFER_DESC ib{}; ib.ByteWidth = kInstSlots * 12; ib.Usage = D3D11_USAGE_DYNAMIC; ib.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        ib.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &bones)) || FAILED(dev->CreateShaderResourceView(bones.Get(), nullptr, &bonesSrv)) ||
            FAILED(dev->CreateBuffer(&ib, nullptr, &instVb))) { printf("S15 buffers failed\n"); return 1; }
    }
    // ---- objects ----
    enum RK { kWorld, kCar, kCRClump, kInstClump, kInstPlate };
    struct RO { const char* name; RK kind; int mesh; int inst; int sinst; int group; };
    std::vector<RO> objs;
    auto add = [&](const char* n, RK k, int mesh, int inst, int sinst, int group) {
        objs.push_back({ n, k, mesh, inst, sinst, group }); return (int)objs.size() - 1; };
    const int oGround = add("ground", kWorld, MGround, 1, 0, 0);
    int oBox[10];
    for (int k = 0; k < 10; ++k) oBox[k] = add("static box (10 unique)", kWorld, MBox0 + k, 1, 0, 0);
    int oPost[40];
    for (int k = 0; k < 40; ++k) oPost[k] = add("roadside post (40 identical)", kWorld, MPost, 1, 0, 0);
    const int oCarO = add("car OVERTAKING alongside the right-hand clumps", kCar, MCar0, 1, 0, 1);
    const int oPlateO = add("its plate (node matrix, G-buffer)", kCar, MPlate, 1, 0, 1);
    const int oCarC = add("ONCOMING car passing the left-hand clumps", kCar, MCar0 + 1, 1, 0, 2);
    const int oVan = add("PARKED van among the right-hand clumps", kWorld, MFarVan, 1, 0, 3);
    int oCR[kCR];
    for (int k = 0; k < kCR; ++k) oCR[k] = add("CAMERA-RELATIVE grass clump (non-instanced)", kCRClump, MClumpCR0 + k, 1, 0, 4);
    int oIB[13], slot = 0;
    for (int k = 0; k < 12; ++k) {
        const int n = (k & 1) ? 3 : 2;
        oIB[k] = add("CAMERA-RELATIVE instanced grass batch (2 / 3 instances)", kInstClump, MClump, n, slot, 5);
        slot += n;
    }
    oIB[12] = add("CAMERA-RELATIVE instanced grass batch (8 instances)", kInstClump, MClump, 8, slot, 5);
    slot += 8;
    const int oIP = add("1-instance INSTANCED plate on the overtaking car (world VP)", kInstPlate, MPlateF, 1, slot, 6);
    slot += 1;
    const int nObj = (int)objs.size();
    (void)oGround;
    auto isClump = [&](int o) { return objs[o].kind == kCRClump || objs[o].kind == kInstClump; };
    std::vector<M4> mvpPrev(nObj, I4()), mvpCur(nObj, I4());
    std::mt19937 rng(1515);
    int checks = 0, fails = 0, consFrames = 0, camRBad = 0;
    long long clumpPx = 0, clumpBadPx = 0, worldPx = 0, worldBadPx = 0, carPx = 0, carBadPx = 0, ipPx = 0, ipBadPx = 0;
    int clumpBadFrames = 0, crMoverFrames = 0, crNotStatic = 0, ibParent = 0, ibNotInst = 0, ipParentOk = 0, ipFrames = 0;
    int carNearClump = 0;
    double worstClump = 0.0, worstCamPx = 0.0;
    DrawIdMv::Stats st0{};
    uint64_t rb0 = 0;
    if (g_rstatic) DrawIdMv::StaticClear(DrawIdMv::kStatClrConfig);
    M4 VprevX = I4();
    const M4 Pw = Proj(kNearWorld);
    const FLOAT zero[4] = { 0, 0, 0, 0 };
    const D3D11_RECT sc0 = { 0, 0, W, H };
    GpuPerf::Restart();
    for (int f = 0; f < frames; ++f) {
        PerfFrame();
        const double camZ = 0.5 * f;
        const M4 camPose = Mul(T(0.0, 1.6, camZ), RotY(0.002 * f));
        M4 Vh; Inv(camPose, Vh);                          // the HEAD's view (flat: the camera's)
        const M4 Te = T(eyeX, eyeY, eyeZ);                // round 7: the eye offset (flat: identity -- bit-identical to round 6)
        const M4 V = Mul(Te, Vh);                         // the eye's view: every world draw, the camera R
        const M4 VPw = Mul(Pw, V);
        // the game's camera-relative matrix: projection * view ROTATION (+ the float residues the capture shows in column 3);
        // round 7 VR: projection * T(eye offset) * head view rotation (the clumps' vertices are relative to the head)
        M4 Vrot = Vh;
        Vrot.m[0][3] = Vrot.m[1][3] = Vrot.m[2][3] = 0.0;
        M4 MVPcr = Mul(Pw, Mul(Te, Vrot));
        MVPcr.m[0][3] += 3.5e-9; MVPcr.m[1][3] += 2.0e-6; MVPcr.m[3][3] += -1.2e-7;
        std::vector<M4> world(nObj, I4());
        world[oGround] = T(0, 0, 100);
        for (int k = 0; k < 10; ++k) world[oBox[k]] = T((k & 1) ? 10.0 + k : -10.0 - k, 0.0, 30.0 + 12.0 * k);
        for (int k = 0; k < 40; ++k) world[oPost[k]] = T((k & 1) ? 7.0 : -7.0, 0.0, 8.0 + 5.0 * (k / 2));
        const M4 carO = T(2.25, 0.0, 6.0 + 0.75 * f);                       // 0.25 m / frame faster than the camera
        world[oCarO] = carO;
        world[oPlateO] = Mul(carO, T(0.0, 0.6, -2.02));
        world[oCarC] = Mul(T(-2.3, 0.0, 110.0 - 1.0 * f), RotY(3.14159265358979));
        world[oVan] = T(5.0, 0.0, 52.0);
        double crPos[kCR][3];
        for (int k = 0; k < kCR; ++k) {
            const bool right = k < 8;
            crPos[k][0] = right ? 3.35 + 0.15 * (k % 3) : -3.4 - 0.15 * (k % 3);
            crPos[k][1] = 0.0;
            crPos[k][2] = right ? 14.0 + 9.0 * k : 16.0 + 9.0 * (k - 8);
            world[oCR[k]] = T(crPos[k][0], crPos[k][1], crPos[k][2]);   // (the truth MVP; the draw itself is camera-relative)
        }
        // per-frame bones (camera-relative clump positions) and instance offsets (camera-relative batches + the world-space plate)
        {
            float bv[kCR * 4] = {};
            for (int k = 0; k < kCR; ++k) {
                bv[k * 4 + 0] = (float)(crPos[k][0] - 0.0); bv[k * 4 + 1] = (float)(crPos[k][1] - 1.6); bv[k * 4 + 2] = (float)(crPos[k][2] - camZ);
            }
            ctx->UpdateSubresource(bones.Get(), 0, nullptr, bv, 0, 0);
            D3D11_MAPPED_SUBRESOURCE mp{};
            if (FAILED(ctx->Map(instVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { printf("S15 instance map failed\n"); return 1; }
            float* iv = (float*)mp.pData;
            memset(iv, 0, kInstSlots * 12);
            for (int k = 0; k < 13; ++k) {
                const RO& ob = objs[oIB[k]];
                const double side = (k < 12) ? ((k < 6) ? 1.0 : -1.0) : 1.0;
                const double bx = k < 12 ? side * (3.6 + 0.2 * (k % 2)) : 3.9;
                const double bz = k < 12 ? 18.0 + 11.0 * (k % 6) + (side < 0 ? 5.0 : 0.0) : 75.0;
                for (int i = 0; i < ob.inst; ++i) {
                    float* q = iv + (ob.sinst + i) * 3;
                    q[0] = (float)(bx + 0.1 * i - 0.0); q[1] = (float)(0.0 - 1.6); q[2] = (float)(bz + (k < 12 ? 0.8 : 0.6) * i - camZ);
                }
            }
            double pl[4] = { 0.0, 1.0, -2.03, 1.0 }, pw[4];
            Xf(carO, pl, pw);
            float* q = iv + objs[oIP].sinst * 3;
            q[0] = (float)pw[0]; q[1] = (float)pw[1]; q[2] = (float)pw[2];
            world[oIP] = T(pw[0], pw[1], pw[2]);
            ctx->Unmap(instVb.Get(), 0);
        }
        for (int o = 0; o < nObj; ++o) mvpCur[o] = Mul(Pw, Mul(V, world[o]));
        // ---- draw order (shuffled), cbuffer windows ----
        std::vector<int> order(nObj);
        for (int o = 0; o < nObj; ++o) order[o] = o;
        std::shuffle(order.begin(), order.end(), rng);
        const int nd = nObj;
        ID3D11Buffer* ring = E.ring[f & 1].Get();
        {
            D3D11_MAPPED_SUBRESOURCE mp{};
            if (FAILED(ctx->Map(ring, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { printf("ring map failed\n"); return 1; }
            float* p = (float*)mp.pData;
            memset(p, 0, 2u << 20);
            for (int d = 0; d < nd; ++d) {
                const int o = order[d];
                const RK k = objs[o].kind;
                const M4& m = (k == kCRClump || k == kInstClump) ? MVPcr : (k == kInstPlate ? VPw : mvpCur[o]);
                float* w = p + (UINT)d * 64u;
                for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) w[(4 + r) * 4 + c] = (float)m.m[r][c];
                w[8 * 4 + 0] = (float)(o + 1); w[8 * 4 + 1] = (float)(d + 1); w[8 * 4 + 2] = 0.0f;
            }
            ctx->Unmap(ring, 0);
        }
        // ---- G-buffer pass ----
        ctx->ClearDepthStencilView(E.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
        ctx->ClearRenderTargetView(E.truthRtv.Get(), zero);
        ctx->ClearRenderTargetView(E.colorRtv.Get(), zero);
        ID3D11RenderTargetView* rtvs[2] = { E.truthRtv.Get(), E.colorRtv.Get() };
        ctx->OMSetRenderTargets(2, rtvs, E.dsv.Get());
        ctx->OMSetDepthStencilState(E.dss.Get(), 1);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        auto halton = [](int i, int b) { double q = 0, fct = 1; while (i > 0) { fct /= b; q += fct * (i % b); i /= b; } return q; };
        const float jx = (float)(halton(f % 8 + 1, 2) - 0.5), jy = (float)(halton(f % 8 + 1, 3) - 0.5);
        const D3D11_VIEWPORT vpW = { jx, jy, (float)(W + g_vpOver), (float)(H + g_vpOver), 0.01f, 0.9f };
        ctx->PSSetShader(E.ps.Get(), nullptr, 0);
        rec.Reset();
        cand.Reset();
        cand.SetDropped(cam.MedoidDropBits());
        for (int d = 0; d < nd; ++d) {
            const int o = order[d];
            const RO& ob = objs[o];
            const Mesh& m = E.meshes[ob.mesh];
            const bool inst = ob.kind == kInstClump || ob.kind == kInstPlate;
            ctx->RSSetViewports(1, &vpW);
            ctx->RSSetScissorRects(1, &sc0);
            ctx->RSSetState((ob.kind == kWorld || ob.kind == kCar) && ob.mesh != MGround && ob.mesh != MPlate ? E.rsBack.Get() : E.rsNone.Get());
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11Buffer* vbs[2] = { E.vb[0].Get(), inst ? instVb.Get() : E.instVb[0].Get() };
            const UINT strides[2] = { sizeof(Vtx), 12 }, offs[2] = { 0, 0 };
            ctx->IASetVertexBuffers(0, 2, vbs, strides, offs);
            ctx->IASetIndexBuffer(E.ib[0].Get(), DXGI_FORMAT_R32_UINT, 0);
            ID3D11ShaderResourceView* vsSrv[4] = { nullptr, nullptr, nullptr, ob.kind == kCRClump ? bonesSrv.Get() : nullptr };
            ctx->VSSetShaderResources(0, 4, vsSrv);
            if (inst) { ctx->IASetInputLayout(E.ilInst.Get()); ctx->VSSetShader(E.vsInst.Get(), nullptr, 0); }
            else { ctx->IASetInputLayout(E.il.Get()); ctx->VSSetShader(ob.kind == kCRClump ? E.vsSkin.Get() : E.vsMain.Get(), nullptr, 0); }
            const UINT first = (UINT)d * 16u, num = 16u;
            ctx1->VSSetConstantBuffers1(0, 1, &ring, &first, &num);
            ctx1->PSSetConstantBuffers1(0, 1, &ring, &first, &num);
            if (!inst) {
                CandidateRecord::DrawKey k{};
                k.ib = E.ib[0].Get(); k.vb = E.vb[0].Get(); k.indexCount = m.ic; k.startIndex = m.si; k.baseVertex = m.bv;
                if (cand.Count(0) < CandidateRecord::kSlots && !cand.Dropped(0)) cand.Record(ctx, 0, ring, first * 16u + 64u, k);
            }
            rec.Record(ctx1, m.ic, inst ? (UINT)ob.inst : 1u, m.si, m.bv, inst ? (UINT)ob.sinst : 0u, inst);
            if (inst) ctx->DrawIndexedInstanced(m.ic, (UINT)ob.inst, m.si, m.bv, (UINT)ob.sinst);
            else ctx->DrawIndexed(m.ic, m.si, m.bv);
        }
        // ---- G-buffer leave: replay (instanced draws too, as S8), depth snapshot, MV generation ----
        DrawIdMv::SetFrame((uint64_t)f + 1);
        DrawIdRecord::SetInstancedProbe((f % 7) == 0);
        if (g_rstatic) { DrawIdMv::PollMain(ctx); DrawIdRecord::SetStaticGate(true); }
        if (!rec.Replay(ctx1, E.idRtv.Get(), E.roDsv.Get(), false, true).ok) { printf("   FAIL replay (frame %d)\n", f); ++fails; }
        DrawIdRecord::SetStaticGate(false);
        DrawIdRecord::SetInstancedProbe(true);
        ID3D11RenderTargetView* nullRtv = nullptr;
        ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
        ctx->CopyResource(E.twin.Get(), E.depth.Get());
        CameraMv::FrameStats fs;
        const bool ok = cam.Generate(ctx, W, H, 0, 0, W, H, cand, E.twinSrv.Get(), E.dUav.Get(), E.mvUav.Get(), &fs, false,
                                     nullptr, nullptr, nullptr, &rec, rec.ReplayFailed() ? nullptr : E.idSrv.Get());
        if (!ok) { printf("   FAIL Generate (frame %d)\n", f); ++fails; }
        // ---- readbacks ----
        ctx->CopyResource(E.stMv.Get(), E.mv.Get());
        ctx->CopyResource(E.stDepth.Get(), E.dOut.Get());
        ctx->CopyResource(E.stTruth.Get(), E.truth.Get());
        const int nTab = (int)cam.DidN();
        if (cam.DidRSrv() && nTab) {
            ComPtr<ID3D11Resource> rres;
            cam.DidRSrv()->GetResource(&rres);
            const D3D11_BOX box{ 0, 0, 0, (UINT)nTab * DrawIdMv::kRStride * 16u, 1, 1 };
            ctx->CopySubresourceRegion(E.stR.Get(), 0, 0, 0, 0, rres.Get(), 0, &box);
        }
        D3D11_MAPPED_SUBRESOURCE mMv{}, mD{}, mT{}, mR{};
        ctx->Map(E.stMv.Get(), 0, D3D11_MAP_READ, 0, &mMv);
        CmpFrame(mMv, W, H);                            // the comparison mode (cmpdump= / cmp=) covers S15 too
        ctx->Map(E.stDepth.Get(), 0, D3D11_MAP_READ, 0, &mD);
        ctx->Map(E.stTruth.Get(), 0, D3D11_MAP_READ, 0, &mT);
        ctx->Map(E.stR.Get(), 0, D3D11_MAP_READ, 0, &mR);
        const float* Rt = (const float*)mR.pData;
        std::vector<int> stCur(nObj, -1), parentOf(nObj, -1), tabIdx(nObj, -1);
        for (int d = 0; d < nd; ++d) tabIdx[order[d]] = d;
        for (int o = 0; o < nObj; ++o) {
            const int ti = tabIdx[o];
            if (ti < 0 || ti >= nTab) continue;
            stCur[o] = (int)(Rt[(ti * 5 + 4) * 4] + 0.5f);
            const float w = Rt[(ti * 5 + 4) * 4 + 3];
            if (ParentMarked(w)) parentOf[o] = (int)(-(double)w - 1.0 - 2.0 * DrawIdMv::kMax + 0.5);
        }
        // ---- the camera R: GPU (consensus) vs exact ----
        M4 RcamG, RcamX;
        {
            if (f == 0) VprevX = V;
            M4 Pinv, Vinv; Inv(Pw, Pinv); Inv(V, Vinv);
            RcamX = Mul(Pw, Mul(VprevX, Mul(Vinv, Pinv)));
            VprevX = V;
            float sv[CameraMv::kSolveFloats];
            if (!cam.ReadSolveBlocking(ctx, sv)) { printf("   FAIL solve readback\n"); ++fails; }
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) RcamG.m[r][c] = sv[r * 4 + c];
            if (sv[35] > 0.5f) ++consFrames;
            double worst = 0.0;
            for (int gy = 0; gy < 7; ++gy) for (int gx = 0; gx < 7; ++gx) for (double zq : { 0.0, kNearWorld / 40.0, kNearWorld / 5.0 }) {
                const double p4[4] = { -0.9 + 0.3 * gx, -0.9 + 0.3 * gy, zq, 1.0 };
                double a[4], b[4];
                Xf(RcamX, p4, a); Xf(RcamG, p4, b);
                if (!(a[3] > 1e-9) || !(b[3] > 1e-9)) { worst = 1e9; continue; }
                worst = std::max(worst, std::hypot((a[0] / a[3] - b[0] / b[3]) * W * 0.5, (a[1] / a[3] - b[1] / b[3]) * H * 0.5));
            }
            if (f >= 2) {
                ++checks;
                if (!(worst <= 0.05)) { ++fails; ++camRBad; if (camRBad <= 3) printf("   FAIL frame %d: camera R off by %.3f px\n", f, worst); }
                worstCamPx = std::max(worstCamPx, worst);
            }
        }
        // ---- per-draw states (frame 2 on) ----
        if (f >= 2) {
            bool crMover = false;
            for (int k = 0; k < kCR; ++k) {
                ++checks;
                if (stCur[oCR[k]] == DrawIdMv::kStMover) crMover = true;
                if (stCur[oCR[k]] != DrawIdMv::kStStatic || parentOf[oCR[k]] >= 0) {
                    ++fails; ++crNotStatic;
                    if (crNotStatic <= 3) printf("   FAIL frame %d: camera-relative clump %d state %d (parent %d), want STATIC (2) by its own pair\n",
                                                 f, k, stCur[oCR[k]], parentOf[oCR[k]]);
                }
            }
            if (crMover) ++crMoverFrames;
            for (int k = 0; k < 13; ++k) {
                ++checks;
                if (parentOf[oIB[k]] >= 0) {
                    ++fails; ++ibParent;
                    if (ibParent <= 3) printf("   FAIL frame %d: instanced grass batch %d (%d instances) took parent %d (state %d)\n", f, k,
                                              objs[oIB[k]].inst, parentOf[oIB[k]], stCur[oIB[k]]);
                } else if (stCur[oIB[k]] != DrawIdMv::kStInstanced) { ++fails; ++ibNotInst; }
            }
            ++ipFrames;
            if (stCur[oIP] == DrawIdMv::kStMover && parentOf[oIP] == tabIdx[oCarO]) ++ipParentOk;
        }
        if (f == 10) { st0 = cam.DrawIds().GetStats(); rb0 = st0.rbFrames; }
        // ---- expected motion per object: the cars (+ their plates) their own exact R, everything else the camera R ----
        std::vector<M4> Rexp(nObj, RcamG);
        if (f >= 1) {
            M4 ic;
            Inv(mvpCur[oCarO], ic);
            const M4 Ro = Mul(mvpPrev[oCarO], ic);
            Rexp[oCarO] = Ro; Rexp[oPlateO] = Ro; Rexp[oIP] = Ro;
            Inv(mvpCur[oCarC], ic);
            Rexp[oCarC] = Mul(mvpPrev[oCarC], ic);
        }
        // ---- per pixel ----
        bool clumpBad = false, nearC = false;
        for (int y = 0; y < H; ++y) {
            const uint16_t* mvRow = (const uint16_t*)((const uint8_t*)mMv.pData + y * mMv.RowPitch);
            const float* dRow = (const float*)((const uint8_t*)mD.pData + y * mD.RowPitch);
            const uint32_t* tRow = (const uint32_t*)((const uint8_t*)mT.pData + y * mT.RowPitch);
            for (int x = 0; x < W; ++x) {
                const uint32_t tObj = tRow[x * 2];
                if (!tObj || f < 1) continue;
                const int o = (int)tObj - 1;
                // the overtaking car within 5 px of a clump pixel (the scene exercises "a mover right beside the grass")
                if (!nearC && f >= 2 && isClump(o) && x >= 5 && y >= 5 && x < W - 5 && y < H - 5) {
                    for (int dy = -5; dy <= 5 && !nearC; dy += 5) {
                        const uint32_t* t2 = (const uint32_t*)((const uint8_t*)mT.pData + (y + dy) * mT.RowPitch);
                        for (int dx = -5; dx <= 5; dx += 5) if (t2[(x + dx) * 2] == (uint32_t)(oCarO + 1)) { nearC = true; break; }
                    }
                }
                const double dd = dRow[x];
                const double z = (dd - 0.01) / 0.89;
                const double u = (x + 0.5) / W, vv = (y + 0.5) / H;
                const double p4[4] = { u * 2 - 1, 1 - vv * 2, std::min(1.0, std::max(0.0, z)), 1.0 };
                double c[4];
                Xf(Rexp[o], p4, c);
                double ex = 0, ey = 0;
                if (c[3] > 1e-6) { ex = ((c[0] / c[3]) * 0.5 + 0.5 - u) * W; ey = ((0.5 - (c[1] / c[3]) * 0.5) - vv) * H; }
                const double gx = H2F(mvRow[x * 2]), gy = H2F(mvRow[x * 2 + 1]);
                const double err = std::hypot(gx - ex, gy - ey);
                const bool mover = o == oCarO || o == oPlateO || o == oCarC || o == oIP;
                const double tol = (mover ? 0.03 + snapPx : 0.03) + std::max(std::fabs(ex), std::fabs(ey)) / 1024.0;
                if (isClump(o)) {
                    ++clumpPx;
                    worstClump = std::max(worstClump, err);
                    if (err > tol) {
                        ++clumpBadPx;
                        if (clumpBadPx <= 3) printf("   FAIL frame %d: clump pixel (%d,%d) of %s: MV (%.3f %.3f), camera motion (%.3f %.3f)\n",
                                                    f, x, y, objs[o].name, gx, gy, ex, ey);
                        clumpBad = true;
                    }
                } else if (o == oIP) { ++ipPx; if (err > tol) ++ipBadPx; }
                else if (mover) { ++carPx; if (err > tol) ++carBadPx; }
                else { ++worldPx; if (err > tol) ++worldBadPx; }
            }
        }
        if (clumpBad) ++clumpBadFrames;
        if (nearC) ++carNearClump;
        ctx->Unmap(E.stMv.Get(), 0); ctx->Unmap(E.stDepth.Get(), 0); ctx->Unmap(E.stTruth.Get(), 0); ctx->Unmap(E.stR.Get(), 0);
        for (int o = 0; o < nObj; ++o) mvpPrev[o] = mvpCur[o];
    }
    rec.Shutdown();
    // ---- scene totals ----
    const DrawIdMv::Stats& ds = cam.DrawIds().GetStats();
    const uint64_t rbN = ds.rbFrames - rb0;
    uint64_t hist[8];
    for (int q = 0; q < 8; ++q) hist[q] = ds.instHist[q] - st0.instHist[q];
    const uint64_t instSum = ds.instSum - st0.instSum, oFree = ds.originFree - st0.originFree;
    printf("   instance counts as the shaders saw them over %llu readbacks (frames 10+): 0 inst %llu, 1 inst %llu, 2 inst %llu, 3 inst %llu, "
           "4..6 inst %llu, 7+ inst %llu, sum %llu (want per readback: 1 / 6 / 6 / 1 draws with 1 / 2 / 3 / 8 instances, sum 39)\n",
           (unsigned long long)rbN, (unsigned long long)hist[0], (unsigned long long)hist[1], (unsigned long long)hist[2],
           (unsigned long long)hist[3], (unsigned long long)(hist[4] + hist[5] + hist[6]), (unsigned long long)hist[7],
           (unsigned long long)instSum);
    checks += 2;
    if (!rbN || hist[0] || hist[1] != rbN || hist[2] != 6 * rbN || hist[3] != 6 * rbN || hist[4] || hist[5] || hist[6] ||
        hist[7] != rbN || instSum != 39 * rbN) { ++fails; printf("   FAIL instance-count plumbing (CPU record -> Tab -> shader)\n"); }
    printf("   origin-free (camera-relative) paired draws held static: %.2f per readback (want 16 = the non-instanced clumps; the "
           "mutation: 0)\n", rbN ? (double)oFree / (double)rbN : 0.0);
    if (!(mut & 4) && oFree != 16 * rbN) { ++fails; printf("   FAIL origin-free count\n"); }
    // round 7 diagnostics (the 'MV draw-ids' log fields): the max |x| |y| |w| of the origin-free draws (frames 0..99: the
    // clumps' column 3 = P * (eye offset, 1) + the residues -- exact), the depth-only / lateral split (here: roadside posts and
    // the overtaking car crossing the eye plane, |w| < 0.20 m with |x| or |y| far over 0.50 -- informational, the nearest miss
    // must lie over the tolerance), the nearest own-pair mover
    {
        float od[5];
        cam.DrawIds().TakeOriginDiag(od);
        const uint64_t dOnly = ds.originDepthOnly - st0.originDepthOnly, mvNear = ds.originMoverNear - st0.originMoverNear;
        const double wantX = Pw.m[0][0] * std::fabs(eyeX) + 3.5e-9, wantY = Pw.m[1][1] * std::fabs(eyeY) + 2.0e-6,
                     wantW = std::fabs(eyeZ) + 1.2e-7;
        printf("   round 7 origin diagnostics: origin-free max |x| %.6g |y| %.6g |w| %.6g (want %.6g / %.6g / %.6g); depth ok but "
               "lateral over 0.50: %llu draw-readbacks (world geometry crossing the eye plane), nearest lateral miss %.4f; own-pair "
               "movers with origin |w| < 0.5 m: %llu, min mover |w| %.3f m\n", (double)od[0], (double)od[1], (double)od[2], wantX,
               wantY, wantW, (unsigned long long)dOnly, (double)od[4], (unsigned long long)mvNear, (double)od[3]);
        checks += 2;
        auto closeTo = [](double a, double b) { return std::fabs(a - b) <= 1e-6 + 1e-4 * std::fabs(b); };
        if (!closeTo(od[0], wantX) || !closeTo(od[1], wantY) || !closeTo(od[2], wantW)) {
            ++fails; printf("   FAIL the read-back max |x| |y| |w| of the origin-free draws\n");
        }
        // (od covers every readback of the scene, dOnly frames 10+: the miss check uses the whole-scene count)
        if (ds.originDepthOnly ? !(od[4] > 0.25f) : !(od[4] < 0.0f)) { ++fails; printf("   FAIL the nearest lateral miss read-back\n"); }
    }
    checks += 4;
    if (clumpBadPx) { ++fails; }
    if (carNearClump == 0) { ++fails; printf("   FAIL the overtaking car never came within 5 px of a clump (the scene must exercise it)\n"); }
    if (carBadPx * 100 > carPx) { ++fails; printf("   FAIL car / plate pixels off their motion: %lld of %lld\n", carBadPx, carPx); }
    if (worldBadPx * 1000 > worldPx) { ++fails; printf("   FAIL static world pixels off the camera motion: %lld of %lld\n", worldBadPx, worldPx); }
    printf("   clump pixels off the camera motion: %lld of %lld in %d frames (worst %.3f px; must be 0); camera-relative clumps not static: "
           "%d draw-frames (frames with one a MOVER: %d); instanced batches with a parent: %d, not state 4: %d; car within 5 px of a "
           "clump in %d frames\n", clumpBadPx, clumpPx, clumpBadFrames, worstClump, crNotStatic, crMoverFrames, ibParent, ibNotInst,
           carNearClump);
    printf("   static world px off: %lld of %lld; car + node plate px off: %lld of %lld; 1-instance instanced plate: took the car in %d of "
           "%d frames, px off %lld of %lld (informational: phase 6b); camera R consensus %d frames, worst vs exact %.4f px\n",
           worldBadPx, worldPx, carBadPx, carPx, ipParentOk, ipFrames, ipBadPx, ipPx, consFrames, worstCamPx);
    PrintPhase8(ctx, ds, "");
    DrawIdMv::SetParentMutation(0);
    DrawIdMv::SetAttach(attachSaved); DrawIdMv::SetTwin(twinSaved); DrawIdMv::SetParent(parentSaved); DrawIdMv::SetInstanced(instSaved);
    printf("   %s: %d / %d checks passed\n", fails ? "FAILED" : "passed", checks - fails, checks);
    tot.checks += checks; tot.fails += fails;
    return fails;
}

// ---- v0.10.0 phase 19: S17 CAR (ATS VR car, captures/dlaa_inject_ats_v0100p17_vr.log) ------------------------------------------
// The player sits in a car driving 0.5 m / frame over a static world (ground, boxes, 60 posts, an oncoming car); the head turns
// like a VR head (yaw +-0.35 rad, pitch +-0.12 rad, a 2 / 1.5 cm head bob) inside the car. Cabin layer (viewport [0.9, 1], a
// FINITE reversed-Z projection like the game's cabin: near 0.05, far 15 m) -- variant 0 = the in-game signature: the car interior's
// four big draws (dashboard + console sharing one matrix, two doors) keep their MVP one cb0 row LATER (row 4 = (0, 0, 0, 1), MVP in
// rows 5..8: the injector's rows 4..7 read is not their MVP), two small real cabin parts, an air freshener swinging on its string
// (a real mover), three camera-relative overlays (cab-fixed, drawn as projection * head rotation + per-frame offsets: their own R is
// the head ROTATION only, as the in-game ic 18 / ic 6 pair); variant 1 = the same car with a VALID interior layout: ONE big body
// draw (1440 indices) and two small parts swinging together (a dangling tag: 2 draws, 6 indices each -- more draws than the body,
// far less IndexCount: the small-cabin rule must rank by IndexCount). The own car's hood is a WORLD-layer draw moving with the car.
// Checks: every cabin-depth pixel's motion vector = the exact cabin motion (head motion inside the car) -- the freshener / tag its
// own --, the hood its own, the world pixels the camera motion; the cabin camera R (solve rows 4..7) = the exact one; the states
// (variant 0: the big draws state 5 "no MVP", 4 per readback counted by the MVP-shape rule). Mutations that must FAIL: "mutshape"
// (variant 0: the big draws pair and become movers with an R in the wrong coordinates -- the hole), "cabsmall=0" (both: the medoid
// of pass A is an overlay's rotation-only R, every interior pixel off by the head-bob parallax).
int g_s17Mut = 0;
int g_layMut = 0;        // v0.10.0 phase 22 "mutlayout": SetParentMutation bit 6 (the MVP layout detection off) in S17 / S18
int RunCarScene(Env& E, float snapPx, Totals& tot, int variant) {
    const int frames = 100;
    const bool shifted = variant == 0;
    printf("\n===== S17 car%s (%d frames, snap %.3f px; mv_cabin_small %u%s) =====\n",
           shifted ? ": interior's 4 big draws with the MVP one cb0 row later (in-game signature), 2 small real parts, a swinging "
                     "freshener, 3 camera-relative overlays, the hood (world layer), VR head motion" :
                     " b: VALID interior layout -- ONE big body draw (1440 indices) + a 2-draw swinging tag (6 indices each), 3 "
                     "camera-relative overlays, the hood, VR head motion",
           frames, (double)snapPx, DrawIdMv::CabinSmall(), (g_s17Mut & 32) ? ", MUTATION: the MVP-shape rule off" :
           (g_layMut ? ", MUTATION: the MVP layout detection off (rows 4..7)" : ""));
    ID3D11Device* dev = E.dev.Get();
    ID3D11DeviceContext* ctx = E.ctx.Get();
    ID3D11DeviceContext1* ctx1 = E.ctx1.Get();
    DrawIdMv::SetParams(8.0f, snapPx);
    DrawIdMv::SetConsensus(true);
    const float attachSaved = DrawIdMv::AttachM();
    const bool twinSaved = DrawIdMv::Twin(), parentSaved = DrawIdMv::Parent(), instSaved = DrawIdMv::Instanced();
    DrawIdMv::SetAttach(0.5f);
    DrawIdMv::SetTwin(true);
    DrawIdMv::SetParent(true);
    DrawIdMv::SetInstanced(true);
    DrawIdMv::SetParentMutation(g_s17Mut | g_layMut);
    CameraMv cam;
    if (!cam.Init(dev)) { printf("CameraMv init failed\n"); return 1; }
    cam.SetEgoPixel(0.0f);
    cam.SetEgoOrigin(0.0f);
    CandidateRecord cand;
    cand.Init(dev);
    DrawIdRecord rec;
    ComPtr<ID3D11Buffer> bones;
    ComPtr<ID3D11ShaderResourceView> bonesSrv;
    {
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 3 * 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 16;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &bones)) || FAILED(dev->CreateShaderResourceView(bones.Get(), nullptr, &bonesSrv))) {
            printf("S17 buffers failed\n"); return 1;
        }
    }
    // ---- objects ----
    enum CK { kWorld, kOncoming, kHood, kCabFixed, kCabShift, kCabSwing, kOverlay };
    struct CO { const char* name; CK kind; int mesh; bool cullNone; double rel[3]; };   // rel: cabin position relative to the head rest
    std::vector<CO> objs;
    auto add = [&](const char* n, CK k, int mesh, bool cn, double x = 0, double y = 0, double z = 0) {
        objs.push_back({ n, k, mesh, cn, { x, y, z } }); return (int)objs.size() - 1; };
    const int oGround = add("ground", kWorld, MGround, true);
    int oBox[10];
    for (int k = 0; k < 10; ++k) oBox[k] = add("static box", kWorld, MBox0 + k, false);
    int oPost[60];
    for (int k = 0; k < 60; ++k) oPost[k] = add("roadside post (60 identical)", kWorld, MPost, false);
    const int oCarC = add("ONCOMING car", kOncoming, MCar0 + 2, false);
    const int oHood = add("own car's HOOD (world layer, moves with the car)", kHood, MS17Hood, false);
    std::vector<int> big, smallReal, swing, ovl;
    if (shifted) {
        big.push_back(add("interior dashboard (MVP in rows 5..8)", kCabShift, MS17Dash, false, 0.0, -0.40, 0.75));
        big.push_back(add("interior console (the dashboard's matrix, MVP in rows 5..8)", kCabShift, MS17Console, false, 0.0, -0.40, 0.75));
        big.push_back(add("interior left door (MVP in rows 5..8)", kCabShift, MS17DoorL, false, -0.60, -0.25, 0.45));
        big.push_back(add("interior right door (MVP in rows 5..8)", kCabShift, MS17DoorR, false, 0.60, -0.25, 0.45));
        smallReal.push_back(add("small real cabin part (a knob)", kCabFixed, MS17Real0, false, 0.20, -0.30, 0.55));
        smallReal.push_back(add("small real cabin part (a switch)", kCabFixed, MS17Real1, false, -0.25, -0.32, 0.60));
        swing.push_back(add("air freshener swinging on its string (a real cabin mover)", kCabSwing, MS17Fresh, true, 0.0, 0.15, 0.50));
    } else {
        big.push_back(add("interior BODY, one big draw (1440 indices)", kCabFixed, MS17Body, false, 0.0, -0.45, 0.90));
        swing.push_back(add("dangling tag, part 1 (swings)", kCabSwing, MS17Fresh, true, 0.0, 0.15, 0.50));
        swing.push_back(add("dangling tag, part 2 (swings with part 1)", kCabSwing, MS17Fresh2, true, 0.0, 0.15, 0.50));
    }
    // v0.10.0 phase 22: the left door (MVP in rows 5..8) OPENS from frame 50 (hinged at its front end): with the real MVP it is a
    // cabin mover with its own motion (phase 19 gave it the cabin R: "no MVP")
    const int oDoorL = shifted ? big[2] : -1;
    for (int k = 0; k < 3; ++k) {
        static const double ovr[3][3] = { { -0.10, -0.15, 0.45 }, { 0.05, -0.18, 0.50 }, { 0.12, -0.12, 0.42 } };
        ovl.push_back(add("CAMERA-RELATIVE overlay (cab-fixed, projection * head rotation)", kOverlay, MS17Ovl0 + k, false,
                          ovr[k][0], ovr[k][1], ovr[k][2]));
    }
    const int nObj = (int)objs.size();
    (void)oGround;
    auto isCab = [&](int o) { const CK k = objs[o].kind; return k == kCabFixed || k == kCabShift || k == kCabSwing || k == kOverlay; };
    std::vector<M4> mvpPrev(nObj, I4()), mvpCur(nObj, I4());
    std::mt19937 rng(1717);
    int checks = 0, fails = 0, camRBad = 0, cabRBad = 0, stBad = 0, swingMover = 0, swingFrames = 0;
    long long cabPx = 0, cabBadPx = 0, swPx = 0, swBadPx = 0, hoodPx = 0, hoodBadPx = 0, worldPx = 0, worldBadPx = 0;
    long long carPx = 0, carBadPx = 0, bigPx = 0, ovlPx = 0;
    int cabBadFrames = 0;
    double worstCab = 0.0, worstCamPx = 0.0, worstCabR = 0.0, worstHood = 0.0;
    DrawIdMv::Stats st0{};
    uint64_t rb0 = 0;
    M4 VprevX = I4(), CprevX = I4();
    // the game's cabin projection is FINITE (in game column 3 z of the cabin draws changes with the origin's depth): reversed-Z,
    // near kNearCabin, far 15 m -> depth = A + B / z_view
    const double nC = kNearCabin, fC = 15.0;
    M4 Pc = Proj(kNearCabin);
    Pc.m[2][2] = -nC / (fC - nC); Pc.m[2][3] = nC * fC / (fC - nC);
    const M4 Pw = Proj(kNearWorld);
    const double kPi = 3.14159265358979;
    const double headRest[3] = { -0.35, 1.2, 0.0 };    // car-local head rest (the cabin parts' rel are relative to it)
    auto doorAng = [&](int f) { return (shifted && f >= 50) ? 0.3 * (1.0 - cos(2.0 * kPi * (double)(f - 50) / 40.0)) : 0.0; };
    int doorMover = 0, doorFrames = 0;
    long long doorPx = 0, doorBadPx = 0;
    double worstDoor = 0.0;
    const FLOAT zero[4] = { 0, 0, 0, 0 };
    const D3D11_RECT sc0 = { 0, 0, W, H };
    GpuPerf::Restart();
    for (int f = 0; f < frames; ++f) {
        PerfFrame();
        const M4 C = T(0.0, 0.0, 0.5 * f);                 // the car: straight ahead (cab-fixed = axis-aligned = camera-relative)
        const double yaw = 0.35 * sin(2.0 * kPi * f / 70.0), pitch = 0.12 * sin(2.0 * kPi * f / 45.0 + 1.0);
        const double bx = 0.02 * sin(2.0 * kPi * f / 40.0), bz = 0.015 * sin(2.0 * kPi * f / 55.0 + 0.5);
        const M4 Hl = Mul(T(headRest[0] + bx, headRest[1], headRest[2] + bz), Mul(RotY(yaw), RotX(pitch)));
        const M4 camPose = Mul(C, Hl);
        M4 V; Inv(camPose, V);
        const double headW[3] = { camPose.m[0][3], camPose.m[1][3], camPose.m[2][3] };
        M4 Vrot = V;
        Vrot.m[0][3] = Vrot.m[1][3] = Vrot.m[2][3] = 0.0;
        const M4 MVPcr = Mul(Pc, Vrot);                  // the overlays' cb0: projection * head rotation (no position)
        const double swingA = 0.25 * sin(2.0 * kPi * f / 30.0);
        std::vector<M4> world(nObj, I4());
        world[oGround] = T(0, 0, 100);
        for (int k = 0; k < 10; ++k) world[oBox[k]] = T((k & 1) ? 9.0 + k : -9.0 - k, 0.0, 35.0 + 14.0 * k);
        for (int k = 0; k < 60; ++k) world[oPost[k]] = T((k & 1) ? 4.5 : -4.5, 0.0, 6.0 + 8.0 * (k / 2));
        world[oCarC] = Mul(T(-3.0, 0.0, 160.0 - 1.0 * f), RotY(kPi));
        world[oHood] = Mul(C, T(headRest[0], 0.65, 2.0));
        double ovPos[3][3] = {};
        for (int o = 0; o < nObj; ++o) {
            const CO& ob = objs[o];
            const M4 L = T(headRest[0] + ob.rel[0], headRest[1] + ob.rel[1], headRest[2] + ob.rel[2]);
            if (o == oDoorL) world[o] = Mul(C, Mul(L, Mul(T(0.0, 0.0, 0.28), Mul(RotY(-doorAng(f)), T(0.0, 0.0, -0.28)))));
            else if (ob.kind == kCabFixed || ob.kind == kCabShift) world[o] = Mul(C, L);
            else if (ob.kind == kCabSwing) {
                const double side = (o == swing[0]) ? 0.0 : 0.035;     // tag part 2 hangs 3.5 cm to the side on the same frame
                world[o] = Mul(C, Mul(L, Mul(RotZ(swingA), T(side, -0.06, 0.0))));
            } else if (ob.kind == kOverlay) {
                const int k = (int)(std::find(ovl.begin(), ovl.end(), o) - ovl.begin());
                const double p4[4] = { 0, 0, 0, 1 };
                double pw[4];
                Xf(Mul(C, L), p4, pw);
                for (int a = 0; a < 3; ++a) ovPos[k][a] = pw[a];
                world[o] = T(pw[0], pw[1], pw[2]);       // (the truth; the draw itself is camera-relative)
            }
        }
        {
            float bv[12] = {};
            for (int k = 0; k < 3; ++k) for (int a = 0; a < 3; ++a) bv[k * 4 + a] = (float)(ovPos[k][a] - headW[a]);
            ctx->UpdateSubresource(bones.Get(), 0, nullptr, bv, 0, 0);
        }
        for (int o = 0; o < nObj; ++o) mvpCur[o] = Mul(isCab(o) ? Pc : Pw, Mul(V, world[o]));
        // ---- draw order (shuffled), cbuffer windows ----
        std::vector<int> order(nObj);
        for (int o = 0; o < nObj; ++o) order[o] = o;
        std::shuffle(order.begin(), order.end(), rng);
        const int nd = nObj;
        ID3D11Buffer* ring = E.ring[f & 1].Get();
        {
            D3D11_MAPPED_SUBRESOURCE mp{};
            if (FAILED(ctx->Map(ring, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { printf("ring map failed\n"); return 1; }
            float* p = (float*)mp.pData;
            memset(p, 0, 2u << 20);
            for (int d = 0; d < nd; ++d) {
                const int o = order[d];
                const CK k = objs[o].kind;
                const M4& m = k == kOverlay ? MVPcr : mvpCur[o];
                float* w = p + (UINT)d * 64u;
                if (k == kCabShift) {                    // row 4 = (0, 0, 0, 1), the MVP in rows 5..8, the truth in row 9
                    w[4 * 4 + 3] = 1.0f;
                    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) w[(5 + r) * 4 + c] = (float)m.m[r][c];
                    w[9 * 4 + 0] = (float)(o + 1); w[9 * 4 + 1] = (float)(d + 1);
                } else {
                    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) w[(4 + r) * 4 + c] = (float)m.m[r][c];
                    w[8 * 4 + 0] = (float)(o + 1); w[8 * 4 + 1] = (float)(d + 1); w[8 * 4 + 2] = 0.0f;
                }
            }
            ctx->Unmap(ring, 0);
        }
        // ---- G-buffer pass ----
        ctx->ClearDepthStencilView(E.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
        ctx->ClearRenderTargetView(E.truthRtv.Get(), zero);
        ctx->ClearRenderTargetView(E.colorRtv.Get(), zero);
        ID3D11RenderTargetView* rtvs[2] = { E.truthRtv.Get(), E.colorRtv.Get() };
        ctx->OMSetRenderTargets(2, rtvs, E.dsv.Get());
        ctx->OMSetDepthStencilState(E.dss.Get(), 1);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        auto halton = [](int i, int b) { double q = 0, fct = 1; while (i > 0) { fct /= b; q += fct * (i % b); i /= b; } return q; };
        const float jx = (float)(halton(f % 8 + 1, 2) - 0.5), jy = (float)(halton(f % 8 + 1, 3) - 0.5);
        const D3D11_VIEWPORT vpW = { jx, jy, (float)(W + g_vpOver), (float)(H + g_vpOver), 0.01f, 0.9f };
        const D3D11_VIEWPORT vpC = { jx, jy, (float)(W + g_vpOver), (float)(H + g_vpOver), 0.9f, 1.0f };
        rec.Reset();
        cand.Reset();
        cand.SetDropped(cam.MedoidDropBits());
        for (int d = 0; d < nd; ++d) {
            const int o = order[d];
            const CO& ob = objs[o];
            const Mesh& m = E.meshes[ob.mesh];
            const bool cab = isCab(o);
            ctx->RSSetViewports(1, cab ? &vpC : &vpW);
            ctx->RSSetScissorRects(1, &sc0);
            ctx->RSSetState(ob.cullNone ? E.rsNone.Get() : E.rsBack.Get());
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11Buffer* vbs[2] = { E.vb[0].Get(), E.instVb[0].Get() };
            const UINT strides[2] = { sizeof(Vtx), 12 }, offs[2] = { 0, 0 };
            ctx->IASetVertexBuffers(0, 2, vbs, strides, offs);
            ctx->IASetIndexBuffer(E.ib[0].Get(), DXGI_FORMAT_R32_UINT, 0);
            ID3D11ShaderResourceView* vsSrv[4] = { nullptr, nullptr, nullptr, ob.kind == kOverlay ? bonesSrv.Get() : nullptr };
            ctx->VSSetShaderResources(0, 4, vsSrv);
            ctx->IASetInputLayout(E.il.Get());
            ctx->VSSetShader(ob.kind == kOverlay ? E.vsSkin.Get() : (ob.kind == kCabShift ? E.vsShift.Get() : E.vsMain.Get()), nullptr, 0);
            ctx->PSSetShader(ob.kind == kCabShift ? E.ps9.Get() : E.ps.Get(), nullptr, 0);
            const UINT first = (UINT)d * 16u, num = 16u;
            ctx1->VSSetConstantBuffers1(0, 1, &ring, &first, &num);
            ctx1->PSSetConstantBuffers1(0, 1, &ring, &first, &num);
            {   // the injector's camera candidates (v0.10.0 phase 22: at the VS's MVP row, as inject.cpp -- rows 4..7 until known)
                CandidateRecord::DrawKey k{};
                k.ib = E.ib[0].Get(); k.vb = E.vb[0].Get(); k.indexCount = m.ic; k.startIndex = m.si; k.baseVertex = m.bv;
                const int layer = cab ? 1 : 0;
                const void* vsp = ob.kind == kOverlay ? (const void*)E.vsSkin.Get() : (ob.kind == kCabShift ? (const void*)E.vsShift.Get()
                                                                                                             : (const void*)E.vsMain.Get());
                if (cand.Count(layer) < CandidateRecord::kSlots && !cand.Dropped(layer))
                    cand.Record(ctx, layer, ring, first * 16u + 16u * DrawIdMv::MvpRow(vsp), k);
            }
            rec.Record(ctx1, m.ic, 1u, m.si, m.bv, 0u, false);
            ctx->DrawIndexed(m.ic, m.si, m.bv);
        }
        ctx->PSSetShader(E.ps.Get(), nullptr, 0);
        // ---- G-buffer leave: replay, depth snapshot, MV generation ----
        DrawIdMv::SetFrame((uint64_t)f + 1);
        if (!rec.Replay(ctx1, E.idRtv.Get(), E.roDsv.Get(), false, true).ok) { printf("   FAIL replay (frame %d)\n", f); ++fails; }
        ID3D11RenderTargetView* nullRtv = nullptr;
        ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
        ctx->CopyResource(E.twin.Get(), E.depth.Get());
        CameraMv::FrameStats fs;
        const bool ok = cam.Generate(ctx, W, H, 0, 0, W, H, cand, E.twinSrv.Get(), E.dUav.Get(), E.mvUav.Get(), &fs, false,
                                     nullptr, nullptr, nullptr, &rec, rec.ReplayFailed() ? nullptr : E.idSrv.Get());
        if (!ok) { printf("   FAIL Generate (frame %d)\n", f); ++fails; }
        // ---- readbacks ----
        ctx->CopyResource(E.stMv.Get(), E.mv.Get());
        ctx->CopyResource(E.stDepth.Get(), E.dOut.Get());
        ctx->CopyResource(E.stTruth.Get(), E.truth.Get());
        const int nTab = (int)cam.DidN();
        if (cam.DidRSrv() && nTab) {
            ComPtr<ID3D11Resource> rres;
            cam.DidRSrv()->GetResource(&rres);
            const D3D11_BOX box{ 0, 0, 0, (UINT)nTab * DrawIdMv::kRStride * 16u, 1, 1 };
            ctx->CopySubresourceRegion(E.stR.Get(), 0, 0, 0, 0, rres.Get(), 0, &box);
        }
        D3D11_MAPPED_SUBRESOURCE mMv{}, mD{}, mT{}, mR{};
        ctx->Map(E.stMv.Get(), 0, D3D11_MAP_READ, 0, &mMv);
        CmpFrame(mMv, W, H);
        ctx->Map(E.stDepth.Get(), 0, D3D11_MAP_READ, 0, &mD);
        ctx->Map(E.stTruth.Get(), 0, D3D11_MAP_READ, 0, &mT);
        ctx->Map(E.stR.Get(), 0, D3D11_MAP_READ, 0, &mR);
        const float* Rt = (const float*)mR.pData;
        std::vector<int> stCur(nObj, -1), tabIdx(nObj, -1);
        for (int d = 0; d < nd; ++d) tabIdx[order[d]] = d;
        for (int o = 0; o < nObj; ++o) {
            const int ti = tabIdx[o];
            if (ti >= 0 && ti < nTab) stCur[o] = (int)(Rt[(ti * 5 + 4) * 4] + 0.5f);
        }
        // ---- the camera R per layer: GPU (solve buffer) vs exact; the cabin R = the head motion inside the car ----
        M4 RcamG, RcabG, RcamX, RcabX;
        {
            if (f == 0) { VprevX = V; CprevX = C; }
            M4 Pinv, Vinv, Pcinv, Cinv; Inv(Pw, Pinv); Inv(V, Vinv); Inv(Pc, Pcinv); Inv(C, Cinv);
            RcamX = Mul(Pw, Mul(VprevX, Mul(Vinv, Pinv)));
            RcabX = Mul(Pc, Mul(VprevX, Mul(CprevX, Mul(Cinv, Mul(Vinv, Pcinv)))));
            VprevX = V; CprevX = C;
            float sv[CameraMv::kSolveFloats];
            if (!cam.ReadSolveBlocking(ctx, sv)) { printf("   FAIL solve readback\n"); ++fails; }
            for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { RcamG.m[r][c] = sv[r * 4 + c]; RcabG.m[r][c] = sv[16 + r * 4 + c]; }
            double worst = 0.0, worstC = 0.0;
            for (int gy = 0; gy < 7; ++gy) for (int gx = 0; gx < 7; ++gx) {
                for (double zq : { 0.0, kNearWorld / 40.0, kNearWorld / 5.0 }) {
                    const double p4[4] = { -0.9 + 0.3 * gx, -0.9 + 0.3 * gy, zq, 1.0 };
                    double a[4], b[4];
                    Xf(RcamX, p4, a); Xf(RcamG, p4, b);
                    if (!(a[3] > 1e-9) || !(b[3] > 1e-9)) { worst = 1e9; continue; }
                    worst = std::max(worst, std::hypot((a[0] / a[3] - b[0] / b[3]) * W * 0.5, (a[1] / a[3] - b[1] / b[3]) * H * 0.5));
                }
                for (double zv : { 0.35, 0.6, 1.0, 2.0 }) {     // cabin points 0.35..2 m away: ndc z = A + B / z
                    const double p4[4] = { -0.9 + 0.3 * gx, -0.9 + 0.3 * gy, Pc.m[2][2] + Pc.m[2][3] / zv, 1.0 };
                    double a[4], b[4];
                    Xf(RcabX, p4, a); Xf(RcabG, p4, b);
                    if (!(a[3] > 1e-9) || !(b[3] > 1e-9)) { worstC = 1e9; continue; }
                    worstC = std::max(worstC, std::hypot((a[0] / a[3] - b[0] / b[3]) * W * 0.5, (a[1] / a[3] - b[1] / b[3]) * H * 0.5));
                }
            }
            if (f >= 2) {
                checks += 2;
                if (!(worst <= 0.05)) { ++fails; ++camRBad; if (camRBad <= 3) printf("   FAIL frame %d: world camera R off by %.3f px\n", f, worst); }
                if (!(worstC <= 0.05)) { ++fails; ++cabRBad; if (cabRBad <= 3) printf("   FAIL frame %d: CABIN camera R off by %.3f px\n", f, worstC); }
                worstCamPx = std::max(worstCamPx, worst);
                worstCabR = std::max(worstCabR, worstC);
            }
        }
        // ---- per-draw states (frame 2 on) ----
        if (f >= 2) {
            auto want = [&](int o, int st, const char* what) {
                ++checks;
                if (stCur[o] != st) {
                    ++fails; ++stBad;
                    if (stBad <= 6) printf("   FAIL frame %d: %s state %d, want %d (%s)\n", f, objs[o].name, stCur[o], st, what);
                }
            };
            // (v0.10.0 phase 22: the interior's real MVP (rows 5..8) is read -- cab-fixed draws are static cabin draws, no longer
            // "no MVP"; the left door is a mover while it opens)
            for (int o : big) {
                if (o == oDoorL && f >= 50) continue;
                want(o, DrawIdMv::kStStatic, shifted ? "cab-fixed, its MVP read from rows 5..8" : "cab-fixed");
            }
            if (oDoorL >= 0 && f >= 52) { ++doorFrames; if (stCur[oDoorL] == DrawIdMv::kStMover) ++doorMover; }
            for (int o : smallReal) want(o, DrawIdMv::kStStatic, "cab-fixed");
            for (int o : ovl) want(o, DrawIdMv::kStStatic, "camera-relative: origin-free, the cabin R");
            want(oHood, DrawIdMv::kStMover, "moves with the car: its own R");
            for (int o : swing) { ++swingFrames; if (stCur[o] == DrawIdMv::kStMover) ++swingMover; }
        }
        if (f == 10) { st0 = cam.DrawIds().GetStats(); rb0 = st0.rbFrames; }
        // ---- expected motion per object: world statics the GPU camera R, cab-fixed / overlays the EXACT cabin R, the movers their
        // exact own R ----
        std::vector<M4> Rexp(nObj, RcamG);
        std::vector<int> cls(nObj, 0);                   // 0 world static, 1 cabin static (incl. overlays + big), 2 swing, 3 hood, 4 car
        if (f >= 1) {
            for (int o = 0; o < nObj; ++o) {
                const CK k = objs[o].kind;
                if (k == kWorld) continue;
                if (o != oDoorL && (k == kCabFixed || k == kCabShift || k == kOverlay)) { Rexp[o] = RcabX; cls[o] = 1; continue; }
                M4 ic;
                Inv(mvpCur[o], ic);
                Rexp[o] = Mul(mvpPrev[o], ic);                   // (phase 22: the door's own motion = the cabin's while closed)
                cls[o] = o == oDoorL ? 5 : (k == kCabSwing ? 2 : (k == kHood ? 3 : 4));
            }
        }
        // ---- per pixel ----
        bool cabBad = false;
        for (int y = 0; y < H; ++y) {
            const uint16_t* mvRow = (const uint16_t*)((const uint8_t*)mMv.pData + y * mMv.RowPitch);
            const float* dRow = (const float*)((const uint8_t*)mD.pData + y * mD.RowPitch);
            const uint32_t* tRow = (const uint32_t*)((const uint8_t*)mT.pData + y * mT.RowPitch);
            for (int x = 0; x < W; ++x) {
                const uint32_t tObj = tRow[x * 2];
                if (!tObj || f < 2) continue;
                const int o = (int)tObj - 1;
                if (o < 0 || o >= nObj) continue;
                const double dd = dRow[x];
                const bool cabD = dd >= 0.9;
                const double z = cabD ? (dd - 0.9) / 0.1 : (dd - 0.01) / 0.89;
                const double u = (x + 0.5) / W, vv = (y + 0.5) / H;
                const double p4[4] = { u * 2 - 1, 1 - vv * 2, std::min(1.0, std::max(0.0, z)), 1.0 };
                double c[4];
                Xf(Rexp[o], p4, c);
                double ex = 0, ey = 0;
                if (c[3] > 1e-6) { ex = ((c[0] / c[3]) * 0.5 + 0.5 - u) * W; ey = ((0.5 - (c[1] / c[3]) * 0.5) - vv) * H; }
                const double gx = H2F(mvRow[x * 2]), gy = H2F(mvRow[x * 2 + 1]);
                const double err = std::hypot(gx - ex, gy - ey);
                const double big2 = std::max(std::fabs(ex), std::fabs(ey)) / 1024.0;
                switch (cls[o]) {
                case 1: {
                    ++cabPx;
                    if (std::find(big.begin(), big.end(), o) != big.end()) ++bigPx;
                    if (std::find(ovl.begin(), ovl.end(), o) != ovl.end()) ++ovlPx;
                    worstCab = std::max(worstCab, err);
                    if (!cabD || err > 0.05 + big2) {
                        ++cabBadPx; cabBad = true;
                        if (cabBadPx <= 4) printf("   FAIL frame %d: interior pixel (%d,%d) of %s: MV (%.3f %.3f), the cabin motion (%.3f %.3f)%s\n",
                                                  f, x, y, objs[o].name, gx, gy, ex, ey, cabD ? "" : " -- NOT at cabin depth");
                    }
                    break;
                }
                case 2: ++swPx; if (err > 0.03 + snapPx + big2) ++swBadPx; break;
                case 5:                                          // v0.10.0 phase 22: the opening door (its own motion)
                    ++doorPx; ++bigPx; worstDoor = std::max(worstDoor, err);
                    if (!cabD || err > 0.03 + snapPx + big2) {
                        ++doorBadPx;
                        if (doorBadPx <= 4) printf("   FAIL frame %d: door pixel (%d,%d): MV (%.3f %.3f), its own motion (%.3f %.3f)%s\n", f, x,
                                                   y, gx, gy, ex, ey, cabD ? "" : " -- NOT at cabin depth");
                    }
                    break;
                case 3: ++hoodPx; worstHood = std::max(worstHood, err); if (err > 0.03 + snapPx + big2) ++hoodBadPx; break;
                case 4: ++carPx; if (err > 0.03 + snapPx + big2) ++carBadPx; break;
                default: ++worldPx; if (err > 0.03 + big2) ++worldBadPx; break;
                }
            }
        }
        if (cabBad) ++cabBadFrames;
        if (g_debugLayer) { const int dl = DrainDebugLayer("S17"); ++checks; if (dl) ++fails; }   // ("debug": every frame)
        ctx->Unmap(E.stMv.Get(), 0); ctx->Unmap(E.stDepth.Get(), 0); ctx->Unmap(E.stTruth.Get(), 0); ctx->Unmap(E.stR.Get(), 0);
        for (int o = 0; o < nObj; ++o) mvpPrev[o] = mvpCur[o];
    }
    rec.Shutdown();
    // ---- scene totals ----
    const DrawIdMv::Stats& ds = cam.DrawIds().GetStats();
    const uint64_t rbN = ds.rbFrames - rb0;
    const uint64_t used = ds.cabSmallUsed - st0.cabSmallUsed, kept = ds.cabSmallKept - st0.cabSmallKept;
    const uint64_t notMvp = ds.notMvp - st0.notMvp, notMvpC = ds.notMvpCabin - st0.notMvpCabin;
    const uint64_t cabPaired = ds.cabPaired - st0.cabPaired, cabVoters = ds.cabVoters - st0.cabVoters;
    const double rbd = rbN ? (double)rbN : 1.0;
    // (v0.10.0 phase 22: the layout detection finds the interior's MVP -- no draw falls back to "no MVP"; the mutation "mutlayout"
    // brings back phase 19 exactly: the 4 big draws caught by the row-3 rule)
    const uint64_t wantShape = (shifted && g_layMut && !(g_s17Mut & 32)) ? 4u : 0u;
    const uint64_t lay1 = ds.lay[1] - st0.lay[1], layNone = (ds.layNone - st0.layNone) + (ds.layHintFail - st0.layHintFail);
    const uint64_t wantLay1 = (shifted && !g_layMut) ? 4u : 0u;
    printf("   readbacks (frames 10+) %llu: small cabin layer: cluster R used %llu, medoid kept (agreed) %llu; last winner ic %u weight %u "
           "dev to medoid %.3f px; cabin paired %.2f / voters %.2f per readback; rows 4..7 not an MVP %.2f per readback (cabin %.2f; "
           "want %llu)\n", (unsigned long long)rbN, (unsigned long long)used, (unsigned long long)kept, ds.cabSmallIc, ds.cabSmallW,
           (double)ds.cabSmallDev, (double)cabPaired / rbd, (double)cabVoters / rbd, (double)notMvp / rbd, (double)notMvpC / rbd,
           (unsigned long long)wantShape);
    printf("   MVP layout (phase 22, frames 10+): rows 5..8 (+1) %.2f per readback (want %llu), none %.2f; VS table: the shifted "
           "interior VS at rows %u..%u, the main VS at %u\n", (double)lay1 / rbd, (unsigned long long)wantLay1, (double)layNone / rbd,
           DrawIdMv::MvpRow(E.vsShift.Get()), DrawIdMv::MvpRow(E.vsShift.Get()) + 3u, DrawIdMv::MvpRow(E.vsMain.Get()));
    checks += 5;
    if (!rbN) { ++fails; printf("   FAIL no diagnostics readback landed\n"); }
    if (notMvpC != wantShape * rbN || notMvp != notMvpC) { ++fails; printf("   FAIL MVP-shape count (CSPair)\n"); }
    if (lay1 != wantLay1 * rbN || (!g_layMut && layNone != 0)) { ++fails; printf("   FAIL MVP layout count (CSGather)\n"); }
    if (shifted && !g_layMut && DrawIdMv::MvpRow(E.vsShift.Get()) != 5u) { ++fails; printf("   FAIL the per-VS table: the interior VS not at row 5\n"); }
    // (v0.10.0 phase 22: in S17 pass A now reads the interior's real MVP too (inject.cpp's candidates at DrawIdMv::MvpRow), so its
    // cabin medoid is a cab-fixed draw that AGREES with the heaviest cluster -- kept, not replaced; the cabin R itself is checked
    // against the exact one every frame. S17b / the mutation: the medoid is still an overlay's rotation-only R and must be replaced)
    const bool needUsed = !shifted || g_layMut != 0;
    if (DrawIdMv::CabinSmall() && (needUsed ? used == 0 : used + kept != rbN)) {
        ++fails;
        printf(needUsed ? "   FAIL the small-cabin rule never replaced the medoid (an overlay's rotation-only R)\n"
                        : "   FAIL the small-cabin rule did not run in every readback (cluster R used + medoid kept != readbacks)\n");
    }
    checks += 5;
    if (cabBadPx) ++fails;
    if (swPx == 0 || bigPx == 0 || ovlPx == 0 || hoodPx == 0) {
        ++fails; printf("   FAIL a part of the car never showed (swing %lld, big %lld, overlays %lld, hood %lld px)\n", swPx, bigPx, ovlPx, hoodPx);
    }
    if (swBadPx * 100 > swPx || swingMover * 10 < swingFrames * 7) {
        ++fails; printf("   FAIL swinging part: px off %lld of %lld, a mover in %d of %d draw-frames (want >= 70 %%)\n", swBadPx, swPx, swingMover, swingFrames);
    }
    if (hoodBadPx * 100 > hoodPx) { ++fails; printf("   FAIL hood pixels off its motion: %lld of %lld\n", hoodBadPx, hoodPx); }
    if (shifted) {                                       // v0.10.0 phase 22: the opening door
        checks += 1;
        if (doorPx == 0 || doorBadPx * 100 > doorPx || doorMover * 10 < doorFrames * 7) {
            ++fails;
            printf("   FAIL opening door: px off its own motion %lld of %lld, a mover in %d of %d frames (want >= 70 %%)\n", doorBadPx,
                   doorPx, doorMover, doorFrames);
        }
        printf("   opening door (MVP in rows 5..8, frames 50+): a mover in %d of %d frames, px off its own motion %lld of %lld (worst "
               "%.3f px)\n", doorMover, doorFrames, doorBadPx, doorPx, worstDoor);
    }
    if (worldBadPx * 1000 > worldPx || carBadPx * 100 > carPx) {
        ++fails; printf("   FAIL world pixels off the camera motion %lld of %lld / oncoming car px off %lld of %lld\n", worldBadPx, worldPx, carBadPx, carPx);
    }
    printf("   interior (cabin-depth) pixels off the cabin motion: %lld of %lld in %d frames (worst %.3f px; must be 0; big draws %lld px, "
           "overlays %lld px); cabin R worst vs exact %.4f px (%d frames over 0.05); world camera R worst %.4f px\n", cabBadPx, cabPx,
           cabBadFrames, worstCab, bigPx, ovlPx, worstCabR, cabRBad, worstCamPx);
    printf("   swinging part: a mover in %d of %d draw-frames, px off %lld of %lld; hood px off %lld of %lld (worst %.3f px); world px off "
           "%lld of %lld; oncoming car px off %lld of %lld; draw states wrong %d\n", swingMover, swingFrames, swBadPx, swPx, hoodBadPx,
           hoodPx, worstHood, worldBadPx, worldPx, carBadPx, carPx, stBad);
    PrintPhase8(ctx, ds, "");
    DrawIdMv::SetParentMutation(0);
    DrawIdMv::SetAttach(attachSaved); DrawIdMv::SetTwin(twinSaved); DrawIdMv::SetParent(parentSaved); DrawIdMv::SetInstanced(instSaved);
    printf("   %s: %d / %d checks passed\n", fails ? "FAILED" : "passed", checks - fails, checks);
    tot.checks += checks; tot.fails += fails;
    return fails;
}

// ---- v0.10.0 phase 22: S18 SHOP WINDOWS (captures/shop_window_audit.md: the ATS shop windows' vertex shaders keep the MVP in cb0
// rows 6..9; phase 19's rows-4..7 read paired them with [MV row 2, (0, 0, 0, 1), MVP row 0, MVP row 1] = a mover with a wrong R as soon
// as the camera moves -- the wobbly pink windows in VR) ---------------------------------------------------------------------------
// A street of static buildings (12 facades, 40 posts, 10 boxes, the ground) with six shop windows on the facades, two families as in
// game (G-buffer glass with vertex shader A or B). Every window is drawn twice on the same mesh: the G-buffer glass and a FORWARD
// reflection draw (a third vertex shader, opaque, 1 cm in front of the glass on the odd windows -- the forward depth wins there --
// and 1 cm behind it on the even ones -- the scene depth wins; recorded in the forward record like a re-drawn forward draw, so both id
// paths carry the rows-6..9 MVP; in game the window forward draws are blend-off / additive and not re-drawn). All window
// draws carry the shop-window cb0 layout: r0 = object position + seed, r1 = parameters, r2..r5 = model-view (r5 = (0, 0, 0, 1)),
// r6..r9 = the MVP, r10 = the truth; the other draws: r0..r3 = model-view, r4..r7 = MVP, r8 = the truth. The camera drives 0.3 m /
// frame, yaws 0.05..0.5 deg / frame (a VR head never stops) with a small pitch wobble; a car overtakes (a real mover).
// Checks (frame 2 on): every window pixel (G-buffer truth; forward or scene depth won) = the EXACT camera motion; every window draw
// (G-buffer and forward) static (state 2), never a mover; the world camera R vs exact; world / car pixels; the read-back layout
// counts (the 6 G-buffer windows at +2 and the 6 forward windows shifted in every readback, no "none"); the per-VS table (the three
// window VSes at row 6, the main VS at row 4). "mutlayout" (the layout detection off = rows 4..7, phase 21) must FAIL.
int RunShopScene(Env& E, float snapPx, Totals& tot) {
    const int frames = 100;
    printf("\n===== S18 shop windows: 6 windows (2 families, MVP in cb0 rows 6..9) as G-buffer glass + forward reflection draws on static "
           "facades, the camera driving 0.3 m / frame and yawing 0.05..0.5 deg / frame, an overtaking car (%d frames, snap %.3f px%s) "
           "=====\n", frames, (double)snapPx, g_layMut ? "; MUTATION: the MVP layout detection off (rows 4..7)" : "");
    ID3D11Device* dev = E.dev.Get();
    ID3D11DeviceContext* ctx = E.ctx.Get();
    ID3D11DeviceContext1* ctx1 = E.ctx1.Get();
    DrawIdMv::SetParams(8.0f, snapPx);
    DrawIdMv::SetConsensus(true);
    const float attachSaved = DrawIdMv::AttachM();
    const bool twinSaved = DrawIdMv::Twin(), parentSaved = DrawIdMv::Parent(), instSaved = DrawIdMv::Instanced();
    DrawIdMv::SetAttach(0.5f);
    DrawIdMv::SetTwin(true);
    DrawIdMv::SetParent(true);
    DrawIdMv::SetInstanced(true);
    DrawIdMv::SetParentMutation(g_layMut);
    View v;
    S6Res r;
    if (!MakeView(E, v, W, H)) { printf("view textures failed\n"); return 1; }
    {
        bool ok = MakeTex(dev, W, H, DXGI_FORMAT_R16G16_UINT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &r.id2) &&
                  SUCCEEDED(dev->CreateRenderTargetView(r.id2.Get(), nullptr, &r.id2Rtv)) &&
                  SUCCEEDED(dev->CreateShaderResourceView(r.id2.Get(), nullptr, &r.id2Srv)) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R16G16_UINT, 0, &r.stId2, true) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, &r.fwd) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32_TYPELESS, 0, &r.stFwd, true) &&
                  MakeTex(dev, W, H, DXGI_FORMAT_R32G8X24_TYPELESS, 0, &r.stTwin, true);
        D3D11_DEPTH_STENCIL_VIEW_DESC dd{}; dd.Format = DXGI_FORMAT_D32_FLOAT; dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_R32_FLOAT; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        ok = ok && SUCCEEDED(dev->CreateDepthStencilView(r.fwd.Get(), &dd, &r.fwdDsv)) &&
             SUCCEEDED(dev->CreateShaderResourceView(r.fwd.Get(), &sd, &r.fwdSrv));
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; ds.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
        ds.StencilEnable = FALSE; ds.StencilReadMask = 0xFF; ds.StencilWriteMask = 0xFF;
        ds.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS };
        ds.BackFace = ds.FrontFace;
        ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&ds, &r.dssFwdGame));
        ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;            // inject.cpp g_fwdDss: GREATER_EQUAL, write ALL
        ok = ok && SUCCEEDED(dev->CreateDepthStencilState(&ds, &r.dssFwdRe));
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;   // the additive reflection
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &r.bsOver));
        bd.AlphaToCoverageEnable = TRUE;                            // inject.cpp g_fwdBs: alpha-to-coverage, mask 0
        bd.RenderTarget[0].BlendEnable = FALSE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].RenderTargetWriteMask = 0;
        ok = ok && SUCCEEDED(dev->CreateBlendState(&bd, &r.bsA2C));
        if (!ok) { printf("S18 resources failed\n"); return 1; }
    }
    CameraMv cam;
    if (!cam.Init(dev)) { printf("CameraMv init failed\n"); return 1; }
    cam.SetEgoPixel(0.0f);
    cam.SetEgoOrigin(0.0f);
    CandidateRecord cand;
    cand.Init(dev);
    DrawIdRecord rec, fwdRec;
    fwdRec.SetForward(true);
    // ---- objects ----
    enum SK { kSWorld, kSCar, kSWinA, kSWinB, kSFwd };
    struct SO { const char* name; SK kind; int mesh; bool cullNone; int win; };
    std::vector<SO> objs;
    auto add = [&](const char* n, SK k, int mesh, bool cn, int win = -1) { objs.push_back({ n, k, mesh, cn, win }); return (int)objs.size() - 1; };
    add("ground", kSWorld, MGround, true);
    for (int k = 0; k < 10; ++k) add("static box", kSWorld, MBox0 + k, false);
    for (int k = 0; k < 40; ++k) add("roadside post (40 identical)", kSWorld, MPost, false);
    for (int k = 0; k < 12; ++k) add("facade (12 identical)", kSWorld, MS18Facade, false);
    const int oCar = add("overtaking CAR (a real mover)", kSCar, MCar0 + 3, false);
    const int kWin = 6;
    int oWinG[kWin], oWinF[kWin];
    for (int w = 0; w < kWin; ++w) {
        const bool famB = (w % 3) == 1;
        oWinG[w] = add(famB ? "shop window glass, family B (VS B, MVP in rows 6..9)" : "shop window glass, family A (VS A, MVP in rows 6..9)",
                       famB ? kSWinB : kSWinA, famB ? MS18WinB : MS18WinA, true, w);
    }
    for (int w = 0; w < kWin; ++w)
        oWinF[w] = add("shop window REFLECTION (forward, VS F, MVP in rows 6..9, 1 cm in front)", kSFwd,
                       objs[oWinG[w]].mesh, true, w);
    const int nObj = (int)objs.size();
    auto isWin = [&](int o) { const SK k = objs[o].kind; return k == kSWinA || k == kSWinB || k == kSFwd; };
    auto vsOf = [&](int o) -> ID3D11VertexShader* {
        const SK k = objs[o].kind;
        return k == kSWinA ? E.vsShift2.Get() : (k == kSWinB ? E.vsShift2B.Get() : (k == kSFwd ? E.vsShift2F.Get() : E.vsMain.Get()));
    };
    std::vector<M4> mvpPrev(nObj, I4()), mvpCur(nObj, I4()), mvCur(nObj, I4());
    std::mt19937 rng(1818);
    int checks = 0, fails = 0, camRBad = 0, stBad = 0, winMoverDraws = 0, winStatDraws = 0, winDraws = 0, carMover = 0, carFrames = 0;
    long long winPx = 0, winBadPx = 0, winFwdPx = 0, worldPx = 0, worldBadPx = 0, carPx = 0, carBadPx = 0;
    double worstWin = 0.0, worstCamPx = 0.0, maxYawDeg = 0.0, minYawDeg = 1e9;
    DrawIdMv::Stats st0{};
    uint64_t rb0 = 0;
    M4 VprevX = I4();
    const M4 Pw = Proj(kNearWorld);
    const double kPi = 3.14159265358979;
    const FLOAT zero[4] = { 0, 0, 0, 0 };
    const D3D11_RECT sc0 = { 0, 0, W, H };
    double yaw = -0.20;
    GpuPerf::Restart();
    for (int f = 0; f < frames; ++f) {
        PerfFrame();
        // ---- camera: 0.3 m / frame, yaw rate 0.05..0.5 deg / frame (a 50-frame cycle), a small pitch wobble ----
        const double rateDeg = 0.05 + 0.45 * (0.5 - 0.5 * cos(2.0 * kPi * f / 50.0));
        if (f > 0) { yaw += rateDeg * kPi / 180.0; maxYawDeg = std::max(maxYawDeg, rateDeg); minYawDeg = std::min(minYawDeg, rateDeg); }
        const M4 camPose = Mul(T(0.0, 1.6, 0.3 * f), Mul(RotY(yaw), RotX(0.01 * sin(2.0 * kPi * f / 37.0))));
        M4 V; Inv(camPose, V);
        std::vector<M4> world(nObj, I4());
        world[0] = T(0, 0, 100);
        for (int k = 0; k < 10; ++k) world[1 + k] = T((k & 1) ? 7.0 + 0.3 * k : -7.0 - 0.3 * k, 0.0, 30.0 + 9.0 * k);
        for (int k = 0; k < 40; ++k) world[11 + k] = T((k & 1) ? 4.5 : -4.5, 0.0, 6.0 + 8.0 * (k / 2));
        for (int k = 0; k < 12; ++k) world[51 + k] = T((k & 1) ? -10.0 : 10.0, 0.0, 25.0 + 15.0 * (k / 2));
        world[oCar] = T(-2.2, 0.0, 15.0 + 0.9 * f);
        for (int w = 0; w < kWin; ++w) {
            const double s = (w & 1) ? -1.0 : 1.0, zc = 25.0 + 15.0 * w;   // on facade w (alternating sides), its street face at |x| 9.5
            world[oWinG[w]] = Mul(T(s * (10.0 - 0.5 - 0.02), 0.0, zc), RotY(s * kPi * 0.5));
            // the reflection: odd windows (left side) 1 cm in FRONT of the glass (the forward depth wins: forward ids), even windows
            // (right side) 1 cm BEHIND it (the scene depth wins: G-buffer ids) -- both id paths carry the rows-6..9 MVP
            world[oWinF[w]] = Mul(T(s * (10.0 - 0.5 - ((w & 1) ? 0.03 : 0.01)), 0.0, zc), RotY(s * kPi * 0.5));
        }
        for (int o = 0; o < nObj; ++o) { mvCur[o] = Mul(V, world[o]); mvpCur[o] = Mul(Pw, mvCur[o]); }
        // ---- draw order: G-buffer shuffled, then the forward reflections; cbuffer windows ----
        std::vector<int> order, forder;
        for (int o = 0; o < nObj; ++o) { if (objs[o].kind == kSFwd) forder.push_back(o); else order.push_back(o); }
        std::shuffle(order.begin(), order.end(), rng);
        const int nd = (int)order.size(), nf = (int)forder.size();
        ID3D11Buffer* ring = E.ring[f & 1].Get();
        {
            D3D11_MAPPED_SUBRESOURCE mp{};
            if (FAILED(ctx->Map(ring, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { printf("ring map failed\n"); return 1; }
            float* p = (float*)mp.pData;
            memset(p, 0, 2u << 20);
            for (int d = 0; d < nd + nf; ++d) {
                const int o = d < nd ? order[d] : forder[d - nd];
                float* w = p + (UINT)d * 64u;
                const float idx1 = (float)((d < nd ? d : d - nd) + 1);
                if (isWin(o)) {                                  // the shop-window layout (audit EID 26287 / 26610)
                    const bool fw = objs[o].kind == kSFwd;
                    w[0] = (float)world[o].m[0][3]; w[1] = (float)world[o].m[1][3]; w[2] = (float)world[o].m[2][3];
                    w[3] = (float)(8509453 + o);
                    w[4] = 94.0f; w[5] = 125.3f; w[6] = fw ? 8.0f : 0.0326f; w[7] = fw ? 4.0f : -7.21f;
                    for (int rr = 0; rr < 4; ++rr) for (int c = 0; c < 4; ++c) {
                        w[(2 + rr) * 4 + c] = (float)mvCur[o].m[rr][c];
                        w[(6 + rr) * 4 + c] = (float)mvpCur[o].m[rr][c];
                    }
                    w[10 * 4 + 0] = (float)(o + 1); w[10 * 4 + 1] = idx1;
                } else {
                    for (int rr = 0; rr < 4; ++rr) for (int c = 0; c < 4; ++c) {
                        w[rr * 4 + c] = (float)mvCur[o].m[rr][c];
                        w[(4 + rr) * 4 + c] = (float)mvpCur[o].m[rr][c];
                    }
                    w[8 * 4 + 0] = (float)(o + 1); w[8 * 4 + 1] = idx1;
                }
            }
            ctx->Unmap(ring, 0);
        }
        auto halton = [](int i, int b) { double q = 0, fct = 1; while (i > 0) { fct /= b; q += fct * (i % b); i /= b; } return q; };
        const float jx = (float)(halton(f % 8 + 1, 2) - 0.5), jy = (float)(halton(f % 8 + 1, 3) - 0.5);
        const D3D11_VIEWPORT vpW = { jx, jy, (float)(W + g_vpOver), (float)(H + g_vpOver), 0.01f, 0.9f };
        auto bindDraw = [&](int o, int d) {
            const SO& ob = objs[o];
            ctx->RSSetViewports(1, &vpW);
            ctx->RSSetScissorRects(1, &sc0);
            ctx->RSSetState(ob.cullNone ? E.rsNone.Get() : E.rsBack.Get());
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11Buffer* vbs[2] = { E.vb[0].Get(), E.instVb[0].Get() };
            const UINT strides[2] = { sizeof(Vtx), 12 }, offs[2] = { 0, 0 };
            ctx->IASetVertexBuffers(0, 2, vbs, strides, offs);
            ctx->IASetIndexBuffer(E.ib[0].Get(), DXGI_FORMAT_R32_UINT, 0);
            ID3D11ShaderResourceView* vsSrv[4] = {};
            ctx->VSSetShaderResources(0, 4, vsSrv);
            ctx->IASetInputLayout(E.il.Get());
            ctx->VSSetShader(vsOf(o), nullptr, 0);
            const UINT first = (UINT)d * 16u, num = 16u;
            ctx1->VSSetConstantBuffers1(0, 1, &ring, &first, &num);
            ctx1->PSSetConstantBuffers1(0, 1, &ring, &first, &num);
        };
        // ---- G-buffer ----
        ctx->ClearDepthStencilView(v.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
        ctx->ClearRenderTargetView(v.truthRtv.Get(), zero);
        ctx->ClearRenderTargetView(v.colorRtv.Get(), zero);
        ID3D11RenderTargetView* rtvs[2] = { v.truthRtv.Get(), v.colorRtv.Get() };
        ctx->OMSetRenderTargets(2, rtvs, v.dsv.Get());
        ctx->OMSetDepthStencilState(E.dss.Get(), 1);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        rec.Reset(); fwdRec.Reset(); cand.Reset();
        cand.SetDropped(cam.MedoidDropBits());
        for (int d = 0; d < nd; ++d) {
            const int o = order[d];
            const Mesh& m = E.meshes[objs[o].mesh];
            bindDraw(o, d);
            ctx->PSSetShader(isWin(o) ? E.ps10.Get() : E.ps.Get(), nullptr, 0);
            CandidateRecord::DrawKey k{};                 // the camera candidates at the VS's MVP row (inject.cpp, phase 22)
            k.ib = E.ib[0].Get(); k.vb = E.vb[0].Get(); k.indexCount = m.ic; k.startIndex = m.si; k.baseVertex = m.bv;
            if (cand.Count(0) < CandidateRecord::kSlots && !cand.Dropped(0))
                cand.Record(ctx, 0, ring, (UINT)d * 256u + 16u * DrawIdMv::MvpRow(vsOf(o)), k);
            rec.Record(ctx1, m.ic, 1u, m.si, m.bv, 0u, false);
            ctx->DrawIndexed(m.ic, m.si, m.bv);
        }
        ctx->PSSetShader(E.ps.Get(), nullptr, 0);
        DrawIdMv::SetFrame((uint64_t)f + 1);
        if (!rec.Replay(ctx1, r.id2Rtv.Get(), v.roDsv.Get(), false, false).ok) { printf("   FAIL replay (frame %d)\n", f); ++fails; }
        // ---- forward pass: the game's reflection draw (scene depth tested, no write, additive) + the re-draw into the forward depth
        // (as inject.cpp FwdOnDraw) + the record ----
        ID3D11RenderTargetView* crt = v.colorRtv.Get();
        bool fwdCleared = false;
        for (int k = 0; k < nf; ++k) {
            const int o = forder[k];
            const Mesh& m = E.meshes[objs[o].mesh];
            bindDraw(o, nd + k);
            ctx->OMSetRenderTargets(1, &crt, v.dsv.Get());
            ctx->OMSetDepthStencilState(r.dssFwdGame.Get(), 0);
            ctx->OMSetBlendState(r.bsOver.Get(), nullptr, 0xFFFFFFFFu);
            ctx->PSSetShader(E.psFwd10.Get(), nullptr, 0);
            ctx->DrawIndexed(m.ic, m.si, m.bv);
            if (!fwdCleared) { ctx->ClearDepthStencilView(r.fwdDsv.Get(), D3D11_CLEAR_DEPTH, 0.0f, 0); fwdCleared = true; }
            ctx->OMSetRenderTargets(1, &crt, r.fwdDsv.Get());
            ctx->OMSetDepthStencilState(r.dssFwdRe.Get(), 0);
            ctx->OMSetBlendState(r.bsA2C.Get(), nullptr, 0xFFFFFFFFu);
            ctx->DrawIndexed(m.ic, m.si, m.bv);
            ctx->OMSetRenderTargets(1, &crt, v.dsv.Get());
            fwdRec.Record(ctx1, m.ic, 1, m.si, m.bv, 0, false);
        }
        {
            const auto fr = fwdRec.ReplayForward(ctx1, r.id2Rtv.Get(), r.fwdDsv.Get(), true, (UINT)rec.Count());
            if (!fr.ok || fr.replayed != nf) { printf("   FAIL forward replay (frame %d): ok %d replayed %d\n", f, (int)fr.ok, fr.replayed); ++fails; }
        }
        ctx->PSSetShader(E.ps.Get(), nullptr, 0);
        ID3D11RenderTargetView* nullRtv = nullptr;
        ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
        ctx->CopyResource(v.twin.Get(), v.depth.Get());
        CameraMv::FrameStats fs;
        {                                                // HDUMP=<frame> (+ HPROBE=1): the dump of this S18 frame (see RunScene)
            char ev[16] = {}, pv[16] = {};
            if (GetEnvironmentVariableA("HDUMP", ev, sizeof(ev)) && atoi(ev) == f) {
                const bool probe = GetEnvironmentVariableA("HPROBE", pv, sizeof(pv)) && atoi(pv) != 0;
                cam.DrawIds().RequestDump("harness HDUMP S18", probe ? 0.5f : -1.0f, probe ? 0.5f : -1.0f);
            }
        }
        const bool ok = cam.Generate(ctx, W, H, 0, 0, W, H, cand, v.twinSrv.Get(), v.dUav.Get(), v.mvUav.Get(), &fs, false,
                                     nullptr, nullptr, r.fwdSrv.Get(), &rec, r.id2Srv.Get(), &fwdRec);
        if (!ok) { printf("   FAIL Generate (frame %d)\n", f); ++fails; }
        if (f >= 1 && cam.DidN() != (uint32_t)(nd + nf)) {
            printf("   FAIL frame %d: %u draws in the per-draw table, want %d\n", f, cam.DidN(), nd + nf); ++fails;
        }
        // ---- readbacks ----
        ctx->CopyResource(v.stMv.Get(), v.mv.Get());
        ctx->CopyResource(v.stDepth.Get(), v.dOut.Get());
        ctx->CopyResource(v.stTruth.Get(), v.truth.Get());
        ctx->CopyResource(r.stId2.Get(), r.id2.Get());
        ctx->CopyResource(r.stFwd.Get(), r.fwd.Get());
        ctx->CopyResource(r.stTwin.Get(), v.twin.Get());
        const int nTab = (int)cam.DidN();
        if (cam.DidRSrv() && nTab) {
            ComPtr<ID3D11Resource> rres;
            cam.DidRSrv()->GetResource(&rres);
            const D3D11_BOX box{ 0, 0, 0, (UINT)nTab * DrawIdMv::kRStride * 16u, 1, 1 };
            ctx->CopySubresourceRegion(E.stR.Get(), 0, 0, 0, 0, rres.Get(), 0, &box);
        }
        D3D11_MAPPED_SUBRESOURCE mMv{}, mD{}, mT{}, mI{}, mR{}, mF{}, mTw{};
        ctx->Map(v.stMv.Get(), 0, D3D11_MAP_READ, 0, &mMv);
        CmpFrame(mMv, W, H);
        ctx->Map(v.stDepth.Get(), 0, D3D11_MAP_READ, 0, &mD);
        ctx->Map(v.stTruth.Get(), 0, D3D11_MAP_READ, 0, &mT);
        ctx->Map(r.stId2.Get(), 0, D3D11_MAP_READ, 0, &mI);
        ctx->Map(E.stR.Get(), 0, D3D11_MAP_READ, 0, &mR);
        ctx->Map(r.stFwd.Get(), 0, D3D11_MAP_READ, 0, &mF);
        ctx->Map(r.stTwin.Get(), 0, D3D11_MAP_READ, 0, &mTw);
        const float* Rt = (const float*)mR.pData;
        std::vector<int> stCur(nObj, -1), tabIdx(nObj, -1);
        for (int d = 0; d < nd; ++d) tabIdx[order[d]] = d;
        for (int k = 0; k < nf; ++k) tabIdx[forder[k]] = nd + k;
        for (int o = 0; o < nObj; ++o) {
            const int ti = tabIdx[o];
            if (ti >= 0 && ti < nTab) stCur[o] = (int)(Rt[(ti * 5 + 4) * 4] + 0.5f);
        }
        // ---- the camera R: GPU vs exact ----
        M4 RcamG, RcamX;
        {
            if (f == 0) VprevX = V;
            M4 Pinv, Vinv; Inv(Pw, Pinv); Inv(V, Vinv);
            RcamX = Mul(Pw, Mul(VprevX, Mul(Vinv, Pinv)));
            VprevX = V;
            float sv[CameraMv::kSolveFloats];
            if (!cam.ReadSolveBlocking(ctx, sv)) { printf("   FAIL solve readback\n"); ++fails; }
            for (int rr = 0; rr < 4; ++rr) for (int c = 0; c < 4; ++c) RcamG.m[rr][c] = sv[rr * 4 + c];
            double worst = 0.0;
            for (int gy = 0; gy < 7; ++gy) for (int gx = 0; gx < 7; ++gx)
                for (double zq : { 0.0, kNearWorld / 100.0, kNearWorld / 30.0, kNearWorld / 8.0 }) {
                    const double p4[4] = { -0.9 + 0.3 * gx, -0.9 + 0.3 * gy, zq, 1.0 };
                    double a[4], b[4];
                    Xf(RcamX, p4, a); Xf(RcamG, p4, b);
                    if (!(a[3] > 1e-9) || !(b[3] > 1e-9)) { worst = 1e9; continue; }
                    worst = std::max(worst, std::hypot((a[0] / a[3] - b[0] / b[3]) * W * 0.5, (a[1] / a[3] - b[1] / b[3]) * H * 0.5));
                }
            if (f >= 2) {
                ++checks;
                if (!(worst <= 0.05)) { ++fails; ++camRBad; if (camRBad <= 3) printf("   FAIL frame %d: world camera R off by %.3f px\n", f, worst); }
                worstCamPx = std::max(worstCamPx, worst);
            }
        }
        // ---- per-draw states (frame 2 on): every window draw static (its own pair = the camera motion), never a mover ----
        if (f >= 2) {
            for (int o = 0; o < nObj; ++o) {
                if (!isWin(o)) continue;
                ++checks; ++winDraws;
                if (stCur[o] == DrawIdMv::kStMover) ++winMoverDraws;
                if (stCur[o] == DrawIdMv::kStStatic) ++winStatDraws;
                if (stCur[o] != DrawIdMv::kStStatic) {
                    ++fails; ++stBad;
                    if (stBad <= 6) printf("   FAIL frame %d: %s (window %d) state %d, want 2 (static)\n", f, objs[o].name, objs[o].win, stCur[o]);
                }
            }
            ++carFrames;
            if (stCur[oCar] == DrawIdMv::kStMover) ++carMover;
        }
        if (f == 10) { st0 = cam.DrawIds().GetStats(); rb0 = st0.rbFrames; }
        // ---- per pixel: window pixels the EXACT camera motion, world pixels the GPU camera R, the car its own ----
        M4 RcarX = RcamG;
        if (f >= 1) { M4 ic; Inv(mvpCur[oCar], ic); RcarX = Mul(mvpPrev[oCar], ic); }
        for (int y = 0; y < H && f >= 2; ++y) {
            const uint16_t* mvRow = (const uint16_t*)((const uint8_t*)mMv.pData + y * mMv.RowPitch);
            const float* dRow = (const float*)((const uint8_t*)mD.pData + y * mD.RowPitch);
            const uint32_t* tRow = (const uint32_t*)((const uint8_t*)mT.pData + y * mT.RowPitch);
            const uint16_t* iRow = (const uint16_t*)((const uint8_t*)mI.pData + y * mI.RowPitch);
            const float* fRow = (const float*)((const uint8_t*)mF.pData + y * mF.RowPitch);
            const float* twRow = (const float*)((const uint8_t*)mTw.pData + y * mTw.RowPitch);
            for (int x = 0; x < W; ++x) {
                const uint32_t tObj = tRow[x * 2];
                if (!tObj) continue;
                const int o = (int)tObj - 1;
                if (o < 0 || o >= nObj) continue;
                float fw = fRow[x];                              // pass B's rule: the forward depth wins when in front (world range)
                if (!(fw >= 0.01f && fw < 0.9f)) fw = 0.0f;
                const bool fwWin = fw > twRow[x * 2];
                const double dd = dRow[x];
                const double z = (dd - 0.01) / 0.89;
                const double u = (x + 0.5) / W, vv = (y + 0.5) / H;
                const double p4[4] = { u * 2 - 1, 1 - vv * 2, std::min(1.0, std::max(0.0, z)), 1.0 };
                const M4& Re = isWin(o) ? RcamX : (o == oCar ? RcarX : RcamG);
                double c[4];
                Xf(Re, p4, c);
                double ex = 0, ey = 0;
                if (c[3] > 1e-6) { ex = ((c[0] / c[3]) * 0.5 + 0.5 - u) * W; ey = ((0.5 - (c[1] / c[3]) * 0.5) - vv) * H; }
                const double gx = H2F(mvRow[x * 2]), gy = H2F(mvRow[x * 2 + 1]);
                const double err = std::hypot(gx - ex, gy - ey);
                const double big2 = std::max(std::fabs(ex), std::fabs(ey)) / 1024.0;
                if (isWin(o)) {
                    ++winPx;
                    if (fwWin && iRow[x * 2 + 1] != 0) ++winFwdPx;   // the forward reflection won the pixel (its forward id is used)
                    worstWin = std::max(worstWin, err);
                    if (err > 0.05 + big2) {
                        ++winBadPx;
                        if (winBadPx <= 4) printf("   FAIL frame %d: window pixel (%d,%d) of %s: MV (%.3f %.3f), the camera motion (%.3f %.3f)\n",
                                                  f, x, y, objs[o].name, gx, gy, ex, ey);
                    }
                } else if (o == oCar) {
                    ++carPx; if (err > 0.03 + snapPx + big2) ++carBadPx;
                } else {
                    ++worldPx; if (err > 0.03 + big2) ++worldBadPx;
                }
            }
        }
        if (g_debugLayer) { const int dl = DrainDebugLayer("S18"); ++checks; if (dl) ++fails; }
        ctx->Unmap(v.stMv.Get(), 0); ctx->Unmap(v.stDepth.Get(), 0); ctx->Unmap(v.stTruth.Get(), 0); ctx->Unmap(r.stId2.Get(), 0);
        ctx->Unmap(E.stR.Get(), 0); ctx->Unmap(r.stFwd.Get(), 0); ctx->Unmap(r.stTwin.Get(), 0);
        for (int o = 0; o < nObj; ++o) mvpPrev[o] = mvpCur[o];
    }
    rec.Shutdown(); fwdRec.Shutdown();
    // ---- scene totals ----
    const DrawIdMv::Stats& ds = cam.DrawIds().GetStats();
    const uint64_t rbN = ds.rbFrames - rb0;
    const double rbd = rbN ? (double)rbN : 1.0;
    const uint64_t lay0 = ds.lay[0] - st0.lay[0], lay2 = ds.lay[2] - st0.lay[2], layFwd = ds.layFwdShift - st0.layFwdShift;
    const uint64_t layNone = (ds.layNone - st0.layNone) + (ds.layHintFail - st0.layHintFail), notMvp = ds.notMvp - st0.notMvp;
    const UINT rowA = DrawIdMv::MvpRow(E.vsShift2.Get()), rowB = DrawIdMv::MvpRow(E.vsShift2B.Get());
    const UINT rowF = DrawIdMv::MvpRow(E.vsShift2F.Get()), rowM = DrawIdMv::MvpRow(E.vsMain.Get());
    printf("   readbacks (frames 10+) %llu: MVP layout per readback: rows 4..7 %.2f, rows 6..9 (+2) %.2f (want %d), forward shifted %.2f "
           "(want %d), none %.2f (want 0); rows 4..7 not an MVP %.2f; VS table: window VS A rows %u..%u, B %u..%u, forward F %u..%u, "
           "main VS %u..%u\n", (unsigned long long)rbN, (double)lay0 / rbd, (double)lay2 / rbd, g_layMut ? 0 : kWin, (double)layFwd / rbd,
           g_layMut ? 0 : kWin, (double)layNone / rbd, (double)notMvp / rbd, rowA, rowA + 3u, rowB, rowB + 3u, rowF, rowF + 3u, rowM,
           rowM + 3u);
    checks += 8;
    if (!rbN) { ++fails; printf("   FAIL no diagnostics readback landed\n"); }
    if (!g_layMut && (lay2 != (uint64_t)kWin * rbN || layFwd != (uint64_t)kWin * rbN || layNone != 0 || notMvp != 0)) {
        ++fails; printf("   FAIL MVP layout counts (CSGather)\n");
    }
    if (!g_layMut && (rowA != 6u || rowB != 6u || rowF != 6u || rowM != 4u)) { ++fails; printf("   FAIL the per-VS layout table\n"); }
    if (winBadPx) { ++fails; }
    if (winPx == 0 || winFwdPx * 10 < winPx || (winPx - winFwdPx) * 10 < winPx) {   // each id path >= 10 % of the window pixels
        ++fails; printf("   FAIL the windows never showed / one id path (forward %lld of %lld px) barely used\n", winFwdPx, winPx);
    }
    if (winMoverDraws) { ++fails; printf("   FAIL window draws resolved MOVER %d times (of %d window draw-frames)\n", winMoverDraws, winDraws); }
    if (worldBadPx * 1000 > worldPx || carBadPx * 100 > carPx || carPx == 0) {
        ++fails; printf("   FAIL world pixels off the camera motion %lld of %lld / car px off %lld of %lld\n", worldBadPx, worldPx, carBadPx, carPx);
    }
    if (carMover * 10 < carFrames * 9) { ++fails; printf("   FAIL the overtaking car a mover in only %d of %d frames\n", carMover, carFrames); }
    printf("   window pixels off the exact camera motion: %lld of %lld (worst %.3f px; must be 0), of them the forward reflection won %lld (forward id), the glass %lld (G-buffer id); "
           "window draw-frames static %d / mover %d of %d; camera yaw rate %.3f..%.3f deg / frame, world camera R worst %.4f px (%d "
           "frames over 0.05); world px off %lld of %lld; car px off %lld of %lld, a mover in %d of %d frames; draw states wrong %d\n",
           winBadPx, winPx, worstWin, winFwdPx, winPx - winFwdPx, winStatDraws, winMoverDraws, winDraws, minYawDeg, maxYawDeg, worstCamPx, camRBad,
           worldBadPx, worldPx, carBadPx, carPx, carMover, carFrames, stBad);
    PrintPhase8(ctx, ds, "");
    DrawIdMv::SetParentMutation(0);
    DrawIdMv::SetAttach(attachSaved); DrawIdMv::SetTwin(twinSaved); DrawIdMv::SetParent(parentSaved); DrawIdMv::SetInstanced(instSaved);
    printf("   %s: %d / %d checks passed\n", fails ? "FAILED" : "passed", checks - fails, checks);
    tot.checks += checks; tot.fails += fails;
    return fails;
}

int main(int argc, char** argv) {
    int only = -1;
    float snapPx = 0.1f;
    if (argc > 1) only = atoi(argv[1]);
    if (argc > 2) snapPx = (float)atof(argv[2]);
    Env E;
    const D3D_FEATURE_LEVEL fls[2] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    // 3rd argument "debug": the D3D11 debug layer (needs the Graphics Tools optional feature); every error / warning it
    // reports is printed and counted as a failed check
    g_debugLayer = argc > 3 && !strcmp(argv[3], "debug");
    const bool mutNoCons = argc > 3 && !strcmp(argv[3], "nocons");   // v0.10.0 phase 3 mutations (S6 must fail)
    const bool mutNoFwd = argc > 3 && !strcmp(argv[3], "nofwd");
    const bool mutNoInherit = argc > 3 && !strcmp(argv[3], "noinherit");  // v0.10.0 phase 5: twins AND attach off (S7 must fail)
    const bool mutNoAttach = argc > 3 && (!strcmp(argv[3], "noattach") || mutNoInherit);   // v0.10.0 phase 4 mutation (S7 must fail)
    const bool mutNoTwin = argc > 3 && (!strcmp(argv[3], "notwin") || mutNoInherit);       // v0.10.0 phase 5 mutation (S7 must fail)
    // v0.10.0 phase 6 mutations (S8 must fail): 1 "noparent" (mv_drawid_parent 0), 2 "parentconj" (the conjugated row-vector
    // formula), 3 "nodepth" (no depth test in the vote). The phase-5 mutations above switch the rigid parents off too, so they
    // keep testing the twins / the origin attach on their own (with the parents on, the vote takes most of their cases).
    const int mutPar = argc <= 3 ? 0 : (!strcmp(argv[3], "noparent") ? 1 : (!strcmp(argv[3], "parentconj") ? 2 :
                                                                           (!strcmp(argv[3], "nodepth") ? 3 : 0)));
    DrawIdMv::SetParent(!(mutNoAttach || mutNoTwin));
    // v0.10.0 phase 8: performance knobs (key=value from the 3rd argument on; the defaults are the DLL's defaults):
    // vres=2|4 (mv_vote_res), cap=N (mv_vote_march_cap, 0 = off), cands=N (mv_cons_cands), legacy (= vres=2 cap=0 cands=128 and
    // every later phase-8 cut off: the phase-7 behaviour)
    for (int a = 3; a < argc; ++a) {
        const char* s = argv[a];
        if (!strcmp(s, "legacy")) {
            DrawIdMv::SetVoteRes(2); DrawIdMv::SetMarchCap(0); DrawIdMv::SetConsCands(128); DrawIdMv::SetMedoidDrop(0);
            DrawIdMv::SetInstReplay(0);
        }
        else if (!strncmp(s, "drop=", 5)) DrawIdMv::SetMedoidDrop((unsigned)atoi(s + 5));
        else if (!strncmp(s, "jerk=", 5)) g_yawJerk = atof(s + 5);   // phase 14 round 5
        else if (!strcmp(s, "mutcr")) g_s15Mut = 4;                  // phase 14 round 6: S15 with the origin-free rule off (must FAIL)
        else if (!strcmp(s, "mutinst")) g_s15Mut = 8;                //   S15 with multi-instance draws listed again (round-4 rule)
        else if (!strcmp(s, "mutr6")) g_s15Mut = 16;                 // round 7: S15 with the round-6 origin-free rule (VR eyes FAIL)
        else if (!strcmp(s, "mutshape")) g_s17Mut = 32;              // v0.10.0 phase 19: S17 with the MVP-shape rule off
        else if (!strcmp(s, "mutlayout")) g_layMut = 64;             // v0.10.0 phase 22: S17 / S18 with rows 4..7 (must FAIL both)
        else if (!strncmp(s, "cabsmall=", 9)) DrawIdMv::SetCabinSmall((unsigned)atoi(s + 9));   // phase 19: mv_cabin_small
        else if (!strncmp(s, "inst=", 5)) DrawIdMv::SetInstReplay(atoi(s + 5));
        else if (!strncmp(s, "cmpdump=", 8)) { if (fopen_s(&g_cmpOut, s + 8, "wb")) g_cmpOut = nullptr; }
        else if (!strncmp(s, "cmp=", 4)) { if (fopen_s(&g_cmpIn, s + 4, "rb")) g_cmpIn = nullptr; }
        else if (!strncmp(s, "vres=", 5)) DrawIdMv::SetVoteRes((unsigned)atoi(s + 5));
        else if (!strncmp(s, "cap=", 4)) DrawIdMv::SetMarchCap((unsigned)atoi(s + 4));
        else if (!strncmp(s, "cands=", 6)) DrawIdMv::SetConsCands((unsigned)atoi(s + 6));
        else if (!strcmp(s, "mutclip")) g_shadowMut = 1;     // v0.10.0 phase 9 (S11 / S12 must FAIL)
        else if (!strcmp(s, "mutview")) g_shadowMut = 2;
        else if (!strcmp(s, "nofold")) DrawIdMv::SetFold(0);       // v0.10.0 phase 9: the phase-8 dispatch chain
        else if (!strncmp(s, "fold=", 5)) DrawIdMv::SetFold(atoi(s + 5));   // v0.10.0 phase 18: 0 / 1 (phase 9) / 2 (default)
        else if (!strcmp(s, "nocompact")) DrawIdMv::SetVoteCompact(false);  // v0.10.0 phase 18: the full-grid vote (phase 8)
        else if (!strncmp(s, "cabmin=", 7)) DrawIdMv::SetConsMinCabin((unsigned)atoi(s + 7));   // phase 18: mv_cons_min_cabin
        else if (!strcmp(s, "hw")) g_hw = true;                    // v0.10.0 phase 18: hardware adapter (GPU timings)
        else if (!strcmp(s, "overscan")) g_vpOver = 1;             // v0.10.0 phase 23: jittered viewports 1 px larger right / bottom
        else if (!strncmp(s, "rstatic=", 8)) {                       // v0.10.0 phase 14: S1-S4 / S13 with the static replay skip
            const int e = atoi(s + 8);
            g_rstatic = e < 2 ? 2u : (e > 16 ? 16u : (unsigned)e);
            DrawIdMv::SetReplayStatic(6u, g_rstatic);
        }
        // v0.10.0 phase 10: mv_parent_max_listed, and the per-draw parts of the perf profiles (inject.cpp kProf)
        else if (!strncmp(s, "listed=", 7)) DrawIdMv::SetParentMaxListed((unsigned)atoi(s + 7));
        else if (!strcmp(s, "medium") || !strcmp(s, "low")) {
            DrawIdMv::SetVoteRes(4); DrawIdMv::SetMarchCap(24000); DrawIdMv::SetInstReplay(1);
            DrawIdMv::SetParentMaxListed(!strcmp(s, "low") ? 32u : 64u);
        }
    }
    printf("phase 8 knobs: mv_vote_res %u, mv_vote_march_cap %u, mv_cons_cands %u, mv_medoid_drop %u, mv_inst_replay %d, "
           "mv_parent_max_listed %u%s%s; phase 14: static replay skip %s (S1-S4, S6-S8, S13); phase 18: mv_fold_dispatch %d, "
           "mv_vote_compact %d%s%s\n",
           DrawIdMv::VoteRes(), DrawIdMv::MarchCap(), DrawIdMv::ConsCands(), DrawIdMv::MedoidDrop(), DrawIdMv::InstReplay(),
           DrawIdMv::ParentMaxListed(),
           g_cmpOut ? ", writing the comparison file" : "", g_cmpIn ? ", comparing with the reference file" : "",
           g_rstatic ? "ON (rstatic)" : "off", DrawIdMv::Fold(), (int)DrawIdMv::VoteCompact(),
           g_hw ? "; HARDWARE adapter (hw)" : "", g_vpOver ? "; phase 23: jittered viewports (W+1) x (H+1) (overscan)" : "");
    if (FAILED(D3D11CreateDevice(nullptr, g_hw ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, g_debugLayer ? D3D11_CREATE_DEVICE_DEBUG : 0, fls, 2,
                                 D3D11_SDK_VERSION, &E.dev, &got, &E.ctx))) {
        printf("no WARP device%s\n", g_debugLayer ? " with the debug layer (Graphics Tools installed?)" : ""); return 1;
    }
    if (g_debugLayer) {
        if (FAILED(E.dev.As(&g_iq))) { printf("no ID3D11InfoQueue\n"); return 1; }
        g_iq->ClearStoredMessages();
        printf("D3D11 debug layer ON\n");
    }
    if (FAILED(E.ctx.As(&E.ctx1))) { printf("no ID3D11DeviceContext1\n"); return 1; }
    GpuPerf::SetOn(true);                               // v0.10.0 phase 8: every section timed (WARP figures)
    CameraMv::RegisterShaders();
    SceneDlaa::RegisterShaders();                       // v0.10.0 phase 7: S9 runs the real SceneDlaa
    ShaderCache::Start();
    while (!ShaderCache::Done()) Sleep(10);
    for (int id : { ShaderCache::kDrawIdPs, ShaderCache::kDrawIdGather, ShaderCache::kDrawIdMatch, ShaderCache::kDrawIdResolve,
                    ShaderCache::kMvReprojDepth, ShaderCache::kDrawIdParentList, ShaderCache::kDrawIdParentVote,   // phase 6
                    ShaderCache::kDrawIdParentPick1, ShaderCache::kDrawIdParentPick2,
                    ShaderCache::kDrawIdParentCount }) {                                                           // phase 6b
        size_t sz = 0;
        if (!ShaderCache::Code((ShaderCache::Id)id, &sz)) { printf("shader %s: %s\n", ShaderCache::Name((ShaderCache::Id)id), ShaderCache::Error((ShaderCache::Id)id)); return 1; }
    }
    if (!DrawIdRecord::InitDevice(E.dev.Get())) { printf("DrawIdRecord::InitDevice failed\n"); return 1; }

    // ---- geometry: one big VB / IB (two pool copies) ----
    Geo g;
    E.meshes.resize(MCount);
    E.meshes[MGround] = g.Ground(300.0f);
    for (int k = 0; k < 10; ++k) E.meshes[MBox0 + k] = g.Box(1.0f + 0.1f * k, 1.0f + 0.15f * k, 1.0f, 0, 1.0f + 0.15f * k, 0);
    E.meshes[MClump] = g.Clump(0.5f, 0.35f);
    E.meshes[MBody] = g.Box(1.1f, 0.8f, 2.3f, 0, 1.3f, 0);
    E.meshes[MWheel] = g.Box(0.15f, 0.45f, 0.45f);
    E.meshes[MPlate] = g.Quad(0.26f, 0.06f);
    E.meshes[MTrailer] = g.Box(1.2f, 1.4f, 4.0f, 0, 1.6f, 0);
    E.meshes[MFoliage] = g.Quad(1.2f, 1.6f, 1.6f);
    E.meshes[MInst] = g.Quad(0.4f, 0.4f, 0.4f);
    E.meshes[MSkin] = g.Box(0.6f, 0.6f, 0.6f, 0, 0.6f, 0, 1.0f);
    E.meshes[MDash] = g.Box(0.8f, 0.08f, 0.15f);
    E.meshes[MWiper] = g.Box(0.35f, 0.015f, 0.01f, 0.35f, 0, 0);
    E.meshes[MCover] = E.meshes[MInst];
    E.meshes[MCab0] = g.Box(0.05f, 0.4f, 0.05f);
    E.meshes[MCab1] = g.Box(0.06f, 0.42f, 0.05f);
    E.meshes[MCab2] = g.Box(0.3f, 0.05f, 0.2f);
    // v0.10.0 phase 3 (S6)
    E.meshes[MPost] = g.Box(0.12f, 1.0f, 0.12f, 0, 1.0f, 0);
    E.meshes[MWire] = g.Quad(12.0f, 0.15f);
    E.meshes[MPlateF] = g.Quad(0.5f, 0.25f);
    E.meshes[MGlass] = g.Quad(0.8f, 0.3f);
    E.meshes[MTruck] = g.Box(1.25f, 1.5f, 5.0f, 0, 1.5f, 0);
    for (int k = 0; k < 12; ++k) E.meshes[MCar0 + k] = g.Box(0.85f + 0.02f * k, 0.7f + 0.03f * k, 2.0f + 0.05f * k, 0, 0.75f, 0);
    // v0.10.0 phase 4 (S7): unique bodies whose rear face is at z = -2.0 (the shared plate sits 2 cm behind it), the shared
    // plate (vertices in the body's frame) and the bracket plate 0.9 m behind the rear face
    for (int k = 0; k < 12; ++k) {
        const float hz = 2.0f + 0.05f * k;
        E.meshes[MPCar0 + k] = g.Box(0.85f + 0.02f * k, 0.7f + 0.03f * k, hz, 0, 0.75f, hz - 2.0f);
    }
    for (int k = 0; k < 4; ++k) {
        const float hz = 2.2f + 0.1f * k;
        E.meshes[MPCarB0 + k] = g.Box(0.9f + 0.03f * k, 0.8f + 0.02f * k, hz, 0, 0.85f, hz - 2.0f);
    }
    E.meshes[MPlateS] = g.QuadZ(0.5f, 0.25f, 0.9f, -2.02f);
    E.meshes[MPlateB] = g.QuadZ(0.5f, 0.25f, 0.5f, -2.9f);
    // v0.10.0 phase 5 (S7): the 12 identical G-buffer plates (below the forward shared plate on the rear face), a sign post, truck
    // OV (12 m long, pivot at its rear face, inner side face at x = -1.2)
    E.meshes[MPlateGS] = g.QuadZ(0.4f, 0.1f, 0.4f, -2.02f);
    E.meshes[MSignPost] = g.Box(0.1f, 1.5f, 0.1f, 0, 1.5f, 0);
    E.meshes[MOV] = g.Box(1.2f, 1.3f, 6.0f, 0, 1.3f, 6.0f);
    // v0.10.0 phase 6 (S8): a 13 m trailer, pivot at its centre, rear face at z = -6.5
    E.meshes[MTrl6] = g.Box(1.25f, 1.4f, 6.5f, 0, 1.6f, 0);
    E.meshes[MTrl6P] = g.Box(1.25f, 1.45f, 6.4f, 0, 1.65f, 0);    // the parked trailer (rear face at z = -6.4)
    E.meshes[MFarVan] = g.Box(1.15f, 1.0f, 2.5f, 0, 1.4f, 0);     // v0.10.0 phase 6c: the far van (rear face at z = -2.5)
    // v0.10.0 phase 14 round 6 (S15): 16 camera-relative grass clumps -- clump meshes whose vertices carry bone k (VSSkin adds
    // Bones[k] = the clump's position relative to THIS frame's camera, as the game's per-frame offset)
    for (int k = 0; k < 16; ++k) {
        E.meshes[MClumpCR0 + k] = g.Clump(0.45f + 0.01f * (float)k, 0.35f);
        for (int q = 0; q < 8; ++q) g.v[(size_t)E.meshes[MClumpCR0 + k].bv + (size_t)q].bone = (float)k;
    }
    // v0.10.0 phase 19 (S17 car): the own car's hood (world layer), the interior's big draws (36 indices per box), small real
    // parts, an air freshener (a quad), three camera-relative overlays (bone k = the overlay's offset from the head), a one-draw body
    E.meshes[MS17Hood] = g.Box(0.8f, 0.04f, 0.5f);
    E.meshes[MS17Dash] = g.Boxes(10, 0.04f, 0.06f, 0.1f, 0.09f, 0.0f, 0.0f, -0.405f, 0.0f, 0.0f);       // 360 indices
    E.meshes[MS17Console] = g.Boxes(6, 0.03f, 0.05f, 0.06f, 0.0f, -0.05f, 0.05f, 0.0f, -0.08f, -0.1f);  // 216, the dash's matrix
    E.meshes[MS17DoorL] = g.Boxes(8, 0.02f, 0.2f, 0.04f, 0.0f, 0.0f, 0.08f, 0.0f, 0.0f, -0.28f);       // 288
    E.meshes[MS17DoorR] = g.Boxes(7, 0.02f, 0.2f, 0.04f, 0.0f, 0.0f, 0.08f, 0.0f, 0.0f, -0.24f);       // 252
    E.meshes[MS17Real0] = g.Box(0.03f, 0.03f, 0.03f);
    E.meshes[MS17Real1] = g.Box(0.035f, 0.025f, 0.03f);
    E.meshes[MS17Fresh] = g.Quad(0.025f, 0.04f);
    E.meshes[MS17Fresh2] = g.Quad(0.02f, 0.03f);
    for (int k = 0; k < 3; ++k) E.meshes[MS17Ovl0 + k] = g.Box(0.02f + 0.004f * (float)k, 0.015f, 0.004f, 0, 0, 0, (float)k);
    E.meshes[MS17Body] = g.Boxes(40, 0.02f, 0.05f, 0.15f, 0.045f, 0.0f, 0.0f, -0.8775f, 0.0f, 0.0f);   // 1440 indices
    // v0.10.0 phase 22 (S18): a facade (1 m thick, 8 m tall, 12 m long along z), two shop-window quads (xy plane, double-sided)
    E.meshes[MS18Facade] = g.Box(0.5f, 4.0f, 6.0f, 0.0f, 4.0f, 0.0f);
    E.meshes[MS18WinA] = g.Quad(1.5f, 1.2f, 2.5f);
    E.meshes[MS18WinB] = g.Quad(2.0f, 1.0f, 2.2f);
    // (indices are relative to each mesh's base vertex; DrawIndexed adds it)
    for (int k = 0; k < 2; ++k) {
        D3D11_BUFFER_DESC bd{}; bd.Usage = D3D11_USAGE_DEFAULT;
        bd.ByteWidth = (UINT)(g.v.size() * sizeof(Vtx)); bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{ g.v.data(), 0, 0 };
        if (FAILED(E.dev->CreateBuffer(&bd, &sd, &E.vb[k]))) return 1;
        bd.ByteWidth = (UINT)(g.i.size() * 4); bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        sd.pSysMem = g.i.data();
        if (FAILED(E.dev->CreateBuffer(&bd, &sd, &E.ib[k]))) return 1;
        // per-instance offsets (float3): 64 instances along the road sides
        std::vector<float> inst;
        for (int q = 0; q < 64; ++q) { inst.push_back((q & 1) ? 5.5f + 0.3f * (q % 5) : -5.5f - 0.3f * (q % 7)); inst.push_back(0.0f); inst.push_back(18.0f + 2.5f * (q / 2)); }
        // instances 20..25: the S3 cover patch right in front of static box 0 (x = -7, z = 30), between it and the camera
        for (int q = 20; q < 26; ++q) { inst[q * 3] = -6.6f + 0.5f * (q - 20) * 0.2f; inst[q * 3 + 1] = 0.6f + 0.25f * (q - 20); inst[q * 3 + 2] = 28.5f; }
        bd.ByteWidth = (UINT)(inst.size() * 4); bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        sd.pSysMem = inst.data();
        if (FAILED(E.dev->CreateBuffer(&bd, &sd, &E.instVb[k]))) return 1;
    }
    for (int k = 0; k < 2; ++k) {
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = 2u << 20; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(E.dev->CreateBuffer(&bd, nullptr, &E.ring[k]))) { printf("ring create failed\n"); return 1; }
    }
    {   // bones: bone 1 lifts its vertices by 0.4 m (a replay that does not bind t3 draws them elsewhere -> EQUAL fails)
        const float bones[8] = { 0, 0, 0, 0, 0, 0.4f, 0, 0 };
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(bones); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 16;
        D3D11_SUBRESOURCE_DATA sd{ bones, 0, 0 };
        if (FAILED(E.dev->CreateBuffer(&bd, &sd, &E.bones)) || FAILED(E.dev->CreateShaderResourceView(E.bones.Get(), nullptr, &E.bonesSrv))) return 1;
    }
    // ---- shaders, layouts, states ----
    ComPtr<ID3DBlob> vsM = Compile("VSMain", "vs_5_0"), vsS = Compile("VSSkin", "vs_5_0"), vsI = Compile("VSInst", "vs_5_0"),
                     psB = Compile("PSMain", "ps_5_0"), vsSh = Compile("VSShift", "vs_5_0"), ps9 = Compile("PSMain9", "ps_5_0");
    if (!vsM || !vsS || !vsI || !psB || !vsSh || !ps9) return 1;
    E.dev->CreateVertexShader(vsSh->GetBufferPointer(), vsSh->GetBufferSize(), nullptr, &E.vsShift);   // v0.10.0 phase 19
    E.dev->CreatePixelShader(ps9->GetBufferPointer(), ps9->GetBufferSize(), nullptr, &E.ps9);
    {                                                    // v0.10.0 phase 22 (S18)
        ComPtr<ID3DBlob> a = Compile("VSShift2", "vs_5_0"), b = Compile("VSShift2B", "vs_5_0"), c = Compile("VSShift2F", "vs_5_0"),
                         p = Compile("PSMain10", "ps_5_0"), q = Compile("PSFwd10", "ps_5_0");
        if (!a || !b || !c || !p || !q) return 1;
        E.dev->CreateVertexShader(a->GetBufferPointer(), a->GetBufferSize(), nullptr, &E.vsShift2);
        E.dev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &E.vsShift2B);
        E.dev->CreateVertexShader(c->GetBufferPointer(), c->GetBufferSize(), nullptr, &E.vsShift2F);
        E.dev->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &E.ps10);
        E.dev->CreatePixelShader(q->GetBufferPointer(), q->GetBufferSize(), nullptr, &E.psFwd10);
    }
    E.dev->CreateVertexShader(vsM->GetBufferPointer(), vsM->GetBufferSize(), nullptr, &E.vsMain);
    E.dev->CreateVertexShader(vsS->GetBufferPointer(), vsS->GetBufferSize(), nullptr, &E.vsSkin);
    E.dev->CreateVertexShader(vsI->GetBufferPointer(), vsI->GetBufferSize(), nullptr, &E.vsInst);
    E.dev->CreatePixelShader(psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &E.ps);
    const D3D11_INPUT_ELEMENT_DESC el[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    const D3D11_INPUT_ELEMENT_DESC eli[4] = { el[0], el[1], el[2],
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 } };
    if (FAILED(E.dev->CreateInputLayout(el, 3, vsM->GetBufferPointer(), vsM->GetBufferSize(), &E.il)) ||
        FAILED(E.dev->CreateInputLayout(eli, 4, vsI->GetBufferPointer(), vsI->GetBufferSize(), &E.ilInst))) { printf("layout failed\n"); return 1; }
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_BACK; rd.DepthClipEnable = TRUE; rd.ScissorEnable = TRUE;
    E.dev->CreateRasterizerState(&rd, &E.rsBack);
    rd.CullMode = D3D11_CULL_NONE;
    E.dev->CreateRasterizerState(&rd, &E.rsNone);
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_GREATER;
    dd.StencilEnable = TRUE; dd.StencilReadMask = 0xFF; dd.StencilWriteMask = 0x0F;
    dd.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS };
    dd.BackFace = dd.FrontFace;
    E.dev->CreateDepthStencilState(&dd, &E.dss);
    // ---- targets: scene depth as the game makes it (typed D32S8, DSV binding only) + a read-only DSV of it ----
    if (!MakeTex(E.dev.Get(), W, H, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, D3D11_BIND_DEPTH_STENCIL, &E.depth)) { printf("depth failed\n"); return 1; }
    E.dev->CreateDepthStencilView(E.depth.Get(), nullptr, &E.dsv);
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        dv.Flags = D3D11_DSV_READ_ONLY_DEPTH | D3D11_DSV_READ_ONLY_STENCIL;
        if (FAILED(E.dev->CreateDepthStencilView(E.depth.Get(), &dv, &E.roDsv))) { printf("read-only DSV failed\n"); return 1; }
    }
    MakeTex(E.dev.Get(), W, H, DXGI_FORMAT_R32G32_UINT, D3D11_BIND_RENDER_TARGET, &E.truth);
    E.dev->CreateRenderTargetView(E.truth.Get(), nullptr, &E.truthRtv);
    MakeTex(E.dev.Get(), W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, &E.color);
    E.dev->CreateRenderTargetView(E.color.Get(), nullptr, &E.colorRtv);
    MakeTex(E.dev.Get(), 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET, &E.scratchRt);
    E.dev->CreateRenderTargetView(E.scratchRt.Get(), nullptr, &E.scratchRtv);
    MakeTex(E.dev.Get(), W, H, DXGI_FORMAT_R16_UINT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &E.idTex);
    E.dev->CreateRenderTargetView(E.idTex.Get(), nullptr, &E.idRtv);
    E.dev->CreateShaderResourceView(E.idTex.Get(), nullptr, &E.idSrv);
    MakeTex(E.dev.Get(), W, H, DXGI_FORMAT_R32G8X24_TYPELESS, D3D11_BIND_SHADER_RESOURCE, &E.twin);
    {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        E.dev->CreateShaderResourceView(E.twin.Get(), &sd, &E.twinSrv);
    }
    MakeTex(E.dev.Get(), W, H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &E.dOut);
    E.dev->CreateUnorderedAccessView(E.dOut.Get(), nullptr, &E.dUav);
    MakeTex(E.dev.Get(), W, H, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, &E.mv);
    E.dev->CreateUnorderedAccessView(E.mv.Get(), nullptr, &E.mvUav);
    MakeTex(E.dev.Get(), W, H, DXGI_FORMAT_R16G16_FLOAT, 0, &E.stMv, true);
    MakeTex(E.dev.Get(), W, H, DXGI_FORMAT_R32_FLOAT, 0, &E.stDepth, true);
    MakeTex(E.dev.Get(), W, H, DXGI_FORMAT_R32G32_UINT, 0, &E.stTruth, true);
    MakeTex(E.dev.Get(), W, H, DXGI_FORMAT_R16_UINT, 0, &E.stId, true);
    {
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = DrawIdMv::kMax * DrawIdMv::kRStride * 16; bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 16;
        if (FAILED(E.dev->CreateBuffer(&bd, nullptr, &E.stR))) { printf("R staging failed\n"); return 1; }
    }

    if (g_debugLayer) {                                 // self-check: a deliberate hazard must show up in the info queue
        ID3D11ShaderResourceView* sv = E.idSrv.Get();
        E.ctx->PSSetShaderResources(0, 1, &sv);
        ID3D11RenderTargetView* rv = E.idRtv.Get();
        E.ctx->OMSetRenderTargets(1, &rv, nullptr);           // SRV + RTV of one texture: the runtime warns and unbinds
        ID3D11RenderTargetView* nr = nullptr;
        E.ctx->OMSetRenderTargets(1, &nr, nullptr);
        const int n = DrainDebugLayer("self-check");
        printf("debug layer self-check: %d message(s) from a deliberate SRV/RTV hazard (must be > 0)\n", n);
        if (n == 0) { printf("FAILED: the debug layer reports nothing\n"); return 1; }
    }
    const SceneCfg scenes[4] = {
        { "S1 street: static copies, shuffled + culled identical clumps, vehicle (4 identical spinning wheels, plate, trailer), "
          "identical oncoming vehicle, alpha-tested foliage, instanced vegetation, skinned mover, cabin", 120, false, false, false, false },
        { "S2 = S1 + buffer-pool rotation (VB / IB pointers change every 7th frame, ring alternates)", 120, true, false, false, false },
        { "S3 = S1 + two G-buffer segments (instanced cover in segment 2) + mid-pass ring WRITE_DISCARD (forced replay)", 110, false, true, true, false },
        { "S4 slow vehicle: crawls 0.05 m / frame, stops at frame 60", 110, false, false, false, true },
    };
    Totals tot;
    std::vector<std::pair<const char*, int>> res;
    for (int s = 0; s < 4; ++s) {
        if (only >= 0 && s + 1 != only) continue;
        if (mutNoCons || mutNoFwd || mutNoAttach || mutNoTwin || mutPar) continue;   // the mutations only run S6 / S7 / S8
        const int fl = RunScene(E, scenes[s], snapPx, tot);
        res.push_back({ scenes[s].name, fl });
    }
    // v0.10.0 phase 14: S13 = S1 with every vehicle (and the skinned mover) parked until frame 40 -- with rstatic=E the static
    // replay skip must stop re-drawing them while parked and pick them up within kLagMax frames once they drive
    if ((only < 0 || only == 13) && !mutNoCons && !mutNoFwd && !mutNoAttach && !mutNoTwin && !mutPar) {
        SceneCfg s13 = { "S13 = S1 with every vehicle parked until frame 40, then driving (rstatic: the static replay skip picks "
                         "them up)", 100, false, false, false, false };
        s13.startVehicle = true;
        const int fl = RunScene(E, s13, snapPx, tot);
        res.push_back({ s13.name, fl });
    }
    // v0.10.0 phase 14 round 3: S14 = a vehicle + plate at the camera's speed (exactly static, then 0.04 px / frame off the camera:
    // "static" within the snap but not deep, then accelerating 0.05 px / frame per frame); with rstatic=E it may be skipped only
    // while exactly static (cmpdump / cmp with and without rstatic: its plate's motion vectors must match)
    if ((only < 0 || only == 14) && !mutNoCons && !mutNoFwd && !mutNoAttach && !mutNoTwin && !mutPar) {
        SceneCfg s14 = { "S14 = a vehicle + plate at the camera's speed: exactly static, then 0.04 px / frame of its own (static by the "
                         "snap, not deep), then accelerating", 120, false, false, false, false };
        s14.matchVehicle = true;
        const int fl = RunScene(E, s14, snapPx, tot);
        res.push_back({ s14.name, fl });
    }
    // v0.10.0 phase 14 round 6: S15 roadside -- camera-relative grass (non-instanced + instanced 2 / 3 / 8) beside an overtaking car,
    // an oncoming car and a parked van: every clump pixel keeps the exact camera motion ("mutcr" / "mutinst": the mutations)
    if ((only < 0 || only == 15) && !mutNoCons && !mutNoFwd && !mutNoAttach && !mutNoTwin && !mutPar) {
        const int fl = RunRoadsideScene(E, snapPx, tot, g_s15Mut, 0);
        res.push_back({ "S15 roadside: camera-relative grass + instanced 2 / 3 / 8 batches keep the camera motion beside an overtaking "
                        "car, an oncoming car and a parked van", fl });
    }
    // round 7: S15 as the two VR eyes (eye offset in the view, the clumps' column 3 = P * (eye offset, 1)); scene 16 = these two
    // alone (scene 15 stays the flat run: its cmpdump / cmp stream is unchanged)
    if ((only < 0 || only == 16) && !mutNoCons && !mutNoFwd && !mutNoAttach && !mutNoTwin && !mutPar) {
        for (int eye = 1; eye <= 2; ++eye) {
            const int fl = RunRoadsideScene(E, snapPx, tot, g_s15Mut, eye);
            res.push_back({ eye == 1 ? "S15 VR eye 0 (eye offset -0.032 0.002 -0.004 m): the camera-relative grass keeps the camera motion"
                                     : "S15 VR eye 1 (eye offset +0.032 0.002 -0.004 m): the camera-relative grass keeps the camera motion",
                            fl });
        }
    }
    if ((only < 0 || only == 5) && !mutNoCons && !mutNoFwd && !mutNoAttach && !mutNoTwin && !mutPar) {   // v0.10.0 phase 2: mirror + main view
        const int fl = RunMirrorScene(E, snapPx, tot);
        res.push_back({ "S5 mirror view (own depth / record / unit, trailer static to the mirror camera, throttled) + main view", fl });
    }
    if ((only < 0 || only == 6) && !mutNoAttach && !mutNoTwin && !mutPar) {   // v0.10.0 phase 3: consensus camera R + forward-pass draw ids
        const int fl = RunConvoyScene(E, snapPx, tot, mutNoCons, mutNoFwd);
        res.push_back({ "S6 consensus camera R (convoy + cars fool the medoid) + forward-pass ids (plate on a moving body, glass, wire)", fl });
    }
    if ((only < 0 || only == 7) && !mutNoCons && !mutNoFwd && !mutPar) {   // v0.10.0 phase 4: plates take their body's motion (attach)
        const int fl = RunPlateScene(E, snapPx, tot, mutNoAttach, mutNoTwin);
        res.push_back({ "S7 plates take their body's motion (12 shared plates H2, bracket plates, camera-VP plate H1, static decal; "
                        "phase 5: twins / keys new every frame)", fl });
    }
    if ((only < 0 || only == 8) && !mutNoCons && !mutNoFwd && !mutNoAttach && !mutNoTwin) {   // v0.10.0 phase 6: rigid parents
        const int fl = RunParentScene(E, snapPx, tot, mutPar);
        res.push_back({ "S8 rigid parents (view-space plate + text chain and a node-matrix forward plate on a turning trailer, parked "
                        "plate, static sign next to a passing truck)", fl });
    }
    // v0.10.0 phase 6c: S8 again with a 6 px march reach -- the plate text inside its background can only reach the trailer
    // through the 2nd pass (the background found it in the 1st); every S8 check must still hold
    if ((only < 0 || only == 8) && !mutNoCons && !mutNoFwd && !mutNoAttach && !mutNoTwin && !mutPar) {
        const int fl = RunParentScene(E, snapPx, tot, 0, 6u);
        res.push_back({ "S8 short reach (6 px march: plate text needs the 2nd pass through its background)", fl });
    }
    // v0.10.0 phase 7: the pre / post-tonemap stage (DlaaStage hysteresis, SceneDlaa's two colour sets, the per-pass flow)
    if ((only < 0 || only == 9) && !mutNoCons && !mutNoFwd && !mutNoAttach && !mutNoTwin && !mutPar) {
        const int fl = RunStageScene(E, tot);
        res.push_back({ "S9 pre/post-tonemap stage: one decision per pass + 60-pass hysteresis, both DLAA units kept alive (no rebuild, "
                        "no reset at a switch)", fl });
    }
    // v0.10.0 phase 8: the pass-slot texture pool
    if ((only < 0 || only == 10) && !mutNoCons && !mutNoFwd && !mutNoAttach && !mutNoTwin && !mutPar) {
        const int fl = RunPoolScene(E, tot);
        res.push_back({ "S10 pass-slot texture pool (mv_slot_pool): alive sets = passes in flight, mirror twins untouched, resize drops", fl });
    }
    // v0.10.0 phase 9: S11 = S6 and S12 = S8, each with two SHADOW pipelines next to the main one (the DLAA-area clip at dlaa_area
    // 50 + a 40 px margin; full and 1/2-resolution forward depth) -- the shadow ids / MVs inside the clip / crop are compared with
    // the main (phase-8) pipeline every frame. + a unit check of DrawIdRecord::ScissorRs / ScaledScissor / ScaleViewport.
    if ((only < 0 || only == 11 || only == 12) && !mutNoCons && !mutNoFwd && !mutNoAttach && !mutNoTwin && !mutPar) {
        const int fl = RunViewUnit(E, tot);
        res.push_back({ "S11/S12 unit: scissor-enabled RS copy (a no-scissor state gets ScissorEnable, everything else equal), "
                        "clip / 1/2-size scissor + viewport maths", fl });
    }
    if ((only < 0 || only == 11) && !mutNoCons && !mutNoFwd && !mutNoAttach && !mutNoTwin && !mutPar) {
        std::unique_ptr<Shadow> pa(new Shadow), pb(new Shadow);   // (heap: two CameraMv + four records would overflow the stack)
        Shadow& a = *pa; Shadow& b = *pb;
        std::unique_ptr<Shadow> pc(new Shadow), pd(new Shadow), pe(new Shadow);   // v0.10.0 phase 10: batch shadows
        Shadow& c = *pc; Shadow& d = *pd; Shadow& g = *pe;
        // S6's vehicles drive near the image centre: the crop is OFF-centre like a VR eye's (optical centre u 0.70), its left edge
        // runs through the plate carrier, the convoy and the cars
        if (!a.Init(E, W, H, 40, 40, 0, "S11 clip, full-res forward", 0.70f, 0.5f) ||
            !b.Init(E, W, H, 40, 40, 1, "S11 clip, 1/2-res forward", 0.70f, 0.5f) ||
            !c.Init(E, W, H, 40, 40, 0, "S11 clip, full-res forward BATCH", 0.70f, 0.5f, true, false) ||
            !d.Init(E, W, H, 40, 40, 0, "S11 clip, full-res forward BATCH + cull", 0.70f, 0.5f, true, true) ||
            !g.Init(E, W, H, 40, 40, 1, "S11 clip, 1/2-res forward BATCH + cull", 0.70f, 0.5f, true, true)) {
            ++tot.checks; ++tot.fails; res.push_back({ "S11 shadow init", 1 });
        } else {
            Shadow* const sh[5] = { &a, &b, &c, &d, &g };
            const int fl = RunConvoyScene(E, snapPx, tot, false, false, sh, 5);
            res.push_back({ "S11 = S6 + DLAA-area clip shadows (full / 1/2-resolution forward depth) vs the whole-image pipeline", fl });
        }
    }
    if ((only < 0 || only == 12) && !mutNoCons && !mutNoFwd && !mutNoAttach && !mutNoTwin && !mutPar) {
        std::unique_ptr<Shadow> pa(new Shadow), pb(new Shadow);   // (heap: two CameraMv + four records would overflow the stack)
        Shadow& a = *pa; Shadow& b = *pb;
        std::unique_ptr<Shadow> pc(new Shadow), pd(new Shadow), pe(new Shadow);   // v0.10.0 phase 10: batch shadows
        Shadow& c = *pc; Shadow& d = *pd; Shadow& g = *pe;
        // the crop's left edge (optical centre u 0.70) runs through the turning trailer and its plates (the vote near the clip)
        if (!a.Init(E, W, H, 40, 40, 0, "S12 clip, full-res forward", 0.70f, 0.5f) ||
            !b.Init(E, W, H, 40, 40, 1, "S12 clip, 1/2-res forward", 0.70f, 0.5f) ||
            !c.Init(E, W, H, 40, 40, 0, "S12 clip, full-res forward BATCH", 0.70f, 0.5f, true, false) ||
            !d.Init(E, W, H, 40, 40, 0, "S12 clip, full-res forward BATCH + cull", 0.70f, 0.5f, true, true) ||
            !g.Init(E, W, H, 40, 40, 1, "S12 clip, 1/2-res forward BATCH + cull", 0.70f, 0.5f, true, true)) {
            ++tot.checks; ++tot.fails; res.push_back({ "S12 shadow init", 1 });
        } else {
            Shadow* const sh[5] = { &a, &b, &c, &d, &g };
            const int fl = RunParentScene(E, snapPx, tot, 0, 0, sh, 5);
            res.push_back({ "S12 = S8 + DLAA-area clip shadows (full / 1/2-resolution forward depth) vs the whole-image pipeline", fl });
        }
    }
    // v0.10.0 phase 19: S17 car -- the interior's big draws with the MVP one cb0 row later (variant 0, the in-game signature) and a
    // valid interior whose one big body draw outweighs a 2-draw swinging tag (variant 1, "S17b"); scene 17 = both; last in the full
    // run, so a full-run cmpdump of an older binary still lines up frame by frame
    if ((only < 0 || only == 17) && !mutNoCons && !mutNoFwd && !mutNoAttach && !mutNoTwin && !mutPar) {
        for (int v = 0; v < 2; ++v) {
            const int fl = RunCarScene(E, snapPx, tot, v);
            res.push_back({ v == 0 ? "S17 car: interior draws whose cb0 rows 4..7 are not their MVP, camera-relative overlays, VR head "
                                     "motion -- every interior pixel the cabin motion"
                                   : "S17b car, valid interior: one big body draw outweighs a 2-draw swinging tag for the cabin R", fl });
        }
    }
    // v0.10.0 phase 22: S18 shop windows (the MVP in cb0 rows 6..9) -- after S17, so an older binary's full-run cmpdump still lines
    // up for S1-S17 (its file is then shorter: the comparison prints "REFERENCE FILE TOO SHORT" for S18 only)
    if ((only < 0 || only == 18) && !mutNoCons && !mutNoFwd && !mutNoAttach && !mutNoTwin && !mutPar) {
        const int fl = RunShopScene(E, snapPx, tot);
        res.push_back({ "S18 shop windows: glass + forward reflection draws with the MVP in cb0 rows 6..9 keep the camera motion while "
                        "the camera drives and yaws", fl });
    }
    printf("\n");
    for (auto& r : res) printf("%-120.120s %s\n", r.first, r.second ? "FAILED" : "passed");
    printf("%s: %d / %d checks passed\n", tot.fails ? "FAILED" : "PASSED", tot.checks - tot.fails, tot.checks);
    if (g_cmpOut) { fclose(g_cmpOut); printf("comparison file written: %llu frames\n", (unsigned long long)g_cmpFrames); }
    if (g_cmpIn) {
        fclose(g_cmpIn);
        printf("COMPARISON vs the reference file: %llu frames, %llu px compared; %llu px differ by > %.2f px in %llu frames (max %.4f px)%s\n",
               (unsigned long long)g_cmpFrames, (unsigned long long)g_cmpPx, (unsigned long long)g_cmpOver, kCmpTolPx,
               (unsigned long long)g_cmpOverFrames, g_cmpMax, g_cmpShort ? " -- REFERENCE FILE TOO SHORT (other scenes?)" : "");
    }
    DrawIdRecord::ShutdownDevice();
    GpuPerf::Shutdown();
    return tot.fails ? 1 : 0;
}
