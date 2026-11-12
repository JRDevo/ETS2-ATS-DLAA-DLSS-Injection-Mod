// GameTaa -- phase 82: the GAME's own TAA together with the mod (dlaa.ini game_taa / game_taa_mix / game_taa_split, menu rows
// "Game TAA" / "Game TAA share" / "Split position"). Pure logic, no D3D: inject.cpp feeds it, tests\game_taa_test.cpp drives it.
//
// WHY: the mod's scope is the game's AA set to Disabled (r_aa 0): the mod jitters the viewport and DLAA / DLSS resolves it. With the
// game's AA set to TAA (r_aa 5) the game jitters its OWN projection and resolves it in a TAA pass before the tonemap; the mod's
// DLAA then ran on an already TAA'd frame (stacked = blur) and the two jitters fought. game_taa lets a player run both:
//   0 Off (default)  = the mod exactly as before. A TAA resolve seen while 0 logs one WARN (game AA should be Disabled).
//   1 Mix            = the game's TAA and the mod's DLAA / DLSS on the SAME raw jittered frame, then blended:
//                      game_taa_mix = the TAA share 0..100 % (default 50, menu steps of 10).
//   2 Split          = left of game_taa_split (% of the width, default 50) the mod's DLAA / DLSS, right of it the game's TAA, a thin
//                      line and small "DLAA" / "DLSS" | "TAA" labels at the top (compare view).
// (The first build had 2 = TAA only, 3 = split: TAA only is dropped -- DLAA off with the game's TAA on is the same picture; an old
// dlaa.ini without game_taa_ver = 2 is migrated: 2 -> 0, 3 -> 2, MigrateMode.)
// In modes 1..2, while the TAA resolve is LIVE (seen within kLiveFrames Presents), the mod's own viewport jitter is HELD (off) for
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
#include <cstdio>
#include <cstring>

namespace GameTaa {

enum Mode : int { kOff = 0, kMix = 1, kSplit = 2, kModeCount = 3 };
constexpr int kMixDefault = 50;      // game_taa_mix: the TAA share in %
constexpr int kMixStep = 10;         // menu step
constexpr int kSplitDefault = 50;    // game_taa_split: the split line's position in % of the width
constexpr int kSplitStep = 5;        // menu step
constexpr int kIniVersion = 2;       // dlaa.ini game_taa_ver: the mode numbering above (absent = the first build's 0..3)
// a game_taa value of the first build (0 off, 1 mix, 2 TAA only, 3 split) -> this numbering
inline int MigrateMode(int old) { return old == 1 ? kMix : (old == 3 ? kSplit : kOff); }
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
inline int StepGrid(int pct, int dir, int step) {   // the next grid value in that direction (a dlaa.ini 45 -> 50 / 40 at step 10)
    const int p = ClampMix(pct);
    return ClampMix(dir < 0 ? ((p + step - 1) / step - 1) * step : (p / step + 1) * step);
}
inline int StepMix(int pct, int dir) { return StepGrid(pct, dir, kMixStep); }
inline int StepSplit(int pct, int dir) { return StepGrid(pct, dir, kSplitStep); }
// the split column in a target: x0 + round(width * pct / 100)
inline float SplitX(float x0, float width, int pct) { return x0 + std::floor(width * (float)ClampMix(pct) / 100.0f + 0.5f); }
// VR (VR game test: two red lines seen double): the split at the same offset from the picture's OPTICAL centre cu (uv) in every eye =
// the same view angle (both eyes have the same total fov, so the same uv scale): x0 + width * (cu + (pct - 50) / 100), clamped into
// the picture. Flat: cu = 0.5 = SplitX.
inline float SplitXc(float x0, float width, int pct, float cu) {
    float u = cu + ((float)ClampMix(pct) - 50.0f) / 100.0f;
    u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
    return x0 + std::floor(width * u + 0.5f);
}
inline const char* ModeName(int m) {
    switch (ClampMode(m)) {
    case kMix: return "mix";
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
    kActTaaOnly = 3  // the mod's main-scene DLAA / DLSS is skipped for this pass (a held pass the mix cannot take: a FALLBACK)
};
// why a held pass fell back to TAA only (the menu's status line says it in the player's language; the log has the long text)
enum Why : int { kWhyNone = 0, kWhyNoResolve, kWhyNoJitter, kWhyNoRaw, kWhyRunFailed, kWhyHdrOut, kWhyDlaaOff, kWhyNoTonemap,
                 kWhyCount };
struct PassIn {
    int  mode = kOff;
    bool hold = false;       // the mod's jitter was held for this pass (StartPass)
    bool resolveSeen = false;// the TAA resolve drew in this pass
    bool jitterOk = false;   // ParseJitter accepted the pass's resolve constants
    bool rawOk = false;      // the raw jittered scene (the compress pass's t0 / the forward colour) is known for this pass
    bool preHdr = false;     // the mod's stage logic wants the pre-tonemap HDR unit for this pass (flat DLAA, no upscaling)
};
// why = a short reason for a kActTaaOnly fallback (logged once per change), nullptr otherwise
inline Act Decide(const PassIn& in, const char** why, int* whyCode = nullptr) {
    if (why) *why = nullptr;
    if (whyCode) *whyCode = kWhyNone;
    const int m = ClampMode(in.mode);
    if (m == kOff || !in.hold) return kActNone;
    const char* w = nullptr;
    int c = kWhyNone;
    if (!in.resolveSeen) { w = "the TAA resolve did not draw in this pass"; c = kWhyNoResolve; }
    else if (!in.jitterOk) { w = "the game's jitter could not be read from the TAA resolve's constants"; c = kWhyNoJitter; }
    else if (!in.rawOk) { w = "the raw jittered scene (the TAA input) is not known for this pass"; c = kWhyNoRaw; }
    else return in.preHdr ? kActHdr : kActLdr;
    if (why) *why = w;
    if (whyCode) *whyCode = c;
    return kActTaaOnly;
}

// The split overlay (drawn after the game's blit, every frame while game_taa = 2): an opaque red line of SplitLineWidth px (2 px per
// 1080 px of the picture's height, at least 2) and two labels in a 5 x 7 pixel font. PackLabel: glyph codes 0 space, 1 D, 2 L, 3 A,
// 4 S, 5 T, 6 O, 7 F, 8 (, 9 ), 10 M, 11 I, 12 X, 13 %, 14..23 the digits 0..9 -- 5 bits each, 4 per part (exact in a float), 3
// parts = up to 12 characters (case-insensitive; other characters = space).
inline float SplitLineWidth(uint32_t h) {          // (capped at 6: a VR eye picture of 5848 px would give 11 px)
    const float w = std::floor(2.0f * (float)h / 1080.0f + 0.5f);
    return w < 2.0f ? 2.0f : (w > 6.0f ? 6.0f : w);
}
inline uint32_t GlyphCode(char c) {
    if (c >= '0' && c <= '9') return 14u + (uint32_t)(c - '0');
    switch (c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c) {
    case 'D': return 1; case 'L': return 2; case 'A': return 3; case 'S': return 4; case 'T': return 5;
    case 'O': return 6; case 'F': return 7; case '(': return 8; case ')': return 9; case 'M': return 10; case 'I': return 11;
    case 'X': return 12; case '%': return 13; default: return 0;
    }
}
struct Label { uint32_t p[3] = { 0, 0, 0 }; uint32_t n = 0; };
inline Label PackLabel(const char* s) {
    Label l;
    for (uint32_t i = 0; s && s[i] && i < 12; ++i) {
        l.p[i / 4u] |= GlyphCode(s[i]) << (5u * (i % 4u));
        l.n = i + 1;
    }
    return l;
}
// one side's label: "DLAA" / "DLSS" (the mod; + " (OFF)" when it did not run on this pass), "TAA" (+ " (OFF)" when the game's TAA is
// not running), "MIX 50%" (the blend; + " (OFF)" when either is missing). `out` gets up to 12 characters.
enum Side : int { kSideMod = 0, kSideTaa = 1, kSideMix = 2 };
inline void SideLabel(int side, bool dlss, bool modRan, bool taaLive, int taaPct, char* out, size_t cap) {
    if (!out || !cap) return;
    const char* off = " (OFF)";
    if (side == kSideTaa) snprintf(out, cap, "TAA%s", taaLive ? "" : off);
    else if (side == kSideMix) {
        if (modRan && taaLive) snprintf(out, cap, "MIX %d%%", ClampMix(taaPct)); else snprintf(out, cap, "MIX%s", off);
    } else snprintf(out, cap, "%s%s", dlss ? "DLSS" : "DLAA", modRan ? "" : off);
}

// The composite's parameters for a mode: the mod's share (alpha) and the split.
// game_taa_pair (menu "Split compares", only in split): what the two sides show -- 0 "DLAA | TAA" (default: the mod left, the game's
// TAA right), 1 "Mix | TAA", 2 "DLAA | Mix"; Mix = the mod's result blended with the game's TAA at game_taa_mix.
enum Pair : int { kPairDlaaTaa = 0, kPairMixTaa = 1, kPairDlaaMix = 2, kPairCount = 3 };
inline int ClampPair(int p) { return p < 0 || p >= kPairCount ? kPairDlaaTaa : p; }
inline int NextPair(int p, int dir) { return (ClampPair(p) + (dir < 0 ? kPairCount - 1 : 1)) % kPairCount; }
inline bool PairHasMix(int p) { return ClampPair(p) != kPairDlaaTaa; }
inline const char* PairName(int p) {
    switch (ClampPair(p)) { case kPairMixTaa: return "Mix | TAA"; case kPairDlaaMix: return "DLAA | Mix"; default: return "DLAA | TAA"; }
}
// the share row matters: mode mix, or split with a Mix side
inline bool ShareUsed(int mode, int pair) { return ClampMode(mode) == kMix || (ClampMode(mode) == kSplit && PairHasMix(pair)); }

// The composite's parameters for a mode: the mod's share (alpha) -- left of the split line in split -- and right of it.
struct Mix { float modShare = 1.0f; bool split = false; float rightShare = 0.0f; };
inline Mix MixOf(int mode, int taaPct, int pair = kPairDlaaTaa) {
    Mix x;
    const float mixShare = 1.0f - (float)ClampMix(taaPct) / 100.0f;
    if (ClampMode(mode) == kMix) x.modShare = mixShare;
    else if (ClampMode(mode) == kSplit) {
        x.split = true;
        switch (ClampPair(pair)) {
        case kPairMixTaa: x.modShare = mixShare; x.rightShare = 0.0f; break;
        case kPairDlaaMix: x.modShare = 1.0f; x.rightShare = mixShare; break;
        default: x.modShare = 1.0f; x.rightShare = 0.0f; break;
        }
    }
    return x;
}
// the two sides of a pair
inline void PairSides(int pair, int* left, int* right) {
    switch (ClampPair(pair)) {
    case kPairMixTaa: *left = kSideMix; *right = kSideTaa; break;
    case kPairDlaaMix: *left = kSideMod; *right = kSideMix; break;
    default: *left = kSideMod; *right = kSideTaa; break;
    }
}

// ---- CbShadow: a CPU copy of the TAA resolve's constants, taken at the game's own UPLOAD (first in-game test, ATS flat 2026-10-30) --
// The resolve reads its cb0 from one of several 2 MB DYNAMIC constant-buffer rings that rotate per frame (3 seen in flat, SIX in VR),
// at an offset that moves a little (813824 -> 812800 bytes), and each ring is filled with ONE Map(WRITE_DISCARD) / Unmap per frame,
// on the game's MAIN thread, before the render thread draws with it. So the copy cannot wait for "the buffer the resolve bound".
// Phase 85 (VR audit: jitter misses in bursts): (1) WATCH BY SHAPE -- once a resolve has shown the ring's ByteWidth, every DYNAMIC
// constant buffer of that width is watched from its first Map (Watch; kBufs 12, LRU), so a ring is copied at its very first fill
// (with kBufs 4 and 6 rings the LRU evicted each ring before it came round = a miss every time); (2) PER-OFFSET CLUSTERS -- the
// copy is not one min..max range of every offset any more (VR: the two eyes' offsets sit at both ends of the ring, so one range
// was ~2 MB, capped, centred on the last offset = the other eye missed) but the CLUSTERS of the recent offsets (kHist, all
// buffers: the rings share their layout), each offset + 256 bytes + kPad each side, circular mod ByteWidth (an offset near the end
// also covers the start), merged, most recent first up to kMaxSpan. That also cuts the per-Unmap copy out of write-combined
// memory from up to 1.5 MB to ~2 x 64 KB. The resolve's draw reads its offset from the copy of THE BUFFER IT BOUND when one of the
// copied segments covers it and that copy is newer than this buffer's previous read (or from the same frame: a VR pair). A miss
// (the offset was new when the ring was filled: grown) is counted; the next fill covers it.
// Thread-safety is the caller's (inject.cpp: one mutex; Map / Unmap come from the main thread, the resolve draw from the render one).
class CbShadow {
public:
    static constexpr int      kBufs = 12, kHist = 32, kMaxSeg = 8;
    static constexpr uint32_t kPad = 32u * 1024u, kMaxSpan = 1024u * 1024u;
    typedef void (*CopyFn)(void* dst, const void* src, size_t n);   // inject.cpp passes a write-combined-friendly copy

    // a resolve draw bound `buf` (ByteWidth `width`) at byte offset `off`: watch it, learn the ring's shape, remember the offset
    void Learn(const void* buf, uint32_t width, uint32_t off) {
        m_width = width;
        m_recent[m_histN % kHist] = off;
        ++m_histN;
        Watch(buf, width);
    }
    // the ring's ByteWidth seen at the resolves (0 = none yet): a DYNAMIC constant buffer of this width is watched from its first Map
    uint32_t Shape() const { return m_width; }
    // watch `buf` (a buffer of the ring's shape, or one a resolve bound); the oldest watched buffer makes room
    void Watch(const void* buf, uint32_t width) {
        if (!buf) return;
        Entry* e = Find(buf);
        if (!e) {
            e = Lru();
            *e = Entry{};
            e->buf = buf;
            ++m_watches;
        }
        e->width = width;
        e->use = ++m_tick;
    }
    bool Watched(const void* buf) const { return buf && FindC(buf) != nullptr; }
    // the game's Map of a watched buffer returned `ptr` (the whole buffer); the copy is taken at its Unmap
    void OnMap(const void* buf, const void* ptr) { if (Entry* e = Find(buf)) { e->mapPtr = ptr; e->use = ++m_tick; } }
    // Unmap: the segments of the bytes the Map returned
    bool OnUnmap(const void* buf, CopyFn copy) {
        Entry* e = Find(buf);
        if (!e || !e->mapPtr) return false;
        const void* p = e->mapPtr;
        e->mapPtr = nullptr;
        return Upload(*e, p, 0, e->width, copy);
    }
    // UpdateSubresource of a watched buffer: `src` = its bytes [left, left + len)
    bool OnUpdate(const void* buf, const void* src, uint32_t left, uint32_t len, CopyFn copy) {
        Entry* e = Find(buf);
        return e && src && Upload(*e, src, left, len, copy);
    }
    // the resolve draw: `n` floats from `off` of the copy of `buf` (false = no fresh copy covers it; *grown = the offset was not
    // covered: it was new when the buffer was filled, the next fill covers it)
    bool Read(const void* buf, uint32_t off, uint64_t frame, float* out, size_t n, bool* grown = nullptr) {
        if (grown) *grown = false;
        Entry* e = Find(buf);
        if (!e || !e->copies) return false;
        const uint64_t bytes = (uint64_t)n * 4u;
        const Seg* sg = nullptr;
        for (int i = 0; i < e->nSeg; ++i)
            if (off >= e->seg[i].lo && (uint64_t)off + bytes <= (uint64_t)e->seg[i].lo + e->seg[i].len) { sg = &e->seg[i]; break; }
        if (!sg) { if (grown) *grown = true; return false; }
        const bool fresh = e->copies != e->readCopies || frame == e->readFrame;
        if (!fresh) return false;
        e->readCopies = e->copies; e->readFrame = frame;
        const unsigned char* s = e->data + sg->at + (off - sg->lo);
        for (size_t i = 0; i < n; ++i) { float f; unsigned char* d = (unsigned char*)&f; for (int k = 0; k < 4; ++k) d[k] = s[4 * i + k]; out[i] = f; }
        return true;
    }
    uint32_t Half() const { return Span() / 2u; }        // (log: half the bytes copied per upload)
    // the bytes one upload copies now (the merged clusters of the recent offsets; 2 x kPad before any)
    uint32_t Span() const {
        Seg s[kMaxSeg];
        int n = 0;
        uint32_t total = 0;
        Segments(m_width ? m_width : 0xFFFFFFFFu, s, &n);
        for (int i = 0; i < n; ++i) total += s[i].len;
        return n ? total : 2u * kPad;
    }
    int  Clusters() const { Seg s[kMaxSeg]; int n = 0; Segments(m_width ? m_width : 0xFFFFFFFFu, s, &n); return n; }
    uint64_t Uploads() const { return m_uploads; }
    uint64_t Watches() const { return m_watches; }
    void Reset() { for (Entry& e : m_e) { delete[] e.data; e = Entry{}; } m_width = 0; m_histN = 0; }
    ~CbShadow() { for (Entry& e : m_e) delete[] e.data; }
    CbShadow() = default;
    CbShadow(const CbShadow&) = delete;
    CbShadow& operator=(const CbShadow&) = delete;

private:
    struct Seg { uint32_t lo = 0, len = 0, at = 0; };   // bytes [lo, lo + len) of the buffer, at `at` in the copy
    struct Entry {
        const void*    buf = nullptr;
        uint32_t       width = 0;
        const void*    mapPtr = nullptr;
        unsigned char* data = nullptr;   // the segments' copies, back to back (capacity cap bytes)
        uint32_t       cap = 0;
        Seg            seg[kMaxSeg];
        int            nSeg = 0;
        uint64_t       copies = 0, readCopies = 0, readFrame = ~0ull, use = 0;
    };
    Entry* Find(const void* buf) { for (Entry& e : m_e) if (e.buf && e.buf == buf) return &e; return nullptr; }
    const Entry* FindC(const void* buf) const { for (const Entry& e : m_e) if (e.buf && e.buf == buf) return &e; return nullptr; }
    Entry* Lru() {
        Entry* l = &m_e[0];
        for (Entry& e : m_e) { if (!e.buf) { delete[] e.data; return &e; } if (e.use < l->use) l = &e; }
        delete[] l->data;
        return l;
    }
    // the merged clusters of the recent offsets inside [0, width): each offset -> [off - kPad, off + 256 + kPad), circular (the part
    // past the end continues at 0, the part before 0 at the end), newest offsets first until kMaxSpan, sorted by lo
    void Segments(uint32_t width, Seg* out, int* n) const {
        *n = 0;
        const int have = m_histN < (uint64_t)kHist ? (int)m_histN : kHist;
        if (!have || width < 256u) return;
        Seg iv[kHist * 2];
        int ni = 0;
        uint64_t total = 0;
        for (int k = 0; k < have; ++k) {                  // newest first
            const uint32_t off = m_recent[(m_histN - 1 - (uint64_t)k) % kHist];
            if (off >= width) continue;
            const uint64_t span = 256u + 2u * (uint64_t)kPad;
            const int64_t lo = (int64_t)off - (int64_t)kPad;
            Seg a[2];
            int na = 0;
            if (span >= width) { a[na++] = Seg{ 0, width, 0 }; }
            else if (lo < 0) {
                a[na++] = Seg{ 0, (uint32_t)(lo + (int64_t)span), 0 };
                a[na++] = Seg{ (uint32_t)((int64_t)width + lo), (uint32_t)(-lo), 0 };
            } else if ((uint64_t)lo + span > width) {
                a[na++] = Seg{ (uint32_t)lo, width - (uint32_t)lo, 0 };
                a[na++] = Seg{ 0, (uint32_t)((uint64_t)lo + span - width), 0 };
            } else {
                a[na++] = Seg{ (uint32_t)lo, (uint32_t)span, 0 };
            }
            // an offset already inside the kept intervals adds nothing; a new one is kept while the total stays within kMaxSpan
            bool inside = false;
            for (int i = 0; i < ni && !inside; ++i) inside = off >= iv[i].lo && (uint64_t)off + 256u <= (uint64_t)iv[i].lo + iv[i].len;
            if (inside) continue;
            uint64_t add = 0;
            for (int j = 0; j < na; ++j) add += a[j].len;
            if (ni && total + add > kMaxSpan) continue;
            for (int j = 0; j < na; ++j) iv[ni++] = a[j];
            total += add;
        }
        for (int i = 1; i < ni; ++i)                      // sort by lo (few entries: insertion sort)
            for (int j = i; j > 0 && iv[j].lo < iv[j - 1].lo; --j) { const Seg t = iv[j]; iv[j] = iv[j - 1]; iv[j - 1] = t; }
        for (int i = 0; i < ni; ++i) {                    // merge overlapping / touching
            if (*n && iv[i].lo <= out[*n - 1].lo + out[*n - 1].len) {
                const uint64_t e = (uint64_t)iv[i].lo + iv[i].len, pe = (uint64_t)out[*n - 1].lo + out[*n - 1].len;
                if (e > pe) out[*n - 1].len = (uint32_t)(e - out[*n - 1].lo);
            } else if (*n < kMaxSeg) {
                out[(*n)++] = iv[i];
            }
        }
        for (int i = 0; i < *n; ++i) {                    // 16-byte aligned starts (the streaming copy)
            const uint32_t a = out[i].lo & 15u;
            out[i].lo -= a; out[i].len += a;
        }
    }
    bool Upload(Entry& e, const void* src, uint32_t srcLeft, uint32_t srcLen, CopyFn copy) {
        const uint32_t width = e.width ? e.width : srcLeft + srcLen;
        Seg s[kMaxSeg];
        int n = 0;
        Segments(width, s, &n);
        if (!n) return false;
        uint32_t need = 0;
        for (int i = 0; i < n; ++i) {                     // clip to the uploaded bytes
            uint64_t lo = s[i].lo, hi = (uint64_t)s[i].lo + s[i].len;
            if (lo < srcLeft) lo = srcLeft;
            if (hi > (uint64_t)srcLeft + srcLen) hi = (uint64_t)srcLeft + srcLen;
            if (hi > width) hi = width;
            s[i].lo = (uint32_t)lo;
            s[i].len = hi > lo ? (uint32_t)(hi - lo) : 0u;
            s[i].at = need;
            need += (s[i].len + 15u) & ~15u;
        }
        if (!need) return false;
        if (e.cap < need) { delete[] e.data; e.cap = need + 64u * 1024u; e.data = new unsigned char[e.cap]; }
        e.nSeg = 0;
        for (int i = 0; i < n; ++i) {
            if (!s[i].len) continue;
            copy(e.data + s[i].at, static_cast<const unsigned char*>(src) + (s[i].lo - srcLeft), s[i].len);
            e.seg[e.nSeg++] = s[i];
        }
        ++e.copies; ++m_uploads;
        return true;
    }
    Entry    m_e[kBufs];
    uint32_t m_width = 0;
    uint32_t m_recent[kHist] = {};
    uint64_t m_histN = 0, m_tick = 0, m_uploads = 0, m_watches = 0;
};

}  // namespace GameTaa
