/*  mesh_upi_avx2_write_only_neon_fp64 — FP64 datapath variant of
 *  tests/cpu/L3_cache_920/mesh_upi_avx2_write_only_int.cpp (2026-11-18
 *  NEON/FP64 expansion completion, user directive).  The x86 AVX2
 *  name is inherited lineage; the datapath is f64x2 NEON.
 *
 *  Two-pass per-thread design (the 2026-11-17 capability rework's
 *  pattern, born here from the start): pass 1 writes fresh random
 *  0.5-multiples through vst1q_f64 across a 4 MB per-thread double
 *  buffer, doubled into a golden copy; pass 2 re-reads the whole
 *  buffer — early lines are long evicted from the 512 KB private L2
 *  by the 4 MB stream, so the re-read is L3/DRAM-served and the
 *  element-exact compare against the golden copy is a genuine
 *  hierarchy verification point (no store-buffer forwarding
 *  involved).  2026-11-18 element-exact rework: the previous
 *  sum-only compare was blind to permutation/swap-class write-path
 *  damage; the per-lane compare is not.
 */
#include <sandstone.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <arm_neon.h>

#define PER_THREAD_DOUBLES (1 << 19)     /* 512K doubles = 4MB/thread */
#define VEC_LANES 2

namespace { /* ODR: per-TU TestData */
struct TestData {
    std::atomic<uint64_t> thread_idx;
};
}

/* exact 0.5-multiple in [-2^19, 2^19) */
static inline double mwol_dist(void) {
    return (double)((int64_t)(random64() % (1ULL << 21)) - (1 << 20)) * 0.5;
}

static int mesh_upi_avx2_write_only_neon_fp64_init(struct test *test) {
    /* throwing-new OOM follows the repo T10 convention (cf.
     * gemm_cdouble_dynamic_square): bad_alloc -> clean SKIP. */
    TestData *td;
    try {
        td = new TestData;
    } catch (const std::bad_alloc &) {
        log_skip(RuntimeSkipCategory, "memory: %s TestData", test->id);
        return EXIT_SKIP;
    }
    td->thread_idx.store(0, std::memory_order_relaxed);
    test->data = td;
    return EXIT_SUCCESS;
}

static int mesh_upi_avx2_write_only_neon_fp64_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData *>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    (void)id;

    alignas(16) double *buf =
        (double *)aligned_alloc(16, PER_THREAD_DOUBLES * sizeof(double));
    alignas(16) double *golden =
        (double *)aligned_alloc(16, PER_THREAD_DOUBLES * sizeof(double));
    if (!buf || !golden) {
        free(buf); free(golden);
        report_fail_msg("%s: alloc failed", test->id);
        return EXIT_FAILURE;
    }

    do {
        /* Pass 1 — write fill via f64x2 stores, doubled into golden */
        for (size_t i = 0; i < PER_THREAD_DOUBLES; i += VEC_LANES) {
            double vals[VEC_LANES];
            for (int l = 0; l < VEC_LANES; ++l)
                vals[l] = mwol_dist();
            float64x2_t v = vld1q_f64(vals);
            vst1q_f64(buf + i, v);
            vst1q_f64(golden + i, v);
        }

        /* Pass 2 — delayed full re-read (L3/DRAM-served), element-exact
         * per-lane compare against the golden copy */
        for (size_t i = 0; i < PER_THREAD_DOUBLES; i += VEC_LANES) {
            float64x2_t loaded = vld1q_f64(buf + i);
            float64x2_t g = vld1q_f64(golden + i);
            uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(loaded),
                                        vreinterpretq_u64_f64(g));
            if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                report_fail_msg("%s: write->delayed-re-read golden "
                                "mismatch at pair %zu: golden (%.17g, "
                                "%.17g) got (%.17g, %.17g) (L3 write path, "
                                "element-exact compare)",
                                test->id, i / VEC_LANES,
                                vgetq_lane_f64(g, 0), vgetq_lane_f64(g, 1),
                                vgetq_lane_f64(loaded, 0), vgetq_lane_f64(loaded, 1));
                free(buf); free(golden);
                return EXIT_FAILURE;
            }
        }
    } while (test_time_condition(test));

    free(buf);
    free(golden);
    return EXIT_SUCCESS;
}

static int mesh_upi_avx2_write_only_neon_fp64_finish(struct test *test) {
    auto *td = static_cast<TestData *>(test->data);
    delete td;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

DECLARE_TEST(mesh_upi_avx2_write_only_neon_fp64, "FP64 variant of mesh_upi_avx2_write_only_int: f64x2 stores over a 4MB per-thread buffer, write-fill then delayed full re-read with element-exact golden verify (L3 write-path SDC, no store-buffer forwarding) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx2_write_only_neon_fp64_init,
    .test_run = mesh_upi_avx2_write_only_neon_fp64_run,
    .test_cleanup = mesh_upi_avx2_write_only_neon_fp64_finish,
    .fracture_loop_count = -1,   /* never fracture: 8MB-traffic rounds */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
