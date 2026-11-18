#include <sandstone.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#ifdef __aarch64__
#include <arm_neon.h>
#include <unistd.h>

#define ARRAY_ELEMS (1 << 20)           // 1M 个 int32 = 4MB（大于 L2，小于典型 L3）
#define NEON_VECTOR_SIZE 4              // NEON 一次处理 4 个 int32
#define VECTOR_SIZE 16                  // 逻辑块大小 16（由 4 个 NEON 向量组成）
#define VECTORS_PER_BLOCK (VECTOR_SIZE / NEON_VECTOR_SIZE)  // = 4
#define TOTAL_BYTES (ARRAY_ELEMS * sizeof(int32_t))

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    alignas(16) int32_t *data;          // 共享只读数据，16 字节对齐
    alignas(16) int32_t *golden;        // init-time element-exact golden copy
    std::atomic<uint32_t> thread_idx;   // 用于线程 ID 分配
};
}

static int mesh_upi_avx512_asymm_read_only_int_init(struct test *test) {
    /* throwing-new OOM follows the repo T10 convention (cf.
     * gemm_cdouble_dynamic_square): bad_alloc -> clean SKIP. */
    TestData *td;
    try {
        td = new TestData;
    } catch (const std::bad_alloc &) {
        log_skip(RuntimeSkipCategory, "memory: %s TestData", test->id);
        return EXIT_SKIP;
    }

    td->data = (int32_t*)aligned_alloc(16, TOTAL_BYTES);
    td->golden = (int32_t*)aligned_alloc(16, TOTAL_BYTES);
    if (!td->data || !td->golden) {
        free(td->data); free(td->golden);
        delete td;
        return EXIT_FAILURE;
    }

    /* randomization hardening H14' (P17): framework RNG (per-thread
     * stream, -s reproducible) replaces std::mt19937; range [-1000000, 1000000). */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };
    for (size_t i = 0; i < ARRAY_ELEMS; ++i) {
        td->golden[i] = td->data[i] = dist();
    }
    td->thread_idx.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx512_asymm_read_only_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    (void)td->thread_idx.fetch_add(1, std::memory_order_relaxed);   /* diagnostics only */

    // 每个线程独立的临时缓冲区，用于一致性测试（Store/Load）
    alignas(16) int32_t store_buf[VECTOR_SIZE];

    do {
        bool consistent = true;

        // 顺序读取整个数组，每次处理 16 个 int32（4 个 NEON 向量）
        for (size_t i = 0; i < ARRAY_ELEMS; i += VECTOR_SIZE) {
            // 加载 16 个元素到临时数组
            int32_t vals[VECTOR_SIZE];
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                int32x4_t vec = vld1q_s32(td->data + i + v * NEON_VECTOR_SIZE);
                /* element-exact L3 verification (2026-11-18 rework):
                 * each loaded vector compared lane-by-lane against the
                 * init-time golden copy — a sum-only compare was blind
                 * to permutation/swap-class corruption. */
                int32x4_t gv = vld1q_s32(td->golden + i + v * NEON_VECTOR_SIZE);
                uint32x4_t gdiff = veorq_u32(vreinterpretq_u32_s32(vec),
                                             vreinterpretq_u32_s32(gv));
                if (vmaxvq_u32(gdiff) != 0) {
                    report_fail_msg("%s: golden mismatch at elem %zu: "
                                    "golden {%d,%d,%d,%d} got {%d,%d,%d,%d} "
                                    "(L3-resident data corruption, "
                                    "element-exact compare)",
                                    test->id, i + v * NEON_VECTOR_SIZE,
                                    vgetq_lane_s32(gv, 0), vgetq_lane_s32(gv, 1),
                                    vgetq_lane_s32(gv, 2), vgetq_lane_s32(gv, 3),
                                    vgetq_lane_s32(vec, 0), vgetq_lane_s32(vec, 1),
                                    vgetq_lane_s32(vec, 2), vgetq_lane_s32(vec, 3));
                    return EXIT_FAILURE;
                }
                vst1q_s32(vals + v * NEON_VECTOR_SIZE, vec);
            }

            // ----- 一致性测试：Store/Load 比较 -----
            // 将 16 个元素存储到临时缓冲区
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                int32x4_t orig = vld1q_s32(vals + v * NEON_VECTOR_SIZE);
                vst1q_s32(store_buf + v * NEON_VECTOR_SIZE, orig);
            }
            // 重新加载并比较
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                int32x4_t orig = vld1q_s32(vals + v * NEON_VECTOR_SIZE);
                int32x4_t reload = vld1q_s32(store_buf + v * NEON_VECTOR_SIZE);
                uint32x4_t cmp = vceqq_s32(orig, reload);
                // 检查所有 4 个元素是否相等
                if (!(vgetq_lane_u32(cmp, 0) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 1) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 2) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 3) == 0xFFFFFFFF)) {
                    consistent = false;
                    break;
                }
            }
            if (!consistent) break;
        }

        if (!consistent) {
            report_fail_msg("%s: store->reload consistency failure "
                            "(LSU forwarding path)", test->id);
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;

}

static int mesh_upi_avx512_asymm_read_only_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    free(td->golden);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx512_asymm_read_only_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx512_asymm_read_only_int");
    return EXIT_SKIP;
}
static int mesh_upi_avx512_asymm_read_only_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx512_asymm_read_only_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx512_asymm_read_only_int,
             "Mutual read stress MESH and UPI (do NEON loads from L1/L2/L3)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx512_asymm_read_only_int_init,
    .test_run = mesh_upi_avx512_asymm_read_only_int_run,
    .test_cleanup = mesh_upi_avx512_asymm_read_only_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
