/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_rmw_fwd_sve
 * @test @b mrn_rmw_l1d_sve
 * @parblock
 * SVE ports of mrn_rmw_fwd / mrn_rmw_l1d (load-pathway subdivision probe
 * for the core-179 SDC; source: missing_testcases_20261009/tests/cpu/
 * misc/mrn_rmw_fwd_vs_l1d.cpp).
 *
 * The original mrn_rmw recipe (str res->temp; dst = ldr temp — same
 * address back-to-back) is run on SVE lanes with the rotating
 * add/sub/xor/and composition per element phase ((base+k)%4) and the
 * phase-predicate dispatch (mrn_rmw_sve idiom). Two SEPARATE tests so
 * neither hot loop carries a regime conditional:
 *
 *   - mrn_rmw_fwd_sve: svst1(temp, res) immediately followed by the
 *     asm-guaranteed ldr z of temp — the store-buffer FORWARDING path.
 *     Expected on a defective core: SDC triggers (mrn_rmw lineage
 *     baseline, ~15 fail/10min on the scalar path).
 *   - mrn_rmw_l1d_sve: same, but every cache line covering the temp
 *     batch is evicted (dc civac + dsb sy) between the svst1 and the
 *     ldr z, forcing the reload through the L1D array / further. If the
 *     SDC disappears here while fwd still triggers, the defect is in
 *     the FORWARDING path; if it persists, the common downstream point
 *     (load port -> register) is implicated.
 *
 * The reload is asm-guaranteed (volatile ldr z + "memory" clobber): it
 * reads the just-stored address, so a plain svld1 could in principle be
 * compile-time-forwarded from the svst1 — the asm boundary keeps the
 * str->ldr pair real, adjacent and in order (u8x16-original discipline).
 *
 * Deviations from the scalar originals (documented):
 *   - the eviction runs once per cache line covering the batch instead
 *     of once per 8-byte element (same end state — all lines flushed
 *     before the reload);
 *   - the rotating ALU runs as 4 vector results + per-lane phase
 *     select (mrn_rmw_sve idiom) instead of a per-element scalar
 *     switch; golden is precomputed in init with the same phase math.
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

#define MRN_RMW_FWD_SVE_COUNT 1024

struct MrnRmwFwdL1dSveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *expected;
};

static int mrn_rmw_fwd_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for mrn_rmw_fwd_sve");
        return EXIT_SKIP;
    }
#endif
    auto *data = static_cast<MrnRmwFwdL1dSveData *>(malloc(sizeof(MrnRmwFwdL1dSveData)));
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_FWD_SVE_COUNT * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_FWD_SVE_COUNT * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_FWD_SVE_COUNT * sizeof(uint64_t)));

    memset_random(data->srcA, MRN_RMW_FWD_SVE_COUNT * sizeof(uint64_t));
    memset_random(data->srcB, MRN_RMW_FWD_SVE_COUNT * sizeof(uint64_t));

    for (int i = 0; i < MRN_RMW_FWD_SVE_COUNT; i++) {
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

static int mrn_rmw_fwd_sve_cleanup(struct test *test)
{
    auto *data = static_cast<MrnRmwFwdL1dSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

/* Shared miscompare logger (cold branch only) — same field set as the
 * scalar originals (inputA/inputB/golden/actual/xor + byte dump). */
static void log_miscompare_u64(const char *tag, unsigned i, const char *op,
                               uint64_t inputA, uint64_t inputB,
                               uint64_t golden, uint64_t actual)
{
    uint64_t xorv = golden ^ actual;
    unsigned char *gb = (unsigned char *)&golden;
    unsigned char *ab = (unsigned char *)&actual;
    unsigned char *xb = (unsigned char *)&xorv;
    log_error("%s: miscompare at index %u (op=%s): "
              "inputA=0x%016lX inputB=0x%016lX golden=0x%016lX "
              "actual=0x%016lX xor=0x%016lX | "
              "bytes[b7..b0] golden=%02X%02X%02X%02X%02X%02X%02X%02X "
              "actual=%02X%02X%02X%02X%02X%02X%02X%02X "
              "xor=%02X%02X%02X%02X%02X%02X%02X%02X",
              tag, i, op,
              (unsigned long)inputA, (unsigned long)inputB,
              (unsigned long)golden, (unsigned long)actual,
              (unsigned long)xorv,
              gb[7], gb[6], gb[5], gb[4], gb[3], gb[2], gb[1], gb[0],
              ab[7], ab[6], ab[5], ab[4], ab[3], ab[2], ab[1], ab[0],
              xb[7], xb[6], xb[5], xb[4], xb[3], xb[2], xb[1], xb[0]);
    report_fail_msg("%s: data miscompare at index %u (op=%s)", tag, i, op);
}

#if defined(__aarch64__)

/* asm 保证的被测 reload (str->ldr 同地址配对的 ldr 侧) */
static inline svuint64_t load_vec_sve(const void *addr)
{
    svuint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

/* 每行一次 dc civac + dsb sy (原版逐 8B 元素重复冲刷同一行 — 终态等价) */
static inline void evict_l1d_range_sve(const void *addr, size_t bytes)
{
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)63;
    uintptr_t end = (uintptr_t)addr + bytes;
    for (uintptr_t a = start; a < end; a += 64) {
        __asm__ volatile("dc civac, %0\n\tdsb sy\n\t" :: "r"(a) : "memory");
    }
}

template<int evict> static int mrn_rmw_fwd_sve_run_impl(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MrnRmwFwdL1dSveData *>(test->data);
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_FWD_SVE_COUNT * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_FWD_SVE_COUNT * sizeof(uint64_t)));
    const int lanes = svcntd();

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < MRN_RMW_FWD_SVE_COUNT; base += lanes) {
            int n = (MRN_RMW_FWD_SVE_COUNT - base < (size_t)lanes)
                        ? (int)(MRN_RMW_FWD_SVE_COUNT - base) : lanes;
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

            /* RMW: 向量 store 到 temp, 立即 ldr z 回读 (转发路径) */
            svst1_u64(pg, temp + base, res);

            if (evict)
                evict_l1d_range_sve(temp + base, (size_t)n * 8);

            svst1_u64(pg, dst + base, load_vec_sve(temp + base));
        }

        if (memcmp(dst, data->expected, MRN_RMW_FWD_SVE_COUNT * sizeof(uint64_t)) != 0) {
            for (int i = 0; i < MRN_RMW_FWD_SVE_COUNT; i++) {
                if (dst[i] != data->expected[i]) {
                    const char *op = (i % 4 == 0) ? "add"
                                   : (i % 4 == 1) ? "sub"
                                   : (i % 4 == 2) ? "xor" : "and";
                    log_miscompare_u64(evict ? "mrn_rmw_l1d_sve" : "mrn_rmw_fwd_sve",
                                       (unsigned)i, op,
                                       data->srcA[i], data->srcB[i],
                                       data->expected[i], dst[i]);
                }
            }
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int mrn_rmw_fwd_sve_run(struct test *test, int cpu)
{
    return mrn_rmw_fwd_sve_run_impl<0>(test, cpu);
}

static int mrn_rmw_l1d_sve_run(struct test *test, int cpu)
{
    return mrn_rmw_fwd_sve_run_impl<1>(test, cpu);
}

#else

static int mrn_rmw_fwd_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for mrn_rmw_fwd_sve");
    return EXIT_SKIP;
}

static int mrn_rmw_l1d_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for mrn_rmw_l1d_sve");
    return EXIT_SKIP;
}

#endif

DECLARE_TEST(mrn_rmw_fwd_sve,
             "SVE mrn_rmw load-path probe (store-buffer FORWARDING): svst1 then asm ldr z same "
             "addr on vector lanes (port of mrn_rmw_fwd)")
    .test_init = mrn_rmw_fwd_sve_init,
    .test_run = mrn_rmw_fwd_sve_run,
    .test_cleanup = mrn_rmw_fwd_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

DECLARE_TEST(mrn_rmw_l1d_sve,
             "SVE mrn_rmw load-path probe (L1D-array read): evict temp lines (dc civac + dsb) "
             "between svst1 and ldr z — distinguishes forwarding vs L1D array (port of mrn_rmw_l1d)")
    .test_init = mrn_rmw_fwd_sve_init,
    .test_run = mrn_rmw_l1d_sve_run,
    .test_cleanup = mrn_rmw_fwd_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
