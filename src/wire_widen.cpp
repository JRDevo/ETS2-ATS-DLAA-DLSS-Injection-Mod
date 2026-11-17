// WireWiden implementation -- phase 61 / 62 / 63. See wire_widen.h.
#ifdef WITH_DLAA

#include "wire_widen.h"
#include <atomic>
#include <cstring>
#include <string>
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
// phase 107b (ETS2 flat captures eurotrucks2_2026.11.14_17.59.49, frames 2247 / 7916; captures\ps_57a4e1f1 / ps_2d7ed0e6 /
// ps_d3af2ebb / vs_3847be86 / vs_9d2904cd / vs_d87f5280 .dxbc + .txt):
// pair C = the CRANES' lattice (frame 2247 EIDs 11884 / 13874 / 13884: 1900 .. 2200 of 2400 .. 4700 triangles thinner than 1 px),
//   VS 07dfaa00 (= pair A's VS) + PS 57a4e1f1. Its input signature is byte for byte pair A's (COLOR0, SV_Position, TEXCOORD0 / 1
//   float3, TEXCOORD2 float2) -> GSolidA + PSSolidC.
// pair D = bridge / guard RAILINGS (frame 7916: 21 draws, 12000 of 16900 triangles thin), PS 2d7ed0e6 (discard below t6.a 0.05)
//   with VS 3847be86 (static), 9d2904cd (BLENDINDICES / BLENDWEIGHT) and d87f5280 (instanced, the ETS2 street lights' VS).
// pair E = the ETS2 STREET LIGHTS (frame 7916 EIDs 8574 / 8578: DrawIndexedInstanced, 5 / 14 instances, 1822 triangles, all thin):
//   VS d87f5280 + PS d3af2ebb (= 2d7ed0e6 without the discard) -- ETS2's counterpart of the ATS phase-66 lights (a484ef85 /
//   34860eed). The three VSs have ONE output signature (COLOR0, SV_Position, TEXCOORD0..3 float3, TEXCOORD4 float4, TEXCOORD5
//   float2 in registers 0..7: the OSGN chunks are byte-identical) = the PSs' input -> one new GS, GSolidD.
// phase 109b: the ETS2 POWER LINES (village RenderDoc audit 2026-11-14, PHASE109_FINDINGS.md C): G-buffer VS 7e42d0ff + PS 848f389f,
// forward VS 277d2302 + PS 69dcaf36 (SrcAlpha / InvSrcAlpha, depth GREATER, no write). The game widens them itself (max(clip w x
// cb0[13].x, radius)) and draws ~1.1 px with alpha = radius / width; the forward-depth batch must draw them without A2C
// (DrawIdRecord::kGsNoA2c) -- no GS of ours, the colour is the game's.
const uint32_t kGameWireVs[4] = { 0x277d2302u, 0x9b42478eu, 0x3ec25e91u, 0xc80564ceu };
const uint32_t kGameWirePs[4] = { 0x69dcaf36u, 0x9e007d9fu, 0x77c035efu, 0x4f93674fu };
const uint32_t kSolidPsC[4]  = { 0x57a4e1f1u, 0x37d8748du, 0x2601f393u, 0x9a23ff42u };
const uint32_t kSolidVsD[4]  = { 0x3847be86u, 0x06f9191cu, 0xf66db962u, 0xa527dda7u };
const uint32_t kSolidVsD2[4] = { 0x9d2904cdu, 0xe36138bcu, 0x5e03d877u, 0x7cb0844au };
const uint32_t kSolidVsD3[4] = { 0xd87f5280u, 0x55fc7383u, 0x6d9de733u, 0xced4ce77u };
const uint32_t kSolidPsD[4]  = { 0x2d7ed0e6u, 0xda6a76ddu, 0xa8854a54u, 0x1f000170u };
const uint32_t kSolidPsE[4]  = { 0xd3af2ebbu, 0xf83ef6b8u, 0xf93028fau, 0xeb7e336du };
// phase 109e (ETS2 RenderDoc audits 2026-11-14, PHASE109_FINDINGS.md E; bytecode in captures\phase109_2026-11-14):
// pair A's VS set gains the lamp-post VS ccd6b6d1 (Norway + town lamp posts; its OSGN is byte-identical to 07dfaa00's);
// pair F = fences + ETS2 town street lights: VS e5a4956d (OSGN = 3847be86's but TEXCOORD5 a float4) + PS 2cc0a6fa -> GSolidF;
// kind G = PS cfc52d43 with pair A's VSs (far bridge railings, chain-link posts, rigging: discard t6.a x COLOR.w < 0.05) -> GSolidA.
// phase 109d: kind H = PS e19e0136 with pair A's VSs (the ETS2 road poles' stripe band, also road-sign faces) -> GSolidA; used ONLY
// by the standalone pole_aa (SolidOnDraw skips it: wire_widen never dithers road signs).
const uint32_t kSolidVsA2[4] = { 0xccd6b6d1u, 0x72cf4230u, 0x608be3e5u, 0x2130d656u };
const uint32_t kSolidVsF[4]  = { 0xe5a4956du, 0xda029000u, 0x29dd09d2u, 0xdd9f10e1u };
const uint32_t kSolidPsF[4]  = { 0x2cc0a6fau, 0xaa3fde11u, 0x3988a341u, 0x0f2eb0a7u };
const uint32_t kSolidPsG[4]  = { 0xcfc52d43u, 0x76841a8bu, 0xcabed458u, 0x61ebcce6u };
const uint32_t kSolidPsH[4]  = { 0xe19e0136u, 0xf0ccff8du, 0x5fee66cbu, 0x14a4f716u };
// phase 111 (the user's census capture captures\census_2026-11-15\shimmer_20261115_104831\census\): pair A's VS set + the
// instanced road-pole / delineator VS 2a9f326e (with e19e0136: the reflector band of 57 instanced posts) and the skinned VS 7fcd07d6
// (with b67da69f); both OSGNs are byte-identical to 07dfaa00's (harness S107). Kind I = PS 2585159b with pair A's VS: pair A's maths
// without the gloss (o1 = cb0[1]; sign / road-furniture atlas, 0.08 px per triangle) -> GSolidA, wire_widen only.
const uint32_t kSolidVsA3[4] = { 0x2a9f326eu, 0x32c24982u, 0x993094b3u, 0x12c05cabu };
const uint32_t kSolidVsA4[4] = { 0x7fcd07d6u, 0x58a1af53u, 0x50fdb049u, 0xa83fdd16u };
const uint32_t kSolidPsI[4]  = { 0x2585159bu, 0xc9f6d768u, 0xfd7f85bbu, 0xe432fcd6u };
// phase 113 (the user's census capture captures\census_2026-11-15\shimmer_20261115_145426\census\, Norway, Volvo Trucks Center by the
// coast road): VS bdce664b draws PS 2585159b (kind I) over the dealership's signage / facade (~1 k px per draw, 54..4284 indices,
// 0.60 px per triangle: not thin). Its OSGN is byte-identical to 07dfaa00's (harness S107) -> pair A's VS set.
const uint32_t kSolidVsA5[4] = { 0xbdce664bu, 0x0da42023u, 0x29213063u, 0xe943586bu };
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
std::atomic<uintptr_t> g_vsSolidD[kSet] = {};                                   // phase 107b
std::atomic<uintptr_t> g_psSolidC[kSet] = {}, g_psSolidD[kSet] = {}, g_psSolidE[kSet] = {};
std::atomic<uintptr_t> g_vsGameWire[kSet] = {}, g_psGameWire[kSet] = {};          // phase 109b
std::atomic<uintptr_t> g_vsSolidF[kSet] = {}, g_psSolidF[kSet] = {}, g_psSolidG[kSet] = {}, g_psSolidH[kSet] = {};   // phase 109d / e
std::atomic<uintptr_t> g_psSolidI[kSet] = {};                                   // phase 111
std::atomic<uint32_t>  g_psGen{0};                   // phase 109d: bumped by every NotePs (SolidKindMemo's invalidation)
// phase 115r (audit B4 / A2): the OSGN-JOINED VSs have sets of their own (they used to fill the 64-slot hand-listed sets: a join flood
// silently dropped a later hand-listed member). 256 slots each; a join that finds no room is counted (JoinFull, logged by inject.cpp).
constexpr int kJoinSet = 256;
std::atomic<uintptr_t> g_vsJoinA[kJoinSet] = {}, g_vsJoinB[kJoinSet] = {}, g_vsJoinD[kJoinSet] = {}, g_vsJoinF[kJoinSet] = {};
std::atomic<int> g_joinFull{0};
std::atomic<uint32_t> g_setFullMask{0};              // phase 115r: bit i = SetNames()[i] overflowed
const std::atomic<uintptr_t>* const kSetIds[] = { g_vs, g_ps, g_psCut, g_vsSolidA, g_vsSolidB, g_psSolidA, g_psSolidB, g_vsSolidD,
                                                  g_psSolidC, g_psSolidD, g_psSolidE, g_vsGameWire, g_psGameWire, g_vsSolidF,
                                                  g_psSolidF, g_psSolidG, g_psSolidH, g_psSolidI };
const char* const kSetNames[] = { "wire VS", "wire PS", "wire PS (cut)", "solid VS A", "solid VS B", "solid PS A", "solid PS B",
                                  "solid VS D", "solid PS C", "solid PS D", "solid PS E", "game wire VS", "game wire PS", "solid VS F",
                                  "solid PS F", "solid PS G", "solid PS H", "solid PS I" };
uint32_t SetBit(const std::atomic<uintptr_t>* set) {
    for (size_t i = 0; i < sizeof(kSetIds) / sizeof(kSetIds[0]); ++i) if (kSetIds[i] == set) return 1u << i;
    return 0;
}
// the join sets: the same rules as SetNote / SetHas (insert when `in`, else forget the address)
void JoinNote(std::atomic<uintptr_t>* set, uintptr_t key, bool in) {
    if (!key) return;
    for (int i = 0; i < kJoinSet; ++i)
        if (set[i].load(std::memory_order_acquire) == key) {
            if (!in) set[i].store(0, std::memory_order_release);
            return;
        }
    if (!in) return;
    for (int i = 0; i < kJoinSet; ++i) {
        uintptr_t e = 0;
        if (set[i].compare_exchange_strong(e, key, std::memory_order_acq_rel)) return;
    }
    g_joinFull.fetch_add(1, std::memory_order_relaxed);
}
bool JoinHas(const std::atomic<uintptr_t>* set, uintptr_t key) {
    if (!key) return false;
    for (int i = 0; i < kJoinSet; ++i) if (set[i].load(std::memory_order_acquire) == key) return true;
    return false;
}
int JoinCount(const std::atomic<uintptr_t>* set) {
    int n = 0;
    for (int i = 0; i < kJoinSet; ++i) if (set[i].load(std::memory_order_relaxed)) ++n;
    return n;
}
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
    g_setFullMask.fetch_or(SetBit(set), std::memory_order_relaxed);   // phase 115r: which set (the log names it)
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

// ---- phase 114: a VS joins a solid VS set by its OUTPUT SIGNATURE ----------------------------------------------------------------
// Every VS added to a set by hand (109e ccd6b6d1, 111 2a9f326e / 7fcd07d6, 113 bdce664b, 66 a484ef85, 107b 9d2904cd / d87f5280) was
// added on ONE piece of evidence: its OSGN chunk is byte-identical to the set's first VS (the GS + PS read the same registers with the
// same masks). The user's 2026-11-15 evening ATS census showed a new one (6c53c441 with pair B's PS 34860eed: thin, "unknown VS",
// never dumped -- no bytecode to check offline). So the check runs in the game now: a VS whose OSGN payload is byte-identical to a
// set's reference joins that set at CreateVertexShader. On the 50-VS corpus in captures\ it reproduces every manual set exactly and adds
// two more (5e03b1ca -> A, 9d3fd72b -> D; harness S114). Moving draws stay undithered either way (phase 80's static gate).
// The OSGN payloads (after the 8-byte chunk header) of 07dfaa00 (A), 6c157c5f (B), 3847be86 (D), e5a4956d (F); captures\vs_*.dxbc.
const uint8_t kOsgnA[156] = {
    0x05, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x92, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x07, 0x08, 0x00, 0x00, 0x92, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x07, 0x08, 0x00, 0x00, 0x92, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x03, 0x0c, 0x00, 0x00, 0x43, 0x4f, 0x4c, 0x4f, 0x52, 0x00, 0x53, 0x56, 0x5f, 0x50, 0x6f, 0x73, 0x69, 0x74, 0x69, 0x6f,
    0x6e, 0x00, 0x54, 0x45, 0x58, 0x43, 0x4f, 0x4f, 0x52, 0x44, 0x00, 0xab,
};
const uint8_t kOsgnB[180] = {
    0x06, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x98, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x9e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0xaa, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x07, 0x08, 0x00, 0x00, 0xaa, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x07, 0x08, 0x00, 0x00, 0xaa, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0xaa, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x03, 0x0c, 0x00, 0x00, 0x43, 0x4f, 0x4c, 0x4f, 0x52, 0x00, 0x53, 0x56, 0x5f, 0x50, 0x6f, 0x73, 0x69, 0x74, 0x69, 0x6f,
    0x6e, 0x00, 0x54, 0x45, 0x58, 0x43, 0x4f, 0x4f, 0x52, 0x44, 0x00, 0xab,
};
const uint8_t kOsgnD[228] = {
    0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0xce, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x07, 0x08, 0x00, 0x00, 0xda, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x07, 0x08, 0x00, 0x00, 0xda, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x07, 0x08, 0x00, 0x00, 0xda, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x07, 0x08, 0x00, 0x00, 0xda, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0xda, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x07, 0x00, 0x00, 0x00, 0x03, 0x0c, 0x00, 0x00, 0x43, 0x4f, 0x4c, 0x4f, 0x52, 0x00, 0x53, 0x56, 0x5f, 0x50, 0x6f, 0x73, 0x69, 0x74, 0x69, 0x6f,
    0x6e, 0x00, 0x54, 0x45, 0x58, 0x43, 0x4f, 0x4f, 0x52, 0x44, 0x00, 0xab,
};
const uint8_t kOsgnF[228] = {
    0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0xc8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0xce, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0xda, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x07, 0x08, 0x00, 0x00, 0xda, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00, 0x07, 0x08, 0x00, 0x00, 0xda, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x07, 0x08, 0x00, 0x00, 0xda, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x05, 0x00, 0x00, 0x00, 0x07, 0x08, 0x00, 0x00, 0xda, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0xda, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x07, 0x00, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x00, 0x43, 0x4f, 0x4c, 0x4f, 0x52, 0x00, 0x53, 0x56, 0x5f, 0x50, 0x6f, 0x73, 0x69, 0x74, 0x69, 0x6f,
    0x6e, 0x00, 0x54, 0x45, 0x58, 0x43, 0x4f, 0x4f, 0x52, 0x44, 0x00, 0xab,
};
const uint8_t* DxbcChunk(const void* code, size_t len, const char tag[4], uint32_t* size) {
    *size = 0;
    if (!code || len < 32) return nullptr;
    const uint8_t* b = static_cast<const uint8_t*>(code);
    if (memcmp(b, "DXBC", 4)) return nullptr;
    auto u32 = [b](size_t o) { uint32_t v; memcpy(&v, b + o, 4); return v; };
    const uint32_t n = u32(28);
    if (n > 64 || 32 + 4ull * n > len) return nullptr;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t off = u32(32 + 4 * i);
        if ((uint64_t)off + 8 > len) return nullptr;
        if (memcmp(b + off, tag, 4)) continue;
        const uint32_t sz = u32(off + 4);
        if ((uint64_t)off + 8 + sz > len) return nullptr;
        *size = sz;
        return b + off + 8;
    }
    return nullptr;
}
int OsgnSet(const void* code, size_t len) {
    uint32_t sz = 0;
    const uint8_t* o = DxbcChunk(code, len, "OSGN", &sz);
    if (!o) return 0;
    if (sz == sizeof(kOsgnA) && !memcmp(o, kOsgnA, sz)) return 'A';
    if (sz == sizeof(kOsgnB) && !memcmp(o, kOsgnB, sz)) return 'B';
    if (sz == sizeof(kOsgnD) && !memcmp(o, kOsgnD, sz)) return 'D';
    if (sz == sizeof(kOsgnF) && !memcmp(o, kOsgnF, sz)) return 'F';
    return 0;
}
namespace { std::atomic<int> g_osgnJoins{ 0 }; }
int OsgnJoins() { return g_osgnJoins.load(std::memory_order_relaxed); }

int NoteVs(const void* vs, const void* code, size_t len) {
    SetNote(g_vs, (uintptr_t)vs, ChecksumIs(code, len, kWireVs));
    SetNote(g_vsSolidA, (uintptr_t)vs, ChecksumIs(code, len, kSolidVsA) || ChecksumIs(code, len, kSolidVsA2) ||
                                       ChecksumIs(code, len, kSolidVsA3) || ChecksumIs(code, len, kSolidVsA4) ||
                                       ChecksumIs(code, len, kSolidVsA5));   // phase 63 (109e: + ccd6b6d1; 111: + 2a9f326e, 7fcd07d6; 113: + bdce664b)
    SetNote(g_vsSolidF, (uintptr_t)vs, ChecksumIs(code, len, kSolidVsF));   // phase 109e
    SetNote(g_vsSolidB, (uintptr_t)vs, ChecksumIs(code, len, kSolidVsB) || ChecksumIs(code, len, kSolidVsB2));   // phase 66: + lights
    SetNote(g_vsSolidD, (uintptr_t)vs, ChecksumIs(code, len, kSolidVsD) || ChecksumIs(code, len, kSolidVsD2) ||
                                       ChecksumIs(code, len, kSolidVsD3));                                         // phase 107b
    SetNote(g_vsGameWire, (uintptr_t)vs, ChecksumIs(code, len, kGameWireVs));                                     // phase 109b
    // phase 114: a VS not in a set by checksum joins the set whose reference OSGN it carries byte for byte (-> the set's letter).
    // phase 115r: into the JOIN set of that letter (never the hand-listed one); every join set forgets this address otherwise (a
    // shader re-created at a joined VS's address with other bytecode)
    const uintptr_t k = (uintptr_t)vs;
    if (!k) return 0;
    const bool hand = SetHas(g_vsSolidA, k) || SetHas(g_vsSolidB, k) || SetHas(g_vsSolidD, k) || SetHas(g_vsSolidF, k);
    const int set = hand ? 0 : OsgnSet(code, len);
    JoinNote(g_vsJoinA, k, set == 'A');
    JoinNote(g_vsJoinB, k, set == 'B');
    JoinNote(g_vsJoinD, k, set == 'D');
    JoinNote(g_vsJoinF, k, set == 'F');
    const std::atomic<uintptr_t>* js = set == 'A' ? g_vsJoinA : set == 'B' ? g_vsJoinB : set == 'D' ? g_vsJoinD : set == 'F' ? g_vsJoinF : nullptr;
    if (!js || !JoinHas(js, k)) return 0;            // (not a join, or its join set is full: JoinFull counts it)
    g_osgnJoins.fetch_add(1, std::memory_order_relaxed);
    return set;
}
int JoinFull() { return g_joinFull.load(std::memory_order_relaxed); }
int JoinedVs() { return JoinCount(g_vsJoinA) + JoinCount(g_vsJoinB) + JoinCount(g_vsJoinD) + JoinCount(g_vsJoinF); }
std::string SetFullNames() {
    const uint32_t m = g_setFullMask.load(std::memory_order_relaxed);
    std::string r;
    for (size_t i = 0; i < sizeof(kSetNames) / sizeof(kSetNames[0]); ++i)
        if (m & (1u << i)) { if (!r.empty()) r += ", "; r += kSetNames[i]; }
    return r;
}
void NotePs(const void* ps, const void* code, size_t len) {
    const int k = PsKind(code, len);
    SetNote(g_ps, (uintptr_t)ps, k == 1);
    SetNote(g_psCut, (uintptr_t)ps, k == 2);
    SetNote(g_psSolidA, (uintptr_t)ps, ChecksumIs(code, len, kSolidPsA));   // phase 63
    SetNote(g_psSolidB, (uintptr_t)ps, ChecksumIs(code, len, kSolidPsB));
    SetNote(g_psSolidC, (uintptr_t)ps, ChecksumIs(code, len, kSolidPsC));   // phase 107b
    SetNote(g_psSolidD, (uintptr_t)ps, ChecksumIs(code, len, kSolidPsD));
    SetNote(g_psSolidE, (uintptr_t)ps, ChecksumIs(code, len, kSolidPsE));
    SetNote(g_psGameWire, (uintptr_t)ps, ChecksumIs(code, len, kGameWirePs));   // phase 109b
    SetNote(g_psSolidF, (uintptr_t)ps, ChecksumIs(code, len, kSolidPsF));   // phase 109e
    SetNote(g_psSolidG, (uintptr_t)ps, ChecksumIs(code, len, kSolidPsG));
    SetNote(g_psSolidH, (uintptr_t)ps, ChecksumIs(code, len, kSolidPsH));   // phase 109d
    SetNote(g_psSolidI, (uintptr_t)ps, ChecksumIs(code, len, kSolidPsI));   // phase 111
    g_psGen.fetch_add(1, std::memory_order_release);
}
uint32_t PsGeneration() { return g_psGen.load(std::memory_order_acquire); }
int SolidPsKindOfCode(const void* code, size_t len) {
    const uint32_t* const k[kSolidKinds] = { kSolidPsA, kSolidPsB, kSolidPsC, kSolidPsD, kSolidPsE, kSolidPsF, kSolidPsG, kSolidPsH,
                                             kSolidPsI };   // phase 111
    for (int i = 0; i < kSolidKinds; ++i) if (ChecksumIs(code, len, k[i])) return i + 1;
    return 0;
}
bool IsGameWirePs(const void* ps) { return SetHas(g_psGameWire, (uintptr_t)ps); }
bool IsGameWireVs(const void* vs) { return SetHas(g_vsGameWire, (uintptr_t)vs); }
bool IsWireVs(const void* vs) { return SetHas(g_vs, (uintptr_t)vs); }
int  WirePsKind(const void* ps) { return SetHas(g_ps, (uintptr_t)ps) ? 1 : (SetHas(g_psCut, (uintptr_t)ps) ? 2 : 0); }
int  KnownVs() { return SetCount(g_vs); }
int  KnownPs() { return SetCount(g_ps) + SetCount(g_psCut); }
int  KnownPsCut() { return SetCount(g_psCut); }
int  SolidPsKind(const void* ps) {
    const uintptr_t p = (uintptr_t)ps;
    if (!p) return 0;
    if (SetHas(g_psSolidA, p)) return kSolidA;
    if (SetHas(g_psSolidB, p)) return kSolidB;
    if (SetHas(g_psSolidC, p)) return kSolidC;           // phase 107b
    if (SetHas(g_psSolidD, p)) return kSolidD;
    if (SetHas(g_psSolidE, p)) return kSolidE;
    if (SetHas(g_psSolidF, p)) return kSolidF;           // phase 109e
    if (SetHas(g_psSolidG, p)) return kSolidG;
    if (SetHas(g_psSolidH, p)) return kSolidH;           // phase 109d
    if (SetHas(g_psSolidI, p)) return kSolidI;           // phase 111
    return 0;
}
bool IsSolidVs(const void* vs, int kind, bool* joined) {
    if (joined) *joined = false;
    const std::atomic<uintptr_t>* hand = nullptr;
    const std::atomic<uintptr_t>* join = nullptr;
    switch (kind) {
    case kSolidA: case kSolidC:                          // phase 107b: the cranes share pair A's VS 07dfaa00
    case kSolidG: case kSolidH:
    case kSolidI:               hand = g_vsSolidA; join = g_vsJoinA; break;   // phase 109d / e (111: + I): pair A's VSs too
    case kSolidB:               hand = g_vsSolidB; join = g_vsJoinB; break;
    case kSolidD: case kSolidE: hand = g_vsSolidD; join = g_vsJoinD; break;
    case kSolidF:               hand = g_vsSolidF; join = g_vsJoinF; break;   // phase 109e
    default:                    return false;
    }
    if (SetHas(hand, (uintptr_t)vs)) return true;
    if (!JoinHas(join, (uintptr_t)vs)) return false;     // phase 115r: an OSGN-joined VS (the caller trusts it less: B4)
    if (joined) *joined = true;
    return true;
}
int  SolidGsOf(int kind) {
    switch (kind) {
    case kSolidA: case kSolidC:
    case kSolidG: case kSolidH:
    case kSolidI:               return 0;                // GSolidA (phase 109d / e: + G, H; phase 111: + I)
    case kSolidB:               return 1;                // GSolidB
    case kSolidD: case kSolidE: return 2;                // GSolidD (phase 107b)
    case kSolidF:               return 3;                // GSolidF (phase 109e)
    default:                    return -1;
    }
}
int  SolidReplayKind(int kind) { const int g = SolidGsOf(kind); return g < 0 ? 0 : 2 + g; }
int  KnownSolid() {
    return SetCount(g_vsSolidA) + SetCount(g_vsSolidB) + SetCount(g_vsSolidD) + SetCount(g_psSolidA) + SetCount(g_psSolidB) +
           SetCount(g_psSolidC) + SetCount(g_psSolidD) + SetCount(g_psSolidE) + SetCount(g_vsSolidF) + SetCount(g_psSolidF) +
           SetCount(g_psSolidG) + SetCount(g_psSolidH) + SetCount(g_psSolidI);   // phase 109d / e, 111
}
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
    noperspective float2 dl : TEXCOORD8;   // phase 109d: pole mode, the pixel's distances to the face's two long edges
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
    noperspective float2 dl : TEXCOORD8;   // phase 109d: pole mode, the pixel's distances to the face's two long edges
};
struct VD {             // phase 107b: VS 3847be86 / 9d2904cd / d87f5280 output = PS 2d7ed0e6 / d3af2ebb input (registers 0..7)
    float4 col : COLOR0;
    float4 pos : SV_Position;
    float3 t0  : TEXCOORD0;
    float3 t1  : TEXCOORD1;
    float3 t2  : TEXCOORD2;
    float3 t3  : TEXCOORD3;
    float4 t4  : TEXCOORD4;
    float2 t5  : TEXCOORD5;
};
struct VDK {
    float4 col : COLOR0;
    float4 pos : SV_Position;
    float3 t0  : TEXCOORD0;
    float3 t1  : TEXCOORD1;
    float3 t2  : TEXCOORD2;
    float3 t3  : TEXCOORD3;
    float4 t4  : TEXCOORD4;
    float2 t5  : TEXCOORD5;
    float4 kf  : TEXCOORD7;
    noperspective float2 dl : TEXCOORD8;   // phase 109d: pole mode, the pixel's distances to the face's two long edges
};
cbuffer SolidCb : register(b0) {
    float4 g_p;          // x, y = half the viewport in pixels, z = the band width W in pixels (wire_min_px), w = pole mode: jitter x (113)
    float4 g_q;          // x = the dither frame (0 .. 1023), y = the minimum keep share (wire_solid_min_share), z = 1: pole mode (109d),
                         // w = pole mode: the pass's jitter y (phase 113; with g_p.w in the band's clip-px frame, y up)
};
struct VF {             // phase 109e: pair F's VS e5a4956d output = PS 2cc0a6fa input (= VD with TEXCOORD5 a float4)
    float4 col : COLOR0;
    float4 pos : SV_Position;
    float3 t0  : TEXCOORD0;
    float3 t1  : TEXCOORD1;
    float3 t2  : TEXCOORD2;
    float3 t3  : TEXCOORD3;
    float4 t4  : TEXCOORD4;
    float4 t5  : TEXCOORD5;
};
struct VFK {
    float4 col : COLOR0;
    float4 pos : SV_Position;
    float3 t0  : TEXCOORD0;
    float3 t1  : TEXCOORD1;
    float3 t2  : TEXCOORD2;
    float3 t3  : TEXCOORD3;
    float4 t4  : TEXCOORD4;
    float4 t5  : TEXCOORD5;
    float4 kf  : TEXCOORD7;
    noperspective float2 dl : TEXCOORD8;   // phase 109d: pole mode, the pixel's distances to the face's two long edges
};
struct Band {
    bool   widen, pos;
    int    sh, s1, sf;
    float2 m, f, n;
    float  k;
    float  lamE, fl, fr;  // phase 109d: lambda on the edge rails, the keep floor, the dither frame of this face
    float2 dn1, dn2;      // phase 109d pole mode: the unit normals of the triangle's two long edges (one orientation) ...
    float  dc1, dc2;      // ... and their offsets (line: dot(p, dn) = dc); 0 in wire mode
};
// phase 109d POLE MODE (g_q.z = 1, the standalone pole_aa): a pole is a BOX of a few faces next to each other on screen, each face a
// quad of two thin triangles. With the wire dither every triangle gets its own tent of keep share k on the SAME per-pixel noise, so
// overlapping bands keep the max, not the sum: the union keeps 40 .. 60 % of the pole's width (forest audit). Pole mode = a STRATIFIED
// keep window per pixel: each triangle's band is a box W wide (no floor), and a band pixel keeps the triangle when the pixel's noise
// falls in the triangle's ACROSS INTERVAL measured from that pixel: [lo, hi] = where the triangle's two long edges cross the across
// axis, relative to the pixel centre, / W (GS: the edges' lines; dl = the pixel's signed distances to them, interpolated without
// perspective -> exact at the pixel centre). The two triangles of one face share their diagonal, so their intervals are complementary
// ([left side, diagonal] + [diagonal, right side] at every height), and neighbouring faces share their corner line: at every pixel
// the kept windows are disjoint and add up to the pole's width / W. Measured from the pixel itself, no screen-wide axis is needed
// (an absolute across coordinate would mix in the screen position times the faces' small tilt). kf = (W, frame, 1, -1) marks it.
// (phase 109d: a C++ raw-string split here, MSVC's 16380-byte literal cap): )" R"(
Band MakeBand(float4 p0, float4 p1, float4 p2) {
    Band b = (Band)0;
    const bool pole = g_q.z > 0.5;
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
    const float2 nh = float2(-e.y, e.x);
    const float r = saturate(h / W);
    if (pole) {
        // phase 109d: a box W wide (n = half of it), k = h / W, no floor, the face's own window in the noise
        b.n = nh * (0.5 * W);
        b.k = r;
        b.lamE = 1.0;
        b.fl = 0.0;
        b.fr = g_q.x;
        // the two long edges (every edge but the shortest, sh -> s1): s1 -> sf and sf -> sh, normals oriented like the across axis
        // (nh with one sign rule for every triangle: dot with a fixed reference > 0)
        const float2 ax = dot(nh, float2(1.0, 0.618)) >= 0.0 ? nh : -nh;
        const float2 d1 = s[b.sf] - s[b.s1], d2 = s[b.sh] - s[b.sf];
        float2 n1 = float2(-d1.y, d1.x) / max(length(d1), 1e-6), n2 = float2(-d2.y, d2.x) / max(length(d2), 1e-6);
        n1 = dot(n1, ax) >= 0.0 ? n1 : -n1;
        n2 = dot(n2, ax) >= 0.0 ? n2 : -n2;
        // phase 113: the lines are moved by the pass's jitter (g_p.w, g_q.w): dl at a pixel is then measured from the UN-jittered
        // pixel centre, so the keep window no longer moves with the jitter -- with the golden-ratio frame term it beat against the
        // 8-phase jitter sequence (harness S107 f: a column of a 0.37 px post empty for 64 frames in a row)
        const float2 jit = float2(g_p.w, g_q.w);
        b.dn1 = n1; b.dc1 = dot(s[b.s1], n1) - dot(jit, n1);
        b.dn2 = n2; b.dc2 = dot(s[b.sf], n2) - dot(jit, n2);
        // phase 113: the box is centred on the triangle's LONGEST edge -- the diagonal the two triangles of a face SHARE -- so both
        // halves of a face evaluate the same pixels (each box was centred on its own short-edge-to-far-vertex line before: the two
        // halves' boxes differed by up to half the face width, a pixel row in one box and not the other lost that half's window and
        // a post's column showed nothing for a frame). The box need not contain the triangle: the keep window is periodic in W, a
        // W-wide box in any place holds exactly one period. Its band vertices are the edge's own two vertices (exact attributes).
        const int le = (l0 >= l1 && l0 >= l2) ? 0 : (l1 >= l2 ? 1 : 2);   // edge i = vertex i -> vertex i + 1
        const int la = le, lb = le == 2 ? 0 : le + 1;
        const float2 dd = s[lb] - s[la];
        const float dlen = length(dd);
        if (dlen >= 1e-3) {
            const float2 e2 = dd / dlen;
            b.sh = la; b.s1 = la; b.sf = lb;             // (the GS: vm = MidX(v[sh], v[s1]) = v[la], vf = v[lb])
            b.m = s[la]; b.f = s[lb];
            b.n = float2(-e2.y, e2.x) * (0.5 * W);
        }
    } else {
        b.n = nh * W;                            // phase 67: the tent's half base (see kWireGs): keep share k in the middle, 0 at the edges
        // the pair factor, then the keep-share floor: DLSS validates its history against the current frame's neighbourhood, so a
        // pixel that shows the wire in 1 frame of 25 (k ~ 0.04 for a far wire) flashes instead of building a faint line -- a share of
        // at least g_q.y keeps it temporally dense (darker than the real 0.1 px wire, like the game's own dashes, but continuous).
        // phase 67 tent pair (same noise -> the union = the max of two tents h / 2 apart): area W k (1 + r / 2 - r^2 / 16) = h. The
        // floor is applied PER PIXEL in the PS, to the tent's inner half (lambda >= 0.5: the old box's W): every pixel there keeps at
        // least g_q.y as before; the soft outer half fades to 0 (a floor there would make the wire 2 W wide and dark).
        b.k = r / (1.0 + 0.5 * r - 0.0625 * r * r);
        b.lamE = 0.0;
        b.fl = g_q.y;
        b.fr = g_q.x;
    }
    b.pos = a2 >= 0.0;
    return b;
}
float4 AtPos(float4 p, float2 s) { return float4(s / g_p.xy * p.w, p.zw); }
// phase 109d: the signed distances of a band position (screen px from the centre, y up) to the triangle's two long edges
float2 DL(Band b, float2 p) { return float2(dot(p, b.dn1) - b.dc1, dot(p, b.dn2) - b.dc2); }
// kf (phase 67) = (keep share k x lambda, the dither frame, lambda = 1 on the middle rail .. 0 on the edge rails, the floor: g_q.y on
// every vertex of a widened band, 0 for a pass-through triangle (k = 1, lambda = 1: always kept, the game's PS exactly))
VAK ToA(VA v, float k, float lam, float fl, float fr) { VAK o; o.col = v.col; o.pos = v.pos; o.t0 = v.t0; o.t1 = v.t1; o.t2 = v.t2; o.kf = float4(k * lam, fr, lam, fl); o.dl = float2(0, 0); return o; }
VBK ToB(VB v, float k, float lam, float fl, float fr) { VBK o; o.col = v.col; o.pos = v.pos; o.t0 = v.t0; o.t1 = v.t1; o.t2 = v.t2; o.t3 = v.t3; o.kf = float4(k * lam, fr, lam, fl); o.dl = float2(0, 0); return o; }
VA MidA(VA a, VA b) {
    VA o; o.col = 0.5 * (a.col + b.col); o.pos = 0.5 * (a.pos + b.pos); o.t0 = 0.5 * (a.t0 + b.t0); o.t1 = 0.5 * (a.t1 + b.t1);
    o.t2 = 0.5 * (a.t2 + b.t2); return o;
}
VB MidB(VB a, VB b) {
    VB o; o.col = 0.5 * (a.col + b.col); o.pos = 0.5 * (a.pos + b.pos); o.t0 = 0.5 * (a.t0 + b.t0); o.t1 = 0.5 * (a.t1 + b.t1);
    o.t2 = 0.5 * (a.t2 + b.t2); o.t3 = 0.5 * (a.t3 + b.t3); return o;
}
VDK ToD(VD v, float k, float lam, float fl, float fr) {
    VDK o; o.col = v.col; o.pos = v.pos; o.t0 = v.t0; o.t1 = v.t1; o.t2 = v.t2; o.t3 = v.t3; o.t4 = v.t4; o.t5 = v.t5;
    o.kf = float4(k * lam, fr, lam, fl); o.dl = float2(0, 0); return o;
}
VD MidD(VD a, VD b) {
    VD o; o.col = 0.5 * (a.col + b.col); o.pos = 0.5 * (a.pos + b.pos); o.t0 = 0.5 * (a.t0 + b.t0); o.t1 = 0.5 * (a.t1 + b.t1);
    o.t2 = 0.5 * (a.t2 + b.t2); o.t3 = 0.5 * (a.t3 + b.t3); o.t4 = 0.5 * (a.t4 + b.t4); o.t5 = 0.5 * (a.t5 + b.t5); return o;
}
VFK ToF(VF v, float k, float lam, float fl, float fr) {
    VFK o; o.col = v.col; o.pos = v.pos; o.t0 = v.t0; o.t1 = v.t1; o.t2 = v.t2; o.t3 = v.t3; o.t4 = v.t4; o.t5 = v.t5;
    o.kf = float4(k * lam, fr, lam, fl); o.dl = float2(0, 0); return o;
}
VF MidF(VF a, VF b) {
    VF o; o.col = 0.5 * (a.col + b.col); o.pos = 0.5 * (a.pos + b.pos); o.t0 = 0.5 * (a.t0 + b.t0); o.t1 = 0.5 * (a.t1 + b.t1);
    o.t2 = 0.5 * (a.t2 + b.t2); o.t3 = 0.5 * (a.t3 + b.t3); o.t4 = 0.5 * (a.t4 + b.t4); o.t5 = 0.5 * (a.t5 + b.t5); return o;
}
// the pass-through triangle (k = 1: the game's PS exactly), or the band (middle rail C, edge rails U / L) as two strips
[maxvertexcount(8)]
void GSolidA(triangle VA v[3], inout TriangleStream<VAK> os) {
    const Band b = MakeBand(v[0].pos, v[1].pos, v[2].pos);
    if (!b.widen) { os.Append(ToA(v[0], 1.0, 1.0, 0.0, g_q.x)); os.Append(ToA(v[1], 1.0, 1.0, 0.0, g_q.x)); os.Append(ToA(v[2], 1.0, 1.0, 0.0, g_q.x)); return; }
    VA vm = MidA(v[b.sh], v[b.s1]);
    VA vf = v[b.sf];
    VAK mU = ToA(vm, b.k, b.lamE, b.fl, b.fr), mC = ToA(vm, b.k, 1.0, b.fl, b.fr), mL = ToA(vm, b.k, b.lamE, b.fl, b.fr),
         fU = ToA(vf, b.k, b.lamE, b.fl, b.fr), fC = ToA(vf, b.k, 1.0, b.fl, b.fr), fL = ToA(vf, b.k, b.lamE, b.fl, b.fr);
    mU.pos = AtPos(vm.pos, b.m + b.n); mC.pos = AtPos(vm.pos, b.m); mL.pos = AtPos(vm.pos, b.m - b.n);
    fU.pos = AtPos(vf.pos, b.f + b.n); fC.pos = AtPos(vf.pos, b.f); fL.pos = AtPos(vf.pos, b.f - b.n);
    if (g_q.z > 0.5) {                           // phase 109d pole mode: the per-pixel interval (see MakeBand)
        const float4 pk = float4(g_p.z, b.fr, 1.0, -1.0);
        mU.kf = pk; mC.kf = pk; mL.kf = pk; fU.kf = pk; fC.kf = pk; fL.kf = pk;
        mU.dl = DL(b, b.m + b.n); mC.dl = DL(b, b.m); mL.dl = DL(b, b.m - b.n);
        fU.dl = DL(b, b.f + b.n); fC.dl = DL(b, b.f); fL.dl = DL(b, b.f - b.n);
    }
    if (b.pos) { os.Append(mU); os.Append(mC); os.Append(fU); os.Append(fC); os.RestartStrip();
                 os.Append(mC); os.Append(mL); os.Append(fC); os.Append(fL); }
    else       { os.Append(mC); os.Append(mU); os.Append(fC); os.Append(fU); os.RestartStrip();
                 os.Append(mL); os.Append(mC); os.Append(fL); os.Append(fC); }
}
[maxvertexcount(8)]
void GSolidB(triangle VB v[3], inout TriangleStream<VBK> os) {
    const Band b = MakeBand(v[0].pos, v[1].pos, v[2].pos);
    if (!b.widen) { os.Append(ToB(v[0], 1.0, 1.0, 0.0, g_q.x)); os.Append(ToB(v[1], 1.0, 1.0, 0.0, g_q.x)); os.Append(ToB(v[2], 1.0, 1.0, 0.0, g_q.x)); return; }
    VB vm = MidB(v[b.sh], v[b.s1]);
    VB vf = v[b.sf];
    VBK mU = ToB(vm, b.k, b.lamE, b.fl, b.fr), mC = ToB(vm, b.k, 1.0, b.fl, b.fr), mL = ToB(vm, b.k, b.lamE, b.fl, b.fr),
         fU = ToB(vf, b.k, b.lamE, b.fl, b.fr), fC = ToB(vf, b.k, 1.0, b.fl, b.fr), fL = ToB(vf, b.k, b.lamE, b.fl, b.fr);
    mU.pos = AtPos(vm.pos, b.m + b.n); mC.pos = AtPos(vm.pos, b.m); mL.pos = AtPos(vm.pos, b.m - b.n);
    fU.pos = AtPos(vf.pos, b.f + b.n); fC.pos = AtPos(vf.pos, b.f); fL.pos = AtPos(vf.pos, b.f - b.n);
    if (g_q.z > 0.5) {                           // phase 109d pole mode: the per-pixel interval (see MakeBand)
        const float4 pk = float4(g_p.z, b.fr, 1.0, -1.0);
        mU.kf = pk; mC.kf = pk; mL.kf = pk; fU.kf = pk; fC.kf = pk; fL.kf = pk;
        mU.dl = DL(b, b.m + b.n); mC.dl = DL(b, b.m); mL.dl = DL(b, b.m - b.n);
        fU.dl = DL(b, b.f + b.n); fC.dl = DL(b, b.f); fL.dl = DL(b, b.f - b.n);
    }
    if (b.pos) { os.Append(mU); os.Append(mC); os.Append(fU); os.Append(fC); os.RestartStrip();
                 os.Append(mC); os.Append(mL); os.Append(fC); os.Append(fL); }
    else       { os.Append(mC); os.Append(mU); os.Append(fC); os.Append(fU); os.RestartStrip();
                 os.Append(mL); os.Append(mC); os.Append(fL); os.Append(fC); }
}
// phase 107b: the railings / ETS2 street lights (pair D / E), the same band as GSolidA / GSolidB
[maxvertexcount(8)]
void GSolidD(triangle VD v[3], inout TriangleStream<VDK> os) {
    const Band b = MakeBand(v[0].pos, v[1].pos, v[2].pos);
    if (!b.widen) { os.Append(ToD(v[0], 1.0, 1.0, 0.0, g_q.x)); os.Append(ToD(v[1], 1.0, 1.0, 0.0, g_q.x)); os.Append(ToD(v[2], 1.0, 1.0, 0.0, g_q.x)); return; }
    VD vm = MidD(v[b.sh], v[b.s1]);
    VD vf = v[b.sf];
    VDK mU = ToD(vm, b.k, b.lamE, b.fl, b.fr), mC = ToD(vm, b.k, 1.0, b.fl, b.fr), mL = ToD(vm, b.k, b.lamE, b.fl, b.fr),
         fU = ToD(vf, b.k, b.lamE, b.fl, b.fr), fC = ToD(vf, b.k, 1.0, b.fl, b.fr), fL = ToD(vf, b.k, b.lamE, b.fl, b.fr);
    mU.pos = AtPos(vm.pos, b.m + b.n); mC.pos = AtPos(vm.pos, b.m); mL.pos = AtPos(vm.pos, b.m - b.n);
    fU.pos = AtPos(vf.pos, b.f + b.n); fC.pos = AtPos(vf.pos, b.f); fL.pos = AtPos(vf.pos, b.f - b.n);
    if (g_q.z > 0.5) {                           // phase 109d pole mode: the per-pixel interval (see MakeBand)
        const float4 pk = float4(g_p.z, b.fr, 1.0, -1.0);
        mU.kf = pk; mC.kf = pk; mL.kf = pk; fU.kf = pk; fC.kf = pk; fL.kf = pk;
        mU.dl = DL(b, b.m + b.n); mC.dl = DL(b, b.m); mL.dl = DL(b, b.m - b.n);
        fU.dl = DL(b, b.f + b.n); fC.dl = DL(b, b.f); fL.dl = DL(b, b.f - b.n);
    }
    if (b.pos) { os.Append(mU); os.Append(mC); os.Append(fU); os.Append(fC); os.RestartStrip();
                 os.Append(mC); os.Append(mL); os.Append(fC); os.Append(fL); }
    else       { os.Append(mC); os.Append(mU); os.Append(fC); os.Append(fU); os.RestartStrip();
                 os.Append(mL); os.Append(mC); os.Append(fL); os.Append(fC); }
}
// phase 109e: pair F (fences / ETS2 town street lights: VS e5a4956d, TEXCOORD5 a float4)
[maxvertexcount(8)]
void GSolidF(triangle VF v[3], inout TriangleStream<VFK> os) {
    const Band b = MakeBand(v[0].pos, v[1].pos, v[2].pos);
    if (!b.widen) { os.Append(ToF(v[0], 1.0, 1.0, 0.0, g_q.x)); os.Append(ToF(v[1], 1.0, 1.0, 0.0, g_q.x)); os.Append(ToF(v[2], 1.0, 1.0, 0.0, g_q.x)); return; }
    VF vm = MidF(v[b.sh], v[b.s1]);
    VF vf = v[b.sf];
    VFK mU = ToF(vm, b.k, b.lamE, b.fl, b.fr), mC = ToF(vm, b.k, 1.0, b.fl, b.fr), mL = ToF(vm, b.k, b.lamE, b.fl, b.fr),
         fU = ToF(vf, b.k, b.lamE, b.fl, b.fr), fC = ToF(vf, b.k, 1.0, b.fl, b.fr), fL = ToF(vf, b.k, b.lamE, b.fl, b.fr);
    mU.pos = AtPos(vm.pos, b.m + b.n); mC.pos = AtPos(vm.pos, b.m); mL.pos = AtPos(vm.pos, b.m - b.n);
    fU.pos = AtPos(vf.pos, b.f + b.n); fC.pos = AtPos(vf.pos, b.f); fL.pos = AtPos(vf.pos, b.f - b.n);
    if (g_q.z > 0.5) {                           // phase 109d pole mode: the per-pixel interval (see MakeBand)
        const float4 pk = float4(g_p.z, b.fr, 1.0, -1.0);
        mU.kf = pk; mC.kf = pk; mL.kf = pk; fU.kf = pk; fC.kf = pk; fL.kf = pk;
        mU.dl = DL(b, b.m + b.n); mC.dl = DL(b, b.m); mL.dl = DL(b, b.m - b.n);
        fU.dl = DL(b, b.f + b.n); fC.dl = DL(b, b.f); fL.dl = DL(b, b.f - b.n);
    }
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
    noperspective float2 dl : TEXCOORD8;   // phase 109d: pole mode, the pixel's distances to the face's two long edges
};
struct VBK {
    float4 col : COLOR0;
    float4 pos : SV_Position;
    float3 t0  : TEXCOORD0;
    float3 t1  : TEXCOORD1;
    float4 t2  : TEXCOORD2;
    float2 t3  : TEXCOORD3;
    float4 kf  : TEXCOORD7;
    noperspective float2 dl : TEXCOORD8;   // phase 109d: pole mode, the pixel's distances to the face's two long edges
};
struct VDK {             // phase 107b (GSolidD's output)
    float4 col : COLOR0;
    float4 pos : SV_Position;
    float3 t0  : TEXCOORD0;
    float3 t1  : TEXCOORD1;
    float3 t2  : TEXCOORD2;
    float3 t3  : TEXCOORD3;
    float4 t4  : TEXCOORD4;
    float2 t5  : TEXCOORD5;
    float4 kf  : TEXCOORD7;
    noperspective float2 dl : TEXCOORD8;   // phase 109d: pole mode, the pixel's distances to the face's two long edges
};
struct VFK {             // phase 109e (GSolidF's output: VDK with a float4 TEXCOORD5)
    float4 col : COLOR0;
    float4 pos : SV_Position;
    float3 t0  : TEXCOORD0;
    float3 t1  : TEXCOORD1;
    float3 t2  : TEXCOORD2;
    float3 t3  : TEXCOORD3;
    float4 t4  : TEXCOORD4;
    float4 t5  : TEXCOORD5;
    float4 kf  : TEXCOORD7;
    noperspective float2 dl : TEXCOORD8;   // phase 109d: pole mode, the pixel's distances to the face's two long edges
};
cbuffer cbuffer0 : register(b0) { float4 cb0[5]; };   // (phase 107b: [5] for pair D / E's cb0[4]; fxc declares what each entry reads)
Texture2D<float4> texture0  : register(t0);
Texture2D<float4> texture1  : register(t1);
Texture2D<float4> texture6  : register(t6);
Texture2D<float4> texture14 : register(t14);          // phase 107b (pair D / E: the normal map)
SamplerState      sampler0 : register(s0);
SamplerState      sampler1 : register(s1);
SamplerState      sampler2 : register(s2);
SamplerState      sampler3 : register(s3);            // phase 107b
// per pixel: a spatial offset (interleaved gradient noise, Jimenez 2014) + the frame times the golden ratio -- every pixel is kept
// in exactly its share k of the frames, evenly spaced in time (no clumps a short TAA history would see as twinkle), and
// neighbouring pixels are out of phase. Keep the pixel when the value < k. Phase 67: k = the tent (kf.x), raised to the floor kf.w
// on the tent's inner half (lambda kf.z >= 0.5) -- the per-pixel temporal density the keep floor exists for.
bool Dropped(float2 px, float4 kf, float2 dl) {
    const float ign = frac(52.9829189 * frac(dot(floor(px), float2(0.06711056, 0.00583715))));
    // phase 113: the across direction of a pole-mode band = the gradient of the pixel's distance to the face's first long edge (its
    // unit normal; exact: dl is interpolated without perspective). Taken here, in uniform flow, before any discard.
    const float2 g = float2(ddx(dl.x), ddy(dl.x));
    if (kf.w < 0.0) {                            // phase 109d pole mode (kf = (W, frame, 1, -1)): the stratified interval
        const float lo = -max(dl.x, dl.y), hi = -min(dl.x, dl.y);   // the long edges' across positions from this pixel, px
        const float kk = saturate((hi - lo) / kf.x);
        // phase 113 (user: "the poles disappeared"; harness S107 f): a per-PIXEL noise left a column of a 0.37 px post empty for up to
        // 18 frames in a row and a 1 px post 31 % of its frames (the game's own raster: 4 / 0) -- DLAA then lets the post fade. Now ONE
        // value per pixel line ALONG the pole (the same for every pixel across it: the faces' and rows' windows stay disjoint), the
        // window measured from the UN-jittered pixel (GS: the lines moved by the pass's jitter), and in time an 8-frame rank-1 lattice
        // (frame mod 8) x 3 / 8: a line with share kk is shown in round(8 kk) of every 8 frames, evenly spread (the longest empty run
        // for a 0.37 px post: 4 frames, as the game's own raster; the golden ratio: 5, and it beat against the 8-phase jitter while the
        // window moved with it). The plastic constant spreads neighbouring lines over the cycle (no whole-post blink).
        const float lid = abs(g.x) >= abs(g.y) ? floor(px.y) : floor(px.x);   // ("line" is an HLSL keyword)
        const float n = frac(lid * 0.7548776662 + 0.375 * (frac(kf.y * 0.125) * 8.0));
        return frac(n - lo / kf.x) >= kk;
    }
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
    if (Dropped(v.pos.xy, v.kf, v.dl)) discard;
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
    if (Dropped(v.pos.xy, v.kf, v.dl)) discard;
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
// phase 107b, pair C: the crane PS 57a4e1f1 (captures\ps_57a4e1f1.txt, instructions 0 .. 16). o3.w = bfi(1, 13, cb0[0].x, 8064) on
// the RAW bits of cb0[0].x (asuint -- not ftou like pair A's cb0[3].x), o3.y = bit 5 = t1.z > 0.5, o2.w = 0.
void PSSolidC(VAK v, out float4 o0 : SV_Target0, out float4 o1 : SV_Target1, out float4 o2 : SV_Target2, out uint4 o3 : SV_Target3) {
    if (Dropped(v.pos.xy, v.kf, v.dl)) discard;
    o0.xyz = rsqrt(dot(v.t0, v.t0)) * v.t0;                            //  0 .. 2
    o0.w = v.t1.z;                                                      //  3
    float3 r0 = texture1.Sample(sampler1, v.t2).xyz;                    //  4
    r0.x = r0.x * r0.x;                                                 //  5
    o1.w = r0.x * 1024.0;                                               //  6
    o1.xyz = r0.yyy;                                                    //  7
    o3.y = (0.5 < r0.z) ? 32u : 0u;                                     //  8, 9 (bfi(1, 5, lt mask, 0))
    const float3 c = texture0.Sample(sampler0, v.t2).xyz * v.col.xyz;   // 10, 11
    o2.xyz = c + c;                                                     // 12
    o2.w = 0.0;                                                         // 13
    o3.w = ((asuint(cb0[0].x) << 13) & 0x2000u) | 8064u;                // 14
    o3.x = 0u; o3.z = 0u;                                               // 15
}
// phase 107b, pairs D / E: PS 2d7ed0e6 (railings; discard below t6.a 0.05) and d3af2ebb (ETS2 street lights; no discard, o3.z without
// bit 0) -- the same colour maths (captures\ps_2d7ed0e6.txt 3 .. 45, ps_d3af2ebb.txt 0 .. 44). Literals = the bytecode's exact
// bits (2.0078125 = 0x40008000, -1.0078125 = 0xbf810000, 0.39990234375 = 0x3eccc000, 0.60009765625 = 0x3f19a000).
void ShadeD(VDK v, float4 r0, out float4 o0, out float4 o1, out float4 o2) {
    float2 n = texture14.Sample(sampler1, v.t4.xy).xy;
    n = n * float2(2.0078125, 2.0078125) + float2(-1.0078125, -1.0078125);
    float3 r2 = n.y * v.t0;
    r2 = v.t3 * n.x + r2;
    const float z = 1.0 / rsqrt(max(1.0 - dot(n, n), 0.0));
    const float3 nn = v.t1 * z + r2;
    const float ao = min(z * 0.39990234375 + 0.60009765625, 1.0);
    o0.xyz = nn * rsqrt(dot(nn, nn));
    o0.w = v.t2.z;
    float s = v.col.w * 2.0 - 1.0;
    s = ((0.0 < s) ? cb0[4].y : cb0[4].x) * s;
    const float4 b = float4(r0.xyz * cb0[0].xyz, saturate(r0.w * cb0[2].z + cb0[2].w));
    float4 t = texture0.Sample(sampler2, v.t5);
    t.xyz = (0.0 < s) ? t.xyz * b.xyz : t.xyz;
    const float m = texture1.Sample(sampler3, v.t5).x * abs(s);   // (sample r.z, t1.yzxw: dest .z = swizzle[2] = t1.x)
    const float4 r = m * (t - b) + b;
    o1.xyz = r.www * cb0[1].xyz;
    o1.w = cb0[1].w;
    const float3 c = r.xyz * v.col.xyz;
    o2.xyz = ao * (c + c);
    o2.w = cb0[0].w;
}
void PSSolidD(VDK v, out float4 o0 : SV_Target0, out float4 o1 : SV_Target1, out float4 o2 : SV_Target2, out uint4 o3 : SV_Target3) {
    const float4 r0 = texture6.Sample(sampler0, v.t4.zw);              //  0
    if (r0.w < 0.04998779296875) discard;                              //  1, 2: the game's cut-off (0x3d4cc000) BEFORE the dither
    if (Dropped(v.pos.xy, v.kf, v.dl)) discard;
    ShadeD(v, r0, o0, o1, o2);
    uint w;
    o3.z = Tail(w) | 1u; o3.w = w; o3.xy = uint2(0, 0);                // 42: bfi(30, 2, q, 1)
}
void PSSolidE(VDK v, out float4 o0 : SV_Target0, out float4 o1 : SV_Target1, out float4 o2 : SV_Target2, out uint4 o3 : SV_Target3) {
    if (Dropped(v.pos.xy, v.kf, v.dl)) discard;
    ShadeD(v, texture6.Sample(sampler0, v.t4.zw), o0, o1, o2);
    uint w;
    o3.z = Tail(w); o3.w = w; o3.xy = uint2(0, 0);                     // 40: ishl q, 2
}
// phase 109e, pair F: PS 2cc0a6fa (church / cemetery fences, ETS2 town street lights; town audit 2026-11-14, captures\phase109_2026-11-14
// \ps_2cc0a6fa.txt 0 .. 36): the pair-D normal and AO, a detail blend by COLOR.w (t1 at t5.zw), t6 x t0 colour, no discard.
void PSSolidF(VFK v, out float4 o0 : SV_Target0, out float4 o1 : SV_Target1, out float4 o2 : SV_Target2, out uint4 o3 : SV_Target3) {
    if (Dropped(v.pos.xy, v.kf, v.dl)) discard;
    float2 n = texture14.Sample(sampler1, v.t4.xy).xy;                 //  0
    n = n * float2(2.0078125, 2.0078125) + float2(-1.0078125, -1.0078125);   //  1
    float3 r1 = n.y * v.t0;                                             //  2
    r1 = v.t3 * n.x + r1;                                               //  3
    const float z = 1.0 / rsqrt(max(1.0 - dot(n, n), 0.0));            //  4 .. 8
    const float3 nn = v.t1 * z + r1;                                    //  9
    const float ao = min(z * 0.39990234375 + 0.60009765625, 1.0);       // 10, 11
    o0.xyz = nn * rsqrt(dot(nn, nn));                                   // 12 .. 14
    o0.w = v.t2.z;                                                      // 15
    float4 a = texture1.Sample(sampler3, v.t5.zw);                      // 16
    a = v.col.wwww * (-a + 1.0) + a;                                    // 17, 18
    float4 c = texture6.Sample(sampler0, v.t4.zw);                      // 19
    c = c * texture0.Sample(sampler2, v.t5.xy);                         // 20, 21
    a = a * c;                                                          // 22
    o1.xyz = a.www * cb0[1].xyz;                                        // 23
    float3 r0 = a.xyz * cb0[0].xyz;                                     // 24
    o1.w = cb0[1].w;                                                    // 25
    r0 = r0 * (v.col.xyz + v.col.xyz);                                  // 26, 27
    o2.xyz = ao * r0;                                                   // 28
    o2.w = cb0[0].w;                                                    // 29
    uint w;
    o3.z = Tail(w); o3.w = w; o3.xy = uint2(0, 0);                      // 30 .. 36
}
// phase 109e: PS cfc52d43 with pair A's VS 07dfaa00 (far bridge railings, chain-link posts, sailboat rigging; town audit): pair A's
// maths with the game's cut-off on t6.a x COLOR.w (0x3d4cc000) FIRST, then the dither; o3.z = bfi(30, 2, q, 1) = q << 2 | 1.
void PSSolidG(VAK v, out float4 o0 : SV_Target0, out float4 o1 : SV_Target1, out float4 o2 : SV_Target2, out uint4 o3 : SV_Target3) {
    float4 r0 = texture6.Sample(sampler0, v.t2);                        //  0
    if (r0.w * v.col.w < 0.04998779296875) discard;                     //  1 .. 3: the game's cut-off BEFORE the dither
    if (Dropped(v.pos.xy, v.kf, v.dl)) discard;
    o0.xyz = rsqrt(dot(v.t0, v.t0)) * v.t0;                             //  4 .. 6
    o0.w = v.t1.z;                                                      //  7
    r0.w = saturate(r0.w * cb0[2].z + cb0[2].w);                        //  8
    r0.xyz = r0.xyz * cb0[0].xyz;                                       //  9
    o1.xyz = r0.www * cb0[1].xyz;                                       // 10
    o1.w = cb0[1].w;                                                    // 11
    const float3 r1 = v.col.xyz + v.col.xyz;                            // 12
    o2.xyz = r0.xyz * r1;                                               // 13
    o2.w = cb0[0].w;                                                    // 14
    uint w;
    o3.z = Tail(w) | 1u; o3.w = w; o3.xy = uint2(0, 0);                 // 15 .. 21
}
// phase 109d: PS e19e0136 with pair A's VS (the red / white STRIPE band of the ETS2 road poles, also road-sign faces; forest audit):
// albedo 2 x t6.rgb x COLOR.rgb, o1.xyz = t6.a x COLOR.a x 0x3e4cc000, o1.w = 60, o2.w = 0, o3.w = bfi(1, 13, ftou(cb0[3].x), 8064).
void PSSolidH(VAK v, out float4 o0 : SV_Target0, out float4 o1 : SV_Target1, out float4 o2 : SV_Target2, out uint4 o3 : SV_Target3) {
    if (Dropped(v.pos.xy, v.kf, v.dl)) discard;
    o0.xyz = rsqrt(dot(v.t0, v.t0)) * v.t0;                             //  0 .. 2
    o0.w = v.t1.z;                                                      //  3
    float4 r0 = texture6.Sample(sampler0, v.t2);                        //  4
    r0 = r0.wxyz * v.col.wxyz;                                          //  5
    o1.xyz = r0.xxx * 0.199951171875;                                   //  6
    o2.xyz = r0.yzw + r0.yzw;                                           //  7
    o1.w = 60.0;                                                        //  8
    o2.w = 0.0;                                                         //  9
    uint w;
    Tail(w);
    o3.w = w; o3.xyz = uint3(0, 0, 0);                                  // 10 .. 12
}
// phase 111: PS 2585159b with pair A's VS 07dfaa00 (the sign / road-furniture atlas of e19e0136; the user's Norway census capture,
// censusdfaa00_2585159b\ps.txt 0 .. 16): pair A's maths without the gloss -- o1 = cb0[1] whole, t6.a unused.
void PSSolidI(VAK v, out float4 o0 : SV_Target0, out float4 o1 : SV_Target1, out float4 o2 : SV_Target2, out uint4 o3 : SV_Target3) {
    if (Dropped(v.pos.xy, v.kf, v.dl)) discard;
    o0.xyz = rsqrt(dot(v.t0, v.t0)) * v.t0;                             //  0 .. 2
    o0.w = v.t1.z;                                                      //  3
    o1 = cb0[1];                                                        //  4
    float3 r0 = texture6.Sample(sampler0, v.t2).xyz;                    //  5
    r0 = r0 * cb0[0].xyz;                                               //  6
    const float3 r1 = v.col.xyz + v.col.xyz;                            //  7
    o2.xyz = r0 * r1;                                                   //  8
    o2.w = cb0[0].w;                                                    //  9
    uint w;
    o3.z = Tail(w); o3.w = w; o3.xy = uint2(0, 0);                      // 10 .. 16
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
ComPtr<ID3D11GeometryShader> g_gsSolid[kSolidGsN];  // phase 63: GSolidA / GSolidB (phase 107b: + GSolidD)
ComPtr<ID3D11PixelShader>    g_psSolid[kSolidKinds]; //          PSSolidA / PSSolidB (phase 107b: + PSSolidC / D / E), index kind - 1
ComPtr<ID3D11Buffer>         g_cbSolid;             //           32 bytes: g_p + g_q (frame)
float                        g_cbSolidVal[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
bool                         g_solidFailed = false;
std::atomic<bool>            g_solidDead{false};    // phase 109r: a solid GS / PS / cb could not be created (until Reset)
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
    if (g_cbSolid) return true;                          // (set last: every GS / PS object exists)
    if (g_solidFailed) return false;
    const ShaderCache::Id gsIds[kSolidGsN] = { ShaderCache::kSolidGsA, ShaderCache::kSolidGsB, ShaderCache::kSolidGsD,
                                               ShaderCache::kSolidGsF };   // phase 109e: + GSolidF
    const ShaderCache::Id psIds[kSolidKinds] = { ShaderCache::kSolidPsA, ShaderCache::kSolidPsB, ShaderCache::kSolidPsC,
                                                 ShaderCache::kSolidPsD, ShaderCache::kSolidPsE,    // phase 107b: + C / D / E
                                                 ShaderCache::kSolidPsF, ShaderCache::kSolidPsG, ShaderCache::kSolidPsH,   // 109d / e
                                                 ShaderCache::kSolidPsI };                                                  // phase 111
    for (ShaderCache::Id id : gsIds) if (!ShaderCache::Ready(id)) { ++g_st.skipNotReady; return false; }
    for (ShaderCache::Id id : psIds) if (!ShaderCache::Ready(id)) { ++g_st.skipNotReady; return false; }
    g_solidFailed = true;
    auto fail = [&](ShaderCache::Id id) {
        LogErr("wire widening: solid-geometry shader %s failed (%s) -- far solid wires, towers, cranes, railings and street lights stay "
            "as the game draws them", ShaderCache::Name(id), ShaderCache::Error(id));
        for (auto& g : g_gsSolid) g.Reset();
        for (auto& p : g_psSolid) p.Reset();
        g_solidDead.store(true, std::memory_order_relaxed);
        return false;
    };
    for (int i = 0; i < kSolidGsN; ++i) {
        size_t sz = 0;
        const void* c = ShaderCache::Code(gsIds[i], &sz);
        if (!c || FAILED(dev->CreateGeometryShader(c, sz, nullptr, &g_gsSolid[i]))) return fail(gsIds[i]);
    }
    for (int i = 0; i < kSolidKinds; ++i) {
        size_t sz = 0;
        const void* c = ShaderCache::Code(psIds[i], &sz);
        if (!c || FAILED(dev->CreatePixelShader(c, sz, nullptr, &g_psSolid[i]))) return fail(psIds[i]);
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 32; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_cbSolid))) {
        LogErr("wire widening: solid constant buffer failed");
        g_solidDead.store(true, std::memory_order_relaxed);
        return false;
    }
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
    ShaderCache::Add(ShaderCache::kSolidGsD, kSolidGs, sizeof(kSolidGs) - 1, "wire_widen_solid_gs_d", "GSolidD", "gs_5_0");   // phase 107b
    ShaderCache::Add(ShaderCache::kSolidPsC, kSolidPs, sizeof(kSolidPs) - 1, "wire_widen_solid_ps_c", "PSSolidC", "ps_5_0");
    ShaderCache::Add(ShaderCache::kSolidPsD, kSolidPs, sizeof(kSolidPs) - 1, "wire_widen_solid_ps_d", "PSSolidD", "ps_5_0");
    ShaderCache::Add(ShaderCache::kSolidPsE, kSolidPs, sizeof(kSolidPs) - 1, "wire_widen_solid_ps_e", "PSSolidE", "ps_5_0");
    ShaderCache::Add(ShaderCache::kSolidGsF, kSolidGs, sizeof(kSolidGs) - 1, "wire_widen_solid_gs_f", "GSolidF", "gs_5_0");   // phase 109e
    ShaderCache::Add(ShaderCache::kSolidPsF, kSolidPs, sizeof(kSolidPs) - 1, "wire_widen_solid_ps_f", "PSSolidF", "ps_5_0");
    ShaderCache::Add(ShaderCache::kSolidPsG, kSolidPs, sizeof(kSolidPs) - 1, "wire_widen_solid_ps_g", "PSSolidG", "ps_5_0");
    ShaderCache::Add(ShaderCache::kSolidPsH, kSolidPs, sizeof(kSolidPs) - 1, "wire_widen_solid_ps_h", "PSSolidH", "ps_5_0");   // phase 109d
    ShaderCache::Add(ShaderCache::kSolidPsI, kSolidPs, sizeof(kSolidPs) - 1, "wire_widen_solid_ps_i", "PSSolidI", "ps_5_0");   // phase 111
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

// phase 63: a solid G-buffer draw of pair `kind` (1 A wires / 2 B towers; phase 107b: 3 C cranes / 4 D railings / 5 E ETS2 street
// lights): the pair's GS (SolidGsOf) + its dither PS copy, frame = the dither frame
bool BindSolid(ID3D11DeviceContext* ctx, int kind, float vpW, float vpH, float minPx, uint32_t frame, float minShare, bool pole,
               float jitX, float jitY) {
    if (!ctx || kind < 1 || kind > kSolidKinds || !(vpW > 1.0f) || !(vpH > 1.0f)) return false;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!Ensure(dev.Get()) || !EnsureSolid(dev.Get())) return false;
    ID3D11GeometryShader* prev = nullptr;
    ctx->GSGetShader(&prev, nullptr, nullptr);
    if (prev) { prev->Release(); ++g_st.skipGs; return false; }
    // phase 113: pole mode also gets the pass's viewport jitter (screen px, y down) in the band's frame (y up): see MakeBand
    const float v[8] = { 0.5f * vpW, 0.5f * vpH, minPx, pole ? jitX : 0.0f, (float)(frame & 1023u), pole ? 0.0f : minShare,
                         pole ? 1.0f : 0.0f, pole ? -jitY : 0.0f };
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
    ctx->GSSetShader(g_gsSolid[SolidGsOf(kind)].Get(), nullptr, 0);
    g_psSwapped = true;
    ++g_st.boundSolid;
    return true;
}

float g_replayMinPx = 1.0f, g_replayPoleMinPx = 1.0f;   // (phase 113: pole_min_px default 1.0)
void SetReplayMinPx(float minPx) { g_replayMinPx = minPx; }
void SetReplayPoleMinPx(float minPx) { g_replayPoleMinPx = minPx; }
bool BindReplay(ID3D11DeviceContext* ctx, int kind, float vpW, float vpH, float scale) {
    if (!ctx) return false;
    if (kind == 0) { ctx->GSSetShader(nullptr, nullptr, 0); ID3D11Buffer* n = nullptr; ctx->GSSetConstantBuffers(0, 1, &n); return true; }
    if (!(vpW > 1.0f) || !(vpH > 1.0f)) return false;
    const bool pole = (kind & kReplayPole) != 0;        // phase 109r: a pole_aa record (pole mode, pole_min_px)
    kind &= ~kReplayPole;
    if (pole && (kind < 2 || kind >= 2 + kSolidGsN)) return false;
    ComPtr<ID3D11Device> dev;
    ctx->GetDevice(&dev);
    if (!Ensure(dev.Get())) return false;
    const float W = (pole ? g_replayPoleMinPx : g_replayMinPx) * (scale > 0.0f ? scale : 1.0f);
    if (kind == 1) {
        if (!EnsureCut(dev.Get())) return false;
        const float v[4] = { 0.5f * vpW, 0.5f * vpH, W, 1.0f };
        if (memcmp(v, g_cbVal, sizeof(v)) != 0) { ctx->UpdateSubresource(g_cb.Get(), 0, nullptr, v, 0, 0); memcpy(g_cbVal, v, sizeof(v)); ++g_st.cbUpdates; }
        ID3D11Buffer* cb = g_cb.Get();
        ctx->GSSetConstantBuffers(0, 1, &cb);
        ctx->GSSetShader(g_gsCut.Get(), nullptr, 0);
        return true;
    }
    if (kind < 2 || kind >= 2 + kSolidGsN) return false;   // 2 + SolidGsOf (phase 107b: never the PS kind -- 4 = GSolidD)
    if (!EnsureSolid(dev.Get())) return false;
    // keep share 1 (not read by the replay PS); g_q.z = 1: pole mode (the per-face boxes of the pole's colour band, phase 109r)
    const float v[8] = { 0.5f * vpW, 0.5f * vpH, W, 0.0f, 0.0f, 1.0f, pole ? 1.0f : 0.0f, 0.0f };
    if (memcmp(v, g_cbSolidVal, sizeof(v)) != 0) { ctx->UpdateSubresource(g_cbSolid.Get(), 0, nullptr, v, 0, 0); memcpy(g_cbSolidVal, v, sizeof(v)); ++g_st.cbUpdates; }
    ID3D11Buffer* cb = g_cbSolid.Get();
    ctx->GSSetConstantBuffers(0, 1, &cb);
    ctx->GSSetShader(g_gsSolid[kind - 2].Get(), nullptr, 0);
    return true;
}

bool PsSwapped() { return g_psSwapped; }
bool SolidDead() { return g_solidDead.load(std::memory_order_relaxed); }

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
    for (auto& g : g_gsSolid) g.Reset();                 // phase 63 (phase 107b: 3 GS, 5 PS)
    for (auto& p : g_psSolid) p.Reset();
    g_cbSolid.Reset(); g_solidFailed = false; g_solidDead.store(false, std::memory_order_relaxed);
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
