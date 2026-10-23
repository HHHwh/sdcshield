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
 * part of the process-wide device_features bitmask. Self-pinning is meant
 * for test_init, before worker threads are created (new threads inherit the
 * creator's VL).
 */

#include <stdbool.h>
#include <sandstone.h>

#if defined(__aarch64__)
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
 * register binding; the encoding's Rd field must stay 0. */
static inline long arm64_sve_vl_bytes(void)
{
    if (!arm64_cpu_has_sve())
        return 0;
    register long vl __asm__("x0");
    asm volatile(".inst 0x04bf5020" : "=r"(vl));
    return vl;
}

/* rdsvl x0, #1 == .inst 0x04bf5820 (derived via as -march=armv9-a+sve+sme). */
static inline long arm64_sme_svl_bytes(void)
{
    if (!arm64_cpu_has_sme())
        return 0;
    register long vl __asm__("x0");
    asm volatile(".inst 0x04bf5820" : "=r"(vl));
    return vl;
}

/* ---- self-pinning (size-specific TUs; call from test_init) ---- */
/*
 * Semantics: bytes is masked to the 16-bit VL length field before the prctl,
 * so out-of-range values are silently truncated and bytes == 0 asks the
 * kernel to restore the default VL (not an error). The kernel rounds the
 * request to the nearest VL it supports; the return value is the effective
 * VL in bytes (>= 0) or -errno — callers MUST check it and must not assume
 * the exact request took effect. New threads created after the call inherit
 * the pinned VL (kernel-documented per-thread semantics). Verification
 * boundary: the SET_VL effect and the clone inheritance are kernel-documented
 * but not yet empirically executed on any available host (this machine has
 * no SVE; gem5 SE maps prctl to ignoreFunc) — selftest_arm64_vl_require_sve_init
 * pins in init so the first SVE-host selftest run proves it automatically.
 */

static inline long arm64_sve_pin_vl(long bytes)
{
    if (!arm64_cpu_has_sve())
        return -EINVAL;
    int rc = prctl(PR_SVE_SET_VL, (unsigned long)(bytes & PR_SVE_VL_LEN_MASK));
    if (rc < 0)
        return -errno;
    return rc & PR_SVE_VL_LEN_MASK;
}

static inline long arm64_sme_pin_vl(long bytes)
{
    if (!arm64_cpu_has_sme())
        return -EINVAL;
    int rc = prctl(PR_SME_SET_VL, (unsigned long)(bytes & PR_SME_VL_LEN_MASK));
    if (rc < 0)
        return -errno;
    return rc & PR_SME_VL_LEN_MASK;
}

/* ---- test-flow helpers ---- */
/* Note: an empty range (min_bits > max_bits) skips unconditionally with the
 * range message — callers own their argument sanity. */

static inline long arm64_require_sve_vl(struct test *test, long min_bits, long max_bits)
{
    if (!arm64_cpu_has_sve()) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SVE not available; %s exercises the SVE datapath", test->id);
        return -1;
    }
    long bits = arm64_sve_vl_bytes() * 8;
    if (bits < min_bits || bits > max_bits) {
        log_skip(CpuNotSupportedSkipCategory,
                 "%s: SVE vector length %ld bits is outside the supported %ld..%ld-bit range",
                 test->id, bits, min_bits, max_bits);
        return -1;
    }
    return bits / 8;
}

static inline long arm64_require_sme_svl(struct test *test, long min_bits, long max_bits)
{
    if (!arm64_cpu_has_sme()) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SME not available; %s exercises the SME datapath", test->id);
        return -1;
    }
    long bits = arm64_sme_svl_bytes() * 8;
    if (bits < min_bits || bits > max_bits) {
        log_skip(CpuNotSupportedSkipCategory,
                 "%s: SME streaming vector length %ld bits is outside the supported %ld..%ld-bit range",
                 test->id, bits, min_bits, max_bits);
        return -1;
    }
    return bits / 8;
}

#else /* !__aarch64__: include-safety stubs; x86-64 untouched by design. */

static inline bool arm64_cpu_has_sve(void)   { return false; }
static inline bool arm64_cpu_has_sve2(void)  { return false; }
static inline bool arm64_cpu_has_sme(void)   { return false; }
static inline long arm64_sve_vl_bytes(void)  { return 0; }
static inline long arm64_sme_svl_bytes(void) { return 0; }
static inline long arm64_sve_pin_vl(long bytes)  { (void)bytes; return -1; }
static inline long arm64_sme_pin_vl(long bytes)  { (void)bytes; return -1; }
static inline long arm64_require_sve_vl(struct test *test, long min_bits, long max_bits)
{
    (void)test; (void)min_bits; (void)max_bits; return -1;
}
static inline long arm64_require_sme_svl(struct test *test, long min_bits, long max_bits)
{
    (void)test; (void)min_bits; (void)max_bits; return -1;
}

#endif /* __aarch64__ */
#endif /* SANDSTONE_ARM64_VL_H */
