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
 * double stream (2*dim*dim doubles = real,imag interleaved), a cold
 * element-wise compare locates the first mismatch and dumps its complex
 * element index, the golden/actual real and imag doubles, their IEEE-754
 * bit xors, and a per-byte breakdown, so an SDC bit-flip can be located.
 * Mirrors the movbe_dump / mrn_rmw_dump design: no extra hot-path reads.
 * Operand freshness (B2-007, R2b freeze blind spot): like the non-dump
 * gemm files (46141ed4), the run loop keeps per-thread lhs/rhs re-rolled
 * from the framework RNG every 16 iterations with the golden recomputed
 * the same batch; once per fracture (round 3 of every 4) a frozen-replay
 * round runs on init's shared Mat::Random draw, so the init-time golden
 * is actually re-compared instead of never being replayed.
 *
 * @note This test requires at least 2 threads to run.
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"
#include "gemm_solve_probe.h"
#include "sandstone_gemm_common.h"

#include <Eigen/Core>
#include <cstdint>
#include <cstring>
#include <new>
using namespace Eigen;

typedef Matrix < std::complex < double >, Dynamic, Dynamic > Mat;

#if defined(__aarch64__) && defined(__linux__)
/* 联合求解 sizing 探针(T9): 谱系契约(检视 #51: 填充与 run() 的
 * fill_rand 同谱系, 均匀随机 [-1,1) 独立 re/im)已单点收口至
 * gemm_solve_probe.h(issue #389, 含 T6 gemm_double_max_arm_body 源头
 * 共 8→1)——填充唯一定义, 探针副本间漂移在编译期不可能;
 * probe↔run() 谱系契约仍需与 fill_rand 手工同步演化(无编译期
 * 绑定, 检视 #392-R3; complex 四流:
 * a.re/a.im/b.re/b.im 经 if constexpr 分支)。 */
static int gemm_solve_once(int n)
{
    return gemm_probe::solve_once<Mat>(n);
}
#endif

/* 矩阵维度: 优先级链同 eigen_gemm_cdouble_dynamic_square
 * (-O eigen_gemm_cdouble_dynamic_square_dump.mdim=N / 配置文件 /
 * SDC_EIGEN_GEMM_CDOUBLE_DYNAMIC_SQUARE_DUMP_MDIM 环境变量 / 自适应;
 * env 通道与非 dump 版 gemm_mdim 同口径: strtoll 严格解析, 形态非法/
 * 溢出按未配置回退, 静默手工 clamp。arm64 联合求解 T11 早退块虽也
 * 补查同一 env, 但下方显式查询先消费——命中即返回不进联合层, 且
 * is_explicit 置位, 满足 sandstone_gemm_common.h 的出参契约)。
 * 历史硬编码 221 是刻意的"怪"维度, 显式配置时保留任意值语义。尺寸只
 * 在 test_init 求解一次并缓存进 eigen_test_data.dim——worker 读缓存
 * 不重算(T9, 旧 static 缓存因 run 期重入而存在, 该路径已删除)。
 * is_explicit 出参(issue #488): 显式通道(knob/env)来源时置 true, 供
 * sdc_gemm_init_shared 的 bad_alloc 分流(显式值不可降档)。 */
static int gemm_mdim(struct test *test, bool *is_explicit)
{
    int64_t v = sdc_knob_int_clamped(test, "mdim", 0, 16, 4096);
    if (v == 0) {
        int64_t envv = 0;
        if (sdc_env_knob_int(test->id, "mdim", &envv))
            v = (envv < 16) ? 16 : (envv > 4096 ? 4096 : envv);
    }
    if (v > 0)
        *is_explicit = true;    /* knob(-O/配置文件)/env 显式通道 */
    if (v <= 0) {
#if defined(__aarch64__) && defined(__linux__)
        /* 联合求解(T9, 参数依据见 gemm_double_dynamic_square.cpp 同位
         * 注释; 本文件差异仅 fam GEMM_C128, 16B/元素)。 */
        struct sdc_joint_args ja = {
            .lo = 16, .hi = 4096,
            .fam = SDC_RSS_GEMM_C128,   /* 0.0631 KB/n²/线程, 实测表值 */
            .bytes_per_elem = 16, .matrices_per_thread = 4, .rss_ratio = 6.0,
            .shared_bytes_per_slice = 0,
            .mem_fraction_permille = 850,
            .ar_fam = SDC_FAM_COMPUTE_BOUND,
            .run_budget_ms = 200000,    /* 全核单迭代(填充+一次乘法×AR)预算 */
        };
        try {
            v = sdc_joint_dim_slice(test, "mdim", &ja, gemm_solve_once);
        } catch (...) {
            log_warning("[联合求解] %s.%s: 探针异常 → 退守下限 %d",
                        test->id, "mdim", 16);
            v = 16;
        }
#else
        v = sdc_auto_square_dim(16, 4, 6.0, 850, thread_count(), 16, 4096, test->id, "mdim");
#endif
    }
    return (int)v;
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

static int eigen_gemm_cdouble_dynamic_square_init(struct test *test) {
    test->data = new(eigen_test_data);
    return sdc_gemm_init_shared(test, CAST(test->data), gemm_mdim);
}

static int eigen_gemm_cdouble_dynamic_square_run(struct test *test, int cpu) {
    auto d = CAST(test->data);
    /* Operand freshness (B2-007, R2b freeze blind spot): per-thread
     * operands re-rolled from the framework RNG every kRerollEvery
     * iterations, both components [-1, 1) bounded like Mat::Random, with
     * the golden recomputed the same batch (46141ed4 non-dump gemm
     * pattern); once per fracture (round 3 of every 4) a frozen-replay
     * round instead runs init's shared lhs/rhs through the very same
     * hot path and compares against init's prod, so the init-time
     * golden is actually re-compared. */
    constexpr int kRerollEvery = 16;
    auto fill_rand = [](Mat &m) {
        for (int r = 0; r < m.rows(); ++r)
            for (int c = 0; c < m.cols(); ++c)
                m(r, c) = std::complex<double>(frandom_scale(2.0) - 1.0,
                                               frandom_scale(2.0) - 1.0);
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
         * local, but this test sets fracture_loop_count=5 — the framework
         * re-enters test_run in a FRESH child process every 5 test_time_
         * condition() rounds, resetting all locals, so a "midpoint == 8"
         * replay could never fire. The frozen replay now triggers on an
         * in-fracture round position (round 3 of every 4; any 4-round
         * window contains exactly one) and does NOT touch the l_lhs/l_rhs/
         * l_prod reroll state — Eigen's operator* reads its operands and
         * writes only the destination, so init's shared matrices can be
         * fed to the very same hot path directly (init itself computed
         * prod = lhs * rhs with it), and the comparison selects init's
         * prod as the golden. The reroll state survives the replay round
         * intact for the next non-replay round. */
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

        /* 比对段在 catch 之外(T10 边界契约, 见 run 内头注)。
         * Original compared the raw double stream (real,imag interleaved).
         * Keep that exact byte-level semantics: compare as 2*dim*dim
         * doubles, and on first mismatch map back to a complex element +
         * real/imag component for the dump. */
        const double *xd = reinterpret_cast<const double *>(x.data());
        const double *pd = reinterpret_cast<const double *>((frozen_replay ? d->prod : l_prod).data());
        const int N = 2 * dim * dim;
        for (int idx = 0; idx < N; ++idx) {
            if (xd[idx] != pd[idx]) {
                // complex element index = idx/2; component = idx%2 (0=real,1=imag)
                int cidx = idx / 2;
                int row = cidx / dim, col = cidx % dim;
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
