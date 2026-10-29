/**
 * @file
 *
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_svd_bidiag_sve
 * @parblock
 * SVD-style Householder bidiagonalization payload on SVE — the SVE port
 * counterpart of eigen_svd_cdouble_noavx512 (whose SVE-backend sibling
 * eigen_svd_cdouble_sve currently crashes: the vendored Eigen5 SVE double
 * packet path SIGSEGVs on the 300x300 BDCSVD workload).
 *
 * This is a self-written, dependency-free SVD-family FMA stress: the
 * Golub-Kahan Householder bidiagonalization that underlies BDCSVD's first
 * phase. Every inner loop (Householder dot-product reductions and rank-1
 * update FMA chains) runs through SVE predicated svld1/svmla/svmls/svaddv,
 * making it a dense-SVE-FMA workload in the same family as the Eigen SVD
 * tests but without the Eigen library.
 *
 * Verification design (B1-015 / R2a): the golden is an INDEPENDENT scalar
 * Householder implementation (bidiagonalize_ref below). The original
 * revision compared the SVE kernel against itself — init precomputed the
 * golden by running the very same bidiagonalize_sve — which hid three real
 * kernel defects: (1) the left-update predicate svwhilelt(base, n-k)
 * silently dropped rows [n-k, n) of every column (~1e-1 relative
 * divergence), (2) eliminated entries were never written back (the output
 * was not bidiagonal at all; off-bidiagonal residue ~2.3 in arbitrary
 * positions), (3) the k+2<n loop bound skipped the final left reflector.
 * A calibration harness (VL=8 emulation of the fixed kernel's arithmetic)
 * measured the fixed SVE kernel vs this scalar reference at ~4e-14
 * relative to ||B||_F — five orders of magnitude inside the kTolRel=1e-9
 * comparison tolerance — while the original buggy kernel diverges at
 * ~1e-1 and fails the same comparison (the B1-015 evidence).
 *
 * PR #244 review (greptile + coderabbit): (a) the left-update used
 * CONTIGUOUS svld1/svst1 where the math needs a column (stride-n)
 * gather/scatter — a real SVE machine would fail every round even with
 * healthy hardware, because the previous self-comparison masked it; the
 * left update is now a gather/scatter loop (svld1_gather_u64offset /
 * svst1_scatter_u64offset) with per-lane offsets base*n+j + l*n. (b) The
 * NaN blind spot in the max-diff accumulation is closed: a non-finite
 * got/golden fails immediately instead of being swallowed by
 * fabs(NaN) > max_diff == false. (c) The reroll rounds additionally run
 * a same-input DOUBLE-PASS byte-exact self-consistency memcmp — the
 * tolerance-based independent-reference compare stays (SVE tree
 * reduction vs scalar serial summation round differently, so byte-equal
 * vs the scalar reference is mathematically impossible — cf. CLAUDE.md's
 * own ULP note on the Eigen family), but every reroll input now also
 * proves the kernel is deterministic on the DUT core (two passes, same
 * input, byte-identical) — deterministic-core (mercurial) defects that
 * poison both the recomputed golden and the DUT identically are caught
 * by the frozen-replay alternation below.
 *
 * Input randomization (B6 / R2b freeze blind spot): the input matrix is
 * re-rolled from the framework RNG every kRerollEvery iterations,
 * per-thread, and the golden is recomputed with the scalar reference at
 * each reroll (the 46141ed4 pattern). Init's fill survives only as the
 * init-time finiteness canary. PR #244 review (coderabbit): every other
 * reroll period is a FROZEN REPLAY — the run replays init's frozen
 * shared input/golden pair, so the init thread's golden (computed on a
 * different core, before the workers start) acts as a cross-core
 * byte-tolerant reference that a deterministic single-core fault cannot
 * poison.
 *
 * Honest scope note: this is bidiagonalization only, not a full SVD
 * (no singular values, no U/V accumulation) — the payload targets the
 * FMA/store-reload datapath, not SVD semantics.
 *
 * RUNTIME-UNVERIFIABLE (SVE): developed on a host without SVE
 * (HWCAP_SVE clear); compile-verified and the skip path executed for
 * real, but the SVE datapath itself has not run on hardware
 * (cf. 8502e491 / 73ebbd3f for the precedent). test_init gates via
 * the framework layer (arm64_require_sve_vl: HWCAP_SVE capability +
 * 128..1024-bit range in one call, all-VL Phase 2 P6; the TU is true
 * VLA — svwhilelt-stepped loops in svcntd()/svcntw() batches — so
 * every tier in range is in-domain). The PR #244-review
 * gather/scatter left-update IS verified on gem5 SVE SE emulation
 * (O3, VL=128/256/512, n=64/68/128): the pre-fix contiguous-load
 * version diverges at ~1.7 vs the scalar reference (the review's bug
 * is real), the fixed byte-offset gather/scatter version stays at
 * ~1e-13 (five orders inside kTolRel) and is byte-identical across
 * double passes.
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

/* 维度 n 运行时可配置, 统一优先级链:
 *   1. -O eigen_svd_bidiag_sve.mdim=N / 配置文件同名条目
 *   2. 环境变量 SDC_EIGEN_SVD_BIDIAG_SVE_MDIM
 *   3. 联合求解 (sdc_joint_dim_slice, P0-2): n_mem 按节点预算 × RSS 表
 *      (SDC_RSS_BIDIAG = 0.0313KB/n²/线程, 实测差分增量口径), n_run 按
 *      200s 全核单迭代预算 (2 解 × AR, 内存族单节点档 AR=25) 以 SVE
 *      同构探针 (run_bidiag_once) 的幂律拟合外推, 取 min 再 clamp
 *      [64, 2048]。旧 auto (sdc_auto_square_dim 纯内存口径, 无时间
 *      校准) 全核时撞 2048 上限, 单次迭代分钟级且 TERM 打不断,
 *      -t 5000 实测 343s SIGQUIT timed out —— 即本次修复的 P0-2。 */
struct bidiag_data {
    double *orig;     /* init 时的输入矩阵 (回放轮读, 重掷轮不读) */
    double *golden;   /* init 首算的标量参考结果 (回放轮比对) */
    double *work;     /* 保留字段(未用, run 用 per-thread TLS); 兼容旧结构 */
    int n;            /* 运行时矩阵维度 */
};

#ifdef __aarch64__

/* per-thread scratch (B6): 本线程重掷的输入 + 独立标量 golden + 工作
 * 副本 (46141ed4 模式: run 全权拥有自己的输入)。框架一线程一核并发
 * 进入 run, 任何 run 内写入的缓冲必须线程私有。惰性分配, cleanup 不
 * 释放: worker 线程先于主线程 cleanup 退出, fork 子进程随后整体回收
 * (dgemm 同款论证)。第二工作副本 work2 供双跑自一致 memcmp (PR #244
 * review): 同输入两跑 DUT, 逐字节一致证明内核在 DUT 核上确定性。
 * l_hv/l_hw 是 Householder 向量缓冲 (各 n 个 double, 大维度栈放不下,
 * 改 TLS 堆分配 —— 改造侧 per-thread 堆缓冲思路并入本惰性分配)。 */
static thread_local double *l_orig = nullptr;
static thread_local double *l_golden = nullptr;
static thread_local double *work = nullptr;
static thread_local double *work2 = nullptr;
static thread_local uint64_t *l_gofs = nullptr; /* 列 gather/scatter 偏移表 */
static thread_local double *l_hv = nullptr;     /* Householder 向量 v */
static thread_local double *l_hw = nullptr;     /* Householder 向量 w */

/* C11 aligned_alloc 契约: size 必须是 alignment 的整数倍。glibc 宽容
 * 接受任意 size(既有模式, CLAUDE.md Task-4 deferred 记录过), 但 ASAN
 * 拦截器严格执行契约并返回 NULL——T10 实测(2026-10-28): ASAN 下
 * gofs 的 32B 请求恒 NULL, 被 bad_alloc/NULL 检查吞成资源性 SKIP,
 * 把代码缺陷与真 OOM 混为一谈。所有 aligned_alloc(64, ...) 的 size
 * 统一向上取整到 64 的倍数, glibc/ASAN 两端行为一致。 */
static inline size_t round_up_64(size_t n)
{
    return (n + 63) & ~(size_t)63;
}

/* 独立标量参考 (B1-015 修复核心): 逐元素标量 Golub-Kahan 双对角化,
 * 与 SVE 内核同算法不同实现 (fixed +sqrt 定号 + 显式消元写回),
 * 是 golden 的唯一来源 —— 不再与 DUT 共享任何代码路径。
 * 标定实测 (n=128, VL=8 仿真): 与修复版 SVE 内核最大偏差
 * ~4e-14 * ||B||_F; off-bidiagonal 残差 0; ||B||_F/||A||_F = 1。 */
static void bidiagonalize_ref(double *A, int n)
{
    /* Householder 缓冲由调用方提供 (n 可达 2048, 栈放不下);
     * 独立标量参考是 golden 的唯一来源 (见上方注释)。
     * size 经 round_up_64 对齐到 alignment 倍数(C11 契约, 见其定义处
     * 注释——n%8!=0 的用户档位下 n*8 非 64 倍数, ASAN 会拒配)。 */
    double *v = (double *)aligned_alloc(64, round_up_64((size_t)n * sizeof(double)));
    double *w = (double *)aligned_alloc(64, round_up_64((size_t)n * sizeof(double)));
    for (int k = 0; k + 1 < n; ++k) {
        /* 左 Householder: 消去列 k 对角线下元素 */
        double norm2 = 0.0;
        for (int i = k; i < n; ++i)
            norm2 += A[i * n + k] * A[i * n + k];
        double alpha = sqrt(norm2);       /* 定号 +sqrt: 无符号跨越分支 */
        double v0 = A[k * n + k] - alpha;
        double vtv = v0 * v0;
        for (int i = k + 1; i < n; ++i)
            vtv += A[i * n + k] * A[i * n + k];
        if (vtv < 1e-300)
            continue;                     /* 列已呈双对角形态 */
        double tau = 2.0 * v0 * v0 / vtv;
        v[k] = 1.0;
        for (int i = k + 1; i < n; ++i)
            v[i] = A[i * n + k] / v0;
        for (int j = k + 1; j < n; ++j) {
            double dot = 0.0;
            for (int i = k; i < n; ++i)
                dot += v[i] * A[i * n + j];
            dot *= tau;
            for (int i = k; i < n; ++i)
                A[i * n + j] = fma(-v[i], dot, A[i * n + j]);
        }
        /* 消元位置显式写回: 正交变换下的精确值 (范数守恒) */
        A[k * n + k] = alpha;
        for (int i = k + 1; i < n; ++i)
            A[i * n + k] = 0.0;

        /* 右 Householder: 消去行 k 次对角线右元素 */
        if (k + 2 >= n)
            break;
        norm2 = 0.0;
        for (int j = k + 1; j < n; ++j)
            norm2 += A[k * n + j] * A[k * n + j];
        if (norm2 < 1e-300)
            continue;
        alpha = sqrt(norm2);
        v0 = A[k * n + k + 1] - alpha;
        vtv = v0 * v0;
        for (int j = k + 2; j < n; ++j)
            vtv += A[k * n + j] * A[k * n + j];
        if (vtv < 1e-300)
            continue;
        tau = 2.0 * v0 * v0 / vtv;
        w[k + 1] = 1.0;
        for (int j = k + 2; j < n; ++j)
            w[j] = A[k * n + j] / v0;
        for (int i = k + 1; i < n; ++i) {
            double dot = 0.0;
            for (int j = k + 1; j < n; ++j)
                dot += w[j] * A[i * n + j];
            dot *= tau;
            for (int j = k + 1; j < n; ++j)
                A[i * n + j] = fma(-w[j], dot, A[i * n + j]);
        }
        A[k * n + k + 1] = alpha;
        for (int j = k + 2; j < n; ++j)
            A[k * n + j] = 0.0;
    }
    free(v);
    free(w);
}

/* Golub-Kahan Householder 双对角化 (SVE 向量化内层, DUT)。
 * 修复 (B6): (1) 左更新谓词按全局行号 base<n 截断 —— 原按 n-k 截断,
 * 静默丢弃行 [n-k, n), 自比 golden 掩盖了整列更新残缺; (2) 消元位置
 * 显式写回 alpha/0 (原先从不写回, 输出根本不是双对角阵, 残差 ~2.3
 * 散布在任意位置), 同时成为强 SDC 探测位; (3) 循环界 k+1<n 补上最后
 * 一个左反射器, 右反射器仅在 k+2<n 时执行; (4) alpha 定号 +sqrt,
 * 消除近零跨越分支。
 * PR #244 review 修复 (greptile/coderabbit): 左更新的归约与 FMA 两个
 * 循环原先用连续 svld1_f64(A + base*n + j) 读"列", 实际读到的是行
 * base 的列 j..j+VL-1 —— 数学上需要的是列 j 的行 base..base+VL-1
 * (步长 n), 任何 VL 的真 SVE 硬件上都会误报失败 (此前的自比 golden
 * 与 VL=8 仿真都掩盖了它)。现在用 gather/scatter (步长 n 偏移表)。
 * v/w 是 Householder 向量缓冲——必须线程私有, 大维度下栈放不下,
 * 改由调用方提供 per-thread 堆缓冲 (各 n 个 double)。gofs 是列
 * gather/scatter 字节偏移表 (lane l → l*n*8, 依赖 n 与调用线程的
 * svcntd(), 由调用方构建) —— 显式参数化而非 TLS 直读, 使校准探针
 * (run_bidiag_once, init 线程, n 逐档变化) 能自带独立表, 不污染
 * run 期按 d->n 定尺寸的 per-thread 表。 */
static void bidiagonalize_sve(double *A, int n, double *v, double *w,
                              const uint64_t *gofs)
{
    for (int k = 0; k + 1 < n; ++k) {
        /* ---- 左 Householder: 消去列 k 的对角线下元素 ---- */
        double norm2 = 0.0;
        for (int i = k; i < n; ++i)
            norm2 += A[i * n + k] * A[i * n + k];
        double alpha = sqrt(norm2);
        double v0 = A[k * n + k] - alpha;
        double vtv = v0 * v0;
        for (int i = k + 1; i < n; ++i)
            vtv += A[i * n + k] * A[i * n + k];
        if (vtv < 1e-300)
            continue;
        double tau = 2.0 * v0 * v0 / vtv;
        v[k] = 1.0;
        for (int i = k + 1; i < n; ++i)
            v[i] = A[i * n + k] / v0;

        /* A = (I - tau*v*v^T) * A: 列 j = k+1..n-1 的 SVE 归约 + FMA 更新。
         * 谓词修复: 按全局行号 (base, n) 截断。
         * PR #244 review: 列访问 = 步长 n 的 gather/scatter, lane l 读
         * A[(base+l)*n + j] (偏移表 gofs 由调用方按 n×svcntd() 构建)。 */
        for (int j = k + 1; j < n; ++j) {
            double dot = 0.0;
            for (int base = k; base < n; base += svcntd()) {
                svbool_t pg = svwhilelt_b64((uint64_t)base, (uint64_t)n);
                svfloat64_t vv = svld1_f64(pg, v + base);
                svfloat64_t aa = svld1_gather_u64offset_f64(
                        pg, A + (size_t)base * n + j,
                        svld1_u64(pg, gofs));
                dot += svaddv_f64(pg, svmul_f64_x(pg, vv, aa));
            }
            dot *= tau;
            for (int base = k; base < n; base += svcntd()) {
                svbool_t pg = svwhilelt_b64((uint64_t)base, (uint64_t)n);
                svfloat64_t vv = svld1_f64(pg, v + base);
                svfloat64_t aa = svld1_gather_u64offset_f64(
                        pg, A + (size_t)base * n + j,
                        svld1_u64(pg, gofs));
                svst1_scatter_u64offset_f64(
                        pg, A + (size_t)base * n + j,
                        svld1_u64(pg, gofs),
                        svmls_f64_x(pg, aa, vv, svdup_f64(dot)));
            }
        }
        /* 消元位置显式写回 (修复点 2) */
        A[k * n + k] = alpha;
        for (int i = k + 1; i < n; ++i)
            A[i * n + k] = 0.0;

        /* ---- 右 Householder: 消去行 k 的次对角线右元素 ---- */
        if (k + 2 >= n)
            break;
        norm2 = 0.0;
        for (int j = k + 1; j < n; ++j)
            norm2 += A[k * n + j] * A[k * n + j];
        if (norm2 < 1e-300)
            continue;
        alpha = sqrt(norm2);
        v0 = A[k * n + k + 1] - alpha;
        vtv = v0 * v0;
        for (int j = k + 2; j < n; ++j)
            vtv += A[k * n + j] * A[k * n + j];
        if (vtv < 1e-300)
            continue;
        tau = 2.0 * v0 * v0 / vtv;
        w[k + 1] = 1.0;
        for (int j = k + 2; j < n; ++j)
            w[j] = A[k * n + j] / v0;

        /* A = A * (I - tau*w*w^T): 行 i = k+1..n-1 (行主序连续访问, 原本正确) */
        for (int i = k + 1; i < n; ++i) {
            double dot = 0.0;
            for (int base = k + 1; base < n; base += svcntd()) {
                svbool_t pg = svwhilelt_b64((uint64_t)base, (uint64_t)n);
                svfloat64_t ww = svld1_f64(pg, w + base);
                svfloat64_t aa = svld1_f64(pg, A + i * n + base);
                dot += svaddv_f64(pg, svmul_f64_x(pg, ww, aa));
            }
            dot *= tau;
            for (int base = k + 1; base < n; base += svcntd()) {
                svbool_t pg = svwhilelt_b64((uint64_t)base, (uint64_t)n);
                svfloat64_t ww = svld1_f64(pg, w + base);
                svfloat64_t aa = svld1_f64(pg, A + i * n + base);
                svst1_f64(pg, A + i * n + base,
                          svmls_f64_x(pg, aa, ww, svdup_f64(dot)));
            }
        }
        A[k * n + k + 1] = alpha;
        for (int j = k + 2; j < n; ++j)
            A[k * n + j] = 0.0;
    }
}

/* [-1, 1) 随机输入: 高位折尾数分数、低位定号 (dgemm random_bounded
 * 同款手法 —— 原始 uint64 重解释 double 有 ~1/4 概率 NaN/Inf)。
 * 与容差标定相同的值域 (||A||_F ~ 74), 标定结论直接适用。 */
static double random_pm_one(void)
{
    uint64_t r = random64();
    double frac = (double)(r >> 11) / 9007199254740992.0; /* [0, 1) */
    return (r & 1) ? (frac - 1.0) : frac;
}

/* 容差比较 (B6): 标定实测修复版 SVE 内核 vs 标量参考最大偏差
 * ~4e-14 * ||B||_F, kTolRel=1e-9 留 5 个数量级余量; 原缺陷内核发散
 * ~1e-1, 独立 golden 必然抓到 (B1-015)。NaN 捕获 (PR #244 review,
 * greptile): 原实现 fabs(NaN) > max_diff 恒 false, 全 NaN 结果也会
 * 通过 —— 现在非有限值直接失败, 不再进入最大差值累加。 */
static constexpr double kTolRel = 1e-9;

static void bidiag_compare_or_fail(const double *got, const double *golden,
                                   int n, long iter)
{
    double norm_g2 = 0.0, max_diff = 0.0;
    int worst = 0;
    for (int i = 0; i < n * n; ++i) {
        norm_g2 += golden[i] * golden[i];
        if (!std::isfinite(got[i]) || !std::isfinite(golden[i])) {
            report_fail_msg("bidiagonalization produced a non-finite value "
                            "at (%d, %d): got %.17g, golden %.17g "
                            "(iteration %ld)",
                            i / n, i % n, got[i], golden[i], iter);
        }
        double diff = fabs(got[i] - golden[i]);
        if (diff > max_diff) {
            max_diff = diff;
            worst = i;
        }
    }
    double tol = kTolRel * sqrt(norm_g2);
    if (!(max_diff <= tol)) {
        report_fail_msg("bidiagonalization diverged at (%d, %d): "
                        "|%.17g - %.17g| = %.3e > tol %.3e (iteration %ld)",
                        worst / n, worst % n, got[worst], golden[worst],
                        max_diff, tol, iter);
    }
}

/* 校准探针 (m7 同构契约, 检视 #51): 构 n×n 随机矩阵 (random_pm_one,
 * 与 init 填充 / run 重掷同谱系) + 一次 SVE 双边对角化, 结果丢弃, 只为
 * 计时 —— 与 run 期迭代同内核 (bidiagonalize_sve) 同填充分布。
 * 禁止用标量参考 bidiagonalize_ref 替代: 两者速度差 4-8×, 会系统性
 * 错标时间模型。gofs/v/w 就地分配 (探针 n 逐档变化, 不能复用 run 期
 * 按 d->n 定尺寸的 TLS 表)。分配失败返回 -1 且不求解: 此时的计时是
 * μs 级**非零**值, 校准层 (sdc_calibrated_dim 的 probe 丢弃本函数
 * 返回值, "计时不可测"分支要求 t<=0.0 精确成立, 此处进不去) 会把它
 * 当作超快求解外推——k 钳制后 dim=hi=2048, 是 P0-2 超时模式的复发
 * 路径而非安全退守; 可达性与 init 自身 2n²×8B 分配失败同源(探针缓冲
 * ≤33MB 更小, 它失败则 init 的更大分配也失败), 由 init 的降档重试/
 * 资源性 SKIP 兜底覆盖(T10 后行为)——失败分支的 log_warning 只为
 * 可观测性留痕。 */
static int run_bidiag_once(int n)
{
    /* size 全部 round_up_64 对齐到 alignment 倍数(C11 契约, 见其定义
     * 处注释; n 非 4 的倍数的档位下 n²*8/n*8 与 svcntd()*8 均可能非
     * 64 倍数, ASAN 拒配)。 */
    size_t bytes = round_up_64((size_t)n * n * sizeof(double));
    double *a = (double *)aligned_alloc(64, bytes);
    double *v = (double *)aligned_alloc(64, round_up_64((size_t)n * sizeof(double)));
    double *w = (double *)aligned_alloc(64, round_up_64((size_t)n * sizeof(double)));
    uint64_t *gofs = (uint64_t *)aligned_alloc(64,
            round_up_64(svcntd() * sizeof(uint64_t)));
    if (!a || !v || !w || !gofs) {
        log_warning("[联合求解] bidiag 探针分配失败 n=%d —— 计时将失真", n);
        free(a); free(v); free(w); free(gofs);
        return -1;
    }
    for (int i = 0; i < n * n; ++i)
        a[i] = random_pm_one();
    for (uint64_t l = 0; l < svcntd(); ++l)
        gofs[l] = l * (uint64_t)n * sizeof(double);
    bidiagonalize_sve(a, n, v, w, gofs);
    free(a); free(v); free(w); free(gofs);
    return 0;
}

static int eigen_svd_bidiag_sve_init(struct test *test)
{
    /* All-VL Phase 2 P6: capability + 128..1024-bit range check in one
     * framework-layer call, replacing the getauxval hand-probe (and its
     * misleading "placeholder" skip wording — absence of SVE is a
     * capability fact, not an unimplemented feature). Gate only: the
     * golden is an n×n scalar reference and the run side sizes its
     * per-thread scratch from a fresh svcntd() read, so no init/run VL
     * state exists to drift. */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
    (void)svl;

    auto *d = new bidiag_data;
    if (!d)
        return EXIT_FAILURE;

    /* 统一配置链解析维度 (见文件头注释): knob > env > 联合求解。
     * P0-2: auto 分支从 sdc_auto_square_dim(全局内存口径, 无时间校准,
     * 全核撞 2048 上限 → 单次迭代分钟级 → -t 5000 实测 343s SIGQUIT
     * timeout) 迁移到 sdc_joint_dim_slice —— n_mem 按节点预算×RSS 表
     * (BIDIAG 0.0313KB/n²/线程), n_run 按 200s 全核单迭代预算以 SVE
     * 同构探针幂律外推, 取 min。 */
    int64_t v = sdc_knob_int_clamped(test, "mdim", 0, 64, 2048);
    if (v == 0)
        sdc_env_knob_int_clamped("eigen_svd_bidiag_sve", "mdim", &v, 64, 2048);
    if (v > 0) {
        d->n = (int)v;
    } else {
        struct sdc_joint_args ja = {
            .lo = 64, .hi = 2048,
            .fam = SDC_RSS_BIDIAG,              /* RSS 表优先, 下方三元组仅兼容保留 */
            .bytes_per_elem = 8, .matrices_per_thread = 3, .rss_ratio = 2.0,
            .shared_bytes_per_slice = 0,   /* orig+golden = 16n² 随所选 n 动态,
                                            * 联合求解无闭式——忽略其一阶项:
                                            * n∈[64,2048] 时 ≤67MB, 占 650‰
                                            * 节点预算 <0.2% */
            .mem_fraction_permille = 650, .ar_fam = SDC_FAM_MEM_BOUND,
            .run_budget_ms = 200000,      /* 全核单迭代(2解×AR)预算: 300s 硬宽限×0.67 */
        };
        d->n = sdc_joint_dim_slice(test, "mdim", &ja, run_bidiag_once);
    }
    d->work = nullptr;   /* 保留字段, run 用 per-thread TLS 缓冲 */

    /* bytes 经 round_up_64 对齐(C11 契约, 见其定义处注释); run() 的
     * bytes 用同公式同 n 取整, memcpy 回放轮两侧尺寸恒一致。 */
    size_t bytes = round_up_64((size_t)d->n * d->n * sizeof(double));
    d->orig   = (double *)aligned_alloc(64, bytes);
    d->golden = (double *)aligned_alloc(64, bytes);
    if (!d->orig || !d->golden) {
        /* T10 init 降级(m2, 对齐 eigen_sparse 先例): init 期分配失败是
         * 资源性而非 SDC——降 n 到 lo(=64, 与上方联合求解 lo/clamp 下限
         * 同值)重分配一次, 仍失败才资源性 SKIP; 原 EXIT_FAILURE 打红
         * 撤销。降档后冻结回放对(orig/golden)与 run 的 d->n 同档一致。 */
        constexpr int kInitRetryLo = 64;
        log_warning("[init 降级] %s: 共享 orig/golden 分配失败 n=%d → 降档 n=%d 重试一次",
                    test->id, d->n, kInitRetryLo);
        free(d->orig);
        free(d->golden);
        d->n = kInitRetryLo;
        bytes = round_up_64((size_t)d->n * d->n * sizeof(double));
        d->orig   = (double *)aligned_alloc(64, bytes);
        d->golden = (double *)aligned_alloc(64, bytes);
        if (!d->orig || !d->golden) {
            free(d->orig);
            free(d->golden);
            delete d;
            log_skip(RuntimeSkipCategory, "memory: %s mdim=%d",
                     test->id, kInitRetryLo);
            return EXIT_SKIP;
        }
        /* 降档成功 → 同步反馈记账口径(PR#302 检视 #25, 同 GEMM cohort):
         * 否则 [反馈] 按求解 n 折算 eff, c 被压向 0.7× 下限, 后续 n 向
         * OOM 方向膨胀。 */
        sdc_joint_cache_note_dim(test, kInitRetryLo);
    }

    /* init 时有限性金丝雀 + 冻结回放基准 (B6 + PR #244 review): init
     * 线程算一次共享 orig/golden 对, worker 每 kRerollEvery 轮回放
     * 一次 —— 跨核参考, 持续性 (mercurial 型) 单核缺陷无法同时污染
     * init 核与 worker 核。随机输入经 Householder 正交变换范数守恒
     * (||B||_F = ||A||_F ~ 74), 远离 NaN/Inf。 */
    for (int i = 0; i < d->n * d->n; ++i)
        d->orig[i] = random_pm_one();
    memcpy(d->golden, d->orig, bytes);
    bidiagonalize_ref(d->golden, d->n);
    for (int i = 0; i < d->n * d->n; ++i) {
        double g = d->golden[i];
        if (g != g || g > 1.0e300 || g < -1.0e300) {
            report_fail_msg("golden bidiagonalization not finite at "
                            "element %d (random operands need tighter "
                            "scaling)", i);
        }
    }

    test->data = d;
    return EXIT_SUCCESS;
}

static int eigen_svd_bidiag_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<bidiag_data *>(test->data);
    const int n = d->n;
    size_t bytes = round_up_64((size_t)n * n * sizeof(double));   /* 同 init 公式, 回放 memcpy 两侧一致 */
    long iter = 0;

    /* B6 (R2b 冻结盲区): 本线程的输入每 kRerollEvery 轮从框架 RNG
     * 重掷 (线程安全: 框架 RNG per-thread 状态), golden 用独立标量
     * 参考同步重算 —— 每次比较都是"新鲜输入 x 独立实现"。
     * golden 重算 ~1ms/16 轮, 分摊开销 ~10% (DUT 单次 ~0.92ms)。
     * PR #244 review (coderabbit): 奇数轮为冻结回放轮 —— 回放 init
     * 共享的 orig/golden (跨核参考), mercurial 型缺陷无法同轮污染
     * 两个核。
     * PR #244 review round 2 (greptile+coderabbit): 回放轮不再改写
     * l_orig (原实现 memcpy(l_orig, d->orig) 会让下一非回放轮拿冻结
     * 输入去比随机输入的 golden, 健康硬件必误报) —— 直接 d->orig →
     * work;且回放按 (iter & 1) 交替触发, 不依赖 since_reroll (该测试
     * fracture_loop_count=5, run 每 5 轮在新子进程重入, 局部计数会
     * 归零, 任何依赖跨 5 轮阈值的触发点都可能永不命中)。交替下实际
     * 重掷周期为 2*kRerollEvery (回放轮不递增 since_reroll)。 */
    constexpr int kRerollEvery = 16;
    int since_reroll = kRerollEvery; /* 首轮强制重掷 */

    TEST_LOOP(test, 1) {
        if (__builtin_expect(!l_orig || !l_golden || !work || !work2 || !l_gofs || !l_hv || !l_hw, 0)) {
            l_orig   = (double *)aligned_alloc(64, bytes);
            l_golden = (double *)aligned_alloc(64, bytes);
            work     = (double *)aligned_alloc(64, bytes);
            work2    = (double *)aligned_alloc(64, bytes);
            l_gofs   = (uint64_t *)aligned_alloc(64, round_up_64(svcntd() * sizeof(uint64_t)));
            /* Householder 向量缓冲 hv/hw: 线程私有, 各 n 个 double
             * (大维度栈放不下, 由本 TLS 分配; 见 bidiagonalize_sve 签名)。 */
            l_hv     = (double *)aligned_alloc(64, round_up_64((size_t)n * sizeof(double)));
            l_hw     = (double *)aligned_alloc(64, round_up_64((size_t)n * sizeof(double)));
            if (!l_orig || !l_golden || !work || !work2 || !l_gofs || !l_hv || !l_hw) {
                /* T10(m2, 对齐 eigen_sparse 先例): worker 期资源性失败
                 * 不是 SDC——线程级 SKIP 不打红(原 report_fail 打红撤销,
                 * 检视口径同 eigen_sparse 检视 #49: 资源失败≠SDC)。部分
                 * 成功的缓冲释放并清指针, 防止任何后续误用。比对段
                 * (bidiag_compare_or_fail/memcmp)本就在本分配段之后,
                 * 不受影响。 */
                free(l_orig);   l_orig   = nullptr;
                free(l_golden); l_golden = nullptr;
                free(work);     work     = nullptr;
                free(work2);    work2    = nullptr;
                free(l_gofs);   l_gofs   = nullptr;
                free(l_hv);     l_hv     = nullptr;
                free(l_hw);     l_hw     = nullptr;
                log_skip(RuntimeSkipCategory, "memory: %s mdim=%d",
                         test->id, n);
                return EXIT_SKIP;
            }
            /* 列 gather/scatter 偏移表: lane l -> 字节偏移 l*n*8。
             * ACLE svld1_gather_u64offset_f64 的偏移单位是字节 (生成的
             * ld1d 无 lsl 缩放 —— gem5 VL=512 仿真实证: 元素单位表在
             * 所有 VL 发散, 字节单位表 MAXDIFF ~1e-13 全绿)。 */
            for (uint64_t l = 0; l < svcntd(); ++l)
                l_gofs[l] = l * (uint64_t)n * sizeof(double);
        }

        if (iter & 1) {
            /* 冻结回放轮: init 共享的冻结输入直拷 work (不碰 l_orig,
             * 重掷状态原样保留), DUT 输出与 init 线程 (另一核) 算好的
             * golden 比对 —— 跨核 byte-tolerant 参考 */
            memcpy(work, d->orig, bytes);
            bidiagonalize_sve(work, n, l_hv, l_hw, l_gofs);
            ++iter;
            bidiag_compare_or_fail(work, d->golden, n, iter);
        } else {
            if (++since_reroll >= kRerollEvery) {
                since_reroll = 0;
                for (int i = 0; i < n * n; ++i)
                    l_orig[i] = random_pm_one();
                memcpy(l_golden, l_orig, bytes);
                bidiagonalize_ref(l_golden, n);
            }

            memcpy(work, l_orig, bytes);
            bidiagonalize_sve(work, n, l_hv, l_hw, l_gofs);
            ++iter;
            bidiag_compare_or_fail(work, l_golden, n, iter);

            /* 双跑自一致 (PR #244 review, greptile "逐字节可重复检查"):
             * 同输入第二跑 DUT, 逐字节 memcmp —— 独立标量参考因树形归约
             * vs 串行求和的舍入差异不可能逐字节一致 (CLAUDE.md ULP 注),
             * 但 DUT 自身在确定性核上必须逐字节可重复; 低位翻转在两次
             * DUT 跑之间不一致时立即被抓。 */
            memcpy(work2, l_orig, bytes);
            bidiagonalize_sve(work2, n, l_hv, l_hw, l_gofs);
            if (memcmp(work, work2, bytes) != 0) {
                for (int i = 0; i < n * n; ++i) {
                    if (memcmp(&work[i], &work2[i], sizeof(double)) != 0) {
                        report_fail_msg("bidiagonalization not repeatable "
                                "on the same input at (%d, %d): pass1 "
                                "%.17g vs pass2 %.17g (iteration %ld)",
                                i / n, i % n, work[i], work2[i], iter);
                        break;
                    }
                }
            }
        }
    }

    return EXIT_SUCCESS;
}

static int eigen_svd_bidiag_sve_cleanup(struct test *test)
{
    auto *d = static_cast<bidiag_data *>(test->data);
    if (d) {
        free(d->orig);
        free(d->golden);
        delete d;
    }
    return EXIT_SUCCESS;
}

/* 注意: per-thread l_orig/l_golden/work 由框架在测试进程退出时随进程
 * 回收 (fork 模型), cleanup 无需逐线程释放 (worker 线程此时已退出,
 * 主线程 free 其他线程的 TLS 指针本就不安全 —— dgemm 同款论证)。 */

#else

static int eigen_svd_bidiag_sve_init(struct test *test)
{
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for eigen_svd_bidiag_sve");
    return EXIT_SKIP;
}

static int eigen_svd_bidiag_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    return EXIT_SKIP;
}
static int eigen_svd_bidiag_sve_cleanup(struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}
#endif

DECLARE_TEST(eigen_svd_bidiag_sve,
             "SVD-style Householder bidiagonalization SDC stress — dense SVE FMA chains "
             "(dot-product reductions + rank-1 svmla/svmls updates) without the Eigen library; "
             "independent scalar golden + framework-RNG re-rolled inputs + frozen cross-core "
             "replay alternation + same-input double-pass byte-exact self-check (B6, PR #244 review)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = eigen_svd_bidiag_sve_init,
    .test_run = eigen_svd_bidiag_sve_run,
    .test_cleanup = eigen_svd_bidiag_sve_cleanup,
    .fracture_loop_count = 5,
    .quality_level = TEST_QUALITY_PROD,
    .arm64_requires = ARM64_REQUIRES_SVE,
END_DECLARE_TEST
