/*  flushed_memcpy_neon — NEON variant of tests/cpu/cache/
 *  flushed_memcpy.cpp (same-day NEON pass; original untouched).
 *
 *  The rotate becomes a VECTOR rotate: each round rotates by k whole
 *  uint32x4 vectors (dst_vec[i] = vld1q(src_vec[(i+k) mod N]) via
 *  vst1q) — 1024 words move as 256 NEON load/store pairs — and the
 *  eviction cadence is unchanged: every 8 written destination LINES,
 *  one already-written line is dc-civac'd immediately, so the vector
 *  copy keeps running into lines being evicted underneath it.  The
 *  verification is the same rotate-expectation memcmp against the
 *  untouched source.
 */
#include "sandstone.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include <arm_neon.h>

static constexpr size_t FMN_BLOCK_SIZE = 4 * 1024;           /* 4 KiB  */
static constexpr size_t FMN_NUM_WORDS  = FMN_BLOCK_SIZE / 4;  /* 1024   */
static constexpr size_t FMN_NUM_VECS   = FMN_NUM_WORDS / 4;   /* 256    */
static constexpr size_t FMN_LINE       = 64;
static constexpr size_t FMN_FLUSH_EVERY_LINES = 8;

static inline void dc_civac(void *addr) {
    __asm__ volatile("dc civac, %0" :: "r"(addr) : "memory");
}

static inline void dsb_sy() {
    __asm__ volatile("dsb sy" ::: "memory");
}

static int flushed_memcpy_neon_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int flushed_memcpy_neon_cleanup(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int flushed_memcpy_neon_run(struct test *test, int cpu) {
    (void)cpu;

    std::vector<uint32_t> src(FMN_NUM_WORDS);
    std::vector<uint32_t> dst(FMN_NUM_WORDS);
    std::vector<uint32_t> expected(FMN_NUM_WORDS);

    uint64_t rounds = 0;

    do {
        /* fresh random source every round */
        for (size_t i = 0; i < FMN_NUM_WORDS; ++i)
            src[i] = (uint32_t)random64();

        /* vector-granular rotate amount */
        const size_t k = 1 + (size_t)(random64() % 63);

        /* vector rotate-copy with flushes interleaved INTO the copy */
        size_t flushed_lines = 0;
        const uint32_t *sv = src.data();
        uint32_t *dv = dst.data();
        for (size_t v = 0; v < FMN_NUM_VECS; ++v) {
            vst1q_u32(dv + v * 4,
                      vld1q_u32(sv + ((v + k) % FMN_NUM_VECS) * 4));
            /* every 8 completed destination LINES (32 vectors), evict
             * one a few lines back — the original cadence */
            if ((v + 1) % (FMN_FLUSH_EVERY_LINES * (FMN_LINE / 16)) == 0) {
                size_t victim_line = v / (FMN_LINE / 16);   /* lines of 4 vecs */
                if (victim_line >= FMN_FLUSH_EVERY_LINES) {
                    victim_line -= FMN_FLUSH_EVERY_LINES;
                    dc_civac(dv + victim_line * (FMN_LINE / 4));
                    ++flushed_lines;
                }
            }
        }
        dsb_sy();

        /* independent golden: the rotate expectation from src */
        for (size_t i = 0; i < FMN_NUM_WORDS; ++i)
            expected[i] = src[(i + k * 4) % FMN_NUM_WORDS];

        if (memcmp(dst.data(), expected.data(), FMN_BLOCK_SIZE) != 0) {
            for (size_t i = 0; i < FMN_NUM_WORDS; ++i) {
                if (dst[i] != expected[i]) {
                    report_fail_msg("flushed_memcpy_neon: vector "
                                    "rotate-copy mismatch at word %zu "
                                    "(round %lu, k=%zu, flushed %zu "
                                    "lines): expected 0x%08x "
                                    "got 0x%08x", i, (unsigned long)rounds,
                                    k, flushed_lines, expected[i], dst[i]);
                    return EXIT_FAILURE;
                }
            }
        }

        ++rounds;
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(flushed_memcpy_neon, "NEON vector rotate-copy (vld1q/vst1q, fresh k each round) with dc-civac evictions interleaved into the copy frontier, verify vs rotate golden (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = flushed_memcpy_neon_init,
    .test_run = flushed_memcpy_neon_run,
    .test_cleanup = flushed_memcpy_neon_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
