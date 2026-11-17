/**
 * @file
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test llc_randomized_load
 * @parblock
 * Stress LLC with persistent randomized loads (633 #31 lineage, user
 * spec 2026-11-02).
 *
 * Two roles per thread, both over per-thread windows sized to the
 * PROBED per-cluster L3 slice (device_info; sysfs-verified 24 MB on
 * Kunpeng 920, where L3 is distributed 24 MB per 24-core cluster) —
 * a capacity-boundary working set for the spec's "persistent
 * randomized loads":
 *   - data[]: random-index LOADS only (memset_random-filled once) —
 *     the pure randomized-load pressure the 633 #31 description asks
 *     for;
 *   - check[]: the store->load verification path — the loaded value
 *     is stored at the same random index and read back THROUGH INLINE
 *     ASM LDR, then bit-compared.
 *
 * WHY THE ASM LOAD (critical, measured reasoning): the user's draft
 * read back with a plain C++ expression (check[idx] = v; loaded =
 * check[idx];) — GCC -O2 forwards the store to the load through the
 * register file (dead-store elimination) and the "verification"
 * compiles to a constant true.  The volatile asm ldr cannot be
 * forwarded, so the read really traverses the store-buffer
 * forwarding path of the LSU — a genuine SDC detection surface (the
 * movbe/locv family's surface).
 *
 * Data is bit-pattern random (memset_random), wider than the draft's
 * uniform-random floats (NaN/Inf/denormal lanes included; comparison
 * is bitwise memcmp so nothing is canonicalised away).
 *
 * Per-thread buffers: no cross-thread races by construction.
 * Adaptations vs the draft (same set as the _2 pair): framework RNG,
 * no per-fail fprintf (report_fail_msg carries the detail),
 * .fracture_loop_count = -1, _cleanup naming.
 * @endparblock
 */

#include "sandstone.h"
#include "sandstone_p.h"   /* device_count/device_info: L3 size probe */
#include <cstdint>
#include <cstring>
#include <vector>

/* Per-cluster L3 size in bytes (same probe as memcpy_l3_cache_size):
 * 1. framework device_info[].cache[2] — topology.cpp fills it from
 *    sysfs at startup (on this host: 24 MB per 24-core cluster,
 *    matching the sysfs shared_cpu_list)
 * 2. sysconf(_SC_LEVEL3_CACHE_SIZE) (returns 0 on this openEuler)
 * 3. 8 MB fallback
 * MUST be called from test_init (topology filled by then); a static/
 * global probe runs before main and silently falls back (memcpy H8'). */
static size_t llrl_l3_size(void) {
    int ndev = device_count();
    for (int i = 0; i < ndev; ++i) {
        int sz = device_info[i].cache[2].cache_data;   /* cache[2] = L3 */
        if (sz > 0)
            return static_cast<size_t>(sz);
    }
    long size = sysconf(_SC_LEVEL3_CACHE_SIZE);
    if (size > 0) return static_cast<size_t>(size);
    return 8 * 1024 * 1024;
}

/* Per-thread window = the probed slice: random reuse distances straddle
 * cache capacity, so the replacement policy sees genuine traffic both
 * ways.  The old 32 MiB constant was an x86 monolithic-LLC guess; on
 * the 920's distributed L3 it oversubscribed the per-thread effective
 * share (24 MB / 24 siblings = 1 MB) by ~32x, degenerating into a DRAM
 * streaming test.  Full-cluster concurrent runs still oversubscribe in
 * aggregate (N x slice vs one slice); the L3-boundary regime is the
 * single-test exclusive run. */

/* anti-forwarding load: really goes to the LSU, not the register file */
static inline uint32_t llrl_ldr(const uint32_t *addr) {
    uint32_t v;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static int llc_randomized_load_init(struct test *test) {
    /* probe HERE — topology is filled by now (see llrl_l3_size) */
    auto *block = new size_t(llrl_l3_size());
    if (!block)
        return EXIT_FAILURE;
    test->data = block;
    return EXIT_SUCCESS;
}

static int llc_randomized_load_cleanup(struct test *test) {
    delete static_cast<size_t *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int llc_randomized_load_run(struct test *test, int cpu) {
    (void)cpu;

    const size_t buffer_bytes = *static_cast<size_t *>(test->data);
    const size_t num_words = buffer_bytes / sizeof(uint32_t);

    std::vector<uint32_t> data(num_words);    /* random-index load source  */
    std::vector<uint32_t> check(num_words);   /* store->load verify window */

    /* bit-pattern random fill (NaN/Inf/denormal lanes included); the
     * comparison is bitwise, so no float semantics can hide a flip */
    memset_random(data.data(), buffer_bytes);
    memset(check.data(), 0, buffer_bytes);

    do {
        size_t idx = random64() % num_words;

        /* randomized LLC load — the pressure the spec asks for */
        uint32_t original = data[idx];

        /* store -> REAL load-back through the LSU forwarding path */
        check[idx] = original;
        uint32_t loaded = llrl_ldr(&check[idx]);

        if (memcmp(&original, &loaded, sizeof(uint32_t)) != 0) {
            report_fail_msg("llc_randomized_load: store->load "
                            "consistency failure at idx %zu: stored "
                            "0x%08x loaded 0x%08x (LSU forwarding path)",
                            idx, original, loaded);
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(llc_randomized_load, "Stress LLC with persistent randomized loads over per-thread windows sized to the probed per-cluster L3 (capacity-boundary working set); store->asm-ldr load-back bit verification on the LSU forwarding path (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = llc_randomized_load_init,
    .test_run = llc_randomized_load_run,
    .test_cleanup = llc_randomized_load_cleanup,
    .fracture_loop_count = -1,   /* never fracture: fast semantic rounds */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
