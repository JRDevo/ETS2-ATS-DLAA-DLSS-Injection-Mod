// WireWiden implementation -- phase 61 / 62 / 63. See wire_widen.h.
#ifdef WITH_DLAA

#include "wire_widen.h"
#include <atomic>
#include <cstring>
#include <wrl/client.h>
#ifndef WW_NO_D3D
#include "shader_cache.h"
#include "log.h"
#endif

using Microsoft::WRL::ComPtr;

namespace WireWiden {

const uint32_t kWireVs[4] = { 0x4186dac5u, 0xa4ee75cfu, 0xb37e764cu, 0xa4e7601cu };
const uint32_t kWirePs[4] = { 0xa5aa1c2du, 0x6137ad37u, 0x6a913e27u, 0xa555d6e0u };
// phase 62: the PSs with the wire signature whose alpha is tex6.a * COLOR.w (ATS capture ats_flat_fence_frame1634, disassembly in
// captures\*.ps_<hash>.txt). PLAIN = no discard; CUT = the same + `discard` below alpha 0.05 (the pole hardware: jumper loops, wire
// ends, insulators -- PS b97c44a3, 153 draws in that frame). The others with the signature output alpha 1 -> not fadable, not listed.
const uint32_t kWirePsPlain[kWirePsPlainN][4] = {
    { 0xa5aa1c2du, 0x6137ad37u, 0x6a913e27u, 0xa555d6e0u },   // the long wires (ETS2 + ATS)
    { 0x1a104d6eu, 0x7f5ae9d1u, 0xebcf2df4u, 0xf6fb8629u },
};
const uint32_t kWirePsCut[kWirePsCutN][4] = {
    { 0xb97c44a3u, 0x49f67c39u, 0xb76a9c05u, 0xd3ed2127u },   // the pole hardware (ATS) -- drawn with the mod's copy (kWireCutPs)
};
// (cedce1aa / 3cc84c95 have the same alpha path + cut-off but other colour maths and 1 draw each in the capture: not copied, so
//  not widened)
// phase 63: the solid G-buffer pairs (A: far high-voltage wires, B: lattice towers; ATS capture amtrucks_2026.10.18_19.23.51)
const uint32_t kSolidVsA[4] = { 0x07dfaa00u, 0xd29866d9u, 0x6d22e7e7u, 0xdbbda59fu };
const uint32_t kSolidPsA[4] = { 0xb67da69fu, 0xf026275bu, 0xee64a79au, 0x32ca5a22u };
const uint32_t kSolidVsB[4] = { 0x6c157c5fu, 0x646cb3efu, 0xe4e0cf7du, 0x483196a8u };
const uint32_t kSolidPsB[4] = { 0x34860eedu, 0x2baa7a97u, 0xe6440e7du, 0x13d245edu };
// phase 66: pair B's second VS -- the STREET LIGHTS (shaft + curved arm + lamp head; ATS capture amtrucks_2026.10.18_20.34.48_frame1277
// EID 12285: all of them ONE DrawIndexedInstanced, 294 indices x 35 instances, PS 34860eed, blend off, depth written). Its output
// signature is byte-identical to 6c157c5f's (the OSGN chunk, 180 bytes: COLOR0, SV_Position, TEXCOORD0..3 in registers 0..5, same
// masks; bytecode from effect.scs, capturess_a484ef85.dxbc / .txt) -> GSolidB + PSSolidB unchanged. The instance comes in as
// vertex attributes (TEXCOORD4 = packed position, TEXCOORD5 = quaternion + scale); no SV_InstanceID anywhere, the GS needs nothing
// per instance (it runs per instance's triangle like any other).
const uint32_t kSolidVsB2[4] = { 0xa484ef85u, 0x250d1b5eu, 0xe47a5540u, 0x29d0b63fu };
int PsKind(const void* code, size_t len) {
    for (int i = 0; i < kWirePsPlainN; ++i) if (ChecksumIs(code, len, kWirePsPlain[i])) return 1;
    for (int i = 0; i < kWirePsCutN; ++i) if (ChecksumIs(code, len, kWirePsCut[i])) return 2;
    return 0;
}

bool ChecksumIs(const void* code, size_t len, const uint32_t want[4]) {
    if (!code || len < 20) return false;
    const unsigned char* b = static_cast<const unsigned char*>(code);
    if (b[0] != 'D' || b[1] != 'X' || b[2] != 'B' || b[3] != 'C') return false;
    for (int i = 0; i < 4; ++i) {
        const uint32_t d = (uint32_t)b[4 + 4 * i] | ((uint32_t)b[5 + 4 * i] << 8) | ((uint32_t)b[6 + 4 * i] << 16) |
                           ((uint32_t)b[7 + 4 * i] << 24);
        if (d != want[i]) return false;
    }
    return true;
}

namespace {
// Shader objects with the wire bytecode (identity only, no ref). A shader created at the address of a released wire shader with
// other bytecode removes the address (Note* sees every creation once the hooks are in).
constexpr int kSet = 64;
std::atomic<int> g_setFull{0};                       // inserts that found no free slot (stale addresses; logged by inject.cpp)
std::atomic<uintptr_t> g_vs[kSet] = {};
std::atomic<uintptr_t> g_ps[kSet] = {};
std::atomic<uintptr_t> g_psCut[kSet] = {};             // phase 62: the wire PSs with the alpha < 0.05 discard
std::atomic<uintptr_t> g_vsSolidA[kSet] = {}, g_vsSolidB[kSet] = {};   // phase 63: the solid G-buffer pairs
std::atomic<uintptr_t> g_psSolidA[kSet] = {}, g_psSolidB[kSet] = {};
void SetNote(std::atomic<uintptr_t>* set, uintptr_t key, bool wire) {
    if (!key) return;
    for (int i = 0; i < kSet; ++i)
        if (set[i].load(std::memory_order_acquire) == key) {
            if (!wire) set[i].store(0, std::memory_order_release);
            return;
        }
    if (!wire) return;
    for (int i = 0; i < kSet; ++i) {
        uintptr_t e = 0;
        if (set[i].compare_exchange_strong(e, key, std::memory_order_acq_rel)) return;
    }
    g_setFull.fetch_add(1, std::memory_order_relaxed);
}
bool SetHas(const std::atomic<uintptr_t>* set, uintptr_t key) {
    if (!key) return false;
    for (int i = 0; i < kSet; ++i) if (set[i].load(std::memory_order_acquire) == key) return true;
    return false;
}
int SetCount(const std::atomic<uintptr_t>* set) {
    int n = 0;
    for (int i = 0; i < kSet; ++i) if (set[i].load(std::memory_order_relaxed)) ++n;
    return n;
}
}

void NoteVs(const void* vs, const void* code, size_t len) {
    SetNote(g_vs, (uintptr_t)vs, ChecksumIs(code, len, kWireVs));
    SetNote(g_vsSolidA, (uintptr_t)vs, ChecksumIs(code, len, kSolidVsA));   // phase 63
    SetNote(g_vsSolidB, (uintptr_t)vs, ChecksumIs(code, len, kSolidVsB) || ChecksumIs(code, len, kSolidVsB2));   // phase 66: + lights
}
void NotePs(const void* ps, const void* code, size_t len) {
    const int k = PsKind(code, len);
    SetNote(g_ps, (uintptr_t)ps, k == 1);
    SetNote(g_psCut, (uintptr_t)ps, k == 2);
    SetNote(g_psSolidA, (uintptr_t)ps, ChecksumIs(code, len, kSolidPsA));   // phase 63
    SetNote(g_psSolidB, (uintptr_t)ps, ChecksumIs(code, len, kSolidPsB));
}
bool IsWireVs(const void* vs) { return SetHas(g_vs, (uintptr_t)vs); }
int  WirePsKind(const void* ps) { return SetHas(g_ps, (uintptr_t)ps) ? 1 : (SetHas(g_psCut, (uintptr_t)ps) ? 2 : 0); }
int  KnownVs() { return SetCount(g_vs); }
int  KnownPs() { return SetCount(g_ps) + SetCount(g_psCut); }
int  KnownPsCut() { return SetCount(g_psCut); }
int  SolidPsKind(const void* ps) { return SetHas(g_psSolidA, (uintptr_t)ps) ? 1 : (SetHas(g_psSolidB, (uintptr_t)ps) ? 2 : 0); }
bool IsSolidVs(const void* vs, int kind) { return kind == 1 ? SetHas(g_vsSolidA, (uintptr_t)vs) : (kind == 2 && SetHas(g_vsSolidB, (uintptr_t)vs)); }
int  KnownSolid() { return SetCount(g_vsSolidA) + SetCount(g_vsSolidB) + SetCount(g_psSolidA) + SetCount(g_psSolidB); }
int  SetFull() { return g_setFull.load(std::memory_order_relaxed); }

namespace {
// The wire VS output = the wire PS input (COLOR0, SV_Position, TEXCOORD0..2, TEXCOORD6 in this order, registers 0..5). The GS
// passes every attribute through; only SV_Position (the band corners) changes, and the coverage factor k goes either into COLOR.w
// (GSMain: the game's own PS multiplies its alpha by it) or into an extra TEXCOORD7 (GSCut: for the mod's copy of the pole-hardware
// PS, which must run the game's alpha cut-off on the UNSCALED alpha -- phase 62).
// kWireGs-BEGIN (the build validates this block with fxc, gs_5_0 / GSMain and GSCut)
const char kWireGs[] = R"(
struct V {
    float4 col : COLOR0;
    float4 pos : SV_Position;
    float3 t0  : TEXCOORD0;
    float3 t1  : TEXCOORD1;
    float2 t2  : TEXCOORD2;
    float3 t6  : TEXCOORD6;
};
struct VK {
    float4 col : COLOR0;
    float4 pos : SV_Position;
    float3 t0  : TEXCOORD0;
    float3 t1  : TEXCOORD1;
    float2 t2  : TEXCOORD2;
    float3 t6  : TEXCOORD6;
    float  k   : TEXCOORD7;
};
cbuffer WireCb : register(b0) {
    float4 g_p;          // x, y = half the viewport in pixels, z = the band width in pixels (wire_min_px),
                         // w = 1: the draw blends over (INV_SRC_ALPHA), 0: additive (ONE)
};
struct Band {
    bool   widen;
    bool   pos;          // the original triangle's winding in this screen space (a2 >= 0)
    int    sh, s1, sf;   // shortest edge sh -> s1, far vertex sf
    float2 m, f, n;      // the band's ends (screen px) and its half-width normal (phase 67: W = the tent's half base)
    float  k;            // coverage factor (phase 67: the tent's peak) of each of the two triangles of a ribbon quad
};
// A triangle whose height h over its longest edge is below W = g_p.z becomes a band W wide from the middle of its SHORTEST edge
// (the strip's end, across its width) to the opposite (far) vertex. The two triangles of a ribbon quad (two corners of one end +
// one of the other, whichever diagonal splits it and whichever edge is the longest) give two parallel bands h / 2 apart, w / 4
// either side of the strip's middle line: one ~W-wide line, no 2 px smear. Coverage: the triangle's area (L h / 2) over the
// band's (L W) per triangle, the pair together r = h / W of the strip:
//   additive (ONE): k + k -> k = r / 2  (the overlap does not matter);
//   over (INV_SRC_ALPHA): overlap W - h / 2 at 1 - (1 - k)^2, two edges h / 2 at k -> 2k - k^2 (1 - u) = r, u = r / 2
//   -> k = (1 - sqrt(1 - (1 - u) r)) / (1 - u), 0 .. 0.586. The ENERGY is continuous with the untouched triangles at h = W (the
//   pair's ~1.5 W band at mean r = 1 vs the 1 px line); the width steps from 1 to ~1.5 W there. A sliver of another mesh whose
//   shortest edge is not tiny gets its band over m -> f only (up to 25 % fainter, never darker). A zero-area triangle (duplicate
//   vertex, collinear) passes through unchanged (the game draws nothing there, neither do we).
// phase 67 (user: at wire_min_px 1.0 the wires look jagged): a HARD-edged band 1 px wide covers exactly one pixel centre per column;
// with the jitter DLAA's history sees a tent (box 1 px x jitter 1 px): the line's peak darkness swings 2 : 1 along a sloped wire
// (1 px dark where it crosses a pixel centre, 2 px half-dark between) = beads / stair steps. So the band has SOFT edges: a tent
// profile 2W wide (the coverage k at the middle line, 0 at both edges, linear between: three rails, two quads) -- the same ink as
// the W box (half base W x peak k), the same half-height width W, and the ripple 1.55 : 1 at W = 1 (1.37 : 1 at 1.2; the old box
// at 1.5 px: 1.45 : 1; 8 Halton phases, see the harness S61 unit). The over-blend k for two tents h / 2 apart:
//   2k - k^2 g = r, g = (1/W) * integral tent(x) tent(x - h/2) = 2/3 - u^2 + u^3 / 2 (u = r / 2) -> k = (1 - sqrt(1 - g r)) / g.
Band MakeBand(V v[3]) {
    Band b = (Band)0;
    const float W = g_p.z;
    b.widen = W > 0.0 && v[0].pos.w > 1e-4 && v[1].pos.w > 1e-4 && v[2].pos.w > 1e-4;
    float2 s[3];
    [unroll] for (int i = 0; i < 3; ++i) s[i] = b.widen ? v[i].pos.xy / v[i].pos.w * g_p.xy : float2(0, 0);
    const float l0 = dot(s[1] - s[0], s[1] - s[0]), l1 = dot(s[2] - s[1], s[2] - s[1]), l2 = dot(s[0] - s[2], s[0] - s[2]);
    const float a2 = (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[2].x - s[0].x) * (s[1].y - s[0].y);
    const float lmax = sqrt(max(l0, max(l1, l2)));
    const float h = lmax > 1e-3 ? abs(a2) / lmax : W;
    b.widen = b.widen && lmax > 1e-3 && h < W && abs(a2) > 0.0;
    b.sh = 0;
    if (l1 < l0 && l1 <= l2) b.sh = 1;
    else if (l2 < l0 && l2 < l1) b.sh = 2;
    b.s1 = b.sh == 2 ? 0 : b.sh + 1;
    b.sf = b.s1 == 2 ? 0 : b.s1 + 1;
    b.m = 0.5 * (s[b.sh] + s[b.s1]);
    b.f = s[b.sf];
    const float2 d = b.f - b.m;
    const float dl = length(d);
    b.widen = b.widen && dl >= 1e-3;
    const float2 e = dl >= 1e-3 ? d / dl : float2(1, 0);
    b.n = float2(-e.y, e.x) * W;
    const float r = saturate(h / W);
    const float u = 0.5 * r;
    const float g = 2.0 / 3.0 - u * u + 0.5 * u * u * u;
    b.k = g_p.w > 0.5 ? (1.0 - sqrt(max(0.0, 1.0 - g * r))) / g : 0.5 * r;
    b.pos = a2 >= 0.0;   // the strip's first triangle (m + n, m - n, f + n) has winding +1 in this screen space
    return b;
}
V Mid(V a, V b) {                        // the middle of a (tiny) edge: clip position + attributes averaged
    V o;
    o.col = 0.5 * (a.col + b.col); o.pos = 0.5 * (a.pos + b.pos);
    o.t0 = 0.5 * (a.t0 + b.t0); o.t1 = 0.5 * (a.t1 + b.t1); o.t2 = 0.5 * (a.t2 + b.t2); o.t6 = 0.5 * (a.t6 + b.t6);
    return o;
}
V At(V v, float2 s) { V o = v; o.pos.xy = s / g_p.xy * v.pos.w; return o; }
VK ToVK(V v, float k) {
    VK o;
    o.col = v.col; o.pos = v.pos; o.t0 = v.t0; o.t1 = v.t1; o.t2 = v.t2; o.t6 = v.t6; o.k = k;
    return o;
}
// phase 67, the tent: rails +n (0), middle (k), -n (0) at both ends -> two 4-vertex strips (upper half, lower half), each in the old
// corner order with one rail replaced by the middle one (it lies between +n and -n, so each half keeps the triangle's winding).
V Wk(V v, float2 s, float k) { V o = At(v, s); o.col.w *= k; return o; }
// GSMain: the game's own PS draws the band; the tent scales COLOR.w (its alpha = tex.a * COLOR.w)
[maxvertexcount(8)]
void GSMain(triangle V v[3], inout TriangleStream<V> os) {
    const Band b = MakeBand(v);
    if (!b.widen) { os.Append(v[0]); os.Append(v[1]); os.Append(v[2]); return; }
    const V vm = Mid(v[b.sh], v[b.s1]);
    const V vf = v[b.sf];
    const V mU = Wk(vm, b.m + b.n, 0.0), mC = Wk(vm, b.m, b.k), mL = Wk(vm, b.m - b.n, 0.0);
    const V fU = Wk(vf, b.f + b.n, 0.0), fC = Wk(vf, b.f, b.k), fL = Wk(vf, b.f - b.n, 0.0);
    if (b.pos) { os.Append(mU); os.Append(mC); os.Append(fU); os.Append(fC); os.RestartStrip();
                 os.Append(mC); os.Append(mL); os.Append(fC); os.Append(fL); }
    else       { os.Append(mC); os.Append(mU); os.Append(fC); os.Append(fU); os.RestartStrip();
                 os.Append(mL); os.Append(mC); os.Append(fL); os.Append(fC); }
}
// GSCut (phase 62): the mod's copy of the pole-hardware PS draws the band; COLOR.w stays the game's, the tent k travels in TEXCOORD7
[maxvertexcount(8)]
void GSCut(triangle V v[3], inout TriangleStream<VK> os) {
    const Band b = MakeBand(v);
    if (!b.widen) { os.Append(ToVK(v[0], 1.0)); os.Append(ToVK(v[1], 1.0)); os.Append(ToVK(v[2], 1.0)); return; }
    const V vm = Mid(v[b.sh], v[b.s1]);
    const V vf = v[b.sf];
    const VK mU = ToVK(At(vm, b.m + b.n), 0.0), mC = ToVK(At(vm, b.m), b.k), mL = ToVK(At(vm, b.m - b.n), 0.0);
    const VK fU = ToVK(At(vf, b.f + b.n), 0.0), fC = ToVK(At(vf, b.f), b.k), fL = ToVK(At(vf, b.f - b.n), 0.0);
    if (b.pos) { os.Append(mU); os.Append(mC); os.Append(fU); os.Append(fC); os.RestartStrip();
                 os.Append(mC); os.Append(mL); os.Append(fC); os.Append(fL); }
    else       { os.Append(mC); os.Append(mU); os.Append(fC); os.Append(fU); os.RestartStrip();
                 os.Append(mL); os.Append(mC); os.Append(fL); os.Append(fC); }
}
)";
// kWireGs-END

// phase 62: the game's pole-hardware PS b97c44a3 (ATS; captures\ats_flat_fence_frame1634.ps_b97c44a3.txt), instruction for
// instruction, with ONE change: the output alpha is multiplied by the band's coverage k (TEXCOORD7) AFTER the game's cut-off test,
// which stays on the unscaled alpha tex.a * COLOR.w. Why not just scale COLOR.w (GSMain): far away the texture's mips carry alpha
// 0.05 .. 0.2 only (captures\ats_flat_fence_frame1634.tex_alpha.txt), so any fade below 1 drops the texel under the game's 0.05
// cut-off -- the pole pieces would vanish instead of dash. Bound only for the widened draws of that PS; the game's resources
// (cb0 at b0, t6 / t10 / t11, s0) are the game's own bindings. The harness renders it next to the game's real bytecode (k = 1).
// kWireCutPs-BEGIN (the build validates this block with fxc, ps_5_0 / PSCut)
const char kWireCutPs[] = R"(
struct VK {
    float4 col : COLOR0;
    float4 pos : SV_Position;
    float3 t0  : TEXCOORD0;
    float3 t1  : TEXCOORD1;
    float2 t2  : TEXCOORD2;
    float3 t6  : TEXCOORD6;
    float  k   : TEXCOORD7;
};
cbuffer cbuffer0 : register(b0) { float4 cb0[7]; };
Texture2D<float4> texture6  : register(t6);
Texture2D<float4> texture10 : register(t10);
Texture2D<float4> texture11 : register(t11);
SamplerState      sampler0  : register(s0);
float4 PSCut(VK v) : SV_Target {
    const float4 r0 = texture6.Sample(sampler0, v.t2);               //  0
    const float a = r0.w * v.col.w;                                   //  1
    if (a < 0.049988) discard;                                        //  2 .. 4 (the game's cut-off, unscaled)
    float2 p = v.t6.yz / v.t6.x;                                      //  5
    p = p * cb0[4].xy;                                                //  6
    const float2 sc = (1.0 / cb0[5].zw) * cb0[5].xy;                  //  7, 8
    const int2 ip = (int2)(sc * p);                                   //  9, 10
    const float3 l11 = texture11.Load(int3(ip, 0)).xyz;               // 12
    float3 l10 = texture10.Load(int3(ip, 0)).xyz;                     // 13
    l10 = cb0[6].xyz * cb0[0].www + l10;                              // 14
    float3 r3 = r0.www * cb0[1].xyz;                                  // 15
    float3 c = r0.xyz * cb0[0].xyz;                                   // 16
    const float3 r4 = v.col.xyz + v.col.xyz;                          // 17
    r3 = r3 * r4;                                                     // 18
    c = c * r4;                                                       // 19
    const float3 r2 = l11 * r3;                                       // 20
    return float4(l10 * c + r2, a * v.k);                             // 21, 3 (alpha * k: phase 62)
}
)";
// kWireCutPs-END
}

const char* GsSource(size_t* len) { if (len) *len = sizeof(kWireGs) - 1; return kWireGs; }
const char* CutPsSource(size_t* len) { if (len) *len = sizeof(kWireCutPs) - 1; return kWireCutPs; }

namespace {
// phase 63: SOLID thin geometry in the G-buffer (no blending, so no alpha to fade): far high-voltage wires (VS 07dfaa00 + PS
// b67da69f) and lattice towers (VS 6c157c5f + PS 34860eed; phase 66: + the street lights, instanced VS a484ef85) -- ATS capture amtrucks_2026.10.18_19.23.51_frame2443 (draws 2025 /
// 2866, captures\g_2025.*.txt / g_2866.*.txt). A triangle thinner than W becomes the same W-wide band as the wires (shortest-edge
// middle -> far vertex), and the mod's copy of the game's G-buffer PS keeps a pixel of the band in a share k of the frames
// (a per-pixel offset + the golden ratio per frame, g_q.x): DLAA's history averages the kept pixels into a faint continuous
// line, the way TAA renders dithered transparency. Both triangles of a quad use the same noise, so their overlap is not counted
// twice: kept share k over the pair's union W + h / 2 = the strip's area h -> k = r / (1 + r / 2), r = h / W, then at least
// wire_solid_min_share (default 0.1 since phase 68, was 0.25: a far wire at k ~ 0.04 would show 1 frame in 25 and flash; user
// A/B 2026-10-19 parked, Alt+F12: at 0.25 the far wires were 34 % darker than the game's own + flickered more (0.109 vs 0.076
// per-pixel frame-to-frame), at 0.1 the same ink as the game's (289 vs 291) and the same flicker). k = 1 for triangles that pass
// through (the copy is then exactly the game's PS). The G-buffer draw-id replay (no GS, depth EQUAL) gives the band pixels no draw
// id -> camera motion: right for these static wires / towers. The MV depth of a dropped band pixel is the background's.
// kSolidGs-BEGIN (the build validates this block with fxc, gs_5_0 / GSolidA and GSolidB)
const char kSolidGs[] = R"(
struct VA {
    float4 col : COLOR0;
    float4 pos : SV_Position;
    float3 t0  : TEXCOORD0;
    float3 t1  : TEXCOORD1;
    float2 t2  : TEXCOORD2;
};
struct VAK {
    float4 col : COLOR0;
    float4 pos : SV_Position;
    float3 t0  : TEXCOORD0;
    float3 t1  : TEXCOORD1;
    float2 t2  : TEXCOORD2;
    float4 kf  : TEXCOORD7;
};
struct VB {
    float4 col : COLOR0;
    float4 pos : SV_Position;
    float3 t0  : TEXCOORD0;
    float3 t1  : TEXCOORD1;
    float4 t2  : TEXCOORD2;
    float2 t3  : TEXCOORD3;
};
struct VBK {
    float4 col : COLOR0;
    float4 pos : SV_Position;
    float3 t0  : TEXCOORD0;
    float3 t1  : TEXCOORD1;
    float4 t2  : TEXCOORD2;
    float2 t3  : TEXCOORD3;
    float4 kf  : TEXCOORD7;
};
cbuffer SolidCb : register(b0) {
    float4 g_p;          // x, y = half the viewport in pixels, z = the band width W in pixels (wire_min_px)
    float4 g_q;          // x = the dither frame (0 .. 1023), y = the minimum keep share (wire_solid_min_share)
};
struct Band {
    bool   widen, pos;
    int    sh, s1, sf;
    float2 m, f, n;
    float  k;
};
Band MakeBand(float4 p0, float4 p1, float4 p2) {
    Band b = (Band)0;
    const float W = g_p.z;
    b.widen = W > 0.0 && p0.w > 1e-4 && p1.w > 1e-4 && p2.w > 1e-4;
    float2 s[3];
    s[0] = b.widen ? p0.xy / p0.w * g_p.xy : float2(0, 0);
    s[1] = b.widen ? p1.xy / p1.w * g_p.xy : float2(0, 0);
    s[2] = b.widen ? p2.xy / p2.w * g_p.xy : float2(0, 0);
    const float l0 = dot(s[1] - s[0], s[1] - s[0]), l1 = dot(s[2] - s[1], s[2] - s[1]), l2 = dot(s[0] - s[2], s[0] - s[2]);
    const float a2 = (s[1].x - s[0].x) * (s[2].y - s[0].y) - (s[2].x - s[0].x) * (s[1].y - s[0].y);
    const float lmax = sqrt(max(l0, max(l1, l2)));
    const float h = lmax > 1e-3 ? abs(a2) / lmax : W;
    b.widen = b.widen && lmax > 1e-3 && h < W && abs(a2) > 0.0;
    b.sh = 0;
    if (l1 < l0 && l1 <= l2) b.sh = 1;
    else if (l2 < l0 && l2 < l1) b.sh = 2;
    b.s1 = b.sh == 2 ? 0 : b.sh + 1;
    b.sf = b.s1 == 2 ? 0 : b.s1 + 1;
    b.m = 0.5 * (s[b.sh] + s[b.s1]);
    b.f = s[b.sf];
    const float2 d = b.f - b.m;
    const float dl = length(d);
    b.widen = b.widen && dl >= 1e-3;
    // phase 65: a ROUND wire (a tube) has faces seen edge-on at its silhouette -- sub-pixel slivers of a wire that is already wider
    // than the band. Widening those adds a dark fringe on both sides (user: close wires too black / thick). So the face's real
    // THICKNESS is measured in 3D: its height over its longest edge, in the scaled clip coordinates (x * half vp x, y * half vp y,
    // w * half vp x: w treated like x, i.e. P00 taken as 1 -- an under-estimate, so a borderline face still widens) divided by the
    // triangle's mean w = the face's width in pixels if it faced the camera. (Not the shortest edge's length: the far HV wire's
    // faces are sheared -- capture draw 2025: the "across" edge lies ~9 deg off the wire, 0.65 px long, 0.1 px thick.) A tube of
    // n faces is face / sin(pi / n) wide: 1.4 x (4) .. 2 x (6) .. 2.6 x (8) -> 2 x as the middle. Only when that estimated wire is
    // thinner than the band does the face widen (far wires yes; a 3 cm wire closer than ~75 m at 4K no).
    const float wm = max((p0.w + p1.w + p2.w) / 3.0, 1e-4);
    const float3 q0 = float3(p0.x * g_p.x, p0.y * g_p.y, p0.w * g_p.x) / wm;
    const float3 q1 = float3(p1.x * g_p.x, p1.y * g_p.y, p1.w * g_p.x) / wm;
    const float3 q2 = float3(p2.x * g_p.x, p2.y * g_p.y, p2.w * g_p.x) / wm;
    const float e3 = sqrt(max(dot(q1 - q0, q1 - q0), max(dot(q2 - q1, q2 - q1), dot(q0 - q2, q0 - q2))));
    const float faceW = e3 > 1e-4 ? length(cross(q1 - q0, q2 - q0)) / e3 : 0.0;
    b.widen = b.widen && 2.0 * faceW < W;
    const float2 e = dl >= 1e-3 ? d / dl : float2(1, 0);
    b.n = float2(-e.y, e.x) * W;                 // phase 67: the tent's half base (see kWireGs): keep share k in the middle, 0 at the edges
    const float r = saturate(h / W);
    // the pair factor, then the keep-share floor: DLSS validates its history against the current frame's neighbourhood, so a
    // pixel that shows the wire in 1 frame of 25 (k ~ 0.04 for a far wire) flashes instead of building a faint line -- a share of
    // at least g_q.y keeps it temporally dense (darker than the real 0.1 px wire, like the game's own dashes, but continuous).
    // phase 67 tent pair (same noise -> the union = the max of two tents h / 2 apart): area W k (1 + r / 2 - r^2 / 16) = h. The
    // floor is applied PER PIXEL in the PS, to the tent's inner half (lambda >= 0.5: the old box's W): every pixel there keeps at
    // least g_q.y as before; the soft outer half fades to 0 (a floor there would make the wire 2 W wide and dark).
    b.k = r / (1.0 + 0.5 * r - 0.0625 * r * r);
    b.pos = a2 >= 0.0;
    return b;
}
float4 AtPos(float4 p, float2 s) { return float4(s / g_p.xy * p.w, p.zw); }
// kf (phase 67) = (keep share k x lambda, the dither frame, lambda = 1 on the middle rail .. 0 on the edge rails, the floor: g_q.y on
// every vertex of a widened band, 0 for a pass-through triangle (k = 1, lambda = 1: always kept, the game's PS exactly))
VAK ToA(VA v, float k, float lam, float fl) { VAK o; o.col = v.col; o.pos = v.pos; o.t0 = v.t0; o.t1 = v.t1; o.t2 = v.t2; o.kf = float4(k * lam, g_q.x, lam, fl); return o; }
VBK ToB(VB v, float k, float lam, float fl) { VBK o; o.col = v.col; o.pos = v.pos; o.t0 = v.t0; o.t1 = v.t1; o.t2 = v.t2; o.t3 = v.t3; o.kf = float4(k * lam, g_q.x, lam, fl); return o; }
VA MidA(VA a, VA b) {
    VA o; o.col = 0.5 * (a.col + b.col); o.pos = 0.5 * (a.pos + b.pos); o.t0 = 0.5 * (a.t0 + b.t0); o.t1 = 0.5 * (a.t1 + b.t1);
    o.t2 = 0.5 * (a.t2 + b.t2); return o;
}
VB MidB(VB a, VB b) {
    VB o; o.col = 0.5 * (a.col + b.col); o.pos = 0.5 * (a.pos + b.pos); o.t0 = 0.5 * (a.t0 + b.t0); o.t1 = 0.5 * (a.t1 + b.t1);
    o.t2 = 0.5 * (a.t2 + b.t2); o.t3 = 0.5 * (a.t3 + b.t3); return o;
}
[maxvertexcount(8)]
void GSolidA(triangle VA v[3], inout TriangleStream<VAK> os) {
    const Band b = MakeBand(v[0].pos, v[1].pos, v[2].pos);
    if (!b.widen) { os.Append(ToA(v[0], 1.0, 1.0, 0.0)); os.Append(ToA(v[1], 1.0, 1.0, 0.0)); os.Append(ToA(v[2], 1.0, 1.0, 0.0)); return; }
    VA vm = MidA(v[b.sh], v[b.s1]);
    VA vf = v[b.sf];
    VAK mU = ToA(vm, b.k, 0.0, g_q.y), mC = ToA(vm, b.k, 1.0, g_q.y), mL = ToA(vm, b.k, 0.0, g_q.y),
        fU = ToA(vf, b.k, 0.0, g_q.y), fC = ToA(vf, b.k, 1.0, g_q.y), fL = ToA(vf, b.k, 0.0, g_q.y);
    mU.pos = AtPos(vm.pos, b.m + b.n); mC.pos = AtPos(vm.pos, b.m); mL.pos = AtPos(vm.pos, b.m - b.n);
    fU.pos = AtPos(vf.pos, b.f + b.n); fC.pos = AtPos(vf.pos, b.f); fL.pos = AtPos(vf.pos, b.f - b.n);
    if (b.pos) { os.Append(mU); os.Append(mC); os.Append(fU); os.Append(fC); os.RestartStrip();
                 os.Append(mC); os.Append(mL); os.Append(fC); os.Append(fL); }
    else       { os.Append(mC); os.Append(mU); os.Append(fC); os.Append(fU); os.RestartStrip();
                 os.Append(mL); os.Append(mC); os.Append(fL); os.Append(fC); }
}
[maxvertexcount(8)]
void GSolidB(triangle VB v[3], inout TriangleStream<VBK> os) {
    const Band b = MakeBand(v[0].pos, v[1].pos, v[2].pos);
    if (!b.widen) { os.Append(ToB(v[0], 1.0, 1.0, 0.0)); os.Append(ToB(v[1], 1.0, 1.0, 0.0)); os.Append(ToB(v[2], 1.0, 1.0, 0.0)); return; }
    VB vm = MidB(v[b.sh], v[b.s1]);
    VB vf = v[b.sf];
    VBK mU = ToB(vm, b.k, 0.0, g_q.y), mC = ToB(vm, b.k, 1.0, g_q.y), mL = ToB(vm, b.k, 0.0, g_q.y),
        fU = ToB(vf, b.k, 0.0, g_q.y), fC = ToB(vf, b.k, 1.0, g_q.y), fL = ToB(vf, b.k, 0.0, g_q.y);
    mU.pos = AtPos(vm.pos, b.m + b.n); mC.pos = AtPos(vm.pos, b.m); mL.pos = AtPos(vm.pos, b.m - b.n);
    fU.pos = AtPos(vf.pos, b.f + b.n); fC.pos = AtPos(vf.pos, b.f); fL.pos = AtPos(vf.pos, b.f - b.n);
    if (b.pos) { os.Append(mU); os.Append(mC); os.Append(fU); os.Append(fC); os.RestartStrip();
                 os.Append(mC); os.Append(mL); os.Append(fC); os.Append(fL); }
    else       { os.Append(mC); os.Append(mU); os.Append(fC); os.Append(fU); os.RestartStrip();
                 os.Append(mL); os.Append(mC); os.Append(fL); os.Append(fC); }
}
)";
// kSolidGs-END

// phase 63: the game's G-buffer PSs b67da69f (A: far HV wires) and 34860eed (B: lattice towers), instruction for instruction
// (captures\g_2025.ps.txt / g_2866.ps.txt), + the dither cut at the top: keep the pixel with probability k (TEXCOORD7.x; 1 =
// always: then exactly the game's PS). The harness renders both next to the game's real bytecode.
// kSolidPs-BEGIN (the build validates this block with fxc, ps_5_0 / PSSolidA and PSSolidB)
const char kSolidPs[] = R"(
struct VAK {
    float4 col : COLOR0;
    float4 pos : SV_Position;
    float3 t0  : TEXCOORD0;
    float3 t1  : TEXCOORD1;
    float2 t2  : TEXCOORD2;
    float4 kf  : TEXCOORD7;
};
struct VBK {
    float4 col : COLOR0;
    float4 pos : SV_Position;
    float3 t0  : TEXCOORD0;
    float3 t1  : TEXCOORD1;
    float4 t2  : TEXCOORD2;
    float2 t3  : TEXCOORD3;
    float4 kf  : TEXCOORD7;
};
cbuffer cbuffer0 : register(b0) { float4 cb0[4]; };
Texture2D<float4> texture0 : register(t0);
Texture2D<float4> texture1 : register(t1);
Texture2D<float4> texture6 : register(t6);
SamplerState      sampler0 : register(s0);
SamplerState      sampler1 : register(s1);
SamplerState      sampler2 : register(s2);
// per pixel: a spatial offset (interleaved gradient noise, Jimenez 2014) + the frame times the golden ratio -- every pixel is kept
// in exactly its share k of the frames, evenly spaced in time (no clumps a short TAA history would see as twinkle), and
// neighbouring pixels are out of phase. Keep the pixel when the value < k. Phase 67: k = the tent (kf.x), raised to the floor kf.w
// on the tent's inner half (lambda kf.z >= 0.5) -- the per-pixel temporal density the keep floor exists for.
bool Dropped(float2 px, float4 kf) {
    const float ign = frac(52.9829189 * frac(dot(floor(px), float2(0.06711056, 0.00583715))));
    const float n = frac(ign + 0.6180339887 * kf.y);
    const float k = kf.z >= 0.5 ? max(kf.x, kf.w) : kf.x;
    return n >= k;
}
uint Tail(out uint w) {                      // o3.z / o3.w of both PSs (cb0[2].x, cb0[3].x)
    const uint q = (uint)min(cb0[2].x * 4096.0, 16383.0);
    const uint t = (uint)cb0[3].x;
    w = ((t << 13) & 0x2000u) | (8064u & ~0x2000u);   // bfi(1, 13, t, 8064)
    return q << 2;
}
void PSSolidA(VAK v, out float4 o0 : SV_Target0, out float4 o1 : SV_Target1, out float4 o2 : SV_Target2, out uint4 o3 : SV_Target3) {
    if (Dropped(v.pos.xy, v.kf)) discard;
    o0.xyz = rsqrt(dot(v.t0, v.t0)) * v.t0;
    o0.w = v.t1.z;
    float4 r0 = texture6.Sample(sampler0, v.t2);
    r0.w = saturate(r0.w * cb0[2].z + cb0[2].w);
    r0.xyz = r0.xyz * cb0[0].xyz;
    o1.xyz = r0.www * cb0[1].xyz;
    o1.w = cb0[1].w;
    const float3 r1 = v.col.xyz + v.col.xyz;
    o2.xyz = r0.xyz * r1;
    o2.w = cb0[0].w;
    uint w;
    o3.z = Tail(w); o3.w = w; o3.xy = uint2(0, 0);
}
void PSSolidB(VBK v, out float4 o0 : SV_Target0, out float4 o1 : SV_Target1, out float4 o2 : SV_Target2, out uint4 o3 : SV_Target3) {
    if (Dropped(v.pos.xy, v.kf)) discard;
    o0.xyz = rsqrt(dot(v.t0, v.t0)) * v.t0;
    o0.w = v.t1.z;
    float4 r0 = texture1.Sample(sampler2, v.t3);
    float4 r1 = -r0 + 1.0;
    r0 = v.col.wwww * r1 + r0;
    r1 = texture6.Sample(sampler0, v.t2.xy);
    const float4 r2 = texture0.Sample(sampler1, v.t2.zw);
    r1 = r1 * r2;
    r0 = r0 * r1;
    o1.xyz = r0.www * cb0[1].xyz;
    r0.xyz = r0.xyz * cb0[0].xyz;
    o1.w = cb0[1].w;
    const float3 c2 = v.col.xyz + v.col.xyz;
    o2.xyz = r0.xyz * c2;
    o2.w = cb0[0].w;
    uint w;
    o3.z = Tail(w); o3.w = w; o3.xy = uint2(0, 0);
}
)";
// kSolidPs-END
}

const char* SolidGsSource(size_t* len) { if (len) *len = sizeof(kSolidGs) - 1; return kSolidGs; }
const char* SolidPsSource(size_t* len) { if (len) *len = sizeof(kSolidPs) - 1; return kSolidPs; }

#ifndef WW_NO_D3D
namespace {
ComPtr<ID3D11GeometryShader> g_gs, g_gsCut;
ComPtr<ID3D11PixelShader>    g_psCutObj;
ComPtr<ID3D11Buffer>         g_cb;
ComPtr<ID3D11GeometryShader> g_gsSolid[2];          // phase 63: GSolidA / GSolidB
ComPtr<ID3D11PixelShader>    g_psSolid[2];          //           PSSolidA / PSSolidB
ComPtr<ID3D11Buffer>         g_cbSolid;             //           32 bytes: g_p + g_q (frame)
float                        g_cbSolidVal[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
bool                         g_solidFailed = false;
ID3D11Device*                g_dev = nullptr;
bool                         g_failed = false, g_cutFailed = false;
float                        g_cbVal[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
ID3D11Buffer*                g_prevCb = nullptr;
ID3D11PixelShader*           g_prevPs = nullptr;    // the game's PS while the cut copy is bound (ref held until Unbind)
bool                         g_psSwapped = false;
struct BsMemo { ID3D11BlendState* bs; int mode; };
constexpr int kBsMemo = 64;
BsMemo                       g_bsMemo[kBsMemo] = {};
int                          g_bsMemoN = 0;
Stats                        g_st;

bool Ensure(ID3D11Device* dev) {
    if (!dev) return false;
    if (g_dev == dev && g_gs && g_cb) return true;
    if (g_dev == dev && g_failed) return false;
    if (!ShaderCache::Ready(ShaderCache::kWireGs)) { ++g_st.skipNotReady; return false; }
    if (g_dev != dev) Reset();
    g_dev = dev; g_failed = true;
    size_t sz = 0;
    const void* code = ShaderCache::Code(ShaderCache::kWireGs, &sz);
    if (!code) { LogErr("wire widening: GS compile failed %s -- off", ShaderCache::Error(ShaderCache::kWireGs)); return false; }
    HRESULT hr = dev->CreateGeometryShader(code, sz, nullptr, &g_gs);
    if (FAILED(hr)) { LogErr("wire widening: CreateGeometryShader hr=0x%lx -- off", (unsigned long)hr); return false; }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = dev->CreateBuffer(&bd, nullptr, &g_cb);
    if (FAILED(hr)) { LogErr("wire widening: constant buffer hr=0x%lx -- off", (unsigned long)hr); g_gs.Reset(); return false; }
    g_cbVal[0] = g_cbVal[1] = g_cbVal[2] = g_cbVal[3] = -1.0f;
    g_failed = false;
    return true;
}
// phase 62: the pole-hardware pair (GSCut + the PS copy), separately: its failure only leaves those draws un-widened
bool EnsureCut(ID3D11Device* dev) {
    if (g_gsCut && g_psCutObj) return true;
    if (g_cutFailed) return false;
    if (!ShaderCache::Ready(ShaderCache::kWireGsCut) || !ShaderCache::Ready(ShaderCache::kWireCutPs)) { ++g_st.skipNotReady; return false; }
    g_cutFailed = true;
    size_t gs = 0, ps = 0;
    const void* gc = ShaderCache::Code(ShaderCache::kWireGsCut, &gs);
    const void* pc = ShaderCache::Code(ShaderCache::kWireCutPs, &ps);
    if (!gc || !pc) {
        LogErr("wire widening: pole-hardware shaders failed to compile (%s / %s) -- the pole pieces stay as the game draws them",
            ShaderCache::Error(ShaderCache::kWireGsCut), ShaderCache::Error(ShaderCache::kWireCutPs));
        return false;
    }
    if (FAILED(dev->CreateGeometryShader(gc, gs, nullptr, &g_gsCut)) || FAILED(dev->CreatePixelShader(pc, ps, nullptr, &g_psCutObj))) {
        LogErr("wire widening: pole-hardware shader objects failed -- the pole pieces stay as the game draws them");
        g_gsCut.Reset(); g_psCutObj.Reset();
        return false;
    }
    g_cutFailed = false;
    return true;
}

// phase 63: the solid pairs' GS + PS copies, separately (a failure only leaves those draws as the game draws them)
bool EnsureSolid(ID3D11Device* dev) {
    if (g_gsSolid[0] && g_gsSolid[1] && g_psSolid[0] && g_psSolid[1] && g_cbSolid) return true;
    if (g_solidFailed) return false;
    const ShaderCache::Id ids[4] = { ShaderCache::kSolidGsA, ShaderCache::kSolidGsB, ShaderCache::kSolidPsA, ShaderCache::kSolidPsB };
    for (ShaderCache::Id id : ids) if (!ShaderCache::Ready(id)) { ++g_st.skipNotReady; return false; }
    g_solidFailed = true;
    for (int i = 0; i < 2; ++i) {
        size_t gs = 0, ps = 0;
        const void* gc = ShaderCache::Code(ids[i], &gs);
        const void* pc = ShaderCache::Code(ids[2 + i], &ps);
        if (!gc || !pc || FAILED(dev->CreateGeometryShader(gc, gs, nullptr, &g_gsSolid[i])) ||
            FAILED(dev->CreatePixelShader(pc, ps, nullptr, &g_psSolid[i]))) {
            LogErr("wire widening: solid-geometry shaders failed (%s / %s) -- far solid wires and towers stay as the game draws them",
                ShaderCache::Error(ids[i]), ShaderCache::Error(ids[2 + i]));
            for (int j = 0; j < 2; ++j) { g_gsSolid[j].Reset(); g_psSolid[j].Reset(); }
            return false;
        }
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_cbSolid))) { LogErr("wire widening: solid constant buffer failed"); return false; }
    for (float& f : g_cbSolidVal) f = -1.0f;
    g_solidFailed = false;
    return true;
}
}

void RegisterShaders() {
    ShaderCache::Add(ShaderCache::kWireGs, kWireGs, sizeof(kWireGs) - 1, "wire_widen_gs", "GSMain", "gs_5_0");
    ShaderCache::Add(ShaderCache::kWireGsCut, kWireGs, sizeof(kWireGs) - 1, "wire_widen_gs_cut", "GSCut", "gs_5_0");
    ShaderCache::Add(ShaderCache::kWireCutPs, kWireCutPs, sizeof(kWireCutPs) - 1, "wire_widen_cut_ps", "PSCut", "ps_5_0");
    ShaderCache::Add(ShaderCache::kSolidGsA, kSolidGs, sizeof(kSolidGs) - 1, "wire_widen_solid_gs_a", "GSolidA", "gs_5_0");   // phase 63
    ShaderCache::Add(ShaderCache::kSolidGsB, kSolidGs, sizeof(kSolidGs) - 1, "wire_widen_solid_gs_b", "GSolidB", "gs_5_0");
    ShaderCache::Add(ShaderCache::kSolidPsA, kSolidPs, sizeof(kSolidPs) - 1, "wire_widen_solid_ps_a", "PSSolidA", "ps_5_0");
    ShaderCache::Add(ShaderCache::kSolidPsB, kSolidPs, sizeof(kSolidPs) - 1, "wire_widen_solid_ps_b", "PSSolidB", "ps_5_0");
}

int BlendMode(ID3D11BlendState* bs) {
    if (!bs) return 0;                                   // the default state does not blend
    for (int i = 0; i < g_bsMemoN; ++i) if (g_bsMemo[i].bs == bs) return g_bsMemo[i].mode;
    D3D11_BLEND_DESC d{};
    bs->GetDesc(&d);
    const D3D11_RENDER_TARGET_BLEND_DESC& r = d.RenderTarget[0];
    const bool src = r.BlendEnable && r.SrcBlend == D3D11_BLEND_SRC_ALPHA && r.BlendOp == D3D11_BLEND_OP_ADD && !d.AlphaToCoverageEnable;
    const int mode = !src ? 0 : (r.DestBlend == D3D11_BLEND_INV_SRC_ALPHA ? 1 : (r.DestBlend == D3D11_BLEND_ONE ? 2 : 0));
    if (g_bsMemoN < kBsMemo) { bs->AddRef(); g_bsMemo[g_bsMemoN++] = { bs, mode }; }
    return mode;
}

bool Bind(ID3D11DeviceContext* ctx, ID3D11BlendState* bs, float vpW, float vpH, float minPx, int psKind) {
    if (!ctx || !(vpW > 1.0f) || !(vpH > 1.0f) || !psKind) return false;
    const int mode = BlendMode(bs);
    if (!mode) { ++g_st.skipBlend; return false; }
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!Ensure(dev.Get())) return false;
    if (psKind == 2 && !EnsureCut(dev.Get())) return false;
    ID3D11GeometryShader* prev = nullptr;
    ctx->GSGetShader(&prev, nullptr, nullptr);
    if (prev) { prev->Release(); ++g_st.skipGs; return false; }   // the game uses a GS here: not ours to replace
    const float v[4] = { 0.5f * vpW, 0.5f * vpH, minPx, mode == 1 ? 1.0f : 0.0f };
    if (memcmp(v, g_cbVal, sizeof(v)) != 0) {
        ctx->UpdateSubresource(g_cb.Get(), 0, nullptr, v, 0, 0);
        memcpy(g_cbVal, v, sizeof(v));
        ++g_st.cbUpdates;
    }
    ctx->GSGetConstantBuffers(0, 1, &g_prevCb);
    ID3D11Buffer* cb = g_cb.Get();
    ctx->GSSetConstantBuffers(0, 1, &cb);
    if (psKind == 2) {                                   // phase 62: the PS copy with the cut-off on the unscaled alpha
        ctx->PSGetShader(&g_prevPs, nullptr, nullptr);
        ctx->PSSetShader(g_psCutObj.Get(), nullptr, 0);
        ctx->GSSetShader(g_gsCut.Get(), nullptr, 0);
        g_psSwapped = true;
        ++g_st.boundCut;
    } else {
        ctx->GSSetShader(g_gs.Get(), nullptr, 0);
    }
    ++g_st.bound;
    return true;
}

// phase 63: a solid G-buffer draw of pair `kind` (1 A wires / 2 B towers): GSolid + the dither PS copy, frame = the dither frame
bool BindSolid(ID3D11DeviceContext* ctx, int kind, float vpW, float vpH, float minPx, uint32_t frame, float minShare) {
    if (!ctx || (kind != 1 && kind != 2) || !(vpW > 1.0f) || !(vpH > 1.0f)) return false;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!Ensure(dev.Get()) || !EnsureSolid(dev.Get())) return false;
    ID3D11GeometryShader* prev = nullptr;
    ctx->GSGetShader(&prev, nullptr, nullptr);
    if (prev) { prev->Release(); ++g_st.skipGs; return false; }
    const float v[8] = { 0.5f * vpW, 0.5f * vpH, minPx, 0.0f, (float)(frame & 1023u), minShare, 0.0f, 0.0f };
    if (memcmp(v, g_cbSolidVal, sizeof(v)) != 0) {
        ctx->UpdateSubresource(g_cbSolid.Get(), 0, nullptr, v, 0, 0);
        memcpy(g_cbSolidVal, v, sizeof(v));
        ++g_st.cbUpdates;
    }
    ctx->GSGetConstantBuffers(0, 1, &g_prevCb);
    ID3D11Buffer* cb = g_cbSolid.Get();
    ctx->GSSetConstantBuffers(0, 1, &cb);
    ctx->PSGetShader(&g_prevPs, nullptr, nullptr);
    ctx->PSSetShader(g_psSolid[kind - 1].Get(), nullptr, 0);
    ctx->GSSetShader(g_gsSolid[kind - 1].Get(), nullptr, 0);
    g_psSwapped = true;
    ++g_st.boundSolid;
    return true;
}

float g_replayMinPx = 1.0f;
void SetReplayMinPx(float minPx) { g_replayMinPx = minPx; }
bool BindReplay(ID3D11DeviceContext* ctx, int kind, float vpW, float vpH, float scale) {
    if (!ctx) return false;
    if (kind == 0) { ctx->GSSetShader(nullptr, nullptr, 0); ID3D11Buffer* n = nullptr; ctx->GSSetConstantBuffers(0, 1, &n); return true; }
    if (!(vpW > 1.0f) || !(vpH > 1.0f)) return false;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!Ensure(dev.Get())) return false;
    const float W = g_replayMinPx * (scale > 0.0f ? scale : 1.0f);
    if (kind == 1) {
        if (!EnsureCut(dev.Get())) return false;
        const float v[4] = { 0.5f * vpW, 0.5f * vpH, W, 1.0f };
        if (memcmp(v, g_cbVal, sizeof(v)) != 0) { ctx->UpdateSubresource(g_cb.Get(), 0, nullptr, v, 0, 0); memcpy(g_cbVal, v, sizeof(v)); ++g_st.cbUpdates; }
        ID3D11Buffer* cb = g_cb.Get();
        ctx->GSSetConstantBuffers(0, 1, &cb);
        ctx->GSSetShader(g_gsCut.Get(), nullptr, 0);
        return true;
    }
    if (kind != 2 && kind != 3) return false;
    if (!EnsureSolid(dev.Get())) return false;
    const float v[8] = { 0.5f * vpW, 0.5f * vpH, W, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };   // keep share 1 (not read by the replay PS)
    if (memcmp(v, g_cbSolidVal, sizeof(v)) != 0) { ctx->UpdateSubresource(g_cbSolid.Get(), 0, nullptr, v, 0, 0); memcpy(g_cbSolidVal, v, sizeof(v)); ++g_st.cbUpdates; }
    ID3D11Buffer* cb = g_cbSolid.Get();
    ctx->GSSetConstantBuffers(0, 1, &cb);
    ctx->GSSetShader(g_gsSolid[kind - 2].Get(), nullptr, 0);
    return true;
}

void Unbind(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->GSSetConstantBuffers(0, 1, &g_prevCb);
    if (g_prevCb) { g_prevCb->Release(); g_prevCb = nullptr; }
    if (g_psSwapped) {
        ctx->PSSetShader(g_prevPs, nullptr, 0);
        if (g_prevPs) { g_prevPs->Release(); g_prevPs = nullptr; }
        g_psSwapped = false;
    }
}

void Reset() {
    g_gs.Reset(); g_gsCut.Reset(); g_psCutObj.Reset(); g_cb.Reset();
    for (int i = 0; i < 2; ++i) { g_gsSolid[i].Reset(); g_psSolid[i].Reset(); }   // phase 63
    g_cbSolid.Reset(); g_solidFailed = false;
    for (float& f : g_cbSolidVal) f = -1.0f;
    g_dev = nullptr; g_failed = false; g_cutFailed = false;
    for (int i = 0; i < g_bsMemoN; ++i) if (g_bsMemo[i].bs) g_bsMemo[i].bs->Release();
    g_bsMemoN = 0;
    g_cbVal[0] = g_cbVal[1] = g_cbVal[2] = g_cbVal[3] = -1.0f;
}

Stats TakeStats() { const Stats s = g_st; g_st = Stats(); return s; }
#endif // WW_NO_D3D

} // namespace WireWiden

#endif // WITH_DLAA
