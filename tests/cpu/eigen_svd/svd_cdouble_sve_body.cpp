/*
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Eigen SVD payload body — compiled ONCE PER VL TIER (128/256/512/1024).
 *
 * Same reason as eigen_gemm/gemm_double_max_arm_body.cpp (design §4.7):
 * Eigen's SVE packet types are compile-time fixed-width and appear as struct
 * members, so one vector-length-agnostic TU cannot exist at the C++ language
 * level. meson compiles this source once per tier with its own Eigen
 * namespace (-DEigen=EigenSVEn) and its own symbol namespace
 * (-DSVD_TEST_NAMESPACE=svd_vln); the dispatch TU selects at run time.
 *
 * The EigenSVDTest template's data type is itself Eigen-parameterised, so
 * allocation and cleanup live here too — the dispatcher only ever handles an
 * opaque void*.
 */
#include "sandstone_eigen_common.h"
#include "sdc_stress_config.h"

#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#include <unistd.h>
#include <cmath>

#ifndef SVD_TEST_NAMESPACE
#  error "SVD_TEST_NAMESPACE must be defined by the build (one namespace per VL tier)"
#endif

namespace SVD_TEST_NAMESPACE {

using namespace Eigen;

typedef Matrix<std::complex<double>, Dynamic, Dynamic> Mat;
typedef Eigen::BDCSVD<Mat> SVD;

using test_type = EigenSVDTest<SVD, Eigen::Dynamic>;

void *make_data(int dim)
{
    auto *d = new test_type::eigen_test_data;
    d->dim = dim;
    return d;
}

/* 联合求解探针 (P0-1, spec §4.5): 与 run 期同构的 tier 内 complex BDCSVD
 * 求解, 结果丢弃只为计时。PR#302 检视 #14 收口: 实现委托
 * test_type::sizing_probe(sandstone_eigen_common.h 的单一实现, 本 TU 以
 * -DEigen=EigenSVEn 编译该头 ⇒ 实例化于 tier 命名空间, 固定宽度 packet
 * 路径与本文件原内联拷贝逐字同语义)——消除两份拷贝的漂移风险: 探针
 * 填充一旦与 calibrated_auto_dim 的谱系契约脱钩, BDCSVD 走 deflation
 * 快收敛路径, 探针系统性低估真实求解时间, 联合求解选出过大的 n, 全核
 * 单次迭代超出 300s 宽限(正是本 PR 修复的超时模式)。
 * 谱系契约(检视 #51)由 sizing_probe 单点承载: 填充与 run() 的
 * fill_matrix_random 同谱系(满秩随机 [-1,1), splitmix 散列 complex
 * 独立 re/im)。禁止标量参考实现(速度差数倍, 会错标时间模型)。
 * 执行线程语义: 本函数在 test_init 的 aux 线程上被 sdc_joint_dim_slice
 * 逐档调用; 框架在 test_init 前已把控制线程 pin 到选定档, init 线程继承
 * 该 VL(arm64_vl.h arm64_require_sve_vl 注释)——本命名空间的编译期固定
 * 宽度 packet(-msve-vector-bits=TIER)恰在该档执行, 与 make_data 分配的
 * 变体同档, dispatcher 按 init 观察到的 tier 分派到这里。 */
int solve_once(int dim)
{
    return test_type::sizing_probe(dim);
}

void cleanup(struct test *test)
{
    test_type::cleanup(test);
}

int run_body(struct test *test, int cpu)
{
    return test_type::run(test, cpu);
}

}  // namespace SVD_TEST_NAMESPACE
