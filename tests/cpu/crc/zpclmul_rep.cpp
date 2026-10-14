/**
 * @copyright
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b zpclmul_rep
 * @parblock
 * Zlib PCLMUL CRC32 folding + rep movsb (ARM64 NEON PMULL version)
 *
 * This test combines a bulk memory copy (the ARM64 analogue of x86
 * rep movsb) with NEON PMULL CRC-32 folding: the data is first copied
 * into a separate buffer, then the hardware path folds the IEEE 802.3
 * CRC-32 (reflected polynomial 0xEDB88320) over the *copy* using
 * PMULL/PMULL2 carry-less multiplication (4-accumulator main loop,
 * 3-level fold-down, partial-vector tail fold, Barrett reduction -
 * structure after libdeflate's crc32_arm_pmullx4). The golden is
 * zlib's independent byte-sliced software crc32() over the original
 * data, so both the copy and the folding are validated by one
 * comparison. A known-answer self-check gates the random loop.
 * Input data is random bytes with a random length in [16, BLOCK_SIZE].
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstring>
#include <zlib.h>

#include "crc32_pmull_fold.h"

#ifdef __aarch64__

static constexpr size_t BLOCK_SIZE = 4096;

/*
 * DUT: bulk copy (rep-movsb analogue) followed by NEON PMULL CRC-32
 * folding over the *copy*, zlib-API semantics. A corruption anywhere
 * in the copy or the folding shows up as a mismatch against the
 * zlib golden computed over the original data.
 */
static uint32_t crc32_hardware(const uint8_t *data, size_t len, uint32_t crc)
{
    alignas(16) uint8_t copy_buf[BLOCK_SIZE];

    memcpy(copy_buf, data, len);   /* the ARM64 analogue of REP MOVS */
    /* zlib API <-> raw state domain: result = ~raw(~crc) */
    return ~crc32_pmull(~crc, copy_buf, len);
}

/* Golden: zlib software CRC-32 (byte-sliced tables, no NEON) over the
 * original data */
static uint32_t crc32_software(const uint8_t *data, size_t len, uint32_t crc)
{
    return crc32(crc, data, len);
}

/* Known-answer self-check gating the random loop: the standard
 * "123456789" -> 0xCBF43926 vector exercises the len < 16 branch, and
 * "123456789" x15 (135 bytes, 135 % 16 = 7 tail) exercises the 4-
 * accumulator main loop plus the partial-vector tail fold. Both go
 * through the copy+folding path and are cross-checked against the
 * zlib golden. */
static bool kat_selfcheck()
{
    static const char kat9[] = "123456789";
    static const size_t kat9_len = 9;
    uint8_t kat135[135];

    for (int i = 0; i < 15; i++)
        memcpy(kat135 + i * 9, kat9, kat9_len);

    if (crc32_hardware(reinterpret_cast<const uint8_t *>(kat9), kat9_len, 0)
            != 0xCBF43926)
        return false;
    for (size_t len : { kat9_len, sizeof(kat135) }) {
        const uint8_t *p = (len == kat9_len)
                        ? reinterpret_cast<const uint8_t *>(kat9) : kat135;
        if (crc32_hardware(p, len, 0) != crc32_software(p, len, 0))
            return false;
    }
    return true;
}

static int zpclmul_rep_run(struct test *test, int cpu) {
    (void)cpu;

    if (!kat_selfcheck()) {
        report_fail_msg("zpclmul_rep: known-answer self-check failed");
        return EXIT_FAILURE;
    }

    alignas(16) uint8_t input[BLOCK_SIZE];
    alignas(16) uint8_t store_buf[BLOCK_SIZE];
    alignas(16) uint8_t reload_buf[BLOCK_SIZE];

    do {
        /* Random length >= 16 keeps both folding branches and the tail
         * fold reachable; the len < 16 branch is covered by the KAT.
         * The framework RNG (random64/random32) is per-thread and
         * reproducible via --rng-state. */
        size_t len = 16 + random64() % (BLOCK_SIZE - 15);
        for (size_t i = 0; i < len; ++i)
            input[i] = (uint8_t)random32();

        uint32_t hw_crc = crc32_hardware(input, len, 0);
        uint32_t sw_crc = crc32_software(input, len, 0);
        bool crc_ok = (hw_crc == sw_crc);

        memcpy(store_buf, input, len);
        memcpy(reload_buf, store_buf, len);
        bool consistent = (memcmp(input, reload_buf, len) == 0);

        if (!(crc_ok && consistent)) {
            report_fail_msg("zpclmul_rep: crc_ok=%d, consistent=%d, len=%zu, "
                            "hw_crc=0x%08X, sw_crc=0x%08X",
                            crc_ok, consistent, len, hw_crc, sw_crc);
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

#else /* !__aarch64__ */

static int zpclmul_rep_run(struct test *test, int cpu) {
    (void)cpu;
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM64 NEON PMULL required");
    return EXIT_SKIP;
}

#endif /* __aarch64__ */

static int zpclmul_rep_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int zpclmul_rep_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(zpclmul_rep, "Zlib PCLMUL CRC32 folding + rep movsb (ARM64 NEON PMULL)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = zpclmul_rep_init,
    .test_run = zpclmul_rep_run,
    .test_cleanup = zpclmul_rep_finish,
    .minimum_cpu = ZPCLMUL_MIN_CPU,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
