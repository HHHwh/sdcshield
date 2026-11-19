/*  NEON variant of tests/cpu/cache/cache_stress_clflush_2_neon.cpp — same-day NEON pass:
 *  load-point memcpy() replaced with the explicit vld1q/vst1q stream
 *  (arm_add_neon_copy.h); flush/verify semantics identical.  Original
  *  untouched; identifiers carry the _neon suffix (ODR).
 */
/*  cache_stress_clflush_2_neon — memory copy under RANDOM cache-line flush
 *  interference (633 #26 lineage, user spec 2026-11-02).
 *
 *  Differentiated from arm_add/cache_stress_clflush (same 633 #26
 *  lineage, written the same day): that one uses a fixed random golden
 *  + deterministic every-8th-line flush cadence; THIS variant uses
 *  per-iteration random re-fill of BOTH buffers (fresh bit patterns
 *  every round, anti-golden-fold by construction) and RANDOM line
 *  flush targets on both src and dst before the copy — the flush
 *  interference points move every round.
 *
 *  Sequence per iteration:
 *    1. src  <- random qwords (fresh every round)
 *    2. dst  <- random qwords (pre-corrupt: a lost copy cannot
 *               masquerade as last round's correct bytes)
 *    3. dc civac x4 on random lines of src AND dst, then dsb sy —
 *       the interference completes before the copy starts
 *    4. memcpy dst <- src
 *    5. memcmp(dst, src) — src is the independent golden (it is never
 *       written after the fill, so dst must equal it bit-exactly)
 *    6. store->reload consistency: copy dst into store_buf, compare —
 *       a second, same-core read pass over the just-copied data
 *
 *  Adaptations to repo conventions (vs the user's draft):
 *    - framework RNG (random64) instead of std::mt19937 seeded from
 *      std::random_device — seeds stay framework-controlled and
 *      reproducible (-s Constant/LCG/AES);
 *    - the per-iteration fprintf PASS/FAIL log block removed — hot
 *      path must carry no I/O (fail is reported through
 *      report_fail_msg only);
 *    - .fracture_loop_count = -1 — microsecond-scale semantic rounds
 *      would otherwise be auto-fractured to ~10 ms runs;
 *    - random qword fill replaces random-double fill (the double bit
 *      patterns are a subset of full random64 space).
 */
#include "sandstone.h"
#include "arm_add_neon_copy.h"
using arm_add_neon::neon_copy;
#include <cstdint>
#include <cstring>
#include <vector>

static constexpr size_t CS2_BLOCK_SIZE = 4096;   /* 4 KB */
static constexpr size_t CS2_CACHE_LINE = 64;

/* ARM64 cache-line clean+invalidate (CLFLUSH equivalent) */
static inline void dc_civac(void *addr) {
    __asm__ volatile("dc civac, %0" :: "r"(addr) : "memory");
}

static inline void dsb_sy() {
    __asm__ volatile("dsb sy" ::: "memory");
}

static int cache_stress_clflush_2_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int cache_stress_clflush_2_run(struct test *test, int cpu) {
    (void)cpu;

    /* per-thread buffers */
    std::vector<uint8_t> src(CS2_BLOCK_SIZE);
    std::vector<uint8_t> dst(CS2_BLOCK_SIZE);
    std::vector<uint8_t> store_buf(CS2_BLOCK_SIZE);

    uint64_t local_iterations = 0;

    do {
        /* 1. fill src with random qwords (fresh every round) */
        for (size_t i = 0; i + sizeof(uint64_t) <= CS2_BLOCK_SIZE;
             i += sizeof(uint64_t)) {
            uint64_t val = random64();
            memcpy(&src[i], &val, sizeof(val));
        }

        /* 2. pre-corrupt dst with random qwords — a store lost this
         * round cannot hide behind last round's correct bytes */
        for (size_t i = 0; i + sizeof(uint64_t) <= CS2_BLOCK_SIZE;
             i += sizeof(uint64_t)) {
            uint64_t val = random64();
            memcpy(&dst[i], &val, sizeof(val));
        }

        /* 3. random cache-line flush interference on BOTH buffers,
         *    drained before the copy starts */
        for (int i = 0; i < 4; ++i) {
            size_t src_off = random64() % (CS2_BLOCK_SIZE - CS2_CACHE_LINE);
            size_t dst_off = random64() % (CS2_BLOCK_SIZE - CS2_CACHE_LINE);
            dc_civac(src.data() + src_off);
            dc_civac(dst.data() + dst_off);
        }
        dsb_sy();

        /* 4. the copy under test */
        neon_copy(dst.data(), src.data(), CS2_BLOCK_SIZE);

        /* 5. verify: src is the independent golden */
        bool data_ok = (memcmp(dst.data(), src.data(), CS2_BLOCK_SIZE) == 0);

        /* 6. store->reload consistency pass */
        neon_copy(store_buf.data(), dst.data(), CS2_BLOCK_SIZE);
        bool consistent =
            (memcmp(store_buf.data(), dst.data(), CS2_BLOCK_SIZE) == 0);

        if (!data_ok || !consistent) {
            report_fail_msg("cache_stress_clflush_2_neon: data mismatch or "
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

static int cache_stress_clflush_2_cleanup(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(cache_stress_clflush_2_neon, "Memory copy under random dc-civac line-flush interference on both src and dst, fresh random data every round (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = cache_stress_clflush_2_init,
    .test_run = cache_stress_clflush_2_run,
    .test_cleanup = cache_stress_clflush_2_cleanup,
    .fracture_loop_count = -1,   /* never fracture: microsecond rounds */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
