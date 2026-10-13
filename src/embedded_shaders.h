// EmbeddedShaders (issue #15) -- the prebuilt DXBC bytecode of the HLSL blocks, generated at build time by
// cmake/validate_shaders.cmake into <build>/shaders/embedded_shaders.cpp (only when SHADER_VALIDATION=ON; the build then
// defines DLAA_EMBEDDED_SHADERS=1). Under Proton, Wine's builtin d3dcompiler_47 (vkd3d HLSL) rejects most of our shaders,
// so ShaderCache hands these bytes straight to Create*Shader and D3DCompile is only the fallback for a shader that has no
// blob (or whose source no longer matches the one the blob was built from).
// A blob is used only when ALL of these match the ShaderCache::Add registration: source length + FNV-1a 64 of the source
// bytes, entry point, profile and compile flags (fxc /O3 = D3DCOMPILE_OPTIMIZATION_LEVEL3; the runtime passes no defines
// and no include handler, and the source name passed to D3DCompile does not change non-debug bytecode).
#pragma once
#ifdef WITH_DLAA

#include <cstddef>
#include <cstdint>

namespace EmbeddedShaders {

// FNV-1a 64 over bytes (the same function as shader_cache.cpp's disk-cache key hash), usable in a constant expression.
constexpr uint64_t Fnv64(const char* p, size_t n, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < n; ++i) { h ^= (uint64_t)(unsigned char)p[i]; h *= 1099511628211ull; }
    return h;
}

struct SrcId {
    uint64_t len;       // bytes, without the terminating 0 (= sizeof(kBlock) - 1, what ShaderCache::Add gets)
    uint64_t hash;      // Fnv64 of those bytes
};
// Source identity of a string literal (the generated file passes each block's raw string here).
template <size_t N>
constexpr SrcId IdOf(const char (&s)[N]) { return SrcId{ (uint64_t)(N - 1), Fnv64(s, N - 1) }; }

struct Blob {
    const char*          block;     // source block name, e.g. "kDrawIdCs" (log only)
    const char*          entry;     // entry point, e.g. "CSGather"
    const char*          profile;   // e.g. "cs_5_0"
    uint32_t             flags;     // D3DCOMPILE_* flags the bytecode was compiled with
    uint64_t             srcLen;
    uint64_t             srcHash;
    const unsigned char* code;      // DXBC
    uint32_t             size;
};

#ifdef DLAA_EMBEDDED_SHADERS
extern const Blob   kBlobs[];
extern const size_t kBlobCount;
#endif

}  // namespace EmbeddedShaders

#endif // WITH_DLAA
