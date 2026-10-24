/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve_rot_ldr_at_top_rowmajor
 * @parblock
 * SVE VLA port of tests/cpu/misc/neon_rot_ldr_at_top_rowmajor.cpp
 * (order variant of the position probe, 2026-09-08).
 *
 * The 55-record dump campaign on the NEON family showed corruption
 * concentrated on the FIRST indices of the inner loop (idx 0 + idx 1 =
 * 36/55 records) while the buffer spans 1024 entries. The parent
 * revisits the buffer in the same order every outer iteration; idx 0/1
 * are the "coldest / most forward-progress-dependent" slots of each
 * pass. This variant alternates the sweep order: even outer iterations
 * ascending, odd descending, so the touch pattern at idx 0/1 differs
 * (on a descending pass index 0 is the LAST slot touched, with a
 * fully-warm pipeline behind it, and the store->load adjacency across
 * the loop seam changes).
 *   strong idx-0/1 signal also on this variant  -> the signature
 *      follows the buffer slots themselves (address-local)
 *   signature moves to the sweep seam / disappears -> it follows the
 *      forward-progress position in the sweep
 * Same SVE skeleton as sve_rot_ldr_at_top otherwise: 3rd ldr at top +
 * 2-src loads + rotating ALU (svadd/sveor/svand/svorr per i%4) +
 * back-to-back str/str on z registers; memcmp in cold path.
 * Buffers are sized in VL slots (VLA by design; all-VL Phase 2 P5
 * migration DONE — compiled in the true-VLA tests_sve_vla library with
 * no -msve-vector-bits; spike evidence: three-state compile probe
 * 128/256/no-flag, zero warnings — see
 * docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md §6).
 * @endparblock
 */

#include "sandstone.h"
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

/* 每缓冲 VL 槽数(原 1024, 缓冲 = 槽数 × VL 字节) — 统一优先级链:
 *   1. -O sve_rot_ldr_at_top_rowmajor.slots=N / 配置文件同名条目
 *   2. 环境变量 SDC_SVE_ROT_LDR_AT_TOP_ROWMAJOR_SLOTS
 *   3. 自适应: 共享 srcA/srcB/scratch/expected 4 + 每线程 temp/dst 2,
 *      共 6 个 VL 宽缓冲, 每槽一个 VL(128b VL 即 16B/槽), 占物理内存
 *      60%, clamp [1024, 1M]。升/降序扫描模式与本旋钮无关。 */
static size_t sve_rot_rowmajor_slots(struct test *test, size_t vl_bytes)
{
    int64_t v = sdc_knob_int_clamped(test, "slots", 0, 1024, 1 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("sve_rot_ldr_at_top_rowmajor", "slots", &v, 1024, 1 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count((int64_t)vl_bytes, 6, 1.0, 600,
                                         thread_count(), 1024, 1 << 20, test->id, "slots");
}

struct SveRotRowMajorData {
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

static int sve_rot_rowmajor_init(struct test *test) {
    /* All-VL Phase 2 P5: framework-layer capability + 128..1024-bit range
     * gate in one call (framework/device/cpu/arm64/arm64_vl.h) — the
     * HWCAP/rdvl probe executes no SVE instruction, so it runs before the
     * buffer geometry below. The framework's compiler_minimum_device gate
     * already catches non-SVE hosts; this keeps a test-specific skip
     * reason even if that ordering ever changes. */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    auto *data = static_cast<SveRotRowMajorData *>(malloc(sizeof(SveRotRowMajorData)));
    data->vl_bytes = (size_t)svl;
    data->vl_slots = (int)sve_rot_rowmajor_slots(test, data->vl_bytes);

    const size_t buf = (size_t)data->vl_slots * data->vl_bytes;
    data->srcA     = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));
    data->srcB     = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));
    data->scratch  = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));
    data->expected = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));

    memset_random(data->srcA, buf);
    memset_random(data->srcB, buf);
    memset_random(data->scratch, buf);

    /* golden: same rotating composition, computed on the same SVE units */
    for (int i = 0; i < data->vl_slots; i++) {
        const size_t off = (size_t)i * data->vl_bytes;
        const svuint64_t a = load_vec(data->srcA + off);
        const svuint64_t b = load_vec(data->srcB + off);
        store_vec(data->expected + off, rot_alu(i, a, b));
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int sve_rot_rowmajor_run(struct test *test, int cpu) {
    auto *data = static_cast<SveRotRowMajorData *>(test->data);

    /* All-VL policy #3: fail closed on init/run VL drift — load-bearing:
     * buffers/golden are sized at init's VL; a different VL here would
     * make the full-vector asm ldr/str walk wrong strides. */
    if (svcntb() != data->vl_bytes) {
        log_error("sve_rot_ldr_at_top_rowmajor: SVE VL drifted between "
                  "init (%zu bytes) and run (%u bytes)", data->vl_bytes,
                  (unsigned)svcntb());
        report_fail_msg("sve_rot_ldr_at_top_rowmajor: SVE VL drifted "
                        "between init and run");
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
            report_fail_msg("sve_rot_ldr_at_top_rowmajor data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int sve_rot_rowmajor_cleanup(struct test *test) {
    auto *data = static_cast<SveRotRowMajorData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(sve_rot_ldr_at_top_rowmajor, "Order variant of sve_rot_ldr_at_top: alternate ascending/descending sweeps (slot-local vs forward-progress-position signature test)")
    .test_init    = sve_rot_rowmajor_init,
    .test_run     = sve_rot_rowmajor_run,
    .test_cleanup = sve_rot_rowmajor_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
