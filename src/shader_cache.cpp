// ShaderCache implementation (v0.7.8). See shader_cache.h.
#ifdef WITH_DLAA

#include "shader_cache.h"
#include "log.h"
#include <d3dcompiler.h>
#include <atomic>
#include <string>

namespace {

struct Entry {
    const char* src = nullptr;
    size_t      len = 0;
    const char* name = nullptr;
    const char* entry = nullptr;
    const char* profile = nullptr;
    ID3DBlob*   code = nullptr;          // held for the process life (never released)
    std::string err;                     // D3DCompile error text (failed shaders only)
    double      ms = 0.0;                // compile wall time
};

Entry             g_e[ShaderCache::kCount];
std::atomic<bool> g_started{false};
std::atomic<bool> g_done{false};         // release-stored by the worker after the last compile

inline double MsSince(const LARGE_INTEGER& t0, const LARGE_INTEGER& f) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return f.QuadPart > 0 ? (double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart : 0.0;
}

// Compiles every registered entry in registration (= enum) order, then publishes Done(). One summary line with the
// per-shader times; a failed shader is logged with its error text here and again by its module when it asks.
DWORD WINAPI CompileAll(LPVOID) {
    LARGE_INTEGER f, t0;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int ok = 0, failed = 0;
    char times[512];
    size_t tl = 0;
    times[0] = 0;
    for (int i = 0; i < ShaderCache::kCount; ++i) {
        Entry& e = g_e[i];
        if (!e.src) continue;
        LARGE_INTEGER s;
        QueryPerformanceCounter(&s);
        ID3DBlob* cso = nullptr;
        ID3DBlob* err = nullptr;
        const HRESULT hr = D3DCompile(e.src, e.len, e.name, nullptr, nullptr, e.entry, e.profile,
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cso, &err);
        e.ms = MsSince(s, f);
        if (SUCCEEDED(hr) && cso) {
            e.code = cso;
            ++ok;
        } else {
            if (cso) cso->Release();
            char buf[64];
            snprintf(buf, sizeof(buf), "hr=0x%lx: ", (unsigned long)hr);
            e.err = buf;
            e.err += err ? (const char*)err->GetBufferPointer() : "(no log)";
            ++failed;
            Log("shader warm-up: %s (%s / %s) D3DCompile FAILED %s", e.name, e.profile, e.entry, e.err.c_str());
        }
        if (err) err->Release();
        if (tl + 40 < sizeof(times)) {
            const int w = snprintf(times + tl, sizeof(times) - tl, " %s %.0f", e.name, e.ms);
            if (w > 0) tl += (size_t)w;
        }
    }
    const double total = MsSince(t0, f);
    g_done.store(true, std::memory_order_release);
    Log("shader warm-up: %d shader(s) compiled once for the process in %.1f ms on a worker thread (%d failed) -- ms per "
        "shader:%s; every DLAA unit creates its shaders from these blobs (no D3DCompile on the render thread)",
        ok, total, failed, times);
    return 0;
}

}  // namespace

namespace ShaderCache {

void Add(Id id, const char* src, size_t len, const char* name, const char* entry, const char* profile) {
    if (id < 0 || id >= kCount || g_started.load(std::memory_order_relaxed)) return;
    Entry& e = g_e[id];
    e.src = src; e.len = len; e.name = name; e.entry = entry; e.profile = profile;
}

void Start() {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    HANDLE h = CreateThread(nullptr, 0, CompileAll, nullptr, 0, nullptr);
    if (h) { CloseHandle(h); return; }                   // detached
    Log("shader warm-up: worker thread creation failed (err %lu) -- compiling on the setup thread", GetLastError());
    CompileAll(nullptr);
}

bool Done() { return g_done.load(std::memory_order_acquire); }

const void* Code(Id id, size_t* size) {
    if (size) *size = 0;
    if (id < 0 || id >= kCount || !Done() || !g_e[id].code) return nullptr;
    if (size) *size = g_e[id].code->GetBufferSize();
    return g_e[id].code->GetBufferPointer();
}

const char* Error(Id id) {
    if (id < 0 || id >= kCount || !Done()) return "";
    if (!g_e[id].src) return "not registered";
    return g_e[id].err.c_str();
}

const char* Name(Id id) {
    if (id < 0 || id >= kCount || !g_e[id].name) return "?";
    return g_e[id].name;
}

}  // namespace ShaderCache

#endif // WITH_DLAA
