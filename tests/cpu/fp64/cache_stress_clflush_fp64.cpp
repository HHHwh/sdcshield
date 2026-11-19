/*  cache_stress_clflush_fp64 — FP64 datapath variant of
 *  tests/cpu/arm_add/cache_stress_clflush.cpp (2026-11-10; parent
 *  untouched).
 *
 *  Parent: batch copy (plain GPR memcpy) into dst with dc civac + dsb sy
 *  evicting already-written dst lines mid-flight, verify against an
 *  independent init-time golden snapshot; dst is pre-corrupted each
 *  round so a lost store cannot masquerade as stale bytes.
 *  This variant keeps the chassis EXACTLY (same per-thread 64 KB
 *  buffers, same line loop, same CSF_FLUSH_EVERY cadence, same civac
 *  victim selection, same poison/memcmp/golden discipline) and re-types
 *  ONLY the payload copy path: the per-line copy and the pre-corrupt
 *  both run as vld1q_f64/vst1q_f64 streams — the copy's loads/stores
 *  now go through the FP register file / FP vector store port instead
 *  of GPR memcpy.  The golden snapshot stays a plain memcpy (issue
 *  #373-style independence: different code path from the path under
 *  test); the verify stays bit-exact memcmp over the same buffers.
 *
 *  Parent untouched; x86-64 untouched (aarch64-only registration).
 */
#include "sandstone.h"
#include <cstdint>
#include <cstring>
#include <arm_neon.h>
#include "../arm_add/arm_add_excite.h"

#define CSF_SIZE        (64 * 1024)              /* 64 KB, 1024 lines */
#define CSF_LINE        64
#define CSF_LINES       (CSF_SIZE / CSF_LINE)
#define CSF_FLUSH_EVERY 8                        /* flush 1 line per 8 copied */

static inline void csf_flush_line(const void *addr) {
    __asm__ volatile(
        "dc civac, %0\n\t"
        "dsb sy\n\t"
        :: "r"(addr) : "memory");
}

/* FP-datapath line copy: 64B as 4x vld1q_f64/vst1q_f64 (bit-pattern
 * pass-through — the datapath, not arithmetic, is the variable) */
static inline void csf_fp64_copy_line(double *dst, const double *src) {
    for (int v = 0; v < CSF_LINE / 16; ++v)
        vst1q_f64(dst + v * 2, vld1q_f64(src + v * 2));
}

static int cache_stress_clflush_fp64_init(struct test *test) {
    (void)test;   /* buffers are per-thread now (thread-safety fix) */
    return EXIT_SUCCESS;
}

static int cache_stress_clflush_fp64_cleanup(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int cache_stress_clflush_fp64_run(struct test *test, int cpu) {
    (void)cpu;
    uint64_t epoch = 0;

    /* PER-THREAD buffers (thread-safety fix — see header note) */
    double *src    = new double[CSF_SIZE / 8];
    double *dst    = new double[CSF_SIZE / 8];
    double *golden = new double[CSF_SIZE / 8];
    if (!src || !dst || !golden) return EXIT_FAILURE;

    memset_random(src, CSF_SIZE);
    memcpy(golden, src, CSF_SIZE);   /* independent golden snapshot (memcpy, issue #373) */
    memset(dst, 0, CSF_SIZE);

    do {
        ++epoch;
        const uint64_t poison = ~epoch;
        const float64x2_t pv = vreinterpretq_f64_u64(vdupq_n_u64(poison));

        /* pre-corrupt dst on the FP datapath: a store lost this round
         * cannot masquerade as last round's correct bytes */
        for (size_t v = 0; v < CSF_SIZE / 16; ++v)
            vst1q_f64(dst + v * 2, pv);

        /* batch copy src -> dst on the FP datapath with ongoing flush
         * interference on the destination's already-written lines */
        for (int line = 0; line < CSF_LINES; ++line) {
            csf_fp64_copy_line(dst + (size_t)line * CSF_LINE / 8,
                               src + (size_t)line * CSF_LINE / 8);
            if (line % CSF_FLUSH_EVERY == 0 && line >= CSF_FLUSH_EVERY) {
                /* evict an already-written dst line (a few lines back,
                 * so it is genuinely completed work under pressure) */
                int victim = line - CSF_FLUSH_EVERY;
                csf_flush_line(&dst[(size_t)victim * CSF_LINE / 8]);
            }
        }

        /* verify against the independent golden (bit-exact) */
        if (memcmp(dst, golden, CSF_SIZE) != 0) {
            for (int line = 0; line < CSF_LINES; ++line) {
                const double *g = golden + (size_t)line * CSF_LINE / 8;
                const double *a = dst    + (size_t)line * CSF_LINE / 8;
                if (memcmp(g, a, CSF_LINE) != 0) {
                    report_fail_msg("cache_stress_clflush_fp64: dst line %d "
                                    "differs from golden under flush "
                                    "interference (g0=%016lx a0=%016lx)",
                                    line,
                                    (unsigned long)*reinterpret_cast<const uint64_t *>(g),
                                    (unsigned long)*reinterpret_cast<const uint64_t *>(a));
                }
            }
        }

        /* excitation filler: NEON switching power (see arm_add_excite.h) */
        if (!arm_add_excite::fill(cpu, epoch)) {
            report_fail_msg("excitation filler store->reload mismatch "
                            "(cpu %d round %lu)", cpu,
                            (unsigned long)epoch);
        }
    } while (test_time_condition(test));

    delete[] src;
    delete[] dst;
    delete[] golden;
    return EXIT_SUCCESS;
}

DECLARE_TEST(cache_stress_clflush_fp64, "FP64 variant of cache_stress_clflush: vld1q_f64/vst1q_f64 line copy with dc civac + dsb sy evicting completed dst lines mid-flight, bit-exact golden memcmp (ARM64)")
    .test_init = cache_stress_clflush_fp64_init,
    .test_run = cache_stress_clflush_fp64_run,
    .test_cleanup = cache_stress_clflush_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture: our semantic rounds are microseconds; auto mode (40) ends the run in ~10 ms */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
