/*  ma_nt_fp64 — FP64 datapath variant of tests/cpu/memory-access/ma_nt.cpp
 *  (2026-11-10; parent untouched).  Same tail-probe family structure;
 *  the load head's non-temporal pair store stream runs on the FP
 *  datapath: stnp d0, d1 (FP-register NT pair, same NT cache-policy
 *  semantics); the tail access pair runs the 64-bit FP-register
 *  datapath, probe/golden from ma_tail_probe_fp64.h (bit-exact
 *  NaN-safe compare).
 */
#include "ma_tail_probe_fp64.h"
#include <arm_neon.h>

namespace { /* ODR: per-TU MA_NT_FP64_T */
struct MA_NT_FP64_T {
    ma_probe_f64::MAF64Data probe;
    double scratch[MA_F64_WORDS];
};
}

static int ma_nt_fp64_init(struct test *test) {
    auto *d = new MA_NT_FP64_T;
    if (!d) return EXIT_FAILURE;
    ma_probe_f64::ma_init_f64(&d->probe);
    memset(d->scratch, 0, sizeof(d->scratch));
    test->data = d;
    return EXIT_SUCCESS;
}

static int ma_nt_fp64_cleanup(struct test *test) {
    delete static_cast<MA_NT_FP64_T *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int ma_nt_fp64_run(struct test *test, int cpu) {
    auto *d = static_cast<MA_NT_FP64_T *>(test->data);
    (void)cpu;
    uint64_t round = 0;

    do {
        ++round;
        /* LOAD HEAD: stnp non-temporal FP-register pair store stream
         * into scratch (stnp d0, d1 — same NT cache policy) */
        for (uint32_t j = 0; j + 1 < MA_F64_WORDS; j += 2)
            __asm__ volatile("stnp %d0, %d1, [%2]"
                             :: "w"(d->probe.src[j]), "w"(d->probe.src[j + 1]),
                                "r"(&d->scratch[j]) : "memory");

        /* TAIL PROBE: 16-element segment, rotating every round */
        const uint32_t base =
            (uint32_t)((round * MA_F64_SEG) % (MA_F64_WORDS - MA_F64_SEG));
        const bool mode_b = (round & 1) != 0;
        for (int i = 0; i < MA_F64_SEG; ++i) {
            const bool ok = mode_b
                ? ma_probe_f64::probe_ldr_str_str_f64(&d->probe, base, i)
                : ma_probe_f64::probe_str_str_f64(&d->probe, base, i);
            if (!ok) {
                report_fail_msg("ma_nt_fp64: tail access-pair mismatch at "
                                "word %u (round %lu, mode %s): temp=0x%016llx "
                                "dst=0x%016llx golden=0x%016llx (mis-placed "
                                "store after NT FP64 pair stream)",
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

DECLARE_TEST(ma_nt_fp64, "FP64 variant of ma_nt: tail access-pair probe after stnp non-temporal FP-register pair stream load: str->str / ldr->str->str back-to-back d-reg asm with bit-exact init-snapshot golden (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ma_nt_fp64_init,
    .test_run = ma_nt_fp64_run,
    .test_cleanup = ma_nt_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture (family convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
