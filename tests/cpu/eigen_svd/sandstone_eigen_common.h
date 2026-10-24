/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 */

#ifndef SANDSTONE_EIGEN_COMMON_H
#define SANDSTONE_EIGEN_COMMON_H

/* Disable deprecation warnings in case the library used is >= 3.5.x. Once
 * majority of the distros move to 3.5 or higher,
 * https://github.com/opendcdiag/opendcdiag/pull/941 should be adopted. */
#define EIGEN_NO_DEPRECATED_WARNING

#include <sandstone.h>
#include "sdc_stress_config.h"

#include <boost/type_traits/is_complex.hpp>
#include <Eigen/Eigenvalues>

namespace {
/* EigenSVDTest<SVD, Dim>: golden-value SVD test template.
 *
 * Dim > 0        — compile-time dimension (all historical tests).
 * Dim == Dynamic — runtime dimension mode: the test's init sets d->dim
 *                  (e.g. from a knob or from available memory) and all
 *                  matrix allocations/comparisons follow it. Used by
 *                  eigen_svd_cdouble_sve, which sizes the matrix from
 *                  per-worker RAM at init time (min 300). */
template <typename SVD, int Dim> struct EigenSVDTest
{
    using Mat = typename SVD::MatrixType;
    static constexpr bool kRuntimeDim = (Dim == Eigen::Dynamic);
    struct eigen_test_data {
        int dim = kRuntimeDim ? 0 : Dim;   /* matrix dimension */
        Mat orig_matrix;
        Mat u_matrix;
        Mat v_matrix;
    };

    [[gnu::noinline]] static void calculate_once(const Mat &orig_matrix, Mat &u, Mat &v)
    {
        SVD fullSvd(orig_matrix, Eigen::ComputeFullU | Eigen::ComputeFullV);
        u = fullSvd.matrixU();
        v = fullSvd.matrixV();
    }

    /* P2 time calibration: the memory-adaptive mdim only caps RSS — one
     * full SVD at a large dim can run for minutes inside a single loop
     * iteration, past which the -t TERM cannot interrupt (framework
     * SIGQUITs the child as "timed out"). Two-probe power-law
     * extrapolation caps ONE solve at kMaxSingleSolveMs so TERM is
     * honoured. Explicit -O mdim / env skips this (user-owned). */
    static constexpr long kMaxSingleSolveMs = 2000;

    static int calibrated_auto_dim(struct test *test, int bytes_per_elem,
                                   int matrices_per_thread, double rss_ratio,
                                   int mem_fraction_permille, int lo, int hi)
    {
        int mem_dim = sdc_auto_square_dim(bytes_per_elem, matrices_per_thread,
                                          rss_ratio, mem_fraction_permille,
                                          thread_count(), lo, hi, test->id, "mdim");
        if (mem_dim <= lo)
            return mem_dim;   /* quick mode / tiny host — nothing to calibrate */
        /* dense SVD is O(n^3)-dominated: the small-probe measured k runs
         * low (preprocessing-dominated at 64..128), so floor it at 3.0 */
        return sdc_calibrated_dim(lo, mem_dim, kMaxSingleSolveMs, test->id,
                                  "mdim", 3.0, [](int n) {
            /* deterministic fill — timing probe only, no RNG dependency */
            Mat m(n, n);
            for (int r = 0; r < n; ++r)
                for (int c = 0; c < n; ++c)
                    m(r, c) = (typename Mat::Scalar)(((r * 31 + c * 17) % 13) - 6)
                              / (typename Mat::Scalar)7;
            Mat u, v;
            calculate_once(m, u, v);
        });
    }

    template <typename FP> static inline std::enable_if_t<boost::is_complex<FP>::value>
    compare_or_fail(const FP *actual, const FP *expected, int dim, const char *name)
    {
        memcmp_or_fail(reinterpret_cast<const typename FP::value_type *>(actual),
                       reinterpret_cast<const typename FP::value_type *>(expected),
                       size_t(2) * static_cast<size_t>(dim) * static_cast<size_t>(dim), name);
    }

    template <typename FP> static inline std::enable_if_t<!boost::is_complex<FP>::value>
    compare_or_fail(const FP *actual, const FP *expected, int dim, const char *name)
    {
        memcmp_or_fail(actual, expected,
                       static_cast<size_t>(dim) * static_cast<size_t>(dim), name);
    }

    static int init(struct test *test)
    {
        /* H11'' (PR #147 review): run() primes per-thread matrices itself
         * and never reads eigen_test_data's matrices — the init-time
         * Mat::Random + golden SVD this function used to compute (for the
         * compile-time Dim users) was dead since the per-thread re-roll
         * landed. Removed. d->dim (set from Dim by the struct default, or
         * by the enclosing test's init for kRuntimeDim — see
         * svd_cdouble_sve.cpp) is the only field run() consumes. */
        auto d = new eigen_test_data;
        test->data = d;
        return EXIT_SUCCESS;
    }

    static int cleanup(struct test *test)
    {
        delete static_cast<eigen_test_data *>(test->data);
        return EXIT_SUCCESS;
    }

    /* randomization hardening H11' (P14): per-thread matrix, re-rolled
     * every kRerollEvery iterations from the framework RNG (per-thread
     * stream, -s reproducible) with values mapped into [-1, 1] like
     * Mat::Random (keeps the SVD well-conditioned — raw random bit
     * patterns would inject Inf/NaN/denormals). The golden is recomputed
     * through the same Eigen SVD on the SAME fresh matrix the same
     * "iteration batch" (same-source repeat, fresh operands — the
     * verify property is unchanged, the operand freshness is new).
     * Per-thread matrices also mean no shared state is written after
     * init, so no cross-thread races. The known multi-thread ULP
     * flakiness of parallel SVD (CLAUDE.md: use -n 1) is unaffected:
     * each thread's SVD is unchanged, only its input differs. */
    static constexpr int kRerollEvery = 16;

    static void fill_matrix_random(Mat &m, int dim)
    {
        using Scalar = typename Mat::Scalar;
        for (int r = 0; r < dim; ++r)
            for (int c = 0; c < dim; ++c) {
                /* [-1, 1) per Scalar; complex gets independent re/im */
                if constexpr (boost::is_complex<Scalar>::value) {
                    using VT = typename Scalar::value_type;
                    VT re, im;
                    if constexpr (sizeof(VT) == sizeof(float)) {
                        re = frandomf_scale(2.0f) - 1.0f;
                        im = frandomf_scale(2.0f) - 1.0f;
                    } else {
                        re = frandom_scale(2.0) - 1.0;
                        im = frandom_scale(2.0) - 1.0;
                    }
                    m(r, c) = Scalar(re, im);
                } else if constexpr (sizeof(Scalar) == sizeof(float)) {
                    m(r, c) = static_cast<Scalar>(frandomf_scale(2.0f) - 1.0f);
                } else {
                    m(r, c) = static_cast<Scalar>(frandom_scale(2.0) - 1.0);
                }
            }
    }

    static int run(struct test *test, int)
    {
        auto d = static_cast<eigen_test_data *>(test->data);

        /* Per-thread fresh matrices + goldens (kRerollEvery cadence).
         * Allocated once; refilled in place. Dynamic-storage matrices
         * (MatrixXd-family with a compile-time Dim, and the kRuntimeDim
         * mode) must be sized here — fixed-size matrices resize to
         * themselves harmlessly. */
        Mat orig, u_gold, v_gold, u, v;
        orig.resize(d->dim, d->dim);
        u_gold.resize(d->dim, d->dim);
        v_gold.resize(d->dim, d->dim);
        u.resize(d->dim, d->dim);
        v.resize(d->dim, d->dim);
        fill_matrix_random(orig, d->dim);
        calculate_once(orig, u_gold, v_gold);

        int since_reroll = 0;
        do {
            calculate_once(orig, u, v);

            compare_or_fail<typename Mat::Scalar>(u.data(), u_gold.data(), d->dim, "Matrix U");
            compare_or_fail<typename Mat::Scalar>(v.data(), v_gold.data(), d->dim, "Matrix V");

            if (++since_reroll >= kRerollEvery) {
                since_reroll = 0;
                fill_matrix_random(orig, d->dim);
                calculate_once(orig, u_gold, v_gold);
            }
        } while (test_time_condition(test));
        return EXIT_SUCCESS;
    }
};

} // unnamed namespace

#endif // SANDSTONE_EIGEN_COMMON_H
