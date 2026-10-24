/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_at_top_rowmajor_sve
 * @parblock
 * SVE port of neon_rot_ldr_at_top_rowmajor (microarchitecture-dimension
 * variant of the position probe, 2026-09-08): the 55-record dump campaign
 * showed corruption concentrated on the FIRST indices of the inner loop
 * (idx 0 + idx 1 = 36/55 records) while the buffer spans 1024 entries.
 * This variant walks the 1024 vector slots in a REVISED order — even
 * outer iterations sweep ascending, odd outer iterations descending — so
 * the touch pattern at idx 0/1 differs (on a descending pass, index 0 is
 * the LAST slot touched, with a fully-warm pipeline behind it, and the
 * store->load adjacency across the loop seam changes).
 *   strong idx-0/1 signal also here -> the signature follows the buffer
 *      slots themselves (address-local, e.g. same L1D set/way)
 *   signature moves to the sweep seam / disappears ->
 *      it follows the forward-progress position in the sweep
 * Same skeleton as the parent otherwise: 3rd ldr (scratch, discarded) at
 * top + 2-source ldr z loads + rotating svadd/sveor/svand/svorr per slot
 * (slot%4) + back-to-back str temp / str dst; memcmp in the cold path.
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

#define NEON_ROT_LDR_AT_TOP_ROWMAJOR_SVE_SLOTS 1024

struct NeonRotLdrAtTopRowMajorSveData {
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

/* 优先级: 1. -O neon_rot_ldr_at_top_rowmajor_sve.slots=N  2. SDC_NEON_ROT_LDR_AT_TOP_ROWMAJOR_SVE_SLOTS
 * 3. 设计默认 1024
 * （诊断探针: 默认保持设计几何, 显式配置才生效 — 槽距/行距模式是激发参数不动） */
static size_t neon_rot_ldr_at_top_rowmajor_sve_slots(struct test *test)
{
    const int64_t dflt = NEON_ROT_LDR_AT_TOP_ROWMAJOR_SVE_SLOTS;
    int64_t v = sdc_knob_int_clamped(test, "slots", 0, NEON_ROT_LDR_AT_TOP_ROWMAJOR_SVE_SLOTS, 1 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("neon_rot_ldr_at_top_rowmajor_sve", "slots", &v, NEON_ROT_LDR_AT_TOP_ROWMAJOR_SVE_SLOTS, 1 << 20);
    return v > 0 ? (size_t)v : (size_t)dflt;
}

#endif

static int neon_rot_ldr_at_top_rowmajor_sve_init(struct test *test) {
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
    const size_t slots = neon_rot_ldr_at_top_rowmajor_sve_slots(test);
    const size_t elems = slots * (size_t)lanes;

    auto *data = static_cast<NeonRotLdrAtTopRowMajorSveData *>(malloc(sizeof(NeonRotLdrAtTopRowMajorSveData)));
    data->lanes = lanes;
    data->slots = (int)slots;
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->scratch = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    memset_random(data->srcA, elems * sizeof(uint64_t));
    memset_random(data->srcB, elems * sizeof(uint64_t));
    memset_random(data->scratch, elems * sizeof(uint64_t));

    /* golden: same rotating composition per slot (slot%4, whole vector), computed on the same SVE units */
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
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)
static int neon_rot_ldr_at_top_rowmajor_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<NeonRotLdrAtTopRowMajorSveData *>(test->data);

    /* 共享缓冲按 init 时的 VL 定容；线程继承同 VL，但失配会导致静默错位
     * (假 SDC)，fail-closed 显式报错 */
    if ((int)svcntd() != data->lanes) {
        log_error("neon_rot_ldr_at_top_rowmajor_sve: vector length changed between init (%d) and run (%d)",
                  data->lanes, (int)svcntd());
        report_fail_msg("neon_rot_ldr_at_top_rowmajor_sve: vector length changed between init and run");
    }

    const size_t elems = (size_t)data->slots * data->lanes;
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 13) {
        const bool ascending = (sweep & 1) == 0;
        for (int n = 0; n < data->slots; n++) {
            const int i = ascending ? n : (data->slots - 1 - n);
            const size_t off = (size_t)i * data->lanes;

            /* 3rd load at the TOP of the iteration (before the sources);
             * value intentionally discarded — this probe measures the
             * presence/position of the load, not its data */
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

            /* both stores write the ALU result — the str temp / str dst
             * pair stays back-to-back with NO memory op in between */
            store_vec_sve(temp + off, res);
            store_vec_sve(dst + off, res);
        }
        sweep++;

        if (memcmp(dst, data->expected, elems * sizeof(uint64_t)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < elems; ++_dump_j) {
                    if (dst[_dump_j] != data->expected[_dump_j]) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("neon_rot_ldr_at_top_rowmajor_sve: mismatch at flat=%zu slot=%zu lane=%zu: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                                      _dump_j, _dump_j / (size_t)data->lanes, _dump_j % (size_t)data->lanes,
                                      (unsigned long long)data->expected[_dump_j],
                                      (unsigned long long)dst[_dump_j],
                                      (unsigned long long)(data->expected[_dump_j] ^ dst[_dump_j]));
                            ++_dump_shown;
                        }
                    }
                }
                log_error("neon_rot_ldr_at_top_rowmajor_sve: total mismatched elements: %zu / %zu (slots x lanes)",
                          _dump_bad, elems);
            }
            report_fail_msg("neon_rot_ldr_at_top_rowmajor_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int neon_rot_ldr_at_top_rowmajor_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_sve");
    return EXIT_SKIP;
}
#endif

static int neon_rot_ldr_at_top_rowmajor_sve_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotLdrAtTopRowMajorSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_at_top_rowmajor_sve,
             "SVE datapath variant of the order probe: alternate ascending/descending "
             "sweeps over 1024 slots with 3rd ldr z at top + rotating ALU + "
             "back-to-back str pair (port of neon_rot_ldr_at_top_rowmajor)")
    .test_init    = neon_rot_ldr_at_top_rowmajor_sve_init,
    .test_run     = neon_rot_ldr_at_top_rowmajor_sve_run,
    .test_cleanup = neon_rot_ldr_at_top_rowmajor_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
