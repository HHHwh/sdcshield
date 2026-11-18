/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0.
 *
 * @test @b core179_kernel_sdc_repro
 * @parblock
 * Core-179 kernel-space SDC reproducer (2026-09-07 evidence chain).
 *
 * Evidence summary (docs/CORE179_PANIC_EVIDENCE_CHAIN_20260907.md and
 * docs/CORE179_KERNEL_SDC_LOADTYPE_AND_PROCESSES_20260907.md):
 *   Running the two verified user-space SDC trigger tests
 *   (neon_rot_ldr_at_top + arm0102_kreg_mask) at full-core load makes the
 *   scheduler spread OS daemons (mi-scavenger / HeapHelper / pmdalinux /
 *   irqbalance / rcu_sched / kworker) onto core 179, where kernel-space
 *   memory accesses silently corrupt: 45x "Ignoring spurious kernel
 *   translation fault" WARNs and 7+ fatal "Unable to handle kernel paging
 *   request" panics, all on CPU 179, zero MCE/RAS hardware errors.
 *   --cpuset cannot prevent this (it only constrains opendcdiag threads);
 *   only hotplug-offlining 179 does.
 *
 * This test reproduces the ENVIRONMENT part of that recipe: it alternates,
 * per outer iteration, the exact hot-loop skeletons of both trigger tests:
 *   A) neon_rot_ldr_at_top: 3rd ldr at iteration top + 2 source loads +
 *      rotating ALU (add/eor/and/orr) + back-to-back str/str (no load between)
 *   B) arm0102_kreg_mask: 2 source loads + rotating sparse compare masks
 *      (vceq/vcgt/vtst) + str -> ldr -> str back-to-back
 *
 * User-space SDC is detected via the normal miscompare channel
 * (result: fail). Kernel-space SDC (spurious faults / panic) cannot be
 * asserted from inside the test — observe with:
 *   journalctl -k | grep -E 'spurious|Oops|panic'
 *   ls /home/sdc/vmcore/
 *
 * USAGE WARNING:
 *   - DIAGNOSTIC mode (reproduce the kernel-space errors / panic): run with
 *     179 online, full-core: this test can and WILL crash the machine when
 *     the fault lands on a fatal kernel path. Only run on a machine you are
 *     allowed to crash, with kdump armed.
 *   - SAFE mode (regression / user-space SDC only): first
 *       echo 0 > /sys/devices/system/cpu/cpu179/online
 *     then run with -n <online-core-count>.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define CORE179_REPRO_COUNT 1024   /* elements per buffer (16KB-32KB) */

struct Core179ReproData {
    /* skeleton A: neon_rot_ldr_at_top (uint64x2 domain) */
    uint64x2_t *srcA64;
    uint64x2_t *srcB64;
    uint64x2_t *scratch64;
    uint64x2_t *expectedA;
    /* skeleton B: arm0102_kreg_mask (uint32x4 compare domain) */
    uint32x4_t *srcA32;
    uint32x4_t *srcB32;
    uint32x4_t *expectedB;
};

#if defined(__aarch64__)

static inline void store_vec64(void *addr, uint64x2_t val) {
    vst1q_u64(static_cast<uint64_t *>(addr), val);
}

static inline uint64x2_t load_vec64(const void *addr) {
    uint64x2_t res;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

static inline void store_vec32(void *addr, uint32x4_t val) {
    vst1q_u32(static_cast<uint32_t *>(addr), val);
}

static inline uint32x4_t load_vec32(const void *addr) {
    uint32x4_t res;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

#endif

static int core179_kernel_sdc_repro_init(struct test *test) {
    auto *data = static_cast<Core179ReproData *>(malloc(sizeof(Core179ReproData)));

    /* skeleton A buffers */
    data->srcA64    = static_cast<uint64x2_t *>(aligned_alloc(64, CORE179_REPRO_COUNT * sizeof(uint64x2_t)));
    data->srcB64    = static_cast<uint64x2_t *>(aligned_alloc(64, CORE179_REPRO_COUNT * sizeof(uint64x2_t)));
    data->scratch64 = static_cast<uint64x2_t *>(aligned_alloc(64, CORE179_REPRO_COUNT * sizeof(uint64x2_t)));
    data->expectedA = static_cast<uint64x2_t *>(aligned_alloc(64, CORE179_REPRO_COUNT * sizeof(uint64x2_t)));

    /* skeleton B buffers */
    data->srcA32    = static_cast<uint32x4_t *>(aligned_alloc(64, CORE179_REPRO_COUNT * sizeof(uint32x4_t)));
    data->srcB32    = static_cast<uint32x4_t *>(aligned_alloc(64, CORE179_REPRO_COUNT * sizeof(uint32x4_t)));
    data->expectedB = static_cast<uint32x4_t *>(aligned_alloc(64, CORE179_REPRO_COUNT * sizeof(uint32x4_t)));

    memset_random(data->srcA64,    CORE179_REPRO_COUNT * sizeof(uint64x2_t));
    memset_random(data->srcB64,    CORE179_REPRO_COUNT * sizeof(uint64x2_t));
    memset_random(data->scratch64, CORE179_REPRO_COUNT * sizeof(uint64x2_t));
    memset_random(data->srcA32,    CORE179_REPRO_COUNT * sizeof(uint32x4_t));
    memset_random(data->srcB32,    CORE179_REPRO_COUNT * sizeof(uint32x4_t));

    /* goldens (issue #373): scalar independent recomputation — pure C
     * integer/compare ops on the scalar pipeline, never the NEON units
     * under test, so a persistent NEON datapath fault cannot corrupt
     * golden and actual identically (the run side keeps both hot-loop
     * skeletons untouched — this test is the environment reproducer,
     * and its user-space miscompare channel now also sees persistent
     * NEON faults). Skeleton A: phase = vector index %4 (2 u64 lanes
     * per vector); skeleton B: phase = vector index %4 (4 u32 lanes per
     * vector); eq(s)/eq-u reduce to bit equality, gt is signed, tst is
     * (a & b) != 0. */
    {
        const uint64_t *sa = reinterpret_cast<const uint64_t *>(data->srcA64);
        const uint64_t *sb = reinterpret_cast<const uint64_t *>(data->srcB64);
        uint64_t       *ex = reinterpret_cast<uint64_t *>(data->expectedA);
        for (int e = 0; e < 2 * CORE179_REPRO_COUNT; e++) {
            switch ((e / 2) % 4) {
                case 0: ex[e] = sa[e] + sb[e]; break;
                case 1: ex[e] = sa[e] ^ sb[e]; break;
                case 2: ex[e] = sa[e] & sb[e]; break;
                case 3: ex[e] = sa[e] | sb[e]; break;
            }
        }
    }
    {
        const uint32_t *sa = reinterpret_cast<const uint32_t *>(data->srcA32);
        const uint32_t *sb = reinterpret_cast<const uint32_t *>(data->srcB32);
        uint32_t       *ex = reinterpret_cast<uint32_t *>(data->expectedB);
        for (int e = 0; e < 4 * CORE179_REPRO_COUNT; e++) {
            switch ((e / 4) % 4) {
                case 0: ex[e] = (sa[e] == sb[e]) ? 0xFFFFFFFFu : 0u; break;
                case 1: ex[e] = ((int32_t)sa[e] > (int32_t)sb[e]) ? 0xFFFFFFFFu : 0u; break;
                case 2: ex[e] = (sa[e] == sb[e]) ? 0xFFFFFFFFu : 0u; break;
                case 3: ex[e] = ((sa[e] & sb[e]) != 0u) ? 0xFFFFFFFFu : 0u; break;
            }
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int core179_kernel_sdc_repro_run(struct test *test, int cpu) {
    auto *data = static_cast<Core179ReproData *>(test->data);

    uint64x2_t *temp64 = static_cast<uint64x2_t *>(aligned_alloc(64, CORE179_REPRO_COUNT * sizeof(uint64x2_t)));
    uint64x2_t *dst64  = static_cast<uint64x2_t *>(aligned_alloc(64, CORE179_REPRO_COUNT * sizeof(uint64x2_t)));
    uint32x4_t *temp32 = static_cast<uint32x4_t *>(aligned_alloc(64, CORE179_REPRO_COUNT * sizeof(uint32x4_t)));
    uint32x4_t *dst32  = static_cast<uint32x4_t *>(aligned_alloc(64, CORE179_REPRO_COUNT * sizeof(uint32x4_t)));

    TEST_LOOP(test, 1 << 13) {
        /* ---- skeleton A: neon_rot_ldr_at_top ----
         * 3rd ldr at iteration top (value discarded), 2 source loads,
         * rotating ALU, back-to-back stores with NO memory op between them */
        for (int i = 0; i < CORE179_REPRO_COUNT; i++) {
            uint64x2_t unused_x = load_vec64(&data->scratch64[i]);
            (void)unused_x;

            uint64x2_t a = load_vec64(&data->srcA64[i]);
            uint64x2_t b = load_vec64(&data->srcB64[i]);
            uint64x2_t res;

            switch (i % 4) {
                case 0: res = vaddq_u64(a, b); break;
                case 1: res = veorq_u64(a, b); break;
                case 2: res = vandq_u64(a, b); break;
                case 3: res = vorrq_u64(a, b); break;
            }

            store_vec64(&temp64[i], res);
            store_vec64(&dst64[i], res);
        }

        if (memcmp(dst64, data->expectedA, CORE179_REPRO_COUNT * sizeof(uint64x2_t)) != 0) {
            report_fail_msg("core179_kernel_sdc_repro skeleton-A (ldr_at_top) data miscompare");
        }

        /* ---- skeleton B: arm0102_kreg_mask ----
         * 2 source loads, rotating sparse compare masks,
         * back-to-back str -> ldr -> str */
        for (int i = 0; i < CORE179_REPRO_COUNT; i++) {
            uint32x4_t a = load_vec32(&data->srcA32[i]);
            uint32x4_t b = load_vec32(&data->srcB32[i]);
            uint32x4_t res;

            switch (i % 4) {
                case 0: res = vceqq_s32(vreinterpretq_s32_u32(a), vreinterpretq_s32_u32(b)); break;
                case 1: res = vcgtq_s32(vreinterpretq_s32_u32(a), vreinterpretq_s32_u32(b)); break;
                case 2: res = vceqq_u32(a, b); break;
                case 3: res = vtstq_u32(a, b); break;
            }

            store_vec32(&temp32[i], res);
            store_vec32(&dst32[i], load_vec32(&temp32[i]));
        }

        if (memcmp(dst32, data->expectedB, CORE179_REPRO_COUNT * sizeof(uint32x4_t)) != 0) {
            report_fail_msg("core179_kernel_sdc_repro skeleton-B (kreg_mask) data miscompare");
        }
    }

    free(dst32);
    free(temp32);
    free(dst64);
    free(temp64);
    return EXIT_SUCCESS;
}

static int core179_kernel_sdc_repro_cleanup(struct test *test) {
    auto *data = static_cast<Core179ReproData *>(test->data);
    if (data) {
        free(data->srcA64);
        free(data->srcB64);
        free(data->scratch64);
        free(data->expectedA);
        free(data->srcA32);
        free(data->srcB32);
        free(data->expectedB);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(core179_kernel_sdc_repro, "Core-179 kernel-space SDC reproducer: full-core dual-recipe load (neon_rot_ldr_at_top + arm0102_kreg_mask skeletons); user-space miscompare via fail channel, kernel-space errors observable in journalctl/vmcore")
    .test_init    = core179_kernel_sdc_repro_init,
    .test_run     = core179_kernel_sdc_repro_run,
    .test_cleanup = core179_kernel_sdc_repro_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
