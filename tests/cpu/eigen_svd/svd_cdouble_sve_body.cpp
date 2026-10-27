/*
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Eigen SVD payload body — compiled ONCE PER VL TIER (128/256/512/1024).
 *
 * Same reason as eigen_gemm/gemm_double_max_arm_body.cpp (design §4.7):
 * Eigen's SVE packet types are compile-time fixed-width and appear as struct
 * members, so one vector-length-agnostic TU cannot exist at the C++ language
 * level. meson compiles this source once per tier with its own Eigen
 * namespace (-DEigen=EigenSVEn) and its own symbol namespace
 * (-DSVD_TEST_NAMESPACE=svd_vln); the dispatch TU selects at run time.
 *
 * The EigenSVDTest template's data type is itself Eigen-parameterised, so
 * allocation and cleanup live here too — the dispatcher only ever handles an
 * opaque void*.
 */
#include "sandstone_eigen_common.h"
#include "sdc_stress_config.h"

#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#include <unistd.h>
#include <cmath>

#ifndef SVD_TEST_NAMESPACE
#  error "SVD_TEST_NAMESPACE must be defined by the build (one namespace per VL tier)"
#endif

namespace SVD_TEST_NAMESPACE {

using namespace Eigen;

typedef Matrix<std::complex<double>, Dynamic, Dynamic> Mat;
typedef Eigen::BDCSVD<Mat> SVD;

using test_type = EigenSVDTest<SVD, Eigen::Dynamic>;

void *make_data(int dim)
{
    auto *d = new test_type::eigen_test_data;
    d->dim = dim;
    return d;
}

void cleanup(struct test *test)
{
    test_type::cleanup(test);
}

int run_body(struct test *test, int cpu)
{
    return test_type::run(test, cpu);
}

}  // namespace SVD_TEST_NAMESPACE
