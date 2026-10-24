/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_at_top_str3_sve
 * @parblock
 * SVE port of neon_rot_ldr_at_top_str3 (source:
 * missing_testcases_20261009/tests/cpu/misc/neon_rot_ldr_at_top_str3.cpp).
 *
 * Store-count variant of the rowmajor position probe: THREE stores per
 * slot (temp, junk, dst — in that order, all writing the same res; junk
 * never read) at the constant 3-load (dead scratch at top + 2 sources)
 * rotating-ALU skeleton. Store-side gradient: parent 2 str / str3 3 str /
 * str4 4 str — a 0-fail here is ambiguous between "store pressure kills
 * the trigger" and "instruction-count/layout shift kills it"; the
 * gradient SHAPE is the signal (pre-registered in the scalar original).
 *
 * Blind spot kept from the original (by design): only dst is verified —
 * the neon_rot_ldr_at_top_str3_checkall sibling closes it.
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

#define NEON_ROT_LDR_AT_TOP_STR3_SVE_SLOTS 1024

struct NeonRotLdrAtTopStr3SveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *scratch;
    uint64_t *expected;
    int lanes;
    int slots;   /* 运行时向量槽数 (knob `slots`, 默认设计值) */
};

#if defined(__aarch64__)

static inline void store_vec_sve(void *addr, svuint64_t val) {
    svst1_u64(svptrue_b64(), static_cast<uint64_t *>(addr), val);
}

static inline svuint64_t load_vec_sve(const void *addr) {
    svuint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

/* 优先级: 1. -O neon_rot_ldr_at_top_str3_sve.slots=N
 *   2. SDC_NEON_ROT_LDR_AT_TOP_STR3_SVE_SLOTS  3. 设计默认 1024
 * （诊断探针: 默认保持设计几何, 显式配置才生效 — 槽距/行距模式是激发参数不动） */
static size_t neon_rot_ldr_at_top_str3_sve_slots(struct test *test)
{
    const int64_t dflt = NEON_ROT_LDR_AT_TOP_STR3_SVE_SLOTS;
    int64_t v = sdc_knob_int_clamped(test, "slots", 0, NEON_ROT_LDR_AT_TOP_STR3_SVE_SLOTS, 1 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("neon_rot_ldr_at_top_str3_sve", "slots", &v, NEON_ROT_LDR_AT_TOP_STR3_SVE_SLOTS, 1 << 20);
    return v > 0 ? (size_t)v : (size_t)dflt;
}

#endif

static int neon_rot_ldr_at_top_str3_sve_init(struct test *test) {
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    const int lanes = (int)(svl / 8);   /* f64 lanes = SVL bytes / 8 */
    if (lanes < 2 || lanes > 16) {
        log_error("%s: f64 lanes %d outside [2,16] — unit drift (svl %ld bytes)",
                  test->id, lanes, svl);
        return EXIT_FAILURE;
    }
    const size_t slots = neon_rot_ldr_at_top_str3_sve_slots(test);
    const size_t elems = slots * (size_t)lanes;

    auto *data = static_cast<NeonRotLdrAtTopStr3SveData *>(malloc(sizeof(NeonRotLdrAtTopStr3SveData)));
    data->lanes = lanes;
    data->slots = (int)slots;
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->scratch = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    memset_random(data->srcA, elems * sizeof(uint64_t));
    memset_random(data->srcB, elems * sizeof(uint64_t));
    memset_random(data->scratch, elems * sizeof(uint64_t));

    svbool_t pg = svptrue_b64();
    for (int i = 0; i < data->slots; i++) {
        svuint64_t a = svld1_u64(pg, data->srcA + (size_t)i * lanes);
        svuint64_t b = svld1_u64(pg, data->srcB + (size_t)i * lanes);
        svuint64_t res = svundef_u64();
        switch (i % 4) {
            case 0: res = svadd_u64_x(pg, a, b); break;
            case 1: res = sveor_u64_x(pg, a, b); break;
            case 2: res = svand_u64_x(pg, a, b); break;
            case 3: res = svorr_u64_x(pg, a, b); break;
        }
        svst1_u64(pg, data->expected + (size_t)i * lanes, res);
    }

    test->data = data;
    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_str3_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)
static int neon_rot_ldr_at_top_str3_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<NeonRotLdrAtTopStr3SveData *>(test->data);

    if ((int)svcntd() != data->lanes) {
        log_error("neon_rot_ldr_at_top_str3_sve: vector length changed between init (%d) and run (%d)",
                  data->lanes, (int)svcntd());
        report_fail_msg("neon_rot_ldr_at_top_str3_sve: vector length changed between init and run");
    }

    const size_t elems = (size_t)data->slots * data->lanes;
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    uint64_t *junk = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 13) {
        const bool ascending = (sweep & 1) == 0;
        for (int n = 0; n < data->slots; n++) {
            const int i = ascending ? n : (data->slots - 1 - n);
            const size_t off = (size_t)i * data->lanes;

            svuint64_t unused_x = load_vec_sve(data->scratch + off);
            (void)unused_x;

            svuint64_t a = load_vec_sve(data->srcA + off);
            svuint64_t b = load_vec_sve(data->srcB + off);
            svuint64_t res = svundef_u64();

            switch (i % 4) {
                case 0: res = svadd_u64_x(svptrue_b64(), a, b); break;
                case 1: res = sveor_u64_x(svptrue_b64(), a, b); break;
                case 2: res = svand_u64_x(svptrue_b64(), a, b); break;
                case 3: res = svorr_u64_x(svptrue_b64(), a, b); break;
            }

            /* 三存同值: temp -> junk -> dst (junk 只写不读, 原版顺序) */
            store_vec_sve(temp + off, res);
            store_vec_sve(junk + off, res);
            store_vec_sve(dst + off, res);
        }
        sweep++;

        /* 原版盲区保留: 仅校验 dst (str3_checkall 才补全三槽) */
        if (memcmp(dst, data->expected, elems * sizeof(uint64_t)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < elems; ++_dump_j) {
                    if (dst[_dump_j] != data->expected[_dump_j]) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("neon_rot_ldr_at_top_str3_sve: mismatch at flat=%zu slot=%zu lane=%zu: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                                      _dump_j, _dump_j / (size_t)data->lanes, _dump_j % (size_t)data->lanes,
                                      (unsigned long long)data->expected[_dump_j],
                                      (unsigned long long)dst[_dump_j],
                                      (unsigned long long)(data->expected[_dump_j] ^ dst[_dump_j]));
                            ++_dump_shown;
                        }
                    }
                }
                log_error("neon_rot_ldr_at_top_str3_sve: total mismatched elements: %zu / %zu (slots x lanes)",
                          _dump_bad, elems);
            }
            report_fail_msg("neon_rot_ldr_at_top_str3_sve data miscompare");
        }
    }

    free(dst);
    free(junk);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int neon_rot_ldr_at_top_str3_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_str3_sve");
    return EXIT_SKIP;
}
#endif

static int neon_rot_ldr_at_top_str3_sve_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotLdrAtTopStr3SveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_at_top_str3_sve,
             "SVE store-count variant of the rowmajor position probe: 3 str per slot (2 useful + "
             "1 dead junk str) at the constant 3-ldr/rot-ALU skeleton; alternating sweep order "
             "(port of neon_rot_ldr_at_top_str3)")
    .test_init    = neon_rot_ldr_at_top_str3_sve_init,
    .test_run     = neon_rot_ldr_at_top_str3_sve_run,
    .test_cleanup = neon_rot_ldr_at_top_str3_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
