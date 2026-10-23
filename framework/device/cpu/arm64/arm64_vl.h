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
 */

#include <stdbool.h>
#include <sandstone.h>

#if defined(__aarch64__) && defined(__linux__)
#include <errno.h>
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
    long bits = arm64_sve_vl_bytes() * 8;
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
#endif /* SANDSTONE_ARM64_VL_H */
