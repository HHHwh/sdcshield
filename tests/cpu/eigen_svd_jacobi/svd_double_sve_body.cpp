/*
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Eigen SVD payload body — double Jacobi SVD,compiled ONCE PER VL TIER
 * (128/256/512/1024)。模式同 svd_cdouble_sve_body.cpp(先例);
 * 服务 eigen_svd_jacobi_double_sve。签名契约见
 * eigen_svd/sandstone_eigen_sve_dispatch.h。
 */
#include "eigen_svd/sandstone_eigen_common.h"
#include "sdc_stress_config.h"

#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#include <unistd.h>

#ifndef EIGEN_SVE_BODY_NS
#  error "EIGEN_SVE_BODY_NS must be defined by the build (one namespace per VL tier)"
#endif

namespace EIGEN_SVE_BODY_NS {

using namespace Eigen;

typedef Matrix<double, Dynamic, Dynamic> Mat;
typedef Eigen::JacobiSVD<Mat> SVD;

using test_type = EigenSVDTest<SVD, Eigen::Dynamic>;

void *make_data(int dim)
{
    auto *d = new test_type::eigen_test_data;
    d->dim = dim;
    return d;
}

int solve_once(int dim)
{
    return test_type::sizing_probe(dim);
}

void cleanup(struct test *test)
{
    test_type::cleanup(test);
}

int run_body(struct test *test, int cpu)
{
    return test_type::run(test, cpu);
}

}  // namespace EIGEN_SVE_BODY_NS
