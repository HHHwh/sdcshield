/*  ma_atomic_fp64 — FP64 datapath variant of tests/cpu/memory-access/ma_atomic.cpp
 *  (2026-11-10; parent untouched).  Same tail-probe family structure;
 *  the load head's exclusive pair stream (ldaxr/stxr) stays on the
 *  GPR datapath verbatim — the architecture has NO FP-register form of
 *  the exclusive pair, and the pair is the head's semantic subject;
 *  this test's FP64 re-typing lives in the tail access pair.  Tail
 *  probe/golden from ma_tail_probe_fp64.h (bit-exact NaN-safe compare).
 */
#include "ma_tail_probe_fp64.h"
#include <arm_neon.h>

namespace { /* ODR: per-TU MA_ATOMIC_FP64_T */
struct MA_ATOMIC_FP64_T {
    ma_probe_f64::MAF64Data probe;
    uint32_t scratch32[MA_F64_WORDS * 2];   /* exclusive-pair target: GPR bit-level (LDAXR/STXR are GPR-only), 16 KiB */
};
}

static int ma_atomic_fp64_init(struct test *test) {
    auto *d = new MA_ATOMIC_FP64_T;
    if (!d) return EXIT_FAILURE;
    ma_probe_f64::ma_init_f64(&d->probe);
    memset(d->scratch32, 0, sizeof(d->scratch32));
    test->data = d;
    return EXIT_SUCCESS;
}

static int ma_atomic_fp64_cleanup(struct test *test) {
    delete static_cast<MA_ATOMIC_FP64_T *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int ma_atomic_fp64_run(struct test *test, int cpu) {
    auto *d = static_cast<MA_ATOMIC_FP64_T *>(test->data);
    (void)cpu;
    uint64_t round = 0;

    do {
        ++round;
        /* LOAD HEAD: ldaxr/stxr exclusive pair stream — kept verbatim
         * on the GPR datapath: LDAXR/STXR have NO FP-register encoding
         * (architecture: exclusive loads/stores are GPR-only), and the
         * exclusive pair is this head's semantic subject.  This test's
         * FP64 re-typing lives in the tail access pair below. */
        for (int k = 0; k < 128; ++k) {
            const uint32_t idx = (uint32_t)(random64() % MA_F64_WORDS);
            uint32_t tmp = 0, res;
            __asm__ volatile(
                "ldaxr %w0, [%2]\n\t"
                "add %w0, %w0, #1\n\t"
                "stxr %w1, %w0, [%2]\n\t"
                : "=&r"(tmp), "=&r"(res)
                : "r"(&d->scratch32[idx])
                : "memory");
            (void)res;   /* no retry: the instruction pair is the payload */
        }

        /* TAIL PROBE: 16-element segment, rotating every round */
        const uint32_t base =
            (uint32_t)((round * MA_F64_SEG) % (MA_F64_WORDS - MA_F64_SEG));
        const bool mode_b = (round & 1) != 0;
        for (int i = 0; i < MA_F64_SEG; ++i) {
            const bool ok = mode_b
                ? ma_probe_f64::probe_ldr_str_str_f64(&d->probe, base, i)
                : ma_probe_f64::probe_str_str_f64(&d->probe, base, i);
            if (!ok) {
                report_fail_msg("ma_atomic_fp64: tail access-pair mismatch at "
                                "word %u (round %lu, mode %s): temp=0x%016llx "
                                "dst=0x%016llx golden=0x%016llx (mis-placed "
                                "store after FP64 exclusive-pair burst)",
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

DECLARE_TEST(ma_atomic_fp64, "FP64 variant of ma_atomic: tail access-pair probe after a GPR ldaxr/stxr exclusive pair stream (the architecture has no FP-register exclusive pair — kept bit-level, parent's): str->str / ldr->str->str back-to-back d-reg asm with bit-exact init-snapshot golden (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ma_atomic_fp64_init,
    .test_run = ma_atomic_fp64_run,
    .test_cleanup = ma_atomic_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture (family convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
