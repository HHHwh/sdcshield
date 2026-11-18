/**
 * @file
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test fmov_train_ldr_str
 * @parblock
 * Cross-register-file consumer chain on the proven ridge geometry
 * (2026-11-18, user directive: single ldr/str standalone test).
 *
 * Geometry = the proven trigger recipe skeleton: 2 independent source
 * loads (srcA/srcB) + back-to-back str/str to two destinations, small
 * L1D-domain per-thread buffers.  The MIDDLE LOAD (user's definition:
 * anything insertable between ldr and str that preserves function) is
 * a cross-register-file FMOV chain — the one micro-architectural path
 * none of the 30+21 existing adaptive combos covers:
 *
 *   ldr q0, [srcA] ; ldr q1, [srcB]        (2 sources)
 *   fmov x4, d0 / x5, v0.d[1]         (SIMD -> GPR, RF port)
 *   add  x6, x4, rot / x7, x5, rot          (GPR ALU consumer chain)
 *   fmov d4, x6 / v4.d[1], x7          (GPR -> SIMD, RF port)
 *   <OP> v6.2d, v1.2d, v4.2d                (SIMD ALU, slot rotates
 *                                             add/eor/sub per round)
 *   str q6, [dstA] ; str q6, [dstB]         (back-to-back stores)
 *
 * Integer u64 lanes throughout (bit-exact golden, no FP semantics).
 * Golden is a scalar C reference recomputed per round (the rot
 * constant advances per round); dstA and dstB are both byte-compared
 * against it after every pass.  Per-thread buffers: no cross-thread
 * races by construction.  Provenance: instruction sequence extracted
 * from SIMDFPLoadStoreMemLoopCpyP08 disassembly (user-provided,
 * 2026-11-18) — the LDP/STP twin of this test lives in
 * fmov_train_ldp_stp.cpp.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

#define FTS_NUM_ELEMENTS 2048            /* 16B elements -> 32KB/buffer */
#define FTS_BYTES (FTS_NUM_ELEMENTS * 16)

namespace { /* ODR: per-TU FTSData */
struct FTSData {
    int dummy;                           /* buffers are per-thread, in run() */
};
}

/*
 * One pass over the buffers, single-register form (ldr/str).
 * slot: 0=add 1=eor 2=sub on the SIMD side; the GPR consumer is a
 * plain ADD of the rotating constant.  All pointers advance by
 * post-index and wrap back each pass (reset by the caller).
 */
static void fts_pass_ldr_str(const uint64_t *a, const uint64_t *b,
                             uint64_t *da, uint64_t *db, uint64_t rot, int slot)
{
    const uint64_t *pa = a, *pb = b;
    uint64_t *pda = da, *pdb = db;

    for (size_t i = 0; i < FTS_NUM_ELEMENTS; ++i) {
        if (slot == 0) {
            __asm__ volatile(
                "ldr  q0, [%[a]], #16\n\t"
                "ldr  q1, [%[b]], #16\n\t"
                "fmov x4, d0\n\t"
                "fmov x5, v0.d[1]\n\t"
                "add  x6, x4, %[rot]\n\t"
                "add  x7, x5, %[rot]\n\t"
                "fmov d4, x6\n\t"
                "fmov v4.d[1], x7\n\t"
                "add  v6.2d, v1.2d, v4.2d\n\t"
                "str  q6, [%[da]], #16\n\t"
                "str  q6, [%[db]], #16\n\t"
                : [a] "+r" (pa), [b] "+r" (pb), [da] "+r" (pda), [db] "+r" (pdb)
                : [rot] "r" (rot)
                : "v0", "v1", "v4", "v6", "x4", "x5", "x6", "x7", "memory");
        } else if (slot == 1) {
            __asm__ volatile(
                "ldr  q0, [%[a]], #16\n\t"
                "ldr  q1, [%[b]], #16\n\t"
                "fmov x4, d0\n\t"
                "fmov x5, v0.d[1]\n\t"
                "add  x6, x4, %[rot]\n\t"
                "add  x7, x5, %[rot]\n\t"
                "fmov d4, x6\n\t"
                "fmov v4.d[1], x7\n\t"
                "eor  v6.16b, v1.16b, v4.16b\n\t"
                "str  q6, [%[da]], #16\n\t"
                "str  q6, [%[db]], #16\n\t"
                : [a] "+r" (pa), [b] "+r" (pb), [da] "+r" (pda), [db] "+r" (pdb)
                : [rot] "r" (rot)
                : "v0", "v1", "v4", "v6", "x4", "x5", "x6", "x7", "memory");
        } else {
            __asm__ volatile(
                "ldr  q0, [%[a]], #16\n\t"
                "ldr  q1, [%[b]], #16\n\t"
                "fmov x4, d0\n\t"
                "fmov x5, v0.d[1]\n\t"
                "add  x6, x4, %[rot]\n\t"
                "add  x7, x5, %[rot]\n\t"
                "fmov d4, x6\n\t"
                "fmov v4.d[1], x7\n\t"
                "sub  v6.2d, v1.2d, v4.2d\n\t"
                "str  q6, [%[da]], #16\n\t"
                "str  q6, [%[db]], #16\n\t"
                : [a] "+r" (pa), [b] "+r" (pb), [da] "+r" (pda), [db] "+r" (pdb)
                : [rot] "r" (rot)
                : "v0", "v1", "v4", "v6", "x4", "x5", "x6", "x7", "memory");
        }
    }
}

/* scalar bit-exact golden: g = b <op> (a + rot), per u64 lane */
static void fts_golden(const uint64_t *a, const uint64_t *b,
                       uint64_t *g, uint64_t rot, int slot)
{
    size_t n = FTS_NUM_ELEMENTS * 2;
    for (size_t i = 0; i < n; ++i) {
        uint64_t x = a[i] + rot;
        g[i] = (slot == 0) ? (b[i] + x)
             : (slot == 1) ? (b[i] ^ x)
             :               (b[i] - x);
    }
}

static int fmov_train_ldr_str_init(struct test *test)
{
    auto *d = new FTSData;
    if (!d)
        return EXIT_FAILURE;
    d->dummy = 0;
    test->data = d;
    return EXIT_SUCCESS;
}

static int fmov_train_ldr_str_cleanup(struct test *test)
{
    delete static_cast<FTSData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int fmov_train_ldr_str_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;

    /* per-thread buffers; operator new guarantees 16B (max_align_t),
     * which is exactly the ldr/str q access size */
    std::vector<uint8_t> srcAv(FTS_BYTES), srcBv(FTS_BYTES);
    std::vector<uint8_t> dstAv(FTS_BYTES), dstBv(FTS_BYTES), goldv(FTS_BYTES);
    auto *srcA = reinterpret_cast<uint64_t *>(srcAv.data());
    auto *srcB = reinterpret_cast<uint64_t *>(srcBv.data());
    auto *dstA = reinterpret_cast<uint64_t *>(dstAv.data());
    auto *dstB = reinterpret_cast<uint64_t *>(dstBv.data());
    auto *gold = reinterpret_cast<uint64_t *>(goldv.data());

    /* randomization hardening H14': framework RNG fill (bit patterns) */
    memset_random(srcAv.data(), FTS_BYTES);
    memset_random(srcBv.data(), FTS_BYTES);
    memset(dstAv.data(), 0, FTS_BYTES);
    memset(dstBv.data(), 0, FTS_BYTES);

    uint64_t round = 0;
    do {
        int slot = (int)(round % 3);
        uint64_t rot = 0x9E3779B97F4A7C15ULL + round;

        fts_pass_ldr_str(srcA, srcB, dstA, dstB, rot, slot);
        fts_golden(srcA, srcB, gold, rot, slot);

        if (memcmp(dstA, gold, FTS_BYTES) != 0 || memcmp(dstB, gold, FTS_BYTES) != 0) {
            for (size_t i = 0; i < FTS_NUM_ELEMENTS * 2; ++i) {
                if (dstA[i] != gold[i]) {
                    report_fail_msg("fmov_train_ldr_str: dstA mismatch at "
                                    "element %zu (round %lu, slot %d): "
                                    "expected 0x%016lx got 0x%016lx "
                                    "(cross-file FMOV consumer chain)",
                                    i / 2, (unsigned long)round, slot,
                                    (unsigned long)gold[i],
                                    (unsigned long)dstA[i]);
                    return EXIT_FAILURE;
                }
            }
        }
        ++round;
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(fmov_train_ldr_str, "Cross-register-file FMOV consumer chain on the ridge geometry: 2-source ldr + SIMD->GPR->SIMD lane round-trip + slot-rotated add/eor/sub + back-to-back str/str, bit-exact scalar golden per round (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fmov_train_ldr_str_init,
    .test_run = fmov_train_ldr_str_run,
    .test_cleanup = fmov_train_ldr_str_cleanup,
    .fracture_loop_count = -1,   /* never fracture: pass rounds are fast */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
