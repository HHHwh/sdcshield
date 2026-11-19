/*  cachebounce_neon_fp64 — FP64 data-path variant of tests/cpu/arm_add/cachebounce_neon.cpp.
 *
 *  Parent: all cores RMW a shared 64-byte bouncing line — each core holds
 *  one uint32x4 slot, vld1q + vaddq(+1) + vst1q (asm), register golden.
 *  This variant keeps the false-sharing chassis EXACTLY (same 64B line,
 *  same slot-per-core layout, same asm ldr/str) and re-types ONLY the
 *  datapath: the slot is a float64x2 (2 f64 lanes in the same 16 bytes),
 *  the RMW add is vaddq_f64(+1.0) — FP pipe / FP register file.
 *
 *  Golden independence is the parent's: the shared slot IS the golden
 *  (each core's slot counts that core's own iterations), the register
 *  local is the second independent copy; cleanup compares bit-exact
 *  (veorq_u64 on the f64 bits, u32-view across reduce).  Iteration
 *  counts are integers far below 2^53, so f64 +1.0 accumulation is
 *  exact — the parent's mod-2^32 closure becomes plain exact-domain
 *  equality (no wrap below 2^53).
 *
 *  Parent untouched; x86-64 untouched (aarch64-only registration).
 */
#include "sandstone.h"
#include <cstdint>
#include <arm_neon.h>

#define CBN_MAX_THREADS 1024
#define CBN_LINE_U32X4  4        /* 16B slots per 64B line (f64x2 = same 16B) */

namespace { /* ODR: per-TU CBNData */
struct CBNData {
    /* shared bouncing window: slot i belongs to core i */
    alignas(64) double slots[CBN_MAX_THREADS * 2];
};
}

static inline float64x2_t cbn_ld_vec(const double *a) {
    float64x2_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(a) : "memory");
    return v;
}
static inline void cbn_st_vec(double *a, float64x2_t v) {
    __asm__ volatile("str %q0, [%1]" :: "w"(v), "r"(a) : "memory");
}

static int cachebounce_neon_fp64_init(struct test *test) {
    auto *d = new CBNData;
    if (!d) return EXIT_FAILURE;
    for (int i = 0; i < CBN_MAX_THREADS * 2; ++i)
        d->slots[i] = 0.0;
    test->data = d;
    return EXIT_SUCCESS;
}

static int cachebounce_neon_fp64_cleanup(struct test *test) {
    auto *d = static_cast<CBNData *>(test->data);
    delete d;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int cachebounce_neon_fp64_run(struct test *test, int cpu) {
    auto *d = static_cast<CBNData *>(test->data);
    double *slot = &d->slots[cpu * 2];         /* this core's f64x2 */

    float64x2_t local = vdupq_n_f64(0.0);       /* register golden */

    do {
        /* FP64 RMW on a line shared by every core — slot accesses
         * via asm (intrinsic store->load pairs get GCC-forwarded,
         * silently degrading the verify to constant-true) */
        float64x2_t v = cbn_ld_vec(slot);
        v = vaddq_f64(v, vdupq_n_f64(1.0));
        cbn_st_vec(slot, v);
        local = vaddq_f64(local, vdupq_n_f64(1.0));
    } while (test_time_condition(test));

    /* the shared slot must hold exactly this core's own iterations
     * (bit-exact: integer-valued f64 lanes) */
    float64x2_t mem = cbn_ld_vec(slot);
    uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(mem),
                                vreinterpretq_u64_f64(local));
    if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
        report_fail_msg("cachebounce_neon_fp64: cpu %d slot mismatch: "
                        "mem-bits {%016llx,%016llx} vs local-bits {%016llx,%016llx} "
                        "(lost or corrupted FP64 store on the shared "
                        "bouncing line)", cpu,
                        (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(mem), 0),
                        (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(mem), 1),
                        (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(local), 0),
                        (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(local), 1));
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(cachebounce_neon_fp64, "FP64 variant of cachebounce_neon: per-core f64x2 slots in a shared 64B bouncing line, asm ldr/vaddq_f64/str RMW with register golden (ARM64)")
    .test_init = cachebounce_neon_fp64_init,
    .test_run = cachebounce_neon_fp64_run,
    .test_cleanup = cachebounce_neon_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
