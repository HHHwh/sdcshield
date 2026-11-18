#include <sandstone.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#ifdef __aarch64__
#include <arm_neon.h>
#include <unistd.h>

#define ARRAY_SIZE (1 << 20)           // 1M 个 int32 = 4MB
#define VECTOR_SIZE 4                  // NEON 每次加载 4 个 int32（128 位）

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    alignas(16) int32_t *data;         // 共享只读数据（16 字节对齐）
    alignas(16) int32_t *golden;       // init-time element-exact golden copy
    std::atomic<uint64_t> thread_idx;  // 用于线程 ID 分配
};
}

static int mesh_upi_avx2_read_only_int_init(struct test *test) {
    /* throwing-new OOM follows the repo T10 convention (cf.
     * gemm_cdouble_dynamic_square): bad_alloc -> clean SKIP. */
    TestData *td;
    try {
        td = new TestData;
    } catch (const std::bad_alloc &) {
        log_skip(RuntimeSkipCategory, "memory: %s TestData", test->id);
        return EXIT_SKIP;
    }

    td->data = (int32_t*)aligned_alloc(16, ARRAY_SIZE * sizeof(int32_t));
    td->golden = (int32_t*)aligned_alloc(16, ARRAY_SIZE * sizeof(int32_t));
    if (!td->data || !td->golden) {
        free(td->data); free(td->golden);
        delete td;
        return EXIT_FAILURE;
    }

    // 生成随机 int32 数据并计算参考累加和
    /* randomization hardening H14' (P17): framework RNG (per-thread
     * stream, -s reproducible) replaces std::mt19937; range [-1000000, 1000000). */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };
    for (size_t i = 0; i < ARRAY_SIZE; ++i) {
        td->golden[i] = td->data[i] = dist();
    }
    td->thread_idx.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx2_read_only_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    (void)id;   /* diagnostics only; the hot path carries no I/O */

    do {
        /* element-exact L3 verification (2026-11-18 rework): every
         * loaded vector is compared lane-by-lane against the init-time
         * golden copy — a sum-only compare was blind to permutation/
         * swap-class corruption; the per-lane compare is not. */
        for (size_t i = 0; i < ARRAY_SIZE; i += VECTOR_SIZE) {
            int32x4_t v = vld1q_s32(td->data + i);
            int32x4_t g = vld1q_s32(td->golden + i);
            uint32x4_t diff = veorq_u32(vreinterpretq_u32_s32(v),
                                        vreinterpretq_u32_s32(g));
            if (vmaxvq_u32(diff) != 0) {
                report_fail_msg("%s: golden "
                                "mismatch at vec %zu: golden {%d,%d,%d,%d} "
                                "got {%d,%d,%d,%d} (L3-resident data "
                                "corruption, element-exact compare)",
                                test->id, i / VECTOR_SIZE,
                                vgetq_lane_s32(g, 0), vgetq_lane_s32(g, 1),
                                vgetq_lane_s32(g, 2), vgetq_lane_s32(g, 3),
                                vgetq_lane_s32(v, 0), vgetq_lane_s32(v, 1),
                                vgetq_lane_s32(v, 2), vgetq_lane_s32(v, 3));
                return EXIT_FAILURE;
            }
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;

}

static int mesh_upi_avx2_read_only_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    free(td->golden);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx2_read_only_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx2_read_only_int");
    return EXIT_SKIP;
}
static int mesh_upi_avx2_read_only_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx2_read_only_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx2_read_only_int, "Mutual read stress MESH and UPI or Ring Interconnect and QPI (do int32 NEON loads from L1/L2/L3)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx2_read_only_int_init,
    .test_run = mesh_upi_avx2_read_only_int_run,
    .test_cleanup = mesh_upi_avx2_read_only_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
