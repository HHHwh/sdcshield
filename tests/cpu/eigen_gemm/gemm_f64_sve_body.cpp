/*
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Eigen GEMM payload body — double,compiled ONCE PER VL TIER
 * (128/256/512/1024)。run 语义镜像 gemm_double_dynamic_square.cpp 的
 * run(per-thread operands + golden 同源首算 + kRerollEvery 重掷 +
 * memcmp_or_fail;T10 bad_alloc 窄 catch 边界契约);solve_once 谱系
 * 契约(检视 #51)同 gemm_double_dynamic_square.cpp 的 gemm_solve_once
 * (splitmix 确定性哈希,与 fill_rand 同分布)。服务
 * eigen_gemm_double_dynamic_square_sve 与 eigen_gemm_double14_sve
 * (两 dispatcher,同一计算,knob 名/lo-hi 差异在各自 TU)。
 * 签名契约见 sandstone_eigen_sve_dispatch.h。
 */
#include <sandstone.h>
#include "sdc_stress_config.h"

#include <Eigen/Core>
#include <new>
#include <cstdint>

#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#include <unistd.h>

#ifndef EIGEN_SVE_BODY_NS
#  error "EIGEN_SVE_BODY_NS must be defined by the build (one namespace per VL tier)"
#endif

namespace EIGEN_SVE_BODY_NS {

using namespace Eigen;

typedef Matrix<double, Dynamic, Dynamic> Mat;

constexpr int kRerollEvery = 16;

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

int run_body(struct test *test, int cpu)
{
    (void)cpu;
    const int dim = static_cast<gemm_sve_data *>(test->data)->dim;

    /* randomization hardening H11'(P14)+ T10 catch 边界:逐字镜像
     * gemm_double_dynamic_square.cpp run 的段结构。 */
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

        /* 比对段在 catch 之外(T10 边界契约)。 */
        memcmp_or_fail(x.data(), l_prod.data(), (size_t)dim * (size_t)dim);

        if (++since_reroll >= kRerollEvery) {
            since_reroll = 0;
            try {
                fill_rand(l_lhs);
                fill_rand(l_rhs);
                l_prod = l_lhs * l_rhs;     /* golden 重算 */
            } catch (const std::bad_alloc &) {
                log_skip(RuntimeSkipCategory, "memory: %s mdim=%d",
                         test->id, dim);
                return EXIT_SKIP;
            }
        }
    } while (test_time_condition(test));
    return EXIT_SUCCESS;
}

int solve_once(int dim)
{
    /* splitmix 同谱系探针(镜像 gemm_double_dynamic_square.cpp 的
     * gemm_solve_once):与 fill_rand 同分布、不依赖 per-thread RNG。 */
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

}  // namespace EIGEN_SVE_BODY_NS
