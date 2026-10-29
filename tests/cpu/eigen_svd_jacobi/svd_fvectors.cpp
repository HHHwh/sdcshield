/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_svd_jacobi_fvectors
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
 * precision input matrices and the "Jacobi" SVD algorithm. Since the
 * randomization hardening (H11') the operands are re-rolled per thread
 * from the framework RNG, mapped into [-1, 1) like Mat::Random; the
 * init-time interesting-vector matrix became dead computation with that
 * change and was removed.
 *
 * This test requires at least 2 threads to run.
 * @endparblock
 */

#include "eigen_svd/sandstone_eigen_common.h"
#include "sdc_stress_config.h"

using namespace Eigen;

typedef Matrix < float, Dynamic, Dynamic > Mat;
typedef Eigen::JacobiSVD < Mat > SVD;

/* 运行时维度模式, 统一配置链。历史硬编码 512。 */
using eigen_svd_jacobi_fvectors_test = EigenSVDTest<SVD, Eigen::Dynamic>;

static int eigen_svd_jacobi_fvectors_init(struct test *test) {
    auto d = new eigen_svd_jacobi_fvectors_test::eigen_test_data;
    /* -O/env 查询与显式短路已收进 calibrated_auto_dim(T11: arm64 显式值
     * 经联合层 knob 早退打预期内存警告; x86 行为不变)。 */
    d->dim = eigen_svd_jacobi_fvectors_test::calibrated_auto_dim(test, 4, 6, 8.0, 650, 64, 2048);
    test->data = d;
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_svd_jacobi_fvectors, "Eigen SVD (Singular Value Decomposition, Jacobi algorithm) solving payload, which issues a bunch of matrix multiplies underneath, now operating fixed FP32 vectors in the framework")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_svd_jacobi_fvectors_init,
  .test_run = eigen_svd_jacobi_fvectors_test::run,
  .test_cleanup = eigen_svd_jacobi_fvectors_test::cleanup,
  .quality_level = TEST_QUALITY_SKIP,
END_DECLARE_TEST
