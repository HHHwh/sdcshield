/*  ma_alu_fixed_fp64 — memory-access tail-probe family, FP64 load head:
 *  fixed FP64 ALU chain (non-rotating).
 *  FP64 variant of ma_alu_fixed (parent: tests/cpu/memory-access/
 *  ma_alu_fixed.cpp).  The swept variable is the LOAD HEAD; the tail
 *  access pair (str->str / ldr->str->str, back-to-back asm, <=2-cycle
 *  store->load forwarding window) and the golden mechanism (init
 *  snapshot + rotating 16-word segment anti-fold, shared
 *  ma_tail_probe.h) are the parent's, untouched.  ONLY the load-head
 *  pressure chain is re-typed: the 128-step integer NEON chain
 *  (eor/mul/add/shl on u32 lanes) becomes a 128-step FP64 chain
 *  (mul/add on f64 lanes), so the head's consumer runs on the FP pipe /
 *  FP register file instead of the integer vector pipe.
 *
 *  The chain's output (scratch) is liveness pressure only — detection
 *  lives in the tail probe, so NaN/Inf propagation in the chain is
 *  irrelevant (constants are chosen mild; random-bit-pattern sources
 *  may still produce NaN/Inf, which is fine).
 *  Fault signature being probed: after a load burst, the second of two
 *  adjacent stores (or the pair after an adjacent ldr) mis-places data
 *  under pressure.
 */
#include "../memory-access/ma_tail_probe.h"
#include <arm_neon.h>

namespace { /* ODR: per-TU MA_ALU_FIXED_FP64_T */
struct MA_ALU_FIXED_FP64_T {
    ma_probe::MAData probe;
    uint32_t scratch[MA_WORDS];
};
}

static int ma_alu_fixed_fp64_init(struct test *test) {
    auto *d = new MA_ALU_FIXED_FP64_T;
    if (!d) return EXIT_FAILURE;
    ma_probe::ma_init(&d->probe);
    memset(d->scratch, 0, sizeof(d->scratch));
    test->data = d;
    return EXIT_SUCCESS;
}

static int ma_alu_fixed_fp64_cleanup(struct test *test) {
    delete static_cast<MA_ALU_FIXED_FP64_T *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int ma_alu_fixed_fp64_run(struct test *test, int cpu) {
    auto *d = static_cast<MA_ALU_FIXED_FP64_T *>(test->data);
    (void)cpu;
    uint64_t round = 0;

    do {
        ++round;
        /* LOAD HEAD: fixed FP64 ALU chain (mul/add, no rotation — the
         * rotating variant is the neon_rot family's already-covered
         * ground).  Same 128-step depth and 2-op-per-step shape as the
         * parent's integer chain; bit pattern of the source words is
         * reinterpreted as 2x f64 lanes. */
        float64x2_t v = vreinterpretq_f64_u32(
                vld1q_u32(&d->probe.src[round % (MA_WORDS - 4)]));
        const float64x2_t k1 = vdupq_n_f64(1.0000001);
        const float64x2_t k2 = vdupq_n_f64(1.0);
        for (int k = 0; k < 128; ++k) {
            v = vmulq_f64(v, k1);
            v = vaddq_f64(v, k2);
        }
        vst1q_u32(&d->scratch[round % (MA_WORDS - 4)],
                  vreinterpretq_u32_f64(v));

        /* TAIL PROBE: 16-word segment, rotating every round; the two
         * modes alternate by round parity */
        const uint32_t base =
            (uint32_t)((round * MA_SEG) % (MA_WORDS - MA_SEG));
        const bool mode_b = (round & 1) != 0;
        for (int i = 0; i < MA_SEG; ++i) {
            const bool ok = mode_b
                ? ma_probe::probe_ldr_str_str(&d->probe, base, i)
                : ma_probe::probe_str_str(&d->probe, base, i);
            if (!ok) {
                report_fail_msg("ma_alu_fixed_fp64: tail access-pair mismatch at "
                                "word %u (round %lu, mode %s): temp=0x%08x "
                                "dst=0x%08x golden=0x%08x (mis-placed "
                                "store after FP64 load burst)",
                                base + (uint32_t)i, (unsigned long)round,
                                mode_b ? "ldr+str+str" : "str+str",
                                d->probe.temp[base + i], d->probe.dst[base + i],
                                d->probe.golden[base + i]);
                return EXIT_FAILURE;
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(ma_alu_fixed_fp64, "FP64 variant of ma_alu_fixed: tail access-pair probe after fixed FP64 NEON ALU chain (non-rotating) load: str->str / ldr->str->str back-to-back asm with init-snapshot golden (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ma_alu_fixed_fp64_init,
    .test_run = ma_alu_fixed_fp64_run,
    .test_cleanup = ma_alu_fixed_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture (family convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
