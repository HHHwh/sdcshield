/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mem_disambig_alu_sve
 * @parblock
 * SVE port of mem_disambig_alu (source: missing_testcases_20261009/
 * tests/cpu/memory/mem_disambig_alu.cpp).
 *
 * Memory-disambiguation predictor probe: store-to-load forwarding at a
 * RANDOM, data-dependent address. The scalar original stores res to
 * buffer+off and immediately reloads the same computed address; the
 * load's address is only known after AGU computation, so the predictor
 * must forward from the just-executed store at an unpredictable
 * address (vs the fixed sequential stride of the mrn family).
 *
 * SVE form: per pattern, svdup_u64(res) is stored with a full-VL
 * svst1_u64 at buffer + off/8 and immediately reloaded with an
 * asm-guaranteed ldr z (the reload pins to the store); every lane must
 * read res back. The rotation is {value ^ off, value + off, value &
 * off, value - off} (i%4) — res computed per pattern exactly as the
 * scalar original (the ALU is not the probe; the random address is).
 *
 * Deviations from the scalar original (documented):
 *   - all vector lanes carry the same pattern value (the original is
 *     scalar — one qword per pattern); the probed path is the
 *     random-address st1d->ld1d forwarding on SVE lanes;
 *   - the offset draw clamps to (SIZE - lanes*8) so the full-VL
 *     store/reload never leaves the 4 KB scratch buffer (the scalar
 *     original clamped to SIZE-16 for its single qword); offsets are
 *     8-byte aligned either way. Offsets are drawn once in init and
 *     replayed identically every sweep (original semantics).
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <sys/auxv.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif
#endif

#define DISAMBIG_ALU_SVE_BUFFER_SIZE 4096
#define DISAMBIG_ALU_SVE_PATTERNS 512

struct MemDisambigAluSveData {
    uint64_t *offsets;    /* byte offsets, 8-aligned, clamped to SIZE - lanes*8 */
    uint64_t *values;
    uint64_t *expected;
    int lanes;
};

static int mem_disambig_alu_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for mem_disambig_alu_sve");
        return EXIT_SKIP;
    }
#endif
    auto *data = static_cast<MemDisambigAluSveData *>(malloc(sizeof(MemDisambigAluSveData)));
    data->lanes = (int)svcntd();
    data->offsets = static_cast<uint64_t *>(malloc(DISAMBIG_ALU_SVE_PATTERNS * sizeof(uint64_t)));
    data->values = static_cast<uint64_t *>(malloc(DISAMBIG_ALU_SVE_PATTERNS * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(malloc(DISAMBIG_ALU_SVE_PATTERNS * sizeof(uint64_t)));

    const uint64_t max_off = DISAMBIG_ALU_SVE_BUFFER_SIZE - (uint64_t)data->lanes * 8;
    for (int i = 0; i < DISAMBIG_ALU_SVE_PATTERNS; i++) {
        data->offsets[i] = (random64() % max_off) & ~7ULL;
        data->values[i] = random64();
        switch (i % 4) {
            case 0: data->expected[i] = data->values[i] ^ data->offsets[i]; break;
            case 1: data->expected[i] = data->values[i] + data->offsets[i]; break;
            case 2: data->expected[i] = data->values[i] & data->offsets[i]; break;
            case 3: data->expected[i] = data->values[i] - data->offsets[i]; break;
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)

static inline svuint64_t load_vec_sve(const void *addr)
{
    svuint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

static int mem_disambig_alu_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MemDisambigAluSveData *>(test->data);

    /* VL fail-closed: 共享 offset 按 init 时的 VL 定容 */
    if ((int)svcntd() != data->lanes) {
        log_error("mem_disambig_alu_sve: vector length changed between init (%d) and run (%d)",
                  data->lanes, (int)svcntd());
        report_fail_msg("mem_disambig_alu_sve: vector length changed between init and run");
    }

    uint8_t *buffer = static_cast<uint8_t *>(aligned_alloc(64, DISAMBIG_ALU_SVE_BUFFER_SIZE));
    svbool_t pg = svptrue_b64();

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < DISAMBIG_ALU_SVE_PATTERNS; i++) {
            uint64_t off = data->offsets[i];
            uint64_t value = data->values[i];
            uint64_t res;
            switch (i % 4) {
                case 0: res = value ^ off; break;
                case 1: res = value + off; break;
                case 2: res = value & off; break;
                default: res = value - off; break;
            }

            /* 数据依赖地址上的 store->load 转发: svst1 满宽, asm ldr z 立即回读 */
            uint64_t *addr = reinterpret_cast<uint64_t *>(buffer + off);
            svuint64_t v = svdup_u64(res);
            svst1_u64(pg, addr, v);
            svuint64_t got = load_vec_sve(addr);

            if (svptest_any(pg, svcmpne_n_u64(pg, got, res))) {
                report_fail_msg("mem_disambig_alu_sve: store-to-load forward (ALU) mismatch at "
                                "pattern %d (offset %llu): expected 0x%llx",
                                i, (unsigned long long)off,
                                (unsigned long long)res);
            }
        }
    }

    free(buffer);
    return EXIT_SUCCESS;
}

#else

static int mem_disambig_alu_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for mem_disambig_alu_sve");
    return EXIT_SKIP;
}

#endif

static int mem_disambig_alu_sve_cleanup(struct test *test)
{
    auto *data = static_cast<MemDisambigAluSveData *>(test->data);
    if (data) {
        free(data->offsets);
        free(data->values);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mem_disambig_alu_sve,
             "SVE mem-disambiguation probe: 2-source data-ALU then full-VL svst1 + asm ldr z "
             "forwarding at random data-dependent addresses (port of mem_disambig_alu)")
    .test_init = mem_disambig_alu_sve_init,
    .test_run = mem_disambig_alu_sve_run,
    .test_cleanup = mem_disambig_alu_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
