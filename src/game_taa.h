// GameTaa -- phase 82: the GAME's own TAA together with the mod (dlaa.ini game_taa / game_taa_mix, menu rows "Game TAA" /
// "Game TAA share"). Pure logic, no D3D: inject.cpp feeds it, tests\game_taa_test.cpp drives it.
//
// WHY: the mod's scope is the game's AA set to Disabled (r_aa 0): the mod jitters the viewport and DLAA / DLSS resolves it. With the
// game's AA set to TAA (r_aa 5) the game jitters its OWN projection and resolves it in a TAA pass before the tonemap; the mod's
// DLAA then ran on an already TAA'd frame (stacked = blur) and the two jitters fought. game_taa lets a player run both:
//   0 Off (default)  = the mod exactly as before. A TAA resolve seen while 0 logs one WARN (game AA should be Disabled).
//   1 Mix            = the game's TAA and the mod's DLAA / DLSS on the SAME raw jittered frame, then blended:
//                      game_taa_mix = the TAA share 0..100 % (default 50, menu steps of 10).
//   2 TAA only       = the mod's main-scene DLAA / DLSS is skipped (an A/B inside the mod; mirrors etc. unchanged).
//   3 Split          = left half the mod's DLAA / DLSS, right half the game's TAA, a thin black divider (compare view).
// In modes 1..3, while the TAA resolve is LIVE (seen within kLiveFrames Presents), the mod's own viewport jitter is HELD (off) for
// the world passes and NGX gets the GAME's jitter of the pass (cb0[8].xy of the TAA resolve, UV units -> pixels). Without a live
// TAA resolve (game AA not TAA, VR if SCS has no TAA there, a game update that changed the shader) everything is mode 0.
//
// THE GAME'S TAA (RenderDoc, ATS flat 3840x2160, captures/taa_2026-10-30/FINDINGS.md), all full-res RGBA16F before the tonemap:
//   PS 535 a7d091c8-...  range compress c / (1 + c k) of the lit scene (t0 = the RAW jittered lit scene) -> the TAA input
//   PS 533 1f2a4a6c-...  3x3 nearest depth -> R16G16_UINT (ping-pong pair)
//   PS 524 6b3e199e-...  TAA RESOLVE: 2 RGBA16F outputs (result + next history); cb0[8].xy = jitter (UV), cb0[8].zw = 0.1 weight,
//                        cb0[11].xy = the target size
//   PS 534 62e1d817-...  luma sharpen + inverse compress -> linear HDR = the tonemap's input (t0 of the tonemap draw)
//   PS 327               tonemap -> RGBA8_SRGB; then the swapchain / eye blit (the mod's post-tonemap DLAA hook point).
// The mod never writes into the resolve's outputs (its history stays pure TAA): its result is blended into the tonemap's INPUT
// (HDR path) or over the game's blit (LDR / DLSS path) -- see inject.cpp "phase 82".
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace GameTaa {

enum Mode : int { kOff = 0, kMix = 1, kTaaOnly = 2, kSplit = 3, kModeCount = 4 };
constexpr int kMixDefault = 50;      // game_taa_mix: the TAA share in %
constexpr int kMixStep = 10;         // menu step
constexpr uint64_t kLiveFrames = 30; // the TAA resolve counts as live this many Presents after it was last seen

// DXBC checksums (bytes 4..19 of the bytecode as 4 little-endian dwords = RenderDoc's "Shader hash"), ATS 2026-10-30.
constexpr uint32_t kResolvePs[4]  = { 0x6b3e199eu, 0xb5bb6baau, 0xb1149c64u, 0x608b8a38u };   // PS 524 TAA resolve
constexpr uint32_t kCompressPs[4] = { 0xa7d091c8u, 0x9f069c86u, 0x2f1d45b6u, 0x973271c1u };   // PS 535 range compress (t0 = raw)
inline bool ChecksumIs(const void* code, size_t len, const uint32_t want[4]) {
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

inline int ClampMode(int m) { return m < kOff || m >= kModeCount ? kOff : m; }
inline int ClampMix(int pct) { return pct < 0 ? 0 : (pct > 100 ? 100 : pct); }
// menu: Right = next mode, Left = previous (wraps); the share in kMixStep steps, clamped (no wrap)
inline int NextMode(int m, int dir) { return (ClampMode(m) + (dir < 0 ? kModeCount - 1 : 1)) % kModeCount; }
inline int StepMix(int pct, int dir) {   // the next grid value in that direction (a dlaa.ini 45 -> 50 / 40)
    const int p = ClampMix(pct);
    return ClampMix(dir < 0 ? ((p + kMixStep - 1) / kMixStep - 1) * kMixStep : (p / kMixStep + 1) * kMixStep);
}
inline const char* ModeName(int m) {
    switch (ClampMode(m)) {
    case kMix: return "mix";
    case kTaaOnly: return "TAA only";
    case kSplit: return "split";
    default: return "off";
    }
}

// The TAA resolve counts as live `lastSeen` (Present number of its last draw, 0 = never) .. + kLiveFrames.
inline bool Live(uint64_t lastSeen, uint64_t now) { return lastSeen != 0 && now >= lastSeen && now - lastSeen <= kLiveFrames; }
// The mod's own world jitter is held for a pass that starts now.
inline bool Hold(int mode, uint64_t lastSeen, uint64_t now) { return ClampMode(mode) != kOff && Live(lastSeen, now); }

// The game's jitter of a pass from the resolve's constant buffer (cb0 from the bound offset, `n` floats, >= 48 needed: rows 0..11).
// Row 8 .xy = the jitter in UV, row 11 .xy = the resolve's target size. Accepted only when row 11 is the target (w x h, +-0.5) and
// the jitter is finite and within +-1 px (TAA jitter is sub-pixel): anything else = not the TAA constants (stale / wrong region).
// Out: the shift in PIXELS (x right, y down: the same axes as the mod's own viewport shift, which NGX gets times jitter_sign).
inline bool ParseJitter(const float* cb, size_t n, uint32_t w, uint32_t h, float* sx, float* sy) {
    if (!cb || n < 48 || !w || !h) return false;
    const float jx = cb[32], jy = cb[33], tw = cb[44], th = cb[45];
    if (!(std::fabs(tw - (float)w) <= 0.5f && std::fabs(th - (float)h) <= 0.5f)) return false;
    if (!std::isfinite(jx) || !std::isfinite(jy)) return false;
    const float px = jx * (float)w, py = jy * (float)h;
    if (std::fabs(px) > 1.0f || std::fabs(py) > 1.0f) return false;
    *sx = px; *sy = py;
    return true;
}

// What the main scene of one pass gets (decided at its tonemap; the blit follows it).
enum Act : int {
    kActNone = 0,    // not held: the mod exactly as without game_taa (mode 0, or no live TAA resolve)
    kActHdr = 1,     // the HDR unit runs at the tonemap on the raw scene, its result is blended into the tonemap's input
    kActLdr = 2,     // the game's tonemap is re-issued on the raw scene into a mod texture; the blit unit (DLAA / DLSS) runs on it
                     // and its result is composited over the game's blit
    kActTaaOnly = 3  // the mod's main-scene DLAA / DLSS is skipped for this pass (mode 2, or a held pass the mix cannot take)
};
struct PassIn {
    int  mode = kOff;
    bool hold = false;       // the mod's jitter was held for this pass (StartPass)
    bool resolveSeen = false;// the TAA resolve drew in this pass
    bool jitterOk = false;   // ParseJitter accepted the pass's resolve constants
    bool rawOk = false;      // the raw jittered scene (the compress pass's t0 / the forward colour) is known for this pass
    bool preHdr = false;     // the mod's stage logic wants the pre-tonemap HDR unit for this pass (flat DLAA, no upscaling)
};
// why = a short reason for a kActTaaOnly fallback (logged once per change), nullptr otherwise
inline Act Decide(const PassIn& in, const char** why) {
    if (why) *why = nullptr;
    const int m = ClampMode(in.mode);
    if (m == kOff || !in.hold) return kActNone;
    const char* w = nullptr;
    if (m == kTaaOnly) w = nullptr;
    else if (!in.resolveSeen) w = "the TAA resolve did not draw in this pass";
    else if (!in.jitterOk) w = "the game's jitter could not be read from the TAA resolve's constants";
    else if (!in.rawOk) w = "the raw jittered scene (the TAA input) is not known for this pass";
    else return in.preHdr ? kActHdr : kActLdr;
    if (why) *why = w;
    return kActTaaOnly;
}

// The composite's parameters for a mode: the mod's share (alpha) and the split.
struct Mix { float modShare = 1.0f; bool split = false; };
inline Mix MixOf(int mode, int taaPct) {
    Mix x;
    if (ClampMode(mode) == kMix) x.modShare = 1.0f - (float)ClampMix(taaPct) / 100.0f;
    else if (ClampMode(mode) == kSplit) x.split = true;
    return x;
}

}  // namespace GameTaa
