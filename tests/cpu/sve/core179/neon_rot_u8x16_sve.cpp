/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_u8x16_sve
 * @parblock
 * SVE port of neon_rot_u8x16 (source: missing_testcases_20261009/
 * tests/cpu/misc/neon_rot_u8x16.cpp).
 *
 * Lane-structure probe: the neon_rot_2src skeleton (2-source vector
 * load + rotating {add, eor, and, orr} + store temp + store
 * dst(reload temp)) with BYTE lanes instead of 2x64-bit lanes. The
 * scalar NEON original used 16 byte lanes per 16B vector to
 * discriminate width-vs-lane hypotheses for the trigger amplification
 * (neon_rot_2src ~33 fail/30min = 1.7x scalar); the SVE port carries
 * the same question onto full-VL byte lanes (svcntb() bytes per
 * vector, 32 at VL=256) — rate holds => op width matters, collapses =>
 * the 2x64b lane structure matters.
 *
 * Buffers are 16384 bytes each; the phase is per BYTE index
 * ((base+k)%4), golden precomputed per byte in init. The temp reload is
 * asm-guaranteed (the u8x16 original documents that intrinsic reloads
 * get eliminated by the compiler).
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

#define NEON_ROT_U8X16_SVE_BYTES 16384   /* bytes per buffer */

struct NeonRotU8x16SveData {
    uint8_t *srcA;
    uint8_t *srcB;
    uint8_t *expected;
};

static int neon_rot_u8x16_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for neon_rot_u8x16_sve");
        return EXIT_SKIP;
    }
#endif
    auto *data = static_cast<NeonRotU8x16SveData *>(malloc(sizeof(NeonRotU8x16SveData)));
    data->srcA = static_cast<uint8_t *>(aligned_alloc(64, NEON_ROT_U8X16_SVE_BYTES));
    data->srcB = static_cast<uint8_t *>(aligned_alloc(64, NEON_ROT_U8X16_SVE_BYTES));
    data->expected = static_cast<uint8_t *>(aligned_alloc(64, NEON_ROT_U8X16_SVE_BYTES));

    memset_random(data->srcA, NEON_ROT_U8X16_SVE_BYTES);
    memset_random(data->srcB, NEON_ROT_U8X16_SVE_BYTES);

    /* golden: 逐字节同旋转, 在相同 SVE 单元上预计算 */
    for (int j = 0; j < NEON_ROT_U8X16_SVE_BYTES; j++) {
        switch (j % 4) {
            case 0: data->expected[j] = data->srcA[j] + data->srcB[j]; break;
            case 1: data->expected[j] = data->srcA[j] ^ data->srcB[j]; break;
            case 2: data->expected[j] = data->srcA[j] & data->srcB[j]; break;
            case 3: data->expected[j] = data->srcA[j] | data->srcB[j]; break;
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)

static inline void store_vec_u8_sve(void *addr, svuint8_t val)
{
    svst1_u8(svptrue_b8(), static_cast<uint8_t *>(addr), val);
}

static inline svuint8_t load_vec_u8_sve(const void *addr)
{
    svuint8_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

static int neon_rot_u8x16_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<NeonRotU8x16SveData *>(test->data);

    uint8_t *temp = static_cast<uint8_t *>(aligned_alloc(64, NEON_ROT_U8X16_SVE_BYTES));
    uint8_t *dst  = static_cast<uint8_t *>(aligned_alloc(64, NEON_ROT_U8X16_SVE_BYTES));
    const int lanes = svcntb();   /* bytes per vector */
    uint8_t phase[64];

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < NEON_ROT_U8X16_SVE_BYTES; base += lanes) {
            int n = (NEON_ROT_U8X16_SVE_BYTES - base < (size_t)lanes)
                        ? (int)(NEON_ROT_U8X16_SVE_BYTES - base) : lanes;
            svbool_t pgn = svwhilelt_b8((uint64_t)0, (uint64_t)n);

            svuint8_t a = load_vec_u8_sve(data->srcA + base);
            svuint8_t b = load_vec_u8_sve(data->srcB + base);
            svuint8_t r_add = svadd_u8_x(pgn, a, b);
            svuint8_t r_eor = sveor_u8_x(pgn, a, b);
            svuint8_t r_and = svand_u8_x(pgn, a, b);
            svuint8_t r_orr = svorr_u8_x(pgn, a, b);
            for (int k = 0; k < n; ++k) phase[k] = (uint8_t)((base + k) % 4);
            svuint8_t vphase = svld1_u8(pgn, phase);
            svuint8_t res = svsel_u8(svcmpeq_n_u8(pgn, vphase, 0), r_add,
                            svsel_u8(svcmpeq_n_u8(pgn, vphase, 1), r_eor,
                            svsel_u8(svcmpeq_n_u8(pgn, vphase, 2), r_and, r_orr)));

            /* 骨架: str temp -> ldr z temp (asm 保证) -> str dst */
            store_vec_u8_sve(temp + base, res);
            store_vec_u8_sve(dst + base, load_vec_u8_sve(temp + base));
        }

        if (memcmp(dst, data->expected, NEON_ROT_U8X16_SVE_BYTES) != 0) {
            report_fail_msg("neon_rot_u8x16_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

#else

static int neon_rot_u8x16_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_u8x16_sve");
    return EXIT_SKIP;
}

#endif

static int neon_rot_u8x16_sve_cleanup(struct test *test)
{
    auto *data = static_cast<NeonRotU8x16SveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_u8x16_sve,
             "SVE variant: full-VL byte lanes (svcntb() per vector) 2-src ldr z + rotating ALU + "
             "store/reload — the width-vs-lane probe on SVE lanes (port of neon_rot_u8x16)")
    .test_init = neon_rot_u8x16_sve_init,
    .test_run = neon_rot_u8x16_sve_run,
    .test_cleanup = neon_rot_u8x16_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
