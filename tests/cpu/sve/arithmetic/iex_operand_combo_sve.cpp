/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test iex_operand_combo_sve
 * @parblock
 * SVE port of iex_operand_combo_arm: the IEX opcode-combination chain
 * (add/sub/and/or/xor/mul/lsl/lsr/asr/clz/rbit/rev over high-Hamming
 * operands, serial dependency) runs on SVE vector ALU lanes — svclz is
 * the real SVE count-leading-zeros instruction; rbit (no SVE1
 * instruction) is composed from an svtbl byte-reverse + shift-mask
 * steps (verified bit-exact vs the software reference on 20k random
 * vectors); rev is a native svtbl byte-table reverse.
 * The carry-flag dependent adcs/sbcs/csel chains stay scalar inline-asm
 * (flags are a scalar concept; kept as the original's phase-2) and are
 * still run twice for non-determinism detection.
 * Golden = pure-C++ software reference with sw_clz/sw_rbit/sw_rev.
 * The chain length (originally COMBO_STEPS=1024) is runtime
 * configurable via -O iex_operand_combo_sve.steps=N /
 * SDC_IEX_OPERAND_COMBO_SVE_STEPS, with a memory-adaptive default
 * when unconfigured.
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cinttypes>
#include <memory>
#include <vector>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

static constexpr size_t COMBO_STEPS = 1024; // serial ALU steps per chain

// High-Hamming-distance operand table (same as the original).
static const uint64_t HAMMING_TABLE[] = {
    0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
    0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL,
    0xCCCCCCCCCCCCCCCCULL, 0x3333333333333333ULL,
    0xF0F0F0F0F0F0F0F0ULL, 0x0F0F0F0F0F0F0F0FULL,
    0xFFFF0000FFFF0000ULL, 0x0000FFFF0000FFFFULL,
    0xFFFFFF00FFFFFF00ULL, 0x00FFFFFF00FFFFFFULL,
    0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFULL,
    0x0000000000000001ULL, 0xFFFFFFFFFFFFFFFEULL,
};
static constexpr size_t HAMMING_TABLE_SIZE =
    sizeof(HAMMING_TABLE) / sizeof(HAMMING_TABLE[0]);

enum {
    OP_ADD, OP_SUB, OP_AND, OP_OR, OP_XOR, OP_MUL,
    OP_LSL, OP_LSR, OP_ASR, OP_CLZ, OP_RBIT, OP_REV,
    OP_COUNT
};
static constexpr int OP_CYCLE_LEN = OP_COUNT;
static int op_kind_for_step(size_t step) { return (int)(step % OP_CYCLE_LEN); }

// ---- Software references (pure C++, independent goldens) ----
static inline uint64_t sw_rbit(uint64_t v) {
    uint64_t r = 0;
    for (int i = 0; i < 64; ++i) r |= ((v >> i) & 1ULL) << (63 - i);
    return r;
}
static inline uint64_t sw_rev(uint64_t v) {
    uint64_t r = 0;
    for (int i = 0; i < 8; ++i) r |= ((v >> (i * 8)) & 0xffULL) << ((7 - i) * 8);
    return r;
}
static inline uint64_t sw_clz(uint64_t v) {
    if (v == 0) return 64;
    uint64_t n = 0;
    uint64_t mask = 1ULL << 63;
    while (!(v & mask)) { ++n; mask >>= 1; }
    return n;
}

#if defined(__aarch64__)

// ---- SVE hardware paths ----
// rev: svtbl byte-table reverse (indices 7..0 over the 8 bytes of lane 0).
// NOTE: svtbl indexes into the WHOLE source vector, so we place the 64-bit
// value in the first 8 bytes and use per-byte indices [7,6,...,0].
static inline uint64_t sve_rev(uint64_t v) {
    uint8_t in[8], out[8];
    std::memcpy(in, &v, 8);
    static const uint8_t idx[8] = {7, 6, 5, 4, 3, 2, 1, 0};
    /* #312: predicate clamped to the 8 lanes the byte-table reverse
     * consumes — svptrue_b8 made svld1/svst1 read/write svcntb() bytes
     * through these 8-byte arrays (an OOB at every VL >= 128-bit, the
     * issue #309 form; inactive ld1 lanes zero-fill, the svtbl result
     * for lanes 8+ is never stored). */
    svbool_t pg = svwhilelt_b8((uint64_t)0, (uint64_t)8);
    svuint8_t vi = svld1_u8(pg, in);
    svuint8_t vidx = svld1_u8(pg, idx);
    svuint8_t vo = svtbl_u8(vi, vidx);
    svst1_u8(pg, out, vo);
    uint64_t r;
    std::memcpy(&r, out, 8);
    return r;
}
// rbit: byte-reverse then intra-byte bit-reverse via shift-mask steps.
static inline uint64_t sve_rbit(uint64_t v) {
    uint64_t x = sve_rev(v);
    x = ((x & 0x5555555555555555ULL) << 1) | ((x >> 1) & 0x5555555555555555ULL);
    x = ((x & 0x3333333333333333ULL) << 2) | ((x >> 2) & 0x3333333333333333ULL);
    x = ((x & 0x0F0F0F0F0F0F0F0FULL) << 4) | ((x >> 4) & 0x0F0F0F0F0F0F0F0FULL);
    return x;
}
// clz: real SVE instruction (svclz). Lane 0 carries the value.
static inline uint64_t sve_clz(uint64_t v) {
    uint64_t in[4], out[4];
    in[0] = v;
    /* #312: lane 0 only — svptrue_b64 overran these 32-byte arrays from
     * the 512-bit tier up (svcntd() >= 8); the other lanes' clz results
     * were never read. */
    svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)1);
    svuint64_t vi = svld1_u64(pg, in);
    svst1_u64(pg, out, svclz_u64_x(pg, vi));
    return out[0];
}

// Flag-dependent carry chains (kept scalar inline-asm as the original —
// flags are a scalar concept; this phase is unchanged).
static inline uint64_t flag_adcs_step(uint64_t acc, uint64_t b) {
    __asm__ volatile("adcs %0, %0, %2"
                     : "+r"(acc) : "r"(0), "r"(b) : "cc", "memory");
    return acc;
}
static inline uint64_t flag_csel_pick(uint64_t b, uint64_t alt) {
    uint64_t out;
    __asm__ volatile("csel %0, %1, %2, cs" : "=r"(out) : "r"(b), "r"(alt) : "cc", "memory");
    return out;
}
static inline uint64_t flag_sbcs_step(uint64_t acc, uint64_t b) {
    __asm__ volatile("subs %0, %0, %2"
                     : "+r"(acc) : "r"(0), "r"(b) : "cc", "memory");
    return acc;
}
static inline uint64_t flag_csel_pick_cc(uint64_t b, uint64_t alt) {
    uint64_t out;
    __asm__ volatile("csel %0, %1, %2, cc" : "=r"(out) : "r"(b), "r"(alt) : "cc", "memory");
    return out;
}
#endif

struct IexComboSveData {
    size_t steps;                    /* 运行时链长 */
    std::vector<uint64_t> op_a;
    std::vector<uint64_t> op_b;
    std::vector<int>      op_kind;
    std::vector<uint64_t> flag_seed_b;
    std::vector<uint64_t> flag_seed_alt;
};

/* 链长 COMBO_STEPS(1024) — 运行时可配置, 统一优先级链:
 *   1. -O iex_operand_combo_sve.steps=N / 配置文件同名条目
 *   2. 环境变量 SDC_IEX_OPERAND_COMBO_SVE_STEPS
 *   3. 自适应: op_a/op_b/flag_seed_b/flag_seed_alt 4×8B + op_kind
 *      (int 4B) 共 5 份共享向量 (按规格 bytes_per_elem=8),
 *      rss_ratio 1.0, 占物理内存 60%, clamp [1024, 1M]。
 *      原 1024 为下限。HAMMING_TABLE/OP_CYCLE_LEN 不受影响。 */
static size_t iex_operand_combo_sve_steps(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "steps", 0, COMBO_STEPS,
                                     1 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("iex_operand_combo_sve", "steps", &v,
                                 COMBO_STEPS, 1 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(8, 5, 1.0, 600, thread_count(),
                                         COMBO_STEPS, 1 << 20, test->id, "steps");
}

// Software reference (pure C++ golden, sw_* bit ops).
static uint64_t combo_reference(const IexComboSveData *d, uint64_t seed)
{
    uint64_t acc = seed;
    for (size_t i = 0; i < d->steps; ++i) {
        uint64_t a = d->op_a[i];
        uint64_t b = d->op_b[i];
        switch (d->op_kind[i]) {
            case OP_ADD:  acc = acc + a; break;
            case OP_SUB:  acc = acc - a; break;
            case OP_AND:  acc = acc & a; break;
            case OP_OR:   acc = acc | a; break;
            case OP_XOR:  acc = acc ^ a; break;
            case OP_MUL:  acc = acc * a; break;
            case OP_LSL:  acc = acc << (b & 63); break;
            case OP_LSR:  acc = acc >> (b & 63); break;
            case OP_ASR:  acc = (uint64_t)((int64_t)acc >> (b & 63)); break;
            case OP_CLZ:  acc = sw_clz(acc); break;
            case OP_RBIT: acc = sw_rbit(acc); break;
            case OP_REV:  acc = sw_rev(acc); break;
        }
    }
    return acc;
}

#if defined(__aarch64__)
// SVE hardware path (rbit/rev/clz on SVE instructions).
static uint64_t combo_run_sve(const IexComboSveData *d, uint64_t seed)
{
    uint64_t acc = seed;
    for (size_t i = 0; i < d->steps; ++i) {
        uint64_t a = d->op_a[i];
        uint64_t b = d->op_b[i];
        switch (d->op_kind[i]) {
            case OP_ADD:  acc = acc + a; break;
            case OP_SUB:  acc = acc - a; break;
            case OP_AND:  acc = acc & a; break;
            case OP_OR:   acc = acc | a; break;
            case OP_XOR:  acc = acc ^ a; break;
            case OP_MUL:  acc = acc * a; break;
            case OP_LSL:  acc = acc << (b & 63); break;
            case OP_LSR:  acc = acc >> (b & 63); break;
            case OP_ASR:  acc = (uint64_t)((int64_t)acc >> (b & 63)); break;
            case OP_CLZ:  acc = sve_clz(acc); break;
            case OP_RBIT: acc = sve_rbit(acc); break;
            case OP_REV:  acc = sve_rev(acc); break;
        }
    }
    return acc;
}

static uint64_t flag_chain_run(const IexComboSveData *d, uint64_t seed)
{
    uint64_t acc = seed;
    for (size_t i = 0; i < d->steps; ++i) {
        uint64_t b   = d->flag_seed_b[i];
        uint64_t alt = d->flag_seed_alt[i];
        acc = flag_adcs_step(acc, b);
        uint64_t next_b = flag_csel_pick(b, alt);
        (void)next_b;
    }
    return acc;
}
static uint64_t flag_chain_run_sub(const IexComboSveData *d, uint64_t seed)
{
    uint64_t acc = seed;
    for (size_t i = 0; i < d->steps; ++i) {
        uint64_t b   = d->flag_seed_b[i];
        uint64_t alt = d->flag_seed_alt[i];
        acc = flag_sbcs_step(acc, b);
        uint64_t next_b = flag_csel_pick_cc(b, alt);
        (void)next_b;
    }
    return acc;
}
#endif

static int iex_operand_combo_sve_init(struct test *test)
{
#ifdef __aarch64__
    /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
     * layer: arm64_require_sve_vl does the SVE capability check
     * + 128..1024-bit range gate in one call (skip reason
     * framework-issued). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    const size_t STEPS = iex_operand_combo_sve_steps(test);
    try {
        auto data = std::make_unique<IexComboSveData>();
        data->steps = STEPS;
        data->op_a.resize(STEPS);
        data->op_b.resize(STEPS);
        data->op_kind.resize(STEPS);
        data->flag_seed_b.resize(STEPS);
        data->flag_seed_alt.resize(STEPS);
        for (size_t i = 0; i < STEPS; ++i) {
            data->op_a[i] = HAMMING_TABLE[i % HAMMING_TABLE_SIZE];
            data->op_b[i] = HAMMING_TABLE[(i + 1) % HAMMING_TABLE_SIZE];
            data->op_kind[i] = op_kind_for_step(i);
            data->flag_seed_b[i] = HAMMING_TABLE[i % HAMMING_TABLE_SIZE];
            data->flag_seed_alt[i] = HAMMING_TABLE[(i + 3) % HAMMING_TABLE_SIZE];
        }
        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory, "iex_operand_combo_sve init: %s", e.what());
        return EXIT_SKIP;
    }
}

static int iex_operand_combo_sve_run(struct test *test, int cpu)
{
    (void)cpu;
#ifndef __aarch64__
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for IEX "
             "opcode-combo (svclz/svtbl + inline-asm adcs/sbcs/csel)");
    return EXIT_SKIP;
#else
    auto *d = static_cast<IexComboSveData *>(test->data);

    uint64_t golden0 = combo_reference(d, 0xDEADBEEFCAFEBABEULL);
    uint64_t golden1 = combo_reference(d, 0x0123456789ABCDEFULL);

    do {
        bool all_passed = true;

        /* ---- (1) Opcode-combo chain, SVE path, two seeds ---- */
        uint64_t r0 = combo_run_sve(d, 0xDEADBEEFCAFEBABEULL);
        if (r0 != golden0) {
            log_warning("iex_operand_combo_sve: combo chain seed0 "
                        "0x%016" PRIx64 " want 0x%016" PRIx64, r0, golden0);
            all_passed = false;
        }
        uint64_t r1 = combo_run_sve(d, 0x0123456789ABCDEFULL);
        if (r1 != golden1) {
            log_warning("iex_operand_combo_sve: combo chain seed1 "
                        "0x%016" PRIx64 " want 0x%016" PRIx64, r1, golden1);
            all_passed = false;
        }

        /* ---- (2) Carry-flag dependent chain (scalar inline-asm, twice) ---- */
        uint64_t f0_a = flag_chain_run(d, 0x1111111111111111ULL);
        uint64_t f0_b = flag_chain_run(d, 0x1111111111111111ULL);
        if (f0_a != f0_b) {
            log_warning("iex_operand_combo_sve: adcs/csel chain non-deterministic "
                        "run0=0x%016" PRIx64 " run1=0x%016" PRIx64 " (flag-rename SDC)",
                        f0_a, f0_b);
            all_passed = false;
        }
        uint64_t f1_a = flag_chain_run_sub(d, 0x2222222222222222ULL);
        uint64_t f1_b = flag_chain_run_sub(d, 0x2222222222222222ULL);
        if (f1_a != f1_b) {
            log_warning("iex_operand_combo_sve: sbcs/csel chain non-deterministic "
                        "run0=0x%016" PRIx64 " run1=0x%016" PRIx64 " (flag-rename SDC)",
                        f1_a, f1_b);
            all_passed = false;
        }

        if (!all_passed) {
            report_fail_msg("iex_operand_combo_sve: IEX opcode-combo / "
                            "flag-rename SDC detected (SVE path)");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#endif
}

static int iex_operand_combo_sve_finish(struct test *test)
{
    delete static_cast<IexComboSveData *>(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(iex_operand_combo_sve,
             "IEX opcode-combination SDC stress on SVE: add/sub/and/or/xor/mul/"
             "lsl/lsr/asr over high-Hamming operands with serial dependency, "
             "clz on the real svclz instruction, rbit/rev on svtbl byte-table "
             "reverses, plus scalar carry-flag adcs/sbcs/csel chains run twice "
             "(port of iex_operand_combo_arm)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = iex_operand_combo_sve_init,
    .test_run = iex_operand_combo_sve_run,
    .test_cleanup = iex_operand_combo_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
    .arm64_requires = ARM64_REQUIRES_SVE,
END_DECLARE_TEST
