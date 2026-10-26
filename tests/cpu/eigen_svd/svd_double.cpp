/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_svd_double
 * @parblock
 * This piece of code aims to stress test the FMA execution units of
 * the CPU, among others, by repetitively solving the singular value
 * decomposition problem on given input matrices, which involve a lot
 * of matrix multiplication operations underneath.
 *
 * The logic comes from the 3rd party library Eigen. The first thread
 * that gets to run computes a "golden value" of the results, those
 * being output matrices "U" and "V". Subsequent runs have results
 * contrasted to those golden values and, whenever they differ, an
 * error is flagged.
 *
 * This particular version of the Eigen SVD tests go for double
 * precision input matrices and the (divide and conquer)
 * bi-diagonalization SVD algorithm.
 *
 * @note This test requires at least 2 threads to run.
 * @endparblock
 */

#include "sandstone_eigen_common.h"
#include "sdc_stress_config.h"

using namespace Eigen;

typedef Matrix < double, Dynamic, Dynamic > Mat;
typedef Eigen::BDCSVD < Mat > SVD;

/*
 * 运行时维度模式, 统一配置链 (-O mdim / 配置文件 / 环境变量 / 自适应):
 * double BDCSVD RSS ~12x, 6 矩阵/线程, 占物理内存 65%。
 * 时间上限: 实测 BDCSVD double N=2048 数分钟级, hi 取 2048。
 * eigen_svd_double 历史硬编码 256; eigen_svd_double2 历史硬编码 128。
 */
using eigen_svd_double_test = EigenSVDTest<SVD, Eigen::Dynamic>;

static int eigen_svd_double_init_common(struct test *test, const char *test_id)
{
    auto d = new eigen_svd_double_test::eigen_test_data;
    int64_t v = sdc_knob_int_clamped(test, "mdim", 0, 64, 2048);
    if (v == 0)
        sdc_env_knob_int_clamped(test_id, "mdim", &v, 64, 2048);
    d->dim = (v > 0) ? (int)v
                     : eigen_svd_double_test::calibrated_auto_dim(test, 8, 6, 12.0, 650, 64, 2048);
    test->data = d;
    return EXIT_SUCCESS;
}

static int eigen_svd_double_init(struct test *test)
{
    return eigen_svd_double_init_common(test, "eigen_svd_double");
}

DECLARE_TEST(eigen_svd_double, "Eigen SVD (Singular Value Decomposition) solving payload,  which issues a bunch of matrix multiplies underneath, now operating on doubles")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_svd_double_init,
  .test_run = eigen_svd_double_test::run,
  .test_cleanup = eigen_svd_double_test::cleanup,
  .fracture_loop_count = 5,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

static int eigen_svd_double2_init(struct test *test)
{
    return eigen_svd_double_init_common(test, "eigen_svd_double2");
}

using eigen_svd_double2_test = eigen_svd_double_test;
DECLARE_TEST(eigen_svd_double2, "Eigen SVD (Singular Value Decomposition) solving payload,  which issues a bunch of matrix multiplies underneath, now operating on doubles")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_svd_double2_init,
  .test_run = eigen_svd_double2_test::run,
  .test_cleanup = eigen_svd_double2_test::cleanup,
  .fracture_loop_count = 5,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
