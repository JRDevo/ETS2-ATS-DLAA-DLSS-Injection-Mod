// CameraMv implementation (v0.4.1). See motion_vectors.h and
// docs/DLAA_INTEGRATION.md ("Motion vectors"). HLSL is embedded and compiled at
// runtime with D3DCompile (d3dcompiler_47 is an OS DLL), same as the depth pass.
// v0.7.8: compiled once per process by the ShaderCache worker thread (shader_cache.h), not per instance.
#ifdef WITH_DLAA

#include "motion_vectors.h"
#include "gpu_perf.h"
#include "shader_cache.h"
#include "log.h"
#include <d3d11_1.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kSlotBytes   = CandidateRecord::kSlotBytes;   // one float4x4 (MVP rows 4..7 of the window)
constexpr UINT kCandBytes   = CandidateRecord::kBytes;       // 16384
// [0..3] R_world, [4..7] R_cabin, [8],[9] per-layer info, [10],[11] chosen-pair info,
// v0.6.0: [12..15] R_ego, [16] InvRow3, [17] ego info (layout: CameraMv::kSolveElems in motion_vectors.h)
constexpr UINT kSolveElems  = (UINT)CameraMv::kSolveElems;

// ---------------------------------------------------------------------------
// Pass A: one thread per layer. For each matched pair k:
//   R_k = MVP_prev_k * inverse(MVP_cur_k)   (current clip -> previous clip, static things)
// then the MEDOID (smallest summed Frobenius distance to the other R's) is the
// layer's R. Moving objects (cars, wheels, wipers) are outliers and lose.
// v0.4.1: world pairs whose object-origin view depth |w| = |MVP_cur[3][3]| is below
// Params.x metres (own truck / trailer move WITH the camera) are excluded from the
// medoid, unless that would leave none (then all valid pairs are used).
// MVP rows are stored as rows: clip.i = dot(row_i, float4(pos,1)).
// v0.6.0 ego reprojection (world thread only, after R_world): the player's own truck exterior (mirrors,
// hood, wheels) is drawn in the WORLD layer but moves WITH the camera, so R_world (static scenery) gives it a
// huge parallax MV. Every matched world pair (AllPairs, up to 128) whose object-origin view depth
// |MVP_cur[3][3]| is below Params.y metres is an ego candidate (first 16 with a valid R_k). Candidates
// within Frobenius 0.004 of R_world are near STATIC things (road piece, sign) and are dropped; R_ego = medoid
// of the rest (rotating wheels lose; a cost tie, e.g. 2 candidates, goes to the one nearer identity), or
// R_world if none is left / there were none / R_world is the identity fallback. InvRow3 = row 3 of
// inverse(MVP_cur) of the chosen world pair: 1/dot(InvRow3, (ndc,z,1)) = that pixel's clip w (view depth, m).
// kSolveShader-BEGIN (the build validates this block with fxc)
const char kSolveShader[] = R"(
cbuffer PairCB : register(b0) {
    uint4  Pairs[32];    // [layer*16 + k] = (prevByteAddr, curByteAddr, indexCount, 0)
    uint4  Counts;       // x = world pairs, y = cabin pairs, z = all matched world pairs (AllPairs, v0.6.0)
    float4 Params;       // x = near_reject_m (world layer only), y = ego_origin_m (v0.6.0), z = v0.10.0 phase 8: bit L = layer L
                         // has no medoid this frame (its candidates were dropped): this pass writes nothing for it
    uint4  AllPairs[128];// v0.6.0: every matched world pair (prevByteAddr, curByteAddr, indexCount, 0)
};
ByteAddressBuffer          CandPrev : register(t0);
ByteAddressBuffer          CandCur  : register(t1);
RWStructuredBuffer<float4> Solve    : register(u0);

float4x4 LoadM(ByteAddressBuffer b, uint addr) {
    return float4x4(asfloat(b.Load4(addr)),      asfloat(b.Load4(addr + 16)),
                    asfloat(b.Load4(addr + 32)), asfloat(b.Load4(addr + 48)));
}

// General 4x4 inverse via cofactors. Returns false if (near-)singular.
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

float Frob(float4x4 a, float4x4 b) {
    float d = 0;
    [unroll] for (int i = 0; i < 4; ++i)
        [unroll] for (int j = 0; j < 4; ++j) {
            float df = a[i][j] - b[i][j];
            d += df * df;
        }
    return sqrt(d);
}

[numthreads(2, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    uint layer = id.x;
    if (layer >= 2) return;
    if ((((uint)Params.z) >> layer & 1u) != 0u) return;     // v0.10.0 phase 8: the per-draw consensus answers for this layer
    uint n = min(Counts[layer], 16u);
    float nearM = (layer == 0) ? Params.x : 0.0;

    float4x4 R[16];
    bool     ok[16];
    bool     elig[16];
    float    wd[16];
    uint     nValid = 0, nFarOk = 0;
    [loop] for (uint k = 0; k < 16; ++k) {
        R[k] = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
        ok[k] = false;
        elig[k] = false;
        wd[k] = 0;
        if (k < n) {
            uint2 pc = Pairs[layer * 16 + k].xy;
            float4x4 Mp = LoadM(CandPrev, pc.x);
            float4x4 Mc = LoadM(CandCur,  pc.y);
            float4x4 ic;
            if (Inv4(Mc, ic)) {
                float4x4 r = mul(Mp, ic);
                float s = 0;
                [unroll] for (int i = 0; i < 4; ++i)
                    [unroll] for (int j = 0; j < 4; ++j)
                        s += r[i][j] * r[i][j];
                if (isfinite(s) && s < 1e30) {
                    R[k] = r; ok[k] = true; ++nValid;
                    // object-origin view depth: clip w of the local origin = MVP row3.w
                    float w = abs(Mc[3][3]);
                    wd[k] = w;
                    bool farEnough = isfinite(w) && w >= nearM;
                    elig[k] = farEnough;
                    if (farEnough) ++nFarOk;
                }
            }
        }
    }
    // Nothing passes the near filter -> fall back to every valid pair.
    bool fallback = (nFarOk == 0);
    if (fallback) {
        [unroll] for (uint q = 0; q < 16; ++q) elig[q] = ok[q];
    }

    int   best = -1;
    float bestCost = 3.0e38;
    [loop] for (uint i2 = 0; i2 < 16; ++i2) {
        if (elig[i2]) {
            float cost = 0;
            [loop] for (uint j2 = 0; j2 < 16; ++j2) {
                if (elig[j2] && j2 != i2) {
                    float d = 0;
                    [unroll] for (int a = 0; a < 4; ++a)
                        [unroll] for (int b = 0; b < 4; ++b) {
                            float df = R[i2][a][b] - R[j2][a][b];
                            d += df * df;
                        }
                    cost += sqrt(d);
                }
            }
            if (cost < bestCost) { bestCost = cost; best = (int)i2; }
        }
    }

    float4x4 outM = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    if (best >= 0) {
        outM = R[best];
    }
    [unroll] for (uint row = 0; row < 4; ++row)
        Solve[layer * 4 + row] = outM[row];
    // info: x = pairs offered, y = valid, z = chosen index (-1 = none -> identity)
    Solve[8 + layer] = float4((float)n, (float)nValid, (float)best, 0);
    // chosen pair: x = |w| (m), y = IndexCount, z = valid pairs rejected by the near filter,
    // w = 1 if the filter would have left nothing and was bypassed
    float cw = 0, cic = 0;
    if (best >= 0) { cw = wd[best]; cic = (float)Pairs[layer * 16 + best].z; }
    Solve[10 + layer] = float4(cw, cic, (float)(nValid - nFarOk), fallback ? 1.0 : 0.0);

    // ---- v0.6.0 ego reprojection (world layer only) -------------------------------------
    if (layer != 0) return;
    float4 invRow3 = float4(0, 0, 0, 0);
    if (best >= 0) {
        float4x4 Mw = LoadM(CandCur, Pairs[best].y);
        float4x4 iw;
        if (Inv4(Mw, iw) && all(isfinite(iw[3]))) invRow3 = iw[3];
    }
    // Scan: one dword (MVP_cur[3][3], byte 60 of the slot) per pair; invert only candidates. R[] is reused.
    uint  nAll  = min(Counts.z, 128u);
    float egoM  = Params.y;
    uint  nCand = 0;
    if (best >= 0 && egoM > 0.0) {
        [loop] for (uint a = 0; a < nAll; ++a) {
            if (nCand >= 16) break;
            uint2 pa = AllPairs[a].xy;
            float wq = abs(asfloat(CandCur.Load(pa.y + 60)));
            if (!(isfinite(wq) && wq < egoM)) continue;
            float4x4 Mp = LoadM(CandPrev, pa.x);
            float4x4 Mc = LoadM(CandCur,  pa.y);
            float4x4 ic;
            if (!Inv4(Mc, ic)) continue;
            float4x4 r = mul(Mp, ic);
            float s = 0;
            [unroll] for (int i = 0; i < 4; ++i)
                [unroll] for (int j = 0; j < 4; ++j)
                    s += r[i][j] * r[i][j];
            if (isfinite(s) && s < 1e30) { R[nCand] = r; ++nCand; }
        }
    }
    // Static reject: within 0.004 (Frobenius) of R_world = static scenery that happens to be near.
    uint nUsed = 0;
    [loop] for (uint e = 0; e < 16; ++e) {
        elig[e] = false;
        if (e < nCand && Frob(R[e], outM) >= 0.004) { elig[e] = true; ++nUsed; }
    }
    float4x4 I4 = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    int   egoBest = -1;
    float egoCost = 3.0e38, egoDistI = 3.0e38;
    [loop] for (uint i3 = 0; i3 < 16; ++i3) {
        if (elig[i3]) {
            float cost = 0;
            [loop] for (uint j3 = 0; j3 < 16; ++j3) {
                if (elig[j3] && j3 != i3) cost += Frob(R[i3], R[j3]);
            }
            float dI  = Frob(R[i3], I4);
            float tol = 1e-5 * max(cost, 1e-6);
            bool better = (cost < egoCost - tol) || (cost <= egoCost + tol && dI < egoDistI);
            if (better) { egoCost = cost; egoDistI = dI; egoBest = (int)i3; }
        }
    }
    float4x4 egoR = outM;
    bool egoFb = true;
    if (egoBest >= 0) { egoR = R[egoBest]; egoFb = false; }
    [unroll] for (uint row2 = 0; row2 < 4; ++row2)
        Solve[12 + row2] = egoR[row2];
    Solve[16] = invRow3;
    // ego info: x = candidates (valid R), y = used after the static reject, z = 1 if R_ego = R_world,
    // w = |R_ego - R_world| (Frobenius)
    Solve[17] = float4((float)nCand, (float)nUsed, egoFb ? 1.0 : 0.0, Frob(egoR, outM));
}
)";
// kSolveShader-END

// ---------------------------------------------------------------------------
// Pass B: per pixel, layer from the depth value (>= 0.9 = cabin [0.9,1.0],
// else world [0.01,0.9]; d == 0 sky -> world, z_ndc = 0 = infinitely far in
// reversed-Z, so rotation-only reprojection falls out). v0.5.0 VR far/sky layer (viewport
// depth range [0, 0.009]): d < 0.01 gives saturate((d-0.01)/0.89) = 0, i.e. the same z_ndc = 0
// path, so it needs no extra branch; cabin stays d >= 0.9. Reproject through the
// layer's R and write the pixel delta (DLSS: current + mv = previous).
// v0.5.7: merged with the depth-convert pass (was kReprojShader reading the R32F copy). Reads the depth
// TWIN directly (R32G8X24_TYPELESS through its R32_FLOAT_X8X24_TYPELESS SRV) and also writes the flattened
// R32F depth NGX consumes (u1, value untouched) -- one full-resolution pass instead of two. Same MV math.
// v0.6.0: a world-layer pixel (not cabin, not the far/sky layer d < 0.01) whose view depth
// 1/dot(InvRow3, (ndc,z,1)) is below EgoPixelM metres is part of the own truck's exterior and is reprojected
// through R_ego (Solve[12..15]) instead of R_world. InvRow3 all zero (no chosen world pair) = never ego.
// v0.6.4 DLAA area: the dispatch covers only the crop (Size); the depth twin is FULL size and is read at
// Origin + id; uv / ndc and the MV pixel scale use FullSize, so the MVs are the same pixel deltas as for the whole
// image (crop coordinates = full coordinates - Origin, a constant). Whole image: Origin 0, FullSize = Size.
// v0.9.0 r5 forward depth (t4, FULL size like the twin, read at the same src; unbound = reads 0): the depth of the forward
// pass's alpha-blended draws that do not write the scene depth (wires, cables, fences, lane paint; re-drawn by inject.cpp
// into their own depth target, alpha >= 0.5 through alpha-to-coverage). d = max(twin, forward): the nearer surface wins
// (reversed-Z), so a wire in front of the sky is reprojected as a world point at its own depth instead of the sky's
// z_ndc = 0 (rotation only, no parallax = the DLSS trail along the wire). A forward surface behind opaque geometry loses
// (the twin is nearer). The flattened R32F depth NGX reads is the same merged value. max(d, 0) = d when t4 is unbound.
// v0.9.0 r11 (round-9 log + video: the road, the side-window view and the near verge seen through the windscreen were
// classified CABIN, (0.5, 0.5, 1.0) in the Ctrl+F6 view, with zero motion -- weather / light dependent): a world-layer
// alpha-blended overlay right at the camera (windscreen drops / dirt / glare) qualified for the re-draw and wrote NEAR
// depth (>= 0.9) over the whole glass; max() then made every pixel behind it "cabin" (R_cabin, z remapped into [0.9,
// 1]). The forward depth may now win only inside the WORLD range [0.01, 0.9); the rest reads as 0 (the twin's depth
// stays). Pixels whose forward depth was >= 0.9 are counted on a 1-in-16 grid (every 4th column and row) into 64
// striped dword counters (u2, CameraMv::FwdNearPx: x 16 on the CPU); unbound u2 = no-op.
// v0.10.0 per-draw MVs (mv_objects = 2, ObjInfo.z = draws): t5 = the pass's draw-id target (replay with depth EQUAL, see
// draw_ids.h), t6 = the per-draw R table (DrawIdMv). A pixel owned by a MOVER draw (state 3) is reprojected with that
// draw's R = MVP_prev * inverse(MVP_cur), z_ndc from the draw's own viewport depth range; every other pixel (static,
// unpaired, instanced, sky, forward depth won) takes the unchanged camera path. u3 = coverage counters on a 1-in-64 grid
// (world px, px with an id, mover px, px of an unpaired draw; 16 stripes each; DrawIdMv reads them back).
// v0.10.0 phase 3: the draw-id target may be R16G16_UINT: .x = the G-buffer draw that owns the pixel (ids 1..ObjInfo.w), .y =
// the FORWARD draw that owns the pixel's forward depth (ids ObjInfo.w + 1..ObjInfo.z; an R16_UINT target reads .y = 0). Where
// the forward depth won (fw > dt: a plate, a wire, glass in front of the scene) the forward id decides the R, elsewhere the
// G-buffer id; zd always from the merged depth d (= fw on a forward pixel). Forward counters: forward px, with a forward
// id, forward movers (dwords 128 / 144 / 160 + stripe). The camera R in Solve[0..7] is the per-draw CONSENSUS when one was
// found (DrawIdMv CSPick), else pass A's medoid.
// v0.10.0 phase 4 ATTACH per pixel (AttachM = dlaa.ini mv_fwd_attach_m, 0 = off): where the forward depth won and the G-buffer
// draw UNDER the pixel (RED id: the surface a plate / decal sits on) is a MOVER and the forward surface lies within AttachM
// metres in front of it (view distance through InvRow3 = Solve[16]; without InvRow3 within ~2 % of the distance), the pixel
// takes that mover's R (z from the merged depth) -- unless the forward draw is a mover itself whose own R agrees within 0.5 px
// at this pixel. In game a bus plate's own pair carried no motion (pale in Ctrl+F6) while the body moved. Counted on the
// 1-in-64 grid at dword 176 + stripe.
// kReprojDepthShader-BEGIN (the build validates this block with fxc)
const char kReprojDepthShader[] = R"(
cbuffer DimsCB : register(b0) {
    float2 Size;         // crop = dispatch size (MV / R32F textures)
    float  EgoPixelM;    // v0.6.0: world pixels closer than this (m) use R_ego; 0 = off
    float  AttachM;      // v0.10.0 phase 4: forward-pixel attach radius (m); 0 = off
    float2 Origin;       // v0.6.4: crop origin in the full image (px, even)
    float2 FullSize;     // v0.6.4: full image size (px) = depth twin size
    float4 TileXf;       // v0.7.8: full-picture ndc -> reference-tile ndc = ndc * xy + zw (identity 1,1,0,0 = untiled)
    uint4  ObjInfo;      // v0.9.0: x = valid stencil-id mask (bit s = id s has its own R this frame), 0 = per-object MVs off
                         // (r2: y = the debug view's "rejected" id, never in x: those pixels keep the camera R)
                         // v0.10.0: z = draws of the per-draw record (draw ids 1..z valid), 0 = per-draw MVs off
                         // v0.10.0 phase 3: w = G-buffer draws (ids 1..w); forward ids w + 1..z
    uint4  FwdMode;      // v0.10.0 phase 9: x = the forward depth (t4) is at 1/2^x of the image (read at src >> x); y = 1: the
                         // forward ids come from FwdIdsH (t7, the forward depth's size), not the GREEN channel of t5
                         // (v0.10.0 phase 14: z = the static replay gate ran -- read by the Ctrl+F6 view only)
};
Texture2D<float>           DepthIn   : register(t0);
StructuredBuffer<float4>   Solve     : register(t1);
Texture2D<uint2>           StencilIn : register(t2);   // v0.9.0: X32_TYPELESS_G8X24_UINT view of the twin (.y = stencil)
StructuredBuffer<float4>   SlotR     : register(t3);   // v0.9.0: [s*4 + row] = R of id s, [64 + s].x = 1 if usable
Texture2D<float>           FwdDepth  : register(t4);   // v0.9.0 r5: forward (blended, no depth write) depth; unbound = 0
Texture2D<uint2>           DrawIds   : register(t5);   // v0.10.0: draw id per pixel (.x G-buffer, .y forward; 0 = none), full size
StructuredBuffer<float4>   DrawR     : register(t6);   // v0.10.0: [i*5 + r] = R rows, [i*5 + 4] = (state, vpMin, vpMax, dev)
Texture2D<uint>            FwdIdsH   : register(t7);   // v0.10.0 phase 9: forward ids at the forward depth's size (FwdMode.y)
RWTexture2D<float2>        MvOut     : register(u0);
RWTexture2D<float>         DepthOut  : register(u1);
RWByteAddressBuffer        FwdCnt    : register(u2);   // v0.9.0 r11: near-range forward-depth px (64 dwords)
RWByteAddressBuffer        DidCnt    : register(u3);   // v0.10.0: coverage counters at dword 64 + counter * 16 + stripe

// v0.10.0 phase 4: view distance (m) of a world-layer depth value through InvRow3; -1 = unknown (no InvRow3)
float ViewDist(float2 ndc, float dep) {
    float4 ir = Solve[16];
    if (!any(ir != 0.0)) return -1.0;
    float iw = dot(ir, float4(ndc, saturate((dep - 0.01) / 0.89), 1.0));
    return iw > 1e-9 ? 1.0 / iw : -1.0;
}
// the forward surface (depth fwd, in front: reversed-Z fwd > scene) sits on the scene surface within AttachM metres
bool AttachGapOk(float2 ndc, float fwd, float scene) {
    float a = ViewDist(ndc, fwd), b = ViewDist(ndc, scene);
    if (a > 0.0 && b > 0.0) return b - a <= AttachM;
    return (fwd - 0.01) <= (scene - 0.01) * 1.02;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)Size.x || id.y >= (uint)Size.y) return;

    uint2 src = id.xy + (uint2)Origin;                  // v0.6.4: pixel in the full image
    // v0.9.0 r11: only a forward depth in the WORLD range wins (an overlay at the camera wrote >= 0.9: counted,
    // ignored)
    float fw = FwdDepth[src >> FwdMode.x];              // v0.10.0 phase 9: a 1/2-resolution forward depth at src >> 1
    if (fw >= 0.9 && ((src.x | src.y) & 3u) == 0u) {
        uint oc;
        FwdCnt.InterlockedAdd(((src.x >> 2) & 63u) * 4u, 1u, oc);
    }
    if (!(fw >= 0.01 && fw < 0.9)) fw = 0.0;
    float dt = DepthIn[src];
    float d = max(dt, fw);                              // v0.9.0 r5: the nearer of scene depth and forward depth
    DepthOut[id.xy] = d;
    float2 uv  = (float2(src) + 0.5) / FullSize;
    // v0.7.8 tiled preview: R (and InvRow3) live in the clip space of the REFERENCE TILE pass, the picture is the
    // composite of all tiles. ndc = this pixel in reference-tile ndc; the reprojected point goes back to full ndc below.
    // Identity (1,1,0,0) for everything untiled (world path: exact, x * 1 + 0 == x).
    // (v0.10.0 phase 4: computed here, before the per-draw block, which needs it for the attach)
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) * TileXf.xy + TileXf.zw;
    // v0.10.0 per-draw MVs: the G-buffer draw that owns this pixel (replayed with depth EQUAL). Only a MOVER (state 3:
    // paired, its own R differs from the camera R) changes the reprojection; static / unpaired / instanced draws keep the
    // camera path below unchanged. A pixel whose forward depth won shows the forward surface (a wire), not that draw.
    bool didMover = false;
    float4x4 Rd = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    float zd = 0.0;
    if (ObjInfo.z != 0u) {
        bool fwWin = fw > dt;                           // v0.10.0 phase 3: the forward surface is what this pixel shows
        uint2 ids = DrawIds[src];
        if (FwdMode.y != 0u) ids.y = FwdIdsH[src >> FwdMode.x];   // v0.10.0 phase 9: the 1/2-resolution forward ids
        uint did = fwWin ? ids.y : ids.x;
        bool idOk = fwWin ? (did > ObjInfo.w && did <= ObjInfo.z) : (did != 0u && did <= ObjInfo.w);
        uint st = 0u;
        if (idOk) {
            uint o = (did - 1u) * 5u;
            float4 info = DrawR[o + 4u];
            st = (uint)(info.x + 0.5);
            if (st == 3u) {
                didMover = true;
                Rd = float4x4(DrawR[o], DrawR[o + 1u], DrawR[o + 2u], DrawR[o + 3u]);
                zd = saturate((d - info.y) / max(info.z - info.y, 1e-6));    // the draw's own viewport depth range
            }
        } else did = 0u;
        // v0.10.0 phase 4 ATTACH per pixel (see the header): the forward surface on a G-buffer mover takes the mover's R
        bool attached = false;
        if (fwWin && AttachM > 0.0 && ids.x != 0u && ids.x <= ObjInfo.w) {
            uint og = (ids.x - 1u) * 5u;
            float4 gi = DrawR[og + 4u];
            if ((uint)(gi.x + 0.5) == 3u && AttachGapOk(ndc, fw, dt)) {
                float4x4 Rg = float4x4(DrawR[og], DrawR[og + 1u], DrawR[og + 2u], DrawR[og + 3u]);
                float zg = saturate((d - gi.y) / max(gi.z - gi.y, 1e-6));
                bool keep = false;
                if (didMover) {
                    float4 ca = mul(Rd, float4(ndc, zd, 1.0)), cb = mul(Rg, float4(ndc, zg, 1.0));
                    keep = ca.w > 1e-6 && cb.w > 1e-6 && length((ca.xy / ca.w - cb.xy / cb.w) * FullSize * 0.5) < 0.5;
                }
                if (!keep) { didMover = true; Rd = Rg; zd = zg; attached = true; }
            }
        }
        if (((src.x | src.y) & 7u) == 0u && d >= 0.01) {
            uint oc, s = (src.x >> 3) & 15u;
            if (!fwWin) {
                DidCnt.InterlockedAdd((64u + s) * 4u, 1u, oc);
                if (did != 0u) DidCnt.InterlockedAdd((80u + s) * 4u, 1u, oc);
                if (didMover) DidCnt.InterlockedAdd((96u + s) * 4u, 1u, oc);
                if (st == 1u) DidCnt.InterlockedAdd((112u + s) * 4u, 1u, oc);
            } else {
                DidCnt.InterlockedAdd((128u + s) * 4u, 1u, oc);
                if (did != 0u) DidCnt.InterlockedAdd((144u + s) * 4u, 1u, oc);
                if (didMover) DidCnt.InterlockedAdd((160u + s) * 4u, 1u, oc);
                if (attached) DidCnt.InterlockedAdd((176u + s) * 4u, 1u, oc);   // v0.10.0 phase 4
            }
        }
    }
    bool  cabin = d >= 0.9;
    float lo = cabin ? 0.9 : 0.01;
    float hi = cabin ? 1.0 : 0.9;
    float z  = saturate((d - lo) / (hi - lo));

    uint base = cabin ? 4u : 0u;
    // v0.9.0 per-object MVs: a world pixel whose stencil upper nibble holds a valid object id uses that id's own R
    // (the object's MVP_prev * inverse(MVP_cur)); everything else keeps the camera / ego R. Off (ObjInfo.x == 0) = no read.
    uint sid = 0;
    if (!cabin && d >= 0.01 && ObjInfo.x != 0u) {
        sid = StencilIn[src].y >> 4;
        if (sid == 0u || ((ObjInfo.x >> sid) & 1u) == 0u || !(SlotR[64u + sid].x > 0.5)) sid = 0;
    }
    // v0.6.0 ego: near world pixels move with the camera's vehicle -> R_ego
    if (sid == 0u && !cabin && d >= 0.01 && EgoPixelM > 0.0) {
        float4 ir = Solve[16];
        if (any(ir != 0.0)) {
            float iw   = dot(ir, float4(ndc, z, 1.0));
            float dist = (abs(iw) > 1e-9) ? 1.0 / iw : 1e9;
            if (dist > 0.0 && dist < EgoPixelM) base = 12u;
        }
    }
    float4x4 R = float4x4(Solve[base], Solve[base + 1], Solve[base + 2], Solve[base + 3]);
    if (sid != 0u) R = float4x4(SlotR[sid * 4u], SlotR[sid * 4u + 1u], SlotR[sid * 4u + 2u], SlotR[sid * 4u + 3u]);
    float4 c   = didMover ? mul(Rd, float4(ndc, zd, 1.0)) : mul(R, float4(ndc, z, 1.0));   // v0.10.0: a mover's own R

    float2 mv = float2(0.0, 0.0);
    if (c.w > 1e-6) {
        float2 pn = (c.xy / c.w - TileXf.zw) / TileXf.xy;      // v0.7.8: reference-tile ndc -> full-picture ndc
        float2 prevUv = float2(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5);
        mv = (prevUv - uv) * FullSize;
        if (!all(isfinite(mv))) mv = float2(0.0, 0.0);
        mv = clamp(mv, -4096.0, 4096.0);
    }
    MvOut[id.xy] = mv;
}
)";
// kReprojDepthShader-END

// ---------------------------------------------------------------------------
// v0.9.0 per-object MVs (ObjRecord, one thread per recorded world draw of the pass being blitted):
//  1. gather: this draw's MVP (rows 4..7 of its VS cbuffer window) from the ring mirror -> Cur[i] (the next frame's Prev)
//  2. pair: among the previous pass's draws with the SAME geometry key (up to 8, PrevList run from the CPU), take the one
//     whose origin lies nearest to where the camera R alone puts this draw's origin (identical meshes: two trucks of
//     one model, reordered draws -- the right partner is the nearest, a wrong one is metres away)
//  3. verdict: 5 probe points (origin, +-2 m on local x / z): real previous clip position (Mp * x) vs the camera-only
//     prediction (R_cam * Mc * x). dev = max screen deviation (px), disp = max clip-space distance (~ m). A pair with
//     disp >= Params.y is not plausible (a wrong partner) and never used.
//  4. the draw that represents stencil id s this pass (Table.z >> 8): R_s = Mp * inverse(Mc) -> SlotR rows, info.x = 1
//  5. a plausible pair with dev > Params.x whose origin is not part of the own truck (|w| >= Params.z): appended to
//     Moving (index, dev, |w|, index count, origin clip, its R) for the CPU id assignment (async readback, ObjIds)
// v0.9.0 r2 (first in-game test: grass clumps, window grids, tree rows were "moving" with the truck standing still).
// Root cause: identical meshes drawn dozens of times. Only the FIRST 8 previous draws of a key (draw order) were pairing
// candidates, so occurrence 30 of a grass clump was paired with the nearest of occurrences 0..7 -- a different clump a
// few metres away: plausible (< 6 m), dev >> 0.5 px -> "moving", with a garbage R. And the identity (key + occurrence
// index) of such a draw names a different clump whenever the engine re-orders them. A pair is now TRUSTED only if
//   a. the key is drawn at most mv_objects_max_dup times (this and the previous pass; CPU flag Table.z bit 12),
//   b. the nearest candidate IS the identity's own previous occurrence (same key, same occurrence index; the CPU passes
//      its rank in the candidate run, Table.z bits 4..7, 15 = none) -- pairing and identity agree,
//   c. the nearest candidate is clearly nearer than the second (second >= 1.5 x best + Params.w),
// and only a trusted pair gives an id its R. A draw that would have been "moving" (plausible, dev > Params.x) but is not
// trusted, or whose origin is closer than mv_objects_ego_m (own truck: Params.z), goes to the REJECT list instead
// (index, reason 1 dup / 2 pairing / 3 ambiguous / 4 own truck, dev, |w|); the moving entries carry the origin's
// displacement (prev clip x, y, w ~ m per frame) for the CPU coherence test.
// v0.9.0 r3: (1) Table.x is a byte offset into ONE mirror that holds up to 3 cbuffer rings at per-ring bases (ObjRecord),
// so the gather is unchanged. (2) reject entries are 64 bytes: + the origin in current clip, the nearest candidate's
// previous origin and the identity's own previous occurrence's previous origin (bit 8 of the reason = valid), so the CPU
// can let a rejected wheel FOLLOW a tagged vehicle (ObjIds::Update, follower rule).
// v0.9.0 r4 (round-3 in-game regression: distant traffic ghosting / wobbling): (1) SPIN per draw (local axes turned beyond
// the camera's motion; moving entries carry it, 128-byte entries): a spinning wheel's R must never stand for its vehicle.
// (2) The id's representative is chosen HERE, per frame, among the id's tagged draws (CSResolve: biggest trusted,
// plausible, non-spinning; Table.z bits 8..11 = the draw's id, bit 13 = may not represent) instead of by the CPU (biggest
// tagged draw, even when its pairing failed that frame). (3) CSVeto: an id whose tagged draw contradicts its motion falls
// back to the camera R for the frame. Three dispatches of this source (CSMain, CSResolve, CSVeto).
// v0.9.0 r6 (round-4 in-game log: distant cars wobble where their tag comes and goes): (1) the representative prefers the
// CPU's anchor (Table.z bit 14; key bit 31), so the GPU's R and the CPU's membership reference are the same draw's; (2) the
// veto fires only on gross errors (> max(4 px, 50 %), Img.w / Ids.y; r4: 1.5 px / 35 %) and appends a VETO record (reject
// reason 6) so the CPU can take a member that contradicts its id while not moving itself out of the id.
// kObjShader-BEGIN (the build validates this block with fxc)
const char kObjShader[] = R"(
cbuffer ObjCB : register(b0) {
    uint4  Counts;       // x = draws this pass, y = Moving capacity, z = PrevList length, w = Reject capacity (r2)
    float4 Params;       // x = moving threshold (px), y = max plausible displacement per frame (clip units ~ m),
                         // z = own-truck origin (m, mv_objects_ego_m), w = pairing margin (clip units ~ m, r2)
    float4 Img;          // xy = full image size (px), z = spin limit (~rad / frame, r4), w = veto tolerance (px, r4)
    uint4  Ids;          // r4: x = valid id mask (ids with a tagged draw that may represent them), r6: y = veto fraction of the
                         // id's correction (float bits; r4: 0.35 fixed), r7: z = own-truck lock limit (px, float bits,
                         // mv_objects_lock_px), w = hold frames (mv_objects_hold; 0 = no holds = r6)
    float4 Ext;          // r10: x = origin margin (mv_objects_origin_margin: |x / w|, |y / w| of an on-screen origin),
                         // r11: y = camera-attached radius (mv_objects_cam_m, m)
};
ByteAddressBuffer          Mirror   : register(t0);   // copy of the game's VS cbuffer ring(s) (r3: ring base + byte offset)
// x = MVP byte offset, y = first PrevList entry (0xFFFFFFFF none), w = index count,
// z = candidates (bits 0..3) | identity partner rank (4..7, 15 = none) | dup over cap (bit 12) |
//     r4: the draw's stencil id (bits 8..11; r2/r3: the id it represented) | may not represent it (bit 13) |
//     r6: it is its id's CPU anchor (bit 14: preferred representative) | r8: it is in the CPU's EGO SET (bit 15: the
//     own truck, decided by the temporal persistence of its screen position -- never a mover, never represents)
StructuredBuffer<uint4>    Table    : register(t1);
ByteAddressBuffer          Prev     : register(t2);   // previous pass of this eye: gathered MVPs (64 B per draw)
StructuredBuffer<uint>     PrevList : register(t3);   // previous draw indices sorted by geometry key
StructuredBuffer<float4>   Solve    : register(t4);   // rows 0..3 = R_world (pass A)
RWByteAddressBuffer        Cur      : register(u0);   // this pass: gathered MVPs
// [s*4 + row] = R of id s, [64 + s] = (usable, state, value, draw); r7: [80 + s*4 + row] / [144 + s] = the same as they
// were at the start of this frame (CSResolve's copy; [144 + s].w = 1: this frame's R came from a fresh representative)
RWStructuredBuffer<float4> SlotR    : register(u1);
// dword 0 = moving count, dword 1 = reject count, r4: dword 2 = vetoed tagged draws, dword 3 = ids without a trusted
// member; moving entries of 128 bytes (r4; r2/r3: 112) from byte 16, reject entries of 64 bytes after Counts.y moving ones
// r7: + a 16-byte TAIL after Counts.w reject entries (TailAddr): held-id mask, hold-expired mask, near-locked draws,
// near-accepted draws
RWByteAddressBuffer        Moving   : register(u2);
// r4: [0..15] per id: the winning representative key ((index count + 1) << 11 | draw, 0 = none), [16 + i] = draw i's
// trusted partner in the previous pass (0xFFFFFFFF = none)
RWByteAddressBuffer        Scratch  : register(u3);

float4x4 LoadM(ByteAddressBuffer b, uint a) {
    return float4x4(asfloat(b.Load4(a)), asfloat(b.Load4(a + 16)), asfloat(b.Load4(a + 32)), asfloat(b.Load4(a + 48)));
}
float4x4 LoadM(RWByteAddressBuffer b, uint a) {
    return float4x4(asfloat(b.Load4(a)), asfloat(b.Load4(a + 16)), asfloat(b.Load4(a + 32)), asfloat(b.Load4(a + 48)));
}
float4 LoadCol3(ByteAddressBuffer b, uint a) {
    return float4(asfloat(b.Load(a + 12)), asfloat(b.Load(a + 28)), asfloat(b.Load(a + 44)), asfloat(b.Load(a + 60)));
}
float4 LoadCol3(RWByteAddressBuffer b, uint a) {
    return float4(asfloat(b.Load(a + 12)), asfloat(b.Load(a + 28)), asfloat(b.Load(a + 44)), asfloat(b.Load(a + 60)));
}
// r7: the counters after the reject list (r10: 80-byte rejects; r11: + 208 = camera-attached draws)
uint TailAddr() { return 16u + Counts.y * 128u + Counts.w * 80u; }

// v0.9.0 r10: the origin's VIEW-SPACE position (m) from the MVP alone. The projection's row 3 is (0, 0, +-1, 0), so MVP
// row 3 = +-(view row 2) and |row 3.xyz| = the world matrix's scale s; row 0 = fx (view row 0) + p (view row 2), so pe
// = dot(row 0, row 3) / s^2 is the skew and |row 0 - pe row 3| / s the focal fx (row 1: q, fy), for a world matrix of
// uniform scale (non-uniform: a part-constant affine error, harmless for the ego set's persistence test). Then X =
// (c0.x - pe c0.w) / fx, Y = (c0.y - qe c0.w) / fy, Z = c0.w -- valid near / behind the camera plane, where c0.xy / w
// blows up. .w = 1: valid.
float4 ViewOrigin(float4x4 M) {
    float3 r0 = M[0].xyz, r1 = M[1].xyz, r3 = M[3].xyz;
    float s2 = dot(r3, r3);
    if (!(s2 > 1e-20)) return float4(0.0, 0.0, 0.0, 0.0);
    float s = sqrt(s2);
    float pe = dot(r0, r3) / s2, qe = dot(r1, r3) / s2;
    float fx = length(r0 - pe * r3) / s, fy = length(r1 - qe * r3) / s;
    if (!(fx > 1e-6) || !(fy > 1e-6)) return float4(0.0, 0.0, 0.0, 0.0);
    float3 v = float3((M[0][3] - pe * M[3][3]) / fx, (M[1][3] - qe * M[3][3]) / fy, M[3][3]);
    return all(isfinite(v)) ? float4(v, 1.0) : float4(0.0, 0.0, 0.0, 0.0);
}
// v0.9.0 r11 CAMERA-ATTACHED (round-9 ego trace: every near sighting at view pos 0 0 0, w 0 -- 35 per readback, 239 of
// 240 near copies flagged): the origin within Ext.y (mv_objects_cam_m) of the camera -- view-space length, or clip |w|,
// |x|, |y| all below it. Rows 4..7 of such a draw hold no object transform (the view-projection of a batched /
// instanced draw, an overlay): never a candidate, mover, representative, ego sighting or attach anchor
// (ObjIds::CamAttached = CPU)
bool CamOrigin(float4x4 M) {
    float4 v = ViewOrigin(M);
    if (v.w > 0.5 && length(v.xyz) < Ext.y) return true;
    return abs(M[3][3]) < Ext.y && abs(M[0][3]) < Ext.y && abs(M[1][3]) < Ext.y;
}
// r3 / r10: one reject entry (80 B: draw, reason bits, dev, |w| | origin | nearest candidate's previous origin | the
// identity's own previous occurrence's previous origin | r10: view-space origin + valid)
void PutRej(uint i, uint whyBits, float dev, float w0, float4 c0, float4 a0, float4 ao, float4 vo) {
    uint rs;
    Moving.InterlockedAdd(4, 1u, rs);
    if (rs >= Counts.w) return;
    uint a = 16u + Counts.y * 128u + rs * 80u;
    Moving.Store4(a,       uint4(i, whyBits, asuint(dev), asuint(w0)));
    Moving.Store4(a + 16u, asuint(c0));
    Moving.Store4(a + 32u, asuint(a0));
    Moving.Store4(a + 48u, asuint(ao));
    Moving.Store4(a + 64u, asuint(vo));
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

// v0.9.0 r9: a second split (the CSMain part grew past the 16380-byte literal cap with the r9 guard): )" R"(
[numthreads(64, 1, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= Counts.x) return;
    uint4 t = Table[i];
    float4x4 Mc = LoadM(Mirror, t.x);
    Cur.Store4(i * 64u,       asuint(Mc[0]));
    Cur.Store4(i * 64u + 16u, asuint(Mc[1]));
    Cur.Store4(i * 64u + 32u, asuint(Mc[2]));
    Cur.Store4(i * 64u + 48u, asuint(Mc[3]));
    Scratch.Store(64u + i * 4u, 0xFFFFFFFFu);                         // r4: no trusted partner (yet)
    uint tag = (t.z >> 8) & 15u;                                      // r4: this draw's stencil id (0 = untagged)
    // v0.9.0 r11: a camera-attached draw is counted (tail + 208) and is nothing else; a TAGGED one is reported (reason
    // 9, bit 9 = no candidate) so the CPU releases its membership
    if (CamOrigin(Mc)) {
        uint oc;
        Moving.InterlockedAdd(TailAddr() + 208u, 1u, oc);
        if (tag != 0u)
            PutRej(i, 9u | 0x200u, 0.0, abs(Mc[3][3]), float4(Mc[0][3], Mc[1][3], Mc[2][3], Mc[3][3]),
                   float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0), ViewOrigin(Mc));
        return;
    }
    bool ego = (t.z & 0x8000u) != 0u;                                 // r8: CPU ego set (own truck)
    bool canRep = tag != 0u && (t.z & 0x2000u) == 0u && !ego && ((Ids.x >> tag) & 1u) != 0u;
    uint nc  = min(t.z & 15u, 8u);
    uint idr = (t.z >> 4) & 15u;                                      // r2: rank of the identity's own previous draw
    bool dupX = (t.z & 0x1000u) != 0u;                                // r2: key drawn more than mv_objects_max_dup times
    if (t.y == 0xFFFFFFFFu || nc == 0u) return;
    float4x4 Rc = float4x4(Solve[0], Solve[1], Solve[2], Solve[3]);
    float4 c0 = float4(Mc[0][3], Mc[1][3], Mc[2][3], Mc[3][3]);      // this draw's origin, current clip
    // v0.9.0 r10 ORIGIN GATE (round 8: the road, a verge and its trees carried a vehicle's R for 13 s -- static batches
    // joined an id by R similarity and their origin, anywhere, often behind the camera, never got a verdict): only a
    // draw whose ORIGIN is on screen (viewport + Ext.x margin) can be a mover or represent its id
    bool onScr = c0.w > 0.05 && abs(c0.x) <= Ext.x * c0.w && abs(c0.y) <= Ext.x * c0.w;
    float4 p0 = mul(Rc, c0);                                          // ... where the camera alone puts it last frame
    float best = 3.0e38, second = 3.0e38;
    uint  bi = PrevList[t.y], bk = 0u;
    [loop] for (uint k = 0; k < nc; ++k) {
        uint  pj = PrevList[t.y + k];
        float4 a0 = LoadCol3(Prev, pj * 64u);
        float dd = length(float3(a0.xy - p0.xy, a0.w - p0.w));
        if (dd < best) { second = best; best = dd; bi = pj; bk = k; }
        else if (dd < second) second = dd;
    }
    float4x4 Mp = LoadM(Prev, bi * 64u);

    float dev = 0.0, disp = 0.0, lock0 = 1.0e9;                       // r8: lock0 stays 1e9 = origin not measured
    uint used = 0;
    float4 pcc[5];                                                    // r9: the probe points (current clip) dev used
    bool   pok[5];
    [unroll] for (uint q = 0; q < 5; ++q) {
        float3 o = (q == 0u) ? float3(0, 0, 0) : ((q == 1u) ? float3(2, 0, 0) : ((q == 2u) ? float3(-2, 0, 0)
                 : ((q == 3u) ? float3(0, 0, 2) : float3(0, 0, -2))));
        float4 x  = float4(o, 1.0);
        float4 cc = mul(Mc, x);
        float4 pc = mul(Rc, cc);
        float4 ac = mul(Mp, x);
        pcc[q] = cc;
        pok[q] = cc.w > 0.05 && pc.w > 0.05 && ac.w > 0.05;
        if (pok[q]) {
            float2 dpx = (ac.xy / ac.w - pc.xy / pc.w) * 0.5 * Img.xy;
            dev  = max(dev, length(dpx));
            disp = max(disp, length(float3(ac.xy - pc.xy, ac.w - pc.w)));
            // r7: how far the point moved ON SCREEN in the frame (previous clip vs current clip, no camera term);
            // r8: measured at the origin only (the probe points at +-2 m swing with the cabin's idle shake)
            if (q == 0u) lock0 = length((ac.xy / ac.w - cc.xy / cc.w) * 0.5 * Img.xy);
            ++used;
        }
    }
    bool plaus = used > 0u && isfinite(dev) && isfinite(disp) && disp < Params.y;
    float w0 = abs(Mc[3][3]);
    // r2: trusted pairing (see the comment above the shader) -- why = the first failed rule (0 = trusted)
    bool consistent = idr != 15u && bk == idr;
    bool unambig = nc == 1u || second >= best * 1.5 + Params.w;
    uint why = dupX ? 1u : (!consistent ? 2u : (!unambig ? 3u : 0u));
    bool trusted = why == 0u;
    bool cand = plaus && dev > Params.x;                              // "moving" under the v0.9.0 rules

    // r4 SPIN: how far the draw's local axes turned in this frame beyond the camera's motion (~radians; the clip-space x / y
    // carry the focal factors, so within ~x1.8). Column a of an MVP is local axis a in clip space; for anything that does
    // not rotate on its own, R_cam * (Mc column a) == Mp column a. A vehicle yaws < 0.03 rad / frame, a wheel turns > 0.1
    // above ~10 km/h: its R carries the spin and must never stand for a vehicle (CPU: never anchors, GPU: never represents).
    float spin = 0.0;
    [unroll] for (uint ax = 0u; ax < 3u; ++ax) {
        float4 vp = float4(Mp[0][ax], Mp[1][ax], Mp[2][ax], Mp[3][ax]);
        float4 vc = mul(Rc, float4(Mc[0][ax], Mc[1][ax], Mc[2][ax], Mc[3][ax]));
        float  lv = max(length(vp.xyw), length(vc.xyw));
        if (lv > 1e-6) spin = max(spin, length(vp.xyw - vc.xyw) / lv);
    }
    if (!isfinite(spin)) spin = 1.0e9;
    bool spinning = spin > Img.z;
    // r7 OWN TRUCK = NEAR AND CAMERA-LOCKED (r2..r6: near only). The own truck's exterior rides with the camera: its
    // clip position does not change from frame to frame (Mp * x == Mc * x up to the cabin's sway). A vehicle passing
    // alongside also has its origin within mv_objects_ego_m (8 m) but crosses the screen by tens of px per frame: r2..r6
    // called it the own truck (a part that was not a member yet could neither take nor join an id: cyan / orange while
    // passing). A SPINNING draw is judged at its origin only (its probe points turn with it): the own truck's wheels stay
    // the own truck, a passing truck's wheels are movers (they can only join an id). Same rule per eye in VR.
    // v0.9.0 r8 (round-6 log, truck standing: the own truck's exterior parts at 1.1..1.3 m held 4 of 15 ids for the
    // whole video -- the idle shake swings the probe points at +-2 m by > 3 px while the origin moves a few px): the
    // lock test is now a cheap PRE-FILTER at the ORIGIN only, for every draw (lock0 <= mv_objects_lock_px, default 6;
    // an origin that could not be measured -- behind the camera -- is NOT locked). The own truck itself is decided on
    // the CPU by the temporal persistence of the origin's screen position (ObjIds ego set, Table.z bit 15): such a draw
    // is never a mover and never represents its id; when "moving" and trusted it is a reason-4 reject, so the CPU keeps
    // seeing it (the flag is dropped when its origin drifts away).
    bool nearO  = w0 < Params.z;
    bool locked = nearO && lock0 <= asfloat(Ids.z);                   // r8: origin only (lock0 = 1e9: not measured)
    bool own    = locked || ego;
    bool moving = cand && trusted && !own;
    if (cand && !moving && trusted) why = 4u;                         // trusted, but the own truck (locked / ego set)
    // v0.9.0 r10: an origin off screen (or behind the camera) overrides every other verdict: never a mover (reason 8)
    if (cand && !onScr) { moving = false; why = 8u; }
    // r4: this draw may represent its id this frame: tagged, allowed (not a follower / known spinner), a trusted plausible
    // pairing and not spinning now. CSResolve takes the biggest (index count) of them per id. r10: origin on screen.
    if (trusted && plaus) Scratch.Store(64u + i * 4u, bi);
    if (canRep && onScr && trusted && plaus && !spinning) {
        // r6: bit 31 = the CPU's anchor (preferred: the CPU judges members against the anchor's R, so the GPU's R must be
        // the same draw's whenever it can be -- r4 took the biggest, and a stopped car that had joined a moving one could
        // become the representative and veto the moving one every frame); index count clamped to 19 bits
        uint key = ((min(t.w, 0x7FFFEu) + 1u) << 11) | (i & 0x7FFu) | ((t.z & 0x4000u) != 0u ? 0x80000000u : 0u);
        uint o0;
        Scratch.InterlockedMax(tag * 4u, key, o0);
    }
    // v0.9.0 r10: a TAGGED draw (an id member) whose origin is off screen is reported even when it is no candidate
    // (reason 8, bit 9 = not a candidate): its latest sighting has no screen position, the CPU releases it
    if (!cand && !onScr && tag != 0u) {
        float4 ao0 = idr < nc ? LoadCol3(Prev, PrevList[t.y + idr] * 64u) : float4(0.0, 0.0, 0.0, 0.0);
        PutRej(i, 8u | 0x200u | (idr < nc ? 0x100u : 0u), dev, w0, c0, float4(Mp[0][3], Mp[1][3], Mp[2][3], Mp[3][3]),
               ao0, ViewOrigin(Mc));
        return;
    }
    if (!cand) return;

    float4x4 ic;
    bool inv = Inv4(Mc, ic);
    float4x4 R = mul(Mp, ic);
    bool rok = inv && plaus && trusted &&
               all(isfinite(R[0])) && all(isfinite(R[1])) && all(isfinite(R[2])) && all(isfinite(R[3]));
    if (!rok) R = float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    // v0.9.0 r9 CAMERA-NOISE GUARD (round 7: road / verges / trees tagged; harness S13: with the camera R off by a
    // small ROTATION -- the solve's float error -- ~20 of 200 static draws per frame deviate > 0.5 px with R = the true
    // camera motion, and mv_objects_hits coherent readbacks did not keep them out: sub-2-px deviations are always
    // "coherent"). Column 2 of an R is where it maps clip (0, 0, 1, 0) = the CAMERA ORIGIN: a rotation about the camera
    // leaves it put, any motion of the draw relative to the world (translation, or rotation about a point away from the
    // camera) moves it by (relative motion) x focal / near. devT = the screen deviation at the origin that the column-2
    // difference between this draw's R and the camera R explains (linearized, at the same probe points as dev: clip z
    // there is the near term). A trusted candidate whose deviation it does not explain (devT < half the moving
    // threshold) moves like the world: the camera R is off, not the draw -- a reject (reason 7, camera noise), never a
    // mover. A translating mover's deviation IS its devT; a part rotating about its own origin has devT ~ angle x its
    // distance (far more).
    if (moving && rok) {
        float4 d2 = float4(R[0][2] - Rc[0][2], R[1][2] - Rc[1][2], R[2][2] - Rc[2][2], R[3][2] - Rc[3][2]);
        float  devT = 0.0;
        [unroll] for (uint q2 = 0; q2 < 5; ++q2) {
            if (!pok[q2]) continue;
            float4 cq = pcc[q2];
            devT = max(devT, length((d2.xy - (cq.xy / cq.w) * d2.w) * (cq.z / cq.w) * 0.5 * Img.xy));
        }
        if (!(devT >= 0.5 * Params.x)) { moving = false; why = 7u; }
    }
    if (trusted && nearO && (own || moving)) {                        // r7 stats: near-locked / near-accepted draws
        uint o1;                                                      // (r9: after the camera-noise guard)
        Moving.InterlockedAdd(TailAddr() + (own ? 8u : 12u), 1u, o1);
    }
    if (moving && rok) {
        uint slot;
        Moving.InterlockedAdd(0, 1u, slot);
        if (slot < Counts.y) {
            float4 a0 = float4(Mp[0][3], Mp[1][3], Mp[2][3], Mp[3][3]);   // the partner's origin, previous clip
            uint a = 16u + slot * 128u;
            Moving.Store4(a,       asuint(float4((float)i, dev, w0, (float)t.w)));
            Moving.Store4(a + 16u, asuint(c0));
            Moving.Store4(a + 32u, asuint(R[0]));
            Moving.Store4(a + 48u, asuint(R[1]));
            Moving.Store4(a + 64u, asuint(R[2]));
            Moving.Store4(a + 80u, asuint(R[3]));
            Moving.Store4(a + 96u, asuint(float4(a0.x - p0.x, a0.y - p0.y, a0.w - p0.w, disp)));   // r2: per-frame motion
            // r4: spin; r10: the view-space origin (z = -1e30: not recoverable)
            float4 vo = ViewOrigin(Mc);
            Moving.Store4(a + 112u, asuint(float4(spin, vo.x, vo.y, vo.w > 0.5 ? vo.z : -1.0e30)));
        }
    } else if (cand && why != 0u) {                                   // r2: filtered -> reject list (stats / debug view)
        // r3: + this origin and two previous origins of the key (the nearest candidate, the identity's own previous
        // occurrence) for the CPU follower test: a rejected wheel whose hub follows a tagged vehicle joins its id;
        // r10: + the view-space origin (PutRej)
        bool ownP = idr < nc;
        float4 ao = ownP ? LoadCol3(Prev, PrevList[t.y + idr] * 64u) : float4(0.0, 0.0, 0.0, 0.0);
        PutRej(i, why | (ownP ? 0x100u : 0u), dev, w0, c0, float4(Mp[0][3], Mp[1][3], Mp[2][3], Mp[3][3]), ao,
               ViewOrigin(Mc));
    }
}

// v0.9.0 r6: the source is split into two C++ raw strings here (MSVC caps one literal at 16380 bytes; r9: three): )" R"(
// r4 (second dispatch, after CSMain): per id s, the R of its representative = the biggest tagged draw that may represent
// it and has a trusted, plausible, non-spinning pairing THIS frame (r2 / r3: the CPU's biggest tagged draw; when that one's
// pairing failed this frame -- e.g. two identical trucks swapped their draw order -- the whole id fell back to the camera
// R for the frame). No such draw = the id keeps the camera R this frame (state -2).
// SlotR[64 + s] = (usable, state, value, draw): state 0 = usable, -1 = not an id this frame, -2 = no trusted member,
// -3 = vetoed by CSVeto (value = the contradiction in px), -4 = R not finite. r7: state 0 value = HOLD COUNT (0 = this
// frame's own R; n = the R of n frames ago, kept).
// v0.9.0 r7 HOLD (round-5 in-game log + video, driving: 4..27 tag losses per second, a passing truck's wheel blinking
// magenta / orange frame by frame): every frame in which an id had no trusted representative (a second instance of its mesh
// appeared: dup / pairing / ambiguous) put the WHOLE vehicle on the camera R for that frame -- off by the vehicle's whole
// relative motion, the pink / orange blink and the DLSS smudge. The id's R of the last frame is wrong only by the
// acceleration over one frame (sub-pixel): an id that is in the valid mask but has no usable R of its own this frame KEEPS
// its previous R for up to Ids.w (mv_objects_hold) frames in a row. SlotR is never cleared between frames, so the
// previous rows / state are still there; they are copied to [80 + s*4 ..] / [144 + s] first for CSHold (the hold of a
// vetoed id, after this dispatch has written the fresh rows). Ids.w = 0 = the r6 motion vectors. The CPU's ObjPrepare
// passes 0 for a frame whose previous frame had no object dispatch (ObjForget: Invalidate, reset, a skipped frame).
// v0.9.0 r8 MASK HOLD (round-6 video: every tagged pixel vanished for exactly one frame, 5 times in 38 s): on a PAIRING
// FAILURE frame (the record is unusable, or it pairs < 50 % of the last good frame's draws) the ids of the last good
// frame's mask stay in Ids.x (ObjPrepare: union with this frame's own mask). They have no representative this frame
// (key 0), so this dispatch gives them the r7 hold above -- last frame's rows, hold count + 1, the same mv_objects_hold
// budget. A record-less held frame (unusable record: nothing to gather) runs ONLY this dispatch, with Counts.x = 0 and
// the scratch cleared (every key 0).
[numthreads(16, 1, 1)]
void CSResolve(uint3 tid : SV_DispatchThreadID) {
    uint s = tid.x;
    if (s == 0u || s >= 16u) return;
    float4 pst = SlotR[64u + s];                                      // r7: last frame's final state of id s
    [unroll] for (uint r = 0u; r < 4u; ++r) SlotR[80u + s * 4u + r] = SlotR[s * 4u + r];
    SlotR[144u + s] = float4(pst.xyz, 0.0);                           // .w = 1 below: a fresh representative
    bool  prevOk  = pst.x > 0.5 && pst.y == 0.0;
    bool  canHold = Ids.w != 0u && prevOk && pst.z < (float)Ids.w;
    uint o0;
    if (((Ids.x >> s) & 1u) == 0u) { SlotR[64u + s] = float4(0.0, -1.0, 0.0, -1.0); return; }   // r7: count cleared
    uint key = Scratch.Load(s * 4u);
    uint i  = key & 0x7FFu;
    uint bi = key != 0u ? Scratch.Load(64u + i * 4u) : 0xFFFFFFFFu;
    float4x4 R = float4x4(0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0);
    float fail = 0.0;                                                 // 0 = fresh R, else the r4 state (-2 / -4)
    if (key == 0u || i >= Counts.x || bi == 0xFFFFFFFFu) {
        fail = -2.0;
        Moving.InterlockedAdd(12, 1u, o0);
    } else {
        float4x4 Mc = LoadM(Cur, i * 64u);
        float4x4 Mp = LoadM(Prev, bi * 64u);
        float4x4 ic;
        bool inv = Inv4(Mc, ic);
        R = mul(Mp, ic);
        if (!inv || !all(isfinite(R[0])) || !all(isfinite(R[1])) || !all(isfinite(R[2])) || !all(isfinite(R[3]))) fail = -4.0;
    }
    if (fail != 0.0) {
        if (canHold) {                                                // r7: the rows keep last frame's R
            SlotR[64u + s] = float4(1.0, 0.0, pst.z + 1.0, -1.0);
            Moving.InterlockedOr(TailAddr(), 1u << s, o0);
        } else {
            SlotR[64u + s] = float4(0.0, fail, 0.0, fail == -4.0 ? (float)i : -1.0);
            if (Ids.w != 0u && prevOk) Moving.InterlockedOr(TailAddr() + 4u, 1u << s, o0);   // r7: the hold ran out
        }
        return;
    }
    SlotR[s * 4u]      = R[0];
    SlotR[s * 4u + 1u] = R[1];
    SlotR[s * 4u + 2u] = R[2];
    SlotR[s * 4u + 3u] = R[3];
    SlotR[64u + s] = float4(1.0, 0.0, 0.0, (float)i);
    SlotR[144u + s] = float4(pst.xyz, 1.0);
}

// r4 (third dispatch): the same-frame VETO. Every tagged draw of a usable id must sit where the id's motion says it was:
// R_s * its origin (current clip) has to land within max(Img.w px, 0.35 x the id's correction at that point) of a previous
// (r6: max(4 px, 0.5 x) -- gross errors only: every veto flips the whole vehicle to the camera R for a frame, a wobble
// of its own; a far vehicle's correction is a few px and must basically never be vetoed)
// draw of its geometry key (any of the <= 8 candidates: a wheel among identical wheels finds its own). A draw that does not
// -- the ids are 2-3 frames old, so after two identical vehicles swapped their draw order the identity of one names the
// other until the CPU catches up -- would get a wrong motion vector: the whole id takes the camera R for this frame
// instead. No previous draw of the key (just appeared) = cannot judge, no veto. The relative part tolerates articulation
// (a trailer turning slightly against its tractor) and vehicles of nearly the same velocity.
// v0.9.0 r7: CSVeto no longer writes SlotR. Each contradicting draw ORs its verdict into the Moving tail (per id:
// bit 0 = vetoed, bit 1 = the camera R puts this draw nearer to a previous draw of its key than the id's R does) and
// CSHold (4th dispatch) applies it once per id: the same r7 hold as CSResolve's (the R of last frame, hold count + 1)
// unless the camera R fits the contradicting draw better (an identity swap of two identical vehicles driving in
// opposite directions: there the id's R is the OTHER vehicle's motion, worse than the camera's -- r4's reason for the
// veto; a stopped car in a moving car's id) or the hold ran out; else state -3 + camera R as in r4..r6. All draws now
// test against the same R (r4..r6: the first veto's write made the later threads of that id skip, so the veto count /
// records depended on thread order).
[numthreads(64, 1, 1)]
void CSVeto(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    if (i >= Counts.x) return;
    uint4 t = Table[i];
    uint tag = (t.z >> 8) & 15u;
    if (tag == 0u || ((Ids.x >> tag) & 1u) == 0u) return;
    if (!(SlotR[64u + tag].x > 0.5)) return;
    if (CamOrigin(LoadM(Cur, i * 64u))) return;                       // r11: camera-attached: never judged
    uint nc = min(t.z & 15u, 8u);
    if (t.y == 0xFFFFFFFFu || nc == 0u) return;
    float4 c0 = LoadCol3(Cur, i * 64u);
    float4x4 Rs = float4x4(SlotR[tag * 4u], SlotR[tag * 4u + 1u], SlotR[tag * 4u + 2u], SlotR[tag * 4u + 3u]);
    float4x4 Rc = float4x4(Solve[0], Solve[1], Solve[2], Solve[3]);
    float4 ps = mul(Rs, c0), pc = mul(Rc, c0);
    if (!(ps.w > 1e-4) || !(pc.w > 1e-4)) return;
    float2 sp = ps.xy / ps.w, cp = pc.xy / pc.w;
    float best = 3.0e38, bestCam = 3.0e38;
    float4 ab = float4(0.0, 0.0, 0.0, 0.0);
    [loop] for (uint k = 0u; k < nc; ++k) {
        float4 a0 = LoadCol3(Prev, PrevList[t.y + k] * 64u);
        if (a0.w > 1e-4) {
            float dd = length((a0.xy / a0.w - sp) * 0.5 * Img.xy);
            if (dd < best) { best = dd; ab = a0; }
            bestCam = min(bestCam, length((a0.xy / a0.w - cp) * 0.5 * Img.xy));   // r7: the same under the camera R
        }
    }
    if (!(best < 1.0e30)) return;
    float corr = length((sp - cp) * 0.5 * Img.xy);
    if (best > max(Img.w, asfloat(Ids.y) * corr)) {                   // r6: Ids.y (r4: 0.35)
        uint o0;
        Moving.InterlockedAdd(8, 1u, o0);
        uint ta = TailAddr();                                         // r7: the per-id verdict for CSHold
        Moving.InterlockedOr(ta + 16u + tag * 4u, best <= bestCam ? 1u : 3u, o0);
        Moving.InterlockedMax(ta + 80u + tag * 4u, asuint(best), o0); // best >= 0: uint order = float order
        Moving.InterlockedMax(ta + 144u + tag * 4u, i, o0);
        // r6: a VETO record in the reject list (reason 6): the CPU takes a member that contradicts its id while not moving
        // itself (a stopped car that had joined a moving one) out of the id -- else this veto would repeat every frame
        PutRej(i, 6u, best, abs(c0.w), c0, ab, float4(0.0, 0.0, 0.0, 0.0), float4(0.0, 0.0, 0.0, 0.0));   // r10: 80 B
    }
}

// v0.9.0 r7 (fourth dispatch, 16 threads): applies CSVeto's verdict per id (see above). SlotR[144 + s] = the id's state
// at the start of this frame (.w = 1: CSResolve gave it a fresh R this frame, so [80 + s*4 ..] = last frame's R).
[numthreads(16, 1, 1)]
void CSHold(uint3 tid : SV_DispatchThreadID) {
    uint s = tid.x;
    if (s == 0u || s >= 16u) return;
    uint ta = TailAddr();
    uint vf = Moving.Load(ta + 16u + s * 4u);
    if (vf == 0u) return;
    float best = asfloat(Moving.Load(ta + 80u + s * 4u));
    float di   = (float)Moving.Load(ta + 144u + s * 4u);
    float4 bk = SlotR[144u + s];
    float4 st = SlotR[64u + s];
    bool want  = Ids.w != 0u && (vf & 2u) == 0u;                      // a hold is allowed for this veto
    bool prevOk = bk.x > 0.5 && bk.y == 0.0;
    uint o0;
    if (want && bk.w > 0.5 && prevOk && bk.z < (float)Ids.w) {        // fresh R vetoed: hold last frame's R
        [unroll] for (uint r = 0u; r < 4u; ++r) SlotR[s * 4u + r] = SlotR[80u + s * 4u + r];
        SlotR[64u + s] = float4(1.0, 0.0, bk.z + 1.0, di);
        Moving.InterlockedOr(ta, 1u << s, o0);
        return;
    }
    if (want && bk.w < 0.5 && st.x > 0.5) return;                     // already a CSResolve hold: it stays
    SlotR[64u + s] = float4(0.0, -3.0, best, di);
    Moving.InterlockedAnd(ta, ~(1u << s), o0);                        // not held after all (a CSResolve hold vetoed)
    if (want && prevOk) Moving.InterlockedOr(ta + 4u, 1u << s, o0);   // the hold ran out
}
)";
// kObjShader-END

bool SameKey(const CameraMv::DrawKey& a, const CameraMv::DrawKey& b) {
    return a.ib == b.ib && a.vb == b.vb && a.ibOffset == b.ibOffset && a.vbOffset == b.vbOffset &&
           a.indexCount == b.indexCount && a.startIndex == b.startIndex && a.baseVertex == b.baseVertex;
}
// v0.9.0 r9 fallback pairing: the key without the buffer pointers and offsets (see CameraMv::SetPairFallback)
bool SameLoose(const CameraMv::DrawKey& a, const CameraMv::DrawKey& b) {
    return a.indexCount == b.indexCount && a.startIndex == b.startIndex && a.baseVertex == b.baseVertex;
}

// v0.7.8: the bytecode comes from the process-wide ShaderCache (compiled once on its worker thread); this only
// creates the device object (was a D3DCompile per instance on the render thread, ~1.9 s for both MV shaders).
bool CreateCs(ID3D11Device* dev, ShaderCache::Id id, ID3D11ComputeShader** out) {
    size_t size = 0;
    const void* code = ShaderCache::Code(id, &size);
    if (!code) {
        Log("MV: %s D3DCompile failed %s", ShaderCache::Name(id), ShaderCache::Error(id));
        return false;
    }
    const HRESULT hr = dev->CreateComputeShader(code, size, nullptr, out);
    if (FAILED(hr)) { Log("MV: %s CreateComputeShader hr=0x%lx", ShaderCache::Name(id), hr); return false; }
    return true;
}

} // namespace

bool CameraMv::s_pairFallback = true;                  // v0.9.0 r9
float CameraMv::s_movingPx = 0.5f;                     // v0.9.0 r11 (synthetic test only)

void CameraMv::RegisterShaders() {
    ShaderCache::Add(ShaderCache::kMvSolve, kSolveShader, sizeof(kSolveShader) - 1, "mv_solve", "CSMain", "cs_5_0");
    ShaderCache::Add(ShaderCache::kMvReprojDepth, kReprojDepthShader, sizeof(kReprojDepthShader) - 1,
                     "mv_reproject_depth", "CSMain", "cs_5_0");
    ShaderCache::Add(ShaderCache::kMvObjects, kObjShader, sizeof(kObjShader) - 1, "mv_objects", "CSMain", "cs_5_0");   // v0.9.0
    // v0.9.0 r4: the per-id representative and the same-frame veto (same source, other entry points)
    ShaderCache::Add(ShaderCache::kMvObjResolve, kObjShader, sizeof(kObjShader) - 1, "mv_objects_resolve", "CSResolve", "cs_5_0");
    ShaderCache::Add(ShaderCache::kMvObjVeto, kObjShader, sizeof(kObjShader) - 1, "mv_objects_veto", "CSVeto", "cs_5_0");
    // v0.9.0 r7: CSVeto's verdict per id (hold / camera R)
    ShaderCache::Add(ShaderCache::kMvObjHold, kObjShader, sizeof(kObjShader) - 1, "mv_objects_hold", "CSHold",
                     "cs_5_0");
    // v0.10.0 per-draw MVs: the replay pixel shader and the pairing compute shaders (draw_ids.cpp)
    DrawIdRecord::RegisterShaders();
    DrawIdMv::RegisterShaders();
}

bool CameraMv::Init(ID3D11Device* dev) {
    if (m_ready) return true;
    if (m_failed || !dev) return false;
    // v0.7.8: warm-up still running -> not yet (NOT failed), retried. v0.10.0 phase 17: only the two shaders created here
    // (mv_solve, mv_reproject_depth: the core group, ~2.1 s) -- the per-object / per-draw parts wait for their own (InitObjects,
    // DrawIdMv::Init); a menu / truck-preview unit never uses them.
    if (!ShaderCache::Ready(ShaderCache::kMvSolve) || !ShaderCache::Ready(ShaderCache::kMvReprojDepth)) return false;
    m_failed = true;                                   // cleared on full success
    m_dev = dev;

    D3D11_FEATURE_DATA_D3D11_OPTIONS opt{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opt, sizeof(opt))))
        Log("MV: device options ConstantBufferPartialUpdate=%d ConstantBufferOffsetting=%d "
            "(CopySubresourceRegion out of the CB ring is plain buffer->buffer, not affected)",
            (int)opt.ConstantBufferPartialUpdate, (int)opt.ConstantBufferOffsetting);

    if (!CreateCs(dev, ShaderCache::kMvSolve, &m_csSolve)) return false;
    if (!CreateCs(dev, ShaderCache::kMvReprojDepth, &m_csReproj)) return false;

    if (!m_prev.Init(dev)) { Log("MV: prev candidate record create failed"); return false; }
    HRESULT hr;

    // Solve buffer: structured float4 x kSolveElems (v0.6.0: 18), UAV (pass A) + SRV (pass B).
    D3D11_BUFFER_DESC sb{};
    sb.ByteWidth = kSolveElems * 16;
    sb.Usage = D3D11_USAGE_DEFAULT;
    sb.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    sb.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    sb.StructureByteStride = 16;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = kSolveElems;
    D3D11_SHADER_RESOURCE_VIEW_DESC ss{};
    ss.Format = DXGI_FORMAT_UNKNOWN;
    ss.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    ss.Buffer.NumElements = kSolveElems;
    if (FAILED(hr = dev->CreateBuffer(&sb, nullptr, &m_solve)) ||
        FAILED(hr = dev->CreateUnorderedAccessView(m_solve.Get(), &ud, &m_solveUav)) ||
        FAILED(hr = dev->CreateShaderResourceView(m_solve.Get(), &ss, &m_solveSrv))) {
        Log("MV: solve buffer create hr=0x%lx", hr); return false;
    }

    // Dynamic constant buffers.
    D3D11_BUFFER_DESC cb{};
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cb.ByteWidth = (34 + kSlots) * 16;                 // v0.6.0: + AllPairs[128] (2592 bytes)
    if (FAILED(hr = dev->CreateBuffer(&cb, nullptr, &m_cbPairs))) { Log("MV: pair cbuffer hr=0x%lx", hr); return false; }
    cb.ByteWidth = 80;                                 // v0.6.4: + Origin, FullSize (was 16); v0.7.8: + TileXf; v0.9.0: + ObjInfo
                                                       // v0.10.0 phase 9: + FwdMode
    if (FAILED(hr = dev->CreateBuffer(&cb, nullptr, &m_cbDims))) { Log("MV: dims cbuffer hr=0x%lx", hr); return false; }

    // Staging buffers for the one-time CB-copy check and the 600-frame R log.
    D3D11_BUFFER_DESC st{};
    st.Usage = D3D11_USAGE_STAGING;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    st.ByteWidth = 2 * kSlotBytes;
    if (FAILED(hr = dev->CreateBuffer(&st, nullptr, &m_stageDbg))) { Log("MV: staging(dbg) hr=0x%lx", hr); return false; }
    st.ByteWidth = kSolveElems * 16;
    if (FAILED(hr = dev->CreateBuffer(&st, nullptr, &m_stageSolve))) { Log("MV: staging(solve) hr=0x%lx", hr); return false; }
    for (int i = 0; i < kRing; ++i)
        if (FAILED(hr = dev->CreateBuffer(&st, nullptr, &m_ring[i]))) { Log("MV: staging(ring %d) hr=0x%lx", i, hr); return false; }

    m_ready = true; m_failed = false;
    Invalidate();
    Log("MV: ready -- candidates %d slots x %d layers, %d world / %d cabin pairs, near-reject %.1f m, medoid R per layer "
        "(CB path: GPU CopySubresourceRegion out of the VS cbuffer ring); ego reprojection origin < %.1f m, "
        "pixels < %.1f m%s", kSlots, kLayers, kWorldPairs, kCabinPairs, m_nearReject, m_egoOrigin, m_egoPixel,
        m_egoPixel > 0.0f ? "" : " (off)");
    return true;
}

bool CandidateRecord::Init(ID3D11Device* dev) {
    if (m_buf) return true;
    if (!dev) return false;
    // Raw (ByteAddress) buffer, written by CopySubresourceRegion / CopyResource.
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kBytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.FirstElement = 0;
    sd.BufferEx.NumElements = kBytes / 4;
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    HRESULT hr;
    if (FAILED(hr = dev->CreateBuffer(&bd, nullptr, &m_buf)) ||
        FAILED(hr = dev->CreateShaderResourceView(m_buf.Get(), &sd, &m_srv))) {
        Log("MV: candidate record create hr=0x%lx", (unsigned long)hr);
        m_srv.Reset(); m_buf.Reset();
        return false;
    }
    Reset();
    return true;
}

void CandidateRecord::Record(ID3D11DeviceContext* ctx, int layer, ID3D11Buffer* src,
                             UINT srcByteOffset, const DrawKey& key) {
    const int n = m_count[layer]++;
    D3D11_BOX box{ srcByteOffset, 0, 0, srcByteOffset + kSlotBytes, 1, 1 };
    ctx->CopySubresourceRegion(m_buf.Get(), 0,
                               (UINT)(layer * kSlots + n) * kSlotBytes, 0, 0, src, 0, &box);
    m_keys[layer][n] = key;
}

void CandidateRecord::CopyFrom(ID3D11DeviceContext* ctx, const CandidateRecord& o) {
    if (!m_buf || !o.m_buf) { Reset(); return; }
    m_drop = o.m_drop;                                          // v0.10.0 phase 8
    if (o.m_drop != 3u) {                                       // (both layers dropped: nothing to copy)
        GpuPerfScope gp(ctx, GpuPerf::kMedoid);
        ctx->CopyResource(m_buf.Get(), o.m_buf.Get());
    }
    for (int l = 0; l < kLayers; ++l) {
        m_count[l] = o.m_count[l];
        for (int i = 0; i < m_count[l]; ++i) m_keys[l][i] = o.m_keys[l][i];
    }
}

void CameraMv::Commit(ID3D11DeviceContext* ctx, const CandidateRecord& cur) {
    if (!m_ready) return;
    m_prev.CopyFrom(ctx, cur);
    m_prevStale = false;                                        // v0.6.1: prev is the newest record again
    ObjNoRun("a frame without MV generation (Commit)", nullptr);   // v0.9.0 r8 diagnostic
    ObjForget();                                                // v0.9.0: no object gather this frame -> no object history
    m_did.Forget();                                             // v0.10.0: nor a draw-id history
}

void CameraMv::Invalidate() {
    m_prev.Reset();
    for (int i = 0; i < kRing; ++i) m_ringPending[i] = false;   // drop in-flight R readbacks (stale history)
    m_rNew = false;
    m_solveGood = false;                                        // v0.6.1: never reuse an R from before
    m_prevStale = false;
    m_missHoldRun = 0;                                          // v0.9.0 r9: a world-miss hold never spans an Invalidate
    ObjForget();                                                // v0.9.0: object pairs need a fresh history too
    m_did.Forget();                                             // v0.10.0: per-draw pairs too
    for (bool& p : m_objRbPending) p = false;                   // ... and in-flight moving readbacks are stale
    if (m_objFeed) ObjIds::Clear();
    ++m_invalidations;
}

// v0.5.1: per-frame async R_world readback ring (see TakeSolve). Poll first (oldest to newest, stop at the
// first copy the GPU has not finished), then queue this frame's copy into a free slot.
void CameraMv::RingStep(ID3D11DeviceContext* ctx) {
    for (;;) {
        int best = -1;
        for (int i = 0; i < kRing; ++i)
            if (m_ringPending[i] && (best < 0 || m_ringSeq[i] < m_ringSeq[best])) best = i;
        if (best < 0) break;
        D3D11_MAPPED_SUBRESOURCE mp{};
        const HRESULT mhr = ctx->Map(m_ring[best].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mp);
        if (mhr == DXGI_ERROR_WAS_STILL_DRAWING) break;
        if (FAILED(mhr)) { m_ringPending[best] = false; continue; }   // never wedge the ring
        memcpy(m_rLast, mp.pData, sizeof(m_rLast));   // v0.6.0: whole solve buffer (kSolveFloats)
        ctx->Unmap(m_ring[best].Get(), 0);
        m_ringPending[best] = false;
        m_rNew = true;
        // v0.6.0: one-time line when ego pixels first become active (ego info [68..71]: cand, used, fallback, dist)
        if (!m_egoLogged && !m_quiet && m_egoPixel > 0.0f && m_rLast[69] > 0.5f) {
            m_egoLogged = true;
            Log("MV: ego reprojection active -- %d near world draws move with the camera (origin < %.1f m), "
                "pixels closer than %.1f m use R_ego", (int)m_rLast[69], m_egoOrigin, m_egoPixel);
        }
    }
    for (int i = 0; i < kRing; ++i) {
        if (m_ringPending[i] || !m_ring[i]) continue;
        ctx->CopyResource(m_ring[i].Get(), m_solve.Get());
        m_ringPending[i] = true;
        m_ringSeq[i] = ++m_ringCtr;
        break;
    }
}

// One-time proof that the GPU copy out of the game's constant-buffer ring really
// delivers data: stage slot 0 of each layer and log the 16 floats. Row 3 of a
// real MVP is a (near) pure-w row; all zeros means the copy path is dead.
void CameraMv::DebugReadback(ID3D11DeviceContext* ctx, const CandidateRecord& cur) {
    int got = 0;
    for (int l = 0; l < kLayers; ++l) {
        if (cur.Count(l) <= 0) continue;
        D3D11_BOX box{ (UINT)(l * kSlots) * kSlotBytes, 0, 0, (UINT)(l * kSlots) * kSlotBytes + kSlotBytes, 1, 1 };
        ctx->CopySubresourceRegion(m_stageDbg.Get(), 0, (UINT)l * kSlotBytes, 0, 0, cur.Buffer(), 0, &box);
        ++got;
    }
    if (!got) return;                                   // retry next frame
    m_dbgDone = true;
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(ctx->Map(m_stageDbg.Get(), 0, D3D11_MAP_READ, 0, &mp))) { Log("MV: debug readback Map failed"); return; }
    const float* f = (const float*)mp.pData;
    for (int l = 0; l < kLayers; ++l) {
        if (cur.Count(l) <= 0) { Log("MV: CB-copy check layer %d: no candidates", l); continue; }
        const float* m = f + l * 16;
        bool allZero = true;
        for (int i = 0; i < 16; ++i) if (m[i] != 0.0f) allZero = false;
        Log("MV: CB-copy check layer %d slot0 MVP rows: [%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f] "
            "[%.4f %.4f %.4f %.4f] [%.4f %.4f %.4f %.4f]%s", l,
            m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11],
            m[12], m[13], m[14], m[15],
            allZero ? "  ** ALL ZERO: partial copy from the CB ring does NOT work on this device **" : "");
    }
    ctx->Unmap(m_stageDbg.Get(), 0);
}

// Async log of the chosen R: copy at every 120th frame, read (DO_NOT_WAIT) from frame
// N+2 on. The full line is logged on 600-multiples; in between, a line is logged
// only while R_world is not ~identity (max |R - I| > 0.01), capped at 200 lines.
void CameraMv::SolveReadback(ID3D11DeviceContext* ctx) {
    if (m_solvePending == 0 && m_frame % 120 == 0) {
        ctx->CopyResource(m_stageSolve.Get(), m_solve.Get());
        m_solvePending = 3;
        m_solveFrame = m_frame;
        return;
    }
    if (m_solvePending > 0 && --m_solvePending == 0) {
        D3D11_MAPPED_SUBRESOURCE mp{};
        const HRESULT hr = ctx->Map(m_stageSolve.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mp);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { m_solvePending = 1; return; }
        if (FAILED(hr)) { Log("MV: solve readback Map hr=0x%lx", (unsigned long)hr); return; }
        const float* f = (const float*)mp.pData;
        float dev = 0.0f;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                const float d = std::fabs(f[r * 4 + c] - (r == c ? 1.0f : 0.0f));
                if (!(d <= dev)) dev = d;                       // also catches NaN
            }
        const bool periodic = (m_solveFrame % 600) == 0;
        const bool moving = dev > 0.01f;
        if (!m_quiet && (periodic || (moving && m_driveLogs < 200))) {
            if (!periodic) ++m_driveLogs;
            // v0.10.0 phase 3: Solve[8].w = the size of the per-draw consensus cluster that replaced the medoid (0 = medoid);
            // the pairs / pick / |w| / IndexCount fields always describe pass A's medoid
            char camR[48];
            if (f[35] > 0.5f) snprintf(camR, sizeof(camR), "camR=consensus (%.0f draws)", f[35]);
            else snprintf(camR, sizeof(camR), "camR=medoid");
            Log("MV: %s frame %llu R_world [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f]"
                " maxdev=%.4f %s (medoid: pairs=%.0f valid=%.0f pick=%.0f) | medoid's world pair |w|=%.1f m IndexCount=%.0f, "
                "near-rejected %.0f/%.0f valid%s | ego: cand=%.0f used=%.0f fallback=%.0f |R_ego-R_world|=%.4f",
                periodic ? "R" : "R(moving)", (unsigned long long)m_solveFrame,
                f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11],
                f[12], f[13], f[14], f[15], dev, camR, f[32], f[33], f[34],
                f[40], f[41], f[42], f[33], f[43] > 0.5f ? " (near filter bypassed: no far pair)" : "",
                f[68], f[69], f[70], f[71]);
        }
        ctx->Unmap(m_stageSolve.Get(), 0);
    }
}

bool CameraMv::ReadSolveBlocking(ID3D11DeviceContext* ctx, float out[kSolveFloats]) {
    if (!m_ready || !ctx || !m_solve) return false;
    if (!m_stageSnap) {
        D3D11_BUFFER_DESC bd{};
        m_solve->GetDesc(&bd);
        bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bd.MiscFlags = 0;
        if (FAILED(m_dev->CreateBuffer(&bd, nullptr, &m_stageSnap))) { m_stageSnap.Reset(); return false; }
    }
    ctx->CopyResource(m_stageSnap.Get(), m_solve.Get());
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(ctx->Map(m_stageSnap.Get(), 0, D3D11_MAP_READ, 0, &mp))) return false;
    memcpy(out, mp.pData, kSolveFloats * sizeof(float));   // v0.6.0: 72 (was 48)
    ctx->Unmap(m_stageSnap.Get(), 0);
    return true;
}

// v0.6.1: dims cbuffer of pass B (Size, EgoPixelM); also read by the MV debug view.
// v0.6.4: + Origin (crop origin in the full image), FullSize.
bool CameraMv::WriteDims(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, uint32_t x0, uint32_t y0,
                         uint32_t fullW, uint32_t fullH) {
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(ctx->Map(m_cbDims.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) return false;
    // v0.10.0 phase 4: w = the forward-pixel attach radius (only with per-draw MVs; 0 = off)
    const float dims[12] = { (float)w, (float)h, m_egoPixel, m_didN ? DrawIdMv::AttachM() : 0.0f,   // v0.6.0: z = EgoPixelM
                             (float)x0, (float)y0, (float)fullW, (float)fullH,      // v0.6.4
                             m_tileXf[0], m_tileXf[1], m_tileXf[2], m_tileXf[3] };  // v0.7.8 tiled preview (else 1,1,0,0)
    // v0.9.0 ObjInfo: x = valid stencil ids; r2: y = the debug view's "rejected" id (0 = none; pass B ignores it)
    // v0.10.0: z = per-draw record draws (draw ids 1..z; 0 = per-draw MVs off this frame)
    const uint32_t obj[4] = { m_objMask, m_objDbgSlot, m_didN, m_didN ? m_didNG : 0u };   // v0.10.0 phase 3: w = G-buffer ids
    // v0.10.0 phase 9 FwdMode: x = the forward depth's resolution shift (only with a forward depth), y = the forward ids are in t7
    // v0.10.0 phase 14: z = 1 when the pass's G-buffer replay ran with the static gate (mv_replay_static_*): world pixels without a
    // G-buffer id may be a static draw that was not re-drawn -- the Ctrl+F6 view shows them olive (pass B does not read z)
    const uint32_t fwdMode[4] = { m_fwdDepth ? m_fwdShift : 0u, (m_didN && m_fwdShift && m_fwdIds) ? 1u : 0u,
                                  (m_didN && m_didStaticGate) ? 1u : 0u, 0u };
    memcpy(mp.pData, dims, sizeof(dims));
    memcpy((uint8_t*)mp.pData + sizeof(dims), obj, sizeof(obj));
    memcpy((uint8_t*)mp.pData + sizeof(dims) + sizeof(obj), fwdMode, sizeof(fwdMode));
    ctx->Unmap(m_cbDims.Get(), 0);
    return true;
}

// Pass B: per-pixel reprojection through the solve buffer -> MV texture (u0) + flattened R32F depth (u1, v0.5.7).
void CameraMv::DispatchReproj(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, ID3D11ShaderResourceView* depthTwinSrv,
                              ID3D11UnorderedAccessView* depthR32Uav, ID3D11UnorderedAccessView* mvUav) {
    ID3D11ShaderResourceView* nullSrv[8] = {};
    ID3D11Buffer* cbs = m_cbDims.Get();
    // v0.9.0: t2 = the twin's stencil view, t3 = the per-id R table -- only while ids are valid (ObjInfo.x != 0)
    // v0.9.0 r5: t4 = the pass's forward depth (null = none this pass: the shader reads 0 and keeps the twin's depth)
    // v0.10.0: t5 = the draw-id target, t6 = the per-draw R table, u3 = coverage counters (only while ObjInfo.z != 0)
    // v0.10.0 phase 9: t7 = the 1/2-resolution forward ids (FwdMode.y)
    ID3D11ShaderResourceView* srvs[8] = { depthTwinSrv, m_solveSrv.Get(), m_objMask ? m_objStencil : nullptr,
                                          m_objMask ? m_objSlotSrv.Get() : nullptr, m_fwdDepth,
                                          m_didN ? m_didIdSrv : nullptr, m_didN ? m_did.RSrv() : nullptr,
                                          (m_didN && m_fwdShift) ? m_fwdIds : nullptr };
    // v0.9.0 r11: u2 = the near-range forward-depth counter (only with a forward depth and a free readback slot)
    int cntSlot = -1;
    if (m_fwdDepth && FwdCountInit())
        for (int k = 0; k < kFwdCntRb; ++k) if (!m_fwdCntPending[k]) { cntSlot = k; break; }
    if (cntSlot >= 0) {
        const UINT zero4[4] = { 0, 0, 0, 0 };
        ctx->ClearUnorderedAccessViewUint(m_fwdCntUav.Get(), zero4);
    }
    ID3D11UnorderedAccessView* uavs[4] = { mvUav, depthR32Uav, cntSlot >= 0 ? m_fwdCntUav.Get() : nullptr,
                                           m_didN ? m_did.CntUav() : nullptr };
    ID3D11UnorderedAccessView* nullUavs[4] = {};
    const UINT keep4[4] = { (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1 };
    const int tq = m_didN ? m_did.PassBTimer().Begin(ctx) : -1;   // v0.10.0: pass B GPU time in mode 2
    const int pq = GpuPerf::Begin(ctx, GpuPerf::kPassB);          // v0.10.0 phase 8
    ctx->CSSetShader(m_csReproj.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &cbs);
    ctx->CSSetShaderResources(0, 8, srvs);
    ctx->CSSetUnorderedAccessViews(0, 4, uavs, keep4);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    // Unbind t0..t7/u0/u1 (r11: u2, v0.10.0: u3): NGX reads the R32F depth and the MV texture as SRVs next.
    ctx->CSSetShaderResources(0, 8, nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 4, nullUavs, keep4);
    GpuPerf::End(ctx, pq);
    m_did.PassBTimer().End(ctx, tq);
    if (cntSlot >= 0) {                                 // v0.9.0 r11: read back without waiting (FwdCountPoll)
        ctx->CopyResource(m_fwdCntRb[cntSlot].Get(), m_fwdCnt.Get());
        m_fwdCntPending[cntSlot] = true;
    }
}

// v0.9.0 r11: the near-range forward-depth counter of pass B (64 striped dwords) + its staging ring (lazily; a failure
// only turns the statistic off)
bool CameraMv::FwdCountInit() {
    if (m_fwdCnt) return true;
    if (m_fwdCntFailed || !m_dev) return false;
    m_fwdCntFailed = true;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kFwdCntBytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = kFwdCntBytes / 4;
    ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    HRESULT hr = m_dev->CreateBuffer(&bd, nullptr, &m_fwdCnt);
    if (SUCCEEDED(hr)) hr = m_dev->CreateUnorderedAccessView(m_fwdCnt.Get(), &ud, &m_fwdCntUav);
    D3D11_BUFFER_DESC st{};
    st.ByteWidth = kFwdCntBytes;
    st.Usage = D3D11_USAGE_STAGING;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int k = 0; k < kFwdCntRb && SUCCEEDED(hr); ++k) hr = m_dev->CreateBuffer(&st, nullptr, &m_fwdCntRb[k]);
    if (FAILED(hr)) {
        Log("MV forward depth: near-range pixel counter unavailable (hr=0x%lx) -- the statistic stays 0",
            (unsigned long)hr);
        m_fwdCntUav.Reset(); m_fwdCnt.Reset();
        for (int k = 0; k < kFwdCntRb; ++k) m_fwdCntRb[k].Reset();
        return false;
    }
    m_fwdCntFailed = false;
    return true;
}

void CameraMv::FwdCountPoll(ID3D11DeviceContext* ctx) {
    for (int k = 0; k < kFwdCntRb; ++k) {
        if (!m_fwdCntPending[k]) continue;
        D3D11_MAPPED_SUBRESOURCE mp{};
        const HRESULT hr = ctx->Map(m_fwdCntRb[k].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mp);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        m_fwdCntPending[k] = false;
        if (FAILED(hr)) continue;
        const uint32_t* c = (const uint32_t*)mp.pData;
        uint64_t sum = 0;
        for (UINT q = 0; q < kFwdCntBytes / 4; ++q) sum += c[q];
        ctx->Unmap(m_fwdCntRb[k].Get(), 0);
        m_fwdNearPx += sum * 16u;                      // 1-in-16 grid
        ++m_fwdNearFrames;
    }
}

void CameraMv::LogFrameLine(const FrameStats& fs, const CandidateRecord& cur) {
    if (m_quiet || m_frame % 600 != 0) return;           // v0.10.0 phase 2: quiet units (mirrors) report elsewhere
    const double g = (double)m_genFrames;
    // v0.9.0 r9: + world-miss frames held (the last good R, no history reset) and pairs found through the loose key
    Log("MV: frame %llu pairs this frame world=%d cabin=%d%s | avg world=%.2f cabin=%.2f | "
        "world-miss frames %llu/%llu bad=%llu stale-reuse=%llu (candidates cur: world=%d cabin=%d) | r9: world-miss "
        "held=%llu (no reset), pairs by fallback key=%llu",
        (unsigned long long)m_frame, fs.pairs[0], fs.pairs[1],
        fs.reused ? " (R reused)" : (fs.missHeld ? " (R held)" : ""),
        g > 0 ? (double)m_pairSum[0] / g : 0.0, g > 0 ? (double)m_pairSum[1] / g : 0.0,
        (unsigned long long)m_missFrames, (unsigned long long)m_genFrames,
        (unsigned long long)m_badFrames, (unsigned long long)m_staleFrames,
        cur.Count(0), cur.Count(1), (unsigned long long)m_missHeld, (unsigned long long)m_candFallback);
}

// v0.5.7: pass B also flattens the depth twin into depthR32Uav. Contract: returns true <=> pass B was
// dispatched (MV texture AND R32F depth written); every `return false` below happens BEFORE any dispatch, so
// on false neither texture was touched and the caller must produce the R32F depth itself (convert pass).
bool CameraMv::Generate(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, uint32_t x0, uint32_t y0,
                        uint32_t fullW, uint32_t fullH, const CandidateRecord& cur,
                        ID3D11ShaderResourceView* depthTwinSrv, ID3D11UnorderedAccessView* depthR32Uav,
                        ID3D11UnorderedAccessView* mvUav, FrameStats* stats, bool candBad,
                        const ObjRecord* objs, ID3D11ShaderResourceView* stencilSrv,
                        ID3D11ShaderResourceView* fwdDepthSrv, const DrawIdRecord* did,
                        ID3D11ShaderResourceView* didIdSrv, const DrawIdRecord* didFwd, uint32_t fwdShift,
                        ID3D11ShaderResourceView* fwdIdSrv) {
    m_objMask = 0;
    m_objDbgSlot = 0;                                  // v0.9.0 r2
    m_fwdDepth = fwdDepthSrv;                          // v0.9.0 r5 (pass B t4; null = twin only)
    m_fwdShift = fwdDepthSrv && fwdShift ? 1u : 0u;    // v0.10.0 phase 9
    m_fwdIds = m_fwdShift ? fwdIdSrv : nullptr;
    m_didN = 0;                                        // v0.10.0 (set by DidPrepare when pass B can use the ids)
    m_didNG = 0;
    m_didIdSrv = nullptr;
    if (ctx && m_fwdCnt) FwdCountPoll(ctx);            // v0.9.0 r11: near-range forward-depth counts that landed
    if (!m_ready || !ctx || !depthTwinSrv || !depthR32Uav || !mvUav || !cur.Ready()) {
        ObjNoRun(!cur.Ready() ? "Generate early-out: the candidate record is not ready"
                              : "Generate early-out: not ready / no depth twin / no target", objs);   // v0.9.0 r8
        ObjForget();
        m_did.Forget();                                // v0.10.0
        return false;
    }
    ++m_frame;
    // v0.9.0 per-object MVs: only with both the pass's object record and the twin's stencil view
    if (!stencilSrv) objs = nullptr;
    if (did) objs = nullptr;                           // v0.10.0: the two object paths never run together
    m_objStencil = stencilSrv;
    const CandidateRecord& prev = m_prev;

    if (!m_loggedFirst && !m_quiet && (cur.Count(0) + cur.Count(1)) > 0) {
        m_loggedFirst = true;
        Log("MV: first candidates collected -- world=%d cabin=%d (cap %d per layer)",
            cur.Count(0), cur.Count(1), kSlots);
    }
    if (!m_dbgDone && m_frame >= 5) DebugReadback(ctx, cur);

    // ---- v0.6.1 reuse path: bad record, or the first good one after it (prev is then >= 2 frames old) -----
    // The solve buffer still holds R of the last good frame (pass A has not run since). Pass B reprojects this
    // frame's depth with it. No matching, no pass A, no async readbacks (the solve buffer did not change).
    // bad: cur is NOT committed (prev stays the last good record, now stale). first good after bad: committed.
    if ((candBad || m_prevStale) && m_solveGood) {
        FrameStats fs;
        fs.worldMiss = false;
        fs.reused = true;
        if (stats) *stats = fs;
        m_last = fs;
        // v0.9.0 (sets m_objMask for WriteDims; 0 = off). R_world = the last good frame's camera motion, not this frame's:
        // r2: no "moving" verdicts from it (camOk false -- while the camera turns every static draw would deviate); the ids'
        // own R (MVP_prev * inverse(MVP_cur)) does not depend on it and is still computed.
        ObjPrepare(ctx, objs, fullW, fullH, false);
        // v0.10.0: per-draw pairs need no camera solve of this frame (R = MVP_prev * inverse(MVP_cur)); the camera R only
        // predicts duplicates' partners and decides "static", so the last good one is fine here.
        const uint32_t didPrep = DidPrepare(ctx, did, didIdSrv, fullW, fullH, didFwd, false);   // no fresh medoid
        if (!WriteDims(ctx, w, h, x0, y0, fullW, fullH)) {   // nothing dispatched: same contract as below
            ObjNoRun("Generate (reuse path): the dims cbuffer could not be mapped", objs);   // v0.9.0 r8
            if (candBad) m_prevStale = true; else Commit(ctx, cur);
            ObjForget();
            m_did.Forget(); m_didN = 0;
            return false;
        }
        ObjDispatch(ctx, objs);                        // v0.9.0: the solve buffer still holds the last good R_world
        // v0.10.0 (phase 3: + the forward draws; the consensus of THIS frame's per-draw pairs replaces the held camera R)
        // v0.10.0 phase 6: + the draw-id target, depth twin and forward depth (pass B's inputs) for the rigid-parent vote
        if (didPrep) m_did.Dispatch(ctx, did, m_solveSrv.Get(), didFwd, m_solveUav.Get(), m_didN ? m_didIdSrv : nullptr,
                                    depthTwinSrv, m_fwdDepth, m_fwdIds);
        DispatchReproj(ctx, w, h, depthTwinSrv, depthR32Uav, mvUav);
        ObjFinish(ctx, objs);
        if (didPrep) m_did.Finish(ctx);                // v0.10.0: this pass becomes the eye's draw-id history
        if (candBad) { ++m_badFrames; m_prevStale = true; }
        else         { ++m_staleFrames; m_prev.CopyFrom(ctx, cur); m_prevStale = false; }   // = Commit() minus its v0.9.0 ObjForget
        LogFrameLine(fs, cur);
        return true;
    }

    // ---- match prev <-> cur per layer -----------------------------------------
    struct Pair { int cur, prev; bool unique; UINT ic; };
    uint32_t pairData[2 * kPairs][4] = {};
    uint32_t counts[4] = {};
    uint32_t allPairs[kSlots][4] = {};                 // v0.6.0: every matched world pair (ego scan in pass A)
    FrameStats fs;
    // v0.10.0 phase 8 (mv_medoid_drop): a layer whose candidates were not collected in this or the previous record has NO
    // medoid this frame: no matching, pass A leaves it alone, the per-draw consensus (DrawIdMv CSPick) writes it -- or the solve
    // buffer keeps the last frame's R of that layer when the consensus fails (the failure re-arms the candidates)
    const uint32_t noMed = (uint32_t)((cur.DropBits() | prev.DropBits()) & 3u);
    for (int l = 0; l < kLayers; ++l) {
        if ((noMed >> l) & 1u) { counts[l] = 0; fs.pairs[l] = 0; continue; }
        Pair found[kSlots];
        int nf = 0, nfb = 0;                           // v0.9.0 r9: nfb = of them through the fallback key
        const int nc = cur.Count(l), np = prev.Count(l);
        for (int i = 0; i < nc; ++i) {
            const DrawKey& k = cur.Key(l, i);
            int occ = 0, totC = 0, totP = 0;
            for (int j = 0; j < nc; ++j) if (SameKey(cur.Key(l, j), k)) { if (j < i) ++occ; ++totC; }
            int pi = -1, seen = 0;
            for (int j = 0; j < np; ++j) {
                if (!SameKey(prev.Key(l, j), k)) continue;
                ++totP;
                if (pi < 0 && seen++ == occ) pi = j;
            }
            if (pi >= 0) { found[nf++] = { i, pi, totC == 1 && totP == 1, k.indexCount }; continue; }
            // v0.9.0 r9 FALLBACK (round-7 log: 21 of 7200 frames paired NOTHING, each one a history reset; an object
            // record with the same 530 draws as the previous one paired 27 %: most keys changed at once while the
            // content did not -- the buffer pointers): no previous draw of the full key -> the loose key (indexCount,
            // startIndex, baseVertex), only when it is drawn exactly once in both records (its full-key partner would
            // share it, so this draw cannot steal one). Not "unique": the unambiguous full-key pairs are preferred for
            // the medoid.
            if (!s_pairFallback || totP) continue;
            int lc = 0, lp = 0, pj = -1;
            for (int j = 0; j < nc && lc < 2; ++j) if (SameLoose(cur.Key(l, j), k)) ++lc;
            if (lc != 1) continue;
            for (int j = 0; j < np && lp < 2; ++j) if (SameLoose(prev.Key(l, j), k)) { ++lp; pj = j; }
            if (lp != 1) continue;
            found[nf++] = { i, pj, false, k.indexCount };
            ++nfb;
        }
        // v0.9.0 r9 unpaired-draw diagnostic (first 5): a frame whose world candidates paired fewer than half through
        // the FULL key -- what changed between the two records (pointers? offsets? counts?)
        if (l == 0 && nc > 0 && (nf - nfb) * 2 < nc && m_unpairedLogs < 5 && !m_quiet) {
            ++m_unpairedLogs;
            char s[1400];
            int o = snprintf(s, sizeof(s), "MV: unpaired-draw sample #%d at frame %llu (%s): world candidates cur %d "
                             "prev %d, paired %d by the full key + %d by the fallback key (%s) |", m_unpairedLogs,
                             (unsigned long long)m_frame,
                             nf - nfb == 0 ? "world-miss by the full key" : "full-key pairing < 50%",
                             nc, np, nf - nfb, nfb, s_pairFallback ? "on" : "off");
            int shown = 0;
            for (int i = 0; i < nc && shown < 6 && o > 0 && o < (int)sizeof(s) - 200; ++i) {
                const DrawKey& k = cur.Key(l, i);
                bool full = false;
                for (int j = 0; j < np && !full; ++j) full = SameKey(prev.Key(l, j), k);
                if (full) continue;
                int lp = 0, pj = -1;
                for (int j = 0; j < np; ++j) if (SameLoose(prev.Key(l, j), k)) { if (pj < 0) pj = j; ++lp; }
                o += snprintf(s + o, sizeof(s) - (size_t)o, " [ib %p vb %p ibOff %u vbOff %u ic %u si %u bv %d -> prev "
                              "same loose key: ", k.ib, k.vb, k.ibOffset, k.vbOffset, k.indexCount, k.startIndex,
                              k.baseVertex);
                if (pj >= 0) {
                    const DrawKey& p = prev.Key(l, pj);
                    o += snprintf(s + o, sizeof(s) - (size_t)o, "%d, first ib %p vb %p ibOff %u vbOff %u]", lp, p.ib,
                                  p.vb, p.ibOffset, p.vbOffset);
                } else {
                    o += snprintf(s + o, sizeof(s) - (size_t)o, "none]");
                }
                ++shown;
            }
            Log("%s", s);
        }
        m_candFallback += (uint64_t)nfb;
        if (l == 0) {
            // v0.6.0: hand ALL matched world pairs to pass A (record order); it scans them for ego candidates.
            for (int a = 0; a < nf; ++a) {
                allPairs[a][0] = (uint32_t)((l * kSlots + found[a].prev) * kSlotBytes);
                allPairs[a][1] = (uint32_t)((l * kSlots + found[a].cur) * kSlotBytes);
                allPairs[a][2] = (uint32_t)found[a].ic;
            }
            counts[2] = (uint32_t)nf;
        }
        const int budget = (l == 0) ? kWorldPairs : kCabinPairs;
        Pair chosen[kPairs];
        int use = 0;
        if (l == 0) {
            // World: unambiguous keys first; take EVENLY SPACED matches over the record
            // order (not the first N) so one kind of early geometry cannot dominate.
            Pair uniq[kSlots], rest[kSlots];
            int nu = 0, nr = 0;
            for (int a = 0; a < nf; ++a) { if (found[a].unique) uniq[nu++] = found[a]; else rest[nr++] = found[a]; }
            if (nu >= budget) {
                for (int j = 0; j < budget; ++j) chosen[use++] = uniq[(int)(((long long)j * nu) / budget)];
            } else {
                for (int a = 0; a < nu; ++a) chosen[use++] = uniq[a];
                const int need = budget - nu;
                if (nr <= need) { for (int a = 0; a < nr; ++a) chosen[use++] = rest[a]; }
                else for (int j = 0; j < need; ++j) chosen[use++] = rest[(int)(((long long)j * nr) / need)];
            }
        } else {
            // Cabin (unchanged): unambiguous keys, then bigger meshes first.
            for (int a = 0; a < nf && a < budget; ++a) {
                int b = a;
                for (int c = a + 1; c < nf; ++c) {
                    const bool better = (found[c].unique != found[b].unique) ? found[c].unique
                                                                             : found[c].ic > found[b].ic;
                    if (better) b = c;
                }
                if (b != a) { Pair t = found[a]; found[a] = found[b]; found[b] = t; }
            }
            use = nf < budget ? nf : budget;
            for (int k = 0; k < use; ++k) chosen[k] = found[k];
        }
        for (int k = 0; k < use; ++k) {
            pairData[l * kPairs + k][0] = (uint32_t)((l * kSlots + chosen[k].prev) * kSlotBytes);
            pairData[l * kPairs + k][1] = (uint32_t)((l * kSlots + chosen[k].cur) * kSlotBytes);
            pairData[l * kPairs + k][2] = (uint32_t)chosen[k].ic;
        }
        counts[l] = (uint32_t)use;
        fs.pairs[l] = use;
    }
    // v0.10.0 phase 8: without a world medoid the per-draw pairing decides whether this frame can have a camera R at all (a
    // previous pass with at least one partner group), not the (dropped) candidates
    uint32_t didPrepEarly = 0;
    if (noMed & 1u) {
        didPrepEarly = DidPrepare(ctx, did, didIdSrv, fullW, fullH, didFwd, false, noMed);
        fs.worldMiss = !(didPrepEarly && m_did.PreparedPairable());
    } else {
        fs.worldMiss = fs.pairs[0] == 0;
    }
    // v0.9.0 r9 WORLD-MISS HOLD (round-7 log: world-miss frames 21 / 7200 = one per ~340 frames, every one reset the
    // DLSS history -- the whole picture re-accumulated from scratch: an aliasing / shimmer flash on trees and fences
    // every ~5 s while driving): the camera moved by a frame's worth since the last good solve, so that R is off by the
    // change of the camera's motion over one frame (sub-pixel), not by the motion itself. A world miss answers with the
    // solve buffer as it is (pass A skipped, like the v0.6.1 reuse path) for up to mv_objects_hold frames in a row;
    // only then (or with no good solve since the last Invalidate, or mv_objects_hold 0) the caller resets the history
    // as before.
    const int missBudget = ObjIds::Hold();
    fs.missHeld = fs.worldMiss && m_solveGood && missBudget > 0 && m_missHoldRun < missBudget;
    if (fs.missHeld) { ++m_missHoldRun; ++m_missHeld; ++m_objStats.worldMissHeld; }
    else if (!fs.worldMiss) m_missHoldRun = 0;
    if (stats) *stats = fs;
    m_last = fs;
    ++m_genFrames;
    if (fs.worldMiss) ++m_missFrames;
    for (int l = 0; l < kLayers; ++l) m_pairSum[l] += (uint64_t)fs.pairs[l];
    char missWhy[96] = "";
    if (fs.missHeld)
        snprintf(missWhy, sizeof(missWhy), "camera R held, frame %d of %d in a row", m_missHoldRun, missBudget);
    else if (fs.worldMiss)
        snprintf(missWhy, sizeof(missWhy), "%s: the history is reset", !m_solveGood ? "no good camera solve to hold"
                 : (missBudget <= 0 ? "mv_objects_hold 0" : "hold budget used"));

    // ---- constant buffers --------------------------------------------------------
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(ctx->Map(m_cbPairs.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) {
        ObjNoRun("Generate: the pair cbuffer could not be mapped", objs);   // v0.9.0 r8 diagnostic
        Commit(ctx, cur); ObjForget(); return false;
    }
    memcpy(mp.pData, pairData, sizeof(pairData));
    memcpy((uint8_t*)mp.pData + sizeof(pairData), counts, sizeof(counts));
    {
        const float params[4] = { m_nearReject, m_egoOrigin, (float)noMed, 0.0f };   // v0.10.0 phase 8: z = layers without a medoid
        memcpy((uint8_t*)mp.pData + sizeof(pairData) + sizeof(counts), params, sizeof(params));
        memcpy((uint8_t*)mp.pData + sizeof(pairData) + sizeof(counts) + sizeof(params), allPairs, sizeof(allPairs));
    }
    ctx->Unmap(m_cbPairs.Get(), 0);
    // v0.9.0 (sets m_objMask for WriteDims; 0 = off). r9: a world miss is a pairing failure for the objects too (mask
    // hold)
    ObjPrepare(ctx, objs, fullW, fullH, !fs.worldMiss, fs.worldMiss, missWhy);
    // v0.10.0 per-draw MVs: CPU grouping + uploads (sets m_didN for WriteDims); never a history reset for them
    // (phase 3: + the forward record; pass A writes a fresh medoid unless the world miss is held)
    const uint32_t didPrep = (noMed & 1u) ? didPrepEarly
                                          : DidPrepare(ctx, did, didIdSrv, fullW, fullH, didFwd, !fs.missHeld, noMed);
    if (!WriteDims(ctx, w, h, x0, y0, fullW, fullH)) {
        ObjNoRun("Generate: the dims cbuffer could not be mapped", objs);   // v0.9.0 r8 diagnostic
        m_didN = 0;
        Commit(ctx, cur); ObjForget(); return false;   // (Commit forgets the draw-id history too)
    }

    const UINT keep = (UINT)-1;
    ID3D11ShaderResourceView*  nullSrv[2] = {};
    ID3D11UnorderedAccessView* nullUav = nullptr;

    // ---- pass A: per-layer medoid R (v0.9.0 r9: not on a held world miss -- the solve buffer keeps the last good R)
    // ----
    if (!fs.missHeld && noMed != 3u) {                 // v0.10.0 phase 8: not when neither layer has a medoid
        GpuPerfScope gp(ctx, GpuPerf::kMedoid);
        ID3D11Buffer* cbs = m_cbPairs.Get();
        ID3D11ShaderResourceView* srvs[2] = { prev.Srv(), cur.Srv() };
        ID3D11UnorderedAccessView* uav = m_solveUav.Get();
        ctx->CSSetShader(m_csSolve.Get(), nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, &cbs);
        ctx->CSSetShaderResources(0, 2, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, &keep);
        ctx->Dispatch(1, 1, 1);
        ctx->CSSetShaderResources(0, 2, nullSrv);
        ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &keep);
    }
    if (!fs.missHeld) m_solveGood = !fs.worldMiss;     // v0.6.1: a reusable R_world is in the solve buffer
    // ---- v0.9.0 per-object pass: gather + pair + verdict + R per stencil id (needs R_world of pass A) ------------
    ObjDispatch(ctx, objs);
    // ---- v0.10.0 per-draw pass: gather + match + R per draw (needs the camera R of pass A for the prediction) ------
    // v0.10.0 phase 3: + the consensus camera R (written into the solve buffer when a cluster is found, before the
    // static / mover verdicts and pass B)
    // v0.10.0 phase 6: + the draw-id target, depth twin and forward depth (pass B's inputs) for the rigid-parent vote
    if (didPrep) m_did.Dispatch(ctx, did, m_solveSrv.Get(), didFwd, m_solveUav.Get(), m_didN ? m_didIdSrv : nullptr,
                                depthTwinSrv, m_fwdDepth, m_fwdIds);
    // ---- pass B: per-pixel reprojection -> MV texture (u0) + flattened R32F depth (u1, v0.5.7) --------
    DispatchReproj(ctx, w, h, depthTwinSrv, depthR32Uav, mvUav);
    const int gq = GpuPerf::Begin(ctx, GpuPerf::kMvMisc);   // v0.10.0 phase 8: diagnostic / commit copies
    ObjFinish(ctx, objs);                              // v0.9.0: moving readback + object history commit
    if (didPrep) m_did.Finish(ctx);                    // v0.10.0: draw-id history commit + diagnostics readback

    SolveReadback(ctx);
    RingStep(ctx);
    GpuPerf::End(ctx, gq);
    LogFrameLine(fs, cur);
    m_prev.CopyFrom(ctx, cur);                         // = Commit() without its v0.9.0 ObjForget (committed by ObjFinish)
    m_prevStale = false;
    return true;
}

// v0.10.0: the pass's draw record -> this eye's DrawIdMv (created lazily: a failure leaves per-draw MVs off, the camera
// path is untouched). m_didN = the draw count when pass B can use the ids (the pass's draw-id target is complete); the
// record still becomes the eye's history without it. Returns the prepared draw count (0 = no dispatch / no commit).
uint32_t CameraMv::DidPrepare(ID3D11DeviceContext* ctx, const DrawIdRecord* did, ID3D11ShaderResourceView* idSrv,
                              uint32_t fullW, uint32_t fullH, const DrawIdRecord* didFwd, bool freshMedoid, uint32_t noMedoid) {
    m_didN = 0;
    m_didNG = 0;
    m_didIdSrv = nullptr;
    m_didStaticGate = false;                           // v0.10.0 phase 14
    if (!did || !ctx) { m_did.Forget(); return 0; }
    if (!m_did.Ready() && !m_did.Init(m_dev.Get())) {
        if (!m_didInitTried && ShaderCache::Done()) {
            m_didInitTried = true;
            Log("MV draw-ids: the pairing unit could not be created -- per-draw motion vectors off for this unit");
        }
        m_did.Forget();
        return 0;
    }
    // v0.10.0 phase 3: forward draws only with the ids (idSrv): without the target their pairing would be pointless
    const uint32_t n = m_did.Prepare(ctx, did, fullW, fullH, idSrv ? didFwd : nullptr, m_nearReject, freshMedoid, noMedoid,
                                     idSrv != nullptr);   // v0.10.0 phase 9: the folded dispatch's rigid-parent bit
    if (n && idSrv) { m_didN = n; m_didNG = m_did.PreparedG(); m_didIdSrv = idSrv; m_didStaticGate = did->StaticGated(); }
    return n;
}

// ===========================================================================================================
// v0.9.0 per-object motion vectors (see ObjRecord / ObjIds in motion_vectors.h and docs/DLAA_INTEGRATION.md)
// ===========================================================================================================

void ObjRecord::Reset() {
    for (int k = 0; k < m_nRings; ++k) if (m_rings[k].buf) m_rings[k].buf->Release();   // v0.9.0 r3: every ring slot
    for (Ring& r : m_rings) r = Ring{};
    m_nRings = 0; m_last = -1; m_need = 0; m_copiedAny = false; m_seals = 0; m_switches = 0;
    m_n = 0;
    m_flushFail = false;
    m_tagged = 0; m_missed = 0;
    m_otherBuf = nullptr; m_otherBig = false;
    for (int s = 0; s < kIds; ++s) { m_rep[s] = -1; m_repIc[s] = 0; }
}

void ObjRecord::Shutdown() {
    Reset();
    m_mirrorSrv.Reset(); m_mirror.Reset(); m_mirrorBytes = 0;
}

// v0.9.0 r9: the fallback pairing key (indexCount, startIndex, baseVertex; never 0 = "none")
uint64_t ObjRecord::LooseKey(UINT indexCount, UINT startIndex, INT baseVertex) {
    uint64_t x = ((uint64_t)indexCount << 32 | (uint64_t)startIndex) * 0x9E3779B97F4A7C15ull;
    x ^= (uint64_t)(uint32_t)baseVertex * 0xC2B2AE3D27D4EB4Full;
    x ^= x >> 31; x *= 0xD6E8FEB86659FD93ull; x ^= x >> 32;
    return x ? x : 1;
}

ObjRecord::Miss ObjRecord::Add(ID3D11Buffer* cb, UINT mvpByteOff, uint64_t id, uint64_t keyHash, uint32_t occ,
                               UINT indexCount, int slot, bool canRep, const Geo* geo) {
    if (!cb) { ++m_missed; return kMissNoCb; }
    // v0.9.0 r3: which ring slot. Common case: the same buffer as the previous draw (one compare); else one of the open
    // slots; else a big (>= 64 KB, the per-draw constant ring class: 2 MB in the capture) buffer opens a new slot while
    // fewer than kRings are open. A small per-object cbuffer (4 KB) never becomes a ring. The class of the last non-ring
    // buffer is cached (one GetDesc per new buffer; identity only, no ref).
    int k = m_last;
    if (k < 0 || m_rings[k].buf != cb) {
        k = -1;
        for (int j = 0; j < m_nRings; ++j)
            if (m_rings[j].buf == cb && !m_rings[j].sealed) { k = j; break; }
        if (k < 0) {
            if ((const void*)cb == m_otherBuf) { ++m_missed; return m_otherBig ? kMissOtherRing : kMissSmallCb; }
            D3D11_BUFFER_DESC bd{};
            cb->GetDesc(&bd);
            if (bd.ByteWidth < 65536u || m_nRings >= kRings) {
                m_otherBuf = cb;
                m_otherBig = bd.ByteWidth >= 65536u;
                ++m_missed;
                return m_otherBig ? kMissOtherRing : kMissSmallCb;
            }
            k = m_nRings++;
            Ring& nr = m_rings[k];
            nr = Ring{};
            nr.buf = cb;
            cb->AddRef();
            nr.bytes = bd.ByteWidth;
            nr.base = (m_need + 255u) & ~255u;           // mirror layout: rings back to back, 256-byte aligned bases
            m_need = nr.base + ((bd.ByteWidth + 15u) & ~15u);
            nr.lo = nr.usedLo = 0xFFFFFFFFu;
            nr.hi = nr.usedHi = 0;
            nr.firstDraw = m_n;
        }
        m_last = k;
    }
    Ring& r = m_rings[k];
    if (m_n >= kMax || mvpByteOff + 64u > r.bytes) { ++m_missed; return kMissFull; }
    if (m_n > 0 && m_ringOf[m_n - 1] != (uint8_t)k) ++m_switches;
    const int i = m_n++;
    m_id[i] = id; m_kh[i] = keyHash; m_off[i] = r.base + mvpByteOff; m_ic[i] = indexCount;
    m_ringOf[i] = (uint8_t)k;
    m_tag[i] = (uint8_t)(((slot > 0 && slot < kIds) ? slot : 0) | (canRep ? 0 : 0x80));   // v0.9.0 r4 (GPU picks the rep)
    m_occ[i] = (uint16_t)(occ < 0xFFFFu ? occ : 0xFFFFu);
    if (geo) { m_geo[i] = *geo; m_lkh[i] = LooseKey(indexCount, geo->startIndex, geo->baseVertex); }   // v0.9.0 r9
    else { m_geo[i] = Geo{}; m_lkh[i] = 0; }
    if (mvpByteOff < r.lo) r.lo = mvpByteOff;
    if (mvpByteOff + 64u > r.hi) r.hi = mvpByteOff + 64u;
    if (mvpByteOff < r.usedLo) r.usedLo = mvpByteOff;
    if (mvpByteOff + 64u > r.usedHi) r.usedHi = mvpByteOff + 64u;
    ++r.draws;
    if (canRep && slot > 0 && slot < kIds && indexCount > m_repIc[slot]) { m_rep[slot] = i; m_repIc[slot] = indexCount; }
    return kMissNone;
}

// v0.9.0 r3: the mirror holds every ring of the pass at its base; it only grows (64 KB steps). Growing inside a pass
// after a ring was already copied (a sealed ring, a discard) carries the old contents over (one GPU copy, rare).
bool ObjRecord::EnsureMirror(ID3D11DeviceContext* ctx, UINT need) {
    if (m_mirror && m_mirrorSrv && m_mirrorBytes >= need) return true;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    const UINT bytes = (need + 0xFFFFu) & ~0xFFFFu;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.NumElements = bytes / 4;
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    ComPtr<ID3D11Buffer> nb;
    ComPtr<ID3D11ShaderResourceView> nsrv;
    HRESULT hr = E_POINTER;
    if (!dev || need == 0 || bytes > 64u * 1024u * 1024u ||
        FAILED(hr = dev->CreateBuffer(&bd, nullptr, &nb)) ||
        FAILED(hr = dev->CreateShaderResourceView(nb.Get(), &sd, &nsrv))) {
        static int fails = 0;
        if (fails++ < 3) Log("MV objects: ring mirror (%u bytes, %d ring(s)) create failed hr=0x%lx", bytes, m_nRings,
                             (unsigned long)hr);
        return false;
    }
    if (m_mirror && m_copiedAny && m_mirrorBytes) {
        const D3D11_BOX box{ 0, 0, 0, m_mirrorBytes, 1, 1 };
        ctx->CopySubresourceRegion(nb.Get(), 0, 0, 0, 0, m_mirror.Get(), 0, &box);
    }
    m_mirror = nb; m_mirrorSrv = nsrv; m_mirrorBytes = bytes;
    return true;
}

bool ObjRecord::FlushRing(ID3D11DeviceContext* ctx, int k) {
    Ring& r = m_rings[k];
    if (r.hi <= r.lo || !r.buf) return true;
    if (!EnsureMirror(ctx, m_need)) {
        m_flushFail = true;                              // the record is unusable this pass (nothing pending any more)
        for (int j = 0; j < m_nRings; ++j) { m_rings[j].lo = 0xFFFFFFFFu; m_rings[j].hi = 0; }
        return false;
    }
    const D3D11_BOX box{ r.lo, 0, 0, r.hi, 1, 1 };
    ctx->CopySubresourceRegion(m_mirror.Get(), 0, r.base + r.lo, 0, 0, r.buf, 0, &box);
    r.copied += r.hi - r.lo;
    r.lo = 0xFFFFFFFFu; r.hi = 0;
    m_copiedAny = true;
    return true;
}

void ObjRecord::Flush(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    for (int k = 0; k < m_nRings; ++k)
        if (m_rings[k].hi > m_rings[k].lo && !FlushRing(ctx, k)) return;
}

bool ObjRecord::OnDiscard(ID3D11DeviceContext* ctx, ID3D11Resource* buf) {
    if (!ctx || !buf) return false;
    for (int k = 0; k < m_nRings; ++k) {
        Ring& r = m_rings[k];
        if (r.sealed || (ID3D11Resource*)r.buf != buf) continue;
        FlushRing(ctx, k);                               // queued before the discard: reads the old contents
        if (r.draws <= 0) return false;
        r.sealed = true;                                 // later draws from this buffer (new contents) -> a new slot
        ++m_seals;
        if (m_last == k) m_last = -1;
        return true;
    }
    return false;
}

ObjRecord::RingInfo ObjRecord::GetRing(int k) const {
    RingInfo ri{};
    if (k < 0 || k >= m_nRings) return ri;
    const Ring& r = m_rings[k];
    ri.buf = r.buf; ri.bytes = r.bytes; ri.base = r.base;
    ri.usedLo = r.usedHi > r.usedLo ? r.usedLo : 0; ri.usedHi = r.usedHi;
    ri.copied = r.copied; ri.draws = r.draws; ri.firstDraw = r.firstDraw; ri.sealed = r.sealed;
    return ri;
}

// ---- ObjIds: identity -> stencil id (rigid-motion groups) ------------------------------------------------------------
namespace ObjIds {
namespace {
// v0.9.0 r6: 512 members (r4: 256 -- round 3 / 4 reached 256 with over-budget; sticky membership keeps more members alive)
constexpr int kMaxMembers = 512;
constexpr int kMaxWait    = 256;
constexpr int kMaxRej     = 256;           // v0.9.0 r2: recently rejected identities (debug view only)
constexpr int kLutBits    = 12;            // v0.9.0 r6: 4096 entries (r4: 2048)
constexpr int kLut        = 1 << kLutBits; // power of two, >= 4 x (kMaxMembers + kMaxRej)
// v0.9.0 r6: readbacks in which a member was NOT DRAWN (not recorded with a paired key in the read-back pass) before it is
// released. r2..r4: readbacks without being seen MOVING (> 0.5 px) -- a far car driving towards the camera or slowing down
// dropped under 0.5 px, lost its tag after 8 readbacks and got it back later: the "pink stops" wobble of round 4.
constexpr int kMissToDrop = 8;
constexpr float kTightFrob = 1.0e-3f;      // same rigid body: R equal up to float noise
constexpr float kOriginPx  = 1.0f;         // loose join: the id's motion moves this identity's origin within 1 px
constexpr float kOriginPxOld = 1.5f;       // v0.9.0 r6: spinning join with a reference R 1..kRefAge readbacks old
constexpr int   kRefAge   = 2;             // v0.9.0 r6: an id's reference R at most this many readbacks old is "recent"
// v0.9.0 r4: a member stays while the id's motion keeps its origin within 1.5 px. v0.9.0 r6: it FITS within max(1.5 px,
// 35 % of its own correction); it is a proven MISFIT only beyond max(4 px, 50 %) -- the GPU veto's threshold -- and only
// a misfit that fits somewhere else (another id, or its own motion: a free id) in kProveRb judged readbacks in a row
// leaves. Between the two it stays (the id's R is closer to its motion than the camera R).
constexpr float kMemberPx  = 1.5f;
constexpr float kMemberRel = 0.35f;
constexpr float kGrossPx   = 4.0f;
constexpr float kGrossRel  = 0.5f;
constexpr int   kProveRb   = 2;
constexpr float kBetter    = 2.0f;         // "clearly better elsewhere": own misfit >= kBetter x the other fit + 1 px
constexpr int   kFollowMaxDup = 8;         // v0.9.0 r3: follower rule only with a complete candidate set (= CameraMv::kObjCands)
// v0.9.0 r3: the loose 1-px join and the follower rule only within this clip-space distance (x, y, w; ~ m, x / y carry the
// projection's focal factor 0.75 / 1.33 in the game: ~20 m sideways, ~11 m up, 15 m in depth) of the id's anchor origin:
// a wheel is next to its vehicle. Without it a slow vehicle far away whose screen motion happens to match another
// vehicle's within 1 px joined that vehicle's id (synthetic test, low resolution). v0.9.0 r6: of ANY member's last origin
// (a semi-trailer's rear wheels are > 15 clip units from a tractor anchor seen side-on).
constexpr float kNearClip = 15.0f;
// v0.9.0 r2 temporal coherence. d = the origin's per-frame displacement vs the camera-only prediction in the previous
// frame's clip space (x, y, w; x / y carry the projection's focal factor, w is metres). A vehicle's d changes by
// millimetres per frame (accelerating, braking, turning, the camera yawing 90 deg/s: < 3 % of |d|); a mis-paired draw
// jumps by metres. Two readbacks are compared when they are at most kCohGap frames apart.
// v0.9.0 r6: the tolerance never drops below kCohPx pixels of footprint at the origin's depth (clip units per px = 2 w /
// image size): at 250 m one pixel is ~0.2-0.4 clip units, so the r2 tolerance (0.15 + 0.35 |d| ~ 0.27 for a 0.33 m/frame
// car) was ~1 px there and sub-pixel noise counted as "jumps" (round 4: ids with jumps 12-27 at a steady speed window).
constexpr int   kCohGap   = 4;
constexpr float kCohAbs   = 0.15f;         // tolerance per frame of gap (clip units ~ m)
constexpr float kCohRel   = 0.35f;         // ... plus this fraction of the larger |d|
constexpr float kCohPx    = 2.0f;          // v0.9.0 r6: ... and at least 2 px of footprint
// v0.9.0 r6 hysteresis: one readback without a sighting is forgiven between two coherent sightings (r2 behaviour; r3 made
// them strictly consecutive) when the identity was not drawn in it, or was drawn sub-threshold while its correction is
// small (< kGapPx: a far car hovering around the 0.5 px "moving" threshold). A copy among identical meshes whose neighbour
// is culled every other frame (a fake "displacement" = the spacing, many px, and a static pairing in between) is not.
constexpr float kGapPx    = 2.0f;
constexpr int   kBadToDrop = 2;            // consecutive REAL jumps that drop a member at once (r2..r4: + rejects)
constexpr int   kRejTtl   = 4;             // readbacks a rejected identity stays marked (debug view)
constexpr int   kDbgSlot  = ObjRecord::kIds - 1;   // 15: "rejected" in the Ctrl+F6 view
// v0.9.0 r4: a freed id is not handed out again for this many frames of the feeding unit (Update's nowFrame)
constexpr uint64_t kQuarantine = 3;
// v0.9.0 r4: followers (when mv_objects_followers = 1) only join an id whose anchor refreshed coherently in this many
// consecutive readbacks
constexpr int   kSolidRb  = 4;
// v0.9.0 r3: follower = joined through the follower rule (no trusted pairing of its own: never anchors / represents its id)
// v0.9.0 r4: spin = a spinning part (wheel): never anchors / represents its id either
// v0.9.0 r4: div = consecutive readbacks in which its motion no longer matched its id's. v0.9.0 r6: consecutive judged
//   readbacks with the same PROOF (target: s = fits id s, -1 = moves on its own; 0 = none)
// v0.9.0 r6: c0 = its origin (current clip) at its last listed sighting (proximity to ANY member, kNearClip)
// v0.9.0 r9: the r8 fastJ / seenAgain / joinU fields (fast-joined-then-released diagnostic) are gone with the
//   non-spinning fast join
// v0.9.0 r10: attach = joined through the r10 attach rule (a plate / mirror on its anchor member's world matrix: never
//   anchors / represents), attAnchor = that member's identity, attMiss = judged readbacks in a row whose origins did
//   not coincide
struct Member { uint64_t id; int slot; int misses; int bad; float ic; float d[3]; uint64_t frame; float w; int dup;
                bool follower; bool spin; int div; int target; float c0[4]; bool hasC0;
                bool attach; uint64_t attAnchor; int attMiss; };
struct AWait  { uint64_t id; uint64_t anchor; int hits; };      // v0.9.0 r10: attach candidates (consecutive)
// v0.9.0 r6: corr = its correction (px) at the last sighting (forgiving a sub-threshold gap, kGapPx)
struct Wait   { uint64_t id; int hits; int age; float d[3]; uint64_t frame; float corr; };
struct Rej    { uint64_t id; int ttl; };
struct FWait  { uint64_t id; int slot; int hits; int age; };   // v0.9.0 r3: follower candidates (consecutive follows)
struct LEnt   { uint64_t id; int slot; int mem; bool follower; };   // mem = member index at the last rebuild (-1 = debug)
// per id (diagnostics, LogIds): readback it was taken at, jumps, re-anchors, the anchor's dup / depth / speed
struct SlotInfo { uint64_t taken; int jumps, reanchors, dup; float w, speed, spMin, spMax; bool spSeen; };
Member g_mem[kMaxMembers]; int g_nMem = 0;
Wait   g_wait[kMaxWait];   int g_nWait = 0;
Rej    g_rej[kMaxRej];     int g_nRej = 0;
constexpr int kMaxFWait = 128;             // v0.9.0 r3
FWait  g_fw[kMaxFWait];    int g_nFw = 0;
float    g_slotR[ObjRecord::kIds][16] = {};
bool     g_slotUsed[ObjRecord::kIds] = {};
uint64_t g_slotAnchor[ObjRecord::kIds] = {};   // the member whose R the id keeps (r6: the member that last gave its reference R)
float    g_slotAnchorIc[ObjRecord::kIds] = {};
float    g_slotC0[ObjRecord::kIds][4] = {};      // v0.9.0 r3: the anchor's origin (current clip) at its last refresh
SlotInfo g_slotInfo[ObjRecord::kIds] = {};
uint64_t g_slotFreeAt[ObjRecord::kIds] = {};     // v0.9.0 r4: nowFrame the id was freed (quarantine)
int      g_slotSolid[ObjRecord::kIds] = {};      // v0.9.0 r4: consecutive coherent anchor refreshes (follower gate)
uint64_t g_slotRUpd[ObjRecord::kIds] = {};       // v0.9.0 r6: Update number of the last reference refresh (R age)
int      g_slotNoBody[ObjRecord::kIds] = {};     // v0.9.0 r6: readbacks the id had spinning / follower members only
uint64_t g_now = 0;                              // v0.9.0 r4: Update's nowFrame
LEnt     g_lut[kLut] = {};
int      g_max = 15;
// v0.9.0 r2 dlaa.ini mv_objects_max_dup. r3: default 6 (was 3) -- the static false movers stay filtered by the trusted-
// pairing rule either way (a static copy pairs with ITSELF; a reordered copy's nearest candidate is itself under another
// occurrence index: "pairing"; the r4 synthetic test flips two static copies at 250 m every frame: never an id).
// v0.9.0 r4: back to 3. The round-3 log shows what 6 let in: wheel meshes drawn 2-6 times per pass ("dup 2 / 3 / 4 / 6"
// anchors, pairs and triples of ids with the SAME IndexCount at the same depth and speed) -- with ring 1 recorded as well
// they took all 15 ids (over-budget +7683 in one window). The spin rule now keeps wheels from anchoring at any cap; 3 is
// the value of the round-2 test the user found clean.
int      g_maxDup = 3;
bool     g_followers = false;                    // v0.9.0 r4 dlaa.ini mv_objects_followers (default 0)
int      g_hits = 2;                       // v0.9.0 r2 dlaa.ini mv_objects_hits (was the constant kHitsToJoin = 2)
float    g_egoM = 8.0f;                    // v0.9.0 r2 dlaa.ini mv_objects_ego_m
// v0.9.0 r7 dlaa.ini mv_objects_lock_px (GPU: own truck = near AND locked). r8: default 6 (r7: 3), measured at the
// origin only: a pre-filter; the own truck itself is the ego set below
float    g_lockPx = 6.0f;
int      g_hold = 3;                       // v0.9.0 r7 dlaa.ini mv_objects_hold (GPU: frames an id may keep its last R)
// v0.9.0 r8 EGO SET (round-6 log, truck standing still: the own truck's exterior parts -- all sharing the truck root as
// origin, 1.1..1.3 m from the interior camera -- took 4..6 of the 15 ids for the whole video: the idle shake moved them
// > 0.5 px against the camera R and swung their probe points at +-2 m by > 3 px, so the r7 per-frame lock test let them
// in; driving, their pairing failed (left / right copies) so they were never judged again: ids without a trusted member
// 3.00 per readback). The own truck is what stays PUT on screen over time, not in one frame: a near identity (origin
// view depth < mv_objects_ego_m, on screen) whose origin stayed within g_egoPx of the centre of its box (per axis) over
// g_egoRb readbacks is flagged; while flagged its reference is a slow running mean (the sway oscillates around it, an
// acceleration nose-lift shifts it slowly) and two sightings in a row farther than g_egoPx from it drop the flag (a car
// that stood next to us drives off; one jolt does not). Fed by the moving AND the reject entries (every near candidate
// reaches the CPU).
// v0.9.0 r9 (round-7 log: "0 flagged of 34 near identities tracked | best unflagged run 1 of 24 readbacks" in every
// dump: every near box restarted at every sighting). Two changes:
//  (1) BY GEOMETRY KEY, one entry per PHYSICAL COPY: a sighting updates the nearest entry of its key (within kEgoMatch
//      x g_egoPx, one sighting per entry and readback), else it opens a new entry. r8 tracked the IDENTITY (key +
//      occurrence index): symmetric copies of one mesh (left / right mirror, lamps, steps) whose draw order flips name
//      the other copy on every flip -- a jump of hundreds of px, a restart. The ego IDENTITIES (Table.z bit 15, members
//      released) are the ones whose last near sighting matched a FLAGGED copy (g_egoId); a sighting that matches an
//      unflagged copy, opens a new one or is not near takes its identity out again.
//  (2) THE POSITION AT THE ENTRY'S REFERENCE DEPTH wr: (0.5 W x, 0.5 H y, 0.5 W w) / wr in clip units (px-equivalent),
//      not the origin's projection 0.5 W x / w. The own truck's parts share the truck root as origin, ~1.2 m in depth
//      but possibly far off screen (below / beside the interior camera: |x / w| of several): there 1 mm of head motion
//      in depth moves the projected origin by tens of px. For an on-screen origin the measure is the projected motion
//      (within ~1.4 x); off screen it is bounded by the clip-space motion itself (~ the real motion of the part).
// v0.9.0 r10 (round-8 log: "0 flagged of 112 near copies tracked | best unflagged run 1 of 24", copies growing 30 ->
// 112: every sighting opened a new copy or restarted its box -- the clip x / y / w of an origin 0.1..1.7 m from the
// camera plane still swung by more than the box): THE POSITION IS THE ORIGIN'S VIEW-SPACE POSITION IN METRES (CSMain
// recovers it from the MVP, Moving / Reject::v), the box g_egoMBox per axis (mv_objects_ego_m_box, default 0.05 m) and
// the copy match kEgoMatch x g_egoMBox. A camera-locked part keeps a constant view-space origin up to the cabin sway
// (cm), a passing vehicle's moves by its relative speed (0.1..1 m per frame). g_egoPx (mv_objects_ego_px) is no longer
// used.
float    g_egoPx = 12.0f;                  // v0.9.0 r8 dlaa.ini mv_objects_ego_px (px of the full render; r10: unused)
int      g_egoRb = 24;                     // v0.9.0 r8 dlaa.ini mv_objects_ego_rb (readbacks seen)
float    g_egoMBox = 0.05f;                // v0.9.0 r10 dlaa.ini mv_objects_ego_m_box (m of view space, per axis)
constexpr int   kEgoMax = 256;             // near copies tracked (r8: identities)
constexpr int   kEgoHash = 1024;           // open addressing, >= 4 x kEgoMax (r9: keyed by the geometry key, duplicates)
constexpr float kEgoAlpha = 1.0f / 32.0f;  // running-mean weight of a flagged copy's reference
constexpr int   kEgoForgetRb = 3600;       // a flagged copy / ego identity not seen for this many readbacks goes (~50 s)
constexpr float kEgoMatch = 3.0f;          // v0.9.0 r9: a sighting belongs to the nearest copy within this x the box
constexpr int   kEgoIdMax = 512;           // v0.9.0 r9: ego identities
constexpr int   kEgoIdHash = 2048;         // >= 4 x kEgoIdMax
// v0.9.0 r9: kh = geometry key, out = flagged sightings beyond the box in a row (r8: g_egoOut[]); r10: ref / lo / hi in
// metres of view space (r9: + wr, the reference depth of the px-equivalent measure)
struct EgoEnt { uint64_t kh; float ref[3], lo[3], hi[3]; int n; int out; bool flag; uint64_t lastU; };
struct EgoId  { uint64_t id; uint64_t lastU; };
EgoEnt   g_ego[kEgoMax]; int g_nEgo = 0;
int      g_egoHash[kEgoHash] = {};         // index + 1 into g_ego, 0 = empty
int      g_egoSetN = 0;                    // flagged entries
float    g_egoMaxDev = 0.0f;               // largest deviation of a flagged sighting since the last LogIds
EgoId    g_egoId[kEgoIdMax]; int g_nEgoId = 0;   // v0.9.0 r9
int      g_egoIdHash[kEgoIdHash] = {};     // index + 1 into g_egoId
int      g_egoTrace = 1;                   // v0.9.0 r9 dlaa.ini mv_objects_ego_trace (r10: default 1)
int      g_egoTraceN = 0;                  // readbacks traced (<= 150)
// v0.9.0 r10 (see the r10 block in motion_vectors.h): the origin gate's margin (mirrored on the GPU through Ext.x), the
// minimum own motion of a new non-spinning mover's sightings, the attach rule
float    g_margin = 1.2f;                  // dlaa.ini mv_objects_origin_margin (|x / w|, |y / w| on screen)
float    g_minPx = 2.0f;                   // dlaa.ini mv_objects_min_px (r11: default 2.0, r10: 1.0)
bool     g_attach = true;                  // dlaa.ini mv_objects_attach
constexpr float kAttachPx  = 0.5f;         // attach: the projected origins within this many px ...
constexpr float kAttachRelW = 0.01f;       // ... and the view depths within this fraction of w
constexpr int   kAttachMissRb = 2;         // judged readbacks in a row without coincidence that release an attached one
constexpr int   kMaxAWait = 128;
AWait    g_aw[kMaxAWait]; int g_nAw = 0;
bool     g_debug = false;                  // v0.9.0 r2 Ctrl+F6 view on: id 15 = rejected
Stats    g_st;
// ---- v0.9.0 r11 (see the r11 block in motion_vectors.h) ----
float    g_camM = 0.3f;                    // dlaa.ini mv_objects_cam_m (the GPU camera-attached test, Ext.y)
int      g_staticRb = 24;                  // dlaa.ini mv_objects_static_rb
// STATIC CLASS history: per identity, bit k of `drawn` / `stat` = readback lastU - k (drawn in it / a static sighting).
// Open addressing (id 0 = empty slot), at most kStLive live entries; entries not drawn for kStaticWin readbacks are
// dropped by StCompact (every 32 readbacks, or when the table is half full).
constexpr int      kStBits = 13;
constexpr int      kStCap = 1 << kStBits;  // 8192 slots
constexpr int      kStLive = kStCap / 2;   // 4096 live identities
constexpr uint64_t kStMask = (1ull << kStaticWin) - 1ull;
struct StEnt { uint64_t id, lastU, drawn, stat; };
StEnt    g_stTab[kStCap];
int      g_stN = 0;                        // occupied slots
uint64_t g_stFullMiss = 0;                 // sightings not recorded (table full; diagnostics)
inline uint32_t StHash(uint64_t id) {
    return (uint32_t)((id * 0xC2B2AE3D27D4EB4Full) >> (64 - kStBits)) & (kStCap - 1);
}
inline int Pop64(uint64_t v) { int c = 0; for (; v; v &= v - 1) ++c; return c; }
StEnt* StFind(uint64_t id) {
    if (!id) id = 1;
    for (uint32_t h = StHash(id);; h = (h + 1) & (kStCap - 1)) {   // terminates: at most half full
        if (!g_stTab[h].id) return nullptr;
        if (g_stTab[h].id == id) return &g_stTab[h];
    }
}
// kind 1 = a sighting of non-static / unknown motion (moving entry, most rejects), 0 = static, 2 = static unless this
// readback already has a sighting of the identity (the `drawn` list, after the lists)
void StMark(uint64_t id, uint64_t U, int kind) {
    if (!id) id = 1;
    uint32_t h = StHash(id);
    while (g_stTab[h].id && g_stTab[h].id != id) h = (h + 1) & (kStCap - 1);
    StEnt& E = g_stTab[h];
    if (!E.id) {
        if (g_stN >= kStLive) { ++g_stFullMiss; return; }
        E.id = id; E.lastU = U; E.drawn = 1; E.stat = kind != 1 ? 1 : 0;
        ++g_stN;
        return;
    }
    if (E.lastU == U) {                                // a second entry of the same readback
        if (kind == 1) E.stat &= ~1ull;
        return;
    }
    const uint64_t sh = U - E.lastU;
    if (sh >= 64) { E.drawn = 0; E.stat = 0; } else { E.drawn <<= sh; E.stat <<= sh; }
    E.lastU = U;
    E.drawn |= 1;
    if (kind != 1) E.stat |= 1;
}
bool StClassOf(const StEnt& E, uint64_t U) {
    const uint64_t sh = U - E.lastU;
    if (sh >= (uint64_t)kStaticWin) return false;
    const int nd = Pop64((E.drawn << sh) & kStMask), ns = Pop64((E.stat << sh) & kStMask);
    return g_staticRb > 0 && nd >= g_staticRb && 2 * ns >= nd;
}
bool StClass(uint64_t id, uint64_t U) {
    if (!g_stN) return false;
    const StEnt* E = StFind(id);
    return E && StClassOf(*E, U);
}
void StCompact(uint64_t U) {                       // drops identities not drawn in the window, rehashes the rest
    static StEnt keep[kStCap];
    int nk = 0;
    for (int k = 0; k < kStCap; ++k)
        if (g_stTab[k].id && U - g_stTab[k].lastU < (uint64_t)kStaticWin) keep[nk++] = g_stTab[k];
    memset(g_stTab, 0, sizeof(g_stTab));
    for (int k = 0; k < nk; ++k) {
        uint32_t h = StHash(keep[k].id);
        while (g_stTab[h].id) h = (h + 1) & (kStCap - 1);
        g_stTab[h] = keep[k];
    }
    g_stN = nk;
}
// v0.9.0 r11 ATTACH DIAGNOSTICS (round-9 log: attached +1..+23 per window, attached-members 0..2): why the dup /
// pairing / ambiguous rejects of the readbacks did not attach -- one "MV objects: attach diagnostics" line per 600
// readbacks
struct AttDiag {
    uint64_t rb, rejects, byWhy[kRejKinds], soft, member, ego, offscreen, noAnchor, farN, farHist[4], wMis, coincide,
             staticCl, anchors;
};
AttDiag  g_attD = {};
// v0.9.0 r10: an origin (current clip) with a screen position inside the viewport + g_margin (the GPU gate's test)
bool OnScreen(const float* c) {
    return c[3] > 0.05f && std::fabs(c[0]) <= g_margin * c[3] && std::fabs(c[1]) <= g_margin * c[3];
}

inline uint32_t LutHash(uint64_t id) { return (uint32_t)((id * 0x9E3779B97F4A7C15ull) >> (64 - kLutBits)) & (kLut - 1); }
const LEnt* LutFind(uint64_t id) {
    for (uint32_t h = LutHash(id);; h = (h + 1) & (kLut - 1)) {   // terminates: the table is at most 1/4 full
        if (!g_lut[h].slot) return nullptr;
        if (g_lut[h].id == id) return &g_lut[h];
    }
}
void LutPut(uint64_t id, int slot, int mem, bool follower) {
    uint32_t h = LutHash(id);
    while (g_lut[h].slot) h = (h + 1) & (kLut - 1);
    g_lut[h].id = id; g_lut[h].slot = slot; g_lut[h].mem = mem; g_lut[h].follower = follower;
}
void RebuildLut() {
    memset(g_lut, 0, sizeof(g_lut));
    // v0.9.0 r4: the LUT's "follower" flag means "may not represent its id" (followers, spinning parts; r10: attached)
    for (int m = 0; m < g_nMem; ++m)
        LutPut(g_mem[m].id, g_mem[m].slot, m, g_mem[m].follower || g_mem[m].spin || g_mem[m].attach);
    if (g_debug)                                       // v0.9.0 r2: rejected identities -> id 15 (debug view, never a member)
        for (int k = 0; k < g_nRej; ++k) if (!LutFind(g_rej[k].id)) LutPut(g_rej[k].id, kDbgSlot, -1, true);
}
int EffMax() { return g_debug ? (g_max < kDbgSlot - 1 ? g_max : kDbgSlot - 1) : g_max; }
float Frob16(const float* a, const float* b) {
    float d = 0.0f;
    for (int i = 0; i < 16; ++i) { const float t = a[i] - b[i]; d += t * t; }
    return std::sqrt(d);
}
// Screen distance (px) between R1 * c and R2 * c (c = an origin in current clip space).
float OriginPx(const float* R1, const float* R2, const float* c, float W, float H) {
    float p1[4], p2[4];
    for (int r = 0; r < 4; ++r) {
        p1[r] = R1[r * 4] * c[0] + R1[r * 4 + 1] * c[1] + R1[r * 4 + 2] * c[2] + R1[r * 4 + 3] * c[3];
        p2[r] = R2[r * 4] * c[0] + R2[r * 4 + 1] * c[1] + R2[r * 4 + 2] * c[2] + R2[r * 4 + 3] * c[3];
    }
    if (!(p1[3] > 1e-4f) || !(p2[3] > 1e-4f)) return 1e30f;
    const float dx = (p1[0] / p1[3] - p2[0] / p2[3]) * 0.5f * W;
    const float dy = (p1[1] / p1[3] - p2[1] / p2[3]) * 0.5f * H;
    const float d = std::sqrt(dx * dx + dy * dy);
    return d == d ? d : 1e30f;
}
// v0.9.0 r3: is the origin c (current clip) within kNearClip of id s's anchor origin
bool NearAnchor(int s, const float* c) {
    const float dx = c[0] - g_slotC0[s][0], dy = c[1] - g_slotC0[s][1], dw = c[3] - g_slotC0[s][3];
    const float d2 = dx * dx + dy * dy + dw * dw;
    return d2 == d2 && d2 <= kNearClip * kNearClip;
}
// v0.9.0 r3 follower test: screen distance (px) between R * c (where an id's motion puts this origin in the previous
// frame) and `a`, a previous-pass origin (prev clip) of a same-key draw. R * c of a draw's own trusted pair IS its
// partner's origin, so "< 1 px" here is exactly the loose join rule (OriginPx) with the partner given explicitly.
float PrevPx(const float* R, const float* c, const float* a, float W, float H) {
    float p[4];
    for (int r = 0; r < 4; ++r) p[r] = R[r * 4] * c[0] + R[r * 4 + 1] * c[1] + R[r * 4 + 2] * c[2] + R[r * 4 + 3] * c[3];
    if (!(p[3] > 1e-4f) || !(a[3] > 1e-4f)) return 1e30f;
    const float dx = (p[0] / p[3] - a[0] / a[3]) * 0.5f * W;
    const float dy = (p[1] / p[3] - a[1] / a[3]) * 0.5f * H;
    const float d = std::sqrt(dx * dx + dy * dy);
    return d == d ? d : 1e30f;
}
float Len3(const float* d) { const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); return l == l ? l : 1e30f; }
// v0.9.0 r6: clip units per pixel at view depth w (the larger of x / y: lenient)
float Footprint(float w, float W, float H) {
    const float m = W < H ? W : H;
    return m > 0.0f && w == w ? 2.0f * (w > 0.0f ? w : -w) / m : 0.0f;
}
// v0.9.0 r2: 1 = the two per-frame displacements agree (roughly constant velocity), 0 = a jump, -1 = not comparable
// (same frame, or more than kCohGap frames apart). v0.9.0 r6: + the footprint floor (w = origin view depth, W x H image).
int Coherent(const float* d0, uint64_t f0, const float* d1, uint64_t f1, float w, float W, float H) {
    if (f1 <= f0 || f1 - f0 > (uint64_t)kCohGap) return -1;
    const float t[3] = { d1[0] - d0[0], d1[1] - d0[1], d1[2] - d0[2] };
    const float l0 = Len3(d0), l1 = Len3(d1);
    float tol = kCohAbs * (float)(f1 - f0) + kCohRel * (l0 > l1 ? l0 : l1);
    const float fp = kCohPx * Footprint(w, W, H);
    if (fp > tol) tol = fp;
    return Len3(t) <= tol ? 1 : 0;
}
// v0.9.0 r6: a moving entry's own correction at its origin (px): where its own motion puts the origin (R * c0 = its
// partner's origin) vs where the camera alone puts it (that minus the displacement d)
float CorrPx(const Moving& m, float W, float H) {
    float a[4];
    for (int r = 0; r < 4; ++r) a[r] = m.R[r * 4] * m.c0[0] + m.R[r * 4 + 1] * m.c0[1] + m.R[r * 4 + 2] * m.c0[2] + m.R[r * 4 + 3] * m.c0[3];
    const float pw = a[3] - m.d[2];
    if (!(a[3] > 1e-4f) || !(pw > 1e-4f)) return 0.0f;
    const float dx = (a[0] / a[3] - (a[0] - m.d[0]) / pw) * 0.5f * W;
    const float dy = (a[1] / a[3] - (a[1] - m.d[1]) / pw) * 0.5f * H;
    const float d = std::sqrt(dx * dx + dy * dy);
    return d == d ? d : 0.0f;
}
// v0.9.0 r2: a rejected identity stays marked for the debug view (only kept while the view is on: no cost otherwise)
void NoteRej(uint64_t id) {
    if (!g_debug) return;
    for (int k = 0; k < g_nRej; ++k) if (g_rej[k].id == id) { g_rej[k].ttl = kRejTtl + 1; return; }
    if (g_nRej < kMaxRej) { g_rej[g_nRej].id = id; g_rej[g_nRej].ttl = kRejTtl + 1; ++g_nRej; }
}
// v0.9.0 r2: per-Update id -> wait-list index map (open addressing; the wait list was a linear search per entry)
constexpr int kWMap = 1024;                // >= 4 x kMaxWait
struct WEnt { uint64_t id; int idx; };
WEnt g_wmap[kWMap];
inline uint32_t WHash(uint64_t id) { return (uint32_t)((id * 0xD6E8FEB86659FD93ull) >> 54) & (kWMap - 1); }
void WMapBuild() {
    for (WEnt& e : g_wmap) e.idx = -1;
    for (int w = 0; w < g_nWait; ++w) {
        uint32_t h = WHash(g_wait[w].id);
        while (g_wmap[h].idx >= 0) h = (h + 1) & (kWMap - 1);
        g_wmap[h].id = g_wait[w].id; g_wmap[h].idx = w;
    }
}
int WMapFind(uint64_t id) {
    for (uint32_t h = WHash(id);; h = (h + 1) & (kWMap - 1)) {
        if (g_wmap[h].idx < 0) return -1;
        if (g_wmap[h].id == id) return g_wmap[h].idx;
    }
}
void WMapPut(uint64_t id, int idx) {
    uint32_t h = WHash(id);
    while (g_wmap[h].idx >= 0) h = (h + 1) & (kWMap - 1);
    g_wmap[h].id = id; g_wmap[h].idx = idx;
}
// member index of `id` as of the last LUT rebuild (-1 = not a member)
int MemberOf(uint64_t id) {
    if (!g_nMem) return -1;
    const LEnt* e = LutFind(id);
    return (e && e->mem >= 0 && e->mem < g_nMem && g_mem[e->mem].id == id) ? e->mem : -1;
}
// v0.9.0 r6: member indices per id, rebuilt once per Update (NearMember scans only its id's members; a member that moved
// to another id later in the same Update is skipped by the slot check, one appended later is not listed -- proximity only)
int g_slotMemIdx[ObjRecord::kIds][kMaxMembers];
int g_slotMemN[ObjRecord::kIds] = {};
void BuildSlotLists() {
    for (int s = 0; s < ObjRecord::kIds; ++s) g_slotMemN[s] = 0;
    for (int m = 0; m < g_nMem; ++m) {
        const int s = g_mem[m].slot;
        if (s >= 0 && s < ObjRecord::kIds) g_slotMemIdx[s][g_slotMemN[s]++] = m;
    }
}
// v0.9.0 r6: is the origin c within kNearClip of id s's anchor origin or of any of its members' last origins
bool NearMember(int s, const float* c) {
    if (NearAnchor(s, c)) return true;
    for (int k = 0; k < g_slotMemN[s]; ++k) {
        const Member& M = g_mem[g_slotMemIdx[s][k]];
        if (M.slot != s || !M.hasC0) continue;
        const float dx = c[0] - M.c0[0], dy = c[1] - M.c0[1], dw = c[3] - M.c0[3];
        const float d2 = dx * dx + dy * dy + dw * dw;
        if (d2 == d2 && d2 <= kNearClip * kNearClip) return true;
    }
    return false;
}
// v0.9.0 r6: free id (not quarantined) for a new anchor; 0 = none (*quarantined = only quarantined ones were free)
int FreeSlot(bool* quarantined) {
    *quarantined = false;
    for (int s = 1; s <= EffMax(); ++s) {
        if (g_slotUsed[s]) continue;
        if (g_now < g_slotFreeAt[s] + kQuarantine) { *quarantined = true; continue; }
        return s;
    }
    return 0;
}
void TakeSlot(int s, const Moving& m) {               // v0.9.0 r6 (was inline in Update): s becomes m's id, m anchors it
    g_slotUsed[s] = true;
    memcpy(g_slotR[s], m.R, sizeof(g_slotR[s]));
    memcpy(g_slotC0[s], m.c0, sizeof(g_slotC0[s]));
    g_slotRUpd[s] = g_st.updates;
    g_slotSolid[s] = 0;
    g_slotNoBody[s] = 0;
    g_slotAnchor[s] = m.id; g_slotAnchorIc[s] = m.ic;
    SlotInfo& S = g_slotInfo[s];
    S = SlotInfo{};
    S.taken = g_st.updates; S.dup = m.dup; S.w = m.w; S.speed = S.spMin = S.spMax = Len3(m.d); S.spSeen = true;
}
void FreeSlotNow(int s) {                             // v0.9.0 r6: + the id-age statistics
    const uint64_t age = g_st.updates - g_slotInfo[s].taken;
    ++g_st.freed; g_st.freedAgeSum += age;
    if (age < (uint64_t)kShortRb) ++g_st.freedShort;
    g_slotUsed[s] = false; g_slotAnchor[s] = 0; g_slotFreeAt[s] = g_now; g_slotSolid[s] = 0; g_slotNoBody[s] = 0;
}
// v0.9.0 r8: a member leaves (not drawn for kMissToDrop readbacks, orphaned, debug reservation, ego set). r9: the r8
// fast-joined-then-released diagnostic it also counted is gone with the non-spinning fast join
void NoteRelease(const Member&) {
    ++g_st.released;
}
// ---- v0.9.0 r8 ego set (see g_egoPx; r9: by geometry key, one entry per physical copy) ----
constexpr int kEgoDropRb = 2;              // sightings in a row beyond g_egoPx that drop a flag (one jolt does not)
inline uint32_t EgoHashOf(uint64_t kh) { return (uint32_t)((kh * 0xC2B2AE3D27D4EB4Full) >> 54) & (kEgoHash - 1); }
inline uint32_t EgoIdHashOf(uint64_t id) { return (uint32_t)((id * 0x9E3779B97F4A7C15ull) >> 53) & (kEgoIdHash - 1); }
void EgoRehash() {                         // also recounts the flagged entries
    memset(g_egoHash, 0, sizeof(g_egoHash));
    g_egoSetN = 0;
    for (int k = 0; k < g_nEgo; ++k) {
        uint32_t h = EgoHashOf(g_ego[k].kh);
        while (g_egoHash[h]) h = (h + 1) & (kEgoHash - 1);
        g_egoHash[h] = k + 1;
        if (g_ego[k].flag) ++g_egoSetN;
    }
}
void EgoIdRehash() {                       // v0.9.0 r9
    memset(g_egoIdHash, 0, sizeof(g_egoIdHash));
    for (int k = 0; k < g_nEgoId; ++k) {
        uint32_t h = EgoIdHashOf(g_egoId[k].id);
        while (g_egoIdHash[h]) h = (h + 1) & (kEgoIdHash - 1);
        g_egoIdHash[h] = k + 1;
    }
}
int EgoIdFind(uint64_t id) {               // terminates: the table is at most 1/4 full
    for (uint32_t h = EgoIdHashOf(id);; h = (h + 1) & (kEgoIdHash - 1)) {
        const int k = g_egoIdHash[h];
        if (!k) return -1;
        if (g_egoId[k - 1].id == id) return k - 1;
    }
}
// v0.9.0 r9: an identity matched a flagged copy (the own truck now) / did not (it is not); removals are compacted by
// EgoAge
void EgoIdPut(uint64_t id, uint64_t U) {
    const int k = EgoIdFind(id);
    if (k >= 0) { g_egoId[k].lastU = U; return; }
    if (g_nEgoId >= kEgoIdMax) return;     // full: EgoAge makes room
    g_egoId[g_nEgoId].id = id; g_egoId[g_nEgoId].lastU = U;
    uint32_t h = EgoIdHashOf(id);
    while (g_egoIdHash[h]) h = (h + 1) & (kEgoIdHash - 1);
    g_egoIdHash[h] = ++g_nEgoId;
}
void EgoIdDrop(uint64_t id) {
    if (!g_nEgoId) return;
    const int k = EgoIdFind(id);
    if (k >= 0) g_egoId[k].lastU = 0;      // 0 = removed (EgoFlagged ignores it, EgoAge compacts it)
}
bool EgoFlagged(uint64_t id) {             // r9: the identity's last near sighting matched a flagged copy
    if (!g_nEgoId) return false;
    const int k = EgoIdFind(id);
    return k >= 0 && g_egoId[k].lastU != 0;
}
// v0.9.0 r10: a sighting's position is the origin's VIEW-SPACE position (m, Moving / Reject::v; r9: clip x / y / w at
// the copy's reference depth, px-equivalent -- still tens of px per mm for an origin 0.1..0.3 m from the camera plane)
void EgoRestart(EgoEnt& E, const float* v) {
    for (int a = 0; a < 3; ++a) E.ref[a] = E.lo[a] = E.hi[a] = v[a];
    E.n = 1;
    E.out = 0;
}
// v0.9.0 r9 ego trace (dlaa.ini mv_objects_ego_trace): one sighting as EgoSee judged it (r10: p / lo / hi / d in m)
struct EgoTr { uint64_t kh, id; int entry; float d, p[3], sx, sy, w; int n; float lo[3], hi[3]; bool flag;
               const char* what; };
// One near candidate sighting (moving or reject entry; identities are unique per pass, so at most one per readback). A
// sighting that is not near (any more) or has no position is never ego: its identity leaves the ego identities (r9: no
// entry changes -- the copy ages out). r10: "no position" = the view-space origin could not be recovered (v NaN); an
// origin at / behind the camera plane has one (r9: w <= 0.05 had none).
void EgoSee(uint64_t id, uint64_t kh, const float* c0, const float* v, float w, float W, float H, uint64_t U,
            EgoTr* tr) {
    const bool nearO = w < g_egoM;
    if (tr) {
        tr->kh = kh; tr->id = id; tr->entry = -1; tr->d = 0.0f; tr->w = c0[3]; tr->n = 0; tr->flag = false; tr->what = "";
        tr->sx = c0[3] > 0.05f ? c0[0] / c0[3] * 0.5f * W : 0.0f;
        tr->sy = c0[3] > 0.05f ? c0[1] / c0[3] * 0.5f * H : 0.0f;
        for (int a = 0; a < 3; ++a) tr->p[a] = tr->lo[a] = tr->hi[a] = 0.0f;
    }
    if (!nearO || !(std::fabs(v[0]) < 1.0e6f) || !(std::fabs(v[1]) < 1.0e6f) || !(std::fabs(v[2]) < 1.0e6f)) {   // NaN
        EgoIdDrop(id);
        if (tr) tr->what = !nearO ? "not near" : "no position";
        return;
    }
    const float box = g_egoMBox;               // r10: metres per axis (r8 / r9: g_egoPx)
    // r9: the nearest copy of this key not sighted yet in this readback
    int best = -1;
    float bd = 1e30f;
    for (uint32_t h = EgoHashOf(kh); g_egoHash[h]; h = (h + 1) & (kEgoHash - 1)) {
        const int k = g_egoHash[h] - 1;
        const EgoEnt& E = g_ego[k];
        if (E.kh != kh || E.lastU == U) continue;
        const float dx = v[0] - E.ref[0], dy = v[1] - E.ref[1], dz = v[2] - E.ref[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d < bd) { bd = d; best = k; }
    }
    if (best < 0 || !(bd <= kEgoMatch * box)) {   // a new copy (or the old one moved far away)
        EgoIdDrop(id);
        if (g_nEgo >= kEgoMax) { if (tr) tr->what = "table full"; return; }   // EgoAge makes room for the next readback
        const int k = g_nEgo++;
        EgoEnt& E = g_ego[k];
        E.kh = kh; E.flag = false; E.lastU = U;
        EgoRestart(E, v);
        uint32_t h = EgoHashOf(kh);
        while (g_egoHash[h]) h = (h + 1) & (kEgoHash - 1);
        g_egoHash[h] = k + 1;
        if (tr) {
            tr->entry = k; tr->d = best < 0 ? 0.0f : bd; tr->n = 1; tr->what = best < 0 ? "new copy" : "new copy (far)";
            for (int a = 0; a < 3; ++a) tr->p[a] = tr->lo[a] = tr->hi[a] = E.ref[a];
        }
        return;
    }
    EgoEnt& E = g_ego[best];
    E.lastU = U;
    const float* p = v;
    const char* what = "";
    if (E.flag) {                              // flagged: the reference is a slow running mean
        if (!(bd <= box)) {
            EgoIdDrop(id);
            what = "flagged, out";
            if (++E.out >= kEgoDropRb) {        // drifted away: a mover again (normal hits)
                E.flag = false; ++g_st.egoDropped;
                EgoRestart(E, v);
                what = "flag DROPPED";
            }
        } else {
            E.out = 0;
            if (bd > g_egoMaxDev) g_egoMaxDev = bd;
            for (int a = 0; a < 3; ++a) E.ref[a] += (p[a] - E.ref[a]) * kEgoAlpha;
            if (E.n < 1000000) ++E.n;
            EgoIdPut(id, U);
            what = "flagged";
        }
    } else {
        // not flagged yet: the box of its sightings must stay within 2 x the box per axis (every sighting within the
        // box of the box centre); a sighting outside starts a new box
        bool restart = false;
        for (int a = 0; a < 3; ++a) {
            if (p[a] < E.lo[a]) E.lo[a] = p[a];
            if (p[a] > E.hi[a]) E.hi[a] = p[a];
            if (E.hi[a] - E.lo[a] > 2.0f * box) restart = true;
        }
        EgoIdDrop(id);
        if (restart) { EgoRestart(E, v); what = "box restarted"; }
        else {
            ++E.n;
            for (int a = 0; a < 3; ++a) E.ref[a] = 0.5f * (E.lo[a] + E.hi[a]);
            what = "box grows";
            if (E.n >= g_egoRb) { E.flag = true; E.out = 0; ++g_st.egoFlagged; EgoIdPut(id, U); what = "FLAGGED"; }
        }
    }
    if (tr) {
        tr->entry = best; tr->d = bd; tr->n = E.n; tr->flag = E.flag; tr->what = what;
        for (int a = 0; a < 3; ++a) { tr->p[a] = p[a]; tr->lo[a] = E.lo[a]; tr->hi[a] = E.hi[a]; }
    }
}
// After the sightings: unflagged copies not seen for 2 x g_egoRb readbacks go (their persistence broke), flagged ones
// and ego identities not seen for kEgoForgetRb readbacks are forgotten, removed identities are compacted; a nearly full
// table drops its stalest unflagged entries.
void EgoAge(uint64_t U) {
    int o = 0;
    for (int k = 0; k < g_nEgo; ++k) {
        const EgoEnt& E = g_ego[k];
        const uint64_t age = U - E.lastU;
        if (!E.flag && age > 2ull * (uint64_t)g_egoRb) continue;
        if (E.flag && age > (uint64_t)kEgoForgetRb) continue;
        g_ego[o++] = E;
    }
    g_nEgo = o;
    while (g_nEgo > kEgoMax - 16) {            // room for new near copies: the stalest unflagged one goes
        int worst = -1;
        for (int k = 0; k < g_nEgo; ++k)
            if (!g_ego[k].flag && (worst < 0 || g_ego[k].lastU < g_ego[worst].lastU)) worst = k;
        if (worst < 0) break;
        g_ego[worst] = g_ego[g_nEgo - 1];
        --g_nEgo;
    }
    EgoRehash();
    o = 0;                                     // v0.9.0 r9: the ego identities
    for (int k = 0; k < g_nEgoId; ++k) {
        const EgoId& I = g_egoId[k];
        if (!I.lastU || U - I.lastU > (uint64_t)kEgoForgetRb) continue;
        g_egoId[o++] = I;
    }
    g_nEgoId = o;
    while (g_nEgoId > kEgoIdMax - 32) {        // room for new ones: the stalest goes
        int worst = 0;
        for (int k = 1; k < g_nEgoId; ++k) if (g_egoId[k].lastU < g_egoId[worst].lastU) worst = k;
        g_egoId[worst] = g_egoId[--g_nEgoId];
    }
    EgoIdRehash();
}
} // namespace

void SetMax(int n) { g_max = n < 1 ? 1 : (n > ObjRecord::kIds - 1 ? ObjRecord::kIds - 1 : n); }
int  Max() { return EffMax(); }
void SetMaxDup(int n) { g_maxDup = n < 1 ? 1 : (n > 8 ? 8 : n); }
int  MaxDup() { return g_maxDup; }
void SetFollowers(bool on) { g_followers = on; }       // v0.9.0 r4
bool Followers() { return g_followers; }
void SetEgoM(float m) { g_egoM = !(m > 0.0f) ? 0.0f : (m > 30.0f ? 30.0f : m); }
float EgoM() { return g_egoM; }
void SetHits(int n) { g_hits = n < 2 ? 2 : (n > 8 ? 8 : n); }
int  Hits() { return g_hits; }
void  SetLockPx(float px) { g_lockPx = !(px >= 0.5f) ? 0.5f : (px > 20.0f ? 20.0f : px); }   // v0.9.0 r7 (NaN -> 0.5)
float LockPx() { return g_lockPx; }
void  SetHold(int n) { g_hold = n < 0 ? 0 : (n > 8 ? 8 : n); }                           // v0.9.0 r7
int   Hold() { return g_hold; }
void  SetEgoPx(float px) { g_egoPx = !(px >= 2.0f) ? 2.0f : (px > 64.0f ? 64.0f : px); }   // v0.9.0 r8 (NaN -> 2)
float EgoPx() { return g_egoPx; }
void  SetEgoRb(int n) { g_egoRb = n < 4 ? 4 : (n > 200 ? 200 : n); }                     // v0.9.0 r8
int   EgoRb() { return g_egoRb; }
bool  IsEgo(uint64_t id) { return EgoFlagged(id); }                                     // v0.9.0 r8 (r9: by key)
void  SetEgoTrace(int on) { g_egoTrace = on ? 1 : 0; g_egoTraceN = 0; }                 // v0.9.0 r9
int   EgoTrace() { return g_egoTrace; }
// v0.9.0 r10 (NaN -> the low limit)
void  SetOriginMargin(float m) { g_margin = !(m >= 1.0f) ? 1.0f : (m > 3.0f ? 3.0f : m); }
float OriginMargin() { return g_margin; }
void  SetMinPx(float px) { g_minPx = !(px >= 0.5f) ? 0.5f : (px > 4.0f ? 4.0f : px); }
float MinPx() { return g_minPx; }
void  SetAttach(bool on) { g_attach = on; }
bool  Attach() { return g_attach; }
void  SetEgoMBox(float m) { g_egoMBox = !(m >= 0.01f) ? 0.01f : (m > 0.5f ? 0.5f : m); }
float EgoMBox() { return g_egoMBox; }
// v0.9.0 r11 (NaN -> the low limit)
void  SetCamM(float m) { g_camM = !(m >= 0.05f) ? 0.05f : (m > 2.0f ? 2.0f : m); }
float CamM() { return g_camM; }
void  SetStaticRb(int n) { g_staticRb = n <= 0 ? 0 : (n < 8 ? 8 : (n > kStaticWin ? kStaticWin : n)); }   // 0 = off
int   StaticRb() { return g_staticRb; }
bool  IsStaticClass(uint64_t id) { return StClass(id, g_st.updates); }
// the CPU copy of CSMain's ViewOrigin (kObjShader): row 3 = +-(view row 2) x world scale, skew / focal from rows 0 / 1
bool MvpViewOrigin(const float* M, float v[3]) {
    const float* r0 = M; const float* r1 = M + 4; const float* r3 = M + 12;
    const double s2 = (double)r3[0] * r3[0] + (double)r3[1] * r3[1] + (double)r3[2] * r3[2];
    if (!(s2 > 1e-20)) return false;
    const double s = std::sqrt(s2);
    const double pe = ((double)r0[0] * r3[0] + (double)r0[1] * r3[1] + (double)r0[2] * r3[2]) / s2;
    const double qe = ((double)r1[0] * r3[0] + (double)r1[1] * r3[1] + (double)r1[2] * r3[2]) / s2;
    double fx = 0.0, fy = 0.0;
    for (int a = 0; a < 3; ++a) {
        const double x = r0[a] - pe * r3[a], y = r1[a] - qe * r3[a];
        fx += x * x; fy += y * y;
    }
    fx = std::sqrt(fx) / s; fy = std::sqrt(fy) / s;
    if (!(fx > 1e-6) || !(fy > 1e-6)) return false;
    const double X = (r0[3] - pe * r3[3]) / fx, Y = (r1[3] - qe * r3[3]) / fy, Z = r3[3];
    if (!std::isfinite(X) || !std::isfinite(Y) || !std::isfinite(Z)) return false;
    v[0] = (float)X; v[1] = (float)Y; v[2] = (float)Z;
    return true;
}
bool CamAttached(const float* M, float m, float* dist) {
    float v[3];
    if (MvpViewOrigin(M, v)) {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (dist) *dist = l;
        if (l < m) return true;
    } else if (dist) {
        *dist = std::fabs(M[15]);
    }
    return std::fabs(M[15]) < m && std::fabs(M[3]) < m && std::fabs(M[7]) < m;
}
int  DebugSlot() { return g_debug ? kDbgSlot : 0; }

void SetDebug(bool on) {
    if (on == g_debug) return;
    g_debug = on;
    g_nRej = 0;
    if (on && g_slotUsed[kDbgSlot]) {                  // id 15 is reserved while the view is on: its members let go
        int out = 0;
        for (int m = 0; m < g_nMem; ++m) {
            if (g_mem[m].slot == kDbgSlot) { NoteRelease(g_mem[m]); continue; }   // v0.9.0 r8: + the fast-join stat
            g_mem[out++] = g_mem[m];
        }
        g_nMem = out;
        FreeSlotNow(kDbgSlot);                         // v0.9.0 r4 quarantine, r6 age stats
    }
    RebuildLut();
}

int Lookup(uint64_t id) {
    if (!g_nMem && !g_nRej) return 0;
    const LEnt* e = LutFind(id);
    return e ? e->slot : 0;
}

int Lookup(uint64_t id, bool* follower) {             // v0.9.0 r3
    *follower = false;
    if (!g_nMem && !g_nRej) return 0;
    const LEnt* e = LutFind(id);
    if (!e) return 0;
    *follower = e->follower;
    return e->slot;
}

bool IsAnchor(uint64_t id, int slot) {                 // v0.9.0 r6
    return slot >= 1 && slot < ObjRecord::kIds && g_slotUsed[slot] && g_slotAnchor[slot] == id;
}

void Clear() {
    g_nMem = 0; g_nWait = 0; g_nRej = 0; g_nFw = 0;
    for (int s = 0; s < ObjRecord::kIds; ++s) {
        g_slotUsed[s] = false; g_slotAnchor[s] = 0; g_slotAnchorIc[s] = 0.0f; g_slotInfo[s] = SlotInfo{};
        g_slotFreeAt[s] = 0; g_slotSolid[s] = 0;     // v0.9.0 r4 (no stencil tag survives a Clear: the LUT is empty)
        g_slotRUpd[s] = 0; g_slotNoBody[s] = 0;      // v0.9.0 r6
    }
    memset(g_lut, 0, sizeof(g_lut));
    g_nEgo = 0; g_egoSetN = 0; g_egoMaxDev = 0.0f;   // v0.9.0 r8: the ego set is learnt again (mv_objects_ego_rb)
    memset(g_egoHash, 0, sizeof(g_egoHash));
    g_nEgoId = 0; memset(g_egoIdHash, 0, sizeof(g_egoIdHash));   // v0.9.0 r9
    g_nAw = 0;                                                   // v0.9.0 r10
    memset(g_stTab, 0, sizeof(g_stTab)); g_stN = 0;              // v0.9.0 r11: the static class is learnt again
    g_attD = AttDiag{};
}

Stats GetStats() {
    Stats s = g_st;
    s.members = g_nMem; s.waiting = g_nWait;
    s.followers = 0; s.spinners = 0;
    for (int m = 0; m < g_nMem; ++m) { if (g_mem[m].follower) ++s.followers; if (g_mem[m].spin) ++s.spinners; }
    s.idsInUse = 0;
    int ages[ObjRecord::kIds]; int na = 0;             // v0.9.0 r6: ages of the ids in use (readbacks held)
    for (int i = 1; i < ObjRecord::kIds; ++i)
        if (g_slotUsed[i]) { ++s.idsInUse; ages[na++] = (int)(g_st.updates - g_slotInfo[i].taken); }
    std::sort(ages, ages + na);
    s.ageMin = na ? ages[0] : 0; s.ageMed = na ? ages[na / 2] : 0; s.ageMax = na ? ages[na - 1] : 0;
    s.egoSet = g_egoSetN; s.egoTracked = g_nEgo; s.egoMaxDev = g_egoMaxDev;   // v0.9.0 r8
    s.egoIds = 0;                                                             // v0.9.0 r9
    for (int k = 0; k < g_nEgoId; ++k) if (g_egoId[k].lastU) ++s.egoIds;
    s.attachedMembers = 0;                                                    // v0.9.0 r10
    for (int m = 0; m < g_nMem; ++m) if (g_mem[m].attach) ++s.attachedMembers;
    s.staticClass = 0;                                                        // v0.9.0 r11
    if (g_stN)
        for (int k = 0; k < kStCap; ++k) if (g_stTab[k].id && StClassOf(g_stTab[k], g_st.updates)) ++s.staticClass;
    return s;
}

void Update(const Moving* e, int n, const Reject* r, int nr, uint64_t frame, float W, float H, uint64_t nowFrame,
            const uint64_t* drawn, int nDrawn) {
    ++g_st.updates;
    const uint64_t U = g_st.updates;                   // v0.9.0 r6: reference R age = U - g_slotRUpd[s]
    g_now = nowFrame;                                   // v0.9.0 r4 (quarantine)
    static bool seen[kMaxMembers], listed[kMaxMembers], drop[kMaxMembers], jumped[kMaxMembers];
    static int  movIdx[kMaxMembers], coh[kMaxMembers];
    // v0.9.0 r10: released (origin off screen / attach no longer valid: no debug-view mark), origin listed now
    static bool offRel[kMaxMembers], attRel[kMaxMembers], c0Now[kMaxMembers];
    memset(seen, 0, sizeof(seen)); memset(listed, 0, sizeof(listed)); memset(drop, 0, sizeof(drop));
    memset(jumped, 0, sizeof(jumped));
    memset(offRel, 0, sizeof(offRel)); memset(attRel, 0, sizeof(attRel)); memset(c0Now, 0, sizeof(c0Now));
    for (int m = 0; m < kMaxMembers; ++m) { movIdx[m] = -1; coh[m] = -1; }
    bool waitSeen[kMaxWait] = {};
    bool waitDrawn[kMaxWait] = {};                     // v0.9.0 r6: drawn (paired) in this pass without a sighting
    if (n > 256) n = 256;
    // E. v0.9.0 r8 EGO SET first: every near candidate of this readback (moving entries + rejects; not the veto
    // records,
    //    which are tagged draws judged against their id) updates the table; an identity flagged now (or before) leaves:
    //    members are released ("ego-released"; an id left without members is freed in step 8), waiting / follower
    //    candidates are dropped, and steps 4 / 5 skip it. The LUT is rebuilt at once when members left (MemberOf reads
    //    the member indices of the last rebuild).
    //    v0.9.0 r9: by geometry key (one entry per physical copy), the ego identities = those whose sighting matched a
    //    flagged copy; mv_objects_ego_trace logs the near sightings of the first 150 readbacks that have one.
    {
        const bool trace = g_egoTrace && g_egoTraceN < 150;
        EgoTr tr[4];
        int ntr = 0, nnear = 0;
        auto see = [&](uint64_t id, uint64_t kh, const float* c0, const float* v, float w) {   // r10: + view-space v
            const bool nearO = w < g_egoM;
            if (nearO) ++nnear;
            EgoSee(id, kh, c0, v, w, W, H, U, (trace && nearO && ntr < 4) ? &tr[ntr] : nullptr);
            if (trace && nearO && ntr < 4) ++ntr;
        };
        for (int i = 0; i < n; ++i) see(e[i].id, e[i].kh, e[i].c0, e[i].v, e[i].w);
        for (int k = 0; k < nr; ++k)                    // r11: camera-attached draws are never ego sightings
            if (r[k].why != kRejVeto && r[k].why != kRejCamOrigin) see(r[k].id, r[k].kh, r[k].c0, r[k].v, r[k].w);
        EgoAge(U);
        if (ntr) {
            ++g_egoTraceN;
            char s[2048];
            int o = snprintf(s, sizeof(s), "MV objects: ego trace #%d (readback %llu, frame %llu): %d near sighting(s), "
                             "%d copies tracked, %d flagged, %d ego identities |", g_egoTraceN, (unsigned long long)U,
                             (unsigned long long)frame, nnear, g_nEgo, g_egoSetN, g_nEgoId);
            for (int t = 0; t < ntr && o > 0 && o < (int)sizeof(s); ++t) {
                const EgoTr& T = tr[t];
                // v0.9.0 r10: positions, box and distance in METRES of view space (r9: px-equivalent)
                o += snprintf(s + o, sizeof(s) - (size_t)o, " [key %016llx id %016llx -> copy %d (%s, d %.3f m) | view "
                              "pos %.3f %.3f %.3f m (projected origin %.0f, %.0f px) w %.3f | n %d box x %.3f..%.3f y "
                              "%.3f..%.3f z %.3f..%.3f m | flag %d]", (unsigned long long)T.kh,
                              (unsigned long long)T.id, T.entry,
                              T.what, (double)T.d, (double)T.p[0], (double)T.p[1], (double)T.p[2], (double)T.sx,
                              (double)T.sy, (double)T.w, T.n, (double)T.lo[0], (double)T.hi[0], (double)T.lo[1],
                              (double)T.hi[1], (double)T.lo[2], (double)T.hi[2], (int)T.flag);
            }
            Log("%s", s);
        }
        if (g_nEgoId) {
            int out = 0;
            for (int m = 0; m < g_nMem; ++m) {
                if (EgoFlagged(g_mem[m].id)) { NoteRelease(g_mem[m]); ++g_st.egoReleased; continue; }
                g_mem[out++] = g_mem[m];
            }
            const bool released = out != g_nMem;
            g_nMem = out;
            out = 0;
            for (int w = 0; w < g_nWait; ++w) if (!EgoFlagged(g_wait[w].id)) g_wait[out++] = g_wait[w];
            g_nWait = out;
            out = 0;
            for (int q = 0; q < g_nFw; ++q) if (!EgoFlagged(g_fw[q].id)) g_fw[out++] = g_fw[q];
            g_nFw = out;
            out = 0;                                    // v0.9.0 r10: attach candidates
            for (int q = 0; q < g_nAw; ++q) if (!EgoFlagged(g_aw[q].id)) g_aw[out++] = g_aw[q];
            g_nAw = out;
            if (released) RebuildLut();
        }
    }
    // S. v0.9.0 r11 STATIC CLASS (round-9 log + video: road and verge magenta-tinted, ids of 39-52 static members at
    //    10-196 m, released 173-585 per window): this readback's sightings go into the per-identity history -- moving
    //    entries and rejects of non-static / unknown motion first (dup / pairing / ambiguous / own truck / off-screen
    //    candidates), then the static ones (camera noise, a tagged non-candidate off screen) and every other drawn
    //    identity (paired, no candidate: within 0.5 px of the camera R). Veto records say nothing about the draw's own
    //    motion and camera-attached draws are never anything: no mark from them (a drawn one is a static sighting).
    //    Members that are static class now are released (an id left without members is freed in step 8).
    {
        for (int i = 0; i < n; ++i) StMark(e[i].id, U, 1);
        for (int k = 0; k < nr; ++k) {
            const Reject& rj = r[k];
            if (rj.why == kRejVeto || rj.why == kRejCamOrigin) continue;
            const bool st = rj.why == kRejNoise || (rj.drawnOnly && rj.why == kRejOffscreen);
            StMark(rj.id, U, st ? 0 : 1);
        }
        for (int k = 0; k < nDrawn; ++k) StMark(drawn[k], U, 2);
        if ((U & 31u) == 0 || g_stN >= kStLive - 64) StCompact(U);
        int out = 0;
        for (int m = 0; m < g_nMem; ++m) {
            if (StClass(g_mem[m].id, U)) { NoteRelease(g_mem[m]); ++g_st.staticReleased; continue; }
            g_mem[out++] = g_mem[m];
        }
        if (out != g_nMem) { g_nMem = out; RebuildLut(); }
    }
    WMapBuild();
    BuildSlotLists();                                   // v0.9.0 r6 (NearMember)
    // 0. v0.9.0 r6 STICKY MEMBERSHIP: a member drawn (and paired) in the read-back pass is SEEN, moving or not
    for (int k = 0; k < nDrawn; ++k) {
        const int mi = MemberOf(drawn[k]);
        if (mi >= 0) { seen[mi] = true; continue; }
        if (g_nWait) { const int wi = WMapFind(drawn[k]); if (wi >= 0) waitDrawn[wi] = true; }
    }
    // importance order: biggest meshes first (vehicle bodies / trailers anchor the ids, wheels join them)
    // v0.9.0 r4: spinning parts AFTER every non-spinning one (they can only join an id, never take one; at distant LODs a
    // wheel mesh can have more indices than the body)
    int ord[256];
    if (n > 256) n = 256;
    for (int i = 0; i < n; ++i) ord[i] = i;
    auto spinOf = [&](int i) { return e[i].spin > kSpinMax; };
    auto before = [&](int a, int b) {                  // true: a goes before b
        const bool sa = spinOf(a), sb = spinOf(b);
        if (sa != sb) return !sa;
        return e[a].ic > e[b].ic;
    };
    for (int i = 1; i < n; ++i) {                     // insertion sort, n <= 256
        const int v = ord[i];
        int j = i - 1;
        while (j >= 0 && before(v, ord[j])) { ord[j + 1] = ord[j]; --j; }
        ord[j + 1] = v;
    }
    // fresh[s] = id s got its reference R from this readback (r3: from its anchor)
    bool fresh[ObjRecord::kIds] = {};
    // 1. existing members seen moving: coherence (r2; r6: footprint floor), state
    for (int k = 0; k < n; ++k) {
        const Moving& m = e[ord[k]];
        const int mi = MemberOf(m.id);
        if (mi < 0) continue;
        Member& M = g_mem[mi];
        seen[mi] = listed[mi] = true;
        movIdx[mi] = ord[k];
        M.misses = 0;
        // v0.9.0 r10: the GPU lists only movers with an on-screen origin; one without is released (defensive)
        if (!OnScreen(m.c0)) { if (!offRel[mi]) { offRel[mi] = true; ++g_st.offscreenReleased; } continue; }
        c0Now[mi] = true;
        M.attach = false;                               // r10: a trusted mover of its own now: a full member
        SlotInfo& S = g_slotInfo[M.slot];
        // v0.9.0 r3: a follower with a trusted pair of its own now (no longer a duplicate) becomes a full member; its
        // stored displacement is not its own (no coherence test on this first sighting)
        const int c = M.follower ? -1 : Coherent(M.d, M.frame, m.d, frame, m.w, W, H);
        coh[mi] = c;
        M.follower = false;
        M.spin = m.spin > kSpinMax;                     // v0.9.0 r4: re-judged every sighting
        if (c == 0) {                                   // v0.9.0 r6: a REAL discontinuity (> the footprint floor)
            ++g_st.incoherent; ++S.jumps;
            jumped[mi] = true;
            if (++M.bad >= kBadToDrop && !drop[mi]) { drop[mi] = true; ++g_st.dropped; NoteRej(m.id); }
        } else {
            M.bad = 0;
        }
        memcpy(M.d, m.d, sizeof(M.d)); M.frame = frame; M.w = m.w; M.dup = m.dup;
        M.ic = m.ic;
        memcpy(M.c0, m.c0, sizeof(M.c0)); M.hasC0 = true;   // v0.9.0 r6
    }
    // 2. v0.9.0 r6 REFERENCE R per id: the anchor when it was seen moving coherently this readback (not spinning, not a
    //    follower, no proof pending against it), else the biggest member that was (it becomes the anchor). r2..r4: only
    //    the anchor refreshed its id, and a sub-threshold anchor (far car) left its spinning wheels nothing to join.
    {
        auto refOk = [&](int mi, int s) {
            if (mi < 0 || mi >= g_nMem || movIdx[mi] < 0 || jumped[mi] || drop[mi] || offRel[mi]) return false;
            const Member& M = g_mem[mi];
            return M.slot == s && !M.spin && !M.follower && !M.attach && M.div == 0;
        };
        int ref[ObjRecord::kIds];
        for (int s = 0; s < ObjRecord::kIds; ++s) ref[s] = -1;
        for (int s = 1; s < ObjRecord::kIds; ++s) {
            if (!g_slotUsed[s]) continue;
            const int a = MemberOf(g_slotAnchor[s]);
            if (refOk(a, s)) ref[s] = a;
        }
        for (int mi = 0; mi < g_nMem; ++mi) {
            const int s = g_mem[mi].slot;
            if (s < 1 || s >= ObjRecord::kIds || !g_slotUsed[s] || !refOk(mi, s)) continue;
            if (ref[s] < 0 || (g_mem[ref[s]].id != g_slotAnchor[s] && g_mem[mi].ic > g_mem[ref[s]].ic)) ref[s] = mi;
        }
        for (int s = 1; s < ObjRecord::kIds; ++s) {
            if (ref[s] < 0) continue;
            const Member& M = g_mem[ref[s]];
            const Moving& m = e[movIdx[ref[s]]];
            memcpy(g_slotR[s], m.R, sizeof(g_slotR[s]));
            memcpy(g_slotC0[s], m.c0, sizeof(g_slotC0[s]));
            g_slotRUpd[s] = U;
            fresh[s] = true;
            if (g_slotAnchor[s] != M.id) {             // r6: the anchor role passes to the member that gave the reference
                g_slotAnchor[s] = M.id; g_slotAnchorIc[s] = M.ic;
                ++g_slotInfo[s].reanchors;
                g_slotSolid[s] = 0;
            }
            if (coh[ref[s]] == 1) ++g_slotSolid[s];     // v0.9.0 r4 (follower gate)
            SlotInfo& S = g_slotInfo[s];
            const float sp = Len3(m.d);
            S.w = m.w; S.speed = sp; S.dup = m.dup;
            if (!S.spSeen) { S.spMin = S.spMax = sp; S.spSeen = true; }
            else { if (sp < S.spMin) S.spMin = sp; if (sp > S.spMax) S.spMax = sp; }
        }
    }
    const int maxId = EffMax();
    auto recent = [&](int s) { return s >= 1 && s < ObjRecord::kIds && g_slotUsed[s] && U - g_slotRUpd[s] <= (uint64_t)kRefAge; };
    // v0.9.0 r6 proof bookkeeping: target t (> 0 = another id, -1 = its own motion) in kProveRb judged readbacks in a row
    auto prove = [&](Member& M, int t) -> bool {
        if (M.target == t) ++M.div; else { M.target = t; M.div = 1; }
        return M.div >= kProveRb;
    };
    auto fits = [&](Member& M) { M.div = 0; M.target = 0; };
    // 3. v0.9.0 r6 MEMBERSHIP RE-CHECK (r4: > 1.5 px in 2 readbacks = out). A member seen moving (its own trusted R) is
    //    judged against its id's reference R of THIS readback (an older one differs by the camera's acceleration: no
    //    verdicts on it): fit (<= max(1.5 px, 35 % of its correction)),
    //    gross misfit (> max(4 px, 50 %)) or in between (no verdict, it stays). A gross misfit that fits another id
    //    clearly better moves there after kProveRb readbacks in a row; a non-spinning one that fits no id takes a free id
    //    with its own motion; with no free id it stays unless the id's R is worse than the camera R (then camera R).
    for (int k = 0; k < n; ++k) {
        const Moving& m = e[ord[k]];
        const int mi = MemberOf(m.id);
        if (mi < 0 || drop[mi] || offRel[mi] || movIdx[mi] != ord[k]) continue;
        Member& M = g_mem[mi];
        const int s = M.slot;
        if (g_slotAnchor[s] == m.id && fresh[s]) { fits(M); continue; }   // it IS the reference
        if (!fresh[s]) continue;
        const float corr = CorrPx(m, W, H);
        const float tolFit = kMemberPx > kMemberRel * corr ? kMemberPx : kMemberRel * corr;
        const float gross = kGrossPx > kGrossRel * corr ? kGrossPx : kGrossRel * corr;
        const float dOwn = OriginPx(m.R, g_slotR[s], m.c0, W, H);
        // v0.9.0 r7: no screen position for the origin (behind the camera: a passing trailer's origin, now a mover
        // within mv_objects_ego_m) = no verdict. r6 read the 1e30 as a gross misfit: a rigid trailer left its id.
        // v0.9.0 r10: the GPU lists only movers with an on-screen origin, so 1e30 = the id's motion puts it behind the
        // camera: no verdict was the hole the static world stayed in through -- a release now
        if (!(dOwn < 1.0e29f)) { offRel[mi] = true; ++g_st.offscreenReleased; continue; }
        if (dOwn <= tolFit) { fits(M); continue; }
        if (dOwn <= gross) continue;                    // between fit and gross: no verdict, stays
        ++g_st.diverged;
        int bt = 0; float bd = 1e30f;
        for (int t = 1; t <= maxId; ++t) {
            if (t == s || !fresh[t] || !NearMember(t, m.c0)) continue;
            const float d = OriginPx(m.R, g_slotR[t], m.c0, W, H);
            if (d <= tolFit && dOwn >= kBetter * d + 1.0f && d < bd) { bd = d; bt = t; }
        }
        const int tgt = bt ? bt : ((!M.spin && !M.follower) ? -1 : 0);
        if (!tgt) { fits(M); continue; }                // a spinning misfit that fits no id: stays (no proof)
        if (!prove(M, tgt)) continue;
        if (tgt > 0) { M.slot = tgt; fits(M); ++g_st.moved; continue; }
        bool q = false;
        const int ns = FreeSlot(&q);
        if (ns) { TakeSlot(ns, m); fresh[ns] = true; M.slot = ns; fits(M); ++g_st.ownId; continue; }
        if (q) ++g_st.quarantined; else ++g_st.overBudget;
        if (dOwn > corr) { drop[mi] = true; ++g_st.leftToCam; NoteRej(m.id); }   // the id's R is worse than the camera R
    }
    // 4. new movers: hysteresis with coherence (r2: mv_objects_hits readbacks whose displacement agrees; r6: one forgiven
    //    readback in between, see kGapPx), then join an id moving the same way or take a free one
    //    v0.9.0 r7 FAST JOIN: before the hits are reached a mover may already JOIN an existing id -- a non-spinning one
    //    whose R equals the id's (kTightFrob: the same rigid body, float noise) next to one of the id's members, a spinning
    //    one whose hub the id's FRESH reference R (this readback) moves within kOriginPx, next to a member. A part that
    //    lost its tag (a drop, a LOD switch = a new identity) was untagged for 2-3 readbacks (round-5 video: a passing
    //    truck's wheel orange / magenta / cyan frame by frame) although its R matched its vehicle's exactly. Taking a NEW
    //    id still needs mv_objects_hits coherent readbacks (that is what keeps mis-paired static meshes out).
    //    v0.9.0 r9: the NON-SPINNING fast join is gone (round-7 log + video: road, verges and trees tagged magenta; one
    //    id with 282 members at 659 m; fast-joined +1063..+2545 per window, released about as many). A static draw that
    //    is a candidate for ONE frame (camera-R float noise in a turn / under acceleration: > 0.5 px, trusted pairing)
    //    has R = the camera R; a far / slow vehicle's R, or an id a static intruder already holds, differs from that by
    //    far less than kTightFrob, so the first sighting joined -- and sticky membership kept it. Without the fast join
    //    it needs mv_objects_hits coherent readbacks like any new mover (r6: a one-frame noise candidate is not seen
    //    again / not coherent). A SPINNING part (a real mover: its axes turn) still joins at once when the id's FRESH
    //    reference R moves its hub within kOriginPx next to a member.
    for (int k = 0; k < n; ++k) {
        const Moving& m = e[ord[k]];
        if (MemberOf(m.id) >= 0) continue;              // (members appended below are not in the LUT: ids are unique per pass)
        if (EgoFlagged(m.id)) continue;                 // v0.9.0 r8: own truck (a sighting from before the GPU knew)
        if (!OnScreen(m.c0)) continue;                  // v0.9.0 r10: the GPU gate (defensive)
        if (StClass(m.id, U)) { ++g_st.staticBlocked; continue; }   // v0.9.0 r11: static class never founds / joins
        // v0.9.0 r10 MIN OWN MOTION: a non-spinning sighting below mv_objects_min_px is no evidence -- it counts as
        // drawn sub-threshold (r6 hysteresis: one such readback is forgiven when the last correction was small), it
        // neither starts nor extends a run (round 8: static draws founded / joined ids from camera-noise candidates)
        if (!(m.spin > kSpinMax) && !(m.dev >= g_minPx)) {
            ++g_st.subMinPx;
            const int ws = WMapFind(m.id);
            if (ws >= 0) waitDrawn[ws] = true;
            continue;
        }
        int wi = WMapFind(m.id);
        if (wi < 0) {
            if (g_nWait >= kMaxWait) continue;
            wi = g_nWait++;
            g_wait[wi].id = m.id; g_wait[wi].hits = 0; g_wait[wi].frame = 0; g_wait[wi].corr = 0.0f;
            WMapPut(m.id, wi);
        }
        Wait& Wt = g_wait[wi];
        waitSeen[wi] = true;
        Wt.age = 0;
        const int c = Wt.hits > 0 ? Coherent(Wt.d, Wt.frame, m.d, frame, m.w, W, H) : -1;
        if (c == 1) ++Wt.hits;
        else {
            if (c == 0) { ++g_st.incoherent; NoteRej(m.id); }
            Wt.hits = 1;                                // this sighting starts a new run
        }
        memcpy(Wt.d, m.d, sizeof(Wt.d)); Wt.frame = frame;
        Wt.corr = CorrPx(m, W, H);                      // v0.9.0 r6
        const bool spinning = m.spin > kSpinMax;        // v0.9.0 r4
        int best = 0;
        bool fast = false;                              // v0.9.0 r7
        if (Wt.hits < g_hits) {
            if (Wt.hits < 1 || !spinning) continue;     // v0.9.0 r9: only a spinning part joins before the hits
            float bestPx = kOriginPx;
            for (int s = 1; s <= maxId; ++s) {
                if (!g_slotUsed[s] || !fresh[s] || !NearMember(s, m.c0)) continue;
                const float d = OriginPx(m.R, g_slotR[s], m.c0, W, H);
                if (d <= bestPx) { bestPx = d; best = s; }
            }
            if (!best) continue;
            fast = true;
        }
        float bestF = kTightFrob;
        if (!spinning && !best)                         // a spinning R equals no vehicle's R
            for (int s = 1; s <= maxId; ++s)
                if (g_slotUsed[s]) { const float f = Frob16(m.R, g_slotR[s]); if (f < bestF) { bestF = f; best = s; } }
        if (!best) {
            float bestPx = 1e30f;
            for (int s = 1; s <= maxId; ++s) {
                // v0.9.0 r3: only next to the id (r6: any member's origin). r4: a spinning part only joins an id whose
                // anchor R is from THIS readback; r6: a reference R up to kRefAge readbacks old (1.5 px when not fresh)
                if (!g_slotUsed[s] || !NearMember(s, m.c0)) continue;
                const uint64_t age = U - g_slotRUpd[s];
                if (spinning && age > (uint64_t)kRefAge) continue;
                const float tol = (!spinning || age == 0) ? kOriginPx : kOriginPxOld;
                const float d = OriginPx(m.R, g_slotR[s], m.c0, W, H);
                if (d <= tol && d < bestPx) { bestPx = d; best = s; }
            }
        }
        if (!best && !spinning) {
            // v0.9.0 r6: an id without a recent reference (its body not moving / replaced by another LOD) whose spinning
            // members seen in THIS readback have their hubs where this mover's motion puts them: the new body of their
            // vehicle -- it joins and gives the id its reference
            for (int s = 1; s <= maxId && !best; ++s) {
                if (!g_slotUsed[s] || recent(s) || !NearMember(s, m.c0)) continue;
                for (int mi = 0; mi < g_nMem && !best; ++mi) {
                    const Member& Sp = g_mem[mi];
                    if (Sp.slot != s || !Sp.spin || movIdx[mi] < 0) continue;
                    const Moving& sm = e[movIdx[mi]];
                    if (OriginPx(sm.R, m.R, sm.c0, W, H) <= kOriginPx) best = s;
                }
            }
            if (best) {
                memcpy(g_slotR[best], m.R, sizeof(g_slotR[best]));
                memcpy(g_slotC0[best], m.c0, sizeof(g_slotC0[best]));
                g_slotRUpd[best] = U; fresh[best] = true;
                g_slotAnchor[best] = m.id; g_slotAnchorIc[best] = m.ic;
                ++g_slotInfo[best].reanchors; g_slotSolid[best] = 0;
            }
        }
        if (best) { ++g_st.joined; if (spinning) ++g_st.spinJoined; if (fast) ++g_st.fastJoined; }
        else if (spinning) { ++g_st.spinWait; continue; }   // v0.9.0 r4: never takes an id; waits for its vehicle's id
        else {
            bool quarantined = false;                   // v0.9.0 r4: a freed id rests kQuarantine frames
            best = FreeSlot(&quarantined);
            if (!best) { if (quarantined) ++g_st.quarantined; else ++g_st.overBudget; continue; }
            TakeSlot(best, m);
            fresh[best] = true;                         // v0.9.0 r3
        }
        if (g_nMem >= kMaxMembers) { ++g_st.overBudget; continue; }
        Member& M = g_mem[g_nMem];
        M.id = m.id; M.slot = best; M.misses = 0; M.bad = 0; M.ic = m.ic; M.div = 0; M.target = 0;
        memcpy(M.d, m.d, sizeof(M.d)); M.frame = frame; M.w = m.w; M.dup = m.dup; M.follower = false;
        M.spin = spinning;                              // v0.9.0 r4
        memcpy(M.c0, m.c0, sizeof(M.c0)); M.hasC0 = true;   // v0.9.0 r6
        M.attach = false; M.attAnchor = 0; M.attMiss = 0;   // v0.9.0 r10
        seen[g_nMem] = listed[g_nMem] = c0Now[g_nMem] = true;
        offRel[g_nMem] = attRel[g_nMem] = false;
        ++g_nMem;
        Wt.hits = -1000000;                             // removed below
    }
    // 5. rejected draws (dup / pairing / ambiguous / own truck: no trusted pair of their own this frame).
    //    v0.9.0 r6 members: SEEN, never dropped for it (r2..r4: 2 rejects in a row dropped a member -- round 4: dropped
    //    44..2133 per window, re-joined right after: spin-joined +1507). With a recent reference R a member's hub is
    //    judged like the r3 follower test (R * origin vs its nearest / own previous partner; reference R of THIS readback
    //    only): a gross misfit that fits
    //    another id clearly better moves there after kProveRb readbacks (identity swap of two identical vehicles).
    //    Non-members: a waiting one restarts its hits; r3 follower rule (mv_objects_followers = 1, solid ids only).
    //    v0.9.0 r10: reason 8 (origin off screen; for a member also reported when it was no candidate) releases a
    //    member and restarts a waiting run; an attached member gets no hub verdict (step 5b judges it); a hub that the
    //    id's motion puts behind the camera is a release (r7: no verdict).
    bool fwSeen[kMaxFWait] = {};
    bool solidBlocked = false;
    auto followSlot = [&](const Reject& rj, int only) -> int {
        int bs = 0;
        float bd = kOriginPx;
        for (int s = (only ? only : 1); s <= (only ? only : maxId); ++s) {
            if (s < 1 || s >= ObjRecord::kIds || !g_slotUsed[s] || !fresh[s] || !NearAnchor(s, rj.c0)) continue;
            float d = PrevPx(g_slotR[s], rj.c0, rj.a[0], W, H);
            if (rj.ownOk) { const float d2 = PrevPx(g_slotR[s], rj.c0, rj.a[1], W, H); if (d2 < d) d = d2; }
            if (d < bd) {
                if (g_slotSolid[s] < kSolidRb) { solidBlocked = true; continue; }   // v0.9.0 r4
                bd = d; bs = s;
            }
        }
        return bs;
    };
    auto hubPx = [&](const Reject& rj, int s) {         // v0.9.0 r6: the r3 follower distance against id s's reference R
        float d = PrevPx(g_slotR[s], rj.c0, rj.a[0], W, H);
        if (rj.ownOk) { const float d2 = PrevPx(g_slotR[s], rj.c0, rj.a[1], W, H); if (d2 < d) d = d2; }
        return d;
    };
    for (int k = 0; k < nr; ++k) {
        const Reject& rj = r[k];
        // only with a COMPLETE candidate set (key drawn <= kFollowMaxDup times in this and the previous pass): then a
        // static copy always finds itself among the candidates (dev ~0, never a reject), so a soft reject really moved.
        // With more draws (grass fields) the own copy may lie outside the 8 candidates and a coincidental 1-px match is
        // likely in a dense field (synthetic test: 40 clumps followed a car).
        const bool soft = g_followers &&                // v0.9.0 r4: follower rule off by default
                          (rj.why == kRejDup || rj.why == kRejPair || rj.why == kRejAmbig) && rj.dup <= kFollowMaxDup;
        solidBlocked = false;
        const int mi = MemberOf(rj.id);
        if (rj.why == kRejVeto) {                       // v0.9.0 r6: a VETO record (CSVeto), not a reject of CSMain
            if (mi < 0 || drop[mi] || offRel[mi] || movIdx[mi] >= 0) continue;   // moving member: own motion (step 3)
            // drawn without moving > 0.5 px (or rejected), yet its id's motion puts it grossly off: it does not move with
            // its id (a stopped car that had joined a moving one). kProveRb such readbacks: out to the camera R (it can
            // qualify again as a mover of its own)
            Member& M = g_mem[mi];
            if (prove(M, -2)) { drop[mi] = true; ++g_st.vetoOut; ++g_st.leftToCam; NoteRej(rj.id); }
            continue;
        }
        if (mi >= 0) {
            Member& M = g_mem[mi];
            seen[mi] = listed[mi] = true;               // evaluated (not a "miss")
            if (drop[mi] || offRel[mi]) continue;
            if (rj.why == kRejOffscreen) { offRel[mi] = true; ++g_st.offscreenReleased; continue; }   // v0.9.0 r10
            if (rj.why == kRejCamOrigin) { offRel[mi] = true; ++g_st.camOriginReleased; continue; }  // v0.9.0 r11
            M.w = rj.w; M.dup = rj.dup;
            memcpy(M.c0, rj.c0, sizeof(M.c0)); M.hasC0 = true;
            c0Now[mi] = true;                           // v0.9.0 r10 (attach: the origins of this readback)
            if (M.attach) continue;                     // v0.9.0 r10: judged by the attach rule (step 5b)
            const int s = M.slot;
            // own truck (r7: near AND camera-locked) / no fresh reference: seen, no verdict. v0.9.0 r9: camera noise
            // too (it moved like the world in this frame: a stopped member; the CSVeto records judge it against its id)
            if (rj.why == kRejEgo || rj.why == kRejNoise || !fresh[s]) continue;
            const float corr = rj.dev;
            const float tolFit = kMemberPx > kMemberRel * corr ? kMemberPx : kMemberRel * corr;
            const float gross = kGrossPx > kGrossRel * corr ? kGrossPx : kGrossRel * corr;
            const float dOwn = hubPx(rj, s);
            // v0.9.0 r7: hub not on screen (behind the camera): no verdict -- r10: a release (see step 3)
            if (!(dOwn < 1.0e29f)) { offRel[mi] = true; ++g_st.offscreenReleased; continue; }
            if (dOwn <= tolFit) { fits(M); if (M.follower) ++g_st.followKept; continue; }
            if (dOwn <= gross) continue;
            ++g_st.diverged;
            int bt = 0; float bd = 1e30f;
            for (int t = 1; t <= maxId; ++t) {
                if (t == s || !fresh[t] || !NearMember(t, rj.c0)) continue;
                const float d = hubPx(rj, t);
                if (d <= tolFit && dOwn >= kBetter * d + 1.0f && d < bd) { bd = d; bt = t; }
            }
            if (!bt) continue;                          // fits no id: no proof, stays
            if (prove(M, bt)) { M.slot = bt; fits(M); ++g_st.moved; }
            continue;
        }
        if (EgoFlagged(rj.id)) { NoteRej(rj.id); continue; }   // v0.9.0 r8: the own truck never follows an id
        const int wi = WMapFind(rj.id);
        if (wi >= 0) { g_wait[wi].hits = 0; waitSeen[wi] = true; g_wait[wi].age = 0; }
        // v0.9.0 r9: static (camera noise): its run restarts, no follower, not "rejected" in the debug view; r10: the
        // same for an origin off screen
        if (rj.why == kRejNoise || rj.why == kRejOffscreen || rj.why == kRejCamOrigin) continue;   // r11: + camera
        if (soft && StClass(rj.id, U)) { ++g_st.staticBlocked; NoteRej(rj.id); continue; }       // r11: static class
        const int fs = soft ? followSlot(rj, 0) : 0;
        if (solidBlocked && !fs) ++g_st.solidBlocked;
        if (!fs) { NoteRej(rj.id); continue; }
        int fi = -1;
        for (int q = 0; q < g_nFw; ++q) if (g_fw[q].id == rj.id) { fi = q; break; }
        if (fi < 0) {
            if (g_nFw >= kMaxFWait) continue;
            fi = g_nFw++;
            g_fw[fi].id = rj.id; g_fw[fi].slot = 0; g_fw[fi].hits = 0;
        }
        FWait& F = g_fw[fi];
        fwSeen[fi] = true;
        F.age = 0;
        if (F.slot == fs) ++F.hits; else { F.slot = fs; F.hits = 1; }
        if (F.hits < g_hits) continue;
        if (g_nMem >= kMaxMembers) { ++g_st.overBudget; continue; }
        Member& M = g_mem[g_nMem];
        M.id = rj.id; M.slot = fs; M.misses = 0; M.bad = 0; M.ic = 0.0f; M.div = 0; M.target = 0;
        M.d[0] = M.d[1] = M.d[2] = 0.0f; M.frame = frame; M.w = rj.w; M.dup = rj.dup; M.follower = true; M.spin = false;
        memcpy(M.c0, rj.c0, sizeof(M.c0)); M.hasC0 = true;   // v0.9.0 r6
        M.attach = false; M.attAnchor = 0; M.attMiss = 0;   // v0.9.0 r10
        seen[g_nMem] = listed[g_nMem] = c0Now[g_nMem] = true;
        ++g_nMem;
        ++g_st.followJoined;
        F.hits = -1000000;                              // removed below
    }
    {                                                   // follower candidates: joined ones and stale ones go
        int o = 0;
        for (int q = 0; q < g_nFw; ++q) {
            if (g_fw[q].hits < 0) continue;
            if (!fwSeen[q]) continue;                   // strictly consecutive (as the wait list, r3)
            g_fw[o++] = g_fw[q];
        }
        g_nFw = o;
    }
    // 5b. v0.9.0 r10 ATTACH (user report: an AI vehicle's licence plate untagged while its body is -- the plate
    //    ghosts). A plate / mirror / lamp is its own draw on its vehicle's world matrix; one plate mesh on every
    //    vehicle is a duplicate (dup / pairing / ambiguous: never a member of its own). Its ORIGIN is its vehicle's
    //    origin: a reject of those reasons whose origin coincides with a non-spinning member's origin of this readback
    //    (projected <= kAttachPx, view depth within kAttachRelW) in mv_objects_hits readbacks in a row (with the same
    //    member) joins that member's id as an ATTACHED member (never anchors / represents). An attached member whose
    //    origin and its anchor's are both listed but no longer coincide in kAttachMissRb readbacks in a row is released
    //    (and when its anchor leaves, 6b).
    if (g_attach) {
        auto coincide = [&](const float* a, const float* b) {
            if (!(a[3] > 0.05f) || !(b[3] > 0.05f)) return false;
            const float dx = (a[0] / a[3] - b[0] / b[3]) * 0.5f * W, dy = (a[1] / a[3] - b[1] / b[3]) * 0.5f * H;
            const float dw = a[3] - b[3];
            return dx * dx + dy * dy <= kAttachPx * kAttachPx && std::fabs(dw) <= kAttachRelW * a[3];
        };
        auto findMem = [&](uint64_t id) {
            for (int m = 0; m < g_nMem; ++m) if (g_mem[m].id == id) return m;
            return -1;
        };
        // (a) attached members: their anchor's id, coincidence while both origins are listed this readback
        for (int m = 0; m < g_nMem; ++m) {
            Member& M = g_mem[m];
            if (!M.attach || drop[m] || offRel[m] || attRel[m]) continue;
            const int a = findMem(M.attAnchor);
            if (a < 0 || drop[a] || offRel[a]) { attRel[m] = true; continue; }   // its anchor leaves (else 6b)
            M.slot = g_mem[a].slot;
            if (!c0Now[m] || !c0Now[a]) continue;      // not judged this readback
            if (coincide(M.c0, g_mem[a].c0)) M.attMiss = 0;
            else if (++M.attMiss >= kAttachMissRb) attRel[m] = true;
        }
        // (b) new attachments: the anchors listed this readback
        static int anc[kMaxMembers];
        int na = 0;
        for (int m = 0; m < g_nMem; ++m) {
            const Member& M = g_mem[m];
            if (c0Now[m] && !drop[m] && !offRel[m] && !attRel[m] && !M.spin && !M.follower && !M.attach &&
                M.slot >= 1 && M.slot < ObjRecord::kIds && g_slotUsed[M.slot]) anc[na++] = m;
        }
        bool awSeen[kMaxAWait] = {};
        g_attD.anchors += (uint64_t)na;                 // v0.9.0 r11 diagnostics (behaviour unchanged)
        for (int k = 0; k < nr; ++k) {
            const Reject& rj = r[k];
            if (rj.why == kRejVeto || rj.drawnOnly) continue;
            ++g_attD.rejects;
            if (rj.why != kRejDup && rj.why != kRejPair && rj.why != kRejAmbig) {
                if (rj.why > 0 && rj.why < kRejKinds) ++g_attD.byWhy[rj.why];
                continue;
            }
            ++g_attD.soft;
            if (MemberOf(rj.id) >= 0) { ++g_attD.member; continue; }
            if (EgoFlagged(rj.id)) { ++g_attD.ego; continue; }
            if (!OnScreen(rj.c0)) { ++g_attD.offscreen; continue; }
            if (!na) { ++g_attD.noAnchor; continue; }
            int bm = -1;
            float bd = 1e30f, nearPx2 = 1e30f, nearW = 0.0f;   // r11 diagnostics: the nearest listed anchor at all
            for (int q = 0; q < na; ++q) {
                const float* b = g_mem[anc[q]].c0;
                if (b[3] > 0.05f && rj.c0[3] > 0.05f) {
                    const float ex = (rj.c0[0] / rj.c0[3] - b[0] / b[3]) * 0.5f * W;
                    const float ey = (rj.c0[1] / rj.c0[3] - b[1] / b[3]) * 0.5f * H;
                    if (ex * ex + ey * ey < nearPx2) {
                        nearPx2 = ex * ex + ey * ey;
                        nearW = std::fabs(rj.c0[3] - b[3]);
                    }
                }
                if (!coincide(rj.c0, b)) continue;
                const float dx = (rj.c0[0] / rj.c0[3] - b[0] / b[3]) * 0.5f * W;
                const float dy = (rj.c0[1] / rj.c0[3] - b[1] / b[3]) * 0.5f * H;
                const float d = dx * dx + dy * dy;
                if (d < bd) { bd = d; bm = anc[q]; }
            }
            if (bm < 0) {
                if (nearPx2 > kAttachPx * kAttachPx) {
                    ++g_attD.farN;
                    const float np = std::sqrt(nearPx2);
                    ++g_attD.farHist[np < 2.0f ? 0 : (np < 8.0f ? 1 : (np < 32.0f ? 2 : 3))];
                } else if (nearW > kAttachRelW * rj.c0[3]) {
                    ++g_attD.wMis;
                }
                continue;
            }
            ++g_attD.coincide;
            if (StClass(rj.id, U)) { ++g_attD.staticCl; ++g_st.staticBlocked; continue; }   // r11: static class
            int ai = -1;
            for (int q = 0; q < g_nAw; ++q) if (g_aw[q].id == rj.id) { ai = q; break; }
            if (ai < 0) {
                if (g_nAw >= kMaxAWait) continue;
                ai = g_nAw++;
                g_aw[ai].id = rj.id; g_aw[ai].anchor = 0; g_aw[ai].hits = 0;
            }
            AWait& A = g_aw[ai];
            awSeen[ai] = true;
            if (A.anchor == g_mem[bm].id) ++A.hits; else { A.anchor = g_mem[bm].id; A.hits = 1; }
            if (A.hits < g_hits) continue;
            if (g_nMem >= kMaxMembers) { ++g_st.overBudget; continue; }
            Member& M = g_mem[g_nMem];
            M.id = rj.id; M.slot = g_mem[bm].slot; M.misses = 0; M.bad = 0; M.ic = 0.0f; M.div = 0; M.target = 0;
            M.d[0] = M.d[1] = M.d[2] = 0.0f; M.frame = frame; M.w = rj.w; M.dup = rj.dup; M.follower = false;
            M.spin = false;
            memcpy(M.c0, rj.c0, sizeof(M.c0)); M.hasC0 = true;
            M.attach = true; M.attAnchor = g_mem[bm].id; M.attMiss = 0;
            seen[g_nMem] = listed[g_nMem] = c0Now[g_nMem] = true;
            drop[g_nMem] = offRel[g_nMem] = attRel[g_nMem] = false;
            ++g_nMem;
            ++g_st.attached;
            A.hits = -1000000;                          // removed below
        }
        int o = 0;                                      // strictly consecutive, joined ones go
        for (int q = 0; q < g_nAw; ++q) if (g_aw[q].hits >= 0 && awSeen[q]) g_aw[o++] = g_aw[q];
        g_nAw = o;
    } else {
        g_nAw = 0;
    }
    // 6. members: dropped (2 real jumps in a row / proven out to the camera R) or not DRAWN for kMissToDrop readbacks
    //    (v0.9.0 r6; r2..r4: not seen moving)
    int out = 0;
    for (int m = 0; m < g_nMem; ++m) {
        if (drop[m]) { NoteRej(g_mem[m].id); continue; }
        if (offRel[m] || attRel[m]) { NoteRelease(g_mem[m]); continue; }   // v0.9.0 r10 (no debug-view mark)
        if (seen[m]) { g_mem[m].misses = 0; if (!listed[m]) ++g_st.seenStill; }
        else if (++g_mem[m].misses >= kMissToDrop) { NoteRelease(g_mem[m]); continue; }
        g_mem[out++] = g_mem[m];
    }
    g_nMem = out;
    // 6b. v0.9.0 r10: an attached member whose anchor member left (in any way) leaves with it; else it keeps its id
    {
        bool any = false;
        for (int m = 0; m < g_nMem && !any; ++m) any = g_mem[m].attach;
        if (any) {
            out = 0;
            for (int m = 0; m < g_nMem; ++m) {
                Member& M = g_mem[m];
                if (M.attach) {
                    int a = -1;
                    for (int q = 0; q < g_nMem; ++q) if (g_mem[q].id == M.attAnchor) { a = q; break; }
                    if (a < 0 || g_mem[a].attach) { NoteRelease(M); continue; }
                    M.slot = g_mem[a].slot;
                }
                g_mem[out++] = M;
            }
            g_nMem = out;
        }
    }
    // 7. waiting list: joined ones go; one readback without a sighting is forgiven (v0.9.0 r6, see kGapPx) -- r3..r5: none
    out = 0;
    for (int w = 0; w < g_nWait; ++w) {
        Wait& Wt = g_wait[w];
        if (Wt.hits < 0) continue;
        if (!waitSeen[w]) {
            if (Wt.age >= 1) continue;                  // a second readback without a sighting
            if (waitDrawn[w] && !(Wt.corr < kGapPx)) continue;   // drawn and static between two fast sightings: not a mover
            Wt.age = 1;
        }
        g_wait[out++] = Wt;
    }
    g_nWait = out;
    // 8. ids: free the empty ones, re-anchor an id whose anchor left (on the biggest remaining member)
    //    v0.9.0 r3: followers never anchor; r4: spinning parts never anchor either; a freed id is quarantined
    //    v0.9.0 r6: an id with spinning / follower members only is kept for kMissToDrop readbacks (a new LOD of its body can
    //    join through its wheels, step 4) and then freed with them; an id is freed when no member is left
    int cnt[ObjRecord::kIds] = {};
    int all[ObjRecord::kIds] = {};
    bool anchorLeft[ObjRecord::kIds] = {};
    for (int s = 1; s < ObjRecord::kIds; ++s) anchorLeft[s] = g_slotUsed[s];
    for (int m = 0; m < g_nMem; ++m) {
        const int s = g_mem[m].slot;
        ++all[s];
        if (g_mem[m].follower || g_mem[m].spin || g_mem[m].attach) continue;   // r10: + attached
        ++cnt[s];
        if (g_mem[m].id == g_slotAnchor[s]) anchorLeft[s] = false;
    }
    bool orphans = false;
    for (int s = 1; s < ObjRecord::kIds; ++s) {
        if (!g_slotUsed[s]) continue;
        if (!all[s]) { FreeSlotNow(s); continue; }
        if (!cnt[s]) {
            if (++g_slotNoBody[s] >= kMissToDrop) { FreeSlotNow(s); orphans = true; }
            continue;
        }
        g_slotNoBody[s] = 0;
        if (anchorLeft[s]) {
            float bi = -1.0f;
            for (int m = 0; m < g_nMem; ++m)
                if (g_mem[m].slot == s && !g_mem[m].follower && !g_mem[m].spin && !g_mem[m].attach &&
                    g_mem[m].ic > bi) {
                    bi = g_mem[m].ic; g_slotAnchor[s] = g_mem[m].id; g_slotAnchorIc[s] = bi;
                }
            ++g_slotInfo[s].reanchors;
            g_slotSolid[s] = 0;                         // v0.9.0 r4: a new anchor is not solid yet
        }
    }
    if (orphans) {                                      // v0.9.0 r3: followers (r4: + spinning parts) of a freed id
        out = 0;
        for (int m = 0; m < g_nMem; ++m) {
            if (!g_slotUsed[g_mem[m].slot]) { NoteRelease(g_mem[m]); continue; }
            g_mem[out++] = g_mem[m];
        }
        g_nMem = out;
    }
    // 9. (r2) debug-view rejects age out
    out = 0;
    for (int k = 0; k < g_nRej; ++k) if (--g_rej[k].ttl > 0) g_rej[out++] = g_rej[k];
    g_nRej = out;
    RebuildLut();
    // 10. v0.9.0 r11 attach diagnostics: one line per 600 readbacks (no behaviour)
    if (++g_attD.rb >= 600) {
        const AttDiag& A = g_attD;
        Log("MV objects: attach diagnostics over %llu readbacks (v0.9.0 r11, attach %s) -- rejects %llu: not dup / "
            "pairing / ambiguous %llu (own truck %llu, camera noise %llu, origin off screen %llu, other %llu) | dup / "
            "pairing / ambiguous %llu: already a member %llu, ego %llu, origin off screen %llu, no member listed in "
            "the readback %llu, nearest member origin > %.1f px %llu (< 2 px %llu, < 8 px %llu, < 32 px %llu, >= 32 px "
            "%llu), within %.1f px but w differs > %.0f%% %llu, coincided %llu (static class %llu) | listed anchors per "
            "readback %.1f | attached members now %d",
            (unsigned long long)A.rb, g_attach ? "on" : "off", (unsigned long long)A.rejects,
            (unsigned long long)(A.rejects - A.soft), (unsigned long long)A.byWhy[kRejEgo],
            (unsigned long long)A.byWhy[kRejNoise], (unsigned long long)A.byWhy[kRejOffscreen],
            (unsigned long long)(A.rejects - A.soft - A.byWhy[kRejEgo] - A.byWhy[kRejNoise] - A.byWhy[kRejOffscreen]),
            (unsigned long long)A.soft, (unsigned long long)A.member, (unsigned long long)A.ego,
            (unsigned long long)A.offscreen, (unsigned long long)A.noAnchor, (double)kAttachPx,
            (unsigned long long)A.farN,
            (unsigned long long)A.farHist[0], (unsigned long long)A.farHist[1], (unsigned long long)A.farHist[2],
            (unsigned long long)A.farHist[3], (double)kAttachPx, (double)(kAttachRelW * 100.0f),
            (unsigned long long)A.wMis, (unsigned long long)A.coincide, (unsigned long long)A.staticCl,
            A.rb ? (double)A.anchors / (double)A.rb : 0.0, GetStats().attachedMembers);
        g_attD = AttDiag{};
    }
}

// v0.9.0 r2: one header line + up to 8 id lines. From these the next log can tell a vehicle ("2 members, dup 1, 25 m,
// 0.40 ~m/frame, window 0.38..0.42, jumps 0") from a mis-paired static thing (many members, dup >> 1, speed jumping).
void LogIds(uint64_t blit) {
    int used = 0;
    for (int s = 1; s < ObjRecord::kIds; ++s) if (g_slotUsed[s]) ++used;
    Log("MV objects ids @blit %llu: %d in use (max %d%s) | rules: max-dup %d, own truck < %.1f m, %d coherent readbacks, "
        "r4: spin > %.2f never anchors, quarantine %llu frames, followers %s | r6: sticky -- a member leaves when not drawn "
        "for %d readbacks or proven elsewhere (misfit > max(%.0f px, %.0f%%) in %d readbacks), jumps > max(r2 tolerance, "
        "%.0f px footprint) | speed = the anchor's origin displacement vs the camera-only prediction (clip units, ~m per "
        "frame; window = min..max since the last dump), jumps = readbacks whose displacement broke coherence | r10: "
        "origin on screen within %.2f (else no mover, members released), new movers from sightings >= %.1f px only, "
        "attach %s | r11: camera-attached (origin < %.2f m) never a mover, static class = drawn in >= %d of the last "
        "%d readbacks with >= 50%% static sightings (%d identities now: never found / join, members released)",
        (unsigned long long)blit, used, EffMax(), g_debug ? ", id 15 = rejected (Ctrl+F6 view on)" : "", g_maxDup,
        (double)g_egoM, g_hits, (double)kSpinMax, (unsigned long long)kQuarantine, g_followers ? "on (solid ids)" : "off",
        kMissToDrop, (double)kGrossPx, (double)(kGrossRel * 100.0f), kProveRb, (double)kCohPx, (double)g_margin,
        (double)g_minPx, g_attach ? "on" : "off", (double)g_camM, g_staticRb, kStaticWin, GetStats().staticClass);
    int shown = 0;
    for (int s = 1; s < ObjRecord::kIds; ++s) {
        if (!g_slotUsed[s]) continue;
        SlotInfo& S = g_slotInfo[s];
        if (shown < 8) {
            int members = 0, maxDup = 0, followers = 0, spinners = 0, attached = 0;
            for (int m = 0; m < g_nMem; ++m)
                if (g_mem[m].slot == s) {
                    ++members;
                    if (g_mem[m].follower) ++followers;   // v0.9.0 r3
                    if (g_mem[m].spin) ++spinners;        // v0.9.0 r4
                    if (g_mem[m].attach) ++attached;      // v0.9.0 r10
                    if (g_mem[m].dup > maxDup) maxDup = g_mem[m].dup;
                }
            // v0.9.0 r4: + spinning members (wheels: never represent the id), solid = coherent anchor refreshes in a row
            // v0.9.0 r6: + the reference R's age (readbacks since a member last moved > 0.5 px: sub-threshold = kept)
            // v0.9.0 r10: + attached members (plates / mirrors on a member's world matrix)
            Log("  id %2d: %d member(s) (%d follower(s), %d spinning, %d attached) | biggest IndexCount %.0f dup %d "
                "(members max dup %d) | depth %.1f m | speed %.2f (window %.2f..%.2f) | held %llu readbacks | "
                "jumps %d | re-anchored %d | solid %d | reference %llu readback(s) old",
                s, members, followers, spinners, attached,
                (double)g_slotAnchorIc[s], S.dup, maxDup, (double)S.w, (double)S.speed,
                (double)(S.spSeen ? S.spMin : S.speed), (double)(S.spSeen ? S.spMax : S.speed),
                (unsigned long long)(g_st.updates - S.taken), S.jumps, S.reanchors, g_slotSolid[s],
                (unsigned long long)(g_st.updates - g_slotRUpd[s]));
        }
        ++shown;
        S.spSeen = false;                               // the window restarts
    }
    if (shown > 8) Log("  (+%d more ids not shown)", shown - 8);
    if (g_nEgo) {                                       // v0.9.0 r8: the own truck as the CPU sees it
        int nearestN = 0, ids = 0;
        for (int k = 0; k < g_nEgo; ++k) if (!g_ego[k].flag && g_ego[k].n > nearestN) nearestN = g_ego[k].n;
        for (int k = 0; k < g_nEgoId; ++k) if (g_egoId[k].lastU) ++ids;
        // v0.9.0 r9: copies of geometry keys (r8: identities), + the ego identities they gave; r10: in view space (m)
        Log("  ego set (own truck, r10 by geometry key, view space): %d flagged of %d near copies tracked, %d ego "
            "identities | flagged sightings deviated up to %.3f m from their reference (box %.3f m per axis, "
            "mv_objects_ego_m_box) | best unflagged run %d of %d readbacks (mv_objects_ego_rb)", g_egoSetN, g_nEgo, ids,
            (double)g_egoMaxDev, (double)g_egoMBox, nearestN, g_egoRb);
    }
    g_egoMaxDev = 0.0f;                                 // the window restarts
}
} // namespace ObjIds

// ---- CameraMv object part ----------------------------------------------------------------------------------------------
bool CameraMv::InitObjects() {
    if (m_objReady) return true;
    if (m_objFailed || !m_ready || !m_dev) return false;
    if (!ShaderCache::Done()) return false;             // v0.10.0 phase 17: Init no longer waits for every shader: retried
    m_objFailed = true;                                 // cleared on full success
    ID3D11Device* dev = m_dev.Get();
    if (!CreateCs(dev, ShaderCache::kMvObjects, &m_csObj)) return false;
    if (!CreateCs(dev, ShaderCache::kMvObjResolve, &m_csObjResolve)) return false;   // v0.9.0 r4
    if (!CreateCs(dev, ShaderCache::kMvObjVeto, &m_csObjVeto)) return false;         // v0.9.0 r4
    if (!CreateCs(dev, ShaderCache::kMvObjHold, &m_csObjHold)) return false;         // v0.9.0 r7
    HRESULT hr = S_OK;
    for (int k = 0; k < 2; ++k) {                       // gathered MVPs (raw), SRV (prev) + UAV (cur)
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (UINT)kObjMax * kSlotBytes;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.NumElements = bd.ByteWidth / 4;
        sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = bd.ByteWidth / 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(hr = dev->CreateBuffer(&bd, nullptr, &m_objMvp[k])) ||
            FAILED(hr = dev->CreateShaderResourceView(m_objMvp[k].Get(), &sd, &m_objMvpSrv[k])) ||
            FAILED(hr = dev->CreateUnorderedAccessView(m_objMvp[k].Get(), &ud, &m_objMvpUav[k]))) {
            Log("MV objects: MVP buffer create hr=0x%lx", (unsigned long)hr); return false;
        }
    }
    {                                                   // CPU -> GPU tables (dynamic structured)
        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = (UINT)kObjMax;
        bd.ByteWidth = (UINT)kObjMax * 16; bd.StructureByteStride = 16;
        if (FAILED(hr = dev->CreateBuffer(&bd, nullptr, &m_objTable)) ||
            FAILED(hr = dev->CreateShaderResourceView(m_objTable.Get(), &sd, &m_objTableSrv))) {
            Log("MV objects: table create hr=0x%lx", (unsigned long)hr); return false;
        }
        bd.ByteWidth = (UINT)kObjMax * 4; bd.StructureByteStride = 4;
        if (FAILED(hr = dev->CreateBuffer(&bd, nullptr, &m_objPrevList)) ||
            FAILED(hr = dev->CreateShaderResourceView(m_objPrevList.Get(), &sd, &m_objPrevListSrv))) {
            Log("MV objects: prev list create hr=0x%lx", (unsigned long)hr); return false;
        }
    }
    // per-id R table (structured float4; v0.9.0 r7: x 160 = + the previous frame's rows / states for the holds).
    // Created zeroed: no id is usable before the first dispatch, so a recreated table (resize, device change, re-init)
    // never holds an R from before (r7 hold counts start cleared).
    {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kObjSlotElems * 16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 16;
        float zero[kObjSlotElems * 4] = {};
        D3D11_SUBRESOURCE_DATA init{ zero, 0, 0 };
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.NumElements = kObjSlotElems;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kObjSlotElems;
        if (FAILED(hr = dev->CreateBuffer(&bd, &init, &m_objSlot)) ||
            FAILED(hr = dev->CreateShaderResourceView(m_objSlot.Get(), &sd, &m_objSlotSrv)) ||
            FAILED(hr = dev->CreateUnorderedAccessView(m_objSlot.Get(), &ud, &m_objSlotUav))) {
            Log("MV objects: id table create hr=0x%lx", (unsigned long)hr); return false;
        }
    }
    const UINT movBytes = kObjTailBase + kObjTailBytes;  // v0.9.0 r2: header + moving + reject list; r7: + tail
    {                                                   // moving list (raw UAV) + staging ring
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = movBytes;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = movBytes / 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(hr = dev->CreateBuffer(&bd, nullptr, &m_objMov)) ||
            FAILED(hr = dev->CreateUnorderedAccessView(m_objMov.Get(), &ud, &m_objMovUav))) {
            Log("MV objects: moving list create hr=0x%lx", (unsigned long)hr); return false;
        }
        D3D11_BUFFER_DESC st{};
        st.ByteWidth = movBytes;
        st.Usage = D3D11_USAGE_STAGING;
        st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (int k = 0; k < kObjRb; ++k)
            if (FAILED(hr = dev->CreateBuffer(&st, nullptr, &m_objRbBuf[k]))) {
                Log("MV objects: readback staging create hr=0x%lx", (unsigned long)hr); return false;
            }
    }
    {
        D3D11_BUFFER_DESC cb{};
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        cb.ByteWidth = 80;                              // v0.9.0 r4: + Ids (was 48); r10: + Ext
        if (FAILED(hr = dev->CreateBuffer(&cb, nullptr, &m_objCb))) { Log("MV objects: cbuffer hr=0x%lx", (unsigned long)hr); return false; }
    }
    {                                                   // v0.9.0 r4: CSMain -> CSResolve scratch (raw UAV)
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (16u + (UINT)kObjMax) * 4u;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = bd.ByteWidth / 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(hr = dev->CreateBuffer(&bd, nullptr, &m_objScratch)) ||
            FAILED(hr = dev->CreateUnorderedAccessView(m_objScratch.Get(), &ud, &m_objScratchUav))) {
            Log("MV objects: scratch create hr=0x%lx", (unsigned long)hr); return false;
        }
    }
    m_objReady = true; m_objFailed = false;
    m_objPrevN = 0; m_objMask = 0;
    m_objHoldOk = false;                                // v0.9.0 r7: the new table holds nothing
    m_objGoodMask = 0; m_objGoodPaired = 0; m_objMaskHoldRun = 0; m_objHoldRun = false;   // v0.9.0 r8
    for (bool& p : m_objRbPending) p = false;
    if (!m_objLogged) {
        m_objLogged = true;
        // v0.9.0 r2: + the trusted-pairing rules (own truck = mv_objects_ego_m, no longer mv_ego_origin_m)
        Log("MV objects: ready (unit%s) -- up to %d draws/pass, stencil ids 1..%d in the upper nibble, moving > %.1f px, "
            "plausible < %.1f m/frame, own truck origin < %.1f m excluded (mv_objects_ego_m) | r2 rules: geometry key drawn "
            "<= %d times (mv_objects_max_dup), pairing = the identity's own previous draw and unambiguous, %d coherent "
            "readbacks before an id (mv_objects_hits) | r3: up to %d VS cbuffer rings mirrored per pass, consecutive "
            "readbacks only, followers %s (mv_objects_followers; solid ids only) | r4: spinning parts (> %.2f rad/frame) "
            "never anchor or represent an id, freed ids rest 3 frames, per-frame representative = biggest trusted "
            "non-spinning tagged draw, an id whose tagged draw contradicts its motion takes the camera R for that "
            "frame, %d moving entries per readback | r6: sticky tags -- a member stays while it is "
            "drawn (moving or not) unless proven elsewhere (misfit > max(4 px, 50%%) twice), veto only > max(%.0f px, "
            "%.0f%%), coherence floor 2 px of footprint, hysteresis forgives one readback | r7: own truck = origin < "
            "%.1f m AND camera-locked (<= %.1f px of screen motion, mv_objects_lock_px), an id without a usable R of its "
            "own keeps its last R for up to %d frame(s) (mv_objects_hold; a vetoed id only when the camera R fits the "
            "draw no better), first-sighting join to an id with an equal R next to a member | r8: the lock test is a "
            "pre-filter at the ORIGIN only (<= %.1f px), the own truck = the ego set (near identities whose origin "
            "stayed within %.0f px of its reference for %d readbacks: mv_objects_ego_px / mv_objects_ego_rb; never a "
            "mover, members released), mask hold on a pairing failure (unusable record or < 50%% of the last good "
            "frame's pairs: the last good ids keep their R within the mv_objects_hold budget) | r9: no first-sighting "
            "join for non-spinning movers (mv_objects_hits coherent readbacks), candidates moving like the world are "
            "camera noise (column-2 test, never movers), ego set by geometry key (one entry per copy, position at the "
            "copy's reference depth), a world-miss frame keeps the last good camera R and the ids (mv_objects_hold "
            "budget, no history reset), fallback pairing by indexCount / startIndex / baseVertex when the full key finds "
            "nothing and the loose key is unique (%s)%s | r10: a mover / member needs its ORIGIN on screen (|x/w|, "
            "|y/w| <= %.2f, mv_objects_origin_margin; else reason 8 and members are released), new non-spinning movers "
            "only from sightings >= %.1f px (mv_objects_min_px), plates / mirrors whose origin coincides with a "
            "member's join it as attached members (%s, mv_objects_attach), ego set in view space (box %.3f m per axis, "
            "mv_objects_ego_m_box) | r11: draws whose origin lies within %.2f m of the camera are camera-attached "
            "(mv_objects_cam_m; reason 9: never movers / members / ego), static class = drawn in >= %d of the last %d "
            "readbacks, >= 50%% of them static (mv_objects_static_rb; never founds / joins an id, members released)",
            m_objFeed ? ", feeds the id table" : "",
            kObjMax, ObjIds::Max(), 0.5, 6.0, (double)ObjIds::EgoM(), ObjIds::MaxDup(), ObjIds::Hits(), ObjRecord::kRings,
            ObjIds::Followers() ? "ON" : "off", (double)ObjIds::kSpinMax, kObjMoving,
            (double)kObjVetoPx, (double)(kObjVetoRel * 100.0f), (double)ObjIds::EgoM(), (double)ObjIds::LockPx(),
            ObjIds::Hold(), (double)ObjIds::LockPx(), (double)ObjIds::EgoPx(), ObjIds::EgoRb(),
            s_pairFallback ? "on" : "off", ObjIds::EgoTrace() ? ", ego trace ON (mv_objects_ego_trace)" : "",
            (double)ObjIds::OriginMargin(), (double)ObjIds::MinPx(), ObjIds::Attach() ? "on" : "off",
            (double)ObjIds::EgoMBox(), (double)ObjIds::CamM(), ObjIds::StaticRb(), ObjIds::kStaticWin);
    }
    return true;
}

// v0.9.0 r8: Counts / Params / Img / Ids of the object dispatches (was inline in ObjPrepare; + the record-less hold)
void CameraMv::ObjWriteCb(void* dst, uint32_t n, uint32_t prevN, bool camOk, uint32_t mask, uint32_t hold,
                          uint32_t fullW, uint32_t fullH) const {
    const uint32_t counts[4] = { n, (uint32_t)kObjMoving, prevN, (uint32_t)kObjRej };
    // x = moving threshold (px; no verdicts while R_world is no camera solve: identity would flag every draw)
    // v0.9.0 r2: z = mv_objects_ego_m (was mv_ego_origin_m, which can be 0 = off), w = pairing margin (clip ~ m)
    const float params[4] = { camOk ? s_movingPx : 3.0e38f, 6.0f, ObjIds::EgoM(), kObjPairMargin };   // r11: s_movingPx
    // v0.9.0 r4: z = spin limit (rad / frame), w = veto tolerance (px); Ids.x = the valid id mask
    const float img[4] = { (float)fullW, (float)fullH, ObjIds::kSpinMax, kObjVetoPx };
    uint32_t vetoRel = 0;                               // v0.9.0 r6: Ids.y = veto fraction (float bits)
    { const float vr = kObjVetoRel; memcpy(&vetoRel, &vr, 4); }
    // v0.9.0 r7: Ids.z = own-truck lock limit (px, float bits; r8: at the origin), Ids.w = hold frames -- 0 when the
    // last Generate had no object dispatch (SlotR then holds no R of the previous frame)
    uint32_t lockPx = 0;
    { const float lp = ObjIds::LockPx(); memcpy(&lockPx, &lp, 4); }
    const uint32_t ids[4] = { mask, vetoRel, lockPx, hold };
    // v0.9.0 r10: Ext.x = the origin gate's margin; r11: Ext.y = the camera-attached radius (mv_objects_cam_m)
    const float ext[4] = { ObjIds::OriginMargin(), ObjIds::CamM(), 0.0f, 0.0f };
    memcpy(dst, counts, 16);
    memcpy((uint8_t*)dst + 16, params, 16);
    memcpy((uint8_t*)dst + 32, img, 16);
    memcpy((uint8_t*)dst + 48, ids, 16);
    memcpy((uint8_t*)dst + 64, ext, 16);
}

// v0.9.0 r8 MASK DROPOUT diagnostic (round-6 video: every tagged pixel gone for exactly one frame, 5 times in 38 s,
// while the per-window stats looked healthy -- frames=600, unusable=0, paired 100 %). Rate-limited: the first 10, then
// one per 600. Names the path (`why`), the masks, the record (draws / paired / tagged vs the last good frame), the id
// table and the FIFO pass this blit consumed vs the pass that became the pairing record.
void CameraMv::ObjDiag(const char* why, const ObjRecord* objs, int n, int paired, uint32_t mask, const char* hold) {
    const uint64_t k = ++m_objStats.maskDropouts;
    if (k > 10 && k % 600 != 0) return;
    const ObjIds::Stats is = ObjIds::GetStats();
    char hs[96];
    if (hold) snprintf(hs, sizeof(hs), "%s", hold);
    else if (m_objHoldRun)
        snprintf(hs, sizeof(hs), "HELD without a record (frame %d of %d in a row)", m_objMaskHoldRun, ObjIds::Hold());
    else if (!m_objHoldOk) snprintf(hs, sizeof(hs), "none (no object dispatch in the previous frame)");
    else if (ObjIds::Hold() <= 0) snprintf(hs, sizeof(hs), "off (mv_objects_hold 0)");
    else if (m_objMaskHoldRun >= ObjIds::Hold())
        snprintf(hs, sizeof(hs), "budget used (%d frames in a row)", m_objMaskHoldRun);
    else if (!m_objGoodMask) snprintf(hs, sizeof(hs), "nothing to hold (last good mask 0)");
    else snprintf(hs, sizeof(hs), "none (the object path did not run)");
    char ps[224] = "pass info unknown";
    if (m_objPassKnown)
        snprintf(ps, sizeof(ps), "this blit consumed pass s=%llu (newest s=%llu, %d unconsumed after it, fifo "
                 "overflows %llu), pairing record from pass %s%llu", (unsigned long long)m_objPassS,
                 (unsigned long long)(m_objPassNewest - 1), m_objPassOut, (unsigned long long)m_objPassOvf,
                 m_objPrevPassKnown ? "s=" : "?", (unsigned long long)(m_objPrevPassKnown ? m_objPrevPassS : 0));
    Log("MV objects: mask dropout #%llu at frame %llu -- %s | mask 0x%04x -> 0x%04x | record %s: %d draws, %d paired "
        "(%.0f%%; last good frame %d), tagged %u (last committed record %u), previous record %d draws | ids in use %d, "
        "members %d, ego set %d | %s | mask hold: %s",
        (unsigned long long)k, (unsigned long long)m_frame, why, m_objDiagPrevMask, mask,
        !objs ? "none" : (objs->Usable() ? "usable" : "UNUSABLE"), n, paired, n > 0 ? 100.0 * paired / n : 0.0,
        m_objDiagGood, m_objCurTagged, m_objPrevTagged, m_objPrevN, is.idsInUse, is.members, is.egoSet, ps, hs);
}

void CameraMv::ObjNoRun(const char* why, const ObjRecord* objs) {
    if (m_objDiagPrevMask) {
        m_objCurTagged = 0;
        ObjDiag(why, objs, objs ? objs->Count() : 0, 0, 0, "none (no object run)");
    }
    m_objDiagPrevMask = 0;
}

// v0.9.0 r8: an UNUSABLE record (nothing recorded, ring copy pending / failed, no mirror) after a frame that had ids:
// the last good frame's ids keep their R (CSResolve alone, every key 0 = the r7 hold) for up to mv_objects_hold frames
// in a row. Nothing is gathered, read back or committed; the next frame pairs nothing (its prev would be 2 frames old)
// and is held the same way, then the frame after it pairs normally.
bool CameraMv::ObjHoldEmpty(ID3D11DeviceContext* ctx, uint32_t fullW, uint32_t fullH) {
    const int hold = m_objHoldOk ? ObjIds::Hold() : 0;
    if (!m_objReady || hold <= 0 || m_objMaskHoldRun >= hold || !m_objGoodMask) return false;
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(ctx->Map(m_objCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) return false;
    ObjWriteCb(mp.pData, 0u, 0u, false, m_objGoodMask, (uint32_t)hold, fullW, fullH);
    ctx->Unmap(m_objCb.Get(), 0);
    m_objHoldRun = true;
    m_objMask = m_objGoodMask;
    m_objDbgSlot = (uint32_t)ObjIds::DebugSlot();
    m_objRunW = (float)fullW; m_objRunH = (float)fullH;
    ++m_objMaskHoldRun;
    ++m_objStats.maskHeld;
    return true;
}

uint32_t CameraMv::ObjPrepare(ID3D11DeviceContext* ctx, const ObjRecord* objs, uint32_t fullW, uint32_t fullH, bool camOk,
                              bool worldMiss, const char* missWhy) {
    m_objRun = false;
    m_objHoldRun = false;                              // v0.9.0 r8
    m_objMask = 0;
    m_objDbgSlot = 0;
    m_objCurTagged = 0;
    m_objDiagGood = m_objGoodPaired;                    // v0.9.0 r8: the baseline this frame is judged against
    // v0.9.0 r8: every exit -- the dropout diagnostic (the previous Generate had ids and this one has none / no object
    // run / a pairing collapse / a hold) and this frame's mask for the next one
    // v0.9.0 r9: + every world-miss frame (round 7: 6 whole-frame tag dropouts in the video, 1 dropout line)
    auto leave = [&](const char* why, int n, int paired, bool held, const char* holdWhy = nullptr) -> uint32_t {
        if (m_objDiagPrevMask && (!(m_objRun || m_objHoldRun) || !m_objMask || paired * 2 < n || held || worldMiss))
            ObjDiag(why, objs, n, paired, m_objMask, holdWhy);
        m_objDiagPrevMask = m_objMask;
        return m_objMask;
    };
    if (!objs) return leave("no object record for this pass (no stencil view)", 0, 0, false);
    if (!objs->Usable() || !InitObjects()) {
        if (objs->Count() > 0) ++m_objStats.unusable;
        const char* why = objs->Count() <= 0 ? "record unusable: no draw recorded (Count 0)"
                        : objs->FlushFailed() ? "record unusable: the ring mirror copy failed"
                        : objs->Pending() ? "record unusable: ring copy still pending (the pass was never flushed)"
                        : !objs->HasMirror() ? "record unusable: no mirror buffer"
                        : "object buffers could not be created";
        const bool held = objs->Usable() ? false : ObjHoldEmpty(ctx, fullW, fullH);   // r8 rule B (record-less hold)
        char wm[256];                                                                   // v0.9.0 r9
        if (worldMiss) { snprintf(wm, sizeof(wm), "world-miss (%s) + %s", missWhy ? missWhy : "?", why); why = wm; }
        return leave(why, objs->Count(), 0, held);
    }
    const int n = objs->Count() < kObjMax ? objs->Count() : kObjMax;
    // v0.9.0 r2: this pass sorted by key hash (was done in ObjFinish; same cost) -> how often each key is drawn
    {
        struct KhIdx { uint64_t kh; uint32_t idx; };
        static KhIdx srt[kObjMax];
        for (int i = 0; i < n; ++i) { srt[i].kh = objs->KeyHash(i); srt[i].idx = (uint32_t)i; }
        std::sort(srt, srt + n, [](const KhIdx& a, const KhIdx& b) { return a.kh < b.kh || (a.kh == b.kh && a.idx < b.idx); });
        for (int i = 0; i < n; ++i) { m_objCurKh[i] = srt[i].kh; m_objCurIdx[i] = srt[i].idx; }
        for (int a = 0; a < n;) {
            int b = a + 1;
            while (b < n && m_objCurKh[b] == m_objCurKh[a]) ++b;
            const uint16_t run = (uint16_t)(b - a < 0xFFFF ? b - a : 0xFFFF);
            for (int k = a; k < b; ++k) m_objCurDup[m_objCurIdx[k]] = run;
            a = b;
        }
    }
    // v0.9.0 r9 FALLBACK PAIRING: this pass's loose keys sorted (committed by ObjFinish as the next frame's prev list)
    m_objCurNL = 0;
    if (s_pairFallback) {
        struct LkIdx { uint64_t lk; uint32_t idx; };
        static LkIdx srl[kObjMax];
        int nl = 0;
        for (int i = 0; i < n; ++i)
            if (objs->LooseKh(i)) { srl[nl].lk = objs->LooseKh(i); srl[nl].idx = (uint32_t)i; ++nl; }
        std::sort(srl, srl + nl,
                  [](const LkIdx& a, const LkIdx& b) { return a.lk < b.lk || (a.lk == b.lk && a.idx < b.idx); });
        for (int i = 0; i < nl; ++i) { m_objCurLkh[i] = srl[i].lk; m_objCurLIdx[i] = srl[i].idx; }
        m_objCurNL = nl;
    }
    auto looseRun = [](const uint64_t* arr, int na, uint64_t lk, int* first) -> int {   // entries of lk in a sorted list
        const uint64_t* lo = std::lower_bound(arr, arr + na, lk);
        const uint64_t* hi = std::upper_bound(lo, arr + na, lk);
        *first = (int)(lo - arr);
        return (int)(hi - lo);
    };
    const bool fbOk = s_pairFallback && m_objPrevN > 0 && m_objPrevNL > 0;
    int nFb = 0;                                       // draws paired through the loose key this frame
    const int maxDup = ObjIds::MaxDup();
    const int rejSlot = ObjIds::DebugSlot();           // r2: the debug view's "rejected" id has no R (never in the mask)
    uint32_t mask = 0;
    int paired = 0;
    uint32_t nTag = 0;                                 // v0.9.0 r8 diagnostics: draws with a real stencil id
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(ctx->Map(m_objTable.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp)))
        return leave("the id table could not be mapped", n, 0, false, "none (Map failed)");
    uint32_t* t = (uint32_t*)mp.pData;
    for (int i = 0; i < n; ++i) {
        const uint64_t kh = objs->KeyHash(i);
        // run of prev draws with this key hash (m_objPrevKh is sorted)
        int lo = 0, hi = m_objPrevN;
        while (lo < hi) { const int mid = (lo + hi) >> 1; if (m_objPrevKh[mid] < kh) lo = mid + 1; else hi = mid; }
        int cntAll = 0;
        while (lo + cntAll < m_objPrevN && m_objPrevKh[lo + cntAll] == kh) ++cntAll;
        int cnt = cntAll < kObjCands ? cntAll : kObjCands;
        // v0.9.0 r9 FALLBACK: no previous draw of the full key -> the loose key when it is drawn exactly once in this
        // and in the previous record (its full-key partner would share it: no steal); the partner is then the
        // identity's own previous draw (rank 0), Table.y = its position in the key-hash-sorted prev list
        bool viaFb = false;
        if (!cntAll && fbOk && objs->LooseKh(i)) {
            int f0 = 0, f1 = 0;
            if (looseRun(m_objCurLkh, m_objCurNL, objs->LooseKh(i), &f0) == 1 &&
                looseRun(m_objPrevLkh, m_objPrevNL, objs->LooseKh(i), &f1) == 1) {
                lo = (int)m_objPrevPos[m_objPrevLIdx[f1]];
                cnt = 1;
                viaFb = true;
                ++nFb;
            }
        }
        // v0.9.0 r2: dup = draws of this key (this or the previous pass), identity partner = the previous draw with the
        // same occurrence index (its rank in the candidate run; 15 = none among the candidates)
        const int dup = m_objCurDup[i] > cntAll ? m_objCurDup[i] : cntAll;
        m_objDup[i] = (uint16_t)(dup < 0xFFFF ? dup : 0xFFFF);
        uint32_t rank = viaFb ? 0u : 15u;              // v0.9.0 r9: a fallback partner is the identity's own
        const uint16_t occ = (uint16_t)objs->Occ(i);
        if (!viaFb) for (int r = 0; r < cnt; ++r) if (m_objPrevOcc[lo + r] == occ) { rank = (uint32_t)r; break; }
        // v0.9.0 r4: every tagged draw carries its id (bits 8..11) and whether it may represent it (bit 13 set = not: a
        // follower / spinning part); the GPU picks the representative per frame (CSResolve). An id is in the mask when one
        // of its tagged draws may represent it and has previous draws of its key (r2 / r3: only the CPU's biggest one).
        const uint8_t tg = objs->Tag(i);
        const uint32_t sid = tg & 15u;
        const bool noRep = (tg & 0x80u) != 0;
        t[i * 4 + 0] = objs->Off(i);
        t[i * 4 + 1] = cnt ? (uint32_t)lo : 0xFFFFFFFFu;
        // v0.9.0 r8: bit 15 = the identity is in the ego set (own truck: never a mover, never represents, not in the
        // mask)
        const bool ego = ObjIds::IsEgo(objs->Id(i));
        // v0.9.0 r6: bit 14 = the draw is its id's CPU anchor (preferred representative)
        const bool anchor = sid && !noRep && !ego && (int)sid != rejSlot && ObjIds::IsAnchor(objs->Id(i), (int)sid);
        t[i * 4 + 2] = (uint32_t)cnt | (rank << 4) | (sid << 8) | (dup > maxDup ? 0x1000u : 0u) | (noRep ? 0x2000u : 0u) |
                       (anchor ? 0x4000u : 0u) | (ego ? 0x8000u : 0u);
        t[i * 4 + 3] = objs->Ic(i);
        // v0.9.0 r6: tag as drawn (the debug view's "rejected" id counts as untagged) | paired -> readback (sticky
        // membership: a member drawn and paired is seen; tag-switch metric)
        m_objCurInfo[i] = (uint8_t)(((int)sid != rejSlot ? sid : 0u) | (cnt ? 0x10u : 0u));
        if (cnt) ++paired;
        if (sid && (int)sid != rejSlot) ++nTag;
        if (sid && (int)sid != rejSlot && !noRep && !ego && cnt) mask |= 1u << sid;
    }
    ctx->Unmap(m_objTable.Get(), 0);
    m_objCurTagged = nTag;
    // v0.9.0 r9 unpaired-draw diagnostic (first 5): fewer than half of the draws found their full key in the previous
    // record -- a sample of 6 of them with their raw keys and the previous record's draw of the same loose key, if any
    if (m_objPrevN > 0 && (paired - nFb) * 2 < n && m_objUnpairedLogs < 5) {
        ++m_objUnpairedLogs;
        char s[1600];
        int o = snprintf(s, sizeof(s), "MV objects: unpaired-draw sample #%d at frame %llu%s: %d draws (previous record "
                         "%d), %d paired by the full key + %d by the fallback key (%s) |", m_objUnpairedLogs,
                         (unsigned long long)m_frame, worldMiss ? " (world-miss frame)" : "", n, m_objPrevN, paired - nFb,
                         nFb, s_pairFallback ? "on" : "off");
        int shown = 0;
        for (int i = 0; i < n && shown < 6 && o > 0 && o < (int)sizeof(s) - 240; ++i) {
            const uint64_t kh = objs->KeyHash(i);
            if (std::binary_search(m_objPrevKh, m_objPrevKh + m_objPrevN, kh)) continue;
            const ObjRecord::Geo& g = objs->GeoOf(i);
            o += snprintf(s + o, sizeof(s) - (size_t)o, " [ib %p vb %p ibOff %u vbOff %u ic %u si %u bv %d -> prev same "
                          "loose key: ", g.ib, g.vb, g.ibOffset, g.vbOffset, objs->Ic(i), g.startIndex, g.baseVertex);
            int f1 = 0;
            const int np = objs->LooseKh(i) ? looseRun(m_objPrevLkh, m_objPrevNL, objs->LooseKh(i), &f1) : 0;
            if (np > 0) {
                const ObjRecord::Geo& q = m_objPrevGeo[m_objPrevLIdx[f1]];
                o += snprintf(s + o, sizeof(s) - (size_t)o, "%d, first ib %p vb %p ibOff %u vbOff %u]", np, q.ib, q.vb,
                              q.ibOffset, q.vbOffset);
            } else {
                o += snprintf(s + o, sizeof(s) - (size_t)o, "none]");
            }
            ++shown;
        }
        Log("%s", s);
    }
    // v0.9.0 r8 rule B, MASK HOLD: a PAIRING FAILURE (fewer than half the draws of the last good -- not held -- frame
    // paired) keeps the last good frame's ids in the mask: those without a representative now get the r7 hold in
    // CSResolve (their rows = last frame's R, hold count + 1), so pass B and the debug view keep them. At most
    // mv_objects_hold frames in a row (the per-id hold budget applies on top); a frame that is not held is the new
    // baseline. A good frame is exactly r7.
    const int holdN = m_objHoldOk ? ObjIds::Hold() : 0;
    // v0.9.0 r9: a WORLD MISS is a pairing failure too (the camera candidates paired nothing: the frame the round-7
    // video showed without any tag); the camera R is held by Generate within the same budget
    const bool collapse = worldMiss || (m_objGoodPaired > 0 && paired * 2 < m_objGoodPaired);
    const uint32_t heldBits = (collapse && holdN > 0 && m_objMaskHoldRun < holdN) ? (m_objGoodMask & ~mask) : 0u;
    const bool held = heldBits != 0;
    char hs[96];                                       // diagnostics: what the hold did (before the baseline moves)
    if (held) snprintf(hs, sizeof(hs), "HELD 0x%04x (frame %d of %d in a row)", heldBits, m_objMaskHoldRun + 1, holdN);
    else if (!collapse) snprintf(hs, sizeof(hs), "none (not a pairing failure: the r7 path)");
    else if (holdN <= 0)
        snprintf(hs, sizeof(hs), "%s", m_objHoldOk ? "off (mv_objects_hold 0)"
                                                   : "none (no object dispatch in the previous frame)");
    else if (m_objMaskHoldRun >= holdN) snprintf(hs, sizeof(hs), "budget used (%d frames in a row)", m_objMaskHoldRun);
    else snprintf(hs, sizeof(hs), "nothing to hold (the last good ids are all in this mask)");
    if (held) { mask |= heldBits; ++m_objMaskHoldRun; ++m_objStats.maskHeld; }
    else if (!worldMiss) { m_objGoodMask = mask; m_objGoodPaired = paired; m_objMaskHoldRun = 0; }   // r9: not a baseline
    m_objStats.fallbackPairs += (uint64_t)nFb;         // v0.9.0 r9
    if (m_objPrevN > 0) {
        if (FAILED(ctx->Map(m_objPrevList.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp)))
            return leave("the previous-draw list could not be mapped", n, paired, false, "none (Map failed)");
        memcpy(mp.pData, m_objPrevIdx, (size_t)m_objPrevN * sizeof(uint32_t));
        ctx->Unmap(m_objPrevList.Get(), 0);
    }
    if (FAILED(ctx->Map(m_objCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp)))
        return leave("the object cbuffer could not be mapped", n, paired, false, "none (Map failed)");
    ObjWriteCb(mp.pData, (uint32_t)n, (uint32_t)m_objPrevN, camOk, mask, (uint32_t)holdN, fullW, fullH);
    ctx->Unmap(m_objCb.Get(), 0);
    m_objRun = true;
    m_objRunN = n;
    m_objRunW = (float)fullW; m_objRunH = (float)fullH;
    m_objMask = mask;
    m_objDbgSlot = (uint32_t)rejSlot;
    ++m_objStats.frames;
    m_objStats.draws += (uint64_t)n;
    m_objStats.paired += (uint64_t)paired;
    m_objStats.tagged += objs->Tagged();
    for (uint32_t b = mask; b; b &= b - 1) ++m_objStats.maskBits;
    char why[256];
    if (worldMiss)                                     // v0.9.0 r9
        snprintf(why, sizeof(why), "world-miss: the camera candidates paired nothing with the previous pass (%s); object "
                 "record: %d of %d draws paired (%d by the fallback key; last good frame %d)", missWhy ? missWhy : "?",
                 paired, n, nFb, m_objDiagGood);
    else if (collapse)
        snprintf(why, sizeof(why), "pairing collapse: %d of %d draws paired (< 50%% of the last good frame's %d)%s",
                 paired, n, m_objDiagGood, m_objPrevN ? "" : ", no previous record (not committed)");
    else if (!mask && !nTag) snprintf(why, sizeof(why), "mask 0: no draw of this record carries a stencil id (the id "
                                      "lookups at draw time returned 0)");
    else if (!mask)
        snprintf(why, sizeof(why), "mask 0: the %u tagged draws have no previous draw of their key or may not "
                 "represent their id", nTag);
    else snprintf(why, sizeof(why), "fewer than half of the record's draws paired");
    return leave(why, n, paired, held, hs);
}

void CameraMv::ObjDispatch(ID3D11DeviceContext* ctx, const ObjRecord* objs) {
    if (!(m_objRun && objs) && !m_objHoldRun) return;
    const UINT zero4[4] = { 0, 0, 0, 0 };
    ctx->ClearUnorderedAccessViewUint(m_objMovUav.Get(), zero4);
    ctx->ClearUnorderedAccessViewUint(m_objScratchUav.Get(), zero4);   // v0.9.0 r4: no representative yet
    const int cur = m_objPing, prev = 1 - m_objPing;
    ID3D11Buffer* cb = m_objCb.Get();
    // v0.9.0 r8: a record-less hold binds no mirror (CSResolve reads Cur / Prev only for a representative: none)
    ID3D11ShaderResourceView* srvs[5] = { (m_objHoldRun || !objs) ? nullptr : objs->MirrorSrv(), m_objTableSrv.Get(),
                                          m_objMvpSrv[prev].Get(), m_objPrevListSrv.Get(), m_solveSrv.Get() };
    ID3D11UnorderedAccessView* uavs[4] = { m_objMvpUav[cur].Get(), m_objSlotUav.Get(), m_objMovUav.Get(),
                                           m_objScratchUav.Get() };
    ID3D11ShaderResourceView* nullSrv[5] = {};
    ID3D11UnorderedAccessView* nullUav[4] = {};
    const UINT keep[4] = { (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1 };
    const UINT groups = ((UINT)m_objRunN + 63u) / 64u;
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetShaderResources(0, 5, srvs);
    ctx->CSSetUnorderedAccessViews(0, 4, uavs, keep);
    if (m_objHoldRun) {                                 // v0.9.0 r8: record-less mask hold -- CSResolve only (keys 0)
        ctx->CSSetShader(m_csObjResolve.Get(), nullptr, 0);
        ctx->Dispatch(1, 1, 1);
        ctx->CSSetShaderResources(0, 5, nullSrv);
        ctx->CSSetUnorderedAccessViews(0, 4, nullUav, keep);
        return;
    }
    ctx->CSSetShader(m_csObj.Get(), nullptr, 0);        // gather + pair + verdict (+ r4: spin, representative candidates)
    ctx->Dispatch(groups, 1, 1);
    ctx->CSSetShader(m_csObjResolve.Get(), nullptr, 0); // v0.9.0 r4: per id: the representative's R (16 threads)
    ctx->Dispatch(1, 1, 1);
    ctx->CSSetShader(m_csObjVeto.Get(), nullptr, 0);    // v0.9.0 r4: same-frame veto (one thread per draw)
    ctx->Dispatch(groups, 1, 1);
    ctx->CSSetShader(m_csObjHold.Get(), nullptr, 0);    // v0.9.0 r7: veto verdict per id (hold / camera R, 16 threads)
    ctx->Dispatch(1, 1, 1);
    ctx->CSSetShaderResources(0, 5, nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 4, nullUav, keep);
}

// Oldest landed staging copy first (never stalls); each one updates the global id table (feeding unit only).
void CameraMv::ObjPollReadback(ID3D11DeviceContext* ctx) {
    static ObjIds::Moving ent[kObjMoving];
    static ObjIds::Reject rej[kObjRej];
    for (;;) {
        int best = -1;
        for (int i = 0; i < kObjRb; ++i)
            if (m_objRbPending[i] && (best < 0 || m_objRbSeq[i] < m_objRbSeq[best])) best = i;
        if (best < 0) break;
        D3D11_MAPPED_SUBRESOURCE mp{};
        const HRESULT hr = ctx->Map(m_objRbBuf[best].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mp);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) break;
        m_objRbPending[best] = false;
        if (FAILED(hr)) continue;
        const uint8_t* b = (const uint8_t*)mp.pData;
        uint32_t cnt = 0, rcnt = 0, veto = 0, noRep = 0;
        memcpy(&cnt, b, 4);
        memcpy(&rcnt, b + 4, 4);                       // v0.9.0 r2: reject list
        memcpy(&veto, b + 8, 4);                       // v0.9.0 r4: vetoed tagged draws, ids without a trusted member
        memcpy(&noRep, b + 12, 4);
        m_objStats.vetoDraws += veto;
        m_objStats.noRepIds += noRep;
        {                                              // v0.9.0 r7 tail: held / hold-expired id masks, near draws
            uint32_t tl[4];
            memcpy(tl, b + kObjTailBase, sizeof(tl));
            auto bits = [](uint32_t v) { int c = 0; for (v &= 0xFFFEu; v; v &= v - 1) ++c; return c; };
            m_objStats.heldIds += (uint64_t)bits(tl[0]);
            m_objStats.holdExpired += (uint64_t)bits(tl[1]);
            m_objStats.nearLocked += tl[2];
            m_objStats.nearAccepted += tl[3];
            uint32_t camN = 0;                         // v0.9.0 r11: camera-attached draws of the pass
            memcpy(&camN, b + kObjTailBase + kObjTailCam, 4);
            m_objStats.camOrigin += camN;
        }
        if (cnt > (uint32_t)kObjMoving) { cnt = (uint32_t)kObjMoving; ++m_objStats.movFull; }
        if (rcnt > (uint32_t)kObjRej) { rcnt = (uint32_t)kObjRej; ++m_objStats.rejFull; }
        int n = 0;
        for (uint32_t k = 0; k < cnt; ++k) {
            float f[32];                               // v0.9.0 r4: 128-byte entries (+ spin)
            memcpy(f, b + 16 + (size_t)k * kObjMovStride, sizeof(f));
            const int idx = (int)f[0];
            if (!(f[0] >= 0.0f) || idx >= m_objRbN[best]) continue;
            ObjIds::Moving& m = ent[n++];
            m.id = m_objRbIds[best][idx];
            m.kh = m_objRbKh[best][idx];               // v0.9.0 r9
            m.dev = f[1]; m.w = f[2]; m.ic = f[3];
            for (int c = 0; c < 4; ++c) m.c0[c] = f[4 + c];
            for (int c = 0; c < 16; ++c) m.R[c] = f[8 + c];
            for (int c = 0; c < 3; ++c) m.d[c] = f[24 + c];
            m.spin = f[28] == f[28] ? f[28] : 1.0e9f;  // v0.9.0 r4
            const bool vok = f[31] > -1.0e29f;         // v0.9.0 r10: the view-space origin (z -1e30 = none)
            for (int c = 0; c < 3; ++c) m.v[c] = vok ? f[29 + c] : std::numeric_limits<float>::quiet_NaN();
            m.dup = m_objRbDup[best][idx];
        }
        int nr = 0;
        for (uint32_t k = 0; k < rcnt; ++k) {
            uint32_t u[20];                            // v0.9.0 r3: 64-byte entries (+ origin, nearest / own partner)
            memcpy(u, b + kObjRejBase + (size_t)k * kObjRejStride, sizeof(u));   // r10: 80 (+ view-space origin)
            const uint32_t why = u[1] & 0xFFu;
            if (u[0] >= (uint32_t)m_objRbN[best] || why == 0 || why >= (uint32_t)ObjIds::kRejKinds) continue;
            ObjIds::Reject& r = rej[nr++];
            r.id = m_objRbIds[best][u[0]];
            r.kh = m_objRbKh[best][u[0]];              // v0.9.0 r9
            r.why = (int)why;
            memcpy(&r.dev, &u[2], 4);
            memcpy(&r.w, &u[3], 4);
            r.dup = m_objRbDup[best][u[0]];
            memcpy(r.c0, &u[4], 16);
            memcpy(r.a[0], &u[8], 16);
            memcpy(r.a[1], &u[12], 16);
            r.ownOk = (u[1] & 0x100u) != 0;
            r.drawnOnly = (u[1] & 0x200u) != 0;        // v0.9.0 r10: reason 8 (r11: or 9) of a tagged non-candidate
            {
                float vf[4];
                memcpy(vf, &u[16], 16);
                const bool vok = vf[3] > 0.5f;
                for (int c = 0; c < 3; ++c) r.v[c] = vok ? vf[c] : std::numeric_limits<float>::quiet_NaN();
            }
            if (!r.drawnOnly) ++m_objStats.rej[r.why];
        }
        ctx->Unmap(m_objRbBuf[best].Get(), 0);
        ++m_objStats.rbRead;
        m_objStats.moving += (uint64_t)n;
        // v0.9.0 r6: every identity of that pass that was drawn with previous draws of its key (paired): its members are
        // SEEN whether they moved > 0.5 px or not (sticky membership)
        static uint64_t drawn[kObjMax];
        int nd = 0;
        for (int k = 0; k < m_objRbN[best]; ++k) if (m_objRbInfo[best][k] & 0x10u) drawn[nd++] = m_objRbIds[best][k];
        ObjTagSwitches(best);                          // v0.9.0 r6: the tags as drawn in that pass vs the last read-back pass
        ObjIds::Update(ent, n, rej, nr, m_objRbFrame[best], m_objRbW[best], m_objRbH[best], m_frame,   // r4: + now
                       drawn, nd);                     // r6: + drawn
    }
}

// v0.9.0 r6 TAG-SWITCH metric. The read-back pass's identities with their stencil tag as drawn, compared with the last
// read-back pass: an identity drawn in both whose tag went 0 -> id (on), id -> 0 (off) or id A -> id B (id change). on + off
// are exactly the pixels that flipped between the camera R and an object R (DLSS history mismatch, the round-4 wobble);
// an id change keeps an object R. Vehicles entering / leaving the view or acquiring their first id show up as "on" once.
void CameraMv::ObjTagSwitches(int rb) {
    const int n = m_objRbN[rb];
    const int nt = 1 - m_swCur;
    uint32_t g = ++m_swGen[nt];
    if (!g) { memset(m_swTab[nt], 0, sizeof(m_swTab[nt])); g = m_swGen[nt] = 1; }
    SwEnt* T = m_swTab[nt];
    const SwEnt* P = m_swTab[m_swCur];
    const uint32_t pg = m_swGen[m_swCur];
    auto hash = [](uint64_t id) { return (uint32_t)((id * 0x9E3779B97F4A7C15ull) >> 52) & (uint32_t)(kSwTab - 1); };
    uint64_t on = 0, off = 0, chg = 0;
    for (int k = 0; k < n; ++k) {
        const uint64_t id = m_objRbIds[rb][k];
        const uint8_t tag = (uint8_t)(m_objRbInfo[rb][k] & 15u);
        uint32_t h = hash(id);
        while (T[h].gen == g && T[h].id != id) h = (h + 1) & (uint32_t)(kSwTab - 1);
        T[h].id = id; T[h].gen = g; T[h].tag = tag;
        if (!m_swHave) continue;
        for (uint32_t q = hash(id);; q = (q + 1) & (uint32_t)(kSwTab - 1)) {
            if (P[q].gen != pg) break;                 // not drawn in the last read-back pass
            if (P[q].id != id) continue;
            const uint8_t pt = P[q].tag;
            if (!pt && tag) ++on; else if (pt && !tag) ++off; else if (pt && tag && pt != tag) ++chg;
            break;
        }
    }
    if (m_swHave) { m_objStats.tagOn += on; m_objStats.tagOff += off; m_objStats.tagIdChange += chg; ++m_objStats.tagCmp; }
    m_swCur = nt;
    m_swHave = true;
}

void CameraMv::ObjFinish(ID3D11DeviceContext* ctx, const ObjRecord* objs) {
    if (m_objHoldRun) {                                 // v0.9.0 r8: record-less mask hold
        // nothing was gathered: no readback, no commit. The last good record is now two frames old -- an R paired
        // against it would be two frames of motion -- so the next frame pairs nothing (and is held the same way:
        // CSResolve finds no representative); SlotR holds this frame's (held) ids for it.
        m_objHoldRun = false;
        m_objPrevN = 0;
        m_objPrevPassKnown = false;
        m_objHoldOk = true;
        return;
    }
    if (!m_objRun || !objs) { ObjForget(); return; }
    m_objRun = false;
    const int n = m_objRunN;
    if (m_objFeed) {
        ObjPollReadback(ctx);
        for (int i = 0; i < kObjRb; ++i) {
            if (m_objRbPending[i] || !m_objRbBuf[i]) continue;
            ctx->CopyResource(m_objRbBuf[i].Get(), m_objMov.Get());
            m_objRbPending[i] = true;
            m_objRbSeq[i] = ++m_objRbCtr;
            m_objRbN[i] = n;
            m_objRbW[i] = m_objRunW; m_objRbH[i] = m_objRunH;
            m_objRbFrame[i] = m_frame;                // v0.9.0 r2: coherence needs the frame gap between readbacks
            for (int k = 0; k < n; ++k) {
                m_objRbIds[i][k] = objs->Id(k); m_objRbDup[i][k] = m_objDup[k];
                m_objRbKh[i][k] = objs->KeyHash(k);   // v0.9.0 r9 (ego set by key)
            }
            memcpy(m_objRbInfo[i], m_objCurInfo, (size_t)n);   // v0.9.0 r6
            break;
        }
    }
    // commit: the buffer just gathered becomes prev; the key map = this record sorted by key hash (ObjPrepare, v0.9.0 r2)
    m_objPing = 1 - m_objPing;
    m_objHoldOk = true;                                 // v0.9.0 r7: SlotR holds this frame's ids for the next frame
    m_objPrevTagged = m_objCurTagged;                   // v0.9.0 r8 diagnostics: the pairing record's tags and pass
    m_objPrevPassS = m_objPassS; m_objPrevPassKnown = m_objPassKnown;
    for (int i = 0; i < n; ++i) {
        m_objPrevKh[i] = m_objCurKh[i];
        m_objPrevIdx[i] = m_objCurIdx[i];
        m_objPrevOcc[i] = (uint16_t)objs->Occ((int)m_objCurIdx[i]);
        m_objPrevPos[m_objCurIdx[i]] = (uint32_t)i;    // v0.9.0 r9: where draw m_objCurIdx[i] sits in the prev list
        m_objPrevGeo[i] = objs->GeoOf(i);              // v0.9.0 r9 (by draw index; diagnostic)
    }
    m_objPrevN = n;
    for (int i = 0; i < m_objCurNL; ++i) { m_objPrevLkh[i] = m_objCurLkh[i]; m_objPrevLIdx[i] = m_objCurLIdx[i]; }   // r9
    m_objPrevNL = m_objCurNL;
}

void CameraMv::Shutdown() {
    // v0.9.0 object part
    m_csObj.Reset();
    m_csObjResolve.Reset(); m_csObjVeto.Reset(); m_objScratchUav.Reset(); m_objScratch.Reset();   // v0.9.0 r4
    m_csObjHold.Reset();                                                                          // v0.9.0 r7
    for (int k = 0; k < 2; ++k) { m_objMvpUav[k].Reset(); m_objMvpSrv[k].Reset(); m_objMvp[k].Reset(); }
    m_objTableSrv.Reset(); m_objTable.Reset(); m_objPrevListSrv.Reset(); m_objPrevList.Reset();
    m_objSlotUav.Reset(); m_objSlotSrv.Reset(); m_objSlot.Reset();
    m_objMovUav.Reset(); m_objMov.Reset(); m_objCb.Reset();
    for (int k = 0; k < kObjRb; ++k) { m_objRbBuf[k].Reset(); m_objRbPending[k] = false; }
    m_objReady = false; m_objFailed = false; m_objPrevN = 0; m_objMask = 0; m_objRun = false; m_objStencil = nullptr;
    m_objHoldOk = false;                                // v0.9.0 r7
    m_stageSnap.Reset(); m_stageSolve.Reset(); m_stageDbg.Reset();
    for (int i = 0; i < kRing; ++i) { m_ring[i].Reset(); m_ringPending[i] = false; }
    m_cbDims.Reset(); m_cbPairs.Reset();
    m_solveSrv.Reset(); m_solveUav.Reset(); m_solve.Reset();
    m_prev.Shutdown();
    m_csReproj.Reset(); m_csSolve.Reset();
    m_fwdCntUav.Reset(); m_fwdCnt.Reset();                                                        // v0.9.0 r11
    for (int k = 0; k < kFwdCntRb; ++k) { m_fwdCntRb[k].Reset(); m_fwdCntPending[k] = false; }
    m_fwdCntFailed = false;
    m_dev.Reset();
    m_ready = false; m_failed = false;
}

#endif // WITH_DLAA
