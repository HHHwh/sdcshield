/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_gemm_float_dynamic_square
 * @parblock
 * This piece of code aims to stress test the exec (in particular FMA)
 * by repetitively solving general matrix matrix multiplication.
 * The multiplication function is from the 3rd party library Eigen.
 * A pair of random matrices are generated as inputs and then multiplied
 * using Eigen's gemm. The multiplication result is compared against a
 * golden result that is computed during init.
 * This particular version of the test goes for single precision input
 * matrices.
 *
 * @note Although the test should run fine on a single thread, it is
 * only expected to catch defects if run on at least 2 cores.
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"

#include <Eigen/Core>
#include <cstdint>
#include <new>
using namespace Eigen;

typedef Matrix < float, Dynamic, Dynamic > Mat;

#if defined(__aarch64__) && defined(__linux__)
/* 联合求解 sizing 探针(T9): 填充与 run() 的 fill_rand 同谱系(均匀随机
 * [-1,1), 检视 #51)——splitmix 确定性哈希满足该契约, 且不依赖 init aux
 * 线程上未定义的 per-thread RNG 状态。GEMM 计时对数值不敏感(无 SVD 式
 * deflation 路径), 谱系由分布一致性满足; 填充 O(n²) 相对乘法 O(n³) 已
 * 计入探针计时(保守方向)。同 T6 gemm_double_max_arm_body 的 solve_once
 * (float 标量转型)。 */
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
            a(r, col) = (float)((double)(h % 2000u) / 1000.0 - 1.0);
            b(r, col) = (float)((double)(h2 % 2000u) / 1000.0 - 1.0);
        }
    c = a * b;
    return 0;
}
#endif

/*
 * 矩阵维度: 同 eigen_gemm_double_dynamic_square 的优先级链
 * (-O mdim / 配置文件 / SDC_EIGEN_GEMM_FLOAT_DYNAMIC_SQUARE_MDIM /
 * 自适应)。float 元素 4 字节, 相同内存预算下可容纳更大 N, clamp
 * [16, 8192]。尺寸只在 test_init 求解一次并缓存进 eigen_test_data.dim
 * ——worker 读缓存不重算(T9, 旧链的 run 期重跑自适应路径已删除)。
 */
static int gemm_mdim(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "mdim", 0, 16, 8192);
    if (v == 0) {
        int64_t envv = 0;
        if (sdc_env_knob_int("eigen_gemm_float_dynamic_square", "mdim", &envv))
            v = (envv < 16) ? 16 : (envv > 8192 ? 8192 : envv);
    }
    if (v > 0)
        return (int)v;
#if defined(__aarch64__) && defined(__linux__)
    /* 联合求解(T9, 参数依据见 gemm_double_dynamic_square.cpp 同位注释;
     * 本文件差异仅 clamp [16,8192] 与 fam GEMM_F32)。 */
    struct sdc_joint_args ja = {
        .lo = 16, .hi = 8192,
        .fam = SDC_RSS_GEMM_F32,    /* 0.0159 KB/n²/线程, 实测表值 */
        .bytes_per_elem = 4, .matrices_per_thread = 4, .rss_ratio = 6.0,
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
    return sdc_auto_square_dim(4, 4, 6.0, 850, thread_count(), 16, 8192, test->id, "mdim");
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

static int eigen_gemm_float_dynamic_square_init(struct test *test) {
    test->data = new(eigen_test_data);
    auto d = CAST(test->data);
    int dim = 0;
    try {
        dim = gemm_mdim(test);   /* knob > env > 自适应, 只解一次 */
        d->dim = dim;
        d->lhs = Mat::Random(dim, dim);
        d->rhs = Mat::Random(dim, dim);
        d->prod = d->lhs * d->rhs;
    } catch (const std::bad_alloc &) {
        /* T10 init 降级(m2, 对齐 eigen_sparse 先例): init 期分配失败是
         * 资源性而非 SDC——降 mdim 到 lo(=16, 与 gemm_mdim 的联合求解
         * lo/clamp 下限同值)重分配一次, 仍失败才资源性 SKIP。原
         * catch(...)->report_fail 对 bad_alloc 打红撤销(其余异常保留)。 */
        constexpr int kInitRetryLo = 16;
        log_warning("[init 降级] %s: 共享矩阵分配失败 mdim=%d → 降档 mdim=%d 重试一次",
                    test->id, dim, kInitRetryLo);
        try {
            d->dim = kInitRetryLo;
            d->lhs = Mat::Random(kInitRetryLo, kInitRetryLo);
            d->rhs = Mat::Random(kInitRetryLo, kInitRetryLo);
            d->prod = d->lhs * d->rhs;
        } catch (const std::bad_alloc &) {
            log_skip(RuntimeSkipCategory, "memory: %s mdim=%d",
                     test->id, kInitRetryLo);
            return EXIT_SKIP;
        }
        /* 降档成功 → 同步反馈记账口径(PR#302 检视 #25): 否则 [反馈] 按
         * 求解 n 折算 eff, c 被压向 0.7× 下限, 后续 n 向 OOM 膨胀。 */
        sdc_joint_cache_note_dim(test, kInitRetryLo);
    } catch (...) {
        report_fail_msg("Exception on Eigen code, most probably OOM");
    }
    return EXIT_SUCCESS;
}

static int eigen_gemm_float_dynamic_square_run(struct test *test, int cpu) {
    /* randomization hardening H11' (P14): per-thread operands re-rolled
     * every kRerollEvery iterations (framework RNG, -s reproducible,
     * [-1, 1) bounded like Mat::Random); golden recomputed the same
     * batch. No shared state is written after init. */
    constexpr int kRerollEvery = 16;
        auto fill_rand = [](Mat &m) {
            for (int r = 0; r < m.rows(); ++r)
                for (int c = 0; c < m.cols(); ++c)
                    m(r, c) = frandomf_scale(2.0f) - 1.0f;
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
        memcmp_or_fail(x.data(), l_prod.data(), (size_t)dim * (size_t)dim);

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

static int eigen_gemm_float_dynamic_square_finish(struct test *test) {
    delete(CAST(test->data));
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_gemm_float_dynamic_square, "Eigen GEMM payload (float, dynamic, square)")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_gemm_float_dynamic_square_init,
  .test_run = eigen_gemm_float_dynamic_square_run,
  .test_cleanup = eigen_gemm_float_dynamic_square_finish,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
