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
 * (-DGEMM_TEST_NAMESPACE=gemm_vln); the dispatch TU picks the variant whose
 * width matches the worker's pinned VL.
 */
#include <sandstone.h>
#include "sdc_stress_config.h"

#include <Eigen/Core>

#include <cmath>
#include <cstdint>
#include <new>
#include <unistd.h>

#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>

#ifndef GEMM_TEST_NAMESPACE
#  error "GEMM_TEST_NAMESPACE must be defined by the build (one namespace per VL tier)"
#endif

namespace GEMM_TEST_NAMESPACE {

using namespace Eigen;

typedef Matrix<double, Dynamic, Dynamic> Mat;

constexpr int kRerollEvery = 16;

static void fill_rand(Mat &m)
{
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.cols(); ++c)
            m(r, c) = frandom_scale(2.0) - 1.0;
}

/* The dispatch TU has already pinned this worker to the tier this variant
 * was compiled for and verified the readback; here we only run the payload.
 * (Keeping the pin in the dispatcher means it happens exactly once per test
 * execution, not once per variant.) T6: the dimension arrives as a
 * parameter — the child init sizes it (knob/env > joint solver) and the
 * dispatcher passes it per call; the extern global this TU used to read is
 * gone. */
int run_body(struct test *test, int cpu, int dim)
{
    (void)cpu;
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

/* 联合求解探针 (T6): 与 run 期同构的 tier 内 dgemm —— 构两张 Mat + 同谱系
 * 填充 + 一次乘法, 结果丢弃只为计时 (run() 每迭代 prime/DUT 乘法的同一次;
 * 谱系契约同 svd_cdouble_sve_body.cpp 的 solve_once, 检视 #51): 填充必须与
 * run() 的 fill_rand 同谱系(均匀随机 [-1,1))——splitmix 确定性哈希满足该
 * 契约, 且不依赖 init aux 线程上未定义的 per-thread RNG 状态。GEMM 的
 * 计时对数值不敏感(不同于 BDCSVD 的 deflation 路径), 谱系由分布一致性
 * 满足; 填充本身的 O(n²) 成本相对乘法 O(n³) 在 n>=1024 档可忽略(且已计入
 * 探针计时, 保守方向)。禁止标量参考实现(速度差数倍, 会错标时间模型)。
 * 执行线程语义: 本函数在 test_init 的 aux 线程上被 sdc_joint_dim_slice
 * 逐档调用; 框架在 test_init 前已把控制线程 pin 到选定档, init 线程继承
 * 该 VL(arm64_vl.h arm64_require_sve_vl 注释)——本命名空间的编译期固定
 * 宽度 packet(-msve-vector-bits=TIER)恰在该档执行, dispatcher 按 init
 * 观察到的 tier 分派到这里。 */
int solve_once(int dim)
{
    Mat a(dim, dim), b(dim, dim), c;
    for (int r = 0; r < dim; ++r)
        for (int col = 0; col < dim; ++col) {
            std::uint64_t h = (std::uint64_t)r * 0x9E3779B97F4A7C15ull
                            ^ (std::uint64_t)col * 0xC2B2AE3D27D4EB4Full;
            h ^= h >> 32;
            h *= 0xD6E8FEB86659FD93ull;
            h ^= h >> 32;
            std::uint64_t h2 = h * 0xD6E8FEB86659FD93ull
                             + 0x9E3779B97F4A7C15ull;
            h2 ^= h2 >> 32;
            a(r, col) = (double)(h % 2000u) / 1000.0 - 1.0;
            b(r, col) = (double)(h2 % 2000u) / 1000.0 - 1.0;
        }
    c = a * b;
    return 0;
}

}  // namespace GEMM_TEST_NAMESPACE
