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

    /* golden (issue #373): scalar independent recomputation — pure C
     * integer ops on the scalar pipeline, never the SVE units under
     * test, so a persistent SVE datapath fault cannot corrupt golden
     * and actual identically. Interleaved layout preserved: slot i's
     * sources are src slots 2i (A) and 2i+1 (B); phase = slot % 4,
     * matching rot_alu(i, ...) in the run loop. */
    {
        const uint64_t *s  = reinterpret_cast<const uint64_t *>(data->src);
        uint64_t       *ex = reinterpret_cast<uint64_t *>(data->expected);
        const size_t lanes_per_slot = data->vl_bytes / sizeof(uint64_t);
        for (int i = 0; i < data->vl_slots; i++) {
            const size_t a_base = (size_t)(2 * i)     * lanes_per_slot;
            const size_t b_base = (size_t)(2 * i + 1) * lanes_per_slot;
            const size_t e_base = (size_t)i           * lanes_per_slot;
            for (size_t l = 0; l < lanes_per_slot; l++) {
                const size_t e = e_base + l;
                switch (i % 4) {
                    case 0: ex[e] = s[a_base + l] + s[b_base + l]; break;
                    case 1: ex[e] = s[a_base + l] ^ s[b_base + l]; break;
                    case 2: ex[e] = s[a_base + l] & s[b_base + l]; break;
                    case 3: ex[e] = s[a_base + l] | s[b_base + l]; break;
                }
            }
        }
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

    /* Loop granularity: the body is one full (alternating-direction)
     * sweep of all vl_slots — 3 vector loads (scratch + the two source
     * vectors) + 2 stores (temp, dst) + the golden memcmp (dst +
     * expected reads) — so one body moves ~7 x vl_slots x vl_bytes of
     * memory per thread. vl_slots is memory-adaptive
     * (sdc_auto_linear_count: 600 permille of RAM across 6 VL-wide
     * buffers, clamped to 1M slots), which makes one body tens of
     * milliseconds and more under bandwidth contention. TEST_LOOP only
     * checks the -t deadline every N bodies (framework/sandstone.h):
     * the N = 1<<13 was inherited from the fixed-geometry NEON
     * ancestors (1024 x 16 B buffers kept one checkpoint under
     * ~0.9 GiB of traffic) — with adaptive sizing the first deadline
     * check slid past the minute scale (measured on the at_top
     * sibling: no checkpoint within 60 s at -n 8 on a 128-core/
     * SVE-256 host), so any realistic -t reads as "Timed Out"
     * (issue #360, same failure class as #326 / #349). N = 4 bounds
     * the exit latency to 4 bodies (~sub-second) while keeping the
     * auto-fracture segment (default 40 checks = 160 bodies) long
     * enough that per-segment re-entry (temp/dst re-alloc) stays
     * negligible. */
    uint64_t sweep = 0;
    TEST_LOOP(test, 4) {
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
    .arm64_requires = ARM64_REQUIRES_SVE,
END_DECLARE_TEST
