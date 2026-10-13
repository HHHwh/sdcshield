#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>

#ifdef __aarch64__
#include <arm_neon.h>

static int kreg3_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int kreg3_run(struct test *test, int cpu) {
    (void)cpu;
    auto mask_dist = []() { return (uint16_t)((0) + (int64_t)(random64() % (uint64_t)((0xFFFF) - (0) + 1))); };
    static std::atomic<uint64_t> iter{0};

    do {
        uint16_t a_val = mask_dist();
        uint16_t b_val = mask_dist();

        /* 硬件: NEON 向量逻辑指令 vorrq/vandq/vbicq (a & ~b)/vmvnq,
         * KORTEST/KTEST 标志由横向归约指令 vmaxvq 从向量结果得出。 */
        uint16x8_t va = vdupq_n_u16(a_val);
        uint16x8_t vb = vdupq_n_u16(b_val);
        uint16x8_t vor   = vorrq_u16(va, vb);
        uint16x8_t vand  = vandq_u16(va, vb);
        uint16x8_t vandn = vbicq_u16(va, vb);      /* a & ~b */
        uint16x8_t vnor  = vmvnq_u16(vor);         /* ~(a|b) */

        /* KORTEST: ZF=(OR==0), CF=(OR==全1); KTEST: ZF=(AND==0), CF=(ANDNOT==0) */
        int hw_zf_kor = (vmaxvq_u16(vor)   == 0) ? 1 : 0;
        int hw_cf_kor = (vmaxvq_u16(vnor)  == 0) ? 1 : 0;
        int hw_zf_kt  = (vmaxvq_u16(vand)  == 0) ? 1 : 0;
        int hw_cf_kt  = (vmaxvq_u16(vandn) == 0) ? 1 : 0;

        /* 软件参考: 独立标量位运算 (原版 sw 直接赋值自 hw, 结构上恒
         * pass —— B1-004)。 */
        int sw_zf_kor = (((uint16_t)(a_val | b_val)) == 0)      ? 1 : 0;
        int sw_cf_kor = (((uint16_t)(a_val | b_val)) == 0xFFFF) ? 1 : 0;
        int sw_zf_kt  = (((uint16_t)(a_val & b_val)) == 0)      ? 1 : 0;
        int sw_cf_kt  = (((uint16_t)(a_val & ~b_val)) == 0)     ? 1 : 0;

        /* 交叉验证: 向量结果与标量 golden 的广播向量全 8 lane 比对
         * (vceqq + vminvq), 标志位 hw(向量归约) vs sw(标量) 交叉比对。 */
        bool val_pass =
            (vminvq_u16(vceqq_u16(vor,   vdupq_n_u16((uint16_t)(a_val | b_val)))) != 0) &&
            (vminvq_u16(vceqq_u16(vand,  vdupq_n_u16((uint16_t)(a_val & b_val)))) != 0) &&
            (vminvq_u16(vceqq_u16(vandn, vdupq_n_u16((uint16_t)(a_val & ~b_val)))) != 0);
        bool flag_pass = (hw_zf_kor == sw_zf_kor) && (hw_cf_kor == sw_cf_kor) &&
                         (hw_zf_kt == sw_zf_kt) && (hw_cf_kt == sw_cf_kt);
        bool passed = val_pass && flag_pass;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);

        if (!passed) {
            const char *color = passed ? "\033[32m" : "\033[31m";
            const char *result_str = passed ? "PASS" : "FAIL";
            fprintf(stderr, "kreg3: Iter %lu, a=0x%04X, b=0x%04X\n",
                    iteration, a_val, b_val);
            fprintf(stderr, "  KORTEST: hw: ZF=%d CF=%d  sw: ZF=%d CF=%d\n",
                    hw_zf_kor, hw_cf_kor, sw_zf_kor, sw_cf_kor);
            fprintf(stderr, "  KTEST :  hw: ZF=%d CF=%d  sw: ZF=%d CF=%d\n",
                    hw_zf_kt, hw_cf_kt, sw_zf_kt, sw_cf_kt);
            fprintf(stderr, "  result=%s%s\033[0m\n", color, result_str);
            report_fail_msg("kreg3: mismatch in NEON vector logic or reduced flags");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int kreg3_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#else // !__aarch64__

// On non-AArch64 (e.g. x86-64) there is no <arm_neon.h>; the test self-skips
// in test_init so the result is marked ignored rather than spuriously
// passing (kreg1.cpp precedent).
static int kreg3_init(struct test *test) {
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON required for "
             "mask register test and compare (vector logic + reduction)");
    return EXIT_SKIP;
}

static int kreg3_run(struct test *test, int cpu) {
    (void)test;
    (void)cpu;
    __builtin_unreachable();    // init reports EXIT_SKIP, run never executes
}

static int kreg3_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(kreg3, "Mask register test and compare (KORTEST, KTEST) on real NEON vector logic with cross-lane reduction")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg3_init,
    .test_run = kreg3_run,
    .test_cleanup = kreg3_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
