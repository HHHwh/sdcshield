/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve_rot_ldr_at_top_rowmajor_k3res
 * @parblock
 * SVE VLA port of tests/cpu/misc/
 * neon_rot_ldr_at_top_rowmajor_k3res.cpp — K3 residency cell:
 * enlarged-footprint variant (2026-09-13 campaign
 * docs/CORE179_READPATH_LOCALIZATION_CAMPAIGN_20260913.md §5 K3 axis).
 *
 * Adjudicates whether the substitution/misaligned-window SOURCE lines
 * are L1D-resident at failure time. The NEON champion footprint
 * (16 KiB per buffer, 4 buffers = 64 KiB total) sits inside the 64 KiB
 * L1D of Kunpeng 920. This cell scales the working set so a sweep
 * evicts most lines before they are re-referenced:
 *   - rate collapses -> the L1D-resident champion footprint is part of
 *     the trigger recipe (footprint sensitivity);
 *   - rate holds + tight source-distance histogram -> no eviction
 *     exposure, rate-invariance is a useful negative;
 *   - rate holds + far (evicted) sources appear -> far sources
 *     participate; L1D-internal selection is disfavoured.
 *
 * Footprint on SVE (deliberate difference vs the NEON cell, which is
 * 4x at fixed 128b): this cell uses 4096 VL slots per buffer, so the
 * per-buffer footprint scales with the hardware vector length —
 * 64 KiB at 128b VL (identical to the NEON cell), 128 KiB at 256b VL,
 * 256 KiB at 512b VL. Interpret the residency readout against the
 * target machine's VL and L1D size.
 *
 * Tag note (SVE-specific): the NEON cell's GPS tag byte_off counts
 * bytes from the buffer start (max 65520 at its fixed footprint). At
 * 256b VL this cell's absolute byte offsets would reach 131064 and
 * overflow the 16b byte_off field. The SVE port therefore encodes the
 * byte offset WITHIN a VL slot (0 .. vl_bytes-8, always < 32) instead;
 * the slot index is not in the tag. The dump carries the full buffers,
 * so absolute positions are recoverable offline. The field-substitution
 * classifier must read byte_off as intra-slot for these dumps.
 *
 * Dump: five buffers to movbe_log/gps_rowmajor/ with the svek3res_
 * prefix (never collides with NEON dumps). GPS tags otherwise
 * unchanged.
 *
 * Build note: all-VL Phase 2 P5 migration DONE — compiled in the
 * true-VLA tests_sve_vla library (-march=armv8.2-a+sve, no
 * -msve-vector-bits), so the 4096-VL-slot footprint scales with the
 * runtime VL exactly as the residency design above describes (spike
 * evidence: three-state compile probe 128/256/no-flag, zero warnings;
 * see docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md §6).
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

/* 每缓冲 VL 槽数(原 4096, 4x footprint; 缓冲 = 槽数 × VL 字节) — 统一优先级链:
 *   1. -O sve_rot_ldr_at_top_rowmajor_k3res.slots=N / 配置文件同名条目
 *   2. 环境变量 SDC_SVE_ROT_LDR_AT_TOP_ROWMAJOR_K3RES_SLOTS
 *   3. 自适应: 共享 srcA/srcB/scratch/expected 4 + 每线程 temp/dst 2,
 *      共 6 个 VL 宽缓冲, 每槽一个 VL(128b VL 即 16B/槽), 占物理内存
 *      60%, clamp [4096, 1M]。默认 4096 = 原始 4x residency 足迹;
 *      GPS tag 编码槽内偏移, 槽数不溢出 byte_off 字段。 */
static size_t sve_k3res_slots(struct test *test, size_t vl_bytes)
{
    int64_t v = sdc_knob_int_clamped(test, "slots", 0, 4096, 1 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("sve_rot_ldr_at_top_rowmajor_k3res", "slots", &v, 4096, 1 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count((int64_t)vl_bytes, 6, 1.0, 600,
                                         thread_count(), 4096, 1 << 20, test->id, "slots");
}

#define SVE_K3RES_DUMP_DIR "movbe_log/gps_rowmajor"

#define SVE_K3RES_MAGIC_A   0xA5A5ULL
#define SVE_K3RES_MAGIC_B   0x5A5AULL
#define SVE_K3RES_MAGIC_SCR 0x0F0FULL

struct SveK3ResData {
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

static inline uint64_t sve_k3res_gps_tag(uint64_t magic, uint64_t byte_off, uint64_t gen) {
    return (magic                      << 48)
         | ((byte_off        & 0xFFFFULL) << 32)
         | ((~byte_off       & 0xFFFFULL) << 16)
         |  (gen             & 0xFFFFULL);
}

static int sve_k3res_init(struct test *test) {
    /* All-VL Phase 2 P5: framework-layer capability + 128..1024-bit range
     * gate in one call (framework/device/cpu/arm64/arm64_vl.h) — the
     * HWCAP/rdvl probe executes no SVE instruction, so it runs before the
     * intra-slot GPS-tag fill below. The framework's
     * compiler_minimum_device gate already catches non-SVE hosts; this
     * keeps a test-specific skip reason even if that ordering ever
     * changes. */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    auto *data = static_cast<SveK3ResData *>(malloc(sizeof(SveK3ResData)));
    data->vl_bytes = (size_t)svl;
    data->vl_slots = (int)sve_k3res_slots(test, data->vl_bytes);

    const size_t buf = (size_t)data->vl_slots * data->vl_bytes;
    data->srcA    = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));
    data->srcB    = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));
    data->scratch = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));
    data->expected= static_cast<uint8_t *>(aligned_alloc_safe(64, buf));

    mkdir(SVE_K3RES_DUMP_DIR, 0755);

    /* tag byte_off = offset WITHIN the VL slot (see header note) */
    const int words_per_slot = (int)(data->vl_bytes / 8);
    for (int i = 0; i < data->vl_slots; i++) {
        uint64_t *wa = reinterpret_cast<uint64_t *>(data->srcA + (size_t)i * data->vl_bytes);
        uint64_t *wb = reinterpret_cast<uint64_t *>(data->srcB + (size_t)i * data->vl_bytes);
        uint64_t *ws = reinterpret_cast<uint64_t *>(data->scratch + (size_t)i * data->vl_bytes);
        for (int lane = 0; lane < words_per_slot; lane++) {
            const uint64_t intra = (size_t)lane * 8;
            wa[lane] = sve_k3res_gps_tag(SVE_K3RES_MAGIC_A,   intra, 0);
            wb[lane] = sve_k3res_gps_tag(SVE_K3RES_MAGIC_B,   intra, 0);
            ws[lane] = sve_k3res_gps_tag(SVE_K3RES_MAGIC_SCR, intra, 0);
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

static int sve_k3res_run(struct test *test, int cpu) {
    auto *data = static_cast<SveK3ResData *>(test->data);

    /* All-VL policy #3: fail closed on init/run VL drift — load-bearing:
     * the 4096-VL-slot footprint and the intra-slot-tagged golden are
     * sized at init's VL; a different VL here would make the full-vector
     * asm ldr/str walk wrong strides. */
    if (svcntb() != data->vl_bytes) {
        log_error("sve_rot_ldr_at_top_rowmajor_k3res: SVE VL drifted "
                  "between init (%zu bytes) and run (%u bytes)",
                  data->vl_bytes, (unsigned)svcntb());
        report_fail_msg("sve_rot_ldr_at_top_rowmajor_k3res: SVE VL "
                        "drifted between init and run");
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
            snprintf(path, sizeof(path), "%s/svek3res_fail_cpu%d_sweep%lu.bin", SVE_K3RES_DUMP_DIR,
                     cpu, static_cast<unsigned long>(sweep));
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(dst,           1, buf, f);
                fwrite(data->expected,1, buf, f);
                fwrite(data->srcA,    1, buf, f);
                fwrite(data->srcB,    1, buf, f);
                fwrite(data->scratch, 1, buf, f);
                fprintf(f, "svek3res sweep=%lu\n", static_cast<unsigned long>(sweep));
                fclose(f);
            }
            report_fail_msg("sve_rot_ldr_at_top_rowmajor_k3res data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int sve_k3res_cleanup(struct test *test) {
    auto *data = static_cast<SveK3ResData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(sve_rot_ldr_at_top_rowmajor_k3res, "K3 residency cell (SVE): 4096 VL slots per buffer (footprint scales with VL) — exposes whether field-substitution sources are L1D-resident at failure time")
    .test_init    = sve_k3res_init,
    .test_run     = sve_k3res_run,
    .test_cleanup = sve_k3res_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
