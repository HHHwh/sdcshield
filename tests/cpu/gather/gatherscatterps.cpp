#include <sandstone.h>
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>
#ifdef __aarch64__
#include <arm_neon.h>          // NEON intrinsics (aarch64 only)
#endif

static constexpr int VECTOR_SIZE = 16;          // 16 个单精度浮点数

/*
 * 源/目标数据大小: 统一配置链:
 *   1. -O gatherscatterps.data_size=N / 配置文件同名条目
 *   2. 环境变量 SDC_GATHERSCATTERPS_DATA_SIZE
 *   3. 自适应: 每线程 src+dst+ref 3 个数组 (4B/元素) ~ 12B/元素, 占
 *      物理内存 60%, clamp [1024, 16M]。历史硬编码 1024。
 *      线性缓冲用 sdc_auto_linear_count（sqrt 缩放会欠缩放压力）。
 * 放大后 gather/scatter 访问跨度加大, 随机索引命中 L2/LLC/DRAM 不同域。
 */
static int gather_data_size(struct test *test)
{
    /* hi 动态: 大内存机器按 60% 预算放开, 小内存机器保持 16M 收口 */
    const int64_t hi = sdc_linear_hi(4, 3, 1.0, 600, thread_count(), 16 << 20);
    int64_t v = sdc_knob_int_clamped(test, "data_size", 0, 1024, hi);
    if (v == 0)
        sdc_env_knob_int_clamped("gatherscatterps", "data_size", &v, 1024, hi);
    if (v > 0)
        return (int)v;
    return (int)sdc_auto_linear_count(4, 3, 1.0, 600, thread_count(), 1024, hi, test->id, "data_size");
}

static int gatherscatterps_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int gatherscatterps_run(struct test *test, int cpu) {
    (void)cpu;
    const int DATA_SIZE = gather_data_size(test);
    const size_t bytes = (size_t)DATA_SIZE * sizeof(float);
    // 每个线程独立分配数据 (堆分配, 支持运行时尺寸)
    float *src = (float *)aligned_alloc(16, bytes);
    float *dst = (float *)aligned_alloc(16, bytes);
    if (!src || !dst) {
        free(src);
        free(dst);
        return EXIT_FAILURE;
    }
    int32_t indices[VECTOR_SIZE];

    auto float_dist = []() { return frandomf_scale((float)(1000.0f) - (float)(-1000.0f)) + (float)(-1000.0f); };
    auto idx_dist = [&]() { return (int)((0) + (int64_t)(random64() % (uint64_t)((DATA_SIZE - 1) - (0) + 1))); };
    auto mask_dist = []() { return (uint16_t)((0) + (int64_t)(random64() % (uint64_t)((0xFFFF) - (0) + 1))); };

    // 压载结构（CodeRabbit 意见）：每轮 DUT 只有 mask 选中 lane 的
    // gather/scatter lane 访存，若内层每轮仍整块 memset dst/ref 并
    // 全数组逐元素比较（各 1024 元素），memset/比较开销依旧远大于 DUT
    // 访存。改为：内层每轮仅重掷 indices/掩码、执行 DUT、对选中 lane
    // 命中的下标做期望值比较并清零（为下一轮 scatter 备好落点）；整块
    // memset 与整块检查移到外层迭代各一次——内层各轮结束时选中下标
    // 均已清零，外层整块检查发现非零即说明 scatter 写到了选中下标
    // 之外的位置（越界写入/污染）。
    constexpr int kInnerRounds = 64;

    do {
        // 生成随机源数据（框架 RNG，每轮重掷）
        for (int i = 0; i < DATA_SIZE; ++i) {
            src[i] = float_dist();
        }

        // 整块清零仅此一次（建立全零基准；内层不再整块 memset）
        memset(dst, 0, bytes);

        uint16_t mask = 0;        // 每轮重掷；保留最后一轮值供外层检查失败现场用
        for (int round = 0; round < kInnerRounds; ++round) {
            // 生成随机索引（16个）
            for (int i = 0; i < VECTOR_SIZE; ++i) {
                indices[i] = idx_dist();
            }
            // 生成随机掩码（16位）
            mask = mask_dist();

            // ---- 前置落点检查（第四轮 review 意见）：本轮 mask 选中 lane 的
            // 落点在 DUT 执行前应全零（外层迭代整块 memset 一次，内层每轮
            // 结束仅清零选中下标）。按位非零即说明更早某轮 scatter 写到了
            // 当时的选中下标之外——该越界值一旦被后续轮次命中，会被正常
            // 写入并清零抹掉，逃过外层整块检查，故须在落点被本轮覆盖前
            // 拦截。memcmp 逐位比较而非 dst[idx]==0，-0.0 等按位非零但
            // 数值为 0 的残留不会漏报 ----
            const float zero = 0.0f;
            for (int i = 0; i < VECTOR_SIZE; ++i) {
                if (mask & (1 << i)) {
                    const int idx = indices[i];
                    if (memcmp(&dst[idx], &zero, sizeof(zero)) != 0) {
                        uint32_t actual_bits, expected_bits;
                        memcpy(&actual_bits, &dst[idx], sizeof(actual_bits));
                        memcpy(&expected_bits, &zero, sizeof(expected_bits));
                        report_fail_msg("gatherscatterps: Scatter wrote outside indices: dst[%d] non-zero before round %d, "
                                        "mask=0x%04x, actual=%f (0x%08x), expected=%f (0x%08x), xor=0x%08x",
                                        idx, round, mask, dst[idx], actual_bits, zero, expected_bits,
                                        actual_bits ^ expected_bits);
                        return EXIT_FAILURE;
                    }
                }
            }

            // ---- DUT: NEON 逐 lane 掩码 gather + 向量乘 + 逐 lane 掩码 scatter ----
            // ARM64 基线无 gather/scatter 指令；vld1q_lane/vst1q_lane 逐 lane
            // 访存是 NEON 上的等价路径（数据流经向量寄存器）。lane 下标须为
            // 编译期常量，故按 128-bit 向量寄存器展开（4 lane/寄存器，共 4 个）。
            // 掩码语义沿用 x86 参考实现：未选中的 gather lane 置零（对应
            // _mm512_maskz 系 zeroing），scatter 仅写入选中 lane。
            // 乘 2 为精确操作（指数加一，无舍入），逐位比较安全。
            // DUT 与 golden 的 scatter 写入顺序均为 lane/i 升序，重复索引时
            // 最终写入者一致，结果可比。
            constexpr int NVEC = VECTOR_SIZE / 4;
            for (int v = 0; v < NVEC; ++v) {
                const int base = v * 4;
                float32x4_t acc = vdupq_n_f32(0.0f);
                if (mask & (1 << (base + 0)))
                    acc = vld1q_lane_f32(&src[indices[base + 0]], acc, 0);
                if (mask & (1 << (base + 1)))
                    acc = vld1q_lane_f32(&src[indices[base + 1]], acc, 1);
                if (mask & (1 << (base + 2)))
                    acc = vld1q_lane_f32(&src[indices[base + 2]], acc, 2);
                if (mask & (1 << (base + 3)))
                    acc = vld1q_lane_f32(&src[indices[base + 3]], acc, 3);
                // 向量计算：乘以 2（未选中 lane 为 0*2=0，无影响）
                acc = vmulq_n_f32(acc, 2.0f);
                // 逐 lane 掩码 scatter 回内存
                if (mask & (1 << (base + 0)))
                    vst1q_lane_f32(&dst[indices[base + 0]], acc, 0);
                if (mask & (1 << (base + 1)))
                    vst1q_lane_f32(&dst[indices[base + 1]], acc, 1);
                if (mask & (1 << (base + 2)))
                    vst1q_lane_f32(&dst[indices[base + 2]], acc, 2);
                if (mask & (1 << (base + 3)))
                    vst1q_lane_f32(&dst[indices[base + 3]], acc, 3);
            }

            // ---- golden + 验证：独立标量期望值，仅比较本轮选中 lane 命中
            // 的下标 ----（标量乘与 DUT 的向量乘是不同数据路径；乘 2 为
            // 精确操作，逐位比较安全。重复索引时各 lane 期望值相同，可比。）
            int bad_idx = -1;
            for (int i = 0; i < VECTOR_SIZE; ++i) {
                if (mask & (1 << i)) {
                    const int idx = indices[i];
                    const float expected = src[idx] * 2.0f;
                    if (memcmp(&dst[idx], &expected, sizeof(expected)) != 0) {
                        bad_idx = idx;
                        break;
                    }
                }
            }
            bool data_ok = (bad_idx < 0);

            // ---- 一致性测试：将 dst 中前 16 个元素加载两次，比较是否一致 ----
            alignas(16) float load_buf1[VECTOR_SIZE];
            alignas(16) float load_buf2[VECTOR_SIZE];
            memcpy(load_buf1, dst, sizeof(load_buf1));
            memcpy(load_buf2, dst, sizeof(load_buf2));
            bool consistent = (memcmp(load_buf1, load_buf2, sizeof(load_buf1)) == 0);

            if (!(data_ok && consistent)) {
                if (!data_ok) {
                    // 失败现场：首个失配下标、首个指向该下标的选中 lane、实际/期望值
                    int lane = -1;
                    for (int i = 0; i < VECTOR_SIZE; ++i) {
                        if ((mask & (1 << i)) && indices[i] == bad_idx) {
                            lane = i;
                            break;
                        }
                    }
                    const float expected = src[bad_idx] * 2.0f;
                    uint32_t actual_bits, expected_bits;
                    memcpy(&actual_bits, &dst[bad_idx], sizeof(actual_bits));
                    memcpy(&expected_bits, &expected, sizeof(expected_bits));
                    report_fail_msg("gatherscatterps: Masked gather-scatter mismatch: first bad index %d (lane %d), "
                                    "mask=0x%04x, src[%d]=%f, actual=%f (0x%08x), expected=%f (0x%08x), xor=0x%08x",
                                    bad_idx, lane, mask, bad_idx, src[bad_idx],
                                    dst[bad_idx], actual_bits, expected, expected_bits,
                                    actual_bits ^ expected_bits);
                } else {
                    report_fail_msg("gatherscatterps: Masked gather-scatter mismatch or consistency failure");
                }
                free(src);
                free(dst);
                return EXIT_FAILURE;
            }

            // ---- 仅清零本轮选中 lane 命中的下标（至多 VECTOR_SIZE 次写）：
            // 为下一轮 scatter 提供干净落点，并使外层整块检查时 dst 恢复全零 ----
            for (int i = 0; i < VECTOR_SIZE; ++i) {
                if (mask & (1 << i)) {
                    dst[indices[i]] = 0.0f;
                }
            }
        }

        // ---- 外层整块检查（每外层迭代一次）：选中下标在内层各轮结束时
        // 已被清零，全数组应仍为全零；发现非零即 scatter 写到了选中下标
        // 之外的位置，报 fail ----
        const float zero = 0.0f;
        for (int i = 0; i < DATA_SIZE; ++i) {
            if (memcmp(&dst[i], &zero, sizeof(zero)) != 0) {
                int lane = -1;
                for (int j = 0; j < VECTOR_SIZE; ++j) {
                    if ((mask & (1 << j)) && indices[j] == i) {
                        lane = j;
                        break;
                    }
                }
                uint32_t actual_bits, expected_bits;
                memcpy(&actual_bits, &dst[i], sizeof(actual_bits));
                memcpy(&expected_bits, &zero, sizeof(expected_bits));
                report_fail_msg("gatherscatterps: Scatter wrote outside indices: first bad index %d (lane %d), "
                                "mask=0x%04x, src[%d]=%f, actual=%f (0x%08x), expected=%f (0x%08x), xor=0x%08x",
                                i, lane, mask, i, src[i],
                                dst[i], actual_bits, zero, expected_bits,
                                actual_bits ^ expected_bits);
                free(src);
                free(dst);
                return EXIT_FAILURE;
            }
        }

    } while (test_time_condition(test));

    free(src);
    free(dst);
    return EXIT_SUCCESS;
}
#else
static int gatherscatterps_run(struct test *test, int cpu) {
    (void)cpu;
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for gatherscatterps");
    return EXIT_SKIP;
}
#endif

static int gatherscatterps_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(gatherscatterps, "Masked gather-scatter single-precision (NEON lane loads/stores, ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = gatherscatterps_init,
    .test_run = gatherscatterps_run,
    .test_cleanup = gatherscatterps_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
