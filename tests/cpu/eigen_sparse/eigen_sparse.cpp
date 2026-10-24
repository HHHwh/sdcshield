/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_sparse
 * @parblock
 * This piece of code aims to stress test the exec (in particular FMA)
 * by repetitively solving the set of linear equations represented by
 * Ax=b where A is a sparse real symmetric matrix uings Cholskey method.
 * The decomposition function is from the 3rd party library Eigen.
 * A random double precision sparse real symmetric matrix (A) and a
 * random vector (b) are generated as inputs and then
 * Eigen::SimplicialCholesky is used to solve the problem Ax=b. The
 * result vector is compared against a golden result.
 *
 * Per-thread golden (H11'/P14 pattern, aligned with the EigenSVDTest
 * template): each worker generates its OWN A/b from the framework RNG
 * (per-thread stream, -s reproducible) and computes the golden on the
 * SAME thread with the same solver — same-source repeat. This removes
 * the old init-time shared golden, whose cross-thread byte-exact
 * comparison was the source of the multi-thread ULP flakiness recorded
 * in CLAUDE.md (different threads' heap alignment can steer Eigen into
 * different SIMD/scalar paths). Inputs re-roll every kRerollEvery
 * iterations; the old "deliberately NOT re-rolled" freeze existed only
 * to protect the shared-golden structure and is obsolete with it.
 *
 * @note Although the test should run fine on a single thread, it is
 * only expected to catch defects if run on at least 2 cores.
 * @endparblock
 */

#include <memory>

#include <sandstone.h>
#include "sdc_stress_config.h"

#include <Eigen/Sparse>

/*
 * 问题规模 n: 统一配置链:
 *   1. -O eigen_sparse.n=N / 配置文件 (sdc_stress.conf) 同名条目
 *   2. 环境变量 SDC_EIGEN_SPARSE_N
 *   3. 自适应: 稀疏矩阵 ~10% 密度, Cholesky 分解 RSS ~ 4*n^2*8B/线程,
 *      占物理内存 70%, clamp [64, 8192]。历史硬编码 256。
 */
static size_t sparse_n(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "n", 0, 64, 8192);
    if (v == 0)
        sdc_env_knob_int_clamped("eigen_sparse", "n", &v, 64, 8192);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_square_dim(8, 4, 4.0, 700, thread_count(), 64, 8192, test->id, "n");
}

namespace {
struct EigenSparseTestData {
    size_t n;   /* matrix dimension — the only field run() consumes */
};

/* Same generator semantics as the historical initialize_problem():
 * ~10% density symmetric off-diagonal entries in [0, 0.1), strictly
 * diagonally dominant (fabs + 0.05) so SimplicialCholesky stays
 * well-conditioned, b in [-1, 1). Operates on the CALLING thread's
 * objects — no shared state. */
static void generate_problem(size_t n,
                             Eigen::SparseMatrix<double> &A,
                             Eigen::VectorXd &b)
{
    A.resize(n, n);
    b.resize(n);
    std::vector<Eigen::Triplet<double>> trip;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            double x = frandom_scale(1.0);
            if (x < 0.1) {
                trip.push_back(Eigen::Triplet<double>(i, j, x));
                if (j > i)
                    trip.push_back(Eigen::Triplet<double>(j, i, x));
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        double x = fabs(frandom_scale(1.0)) + 0.05;
        trip.push_back(Eigen::Triplet<double>(i, i, x));
    }
    A.setFromTriplets(trip.begin(), trip.end());
    /* randomization hardening H11' (P14): b from the framework RNG
     * ([-1, 1), -s reproducible) instead of Eigen's deterministic
     * VectorXd::Random. */
    for (size_t i = 0; i < n; ++i)
        b[i] = frandom_scale(2.0) - 1.0;
}
}

static int eigen_sparse_init(struct test *test) {
    auto d = std::make_unique<EigenSparseTestData>();
    d->n = sparse_n(test);
    test->data = d.release();
    return EXIT_SUCCESS;
}

static int eigen_sparse_cleanup(struct test *test) {
    delete static_cast<EigenSparseTestData *>(test->data);
    return EXIT_SUCCESS;
}

/* Input re-roll cadence (H11' alignment with the EigenSVDTest template:
 * same-source repeat, fresh operands). */
static constexpr int kRerollEvery = 16;

static int eigen_sparse_run(struct test *test, int cpu) {
    (void)cpu;
    auto *d = static_cast<EigenSparseTestData *>(test->data);
    const size_t n = d->n;

    /* Per-thread problem + golden: allocated once, refilled in place.
     * The golden is computed on THIS thread with the same solver that
     * recomputes it below — cross-thread SIMD-path divergence (the old
     * shared-golden ULP hazard) cannot produce a false mismatch. */
    Eigen::SparseMatrix<double> A;
    Eigen::VectorXd b, golden, x;
    generate_problem(n, A, b);
    {
        Eigen::SimplicialCholesky<Eigen::SparseMatrix<double>> solver;
        try {
            golden = solver.compute(A).solve(b);
        } catch (...) {
            log_skip(TestResourceIssueSkipCategory, "Exception on Eigen code, most probably OOM");
            return EXIT_SKIP;
        }
        if (solver.info() != Eigen::Success) {
            report_fail(test);
            return EXIT_FAILURE;
        }
    }

    int since_reroll = 0;
    do {
        Eigen::SimplicialCholesky<Eigen::SparseMatrix<double>> solver;
        try {
            x = solver.compute(A).solve(b);
        } catch (...) {
            report_fail_msg("Exception on Eigen code, most probably OOM");
            return EXIT_FAILURE;
        }
        if (solver.info() != Eigen::Success) {
            report_fail(test);
            return EXIT_FAILURE;
        }
        if (x != golden) {
            report_fail(test);
            return EXIT_FAILURE;
        }

        if (++since_reroll >= kRerollEvery) {
            since_reroll = 0;
            generate_problem(n, A, b);
            Eigen::SimplicialCholesky<Eigen::SparseMatrix<double>> solver;
            try {
                golden = solver.compute(A).solve(b);
            } catch (...) {
                log_skip(TestResourceIssueSkipCategory, "Exception on Eigen code, most probably OOM");
                return EXIT_SKIP;
            }
            if (solver.info() != Eigen::Success) {
                report_fail(test);
                return EXIT_FAILURE;
            }
        }
    } while (test_time_condition(test));
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_sparse, "Eigen sparse linear algebra payload. Solve Ax=b using Cholskey (real symmetric A)")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_sparse_init,
  .test_run = eigen_sparse_run,
  .test_cleanup = eigen_sparse_cleanup,
  .desired_duration = -1,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
