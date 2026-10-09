// ShaderCache implementation (v0.7.8; v0.10.0 phase 17: core group first, several worker threads, DXBC disk cache).
// See shader_cache.h.
#ifdef WITH_DLAA

#include "shader_cache.h"
#include "log.h"
#include <d3dcompiler.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>
#include <thread>

namespace {

struct Entry {
    const char* src = nullptr;
    size_t      len = 0;
    const char* name = nullptr;
    const char* entry = nullptr;
    const char* profile = nullptr;
    ID3DBlob*   code = nullptr;          // held for the process life (never released)
    std::string err;                     // D3DCompile error text (failed shaders only)
    double      ms = 0.0;                // compile (or disk cache load) wall time
    bool        cached = false;          // v0.10.0 phase 17: the blob came from the disk cache
    std::atomic<bool> ready{false};      // v0.10.0 phase 17: release-stored right after THIS entry is done (ok or failed)
};

constexpr UINT kCompileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;

Entry             g_e[ShaderCache::kCount];
std::atomic<bool> g_started{false};
std::atomic<bool> g_done{false};         // release-stored by the worker that finished the last entry
std::atomic<bool> g_coreDone{false};     // v0.10.0 phase 17: release-stored once the core group (ids 0..kCoreLast) is done

// v0.10.0 phase 17 worker pool state
std::atomic<int>  g_next{0};             // next position in OrderAt to take
std::atomic<int>  g_left{0};             // entries not finished yet (all ids, registered or not)
std::atomic<int>  g_coreLeft{0};         // core entries not finished yet
std::atomic<int>  g_fromDisk{0}, g_compiled{0}, g_failed{0}, g_diskBad{0}, g_diskWriteFail{0};
int               g_threads = 1;
LARGE_INTEGER     g_freq{}, g_t0{};
std::wstring      g_dir;                 // disk cache folder incl. the trailing backslash ("" = no disk cache)

// v0.10.0 phase 17: compile ORDER. The core group (camera MVs, depth convert, RCAS, composite, MV debug, the preview blit /
// depth assembly / edge fill: everything a menu / truck-preview unit needs, ~2.1 s) first, then the tuning menu quad (2
// tiny shaders), then the per-object / per-draw world set (~18 s in series at phase 16: drawid_parent_pick1 alone ~7.4 s).
// Until phase 16 everything waited for the LAST shader of ONE worker thread (ShaderCache::Done): the ATS VR logs
// captures/dlaa_inject_ats_v0100p15_vr2.log / _p16_vr.log show the menu / garage DLAA starting ~20 s after the load for
// exactly that reason. Built once in Start (before the workers exist).
int g_order[ShaderCache::kCount];
void BuildOrder() {
    int n = 0;
    for (int i = 0; i <= ShaderCache::kCoreLast; ++i) g_order[n++] = i;
    g_order[n++] = ShaderCache::kMenuVs;
    g_order[n++] = ShaderCache::kMenuPs;
    for (int i = ShaderCache::kCoreLast + 1; i < ShaderCache::kCount; ++i)
        if (i != ShaderCache::kMenuVs && i != ShaderCache::kMenuPs) g_order[n++] = i;
}

inline double MsSince(const LARGE_INTEGER& t0) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return g_freq.QuadPart > 0 ? (double)(t.QuadPart - t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart : 0.0;
}

// ---- v0.10.0 phase 17: DXBC disk cache ---------------------------------------------------------------------------------
// <game exe folder>\dlaa_shader_cache\<name>_<key>.dxbc, key = FNV-1a 64 of (format tag, source, name, entry, profile, flags,
// D3D_COMPILER_VERSION) -- a changed shader source / entry / profile / flag gives another file name, so a stale file is never
// even opened (and is deleted when the new one is written). File = DiskHdr + the DXBC bytes; a load checks the header (magic,
// version, key, size = file size) and an FNV-1a 64 of the payload, and the payload must start with "DXBC". Anything else =
// ignored + recompiled (and rewritten). Written to a temp file and renamed into place (MOVEFILE_REPLACE_EXISTING), so a
// crash mid-write never leaves a half file under the real name. Bytecode only: the D3D runtime still validates it at
// Create*Shader like a freshly compiled blob.
struct DiskHdr {
    char     magic[4];                   // "DLSC"
    uint32_t version;                    // kDiskVersion
    uint64_t key;                        // EntryKey of the entry it was compiled from
    uint64_t payloadHash;                // Fnv64 of the DXBC bytes
    uint32_t size;                       // DXBC bytes after the header
    uint32_t reserved;
};
constexpr uint32_t kDiskVersion = 1;

uint64_t Fnv64(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
uint64_t EntryKey(const Entry& e) {
    uint64_t h = Fnv64("dlaa-shader-cache-v1", 20);
    h = Fnv64(e.src, e.len, h);
    h = Fnv64(e.name, strlen(e.name) + 1, h);
    h = Fnv64(e.entry, strlen(e.entry) + 1, h);
    h = Fnv64(e.profile, strlen(e.profile) + 1, h);
    const uint32_t extra[2] = { (uint32_t)kCompileFlags, (uint32_t)D3D_COMPILER_VERSION };
    return Fnv64(extra, sizeof(extra), h);
}
std::wstring DiskPath(const Entry& e, uint64_t key) {
    wchar_t nm[160];
    swprintf(nm, 160, L"%hs_%016llx.dxbc", e.name, (unsigned long long)key);
    return g_dir + nm;
}

// true = e.code set from the disk cache. *bad = a file with the right name existed but failed a check.
bool DiskLoad(Entry& e, uint64_t key, bool* bad) {
    *bad = false;
    if (g_dir.empty()) return false;
    const std::wstring path = DiskPath(e, key);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    LARGE_INTEGER fs{};
    if (GetFileSizeEx(h, &fs) && fs.QuadPart > (LONGLONG)sizeof(DiskHdr) && fs.QuadPart < (LONGLONG)(16u << 20)) {
        DiskHdr hd{};
        DWORD rd = 0;
        if (ReadFile(h, &hd, sizeof(hd), &rd, nullptr) && rd == sizeof(hd) && memcmp(hd.magic, "DLSC", 4) == 0 &&
            hd.version == kDiskVersion && hd.key == key && (LONGLONG)hd.size + (LONGLONG)sizeof(hd) == fs.QuadPart &&
            hd.size >= 4) {
            ID3DBlob* blob = nullptr;
            if (SUCCEEDED(D3DCreateBlob(hd.size, &blob)) && blob) {
                if (ReadFile(h, blob->GetBufferPointer(), hd.size, &rd, nullptr) && rd == hd.size &&
                    memcmp(blob->GetBufferPointer(), "DXBC", 4) == 0 &&
                    Fnv64(blob->GetBufferPointer(), hd.size) == hd.payloadHash) {
                    e.code = blob;
                    ok = true;
                } else {
                    blob->Release();
                }
            }
        }
    }
    CloseHandle(h);
    if (!ok) *bad = true;
    return ok;
}

// Removes <name>_*.dxbc files other than `keep` (stale versions of this shader), then writes the blob atomically.
void DiskStore(const Entry& e, uint64_t key) {
    if (g_dir.empty() || !e.code) return;
    const std::wstring path = DiskPath(e, key);
    {
        wchar_t pat[160];
        swprintf(pat, 160, L"%hs_*.dxbc", e.name);
        WIN32_FIND_DATAW fd{};
        HANDLE fh = FindFirstFileW((g_dir + pat).c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring other = g_dir + fd.cFileName;
                // (a name like "drawid_pick_*" also matches "drawid_pick_resolve_<key>": only delete exact <name>_<16 hex>.dxbc)
                const size_t nl = strlen(e.name);
                bool exact = wcslen(fd.cFileName) == nl + 1 + 16 + 5;
                for (size_t c = nl + 1; exact && c < nl + 17; ++c) exact = iswxdigit(fd.cFileName[c]) != 0;
                if (exact && other != path) DeleteFileW(other.c_str());
            } while (FindNextFileW(fh, &fd));
            FindClose(fh);
        }
    }
    wchar_t tmpSuffix[32];
    swprintf(tmpSuffix, 32, L".tmp%lu", GetCurrentThreadId());
    const std::wstring tmp = path + tmpSuffix;
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { g_diskWriteFail.fetch_add(1); return; }
    DiskHdr hd{};
    memcpy(hd.magic, "DLSC", 4);
    hd.version = kDiskVersion;
    hd.key = key;
    hd.size = (uint32_t)e.code->GetBufferSize();
    hd.payloadHash = Fnv64(e.code->GetBufferPointer(), hd.size);
    DWORD wr = 0, wr2 = 0;
    const bool ok = WriteFile(h, &hd, sizeof(hd), &wr, nullptr) && wr == sizeof(hd) &&
                    WriteFile(h, e.code->GetBufferPointer(), hd.size, &wr2, nullptr) && wr2 == hd.size;
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        g_diskWriteFail.fetch_add(1);
    }
}

void LogSummary() {
    char times[1600];
    size_t tl = 0;
    times[0] = 0;
    for (int k = 0; k < ShaderCache::kCount; ++k) {
        const Entry& e = g_e[g_order[k]];
        if (!e.src || tl + 48 >= sizeof(times)) continue;
        const int w = snprintf(times + tl, sizeof(times) - tl, " %s %.0f%s", e.name, e.ms, e.cached ? "c" : "");
        if (w > 0) tl += (size_t)w;
    }
    Log("shader warm-up: %d from the disk cache, %d compiled in %.1f ms on %d thread(s) (%d failed; disk cache %s, %d bad / "
        "unreadable file(s) ignored, %d write failure(s)) -- ms per shader (c = disk cache):%s; every DLAA unit creates its "
        "shaders from these blobs (no D3DCompile on the render thread)", g_fromDisk.load(), g_compiled.load(),
        MsSince(g_t0), g_threads, g_failed.load(), g_dir.empty() ? "off (folder unavailable)" : "on", g_diskBad.load(),
        g_diskWriteFail.load(), times);
}

// One entry: disk cache, else D3DCompile (+ store). Publishes Ready(id) and the core / all completion.
void DoEntry(int id) {
    Entry& e = g_e[id];
    if (e.src) {
        LARGE_INTEGER s;
        QueryPerformanceCounter(&s);
        const uint64_t key = EntryKey(e);
        bool bad = false;
        if (DiskLoad(e, key, &bad)) {
            e.cached = true;
            g_fromDisk.fetch_add(1);
        } else {
            if (bad) g_diskBad.fetch_add(1);
            ID3DBlob* cso = nullptr;
            ID3DBlob* err = nullptr;
            const HRESULT hr = D3DCompile(e.src, e.len, e.name, nullptr, nullptr, e.entry, e.profile, kCompileFlags, 0,
                                          &cso, &err);
            if (SUCCEEDED(hr) && cso) {
                e.code = cso;
                g_compiled.fetch_add(1);
                DiskStore(e, key);
            } else {
                if (cso) cso->Release();
                char buf[64];
                snprintf(buf, sizeof(buf), "hr=0x%lx: ", (unsigned long)hr);
                e.err = buf;
                e.err += err ? (const char*)err->GetBufferPointer() : "(no log)";
                g_failed.fetch_add(1);
                Log("shader warm-up: %s (%s / %s) D3DCompile FAILED %s", e.name, e.profile, e.entry, e.err.c_str());
            }
            if (err) err->Release();
        }
        e.ms = MsSince(s);
    }
    e.ready.store(true, std::memory_order_release);      // this blob (or its error) is visible now
    if (id <= ShaderCache::kCoreLast && g_coreLeft.fetch_sub(1) == 1) {
        g_coreDone.store(true, std::memory_order_release);
        Log("shader warm-up: core set (ids 0..%d: camera MVs, depth, RCAS, composite, preview blit / depth assembly / edge "
            "fill) ready after %.1f ms (%d from the disk cache so far) -- menu / truck-preview DLAA units can start now; the "
            "tuning menu quad and the per-object / per-draw world set keep going (v0.10.0 phase 17)",
            (int)ShaderCache::kCoreLast, MsSince(g_t0), g_fromDisk.load());
    }
    if (g_left.fetch_sub(1) == 1) {
        g_coreDone.store(true, std::memory_order_release);
        g_done.store(true, std::memory_order_release);
        LogSummary();
    }
}

// Worker: takes the next entry in priority order until none is left.
DWORD WINAPI Worker(LPVOID) {
    for (;;) {
        const int k = g_next.fetch_add(1);
        if (k >= ShaderCache::kCount) return 0;
        DoEntry(g_order[k]);
    }
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
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    BuildOrder();
    g_left.store(kCount);
    g_coreLeft.store(kCoreLast + 1);
    // v0.10.0 phase 17: disk cache folder next to the game exe (= next to dinput8.dll and dlaa_inject.log)
    {
        wchar_t exe[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        wchar_t* slash = (n > 0 && n < MAX_PATH) ? wcsrchr(exe, L'\\') : nullptr;
        if (slash) {
            slash[1] = 0;
            std::wstring dir = std::wstring(exe) + L"dlaa_shader_cache\\";
            if (CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) g_dir = dir;
            else Log("shader warm-up: disk cache folder %ls not available (err %lu) -- every start compiles", dir.c_str(),
                     GetLastError());
        }
    }
    // v0.10.0 phase 17: several workers (hardware threads / 2, 2..8) take the entries in priority order
    unsigned hc = std::thread::hardware_concurrency();
    int t = (int)(hc / 2);
    if (t < 2) t = 2;
    if (t > 8) t = 8;
    int made = 0;
    for (int i = 0; i < t; ++i) {
        HANDLE h = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (!h) continue;
        if (i > 0) SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);   // the extra workers yield to the game's boot
        CloseHandle(h);                              // detached
        ++made;
    }
    g_threads = made > 0 ? made : 1;
    Log("shader warm-up: %d shader slot(s), %d worker thread(s) (%u hardware threads), core group first, disk cache %ls",
        (int)kCount, g_threads, hc, g_dir.empty() ? L"off" : g_dir.c_str());
    if (made > 0) return;
    Log("shader warm-up: worker thread creation failed (err %lu) -- compiling on the setup thread", GetLastError());
    Worker(nullptr);
}

bool Done() { return g_done.load(std::memory_order_acquire); }

bool CoreDone() { return g_coreDone.load(std::memory_order_acquire); }

bool Ready(Id id) {
    if (id < 0 || id >= kCount) return false;
    return Done() || g_e[id].ready.load(std::memory_order_acquire);
}

const void* Code(Id id, size_t* size) {
    if (size) *size = 0;
    if (id < 0 || id >= kCount || !Ready(id) || !g_e[id].code) return nullptr;
    if (size) *size = g_e[id].code->GetBufferSize();
    return g_e[id].code->GetBufferPointer();
}

const char* Error(Id id) {
    if (id < 0 || id >= kCount || !Ready(id)) return "";
    if (!g_e[id].src) return "not registered";
    return g_e[id].err.c_str();
}

const char* Name(Id id) {
    if (id < 0 || id >= kCount || !g_e[id].name) return "?";
    return g_e[id].name;
}

}  // namespace ShaderCache

#endif // WITH_DLAA
