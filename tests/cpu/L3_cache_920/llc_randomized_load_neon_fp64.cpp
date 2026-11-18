/*  llc_randomized_load_neon_fp64 — FP64 datapath variant of
 *  tests/cpu/L3_cache_920/llc_randomized_load.cpp (2026-11-18
 *  NEON/FP64 expansion completion, user directive).
 *
 *  Same two-role structure on the f64x2 register class:
 *   - data[]: RANDOM-INDEX vld1q_f64 loads over the per-thread window
 *     sized to the PROBED per-cluster L3 (capacity-boundary roaming,
 *     the #31 pressure);
 *   - check[]: the loaded VECTOR stored at the same index and read
 *     back through inline asm "ldr q" (anti-forwarding), bit-compared
 *     with veorq/vmaxvq on the raw u64 lanes.
 *
 *  Data is exact 0.5-multiples (|v| <= 2^20): every partial sum is
 *  exact in f64, so golden/sweep sums are bit-exact (the mesh f64
 *  family's byte-exact discipline).  L3-capability design baked in
 *  from birth: init-time golden vector-sum + periodic DENSE sweep —
 *  window-sized traffic evicts the private L2, so the sweep compare
 *  is L3/DRAM-served.
 */
#include "sandstone.h"
#include "sandstone_p.h"   /* device_count/device_info: L3 size probe */
#include <cstdint>
#include <cstring>
#include <vector>
#include <arm_neon.h>

/* Per-cluster L3 size (same 3-step probe as memcpy_l3_cache_size;
 * MUST be called from test_init — see the memcpy H8' lesson). */
static size_t llrlnf_l3_size(void) {
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

/* exact 0.5-multiple in [-2^19, 2^19): every partial sum exact */
static inline double llrlnf_dist(void) {
    return (double)((int64_t)(random64() % (1ULL << 21)) - (1 << 20)) * 0.5;
}

/* anti-forwarding f64x2 load: "w" constraint pins a NEON register */
static inline float64x2_t ldr_vec_f64(const double *addr) {
    float64x2_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(addr) : "memory");
    return v;
}

static int llc_randomized_load_neon_fp64_init(struct test *test) {
    /* probe HERE — topology is filled by now */
    auto *block = new size_t(llrlnf_l3_size());
    if (!block)
        return EXIT_FAILURE;
    test->data = block;
    return EXIT_SUCCESS;
}

static int llc_randomized_load_neon_fp64_cleanup(struct test *test) {
    delete static_cast<size_t *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int llc_randomized_load_neon_fp64_run(struct test *test, int cpu) {
    (void)cpu;
    const size_t buffer_bytes = *static_cast<size_t *>(test->data);
    const size_t num_vecs = buffer_bytes / 16;

    std::vector<double> data(num_vecs * 2);    /* random-index loads */
    std::vector<double> check(num_vecs * 2);   /* store->load verify */

    for (size_t i = 0; i < num_vecs * 2; ++i)
        data[i] = llrlnf_dist();
    memset(check.data(), 0, buffer_bytes);

    /* init-time golden sum of data[] (never written after; exact
     * 0.5-multiple arithmetic, fixed loop order) */
    double golden_sum = 0.0;
    for (size_t v = 0; v < num_vecs; ++v) {
        float64x2_t x = vld1q_f64(data.data() + v * 2);
        golden_sum += vgetq_lane_f64(x, 0) + vgetq_lane_f64(x, 1);
    }

    do {
        for (uint64_t batch = 0; batch < (1ULL << 20); ++batch) {
            size_t vidx = random64() % num_vecs;
            double *dp = data.data() + vidx * 2;
            double *cp = check.data() + vidx * 2;

            /* randomized LLC f64x2 load — the pressure */
            float64x2_t original = vld1q_f64(dp);

            /* store -> REAL vector load-back through the LSU */
            vst1q_f64(cp, original);
            float64x2_t loaded = ldr_vec_f64(cp);

            uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(original),
                                         vreinterpretq_u64_f64(loaded));
            if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                report_fail_msg("llc_randomized_load_neon_fp64: store->load "
                                "mismatch at vec %zu (LSU forwarding path)",
                                vidx);
                return EXIT_FAILURE;
            }
        }

        /* L3 verification sweep: dense f64x2 re-read of data[] —
         * window-sized traffic evicts the private L2, so the sum
         * compare is L3/DRAM-served. */
        double sweep_sum = 0.0;
        for (size_t v = 0; v < num_vecs; ++v) {
            float64x2_t x = vld1q_f64(data.data() + v * 2);
            sweep_sum += vgetq_lane_f64(x, 0) + vgetq_lane_f64(x, 1);
        }
        if (sweep_sum != golden_sum) {
            report_fail_msg("llc_randomized_load_neon_fp64: golden sweep "
                            "mismatch: init sum %.17g sweep sum %.17g "
                            "(L3-resident data corruption)",
                            golden_sum, sweep_sum);
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(llc_randomized_load_neon_fp64, "FP64 variant of llc_randomized_load: random LLC roaming with vld1q_f64 loads over per-thread windows sized to the probed per-cluster L3; asm ldr-q read-back on the LSU forwarding path PLUS periodic dense golden f64-sum sweep (L3-path verification) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = llc_randomized_load_neon_fp64_init,
    .test_run = llc_randomized_load_neon_fp64_run,
    .test_cleanup = llc_randomized_load_neon_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
