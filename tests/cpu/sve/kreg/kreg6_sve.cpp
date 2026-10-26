#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <random>
#include <cstring>
#include <atomic>
#include <ctime>
#include <unistd.h>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

#define KREG6_ELEMENTS 16   /* 原负载: 16 位掩码 → 16 个 32 位展开 */

static int kreg6_sve_init(struct test *test) {
    (void)test;
#ifdef __aarch64__
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int kreg6_sve_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<uint16_t> mask16_dist(0, 0xFFFF);
    static std::atomic<uint64_t> iter{0};

    do {
        uint16_t mask_val = mask16_dist(rng);

        /* ---- SVE 硬件: 掩码展开用真谓词选择指令 svsel ----
         * 谓词 = (bit_i & mask) != 0 (svcmpne), 展开 = svsel(p, all1, all0)。
         * VL 无关: svcntw() 分批 + whilelt 尾部。 */
        uint32_t hw_vals[KREG6_ELEMENTS];
        {
            const int lanes = svcntw();
            uint32_t bits[16], ms[16], out[16];
            for (int base = 0; base < KREG6_ELEMENTS; base += lanes) {
                int n = (KREG6_ELEMENTS - base < lanes) ? (KREG6_ELEMENTS - base) : lanes;
                for (int i = 0; i < n; ++i) {
                    bits[i] = 1u << (base + i);
                    ms[i] = mask_val;
                }
                svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)n);
                svuint32_t vb = svld1_u32(pg, bits), vm = svld1_u32(pg, ms);
                svbool_t p = svcmpne_n_u32(pg, svand_u32_x(pg, vb, vm), 0);
                svst1_u32(pg, out, svsel_u32(p, svdup_u32(0xFFFFFFFFu), svdup_u32(0)));
                for (int i = 0; i < n; ++i) hw_vals[base + i] = out[i];
            }
        }

        /* 独立标量软件参考 (真 golden) */
        uint32_t sw_vals[KREG6_ELEMENTS];
        for (int i = 0; i < KREG6_ELEMENTS; ++i)
            sw_vals[i] = (mask_val & (1u << i)) ? 0xFFFFFFFFu : 0;

        /* ---- 存储一致性测试 ---- */
        uint32_t reload_vals[KREG6_ELEMENTS];
        memcpy(reload_vals, hw_vals, sizeof(hw_vals));
        bool consistent = (memcmp(reload_vals, hw_vals, sizeof(hw_vals)) == 0);

        /* ---- 逐元素比较 ---- */
        bool all_match = true;
        for (int i = 0; i < KREG6_ELEMENTS; ++i) {
            if (hw_vals[i] != sw_vals[i]) { all_match = false; break; }
        }
        bool passed = all_match && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        fprintf(stderr, "kreg6_sve: Iter %lu, mask=0x%04X\n", iteration, mask_val);
        fprintf(stderr, "  sw: ");
        for (int i = 0; i < KREG6_ELEMENTS; ++i) fprintf(stderr, "%08X ", sw_vals[i]);
        fprintf(stderr, "\n  hw: ");
        for (int i = 0; i < KREG6_ELEMENTS; ++i) fprintf(stderr, "%08X ", hw_vals[i]);
        fprintf(stderr, "\n  consistent=%d, result=%s%s\033[0m\n",
                consistent, color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("kreg6_sve: mismatch in SVE mask expansion or consistency");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int kreg6_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for kreg6_sve");
    return EXIT_SKIP;
}
#endif

static int kreg6_sve_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(kreg6_sve, "16-bit mask expansion to per-element all-ones/zeros on real SVE predicate select (svsel)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg6_sve_init,
    .test_run = kreg6_sve_run,
    .test_cleanup = kreg6_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
