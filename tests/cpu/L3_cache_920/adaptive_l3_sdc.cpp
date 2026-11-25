/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b adaptive_l3_sdc
 * @parblock
 * Adaptive L3-domain SDC test (2026-11-18).  Runtime model is the
 * adaptive_mem_sdc chassis (tests/cpu/misc/, 2026-11-13): every
 * short-lived fork_each_test child picks ONE combo from a 21-entry
 * rotation and runs it for its whole lifetime — combat mode by
 * wall-clock slice (30min per combo, anchor in a named shm), test mode
 * one TEST_SLICE window (default 600s) per combo in rotation order,
 * verify mode by a round-robin counter (each child takes the next
 * combo, so a short -t run covers all 21 goldens).
 *
 * Engine layer is new: the 21 combos = 7 L3-domain access patterns x
 * 3 datapaths (scalar / NEON u32x4 / FP64 f64x2), each pattern
 * semantically identical to its verified parent in this directory:
 *   SEQ_READ    mesh_upi_avx2_read_L3_int.cpp / _neon_fp64.cpp
 *               (shared 4MB array, golden sum, full re-read per pass)
 *   RAND_READ   llc_randomized_load.cpp / _neon / _neon_fp64
 *               (per-thread L3-sized window, random-index loads,
 *                store->asm-ldr forwarding pair, periodic dense sweep)
 *   STRIDE_READ llc_uniform_load.cpp / _neon / _neon_fp64
 *               (128B stride walk + per-pass dense sweep)
 *   WRITE       mesh_upi_avx2_write_only_int.cpp / _neon_fp64
 *               (4MB per-thread buffer, write-fill + golden sum,
 *                delayed full re-read)
 *   COPY        memcpy_l3_cache_size.cpp / _neon / _neon_fp64
 *               (1.5xL3 window, random L3-scale blocks, 0x5A poison,
 *                full memcmp)
 *   RMW         llcstress.cpp / llcstress_neon.cpp / _neon_fp64
 *               (64MB shared window, band-interleaved per-thread
 *                slots, immediate read-back + delayed sweep)
 *   FETCH       l3_execute.cpp / _neon / _neon_fp64
 *               (dc civac + ic ivau refetch of a noinline ALU kernel,
 *                init-style golden snapshot, bit compare)
 *
 * On SDC the failing thread sets the shm sdc_flag (combat semantics:
 * later children idle), reports with the combo tag, and fails.
 *
 * Env contract: ADAPTIVE_L3_SDC_KEY (shm name suffix), MODE
 * (combat|test), TEST_SLICE (seconds, default 600), ORDER (full
 * permutation of 0..20), COMBO (lock every child to one combo),
 * VERIFY=1 (round-robin golden smoke), COVFILE (one line per child:
 * "<iso> <pid> <dp_tag> <mode> <combo>").
 *
 * NOTE on .fracture_loop_count: deliberately NOT set (same as the
 * adaptive_mem_sdc chassis).  The rotation model NEEDS the framework's
 * default auto-fracture: each child lives ~40 semantic rounds, then
 * the parent re-forks and the next child picks the next combo.  With
 * fracture_loop_count = -1 a single child would own the whole -t and
 * VERIFY coverage would cap at the thread count (4 combos at -n 4),
 * making the 21-combo golden-smoke gate unreachable.
 * @endparblock
 */

#include "sandstone.h"
#include "sandstone_p.h"              /* device_count/device_info: L3 probe */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <vector>

#if defined(__aarch64__)
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <arm_neon.h>
#endif

/* ------------------------------------------------------------------ */
/* Combo table: 21 rows = 7 patterns x 3 datapaths                     */
/* ------------------------------------------------------------------ */

enum AL3Pattern {
    AL3_SEQ_READ, AL3_RAND_READ, AL3_STRIDE_READ, AL3_WRITE,
    AL3_COPY, AL3_RMW, AL3_FETCH,
    AL3_PATTERN_COUNT
};
enum AL3Datapath { DP_SCALAR, DP_NEON, DP_FP64 };

#define AL3_COMBO_COUNT 21
#define AL3_SLICE_SECONDS 1800        /* combat-mode slice (30min) */

struct AL3ComboDef {
    enum AL3Pattern pattern;
    enum AL3Datapath dp;
    const char *label;                /* static geometry descriptor */
};

/* Table order: pattern outer, datapath inner (scalar, neon, fp64);
 * combo# = row index everywhere (tags, COMBO lock, verify round-robin). */
static const struct AL3ComboDef kComboTable[AL3_COMBO_COUNT] = {
    /* #0-2  seq_read: shared 4MB, golden sum, full re-read */
    { AL3_SEQ_READ,    DP_SCALAR, "4MB_shared" },
    { AL3_SEQ_READ,    DP_NEON,   "4MB_shared" },
    { AL3_SEQ_READ,    DP_FP64,   "4MB_shared" },
    /* #3-5  rand_read: per-thread L3 window, forwarding pair + sweep */
    { AL3_RAND_READ,   DP_SCALAR, "l3_win" },
    { AL3_RAND_READ,   DP_NEON,   "l3_win" },
    { AL3_RAND_READ,   DP_FP64,   "l3_win" },
    /* #6-8  stride_read: 128B stride walk + dense sweep */
    { AL3_STRIDE_READ, DP_SCALAR, "l3_win_128B" },
    { AL3_STRIDE_READ, DP_NEON,   "l3_win_128B" },
    { AL3_STRIDE_READ, DP_FP64,   "l3_win_128B" },
    /* #9-11 write: 4MB per-thread, two-pass fill/re-read */
    { AL3_WRITE,       DP_SCALAR, "4MB_priv" },
    { AL3_WRITE,       DP_NEON,   "4MB_priv" },
    { AL3_WRITE,       DP_FP64,   "4MB_priv" },
    /* #12-14 copy: 1.5xL3 window, random blocks, poison + memcmp */
    { AL3_COPY,        DP_SCALAR, "1.5xL3_blk" },
    { AL3_COPY,        DP_NEON,   "1.5xL3_blk" },
    { AL3_COPY,        DP_FP64,   "1.5xL3_blk" },
    /* #15-17 rmw: 64MB shared, band interleave, shadow + sweep */
    { AL3_RMW,         DP_SCALAR, "64MB_band" },
    { AL3_RMW,         DP_NEON,   "64MB_band" },
    { AL3_RMW,         DP_FP64,   "64MB_band" },
    /* #18-20 fetch: civac+ivau refetch, golden snapshot bit-compare */
    { AL3_FETCH,       DP_SCALAR, "civac_ivau" },
    { AL3_FETCH,       DP_NEON,   "civac_ivau" },
    { AL3_FETCH,       DP_FP64,   "civac_ivau" },
};

/* Rotation order: slice slot -> combo ID.  Identity = table order:
 * UNCALIBRATED (no measured-TTF sweep exists for the L3 combo space —
 * this machine's zeros are negative-space data, another machine's
 * could be the fastest arms; each host recalibrates via
 * ADAPTIVE_L3_SDC_ORDER, a full permutation of 0..20). */
static const int kPriorityOrder[AL3_COMBO_COUNT] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
};

#if defined(__aarch64__)

/* ------------------------------------------------------------------ */
/* Shared plumbing state (test->data; per-child, threads share it)     */
/* ------------------------------------------------------------------ */

namespace { /* ODR: per-TU data structs (66-file same-name hazard) */

struct AL3Shm {                       /* the ONLY cross-child state */
    uint32_t ready;                   /* 0 -> 1 by creator (release) */
    uint32_t verify_ctr;              /* verify round-robin (fetch_add) */
    uint32_t sdc_flag;                /* set after any child detects SDC */
    struct timespec anchor;           /* wall-clock anchor (MONOTONIC) */
};

struct AL3Data {
    struct AL3Shm *shm;
    size_t l3_size;                   /* probed per-cluster L3 (bytes) */
    int test_mode;                    /* 0=combat 1=test */
    int test_slice;                   /* test-mode slice seconds */
    int combo;                        /* this child's combo */
    char tag[96];                     /* "combo#N [pattern/dp/geom]" */
    /* SEQ_READ shared 4MB array (all threads re-read; race-free RO) */
    void *seq_buf;                    /* int32[1M] or double[512K] */
    void *seq_golden_buf;             /* element-exact golden copy (2026-11-18) */
    /* RMW shared 64MB window (per-child; slots per-thread by band) */
    uint8_t *rmw_mem;
};

} /* namespace */

/* SEQ geometry: shared 4MB (just over L2, inside the cluster L3) */
static constexpr size_t AL3_SEQ_INTS    = 1 << 20;   /* 1M int32 */
static constexpr size_t AL3_SEQ_DOUBLES = 1 << 19;   /* 512K double */
/* RMW geometry: 64MB shared window (parent llcstress family) */
static constexpr size_t AL3_RMW_BYTES   = 64ULL * 1024 * 1024;
static constexpr size_t AL3_RMW_STRIDE_QW = 1024;    /* scalar u64 slots/band */
static constexpr size_t AL3_RMW_STRIDE_V  = 512;     /* vec slots/band */
static constexpr uint64_t AL3_RMW_SWEEP_EVERY = 65536;
/* STRIDE geometry: 128B stride in words / 16B vectors */
static constexpr size_t AL3_STRIDE_WORDS = 128 / sizeof(uint32_t);
static constexpr size_t AL3_STRIDE_VECS  = 128 / 16;
/* FETCH geometry */
static constexpr int    AL3_FETCH_NUM_INPUTS = 64;
static constexpr size_t AL3_FETCH_FLUSH_BYTES = 256;
static constexpr size_t AL3_FETCH_LINE = 64;

/* ------------------------------------------------------------------ */
/* Small accessors (anti-forwarding asm per the parents)               */
/* ------------------------------------------------------------------ */

/* scalar anti-forwarding load (llc_randomized_load.cpp) */
static inline uint32_t al3_ldr_w(const uint32_t *addr) {
    uint32_t v;
    __asm__ volatile ("ldr %w0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    return v;
}
/* vector anti-forwarding loads ("=w" pins a NEON register) */
static inline uint32x4_t al3_ldr_q_u32(const uint32_t *addr) {
    uint32x4_t v;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(v) : "r"(addr) : "memory");
    return v;
}
static inline float64x2_t al3_ldr_q_f64(const double *addr) {
    float64x2_t v;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(v) : "r"(addr) : "memory");
    return v;
}
/* RMW vector slot access — BOTH directions asm (llcstress_neon: an
 * intrinsic vst1q->vld1q pair on the same address gets store-transformed
 * + load-forwarded by GCC; asm cannot be) */
static inline uint32x4_t al3_ld_vec(const uint32_t *a) {
    uint32x4_t v;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(v) : "r"(a) : "memory");
    return v;
}
static inline void al3_st_vec(uint32_t *a, uint32x4_t v) {
    __asm__ volatile ("str %q0, [%1]" :: "w"(v), "r"(a) : "memory");
}
static inline float64x2_t al3_ld_vec_f64(const double *a) {
    float64x2_t v;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(v) : "r"(a) : "memory");
    return v;
}
static inline void al3_st_vec_f64(double *a, float64x2_t v) {
    __asm__ volatile ("str %q0, [%1]" :: "w"(v), "r"(a) : "memory");
}

/* exact 0.5-multiple in [-2^19, 2^19): every partial sum exact in f64
 * (mesh f64 family discipline, cf. mesh_upi_avx2_read_L3_neon_fp64) */
static inline double al3_dist_f64(void) {
    return (double)((int64_t)(random64() % (1ULL << 21)) - (1 << 20)) * 0.5;
}
/* int32 dist [-1000000, 1000000) (mesh_upi_avx2_read_L3_int.cpp) */
static inline int32_t al3_dist_int(void) {
    return (-1000000) + (int32_t)(random64() % (uint64_t)(1000000 - (-1000000) + 1));
}

/* SDC fail path: combat-freeze flag first, then the tagged report */
static void al3_fail(struct AL3Data *d, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void al3_fail(struct AL3Data *d, const char *fmt, ...)
{
    if (d->shm)
        __atomic_store_n(&d->shm->sdc_flag, 1u, __ATOMIC_RELEASE);
    char detail[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    report_fail_msg("adaptive_l3_sdc %s: %s", d->tag, detail);
}

/* ------------------------------------------------------------------ */
/* Engine: SEQ_READ — shared 4MB, element-exact golden, full re-read   */
/* ------------------------------------------------------------------ */

static int al3_seq_read_run(struct test *test, struct AL3Data *d, int cpu)
{
    (void)cpu;
    const struct AL3ComboDef &cd = kComboTable[d->combo];

    /* 2026-11-18 element-exact rework (parent-test sync): full re-read
     * compared lane-by-lane against the init-time golden copy — the
     * previous sum-only compare was blind to permutation/swap-class
     * corruption. */
    if (cd.dp == DP_FP64) {
        const double *buf = static_cast<const double *>(d->seq_buf);
        const double *g = static_cast<const double *>(d->seq_golden_buf);
        do {
            for (size_t i = 0; i < AL3_SEQ_DOUBLES; i += 2) {
                float64x2_t v = vld1q_f64(buf + i);
                float64x2_t gv = vld1q_f64(g + i);
                uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(v),
                                            vreinterpretq_u64_f64(gv));
                if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                    al3_fail(d, "golden mismatch at pair %zu (L3 read path, "
                               "element-exact): golden (%.17g, %.17g) got "
                               "(%.17g, %.17g)", i / 2,
                             vgetq_lane_f64(gv, 0), vgetq_lane_f64(gv, 1),
                             vgetq_lane_f64(v, 0), vgetq_lane_f64(v, 1));
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
    } else if (cd.dp == DP_NEON) {
        const int32_t *buf = static_cast<const int32_t *>(d->seq_buf);
        const int32_t *g = static_cast<const int32_t *>(d->seq_golden_buf);
        do {
            for (size_t i = 0; i < AL3_SEQ_INTS; i += 4) {
                int32x4_t v = vld1q_s32(buf + i);
                int32x4_t gv = vld1q_s32(g + i);
                uint32x4_t diff = veorq_u32(vreinterpretq_u32_s32(v),
                                            vreinterpretq_u32_s32(gv));
                if (vmaxvq_u32(diff) != 0) {
                    al3_fail(d, "golden mismatch at vec %zu (L3 read path, "
                               "element-exact): golden {%d,%d,%d,%d} got "
                               "{%d,%d,%d,%d}", i / 4,
                             vgetq_lane_s32(gv, 0), vgetq_lane_s32(gv, 1),
                             vgetq_lane_s32(gv, 2), vgetq_lane_s32(gv, 3),
                             vgetq_lane_s32(v, 0), vgetq_lane_s32(v, 1),
                             vgetq_lane_s32(v, 2), vgetq_lane_s32(v, 3));
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
    } else {
        const int32_t *buf = static_cast<const int32_t *>(d->seq_buf);
        const int32_t *g = static_cast<const int32_t *>(d->seq_golden_buf);
        do {
            for (size_t i = 0; i < AL3_SEQ_INTS; ++i) {
                if (buf[i] != g[i]) {
                    al3_fail(d, "golden mismatch at idx %zu (L3 read path, "
                               "element-exact): golden 0x%08x got 0x%08x",
                             i, (unsigned)g[i], (unsigned)buf[i]);
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
    }
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Engine: RAND_READ — L3 window, random loads + forwarding pair +     */
/* periodic dense golden sweep                                         */
/* ------------------------------------------------------------------ */

static int al3_rand_read_run(struct test *test, struct AL3Data *d, int cpu)
{
    (void)cpu;
    const struct AL3ComboDef &cd = kComboTable[d->combo];
    const size_t buffer_bytes = d->l3_size;

    if (cd.dp == DP_FP64) {
        const size_t num_vecs = buffer_bytes / 16;
        std::vector<double> data(num_vecs * 2);
        std::vector<double> golden(num_vecs * 2);
        std::vector<double> check(num_vecs * 2);
        for (size_t i = 0; i < num_vecs * 2; ++i)
            data[i] = al3_dist_f64();
        memcpy(golden.data(), data.data(), buffer_bytes);
        memset(check.data(), 0, buffer_bytes);
        do {
            for (uint64_t batch = 0; batch < (1ULL << 20); ++batch) {
                size_t vidx = random64() % num_vecs;
                double *dp = data.data() + vidx * 2;
                double *cp = check.data() + vidx * 2;
                float64x2_t original = vld1q_f64(dp);
                vst1q_f64(cp, original);
                float64x2_t loaded = al3_ldr_q_f64(cp);
                uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(original),
                                             vreinterpretq_u64_f64(loaded));
                if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                    al3_fail(d, "store->load mismatch at vec %zu (LSU "
                               "forwarding path)", vidx);
                    return EXIT_FAILURE;
                }
            }
            for (size_t v = 0; v < num_vecs; ++v) {
                float64x2_t x = vld1q_f64(data.data() + v * 2);
                float64x2_t g = vld1q_f64(golden.data() + v * 2);
                uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(x),
                                            vreinterpretq_u64_f64(g));
                if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                    al3_fail(d, "golden sweep mismatch at vec %zu: golden "
                               "(%.17g, %.17g) got (%.17g, %.17g) "
                               "(L3-resident data corruption, element-exact)",
                             v, vgetq_lane_f64(g, 0), vgetq_lane_f64(g, 1),
                             vgetq_lane_f64(x, 0), vgetq_lane_f64(x, 1));
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
    } else if (cd.dp == DP_NEON) {
        const size_t num_vecs = buffer_bytes / 16;
        std::vector<uint32_t> data(num_vecs * 4);
        std::vector<uint32_t> golden(num_vecs * 4);
        std::vector<uint32_t> check(num_vecs * 4);
        memset_random(data.data(), buffer_bytes);
        memcpy(golden.data(), data.data(), buffer_bytes);
        memset(check.data(), 0, buffer_bytes);
        do {
            for (uint64_t batch = 0; batch < (1ULL << 20); ++batch) {
                size_t vidx = random64() % num_vecs;
                uint32_t *dp = data.data() + vidx * 4;
                uint32_t *cp = check.data() + vidx * 4;
                uint32x4_t original = vld1q_u32(dp);
                vst1q_u32(cp, original);
                uint32x4_t loaded = al3_ldr_q_u32(cp);
                uint32x4_t diff = veorq_u32(original, loaded);
                if (vmaxvq_u32(diff) != 0) {
                    al3_fail(d, "store->load mismatch at vec %zu: stored "
                               "{%08x,%08x,%08x,%08x} loaded {%08x,%08x,"
                               "%08x,%08x} (LSU forwarding path)", vidx,
                             vgetq_lane_u32(original, 0), vgetq_lane_u32(original, 1),
                             vgetq_lane_u32(original, 2), vgetq_lane_u32(original, 3),
                             vgetq_lane_u32(loaded, 0), vgetq_lane_u32(loaded, 1),
                             vgetq_lane_u32(loaded, 2), vgetq_lane_u32(loaded, 3));
                    return EXIT_FAILURE;
                }
            }
            for (size_t v = 0; v < num_vecs; ++v) {
                uint32x4_t x = vld1q_u32(data.data() + v * 4);
                uint32x4_t g = vld1q_u32(golden.data() + v * 4);
                uint32x4_t diff = veorq_u32(x, g);
                if (vmaxvq_u32(diff) != 0) {
                    al3_fail(d, "golden sweep mismatch at vec %zu: golden "
                               "{%08x,%08x,%08x,%08x} got {%08x,%08x,%08x,"
                               "%08x} (L3-resident data corruption, "
                               "element-exact)", v,
                             vgetq_lane_u32(g, 0), vgetq_lane_u32(g, 1),
                             vgetq_lane_u32(g, 2), vgetq_lane_u32(g, 3),
                             vgetq_lane_u32(x, 0), vgetq_lane_u32(x, 1),
                             vgetq_lane_u32(x, 2), vgetq_lane_u32(x, 3));
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
    } else {
        const size_t num_words = buffer_bytes / sizeof(uint32_t);
        std::vector<uint32_t> data(num_words);
        std::vector<uint32_t> golden(num_words);
        std::vector<uint32_t> check(num_words);
        memset_random(data.data(), buffer_bytes);
        memcpy(golden.data(), data.data(), buffer_bytes);
        memset(check.data(), 0, buffer_bytes);
        do {
            for (uint64_t batch = 0; batch < (1ULL << 22); ++batch) {
                size_t idx = random64() % num_words;
                uint32_t original = data[idx];
                check[idx] = original;
                uint32_t loaded = al3_ldr_w(&check[idx]);
                if (memcmp(&original, &loaded, sizeof(uint32_t)) != 0) {
                    al3_fail(d, "store->load consistency failure at idx "
                               "%zu: stored 0x%08x loaded 0x%08x (LSU "
                               "forwarding path)", idx, original, loaded);
                    return EXIT_FAILURE;
                }
            }
            for (size_t i = 0; i < num_words; ++i) {
                if (data[i] != golden[i]) {
                    al3_fail(d, "golden sweep mismatch at idx %zu: golden "
                               "0x%08x got 0x%08x (L3-resident data "
                               "corruption, element-exact)", i,
                             (unsigned)golden[i], (unsigned)data[i]);
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
    }
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Engine: STRIDE_READ — 128B stride walk + per-pass dense sweep       */
/* ------------------------------------------------------------------ */

static int al3_stride_read_run(struct test *test, struct AL3Data *d, int cpu)
{
    (void)cpu;
    const struct AL3ComboDef &cd = kComboTable[d->combo];
    const size_t buffer_bytes = d->l3_size;

    if (cd.dp == DP_FP64) {
        const size_t num_vecs = buffer_bytes / 16;
        std::vector<double> data(num_vecs * 2);
        std::vector<double> golden(num_vecs * 2);
        std::vector<double> check(num_vecs * 2);
        for (size_t i = 0; i < num_vecs * 2; ++i)
            data[i] = al3_dist_f64();
        memcpy(golden.data(), data.data(), buffer_bytes);
        memset(check.data(), 0, buffer_bytes);
        size_t offset = random64() % AL3_STRIDE_VECS;
        do {
            for (size_t v = offset; v < num_vecs; v += AL3_STRIDE_VECS) {
                double *dp = data.data() + v * 2;
                double *cp = check.data() + v * 2;
                float64x2_t original = vld1q_f64(dp);
                vst1q_f64(cp, original);
                float64x2_t loaded = al3_ldr_q_f64(cp);
                uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(original),
                                             vreinterpretq_u64_f64(loaded));
                if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                    al3_fail(d, "store->load mismatch at vec %zu (LSU "
                               "forwarding path under strided access)", v);
                    return EXIT_FAILURE;
                }
            }
            offset = (offset + 1) % AL3_STRIDE_VECS;
            for (size_t v = 0; v < num_vecs; ++v) {
                float64x2_t x = vld1q_f64(data.data() + v * 2);
                float64x2_t g = vld1q_f64(golden.data() + v * 2);
                uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(x),
                                            vreinterpretq_u64_f64(g));
                if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                    al3_fail(d, "golden sweep mismatch at vec %zu: golden "
                               "(%.17g, %.17g) got (%.17g, %.17g) "
                               "(L3-resident data corruption, element-exact)",
                             v, vgetq_lane_f64(g, 0), vgetq_lane_f64(g, 1),
                             vgetq_lane_f64(x, 0), vgetq_lane_f64(x, 1));
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
    } else if (cd.dp == DP_NEON) {
        const size_t num_vecs = buffer_bytes / 16;
        std::vector<uint32_t> data(num_vecs * 4);
        std::vector<uint32_t> golden(num_vecs * 4);
        std::vector<uint32_t> check(num_vecs * 4);
        memset_random(data.data(), buffer_bytes);
        memcpy(golden.data(), data.data(), buffer_bytes);
        memset(check.data(), 0, buffer_bytes);
        size_t offset = random64() % AL3_STRIDE_VECS;
        do {
            for (size_t v = offset; v < num_vecs; v += AL3_STRIDE_VECS) {
                uint32_t *dp = data.data() + v * 4;
                uint32_t *cp = check.data() + v * 4;
                uint32x4_t original = vld1q_u32(dp);
                vst1q_u32(cp, original);
                uint32x4_t loaded = al3_ldr_q_u32(cp);
                uint32x4_t diff = veorq_u32(original, loaded);
                if (vmaxvq_u32(diff) != 0) {
                    al3_fail(d, "store->load mismatch at vec %zu: stored "
                               "{%08x,%08x,%08x,%08x} loaded {%08x,%08x,"
                               "%08x,%08x} (LSU forwarding path under "
                               "strided access)", v,
                             vgetq_lane_u32(original, 0), vgetq_lane_u32(original, 1),
                             vgetq_lane_u32(original, 2), vgetq_lane_u32(original, 3),
                             vgetq_lane_u32(loaded, 0), vgetq_lane_u32(loaded, 1),
                             vgetq_lane_u32(loaded, 2), vgetq_lane_u32(loaded, 3));
                    return EXIT_FAILURE;
                }
            }
            offset = (offset + 1) % AL3_STRIDE_VECS;
            for (size_t v = 0; v < num_vecs; ++v) {
                uint32x4_t x = vld1q_u32(data.data() + v * 4);
                uint32x4_t g = vld1q_u32(golden.data() + v * 4);
                uint32x4_t diff = veorq_u32(x, g);
                if (vmaxvq_u32(diff) != 0) {
                    al3_fail(d, "golden sweep mismatch at vec %zu: golden "
                               "{%08x,%08x,%08x,%08x} got {%08x,%08x,%08x,"
                               "%08x} (L3-resident data corruption, "
                               "element-exact)", v,
                             vgetq_lane_u32(g, 0), vgetq_lane_u32(g, 1),
                             vgetq_lane_u32(g, 2), vgetq_lane_u32(g, 3),
                             vgetq_lane_u32(x, 0), vgetq_lane_u32(x, 1),
                             vgetq_lane_u32(x, 2), vgetq_lane_u32(x, 3));
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
    } else {
        const size_t num_words = buffer_bytes / sizeof(uint32_t);
        std::vector<uint32_t> data(num_words);
        std::vector<uint32_t> golden(num_words);
        std::vector<uint32_t> check(num_words);
        memset_random(data.data(), buffer_bytes);
        memcpy(golden.data(), data.data(), buffer_bytes);
        memset(check.data(), 0, buffer_bytes);
        size_t offset = random64() % AL3_STRIDE_WORDS;
        do {
            for (size_t idx = offset; idx < num_words; idx += AL3_STRIDE_WORDS) {
                uint32_t original = data[idx];
                check[idx] = original;
                uint32_t loaded = al3_ldr_w(&check[idx]);
                if (memcmp(&original, &loaded, sizeof(uint32_t)) != 0) {
                    al3_fail(d, "store->load consistency failure at idx "
                               "%zu: stored 0x%08x loaded 0x%08x (LSU "
                               "forwarding path under strided access)",
                             idx, original, loaded);
                    return EXIT_FAILURE;
                }
            }
            offset = (offset + 1) % AL3_STRIDE_WORDS;
            for (size_t i = 0; i < num_words; ++i) {
                if (data[i] != golden[i]) {
                    al3_fail(d, "golden sweep mismatch at idx %zu: golden "
                               "0x%08x got 0x%08x (L3-resident data "
                               "corruption, element-exact)", i,
                             (unsigned)golden[i], (unsigned)data[i]);
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
    }
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Engine: WRITE — 4MB per-thread, write-fill + golden, delayed        */
/* full re-read (L3/DRAM-served verification point)                    */
/* ------------------------------------------------------------------ */

static int al3_write_run(struct test *test, struct AL3Data *d, int cpu)
{
    (void)cpu;
    const struct AL3ComboDef &cd = kComboTable[d->combo];

    if (cd.dp == DP_FP64) {
        alignas(16) double *buf = static_cast<double *>(
                aligned_alloc(16, AL3_SEQ_DOUBLES * sizeof(double)));
        alignas(16) double *golden = static_cast<double *>(
                aligned_alloc(16, AL3_SEQ_DOUBLES * sizeof(double)));
        if (!buf || !golden) {
            free(buf); free(golden);
            al3_fail(d, "alloc failed");
            return EXIT_FAILURE;
        }
        do {
            /* 2026-11-18 element-exact rework (parent-test sync): the
             * previous sum-only compare was blind to permutation/
             * swap-class write-path damage. */
            for (size_t i = 0; i < AL3_SEQ_DOUBLES; i += 2) {
                double vals[2];
                for (int l = 0; l < 2; ++l)
                    vals[l] = al3_dist_f64();
                float64x2_t v = vld1q_f64(vals);
                vst1q_f64(buf + i, v);
                vst1q_f64(golden + i, v);
            }
            for (size_t i = 0; i < AL3_SEQ_DOUBLES; i += 2) {
                float64x2_t loaded = vld1q_f64(buf + i);
                float64x2_t g = vld1q_f64(golden + i);
                uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(loaded),
                                            vreinterpretq_u64_f64(g));
                if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                    al3_fail(d, "write->delayed-re-read golden mismatch at "
                               "pair %zu (L3 write path, element-exact): "
                               "golden (%.17g, %.17g) got (%.17g, %.17g)",
                             i / 2,
                             vgetq_lane_f64(g, 0), vgetq_lane_f64(g, 1),
                             vgetq_lane_f64(loaded, 0), vgetq_lane_f64(loaded, 1));
                    free(buf); free(golden);
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
        free(buf);
        free(golden);
    } else if (cd.dp == DP_NEON) {
        alignas(16) int32_t *buf = static_cast<int32_t *>(
                aligned_alloc(16, AL3_SEQ_INTS * sizeof(int32_t)));
        alignas(16) int32_t *golden = static_cast<int32_t *>(
                aligned_alloc(16, AL3_SEQ_INTS * sizeof(int32_t)));
        if (!buf || !golden) {
            free(buf); free(golden);
            al3_fail(d, "alloc failed");
            return EXIT_FAILURE;
        }
        do {
            for (size_t i = 0; i < AL3_SEQ_INTS; i += 4) {
                int32_t vals[4];
                for (int j = 0; j < 4; ++j)
                    vals[j] = al3_dist_int();
                int32x4_t v = vld1q_s32(vals);
                vst1q_s32(buf + i, v);
                vst1q_s32(golden + i, v);
            }
            for (size_t i = 0; i < AL3_SEQ_INTS; i += 4) {
                int32x4_t loaded = vld1q_s32(buf + i);
                int32x4_t g = vld1q_s32(golden + i);
                uint32x4_t diff = veorq_u32(vreinterpretq_u32_s32(loaded),
                                            vreinterpretq_u32_s32(g));
                if (vmaxvq_u32(diff) != 0) {
                    al3_fail(d, "write->delayed-re-read golden mismatch at "
                               "vec %zu (L3 write path, element-exact): "
                               "golden {%d,%d,%d,%d} got {%d,%d,%d,%d}",
                             i / 4,
                             vgetq_lane_s32(g, 0), vgetq_lane_s32(g, 1),
                             vgetq_lane_s32(g, 2), vgetq_lane_s32(g, 3),
                             vgetq_lane_s32(loaded, 0), vgetq_lane_s32(loaded, 1),
                             vgetq_lane_s32(loaded, 2), vgetq_lane_s32(loaded, 3));
                    free(buf); free(golden);
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
        free(buf);
        free(golden);
    } else {
        alignas(16) int32_t *buf = static_cast<int32_t *>(
                aligned_alloc(16, AL3_SEQ_INTS * sizeof(int32_t)));
        alignas(16) int32_t *golden = static_cast<int32_t *>(
                aligned_alloc(16, AL3_SEQ_INTS * sizeof(int32_t)));
        if (!buf || !golden) {
            free(buf); free(golden);
            al3_fail(d, "alloc failed");
            return EXIT_FAILURE;
        }
        do {
            for (size_t i = 0; i < AL3_SEQ_INTS; ++i)
                golden[i] = buf[i] = al3_dist_int();
            for (size_t i = 0; i < AL3_SEQ_INTS; ++i) {
                if (buf[i] != golden[i]) {
                    al3_fail(d, "write->delayed-re-read golden mismatch at "
                               "idx %zu (L3 write path, element-exact): "
                               "golden 0x%08x got 0x%08x", i,
                             (unsigned)golden[i], (unsigned)buf[i]);
                    free(buf); free(golden);
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
        free(buf);
        free(golden);
    }
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Engine: COPY — 1.5xL3 window, random L3-scale blocks (64B-aligned   */
/* offsets), 0x5A poison, full memcmp                                  */
/* ------------------------------------------------------------------ */

static int al3_copy_run(struct test *test, struct AL3Data *d, int cpu)
{
    (void)cpu;
    const struct AL3ComboDef &cd = kComboTable[d->combo];
    const size_t BLOCK_SIZE = d->l3_size;
    const size_t window = BLOCK_SIZE + (BLOCK_SIZE / 2);
    std::vector<uint8_t> src(window);
    std::vector<uint8_t> dst(window);

    do {
        size_t max_len = BLOCK_SIZE;
        size_t len = 64 * (1 + random32() % (max_len / 64));
        size_t src_off = 64 * (random32() % ((window - len) / 64 + 1));
        size_t dst_off = 64 * (random32() % ((window - len) / 64 + 1));

        memset_random(src.data() + src_off, len);
        memset(dst.data() + dst_off, 0x5A, len);   /* poison: lazy copy fails */

        if (cd.dp == DP_FP64) {
            const double *s = reinterpret_cast<const double *>(src.data() + src_off);
            double *dd = reinterpret_cast<double *>(dst.data() + dst_off);
            for (size_t i = 0; i < len / 16; ++i) {
                float64x2_t v = vld1q_f64(s + i * 2);
                vst1q_f64(dd + i * 2, v);
            }
        } else if (cd.dp == DP_NEON) {
            /* explicit vld1q_u32/vst1q_u32 16B stream (64B-multiple len,
             * no tail) — NEON datapath attribution, libc dispatcher off */
            const uint32_t *s = reinterpret_cast<const uint32_t *>(src.data() + src_off);
            uint32_t *dd = reinterpret_cast<uint32_t *>(dst.data() + dst_off);
            for (size_t i = 0; i < len / 16; ++i) {
                uint32x4_t v = vld1q_u32(s + i * 4);
                vst1q_u32(dd + i * 4, v);
            }
        } else {
            memcpy(dst.data() + dst_off, src.data() + src_off, len);
        }

        if (memcmp(dst.data() + dst_off, src.data() + src_off, len) != 0) {
            al3_fail(d, "copy mismatch (len %zu, src_off %zu, dst_off %zu, "
                       "L3 block %zu)", len, src_off, dst_off, BLOCK_SIZE);
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Engine: RMW — 64MB shared window, band-interleaved per-thread       */
/* slots, immediate read-back + delayed shadow sweep                   */
/* ------------------------------------------------------------------ */

static int al3_rmw_run(struct test *test, struct AL3Data *d, int cpu)
{
    const struct AL3ComboDef &cd = kComboTable[d->combo];

    if (cd.dp == DP_FP64) {
        double *base = reinterpret_cast<double *>(d->rmw_mem);
        const size_t bands = AL3_RMW_BYTES / 16 / AL3_RMW_STRIDE_V;
        std::vector<double> counts(bands * 2, 0.0);
        do {
            for (uint64_t batch = 0; batch < AL3_RMW_SWEEP_EVERY; ++batch) {
                size_t band = random64() % bands;
                double *slot = base + (band * AL3_RMW_STRIDE_V + (size_t)cpu) * 2;
                float64x2_t v = al3_ld_vec_f64(slot);
                v = vaddq_f64(v, vdupq_n_f64(1.0));
                al3_st_vec_f64(slot, v);
                double *cnt = counts.data() + band * 2;
                float64x2_t c = vaddq_f64(vld1q_f64(cnt), vdupq_n_f64(1.0));
                vst1q_f64(cnt, c);
                float64x2_t mem_v = al3_ld_vec_f64(slot);
                uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(mem_v),
                                             vreinterpretq_u64_f64(c));
                if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                    al3_fail(d, "slot read-back mismatch (band %zu): "
                               "mem-bits {%016llx,%016llx} vs expected-bits "
                               "{%016llx,%016llx} (lost/corrupted FP64 store)",
                             band,
                             (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(mem_v), 0),
                             (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(mem_v), 1),
                             (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(c), 0),
                             (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(c), 1));
                    return EXIT_FAILURE;
                }
            }
            for (size_t b = 0; b < bands; ++b) {
                double *slot = base + (b * AL3_RMW_STRIDE_V + (size_t)cpu) * 2;
                float64x2_t mem_v = al3_ld_vec_f64(slot);
                float64x2_t c = vld1q_f64(counts.data() + b * 2);
                uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(mem_v),
                                             vreinterpretq_u64_f64(c));
                if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                    al3_fail(d, "delayed re-verification mismatch (band %zu): "
                               "mem-bits {%016llx,%016llx} vs expected-bits "
                               "{%016llx,%016llx} (L3-resident corruption)", b,
                             (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(mem_v), 0),
                             (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(mem_v), 1),
                             (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(c), 0),
                             (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(c), 1));
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
    } else if (cd.dp == DP_NEON) {
        uint32_t *base = reinterpret_cast<uint32_t *>(d->rmw_mem);
        const size_t bands = AL3_RMW_BYTES / 16 / AL3_RMW_STRIDE_V;
        std::vector<uint32_t> counts(bands * 4, 0);
        do {
            for (uint64_t batch = 0; batch < AL3_RMW_SWEEP_EVERY; ++batch) {
                size_t band = random64() % bands;
                uint32_t *slot = base + (band * AL3_RMW_STRIDE_V + (size_t)cpu) * 4;
                uint32x4_t v = al3_ld_vec(slot);
                v = vaddq_u32(v, vdupq_n_u32(1));
                al3_st_vec(slot, v);
                uint32_t *cnt = counts.data() + band * 4;
                uint32x4_t c = vaddq_u32(vld1q_u32(cnt), vdupq_n_u32(1));
                vst1q_u32(cnt, c);
                uint32x4_t mem_v = al3_ld_vec(slot);
                uint32x4_t diff = veorq_u32(mem_v, c);
                if (vmaxvq_u32(diff) != 0) {
                    al3_fail(d, "slot read-back mismatch (band %zu): mem "
                               "{%u,%u,%u,%u} vs expected {%u,%u,%u,%u} "
                               "(lost/corrupted NEON store)", band,
                             vgetq_lane_u32(mem_v, 0), vgetq_lane_u32(mem_v, 1),
                             vgetq_lane_u32(mem_v, 2), vgetq_lane_u32(mem_v, 3),
                             vgetq_lane_u32(c, 0), vgetq_lane_u32(c, 1),
                             vgetq_lane_u32(c, 2), vgetq_lane_u32(c, 3));
                    return EXIT_FAILURE;
                }
            }
            for (size_t b = 0; b < bands; ++b) {
                uint32_t *slot = base + (b * AL3_RMW_STRIDE_V + (size_t)cpu) * 4;
                uint32x4_t mem_v = al3_ld_vec(slot);
                uint32x4_t c = vld1q_u32(counts.data() + b * 4);
                uint32x4_t diff = veorq_u32(mem_v, c);
                if (vmaxvq_u32(diff) != 0) {
                    al3_fail(d, "delayed re-verification mismatch (band %zu): "
                               "mem {%u,%u,%u,%u} vs expected {%u,%u,%u,%u} "
                               "(L3-resident corruption)", b,
                             vgetq_lane_u32(mem_v, 0), vgetq_lane_u32(mem_v, 1),
                             vgetq_lane_u32(mem_v, 2), vgetq_lane_u32(mem_v, 3),
                             vgetq_lane_u32(c, 0), vgetq_lane_u32(c, 1),
                             vgetq_lane_u32(c, 2), vgetq_lane_u32(c, 3));
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
    } else {
        const size_t bands = AL3_RMW_BYTES / 8 / AL3_RMW_STRIDE_QW;
        std::vector<uint64_t> shadow(bands, 0);
        do {
            for (uint64_t batch = 0; batch < AL3_RMW_SWEEP_EVERY; ++batch) {
                size_t band = random64() % bands;
                size_t offset = (band * AL3_RMW_STRIDE_QW + (size_t)cpu) * 8;
                uint64_t write_val = random64();
                uint64_t *ptr = reinterpret_cast<uint64_t *>(d->rmw_mem + offset);
                *ptr = write_val;
                uint64_t read_val = *ptr;
                if (read_val != write_val) {
                    al3_fail(d, "read-back mismatch (band %zu, offset 0x%zx): "
                               "wrote 0x%016llx read 0x%016llx", band, offset,
                             (unsigned long long)write_val,
                             (unsigned long long)read_val);
                    return EXIT_FAILURE;
                }
                shadow[band] = write_val;
            }
            for (size_t b = 0; b < bands; ++b) {
                size_t offset = (b * AL3_RMW_STRIDE_QW + (size_t)cpu) * 8;
                uint64_t v = *reinterpret_cast<const uint64_t *>(d->rmw_mem + offset);
                if (v != shadow[b]) {
                    al3_fail(d, "delayed re-verification mismatch (band %zu, "
                               "offset 0x%zx): wrote 0x%016llx reread "
                               "0x%016llx (L3-resident corruption)", b, offset,
                             (unsigned long long)shadow[b],
                             (unsigned long long)v);
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
    }
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Engine: FETCH — civac+ivau refetch of a noinline kernel, golden     */
/* snapshot bit-compare (l3_execute family)                            */
/* ------------------------------------------------------------------ */

static __attribute__((noinline)) double al3_fetch_worker_scalar(double a, double b) {
    double denom = (b == 0.0) ? 1.0 : b;
    return a * a + b * b - a * b + a / denom;
}

static __attribute__((noinline)) uint32x4_t al3_fetch_worker_neon(uint32x4_t v, uint32_t round) {
    const uint32x4_t k1 = vdupq_n_u32(0x2545F491u);
    const uint32x4_t k2 = vdupq_n_u32(0x9E3779B1u);
    uint32x4_t a = veorq_u32(v, k1);
    uint32x4_t b = vaddq_u32(v, vdupq_n_u32(round));
    for (int i = 0; i < 8; ++i) {
        a = veorq_u32(a, vmulq_u32(b, k1));
        b = vaddq_u32(b, vshlq_n_u32(a, 3));
    }
    return veorq_u32(a, vmulq_u32(b, k2));
}

static __attribute__((noinline)) float64x2_t al3_fetch_worker_f64(float64x2_t v, uint32_t round) {
    const float64x2_t k1 = vdupq_n_f64(1.0000001);
    const float64x2_t k2 = vdupq_n_f64(1.5);
    float64x2_t a = vsubq_f64(v, k1);
    float64x2_t b = vaddq_f64(v, vdupq_n_f64((double)round));
    for (int i = 0; i < 8; ++i) {
        a = vaddq_f64(a, vmulq_f64(b, k1));
        b = vsubq_f64(b, a);
    }
    return vmulq_f64(a, vsubq_f64(b, k2));
}

/* dc civac (to PoC, evicts L2) -> dsb -> ic ivau -> dsb -> isb: the
 * next fetch misses L1I AND L2 and is genuinely L3-served */
static inline void al3_flush_icache(const void *start, size_t len) {
    const char *base = reinterpret_cast<const char *>(
        reinterpret_cast<uintptr_t>(start) & ~(uintptr_t)(AL3_FETCH_LINE - 1));
    const char *end = reinterpret_cast<const char *>(start) + len;
    for (const char *p = base; p < end; p += AL3_FETCH_LINE)
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (const char *p = base; p < end; p += AL3_FETCH_LINE)
        __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

static int al3_fetch_run(struct test *test, struct AL3Data *d, int cpu)
{
    (void)cpu;
    const struct AL3ComboDef &cd = kComboTable[d->combo];

    /* deterministic inputs + pre-flush golden snapshot (same-source
     * cross-time recompute; all threads see identical constants) */
    if (cd.dp == DP_FP64) {
        double in[AL3_FETCH_NUM_INPUTS][2];
        double golden[AL3_FETCH_NUM_INPUTS][2];
        for (int i = 0; i < AL3_FETCH_NUM_INPUTS; ++i) {
            for (int l = 0; l < 2; ++l)
                in[i][l] = (double)(i * 7 + l * 13 + 1);   /* mild integers */
            float64x2_t r = al3_fetch_worker_f64(vld1q_f64(in[i]), (uint32_t)i);
            vst1q_f64(golden[i], r);
        }
        do {
            al3_flush_icache(reinterpret_cast<const void *>(&al3_fetch_worker_f64),
                             AL3_FETCH_FLUSH_BYTES);
            for (int i = 0; i < AL3_FETCH_NUM_INPUTS; ++i) {
                float64x2_t r = al3_fetch_worker_f64(vld1q_f64(in[i]), (uint32_t)i);
                float64x2_t g = vld1q_f64(golden[i]);
                uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(r),
                                             vreinterpretq_u64_f64(g));
                if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                    uint64_t out[2], gw[2];
                    vst1q_u64(out, vreinterpretq_u64_f64(r));
                    vst1q_u64(gw, vreinterpretq_u64_f64(g));
                    al3_fail(d, "FP64 execution differs from init snapshot "
                               "after I-cache reload: input %d: expected-bits "
                               "{%016llx,%016llx} got-bits {%016llx,%016llx} "
                               "(fetch-path corruption?)", i,
                             (unsigned long long)gw[0], (unsigned long long)gw[1],
                             (unsigned long long)out[0], (unsigned long long)out[1]);
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
    } else if (cd.dp == DP_NEON) {
        uint32_t in[AL3_FETCH_NUM_INPUTS][4];
        uint32_t golden[AL3_FETCH_NUM_INPUTS][4];
        for (int i = 0; i < AL3_FETCH_NUM_INPUTS; ++i) {
            for (int l = 0; l < 4; ++l)
                in[i][l] = (uint32_t)(i * 0x9E3779B1u + l * 0x85EBCA6Bu);
            uint32x4_t r = al3_fetch_worker_neon(vld1q_u32(in[i]), (uint32_t)i);
            vst1q_u32(golden[i], r);
        }
        do {
            al3_flush_icache(reinterpret_cast<const void *>(&al3_fetch_worker_neon),
                             AL3_FETCH_FLUSH_BYTES);
            for (int i = 0; i < AL3_FETCH_NUM_INPUTS; ++i) {
                uint32x4_t r = al3_fetch_worker_neon(vld1q_u32(in[i]), (uint32_t)i);
                uint32x4_t g = vld1q_u32(golden[i]);
                uint32x4_t diff = veorq_u32(r, g);
                if (vmaxvq_u32(diff) != 0) {
                    uint32_t out[4];
                    vst1q_u32(out, r);
                    al3_fail(d, "NEON execution differs from init snapshot "
                               "after I-cache reload: input %d: expected "
                               "{%08x,%08x,%08x,%08x} got {%08x,%08x,%08x,"
                               "%08x} (fetch-path corruption?)", i,
                             golden[i][0], golden[i][1], golden[i][2], golden[i][3],
                             out[0], out[1], out[2], out[3]);
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
    } else {
        double in_a[AL3_FETCH_NUM_INPUTS];
        double in_b[AL3_FETCH_NUM_INPUTS];
        uint64_t golden[AL3_FETCH_NUM_INPUTS];
        for (int i = 0; i < AL3_FETCH_NUM_INPUTS; ++i) {
            in_a[i] = (double)(i - 32) * 0.0625;
            in_b[i] = (double)((i * 7) % 13) * 0.25 - 1.5;
            double r = al3_fetch_worker_scalar(in_a[i], in_b[i]);
            memcpy(&golden[i], &r, sizeof(r));
        }
        do {
            al3_flush_icache(reinterpret_cast<const void *>(&al3_fetch_worker_scalar),
                             AL3_FETCH_FLUSH_BYTES);
            for (int i = 0; i < AL3_FETCH_NUM_INPUTS; ++i) {
                double r = al3_fetch_worker_scalar(in_a[i], in_b[i]);
                uint64_t bits;
                memcpy(&bits, &r, sizeof(bits));
                if (memcmp(&bits, &golden[i], sizeof(bits)) != 0) {
                    al3_fail(d, "execution differs from init snapshot after "
                               "I-cache reload: input %d (a=%.6e b=%.6e): "
                               "expected 0x%016llx got 0x%016llx (fetch-path "
                               "corruption?)", i, in_a[i], in_b[i],
                             (unsigned long long)golden[i],
                             (unsigned long long)bits);
                    return EXIT_FAILURE;
                }
            }
        } while (test_time_condition(test));
    }
    return EXIT_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Per-child plumbing: shm, combo pick, covfile trace                  */
/* ------------------------------------------------------------------ */

static const char *al3_pattern_name(enum AL3Pattern p)
{
    switch (p) {
        case AL3_SEQ_READ:    return "seq_read";
        case AL3_RAND_READ:   return "rand_read";
        case AL3_STRIDE_READ: return "stride_read";
        case AL3_WRITE:       return "write";
        case AL3_COPY:        return "copy";
        case AL3_RMW:         return "rmw";
        default:              return "fetch";
    }
}

static const char *al3_dp_name(enum AL3Datapath dp)
{
    switch (dp) {
        case DP_SCALAR: return "scalar";
        case DP_NEON:   return "neon";
        default:        return "fp64";
    }
}

/* per-cluster L3 size in bytes (3-step probe, memcpy_l3_cache_size
 * convention; MUST be called from test_init — topology filled by then,
 * a static/global probe runs before main and silently falls back) */
static size_t al3_l3_size(void)
{
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

/* Map (or create) the named shm; creator publishes the anchor.
 * Latecomers fstat-gate on size BEFORE mmap and stay pure observers of
 * the object (only the creator sizes it): mmap of a 0-byte tmpfs file
 * succeeds with a bare VMA and the first ready-spin touch then raises
 * SIGBUS/BUS_ADRERR while the creator is between shm_open and ftruncate,
 * or after a creator died there — same 2026-11-24 root cause as
 * adaptive_mem_sdc's shm_setup (board fail + V1/V3 repro). */
static struct AL3Shm *al3_shm_setup(void)
{
    const char *key = getenv("ADAPTIVE_L3_SDC_KEY");
    char name[128];
    if (key) {
        snprintf(name, sizeof(name), "/adaptive_l3_sdc_%s", key);
    } else {
        /* derive from the main process pid: every child of one run
         * shares the name; a later ad-hoc run gets a fresh object */
        snprintf(name, sizeof(name), "/adaptive_l3_sdc_ppid%ld", (long)getppid());
    }

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    bool creator = (fd >= 0);
    if (!creator) {
        if (errno != EEXIST)
            return nullptr;
        fd = shm_open(name, O_RDWR, 0600);
        if (fd < 0)
            return nullptr;
        /* wait until the creator has sized the object: an undersized
         * mapping would fault on first touch (see header comment).
         * Healthy path costs one fstat; the 500000 x 10us bound is 5s
         * in nominal arithmetic but ~31s measured on the dev host
         * (timer slack makes usleep(10) cost ~60us) and stretches
         * further under load — on timeout return nullptr (the pick
         * path idles the child). */
        struct stat st;
        bool sized = false;
        for (int i = 0; i < 500000; i++) {
            if (fstat(fd, &st) == 0 && st.st_size >= (off_t)sizeof(struct AL3Shm)) {
                sized = true;
                break;
            }
            usleep(10);
        }
        if (!sized) {
            close(fd);
            return nullptr;
        }
    }
    if (creator && ftruncate(fd, sizeof(struct AL3Shm)) != 0) {
        close(fd);
        return nullptr;
    }
    auto *shm = static_cast<struct AL3Shm *>(
            mmap(nullptr, sizeof(struct AL3Shm), PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd, 0));
    close(fd);
    if (shm == MAP_FAILED)
        return nullptr;
    if (creator) {
        memset(shm, 0, sizeof(*shm));
        clock_gettime(CLOCK_MONOTONIC, &shm->anchor);
        __atomic_store_n(&shm->ready, 1u, __ATOMIC_RELEASE);
    } else {
        for (int i = 0; i < 200000; i++) {
            if (__atomic_load_n(&shm->ready, __ATOMIC_ACQUIRE) == 1u)
                break;
            usleep(10);
        }
    }
    return shm;
}

/* rotation order: kPriorityOrder unless ADAPTIVE_L3_SDC_ORDER carries a
 * valid full permutation of 0..20 */
static int g_al3_rot_order[AL3_COMBO_COUNT];

static void al3_order_setup(void)
{
    memcpy(g_al3_rot_order, kPriorityOrder, sizeof(g_al3_rot_order));
    const char *spec = getenv("ADAPTIVE_L3_SDC_ORDER");
    if (spec == nullptr)
        return;

    int tmp[AL3_COMBO_COUNT];
    bool seen[AL3_COMBO_COUNT] = { false };
    int n = 0;
    const char *p = spec;
    while (*p != '\0') {
        char *end = nullptr;
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v >= AL3_COMBO_COUNT || seen[v])
            goto bad;
        seen[v] = true;
        if (n < AL3_COMBO_COUNT)
            tmp[n++] = (int)v;
        if (*end == '\0')
            break;
        if (*end != ',')
            goto bad;
        p = end + 1;
    }
    if (n != AL3_COMBO_COUNT)
        goto bad;
    memcpy(g_al3_rot_order, tmp, sizeof(g_al3_rot_order));
    return;
bad:
    fprintf(stderr, "adaptive_l3_sdc: ADAPTIVE_L3_SDC_ORDER must be a permutation of 0..%d, keeping default table order (ignoring '%s')\n",
            AL3_COMBO_COUNT - 1, spec);
}

static int64_t al3_seconds_since(const struct timespec *anchor, struct timespec *now)
{
    clock_gettime(CLOCK_MONOTONIC, now);
    int64_t s = (int64_t)(now->tv_sec - anchor->tv_sec);
    if (now->tv_nsec < anchor->tv_nsec)
        s -= 1;
    return s < 0 ? 0 : s;
}

/* Pick this child's combo; -1 = idle (combat post-SDC, survey complete,
 * shm unusable).  Per-CHILD (test_init), so all threads of the child
 * run the same combo and shared buffers (SEQ array, RMW window) are
 * pattern-correct by construction. */
static int al3_pick_combo(struct AL3Shm *shm, int test_mode, int test_slice)
{
    if (!shm)
        return -1;
    /* combat only: first SDC freezes every later child to idle */
    if (!test_mode &&
        __atomic_load_n(&shm->sdc_flag, __ATOMIC_ACQUIRE) != 0)
        return -1;
    if (__atomic_load_n(&shm->ready, __ATOMIC_ACQUIRE) != 1u)
        return -1;

    const char *force = getenv("ADAPTIVE_L3_SDC_COMBO");
    if (force != nullptr) {
        int c = atoi(force);
        if (c >= 0 && c < AL3_COMBO_COUNT)
            return c;
    }
    if (getenv("ADAPTIVE_L3_SDC_VERIFY") != nullptr) {
        uint32_t prev = __atomic_fetch_add(&shm->verify_ctr, 1u, __ATOMIC_RELAXED);
        return (int)(prev % AL3_COMBO_COUNT);
    }

    struct timespec now;
    if (test_mode) {
        int64_t elapsed = al3_seconds_since(&shm->anchor, &now);
        int64_t idx = elapsed / test_slice;
        if (idx >= AL3_COMBO_COUNT)
            return -1;
        return g_al3_rot_order[idx];
    }
    int64_t elapsed = al3_seconds_since(&shm->anchor, &now);
    int64_t idx = elapsed / AL3_SLICE_SECONDS;
    if (idx > AL3_COMBO_COUNT - 1)
        idx = AL3_COMBO_COUNT - 1;
    return g_al3_rot_order[idx];
}

/* one line per child: "<iso> <pid> <dp_tag> <mode> <combo>" */
static void al3_covfile_trace(int combo)
{
    const char *path = getenv("ADAPTIVE_L3_SDC_COVFILE");
    if (!path || combo < 0)
        return;
    char line[160];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char iso[32];
    strftime(iso, sizeof(iso), "%FT%T", &tm);
    snprintf(line, sizeof(line), "%s %d %s %s %d\n", iso, (int)getpid(),
             al3_dp_name(kComboTable[combo].dp),
             getenv("ADAPTIVE_L3_SDC_VERIFY") ? "verify"
                 : (getenv("ADAPTIVE_L3_SDC_MODE") && strcmp(getenv("ADAPTIVE_L3_SDC_MODE"), "test") == 0)
                     ? "test" : "normal",
             combo);
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0)
        return;
    (void)!write(fd, line, strlen(line));
    close(fd);
}

static int adaptive_l3_sdc_init(struct test *test)
{
    /* throwing-new OOM follows the repo T10 convention (cf.
     * gemm_cdouble_dynamic_square): bad_alloc -> clean SKIP. */
    struct AL3Data *d;
    try {
        d = new struct AL3Data;
    } catch (const std::bad_alloc &) {
        log_skip(RuntimeSkipCategory, "memory: %s AL3Data", test->id);
        return EXIT_SKIP;
    }
    memset(d, 0, sizeof(*d));

    d->shm = al3_shm_setup();
    d->l3_size = al3_l3_size();
    d->test_mode = 0;
    d->test_slice = 600;
    const char *mode = getenv("ADAPTIVE_L3_SDC_MODE");
    if (mode != nullptr) {
        if (strcmp(mode, "test") == 0)
            d->test_mode = 1;
        else if (strcmp(mode, "combat") != 0)
            fprintf(stderr, "adaptive_l3_sdc: ADAPTIVE_L3_SDC_MODE must be combat|test, ignoring '%s'\n", mode);
    }
    const char *ts = getenv("ADAPTIVE_L3_SDC_TEST_SLICE");
    if (ts != nullptr) {
        int v = atoi(ts);
        if (v >= 1)
            d->test_slice = v;
        else
            fprintf(stderr, "adaptive_l3_sdc: ADAPTIVE_L3_SDC_TEST_SLICE must be >=1 second, keeping default 600\n");
    }
    al3_order_setup();

    d->combo = al3_pick_combo(d->shm, d->test_mode, d->test_slice);
    al3_covfile_trace(d->combo);

    if (d->combo < 0) {
        test->data = d;             /* idle child; run returns immediately */
        return EXIT_SUCCESS;
    }

    const struct AL3ComboDef &cd = kComboTable[d->combo];

    /* combo geometry tag for fail reports (pattern/dp/dynamic geometry) */
    char geom[48];
    switch (cd.pattern) {
        case AL3_SEQ_READ:    snprintf(geom, sizeof(geom), "4MB_shared"); break;
        case AL3_RAND_READ:   snprintf(geom, sizeof(geom), "l3_%zuMB", d->l3_size >> 20); break;
        case AL3_STRIDE_READ: snprintf(geom, sizeof(geom), "l3_%zuMB_128B", d->l3_size >> 20); break;
        case AL3_WRITE:       snprintf(geom, sizeof(geom), "4MB_priv"); break;
        case AL3_COPY:        snprintf(geom, sizeof(geom), "1.5xl3_%zuMB", d->l3_size >> 20); break;
        case AL3_RMW:         snprintf(geom, sizeof(geom), "64MB_band"); break;
        default:              snprintf(geom, sizeof(geom), "civac_ivau"); break;
    }
    snprintf(d->tag, sizeof(d->tag), "combo#%d [%s/%s/%s]",
             d->combo, al3_pattern_name(cd.pattern), al3_dp_name(cd.dp), geom);

    /* pattern-shared buffers, filled once per child */
    if (cd.pattern == AL3_SEQ_READ) {
        if (cd.dp == DP_FP64) {
            auto *buf = static_cast<double *>(
                    aligned_alloc(16, AL3_SEQ_DOUBLES * sizeof(double)));
            auto *g = static_cast<double *>(
                    aligned_alloc(16, AL3_SEQ_DOUBLES * sizeof(double)));
            if (!buf || !g) {
                free(buf); free(g);
                delete d;
                return EXIT_FAILURE;
            }
            for (size_t i = 0; i < AL3_SEQ_DOUBLES; ++i) {
                g[i] = buf[i] = al3_dist_f64();
            }
            d->seq_buf = buf;
            d->seq_golden_buf = g;
        } else {
            auto *buf = static_cast<int32_t *>(
                    aligned_alloc(16, AL3_SEQ_INTS * sizeof(int32_t)));
            auto *g = static_cast<int32_t *>(
                    aligned_alloc(16, AL3_SEQ_INTS * sizeof(int32_t)));
            if (!buf || !g) {
                free(buf); free(g);
                delete d;
                return EXIT_FAILURE;
            }
            for (size_t i = 0; i < AL3_SEQ_INTS; ++i) {
                g[i] = buf[i] = al3_dist_int();
            }
            d->seq_buf = buf;
            d->seq_golden_buf = g;
        }
    } else if (cd.pattern == AL3_RMW) {
        d->rmw_mem = new uint8_t[AL3_RMW_BYTES]();   /* zero-initialized */
        if (!d->rmw_mem) {
            free(d->seq_buf);
            delete d;
            return EXIT_FAILURE;
        }
    }

    test->data = d;
    return EXIT_SUCCESS;
}

static int adaptive_l3_sdc_run(struct test *test, int cpu)
{
    auto *d = static_cast<struct AL3Data *>(test->data);
    if (!d || d->combo < 0)
        return EXIT_SUCCESS;         /* post-SDC / survey complete / shm unusable */

    static int (*const engine_fns[AL3_PATTERN_COUNT])(struct test *, struct AL3Data *, int) = {
        &al3_seq_read_run,
        &al3_rand_read_run,
        &al3_stride_read_run,
        &al3_write_run,
        &al3_copy_run,
        &al3_rmw_run,
        &al3_fetch_run,
    };
    return engine_fns[kComboTable[d->combo].pattern](test, d, cpu);
}

static int adaptive_l3_sdc_cleanup(struct test *test)
{
    auto *d = static_cast<struct AL3Data *>(test->data);
    if (d) {
        free(d->seq_buf);
        free(d->seq_golden_buf);
        delete[] d->rmw_mem;
        delete d;
    }
    test->data = nullptr;
    return EXIT_SUCCESS;
}

#else /* !__aarch64__ — not registered outside the aarch64 list; stubs */

static int adaptive_l3_sdc_init(struct test *test) { (void)test; return EXIT_SKIP; }
static int adaptive_l3_sdc_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int adaptive_l3_sdc_cleanup(struct test *test) { (void)test; return EXIT_SUCCESS; }

#endif

DECLARE_TEST(adaptive_l3_sdc, "Adaptive L3-domain SDC test: 21-combo rotation (7 access patterns seq/rand/stride-read, write, copy, rmw, fetch x 3 datapaths scalar/NEON/FP64) over L3-scale windows probed per cluster; short-lived fork children pick one combo per wall-clock slice (shm anchor); MODE=combat (default: first SDC freezes the rotation) or MODE=test (survey: one TEST_SLICE window per combo, detections not frozen); VERIFY round-robin golden smoke; SDC report carries the combo tag (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = adaptive_l3_sdc_init,
    .test_run = adaptive_l3_sdc_run,
    .test_cleanup = adaptive_l3_sdc_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
