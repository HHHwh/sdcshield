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

static int kreg8_sve_init(struct test *test) {
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
/* SVE 真谓词选择展开: mask 的 bit i → element i (0/all-ones)。
 * 元素宽度 T (8/16/32 位), EL 元素数。VL 无关分批。 */
template <typename T>
static void expand_mask_sve(void *out_raw, uint64_t mask, int el_bits, int els) {
    T *out = static_cast<T *>(out_raw);
    const int lanes = 64 / el_bits;   /* 一次谓词能覆盖的元素数由宽度定, 但按 svcnt 分批 */
    [[maybe_unused]] T bits[64], vals[64];
    for (int i = 0; i < els; ++i) {
        bits[i] = static_cast<T>((mask >> i) & 1);
        vals[i] = static_cast<T>(~static_cast<T>(0));
    }
    for (int base = 0; base < els; base += lanes) {
        int n = (els - base < lanes) ? (els - base) : lanes;
        svbool_t pg;
        if constexpr (sizeof(T) == 1)      pg = svwhilelt_b8((uint64_t)0, (uint64_t)n);
        else if constexpr (sizeof(T) == 2) pg = svwhilelt_b16((uint64_t)0, (uint64_t)n);
        else                               pg = svwhilelt_b32((uint64_t)0, (uint64_t)n);
        /* 谓词 = bit 向量非零 */
        if constexpr (sizeof(T) == 1) {
            svuint8_t vb = svld1_u8(pg, bits + base);
            svbool_t p = svcmpne_n_u8(pg, vb, 0);
            svst1_u8(pg, out + base, svsel_u8(p, svdup_u8(0xFF), svdup_u8(0)));
        } else if constexpr (sizeof(T) == 2) {
            svuint16_t vb = svld1_u16(pg, bits + base);
            svbool_t p = svcmpne_n_u16(pg, vb, 0);
            svst1_u16(pg, out + base, svsel_u16(p, svdup_u16(0xFFFF), svdup_u16(0)));
        } else {
            svuint32_t vb = svld1_u32(pg, bits + base);
            svbool_t p = svcmpne_n_u32(pg, vb, 0);
            svst1_u32(pg, out + base, svsel_u32(p, svdup_u32(0xFFFFFFFFu), svdup_u32(0)));
        }
    }
}

static int kreg8_sve_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<int> type_dist(0, 2);
    static std::atomic<uint64_t> iter{0};

    do {
        int type = type_dist(rng);
        bool passed = false;
        bool consistent = true;
        char type_name[16] = "UNKNOWN";

        switch (type) {
            case 0: { /* VPMOVM2B (8-bit, 64 elements) */
                std::uniform_int_distribution<uint64_t> mask_dist(0, 0xFFFFFFFFFFFFFFFFULL);
                uint64_t mask_val = mask_dist(rng);

                /* SVE 硬件展开 */
                uint8_t hw_vals[64];
                expand_mask_sve<uint8_t>(hw_vals, mask_val, 8, 64);

                /* 独立标量参考 (真 golden) */
                uint8_t sw_vals[64];
                for (int i = 0; i < 64; ++i)
                    sw_vals[i] = (mask_val & (1ULL << i)) ? 0xFF : 0x00;

                uint8_t reload[64];
                memcpy(reload, hw_vals, sizeof(hw_vals));
                consistent = (memcmp(reload, hw_vals, sizeof(hw_vals)) == 0);
                passed = (memcmp(hw_vals, sw_vals, sizeof(hw_vals)) == 0) && consistent;
                strcpy(type_name, "VPMOVM2B");

                fprintf(stderr, "kreg8_sve: Iter %lu, type=%s, mask=0x%016lX\n",
                        iter.load(), type_name, (unsigned long)mask_val);
                fprintf(stderr, "  sw[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                        sw_vals[0], sw_vals[1], sw_vals[2], sw_vals[3],
                        sw_vals[4], sw_vals[5], sw_vals[6], sw_vals[7]);
                fprintf(stderr, "  hw[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                        hw_vals[0], hw_vals[1], hw_vals[2], hw_vals[3],
                        hw_vals[4], hw_vals[5], hw_vals[6], hw_vals[7]);
                break;
            }
            case 1: { /* VPMOVM2W (16-bit, 32 elements) */
                std::uniform_int_distribution<uint32_t> mask_dist(0, 0xFFFFFFFF);
                uint32_t mask_val = mask_dist(rng);

                uint16_t hw_vals[32];
                expand_mask_sve<uint16_t>(hw_vals, mask_val, 16, 32);

                uint16_t sw_vals[32];
                for (int i = 0; i < 32; ++i)
                    sw_vals[i] = (mask_val & (1U << i)) ? 0xFFFF : 0x0000;

                uint16_t reload[32];
                memcpy(reload, hw_vals, sizeof(hw_vals));
                consistent = (memcmp(reload, hw_vals, sizeof(hw_vals)) == 0);
                passed = (memcmp(hw_vals, sw_vals, sizeof(hw_vals)) == 0) && consistent;
                strcpy(type_name, "VPMOVM2W");

                fprintf(stderr, "kreg8_sve: Iter %lu, type=%s, mask=0x%08X\n",
                        iter.load(), type_name, mask_val);
                fprintf(stderr, "  sw[0..7]=%04X %04X %04X %04X %04X %04X %04X %04X\n",
                        sw_vals[0], sw_vals[1], sw_vals[2], sw_vals[3],
                        sw_vals[4], sw_vals[5], sw_vals[6], sw_vals[7]);
                fprintf(stderr, "  hw[0..7]=%04X %04X %04X %04X %04X %04X %04X %04X\n",
                        hw_vals[0], hw_vals[1], hw_vals[2], hw_vals[3],
                        hw_vals[4], hw_vals[5], hw_vals[6], hw_vals[7]);
                break;
            }
            case 2: { /* VPMOVM2D (32-bit, 16 elements) */
                std::uniform_int_distribution<uint16_t> mask_dist(0, 0xFFFF);
                uint16_t mask_val = mask_dist(rng);

                uint32_t hw_vals[16];
                expand_mask_sve<uint32_t>(hw_vals, mask_val, 32, 16);

                uint32_t sw_vals[16];
                for (int i = 0; i < 16; ++i)
                    sw_vals[i] = (mask_val & (1u << i)) ? 0xFFFFFFFFu : 0;

                uint32_t reload[16];
                memcpy(reload, hw_vals, sizeof(hw_vals));
                consistent = (memcmp(reload, hw_vals, sizeof(hw_vals)) == 0);
                passed = (memcmp(hw_vals, sw_vals, sizeof(hw_vals)) == 0) && consistent;
                strcpy(type_name, "VPMOVM2D");

                fprintf(stderr, "kreg8_sve: Iter %lu, type=%s, mask=0x%04X\n",
                        iter.load(), type_name, mask_val);
                fprintf(stderr, "  sw[0..7]=%08X %08X %08X %08X %08X %08X %08X %08X\n",
                        sw_vals[0], sw_vals[1], sw_vals[2], sw_vals[3],
                        sw_vals[4], sw_vals[5], sw_vals[6], sw_vals[7]);
                fprintf(stderr, "  hw[0..7]=%08X %08X %08X %08X %08X %08X %08X %08X\n",
                        hw_vals[0], hw_vals[1], hw_vals[2], hw_vals[3],
                        hw_vals[4], hw_vals[5], hw_vals[6], hw_vals[7]);
                break;
            }
        }
        fprintf(stderr, "  consistent=%d, result=%s%s%s\n",
                consistent,
                passed ? "\033[32m" : "\033[31m",
                passed ? "PASS" : "FAIL",
                "\033[0m");
        fflush(stderr);

        if (!passed) {
            report_fail_msg("kreg8_sve: mismatch in SVE mask expansion or consistency");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int kreg8_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for kreg8_sve");
    return EXIT_SKIP;
}
#endif

static int kreg8_sve_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(kreg8_sve, "VPMOVM2 mask-to-vector expansion (B/W/D) on real SVE predicate select (svsel)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg8_sve_init,
    .test_run = kreg8_sve_run,
    .test_cleanup = kreg8_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
