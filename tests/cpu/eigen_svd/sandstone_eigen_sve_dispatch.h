/**
 * @file
 *
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Eigen SVE per-tier payload 的公共 dispatcher 框架(设计
 * docs/superpowers/specs/2026-11-02-eigen-sve-unification-design.md §3)。
 *
 * 背景:Eigen 的 SVE packet 后端是编译期定宽(arm_sve_vector_bits),VLA
 * 在 C++ 语言层面不可编译(sizeless struct 成员,两次 spike 确认)——
 * payload 按 tier(128/256/512/1024)各编译一次,运行时按框架 pin 的 VL
 * 分派(先例 eigen_svd_cdouble_sve / eigen_gemm_double_max_arm)。
 * 本头把两个先例 dispatcher 中逐字重复的 tier 样板(命名空间声明 /
 * tier 白名单 / make_data / solve_once / run 重 pin / cleanup 分派)
 * 收敛为两个宏,全部 eigen SVE 用例(既有 2 个 + 新增 12 个)单点维护。
 *
 * 用法(dispatcher TU,每用例一文件):
 *   EIGEN_SVE_DECLARE_BODY_NS(my_pfx);      // 4 个 per-tier namespace
 *   EIGEN_SVE_DEFINE_DISPATCHERS(my_pfx);    // 分派套件
 *   ... 自己的 test_init(knob/env/联合求解 → my_pfx##_make_data(dim))
 *
 * body TU 契约(meson 按 tier 编 4 次,-DEIGEN_SVE_BODY_NS=<pfx>_vl<t>):
 *   namespace <pfx>_vl<t> {
 *     void *make_data(int dim);   // dim 已解出;分配本用例 test->data
 *     int    solve_once(int n);   // 联合求解计时探针(同 tier packet)
 *     int    run_body(struct test *, int cpu);  // 从 test->data 读 dim
 *     void   cleanup(struct test *);
 *   }
 * data 类型由 body 自定,跨 tier 布局一致(同结构定义),dispatcher 只见
 * void*。
 */

#ifndef SANDSTONE_EIGEN_SVE_DISPATCH_H
#define SANDSTONE_EIGEN_SVE_DISPATCH_H

#include <sandstone.h>
#include "sdc_stress_config.h"

#include <device/cpu/arm64/arm64_vl.h>

/* 每 body TU 一个 namespace per tier,导出同一组 4 函数。 */
#define EIGEN_SVE_DECLARE_BODY_NS(pfx)                                       \
    namespace pfx##_vl128  {                                                 \
        void *make_data(int); void cleanup(struct test *);                   \
        int run_body(struct test *, int); int solve_once(int);                \
    }                                                                        \
    namespace pfx##_vl256  {                                                 \
        void *make_data(int); void cleanup(struct test *);                   \
        int run_body(struct test *, int); int solve_once(int);                \
    }                                                                        \
    namespace pfx##_vl512  {                                                 \
        void *make_data(int); void cleanup(struct test *);                   \
        int run_body(struct test *, int); int solve_once(int);                \
    }                                                                        \
    namespace pfx##_vl1024 {                                                 \
        void *make_data(int); void cleanup(struct test *);                   \
        int run_body(struct test *, int); int solve_once(int);                \
    }

/* 分派套件(每 dispatcher TU 一次)。pfx 必须与 EIGEN_SVE_DECLARE_BODY_NS
 * 相同。生成符号全部 static——多个 dispatcher TU 共用本头不冲突。
 *
 * run 语义(迁移自 svd_cdouble_sve.cpp,全用例统一 fail-closed):
 *   - SVE 消失(pin 与 run 之间)→ SKIP;
 *   - pin 被拒(pinned != want)→ SKIP;
 *   - readback != want → FAIL(框架 arm64 spec policy 契约);
 *   - worker 档 != init 记录档 → FAIL(variant/data 跨档重解释防护;
 *     对 tier-agnostic data 的 GEMM 这是 fail-closed 增严,见迁移注释);
 *   - 全部通过 → 分派到 worker 实际档的 run_body。
 */
#define EIGEN_SVE_DEFINE_DISPATCHERS(pfx)                                    \
    static long pfx##_tier_bits;                                              \
                                                                              \
    static bool pfx##_tier_select(struct test *test, long svl_bytes)         \
    {                                                                         \
        switch (svl_bytes * 8) {                                              \
        case 128:                                                             \
        case 256:                                                             \
        case 512:                                                             \
        case 1024:                                                            \
            pfx##_tier_bits = svl_bytes * 8;                                  \
            return true;                                                      \
        default:                                                              \
            log_skip(CpuNotSupportedSkipCategory,                             \
                     "%s: no Eigen SVE variant compiled for the %ld-bit tier",\
                     test->id, svl_bytes * 8);                                \
            return false;                                                     \
        }                                                                     \
    }                                                                         \
                                                                              \
    static int pfx##_solve_once(int n)                                        \
    {                                                                         \
        switch (pfx##_tier_bits) {                                            \
        case 128:  return pfx##_vl128::solve_once(n);                         \
        case 256:  return pfx##_vl256::solve_once(n);                         \
        case 512:  return pfx##_vl512::solve_once(n);                         \
        case 1024: return pfx##_vl1024::solve_once(n);                        \
        default:   return -1;  /* unreachable: tier_select 先行 */            \
        }                                                                     \
    }                                                                         \
                                                                              \
    static void *pfx##_make_data(int dim)                                     \
    {                                                                         \
        switch (pfx##_tier_bits) {                                            \
        case 128:  return pfx##_vl128::make_data(dim);                        \
        case 256:  return pfx##_vl256::make_data(dim);                        \
        case 512:  return pfx##_vl512::make_data(dim);                        \
        case 1024: return pfx##_vl1024::make_data(dim);                       \
        default:   return nullptr;  /* unreachable: tier_select 先行 */       \
        }                                                                     \
    }                                                                         \
                                                                              \
    static int pfx##_run(struct test *test, int cpu)                          \
    {                                                                         \
        const long want_bytes = arm64_sve_vl_bytes();                         \
        if (want_bytes <= 0) {                                                \
            log_skip(CpuNotSupportedSkipCategory,                             \
                     "%s: SVE disappeared between pin and run", test->id);   \
            return EXIT_SKIP;                                                 \
        }                                                                     \
        const long pinned = arm64_sve_pin_vl(want_bytes);                     \
        if (pinned != want_bytes) {                                           \
            log_skip(CpuNotSupportedSkipCategory,                             \
                     "%s: cannot pin the worker to the %ld-bit VL "           \
                     "(prctl rc=%ld)", test->id, want_bytes * 8, pinned);      \
            return EXIT_SKIP;                                                 \
        }                                                                     \
        if (arm64_sve_vl_bytes() != want_bytes) {                             \
            log_error("%s: pinned %ld bytes but read back %ld",               \
                      test->id, want_bytes, arm64_sve_vl_bytes());            \
            report_fail_msg("%s: SVE VL readback mismatch after pin",         \
                            test->id);                                        \
        }                                                                     \
        if (want_bytes * 8 != pfx##_tier_bits) {                               \
            log_error("%s: run tier %ld-bit differs from the %ld-bit "        \
                      "variant the data was allocated for", test->id,          \
                      want_bytes * 8, pfx##_tier_bits);                       \
            report_fail_msg("%s: Eigen SVE variant/tier mismatch",           \
                            test->id);                                        \
        }                                                                     \
        log_info("%s: Eigen SVE variant = %ld-bit", test->id,                  \
                 want_bytes * 8);                                             \
        switch (want_bytes * 8) {                                             \
        case 128:  return pfx##_vl128::run_body(test, cpu);                   \
        case 256:  return pfx##_vl256::run_body(test, cpu);                   \
        case 512:  return pfx##_vl512::run_body(test, cpu);                   \
        case 1024: return pfx##_vl1024::run_body(test, cpu);                  \
        default:                                                              \
            log_skip(CpuNotSupportedSkipCategory,                             \
                     "%s: no Eigen SVE variant compiled for the %ld-bit "     \
                     "tier", test->id, want_bytes * 8);                        \
            return EXIT_SKIP;                                                 \
        }                                                                     \
    }                                                                         \
                                                                              \
    static void pfx##_cleanup(struct test *test)                              \
    {                                                                         \
        switch (pfx##_tier_bits) {                                            \
        case 128:  pfx##_vl128::cleanup(test);  break;                        \
        case 256:  pfx##_vl256::cleanup(test);  break;                        \
        case 512:  pfx##_vl512::cleanup(test);  break;                        \
        case 1024: pfx##_vl1024::cleanup(test); break;                        \
        default: break;  /* tier 未选 → init 已 SKIP,无 data */               \
        }                                                                     \
    }

#endif // SANDSTONE_EIGEN_SVE_DISPATCH_H
