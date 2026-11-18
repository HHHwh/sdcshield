#include "sandstone.h"

#ifdef __aarch64__
#include <arm_neon.h>
#include <cstdlib>
#endif

#define NEON_ADD_ELEMENTS (1u << 10)
#define NEON_ADD_BUF_SIZE (NEON_ADD_ELEMENTS * sizeof(uint32_t))

struct neon_add_t_ {
        uint32_t *a;
        uint32_t *b;
        uint32_t *golden;
};
typedef struct neon_add_t_ neon_add_t;

#ifdef __aarch64__
static void prv_do_add(const uint32_t *a, const uint32_t *b, uint32_t *res)
{
        for (size_t i = 0; i < NEON_ADD_ELEMENTS / 4; i++) {
                uint32x4_t r1 = vld1q_u32(&a[i*4]);
                uint32x4_t r2 = vld1q_u32(&b[i*4]);
                uint32x4_t r3 = vaddq_u32(r1, r2);
                vst1q_u32(&res[i*4], r3);
        }
}

/* golden (issue #373): scalar independent recomputation — pure C on the
 * scalar pipeline, never the NEON units under test (prv_do_add stays the
 * run-side payload; a persistent NEON add fault used to corrupt golden
 * and actual identically). The plain elementwise add is the canonical
 * auto-vectorization idiom, so the helper carries no-tree-vectorize —
 * pinned mechanically by scripts/check-scalar-golden.sh. */
#pragma GCC push_options
#pragma GCC optimize("no-tree-vectorize")
static void prv_scalar_golden_add(const uint32_t *a, const uint32_t *b, uint32_t *res)
{
        for (size_t i = 0; i < NEON_ADD_ELEMENTS; i++)
                res[i] = a[i] + b[i];
}
#pragma GCC pop_options
#endif

static int neon_add_init(struct test *test)
{
#ifdef __aarch64__
        neon_add_t *na = static_cast<neon_add_t *>(malloc(sizeof(*na)));

        na->a = static_cast<uint32_t *>(aligned_alloc(64, NEON_ADD_BUF_SIZE));
        na->b = static_cast<uint32_t *>(aligned_alloc(64, NEON_ADD_BUF_SIZE));
        na->golden = static_cast<uint32_t *>(aligned_alloc_safe(64, NEON_ADD_BUF_SIZE));

        memset_random(na->a, NEON_ADD_BUF_SIZE);
        memset_random(na->b, NEON_ADD_BUF_SIZE);
        prv_scalar_golden_add(na->a, na->b, na->golden);

        test->data = na;

        return EXIT_SUCCESS;
#else
        return EXIT_SUCCESS;
#endif
}

static int neon_add_run(struct test *test, int cpu)
{
#ifdef __aarch64__
        neon_add_t *na = static_cast<neon_add_t *>(test->data);
        uint32_t *res = static_cast<uint32_t *>(aligned_alloc(64, NEON_ADD_BUF_SIZE));

        TEST_LOOP(test, 1 << 13) {
                memset(res, 0, NEON_ADD_BUF_SIZE);
                prv_do_add(na->a, na->b, res);
                memcmp_or_fail(res, na->golden, NEON_ADD_ELEMENTS);
        }

        free(res);

        return EXIT_SUCCESS;
#else
        return EXIT_SUCCESS;
#endif
}

static int neon_add_cleanup(struct test *test)
{
#ifdef __aarch64__
        neon_add_t *na = static_cast<neon_add_t *>(test->data);

        if (na) {
                free(na->golden);
                free(na->b);
                free(na->a);
                free(na);
        }
#endif

        return EXIT_SUCCESS;
}

DECLARE_TEST(neon_add, "Repeatedly add arrays of unsigned integers using ARM NEON instructions")
        .test_init = neon_add_init,
        .test_run = neon_add_run,
        .test_cleanup = neon_add_cleanup,
        .quality_level = TEST_QUALITY_BETA,
END_DECLARE_TEST
