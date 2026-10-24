/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve_rot_ldr_at_top_rowmajor_k5inter_rand
 * @parblock
 * SVE VLA port of tests/cpu/misc/
 * neon_rot_ldr_at_top_rowmajor_k5inter_rand.cpp — interleaved-layout
 * NATURAL-data control for the K5 cell (2026-09-13).
 *
 * k5far changes TWO things vs the chassis at once: the source layout
 * (two parallel buffers -> one interleaved buffer) and keeps the tags.
 * This cell holds the interleaved layout but fills with NATURAL random
 * data (memset_random), so the k5far-vs-this pair isolates the LAYOUT
 * effect and this-vs-parent isolates layout-without-tags. It is also
 * the dump-capable A-track of Phase 0.5 under the champion layout.
 * Rate readout only (no tags -> no fine classification); dump gives
 * coarse classification (recoverable-window / Hamming / byte
 * granularity) per design doc §9.2.
 *
 * SVE specifics: sources live in ONE interleaved buffer of
 * 2*VL-slots (srcA = slot 2i, srcB = slot 2i+1); the third load at
 * iteration top reads a separate scratch buffer. On miscompare the
 * five views (dst/expected/srcA-view/srcB-view/scratch) are dumped to
 * movbe_log/gps_rowmajor/ with the svek5ir_ prefix so SVE dumps never
 * collide with NEON ones.
 *
 * Build note: all-VL Phase 2 P5 migration DONE — compiled in the
 * true-VLA tests_sve_vla library (-march=armv8.2-a+sve, no
 * -msve-vector-bits), so the interleaved 2*VL-slot layout and the
 * dump de-interleave both follow the runtime VL (spike evidence:
 * three-state compile probe 128/256/no-flag, zero warnings; see
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

/* 结果槽 VL 数(原 1024; src 交错缓冲 2 倍宽, 缓冲 = 槽数 × VL 字节) — 统一优先级链:
 *   1. -O sve_rot_ldr_at_top_rowmajor_k5inter_rand.slots=N / 配置文件同名条目
 *   2. 环境变量 SDC_SVE_ROT_LDR_AT_TOP_ROWMAJOR_K5INTER_RAND_SLOTS
 *   3. 自适应: 共享 src(2 VL 宽)+scratch+expected 3 + 每线程 temp/dst 2,
 *      每结果槽共 6 个 VL(128b VL 即 16B/槽), 占物理内存 60%,
 *      clamp [1024, 1M]。失败路径 viewA/viewB 用同一槽数(随 vl_slots)。 */
static size_t sve_k5ir_slots(struct test *test, size_t vl_bytes)
{
    int64_t v = sdc_knob_int_clamped(test, "slots", 0, 1024, 1 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("sve_rot_ldr_at_top_rowmajor_k5inter_rand", "slots", &v, 1024, 1 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count((int64_t)vl_bytes, 6, 1.0, 600,
                                         thread_count(), 1024, 1 << 20, test->id, "slots");
}

#define SVE_K5IR_DUMP_DIR "movbe_log/gps_rowmajor"

struct SveK5InterRandData {
    int      vl_slots;   /* result elements (K5IR_COUNT equivalent) */
    size_t   vl_bytes;
    uint8_t *src;        /* interleaved: slot 2i = A, slot 2i+1 = B */
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

static int sve_k5ir_init(struct test *test) {
    /* All-VL Phase 2 P5: framework-layer capability + 128..1024-bit range
     * gate in one call (framework/device/cpu/arm64/arm64_vl.h) — the
     * HWCAP/rdvl probe executes no SVE instruction, so it runs before the
     * interleaved-layout geometry below. The framework's
     * compiler_minimum_device gate already catches non-SVE hosts; this
     * keeps a test-specific skip reason even if that ordering ever
     * changes. */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    auto *data = static_cast<SveK5InterRandData *>(malloc(sizeof(SveK5InterRandData)));
    data->vl_bytes = (size_t)svl;
    data->vl_slots = (int)sve_k5ir_slots(test, data->vl_bytes);

    const size_t src_buf = (size_t)data->vl_slots * 2 * data->vl_bytes;
    const size_t res_buf = (size_t)data->vl_slots * data->vl_bytes;
    data->src     = static_cast<uint8_t *>(aligned_alloc_safe(64, src_buf));
    data->scratch = static_cast<uint8_t *>(aligned_alloc_safe(64, res_buf));
    data->expected= static_cast<uint8_t *>(aligned_alloc_safe(64, res_buf));

    mkdir(SVE_K5IR_DUMP_DIR, 0755);

    memset_random(data->src, src_buf);
    memset_random(data->scratch, res_buf);

    for (int i = 0; i < data->vl_slots; i++) {
        const size_t off_a = ((size_t)2 * i) * data->vl_bytes;
        const size_t off_b = off_a + data->vl_bytes;
        const svuint64_t a = load_vec(data->src + off_a);
        const svuint64_t b = load_vec(data->src + off_b);
        store_vec(data->expected + (size_t)i * data->vl_bytes, rot_alu(i, a, b));
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int sve_k5ir_run(struct test *test, int cpu) {
    auto *data = static_cast<SveK5InterRandData *>(test->data);

    /* All-VL policy #3: fail closed on init/run VL drift — load-bearing:
     * the interleaved src layout (slot 2i / 2i+1) and the golden are
     * sized at init's VL; a different VL here would make the full-vector
     * asm ldr/str walk wrong strides and corrupt the dump views too. */
    if (svcntb() != data->vl_bytes) {
        log_error("sve_rot_ldr_at_top_rowmajor_k5inter_rand: SVE VL "
                  "drifted between init (%zu bytes) and run (%u bytes)",
                  data->vl_bytes, (unsigned)svcntb());
        report_fail_msg("sve_rot_ldr_at_top_rowmajor_k5inter_rand: SVE VL "
                        "drifted between init and run");
    }

    const size_t res_buf = (size_t)data->vl_slots * data->vl_bytes;
    uint8_t *temp = static_cast<uint8_t *>(aligned_alloc_safe(64, res_buf));
    uint8_t *dst  = static_cast<uint8_t *>(aligned_alloc_safe(64, res_buf));

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 13) {
        const bool ascending = (sweep & 1) == 0;
        for (int n = 0; n < data->vl_slots; n++) {
            const int i = ascending ? n : (data->vl_slots - 1 - n);
            const size_t off_a = ((size_t)2 * i) * data->vl_bytes;
            const size_t off   = (size_t)i * data->vl_bytes;

            svuint64_t unused_x = load_vec(data->scratch + off);
            (void)unused_x;

            const svuint64_t a = load_vec(data->src + off_a);
            const svuint64_t b = load_vec(data->src + off_a + data->vl_bytes);
            const svuint64_t res = rot_alu(i, a, b);

            store_vec(temp + off, res);
            store_vec(dst + off, res);
        }
        sweep++;

        if (memcmp(dst, data->expected, res_buf) != 0) {
            char path[256];
            snprintf(path, sizeof(path), "%s/svek5ir_fail_cpu%d_sweep%lu.bin", SVE_K5IR_DUMP_DIR,
                     cpu, static_cast<unsigned long>(sweep));
            FILE *f = fopen(path, "wb");
            if (f) {
                /* de-interleave the source into parallel views so the
                 * dump layout matches the NEON k5ir dump exactly */
                uint8_t *viewA = static_cast<uint8_t *>(aligned_alloc_safe(64, res_buf));
                uint8_t *viewB = static_cast<uint8_t *>(aligned_alloc_safe(64, res_buf));
                for (int i = 0; i < data->vl_slots; i++) {
                    memcpy(viewA + (size_t)i * data->vl_bytes,
                           data->src + ((size_t)2 * i) * data->vl_bytes, data->vl_bytes);
                    memcpy(viewB + (size_t)i * data->vl_bytes,
                           data->src + ((size_t)2 * i + 1) * data->vl_bytes, data->vl_bytes);
                }
                fwrite(dst,           1, res_buf, f);
                fwrite(data->expected,1, res_buf, f);
                fwrite(viewA,         1, res_buf, f);
                fwrite(viewB,         1, res_buf, f);
                fwrite(data->scratch, 1, res_buf, f);
                fprintf(f, "svek5inter_rand sweep=%lu\n", static_cast<unsigned long>(sweep));
                fclose(f);
                free(viewA);
                free(viewB);
            }
            report_fail_msg("sve_rot_ldr_at_top_rowmajor_k5inter_rand data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int sve_k5ir_cleanup(struct test *test) {
    auto *data = static_cast<SveK5InterRandData *>(test->data);
    if (data) {
        free(data->src);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(sve_rot_ldr_at_top_rowmajor_k5inter_rand, "K5 layout control (SVE): interleaved dual-source layout with NATURAL random data (no tags) — isolates layout effect from tag effect; dump-capable A-track")
    .test_init    = sve_k5ir_init,
    .test_run     = sve_k5ir_run,
    .test_cleanup = sve_k5ir_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
