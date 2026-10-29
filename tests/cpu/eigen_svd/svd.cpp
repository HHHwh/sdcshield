/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_svd
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
 * precision input matrices and the (divide and conquer)
 * bi-diagonalization SVD algorithm.
 *
 * @note This test requires at least 2 threads to run.
 * @endparblock
 */

#include "sandstone_eigen_common.h"
#include "sdc_stress_config.h"

using namespace Eigen;

typedef Matrix < float, Dynamic, Dynamic > Mat;
typedef Eigen::BDCSVD < Mat > SVD;

/*
 * 运行时维度模式: 维度经统一配置链解析:
 *   1. -O eigen_svd.mdim=N / 配置文件同名条目
 *   2. 环境变量 SDC_EIGEN_SVD_MDIM
 *   3. 自适应: 6 矩阵/线程(orig/u_gold/v_gold/u/v + BDCSVD 工作区,
 *      RSS ~12x), 占物理内存 65%, clamp [64, 4096]。
 * 历史硬编码 256。float SVD 时间上限: N=4096 远超 10 分钟预算,
 * 因此 hi 保守取 2048 (实测 BDCSVD float N=2048 数十秒级)。
 */
using eigen_svd_test = EigenSVDTest<SVD, Eigen::Dynamic>;

static int eigen_svd_init(struct test *test)
{
    auto d = new eigen_svd_test::eigen_test_data;
    /* -O/env 查询与显式短路已收进 calibrated_auto_dim(T11: arm64 显式值
     * 经联合层 knob 早退打预期内存警告; x86 行为不变)。 */
    d->dim = eigen_svd_test::calibrated_auto_dim(test, 4, 6, 12.0, 650, 64, 2048);
    test->data = d;
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_svd, "Eigen SVD (Singular Value Decomposition) solving payload, which issues a bunch of matrix multiplies underneath")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_svd_init,
  .test_run = eigen_svd_test::run,
  .test_cleanup = eigen_svd_test::cleanup,
  .fracture_loop_count = 2,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
