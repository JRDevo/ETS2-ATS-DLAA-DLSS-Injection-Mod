// GpuPass -- phase 36: GPU time per PASS TYPE of the game's frame (measurement only, no behaviour change).
//
// Why: VRS (phase 34) covers the world G-buffer bind and the main forward bind only and saved ~0.5 ms GPU/frame. To decide
// whether extending it to the deferred LIGHTING pass pays, we need to know what every other pass of an eye costs.
//
// How: the game's OMSetRenderTargets binds cut the frame into intervals. Each hook of the game's render thread stamps TWO
// GPU timestamps: one at the top (before any mod work of that hook) and one at the end (after the mod's bind-time work: the
// draw-id replay of a G-buffer leave, VRS, ...). The interval [end stamp of bind k, top stamp of bind k+1] is the game's work
// under bind k and is charged to the CLASS of bind k; [top, end] of one hook is the mod's bind-time work ("bindwork").
// Classes (Classify): shadow (depth-only bind on a depth that is not the scene depth / scene size), gbuffer (4 RTV on the
// scene depth), lighting (a bind of the scene's size between the G-buffer and the forward bind, e.g. the 2-RTV light
// accumulation), forward (1 RTV RGBA16F scene size on the scene depth), post (a bind not larger than the scene after the
// forward bind: tonemap / bloom / ...), mirror (any bind while a mirror view is open), other (depth-only on the scene depth,
// binds before the G-buffer, larger / UI targets). The mod's blit-time work (DLAA, NGX, RCAS) runs inside the draws of
// post / other: the "mod" figure of the log line is GpuPerf's own per-section total and OVERLAPS the classes.
//
// Timestamps only (one TIMESTAMP_DISJOINT per frame set, 8 sets in flight, read back with DONOTFLUSH several frames later):
// no CPU stall. A set is one Present frame (GpuPerf::SetFrame's counter). The interval after the LAST bind of a frame is
// dropped (it ends at the next frame's first bind and would include idle / Present time). Like the "GPU spans" figure,
// an interval also contains GPU IDLE time when the GPU waits for the CPU's submission.
//
// Gate: GpuPerf::On() (dlaa.ini debug = 1 / perf_timers); off = every entry point is one bool test. Render thread only.
// Compiled only when WITH_DLAA=1.
#pragma once
#ifdef WITH_DLAA

#include <d3d11.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "gpu_perf.h"
#include "log.h"

namespace GpuPass {

enum Cls : int { kShadow = 0, kGbuffer, kLighting, kForward, kPost, kMirror, kOther, kBindWork, kNCls };
inline const char* ClsName(int c) {
    static const char* const n[kNCls] = { "shadow", "gbuffer", "lighting", "forward", "post", "mirror", "other", "bindwork" };
    return c >= 0 && c < kNCls ? n[c] : "?";
}
inline const char* FmtName(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_UNKNOWN: return "-";
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32F";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16F";
    case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16N";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2N";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8N";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8S";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8N";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8S";
    case DXGI_FORMAT_R16G16_FLOAT: return "R16G16F";
    case DXGI_FORMAT_R16G16_UNORM: return "R16G16N";
    case DXGI_FORMAT_R8G8_UNORM: return "R8G8N";
    case DXGI_FORMAT_R8_UNORM: return "R8N";
    case DXGI_FORMAT_R16_FLOAT: return "R16F";
    case DXGI_FORMAT_R16_UNORM: return "R16N";
    case DXGI_FORMAT_R32_FLOAT: return "R32F";
    case DXGI_FORMAT_R32_UINT: return "R32U";
    case DXGI_FORMAT_R16_UINT: return "R16U";
    case DXGI_FORMAT_D32_FLOAT: return "D32F";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32S8";
    case DXGI_FORMAT_D16_UNORM: return "D16";
    case DXGI_FORMAT_R32_TYPELESS: return "R32T";
    case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8T";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24T";
    case DXGI_FORMAT_R16_TYPELESS: return "R16T";
    default: return "?";
    }
}

namespace detail {
constexpr int kSets = 8;
constexpr int kTs = 1536;              // timestamps per frame (2 per game bind)
struct Set {
    ID3D11Query* dis = nullptr;
    ID3D11Query* ts[kTs] = {};
    int8_t   cls[kTs] = {};            // class of the interval that STARTS at stamp i
    int      nTs = 0;
    bool     open = false, inFlight = false;
    uint64_t frame = 0;
    int      gbufBinds = 0, binds = 0;
};
struct Info {                          // a bind signature (cache entry; identity of the views is the key)
    const void* k0 = nullptr; const void* k1 = nullptr; const void* kd = nullptr; UINT n = 0; bool used = false;
    UINT w0 = 0, h0 = 0; DXGI_FORMAT f0 = DXGI_FORMAT_UNKNOWN, f1 = DXGI_FORMAT_UNKNOWN;
    bool hasDsv = false; UINT dw = 0, dh = 0; DXGI_FORMAT df = DXGI_FORMAT_UNKNOWN; const void* dres = nullptr;
};
constexpr int kCache = 512;
inline Set      g_set[kSets];
inline Info     g_cache[kCache];
inline int      g_cur = -1, g_next = 0;
inline uint64_t g_frame = 0;
inline bool     g_broken = false, g_pendTop = false;
inline int      g_stage = 0;           // 0 = before the G-buffer, 1 = after the G-buffer, 2 = after the forward bind
inline uint64_t g_sigHash[16] = {};
inline int      g_sigN = 0;
// window
inline double   g_sum[kNCls] = {};
inline uint64_t g_frames = 0, g_binds = 0, g_gbufBinds = 0, g_dropped = 0;
inline ULONGLONG g_nextReport = 0;

inline void ReleaseSet(Set& s) {
    if (s.dis) s.dis->Release();
    for (ID3D11Query*& q : s.ts) if (q) { q->Release(); q = nullptr; }
    s = Set();
}
inline void CloseCur(ID3D11DeviceContext* ctx) {
    if (g_cur < 0) return;
    Set& s = g_set[g_cur];
    ctx->End(s.dis);
    s.open = false; s.inFlight = true;
    g_cur = -1;
}
inline ID3D11Query* Ts(ID3D11DeviceContext* ctx, Set& s, int i) {
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
// lands finished sets, oldest first; never blocks
inline void Poll(ID3D11DeviceContext* ctx) {
    for (int k = 0; k < kSets; ++k) {
        Set& s = g_set[(g_next + k) % kSets];
        if (!s.inFlight) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd{};
        if (ctx->GetData(s.dis, &dd, sizeof(dd), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
        static UINT64 t[kTs];
        bool got = true;
        for (int i = 0; i < s.nTs && got; ++i)
            got = ctx->GetData(s.ts[i], &t[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (!got) return;
        s.inFlight = false;
        if (dd.Disjoint || !dd.Frequency || s.nTs < 2) { ++g_dropped; continue; }
        const double toMs = 1000.0 / (double)dd.Frequency;
        for (int i = 0; i + 1 < s.nTs; ++i)
            if (t[i + 1] >= t[i]) g_sum[s.cls[i]] += (double)(t[i + 1] - t[i]) * toMs;
        ++g_frames; g_binds += (uint64_t)s.binds; g_gbufBinds += (uint64_t)s.gbufBinds;
    }
}
inline Set* Cur(ID3D11DeviceContext* ctx) {
    if (g_broken) return nullptr;
    if (g_cur >= 0 && g_set[g_cur].frame != g_frame) CloseCur(ctx);
    if (g_cur < 0) {
        CpuProf::Block cpb(CpuProf::kBGpuPerf);
        Poll(ctx);
        Set& s = g_set[g_next];
        if (s.inFlight || s.open) { ++g_dropped; return nullptr; }
        if (!s.dis) {
            ID3D11Device* dev = nullptr;
            ctx->GetDevice(&dev);
            D3D11_QUERY_DESC qd{};
            qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            const bool ok = dev && SUCCEEDED(dev->CreateQuery(&qd, &s.dis));
            if (dev) dev->Release();
            if (!ok) { s.dis = nullptr; g_broken = true; LogErr("GPU per pass (phase 36): query creation failed -- off"); return nullptr; }
        }
        ctx->Begin(s.dis);
        s.open = true; s.nTs = 0; s.binds = 0; s.gbufBinds = 0; s.frame = g_frame;
        g_cur = g_next; g_next = (g_next + 1) % kSets;
        g_stage = 0;
    }
    return &g_set[g_cur];
}
inline bool Stamp(ID3D11DeviceContext* ctx, int cls) {
    Set* s = Cur(ctx);
    if (!s) return false;
    if (s->nTs >= kTs) { ++g_dropped; return false; }
    CpuProf::Block cpb(CpuProf::kBGpuPerf);
    ID3D11Query* q = Ts(ctx, *s, s->nTs);
    if (!q) return false;
    ctx->End(q);
    s->cls[s->nTs++] = (int8_t)cls;
    return true;
}
inline bool Desc(ID3D11View* v, D3D11_TEXTURE2D_DESC* d, const void** res) {
    if (!v) return false;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (!r) return false;
    bool ok = false;
    ID3D11Texture2D* t = nullptr;
    if (SUCCEEDED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) { t->GetDesc(d); ok = true; t->Release(); }
    if (res) *res = r;
    r->Release();
    return ok;
}
inline uint64_t Mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h; }
}  // namespace detail

inline bool On() { return GpuPerf::On(); }
inline void SetFrame(uint64_t f) { detail::g_frame = f; }

// hkOMSetRenderTargets, top (after the game-context / t_inDlaa tests, before any mod work of the hook)
inline void BindTop(ID3D11DeviceContext* ctx) {
    using namespace detail;
    g_pendTop = Stamp(ctx, kBindWork);
    if (g_pendTop) ++g_set[g_cur].binds;
}

// The class of this bind (hkOMSetRenderTargets, after MirOnBind). `pass` = the mod's own jitter-pass test (scene depth + 4 RTV
// or 1 RTV RGBA16F scene size); sceneDepth / sceneW / sceneH as the mod knows them.
inline int Classify(UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv, bool pass, bool mirror,
                    const void* sceneDepth, UINT sceneW, UINT sceneH) {
    using namespace detail;
    if (!g_pendTop) return kOther;
    CpuProf::Block cpb(CpuProf::kBGpuPerf);
    ID3D11RenderTargetView* r0 = n > 0 && rtvs ? rtvs[0] : nullptr;
    ID3D11RenderTargetView* r1 = n > 1 && rtvs ? rtvs[1] : nullptr;
    const uint64_t hs = Mix(Mix(Mix((uint64_t)(uintptr_t)r0, (uint64_t)(uintptr_t)r1), (uint64_t)(uintptr_t)dsv), n);
    Info& c = g_cache[hs % kCache];
    if (!(c.used && c.k0 == r0 && c.k1 == r1 && c.kd == dsv && c.n == n)) {
        c = Info();
        c.used = true; c.k0 = r0; c.k1 = r1; c.kd = dsv; c.n = n;
        D3D11_TEXTURE2D_DESC d{};
        if (Desc(r0, &d, nullptr)) { c.w0 = d.Width; c.h0 = d.Height; c.f0 = d.Format; }
        if (r1 && Desc(r1, &d, nullptr)) c.f1 = d.Format;
        if (dsv) {
            c.hasDsv = true;
            if (Desc(dsv, &d, &c.dres)) { c.dw = d.Width; c.dh = d.Height; c.df = d.Format; }
        }
    }
    const bool sceneDsv = c.hasDsv && c.dres && c.dres == sceneDepth;
    const bool eyeSize = sceneW && c.w0 == sceneW && c.h0 == sceneH;
    int cls = kOther;
    if (mirror) cls = kMirror;
    else if (n == 0 || !r0) {
        cls = (c.hasDsv && !sceneDsv && !(sceneW && c.dw == sceneW && c.dh == sceneH)) ? kShadow : kOther;
    } else if (pass && n == 4) { cls = kGbuffer; g_stage = 1; }
    else if (pass && n == 1) { cls = kForward; g_stage = 2; }
    else if (g_stage == 1 && eyeSize) cls = kLighting;
    else if (g_stage == 2 && sceneW && c.w0 <= sceneW && c.h0 <= sceneH) cls = kPost;
    if (cls == kGbuffer) ++g_set[g_cur].gbufBinds;
    // the first 16 distinct signatures, in frame order, with the class assigned (to check the classification in the log)
    if (g_sigN < 16) {
        uint64_t h = Mix(Mix(Mix(Mix(Mix(n, c.w0), c.h0), (uint64_t)c.f0 * 131 + c.f1), (uint64_t)c.hasDsv * 7 + c.dw * 3 + c.dh),
                         (uint64_t)c.df * 17 + (uint64_t)sceneDsv * 5 + (uint64_t)cls * 11 + (uint64_t)mirror);
        bool seen = false;
        for (int i = 0; i < g_sigN; ++i) if (g_sigHash[i] == h) { seen = true; break; }
        if (!seen) {
            g_sigHash[g_sigN++] = h;
            LogDbg("GPU pass signature #%d (phase 36): class=%s | %u RTV, rt0 %ux%u %s, rt1 %s | dsv %s %ux%u %s | stage %d%s", g_sigN,
                ClsName(cls), n, c.w0, c.h0, FmtName(c.f0), FmtName(c.f1), !c.hasDsv ? "none" : (sceneDsv ? "SCENE-DEPTH" : "other"),
                c.dw, c.dh, FmtName(c.df), g_stage, mirror ? " | mirror view open" : "");
        }
    }
    return cls;
}

// hkOMSetRenderTargets, end: the bind's own class starts here (the mod's bind-time work is behind us)
inline void BindEnd(ID3D11DeviceContext* ctx, int cls) {
    using namespace detail;
    if (!g_pendTop) return;
    g_pendTop = false;
    Stamp(ctx, cls);
}

inline bool ReportDue() { return detail::g_nextReport <= GetTickCount64(); }

// The log line (about every 5 s): ms per frame. sceneMs < 0 = no "GPU spans" scene figure yet.
inline void Report(double sceneMs, uint32_t sceneN) {
    using namespace detail;
    const ULONGLONG now = GetTickCount64();
    if (g_nextReport == 0) { g_nextReport = now + 5000; return; }       // first call arms the window
    g_nextReport = now + 5000;
    if (!g_frames) { LogDbg("GPU per pass (phase 36): no landed frames yet (%llu dropped)", (unsigned long long)g_dropped); return; }
    const double f = (double)g_frames;
    double mod = 0.0; bool haveMod = false;
    for (int e = 0; e < 2; ++e) {
        GpuPerf::EyeStats st;
        if (GpuPerf::Stats(e, &st)) { mod += st.total + st.avg[GpuPerf::kNgx] + st.avg[GpuPerf::kMirNgx]; haveMod = true; }
    }
    double S = 0.0;
    for (int c = 0; c < kNCls; ++c) S += g_sum[c];
    char scene[48];
    if (sceneMs >= 0.0) snprintf(scene, sizeof(scene), "%.2f", sceneMs); else snprintf(scene, sizeof(scene), "n/a");
    char modS[24];
    if (haveMod) snprintf(modS, sizeof(modS), "%.2f", mod); else snprintf(modS, sizeof(modS), "n/a");
    LogDbg("GPU per pass (phase 36): shadow %.2f | gbuffer %.2f | lighting %.2f | forward %.2f | post %.2f | mirror %.2f | other %.2f | "
        "mod %s ms/frame (sum S %.2f vs scene span T %s; n %llu frames) | mod bind-time work %.2f (in S), binds/frame %.1f, "
        "G-buffer binds/frame %.2f, dropped %llu | classes include the GPU waiting time between binds; mod overlaps post/other",
        g_sum[kShadow] / f, g_sum[kGbuffer] / f, g_sum[kLighting] / f, g_sum[kForward] / f, g_sum[kPost] / f,
        g_sum[kMirror] / f, g_sum[kOther] / f, modS, S / f, scene, (unsigned long long)g_frames, g_sum[kBindWork] / f,
        (double)g_binds / f, (double)g_gbufBinds / f, (unsigned long long)g_dropped);
    (void)sceneN;
    memset(g_sum, 0, sizeof(g_sum));
    g_frames = g_binds = g_gbufBinds = g_dropped = 0;
    memset(g_cache, 0, sizeof(g_cache));            // view pointers can be reused by the game: re-derive the signatures
}

inline void Shutdown() {
    using namespace detail;
    for (Set& s : g_set) ReleaseSet(s);
    g_cur = -1; g_next = 0; g_broken = false; g_pendTop = false;
    memset(g_sum, 0, sizeof(g_sum));
    g_frames = g_binds = g_gbufBinds = g_dropped = 0;
    memset(g_cache, 0, sizeof(g_cache));
}

}  // namespace GpuPass

#endif // WITH_DLAA
