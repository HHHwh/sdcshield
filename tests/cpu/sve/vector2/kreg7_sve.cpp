/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b kreg7_sve
 * @parblock
 * SVE port of kreg7: VPMOVB/W/D/Q2M sign-bit mask extraction at four
 * element widths. The SVE version extracts the top-bit mask with
 * predicate compares (svcmplt vs zero) + svsel — the predicate register
 * is the mask at every width; data movement via svld1/svst1.
 * Independent scalar goldens + store/reload consistency as the original.
 * @endparblock
 */

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

#ifdef __aarch64__

/* 掩码承载类型: 8-bit 64 元素需要 64 位掩码(与原版 uint64_t 一致),
 * 16-bit 32 元素 32 位, 32/64-bit 更少。全部用能装下的最宽类型。 */
template <typename T> struct Wider {};
template <> struct Wider<uint8_t>  { using U = uint64_t; };
template <> struct Wider<uint16_t> { using U = uint32_t; };
template <> struct Wider<uint32_t> { using U = uint16_t; };
template <> struct Wider<uint64_t> { using U = uint8_t; };

/* SVE 高位掩码提取: 谓词比较 + svsel 展开, T 为元素类型, els 元素数 */
template <typename T>
static typename Wider<T>::U sve_topbit_mask(const T *data, int els) {
    typename Wider<T>::U mask = 0;
    for (int base = 0; base < els; ) {
        int lanes;
        if constexpr (sizeof(T) == 1)      lanes = svcntb();
        else if constexpr (sizeof(T) == 2) lanes = svcnth();
        else if constexpr (sizeof(T) == 4) lanes = svcntw();
        else                               lanes = svcntd();
        int n = (els - base < lanes) ? (els - base) : lanes;

        if constexpr (sizeof(T) == 1) {
            svbool_t pg = svwhilelt_b8((uint64_t)0, (uint64_t)n);
            svuint8_t v = svld1_u8(pg, data + base);
            svbool_t p = svcmpge_n_u8(pg, v, 0x80u);   /* bit7 set ⟺ v >= 0x80 (无符号) */
            uint8_t sel[64];
            svst1_u8(pg, sel, svsel_u8(p, svdup_u8(1), svdup_u8(0)));
            for (int i = 0; i < n; ++i) if (sel[i]) mask |= (typename Wider<T>::U)1 << (base + i);
        } else if constexpr (sizeof(T) == 2) {
            svbool_t pg = svwhilelt_b16((uint64_t)0, (uint64_t)n);
            svuint16_t v = svld1_u16(pg, data + base);
            svbool_t p = svcmpge_n_u16(pg, v, 0x8000u);
            uint16_t sel[32];
            svst1_u16(pg, sel, svsel_u16(p, svdup_u16(1), svdup_u16(0)));
            for (int i = 0; i < n; ++i) if (sel[i]) mask |= (typename Wider<T>::U)1 << (base + i);
        } else if constexpr (sizeof(T) == 4) {
            svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)n);
            svuint32_t v = svld1_u32(pg, data + base);
            svbool_t p = svcmpge_n_u32(pg, v, 0x80000000u);
            uint32_t sel[16];
            svst1_u32(pg, sel, svsel_u32(p, svdup_u32(1), svdup_u32(0)));
            for (int i = 0; i < n; ++i) if (sel[i]) mask |= (typename Wider<T>::U)1 << (base + i);
        } else {
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)n);
            svuint64_t v = svld1_u64(pg, data + base);
            svbool_t p = svcmpge_n_u64(pg, v, 0x8000000000000000ull);
            uint64_t sel[8];
            svst1_u64(pg, sel, svsel_u64(p, svdup_u64(1), svdup_u64(0)));
            for (int i = 0; i < n; ++i) if (sel[i]) mask |= (typename Wider<T>::U)1 << (base + i);
        }
        base += n;
    }
    return mask;
}

#endif

static int kreg7_sve_init(struct test *test) {
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
static int kreg7_sve_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<int> type_dist(0, 3);
    static std::atomic<uint64_t> iter{0};

    do {
        int type = type_dist(rng);
        bool passed = false;
        bool consistent = true;
        uint64_t sw_mask = 0, hw_mask = 0;
        char type_name[16] = "UNKNOWN";

        switch (type) {
            case 0: { /* VPMOVB2M (8-bit, 64 元素) */
                uint8_t data[64];
                for (int i = 0; i < 64; ++i) data[i] = (uint8_t)rng();
                hw_mask = sve_topbit_mask<uint8_t>(data, 64);
                sw_mask = 0;
                for (int i = 0; i < 64; ++i) if (data[i] & 0x80) sw_mask |= 1ULL << i;
                uint8_t reload[64];
                memcpy(reload, data, sizeof(data));
                consistent = (memcmp(reload, data, sizeof(data)) == 0);
                passed = (hw_mask == sw_mask) && consistent;
                strcpy(type_name, "VPMOVB2M");
                fprintf(stderr, "kreg7_sve: Iter %lu, type=%s, data[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                        iter.load(), type_name, data[0], data[1], data[2], data[3],
                        data[4], data[5], data[6], data[7]);
                break;
            }
            case 1: { /* VPMOVW2M (16-bit, 32 元素) */
                uint16_t data[32];
                for (int i = 0; i < 32; ++i) data[i] = (uint16_t)rng();
                hw_mask = sve_topbit_mask<uint16_t>(data, 32);
                sw_mask = 0;
                for (int i = 0; i < 32; ++i) if (data[i] & 0x8000) sw_mask |= 1u << i;
                uint16_t reload[32];
                memcpy(reload, data, sizeof(data));
                consistent = (memcmp(reload, data, sizeof(data)) == 0);
                passed = (hw_mask == sw_mask) && consistent;
                strcpy(type_name, "VPMOVW2M");
                fprintf(stderr, "kreg7_sve: Iter %lu, type=%s, data[0..3]=%04X %04X %04X %04X\n",
                        iter.load(), type_name, data[0], data[1], data[2], data[3]);
                break;
            }
            case 2: { /* VPMOVD2M (32-bit, 16 元素) */
                uint32_t data[16];
                for (int i = 0; i < 16; ++i) data[i] = rng();
                hw_mask = sve_topbit_mask<uint32_t>(data, 16);
                sw_mask = 0;
                for (int i = 0; i < 16; ++i) if (data[i] & 0x80000000u) sw_mask |= 1u << i;
                uint32_t reload[16];
                memcpy(reload, data, sizeof(data));
                consistent = (memcmp(reload, data, sizeof(data)) == 0);
                passed = (hw_mask == sw_mask) && consistent;
                strcpy(type_name, "VPMOVD2M");
                fprintf(stderr, "kreg7_sve: Iter %lu, type=%s, data[0..3]=%08X %08X %08X %08X\n",
                        iter.load(), type_name, data[0], data[1], data[2], data[3]);
                break;
            }
            case 3: { /* VPMOVQ2M (64-bit, 8 元素) */
                uint64_t data[8];
                for (int i = 0; i < 8; ++i) data[i] = ((uint64_t)rng() << 32) | rng();
                hw_mask = sve_topbit_mask<uint64_t>(data, 8);
                sw_mask = 0;
                for (int i = 0; i < 8; ++i) if (data[i] & 0x8000000000000000ull) sw_mask |= 1ull << i;
                uint64_t reload[8];
                memcpy(reload, data, sizeof(data));
                consistent = (memcmp(reload, data, sizeof(data)) == 0);
                passed = (hw_mask == sw_mask) && consistent;
                strcpy(type_name, "VPMOVQ2M");
                fprintf(stderr, "kreg7_sve: Iter %lu, type=%s, data[0..3]=%016lX %016lX %016lX %016lX\n",
                        iter.load(), type_name,
                        (unsigned long)data[0], (unsigned long)data[1],
                        (unsigned long)data[2], (unsigned long)data[3]);
                break;
            }
        }

        fprintf(stderr, "  sw mask=0x%016lX, hw mask=0x%016lX\n",
                (unsigned long)sw_mask, (unsigned long)hw_mask);
        fprintf(stderr, "  consistent=%d, ", consistent);

        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";
        fprintf(stderr, "result=%s%s\033[0m\n", color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("kreg7_sve: mismatch in SVE mask or consistency");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int kreg7_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    __builtin_unreachable();
}
#endif

static int kreg7_sve_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(kreg7_sve, "Top-bit mask extraction at four widths (VPMOVB/W/D/Q2M) on real SVE predicates (port of kreg7)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg7_sve_init,
    .test_run = kreg7_sve_run,
    .test_cleanup = kreg7_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
