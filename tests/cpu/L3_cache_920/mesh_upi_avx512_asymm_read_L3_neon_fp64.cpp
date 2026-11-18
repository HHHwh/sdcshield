/*  mesh_upi_avx512_asymm_read_L3_neon_fp64 — FP64 datapath variant
 *  of tests/cpu/L3_cache_920/mesh_upi_avx512_asymm_read_L3_int.cpp
 *  (2026-11-18 NEON/FP64 expansion completion, user directive).  The
 *  x86 AVX-512 name is inherited lineage; the datapath is f64x2 NEON.
 *
 *  Shared 4 MB double array (L3-scale residency, single copy shared
 *  by all threads).  Per 16-double block: f64x2 loads + lane-sum,
 *  PLUS a store->reload consistency sub-check on a stack buffer
 *  (the int asymm family's block structure, re-typed to f64 lanes).
 *  Pass-level verification: the accumulated sum vs the init-time
 *  golden — the L3 read-path verification point.
 *
 *  Data is exact 0.5-multiples (|v| <= 2^20): partial sums exact in
 *  f64, golden compare bit-exact (mesh f64 family discipline).
 */
#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <arm_neon.h>

#define ARRAY_DOUBLES (1 << 19)          /* 512K doubles = 4MB shared */
#define BLOCK_DOUBLES 16                 /* 8 f64x2 vectors per block */
#define VEC_LANES 2
#define VECS_PER_BLOCK (BLOCK_DOUBLES / VEC_LANES)

namespace { /* ODR: per-TU TestData */
struct TestData {
    alignas(16) double *data;            /* shared, 16B aligned */
    double golden_sum;                   /* init-time exact sum */
    std::atomic<uint64_t> thread_idx;
};
}

/* exact 0.5-multiple in [-2^19, 2^19) */
static inline double marl_dist(void) {
    return (double)((int64_t)(random64() % (1ULL << 21)) - (1 << 20)) * 0.5;
}

static int mesh_upi_avx512_asymm_read_L3_neon_fp64_init(struct test *test) {
    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;

    td->data = (double *)aligned_alloc(16, ARRAY_DOUBLES * sizeof(double));
    if (!td->data) {
        delete td;
        return EXIT_FAILURE;
    }

    double sum = 0.0;
    for (size_t i = 0; i < ARRAY_DOUBLES; ++i) {
        td->data[i] = marl_dist();
        sum += td->data[i];
    }
    td->golden_sum = sum;
    td->thread_idx.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx512_asymm_read_L3_neon_fp64_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData *>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    (void)id;

    do {
        double local_sum = 0.0;
        bool consistent = true;
        alignas(16) double store_buf[BLOCK_DOUBLES];

        for (size_t i = 0; i < ARRAY_DOUBLES; i += BLOCK_DOUBLES) {
            /* load the block as f64x2 vectors, accumulate the sum */
            float64x2_t vecs[VECS_PER_BLOCK];
            for (int v = 0; v < VECS_PER_BLOCK; ++v) {
                vecs[v] = vld1q_f64(td->data + i + v * VEC_LANES);
                local_sum += vgetq_lane_f64(vecs[v], 0) + vgetq_lane_f64(vecs[v], 1);
            }

            /* block store->reload consistency sub-check (stack buf) */
            for (int v = 0; v < VECS_PER_BLOCK; ++v)
                vst1q_f64(store_buf + v * VEC_LANES, vecs[v]);
            for (int v = 0; v < VECS_PER_BLOCK; ++v) {
                float64x2_t orig = vld1q_f64(td->data + i + v * VEC_LANES);
                float64x2_t reload = vld1q_f64(store_buf + v * VEC_LANES);
                uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(orig),
                                             vreinterpretq_u64_f64(reload));
                if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                    consistent = false;
                    break;
                }
            }
            if (!consistent) break;
        }

        bool sum_ok = (local_sum == td->golden_sum);
        if (!sum_ok || !consistent) {
            report_fail_msg("mesh_upi_avx512_asymm_read_L3_neon_fp64: "
                            "sum mismatch or block inconsistency after "
                            "full-array re-read (L3 read path): golden "
                            "%.17g got %.17g consistent=%d",
                            td->golden_sum, local_sum, (int)consistent);
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int mesh_upi_avx512_asymm_read_L3_neon_fp64_finish(struct test *test) {
    auto *td = static_cast<TestData *>(test->data);
    free(td->data);
    delete td;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

DECLARE_TEST(mesh_upi_avx512_asymm_read_L3_neon_fp64, "FP64 variant of mesh_upi_avx512_asymm_read_L3_int: sequential f64x2 reads over a shared 4MB L3-scale array with per-block store->reload consistency, per-pass golden f64-sum re-verify (L3 read-path SDC) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx512_asymm_read_L3_neon_fp64_init,
    .test_run = mesh_upi_avx512_asymm_read_L3_neon_fp64_run,
    .test_cleanup = mesh_upi_avx512_asymm_read_L3_neon_fp64_finish,
    .fracture_loop_count = -1,   /* never fracture: full-array rounds */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
