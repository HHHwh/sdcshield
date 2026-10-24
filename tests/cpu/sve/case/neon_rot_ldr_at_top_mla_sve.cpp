/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_at_top_mla_sve
 * @parblock
 * SVE port of neon_rot_ldr_at_top_mla (source:
 * missing_testcases_20261009/tests/cpu/misc/neon_rot_ldr_at_top_mla.cpp).
 *
 * Algebraic variant of the position probe (original-parent lineage,
 * forward-only sweep): the rotation runs in 32-bit lanes —
 * mul / mla-style (a + a*b) / mls-style (b - a*b) / eor (control) —
 * probing the MAC pipe + accumulate chains instead of 2-input logic/add
 * ops. Sources are narrowed to 15-bit magnitude (& 0x7FFF) and
 * PERSISTED back to the source buffers in init so run-side recomputation
 * sees identical inputs and s32 products never overflow (original
 * semantics). Same slot geometry otherwise: dead 3rd ldr z (scratch) at
 * top + 2 source loads + back-to-back store pair, 1024 slots of
 * svcntw() u32 lanes.
 *
 * Instruction note: the original composes mla/mls explicitly as
 * add(mul)/sub(mul) (NEON has no u32 mla); the SVE port composes the
 * same way (svadd(svmul)/svsub(svmul)) so the instruction structure
 * matches the original datapath.
 * @endparblock
 */

#include "sandstone.h"
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

#define NEON_ROT_LDR_AT_TOP_MLA_SVE_SLOTS 1024

struct NeonRotLdrAtTopMlaSveData {
    uint32_t *srcA;
    uint32_t *srcB;
    uint32_t *scratch;
    uint32_t *expected;
    int lanes;
    int slots;   /* 运行时向量槽数 (knob `slots`, 默认设计值) */
};

#if defined(__aarch64__)

static inline void store_vec_sve(void *addr, svuint32_t val) {
    svst1_u32(svptrue_b32(), static_cast<uint32_t *>(addr), val);
}

static inline svuint32_t load_vec_sve(const void *addr) {
    svuint32_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

/* 优先级: 1. -O neon_rot_ldr_at_top_mla_sve.slots=N
 *   2. SDC_NEON_ROT_LDR_AT_TOP_MLA_SVE_SLOTS  3. 设计默认 1024
 * （诊断探针: 默认保持设计几何, 显式配置才生效 — 槽距/行距模式是激发参数不动） */
static size_t neon_rot_ldr_at_top_mla_sve_slots(struct test *test)
{
    const int64_t dflt = NEON_ROT_LDR_AT_TOP_MLA_SVE_SLOTS;
    int64_t v = sdc_knob_int_clamped(test, "slots", 0, NEON_ROT_LDR_AT_TOP_MLA_SVE_SLOTS, 1 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("neon_rot_ldr_at_top_mla_sve", "slots", &v, NEON_ROT_LDR_AT_TOP_MLA_SVE_SLOTS, 1 << 20);
    return v > 0 ? (size_t)v : (size_t)dflt;
}

#endif

static int neon_rot_ldr_at_top_mla_sve_init(struct test *test) {
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    const int lanes = (int)(svl / 4);   /* f32 lanes = SVL bytes / 4 */
    if (lanes < 4 || lanes > 32) {
        log_error("%s: f32 lanes %d outside [4,32] — unit drift (svl %ld bytes)",
                  test->id, lanes, svl);
        return EXIT_FAILURE;
    }
    const size_t slots = neon_rot_ldr_at_top_mla_sve_slots(test);
    const size_t elems = slots * (size_t)lanes;

    auto *data = static_cast<NeonRotLdrAtTopMlaSveData *>(malloc(sizeof(NeonRotLdrAtTopMlaSveData)));
    data->lanes = lanes;
    data->slots = (int)slots;
    data->srcA = static_cast<uint32_t *>(aligned_alloc(64, elems * sizeof(uint32_t)));
    data->srcB = static_cast<uint32_t *>(aligned_alloc(64, elems * sizeof(uint32_t)));
    data->scratch = static_cast<uint32_t *>(aligned_alloc(64, elems * sizeof(uint32_t)));
    data->expected = static_cast<uint32_t *>(aligned_alloc(64, elems * sizeof(uint32_t)));

    memset_random(data->srcA, elems * sizeof(uint32_t));
    memset_random(data->srcB, elems * sizeof(uint32_t));
    memset_random(data->scratch, elems * sizeof(uint32_t));

    /* 收窄持久化 + golden: mul / mla式(a+a*b) / mls式(b-a*b) / eor, 同 SVE 单元 */
    svbool_t pg = svptrue_b32();
    svuint32_t vmask = svdup_u32(0x00007fffu);
    for (int i = 0; i < data->slots; i++) {
        const size_t off = (size_t)i * lanes;
        svuint32_t a = svand_u32_x(pg, svld1_u32(pg, data->srcA + off), vmask);
        svuint32_t b = svand_u32_x(pg, svld1_u32(pg, data->srcB + off), vmask);
        svst1_u32(pg, data->srcA + off, a);   /* persist narrowed sources */
        svst1_u32(pg, data->srcB + off, b);
        svint32_t sa = svreinterpret_s32_u32(a);
        svint32_t sb = svreinterpret_s32_u32(b);
        svuint32_t res = svundef_u32();
        switch (i % 4) {
            case 0: res = svreinterpret_u32_s32(svmul_s32_x(pg, sa, sb)); break;
            case 1: res = svreinterpret_u32_s32(svadd_s32_x(pg, svmul_s32_x(pg, sa, sb), sa)); break;
            case 2: res = svreinterpret_u32_s32(svsub_s32_x(pg, sb, svmul_s32_x(pg, sa, sb))); break;
            case 3: res = sveor_u32_x(pg, a, b); break;
        }
        svst1_u32(pg, data->expected + off, res);
    }

    test->data = data;
    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_mla_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)
static int neon_rot_ldr_at_top_mla_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<NeonRotLdrAtTopMlaSveData *>(test->data);

    if ((int)svcntw() != data->lanes) {
        log_error("neon_rot_ldr_at_top_mla_sve: vector length changed between init (%d) and run (%d)",
                  data->lanes, (int)svcntw());
        report_fail_msg("neon_rot_ldr_at_top_mla_sve: vector length changed between init and run");
    }

    const size_t elems = (size_t)data->slots * data->lanes;
    uint32_t *temp = static_cast<uint32_t *>(aligned_alloc(64, elems * sizeof(uint32_t)));
    uint32_t *dst  = static_cast<uint32_t *>(aligned_alloc(64, elems * sizeof(uint32_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < data->slots; i++) {
            const size_t off = (size_t)i * data->lanes;

            svuint32_t unused_x = load_vec_sve(data->scratch + off);
            (void)unused_x;

            svuint32_t a = load_vec_sve(data->srcA + off);
            svuint32_t b = load_vec_sve(data->srcB + off);
            svint32_t sa = svreinterpret_s32_u32(a);
            svint32_t sb = svreinterpret_s32_u32(b);
            svuint32_t res = svundef_u32();

            switch (i % 4) {
                case 0: res = svreinterpret_u32_s32(svmul_s32_x(svptrue_b32(), sa, sb)); break;
                case 1: res = svreinterpret_u32_s32(svadd_s32_x(svptrue_b32(), svmul_s32_x(svptrue_b32(), sa, sb), sa)); break;
                case 2: res = svreinterpret_u32_s32(svsub_s32_x(svptrue_b32(), sb, svmul_s32_x(svptrue_b32(), sa, sb))); break;
                case 3: res = sveor_u32_x(svptrue_b32(), a, b); break;
            }

            store_vec_sve(temp + off, res);
            store_vec_sve(dst + off, res);
        }

        if (memcmp(dst, data->expected, elems * sizeof(uint32_t)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < elems; ++_dump_j) {
                    if (dst[_dump_j] != data->expected[_dump_j]) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("neon_rot_ldr_at_top_mla_sve: mismatch at flat=%zu slot=%zu lane=%zu: golden=0x%08x actual=0x%08x xor=0x%08x",
                                      _dump_j, _dump_j / (size_t)data->lanes, _dump_j % (size_t)data->lanes,
                                      (unsigned)data->expected[_dump_j],
                                      (unsigned)dst[_dump_j],
                                      (unsigned)(data->expected[_dump_j] ^ dst[_dump_j]));
                            ++_dump_shown;
                        }
                    }
                }
                log_error("neon_rot_ldr_at_top_mla_sve: total mismatched elements: %zu / %zu (slots x lanes)",
                          _dump_bad, elems);
            }
            report_fail_msg("neon_rot_ldr_at_top_mla_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int neon_rot_ldr_at_top_mla_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_mla_sve");
    return EXIT_SKIP;
}
#endif

static int neon_rot_ldr_at_top_mla_sve_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotLdrAtTopMlaSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_at_top_mla_sve,
             "SVE algebraic variant of the position probe: rotation mul/mla-style/mls-style/eor "
             "in 32-bit lanes (MAC pipe + accumulate chain signature test) at forward-only sweep "
             "with 3rd ldr z at top (port of neon_rot_ldr_at_top_mla)")
    .test_init    = neon_rot_ldr_at_top_mla_sve_init,
    .test_run     = neon_rot_ldr_at_top_mla_sve_run,
    .test_cleanup = neon_rot_ldr_at_top_mla_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
