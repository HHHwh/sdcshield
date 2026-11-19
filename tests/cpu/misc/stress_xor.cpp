/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b stress_xor
 * @parblock
 * Single-instruction pressure test for core-179 SDC: hot loop saturates the
 * 64-bit XOR instruction (`eor` on AArch64) only. Same load-path layout as
 * the verified-to-trigger mrn_rmw_dump (str res->temp[i]; dst[i]=ldr temp[i]),
 * but every element computes a^b via inline-asm EOR instead of the 4-op
 * switch — so the only arithmetic in the hot loop is XOR.
 *
 * Purpose: isolate whether the XOR instruction itself triggers the 179
 * defect (mrn_rmw_dump's recompute samples showed op=xor prevalent).
 * Mixed mrn_rmw interleaves add/sub/xor/and; this runs only XOR so the
 * trigger rate, if any, is attributable to XOR alone.
 *
 * On miscompare, logs inputA/inputB/golden/actual/xor + per-byte breakdown
 * (log-only; the former per-miscompare /tmp record was removed — nothing
 * consumed the file and the log carries the same fields). All logging
 * lives in the cold failure branch; the hot path is stable.
 *
 * asm note: output `=&r` (early-clobber) — see stress_sub.cpp header.
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
// Pure 64-bit XOR (AArch64 mnemonic is EOR). & = early-clobber.
static inline uint64_t op_eor(uint64_t a, uint64_t b) {
    uint64_t res;
    __asm__ volatile ("eor %0, %1, %2" : "=&r"(res) : "r"(a), "r"(b));
    return res;
}
#else
static inline void store_qword(void *addr, uint64_t val) {
    *reinterpret_cast<uint64_t*>(addr) = val;
}
static inline uint64_t load_qword(const void *addr) {
    return *reinterpret_cast<const uint64_t*>(addr);
}
static inline uint64_t op_eor(uint64_t a, uint64_t b) { return a ^ b; }
#endif

static int stress_xor_init(struct test *test) {
    auto *data = static_cast<StressData *>(malloc(sizeof(StressData)));
    data->srcA = static_cast<uint64_t *>(aligned_alloc_safe(64, STRESS_COUNT * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc_safe(64, STRESS_COUNT * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc_safe(64, STRESS_COUNT * sizeof(uint64_t)));

    memset_random(data->srcA, STRESS_COUNT * sizeof(uint64_t));
    memset_random(data->srcB, STRESS_COUNT * sizeof(uint64_t));

    for (int i = 0; i < STRESS_COUNT; i++) {
        data->expected[i] = data->srcA[i] ^ data->srcB[i];
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int stress_xor_run(struct test *test, int cpu) {
    auto *data = static_cast<StressData *>(test->data);
    (void)cpu;
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc_safe(64, STRESS_COUNT * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc_safe(64, STRESS_COUNT * sizeof(uint64_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < STRESS_COUNT; i++) {
            uint64_t a = data->srcA[i];
            uint64_t b = data->srcB[i];
            uint64_t res = op_eor(a, b);    // the ONLY arithmetic instruction
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
                    log_error("stress_xor: miscompare at index %u: "
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

                    report_fail_msg("stress_xor: data miscompare at index %u", (unsigned)i);
                }
            }
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int stress_xor_cleanup(struct test *test) {
    auto *data = static_cast<StressData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(stress_xor, "single-instruction pressure test: hot loop saturates 64-bit XOR/EOR only (mrn_rmw layout, xor alone) — isolates whether XOR triggers core-179 SDC")
    .test_init = stress_xor_init,
    .test_run = stress_xor_run,
    .test_cleanup = stress_xor_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
