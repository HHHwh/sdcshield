/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b cache_stress_civac
 * @parblock
 * Corrected variant of cache_stress_aggressor: evicts the just-stored data
 * cache line with a proper `dc civac` + `dsb sy` (NOT __builtin___clear_cache,
 * which on aarch64 clears the *instruction* cache via libc __clear_cache and
 * does NOT evict a data line), forcing the reload to fetch from L2/L3.
 *
 * Design intent: a single per-cpu worker (the framework already schedules one
 * worker per core — do NOT spawn std::thread inside the worker, that fights
 * the framework's core-binding model) does a plain ldr/str bswap round-trip
 * + store→load-forwarding reload, with `dc civac` evicting the stored line
 * before the reload so the reload path is L2/L3 instead of L1D. This mirrors
 * movbe_l2's correct evict path (movbe_l1d_vs_l2.cpp evict_l1d_line) but
 * uses ordinary integer ldr/str (179 SDC's path is integer store→reload,
 * NOT NEON vst1q/vld1q as in the original cache_stress_aggressor).
 *
 * Compare against cachebounce_sdc: the latter keeps input resident in L1D
 * (ping-pong pressure, reload hits L1D — expected to trigger 179 SDC); this
 * test evicts before reload (reload goes L2/L3 — expected 0-fail per movbe_l2,
 * though dc civac+dsb also stretches timing, so 0-fail cannot be cleanly
 * attributed to "bypassing L1D"; see intervention-decoupling-impossible-use-pmu).
 *
 * On miscompare the cold failure branch dumps input/golden/actual/xor with a
 * per-byte breakdown (mirrors movbe_dump). No recompute field: recompute's
 * input is itself read via L1D (same ldr path), so it has no independence to
 * rule out L1D read faults (see mrn-rmw-variant-paths-and-recompute-limit).
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

static constexpr size_t CACHE_LINE_SIZE = 64;
/* 16384 uint32_t = 64KB = exactly fills a 64KB L1D (matches movbe footprint). */
static constexpr size_t CSTRESS_BUFFER_SIZE = 1 << 14;

struct CacheStressCivacData {
    uint32_t *input;    /* shared source buffer (reload target after evict) */
    uint32_t *swapped;  /* shared store target (bswapped value written here) */
};

#if defined(__aarch64__)
/* Correct data-cache-line evict: clean+invalidate to the point of coherency,
 * then drain. This forces a subsequent reload of this line to miss L1D and
 * fetch from L2/L3. (movbe_l1d_vs_l2.cpp:131-138 identical.) */
static inline void evict_l1d_line(const void *addr) {
    __asm__ volatile(
        "dc civac, %0\n\t"
        "dsb sy\n\t"
        :: "r"(addr) : "memory");
}
#endif

static int cache_stress_civac_init(struct test *test) {
    auto *data = new CacheStressCivacData();
    if (!data) return EXIT_FAILURE;

    data->input   = static_cast<uint32_t *>(aligned_alloc_safe(CACHE_LINE_SIZE, CSTRESS_BUFFER_SIZE * sizeof(uint32_t)));
    data->swapped = static_cast<uint32_t *>(aligned_alloc_safe(CACHE_LINE_SIZE, CSTRESS_BUFFER_SIZE * sizeof(uint32_t)));

    for (size_t i = 0; i < CSTRESS_BUFFER_SIZE; ++i) {
        data->input[i] = random32();
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int cache_stress_civac_run(struct test *test, int cpu) {
    auto *data = static_cast<CacheStressCivacData *>(test->data);
    (void)cpu;

    /* Loop granularity (issues #369/#360 class): the body is one full
     * 16384-element sweep whose every element pays a `dc civac` + `dsb sy`
     * line-evict + full drain before the reload — ~3.7 ms per body
     * (measured: 30.2 s quick -n4 on a 128-core Kunpeng 920, 30.6 s on the
     * CI 4-vCPU Neoverse-N2 runner). The N = 1<<13 inherited from the
     * movbe_dump.cpp ancestry (plain ldr/rev/str body, no per-element
     * barrier, checkpoint < 270 ms) therefore slid the first deadline check
     * to ~30 s; --quick stops at that first check, so 4 worker threads
     * burned ~121 s of CPU per forked test child — exactly on the quick CI
     * step's `ulimit -St 120`, zero margin, and runner contention turned it
     * into SIGXCPU flakes (reproduced locally: crash code 24 under
     * `ulimit -St 120`, pass under 600). N = 4 lands the checkpoint at
     * ~15 ms, inside the framework's TargetLoopDuration = 10 ms band
     * (sandstone_p.h). The hot 16384-element loop is untouched: the check
     * runs only between complete passes and adds no conditional inside the
     * hot loop; in uncapped -t runs the per-second stimulus is unchanged —
     * only the -t overshoot shrinks from ~30 s to one checkpoint. */
    TEST_LOOP(test, 4) {
        for (size_t i = 0; i < CSTRESS_BUFFER_SIZE; ++i) {
            /* Hot path: ldr input → bswap → str swapped → evict swapped's
             * line → bswap back → ldr input (reload, now L2/L3 after evict
             * of input? NO: we evict swapped, then reload input). To make
             * the *reload* miss L1D we evict input[i]'s line before the
             * compare-reload (mirrors movbe_l2 which evicts input[i]). */
            uint32_t val = data->input[i];
            val = __builtin_bswap32(val);

            data->swapped[i] = val;

            val = __builtin_bswap32(val);

#if defined(__aarch64__)
            /* Evict input[i]'s line so the reload below fetches from L2/L3
             * instead of hitting L1D. (movbe_l2 path.) */
            evict_l1d_line(&data->input[i]);
#endif
            /* Reload input[i] (goes L2/L3 after evict) and compare against
             * the round-tripped val. Golden == input[i] (a correct double
             * bswap restores the original). */
            if (val != data->input[i]) {  /* cold branch only */
                uint32_t inputv = data->input[i];
                uint32_t golden = data->input[i];
                uint32_t actual = val;
                uint32_t xorv = golden ^ actual;
                unsigned char *ib = (unsigned char *)&inputv;
                unsigned char *gb = (unsigned char *)&golden;
                unsigned char *ab = (unsigned char *)&actual;
                unsigned char *xb = (unsigned char *)&xorv;

                log_error("cache_stress_civac: round-trip failed at index %u "
                          "(cpu=%d): input=0x%08X golden=0x%08X actual=0x%08X "
                          "xor=0x%08X | bytes[b3 b2 b1 b0] input=%02X%02X%02X%02X "
                          "golden=%02X%02X%02X%02X actual=%02X%02X%02X%02X "
                          "xor=%02X%02X%02X%02X",
                          (unsigned)i, cpu, inputv, golden, actual, xorv,
                          ib[3], ib[2], ib[1], ib[0],
                          gb[3], gb[2], gb[1], gb[0],
                          ab[3], ab[2], ab[1], ab[0],
                          xb[3], xb[2], xb[1], xb[0]);

            }
        }
    }

    return EXIT_SUCCESS;
}

static int cache_stress_civac_cleanup(struct test *test) {
    auto *data = static_cast<CacheStressCivacData *>(test->data);
    if (data) {
        free(data->input);
        free(data->swapped);
        delete data;
        test->data = nullptr;
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(cache_stress_civac, "dc civac evict + str→ldr reload SDC probe (L2/L3 reload path; correct evict, not __clear_cache; ordinary ldr/str)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = cache_stress_civac_init,
    .test_run = cache_stress_civac_run,
    .test_cleanup = cache_stress_civac_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
