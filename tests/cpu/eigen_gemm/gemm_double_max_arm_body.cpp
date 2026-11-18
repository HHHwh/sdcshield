/*
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Eigen GEMM payload body — compiled ONCE PER VL TIER (128/256/512/1024).
 *
 * Why this file exists (design §4.7): Eigen's SVE packet types are
 * compile-time fixed-width — `arm_sve_vector_bits(EIGEN_ARM64_SVE_VL)` — and
 * are used as struct members (`PacketXcd`), so a single vector-length-
 * agnostic TU is impossible at the C++ language level (twice-confirmed
 * spike). The workaround is not to make one TU adaptive but to compile the
 * same source N times, each with its own Eigen namespace
 * (-DEigen=EigenSVEn) and its own exported symbol namespace
 * (-DEIGEN_SVE_BODY_NS=gemm_vln — macro unified across all Eigen SVE body
 * TUs 2026-11-02, was -DGEMM_TEST_NAMESPACE; the build now derives it from
 * the [body-source, namespace-prefix] table in tests/cpu/meson.build);
 * the dispatch TU picks the variant whose
 * width matches the worker's pinned VL.
 */
#include <sandstone.h>
#include "sdc_stress_config.h"
#include "gemm_solve_probe.h"

#include <Eigen/Core>

#include <cmath>
#include <cstdint>
#include <new>
#include <unistd.h>

#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>

#ifndef EIGEN_SVE_BODY_NS
#  error "EIGEN_SVE_BODY_NS must be defined by the build (one namespace per VL tier)"
#endif

namespace EIGEN_SVE_BODY_NS {

using namespace Eigen;

typedef Matrix<double, Dynamic, Dynamic> Mat;

constexpr int kRerollEvery = 16;

/* Per-child dimension holder (2026-11-02 unification): the dispatcher's
 * test_init sizes the dimension (knob/env > joint solver) and stores it
 * here via make_data; run_body reads it back from test->data. Replaces
 * both the extern global this TU read pre-T6 and the per-call parameter
 * the T6 signature carried — the dispatcher only passes (test, cpu) now,
 * same shape as the SVD pair's body contract. Layout is identical at
 * every tier (plain int), so the dispatcher treats it as an opaque void*. */
struct gemm_sve_data { int dim; };

void *make_data(int dim)
{
    auto *d = new gemm_sve_data;
    d->dim = dim;
    return d;
}

void cleanup(struct test *test)
{
    delete static_cast<gemm_sve_data *>(test->data);
    test->data = nullptr;
}

static void fill_rand(Mat &m)
{
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.cols(); ++c)
            m(r, c) = frandom_scale(2.0) - 1.0;
}

/* The dispatch TU has already pinned this worker to the tier this variant
 * was compiled for and verified the readback; here we only run the payload.
 * (Keeping the pin in the dispatcher means it happens exactly once per test
 * execution, not once per variant.) T6: the child init sizes the dimension
 * (knob/env > joint solver); since the 2026-11-02 unification it arrives
 * through test->data (see gemm_sve_data above), not as a parameter. */
int run_body(struct test *test, int cpu)
{
    (void)cpu;
    const int dim = static_cast<gemm_sve_data *>(test->data)->dim;
    /* T10 bad_alloc 降级(m2, 对齐 eigen_sparse 先例): 分配/求解段包窄
     * catch(仅 std::bad_alloc)→ 线程级 SKIP——资源性失败不是 SDC 不打红;
     * 比对段(memcmp_or_fail → report_fail)恒在 catch 之外, SDC 误配必须
     * 打红。worker 期不做降档重试(m2: 改共享尺寸有跨线程竞态,
     * v1 的重试+FAIL 语义与先例相反, 已撤销)。本 TU 的 init(dispatcher
     * 侧)不持有矩阵, 无 init 降级点; solve_once 探针由 dispatcher 的
     * catch(...)->MDIM_MIN 兜底。 */
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

        /* 比对段在 catch 之外(T10 边界契约, 见 run_body 头注)。 */
        memcmp_or_fail(x.data(), l_prod.data(), dim * dim);

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

/* 联合求解探针 (T6): 计时载荷——构两张 Mat + 同谱系填充 + 一次乘法,
 * 结果丢弃只为计时(禁止标量参考实现, 速度差数倍会错标时间模型)。
 * 填充实现已单点收口至 gemm_solve_probe.h(issue #389, 8→1);谱系
 * 契约(检视 #51)的 probe 侧随之单源化, probe↔run() 侧仍需与
 * fill_rand 手工同步演化(无编译期绑定, 检视 #392-R3); 本函数保留 extern 符号供 dispatcher 按 tier 链接
 * (gemm_double_max_arm.cpp 的 GEMM_TEST_NAMESPACE 声明)。
 * 执行线程语义: 本函数在 test_init 的 aux 线程上被 sdc_joint_dim_slice
 * 逐档调用; 框架在 test_init 前已把控制线程 pin 到选定档, init 线程继承
 * 该 VL(arm64_vl.h arm64_require_sve_vl 注释)——本命名空间的编译期固定
 * 宽度 packet(-msve-vector-bits=TIER)恰在该档执行, dispatcher 按 init
 * 观察到的 tier 分派到这里。 */
int solve_once(int dim)
{
    return gemm_probe::solve_once<Mat>(dim);
}

}  // namespace EIGEN_SVE_BODY_NS
