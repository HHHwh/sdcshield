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
#include "gemm_solve_probe.h"
#include "sandstone_gemm_common.h"

#include <Eigen/Core>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <new>
using namespace Eigen;

typedef Matrix < float, Dynamic, Dynamic > Mat;

#if defined(__aarch64__) && defined(__linux__)
/* 联合求解 sizing 探针(T9): 谱系契约(检视 #51: 填充与 run() 的
 * fill_rand 同谱系, 均匀随机 [-1,1))已单点收口至 gemm_solve_probe.h
 * (issue #389, 含 T6 gemm_double_max_arm_body 源头共 8→1)——填充唯一定义, 探针副本间
 * 漂移在编译期不可能; probe↔run() 谱系契约仍需与 fill_rand 手工
 * 同步演化(无编译期绑定, 检视 #392-R3; float 经 (Scalar) 先算 double 再收窄,
 * 与原 (float)((double)…) 逐位同)。 */
static int gemm_solve_once(int n)
{
    return gemm_probe::solve_once<Mat>(n);
}
#endif

// Matrix dimension. Configurable without recompiling via the unified
// priority chain (see sdc_stress_config.h):
//   1. -O eigen_gemm_float_dynamic_square_dump.mdim=N (or the same key in
//      sdc_stress.conf / $SDC_STRESS_CONFIG)
//   2. legacy GEMM_K_DIM env var (kept for old scripts; bare atoi with
//      silent manual clamp — semantics frozen)
//   3. SDC_EIGEN_GEMM_FLOAT_DYNAMIC_SQUARE_DUMP_MDIM env var (the same
//      channel the non-dump gemm_mdim reads: strict strtoll parse, invalid
//      or overflowing input counts as unconfigured, silent manual clamp)
//   4. environment-adaptive default (arm64: joint memory/time solver; other
//      arches: the legacy 85%-of-RAM memory model)
// (The arm64 joint solver's T11 knob early-exit in sdc_stress_config.h
// probes the same SDC_..._MDIM env var as a fallback; with the explicit
// lookup above it cannot fire here anymore — an env hit returns before the
// solver is reached, and is_explicit is set, matching the non-dump
// gemm_mdim contract in sandstone_gemm_common.h.)
// The dimension is solved ONCE in test_init and cached into
// eigen_test_data.dim — workers read the cache and never re-solve (T9: the
// old static-in-function cache existed for run()-side re-entry, which is
// gone).
// is_explicit out-param (issue #488): set true when the value came from an
// explicit channel (knob or either env var), for sdc_gemm_init_shared's
// bad_alloc split (explicit dims are not downgraded).
static int gemm_k_dim(struct test *test, bool *is_explicit) {
    int64_t v = sdc_knob_int_clamped(test, "mdim", 0, 16, 8192);
    if (v == 0) {
        if (const char *env = getenv("GEMM_K_DIM")) {
            int parsed = atoi(env);
            if (parsed > 0) v = parsed;
        }
    }
    if (v == 0) {
        int64_t envv = 0;
        if (sdc_env_knob_int(test->id, "mdim", &envv))
            v = (envv < 16) ? 16 : (envv > 8192 ? 8192 : envv);
    }
    if (v > 0) {
        *is_explicit = true;    /* knob / GEMM_K_DIM / SDC_..._MDIM env */
        return (int)(v < 16 ? 16 : (v > 8192 ? 8192 : v));
    }
#if defined(__aarch64__) && defined(__linux__)
    /* 联合求解(T9, 参数依据见 gemm_double_dynamic_square.cpp 同位注释;
     * 本文件差异仅 clamp [16,8192] 与 fam GEMM_F32)。 */
    struct sdc_joint_args ja = {
        .lo = 16, .hi = 8192,
        .fam = SDC_RSS_GEMM_F32,    /* 0.0159 KB/n²/线程, 实测表值 */
        .bytes_per_elem = 4, .matrices_per_thread = 4, .rss_ratio = 6.0,
        .shared_bytes_per_slice = 0,
        .mem_fraction_permille = 850,
        .ar_fam = SDC_FAM_COMPUTE_BOUND,
        .run_budget_ms = 200000,    /* 全核单迭代(填充+一次乘法×AR)预算 */
    };
    try {
        return sdc_joint_dim_slice(test, "mdim", &ja, gemm_solve_once);
    } catch (...) {
        log_warning("[联合求解] %s.%s: 探针异常 → 退守下限 %d",
                    test->id, "mdim", 16);
        return 16;
    }
#else
    return sdc_auto_square_dim(4, 4, 6.0, 850, thread_count(), 16, 8192, test->id, "mdim");
#endif
}

namespace {
struct eigen_test_data {
    Mat lhs;
    Mat rhs;
    Mat prod;
    int dim;    /* init 求解的最终维度(T9): worker 读缓存不重算;
                 * new(eigen_test_data) 默认初始化下未定值, init 在任何
                 * 可抛路径前赋值 */
};
}

#define CAST(_x) static_cast<struct eigen_test_data *>(_x)

static int eigen_gemm_float_dynamic_square_init(struct test *test) {
    test->data = new(eigen_test_data);
    return sdc_gemm_init_shared(test, CAST(test->data), gemm_k_dim);
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
    const int dim = d->dim;   /* init 缓存值——worker 不重算 */
    /* T10 bad_alloc 降级(m2, 对齐 eigen_sparse 先例): 分配/求解段包窄
     * catch(仅 std::bad_alloc)→ 线程级 SKIP——资源性失败不是 SDC 不打红;
     * 逐元素比对循环(report_fail_msg)恒在 catch 之外, SDC 误配必须打红。
     * worker 期不做降档重试(m2: 改共享 d->dim 有跨线程竞态,
     * v1 的重试+FAIL 语义与先例相反, 已撤销)。 */
    Mat l_lhs, l_rhs, l_prod;
    try {
        l_lhs.resize(dim, dim);
        l_rhs.resize(dim, dim);
        l_prod.resize(dim, dim);
        fill_rand(l_lhs);
        fill_rand(l_rhs);
        l_prod = l_lhs * l_rhs;             /* golden 同源首算 */
    } catch (const std::bad_alloc &) {
        log_skip(RuntimeSkipCategory, "memory: %s mdim=%d", test->id, dim);
        return EXIT_SKIP;
    }
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
        try {
            x = lhs * rhs;                      // FMA hot path: unchanged
        } catch (const std::bad_alloc &) {
            log_skip(RuntimeSkipCategory, "memory: %s mdim=%d", test->id, dim);
            return EXIT_SKIP;
        }

        /* 比对段在 catch 之外(T10 边界契约, 见 run 内头注)。 */
        const float *xd = x.data();
        const float *pd = (frozen_replay ? d->prod : l_prod).data();
        // Element-wise byte-exact compare (same semantics as the original
        // memcmp_or_fail). On the first mismatch, dump golden/actual + the
        // IEEE-754 bit xor and per-byte breakdown, then fail. All of this
        // is cold: the hot path only does the gemm + the compare branch.
        bool ok = true;
        for (int idx = 0; idx < dim * dim; ++idx) {
            if (xd[idx] != pd[idx]) {
                ok = false;
                uint32_t golden_bits, actual_bits;
                memcpy(&golden_bits, &pd[idx], sizeof(uint32_t));
                memcpy(&actual_bits, &xd[idx], sizeof(uint32_t));
                uint32_t xorv = golden_bits ^ actual_bits;
                int row = idx / dim, col = idx % dim;
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
            try {
                fill_rand(l_lhs);
                fill_rand(l_rhs);
                l_prod = l_lhs * l_rhs;     /* golden 重算 */
            } catch (const std::bad_alloc &) {
                log_skip(RuntimeSkipCategory, "memory: %s mdim=%d", test->id, dim);
                return EXIT_SKIP;
            }
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
