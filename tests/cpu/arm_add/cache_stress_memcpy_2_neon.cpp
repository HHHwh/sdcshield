/*  NEON variant of tests/cpu/cache/cache_stress_memcpy_2_neon.cpp — same-day NEON pass:
 *  load-point memcpy() replaced with the explicit vld1q/vst1q stream
 *  (arm_add_neon_copy.h); flush/verify semantics identical.  Original
  *  untouched; identifiers carry the _neon suffix (ODR).
 */
/*  cache_stress_memcpy_2_neon — pure memory copy control, no flush
 *  (633 #27 lineage, user spec 2026-11-02; "REP MOVS" in the lineage
 *  description is the x86 original's wording — on ARM64 the copy is
 *  the libc memcpy / its internal LDP/STP loop, kept as-is because the
 *  instruction under study in the #26/#27 pair is the FLUSH, not the
 *  copy).
 *
 *  The CONTROL ARM of cache_stress_clflush_2 (633 #26): byte-for-byte
 *  the same random-refill + copy + verify workload with ZERO flush
 *  interference — any failure here that does not reproduce under
 *  cache_stress_clflush_2 localises the fault to the copy path itself;
 *  failures appearing ONLY under clflush_2 are the eviction-pressure
 *  signature.
 *
 *  Per iteration: src <- fresh random64 qwords (independent golden,
 *  never written after the fill), dst <- random64 pre-corruption (a
 *  lost copy cannot hide behind last round's bytes), copy, memcmp,
 *  store->reload consistency pass.
 *
 *  Adaptations to repo conventions (vs the user's draft, same set as
 *  cache_stress_clflush_2): framework random64() instead of
 *  std::mt19937/random_device (reproducible seeds); per-iteration
 *  fprintf log block removed (hot path carries no I/O); random-double
 *  fill widened to random64; .fracture_loop_count = -1 (microsecond
 *  rounds); _cleanup naming.
 */
#include "sandstone.h"
#include "arm_add_neon_copy.h"
using arm_add_neon::neon_copy;
#include <cstdint>
#include <cstring>
#include <vector>

static constexpr size_t CS2M_BLOCK_SIZE = 4096;   /* 4 KB */

static int cache_stress_memcpy_2_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int cache_stress_memcpy_2_run(struct test *test, int cpu) {
    (void)cpu;

    /* per-thread buffers */
    std::vector<uint8_t> src(CS2M_BLOCK_SIZE);
    std::vector<uint8_t> dst(CS2M_BLOCK_SIZE);
    std::vector<uint8_t> store_buf(CS2M_BLOCK_SIZE);

    uint64_t local_iterations = 0;

    do {
        /* 1. fill src with random qwords — the independent golden */
        for (size_t i = 0; i + sizeof(uint64_t) <= CS2M_BLOCK_SIZE;
             i += sizeof(uint64_t)) {
            uint64_t val = random64();
            memcpy(&src[i], &val, sizeof(val));
        }

        /* 2. pre-corrupt dst — lost stores cannot hide behind last
         * round's correct bytes */
        for (size_t i = 0; i + sizeof(uint64_t) <= CS2M_BLOCK_SIZE;
             i += sizeof(uint64_t)) {
            uint64_t val = random64();
            memcpy(&dst[i], &val, sizeof(val));
        }

        /* 3. pure copy — no flush interference anywhere */
        neon_copy(dst.data(), src.data(), CS2M_BLOCK_SIZE);

        /* 4. verify: src is the independent golden */
        bool data_ok = (memcmp(dst.data(), src.data(), CS2M_BLOCK_SIZE) == 0);

        /* 5. store->reload consistency pass */
        neon_copy(store_buf.data(), dst.data(), CS2M_BLOCK_SIZE);
        bool consistent =
            (memcmp(store_buf.data(), dst.data(), CS2M_BLOCK_SIZE) == 0);

        if (!data_ok || !consistent) {
            report_fail_msg("cache_stress_memcpy_2_neon: data mismatch or "
                            "consistency failure at iteration %lu "
                            "(data_ok=%d consistent=%d, src[0..7]=%02X "
                            "%02X %02X %02X %02X %02X %02X %02X, "
                            "dst[0..7]=%02X %02X %02X %02X %02X %02X "
                            "%02X %02X)",
                            (unsigned long)local_iterations, (int)data_ok,
                            (int)consistent,
                            src[0], src[1], src[2], src[3],
                            src[4], src[5], src[6], src[7],
                            dst[0], dst[1], dst[2], dst[3],
                            dst[4], dst[5], dst[6], dst[7]);
            return EXIT_FAILURE;
        }

        ++local_iterations;
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int cache_stress_memcpy_2_cleanup(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(cache_stress_memcpy_2_neon, "Pure memory copy control, no flush (633 #27 REP MOVS lineage) — the zero-interference arm of cache_stress_clflush_2 (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = cache_stress_memcpy_2_init,
    .test_run = cache_stress_memcpy_2_run,
    .test_cleanup = cache_stress_memcpy_2_cleanup,
    .fracture_loop_count = -1,   /* never fracture: microsecond rounds */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
