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

    // 压载结构（CodeRabbit 意见）：每轮 DUT 只有 gather/scatter 共
    // 2*VECTOR_SIZE 次 lane 访存，若内层每轮仍整块 memset dst/ref 并
    // 全数组逐元素比较（各 1024 元素），memset/比较开销依旧远大于 DUT
    // 访存。改为：内层每轮仅重掷 indices、执行 DUT、对命中下标做期望
    // 值比较并清零（为下一轮 scatter 备好落点）；整块 memset 与整块
    // 检查移到外层迭代各一次——内层各轮结束时命中下标均已清零，外层
    // 整块检查发现非零即说明 scatter 写到了 indices 之外的位置（越界
    // 写入/污染）。
    constexpr int kInnerRounds = 64;

    do {
        // 生成随机源数据（框架 RNG，每轮重掷）
        for (int i = 0; i < DATA_SIZE; ++i) {
            src[i] = int_dist();
        }

        // 整块清零仅此一次（建立全零基准；内层不再整块 memset）
        memset(dst, 0, sizeof(dst));

        for (int round = 0; round < kInnerRounds; ++round) {
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

            // ---- golden + 验证：独立标量期望值，仅比较本轮命中下标 ----
            // （标量加与 DUT 的向量加是不同数据路径。重复索引时各 lane
            // 期望值相同，可比。）
            int bad_idx = -1;
            for (int i = 0; i < VECTOR_SIZE; ++i) {
                const int idx = (int)indices[i];
                const int64_t expected = src[idx] + 1;
                if (memcmp(&dst[idx], &expected, sizeof(expected)) != 0) {
                    bad_idx = idx;
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
                    const int64_t expected = src[bad_idx] + 1;
                    uint64_t actual_bits, expected_bits;
                    memcpy(&actual_bits, &dst[bad_idx], sizeof(actual_bits));
                    memcpy(&expected_bits, &expected, sizeof(expected_bits));
                    report_fail_msg("gatherscatter_i64: Gather-scatter mismatch: first bad index %d (lane %d), "
                                    "src[%d]=%lld, actual=%lld (0x%016llx), expected=%lld (0x%016llx), xor=0x%016llx",
                                    bad_idx, lane, bad_idx, (long long)src[bad_idx],
                                    (long long)dst[bad_idx], (unsigned long long)actual_bits,
                                    (long long)expected, (unsigned long long)expected_bits,
                                    (unsigned long long)(actual_bits ^ expected_bits));
                } else {
                    report_fail_msg("gatherscatter_i64: Gather-scatter mismatch or consistency failure");
                }
                return EXIT_FAILURE;
            }

            // ---- 仅清零本轮命中下标（VECTOR_SIZE 次写）：为下一轮 scatter
            // 提供干净落点，并使外层整块检查时 dst 恢复全零 ----
            for (int i = 0; i < VECTOR_SIZE; ++i) {
                dst[indices[i]] = 0;
            }
        }

        // ---- 外层整块检查（每外层迭代一次）：命中下标在内层各轮结束时
        // 已被清零，全数组应仍为全零；发现非零即 scatter 写到了 indices
        // 之外的位置，报 fail ----
        const int64_t zero = 0;
        for (int i = 0; i < DATA_SIZE; ++i) {
            if (memcmp(&dst[i], &zero, sizeof(zero)) != 0) {
                int lane = -1;
                for (int j = 0; j < VECTOR_SIZE; ++j) {
                    if (indices[j] == i) {
                        lane = j;
                        break;
                    }
                }
                uint64_t actual_bits, expected_bits;
                memcpy(&actual_bits, &dst[i], sizeof(actual_bits));
                memcpy(&expected_bits, &zero, sizeof(expected_bits));
                report_fail_msg("gatherscatter_i64: Scatter wrote outside indices: first bad index %d (lane %d), "
                                "src[%d]=%lld, actual=%lld (0x%016llx), expected=%lld (0x%016llx), xor=0x%016llx",
                                i, lane, i, (long long)src[i],
                                (long long)dst[i], (unsigned long long)actual_bits,
                                (long long)zero, (unsigned long long)expected_bits,
                                (unsigned long long)(actual_bits ^ expected_bits));
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
