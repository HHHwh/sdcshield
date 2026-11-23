/**
 * @file
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test fmov_train_gdroop_ldp_stp_intr
 * @parblock
 * INTRINSICS-BUILT LDP/STP pair-form twin of fmov_train_gdroop_ldr_str
 * _intr: same intrinsics-built cross-RF chain, same barrier/knobs/
 * golden; the pair load/store encoding (ldp/stp q,q post-index #32)
 * is pinned in asm — no intrinsic can express it.  Together with the
 * asm-built fmov_train_gdroop_ldp_stp it forms the pair-form arm of
 * the encoding discriminator.
 *
 * INTRINSICS-BUILT twin of fmov_train_gdroop_ldr_str (fp64_dump
 * minimal-asm pattern, 2026-11-21 user directive).  Synchronized
 * waveform, barrier, knobs, golden and honest boundaries are
 * IDENTICAL to the asm-built twin — the ONLY variable is how the
 * victim's cross-register-file consumer chain is constructed:
 * intrinsics with GCC-chosen encodings here (vgetq_lane->umov,
 * vcreate->fmov d,x, vcombine->ins; objdump-pinned) vs hand-pinned
 * fmov forms there.  Together they are a controlled encoding
 * discriminator: does the excitation need the exact fmov encodings,
 * or does any SIMD->GPR->SIMD round-trip do?
 *
 * asm inventory (minimal-necessary, fp64_dump discipline):
 *   - fgj_ldp/fgj_stp: the PAIR load/store encodings (ldp/stp q,q
 *     post-index #32 — inexpressible in intrinsics; this twin's
 *     distinguishing form)
 *   - four zero-instruction register-class pins in fgj_cross_rf
 *     (force the cross-RF trip through the GPR file; collapse record
 *     in the ldr/str intrinsics twin)
 *   - yield (stall/barrier spin) and nop (gap): hint instructions,
 *     no intrinsic exists
 * Everything else — burst, chain ops, stores, barrier, golden — is
 * intrinsics / plain C++.
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
 *              chunk (M elements of the intrinsics-built cross-RF
 *              recipe, pair-form loads/stores) } until the buffer pass
 *              completes — the chunk loop keeps toggling current
 *              (repeated di/dt edges) while the victim's back-to-back
 *              str;str captures run inside the sagged window
 *   golden recompute (scalar, parent semantics: slot/rot rotate per
 *   round) + memcmp dstA/dstB — EVERY core's result is checked every
 *   round: full coverage, no fault-core knowledge required.
 *
 * Knobs (read at init, -O fmov_train_gdroop_ldp_stp_intr.<k>=<v>):
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
 * Provenance: victim = intrinsics-built cross-RF chain with pinned
 * pair encodings (semantics of the fmov_train_ldp_stp recipe, g = b
 * <op> (a + rot) per u64 lane); aggressor =
 * power_virus_dit burst lineage (8-chain form from fmov_train_droop);
 * barrier = mesh_upi_sve sense-reversal (B1-001).  The ldr/str
 * intrinsics twin lives in fmov_train_gdroop_ldr_str_intr.cpp; the
 * asm-built pair twin in fmov_train_gdroop_ldp_stp.cpp.
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

#define FGJ_NUM_ELEMENTS 2048            /* 16B elements -> 32KB/buffer */
#define FGJ_BYTES (FGJ_NUM_ELEMENTS * 16)

#define FGJ_STALL_DEFAULT 16384
#define FGJ_STALL_MIN     0
#define FGJ_STALL_MAX     1048576
#define FGJ_BURST_DEFAULT 4096
#define FGJ_BURST_MIN     16
#define FGJ_BURST_MAX     65536
#define FGJ_CHUNK_DEFAULT 64
#define FGJ_CHUNK_MIN     16
#define FGJ_CHUNK_MAX     2048
#define FGJ_GAP_DEFAULT   0
#define FGJ_GAP_MIN       0
#define FGJ_GAP_MAX       255

/* racy-by-intent sink: keeps the aggressor chain live; never read */
static volatile uint32_t fgj_sink;

namespace { /* ODR: per-TU GjData */
struct GjData {
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
static bool fgj_barrier(struct test *test, struct GjData *d,
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
static inline void fgj_stall(int n)
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
static inline uint32_t fgj_burst(int n, int toggle)
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
static inline void fgj_gap(int g)
{
    for (int i = 0; i < g; ++i)
        __asm__ volatile("nop");
}

/*
 * VICTIM chunk, INTRINSICS-BUILT pair form.  The chain is the same
 * intrinsics-built cross-RF consumer as the ldr/str twin (extract ->
 * register-class pins -> GPR add -> recombine -> slot op); the ONLY
 * asm beyond the zero-instruction pins is the PAIR load/store
 * encoding — ldp/stp q,q with post-index #32 has no intrinsic, so it
 * is pinned directly (minimal-necessary: the pair encoding IS this
 * twin's distinguishing form).
 */
static inline void fgj_ldp(uint64x2_t &x0, uint64x2_t &x1,
                           const uint64_t *&p)
{
    __asm__ volatile("ldp %q0, %q1, [%2], #32"
                     : "=w"(x0), "=w"(x1), "+r"(p)
                     :
                     : "memory");
}

static inline void fgj_stp(uint64_t *&p, uint64x2_t x0, uint64x2_t x1)
{
    /* p is "+r" (read-write): the post-index writeback modifies the
     * address register, so it MUST be declared as an output — with a
     * plain "r" input the hardware writeback corrupts the register
     * behind the compiler's back (observed: pointer overshoot by 16B
     * per pair, out-of-bounds stores, segfault).  No manual p += 4:
     * the writeback already advances it by 32. */
    __asm__ volatile("stp %q1, %q2, [%0], #32"
                     : "+r"(p)
                     : "w"(x0), "w"(x1)
                     : "memory");
}

/* cross-RF consumer chain, intrinsics-built (identical to the ldr/str
 * twin — see its comment for the collapse record and pin rationale) */
static inline uint64x2_t fgj_cross_rf(uint64x2_t a, uint64_t rot)
{
    uint64_t lo = vgetq_lane_u64(a, 0);
    uint64_t hi = vgetq_lane_u64(a, 1);
    asm("" : "+r"(lo));
    asm("" : "+r"(hi));
    uint64_t rlo = lo + rot;
    uint64_t rhi = hi + rot;
    asm("" : "+r"(rlo));
    asm("" : "+r"(rhi));
    return vcombine_u64(vcreate_u64(rlo), vcreate_u64(rhi));
}

static inline uint64x2_t fgj_op(int slot, uint64x2_t b, uint64x2_t v4)
{
    switch (slot) {
        case 0:  return vaddq_u64(b, v4);
        case 1:  return veorq_u64(b, v4);
        default: return vsubq_u64(b, v4);
    }
}

/* count must be EVEN (pair body = 2 elements); the chunk knob is
 * forced even at init. */
static void fgj_victim_chunk(const uint64_t *a, const uint64_t *b,
                             uint64_t *da, uint64_t *db, uint64_t rot,
                             int slot, int start_elem, int count)
{
    const uint64_t *pa = a + (size_t)start_elem * 2;
    const uint64_t *pb = b + (size_t)start_elem * 2;
    uint64_t *pda = da + (size_t)start_elem * 2;
    uint64_t *pdb = db + (size_t)start_elem * 2;

    for (int i = 0; i < count; i += 2) {
        uint64x2_t a0, a1, b0, b1;
        fgj_ldp(a0, a1, pa);
        fgj_ldp(b0, b1, pb);
        const uint64x2_t r0 = fgj_op(slot, b0, fgj_cross_rf(a0, rot));
        const uint64x2_t r1 = fgj_op(slot, b1, fgj_cross_rf(a1, rot));
        fgj_stp(pda, r0, r1);
        fgj_stp(pdb, r0, r1);
    }
}

/* scalar bit-exact golden: g = b <op> (a + rot) — parent formula verbatim */
static void fgj_golden(const uint64_t *a, const uint64_t *b,
                       uint64_t *g, uint64_t rot, int slot)
{
    size_t n = FGJ_NUM_ELEMENTS * 2;
    for (size_t i = 0; i < n; ++i) {
        uint64_t x = a[i] + rot;
        g[i] = (slot == 0) ? (b[i] + x)
             : (slot == 1) ? (b[i] ^ x)
             :               (b[i] - x);
    }
}

static int fmov_train_gdroop_ldp_stp_intr_init(struct test *test)
{
    auto *d = new GjData;
    if (!d)
        return EXIT_FAILURE;
    d->arrived.store(0, std::memory_order_relaxed);
    d->epoch.store(0, std::memory_order_relaxed);

    int64_t v;
    v = get_testspecific_knob_value_int(test, "stall", FGJ_STALL_DEFAULT);
    d->stall = (int)v;
    if (d->stall < FGJ_STALL_MIN) d->stall = FGJ_STALL_MIN;
    if (d->stall > FGJ_STALL_MAX) d->stall = FGJ_STALL_MAX;

    v = get_testspecific_knob_value_int(test, "burst", FGJ_BURST_DEFAULT);
    d->burst = (int)v;
    if (d->burst < FGJ_BURST_MIN) d->burst = FGJ_BURST_MIN;
    if (d->burst > FGJ_BURST_MAX) d->burst = FGJ_BURST_MAX;

    v = get_testspecific_knob_value_int(test, "chunk", FGJ_CHUNK_DEFAULT);
    d->chunk = (int)v;
    if (d->chunk < FGJ_CHUNK_MIN) d->chunk = FGJ_CHUNK_MIN;
    if (d->chunk > FGJ_CHUNK_MAX) d->chunk = FGJ_CHUNK_MAX;
    d->chunk &= ~1;   /* pair form: 2 elements per body, force even */

    v = get_testspecific_knob_value_int(test, "gap", FGJ_GAP_DEFAULT);
    d->gap = (int)v;
    if (d->gap < FGJ_GAP_MIN) d->gap = FGJ_GAP_MIN;
    if (d->gap > FGJ_GAP_MAX) d->gap = FGJ_GAP_MAX;

    v = get_testspecific_knob_value_int(test, "toggle", 0);
    d->toggle = (v != 0) ? 1 : 0;

    test->data = d;
    return EXIT_SUCCESS;
}

static int fmov_train_gdroop_ldp_stp_intr_cleanup(struct test *test)
{
    delete static_cast<GjData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int fmov_train_gdroop_ldp_stp_intr_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<GjData *>(test->data);
    const int stall = d->stall;
    const int burst = d->burst;
    const int chunk = d->chunk;
    const int gap = d->gap;
    const int toggle = d->toggle;

    /* per-thread buffers (parent geometry: 32KB each, L1D-domain);
     * 32B-aligned via aligned_alloc — the LDP/STP Q-pair access size
     * (std::vector only guarantees 16B, parent ldp_stp discipline) */
    auto *srcA = static_cast<uint64_t *>(aligned_alloc(32, FGJ_BYTES));
    auto *srcB = static_cast<uint64_t *>(aligned_alloc(32, FGJ_BYTES));
    auto *dstA = static_cast<uint64_t *>(aligned_alloc(32, FGJ_BYTES));
    auto *dstB = static_cast<uint64_t *>(aligned_alloc(32, FGJ_BYTES));
    auto *gold = static_cast<uint64_t *>(aligned_alloc(32, FGJ_BYTES));
    if (!srcA || !srcB || !dstA || !dstB || !gold) {
        free(srcA); free(srcB); free(dstA); free(dstB); free(gold);
        report_fail_msg("fmov_train_gdroop_ldp_stp_intr: buffer allocation failed");
        return EXIT_FAILURE;
    }

    memset_random(srcA, FGJ_BYTES);
    memset_random(srcB, FGJ_BYTES);
    memset(dstA, 0, FGJ_BYTES);
    memset(dstB, 0, FGJ_BYTES);

    uint32_t sink_acc = 0;
    uint32_t my_epoch = 0;

    uint64_t round = 0;
    do {
        const int slot = (int)(round % 3);
        const uint64_t rot = 0x9E3779B97F4A7C15ULL + round;

        /* round-start barrier: align all cores, then the quiet half */
        if (!fgj_barrier(test, d, &my_epoch))
            break;
        fgj_stall(stall);
        /* the step-edge barrier: every core leaves into the burst
         * together — the synchronized global current step */
        if (!fgj_barrier(test, d, &my_epoch))
            break;

        /* burst/victim chunks inside the sagged window; the chunk
         * loop keeps toggling the aggregate current (repeated edges) */
        for (int start = 0; start < FGJ_NUM_ELEMENTS; start += chunk) {
            const int m = (chunk < FGJ_NUM_ELEMENTS - start)
                        ? chunk : (FGJ_NUM_ELEMENTS - start);
            sink_acc ^= fgj_burst(burst, toggle);
            fgj_gap(gap);
            fgj_victim_chunk(srcA, srcB, dstA, dstB, rot, slot, start, m);
        }

        fgj_golden(srcA, srcB, gold, rot, slot);

        if (memcmp(dstA, gold, FGJ_BYTES) != 0 || memcmp(dstB, gold, FGJ_BYTES) != 0) {
            for (size_t i = 0; i < FGJ_NUM_ELEMENTS * 2; ++i) {
                if (dstA[i] != gold[i]) {
                    report_fail_msg("fmov_train_gdroop_ldp_stp_intr: dstA mismatch "
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
    fgj_sink = sink_acc;

    free(srcA); free(srcB); free(dstA); free(dstB); free(gold);
    return EXIT_SUCCESS;
}

DECLARE_TEST(fmov_train_gdroop_ldp_stp_intr, "All-core synchronized global-droop excitation (all-core undervolt lineage): sense-reversal barrier aligns every worker through stall (low current) -> synchronized FMA burst (global current step) -> fmov_train_ldp_stp recipe chunks inside the sagged window, byte-exact per-round scalar golden on every core; knobs stall/burst/chunk/gap/toggle (-O) sweep the global waveform (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fmov_train_gdroop_ldp_stp_intr_init,
    .test_run = fmov_train_gdroop_ldp_stp_intr_run,
    .test_cleanup = fmov_train_gdroop_ldp_stp_intr_cleanup,
    .fracture_loop_count = -1,   /* never fracture: barrier rides the round loop */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST

#else  /* !__aarch64__ */

static int fmov_train_gdroop_ldp_stp_intr_init(struct test *test)  { (void)test; return EXIT_SUCCESS; }
static int fmov_train_gdroop_ldp_stp_intr_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SUCCESS; }
static int fmov_train_gdroop_ldp_stp_intr_cleanup(struct test *test) { (void)test; return EXIT_SUCCESS; }

DECLARE_TEST(fmov_train_gdroop_ldp_stp_intr, "All-core synchronized global-droop excitation (ARM64-only)")
    .test_init    = fmov_train_gdroop_ldp_stp_intr_init,
    .test_run     = fmov_train_gdroop_ldp_stp_intr_run,
    .test_cleanup = fmov_train_gdroop_ldp_stp_intr_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif
