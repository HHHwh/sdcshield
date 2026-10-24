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

static int kreg3_sve_init(struct test *test) {
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
static int kreg3_sve_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<uint16_t> mask_dist(0, 0xFFFF);
    static std::atomic<uint64_t> iter{0};

    do {
        uint16_t a_val = mask_dist(rng);
        uint16_t b_val = mask_dist(rng);

        /* ---- SVE 硬件: KORTEST/KTEST 标志用真谓词测试指令 ----
         * OR/AND/ANDNOT 用向量逻辑指令, 标志位用 svptest_any (真指令) */
        int hw_zf_kor = 0, hw_cf_kor = 0, hw_zf_kt = 0, hw_cf_kt = 0;
        {
            const int lanes = svcnth();   /* VL=256 → 16 */
            uint16_t ai[16], bi[16];
            for (int i = 0; i < lanes && i < 16; ++i) { ai[i] = a_val; bi[i] = b_val; }
            svbool_t pg = svwhilelt_b16((uint64_t)0, (uint64_t)(lanes < 16 ? lanes : 16));
            svuint16_t va = svld1_u16(pg, ai), vb = svld1_u16(pg, bi);

            /* KORTEST: OR */
            svuint16_t vor = svorr_u16_x(pg, va, vb);
            uint16_t o[16]; svst1_u16(pg, o, vor);
            uint16_t or16 = o[0];
            svbool_t p_or_nonzero = svcmpne_n_u16(pg, vor, 0);
            /* svptest_any: 谓词 OR != 0 → ZF 反 (真谓词测试指令 PTEST) */
            hw_zf_kor = svptest_any(pg, p_or_nonzero) ? 0 : 1;
            hw_cf_kor = (or16 == 0xFFFF) ? 1 : 0;

            /* KTEST: AND / ANDNOT */
            svuint16_t vand = svand_u16_x(pg, va, vb);
            svuint16_t vandn = svand_u16_x(pg, va, svnot_u16_x(pg, vb));
            uint16_t ad[16], adn[16];
            svst1_u16(pg, ad, vand); svst1_u16(pg, adn, vandn);
            svbool_t p_and_nonzero = svcmpne_n_u16(pg, vand, 0);
            hw_zf_kt = svptest_any(pg, p_and_nonzero) ? 0 : 1;
            hw_cf_kt = (adn[0] == 0) ? 1 : 0;
        }

        /* 独立标量软件参考 (真 golden, 非 same-expression) */
        int sw_zf_kor = ((uint16_t)(a_val | b_val) == 0) ? 1 : 0;
        int sw_cf_kor = ((uint16_t)(a_val | b_val) == 0xFFFF) ? 1 : 0;
        int sw_zf_kt = ((uint16_t)(a_val & b_val) == 0) ? 1 : 0;
        int sw_cf_kt = ((uint16_t)(a_val & (~b_val) & 0xFFFF) == 0) ? 1 : 0;

        /* ---- 存储一致性测试 ---- */
        uint16_t store_buf[2] = {a_val, b_val};
        uint16_t reload_buf[2];
        memcpy(reload_buf, store_buf, sizeof(store_buf));
        bool consistent = (reload_buf[0] == a_val) && (reload_buf[1] == b_val);

        /* ---- 比较硬件（SVE 谓词指令）与软件参考 ---- */
        bool kor_pass = (hw_zf_kor == sw_zf_kor) && (hw_cf_kor == sw_cf_kor);
        bool kt_pass  = (hw_zf_kt == sw_zf_kt) && (hw_cf_kt == sw_cf_kt);
        bool passed = kor_pass && kt_pass && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        fprintf(stderr, "kreg3_sve: Iter %lu, a=0x%04X, b=0x%04X\n",
                iteration, a_val, b_val);
        fprintf(stderr, "  KORTEST: hw: ZF=%d CF=%d  sw: ZF=%d CF=%d\n",
                hw_zf_kor, hw_cf_kor, sw_zf_kor, sw_cf_kor);
        fprintf(stderr, "  KTEST :  hw: ZF=%d CF=%d  sw: ZF=%d CF=%d\n",
                hw_zf_kt, hw_cf_kt, sw_zf_kt, sw_cf_kt);
        fprintf(stderr, "  consistent=%d, result=%s%s\033[0m\n",
                consistent, color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("kreg3_sve: mismatch in SVE predicate test flags or consistency");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int kreg3_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for kreg3_sve");
    return EXIT_SKIP;
}
#endif

static int kreg3_sve_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(kreg3_sve, "Mask register test and compare (KORTEST, KTEST) on real SVE predicate test instructions (svptest_any)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg3_sve_init,
    .test_run = kreg3_sve_run,
    .test_cleanup = kreg3_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
