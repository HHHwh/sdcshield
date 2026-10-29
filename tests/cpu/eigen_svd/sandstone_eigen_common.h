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

#include <cstdint>
#include <new>

namespace {
/* RSS_RATIO_TABLE family key for an SVD instantiation (T8 联合求解): 算法
 * 由 SVD 模板参数经重载决议区分(BDCSVD vs JacobiSVD), 元素宽度由矩阵
 * Scalar 映射(F32/F64/C128)。调用点传入的传统三元组
 * (bytes×matrices×rss_ratio)自此降为防御性 fallback——fam 命中 RSS 表
 * 后联合求解即忽略之(sdc_stress_config.h 的 c 优先级链: 反馈→表→三元组)。
 * Eigen 的前向声明(Core/util/ForwardDeclarations.h)使这里只需指名模板
 * 无需完整定义; svd_cdouble_sve_body.cpp 以 -DEigen=EigenSVE 编译本头,
 * 宏替换后指名的是改名命名空间下的同名模板, 语义一致。 */
template <typename MatT, int Opts>
constexpr enum sdc_rss_family svd_rss_family_of(const Eigen::BDCSVD<MatT, Opts> *)
{
    using S = typename MatT::Scalar;
    if constexpr (boost::is_complex<S>::value) {
        static_assert(sizeof(S) == 16,
                      "complex<float> needs its own RSS table entry");
        return SDC_RSS_BDCSVD_C128;
    } else if constexpr (sizeof(S) == sizeof(float)) {
        return SDC_RSS_BDCSVD_F32;
    } else {
        return SDC_RSS_BDCSVD_F64;
    }
}

template <typename MatT, int Opts>
constexpr enum sdc_rss_family svd_rss_family_of(const Eigen::JacobiSVD<MatT, Opts> *)
{
    using S = typename MatT::Scalar;
    if constexpr (boost::is_complex<S>::value) {
        static_assert(sizeof(S) == 16,
                      "complex<float> needs its own RSS table entry");
        return SDC_RSS_JACOBI_C128;
    } else if constexpr (sizeof(S) == sizeof(float)) {
        return SDC_RSS_JACOBI_F32;
    } else {
        return SDC_RSS_JACOBI_F64;
    }
}

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

    /* Dimension via the joint memory×time solver (T8, spec §4.3): the old
     * two-step chain (sdc_auto_square_dim global memory model +
     * sdc_calibrated_dim 2 s single-solve cap) is replaced by ONE
     * sdc_joint_dim_slice call. Signature and every call site's
     * bytes/matrices/rss_ratio/permille stay untouched, so all ten
     * template users (eigen_svd{,_double,_double2,_fvectors,_cdouble,
     * _cdouble_noavx512} + eigen_svd_jacobi{,_double,_cdouble,_fvectors})
     * migrate in this single edit. Per knob of the joint model:
     *   memory — per-NUMA-node budget (three-source sdc_budget_bytes)
     *            against the RSS table coefficient of rss_family(); the
     *            call-site legacy triple passed through below is the
     *            defensive fallback the table supersedes (call-site
     *            comments describing "RSS ~12x/8x" describe that
     *            fallback model, not the effective one);
     *   time   — a full-core single-iteration budget
     *            (run_budget_ms, ×2 solves × AR(mem-bound, topology))
     *            replaces the old 2 s single-solve cap: the old cap was
     *            a single-threaded notion that left full-core contention
     *            entirely out of the model, so it over-shrunk n on
     *            many-core hosts;
     *   lo/hi  — the call-site clamp pair, unchanged;
     *   probe  — sizing_probe() below, passed verbatim (谱系契约
     *            检视 #51; PR#302 检视 #14 起为单一实现, SVE tier 派发
     *            体的 solve_once 亦委托此): the joint solver re-uses
     *            it for its power-law fit (k floor 3.0 = dense-SVD
     *            complexity).
     * quick mode / a degenerate [lo,hi] collapse to lo inside the joint
     * solver — the same shortcut the old chain took. Explicit -O mdim /
     * env bypasses this function entirely (user-owned).
     *
     * Cross-arch note (rule 5): the joint call is guarded to
     * aarch64+linux; every other arch (x86-64 reference) keeps the
     * legacy two-step chain VERBATIM below — see the #else branch
     * comment for why.
     *
     * run_budget_ms deviates from the joint contract value 200000 (= 300 s
     * hard grace × 0.67): the measured WORST-child slowdown on a
     * single-memory-node host exceeds the AR table's 1-node mem-bound
     * entry (25). T5 measured AR≈39 for complex BDCSVD at a ~31 GiB
     * footprint (eigen_svd_cdouble_sve.cpp run_budget_ms note); T8
     * re-measured the same class here — at 200000 the solver picked
     * n=2048 for eigen_svd (float, ~30.5 GiB aggregate) and the
     * children on the memory-less NUMA nodes could not finish ONE
     * full-core iteration inside the 300 s grace (SIGQUIT at 321 s
     * elapsed, /tmp/t8_eigen_svd.log, 2026-10-28): effective worst-child
     * AR ≥ 40 = 1.6× the table row. 140000 sizes the iteration so that
     * budget × 1.6 + init probes ≈ 232 s stays inside the grace with
     * ~23% margin (T5's measured outcome: ~225 s wall including init),
     * while still lifting the memory-bound dims well above the legacy
     * triple's (eigen_svd_double: 763 → ~1450). The principled fix is
     * the AR table entry itself (framework/sdc_numa.cpp — T7/T9 scope,
     * footprint-aware recalibration); this per-family budget is the
     * in-scope compensation and should be revisited when the table is
     * re-calibrated. */
    static constexpr enum sdc_rss_family rss_family()
    {
        return svd_rss_family_of(static_cast<const SVD *>(nullptr));
    }

    /* Sizing probe (PR#302 检视 #14 收口: 单一实现, 供下方两个 arch 分支
     * 与 SVE tier 派发体的 solve_once 共用——两份拷贝一旦漂移, 其中一份
     * 探针就会低估求解时间, 联合求解选出过大的 n, 全核单次迭代超出 300s
     * 宽限——正是本 PR 修复的超时模式)。
     * 谱系契约(检视 #51, verbatim from the pre-T8 chain): deterministic
     * hash fill — timing probe only, no RNG dependency. probe 填充必须与
     * run() 的 fill_matrix_random 同谱系(满秩随机 [-1,1))——旧
     * (r*31+c*17)%13 条纹阵是 13×13 模式的周期平铺, 秩 ≤13
     * (实测 512² rank=12), BDCSVD/JacobiSVD 对零尾奇异值走
     * deflation/快收敛路径, probe 系统性低估真实求解
     * (2026-10-26 鲲鹏 920 -O3 实测 probe/random 比值:
     * BDCSVD<double> 0.25~0.54×、Jacobi<double> 0.15~0.46×
     * 且随维度恶化、BDCSVD<complex> 0.47~0.70×; 对照
     * eigen_sparse 侧同谱系 probe 实测 0.95×)。splitmix 散列
     * [-1,1)(complex 独立 re/im)与随机阵同谱系, 校准计时
     * 口径与 run 期单解一致。改这里的值域/结构必须与
     * fill_matrix_random 同步演化。 */
    static int sizing_probe(int n)
    {
        Mat m(n, n);
        using Scalar = typename Mat::Scalar;
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c) {
                std::uint64_t h = (std::uint64_t)r * 0x9E3779B97F4A7C15ull
                                ^ (std::uint64_t)c * 0xC2B2AE3D27D4EB4Full;
                h ^= h >> 32;
                h *= 0xD6E8FEB86659FD93ull;
                h ^= h >> 32;
                if constexpr (boost::is_complex<Scalar>::value) {
                    std::uint64_t h2 = h * 0xD6E8FEB86659FD93ull
                                     + 0x9E3779B97F4A7C15ull;
                    h2 ^= h2 >> 32;
                    m(r, c) = Scalar(
                        (typename Scalar::value_type)((double)(h % 2000u) / 1000.0 - 1.0),
                        (typename Scalar::value_type)((double)(h2 % 2000u) / 1000.0 - 1.0));
                } else {
                    m(r, c) = (Scalar)((double)(h % 2000u) / 1000.0 - 1.0);
                }
            }
        Mat u, v;
        calculate_once(m, u, v);
        return 0;
    }

    static int calibrated_auto_dim(struct test *test, int bytes_per_elem,
                                   int matrices_per_thread, double rss_ratio,
                                   int mem_fraction_permille, int lo, int hi)
    {
#if defined(__aarch64__) && defined(__linux__)
        /* T11 (P2b): -O/env 查询从各 svd_*.cpp 的 init 收进联合层——
         * calibrated_auto_dim 现被无条件调用(含显式 knob 命中时), 显式值
         * 由 sdc_joint_dim_slice 的 knob 早退处理: 读 cache/补查 env 后按
         * 模型口径打预期内存档位警告([内存越界]/[欠压力], 仅提示不阻断)
         * 并原值返回, 优先于 quick 收缩(与原调用方短路语义一致)。
         * [压力映射] 日志仍只打一次: -O 通道由 knob 早退的 cache-miss
         * 兜底查询发出, env 通道由早退块补查发出, 与迁移前调用方的
         * 查询序列逐字等价。 */
        struct sdc_joint_args ja = {
            .lo = lo, .hi = hi,
            .fam = rss_family(),
            .bytes_per_elem = bytes_per_elem, .matrices_per_thread = matrices_per_thread,
            .rss_ratio = rss_ratio,       /* legacy triple (pre-table model):
                                          * ignored while fam selects the RSS table */
            .shared_bytes_per_slice = 0,  /* init holds no matrices — run()
                                          * primes per-thread state itself */
            .mem_fraction_permille = mem_fraction_permille,
            .ar_fam = SDC_FAM_MEM_BOUND,  /* dense SVD is bandwidth-bound at full core */
            .run_budget_ms = 140000,      /* 全核单迭代(2解×AR)预算; 见上方块注释 */
        };
        try {
            return sdc_joint_dim_slice(test, "mdim", &ja, sizing_probe);
        } catch (...) {
            /* probe 期分配异常(如 ASAN 构建的 OOM): init 不 crash,
             * 退守下限并诚实记录(检视 #40; 计划文档 P2 原则
             * 『上限宁小勿大』——退内存自适应档会在异常为环境性/
             * 瞬时性时静默复活『单解分钟级、TERM 打不断』的老问题,
             * 退 lo 保证 TERM 可兑现且 run() 仍执行同一指令路径)。 */
            log_warning("[联合求解] %s.%s: 探针异常 → 退守下限 %d",
                        test->id, "mdim", lo);
            return lo;
        }
#else
        /* rule 5 (x86-64 untouched): the legacy two-step chain stays
         * VERBATIM on non-aarch64 — sdc_joint_dim_slice's x86 branch
         * ignores solve_once/run_budget_ms (its time calibration is the
         * CALLER's chain by contract), so routing x86 through the joint
         * solver would silently drop the 2 s single-solve cap and swap
         * the legacy memory triple for the ARM-measured RSS table. The
         * ten template users are the first CROSS-ARCH joint consumers
         * (T4/T5/T6's were ARM-only TUs); this guard is the additive
         * porting pattern.
         * T11: the caller-side -O/env query + explicit short-circuit
         * moved HERE verbatim (same helpers, same clamp, same logs, same
         * order — only relocated from each svd_*.cpp init so the arm64
         * side can route through the joint layer); env name = test->id,
         * identical to the test_id literal every init passed. Behavior
         * on x86-64 is unchanged. */
        int64_t v = sdc_knob_int_clamped(test, "mdim", 0, lo, hi);
        if (v == 0)
            sdc_env_knob_int_clamped(test ? test->id : nullptr, "mdim",
                                     &v, lo, hi);
        if (v > 0)
            return (int)v;
        constexpr long kMaxSingleSolveMs = 2000;
        int mem_dim = sdc_auto_square_dim(bytes_per_elem, matrices_per_thread,
                                          rss_ratio, mem_fraction_permille,
                                          thread_count(), lo, hi, test->id, "mdim");
        if (mem_dim <= lo)
            return mem_dim;   /* quick mode / tiny host — nothing to calibrate */
        /* dense SVD is O(n^3)-dominated: the small-probe measured k runs
         * low (preprocessing-dominated at 64..128), so floor it at 3.0 */
        try {
            return sdc_calibrated_dim(lo, mem_dim, kMaxSingleSolveMs,
                                      test->id, "mdim", 3.0, sizing_probe);
        } catch (...) {
            /* probe 期分配异常(如 ASAN 构建的 OOM): init 不 crash,
             * 退守校准下限并诚实记录(检视 #40; 计划文档 P2 原则
             * 『上限宁小勿大』——退内存自适应档会在异常为环境性/
             * 瞬时性时静默复活『单解分钟级、TERM 打不断』的老问题,
             * 退 lo 保证 TERM 可兑现且 run() 仍执行同一指令路径)。 */
            log_warning("[时间校准] %s.%s: 试解异常 → 退守校准下限 %d",
                        test->id, "mdim", lo);
            return lo;
        }
#endif
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
     * init, so no cross-thread races. Because the golden is also
     * per-thread and same-source (recomputed on the SAME thread from
     * the SAME fresh matrix), cross-thread Eigen SIMD/scalar path
     * divergence (heap-alignment-steered — the old shared-golden
     * mismatch source) can no longer produce false mismatches: the
     * multi-thread ULP flakiness this family used to record in
     * CLAUDE.md is gone (2026-10-24 fix; CLAUDE.md now says run
     * full-core, `-n 1` only as a cross-check, no longer required). */
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
         * themselves harmlessly.
         *
         * T10 bad_alloc 降级 (m2, 对齐 eigen_sparse 先例): 分配/求解段包
         * 窄 catch —— 仅 std::bad_alloc。资源性失败不是 SDC, 线程级
         * SKIP 不打红; 其他异常不被吞(NDEBUG 生产构建下 Eigen
         * compute/solve 实际可抛的异常只有 bad_alloc, sparse 检视 #49
         * 同论证)。比对段(compare_or_fail → memcmp_or_fail →
         * report_fail)恒在 catch 之外——SDC 误配必须打红, 绝不能被资源
         * 性 SKIP 吞掉。worker 期不做降档重试(m2: 改共享 d->dim 有跨
         * 线程竞态, 且 v1 的重试+FAIL 语义与先例相反, 已撤销)。 */
        Mat orig, u_gold, v_gold, u, v;
        try {
            orig.resize(d->dim, d->dim);
            u_gold.resize(d->dim, d->dim);
            v_gold.resize(d->dim, d->dim);
            u.resize(d->dim, d->dim);
            v.resize(d->dim, d->dim);
            fill_matrix_random(orig, d->dim);
            calculate_once(orig, u_gold, v_gold);   /* golden 同源首算 */
        } catch (const std::bad_alloc &) {
            log_skip(RuntimeSkipCategory, "memory: %s mdim=%d",
                     test->id, d->dim);
            return EXIT_SKIP;
        }

        int since_reroll = 0;
        do {
            try {
                calculate_once(orig, u, v);         /* DUT 求解 */
            } catch (const std::bad_alloc &) {
                log_skip(RuntimeSkipCategory, "memory: %s mdim=%d",
                         test->id, d->dim);
                return EXIT_SKIP;
            }

            /* 比对段在 catch 之外(T10 边界契约, 见 run 头注释)。 */
            compare_or_fail<typename Mat::Scalar>(u.data(), u_gold.data(), d->dim, "Matrix U");
            compare_or_fail<typename Mat::Scalar>(v.data(), v_gold.data(), d->dim, "Matrix V");

            if (++since_reroll >= kRerollEvery) {
                since_reroll = 0;
                try {
                    fill_matrix_random(orig, d->dim);
                    calculate_once(orig, u_gold, v_gold);   /* golden 重算 */
                } catch (const std::bad_alloc &) {
                    log_skip(RuntimeSkipCategory, "memory: %s mdim=%d",
                             test->id, d->dim);
                    return EXIT_SKIP;
                }
            }
        } while (test_time_condition(test));
        return EXIT_SUCCESS;
    }
};

} // unnamed namespace

#endif // SANDSTONE_EIGEN_COMMON_H
