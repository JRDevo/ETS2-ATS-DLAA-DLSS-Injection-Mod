// GpuPerf -- v0.10.0 phase 8: one GPU timer per sub-pass of the mod, per eye ("perf eye N" lines every 600 blits).
//
// Why: the in-game log had one figure for the whole per-draw pairing chain ("pairing GPU 1.0-1.4 ms") and nothing for the
// rest of the mod's own GPU work (depth snapshot, candidate copies, medoid, pass B, copies, RCAS, mirror units). The goal for
// v0.10.0 is <= 1 ms per eye for everything except the NGX evaluation itself (VR: 2 eyes at 72 Hz) -- that needs every
// sub-pass measured on its own.
//
// How: timestamp queries only (no per-section disjoint query): ONE TIMESTAMP_DISJOINT per Present frame ("set") brackets every
// timestamp of that frame; a section costs 1-2 timestamps (Next() shares one between two back-to-back sections, so the
// per-draw dispatch chain is one timestamp per dispatch). Sets land 2-4 frames later through GetData(DONOTFLUSH) -- never a
// blocking readback. Each section is tagged with the PASS it belongs to (SetPass: the pass sequence number); the blit that
// consumes the pass tells which eye it was (NoteEye), so the per-eye sums are exact even when a pass's replay and its blit
// land in different frames. Sections marked SAMPLED (interleaved per-draw work inside the game's passes: the candidate copies,
// the forward-depth re-draws, the instanced / non-instanced split of the replay) are only timed in 1 of kSampleEvery passes
// (every draw bracketed: an UPPER bound -- the brackets serialise the GPU around each small piece of work); their average is
// per sampled pass. (A sampled pass costs a little more GPU time itself: the "max" figures can include that.)
//
// On with dlaa.ini debug = 1 (perf_timers = -1, default) or perf_timers = 1; off = every call returns at once (one bool test).
// Render thread only (the game's immediate context). Compiled only when WITH_DLAA=1.
#pragma once
#ifdef WITH_DLAA

#include <d3d11.h>
#include <cstdint>

namespace GpuPerf {

enum Sec : int {
    // pass-time work (inside / between the game's passes)
    kReplay = 0,      // G-buffer draw-id replay (every segment of the pass)
    kFwdReplay,       // forward draw-id replay
    kSnap,            // depth snapshot (scene depth -> twin copy)
    kCandCopy,        // SAMPLED: per-draw candidate MVP copies (the old medoid path) inside the G-buffer pass
    kFwdDepth,        // v0.10.0 phase 10: the batched forward-depth re-draw at the pass end (EXACT, every pass; was SAMPLED
                      // "fwd-depth*": per re-draw brackets interleaved with the game's forward draws = an upper bound)
    kFwdInit,         // v0.10.0 phase 10: its clear + occlusion init (mv_fwd_depth_cull)
    kReplayInst,      // SAMPLED breakdown of kReplay (not in the total): runs of instanced / no-MVP draws
    kReplayRest,      // SAMPLED breakdown of kReplay (not in the total): runs of the other draws
    // blit-time work (SceneDlaa::Run / CameraMv::Generate / DrawIdMv::Dispatch)
    kCopyIn,          // colour copy into the DLAA input
    kConvert,         // depth convert (only when the MV pass did not run)
    kMedoid,          // pass A (the medoid camera R + R_ego) and the candidate-record commit copy
    kGather, kMatch, kPair, kVote, kPick, kResolve,          // per-draw pairing + consensus camera R
    kInherit, kParCount, kParList, kTwin, kAttach,            // twins / attach / rigid-parent list
    kParVote1, kParPick1, kParVote2, kParPick2,               // rigid-parent marching vote, both passes
    kPickRes, kListTwin,                                      // v0.10.0 phase 9 folded: pick+resolve+inherit / p-list+twin+attach
    kResInh,                                                  // v0.10.0 phase 18 (mv_fold_dispatch 2): resolve + inherit, wide
    kParCol1, kParCol2,                                       // v0.10.0 phase 18 (mv_vote_compact): the march items of each vote
                                                              // pass (p-vote1 / p-vote2 are then the marches themselves)
    kPassB,           // pass B: per-pixel reprojection + depth flatten
    kMvMisc,          // the rest of the MV generation (diagnostic copies, commit copies)
    kNgx,             // the NGX evaluation itself (NOT in the mod total)
    kRcas,            // RCAS sharpen
    kCopyOut,         // copy-back / composite
    kDbgView,         // Ctrl+F6 debug view
    kMirror,          // mirror units: everything except their NGX evaluation (replay, snapshot, MV, copies, RCAS)
    kMirNgx,          // mirror units' NGX evaluation (NOT in the mod total)
    kOverlay,         // v0.10.3 phase 29b: the fps box draws (TuningMenu::DrawWidget: the VR quad image, each eye picture, the
                      // flat / desktop-mirror backbuffer) -- PER FRAME, not per pass: not tagged with an eye (FrameStats), NOT in
                      // the per-eye mod total
    kBias,            // phase 49: the glow bias mask -- the pass-end re-draw of the additive forward draws + the share pass (pass
                      // time, EXACT) and the crop / reprojection step before NGX (blit time)
    kCount
};

void SetOn(bool on);
bool On();
const char* Name(int sec);
bool InTotal(int sec);                  // false: NGX / mirror NGX / the sampled replay breakdown
bool IsSampled(int sec);
bool IsPerFrame(int sec);               // v0.10.3 phase 29b: kOverlay -- summed per landed query set (= per Present), not per eye

void SetFrame(uint64_t frame);          // the Present counter (a new value rolls the set over at the next Begin; no D3D call)
void SetPass(uint64_t tag);             // the pass the following sections belong to
uint64_t Pass();
void SetMirror(bool on);                // mirror scope: every section -> kMirror (NGX -> kMirNgx)
bool Mirror();
constexpr uint64_t kSampleEvery = 31;   // sampled sections: 1 pass in 31 (odd: in VR both eyes' passes get sampled; v0.12.0: 31 is
                                        // prime, so every flat multi-view view gets sampled for any view count up to 30)
constexpr int kViews = 8;               // v0.12.0 issue #12: eyes / views with their own statistics (VR 0..1, flat multi-view 0..7;
                                        // = FlatViews::kMaxViews in inject.cpp)
bool Sampled(uint64_t tag);

// Timed section: Begin -> token (-1 = not timed: off / set full), Next = End(tok) + Begin(sec) sharing one timestamp.
int  Begin(ID3D11DeviceContext* ctx, int sec);
int  Next(ID3D11DeviceContext* ctx, int tok, int sec);
void End(ID3D11DeviceContext* ctx, int tok);

void NoteEye(uint64_t tag, int eye);    // the blit (or the pre-tonemap run) of pass `tag` is eye `eye`
void Poll(ID3D11DeviceContext* ctx);    // lands finished sets (non-blocking)

struct EyeStats {
    uint64_t passes = 0;                // passes of this eye with landed sections in the window
    double   avg[kCount] = {}, mx[kCount] = {};
    uint64_t n[kCount] = {};            // passes in which the section ran (sampled sections: their sample count)
    double   total = 0.0, totalMax = 0.0;   // mod total per pass (sections InTotal; sampled ones by their own average)
};
// window statistics of eye / view 0..kViews-1 since the last Restart (false = nothing landed for that eye / view)
bool Stats(int eye, EyeStats* out);
uint64_t Unattributed();                // landed sections whose pass never reached a blit (window)
uint64_t Dropped();                     // sections not timed because a set was full / a set was lost (window)
// v0.10.3 phase 29b: a per-frame section's window sums since the last Restart: its ms, the largest one-frame sum, the query sets
// (= Present frames) it ran in, and every set that landed (the per-frame average = sumMs / sets: a frame without it counts 0)
struct FrameStat { double sumMs = 0.0, maxMs = 0.0; uint64_t n = 0, sets = 0; };
FrameStat FrameStats(int sec);
void Restart();                         // restarts the window
uint32_t Restarts();                    // window restarts so far (the tuning menu's own 1-s deltas see a restart in between)
// "perf eye N: ..." lines (one per eye with data); sceneMs < 0 = no GPU spans figure. Restarts the window.
void LogWindow(uint64_t blit, double sceneMs, uint32_t sceneN);
void Shutdown();                        // releases every query (device change / harness exit)

}  // namespace GpuPerf

// Scope helper: Begin in the constructor, End in the destructor.
struct GpuPerfScope {
    ID3D11DeviceContext* ctx;
    int tok;
    GpuPerfScope(ID3D11DeviceContext* c, int sec) : ctx(c), tok(GpuPerf::Begin(c, sec)) {}
    ~GpuPerfScope() { GpuPerf::End(ctx, tok); }
    GpuPerfScope(const GpuPerfScope&) = delete;
    GpuPerfScope& operator=(const GpuPerfScope&) = delete;
};

#endif // WITH_DLAA
