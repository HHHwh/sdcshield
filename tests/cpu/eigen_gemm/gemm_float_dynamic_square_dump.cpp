/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_gemm_float_dynamic_square_dump
 * @parblock
 * Dump-on-failure variant of eigen_gemm_float_dynamic_square. The FMA
 * hot path (x = lhs*rhs) is byte-for-byte identical to the original;
 * only the verification changes: instead of memcmp_or_fail over the whole
 * matrix, a cold element-wise compare loop locates the first mismatch and
 * dumps its row/col, golden, actual, IEEE-754 bit xor, and a per-byte
 * breakdown, so an SDC bit-flip in the product can be located. Mirrors the
 * movbe_dump / mrn_rmw_dump design: no extra hot-path reads or snapshots.
 * Operand freshness (B2-007, R2b freeze blind spot): like the non-dump
 * gemm files (46141ed4), the run loop keeps per-thread lhs/rhs re-rolled
 * from the framework RNG every 16 iterations with the golden recomputed
 * the same batch; once per fracture (round 3 of every 4) a frozen-replay
 * round runs on init's shared Mat::Random draw, so the init-time golden
 * is actually re-compared instead of never being replayed.
 *
 * @note Although the test should run fine on a single thread, it is
 * only expected to catch defects if run on at least 2 cores.
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"

#include <Eigen/Core>
#include <cstdlib>
#include <cstdint>
#include <cstring>
using namespace Eigen;

// Matrix dimension. Configurable without recompiling via the unified
// priority chain (see sdc_stress_config.h):
//   1. -O eigen_gemm_float_dynamic_square_dump.mdim=N (or the same key in
//      sdc_stress.conf / $SDC_STRESS_CONFIG)
//   2. legacy GEMM_K_DIM env var (kept for old scripts)
//   3. environment-adaptive default (85% of RAM, 4 matrices/thread, 6x RSS)
static int gemm_k_dim(struct test *test) {
    static int dim = -1;
    if (dim < 0) {
        int64_t v = sdc_knob_int_clamped(test, "mdim", 0, 16, 8192);
        if (v == 0) {
            if (const char *env = getenv("GEMM_K_DIM")) {
                int parsed = atoi(env);
                if (parsed > 0) v = parsed;
            }
        }
        if (v > 0) {
            dim = (int)(v < 16 ? 16 : (v > 8192 ? 8192 : v));
        } else {
            dim = sdc_auto_square_dim(4, 4, 6.0, 850, thread_count(), 16, 8192, test->id, "mdim");
        }
    }
    return dim;
}
#define M_DIM gemm_k_dim(test)

typedef Matrix < float, Dynamic, Dynamic > Mat;

namespace {
struct eigen_test_data {
    Mat lhs;
    Mat rhs;
    Mat prod;
};
}

#define CAST(_x) static_cast<struct eigen_test_data *>(_x)

static int eigen_gemm_float_dynamic_square_init(struct test *test) {
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

static int eigen_gemm_float_dynamic_square_run(struct test *test, int cpu) {
    auto d = CAST(test->data);
    /* Operand freshness (B2-007, R2b freeze blind spot): per-thread
     * operands re-rolled from the framework RNG every kRerollEvery
     * iterations, [-1, 1) bounded like Mat::Random, with the golden
     * recomputed the same batch (46141ed4 non-dump gemm pattern); once
     * per fracture (round 3 of every 4) a frozen-replay round instead
     * runs init's shared lhs/rhs through the very same hot path and
     * compares against init's prod, so the init-time golden is actually
     * re-compared. */
    constexpr int kRerollEvery = 16;
    auto fill_rand = [](Mat &m) {
        for (int r = 0; r < m.rows(); ++r)
            for (int c = 0; c < m.cols(); ++c)
                m(r, c) = frandomf_scale(2.0f) - 1.0f;
    };
    Mat l_lhs, l_rhs, l_prod;
    l_lhs.resize(M_DIM, M_DIM);
    l_rhs.resize(M_DIM, M_DIM);
    l_prod.resize(M_DIM, M_DIM);
    fill_rand(l_lhs);
    fill_rand(l_rhs);
    l_prod = l_lhs * l_rhs;
    int since_reroll = 0;
    long iter = 0;

    do {
        /* PR #244 review round 2 (greptile): `since_reroll` is a run()
         * local, but the framework's fracture loop re-enters test_run in
         * a FRESH child process after a bounded number of
         * test_time_condition() rounds, resetting all locals, so a
         * "midpoint == 8" replay could easily never fire (and with a
         * short fracture it never does). The frozen replay now triggers
         * on an in-fracture round position (round 3 of every 4; any
         * 4-round window contains exactly one) and does NOT touch the
         * l_lhs/l_rhs/l_prod reroll state — Eigen's operator* reads its
         * operands and writes only the destination, so init's shared
         * matrices can be fed to the very same hot path directly (init
         * itself computed prod = lhs * rhs with it), and the comparison
         * selects init's prod as the golden. The reroll state survives
         * the replay round intact for the next non-replay round. */
        const bool frozen_replay = (iter & 3) == 2;
        /* replay rounds feed init's shared operands straight into the
         * same operator* (it reads its operands, writes only the
         * destination); the l_* reroll state is never written */
        const Mat &lhs = frozen_replay ? d->lhs : l_lhs;
        const Mat &rhs = frozen_replay ? d->rhs : l_rhs;
        Mat x;
        x = lhs * rhs;                          // FMA hot path: unchanged

        const float *xd = x.data();
        const float *pd = (frozen_replay ? d->prod : l_prod).data();
        // Element-wise byte-exact compare (same semantics as the original
        // memcmp_or_fail). On the first mismatch, dump golden/actual + the
        // IEEE-754 bit xor and per-byte breakdown, then fail. All of this
        // is cold: the hot path only does the gemm + the compare branch.
        bool ok = true;
        for (int idx = 0; idx < M_DIM * M_DIM; ++idx) {
            if (xd[idx] != pd[idx]) {
                ok = false;
                uint32_t golden_bits, actual_bits;
                memcpy(&golden_bits, &pd[idx], sizeof(uint32_t));
                memcpy(&actual_bits, &xd[idx], sizeof(uint32_t));
                uint32_t xorv = golden_bits ^ actual_bits;
                int row = idx / M_DIM, col = idx % M_DIM;
                unsigned char *gb = (unsigned char *)&golden_bits;
                unsigned char *ab = (unsigned char *)&actual_bits;
                unsigned char *xb = (unsigned char *)&xorv;
                log_error("eigen_gemm_float_dump: miscompare at [%d,%d] (idx=%d): "
                          "golden=%g actual=%g | bits golden=0x%08X actual=0x%08X "
                          "xor=0x%08X (flipped bit(s)) | "
                          "bytes[b3..b0] golden=%02X%02X%02X%02X "
                          "actual=%02X%02X%02X%02X xor=%02X%02X%02X%02X",
                          row, col, idx,
                          (double)pd[idx], (double)xd[idx],
                          golden_bits, actual_bits, xorv,
                          gb[3], gb[2], gb[1], gb[0],
                          ab[3], ab[2], ab[1], ab[0],
                          xb[3], xb[2], xb[1], xb[0]);
                report_fail_msg("eigen_gemm_float_dump: product miscompare at [%d,%d]", row, col);
            }
        }
        (void)ok;
        /* Frozen replay (PR #244 review, coderabbit): round 3 of every 4
         * runs init's shared operands through the same hot path and
         * compares against init's golden — init computed it on a
         * different core before the workers started, so this round's
         * compare is a CROSS-CORE reference. The reroll's golden
         * recompute runs on this very same core, so a mercurial
         * deterministic core defect would reproduce identically on both
         * sides and never trip the compare — replaying init's frozen
         * golden closes that blind spot. The replay round does not
         * touch l_lhs/l_rhs/l_prod, so the reroll state survives it;
         * replay rounds do not advance the reroll counter either (the
         * state stays valid across the replay). */
        if (!frozen_replay && ++since_reroll >= kRerollEvery) {
            since_reroll = 0;
            fill_rand(l_lhs);
            fill_rand(l_rhs);
            l_prod = l_lhs * l_rhs;
        }
        ++iter;
    } while (test_time_condition(test));
    return EXIT_SUCCESS;
}

static int eigen_gemm_float_dynamic_square_finish(struct test *test) {
    delete(CAST(test->data));
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_gemm_float_dynamic_square_dump, "Eigen GEMM payload (float, dynamic, square) — dump variant records golden/actual/xor on miscompare")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_gemm_float_dynamic_square_init,
  .test_run = eigen_gemm_float_dynamic_square_run,
  .test_cleanup = eigen_gemm_float_dynamic_square_finish,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
