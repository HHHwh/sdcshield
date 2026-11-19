/*  mfence_neon — NEON variant of tests/cpu/memory/mfence.cpp.
 *
 *  Original: scalar stores to A, a full memory barrier (__sync_synchronize
 *  -> DMB SY on ARMv8), then stores to B, both verified against golden
 *  tables.  This variant moves the stores onto the 16B NEON datapath:
 *  vst1q_u64 to A, DMB SY (inline asm, the instruction the original's
 *  barrier compiles to), vst1q_u64 to B, then vld1q_u64 verification of
 *  both — the barrier straddles NEON stores instead of scalar ones.
 *
 *  Original untouched; x86-64 untouched (aarch64-only registration).
 */
#include "sandstone.h"
#include <cstdint>
#include <cstdlib>
#include <arm_neon.h>

#define MFN_BUFFER_SIZE (16 * 1024)   /* in uint64x2 elements */

namespace { /* ODR: per-TU MFNData */
struct MFNData {
    uint64x2_t *golden_a;
    uint64x2_t *golden_b;
    uint64x2_t *store_a;
    uint64x2_t *store_b;
};
}

static int mfence_neon_init(struct test *test) {
    auto *d = new MFNData;
    if (!d) return EXIT_FAILURE;
    d->golden_a = static_cast<uint64x2_t *>(malloc(MFN_BUFFER_SIZE * sizeof(uint64x2_t)));
    d->golden_b = static_cast<uint64x2_t *>(malloc(MFN_BUFFER_SIZE * sizeof(uint64x2_t)));
    d->store_a  = static_cast<uint64x2_t *>(malloc(MFN_BUFFER_SIZE * sizeof(uint64x2_t)));
    d->store_b  = static_cast<uint64x2_t *>(malloc(MFN_BUFFER_SIZE * sizeof(uint64x2_t)));
    if (!d->golden_a || !d->golden_b || !d->store_a || !d->store_b)
        return EXIT_FAILURE;
    for (int i = 0; i < MFN_BUFFER_SIZE; ++i) {
        d->golden_a[i] = uint64x2_t{random64(), random64()};
        d->golden_b[i] = uint64x2_t{random64(), random64()};
        d->store_a[i]  = vdupq_n_u64(0);
        d->store_b[i]  = vdupq_n_u64(0);
    }
    test->data = d;
    return EXIT_SUCCESS;
}

static int mfence_neon_cleanup(struct test *test) {
    auto *d = static_cast<MFNData *>(test->data);
    free(d->golden_a);
    free(d->golden_b);
    free(d->store_a);
    free(d->store_b);
    delete d;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int mfence_neon_run(struct test *test, int cpu) {
    (void)cpu;
    auto *d = static_cast<MFNData *>(test->data);

    do {
        uint32_t idx = random32() & (MFN_BUFFER_SIZE - 1);

        /* NEON store to A */
        vst1q_u64(reinterpret_cast<uint64_t *>(&d->store_a[idx]), d->golden_a[idx]);

        /* full barrier — the same DMB SY the original's
         * __sync_synchronize compiles to, emitted explicitly */
        __asm__ volatile("dmb sy" ::: "memory");

        /* NEON store to B */
        vst1q_u64(reinterpret_cast<uint64_t *>(&d->store_b[idx]), d->golden_b[idx]);

        /* verify both through the NEON load path */
        uint64x2_t la = vld1q_u64(reinterpret_cast<const uint64_t *>(&d->store_a[idx]));
        uint64x2_t lb = vld1q_u64(reinterpret_cast<const uint64_t *>(&d->store_b[idx]));
        uint64x2_t da = veorq_u64(la, d->golden_a[idx]);
        uint64x2_t db = veorq_u64(lb, d->golden_b[idx]);
        if ((vgetq_lane_u64(da, 0) | vgetq_lane_u64(da, 1)) != 0) {
            report_fail_msg("mfence_neon: store A mismatch at index %u: "
                            "expected {%llx,%llx}", idx,
                            (unsigned long long)vgetq_lane_u64(d->golden_a[idx], 0),
                            (unsigned long long)vgetq_lane_u64(d->golden_a[idx], 1));
        }
        if ((vgetq_lane_u64(db, 0) | vgetq_lane_u64(db, 1)) != 0) {
            report_fail_msg("mfence_neon: store B mismatch at index %u: "
                            "expected {%llx,%llx}", idx,
                            (unsigned long long)vgetq_lane_u64(d->golden_b[idx], 0),
                            (unsigned long long)vgetq_lane_u64(d->golden_b[idx], 1));
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(mfence_neon, "NEON variant of mfence: vst1q store to A, DMB SY barrier, vst1q store to B, NEON-load verification of both (ARM64)")
    .test_init = mfence_neon_init,
    .test_run = mfence_neon_run,
    .test_cleanup = mfence_neon_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
