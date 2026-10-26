/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test fisttp_sve
 * @parblock
 * SVE port of fisttp_arm: floating-point to integer conversion
 * (round-toward-zero) on real SVE conversion instructions —
 * svcvt_s32_f32_z (verified to have FCVTZS truncation semantics:
 * 1.9->1, -2.9->-2). A fixed random table of float inputs (range
 * [-100, 100]) is generated at init and the golden int32 outputs are
 * precomputed with std::trunc. Each run re-converts on SVE lanes and
 * compares, plus a store/load consistency check. Same NUM_ELEMENTS,
 * FLOAT_MIN/MAX and FIXED_SEED as the original.
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include <cmath>
#include <thread>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

/* 元素数 NUM_ELEMENTS(1024) — 运行时可配置, 统一优先级链:
 *   1. -O fisttp_sve.num_elements=N / 配置文件同名条目
 *   2. 环境变量 SDC_FISTTP_SVE_NUM_ELEMENTS
 *   3. 自适应: 每线程 golden_in(4B 共享)+out(4B)+store(4B)+reload(4B)+
 *      golden_out(4B 共享) ~= 20B/元素, 占物理内存 60%,
 *      clamp [1024, 16M]。线性缓冲用 sdc_auto_linear_count
 *      （sqrt 缩放会欠缩放压力）。 */
static size_t fisttp_sve_num_elements(struct test *test)
{
    /* hi 动态: 大内存机器按 60% 预算放开, 小内存机器保持 16M 收口 */
    const int64_t hi = sdc_linear_hi(20, 1, 1.0, 600, thread_count(), 16 << 20);
    int64_t v = sdc_knob_int_clamped(test, "num_elements", 0, 1024, hi);
    if (v == 0)
        sdc_env_knob_int_clamped("fisttp_sve", "num_elements", &v, 1024, hi);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(20, 1, 1.0, 600, thread_count(),
                                         1024, hi, test->id, "num_elements");
}

static constexpr float FLOAT_MIN = -100.0f;
static constexpr float FLOAT_MAX = 100.0f;
static constexpr uint64_t FIXED_SEED = 0x123456789ABCDEF0ULL;

struct FisttpSveData {
    std::vector<float> golden_input;
    std::vector<int32_t> golden_output;
    // 每线程独立输出/一致性缓冲（run 时按 cpu 索引）
    std::vector<std::vector<int32_t>> out_buf;
    std::vector<std::vector<int32_t>> store_buf;
    std::vector<std::vector<int32_t>> reload_buf;
};

static inline int32_t truncate_to_int(float val) {
    return static_cast<int32_t>(std::trunc(val));
}

static int fisttp_sve_init(struct test *test) {
#ifdef __aarch64__
    /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
     * layer: arm64_require_sve_vl does the SVE capability check
     * + 128..1024-bit range gate in one call (skip reason
     * framework-issued). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif

    auto *d = new (std::nothrow) FisttpSveData;
    if (!d) return EXIT_FAILURE;

    const size_t NUM_ELEMENTS = fisttp_sve_num_elements(test);
    d->golden_input.resize(NUM_ELEMENTS);
    d->golden_output.resize(NUM_ELEMENTS);

    int max_cpus = num_cpus();
    if (max_cpus <= 0) max_cpus = static_cast<int>(std::thread::hardware_concurrency());
    if (max_cpus <= 0) max_cpus = 256;

    d->out_buf.resize(max_cpus);
    d->store_buf.resize(max_cpus);
    d->reload_buf.resize(max_cpus);
    for (int i = 0; i < max_cpus; ++i) {
        d->out_buf[i].resize(NUM_ELEMENTS);
        d->store_buf[i].resize(NUM_ELEMENTS);
        d->reload_buf[i].resize(NUM_ELEMENTS);
    }

    std::mt19937_64 rng(FIXED_SEED);
    std::uniform_real_distribution<float> dist(FLOAT_MIN, FLOAT_MAX);

    for (size_t i = 0; i < NUM_ELEMENTS; ++i) {
        d->golden_input[i] = dist(rng);
        d->golden_output[i] = truncate_to_int(d->golden_input[i]);
    }

    test->data = d;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int fisttp_sve_run(struct test *test, int cpu) {
    auto *d = static_cast<FisttpSveData *>(test->data);
    if (!d) return EXIT_FAILURE;

    const size_t NUM_ELEMENTS = d->golden_input.size();
    size_t cpu_idx = (cpu >= 0 && static_cast<size_t>(cpu) < d->out_buf.size())
                     ? static_cast<size_t>(cpu) : 0;
    int32_t *out = d->out_buf[cpu_idx].data();
    int32_t *store_buf = d->store_buf[cpu_idx].data();
    int32_t *reload_buf = d->reload_buf[cpu_idx].data();

    do {
        /* SVE 硬件转换: svcvt_s32_f32_z = round-toward-zero (FCVTZS 语义) */
        const float *in = d->golden_input.data();
        for (size_t base = 0; base < NUM_ELEMENTS; base += svcntw()) {
            svbool_t pg = svwhilelt_b32((uint64_t)base, (uint64_t)NUM_ELEMENTS);
            svfloat32_t vf = svld1_f32(pg, in + base);
            svst1_s32(pg, out + base, svcvt_s32_f32_x(pg, vf));
        }

        /* 结果校验 + store/load 一致性（与原版相同结构） */
        bool data_ok = (std::memcmp(out, d->golden_output.data(),
                                    NUM_ELEMENTS * sizeof(int32_t)) == 0);
        std::memcpy(store_buf, out, NUM_ELEMENTS * sizeof(int32_t));
        std::memcpy(reload_buf, store_buf, NUM_ELEMENTS * sizeof(int32_t));
        bool consistent = (std::memcmp(reload_buf, out,
                                       NUM_ELEMENTS * sizeof(int32_t)) == 0);

        if (!data_ok || !consistent) {
            int mismatch_idx = -1;
            for (size_t i = 0; i < NUM_ELEMENTS; ++i) {
                if (out[i] != d->golden_output[i]) { mismatch_idx = (int)i; break; }
            }
            char ctx_msg[200];
            snprintf(ctx_msg, sizeof(ctx_msg),
                     "fisttp_sve: SVE conversion mismatch on CPU %d at idx %d "
                     "(out=%d golden=%d, data_ok=%d, consistent=%d)",
                     cpu, mismatch_idx,
                     mismatch_idx >= 0 ? out[mismatch_idx] : 0,
                     mismatch_idx >= 0 ? d->golden_output[mismatch_idx] : 0,
                     (int)data_ok, (int)consistent);
            log_data("fisttp_sve input (float, runtime-sized)",
                     in, NUM_ELEMENTS * sizeof(float));
            log_data("fisttp_sve output (int32 SVE svcvt result, runtime-sized)",
                     out, NUM_ELEMENTS * sizeof(int32_t));
            log_data("fisttp_sve golden (std::trunc int32 result, runtime-sized)",
                     d->golden_output.data(), NUM_ELEMENTS * sizeof(int32_t));
            report_fail_msg("%s", ctx_msg);
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int fisttp_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for fisttp_sve");
    return EXIT_SKIP;
}
#endif

static int fisttp_sve_finish(struct test *test) {
    auto *d = static_cast<FisttpSveData *>(test->data);
    if (d) delete d;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

DECLARE_TEST(fisttp_sve, "Floating-point to integer conversion (round-toward-zero) on SVE svcvt lanes (port of fisttp_arm)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fisttp_sve_init,
    .test_run = fisttp_sve_run,
    .test_cleanup = fisttp_sve_finish,
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
