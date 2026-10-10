/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_at_top_rowmajor_gps_sve
 * @parblock
 * SVE port of neon_rot_ldr_at_top_rowmajor_gps (source:
 * missing_testcases_20261009/tests/cpu/misc/
 * neon_rot_ldr_at_top_rowmajor_gps.cpp).
 *
 * GPS-tagged chassis of the rowmajor position probe: every 64-bit word
 * self-reports its source — tag = magic(16b) | byte_off(16b) |
 * ~byte_off(16b) | gen(16b) — so a captured fail pinpoints which buffer
 * and which byte offset each corrupted word CAME from (the scalar
 * campaign's dominant signature was same-buffer field substitution from
 * a neighbouring srcA word, distance ±8..±28 elements). Magics:
 * srcA=0xA5A5, srcB=0x5A5A (complementary — a two-source splice is
 * visible inside one word), scratch=0x0F0F. Generation held at 0
 * (rotating it would dirty the source lines and break the init golden).
 *
 * DETERMINISTIC fill — no RNG (auto-seed irrelevant). The hot loop is
 * the rowmajor skeleton (dead scratch ldr z at top, 2 sources, rotating
 * add/eor/and/orr per slot, back-to-back store pair, alternating
 * sweeps). Cold path: five-buffer snapshot dump (dst, expected, srcA,
 * srcB, scratch) + trailing "cpu=%d sweep=%lu" line to
 * movbe_log/gps_rowmajor/ (CWD-relative, kept from the original;
 * classification is offline).
 *
 * Note: byte_off wraps at 64KB (16-bit field) — no wrap here (buffers
 * are SLOTS*lanes*8 = 32KB at VL=256); the k3res sibling documents the
 * wrap explicitly.
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

#define NEON_ROT_LDR_AT_TOP_ROWMAJOR_GPS_SVE_SLOTS 1024
#define GPS_SVE_DUMP_DIR "movbe_log/gps_rowmajor"

#define GPS_SVE_MAGIC_A   0xA5A5ULL
#define GPS_SVE_MAGIC_B   0x5A5AULL
#define GPS_SVE_MAGIC_SCR 0x0F0FULL

static inline uint64_t gps_sve_tag(uint64_t magic, uint64_t byte_off, uint64_t gen)
{
    return (magic << 48)
         | ((byte_off & 0xFFFFULL) << 32)
         | ((~byte_off & 0xFFFFULL) << 16)
         | (gen & 0xFFFFULL);
}

struct NeonRotLdrAtTopRowmajorGpsSveData {
    uint64_t *srcA;
    uint64_t *srcB;
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

static int neon_rot_ldr_at_top_rowmajor_gps_sve_init(struct test *test) {
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_gps_sve");
        return EXIT_SKIP;
    }

    const int lanes = (int)svcntd();
    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_ROWMAJOR_GPS_SVE_SLOTS * lanes;

    auto *data = static_cast<NeonRotLdrAtTopRowmajorGpsSveData *>(malloc(sizeof(NeonRotLdrAtTopRowmajorGpsSveData)));
    data->lanes = lanes;
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->scratch = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    /* GPS 标签填充 (确定性, 无 RNG): 逐 u64 词, byte_off = 词内偏移 */
    for (size_t j = 0; j < elems; j++) {
        data->srcA[j]    = gps_sve_tag(GPS_SVE_MAGIC_A, (uint64_t)j * 8, 0);
        data->srcB[j]    = gps_sve_tag(GPS_SVE_MAGIC_B, (uint64_t)j * 8, 0);
        data->scratch[j] = gps_sve_tag(GPS_SVE_MAGIC_SCR, (uint64_t)j * 8, 0);
    }

    /* golden: 从标签数据按父系旋转预计算 (标签流经 ALU) */
    svbool_t pg = svptrue_b64();
    for (int i = 0; i < NEON_ROT_LDR_AT_TOP_ROWMAJOR_GPS_SVE_SLOTS; i++) {
        svuint64_t a = svld1_u64(pg, data->srcA + (size_t)i * lanes);
        svuint64_t b = svld1_u64(pg, data->srcB + (size_t)i * lanes);
        svuint64_t res;
        switch (i % 4) {
            case 0: res = svadd_u64_x(pg, a, b); break;
            case 1: res = sveor_u64_x(pg, a, b); break;
            case 2: res = svand_u64_x(pg, a, b); break;
            case 3: res = svorr_u64_x(pg, a, b); break;
        }
        svst1_u64(pg, data->expected + (size_t)i * lanes, res);
    }

    mkdir(GPS_SVE_DUMP_DIR, 0755);   /* EEXIST is fine (原版同款) */

    test->data = data;
    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_gps_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)
static int neon_rot_ldr_at_top_rowmajor_gps_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<NeonRotLdrAtTopRowmajorGpsSveData *>(test->data);

    if ((int)svcntd() != data->lanes) {
        log_error("neon_rot_ldr_at_top_rowmajor_gps_sve: vector length changed between init (%d) and run (%d)",
                  data->lanes, (int)svcntd());
        report_fail_msg("neon_rot_ldr_at_top_rowmajor_gps_sve: vector length changed between init and run");
    }

    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_ROWMAJOR_GPS_SVE_SLOTS * data->lanes;
    const size_t bytes = elems * sizeof(uint64_t);
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, bytes));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, bytes));

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 13) {
        const bool ascending = (sweep & 1) == 0;
        for (int n = 0; n < NEON_ROT_LDR_AT_TOP_ROWMAJOR_GPS_SVE_SLOTS; n++) {
            const int i = ascending ? n : (NEON_ROT_LDR_AT_TOP_ROWMAJOR_GPS_SVE_SLOTS - 1 - n);
            const size_t off = (size_t)i * data->lanes;

            svuint64_t unused_x = load_vec_sve(data->scratch + off);
            (void)unused_x;

            svuint64_t a = load_vec_sve(data->srcA + off);
            svuint64_t b = load_vec_sve(data->srcB + off);
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
            /* 冷路径: 五缓冲快照 + 尾行 (分类离线进行, 原版纪律) */
            char path[256];
            snprintf(path, sizeof(path),
                     "%s/neon_rot_ldr_at_top_rowmajor_gps_sve_fail_cpu%d_sweep%lu.bin",
                     GPS_SVE_DUMP_DIR, device_info[cpu].cpu_number, sweep - 1);
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                ssize_t total = 0;
                const char *bufs[5] = {
                    (const char *)dst, (const char *)data->expected,
                    (const char *)data->srcA, (const char *)data->srcB,
                    (const char *)data->scratch,
                };
                for (int k = 0; k < 5; ++k) {
                    total = 0;
                    while ((size_t)total < bytes) {
                        ssize_t w = write(fd, bufs[k] + total, bytes - (size_t)total);
                        if (w <= 0) break;
                        total += w;
                    }
                }
                char tail[64];
                int len = snprintf(tail, sizeof(tail), "cpu=%d sweep=%lu\n",
                                   device_info[cpu].cpu_number, sweep - 1);
                if (write(fd, tail, (size_t)len) < 0) { /* best effort */ }
                close(fd);
            } else {
                log_error("neon_rot_ldr_at_top_rowmajor_gps_sve: cannot open dump %s (errno %d)",
                          path, errno);
            }
            report_fail_msg("neon_rot_ldr_at_top_rowmajor_gps_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int neon_rot_ldr_at_top_rowmajor_gps_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_gps_sve");
    return EXIT_SKIP;
}
#endif

static int neon_rot_ldr_at_top_rowmajor_gps_sve_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotLdrAtTopRowmajorGpsSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_at_top_rowmajor_gps_sve,
             "SVE GPS-tagged chassis of the rowmajor position probe: every 64b word self-reports "
             "its source (magic|byte_off|~byte_off|gen); five-buffer dump on fail "
             "(port of neon_rot_ldr_at_top_rowmajor_gps)")
    .test_init    = neon_rot_ldr_at_top_rowmajor_gps_sve_init,
    .test_run     = neon_rot_ldr_at_top_rowmajor_gps_sve_run,
    .test_cleanup = neon_rot_ldr_at_top_rowmajor_gps_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
