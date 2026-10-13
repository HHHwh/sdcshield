/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_at_top_rowmajor_dump_sve
 * @parblock
 * SVE port of neon_rot_ldr_at_top_rowmajor_dump (source:
 * missing_testcases_20261009/tests/cpu/misc/
 * neon_rot_ldr_at_top_rowmajor_dump.cpp).
 *
 * Instrumented chassis of the rowmajor position probe: the hot loop is
 * byte-identical to neon_rot_ldr_at_top_rowmajor_sve (dead scratch ldr
 * z at top, 2 source loads, rotating ALU per slot, back-to-back store
 * pair, alternating sweep direction). The cold path (memcmp mismatch
 * branch only) scans every mismatched u64 element, logs
 * srcA/srcB/golden/actual/xor, and appends one packed binary record
 * per mismatch to /var/tmp/neon_rot_ldr_at_top_rowmajor_dump_sve_<pid>.bin
 * — answering WHAT was computed wrong and whether errors follow the
 * sweep direction (address-local vs sweep-seam).
 *
 * Record format deviation (documented): the scalar original stores a
 * 16-byte NEON vector as lo/hi pairs; the SVE port records at u64
 * ELEMENT granularity (one record per mismatched element, single u64
 * value fields) — finer resolution, same information set, and
 * VL-agnostic.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <sys/auxv.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif
#endif

#define NEON_ROT_LDR_AT_TOP_ROWMAJOR_DUMP_SVE_SLOTS 1024

#pragma pack(push, 1)
struct rowmajor_dump_sve_record {
    uint64_t timestamp_us;   /* CLOCK_MONOTONIC */
    uint32_t cpu;            /* device_info[cpu].cpu_number — logical CPU */
    uint32_t iteration;      /* 0-based sweep */
    uint32_t sweep_dir;      /* 0=ascending 1=descending */
    uint32_t index;          /* u64 element index */
    uint32_t op;             /* slot%4: 0=add 1=eor 2=and 3=orr */
    uint32_t _pad;
    uint64_t srcA;
    uint64_t srcB;
    uint64_t exp;
    uint64_t act;
    uint64_t xorv;
};
#pragma pack(pop)

struct NeonRotLdrAtTopRowmajorDumpSveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *scratch;
    uint64_t *expected;
    int lanes;
    int dump_fd;
};

static uint64_t dump_sve_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

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

static int neon_rot_ldr_at_top_rowmajor_dump_sve_init(struct test *test) {
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_dump_sve");
        return EXIT_SKIP;
    }

    const int lanes = (int)svcntd();
    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_ROWMAJOR_DUMP_SVE_SLOTS * lanes;

    auto *data = static_cast<NeonRotLdrAtTopRowmajorDumpSveData *>(malloc(sizeof(NeonRotLdrAtTopRowmajorDumpSveData)));
    data->lanes = lanes;
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->scratch = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    memset_random(data->srcA, elems * sizeof(uint64_t));
    memset_random(data->srcB, elems * sizeof(uint64_t));
    memset_random(data->scratch, elems * sizeof(uint64_t));

    svbool_t pg = svptrue_b64();
    for (int i = 0; i < NEON_ROT_LDR_AT_TOP_ROWMAJOR_DUMP_SVE_SLOTS; i++) {
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

    char path[128];
    snprintf(path, sizeof(path), "/var/tmp/neon_rot_ldr_at_top_rowmajor_dump_sve_%d.bin", (int)getpid());
    data->dump_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (data->dump_fd < 0) {
        log_error("neon_rot_ldr_at_top_rowmajor_dump_sve: cannot open %s (errno %d) — "
                  "continuing without binary records", path, errno);
    }

    test->data = data;
    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_dump_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)
static int neon_rot_ldr_at_top_rowmajor_dump_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<NeonRotLdrAtTopRowmajorDumpSveData *>(test->data);

    if ((int)svcntd() != data->lanes) {
        log_error("neon_rot_ldr_at_top_rowmajor_dump_sve: vector length changed between init (%d) and run (%d)",
                  data->lanes, (int)svcntd());
        report_fail_msg("neon_rot_ldr_at_top_rowmajor_dump_sve: vector length changed between init and run");
    }

    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_ROWMAJOR_DUMP_SVE_SLOTS * data->lanes;
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 13) {
        const bool ascending = (sweep & 1) == 0;
        for (int n = 0; n < NEON_ROT_LDR_AT_TOP_ROWMAJOR_DUMP_SVE_SLOTS; n++) {
            const int i = ascending ? n : (NEON_ROT_LDR_AT_TOP_ROWMAJOR_DUMP_SVE_SLOTS - 1 - n);
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
        const uint64_t sweep_dir = ascending ? 0 : 1;
        sweep++;

        if (memcmp(dst, data->expected, elems * sizeof(uint64_t)) != 0) {
            /* 冷路径: 逐元素扫描 + 日志 + 二进制记录 (元素粒度) */
            for (size_t j = 0; j < elems; j++) {
                if (dst[j] != data->expected[j]) {
                    const int slot = (int)(j / (size_t)data->lanes);
                    const uint32_t op = (uint32_t)(slot % 4);
                    log_error("neon_rot_ldr_at_top_rowmajor_dump_sve: miscompare at index %zu "
                              "(slot %d op=%u dir=%s): srcA=0x%016lX srcB=0x%016lX "
                              "exp=0x%016lX act=0x%016lX xor=0x%016lX",
                              j, slot, op, ascending ? "asc" : "desc",
                              (unsigned long)data->srcA[j], (unsigned long)data->srcB[j],
                              (unsigned long)data->expected[j], (unsigned long)dst[j],
                              (unsigned long)(data->expected[j] ^ dst[j]));
                    if (data->dump_fd >= 0) {
                        rowmajor_dump_sve_record rec;
                        rec.timestamp_us = dump_sve_now_us();
                        rec.cpu = (uint32_t)device_info[cpu].cpu_number;
                        rec.iteration = (uint32_t)(sweep - 1);
                        rec.sweep_dir = (uint32_t)sweep_dir;
                        rec.index = (uint32_t)j;
                        rec.op = op;
                        rec._pad = 0;
                        rec.srcA = data->srcA[j];
                        rec.srcB = data->srcB[j];
                        rec.exp = data->expected[j];
                        rec.act = dst[j];
                        rec.xorv = data->expected[j] ^ dst[j];
                        if (write(data->dump_fd, &rec, sizeof rec) < 0) {
                            /* 记录失败不影响检测本身 */
                        }
                    }
                }
            }
            report_fail_msg("neon_rot_ldr_at_top_rowmajor_dump_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int neon_rot_ldr_at_top_rowmajor_dump_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_rowmajor_dump_sve");
    return EXIT_SKIP;
}
#endif

static int neon_rot_ldr_at_top_rowmajor_dump_sve_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotLdrAtTopRowmajorDumpSveData *>(test->data);
    if (data) {
        if (data->dump_fd >= 0)
            close(data->dump_fd);
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_at_top_rowmajor_dump_sve,
             "SVE dump variant of the rowmajor position probe: same hot loop; on fail logs "
             "srcA/srcB/exp/act/xor per element + binary records (element-granular) to /var/tmp "
             "(port of neon_rot_ldr_at_top_rowmajor_dump)")
    .test_init    = neon_rot_ldr_at_top_rowmajor_dump_sve_init,
    .test_run     = neon_rot_ldr_at_top_rowmajor_dump_sve_run,
    .test_cleanup = neon_rot_ldr_at_top_rowmajor_dump_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
