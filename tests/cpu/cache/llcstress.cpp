/*  llcstress — high-frequency random LLC access with store->load
 *  verification (633 #33 lineage, user spec 2026-11-02).
 *
 *  Workload: a 64 MB shared window (larger than any LLC on this class
 *  of part) hammered with random-offset qword store/load pairs — the
 *  random offsets keep LLC hit rates low and the replacement policy
 *  under pressure; the data is fresh random64 every round ("dynamic
 *  random data").
 *
 *  Detection-surface honesty (2026-11-17 L3 audit): on the Kunpeng
 *  920 the 64 MB window exceeds the per-cluster L3 slice (24 MB), so
 *  the LLC replacement pressure is real — but the verification pair
 *  (store then IMMEDIATE load-back of the same qword) is served by
 *  the store-buffer/L1 forwarding path and does not reach L2/L3.
 *  This test stresses LLC traffic and verifies the LSU forwarding
 *  path; L3-resident data-integrity verification is carried by the
 *  mesh read_L3 family (sum/golden over a shared 4 MB array).
 *
 *  FALSE-POSITIVE FIX over the user's draft (measured reasoning): the
 *  draft let ALL threads randomly write ANY qword in the shared 64 MB
 *  and verified read-back == just-written value.  With ~48 writers and
 *  a nanosecond-scale write->read window, cross-thread same-slot
 *  collisions are a LEGAL data race read as a mismatch — order of
 *  several spurious FAILs per second.  This variant INTERLEAVES the
 *  slots: thread i owns every qword slot congruent to i (mod 1024),
 *  i.e. one qword per 8 KB stride.  Each thread still roams the full
 *  64 MB (LLC pressure preserved), adjacent cachelines are still
 *  shared across threads (false-sharing cadence — excitation value),
 *  but no two threads ever write the same qword: the read-back check
 *  is now race-free and a mismatch is a genuine store/load-path SDC.
 *
 *  Adaptations to repo conventions (same set as the _2 pair): framework
 *  random64() instead of std::mt19937/random_device; per-iteration
 *  fprintf log block removed (hot path carries no I/O); the volatile
 *  local store_buf/reload "consistency" pass removed (a stack-local
 *  volatile reload tests no real path); the per-iteration
 *  __sync_synchronize() barriers removed (same-thread read-after-write
 *  needs no barrier, and a barrier never fixed the race above);
 *  .fracture_loop_count = -1; _cleanup naming; TestData wrapped in an
 *  anonymous namespace (same-name struct ODR hazard).
 */
#include "sandstone.h"
#include <cstdint>
#include <vector>

static constexpr size_t LLS_MEM_SIZE  = 64ULL * 1024 * 1024;  /* 64 MB */
static constexpr size_t LLS_STRIDE_QW = 1024;   /* qwords per interleave band */

namespace { /* ODR: per-TU LLSData */
struct LLSData {
    std::vector<uint8_t> mem;   /* shared window; slots are per-thread by construction */
};
}

static int llcstress_init(struct test *test) {
    auto *td = new LLSData;
    if (!td) return EXIT_FAILURE;
    td->mem.resize(LLS_MEM_SIZE);
    test->data = td;
    return EXIT_SUCCESS;
}

static int llcstress_cleanup(struct test *test) {
    auto *td = static_cast<LLSData *>(test->data);
    delete td;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int llcstress_run(struct test *test, int cpu) {
    auto *td = static_cast<LLSData *>(test->data);

    const size_t bands = LLS_MEM_SIZE / 8 / LLS_STRIDE_QW;
    uint64_t local_iterations = 0;

    do {
        /* random band; our qword slot inside it = band*1024 + cpu —
         * full-window roaming, zero same-slot races across threads */
        size_t band = random64() % bands;
        size_t offset = (band * LLS_STRIDE_QW + (size_t)cpu) * 8;

        uint64_t write_val = random64();
        uint64_t *ptr = reinterpret_cast<uint64_t *>(td->mem.data() + offset);

        /* the stress pair: store, then load back and verify */
        *ptr = write_val;
        uint64_t read_val = *ptr;

        if (read_val != write_val) {
            report_fail_msg("llcstress: read-back mismatch at iteration "
                            "%lu (band %zu, offset 0x%zx): wrote "
                            "0x%016lx read 0x%016lx",
                            (unsigned long)local_iterations, band, offset,
                            (unsigned long)write_val,
                            (unsigned long)read_val);
            return EXIT_FAILURE;
        }

        ++local_iterations;
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(llcstress, "High-frequency random LLC access over a 64MB window (interleaved per-thread qword slots, race-free read-back verification, dynamic random data) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = llcstress_init,
    .test_run = llcstress_run,
    .test_cleanup = llcstress_cleanup,
    .fracture_loop_count = -1,   /* never fracture: fast semantic rounds */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
