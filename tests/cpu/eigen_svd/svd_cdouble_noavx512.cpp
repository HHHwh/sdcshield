/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_svd_cdouble_noavx512
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
 * precision, complex numbers input matrices and the (divide and
 * conquer) bi-diagonalization SVD algorithm.
 *
 * @note This test requires at least 2 threads to run.
 * @endparblock
 */

#include "sandstone_eigen_common.h"
#include "sdc_stress_config.h"

using namespace Eigen;

typedef Matrix < std::complex <double >, Dynamic, Dynamic > Mat;
typedef Eigen::BDCSVD < Mat > SVD;

/*
 * 运行时维度模式, 统一配置链 (-O eigen_svd_cdouble_noavx512.mdim=N /
 * 配置文件 / 环境变量 / 自适应)。历史硬编码 300。
 */
using eigen_svd_cdouble_noavx512_test = EigenSVDTest<SVD, Eigen::Dynamic>;

static int eigen_svd_cdouble_noavx512_init(struct test *test)
{
    auto d = new eigen_svd_cdouble_noavx512_test::eigen_test_data;
    /* -O/env 查询与显式短路已收进 calibrated_auto_dim(T11: arm64 显式值
     * 经联合层 knob 早退打预期内存警告; x86 行为不变)。 */
    d->dim = eigen_svd_cdouble_noavx512_test::calibrated_auto_dim(test, 16, 6, 12.0, 650, 64, 2048);
    test->data = d;
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_svd_cdouble_noavx512, "Eigen SVD (Singular Value Decomposition) solving payload, which issues a bunch of matrix multiplies underneath, now operating on std::complex<double>")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_svd_cdouble_noavx512_init,
  .test_run = eigen_svd_cdouble_noavx512_test::run,
  .test_cleanup = eigen_svd_cdouble_noavx512_test::cleanup,
  .fracture_loop_count = 5,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
