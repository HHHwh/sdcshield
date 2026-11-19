/*  mem_disambig_alu_neon_fp64 — FP64 data-path variant of tests/cpu/arm_add/mem_disambig_alu_neon.cpp
 *  (2026-11-10; parent untouched).
 *
 *  Parent: a rotating vector ALU (veorq/vaddq/vandq/vsubq on uint64x2,
 *  the offset broadcast as the second source, rotating per pattern)
 *  feeds the 16B NEON store-to-load forward; expected table is the
 *  init-time same-source recompute.
 *  This variant keeps the chassis EXACTLY (same offsets, same random
 *  two-source inputs, same store/reload forward path, same init
 *  snapshot mechanism) and re-types ONLY the producer ALU to the FP64
 *  pipe: vaddq/vsubq/vmulq/vsubq(rev) on float64x2 — the FP
 *  register file and FP add/mul units are now the producer in front
 *  of the forward.  Random bit patterns reinterpreted as doubles may
 *  go NaN/Inf — deterministic single-op IEEE754 propagation, golden
 *  is the same-op init snapshot, bit-compared (parent discipline).
 *  The offset (broadcast operand) is < 4096, exactly representable.
 *
 *  Original untouched; x86-64 untouched (aarch64-only registration).
 */
#include "sandstone.h"
#include <cstdint>
#include <cstdlib>
#include <arm_neon.h>

#define MDAN_BUFFER_SIZE   4096
#define MDAN_PATTERN_COUNT 512

namespace { /* ODR: per-TU MDANData */
struct MDANData {
    uint64_t *offsets;      /* 16B-aligned */
    uint64x2_t *values;     /* random two-source inputs */
    uint64x2_t *expected;   /* init-time FP64 ALU snapshot */
};
}

static inline uint64x2_t mdan_alu_f64(const uint64x2_t &v, uint64_t off, int phase) {
    const float64x2_t o = vdupq_n_f64((double)off);   /* off < 4096: exact */
    const float64x2_t fv = vreinterpretq_f64_u64(v);
    switch (phase & 3) {
        case 0:  return vreinterpretq_u64_f64(vaddq_f64(fv, o));
        case 1:  return vreinterpretq_u64_f64(vsubq_f64(fv, o));
        case 2:  return vreinterpretq_u64_f64(vmulq_f64(fv, o));
        default: return vreinterpretq_u64_f64(vsubq_f64(o, fv));  /* swapped-operand sub */
    }
}

static int mem_disambig_alu_neon_fp64_init(struct test *test) {
    auto *d = new MDANData;
    if (!d) return EXIT_FAILURE;
    d->offsets  = static_cast<uint64_t *>(malloc(MDAN_PATTERN_COUNT * sizeof(uint64_t)));
    d->values   = static_cast<uint64x2_t *>(malloc(MDAN_PATTERN_COUNT * sizeof(uint64x2_t)));
    d->expected = static_cast<uint64x2_t *>(malloc(MDAN_PATTERN_COUNT * sizeof(uint64x2_t)));
    if (!d->offsets || !d->values || !d->expected) return EXIT_FAILURE;
    for (int i = 0; i < MDAN_PATTERN_COUNT; ++i) {
        d->offsets[i] = (random64() % (MDAN_BUFFER_SIZE - 16)) & ~15ULL;
        uint64x2_t v = {random64(), random64()};
        d->values[i] = v;
        d->expected[i] = mdan_alu_f64(v, d->offsets[i], i);
    }
    test->data = d;
    return EXIT_SUCCESS;
}

static int mem_disambig_alu_neon_fp64_cleanup(struct test *test) {
    auto *d = static_cast<MDANData *>(test->data);
    free(d->offsets);
    free(d->values);
    free(d->expected);
    delete d;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int mem_disambig_alu_neon_fp64_run(struct test *test, int cpu) {
    (void)cpu;
    auto *d = static_cast<MDANData *>(test->data);
    uint8_t *buffer = static_cast<uint8_t *>(aligned_alloc(64, MDAN_BUFFER_SIZE));

    do {
        for (int i = 0; i < MDAN_PATTERN_COUNT; ++i) {
            uint64_t off = d->offsets[i];
            uint64x2_t res = mdan_alu_f64(d->values[i], off, i);

            /* FP64 vector ALU result through the store buffer */
            vst1q_u64(reinterpret_cast<uint64_t *>(buffer + off), res);
            uint64x2_t loaded = vld1q_u64(reinterpret_cast<const uint64_t *>(buffer + off));

            uint64x2_t diff = veorq_u64(loaded, d->expected[i]);
            if ((vgetq_lane_u64(diff, 0) | vgetq_lane_u64(diff, 1)) != 0) {
                report_fail_msg("mem_disambig_alu_neon_fp64: forward (FP64 "
                                "vector ALU) mismatch at pattern %d (offset "
                                "%llu): expected {%llx,%llx} got "
                                "{%llx,%llx}", i,
                                (unsigned long long)off,
                                (unsigned long long)vgetq_lane_u64(d->expected[i], 0),
                                (unsigned long long)vgetq_lane_u64(d->expected[i], 1),
                                (unsigned long long)vgetq_lane_u64(loaded, 0),
                                (unsigned long long)vgetq_lane_u64(loaded, 1));
            }
        }
    } while (test_time_condition(test));

    free(buffer);
    return EXIT_SUCCESS;
}

DECLARE_TEST(mem_disambig_alu_neon_fp64, "FP64 variant of mem_disambig_alu_neon: vaddq/vsubq/vmulq/vsubq(rev) FP64 producer feeding the 16B NEON store-to-load forward, init-snapshot golden (ARM64)")
    .test_init = mem_disambig_alu_neon_fp64_init,
    .test_run = mem_disambig_alu_neon_fp64_run,
    .test_cleanup = mem_disambig_alu_neon_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
