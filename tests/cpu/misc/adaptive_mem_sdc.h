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
enum AdaptLoad  { LOAD_FP64ROT, LOAD_INTROT, LOAD_EMPTY, LOAD_MLA, LOAD_SUB,
                  LOAD_XFMOV };   /* 2026-11-18: cross-file FMOV consumer chain */
enum AdaptSweep { SWEEP_ALT, SWEEP_MONO };

#define ADAPT_COMBO_COUNT 36
#define ADAPT_SLICE_SECONDS 1800
/* Fallback element count for the engine defensive path only (test->data
 * missing); the REAL footprint is drawn at test_init (2026-11-25):
 * elements-per-buffer tiers {64K, 128K, 256K, 512K} weighted 40/30/20/10
 * (512K highest), then memory-guarded — ncpu × 9 buffers × count ×
 * elem_bytes (16 on NEON, svcntb() on SVE) must fit MemAvailable × 0.8;
 * over-budget random picks downgrade to the largest tier that fits, a 64K
 * that still does not fit skips the test honestly (EXIT_SKIP, rule 6).
 * ADAPTIVE_MEM_SDC_COUNT={64|128|256|512} pins one tier and BYPASSES the
 * downgrade (stderr warning when over budget — explicit operator intent
 * keeps campaign determinism); ADAPTIVE_MEM_SDC_MEM_BUDGET_MB overrides
 * the MemAvailable input (verification/ops).  The EFFECTIVE tier rides
 * the fail tag ([.../count%dK]) and the COVFILE 6th column. */
#define ADAPT_COUNT 1024

struct ComboDef {
    enum AdaptDtype dt;
    int ldr;                             /* independent ALU sources: 2 or 3 */
    int str;                             /* chained copies per element (2026-11-25: str axis = chain length) —
                                            str2 = 2 copies, str3 = 3, str4 = 6 (the literal A->B->C->A->B->A cycle);
                                            every link reloads the just-stored address, dst is the sink */
    enum AdaptLoad load;
    const char *label;
};

/* Stable recipe table (combo# = index = row): FP64 segment first, then
 * int; within each: ldr 2 before 3, str 2/3/4, load order
 * INTROT > EMPTY > MLA > SUB > XFMOV (rows #30-35, 2026-11-18 user
 * directive: the cross-file FMOV middle load as a family choice; the
 * standalone validation pair lives in fmov_train_{ldr_str,ldp_stp}.cpp).
 * EMPTY lives only in the int segment (with no ALU the dtype is
 * unobservable — deduped in v2). */
extern const struct ComboDef kComboTable[ADAPT_COMBO_COUNT];

/* Rotation order: slice slot -> combo ID.  Default = measured
 * first-detection TTF from the 2026-11-14 30-combo locked sweep
 * (fastest #12 first), zero arms as the reserve layer in table order,
 * then the six XFMOV rows uncalibrated (11-18 standalone 10min
 * full-core arms: 0 fail on this host — negative-space data, other
 * hosts may differ per the machine-generality rule);
 * source: docs/20261114/adaptive_combo_sweep/RESULTS.md.  Overridable
 * per run with ADAPTIVE_MEM_SDC_ORDER (full permutation of 0..35). */
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
/* RDVL element width in bytes (svcntb()).  Defined in the SVE TU; the
 * init-time memory guard calls it ONLY after HWCAP advertised SVE. */
int adaptive_sve_probe_vl_bytes(void);

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
    int count;                           /* effective footprint tier, elements (2026-11-25) */
    int profile;                         /* 0=standard, 1=stress, 2=soak */
};

#ifdef __cplusplus
}
#endif

#endif /* ADAPTIVE_MEM_SDC_H */
