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

#include <Eigen/Core>
#include <cstdint>
#include <cstring>
#include <new>
using namespace Eigen;

typedef Matrix < std::complex < double >, Dynamic, Dynamic > Mat;

#if defined(__aarch64__) && defined(__linux__)
/* 联合求解 sizing 探针(T9): 填充与 run() 的 fill_rand 同谱系(均匀随机
 * [-1,1) 独立 re/im, 检视 #51)——splitmix 确定性哈希满足该契约, 且不
 * 依赖 init aux 线程上未定义的 per-thread RNG 状态。GEMM 计时对数值
 * 不敏感(无 SVD 式 deflation 路径), 谱系由分布一致性满足; 填充 O(n²)
 * 相对乘法 O(n³) 已计入探针计时(保守方向)。同 T6 gemm_double_max_arm
 * _body 的 solve_once(complex 四流: a.re/a.im/b.re/b.im)。 */
static int gemm_solve_once(int n)
{
    Mat a(n, n), b(n, n), c;
    for (int r = 0; r < n; ++r)
        for (int col = 0; col < n; ++col) {
            std::uint64_t h = (std::uint64_t)r * 0x9E3779B97F4A7C15ull
                            ^ (std::uint64_t)col * 0xC2B2AE3D27D4EB4Full;
            h ^= h >> 32;
            h *= 0xD6E8FEB86659FD93ull;
            h ^= h >> 32;
            std::uint64_t h2 = h * 0xD6E8FEB86659FD93ull
                             + 0x9E3779B97F4A7C15ull;
            h2 ^= h2 >> 32;
            std::uint64_t h3 = h2 * 0xD6E8FEB86659FD93ull
                             + 0x9E3779B97F4A7C15ull;
            h3 ^= h3 >> 32;
            std::uint64_t h4 = h3 * 0xD6E8FEB86659FD93ull
                             + 0x9E3779B97F4A7C15ull;
            h4 ^= h4 >> 32;
            a(r, col) = std::complex<double>(
                (double)(h % 2000u) / 1000.0 - 1.0,
                (double)(h2 % 2000u) / 1000.0 - 1.0);
            b(r, col) = std::complex<double>(
                (double)(h3 % 2000u) / 1000.0 - 1.0,
                (double)(h4 % 2000u) / 1000.0 - 1.0);
        }
    c = a * b;
    return 0;
}
#endif

/* 矩阵维度: 优先级链同 eigen_gemm_cdouble_dynamic_square
 * (-O eigen_gemm_cdouble_dynamic_square_dump.mdim=N / 配置文件 / 自适应)。
 * 历史硬编码 221 是刻意的"怪"维度, 显式配置时保留任意值语义。尺寸只
 * 在 test_init 求解一次并缓存进 eigen_test_data.dim——worker 读缓存
 * 不重算(T9, 旧 static 缓存因 run 期重入而存在, 该路径已删除)。 */
static int gemm_mdim(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "mdim", 0, 16, 4096);
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
    auto d = CAST(test->data);
    int dim = 0;
    try {
        dim = gemm_mdim(test);   /* knob > 自适应, 只解一次 */
        d->dim = dim;
        d->lhs = Mat::Random(dim, dim);
        d->rhs = Mat::Random(dim, dim);
        d->prod = d->lhs * d->rhs;
    } catch (const std::bad_alloc &) {
        /* T10 init 降级(m2, 对齐 eigen_sparse 先例): init 期分配失败是
         * 资源性而非 SDC——降 mdim 到 lo(=16, 与 gemm_mdim 的联合求解
         * lo/clamp 下限同值)重分配一次, 仍失败才资源性 SKIP。原
         * catch(...)->report_fail 对 bad_alloc 打红撤销(其余异常保留)。
         * 冻结回放轮读 d->lhs/rhs/prod, 降档后三者与 d->dim 同档一致。 */
        constexpr int kInitRetryLo = 16;
        log_warning("[init 降级] %s: 共享矩阵分配失败 mdim=%d → 降档 mdim=%d 重试一次",
                    test->id, dim, kInitRetryLo);
        try {
            d->dim = kInitRetryLo;
            d->lhs = Mat::Random(kInitRetryLo, kInitRetryLo);
            d->rhs = Mat::Random(kInitRetryLo, kInitRetryLo);
            d->prod = d->lhs * d->rhs;
        } catch (const std::bad_alloc &) {
            log_skip(RuntimeSkipCategory, "memory: %s mdim=%d",
                     test->id, kInitRetryLo);
            return EXIT_SKIP;
        }
        /* 降档成功 → 同步反馈记账口径(PR#302 检视 #25): 否则 [反馈] 按
         * 求解 n 折算 eff, c 被压向 0.7× 下限, 后续 n 向 OOM 膨胀。 */
        sdc_joint_cache_note_dim(test, kInitRetryLo);
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
