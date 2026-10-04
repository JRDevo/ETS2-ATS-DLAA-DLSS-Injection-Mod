// CameraMv implementation (v0.4.1). See motion_vectors.h and
// docs/DLAA_INTEGRATION.md ("Motion vectors"). HLSL is embedded and compiled at
// runtime with D3DCompile (d3dcompiler_47 is an OS DLL), same as the depth pass.
#ifdef WITH_DLAA

#include "motion_vectors.h"
#include "log.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstring>

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
    float4 Params;       // x = near_reject_m (world layer only), y = ego_origin_m (v0.6.0)
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
// kReprojDepthShader-BEGIN (the build validates this block with fxc)
const char kReprojDepthShader[] = R"(
cbuffer DimsCB : register(b0) {
    float2 Size;         // crop = dispatch size (MV / R32F textures)
    float  EgoPixelM;    // v0.6.0: world pixels closer than this (m) use R_ego; 0 = off
    float  _pad;
    float2 Origin;       // v0.6.4: crop origin in the full image (px, even)
    float2 FullSize;     // v0.6.4: full image size (px) = depth twin size
};
Texture2D<float>           DepthIn  : register(t0);
StructuredBuffer<float4>   Solve    : register(t1);
RWTexture2D<float2>        MvOut    : register(u0);
RWTexture2D<float>         DepthOut : register(u1);

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)Size.x || id.y >= (uint)Size.y) return;

    uint2 src = id.xy + (uint2)Origin;                  // v0.6.4: pixel in the full image
    float d = DepthIn[src];
    DepthOut[id.xy] = d;
    bool  cabin = d >= 0.9;
    float lo = cabin ? 0.9 : 0.01;
    float hi = cabin ? 1.0 : 0.9;
    float z  = saturate((d - lo) / (hi - lo));

    float2 uv  = (float2(src) + 0.5) / FullSize;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    uint base = cabin ? 4u : 0u;
    // v0.6.0 ego: near world pixels move with the camera's vehicle -> R_ego
    if (!cabin && d >= 0.01 && EgoPixelM > 0.0) {
        float4 ir = Solve[16];
        if (any(ir != 0.0)) {
            float iw   = dot(ir, float4(ndc, z, 1.0));
            float dist = (abs(iw) > 1e-9) ? 1.0 / iw : 1e9;
            if (dist > 0.0 && dist < EgoPixelM) base = 12u;
        }
    }
    float4x4 R = float4x4(Solve[base], Solve[base + 1], Solve[base + 2], Solve[base + 3]);
    float4 c   = mul(R, float4(ndc, z, 1.0));

    float2 mv = float2(0.0, 0.0);
    if (c.w > 1e-6) {
        float2 pn = c.xy / c.w;
        float2 prevUv = float2(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5);
        mv = (prevUv - uv) * FullSize;
        if (!all(isfinite(mv))) mv = float2(0.0, 0.0);
        mv = clamp(mv, -4096.0, 4096.0);
    }
    MvOut[id.xy] = mv;
}
)";
// kReprojDepthShader-END

bool SameKey(const CameraMv::DrawKey& a, const CameraMv::DrawKey& b) {
    return a.ib == b.ib && a.vb == b.vb && a.ibOffset == b.ibOffset && a.vbOffset == b.vbOffset &&
           a.indexCount == b.indexCount && a.startIndex == b.startIndex && a.baseVertex == b.baseVertex;
}

bool CompileCs(ID3D11Device* dev, const char* src, size_t len, const char* name,
               ID3D11ComputeShader** out) {
    ComPtr<ID3DBlob> cso, err;
    HRESULT hr = D3DCompile(src, len, name, nullptr, nullptr, "CSMain", "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cso, &err);
    if (FAILED(hr)) {
        Log("MV: %s D3DCompile failed hr=0x%lx: %s", name, hr,
            err ? (const char*)err->GetBufferPointer() : "(no log)");
        return false;
    }
    hr = dev->CreateComputeShader(cso->GetBufferPointer(), cso->GetBufferSize(), nullptr, out);
    if (FAILED(hr)) { Log("MV: %s CreateComputeShader hr=0x%lx", name, hr); return false; }
    return true;
}

} // namespace

bool CameraMv::Init(ID3D11Device* dev) {
    if (m_ready) return true;
    if (m_failed || !dev) return false;
    m_failed = true;                                   // cleared on full success
    m_dev = dev;

    D3D11_FEATURE_DATA_D3D11_OPTIONS opt{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &opt, sizeof(opt))))
        Log("MV: device options ConstantBufferPartialUpdate=%d ConstantBufferOffsetting=%d "
            "(CopySubresourceRegion out of the CB ring is plain buffer->buffer, not affected)",
            (int)opt.ConstantBufferPartialUpdate, (int)opt.ConstantBufferOffsetting);

    if (!CompileCs(dev, kSolveShader, sizeof(kSolveShader) - 1, "mv_solve", &m_csSolve)) return false;
    if (!CompileCs(dev, kReprojDepthShader, sizeof(kReprojDepthShader) - 1, "mv_reproject_depth", &m_csReproj)) return false;

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
    cb.ByteWidth = 32;                                 // v0.6.4: + Origin, FullSize (was 16)
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
    ctx->CopyResource(m_buf.Get(), o.m_buf.Get());
    for (int l = 0; l < kLayers; ++l) {
        m_count[l] = o.m_count[l];
        for (int i = 0; i < m_count[l]; ++i) m_keys[l][i] = o.m_keys[l][i];
    }
}

void CameraMv::Commit(ID3D11DeviceContext* ctx, const CandidateRecord& cur) {
    if (!m_ready) return;
    m_prev.CopyFrom(ctx, cur);
    m_prevStale = false;                                        // v0.6.1: prev is the newest record again
}

void CameraMv::Invalidate() {
    m_prev.Reset();
    for (int i = 0; i < kRing; ++i) m_ringPending[i] = false;   // drop in-flight R readbacks (stale history)
    m_rNew = false;
    m_solveGood = false;                                        // v0.6.1: never reuse an R from before
    m_prevStale = false;
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
        if (!m_egoLogged && m_egoPixel > 0.0f && m_rLast[69] > 0.5f) {
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
        if (periodic || (moving && m_driveLogs < 200)) {
            if (!periodic) ++m_driveLogs;
            Log("MV: %s frame %llu R_world [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f]"
                " maxdev=%.4f (pairs=%.0f valid=%.0f pick=%.0f) | chosen world pair |w|=%.1f m IndexCount=%.0f, "
                "near-rejected %.0f/%.0f valid%s | ego: cand=%.0f used=%.0f fallback=%.0f |R_ego-R_world|=%.4f",
                periodic ? "R" : "R(moving)", (unsigned long long)m_solveFrame,
                f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11],
                f[12], f[13], f[14], f[15], dev, f[32], f[33], f[34],
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
    const float dims[8] = { (float)w, (float)h, m_egoPixel, 0.0f,                  // v0.6.0: z = EgoPixelM
                            (float)x0, (float)y0, (float)fullW, (float)fullH };     // v0.6.4
    memcpy(mp.pData, dims, sizeof(dims));
    ctx->Unmap(m_cbDims.Get(), 0);
    return true;
}

// Pass B: per-pixel reprojection through the solve buffer -> MV texture (u0) + flattened R32F depth (u1, v0.5.7).
void CameraMv::DispatchReproj(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, ID3D11ShaderResourceView* depthTwinSrv,
                              ID3D11UnorderedAccessView* depthR32Uav, ID3D11UnorderedAccessView* mvUav) {
    ID3D11ShaderResourceView* nullSrv[2] = {};
    ID3D11Buffer* cbs = m_cbDims.Get();
    ID3D11ShaderResourceView* srvs[2] = { depthTwinSrv, m_solveSrv.Get() };
    ID3D11UnorderedAccessView* uavs[2] = { mvUav, depthR32Uav };
    ID3D11UnorderedAccessView* nullUavs[2] = {};
    const UINT keep2[2] = { (UINT)-1, (UINT)-1 };
    ctx->CSSetShader(m_csReproj.Get(), nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &cbs);
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetUnorderedAccessViews(0, 2, uavs, keep2);
    ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    // Unbind t0/t1/u0/u1: NGX reads the R32F depth and the MV texture as SRVs next.
    ctx->CSSetShaderResources(0, 2, nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 2, nullUavs, keep2);
}

void CameraMv::LogFrameLine(const FrameStats& fs, const CandidateRecord& cur) {
    if (m_frame % 600 != 0) return;
    const double g = (double)m_genFrames;
    Log("MV: frame %llu pairs this frame world=%d cabin=%d%s | avg world=%.2f cabin=%.2f | "
        "world-miss frames %llu/%llu bad=%llu stale-reuse=%llu (candidates cur: world=%d cabin=%d)",
        (unsigned long long)m_frame, fs.pairs[0], fs.pairs[1], fs.reused ? " (R reused)" : "",
        g > 0 ? (double)m_pairSum[0] / g : 0.0, g > 0 ? (double)m_pairSum[1] / g : 0.0,
        (unsigned long long)m_missFrames, (unsigned long long)m_genFrames,
        (unsigned long long)m_badFrames, (unsigned long long)m_staleFrames,
        cur.Count(0), cur.Count(1));
}

// v0.5.7: pass B also flattens the depth twin into depthR32Uav. Contract: returns true <=> pass B was
// dispatched (MV texture AND R32F depth written); every `return false` below happens BEFORE any dispatch, so
// on false neither texture was touched and the caller must produce the R32F depth itself (convert pass).
bool CameraMv::Generate(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, uint32_t x0, uint32_t y0,
                        uint32_t fullW, uint32_t fullH, const CandidateRecord& cur,
                        ID3D11ShaderResourceView* depthTwinSrv, ID3D11UnorderedAccessView* depthR32Uav,
                        ID3D11UnorderedAccessView* mvUav, FrameStats* stats, bool candBad) {
    if (!m_ready || !ctx || !depthTwinSrv || !depthR32Uav || !mvUav || !cur.Ready()) return false;
    ++m_frame;
    const CandidateRecord& prev = m_prev;

    if (!m_loggedFirst && (cur.Count(0) + cur.Count(1)) > 0) {
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
        if (!WriteDims(ctx, w, h, x0, y0, fullW, fullH)) {   // nothing dispatched: same contract as below
            if (candBad) m_prevStale = true; else Commit(ctx, cur);
            return false;
        }
        DispatchReproj(ctx, w, h, depthTwinSrv, depthR32Uav, mvUav);
        if (candBad) { ++m_badFrames; m_prevStale = true; }
        else         { ++m_staleFrames; Commit(ctx, cur); }   // Commit clears m_prevStale
        LogFrameLine(fs, cur);
        return true;
    }

    // ---- match prev <-> cur per layer -----------------------------------------
    struct Pair { int cur, prev; bool unique; UINT ic; };
    uint32_t pairData[2 * kPairs][4] = {};
    uint32_t counts[4] = {};
    uint32_t allPairs[kSlots][4] = {};                 // v0.6.0: every matched world pair (ego scan in pass A)
    FrameStats fs;
    for (int l = 0; l < kLayers; ++l) {
        Pair found[kSlots];
        int nf = 0;
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
            if (pi >= 0) found[nf++] = { i, pi, totC == 1 && totP == 1, k.indexCount };
        }
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
    fs.worldMiss = fs.pairs[0] == 0;
    if (stats) *stats = fs;
    m_last = fs;
    ++m_genFrames;
    if (fs.worldMiss) ++m_missFrames;
    for (int l = 0; l < kLayers; ++l) m_pairSum[l] += (uint64_t)fs.pairs[l];

    // ---- constant buffers --------------------------------------------------------
    D3D11_MAPPED_SUBRESOURCE mp{};
    if (FAILED(ctx->Map(m_cbPairs.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) { Commit(ctx, cur); return false; }
    memcpy(mp.pData, pairData, sizeof(pairData));
    memcpy((uint8_t*)mp.pData + sizeof(pairData), counts, sizeof(counts));
    {
        const float params[4] = { m_nearReject, m_egoOrigin, 0.0f, 0.0f };
        memcpy((uint8_t*)mp.pData + sizeof(pairData) + sizeof(counts), params, sizeof(params));
        memcpy((uint8_t*)mp.pData + sizeof(pairData) + sizeof(counts) + sizeof(params), allPairs, sizeof(allPairs));
    }
    ctx->Unmap(m_cbPairs.Get(), 0);
    if (!WriteDims(ctx, w, h, x0, y0, fullW, fullH)) { Commit(ctx, cur); return false; }

    const UINT keep = (UINT)-1;
    ID3D11ShaderResourceView*  nullSrv[2] = {};
    ID3D11UnorderedAccessView* nullUav = nullptr;

    // ---- pass A: per-layer medoid R ----------------------------------------------
    {
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
    m_solveGood = !fs.worldMiss;                       // v0.6.1: a reusable R_world is in the solve buffer
    // ---- pass B: per-pixel reprojection -> MV texture (u0) + flattened R32F depth (u1, v0.5.7) --------
    DispatchReproj(ctx, w, h, depthTwinSrv, depthR32Uav, mvUav);

    SolveReadback(ctx);
    RingStep(ctx);
    LogFrameLine(fs, cur);
    Commit(ctx, cur);
    return true;
}

void CameraMv::Shutdown() {
    m_stageSnap.Reset(); m_stageSolve.Reset(); m_stageDbg.Reset();
    for (int i = 0; i < kRing; ++i) { m_ring[i].Reset(); m_ringPending[i] = false; }
    m_cbDims.Reset(); m_cbPairs.Reset();
    m_solveSrv.Reset(); m_solveUav.Reset(); m_solve.Reset();
    m_prev.Shutdown();
    m_csReproj.Reset(); m_csSolve.Reset();
    m_dev.Reset();
    m_ready = false; m_failed = false;
}

#endif // WITH_DLAA
