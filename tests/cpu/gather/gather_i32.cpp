#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#ifdef __aarch64__
#include <arm_neon.h>          // NEON intrinsics (aarch64 only)
#endif

static constexpr int VECTOR_SIZE = 8;          // 8 个 int32
static constexpr int DATA_SIZE = 1024;         // 源数据大小

static int gather_i32_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int gather_i32_run(struct test *test, int cpu) {
    (void)cpu;
    // 每个线程独立分配数据
    alignas(16) int32_t src[DATA_SIZE];
    int indices[VECTOR_SIZE];

    auto int_dist = []() { return (int32_t)((-10000) + (int64_t)(random64() % (uint64_t)((10000) - (-10000) + 1))); };
    auto idx_dist = []() { return (int)((0) + (int64_t)(random64() % (uint64_t)((DATA_SIZE - 1) - (0) + 1))); };

    // 内层压载轮数（CodeRabbit 意见）：重掷 src 一次是 1024 次 RNG 调用，
    // 原单轮结构只做 2 个向量、8 次 vld1q_lane，时间几乎全花在 RNG 上，
    // gather 访存通路压力太小；改为每次刷新 src 后内层跑 M 轮（每轮重掷
    // indices + gather + 比较），把时间比例倒转为 DUT 压载为主。
    constexpr int kInnerRounds = 64;

    do {
        // 生成随机源数据（框架 RNG，每轮重掷）
        for (int i = 0; i < DATA_SIZE; ++i) {
            src[i] = int_dist();
        }

        for (int round = 0; round < kInnerRounds; ++round) {
            // 生成随机索引
            for (int i = 0; i < VECTOR_SIZE; ++i) {
                indices[i] = idx_dist();
            }

            // ---- DUT: NEON 逐 lane gather（vld1q_lane_s32） ----
            // ARM64 基线无 gather 指令；vld1q_lane 逐 lane 加载是 NEON 上的
            // gather 等价路径（数据流经向量寄存器）。lane 下标须为编译期常量，
            // 故按 128-bit 向量寄存器展开（4 lane/寄存器，共 2 个）。
            constexpr int NVEC = VECTOR_SIZE / 4;
            int32x4_t vecs[NVEC];
            for (int v = 0; v < NVEC; ++v) {
                int32x4_t acc = vdupq_n_s32(0);
                const int base = v * 4;
                acc = vld1q_lane_s32(&src[indices[base + 0]], acc, 0);
                acc = vld1q_lane_s32(&src[indices[base + 1]], acc, 1);
                acc = vld1q_lane_s32(&src[indices[base + 2]], acc, 2);
                acc = vld1q_lane_s32(&src[indices[base + 3]], acc, 3);
                vecs[v] = acc;
            }
            int32_t gathered[VECTOR_SIZE];
            for (int v = 0; v < NVEC; ++v)
                vst1q_s32(&gathered[v * 4], vecs[v]);

            // ---- golden: 独立标量实现（与 DUT 不同的数据路径） ----
            int32_t ref[VECTOR_SIZE];
            for (int i = 0; i < VECTOR_SIZE; ++i) {
                ref[i] = src[indices[i]];
            }

            // ---- 比较硬件结果与参考（逐位比较，定位首个失配 lane） ----
            int bad_lane = -1;
            for (int i = 0; i < VECTOR_SIZE; ++i) {
                if (memcmp(&gathered[i], &ref[i], sizeof(gathered[0])) != 0) {
                    bad_lane = i;
                    break;
                }
            }
            bool data_ok = (bad_lane < 0);

            // ---- 一致性测试：存储 gathered 到内存再加载比较 ----
            alignas(16) int32_t store_buf[VECTOR_SIZE];
            memcpy(store_buf, gathered, sizeof(gathered));
            int32_t reload[VECTOR_SIZE];
            memcpy(reload, store_buf, sizeof(store_buf));
            bool consistent = (memcmp(reload, gathered, sizeof(gathered)) == 0);

            if (!(data_ok && consistent)) {
                if (!data_ok) {
                    // 失败现场：首个失配 lane 号、该 lane 的 indices 值、实际/期望值
                    uint32_t actual_bits, expected_bits;
                    memcpy(&actual_bits, &gathered[bad_lane], sizeof(actual_bits));
                    memcpy(&expected_bits, &ref[bad_lane], sizeof(expected_bits));
                    report_fail_msg("gather_i32: Gather mismatch: first bad lane %d, indices[%d]=%d, "
                                    "actual=%d (0x%08x), expected=%d (0x%08x), xor=0x%08x",
                                    bad_lane, bad_lane, indices[bad_lane],
                                    gathered[bad_lane], actual_bits, ref[bad_lane], expected_bits,
                                    actual_bits ^ expected_bits);
                } else {
                    report_fail_msg("gather_i32: Gather mismatch or consistency failure");
                }
                return EXIT_FAILURE;
            }
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int gather_i32_run(struct test *test, int cpu) {
    (void)cpu;
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for gather_i32");
    return EXIT_SKIP;
}
#endif

static int gather_i32_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(gather_i32, "Gather 32-bit integers (NEON lane loads, ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = gather_i32_init,
    .test_run = gather_i32_run,
    .test_cleanup = gather_i32_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
