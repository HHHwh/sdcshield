/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0.
 *
 * @test @b arm0102_kreg_mask_dump
 * @parblock
 * Dump-on-failure variant of arm0102_kreg_mask (2026-09-08).
 *
 * Same trigger skeleton as the parent (2 source loads + rotating sparse
 * compare masks (cmeq s32 / cmgt s32 / cmeq u32 / cmtst u32) + back-to-back
 * str -> ldr -> str), hot loop byte-identical, so the SDC trigger rate is
 * unchanged. On memcmp mismatch the cold path logs, per mismatching index,
 * srcA/srcB/expected/actual/xor plus a binary record to
 * /var/tmp/arm0102_kreg_mask_dump_<pid>.bin (movbe_dump design; on-disk
 * location per the 09-08 panic lesson: /tmp is tmpfs and loses records on
 * crash).
 *
 * 2026-09-08 note: in the 3h excl-180 run this skeleton produced 0 fails
 * (neon_rot_ldr_at_top took all 114), so this dump variant exists to catch
 * its first recorded wrong values whenever it does fire -- the parent test
 * has historically triggered (kreg-mask-4th-freeze memory: vceq/vcgt/vtst
 * sparse-mask compare class was implicated in the 09-04 22:38 freeze).
 *
 * Record fields mirror neon_rot_ldr_at_top_dump but in u32x4 domain:
 * timestamp_us, cpu, iteration, index, op (0=cmeq_s32,1=cmgt_s32,
 * 2=cmeq_u32,3=cmtst_u32), srcA/srcB/exp/act/xor as 4x u32 each.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define ARM0102_KREG_MASK_DUMP_COUNT 1024

struct Arm0102KregMaskDumpData {
    uint32x4_t *srcA;
    uint32x4_t *srcB;
    uint32x4_t *expected;
    int dump_fd;
};

#pragma pack(push, 1)
struct arm0102_kreg_mask_dump_record {
    uint64_t timestamp_us;
    uint32_t cpu;
    uint32_t iteration;
    uint32_t index;
    uint32_t op;             /* 0=cmeq_s32 1=cmgt_s32 2=cmeq_u32 3=cmtst */
    uint32_t srcA[4];
    uint32_t srcB[4];
    uint32_t exp[4];
    uint32_t act[4];
    uint32_t xorv[4];
};
#pragma pack(pop)

#if defined(__aarch64__)

static inline void store_vec(void *addr, uint32x4_t val) {
    vst1q_u32(static_cast<uint32_t *>(addr), val);
}

static inline uint32x4_t load_vec(const void *addr) {
    uint32x4_t res;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

/* cold-path helper: full-vector equality test */
static inline bool vec32_eq(uint32x4_t x, uint32x4_t y) {
    return (vgetq_lane_u32(x,0) == vgetq_lane_u32(y,0)) &&
           (vgetq_lane_u32(x,1) == vgetq_lane_u32(y,1)) &&
           (vgetq_lane_u32(x,2) == vgetq_lane_u32(y,2)) &&
           (vgetq_lane_u32(x,3) == vgetq_lane_u32(y,3));
}

#endif

static int arm0102_kreg_mask_dump_init(struct test *test) {
    auto *data = static_cast<Arm0102KregMaskDumpData *>(malloc(sizeof(Arm0102KregMaskDumpData)));
    data->srcA = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_KREG_MASK_DUMP_COUNT * sizeof(uint32x4_t)));
    data->srcB = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_KREG_MASK_DUMP_COUNT * sizeof(uint32x4_t)));
    data->expected = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_KREG_MASK_DUMP_COUNT * sizeof(uint32x4_t)));

    memset_random(data->srcA, ARM0102_KREG_MASK_DUMP_COUNT * sizeof(uint32x4_t));
    memset_random(data->srcB, ARM0102_KREG_MASK_DUMP_COUNT * sizeof(uint32x4_t));

    /* golden: same rotating composition, computed on the same NEON units */
    for (int i = 0; i < ARM0102_KREG_MASK_DUMP_COUNT; i++) {
        switch (i % 4) {
            case 0: data->expected[i] = vceqq_s32(vreinterpretq_s32_u32(data->srcA[i]),
                                                  vreinterpretq_s32_u32(data->srcB[i])); break;
            case 1: data->expected[i] = vcgtq_s32(vreinterpretq_s32_u32(data->srcA[i]),
                                                  vreinterpretq_s32_u32(data->srcB[i])); break;
            case 2: data->expected[i] = vceqq_u32(data->srcA[i], data->srcB[i]); break;
            case 3: data->expected[i] = vtstq_u32(data->srcA[i], data->srcB[i]); break;
        }
    }

    char path[256];
    snprintf(path, sizeof(path), "/var/tmp/arm0102_kreg_mask_dump_%d.bin", getpid());
    data->dump_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (data->dump_fd < 0) {
        log_error("arm0102_kreg_mask_dump: failed to open dump log %s", path);
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int arm0102_kreg_mask_dump_run(struct test *test, int cpu) {
    auto *data = static_cast<Arm0102KregMaskDumpData *>(test->data);

    uint32x4_t *temp = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_KREG_MASK_DUMP_COUNT * sizeof(uint32x4_t)));
    uint32x4_t *dst  = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_KREG_MASK_DUMP_COUNT * sizeof(uint32x4_t)));

    uint32_t iter = 0;
    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < ARM0102_KREG_MASK_DUMP_COUNT; i++) {
            uint32x4_t a = load_vec(&data->srcA[i]);   /* ldr q — source 1 */
            uint32x4_t b = load_vec(&data->srcB[i]);   /* ldr q — source 2 */
            uint32x4_t res;

            switch (i % 4) {                            /* payload rotation */
                case 0: res = vceqq_s32(vreinterpretq_s32_u32(a), vreinterpretq_s32_u32(b)); break; /* eq */
                case 1: res = vcgtq_s32(vreinterpretq_s32_u32(a), vreinterpretq_s32_u32(b)); break; /* gt */
                case 2: res = vceqq_u32(a, b); break;                                              /* eq-u */
                case 3: res = vtstq_u32(a, b); break;                                              /* tst */
            }

            store_vec(&temp[i], res);
            store_vec(&dst[i], load_vec(&temp[i]));     /* back-to-back str->ldr->str */
        }

        /* cold path: single memcmp per iteration (identical to parent's
         * per-iteration cost); the per-index scan + dump runs ONLY inside
         * the mismatch branch */
        if (memcmp(dst, data->expected, ARM0102_KREG_MASK_DUMP_COUNT * sizeof(uint32x4_t)) != 0) {
            for (int i = 0; i < ARM0102_KREG_MASK_DUMP_COUNT; i++) {
                if (!vec32_eq(dst[i], data->expected[i])) {
                    uint32x4_t a = load_vec(&data->srcA[i]);
                    uint32x4_t b = load_vec(&data->srcB[i]);
                    uint32x4_t exp = data->expected[i];
                    uint32x4_t act = dst[i];
                    uint32x4_t x = veorq_u32(exp, act);

                    log_error("arm0102_kreg_mask_dump: idx %u op %u cpu %u: "
                              "srcA=%08x%08x%08x%08x srcB=%08x%08x%08x%08x "
                              "exp=%08x%08x%08x%08x act=%08x%08x%08x%08x "
                              "xor=%08x%08x%08x%08x",
                              (unsigned)i, (unsigned)(i % 4),
                              (unsigned)device_info[cpu].cpu_number, /* logical CPU */
                              vgetq_lane_u32(a,3), vgetq_lane_u32(a,2), vgetq_lane_u32(a,1), vgetq_lane_u32(a,0),
                              vgetq_lane_u32(b,3), vgetq_lane_u32(b,2), vgetq_lane_u32(b,1), vgetq_lane_u32(b,0),
                              vgetq_lane_u32(exp,3), vgetq_lane_u32(exp,2), vgetq_lane_u32(exp,1), vgetq_lane_u32(exp,0),
                              vgetq_lane_u32(act,3), vgetq_lane_u32(act,2), vgetq_lane_u32(act,1), vgetq_lane_u32(act,0),
                              vgetq_lane_u32(x,3), vgetq_lane_u32(x,2), vgetq_lane_u32(x,1), vgetq_lane_u32(x,0));

                    if (data->dump_fd >= 0) {
                        struct timeval tv;
                        gettimeofday(&tv, nullptr);
                        struct arm0102_kreg_mask_dump_record rec = {
                            .timestamp_us = (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec,
                            .cpu = (uint32_t)device_info[cpu].cpu_number, /* cpu param is thread_num; translate to logical CPU */
                            .iteration = iter,
                            .index = (uint32_t)i,
                            .op = (uint32_t)(i % 4),
                        };
                        for (int l = 0; l < 4; l++) {
                            rec.srcA[l] = vgetq_lane_u32(a, l);
                            rec.srcB[l] = vgetq_lane_u32(b, l);
                            rec.exp[l]  = vgetq_lane_u32(exp, l);
                            rec.act[l]  = vgetq_lane_u32(act, l);
                            rec.xorv[l] = vgetq_lane_u32(x, l);
                        }
                        if (write(data->dump_fd, &rec, sizeof(rec)) < 0) { /* best effort */ }
                    }
                }
            }
            report_fail_msg("arm0102_kreg_mask_dump data miscompare (records in dump bin)");
        }
        iter++;
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int arm0102_kreg_mask_dump_cleanup(struct test *test) {
    auto *data = static_cast<Arm0102KregMaskDumpData *>(test->data);
    if (data) {
        if (data->dump_fd >= 0) close(data->dump_fd);
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(arm0102_kreg_mask_dump, "Dump variant of arm0102_kreg_mask: same hot loop (2-src + rotating compare masks + str/ldr/str); on fail logs srcA/srcB/exp/act/xor per index + binary records")
    .test_init    = arm0102_kreg_mask_dump_init,
    .test_run     = arm0102_kreg_mask_dump_run,
    .test_cleanup = arm0102_kreg_mask_dump_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
