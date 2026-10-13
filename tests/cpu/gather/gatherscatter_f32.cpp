#include <sandstone.h>
#include <cstdint>
#include <cstring>
#ifdef __aarch64__
#include <arm_neon.h>          // NEON intrinsics (aarch64 only)
#endif

static constexpr int VECTOR_SIZE = 16;          // 16 个单精度浮点数
static constexpr int DATA_SIZE = 1024;          // 源/目标数据大小

static int gatherscatter_f32_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int gatherscatter_f32_run(struct test *test, int cpu) {
    (void)cpu;
    // 每个线程独立分配数据
    alignas(16) float src[DATA_SIZE];
    alignas(16) float dst[DATA_SIZE];
    int indices[VECTOR_SIZE];

    auto float_dist = []() { return frandomf_scale((float)(1000.0f) - (float)(-1000.0f)) + (float)(-1000.0f); };
    auto idx_dist = []() { return (int)((0) + (int64_t)(random64() % (uint64_t)((DATA_SIZE - 1) - (0) + 1))); };

    do {
        // 生成随机源数据（框架 RNG，每轮重掷）
        for (int i = 0; i < DATA_SIZE; ++i) {
            src[i] = float_dist();
        }
        memset(dst, 0, sizeof(dst));
        // 生成随机索引（16个）
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            indices[i] = idx_dist();
        }

        // ---- DUT: NEON 逐 lane gather + 向量乘 + 逐 lane scatter ----
        // ARM64 基线无 gather/scatter 指令；vld1q_lane/vst1q_lane 逐 lane
        // 访存是 NEON 上的等价路径（数据流经向量寄存器）。lane 下标须为
        // 编译期常量，故按 128-bit 向量寄存器展开（4 lane/寄存器，共 4 个）。
        // 乘 2 为精确操作（指数加一，无舍入），逐位比较安全。
        // DUT 与 golden 的 scatter 写入顺序均为 lane/i 升序，重复索引时
        // 最终写入者一致，结果可比。
        constexpr int NVEC = VECTOR_SIZE / 4;
        for (int v = 0; v < NVEC; ++v) {
            const int base = v * 4;
            float32x4_t acc = vdupq_n_f32(0.0f);
            acc = vld1q_lane_f32(&src[indices[base + 0]], acc, 0);
            acc = vld1q_lane_f32(&src[indices[base + 1]], acc, 1);
            acc = vld1q_lane_f32(&src[indices[base + 2]], acc, 2);
            acc = vld1q_lane_f32(&src[indices[base + 3]], acc, 3);
            // 向量计算：乘以 2（数据全程驻留向量寄存器）
            acc = vmulq_n_f32(acc, 2.0f);
            // 逐 lane scatter 回内存
            vst1q_lane_f32(&dst[indices[base + 0]], acc, 0);
            vst1q_lane_f32(&dst[indices[base + 1]], acc, 1);
            vst1q_lane_f32(&dst[indices[base + 2]], acc, 2);
            vst1q_lane_f32(&dst[indices[base + 3]], acc, 3);
        }

        // ---- golden: 独立标量实现（与 DUT 不同的数据路径） ----
        float ref[DATA_SIZE];
        memset(ref, 0, sizeof(ref));
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            const int idx = indices[i];
            ref[idx] = src[idx] * 2.0f;
        }

        // ---- 验证：比较目标数组与参考（逐位比较） ----
        bool data_ok = (memcmp(dst, ref, sizeof(dst)) == 0);

        // ---- 一致性测试：将 dst 中前 16 个元素加载两次，比较是否一致 ----
        alignas(16) float load_buf1[VECTOR_SIZE];
        alignas(16) float load_buf2[VECTOR_SIZE];
        memcpy(load_buf1, dst, sizeof(load_buf1));
        memcpy(load_buf2, dst, sizeof(load_buf2));
        bool consistent = (memcmp(load_buf1, load_buf2, sizeof(load_buf1)) == 0);

        if (!(data_ok && consistent)) {
            report_fail_msg("gatherscatter_f32: Gather-scatter mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int gatherscatter_f32_run(struct test *test, int cpu) {
    (void)cpu;
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for gatherscatter_f32");
    return EXIT_SKIP;
}
#endif

static int gatherscatter_f32_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(gatherscatter_f32, "Gather-scatter single-precision (NEON lane loads/stores, ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = gatherscatter_f32_init,
    .test_run = gatherscatter_f32_run,
    .test_cleanup = gatherscatter_f32_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
