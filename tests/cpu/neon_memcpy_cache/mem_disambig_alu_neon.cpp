/*  mem_disambig_alu_neon — NEON variant of tests/cpu/memory/
 *  mem_disambig_alu.cpp.
 *
 *  Original: scalar two-source ALU (xor/add/and/sub, rotating) whose
 *  result is stored and immediately re-loaded through the store
 *  buffer, checked against an init-time expected table.
 *  This variant: the ALU is the NEON vector pipeline — veorq/vaddq/
 *  vandq/vsubq on uint64x2, rotating per pattern — and the store/
 *  reload is the 16B NEON path.  The expected table is computed at
 *  init with the SAME NEON ops from the same random inputs (same-
 *  source recompute: any runtime bit flip in either the ALU or the
 *  NEON store/load path surfaces as a mismatch vs the init snapshot).
 *
 *  Complementary to mem_disambiguation_neon (that one stresses raw
 *  forwarding; this one adds the vector-ALU producer in front of the
 *  forward).  Original untouched; x86-64 untouched (aarch64-only).
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
    uint64x2_t *expected;   /* init-time NEON ALU snapshot */
};
}

static inline uint64x2_t mdan_alu(const uint64x2_t &v, uint64_t off, int phase) {
    uint64x2_t o = vdupq_n_u64(off);
    switch (phase & 3) {
        case 0: return veorq_u64(v, o);
        case 1: return vaddq_u64(v, o);
        case 2: return vandq_u64(v, o);
        default: return vsubq_u64(v, o);
    }
}

static int mem_disambig_alu_neon_init(struct test *test) {
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
        d->expected[i] = mdan_alu(v, d->offsets[i], i);
    }
    test->data = d;
    return EXIT_SUCCESS;
}

static int mem_disambig_alu_neon_cleanup(struct test *test) {
    auto *d = static_cast<MDANData *>(test->data);
    free(d->offsets);
    free(d->values);
    free(d->expected);
    delete d;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int mem_disambig_alu_neon_run(struct test *test, int cpu) {
    (void)cpu;
    auto *d = static_cast<MDANData *>(test->data);
    uint8_t *buffer = static_cast<uint8_t *>(aligned_alloc(64, MDAN_BUFFER_SIZE));

    do {
        for (int i = 0; i < MDAN_PATTERN_COUNT; ++i) {
            uint64_t off = d->offsets[i];
            uint64x2_t res = mdan_alu(d->values[i], off, i);

            /* NEON vector ALU result through the store buffer */
            vst1q_u64(reinterpret_cast<uint64_t *>(buffer + off), res);
            uint64x2_t loaded = vld1q_u64(reinterpret_cast<const uint64_t *>(buffer + off));

            uint64x2_t diff = veorq_u64(loaded, d->expected[i]);
            if ((vgetq_lane_u64(diff, 0) | vgetq_lane_u64(diff, 1)) != 0) {
                report_fail_msg("mem_disambig_alu_neon: forward (vector "
                                "ALU) mismatch at pattern %d (offset "
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

DECLARE_TEST(mem_disambig_alu_neon, "NEON variant of mem_disambig_alu: veorq/vaddq/vandq/vsubq vector ALU producer feeding the 16B NEON store-to-load forward, init-snapshot golden (ARM64)")
    .test_init = mem_disambig_alu_neon_init,
    .test_run = mem_disambig_alu_neon_run,
    .test_cleanup = mem_disambig_alu_neon_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
