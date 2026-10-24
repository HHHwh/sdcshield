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
 * Buffers are 16384 bytes each (plus a 256-byte slack on the buffers read
 * by the unpredicated full-VL `ldr z`, so the tail batch at any VL stays
 * in-bounds); the phase is per BYTE index ((base+k)%4), golden precomputed
 * per byte in init. The temp reload is asm-guaranteed (the u8x16 original
 * documents that intrinsic reloads get eliminated by the compiler).
 * @endparblock
 */

#include "sandstone.h"
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)
#include <arm_sve.h>

#ifndef SVE_MAX_VECTOR_BYTES
/* GCC 12 的 arm_sve.h 不提供该 ACLE 宏 (movbe_slf_sweep 同款自定义):
 * SVE 架构最大 VL 2048bit = 256 字节 */
#define SVE_MAX_VECTOR_BYTES 256
#endif
#include <device/cpu/arm64/arm64_vl.h>
#endif

#define NEON_ROT_U8X16_SVE_BYTES 16384   /* bytes per buffer */
/* 满 VL `ldr z` 尾部松弛: SVE 架构最大 VL 2048bit = 256 字节。非 2 幂 VL
 * (如 48B) 下尾批 n<svcntb(), 无谓词的全宽加载会读到缓冲末尾之外; 给被该
 * helper 读取的缓冲(srcA/srcB/temp)各留 256B 松弛, 任意 VL 都不越界
 * (review 4249143905)。 */
#define NEON_ROT_U8X16_SVE_LDR_SLACK 256
#define NEON_ROT_U8X16_SVE_ALLOC (NEON_ROT_U8X16_SVE_BYTES + NEON_ROT_U8X16_SVE_LDR_SLACK)

struct NeonRotU8x16SveData {
    uint8_t *srcA;
    uint8_t *srcB;
    uint8_t *expected;
    size_t buffer_bytes;   /* 运行时每缓冲字节数 (knob `buffer_bytes`, 默认设计值) */
};

#if defined(__aarch64__)

/* 优先级: 1. -O neon_rot_u8x16_sve.buffer_bytes=N
 *   2. SDC_NEON_ROT_U8X16_SVE_BUFFER_BYTES  3. 设计默认 16384
 * （诊断探针: 默认保持设计几何, 显式配置才生效 — 槽距/行距模式是激发参数不动） */
static size_t neon_rot_u8x16_sve_buffer_bytes(struct test *test)
{
    const int64_t dflt = NEON_ROT_U8X16_SVE_BYTES;
    int64_t v = sdc_knob_int_clamped(test, "buffer_bytes", 0, NEON_ROT_U8X16_SVE_BYTES, 256 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("neon_rot_u8x16_sve", "buffer_bytes", &v, NEON_ROT_U8X16_SVE_BYTES, 256 << 20);
    return v > 0 ? (size_t)v : (size_t)dflt;
}

#endif

static int neon_rot_u8x16_sve_init(struct test *test)
{
    size_t buffer_bytes = NEON_ROT_U8X16_SVE_BYTES;
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
    buffer_bytes = neon_rot_u8x16_sve_buffer_bytes(test);
    /* 全 VL ldr/str 访问 + aligned_alloc(64,·) 尺寸约束 (原 16/64 对齐保留):
     * 向下取整到 svcntb() 与 64 的公倍数 (均为 2 的幂, 取大者即公倍数) */
    size_t align = (size_t)svl;   /* VL 字节 = SVL bytes (svcntb 同值) */
    if (align < 64)
        align = 64;
    if (buffer_bytes % align != 0) {
        const size_t rounded = buffer_bytes - buffer_bytes % align;
        log_warning("neon_rot_u8x16_sve: buffer_bytes=%zu 不是 svcntb()=%d 与 64 的公倍数, "
                    "向下取整为 %zu",
                    buffer_bytes, (int)svl, rounded);
        buffer_bytes = rounded;
    }
#endif
    auto *data = static_cast<NeonRotU8x16SveData *>(malloc(sizeof(NeonRotU8x16SveData)));
    data->buffer_bytes = buffer_bytes;
    /* 满 VL 无谓词 ldr z 的尾部松弛 (review 4249143905): 被读取的
     * srcA/srcB/temp 各 +256B, 运行时尺寸 = buffer_bytes + slack */
    data->srcA = static_cast<uint8_t *>(aligned_alloc(64, buffer_bytes + NEON_ROT_U8X16_SVE_LDR_SLACK));
    data->srcB = static_cast<uint8_t *>(aligned_alloc(64, buffer_bytes + NEON_ROT_U8X16_SVE_LDR_SLACK));
    data->expected = static_cast<uint8_t *>(aligned_alloc(64, buffer_bytes));

    /* 填满含松弛的整段, 使尾部全宽加载读到的松弛字节为已初始化值 */
    memset_random(data->srcA, buffer_bytes + NEON_ROT_U8X16_SVE_LDR_SLACK);
    memset_random(data->srcB, buffer_bytes + NEON_ROT_U8X16_SVE_LDR_SLACK);

    /* golden: 逐字节同旋转, 在相同 SVE 单元上预计算 */
    for (size_t j = 0; j < buffer_bytes; j++) {
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

/* 谓词化存储: 尾部 batch 用 pgn 截断, 任意 VL (含非 2 幂) 都不会写越界 */
static inline void store_vec_u8_sve(svbool_t pg, void *addr, svuint8_t val)
{
    svst1_u8(pg, static_cast<uint8_t *>(addr), val);
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

    uint8_t *temp = static_cast<uint8_t *>(aligned_alloc(64, data->buffer_bytes + NEON_ROT_U8X16_SVE_LDR_SLACK));
    uint8_t *dst  = static_cast<uint8_t *>(aligned_alloc(64, data->buffer_bytes));
    const int lanes = svcntb();   /* bytes per vector */
    /* temp 被满 VL ldr z 读取(含尾部松弛区), 先置 0 保证读取字节已初始化 */
    memset(temp, 0, data->buffer_bytes + NEON_ROT_U8X16_SVE_LDR_SLACK);
    /* SVE 架构最大 VL 2048bit → svcntb() ≤ 256 (movbe_slf_sweep
     * VL-portable 范式): n = min(bytes-base, svcntb()) 全 VL 覆盖 */
    uint8_t phase[SVE_MAX_VECTOR_BYTES];

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < data->buffer_bytes; base += lanes) {
            int n = (data->buffer_bytes - base < (size_t)lanes)
                        ? (int)(data->buffer_bytes - base) : lanes;
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
            store_vec_u8_sve(pgn, temp + base, res);
            store_vec_u8_sve(pgn, dst + base, load_vec_u8_sve(temp + base));
        }

        if (memcmp(dst, data->expected, data->buffer_bytes) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < data->buffer_bytes; ++_dump_j) {
                    if (dst[_dump_j] != data->expected[_dump_j]) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("neon_rot_u8x16_sve: mismatch at index %zu: golden=0x%02x actual=0x%02x xor=0x%02x",
                                      _dump_j,
                                      (unsigned)data->expected[_dump_j],
                                      (unsigned)dst[_dump_j],
                                      (unsigned)(data->expected[_dump_j] ^ dst[_dump_j]));
                            ++_dump_shown;
                        }
                    }
                }
                log_error("neon_rot_u8x16_sve: total mismatched elements: %zu / %zu",
                          _dump_bad, data->buffer_bytes);
            }
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
