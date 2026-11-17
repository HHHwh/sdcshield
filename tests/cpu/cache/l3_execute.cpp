/**
 * @file
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test l3_execute
 * @parblock
 * Reads and executes code through the re-fetch path at the same time
 * (633 #30 lineage, user spec 2026-11-02).  Fetch-path honesty
 * (2026-11-17 L3 audit): ic ivau invalidates to the PoU, which on
 * this class of part is the private unified L2 — the 256 B hot
 * worker stays L2-resident, so the forced re-fetch is served from
 * L2 in practice and L3 only participates on an L2 miss.  This is
 * an instruction-fetch-path SDC detector, not an L3 test; the
 * historical l3 name is kept for registry stability.
 *
 * Mechanism: every iteration the worker function's instruction-cache
 * lines are invalidated (ic ivau per 64B line, then dsb ish + isb —
 * the ARM-specified fetch-resync sequence), so the next call must
 * re-fetch the code through the L2/L3 fetch path; the executed
 * result is bit-compared against an INIT-TIME snapshot of the same
 * function on the same inputs.
 *
 * This is the same-source cross-time recompute pattern the eigen
 * family uses: a bit flip on the instruction/fetch path makes the
 * post-flush execution differ from the init snapshot — which is
 * exactly the SDC this test hunts (rare, but the 633 #30
 * description's whole point).  Golden is a fixed deterministic input
 * table (64 pairs, including b == 0 boundary cases) so the snapshot
 * is stable across runs.
 *
 * Design traps found in the user's draft (all fixed here):
 *   1. worker and software_ref were LITERALLY IDENTICAL functions —
 *      GCC -O2's -fipa-icf folds identical functions to one address,
 *      so flushing "worker" also flushed the "reference" (self-
 *      verification).  The reference is gone; the init snapshot
 *      replaces it.
 *   2. a 1e-9 floating tolerance would swallow ULP-level SDC — the
 *      compare is now bitwise (same function + same input must be
 *      bit-identical; deterministic FP, no fast-math).
 *   3. the local store->load "consistency" pass (tmp = val; loaded
 *      = tmp;) is the same compiler-forwarding fake as in the llc
 *      pair — removed.
 *   4. __builtin___clear_cache is actually CORRECT here (it
 *      invalidates the I-cache — what this test wants); we emit the
 *      explicit ic ivau + dsb + isb sequence anyway for clarity and
 *      to pin the exact instructions under test.
 *
 * Concurrency: the input table and golden are read-only shared
 * (init once); each thread invalidates its OWN core's I-cache lines
 * — ic ivau acts on the executing PE — and re-fetches; the code
 * itself is never written.  Adaptations vs the draft (family set):
 * framework-deterministic inputs (no runtime RNG at all), no
 * per-fail fprintf, .fracture_loop_count = -1, _cleanup naming.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

static constexpr int   L3E_NUM_INPUTS = 64;
static constexpr size_t L3E_FLUSH_BYTES = 256;   /* covers worker's code */
static constexpr size_t L3E_LINE = 64;

/* The function under test: plain deterministic FP math, noinline so
 * every call is a real fetch+execute of THIS code. */
static __attribute__((noinline)) double l3e_worker(double a, double b) {
    double denom = (b == 0.0) ? 1.0 : b;
    return a * a + b * b - a * b + a / denom;
}

namespace { /* ODR: per-TU L3EData */
struct L3EData {
    double a[L3E_NUM_INPUTS];
    double b[L3E_NUM_INPUTS];
    uint64_t golden[L3E_NUM_INPUTS];   /* init-time bit snapshot */
};
}

/* invalidate the I-cache lines covering [start, start+len), then
 * resync fetch (dsb ish + isb) — the ARM-specified sequence */
static inline void l3e_flush_icache(const void *start, size_t len) {
    const char *p = reinterpret_cast<const char *>(
        reinterpret_cast<uintptr_t>(start) & ~(uintptr_t)(L3E_LINE - 1));
    const char *end = reinterpret_cast<const char *>(start) + len;
    for (; p < end; p += L3E_LINE)
        __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

static int l3_execute_init(struct test *test) {
    auto *d = new L3EData;
    if (!d) return EXIT_FAILURE;

    /* deterministic inputs; b == 0 at i = 12, 25, 38, 51 (boundary
     * branch coverage); no runtime RNG anywhere in this test */
    for (int i = 0; i < L3E_NUM_INPUTS; ++i) {
        d->a[i] = (double)(i - 32) * 0.0625;
        d->b[i] = (double)((i * 7) % 13) * 0.25 - 1.5;
        double r = l3e_worker(d->a[i], d->b[i]);
        memcpy(&d->golden[i], &r, sizeof(r));
    }
    test->data = d;
    return EXIT_SUCCESS;
}

static int l3_execute_cleanup(struct test *test) {
    delete static_cast<L3EData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int l3_execute_run(struct test *test, int cpu) {
    (void)cpu;
    auto *d = static_cast<L3EData *>(test->data);

    do {
        /* force the next call to re-fetch the code from L2/L3 */
        l3e_flush_icache(reinterpret_cast<const void *>(&l3e_worker),
                         L3E_FLUSH_BYTES);

        for (int i = 0; i < L3E_NUM_INPUTS; ++i) {
            double r = l3e_worker(d->a[i], d->b[i]);
            uint64_t bits;
            memcpy(&bits, &r, sizeof(bits));

            if (memcmp(&bits, &d->golden[i], sizeof(bits)) != 0) {
                report_fail_msg("l3_execute: execution differs from "
                                "init snapshot after I-cache reload: "
                                "input %d (a=%.6e b=%.6e): expected "
                                "0x%016lx got 0x%016lx (fetch-path "
                                "corruption?)", i, d->a[i], d->b[i],
                                (unsigned long)d->golden[i],
                                (unsigned long)bits);
                return EXIT_FAILURE;
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(l3_execute, "Forces instruction re-fetch (ic-ivau + dsb/isb resync) and bit-compares executed results to the init snapshot (fetch-path SDC; re-fetch served from L2-the-PoU in practice, historical l3 name kept) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = l3_execute_init,
    .test_run = l3_execute_run,
    .test_cleanup = l3_execute_cleanup,
    .fracture_loop_count = -1,   /* never fracture: ic-flush rounds are slow */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
