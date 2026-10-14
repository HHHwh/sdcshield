#include <sandstone.h>
#include <cstdint>
#include <cstring>
#ifdef __aarch64__
#include <arm_neon.h>          // NEON intrinsics (aarch64 only)
#endif

static constexpr int VECTOR_SIZE = 8;           // 8 个 int64
static constexpr int DATA_SIZE = 1024;          // 源/目标数据大小

static int gatherscatter_i64_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int gatherscatter_i64_run(struct test *test, int cpu) {
    (void)cpu;
    // 每个线程独立分配数据
    alignas(16) int64_t src[DATA_SIZE];
    alignas(16) int64_t dst[DATA_SIZE];
    int64_t indices[VECTOR_SIZE];

    auto int_dist = []() { return (int64_t)((-1000000LL) + (int64_t)(random64() % (uint64_t)((1000000LL) - (-1000000LL) + 1))); };
    auto idx_dist = []() { return (int)((0) + (int64_t)(random64() % (uint64_t)((DATA_SIZE - 1) - (0) + 1))); };

    // 内层压载轮数（CodeRabbit 意见）：重掷 src 一次是 1024 次 RNG 调用，
    // 原单轮结构只做 4 个向量、16 次 lane 访存，时间几乎全花在 RNG 上，
    // gather/scatter 访存通路压力太小；改为每次刷新 src 后内层跑 M 轮
    // （每轮重掷 indices + gather/scatter + 比较），把时间比例倒转为
    // DUT 压载为主。scatter 目标缓冲每轮重置，避免上一轮写入污染。
    constexpr int kInnerRounds = 64;

    do {
        // 生成随机源数据（框架 RNG，每轮重掷）
        for (int i = 0; i < DATA_SIZE; ++i) {
            src[i] = int_dist();
        }

        for (int round = 0; round < kInnerRounds; ++round) {
            memset(dst, 0, sizeof(dst));
            // 生成随机索引（8个64位）
            for (int i = 0; i < VECTOR_SIZE; ++i) {
                indices[i] = idx_dist();
            }

            // ---- DUT: NEON 逐 lane gather + 向量加 + 逐 lane scatter ----
            // ARM64 基线无 gather/scatter 指令；vld1q_lane/vst1q_lane 逐 lane
            // 访存是 NEON 上的等价路径（数据流经向量寄存器）。lane 下标须为
            // 编译期常量，故按 128-bit 向量寄存器展开（2 lane/寄存器，共 4 个）。
            // DUT 与 golden 的 scatter 写入顺序均为 lane/i 升序，重复索引时
            // 最终写入者一致，结果可比。
            constexpr int NVEC = VECTOR_SIZE / 2;
            for (int v = 0; v < NVEC; ++v) {
                const int base = v * 2;
                int64x2_t acc = vdupq_n_s64(0);
                acc = vld1q_lane_s64(&src[indices[base + 0]], acc, 0);
                acc = vld1q_lane_s64(&src[indices[base + 1]], acc, 1);
                // 向量计算：加 1（数据全程驻留向量寄存器；64-bit 向量无 _n 变体）
                acc = vaddq_s64(acc, vdupq_n_s64(1));
                // 逐 lane scatter 回内存
                vst1q_lane_s64(&dst[indices[base + 0]], acc, 0);
                vst1q_lane_s64(&dst[indices[base + 1]], acc, 1);
            }

            // ---- golden: 独立标量实现（与 DUT 不同的数据路径） ----
            int64_t ref[DATA_SIZE];
            memset(ref, 0, sizeof(ref));
            for (int i = 0; i < VECTOR_SIZE; ++i) {
                const int idx = indices[i];
                ref[idx] = src[idx] + 1;
            }

            // ---- 验证：比较目标数组与参考（逐位比较，定位首个失配下标） ----
            int bad_idx = -1;
            for (int i = 0; i < DATA_SIZE; ++i) {
                if (memcmp(&dst[i], &ref[i], sizeof(dst[0])) != 0) {
                    bad_idx = i;
                    break;
                }
            }
            bool data_ok = (bad_idx < 0);

            // ---- 一致性测试：将 dst 中前 8 个元素加载两次，比较是否一致 ----
            alignas(16) int64_t load_buf1[VECTOR_SIZE];
            alignas(16) int64_t load_buf2[VECTOR_SIZE];
            memcpy(load_buf1, dst, sizeof(load_buf1));
            memcpy(load_buf2, dst, sizeof(load_buf2));
            bool consistent = (memcmp(load_buf1, load_buf2, sizeof(load_buf1)) == 0);

            if (!(data_ok && consistent)) {
                if (!data_ok) {
                    // 失败现场：首个失配下标、首个指向该下标的 lane、实际/期望值
                    int lane = -1;
                    for (int i = 0; i < VECTOR_SIZE; ++i) {
                        if (indices[i] == bad_idx) {
                            lane = i;
                            break;
                        }
                    }
                    uint64_t actual_bits, expected_bits;
                    memcpy(&actual_bits, &dst[bad_idx], sizeof(actual_bits));
                    memcpy(&expected_bits, &ref[bad_idx], sizeof(expected_bits));
                    report_fail_msg("gatherscatter_i64: Gather-scatter mismatch: first bad index %d (lane %d), "
                                    "src[%d]=%lld, actual=%lld (0x%016llx), expected=%lld (0x%016llx), xor=0x%016llx",
                                    bad_idx, lane, bad_idx, (long long)src[bad_idx],
                                    (long long)dst[bad_idx], (unsigned long long)actual_bits,
                                    (long long)ref[bad_idx], (unsigned long long)expected_bits,
                                    (unsigned long long)(actual_bits ^ expected_bits));
                } else {
                    report_fail_msg("gatherscatter_i64: Gather-scatter mismatch or consistency failure");
                }
                return EXIT_FAILURE;
            }
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int gatherscatter_i64_run(struct test *test, int cpu) {
    (void)cpu;
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for gatherscatter_i64");
    return EXIT_SKIP;
}
#endif

static int gatherscatter_i64_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(gatherscatter_i64, "Gather-scatter 64-bit integers (NEON lane loads/stores, ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = gatherscatter_i64_init,
    .test_run = gatherscatter_i64_run,
    .test_cleanup = gatherscatter_i64_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
