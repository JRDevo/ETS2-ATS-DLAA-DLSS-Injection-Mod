// CameraMv -- camera-reprojection motion vectors for ETS2/ATS (v0.4.2).
// The engine has no velocity buffer and no per-frame camera cbuffer, only a
// per-draw MVP in a big dynamic VS constant-buffer ring. Method (details in
// docs/DLAA_INTEGRATION.md):
//   * While the main G-buffer pass runs, DrawIndexed is hooked; for the first N
//     draws per depth layer (world / cabin) we GPU-copy the draw's 64-byte MVP
//     (VS cb slot 0, byte offset 64..127 of the draw's window) into a candidate
//     buffer and remember a geometry key (IB, VB, offsets, counts).
//   * At the swapchain blit: keys present in BOTH this and the previous frame's
//     list are static-object candidates. R = MVP_prev * inverse(MVP_cur) maps
//     current clip space to previous clip space for everything static in that
//     layer (world matrix cancels). A compute pass computes R per pair and picks
//     the medoid per layer (rejects moving objects); a second pass reprojects
//     every pixel (layer from its depth value) into an RG16F MV texture.
//   * v0.6.0: a third matrix R_ego (medoid of the near world draws that move WITH the camera: own truck
//     mirrors / hood / wheels) is used for world-layer pixels closer than mv_ego_pixel_m.
//   MV convention (DLSS): current_pixel + mv = previous_pixel, pixels, origin
//   top-left, MVScale (1,1), not jittered.
// Compiled only when WITH_DLAA=1.
#pragma once
#ifdef WITH_DLAA

#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

// One pass's worth of MV candidates (v0.5.4): a GPU buffer of per-draw MVPs + the CPU geometry keys. Recorded
// per G-buffer PASS (the pass FIFO slot owns one) and committed to an eye's CameraMv at that pass's blit.
class CandidateRecord {
public:
    static constexpr int  kLayers    = 2;      // 0 = world, 1 = cabin
    static constexpr int  kSlots     = 128;    // candidates per layer
    static constexpr UINT kSlotBytes = 64;     // one float4x4
    static constexpr UINT kBytes     = kLayers * kSlots * kSlotBytes;   // 16384

    struct DrawKey {
        const void* ib; const void* vb;
        UINT ibOffset, vbOffset;
        UINT indexCount, startIndex;
        INT  baseVertex;
    };

    bool Init(ID3D11Device* dev);                       // lazily creates the buffer; false on failure
    bool Ready() const { return m_buf != nullptr; }
    void Reset() { for (int l = 0; l < kLayers; ++l) m_count[l] = 0; }
    bool LayerFull(int layer) const { return m_count[layer] >= kSlots; }
    int  Count(int layer) const { return m_count[layer]; }
    const DrawKey& Key(int layer, int i) const { return m_keys[layer][i]; }
    // GPU-copies 64 bytes at `srcByteOffset` of `src` into the next free slot of `layer`, stores `key`.
    void Record(ID3D11DeviceContext* ctx, int layer, ID3D11Buffer* src, UINT srcByteOffset, const DrawKey& key);
    // Becomes a copy of `o` (GPU CopyResource + keys + counts).
    void CopyFrom(ID3D11DeviceContext* ctx, const CandidateRecord& o);
    ID3D11Buffer* Buffer() const { return m_buf.Get(); }
    ID3D11ShaderResourceView* Srv() const { return m_srv.Get(); }
    void Shutdown() { m_srv.Reset(); m_buf.Reset(); Reset(); }

private:
    Microsoft::WRL::ComPtr<ID3D11Buffer>             m_buf;   // raw, 2 layers x 128 slots x 64 B
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_srv;
    int     m_count[kLayers] = {};
    DrawKey m_keys[kLayers][kSlots];
};

class CameraMv {
public:
    static constexpr int kLayers = CandidateRecord::kLayers;      // 0 = world, 1 = cabin
    static constexpr int kSlots  = CandidateRecord::kSlots;       // candidates per layer per frame
    static constexpr UINT kSlotBytes = CandidateRecord::kSlotBytes;
    static constexpr int kPairs  = 16;     // max matched pairs per layer (buffer stride)
    static constexpr int kWorldPairs = 16; // world pair budget (v0.4.1: was 8)
    static constexpr int kCabinPairs = 8;  // cabin pair budget (unchanged)
    // v0.6.0 solve buffer (structured float4): [0..3] R_world, [4..7] R_cabin, [8],[9] per-layer info,
    // [10],[11] chosen-pair info, [12..15] R_ego, [16] InvRow3 (row 3 of inverse(MVP_cur) of the chosen world
    // pair), [17] ego info (candidates, used after the static reject, fallback to R_world, |R_ego - R_world|).
    static constexpr int kSolveElems  = 18;
    static constexpr int kSolveFloats = kSolveElems * 4;   // 72
    using DrawKey = CandidateRecord::DrawKey;

    struct FrameStats {                    // result of the last Generate()
        int  pairs[kLayers] = {0, 0};
        bool worldMiss = true;             // world layer had no usable pair
        bool reused = false;               // v0.6.1: no matching / pass A; pass B ran with the last good R
    };

    // Creates shaders + buffers (device only; independent of the render size).
    // Safe to call repeatedly; returns false (and stays unusable) on failure.
    bool Init(ID3D11Device* dev);
    bool Ready() const { return m_ready; }

    // Blit side --------------------------------------------------------------
    // v0.5.4: `cur` = the candidate record of the pass being blitted; the eye's previous COMMITTED record
    // (m_prev) is "prev". Matches them, runs both compute passes into `mvUav` (RG16F, w x h), then commits
    // `cur` as this eye's new prev. Caller saved/restores CS state. Returns false if the passes could not
    // run (mvUav is then left untouched).
    // v0.5.7: pass B is merged with the depth convert: it reads the depth TWIN (`depthTwinSrv`,
    // R32_FLOAT_X8X24_TYPELESS view, w x h) and writes the flattened depth into `depthR32Uav` (R32F, w x h)
    // as well as the MVs. true <=> both were written; on false NEITHER was touched (all failure returns come
    // before any dispatch) and the caller must run its own depth convert.
    // v0.6.1 `candBad` (the caller classified `cur` as an incomplete record): no matching, no pass A; pass B
    // reprojects with the solve buffer as it is (R of the last good frame), `cur` is NOT committed and prev is
    // marked stale. The first good record after that (prev is then >= 2 frames old) also reuses R once, but
    // is committed. Both: stats->worldMiss = false, stats->reused = true. Only while the solve buffer holds a
    // good solve (pass A with world pairs since the last Invalidate); otherwise the normal path runs.
    // v0.6.4 DLAA area: (w, h) = the crop (dispatch size, = the MV / R32F textures), (x0, y0) = its origin in
    // the full image, (fullW, fullH) = the full image (= the depth twin). Pass B reads the twin at origin + id
    // and computes uv / ndc / the MV pixel scale from the FULL size, so the MVs are unchanged pixel deltas.
    // Whole image: x0 = y0 = 0, fullW = w, fullH = h (identical math to v0.6.3).
    bool Generate(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, uint32_t x0, uint32_t y0,
                  uint32_t fullW, uint32_t fullH, const CandidateRecord& cur,
                  ID3D11ShaderResourceView* depthTwinSrv, ID3D11UnorderedAccessView* depthR32Uav,
                  ID3D11UnorderedAccessView* mvUav, FrameStats* stats, bool candBad = false);
    // Commits `cur` as the new prev without generating (frame without MV generation). Clears the stale flag.
    void Commit(ID3D11DeviceContext* ctx, const CandidateRecord& cur);
    // Forgets every candidate (toggle, resize): next frame has no history. v0.6.1: also forgets the good
    // solve / stale state and bumps InvalidateCount() (the blit's last-good record bookkeeping keys on it).
    void Invalidate();
    uint64_t InvalidateCount() const { return m_invalidations; }
    uint64_t BadFrames() const { return m_badFrames; }

    void Shutdown();

    // World-layer near filter (v0.4.1): pairs whose object-origin view depth |w|
    // is below this many metres are excluded from the medoid (own truck, trailer).
    void SetNearReject(float m) { m_nearReject = m < 0.0f ? 0.0f : m; }
    float NearReject() const { return m_nearReject; }

    // v0.6.0 ego reprojection: world draws whose object-origin view depth is below EgoOrigin metres are
    // ego candidates (own truck exterior: mirrors, hood, wheels); world-layer pixels closer than EgoPixel
    // metres use R_ego instead of R_world (0 = off, everything world as before).
    void SetEgoOrigin(float m) { m_egoOrigin = m < 0.0f ? 0.0f : (m > 30.0f ? 30.0f : m); }
    void SetEgoPixel(float m)  { m_egoPixel  = m < 0.0f ? 0.0f : (m > 20.0f ? 20.0f : m); }
    float EgoOrigin() const { return m_egoOrigin; }
    float EgoPixel() const  { return m_egoPixel; }

    // v0.6.0 MV debug view: the solve buffer SRV and the dims cbuffer (Size, EgoPixelM; v0.6.4 + Origin, FullSize) of the last
    // Generate(), so the debug shader can redo pass B's ego classification. Only valid after a Generate()
    // that returned true.
    ID3D11ShaderResourceView* SolveSrv() const { return m_solveSrv.Get(); }
    ID3D11Buffer* DimsCb() const { return m_cbDims.Get(); }

    const FrameStats& Last() const { return m_last; }

    // v0.5.1: every Generate() copies the solve buffer into a ring of 3 staging buffers and polls the
    // oldest ones with MAP_FLAG_DO_NOT_WAIT (never stalls). TakeSolve() returns true once per new
    // readback and fills R_world (row-major 16 floats, 2-3 frames old).
    // v0.6.0: the ring reads the whole solve buffer (m_rLast = kSolveFloats); TakeSolve still hands out R_world.
    bool TakeSolve(float rWorld[16]) {
        if (!m_rNew) return false;
        m_rNew = false;
        for (int i = 0; i < 16; ++i) rWorld[i] = m_rLast[i];
        return true;
    }

    // Snapshot (v0.4.2, F10): blocking readback of the solve buffer (v0.6.0: kSolveFloats = 72 floats:
    // R_world[0..15], R_cabin[16..31], stats[32..47], R_ego[48..63], InvRow3[64..67], ego info[68..71]) as
    // left by the last Generate(). Uses its own staging buffer.
    bool ReadSolveBlocking(ID3D11DeviceContext* ctx, float out[kSolveFloats]);

private:
    void DebugReadback(ID3D11DeviceContext* ctx, const CandidateRecord& cur);
    void SolveReadback(ID3D11DeviceContext* ctx);
    void RingStep(ID3D11DeviceContext* ctx);
    // v0.6.1 shared by the normal and the reuse path of Generate().
    bool WriteDims(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, uint32_t x0, uint32_t y0,
                   uint32_t fullW, uint32_t fullH);
    void DispatchReproj(ID3D11DeviceContext* ctx, uint32_t w, uint32_t h, ID3D11ShaderResourceView* depthTwinSrv,
                        ID3D11UnorderedAccessView* depthR32Uav, ID3D11UnorderedAccessView* mvUav);
    void LogFrameLine(const FrameStats& fs, const CandidateRecord& cur);

    static constexpr int kRing = 3;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_ring[kRing];
    bool     m_ringPending[kRing] = {};
    uint64_t m_ringSeq[kRing] = {};
    uint64_t m_ringCtr = 0;
    float    m_rLast[kSolveFloats] = {};   // v0.6.0: whole solve buffer (was R_world only)
    bool     m_rNew = false;
    bool     m_egoLogged = false;          // v0.6.0 one-time "ego reprojection active" line

    bool m_ready = false;
    bool m_failed = false;
    uint64_t m_frame = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> m_dev;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_csSolve, m_csReproj;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_solve;                  // kSolveElems (18) x float4 structured
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_solveUav;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_solveSrv;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbPairs, m_cbDims;      // dynamic constant buffers (v0.6.4: dims 32 B)
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_stageDbg, m_stageSolve; // staging readbacks
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_stageSnap;              // blocking snapshot readback (on demand)

    CandidateRecord m_prev;                                        // this eye's last committed record

    FrameStats m_last;
    bool     m_loggedFirst = false;
    bool     m_dbgDone = false;
    int      m_solvePending = 0;      // frames until the solve staging copy is read
    uint64_t m_solveFrame = 0;        // frame number the pending staging copy was taken at
    int      m_driveLogs = 0;         // "R_world not identity" lines written (cap 200)
    float    m_nearReject = 30.0f;
    float    m_egoOrigin = 8.0f;      // v0.6.0 mv_ego_origin_m
    float    m_egoPixel = 3.0f;       // v0.6.0 mv_ego_pixel_m (0 = off)
    uint64_t m_missFrames = 0, m_genFrames = 0;
    uint64_t m_pairSum[kLayers] = {0, 0};
    // ---- v0.6.1 reuse of the last good reprojection on incomplete candidate records ----
    bool     m_solveGood = false;     // solve buffer holds a pass A result WITH world pairs (since Invalidate)
    bool     m_prevStale = false;     // a bad record was skipped: m_prev is >= 2 frames old
    uint64_t m_badFrames = 0;         // bad records answered with the last good R (not committed)
    uint64_t m_staleFrames = 0;       // first good records after a bad one (R reused once more, committed)
    uint64_t m_invalidations = 0;     // Invalidate() calls
};

#endif // WITH_DLAA
