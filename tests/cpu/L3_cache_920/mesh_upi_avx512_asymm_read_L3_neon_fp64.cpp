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
    alignas(16) double *golden;          /* init-time element-exact golden copy */
    std::atomic<uint64_t> thread_idx;
};
}

/* exact 0.5-multiple in [-2^19, 2^19) */
static inline double marl_dist(void) {
    return (double)((int64_t)(random64() % (1ULL << 21)) - (1 << 20)) * 0.5;
}

static int mesh_upi_avx512_asymm_read_L3_neon_fp64_init(struct test *test) {
    /* throwing-new OOM follows the repo T10 convention (cf.
     * gemm_cdouble_dynamic_square): bad_alloc -> clean SKIP. */
    TestData *td;
    try {
        td = new TestData;
    } catch (const std::bad_alloc &) {
        log_skip(RuntimeSkipCategory, "memory: %s TestData", test->id);
        return EXIT_SKIP;
    }

    td->data = (double *)aligned_alloc(16, ARRAY_DOUBLES * sizeof(double));
    td->golden = (double *)aligned_alloc(16, ARRAY_DOUBLES * sizeof(double));
    if (!td->data || !td->golden) {
        free(td->data); free(td->golden);
        delete td;
        return EXIT_FAILURE;
    }

    for (size_t i = 0; i < ARRAY_DOUBLES; ++i) {
        td->golden[i] = td->data[i] = marl_dist();
    }
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
        /* element-exact L3 verification (2026-11-18 rework): each
         * loaded vector is compared lane-by-lane against the
         * init-time golden copy — a sum-only compare was blind to
         * permutation/swap-class corruption. */
        bool consistent = true;
        alignas(16) double store_buf[BLOCK_DOUBLES];

        for (size_t i = 0; i < ARRAY_DOUBLES; i += BLOCK_DOUBLES) {
            /* load the block as f64x2 vectors, accumulate the sum */
            float64x2_t vecs[VECS_PER_BLOCK];
            for (int v = 0; v < VECS_PER_BLOCK; ++v) {
                vecs[v] = vld1q_f64(td->data + i + v * VEC_LANES);
                float64x2_t gv = vld1q_f64(td->golden + i + v * VEC_LANES);
                uint64x2_t gdiff = veorq_u64(vreinterpretq_u64_f64(vecs[v]),
                                             vreinterpretq_u64_f64(gv));
                if (vmaxvq_u32(vreinterpretq_u32_u64(gdiff)) != 0) {
                    report_fail_msg("%s: golden mismatch at elem %zu: "
                                    "golden (%.17g, %.17g) got (%.17g, %.17g) "
                                    "(L3 read path, element-exact compare)",
                                    test->id, i + v * VEC_LANES,
                                    vgetq_lane_f64(gv, 0), vgetq_lane_f64(gv, 1),
                                    vgetq_lane_f64(vecs[v], 0), vgetq_lane_f64(vecs[v], 1));
                    return EXIT_FAILURE;
                }
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

        if (!consistent) {
            report_fail_msg("%s: block store->reload inconsistency "
                            "(LSU forwarding path)", test->id);
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int mesh_upi_avx512_asymm_read_L3_neon_fp64_finish(struct test *test) {
    auto *td = static_cast<TestData *>(test->data);
    free(td->data);
    free(td->golden);
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
