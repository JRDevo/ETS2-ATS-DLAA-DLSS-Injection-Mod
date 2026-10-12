// FpsCab runtime -- phase 47: the VR fps box attached to the vehicle (fps_vr_lock = 2). See fps_cab.h for the design and the
// maths. Render thread only (the game's immediate context): OnBlit at each VR eye blit, Get at each drawn-in box draw.
#ifdef WITH_DLAA

#include "fps_cab.h"
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
    int linesLock = 0, linesWhy = 0, linesReacq = 0, linesDemote = 0, linesRelock = 0;
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
};
St g;

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

// The lock's entries present in `rec`: idx[e] = the record index (-1 = absent / ambiguous) -- by full key, else a unique loose
// key (the game's buffer pool rotated the pointers).
void Match(DrawIdRecord& rec, int idx[kCand]) {
    int fullN[kCand] = {}, looseN[kCand] = {}, fullI[kCand], looseI[kCand];
    const int cnt = rec.Count();
    for (int i = 0; i < cnt; ++i) {
        const uint8_t f = rec.Flags(i);
        if (!(f & DrawIdRecord::kFCabin) || (f & (DrawIdRecord::kFInstanced | DrawIdRecord::kFNoMvp | DrawIdRecord::kFFwd))) continue;
        const uint64_t fk = rec.FullKey(i), lk = rec.LooseKey(i);
        for (int e = 0; e < g.lk.n; ++e) {
            if (g.lk.e[e].full == fk) { if (!fullN[e]++) fullI[e] = i; }
            else if (g.lk.e[e].loose == lk) { if (!looseN[e]++) looseI[e] = i; }
        }
    }
    for (int e = 0; e < kCand; ++e) {
        idx[e] = -1;
        if (e >= g.lk.n) continue;
        int i = fullN[e] == 1 ? fullI[e] : (fullN[e] == 0 && looseN[e] == 1 ? looseI[e] : -1);
        if (i >= 0 && (rec.MirrorOff(i) == 0xFFFFFFFFu || rec.MirrorRows(i) < (UINT)g.lk.e[e].row + 4u)) i = -1;
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
    memcpy(L.centrePlay, cc, sizeof(cc));
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
struct Moving { uint64_t full, loose; uint32_t ic; };
Moving g_moving[kMoveMax];
int g_nMoving = 0, g_linesMoving = 0;
bool IsMoving(uint64_t full, uint64_t loose) {
    for (int i = 0; i < g_nMoving; ++i) if (g_moving[i].full == full || g_moving[i].loose == loose) return true;
    return false;
}
void Remember(const Entry& en) {
    if (IsMoving(en.full, en.loose)) return;
    if (g_nMoving == kMoveMax) { memmove(g_moving, g_moving + 1, sizeof(Moving) * (kMoveMax - 1)); --g_nMoving; }
    g_moving[g_nMoving++] = Moving{ en.full, en.loose, en.ic };
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
    int nk = 0;
    V v[2 * kCand][2];
    for (int e = 0; e < 2; ++e)
        for (int j = 0; j < sn.eye[e].n; ++j) {
            const Cand& c = sn.eye[e].c[j];
            int k = 0;
            while (k < nk && keys[k] != c.full) ++k;
            if (k == nk) { keys[nk] = c.full; ics[nk] = c.ic; looses[nk] = c.loose; ++nk; }
            const float* rows4;
            const int nr = RowsOf(data, e, j, c, &rows4);
            V& x = v[k][e];
            x.why = CandVerdict(rows4, nr, &x.M, &x.row, &x.ef);
            if (!x.why && IsMoving(c.full, c.loose)) x.why = "a remembered MOVING part (phase 47d)";
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
            if (IsMoving(keys[k], looses[k])) { ++skip; if (ics[k] > skipIc) skipIc = ics[k]; }
        if (skip && g_linesMoving < kLogLines) {
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
    if (lg4 && g.linesLock < kLogLines) {
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
    if (g.linesLock < kLogLines) {
        ++g.linesLock;
        Log("fps box attached to vehicle: LOCKED ON (lock #%llu) -- reference cab draw IndexCount %u (MVP in cb0 rows %d..%d), seen in %s; "
            "%d cab part(s) of its rigid group (IndexCount sum %llu, the largest of %d transform group(s); %d part(s) with their own "
            "transform -- wheel / doors / needles -- not used) can carry the box; play -> cab map from %s%s (game IPD / headset IPD %.3f, %.1f model units per m); "
            "anchor %s, box centre (%+.3f %+.3f %+.3f) m in the play space, %.3f m wide%s",
            (unsigned long long)g.locks, L.e[0].ic, L.e[0].row, L.e[0].row + 3, both ? "both eyes" : (ea == 0 ? "eye 0 only" : "eye 1 only"),
            L.n, (unsigned long long)gsum[bestG], ng, others, L.xr ? (n == 2 ? "both eyes' OpenXR poses" : "one eye's OpenXR pose") : "NO OpenXR pose (the box placed from the eye's own "
            "view at the lock)", swapped ? ", the eye order swapped (OpenXR view vs eye picture)" : "", ipdRatio, L.upm,
            !L.xr ? "straight ahead of the eye" : (L.local ? "around the seat (LOCAL space origin)" : "around the eyes' midpoint at the lock"),
            cc[0], cc[1], cc[2], wM, groupOnly ? "" : " -- NO shared transform among the parts (each its own group): all usable "
            "parts carry the box, the monitor (every ~0.08 s) drops a moving one");
    }
    if (lg4 && g.linesLock < kLogLines) {
        ++g.linesLock;
        LogDbg("fps box attached to vehicle: part-to-part transforms of eye %d vs the other eye differ by %.2f mm at most (1 m from "
               "the reference's origin; 0 = a rigid pair seen identically by both eyes)", ea, maxEyeDiffMm);
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

bool Active() { return g.active; }

void Reset(const char* why) {
    const bool had = g.lk.have;
    DropLock();
    if (had && g.linesRelock < kLogLines) {
        ++g.linesRelock;
        Log("fps box attached to vehicle: lock dropped (%s) -- locking on again", why ? why : "reset");
    }
}

void SetActive(bool on, uint64_t frame) {
    if (on == g.active) return;
    g.active = on;
    DropLock();
    for (EyeCur& c : g.eye) { c.srv.Reset(); c.frame = ~0ull; c.why = kNoBlit; c.noCabLogged = false; c.latchAge = 99; }
    g.lastWhy = -1;
    g.latched = false; g.onRun = g.offRun = 0; g.hist = 0; g.steady = 0;   // phase 47b
    ResetWindow();
    Log("fps box attached to vehicle: %s (Present #%llu)", on ? "ON -- locking on to the cab at the next world frame" : "off",
        (unsigned long long)frame);
}

void SetSettings(const Settings& s) {
    if (s == g.set) return;
    g.set = s;
    if (g.lk.have) BuildCorners();                      // the same lock frame: the box moves / turns / resizes in the cab
}

void SetMonitorEvery(int blits) { g.monitorEvery = blits < 2 ? 2 : blits; }

void OnBlit(ID3D11DeviceContext* ctx, int eye, DrawIdRecord& rec, uint64_t serial, uint64_t frame, bool frameStart, const XrIn& xr) {
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

bool Attached() { return g.active && g.latched; }

void NoteDraw(bool attached, int why) {
    (void)why;
    if (attached) { ++g.attached; ++g.wAttached; }
    else { ++g.held; ++g.wHeld; }                                     // attached frame, this eye could not: no box (never the other kind)
}
void Shutdown() {
    g_nMoving = 0;                                      // phase 47d: the keys hold VS pointers of the old device
    for (Snap& s : g.snap) { s.buf.Reset(); s.state = 0; }
    for (EyeCur& c : g.eye) { c.srv.Reset(); c.frame = ~0ull; c.why = kNoBlit; }
    DropLock();
}

Stats GetStats() {
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
    return s;
}

}  // namespace FpsCab

#endif  // WITH_DLAA
