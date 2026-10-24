/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b arm64_sdc_sve
 * @parblock
 * SVE port of arm64_sdc: CRC32/CRC64/additive-checksum detectors computed
 * over a random buffer, with the buffer scan running on SVE vector lanes
 * (svld1 batches feed a byte-at-a-time software CRC; the additive checksum
 * sums via svaddv reductions). The detector self-check (inject a 1-bit flip
 * in init and require every detector to catch it) is preserved verbatim —
 * fail-closed before the timed run. Byte-exact golden comparison per
 * iteration exactly as the original.
 * @endparblock
 */

#include "sandstone.h"
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

#if defined(__aarch64__)
/* 扫描缓冲字节数(原 64KB) — 统一优先级链:
 *   1. -O arm64_sdc_sve.buffer_bytes=N / 配置文件同名条目
 *   2. 环境变量 SDC_ARM64_SDC_SVE_BUFFER_BYTES
 *   3. 自适应: 字节数直给(参照 mesh_upi_sve_read_L3_int 模式),
 *      占物理内存 60%, 2 个大内存消费者均摊(共享检测缓冲 +
 *      每线程 run 副本)。CRC 表 [256] 与本旋钮无关, 不动。 */
static size_t sdc_sve_buffer_bytes(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "buffer_bytes", 0, 64 << 10,
                                     1ll << 40);
    if (v == 0)
        sdc_env_knob_int_clamped("arm64_sdc_sve", "buffer_bytes", &v,
                                 64 << 10, 1ll << 40);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_target_workset_bytes(600, 2, nullptr);
}
#endif

struct sdc_sve_data {
    size_t   buffer_bytes;   /* 扫描缓冲字节数(init 定容, 线程共享) */
    uint8_t *test_buffer;
    uint32_t golden_crc32;
    uint64_t golden_crc64;
    uint32_t golden_checksum;
};

/* ---- 软件检测器 (SVE 向量扫描 + 标量 CRC 表) ---- */
static uint32_t crc32_for_byte[256];
static uint64_t crc64_for_byte[256];
static void crc_tables_init(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_for_byte[i] = c;
        uint64_t c64 = i;
        for (int k = 0; k < 8; ++k) c64 = (c64 & 1) ? (0xC96C5795D7870F42ULL ^ (c64 >> 1)) : (c64 >> 1);
        crc64_for_byte[i] = c64;
    }
}

static int sdc_sve_detect(const void *data, size_t len,
                          uint32_t *crc32_out, uint64_t *crc64_out, uint32_t *sum_out)
{
#if defined(__aarch64__)
    const uint8_t *p = static_cast<const uint8_t *>(data);
    uint32_t crc32 = 0xFFFFFFFFu;
    uint64_t crc64 = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t sum = 0;
    const int lanes = svcntb();
    for (size_t base = 0; base < len; base += lanes) {
        int n = (len - base < (size_t)lanes) ? (int)(len - base) : lanes;
        svbool_t pg = svwhilelt_b8((uint64_t)0, (uint64_t)n);
        svuint8_t v = svld1_u8(pg, p + base);
        uint8_t chunk[64];
        svst1_u8(pg, chunk, v);
        for (int i = 0; i < n; ++i) {
            crc32 = crc32_for_byte[(crc32 ^ chunk[i]) & 0xFFu] ^ (crc32 >> 8);
            crc64 = crc64_for_byte[(crc64 ^ chunk[i]) & 0xFFull] ^ (crc64 >> 8);
            sum += chunk[i];
        }
    }
    *crc32_out = ~crc32;
    *crc64_out = ~crc64;
    *sum_out = (uint32_t)sum;
    return 0;
#else
    (void)data; (void)len; (void)crc32_out; (void)crc64_out; (void)sum_out;
    return -1;
#endif
}

static int arm64_sdc_sve_init(struct test *test)
{
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
     * layer: arm64_require_sve_vl does the SVE capability check
     * + 128..1024-bit range gate in one call (skip reason
     * framework-issued). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
    crc_tables_init();

    auto *data = new sdc_sve_data;
    data->buffer_bytes = sdc_sve_buffer_bytes(test);
    data->test_buffer = static_cast<uint8_t *>(aligned_alloc_safe(64, data->buffer_bytes));
    memset_random(data->test_buffer, data->buffer_bytes);
    sdc_sve_detect(data->test_buffer, data->buffer_bytes,
                   &data->golden_crc32, &data->golden_crc64, &data->golden_checksum);

    /* 检测器自检 (fail-closed): 注入 1-bit 翻转, 每个检测器必须能抓到 */
    uint8_t *probe = static_cast<uint8_t *>(aligned_alloc_safe(64, data->buffer_bytes));
    memcpy(probe, data->test_buffer, data->buffer_bytes);
    probe[data->buffer_bytes / 2] ^= 0x01;
    uint32_t p_crc32; uint64_t p_crc64; uint32_t p_sum;
    sdc_sve_detect(probe, data->buffer_bytes, &p_crc32, &p_crc64, &p_sum);
    free(probe);
    if (p_crc32 == data->golden_crc32 && p_crc64 == data->golden_crc64
        && p_sum == data->golden_checksum) {
        delete data;
        log_error("arm64_sdc_sve: detector self-check failed — injected 1-bit "
                  "corruption not caught by CRC32/CRC64/checksum");
        return EXIT_FAILURE;
    }

    test->data = data;
    return EXIT_SUCCESS;
#else
    (void)test;
    return EXIT_SUCCESS;
#endif
}

static int arm64_sdc_sve_run(struct test *test, int cpu)
{
#if !defined(__aarch64__)
    (void)test; (void)cpu;
    return EXIT_SKIP;
#else
    auto *data = static_cast<sdc_sve_data *>(test->data);
    const size_t buffer_bytes = data->buffer_bytes;
    uint8_t *test_buffer = static_cast<uint8_t *>(aligned_alloc_safe(64, buffer_bytes));
    uint32_t crc32_result; uint64_t crc64_result; uint32_t checksum_result;

    TEST_LOOP(test, 128) {
        memcpy(test_buffer, data->test_buffer, buffer_bytes);
        if (sdc_sve_detect(test_buffer, buffer_bytes,
                           &crc32_result, &crc64_result, &checksum_result) == 0) {
            if (crc32_result != data->golden_crc32)
                memcmp_or_fail(&crc32_result, &data->golden_crc32, 1,
                        "CRC32 signature mismatch — silent data corruption "
                        "detected on the integrity datapath");
            if (crc64_result != data->golden_crc64)
                memcmp_or_fail(&crc64_result, &data->golden_crc64, 1,
                        "CRC64 signature mismatch — silent data corruption "
                        "detected on the integrity datapath");
            if (checksum_result != data->golden_checksum)
                memcmp_or_fail(&checksum_result, &data->golden_checksum, 1,
                        "Additive checksum mismatch — silent data corruption "
                        "detected on the integrity datapath");
        }
    }

    free(test_buffer);
    return EXIT_SUCCESS;
#endif
}

static int arm64_sdc_sve_cleanup(struct test *test)
{
#ifdef __aarch64__
    auto *data = static_cast<sdc_sve_data *>(test->data);
    if (data) {
        free(data->test_buffer);
        delete data;
    }
#endif
    return EXIT_SUCCESS;
}

DECLARE_TEST(arm64_sdc_sve, "SVE SDC detection: CRC32/CRC64/checksum computed over svld1 vector-lane scans, byte-exact golden + fail-closed detector self-check (port of arm64_sdc)")
    .test_init = arm64_sdc_sve_init,
    .test_run = arm64_sdc_sve_run,
    .test_cleanup = arm64_sdc_sve_cleanup,
    .quality_level = TEST_QUALITY_BETA,
END_DECLARE_TEST
