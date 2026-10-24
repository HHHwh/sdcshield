/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b libomp_ctx_arm
 * @parblock
 * libomp fork/join context emulation SDC stress — escalation T2 of the
 * cpu139 campaign (cn23154, HiSilicon 0xd22).
 *
 * Rationale: the XLSDFT fault history (docs/cases/, libomp metadata
 * corruption 5/17) implicates the OpenMP runtime's team/TLS metadata as
 * a corruption victim around parallel-region transitions. The
 * microarchitectural ingredients: bursty full-vector SVE FMA activity
 * alternating with collective idle windows, and rarely-touched metadata
 * (function-pointer tables, counters) read again after each window.
 *
 * HONEST BOUNDARY: this test emulates the ACCESS PATTERN of libomp team
 * metadata, NOT its layout — the real kmp TLS/metadata internals cannot
 * be replicated inside a test and no claim is made that they are. Each
 * round has three windows:
 *
 *  - ACTIVE: a 128-step serial svmla_f64_x dependency chain (the
 *    sve512_f64_chain_arm recipe) over shared framework-RNG operand
 *    streams, byte-compared against a scalar std::fma golden.
 *  - JOIN: a time-synchronized collective park — a shared deadline
 *    atomic arms a bounded idle window (SVE micro-op + sched_yield),
 *    and every parked thread releases at one absolute instant. No
 *    thread ever waits on another thread's PROGRESS, so the pattern
 *    cannot deadlock; the thread that arms a window (the "round
 *    master", mirroring libomp's master thread) skips the park and
 *    updates the shared team metadata instead.
 *  - CHECK: the per-thread and shared ("team") metadata structs are
 *    exercised in three layers — pointer table compared against the
 *    static golden, every entry CALLED through the live pointer
 *    (indirect branch after an idle window) with the return checked,
 *    and the whole struct memcmp'd byte-exact against a deterministically
 *    updated golden. Team counters are verified against per-thread
 *    shadows replayed under the team lock, so concurrent masters can
 *    never produce a false positive.
 *
 * All-VL 128..1024-bit via arm64_require_sve_vl (all-VL Phase 2 P3;
 * the pre-P3 512-bit single-value gate and its issue #182 caveat are
 * cleared by the four-tier dual-kernel evidence in gem5-fi
 * feat/sve512-dual); test_init gates via the framework layer and
 * test_run re-checks the VL, failing closed on drift. ARM64-native.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <cmath>
#include <memory>
#include <vector>
#include <atomic>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#include <sched.h>
#include <time.h>
#endif

// f64 FMLA chain steps per ACTIVE burst (shorter than f64_chain's 512:
// the rhythm, not the chain, is the subject here) — VL-independent (a
// per-lane dependency sequence; the lane count, not the length, scales
// with VL).
static constexpr size_t CHAIN_STEPS = 128;

// Collective park window length. Bounds the join phase; also bounds the
// worst-case round time, so the test can never hang on peer progress.
static constexpr uint64_t PARK_NS = 400000;   // 400 us

// Finite high-Hamming f64 patterns: |x| <= 2 keeps every 128-step
// product/sum finite, so lanes stay byte-exact comparable (f64_chain
// recipe).
static const uint64_t F64_FINITE[] = {
    0x3FF5555555555555ULL,   //  1.3333...
    0xBFFAAAAAAAAAAAAAULL,   // -1.6667...
    0x3FD5555555555555ULL,   //  0.3333...
    0xBFEAAAAAAAAAAAAAULL,   // -0.8333...
    0x0015555555555555ULL,   //  denormal +
    0x801AAAAAAAAAAAAAULL,   //  denormal -
    0x3FF0000000000000ULL,   //  1.0
    0xBFF0000000000000ULL,   // -1.0
    0x0005555555555555ULL,   //  denormal +
    0x800AAAAAAAAAAAAAULL,   //  denormal -
    0x3FF999999999999AULL,   //  1.6
    0xBFF6666666666667ULL,   // -1.4
    0x3FE5555555555555ULL,   //  0.6667...
    0xBFE6666666666667ULL,   // -0.7
    0x3FF3333333333333ULL,   //  1.2
    0xBFFCCCCCCCCCCCCDULL,   // -1.8
};
static constexpr size_t F64_FINITE_SIZE =
    sizeof(F64_FINITE) / sizeof(F64_FINITE[0]);

// Emulated metadata thunk table: what a runtime keeps as dispatch /
// hook pointers. Returns a base constant XOR the argument, so an
// indirect call through a (verified) pointer is checkable.
static int libomp_thunk_fn0(int x) { return 0x11110000 ^ x; }
static int libomp_thunk_fn1(int x) { return 0x22220000 ^ x; }
static int libomp_thunk_fn2(int x) { return 0x33330000 ^ x; }
static int libomp_thunk_fn3(int x) { return 0x44440000 ^ x; }

static constexpr int THUNK_BASE[4] = {
    0x11110000, 0x22220000, 0x33330000, 0x44440000
};

static int (*libomp_thunk(unsigned i))(int)
{
    switch (i) {
    case 0:  return libomp_thunk_fn0;
    case 1:  return libomp_thunk_fn1;
    case 2:  return libomp_thunk_fn2;
    default: return libomp_thunk_fn3;
    }
}

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
// Emulated "team" metadata: the shared structure a runtime mutates at
// region boundaries and every thread reads after the join.
struct TeamMeta {
    int (*thunk[4])(int);
    uint64_t refs[4];
    uint64_t generation;
    uint32_t flags;
    uint32_t pad_;
};

// Per-thread emulated metadata (the TLS-flavoured sibling).
struct ThreadMeta {
    int (*thunk[4])(int);
    uint64_t refs[4];
    uint64_t generation;
    uint32_t thread_id;
    uint32_t flags;
};

struct LibompCtx {
    // Time-synchronized window barrier (deadlock-free by construction:
    // nobody waits on peer progress, only on an absolute deadline).
    alignas(64) std::atomic<uint64_t> deadline_ns{0};
    alignas(64) std::atomic<uint32_t> round_seq{0};

    // Shared team metadata + spinlock (real runtimes lock team state too).
    alignas(64) std::atomic_flag team_lock;
    TeamMeta team;

    // Shared operand streams for the ACTIVE burst (framework RNG, H12').
    size_t vl_d;
    std::vector<uint64_t> seeds_f64;
    std::vector<uint64_t> m_f64;
    std::vector<uint64_t> a_f64;
};

static inline uint64_t monotonic_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

#ifdef __aarch64__

// ---------------------------------------------------------------------------
// ACTIVE window kernels (f64_chain recipe, shared streams).
// ---------------------------------------------------------------------------
static void fma_burst_golden(size_t lanes, const uint64_t *seeds,
                             const uint64_t *m, const uint64_t *a,
                             uint64_t *out)
{
    for (size_t lane = 0; lane < lanes; ++lane) {
        double acc;
        memcpy(&acc, seeds + lane, 8);
        for (size_t i = 0; i < CHAIN_STEPS; ++i) {
            double mm, aa;
            memcpy(&mm, m + i * lanes + lane, 8);
            memcpy(&aa, a + i * lanes + lane, 8);
            acc = std::fma(mm, aa, acc);
        }
        memcpy(out + lane, &acc, 8);
    }
}

static void fma_burst_hw(size_t lanes, const uint64_t *seeds,
                         const uint64_t *m, const uint64_t *a,
                         uint64_t *out)
{
    const svbool_t pg = svptrue_b64();
    svfloat64_t acc = svld1_f64(pg, reinterpret_cast<const double *>(seeds));
    for (size_t i = 0; i < CHAIN_STEPS; ++i) {
        svfloat64_t mv = svld1_f64(pg, reinterpret_cast<const double *>(m + i * lanes));
        svfloat64_t av = svld1_f64(pg, reinterpret_cast<const double *>(a + i * lanes));
        acc = svmla_f64_x(pg, acc, mv, av);
    }
    svst1_f64(pg, reinterpret_cast<double *>(out), acc);
}

// ---------------------------------------------------------------------------
// JOIN window: time-synchronized collective park.
//
// The window is an absolute deadline. A thread finding the deadline
// elapsed stores the next deadline and publishes the round by bumping
// round_seq; the CAS winner is the round's "master" (returns true) and
// proceeds without parking, mirroring the master thread's role at a
// region boundary. Everyone else parks until the shared instant with
// SVE micro-activity + sched_yield — the idle rhythm between regions.
// ---------------------------------------------------------------------------
static bool park_window(LibompCtx *ctx, uint32_t *armed_seq)
{
    const svbool_t pg = svptrue_b64();
    for (;;) {
        uint32_t seq = ctx->round_seq.load(std::memory_order_acquire);
        uint64_t dl = ctx->deadline_ns.load(std::memory_order_relaxed);
        uint64_t now = monotonic_ns();
        if (now >= dl) {
            // Arm the next window: deadline first, then the sequence
            // bump, so an acquire reader of the new seq sees the new
            // deadline (a stale pairing only ever parks one extra
            // window; it can never block).
            ctx->deadline_ns.store(now + PARK_NS, std::memory_order_relaxed);
            if (ctx->round_seq.compare_exchange_weak(
                    seq, seq + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                *armed_seq = seq + 1;
                return true;
            }
            continue;   // peer armed first: re-read the fresh window
        }
        // Idle-window micro-activity: short SVE batches whose result is
        // stored to a stack scratch (sizeless types cannot be inline-asm
        // operands, and the store keeps the batch from being optimized
        // away), then yield — the parked-thread rhythm between regions.
        // svst1 writes svcntd() doubles; the require gate caps VL at
        // 1024-bit = 16 f64 lanes — the pre-P3 micro[8] was exactly full
        // at 512-bit and overflowed only at 1024-bit (16 into 8); [16]
        // covers the whole gated range.
        double micro[16];
        while (now < dl) {
            svfloat64_t v = svdup_f64(1.0);
            for (int i = 0; i < 32; ++i)
                v = svadd_f64_x(pg, v, v);
            svst1_f64(pg, micro, v);
            sched_yield();
            now = monotonic_ns();
        }
        return false;
    }
}

// Master duty: mutate the shared team metadata for round @p S. The lock
// serializes masters against verifiers, and the update is a CANONICAL
// CATCH-UP: the team state is kept a pure function of the highest arm
// number it has seen (refs[s&3] counts s in 1..generation). A preempted
// winner resuming after a later winner can only extend the range — never
// skew a slot — which is exactly the false positive the first 608-thread
// smoke exposed when generation counted bare updates (refs[0]=10 vs
// golden=11, refs[1]=12 vs 11 skew, build11 all-core smoke).
static void team_master_update(LibompCtx *ctx, uint32_t S)
{
    while (ctx->team_lock.test_and_set(std::memory_order_acquire))
        ;
    while (ctx->team.generation < S) {
        ctx->team.generation += 1;
        ctx->team.refs[ctx->team.generation & 3] += 1;
    }
    ctx->team_lock.clear(std::memory_order_release);
}

// ---------------------------------------------------------------------------
// CHECK window: verify the shared team metadata. Every entry is first
// compared against the static golden, then CALLED through the snapshot
// pointer (indirect branch right after the idle window); counters are
// checked against a per-thread shadow replayed to the snapshot's
// generation. Only pointers that verified are called, so a corrupted
// entry is reported, never jumped through.
// ---------------------------------------------------------------------------
static void verify_team(LibompCtx *ctx, uint32_t *shadow_seq,
                        uint64_t shadow_refs[4], uint32_t round)
{
    TeamMeta snap;
    while (ctx->team_lock.test_and_set(std::memory_order_acquire))
        ;
    snap = ctx->team;
    ctx->team_lock.clear(std::memory_order_release);

    bool ok = true;
    for (unsigned i = 0; i < 4; ++i) {
        if (snap.thunk[i] != libomp_thunk(i)) {
            log_warning("libomp_ctx team: thunk[%u] golden=%p actual=%p",
                        i, (void *)(size_t)libomp_thunk(i),
                        (void *)(size_t)snap.thunk[i]);
            ok = false;
        }
    }
    const int arg = int(round & 0xFF);
    for (unsigned i = 0; i < 4; ++i) {
        if (snap.thunk[i] != libomp_thunk(i))
            continue;   // already reported; never call a bad pointer
        int r = snap.thunk[i](arg);
        if (r != (THUNK_BASE[i] ^ arg)) {
            log_warning("libomp_ctx team: thunk[%u](%d) ret=%08x expected=%08x",
                        i, arg, r, THUNK_BASE[i] ^ arg);
            ok = false;
        }
    }
    while (*shadow_seq < snap.generation) {
        ++*shadow_seq;
        shadow_refs[*shadow_seq & 3] += 1;
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (snap.refs[i] != shadow_refs[i]) {
            log_warning("libomp_ctx team: refs[%u] golden=%" PRIu64
                        " actual=%" PRIu64, i, shadow_refs[i], snap.refs[i]);
            ok = false;
        }
    }
    if (snap.flags != 0x5A5A0000u) {
        log_warning("libomp_ctx team: flags=%08x expected=5a5a0000", snap.flags);
        ok = false;
    }
    if (!ok)
        report_fail_msg("libomp_ctx_arm: shared team metadata corruption "
                        "detected (thunk table / counters / flags)");
}

// ---------------------------------------------------------------------------
// test_init
// ---------------------------------------------------------------------------
static int libomp_ctx_arm_init(struct test *test)
{
#ifndef __aarch64__
    (void)test;
    return EXIT_SUCCESS;
#else
    /* All-VL Phase 2 P3: capability + 128..1024-bit range check in one
       framework-layer call (the geometry has been svcnt-dynamic all
       along; only the old single-value gate pinned one tier). The
       historical VL=128 golden mismatch on a hosted CI runner
       (issue #182) is cleared by the four-tier dual-kernel evidence
       (gem5-fi feat/sve512-dual, 48 runs). arm64_require_sve_vl
       returns the VL in BYTES — f64 lanes = svl/8, range-checked
       below (release-effective, NDEBUG-proof). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    try {
        auto ctx = std::make_unique<LibompCtx>();
        ctx->vl_d = (size_t)svl / 8;   /* f64 lanes = SVL bytes / 8 */
        if (ctx->vl_d < 2 || ctx->vl_d > 16) {
            log_error("libomp_ctx_arm: f64 lanes %zu outside "
                      "[2,16] — unit drift (svl %ld bytes)",
                      ctx->vl_d, svl);
            return EXIT_FAILURE;
        }

        // randomization hardening H12' (f64_chain recipe): operands from
        // the framework RNG, every f64 finite with |x| <= 2, 25% from the
        // high-Hamming table.
        auto random_finite_f64 = []() -> uint64_t {
            if ((random32() & 3) == 0)
                return F64_FINITE[random32() % F64_FINITE_SIZE];
            uint64_t sign = (random64() & 1ULL) << 63;
            uint64_t mantissa = random64() & 0x000FFFFFFFFFFFFFULL;
            uint64_t exp;
            uint64_t raw = random64();
            switch ((raw >> 62) & 3ULL) {
            case 0:  exp = 0x3FEULL; break;   /* [0.5, 1) */
            case 1:  exp = 0x3FFULL; break;   /* [1, 2)   */
            default: exp = 0x3FEULL; break;   /* keep |x| < 2 */
            }
            return sign | (exp << 52) | mantissa;
        };

        ctx->seeds_f64.resize(ctx->vl_d);
        for (size_t lane = 0; lane < ctx->vl_d; ++lane)
            ctx->seeds_f64[lane] = 0x3FF0000000000000ULL |
                (random64() & 0x000FFFFFFFFFFFFFULL);

        const size_t n64 = CHAIN_STEPS * ctx->vl_d;
        ctx->m_f64.resize(n64);
        ctx->a_f64.resize(n64);
        for (size_t i = 0; i < n64; ++i) {
            ctx->m_f64[i] = random_finite_f64();
            ctx->a_f64[i] = random_finite_f64();
        }

        for (unsigned i = 0; i < 4; ++i)
            ctx->team.thunk[i] = libomp_thunk(i);
        ctx->team.flags = 0x5A5A0000u;

        test->data = ctx.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "libomp_ctx_arm init: %s", e.what());
        return EXIT_SKIP;
    }
#endif
}

// ---------------------------------------------------------------------------
// test_run: ACTIVE burst -> JOIN park -> CHECK metadata, every round.
// ---------------------------------------------------------------------------
static int libomp_ctx_arm_run(struct test *test, int cpu)
{
#ifndef __aarch64__
    (void)test;
    (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 SVE libomp context "
             "emulation required");
    return EXIT_SKIP;
#else
    auto *ctx = static_cast<LibompCtx *>(test->data);

    /* All-VL policy #3: fail closed on init/run VL drift. */
    if (svcntd() != ctx->vl_d) {
        log_error("libomp_ctx_arm: VL drifted between init (%zu "
                  "lanes) and run (%u lanes)", ctx->vl_d,
                  (unsigned)svcntd());
        report_fail_msg("libomp_ctx_arm: SVE VL drifted between "
                        "init and run");
    }

    const size_t lanes = ctx->vl_d;

    // Burst golden: streams are fixed for the whole run (drawn once in
    // init), so the scalar reference is computed once per thread.
    std::vector<uint64_t> golden64(lanes);
    std::vector<uint64_t> hw(lanes);
    fma_burst_golden(lanes, ctx->seeds_f64.data(), ctx->m_f64.data(),
                     ctx->a_f64.data(), golden64.data());

    // Per-thread metadata + golden mirror. Zero-initialized so padding
    // bytes are identical in both; fields written after init are only
    // ever updated accretively (+=), so a flipped bit accumulates
    // detectably instead of being overwritten away.
    ThreadMeta meta{};
    ThreadMeta meta_golden{};
    for (unsigned i = 0; i < 4; ++i) {
        meta.thunk[i] = libomp_thunk(i);
        meta_golden.thunk[i] = libomp_thunk(i);
    }
    meta.thread_id = meta_golden.thread_id = uint32_t(cpu);
    meta.flags = meta_golden.flags = 0xA5A50000u;

    // Team shadow: per-thread replay of the deterministic master updates.
    uint32_t shadow_seq = 0;
    uint64_t shadow_refs[4] = {0, 0, 0, 0};

    uint32_t rounds = 0;
    do {
        ++rounds;
        const int arg = int(rounds & 0xFF);

        // ---- ACTIVE window: full-vector FMLA chain, byte-exact check.
        fma_burst_hw(lanes, ctx->seeds_f64.data(), ctx->m_f64.data(),
                     ctx->a_f64.data(), hw.data());
        bool burst_ok = true;
        for (size_t lane = 0; lane < lanes; ++lane) {
            if (hw[lane] != golden64[lane]) {
                log_warning("libomp_ctx burst[cpu %d]: lane %zu "
                            "golden=0x%016" PRIx64 " actual=0x%016" PRIx64
                            " xor=0x%016" PRIx64,
                            cpu, lane, golden64[lane], hw[lane],
                            golden64[lane] ^ hw[lane]);
                burst_ok = false;
            }
        }
        if (!burst_ok) {
            report_fail_msg("libomp_ctx_arm: SVE f64 FMLA burst SDC "
                            "detected on cpu %d", cpu);
            return EXIT_FAILURE;
        }

        // ---- JOIN window: collective park (or master duty).
        uint32_t armed_seq = 0;
        if (park_window(ctx, &armed_seq))
            team_master_update(ctx, armed_seq);

        // ---- CHECK window, per-thread metadata: pointer layer, call
        // layer (only through verified pointers), counter layer.
        bool meta_ok = true;
        for (unsigned i = 0; i < 4; ++i) {
            if (meta.thunk[i] != meta_golden.thunk[i]) {
                log_warning("libomp_ctx meta[cpu %d]: thunk[%u] golden=%p "
                            "actual=%p", cpu, i,
                            (void *)(size_t)meta_golden.thunk[i],
                            (void *)(size_t)meta.thunk[i]);
                meta_ok = false;
            }
        }
        for (unsigned i = 0; i < 4; ++i) {
            if (meta.thunk[i] != meta_golden.thunk[i])
                continue;   // already reported; never call a bad pointer
            int r = meta.thunk[i](arg);
            if (r != (THUNK_BASE[i] ^ arg)) {
                log_warning("libomp_ctx meta[cpu %d]: thunk[%u](%d) "
                            "ret=%08x expected=%08x", cpu, i, arg, r,
                            THUNK_BASE[i] ^ arg);
                meta_ok = false;
            }
        }
        meta.refs[rounds & 3] += 1;
        meta.generation += 1;
        meta_golden.refs[rounds & 3] += 1;
        meta_golden.generation += 1;
        if (memcmp(&meta, &meta_golden, sizeof(meta)) != 0) {
            log_warning("libomp_ctx meta[cpu %d]: memcmp mismatch "
                        "(gen live=%" PRIu64 " golden=%" PRIu64
                        " tid live=%08x golden=%08x flags live=%08x "
                        "golden=%08x)", cpu, meta.generation,
                        meta_golden.generation, meta.thread_id,
                        meta_golden.thread_id, meta.flags, meta_golden.flags);
            for (unsigned i = 0; i < 4; ++i)
                log_warning("libomp_ctx meta[cpu %d]: refs[%u] live=%" PRIu64
                            " golden=%" PRIu64, cpu, i, meta.refs[i],
                            meta_golden.refs[i]);
            meta_ok = false;
        }
        if (!meta_ok) {
            report_fail_msg("libomp_ctx_arm: per-thread metadata corruption "
                            "detected on cpu %d (thunk table / counters / "
                            "thread id)", cpu);
            return EXIT_FAILURE;
        }

        // ---- CHECK window, shared team metadata.
        verify_team(ctx, &shadow_seq, shadow_refs, rounds);

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#endif
}

// ---------------------------------------------------------------------------
// test_cleanup
// ---------------------------------------------------------------------------
static int libomp_ctx_arm_cleanup(struct test *test)
{
#ifdef __aarch64__
    delete static_cast<LibompCtx *>(test->data);
#else
    (void)test;
#endif
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(libomp_ctx_arm,
             "libomp fork/join context emulation SDC stress (access "
             "pattern, not layout): SVE f64 FMLA bursts alternating with "
             "time-synchronized collective park windows, plus "
             "function-pointer-table and counter metadata canaries checked "
             "by indirect calls and byte-exact memcmp every round (all-VL "
             "128..1024-bit via arm64_require_sve_vl)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = libomp_ctx_arm_init,
    .test_run = libomp_ctx_arm_run,
    .test_cleanup = libomp_ctx_arm_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
