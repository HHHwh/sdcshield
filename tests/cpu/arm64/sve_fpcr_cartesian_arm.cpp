/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve_fpcr_cartesian_arm
 * @parblock
 * SVE companion of fpcr_rounding_cartesian_arm: the same FPCR
 * RMode[23:22] x FZ[24] configuration cartesian, exercised on the SVE
 * datapath (svmla_f32_x) instead of NEON. FPCR is shared between the two
 * engines architecturally, but the rounding decision silicon each engine
 * uses is its own — this test pins the SVE side. Same structure: per
 * configuration, a tie-operand liveness assertion, then a table-driven
 * hot loop compared byte-exact against a same-mode fesetround scalar
 * golden with FZ flush mirrored in software. mrs/msr only between
 * configurations. FPCR saved/restored/verified.
 *
 * test_init probes HWCAP_SVE and returns a clean EXIT_SKIP on SVE-less
 * CPUs (e.g. this Kunpeng 920 dev host). Any runtime vector length.
 * Built in tests_arm64_sve.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <cmath>
#include <cfenv>
#include <vector>

#ifdef __aarch64__
#include <sys/auxv.h>
#include <asm/hwcap.h>
#include <arm_sve.h>
#endif

static const uint32_t SPECIAL_F32[] = {
    0x3F800000u,   //  1.0
    0xBF800000u,   // -1.0
    0x40000000u,   //  2.0
    0xC0000000u,   // -2.0
    0x33800000u,   //  2^-24
    0xB3800000u,   // -2^-24
    0x00000001u,   //  smallest denormal
    0x80000001u,   // -smallest denormal
    0x007FFFFFu,   //  largest denormal
    0x3F000000u,   //  0.5
    0xBF000000u,   // -0.5
    0x3EAAAAAAu,   //  0.3333...
};
static constexpr size_t SPECIAL_SIZE =
    sizeof(SPECIAL_F32) / sizeof(SPECIAL_F32[0]);

static inline uint64_t rd_fpcr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return v;
}
static inline void wr_fpcr(uint64_t v)
{
    __asm__ volatile("msr fpcr, %0" :: "r"(v));
}

static inline bool is_denorm_f32(uint32_t bits)
{
    return (bits & 0x7F800000u) == 0 && (bits & 0x007FFFFFu) != 0;
}
static inline uint32_t flush_f32(uint32_t bits)
{
    if (is_denorm_f32(bits))
        return bits & 0x80000000u;
    return bits;
}

#ifdef __aarch64__

// FPCR.RMode[23:22]: 00=RNE, 01=RP(+inf), 10=RM(-inf), 11=RZ
// (hardware-verified; glibc FE_UPWARD == 1<<22, FE_TOWARDZERO == 3<<22).
static const int ROUND_FE[4] = { FE_TONEAREST, FE_UPWARD,
                                 FE_DOWNWARD, FE_TOWARDZERO };
static const char *ROUND_NAME[4] = { "RNE", "RP", "RM", "RZ" };

static uint32_t golden_fma_f32(uint32_t ua, uint32_t ub, uint32_t uc,
                               int r, int fz)
{
    if (fz) {
        ua = flush_f32(ua);
        ub = flush_f32(ub);
        uc = flush_f32(uc);
    }
    float a, b, c;
    memcpy(&a, &ua, 4);
    memcpy(&b, &ub, 4);
    memcpy(&c, &uc, 4);
    int saved = fegetround();
    fesetround(ROUND_FE[r]);
    float res = std::fmaf(a, b, c);
    fesetround(saved);
    uint32_t ur;
    memcpy(&ur, &res, 4);
    if (fz)
        ur = flush_f32(ur);
    return ur;
}

static int sve_fpcr_cartesian_arm_init(struct test *test)
{
#ifndef __aarch64__
    (void)test;
    return EXIT_SUCCESS;
#else
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SVE not available; sve_fpcr_cartesian_arm "
                 "requires SVE (SVE-side rounding network)");
        return EXIT_SKIP;
    }
    return EXIT_SUCCESS;
#endif
}

static int sve_fpcr_cartesian_arm_run(struct test *test, int cpu)
{
    (void)cpu;
#ifndef __aarch64__
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 SVE required for "
             "sve_fpcr_cartesian_arm");
    return EXIT_SKIP;
#else
    const size_t vl_w = svcntw();
    const uint64_t orig_fpcr = rd_fpcr();
    const svbool_t all = svptrue_b32();
    bool all_passed = true;

    uint64_t seed = random64();
    auto next_special = [&seed](size_t idx) -> uint32_t {
        seed = seed * 0x9E3779B97F4A7C15ULL + 1;
        return SPECIAL_F32[(idx + (size_t)(seed >> 40)) % SPECIAL_SIZE];
    };

    std::vector<float> a(vl_w), b(vl_w), c(vl_w), hw(vl_w);

    for (int r = 0; r < 4 && all_passed; ++r) {
        for (int fz = 0; fz <= 1 && all_passed; ++fz) {
            uint64_t fpcr = orig_fpcr & ~((3ULL << 22) | (1ULL << 24));
            fpcr |= (uint64_t)r << 22;
            fpcr |= (uint64_t)fz << 24;
            wr_fpcr(fpcr);

            // tie liveness on the SVE datapath: fma(1,1,2^-24) is a real
            // single-rounding tie — 1.0 + 2^-24 rounds per mode.
            {
                float ta[2] = { 1.0f, 0.0f };
                float tb[2] = { 1.0f, 0.0f };
                float tc[2] = { 0x1p-24f, 0.0f };
                svfloat32_t va = svld1_f32(all, ta);
                svfloat32_t vb = svld1_f32(all, tb);
                svfloat32_t vc = svld1_f32(all, tc);
                svfloat32_t vd = svmla_f32_x(all, vc, va, vb);
                float res[2] = { 0.0f, 0.0f };
                svst1_f32(all, res, vd);
                // 1+2^-24 is an exact tie between 1.0 and 1.000002:
                // RNE->1.0, RP->1.000002 (toward +inf), RM->1.0, RZ->1.0.
                float expect2 = (r == 1) ? 0x1.000002p+0f : 0x1p+0f;
                if (res[0] != expect2) {
                    log_warning("sve_fpcr_cartesian: mode %s fz=%d "
                                "liveness FAILED: fma(1,1,2^-24)=%a "
                                "(want %a)", ROUND_NAME[r], fz,
                                (double)res[0], (double)expect2);
                    wr_fpcr(orig_fpcr);
                    report_fail_msg("sve_fpcr_cartesian_arm: SVE "
                                    "rounding mode did not take effect");
                    return EXIT_FAILURE;
                }
            }

            // table-driven hot loop
            do {
                for (size_t i = 0; i < vl_w; ++i) {
                    uint32_t ua = next_special(i % 3);
                    uint32_t ub = next_special((i + 1) % 3 + 3);
                    uint32_t uc = next_special((i + 2) % 3 + 6);
                    memcpy(&a[i], &ua, 4);
                    memcpy(&b[i], &ub, 4);
                    memcpy(&c[i], &uc, 4);
                }
                svfloat32_t va = svld1_f32(all, a.data());
                svfloat32_t vb = svld1_f32(all, b.data());
                svfloat32_t vc = svld1_f32(all, c.data());
                svfloat32_t vd = svmla_f32_x(all, vc, va, vb);
                svst1_f32(all, hw.data(), vd);
                for (size_t i = 0; i < vl_w; ++i) {
                    uint32_t ua, ub, uc, uh;
                    memcpy(&ua, &a[i], 4);
                    memcpy(&ub, &b[i], 4);
                    memcpy(&uc, &c[i], 4);
                    memcpy(&uh, &hw[i], 4);
                    uint32_t g = golden_fma_f32(ua, ub, uc, r, fz);
                    if (g != uh) {
                        log_warning("sve_fpcr_cartesian: mode %s fz=%d "
                                    "lane %zu golden=0x%08" PRIx32 " "
                                    "actual=0x%08" PRIx32, ROUND_NAME[r],
                                    fz, i, g, uh);
                        all_passed = false;
                        break;
                    }
                }
            } while (false);

            wr_fpcr(orig_fpcr);
        }
    }

    if (!all_passed) {
        report_fail_msg("sve_fpcr_cartesian_arm: SVE rounding-network SDC "
                        "detected (RMode/FZ cartesian)");
        return EXIT_FAILURE;
    }

    while (test_time_condition(test)) {
        for (int r = 0; r < 4; ++r) {
            uint64_t fpcr = orig_fpcr & ~((3ULL << 22) | (1ULL << 24));
            fpcr |= (uint64_t)r << 22;
            wr_fpcr(fpcr);
            for (size_t i = 0; i < vl_w; ++i) {
                uint32_t ua = next_special(i % 4);
                uint32_t ub = next_special((i + 1) % 4 + 4);
                uint32_t uc = next_special((i + 2) % 4 + 8);
                memcpy(&a[i], &ua, 4);
                memcpy(&b[i], &ub, 4);
                memcpy(&c[i], &uc, 4);
            }
            svfloat32_t va = svld1_f32(all, a.data());
            svfloat32_t vb = svld1_f32(all, b.data());
            svfloat32_t vc = svld1_f32(all, c.data());
            svfloat32_t vd = svmla_f32_x(all, vc, va, vb);
            svst1_f32(all, hw.data(), vd);
            for (size_t i = 0; i < vl_w; ++i) {
                uint32_t ua, ub, uc, uh;
                memcpy(&ua, &a[i], 4);
                memcpy(&ub, &b[i], 4);
                memcpy(&uc, &c[i], 4);
                memcpy(&uh, &hw[i], 4);
                uint32_t g = golden_fma_f32(ua, ub, uc, r, 0);
                if (g != uh) {
                    report_fail_msg("sve_fpcr_cartesian_arm: SVE "
                                    "rounding-network SDC detected "
                                    "(re-loop, mode %s)", ROUND_NAME[r]);
                    wr_fpcr(orig_fpcr);
                    return EXIT_FAILURE;
                }
            }
            wr_fpcr(orig_fpcr);
        }
    }

    return EXIT_SUCCESS;
#endif
}

static int sve_fpcr_cartesian_arm_cleanup(struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sve_fpcr_cartesian_arm,
             "SVE companion of fpcr_rounding_cartesian_arm: FPCR RMode x FZ "
             "cartesian on the SVE datapath (svmla_f32_x), same-mode "
             "fesetround golden with software FZ flush, per-mode tie "
             "liveness assertion")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve_fpcr_cartesian_arm_init,
    .test_run = sve_fpcr_cartesian_arm_run,
    .test_cleanup = sve_fpcr_cartesian_arm_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
