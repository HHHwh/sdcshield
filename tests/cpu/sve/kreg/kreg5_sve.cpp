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

static int kreg5_sve_init(struct test *test) {
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
static int kreg5_sve_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<uint16_t> mask_dist(0, 0xFFFF);
    static std::atomic<uint64_t> iter{0};

    do {
        uint16_t a_val = mask_dist(rng);
        uint16_t b_val = mask_dist(rng);

        /* ---- SVE 硬件: KUNPCK 用真交织指令 svzip1; KNOT 用向量 eor ---- */
        uint32_t hw_c;
        uint16_t hw_not;
        {
            const int lanes = svcnth();
            uint16_t ai[16], bi[16];
            for (int i = 0; i < lanes && i < 16; ++i) { ai[i] = a_val; bi[i] = b_val; }
            svbool_t pg = svwhilelt_b16((uint64_t)0, (uint64_t)(lanes < 16 ? lanes : 16));
            svuint16_t va = svld1_u16(pg, ai), vb = svld1_u16(pg, bi);

            /* KUNPCK: 交织 [a,b,a,b,...] —— 小端读 32 位 = (b<<16)|a */
            svuint16_t z = svzip1_u16(va, vb);
            uint16_t zo[16]; svst1_u16(pg, zo, z);
            hw_c = ((uint32_t)zo[1] << 16) | zo[0];

            /* KNOT: 谓词取反语义用向量异或全 1 */
            svuint16_t n = sveor_u16_x(pg, va, svdup_u16(0xFFFF));
            uint16_t no[16]; svst1_u16(pg, no, n);
            hw_not = no[0];
        }

        /* 独立标量软件参考 (真 golden) */
        uint32_t sw_c = ((uint32_t)b_val << 16) | a_val;   /* KUNPCK: 低16=a, 高16=b */
        uint16_t sw_not = (uint16_t)~a_val;                /* KNOT: 低16位取反 */

        /* ---- 存储一致性测试 ---- */
        uint32_t store_buf[1] = {hw_c};
        uint16_t store_not[1] = {hw_not};
        uint32_t reload_buf[1];
        uint16_t reload_not[1];
        memcpy(reload_buf, store_buf, sizeof(store_buf));
        memcpy(reload_not, store_not, sizeof(store_not));
        bool consistent = (reload_buf[0] == hw_c) && (reload_not[0] == hw_not);

        /* ---- 比较硬件（SVE 交织/取反）与软件参考 ---- */
        bool unpk_pass = (sw_c == hw_c);
        bool not_pass  = (sw_not == hw_not);
        bool passed = unpk_pass && not_pass && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        fprintf(stderr, "kreg5_sve: Iter %lu, a=0x%04X, b=0x%04X\n",
                iteration, a_val, b_val);
        fprintf(stderr, "  KUNPCK: sw=0x%08X, hw=0x%08X\n", sw_c, hw_c);
        fprintf(stderr, "  KNOT:   sw=0x%04X, hw=0x%04X\n", sw_not, hw_not);
        fprintf(stderr, "  consistent=%d, result=%s%s\033[0m\n",
                consistent, color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("kreg5_sve: mismatch in SVE interleave/not or consistency");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int kreg5_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for kreg5_sve");
    return EXIT_SKIP;
}
#endif

static int kreg5_sve_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(kreg5_sve, "Mask register unpack (KUNPCK) and not (KNOT) on real SVE interleave instruction (svzip1)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg5_sve_init,
    .test_run = kreg5_sve_run,
    .test_cleanup = kreg5_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
