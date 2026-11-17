/*  mfence_pure_neon — NEON variant of tests/cpu/memory/mfence_pure.cpp.
 *
 *  Original: scalar load from a source, a full barrier (__sync_synchronize
 *  -> DMB SY), then a store to the destination, verifying the loaded
 *  value against the golden source.  This variant: vld1q_u64 source load,
 *  explicit DMB SY, vst1q_u64 destination store, NEON-load verification —
 *  the barrier sits between a NEON load and a NEON store, so the load
 *  must complete (visibility-wise) before the store is issued on the
 *  vector path.
 *
 *  Original untouched; x86-64 untouched (aarch64-only registration).
 */
#include "sandstone.h"
#include <cstdint>
#include <cstdlib>
#include <arm_neon.h>

#define MFPN_BUFFER_SIZE (16 * 1024)   /* in uint64x2 elements */

namespace { /* ODR: per-TU MFPNData */
struct MFPNData {
    uint64x2_t *data_src;
    uint64x2_t *data_dst;
    uint64x2_t *golden_dst;
};
}

static int mfence_pure_neon_init(struct test *test) {
    auto *d = new MFPNData;
    if (!d) return EXIT_FAILURE;
    d->data_src   = static_cast<uint64x2_t *>(malloc(MFPN_BUFFER_SIZE * sizeof(uint64x2_t)));
    d->data_dst   = static_cast<uint64x2_t *>(malloc(MFPN_BUFFER_SIZE * sizeof(uint64x2_t)));
    d->golden_dst = static_cast<uint64x2_t *>(malloc(MFPN_BUFFER_SIZE * sizeof(uint64x2_t)));
    if (!d->data_src || !d->data_dst || !d->golden_dst)
        return EXIT_FAILURE;
    for (int i = 0; i < MFPN_BUFFER_SIZE; ++i) {
        d->data_src[i]   = uint64x2_t{random64(), random64()};
        d->data_dst[i]   = vdupq_n_u64(0);
        d->golden_dst[i] = d->data_src[i];   /* independent golden snapshot */
    }
    test->data = d;
    return EXIT_SUCCESS;
}

static int mfence_pure_neon_cleanup(struct test *test) {
    auto *d = static_cast<MFPNData *>(test->data);
    free(d->data_src);
    free(d->data_dst);
    free(d->golden_dst);
    delete d;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int mfence_pure_neon_run(struct test *test, int cpu) {
    (void)cpu;
    auto *d = static_cast<MFPNData *>(test->data);

    do {
        uint32_t idx = random32() & (MFPN_BUFFER_SIZE - 1);

        /* NEON load from source */
        uint64x2_t val = vld1q_u64(reinterpret_cast<const uint64_t *>(&d->data_src[idx]));

        /* full barrier between the vector load and the vector store */
        __asm__ volatile("dmb sy" ::: "memory");

        /* NEON store to destination */
        vst1q_u64(reinterpret_cast<uint64_t *>(&d->data_dst[idx]), d->golden_dst[idx]);

        /* verify the loaded value against the source and the stored
         * value against the golden, both through NEON loads */
        uint64x2_t back = vld1q_u64(reinterpret_cast<const uint64_t *>(&d->data_dst[idx]));
        uint64x2_t dv = veorq_u64(val, d->data_src[idx]);
        uint64x2_t ds = veorq_u64(back, d->golden_dst[idx]);
        if ((vgetq_lane_u64(dv, 0) | vgetq_lane_u64(dv, 1)) != 0) {
            report_fail_msg("mfence_pure_neon: source load mismatch at "
                            "index %u: loaded {%llx,%llx} vs src "
                            "{%llx,%llx}", idx,
                            (unsigned long long)vgetq_lane_u64(val, 0),
                            (unsigned long long)vgetq_lane_u64(val, 1),
                            (unsigned long long)vgetq_lane_u64(d->data_src[idx], 0),
                            (unsigned long long)vgetq_lane_u64(d->data_src[idx], 1));
        }
        if ((vgetq_lane_u64(ds, 0) | vgetq_lane_u64(ds, 1)) != 0) {
            report_fail_msg("mfence_pure_neon: destination store mismatch "
                            "at index %u: expected {%llx,%llx}", idx,
                            (unsigned long long)vgetq_lane_u64(d->golden_dst[idx], 0),
                            (unsigned long long)vgetq_lane_u64(d->golden_dst[idx], 1));
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(mfence_pure_neon, "NEON variant of mfence_pure: vld1q source load, DMB SY barrier, vst1q destination store, dual NEON verification (ARM64)")
    .test_init = mfence_pure_neon_init,
    .test_run = mfence_pure_neon_run,
    .test_cleanup = mfence_pure_neon_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
