/*  mesh_upi_avx2_write_only_neon_fp64 — FP64 datapath variant of
 *  tests/cpu/L3_cache_920/mesh_upi_avx2_write_only_int.cpp (2026-11-18
 *  NEON/FP64 expansion completion, user directive).  The x86 AVX2
 *  name is inherited lineage; the datapath is f64x2 NEON.
 *
 *  Two-pass per-thread design (the 2026-11-17 capability rework's
 *  pattern, born here from the start): pass 1 writes fresh random
 *  0.5-multiples through vst1q_f64 across a 4 MB per-thread double
 *  buffer, accumulating the golden sum; pass 2 re-reads the whole
 *  buffer — early lines are long evicted from the 512 KB private L2
 *  by the 4 MB stream, so the re-read is L3/DRAM-served and the sum
 *  compare is a genuine hierarchy verification point (no
 *  store-buffer forwarding involved).
 *
 *  Data is exact 0.5-multiples: partial sums exact, bit-exact golden.
 */
#include <sandstone.h>
#include <cstdint>
#include <cstdio>
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
    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;
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
    if (!buf) {
        report_fail_msg("mesh_upi_avx2_write_only_neon_fp64: alloc failed");
        return EXIT_FAILURE;
    }

    do {
        /* Pass 1 — write fill via f64x2 stores + golden sum */
        double golden_sum = 0.0;
        for (size_t i = 0; i < PER_THREAD_DOUBLES; i += VEC_LANES) {
            double vals[VEC_LANES];
            for (int l = 0; l < VEC_LANES; ++l)
                vals[l] = mwol_dist();
            float64x2_t v = vld1q_f64(vals);
            vst1q_f64(buf + i, v);
            golden_sum += vals[0] + vals[1];
        }

        /* Pass 2 — delayed full re-read (L3/DRAM-served) + sum compare */
        double reread_sum = 0.0;
        for (size_t i = 0; i < PER_THREAD_DOUBLES; i += VEC_LANES) {
            float64x2_t loaded = vld1q_f64(buf + i);
            reread_sum += vgetq_lane_f64(loaded, 0) + vgetq_lane_f64(loaded, 1);
        }

        if (reread_sum != golden_sum) {
            report_fail_msg("mesh_upi_avx2_write_only_neon_fp64: "
                            "write->delayed-re-read sum mismatch (L3 write "
                            "path): golden %.17g reread %.17g",
                            golden_sum, reread_sum);
            free(buf);
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));

    free(buf);
    return EXIT_SUCCESS;
}

static int mesh_upi_avx2_write_only_neon_fp64_finish(struct test *test) {
    auto *td = static_cast<TestData *>(test->data);
    delete td;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

DECLARE_TEST(mesh_upi_avx2_write_only_neon_fp64, "FP64 variant of mesh_upi_avx2_write_only_int: f64x2 stores over a 4MB per-thread buffer, write-fill then delayed full re-read with golden f64-sum verify (L3 write-path SDC, no store-buffer forwarding) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx2_write_only_neon_fp64_init,
    .test_run = mesh_upi_avx2_write_only_neon_fp64_run,
    .test_cleanup = mesh_upi_avx2_write_only_neon_fp64_finish,
    .fracture_loop_count = -1,   /* never fracture: 8MB-traffic rounds */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
