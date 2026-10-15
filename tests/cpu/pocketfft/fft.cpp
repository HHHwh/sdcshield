/**
 * @file
 *
 * @copyright
 * Copyright 2026 ISCAS.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b pocketfft_fft
 * @parblock
 * FFT stress via pocketfft (BSD-3, the classic SciPy FFT engine, master
 * C edition): a complex forward cfft whose iterative multi-pass
 * factorisation (radix 4/2 for N=4096) walks twiddle-FMA butterfly
 * chains and bit-reversal-style decimation-in-frequency reordering —
 * store->load scatter pressure across the working set, the access
 * structure that discriminates the CORE179 defect. Each iteration
 * copies the input, runs the forward transform and byte-compares the
 * spectrum against a golden computed from freshly re-rolled operands
 * every kRerollEvery iterations (per-thread, framework RNG — B2-005;
 * fixed input + fixed binary => bit-identical spectrum, and the
 * framework RNG keeps -s replays reproducible); once per fracture
 * (round 3 of every 4) a frozen-replay round runs on init's shared
 * inputs/goldens, so the init-time golden is actually re-compared
 * instead of staying only the finiteness canary. The inverse cfft
 * (1/N scaling) is then
 * run as pure additional load: the forward->backward round trip is NOT
 * bit-exact under FFT rounding (measured max rel err ~3e-11 on this
 * very payload) and is therefore never compared — the SDC verdict
 * comes exclusively from the golden-spectrum memcmp.
 *
 * The transform length is runtime-configurable via the test knob
 * "-O pocketfft_fft.n=N" (test-id prefix required — a bare "n=N" is
 * silently ignored), selecting between three distinct algorithm
 * lineages inside pocketfft.c (make_cfft_plan, pocketfft.c:2073):
 * length<50 or largest_prime_factor(N)<=sqrt(N) picks the packed
 * multi-pass plan (cfftp), anything else picks Bluestein. The
 * whitelist therefore covers:
 * - powers of two (512..16384, default 4096): pure radix-4/radix-2
 *   multi-pass (pass4f/pass2f), the historical behavior;
 * - primes 4099/8191: largest_prime_factor(N)==N>sqrt(N) forces the
 *   Bluestein path — FFT re-expressed as a convolution, i.e. two
 *   extra complex FFTs of length good_size(2N-1) plus pointwise
 *   complex multiplies (fftblue_fft, pocketfft.c:1945): a completely
 *   different instruction sequence and memory pattern;
 * - mixed radix 6144 (3*2^11) and 10000 (2^4*5^4): packed plans whose
 *   factorisation mixes radf3/radf5 with radf2/radf4 passes in ONE
 *   transform, exercising the generic odd-radix butterflies.
 * Values outside the whitelist fail loudly (this is a lineage table,
 * not a validity check — pocketfft accepts any N).
 *
 * A second, independent segment exercises rfft_forward: real input,
 * halfcomplex-packed output in the same n doubles (a third transform
 * structure: the real-input packing halves the stored spectrum and
 * walks the radf* butterflies with different data layouts). Its
 * spectrum is likewise byte-compared against a per-thread golden
 * recomputed from re-rolled operands. The inverse transforms
 * (cfft_backward AND rfft_backward) are verified
 * with a tolerance round-trip check: neither round trip is bit-exact under
 * FFT rounding (measured max rel err ~3e-11), so their outputs are compared
 * against the original inputs at 1e-6 relative — catching gross inverse-path
 * corruption (dead butterfly, corrupted twiddle table) without false
 * positives on healthy silicon.
 * @endparblock
 */

#include <sandstone.h>

/* pocketfft.h has no extern "C" guard and pocketfft.c is compiled as C
 * into tests_base_a, so the declarations must be given C linkage here. */
extern "C" {
#include <pocketfft.h>
}

#include <string.h>
#include <stdlib.h>
#include <math.h>

namespace {
struct fft_test_data {
    int n;                /* transform length, from the -O pocketfft_fft.n=N knob */
    double *input;        /* interleaved re,im — length 2*n; init fill stays only as
                             the init-time finiteness canary between the run loop's
                             once-per-fracture frozen-replay rounds (B2-005: run
                             otherwise keeps per-thread re-rolled l_input) */
    double *golden_spec;  /* forward cfft spectrum of input, computed once in init;
                             same canary/replay role as input above */
    cfft_plan plan;       /* shared, see the threading note below */
    double *rinput;       /* real FFT input — length n; canary role as above */
    double *rgolden;      /* forward rfft spectrum of rinput (halfcomplex packed,
                             n doubles), computed once in init; canary role */
    rfft_plan rplan;      /* shared, same threading note as plan */
};
}

#define CAST(_x) static_cast<struct fft_test_data *>(_x)

/* The plans are created in init and shared by all worker threads: reading
 * pocketfft.c, a cfftp/rfftp plan holds only the factorisation and the
 * precomputed twiddle tables (plan->fct[].tw / tws point into plan->mem),
 * which are read-only after make_*_fftp_plan() returns, and a Bluestein
 * plan likewise holds only the read-only b_k / FFT(b_k) tables; every
 * per-call scratch buffer (pass_all()'s ch, rfftp_forward()'s ch,
 * fftblue_fft()'s akf, rfftblue_*()'s tmp) is malloc'd locally inside
 * the call and freed before returning, and the library has no mutable
 * static state. So concurrent cfft_forward()/rfft_forward() calls on
 * one plan are safe — unlike the OpenBLAS packing-buffer pool there is
 * no shared mutable metadata at all. Sharing the plans also keeps
 * make/destroy (a malloc + twiddle-table rebuild) out of the timed
 * loop, where it would be pure allocator noise.
 *
 * The work buffers ARE written every iteration, so they are per-thread
 * like the openblas_* scratch buffers: allocated lazily on first use and
 * intentionally not freed in cleanup — the worker threads are gone (or
 * never existed) when the main thread runs test_cleanup, and the forked
 * child exits right after, so the OS reclaims the storage. calloc zeroes
 * them once so a skipped/short first iteration can never compare garbage.
 *
 * B2-005 (R2b freeze blind spot): l_input/l_golden_spec (complex, 2*n
 * doubles) and l_rinput/l_rgolden (real, n doubles) are this thread's
 * re-rolled inputs and goldens — the shared d->input/golden_spec/
 * rinput/rgolden are otherwise only the init-time finiteness canaries,
 * so the run loop's once-per-fracture frozen-replay round copies the
 * frozen inputs into the per-thread work buffers to actually re-verify
 * the frozen goldens (the same-plan reroll alone cannot catch a
 * mercurial core defect). */
static thread_local double *work = nullptr;   /* complex, 2*n doubles */
static thread_local double *rwork = nullptr;  /* real, n doubles */
static thread_local double *l_input = nullptr;       /* complex, 2*n doubles */
static thread_local double *l_golden_spec = nullptr; /* complex, 2*n doubles */
static thread_local double *l_rinput = nullptr;      /* real, n doubles */
static thread_local double *l_rgolden = nullptr;     /* real, n doubles */

/* Map a random uint64 onto a double in [-1, 1) (top 53 bits as mantissa
 * fraction, folded around zero). FFT has no domain restriction on its
 * inputs, but the bounded magnitude keeps the complex spectrum bounded
 * by N*max|x| = N (measured max |X_k| ~ N/29 for this payload), so every
 * butterfly value stays finite and far from overflow — a NaN/Inf would
 * poison the byte-exact comparison (NaN != NaN). */
static double random_bounded(void)
{
    uint64_t r = random64();
    double frac = (double)(r >> 11) / 9007199254740992.0; /* [0, 1) */
    return frac * 2.0 - 1.0;
}

static int pocketfft_fft_init(struct test *test)
{
    auto d = new(fft_test_data);
    test->data = d;
    d->input = nullptr;
    d->golden_spec = nullptr;
    d->rinput = nullptr;
    d->rgolden = nullptr;
    d->plan = nullptr;
    d->rplan = nullptr;

    /* Lineage whitelist, not a validity check: pocketfft handles any N,
     * the table pins the three algorithm lineages of make_cfft_plan /
     * make_rfft_plan (pocketfft.c:2073/2133 — length<50 or
     * largest_prime_factor(N)<=sqrt(N) packs, else Bluestein). Powers of
     * two hit pure radix-4/2 multi-pass; primes 4099/8191 force the
     * Bluestein convolution (FFT-as-convolution: two extra complex FFTs
     * of length good_size(2N-1) + pointwise multiplies — a completely
     * different instruction sequence); 6144=3*2^11 and 10000=2^4*5^4
     * mix radf3/radf5 with radf2/radf4 passes in one factorisation.
     * 4097 is prime too but deliberately NOT whitelisted: the gate is
     * the lineage table, not a primality test. */
    static const int n_whitelist[] = {
        512, 1024, 2048, 4096, 8192, 16384,   /* pow2 */
        4099, 8191,                           /* prime -> Bluestein */
        6144, 10000,                          /* mixed radix (3/5) */
    };
    int64_t knob = get_testspecific_knob_value_int(test, "n", 4096);
    bool n_ok = false;
    for (size_t i = 0; i < sizeof(n_whitelist) / sizeof(n_whitelist[0]); ++i)
        if (knob == n_whitelist[i]) {
            n_ok = true;
            break;
        }
    if (!n_ok)
        report_fail_msg("n knob invalid: %ld (valid: 512..16384 pow2, "
                        "4099/8191 prime-Bluestein, 6144/10000 mixed)",
                        (long)knob);
    d->n = (int)knob;

    d->input       = (double *)malloc(2 * d->n * sizeof(double));
    d->golden_spec = (double *)malloc(2 * d->n * sizeof(double));
    if (!d->input || !d->golden_spec)
        report_fail_msg("OOM allocating %zu bytes", 2 * d->n * sizeof(double) * 2);
    /* high-entropy random input (framework RNG; libc rand is trapped) */
    for (int i = 0; i < 2 * d->n; ++i)
        d->input[i] = random_bounded();

    d->plan = make_cfft_plan(d->n);
    if (!d->plan)
        report_fail_msg("make_cfft_plan(%d) failed", d->n);

    /* golden spectrum, once: forward of the very input above with the very
     * plan the run loop will use (same binary + same input => bit-identical
     * spectrum; forward repeat determinism empirically verified: 100
     * repeat calls memcmp == 0) */
    memcpy(d->golden_spec, d->input, 2 * d->n * sizeof(double));
    if (cfft_forward(d->plan, d->golden_spec, 1.0) != 0)
        report_fail_msg("cfft_forward failed while computing the golden spectrum");

    /* finiteness tripwire: a non-finite golden value would make the
     * byte-exact comparison meaningless, so fail loudly at the source
     * (same tripwire as openblas_dgemm / sleef_neon). With |x| < 1 every
     * |X_k| of the complex transform is bounded by N*max|x| = N (measured
     * max ~ N/29), so the cutoff 4.0*N holds for every whitelisted N and
     * any hit here means the input mapping or the library broke. */
    for (int i = 0; i < 2 * d->n; ++i) {
        double g = d->golden_spec[i];
        if (g != g || g > 4.0 * (double)d->n || g < -4.0 * (double)d->n) {
            report_fail_msg("golden spectrum not finite/in-range at element %d "
                            "(input mapping needs tighter scaling)", i);
        }
    }

    /* real-FFT segment: independent random real input (filled AFTER the
     * complex input, so the complex segment's RNG consumption and thus
     * its default-n behavior stay byte-identical), same-length rfft plan.
     * rfft_forward works in-place on n doubles: real input in, the
     * halfcomplex-packed spectrum out, still n doubles. */
    d->rinput  = (double *)malloc(d->n * sizeof(double));
    d->rgolden = (double *)malloc(d->n * sizeof(double));
    if (!d->rinput || !d->rgolden)
        report_fail_msg("OOM allocating %zu bytes", d->n * sizeof(double) * 2);
    for (int i = 0; i < d->n; ++i)
        d->rinput[i] = random_bounded();

    d->rplan = make_rfft_plan(d->n);
    if (!d->rplan)
        report_fail_msg("make_rfft_plan(%d) failed", d->n);

    memcpy(d->rgolden, d->rinput, d->n * sizeof(double));
    if (rfft_forward(d->rplan, d->rgolden, 1.0) != 0)
        report_fail_msg("rfft_forward failed while computing the golden spectrum");

    /* same tripwire for the real transform: |X_k| <= 2*N*max|x| < 2N for
     * the halfcomplex packing (measured max well below that), cutoff
     * 8.0*N with the same 4x safety factor as the complex bound. */
    for (int i = 0; i < d->n; ++i) {
        double g = d->rgolden[i];
        if (g != g || g > 8.0 * (double)d->n || g < -8.0 * (double)d->n) {
            report_fail_msg("rfft golden spectrum not finite/in-range at element %d "
                            "(input mapping needs tighter scaling)", i);
        }
    }
    return EXIT_SUCCESS;
}

static int pocketfft_fft_run(struct test *test, int cpu)
{
    auto d = CAST(test->data);
    /* B2-005 (R2b freeze blind spot): this thread's inputs and goldens
     * are re-rolled from the framework RNG every kRerollEvery iterations
     * instead of replaying init's frozen bytes forever; once per
     * fracture (round 3 of every 4) a frozen-replay round instead runs
     * init's shared d->input/d->rinput through the same plans and
     * compares against d->golden_spec/d->rgolden, so the frozen goldens
     * are actually re-compared. The reroll goldens are recomputed with
     * the very same plans the verified calls below use, on the same
     * input distribution, so the finiteness bounds argued in init carry
     * over; -s replays stay reproducible since the framework RNG is
     * seeded per test. */
    constexpr int kRerollEvery = 16;
    int since_reroll = kRerollEvery; /* force the first-iteration fill */
    long iter = 0;
    TEST_LOOP(test, 1) {
        /* lazily allocate this thread's scratch buffers */
        if (__builtin_expect(!work || !rwork || !l_input || !l_golden_spec ||
                             !l_rinput || !l_rgolden, 0)) {
            if (!work) {
                work = (double *)calloc(2 * d->n, sizeof(double));
                if (!work)
                    report_fail_msg("OOM allocating thread scratch (%zu bytes)",
                                    2 * d->n * sizeof(double));
            }
            if (!rwork) {
                rwork = (double *)calloc(d->n, sizeof(double));
                if (!rwork)
                    report_fail_msg("OOM allocating rfft thread scratch (%zu bytes)",
                                    d->n * sizeof(double));
            }
            if (!l_input) {
                l_input = (double *)malloc(2 * d->n * sizeof(double));
                if (!l_input)
                    report_fail_msg("OOM allocating complex input (%zu bytes)",
                                    2 * d->n * sizeof(double));
            }
            if (!l_golden_spec) {
                l_golden_spec = (double *)malloc(2 * d->n * sizeof(double));
                if (!l_golden_spec)
                    report_fail_msg("OOM allocating complex golden (%zu bytes)",
                                    2 * d->n * sizeof(double));
            }
            if (!l_rinput) {
                l_rinput = (double *)malloc(d->n * sizeof(double));
                if (!l_rinput)
                    report_fail_msg("OOM allocating real input (%zu bytes)",
                                    d->n * sizeof(double));
            }
            if (!l_rgolden) {
                l_rgolden = (double *)malloc(d->n * sizeof(double));
                if (!l_rgolden)
                    report_fail_msg("OOM allocating real golden (%zu bytes)",
                                    d->n * sizeof(double));
            }
        }

        /* PR #244 review round 2 (greptile): `since_reroll` is a run()
         * local, but this test sets fracture_loop_count=4 — the framework
         * re-enters test_run in a FRESH child process every 4 TEST_LOOP
         * iterations, resetting all locals, so a "midpoint == 8" replay
         * could never fire. The frozen replay now triggers on an
         * in-fracture round position (round 3 of every 4) and reads the
         * frozen inputs straight into the copy-in work buffers —
         * l_input/l_rinput/l_golden_spec/l_rgolden are NOT touched, so
         * the reroll state survives intact across the replay round
         * (which also does not advance the reroll counter).
         * PR #244 review (coderabbit): the reroll's golden recompute
         * uses the very same plans, so a mercurial deterministic core
         * defect would reproduce identically on both sides and never
         * trip the compare — replaying init's frozen golden (computed
         * on a different core before the workers started) closes that
         * blind spot. */
        const bool frozen_replay = (iter & 3) == 2;
        if (!frozen_replay && ++since_reroll >= kRerollEvery) {
            since_reroll = 0;
            for (int i = 0; i < 2 * d->n; ++i)
                l_input[i] = random_bounded();
            memcpy(l_golden_spec, l_input, 2 * d->n * sizeof(double));
            if (cfft_forward(d->plan, l_golden_spec, 1.0) != 0)
                report_fail_msg("cfft_forward failed while re-rolling the golden spectrum");
            for (int i = 0; i < d->n; ++i)
                l_rinput[i] = random_bounded();
            memcpy(l_rgolden, l_rinput, d->n * sizeof(double));
            if (rfft_forward(d->rplan, l_rgolden, 1.0) != 0)
                report_fail_msg("rfft_forward failed while re-rolling the golden spectrum");
        }
        /* the DUT input/golden pair of this round: the frozen shared
         * pair on a replay round, this thread's re-rolled pair otherwise */
        const double *cur_input = frozen_replay ? d->input : l_input;
        const double *cur_golden_spec = frozen_replay ? d->golden_spec : l_golden_spec;
        const double *cur_rinput = frozen_replay ? d->rinput : l_rinput;
        const double *cur_rgolden = frozen_replay ? d->rgolden : l_rgolden;

        /* copy-in (dirties the cache lines, then the butterflies reload
         * them through the decimated access pattern) */
        memcpy(work, cur_input, 2 * d->n * sizeof(double));

        /* forward: the SDC verdict — spectrum must be byte-identical to
         * this round's golden spectrum */
        if (cfft_forward(d->plan, work, 1.0) != 0)
            report_fail_msg("cfft_forward failed on cpu %d", cpu);
        memcmp_or_fail(work, cur_golden_spec, 2 * d->n, "golden spectrum");

        /* inverse with 1/N scaling. The round trip is not bit-exact under
         * FFT rounding (measured max rel err ~3e-11 on this payload), so it
         * is checked against a TOLERANCE instead of byte-compared: the
         * backward input is the just-verified golden spectrum, so its output
         * must approximate the original input to within that rounding gap.
         * tol = 1e-6 * max(|x|, 1e-9): >4 orders of headroom over the healthy
         * ~3e-11 gap (no false positives on healthy silicon) while a real
         * inverse-path SDC — a dead butterfly stage, a corrupted twiddle
         * table — produces O(1)-magnitude deviations, orders above it. This
         * turns the inverse from dead load into a real verification. */
        if (cfft_backward(d->plan, work, 1.0 / d->n) != 0)
            report_fail_msg("cfft_backward failed on cpu %d", cpu);
        for (int i = 0; i < 2 * d->n; ++i) {
            double x = cur_input[i];
            double diff = fabs(work[i] - x);
            double tol = 1e-6 * fmax(fabs(x), 1e-9);
            if (diff > tol) {
                report_fail_msg("cfft round-trip mismatch at element %d: %g vs "
                                "input %g (diff %g, tol %g)", i, work[i], x,
                                diff, tol);
            }
        }

        /* real-FFT segment: same copy->forward->byte-compare structure
         * on the rfft plan (halfcomplex packed spectrum, n doubles). */
        memcpy(rwork, cur_rinput, d->n * sizeof(double));
        if (rfft_forward(d->rplan, rwork, 1.0) != 0)
            report_fail_msg("rfft_forward failed on cpu %d", cpu);
        memcmp_or_fail(rwork, cur_rgolden, d->n, "rfft golden spectrum");

        /* rfft_backward mirrors cfft_backward above: checked against a
         * tolerance (same calibration rationale) instead of being dead load. */
        if (rfft_backward(d->rplan, rwork, 1.0 / d->n) != 0)
            report_fail_msg("rfft_backward failed on cpu %d", cpu);
        for (int i = 0; i < d->n; ++i) {
            double x = cur_rinput[i];
            double diff = fabs(rwork[i] - x);
            double tol = 1e-6 * fmax(fabs(x), 1e-9);
            if (diff > tol) {
                report_fail_msg("rfft round-trip mismatch at element %d: %g vs "
                                "input %g (diff %g, tol %g)", i, rwork[i], x,
                                diff, tol);
            }
        }
        ++iter;
    }
    return EXIT_SUCCESS;
}

static int pocketfft_fft_cleanup(struct test *test)
{
    auto d = CAST(test->data);
    destroy_cfft_plan(d->plan);
    destroy_rfft_plan(d->rplan);
    free(d->input);
    free(d->golden_spec);
    free(d->rinput);
    free(d->rgolden);
    delete d;
    return EXIT_SUCCESS;
}

DECLARE_TEST(pocketfft_fft, "pocketfft complex+real FFT (bit-reversal decimation scatter + twiddle FMA butterflies)")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = pocketfft_fft_init,
  .test_run = pocketfft_fft_run,
  .test_cleanup = pocketfft_fft_cleanup,
  .fracture_loop_count = 4,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
