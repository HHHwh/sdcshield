/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_at_top_rowmajor_dualcmp_sve
 * @parblock
 * SVE port of neon_rot_ldr_at_top_rowmajor_dualcmp (source:
 * missing_testcases_20261009/tests/cpu/misc/
 * neon_rot_ldr_at_top_rowmajor_dualcmp.cpp).
 *
 * Dual-store comparison chassis of the rowmajor position probe: the
 * baseline skeleton stores res to temp (1st leg) and dst (2nd leg)
 * back-to-back; verification checks BOTH store legs against the golden
 * AND their mutual consistency, then classifies:
 *   a_only  — dst wrong, temp right   -> 2nd store leg
 *   b_only  — temp wrong, dst right   -> 1st store leg
 *   ab_div  — temp != dst             -> stores diverged (at/after the fork)
 *   ab_same — temp == dst != golden   -> upstream (load/ALU/bypass)
 * The within-run A/B comparison is immune to the layout confound (both
 * legs share one loop/sweep/timing). Cold path dumps the five buffers +
 * stats line to movbe_log/gps_rowmajor/ (CWD-relative, original's dir).
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

#define NEON_ROT_LDR_AT_TOP_ROWMAJOR_DUALCMP_SVE_SLOTS 1024
#define DUALCMP_SVE_DUMP_DIR "movbe_log/gps_rowmajor"

struct NeonRotLdrAtTopRowmajorDualcmpSveData {
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

/* 冷路径分类: 标量逐元素 (性能无关紧要) */
struct DualcmpSveStats {
    uint64_t a_only, b_only, ab_div, ab_same;
    size_t first_bad;
};

static void dualcmp_sve_classify(const uint64_t *dst, const uint64_t *temp,
                                 const uint64_t *expected, size_t elems,
                                 DualcmpSveStats *st)
{
    st->a_only = st->b_only = st->ab_div = st->ab_same = 0;
    st->first_bad = (size_t)-1;
    for (size_t j = 0; j < elems; j++) {
        const bool dst_bad = dst[j] != expected[j];
        const bool tmp_bad = temp[j] != expected[j];
        if (!dst_bad && !tmp_bad) continue;
        if (st->first_bad == (size_t)-1) st->first_bad = j;
        if (tmp_bad && dst_bad) {
            if (temp[j] != dst[j]) st->ab_div++;
            else st->ab_same++;
        } else if (dst_bad) {
            st->a_only++;
        } else {
            st->b_only++;
        }
    }
}

#endif

static int neon_rot_ldr_at_top_rowmajor_dualcmp_sve_init(struct test *test) {
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_dualcmp_sve");
        return EXIT_SKIP;
    }

    const int lanes = (int)svcntd();
    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_ROWMAJOR_DUALCMP_SVE_SLOTS * lanes;

    auto *data = static_cast<NeonRotLdrAtTopRowmajorDualcmpSveData *>(malloc(sizeof(NeonRotLdrAtTopRowmajorDualcmpSveData)));
    data->lanes = lanes;
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->scratch = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    memset_random(data->srcA, elems * sizeof(uint64_t));
    memset_random(data->srcB, elems * sizeof(uint64_t));
    memset_random(data->scratch, elems * sizeof(uint64_t));

    svbool_t pg = svptrue_b64();
    for (int i = 0; i < NEON_ROT_LDR_AT_TOP_ROWMAJOR_DUALCMP_SVE_SLOTS; i++) {
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

    mkdir(DUALCMP_SVE_DUMP_DIR, 0755);   /* EEXIST is fine */

    test->data = data;
    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_dualcmp_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)
static int neon_rot_ldr_at_top_rowmajor_dualcmp_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<NeonRotLdrAtTopRowmajorDualcmpSveData *>(test->data);

    if ((int)svcntd() != data->lanes) {
        log_error("neon_rot_ldr_at_top_rowmajor_dualcmp_sve: vector length changed between init (%d) and run (%d)",
                  data->lanes, (int)svcntd());
        report_fail_msg("neon_rot_ldr_at_top_rowmajor_dualcmp_sve: vector length changed between init and run");
    }

    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_ROWMAJOR_DUALCMP_SVE_SLOTS * data->lanes;
    const size_t bytes = elems * sizeof(uint64_t);
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, bytes));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, bytes));

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 13) {
        const bool ascending = (sweep & 1) == 0;
        for (int n = 0; n < NEON_ROT_LDR_AT_TOP_ROWMAJOR_DUALCMP_SVE_SLOTS; n++) {
            const int i = ascending ? n : (NEON_ROT_LDR_AT_TOP_ROWMAJOR_DUALCMP_SVE_SLOTS - 1 - n);
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

            /* 基线双存: temp (第1腿) 与 dst (第2腿) 背靠背 */
            store_vec_sve(temp + off, res);
            store_vec_sve(dst + off, res);
        }
        sweep++;

        /* 双无条件 memcmp: 两条 store 腿各自 vs golden */
        const bool dst_bad = memcmp(dst, data->expected, bytes) != 0;
        const bool tmp_bad = memcmp(temp, data->expected, bytes) != 0;
        if (dst_bad || tmp_bad) {
            DualcmpSveStats st;
            dualcmp_sve_classify(dst, temp, data->expected, elems, &st);

            char path[256];
            snprintf(path, sizeof(path),
                     "%s/neon_rot_ldr_at_top_rowmajor_dualcmp_sve_fail_cpu%d_sweep%lu.bin",
                     DUALCMP_SVE_DUMP_DIR, device_info[cpu].cpu_number, sweep - 1);
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                const char *bufs[5] = {
                    (const char *)dst, (const char *)data->expected,
                    (const char *)temp, (const char *)data->srcA,
                    (const char *)data->srcB,
                };
                for (int k = 0; k < 5; ++k) {
                    ssize_t total = 0;
                    while ((size_t)total < bytes) {
                        ssize_t w = write(fd, bufs[k] + total, bytes - (size_t)total);
                        if (w <= 0) break;
                        total += w;
                    }
                }
                char tail[192];
                int len = snprintf(tail, sizeof(tail),
                                   "cpu=%d sweep=%lu a_only=%lu b_only=%lu ab_div=%lu ab_same=%lu first_bad=%zu\n",
                                   device_info[cpu].cpu_number, sweep - 1,
                                   (unsigned long)st.a_only, (unsigned long)st.b_only,
                                   (unsigned long)st.ab_div, (unsigned long)st.ab_same,
                                   st.first_bad == (size_t)-1 ? (size_t)0 : st.first_bad);
                if (write(fd, tail, (size_t)len) < 0) { /* best effort */ }
                close(fd);
            }
            report_fail_msg("neon_rot_ldr_at_top_rowmajor_dualcmp_sve: a_only=%lu b_only=%lu "
                            "ab_div=%lu ab_same=%lu first_bad=%zu",
                            (unsigned long)st.a_only, (unsigned long)st.b_only,
                            (unsigned long)st.ab_div, (unsigned long)st.ab_same,
                            st.first_bad == (size_t)-1 ? (size_t)0 : st.first_bad);
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int neon_rot_ldr_at_top_rowmajor_dualcmp_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_dualcmp_sve");
    return EXIT_SKIP;
}
#endif

static int neon_rot_ldr_at_top_rowmajor_dualcmp_sve_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotLdrAtTopRowmajorDualcmpSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_at_top_rowmajor_dualcmp_sve,
             "SVE dual-store comparison chassis: baseline rowmajor skeleton, checks BOTH str legs "
             "(temp & dst) + mutual consistency — which store leg carries the SDC, or is it "
             "upstream of the fork (port of neon_rot_ldr_at_top_rowmajor_dualcmp)")
    .test_init    = neon_rot_ldr_at_top_rowmajor_dualcmp_sve_init,
    .test_run     = neon_rot_ldr_at_top_rowmajor_dualcmp_sve_run,
    .test_cleanup = neon_rot_ldr_at_top_rowmajor_dualcmp_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
