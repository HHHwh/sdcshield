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
#include "gemm_solve_probe.h"
#include "sandstone_gemm_common.h"

#include <Eigen/Core>
#include <cstdint>
#include <new>
using namespace Eigen;

typedef Matrix < std::complex < double >, Dynamic, Dynamic > Mat;

#if defined(__aarch64__) && defined(__linux__)
/* 联合求解 sizing 探针(T9): 谱系契约(检视 #51: 填充与 run() 的
 * fill_rand 同谱系, 均匀随机 [-1,1) 独立 re/im)已单点收口至
 * gemm_solve_probe.h(issue #389, 含 T6 gemm_double_max_arm_body 源头
 * 共 8→1)——填充唯一定义, 探针副本间漂移在编译期不可能;
 * probe↔run() 谱系契约仍需与 fill_rand 手工同步演化(无编译期
 * 绑定, 检视 #392-R3; complex 四流:
 * a.re/a.im/b.re/b.im 经 if constexpr 分支)。 */
static int gemm_solve_once(int n)
{
    return gemm_probe::solve_once<Mat>(n);
}
#endif

/*
 * 矩阵维度: 优先级链同 eigen_gemm_double_dynamic_square; complex<double>
 * 元素 16 字节, 自适应按 16B/元素计算 (历史硬编码 221 是刻意的"怪"维度,
 * 显式配置时保留任意值语义)。尺寸只在 test_init 求解一次并缓存进
 * eigen_test_data.dim——worker 读缓存不重算(T9, 旧链的 run 期重跑
 * 自适应路径已删除)。
 * is_explicit 出参(issue #488): 显式通道(knob/env)来源时置 true,
 * 供 sdc_gemm_init_shared 的 bad_alloc 分流(显式值不可降档)。
 */
static int gemm_mdim(struct test *test, bool *is_explicit)
{
    int64_t v = sdc_knob_int_clamped(test, "mdim", 0, 16, 4096);
    if (v == 0) {
        int64_t envv = 0;
        if (sdc_env_knob_int("eigen_gemm_cdouble_dynamic_square", "mdim", &envv))
            v = (envv < 16) ? 16 : (envv > 4096 ? 4096 : envv);
    }
    if (v > 0) {
        *is_explicit = true;    /* knob(-O/配置文件)/env 显式通道 */
        return (int)v;
    }
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
    return sdc_gemm_init_shared(test, CAST(test->data), gemm_mdim);
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
    /* T10 bad_alloc 降级(m2, 对齐 eigen_sparse 先例): 分配/求解段包窄
     * catch(仅 std::bad_alloc)→ 线程级 SKIP——资源性失败不是 SDC 不打红;
     * 比对段(memcmp_or_fail → report_fail)恒在 catch 之外, SDC 误配必须
     * 打红。worker 期不做降档重试(m2: 改共享 d->dim 有跨线程竞态,
     * v1 的重试+FAIL 语义与先例相反, 已撤销)。 */
    Mat l_lhs, l_rhs, l_prod;
    try {
        l_lhs.resize(dim, dim);
        l_rhs.resize(dim, dim);
        l_prod.resize(dim, dim);
        fill_rand(l_lhs);
        fill_rand(l_rhs);
        l_prod = l_lhs * l_rhs;             /* golden 同源首算 */
    } catch (const std::bad_alloc &) {
        log_skip(RuntimeSkipCategory, "memory: %s mdim=%d", test->id, dim);
        return EXIT_SKIP;
    }
    int since_reroll = 0;

    do {
        Mat x;
        try {
            x = l_lhs * l_rhs;              /* DUT 求解 */
        } catch (const std::bad_alloc &) {
            log_skip(RuntimeSkipCategory, "memory: %s mdim=%d", test->id, dim);
            return EXIT_SKIP;
        }

        /* 比对段在 catch 之外(T10 边界契约, 见 run 内头注)。 */
        memcmp_or_fail(reinterpret_cast<double *>(x.data()),
                       reinterpret_cast<double *>(l_prod.data()),
                       2 * (size_t)dim * (size_t)dim);

        if (++since_reroll >= kRerollEvery) {
            since_reroll = 0;
            try {
                fill_rand(l_lhs);
                fill_rand(l_rhs);
                l_prod = l_lhs * l_rhs;     /* golden 重算 */
            } catch (const std::bad_alloc &) {
                log_skip(RuntimeSkipCategory, "memory: %s mdim=%d", test->id, dim);
                return EXIT_SKIP;
            }
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
