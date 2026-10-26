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
#include "sdc_stress_config.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

/* GEMM 维度 n (M=N=K) 运行时可配置, 统一优先级链:
 *   1. -O acl_gemm_sve.dim=N / 配置文件同名条目
 *   2. 环境变量 SDC_ACL_GEMM_SVE_DIM
 *   3. 自适应: float 4B/元素, 每线程 A/B/golden 共享 + per-thread C ≈
 *      4 份矩阵, rss_ratio 2.0 (分配器余量, GEMM 无额外临时大矩阵),
 *      占物理内存 60%, clamp [64, 4096]。原硬编码 64。 */
struct acl_gemm_sve_data {
    int n;                   /* 运行时 GEMM 维度 (M=N=K) */
    std::vector<float> A;
    std::vector<float> B;
    std::vector<float> golden;
};

static void naive_gemm(const float *A, const float *B, float *C, int n)
{
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            float acc = 0.0f;
            for (int k = 0; k < n; ++k) {
                /* fmaf: 单次舍入 fused 乘加, 与 SVE svmla(FMLA)舍入序
                 * 一致 -> 有限值输入下位级可比较。 */
                acc = std::fmaf(A[(size_t)i * n + k], B[(size_t)k * n + j], acc);
            }
            C[(size_t)i * n + j] = acc;
        }
}

static int acl_gemm_sve_init(struct test *test)
{
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
     * layer: arm64_require_sve_vl does the SVE capability check
     * + 128..1024-bit range gate in one call (skip reason
     * framework-issued). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    auto *d = new acl_gemm_sve_data;

    /* 统一配置链解析维度 (见上方注释) */
    int64_t v = sdc_knob_int_clamped(test, "dim", 0, 64, 4096);
    if (v == 0)
        sdc_env_knob_int_clamped("acl_gemm_sve", "dim", &v, 64, 4096);
    d->n = (v > 0) ? (int)v
                   : sdc_auto_square_dim(4, 4, 2.0, 600, thread_count(),
                                         64, 4096, test->id, "dim");

    d->A.resize((size_t)d->n * d->n);
    d->B.resize((size_t)d->n * d->n);
    d->golden.resize((size_t)d->n * d->n);
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
    naive_gemm(d->A.data(), d->B.data(), d->golden.data(), d->n);
    test->data = d;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)
static int acl_gemm_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<acl_gemm_sve_data *>(test->data);
    const int N = d->n;
    std::vector<float> C((size_t)N * N);
    const int lanes = svcntw();
    /* B 行转置缓冲: n 可大于 64, 栈上定长放不下 -> 堆上一次性分配,
     * run 只调一次, 循环外统一释放。 */
    float *brow = (float *)aligned_alloc(64, (size_t)N * sizeof(float));
    if (!brow) {
        report_fail_msg("acl_gemm_sve: brow alloc failed");
        return EXIT_FAILURE;
    }

    TEST_LOOP(test, 64) {
        /* SVE GEMM: 每 i 行的累加器向量按 j-lane 展开, K 步进 svmla */
        for (int i = 0; i < N; ++i) {
            for (size_t jbase = 0; jbase < (size_t)N; jbase += lanes) {
                int n = (N - (int)jbase < lanes) ? N - (int)jbase : lanes;
                svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)n);
                svfloat32_t acc = svdup_f32(0.0f);
                for (int k = 0; k < N; ++k) {
                    /* B 行 k 的 jbase..jbase+n 列 → 向量 (谓词控) */
                    for (int c = 0; c < n; ++c) brow[c] = d->B[(size_t)k * N + jbase + c];
                    svfloat32_t vb = svld1_f32(pg, brow);
                    /* svmla = FMLA fused 乘加(单次舍入), 与 golden 的
                     * fmaf 舍入序一致, 有限值输入下位级一致 */
                    acc = svmla_f32_x(pg, acc,
                              svdup_f32(d->A[(size_t)i * N + k]), vb);
                }
                svst1_f32(pg, C.data() + (size_t)i * N + jbase, acc);
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
            free(brow); /* heap brow: 释放后再 report_fail_msg (noreturn) */
            report_fail_msg("acl_gemm_sve: GEMM output miscompare (SVE svmla path)");
        }
    }
    free(brow);
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
