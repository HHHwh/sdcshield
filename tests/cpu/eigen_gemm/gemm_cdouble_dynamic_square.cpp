/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_gemm_cdouble_dynamic_square
 * @parblock
 * This piece of code aims to stress test the exec (in particular FMA)
 * by repetitively solving general matrix matrix multiplication.  The
 * multiplication function is from the 3rd party library Eigen.  A
 * pair of random matrices are generated as inputs and then multiplied
 * using Eigen's gemm. The multiplication result is compared against a
 * golden result that is computed during init.  This particular
 * version of the test goes for double precision, complex numbers
 * input matrices.  Although the test should run fine on a single
 * thread, it is only expected to catch defects if run on at least 2
 * cores.
 *
 * @note This test requires at least 2 threads to run.
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"

#include <Eigen/Core>
#include <cstdint>
using namespace Eigen;

typedef Matrix < std::complex < double >, Dynamic, Dynamic > Mat;

#if defined(__aarch64__) && defined(__linux__)
/* 联合求解 sizing 探针(T9): 填充与 run() 的 fill_rand 同谱系(均匀随机
 * [-1,1) 独立 re/im, 检视 #51)——splitmix 确定性哈希满足该契约, 且不
 * 依赖 init aux 线程上未定义的 per-thread RNG 状态。GEMM 计时对数值
 * 不敏感(无 SVD 式 deflation 路径), 谱系由分布一致性满足; 填充 O(n²)
 * 相对乘法 O(n³) 已计入探针计时(保守方向)。同 T6 gemm_double_max_arm
 * _body 的 solve_once(complex 四流: a.re/a.im/b.re/b.im)。 */
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
            std::uint64_t h3 = h2 * 0xD6E8FEB86659FD93ull
                             + 0x9E3779B97F4A7C15ull;
            h3 ^= h3 >> 32;
            std::uint64_t h4 = h3 * 0xD6E8FEB86659FD93ull
                             + 0x9E3779B97F4A7C15ull;
            h4 ^= h4 >> 32;
            a(r, col) = std::complex<double>(
                (double)(h % 2000u) / 1000.0 - 1.0,
                (double)(h2 % 2000u) / 1000.0 - 1.0);
            b(r, col) = std::complex<double>(
                (double)(h3 % 2000u) / 1000.0 - 1.0,
                (double)(h4 % 2000u) / 1000.0 - 1.0);
        }
    c = a * b;
    return 0;
}
#endif

/*
 * 矩阵维度: 优先级链同 eigen_gemm_double_dynamic_square; complex<double>
 * 元素 16 字节, 自适应按 16B/元素计算 (历史硬编码 221 是刻意的"怪"维度,
 * 显式配置时保留任意值语义)。尺寸只在 test_init 求解一次并缓存进
 * eigen_test_data.dim——worker 读缓存不重算(T9, 旧链的 run 期重跑
 * 自适应路径已删除)。
 */
static int gemm_mdim(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "mdim", 0, 16, 4096);
    if (v == 0) {
        int64_t envv = 0;
        if (sdc_env_knob_int("eigen_gemm_cdouble_dynamic_square", "mdim", &envv))
            v = (envv < 16) ? 16 : (envv > 4096 ? 4096 : envv);
    }
    if (v > 0)
        return (int)v;
#if defined(__aarch64__) && defined(__linux__)
    /* 联合求解(T9, 参数依据见 gemm_double_dynamic_square.cpp 同位注释;
     * 本文件差异仅 fam GEMM_C128, 16B/元素)。 */
    struct sdc_joint_args ja = {
        .lo = 16, .hi = 4096,
        .fam = SDC_RSS_GEMM_C128,   /* 0.0631 KB/n²/线程, 实测表值 */
        .bytes_per_elem = 16, .matrices_per_thread = 4, .rss_ratio = 6.0,
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
    return sdc_auto_square_dim(16, 4, 6.0, 850, thread_count(), 16, 4096, test->id, "mdim");
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

static int eigen_gemm_cdouble_dynamic_square_init(struct test *test) {
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

static int eigen_gemm_cdouble_dynamic_square_run(struct test *test, int cpu) {
    /* randomization hardening H11' (P14): per-thread operands re-rolled
     * every kRerollEvery iterations (framework RNG, -s reproducible,
     * [-1, 1) bounded like Mat::Random); golden recomputed the same
     * batch. No shared state is written after init. */
    constexpr int kRerollEvery = 16;
        auto fill_rand = [](Mat &m) {
            for (int r = 0; r < m.rows(); ++r)
                for (int c = 0; c < m.cols(); ++c)
                    m(r, c) = std::complex<double>(frandom_scale(2.0) - 1.0,
                                                   frandom_scale(2.0) - 1.0);
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
        Mat x;
        x = l_lhs * l_rhs;

        memcmp_or_fail(reinterpret_cast<double *>(x.data()),
                       reinterpret_cast<double *>(l_prod.data()),
                       2 * (size_t)dim * (size_t)dim);

        if (++since_reroll >= kRerollEvery) {
            since_reroll = 0;
            fill_rand(l_lhs);
            fill_rand(l_rhs);
            l_prod = l_lhs * l_rhs;
        }
    } while (test_time_condition(test));
    return EXIT_SUCCESS;
}

static int eigen_gemm_cdouble_dynamic_square_finish(struct test *test) {
    delete(CAST(test->data));
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_gemm_cdouble_dynamic_square, "Eigen GEMM payload (cplx double, dynamic, square)")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_gemm_cdouble_dynamic_square_init,
  .test_run = eigen_gemm_cdouble_dynamic_square_run,
  .test_cleanup = eigen_gemm_cdouble_dynamic_square_finish,
  .fracture_loop_count = 5,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
