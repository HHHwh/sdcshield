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

static int kreg9_sve_init(struct test *test) {
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
/* KMOVB/W/D/Q 的 SVE 等价: 掩码值经向量通路传输 (svld1/svst1),
 * 并用谓词计数 (svcntp) 验证非零 lane 数 —— 传输保真 + 谓词统计双校验。 */
template <typename T>
static bool kmov_sve(T orig, T &gpr_out, uint64_t &pred_count, uint64_t &lanes_out) {
    /* VL 无关: 按实际谓词宽度取 lane 数 (svcnt* 由元素宽度定) */
    int lanes;
    if constexpr (sizeof(T) == 1)      lanes = svcntb();
    else if constexpr (sizeof(T) == 2) lanes = svcnth();
    else if constexpr (sizeof(T) == 4) lanes = svcntw();
    else                               lanes = svcntd();
    if (lanes > 64) lanes = 64;
    T in[64], out[64];
    for (int i = 0; i < lanes; ++i) in[i] = orig;

    svbool_t pg;
    if constexpr (sizeof(T) == 1)      pg = svwhilelt_b8((uint64_t)0, (uint64_t)lanes);
    else if constexpr (sizeof(T) == 2) pg = svwhilelt_b16((uint64_t)0, (uint64_t)lanes);
    else if constexpr (sizeof(T) == 4) pg = svwhilelt_b32((uint64_t)0, (uint64_t)lanes);
    else                               pg = svwhilelt_b64((uint64_t)0, (uint64_t)lanes);

    if constexpr (sizeof(T) == 1) {
        svuint8_t v = svld1_u8(pg, in);
        svbool_t p = svcmpne_n_u8(pg, v, 0);
        pred_count = svcntp_b8(pg, p);
        svst1_u8(pg, out, v);
    } else if constexpr (sizeof(T) == 2) {
        svuint16_t v = svld1_u16(pg, in);
        svbool_t p = svcmpne_n_u16(pg, v, 0);
        pred_count = svcntp_b16(pg, p);
        svst1_u16(pg, out, v);
    } else if constexpr (sizeof(T) == 4) {
        svuint32_t v = svld1_u32(pg, in);
        svbool_t p = svcmpne_n_u32(pg, v, 0);
        pred_count = svcntp_b32(pg, p);
        svst1_u32(pg, out, v);
    } else {
        svuint64_t v = svld1_u64(pg, in);
        svbool_t p = svcmpne_n_u64(pg, v, 0);
        pred_count = svcntp_b64(pg, p);
        svst1_u64(pg, out, v);
    }
    gpr_out = out[0];
    lanes_out = (uint64_t)lanes;
    return true;
}

static int kreg9_sve_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<int> type_dist(0, 3);
    static std::atomic<uint64_t> iter{0};

    do {
        int type = type_dist(rng);
        bool passed = false;
        bool consistent = true;
        char type_name[16] = "UNKNOWN";

        switch (type) {
            case 0: { /* KMOVB (8-bit) */
                uint8_t orig = static_cast<uint8_t>(rng() & 0xFF);
                uint8_t gpr; uint64_t pc, lanes_used;
                kmov_sve<uint8_t>(orig, gpr, pc, lanes_used);
                bool equal = (gpr == orig);
                uint64_t sw_pc = orig ? lanes_used : 0;   /* 实际 lanes 全载 orig */
                bool pred_ok = (pc == sw_pc);
                uint8_t store = gpr;
                uint8_t reload;
                memcpy(&reload, &store, sizeof(store));
                bool cons = (reload == gpr);
                consistent = cons;
                passed = equal && cons && pred_ok;
                strcpy(type_name, "KMOVB");
                fprintf(stderr, "kreg9_sve: Iter %lu, type=%s, orig=0x%02X, gpr=0x%02X, pc=%lu/%lu\n",
                        iter.load(), type_name, orig, gpr, (unsigned long)pc, (unsigned long)sw_pc);
                break;
            }
            case 1: { /* KMOVW (16-bit) */
                uint16_t orig = static_cast<uint16_t>(rng() & 0xFFFF);
                uint16_t gpr; uint64_t pc, lanes_used;
                kmov_sve<uint16_t>(orig, gpr, pc, lanes_used);
                bool equal = (gpr == orig);
                uint64_t sw_pc = orig ? lanes_used : 0;
                bool pred_ok = (pc == sw_pc);
                uint16_t store = gpr;
                uint16_t reload;
                memcpy(&reload, &store, sizeof(store));
                bool cons = (reload == gpr);
                consistent = cons;
                passed = equal && cons && pred_ok;
                strcpy(type_name, "KMOVW");
                fprintf(stderr, "kreg9_sve: Iter %lu, type=%s, orig=0x%04X, gpr=0x%04X, pc=%lu/%lu\n",
                        iter.load(), type_name, orig, gpr, (unsigned long)pc, (unsigned long)sw_pc);
                break;
            }
            case 2: { /* KMOVD (32-bit) */
                uint32_t orig = rng();
                uint32_t gpr; uint64_t pc, lanes_used;
                kmov_sve<uint32_t>(orig, gpr, pc, lanes_used);
                bool equal = (gpr == orig);
                uint64_t sw_pc = orig ? lanes_used : 0;
                bool pred_ok = (pc == sw_pc);
                uint32_t store = gpr;
                uint32_t reload;
                memcpy(&reload, &store, sizeof(store));
                bool cons = (reload == gpr);
                consistent = cons;
                passed = equal && cons && pred_ok;
                strcpy(type_name, "KMOVD");
                fprintf(stderr, "kreg9_sve: Iter %lu, type=%s, orig=0x%08X, gpr=0x%08X, pc=%lu/%lu\n",
                        iter.load(), type_name, orig, gpr, (unsigned long)pc, (unsigned long)sw_pc);
                break;
            }
            case 3: { /* KMOVQ (64-bit) */
                uint64_t orig = (static_cast<uint64_t>(rng()) << 32) | rng();
                uint64_t gpr; uint64_t pc, lanes_used;
                kmov_sve<uint64_t>(orig, gpr, pc, lanes_used);
                bool equal = (gpr == orig);
                uint64_t sw_pc = orig ? lanes_used : 0;
                bool pred_ok = (pc == sw_pc);
                uint64_t store = gpr;
                uint64_t reload;
                memcpy(&reload, &store, sizeof(store));
                bool cons = (reload == gpr);
                consistent = cons;
                passed = equal && cons && pred_ok;
                strcpy(type_name, "KMOVQ");
                fprintf(stderr, "kreg9_sve: Iter %lu, type=%s, orig=0x%016lX, gpr=0x%016lX, pc=%lu/%lu\n",
                        iter.load(), type_name, (unsigned long)orig, (unsigned long)gpr,
                        (unsigned long)pc, (unsigned long)sw_pc);
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
            report_fail_msg("kreg9_sve: mismatch in SVE mask move or consistency");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int kreg9_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for kreg9_sve");
    return EXIT_SKIP;
}
#endif

static int kreg9_sve_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(kreg9_sve, "Mask register moves (KMOVB/W/D/Q) via SVE vector/predicate lanes with svcntp verification")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg9_sve_init,
    .test_run = kreg9_sve_run,
    .test_cleanup = kreg9_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
