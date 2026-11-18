/*  mesh_upi_sse_read_only_neon_fp64 — FP64 datapath variant of
 *  tests/cpu/L3_cache_920/mesh_upi_sse_read_only_int.cpp (2026-11-18
 *  NEON/FP64 expansion completion, user directive).  The x86 AVX2
 *  name is inherited lineage; the datapath is f64x2 NEON.
 *
 *  Shared 4 MB double array (∈ (L2 512KB, 簇 L3 24MB) → steady-state
 *  L3 residency, single copy shared by all threads — residency does
 *  not degrade with thread count).  Every pass re-reads the whole
 *  array with vld1q_f64 and re-sums against the init-time golden —
 *  the L3 read-path verification point.
 *
 *  Data is exact 0.5-multiples (|v| <= 2^20): every partial sum is
 *  exact in f64, golden compare bit-exact (mesh f64 family
 *  discipline, cf. mesh_upi_avx512_symm_f64).
 */
#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <arm_neon.h>

#define ARRAY_DOUBLES (1 << 19)          /* 512K doubles = 4MB shared */
#define VEC_LANES 2                      /* f64x2 per load */

namespace { /* ODR: per-TU TestData */
struct TestData {
    alignas(16) double *data;            /* shared, 16B aligned */
    double golden_sum;                   /* init-time exact sum */
    std::atomic<uint64_t> thread_idx;
};
}

/* exact 0.5-multiple in [-2^19, 2^19) */
static inline double mrl_dist(void) {
    return (double)((int64_t)(random64() % (1ULL << 21)) - (1 << 20)) * 0.5;
}

static int mesh_upi_sse_read_only_neon_fp64_init(struct test *test) {
    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;

    td->data = (double *)aligned_alloc(16, ARRAY_DOUBLES * sizeof(double));
    if (!td->data) {
        delete td;
        return EXIT_FAILURE;
    }

    double sum = 0.0;
    for (size_t i = 0; i < ARRAY_DOUBLES; ++i) {
        td->data[i] = mrl_dist();
        sum += td->data[i];
    }
    td->golden_sum = sum;
    td->thread_idx.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_sse_read_only_neon_fp64_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData *>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    (void)id;

    do {
        double local_sum = 0.0;

        /* sequential f64x2 reads over the shared 4MB array —
         * L3-resident steady state, sum vs golden is the L3
         * read-path verification */
        for (size_t i = 0; i < ARRAY_DOUBLES; i += VEC_LANES) {
            float64x2_t v = vld1q_f64(td->data + i);
            local_sum += vgetq_lane_f64(v, 0) + vgetq_lane_f64(v, 1);
        }

        if (local_sum != td->golden_sum) {
            report_fail_msg("mesh_upi_sse_read_only_neon_fp64: sum mismatch "
                            "after full-array re-read (L3 read path): "
                            "golden %.17g got %.17g",
                            td->golden_sum, local_sum);
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int mesh_upi_sse_read_only_neon_fp64_finish(struct test *test) {
    auto *td = static_cast<TestData *>(test->data);
    free(td->data);
    delete td;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

DECLARE_TEST(mesh_upi_sse_read_only_neon_fp64, "FP64 variant of mesh_upi_sse_read_only_int: 100% sequential f64x2 reads over a shared 4MB L3-scale array, per-pass golden f64-sum re-verify (L3 read-path SDC) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sse_read_only_neon_fp64_init,
    .test_run = mesh_upi_sse_read_only_neon_fp64_run,
    .test_cleanup = mesh_upi_sse_read_only_neon_fp64_finish,
    .fracture_loop_count = -1,   /* never fracture: full-array rounds */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
