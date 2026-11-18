/**
 * @file
 *
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_gemm_double_dynamic_square_sve
 * @parblock
 * ARM64 SVE-backed counterpart of @ref eigen_gemm_double_dynamic_square。
 *
 * 同一 Eigen GEMM(double, dynamic, square)负载,payload 以 SVE packet
 * 后端按档编译(128/256/512/1024),dispatcher 按框架 pin 的 VL 分派
 * (模式同 @ref eigen_gemm_double_max_arm;样板收敛于
 * eigen_svd/sandstone_eigen_sve_dispatch.h)。run 语义逐字镜像原用例
 * (per-thread operands + golden 同源首算 + kRerollEvery 重掷 +
 * memcmp_or_fail),body 在 gemm_f64_sve_body.cpp(与
 * eigen_gemm_double14_sve 共享,同一计算,lo-hi 差异在各自 TU)。
 *
 * 维度:knob/env > 联合求解,镜像 eigen_gemm_double_dynamic_square 的
 * clamp [16, 4096] 与 GEMM_F64 联合层参数口径;
 * override: -O eigen_gemm_double_dynamic_square_sve.mdim=N (16..4096)。
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"
#include "eigen_svd/sandstone_eigen_sve_dispatch.h"

/* 既有先例 svd_double_sve.cpp:全 TU 在 aarch64 guard 内,无 #else——
 * dispatcher TU 只在 aarch64 编进 tests_sve,x86-64 零注册(rule 5)。 */
#if defined(__aarch64__)

#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>

EIGEN_SVE_DECLARE_BODY_NS(gemm_f64)
EIGEN_SVE_DEFINE_DISPATCHERS(gemm_f64)

#define MDIM_MIN 16
#define MDIM_MAX 4096

static int eigen_gemm_double_dynamic_square_sve_init(struct test *test)
{
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
    if (!gemm_f64_tier_select(test, svl))
        return EXIT_SKIP;

    int dim;
    int64_t knob = sdc_knob_int_clamped(test, "mdim", 0, MDIM_MIN, MDIM_MAX);
    if (knob == 0)
        sdc_env_knob_int_clamped("eigen_gemm_double_dynamic_square_sve", "mdim",
                                 &knob, MDIM_MIN, MDIM_MAX);
    if (knob > 0) {
        dim = (int)knob;
    } else {
        struct sdc_joint_args ja = {
            .lo = MDIM_MIN, .hi = MDIM_MAX,
            .fam = SDC_RSS_GEMM_F64,  /* RSS 表项与 NEON 版共用(同算法同矩阵数) */
            .bytes_per_elem = 8, .matrices_per_thread = 4, .rss_ratio = 6.0,
            .shared_bytes_per_slice = 0,   /* make_data holds no matrices */
            .mem_fraction_permille = 850,
            .ar_fam = SDC_FAM_COMPUTE_BOUND,
            .run_budget_ms = 200000,   /* 全核单迭代预算,镜像原用例联合层值 */
        };
        try {
            dim = sdc_joint_dim_slice(test, "mdim", &ja, gemm_f64_solve_once);
        } catch (...) {
            log_warning("[联合求解] %s: mdim 探针异常 → 退守下限 %d",
                        test->id, MDIM_MIN);
            dim = MDIM_MIN;
        }
    }
    test->data = gemm_f64_make_data(dim);
    return EXIT_SUCCESS;
}

/* DECLARE_TEST 需要普通函数符号——一行 wrapper(DECLARE_TEST 之前定义)。 */
static int eigen_gemm_double_dynamic_square_sve_run(struct test *test, int cpu)
{
    return gemm_f64_run(test, cpu);
}

static int eigen_gemm_double_dynamic_square_sve_cleanup(struct test *test)
{
    gemm_f64_cleanup(test);
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_gemm_double_dynamic_square_sve, "Eigen GEMM payload (double, dynamic, square) on the ARM64 SVE packet backend (per-tier 128/256/512/1024-bit variants selected at the framework-pinned VL); counterpart of eigen_gemm_double_dynamic_square")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_gemm_double_dynamic_square_sve_init,
  .test_run = eigen_gemm_double_dynamic_square_sve_run,
  .test_cleanup = eigen_gemm_double_dynamic_square_sve_cleanup,
  .quality_level = TEST_QUALITY_PROD,
  .arm64_requires = ARM64_REQUIRES_SVE,
END_DECLARE_TEST

#endif // __aarch64__
