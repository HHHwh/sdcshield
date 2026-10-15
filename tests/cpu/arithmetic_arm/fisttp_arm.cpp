// 文件：tests/cpu/arithmetic_arm/fisttp_arm.cpp
/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test fisttp_arm
 * @parblock
 * Floating-point to integer conversion (FCVTZS, round-toward-zero) via the Arm
 * Compute Library.  A fixed random table of float input (range [-100, 100]) is
 * generated at init and the golden int32 outputs are precomputed with
 * std::trunc.  Each run re-runs ACL's NECast (which issues FCVTZS) on the input
 * and compares the int32 output against the golden, plus a store/load
 * consistency check.  Pattern follows Intel OpenDCDiag's Eigen-based design.
 *
 * Per-thread ACL contexts (tensor + cast op) are preconfigured in init with
 * import_memory pointing at per-thread stack buffers, so ACL does not allocate
 * or own any memory that would need to survive sandstone's fork-per-iteration.
 *
 * Logging follows SDCShield convention: pass path is silent; on mismatch the
 * failing inputs and actual-vs-golden outputs are dumped via log_data() before
 * the thread is marked failed via report_fail_msg().
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>
#include <cmath>
#include <thread>

// ACL 核心头文件
#include <arm_compute/runtime/NEON/NEFunctions.h>
#include <arm_compute/runtime/Scheduler.h>
#include <arm_compute/core/TensorInfo.h>

/* 元素数 NUM_ELEMENTS(1024) — 运行时可配置, 统一优先级链:
 *   1. -O fisttp_arm.num_elements=N / 配置文件同名条目
 *   2. 环境变量 SDC_FISTTP_ARM_NUM_ELEMENTS
 *   3. 自适应: 每线程 input(4B)+output(4B)+store(4B)+reload(4B)+
 *      golden(4B) = 20B/元素, 占物理内存 60%, clamp [1024, 16M]。
 *      线性缓冲用 sdc_auto_linear_count（sqrt 缩放会欠缩放压力）。
 * ACL tensor 维度在 init 阶段 configure, 尺寸在 init 内解析一次。 */
static size_t fisttp_num_elements(struct test *test)
{
    /* hi 动态: 大内存机器按 60% 预算放开, 小内存机器保持 16M 收口;
     * 但每轮 run 对全缓冲做 生成+golden+cast+memcpy+memcmp 全量遍历,
     * hi 再按"单轮遍历 <= ~240s (~2GB elem/s/线程)"封顶, 否则单轮
     * 超过框架 hard timeout (5*duration+30s, >=300s) 会被判 timed out
     * (376GB 机 -n 2 自适应 6.06e9 元素实测 327s 超时)。 */
    int64_t hi = sdc_linear_hi(20, 1, 1.0, 600, thread_count(), 16 << 20);
    const int64_t hi_time = 2000000000;   /* 单轮全量遍历时间安全上限 */
    if (hi > hi_time)
        hi = hi_time;
    int64_t v = sdc_knob_int_clamped(test, "num_elements", 0, 1024, hi);
    if (v == 0)
        sdc_env_knob_int_clamped("fisttp_arm", "num_elements", &v, 1024, hi);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(20, 1, 1.0, 600, thread_count(),
                                         1024, hi, test->id, "num_elements");
}

static constexpr float FLOAT_MIN = -100.0f;
static constexpr float FLOAT_MAX = 100.0f;

// 单个线程专属的资源上下文。定长栈数组 → 堆指针, 支持运行时尺寸。
struct PerThreadContext {
    alignas(128) float *input;
    alignas(128) int32_t *output;
    alignas(128) int32_t *store_buf;
    alignas(128) int32_t *reload_buf;
    // 每线程独立 golden（run 每迭代重掷时无共享写竞争）
    alignas(128) int32_t *golden_output;
    size_t num_elements;

    arm_compute::Tensor src_tensor{};
    arm_compute::Tensor dst_tensor{};
    arm_compute::NECast cast_op{};
};

struct SharedTestData {
    // Operands/goldens live per-thread in PerThreadContext (re-rolled each
    // iteration in run); only the ACL operator contexts are shared here.
    // 使用 unique_ptr 确保内存地址绝对固定，避免 resize/push_back 触发析构和深浅拷贝问题
    std::vector<std::unique_ptr<PerThreadContext>> thread_ctx;
};

static inline int32_t truncate_to_int(float val) {
    return static_cast<int32_t>(std::trunc(val));
}

// ============================================================================
// 初始化：生成固定随机输入，预计算黄金结果。所有线程共享同一份只读数据。
// 在单线程 init 阶段预先 configure 所有算子，确保多线程运行阶段零数据竞争。
// ============================================================================
static int fisttp_arm_init(struct test *test) {
    // 1. 禁用 ACL 内部多线程，防止 ACL 的内部工作线程与 Sandstone 的绑核线程发生竞争
    arm_compute::Scheduler::get().set_num_threads(1);

    auto *shared_data = new (std::nothrow) SharedTestData;
    if (!shared_data) return EXIT_FAILURE;

    /* ctx 数按实际并发线程数分配(非 num_cpus()): 内存自适应模型的预算按
     * thread_count 分摊, 若按 num_cpus() 全机分配, -n 2 时实际驻留
     * = 64/2 倍预算(本机 64 核实测 375s 超时/内存远超模型)。
     * run() 的 cpu 索引 < thread_count, 线程数足够覆盖。 */
    int max_cpus = thread_count();
    if (max_cpus <= 0) max_cpus = static_cast<int>(std::thread::hardware_concurrency());
    if (max_cpus <= 0) max_cpus = 256;

    shared_data->thread_ctx.reserve(max_cpus);

    // Operands are re-rolled every iteration in fisttp_arm_run from the
    // framework's per-thread RNG (frandomf_scale, -s reproducible) into
    // per-thread buffers, so no shared operand state is needed here anymore
    // (the fixed mt19937_64 seed that made every run byte-identical is gone).
    const size_t num_elements = fisttp_num_elements(test);

    // 2. 在单线程 Init 阶段预先初始化并 configure 所有算子，确保多线程运行阶段零数据竞争
    arm_compute::TensorInfo src_info(arm_compute::TensorShape(num_elements), 1, arm_compute::DataType::F32);
    arm_compute::TensorInfo dst_info(arm_compute::TensorShape(num_elements), 1, arm_compute::DataType::S32);

    for (int i = 0; i < max_cpus; ++i) {
        auto ctx = std::make_unique<PerThreadContext>();
        ctx->num_elements = num_elements;
        ctx->input         = (float *)  aligned_alloc(128, num_elements * sizeof(float));
        ctx->output        = (int32_t *)aligned_alloc(128, num_elements * sizeof(int32_t));
        ctx->store_buf     = (int32_t *)aligned_alloc(128, num_elements * sizeof(int32_t));
        ctx->reload_buf    = (int32_t *)aligned_alloc(128, num_elements * sizeof(int32_t));
        ctx->golden_output = (int32_t *)aligned_alloc(128, num_elements * sizeof(int32_t));
        if (!ctx->input || !ctx->output || !ctx->store_buf ||
            !ctx->reload_buf || !ctx->golden_output) {
            report_fail_msg("fisttp_arm: per-thread buffer alloc failed for "
                            "num_elements=%zu (reduce fisttp_arm.num_elements)",
                            num_elements);
            return EXIT_FAILURE;
        }

        ctx->src_tensor.allocator()->init(src_info);
        ctx->dst_tensor.allocator()->init(dst_info);

        ctx->src_tensor.allocator()->import_memory(ctx->input);
        ctx->dst_tensor.allocator()->import_memory(ctx->output);

        // 核心安全保证：configure 仅在 init 阶段单线程调用一次
        ctx->cast_op.configure(&ctx->src_tensor, &ctx->dst_tensor, arm_compute::ConvertPolicy::WRAP);

        shared_data->thread_ctx.push_back(std::move(ctx));
    }

    test->data = shared_data;
    return EXIT_SUCCESS;
}

// 运行测试：每次迭代调用 ACL NECast (FCVTZS) 并与 golden 比较
static int fisttp_arm_run(struct test *test, int cpu) {
    auto *shared_data = static_cast<SharedTestData*>(test->data);
    if (!shared_data) return EXIT_FAILURE;

    size_t cpu_idx = (cpu >= 0 && static_cast<size_t>(cpu) < shared_data->thread_ctx.size())
                     ? static_cast<size_t>(cpu) : 0;
    auto &ctx = *shared_data->thread_ctx[cpu_idx];

    do {
        const size_t NUM_ELEMENTS = ctx.num_elements;
        // 1. 输入数据准备：每迭代重掷（框架 RNG 按线程独立流，无共享竞争），
        //    golden 同迭代用独立标量 truncate_to_int 重算到每线程缓冲（复现铁律）
        for (size_t i = 0; i < NUM_ELEMENTS; ++i) {
            ctx.input[i] = frandomf_scale(FLOAT_MAX - FLOAT_MIN) + FLOAT_MIN;
            ctx.golden_output[i] = truncate_to_int(ctx.input[i]);
        }

        // 2. 压测运行：仅调用预先 configure 好的 run() 函数
        ctx.cast_op.run();

        // 3. 结果校验（FCVTZS 转换为整数比对 + Cache 一致性比对）
        bool data_ok = (std::memcmp(ctx.output, ctx.golden_output, NUM_ELEMENTS * sizeof(int32_t)) == 0);

        std::memcpy(ctx.store_buf, ctx.output, NUM_ELEMENTS * sizeof(int32_t));
        std::memcpy(ctx.reload_buf, ctx.store_buf, NUM_ELEMENTS * sizeof(int32_t));
        bool consistent = (std::memcmp(ctx.reload_buf, ctx.output, NUM_ELEMENTS * sizeof(int32_t)) == 0);

        if (!data_ok || !consistent) {
            // Fail 详查：把本次输入与输出落进 yaml 的 data: 字段，再用 report_fail_msg
            // 记录失败位置（框架自动标 thread failed）。report_fail_msg 是 noreturn。
            // 定位首个不符元素
            int mismatch_idx = -1;
            for (size_t i = 0; i < NUM_ELEMENTS; ++i) {
                if (ctx.output[i] != ctx.golden_output[i]) { mismatch_idx = (int)i; break; }
            }
            char ctx_msg[200];
            snprintf(ctx_msg, sizeof(ctx_msg),
                     "fisttp_arm (ACL): conversion mismatch on CPU %d at idx %d "
                     "(out=%d golden=%d, data_ok=%d, consistent=%d)",
                     cpu, mismatch_idx,
                     mismatch_idx >= 0 ? ctx.output[mismatch_idx] : 0,
                     mismatch_idx >= 0 ? ctx.golden_output[mismatch_idx] : 0,
                     (int)data_ok, (int)consistent);
            log_data("fisttp input (float, runtime-sized)",
                     ctx.input, NUM_ELEMENTS * sizeof(float));
            log_data("fisttp output (int32 ACL FCVTZS result, runtime-sized)",
                     ctx.output, NUM_ELEMENTS * sizeof(int32_t));
            log_data("fisttp golden (std::trunc int32 result, runtime-sized)",
                     ctx.golden_output, NUM_ELEMENTS * sizeof(int32_t));
            report_fail_msg("%s", ctx_msg);
            // report_fail_msg 不返回
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int fisttp_arm_finish(struct test *test) {
    auto *shared_data = static_cast<SharedTestData*>(test->data);
    if (shared_data) {
        for (auto &ctx : shared_data->thread_ctx) {
            free(ctx->input);
            free(ctx->output);
            free(ctx->store_buf);
            free(ctx->reload_buf);
            free(ctx->golden_output);
        }
        delete shared_data;
    }
    test->data = nullptr;
    return EXIT_SUCCESS;
}

// ============================================================================
// 测试声明
// ============================================================================
DECLARE_TEST(fisttp_arm, "Floating-point to integer conversion (FCVTZS) via Arm Compute Library")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fisttp_arm_init,
    .test_run = fisttp_arm_run,
    .test_cleanup = fisttp_arm_finish,
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
