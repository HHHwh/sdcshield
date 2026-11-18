/*  l3_execute_neon_fp64 — FP64 data-path variant of
 *  tests/cpu/L3_cache_920/l3_execute_neon.cpp (2026-11-10; parents
 *  moved into L3_cache_920 by the 2026-11-18 unification).
 *
 *  Same fetch-path probe structure — the 2026-11-17 capability
 *  rework's dc civac pass (clean+invalidate to the PoC, evicting the
 *  code lines from the private L2) followed by ic ivau + dsb ish +
 *  isb resync, so the re-fetch is genuinely L3-served; executed
 *  results bit-compared to the INIT-TIME snapshot —
 *  but the worker's compute kernel is the FP64 vector pipeline: a
 *  fixed vmulq/vaddq/vsubq chain over float64x2, so the code being
 *  re-fetched is FP64-heavy machine code (the fetch corruption of an
 *  FP64 instruction word is what this variant would surface).
 *
 *  Inputs are mild deterministic integers-as-doubles (i*7 + l*13 + 1,
 *  all < 1000) and the chain constants are mild (no overflow to
 *  Inf/NaN saturation in 8 steps), so the recompute stays in the
 *  finite domain — a corrupted fetch shows as a plain bit mismatch.
 *  Golden mechanism is the parent's: init calls the same worker
 *  (same-source cross-time recompute), single worker function (no
 *  ICF hazard by construction), bit-compared via veorq_u64.
 */
#include "sandstone.h"
#include <cstdint>
#include <cstring>
#include <arm_neon.h>

static constexpr int    L3EN_NUM_INPUTS = 64;
static constexpr size_t L3EN_FLUSH_BYTES = 256;
static constexpr size_t L3EN_LINE = 64;

/* The function under test: fixed FP64 compute chain, noinline
 * so every call is a real fetch+execute of THIS code. */
static __attribute__((noinline)) float64x2_t l3en_worker_f64(float64x2_t v, uint32_t round) {
    const float64x2_t k1 = vdupq_n_f64(1.0000001);
    const float64x2_t k2 = vdupq_n_f64(1.5);
    float64x2_t a = vsubq_f64(v, k1);
    float64x2_t b = vaddq_f64(v, vdupq_n_f64((double)round));
    for (int i = 0; i < 8; ++i) {
        a = vaddq_f64(a, vmulq_f64(b, k1));
        b = vsubq_f64(b, a);
    }
    return vmulq_f64(a, vsubq_f64(b, k2));
}

namespace { /* ODR: per-TU L3ENData */
struct L3ENData {
    double in[L3EN_NUM_INPUTS][2];         /* deterministic vector inputs */
    double golden[L3EN_NUM_INPUTS][2];     /* init-time bit snapshot */
};
}

/* 2026-11-18 L3-capability rework (twin of the scalar parent's): the
 * dc civac pass (clean+invalidate to the PoC) EVICTS the code lines
 * from the private unified L2 first — bare ic ivau only reaches the
 * PoU = L2 and the refetch stayed L2-served.  After civac + ivau the
 * next fetch misses L1I AND L2 and is genuinely L3-served. */
static inline void l3en_flush_icache(const void *start, size_t len) {
    const char *base = reinterpret_cast<const char *>(
        reinterpret_cast<uintptr_t>(start) & ~(uintptr_t)(L3EN_LINE - 1));
    const char *end = reinterpret_cast<const char *>(start) + len;
    for (const char *p = base; p < end; p += L3EN_LINE)
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (const char *p = base; p < end; p += L3EN_LINE)
        __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

static int l3_execute_neon_fp64_init(struct test *test) {
    /* throwing-new OOM follows the repo T10 convention (cf.
     * gemm_cdouble_dynamic_square): bad_alloc -> clean SKIP. */
    L3ENData *d;
    try {
        d = new L3ENData;
    } catch (const std::bad_alloc &) {
        log_skip(RuntimeSkipCategory, "memory: %s L3ENData", test->id);
        return EXIT_SKIP;
    }
    for (int i = 0; i < L3EN_NUM_INPUTS; ++i) {
        for (int l = 0; l < 2; ++l)
            d->in[i][l] = (double)(i * 7 + l * 13 + 1);   /* mild integers */
        float64x2_t v = vld1q_f64(d->in[i]);
        float64x2_t r = l3en_worker_f64(v, (uint32_t)i);
        vst1q_f64(d->golden[i], r);
    }
    test->data = d;
    return EXIT_SUCCESS;
}

static int l3_execute_neon_fp64_cleanup(struct test *test) {
    delete static_cast<L3ENData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int l3_execute_neon_fp64_run(struct test *test, int cpu) {
    (void)cpu;
    auto *d = static_cast<L3ENData *>(test->data);

    do {
        /* force the next call to re-fetch the FP64 code from L2/L3 */
        l3en_flush_icache(reinterpret_cast<const void *>(&l3en_worker_f64),
                          L3EN_FLUSH_BYTES);

        for (int i = 0; i < L3EN_NUM_INPUTS; ++i) {
            float64x2_t v = vld1q_f64(d->in[i]);
            float64x2_t r = l3en_worker_f64(v, (uint32_t)i);

            float64x2_t g = vld1q_f64(d->golden[i]);
            uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(r),
                                        vreinterpretq_u64_f64(g));
            if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                uint64_t out[2], gw[2];
                vst1q_u64(out, vreinterpretq_u64_f64(r));
                vst1q_u64(gw, vreinterpretq_u64_f64(g));
                report_fail_msg("l3_execute_neon_fp64: FP64 execution differs "
                                "from init snapshot after I-cache reload: "
                                "input %d: expected-bits {%016llx,%016llx} "
                                "got-bits {%016llx,%016llx} (fetch-path "
                                "corruption of FP64 code?)", i,
                                (unsigned long long)gw[0], (unsigned long long)gw[1],
                                (unsigned long long)out[0], (unsigned long long)out[1]);
                return EXIT_FAILURE;
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(l3_execute_neon_fp64, "FP64 variant of l3_execute_neon: reads and executes FP64 code from L3: dc-civac evicts the code lines from L2 (to PoC) before ic-ivau + dsb/isb, so the re-fetch of the vmulq/vaddq/vsubq_f64 kernel is genuinely L3-served; results bit-compared to the init snapshot (L3->ICache fetch-path SDC on FP64 code, 2026-11-18 rework) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = l3_execute_neon_fp64_init,
    .test_run = l3_execute_neon_fp64_run,
    .test_cleanup = l3_execute_neon_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
