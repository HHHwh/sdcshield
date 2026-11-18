/**
 * @file
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test fmov_train_ldp_stp
 * @parblock
 * LDP/STP pair-form twin of fmov_train_ldr_str (2026-11-18, user
 * directive: single ldp/stp standalone test).
 *
 * Identical semantics and golden; only the load/store instruction
 * form changes — post-indexed PAIRS: one LDP Q,Q,#32 loads two
 * elements (32B, single pointer update — the libc-memcpy inner-loop
 * encoding the user's SIMDFPLoadStoreMemLoopCpyP08 disassembly uses),
 * one STP Q,Q,#32 stores two elements back-to-back.  Two elements per
 * iteration, so the cross-register-file FMOV consumer chain runs
 * twice per body (x4-x7 for element 0, x8-x11 for element 1):
 *
 *   ldp q0, q1, [srcA], #32 ; ldp q2, q3, [srcB], #32  (2 sources)
 *   fmov x, v.d[..] / add / fmov v.d[..], x             (RF round-trip)
 *   <OP> v6.2d / v7.2d                                   (slot rotation)
 *   stp q6, q7, [dstA], #32 ; stp q6, q7, [dstB], #32   (pair stores)
 *
 * Buffers 32B-aligned (LDP/STP Q-pair access size).  Integer u64
 * lanes, bit-exact scalar golden recomputed per round; per-thread
 * buffers, no cross-thread races.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

#define FTP_NUM_ELEMENTS 2048            /* 16B elements -> 32KB/buffer */
#define FTP_BYTES (FTP_NUM_ELEMENTS * 16)

namespace { /* ODR: per-TU FTPData */
struct FTPData {
    int dummy;                           /* buffers are per-thread, in run() */
};
}

/*
 * One pass, pair form: two elements per body.  slot: 0=add 1=eor
 * 2=sub on the SIMD side; GPR consumer is a plain ADD of the rotating
 * constant (both lanes, both elements — same semantics as the ldr/str
 * twin, golden shared).
 */
static void ftp_pass_ldp_stp(const uint64_t *a, const uint64_t *b,
                             uint64_t *da, uint64_t *db, uint64_t rot, int slot)
{
    const uint64_t *pa = a, *pb = b;
    uint64_t *pda = da, *pdb = db;

    for (size_t i = 0; i < FTP_NUM_ELEMENTS; i += 2) {
        if (slot == 0) {
            __asm__ volatile(
                "ldp  q0, q1, [%[a]], #32\n\t"
                "ldp  q2, q3, [%[b]], #32\n\t"
                "fmov x4, d0\n\t"
                "fmov x5, v0.d[1]\n\t"
                "add  x6, x4, %[rot]\n\t"
                "add  x7, x5, %[rot]\n\t"
                "fmov d4, x6\n\t"
                "fmov v4.d[1], x7\n\t"
                "fmov x8, d1\n\t"
                "fmov x9, v1.d[1]\n\t"
                "add  x10, x8, %[rot]\n\t"
                "add  x11, x9, %[rot]\n\t"
                "fmov d5, x10\n\t"
                "fmov v5.d[1], x11\n\t"
                "add  v6.2d, v2.2d, v4.2d\n\t"
                "add  v7.2d, v3.2d, v5.2d\n\t"
                "stp  q6, q7, [%[da]], #32\n\t"
                "stp  q6, q7, [%[db]], #32\n\t"
                : [a] "+r" (pa), [b] "+r" (pb), [da] "+r" (pda), [db] "+r" (pdb)
                : [rot] "r" (rot)
                : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
                  "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "memory");
        } else if (slot == 1) {
            __asm__ volatile(
                "ldp  q0, q1, [%[a]], #32\n\t"
                "ldp  q2, q3, [%[b]], #32\n\t"
                "fmov x4, d0\n\t"
                "fmov x5, v0.d[1]\n\t"
                "add  x6, x4, %[rot]\n\t"
                "add  x7, x5, %[rot]\n\t"
                "fmov d4, x6\n\t"
                "fmov v4.d[1], x7\n\t"
                "fmov x8, d1\n\t"
                "fmov x9, v1.d[1]\n\t"
                "add  x10, x8, %[rot]\n\t"
                "add  x11, x9, %[rot]\n\t"
                "fmov d5, x10\n\t"
                "fmov v5.d[1], x11\n\t"
                "eor  v6.16b, v2.16b, v4.16b\n\t"
                "eor  v7.16b, v3.16b, v5.16b\n\t"
                "stp  q6, q7, [%[da]], #32\n\t"
                "stp  q6, q7, [%[db]], #32\n\t"
                : [a] "+r" (pa), [b] "+r" (pb), [da] "+r" (pda), [db] "+r" (pdb)
                : [rot] "r" (rot)
                : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
                  "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "memory");
        } else {
            __asm__ volatile(
                "ldp  q0, q1, [%[a]], #32\n\t"
                "ldp  q2, q3, [%[b]], #32\n\t"
                "fmov x4, d0\n\t"
                "fmov x5, v0.d[1]\n\t"
                "add  x6, x4, %[rot]\n\t"
                "add  x7, x5, %[rot]\n\t"
                "fmov d4, x6\n\t"
                "fmov v4.d[1], x7\n\t"
                "fmov x8, d1\n\t"
                "fmov x9, v1.d[1]\n\t"
                "add  x10, x8, %[rot]\n\t"
                "add  x11, x9, %[rot]\n\t"
                "fmov d5, x10\n\t"
                "fmov v5.d[1], x11\n\t"
                "sub  v6.2d, v2.2d, v4.2d\n\t"
                "sub  v7.2d, v3.2d, v5.2d\n\t"
                "stp  q6, q7, [%[da]], #32\n\t"
                "stp  q6, q7, [%[db]], #32\n\t"
                : [a] "+r" (pa), [b] "+r" (pb), [da] "+r" (pda), [db] "+r" (pdb)
                : [rot] "r" (rot)
                : "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
                  "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "memory");
        }
    }
}

/* scalar bit-exact golden (identical semantics to the ldr/str twin) */
static void ftp_golden(const uint64_t *a, const uint64_t *b,
                       uint64_t *g, uint64_t rot, int slot)
{
    size_t n = FTP_NUM_ELEMENTS * 2;
    for (size_t i = 0; i < n; ++i) {
        uint64_t x = a[i] + rot;
        g[i] = (slot == 0) ? (b[i] + x)
             : (slot == 1) ? (b[i] ^ x)
             :               (b[i] - x);
    }
}

static int fmov_train_ldp_stp_init(struct test *test)
{
    auto *d = new FTPData;
    if (!d)
        return EXIT_FAILURE;
    d->dummy = 0;
    test->data = d;
    return EXIT_SUCCESS;
}

static int fmov_train_ldp_stp_cleanup(struct test *test)
{
    delete static_cast<FTPData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int fmov_train_ldp_stp_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;

    /* per-thread buffers; the four asm-touched buffers are 32B-aligned
     * (LDP/STP Q-pair access size) via aligned_alloc — std::vector's
     * heap block is only guaranteed 16B (max_align_t) and alignas on
     * the vector OBJECT does not align its buffer. */
    auto *srcA = static_cast<uint64_t *>(aligned_alloc(32, FTP_BYTES));
    auto *srcB = static_cast<uint64_t *>(aligned_alloc(32, FTP_BYTES));
    auto *dstA = static_cast<uint64_t *>(aligned_alloc(32, FTP_BYTES));
    auto *dstB = static_cast<uint64_t *>(aligned_alloc(32, FTP_BYTES));
    auto *gold = static_cast<uint64_t *>(aligned_alloc(32, FTP_BYTES));
    if (!srcA || !srcB || !dstA || !dstB || !gold) {
        free(srcA); free(srcB); free(dstA); free(dstB); free(gold);
        report_fail_msg("fmov_train_ldp_stp: buffer allocation failed");
        return EXIT_FAILURE;
    }

    memset_random(srcA, FTP_BYTES);
    memset_random(srcB, FTP_BYTES);
    memset(dstA, 0, FTP_BYTES);
    memset(dstB, 0, FTP_BYTES);

    uint64_t round = 0;
    do {
        int slot = (int)(round % 3);
        uint64_t rot = 0x9E3779B97F4A7C15ULL + round;

        ftp_pass_ldp_stp(srcA, srcB, dstA, dstB, rot, slot);
        ftp_golden(srcA, srcB, gold, rot, slot);

        if (memcmp(dstA, gold, FTP_BYTES) != 0 || memcmp(dstB, gold, FTP_BYTES) != 0) {
            for (size_t i = 0; i < FTP_NUM_ELEMENTS * 2; ++i) {
                if (dstA[i] != gold[i]) {
                    report_fail_msg("fmov_train_ldp_stp: dstA mismatch at "
                                    "element %zu (round %lu, slot %d): "
                                    "expected 0x%016lx got 0x%016lx "
                                    "(LDP/STP pair form, cross-file FMOV "
                                    "consumer chain)",
                                    i / 2, (unsigned long)round, slot,
                                    (unsigned long)gold[i],
                                    (unsigned long)dstA[i]);
                    free(srcA); free(srcB); free(dstA); free(dstB); free(gold);
                    return EXIT_FAILURE;
                }
            }
        }
        ++round;
    } while (test_time_condition(test));

    free(srcA); free(srcB); free(dstA); free(dstB); free(gold);
    return EXIT_SUCCESS;
}

DECLARE_TEST(fmov_train_ldp_stp, "LDP/STP pair-form twin of fmov_train_ldr_str: post-indexed Q-pair load trains (32B/instr) + cross-register-file FMOV consumer chain + slot-rotated add/eor/sub + pair stores, bit-exact scalar golden per round (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fmov_train_ldp_stp_init,
    .test_run = fmov_train_ldp_stp_run,
    .test_cleanup = fmov_train_ldp_stp_cleanup,
    .fracture_loop_count = -1,   /* never fracture: pass rounds are fast */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
