/*  mem_disambiguation_neon — NEON variant of tests/cpu/memory/
 *  mem_disambiguation.cpp.
 *
 *  Original: three store-buffer forwarding patterns on the SCALAR
 *  qword path (asm ldr/str): full-width forward, partial overlap
 *  forward, repeated-store training.  This variant replays the same
 *  three patterns through the 16-byte NEON datapath (vst1q/vld1q),
 *  exercising the vector LSU's store-to-load forwarding and its
 *  partial-overlap windows — a detection surface the scalar original
 *  does not touch (complementary pair, both kept).
 *
 *    Pattern 1: vst1q_u64_x2 (16B) then vld1q_u64_x2 (16B) same
 *               address — NEON full-width forward.
 *    Pattern 2: 16B vector store, then an 8-byte partial load from
 *               offset+4 (inside the stored window) — NEON partial
 *               forwarding logic.
 *    Pattern 3: three repeated 16B stores to one address, then the
 *               load — store-buffer predictor training on the NEON
 *               path.
 *
 *  512 patterns per sweep, offsets 16B-aligned random, values random
 *  per init.  Original untouched; x86-64 untouched (aarch64-only).
 */
#include "sandstone.h"
#include <cstdint>
#include <cstdlib>
#include <arm_neon.h>

#define MDN_BUFFER_SIZE   4096
#define MDN_PATTERN_COUNT 512

namespace { /* ODR: per-TU MDNData */
struct MDNData {
    uint64_t *offsets;   /* 16B-aligned, in [0, SIZE-16) */
    uint64_t *values;    /* random qwords; vector value = {values[2i], values[2i+1]} */
};
}

static int mem_disambiguation_neon_init(struct test *test) {
    auto *d = new MDNData;
    if (!d) return EXIT_FAILURE;
    d->offsets = static_cast<uint64_t *>(malloc(MDN_PATTERN_COUNT * sizeof(uint64_t)));
    d->values  = static_cast<uint64_t *>(malloc(MDN_PATTERN_COUNT * sizeof(uint64_t)));
    if (!d->offsets || !d->values) return EXIT_FAILURE;
    for (int i = 0; i < MDN_PATTERN_COUNT; ++i) {
        d->offsets[i] = (random64() % (MDN_BUFFER_SIZE - 16)) & ~15ULL;
        d->values[i]  = random64();
    }
    test->data = d;
    return EXIT_SUCCESS;
}

static int mem_disambiguation_neon_cleanup(struct test *test) {
    auto *d = static_cast<MDNData *>(test->data);
    free(d->offsets);
    free(d->values);
    delete d;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int mem_disambiguation_neon_run(struct test *test, int cpu) {
    (void)cpu;
    auto *d = static_cast<MDNData *>(test->data);
    uint8_t *buffer = static_cast<uint8_t *>(aligned_alloc(64, MDN_BUFFER_SIZE));

    do {
        /* Pattern 1 — full-width NEON forward */
        for (int i = 0; i < MDN_PATTERN_COUNT; i += 2) {
            uint64_t off = d->offsets[i];
            uint64x2_t value = vld1q_u64(&d->values[i]);
            vst1q_u64(reinterpret_cast<uint64_t *>(buffer + off), value);
            uint64x2_t loaded = vld1q_u64(reinterpret_cast<const uint64_t *>(buffer + off));
            uint64x2_t diff = veorq_u64(loaded, value);
            if ((vgetq_lane_u64(diff, 0) | vgetq_lane_u64(diff, 1)) != 0) {
                report_fail_msg("mem_disambiguation_neon: NEON full-width "
                                "forward mismatch at pattern %d (offset "
                                "%llu): expected {%llx,%llx} got "
                                "{%llx,%llx}", i,
                                (unsigned long long)off,
                                (unsigned long long)vgetq_lane_u64(value, 0),
                                (unsigned long long)vgetq_lane_u64(value, 1),
                                (unsigned long long)vgetq_lane_u64(loaded, 0),
                                (unsigned long long)vgetq_lane_u64(loaded, 1));
            }
        }

        /* Pattern 2 — partial overlap: 16B store, 8B load at off+4 */
        for (int i = 0; i < MDN_PATTERN_COUNT; i += 2) {
            uint64_t off = d->offsets[i];
            uint64x2_t value = vld1q_u64(&d->values[i]);
            vst1q_u64(reinterpret_cast<uint64_t *>(buffer + off), value);
            /* 8 bytes starting 4 bytes into the stored window: the
             * partial lane pair [0..1] of a shifted vector view */
            uint64_t loaded = *reinterpret_cast<uint64_t *>(buffer + off + 4);
            uint64_t expect = (vgetq_lane_u64(value, 0) >> 32)
                              | (vgetq_lane_u64(value, 1) << 32);
            if (loaded != expect) {
                report_fail_msg("mem_disambiguation_neon: NEON partial "
                                "forward mismatch at pattern %d (offset "
                                "%llu): expected 0x%llx got 0x%llx", i,
                                (unsigned long long)off,
                                (unsigned long long)expect,
                                (unsigned long long)loaded);
            }
        }

        /* Pattern 3 — repeated store training on the NEON path */
        for (int i = 0; i < MDN_PATTERN_COUNT; i += 2) {
            uint64x2_t value = vld1q_u64(&d->values[i]);
            vst1q_u64(reinterpret_cast<uint64_t *>(buffer), value);
            vst1q_u64(reinterpret_cast<uint64_t *>(buffer), value);
            vst1q_u64(reinterpret_cast<uint64_t *>(buffer), value);
            uint64x2_t loaded = vld1q_u64(reinterpret_cast<const uint64_t *>(buffer));
            uint64x2_t diff = veorq_u64(loaded, value);
            if ((vgetq_lane_u64(diff, 0) | vgetq_lane_u64(diff, 1)) != 0) {
                report_fail_msg("mem_disambiguation_neon: NEON repeated "
                                "store-load mismatch at pattern %d: "
                                "expected {%llx,%llx}", i,
                                (unsigned long long)vgetq_lane_u64(value, 0),
                                (unsigned long long)vgetq_lane_u64(value, 1));
            }
        }
    } while (test_time_condition(test));

    free(buffer);
    return EXIT_SUCCESS;
}

DECLARE_TEST(mem_disambiguation_neon, "NEON variant of mem_disambiguation: 16B vector store-to-load forwarding, partial-overlap forward, and repeated-store training on the NEON LSU path (ARM64)")
    .test_init = mem_disambiguation_neon_init,
    .test_run = mem_disambiguation_neon_run,
    .test_cleanup = mem_disambiguation_neon_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
