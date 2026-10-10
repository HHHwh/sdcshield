/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_at_top_rowmajor_k3res_sve
 * @parblock
 * SVE port of neon_rot_ldr_at_top_rowmajor_k3res (source:
 * missing_testcases_20261009/tests/cpu/misc/
 * neon_rot_ldr_at_top_rowmajor_k3res.cpp).
 *
 * K3 residency cell: the GPS-tagged chassis at 4x the champion
 * footprint — 4096 slots (the champion's 1024 slots x 4; at VL=256 each
 * buffer is 4096*4*8 = 128 KB, vs the scalar original's 64 KB).
 * Pre-registered readouts: rate collapses -> footprint is in the
 * recipe; rate holds + tight neighbour histogram -> uninformative-for-
 * residency negative; rate holds + far sources appear -> far/evicted
 * sources participate, L1D-internal selection disfavoured.
 *
 * GPS tags, parallel srcA/srcB layout (NOT interleaved), rowmajor
 * skeleton, five-buffer dump on fail.
 *
 * Deviation (documented): at VL=256 the per-buffer footprint doubles vs
 * the scalar original (128 KB vs 64 KB), so byte_off EXCEEDS the 16-bit
 * tag field; the field WRAPS (byte_off & 0xFFFF, complement taken of
 * the wrapped value so the integrity check stays self-consistent).
 * The wrap is deterministic and decodable offline; source-buffer
 * identity (the magic field) is unaffected.
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

#define NEON_ROT_LDR_AT_TOP_ROWMAJOR_K3RES_SVE_SLOTS 4096   /* 4x champion footprint */
#define K3RES_SVE_DUMP_DIR "movbe_log/gps_rowmajor"

#define K3RES_SVE_MAGIC_A   0xA5A5ULL
#define K3RES_SVE_MAGIC_B   0x5A5AULL
#define K3RES_SVE_MAGIC_SCR 0x0F0FULL

static inline uint64_t k3res_sve_tag(uint64_t magic, uint64_t byte_off, uint64_t gen)
{
    const uint64_t off = byte_off & 0xFFFFULL;   /* 16 位字段回绕 (见头注释) */
    return (magic << 48) | (off << 32) | ((~off & 0xFFFFULL) << 16) | (gen & 0xFFFFULL);
}

struct NeonRotLdrAtTopRowmajorK3resSveData {
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

static int neon_rot_ldr_at_top_rowmajor_k3res_sve_init(struct test *test) {
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_k3res_sve");
        return EXIT_SKIP;
    }

    const int lanes = (int)svcntd();
    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_ROWMAJOR_K3RES_SVE_SLOTS * lanes;

    auto *data = static_cast<NeonRotLdrAtTopRowmajorK3resSveData *>(malloc(sizeof(NeonRotLdrAtTopRowmajorK3resSveData)));
    data->lanes = lanes;
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->scratch = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    for (size_t j = 0; j < elems; j++) {
        data->srcA[j]    = k3res_sve_tag(K3RES_SVE_MAGIC_A, (uint64_t)j * 8, 0);
        data->srcB[j]    = k3res_sve_tag(K3RES_SVE_MAGIC_B, (uint64_t)j * 8, 0);
        data->scratch[j] = k3res_sve_tag(K3RES_SVE_MAGIC_SCR, (uint64_t)j * 8, 0);
    }

    svbool_t pg = svptrue_b64();
    for (int i = 0; i < NEON_ROT_LDR_AT_TOP_ROWMAJOR_K3RES_SVE_SLOTS; i++) {
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

    mkdir(K3RES_SVE_DUMP_DIR, 0755);

    test->data = data;
    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_k3res_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)
static int neon_rot_ldr_at_top_rowmajor_k3res_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<NeonRotLdrAtTopRowmajorK3resSveData *>(test->data);

    if ((int)svcntd() != data->lanes) {
        log_error("neon_rot_ldr_at_top_rowmajor_k3res_sve: vector length changed between init (%d) and run (%d)",
                  data->lanes, (int)svcntd());
        report_fail_msg("neon_rot_ldr_at_top_rowmajor_k3res_sve: vector length changed between init and run");
    }

    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_ROWMAJOR_K3RES_SVE_SLOTS * data->lanes;
    const size_t bytes = elems * sizeof(uint64_t);
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, bytes));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, bytes));

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 13) {
        const bool ascending = (sweep & 1) == 0;
        for (int n = 0; n < NEON_ROT_LDR_AT_TOP_ROWMAJOR_K3RES_SVE_SLOTS; n++) {
            const int i = ascending ? n : (NEON_ROT_LDR_AT_TOP_ROWMAJOR_K3RES_SVE_SLOTS - 1 - n);
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
            char path[256];
            snprintf(path, sizeof(path),
                     "%s/k3res_sve_fail_cpu%d_sweep%lu.bin",
                     K3RES_SVE_DUMP_DIR, device_info[cpu].cpu_number, sweep - 1);
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                const char *bufs[5] = {
                    (const char *)dst, (const char *)data->expected,
                    (const char *)data->srcA, (const char *)data->srcB,
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
                int len = snprintf(tail, sizeof(tail), "k3res sweep=%lu\n", sweep - 1);
                if (write(fd, tail, (size_t)len) < 0) { /* best effort */ }
                close(fd);
            }
            report_fail_msg("neon_rot_ldr_at_top_rowmajor_k3res_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int neon_rot_ldr_at_top_rowmajor_k3res_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_k3res_sve");
    return EXIT_SKIP;
}
#endif

static int neon_rot_ldr_at_top_rowmajor_k3res_sve_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotLdrAtTopRowmajorK3resSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_at_top_rowmajor_k3res_sve,
             "SVE K3 residency cell: 4x-champion footprint (4096 slots) GPS-tagged chassis — "
             "exposes whether field-substitution sources are L1D-resident at failure time; "
             "byte_off tag field wraps at 64KB (port of neon_rot_ldr_at_top_rowmajor_k3res)")
    .test_init    = neon_rot_ldr_at_top_rowmajor_k3res_sve_init,
    .test_run     = neon_rot_ldr_at_top_rowmajor_k3res_sve_run,
    .test_cleanup = neon_rot_ldr_at_top_rowmajor_k3res_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
