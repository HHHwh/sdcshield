/**
 * @file
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test fmov_train_3ldr_str
 * @parblock
 * 3-source / 3-destination extension of fmov_train_ldr_str (2026-11-19,
 * user directive: "3ldr 3str" extension of the pair).
 *
 * Same cross-register-file FMOV consumer chain and proven ridge
 * geometry as the 2+2 parent; the extension widens both memory sides
 * and keeps the family discipline that EVERY load has a consumer:
 *
 *   ldr q0, [srcA] ; ldr q1, [srcB] ; ldr q2, [srcC]  (3 sources)
 *   fmov x4, d0 / x5, v0.d[1]                        (SIMD -> GPR)
 *   add  x6, x4, rot / x7, x5, rot                    (GPR ALU chain)
 *   fmov d4, x6 / v4.d[1], x7                        (GPR -> SIMD)
 *   <OP> v6.2d, v1.2d, v4.2d                         (chain op 1:
 *                                                      srcB op FMOV(srcA))
 *   <OP> v8.2d, v6.2d, v2.2d                         (chain op 2:
 *                                                      result op srcC)
 *   str q8, [dstA] ; str q8, [dstB] ; str q8, [dstC] (3 back-to-back stores)
 *
 * So the per-lane semantics are g = (b <op> (a + rot)) <op> c, with the
 * slot rotating add/eor/sub per round over u64 integer lanes — bit-exact
 * against the scalar golden.  Integer u64 lanes throughout, no FP
 * semantics.  Per-thread buffers: no cross-thread races by construction.
 * Provenance: instruction sequence extends the SIMDFPLoadStoreMemLoopCpyP08
 * extract behind the 2+2 parent (2026-11-18); the LDP/STP twin of THIS
 * test lives in fmov_train_3ldp_stp.cpp.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

#define FTS3_NUM_ELEMENTS 2048            /* 16B elements -> 32KB/buffer */
#define FTS3_BYTES (FTS3_NUM_ELEMENTS * 16)

namespace { /* ODR: per-TU FTS3Data */
struct FTS3Data {
    int dummy;                           /* buffers are per-thread, in run() */
};
}

/*
 * One pass over the buffers, single-register form (ldr/str), 3 sources
 * and 3 destinations.  slot: 0=add 1=eor 2=sub, applied to BOTH chain
 * ops; the GPR consumer is a plain ADD of the rotating constant.  All
 * pointers advance by post-index and wrap back each pass (reset by the
 * caller).
 */
static void fts3_pass_3ldr_str(const uint64_t *a, const uint64_t *b,
                               const uint64_t *c,
                               uint64_t *da, uint64_t *db, uint64_t *dc,
                               uint64_t rot, int slot)
{
    const uint64_t *pa = a, *pb = b, *pc = c;
    uint64_t *pda = da, *pdb = db, *pdc = dc;

    for (size_t i = 0; i < FTS3_NUM_ELEMENTS; ++i) {
        if (slot == 0) {
            __asm__ volatile(
                "ldr  q0, [%[a]], #16\n\t"
                "ldr  q1, [%[b]], #16\n\t"
                "ldr  q2, [%[c]], #16\n\t"
                "fmov x4, d0\n\t"
                "fmov x5, v0.d[1]\n\t"
                "add  x6, x4, %[rot]\n\t"
                "add  x7, x5, %[rot]\n\t"
                "fmov d4, x6\n\t"
                "fmov v4.d[1], x7\n\t"
                "add  v6.2d, v1.2d, v4.2d\n\t"
                "add  v8.2d, v6.2d, v2.2d\n\t"
                "str  q8, [%[da]], #16\n\t"
                "str  q8, [%[db]], #16\n\t"
                "str  q8, [%[dc]], #16\n\t"
                : [a] "+r" (pa), [b] "+r" (pb), [c] "+r" (pc),
                  [da] "+r" (pda), [db] "+r" (pdb), [dc] "+r" (pdc)
                : [rot] "r" (rot)
                : "v0", "v1", "v2", "v4", "v6", "v8",
                  "x4", "x5", "x6", "x7", "memory");
        } else if (slot == 1) {
            __asm__ volatile(
                "ldr  q0, [%[a]], #16\n\t"
                "ldr  q1, [%[b]], #16\n\t"
                "ldr  q2, [%[c]], #16\n\t"
                "fmov x4, d0\n\t"
                "fmov x5, v0.d[1]\n\t"
                "add  x6, x4, %[rot]\n\t"
                "add  x7, x5, %[rot]\n\t"
                "fmov d4, x6\n\t"
                "fmov v4.d[1], x7\n\t"
                "eor  v6.16b, v1.16b, v4.16b\n\t"
                "eor  v8.16b, v6.16b, v2.16b\n\t"
                "str  q8, [%[da]], #16\n\t"
                "str  q8, [%[db]], #16\n\t"
                "str  q8, [%[dc]], #16\n\t"
                : [a] "+r" (pa), [b] "+r" (pb), [c] "+r" (pc),
                  [da] "+r" (pda), [db] "+r" (pdb), [dc] "+r" (pdc)
                : [rot] "r" (rot)
                : "v0", "v1", "v2", "v4", "v6", "v8",
                  "x4", "x5", "x6", "x7", "memory");
        } else {
            __asm__ volatile(
                "ldr  q0, [%[a]], #16\n\t"
                "ldr  q1, [%[b]], #16\n\t"
                "ldr  q2, [%[c]], #16\n\t"
                "fmov x4, d0\n\t"
                "fmov x5, v0.d[1]\n\t"
                "add  x6, x4, %[rot]\n\t"
                "add  x7, x5, %[rot]\n\t"
                "fmov d4, x6\n\t"
                "fmov v4.d[1], x7\n\t"
                "sub  v6.2d, v1.2d, v4.2d\n\t"
                "sub  v8.2d, v6.2d, v2.2d\n\t"
                "str  q8, [%[da]], #16\n\t"
                "str  q8, [%[db]], #16\n\t"
                "str  q8, [%[dc]], #16\n\t"
                : [a] "+r" (pa), [b] "+r" (pb), [c] "+r" (pc),
                  [da] "+r" (pda), [db] "+r" (pdb), [dc] "+r" (pdc)
                : [rot] "r" (rot)
                : "v0", "v1", "v2", "v4", "v6", "v8",
                  "x4", "x5", "x6", "x7", "memory");
        }
    }
}

/* scalar bit-exact golden: g = (b <op> (a + rot)) <op> c, per u64 lane */
static void fts3_golden(const uint64_t *a, const uint64_t *b,
                        const uint64_t *c,
                        uint64_t *g, uint64_t rot, int slot)
{
    size_t n = FTS3_NUM_ELEMENTS * 2;
    for (size_t i = 0; i < n; ++i) {
        uint64_t x = (slot == 0) ? (b[i] + (a[i] + rot))
                   : (slot == 1) ? (b[i] ^ (a[i] + rot))
                   :               (b[i] - (a[i] + rot));
        g[i] = (slot == 0) ? (x + c[i])
             : (slot == 1) ? (x ^ c[i])
             :               (x - c[i]);
    }
}

static int fmov_train_3ldr_str_init(struct test *test)
{
    auto *d = new FTS3Data;
    if (!d)
        return EXIT_FAILURE;
    d->dummy = 0;
    test->data = d;
    return EXIT_SUCCESS;
}

static int fmov_train_3ldr_str_cleanup(struct test *test)
{
    delete static_cast<FTS3Data *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int fmov_train_3ldr_str_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;

    /* per-thread buffers; operator new guarantees 16B (max_align_t),
     * which is exactly the ldr/str q access size */
    std::vector<uint8_t> srcAv(FTS3_BYTES), srcBv(FTS3_BYTES), srcCv(FTS3_BYTES);
    std::vector<uint8_t> dstAv(FTS3_BYTES), dstBv(FTS3_BYTES), dstCv(FTS3_BYTES);
    std::vector<uint8_t> goldv(FTS3_BYTES);
    auto *srcA = reinterpret_cast<uint64_t *>(srcAv.data());
    auto *srcB = reinterpret_cast<uint64_t *>(srcBv.data());
    auto *srcC = reinterpret_cast<uint64_t *>(srcCv.data());
    auto *dstA = reinterpret_cast<uint64_t *>(dstAv.data());
    auto *dstB = reinterpret_cast<uint64_t *>(dstBv.data());
    auto *dstC = reinterpret_cast<uint64_t *>(dstCv.data());
    auto *gold = reinterpret_cast<uint64_t *>(goldv.data());

    /* randomization hardening H14': framework RNG fill (bit patterns) */
    memset_random(srcAv.data(), FTS3_BYTES);
    memset_random(srcBv.data(), FTS3_BYTES);
    memset_random(srcCv.data(), FTS3_BYTES);
    memset(dstAv.data(), 0, FTS3_BYTES);
    memset(dstBv.data(), 0, FTS3_BYTES);
    memset(dstCv.data(), 0, FTS3_BYTES);

    uint64_t round = 0;
    do {
        int slot = (int)(round % 3);
        uint64_t rot = 0x9E3779B97F4A7C15ULL + round;

        fts3_pass_3ldr_str(srcA, srcB, srcC, dstA, dstB, dstC, rot, slot);
        fts3_golden(srcA, srcB, srcC, gold, rot, slot);

        if (memcmp(dstA, gold, FTS3_BYTES) != 0 || memcmp(dstB, gold, FTS3_BYTES) != 0
            || memcmp(dstC, gold, FTS3_BYTES) != 0) {
            for (size_t i = 0; i < FTS3_NUM_ELEMENTS * 2; ++i) {
                if (dstA[i] != gold[i]) {
                    report_fail_msg("fmov_train_3ldr_str: dstA mismatch at "
                                    "element %zu (round %lu, slot %d): "
                                    "expected 0x%016lx got 0x%016lx "
                                    "(3-src cross-file FMOV consumer chain)",
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

DECLARE_TEST(fmov_train_3ldr_str, "Cross-register-file FMOV consumer chain, 3-source/3-destination form: 3 ldr + SIMD->GPR->SIMD lane round-trip + chained slot-rotated add/eor/sub + 3 back-to-back str, bit-exact scalar golden per round (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fmov_train_3ldr_str_init,
    .test_run = fmov_train_3ldr_str_run,
    .test_cleanup = fmov_train_3ldr_str_cleanup,
    .fracture_loop_count = -1,   /* never fracture: pass rounds are fast */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
