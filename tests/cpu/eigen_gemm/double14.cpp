/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_gemm_double14
 * @parblock
 * This piece of code aims to stress test the exec (in particular FMA)
 * by repetitively solving general matrix matrix multiplication.  The
 * multiplication function is from the 3rd party library Eigen.  A
 * pair of random matrices are generated as inputs and then multiplied
 * using Eigen's gemm. The multiplication result is compared against a
 * golden result that is computed during init.  This particular
 * version of the test goes for double precision input matrices. This
 * variation adds extra copies and consistency checks.
 *
 * @note Although the test should run fine on a single thread, it is
 * only expected to catch defects if run on at least 2 cores.
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"

#include <Eigen/Core>
#include <cstdint>
using namespace Eigen;

typedef Matrix < double, Dynamic, Dynamic > Mat;

#if defined(__aarch64__) && defined(__linux__)
/* 联合求解 sizing 探针(T9): 填充与 run() 的 fill_rand 同谱系(均匀随机
 * [-1,1), 检视 #51)——splitmix 确定性哈希满足该契约, 且不依赖 init aux
 * 线程上未定义的 per-thread RNG 状态。GEMM 计时对数值不敏感(无 SVD 式
 * deflation 路径), 谱系由分布一致性满足; 填充 O(n²) 相对乘法 O(n³) 已
 * 计入探针计时(保守方向)。同 T6 gemm_double_max_arm_body 的 solve_once。 */
static int gemm_solve_once(int n)
{
    Mat a(n, n), b(n, n), c;
    for (int r = 0; r < n; ++r)
        for (int col = 0; col < n; ++col) {
            std::uint64_t h = (std::uint64_t)r * 0x9E3779B97F4A7C15ull
                            ^ (std::uint64_t)col * 0xC2B2AE3D27D4EB4Full;
            h ^= h >> 32;
            h *= 0xD6E8FEB86659FD93ull;
            h ^= h >> 32;
            std::uint64_t h2 = h * 0xD6E8FEB86659FD93ull
                             + 0x9E3779B97F4A7C15ull;
            h2 ^= h2 >> 32;
            a(r, col) = (double)(h % 2000u) / 1000.0 - 1.0;
            b(r, col) = (double)(h2 % 2000u) / 1000.0 - 1.0;
        }
    c = a * b;
    return 0;
}
#endif

/* 矩阵维度: 优先级链同 eigen_gemm_double_dynamic_square
 * (-O eigen_gemm_double14.mdim=N / 配置文件 / 环境变量 / 自适应)。
 * 尺寸只在 test_init 求解一次并缓存进 eigen_test_data.dim——worker
 * 读缓存不重算(T9, 旧链的 run 期重跑自适应路径已删除)。 */
static int gemm_mdim(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "mdim", 0, 16, 4096);
    if (v == 0) {
        int64_t envv = 0;
        if (sdc_env_knob_int("eigen_gemm_double14", "mdim", &envv))
            v = (envv < 16) ? 16 : (envv > 4096 ? 4096 : envv);
    }
    if (v > 0)
        return (int)v;
#if defined(__aarch64__) && defined(__linux__)
    /* 联合求解(T9)——但对本用例刻意不走 RSS 表: double14 的 run 循环
     * 每线程同时驻留 6 个矩阵(l_lhs/l_rhs/l_prod + 循环内 _x/_y/_prod,
     * 后三者与前三者在 _prod = _x*_y 期间并存), 是 dynamic_square 家族
     * 其余 6 文件(4 矩阵: l_* 三连 + reroll 乘积 x)的 1.5 倍。RSS 表
     * GEMM_F64(0.0315 KB = 4 矩阵×8B, T6/T9 按 4 矩阵用例差分实测)对
     * 本用例系统性低估 1.5×: 按表值求解 n≈3.5k 时峰值需求 ≈ 74 GiB,
     * 对 ~46 GiB 节点预算必然 OOM/bad_alloc(run 期分配无 try/catch,
     * 直接 FAIL)。故走 legacy 三元组通道(fam=SDC_RSS_INVALID)并给
     * 诚实模型: 8B×6 矩阵×rss_ratio 1.0(差分口径实测 ≈ bytes×矩阵数
     * ×1.03, 见 GEMM_F64 表注释)→ c=48 B/n²/线程; VmHWM 反馈闭环照常
     * 生效(锚 = 该三元组)。若后续把 6 矩阵结构收编进 RSS 表(新增
     * family 条目), 此处应回迁 fam。其余参数依据见
     * gemm_double_dynamic_square.cpp 同位注释(clamp [16,4096] /
     * shared 3×8×n² <1% 忽略 / run_budget 200000 / AR 已知边界)。 */
    struct sdc_joint_args ja = {
        .lo = 16, .hi = 4096,
        .fam = SDC_RSS_INVALID,     /* 无 6 矩阵 GEMM 表条目 → legacy 三元组 */
        .bytes_per_elem = 8, .matrices_per_thread = 6, .rss_ratio = 1.0,
        .shared_bytes_per_slice = 0,
        .mem_fraction_permille = 850,
        .ar_fam = SDC_FAM_COMPUTE_BOUND,
        .run_budget_ms = 200000,    /* 全核单迭代(填充+一次乘法×AR)预算 */
    };
    try {
        return sdc_joint_dim_slice(test, "mdim", &ja, gemm_solve_once);
    } catch (...) {
        log_warning("[联合求解] %s.%s: 探针异常 → 退守下限 %d",
                    test->id, "mdim", 16);
        return 16;
    }
#else
    return sdc_auto_square_dim(8, 4, 6.0, 850, thread_count(), 16, 4096, test->id, "mdim");
#endif
}

namespace {
struct eigen_test_data {
    Mat lhs;
    Mat rhs;
    Mat prod;
    int dim;    /* init 求解的最终维度(T9): worker 读缓存不重算;
                 * new(eigen_test_data) 默认初始化下未定值, init 在任何
                 * 可抛路径前赋值 */
};
}

#define CAST(_x) static_cast<struct eigen_test_data *>(_x)

static int eigen_gemm_double14_init(struct test *test) {
    test->data = new(eigen_test_data);
    auto d = CAST(test->data);
    try {
        const int dim = gemm_mdim(test);   /* knob > env > 自适应, 只解一次 */
        d->dim = dim;
        d->lhs = Mat::Random(dim, dim);
        d->rhs = Mat::Random(dim, dim);
        d->prod = d->lhs * d->rhs;
    } catch (...) {
        report_fail_msg("Exception on Eigen code, most probably OOM");
    }
    return EXIT_SUCCESS;
}

static int eigen_gemm_double14_run(struct test *test, int cpu) {
    /* randomization hardening H11' (P14): per-thread operands re-rolled
     * every kRerollEvery iterations (framework RNG, [-1, 1) bounded);
     * golden recomputed the same batch. */
    constexpr int kRerollEvery = 16;
    auto fill_rand = [](Mat &m) {
        for (int r = 0; r < m.rows(); ++r)
            for (int c = 0; c < m.cols(); ++c)
                m(r, c) = frandom_scale(2.0) - 1.0;
    };
    const int dim = CAST(test->data)->dim;   /* init 缓存值——worker 不重算 */
    Mat l_lhs, l_rhs, l_prod;
    l_lhs.resize(dim, dim);
    l_rhs.resize(dim, dim);
    l_prod.resize(dim, dim);
    fill_rand(l_lhs);
    fill_rand(l_rhs);
    l_prod = l_lhs * l_rhs;
    int since_reroll = 0;

    do {
        Mat _x;
        _x = l_lhs;
        Mat _y;
        _y = l_rhs;
        Mat _prod;
        _prod = _x * _y;

        if (!_x.isApprox(l_lhs)) {
                report_fail_msg("_x.isApprox failed");
        }
        memcmp_or_fail(_x.data(), l_lhs.data(), (size_t)dim * (size_t)dim);

        if (!_y.isApprox(l_rhs)) {
                report_fail_msg("_y.isApprox failed");
        }
        memcmp_or_fail(_y.data(), l_rhs.data(), (size_t)dim * (size_t)dim);

        if (!_prod.isApprox(l_prod)) {
                report_fail_msg("_prod.isApprox failed");
        }
        memcmp_or_fail(_prod.data(), l_prod.data(), (size_t)dim * (size_t)dim);

        if (++since_reroll >= kRerollEvery) {
            since_reroll = 0;
            fill_rand(l_lhs);
            fill_rand(l_rhs);
            l_prod = l_lhs * l_rhs;
        }
    } while (test_time_condition(test));
    return EXIT_SUCCESS;
}

static int eigen_gemm_double14_finish(struct test *test) {
    delete(CAST(test->data));
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_gemm_double14, "Eigen GEMM payload (double, dynamic, square)")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_gemm_double14_init,
  .test_run = eigen_gemm_double14_run,
  .test_cleanup = eigen_gemm_double14_finish,
  .fracture_loop_count = 4,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
