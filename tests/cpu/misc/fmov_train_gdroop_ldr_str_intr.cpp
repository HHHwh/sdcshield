/**
 * @file
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test fmov_train_gdroop_ldr_str_intr
 * @parblock
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
 *   - fgi_load: ONE pinned ldr q (prevents reorder/merge/DCE — the
 *     pattern of the most effective test on this host,
 *     neon_rot_ldr_at_top_rowmajor_fp64_dump)
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
 *              recipe) } until the buffer pass
 *              completes — the chunk loop keeps toggling current
 *              (repeated di/dt edges) while the victim's back-to-back
 *              str;str captures run inside the sagged window
 *   golden recompute (scalar, parent semantics: slot/rot rotate per
 *   round) + memcmp dstA/dstB — EVERY core's result is checked every
 *   round: full coverage, no fault-core knowledge required.
 *
 * Knobs (read at init, -O fmov_train_gdroop_ldr_str_intr.<k>=<v>):
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
 * Provenance: victim = intrinsics-built cross-RF chain (semantics of
 * the fmov_train_ldr_str recipe, g = b <op> (a + rot) per u64 lane);
 * aggressor =
 * power_virus_dit burst lineage (8-chain form from fmov_train_droop);
 * barrier = mesh_upi_sve sense-reversal (B1-001).  The LDP/STP pair
 * twin of THIS test lives in fmov_train_gdroop_ldp_stp.cpp.
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

#define FGI_NUM_ELEMENTS 2048            /* 16B elements -> 32KB/buffer */
#define FGI_BYTES (FGI_NUM_ELEMENTS * 16)

#define FGI_STALL_DEFAULT 16384
#define FGI_STALL_MIN     0
#define FGI_STALL_MAX     1048576
#define FGI_BURST_DEFAULT 4096
#define FGI_BURST_MIN     16
#define FGI_BURST_MAX     65536
#define FGI_CHUNK_DEFAULT 64
#define FGI_CHUNK_MIN     16
#define FGI_CHUNK_MAX     2048
#define FGI_GAP_DEFAULT   0
#define FGI_GAP_MIN       0
#define FGI_GAP_MAX       255

/* racy-by-intent sink: keeps the aggressor chain live; never read */
static volatile uint32_t fgi_sink;

namespace { /* ODR: per-TU GiData */
struct GiData {
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
static bool fgi_barrier(struct test *test, struct GiData *d,
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
static inline void fgi_stall(int n)
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
static inline uint32_t fgi_burst(int n, int toggle)
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
static inline void fgi_gap(int g)
{
    for (int i = 0; i < g; ++i)
        __asm__ volatile("nop");
}

/*
 * VICTIM chunk, INTRINSICS-BUILT (fp64_dump minimal-asm pattern).  The
 * cross-register-file consumer chain is expressed in NEON intrinsics
 * and the ENCODING IS GCC's CHOICE: vgetq_lane_u64 -> umov,
 * vcreate_u64 -> fmov d,x, vcombine_u64 -> ins (actual codegen pinned
 * by the objdump gate in the commit).  This twin deliberately lets
 * the compiler build the chain — it is the DISCRIMINATOR against the
 * asm-built fmov_train_gdroop_ldr_str: does the excitation need the
 * exact hand-pinned fmov encodings, or does any SIMD->GPR->SIMD
 * round-trip do?  The ONE asm below (fgi_load) is the fp64_dump
 * pattern: pins the ldr q load so the compiler cannot reorder, merge
 * or (for a dead result) eliminate it.
 */
static inline uint64x2_t fgi_load(const void *addr)
{
    uint64x2_t res;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

/* cross-RF consumer chain, intrinsics-built: lane extract (GPR) ->
 * scalar add (GPR ALU) -> recombine (SIMD).
 *
 * The register-class pins below emit ZERO instructions — they only
 * force the extracted lanes into the GENERAL register file.  Without
 * them GCC lowers the whole round-trip onto d-registers (observed in
 * the first build: "mov d6, v0.d[1]; add d6, d6, d8; mov v0.d[1],
 * v6.d[0]" — lane shuffles INSIDE the vector file, zero GPR
 * crossing, umov count 0) and the cross-RF excitation target ceases
 * to exist.  This is the fp64_dump discipline applied to a compiler
 * DECISION rather than an instruction: asm pins what the compiler
 * cannot be trusted to keep. */
static inline uint64x2_t fgi_cross_rf(uint64x2_t a, uint64_t rot)
{
    uint64_t lo = vgetq_lane_u64(a, 0);
    uint64_t hi = vgetq_lane_u64(a, 1);
    asm("" : "+r"(lo));
    asm("" : "+r"(hi));
    uint64_t rlo = lo + rot;
    uint64_t rhi = hi + rot;
    /* second pin pair: forces the ADD itself into the GPR ALU.  With
     * only the extract pins GCC satisfies them with a pointless
     * d->x->d bounce and still computes the adds on the vector side
     * (observed: "fmov x0,d0; fmov d0,x0; add d0,d8,d0").  Pinning the
     * add RESULTS to x-class makes the GPR add the cheapest shape. */
    asm("" : "+r"(rlo));
    asm("" : "+r"(rhi));
    return vcombine_u64(vcreate_u64(rlo), vcreate_u64(rhi));
}

/* slot op: 0=add 1=eor 2=sub, per u64 lane (bit-identical to the
 * asm parent's add v.2d / eor v.16b / sub v.2d). */
static inline uint64x2_t fgi_op(int slot, uint64x2_t b, uint64x2_t v4)
{
    switch (slot) {
        case 0:  return vaddq_u64(b, v4);
        case 1:  return veorq_u64(b, v4);
        default: return vsubq_u64(b, v4);
    }
}

static void fgi_victim_chunk(const uint64_t *a, const uint64_t *b,
                             uint64_t *da, uint64_t *db, uint64_t rot,
                             int slot, int start_elem, int count)
{
    for (int i = 0; i < count; ++i) {
        const size_t o = (size_t)(start_elem + i) * 2;
        const uint64x2_t va = fgi_load(a + o);
        const uint64x2_t vb = fgi_load(b + o);
        const uint64x2_t v4 = fgi_cross_rf(va, rot);
        const uint64x2_t res = fgi_op(slot, vb, v4);
        vst1q_u64(da + o, res);
        vst1q_u64(db + o, res);
    }
}

/* scalar bit-exact golden: g = b <op> (a + rot) — parent formula verbatim */
static void fgi_golden(const uint64_t *a, const uint64_t *b,
                       uint64_t *g, uint64_t rot, int slot)
{
    size_t n = FGI_NUM_ELEMENTS * 2;
    for (size_t i = 0; i < n; ++i) {
        uint64_t x = a[i] + rot;
        g[i] = (slot == 0) ? (b[i] + x)
             : (slot == 1) ? (b[i] ^ x)
             :               (b[i] - x);
    }
}

static int fmov_train_gdroop_ldr_str_intr_init(struct test *test)
{
    auto *d = new GiData;
    if (!d)
        return EXIT_FAILURE;
    d->arrived.store(0, std::memory_order_relaxed);
    d->epoch.store(0, std::memory_order_relaxed);

    int64_t v;
    v = get_testspecific_knob_value_int(test, "stall", FGI_STALL_DEFAULT);
    d->stall = (int)v;
    if (d->stall < FGI_STALL_MIN) d->stall = FGI_STALL_MIN;
    if (d->stall > FGI_STALL_MAX) d->stall = FGI_STALL_MAX;

    v = get_testspecific_knob_value_int(test, "burst", FGI_BURST_DEFAULT);
    d->burst = (int)v;
    if (d->burst < FGI_BURST_MIN) d->burst = FGI_BURST_MIN;
    if (d->burst > FGI_BURST_MAX) d->burst = FGI_BURST_MAX;

    v = get_testspecific_knob_value_int(test, "chunk", FGI_CHUNK_DEFAULT);
    d->chunk = (int)v;
    if (d->chunk < FGI_CHUNK_MIN) d->chunk = FGI_CHUNK_MIN;
    if (d->chunk > FGI_CHUNK_MAX) d->chunk = FGI_CHUNK_MAX;

    v = get_testspecific_knob_value_int(test, "gap", FGI_GAP_DEFAULT);
    d->gap = (int)v;
    if (d->gap < FGI_GAP_MIN) d->gap = FGI_GAP_MIN;
    if (d->gap > FGI_GAP_MAX) d->gap = FGI_GAP_MAX;

    v = get_testspecific_knob_value_int(test, "toggle", 0);
    d->toggle = (v != 0) ? 1 : 0;

    test->data = d;
    return EXIT_SUCCESS;
}

static int fmov_train_gdroop_ldr_str_intr_cleanup(struct test *test)
{
    delete static_cast<GiData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int fmov_train_gdroop_ldr_str_intr_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<GiData *>(test->data);
    const int stall = d->stall;
    const int burst = d->burst;
    const int chunk = d->chunk;
    const int gap = d->gap;
    const int toggle = d->toggle;

    /* per-thread buffers (parent geometry: 32KB each, L1D-domain) */
    std::vector<uint8_t> srcAv(FGI_BYTES), srcBv(FGI_BYTES);
    std::vector<uint8_t> dstAv(FGI_BYTES), dstBv(FGI_BYTES), goldv(FGI_BYTES);
    auto *srcA = reinterpret_cast<uint64_t *>(srcAv.data());
    auto *srcB = reinterpret_cast<uint64_t *>(srcBv.data());
    auto *dstA = reinterpret_cast<uint64_t *>(dstAv.data());
    auto *dstB = reinterpret_cast<uint64_t *>(dstBv.data());
    auto *gold = reinterpret_cast<uint64_t *>(goldv.data());

    memset_random(srcAv.data(), FGI_BYTES);
    memset_random(srcBv.data(), FGI_BYTES);
    memset(dstAv.data(), 0, FGI_BYTES);
    memset(dstBv.data(), 0, FGI_BYTES);

    uint32_t sink_acc = 0;
    uint32_t my_epoch = 0;

    uint64_t round = 0;
    do {
        const int slot = (int)(round % 3);
        const uint64_t rot = 0x9E3779B97F4A7C15ULL + round;

        /* round-start barrier: align all cores, then the quiet half */
        if (!fgi_barrier(test, d, &my_epoch))
            break;
        fgi_stall(stall);
        /* the step-edge barrier: every core leaves into the burst
         * together — the synchronized global current step */
        if (!fgi_barrier(test, d, &my_epoch))
            break;

        /* burst/victim chunks inside the sagged window; the chunk
         * loop keeps toggling the aggregate current (repeated edges) */
        for (int start = 0; start < FGI_NUM_ELEMENTS; start += chunk) {
            const int m = (chunk < FGI_NUM_ELEMENTS - start)
                        ? chunk : (FGI_NUM_ELEMENTS - start);
            sink_acc ^= fgi_burst(burst, toggle);
            fgi_gap(gap);
            fgi_victim_chunk(srcA, srcB, dstA, dstB, rot, slot, start, m);
        }

        fgi_golden(srcA, srcB, gold, rot, slot);

        if (memcmp(dstA, gold, FGI_BYTES) != 0 || memcmp(dstB, gold, FGI_BYTES) != 0) {
            for (size_t i = 0; i < FGI_NUM_ELEMENTS * 2; ++i) {
                if (dstA[i] != gold[i]) {
                    report_fail_msg("fmov_train_gdroop_ldr_str_intr: dstA mismatch "
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
    fgi_sink = sink_acc;

    return EXIT_SUCCESS;
}

DECLARE_TEST(fmov_train_gdroop_ldr_str_intr, "All-core synchronized global-droop excitation (all-core undervolt lineage): sense-reversal barrier aligns every worker through stall (low current) -> synchronized FMA burst (global current step) -> fmov_train_ldr_str recipe chunks inside the sagged window, byte-exact per-round scalar golden on every core; knobs stall/burst/chunk/gap/toggle (-O) sweep the global waveform (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fmov_train_gdroop_ldr_str_intr_init,
    .test_run = fmov_train_gdroop_ldr_str_intr_run,
    .test_cleanup = fmov_train_gdroop_ldr_str_intr_cleanup,
    .fracture_loop_count = -1,   /* never fracture: barrier rides the round loop */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST

#else  /* !__aarch64__ */

static int fmov_train_gdroop_ldr_str_intr_init(struct test *test)  { (void)test; return EXIT_SUCCESS; }
static int fmov_train_gdroop_ldr_str_intr_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SUCCESS; }
static int fmov_train_gdroop_ldr_str_intr_cleanup(struct test *test) { (void)test; return EXIT_SUCCESS; }

DECLARE_TEST(fmov_train_gdroop_ldr_str_intr, "All-core synchronized global-droop excitation (ARM64-only)")
    .test_init    = fmov_train_gdroop_ldr_str_intr_init,
    .test_run     = fmov_train_gdroop_ldr_str_intr_run,
    .test_cleanup = fmov_train_gdroop_ldr_str_intr_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif
