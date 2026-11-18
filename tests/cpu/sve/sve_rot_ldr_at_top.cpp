/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve_rot_ldr_at_top
 * @parblock
 * SVE VLA port of tests/cpu/misc/neon_rot_ldr_at_top.cpp (position-vs-
 * existence probe, plan ② of the 2026-09-04 attribution split).
 *
 * Same instruction shape as the NEON parent, lifted to SVE:
 *   ldrX(scratch)  ldrA  ldrB  ALU  str temp  str dst
 * The third load sits at the TOP of the iteration (before the source
 * loads), reading a scratch buffer; its value is discarded. Same
 * 3xldr+2xstr AGU mix as the NEON fwd_probe; the only variable vs a
 * future SVE fwd_probe is the third load's POSITION.
 *
 * Differences vs the NEON parent, all forced by SVE's scalable vector
 * length (VL):
 *   - buffers are sized in VL slots: `SLOTS` z-register-width elements
 *     each, allocated at init from the gated VL (arm64_require_sve_vl
 *     return), so the test runs unchanged on 128b/256b/512b/1024b
 *     hardware (VLA by design; all-VL Phase 2 P5 migration DONE —
 *     compiled in the true-VLA tests_sve_vla library with no
 *     -msve-vector-bits, so the sizeless types adopt the runtime VL;
 *     spike evidence: three-state compile probe 128/256/no-flag, zero
 *     warnings — see
 *     docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md §6);
 *   - loads/stores use inline asm `ldr`/`str` on z registers (NEON
 *     parent: `ldr %q0` on q registers) — one instruction each, no
 *     compiler-generated addressing freedom;
 *   - the ALU op is the svadd/sveor/svand/svorr _u64_x intrinsic
 *     (predicated with svptrue_b64()), which compiles to a single
 *     add/eor/and/orr z-register instruction — the direct SVE
 *     analogue of vaddq/veorq/vandq/vorrq_u64;
 *   - golden is recomputed in init with scalar C integer ops on the
 *     scalar pipeline (issue #373 paradigm), independent of the SVE
 *     units under test.
 *
 * Runs only on SVE hardware; the framework skips it elsewhere with
 * "test compiled with sve" (compiler_minimum_device gating).
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
 *   1. -O sve_rot_ldr_at_top.slots=N / 配置文件同名条目
 *   2. 环境变量 SDC_SVE_ROT_LDR_AT_TOP_SLOTS
 *   3. 自适应: 共享 srcA/srcB/scratch/expected 4 + 每线程 temp/dst 2,
 *      共 6 个 VL 宽缓冲, 每槽一个 VL(128b VL 即 16B/槽), 占物理内存
 *      60%, clamp [1024, 1M]。槽距 = 1 个 VL(连续排布), 槽数只决定
 *      缓冲槽数, 不影响槽间地址模式。 */
static size_t sve_rot_ldr_at_top_slots(struct test *test, size_t vl_bytes)
{
    int64_t v = sdc_knob_int_clamped(test, "slots", 0, 1024, 1 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("sve_rot_ldr_at_top", "slots", &v, 1024, 1 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count((int64_t)vl_bytes, 6, 1.0, 600,
                                         thread_count(), 1024, 1 << 20, test->id, "slots");
}

struct SveRotLdrAtTopData {
    int      vl_slots;   /* element count in full z-register widths */
    size_t   vl_bytes;   /* init-time VL bytes returned by arm64_require_sve_vl */
    uint8_t *srcA;
    uint8_t *srcB;
    uint8_t *scratch;
    uint8_t *expected;
};

#if defined(__aarch64__)

/* one z-register load/store — same single-instruction form as the NEON
 * parent's %q0 inline asm, lifted to a scalable z register */
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

static int sve_rot_ldr_at_top_init(struct test *test) {
    /* All-VL Phase 2 P5: framework-layer capability + 128..1024-bit range
     * gate in one call (framework/device/cpu/arm64/arm64_vl.h) — the
     * HWCAP/rdvl probe executes no SVE instruction, so it runs before the
     * first svcntb() below. The framework's compiler_minimum_device gate
     * (the "test compiled with sve" skip) already catches non-SVE hosts;
     * this keeps a test-specific skip reason even if that ordering ever
     * changes. */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    auto *data = static_cast<SveRotLdrAtTopData *>(malloc(sizeof(SveRotLdrAtTopData)));
    data->vl_bytes = (size_t)svl;
    data->vl_slots = (int)sve_rot_ldr_at_top_slots(test, data->vl_bytes);

    const size_t buf = data->vl_slots * data->vl_bytes;
    data->srcA     = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));
    data->srcB     = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));
    data->scratch  = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));
    data->expected = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));

    memset_random(data->srcA, buf);
    memset_random(data->srcB, buf);
    memset_random(data->scratch, buf);

    /* golden (issue #373): scalar independent recomputation — pure C
     * integer ops on the scalar pipeline, never the SVE units under
     * test, so a persistent SVE datapath fault (stuck lane / unit
     * computing deterministic wrong bits) can no longer corrupt golden
     * and actual identically; it surfaces as a byte difference at the
     * run-side memcmp. Phase = slot index % 4 — all lanes of a slot
     * share the phase, matching rot_alu(i, ...) in the run loop. */
    {
        const uint64_t *sa = reinterpret_cast<const uint64_t *>(data->srcA);
        const uint64_t *sb = reinterpret_cast<const uint64_t *>(data->srcB);
        uint64_t       *ex = reinterpret_cast<uint64_t *>(data->expected);
        const size_t lanes_per_slot = data->vl_bytes / sizeof(uint64_t);
        for (int i = 0; i < data->vl_slots; i++) {
            const size_t base = (size_t)i * lanes_per_slot;
            for (size_t l = 0; l < lanes_per_slot; l++) {
                const size_t e = base + l;
                switch (i % 4) {
                    case 0: ex[e] = sa[e] + sb[e]; break;
                    case 1: ex[e] = sa[e] ^ sb[e]; break;
                    case 2: ex[e] = sa[e] & sb[e]; break;
                    case 3: ex[e] = sa[e] | sb[e]; break;
                }
            }
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int sve_rot_ldr_at_top_run(struct test *test, int cpu) {
    auto *data = static_cast<SveRotLdrAtTopData *>(test->data);

    /* All-VL policy #3: fail closed on init/run VL drift — load-bearing
     * here, not ceremony: the buffers and the golden are sized/computed
     * at init's VL, and a different VL in run would make the full-vector
     * asm ldr/str walk wrong strides. */
    if (svcntb() != data->vl_bytes) {
        log_error("sve_rot_ldr_at_top: SVE VL drifted between init (%zu "
                  "bytes) and run (%u bytes)", data->vl_bytes,
                  (unsigned)svcntb());
        report_fail_msg("sve_rot_ldr_at_top: SVE VL drifted between init "
                        "and run");
    }

    const size_t buf = (size_t)data->vl_slots * data->vl_bytes;
    uint8_t *temp = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));
    uint8_t *dst  = static_cast<uint8_t *>(aligned_alloc_safe(64, buf));

    /* Loop granularity: the body is a full sweep of all vl_slots —
     * 3 vector loads (scratch + srcA + srcB) + 2 stores (temp, dst) +
     * the golden memcmp (dst + expected reads) — so one body moves
     * ~7 x vl_slots x vl_bytes of memory per thread. vl_slots is
     * memory-adaptive (sdc_auto_linear_count: 600 permille of RAM
     * across 6 VL-wide buffers, clamped to 1M slots), which makes one
     * body tens of milliseconds and more under bandwidth contention.
     * TEST_LOOP only checks the -t deadline every N bodies
     * (framework/sandstone.h): the N = 1<<13 was inherited from the
     * fixed-geometry NEON ancestor (1024 x 16 B buffers kept one
     * checkpoint under ~0.9 GiB of traffic) — with adaptive sizing
     * the first deadline check slid past the minute scale (measured:
     * no checkpoint within 60 s at -n 8 on a 128-core/SVE-256 host),
     * so any realistic -t reads as "Timed Out" (issue #360, same
     * failure class as #326 / #349). N = 4 bounds the exit latency to
     * 4 bodies (~sub-second) while keeping the auto-fracture segment
     * (default 40 checks = 160 bodies) long enough that per-segment
     * re-entry (temp/dst re-alloc) stays negligible. */
    TEST_LOOP(test, 4) {
        for (int i = 0; i < data->vl_slots; i++) {
            const size_t off = (size_t)i * data->vl_bytes;

            /* 3rd load at the TOP of the iteration (before the sources);
             * value intentionally discarded — this probe measures the
             * presence/position of the load, not its data */
            svuint64_t unused_x = load_vec(data->scratch + off);
            (void)unused_x;

            const svuint64_t a = load_vec(data->srcA + off);
            const svuint64_t b = load_vec(data->srcB + off);
            const svuint64_t res = rot_alu(i, a, b);

            /* both stores write the ALU result — the str temp / str dst
             * pair stays back-to-back with NO memory op in between */
            store_vec(temp + off, res);
            store_vec(dst + off, res);
        }

        if (memcmp(dst, data->expected, buf) != 0) {
            report_fail_msg("sve_rot_ldr_at_top data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int sve_rot_ldr_at_top_cleanup(struct test *test) {
    auto *data = static_cast<SveRotLdrAtTopData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(sve_rot_ldr_at_top, "SVE position probe: 3rd ldr at iteration top (VLA port of the NEON rot_ldr_at_top probe; 3xldr+2xstr on z registers)")
    .test_init    = sve_rot_ldr_at_top_init,
    .test_run     = sve_rot_ldr_at_top_run,
    .test_cleanup = sve_rot_ldr_at_top_cleanup,
    .quality_level = TEST_QUALITY_PROD,
    .arm64_requires = ARM64_REQUIRES_SVE,
END_DECLARE_TEST
