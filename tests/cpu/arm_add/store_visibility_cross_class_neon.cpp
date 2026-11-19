/*  store_visibility_cross_class_neon — NEON variant of
 *  tests/cpu/arm_add/store_visibility_cross_class.cpp (original
 *  untouched).
 *
 *  Same cross-coherency-domain visibility probe, with the payload on
 *  the 16-byte NEON store path: each reader slot is a uint32x4 vector
 *  counter.  The writer bumps the slot with an ASM vector
 *  load-add-store (asm, not intrinsics — GCC store-transforms and
 *  load-forwards same-address intrinsic pairs; measured today), then
 *  dmb ish; readers read the slot twice (unbarriered, then after
 *  dmb ish) and fail on backwards reads or version regressions — a
 *  precisely-old vector surviving the barrier is the reported
 *  signature, now on the NEON datapath.
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
    alignas(64) uint32_t slots[SVCN_MAX_THREADS][4];
};
}

static inline uint32x4_t ld_vec(const uint32_t *a) {
    uint32x4_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(a) : "memory");
    return v;
}
static inline void st_vec(uint32_t *a, uint32x4_t v) {
    __asm__ volatile("str %q0, [%1]" :: "w"(v), "r"(a) : "memory");
}

static int store_visibility_cross_class_neon_init(struct test *test) {
    auto *d = new SVCNData;
    if (!d) return EXIT_FAILURE;
    for (int i = 0; i < SVCN_MAX_THREADS; ++i)
        for (int l = 0; l < 4; ++l)
            d->slots[i][l] = 0;
    test->data = d;
    return EXIT_SUCCESS;
}

static int store_visibility_cross_class_neon_cleanup(struct test *test) {
    delete static_cast<SVCNData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int store_visibility_cross_class_neon_run(struct test *test, int cpu) {
    auto *d = static_cast<SVCNData *>(test->data);

    if (cpu == 0) {
        /* writer: bump every slot, barrier, same-core read-back */
        uint64_t round = 0;
        do {
            ++round;
            for (int r = 0; r < SVCN_MAX_THREADS; ++r) {
                uint32_t *slot = d->slots[r];
                uint32x4_t v = ld_vec(slot);
                v = vaddq_u32(v, vdupq_n_u32(1));
                st_vec(slot, v);
                __asm__ volatile("dmb ish" ::: "memory");
                /* layer 1: same-core read-back must see v */
                uint32x4_t back = ld_vec(slot);
                uint32x4_t diff = veorq_u32(back, v);
                if (vmaxvq_u32(diff) != 0) {
                    report_fail_msg("store_visibility_cross_class_neon: "
                                    "slot %d same-core read-back differs "
                                    "(round %lu)", r, (unsigned long)round);
                }
            }
        } while (test_time_condition(test));
    } else {
        /* reader: unbarriered read, dmb ish, barriered read */
        uint64_t round = 0;
        uint32x4_t last_seen = vdupq_n_u32(0);
        do {
            ++round;
            uint32_t *slot = d->slots[cpu];
            for (int i = 0; i < 200; ++i) { __asm__ volatile("" ::: "memory"); }

            uint32x4_t r1 = ld_vec(slot);            /* no barrier */
            __asm__ volatile("dmb ish" ::: "memory");
            uint32x4_t r2 = ld_vec(slot);            /* after barrier */

            /* r2 must not be OLDER than r1: the writer legally keeps
             * bumping between our two reads, so r2 > r1 is progress,
             * NOT a failure (the first draft failed on != and broke on
             * healthy cores); only a lane-wise backwards r2 fails */
            uint32x4_t back_diff = vcltq_u32(r2, r1);
            if (vmaxvq_u32(back_diff) != 0) {
                report_fail_msg("store_visibility_cross_class_neon: cpu %d "
                                "barriered read went BACKWARDS (round %lu)",
                                cpu, (unsigned long)round);
            }
            /* regression vs last round: lane values only grow
             * (unsigned lane-wise compare, any hit = fail) */
            uint32x4_t reg1 = vcltq_u32(r1, last_seen);
            uint32x4_t reg2 = vcltq_u32(r2, last_seen);
            uint32x4_t reg = vorrq_u32(reg1, reg2);
            if (vmaxvq_u32(reg) != 0) {
                report_fail_msg("store_visibility_cross_class_neon: cpu %d "
                                "vector regressed: last {%u,%u,%u,%u}, r1 "
                                "{%u,%u,%u,%u}, r2 {%u,%u,%u,%u} "
                                "(precisely-old vector survived)", cpu,
                                vgetq_lane_u32(last_seen, 0), vgetq_lane_u32(last_seen, 1),
                                vgetq_lane_u32(last_seen, 2), vgetq_lane_u32(last_seen, 3),
                                vgetq_lane_u32(r1, 0), vgetq_lane_u32(r1, 1),
                                vgetq_lane_u32(r1, 2), vgetq_lane_u32(r1, 3),
                                vgetq_lane_u32(r2, 0), vgetq_lane_u32(r2, 1),
                                vgetq_lane_u32(r2, 2), vgetq_lane_u32(r2, 3));
            }
            last_seen = r2;
        } while (test_time_condition(test));
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(store_visibility_cross_class_neon, "Cross-coherency-domain NEON store visibility: asm vst/vld vector slots, writer str+dmb ish, readers check pre/post-barrier reads for precisely-old vectors (ARM64)")
    .test_init = store_visibility_cross_class_neon_init,
    .test_run = store_visibility_cross_class_neon_run,
    .test_cleanup = store_visibility_cross_class_neon_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
