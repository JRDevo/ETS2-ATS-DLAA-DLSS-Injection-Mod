// GpuPerf -- v0.10.0 phase 8 per-sub-pass GPU timers (see gpu_perf.h).
#ifdef WITH_DLAA
#include "gpu_perf.h"
#include "log.h"

#include <cstring>
#include <cstdio>

namespace GpuPerf {
namespace {

struct SecInfo { const char* name; bool inTotal; bool sampled; };
const SecInfo kInfo[kCount] = {
    { "replay", true, false },        { "fwd-replay", true, false },   { "snapshot", true, false },
    { "cand-copy*", true, true },     { "fwd-depth", true, false },    { "fwd-init", true, false },   // phase 10: exact
    { "replay-inst*", false, true },  { "replay-rest*", false, true },
    { "copy-in", true, false },       { "convert", true, false },      { "medoid", true, false },
    { "gather", true, false },        { "match", true, false },        { "pair", true, false },
    { "vote", true, false },          { "pick", true, false },         { "resolve", true, false },
    { "inherit", true, false },       { "p-count", true, false },      { "p-list", true, false },
    { "twin", true, false },          { "attach", true, false },
    { "p-vote1", true, false },       { "p-pick1", true, false },      { "p-vote2", true, false },
    { "p-pick2", true, false },       { "pick+res", true, false },     { "list+twin", true, false },
    { "pass-B", true, false },        { "mv-misc", true, false },
    { "NGX", false, false },          { "rcas", true, false },         { "copy-out", true, false },
    { "dbg-view", true, false },      { "mirrors", true, false },      { "mirror-NGX", false, false },
};
static_assert(sizeof(kInfo) / sizeof(kInfo[0]) == kCount, "section table");

constexpr int kSets = 8;               // frames in flight
constexpr int kTs = 1024;              // timestamps per frame (queries created on first use)
constexpr int kEnt = 768;              // sections per frame (a sampled pass brackets every forward re-draw / candidate copy)
constexpr int kTags = 8;               // distinct passes per frame (accounting)

struct Ent { int16_t sec; int16_t t0, t1; uint64_t tag; };
struct Set {
    ID3D11Query* dis = nullptr;
    ID3D11Query* ts[kTs] = {};
    Ent      ent[kEnt];
    int      nTs = 0, nEnt = 0;
    bool     open = false, inFlight = false;
    uint64_t frame = 0;
};

bool     g_on = false;
bool     g_broken = false;
Set      g_set[kSets];
int      g_cur = -1;                   // open set (-1 = none)
int      g_next = 0;                   // next set to open
uint64_t g_frame = 0;
uint64_t g_tag = 0;
bool     g_mirror = false;
uint32_t g_gen = 1;                    // token generation (low bits of the set index)
uint32_t g_restarts = 0;               // Restart() calls (Restarts())

// tag -> eye (ring)
struct TagEye { uint64_t tag; int eyeP1; };   // eyeP1 = eye + 1 (0 = empty)
constexpr int kMap = 64;
TagEye   g_map[kMap];
int      g_mapHead = 0;

// window
struct Win {
    uint64_t passes[2] = {};
    uint64_t lastTag[2] = {};
    double   sum[2][kCount] = {}, mx[2][kCount] = {};
    uint64_t n[2][kCount] = {};
    double   totMax[2] = {};
    uint64_t unattributed = 0, dropped = 0;
} g_w;

int EyeOf(uint64_t tag) {
    for (int i = 0; i < kMap; ++i) if (g_map[i].eyeP1 > 0 && g_map[i].tag == tag) return g_map[i].eyeP1 - 1;
    return -1;
}

void ReleaseSet(Set& s) {
    if (s.dis) s.dis->Release();
    for (ID3D11Query*& q : s.ts) if (q) { q->Release(); q = nullptr; }
    s = Set();
}

ID3D11Query* Ts(ID3D11DeviceContext* ctx, Set& s, int i) {
    if (!s.ts[i]) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_TIMESTAMP;
        const bool ok = dev && SUCCEEDED(dev->CreateQuery(&qd, &s.ts[i]));
        if (dev) dev->Release();
        if (!ok) { s.ts[i] = nullptr; return nullptr; }
    }
    return s.ts[i];
}

void CloseCur(ID3D11DeviceContext* ctx) {
    if (g_cur < 0) return;
    Set& s = g_set[g_cur];
    // sections still open (never ended) are dropped
    for (int e = 0; e < s.nEnt; ++e) if (s.ent[e].t1 < 0) { s.ent[e].sec = -1; ++g_w.dropped; }
    ctx->End(s.dis);
    s.open = false;
    s.inFlight = true;
    g_cur = -1;
}

bool OpenCur(ID3D11DeviceContext* ctx) {
    Set& s = g_set[g_next];
    if (s.inFlight || s.open) return false;          // every set still in flight: this frame is not timed
    if (!s.dis) {
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        D3D11_QUERY_DESC qd{};
        qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
        const bool ok = dev && SUCCEEDED(dev->CreateQuery(&qd, &s.dis));
        if (dev) dev->Release();
        if (!ok) { s.dis = nullptr; g_broken = true; Log("GPU perf: query creation failed -- perf timers off"); return false; }
    }
    ctx->Begin(s.dis);
    s.open = true;
    s.nTs = 0; s.nEnt = 0;
    s.frame = g_frame;
    g_cur = g_next;
    g_next = (g_next + 1) % kSets;
    if (++g_gen == 0) g_gen = 1;
    return true;
}

// the open set of THIS frame (rolls over at a new frame); nullptr = not timed
Set* Cur(ID3D11DeviceContext* ctx) {
    if (!g_on || g_broken || !ctx) return nullptr;
    if (g_cur >= 0 && g_set[g_cur].frame != g_frame) CloseCur(ctx);
    if (g_cur < 0) {
        Poll(ctx);
        if (!OpenCur(ctx)) { ++g_w.dropped; return nullptr; }
    }
    return &g_set[g_cur];
}

int MapSec(int sec) {
    if (!g_mirror) return sec;
    return sec == kNgx ? kMirNgx : kMirror;
}

// token = generation (10 bits) | set (4 bits) | section (16 bits); always >= 0
inline int Tok(int ent) { return (int)(((g_gen & 0x3FFu) << 20) | ((uint32_t)g_cur << 16) | (uint32_t)ent); }
inline bool TokOk(int tok, int* ent) {
    if (tok < 0 || g_cur < 0) return false;
    if ((((uint32_t)tok >> 20) & 0x3FFu) != (g_gen & 0x3FFu) || (((uint32_t)tok >> 16) & 0xFu) != (uint32_t)g_cur) return false;
    *ent = tok & 0xFFFF;
    return *ent < g_set[g_cur].nEnt;
}

int Stamp(ID3D11DeviceContext* ctx, Set& s) {
    if (s.nTs >= kTs) return -1;
    ID3D11Query* q = Ts(ctx, s, s.nTs);
    if (!q) return -1;
    ctx->End(q);
    return s.nTs++;
}

}  // namespace

void SetOn(bool on) { g_on = on; }
bool On() { return g_on && !g_broken; }
const char* Name(int sec) { return sec >= 0 && sec < kCount ? kInfo[sec].name : "?"; }
bool InTotal(int sec) { return sec >= 0 && sec < kCount && kInfo[sec].inTotal; }
bool IsSampled(int sec) { return sec >= 0 && sec < kCount && kInfo[sec].sampled; }
void SetFrame(uint64_t frame) { g_frame = frame; }
void SetPass(uint64_t tag) { g_tag = tag; }
uint64_t Pass() { return g_tag; }
void SetMirror(bool on) { g_mirror = on; }
bool Mirror() { return g_mirror; }
bool Sampled(uint64_t tag) { return g_on && (tag % kSampleEvery) == 0; }

int Begin(ID3D11DeviceContext* ctx, int sec) {
    Set* s = Cur(ctx);
    if (!s) return -1;
    if (s->nEnt >= kEnt) { ++g_w.dropped; return -1; }
    const int t = Stamp(ctx, *s);
    if (t < 0) { ++g_w.dropped; return -1; }
    Ent& e = s->ent[s->nEnt];
    e.sec = (int16_t)MapSec(sec); e.t0 = (int16_t)t; e.t1 = -1; e.tag = g_tag;
    return Tok(s->nEnt++);
}

int Next(ID3D11DeviceContext* ctx, int tok, int sec) {
    int ei = -1;
    if (!TokOk(tok, &ei)) return Begin(ctx, sec);
    Set& s = g_set[g_cur];
    const int t = Stamp(ctx, s);
    if (t < 0) { s.ent[ei].sec = -1; ++g_w.dropped; return -1; }
    s.ent[ei].t1 = (int16_t)t;
    if (s.nEnt >= kEnt) { ++g_w.dropped; return -1; }
    Ent& e = s.ent[s.nEnt];
    e.sec = (int16_t)MapSec(sec); e.t0 = (int16_t)t; e.t1 = -1; e.tag = g_tag;
    return Tok(s.nEnt++);
}

void End(ID3D11DeviceContext* ctx, int tok) {
    int ei = -1;
    if (!TokOk(tok, &ei)) return;
    Set& s = g_set[g_cur];
    const int t = Stamp(ctx, s);
    if (t < 0) { s.ent[ei].sec = -1; ++g_w.dropped; return; }
    s.ent[ei].t1 = (int16_t)t;
}

void NoteEye(uint64_t tag, int eye) {
    if (!g_on) return;
    if (eye < 0) return;
    for (int i = 0; i < kMap; ++i) if (g_map[i].eyeP1 > 0 && g_map[i].tag == tag) { g_map[i].eyeP1 = eye + 1; return; }
    g_map[g_mapHead] = { tag, eye + 1 };
    g_mapHead = (g_mapHead + 1) % kMap;
}

void Poll(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    // oldest first: the set after the open one in ring order
    for (int k = 0; k < kSets; ++k) {
        Set& s = g_set[(g_next + k) % kSets];
        if (!s.inFlight) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
        if (ctx->GetData(s.dis, &dd, sizeof(dd), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;   // later sets land later
        UINT64 t[kTs];
        bool got = true;
        for (int i = 0; i < s.nTs && got; ++i)
            got = ctx->GetData(s.ts[i], &t[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (!got) return;
        s.inFlight = false;
        if (dd.Disjoint || !dd.Frequency) { g_w.dropped += (uint64_t)s.nEnt; continue; }
        const double toMs = 1000.0 / (double)dd.Frequency;
        // per (tag) accumulation inside this set
        uint64_t tags[kTags]; int nTags = 0; double secSum[kTags][kCount]; bool seen[kTags][kCount];
        memset(secSum, 0, sizeof(secSum)); memset(seen, 0, sizeof(seen));
        for (int e = 0; e < s.nEnt; ++e) {
            const Ent& en = s.ent[e];
            if (en.sec < 0 || en.t1 < 0) continue;
            int ti = -1;
            for (int q = 0; q < nTags; ++q) if (tags[q] == en.tag) { ti = q; break; }
            if (ti < 0) {
                if (nTags >= kTags) { ++g_w.dropped; continue; }
                ti = nTags; tags[nTags++] = en.tag;
            }
            const double ms = t[en.t1] >= t[en.t0] ? (double)(t[en.t1] - t[en.t0]) * toMs : 0.0;
            secSum[ti][en.sec] += ms;
            seen[ti][en.sec] = true;
        }
        for (int q = 0; q < nTags; ++q) {
            const int eye = EyeOf(tags[q]);
            if (eye < 0 || eye > 1) {
                for (int c = 0; c < kCount; ++c) if (seen[q][c]) ++g_w.unattributed;
                continue;
            }
            if (tags[q] > g_w.lastTag[eye] || g_w.passes[eye] == 0) { ++g_w.passes[eye]; g_w.lastTag[eye] = tags[q]; }
            double tot = 0.0;
            for (int c = 0; c < kCount; ++c) {
                if (!seen[q][c]) continue;
                g_w.sum[eye][c] += secSum[q][c];
                ++g_w.n[eye][c];
                if (secSum[q][c] > g_w.mx[eye][c]) g_w.mx[eye][c] = secSum[q][c];
                if (kInfo[c].inTotal) tot += secSum[q][c];
            }
            if (tot > g_w.totMax[eye]) g_w.totMax[eye] = tot;
        }
    }
}

bool Stats(int eye, EyeStats* out) {
    if (eye < 0 || eye > 1 || !out) return false;
    *out = EyeStats();
    const uint64_t np = g_w.passes[eye];
    out->passes = np;
    if (!np) return false;
    for (int c = 0; c < kCount; ++c) {
        out->n[c] = g_w.n[eye][c];
        out->mx[c] = g_w.mx[eye][c];
        // sampled sections: per sampled pass; the others: per pass of the eye (a section that did not run counts as 0)
        const uint64_t div = kInfo[c].sampled ? g_w.n[eye][c] : np;
        out->avg[c] = div ? g_w.sum[eye][c] / (double)div : 0.0;
        if (kInfo[c].inTotal) out->total += out->avg[c];
    }
    out->totalMax = g_w.totMax[eye];
    return true;
}

uint64_t Unattributed() { return g_w.unattributed; }
uint64_t Dropped() { return g_w.dropped; }
void Restart() { g_w = Win(); ++g_restarts; }
uint32_t Restarts() { return g_restarts; }

void LogWindow(uint64_t blit, double sceneMs, uint32_t sceneN) {
    if (!g_on) return;
    for (int eye = 0; eye < 2; ++eye) {
        EyeStats st;
        if (!Stats(eye, &st)) continue;
        char buf[3072];
        int o = snprintf(buf, sizeof(buf), "perf eye %d @blit %llu: mod GPU %.3f avg / %.3f max ms per pass (%llu passes; "
                         "everything except NGX; NGX %.3f avg, mirror NGX %.3f avg) | GPU spans scene ", eye,
                         (unsigned long long)blit, st.total, st.totalMax, (unsigned long long)st.passes, st.avg[kNgx],
                         st.avg[kMirNgx]);
        if (sceneMs >= 0.0) o += snprintf(buf + o, sizeof(buf) - (size_t)o, "%.2f ms/frame (n=%u)", sceneMs, sceneN);
        else o += snprintf(buf + o, sizeof(buf) - (size_t)o, "n/a");
        o += snprintf(buf + o, sizeof(buf) - (size_t)o, " | ms avg/max:");
        for (int c = 0; c < kCount && o > 0 && o < (int)sizeof(buf) - 64; ++c) {
            if (c == kNgx || c == kMirNgx || !st.n[c]) continue;
            if (kInfo[c].sampled)
                o += snprintf(buf + o, sizeof(buf) - (size_t)o, " %s %.3f/%.3f (%llu sampled)", kInfo[c].name, st.avg[c], st.mx[c],
                              (unsigned long long)st.n[c]);
            else
                o += snprintf(buf + o, sizeof(buf) - (size_t)o, " %s %.3f/%.3f", kInfo[c].name, st.avg[c], st.mx[c]);
        }
        Log("%s", buf);
    }
    if (g_w.unattributed || g_w.dropped)
        Log("perf @blit %llu: %llu sections of passes that never reached a blit (not in the eye lines), %llu sections not timed "
            "(a frame's query set full / all sets in flight / disjoint)", (unsigned long long)blit,
            (unsigned long long)g_w.unattributed, (unsigned long long)g_w.dropped);
    Restart();
}

void Shutdown() {
    for (Set& s : g_set) ReleaseSet(s);
    g_cur = -1; g_next = 0; g_broken = false;
    for (TagEye& m : g_map) m = { 0, 0 };
    Restart();
}

}  // namespace GpuPerf

#endif // WITH_DLAA
