/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sme_za_tileview_arm
 * @parblock
 * SME/ZA tile-view consistency SDC stress. The ZA addressing semantics
 * this test pins down were measured on cn23154 (HiSilicon 0xd22,
 * SME SVL = 512-bit = 8 f64 per slice, probe logs p4c-sme-map.out, run
 * identically green on cpu139 and cpu140):
 *
 *   - za0h.d[slice, 0] is a horizontal-slice round trip: rows loaded
 *     with ld1d {za0h.d[w12, 0]} come back byte-identical through
 *     st1d {za0h.d[w12, 0]}.
 *   - The vertical view transposes: after loading row-major a into
 *     za0h, st1d {za0v.d[w12, 0]} stores column w12 of the 8x8 tile -
 *     the store buffer is the transpose of a.
 *   - The ZAd vector-select immediate shifts the effective slice:
 *     reading za0h.d[r, 1] returns the row written via za0h.d[r+1 mod 8, 0].
 *   - Tiles are disjoint: writes to za1h.d never disturb the za0h
 *     contents (measured zero cross-tile visibility; here the stronger
 *     form - a's za0h image must survive the za1h traffic intact).
 *
 * Every CPU thread re-runs the kernel into its own stack buffers and
 * memcmps five outputs against scalar-computed goldens, so a ZA
 * read/write or tile-isolation fault on one core surfaces as a
 * single-core FAIL while the others stay green.
 *
 * The asm is a naked function: ZA slice index registers must be
 * w12-w15 and the operand forms (p0/z on ZA loads, plain predicate on
 * ZA stores, register-offset addressing) are spelled out so codegen
 * cannot drift from the measured recipe.
 *
 * Gates: HWCAP2 SME bit, then a runtime rdsvl check pinning the
 * characterized geometry (8 f64 per slice). Non-aarch64 hosts, non-
 * clang compilers, and uncharacterized SVLs skip with an honest reason.
 * @endparblock
 */

/* Clang defines __ARM_FEATURE_BTI for any armv8.5+/v9 -march even when no
 * BTI landing pad is emitted (this TU builds with branch protection
 * disabled; objdump of the object shows zero bti instructions). Sandstone's
 * generated feature header maps that macro into compiler_minimum_device,
 * which would gate this test on HWCAP2_BTI — a bit the SME-capable
 * HiSilicon 0xd22 does not expose, silently skipping the whole SME family
 * on exactly the hardware it exists to test. Undefine the false macro so
 * the only gates that remain are the honest runtime ones below (HWCAP2_SME
 * and the measured SVL geometry). */
#ifdef __ARM_FEATURE_BTI
#undef __ARM_FEATURE_BTI
#endif

#include <sandstone.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#ifdef __aarch64__
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif

/* Kernel headers of the SME era define this; older ones (5.10) do not. */
#ifndef HWCAP2_SME
#define HWCAP2_SME (1UL << 23)
#endif

/* Measured geometry on cn23154: SME SVL = 512-bit -> 8 f64 per ZA slice
 * and an 8x8 f64 tile per ZAd view. Other SVLs are skipped, not guessed. */
static constexpr long kTileN = 8;

struct SmeZaTileviewData {
    /* Shared read-only inputs and byte-exact goldens, each kTileN*kTileN. */
    std::vector<double> a, b;
    std::vector<double> golden_id_a, golden_tr_a, golden_rot_a;
    std::vector<double> golden_id_b;
};

#if defined(__aarch64__) && defined(__clang__)

struct SmeZaParams {
    const double *a;
    const double *b;
    double *id_a;   /* za0h round trip of a           */
    double *tr_a;   /* za0v vertical read = transpose */
    double *rot_a;  /* za0h.d[r, 1] read              */
    double *id_b;   /* za1h round trip of b           */
    double *id_a2;  /* za0h re-read after za1h writes */
};

/* One smstart/smstop window, five store phases, all against the
 * zero {za} baseline. Row offsets are row*8 elements, lsl #3 scaled -
 * the exact addressing shape the probes measured with. */
__attribute__((naked))
static void za_tileview_kernel(const SmeZaParams *p)
{
    asm volatile(
        "ldr x1, [x0, #0]\n\t"
        "ldr x2, [x0, #8]\n\t"
        "ldr x3, [x0, #16]\n\t"
        "ldr x4, [x0, #24]\n\t"
        "ldr x5, [x0, #32]\n\t"
        "ldr x6, [x0, #40]\n\t"
        "ldr x7, [x0, #48]\n\t"
        "smstart sm\n\t"
        "smstart za\n\t"
        "zero {za}\n\t"
        "ptrue p0.d\n\t"
        "mov x8, #8\n\t"
        "mov x9, #0\n"
        "1:\n\t"
        "cmp x9, #8\n\t"
        "b.ge 2f\n\t"
        "mov w12, w9\n\t"
        "mul x10, x9, x8\n\t"
        "ld1d {za0h.d[w12, 0]}, p0/z, [x1, x10, lsl #3]\n\t"
        "add x9, x9, #1\n\t"
        "b 1b\n"
        "2:\n\t"
        "mov x9, #0\n"
        "3:\n\t"
        "cmp x9, #8\n\t"
        "b.ge 4f\n\t"
        "mov w12, w9\n\t"
        "mul x10, x9, x8\n\t"
        "st1d {za0h.d[w12, 0]}, p0, [x3, x10, lsl #3]\n\t"
        "st1d {za0v.d[w12, 0]}, p0, [x4, x10, lsl #3]\n\t"
        "st1d {za0h.d[w12, 1]}, p0, [x5, x10, lsl #3]\n\t"
        "add x9, x9, #1\n\t"
        "b 3b\n"
        "4:\n\t"
        "mov x9, #0\n"
        "5:\n\t"
        "cmp x9, #8\n\t"
        "b.ge 6f\n\t"
        "mov w12, w9\n\t"
        "mul x10, x9, x8\n\t"
        "ld1d {za1h.d[w12, 0]}, p0/z, [x2, x10, lsl #3]\n\t"
        "add x9, x9, #1\n\t"
        "b 5b\n"
        "6:\n\t"
        "mov x9, #0\n"
        "7:\n\t"
        "cmp x9, #8\n\t"
        "b.ge 8f\n\t"
        "mov w12, w9\n\t"
        "mul x10, x9, x8\n\t"
        "st1d {za1h.d[w12, 0]}, p0, [x6, x10, lsl #3]\n\t"
        "st1d {za0h.d[w12, 0]}, p0, [x7, x10, lsl #3]\n\t"
        "add x9, x9, #1\n\t"
        "b 7b\n"
        "8:\n\t"
        "smstop za\n\t"
        "smstop sm\n\t"
        "ret\n"
    );
}

#endif // __aarch64__ && __clang__

static int sme_za_tileview_arm_init(struct test *test)
{
#if !defined(__aarch64__) || !defined(__clang__)
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): the SME asm kernel is only "
             "built with clang on aarch64 (armv9-a+sve+sme+sme-f64f64)");
    return EXIT_SKIP;
#else
    unsigned long hwcap2 = getauxval(AT_HWCAP2);
    if ((hwcap2 & HWCAP2_SME) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SME not available; sme_za_tileview_arm exercises "
                 "the ZA tile-view datapath");
        return EXIT_SKIP;
    }

    /* RDVL Xd, #imm = VL_bytes * imm, so #1 reads the streaming vector
     * length in bytes. SME implies SVE, so rdsvl cannot SIGILL here. */
    long svl_bytes;
    asm volatile("rdsvl %0, #1" : "=r"(svl_bytes));
    if (svl_bytes != kTileN * 8) {
        log_skip(CpuNotSupportedSkipCategory,
                 "sme_za_tileview_arm: SME streaming vector length %ld "
                 "bytes is not the characterized 64-byte (512-bit) "
                 "geometry; tile-view semantics at other SVLs are "
                 "unmeasured", svl_bytes);
        return EXIT_SKIP;
    }

    try {
        auto data = std::make_unique<SmeZaTileviewData>();
        constexpr long n = kTileN;
        constexpr size_t nn = static_cast<size_t>(n) * n;

        data->a.resize(nn);
        data->b.resize(nn);
        data->golden_id_a.resize(nn);
        data->golden_tr_a.resize(nn);
        data->golden_rot_a.resize(nn);
        data->golden_id_b.resize(nn);

        for (long i = 0; i < n; ++i)
            for (long j = 0; j < n; ++j) {
                const size_t ij = static_cast<size_t>(i) * n + j;
                const size_t ji = static_cast<size_t>(j) * n + i;
                const size_t rot = static_cast<size_t>((i + 1) % n) * n + j;
                /* Exact doubles: small integers plus halves/quarters,
                 * bit-dense and distinct per element so any flipped bit
                 * shows up in the memcmp. */
                data->a[ij] =
                    static_cast<double>((i * 131 + j) % 977) + 0.5 * (i + 1);
                data->b[ij] =
                    static_cast<double>((i * 197 + j) % 883) + 0.25 * (j + 1);
            }

        /* Goldens must be derived only after a/b are fully populated: the
         * transpose reads a[ji] and the slice-shift reads a[rot], both of
         * which point at later rows for i < n-1 — reading them inside the
         * fill loop above would pick up resize-zeroed elements and poison
         * every cross-row golden. */
        for (long i = 0; i < n; ++i)
            for (long j = 0; j < n; ++j) {
                const size_t ij = static_cast<size_t>(i) * n + j;
                const size_t ji = static_cast<size_t>(j) * n + i;
                const size_t rot = static_cast<size_t>((i + 1) % n) * n + j;
                data->golden_id_a[ij] = data->a[ij];
                /* za0v.d[c] stores column c: the transpose. */
                data->golden_tr_a[ij] = data->a[ji];
                /* za0h.d[r, 1] reads the row written via slice r+1 mod n. */
                data->golden_rot_a[ij] = data->a[rot];
                data->golden_id_b[ij] = data->b[ij];
            }

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sme_za_tileview_arm init: %s", e.what());
        return EXIT_SKIP;
    }
#endif
}

static int sme_za_tileview_arm_run(struct test *test, int cpu)
{
    (void)cpu;
#if !defined(__aarch64__) || !defined(__clang__)
    (void)test;
    return EXIT_SUCCESS;
#else
    auto *d = static_cast<SmeZaTileviewData *>(test->data);
    constexpr long n = kTileN;
    constexpr size_t count = static_cast<size_t>(n) * n;
    constexpr size_t bytes = count * sizeof(double);

    /* Per-thread output buffers: 608 CPUs run this concurrently and a
     * shared dst would be a cross-thread race. Every byte of each
     * buffer is rewritten by the kernel on every pass, so no memset. */
    double id_a[kTileN * kTileN];
    double tr_a[kTileN * kTileN];
    double rot_a[kTileN * kTileN];
    double id_b[kTileN * kTileN];
    double id_a2[kTileN * kTileN];

    SmeZaParams params = {
        d->a.data(), d->b.data(),
        id_a, tr_a, rot_a, id_b, id_a2,
    };

    do {
        za_tileview_kernel(&params);

        if (memcmp(id_a, d->golden_id_a.data(), bytes) != 0) {
            memcmp_or_fail(id_a, d->golden_id_a.data(), count,
                           "sme_za_tileview_arm: za0h.d[slice, 0] round-trip "
                           "mismatch");
        }
        if (memcmp(tr_a, d->golden_tr_a.data(), bytes) != 0) {
            memcmp_or_fail(tr_a, d->golden_tr_a.data(), count,
                           "sme_za_tileview_arm: za0v.d vertical read is not "
                           "the transpose of the za0h loads");
        }
        if (memcmp(rot_a, d->golden_rot_a.data(), bytes) != 0) {
            memcmp_or_fail(rot_a, d->golden_rot_a.data(), count,
                           "sme_za_tileview_arm: za0h.d[slice, 1] read does "
                           "not track the slice+1 vector select");
        }
        if (memcmp(id_b, d->golden_id_b.data(), bytes) != 0) {
            memcmp_or_fail(id_b, d->golden_id_b.data(), count,
                           "sme_za_tileview_arm: za1h.d[slice, 0] round-trip "
                           "mismatch");
        }
        if (memcmp(id_a2, d->golden_id_a.data(), bytes) != 0) {
            memcmp_or_fail(id_a2, d->golden_id_a.data(), count,
                           "sme_za_tileview_arm: za1h writes disturbed the "
                           "za0h tile (tile isolation violation)");
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#endif
}

static int sme_za_tileview_arm_cleanup(struct test *test)
{
    delete static_cast<SmeZaTileviewData *>(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(sme_za_tileview_arm,
             "SME/ZA tile-view SDC stress: za0h round trip, za0v transpose, "
             "za0h.d[slice, 1] vector-select shift, za1h round trip, and "
             "za0h integrity across za1h traffic, byte-exact against "
             "scalar goldens (cn23154 measured ZA semantics)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sme_za_tileview_arm_init,
    .test_run = sme_za_tileview_arm_run,
    .test_cleanup = sme_za_tileview_arm_cleanup,
    .fracture_loop_count = -1,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
