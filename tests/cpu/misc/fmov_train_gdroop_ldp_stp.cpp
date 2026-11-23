/**
 * @file
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test fmov_train_gdroop_ldp_stp
 * @parblock
 * LDP/STP pair-form twin of fmov_train_gdroop_ldr_str: identical
 * synchronized-waveform structure, barrier machinery, knobs and
 * honest boundaries — only the victim recipe is the parent
 * fmov_train_ldp_stp pair form (post-indexed Q-pair trains, 32B/instr,
 * x4-x11 dual FMOV chains).  The chunk knob is forced EVEN at init
 * (the pair body consumes two elements per iteration).
 *
 * All-core SYNCHRONIZED global-droop excitation (2026-11-21, corrected
 * lineage: the fault machine's reproduction condition was ALL-CORE
 * undervolt — global Vcore -85mV at default temperature, or -70mV at
 * 90C — not a per-fault-core offset).
 *
 * Physics of the correction: a global undervolt lowers the timing
 * baseline of the WHOLE die; the fault core is merely the weakest
 * link.  A single core's (or a few cores') FMA burst cannot pull a
 * server VRM's global output measurably down — local die-level droop
 * does not replicate a global -85mV.  The load-replicable analog is
 * the sum of (a) sustained load-line sag under full-chip current and
 * (b) the transient undershoot when ALL cores step from low to max
 * current together.  Both require every core to switch in LOCKSTEP,
 * which process-level orchestration (two taskset'd sdcshield
 * instances) cannot deliver — there is no shared time base between
 * processes at microsecond scale.  This test therefore runs the
 * synchronization IN-PROCESS: the sandstone fork model gives one child
 * with per-CPU worker threads sharing the address space, and a
 * sense-reversal barrier (mesh_upi_sve B1-001 pattern) aligns them.
 *
 * Synchronized per-round waveform (every worker, in lockstep):
 *
 *   BARRIER  -> all cores aligned
 *   STALL    -> yield loop, all cores at LOW current (the quiet
 *               baseline before the step)
 *   BARRIER  -> the step edge: every core leaves this barrier into
 *               the burst within nanoseconds of each other
 *   chunks:  { AGGRESSOR burst (N FMAs, 8 parallel dependent chains,
 *              optionally worst-toggle) -> GAP (g NOPs) -> VICTIM
 *              chunk (M elements of the fmov_train_ldp_stp pair
 *              recipe, instruction-for-instruction) } until the buffer pass
 *              completes — the chunk loop keeps toggling current
 *              (repeated di/dt edges) while the victim's back-to-back
 *              str;str captures run inside the sagged window
 *   golden recompute (scalar, parent semantics: slot/rot rotate per
 *   round) + memcmp dstA/dstB — EVERY core's result is checked every
 *   round: full coverage, no fault-core knowledge required.
 *
 * Knobs (read at init, -O fmov_train_gdroop_ldp_stp.<k>=<v>):
 *   stall  yield-loop iterations, the low-current half of the global
 *          waveform (default 16384 ~= 6us at 2.6GHz — order-of-
 *          magnitude estimate, uncontended yield ~1 cycle; clamp
 *          0..1048576)
 *   burst  FMAs per aggressor burst (default 4096, clamp 16..65536)
 *   chunk  victim elements per adjacency (default 64, clamp 16..2048)
 *   gap    NOPs between burst and victim (default 0, clamp 0..255)
 *   toggle 1 = worst-toggle aggressor (veorq all-0/all-ones mask per
 *          step; default 0)
 *
 * Honest boundaries:
 *   - The aggressor accumulator is sunk, NOT compared (exciter by
 *     design; FMA-path detection is power_virus_dit's job).  This
 *     test's detection face is the VICTIM path, identical to the
 *     parent pair's.
 *   - Whether the synchronized waveform's sag actually reaches the
 *     85mV-equivalent depth on the fault machine is EXACTLY what the
 *     (stall, burst, chunk, gap, toggle) sweep answers — the physics
 *     above argues it is the only load-side candidate, not that it
 *     is sufficient.  Pre-registered verdict on a HEALTHY machine
 *     (this host): expected 0 fail.
 *   - Barrier spin is throttled (test_time_condition every 131072
 *     spins, mesh B1-001 discipline) so a cancelled/lagging worker
 *     cannot wedge the child past the framework's time budget.
 *
 * Provenance: victim = fmov_train_ldp_stp pair recipe
 * (SIMDFPLoadStoreMemLoopCpyP08 extract, 2026-11-18); aggressor =
 * power_virus_dit burst lineage (8-chain form from fmov_train_droop);
 * barrier = mesh_upi_sve sense-reversal (B1-001).  The ldr/str twin
 * of THIS test lives in fmov_train_gdroop_ldr_str.cpp.
 * ARM64-only; x86-64 stub.
 * @endparblock
 */

#include "sandstone.h"
#include <test_knobs.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <vector>

#if defined(__aarch64__)
#include <arm_neon.h>

#define FGP_NUM_ELEMENTS 2048            /* 16B elements -> 32KB/buffer */
#define FGP_BYTES (FGP_NUM_ELEMENTS * 16)

#define FGP_STALL_DEFAULT 16384
#define FGP_STALL_MIN     0
#define FGP_STALL_MAX     1048576
#define FGP_BURST_DEFAULT 4096
#define FGP_BURST_MIN     16
#define FGP_BURST_MAX     65536
#define FGP_CHUNK_DEFAULT 64
#define FGP_CHUNK_MIN     16
#define FGP_CHUNK_MAX     2048
#define FGP_GAP_DEFAULT   0
#define FGP_GAP_MIN       0
#define FGP_GAP_MAX       255

/* racy-by-intent sink: keeps the aggressor chain live; never read */
static volatile uint32_t fgp_sink;

namespace { /* ODR: per-TU GpData */
struct GpData {
    int stall;
    int burst;
    int chunk;
    int gap;
    int toggle;
    /* sense-reversal barrier state (mesh_upi_sve B1-001 pattern).
     * Valid across the worker THREADS of one child process — the
     * sandstone model runs per-CPU workers of a test inside one
     * fork, so test->data is shared. */
    std::atomic<uint32_t> arrived;
    std::atomic<uint32_t> epoch;
};
}

/*
 * Sense-reversal barrier.  Returns false when the framework's time
 * budget ran out while waiting (the caller must stop cleanly); true
 * after all thread_count() workers have arrived.  -n 1 degenerates
 * to an immediate pass-through.
 */
static bool fgp_barrier(struct test *test, struct GpData *d,
                        uint32_t *my_epoch)
{
    const uint32_t total = (uint32_t)thread_count();
    uint32_t done = d->arrived.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (done == total) {
        d->arrived.store(0, std::memory_order_relaxed);
        d->epoch.store(*my_epoch + 1, std::memory_order_release);
        ++*my_epoch;
        return true;
    }
    uint32_t spin = 0;
    while (d->epoch.load(std::memory_order_acquire) == *my_epoch) {
        if ((++spin & 131071u) == 0) {
            if (!test_time_condition(test))
                return false;      /* out of budget: bail, do not wedge */
        } else {
            __asm__ volatile("yield");
        }
    }
    ++*my_epoch;
    return true;
}

/* STALL: the low-current half of the global waveform.  "yield" lets
 * the core throttle down (power_virus_dit stall lineage). */
static inline void fgp_stall(int n)
{
    for (int i = 0; i < n; ++i)
        __asm__ volatile("yield");
}

/*
 * AGGRESSOR: 8 parallel dependent FMA chains (fmov_train_droop's
 * ftd_burst, power_virus_dit lineage).  toggle=1 adds a per-step
 * veorq with an alternating all-0/all-ones mask (worst toggle).
 * Result XOR-reduced to the caller's sink accumulator (exciter: kept
 * live, never compared).
 */
static inline uint32_t fgp_burst(int n, int toggle)
{
    const float32x4_t a0 = vdupq_n_f32(1.0f);     /* 0x3f800000 */
    const float32x4_t a1 = vdupq_n_f32(-2.0f);    /* 0xc0000000 */
    const float32x4_t b0 = vdupq_n_f32(4.0f);     /* 0x40800000 */
    const float32x4_t b1 = vdupq_n_f32(0.125f);   /* 0x3e000000 */
    const uint32x4_t m0 = vdupq_n_u32(0x00000000u);
    const uint32x4_t m1 = vdupq_n_u32(0xffffffffu);
    float32x4_t c0 = vdupq_n_f32(0.0f);
    float32x4_t c1 = vdupq_n_f32(0.0f);
    float32x4_t c2 = vdupq_n_f32(0.0f);
    float32x4_t c3 = vdupq_n_f32(0.0f);
    float32x4_t c4 = vdupq_n_f32(0.0f);
    float32x4_t c5 = vdupq_n_f32(0.0f);
    float32x4_t c6 = vdupq_n_f32(0.0f);
    float32x4_t c7 = vdupq_n_f32(0.0f);
    int n8 = n / 8;
    if (toggle) {
        for (int i = 0; i < n8; ++i) {
            const uint32x4_t m = (i & 1) ? m1 : m0;
            c0 = vreinterpretq_f32_u32(veorq_u32(
                    vreinterpretq_u32_f32(vfmaq_f32(c0, a0, b0)), m));
            c1 = vreinterpretq_f32_u32(veorq_u32(
                    vreinterpretq_u32_f32(vfmaq_f32(c1, a1, b1)), m));
            c2 = vreinterpretq_f32_u32(veorq_u32(
                    vreinterpretq_u32_f32(vfmaq_f32(c2, a0, b0)), m));
            c3 = vreinterpretq_f32_u32(veorq_u32(
                    vreinterpretq_u32_f32(vfmaq_f32(c3, a1, b1)), m));
            c4 = vreinterpretq_f32_u32(veorq_u32(
                    vreinterpretq_u32_f32(vfmaq_f32(c4, a0, b0)), m));
            c5 = vreinterpretq_f32_u32(veorq_u32(
                    vreinterpretq_u32_f32(vfmaq_f32(c5, a1, b1)), m));
            c6 = vreinterpretq_f32_u32(veorq_u32(
                    vreinterpretq_u32_f32(vfmaq_f32(c6, a0, b0)), m));
            c7 = vreinterpretq_f32_u32(veorq_u32(
                    vreinterpretq_u32_f32(vfmaq_f32(c7, a1, b1)), m));
        }
    } else {
        for (int i = 0; i < n8; ++i) {
            c0 = vfmaq_f32(c0, a0, b0);
            c1 = vfmaq_f32(c1, a1, b1);
            c2 = vfmaq_f32(c2, a0, b0);
            c3 = vfmaq_f32(c3, a1, b1);
            c4 = vfmaq_f32(c4, a0, b0);
            c5 = vfmaq_f32(c5, a1, b1);
            c6 = vfmaq_f32(c6, a0, b0);
            c7 = vfmaq_f32(c7, a1, b1);
        }
    }
    uint32x4_t x = vreinterpretq_u32_f32(c0);
    x = veorq_u32(x, vreinterpretq_u32_f32(c1));
    x = veorq_u32(x, vreinterpretq_u32_f32(c2));
    x = veorq_u32(x, vreinterpretq_u32_f32(c3));
    x = veorq_u32(x, vreinterpretq_u32_f32(c4));
    x = veorq_u32(x, vreinterpretq_u32_f32(c5));
    x = veorq_u32(x, vreinterpretq_u32_f32(c6));
    x = veorq_u32(x, vreinterpretq_u32_f32(c7));
    return vaddvq_u32(x);
}

/* GAP: g NOPs, phase adjust between burst and victim. */
static inline void fgp_gap(int g)
{
    for (int i = 0; i < g; ++i)
        __asm__ volatile("nop");
}

/*
 * VICTIM chunk: the fmov_train_ldp_stp hot loop instruction-for-
 * instruction (pair form: post-indexed LDP/STP Q-pairs, 32B/instr,
 * x4-x11 dual FMOV chains, same slot rotation), starting at element
 * start_elem, running count elements.  count must be EVEN — the pair
 * body consumes two elements; the caller guarantees it (chunk knob is
 * forced even at init).
 */
static void fgp_victim_chunk(const uint64_t *a, const uint64_t *b,
                             uint64_t *da, uint64_t *db, uint64_t rot,
                             int slot, int start_elem, int count)
{
    const uint64_t *pa = a + (size_t)start_elem * 2;
    const uint64_t *pb = b + (size_t)start_elem * 2;
    uint64_t *pda = da + (size_t)start_elem * 2;
    uint64_t *pdb = db + (size_t)start_elem * 2;

    for (int i = 0; i < count; i += 2) {
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

/* scalar bit-exact golden: g = b <op> (a + rot) — parent formula verbatim */
static void fgp_golden(const uint64_t *a, const uint64_t *b,
                       uint64_t *g, uint64_t rot, int slot)
{
    size_t n = FGP_NUM_ELEMENTS * 2;
    for (size_t i = 0; i < n; ++i) {
        uint64_t x = a[i] + rot;
        g[i] = (slot == 0) ? (b[i] + x)
             : (slot == 1) ? (b[i] ^ x)
             :               (b[i] - x);
    }
}

static int fmov_train_gdroop_ldp_stp_init(struct test *test)
{
    auto *d = new GpData;
    if (!d)
        return EXIT_FAILURE;
    d->arrived.store(0, std::memory_order_relaxed);
    d->epoch.store(0, std::memory_order_relaxed);

    int64_t v;
    v = get_testspecific_knob_value_int(test, "stall", FGP_STALL_DEFAULT);
    d->stall = (int)v;
    if (d->stall < FGP_STALL_MIN) d->stall = FGP_STALL_MIN;
    if (d->stall > FGP_STALL_MAX) d->stall = FGP_STALL_MAX;

    v = get_testspecific_knob_value_int(test, "burst", FGP_BURST_DEFAULT);
    d->burst = (int)v;
    if (d->burst < FGP_BURST_MIN) d->burst = FGP_BURST_MIN;
    if (d->burst > FGP_BURST_MAX) d->burst = FGP_BURST_MAX;

    v = get_testspecific_knob_value_int(test, "chunk", FGP_CHUNK_DEFAULT);
    d->chunk = (int)v;
    if (d->chunk < FGP_CHUNK_MIN) d->chunk = FGP_CHUNK_MIN;
    if (d->chunk > FGP_CHUNK_MAX) d->chunk = FGP_CHUNK_MAX;
    d->chunk &= ~1;   /* pair form: 2 elements per body, force even */

    v = get_testspecific_knob_value_int(test, "gap", FGP_GAP_DEFAULT);
    d->gap = (int)v;
    if (d->gap < FGP_GAP_MIN) d->gap = FGP_GAP_MIN;
    if (d->gap > FGP_GAP_MAX) d->gap = FGP_GAP_MAX;

    v = get_testspecific_knob_value_int(test, "toggle", 0);
    d->toggle = (v != 0) ? 1 : 0;

    test->data = d;
    return EXIT_SUCCESS;
}

static int fmov_train_gdroop_ldp_stp_cleanup(struct test *test)
{
    delete static_cast<GpData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int fmov_train_gdroop_ldp_stp_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<GpData *>(test->data);
    const int stall = d->stall;
    const int burst = d->burst;
    const int chunk = d->chunk;
    const int gap = d->gap;
    const int toggle = d->toggle;

    /* per-thread buffers (parent geometry: 32KB each, L1D-domain); the
     * asm-touched buffers are 32B-aligned (LDP/STP Q-pair access size)
     * via aligned_alloc — std::vector's heap block only guarantees 16B
     * (max_align_t), same discipline as the parent fmov_train_ldp_stp */
    auto *srcA = static_cast<uint64_t *>(aligned_alloc(32, FGP_BYTES));
    auto *srcB = static_cast<uint64_t *>(aligned_alloc(32, FGP_BYTES));
    auto *dstA = static_cast<uint64_t *>(aligned_alloc(32, FGP_BYTES));
    auto *dstB = static_cast<uint64_t *>(aligned_alloc(32, FGP_BYTES));
    auto *gold = static_cast<uint64_t *>(aligned_alloc(32, FGP_BYTES));
    if (!srcA || !srcB || !dstA || !dstB || !gold) {
        free(srcA); free(srcB); free(dstA); free(dstB); free(gold);
        report_fail_msg("fmov_train_gdroop_ldp_stp: buffer allocation failed");
        return EXIT_FAILURE;
    }

    memset_random(srcA, FGP_BYTES);
    memset_random(srcB, FGP_BYTES);
    memset(dstA, 0, FGP_BYTES);
    memset(dstB, 0, FGP_BYTES);

    uint32_t sink_acc = 0;
    uint32_t my_epoch = 0;

    uint64_t round = 0;
    do {
        const int slot = (int)(round % 3);
        const uint64_t rot = 0x9E3779B97F4A7C15ULL + round;

        /* round-start barrier: align all cores, then the quiet half */
        if (!fgp_barrier(test, d, &my_epoch))
            break;
        fgp_stall(stall);
        /* the step-edge barrier: every core leaves into the burst
         * together — the synchronized global current step */
        if (!fgp_barrier(test, d, &my_epoch))
            break;

        /* burst/victim chunks inside the sagged window; the chunk
         * loop keeps toggling the aggregate current (repeated edges) */
        for (int start = 0; start < FGP_NUM_ELEMENTS; start += chunk) {
            const int m = (chunk < FGP_NUM_ELEMENTS - start)
                        ? chunk : (FGP_NUM_ELEMENTS - start);
            sink_acc ^= fgp_burst(burst, toggle);
            fgp_gap(gap);
            fgp_victim_chunk(srcA, srcB, dstA, dstB, rot, slot, start, m);
        }

        fgp_golden(srcA, srcB, gold, rot, slot);

        if (memcmp(dstA, gold, FGP_BYTES) != 0 || memcmp(dstB, gold, FGP_BYTES) != 0) {
            for (size_t i = 0; i < FGP_NUM_ELEMENTS * 2; ++i) {
                if (dstA[i] != gold[i]) {
                    report_fail_msg("fmov_train_gdroop_ldp_stp: dstA mismatch "
                                    "at element %zu (round %lu, slot %d, "
                                    "stall %d burst %d chunk %d gap %d "
                                    "toggle %d): expected 0x%016lx got "
                                    "0x%016lx (global-droop-excited FMOV "
                                    "consumer chain)",
                                    i / 2, (unsigned long)round, slot,
                                    stall, burst, chunk, gap, toggle,
                                    (unsigned long)gold[i],
                                    (unsigned long)dstA[i]);
                    return EXIT_FAILURE;
                }
            }
        }
        ++round;
    } while (test_time_condition(test));

    /* keep the aggressor chain live (sink, never compared) */
    fgp_sink = sink_acc;

    free(srcA); free(srcB); free(dstA); free(dstB); free(gold);
    return EXIT_SUCCESS;
}

DECLARE_TEST(fmov_train_gdroop_ldp_stp, "All-core synchronized global-droop excitation (all-core undervolt lineage): sense-reversal barrier aligns every worker through stall (low current) -> synchronized FMA burst (global current step) -> fmov_train_ldp_stp recipe chunks inside the sagged window, byte-exact per-round scalar golden on every core; knobs stall/burst/chunk/gap/toggle (-O) sweep the global waveform (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fmov_train_gdroop_ldp_stp_init,
    .test_run = fmov_train_gdroop_ldp_stp_run,
    .test_cleanup = fmov_train_gdroop_ldp_stp_cleanup,
    .fracture_loop_count = -1,   /* never fracture: barrier rides the round loop */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST

#else  /* !__aarch64__ */

static int fmov_train_gdroop_ldp_stp_init(struct test *test)  { (void)test; return EXIT_SUCCESS; }
static int fmov_train_gdroop_ldp_stp_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SUCCESS; }
static int fmov_train_gdroop_ldp_stp_cleanup(struct test *test) { (void)test; return EXIT_SUCCESS; }

DECLARE_TEST(fmov_train_gdroop_ldp_stp, "All-core synchronized global-droop excitation (ARM64-only)")
    .test_init    = fmov_train_gdroop_ldp_stp_init,
    .test_run     = fmov_train_gdroop_ldp_stp_run,
    .test_cleanup = fmov_train_gdroop_ldp_stp_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif
