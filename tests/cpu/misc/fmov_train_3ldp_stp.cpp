/**
 * @file
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test fmov_train_3ldp_stp
 * @parblock
 * LDP/STP pair-form twin of fmov_train_3ldr_str (2026-11-19, user
 * directive: "3ldr 3str" extension of the fmov_train_ldp_stp pair).
 *
 * Identical semantics and golden; the load/store form is the post-indexed
 * PAIR: one LDP Q,Q,#32 loads two elements (32B, single pointer update),
 * one STP Q,Q,#32 stores two elements back-to-back.  Three sources and
 * three destinations, so per body:
 *
 *   ldp q0,q1,[srcA],#32 ; ldp q2,q3,[srcB],#32 ; ldp q4,q5,[srcC],#32
 *   fmov x, v.d[..] / add / fmov v.d[..], x   (RF round-trip, x4-x11)
 *   <OP> v8.2d, v2.2d, v6.2d                  (chain op 1, element 0)
 *   <OP> v9.2d, v3.2d, v7.2d                  (chain op 1, element 1)
 *   <OP> v10.2d, v8.2d, v4.2d                 (chain op 2: result op srcC)
 *   <OP> v11.2d, v9.2d, v5.2d
 *   stp q10,q11,[dstA],#32 ; stp q10,q11,[dstB],#32 ; stp q10,q11,[dstC],#32
 *
 * Per-lane semantics g = (b <op> (a + rot)) <op> c, slot rotating
 * add/eor/sub per round over u64 integer lanes — bit-exact against the
 * scalar golden shared with the ldr/str twin.  Buffers 32B-aligned
 * (LDP/STP Q-pair access size).  Per-thread buffers, no cross-thread
 * races.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

#define FTP3_NUM_ELEMENTS 2048            /* 16B elements -> 32KB/buffer */
#define FTP3_BYTES (FTP3_NUM_ELEMENTS * 16)

namespace { /* ODR: per-TU FTP3Data */
struct FTP3Data {
    int dummy;                           /* buffers are per-thread, in run() */
};
}

/*
 * One pass, pair form: two elements per body, three sources and three
 * destinations.  slot: 0=add 1=eor 2=sub applied to both chain ops;
 * GPR consumer is a plain ADD of the rotating constant (both lanes,
 * both elements — same semantics as the 3ldr/3str twin, golden shared).
 */
static void ftp3_pass_3ldp_stp(const uint64_t *a, const uint64_t *b,
                               const uint64_t *c,
                               uint64_t *da, uint64_t *db, uint64_t *dc,
                               uint64_t rot, int slot)
{
    const uint64_t *pa = a, *pb = b, *pc = c;
    uint64_t *pda = da, *pdb = db, *pdc = dc;

    for (size_t i = 0; i < FTP3_NUM_ELEMENTS; i += 2) {
        if (slot == 0) {
            __asm__ volatile(
                "ldp  q0, q1, [%[a]], #32\n\t"
                "ldp  q2, q3, [%[b]], #32\n\t"
                "ldp  q4, q5, [%[c]], #32\n\t"
                "fmov x4, d0\n\t"
                "fmov x5, v0.d[1]\n\t"
                "add  x6, x4, %[rot]\n\t"
                "add  x7, x5, %[rot]\n\t"
                "fmov d6, x6\n\t"
                "fmov v6.d[1], x7\n\t"
                "fmov x8, d1\n\t"
                "fmov x9, v1.d[1]\n\t"
                "add  x10, x8, %[rot]\n\t"
                "add  x11, x9, %[rot]\n\t"
                "fmov d7, x10\n\t"
                "fmov v7.d[1], x11\n\t"
                "add  v8.2d, v2.2d, v6.2d\n\t"
                "add  v9.2d, v3.2d, v7.2d\n\t"
                "add  v10.2d, v8.2d, v4.2d\n\t"
                "add  v11.2d, v9.2d, v5.2d\n\t"
                "stp  q10, q11, [%[da]], #32\n\t"
                "stp  q10, q11, [%[db]], #32\n\t"
                "stp  q10, q11, [%[dc]], #32\n\t"
                : [a] "+r" (pa), [b] "+r" (pb), [c] "+r" (pc),
                  [da] "+r" (pda), [db] "+r" (pdb), [dc] "+r" (pdc)
                : [rot] "r" (rot)
                : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
                  "v8", "v9", "v10", "v11",
                  "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "memory");
        } else if (slot == 1) {
            __asm__ volatile(
                "ldp  q0, q1, [%[a]], #32\n\t"
                "ldp  q2, q3, [%[b]], #32\n\t"
                "ldp  q4, q5, [%[c]], #32\n\t"
                "fmov x4, d0\n\t"
                "fmov x5, v0.d[1]\n\t"
                "add  x6, x4, %[rot]\n\t"
                "add  x7, x5, %[rot]\n\t"
                "fmov d6, x6\n\t"
                "fmov v6.d[1], x7\n\t"
                "fmov x8, d1\n\t"
                "fmov x9, v1.d[1]\n\t"
                "add  x10, x8, %[rot]\n\t"
                "add  x11, x9, %[rot]\n\t"
                "fmov d7, x10\n\t"
                "fmov v7.d[1], x11\n\t"
                "eor  v8.16b, v2.16b, v6.16b\n\t"
                "eor  v9.16b, v3.16b, v7.16b\n\t"
                "eor  v10.16b, v8.16b, v4.16b\n\t"
                "eor  v11.16b, v9.16b, v5.16b\n\t"
                "stp  q10, q11, [%[da]], #32\n\t"
                "stp  q10, q11, [%[db]], #32\n\t"
                "stp  q10, q11, [%[dc]], #32\n\t"
                : [a] "+r" (pa), [b] "+r" (pb), [c] "+r" (pc),
                  [da] "+r" (pda), [db] "+r" (pdb), [dc] "+r" (pdc)
                : [rot] "r" (rot)
                : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
                  "v8", "v9", "v10", "v11",
                  "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "memory");
        } else {
            __asm__ volatile(
                "ldp  q0, q1, [%[a]], #32\n\t"
                "ldp  q2, q3, [%[b]], #32\n\t"
                "ldp  q4, q5, [%[c]], #32\n\t"
                "fmov x4, d0\n\t"
                "fmov x5, v0.d[1]\n\t"
                "add  x6, x4, %[rot]\n\t"
                "add  x7, x5, %[rot]\n\t"
                "fmov d6, x6\n\t"
                "fmov v6.d[1], x7\n\t"
                "fmov x8, d1\n\t"
                "fmov x9, v1.d[1]\n\t"
                "add  x10, x8, %[rot]\n\t"
                "add  x11, x9, %[rot]\n\t"
                "fmov d7, x10\n\t"
                "fmov v7.d[1], x11\n\t"
                "sub  v8.2d, v2.2d, v6.2d\n\t"
                "sub  v9.2d, v3.2d, v7.2d\n\t"
                "sub  v10.2d, v8.2d, v4.2d\n\t"
                "sub  v11.2d, v9.2d, v5.2d\n\t"
                "stp  q10, q11, [%[da]], #32\n\t"
                "stp  q10, q11, [%[db]], #32\n\t"
                "stp  q10, q11, [%[dc]], #32\n\t"
                : [a] "+r" (pa), [b] "+r" (pb), [c] "+r" (pc),
                  [da] "+r" (pda), [db] "+r" (pdb), [dc] "+r" (pdc)
                : [rot] "r" (rot)
                : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
                  "v8", "v9", "v10", "v11",
                  "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "memory");
        }
    }
}

/* scalar bit-exact golden: g = (b <op> (a + rot)) <op> c, per u64 lane
 * (identical semantics to the 3ldr/3str twin) */
static void ftp3_golden(const uint64_t *a, const uint64_t *b,
                        const uint64_t *c,
                        uint64_t *g, uint64_t rot, int slot)
{
    size_t n = FTP3_NUM_ELEMENTS * 2;
    for (size_t i = 0; i < n; ++i) {
        uint64_t x = (slot == 0) ? (b[i] + (a[i] + rot))
                   : (slot == 1) ? (b[i] ^ (a[i] + rot))
                   :               (b[i] - (a[i] + rot));
        g[i] = (slot == 0) ? (x + c[i])
             : (slot == 1) ? (x ^ c[i])
             :               (x - c[i]);
    }
}

static int fmov_train_3ldp_stp_init(struct test *test)
{
    auto *d = new FTP3Data;
    if (!d)
        return EXIT_FAILURE;
    d->dummy = 0;
    test->data = d;
    return EXIT_SUCCESS;
}

static int fmov_train_3ldp_stp_cleanup(struct test *test)
{
    delete static_cast<FTP3Data *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int fmov_train_3ldp_stp_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;

    /* per-thread buffers; the six asm-touched buffers are 32B-aligned
     * (LDP/STP Q-pair access size) via aligned_alloc — std::vector's
     * heap block is only guaranteed 16B (max_align_t) and alignas on
     * the vector OBJECT does not align its buffer. */
    auto *srcA = static_cast<uint64_t *>(aligned_alloc(32, FTP3_BYTES));
    auto *srcB = static_cast<uint64_t *>(aligned_alloc(32, FTP3_BYTES));
    auto *srcC = static_cast<uint64_t *>(aligned_alloc(32, FTP3_BYTES));
    auto *dstA = static_cast<uint64_t *>(aligned_alloc(32, FTP3_BYTES));
    auto *dstB = static_cast<uint64_t *>(aligned_alloc(32, FTP3_BYTES));
    auto *dstC = static_cast<uint64_t *>(aligned_alloc(32, FTP3_BYTES));
    auto *gold = static_cast<uint64_t *>(aligned_alloc(32, FTP3_BYTES));
    if (!srcA || !srcB || !srcC || !dstA || !dstB || !dstC || !gold) {
        free(srcA); free(srcB); free(srcC);
        free(dstA); free(dstB); free(dstC); free(gold);
        report_fail_msg("fmov_train_3ldp_stp: buffer allocation failed");
        return EXIT_FAILURE;
    }

    memset_random(srcA, FTP3_BYTES);
    memset_random(srcB, FTP3_BYTES);
    memset_random(srcC, FTP3_BYTES);
    memset(dstA, 0, FTP3_BYTES);
    memset(dstB, 0, FTP3_BYTES);
    memset(dstC, 0, FTP3_BYTES);

    uint64_t round = 0;
    do {
        int slot = (int)(round % 3);
        uint64_t rot = 0x9E3779B97F4A7C15ULL + round;

        ftp3_pass_3ldp_stp(srcA, srcB, srcC, dstA, dstB, dstC, rot, slot);
        ftp3_golden(srcA, srcB, srcC, gold, rot, slot);

        if (memcmp(dstA, gold, FTP3_BYTES) != 0 || memcmp(dstB, gold, FTP3_BYTES) != 0
            || memcmp(dstC, gold, FTP3_BYTES) != 0) {
            for (size_t i = 0; i < FTP3_NUM_ELEMENTS * 2; ++i) {
                if (dstA[i] != gold[i]) {
                    report_fail_msg("fmov_train_3ldp_stp: dstA mismatch at "
                                    "element %zu (round %lu, slot %d): "
                                    "expected 0x%016lx got 0x%016lx "
                                    "(3-src LDP/STP pair form, cross-file "
                                    "FMOV consumer chain)",
                                    i / 2, (unsigned long)round, slot,
                                    (unsigned long)gold[i],
                                    (unsigned long)dstA[i]);
                    free(srcA); free(srcB); free(srcC);
                    free(dstA); free(dstB); free(dstC); free(gold);
                    return EXIT_FAILURE;
                }
            }
        }
        ++round;
    } while (test_time_condition(test));

    free(srcA); free(srcB); free(srcC);
    free(dstA); free(dstB); free(dstC); free(gold);
    return EXIT_SUCCESS;
}

DECLARE_TEST(fmov_train_3ldp_stp, "LDP/STP pair-form twin of fmov_train_3ldr_str: post-indexed Q-pair load trains, 3 sources (6 Q per body) + cross-register-file FMOV consumer chain + chained slot-rotated add/eor/sub + 3 pair stores, bit-exact scalar golden per round (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fmov_train_3ldp_stp_init,
    .test_run = fmov_train_3ldp_stp_run,
    .test_cleanup = fmov_train_3ldp_stp_cleanup,
    .fracture_loop_count = -1,   /* never fracture: pass rounds are fast */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
