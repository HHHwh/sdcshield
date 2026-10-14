/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_gemm_cdouble_dynamic_square_dump
 * @parblock
 * Dump-on-failure variant of eigen_gemm_cdouble_dynamic_square. The FMA
 * hot path (x = lhs*rhs) is byte-for-byte identical to the original;
 * only the verification changes: instead of memcmp_or_fail over the raw
 * double stream (2*M_DIM*M_DIM doubles = real,imag interleaved), a cold
 * element-wise compare locates the first mismatch and dumps its complex
 * element index, the golden/actual real and imag doubles, their IEEE-754
 * bit xors, and a per-byte breakdown, so an SDC bit-flip can be located.
 * Mirrors the movbe_dump / mrn_rmw_dump design: no extra hot-path reads.
 * Operand freshness (B2-007, R2b freeze blind spot): like the non-dump
 * gemm files (46141ed4), the run loop keeps per-thread lhs/rhs re-rolled
 * from the framework RNG every 16 iterations with the golden recomputed
 * the same batch; each cycle's midpoint runs one frozen-replay round on
 * init's shared Mat::Random draw, so the init-time golden is actually
 * re-compared instead of never being replayed.
 *
 * @note This test requires at least 2 threads to run.
 * @endparblock
 */

#include <sandstone.h>

#include <Eigen/Core>
#include <cstdint>
#include <cstring>
using namespace Eigen;

#define M_DIM 221 // weird dim on purpose

typedef Matrix < std::complex < double >, Dynamic, Dynamic > Mat;

namespace {
struct eigen_test_data {
    Mat lhs;
    Mat rhs;
    Mat prod;
};
}

#define CAST(_x) static_cast<struct eigen_test_data *>(_x)

static int eigen_gemm_cdouble_dynamic_square_init(struct test *test) {
    test->data = new(eigen_test_data);
    try {
        CAST(test->data)->lhs = Mat::Random(M_DIM, M_DIM);
        CAST(test->data)->rhs = Mat::Random(M_DIM, M_DIM);
        CAST(test->data)->prod = CAST(test->data)->lhs * CAST(test->data)->rhs;
    } catch (...) {
        report_fail_msg("Exception on Eigen code, most probably OOM");
    }
    return EXIT_SUCCESS;
}

static int eigen_gemm_cdouble_dynamic_square_run(struct test *test, int cpu) {
    auto d = CAST(test->data);
    /* Operand freshness (B2-007, R2b freeze blind spot): per-thread
     * operands re-rolled from the framework RNG every kRerollEvery
     * iterations, both components [-1, 1) bounded like Mat::Random, with
     * the golden recomputed the same batch (46141ed4 non-dump gemm
     * pattern); each cycle's midpoint one frozen-replay round instead
     * restores init's shared lhs/rhs/prod snapshot into the per-thread
     * buffers, so the init-time golden is actually re-compared. */
    constexpr int kRerollEvery = 16;
    auto fill_rand = [](Mat &m) {
        for (int r = 0; r < m.rows(); ++r)
            for (int c = 0; c < m.cols(); ++c)
                m(r, c) = std::complex<double>(frandom_scale(2.0) - 1.0,
                                               frandom_scale(2.0) - 1.0);
    };
    Mat l_lhs, l_rhs, l_prod;
    l_lhs.resize(M_DIM, M_DIM);
    l_rhs.resize(M_DIM, M_DIM);
    l_prod.resize(M_DIM, M_DIM);
    fill_rand(l_lhs);
    fill_rand(l_rhs);
    l_prod = l_lhs * l_rhs;
    int since_reroll = 0;

    do {
        Mat x;
        x = l_lhs * l_rhs;                          // FMA hot path: unchanged

        // Original compared the raw double stream (real,imag interleaved).
        // Keep that exact byte-level semantics: compare as 2*M_DIM*M_DIM
        // doubles, and on first mismatch map back to a complex element +
        // real/imag component for the dump.
        const double *xd = reinterpret_cast<const double *>(x.data());
        const double *pd = reinterpret_cast<const double *>(l_prod.data());
        const int N = 2 * M_DIM * M_DIM;
        for (int idx = 0; idx < N; ++idx) {
            if (xd[idx] != pd[idx]) {
                // complex element index = idx/2; component = idx%2 (0=real,1=imag)
                int cidx = idx / 2;
                int row = cidx / M_DIM, col = cidx % M_DIM;
                const char *comp = (idx % 2 == 0) ? "real" : "imag";
                uint64_t golden_bits, actual_bits;
                memcpy(&golden_bits, &pd[idx], sizeof(uint64_t));
                memcpy(&actual_bits, &xd[idx], sizeof(uint64_t));
                uint64_t xorv = golden_bits ^ actual_bits;
                unsigned char *gb = (unsigned char *)&golden_bits;
                unsigned char *ab = (unsigned char *)&actual_bits;
                unsigned char *xb = (unsigned char *)&xorv;
                log_error("eigen_gemm_cdouble_dump: miscompare at [%d,%d].%s (didx=%d): "
                          "golden=%g actual=%g | bits golden=0x%016lX actual=0x%016lX "
                          "xor=0x%016lX (flipped bit(s)) | "
                          "bytes[b7..b0] golden=%02X%02X%02X%02X%02X%02X%02X%02X "
                          "actual=%02X%02X%02X%02X%02X%02X%02X%02X "
                          "xor=%02X%02X%02X%02X%02X%02X%02X%02X",
                          row, col, comp, idx,
                          pd[idx], xd[idx],
                          (unsigned long)golden_bits, (unsigned long)actual_bits,
                          (unsigned long)xorv,
                          gb[7], gb[6], gb[5], gb[4], gb[3], gb[2], gb[1], gb[0],
                          ab[7], ab[6], ab[5], ab[4], ab[3], ab[2], ab[1], ab[0],
                          xb[7], xb[6], xb[5], xb[4], xb[3], xb[2], xb[1], xb[0]);
                report_fail_msg("eigen_gemm_cdouble_dump: product miscompare at [%d,%d].%s", row, col, comp);
            }
        }
        /* At each cycle's midpoint one frozen-replay round instead
         * restores init's shared lhs/rhs/prod (Mat operator=
         * deep-copies) into the per-thread buffers: the reroll's golden
         * recompute runs on this very same core, so a mercurial
         * deterministic core defect would reproduce identically on both
         * sides and never trip the compare — replaying init's frozen
         * golden closes that blind spot. */
        if (++since_reroll >= kRerollEvery) {
            since_reroll = 0;
            fill_rand(l_lhs);
            fill_rand(l_rhs);
            l_prod = l_lhs * l_rhs;
        } else if (since_reroll == kRerollEvery / 2) {
            l_lhs = d->lhs;
            l_rhs = d->rhs;
            l_prod = d->prod;
        }
    } while (test_time_condition(test));
    return EXIT_SUCCESS;
}

static int eigen_gemm_cdouble_dynamic_square_finish(struct test *test) {
    delete(CAST(test->data));
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_gemm_cdouble_dynamic_square_dump, "Eigen GEMM payload (cplx double, dynamic, square) — dump variant records golden/actual/xor on miscompare")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_gemm_cdouble_dynamic_square_init,
  .test_run = eigen_gemm_cdouble_dynamic_square_run,
  .test_cleanup = eigen_gemm_cdouble_dynamic_square_finish,
  .fracture_loop_count = 5,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
