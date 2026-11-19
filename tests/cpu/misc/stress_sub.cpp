/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b stress_sub
 * @parblock
 * Single-instruction pressure test for core-179 SDC: hot loop saturates the
 * 64-bit SUB instruction (`sub`) only. Same load-path layout as the
 * verified-to-trigger mrn_rmw_dump (str res->temp[i]; dst[i]=ldr temp[i]),
 * but every element computes a-b via inline-asm SUB instead of the 4-op
 * switch — so the only arithmetic in the hot loop is SUB.
 *
 * Purpose: isolate whether the SUB instruction itself triggers the 179
 * defect (mrn_rmw_dump's recompute samples showed sub miscomputing). Mixed
 * mrn_rmw interleaves add/sub/xor/and; this runs only SUB so the trigger
 * rate, if any, is attributable to SUB alone.
 *
 * On miscompare, logs inputA/inputB/golden/actual/xor + per-byte breakdown
 * (log-only; the former per-miscompare /tmp record was removed — nothing
 * consumed the file and the log carries the same fields). All logging lives
 * in the cold failure branch; the hot path (read inputs,
 * SUB, store, reload) is byte-for-byte stable.
 *
 * asm constraint note: output uses `=&r` (early-clobber) because the SUB
 * result register must not alias either input register — plain `=r` lets
 * gcc pick the same register for output and an input, and an in-asm read of
 * that register then corrupts it (crashed at RIP=0x2513daa3 on full-core
 * before, per memory movbe-l1d-variants-schedule-sensitivity).
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#define STRESS_COUNT 1024

struct StressData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *expected;
};

#if defined(__aarch64__)
static inline void store_qword(void *addr, uint64_t val) {
    __asm__ volatile ("str %1, [%0]" : : "r"(addr), "r"(val) : "memory");
}
static inline uint64_t load_qword(const void *addr) {
    uint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=r"(res) : "r"(addr) : "memory");
    return res;
}
// Pure 64-bit SUB. & = early-clobber (see file header).
static inline uint64_t op_sub(uint64_t a, uint64_t b) {
    uint64_t res;
    __asm__ volatile ("sub %0, %1, %2" : "=&r"(res) : "r"(a), "r"(b));
    return res;
}
#else
static inline void store_qword(void *addr, uint64_t val) {
    *reinterpret_cast<uint64_t*>(addr) = val;
}
static inline uint64_t load_qword(const void *addr) {
    return *reinterpret_cast<const uint64_t*>(addr);
}
static inline uint64_t op_sub(uint64_t a, uint64_t b) { return a - b; }
#endif

static int stress_sub_init(struct test *test) {
    auto *data = static_cast<StressData *>(malloc(sizeof(StressData)));
    data->srcA = static_cast<uint64_t *>(aligned_alloc_safe(64, STRESS_COUNT * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc_safe(64, STRESS_COUNT * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc_safe(64, STRESS_COUNT * sizeof(uint64_t)));

    memset_random(data->srcA, STRESS_COUNT * sizeof(uint64_t));
    memset_random(data->srcB, STRESS_COUNT * sizeof(uint64_t));

    for (int i = 0; i < STRESS_COUNT; i++) {
        data->expected[i] = data->srcA[i] - data->srcB[i];
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int stress_sub_run(struct test *test, int cpu) {
    auto *data = static_cast<StressData *>(test->data);
    (void)cpu;
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc_safe(64, STRESS_COUNT * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc_safe(64, STRESS_COUNT * sizeof(uint64_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < STRESS_COUNT; i++) {
            uint64_t a = data->srcA[i];
            uint64_t b = data->srcB[i];
            uint64_t res = op_sub(a, b);    // the ONLY arithmetic instruction
            store_qword(&temp[i], res);
            store_qword(&dst[i], load_qword(&temp[i]));
        }

        if (memcmp(dst, data->expected, STRESS_COUNT * sizeof(uint64_t)) != 0) {
            for (int i = 0; i < STRESS_COUNT; i++) {
                if (dst[i] != data->expected[i]) {
                    uint64_t golden = data->expected[i];
                    uint64_t actual = dst[i];
                    uint64_t xorv = golden ^ actual;
                    unsigned char *gb = (unsigned char *)&golden;
                    unsigned char *ab = (unsigned char *)&actual;
                    unsigned char *xb = (unsigned char *)&xorv;
                    log_error("stress_sub: miscompare at index %u: "
                              "inputA=0x%016lX inputB=0x%016lX golden=0x%016lX "
                              "actual=0x%016lX xor=0x%016lX | "
                              "bytes[b7..b0] golden=%02X%02X%02X%02X%02X%02X%02X%02X "
                              "actual=%02X%02X%02X%02X%02X%02X%02X%02X "
                              "xor=%02X%02X%02X%02X%02X%02X%02X%02X",
                              (unsigned)i,
                              (unsigned long)data->srcA[i], (unsigned long)data->srcB[i],
                              (unsigned long)golden, (unsigned long)actual, (unsigned long)xorv,
                              gb[7], gb[6], gb[5], gb[4], gb[3], gb[2], gb[1], gb[0],
                              ab[7], ab[6], ab[5], ab[4], ab[3], ab[2], ab[1], ab[0],
                              xb[7], xb[6], xb[5], xb[4], xb[3], xb[2], xb[1], xb[0]);

                    report_fail_msg("stress_sub: data miscompare at index %u", (unsigned)i);
                }
            }
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int stress_sub_cleanup(struct test *test) {
    auto *data = static_cast<StressData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(stress_sub, "single-instruction pressure test: hot loop saturates 64-bit SUB only (mrn_rmw layout, sub alone) — isolates whether SUB triggers core-179 SDC")
    .test_init = stress_sub_init,
    .test_run = stress_sub_run,
    .test_cleanup = stress_sub_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
