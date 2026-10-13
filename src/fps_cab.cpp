// FpsCab runtime -- phase 47: the VR fps box attached to the vehicle (fps_vr_lock = 2). See fps_cab.h for the design and the
// maths. Render thread only (the game's immediate context): OnBlit at each VR eye blit, Get at each drawn-in box draw.
#ifdef WITH_DLAA

#include "fps_cab.h"
#include "fps_profile.h"
#include "draw_ids.h"
#include "log.h"
#include <windows.h>
#include <wrl/client.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace FpsCab {
namespace {

constexpr int  kRb = 4;                                 // snapshots in flight (staging buffers)
constexpr UINT kRowBytes = 128;                         // cb0 rows 4..11 of one candidate (the MVP block sits in rows 4..11)
constexpr UINT kStageBytes = 2u * (UINT)kCand * kRowBytes;
constexpr int  kRelockBlits = 90;                       // blits of an eye with cab draws but none of the lock's parts: a new cab
constexpr int  kLockRetryBlits = 144;                   // a lock that found no usable part: try again after ~1 s (2 eyes x 72 Hz)
constexpr double kAgreeM = 0.015, kDemoteM = 0.03;      // monitor: parts agree within 1.5 cm; a part 3 cm off the group moves
constexpr int  kLogLines = 32;                          // level-3 / level-4 event lines per kind (no flood)
// phase 47b: lock on only after this many blits in a row (both eyes) with cab draws in a flushed record and usable OpenXR poses (~0.5 s at 72 Hz):
// never during a loading screen / its first frames (ATS 14:32 test: lock #1 on a loading picture, eye 1 only, no pose)
constexpr int  kLockSteady = 72;
constexpr int  kLockRefuseRetry = 36;                   // a refused lock (one eye only / no pose): try again after ~0.25 s

struct Cand {
    int i = -1;                                         // index in the record
    uint64_t full = 0, loose = 0;
    uint32_t ic = 0, off = 0, rows = 0;                 // IndexCount, MirrorOff (row 4), rows mirrored from row 0
    uint32_t stride = 0, fmt = 0;                       // phase 51: VB0 stride + index format (the vehicle key, fps_profile.h)
    int ent = -1;                                       // monitor snapshots: the lock entry it matched
};
struct SnapEye {
    bool done = false;
    int n = 0;
    Cand c[kCand];
    bool xrOk = false;
    Pose pose{};
    double fw = 1.6;
};
struct Snap {
    ComPtr<ID3D11Buffer> buf;
    int state = 0;                                      // 0 free, 1 open (collecting at the eyes' blits), 2 copied (readback)
    int kind = 0;                                       // 1 lock, 2 monitor
    uint64_t gen = 0;                                   // the lock generation it belongs to (a monitor of an old lock is dropped)
    uint64_t blit = 0;                                  // g.blits when opened
    uint64_t frame = 0;                                 // the Present count when opened (the lock's moment t0)
    uint64_t serial0 = 0;                               // the pass serial of the frame's first eye (opened there; the other eye = + 1)
    bool xrLocal = false;
    SnapEye eye[2];
};
struct Entry {
    uint64_t full = 0, loose = 0;
    uint32_t ic = 0;
    uint32_t stride = 0, fmt = 0;                       // phase 51b: VB0 stride + index format (a stored cab pose names its part by them)
    int row = 4;                                        // the MVP block's first cb0 row (4..8)
    M4 T = Identity();                                  // the reference's model space -> this part's (lock time)
    double a[4][4] = {};                                // the box corners in this part's model space (homogeneous)
    double ac[4] = {};                                  // the box centre
    bool demoted = false;                               // a moving part (monitor): never used again for this lock
};
struct LockSt {
    bool have = false;
    uint64_t gen = 0;
    int n = 0;
    Entry e[kCand];
    M4 A = Identity();                                  // play space -> the reference's model space at the lock
    LockFrame lf;
    double upm = 1.0, playPerGame = 1.0;
    double centrePlay[3] = {};
    bool xr = false, local = false;
    int eyesUsed = 0;
    uint64_t frame = 0;                                 // the Present count of the lock-on readback (t0)
    bool swapped = false;                               // the OpenXR views came in the other eye order (the monitor uses that)
    uint64_t vehicleKey = 0;                            // phase 51: FpsProfile::VehicleKey of the lock-on readback's cab draws
    bool sameCab = false;                               // phase 51: a 47e re-lock that kept the previous lock's spot (same cab)
    // phase 51b: where the play -> cab map came from (GetLockInfo), the stored cab pose used (verbatim: CurrentCabPose)
    int placed = kPlacedNone;
    int samples = 0, samplesUsed = 0;
    double spreadMm = 0.0;
    bool cabMissing = false;
    uint32_t missingIc = 0;
    bool hasCab = false;
    FpsProfile::CabPose cab;
    int cabFound = 0, cabAgree = 0;                     // phase 51c: stored parts found in this lock / in the agreeing majority
    uint32_t cabUsedIc = 0;                             // the stored part the placement came from
    bool cabAmbiguous = false;                          // the found stored parts disagree with no majority (-> the play space)
    bool remeasured = false;                            // phase 51c: a lock-on asked for by Remeasure (the stored pose not used)
    double kRef = 1.0;                                  // the reference eye frame's game metres per model unit (playPerGame)
    // phase 51d: the CAB FRAME (the rotation shared by the largest summed IndexCount of the lock's cab draws) and the box square to it
    bool haveCabS = false;
    double cabS[3][3] = {};                             // the cab's axes in the reference's model space (FrameRot(ref, cab))
    int cabFrameDraws = 0;
    uint32_t cabFrameIc = 0;
    uint64_t cabFrameSum = 0;
    bool level = false, leveledOld = false, cabFrameRejected = false;
    double turnDeg = 0.0, tiltSide = 0.0, tiltFore = 0.0, tiltYaw = 0.0;
    M4 v2p = Identity();                                // the box's frame -> the play space at the lock (the cab-vs-play figure)
};
struct EyeCur {
    uint64_t serial = 0, frame = ~0ull;
    int entry = -1, lastEntry = -1;
    uint32_t off = 0;
    ComPtr<ID3D11ShaderResourceView> srv;
    int why = kNoBlit;
    uint32_t noMatch = 0;
    bool noCabLogged = false;
    int latchAge = 99;                                  // phase 47b: Latches (Presents) since this eye's last blit (0 = this frame;
                                                        // 1 is accepted too: a Present between the two eyes' blits)
};
struct St {
    bool active = false;
    Settings set;
    LockSt lk;
    EyeCur eye[2];
    Snap snap[kRb];
    int open = -1;                                      // the open snapshot (state 1)
    uint64_t blits = 0, lastMonitor = 0, nextLock = 0;
    int monitorEvery = 12;                              // blits (both eyes): 6 frames = ~0.08 s at 72 Hz (phase 47d; was 72 = 0.5 s)
    uint64_t gen = 0;
    // totals
    uint64_t locks = 0, relocks = 0, reacq = 0, attached = 0, fallback = 0;
    int lockFails = 0;
    int linesLock = 0, linesWhy = 0, linesReacq = 0, linesDemote = 0, linesRelock = 0, linesIcMatch = 0, linesKey = 0;
    // phase 47e: the lock dropped by an AUTOMATIC re-lock (cab change / every carrier moving): kept so the new lock on the same cab
    // puts the box back in the same spot of the cab (cleared by SetActive / Reset / Shutdown: a fresh lock there)
    LockSt prev;
    bool havePrev = false;
    uint64_t prevLockNo = 0;
    int lastWhy = -1;
    // phase 47b: the per-frame attach decision (Latch) + the lock gate
    bool latched = false;
    int onRun = 0, offRun = 0;
    uint32_t hist = 0;                                  // bit i = frame -i was NOT attachable (the last 8 frames)
    uint64_t steady = 0;                                // blits in a row with cab draws + usable poses (the lock gate)
    uint64_t latchOn = 0, latchOff = 0, held = 0, monitors = 0, lockRefusals = 0;
    double monitorUs = 0.0;                             // phase 47d: CPU spent in ProcessMonitor (total)
    int linesLatch = 0;
    // the 10-s window (level 4)
    ULONGLONG winT0 = 0;
    uint64_t wAttached = 0, wFallback = 0, wWhy[kWhyCount] = {}, wReacq = 0, wMon = 0, wHeld = 0, wFrOn = 0, wFrOff = 0;
    double maxDev = 0.0, sumDev2 = 0.0, maxJump = 0.0;
    int nDev = 0;
    double lastPlay[3] = {};
    bool lastHave = false;
    // phase 51b: the stored cab pose source (inject.cpp: the vehicle profiles) and the multi-readback lock-on
    CabPoseFn provider = nullptr;
    int lockSamples = kLockSamplesDefault;
    struct Acc {
        int n = 0, restarts = 0;
        uint32_t ic = 0, stride = 0, fmt = 0;           // the reference part the maps are in
        M4 A[kMaxLockSamples];
        int rejected = 0;                               // phase 51c: readbacks with a broken map (IPD ratio) not used
    } acc;
    bool remeasure = false;                             // phase 51c: the next lock-on ignores the stored cab pose (Remeasure)
    uint64_t cabMovedSerial = 0;                        // phase 51c: a stored cab part found moving (the profile drops it)
    FpsProfile::CabPart cabMoved;
    static constexpr int kMovedQ = 16;                  // phase 51d: every report (CabMovedSince), not only the last
    FpsProfile::CabPart movedQ[kMovedQ];
    int linesIpd = 0;
};
St g;

// phase 51b: ONE lock around the whole state. The render thread (OnBlit / Get / NoteDraw: the eye blits and draws) and the game's
// MAIN thread (Present: FpsCabSync -> SetActive / SetSettings, Latch, Attached, Reset from the menu, the profile tick's LockCount /
// VehicleKey / GetLockInfo / CurrentCabPose / PlaceFromCab) both call in. A CRITICAL_SECTION (recursive: GetStats calls
// VehicleKey; uncontended ~20 ns). Held for one call: OnBlit's longest is a lock-on readback (ProcessLock, tens of us).
// Lock order: this lock, then (inside ProcessLock) the provider's own profile lock -- inject.cpp never calls FpsCab while it
// holds the profile lock.
struct Cs {
    CRITICAL_SECTION cs;
    Cs() { InitializeCriticalSectionAndSpinCount(&cs, 1000); }
    ~Cs() { DeleteCriticalSection(&cs); }
} g_cs;
struct Guard {
    Guard() { EnterCriticalSection(&g_cs.cs); }
    ~Guard() { LeaveCriticalSection(&g_cs.cs); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

const char* const kWhyNames[kWhyCount] = {
    "attached", "mode 2 not active", "no world blit of this eye this frame (menu / loading / preview picture)",
    "no flushed draw record (DLAA / motion vectors / per-part motion off or passive)", "locking on (waiting for the readback)",
    "no cab draw in this picture (exterior camera / no cabin layer)", "none of the lock's cab parts in this picture",
    "the attached-box shader is not ready / failed", "settling (the attach must hold 8 frames in a row first)",
    "stale (a newer pass started since this eye's blit)",
};

int Gather(DrawIdRecord& rec, Cand* out, int cap) {
    int n = 0;
    const int cnt = rec.Count();
    uint64_t dup[kCand];
    int nd = 0;
    for (int i = 0; i < cnt; ++i) {
        const uint8_t f = rec.Flags(i);
        if (!(f & DrawIdRecord::kFCabin) || (f & (DrawIdRecord::kFInstanced | DrawIdRecord::kFNoMvp | DrawIdRecord::kFFwd))) continue;
        const UINT off = rec.MirrorOff(i), rows = rec.MirrorRows(i);
        if (off == 0xFFFFFFFFu || rows < 8u) continue;
        Cand c;
        c.i = i; c.full = rec.FullKey(i); c.loose = rec.LooseKey(i); c.ic = rec.IndexCount(i); c.off = off; c.rows = rows;
        c.stride = rec.VbStride(i); c.fmt = rec.IbFormat(i);   // phase 51
        bool isDup = false;                             // the same geometry key twice in one pass: ambiguous, never a reference
        for (int k = 0; k < n; ++k) if (out[k].full == c.full) { isDup = true; if (nd < kCand) dup[nd++] = c.full; break; }
        if (isDup) continue;
        if (n < cap) out[n++] = c;
        else if (c.ic > out[n - 1].ic) out[n - 1] = c;
        else continue;
        for (int k = n - 1; k > 0 && out[k].ic > out[k - 1].ic; --k) { const Cand t = out[k]; out[k] = out[k - 1]; out[k - 1] = t; }
    }
    if (nd) {                                           // drop the ambiguous keys
        int m = 0;
        for (int k = 0; k < n; ++k) {
            bool d = false;
            for (int j = 0; j < nd; ++j) if (dup[j] == out[k].full) d = true;
            if (!d) out[m++] = out[k];
        }
        n = m;
    }
    return n;
}

bool AnyCab(DrawIdRecord& rec) {
    const int cnt = rec.Count();
    for (int i = 0; i < cnt; ++i) {
        const uint8_t f = rec.Flags(i);
        if ((f & DrawIdRecord::kFCabin) && !(f & (DrawIdRecord::kFInstanced | DrawIdRecord::kFNoMvp | DrawIdRecord::kFFwd))) return true;
    }
    return false;
}

bool IsMoving(uint64_t full, uint64_t loose);            // (phase 47e: defined with the moving-parts list below)
bool IsMovingGeom(uint32_t ic, uint32_t stride, uint32_t fmt);   // phase 51c

// The lock's entries present in `rec`: idx[e] = the record index (-1 = absent / ambiguous) -- by full key, else a unique loose
// key (the game's buffer pool rotated the pointers), else (phase 47e) the one cab draw with the entry's IndexCount.
void Match(DrawIdRecord& rec, int idx[kCand]) {
    int fullN[kCand] = {}, looseN[kCand] = {}, fullI[kCand], looseI[kCand], icN[kCand] = {}, icI[kCand];
    const int cnt = rec.Count();
    for (int i = 0; i < cnt; ++i) {
        const uint8_t f = rec.Flags(i);
        if (!(f & DrawIdRecord::kFCabin) || (f & (DrawIdRecord::kFInstanced | DrawIdRecord::kFNoMvp | DrawIdRecord::kFFwd))) continue;
        const uint64_t fk = rec.FullKey(i), lk = rec.LooseKey(i);
        for (int e = 0; e < g.lk.n; ++e) {
            if (g.lk.e[e].full == fk) { if (!fullN[e]++) fullI[e] = i; }
            else if (g.lk.e[e].loose == lk) { if (!looseN[e]++) looseI[e] = i; }
            else if (g.lk.e[e].ic == rec.IndexCount(i) && !IsMoving(fk, lk) &&
                     !IsMovingGeom(rec.IndexCount(i), rec.VbStride(i), rec.IbFormat(i))) { if (!icN[e]++) icI[e] = i; }   // phase 47e (51c: + geometry)
        }
    }
    for (int e = 0; e < kCand; ++e) {
        idx[e] = -1;
        if (e >= g.lk.n) continue;
        int i = fullN[e] == 1 ? fullI[e] : (fullN[e] == 0 && looseN[e] == 1 ? looseI[e] : -1);
        bool byIc = false;
        if (i < 0 && fullN[e] == 0 && looseN[e] == 0 && icN[e] == 1) { i = icI[e]; byIc = true; }   // phase 47e: the keys changed, the mesh did not
        if (i >= 0 && (rec.MirrorOff(i) == 0xFFFFFFFFu || rec.MirrorRows(i) < (UINT)g.lk.e[e].row + 4u)) i = -1;
        if (i >= 0 && byIc && g.linesIcMatch < kLogLines) {
            ++g.linesIcMatch;
            Log("fps box attached to vehicle: cab part IndexCount %u found by its IndexCount only (the game changed its buffers / shader) "
                "-- the lock is kept", g.lk.e[e].ic);
        }
        idx[e] = i;
    }
}

void BuildCorners() {
    LockSt& L = g.lk;
    double cp[4][3], cc[3];
    BoxPlay(g.set, L.lf, cp, cc);
    double ref[4][4], refC[4];
    for (int k = 0; k < 4; ++k) {
        const double p[4] = { cp[k][0], cp[k][1], cp[k][2], 1.0 };
        Apply(L.A, p, ref[k]);
    }
    const double pc[4] = { cc[0], cc[1], cc[2], 1.0 };
    Apply(L.A, pc, refC);
    for (int e = 0; e < L.n; ++e) {
        for (int k = 0; k < 4; ++k) Apply(L.e[e].T, ref[k], L.e[e].a[k]);
        Apply(L.e[e].T, refC, L.e[e].ac);
    }
    {                                                    // phase 51d: the box's frame -> the play space of the lock
        const double h[4] = { cc[0], cc[1], cc[2], 1.0 };
        double o[4];
        Apply(L.v2p, h, o);
        for (int i = 0; i < 3; ++i) L.centrePlay[i] = o[i];
    }
    g.lastHave = false;
}

void ResetWindow() {
    g.winT0 = GetTickCount64();
    g.wAttached = g.wFallback = g.wReacq = g.wMon = g.wHeld = g.wFrOn = g.wFrOff = 0;
    memset(g.wWhy, 0, sizeof(g.wWhy));
    g.maxDev = g.sumDev2 = g.maxJump = 0.0;
    g.nDev = 0;
}

void DropLock() {
    g.lk = LockSt();
    g.lk.gen = ++g.gen;
    for (EyeCur& c : g.eye) { c.entry = c.lastEntry = -1; c.noMatch = 0; }
    for (Snap& s : g.snap) if (s.state == 1) s.state = 0;   // (copied ones land and are dropped by their generation)
    g.open = -1;
    g.nextLock = 0;
    g.lockFails = 0;
    g.acc = St::Acc();                                  // phase 51b: the readbacks of a lock-on in progress
}

// phase 51d: a cab part found MOVING (or a stored part the lock-on skips as a remembered moving one) -> the profile drops it
void ReportMoved(uint32_t ic, uint32_t stride, uint32_t fmt) {
    FpsProfile::CabPart pt;
    pt.ic = ic; pt.stride = stride; pt.fmt = fmt;
    g.movedQ[g.cabMovedSerial % St::kMovedQ] = pt;
    g.cabMoved = pt;
    ++g.cabMovedSerial;
}

// phase 51d: turn the box's frame of lock L (L.A: that frame -> the reference's model space; L.lf) onto the CAB's axes (L.cabS),
// about the pivot. True = levelled (*TQ = the turn: box frame -> the former frame); false = no cab frame / turned too far (kept).
bool LevelLock(LockSt& L, M4* TQ) {
    if (!L.haveCabS) return false;
    double Q[3][3], deg = 0.0;
    const bool ok = CabLevel(L.A, L.cabS, Q, &deg);
    L.turnDeg = deg;
    if (!ok) { L.cabFrameRejected = true; return false; }
    CabTilt(Q, &L.tiltSide, &L.tiltFore, &L.tiltYaw);
    *TQ = LevelT(Q, L.lf.pivot);
    L.A = Mul(L.A, *TQ);
    L.lf.yaw0 = 0.0;                                    // (the cab's forward is the box's forward now)
    L.level = true;
    return true;
}

// phase 51b: place lock L from a stored cab pose. Phase 51c: the pose holds the box in up to 3 cab parts; every stored part that is
// one of L's carriers (ic + stride + format; not demoted, not a remembered moving part) gives a reference map A_ref = T_j^-1 A_j.
// Rigid parts give the same map; a part that moved since the pose was stored (the steering wheel turned) gives another one. The
// largest group of maps whose box corners agree within kCabAgreeM wins (ties: the larger summed IndexCount); two or more found
// parts that all disagree = ambiguous: not placed (the play space then). One found part is used as it is.
constexpr double kCabAgreeM = 0.01;
bool ApplyCab(LockSt& L, const FpsProfile::CabPose& c, const Settings& set, bool report) {
    L.cabFound = L.cabAgree = 0; L.cabAmbiguous = false; L.cabUsedIc = 0;
    if (!FpsProfile::CabPoseOk(c)) return false;
    struct Cnd { M4 A; uint32_t ic; double corner[5][3]; };
    Cnd cd[FpsProfile::kCabParts];
    int nc = 0;
    double pc[4][3], cc[3];
    BoxPlay(set, CabFrame(c), pc, cc);
    for (int i = 0; i < c.Parts() && nc < FpsProfile::kCabParts; ++i) {
        const FpsProfile::CabPart pt = c.Part(i);
        if (IsMovingGeom(pt.ic, pt.stride, pt.fmt)) {     // phase 51d: a stored part known to move -> dropped from the pose
            if (report) ReportMoved(pt.ic, pt.stride, pt.fmt);
            continue;
        }
        for (int j = 0; j < L.n; ++j) {
            const Entry& en = L.e[j];
            if (en.demoted || en.ic != pt.ic || en.stride != pt.stride || en.fmt != pt.fmt) continue;
            FpsProfile::CabPose one = c;
            one.SetPart(0, pt);
            Cnd& x = cd[nc];
            if (!RefMapFromCab(one, en.T, &x.A)) break;
            x.ic = pt.ic;
            for (int k = 0; k < 5; ++k) {
                const double h[4] = { k < 4 ? pc[k][0] : cc[0], k < 4 ? pc[k][1] : cc[1], k < 4 ? pc[k][2] : cc[2], 1.0 };
                double o[4];
                Apply(x.A, h, o);
                for (int q = 0; q < 3; ++q) x.corner[k][q] = o[q];
            }
            ++nc;
            break;
        }
    }
    L.cabFound = nc;
    if (!nc) return false;
    int best = -1, bestN = 0;
    uint64_t bestSum = 0;
    for (int a = 0; a < nc; ++a) {
        int m = 0;
        uint64_t sum = 0;
        for (int b = 0; b < nc; ++b) {
            double d = 0.0;
            for (int k = 0; k < 5; ++k) {
                const double t[3] = { cd[a].corner[k][0] - cd[b].corner[k][0], cd[a].corner[k][1] - cd[b].corner[k][1], cd[a].corner[k][2] - cd[b].corner[k][2] };
                d = std::fmax(d, Len3(t) * L.kRef);
            }
            if (d < kCabAgreeM) { ++m; sum += cd[b].ic; }
        }
        if (m > bestN || (m == bestN && sum > bestSum)) { best = a; bestN = m; bestSum = sum; }
    }
    L.cabAgree = bestN;
    if (nc >= 2 && bestN < 2) { L.cabAmbiguous = true; return false; }
    const M4& A = cd[best].A;
    L.A = A;
    L.lf = CabFrame(c);
    const double col[3] = { A.m[0][0], A.m[1][0], A.m[2][0] };
    const double upm = Len3(col);
    if (upm > 1e-9) { L.upm = upm; L.playPerGame = 1.0 / (upm * L.kRef); }
    L.placed = kPlacedCab;
    L.hasCab = true;
    L.cab = c;
    L.cabUsedIc = cd[best].ic;
    L.cabMissing = false;
    // phase 51d: an older pose (built in its lock's play space) is levelled onto the cab once -- every stored part turned by the same
    // T_Q (they share the pose's frame); the profile stores the levelled pose ("cabsq"). A square pose is used as it is (its residual
    // turn against this lock's cab frame is only logged).
    L.level = c.level; L.leveledOld = false; L.cabFrameRejected = false; L.turnDeg = 0.0;
    L.tiltSide = L.tiltFore = L.tiltYaw = 0.0;
    L.v2p = Identity();
    if (!c.level) {
        M4 TQ;
        if (LevelLock(L, &TQ)) {
            FpsProfile::CabPose lc = c;
            for (int i = 0; i < lc.Parts(); ++i) {
                FpsProfile::CabPart pt = lc.Part(i);
                M4 Aj = Identity();
                for (int r = 0; r < 3; ++r) for (int q = 0; q < 4; ++q) Aj.m[r][q] = pt.A[r][q];
                const M4 Al = Mul(Aj, TQ);
                for (int r = 0; r < 3; ++r) for (int q = 0; q < 4; ++q) pt.A[r][q] = Al.m[r][q];
                lc.SetPart(i, pt);
            }
            lc.yaw0 = 0.0;
            lc.level = true;
            L.cab = lc;
            L.leveledOld = true;
        }
    } else if (L.haveCabS) {
        double Q[3][3], deg = 0.0;
        CabLevel(L.A, L.cabS, Q, &deg);
        L.turnDeg = deg;                                // (the residual: ~0 for a rigid cab)
    }
    return true;
}

const char* PlacedText(const LockSt& L, char* b, size_t n) {
    switch (L.placed) {
    case kPlacedCab: snprintf(b, n, "the STORED CAB POSE of this vehicle (part IndexCount %u, %d of %d stored part(s) found agree; no play -> cab map "
                              "of this lock-on used)", L.cabUsedIc, L.cabAgree, L.cabFound); break;
    case kPlacedSameCab: snprintf(b, n, "the previous lock's map (a re-lock on the same cab)"); break;
    case kPlacedPlayAvg: snprintf(b, n, "the play space, averaged over %d lock readbacks (%d kept, spread %.1f mm)%s", L.samples, L.samplesUsed,
                                  L.spreadMm, L.cabMissing ? " -- the stored cab pose's part is not among the rigid parts" : ""); break;
    case kPlacedPlay: snprintf(b, n, "the play space, one lock readback%s", L.cabMissing ? " -- the stored cab pose's part is not among the rigid parts" : ""); break;
    default: snprintf(b, n, "-"); break;
    }
    return b;
}

// ---- snapshots ------------------------------------------------------------------------------------------------------------
int OpenSnap(ID3D11DeviceContext* ctx, int kind, bool xrLocal, uint64_t frame, uint64_t serial) {
    for (int s = 0; s < kRb; ++s) {
        Snap& sn = g.snap[s];
        if (sn.state != 0) continue;
        if (!sn.buf) {
            ComPtr<ID3D11Device> dev;
            ctx->GetDevice(&dev);
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = kStageBytes;
            bd.Usage = D3D11_USAGE_STAGING;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (!dev || FAILED(dev->CreateBuffer(&bd, nullptr, &sn.buf))) {
                static int s_fail = 0;
                if (s_fail++ < 3) LogErr("fps box attached to vehicle: staging buffer (%u bytes) create failed", kStageBytes);
                return -1;
            }
        }
        sn.state = 1; sn.kind = kind; sn.gen = g.lk.gen; sn.blit = g.blits; sn.frame = frame; sn.serial0 = serial; sn.xrLocal = xrLocal;
        for (SnapEye& e : sn.eye) e = SnapEye();
        g.open = s;
        return s;
    }
    return -1;
}

void CopyCands(ID3D11DeviceContext* ctx, DrawIdRecord& rec, Snap& sn, int eye) {
    ComPtr<ID3D11Resource> mirror;
    rec.MirrorSrv()->GetResource(&mirror);
    SnapEye& se = sn.eye[eye];
    for (int j = 0; j < se.n; ++j) {
        const Cand& c = se.c[j];
        UINT bytes = (c.rows - 4u) * 16u;
        if (bytes > kRowBytes) bytes = kRowBytes;
        const D3D11_BOX box{ c.off, 0, 0, c.off + bytes, 1, 1 };
        ctx->CopySubresourceRegion(sn.buf.Get(), 0, ((UINT)eye * (UINT)kCand + (UINT)j) * kRowBytes, 0, 0, mirror.Get(), 0, &box);
    }
    se.done = true;
}

// the rows of candidate j of eye e (rows4 = cb0 rows 4.., n rows)
int RowsOf(const float* data, int e, int j, const Cand& c, const float** rows4) {
    *rows4 = data + ((size_t)(e * kCand + j) * kRowBytes) / sizeof(float);
    int n = (int)c.rows - 4;
    return n > 8 ? 8 : n;
}

// ---- phase 47d: MOVING PARTS remembered for the session ------------------------------------------------------------------
// A part the monitor found moving (steering wheel, door, wiper) is remembered by its full key and its loose key (IndexCount, start
// index, base vertex, VS: no buffer pointers, so it survives the game's buffer-pool rotation). One list for the session, not one
// per truck: a key is the geometry of one draw of one cab, so another cab's parts do not match it, and going back to a truck
// finds its wheel again. Every lock-on (also after the exterior camera / menus / a re-lock / a cab change) refuses those parts.
// Dropped at a device change (the VS pointers in the keys change).
constexpr int kMoveMax = 64;
// Phase 51c: also by GEOMETRY (IndexCount + VB0 stride + index format): the ATS 14:40 test remembered the steering wheel (IndexCount
// 13200) by its keys, the keys changed (47e), and the next two lock-ons took the wheel as their reference again.
struct Moving { uint64_t full, loose; uint32_t ic, stride, fmt; };
Moving g_moving[kMoveMax];
int g_nMoving = 0, g_linesMoving = 0;
bool IsMoving(uint64_t full, uint64_t loose) {
    for (int i = 0; i < g_nMoving; ++i) if (g_moving[i].full == full || g_moving[i].loose == loose) return true;
    return false;
}
bool IsMovingGeom(uint32_t ic, uint32_t stride, uint32_t fmt) {
    for (int i = 0; i < g_nMoving; ++i) if (g_moving[i].ic == ic && g_moving[i].stride == stride && g_moving[i].fmt == fmt) return true;
    return false;
}
void Remember(const Entry& en) {
    if (IsMoving(en.full, en.loose) && IsMovingGeom(en.ic, en.stride, en.fmt)) return;
    if (g_nMoving == kMoveMax) { memmove(g_moving, g_moving + 1, sizeof(Moving) * (kMoveMax - 1)); --g_nMoving; }
    g_moving[g_nMoving++] = Moving{ en.full, en.loose, en.ic, en.stride, en.fmt };
    if (g_linesMoving < kLogLines) {
        ++g_linesMoving;
        Log("fps box attached to vehicle: cab part IndexCount %u REMEMBERED as moving for this session (%d remembered) -- no lock-on / "
            "re-acquire will use it again", en.ic, g_nMoving);
    }
}

const char* CandVerdict(const float* rows4, int nr, M4* M, int* row, EyeFrame* ef) {
    *row = FindMvpRow(rows4, nr, M);
    if (!*row) return "no MVP-shaped block";
    if (OriginFree(*M)) return "camera-relative (origin at the eye)";
    if (!EyeFrameOf(*M, ef)) return "degenerate";
    if (!(ef->k > 1e-3 && ef->k < 1e3)) return "scale out of range";
    if (Len3(ef->C) * ef->k > 30.0) return "its origin is over 30 m from the eye";
    return nullptr;
}

void ProcessLock(Snap& sn, const float* data) {
    struct V { bool ok = false; M4 M; int row = 0; EyeFrame ef; const char* why = "absent"; };
    // keys over both eyes, eye 0's order first (biggest IndexCount first in each)
    uint64_t keys[2 * kCand]; uint32_t ics[2 * kCand]; uint64_t looses[2 * kCand];
    uint32_t strides[2 * kCand], fmts[2 * kCand];       // phase 51b
    int nk = 0;
    V v[2 * kCand][2];
    for (int e = 0; e < 2; ++e)
        for (int j = 0; j < sn.eye[e].n; ++j) {
            const Cand& c = sn.eye[e].c[j];
            int k = 0;
            while (k < nk && keys[k] != c.full) ++k;
            if (k == nk) { keys[nk] = c.full; ics[nk] = c.ic; looses[nk] = c.loose; strides[nk] = c.stride; fmts[nk] = c.fmt; ++nk; }
            const float* rows4;
            const int nr = RowsOf(data, e, j, c, &rows4);
            V& x = v[k][e];
            x.why = CandVerdict(rows4, nr, &x.M, &x.row, &x.ef);
            if (!x.why && (IsMoving(c.full, c.loose) || IsMovingGeom(c.ic, c.stride, c.fmt))) x.why = "a remembered MOVING part (phase 47d / 51c)";
            x.ok = x.why == nullptr;
        }
    // sort the keys by IndexCount (biggest first; stable)
    int ord[2 * kCand];
    for (int k = 0; k < nk; ++k) ord[k] = k;
    for (int a = 1; a < nk; ++a)
        for (int b = a; b > 0 && ics[ord[b]] > ics[ord[b - 1]]; --b) { const int t = ord[b]; ord[b] = ord[b - 1]; ord[b - 1] = t; }
    // phase 47c: the RIGID CAB GROUP. Parts fixed to the cab share one model transform, so in one eye's pass they carry the SAME
    // MVP; the steering wheel / doors / wipers / needles have their own. The keys usable in both eyes are grouped by identical
    // MVP in both eyes (SameTransform); the group with the largest summed IndexCount is the cab (summed, not counted: one big
    // shell outweighs a cluster of tiny gauge draws), its biggest member the reference, and only its members carry the box
    // (ATS car -> truck test: the biggest draw, the steering wheel, was picked and rode the wheel until the monitor caught it).
    int grp[2 * kCand], rep[2 * kCand], gn[2 * kCand] = {};
    uint64_t gsum[2 * kCand] = {};
    int ng = 0;
    for (int q = 0; q < nk; ++q) {
        const int k = ord[q];
        grp[k] = -1;
        if (!(v[k][0].ok && v[k][1].ok)) continue;
        int j = 0;
        while (j < ng && !(SameTransform(v[k][0].M, v[rep[j]][0].M) && SameTransform(v[k][1].M, v[rep[j]][1].M))) ++j;
        if (j == ng) rep[ng++] = k;                      // (ord = biggest first: a group's rep is its biggest member)
        grp[k] = j; ++gn[j]; gsum[j] += ics[k];
    }
    int bestG = -1;
    for (int j = 0; j < ng; ++j) if (bestG < 0 || gsum[j] > gsum[bestG]) bestG = j;
    int ref = bestG >= 0 ? rep[bestG] : -1;
    int others = 0;
    for (int q = 0; q < nk; ++q) if (grp[ord[q]] >= 0 && grp[ord[q]] != bestG) ++others;
    {                                                    // phase 47d: the remembered moving parts this lock-on skipped (level 3)
        int skip = 0;
        uint32_t skipIc = 0;
        for (int k = 0; k < nk; ++k)
            if (IsMoving(keys[k], looses[k]) || IsMovingGeom(ics[k], strides[k], fmts[k])) { ++skip; if (ics[k] > skipIc) skipIc = ics[k]; }
        if (skip && g.acc.n == 0 && g_linesMoving < kLogLines) {   // (phase 51b: once per lock-on)
            ++g_linesMoving;
            Log("fps box attached to vehicle: lock-on SKIPS %d remembered moving part(s) (biggest IndexCount %u)", skip, skipIc);
        }
    }
    // phase 47d: a best group of ONE part (every usable part has its own transform -- the ATS 15:06 truck: 3 parts, 3 transforms)
    // proves nothing: then every usable part carries the box (ranked by IndexCount) and the monitor sorts out the moving ones
    const bool groupOnly = bestG >= 0 && gn[bestG] >= 2;
    // phase 47b: BOTH eyes or no lock (the ATS 14:32 lock #1 was eye 1 only: a loading picture, no IPD, no pose) -- one eye is
    // refused below, the lock is tried again
    const bool lg4 = (LogLevel() >= 4);
    if (lg4 && g.acc.n == 0 && g.linesLock < kLogLines) {   // (phase 51b: the first readback of a lock-on only)
        ++g.linesLock;
        char buf[1800];
        int o = 0;
        for (int q = 0; q < nk && o < (int)sizeof(buf) - 120; ++q) {
            const int k = ord[q];
            o += snprintf(buf + o, sizeof(buf) - (size_t)o, "%s[IndexCount %u: eye0 %s%s, eye1 %s%s]", q ? " " : "", ics[k],
                          v[k][0].ok ? "ok rows " : v[k][0].why, v[k][0].ok ? (v[k][0].row == 4 ? "4..7" : v[k][0].row == 5 ? "5..8"
                          : v[k][0].row == 6 ? "6..9" : v[k][0].row == 7 ? "7..10" : "8..11") : "",
                          v[k][1].ok ? "ok rows " : v[k][1].why, v[k][1].ok ? (v[k][1].row == 4 ? "4..7" : v[k][1].row == 5 ? "5..8"
                          : v[k][1].row == 6 ? "6..9" : v[k][1].row == 7 ? "7..10" : "8..11") : "");
        }
        LogDbg("fps box attached to vehicle: lock-on readback -- %d cab draw(s) (eye 0: %d, eye 1: %d; biggest IndexCount first): %s -> "
               "reference: %s", nk, sn.eye[0].n, sn.eye[1].n, nk ? buf : "none",
               ref < 0 ? "NONE usable in both eyes" : "the biggest part usable in both eyes");
    }
    // phase 47b: no OpenXR pose of either eye = refused too (the box would be placed from the eye's own view: not where lock 1 is)
    const bool posesOk = sn.eye[0].xrOk && sn.eye[1].xrOk;
    if (ref >= 0 && !posesOk) {
        ++g.lockRefusals;
        g.nextLock = g.blits + kLockRefuseRetry;
        if (g.linesLock < kLogLines) {
            ++g.linesLock;
            Log("fps box attached to vehicle: lock-on REFUSED -- no usable OpenXR pose of %s (the views located in a VIEW space / "
                "untracked: a loading screen?); trying again", !sn.eye[0].xrOk && !sn.eye[1].xrOk ? "either eye" : (!sn.eye[0].xrOk ? "eye 0" : "eye 1"));
        }
        return;
    }
    if (ref < 0) {
        ++g.lockFails;
        if (sn.eye[0].n == 0 || sn.eye[1].n == 0) ++g.lockRefusals;
        g.nextLock = g.blits + ((sn.eye[0].n == 0 || sn.eye[1].n == 0) ? kLockRefuseRetry : kLockRetryBlits);
        if (g.linesLock < kLogLines) {
            ++g.linesLock;
            Log("fps box attached to vehicle: no cab draw usable in BOTH eyes in the lock-on readback (%d candidate(s), eye 0: %d, eye 1: "
                "%d; try %d) -- the box stays fixed in the play space, trying again", nk, sn.eye[0].n, sn.eye[1].n, g.lockFails);
        }
        return;
    }
    const int ea = v[ref][0].ok ? 0 : 1;                 // the eye the anchor maps come from
    const bool both = v[ref][0].ok && v[ref][1].ok;
    LockSt L;
    // phase 51d: the CAB FRAME -- the model rotation shared by the largest summed IndexCount of this eye's cab draws (camera-relative
    // ones included: their rotation is the cab's), not a remembered moving part, not head-locked (a bare projection)
    struct RotG { EyeFrame ef; uint64_t sum = 0; int n = 0, nCamRel = 0; uint32_t ic = 0; };
    RotG rg[2 * kCand];
    int nrg = 0;
    for (int q = 0; q < nk; ++q) {
        const int k = ord[q];
        const V& x = v[k][ea];
        if (!x.row) continue;
        if (IsMoving(keys[k], looses[k]) || IsMovingGeom(ics[k], strides[k], fmts[k])) continue;
        EyeFrame ef;
        if (!EyeFrameOf(x.M, &ef) || HeadLocked(ef)) continue;
        int j = 0;
        while (j < nrg && !SameRot(rg[j].ef, ef)) ++j;
        if (j == nrg) { if (nrg == 2 * kCand) continue; rg[nrg] = RotG(); rg[nrg].ef = ef; rg[nrg].ic = ics[k]; ++nrg; }
        rg[j].sum += ics[k]; ++rg[j].n;
        if (OriginFree(x.M)) ++rg[j].nCamRel;
    }
    int bestR = -1;
    for (int j = 0; j < nrg; ++j) if (bestR < 0 || rg[j].sum > rg[bestR].sum) bestR = j;
    if (bestR >= 0) {
        FrameRot(v[ref][ea].ef, rg[bestR].ef, L.cabS);
        L.haveCabS = true;
        L.cabFrameDraws = rg[bestR].n; L.cabFrameIc = rg[bestR].ic; L.cabFrameSum = rg[bestR].sum;
    }
    L.gen = g.lk.gen;
    L.frame = sn.frame;
    L.xr = sn.eye[ea].xrOk && (!both || sn.eye[1 - ea].xrOk);
    L.local = sn.xrLocal;
    EyeFrame ef[2];
    Pose ps[2];
    int n = 1;
    bool swapped = false;
    double ipdRatio = 0.0;
    if (L.xr) {
        if (both) { ef[0] = v[ref][0].ef; ef[1] = v[ref][1].ef; ps[0] = sn.eye[0].pose; ps[1] = sn.eye[1].pose; n = 2; }
        else { ef[0] = v[ref][ea].ef; ps[0] = sn.eye[ea].pose; }
    } else {                                              // no usable OpenXR poses: the play space := eye ea's view at the lock
        ef[0] = v[ref][ea].ef;
        ps[0] = Pose{ { 0.0, 0.0, 0.0 }, { 0.0, 0.0, 0.0, 1.0 } };
    }
    if (!PlayToModel(ef, ps, n, &L.A, &L.upm, &swapped, &ipdRatio)) {
        ++g.lockFails;
        g.nextLock = g.blits + kLockRetryBlits;
        if (g.linesLock < kLogLines) { ++g.linesLock; Log("fps box attached to vehicle: the play -> cab map is degenerate (try %d)", g.lockFails); }
        return;
    }
    // the anchor frame (mode 1's rule): LOCAL = around the seat; else around the eyes' midpoint, forward = the head's yaw
    if (L.xr && !L.local) {
        const Pose& a = sn.eye[ea].pose;
        const Pose& b = both ? sn.eye[1 - ea].pose : a;
        for (int i = 0; i < 3; ++i) L.lf.pivot[i] = 0.5 * (a.p[i] + b.p[i]);
        const double f0[3] = { 0.0, 0.0, -1.0 };
        double fw[3];
        QRot(a.q, f0, fw);
        L.lf.yaw0 = std::atan2(fw[0], -fw[2]);
    }
    L.lf.fw = L.xr ? sn.eye[ea].fw : 2.0 / v[ref][ea].ef.p00;
    if (!(L.lf.fw > 0.1 && L.lf.fw < 20.0)) L.lf.fw = 2.0 / v[ref][ea].ef.p00;
    L.playPerGame = 1.0 / (L.upm * v[ref][ea].ef.k);
    L.kRef = v[ref][ea].ef.k;
    L.swapped = swapped;
    L.eyesUsed = n;
    // the entries: the reference first, then every other part usable in the anchor eye (biggest first); T = M_c^-1 M_ref
    double maxEyeDiffMm = 0.0;
    auto addEntry = [&](int k) {
        if (L.n >= kCand) return;
        int e = v[k][ea].ok ? ea : (v[k][1 - ea].ok && v[ref][1 - ea].ok ? 1 - ea : -1);
        if (e < 0) return;
        M4 inv;
        if (!Inv(v[k][e].M, &inv)) return;
        Entry& en = L.e[L.n];
        en = Entry();
        en.full = keys[k]; en.loose = looses[k]; en.ic = ics[k]; en.row = v[k][e].row;
        en.stride = strides[k]; en.fmt = fmts[k];       // phase 51b
        en.T = Mul(inv, v[ref][e].M);
        if (!Finite(en.T)) return;
        // the other eye must give the same part-to-part transform (a rigid pair seen twice): its size in mm at 1 m (log)
        const int o = 1 - e;
        if (k != ref && v[k][o].ok && v[ref][o].ok) {
            M4 inv2;
            if (Inv(v[k][o].M, &inv2)) {
                const M4 T2 = Mul(inv2, v[ref][o].M);
                for (int c = 0; c < 4; ++c) {
                    const double p[4] = { c == 1 ? 1.0 / v[ref][e].ef.k : 0.0, c == 2 ? 1.0 / v[ref][e].ef.k : 0.0,
                                          c == 3 ? 1.0 / v[ref][e].ef.k : 0.0, 1.0 };
                    double x1[4], x2[4];
                    Apply(en.T, p, x1); Apply(T2, p, x2);
                    if (std::fabs(x1[3]) > 1e-12 && std::fabs(x2[3]) > 1e-12) {
                        double s = 0.0;
                        for (int i = 0; i < 3; ++i) { const double t = x1[i] / x1[3] - x2[i] / x2[3]; s += t * t; }
                        maxEyeDiffMm = std::fmax(maxEyeDiffMm, std::sqrt(s) * v[k][o].ef.k * 1000.0);
                    }
                }
            }
        }
        ++L.n;
    };
    addEntry(ref);
    for (int q = 0; q < nk; ++q)                                     // phase 47c: the cab group only (47d: all when it is one part)
        if (ord[q] != ref && grp[ord[q]] >= 0 && (!groupOnly || grp[ord[q]] == bestG)) addEntry(ord[q]);
    L.have = true;
    {                                                    // phase 51: the vehicle key -- every cab draw of both eyes' readback
        FpsProfile::Part parts[2 * kCand], used[FpsProfile::kKeyParts];
        int np = 0, nu = 0;
        for (int e = 0; e < 2; ++e)
            for (int j = 0; j < sn.eye[e].n; ++j) parts[np++] = FpsProfile::Part{ sn.eye[e].c[j].ic, sn.eye[e].c[j].stride, sn.eye[e].c[j].fmt };
        L.vehicleKey = FpsProfile::VehicleKey(parts, np, used, &nu);
        if (g.acc.n == 0 && g.linesKey < kLogLines) {   // (phase 51b: once per lock-on, not per readback)
            ++g.linesKey;
            char b[160];
            int o = 0;
            for (int k = 0; k < nu && o < (int)sizeof(b) - 40; ++k)
                o += snprintf(b + o, sizeof(b) - (size_t)o, "%s%u/%u/%u", k ? ", " : "", used[k].ic, used[k].stride, used[k].fmt);
            Log("fps box attached to vehicle: vehicle key %016llx from the %d biggest cab draw(s) (IndexCount/stride/index format: %s)",
                (unsigned long long)L.vehicleKey, nu, nu ? b : "none");
        }
    }
    // phase 51b: a STORED CAB POSE of this vehicle (the provider: inject.cpp's per-vehicle profiles) places the box -- the same spot
    // in the cab at every lock-on, no play -> cab map of this lock-on involved (its pose error cannot move the box)
    bool fromCab = false;
    if (g.remeasure) { L.remeasured = true; g.havePrev = false; g.prev = LockSt(); }   // phase 51c: a fresh map, asked for
    else if (g.provider && L.vehicleKey) {
        FpsProfile::CabPose cp;
        if (g.provider(L.vehicleKey, &cp)) {
            fromCab = ApplyCab(L, cp, g.set, g.acc.n == 0 && g.acc.rejected == 0);   // (phase 51d: report once per lock-on)
            if (!fromCab) { L.cabMissing = !L.cabAmbiguous; L.missingIc = cp.ic; }
        }
    }
    // phase 47e: a re-lock on the same cab keeps the box's spot in the cab. A new entry i and an old one j with the same IndexCount
    // are the same mesh: play space -> old reference model -> part j model (= part i model) -> new reference model.
    if (fromCab) { g.havePrev = false; g.prev = LockSt(); }
    if (!fromCab && g.havePrev) {
        const LockSt& P = g.prev;
        int bi = -1, bj = -1;
        for (int i = 0; i < L.n && bi < 0; ++i)
            for (int j = 0; j < P.n; ++j) if (L.e[i].ic == P.e[j].ic) { bi = i; bj = j; break; }
        M4 inv, A2;
        if (bi >= 0 && Inv(L.e[bi].T, &inv)) {
            A2 = Mul(inv, Mul(P.e[bj].T, P.A));
            if (Finite(A2)) {
                L.A = A2;
                L.lf = P.lf; L.upm = P.upm; L.playPerGame = P.playPerGame; L.xr = P.xr; L.local = P.local;
                L.level = P.level; L.turnDeg = P.turnDeg; L.tiltSide = P.tiltSide; L.tiltFore = P.tiltFore; L.tiltYaw = P.tiltYaw;   // phase 51d
                L.v2p = P.v2p;
                L.sameCab = true;                        // phase 51
                L.placed = kPlacedSameCab;               // phase 51b
                if (g.linesLock < kLogLines) {
                    ++g.linesLock;
                    Log("fps box attached to vehicle: re-lock on the same cab (IndexCount %u was in lock #%llu) -- the box keeps its spot in the cab",
                        L.e[bi].ic, (unsigned long long)g.prevLockNo);
                }
            }
        }
        g.havePrev = false;
        g.prev = LockSt();
    }
    // phase 51b: from the play space -- the map AVERAGED over several lock readbacks (outliers dropped: AverageMaps), not one frame's
    if (L.placed == kPlacedNone) {
        // phase 51c: a BROKEN map is never used -- the two eyes' camera centres must be the headset's IPD apart (ATS 14:43:32 lock #7:
        // "game IPD / headset IPD 0.006", the two pictures from one camera). Up to 240 such readbacks (~2 .. 5 s) are skipped, then accepted (a game IPD that really differs).
        if (n == 2 && !(ipdRatio >= 0.8 && ipdRatio <= 1.25) && g.acc.rejected < 240) {
            ++g.acc.rejected;
            if (g.linesIpd < kLogLines) {
                ++g.linesIpd;
                Log("fps box attached to vehicle: lock readback NOT USED -- game IPD / headset IPD %.3f (outside 0.8 .. 1.25: a broken play "
                    "-> cab map); trying the next one (%d skipped)", ipdRatio, g.acc.rejected);
            }
            g.nextLock = 0;
            return;
        }
        L.placed = kPlacedPlay; L.samples = L.samplesUsed = 1;
        if (g.lockSamples > 1) {
            St::Acc& a = g.acc;
            const Entry& r = L.e[0];
            if (a.n && (a.ic != r.ic || a.stride != r.stride || a.fmt != r.fmt)) { a.n = 0; ++a.restarts; }   // another reference
            if (!a.n) { a.ic = r.ic; a.stride = r.stride; a.fmt = r.fmt; }
            if (a.n < kMaxLockSamples) a.A[a.n++] = L.A;
            const int want = g.lockSamples < kMaxLockSamples ? g.lockSamples : kMaxLockSamples;
            if (a.n < want && a.restarts < 4) { g.nextLock = 0; return; }   // the next readback (the lock-on goes on)
            double cp[4][3], cc[3];
            BoxPlay(g.set, L.lf, cp, cc);
            const double probe[5][3] = { { cp[0][0], cp[0][1], cp[0][2] }, { cp[1][0], cp[1][1], cp[1][2] }, { cp[2][0], cp[2][1], cp[2][2] },
                                         { cp[3][0], cp[3][1], cp[3][2] }, { cc[0], cc[1], cc[2] } };
            M4 avg;
            double sp = 0.0;
            const int used = AverageMaps(a.A, a.n, probe, 5, 0.002 * L.upm, &avg, &sp);
            if (used > 0) {
                L.A = avg; L.placed = kPlacedPlayAvg; L.samples = a.n; L.samplesUsed = used;
                L.spreadMm = L.upm > 0.0 ? sp / L.upm * 1000.0 : 0.0;
            }
            a = St::Acc();
        }
        M4 TQ;                                           // phase 51d: square to the cab (the play space only gives the pivot + scale)
        if (LevelLock(L, &TQ)) L.v2p = TQ;
    }
    g.acc = St::Acc();                                  // (the readbacks of this lock-on, the rejected count)
    g.remeasure = false;
    g.lk = L;
    BuildCorners();
    g.maxDev = g.sumDev2 = g.maxJump = 0.0;             // (the cab-vs-play-space figures restart with the lock)
    g.nDev = 0;
    ++g.locks;
    g.lockFails = 0;
    for (EyeCur& c : g.eye) { c.lastEntry = -1; c.noMatch = 0; }
    g.lastMonitor = g.blits;
    double cp[4][3], cc[3], wM = 0.0;
    BoxPlay(g.set, L.lf, cp, cc, &wM);
    {                                                    // (the box centre in the play space of the lock: through the cab turn)
        const double h[4] = { cc[0], cc[1], cc[2], 1.0 };
        double o[4];
        Apply(L.v2p, h, o);
        cc[0] = o[0]; cc[1] = o[1]; cc[2] = o[2];
    }
    char pt[200], cf[320];
    if (!L.haveCabS) snprintf(cf, sizeof(cf), "NO cab frame (no usable cab draw rotation): the box is built in the play space");
    else {
        const int o = snprintf(cf, sizeof(cf), "cab frame from %d draw(s) sharing one rotation (IndexCount sum %llu, biggest %u); ", L.cabFrameDraws,
                               (unsigned long long)L.cabFrameSum, L.cabFrameIc);
        if (L.cabFrameRejected)
            snprintf(cf + o, sizeof(cf) - (size_t)o, "NOT used: %.1f deg from the box's frame (over %.0f) -- the box is built in the play space",
                     L.turnDeg, kCabLevelMaxDeg);
        else if (L.placed == kPlacedCab && !L.leveledOld)
            snprintf(cf + o, sizeof(cf) - (size_t)o, "the stored pose is square to the cab already (%.2f deg residual)", L.turnDeg);
        else if (L.level)
            snprintf(cf + o, sizeof(cf) - (size_t)o, "the box is SQUARE TO THE CAB -- %s turned %.2f deg onto it (the cab leans %+.1f deg sideways, "
                     "%+.1f deg forward and is turned %+.1f deg against %s)", L.leveledOld ? "the OLDER stored pose (play-space built)" : "its frame",
                     L.turnDeg, L.tiltSide, L.tiltFore, L.tiltYaw, L.leveledOld ? "that pose's frame" : "the play space");
        else snprintf(cf + o, sizeof(cf) - (size_t)o, "not levelled");
    }
    if (g.linesLock < kLogLines) {
        ++g.linesLock;
        Log("fps box attached to vehicle: LOCKED ON (lock #%llu) -- reference cab draw IndexCount %u (MVP in cb0 rows %d..%d), seen in %s; "
            "%d cab part(s) of its rigid group (IndexCount sum %llu, the largest of %d transform group(s); %d part(s) with their own "
            "transform -- wheel / doors / needles -- not used) can carry the box; play -> cab map from %s%s (game IPD / headset IPD %.3f, %.1f model units per m); "
            "anchor %s, box centre (%+.3f %+.3f %+.3f) m in the play space, %.3f m wide%s; placed from %s; %s",
            (unsigned long long)g.locks, L.e[0].ic, L.e[0].row, L.e[0].row + 3, both ? "both eyes" : (ea == 0 ? "eye 0 only" : "eye 1 only"),
            L.n, (unsigned long long)gsum[bestG], ng, others, L.xr ? (n == 2 ? "both eyes' OpenXR poses" : "one eye's OpenXR pose") : "NO OpenXR pose (the box placed from the eye's own "
            "view at the lock)", swapped ? ", the eye order swapped (OpenXR view vs eye picture)" : "", ipdRatio, L.upm,
            !L.xr ? "straight ahead of the eye" : (L.local ? "around the seat (LOCAL space origin)" : "around the eyes' midpoint at the lock"),
            cc[0], cc[1], cc[2], wM, groupOnly ? "" : " -- NO shared transform among the parts (each its own group): all usable "
            "parts carry the box, the monitor (every ~0.08 s) drops a moving one", PlacedText(L, pt, sizeof(pt)), cf);
    }
    if (lg4 && g.linesLock < kLogLines) {
        ++g.linesLock;
        LogDbg("fps box attached to vehicle: part-to-part transforms of eye %d vs the other eye differ by %.2f mm at most (1 m from "
               "the reference's origin; 0 = a rigid pair seen identically by both eyes)", ea, maxEyeDiffMm);
    }
    if (lg4 && bestR >= 0 && g.linesLock < kLogLines) {  // phase 51d: the rotation groups against the cab frame (evidence)
        ++g.linesLock;
        char b[900];
        int o = 0;
        for (int j = 0; j < nrg && o < (int)sizeof(b) - 120; ++j) {
            double R[3][3];
            FrameRot(rg[bestR].ef, rg[j].ef, R);
            const double c = std::fmax(-1.0, std::fmin(1.0, 0.5 * (R[0][0] + R[1][1] + R[2][2] - 1.0)));
            o += snprintf(b + o, sizeof(b) - (size_t)o, "%s[%d draw(s) from IndexCount %u, sum %llu, %d camera-relative: %.2f deg]", j ? " " : "",
                          rg[j].n, rg[j].ic, (unsigned long long)rg[j].sum, rg[j].nCamRel, std::acos(c) * 57.29577951308232);
        }
        LogDbg("fps box attached to vehicle: cab draw rotation groups of eye %d against the cab frame (the heaviest): %s", ea, b);
    }
}

void ProcessMonitor(Snap& sn, const float* data) {
    LockSt& L = g.lk;
    ++g.wMon;
    ++g.monitors;
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceCounter(&q0);
    struct Tm { LARGE_INTEGER& a; LARGE_INTEGER& b; LARGE_INTEGER& f; ~Tm() { QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
        g.monitorUs += (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart; } } tm{ q0, q1, qf };
    for (int e = 0; e < 2; ++e) {
        const SnapEye& se = sn.eye[e];
        if (!se.done || se.n < 1) continue;
        M4 M[kCand];
        int ent[kCand], m = 0;
        for (int j = 0; j < se.n && m < kCand; ++j) {
            const Cand& c = se.c[j];
            if (c.ent < 0 || c.ent >= L.n) continue;
            const float* rows4;
            const int nr = RowsOf(data, e, j, c, &rows4);
            const int r = L.e[c.ent].row;
            if (r - 4 + 4 > nr) continue;
            const M4 b = FromRows(rows4 + (r - 4) * 4);
            if (!Finite(b) || !ShapeOk(b)) continue;
            M[m] = b; ent[m] = c.ent; ++m;
        }
        // stats: where the attached box is in the play space now (eye 0's current part)
        if (e == 0 && se.xrOk && L.xr) {
            const int use = g.eye[0].lastEntry;          // (the part eye 0 used last; .entry is cleared at each blit)
            for (int q = 0; q < m; ++q) {
                if (ent[q] != use) continue;
                double P[3];
                if (PlayOf(M[q], L.e[use].ac, se.pose, L.playPerGame, P)) {
                    const double d[3] = { P[0] - L.centrePlay[0], P[1] - L.centrePlay[1], P[2] - L.centrePlay[2] };
                    const double dev = Len3(d);
                    g.maxDev = std::fmax(g.maxDev, dev);
                    g.sumDev2 += dev * dev;
                    ++g.nDev;
                    if (g.lastHave) {
                        const double j3[3] = { P[0] - g.lastPlay[0], P[1] - g.lastPlay[1], P[2] - g.lastPlay[2] };
                        g.maxJump = std::fmax(g.maxJump, Len3(j3));
                    }
                    memcpy(g.lastPlay, P, sizeof(P));
                    g.lastHave = true;
                }
            }
        }
        if (m < 2) continue;
        // consensus: the group of parts that put the box centre at the same spot (a rigid cab)
        bool agree[kCand][kCand] = {};
        double dis[kCand][kCand] = {};
        for (int a = 0; a < m; ++a)
            for (int b = 0; b < m; ++b) {
                const double d = a == b ? 0.0 : Disagree(M[a], L.e[ent[a]].ac, M[b], L.e[ent[b]].ac);
                dis[a][b] = d;
                agree[a][b] = d >= 0.0 && d < kAgreeM;
            }
        int best = -1;
        uint64_t bestScore = 0;
        for (int a = 0; a < m; ++a) {
            if (L.e[ent[a]].demoted) continue;
            uint64_t s = 0;
            for (int b = 0; b < m; ++b) if (agree[a][b] && agree[b][a] && !L.e[ent[b]].demoted) s += L.e[ent[b]].ic;
            if (best < 0 || s > bestScore) { best = a; bestScore = s; }
        }
        if (best < 0) continue;
        for (int a = 0; a < m; ++a) {
            Entry& en = L.e[ent[a]];
            if (en.demoted || a == best) continue;
            const double d = std::fmax(dis[a][best], dis[best][a]);
            if (!(d > kDemoteM)) continue;
            en.demoted = true;
            Remember(en);                                  // phase 47d: never a carrier again on this cab (this session)
            // phase 51c / 51d: the profile drops it from the vehicle's stored pose -- also when this lock was placed from the play space
            // (its pose, stored at the lock, still holds the part: the ATS 15:02 wheel 13200 stayed first in the line)
            ReportMoved(en.ic, en.stride, en.fmt);
            if (g.linesDemote < kLogLines) {
                ++g.linesDemote;
                Log("fps box attached to vehicle: cab part IndexCount %u MOVES against the cab (it puts the box %.1f cm away from the "
                    "group led by IndexCount %u, eye %d) -- a moving part (steering wheel / door / wiper): not used again for this lock",
                    en.ic, d * 100.0, L.e[ent[best]].ic, e);
            }
        }
    }
}

void PollSnaps(ID3D11DeviceContext* ctx) {
    for (int s = 0; s < kRb; ++s) {
        Snap& sn = g.snap[s];
        if (sn.state != 2 || !sn.buf) continue;
        D3D11_MAPPED_SUBRESOURCE ms{};
        const HRESULT hr = ctx->Map(sn.buf.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) continue;
        if (FAILED(hr) || !ms.pData) { sn.state = 0; continue; }
        static float s_data[kStageBytes / sizeof(float)];
        memcpy(s_data, ms.pData, kStageBytes);
        ctx->Unmap(sn.buf.Get(), 0);
        sn.state = 0;
        if (sn.gen != g.lk.gen || !g.active) continue;   // an old lock's / a switched-off box's readback
        if (sn.kind == 1 && !g.lk.have) ProcessLock(sn, s_data);
        else if (sn.kind == 2 && g.lk.have) ProcessMonitor(sn, s_data);
    }
}

void CloseOpen() {
    if (g.open < 0) return;
    Snap& sn = g.snap[g.open];
    const bool any = sn.eye[0].done || sn.eye[1].done;
    sn.state = any ? 2 : 0;
    g.open = -1;
}

void LogWindow() {
    const ULONGLONG now = GetTickCount64();
    if (!g.winT0) { ResetWindow(); return; }
    if (now - g.winT0 < 10000) return;
    if ((LogLevel() >= 4)) {
        char wh[400];
        int o = 0;
        for (int w = 1; w < kWhyCount && o < (int)sizeof(wh) - 80; ++w)
            if (g.wWhy[w]) o += snprintf(wh + o, sizeof(wh) - (size_t)o, "%s%llu %s", o ? ", " : "", (unsigned long long)g.wWhy[w], kWhyNames[w]);
        int dem = 0;
        for (int e = 0; e < g.lk.n; ++e) if (g.lk.e[e].demoted) ++dem;
        const int r0 = g.eye[0].lastEntry, r1 = g.eye[1].lastEntry;
        LogDbg("fps box attached to vehicle (%.0f s): %llu frame(s) attached (%llu eye pictures drawn, %llu held without a box), %llu frame(s) "
               "fixed in the play space%s%s%s | monitor readbacks %llu, lock refusals %llu (total) | "
               "lock #%llu %s: part in use eye 0 %d (IndexCount %u), eye 1 %d (IndexCount %u); %d part(s), %d moving; re-acquires %llu "
               "(total %llu), new locks after a cab change %llu | THE CAB AGAINST THE PLAY SPACE (the attached box vs its spot at the lock, %llu "
               "readbacks ~0.08 s apart in this window): max %.1f cm, rms %.1f cm; largest change between two readbacks %.1f cm",
               (double)(now - g.winT0) / 1000.0, (unsigned long long)g.wFrOn, (unsigned long long)g.wAttached, (unsigned long long)g.wHeld,
               (unsigned long long)g.wFrOff, o ? " (" : "",
               o ? wh : "", o ? ")" : "", (unsigned long long)g.wMon, (unsigned long long)g.lockRefusals, (unsigned long long)g.locks, g.lk.have ? "held" : "NOT held (locking / none)", r0,
               r0 >= 0 && r0 < g.lk.n ? g.lk.e[r0].ic : 0u, r1, r1 >= 0 && r1 < g.lk.n ? g.lk.e[r1].ic : 0u, g.lk.n, dem,
               (unsigned long long)g.wReacq, (unsigned long long)g.reacq, (unsigned long long)g.relocks, (unsigned long long)g.nDev,
               g.maxDev * 100.0, g.nDev ? std::sqrt(g.sumDev2 / g.nDev) * 100.0 : 0.0, g.maxJump * 100.0);
    }
    ResetWindow();
}

}  // namespace

const char* WhyName(int why) { return why >= 0 && why < kWhyCount ? kWhyNames[why] : "?"; }

bool Active() {
    Guard gd;                                           // phase 51b: main + render thread
    return g.active;
}

void Reset(const char* why) {
    Guard gd;                                           // phase 51b: main + render thread
    const bool had = g.lk.have;
    DropLock();
    g.havePrev = false;                                 // phase 47e: a user reset = a fresh lock
    if (had && g.linesRelock < kLogLines) {
        ++g.linesRelock;
        Log("fps box attached to vehicle: lock dropped (%s) -- locking on again", why ? why : "reset");
    }
}

void SetActive(bool on, uint64_t frame) {
    Guard gd;                                           // phase 51b: main + render thread
    if (on == g.active) return;
    g.active = on;
    DropLock();
    g.havePrev = false;                                 // phase 47e
    for (EyeCur& c : g.eye) { c.srv.Reset(); c.frame = ~0ull; c.why = kNoBlit; c.noCabLogged = false; c.latchAge = 99; }
    g.lastWhy = -1;
    g.latched = false; g.onRun = g.offRun = 0; g.hist = 0; g.steady = 0;   // phase 47b
    ResetWindow();
    Log("fps box attached to vehicle: %s (Present #%llu)", on ? "ON -- locking on to the cab at the next world frame" : "off",
        (unsigned long long)frame);
}

void SetSettings(const Settings& s) {
    Guard gd;                                           // phase 51b: main + render thread
    if (s == g.set) return;
    g.set = s;
    if (g.lk.have) BuildCorners();                      // the same lock frame: the box moves / turns / resizes in the cab
}

void SetMonitorEvery(int blits) {
    Guard gd;                                           // phase 51b: main + render thread
    g.monitorEvery = blits < 2 ? 2 : blits;
}

void OnBlit(ID3D11DeviceContext* ctx, int eye, DrawIdRecord& rec, uint64_t serial, uint64_t frame, bool frameStart, const XrIn& xr) {
    Guard gd;                                           // phase 51b: main + render thread
    if (!g.active || !ctx || eye < 0 || eye > 1) return;
    ++g.blits;
    EyeCur& cur = g.eye[eye];
    cur.serial = serial;
    cur.frame = frame;
    cur.srv.Reset();
    cur.entry = -1;
    cur.latchAge = 0;
    PollSnaps(ctx);
    // the open snapshot: only the frame's second eye pass (serial + 1) joins it; anything else closes it (the opener's eye only)
    if (g.open >= 0) {
        Snap& sn = g.snap[g.open];
        if (sn.eye[eye].done || serial != sn.serial0 + 1) CloseOpen();   // (both eyes of ONE frame, else the opener's eye only)
    }
    if (!rec.Usable()) {
        cur.why = kNoRecord;
        g.steady = 0;
        LogWindow();
        return;
    }
    // phase 47b: the lock gate -- cab draws in a flushed record + usable poses, kLockSteady blits in a row
    if (xr.ok && AnyCab(rec)) ++g.steady; else g.steady = 0;
    if (!g.lk.have) {
        Cand c[kCand];
        const int n = Gather(rec, c, kCand);
        cur.why = n ? kLocking : kNoCab;
        if (n && g.open < 0 && frameStart && g.blits >= g.nextLock && g.steady >= (uint64_t)kLockSteady) {
            bool pending = false;
            for (const Snap& s : g.snap) if (s.state == 2 && s.kind == 1 && s.gen == g.lk.gen) pending = true;
            if (!pending) OpenSnap(ctx, 1, xr.local, frame, serial);
        }
        if (n && g.open >= 0 && g.snap[g.open].kind == 1 && !g.snap[g.open].eye[eye].done) {
            Snap& sn = g.snap[g.open];
            SnapEye& se = sn.eye[eye];
            se.n = n;
            memcpy(se.c, c, sizeof(Cand) * (size_t)n);
            se.xrOk = xr.ok; se.pose = xr.eye[eye]; se.fw = xr.fw[eye];
            if (eye == 0) sn.xrLocal = xr.local;
            CopyCands(ctx, rec, sn, eye);
            if (sn.eye[0].done && sn.eye[1].done) CloseOpen();
        }
        LogWindow();
        return;
    }
    int idx[kCand];
    Match(rec, idx);
    int use = -1;
    for (int e = 0; e < g.lk.n; ++e) if (idx[e] >= 0 && !g.lk.e[e].demoted) { use = e; break; }
    if (use < 0 && g.lk.n > 0) {                       // phase 47d: every carrier found moving -> a new lock at once (it skips them)
        bool all = true;
        for (int e = 0; e < g.lk.n; ++e) if (!g.lk.e[e].demoted) all = false;
        if (all) {
            ++g.relocks;
            if (g.linesRelock < kLogLines) {
                ++g.linesRelock;
                Log("fps box attached to vehicle: every carrier of lock #%llu was found moving -- locking on again now", (unsigned long long)g.locks);
            }
            cur.why = kLocking;
            g.prev = g.lk; g.havePrev = g.lk.have; g.prevLockNo = g.locks;   // phase 47e
            DropLock();
            LogWindow();
            return;
        }
    }
    if (use < 0) {
        const bool cab = AnyCab(rec);
        cur.why = cab ? kNotFound : kNoCab;
        if (cab && ++cur.noMatch > (uint32_t)kRelockBlits) {
            ++g.relocks;
            if (g.linesRelock < kLogLines) {
                ++g.linesRelock;
                Log("fps box attached to vehicle: eye %d has cab draws but none of the lock's %d part(s) for %d frames -- a new cab "
                    "(truck change / LOD): locking on again (new lock #%llu after a cab change)", eye, g.lk.n, kRelockBlits,
                    (unsigned long long)g.relocks);
            }
            g.prev = g.lk; g.havePrev = g.lk.have; g.prevLockNo = g.locks;   // phase 47e
            DropLock();
        }
        LogWindow();
        return;
    }
    cur.noMatch = 0;
    const int i = idx[use];
    cur.entry = use;
    cur.off = rec.MirrorOff(i) + (UINT)(g.lk.e[use].row - 4) * 16u;
    cur.srv = rec.MirrorSrv();
    cur.why = kOk;
    if (cur.lastEntry >= 0 && cur.lastEntry != use) {
        ++g.reacq; ++g.wReacq;
        if (g.linesReacq < kLogLines) {
            ++g.linesReacq;
            Log("fps box attached to vehicle: eye %d re-acquired -- cab part %d (IndexCount %u) %s, now part %d (IndexCount %u) carries "
                "the box (re-acquire #%llu)", eye, cur.lastEntry, g.lk.e[cur.lastEntry].ic,
                g.lk.e[cur.lastEntry].demoted ? "moves against the cab" : "is not in this picture", use, g.lk.e[use].ic,
                (unsigned long long)g.reacq);
        }
    }
    cur.lastEntry = use;
    // the monitor snapshot (lagged: consensus + the play-space statistic)
    if (g.open < 0 && frameStart && g.blits - g.lastMonitor >= (uint64_t)g.monitorEvery) {
        if (OpenSnap(ctx, 2, xr.local, frame, serial) >= 0) g.lastMonitor = g.blits;
    }
    if (g.open >= 0 && g.snap[g.open].kind == 2 && !g.snap[g.open].eye[eye].done) {
        Snap& sn = g.snap[g.open];
        SnapEye& se = sn.eye[eye];
        se.n = 0;
        for (int e = 0; e < g.lk.n && se.n < kCand; ++e) {
            if (idx[e] < 0) continue;
            Cand& c = se.c[se.n++];
            c.i = idx[e]; c.full = rec.FullKey(idx[e]); c.loose = rec.LooseKey(idx[e]); c.ic = rec.IndexCount(idx[e]);
            c.off = rec.MirrorOff(idx[e]); c.rows = rec.MirrorRows(idx[e]); c.ent = e;
        }
        const int xe = g.lk.swapped ? 1 - eye : eye;  // (the lock found the OpenXR views in the other order)
        se.xrOk = xr.ok; se.pose = xr.eye[xe]; se.fw = xr.fw[xe];
        CopyCands(ctx, rec, sn, eye);
        if (sn.eye[0].done && sn.eye[1].done) CloseOpen();
    }
    LogWindow();
}

bool Get(int eye, uint64_t passSeq, Place* out, int* why) {
    Guard gd;                                           // phase 51b: main + render thread
    int w = kOk;
    if (!g.active) w = kInactive;
    else if (eye < 0 || eye > 1 || g.eye[eye].frame == ~0ull || g.eye[eye].latchAge > 1) w = kNoBlit;
    else if (passSeq > g.eye[eye].serial + 2) w = kStale;           // a newer pass than this frame's two may have reset the slot
    else w = g.eye[eye].why;
    if (w == kOk && (!g.eye[eye].srv || g.eye[eye].entry < 0 || g.eye[eye].entry >= g.lk.n)) w = kNotFound;
    if (w == kOk && !g.latched) w = kSettling;                         // phase 47b: the frame's verdict decides, not the eye
    if (why) *why = w;
    if (w != kOk || !out) return false;
    const EyeCur& c = g.eye[eye];
    out->srv = c.srv.Get();
    out->off = c.off;
    out->entry = c.entry;
    for (int k = 0; k < 4; ++k) for (int i = 0; i < 4; ++i) out->corner[k][i] = (float)g.lk.e[c.entry].a[k][i];
    return true;
}

// phase 47b: the frame's verdict at Present -> the hysteresis. A frame is attachable when both eyes had their blit with the attached
// placement available; the reason of the first eye that had not is kept for the log.
void Latch() {
    Guard gd;                                           // phase 51b: main + render thread
    if (!g.active) { g.latched = false; g.onRun = g.offRun = 0; g.hist = 0; return; }
    int why = kOk;
    for (int e = 0; e < 2 && why == kOk; ++e) {
        const EyeCur& c = g.eye[e];
        if (c.latchAge > 1) why = kNoBlit;
        else if (c.why != kOk || !c.srv || c.entry < 0) why = c.why != kOk ? c.why : kNotFound;
    }
    for (EyeCur& c : g.eye) if (c.latchAge < 99) ++c.latchAge;
    if (why == kOk) { ++g.onRun; g.offRun = 0; } else { ++g.offRun; g.onRun = 0; }
    g.hist = ((g.hist << 1) | (why == kOk ? 0u : 1u)) & 0xFFu;
    int misses = 0;
    for (uint32_t h = g.hist; h; h &= h - 1) ++misses;
    const bool was = g.latched;
    // on: kOnFrames attachable frames in a row; off: kOffFrames of the last 8 frames not attachable (in a row or not: a loading
    // screen whose every 3rd frame looks attachable must not flap -- ATS 14:32 test)
    if (!g.latched && g.onRun >= kOnFrames) g.latched = true;
    else if (g.latched && misses >= kOffFrames) g.latched = false;
    if (g.latched) ++g.wFrOn; else { ++g.wFrOff; if (why > 0 && why < kWhyCount) ++g.wWhy[why]; }
    if (was != g.latched) {
        (g.latched ? g.latchOn : g.latchOff)++;
        if (g.linesLatch < kLogLines) {
            ++g.linesLatch;
            if (g.latched)
                Log("fps box attached to vehicle: ATTACHED to the cab (%d frames in a row with both eyes attachable; switch #%llu)",
                    kOnFrames, (unsigned long long)(g.latchOn + g.latchOff));
            else
                Log("fps box attached to vehicle: drawn FIXED IN THE PLAY SPACE (as lock 1: the quad layer / drawn in) -- %d of the last 8 frames without "
                    "the attach, last reason: %s (switch #%llu)", kOffFrames, kWhyNames[why], (unsigned long long)(g.latchOn + g.latchOff));
        }
    }
}

bool Attached() {
    Guard gd;                                           // phase 51b: main + render thread
    return g.active && g.latched;
}

uint64_t LockCount() {
    Guard gd;                                           // phase 51b: main + render thread
    return g.locks;
}

// ---- phase 51b ----------------------------------------------------------------------------------------------------------------
LockInfo GetLockInfo() {
    Guard gd;                                           // phase 51b: main + render thread
    LockInfo i;
    if (!g.lk.have) return i;
    i.placed = g.lk.placed; i.samples = g.lk.samples; i.samplesUsed = g.lk.samplesUsed; i.spreadMm = g.lk.spreadMm;
    i.cabIc = g.lk.hasCab ? g.lk.cabUsedIc : 0u; i.cabMissing = g.lk.cabMissing; i.missingIc = g.lk.missingIc;
    i.cabFound = g.lk.cabFound; i.cabAgree = g.lk.cabAgree; i.cabAmbiguous = g.lk.cabAmbiguous; i.remeasured = g.lk.remeasured;
    i.level = g.lk.level; i.leveledOld = g.lk.leveledOld; i.turnDeg = g.lk.turnDeg;   // phase 51d
    i.tiltSide = g.lk.tiltSide; i.tiltFore = g.lk.tiltFore; i.tiltYaw = g.lk.tiltYaw;
    i.cabFrameDraws = g.lk.haveCabS ? g.lk.cabFrameDraws : 0; i.cabFrameIc = g.lk.cabFrameIc; i.cabFrameSum = g.lk.cabFrameSum;
    i.cabFrameRejected = g.lk.cabFrameRejected;
    return i;
}
bool CurrentCabPose(FpsProfile::CabPose* out) {
    Guard gd;                                           // phase 51b: main + render thread
    if (!g.lk.have || g.lk.n < 1 || !out) return false;
    if (g.lk.hasCab) { *out = g.lk.cab; return true; }
    // phase 51c: the box in up to kCabParts carriers (the reference first, then the biggest; not demoted / remembered moving):
    // part j's map = T_j A (T_j: the reference's model space -> part j's)
    *out = FpsProfile::CabPose();
    int np = 0;
    for (int e = 0; e < g.lk.n && np < FpsProfile::kCabParts; ++e) {
        const Entry& r = g.lk.e[e];
        if (r.demoted || IsMovingGeom(r.ic, r.stride, r.fmt)) continue;
        const M4 Aj = Mul(r.T, g.lk.A);
        if (!Finite(Aj)) continue;
        if (np == 0) CabPoseSet(Aj, g.lk.lf, r.ic, r.stride, r.fmt, out);
        else {
            FpsProfile::CabPart pt;
            pt.ic = r.ic; pt.stride = r.stride; pt.fmt = r.fmt;
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) pt.A[i][j] = Aj.m[i][j];
            out->x[out->nx++] = pt;
        }
        ++np;
    }
    out->level = g.lk.level;                            // phase 51d: square to the cab ("cabsq")
    return np > 0 && FpsProfile::CabPoseOk(*out);
}
bool PlaceFromCab(const FpsProfile::CabPose& c) {
    Guard gd;                                           // phase 51b: main + render thread
    if (!g.lk.have || !ApplyCab(g.lk, c, g.set, true)) return false;
    BuildCorners();
    return true;
}
void Remeasure() {
    Guard gd;                                           // phase 51c: main thread (the menu)
    const bool had = g.lk.have;
    DropLock();
    g.havePrev = false;
    g.prev = LockSt();
    g.remeasure = true;
    Log("fps box attached to vehicle: RE-MEASURE the cab spot -- %s; the next lock-on takes a fresh averaged play -> cab map (the stored "
        "cab pose is not used, the settings are kept)", had ? "lock dropped" : "no lock held");
}
uint64_t CabMovedSerial(FpsProfile::CabPart* part) {
    Guard gd;
    if (part) *part = g.cabMoved;
    return g.cabMovedSerial;
}
int CabMovedSince(uint64_t* serial, FpsProfile::CabPart* out, int cap) {
    Guard gd;                                           // phase 51d
    uint64_t s = *serial;
    if (s > g.cabMovedSerial) s = g.cabMovedSerial;
    if (g.cabMovedSerial - s > (uint64_t)St::kMovedQ) s = g.cabMovedSerial - (uint64_t)St::kMovedQ;
    int n = 0;
    while (s < g.cabMovedSerial && n < cap) out[n++] = g.movedQ[s++ % St::kMovedQ];
    *serial = s;
    return n;
}
void SetCabPoseProvider(CabPoseFn fn) {
    Guard gd;                                           // phase 51b: main + render thread
    g.provider = fn;
}
void SetLockSamples(int n) {
    Guard gd;                                           // phase 51b: main + render thread
    g.lockSamples = n < 1 ? 1 : (n > kMaxLockSamples ? kMaxLockSamples : n);
}
bool EntryCorners(uint32_t ic, double out[4][4]) {
    Guard gd;                                           // phase 51b: main + render thread
    for (int e = 0; e < g.lk.n; ++e)
        if (g.lk.have && g.lk.e[e].ic == ic) { memcpy(out, g.lk.e[e].a, sizeof(g.lk.e[e].a)); return true; }
    return false;
}
uint64_t VehicleKey() {
    Guard gd;                                           // phase 51b: main + render thread
    return g.lk.have ? g.lk.vehicleKey : 0;
}
bool LockSameCab() {
    Guard gd;                                           // phase 51b: main + render thread
    return g.lk.have && g.lk.sameCab;
}

void NoteDraw(bool attached, int why) {
    Guard gd;                                           // phase 51b: main + render thread
    (void)why;
    if (attached) { ++g.attached; ++g.wAttached; }
    else { ++g.held; ++g.wHeld; }                                     // attached frame, this eye could not: no box (never the other kind)
}
void Shutdown() {
    Guard gd;                                           // phase 51b: main + render thread
    g_nMoving = 0;                                      // phase 47d: the keys hold VS pointers of the old device
    for (Snap& s : g.snap) { s.buf.Reset(); s.state = 0; }
    for (EyeCur& c : g.eye) { c.srv.Reset(); c.frame = ~0ull; c.why = kNoBlit; }
    DropLock();
    g.havePrev = false;                                 // phase 47e
    g.prev = LockSt();
}

Stats GetStats() {
    Guard gd;                                           // phase 51b: main + render thread
    Stats s{};
    s.state = !g.active ? 0 : (g.lk.have ? 2 : 1);
    s.entries = g.lk.n;
    for (int e = 0; e < g.lk.n; ++e) if (g.lk.e[e].demoted) ++s.demoted;
    s.reacquires = g.reacq; s.relocks = g.relocks; s.locks = g.locks; s.attached = g.attached; s.fallback = g.fallback;
    s.refEntry[0] = g.eye[0].lastEntry; s.refEntry[1] = g.eye[1].lastEntry;
    s.lockFrame = g.lk.frame;
    s.refIc = g.lk.have && g.lk.n ? g.lk.e[0].ic : 0u; s.remembered = g_nMoving; s.monitorUs = g.monitorUs;
    s.devMaxM = g.maxDev; s.devN = g.nDev;
    s.lockEyes = g.lk.have ? g.lk.eyesUsed : 0; s.lockXr = g.lk.have && g.lk.xr;
    s.monitors = g.monitors; s.latchOn = g.latchOn; s.latchOff = g.latchOff; s.held = g.held; s.lockRefusals = g.lockRefusals;
    s.latched = g.latched;
    s.vehicleKey = VehicleKey(); s.sameCab = LockSameCab();   // phase 51
    s.placed = g.lk.have ? g.lk.placed : kPlacedNone;         // phase 51b
    return s;
}

}  // namespace FpsCab

#endif  // WITH_DLAA
