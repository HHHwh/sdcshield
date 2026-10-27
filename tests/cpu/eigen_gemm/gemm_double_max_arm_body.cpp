/*
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Eigen GEMM payload body — compiled ONCE PER VL TIER (128/256/512/1024).
 *
 * Why this file exists (design §4.7): Eigen's SVE packet types are
 * compile-time fixed-width — `arm_sve_vector_bits(EIGEN_ARM64_SVE_VL)` — and
 * are used as struct members (`PacketXcd`), so a single vector-length-
 * agnostic TU is impossible at the C++ language level (twice-confirmed
 * spike). The workaround is not to make one TU adaptive but to compile the
 * same source N times, each with its own Eigen namespace
 * (-DEigen=EigenSVEn) and its own exported symbol namespace
 * (-DGEMM_TEST_NAMESPACE=gemm_vln); the dispatch TU picks the variant whose
 * width matches the worker's pinned VL.
 */
#include <sandstone.h>
#include "sdc_stress_config.h"

#include <Eigen/Core>

#include <cmath>
#include <cstdint>
#include <unistd.h>

#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>

#ifndef GEMM_TEST_NAMESPACE
#  error "GEMM_TEST_NAMESPACE must be defined by the build (one namespace per VL tier)"
#endif

/* Sized once by the dispatch TU's test_preinit. */
extern int eigen_gemm_double_max_arm_dim;

namespace GEMM_TEST_NAMESPACE {

using namespace Eigen;

typedef Matrix<double, Dynamic, Dynamic> Mat;

constexpr int kRerollEvery = 16;

static void fill_rand(Mat &m)
{
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.cols(); ++c)
            m(r, c) = frandom_scale(2.0) - 1.0;
}

/* The dispatch TU has already pinned this worker to the tier this variant
 * was compiled for and verified the readback; here we only run the payload.
 * (Keeping the pin in the dispatcher means it happens exactly once per test
 * execution, not once per variant.) */
int run_body(struct test *test, int cpu)
{
    (void)cpu;
    const int dim = eigen_gemm_double_max_arm_dim;
    Mat l_lhs, l_rhs, l_prod;
    l_lhs.resize(dim, dim);
    l_rhs.resize(dim, dim);
    l_prod.resize(dim, dim);
    fill_rand(l_lhs);
    fill_rand(l_rhs);
    l_prod = l_lhs * l_rhs;
    int since_reroll = 0;

    do {
        Mat x;
        x = l_lhs * l_rhs;

        memcmp_or_fail(x.data(), l_prod.data(), dim * dim);

        if (++since_reroll >= kRerollEvery) {
            since_reroll = 0;
            fill_rand(l_lhs);
            fill_rand(l_rhs);
            l_prod = l_lhs * l_rhs;
        }
    } while (test_time_condition(test));
    return EXIT_SUCCESS;
}

}  // namespace GEMM_TEST_NAMESPACE
