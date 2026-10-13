#include <sandstone.h>
#include <cstdint>
#include <cstring>
#ifdef __aarch64__
#include <arm_neon.h>          // NEON intrinsics (aarch64 only)
#endif

static constexpr int VECTOR_SIZE = 8;           // 8 个双精度浮点数
static constexpr int DATA_SIZE = 1024;          // 源/目标数据大小

static int gatherscatterpd_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int gatherscatterpd_run(struct test *test, int cpu) {
    (void)cpu;
    // 每个线程独立分配数据
    alignas(16) double src[DATA_SIZE];
    alignas(16) double dst[DATA_SIZE];
    int64_t indices[VECTOR_SIZE];

    auto double_dist = []() { return frandom_scale((double)(1000.0) - (double)(-1000.0)) + (double)(-1000.0); };
    auto idx_dist = []() { return (int)((0) + (int64_t)(random64() % (uint64_t)((DATA_SIZE - 1) - (0) + 1))); };
    auto mask_dist = []() { return (uint8_t)((0) + (int64_t)(random64() % (uint64_t)((0xFF) - (0) + 1))); };

    do {
        // 生成随机源数据（框架 RNG，每轮重掷）
        for (int i = 0; i < DATA_SIZE; ++i) {
            src[i] = double_dist();
        }
        memset(dst, 0, sizeof(dst));
        // 生成随机索引（8个64位）
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            indices[i] = idx_dist();
        }
        // 生成随机掩码（8位）
        uint8_t mask = mask_dist();

        // ---- DUT: NEON 逐 lane 掩码 gather + 向量乘 + 逐 lane 掩码 scatter ----
        // ARM64 基线无 gather/scatter 指令；vld1q_lane/vst1q_lane 逐 lane
        // 访存是 NEON 上的等价路径（数据流经向量寄存器）。lane 下标须为
        // 编译期常量，故按 128-bit 向量寄存器展开（2 lane/寄存器，共 4 个）。
        // 掩码语义沿用 x86 参考实现：未选中的 gather lane 置零（对应
        // _mm512_maskz 系 zeroing），scatter 仅写入选中 lane。
        // 乘 2 为精确操作（指数加一，无舍入），逐位比较安全。
        // DUT 与 golden 的 scatter 写入顺序均为 lane/i 升序，重复索引时
        // 最终写入者一致，结果可比。
        constexpr int NVEC = VECTOR_SIZE / 2;
        for (int v = 0; v < NVEC; ++v) {
            const int base = v * 2;
            float64x2_t acc = vdupq_n_f64(0.0);
            if (mask & (1 << (base + 0)))
                acc = vld1q_lane_f64(&src[indices[base + 0]], acc, 0);
            if (mask & (1 << (base + 1)))
                acc = vld1q_lane_f64(&src[indices[base + 1]], acc, 1);
            // 向量计算：乘以 2（未选中 lane 为 0*2=0，无影响）
            acc = vmulq_n_f64(acc, 2.0);
            // 逐 lane 掩码 scatter 回内存
            if (mask & (1 << (base + 0)))
                vst1q_lane_f64(&dst[indices[base + 0]], acc, 0);
            if (mask & (1 << (base + 1)))
                vst1q_lane_f64(&dst[indices[base + 1]], acc, 1);
        }

        // ---- golden: 独立标量实现（与 DUT 不同的数据路径） ----
        double ref[DATA_SIZE];
        memset(ref, 0, sizeof(ref));
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            if (mask & (1 << i)) {
                const int idx = indices[i];
                ref[idx] = src[idx] * 2.0;
            }
        }

        // ---- 验证：比较目标数组与参考（逐位比较） ----
        bool data_ok = (memcmp(dst, ref, sizeof(dst)) == 0);

        // ---- 一致性测试：将 dst 中前 8 个元素加载两次，比较是否一致 ----
        alignas(16) double load_buf1[VECTOR_SIZE];
        alignas(16) double load_buf2[VECTOR_SIZE];
        memcpy(load_buf1, dst, sizeof(load_buf1));
        memcpy(load_buf2, dst, sizeof(load_buf2));
        bool consistent = (memcmp(load_buf1, load_buf2, sizeof(load_buf1)) == 0);

        if (!(data_ok && consistent)) {
            report_fail_msg("gatherscatterpd: Masked gather-scatter mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int gatherscatterpd_run(struct test *test, int cpu) {
    (void)cpu;
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for gatherscatterpd");
    return EXIT_SKIP;
}
#endif

static int gatherscatterpd_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(gatherscatterpd, "Masked gather-scatter double-precision (NEON lane loads/stores, ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = gatherscatterpd_init,
    .test_run = gatherscatterpd_run,
    .test_cleanup = gatherscatterpd_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
