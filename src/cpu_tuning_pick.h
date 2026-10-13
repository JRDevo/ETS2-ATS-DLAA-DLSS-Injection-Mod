// phase 54: the pure decision part of the CPU tuning (cpu_tuning.h): which logical processors are the P-cores of an Intel
// hybrid CPU, and whether the soft P-core preference may be used at all. No Windows calls, so tests\cpu_tuning_test.cpp can
// drive it with made-up CPUs.
#pragma once
#include <vector>
#include <cstdint>

namespace CpuTuningPick {

// One entry of GetSystemCpuSetInformation (SYSTEM_CPU_SET_INFORMATION.CpuSet): id, group, EfficiencyClass.
// Windows: a HIGHER efficiency class = a more performant core (P-core); the lowest = the LP-E / E-cores.
struct CpuSetEntry { uint32_t id; uint16_t group; uint8_t effClass; };

enum Verdict {
    kOk = 0,            // use ids
    kNoSets,            // no CPU set information
    kMultiGroup,        // processors in a group other than 0 (>64 logical processors): not handled
    kOneClass,          // all logical processors have the same efficiency class: not a hybrid CPU
    kAffinityNarrowed,  // the process affinity is already narrower than the system's (cpu_cache_pin or a launcher): that wins
    kTooFewP,           // fewer than 2 P-core logical processors: not worth restricting
};

struct Pick {
    Verdict               verdict = kNoSets;
    std::vector<uint32_t> ids;              // the CPU set ids of the highest efficiency class (kOk)
    int                   pClass = 0;       // the highest class
    int                   pCount = 0;       // logical processors in it
    int                   otherCount = 0;   // logical processors in the lower classes
    int                   classCount = 0;   // distinct classes
};

inline const char* Why(Verdict v) {
    switch (v) {
    case kOk:               return "ok";
    case kNoSets:           return "no CPU set information";
    case kMultiGroup:       return "more than one processor group (only group 0 is handled)";
    case kOneClass:         return "all cores have the same efficiency class (not a hybrid CPU)";
    case kAffinityNarrowed: return "the process affinity is already narrowed (cpu_cache_pin or a launcher wins; cpu sets are not touched)";
    case kTooFewP:          return "fewer than 2 P-core logical processors";
    }
    return "?";
}

// `affinityNarrowed` = GetProcessAffinityMask's process mask is not the full system mask. Order of the checks: topology first
// (so the log can always say what the CPU is), then the affinity.
inline Pick PickPCores(const std::vector<CpuSetEntry>& sets, bool affinityNarrowed) {
    Pick p;
    if (sets.empty()) { p.verdict = kNoSets; return p; }
    int hi = -1, lo = 256;
    for (const CpuSetEntry& e : sets) {
        if (e.group != 0) { p.verdict = kMultiGroup; return p; }
        if ((int)e.effClass > hi) hi = e.effClass;
        if ((int)e.effClass < lo) lo = e.effClass;
    }
    bool seen[256] = {};
    for (const CpuSetEntry& e : sets) {
        if (!seen[e.effClass]) { seen[e.effClass] = true; ++p.classCount; }
        if ((int)e.effClass == hi) { p.ids.push_back(e.id); ++p.pCount; } else ++p.otherCount;
    }
    p.pClass = hi;
    if (hi == lo) { p.verdict = kOneClass; p.ids.clear(); p.pCount = 0; p.otherCount = 0; return p; }
    if (p.pCount < 2) { p.verdict = kTooFewP; p.ids.clear(); return p; }
    if (affinityNarrowed) { p.verdict = kAffinityNarrowed; p.ids.clear(); return p; }
    p.verdict = kOk;
    return p;
}

// The menu's one row: 0 = all three off, 1 = all three on, 2 = a mix (set in dlaa.ini).
inline int Summary(int pcore, int priority, int noThrottle) {
    const int n = (pcore ? 1 : 0) + (priority ? 1 : 0) + (noThrottle ? 1 : 0);
    return n == 0 ? 0 : (n == 3 ? 1 : 2);
}

// What a Left / Right press on the row does: all off -> all on; all on or a mix -> all off.
inline int NextRowState(int summary) { return summary == 0 ? 1 : 0; }

} // namespace CpuTuningPick
