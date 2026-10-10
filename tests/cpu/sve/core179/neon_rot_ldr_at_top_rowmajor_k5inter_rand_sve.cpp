/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve
 * @parblock
 * SVE port of neon_rot_ldr_at_top_rowmajor_k5inter_rand (source:
 * missing_testcases_20261009/tests/cpu/misc/
 * neon_rot_ldr_at_top_rowmajor_k5inter_rand.cpp).
 *
 * K5 layout CONTROL cell: the interleaved dual-source layout of k5far
 * (src[2i]/src[2i+1] in ONE buffer) but with NATURAL random data
 * (memset_random) instead of GPS tags. Isolates the LAYOUT effect from
 * the TAG effect:
 *   k5far vs this  -> layout effect;
 *   this vs the plain-random parent -> layout-without-tags.
 * Doubles as the dump-capable A-track of the 0.5-phase classification
 * (rate readout + coarse classification only — no tags, no fine
 * classification). Five-buffer dump (viewA/viewB materialized) on fail.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <sys/auxv.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif
#endif

#define NEON_ROT_LDR_AT_TOP_ROWMAJOR_K5IR_SVE_SLOTS 1024
#define K5IR_SVE_DUMP_DIR "movbe_log/gps_rowmajor"

struct NeonRotLdrAtTopRowmajorK5irSveData {
    uint64_t *src;        /* 2 x SLOTS vectors, interleaved dual source */
    uint64_t *scratch;
    uint64_t *expected;
    int lanes;
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

#endif

static int neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve_init(struct test *test) {
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve");
        return EXIT_SKIP;
    }

    const int lanes = (int)svcntd();
    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_ROWMAJOR_K5IR_SVE_SLOTS * lanes;

    auto *data = static_cast<NeonRotLdrAtTopRowmajorK5irSveData *>(malloc(sizeof(NeonRotLdrAtTopRowmajorK5irSveData)));
    data->lanes = lanes;
    data->src = static_cast<uint64_t *>(aligned_alloc(64, 2 * elems * sizeof(uint64_t)));
    data->scratch = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    /* 自然随机填充 (布局控制: 与 k5far 唯一差异 = 无标签) */
    memset_random(data->src, 2 * elems * sizeof(uint64_t));
    memset_random(data->scratch, elems * sizeof(uint64_t));

    svbool_t pg = svptrue_b64();
    for (int i = 0; i < NEON_ROT_LDR_AT_TOP_ROWMAJOR_K5IR_SVE_SLOTS; i++) {
        svuint64_t a = svld1_u64(pg, data->src + (size_t)(2 * i) * lanes);
        svuint64_t b = svld1_u64(pg, data->src + (size_t)(2 * i + 1) * lanes);
        svuint64_t res;
        switch (i % 4) {
            case 0: res = svadd_u64_x(pg, a, b); break;
            case 1: res = sveor_u64_x(pg, a, b); break;
            case 2: res = svand_u64_x(pg, a, b); break;
            case 3: res = svorr_u64_x(pg, a, b); break;
        }
        svst1_u64(pg, data->expected + (size_t)i * lanes, res);
    }

    mkdir(K5IR_SVE_DUMP_DIR, 0755);

    test->data = data;
    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)
static int neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<NeonRotLdrAtTopRowmajorK5irSveData *>(test->data);

    if ((int)svcntd() != data->lanes) {
        log_error("neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve: vector length changed between init (%d) and run (%d)",
                  data->lanes, (int)svcntd());
        report_fail_msg("neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve: vector length changed between init and run");
    }

    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_ROWMAJOR_K5IR_SVE_SLOTS * data->lanes;
    const size_t bytes = elems * sizeof(uint64_t);
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, bytes));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, bytes));

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 13) {
        const bool ascending = (sweep & 1) == 0;
        for (int n = 0; n < NEON_ROT_LDR_AT_TOP_ROWMAJOR_K5IR_SVE_SLOTS; n++) {
            const int i = ascending ? n : (NEON_ROT_LDR_AT_TOP_ROWMAJOR_K5IR_SVE_SLOTS - 1 - n);
            const size_t off = (size_t)i * data->lanes;

            svuint64_t unused_x = load_vec_sve(data->scratch + off);
            (void)unused_x;

            svuint64_t a = load_vec_sve(data->src + (size_t)(2 * i) * data->lanes);
            svuint64_t b = load_vec_sve(data->src + (size_t)(2 * i + 1) * data->lanes);
            svuint64_t res;

            switch (i % 4) {
                case 0: res = svadd_u64_x(svptrue_b64(), a, b); break;
                case 1: res = sveor_u64_x(svptrue_b64(), a, b); break;
                case 2: res = svand_u64_x(svptrue_b64(), a, b); break;
                case 3: res = svorr_u64_x(svptrue_b64(), a, b); break;
            }

            store_vec_sve(temp + off, res);
            store_vec_sve(dst + off, res);
        }
        sweep++;

        if (memcmp(dst, data->expected, bytes) != 0) {
            uint64_t *viewA = static_cast<uint64_t *>(aligned_alloc(64, bytes));
            uint64_t *viewB = static_cast<uint64_t *>(aligned_alloc(64, bytes));
            for (int i = 0; i < NEON_ROT_LDR_AT_TOP_ROWMAJOR_K5IR_SVE_SLOTS; i++) {
                memcpy(viewA + (size_t)i * data->lanes,
                       data->src + (size_t)(2 * i) * data->lanes, bytes / NEON_ROT_LDR_AT_TOP_ROWMAJOR_K5IR_SVE_SLOTS);
                memcpy(viewB + (size_t)i * data->lanes,
                       data->src + (size_t)(2 * i + 1) * data->lanes, bytes / NEON_ROT_LDR_AT_TOP_ROWMAJOR_K5IR_SVE_SLOTS);
            }
            char path[256];
            snprintf(path, sizeof(path),
                     "%s/k5ir_sve_fail_cpu%d_sweep%lu.bin",
                     K5IR_SVE_DUMP_DIR, device_info[cpu].cpu_number, sweep - 1);
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                const char *bufs[5] = {
                    (const char *)dst, (const char *)data->expected,
                    (const char *)viewA, (const char *)viewB,
                    (const char *)data->scratch,
                };
                for (int k = 0; k < 5; ++k) {
                    ssize_t total = 0;
                    while ((size_t)total < bytes) {
                        ssize_t w = write(fd, bufs[k] + total, bytes - (size_t)total);
                        if (w <= 0) break;
                        total += w;
                    }
                }
                char tail[64];
                int len = snprintf(tail, sizeof(tail), "k5inter_rand sweep=%lu\n", sweep - 1);
                if (write(fd, tail, (size_t)len) < 0) { /* best effort */ }
                close(fd);
            }
            free(viewB);
            free(viewA);
            report_fail_msg("neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve");
    return EXIT_SKIP;
}
#endif

static int neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotLdrAtTopRowmajorK5irSveData *>(test->data);
    if (data) {
        free(data->src);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve,
             "SVE K5 layout control: interleaved dual-source layout with NATURAL random data "
             "(no tags) — isolates the layout effect from the tag effect; dump-capable A-track "
             "(port of neon_rot_ldr_at_top_rowmajor_k5inter_rand)")
    .test_init    = neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve_init,
    .test_run     = neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve_run,
    .test_cleanup = neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
