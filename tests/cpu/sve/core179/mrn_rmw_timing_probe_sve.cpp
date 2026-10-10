/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_rmw_nop_sve
 * @test @b mrn_rmw_nop1_sve
 * @test @b mrn_rmw_nop4_sve
 * @test @b mrn_rmw_nop8_sve
 * @test @b mrn_rmw_nop16_sve
 * @test @b mrn_rmw_nop64_sve
 * @parblock
 * SVE ports of the mrn_rmw timing-probe battery (source:
 * missing_testcases_20261009/tests/cpu/misc/mrn_rmw_timing_probe.cpp).
 *
 * Same-address forwarding skeleton as mrn_rmw_fwd_sve, with N data-
 * independent ALU no-ops (mov x9, x9 via .rept) inserted between the
 * svst1(temp) and the reload, widening the str->ldr time gap:
 *
 *   nop1=1 / nop(2)=6 / nop4≈1 historical fail counts on the scalar
 *   path — the series bisects the width of the str->ldr race window on
 *   the forwarding path; nop8/16/64 walk the descent toward the 0-fail
 *   plateau (nop64 = same-address cell of the 2x2 timing matrix; the
 *   different-address cell mrn_rmw_diff_nop64 does not exist in the
 *   transfer package).
 *
 * The no-ops are fused into the SAME inline-asm block as the ldr z (the
 * scalar original's structure) so the compiler can neither remove them
 * nor move the reload across them. mov x9, x9 preserves x9's value, so
 * the undeclared clobber of a hard register is harmless (original's
 * argument, verified there by objdump).
 *
 * Deviation note (documented): the gap series was cycle-calibrated on
 * the scalar pipeline of the original machine; on SVE vector lanes the
 * absolute cycle distance differs — the DESIGN (monotone gap series on
 * the forwarding path, store buffer undrained) is what ports. Six
 * distinct run functions because .rept needs a compile-time count
 * (mirroring the scalar original's six clones).
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <sys/auxv.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif
#endif

#define MRN_RMW_NOP_SVE_COUNT 1024

struct MrnRmwNopSveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *expected;
};

static int mrn_rmw_nop_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for mrn_rmw_nop_sve");
        return EXIT_SKIP;
    }
#endif
    auto *data = static_cast<MrnRmwNopSveData *>(malloc(sizeof(MrnRmwNopSveData)));
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_NOP_SVE_COUNT * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_NOP_SVE_COUNT * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_NOP_SVE_COUNT * sizeof(uint64_t)));

    memset_random(data->srcA, MRN_RMW_NOP_SVE_COUNT * sizeof(uint64_t));
    memset_random(data->srcB, MRN_RMW_NOP_SVE_COUNT * sizeof(uint64_t));

    for (int i = 0; i < MRN_RMW_NOP_SVE_COUNT; i++) {
        switch (i % 4) {
            case 0: data->expected[i] = data->srcA[i] + data->srcB[i]; break;
            case 1: data->expected[i] = data->srcA[i] - data->srcB[i]; break;
            case 2: data->expected[i] = data->srcA[i] ^ data->srcB[i]; break;
            case 3: data->expected[i] = data->srcA[i] & data->srcB[i]; break;
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int mrn_rmw_nop_sve_cleanup(struct test *test)
{
    auto *data = static_cast<MrnRmwNopSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

static void log_miscompare_nop(const char *tag, unsigned i, const char *op,
                               uint64_t golden, uint64_t actual)
{
    log_error("%s: miscompare at index %u (op=%s): "
              "golden=0x%016lX actual=0x%016lX xor=0x%016lX",
              tag, i, op, (unsigned long)golden, (unsigned long)actual,
              (unsigned long)(golden ^ actual));
    report_fail_msg("%s: data miscompare at index %u (op=%s)", tag, i, op);
}

#if defined(__aarch64__)

/* N 个数据无关 ALU no-op 与 ldr z 融合于同一 asm 块 (原版结构):
 * 编译器既不能删 nop 也不能把 reload 移过它们 */
static inline svuint64_t load_delayed1_sve(const void *addr)
{
    svuint64_t res;
    __asm__ volatile(".rept 1\n\tmov x9, x9\n\t.endr\n\tldr %0, [%1]"
                     : "=w"(res) : "r"(addr) : "memory");
    return res;
}

static inline svuint64_t load_delayed2_sve(const void *addr)
{
    svuint64_t res;
    __asm__ volatile(".rept 2\n\tmov x9, x9\n\t.endr\n\tldr %0, [%1]"
                     : "=w"(res) : "r"(addr) : "memory");
    return res;
}

static inline svuint64_t load_delayed4_sve(const void *addr)
{
    svuint64_t res;
    __asm__ volatile(".rept 4\n\tmov x9, x9\n\t.endr\n\tldr %0, [%1]"
                     : "=w"(res) : "r"(addr) : "memory");
    return res;
}

static inline svuint64_t load_delayed8_sve(const void *addr)
{
    svuint64_t res;
    __asm__ volatile(".rept 8\n\tmov x9, x9\n\t.endr\n\tldr %0, [%1]"
                     : "=w"(res) : "r"(addr) : "memory");
    return res;
}

static inline svuint64_t load_delayed16_sve(const void *addr)
{
    svuint64_t res;
    __asm__ volatile(".rept 16\n\tmov x9, x9\n\t.endr\n\tldr %0, [%1]"
                     : "=w"(res) : "r"(addr) : "memory");
    return res;
}

static inline svuint64_t load_delayed64_sve(const void *addr)
{
    svuint64_t res;
    __asm__ volatile(".rept 64\n\tmov x9, x9\n\t.endr\n\tldr %0, [%1]"
                     : "=w"(res) : "r"(addr) : "memory");
    return res;
}

/* 共享主体: 延迟加载函数与日志 tag 以模板参数注入 (编译期定死, 热循环
 * 无 regime 分支; .rept 计数是编译期常量, 六个实例化各自成体) */
template<svuint64_t (*delayed_load)(const void *), const char *tag>
static int mrn_rmw_nop_sve_run_impl(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MrnRmwNopSveData *>(test->data);
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_NOP_SVE_COUNT * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_NOP_SVE_COUNT * sizeof(uint64_t)));
    const int lanes = svcntd();

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < MRN_RMW_NOP_SVE_COUNT; base += lanes) {
            int n = (MRN_RMW_NOP_SVE_COUNT - base < (size_t)lanes)
                        ? (int)(MRN_RMW_NOP_SVE_COUNT - base) : lanes;
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)n);

            svuint64_t va = svld1_u64(pg, data->srcA + base);
            svuint64_t vb = svld1_u64(pg, data->srcB + base);
            svuint64_t r_add = svadd_u64_x(pg, va, vb);
            svuint64_t r_sub = svsub_u64_x(pg, va, vb);
            svuint64_t r_eor = sveor_u64_x(pg, va, vb);
            svuint64_t r_and = svand_u64_x(pg, va, vb);
            uint64_t phase[8];
            for (int k = 0; k < n; ++k) phase[k] = (base + k) % 4;
            svuint64_t vphase = svld1_u64(pg, phase);
            svuint64_t res = svsel_u64(svcmpeq_n_u64(pg, vphase, 0), r_add,
                             svsel_u64(svcmpeq_n_u64(pg, vphase, 1), r_sub,
                             svsel_u64(svcmpeq_n_u64(pg, vphase, 2), r_eor, r_and)));

            /* svst1(temp) -> N no-op -> ldr z(temp): 转发路径保留, 时间窗加宽 */
            svst1_u64(pg, temp + base, res);
            svst1_u64(pg, dst + base, delayed_load(temp + base));
        }

        if (memcmp(dst, data->expected, MRN_RMW_NOP_SVE_COUNT * sizeof(uint64_t)) != 0) {
            for (int i = 0; i < MRN_RMW_NOP_SVE_COUNT; i++) {
                if (dst[i] != data->expected[i]) {
                    const char *op = (i % 4 == 0) ? "add"
                                   : (i % 4 == 1) ? "sub"
                                   : (i % 4 == 2) ? "xor" : "and";
                    log_miscompare_nop(tag, (unsigned)i, op,
                                       data->expected[i], dst[i]);
                }
            }
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static const char tag_nop[]  = "mrn_rmw_nop_sve";
static const char tag_nop1[] = "mrn_rmw_nop1_sve";
static const char tag_nop4[] = "mrn_rmw_nop4_sve";
static const char tag_nop8[] = "mrn_rmw_nop8_sve";
static const char tag_nop16[] = "mrn_rmw_nop16_sve";
static const char tag_nop64[] = "mrn_rmw_nop64_sve";

static int mrn_rmw_nop_sve_run(struct test *test, int cpu)
{
    return mrn_rmw_nop_sve_run_impl<load_delayed2_sve, tag_nop>(test, cpu);
}

static int mrn_rmw_nop1_sve_run(struct test *test, int cpu)
{
    return mrn_rmw_nop_sve_run_impl<load_delayed1_sve, tag_nop1>(test, cpu);
}

static int mrn_rmw_nop4_sve_run(struct test *test, int cpu)
{
    return mrn_rmw_nop_sve_run_impl<load_delayed4_sve, tag_nop4>(test, cpu);
}

static int mrn_rmw_nop8_sve_run(struct test *test, int cpu)
{
    return mrn_rmw_nop_sve_run_impl<load_delayed8_sve, tag_nop8>(test, cpu);
}

static int mrn_rmw_nop16_sve_run(struct test *test, int cpu)
{
    return mrn_rmw_nop_sve_run_impl<load_delayed16_sve, tag_nop16>(test, cpu);
}

static int mrn_rmw_nop64_sve_run(struct test *test, int cpu)
{
    return mrn_rmw_nop_sve_run_impl<load_delayed64_sve, tag_nop64>(test, cpu);
}

#else

#define NOP_SVE_STUB(name) \
    static int name(struct test *test, int cpu) { \
        (void)test; (void)cpu; \
        log_skip(CpuNotSupportedSkipCategory, \
                 "to be implemented (placeholder): ARM SVE required for " #name); \
        return EXIT_SKIP; \
    }

NOP_SVE_STUB(mrn_rmw_nop_sve_run)
NOP_SVE_STUB(mrn_rmw_nop1_sve_run)
NOP_SVE_STUB(mrn_rmw_nop4_sve_run)
NOP_SVE_STUB(mrn_rmw_nop8_sve_run)
NOP_SVE_STUB(mrn_rmw_nop16_sve_run)
NOP_SVE_STUB(mrn_rmw_nop64_sve_run)

#endif

DECLARE_TEST(mrn_rmw_nop_sve,
    "SVE mrn_rmw timing-only probe: 2 ALU no-ops between svst1 and ldr z (forwarding path kept, "
    "store buffer undrained) — bisects the str->ldr race window (port of mrn_rmw_nop)")
    .test_init = mrn_rmw_nop_sve_init,
    .test_run = mrn_rmw_nop_sve_run,
    .test_cleanup = mrn_rmw_nop_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

DECLARE_TEST(mrn_rmw_nop1_sve,
    "SVE mrn_rmw timing-only probe: 1 ALU no-op between svst1 and ldr z — the ~1-cycle point of "
    "the gap series (port of mrn_rmw_nop1)")
    .test_init = mrn_rmw_nop_sve_init,
    .test_run = mrn_rmw_nop1_sve_run,
    .test_cleanup = mrn_rmw_nop_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

DECLARE_TEST(mrn_rmw_nop4_sve,
    "SVE mrn_rmw timing-only probe: 4 ALU no-ops between svst1 and ldr z — past the non-monotonic "
    "1->2 rise (port of mrn_rmw_nop4)")
    .test_init = mrn_rmw_nop_sve_init,
    .test_run = mrn_rmw_nop4_sve_run,
    .test_cleanup = mrn_rmw_nop_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

DECLARE_TEST(mrn_rmw_nop8_sve,
    "SVE mrn_rmw timing-only probe: 8 ALU no-ops between svst1 and ldr z — descent toward the "
    "0-fail plateau (port of mrn_rmw_nop8)")
    .test_init = mrn_rmw_nop_sve_init,
    .test_run = mrn_rmw_nop8_sve_run,
    .test_cleanup = mrn_rmw_nop_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

DECLARE_TEST(mrn_rmw_nop16_sve,
    "SVE mrn_rmw timing-only probe: 16 ALU no-ops between svst1 and ldr z — residual tail of the "
    "race window (port of mrn_rmw_nop16)")
    .test_init = mrn_rmw_nop_sve_init,
    .test_run = mrn_rmw_nop16_sve_run,
    .test_cleanup = mrn_rmw_nop_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

DECLARE_TEST(mrn_rmw_nop64_sve,
    "SVE mrn_rmw timing-only probe: 64 ALU no-ops between svst1 and ldr z — 0-fail plateau / "
    "same-address cell of the 2x2 timing matrix (port of mrn_rmw_nop64)")
    .test_init = mrn_rmw_nop_sve_init,
    .test_run = mrn_rmw_nop64_sve_run,
    .test_cleanup = mrn_rmw_nop_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
