/*  ma_barrier_fp64 — FP64 datapath variant of tests/cpu/memory-access/ma_barrier.cpp
 *  (2026-11-10; parent untouched).  Same tail-probe family structure;
 *  the load head's dense dmb ish barrier stream (512/round, a pure
 *  semantic stream with no data path) is kept verbatim; the tail
 *  access pair runs the 64-bit FP-register datapath, probe/golden
 *  from ma_tail_probe_fp64.h (bit-exact NaN-safe compare).
 */
#include "ma_tail_probe_fp64.h"
#include <arm_neon.h>

namespace { /* ODR: per-TU MA_BARRIER_FP64_T */
struct MA_BARRIER_FP64_T {
    ma_probe_f64::MAF64Data probe;
    double scratch[MA_F64_WORDS];
};
}

static int ma_barrier_fp64_init(struct test *test) {
    auto *d = new MA_BARRIER_FP64_T;
    if (!d) return EXIT_FAILURE;
    ma_probe_f64::ma_init_f64(&d->probe);
    memset(d->scratch, 0, sizeof(d->scratch));
    test->data = d;
    return EXIT_SUCCESS;
}

static int ma_barrier_fp64_cleanup(struct test *test) {
    delete static_cast<MA_BARRIER_FP64_T *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int ma_barrier_fp64_run(struct test *test, int cpu) {
    auto *d = static_cast<MA_BARRIER_FP64_T *>(test->data);
    (void)cpu;
    uint64_t round = 0;

    do {
        ++round;
        /* LOAD HEAD: dense dmb ish barrier stream (kept verbatim —
         * pure barrier semantics, no data path to re-type) */
        for (int k = 0; k < 512; ++k)
            __asm__ volatile("dmb ish" ::: "memory");

        /* TAIL PROBE: 16-element segment, rotating every round */
        const uint32_t base =
            (uint32_t)((round * MA_F64_SEG) % (MA_F64_WORDS - MA_F64_SEG));
        const bool mode_b = (round & 1) != 0;
        for (int i = 0; i < MA_F64_SEG; ++i) {
            const bool ok = mode_b
                ? ma_probe_f64::probe_ldr_str_str_f64(&d->probe, base, i)
                : ma_probe_f64::probe_str_str_f64(&d->probe, base, i);
            if (!ok) {
                report_fail_msg("ma_barrier_fp64: tail access-pair mismatch at "
                                "word %u (round %lu, mode %s): temp=0x%016llx "
                                "dst=0x%016llx golden=0x%016llx (mis-placed "
                                "store after barrier stream)",
                                base + (uint32_t)i, (unsigned long)round,
                                mode_b ? "ldr+str+str" : "str+str",
                                (unsigned long long)ma_probe_f64::f64_bits(d->probe.temp[base + i]),
                                (unsigned long long)ma_probe_f64::f64_bits(d->probe.dst[base + i]),
                                (unsigned long long)ma_probe_f64::f64_bits(d->probe.golden[base + i]));
                return EXIT_FAILURE;
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(ma_barrier_fp64, "FP64 variant of ma_barrier: tail access-pair probe after dense dmb ish barrier stream load: str->str / ldr->str->str back-to-back d-reg asm with bit-exact init-snapshot golden (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ma_barrier_fp64_init,
    .test_run = ma_barrier_fp64_run,
    .test_cleanup = ma_barrier_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture (family convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
