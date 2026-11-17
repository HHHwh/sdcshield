/*
 * Copyright 2026 ISCAS.
 * SPDX-License-Identifier: Apache-2.0
 *
 * sdc_stress_config_tests.cpp — sdc_calibrated_dim 探针返回码契约的
 * 回归锁定(issue #420)。
 *
 * 背景: eigen_svd_bidiag_sve 的校准探针 run_bidiag_once 在分配失败时
 * 返回 -1, 而校准层的 probe lambda 曾完全丢弃 solve_once 返回值——
 * 失败探针的 μs 级非零计时噪声被当作超快求解外推(k=log2(t2/t1)≈0 被
 * k_floor 钳制, fit 走 a=t1/p1^k 得 ~1e-13 的"合法"系数), dim 钳到 hi,
 * 联合层 n_run 失控 → mdim 落 n_mem/hi 上限——P0-2 超时模式的复发路径。
 * 修复(framework/sdc_stress_config.h): 非零返回码 → 负哨兵计时 →
 * 退守校准下限 lo, fit 保持无效; 失败 ≠ "时间不是约束"(那是两者皆
 * 恰为 0 的语义, 仍然交还 hi), 与 Eigen 族探针 bad_alloc 上抛 →
 * 调用方 catch(...) → lo 的教义同向。
 *
 * 本 TU 只触达 sdc_calibrated_dim(联合层 sdc_joint_dim_slice 的失败
 * 退守由 issue #420 的 /tmp 机制层 harness 与 gem5-fi SVE 工作负载证据
 * 覆盖——单测侧链接 sdc_numa/反馈符号的边际成本大于其锁定价值, 如实
 * 记录该边界)。其外部依赖仅 log_warning 宏展开所需的 thread_num 与
 * log_message(sandstone.h 声明)——在此提供 unittests 域定义, 与
 * sandstone_unittests_utils.cpp 的 dummy 模式同款。
 */

#include <gtest/gtest.h>

#include "sdc_stress_config.h"

#include <chrono>

thread_local int thread_num = -1;

void log_message(int, const char *msg, ...)
{
    /* 静默: 校准层的 log_warning 是可观测性输出, 不进断言路径 */
    (void)msg;
}

/* run_bidiag_once 失败分支的忠实替身: burn ~50μs 后 return -1。
 * 真实失败分支(4 次 aligned_alloc 尝试 + 释放 + log_warning)本身耗时
 * μs 级且非零——这正是 issue #420 的"失败的探针仍测得 μs>0, 进不去
 * t<=0 守卫"。此处以确定性 busy-wait 固化该形态: 修复前本组用例确定
 * 性变红(dim=hi 而非依赖时钟恰好读到 0 的巧合), 修复后负哨兵与计时
 * 无关、判定确定。 */
static int probe_fail(int)
{
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::microseconds(50))
        ;
    return -1;
}

/* 顶档失败形态(瞬态内存压力: 大分配失败、小分配成功——探针抬升档
 * 与 mid 档的 n² 缓冲远大于低档) */
static int probe_top_fail(int n)
{
    if (n >= 1024)
        return probe_fail(n);
    return 0;
}

/* 健康探针: 确定性 ~50μs "求解"——锁健康路径不受修复影响(全部探针
 * 档 <100ms → 抬升到 hi, k→k_floor, dim 钳 hi, fit 经退化回退有效) */
static int probe_ok(int)
{
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::microseconds(50))
        ;
    return 0;
}

TEST(SdcCalibratedDim, ProbeFailureFallsBackToLo)
{
    struct sdc_fit fit = { -1.0, -1.0 };   /* 非法初值: 早退路径须写 {0,0} */
    int dim = sdc_calibrated_dim(64, 2048, 10000, "ut420", "mdim", 3.0,
                                 probe_fail, &fit, 1.0);
    EXPECT_EQ(dim, 64);                    /* 修复前: 2048(μs 噪声外推) */
    EXPECT_EQ(fit.a_ms, 0.0);              /* fit 无效 → 消费方跳过 n_run */
    EXPECT_EQ(fit.p, 0.0);
}

TEST(SdcCalibratedDim, ProbeFailureAtEscalatedTierFallsBackToLo)
{
    int dim = sdc_calibrated_dim(64, 2048, 10000, "ut420", "mdim", 3.0,
                                 probe_top_fail, nullptr);
    EXPECT_EQ(dim, 64);                    /* 修复前: 2048 */
}

TEST(SdcCalibratedDim, HealthyProbeUnchanged)
{
    struct sdc_fit fit = { 0.0, 0.0 };
    int dim = sdc_calibrated_dim(64, 2048, 10000, "ut420", "mdim", 3.0,
                                 probe_ok, &fit, 1.0);
    EXPECT_EQ(dim, 2048);                  /* 微秒级求解: 时间不构成约束 */
    EXPECT_GT(fit.a_ms, 0.0);              /* mid==p1 退化回退 a=t1/p1^k */
    EXPECT_DOUBLE_EQ(fit.p, 3.0);
}

/*
 * issue #692: sdc_joint_knob_value cache 命中重 clamp。cache 键
 * (test->id+"."+key)不含边界——命中值是按首次查询调用点的边界 clamp
 * 的结果, 边界不同的二次查询须按本次 [lo, hi] 重 clamp; 0(未显式配置
 * 哨兵)透传。生产代码当前所有调用方预热边界与 ja.lo/hi 一致、且显式值
 * 都在调用方 init 短路(命中路径今天只返回 0), 修复分支无法经真实二进制
 * 触达——按本文件头注的机制级验证教义, 直接操作本 TU 的 cache 实例
 * (static inline 函数局部 static, 每 TU 一份)锁定该分支契约。
 */
TEST(SdcJointKnobValue, CacheHitInRangePassesThrough)
{
    struct test t = {};
    t.id = "ut692";
    sdc_knob_cache()[sdc_knob_cache_key(&t, "mdim")] = 500;
    EXPECT_EQ(sdc_joint_knob_value(&t, "mdim", 64, 2048), 500);
}

TEST(SdcJointKnobValue, CacheHitAboveHiReclampsDown)
{
    struct test t = {};
    t.id = "ut692";
    sdc_knob_cache()[sdc_knob_cache_key(&t, "mdim")] = 5000;
    EXPECT_EQ(sdc_joint_knob_value(&t, "mdim", 64, 2048), 2048);  /* 修复前: 5000 */
}

TEST(SdcJointKnobValue, CacheHitBelowLoReclampsUp)
{
    struct test t = {};
    t.id = "ut692";
    sdc_knob_cache()[sdc_knob_cache_key(&t, "mdim")] = 16;
    EXPECT_EQ(sdc_joint_knob_value(&t, "mdim", 64, 2048), 64);    /* 修复前: 16 */
}

TEST(SdcJointKnobValue, CacheHitZeroSentinelPassesThrough)
{
    struct test t = {};
    t.id = "ut692";
    sdc_knob_cache()[sdc_knob_cache_key(&t, "mdim")] = 0;
    /* 0 = 未显式配置哨兵, 不参与 clamp——否则 def=0 语义(→ env 兜底/自适应
     * 链)会被 lo 抬升破坏 */
    EXPECT_EQ(sdc_joint_knob_value(&t, "mdim", 64, 2048), 0);
}
