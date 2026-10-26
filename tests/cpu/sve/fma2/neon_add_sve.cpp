/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_add_sve
 * @parblock
 * SVE port of neon_add: repeated array addition on SVE vector lanes
 * (svld1/svadd/svst1) instead of NEON vaddq. Golden precomputed in init
 * on the same SVE units (same pattern as the original); every run
 * recomputes and memcmp-compares.
 * The element count (originally SVE_ADD_ELEMENTS=1024) is runtime
 * configurable via -O neon_add_sve.count=N / SDC_NEON_ADD_SVE_COUNT,
 * with a memory-adaptive default when unconfigured.
 * @endparblock
 */

#include "sandstone.h"
#include "sdc_stress_config.h"

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#include <cstdlib>
#endif

#define SVE_ADD_ELEMENTS (1u << 10)

struct neon_add_sve_t_ {
        uint32_t *a;
        uint32_t *b;
        uint32_t *golden;
        size_t count;
};
typedef struct neon_add_sve_t_ neon_add_sve_t;

#ifdef __aarch64__
/* 元素数 SVE_ADD_ELEMENTS(1024) — 运行时可配置, 统一优先级链:
 *   1. -O neon_add_sve.count=N / 配置文件同名条目
 *   2. 环境变量 SDC_NEON_ADD_SVE_COUNT
 *   3. 自适应: a/b/golden(共享)+每线程 res, 4×4B/元素,
 *      占物理内存 60%, clamp [1024, 64M]。原 1024 为下限。 */
static size_t neon_add_sve_count(struct test *test)
{
        int64_t v = sdc_knob_int_clamped(test, "count", 0, SVE_ADD_ELEMENTS, 64 << 20);
        if (v == 0)
                sdc_env_knob_int_clamped("neon_add_sve", "count", &v, SVE_ADD_ELEMENTS, 64 << 20);
        if (v > 0)
                return (size_t)v;
        return (size_t)sdc_auto_linear_count(4, 4, 1.0, 600, thread_count(),
                                             SVE_ADD_ELEMENTS, 64 << 20, test->id, "count");
}

static void prv_do_add(const uint32_t *a, const uint32_t *b, uint32_t *res, size_t n)
{
        /* VL 无关: svcntw() 分批 + whilelt 尾部 */
        for (size_t base = 0; base < n; base += svcntw()) {
                svbool_t pg = svwhilelt_b32((uint64_t)base, (uint64_t)n);
                svuint32_t r1 = svld1_u32(pg, a + base);
                svuint32_t r2 = svld1_u32(pg, b + base);
                svst1_u32(pg, res + base, svadd_u32_x(pg, r1, r2));
        }
}
#endif

static int neon_add_sve_init(struct test *test)
{
#ifdef __aarch64__
        /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
         * layer: arm64_require_sve_vl does the SVE capability check
         * + 128..1024-bit range gate in one call (skip reason
         * framework-issued). */
        const long svl = arm64_require_sve_vl(test, 128, 1024);
        if (svl < 0)
                return EXIT_SKIP;

        neon_add_sve_t *na = static_cast<neon_add_sve_t *>(malloc(sizeof(*na)));
        const size_t count = neon_add_sve_count(test);
        const size_t buf_size = count * sizeof(uint32_t);
        na->count = count;

        /* aligned_alloc_safe: 运行时 count*4 不保证 64 的倍数, 向上取整保对齐 */
        na->a = static_cast<uint32_t *>(aligned_alloc_safe(64, buf_size));
        na->b = static_cast<uint32_t *>(aligned_alloc_safe(64, buf_size));
        na->golden = static_cast<uint32_t *>(aligned_alloc_safe(64, buf_size));

        memset_random(na->a, buf_size);
        memset_random(na->b, buf_size);
        prv_do_add(na->a, na->b, na->golden, count);

        test->data = na;

        return EXIT_SUCCESS;
#else
        return EXIT_SUCCESS;
#endif
}

static int neon_add_sve_run(struct test *test, int cpu)
{
#ifdef __aarch64__
        neon_add_sve_t *na = static_cast<neon_add_sve_t *>(test->data);
        const size_t count = na->count;
        const size_t buf_size = count * sizeof(uint32_t);
        uint32_t *res = static_cast<uint32_t *>(aligned_alloc_safe(64, buf_size));

        TEST_LOOP(test, 1 << 13) {
                memset(res, 0, buf_size);
                prv_do_add(na->a, na->b, res, count);
                memcmp_or_fail(res, na->golden, count);
        }

        free(res);

        return EXIT_SUCCESS;
#else
        return EXIT_SUCCESS;
#endif
}

static int neon_add_sve_cleanup(struct test *test)
{
#ifdef __aarch64__
        neon_add_sve_t *na = static_cast<neon_add_sve_t *>(test->data);

        if (na) {
                free(na->golden);
                free(na->b);
                free(na->a);
                free(na);
        }
#endif

        return EXIT_SUCCESS;
}

DECLARE_TEST(neon_add_sve, "Repeatedly add arrays of unsigned integers using ARM SVE instructions (port of neon_add)")
        .test_init = neon_add_sve_init,
        .test_run = neon_add_sve_run,
        .test_cleanup = neon_add_sve_cleanup,
        .quality_level = TEST_QUALITY_BETA,
END_DECLARE_TEST
