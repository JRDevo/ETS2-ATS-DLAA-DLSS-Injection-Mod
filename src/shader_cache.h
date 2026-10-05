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
    kCount
};

// Registers one source (before Start; later calls are ignored). `src` / `name` / `entry` / `profile` must stay
// valid for the process life (string literals / static arrays). `len` excludes the terminating 0.
void Add(Id id, const char* src, size_t len, const char* name, const char* entry, const char* profile);
// Spawns the one-time compile worker (idempotent). Not under the loader lock. If the thread cannot be created the
// shaders are compiled right here (the caller is the setup thread, not the render thread).
void Start();
// True once every registered shader has been attempted (success or failure). Acquire: the blobs are visible.
bool Done();
// Bytecode of `id` once Done(); nullptr (size 0) while not done, when not registered or when the compile failed.
const void* Code(Id id, size_t* size);
// D3DCompile error text of a failed shader ("" when it compiled / is not done yet).
const char* Error(Id id);
// Debug name given to Add ("?" when not registered).
const char* Name(Id id);

}  // namespace ShaderCache

#endif // WITH_DLAA
