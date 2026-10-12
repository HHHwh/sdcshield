/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b acl_gemm_sve
 * @parblock
 * SVE port of acl_gemm: F32 GEMM (M=N=K=64) computed on SVE svmla
 * micro-kernels (outer-product accumulation per K-step, VL-agnostic
 * predicated loads) replacing the Arm Compute Library NEGEMM run path;
 * the golden stays the naive triple loop, computed with std::fmaf so
 * golden and SVE path share the identical fused single-rounding
 * multiply-add (the compiler fuses the SVE svmul+svadd pattern into
 * FMLA anyway, so an unfused double-rounding golden can never match
 * bit-exactly) and compare byte-exact on finite inputs (matrix entries
 * are filtered to finite values; NaN payload propagation is
 * implementation freedom, see fpu_special_values_sve). Any mismatch
 * indicates silent corruption in the FPU/FMA/SVE pipeline.
 * @endparblock
 */

#include "sandstone.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <sys/auxv.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif
#endif

#define GEMM_DIM 64

struct acl_gemm_sve_data {
    std::vector<float> A;
    std::vector<float> B;
    std::vector<float> golden;
};

static void naive_gemm(const float *A, const float *B, float *C)
{
    for (int i = 0; i < GEMM_DIM; ++i)
        for (int j = 0; j < GEMM_DIM; ++j) {
            float acc = 0.0f;
            for (int k = 0; k < GEMM_DIM; ++k) {
                /* fmaf: 单次舍入 fused 乘加, 与 SVE svmla(FMLA)舍入序
                 * 一致 -> 有限值输入下位级可比较。 */
                acc = std::fmaf(A[i * GEMM_DIM + k], B[k * GEMM_DIM + j], acc);
            }
            C[i * GEMM_DIM + j] = acc;
        }
}

static int acl_gemm_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for acl_gemm_sve");
        return EXIT_SKIP;
    }
#endif
    auto *d = new acl_gemm_sve_data;
    d->A.resize(GEMM_DIM * GEMM_DIM);
    d->B.resize(GEMM_DIM * GEMM_DIM);
    d->golden.resize(GEMM_DIM * GEMM_DIM);
    memset_random(d->A.data(), d->A.size() * sizeof(float));
    memset_random(d->B.data(), d->B.size() * sizeof(float));
    /* memset_random 任意位模式, 每矩阵约 1/256 元素为 NaN/Inf。NaN
     * payload 传播是 IEEE-754 实现自由(同类定性见 fpu_special_values_sve),
     * GEMM golden 无法对 NaN 位级比较; 过滤为有限值。特殊值边界行为
     * 由 fpu_special_values_sve 专门覆盖。 */
    for (size_t idx = 0; idx < d->A.size(); ++idx)
        if (!std::isfinite(d->A[idx]))
            d->A[idx] = 1.5f;
    for (size_t idx = 0; idx < d->B.size(); ++idx)
        if (!std::isfinite(d->B[idx]))
            d->B[idx] = 1.5f;
    naive_gemm(d->A.data(), d->B.data(), d->golden.data());
    test->data = d;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)
static int acl_gemm_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<acl_gemm_sve_data *>(test->data);
    std::vector<float> C(GEMM_DIM * GEMM_DIM);
    const int lanes = svcntw();
    float brow[64];

    TEST_LOOP(test, 64) {
        /* SVE GEMM: 每 i 行的累加器向量按 j-lane 展开, K 步进 svmla */
        for (int i = 0; i < GEMM_DIM; ++i) {
            for (size_t jbase = 0; jbase < (size_t)GEMM_DIM; jbase += lanes) {
                int n = (GEMM_DIM - (int)jbase < lanes) ? GEMM_DIM - (int)jbase : lanes;
                svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)n);
                svfloat32_t acc = svdup_f32(0.0f);
                for (int k = 0; k < GEMM_DIM; ++k) {
                    /* B 行 k 的 jbase..jbase+n 列 → 向量 (谓词控) */
                    for (int c = 0; c < n; ++c) brow[c] = d->B[k * GEMM_DIM + jbase + c];
                    svfloat32_t vb = svld1_f32(pg, brow);
                    /* svmla = FMLA fused 乘加(单次舍入), 与 golden 的
                     * fmaf 舍入序一致, 有限值输入下位级一致 */
                    acc = svmla_f32_x(pg, acc,
                              svdup_f32(d->A[i * GEMM_DIM + k]), vb);
                }
                svst1_f32(pg, C.data() + i * GEMM_DIM + jbase, acc);
            }
        }
        if (memcmp(C.data(), d->golden.data(), C.size() * sizeof(float)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (bit pattern + float) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < C.size(); ++_dump_j) {
                    uint32_t _dump_g, _dump_a;
                    memcpy(&_dump_g, &d->golden[_dump_j], 4);
                    memcpy(&_dump_a, &C[_dump_j], 4);
                    if (_dump_g != _dump_a) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("acl_gemm_sve: mismatch at index %zu: golden=0x%08x (%g) actual=0x%08x (%g) xor=0x%08x",
                                      _dump_j, _dump_g, d->golden[_dump_j], _dump_a, C[_dump_j],
                                      _dump_g ^ _dump_a);
                            ++_dump_shown;
                        }
                    }
                }
                log_error("acl_gemm_sve: total mismatched elements: %zu / %d",
                          _dump_bad, (int)C.size());
            }
            report_fail_msg("acl_gemm_sve: GEMM output miscompare (SVE svmla path)");
        }
    }
    return EXIT_SUCCESS;
}
#else
static int acl_gemm_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    return EXIT_SKIP;
}
#endif

static int acl_gemm_sve_finish(struct test *test)
{
    delete static_cast<acl_gemm_sve_data *>(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(acl_gemm_sve, "F32 GEMM (64x64) on SVE svmla micro-kernels vs naive golden (port of acl_gemm)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = acl_gemm_sve_init,
    .test_run = acl_gemm_sve_run,
    .test_cleanup = acl_gemm_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
