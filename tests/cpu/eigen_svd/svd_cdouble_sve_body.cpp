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
 * 求解 —— 构 Mat + splitmix 同谱系填充 + 一次完整求解(calculate_once, 即
 * run() 每迭代 prime/DUT 两次求解中的同一次), 结果丢弃只为计时。
 * 谱系契约(检视 #51, 与 sandstone_eigen_common.h calibrated_auto_dim
 * 探针同一份): 填充必须与 run() 的 fill_matrix_random 同谱系(满秩随机
 * [-1,1))——结构化/条纹阵对 BDCSVD 走 deflation 快收敛路径, 探针会系统性
 * 低估真实求解时间。splitmix 散列(complex 独立 re/im)满足该契约。
 * 禁止标量参考实现(速度差数倍, 会错标时间模型)。
 * 执行线程语义: 本函数在 test_init 的 aux 线程上被 sdc_joint_dim_slice
 * 逐档调用; 框架在 test_init 前已把控制线程 pin 到选定档, init 线程继承
 * 该 VL(arm64_vl.h arm64_require_sve_vl 注释)——本命名空间的编译期固定
 * 宽度 packet(-msve-vector-bits=TIER)恰在该档执行, 与 make_data 分配的
 * 变体同档, dispatcher 按 init 观察到的 tier 分派到这里。 */
int solve_once(int dim)
{
    Mat m(dim, dim);
    for (int r = 0; r < dim; ++r)
        for (int c = 0; c < dim; ++c) {
            std::uint64_t h = (std::uint64_t)r * 0x9E3779B97F4A7C15ull
                            ^ (std::uint64_t)c * 0xC2B2AE3D27D4EB4Full;
            h ^= h >> 32;
            h *= 0xD6E8FEB86659FD93ull;
            h ^= h >> 32;
            std::uint64_t h2 = h * 0xD6E8FEB86659FD93ull
                             + 0x9E3779B97F4A7C15ull;
            h2 ^= h2 >> 32;
            m(r, c) = std::complex<double>(
                (double)(h % 2000u) / 1000.0 - 1.0,
                (double)(h2 % 2000u) / 1000.0 - 1.0);
        }
    Mat u, v;
    test_type::calculate_once(m, u, v);
    return 0;
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
