/*
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SANDSTONE_ARM64_SPEC_PREPARE_H
#define SANDSTONE_ARM64_SPEC_PREPARE_H

#include <sandstone.h>

#if defined(__aarch64__) && defined(__linux__)
#include <device/cpu/arm64/arm64_vl.h>

/*
 * Design §4.4 — run on THIS worker thread, immediately before test_run.
 *
 * Why here and not in test_init: arm64_vl.h documents the hard boundary —
 * ordinary tests run test_init on a throwaway aux thread, and a prctl there
 * never reaches the per-CPU workers (workers are created by the control
 * thread and inherit ITS vector length). The only thread whose VL matters is
 * the one that executes the test body.
 *
 * The framework pins every SVE/SVE2 test to the host's HIGHEST supported tier
 * so that "runs at the chip's maximum spec" is an explicit, verifiable fact
 * rather than an assumption about the kernel default (which
 * /proc/sys/abi/sve_default_vector_length can lower).
 *
 * Failure semantics (design §5):
 *   pin refused by the environment  -> SKIP (not our defect: seccomp, SE mode)
 *   pin succeeded but readback differs -> FAIL (invariant broken; never let
 *                                         fixed-width code run at the wrong VL)
 */

/*
 * Design §4.4 + the init/run invariant: test_init runs on an aux thread that
 * the CONTROL thread creates, and that aux thread freezes goldens and sizes
 * buffers from ITS current VL; the per-CPU workers are created later by the
 * same control thread and inherit its VL. Pinning only the workers is wrong on
 * a host whose kernel default VL is below the silicon maximum (the very case
 * §4.1 exists for): init would freeze the default tier, the workers would run
 * the maximum tier, and every init/run drift guard would fire. So the control
 * thread must reach the target tier BEFORE test_init is called.
 *
 * Deliberately silent and best-effort: the authoritative verdict stays with
 * arm64_spec_prepare_test() on each worker (refused pin -> SKIP, readback
 * mismatch -> FAIL). Here we only need the pin attempt on the thread whose
 * children inherit it.
 */
static inline void arm64_spec_pin_control_thread(const struct test *test)
{
    const struct arm64_spec *s = arm64_spec_get();
    const uint8_t need = test->arm64_requires;

    if ((need & (ARM64_REQUIRES_SVE | ARM64_REQUIRES_SVE2))
            && s->max_sve_vl_bits > 0)
        (void)arm64_sve_pin_vl(s->max_sve_vl_bits / 8);
    if ((need & (ARM64_REQUIRES_SME | ARM64_REQUIRES_SME2))
            && s->max_sme_svl_bits > 0)
        (void)arm64_sme_pin_vl(s->max_sme_svl_bits / 8);
}

static inline int arm64_spec_prepare_test(const struct test *test)
{
    const struct arm64_spec *s = arm64_spec_get();
    const uint8_t need = test->arm64_requires;

    if (need & (ARM64_REQUIRES_SVE | ARM64_REQUIRES_SVE2)) {
        const long want = s->max_sve_vl_bits;
        if (want > 0) {
            const long rc = arm64_sve_pin_vl(want / 8);
            if (rc < 0) {
                log_skip(CpuNotSupportedSkipCategory,
                         "%s: cannot pin this worker to the %ld-bit SVE tier "
                         "(prctl rc=%ld)", test->id, want, rc);
                return EXIT_SKIP;
            }
            const long got = arm64_sve_vl_bytes() * 8;
            if (got != want) {
                log_error("%s: pinned the %ld-bit SVE tier but read back %ld",
                          test->id, want, got);
                report_fail_msg("%s: SVE VL readback mismatch after pin",
                                test->id);
                return EXIT_FAILURE;   /* unreachable: report_fail_msg is noreturn */
            }
        }
    }
    if (need & (ARM64_REQUIRES_SME | ARM64_REQUIRES_SME2)) {
        const long want = s->max_sme_svl_bits;
        if (want > 0) {
            const long rc = arm64_sme_pin_vl(want / 8);
            if (rc < 0) {
                log_skip(CpuNotSupportedSkipCategory,
                         "%s: cannot pin this worker to the %ld-bit SME SVL "
                         "(prctl rc=%ld)", test->id, want, rc);
                return EXIT_SKIP;
            }
            const long got = arm64_sme_svl_bytes() * 8;
            if (got != want) {
                log_error("%s: pinned the %ld-bit SME SVL but read back %ld",
                          test->id, want, got);
                report_fail_msg("%s: SME SVL readback mismatch after pin",
                                test->id);
                return EXIT_FAILURE;
            }
        }
    }
    return EXIT_SUCCESS;
}
#else
/* x86-64 and every other architecture: no behaviour change (rule 5). */
static inline void arm64_spec_pin_control_thread(const struct test *test)
{
    (void)test;
}

static inline int arm64_spec_prepare_test(const struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}
#endif

#endif /* SANDSTONE_ARM64_SPEC_PREPARE_H */
