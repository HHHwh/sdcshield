/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_svd_jacobi
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
 * This particular version of the Eigen SVD tests go for single
 * precision input matrices and the "Jacobi" SVD algorithm.
 *
 * This test requires at least 2 threads to run.
 * @endparblock
 */

#include "eigen_svd/sandstone_eigen_common.h"
#include "sdc_stress_config.h"

using namespace Eigen;

typedef Matrix < float, Dynamic, Dynamic > Mat;
typedef Eigen::JacobiSVD < Mat > SVD;

/* 运行时维度模式, 统一配置链 (-O eigen_svd_jacobi.mdim=N / 配置文件 /
 * 环境变量 SDC_EIGEN_SVD_JACOBI_MDIM / 自适应)。历史硬编码 256。
 * Jacobi SVD 的 RSS 增长远低于 BDCSVD, 用 rss_ratio 8。 */
using eigen_svd_jacobi_test = EigenSVDTest<SVD, Eigen::Dynamic>;

static int eigen_svd_jacobi_init(struct test *test)
{
    auto d = new eigen_svd_jacobi_test::eigen_test_data;
    int64_t v = sdc_knob_int_clamped(test, "mdim", 0, 64, 2048);
    if (v == 0)
        sdc_env_knob_int_clamped("eigen_svd_jacobi", "mdim", &v, 64, 2048);
    d->dim = (v > 0) ? (int)v
                     : eigen_svd_jacobi_test::calibrated_auto_dim(test, 4, 6, 8.0, 650, 64, 2048);
    test->data = d;
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_svd_jacobi, "Eigen SVD (Singular Value Decomposition, Jacobi algorithm) solving payload, which issues a bunch of matrix multiplies underneath")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_svd_jacobi_init,
  .test_run = eigen_svd_jacobi_test::run,
  .test_cleanup = eigen_svd_jacobi_test::cleanup,
  .quality_level = TEST_QUALITY_SKIP,
END_DECLARE_TEST
