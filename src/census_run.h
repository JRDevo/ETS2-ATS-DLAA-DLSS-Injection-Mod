// census_run.h -- phase 108 "shader census", the D3D / hook side. PART OF src/inject.cpp: included ONCE, inside WITH_DLAA, right
// before hkDrawIndexedInstanced, and it uses inject.cpp's globals directly (g_lodPsCur, g_gbufPass, g_jitterPass, g_mirJit,
// t_inDlaa, oDrawIndexed, Cutout::CensusWhy, Cap::WritePng, PathNextToDll, the log). The pure parts are src/census.h.
//
// FLOW (render thread unless noted):
//   1. Create*Shader (any thread): CensusNotePs / CensusNoteVs -- one ShInfo per distinct bytecode checksum (the PS scanned once by
//      Census::ScanPs, the bytecode kept up to kCodeCap for the dumps), a lock-free pointer -> info table for the draw path.
//   2. Every DrawIndexed / DrawIndexedInstanced of a jittered world / mirror pass (G-buffer or forward, not a preview, not our
//      work): CensusBefore. FAST PATH = the PS pointer memo (one compare while the PS does not change) and one flag: a PS without
//      a structural kind / a known VS set / a thin pair returns at once (every kThinStride-th G-buffer draw goes on for the thin
//      sampling). SLOW PATH (interesting draws): the VS (hkVSSetShader's note when did_shadow hooked it, else VSGetShader), the
//      pair (memo, then an open table), the pair's counters, the first-sight state text, the verdict, a sampled occlusion query.
//   3. After the game's draw: the query's End; at the end of the hook (game state back): the TINT re-draw when the census view is
//      on (phase 109r: ONLY then; with a dump picture pending, its pair alone) -- the same draw with the census PS (one colour from cb13), RGB-only alpha blend on
//      RT0 for forward draws, RT2 = the albedo for G-buffer draws (109r; the other targets masked), the game's depth-stencil state as a twin without depth / stencil writes (EQUAL when the game wrote
//      depth). Flat and VR, G-buffer (the tinted albedo is then lit) and forward. View off = no re-draw, no state reads.
//   4. Present (CensusOnPresent): query results -> pixels per pair -> the thin verdict; dump grabs (staging copies made at a draw,
//      mapped here with DO_NOT_WAIT, written by the census writer thread, unmapped when it is done); the picture requests; every
//      kWriteSecs the census file (a snapshot of the suspect pairs is formatted + written by the writer thread), and kSoonSecs
//      after a NEW suspect. Phase 109r: no exit write (the loader lock after the other threads were killed: not safe).
// COST: view off, per draw of the passes above ~3-5 ns (memo + flag); per interesting draw ~40-80 ns (pair memo / lookup + counters;
// + VSGetShader without did_shadow); <= kQueriesPerPresent occlusion queries. Measured by the slow path / Present timers in the
// periodic LogDbg line (the fast path is counted, not timed).
// PHASE 113b: a pair under the px-per-triangle rule is only a thin CANDIDATE; its draws are JITTER-PROBED (census_probe.h: after the
// game's draw, the draw re-issued depth-only at the 8 jitter offsets into the mod's stencil; <= kProbesPerPresent per Present, 2 of
// the occlusion queries each) and only a pair the probes confirm is thin -- and only its draws whose own probe was thin are tinted.
// PHASE 115: step 3 in the main world passes writes the census MASK instead (MaskTint: an R32_UINT target per pass slot, the kind +
// the draw's depth per pixel), and every eye / screen blit PAINTS it into the blit source after DLAA (PaintAtBlit, census_paint.h): a
// checkerboard of the kind's screen colour and black, swapped 4 times a second -- DLAA never sees it. Mirrors keep the in-place tint.
// A pair a double-Insert capture saved (Pair::capSession) is not highlighted after that capture (this session only); the bug icon's
// counts (g_bugToCapture / g_bugCaptured) are taken at Present.
// PHASE 116: a draw whose ONLY tint kinds a switched-off fix would cover (Pair::fixOff, Census::ShownKinds) is not tinted, not
// counted "to capture" and not captured by the bug report's census part; the bug icon counts those pairs (g_bugFixOff, "N: fix is
// off") and the legend says such objects are not coloured. dlaa_census.txt still lists them, with a "fix off" note.
// PHASE 110b: a cut-out pair is judged over the textures it really draws (CutCheck: view on = every draw of the pair, else 1 in
// kCutStride; a view-pointer memo, kSrvRecheck Presents) and its draw is tinted orange only when ITS texture is BAD; the fixes count
// only while ON (FixMaskNow, re-judged at Present when one is switched); the tint is capped at kTintMaxPerFrame re-draws per
// Present and its GPU time is sampled (TsTick: one frame in kTsEvery, up to kTsMax timestamp pairs) for the periodic line.
#pragma once
// (no #include here: this file is read inside inject.cpp's anonymous namespace -- <map>, <array>, <thread> are at inject.cpp's top)

int  g_censusCfg = 0;            // dlaa.ini census 0/1 (default 0 since phase 109r: a dev tool). Phase 110: LIVE = the menu row "Show
                                 //   unfixed shaders" / key_census_view (1 in dlaa.ini = on from the start); Save writes it, Reset removes it
int  g_censusDumpCfg = 0;        // dlaa.ini census_dump 0/1 (default 0 since phase 109r), startup only (capped: kDumpMax dumps, census_max_mb)
int  g_censusMaxMb = 500;        // dlaa.ini census_max_mb 16..4096 (default 500): the dumps' disk budget for this session
bool g_censusOn = false;         // census live (row on && ready && the device is the game's); the draw hooks' gate
bool g_censusReady = false;      // phase 110: CensusStart ran (the PS hooks are in): the row can switch it on any time

namespace CensusRun {
using namespace Census;

constexpr int      kObjMax = 32768;          // shader OBJECTS (pointer table; power of two)
constexpr int      kInfoMax = 16384;         // distinct bytecodes (VS + PS)
constexpr size_t   kCodeCap = 96ull << 20;   // bytecode kept for the dumps
constexpr size_t   kCodeCapIdle = 24ull << 20;   // phase 110: ... while the census was never on: every VS / plain PS below this (an
                                                 //   interesting PS always, up to kCodeCap), so a later switch-on can still dump them
constexpr int      kCapArmFrames = 3;            // phase 110: the capture dumps every suspect pair drawn in this many frames after the key
constexpr int      kCapMaxPairs = 48;            //   full dumps per capture (more: text + picture only)
constexpr int      kCapMaxPictures = 16;         //   per-pair pictures per capture (each ~kHiFrames Presents with the pair alone tinted)
constexpr int      kCapTexRetry = 90;            //   Presents after the key a texture refused by the in-flight cap is grabbed again
constexpr uint64_t kCapStartWait = 120;          // phase 112r: Presents after the key the capture itself may take to start
constexpr uint64_t kCapDeadline = 3600;          //   Presents after the key the census part gives up waiting (the bug report goes on)
constexpr int      kPairMax = 4096;
constexpr int      kThinStride = 16;         // every 16th G-buffer draw of an uninteresting PS joins the thin sampling
constexpr int      kQueriesPerPresent = 8;
constexpr int      kQueryRing = 512;          // phase 110: 128 -> 512 (a capture queries every suspect draw of its armed frames)
constexpr int      kSamplesSuspect = 32, kSamplesThin = 8;
constexpr int      kProbesPerPresent = 2;     // phase 113b: jitter probes per Present (2 occlusion queries each, out of the 8 above;
                                              //   reserved only while a draw waited for one in the last Present)
constexpr int      kProbeRing = 64;           // phase 113b: probes in flight (results read at Present, ~2 frames later)
constexpr double   kWriteSecs = 60.0;
constexpr int      kDumpMax = 200;
constexpr double   kDumpGapSecs = 2.0;
constexpr uint64_t kDumpTexCap = 32ull << 20;      // one texture
constexpr uint64_t kDumpPairCap = 64ull << 20;     // one dump's textures + buffers
constexpr uint64_t kInFlightCap = 192ull << 20;    // staging copies waiting for their read
constexpr int      kHiFrames = 8;                  // the dump picture: the pair tinted this many Presents (DLSS history), then read
constexpr uint32_t kCutStride = 8;                 // phase 110b: view off, every 8th draw of a cut-out pair checks its texture
constexpr int      kTintMaxPerFrame = 4000;        // phase 110b: safety cap on tint re-draws per Present (counted when it bites)
constexpr int      kTsMax = 256;                   // phase 110b: tint re-draws timed in one sampled frame (timestamp pairs)
constexpr uint64_t kTsEvery = 120;                 //   one sampled frame every this many Presents (the census GPU cost line)

// ---- shader infos -------------------------------------------------------------------------------------------------------------
struct ShInfo {
    uint32_t sum[4] = {};
    bool     ps = false;
    PsScan   scan;
    int      waterRule = 0;
    bool     prFix = false;                    // phase 109a: PixelReader::Make made a twin (the pixel-reader read is fixed)
    bool     gameWirePs = false;               // phase 109b: the power-line forward PS 69dcaf36
    int      wirePs = 0;                       // WireWiden::PsKind
    int      solidPs = 0;                      // 1..5 by checksum
    bool     interest = false;                 // PS: structural kind or a known VS set
    std::atomic<bool> thinPair{false};         // PS: a pair with it passed the thin pre-filter (render thread; 113b: its draws are probed)
    int      vsLayout = 0;                     // VS (phase 115): Census::VsPosLayout -- which census mask PS links to it (0 = PSMask0)
    std::vector<uint8_t> code;                 // immutable once published (empty past kCodeCap)
};
ShInfo*               g_info[kInfoMax] = {};
std::atomic<int>      g_infoN{0};
std::map<std::array<uint32_t, 5>, int> g_infoBySum;   // (sum, ps) -> index; g_lk
std::atomic<void*>    g_objKey[kObjMax];
std::atomic<int>      g_objInfo[kObjMax];
std::atomic<uint32_t> g_objGen{0};
std::atomic<int>      g_objN{0};
std::atomic<uint64_t> g_codeBytes{0};
std::atomic<uint32_t> g_objFull{0}, g_psNoted{0}, g_vsNoted{0};
std::atomic<bool>     g_codeAll{false};          // phase 110: the census was on once (or census = 1 at start): keep every bytecode
std::atomic<uint32_t> g_codeSkipped{0};          // bytecodes not kept (idle cap / kCodeCap)
SRWLOCK               g_lk = SRWLOCK_INIT;

inline uint32_t ObjHash(const void* p) {
    return (uint32_t)((((uint64_t)(uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull) >> 49) & (kObjMax - 1);
}
inline int LookupObj(const void* p) {
    if (!p) return -1;
    for (int k = 0, i = (int)ObjHash(p); k < kObjMax; ++k, i = (i + 1) & (kObjMax - 1)) {
        void* const q = g_objKey[i].load(std::memory_order_acquire);
        if (q == p) return g_objInfo[i].load(std::memory_order_acquire);
        if (!q) return -1;
    }
    return -1;
}
inline int SolidPsByCode(const void* code, size_t len) { return WireWiden::SolidPsKindOfCode(code, len); }   // (109d / e: kinds F .. H)
// phase 112r (perf audit): `sumIn` = the DXBC checksum the caller already has (else computed here); `prWhy` = PrNotePs's
// PixelReader::Make result for this bytecode (-1 = not made: made here when the scan needs it). The ShInfo of a NEW bytecode
// (ScanPs, the water classifier, ...) is built OUTSIDE the exclusive lock (other threads' creates and the draw path's LookupObj do not
// wait for it); a racing twin of the same bytecode is dropped. Returns the PS's discard-alpha t-slots (Cutout's slot mask; 0 = none
// known / a VS).
uint32_t Note(void* obj, const void* code, size_t len, bool ps, const uint32_t* sumIn = nullptr, int prWhy = -1) {
    if (!obj || !code || len < 32) return 0;
    uint32_t sum[4];
    if (sumIn) memcpy(sum, sumIn, 16);
    else if (!Checksum(code, len, sum)) return 0;
    const std::array<uint32_t, 5> key = { sum[0], sum[1], sum[2], sum[3], ps ? 1u : 0u };
    int idx = -1;
    AcquireSRWLockShared(&g_lk);
    {
        auto it = g_infoBySum.find(key);
        if (it != g_infoBySum.end()) idx = it->second;
    }
    ReleaseSRWLockShared(&g_lk);
    ShInfo* fresh = nullptr;
    if (idx < 0 && g_infoN.load(std::memory_order_relaxed) < kInfoMax) {
        fresh = new (std::nothrow) ShInfo();
        if (fresh) {
            ShInfo* const s = fresh;
            memcpy(s->sum, sum, 16);
            s->ps = ps;
            if (ps) {
                s->scan = ScanPs(code, len);
                s->waterRule = WaterPs::Classify(code, len).rule;
                s->prFix = (s->scan.kinds & kPixel) &&
                           (prWhy >= 0 ? prWhy : (int)PixelReader::Make(code, len).why) == (int)PixelReader::kOk;   // phase 109a
                s->wirePs = WireWiden::PsKind(code, len);
                s->solidPs = SolidPsByCode(code, len);
                s->gameWirePs = WireWiden::ChecksumIs(code, len, WireWiden::kGameWirePs);   // phase 109b
                s->interest = s->scan.kinds != 0 || s->wirePs != 0 || s->solidPs != 0 || s->gameWirePs;
            } else s->vsLayout = VsPosLayout(code, len);    // phase 115 (the census mask PS that links to it)
        }
    }
    AcquireSRWLockExclusive(&g_lk);
    if (idx < 0) {
        auto it = g_infoBySum.find(key);
        if (it != g_infoBySum.end()) idx = it->second;   // (another thread noted the same bytecode meanwhile: ours is dropped)
        else if (fresh && g_infoN.load(std::memory_order_relaxed) < kInfoMax) {
            ShInfo* const s = fresh;
            fresh = nullptr;
            const size_t had = g_codeBytes.load(std::memory_order_relaxed);
            const bool keep = had + len <= kCodeCap &&
                              (g_codeAll.load(std::memory_order_relaxed) || (ps && s->interest) || had + len <= kCodeCapIdle);
            if (keep) {
                s->code.assign(static_cast<const uint8_t*>(code), static_cast<const uint8_t*>(code) + len);
                g_codeBytes.fetch_add(len, std::memory_order_relaxed);
            } else g_codeSkipped.fetch_add(1, std::memory_order_relaxed);
            idx = g_infoN.load(std::memory_order_relaxed);
            g_info[idx] = s;
            g_infoN.store(idx + 1, std::memory_order_release);
            g_infoBySum.emplace(key, idx);
        }
    }
    if (idx >= 0) {
        int at = -1, empty = -1;
        for (int k = 0, i = (int)ObjHash(obj); k < kObjMax; ++k, i = (i + 1) & (kObjMax - 1)) {
            void* const q = g_objKey[i].load(std::memory_order_relaxed);
            if (q == obj) { at = i; break; }
            if (!q) { empty = i; break; }
        }
        if (at >= 0) g_objInfo[at].store(idx, std::memory_order_release);   // a re-created object at a released one's address
        else if (empty >= 0 && g_objN.load(std::memory_order_relaxed) < kObjMax * 3 / 4) {
            g_objInfo[empty].store(idx, std::memory_order_relaxed);
            g_objKey[empty].store(obj, std::memory_order_release);
            g_objN.fetch_add(1, std::memory_order_relaxed);
        } else g_objFull.fetch_add(1, std::memory_order_relaxed);
        g_objGen.fetch_add(1, std::memory_order_release);
    }
    const uint32_t mask = (idx >= 0 && ps) ? g_info[idx]->scan.discardTex : 0u;
    ReleaseSRWLockExclusive(&g_lk);
    delete fresh;                                        // (a racing twin, or the table filled meanwhile)
    (ps ? g_psNoted : g_vsNoted).fetch_add(1, std::memory_order_relaxed);
    return mask;
}

// ---- pairs (render thread) ------------------------------------------------------------------------------------------------------
struct Pair {
    int      vs = -1, ps = -1;               // info indices (vs -1 = a VS created before the hooks)
    uint32_t structural = 0;
    Cover    cover;
    Verdict  v;
    uint32_t fixOff = 0;                     // phase 113: suspect kinds a fix of the mod would cover if switched on (FixOffKinds)
    bool     thin = false;                   // phase 113b: CONFIRMED thin (the pre-filter AND the jitter probe) -- Judge's `thin`
    bool     thinPre = false;                // phase 113b: the px-per-triangle pre-filter alone (a candidate: its draws are probed)
    CensusProbe::PairProbe probe;            // phase 113b: the jitter probes of its draws (census_probe.h)
    uint64_t probeAt = 0;                    //   Present + 1 of its last probe (one per pair per Present)
    bool     interest = false;               // drawn through the slow path every time (else only when sampled)
    uint64_t firstPresent = 0, lastPresent = 0, framesSeen = 0, draws = 0;
    double   firstSec = 0.0;
    SYSTEMTIME firstTime{};
    uint32_t icMin = 0xFFFFFFFFu, icMax = 0, instMax = 0;
    bool     inst = false, nonInst = false;
    uint8_t  passMask = 0;                   // 1 G-buffer, 2 forward, 4 mirror, 8 mirror G-buffer (phase 112r)
    uint8_t  posMask = 0;                    // phase 111: the world passes of a frame it was drawn in: bit 0 = the 1st, 1 = the 2nd
                                             //   (VR: the two eye passes), 2 = a later one (flat multi-view)
    float    vpW = 0, vpH = 0;
    uint64_t pixSum = 0, triSum = 0, pixSumAll = 0;   // (pixSum: phase 111, cut-out samples divided by their alpha coverage)
    uint32_t pixMax = 0;
    int      covSamples = 0;                 // phase 111: thin samples corrected by the alpha coverage
    int      samples = 0, samplesAll = 0;    // thin samples (tris >= min, px > 0) / every result
    bool     captured = false;               // (the first-sight state text is taken -- not the bug-report capture below)
    std::shared_ptr<const std::string> state;   // the first-sight draw state (immutable)
    int      vsLayout = 0;                   // phase 115: its VS's Census::VsPosLayout (the mask PS: 0 = no depth in the word)
    int      capSession = 0;                 // phase 115: the capture number that SAVED it this session (dump / link; 0 = never) --
                                             //   not highlighted any more once that capture has finished (Census::CapturedHidden)
    uint64_t shownAt = 0;                    // phase 115: Present + 1 it was last shown in the live view (the bug icon's count)
    uint64_t fixOffAt = 0;                   // phase 116: Present + 1 a draw of it was last NOT shown only because a fix is off
    uint32_t cutSlots = 0;
    CutTrack cut;                            // phase 110b: the textures it really draws (each checked draw), see census.h
    uint32_t cutTick = 0;                    //   view off: every kCutStride-th draw is checked (view on: every draw, the tint needs it)
    int      cutLast = -1;                   // phase 112r: the last per-draw answer (a busy phase-77 lock reuses it: both VR eyes alike)
    bool     logged = false;
    int      dump = 0;                       // 0 none, 1 queued, 2 started, 3 written, -1 refused (cap)
    char     dumpDir[48] = "";               // "<vs8>_<ps8>"
    std::string where;                       // phase 110: the folder of its last FULL dump (relative to the game folder), "" none
    std::vector<int> marks;
    // phase 110: the capture part (double Insert with the view on) -- valid while capNo == CapRun::no
    int      capNo = 0, capAction = 0, capDraws = 0, capQueries = 0;
    uint64_t capPx = 0, capPicPresent = 0;
    uint32_t capTexLeft = 0;                 // t-slots refused by the in-flight cap (grabbed at a later draw, kCapTexRetry)
    std::string capTex;                      // per-slot texture status lines (index.txt)
};
std::vector<Pair> g_pairs;
uint32_t          g_pairKey[kPairMax * 2] = {};   // (vs + 1) << 16 | (ps + 1); 0 = empty
int               g_pairIdx[kPairMax * 2] = {};
int               g_pairFull = 0;
inline uint32_t PairSlot(uint32_t k) { return (k * 2654435761u) >> 19 & (kPairMax * 2 - 1); }

// memos (render thread)
const void* m_ps = nullptr;
uint32_t    m_gen = ~0u;
int         m_psIdx = -1;
bool        m_psInterest = false;
const void* m_vsMemo = nullptr;
int         m_vsIdx = -1;
const void* m_pairVs = nullptr;
const void* m_pairPs = nullptr;
int         m_pairI = -1;
int         m_thinTick = 0;
ID3D11DeviceContext*  m_c1Owner = nullptr;   // phase 109r: the context m_c1 was queried from (compare only)
ID3D11DeviceContext1* m_c1 = nullptr;        //   its ID3D11DeviceContext1 (the tint's cb13 window save / restore; ref kept)
void*       g_vsCur = nullptr;              // hkVSSetShader's note (did_shadow hooked it); g_vsHooked tells
bool        g_vsValid = false;               // false after ClearState / ExecuteCommandList / SwapDeviceContextState: read it once
bool        g_vsHooked = false;
uint32_t    g_vsCheck = 0;

// statistics
uint64_t g_cFast = 0, g_cSlow = 0, g_cUnknownPs = 0, g_cTint = 0, g_cQueries = 0, g_cQueryDrop = 0;
uint64_t g_ticksSlow = 0, g_ticksPresent = 0, g_ticksTint = 0;
uint64_t g_presentsCounted = 0;
// phase 110b: tint cost -- re-draws per frame (cap) + the GPU time of the tint re-draws, sampled: in one frame every kTsEvery
// Presents, up to kTsMax re-draws get a timestamp before / after (one disjoint query around the frame); the sum is scaled up by
// (re-draws in that frame / timed ones). An estimate: a timestamp pair brackets the GPU time from the previous command's end.
int      g_tintThisFrame = 0;
// phase 111 (VR): the cap is counted per WORLD PASS -- flat: one pass per Present = the old per-Present cap; VR: each eye pass gets
// kTintMaxPerFrame / 2, so a cap that bites cuts the SAME draws in both eyes (a per-Present count bit in the second eye only: an
// eye-only tint = binocular flicker). A mirror pass shares the pass it follows.
int      g_tintThisPass = 0;
uint64_t g_tintPassSeq = ~0ull;
inline int TintCapPerPass() { return g_vrMode ? kTintMaxPerFrame / 2 : kTintMaxPerFrame; }
uint64_t g_cTintCapped = 0;
struct TsFrame {
    ID3D11Query* dis = nullptr;
    ID3D11Query* b[kTsMax] = {};
    ID3D11Query* e[kTsMax] = {};
    bool     ok = false, tried = false;
    int      state = 0;                            // 0 idle, 1 armed (this frame's tints are timed), 2 waiting for the results
    int      timed = 0, tints = 0;
    uint64_t at = 0;
} g_ts;
double   g_tsMsSum = 0.0;                          // estimated tint GPU ms, summed over the sampled frames
int      g_tsFrames = 0, g_tsDisjoint = 0;
double   g_tsLastMs = -1.0;
LARGE_INTEGER g_t0{};
double g_qpf = 0.0;
inline double NowSec() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return g_qpf > 0 ? (double)(t.QuadPart - g_t0.QuadPart) / g_qpf : 0.0; }

// view / highlight / legend
std::atomic<bool> g_view{false};
int      g_hiPair = -1;                      // the dump picture's pair (tinted alone while g_hiLeft > 0)
int      g_hiLeft = 0;
double   g_legendUntil = -1.0;                 // phase 112b: the colour legend box shows until then (kLegendSecs after switch-on)
double   g_markNoteUntil = -1.0;               // phase 112b: the status box's mark note until then (was g_legendUntil: one timer
                                               //   for both, so a mark cut the legend short)
constexpr double kLegendSecs = 15.0;           // phase 112b: was 10 s (two lines); the legend now has 8
double   g_fixOffLineUntil = -1.0;             // phase 116 (audit S2): the legend's "fix is off" line is held until then -- latched so
                                               //   it does not flicker frame-to-frame with g_bugFixOff; reset when the view goes off
double   g_nextWrite = 20.0;                 // the next census-file write (NowSec)
constexpr double kSoonSecs = 5.0;            // phase 109r: a NEW suspect brings the next write this close (no exit write any more)
char     g_legendMsg[96] = "";

// device objects
ID3D11Device*            g_dev = nullptr;    // identity of the game's device (first use)
bool                     g_devBad = false;
ID3D11PixelShader*       g_tintPs = nullptr;     // forward draws: SV_Target0 (the HDR colour)
ID3D11PixelShader*       g_tintPsG = nullptr;    // phase 109r: G-buffer draws: SV_Target2 (o2 = albedo; o0 normal, o1 specular)
bool                     g_tintTried = false;
ID3D11BlendState*        g_tintBs = nullptr;     // RT0 RGB only
ID3D11BlendState*        g_tintBsG = nullptr;    // phase 109r: RT2 RGB only
ID3D11Buffer*            g_tintCb[kKindN][2] = {};   // [kind][0 G-buffer albedo, 1 forward HDR]
struct DssTwin { ID3D11DepthStencilState* game; ID3D11DepthStencilState* twin; bool equal;
                 int mode; };                     // phase 115r (B8): Census::MaskMode of a draw with this state (kMaskModeEqual ...)
DssTwin                  g_dss[64] = {};
int                      g_dssN = 0;
// phase 115: the census MASK of each pass slot (src/census_paint.h): R32_UINT at the pass's depth size, cleared at the pass's first
// suspect draw, painted at its blit. One per FIFO slot (a pass and its blit are apart: VR draws both eyes' passes before their blits).
struct MaskSlot {
    ID3D11Texture2D*          tex = nullptr;
    ID3D11RenderTargetView*   rtv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    UINT                      w = 0, h = 0;
    uint64_t                  pass = ~0ull;        // the pass it holds (cleared for), ~0 = none
    bool                      any = false;         // a suspect draw wrote into it
};
MaskSlot                 g_mask[kRing];
ID3D11PixelShader*       g_maskPs[3] = {};          // PSMask0 (no inputs), PSMask1 (SV_Position v0), PSMask2 (COLOR0 v0 + SV_Position v1)
ID3D11Buffer*            g_maskCb[kKindN][4] = {};  // uint4(kind + 1, Census::MaskMode (115r: + the test modes), 0, 0)
bool                     g_maskTried = false, g_maskOk = false;
CensusPaint::Kit         g_paint;
int                      g_paintPhase = 0;          // Census::PaintPhase, taken once per Present (both VR eyes of a frame alike)
uint64_t                 g_cMaskDraws = 0, g_cMaskSkipSize = 0, g_cMaskFail = 0, g_cPaints = 0, g_cPaintFail = 0;
uint64_t                 g_cMaskLayout[3] = {};     // mask draws per mask PS (0 = no depth in the word)
uint64_t                 g_ticksPaint = 0;
// phase 115: the bug icon -- what is left to capture here (distinct suspect pairs shown in the last ~1 s) and what was captured
std::atomic<int>         g_bugToCapture{0}, g_bugCaptured{0};
std::atomic<int>         g_bugFixOff{0};            // phase 116: distinct pairs hidden in the last ~1 s only because a fix is off
double                   g_bugNext = 0.0;           // the next count (kBugEvery: a few times a second at most)
constexpr double         kBugEvery = 0.5;           // phase 115r (P5): 2 Hz (was 4): each change = a GDI rebuild (VR: the fps box's)
constexpr double         kBugWindowSecs = 1.0;
double                   g_presentSec[256] = {};    // NowSec at Present n (n & 255): the count's 1 s window in Presents
struct Query { ID3D11Query* q = nullptr; int pair = -1; uint32_t tris = 0; uint64_t issued = 0; bool busy = false; bool cap = false; int capNo = 0;
               float cov = -1.0f;             // phase 111: the drawn alpha texture's coverage at the cut (-1 unknown / no cut-out)
               int probe = -1; int role = 0; };   // phase 113b: a jitter probe's query (g_pr index; role 1 = the 8-pass sum, 2 = all 8)
Query                    g_q[kQueryRing];
int                      g_qNext = 0, g_qBudget = kQueriesPerPresent;
// phase 113b: the jitter probes (census_probe.h) -- one record per probe in flight, the per-draw verdicts, the stencil buffers
struct ProbeRec { bool busy = false; int pair = -1; int slot = -1; uint64_t key = 0; int qSum = -1, qAll = -1; int got = 0; bool lost = false;
                  CensusProbe::Result r; };
ProbeRec                 g_pr[kProbeRing];
int                      g_prNext = 0;
CensusProbe::KeyTable*   g_keys = nullptr;      // (allocated at the first probe-able draw: 16384 x 32 bytes)
CensusProbe::Kit         g_kit[2];              // render-size stencil buffers (VR eyes of two sizes: one each)
int                      g_probeLeft = 0;       // probes left this Present
bool                     g_probeWant = false;   // a draw wanted a probe this Present (the next one reserves kProbesPerPresent)
uint64_t                 g_cProbes = 0, g_cProbeFail = 0, g_cProbeLost = 0, g_ticksProbe = 0;

// ---- the writer thread (files only; never a D3D call) ---------------------------------------------------------------------------
std::mutex                         g_wMx;
std::condition_variable            g_wCv;
std::deque<std::function<void()>>  g_wJobs;
bool                               g_wStarted = false;
std::atomic<int>                   g_wPending{0};
void WriterMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        std::function<void()> j;
        {
            std::unique_lock<std::mutex> lk(g_wMx);
            g_wCv.wait(lk, [] { return !g_wJobs.empty(); });
            j = std::move(g_wJobs.front());
            g_wJobs.pop_front();
        }
        try { j(); } catch (...) {}
        g_wPending.fetch_sub(1, std::memory_order_relaxed);
    }
}
bool Queue(std::function<void()> f) {
    std::lock_guard<std::mutex> lk(g_wMx);
    if (!g_wStarted) {
        try { std::thread(WriterMain).detach(); g_wStarted = true; } catch (...) { return false; }
    }
    if (g_wJobs.size() > 256) return false;
    g_wJobs.push_back(std::move(f));
    g_wPending.fetch_add(1, std::memory_order_relaxed);
    g_wCv.notify_one();
    return true;
}

// ---- paths ---------------------------------------------------------------------------------------------------------------------
bool DirPath(const wchar_t* sub, std::wstring& out) {      // dlaa_census\<sub>\ next to the DLL (created on the writer thread)
    wchar_t p[MAX_PATH];
    if (!PathNextToDll(L"dlaa_census\\", p)) return false;
    out = p;
    if (sub && *sub) { out += sub; out += L"\\"; }
    return true;
}
void MakeDirs(const std::wstring& d) {                    // every missing folder of the absolute path d (writer thread)
    // (phase 110: recursive -- a capture's census folder is dlaa_selftest\shimmer_<time>\census\<vs8>_<ps8>\)
    for (size_t i = 3; i < d.size(); ++i)
        if (d[i] == L'\\') CreateDirectoryW(d.substr(0, i).c_str(), nullptr);
    CreateDirectoryW(d.c_str(), nullptr);
}
bool WriteFileAll(const std::wstring& path, const void* data, size_t n) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    const bool ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

// ---- formats ---------------------------------------------------------------------------------------------------------------------
const char* FmtName(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_UINT: return "R16G16B16A16_UINT";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
    case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32_FLOAT_X8X24_TYPELESS";
    case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
    case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
    case DXGI_FORMAT_R16_UNORM: return "R16_UNORM";
    case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
    case DXGI_FORMAT_R8_UNORM: return "R8_UNORM";
    case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
    case DXGI_FORMAT_BC1_UNORM: return "BC1_UNORM";
    case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1_UNORM_SRGB";
    case DXGI_FORMAT_BC2_UNORM: return "BC2_UNORM";
    case DXGI_FORMAT_BC3_UNORM: return "BC3_UNORM";
    case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3_UNORM_SRGB";
    case DXGI_FORMAT_BC4_UNORM: return "BC4_UNORM";
    case DXGI_FORMAT_BC5_UNORM: return "BC5_UNORM";
    case DXGI_FORMAT_BC5_SNORM: return "BC5_SNORM";
    case DXGI_FORMAT_BC6H_UF16: return "BC6H_UF16";
    case DXGI_FORMAT_BC7_UNORM: return "BC7_UNORM";
    case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7_UNORM_SRGB";
    default: return nullptr;
    }
}
std::string FmtText(DXGI_FORMAT f) {
    if (const char* n = FmtName(f)) return n;
    char b[24]; snprintf(b, sizeof(b), "DXGI %d", (int)f); return b;
}
bool IsBc3(DXGI_FORMAT f) { return f == DXGI_FORMAT_BC3_UNORM || f == DXGI_FORMAT_BC3_UNORM_SRGB || f == DXGI_FORMAT_BC3_TYPELESS; }

// ---- device objects -------------------------------------------------------------------------------------------------------------
bool DevOk(ID3D11DeviceContext* ctx) {
    if (g_devBad) return false;
    if (g_dev) return true;
    ID3D11Device* d = nullptr;
    ctx->GetDevice(&d);
    if (!d) return false;
    g_dev = d;                                           // identity; one reference kept for the session (the game's device)
    return true;
}
// the tint blend: alpha blend of RGB on render target `rt` alone, every other target masked (phase 109r: rt 2 for G-buffer draws)
ID3D11BlendState* TintBlend(int rt) {
    D3D11_BLEND_DESC bd{};
    bd.IndependentBlendEnable = TRUE;
    for (int i = 0; i < 8; ++i) {
        D3D11_RENDER_TARGET_BLEND_DESC& r = bd.RenderTarget[i];
        if (i == rt) {
            r.BlendEnable = TRUE;
            r.SrcBlend = D3D11_BLEND_SRC_ALPHA; r.DestBlend = D3D11_BLEND_INV_SRC_ALPHA; r.BlendOp = D3D11_BLEND_OP_ADD;
            r.SrcBlendAlpha = D3D11_BLEND_ZERO; r.DestBlendAlpha = D3D11_BLEND_ONE; r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            r.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        } else {
            r.SrcBlend = D3D11_BLEND_ONE; r.DestBlend = D3D11_BLEND_ZERO; r.BlendOp = D3D11_BLEND_OP_ADD;
            r.SrcBlendAlpha = D3D11_BLEND_ONE; r.DestBlendAlpha = D3D11_BLEND_ZERO; r.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            r.RenderTargetWriteMask = 0;                 // (the tint PS writes only its one target: the others stay the game's)
        }
    }
    ID3D11BlendState* b = nullptr;
    return SUCCEEDED(g_dev->CreateBlendState(&bd, &b)) ? b : nullptr;
}
bool EnsureTint() {
    if (g_tintPs && g_tintBs && g_tintCb[0][0]) return true;
    if (g_tintTried || !g_dev) return false;
    if (!ShaderCache::Ready(ShaderCache::kCensusTintPs) || !ShaderCache::Ready(ShaderCache::kCensusTintGbufPs))
        return false;                                    // compiled later: retry at the next tinted draw
    g_tintTried = true;
    size_t sz = 0;
    const void* code = ShaderCache::Code(ShaderCache::kCensusTintPs, &sz);
    if (!code || FAILED(g_dev->CreatePixelShader(code, sz, nullptr, &g_tintPs))) {
        LogErr("census (phase 108): the tint pixel shader could not be created (%s) -- the census view stays empty",
               ShaderCache::Error(ShaderCache::kCensusTintPs));
        return false;
    }
    code = ShaderCache::Code(ShaderCache::kCensusTintGbufPs, &sz);
    if (!code || FAILED(g_dev->CreatePixelShader(code, sz, nullptr, &g_tintPsG))) {
        g_tintPsG = nullptr;
        LogWarn("census (phase 108): WARNING the G-buffer tint pixel shader could not be created (%s) -- G-buffer suspects stay "
                "untinted", ShaderCache::Error(ShaderCache::kCensusTintGbufPs));
    }
    g_tintBs = TintBlend(0);
    if (!g_tintBs) return false;
    if (g_tintPsG && !(g_tintBsG = TintBlend(2))) { g_tintPsG->Release(); g_tintPsG = nullptr; }
    for (int k = 0; k < kKindN; ++k)
        for (int h = 0; h < 2; ++h) {
            float c[3];
            KindColour(k, c);                            // (phase 115: screen colours -> linear for the in-place mirror tint)
            for (float& x : c) x = SrgbToLinear(x);
            const float s = h ? 2.0f : 1.0f;             // the forward pass is HDR (linear, before the exposure): brighter
            const float v[4] = { c[0] * s, c[1] * s, c[2] * s, 0.7f };
            D3D11_BUFFER_DESC b{};
            b.ByteWidth = 16; b.Usage = D3D11_USAGE_IMMUTABLE; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA sd{ v, 0, 0 };
            if (FAILED(g_dev->CreateBuffer(&b, &sd, &g_tintCb[k][h]))) return false;
        }
    Log("census (phase 108): tint objects created (PS + blend + %d colour buffers)", kKindN * 2);
    return true;
}
// equal (optional) = the twin tests EQUAL: the game's state wrote depth, so the draw's depth IS the buffer's (phase 115: the mask word
// carries it and the paint checks it against the pass's final depth)
ID3D11DepthStencilState* DssTwinOf(ID3D11DepthStencilState* game, bool* equal = nullptr, int* mode = nullptr) {
    for (int i = 0; i < g_dssN; ++i)
        if (g_dss[i].game == game) { if (equal) *equal = g_dss[i].equal; if (mode) *mode = g_dss[i].mode; return g_dss[i].twin; }
    D3D11_DEPTH_STENCIL_DESC d{};
    if (game) game->GetDesc(&d);
    else { d.DepthEnable = TRUE; d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; d.DepthFunc = D3D11_COMPARISON_LESS; }
    const bool eq = d.DepthEnable && d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
    const int md = MaskModeOf(d.DepthEnable != FALSE, eq, (int)d.DepthFunc);   // phase 115r (B8)
    if (mode) *mode = md;
    if (eq) d.DepthFunc = D3D11_COMPARISON_EQUAL;        // its own depth
    d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    d.StencilEnable = FALSE;
    ID3D11DepthStencilState* t = nullptr;
    if (FAILED(g_dev->CreateDepthStencilState(&d, &t))) return nullptr;
    if (equal) *equal = eq;
    if (g_dssN < 64) {
        if (game) game->AddRef();
        g_dss[g_dssN++] = { game, t, eq, md };
    } else {
        static ID3D11DepthStencilState* s_last = nullptr;   // full: the newest twin is kept alone (rare: 64 game states)
        if (s_last) s_last->Release();
        s_last = t;
    }
    return t;
}

// ---- the cut-out reason of a pair (render thread; at a draw of it) --------------------------------------------------------------
// one view's reason (-1 = the phase-77 lock was busy: no answer this time)
int CutReasonOfView(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* v, uint32_t s, std::string* tex) {
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (!r) return kCutNoTex;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    int why = kCutNoTex;
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
        // phase 114 (the 2026-11-15 evening VR captures): what "not BC3" / "shape rule" really were -- the ATS plate / sign atlas is a
        // 2048 x 2048 RGBA8 RENDER TARGET the game composes (the plates, the perforated wall band), the pennant flags an OPAQUE BC1,
        // and textures with ONE mip cannot lose coverage to their mips. Only a BC1 with see-through texels stays unhandled.
        if (d.BindFlags & D3D11_BIND_RENDER_TARGET) why = kCutRenderTarget;
        else if (d.MipLevels == 1 && d.ArraySize == 1) why = kCutOneMip;
        else if (Cutout::IsBc1Fmt(d.Format)) {
            why = Cutout::CensusWhy(r);                  // the uploader's BC1 check (kCutOpaque / kCutBc1Alpha), -1: g_mx busy
            if (why == kCutUnknown) why = Cutout::CensusLate(ctx, r, d, why);   // never seen: read back for the check (late path)
            else if (why != kCutOpaque && why != kCutBc1Alpha && why != kCutLate && why >= 0) why = kCutFormat;
        }
        else if (!IsBc3(d.Format)) why = kCutFormat;
        else {
            why = Cutout::CensusWhy(r);                  // (-1: g_mx busy)
            // phase 113: no phase-77 decision (or a full worker queue) -> its GPU read-back starts now (Cutout::CensusLate)
            if (why == kCutUnknown || why == kCutQueue) why = Cutout::CensusLate(ctx, r, d, why);
        }
        if (tex) { char b[96]; snprintf(b, sizeof(b), "t%u %s %ux%u", s, FmtText(d.Format).c_str(), d.Width, d.Height); *tex = b; }
    }
    r->Release();
    return why;
}
int CutReasonNow(ID3D11DeviceContext* ctx, uint32_t slots, std::string* tex) {
    if (!g_cutoutMips) return kCutOff;
    int best = kCutNoTex;
    for (uint32_t s = 0; s < 32; ++s) {
        if (!(slots & (1u << s))) continue;
        ID3D11ShaderResourceView* v = nullptr;
        ctx->PSGetShaderResources(s, 1, &v);
        if (!v) continue;
        int why = CutReasonOfView(ctx, v, s, tex);
        v->Release();
        if (why < 0) why = kCutPending;                  // (the lock was busy: asked again at a later draw)
        best = CutWorse(best, why);                      // one bad alpha slot makes the draw a suspect
    }
    return best;
}
// phase 110b: the reason of the texture THIS draw uses (render thread, every checked draw). A direct-mapped memo by view pointer keeps
// the answer kSrvRecheck Presents (then asks the phase-77 table again: a texture patched later turns OK within ~0.5 s); a busy
// lock reuses the memo or answers -1 (no verdict for this draw).
constexpr uint32_t kSrvWhyN = 2048;                      // power of two
constexpr uint64_t kSrvRecheck = 30;
struct SrvWhy { const void* srv; int why; uint64_t at; };
SrvWhy g_srvWhy[kSrvWhyN] = {};
uint64_t g_cCutChecks = 0, g_cCutMemo = 0;
int CutReasonDraw(ID3D11DeviceContext* ctx, uint32_t slots, uint64_t n) {
    if (!g_cutoutMips) return kCutOff;
    int best = kCutNoTex;
    for (uint32_t s = 0; s < 32; ++s) {
        if (!(slots & (1u << s))) continue;
        ID3D11ShaderResourceView* v = nullptr;
        ctx->PSGetShaderResources(s, 1, &v);
        if (!v) continue;
        uint64_t x = (uint64_t)(uintptr_t)v;
        x ^= x >> 29; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 32;
        SrvWhy& m = g_srvWhy[x & (kSrvWhyN - 1)];
        int why;
        ++g_cCutChecks;
        if (m.srv == v && n < m.at + kSrvRecheck) { why = m.why; ++g_cCutMemo; }
        else {
            why = CutReasonOfView(ctx, v, s, nullptr);
            if (why >= 0) { m.srv = v; m.why = why; m.at = n; }
            else if (m.srv == v) why = m.why;
        }
        v->Release();
        if (why < 0) return -1;
        best = CutWorse(best, why);
    }
    return best;
}

// phase 111: the alpha coverage at the cut of the texture(s) THIS draw uses in its discard slots (the lowest; -1 = none known:
// cutout_mips = 0, not BC3, not analysed yet, the lock busy). Only for the <= kQueriesPerPresent sampled thin queries per Present.
double CutCovDraw(ID3D11DeviceContext* ctx, uint32_t slots) {
    if (!g_cutoutMips || !slots) return -1.0;
    double best = -1.0;
    for (uint32_t s = 0; s < 32; ++s) {
        if (!(slots & (1u << s))) continue;
        ID3D11ShaderResourceView* v = nullptr;
        ctx->PSGetShaderResources(s, 1, &v);
        if (!v) continue;
        ID3D11Resource* r = nullptr;
        v->GetResource(&r);
        v->Release();
        if (!r) continue;
        double c = -1.0;
        Cutout::CensusWhy(r, &c);
        r->Release();
        if (c >= 0.0 && (best < 0.0 || c < best)) best = c;
    }
    return best;
}

// phase 111: the position of the current world pass in this frame (0 = the first world pass after the Present; VR: one per eye),
// capped to 2 = "later"; the eye it will be blitted to only when a certain source knows it (VrsDecideEye: blit, RT cycle, forward
// colour, frame pair; -1 = not certain). eye 0 = the RIGHT eye in this mod.
inline int CurPassPos() {
    if (!g_passSeq) return 0;
    const PassSlot& sl = g_ring[SlotOf(g_passSeq - 1)];
    const int pos = sl.s == g_passSeq - 1 ? sl.framePos : 0;
    return pos < 0 ? 0 : (pos > 2 ? 2 : pos);
}
int CurPassEye() {
    if (!g_vrMode || !g_passSeq) return -1;
    int src = 0, eyes[4] = { -1, -1, -1, -1 };
    bool dis = false;
    const int e = VrsDecideEye(g_passSeq - 1, nullptr, &src, eyes, &dis);
    return dis ? -1 : e;
}
inline const char* EyeName(int e) { return e == 0 ? "eye 0 = RIGHT eye" : (e == 1 ? "eye 1 = LEFT eye" : "eye not certain"); }

// ---- phase 110b: which fixes are ON now (the live switches) -> each pair's Cover ----------------------------------------------
uint32_t FixMaskNow() {
    return ((g_wireCfg && g_wireOn.load(std::memory_order_relaxed)) ? 1u : 0u) | (g_poleCfg ? 2u : 0u) | (g_powerLineCfg ? 4u : 0u) |
           ((g_prFixCfg && g_prFixBuild) ? 8u : 0u) | (g_waterFixCfg ? 16u : 0u) | (g_jitterOverscan ? 32u : 0u);   // (phase 112: the
                                                                                    //   twins exist; Picture fixes Off = all off)
}
uint32_t g_fixMask = 0xFFFFFFFFu;                        // the mask the pairs were judged with (~0 = not yet)
inline void ApplyFixes(Cover& c, uint32_t m) {
    c.wireOn = (m & 1u) != 0; c.poleOn = (m & 2u) != 0; c.powerLineOn = (m & 4u) != 0;
    c.prFixOn = (m & 8u) != 0; c.waterFixOn = (m & 16u) != 0; c.overscan = (m & 32u) != 0;
}

// ---- the first-sight state text (render thread, once per pair) ------------------------------------------------------------------
const char* BlendName(D3D11_BLEND b) {
    static const char* const k[] = { "?", "ZERO", "ONE", "SRC_COLOR", "INV_SRC_COLOR", "SRC_ALPHA", "INV_SRC_ALPHA", "DEST_ALPHA",
        "INV_DEST_ALPHA", "DEST_COLOR", "INV_DEST_COLOR", "SRC_ALPHA_SAT", "?", "?", "BLEND_FACTOR", "INV_BLEND_FACTOR", "SRC1_COLOR",
        "INV_SRC1_COLOR", "SRC1_ALPHA", "INV_SRC1_ALPHA" };
    return ((int)b >= 0 && (int)b < 20) ? k[(int)b] : "?";
}
const char* CmpName(D3D11_COMPARISON_FUNC f) {
    static const char* const k[] = { "?", "NEVER", "LESS", "EQUAL", "LESS_EQUAL", "GREATER", "NOT_EQUAL", "GREATER_EQUAL", "ALWAYS" };
    return ((int)f >= 0 && (int)f < 9) ? k[(int)f] : "?";
}
std::string StateText(ID3D11DeviceContext* ctx, const ShInfo* psi) {
    std::string s;
    char b[512];
    {
        ID3D11BlendState* bs = nullptr; FLOAT bf[4] = {}; UINT mask = 0;
        ctx->OMGetBlendState(&bs, bf, &mask);
        D3D11_BLEND_DESC d{};
        if (bs) { bs->GetDesc(&d); bs->Release(); }
        const auto& r = d.RenderTarget[0];
        if (!bs) snprintf(b, sizeof(b), "    blend: default (opaque)\n");
        else snprintf(b, sizeof(b), "    blend: RT0 %s %s * src + %s * dst (alpha %s / %s), write mask 0x%x, alpha-to-coverage %d, "
                      "independent %d\n", r.BlendEnable ? "ON" : "off", BlendName(r.SrcBlend), BlendName(r.DestBlend),
                      BlendName(r.SrcBlendAlpha), BlendName(r.DestBlendAlpha), (unsigned)r.RenderTargetWriteMask,
                      (int)d.AlphaToCoverageEnable, (int)d.IndependentBlendEnable);
        s += b;
    }
    {
        ID3D11DepthStencilState* ds = nullptr; UINT ref = 0;
        ctx->OMGetDepthStencilState(&ds, &ref);
        D3D11_DEPTH_STENCIL_DESC d{};
        if (ds) { ds->GetDesc(&d); ds->Release(); }
        snprintf(b, sizeof(b), "    depth: %s, write %s, func %s; stencil %s (ref %u, write mask 0x%02x)\n", d.DepthEnable ? "ON" : "off",
                 d.DepthWriteMask ? "ALL" : "off", CmpName(d.DepthFunc), d.StencilEnable ? "ON" : "off", ref,
                 (unsigned)d.StencilWriteMask);
        s += b;
    }
    {
        ID3D11RasterizerState* rs = nullptr;
        ctx->RSGetState(&rs);
        D3D11_RASTERIZER_DESC d{};
        if (rs) { rs->GetDesc(&d); rs->Release(); }
        D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ctx->IAGetPrimitiveTopology(&topo);
        snprintf(b, sizeof(b), "    raster: %s, cull %s, depth bias %d / slope %.4g / clamp %.4g, scissor %d, AA lines %d, topology %d\n",
                 rs ? (d.FillMode == D3D11_FILL_WIREFRAME ? "wireframe" : "solid") : "default",
                 rs ? (d.CullMode == D3D11_CULL_NONE ? "none" : (d.CullMode == D3D11_CULL_FRONT ? "front" : "back")) : "back",
                 d.DepthBias, (double)d.SlopeScaledDepthBias, (double)d.DepthBiasClamp, (int)d.ScissorEnable,
                 (int)d.AntialiasedLineEnable, (int)topo);
        s += b;
    }
    // the PS's declared textures + samplers
    const uint32_t decl = psi ? psi->scan.texDecl : 0xFFFFu;
    for (uint32_t t = 0; t < 32; ++t) {
        if (!(decl & (1u << t))) continue;
        ID3D11ShaderResourceView* v = nullptr;
        ctx->PSGetShaderResources(t, 1, &v);
        if (!v) { snprintf(b, sizeof(b), "    t%u: (nothing bound)\n", t); s += b; continue; }
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        v->GetDesc(&vd);
        ID3D11Resource* r = nullptr;
        v->GetResource(&r);
        v->Release();
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        if (r) r->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC d{};
            static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
            const char* role = (d.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) ? " (render target / depth: the frame's own)" : "";
            snprintf(b, sizeof(b), "    t%u: Texture2D %s %ux%u mips %u array %u%s, view %s%s%s%s\n", t, FmtText(d.Format).c_str(), d.Width,
                     d.Height, d.MipLevels, d.ArraySize, (d.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) ? " cube" : "",
                     FmtText(vd.Format).c_str(), role, (psi && (psi->scan.discardTex & (1u << t))) ? " [alpha of the discard]" : "",
                     (psi && (psi->scan.pixelTex & (1u << t))) ? " [read at the interpolated pixel]" : "");
        } else {
            snprintf(b, sizeof(b), "    t%u: resource dimension %d, view format %s\n", t, (int)dim, FmtText(vd.Format).c_str());
        }
        s += b;
        if (r) r->Release();
    }
    for (UINT k = 0; k < 8; ++k) {
        ID3D11SamplerState* sm = nullptr;
        ctx->PSGetSamplers(k, 1, &sm);
        if (!sm) continue;
        D3D11_SAMPLER_DESC d{};
        sm->GetDesc(&d);
        sm->Release();
        snprintf(b, sizeof(b), "    s%u: filter 0x%x, address %d/%d/%d, max aniso %u, mip LOD bias %.3f, LOD %.1f..%.1f, compare %s\n", k,
                 (unsigned)d.Filter, (int)d.AddressU, (int)d.AddressV, (int)d.AddressW, d.MaxAnisotropy, (double)d.MipLODBias,
                 (double)d.MinLOD, (double)(d.MaxLOD > 1e6f ? 1e6f : d.MaxLOD), CmpName(d.ComparisonFunc));
        s += b;
    }
    return s;
}

// ---- grabs: staging copies read back without a stall (render thread maps, the writer reads + writes) ---------------------------
struct Grab {
    ID3D11Resource* stg = nullptr;
    int      type = 0;                       // 0 texture -> DDS, 1 buffer -> .bin, 2 picture -> PNG
    std::wstring path;
    D3D11_TEXTURE2D_DESC td{};
    UINT     bytes = 0;                      // buffer size (type 1)
    uint64_t issued = 0;
    bool     mapped = false;
    std::vector<D3D11_MAPPED_SUBRESOURCE> maps;
    std::shared_ptr<std::atomic<int>> done;
    uint64_t cost = 0;                       // bytes counted in flight / on disk
    bool     cap = false;                    // phase 110: part of a capture's census folder (the capture waits for it)
};
std::vector<Grab> g_grabs;
bool     g_grabForCap = false;               // phase 110: grabs made now belong to the capture's census part
uint64_t g_inFlight = 0, g_diskBytes = 0;
int      g_dumpsStarted = 0, g_dumpsRefused = 0;
double   g_lastDumpSec = -1e9;
std::deque<int> g_dumpQueue;                 // pairs waiting for their dump (started at their next draw)
std::atomic<int> g_filesOk{0}, g_filesFail{0};
bool     g_picBusy = false;                  // one back-buffer picture in flight
struct PicReq { std::wstring path; uint64_t at; };
std::deque<PicReq> g_picReq;
ID3D11Texture2D* g_picStg = nullptr;
D3D11_TEXTURE2D_DESC g_picDesc{};

const char kWhyInFlight[] = "staging copies in flight full (192 MB)";
bool BudgetOk(uint64_t add) {
    return g_diskBytes + add <= (uint64_t)g_censusMaxMb << 20 && g_inFlight + add <= kInFlightCap;
}
// a Texture2D (not a render target, known format, <= kDumpTexCap) -> a staging copy; false = not grabbed (why in `why`)
bool GrabTexture(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const std::wstring& path, uint64_t n, uint64_t* spent,
                 const char** why) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    if (d.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) { *why = "render target / depth (the frame's own)"; return false; }
    if (d.SampleDesc.Count > 1) { *why = "MSAA"; return false; }
    const FmtInfo fi = FormatInfo((uint32_t)d.Format);
    if (!fi.bytes) { *why = "format not handled by the dump"; return false; }
    uint64_t bytes = 0;
    for (UINT m = 0; m < d.MipLevels; ++m) {
        uint32_t rb, rows;
        MipRows((uint32_t)d.Format, MipDim(d.Width, m), MipDim(d.Height, m), &rb, &rows);
        bytes += (uint64_t)rb * rows;
    }
    bytes *= d.ArraySize;
    if (bytes > kDumpTexCap) { *why = "over 32 MB"; return false; }
    if (*spent + bytes > kDumpPairCap) { *why = "over the 64 MB per dump"; return false; }
    if (g_diskBytes + bytes > (uint64_t)g_censusMaxMb << 20) { *why = "census_max_mb budget spent"; return false; }
    if (g_inFlight + bytes > kInFlightCap) { *why = kWhyInFlight; return false; }   // (phase 110: a capture retries it later)
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D* stg = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &stg)) || !stg) { *why = "staging create failed"; return false; }
    ctx->CopyResource(stg, tex);
    Grab g;
    g.stg = stg; g.type = 0; g.path = path; g.td = d; g.issued = n; g.cost = bytes; g.cap = g_grabForCap;
    g_grabs.push_back(std::move(g));
    g_inFlight += bytes;
    *spent += bytes;
    return true;
}
bool GrabBuffer(ID3D11DeviceContext* ctx, ID3D11Buffer* buf, UINT first, UINT num, const std::wstring& path, uint64_t n, uint64_t* spent) {
    D3D11_BUFFER_DESC d{};
    buf->GetDesc(&d);
    UINT off = first * 16, len = num * 16;
    if (!num) { off = 0; len = d.ByteWidth; }
    if (off >= d.ByteWidth) return false;
    if (off + len > d.ByteWidth) len = d.ByteWidth - off;
    if (len > 65536) len = 65536;
    if (!len || *spent + len > kDumpPairCap || !BudgetOk(len)) return false;
    D3D11_BUFFER_DESC sd{};
    sd.ByteWidth = len; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* stg = nullptr;
    if (FAILED(g_dev->CreateBuffer(&sd, nullptr, &stg)) || !stg) return false;
    const D3D11_BOX box{ off, 0, 0, off + len, 1, 1 };
    ctx->CopySubresourceRegion(stg, 0, 0, 0, 0, buf, 0, &box);
    Grab g;
    g.stg = stg; g.type = 1; g.path = path; g.bytes = len; g.issued = n; g.cost = len; g.cap = g_grabForCap;
    g_grabs.push_back(std::move(g));
    g_inFlight += len;
    *spent += len;
    return true;
}
// the writer's part of one grab (maps are valid until the render thread sees `done`)
void WriteGrab(int type, const std::wstring& path, const D3D11_TEXTURE2D_DESC& td, UINT bytes,
               const std::vector<D3D11_MAPPED_SUBRESOURCE>& maps) {
    bool ok = false;
    MakeDirs(path.substr(0, path.find_last_of(L'\\') + 1));   // dlaa_census\ + this folder (the writer thread, never the render thread)
    if (type == 0) {
        const bool cube = (td.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) != 0;
        std::vector<uint8_t> o = DdsHeader((uint32_t)td.Format, td.Width, td.Height, td.MipLevels, td.ArraySize, cube);
        for (UINT a = 0; a < td.ArraySize; ++a)
            for (UINT m = 0; m < td.MipLevels; ++m) {
                const UINT sub = m + a * td.MipLevels;
                if (sub >= maps.size()) break;
                uint32_t rb, rows;
                MipRows((uint32_t)td.Format, MipDim(td.Width, m), MipDim(td.Height, m), &rb, &rows);
                const uint8_t* p = static_cast<const uint8_t*>(maps[sub].pData);
                for (uint32_t y = 0; y < rows; ++y) o.insert(o.end(), p + (size_t)y * maps[sub].RowPitch, p + (size_t)y * maps[sub].RowPitch + rb);
            }
        ok = WriteFileAll(path, o.data(), o.size());
    } else if (type == 1) {
        ok = !maps.empty() && WriteFileAll(path, maps[0].pData, bytes);
    } else if (!maps.empty()) {
        // the presented picture -> RGBA8, halved while wider than 1920 (box filter), PNG
        UINT w = td.Width, h = td.Height;
        const uint8_t* src = static_cast<const uint8_t*>(maps[0].pData);
        const UINT pitch = maps[0].RowPitch;
        const DXGI_FORMAT f = td.Format;
        const bool bgra = Cap::IsBgra8(f), rgba = Cap::IsRgba8(f), ten = f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R10G10B10A2_TYPELESS;
        if (bgra || rgba || ten) {
            UINT s = 1;
            while (w / s > 1920 && s < 8) s *= 2;
            const UINT ow = w / s, oh = h / s;
            std::vector<uint8_t> o((size_t)ow * oh * 4);
            for (UINT y = 0; y < oh; ++y)
                for (UINT x = 0; x < ow; ++x) {
                    uint32_t acc[3] = {};
                    for (UINT yy = 0; yy < s; ++yy)
                        for (UINT xx = 0; xx < s; ++xx) {
                            const uint8_t* q = src + (size_t)(y * s + yy) * pitch + (size_t)(x * s + xx) * 4;
                            if (ten) {
                                uint32_t v; memcpy(&v, q, 4);
                                acc[0] += (v & 1023u) >> 2; acc[1] += ((v >> 10) & 1023u) >> 2; acc[2] += ((v >> 20) & 1023u) >> 2;
                            } else if (bgra) { acc[0] += q[2]; acc[1] += q[1]; acc[2] += q[0]; }
                            else { acc[0] += q[0]; acc[1] += q[1]; acc[2] += q[2]; }
                        }
                    uint8_t* d = &o[((size_t)y * ow + x) * 4];
                    for (int c = 0; c < 3; ++c) d[c] = (uint8_t)(acc[c] / (s * s));
                    d[3] = 255;
                }
            ok = Cap::WritePng(path.c_str(), o, ow, oh);
        }
    }
    (ok ? g_filesOk : g_filesFail).fetch_add(1, std::memory_order_relaxed);
}
// Present: map ready grabs (DO_NOT_WAIT) -> writer; unmap + release finished ones; drop stale ones
void PumpGrabs(ID3D11DeviceContext* ctx, uint64_t n) {
    for (size_t i = 0; i < g_grabs.size();) {
        Grab& g = g_grabs[i];
        if (g.mapped) {
            if (g.done && g.done->load(std::memory_order_acquire)) {
                for (UINT s = 0; s < (UINT)g.maps.size(); ++s) ctx->Unmap(g.stg, s);
                g.stg->Release();
                g_inFlight -= g.cost;
                g_grabs.erase(g_grabs.begin() + i);
                continue;
            }
            ++i;
            continue;
        }
        // phase 110: a capture makes hundreds of grabs at once and Queue refuses past 256 waiting jobs -- map only while the writer
        // has room (the rest waits mapped-not, in flight, for a later Present; never dropped for that)
        if (g_wPending.load(std::memory_order_relaxed) >= 192) { ++i; continue; }
        const UINT subs = g.type == 0 ? g.td.MipLevels * g.td.ArraySize : 1;
        std::vector<D3D11_MAPPED_SUBRESOURCE> maps(subs);
        UINT got = 0;
        for (; got < subs; ++got)
            if (ctx->Map(g.stg, got, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &maps[got]) != S_OK) break;
        if (got < subs) {
            for (UINT s = 0; s < got; ++s) ctx->Unmap(g.stg, s);
            if (n > g.issued + 600) {                     // never became ready: drop it
                g.stg->Release();
                g_inFlight -= g.cost;
                g_filesFail.fetch_add(1, std::memory_order_relaxed);
                g_grabs.erase(g_grabs.begin() + i);
                continue;
            }
            ++i;
            continue;
        }
        g.maps = maps;
        g.mapped = true;
        g.done = std::make_shared<std::atomic<int>>(0);
        auto done = g.done;
        const int type = g.type;
        const std::wstring path = g.path;
        const D3D11_TEXTURE2D_DESC td = g.td;
        const UINT bytes = g.bytes;
        if (type != 2) g_diskBytes += g.cost;
        if (!Queue([type, path, td, bytes, maps, done] { WriteGrab(type, path, td, bytes, maps); done->store(1, std::memory_order_release); }))
            done->store(1, std::memory_order_release);   // (the queue refused: unmapped next Present, nothing written)
        ++i;
    }
}

// ---- the picture (dump / mark): flat = the presented back buffer; VR (phase 111) = eye 0's output at its blit -------------------
const char* g_picSource = nullptr;                        // phase 111: what the last picture was taken from (index.txt / the log)
bool        g_picEyeBad = false;                          // phase 111: the eye texture cannot be saved (format / array): back buffer
bool PicFromTexture(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, uint64_t n, const char* what) {
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    const bool fmtOk = Cap::IsBgra8(d.Format) || Cap::IsRgba8(d.Format) || d.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
    if (!fmtOk || d.SampleDesc.Count > 1 || d.ArraySize != 1) return false;
    if (g_picStg && (g_picDesc.Width != d.Width || g_picDesc.Height != d.Height || g_picDesc.Format != d.Format)) {
        g_picStg->Release(); g_picStg = nullptr;
    }
    if (!g_picStg) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.MipLevels = 1; sd.ArraySize = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &g_picStg))) { g_picStg = nullptr; return false; }
        g_picDesc = d;
    }
    PicReq r = g_picReq.front();
    g_picReq.pop_front();
    ctx->CopySubresourceRegion(g_picStg, 0, 0, 0, 0, src, 0, nullptr);
    g_picStg->AddRef();                                   // the grab's own reference (released when written)
    Grab g;
    g.stg = g_picStg; g.type = 2; g.path = r.path; g.td = d; g.td.MipLevels = 1; g.td.ArraySize = 1; g.issued = n;
    g_grabs.push_back(std::move(g));
    g_picBusy = true;
    g_picSource = what;
    return true;
}
// VR, at an eye's blit (inject.cpp, eye 0 = the RIGHT eye, after DLAA + the composite; before the status box / panel are drawn on
// it): a picture that is due is taken from this eye picture (n = the Present counter of this frame)
void PicAtEyeBlit(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, int eye, uint64_t n) {
    if (!g_dev) return;                                   // phase 112r: no draw seen yet (PicFromTexture creates on g_dev)
    if (!g_vrMode || eye != 0 || !tex || g_picEyeBad || g_picBusy || g_picReq.empty() || g_picReq.front().at > n) return;
    if (!PicFromTexture(ctx, tex, n, "VR eye 0 (the RIGHT eye) after DLAA, at its blit")) {
        g_picEyeBad = true;
        D3D11_TEXTURE2D_DESC d{};
        tex->GetDesc(&d);
        LogWarn("census (phase 111): the VR eye picture cannot be saved (format %u, array %u, samples %u) -- the census pictures use the "
                "desktop mirror window (back buffer) instead", (unsigned)d.Format, d.ArraySize, d.SampleDesc.Count);
    }
}
void PicAtPresent(ID3D11DeviceContext* ctx, IDXGISwapChain* sc, uint64_t n) {
    if (g_picBusy) {                                      // the last one: its grab is gone = written
        bool still = false;
        for (const Grab& g : g_grabs) if (g.type == 2) still = true;
        if (!still) g_picBusy = false;
        else return;
    }
    // phase 111: VR takes its pictures at eye 0's blit (PicAtEyeBlit); the back buffer only when that cannot work, or a request that
    // the eye blit has not served 3 Presents after it was due (no blit: a loading screen, the game paused)
    const bool eyeWay = g_vrMode && !g_picEyeBad;
    if (g_picReq.empty() || g_picReq.front().at > n || (eyeWay && n < g_picReq.front().at + 3) || !sc) {
        if (!sc && !g_picReq.empty() && g_picReq.front().at + (eyeWay ? 3 : 0) <= n) g_picReq.pop_front();   // present-layer: no back buffer
        return;
    }
    PicReq r = g_picReq.front();
    g_picReq.pop_front();
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return;
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    const bool fmtOk = Cap::IsBgra8(d.Format) || Cap::IsRgba8(d.Format) || d.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
    if (!fmtOk || d.SampleDesc.Count > 1) { bb->Release(); return; }
    if (g_picStg && (g_picDesc.Width != d.Width || g_picDesc.Height != d.Height || g_picDesc.Format != d.Format)) {
        g_picStg->Release(); g_picStg = nullptr;
    }
    if (!g_picStg) {
        D3D11_TEXTURE2D_DESC sd = d;
        sd.MipLevels = 1; sd.ArraySize = 1; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &g_picStg))) { g_picStg = nullptr; bb->Release(); return; }
        g_picDesc = d;
    }
    ctx->CopySubresourceRegion(g_picStg, 0, 0, 0, 0, bb, 0, nullptr);
    bb->Release();
    g_picStg->AddRef();                                   // the grab's own reference (released when written)
    Grab g;
    g.stg = g_picStg; g.type = 2; g.path = r.path; g.td = d; g.td.MipLevels = 1; g.td.ArraySize = 1; g.issued = n;
    g_grabs.push_back(std::move(g));
    g_picBusy = true;
    g_picSource = g_vrMode ? "the desktop mirror window (back buffer; the VR runtime picks its eye)" : "the presented back buffer";
}

// ---- the draw's own state (phase 110: every dump writes it; render thread, at the draw) ----------------------------------------
struct DrawArgs { UINT ic = 0, inst = 1, si = 0, sinst = 0; INT bv = 0; bool instanced = false; };
// the input layouts (phase 110: the CreateInputLayout hook, any thread): layout object -> its element list as text. D3D11 cannot read
// a layout's elements back from the object, so they are noted at creation (a re-created object at a released address overwrites)
constexpr size_t kIlMax = 16384;
SRWLOCK g_ilLk = SRWLOCK_INIT;
std::map<const void*, std::shared_ptr<const std::string>> g_il;
std::atomic<uint32_t> g_ilNoted{0}, g_ilFull{0};
std::shared_ptr<const std::string> LayoutText(const void* il) {
    if (!il) return nullptr;
    AcquireSRWLockShared(&g_ilLk);
    auto it = g_il.find(il);
    std::shared_ptr<const std::string> r = it != g_il.end() ? it->second : nullptr;
    ReleaseSRWLockShared(&g_ilLk);
    return r;
}
std::string ResText(ID3D11Resource* r) {
    char b[200];
    if (!r) return "(none)";
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
        snprintf(b, sizeof(b), "Texture2D %s %ux%u mips %u array %u samples %u bind 0x%x", FmtText(d.Format).c_str(), d.Width, d.Height,
                 d.MipLevels, d.ArraySize, d.SampleDesc.Count, (unsigned)d.BindFlags);
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d{};
        static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
        snprintf(b, sizeof(b), "Buffer %u bytes, usage %d, bind 0x%x, cpu 0x%x, misc 0x%x, stride %u", d.ByteWidth, (int)d.Usage,
                 (unsigned)d.BindFlags, (unsigned)d.CPUAccessFlags, (unsigned)d.MiscFlags, d.StructureByteStride);
    } else snprintf(b, sizeof(b), "resource dimension %d", (int)dim);
    return b;
}
std::string BufText(ID3D11Buffer* bf) { return bf ? ResText(bf) : std::string("(none)"); }
// everything StateText (first sight) does not hold: the draw call, viewports, scissors, targets, IA, the VS side, constant buffers
std::string DrawText(ID3D11DeviceContext* ctx, const DrawArgs& a) {
    std::string s;
    char b[400];
    if (a.instanced)
        snprintf(b, sizeof(b), "    draw: DrawIndexedInstanced(IndexCountPerInstance %u, InstanceCount %u, StartIndexLocation %u, "
                 "BaseVertexLocation %d, StartInstanceLocation %u)\n", a.ic, a.inst, a.si, a.bv, a.sinst);
    else snprintf(b, sizeof(b), "    draw: DrawIndexed(IndexCount %u, StartIndexLocation %u, BaseVertexLocation %d)\n", a.ic, a.si, a.bv);
    s += b;
    snprintf(b, sizeof(b), "    pass: %s (jittered %d, G-buffer %d, mirror %d), jitter overscan viewport shift %+.4f %+.4f px\n",
             g_mirJit ? (g_mirGbuf ? "mirror G-buffer" : "mirror forward") : (g_gbufPass ? "G-buffer" : "forward"), (int)g_jitterPass,
             (int)g_gbufPass, (int)g_mirJit,
             (double)g_vpShiftX, (double)g_vpShiftY);
    s += b;
    if (g_vrMode) {                                       // phase 111: which eye this dumped draw belongs to
        const int e = CurPassEye();
        snprintf(b, sizeof(b), "    VR: world pass %d of this frame (s=%llu), %s%s\n", CurPassPos() + 1,
                 (unsigned long long)(g_passSeq ? g_passSeq - 1 : 0), EyeName(e), g_mirJit ? " (a mirror pass inside it)" : "");
        s += b;
    }
    {
        D3D11_VIEWPORT vp[16]; UINT nv = 16;
        ctx->RSGetViewports(&nv, vp);
        for (UINT i = 0; i < nv && i < 16; ++i) {
            snprintf(b, sizeof(b), "    viewport %u: x %.3f y %.3f w %.3f h %.3f depth %.3f..%.3f\n", i, (double)vp[i].TopLeftX,
                     (double)vp[i].TopLeftY, (double)vp[i].Width, (double)vp[i].Height, (double)vp[i].MinDepth, (double)vp[i].MaxDepth);
            s += b;
        }
        D3D11_RECT sr[16]; UINT ns = 16;
        ctx->RSGetScissorRects(&ns, sr);
        for (UINT i = 0; i < ns && i < 16; ++i) {
            snprintf(b, sizeof(b), "    scissor %u: %ld,%ld .. %ld,%ld\n", i, sr[i].left, sr[i].top, sr[i].right, sr[i].bottom);
            s += b;
        }
    }
    {
        ID3D11RenderTargetView* rtv[8] = {};
        ID3D11DepthStencilView* dsv = nullptr;
        ctx->OMGetRenderTargets(8, rtv, &dsv);
        for (int i = 0; i < 8; ++i) {
            if (!rtv[i]) continue;
            D3D11_RENDER_TARGET_VIEW_DESC vd{};
            rtv[i]->GetDesc(&vd);
            ID3D11Resource* r = nullptr;
            rtv[i]->GetResource(&r);
            snprintf(b, sizeof(b), "    RT%d: view %s dim %d mip %u, %s\n", i, FmtText(vd.Format).c_str(), (int)vd.ViewDimension,
                     vd.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D ? vd.Texture2D.MipSlice : 0u, ResText(r).c_str());
            s += b;
            if (r) r->Release();
            rtv[i]->Release();
        }
        if (dsv) {
            D3D11_DEPTH_STENCIL_VIEW_DESC vd{};
            dsv->GetDesc(&vd);
            ID3D11Resource* r = nullptr;
            dsv->GetResource(&r);
            snprintf(b, sizeof(b), "    depth target: view %s dim %d flags 0x%x, %s\n", FmtText(vd.Format).c_str(), (int)vd.ViewDimension,
                     (unsigned)vd.Flags, ResText(r).c_str());
            s += b;
            if (r) r->Release();
            dsv->Release();
        } else s += "    depth target: (none)\n";
    }
    {
        ID3D11InputLayout* il = nullptr;
        ctx->IAGetInputLayout(&il);
        const std::shared_ptr<const std::string> lt = LayoutText(il);
        snprintf(b, sizeof(b), "    input layout %p: ", (void*)il);
        s += b;
        s += lt ? *lt : std::string(il ? "(created before the hook: elements unknown -- see the VS input signature in vs.txt)\n"
                                       : "(none)\n");
        if (il) il->Release();
        ID3D11Buffer* vb[8] = {}; UINT st[8] = {}, of[8] = {};
        ctx->IAGetVertexBuffers(0, 8, vb, st, of);
        for (int i = 0; i < 8; ++i) {
            if (!vb[i]) continue;
            snprintf(b, sizeof(b), "    vertex buffer %d: stride %u offset %u, %s\n", i, st[i], of[i], BufText(vb[i]).c_str());
            s += b;
            vb[i]->Release();
        }
        ID3D11Buffer* ib = nullptr; DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN; UINT io = 0;
        ctx->IAGetIndexBuffer(&ib, &f, &io);
        snprintf(b, sizeof(b), "    index buffer: %s offset %u, %s\n", FmtText(f).c_str(), io, BufText(ib).c_str());
        s += b;
        if (ib) ib->Release();
        s += "    (geometry contents are not saved: the vertex / index data needs a RenderDoc capture)\n";
    }
    {
        ID3D11ShaderResourceView* v[16] = {};
        ctx->VSGetShaderResources(0, 16, v);
        for (int i = 0; i < 16; ++i) {
            if (!v[i]) continue;
            ID3D11Resource* r = nullptr;
            v[i]->GetResource(&r);
            snprintf(b, sizeof(b), "    VS t%d: %s\n", i, ResText(r).c_str());
            s += b;
            if (r) r->Release();
            v[i]->Release();
        }
        ID3D11SamplerState* sm[4] = {};
        ctx->VSGetSamplers(0, 4, sm);
        for (int i = 0; i < 4; ++i) if (sm[i]) { snprintf(b, sizeof(b), "    VS s%d: bound\n", i); s += b; sm[i]->Release(); }
    }
    {
        ID3D11DeviceContext1* c1 = nullptr;
        ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1);
        for (int stage = 0; stage < 2; ++stage) {
            ID3D11Buffer* cb[14] = {};
            UINT first[14] = {}, num[14] = {};
            if (c1) { if (stage == 0) c1->PSGetConstantBuffers1(0, 14, cb, first, num); else c1->VSGetConstantBuffers1(0, 14, cb, first, num); }
            else    { if (stage == 0) ctx->PSGetConstantBuffers(0, 14, cb); else ctx->VSGetConstantBuffers(0, 14, cb); }
            for (int k = 0; k < 14; ++k) {
                if (!cb[k]) continue;
                snprintf(b, sizeof(b), "    %s cb%d: window first %u, %u constants, %s\n", stage == 0 ? "PS" : "VS", k, first[k], num[k],
                         BufText(cb[k]).c_str());
                s += b;
                cb[k]->Release();
            }
        }
        if (c1) c1->Release();
    }
    return s;
}

// ---- the dump of one pair (started at a draw of it: its textures / cbuffers are bound) ------------------------------------------
// dir = the absolute folder (ends with '\'), rel = the same relative to the game folder (the texts), full = shaders + disassembly +
// textures + constant buffers (else only draw.txt: the state at this draw, the verdict, the link), extra = more text for draw.txt.
// Phase 110: shared by the census_dump path (dlaa_census\<vs8>_<ps8>\) and the capture path (<capture>\census\<vs8>_<ps8>\).
std::wstring Widen(const char* s) { std::wstring w; while (*s) w += (wchar_t)(unsigned char)*s++; return w; }
std::string PairText(const Pair& p, int rank);
// the PS's declared textures `slots` of pair p -> tex_tN.dds in dir; the status lines into `note`; slots refused only by the in-flight
// cap come back in the result (a capture grabs them at a later draw)
uint32_t GrabPsTextures(ID3D11DeviceContext* ctx, uint32_t slots, const std::wstring& dir, uint64_t n, uint64_t* spent,
                        std::string& note, bool late) {
    uint32_t left = 0;
    for (uint32_t t = 0; t < 32; ++t) {
        if (!(slots & (1u << t))) continue;
        ID3D11ShaderResourceView* v = nullptr;
        ctx->PSGetShaderResources(t, 1, &v);
        char b[200];
        if (!v) { snprintf(b, sizeof(b), "  t%u: nothing bound%s\n", t, late ? " (later draw)" : ""); note += b; continue; }
        ID3D11Resource* r = nullptr;
        v->GetResource(&r);
        v->Release();
        if (!r) continue;
        D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
        r->GetType(&dim);
        const char* why = "not a Texture2D";
        bool ok = false;
        wchar_t nm[32];
        swprintf(nm, 32, L"tex_t%u.dds", t);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ok = GrabTexture(ctx, static_cast<ID3D11Texture2D*>(r), dir + nm, n, spent, &why);
        r->Release();
        const bool retry = !ok && why == kWhyInFlight && g_grabForCap;
        if (retry) left |= 1u << t;
        if (ok) snprintf(b, sizeof(b), "  t%u: saved as tex_t%u.dds (DX10 DDS, every mip / slice)%s\n", t, t,
                         late ? " -- at a LATER draw of the pair (Present # in index.txt; can be another mesh's texture)" : "");
        else if (retry) snprintf(b, sizeof(b), "  t%u: deferred (staging in flight full): grabbed at a later draw, see index.txt\n", t);
        else snprintf(b, sizeof(b), "  t%u: not saved -- %s\n", t, why);
        note += b;
    }
    return left;
}
uint64_t DumpPair(ID3D11DeviceContext* ctx, int pi, uint64_t n, const DrawArgs& a, const std::wstring& dir, const std::string& rel,
                  bool full, const std::string& extra) {
    Pair& p = g_pairs[pi];
    const ShInfo* vsi = p.vs >= 0 ? g_info[p.vs] : nullptr;
    const ShInfo* psi = g_info[p.ps];
    uint64_t spent = 0;
    std::string texNote;
    if (full) {
        p.capTexLeft = GrabPsTextures(ctx, psi->scan.texDecl, dir, n, &spent, texNote, false);
        // constant buffers: PS b0..b7, VS b0..b7 (the bound windows, D3D11.1 offsets)
        ID3D11DeviceContext1* c1 = nullptr;
        ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1);
        for (int stage = 0; stage < 2; ++stage) {
            ID3D11Buffer* cb[8] = {};
            UINT first[8] = {}, num[8] = {};
            if (c1) { if (stage == 0) c1->PSGetConstantBuffers1(0, 8, cb, first, num); else c1->VSGetConstantBuffers1(0, 8, cb, first, num); }
            else    { if (stage == 0) ctx->PSGetConstantBuffers(0, 8, cb); else ctx->VSGetConstantBuffers(0, 8, cb); }
            for (UINT k = 0; k < 8; ++k) {
                if (!cb[k]) continue;
                wchar_t nm[32];
                swprintf(nm, 32, L"%ls_cb%u.bin", stage == 0 ? L"ps" : L"vs", k);
                const bool ok = GrabBuffer(ctx, cb[k], first[k], num[k], dir + nm, n, &spent);
                char b[160];
                snprintf(b, sizeof(b), "  %s cb%u: %s (window first %u, %u constants; at most the first 64 KB)\n", stage == 0 ? "PS" : "VS",
                         k, ok ? "saved" : "not saved (budget)", first[k], num[k]);
                texNote += b;
                cb[k]->Release();
            }
        }
        if (c1) c1->Release();
        p.where = rel;
    }
    // shaders + text on the writer
    std::vector<uint8_t> vsCode = full && vsi ? vsi->code : std::vector<uint8_t>();
    std::vector<uint8_t> psCode = full ? psi->code : std::vector<uint8_t>();
    if (full && vsi && vsCode.empty()) texNote += "  vs.dxbc: NOT kept (created while the census was off, over the 24 MB idle bytecode cap)\n";
    if (full && !vsi) texNote += "  vs.dxbc: none (the VS was created before the mod's hooks)\n";
    if (full && psCode.empty()) texNote += "  ps.dxbc: NOT kept (created while the census was off, over the 24 MB idle bytecode cap)\n";
    const double pxt = p.triSum ? (double)p.pixSum / (double)p.triSum : 0.0;
    std::string txt = PairText(p, 0) + WhyText(p.v, p.cover, psi->scan, pxt, p.samples, p.probe.probes, p.probe.thin, p.probe.Stable());
    char hb[160];
    snprintf(hb, sizeof(hb), "\n  state at THIS draw (Present #%llu):\n", (unsigned long long)n);
    txt += hb;
    txt += StateText(ctx, psi);
    txt += DrawText(ctx, a);
    if (full) { txt += "\n  dump contents at the draw:\n"; txt += texNote; }
    txt += extra;
    if (g_grabForCap) p.capTex = texNote;
    const bool queued = Queue([dir, vsCode, psCode, txt] {
        MakeDirs(dir);
        typedef HRESULT (WINAPI* Dis_t)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);
        static Dis_t s_dis = nullptr;
        static bool s_tried = false;
        if (!s_tried) {
            s_tried = true;
            if (HMODULE h = LoadLibraryW(L"d3dcompiler_47.dll")) s_dis = (Dis_t)GetProcAddress(h, "D3DDisassemble");
        }
        auto one = [&](const std::vector<uint8_t>& code, const wchar_t* bin, const wchar_t* txtName) {
            if (code.empty()) return;
            (WriteFileAll(dir + bin, code.data(), code.size()) ? g_filesOk : g_filesFail).fetch_add(1);
            ID3DBlob* blob = nullptr;
            if (s_dis && SUCCEEDED(s_dis(code.data(), code.size(), 0x4 | 0x80 /* D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING | D3D_DISASM_PRINT_HEX_LITERALS (exact constants) */, nullptr, &blob)) && blob) {
                size_t len = blob->GetBufferSize();
                const char* s = static_cast<const char*>(blob->GetBufferPointer());
                while (len && !s[len - 1]) --len;
                WriteFileAll(dir + txtName, s, len);
                blob->Release();
            }
        };
        one(vsCode, L"vs.dxbc", L"vs.txt");
        one(psCode, L"ps.dxbc", L"ps.txt");
        (WriteFileAll(dir + L"draw.txt", txt.data(), txt.size()) ? g_filesOk : g_filesFail).fetch_add(1);
    });
    if (!queued) {                                        // phase 112r: the writer queue was full -- say so (index.txt / the log)
        g_filesFail.fetch_add(1);
        if (g_grabForCap) p.capTex += "  draw.txt, vs / ps .dxbc + .txt: not written (queue full)\n";
        static int s_full = 0;
        if (s_full++ < 8) LogWarn("census (phase 112r): the writer queue is full -- draw.txt / shaders of %s not written", rel.c_str());
        return spent;
    }
    g_diskBytes += vsCode.size() + psCode.size() + txt.size();
    return spent;
}
// the census_dump path (phase 108): a queued dump of a NEW suspect, at its next draw -> dlaa_census\<vs8>_<ps8>\ + its picture
void StartDump(ID3D11DeviceContext* ctx, int pi, uint64_t n, const DrawArgs& a) {
    Pair& p = g_pairs[pi];
    const ShInfo* vsi = p.vs >= 0 ? g_info[p.vs] : nullptr;
    snprintf(p.dumpDir, sizeof(p.dumpDir), "%s_%s", vsi ? Short(vsi->sum).c_str() : "novs", Short(g_info[p.ps]->sum).c_str());
    std::wstring dir;
    if (!DirPath(Widen(p.dumpDir).c_str(), dir)) { p.dump = -1; return; }
    p.dump = 2;                                           // (the folders are made by the writer thread, before each file)
    ++g_dumpsStarted;
    g_lastDumpSec = NowSec();
    const uint64_t spent = DumpPair(ctx, pi, n, a, dir, std::string("dlaa_census\\") + p.dumpDir + "\\", true, "");
    // the frame with only this pair tinted (census view on; view off: untinted, phase 109r), kHiFrames Presents from now
    g_hiPair = pi;
    g_hiLeft = kHiFrames;
    g_picReq.push_back({ dir + L"picture.png", n + kHiFrames - 2 });   // (read while the tint is still on: g_hiLeft >= 1)
    Log("census (phase 108): dump %d started -> dlaa_census\\%s\\ (%s; %.1f MB staged, Present #%llu)", g_dumpsStarted, p.dumpDir,
        KindList(p.v.Suspect()).c_str(), (double)spent / (1024.0 * 1024.0), (unsigned long long)n);
}

// ---- verdict + first sight ------------------------------------------------------------------------------------------------------
void Rejudge(Pair& p) {
    const ShInfo* psi = g_info[p.ps];
    p.v = Judge(p.structural, p.cover, p.thin);
    p.fixOff = FixOffKinds(p.structural, p.cover, p.thin);   // phase 113 (the legend's "a fix is switched off" line)
    p.interest = psi->interest || p.thinPre;              // (113b: a candidate's every draw goes through the slow path: probes, tint)
}
// phase 113b: the pair's thinness from its pre-filter + its probes (both may change at Present)
void UpdateThin(Pair& p) {
    const bool thin = p.thinPre && p.probe.Verdict() == CensusProbe::kThin;
    if (thin != p.thin) { p.thin = thin; Rejudge(p); }
}
void OnSuspect(ID3D11DeviceContext* ctx, int pi, uint64_t n) {
    Pair& p = g_pairs[pi];
    if (p.logged) return;
    p.logged = true;
    if (g_nextWrite > NowSec() + kSoonSecs) g_nextWrite = NowSec() + kSoonSecs;   // phase 109r: the file follows soon (no exit write)
    static int s_lines = 0;
    const ShInfo* vsi = p.vs >= 0 ? g_info[p.vs] : nullptr;
    const ShInfo* psi = g_info[p.ps];
    if (s_lines++ < 64)
        Log("census (phase 108): NEW suspect #%d -- %s | VS %s PS %s | %s pass %.0fx%.0f, IndexCount %u%s (Present #%llu)%s", pi,
            KindList(p.v.Suspect()).c_str(), vsi ? Short(vsi->sum).c_str() : "(before the hooks)", Short(psi->sum).c_str(),
            (p.passMask & 8) ? "mirror G-buffer" : ((p.passMask & 4) ? "mirror" : ((p.passMask & 1) ? "G-buffer" : "forward")), (double)p.vpW, (double)p.vpH, p.icMin,
            p.inst ? " instanced" : "", (unsigned long long)n, s_lines == 64 ? " (no more NEW suspect lines)" : "");
    if (g_censusDumpCfg && p.dump == 0) {
        if (g_dumpsStarted + (int)g_dumpQueue.size() >= kDumpMax || g_dumpQueue.size() >= 256) { p.dump = -1; ++g_dumpsRefused; }
        else { p.dump = 1; g_dumpQueue.push_back(pi); }
    }
    (void)ctx;
}
void FirstSight(ID3D11DeviceContext* ctx, Pair& p) {
    p.captured = true;
    p.state = std::make_shared<const std::string>(StateText(ctx, g_info[p.ps]));
    if (p.structural & kCutout) p.cutSlots = g_info[p.ps]->scan.discardTex;   // (the reason: per checked draw, SlowBody)
}
// phase 110b: one checked draw of a cut-out pair -> its CutTrack; the pair is re-judged when the shown reason changes (BUG before
// 110b: FirstSight set the reason WITHOUT a re-judge, so every cut-out pair kept the verdict of its creation -- "not seen" = suspect --
// and the 3 re-checks re-judged only when the reason differed from the never-judged one). Returns this draw's reason (-1 = unknown).
int CutCheck(ID3D11DeviceContext* ctx, Pair& p, uint64_t n) {
    int r = CutReasonDraw(ctx, p.cutSlots, n);
    // phase 112r: the lock busy (-1) and no memo for this view: the pair's last answer stands in -- else one VR eye's draw went
    // untinted while the other was orange (binocular flicker in the census view)
    if (r >= 0) { p.cut.Note(r, n); p.cutLast = r; }
    else r = p.cutLast;
    const int shown = p.cut.Shown(n);
    if (shown != p.cover.cutReason) { p.cover.cutReason = shown; Rejudge(p); }
    return r;
}

// ---- phase 110: the census part of a bug report (the capture key pressed twice, or Capture now, while the census view is on) ----
// At the key (CensusOnCapture, Present n): a MARK whose picture is the capture's census\tinted.png (frame n+1, every suspect tinted),
// and the ARMED frames n+1 .. n+kCapArmFrames: every suspect pair drawn there gets <capture>\census\<vs8>_<ps8>\ at its first draw
// (CapDecide: a full dump, a link to an earlier full dump still on disk, or text only past the budget) and an occlusion query
// around EVERY one of its draws there (its screen coverage in those frames). The capture's own frames are untinted (TintHeld).
// After the capture's sets: one picture per pair (the biggest first, kCapMaxPictures) with that pair alone tinted, then
// census\index.txt + census\dlaa_census.txt; CaptureFinishTick waits for that (CensusCaptureBusy) before the zip.
struct CapRun {
    bool        active = false;
    int         no = 0;                      // capture number (Pair::capNo)
    int         stage = 0;                   // 0 armed, 1 queries / sets, 2 pictures, 3 grabs queued, 4 index written
    uint64_t    key = 0, armFrom = 0, armTo = 0;
    std::wstring dir;                        // absolute ...\dlaa_selftest\shimmer_<time>\census\ (ends with '\')
    std::string rel;                         // the same from the game folder
    int         mark = 0;
    std::vector<int> pairs;                  // in the order first drawn
    std::vector<int> pics;                   // the picture order (CapPictureOrder)
    size_t      picNext = 0;
    int         picPair = -1;                // the pair of the picture in flight
    const char* picWhy = nullptr;            // why no pictures (null = taken)
    int         dumped = 0, linked = 0, textOnly = 0;
    uint64_t    staged = 0;
    double      keySec = 0.0;
    SYSTEMTIME  keyTime{};
    std::shared_ptr<std::atomic<int>> indexDone;
};
CapRun g_cap;
inline bool TintHeld() { return g_testRunning && g_testShimmer; }   // the capture's sets run: its frames show the real picture
// the first draw of a suspect pair in the armed frames (render thread, the pair's state bound)
void CapFirstDraw(ID3D11DeviceContext* ctx, int pi, uint64_t n, const DrawArgs& a) {
    Pair& p = g_pairs[pi];
    p.capNo = g_cap.no;
    p.capDraws = 0; p.capQueries = 0; p.capPx = 0; p.capPicPresent = 0; p.capTexLeft = 0; p.capTex.clear();
    const ShInfo* vsi = p.vs >= 0 ? g_info[p.vs] : nullptr;
    snprintf(p.dumpDir, sizeof(p.dumpDir), "%s_%s", vsi ? Short(vsi->sum).c_str() : "novs", Short(g_info[p.ps]->sum).c_str());
    bool onDisk = false;
    if (!p.where.empty()) {                               // an earlier FULL dump: still there? (one file-system call per pair, at
        wchar_t base[MAX_PATH];                           //   a key press -- capture_keep may have deleted an older capture)
        if (PathNextToDll(L"", base)) onDisk = GetFileAttributesW((std::wstring(base) + Widen(p.where.c_str())).c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    const bool budget = g_diskBytes + g_inFlight < ((uint64_t)g_censusMaxMb << 20);
    p.capAction = CapDecide(!p.where.empty(), onDisk, g_cap.dumped, kCapMaxPairs, budget);
    const std::wstring dir = g_cap.dir + Widen(p.dumpDir) + L"\\";
    const std::string rel = g_cap.rel + p.dumpDir + "\\";
    std::string extra;
    if (p.capAction == kCapLink) extra = "\n  shaders, textures and constant buffers: see the earlier dump " + p.where + "\n";
    if (p.capAction == kCapTextOnly) extra = "\n  shaders, textures and constant buffers: NOT saved (the dump budget is spent: census_max_mb or 48 pairs per capture)\n";
    g_grabForCap = true;
    g_cap.staged += DumpPair(ctx, pi, n, a, dir, rel, p.capAction == kCapDump, extra);
    g_grabForCap = false;
    if (p.capAction == kCapDump) ++g_cap.dumped; else if (p.capAction == kCapLink) ++g_cap.linked; else ++g_cap.textOnly;
    if (CapSaves(p.capAction) && !p.capSession) p.capSession = g_cap.no;   // phase 115: saved -> not highlighted after this capture
    g_cap.pairs.push_back(pi);
}
// phase 115r (audit B7): a capture that was CANCELLED (never started, or the device changed) saved nothing -- its pairs are
// highlighted again (CapFirstDraw marks them at their first armed draw, before the capture is known to run)
void CapUnsave(int no) {
    if (no <= 0) return;
    for (Pair& p : g_pairs) if (p.capSession == no) p.capSession = 0;
}
// a later draw of a pair whose textures were refused by the in-flight cap: grab them now (same folder, noted in index.txt)
void CapLateTextures(ID3D11DeviceContext* ctx, int pi, uint64_t n) {
    Pair& p = g_pairs[pi];
    if (g_inFlight >= kInFlightCap / 2) return;           // (wait until the writer has freed half of the staging)
    uint64_t spent = 0;
    std::string note;
    g_grabForCap = true;
    const uint32_t left = GrabPsTextures(ctx, p.capTexLeft, g_cap.dir + Widen(p.dumpDir) + L"\\", n, &spent, note, true);
    g_grabForCap = false;
    char b[64];
    snprintf(b, sizeof(b), "  (later draw, Present #%llu:)\n", (unsigned long long)n);
    p.capTex += b;
    p.capTex += note;
    p.capTexLeft = left;
    g_cap.staged += spent;
}

// ---- the draw hooks -------------------------------------------------------------------------------------------------------------
struct Tok { int pair = -1; int q = -1; int tint = -1; bool hdr = false; int probe = -1;   // (113b: probe = a g_pr record to run)
             bool mirror = false; int vsl = 0; };   // phase 115: a mirror pass (the in-place tint) / the VS layout (the mask PS)

void SlowBody(ID3D11DeviceContext* ctx, const DrawArgs& a, bool sampleOnly, Tok& t, uint64_t t0);
// (the mod's own D3D calls inside -- copies, queries, getters -- run with t_inDlaa quiet: the hooks pass them through, VRS untouched)
inline void Slow(ID3D11DeviceContext* ctx, const DrawArgs& a, bool sampleOnly, Tok& t) {
    const uint64_t t0 = __rdtsc();
    t_inDlaa.Quiet(true);
    SlowBody(ctx, a, sampleOnly, t, t0);
    t_inDlaa.Quiet(false);
    g_ticksSlow += __rdtsc() - t0;
}
// one occlusion query around this draw (true = begun; t.q = its ring slot)
bool BeginQuery(ID3D11DeviceContext* ctx, int pi, uint32_t tris, uint64_t n, Tok& t, bool cap) {
    Query& q = g_q[g_qNext];
    if (q.busy) return false;
    if (!q.q) {
        D3D11_QUERY_DESC qd{ D3D11_QUERY_OCCLUSION, 0 };
        if (FAILED(g_dev->CreateQuery(&qd, &q.q))) { q.q = nullptr; return false; }
    }
    ctx->Begin(q.q);
    q.busy = true; q.pair = pi; q.tris = tris; q.issued = n; q.cap = cap; q.capNo = cap ? g_cap.no : 0; q.cov = -1.0f;
    q.probe = -1; q.role = 0;                            // (phase 113b: not a probe's query)
    t.q = g_qNext;
    g_qNext = (g_qNext + 1) % kQueryRing;
    ++g_cQueries;
    return true;
}
// phase 113b: does this thin CANDIDATE want a probe of a draw without a fresh verdict? A pair whose thinness a fix covers (wire_widen,
// pole_aa, the known list) only until it has a verdict (the file shows it); any other until kPairProbesFull probes, then only while
// the view is on and the pair is thin (so its new draws get their own verdict for the tint).
bool ProbeWanted(const Pair& p, bool viewNow) {
    const int made = p.probe.probes + p.probe.tiny;
    if (made >= CensusProbe::kPairProbesFull)
        return viewNow && p.thin && made < 8 * CensusProbe::kPairProbesFull;
    if (Judge(p.structural, p.cover, true).covered & kThin) return p.probe.probes < CensusProbe::kPairProbesMin;
    return true;
}
// two occlusion queries + a record for the probe of this draw (run after the game's draw: Probe)
bool StartProbe(int pi, int key, uint64_t n, Tok& t) {
    ProbeRec& r = g_pr[g_prNext];
    const int ia = g_qNext, ib = (g_qNext + 1) % kQueryRing;
    Query& a = g_q[ia];
    Query& b = g_q[ib];
    if (r.busy || a.busy || b.busy) return false;
    D3D11_QUERY_DESC qd{ D3D11_QUERY_OCCLUSION, 0 };
    if (!a.q && FAILED(g_dev->CreateQuery(&qd, &a.q))) { a.q = nullptr; return false; }
    if (!b.q && FAILED(g_dev->CreateQuery(&qd, &b.q))) { b.q = nullptr; return false; }
    for (int k = 0; k < 2; ++k) {
        Query& q = k ? b : a;
        q.busy = true; q.pair = pi; q.tris = 0; q.issued = n; q.cap = false; q.capNo = 0; q.cov = -1.0f; q.probe = g_prNext; q.role = k + 1;
    }
    r = ProbeRec{};
    r.busy = true; r.pair = pi; r.slot = key; r.key = g_keys->s[key].key; r.qSum = ia; r.qAll = ib;
    t.probe = g_prNext;
    g_prNext = (g_prNext + 1) % kProbeRing;
    g_qNext = (ib + 1) % kQueryRing;
    ++g_cProbes;
    return true;
}
void SlowBody(ID3D11DeviceContext* ctx, const DrawArgs& a, bool sampleOnly, Tok& t, uint64_t t0) {
    (void)t0;
    const UINT ic = a.ic, inst = a.inst;
    const bool instanced = a.instanced;
    ++g_cSlow;
    if (!DevOk(ctx)) return;
    void* vs = nullptr;
    if (g_vsHooked && g_vsValid && (++g_vsCheck & 255u)) vs = g_vsCur;
    else {
        ID3D11VertexShader* v = nullptr;
        ctx->VSGetShader(&v, nullptr, nullptr);
        vs = v;
        if (v) v->Release();
        if (g_vsHooked && g_vsValid && vs != g_vsCur) {  // every 256th: the hook's note must match the getter (a vtable rewrite
            g_vsHooked = false;                            // can bypass a detour) -- a mismatch = the getter from now on
            LogWarn("census (phase 108): the VSSetShader note disagreed with VSGetShader -- the census reads the VS per draw from now on");
        }
        if (g_vsHooked) { g_vsCur = vs; g_vsValid = true; }
    }
    const void* ps = m_ps;
    int pi = -1;
    if (vs == m_pairVs && ps == m_pairPs && m_pairI >= 0) pi = m_pairI;
    else {
        if (vs != m_vsMemo) { m_vsMemo = vs; m_vsIdx = LookupObj(vs); }
        const uint32_t key = ((uint32_t)(m_vsIdx + 1) << 16) | (uint32_t)(m_psIdx + 1);
        uint32_t s = PairSlot(key);
        for (int k = 0; k < kPairMax * 2; ++k, s = (s + 1) & (kPairMax * 2 - 1)) {
            if (g_pairKey[s] == key) { pi = g_pairIdx[s]; break; }
            if (!g_pairKey[s]) {
                if ((int)g_pairs.size() >= kPairMax) { ++g_pairFull; break; }
                pi = (int)g_pairs.size();
                g_pairs.emplace_back();
                Pair& p = g_pairs.back();
                p.vs = m_vsIdx; p.ps = m_psIdx;
                const ShInfo* psi = g_info[m_psIdx];
                p.structural = psi->scan.kinds;
                p.cover.waterRule = psi->waterRule;
                p.cover.pixelFix = psi->prFix;
                p.cover.gameWire = psi->gameWirePs && WireWiden::IsGameWireVs(vs);   // phase 109b
                p.cover.wirePsKind = psi->wirePs;
                p.cover.wireVs = psi->wirePs && WireWiden::IsWireVs(vs);
                p.cover.solidKind = psi->solidPs;
                p.cover.solidVs = psi->solidPs && WireWiden::IsSolidVs(vs, psi->solidPs);
                p.cover.cutReason = kCutPending;          // phase 110b: undecided until its first checked draw (never "suspect")
                if (m_vsIdx >= 0) p.cover.knownPair = KnownPairIndex(g_info[m_vsIdx]->sum, psi->sum);   // phase 111
                p.vsLayout = m_vsIdx >= 0 ? g_info[m_vsIdx]->vsLayout : 0;   // phase 115
                ApplyFixes(p.cover, g_fixMask == 0xFFFFFFFFu ? FixMaskNow() : g_fixMask);   // phase 110b
                p.firstPresent = g_frames.load(std::memory_order_relaxed);
                p.firstSec = NowSec();
                GetLocalTime(&p.firstTime);
                p.vpW = g_gameVpN ? g_gameVp[0].Width : 0.0f; p.vpH = g_gameVpN ? g_gameVp[0].Height : 0.0f;
                Rejudge(p);
                g_pairKey[s] = key; g_pairIdx[s] = pi;
                break;
            }
        }
        m_pairVs = vs; m_pairPs = ps; m_pairI = pi;
    }
    if (pi < 0) return;
    Pair& p = g_pairs[pi];
    const uint64_t n = g_frames.load(std::memory_order_relaxed);
    const bool viewNow = g_view.load(std::memory_order_relaxed) && !TintHeld();
    int drawCut = -1;                                     // phase 110b: this draw's cut-out texture reason (-1 = not checked)
    if (!sampleOnly || p.interest) {                      // counted draws: every draw of an interesting pair
        if (p.lastPresent != n || !p.draws) { ++p.framesSeen; p.lastPresent = n; }
        ++p.draws;
        if (ic < p.icMin) p.icMin = ic;
        if (ic > p.icMax) p.icMax = ic;
        if (instanced) { p.inst = true; if (inst > p.instMax) p.instMax = inst; } else p.nonInst = true;
        p.passMask |= (uint8_t)(g_mirJit ? (g_mirGbuf ? 4 | 8 : 4) : (g_gbufPass ? 1 : 2));   // (112r: + the mirror G-buffer)
        p.posMask |= (uint8_t)(1u << CurPassPos());       // phase 111
        if (p.interest && !p.captured) FirstSight(ctx, p);
        // phase 110b: the texture of this draw (view on: every draw -- the tint follows the draw's own texture; off: 1 in kCutStride)
        if ((p.structural & kCutout) && (viewNow || p.cut.at[0] + p.cut.at[1] + p.cut.at[2] == 0 || (++p.cutTick % kCutStride) == 0))
            drawCut = CutCheck(ctx, p, n);
        if (p.v.Suspect() && !p.logged) OnSuspect(ctx, pi, n);
        // a queued dump starts at a draw of its pair (rate-limited; not while a capture's census part runs)
        if (p.dump == 1 && g_hiLeft == 0 && !g_cap.active && NowSec() - g_lastDumpSec >= kDumpGapSecs) {
            for (size_t k = 0; k < g_dumpQueue.size(); ++k) if (g_dumpQueue[k] == pi) { g_dumpQueue.erase(g_dumpQueue.begin() + k); break; }
            if (g_diskBytes + g_inFlight < ((uint64_t)g_censusMaxMb << 20)) StartDump(ctx, pi, n, a);
            else { p.dump = -1; ++g_dumpsRefused; }
        }
    }
    const uint32_t tris = (ic / 3u) * (instanced ? (inst ? inst : 1u) : 1u);
    // phase 110: the armed frames of a capture -- every suspect pair drawn: its folder at the first draw, a query around EVERY draw
    bool capQ = false;
    if (g_cap.active && CapWanted(p.v.Suspect(), p.fixOff)) {   // phase 116: not a pair whose only kinds a switched-off fix covers
        if (g_cap.stage == 0 && n >= g_cap.armFrom && n <= g_cap.armTo) {
            if (p.capNo != g_cap.no) CapFirstDraw(ctx, pi, n, a);
            ++p.capDraws;
            capQ = BeginQuery(ctx, pi, tris, n, t, true);
        } else if (p.capNo == g_cap.no && p.capTexLeft && n <= g_cap.key + kCapTexRetry) CapLateTextures(ctx, pi, n);
    }
    // a sampled occlusion query
    const int want = p.v.Suspect() ? kSamplesSuspect : kSamplesThin;
    // (113b: the queries the jitter probes of this Present still need stay free)
    if (!capQ && !g_mirJit && g_qBudget > 2 * g_probeLeft && p.samplesAll < want && (p.v.Suspect() || tris >= kThinMinTris))   // (111: no mirror)
        if (BeginQuery(ctx, pi, tris, n, t, false)) {
            --g_qBudget;
            if (p.structural & kCutout) g_q[t.q].cov = (float)CutCovDraw(ctx, g_info[p.ps]->scan.discardTex);   // phase 111
        }
    // phase 113b: a thin CANDIDATE's draw (main / eye passes, never a mirror): its key (pair + draw call + pass position + ordinal)
    // -> its own last probe verdict (the tint below), and a probe when it has no fresh verdict (one per pair per Present, the budget)
    int key = -1;
    if (p.thinPre && !g_mirJit) {
        if (!g_keys) g_keys = new (std::nothrow) CensusProbe::KeyTable();
        if (g_keys) {
            key = g_keys->Resolve(pi, ic, a.si, a.bv, instanced ? inst : 0u, instanced ? a.sinst : 0u,
                                 CensusProbe::KeyPos(g_vrMode, CurPassPos()), g_passSeq);   // phase 115r (B3): VR eyes share
            if (key >= 0 && !g_keys->Fresh(key, n) && p.probeAt != n + 1 && ProbeWanted(p, viewNow)) {
                g_probeWant = true;
                if (g_probeLeft > 0 && g_qBudget >= 2 && StartProbe(pi, key, n, t)) { --g_probeLeft; g_qBudget -= 2; p.probeAt = n + 1; }
            }
        }
    }
    // the tint -- ONLY while the census view is on (phase 109r: a dump taken with the view off left its pair tinted for kHiFrames
    // Presents with no key press = random magenta / orange flashes; its picture.png is now untinted). View on: every suspect, or
    // while a dump picture is pending its pair alone. Phase 110: not while a bug report capture runs its sets (TintHeld).
    if (viewNow) {
        // phase 110b: a cut-out pair's draw is orange only when ITS texture is a bad one (a tree PS draws patched and unpatched
        // textures: the pair verdict alone tinted every tree)
        uint32_t k = p.v.Suspect();
        if ((k & kCutout) && (drawCut < 0 || CutClassOf(drawCut) != kCutClsBad))
            k &= ~(uint32_t)kCutout;
        // phase 113b: thin (and unknown-VS, which needs thin) only on the draws whose OWN last probe was thin (a pair drawing big
        // windows and thin sign posts: the posts); a draw not probed yet stays untinted (never tinted on weak evidence)
        if ((k & (kThin | kVs)) && !(g_keys && g_keys->Thin(key)))
            k &= ~(uint32_t)(kThin | kVs);
        // phase 114: a dump picture tints its pair's draws by the SAME per-draw rules as the live view (the evening captures'
        // picture.png of 07dfaa00_6ff10d0c coloured a whole car cyan while the view itself coloured none of its body)
        // phase 115: a pair SAVED by a double-Insert capture this session is not highlighted any more once that capture finished
        // (memory only: the next game start shows it again unless the mod then has a fix) -- its own picture.png above still is
        if (g_hiLeft > 0) { if (pi == g_hiPair && k) t.tint = TintKind(k); }
        else if (k && CapturedHidden(p.capSession, g_cap.active, g_cap.no)) { /* captured this session: not shown */ }
        else if (k && !ShownKinds(k, p.fixOff)) p.fixOffAt = n + 1;   // phase 116: only kinds a switched-off fix covers: not shown
        else if (k) {
            if (g_tintPassSeq != g_passSeq) { g_tintPassSeq = g_passSeq; g_tintThisPass = 0; }   // phase 111: per world pass
            if (g_tintThisPass < TintCapPerPass()) {
                t.tint = TintKind(ShownKinds(k, p.fixOff)); ++g_tintThisFrame; ++g_tintThisPass;   // (116: the colour of the kinds left)
                p.shownAt = n + 1;                        // phase 115: the bug icon's count (shown in the last ~1 s)
            }
            else ++g_cTintCapped;
        }
    }
    // phase 112r: a mirror's G-buffer pass is a G-buffer too (its RT0 is not HDR colour: the albedo RT2 is tinted, as the main one)
    if (t.tint >= 0) { t.pair = pi; t.hdr = !(g_gbufPass || g_mirGbuf); t.mirror = g_mirJit; t.vsl = p.vsLayout; }
}

// before the game's draw (the gates are the caller's: census on, game context, not our work, jittered world / mirror pass)
inline void Before(ID3D11DeviceContext* ctx, const DrawArgs& a, Tok& t) {
    const void* ps = (const void*)g_lodPsCur;
    const uint32_t gen = g_objGen.load(std::memory_order_acquire);
    if (ps != m_ps || gen != m_gen) {
        m_ps = ps; m_gen = gen;
        m_vsMemo = m_pairVs = m_pairPs = (const void*)~(uintptr_t)0;   // a new / re-created shader: the VS + pair memos look again
        m_pairI = -1;
        m_psIdx = LookupObj(ps);
        const ShInfo* s = m_psIdx >= 0 ? g_info[m_psIdx] : nullptr;
        m_psInterest = s && (s->interest || s->thinPair.load(std::memory_order_relaxed));
    }
    ++g_cFast;
    if (m_psIdx < 0) { ++g_cUnknownPs; return; }         // a PS created before the hooks (or the table full): not classified
    if (m_psInterest) { Slow(ctx, a, false, t); return; }
    if (g_qBudget <= 0) return;
    if (++m_thinTick < kThinStride) return;
    m_thinTick = 0;
    Slow(ctx, a, true, t);
}
inline void AfterDraw(ID3D11DeviceContext* ctx, const Tok& t) {
    if (t.q >= 0 && g_q[t.q].q) { t_inDlaa.Quiet(true); ctx->End(g_q[t.q].q); t_inDlaa.Quiet(false); }
}
// phase 113b: the jitter probe of this draw (after the game's draw and the mod's own work on it, the game's state bound): the draw
// re-issued 9 times depth-only into the mod's stencil (CensusProbe::Run; the game's targets / states / viewport / PS come back
// exactly). The viewport's current shift is what ReconcileViewports applied (g_vpShiftX / Y).
template <class F> void Probe(ID3D11DeviceContext* ctx, const Tok& t, F&& redraw) {
    if (t.probe < 0) return;
    ProbeRec& r = g_pr[t.probe];
    if (!r.busy || r.qSum < 0 || r.qAll < 0) return;
    const uint64_t t0 = __rdtsc();
    t_inDlaa.Quiet(true);
    const bool ok = CensusProbe::Run(ctx, g_kit, 2, g_vpShifted ? g_vpShiftX : 0.0f, g_vpShifted ? g_vpShiftY : 0.0f, g_q[r.qSum].q,
                                     g_q[r.qAll].q, redraw,
                                     [ctx](CensusProbe::Kit& k) {   // phase 115r (P5): depth + stencil together = a fast clear (the probe never tests depth)
                                         oClearDSV(ctx, k.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0); });
    t_inDlaa.Quiet(false);
    g_ticksProbe += __rdtsc() - t0;
    if (!ok) {                                            // (multisampled / no target / the stencil buffer could not be made)
        for (const int i : { r.qSum, r.qAll }) { g_q[i].busy = false; g_q[i].probe = -1; g_q[i].role = 0; }
        r.busy = false;
        if (g_cProbeFail++ < 4)
            LogWarn("census (phase 113b): a jitter probe could not run (multisampled or unknown target, or the stencil buffer could "
                    "not be made) -- that draw keeps no verdict");
    }
}
// ---- phase 115: the census MASK (main world passes) + its PAINT at the blit -------------------------------------------------------
// the mask PSs, their code buffers and the paint kit (from the shader cache; false while they compile = the old in-place tint, and
// for good when they cannot be made: one log line)
bool EnsureMaskObjs() {
    if (g_maskOk) return true;
    if (g_maskTried || !g_dev) return false;
    static const ShaderCache::Id kIds[5] = { ShaderCache::kCensusMaskPs0, ShaderCache::kCensusMaskPs1, ShaderCache::kCensusMaskPs2,
                                             ShaderCache::kCensusPaintVs, ShaderCache::kCensusPaintPs };
    for (const ShaderCache::Id id : kIds) if (!ShaderCache::Ready(id)) return false;
    g_maskTried = true;
    const char* bad = nullptr;
    for (int k = 0; k < 3 && !bad; ++k) {
        size_t sz = 0;
        const void* code = ShaderCache::Code(kIds[k], &sz);
        if (!code || FAILED(g_dev->CreatePixelShader(code, sz, nullptr, &g_maskPs[k]))) { g_maskPs[k] = nullptr; bad = ShaderCache::Error(kIds[k]); }
    }
    for (int k = 0; k < kKindN && !bad; ++k)
        for (int c = 0; c < 4 && !bad; ++c) {
            const uint32_t v[4] = { (uint32_t)k + 1u, (uint32_t)c, 0u, 0u };
            D3D11_BUFFER_DESC b{};
            b.ByteWidth = 16; b.Usage = D3D11_USAGE_IMMUTABLE; b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA sd{ v, 0, 0 };
            if (FAILED(g_dev->CreateBuffer(&b, &sd, &g_maskCb[k][c]))) { g_maskCb[k][c] = nullptr; bad = "constant buffer"; }
        }
    if (!bad) {
        size_t vl = 0, pl = 0;
        const void* vc = ShaderCache::Code(ShaderCache::kCensusPaintVs, &vl);
        const void* pc = ShaderCache::Code(ShaderCache::kCensusPaintPs, &pl);
        if (!g_paint.Ensure(g_dev, vc, vl, pc, pl)) bad = "the paint shaders / states";
    }
    if (bad) {
        LogErr("census (phase 115): the census mask / paint objects could not be made (%s) -- the view falls back to the old in-place "
               "tint", bad);
        return false;
    }
    g_maskOk = true;
    Log("census (phase 115): mask + paint objects created -- suspect draws go into a per-pass R32_UINT mask, painted at the blit as a "
        "flashing checkerboard (after DLAA)");
    return true;
}
void ReleaseMasks() {
    for (MaskSlot& m : g_mask) {
        if (m.srv) m.srv->Release();
        if (m.rtv) m.rtv->Release();
        if (m.tex) m.tex->Release();
        m = MaskSlot{};
    }
    g_paint.ReleaseViews();
    for (CensusProbe::Kit& k : g_kit) k.Release();       // phase 115r (B10): the probe's render-size stencil buffers (VR ~2 x 51 MB)
}
// the current pass's mask, (re)made at the bound targets' size and cleared at the pass's first suspect draw; null = no target /
// another size than the pass's first (a draw into a smaller target inside the world pass: not marked)
// phase 115r (audit B9): the mask is sized from the PASS's scene depth (g_sceneW / H), not from its first suspect draw (one into a
// smaller target used to fix the size for the whole pass: every full-size suspect after it was skipped); without a scene depth yet,
// from the draw's targets as before. P5: the bound targets' size is looked up once per (pass, depth view / target 0).
const void* g_mnView = nullptr;
uint64_t    g_mnPass = ~0ull;
UINT        g_mnW = 0, g_mnH = 0;
bool        g_mnOk = false;
MaskSlot* MaskNow(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv, ID3D11RenderTargetView* rtv0) {
    if (g_passSeq == 0) return nullptr;
    const uint64_t s = g_passSeq - 1;
    const void* view = dsv ? (const void*)dsv : (const void*)rtv0;
    if (view != g_mnView || s != g_mnPass) {
        g_mnView = view; g_mnPass = s;
        g_mnOk = CensusProbe::TargetSize(dsv, rtv0, &g_mnW, &g_mnH);
    }
    if (!g_mnOk) return nullptr;
    const UINT tw = g_mnW, th = g_mnH;
    const UINT w = (g_sceneW && g_sceneH) ? g_sceneW : tw, h = (g_sceneW && g_sceneH) ? g_sceneH : th;
    MaskSlot& m = g_mask[SlotOf(s)];
    if (m.pass != s) {
        if (!m.tex || m.w != w || m.h != h) {
            if (m.srv) m.srv->Release();
            if (m.rtv) m.rtv->Release();
            if (m.tex) m.tex->Release();
            m = MaskSlot{};
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R32_UINT;
            td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &m.tex)) || FAILED(g_dev->CreateRenderTargetView(m.tex, nullptr, &m.rtv)) ||
                FAILED(g_dev->CreateShaderResourceView(m.tex, nullptr, &m.srv))) {
                if (m.srv) m.srv->Release();
                if (m.rtv) m.rtv->Release();
                if (m.tex) m.tex->Release();
                m = MaskSlot{};
                if (g_cMaskFail++ < 3) LogWarn("census (phase 115): the %ux%u census mask could not be made -- this pass is not painted", w, h);
                return nullptr;
            }
            m.w = w; m.h = h;
        }
        const FLOAT zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        oClearRTV(ctx, m.rtv, zero);
        m.pass = s;
        m.any = false;
    }
    if (m.w != tw || m.h != th) { ++g_cMaskSkipSize; return nullptr; }   // (a draw into another target size: not marked)
    return &m;
}
// the suspect draw again, into the current pass's mask ALONE (the game's colour targets untouched), with the game's depth buffer
// and its depth-stencil state as a twin (EQUAL for a draw that wrote depth); the mask PS that links to the draw's VS writes the word
// (Census::MaskWord). The game's targets (UAVs kept), PS + class instances, cb13 (+ its D3D11.1 window), blend, depth-stencil come back.
template <class F> bool MaskTint(ID3D11DeviceContext* ctx, const Tok& t, F&& redraw) {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
    bool done = false;
    MaskSlot* const m = MaskNow(ctx, dsv, rtv[0]);
    if (m && m->rtv) {
        ID3D11PixelShader* gps = nullptr;
        ID3D11ClassInstance* gci[8] = {};
        UINT nci = 8;
        ctx->PSGetShader(&gps, gci, &nci);
        if (m_c1Owner != ctx) {
            if (m_c1) { m_c1->Release(); m_c1 = nullptr; }
            ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&m_c1);
            m_c1Owner = ctx;
        }
        ID3D11Buffer* gcb = nullptr;
        UINT gcbFirst = 0, gcbNum = 0;
        if (m_c1) m_c1->PSGetConstantBuffers1(13, 1, &gcb, &gcbFirst, &gcbNum);
        else ctx->PSGetConstantBuffers(13, 1, &gcb);
        ID3D11BlendState* gbs = nullptr; FLOAT bf[4] = {}; UINT mask = 0;
        ctx->OMGetBlendState(&gbs, bf, &mask);
        ID3D11DepthStencilState* gds = nullptr; UINT ref = 0;
        ctx->OMGetDepthStencilState(&gds, &ref);
        bool eq = false;
        int md = kMaskModeNone;
        ID3D11DepthStencilState* const twin = DssTwinOf(gds, &eq, &md);
        const int lay = (t.vsl >= 0 && t.vsl < 3) ? t.vsl : 0;
        const int check = lay != kVsLayoutOther ? md : kMaskModeNone;   // phase 115r (B8): + the test modes (no depth write)
        if (twin && g_maskPs[lay] && g_maskCb[t.tint][check]) {
            // phase 115r (audit B5 / A1): our binds, draw and restore pass the hooks UNTRACKED (as Tint / Probe) -- else
            // hkOMSetDepthStencilState recorded the census twin as the game's state and bound an object-id twin of it (wrong ids)
            const bool wasQuiet = t_inDlaa;
            t_inDlaa.Quiet(true);
            ID3D11RenderTargetView* ours = m->rtv;
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(1, &ours, dsv, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
            ctx->PSSetShader(g_maskPs[lay], nullptr, 0);
            ctx->PSSetConstantBuffers(13, 1, &g_maskCb[t.tint][check]);
            ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);   // (the default: no blending, every channel written)
            ctx->OMSetDepthStencilState(twin, 0);
            const bool timed = g_ts.state == 1 && g_ts.timed < kTsMax;   // phase 110b: the sampled frame's GPU cost
            if (timed) ctx->End(g_ts.b[g_ts.timed]);
            redraw();
            if (timed) ctx->End(g_ts.e[g_ts.timed++]);
            if (g_ts.state == 1) ++g_ts.tints;
            UINT nrt = 0;                                 // (the game's count: a UAV bound after its targets keeps its slot)
            for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) if (rtv[i]) nrt = i + 1;
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(nrt, nrt ? rtv : nullptr, dsv, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
            ctx->PSSetShader(gps, nci ? gci : nullptr, nci);
            if (m_c1 && gcb && gcbNum) m_c1->PSSetConstantBuffers1(13, 1, &gcb, &gcbFirst, &gcbNum);
            else ctx->PSSetConstantBuffers(13, 1, &gcb);
            ctx->OMSetBlendState(gbs, bf, mask);
            ctx->OMSetDepthStencilState(gds, ref);
            t_inDlaa.Quiet(wasQuiet);
            m->any = true;
            ++g_cMaskDraws; ++g_cMaskLayout[lay]; ++g_cTint;
            done = true;
        }
        if (gps) gps->Release();
        for (UINT i = 0; i < nci && i < 8; ++i) if (gci[i]) gci[i]->Release();
        if (gcb) gcb->Release();
        if (gbs) gbs->Release();
        if (gds) gds->Release();
    }
    for (auto*& r : rtv) if (r) { r->Release(); r = nullptr; }
    if (dsv) dsv->Release();
    return done;
}
// at the eye / screen blit (inject.cpp, render thread, game context, our work): the pass's mask painted into the blit source `dst`
// (CensusPaint::Paint). depthSrv = the pass's final depth as R32_FLOAT (its depth snapshot; null = no depth check), (ox, oy) = the
// checker's anchor in dst px (VR: the eye's optical centre).
void PaintAtBlit(ID3D11DeviceContext* ctx, uint64_t passS, ID3D11Texture2D* dst, ID3D11ShaderResourceView* depthSrv, UINT dw, UINT dh,
                 float ox, float oy) {
    if (!dst || !g_maskOk) return;
    MaskSlot& m = g_mask[SlotOf(passS)];
    if (m.pass != passS || !m.any || !m.srv) return;
    const uint64_t t0 = __rdtsc();
    D3D11_TEXTURE2D_DESC td{};
    dst->GetDesc(&td);
    CensusPaint::Params p;
    p.maskW = m.w; p.maskH = m.h; p.dstW = td.Width; p.dstH = td.Height;
    p.cell = PaintCell(td.Width);
    p.phase = g_paintPhase;
    p.depth = depthSrv && dw == m.w && dh == m.h;
    p.ox = ox; p.oy = oy;
    if (CensusPaint::Paint(ctx, g_paint, dst, m.srv, p.depth ? depthSrv : nullptr, p)) ++g_cPaints;
    else if (g_cPaintFail++ < 3)
        LogWarn("census (phase 115): the blit source (%ux%u, format %d, bind 0x%x) cannot be painted -- no checkerboard on this view",
                td.Width, td.Height, (int)td.Format, td.BindFlags);
    g_ticksPaint += __rdtsc() - t0;
}

// at the end of the hook (the game's state is back): the tint re-draw. Phase 115: the main world passes write the census MASK
// (MaskTint, painted at the blit); a mirror pass -- and every pass while the mask objects are not made -- the old in-place tint.
template <class F> void Tint(ID3D11DeviceContext* ctx, const Tok& t, F&& redraw) {
    if (t.tint < 0) return;
    if (!t.mirror && EnsureMaskObjs()) {
        const uint64_t t0 = __rdtsc();
        MaskTint(ctx, t, redraw);
        g_ticksTint += __rdtsc() - t0;
        return;
    }
    if (!EnsureTint()) return;
    // phase 109r: a G-buffer draw tints its ALBEDO (RT2); a forward draw its HDR colour (RT0)
    ID3D11PixelShader* const tps = t.hdr ? g_tintPs : g_tintPsG;
    ID3D11BlendState* const tbs = t.hdr ? g_tintBs : g_tintBsG;
    if (!tps || !tbs) return;
    const uint64_t t0 = __rdtsc();
    ID3D11PixelShader* gps = nullptr;
    ID3D11ClassInstance* gci[8] = {};
    UINT nci = 8;
    ctx->PSGetShader(&gps, gci, &nci);
    // phase 109r: slot 13 with its D3D11.1 window (first / count), so a game ring-buffer window there comes back exactly
    if (m_c1Owner != ctx) {
        if (m_c1) { m_c1->Release(); m_c1 = nullptr; }
        ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&m_c1);
        m_c1Owner = ctx;
    }
    ID3D11Buffer* gcb = nullptr;
    UINT gcbFirst = 0, gcbNum = 0;
    if (m_c1) m_c1->PSGetConstantBuffers1(13, 1, &gcb, &gcbFirst, &gcbNum);
    else ctx->PSGetConstantBuffers(13, 1, &gcb);
    ID3D11BlendState* gbs = nullptr; FLOAT bf[4] = {}; UINT mask = 0;
    ctx->OMGetBlendState(&gbs, bf, &mask);
    ID3D11DepthStencilState* gds = nullptr; UINT ref = 0;
    ctx->OMGetDepthStencilState(&gds, &ref);
    ID3D11DepthStencilState* twin = DssTwinOf(gds);
    if (twin) {
        t_inDlaa.Quiet(true);                            // binds + one draw of ours (VRS stays as the game's draw had it)
        ctx->PSSetShader(tps, nullptr, 0);
        ctx->PSSetConstantBuffers(13, 1, &g_tintCb[t.tint][t.hdr ? 1 : 0]);
        ctx->OMSetBlendState(tbs, nullptr, 0xFFFFFFFFu);
        ctx->OMSetDepthStencilState(twin, 0);
        const bool timed = g_ts.state == 1 && g_ts.timed < kTsMax;   // phase 110b: the sampled frame's GPU cost
        if (timed) ctx->End(g_ts.b[g_ts.timed]);
        redraw();
        if (timed) ctx->End(g_ts.e[g_ts.timed++]);
        if (g_ts.state == 1) ++g_ts.tints;
        ctx->PSSetShader(gps, nci ? gci : nullptr, nci);
        if (m_c1 && gcb && gcbNum) m_c1->PSSetConstantBuffers1(13, 1, &gcb, &gcbFirst, &gcbNum);
        else ctx->PSSetConstantBuffers(13, 1, &gcb);
        ctx->OMSetBlendState(gbs, bf, mask);
        ctx->OMSetDepthStencilState(gds, ref);
        t_inDlaa.Quiet(false);
        ++g_cTint;
    }
    if (gps) gps->Release();
    for (UINT i = 0; i < nci && i < 8; ++i) if (gci[i]) gci[i]->Release();
    if (gcb) gcb->Release();
    if (gbs) gbs->Release();
    if (gds) gds->Release();
    g_ticksTint += __rdtsc() - t0;
}

// ---- the census text --------------------------------------------------------------------------------------------------------------
struct Mark { int no; uint64_t present; double sec; SYSTEMTIME tm; std::vector<int> pairs; std::string pic; };
std::vector<Mark> g_marks;

std::string TimeText(const SYSTEMTIME& t) {
    char b[40];
    snprintf(b, sizeof(b), "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}
// phase 113b: the jitter probe line of a pair
std::string ProbeText(const Pair& p) {
    char b[400];
    const CensusProbe::PairProbe& q = p.probe;
    snprintf(b, sizeof(b), "  jitter probe (phase 113b): %d probe(s) of its draws, %d thin, %d too small (under %.0f px); %.0f %% of its "
             "pixels covered at all 8 jitter offsets (lowest %.0f %%; thin below %.0f %%) -> %s%s\n", q.probes, q.thin, q.tiny,
             CensusProbe::kProbeMinPx, q.Stable() * 100.0, q.probes ? q.sMin * 100.0 : 0.0, CensusProbe::kStableThin * 100.0,
             CensusProbe::VerdictName(q.Verdict()), p.thinPre ? "" : " (not a candidate any more: px per triangle)");
    return b;
}
std::string PairText(const Pair& p, int rank) {
    std::string s;
    char b[640];
    const ShInfo* vsi = p.vs >= 0 ? g_info[p.vs] : nullptr;
    const ShInfo* psi = g_info[p.ps];
    const uint32_t sus = p.v.Suspect();
    const double avgPx = p.samplesAll ? (double)p.pixSumAll / p.samplesAll : 0.0;
    snprintf(b, sizeof(b), "== %s%d %s: %s   (impact %.0f)\n", rank ? "#" : "", rank ? rank : 0, sus ? "SUSPECT" : "known",
             sus ? KindList(sus).c_str() : KindList(p.v.kinds).c_str(), Impact(sus, p.framesSeen, avgPx, p.draws));
    s += b;
    if (sus & p.fixOff) {                                 // phase 116: kinds a fix of the mod covers, but that fix is switched off
        snprintf(b, sizeof(b), "  fix off: %s would be covered by a fix of the mod that is switched off (Picture preset / its row) -- %s\n",
                 KindList(sus & p.fixOff).c_str(), CapWanted(sus, p.fixOff)
                     ? "those kinds are not coloured; the other kinds still are (counted, captured)"
                     : "not coloured, not counted to capture, not captured by the bug report (phase 116)");
        s += b;
    }
    snprintf(b, sizeof(b), "  VS %s  %s\n  PS %s  %s\n", vsi ? Short(vsi->sum).c_str() : "--------",
             vsi ? ConstText(vsi->sum).c_str() : "(created before the mod's hooks: no checksum)", Short(psi->sum).c_str(),
             ConstText(psi->sum).c_str());
    s += b;
    snprintf(b, sizeof(b), "  kinds %s | covered %s | water_ps rule %d, pixel-reader twin %s, wire PS %d (wire VS %s), solid PS kind %d (VS in its set %s)%s%s\n",
             KindList(p.v.kinds).c_str(), KindList(p.v.covered).c_str(), p.cover.waterRule, p.cover.pixelFix ? "yes" : "no", p.cover.wirePsKind,
             p.cover.wireVs ? "yes" : "no", p.cover.solidKind, p.cover.solidVs ? "yes" : "no",
             (p.structural & kCutout) ? " | cut-out alpha: " : "", (p.structural & kCutout) ? CutReasonName(p.cover.cutReason) : "");
    s += b;
    if (p.structural & kCutout) {                         // phase 110b: the textures it really drew (checked draws per class)
        snprintf(b, sizeof(b), "  cut-out textures (checked draws): OK %llu (last: %s), waiting %llu (last: %s), BAD %llu (last: %s)%s\n",
                 (unsigned long long)p.cut.checks[0], p.cut.at[0] ? CutReasonName(p.cut.why[0]) : "-", (unsigned long long)p.cut.checks[1],
                 p.cut.at[1] ? CutReasonName(p.cut.why[1]) : "-", (unsigned long long)p.cut.checks[2],
                 p.cut.at[2] ? CutReasonName(p.cut.why[2]) : "-",
                 CutClassOf(p.cover.cutReason) == kCutClsBad ? " -- a BAD texture was drawn in the last 300 Presents" : "");
        s += b;
    }
    snprintf(b, sizeof(b), "  PS scan: ld at the interpolated pixel %d (slots 0x%x), projective samples %d, scrolling samples %d, discards %d "
             "(alpha slots 0x%x), parity %d\n", psi->scan.pixelLds, psi->scan.pixelTex, psi->scan.projSamples, psi->scan.scrollSamples,
             psi->scan.discards, psi->scan.discardTex, (int)psi->scan.parity);
    s += b;
    snprintf(b, sizeof(b), "  first seen %s (+%.1f s), Present #%llu, %s%s%s pass, viewport %.0fx%.0f | camera world position: not "
             "available (the game sends camera-relative matrices only)\n", TimeText(p.firstTime).c_str(), p.firstSec,
             (unsigned long long)p.firstPresent, (p.passMask & 1) ? "G-buffer " : "", (p.passMask & 2) ? "forward " : "",
             (p.passMask & 8) ? "mirror (G-buffer) " : ((p.passMask & 4) ? "mirror " : ""), (double)p.vpW, (double)p.vpH);
    s += b;
    if (g_vrMode || (p.posMask & 4)) {                     // phase 111
        snprintf(b, sizeof(b), "  world passes drawn in: %s%s%s (VR: one pass per eye; draws below count both eyes)\n",
                 (p.posMask & 1) ? "1st " : "", (p.posMask & 2) ? "2nd " : "", (p.posMask & 4) ? "later " : "");
        s += b;
    }
    snprintf(b, sizeof(b), "  frames seen %llu, draws %llu (%.1f per frame), IndexCount %u..%u, instanced %s%s, non-instanced %s\n",
             (unsigned long long)p.framesSeen, (unsigned long long)p.draws, p.framesSeen ? (double)p.draws / (double)p.framesSeen : 0.0,
             p.icMin == 0xFFFFFFFFu ? 0u : p.icMin, p.icMax, p.inst ? "yes" : "no", p.inst ? (" (max " + std::to_string(p.instMax) + " instances)").c_str() : "",
             p.nonInst ? "yes" : "no");
    s += b;
    if (p.samplesAll)
        snprintf(b, sizeof(b), "  screen pixels (occlusion samples %d): avg %.0f, max %u | thin samples %d: %.2f px per triangle (thin below "
                 "%.2f%s)\n", p.samplesAll, avgPx, p.pixMax, p.samples, p.triSum ? (double)p.pixSum / (double)p.triSum : 0.0, kThinPxPerTri,
                 p.covSamples ? (", " + std::to_string(p.covSamples) + " cut-out sample(s) divided by the alpha coverage, phase 111").c_str() : "");
    else snprintf(b, sizeof(b), "  screen pixels: not sampled yet\n");
    s += b;
    if (p.thinPre || p.probe.probes || p.probe.tiny) s += ProbeText(p);   // phase 113b
    if (p.state) { s += "  state at first sight:\n"; s += *p.state; }
    if (!p.where.empty()) { s += "  dump: "; s += p.where; s += "\n"; }   // phase 110: the last full dump (census_dump or a capture)
    else if (p.dump == 1) s += "  dump: queued (starts at its next draw, one every 2 s)\n";
    else if (p.dump == -1) s += "  dump: not made (cap: 200 dumps / census_max_mb)\n";
    if (!p.marks.empty()) {
        s += "  marks:";
        for (int m : p.marks) { snprintf(b, sizeof(b), " %d", m); s += b; }
        s += "\n";
    }
    if (p.capSession > 0) {                               // phase 115
        snprintf(b, sizeof(b), "  captured this session (bug report capture %d, %s): not highlighted again until the next game start\n",
                 p.capSession, p.where.empty() ? "no full dump folder" : p.where.c_str());
        s += b;
    }
    return s;
}
struct Snapshot {
    std::vector<Pair> pairs;
    std::vector<Mark> marks;
    std::string head;
};
std::string FormatCensus(const Snapshot& sn) {
    std::vector<int> sus, known;
    for (int i = 0; i < (int)sn.pairs.size(); ++i) {
        const Pair& p = sn.pairs[i];
        if (p.v.Suspect()) sus.push_back(i); else if (p.v.kinds) known.push_back(i);
    }
    auto imp = [&](int i) {
        const Pair& p = sn.pairs[i];
        return Impact(p.v.Suspect(), p.framesSeen, p.samplesAll ? (double)p.pixSumAll / p.samplesAll : 0.0, p.draws);
    };
    std::stable_sort(sus.begin(), sus.end(), [&](int a, int b) { return imp(a) > imp(b); });
    std::string s = sn.head;
    char b[256];
    size_t capd = 0;                                      // phase 115: saved by a double-Insert capture this session
    for (const Pair& p : sn.pairs) if (p.capSession > 0) ++capd;
    size_t offOnly = 0;                                   // phase 116: suspects whose only kinds a switched-off fix covers
    for (int i : sus) if (!CapWanted(sn.pairs[i].v.Suspect(), sn.pairs[i].fixOff)) ++offOnly;
    snprintf(b, sizeof(b), "# suspects %zu (%zu of them only because a fix is switched off: not coloured, see \"fix off\"), known (covered) "
             "pairs with a kind %zu, marks %zu, captured this session %zu (not highlighted until the next start; listed below)\n\n",
             sus.size(), offOnly, known.size(), sn.marks.size(), capd);
    s += b;
    std::vector<int> rankOf(sn.pairs.size(), 0);
    for (size_t r = 0; r < sus.size(); ++r) rankOf[sus[r]] = (int)r + 1;
    for (size_t r = 0; r < sus.size(); ++r) { s += PairText(sn.pairs[sus[r]], (int)r + 1); s += "\n"; }
    if (!known.empty()) {
        s += "== KNOWN pairs (every kind they have is covered by a mod feature that is ON, or a cut-out whose drawn textures are OK / "
             "waiting)\n";
        for (int i : known) {
            const Pair& p = sn.pairs[i];
            snprintf(b, sizeof(b), "  VS %s PS %s  kinds %s  frames %llu draws %llu%s%s\n", p.vs >= 0 ? Short(g_info[p.vs]->sum).c_str() : "--------",
                     Short(g_info[p.ps]->sum).c_str(), KindList(p.v.kinds).c_str(), (unsigned long long)p.framesSeen,
                     (unsigned long long)p.draws, (p.v.kinds & kCutout) ? " | cut-out alpha: " : "",
                     (p.v.kinds & kCutout) ? CutReasonName(p.cover.cutReason) : "");
            s += b;
            if (p.cover.knownPair >= 0) {                 // phase 111: the known list's reason
                int kn = 0;
                const KnownPair* kp = KnownPairs(&kn);
                if (p.cover.knownPair < kn) { s += "    known pair (phase 111): "; s += kp[p.cover.knownPair].what; s += "\n"; }
            }
        }
        s += "\n";
    }
    {   // phase 113b: the px-per-triangle candidates the jitter probe cleared or is still measuring (never tinted, no suspect)
        std::vector<int> cand;
        for (int i = 0; i < (int)sn.pairs.size(); ++i)
            if (sn.pairs[i].thinPre && !sn.pairs[i].thin) cand.push_back(i);
        if (!cand.empty()) {
            s += "== THIN CANDIDATES the jitter probe cleared or is still measuring (phase 113b: under 0.50 px per triangle, but their "
                 "coverage does not flicker under the jitter -- dense meshes -- or not enough probes yet; not tinted)\n";
            for (int i : cand) {
                const Pair& p = sn.pairs[i];
                const CensusProbe::PairProbe& q = p.probe;
                snprintf(b, sizeof(b), "  VS %s PS %s  %s: %d probe(s), %d thin, %d too small, %.0f %% stable | %.2f px per triangle, "
                         "frames %llu%s\n", p.vs >= 0 ? Short(g_info[p.vs]->sum).c_str() : "--------", Short(g_info[p.ps]->sum).c_str(),
                         CensusProbe::VerdictName(q.Verdict()), q.probes, q.thin, q.tiny, q.Stable() * 100.0,
                         p.triSum ? (double)p.pixSum / (double)p.triSum : 0.0, (unsigned long long)p.framesSeen,
                         p.cover.knownPair >= 0 ? " | on the known list (phase 111 / 113)" : "");
                s += b;
            }
            s += "\n";
        }
    }
    if (capd) {                                           // phase 115
        s += "== CAPTURED THIS SESSION (a double-Insert bug report saved them -- dumped or linked; the live view stops flashing them "
             "until the next game start, then they show again unless the mod has a fix for them)\n";
        for (const Pair& p : sn.pairs) {
            if (p.capSession <= 0) continue;
            snprintf(b, sizeof(b), "  VS %s PS %s  kinds %s  capture %d  %s\n", p.vs >= 0 ? Short(g_info[p.vs]->sum).c_str() : "--------",
                     Short(g_info[p.ps]->sum).c_str(), KindList(p.v.Suspect() ? p.v.Suspect() : p.v.kinds).c_str(), p.capSession,
                     p.where.empty() ? "" : p.where.c_str());
            s += b;
        }
        s += "\n";
    }
    if (!sn.marks.empty()) {
        s += "== MARKS\n";
        for (const Mark& m : sn.marks) {
            snprintf(b, sizeof(b), "  MARK %d at %s (+%.1f s), Present #%llu, camera world position not available; picture %s; suspects on screen:",
                     m.no, TimeText(m.tm).c_str(), m.sec, (unsigned long long)m.present, m.pic.c_str());
            s += b;
            if (m.pairs.empty()) s += " none";
            for (int pi : m.pairs) {
                if (pi < 0 || pi >= (int)sn.pairs.size()) continue;
                snprintf(b, sizeof(b), " #%d (%s/%s %s)", rankOf[pi], sn.pairs[pi].vs >= 0 ? Short(g_info[sn.pairs[pi].vs]->sum).c_str() : "--------",
                         Short(g_info[sn.pairs[pi].ps]->sum).c_str(), KindList(sn.pairs[pi].v.Suspect()).c_str());
                s += b;
            }
            s += "\n";
        }
    }
    return s;
}
std::string HeadText(const char* why) {
    std::string s;
    char b[6144];                                         // phase 110b: 1024 cut the KINDS line short (the 2026-11-15 file); 115: 6144
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(b, sizeof(b),
             "# dlaa_census.txt -- phase 108 shader census, ETS2 / ATS DLAA mod v%s, written %s (%s, overwritten every 60 s, %.0f s after a new suspect and at a mark)\n"
             "# mode %s, jitter_overscan %d, cutout_mips %d, wire_widen %d, water_overscan_fix %d, pole_aa %d, power_line_fix %d, "
             "pixel_reader_fix %d | Presents %llu, census time %.0f s\n"
             "# draws looked at %llu (slow path %llu), draws with a PS created before the hooks %llu, shaders noted PS %u / VS %u, "
             "distinct bytecodes %d, pairs %zu (table full %d), occlusion samples %llu, tinted draws %llu, jitter probes %llu (failed "
             "%llu, lost %llu)\n"
             "# dumps started %d (refused by the cap %d), files written %d (failed %d), %.1f MB on disk (census_max_mb %d)\n"
             "# KINDS: pixel-reader = ld at a pixel index computed from an interpolant (vN.yz / vN.x * cb -> ftoi): one texel off under "
             "the jitter overscan | cut-out = discard on a texture alpha, and a texture it DREW in the last 300 Presents is unhandled "
             "AND may lose coverage (no phase-77 analysis: cutout_mips 0 / not BC3 / shape rule / cannot be patched (not DEFAULT usage); or a "
             "sparse alpha under 2 %% whose real mips were not judged; phase 114: a BC1 WITH see-through texels -- an opaque BC1, a "
             "render target the game draws and a texture with one mip are OK) "
             "| thin = < %.2f px per "
             "triangle (sampled occlusion queries: the candidates) AND the jitter probe (phase 113b: the draw re-drawn depth-only at the "
             "8 jitter offsets; thin = under %.0f %% of its pixels covered at all 8, on at least %d probes of %d+ and %.0f %% of them; only "
             "the draws whose own probe was thin are tinted) | water-like = projective texture lookup + a G-buffer read / scrolling samples | "
             "unknown-VS = a known wire / solid PS with a VS outside its set\n"
             "# KNOWN (covered) per kind, only while that fix is ON: pixel-reader / water-like by phase 97 (water_overscan_fix) or "
             "109a (pixel_reader_fix), thin by wire_widen's pairs (phases 61-66, 107b, 109e), pole_aa (109d) or power_line_fix (109b); "
             "cut-out (phase 110b) when its drawn textures are patched / applied / no loss / soft alpha / coverage over 60 %% (the "
             "game's mips keep those) or waiting (pending / worker queue full / phase 113: never seen by the uploader -> read back from "
             "the GPU: re-checked, never tinted). Constants below are in "
             "wire_widen.cpp's ChecksumIs format.\n"
             "# Sorted by impact = kind weight x frames seen x (1 + avg pixels / 1000). Live view: %s; mark: %s.\n"
             "# Phase 115: the live view PAINTS the suspects after DLAA as a flashing checkerboard (kind colour / black, 4 swaps a second); "
             "a pair a double-Insert capture saved this session is not highlighted again until the next game start (CAPTURED THIS "
             "SESSION below).\n",
             DLAA_INJECTOR_VERSION, TimeText(t).c_str(), why, kSoonSecs, g_vrMode ? "VR" : (g_flatMode ? "flat" : "undetected"), g_jitterOverscan,
             g_cutoutMips, (int)(g_wireCfg && g_wireOn.load()), g_waterFixCfg, g_poleCfg, g_powerLineCfg, g_prFixCfg,
             (unsigned long long)g_frames.load(std::memory_order_relaxed), NowSec(),
             (unsigned long long)g_cFast, (unsigned long long)g_cSlow, (unsigned long long)g_cUnknownPs, g_psNoted.load(), g_vsNoted.load(),
             g_infoN.load(), g_pairs.size(), g_pairFull, (unsigned long long)g_cQueries, (unsigned long long)g_cTint,
             (unsigned long long)g_cProbes, (unsigned long long)g_cProbeFail, (unsigned long long)g_cProbeLost, g_dumpsStarted,
             g_dumpsRefused, g_filesOk.load(), g_filesFail.load(), (double)g_diskBytes / (1024.0 * 1024.0), g_censusMaxMb,
             kThinPxPerTri, CensusProbe::kStableThin * 100.0, CensusProbe::kPairThinMin, CensusProbe::kPairProbesMin,
             CensusProbe::kPairThinShare * 100.0, KeyName(KA_CENSUS_VIEW), KeyName(KA_CENSUS_MARK));
    s += b;
    return s;
}
Snapshot TakeSnapshot(const char* why) {
    Snapshot sn;
    sn.pairs = g_pairs;                                   // (strings are shared_ptr: a copy of the POD + pointers)
    sn.marks = g_marks;
    sn.head = HeadText(why);
    return sn;
}
void WriteCensusFile(const Snapshot& sn) {
    wchar_t path[MAX_PATH], tmp[MAX_PATH];
    if (!PathNextToDll(L"dlaa_census.txt", path) || !PathNextToDll(L"dlaa_census.txt.tmp", tmp)) return;
    const std::string txt = FormatCensus(sn);
    if (WriteFileAll(tmp, txt.data(), txt.size())) MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

// ---- phase 110: the capture's census part, per Present (after the query results and the grab pump) ------------------------------
std::string CapHeadText(uint64_t n) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[3072];                                         // (phase 111: 2400 -> 3072, the picture-source line)
    snprintf(b, sizeof(b),
             "# census\\index.txt -- phase 110: the shader census part of this bug report (ETS2 / ATS DLAA mod v%s), written %s\n"
             "# The capture key was pressed at %s, Present #%llu, with the census view ON (MARK %d in dlaa_census.txt). The ARMED frames\n"
             "# Present #%llu..#%llu (the frames right after the key) were searched: every suspect pair drawn there is listed below.\n"
             "# Pairs %zu: dumped here %d, linked to an earlier dump %d, text only (budget) %d; %.1f MB of textures / buffers staged.\n"
             "# Pictures: %s.\n"
             "# FILES: tinted.png = the frame after the key, every suspect tinted (magenta pixel-reader, orange cut-out, cyan thin, blue\n"
             "#   water-like, yellow unknown VS). <vs8>_<ps8>\\ per pair: draw.txt (the census entry, WHY it is a suspect, the state at the\n"
             "#   draw: blend / depth-stencil / rasterizer / topology, every PS texture slot + samplers, the draw call, viewports, scissors,\n"
             "#   render targets + depth target, input layout elements, vertex / index buffers, VS textures, every PS / VS constant buffer\n"
             "#   window), vs.dxbc / ps.dxbc + vs.txt / ps.txt (D3DDisassemble), tex_tN.dds (the PS's textures, DX10 DDS, every mip),\n"
             "#   ps_cbN.bin / vs_cbN.bin (the bound constant-buffer windows, b0..b7), picture.png (that pair ALONE tinted, taken after\n"
             "#   the capture's own frames -- empty when it had left the screen). dlaa_census.txt = the whole census at this moment.\n"
             "# Coverage = occlusion-query pixels summed over every draw of the pair in the armed frames (render size; VR: both eyes).\n"
             "# Pictures (tinted.png, picture.png) are taken from: %s.\n"
             "# Camera world position: not available (the game sends camera-relative matrices only).\n",
             DLAA_INJECTOR_VERSION, TimeText(t).c_str(), TimeText(g_cap.keyTime).c_str(), (unsigned long long)g_cap.key, g_cap.mark,
             (unsigned long long)g_cap.armFrom, (unsigned long long)g_cap.armTo, g_cap.pairs.size(), g_cap.dumped, g_cap.linked,
             g_cap.textOnly, (double)g_cap.staged / 1048576.0,
             g_cap.picWhy ? g_cap.picWhy : "one per pair, the biggest on screen first (at most 16), each with the pair alone tinted",
             g_picSource ? g_picSource : (g_vrMode ? "VR eye 0 (the RIGHT eye) after DLAA, at its blit" : "the presented back buffer"));
    (void)n;
    return b;
}
void CapTick(IDXGISwapChain* sc, uint64_t n) {
    if (!g_cap.active) return;
    const bool late = n > g_cap.key + kCapDeadline;
    if (g_cap.stage == 0) {
        if (n <= g_cap.armTo) return;
        g_cap.stage = 1;
        Log("census (phase 110): capture part -- %zu suspect pair(s) in the armed frames (dumped %d, linked %d, text only %d; %.1f MB "
            "staged)", g_cap.pairs.size(), g_cap.dumped, g_cap.linked, g_cap.textOnly, (double)g_cap.staged / 1048576.0);
    }
    if (g_cap.stage == 1) {                               // the coverage queries + the capture's own frames (they are untinted)
        bool qBusy = false;
        for (const Query& q : g_q) if (q.busy && q.cap && q.capNo == g_cap.no) qBusy = true;
        // (phase 112r: + the capture not taken up yet -- held until it starts or OnPresent's kCapStartWait cancels this part)
        if (!late && ((qBusy && n <= g_cap.armTo + 120) || TintHeld() || g_selfTestRequest.load(std::memory_order_relaxed))) return;
        std::vector<CapRank> rk;
        for (int pi : g_cap.pairs) {
            const Pair& p = g_pairs[pi];
            rk.push_back({ pi, p.capPx, Impact(p.v.Suspect(), p.framesSeen, p.samplesAll ? (double)p.pixSumAll / p.samplesAll : 0.0, p.draws),
                           p.capDraws });
        }
        g_cap.pics = CapPictureOrder(rk, kCapMaxPictures);
        if (late) g_cap.picWhy = "not taken (the census part timed out)";
        else if (!sc && (!g_vrMode || g_picEyeBad)) g_cap.picWhy = "not taken (present-layer mode: no back buffer at Present)";   // (111: VR = eye 0)
        else if (!g_view.load(std::memory_order_relaxed)) g_cap.picWhy = "not taken (the census view was switched off)";
        if (g_cap.picWhy) g_cap.pics.clear();
        g_cap.picNext = 0;
        g_cap.stage = 2;
    }
    if (g_cap.stage == 2) {                               // one picture per pair, the pair alone tinted (kHiFrames Presents each)
        if (late) { g_cap.picNext = g_cap.pics.size(); if (!g_cap.picWhy) g_cap.picWhy = "partly taken (the census part timed out)"; }
        else {
            if (g_hiLeft > 0 || g_picBusy || !g_picReq.empty()) return;   // (a picture in flight: this capture's or a census_dump one)
            if (g_cap.picNext < g_cap.pics.size() && !g_view.load(std::memory_order_relaxed)) {
                g_cap.picWhy = "partly taken (the census view was switched off)";
                g_cap.picNext = g_cap.pics.size();
            }
            if (g_cap.picNext < g_cap.pics.size()) {
                const int pi = g_cap.pics[g_cap.picNext++];
                Pair& p = g_pairs[pi];
                g_hiPair = pi;
                g_hiLeft = kHiFrames;
                p.capPicPresent = n + kHiFrames - 2;
                g_picReq.push_back({ g_cap.dir + Widen(p.dumpDir) + L"\\picture.png", n + kHiFrames - 2 });
                return;
            }
        }
        g_hiLeft = 0;
        g_cap.stage = 3;
    }
    if (g_cap.stage == 3) {                               // every grab of the capture mapped = queued on the writer before the index
        bool waiting = false;
        for (const Grab& g : g_grabs) if (g.cap && !g.mapped) waiting = true;
        if (n <= g_cap.key + kCapTexRetry)                // a deferred texture may still come (CapLateTextures): before the index
            for (int pi : g_cap.pairs) if (g_pairs[pi].capTexLeft) waiting = true;
        if (waiting && !late) return;
        std::vector<CapRow> rows;
        for (int pi : g_cap.pairs) {
            const Pair& p = g_pairs[pi];
            const ShInfo* vsi = p.vs >= 0 ? g_info[p.vs] : nullptr;
            CapRow r;
            r.folder = p.dumpDir;
            r.vs = vsi ? ConstText(vsi->sum) : "";
            r.ps = ConstText(g_info[p.ps]->sum);
            r.kinds = KindList(p.v.Suspect());
            r.action = p.capAction;
            if (p.capAction == kCapLink) r.link = p.where;
            r.px = p.capPx; r.pxQueries = p.capQueries; r.draws = p.capDraws;
            char b[200];
            if (p.capPicPresent) snprintf(b, sizeof(b), "picture.png (Present #%llu, %llu Presents after the key; the pair alone tinted -- "
                                          "empty if it had left the screen)", (unsigned long long)p.capPicPresent,
                                          (unsigned long long)(p.capPicPresent - g_cap.key));
            else snprintf(b, sizeof(b), "%s", g_cap.picWhy ? g_cap.picWhy : "not taken (only the 16 biggest pairs get one)");
            r.picture = b;
            r.why = WhyText(p.v, p.cover, g_info[p.ps]->scan, p.triSum ? (double)p.pixSum / (double)p.triSum : 0.0, p.samples,
                            p.probe.probes, p.probe.thin, p.probe.Stable());
            r.tex = p.capTex;
            if (p.capTexLeft) { snprintf(b, sizeof(b), "  (t-slots 0x%x never grabbed: the staging stayed full)\n", p.capTexLeft); r.tex += b; }
            rows.push_back(std::move(r));
        }
        const std::string idx = CapIndexText(CapHeadText(n), rows);
        auto sn = std::make_shared<Snapshot>(TakeSnapshot("bug report capture"));
        g_cap.indexDone = std::make_shared<std::atomic<int>>(0);
        auto done = g_cap.indexDone;
        const std::wstring dir = g_cap.dir;
        if (!Queue([dir, idx, sn, done] {
                MakeDirs(dir);
                (WriteFileAll(dir + L"index.txt", idx.data(), idx.size()) ? g_filesOk : g_filesFail).fetch_add(1);
                const std::string all = FormatCensus(*sn);
                (WriteFileAll(dir + L"dlaa_census.txt", all.data(), all.size()) ? g_filesOk : g_filesFail).fetch_add(1);
                done->store(1, std::memory_order_release);
            }))
            done->store(1, std::memory_order_release);
        g_cap.stage = 4;
        return;
    }
    if (g_cap.stage == 4) {
        if (!g_cap.indexDone->load(std::memory_order_acquire) && !late) return;
        g_cap.active = false;
        Log("census (phase 110): capture part done -> %s (%zu pair(s), index.txt, dlaa_census.txt%s) -- the bug report zip follows",
            g_cap.rel.c_str(), g_cap.pairs.size(), g_cap.picWhy ? "" : ", pictures");
    }
}

// ---- Present --------------------------------------------------------------------------------------------------------------------
bool   g_firstLine = false;
// phase 110b: the sampled tint GPU cost (render thread, at Present: the game's context; t_inDlaa quiet)
void TsTick(ID3D11DeviceContext* ctx, uint64_t n) {
    if (g_ts.state == 1) { ctx->End(g_ts.dis); g_ts.state = 2; g_ts.at = n; return; }
    if (g_ts.state == 2) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        const HRESULT hr = ctx->GetData(g_ts.dis, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) {
            g_ts.state = 0;
            if (dj.Disjoint || !dj.Frequency) { ++g_tsDisjoint; return; }
            double ticks = 0.0;
            for (int i = 0; i < g_ts.timed; ++i) {
                UINT64 b = 0, e = 0;
                if (ctx->GetData(g_ts.b[i], &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
                    ctx->GetData(g_ts.e[i], &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) { ++g_tsDisjoint; return; }
                if (e > b) ticks += (double)(e - b);
            }
            double ms = ticks * 1000.0 / (double)dj.Frequency;
            if (g_ts.timed > 0 && g_ts.tints > g_ts.timed) ms *= (double)g_ts.tints / (double)g_ts.timed;
            g_tsMsSum += ms; ++g_tsFrames; g_tsLastMs = ms;
        } else if (hr != S_FALSE || n > g_ts.at + 120) g_ts.state = 0;
        return;
    }
    if (!g_view.load(std::memory_order_relaxed) || TintHeld() || n % kTsEvery != 0) return;
    if (!g_ts.tried) {                                    // once: 1 disjoint + 2 x kTsMax timestamp queries
        g_ts.tried = true;
        D3D11_QUERY_DESC dd{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, td{ D3D11_QUERY_TIMESTAMP, 0 };
        g_ts.ok = SUCCEEDED(g_dev->CreateQuery(&dd, &g_ts.dis));
        for (int i = 0; i < kTsMax && g_ts.ok; ++i)
            g_ts.ok = SUCCEEDED(g_dev->CreateQuery(&td, &g_ts.b[i])) && SUCCEEDED(g_dev->CreateQuery(&td, &g_ts.e[i]));
        if (!g_ts.ok) LogWarn("census (phase 110b): timestamp queries could not be created -- the tint GPU cost is not measured");
    }
    if (!g_ts.ok) return;
    ctx->Begin(g_ts.dis);
    g_ts.state = 1; g_ts.timed = 0; g_ts.tints = 0;
}
// phase 113b: one of a probe's two query results (ok = read; else lost). With both: the pair's probes + the draw's own verdict.
void ProbeResult(Query& q, uint64_t px, bool ok, uint64_t n) {
    const int ri = q.probe;
    const int role = q.role;
    q.probe = -1; q.role = 0;
    if (ri < 0 || ri >= kProbeRing) return;
    ProbeRec& r = g_pr[ri];
    if (!r.busy) return;
    if (!ok) r.lost = true;
    else if (role == 1) r.r.sum8 = px;
    else r.r.all = px;
    r.got |= role;
    if (r.got != 3) return;
    r.busy = false;
    if (r.lost) { ++g_cProbeLost; return; }
    if (r.pair < 0 || r.pair >= (int)g_pairs.size()) return;
    Pair& p = g_pairs[r.pair];
    p.probe.Add(r.r);
    if (g_keys) g_keys->Note(r.slot, r.key, r.r, n);
    UpdateThin(p);
}
void OnPresent(IDXGISwapChain* sc, uint64_t n) {
    ID3D11DeviceContext* ctx = g_gameCtx.load(std::memory_order_relaxed);
    if (!ctx || !g_dev) { if (g_cap.active && n > g_cap.armTo + 120) g_cap.active = false; return; }   // (never drew: nothing to wait for)
    {                                                     // a new game device: our objects belong to the old one -> census off
        ID3D11Device* d = nullptr;
        ctx->GetDevice(&d);
        if (d) d->Release();
        if (d != g_dev) {
            g_devBad = true;
            g_censusOn = false;
            g_view = false;
            if (g_cap.active) CapUnsave(g_cap.no);        // phase 115r (B7)
            g_cap.active = false;                         // (phase 110: the bug report does not wait for it)
            ReleaseMasks();                               // phase 115 (the old device's textures; the shaders stay unused)
            delete g_keys;                                // phase 115r (B10): the probe verdicts of the old device's draws
            g_keys = nullptr;
            LogWarn("census (phase 108): the game's device changed -- the census stops for this session (the file keeps what it has)");
            return;
        }
    }
    // phase 112r: the capture key armed this census part but the capture itself was never taken up (no eye blit / tonemap point
    // ran SelfTestStep): stop waiting for it (CensusCancelCapture: SelfTestStep's own refusals)
    if (g_cap.active && g_cap.stage <= 1 && g_selfTestRequest.load(std::memory_order_relaxed) && n > g_cap.key + kCapStartWait) {
        g_cap.active = false;
        CapUnsave(g_cap.no);                              // phase 115r (B7): nothing saved -> highlighted again
        Cap::g_nextSub[0] = 0;
        Log("census (phase 112r): the bug report capture did not start within %llu Presents -- its census part is cancelled "
            "(Present #%llu)", (unsigned long long)kCapStartWait, (unsigned long long)n);
    }
    const uint64_t t0 = __rdtsc();
    ++g_presentsCounted;
    g_qBudget = kQueriesPerPresent;
    g_probeLeft = g_probeWant ? kProbesPerPresent : 0;    // phase 113b: reserved only while a draw waits for a probe
    g_probeWant = false;
    g_tintThisFrame = 0;
    {   // phase 115: the checker's phase (one value for both VR eyes of the next frame) + the bug icon's count, a few times a second
        const double now = NowSec();
        g_paintPhase = PaintPhase(now);
        g_presentSec[n & 255] = now;
        if (now >= g_bugNext) {
            g_bugNext = now + kBugEvery;
            uint64_t fromN = n + 1;                       // the first Present (+ 1) of the last kBugWindowSecs (<= 255 Presents)
            for (uint64_t k = 0; k < 255 && k <= n; ++k) {
                if (g_presentSec[(n - k) & 255] < now - kBugWindowSecs) break;
                fromN = n - k + 1;
            }
            static std::vector<uint64_t> shown, off;      // (116: kept between counts -- no allocation once grown)
            static std::vector<int> cs;
            shown.resize(g_pairs.size()); off.resize(g_pairs.size()); cs.resize(g_pairs.size());
            for (size_t i = 0; i < g_pairs.size(); ++i) {
                const Pair& p = g_pairs[i];
                // (a pair a fix now covers is no longer "to capture"; 116: nor one whose only kinds a switched-off fix covers)
                shown[i] = CapWanted(p.v.Suspect(), p.fixOff) ? p.shownAt : 0;
                off[i] = (p.v.Suspect() & p.fixOff) ? p.fixOffAt : 0;   // (116: the fix switched on since: not counted)
                cs[i] = p.capSession;
            }
            const BugCount bc = CountBug(shown.data(), cs.data(), shown.size(), fromN, off.data());
            g_bugToCapture = bc.toCapture;
            g_bugCaptured = bc.captured;
            g_bugFixOff = bc.fixOff;
        }
    }
    TsTick(ctx, n);                                       // phase 110b: the sampled tint GPU cost
    {   // phase 110b: a fix switched live (wire_widen Alt+F12, the pole_aa row, ...) -> every pair is judged again with it
        const uint32_t m = FixMaskNow();
        if (m != g_fixMask) {
            const bool first = g_fixMask == 0xFFFFFFFFu;
            g_fixMask = m;
            for (Pair& p : g_pairs) { ApplyFixes(p.cover, m); Rejudge(p); }
            if (!first)
                Log("census (phase 110b): a fix was switched (wire_widen %d, pole_aa %d, power_line_fix %d, pixel_reader_fix %d, "
                    "water_overscan_fix %d, jitter_overscan %d) -- %zu pairs judged again (Present #%llu)", (int)(m & 1u), (int)((m >> 1) & 1u),
                    (int)((m >> 2) & 1u), (int)((m >> 3) & 1u), (int)((m >> 4) & 1u), (int)((m >> 5) & 1u), g_pairs.size(),
                    (unsigned long long)n);
        }
    }
    if (n % 30 == 0)                                      // phase 110b: a cut-out pair whose bad texture left kCutWindow ago
        for (Pair& p : g_pairs)
            if (p.structural & kCutout) {
                const int shown = p.cut.Shown(n);
                if (shown != p.cover.cutReason && (p.cut.at[0] | p.cut.at[1] | p.cut.at[2])) { p.cover.cutReason = shown; Rejudge(p); }
            }
    // occlusion results
    for (Query& q : g_q) {
        if (!q.busy || q.issued + 2 > n) continue;
        UINT64 px = 0;
        const HRESULT hr = ctx->GetData(q.q, &px, sizeof(px), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK) {
            q.busy = false;
            if (q.probe >= 0) { ProbeResult(q, px, true, n); continue; }   // phase 113b
            if (q.pair < 0 || q.pair >= (int)g_pairs.size()) continue;
            Pair& p = g_pairs[q.pair];
            if (q.cap) {                                  // phase 110: a capture's coverage query (every draw: kept out of the samples)
                if (q.capNo == p.capNo) { p.capPx += px; ++p.capQueries; }
                continue;
            }
            ++p.samplesAll;
            p.pixSumAll += px;
            if (px > p.pixMax) p.pixMax = (uint32_t)(px > 0xFFFFFFFFull ? 0xFFFFFFFFull : px);
            if (px > 0 && q.tris >= kThinMinTris) {
                ++p.samples; p.pixSum += ThinPixels(px, q.cov); p.triSum += q.tris;   // phase 111: cut-out px / alpha coverage
                if (q.cov > 0.0f && q.cov < 1.0f) ++p.covSamples;
                const bool pre = ThinVerdict(p.pixSum, p.triSum, p.samples);   // phase 113b: the PRE-filter (the probe confirms)
                if (pre != p.thinPre) {
                    p.thinPre = pre;
                    Rejudge(p);
                    UpdateThin(p);
                    if (pre) { g_info[p.ps]->thinPair.store(true, std::memory_order_relaxed); g_objGen.fetch_add(1, std::memory_order_release); }
                }
            }
        } else if (hr != S_FALSE || n > q.issued + 120) {
            q.busy = false; ++g_cQueryDrop;
            if (q.probe >= 0) ProbeResult(q, 0, false, n);   // phase 113b: its record gives up (lost)
        }
    }
    if (g_hiLeft > 0) --g_hiLeft;
    PicAtPresent(ctx, sc, n);
    PumpGrabs(ctx, n);
    CapTick(sc, n);                                       // phase 110
    for (Pair& p : g_pairs) if (p.dump == 2) p.dump = 3;   // (a started dump's files follow on the writer thread)
    const double now = NowSec();
    if (g_censusOn && now >= g_nextWrite) {               // (phase 110: off = only the drain above, no file)
        g_nextWrite = now + kWriteSecs;
        auto sn = std::make_shared<Snapshot>(TakeSnapshot("periodic"));
        Queue([sn] { WriteCensusFile(*sn); });
        const double tf = g_qpcFreq > 0 ? 1000.0 / (double)g_qpcFreq : 0.0;   // (ticks -> QPC -> ms)
        LogDbg("census (phase 108): %zu pairs, suspects %d, CPU per Present: slow path %.4f ms, tint %.4f ms, Present work %.4f ms "
               "(fast-path calls %.0f per Present, not timed) | queries %llu (dropped %llu), dumps %d, files %d / failed %d, %.1f MB | "
               "phase 110: bytecode kept %.1f MB (not kept %u), input layouts %u | phase 110b: tint %.1f re-draws per Present (capped "
               "%llu), tint GPU ~%.3f ms/frame avg over %d sampled frames (last %.3f; disjoint / lost %d), cut-out texture checks %llu "
               "(memo %.0f %%), waiting cut-out pairs %d | phase 113b: jitter probes %llu (failed %llu, lost %llu), %.4f ms CPU per "
               "Present, candidates %d (thin %d, steady %d, measuring %d) | phase 115: mask draws %llu (no depth %llu, SV_Position v0 "
               "%llu, COLOR0 + SV_Position %llu; other target size %llu), paints %llu (failed %llu), paint CPU %.4f ms per Present, "
               "bug icon %d to capture / %d captured / %d fix off (phase 116: not shown)",
               g_pairs.size(), [] { int c = 0; for (const Pair& p : g_pairs) if (p.v.Suspect()) ++c; return c; }(),
               g_presentsCounted ? (double)TscToQpc(g_ticksSlow) * tf / g_presentsCounted : 0.0, g_presentsCounted ? (double)TscToQpc(g_ticksTint) * tf / g_presentsCounted : 0.0,
               g_presentsCounted ? (double)TscToQpc(g_ticksPresent) * tf / g_presentsCounted : 0.0,
               g_presentsCounted ? (double)g_cFast / g_presentsCounted : 0.0, (unsigned long long)g_cQueries,
               (unsigned long long)g_cQueryDrop, g_dumpsStarted, g_filesOk.load(), g_filesFail.load(), (double)g_diskBytes / 1048576.0,
               (double)g_codeBytes.load() / 1048576.0, g_codeSkipped.load(), g_ilNoted.load(),
               g_presentsCounted ? (double)g_cTint / g_presentsCounted : 0.0, (unsigned long long)g_cTintCapped,
               g_tsFrames ? g_tsMsSum / g_tsFrames : 0.0, g_tsFrames, g_tsLastMs < 0 ? 0.0 : g_tsLastMs, g_tsDisjoint,
               (unsigned long long)g_cCutChecks, g_cCutChecks ? 100.0 * (double)g_cCutMemo / (double)g_cCutChecks : 0.0,
               [] { int c = 0; for (const Pair& p : g_pairs)
                         if ((p.structural & kCutout) && CutClassOf(p.cover.cutReason) == kCutClsWait) ++c; return c; }(),
               (unsigned long long)g_cProbes, (unsigned long long)g_cProbeFail, (unsigned long long)g_cProbeLost,
               g_presentsCounted ? (double)TscToQpc(g_ticksProbe) * tf / g_presentsCounted : 0.0,
               [] { int c = 0; for (const Pair& p : g_pairs) c += p.thinPre; return c; }(),
               [] { int c = 0; for (const Pair& p : g_pairs) c += p.thinPre && p.probe.Verdict() == CensusProbe::kThin; return c; }(),
               [] { int c = 0; for (const Pair& p : g_pairs) c += p.thinPre && p.probe.Verdict() == CensusProbe::kSteady; return c; }(),
               [] { int c = 0; for (const Pair& p : g_pairs) c += p.thinPre && p.probe.Verdict() == CensusProbe::kMeasuring; return c; }(),
               (unsigned long long)g_cMaskDraws, (unsigned long long)g_cMaskLayout[0], (unsigned long long)g_cMaskLayout[1],
               (unsigned long long)g_cMaskLayout[2], (unsigned long long)g_cMaskSkipSize, (unsigned long long)g_cPaints,
               (unsigned long long)g_cPaintFail, g_presentsCounted ? (double)TscToQpc(g_ticksPaint) * tf / g_presentsCounted : 0.0,
               g_bugToCapture.load(), g_bugCaptured.load(), g_bugFixOff.load());
    }
    g_ticksPresent += __rdtsc() - t0;
}
// phase 110: work left after the census was switched off (grabs in flight, a picture, a capture's census part) -- drained at Present
inline bool Draining() { return !g_grabs.empty() || !g_picReq.empty() || g_picBusy || g_cap.active; }

// a MARK (Ctrl+Shift+F6, or the capture key with the view on): picAbs / picRel = its picture (next Present)
int MarkNow(uint64_t n, const std::wstring& picAbs, const std::string& picRel, const std::wstring& picDir) {
    Mark m;
    m.no = (int)g_marks.size() + 1;
    m.present = n;
    m.sec = NowSec();
    GetLocalTime(&m.tm);
    for (int i = 0; i < (int)g_pairs.size(); ++i) {
        Pair& p = g_pairs[i];
        // phase 116 (audit N3): count only a pair the census would colour / capture (a suspect kind no switched-off fix covers), so
        // the MARK's "N suspects on screen" matches what is actually flashing
        if (CapWanted(p.v.Suspect(), p.fixOff) && p.lastPresent + 2 >= n) { m.pairs.push_back(i); p.marks.push_back(m.no); }
    }
    if (!picAbs.empty()) {
        g_picReq.push_back({ picAbs, n + 1 });
        m.pic = picRel;
        Queue([picDir] { MakeDirs(picDir); });
    }
    g_marks.push_back(m);
    snprintf(g_legendMsg, sizeof(g_legendMsg), "MARK %d saved: %zu suspects on screen", m.no, m.pairs.size());
    g_markNoteUntil = NowSec() + 4.0;
    auto sn = std::make_shared<Snapshot>(TakeSnapshot("mark"));
    Queue([sn] { WriteCensusFile(*sn); });
    return m.no;
}

}  // namespace CensusRun

// ---- the entry points inject.cpp calls (declared near its top) -----------------------------------------------------------------
// phase 110: every created shader is classified (the census can be switched on live and must know the shaders made before)
// phase 112r: + the caller's checksum / PixelReader result (no second computation); returns the discard-alpha t-slot mask (Cutout)
uint32_t CensusNotePs(void* ps, const void* code, size_t len, const uint32_t* sum, int prWhy) {
    return CensusRun::Note(ps, code, len, true, sum, prWhy);
}
void CensusNoteVs(void* vs, const void* code, size_t len) { CensusRun::Note(vs, code, len, false); }
// phase 110: the CreateInputLayout hook (any thread) -- the element list as text for the dumps' draw.txt
void CensusNoteLayout(void* il, const D3D11_INPUT_ELEMENT_DESC* d, unsigned n, const void* code, size_t len) {
    using namespace CensusRun;
    if (!il) return;
    std::string s;
    char b[240];
    uint32_t sum[4];
    const bool hs = Census::Checksum(code, len, sum);
    snprintf(b, sizeof(b), "%u element(s), validated against the bytecode %s\n", n, hs ? Short(sum).c_str() : "(not DXBC)");
    s += b;
    for (unsigned i = 0; d && i < n && i < 32; ++i) {
        char off[16];
        if (d[i].AlignedByteOffset == D3D11_APPEND_ALIGNED_ELEMENT) snprintf(off, sizeof(off), "append");
        else snprintf(off, sizeof(off), "%u", d[i].AlignedByteOffset);
        snprintf(b, sizeof(b), "      %s%u %s, slot %u, offset %s, %s (step %u)\n", d[i].SemanticName ? d[i].SemanticName : "?",
                 d[i].SemanticIndex, FmtText(d[i].Format).c_str(), d[i].InputSlot, off,
                 d[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA ? "per instance" : "per vertex", d[i].InstanceDataStepRate);
        s += b;
    }
    auto sp = std::make_shared<const std::string>(std::move(s));
    AcquireSRWLockExclusive(&g_ilLk);
    if (g_il.size() < kIlMax || g_il.count(il)) g_il[il] = sp;
    else g_ilFull.fetch_add(1, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_ilLk);
    g_ilNoted.fetch_add(1, std::memory_order_relaxed);
}
void CensusNoteVsSet(void* vs) {                         // render thread (hkVSSetShader); ~0 = unknown (ClearState and co.)
    CensusRun::g_vsValid = vs != (void*)~(uintptr_t)0;
    CensusRun::g_vsCur = CensusRun::g_vsValid ? vs : nullptr;
}
// phase 110: the menu row "Show unfixed shaders" exists (else hidden): the PS hooks are in and the game's device never changed
bool CensusUsable() { return g_censusReady && !CensusRun::g_devBad; }
bool CensusViewOn() { return g_censusOn && CensusRun::g_view.load(std::memory_order_relaxed); }
// phase 110: the census + its tint view on / off, LIVE (the row, key_census_view, Reset); g_censusCfg is what Save writes
void CensusSetLive(bool on, uint64_t n, const char* via) {
    using namespace CensusRun;
    if (!CensusUsable()) {
        Log("census (phase 110): cannot switch it %s -- %s (%s, Present #%llu)", on ? "on" : "off",
            g_devBad ? "the game's device changed" : "the PSSetShader / CreatePixelShader hooks are not installed", via,
            (unsigned long long)n);
        PlayTones(1, 300, 250, 0);
        return;
    }
    if (on && g_dev) {                                    // phase 112r: the device check OnPresent does (it runs only while on): a
        ID3D11DeviceContext* const gc = g_gameCtx.load(std::memory_order_relaxed);   //   switch-on after a device change must never
        ID3D11Device* d = nullptr;                        //   draw with the old device's tint / query / staging objects
        if (gc) gc->GetDevice(&d);
        if (d) d->Release();
        if (d && d != g_dev) {
            g_devBad = true;
            Log("census (phase 112r): cannot switch it on -- the game's device changed since the census objects were made (%s, "
                "Present #%llu)", via, (unsigned long long)n);
            PlayTones(1, 300, 250, 0);
            return;
        }
    }
    g_censusCfg = on ? 1 : 0;
    if (on) g_codeAll = true;
    if (on && !g_censusOn) g_vsValid = false;            // phase 112r: hkVSSetShader skips its note while off -> read it once again
    if (!on && g_ts.state != 0) {                        // phase 112r: a sampled frame's disjoint query left open (state 1: Begun)
        ID3D11DeviceContext* const gc = g_gameCtx.load(std::memory_order_relaxed);
        if (g_ts.state == 1 && gc && g_ts.dis) {
            const bool was = t_inDlaa;
            t_inDlaa.Quiet(true);
            gc->End(g_ts.dis);                           // (its result is never read: state 0 below)
            t_inDlaa.Quiet(was);
        }
        g_ts.state = 0;
    }
    g_censusOn = on;
    g_view = on;
    if (!on) ReleaseMasks();                             // phase 115: the masks (up to ~37 MB each in VR) only while the view is on
    g_legendUntil = on ? NowSec() + kLegendSecs : -1.0;   // phase 112b: the colour legend box (CensusLegendNow), every switch-on
    g_markNoteUntil = -1.0;
    g_legendMsg[0] = 0;
    int sus = 0;
    for (const Pair& p : g_pairs) if (p.v.Suspect()) ++sus;
    Log("census view %s (phase 110: %s, Present #%llu): %d suspect pairs so far%s", on ? "ON" : "OFF", via, (unsigned long long)n, sus,
        on ? " -- unfixed draws are tinted; the capture key pressed twice now also saves every suspect on screen"
           : " -- the census stops looking at draws (no cost); what it found stays for the next switch-on");
    PlayTones(on ? 1 : 2, on ? 1000 : 400, 80, 60);
}
void CensusStart(bool vsHooked) {
    QueryPerformanceCounter(&CensusRun::g_t0);
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); CensusRun::g_qpf = (double)f.QuadPart;
    CensusRun::g_vsHooked = vsHooked;
    g_censusReady = true;
    g_censusOn = g_censusCfg != 0;
    CensusRun::g_view = g_censusOn;
    if (g_censusOn) CensusRun::g_codeAll = true;
    Log("census (phase 108 / 110): ready, %s (census_dump=%d census_max_mb=%d) -- the menu row \"Show unfixed shaders\" / %s switches it "
        "live: magenta pixel-reader, orange cut-out, cyan thin, blue water-like, yellow unknown VS; %s = mark; the capture key pressed "
        "twice with it on also saves every suspect on screen; VS from %s", g_censusOn ? "ON (dlaa.ini census = 1)" : "off",
        g_censusDumpCfg, g_censusMaxMb, KeyName(KA_CENSUS_VIEW), KeyName(KA_CENSUS_MARK),
        vsHooked ? "the VSSetShader hook (did_shadow)" : "VSGetShader per interesting draw (did_shadow = 0)");
}
void CensusOnPresent(IDXGISwapChain* sc, uint64_t n) {
    if (!g_censusOn && !CensusRun::Draining()) return;  // phase 110: off = one flag + a few empty checks per Present
    const bool was = t_inDlaa;
    t_inDlaa.Quiet(true);                                // our copies / maps / queries pass the hooks untracked
    try { CensusRun::OnPresent(sc, n); } catch (...) {}
    t_inDlaa.Quiet(was);
}
// phase 111 (VR): at an eye's blit -- a census picture that is due is taken from eye 0's own picture (the RIGHT eye, after DLAA)
// instead of the desktop mirror window. Off = one flag test.
void CensusOnEyeBlit(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, int eye) {
    if ((!g_censusOn && !CensusRun::Draining()) || !g_vrMode || eye != 0) return;
    const bool was = t_inDlaa;
    t_inDlaa.Quiet(true);
    try { CensusRun::PicAtEyeBlit(ctx, tex, eye, g_frames.load(std::memory_order_relaxed)); } catch (...) {}
    t_inDlaa.Quiet(was);
}
// phase 115: at every eye / screen blit (inject.cpp, before the game's blit draw, after DLAA): the pass's census mask painted into the
// blit source as the flashing checkerboard. Off = one flag test.
void CensusPaintBlit(ID3D11DeviceContext* ctx, uint64_t passS, ID3D11Texture2D* dst, ID3D11ShaderResourceView* depthSrv, UINT dw,
                     UINT dh, float ox, float oy) {
    if (!g_censusOn || !CensusRun::g_maskOk) return;
    const bool was = t_inDlaa;
    t_inDlaa.Quiet(true);
    try { CensusRun::PaintAtBlit(ctx, passS, dst, depthSrv, dw, dh, ox, oy); } catch (...) {}
    t_inDlaa.Quiet(was);
}
// phase 115: the bug icon (shown while the census view is on): what is left to capture here (distinct suspect pairs shown in the last
// ~1 s, both eyes once) and what was captured this session. False = no icon now.
// Phase 116: fixOff = distinct pairs in that window NOT shown only because a fix of the mod is switched off.
bool CensusBugNow(int* toCapture, int* captured, int* fixOff) {
    if (!CensusViewOn()) return false;
    *toCapture = CensusRun::g_bugToCapture.load(std::memory_order_relaxed);
    *captured = CensusRun::g_bugCaptured.load(std::memory_order_relaxed);
    *fixOff = CensusRun::g_bugFixOff.load(std::memory_order_relaxed);
    return true;
}
void CensusToggleView(uint64_t n) { CensusSetLive(!g_censusOn, n, KeyName(KA_CENSUS_VIEW)); }   // phase 110: the row's switch
void CensusMark(uint64_t n) {
    using namespace CensusRun;
    if (!g_censusOn) { PlayTones(1, 300, 250, 0); return; }
    wchar_t nm[48];
    swprintf(nm, 48, L"mark_%02d.png", (int)g_marks.size() + 1);
    std::wstring dir;
    std::wstring pic;
    char rel[64] = "";
    if (DirPath(L"marks", dir)) { pic = dir + nm; snprintf(rel, sizeof(rel), "dlaa_census\\marks\\mark_%02d.png", (int)g_marks.size() + 1); }
    const int no = MarkNow(n, pic, rel, dir);
    Log("census MARK %d (%s, Present #%llu): %zu suspects on screen -- dlaa_census.txt has the list", no, KeyName(KA_CENSUS_MARK),
        (unsigned long long)n, g_marks.back().pairs.size());
    PlayTones(2, 1200, 60, 40);
}
// phase 110: the bug report capture was accepted (the capture key pressed twice, or Capture now). Census view OFF: nothing (the
// capture is exactly as before). ON: a MARK (picture = census\tinted.png) + the armed frames (CapRun above). sub = the capture's
// folder name "shimmer_<date>_<time>\" (SelfTestStep makes the folder with that name).
void CensusOnCapture(uint64_t n, const wchar_t* sub) {
    using namespace CensusRun;
    if (!CensusViewOn() || !sub || !*sub) return;
    if (g_cap.active) {
        Log("census (phase 110): the last capture's census part is still running -- this capture has no census part (Present #%llu)",
            (unsigned long long)n);
        return;
    }
    wchar_t base[MAX_PATH];
    if (!PathNextToDll(L"dlaa_selftest\\", base)) return;
    static int s_no = 0;
    g_cap = CapRun{};
    g_cap.active = true;
    g_cap.no = ++s_no;
    g_cap.dir = std::wstring(base) + sub + L"census\\";
    g_cap.rel = "dlaa_selftest\\";
    for (const wchar_t* c = sub; *c; ++c) g_cap.rel += (char)(*c < 128 ? *c : '_');
    g_cap.rel += "census\\";
    g_cap.key = n;
    g_cap.armFrom = n + 1;
    g_cap.armTo = n + kCapArmFrames;
    g_cap.keySec = NowSec();
    GetLocalTime(&g_cap.keyTime);
    g_cap.mark = MarkNow(n, g_cap.dir + L"tinted.png", g_cap.rel + "tinted.png", g_cap.dir);
    g_legendUntil = -1.0;                                // phase 112b: done what the legend asked -- the status box takes over
    Log("census (phase 110): bug report with the census view on -- MARK %d; every suspect drawn in Presents #%llu..#%llu goes into %s "
        "(shaders, disassembly, textures, constant buffers, state, coverage, a picture each; budget %d MB, %d full dumps)", g_cap.mark,
        (unsigned long long)g_cap.armFrom, (unsigned long long)g_cap.armTo, g_cap.rel.c_str(), g_censusMaxMb, kCapMaxPairs);
}
// phase 112r: the capture this census part belongs to never started (SelfTestStep refused it: device changed / no folder, or the
// request was not taken within kCapStartWait Presents) -- the census part stops (no folder of its own, no work left) and the folder
// name fixed at the key is dropped (a later capture gets its own)
void CensusCancelCapture(uint64_t n, const char* why) {
    using namespace CensusRun;
    Cap::g_nextSub[0] = 0;
    if (!g_cap.active) return;
    g_cap.active = false;
    CapUnsave(g_cap.no);                                 // phase 115r (B7): nothing saved -> highlighted again
    Log("census (phase 112r): the bug report capture did not start (%s) -- its census part is cancelled (Present #%llu)", why,
        (unsigned long long)n);
}
// phase 110: CaptureFinishTick waits for this before the zip (n = the current Present; a hard stop after the deadline)
bool CensusCaptureBusy(uint64_t n) {
    using namespace CensusRun;
    return g_cap.active && n <= g_cap.key + kCapDeadline + 300;
}
// the status box's two lines (CaptureStatus asks when no capture runs): a mark's note for 4 s. Phase 112b: the colour legend is no
// longer squeezed into these two lines (the title was cut: "Census view: magenta pixel-reader, ora...") -- CensusLegendNow below.
bool CensusStatus(wchar_t* l1, wchar_t* l2, size_t cap) {
    using namespace CensusRun;
    if (!g_censusOn || !g_legendMsg[0] || g_markNoteUntil < 0.0 || NowSec() > g_markNoteUntil) return false;
    swprintf(l1, cap, L"Census: %hs", g_legendMsg);
    swprintf(l2, cap, L"see dlaa_census.txt");
    return true;
}
// phase 112b: the colour legend box (at Present, CaptureAtPresent): shown for kLegendSecs after every switch-on (row or key), and
// again while the menu is open on the row "Show unfixed shaders" -- only while the view is on (no colours, nothing to explain).
// lg = the text with the player's OWN bindings of the capture key (pressed twice) and key_census_view. False = not now.
// phase 114: the user could not read the 15 s legend in the headset in time -- a finished double-Insert capture (CaptureAtPresent)
// shows it again for kLegendSecs while the view is on (the menu's census row shows it too, as before)
void CensusLegendAgain() {
    using namespace CensusRun;
    if (CensusViewOn()) g_legendUntil = NowSec() + kLegendSecs;
}
bool CensusLegendNow(TuningMenu::Legend& lg) {
    using namespace CensusRun;
    if (!CensusViewOn()) { g_fixOffLineUntil = -1.0; return false; }   // phase 116 (S2): the view off clears the latch
    const bool timer = g_legendUntil >= 0.0 && NowSec() <= g_legendUntil;
    const bool row = g_menuOpen.load(std::memory_order_relaxed) && g_menuSel == MR_CENSUS && g_menuPage == kMenuPageMain;
    if (!timer && !row) return false;
    wchar_t cap[96], view[96];
    if (!MultiByteToWideChar(CP_UTF8, 0, KeyNameUi(KA_SHIMMER_CAP), -1, cap, 96)) cap[0] = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, KeyNameUi(KA_CENSUS_VIEW), -1, view, 96)) view[0] = 0;
    // phase 113 / 116: one more line while objects are not coloured because a fix of the mod is switched off. Phase 116 (audit S2):
    // the count (g_bugFixOff) blinks to 0 and back as the player drives; the line is LATCHED -- held for kLegendSecs after the last
    // frame the count read > 0 -- so it does not flicker. It clears only when the census view is switched off (above). Cost: one
    // compare + one store per Present while the legend shows.
    const double now = NowSec();
    if (g_bugFixOff.load(std::memory_order_relaxed) > 0) g_fixOffLineUntil = now + kLegendSecs;
    CensusLegendFill(lg, cap, view, g_fixOffLineUntil >= 0.0 && now <= g_fixOffLineUntil);
    return true;
}
// process exit (OnProcessDetach, loader lock, the other threads were killed): phase 109r -- NO write here any more. A thread killed
// mid-change could have left g_pairs half-updated or the heap lock held, and the snapshot + format allocate heavily (a crash or a
// hang at exit). The file is current instead: the periodic write every kWriteSecs, plus one kSoonSecs after a NEW suspect and one
// at every mark (OnSuspect / CensusMark).
void CensusAtExit() {}
