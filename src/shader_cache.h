// ShaderCache (v0.7.8) -- every embedded HLSL shader is compiled ONCE per process, on a worker thread, long before
// the first DLAA unit needs it. Before v0.7.8 each unit D3DCompile'd its shaders inside its first Run on the render
// thread (CameraMv::Init alone: ~1.9 s for the solve + reprojection shaders, per instance), which froze the game for
// 2 s at "Continue career" (flat, world unit) and 4 s at the VR main menu (two preview units).
// Flow: each module registers its sources (Add, static arrays that live for the process) from the setup thread,
// then Start() spawns the worker (never from DllMain: the loader lock). The worker compiles everything in
// registration order with the same flags as before (D3DCOMPILE_OPTIMIZATION_LEVEL3) and keeps the bytecode blobs
// for the process life; D3DCompile needs no device. Units create their shader OBJECTS from the blobs (fast,
// per device). Until Done() is true a unit simply does not run (the frame passes through untouched, nothing is
// marked failed): callers test SceneDlaa::ShadersReady() / ShaderCache::Done() and retry next frame.
// v0.10.0 phase 17: the worker compiles the CORE group (ids 0..kCoreLast: everything a menu / truck-preview unit needs) first,
// then the tuning menu quad, then the per-object / per-draw world set, and publishes every blob as soon as it is compiled
// (Ready / Code per id). CoreDone() = the core group is through (~2.1 s after Start; Done() = all, ~20 s at phase 16). The
// world / mirror units keep waiting for Done(); the menu / truck-preview units, their jitter and the tuning menu do not.
// v0.10.0 phase 17 also: several workers (hardware threads / 2, 2..8; the extra ones below normal priority) take the entries
// in that order, and every compiled DXBC is kept on disk next to the game exe (dlaa_shader_cache\<name>_<key>.dxbc, key =
// hash of source / entry / profile / flags / compiler version, header + payload hash checked at load; a bad or stale file is
// ignored, recompiled and rewritten) -- later starts load instead of compiling. Still never on the render thread.
// Compiled only when WITH_DLAA=1.
#pragma once
#ifdef WITH_DLAA

#include <cstddef>

namespace ShaderCache {

enum Id : int {
    kMvSolve = 0,          // motion_vectors.cpp kSolveShader            cs_5_0 (the slow one, ~1.5 s)
    kMvReprojDepth,        // motion_vectors.cpp kReprojDepthShader      cs_5_0
    kDepthConvert,         // scene_dlaa.cpp     kDepthConvertShader     cs_5_0
    kRcas,                 // scene_dlaa.cpp     kRcasShader             cs_5_0
    kCompositeVs,          // scene_dlaa.cpp     kCompositeVs            vs_5_0 (upscale composite)
    kCompositePs,          // scene_dlaa.cpp     kCompositePs            ps_5_0
    kMvDebug,              // scene_dlaa.cpp     kMvDebugShader          cs_5_0 (Ctrl+F6 view)
    kPvBlitVs,             // preview_blit.cpp   kPvBlitVs               vs_5_0 (flat profile-screen preview)
    kPvBlitPs,             // preview_blit.cpp   kPvBlitPs               ps_5_0
    kPvStats,              // inject.cpp         kPvStatsShader          cs_5_0 (Ctrl+F10 preview capture metrics)
    kPvDepthAsm,           // inject.cpp         kPvDepthAsmShader       cs_5_0 (tiled preview: picture depth assembly)
    kPvEdgeFill,           // inject.cpp         kPvEdgeFillShader       cs_5_0 (v0.8.1: preview tile edge, zero-guarded fill)
                           // ^ v0.10.0 phase 17: the last id of the CORE group (kCoreLast): new world-only shaders go after it
    kMvObjects,            // motion_vectors.cpp kObjShader              cs_5_0 (v0.9.0 per-object MVs: gather + verdict + id R)
    kMvObjResolve,         // motion_vectors.cpp kObjShader / CSResolve  cs_5_0 (v0.9.0 r4: per-id representative R)
    kMvObjVeto,            // motion_vectors.cpp kObjShader / CSVeto     cs_5_0 (v0.9.0 r4: same-frame id veto)
    kMvObjHold,            // motion_vectors.cpp kObjShader / CSHold     cs_5_0 (v0.9.0 r7: per-id veto verdict / hold)
    kDrawIdPs,             // draw_ids.cpp       kDrawIdPs               ps_5_0 (v0.10.0 draw-id replay pixel shader)
    kDrawIdGather,         // draw_ids.cpp       kDrawIdCs / CSGather    cs_5_0 (v0.10.0 per-draw MVs: MVP gather)
    kDrawIdMatch,          // draw_ids.cpp       kDrawIdCs / CSMatch     cs_5_0 (v0.10.0: nearest partner, both sides)
    kDrawIdResolve,        // draw_ids.cpp       kDrawIdCs / CSResolve   cs_5_0 (v0.10.0: mutual pairs -> R per draw)
    kDrawIdPair,           // draw_ids.cpp       kDrawIdCs / CSPair      cs_5_0 (v0.10.0 phase 3: mutual pair + R_draw)
    kDrawIdVote,           // draw_ids.cpp       kDrawIdCs / CSVote      cs_5_0 (v0.10.0 phase 3: consensus votes)
    kDrawIdPick,           // draw_ids.cpp       kDrawIdCs / CSPick      cs_5_0 (v0.10.0 phase 3: consensus camera R)
    kDrawIdAttach,         // draw_ids.cpp       kDrawIdCs / CSAttach    cs_5_0 (v0.10.0 phase 4: forward draws on a mover)
    kDrawIdInherit1,       // draw_ids.cpp       kDrawIdCs / CSInherit1  cs_5_0 (v0.10.0 phase 5: twin table + mover list)
    kDrawIdTwin,           // draw_ids.cpp       kDrawIdCs / CSTwin      cs_5_0 (v0.10.0 phase 5: unpaired draws -> matrix twin)
    kDrawIdParentList,     // draw_ids.cpp       kDrawIdCs / CSParentList  cs_5_0 (v0.10.0 phase 6: unpaired draws -> U slots)
    kDrawIdParentVote,     // draw_ids.cpp       kDrawIdCs / CSParentVote  cs_5_0 (v0.10.0 phase 6: pixel-neighbour parent vote)
    kDrawIdParentPick1,    // draw_ids.cpp       kDrawIdCs / CSParentPick1 cs_5_0 (v0.10.0 phase 6: rigid parent -> R)
    kDrawIdParentPick2,    // draw_ids.cpp       kDrawIdCs / CSParentPick2 cs_5_0 (v0.10.0 phase 6: parent via an unpaired neighbour)
    kDrawIdParentCount,    // draw_ids.cpp       kDrawIdCs / CSParentCount cs_5_0 (v0.10.0 phase 6b: instanced draw pixel count)
    kDrawIdPickResolve,    // draw_ids.cpp       kDrawIdCs / CSPickResolve cs_5_0 (v0.10.0 phase 9: pick + resolve + inherit, 1 group)
    kDrawIdListTwin,       // draw_ids.cpp       kDrawIdCs / CSListTwin    cs_5_0 (v0.10.0 phase 9: parent list + twin + attach, 1 group)
    kDrawIdPickPar,        // draw_ids.cpp       kDrawIdCs / CSPickPar     cs_5_0 (v0.10.0 phase 18: consensus pick, statistic spread)
    kDrawIdResInh,         // draw_ids.cpp       kDrawIdCs / CSResolveInherit cs_5_0 (v0.10.0 phase 18: resolve + inherit, wide)
    kDrawIdParentCollect,  // draw_ids.cpp       kDrawIdCs / CSParentCollect cs_5_0 (v0.10.0 phase 18: compact vote, march items)
    kDrawIdParentMarch,    // draw_ids.cpp       kDrawIdCs / CSParentMarch cs_5_0 (v0.10.0 phase 18: one thread per item + direction)
    kFwdInitVs,            // draw_ids.cpp       kFwdInitShader / VSMain   vs_5_0 (v0.10.0 phase 10: forward-depth occlusion init)
    kFwdInitPs0,           // draw_ids.cpp       kFwdInitShader / PSInit0  ps_5_0 (full resolution)
    kFwdInitPs1,           // draw_ids.cpp       kFwdInitShader / PSInit1  ps_5_0 (1/2 resolution: min of 2x2)
    kMenuVs,               // menu.cpp           kMenuVs                 vs_5_0 (v0.10.0 tuning menu panel quad)
    kMenuPs,               // menu.cpp           kMenuPs                 ps_5_0
    kCount,
    kCoreLast = kPvEdgeFill  // v0.10.0 phase 17: ids 0..kCoreLast = the core group (compiled first, CoreDone)
};

// Registers one source (before Start; later calls are ignored). `src` / `name` / `entry` / `profile` must stay
// valid for the process life (string literals / static arrays). `len` excludes the terminating 0.
void Add(Id id, const char* src, size_t len, const char* name, const char* entry, const char* profile);
// Spawns the one-time compile worker (idempotent). Not under the loader lock. If the thread cannot be created the
// shaders are compiled right here (the caller is the setup thread, not the render thread).
void Start();
// True once every registered shader has been attempted (success or failure). Acquire: the blobs are visible.
bool Done();
// v0.10.0 phase 17: true once the core group (ids 0..kCoreLast) has been attempted. Acquire: their blobs are visible.
bool CoreDone();
// v0.10.0 phase 17: true once `id` has been attempted (success or failure), or Done().
bool Ready(Id id);
// Bytecode of `id` once Ready(id) (phase 16: once Done()); nullptr (size 0) while not ready, when not registered or when
// the compile failed.
const void* Code(Id id, size_t* size);
// D3DCompile error text of a failed shader ("" when it compiled / is not ready yet).
const char* Error(Id id);
// Debug name given to Add ("?" when not registered).
const char* Name(Id id);

}  // namespace ShaderCache

#endif // WITH_DLAA
