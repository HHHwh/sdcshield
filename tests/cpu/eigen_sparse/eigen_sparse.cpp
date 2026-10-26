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
 * @note Detection works at any core count: the per-thread same-source
 * repeat (recompute + memcmp against the golden produced on the same
 * thread) catches transient/intermittent non-determinism even when
 * single-threaded; multi-core only raises the concurrent-exposure
 * probability. Full-core is the recommended detection posture.
 *
 * Detection-channel disclosure: the same-source repeat detects
 * NON-deterministic defects — transient/intermittent/marginal errors
 * where the same computation yields different bits on the same input
 * (this includes errors that hit the GOLDEN computation itself: the
 * next same-source re-run mismatches the corrupted golden and FAILs).
 * It is structurally blind to deterministic core defects (a
 * permanently wrong instruction/data path computes the golden and
 * the re-run identically wrong, so the memcmp matches). Accuracy
 * note (检视 #52): the fixed-golden test family covers deterministic
 * defects on its OWN instruction paths (FMA/SIMD/crypto/compression
 * golden data) — no test in the suite exercises any Eigen
 * factorization path against fixed golden data (the whole Eigen
 * family gemm/sparse/svd is same-source repeat), so deterministic
 * defects on the sparse-Cholesky path are a disclosed coverage gap.
 * The old shared-golden structure did provide a probabilistic
 * channel for them (a deterministically wrong worker core mismatched
 * the init golden), at the cost of the full-core false-positive
 * channel that dominated operations; this design trades the former
 * away to eliminate the latter — a deliberate, disclosed choice.
 * @endparblock
 */

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <sandstone.h>
#include "sdc_stress_config.h"

#include <Eigen/Sparse>

/* 时间校准下限(与 sdc_calibrated_dim 的 lo 实参一致; probe 异常
 * 兜底退守到此, 见 sparse_n 内注释)。 */
static constexpr int kCalibLo = 64;

/* —— 校准 probe 与 generate_problem 的负载形状契约(检视 #43) ——
 * 两处独立构造同一个负载形状, 以下参数必须同步演化: 非对角密度支配
 * Cholesky fill-in(= 校准的计时基数), 对角值域支配数值性状。单边
 * 改动会使校准计时静默失真——改这里必须同时核对另一处。probe 的
 * 非对角取常值 0.05 = generate 值域 [0, 0.1) 的均值(计时近似)。 */
static constexpr int kOffDiagDensityDenom = 10; /* 命中率 1/10 (~10% 密度) */
static constexpr double kDiagBase = 0.05;       /* generate: |x|+base ∈ [0.05,1.05);
                                                   probe: i+1+base 强对角占优恒正定 */

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
    int mem_dim = sdc_auto_square_dim(8, 4, 4.0, 700, thread_count(), 64, 8192, test->id, "n");
    /* P2 time calibration: one full Cholesky solve at the memory-adaptive
     * dim can run for minutes inside a single loop iteration (the -t TERM
     * cannot interrupt;实测自适应档满核 600s SIGQUIT timed out) — cap it
     * so TERM is honoured. Deterministic pattern fill, ~10% density,
     * diagonally dominant; probe-only, results discarded. */
    if (mem_dim > kCalibLo) {
        int calibrated;
        try {
            calibrated = sdc_calibrated_dim(
                kCalibLo, mem_dim, 2000, test->id, "n", 3.0, [](int n) {
                /* 散列伪随机放置(~10% 密度, 空间散乱非带状)——带状/规则
                 * 结构的 Cholesky fill-in 极少, 会把 probe 成本低估几个
                 * 数量级(实测规则 probe 64 档仅 0.1ms vs 真实随机 ~ms 级),
                 * 必须模拟真实随机分布的 fill-in 结构。
                 * 64-bit mix(splitmix 风格乘法双射 + 高半混合): 旧 32 位
                 * 散列 (i*0x9E3779B1 ^ j*0x9E37) 的 bit0 = i奇偶^j奇偶,
                 * 异奇偶 (i,j) 恒不命中(实测 0/16777216), 同奇偶密度翻倍
                 * → 奇偶行列重排后呈置换块对角, fill-in 低估 ~2x/flops
                 * ~4x(实测 n=1024: 旧 probe 0.22x real, 新 0.95x)。 */
                Eigen::SparseMatrix<double> A(n, n);
                std::vector<Eigen::Triplet<double>> trip;
                for (int i = 0; i < n; ++i)
                    for (int j = i + 1; j < n; ++j) {
                        uint64_t h = (uint64_t)i * 0x9E3779B97F4A7C15ull
                                   ^ (uint64_t)j * 0xC2B2AE3D27D4EB4Full;
                        h ^= h >> 32;
                        if (h % kOffDiagDensityDenom == 0) {
                            trip.push_back(Eigen::Triplet<double>(i, j, 0.05));
                            trip.push_back(Eigen::Triplet<double>(j, i, 0.05));
                        }
                    }
                for (int i = 0; i < n; ++i)
                    trip.push_back(Eigen::Triplet<double>(i, i, i + 1.0 + kDiagBase));
                A.setFromTriplets(trip.begin(), trip.end());
                Eigen::VectorXd b = Eigen::VectorXd::Constant(n, 0.5);
                Eigen::SimplicialCholesky<Eigen::SparseMatrix<double>> solver;
                /* solve() 返回惰性表达式(SparseSolverBase::solve →
                 * Solve<> 模板), 丢弃返回值则三角回代根本不执行,
                 * probe 只计了 compute(分解)。必须落入具体向量才
                 * 与 run() 的 "x = compute(A).solve(b)" 同构(检视
                 * #44)。 */
                Eigen::VectorXd y = solver.compute(A).solve(b);
            });
        } catch (...) {
            /* probe 期分配异常(如 ASAN 构建的 OOM): init 不 crash,
             * 退守校准下限并诚实记录(检视 #40)。计划文档 P2 原则
             * 『上限宁小勿大』: 退内存自适应档在异常为环境性/瞬时性
             * (ASAN 配额、init 期外部内存压力解除)时会静默复活
             * 『单解分钟级、TERM 打不断』的老问题; 退 lo 保证 TERM
             * 可兑现, 压力变小但口径诚实, 且 run() 仍执行同一指令
             * 路径(ASAN 场景优于整测 SKIP)。 */
            log_warning("[时间校准] %s.%s: 试解异常 → 退守校准下限 %d",
                        test->id, "n", kCalibLo);
            calibrated = kCalibLo;
        }
        mem_dim = calibrated;
    }
    return (size_t)mem_dim;
}

namespace {
struct EigenSparseTestData {
    size_t n;   /* matrix dimension — the only field run() consumes */
};

/* Same generator semantics as the historical initialize_problem():
 * ~10% density symmetric off-diagonal entries in [0, 0.1), diagonal
 * in [0.05, 1.05) (fabs + kDiagBase), b in [-1, 1). NOTE: the matrix
 * is only *weakly* diagonally dominant — expected off-diagonal row
 * sum is 0.005*(n-1) (10% hit × E[0,0.1)=0.05), which exceeds the
 * 1.05 diagonal cap for n ≳ 212, so Gershgorin positive-
 * definiteness does NOT hold at production dims. This is fine for
 * the solver actually used: SimplicialCholesky defaults to LDLT
 * (m_LDLT(true)), and LDLT 的 NumericalIssue 唯一触发条件是主元
 * 精确为 0(vendored Eigen 5 SimplicialCholesky_impl.h:
 * `if (d == RealScalar(0)) ok = false`)——负主元(非正定)不构成
 * 失败, LDLT 本就容忍不定阵, 故『非正定 → 数值失败假阳』通道
 * 在浮点意义下近似测度零(检视 #46)。info() != Success →
 * report_fail 维持 FAIL: 它同时是『生成期位翻转恰好打掉主元』
 * 这类结构损坏的唯一检出通道, 降级为 SKIP 会引入漏检。
 * do NOT switch to LLT (setMode(SimplicialCholeskyLLT)) without
 * re-validating — LLT 路径的失败条件是 d <= 0(真·非正定即炸)。
 * Operates on the CALLING thread's objects — no shared state. */
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
            if (x < 1.0 / kOffDiagDensityDenom) {
                trip.push_back(Eigen::Triplet<double>(i, j, x));
                if (j > i)
                    trip.push_back(Eigen::Triplet<double>(j, i, x));
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        double x = fabs(frandom_scale(1.0)) + kDiagBase;
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
 * same-source repeat, fresh operands). reroll 档迭代的真实成本 ≈
 * 构造 + 2×求解(golden 重算 + 下一轮 x), TERM 等待窗口按此口径
 * 估计——见 sdc_stress_config.h 的满核实测注记。 */
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
    try {
        generate_problem(n, A, b);
    } catch (...) {
        log_skip(TestResourceIssueSkipCategory, "Exception on Eigen code, most probably OOM");
        return EXIT_SKIP;
    }
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

    /* 异常分类契约(检视 #39/#49, 两轮意见方向相反故显式固定): 各路径
     * 逐字保留 main 基线的分类——run 期求解异常自 OpenDCDiag 原始
     * 代码起就是 report_fail(异常即异常, 宁可误报), 构造/golden
     * 异常自 init 时代起就是资源 SKIP(golden 无法建立 = 该线程无
     * 基准可言)。本 PR 把 golden 搬进 run 后不引入任何分类语义变化。
     * 内存水位: 每线程峰值在 reroll 档(新旧两个 LDLT 因子短暂并存),
     * 恰在 SKIP 防护内; x 路径仅持单因子, 水位低于 reroll 档。
     * 检出无损论证(检视 #49): ① NDEBUG 生产构建下 Eigen 断言关闭,
     * compute/solve 实际可抛的异常只有 bad_alloc(资源性, SKIP 语义
     * 正确); ② SDC 打坏 golden 的"值"不丢检——下一轮同源重算
     * memcmp 立即 mismatch FAIL; ③ A 结构的持久损坏以 16:1 暴露比
     * 先打在 x 路径(每 reroll 周期 16 次 x 求解 vs 1 次 golden);
     * ④ SKIP 亦有痕(log_skip 落日志), 仅不进战役 fail 分类。 */
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
        /* bit-exact compare (memcmp_or_fail 的 count 语义是元素数,
         * 内部乘 sizeof(double)): Eigen 的 operator!= 走 IEEE ==,
         * 存在两类失真——(a) +0.0 == -0.0 视为相等(符号位盲区);
         * (b) 同位 NaN 的 x != golden 恒真(同源重算位级一致的合法
         * 结果被误报 FAIL)。same-source repeat 的验证属性是位级
         * 一致, memcmp 语义与之精确匹配。 */
        memcmp_or_fail(x.data(), golden.data(), n, "sparse Cholesky solution");

        if (++since_reroll >= kRerollEvery) {
            since_reroll = 0;
            try {
                generate_problem(n, A, b);
            } catch (...) {
                log_skip(TestResourceIssueSkipCategory, "Exception on Eigen code, most probably OOM");
                return EXIT_SKIP;
            }
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
