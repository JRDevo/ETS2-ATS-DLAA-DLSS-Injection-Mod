// CensusProbe -- phase 113b: the census's THIN test, "does this draw's coverage flicker under the jitter?".
//
// WHY: phases 108-113 called a pair thin when its occlusion-query pixels per triangle were < 0.5. A DENSE mesh (a building facade
// atlas, a far LOD forest, a terrain strip, a car: many tiny triangles with no gaps between them) qualifies while its surface is solid
// and steady; the tint then coloured whole buildings / hill tops cyan (the 2026-11-15 16:33 Volvo Trucks Center capture), and phase
// 111 / 113 had to hand-list 14 such pairs (Census::KnownPairs). Pixels per triangle stays as the cheap PRE-FILTER only.
//
// THE MEASURE (one "probe" of one draw, render thread, right after the game's draw): the same draw is re-drawn DEPTH-ONLY (null PS,
// no colour target, depth test off) into the mod's own render-size stencil buffer, once at each of the 8 Halton(2,3) jitter offsets
// the mod uses (its own un-jittered viewport + offset k). Pass k marks bit k of every pixel it covers (stencil NOT_EQUAL on bit k,
// REPLACE: a pixel counts once per pass however many triangles cover it -- so a dense mesh is NOT counted per triangle); one
// occlusion query spans the 8 passes = SUM over the phases of the pixels covered = 8 x the mean coverage M. A 9th pass (offset 0,
// stencil EQUAL 0xFF, op ZERO: each pixel once) counts the pixels covered at ALL 8 offsets = I. STABLE FRACTION S = I / M:
//   a solid region of w x h pixels: S ~ (w - 0.81)(h - 0.78) / (w h) (the 8 offsets span 0.81 x 0.78 px) -- 50 x 50 px 0.97, a 6 px
//   box 0.75, a 3 px box 0.53; whatever its triangles: one big quad or 30 000 sub-pixel ones give the same S;
//   a post / wire / rail of width w < 0.8 px: S = 0 (no pixel is covered at every offset: it dashes), w = 1 px: 0.19, 1.5 px: 0.46.
// S is per PIXEL, so many thin instances in one draw (57 delineator posts) or a sloped wire do not average out the way a scalar
// pixel count does (the coverage-wobble idea -- one occlusion count per jitter phase, (max - min) / mean -- measured ~0.0 - 0.1 for
// both on WARP: tests/census_test.cpp Phase113b). Measured in ONE frame: the camera / the object may move (no "static" gate needed).
// THIN = S < kStableThin over a probe with a mean coverage of at least kProbeMinPx pixels (smaller draws are no evidence either way).
//
// THE VERDICT (per pair, PairProbe): "measuring" until kPairProbesMin probes with enough pixels -- never tinted, never a suspect --
// then THIN when at least kPairThinMin probes AND kPairThinShare of them were thin, else STEADY. Per DRAW (KeyTable): the last probe's
// verdict of that draw (pair + draw arguments + world-pass position + the n-th such draw in the pass): the census tint colours only
// the draws of a thin pair whose own last probe was thin (a sign atlas PS that draws big windows AND thin sign posts: the posts only).
//
// Pure parts (constants, offsets, the verdicts, the draw key table) + the D3D11 probe itself (Kit / Run: header-only, any D3D11 device,
// tested on WARP by tests/census_test.cpp). inject.cpp's src/census_run.h decides WHEN to probe (budget, pairs, mirror / VR rules).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <d3d11.h>

namespace CensusProbe {

constexpr int    kPhases = 8;            // the Halton(2,3) offsets 1..8 (stencil bits 0..7)
constexpr double kStableThin = 0.35;     // stable fraction below this = thin (a post / wire under ~1.3 px, a blob under ~2.2 px)
constexpr double kProbeMinPx = 8.0;      // mean pixels per phase below this: the probe is no evidence (a 1-3 px glint / far sprite)
constexpr int    kPairProbesMin = 3;     // probes with enough pixels before a pair verdict
constexpr int    kPairThinMin = 2;       // thin probes for a THIN pair ...
constexpr double kPairThinShare = 0.25;  // ... and at least this share of its probes
constexpr int    kPairProbesFull = 48;   // a pair with this many probes: only its draws without a fresh verdict are probed
constexpr uint64_t kKeyFresh = 900;      // Presents a draw's verdict counts as fresh (~15 s at 60 fps): no new probe of it before

// the jitter offset of phase k in pixels -- the mod's JitterOfPhase (inject.cpp): Halton(k + 1, 2 / 3) - 0.5
inline float HaltonP(int index, int base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) { f /= (float)base; r += f * (float)(index % base); index /= base; }
    return r;
}
inline void Offset(int k, float* x, float* y) {
    *x = HaltonP(k + 1, 2) - 0.5f;
    *y = HaltonP(k + 1, 3) - 0.5f;
}

// ---- one probe's result ---------------------------------------------------------------------------------------------------------
enum Verdict : int { kMeasuring = 0, kSteady = 1, kThin = 2, kSmall = 3 };   // (kSmall: a probe under kProbeMinPx)
inline const char* VerdictName(int v) {
    switch (v) {
    case kSteady: return "steady";
    case kThin: return "THIN";
    case kSmall: return "too small";
    default: return "measuring";
    }
}
struct Result {
    uint64_t sum8 = 0;   // pixels covered, summed over the 8 offsets (each pixel once per offset)
    uint64_t all = 0;    // pixels covered at all 8 offsets
    double Mean() const { return (double)sum8 / (double)kPhases; }
    double Stable() const { const double m = Mean(); return m > 0.0 ? (double)all / m : 0.0; }
    int Verdict() const { return Mean() < kProbeMinPx ? kSmall : (Stable() < kStableThin ? kThin : kSteady); }
};

// ---- the pair's verdict over its probes ---------------------------------------------------------------------------------------
struct PairProbe {
    int      probes = 0;                 // probes with enough pixels
    int      thin = 0;                   // ... of them thin
    int      tiny = 0;                   // probes under kProbeMinPx (no evidence)
    uint64_t sum8 = 0, all = 0;          // pooled over the counted probes (the file's "stable fraction")
    double   sMin = 1.0;                 // the lowest stable fraction of a counted probe
    void Add(const Result& r) {
        const int v = r.Verdict();
        if (v == kSmall) { ++tiny; return; }
        ++probes;
        if (v == kThin) ++thin;
        sum8 += r.sum8; all += r.all;
        const double s = r.Stable();
        if (s < sMin) sMin = s;
    }
    double Stable() const { return sum8 ? (double)all * (double)kPhases / (double)sum8 : 0.0; }
    int Verdict() const {
        if (probes < kPairProbesMin) return kMeasuring;
        return (thin >= kPairThinMin && (double)thin >= kPairThinShare * (double)probes) ? kThin : kSteady;
    }
};

// ---- the draw key table (render thread): the last probe verdict per draw -------------------------------------------------------
// A draw = the pair + its draw arguments + the world pass position (VR: the eye pass; the census probes positions 0 / 1 only) + the
// ordinal among the draws with the same key in this pass (the same mesh drawn at several places: up to kOrdMax, then no verdict).
// Phase 115r (audit B6): 2-WAY buckets (was direct-mapped: ~11 % of 2000 live keys collided, replaced each other every pass and never
// kept a verdict = never flashed). A new key takes a free way, else the way not drawn for the longest -- but only one not drawn for
// kKeyKeepPasses passes: two live keys of a bucket both keep their verdicts; a third live one gets none (-1) until one goes stale.
// Phase 115r (audit B3): in VR the pass position is NOT in the key (KeyPos): both eyes' draws of a mesh share one verdict (eye 0's
// probe used to spend the budget and the flash came in one eye only).
constexpr int kKeySlots = 16384;         // power of two (8192 buckets x 2 ways)
constexpr int kOrdMax = 4;
constexpr uint64_t kKeyKeepPasses = 240; // a way drawn within this many passes is never evicted (VR: 2 passes per frame = ~2 s)
inline int KeyPos(bool vr, int passPos) { return vr ? 0 : passPos; }
inline uint64_t DrawKey(int pair, uint32_t ic, uint32_t si, int32_t bv, uint32_t inst, uint32_t sinst, int pos, int ord) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)pair;
    auto mix = [&h](uint64_t v) {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 31;
    };
    mix(ic); mix(si); mix((uint32_t)bv); mix(inst); mix(sinst); mix((uint64_t)(pos * 16 + ord));
    return h | 1ull;                     // (0 = an empty slot)
}
struct KeySlot {
    uint64_t key = 0;
    uint64_t pass = 0;                   // pass serial + 1 of its last draw (the ordinal: taken in this pass)
    uint64_t at = 0;                     // Present + 1 of its last probe (0 = never probed)
    float    stable = 0.0f;
    uint8_t  verdict = kMeasuring;       // the last probe's: kSteady / kThin / kSmall (kMeasuring = never probed)
};
struct KeyTable {
    KeySlot s[kKeySlots];
    // the slot of this draw in pass `passSeq` (claims / replaces one); -1 = more than kOrdMax draws with the same key in this pass
    int Resolve(int pair, uint32_t ic, uint32_t si, int32_t bv, uint32_t inst, uint32_t sinst, int pos, uint64_t passSeq) {
        for (int ord = 0; ord < kOrdMax; ++ord) {
            const uint64_t k = DrawKey(pair, ic, si, bv, inst, sinst, pos, ord);
            const int b = (int)(k & (uint64_t)(kKeySlots - 2));   // the bucket's first way (even)
            int hit = -1;
            for (int w = 0; w < 2; ++w) if (s[b + w].key == k) hit = b + w;
            if (hit >= 0) {
                KeySlot& e = s[hit];
                if (e.pass == passSeq + 1) continue;   // an earlier draw with the same arguments in this pass: the next ordinal
                e.pass = passSeq + 1;
                return hit;
            }
            int v = -1;                                // a free way, else the stalest -- if it is stale enough
            for (int w = 0; w < 2 && v < 0; ++w) if (!s[b + w].key) v = b + w;
            if (v < 0) {
                const int o = s[b].pass <= s[b + 1].pass ? b : b + 1;
                if (passSeq + 1 >= s[o].pass + kKeyKeepPasses) v = o;
            }
            if (v < 0) return -1;                      // both ways live: no verdict for this draw now (a later pass may get one)
            KeySlot& e = s[v];
            e = KeySlot{};
            e.key = k; e.pass = passSeq + 1;
            return v;
        }
        return -1;
    }
    // a probe result of slot i (its key must still be `key`: a collision may have replaced it meanwhile)
    void Note(int i, uint64_t key, const Result& r, uint64_t present) {
        if (i < 0 || i >= kKeySlots || s[i].key != key) return;
        s[i].verdict = (uint8_t)r.Verdict();
        s[i].stable = (float)r.Stable();
        s[i].at = present + 1;
    }
    bool Fresh(int i, uint64_t present) const {
        return i >= 0 && s[i].at != 0 && present + 1 >= s[i].at && present + 1 - s[i].at <= kKeyFresh;
    }
    bool Thin(int i) const { return i >= 0 && s[i].verdict == kThin; }
};

// ---- the D3D11 probe ------------------------------------------------------------------------------------------------------------
// The mod's objects (one per world pass position: VR eyes may differ in size): a render-size D24S8 stencil buffer + 9 depth-stencil
// states (8 per-phase bit states, 1 "covered at all 8" state).
struct Kit {
    ID3D11Device*            dev = nullptr;
    ID3D11Texture2D*         tex = nullptr;
    ID3D11DepthStencilView*  dsv = nullptr;
    UINT                     w = 0, h = 0;
    ID3D11DepthStencilState* bit[kPhases] = {};
    ID3D11DepthStencilState* allState = nullptr;
    bool                     statesFailed = false;
    int                      creates = 0;      // stencil buffers made (a size change makes a new one)
    void ReleaseTarget() {
        if (dsv) { dsv->Release(); dsv = nullptr; }
        if (tex) { tex->Release(); tex = nullptr; }
        w = h = 0;
    }
    void Release() {
        ReleaseTarget();
        for (auto*& s : bit) if (s) { s->Release(); s = nullptr; }
        if (allState) { allState->Release(); allState = nullptr; }
        dev = nullptr;
    }
    // the states once, the stencil buffer at this size (true = usable; `cleared` = it was (re)made: the caller clears it once)
    bool Ensure(ID3D11Device* d, UINT width, UINT height, bool* made) {
        if (made) *made = false;
        if (!d || !width || !height || statesFailed) return false;
        if (dev && dev != d) Release();
        dev = d;
        if (!allState) {
            D3D11_DEPTH_STENCIL_DESC ds{};
            ds.DepthEnable = FALSE;
            ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
            ds.StencilEnable = TRUE;
            for (int k = 0; k < kPhases && !statesFailed; ++k) {
                ds.StencilReadMask = ds.StencilWriteMask = (UINT8)(1u << k);
                ds.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_NOT_EQUAL };
                ds.BackFace = ds.FrontFace;
                if (FAILED(d->CreateDepthStencilState(&ds, &bit[k]))) statesFailed = true;
            }
            ds.StencilReadMask = ds.StencilWriteMask = 0xFF;
            ds.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_ZERO, D3D11_COMPARISON_EQUAL };
            ds.BackFace = ds.FrontFace;
            if (!statesFailed && FAILED(d->CreateDepthStencilState(&ds, &allState))) statesFailed = true;
            if (statesFailed) { Release(); statesFailed = true; return false; }
        }
        if (tex && w == width && h == height) return true;
        ReleaseTarget();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = width; td.Height = height; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(d->CreateTexture2D(&td, nullptr, &tex))) { tex = nullptr; return false; }
        if (FAILED(d->CreateDepthStencilView(tex, nullptr, &dsv))) { ReleaseTarget(); return false; }
        w = width; h = height;
        ++creates;
        if (made) *made = true;
        return true;
    }
};

// the size of the bound targets (the game's depth view, else render target 0); false = none / multisampled / not a 2D texture
inline bool TargetSize(ID3D11DepthStencilView* dsv, ID3D11RenderTargetView* rtv0, UINT* w, UINT* h) {
    ID3D11Resource* r = nullptr;
    if (dsv) dsv->GetResource(&r);
    else if (rtv0) rtv0->GetResource(&r);
    if (!r) return false;
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    r->GetType(&dim);
    bool ok = false;
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d{};
        static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
        ok = d.SampleDesc.Count == 1 && d.Width && d.Height;
        *w = d.Width; *h = d.Height;
    }
    r->Release();
    return ok;
}

// The probe of the draw `redraw()` re-issues (the game's own call, its state bound). shiftX / Y = the jitter shift the bound viewport
// carries now (the mod's ReconcileViewports shift; 0 when unshifted): the 8 passes use the un-jittered viewport + Offset(k). qSum
// spans the 8 passes, qAll the 9th (both occlusion queries, results read later by the caller). clear() clears the kit's stencil to 0
// (inject.cpp: the original ClearDepthStencilView, not the hooked one). TEST ONLY: qPhase = 8 occlusion queries, one around each
// phase pass (the per-phase pixel counts; qSum must then be null -- no nested queries). The game's state is restored exactly:
// render targets (UAVs kept), depth-stencil state + ref, viewports, the pixel shader + class instances. Returns false (nothing
// drawn) when the targets are multisampled / unknown or the kit cannot be made.
// kits / nKits: the kit whose buffer has the target's size is used (else an unused one, else the first is re-made: VR eyes of two
// sizes keep one buffer each). clear(kit) clears that kit's stencil.
template <class Redraw, class Clear>
bool Run(ID3D11DeviceContext* ctx, Kit* kits, int nKits, float shiftX, float shiftY, ID3D11Query* qSum, ID3D11Query* qAll,
         Redraw&& redraw, Clear&& clear, ID3D11Query* const* qPhase = nullptr) {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* gdsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &gdsv);
    auto releaseTargets = [&] {
        for (auto*& v : rtv) if (v) { v->Release(); v = nullptr; }
        if (gdsv) { gdsv->Release(); gdsv = nullptr; }
    };
    UINT w = 0, h = 0;
    bool made = false;
    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (dev) dev->Release();                              // (identity only: the context holds the device)
    if (!kits || nKits < 1 || !TargetSize(gdsv, rtv[0], &w, &h)) { releaseTargets(); return false; }
    Kit* pick = nullptr;
    for (int i = 0; i < nKits && !pick; ++i) if (kits[i].tex && kits[i].w == w && kits[i].h == h) pick = &kits[i];
    for (int i = 0; i < nKits && !pick; ++i) if (!kits[i].tex) pick = &kits[i];
    Kit& kit = pick ? *pick : kits[0];
    if (!kit.Ensure(dev, w, h, &made)) { releaseTargets(); return false; }
    // the game's state
    ID3D11DepthStencilState* gdss = nullptr;
    UINT gref = 0;
    ctx->OMGetDepthStencilState(&gdss, &gref);
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&nvp, vp);
    ID3D11PixelShader* gps = nullptr;
    ID3D11ClassInstance* gci[8] = {};
    UINT nci = 8;
    ctx->PSGetShader(&gps, gci, &nci);
    // the probe
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, kit.dsv, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
    if (made) clear(kit);                                 // (a new buffer: undefined content; afterwards every probe leaves it 0)
    ctx->PSSetShader(nullptr, nullptr, 0);
    D3D11_VIEWPORT v[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    const UINT nv = nvp ? nvp : 1;
    if (!nvp) { vp[0] = D3D11_VIEWPORT{ 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f }; }
    auto at = [&](int k) {
        float ox, oy;
        Offset(k, &ox, &oy);
        memcpy(v, vp, sizeof(D3D11_VIEWPORT) * nv);
        v[0].TopLeftX = vp[0].TopLeftX - shiftX + ox;
        v[0].TopLeftY = vp[0].TopLeftY - shiftY + oy;
        ctx->RSSetViewports(nv, v);
    };
    if (qSum && !qPhase) ctx->Begin(qSum);
    for (int k = 0; k < kPhases; ++k) {
        at(k);
        ctx->OMSetDepthStencilState(kit.bit[k], 0xFF);
        if (qPhase && qPhase[k]) ctx->Begin(qPhase[k]);
        redraw();
        if (qPhase && qPhase[k]) ctx->End(qPhase[k]);
    }
    if (qSum && !qPhase) ctx->End(qSum);
    at(0);                                                // (I is inside the offset-0 coverage)
    ctx->OMSetDepthStencilState(kit.allState, 0xFF);
    if (qAll) ctx->Begin(qAll);
    redraw();
    if (qAll) ctx->End(qAll);
    // back to 0: the pixels the 9th pass did not zero (covered at some offsets only) -- the 8 passes again with op ZERO would cost 8
    // more draws; one clear of the mod's own buffer is cheaper on the GPU than 8 vertex passes and costs one call
    clear(kit);
    // the game's state, exactly
    if (nvp) ctx->RSSetViewports(nvp, vp);
    UINT nrt = 0;                                         // (the game's count: a UAV bound after its targets keeps its slot)
    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) if (rtv[i]) nrt = i + 1;
    ctx->OMSetRenderTargetsAndUnorderedAccessViews(nrt, nrt ? rtv : nullptr, gdsv, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
    ctx->OMSetDepthStencilState(gdss, gref);
    ctx->PSSetShader(gps, nci ? gci : nullptr, nci);
    if (gdss) gdss->Release();
    if (gps) gps->Release();
    for (UINT i = 0; i < nci && i < 8; ++i) if (gci[i]) gci[i]->Release();
    releaseTargets();
    return true;
}

}  // namespace CensusProbe
