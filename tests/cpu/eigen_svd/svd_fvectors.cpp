/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_svd_fvectors
 * @endparblock
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
 * precision input matrices and the (divide and conquer)
 * bi-diagonalization SVD algorithm. Since the randomization hardening
 * (H11') the operands are re-rolled per thread from the framework RNG,
 * mapped into [-1, 1) like Mat::Random; the init-time interesting-vector
 * matrix became dead computation with that change and was removed.
 *
 * @note This test requires at least 2 threads to run.
 * @endparblock
 */

#include "sandstone_eigen_common.h"
#include "sdc_stress_config.h"

using namespace Eigen;
typedef Matrix < float, Dynamic, Dynamic > Mat;
typedef Eigen::BDCSVD < Mat > SVD;

/* 运行时维度模式, 统一配置链 (-O eigen_svd_fvectors.mdim=N / 配置文件 /
 * 环境变量 SDC_EIGEN_SVD_FVECTORS_MDIM / 自适应)。历史硬编码 512。 */
using eigen_svd_fvectors_test = EigenSVDTest<SVD, Eigen::Dynamic>;

static int eigen_svd_fvectors_init(struct test *test) {
    auto d = new eigen_svd_fvectors_test::eigen_test_data;
    int64_t v = sdc_knob_int_clamped(test, "mdim", 0, 64, 2048);
    if (v == 0)
        sdc_env_knob_int_clamped("eigen_svd_fvectors", "mdim", &v, 64, 2048);
    d->dim = (v > 0) ? (int)v
                     : eigen_svd_fvectors_test::calibrated_auto_dim(test, 4, 6, 12.0, 650, 64, 2048);
    test->data = d;
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_svd_fvectors, "Eigen SVD (Singular Value Decomposition) solving payload, which issues a bunch of matrix multiplies underneath, now operating fixed FP32 vectors in the framework")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_svd_fvectors_init,
  .test_run = eigen_svd_fvectors_test::run,
  .test_cleanup = eigen_svd_fvectors_test::cleanup,
  .fracture_loop_count = 5,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
