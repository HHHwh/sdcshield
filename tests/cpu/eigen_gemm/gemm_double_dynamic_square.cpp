/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_gemm_double_dynamic_square
 * @parblock
 * This piece of code aims to stress test the exec (in particular FMA)
 * by repetitively solving general matrix matrix multiplication.  The
 * multiplication function is from the 3rd party library Eigen.  A
 * pair of random matrices are generated as inputs and then multiplied
 * using Eigen's gemm. The multiplication result is compared against a
 * golden result that is computed during init.  This particular
 * version of the test goes for double precision input matrices.
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
#include <cstdint>
#include <new>
using namespace Eigen;

typedef Matrix < double, Dynamic, Dynamic > Mat;

#if defined(__aarch64__) && defined(__linux__)
/* 联合求解 sizing 探针(T9): 谱系契约(检视 #51: 填充与 run() 的
 * fill_rand 同谱系, 均匀随机 [-1,1))已单点收口至 gemm_solve_probe.h
 * (issue #389, 含 T6 gemm_double_max_arm_body 源头共 8→1)——填充唯一定义, 探针副本间
 * 漂移在编译期不可能; probe↔run() 谱系契约仍需与 fill_rand 手工
 * 同步演化(无编译期绑定, 检视 #392-R3)。 */
static int gemm_solve_once(int n)
{
    return gemm_probe::solve_once<Mat>(n);
}
#endif

/*
 * 矩阵维度 N: 运行时可配, 优先级链:
 *   1. -O eigen_gemm_double_dynamic_square.mdim=N / 配置文件同名条目
 *      (sdc_stress.conf 或 $SDC_STRESS_CONFIG)
 *   2. 环境变量 SDC_EIGEN_GEMM_DOUBLE_DYNAMIC_SQUARE_MDIM
 *   3. 缺省自适应(arm64: 联合求解; 其他架构: 旧内存自适应链),
 *      clamp [16, 4096]。
 * 历史缺省 256 (= 未配置且小内存环境下的自适应下限附近)。
 * 尺寸只在 test_init 求解一次并缓存进 eigen_test_data.dim——worker
 * 读缓存, 不再重算(T9: 联合求解的探针与口径登记不能在 worker 上下文
 * 重入, 旧链的 run 期重跑自适应路径已删除)。
 * is_explicit 出参(issue #488): 显式通道(knob/env)来源时置 true,
 * 供 sdc_gemm_init_shared 的 bad_alloc 分流(显式值不可降档)。
 */
static int gemm_mdim(struct test *test, bool *is_explicit)
{
    int64_t v = sdc_knob_int_clamped(test, "mdim", 0, 16, 4096);
    if (v == 0) {
        int64_t envv = 0;
        if (sdc_env_knob_int("eigen_gemm_double_dynamic_square", "mdim", &envv))
            v = (envv < 16) ? 16 : (envv > 4096 ? 4096 : envv);
    }
    if (v > 0) {
        *is_explicit = true;    /* knob(-O/配置文件)/env 显式通道 */
        return (int)v;
    }
#if defined(__aarch64__) && defined(__linux__)
    /* 联合求解(T9, 同 T6 gemm_double_max_arm / T8 SVD 族模式): 一次
     * sdc_joint_dim_slice 同时约束内存(per-NUMA-node 四源预算 vs RSS
     * 表 GEMM_F64 实测系数)与时间(全核单迭代预算 × AR 幂律外推)。
     *   - legacy 三元组 (8,4,6.0) 是 fam 失配时的防御性 fallback,
     *     fam 命中 RSS 表后忽略;
     *   - shared_bytes_per_slice=0: init 持有 3 个共享矩阵 lhs/rhs/
     *     prod(3×8×n², n=4096 天花板时 403MB)——单 slice 相对节点预算 <1%
     *     (聚合全节点约 3%), n_mem 预算口径一阶忽略(T4 同款近似); 该项
     *     不随 slice 线程数缩放, 在 VmHWM 反馈 eff 口径里曾被整摊进
     *     per-thread 项(-n 1 部署 eff +74%), 现由 issue #766 框架侧统一
     *     扣除(反馈分子基线推进到 init 后), 调用方无需登记;
     *   - run_budget_ms=200000 保持契约值(GEMM 族, T6 同款无补偿)。
     *     旧已知边界(T6 审查实测)已由 #506 AR 表重标定收口(3c022449,
     *     2026-11-08; issue #689 审计复核): 旧表 compute 族单节点 5.0
     *     低估天花板足迹的 GEMM——N=3364 全核单乘实测 ~55-60s vs 静默
     *     模型 ~4.0s, 有效 AR≈15(3× 旧表值), 时间侧绑定的主机会把 200s
     *     迭代实际跑成 ~600s(超 300s 硬宽限 SIGQUIT); 表行1 comp 现为
     *     该实测上界 15.0(sdc_numa.cpp 表注: 中足迹 4.4/256-bit 档
     *     11.2/天花板 ~15, 取上界), 时侧 binding 主机单迭代回到
     *     ~200s ≤ 300s 宽限。本宿主类内存侧先绑定(n_mem < n_run),
     *     该路径本机不暴露。
     * x86-64(rule 5): #else 保留旧链原样——joint 的 x86 分支已补齐
     * 时间校准与四源预算(#482), 但迁移仍会把内存三元组换成 ARM 实测
     * 的 RSS 表/四源全局预算, sizing 数值变化, rule 5 保持旧链。 */
    struct sdc_joint_args ja = {
        .lo = 16, .hi = 4096,
        .fam = SDC_RSS_GEMM_F64,    /* 0.0315 KB/n²/线程, T9 重标定后 */
        .bytes_per_elem = 8, .matrices_per_thread = 4, .rss_ratio = 6.0,
        .shared_bytes_per_slice = 0,
        .mem_fraction_permille = 850,
        .ar_fam = SDC_FAM_COMPUTE_BOUND,
        .run_budget_ms = 200000,    /* 全核单迭代(填充+一次乘法×AR)预算 */
    };
    try {
        return sdc_joint_dim_slice(test, "mdim", &ja, gemm_solve_once);
    } catch (...) {
        /* 探针分配异常(瞬时 OOM 等): 退守下限——「上限宁小勿大」
         * (T6/T8 同 doctrine), 探针绝不能终止 child。 */
        log_warning("[联合求解] %s.%s: 探针异常 → 退守下限 %d",
                    test->id, "mdim", 16);
        return 16;
    }
#else
    return sdc_auto_square_dim(8, 4, 6.0, 850, thread_count(), 16, 4096, test->id, "mdim");
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

static int eigen_gemm_double_dynamic_square_init(struct test *test) {
    test->data = new(eigen_test_data);
    return sdc_gemm_init_shared(test, CAST(test->data), gemm_mdim);
}

static int eigen_gemm_double_dynamic_square_run(struct test *test, int cpu) {
    /* randomization hardening H11' (P14): per-thread operands re-rolled
     * every kRerollEvery iterations (framework RNG, -s reproducible,
     * [-1, 1) bounded like Mat::Random); golden recomputed the same
     * batch. No shared state is written after init. */
    constexpr int kRerollEvery = 16;
        auto fill_rand = [](Mat &m) {
            for (int r = 0; r < m.rows(); ++r)
                for (int c = 0; c < m.cols(); ++c)
                    m(r, c) = frandom_scale(2.0) - 1.0;
        };
    const int dim = CAST(test->data)->dim;   /* init 缓存值——worker 不重算 */
    /* T10 bad_alloc 降级(m2, 对齐 eigen_sparse 先例): 分配/求解段包窄
     * catch(仅 std::bad_alloc)→ 线程级 SKIP——资源性失败不是 SDC 不打红;
     * 比对段(memcmp_or_fail → report_fail)恒在 catch 之外, SDC 误配必须
     * 打红。worker 期不做降档重试(m2: 改共享 d->dim 有跨线程竞态,
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

    do {
        Mat x;
        try {
            x = l_lhs * l_rhs;              /* DUT 求解 */
        } catch (const std::bad_alloc &) {
            log_skip(RuntimeSkipCategory, "memory: %s mdim=%d", test->id, dim);
            return EXIT_SKIP;
        }

        /* 比对段在 catch 之外(T10 边界契约, 见 run 内头注)。 */
        memcmp_or_fail(x.data(), l_prod.data(), (size_t)dim * (size_t)dim);

        if (++since_reroll >= kRerollEvery) {
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
    } while (test_time_condition(test));
    return EXIT_SUCCESS;
}

static int eigen_gemm_double_dynamic_square_finish(struct test *test) {
    delete(CAST(test->data));
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_gemm_double_dynamic_square, "Eigen GEMM payload (double, dynamic, square)")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_gemm_double_dynamic_square_init,
  .test_run = eigen_gemm_double_dynamic_square_run,
  .test_cleanup = eigen_gemm_double_dynamic_square_finish,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
