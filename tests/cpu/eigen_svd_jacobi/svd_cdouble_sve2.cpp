/**
 * @file
 *
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_svd_jacobi_cdouble_sve2
 * @parblock
 * ARM64 SVE2 counterpart of @ref eigen_svd_jacobi_cdouble_sve。
 *
 * 同一 Eigen JacobiSVD(complex<double>)负载,payload 以 SVE packet 后端
 * 按档编译 (128/256/512/1024),dispatcher 按框架 pin 的 VL 分派(模式同
 * @ref eigen_svd_cdouble_sve;样板收敛于
 * eigen_svd/sandstone_eigen_sve_dispatch.h)。Jacobi SVD 经 QR 预条件
 * + 双侧平面旋转迭代对角化——区别于 BDCSVD 的 Householder 双对角化
 * + 分治路径的另一 dense SVE FMA 场景。
 *
 * 诚实边界(design spec §4):本变体的指令集实质 = SVE1 子集在 SVE2
 * 硬件上向后兼容执行(spike 实证 +sve2 目标零 SVE2-only 指令);价值 =
 * SVE2 门控入口 + 编译目标声明,不是 SVE2-only 数据通路覆盖。
 *
 * 级别:TEST_QUALITY_SKIP,镜像原用例 eigen_svd_jacobi_cdouble(级别继承上游,
 * issue #322;晋升另案)——默认集不含本用例,显式运行:
 * --quality=-1 -e eigen_svd_jacobi_cdouble_sve2。
 *
 * 维度:knob/env > 联合求解,镜像 eigen_svd_jacobi_cdouble 的
 * calibrated_auto_dim(test, 16, 6, 8.0, 650, 64, 2048) 参数口径
 * (Jacobi 族 rss_ratio 原值 8,低于 BDCSVD 的 12);
 * override: -O eigen_svd_jacobi_cdouble_sve2.mdim=N (64..2048)。
 * @endparblock
 */

#include "eigen_svd/sandstone_eigen_common.h"
#include "eigen_svd/sandstone_eigen_sve_dispatch.h"
#include "sdc_stress_config.h"

/* 既有先例 svd_cdouble_sve.cpp:全 TU 在 aarch64 guard 内,无 #else——
 * dispatcher TU 只在 aarch64 编进 tests_sve,x86-64 零注册(rule 5)。 */
#if defined(__aarch64__)

#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>

EIGEN_SVE_DECLARE_BODY_NS(svd_j_c128_sve2)
EIGEN_SVE_DEFINE_DISPATCHERS(svd_j_c128_sve2)

#define MDIM_MIN 64
#define MDIM_MAX 2048

static int eigen_svd_jacobi_cdouble_sve2_init(struct test *test)
{
    if (!arm64_cpu_has_sve2()) {
        log_skip(CpuNotSupportedSkipCategory,
                 "%s: SVE2 required (HWCAP2_SVE2 absent); the SVE2 companion "
                 "runs on SVE2 hardware or gem5 SVE2 emulation — see the "
                 "honest-scope note in this file's header", test->id);
        return EXIT_SKIP;
    }
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
    if (!svd_j_c128_sve2_tier_select(test, svl))
        return EXIT_SKIP;

    int dim;
    int64_t knob = sdc_knob_int_clamped(test, "mdim", 0, MDIM_MIN, MDIM_MAX);
    if (knob == 0)
        sdc_env_knob_int_clamped("eigen_svd_jacobi_cdouble_sve2", "mdim", &knob,
                                 MDIM_MIN, MDIM_MAX);
    if (knob > 0) {
        dim = (int)knob;
    } else {
        struct sdc_joint_args ja = {
            .lo = MDIM_MIN, .hi = MDIM_MAX,
            .fam = SDC_RSS_JACOBI_C128,  /* RSS 表项与 NEON 版共用(同算法同矩阵数) */
            .bytes_per_elem = 16, .matrices_per_thread = 6, .rss_ratio = 8.0,
            .shared_bytes_per_slice = 0,   /* make_data holds no matrices */
            .mem_fraction_permille = 650,
            .ar_fam = SDC_FAM_MEM_BOUND,
            .run_budget_ms = 200000,   /* 镜像 eigen_svd/sandstone_eigen_common.h 联合层值 */
        };
        try {
            dim = sdc_joint_dim_slice(test, "mdim", &ja, svd_j_c128_sve2_solve_once);
        } catch (...) {
            log_warning("[联合求解] %s: mdim 探针异常 → 退守下限 %d",
                        test->id, MDIM_MIN);
            dim = MDIM_MIN;
        }
    }
    test->data = svd_j_c128_sve2_make_data(dim);
    return EXIT_SUCCESS;
}

/* DECLARE_TEST 需要普通函数符号——一行 wrapper(DECLARE_TEST 之前定义)。 */
static int eigen_svd_jacobi_cdouble_sve2_run(struct test *test, int cpu)
{
    return svd_j_c128_sve2_run(test, cpu);
}

static int eigen_svd_jacobi_cdouble_sve2_cleanup(struct test *test)
{
    svd_j_c128_sve2_cleanup(test);
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_svd_jacobi_cdouble_sve2, "Eigen SVD (Singular Value Decomposition, Jacobi algorithm) solving payload on the ARM64 SVE2 packet backend (per-tier 128/256/512/1024-bit variants, body compiled for the armv9-a+sve2 target); SKIP-level following the NEON counterpart (issue #322), run explicitly with --quality=-1; counterpart of eigen_svd_jacobi_cdouble_sve")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_svd_jacobi_cdouble_sve2_init,
  .test_run = eigen_svd_jacobi_cdouble_sve2_run,
  .test_cleanup = eigen_svd_jacobi_cdouble_sve2_cleanup,
  .quality_level = TEST_QUALITY_SKIP,   /* 镜像原用例 eigen_svd_jacobi_cdouble(issue #322):显式运行 --quality=-1 */
  .arm64_requires = ARM64_REQUIRES_SVE2,
END_DECLARE_TEST

#endif // __aarch64__
