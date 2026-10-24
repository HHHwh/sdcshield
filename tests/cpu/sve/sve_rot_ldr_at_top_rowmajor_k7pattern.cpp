/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve_rot_ldr_at_top_rowmajor_k7p_zero / k7p_one / k7p_rand / k7p_hiham
 * @parblock
 * SVE VLA port of tests/cpu/misc/
 * neon_rot_ldr_at_top_rowmajor_k7pattern.cpp — K7 pattern battery,
 * SVE lane shape (2026-09-13 campaign
 * docs/CORE179_READPATH_LOCALIZATION_CAMPAIGN_20260913.md §8).
 *
 * Real question: is the NEON lane curve (2x64b=223/30min on the parent,
 * 4x32b=0, 16x8b=2) a PHYSICAL-lane effect or a pattern-activation
 * effect? The first GPS round showed the damage is pattern-SENSITIVE
 * (orr 71% / and 23% / eor 5% / add 1%; corrupted fields avoid
 * all-0/all-1 byte segments). This battery maps the activation surface
 * of the SVE 64b-lane path (VL/8 lanes per z register, scalable):
 *
 *   k7p_zero : all-zero operands (both sources)         -> activation floor
 *   k7p_one  : all-one operands                          -> complementary floor
 *   k7p_rand : GPS-tagged operands                       -> B-track baseline
 *   k7p_hiham: high-Hamming tags (alternating 0x55/0xAA) -> maximum transition density
 *
 * All four share the SAME hot loop (SVE family skeleton, svadd/
 * sveor/svand/svorr _u64_x, i%4 rotation) — only the fill differs.
 * The rand cell doubles as the dump-capable A-track with GPS tags so
 * the field-substitution classifier works on it.
 *
 * Pre-registered readouts (unchanged from the NEON battery):
 *   - zero/one ~0 fail while rand/hiham fail -> pattern gating;
 *   - all cells fail similarly -> pattern gating weak in the 64b-lane
 *     path, the NEON 4x32b zero needs a lane-shape explanation;
 *   - hiham >> rand -> transition density matters (physical coupling).
 *
 * SVE/Tag note: each z-register slot holds vl_bytes/8 64b words; the
 * GPS tag byte_off counts bytes from the start of the buffer, so at
 * 256b VL the maximum byte_off is 1023*32+24 = 32760 < 65535 — the
 * 16b byte_off field still fits for any VL up to 512b inclusive.
 *
 * Build note: all-VL Phase 2 P5 migration DONE — compiled in the
 * true-VLA tests_sve_vla library (-march=armv8.2-a+sve, no
 * -msve-vector-bits), so the vl_bytes/8-lane words-per-slot and the
 * GPS-tag fill follow the runtime VL (spike evidence: three-state
 * compile probe 128/256/no-flag, zero warnings; see
 * docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md §6).
 * @endparblock
 */

#include "sandstone.h"
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <sys/stat.h>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

/* 每缓冲 VL 槽数(原 1024, 缓冲 = 槽数 × VL 字节) — 统一优先级链:
 *   1. -O sve_rot_ldr_at_top_rowmajor_k7p_{zero,one,rand,hiham}.slots=N
 *      / 配置文件同名条目 (4 个电池单元经 test->id 各自独立)
 *   2. 环境变量 SDC_SVE_ROT_LDR_AT_TOP_ROWMAJOR_K7P_<CELL>_SLOTS
 *   3. 自适应: 共享 srcA/srcB/scratch/expected 4 + 每线程 temp/dst 2,
 *      共 6 个 VL 宽缓冲, 每槽一个 VL(128b VL 即 16B/槽), 占物理内存
 *      60%, clamp [1024, 1M]。四个单元同一 hot loop, 槽数只决定
 *      缓冲槽数; 模式填充随运行时槽数生成, golden 同口径。 */
static size_t sve_k7p_slots(struct test *test, size_t vl_bytes)
{
    int64_t v = sdc_knob_int_clamped(test, "slots", 0, 1024, 1 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped(test->id, "slots", &v, 1024, 1 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count((int64_t)vl_bytes, 6, 1.0, 600,
                                         thread_count(), 1024, 1 << 20, test->id, "slots");
}

#define SVE_K7P_DUMP_DIR "movbe_log/gps_rowmajor"

enum SveK7Pattern {
    P_ZERO,
    P_ONE,
    P_RAND_GPS,     /* GPS-tagged = B-track shape */
    P_HIHAM,        /* 0x55AA alternating high-Hamming */
};

struct SveK7PData {
    int      vl_slots;
    size_t   vl_bytes;
    uint8_t *srcA;
    uint8_t *srcB;
    uint8_t *scratch;
    uint8_t *expected;
};

#if defined(__aarch64__)

static inline svuint64_t load_vec(const void *addr) {
    svuint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

static inline void store_vec(void *addr, svuint64_t val) {
    __asm__ volatile ("str %0, [%1]" :: "w"(val), "r"(addr) : "memory");
}

static inline svuint64_t rot_alu(int i, svuint64_t a, svuint64_t b) {
    switch (i % 4) {
        case 0:  return svadd_u64_x(svptrue_b64(), a, b);
        case 1:  return sveor_u64_x(svptrue_b64(), a, b);
        case 2:  return svand_u64_x(svptrue_b64(), a, b);
        default: return svorr_u64_x(svptrue_b64(), a, b);
    }
}

#endif

static inline uint64_t sve_gps_tag(uint64_t magic, uint64_t byte_off, uint64_t gen) {
    return (magic                      << 48)
         | ((byte_off        & 0xFFFFULL) << 32)
         | ((~byte_off       & 0xFFFFULL) << 16)
         |  (gen             & 0xFFFFULL);
}

/* pattern fill: P_RAND_GPS keeps the GPS tag structure (classifier
 * works); the others are uniform patterns per the cell's purpose. */
static uint64_t sve_pattern_word(enum SveK7Pattern p, int which, uint64_t byte_off) {
    switch (p) {
        case P_ZERO:     return 0x0000000000000000ULL;
        case P_ONE:      return 0xFFFFFFFFFFFFFFFFULL;
        case P_RAND_GPS: {
            const uint64_t magic = (which == 0) ? 0xA5A5ULL : ((which == 1) ? 0x5A5AULL : 0x0F0FULL);
            return sve_gps_tag(magic, byte_off, 0);
        }
        case P_HIHAM: {
            /* high-Hamming with position info: alternate 0x55/0xAA per
             * byte so every adjacent byte pair differs in 8 bits, and
             * byte_off parity is recoverable offline */
            uint64_t w = 0;
            for (int b = 0; b < 8; b++) {
                const uint64_t nib = (((byte_off + static_cast<uint64_t>(b)) & 1) ? 0xAAULL : 0x55ULL);
                w |= (nib << (8 * b));
            }
            return w;
        }
    }
    return 0;
}

static int sve_k7p_init_common(struct test *test, enum SveK7Pattern p) {
    /* All-VL Phase 2 P5: framework-layer capability + 128..1024-bit range
     * gate in one call (framework/device/cpu/arm64/arm64_vl.h) — the
     * HWCAP/rdvl probe executes no SVE instruction, so it runs before the
     * words-per-slot pattern fill below. Shared by all four battery
     * cells. The framework's compiler_minimum_device gate already
     * catches non-SVE hosts; this keeps a test-specific skip reason even
     * if that ordering ever changes. */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    auto *data = static_cast<SveK7PData *>(malloc(sizeof(SveK7PData)));
    data->vl_bytes = (size_t)svl;
    data->vl_slots = (int)sve_k7p_slots(test, data->vl_bytes);

    const size_t buf = (size_t)data->vl_slots * data->vl_bytes;
    data->srcA    = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));
    data->srcB    = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));
    data->scratch = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));
    data->expected= static_cast<uint8_t *>(aligned_alloc_safe(64, buf));

    mkdir(SVE_K7P_DUMP_DIR, 0755);

    /* fill word-by-word: word (slot, lane, which) gets the pattern
     * value for its byte offset in the buffer */
    const int words_per_slot = (int)(data->vl_bytes / 8);
    for (int i = 0; i < data->vl_slots; i++) {
        uint64_t *wa = reinterpret_cast<uint64_t *>(data->srcA + (size_t)i * data->vl_bytes);
        uint64_t *wb = reinterpret_cast<uint64_t *>(data->srcB + (size_t)i * data->vl_bytes);
        uint64_t *ws = reinterpret_cast<uint64_t *>(data->scratch + (size_t)i * data->vl_bytes);
        for (int lane = 0; lane < words_per_slot; lane++) {
            const uint64_t base = (size_t)i * data->vl_bytes + (size_t)lane * 8;
            wa[lane] = sve_pattern_word(p, 0, base);
            wb[lane] = sve_pattern_word(p, 1, base);
            ws[lane] = sve_pattern_word(p, 2, base);
        }
    }

    for (int i = 0; i < data->vl_slots; i++) {
        const size_t off = (size_t)i * data->vl_bytes;
        const svuint64_t a = load_vec(data->srcA + off);
        const svuint64_t b = load_vec(data->srcB + off);
        store_vec(data->expected + off, rot_alu(i, a, b));
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int sve_k7p_run_common(struct test *test, int cpu, const char *name) {
    auto *data = static_cast<SveK7PData *>(test->data);

    /* All-VL policy #3: fail closed on init/run VL drift — load-bearing:
     * the pattern fill is word-addressed by init's vl_bytes/8
     * words-per-slot; a different VL here would make the full-vector asm
     * ldr/str walk wrong strides. Shared by all four battery cells. */
    if (svcntb() != data->vl_bytes) {
        log_error("sve_rot_ldr_at_top_rowmajor_%s: SVE VL drifted between "
                  "init (%zu bytes) and run (%u bytes)", name,
                  data->vl_bytes, (unsigned)svcntb());
        report_fail_msg("sve_rot_ldr_at_top_rowmajor_%s: SVE VL drifted "
                        "between init and run", name);
    }

    const size_t buf = (size_t)data->vl_slots * data->vl_bytes;
    uint8_t *temp = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));
    uint8_t *dst  = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 13) {
        const bool ascending = (sweep & 1) == 0;
        for (int n = 0; n < data->vl_slots; n++) {
            const int i = ascending ? n : (data->vl_slots - 1 - n);
            const size_t off = (size_t)i * data->vl_bytes;

            svuint64_t unused_x = load_vec(data->scratch + off);
            (void)unused_x;

            const svuint64_t a = load_vec(data->srcA + off);
            const svuint64_t b = load_vec(data->srcB + off);
            const svuint64_t res = rot_alu(i, a, b);

            store_vec(temp + off, res);
            store_vec(dst + off, res);
        }
        sweep++;

        if (memcmp(dst, data->expected, buf) != 0) {
            char path[256];
            snprintf(path, sizeof(path), "%s/sve%s_fail_cpu%d_sweep%lu.bin", SVE_K7P_DUMP_DIR, name,
                     cpu, static_cast<unsigned long>(sweep));
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(dst,           1, buf, f);
                fwrite(data->expected,1, buf, f);
                fwrite(data->srcA,    1, buf, f);
                fwrite(data->srcB,    1, buf, f);
                fwrite(data->scratch, 1, buf, f);
                fprintf(f, "sve%s sweep=%lu\n", name, static_cast<unsigned long>(sweep));
                fclose(f);
            }
            report_fail_msg("sve_rot_ldr_at_top_rowmajor_%s data miscompare", name);
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int sve_k7p_cleanup_common(struct test *test) {
    auto *data = static_cast<SveK7PData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

/* ---- four battery cells (same skeleton, fill-only delta) ---- */

static int sve_k7p_zero_init(struct test *t)  { return sve_k7p_init_common(t, P_ZERO); }
static int sve_k7p_one_init(struct test *t)   { return sve_k7p_init_common(t, P_ONE); }
static int sve_k7p_rand_init(struct test *t)  { return sve_k7p_init_common(t, P_RAND_GPS); }
static int sve_k7p_hiham_init(struct test *t) { return sve_k7p_init_common(t, P_HIHAM); }

static int sve_k7p_zero_run(struct test *t, int c)  { return sve_k7p_run_common(t, c, "k7p_zero"); }
static int sve_k7p_one_run(struct test *t, int c)   { return sve_k7p_run_common(t, c, "k7p_one"); }
static int sve_k7p_rand_run(struct test *t, int c)  { return sve_k7p_run_common(t, c, "k7p_rand"); }
static int sve_k7p_hiham_run(struct test *t, int c) { return sve_k7p_run_common(t, c, "k7p_hiham"); }

DECLARE_TEST(sve_rot_ldr_at_top_rowmajor_k7p_zero, "K7 pattern cell (SVE): all-zero operands (activation floor of the SVE 64b-lane path)")
    .test_init    = sve_k7p_zero_init,
    .test_run     = sve_k7p_zero_run,
    .test_cleanup = sve_k7p_cleanup_common,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

DECLARE_TEST(sve_rot_ldr_at_top_rowmajor_k7p_one, "K7 pattern cell (SVE): all-one operands (complementary activation floor)")
    .test_init    = sve_k7p_one_init,
    .test_run     = sve_k7p_one_run,
    .test_cleanup = sve_k7p_cleanup_common,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

DECLARE_TEST(sve_rot_ldr_at_top_rowmajor_k7p_rand, "K7 pattern cell (SVE): GPS-tagged operands (B-track baseline; doubles as dump-capable A-track for Phase 0.5)")
    .test_init    = sve_k7p_rand_init,
    .test_run     = sve_k7p_rand_run,
    .test_cleanup = sve_k7p_cleanup_common,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

DECLARE_TEST(sve_rot_ldr_at_top_rowmajor_k7p_hiham, "K7 pattern cell (SVE): 0x55/0xAA alternating high-Hamming operands (maximum transition density)")
    .test_init    = sve_k7p_hiham_init,
    .test_run     = sve_k7p_hiham_run,
    .test_cleanup = sve_k7p_cleanup_common,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
