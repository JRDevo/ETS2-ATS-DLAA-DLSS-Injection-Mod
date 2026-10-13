// Phase 54 unit test of src/cpu_tuning_pick.h (CPU tuning: which logical processors are the P-cores, when the soft P-core
// preference is skipped, and the menu row's one-state summary). No Windows calls, no game, made-up CPUs.
// Build: tests\build_cpu_tuning_test.bat. Exit code 0 = every check passed.
#include "../src/cpu_tuning_pick.h"
#include <cstdio>
#include <cstdint>
#include <vector>

using namespace CpuTuningPick;

static int g_checks = 0, g_fails = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fails; printf("   FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// n logical processors of one class, ids from `first`
static void Add(std::vector<CpuSetEntry>& v, uint32_t first, int n, uint8_t cls, uint16_t group = 0) {
    for (int i = 0; i < n; ++i) v.push_back({ first + (uint32_t)i, group, cls });
}

int main() {
    // 1. Intel 12th-14th gen style: 8 P-cores with HT (16 LPs, class 1) + 8 E-cores (class 0), ids 0..23 -> P = 0..15
    {
        std::vector<CpuSetEntry> v; Add(v, 0, 16, 1); Add(v, 16, 8, 0);
        const Pick p = PickPCores(v, false);
        CHECK(p.verdict == kOk, "i9 style verdict %d", p.verdict);
        CHECK(p.pCount == 16 && (int)p.ids.size() == 16 && p.otherCount == 8 && p.classCount == 2 && p.pClass == 1,
              "i9 counts p=%d ids=%zu o=%d c=%d cls=%d", p.pCount, p.ids.size(), p.otherCount, p.classCount, p.pClass);
        bool ok = true; for (size_t i = 0; i < p.ids.size(); ++i) if (p.ids[i] != i) ok = false;
        CHECK(ok, "i9 ids are 0..15");
    }
    // 2. the affinity is already narrowed (cpu_cache_pin / launcher): topology still reported, ids dropped, verdict says why
    {
        std::vector<CpuSetEntry> v; Add(v, 0, 16, 1); Add(v, 16, 8, 0);
        const Pick p = PickPCores(v, true);
        CHECK(p.verdict == kAffinityNarrowed && p.ids.empty(), "narrowed verdict %d ids %zu", p.verdict, p.ids.size());
        CHECK(p.pCount == 16 && p.otherCount == 8, "narrowed still reports the topology");
    }
    // 3. non-hybrid (one class): nothing, whatever the affinity
    {
        std::vector<CpuSetEntry> v; Add(v, 0, 24, 0);
        CHECK(PickPCores(v, false).verdict == kOneClass, "one class (Ryzen / non-hybrid)");
        CHECK(PickPCores(v, true).verdict == kOneClass, "one class + narrowed affinity: still one class");
        CHECK(PickPCores(v, false).ids.empty(), "one class: no ids");
    }
    // 4. three classes (Meteor Lake style: P = 2, E = 1, LP-E = 0): only the highest class
    {
        std::vector<CpuSetEntry> v; Add(v, 0, 12, 2); Add(v, 12, 8, 1); Add(v, 20, 2, 0);
        const Pick p = PickPCores(v, false);
        CHECK(p.verdict == kOk && p.pCount == 12 && p.otherCount == 10 && p.classCount == 3 && p.pClass == 2,
              "3 classes: p=%d o=%d c=%d cls=%d", p.pCount, p.otherCount, p.classCount, p.pClass);
    }
    // 5. interleaved ids (P and E not contiguous): the ids are the exact ones of the highest class
    {
        std::vector<CpuSetEntry> v;
        for (uint32_t i = 0; i < 12; ++i) v.push_back({ i, 0, (uint8_t)((i % 3 == 2) ? 0 : 1) });   // ids 2,5,8,11 are E
        const Pick p = PickPCores(v, false);
        CHECK(p.verdict == kOk && p.pCount == 8 && (int)p.ids.size() == 8, "interleaved count %d", p.pCount);
        bool noE = true; for (uint32_t id : p.ids) if (id % 3 == 2) noE = false;
        CHECK(noE, "interleaved: no E-core id in the P set");
    }
    // 6. no information / other groups / single P LP
    CHECK(PickPCores({}, false).verdict == kNoSets, "empty list");
    {
        std::vector<CpuSetEntry> v; Add(v, 0, 64, 1); Add(v, 0, 8, 0, 1);
        CHECK(PickPCores(v, false).verdict == kMultiGroup, "a second processor group");
    }
    {
        std::vector<CpuSetEntry> v; Add(v, 0, 1, 1); Add(v, 1, 3, 0);
        CHECK(PickPCores(v, false).verdict == kTooFewP, "a single P logical processor");
    }
    // 7. the reason texts exist for every verdict
    for (int i = 0; i <= kTooFewP; ++i) CHECK(Why((Verdict)i) && Why((Verdict)i)[0], "Why(%d)", i);
    // 8. the menu row: summary + next state
    CHECK(Summary(0, 0, 0) == 0, "all off");
    CHECK(Summary(1, 1, 1) == 1, "all on");
    CHECK(Summary(1, 0, 0) == 2 && Summary(0, 1, 1) == 2 && Summary(1, 1, 0) == 2, "mixed");
    CHECK(NextRowState(0) == 1, "off -> on");
    CHECK(NextRowState(1) == 0, "on -> off");
    CHECK(NextRowState(2) == 0, "mixed -> off");

    printf("cpu_tuning_test: %d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
