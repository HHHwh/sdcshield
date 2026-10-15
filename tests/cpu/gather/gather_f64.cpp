#include <sandstone.h>
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#ifdef __aarch64__
#include <arm_neon.h>          // NEON intrinsics (aarch64 only)
#endif

static constexpr int VECTOR_SIZE = 4;          // 4 个双精度浮点数

/*
 * 源数据大小: 统一配置链:
 *   1. -O gather_f64.data_size=N / 配置文件同名条目
 *   2. 环境变量 SDC_GATHER_F64_DATA_SIZE
 *   3. 自适应: 每线程 src(8B) + 索引(8B/4) ~ 10B/元素, 占物理内存
 *      60%, clamp [1024, 动态hi]（大内存机器按预算放开, 小机器 16M）。
 *      线性缓冲用 sdc_auto_linear_count（sqrt 缩放会欠缩放压力）。
 * 放大后 gather 访问跨度加大, 随机索引命中 L2/LLC/DRAM 不同域。
 */
static int gather_data_size(struct test *test)
{
    /* hi 动态: 大内存机器按 60% 预算放开, 小内存机器保持 16M 收口 */
    const int64_t hi = sdc_linear_hi(8, 1, 1.5, 600, thread_count(), 16 << 20);
    int64_t v = sdc_knob_int_clamped(test, "data_size", 0, 1024, hi);
    if (v == 0)
        sdc_env_knob_int_clamped("gather_f64", "data_size", &v, 1024, hi);
    if (v > 0)
        return (int)v;
    return (int)sdc_auto_linear_count(8, 1, 1.5, 600, thread_count(), 1024, hi, test->id, "data_size");
}

static int gather_f64_init(struct test *test) {
    /* 预热 knob 缓存: data_size 在 run(worker线程)里经 sdc_knob_int_clamped
     * 的缓存读取, 必须在 init(主线程)查询一次填充缓存, 否则 debug 构建下
     * worker 线程查询 knob 触发 abort, 且显式配置会失效。 */
    (void)gather_data_size(test);
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int gather_f64_run(struct test *test, int cpu) {
    (void)cpu;
    const int DATA_SIZE = gather_data_size(test);
    // 每个线程独立分配数据 (堆分配, 支持运行时尺寸)
    double *src = (double *)aligned_alloc(16, (size_t)DATA_SIZE * sizeof(double));
    if (!src)
        return EXIT_FAILURE;
    long long indices[VECTOR_SIZE];

    auto double_dist = []() { return frandom_scale((double)(1000.0) - (double)(-1000.0)) + (double)(-1000.0); };
    auto idx_dist = [&]() { return (int)((0) + (int64_t)(random64() % (uint64_t)((DATA_SIZE - 1) - (0) + 1))); };

    // 内层压载轮数（CodeRabbit 意见）：重掷 src 一次是 1024 次 RNG 调用，
    // 原单轮结构只做 2 个向量、4 次 vld1q_lane，时间几乎全花在 RNG 上，
    // gather 访存通路压力太小；改为每次刷新 src 后内层跑 M 轮（每轮重掷
    // indices + gather + 比较），把时间比例倒转为 DUT 压载为主。
    constexpr int kInnerRounds = 64;

    do {
        // 生成随机源数据（框架 RNG，每轮重掷）
        for (int i = 0; i < DATA_SIZE; ++i) {
            src[i] = double_dist();
        }

        for (int round = 0; round < kInnerRounds; ++round) {
            // 生成随机索引（64位）
            for (int i = 0; i < VECTOR_SIZE; ++i) {
                indices[i] = idx_dist();
            }

            // ---- DUT: NEON 逐 lane gather（vld1q_lane_f64） ----
            // ARM64 基线无 gather 指令；vld1q_lane 逐 lane 加载是 NEON 上的
            // gather 等价路径（数据流经向量寄存器）。lane 下标须为编译期常量，
            // 故按 128-bit 向量寄存器展开（2 lane/寄存器，共 2 个）。
            constexpr int NVEC = VECTOR_SIZE / 2;
            float64x2_t vecs[NVEC];
            for (int v = 0; v < NVEC; ++v) {
                float64x2_t acc = vdupq_n_f64(0.0);
                const int base = v * 2;
                acc = vld1q_lane_f64(&src[indices[base + 0]], acc, 0);
                acc = vld1q_lane_f64(&src[indices[base + 1]], acc, 1);
                vecs[v] = acc;
            }
            double gathered[VECTOR_SIZE];
            for (int v = 0; v < NVEC; ++v)
                vst1q_f64(&gathered[v * 2], vecs[v]);

            // ---- golden: 独立标量实现（与 DUT 不同的数据路径） ----
            double ref[VECTOR_SIZE];
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
            alignas(16) double store_buf[VECTOR_SIZE];
            memcpy(store_buf, gathered, sizeof(gathered));
            double reload[VECTOR_SIZE];
            memcpy(reload, store_buf, sizeof(store_buf));
            bool consistent = (memcmp(reload, gathered, sizeof(gathered)) == 0);

            if (!(data_ok && consistent)) {
                if (!data_ok) {
                    // 失败现场：首个失配 lane 号、该 lane 的 indices 值、实际/期望值
                    uint64_t actual_bits, expected_bits;
                    memcpy(&actual_bits, &gathered[bad_lane], sizeof(actual_bits));
                    memcpy(&expected_bits, &ref[bad_lane], sizeof(expected_bits));
                    report_fail_msg("gather_f64: Gather mismatch: first bad lane %d, indices[%d]=%lld, "
                                    "actual=%f (0x%016llx), expected=%f (0x%016llx), xor=0x%016llx",
                                    bad_lane, bad_lane, (long long)indices[bad_lane],
                                    gathered[bad_lane], (unsigned long long)actual_bits,
                                    ref[bad_lane], (unsigned long long)expected_bits,
                                    (unsigned long long)(actual_bits ^ expected_bits));
                } else {
                    report_fail_msg("gather_f64: Gather mismatch or consistency failure");
                }
                free(src);
                return EXIT_FAILURE;
            }
        }
    } while (test_time_condition(test));

    free(src);
    return EXIT_SUCCESS;
}
#else
static int gather_f64_run(struct test *test, int cpu) {
    (void)cpu;
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for gather_f64");
    return EXIT_SKIP;
}
#endif

static int gather_f64_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(gather_f64, "Gather double-precision (NEON lane loads, ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = gather_f64_init,
    .test_run = gather_f64_run,
    .test_cleanup = gather_f64_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
