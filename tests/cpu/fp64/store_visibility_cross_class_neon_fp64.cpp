/*  store_visibility_cross_class_neon_fp64 — FP64 data-path variant of
 *  tests/cpu/arm_add/store_visibility_cross_class_neon.cpp (2026-11-10;
 *  parent untouched).
 *
 *  Same cross-coherency-domain visibility probe, with the payload on
 *  the FP64 store path: each reader slot is a float64x2 vector counter.
 *  The writer bumps the slot with an ASM vector load-add-store (asm,
 *  not intrinsics — GCC store-transforms and load-forwards
 *  same-address intrinsic pairs; the parent's measured trap), then
 *  dmb ish; readers read the slot twice (unbarriered, then after
 *  dmb ish) and fail on backwards reads or version regressions — a
 *  precisely-old vector surviving the barrier is the reported
 *  signature, now on the FP64 datapath.
 *
 *  Counters are integer-valued doubles (far below 2^53), so the
 *  lane-wise vcltq_f64 ordering checks are exactly the parent's
 *  vcltq_u32 ordering semantics; the writer's same-core read-back
 *  compare is bit-exact (veorq_u64 on the f64 bits, u32-view
 *  across reduce).
 *
 *  -n 1: only the writer runs; its same-core read-back (layer 1)
 *  exercises the full asm-vector-store -> barrier -> asm-load chain.
 */
#include "sandstone.h"
#include <cstdint>
#include <atomic>
#include <arm_neon.h>

#define SVCN_MAX_THREADS 1024

namespace { /* ODR: per-TU SVCNData */
struct SVCNData {
    alignas(64) double slots[SVCN_MAX_THREADS][2];
};
}

static inline float64x2_t ld_vec(const double *a) {
    float64x2_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(a) : "memory");
    return v;
}
static inline void st_vec(double *a, float64x2_t v) {
    __asm__ volatile("str %q0, [%1]" :: "w"(v), "r"(a) : "memory");
}

static int store_visibility_cross_class_neon_fp64_init(struct test *test) {
    auto *d = new SVCNData;
    if (!d) return EXIT_FAILURE;
    for (int i = 0; i < SVCN_MAX_THREADS; ++i)
        for (int l = 0; l < 2; ++l)
            d->slots[i][l] = 0.0;
    test->data = d;
    return EXIT_SUCCESS;
}

static int store_visibility_cross_class_neon_fp64_cleanup(struct test *test) {
    delete static_cast<SVCNData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int store_visibility_cross_class_neon_fp64_run(struct test *test, int cpu) {
    auto *d = static_cast<SVCNData *>(test->data);

    if (cpu == 0) {
        /* writer: bump every slot, barrier, same-core read-back */
        uint64_t round = 0;
        do {
            ++round;
            for (int r = 0; r < SVCN_MAX_THREADS; ++r) {
                double *slot = d->slots[r];
                float64x2_t v = ld_vec(slot);
                v = vaddq_f64(v, vdupq_n_f64(1.0));
                st_vec(slot, v);
                __asm__ volatile("dmb ish" ::: "memory");
                /* layer 1: same-core read-back must see v (bit-exact) */
                float64x2_t back = ld_vec(slot);
                uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(back),
                                            vreinterpretq_u64_f64(v));
                if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                    report_fail_msg("store_visibility_cross_class_neon_fp64: "
                                    "slot %d same-core read-back differs "
                                    "(round %lu)", r, (unsigned long)round);
                }
            }
        } while (test_time_condition(test));
    } else {
        /* reader: unbarriered read, dmb ish, barriered read */
        uint64_t round = 0;
        float64x2_t last_seen = vdupq_n_f64(0.0);
        do {
            ++round;
            double *slot = d->slots[cpu];
            for (int i = 0; i < 200; ++i) { __asm__ volatile("" ::: "memory"); }

            float64x2_t r1 = ld_vec(slot);            /* no barrier */
            __asm__ volatile("dmb ish" ::: "memory");
            float64x2_t r2 = ld_vec(slot);            /* after barrier */

            /* r2 must not be OLDER than r1: the writer legally keeps
             * bumping between our two reads, so r2 > r1 is progress,
             * NOT a failure (parent's first-draft lesson); only a
             * lane-wise backwards r2 fails (integer-valued doubles:
             * vcltq_f64 == parent's vcltq_u32 ordering exactly) */
            uint64x2_t back_diff = vcltq_f64(r2, r1);
            if (vmaxvq_u32(vreinterpretq_u32_u64(back_diff)) != 0) {
                report_fail_msg("store_visibility_cross_class_neon_fp64: cpu %d "
                                "barriered read went BACKWARDS (round %lu)",
                                cpu, (unsigned long)round);
            }
            /* regression vs last round: lane values only grow */
            uint64x2_t reg1 = vcltq_f64(r1, last_seen);
            uint64x2_t reg2 = vcltq_f64(r2, last_seen);
            uint64x2_t reg = vorrq_u64(reg1, reg2);
            if (vmaxvq_u32(vreinterpretq_u32_u64(reg)) != 0) {
                report_fail_msg("store_visibility_cross_class_neon_fp64: cpu %d "
                                "vector regressed: last-bits {%016llx,%016llx}, "
                                "r1-bits {%016llx,%016llx}, r2-bits "
                                "{%016llx,%016llx} (precisely-old vector survived)",
                                cpu,
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(last_seen), 0),
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(last_seen), 1),
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(r1), 0),
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(r1), 1),
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(r2), 0),
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(r2), 1));
            }
            last_seen = r2;
        } while (test_time_condition(test));
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(store_visibility_cross_class_neon_fp64, "FP64 variant of store_visibility_cross_class_neon: cross-coherency-domain FP64 store visibility: asm vst/vld f64x2 slots, writer str+dmb ish, readers check pre/post-barrier reads for precisely-old vectors (ARM64)")
    .test_init = store_visibility_cross_class_neon_fp64_init,
    .test_run = store_visibility_cross_class_neon_fp64_run,
    .test_cleanup = store_visibility_cross_class_neon_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
