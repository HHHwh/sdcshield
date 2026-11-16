/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared definitions for adaptive_mem_sdc (2026-11-13).
 * Spec: docs/ADAPTIVE_MEM_SDC_DESIGN_20261113.md (v2, approved).
 *
 * Single test case that auto-selects the machine's widest SIMD ISA
 * (SVE2 > SVE > NEON via HWCAP) and sweeps a 30-combo priority
 * sequence (dtype > ldr count > str count > middle load), 30min per
 * combo wall-clock slice, stopping and reporting the combo tag on the
 * first SDC.  Runtime model rides the framework's native fork_each_test
 * cycling: each short-lived child picks its combo from a wall-clock
 * anchor in a small named shared-memory object (verify mode: round-robin
 * counter), so no in-process switching exists.
 */

#ifndef ADAPTIVE_MEM_SDC_H
#define ADAPTIVE_MEM_SDC_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

enum AdaptDtype { DT_FP64, DT_INT };
enum AdaptLoad  { LOAD_FP64ROT, LOAD_INTROT, LOAD_EMPTY, LOAD_MLA, LOAD_SUB };
enum AdaptSweep { SWEEP_ALT, SWEEP_MONO };

#define ADAPT_COMBO_COUNT 30
#define ADAPT_SLICE_SECONDS 1800
#define ADAPT_COUNT 1024                 /* elements per buffer, family convention */

struct ComboDef {
    enum AdaptDtype dt;
    int ldr;                             /* independent ALU sources: 2 or 3 */
    int str;                             /* stores per element: 2/3/4, dst last */
    enum AdaptLoad load;
    const char *label;
};

/* Stable recipe table (combo# = index = row): FP64 segment first, then
 * int; within each: ldr 2 before 3, str 2/3/4, load order
 * INTROT > EMPTY > MLA > SUB.  EMPTY lives only in the int segment
 * (with no ALU the dtype is unobservable — deduped in v2). */
extern const struct ComboDef kComboTable[ADAPT_COMBO_COUNT];

/* Rotation order: slice slot -> combo ID.  Default = measured
 * first-detection TTF from the 2026-11-14 30-combo locked sweep
 * (fastest #12 first), zero arms as the reserve layer in table order;
 * source: docs/20261114/adaptive_combo_sweep/RESULTS.md.  Overridable
 * per run with ADAPTIVE_MEM_SDC_ORDER (full permutation of 0..29). */
extern const int kPriorityOrder[ADAPT_COMBO_COUNT];

/* Shared-memory state — the ONLY cross-child state.  No std::atomic
 * here (placement-new does not work across processes): all atomic ops
 * use GCC __atomic_* builtins on these plain fields. */
struct AdaptiveShm {
    uint32_t ready;                      /* 0 -> 1 by creator (release store) */
    uint32_t verify_ctr;                 /* verify-mode round-robin (fetch_add) */
    uint32_t sdc_flag;                   /* set to 1 after any child detects SDC */
    struct timespec anchor;              /* wall-clock anchor (CLOCK_MONOTONIC) */
};

/* Engine entry points.  Each fully owns one combo for the child's whole
 * lifetime: allocate buffers, fill, recompute golden, run the hot loop
 * under TEST_LOOP, per-sweep memcmp, report with combo tag. */
struct test;
int adaptive_neon_run_combo(struct test *test, int cpu, int combo);
int adaptive_sve_run_combo(struct test *test, int cpu, int combo);

/* Per-child plumbing state (test->data), shared by both engine TUs */
struct AdaptiveMemSdcData {
    struct AdaptiveShm *shm;             /* mmap'd, shared across children */
    int isa;                             /* 0=NEON 1=SVE 2=SVE2 */
    int test_mode;                       /* ADAPTIVE_MEM_SDC_MODE: 0=combat
                                          * (default: priority rotation, sdc_flag
                                          * freezes later children to idle) 1=test
                                          * (survey: rotate through EVERY combo,
                                          * keep running after detections, idle
                                          * after the last slot) */
    int test_slice;                      /* test-mode slice seconds (default 600) */
};

#ifdef __cplusplus
}
#endif

#endif /* ADAPTIVE_MEM_SDC_H */
