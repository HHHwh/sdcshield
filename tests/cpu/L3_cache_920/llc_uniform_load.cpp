/**
 * @file
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test llc_uniform_load
 * @parblock
 * Stress LLC with persistent uniform strided loads (633 #32 lineage,
 * user spec 2026-11-02).
 *
 * The uniform-strided sibling of llc_randomized_load: a fixed
 * 128-byte stride walks two per-thread windows sized to the PROBED
 * per-cluster L3 slice (device_info; sysfs-verified 24 MB on Kunpeng
 * 920 — L3 is distributed 24 MB per 24-core cluster), so the stride
 * trains the hardware prefetcher at the cache capacity boundary
 * (the 633 #32 behaviour), and the walk start rotates by one
 * element per full pass for pattern diversity.
 *
 *   - data[]: strided LOADS only (memset_random-filled once) — the
 *     pure strided-load pressure;
 *   - check[]: the store->load verification path — the loaded value
 *     is stored at the same index and read back THROUGH INLINE ASM
 *     LDR, then bit-compared.
 *
 * WHY THE ASM LOAD (same measured reasoning as
 * llc_randomized_load): the draft's plain-C++ read-back
 * (check[idx] = v; loaded = check[idx];) is forwarded through the
 * register file by GCC -O2 and the verification folds to constant
 * true.  The volatile asm ldr really traverses the LSU's
 * store-buffer forwarding path — the genuine detection surface, and
 * one the prefetcher cannot mask (forwarding happens before any
 * prefetch interaction).
 *
 * Data is bit-pattern random (memset_random) — wider than the
 * draft's uniform-random floats; comparison is bitwise.  Per-thread
 * buffers: no cross-thread races.  Adaptations vs the draft (same
 * set as the family): framework RNG, no per-fail fprintf,
 * .fracture_loop_count = -1, _cleanup naming.
 * @endparblock
 */

#include "sandstone.h"
#include "sandstone_p.h"   /* device_count/device_info: L3 size probe */
#include <cstdint>
#include <cstring>
#include <vector>

/* Per-cluster L3 size in bytes — same probe and same init-time
 * discipline as llc_randomized_load (device_info -> sysconf -> 8 MB
 * fallback; MUST be called from test_init, see the sibling's comment
 * and the memcpy H8' lesson). */
static size_t llul_l3_size(void) {
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

/* Per-thread window = the probed slice (capacity-boundary stride
 * walk); the old 32 MiB constant degenerated into DRAM streaming on
 * the 920's distributed L3 — see llc_randomized_load's geometry
 * comment for the arithmetic. */
static constexpr size_t LLUL_STRIDE = 128 / sizeof(uint32_t);  /* 128B stride */

/* anti-forwarding load: really goes to the LSU, not the register file */
static inline uint32_t llul_ldr(const uint32_t *addr) {
    uint32_t v;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static int llc_uniform_load_init(struct test *test) {
    /* probe HERE — topology is filled by now (see llul_l3_size) */
    /* throwing-new OOM follows the repo T10 convention (cf.
     * gemm_cdouble_dynamic_square): bad_alloc -> clean SKIP. */
    try {
        test->data = new size_t(llul_l3_size());
    } catch (const std::bad_alloc &) {
        log_skip(RuntimeSkipCategory, "memory: %s l3-size word", test->id);
        return EXIT_SKIP;
    }
    return EXIT_SUCCESS;
}

static int llc_uniform_load_cleanup(struct test *test) {
    delete static_cast<size_t *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int llc_uniform_load_run(struct test *test, int cpu) {
    (void)cpu;

    const size_t buffer_bytes = *static_cast<size_t *>(test->data);
    const size_t num_words = buffer_bytes / sizeof(uint32_t);

    std::vector<uint32_t> data(num_words);    /* strided load source   */
    std::vector<uint32_t> golden(num_words);  /* element-exact golden copy */
    std::vector<uint32_t> check(num_words);   /* store->load window    */

    memset_random(data.data(), buffer_bytes);
    memcpy(golden.data(), data.data(), buffer_bytes);
    memset(check.data(), 0, buffer_bytes);

    /* random walk start; rotates by one element each full pass */
    size_t offset = random64() % LLUL_STRIDE;

    do {
        for (size_t idx = offset; idx < num_words; idx += LLUL_STRIDE) {
            /* uniform strided load — trains/stresses the prefetcher */
            uint32_t original = data[idx];

            /* store -> REAL load-back through the LSU forwarding path
             * (secondary surface, kept) */
            check[idx] = original;
            uint32_t loaded = llul_ldr(&check[idx]);

            if (memcmp(&original, &loaded, sizeof(uint32_t)) != 0) {
                report_fail_msg("llc_uniform_load: store->load "
                                "consistency failure at idx %zu: stored "
                                "0x%08x loaded 0x%08x (LSU forwarding "
                                "path under strided access)",
                                idx, original, loaded);
                return EXIT_FAILURE;
            }
        }
        offset = (offset + 1) % LLUL_STRIDE;

        /* L3 verification sweep (2026-11-18 element-exact rework): the
         * stride pass covers only 1/STRIDE of the window; this DENSE
         * pass re-reads every word and compares ELEMENT-EXACT against
         * the init-time golden copy — window-sized traffic evicts the
         * private L2, so the compare is L3/DRAM-served. Element-exact
         * (not a sum): permutation/swap-class L3 corruption changes the
         * word at its index and FAILS here, where a sum-only check is
         * blind to order-preserving compensating damage. */
        for (size_t i = 0; i < num_words; ++i) {
            if (data[i] != golden[i]) {
                report_fail_msg("llc_uniform_load: golden sweep mismatch "
                                "at idx %zu: golden 0x%08x got 0x%08x "
                                "(L3-resident data corruption, "
                                "element-exact compare)",
                                i, golden[i], data[i]);
                return EXIT_FAILURE;
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(llc_uniform_load, "Stress LLC with persistent uniform 128B-strided loads over per-thread windows sized to the probed per-cluster L3; store->asm-ldr load-back on the LSU forwarding path PLUS per-pass dense element-exact golden sweep of the whole window (L3-path verification, 2026-11-18 rework) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = llc_uniform_load_init,
    .test_run = llc_uniform_load_run,
    .test_cleanup = llc_uniform_load_cleanup,
    .fracture_loop_count = -1,   /* never fracture: fast semantic rounds */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
