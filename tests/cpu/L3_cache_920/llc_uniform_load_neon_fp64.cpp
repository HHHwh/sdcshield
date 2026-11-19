/*  llc_uniform_load_neon_fp64 — FP64 datapath variant of
 *  tests/cpu/L3_cache_920/llc_uniform_load.cpp (2026-11-18
 *  NEON/FP64 expansion completion, user directive).
 *
 *  The uniform stride becomes a VECTOR stride: one f64x2 load every
 *  128 bytes across the per-thread window sized to the PROBED
 *  per-cluster L3 (prefetcher stress, the #32 pressure), walk start
 *  rotating by one vector per pass.  Verification: asm "ldr q"
 *  read-back bit-compared on the raw u64 lanes, PLUS the per-pass
 *  DENSE golden f64-sum sweep (the stride walk covers only 1/16 of
 *  the window; the sweep covers all of it).
 *
 *  Data is exact 0.5-multiples: partial sums exact, bit-exact
 *  golden compare (mesh f64 family discipline).
 */
#include "sandstone.h"
#include "sandstone_p.h"   /* device_count/device_info: L3 size probe */
#include <cstdint>
#include <cstring>
#include <vector>
#include <arm_neon.h>

/* Per-cluster L3 size (same 3-step probe as memcpy_l3_cache_size;
 * MUST be called from test_init — see the memcpy H8' lesson). */
static size_t llulnf_l3_size(void) {
    int ndev = device_count();
    for (int i = 0; i < ndev; ++i) {
        int sz = device_info[i].cache[2].cache_data;   /* cache[2] = L3 */
        if (sz > 0)
            return static_cast<size_t>(sz);
    }
    long size = sysconf(_SC_LEVEL3_CACHE_SIZE);
    if (size > 0) return static_cast<size_t>(size);
    return 8 * 1024 * 1024;
}

/* exact 0.5-multiple in [-2^19, 2^19) */
static inline double llulnf_dist(void) {
    return (double)((int64_t)(random64() % (1ULL << 21)) - (1 << 20)) * 0.5;
}

/* anti-forwarding f64x2 load */
static inline float64x2_t ldr_vec_f64(const double *addr) {
    float64x2_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(addr) : "memory");
    return v;
}

static constexpr size_t LLULNF_STRIDE_VECS = 128 / 16;   /* one f64x2 per 128B */

static int llc_uniform_load_neon_fp64_init(struct test *test) {
    /* throwing-new OOM follows the repo T10 convention (cf.
     * gemm_cdouble_dynamic_square): bad_alloc -> clean SKIP. */
    try {
        test->data = new size_t(llulnf_l3_size());
    } catch (const std::bad_alloc &) {
        log_skip(RuntimeSkipCategory, "memory: %s l3-size word", test->id);
        return EXIT_SKIP;
    }
    return EXIT_SUCCESS;
}

static int llc_uniform_load_neon_fp64_cleanup(struct test *test) {
    delete static_cast<size_t *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int llc_uniform_load_neon_fp64_run(struct test *test, int cpu) {
    (void)cpu;
    const size_t buffer_bytes = *static_cast<size_t *>(test->data);
    const size_t num_vecs = buffer_bytes / 16;

    std::vector<double> data(num_vecs * 2);
    std::vector<double> golden(num_vecs * 2);
    std::vector<double> check(num_vecs * 2);

    for (size_t i = 0; i < num_vecs * 2; ++i)
        data[i] = llulnf_dist();
    memcpy(golden.data(), data.data(), buffer_bytes);
    memset(check.data(), 0, buffer_bytes);

    size_t offset = random64() % LLULNF_STRIDE_VECS;

    do {
        for (size_t v = offset; v < num_vecs; v += LLULNF_STRIDE_VECS) {
            double *dp = data.data() + v * 2;
            double *cp = check.data() + v * 2;

            /* uniform strided f64x2 load — prefetcher stress */
            float64x2_t original = vld1q_f64(dp);

            /* store -> REAL vector load-back through the LSU */
            vst1q_f64(cp, original);
            float64x2_t loaded = ldr_vec_f64(cp);

            uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(original),
                                         vreinterpretq_u64_f64(loaded));
            if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                report_fail_msg("llc_uniform_load_neon_fp64: store->load "
                                "mismatch at vec %zu (LSU forwarding path "
                                "under strided access)", v);
                return EXIT_FAILURE;
            }
        }
        offset = (offset + 1) % LLULNF_STRIDE_VECS;

        /* L3 verification sweep (2026-11-18 element-exact rework):
         * dense f64x2 re-read, each lane compared bit-exact against
         * the init-time golden copy — window-sized traffic evicts the
         * private L2, so the compare is L3/DRAM-served. Element-exact
         * (not a sum): permutation/swap-class L3 corruption and a
         * wrong-index load returning a different legal vector both
         * FAIL here where a sum-only check is order-blind. */
        for (size_t v = 0; v < num_vecs; ++v) {
            float64x2_t x = vld1q_f64(data.data() + v * 2);
            float64x2_t g = vld1q_f64(golden.data() + v * 2);
            uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(x),
                                        vreinterpretq_u64_f64(g));
            if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                report_fail_msg("%s: golden sweep mismatch at vec %zu: "
                                "golden (%.17g, %.17g) got (%.17g, %.17g) "
                                "(L3-resident data corruption, "
                                "element-exact compare)",
                                test->id, v,
                                vgetq_lane_f64(g, 0), vgetq_lane_f64(g, 1),
                                vgetq_lane_f64(x, 0), vgetq_lane_f64(x, 1));
                return EXIT_FAILURE;
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(llc_uniform_load_neon_fp64, "FP64 variant of llc_uniform_load: uniform 128B-strided vld1q_f64 loads over per-thread windows sized to the probed per-cluster L3 (prefetcher stress); asm ldr-q read-back PLUS per-pass dense element-exact golden sweep (L3-path verification) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = llc_uniform_load_neon_fp64_init,
    .test_run = llc_uniform_load_neon_fp64_run,
    .test_cleanup = llc_uniform_load_neon_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
