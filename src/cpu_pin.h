// phase 32: keep the game on the 3D V-Cache cores (dlaa.ini cpu_cache_pin, default 0 = off; 1 = auto).
//
// AMD X3D dual-CCD parts (7950X3D / 9950X3D ...) have the big L3 (96 MB) on one CCD only; the other CCD has 32 MB and higher clocks.
// A game that likes cache wants all its threads on the big-L3 CCD; Windows' scheduler guesses that through the AMD 3D V-Cache driver
// and often gets it wrong. This pins the PROCESS affinity to the logical processors of the biggest L3 -- only when the CPU really
// has L3 caches of DIFFERENT sizes (GetLogicalProcessorInformationEx(RelationCache), no driver / registry). Nothing else is touched:
// single-CCD X3D, non-X3D, Intel (one shared L3), >1 processor group, an affinity the user / launcher already narrowed to a subset
// -- all do nothing and say why in the log. Called ONCE from StartInjection (DllMain time: kernel32 only, no loads, no locks).
#pragma once
#include <windows.h>
#include <vector>
#include <cstdint>
#include "log.h"

namespace CpuPin {

inline int PopCount64(unsigned long long m) {
    int n = 0;
    while (m) { m &= m - 1; ++n; }
    return n;
}

// Pure decision part (also used by the scratch check): the L3 entries -> the mask to keep, or 0 + a reason.
struct L3Entry { DWORD sizeBytes; unsigned long long mask; };

inline unsigned long long PickMask(const std::vector<L3Entry>& l3, const char** why) {
    if (l3.size() < 2) { *why = "fewer than 2 L3 caches (one shared L3)"; return 0; }
    DWORD maxSize = 0, minSize = 0xFFFFFFFFu;
    for (const L3Entry& e : l3) { if (e.sizeBytes > maxSize) maxSize = e.sizeBytes; if (e.sizeBytes < minSize) minSize = e.sizeBytes; }
    if (maxSize == minSize) { *why = "all L3 caches have the same size (not a mixed V-Cache part)"; return 0; }
    unsigned long long m = 0;
    for (const L3Entry& e : l3) if (e.sizeBytes == maxSize) m |= e.mask;       // two equal largest: union
    return m;
}

inline void Apply(int enabled) {
    if (!enabled) { Log("cpu_cache_pin (phase 32): off (cpu_cache_pin=0) -- process affinity untouched"); return; }

    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) {
        LogErr("cpu_cache_pin (phase 32): cache topology not readable (error %lu) -- nothing done", GetLastError());
        return;
    }
    std::vector<unsigned char> buf(len);
    if (!GetLogicalProcessorInformationEx(RelationCache, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len)) {
        LogErr("cpu_cache_pin (phase 32): cache topology not readable (error %lu) -- nothing done", GetLastError());
        return;
    }
    if (GetActiveProcessorGroupCount() > 1) {
        Log("cpu_cache_pin (phase 32): %u processor groups -- nothing done (only a single group is handled)",
            (unsigned)GetActiveProcessorGroupCount());
        return;
    }

    std::vector<L3Entry> l3;
    for (DWORD off = 0; off < len;) {
        const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* e = (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*)(buf.data() + off);
        if (e->Size == 0) break;
        if (e->Relationship == RelationCache && e->Cache.Level == 3 &&
            (e->Cache.Type == CacheUnified || e->Cache.Type == CacheData)) {
            unsigned long long m = 0;
            if (e->Cache.GroupCount > 1) {
                for (WORD g = 0; g < e->Cache.GroupCount; ++g)
                    if (e->Cache.GroupMasks[g].Group == 0) m |= (unsigned long long)e->Cache.GroupMasks[g].Mask;
            } else {
                if (e->Cache.GroupMask.Group == 0) m = (unsigned long long)e->Cache.GroupMask.Mask;
            }
            if (m) l3.push_back({ e->Cache.CacheSize, m });
        }
        off += e->Size;
    }

    for (const L3Entry& e : l3)
        Log("cpu_cache_pin (phase 32): L3 %lu MB mask 0x%llx (%d logical processors)", (unsigned long)(e.sizeBytes >> 20), e.mask,
            PopCount64(e.mask));

    const char* why = "";
    const unsigned long long want = PickMask(l3, &why);
    if (!want) { Log("cpu_cache_pin (phase 32): nothing done -- %s", why); return; }

    DWORD_PTR procMask = 0, sysMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask)) {
        LogErr("cpu_cache_pin (phase 32): GetProcessAffinityMask failed (error %lu) -- nothing done", GetLastError());
        return;
    }
    const int keep = PopCount64(want);
    if ((want & (unsigned long long)procMask) != want || want == (unsigned long long)procMask) {
        Log("cpu_cache_pin (phase 32): nothing done -- the V-Cache mask 0x%llx is not a strict subset of the process affinity 0x%llx",
            want, (unsigned long long)procMask);
        return;
    }
    if (keep < 4) {
        Log("cpu_cache_pin (phase 32): nothing done -- the V-Cache mask 0x%llx has only %d logical processors (< 4)", want, keep);
        return;
    }
    if (SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)want))
        Log("cpu_cache_pin (phase 32): process affinity 0x%llx -> 0x%llx (the biggest L3 only: %d logical processors kept)",
            (unsigned long long)procMask, want, keep);
    else
        LogErr("cpu_cache_pin (phase 32): SetProcessAffinityMask(0x%llx) failed (error %lu) -- affinity unchanged", want, GetLastError());
}

} // namespace CpuPin
