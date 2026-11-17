/**
 * @file
 *
 * @copyright
 * Copyright 2026 ISCAS.
 * SPDX-License-Identifier: Apache-2.0
 *
 * sandstone_gemm_common.h — eigen_gemm 用例族的共享 helper(issue #440)。
 *
 * 此前 T10 init 降档块(约 25 行)与联合求解探针 gemm_solve_once 在本目录
 * 7 个文件中逐字复制粘贴(double14 / {double,float,cdouble}_dynamic_square
 * 及其 _dump 变体), 降档处的反馈口径失效(PR#302 检视 #25 引入的
 * sdc_joint_cache_note_dim 同步, issue #545 改为 sdc_joint_cache_drop)
 * 被迫同步打进全部 7 份——漏一处即口径分叉。本头收敛 init
 * 降档块; 联合求解探针已由 PR #392 的 gemm_solve_probe.h(谱系单源
 * eigen_splitmix_lineage.h, issue #389)承载——本头不复制探针, 两类
 * 修正各落各的唯一权威处。
 *
 * 不在共享范围(如实边界): gemm_mdim 的 knob/env/clamp/fam 是各文件真参数;
 * gemm_double_max_arm 系 init 不持有矩阵、无降档点; eigen_svd_bidiag_sve
 * 的降档是裸 aligned_alloc 口径(lo=64), 与 Eigen Mat 异常口径不同构。
 */

#ifndef SANDSTONE_GEMM_COMMON_H
#define SANDSTONE_GEMM_COMMON_H

#include <sandstone.h>
#include "sdc_stress_config.h"

#include <Eigen/Core>
#include <complex>
#include <cstdint>
#include <new>

/* ------------------------------------------------------------------ */
/* T10 init 降档块(7 用例共享; 跨架构无守卫——替换的原内联代码即无守卫)  */
/* ------------------------------------------------------------------ */

/* eigen_gemm 族 init 主体: 调用方 mdim 函数解维度(-O knob > env > 自适应,
 * 只解一次)→ 共享三矩阵(lhs/rhs/prod)分配与 golden 首算; std::bad_alloc
 * 时按维度来源分流(issue #488)。Data 是各文件匿名命名空间的
 * eigen_test_data{Mat lhs, rhs, prod; int dim}(成员名即接口), mdim 是该
 * 文件的尺寸求解函数。mdim 出参契约(explicit_dim): 共享头调用前预置
 * false, mdim 仅在返回值来自用户显式通道(-O/配置文件 knob 或 env)时置
 * true; 自适应路径与 mdim 求解期抛异常(探针 bad_alloc 等)均不置位——
 * 共享头按自适应处理。与被收敛的 7 份内联拷贝的差异仅 catch 分流:
 *   - init 期分配失败是资源性而非 SDC →
 *     dim 来自自适应: 降 mdim 到 16(与联合求解的 lo/clamp 下限同值)
 *     重分配一次, 仍失败才资源性 SKIP(不打红);
 *     dim 来自用户显式通道: 不降档——T11 契约「显式值用户自担、旁路
 *     一切自动」, 静默 4096→16 把压力砍到 1/65536 还报 PASS 属假通过
 *     (rule 6), 资源不足诚实 SKIP(issue #488);
 *   - 降档成功 → sdc_joint_cache_drop 注销反馈登记(issue #545, 取代
 *     PR#302 检视 #25 的『同步分母 n』): 本 child 的 VmHWM 含降档前
 *     部分成功的 O(n²) 分配峰值, 只同步分母会把 eff 高估
 *     (n_solved/16)² 倍, 读侧向上无死区直接采纳 → 后续
 *     fracture 的 n 被压向 lo、压力静默塌缩; #385 的基线扣除只移除
 *     fork 起点、4×锚上钳只限幅, 都不恢复被高估的工作集 → 不发布
 *     样本仍是唯一不引入虚假记忆的选择;
 *   - 其余异常 → report_fail_msg(打红)。
 * 触发面边界(实测 2026-11-08): 生产构建链接 framework/sysdeps/linux/
 * malloc.cpp 的链接级 malloc 覆盖, 分配失败被 null_pointer_consumption
 * 拦截直接 raise(SIGKILL), bad_alloc 在生产构建不可达——显式 mdim 内存
 * 不足在生产构建表现为 crash(TestResult::OutOfMemory)而非降档假 PASS;
 * 本头的 init catch 分流与族内各用例 run() 的同型 bad_alloc→线程级
 * SKIP catch——double14 与 {double,float,cdouble}_dynamic_square{,_dump}
 * (include 本头的 7 用例)及 4 个不 include 本头的 per-tier body
 * (gemm_double_max_arm_body / gemm_{f64,f32}_sve_body / gemm_c128_sve_body,
 * 由各 dispatcher include)——均只在异常可达构建(ASAN/TSAN, 插桩被
 * __SANITIZE_* 排除)生效, 生产 run 期分配失败同样表现为 crash(issue
 * #640: 族级边界自此声明在代码处, 不再只存于已出库的 plan 文档)。
 * dump 变体的冻结回放轮读 d->lhs/rhs/prod——降档后三者与 d->dim 同档
 * 一致(降档路径对 dump 同样成立); 显式 SKIP 路径 run 不执行, 无交互。 */
template <typename Data>
static inline int sdc_gemm_init_shared(struct test *test, Data *d,
                                        int (*mdim)(struct test *, bool *))
{
    using Mat = decltype(d->lhs);
    int dim = 0;
    bool explicit_dim = false;   /* mdim 仅在显式通道(knob/env)置 true */
    try {
        dim = mdim(test, &explicit_dim);   /* knob > env > 自适应, 只解一次 */
        d->dim = dim;
        d->lhs = Mat::Random(dim, dim);
        d->rhs = Mat::Random(dim, dim);
        d->prod = d->lhs * d->rhs;
    } catch (const std::bad_alloc &) {
        if (explicit_dim) {
            log_skip(RuntimeSkipCategory,
                     "memory: %s mdim=%d (user-explicit, not downgradable)",
                     test->id, dim);
            return EXIT_SKIP;
        }
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
        /* issue #545: 降档后 run 的工作集是 retry 档, 但 VmHWM 峰值
         * 含降档前部分成功的 O(n²) 分配, 无法按 retry 口径折算——
         * 注销登记, 本 child 不写反馈。 */
        sdc_joint_cache_drop(test);
    } catch (...) {
        report_fail_msg("Exception on Eigen code, most probably OOM");
    }
    return EXIT_SUCCESS;
}

#endif /* SANDSTONE_GEMM_COMMON_H */
