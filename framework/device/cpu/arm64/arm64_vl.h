/*
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SANDSTONE_ARM64_VL_H
#define SANDSTONE_ARM64_VL_H

/*
 * ARM64 SVE/SME vector-length (VL/SVL) layer — all-VL Phase 2 (P1).
 * Spec: docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md §5.
 *
 * Layer 1 (pure queries): usable from any aarch64 TU regardless of -march —
 *   rdvl/rdsvl are .inst-encoded (same compiler-independence trick as
 *   selftest.cpp's smstart probe); getauxval/prctl are syscalls.
 * Layer 2 (test-flow helpers): capability check + VL range check + log_skip
 *   in one call, replacing per-test getauxval boilerplate.
 *
 * Hard boundary: VL/SVL are per-thread runtime quantities, deliberately NOT
 * part of the process-wide device_features bitmask. Pinning only affects the
 * calling thread — it does NOT reach the framework's per-CPU workers when done
 * from test_init: ordinary tests run test_init on a throwaway aux thread
 * (sandstone_run.cpp run_one_test_inner(init_in_aux_thread=true); its pinning
 * doesn't affect the control thread, per the framework's own comment), while
 * workers are created by the control thread and inherit ITS (unpinned) VL.
 * Pin where the VL is needed: inside test_run on each worker thread. The
 * kernel's clone-inheritance rule (new threads inherit the creator's VL)
 * still holds — the framework's thread structure is simply not "test_init
 * creates the workers".
 *
 * UNIT CONVENTION (P6 audit, 2026-10-24 — 298 call sites counted: ~200
 * pure gates, 97 lane conversions, all pin sites in bytes): the require
 * helpers take the RANGE in BITS (matching how the spec and skip messages
 * speak: "128..1024-bit") but RETURN BYTES (what buffers and lanes need);
 * the pin helpers take and return BYTES. Lane conversions are therefore
 * divisions: f64 lanes = svl/8, f32 lanes = svl/4 (the P2 Critical-1
 * lesson — dividing bytes by the wrong power of two compiles silently;
 * every conversion site carries a unit comment plus a release-effective
 * range check that catches drift).
 */

#include <stdbool.h>
#include <sandstone.h>

#if defined(__aarch64__) && defined(__linux__)
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <asm/hwcap.h>

/* prctl numbers/masks, for build environments whose linux/prctl.h predates
 * kernel 5.19 (SME). Values per uapi linux/prctl.h. */
#ifndef PR_SVE_SET_VL
#define PR_SVE_SET_VL 50
#endif
#ifndef PR_SVE_GET_VL
#define PR_SVE_GET_VL 51
#endif
#ifndef PR_SVE_VL_LEN_MASK
#define PR_SVE_VL_LEN_MASK 0xffff
#endif
#ifndef PR_SME_SET_VL
#define PR_SME_SET_VL 63
#endif
#ifndef PR_SME_GET_VL
#define PR_SME_GET_VL 64
#endif
#ifndef PR_SME_VL_LEN_MASK
#define PR_SME_VL_LEN_MASK 0xffff
#endif

/* HWCAP2 bits, for build environments whose uapi asm/hwcap.h predates the
 * features (openEuler 20.03-LTS ships 4.19-era headers with no HWCAP2_*
 * definitions at all — SVE2 landed in uapi 5.10, SME in 5.15; the CI
 * Multi-version (20.03-LTS) source build and this repo's own oe2203
 * kernel-headers both lack HWCAP2_SME). Values per uapi asm/hwcap.h; same
 * fallback pattern as tests/cpu/arm64/sme_fmopa_stress_arm.cpp. Runtime is
 * unaffected: kernels that predate the uapi never set these auxv bits, so
 * the probes still return false. */
#ifndef HWCAP2_SVE2
#define HWCAP2_SVE2 (1UL << 1)
#endif
#ifndef HWCAP2_SME
#define HWCAP2_SME (1UL << 23)
#endif
#ifndef HWCAP2_SME2
#define HWCAP2_SME2 (1UL << 37)   /* uapi 6.0 */
#endif

/* ---- capability probes: honest runtime, immune to the compile-time
 * short-circuit of device_has_feature() in +sve/+sme-compiled TUs ---- */

static inline bool arm64_cpu_has_sve(void)
{
    return (getauxval(AT_HWCAP) & HWCAP_SVE) != 0;
}

static inline bool arm64_cpu_has_sve2(void)
{
    return (getauxval(AT_HWCAP2) & HWCAP2_SVE2) != 0;
}

static inline bool arm64_cpu_has_sme(void)
{
    return (getauxval(AT_HWCAP2) & HWCAP2_SME) != 0;
}

/* ---- current-thread vector lengths (bytes; 0 without the feature) ---- */

/* rdvl x0, #1 == .inst 0x04bf5020 (derived via as -march=armv8.2-a+sve +
 * objdump, 2026-10-23). The destination is pinned to x0 by the local
 * register binding; the encoding's Rd field must stay 0. The operand is
 * read-write ("+r" on an initialized variable): input-side local register
 * bindings are a long-documented GCC guarantee, whereas bare output
 * ("=r" on an uninitialized one) was only documented from GCC 12 — on the
 * repo's GCC-10 baseline it merely happens to work. */
static inline long arm64_sve_vl_bytes(void)
{
    if (!arm64_cpu_has_sve())
        return 0;
    register long vl __asm__("x0") = 0;
    asm volatile(".inst 0x04bf5020" : "+r"(vl));
    return vl;
}

/* rdsvl x0, #1 == .inst 0x04bf5820. Encoding-verification boundary, stated
 * honestly: unlike the rdvl encoding above (independently reproduced with
 * this repo's `as`, binutils 2.37), rdsvl needs binutils 2.38+ (SME) which
 * this machine lacks, so this encoding is taken from the Arm ARM encoding
 * tables (same 0x04bf5xxx RDVL/RDSVL space, bit 11 set). First executed
 * paths on an SME host: --dump-cpu-info (cpu_device.cpp, the usual
 * first command on a new host — a wrong encoding would SIGILL there) and
 * selftest_arm64_vl_api's SVL range check (fails loudly rather than
 * silently mis-reporting). Same "+r" input-binding hardening as
 * arm64_sve_vl_bytes(). */
static inline long arm64_sme_svl_bytes(void)
{
    if (!arm64_cpu_has_sme())
        return 0;
    register long vl __asm__("x0") = 0;
    asm volatile(".inst 0x04bf5820" : "+r"(vl));
    return vl;
}

/* ---- self-pinning (size-specific TUs; pin on the thread that needs the VL,
 * i.e. inside test_run workers — see the header comment on why test_init
 * pinning does not propagate to framework workers) ---- */
/*
 * Semantics: bytes <= 0 or > PR_xx_VL_LEN_MASK is rejected with -EINVAL up
 * front (no silent truncation); the kernel then selects the largest
 * supported VL not exceeding the request (round-down, per
 * vec_set_vector_length/find_supported_vector_length in
 * arch/arm64/kernel/fpsimd.c) — a zero length would fail the kernel's
 * sve_vl_valid() check with -EINVAL, it does NOT restore any default. The
 * return value is the effective VL in bytes (>= 16, since SVE_VL_MIN is
 * always supported and a successful PR_SVE_SET_VL never returns less) or
 * -errno; a prctl that returns 0 is an execution layer silently ignoring
 * the call (e.g. gem5 SE maps prctl to ignoreFunc) and is surfaced as
 * -ENOSYS so callers doing `if (pin(vl) < 0) skip;` never mistake
 * "ignored" for "applied". Callers MUST check the return value and must
 * not assume the exact request took effect. The call only
 * affects the calling thread; threads created by THIS thread afterwards
 * inherit the pinned VL (kernel-documented per-thread semantics). Verification
 * boundary: the SET_VL effect and rdvl read-back are kernel-documented but
 * not yet empirically executed on any available host (this machine has
 * no SVE; gem5 SE maps prctl to ignoreFunc) — selftest_arm64_vl_require_sve
 * pins an alternate VL on the worker thread and reads it back, so the first
 * SVE-host selftest run *attempts* the round-trip proof; on hosts where
 * prctl is blocked (-EPERM, e.g. seccomp) the test degrades to a logged
 * warning rather than claiming the proof.
 */

static inline long arm64_sve_pin_vl(long bytes)
{
    if (!arm64_cpu_has_sve())
        return -EINVAL;
    if (bytes <= 0 || bytes > PR_SVE_VL_LEN_MASK)
        return -EINVAL; /* reject instead of silently truncating the 16-bit field */
    int rc = prctl(PR_SVE_SET_VL, (unsigned long)(bytes & PR_SVE_VL_LEN_MASK));
    if (rc < 0)
        return -errno;
    if (rc == 0)
        return -ENOSYS; /* a real kernel returns the new VL (>= 16); 0 = ignored */
    return rc & PR_SVE_VL_LEN_MASK;
}

static inline long arm64_sme_pin_vl(long bytes)
{
    if (!arm64_cpu_has_sme())
        return -EINVAL;
    if (bytes <= 0 || bytes > PR_SME_VL_LEN_MASK)
        return -EINVAL; /* reject instead of silently truncating the 16-bit field */
    int rc = prctl(PR_SME_SET_VL, (unsigned long)(bytes & PR_SME_VL_LEN_MASK));
    if (rc < 0)
        return -errno;
    if (rc == 0)
        return -ENOSYS; /* a real kernel returns the new VL (>= 16); 0 = ignored */
    return rc & PR_SME_VL_LEN_MASK;
}

/* ---- chip spec: one-shot probe, cached (design §4.1) ----
 *
 * Why a separate probe from the queries above: "current VL" is not "maximum
 * supported VL". The kernel default (and any per-process lowering via
 * /proc/sys/abi/sve_default_vector_length) can be below what the silicon
 * supports, so a policy that says "run at the highest tier this chip has"
 * must ask the kernel for the ceiling and read back what it granted.
 *
 * PR_SVE_SET_VL is per-thread, so the probe runs on a throwaway thread
 * (design §4.1) and the calling thread's VL, PR_SVE_VL_INHERIT flag and any
 * PR_SVE_SET_VL_ONEXEC setting are left untouched. The throwaway thread
 * requests 2048-bit; the kernel rounds down to the largest supported tier
 * and returns it (verified natively 2026-10-27 on an SVE host: requests
 * 512/1024/2048/4096 on a 256-bit chip all return 256). Only the low
 * PR_*_VL_LEN_MASK bits of that return are used: a caller that had
 * PR_SVE_VL_INHERIT set gets the flag ORed into PR_SVE_GET_VL results, and
 * treating it as part of the length would fabricate a ~1M-bit maximum.
 *
 * If prctl is refused entirely (seccomp sandbox, emulator) the granted value
 * is 0: fall back to the current thread's VL and mark max_vl_known=false —
 * never claim a maximum we could not measure.
 *
 * ARM64_SPEC_MAX_TIER_BITS = 2048 is the architectural probing ceiling and
 * the reserved 2048-bit tier; this project implements 128/256/512/1024 only
 * (spec §6), so nothing downstream branches on 2048.
 */
#define ARM64_SPEC_MAX_TIER_BITS 2048
/* ARM64_REQUIRES_{SVE,SVE2,SME,SME2} live in sandstone.h, next to the
 * struct test field that carries them, so test TUs can declare the
 * requirement without including this header. */

struct arm64_spec {
    bool has_sve, has_sve2, has_sme, has_sme2;
    long max_sve_vl_bits;    /* 0 when SVE is absent */
    long max_sme_svl_bits;   /* 0 when SME is absent */
    bool max_vl_known;       /* false = prctl refused; max_* is the fallback */
};

/* Throwaway-thread probe body: request `want_bytes`, keep the granted length.
 * Runs on a thread the caller just created, so nothing here can disturb the
 * caller's per-thread VL state. */
struct arm64_spec_probe_arg {
    int set_vl_cmd;
    long want_bytes;
    long granted_bytes;
};

static inline void *arm64_spec_probe_thread(void *p)
{
    struct arm64_spec_probe_arg *arg = (struct arm64_spec_probe_arg *)p;
    int rc = prctl(arg->set_vl_cmd,
                   (unsigned long)(arg->want_bytes & PR_SVE_VL_LEN_MASK));
    /* A successful PR_SVE_SET_VL returns the effective VL, possibly ORed with
       the INHERIT flag; only the length field is meaningful here. */
    arg->granted_bytes = (rc > 0) ? (rc & PR_SVE_VL_LEN_MASK) : 0;
    return NULL;
}

/* Request `want_bytes` on a scratch thread; returns the granted bytes
 * (0 = the kernel/execution layer refused or the thread could not start). */
static inline long arm64_vl_probe_max(int set_vl_cmd, long want_bytes)
{
    struct arm64_spec_probe_arg arg = { set_vl_cmd, want_bytes, 0 };
    pthread_t thread;

    if (pthread_create(&thread, NULL, arm64_spec_probe_thread, &arg) != 0)
        return 0;
    pthread_join(thread, NULL);
    return arg.granted_bytes;
}

/* One-shot, once-per-process probe. Publication is an atomic state machine so
 * concurrent first callers cannot observe a half-written `spec`: the winner
 * fills it and releases; losers wait for the ready state. (The first call
 * normally happens during test-set construction, before any worker exists.) */
static inline const struct arm64_spec *arm64_spec_get(void)
{
    static struct arm64_spec spec;
    static int state;      /* 0 = cold, 1 = probing, 2 = ready */

    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) == 2)
        return &spec;

    int expected = 0;
    if (!__atomic_compare_exchange_n(&state, &expected, 1, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&state, __ATOMIC_ACQUIRE) != 2)
            ;   /* bounded: the probe is a handful of prctl/getauxv calls */
        return &spec;
    }

    spec.has_sve  = arm64_cpu_has_sve();
    spec.has_sve2 = arm64_cpu_has_sve2();
    spec.has_sme  = arm64_cpu_has_sme();
    spec.has_sme2 = (getauxval(AT_HWCAP2) & HWCAP2_SME2) != 0;
    spec.max_sve_vl_bits = 0;
    spec.max_sme_svl_bits = 0;
    spec.max_vl_known = false;

    if (spec.has_sve) {
        long granted = arm64_vl_probe_max(PR_SVE_SET_VL,
                                          ARM64_SPEC_MAX_TIER_BITS / 8);
        long bytes = granted > 0 ? granted : arm64_sve_vl_bytes();
        spec.max_sve_vl_bits = bytes > 0 ? bytes * 8 : 0;
        spec.max_vl_known = granted > 0;

        /* Explicit override, honoured everywhere the spec is consumed
           (selection, worker pin, per-tier dispatch) so all three stay
           consistent. Purpose: exercise a lower tier on a host whose
           maximum is higher (otherwise the highest-tier default makes
           those code paths untestable), and let an operator pin a tier for
           a targeted campaign. Only lowering is allowed, and the request is
           re-probed: a value the silicon does not implement (e.g. 384-bit
           where only 128/256/512 exist) is accepted at the tier the kernel
           actually grants, never at a tier the worker pin would then fail
           to reproduce. */
        const char *want = getenv("SDCSHIELD_ARM64_SPEC_MAX_VL");
        if (want && *want) {
            char *end = NULL;
            long bits = strtol(want, &end, 10);
            if (end != want && bits >= 128 && bits <= ARM64_SPEC_MAX_TIER_BITS
                    && (bits % 128) == 0
                    && (bits < spec.max_sve_vl_bits || !spec.max_vl_known)) {
                long granted = arm64_vl_probe_max(PR_SVE_SET_VL, bits / 8);
                if (granted > 0)
                    spec.max_sve_vl_bits = granted * 8;
                else if (!spec.max_vl_known)
                    spec.max_sve_vl_bits = bits;   /* cannot measure: keep request */
            }
        }
    }
    if (spec.has_sme) {
        long granted = arm64_vl_probe_max(PR_SME_SET_VL,
                                          ARM64_SPEC_MAX_TIER_BITS / 8);
        long bytes = granted > 0 ? granted : arm64_sme_svl_bytes();
        spec.max_sme_svl_bits = bytes > 0 ? bytes * 8 : 0;
    }
    __atomic_store_n(&state, 2, __ATOMIC_RELEASE);
    return &spec;
}

/* Human-readable one-liner for --dump-cpu-info and run logs. Caller owns the
 * buffer; 128 bytes is enough for every path below. */
static inline const char *arm64_spec_describe(char *buf, size_t len)
{
    const struct arm64_spec *s = arm64_spec_get();
    size_t used;

    if (!s->has_sve)
        used = (size_t)snprintf(buf, len, "SVE no");
    else if (s->max_vl_known)
        used = (size_t)snprintf(buf, len, "SVE yes (max VL %ld bits)",
                                s->max_sve_vl_bits);
    else
        used = (size_t)snprintf(buf, len,
                                "SVE yes (max VL unknown, fell back to %ld bits)",
                                s->max_sve_vl_bits);

    if (used < len)
        snprintf(buf + used, len - used, " | SVE2 %s | SME %s | SME2 %s",
                 s->has_sve2 ? "yes" : "no",
                 s->has_sme ? "yes" : "no",
                 s->has_sme2 ? "yes" : "no");
    return buf;
}

/* ---- test-flow helpers ---- */
/* Note: an empty range (min_bits > max_bits) skips unconditionally with the
 * range message — callers own their argument sanity. Units: min_bits/max_bits
 * are bits as requested by tests (e.g. 128/256/512); the return value is
 * bytes, ready to feed arm64_sve_pin_vl(bytes) — do NOT multiply by 8 again
 * (unit mix-up deferred to P6 per the PR description). On skip the helpers
 * return EXIT_SKIP (not a bare -1): the framework's init-result handling
 * (sandstone_run.cpp) logs "Unexpected OS error: %s" with strerror(-ret) for
 * any negative value other than EXIT_SKIP, so a bare -1 would emit a
 * misleading "Operation not permitted" on top of the real log_skip reason. */

static inline long arm64_require_sve_vl(struct test *test, long min_bits, long max_bits)
{
    if (!arm64_cpu_has_sve()) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SVE not available; %s exercises the SVE datapath", test->id);
        return EXIT_SKIP;
    }
    /* Align with the tier the worker will run at: test_init executes on a
     * throwaway aux thread that still carries the PROCESS default VL, while
     * arm64_spec_prepare_test() pins the worker to the spec maximum. When the
     * spec is lower than the process default (possible only through the
     * SDCSHIELD_ARM64_SPEC_MAX_VL override — the kernel default otherwise
     * equals the maximum) the two would disagree and the run-side drift guard
     * would fire for an artificial reason. Clamp to the spec so init sizes its
     * geometry for the tier that will actually execute. */
    long bits = arm64_sve_vl_bytes() * 8;
    {
        const struct arm64_spec *spec = arm64_spec_get();
        if (spec->max_sve_vl_bits > 0 && bits > spec->max_sve_vl_bits)
            bits = spec->max_sve_vl_bits;
    }
    if (bits < min_bits || bits > max_bits) {
        log_skip(CpuNotSupportedSkipCategory,
                 "%s: SVE vector length %ld bits is outside the supported %ld..%ld-bit range",
                 test->id, bits, min_bits, max_bits);
        return EXIT_SKIP;
    }
    return bits / 8;
}

static inline long arm64_require_sme_svl(struct test *test, long min_bits, long max_bits)
{
    if (!arm64_cpu_has_sme()) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SME not available; %s exercises the SME datapath", test->id);
        return EXIT_SKIP;
    }
    long bits = arm64_sme_svl_bytes() * 8;
    if (bits < min_bits || bits > max_bits) {
        log_skip(CpuNotSupportedSkipCategory,
                 "%s: SME streaming vector length %ld bits is outside the supported %ld..%ld-bit range",
                 test->id, bits, min_bits, max_bits);
        return EXIT_SKIP;
    }
    return bits / 8;
}

/* ---- all-VL policy #3: single-source VL guards (PR #283 review) ----
 *
 * The sve512 family (10 files) repeats the same two guards per file,
 * hand-copied with a f64/f32 triple asymmetry — svcntd/vl_d/[2,16] with
 * divisor 8 vs svcntw/vl_w/[4,32] with divisor 4. All 10 copies were
 * verified correct, but every future all-VL migration (P4+) re-faces
 * the same copy-paste trap; both guards now live here, with the width
 * carried in the name so the counter, the divisor and the range can
 * never be mis-paired again.
 *
 * arm64_sve_f64_lanes_checked / arm64_sve_f32_lanes_checked: init-side
 * conversion of an arm64_require_sve_vl result (bytes) into the lane
 * count, range-checked for the 128..1024-bit all-VL span
 * (release-effective, NDEBUG-proof); returns 0 and logs the reason when
 * out of range — callers fail closed with EXIT_FAILURE. */
static inline size_t arm64_sve_f64_lanes_checked(struct test *test, long svl)
{
    size_t lanes = (size_t)svl / 8;   /* f64 lanes = SVL bytes / 8 */
    if (lanes < 2 || lanes > 16) {
        log_error("%s: f64 lanes %zu outside [2,16] — unit drift "
                  "(svl %ld bytes)", test->id, lanes, svl);
        return 0;
    }
    return lanes;
}

static inline size_t arm64_sve_f32_lanes_checked(struct test *test, long svl)
{
    size_t lanes = (size_t)svl / 4;   /* f32 lanes = SVL bytes / 4 */
    if (lanes < 4 || lanes > 32) {
        log_error("%s: f32 lanes %zu outside [4,32] — unit drift "
                  "(svl %ld bytes)", test->id, lanes, svl);
        return 0;
    }
    return lanes;
}

#else /* !(__aarch64__ && __linux__): include-safety stubs. Covers x86-64
       * (untouched by design) and Windows ARM64, whose toolchains have no
       * <sys/auxv.h>/<asm/hwcap.h>/prctl — coding_style_guide requires
       * cross-platform (Linux + Windows) source compatibility. Stubs mirror
       * the aarch64 no-capability paths: pin returns -EINVAL and the require
       * helpers log_skip with the same category, so a shared TU that reaches
       * them skips for a stated reason instead of an unexplained -1. */

#include <errno.h>

static inline bool arm64_cpu_has_sve(void)   { return false; }
static inline bool arm64_cpu_has_sve2(void)  { return false; }
static inline bool arm64_cpu_has_sme(void)   { return false; }
static inline long arm64_sve_vl_bytes(void)  { return 0; }
static inline long arm64_sme_svl_bytes(void) { return 0; }
static inline long arm64_sve_pin_vl(long bytes)  { (void)bytes; return -EINVAL; }
static inline long arm64_sme_pin_vl(long bytes)  { (void)bytes; return -EINVAL; }
/* No SVE on these platforms -> no valid lane count (mirrors the
 * capability stubs above; callers fail closed). */
static inline size_t arm64_sve_f64_lanes_checked(struct test *test, long svl)
{ (void)test; (void)svl; return 0; }
static inline size_t arm64_sve_f32_lanes_checked(struct test *test, long svl)
{ (void)test; (void)svl; return 0; }
static inline long arm64_require_sve_vl(struct test *test, long min_bits, long max_bits)
{
    (void)min_bits; (void)max_bits;
    log_skip(CpuNotSupportedSkipCategory,
             "ARM64 SVE not available; %s exercises the SVE datapath", test->id);
    return EXIT_SKIP;
}
static inline long arm64_require_sme_svl(struct test *test, long min_bits, long max_bits)
{
    (void)min_bits; (void)max_bits;
    log_skip(CpuNotSupportedSkipCategory,
             "ARM64 SME not available; %s exercises the SME datapath", test->id);
    return EXIT_SKIP;
}

#endif /* __aarch64__ && __linux__ */

/* ---- all-VL policy #3, run side: init/run VL-drift fail-closed check ----
 *
 * ARM64_SVE_VL_DRIFT_CHECK_F64 / _F32: verifies inside test_run that the
 * calling worker thread's VL still yields the lane count recorded at
 * init, and fails closed otherwise. Macros rather than functions on
 * purpose: svcntd()/svcntw() come from <arm_sve.h>, which this header
 * deliberately does not include (it must stay usable from any aarch64
 * TU regardless of -march) — the intrinsics resolve at the expansion
 * site, i.e. only inside SVE-compiled TUs; expanding them anywhere else
 * is a compile error, which is the intended fail-loud behavior.
 * report_fail_msg() is noreturn (kills the calling worker thread), so
 * the drift path never falls through into vector code. init_lanes is
 * evaluated exactly once; pass a plain member read. */
#define ARM64_SVE_VL_DRIFT_CHECK_F64(test, init_lanes)                        \
    do {                                                                      \
        const size_t arm64_vl_init_lanes_ = (init_lanes);                     \
        if (svcntd() != arm64_vl_init_lanes_) {                               \
            log_error("%s: VL drifted between init (%zu lanes) and run "      \
                      "(%u lanes)", (test)->id, arm64_vl_init_lanes_,         \
                      (unsigned)svcntd());                                    \
            report_fail_msg("%s: SVE VL drifted between init and run",        \
                            (test)->id);                                      \
        }                                                                     \
    } while (0)

#define ARM64_SVE_VL_DRIFT_CHECK_F32(test, init_lanes)                        \
    do {                                                                      \
        const size_t arm64_vl_init_lanes_ = (init_lanes);                     \
        if (svcntw() != arm64_vl_init_lanes_) {                               \
            log_error("%s: VL drifted between init (%zu lanes) and run "      \
                      "(%u lanes)", (test)->id, arm64_vl_init_lanes_,         \
                      (unsigned)svcntw());                                    \
            report_fail_msg("%s: SVE VL drifted between init and run",        \
                            (test)->id);                                      \
        }                                                                     \
    } while (0)

#endif /* SANDSTONE_ARM64_VL_H */
