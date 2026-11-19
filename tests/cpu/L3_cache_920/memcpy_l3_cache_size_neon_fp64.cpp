/*  memcpy_l3_cache_size_neon_fp64 — FP64 datapath variant of
 *  tests/cpu/L3_cache_920/memcpy_l3_cache_size.cpp (2026-11-18
 *  NEON/FP64 expansion completion, user directive).
 *
 *  Everything (probe, 1.5xL3 window, random block L3/8..1xL3, random
 *  64B-aligned offsets, destination poisoning, memcmp full re-read
 *  verification) identical to the parent — only the copy loop runs
 *  on the f64x2 register class (vld1q_f64/vst1q_f64), covering the
 *  FP load/store ports on the L3-scale copy path.  Block lengths are
 *  64B multiples, so the f64x2 loop covers them with no tail.
 */
#include <sandstone.h>
#include "sandstone_p.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <arm_neon.h>

static size_t get_l3_size() {
    int ndev = device_count();
    for (int i = 0; i < ndev; ++i) {
        int sz = device_info[i].cache[2].cache_data;   /* cache[2] = L3 */
        if (sz > 0)
            return static_cast<size_t>(sz);
    }
    long size = sysconf(_SC_LEVEL3_CACHE_SIZE);
    if (size > 0) return static_cast<size_t>(size);
    return 8 * 1024 * 1024;
}

static int memcpy_l3_cache_size_neon_fp64_init(struct test *test) {
    /* probe HERE — topology is filled by now (memcpy H8' lesson) */
    /* throwing-new OOM follows the repo T10 convention (cf.
     * gemm_cdouble_dynamic_square): bad_alloc -> clean SKIP. */
    try {
        test->data = new size_t(get_l3_size());
    } catch (const std::bad_alloc &) {
        log_skip(RuntimeSkipCategory, "memory: %s l3-size word", test->id);
        return EXIT_SKIP;
    }
    return EXIT_SUCCESS;
}

static int memcpy_l3_cache_size_neon_fp64_run(struct test *test, int cpu) {
    (void)cpu;
    const size_t BLOCK_SIZE = *static_cast<size_t *>(test->data);

    const size_t window = BLOCK_SIZE + (BLOCK_SIZE / 2);
    std::vector<uint8_t> src(window);
    std::vector<uint8_t> dst(window);

    do {
        size_t max_len = BLOCK_SIZE;
        size_t len = 64 * (1 + random32() % (max_len / 64));   /* 64B multiple */
        size_t src_off = 64 * (random32() % ((window - len) / 64 + 1));
        size_t dst_off = 64 * (random32() % ((window - len) / 64 + 1));

        memset_random(src.data() + src_off, len);
        memset(dst.data() + dst_off, 0x5A, len);   /* poison: lazy copy fails */

        /* f64x2 copy loop — the FP register class on the L3 path */
        const double *s = reinterpret_cast<const double *>(src.data() + src_off);
        double *d = reinterpret_cast<double *>(dst.data() + dst_off);
        for (size_t i = 0; i < len / 16; ++i) {
            float64x2_t v = vld1q_f64(s + i * 2);
            vst1q_f64(d + i * 2, v);
        }

        bool data_ok = (memcmp(dst.data() + dst_off, src.data() + src_off, len) == 0);

        if (!data_ok) {
            report_fail_msg("memcpy_l3_cache_size_neon_fp64: copy mismatch "
                            "(len %zu, src_off %zu, dst_off %zu, L3 block %zu)",
                            len, src_off, dst_off, BLOCK_SIZE);
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int memcpy_l3_cache_size_neon_fp64_finish(struct test *test) {
    delete static_cast<size_t *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

DECLARE_TEST(memcpy_l3_cache_size_neon_fp64, "FP64 variant of memcpy_l3_cache_size: L3-sized block copy on the f64x2 register class (vld1q_f64/vst1q_f64), random data, random offsets, memcmp full re-read verification (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = memcpy_l3_cache_size_neon_fp64_init,
    .test_run = memcpy_l3_cache_size_neon_fp64_run,
    .test_cleanup = memcpy_l3_cache_size_neon_fp64_finish,
    .fracture_loop_count = -1,   /* never fracture: L3-scale rounds */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
