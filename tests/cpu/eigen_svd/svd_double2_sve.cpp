/**
 * @file
 *
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_svd_double2_sve
 * @parblock
 * ARM64 SVE-backed counterpart of @ref eigen_svd_double2。
 *
 * 同一 Eigen BDCSVD(double)负载,payload 以 SVE packet 后端按档编译
 * (128/256/512/1024),dispatcher 按框架 pin 的 VL 分派(模式同
 * @ref eigen_svd_cdouble_sve;样板收敛于 sandstone_eigen_sve_dispatch.h;
 * body 与 @ref eigen_svd_double_sve 共享 svd_f64 per-tier 套件)。
 * BDCSVD 第一阶段即 Golub-Kahan Householder 双对角化——本用例承接
 * 退役的 eigen_svd_bidiag_sve 的 dense SVE FMA 场景(后者手写内核,
 * 存在理由——vendored Eigen SVE packet SIGSEGV——已被 issue #410 消除)。
 * 历史注:double2 与 double 版是同一计算的重复注册(NEON 侧共用
 * init_common,历史硬编码 dim 128/256 已由自适应取代),SVE 版维持
 * 镜像对称——本 dispatcher 与 eigen_svd_double_sve 仅 test id/env 名/
 * 描述串/文件头注差异。
 *
 * 维度:knob/env > 联合求解,镜像 eigen_svd_double2 的
 * calibrated_auto_dim(test, 8, 6, 12.0, 650, 64, 2048) 参数口径;
 * override: -O eigen_svd_double2_sve.mdim=N (64..2048)。
 * @endparblock
 */

#include "sandstone_eigen_common.h"
#include "sandstone_eigen_sve_dispatch.h"
#include "sdc_stress_config.h"

/* 既有先例 svd_cdouble_sve.cpp:全 TU 在 aarch64 guard 内,无 #else——
 * dispatcher TU 只在 aarch64 编进 tests_sve,x86-64 零注册(rule 5)。 */
#if defined(__aarch64__)

#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>

EIGEN_SVE_DECLARE_BODY_NS(svd_f64)
EIGEN_SVE_DEFINE_DISPATCHERS(svd_f64)

#define MDIM_MIN 64
#define MDIM_MAX 2048

static int eigen_svd_double2_sve_init(struct test *test)
{
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
    if (!svd_f64_tier_select(test, svl))
        return EXIT_SKIP;

    int dim;
    int64_t knob = sdc_knob_int_clamped(test, "mdim", 0, MDIM_MIN, MDIM_MAX);
    if (knob == 0)
        sdc_env_knob_int_clamped("eigen_svd_double2_sve", "mdim", &knob,
                                 MDIM_MIN, MDIM_MAX);
    if (knob > 0) {
        dim = (int)knob;
    } else {
        struct sdc_joint_args ja = {
            .lo = MDIM_MIN, .hi = MDIM_MAX,
            .fam = SDC_RSS_BDCSVD_F64,  /* RSS 表项与 NEON 版共用(同算法同矩阵数) */
            .bytes_per_elem = 8, .matrices_per_thread = 6, .rss_ratio = 12.0,
            .shared_bytes_per_slice = 0,   /* make_data holds no matrices */
            .mem_fraction_permille = 650,
            .ar_fam = SDC_FAM_MEM_BOUND,
            .run_budget_ms = 200000,   /* 镜像 sandstone_eigen_common.h 联合层值 */
        };
        try {
            dim = sdc_joint_dim_slice(test, "mdim", &ja, svd_f64_solve_once);
        } catch (...) {
            log_warning("[联合求解] %s: mdim 探针异常 → 退守下限 %d",
                        test->id, MDIM_MIN);
            dim = MDIM_MIN;
        }
    }
    test->data = svd_f64_make_data(dim);
    return EXIT_SUCCESS;
}

/* DECLARE_TEST 需要普通函数符号——一行 wrapper(DECLARE_TEST 之前定义)。 */
static int eigen_svd_double2_sve_run(struct test *test, int cpu)
{
    return svd_f64_run(test, cpu);
}

static int eigen_svd_double2_sve_cleanup(struct test *test)
{
    svd_f64_cleanup(test);
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_svd_double2_sve, "Eigen SVD (Singular Value Decomposition) solving payload on the ARM64 SVE packet backend (per-tier 128/256/512/1024-bit variants selected at the framework-pinned VL); counterpart of eigen_svd_double2")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_svd_double2_sve_init,
  .test_run = eigen_svd_double2_sve_run,
  .test_cleanup = eigen_svd_double2_sve_cleanup,
  .fracture_loop_count = 5,
  .quality_level = TEST_QUALITY_PROD,
  .arm64_requires = ARM64_REQUIRES_SVE,
END_DECLARE_TEST

#endif // __aarch64__
