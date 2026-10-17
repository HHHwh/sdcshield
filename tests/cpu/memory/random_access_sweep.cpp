/**
 * @file random_access_sweep.cpp
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test random_access_sweep
 * @parblock
 * Random-access address-space sweep: each worker thread maps a large
 * anonymous region and, every iteration, picks a RANDOM offset and a
 * RANDOM block length (log-uniform from 64 B to 2 MiB, covering the
 * L1/L2/L3/DRAM scales), fills the block with random data, runs a NEON
 * read-modify-write transform over it, and verifies the result
 * byte-exactly against a same-iteration scalar golden.
 *
 * Rationale (SDC randomization hardening H5'): the suite's address
 * coverage was bimodal — either large-and-fixed (memcpy_l3's 8 MiB
 * fallback, mmu_stress_arm's 16 MiB walk) or small-and-random (mfence's
 * 32 KiB random index). NO test combined a large mmap with random
 * offsets AND varying block sizes, which is exactly the shape that
 * sweeps cache sets/ways, TLB entries and DRAM banks unpredictably —
 * the address dimension the CPU-122 hunt needs mapped.
 *
 * Knobs (-O random_access_sweep.<key>=<value>):
 *   size_mb=N    per-thread region size in MiB (default: memory-adaptive,
 *                min(512 MiB, MemAvailable*0.6/threads), floor 8 MiB)
 *   mode=SEQ     sequential block walk (address control; default RAND)
 *   valmode=PATTERN  0xAA/0x55 alternating fill instead of random
 *   valmode=SPECIALS high-Hamming 8-word table (like lsu_store_forward)
 * @endparblock
 */

#include <sandstone.h>
#include <test_knobs.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <vector>

#ifdef __aarch64__
#include <arm_neon.h>
#include <sys/mman.h>
#endif

/* ---- parameters ---- */
static constexpr size_t MIN_BLOCK = 64;            /* one cache line */
static constexpr size_t MAX_BLOCK = 2u << 20;      /* 2 MiB */
static constexpr size_t DEFAULT_SIZE_FLOOR = 8u << 20;    /* 8 MiB */
static constexpr size_t DEFAULT_SIZE_CAP  = 512u << 20;   /* 512 MiB */

enum AddrMode  { ADDR_RAND, ADDR_SEQ };
enum ValMode   { VAL_RANDOM, VAL_PATTERN, VAL_SPECIALS };

/* One region PER CPU: test_run executes concurrently on every worker
 * thread sharing the same test->data, so a single region would race
 * (fill/transform from two threads on overlapping blocks — observed as
 * immediate multi-thread failures). Same per-cpu pattern as
 * fisttp_arm's thread_ctx. */
struct RegionInfo {
    uint8_t *base;
    size_t   size;
};

struct SweepData {
    std::vector<RegionInfo> regions;   /* one per cpu */
    size_t region_size;                /* common size */
    /* knobs 在 init(main 线程)读取后缓存——get_testspecific_knob_value_int
     * 仅允许 init 调用，run 的 per-cpu worker 线程直接调用会触发框架
     * Internal error abort（2026-10-17 战役事件#002 实测捕获） */
    AddrMode addr_mode = ADDR_RAND;
    ValMode val_mode = VAL_RANDOM;
};

/* High-Hamming value table (same construction as lsu_store_forward_arm /
 * operand_space_arm: successive entries maximise data-path gate toggling).
 */
static const uint64_t SPECIALS_TABLE[] = {
    0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
    0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL,
    0x0F0F0F0F0F0F0F0FULL, 0xF0F0F0F0F0F0F0F0ULL,
    0x123456789ABCDEF0ULL, 0xFEDCBA9876543210ULL,
};

/* Read MemAvailable from /proc/meminfo, in KiB; -1 if unreadable.
 * (Same helper shape as memcpy_rewr.cpp.)
 */
static long read_mem_available_kib(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;
    long val = -1;
    char key[64];
    while (fscanf(f, "%63s %ld", key, &val) == 2) {
        if (strcmp(key, "MemAvailable:") == 0)
            break;
        val = -1;
        int c;
        while ((c = fgetc(f)) != EOF && c != '\n')
            ;
    }
    fclose(f);
    return val;
}

/* Memory-adaptive per-thread region size: 60% of MemAvailable split over
 * the worker threads, capped at 512 MiB, floored at 8 MiB. The 0.6
 * factor leaves headroom for the framework, the golden recompute and
 * the page cache (same conservative reasoning as svd_cdouble_sve's
 * per-worker RAM budget).
 */
static size_t default_region_bytes(void)
{
    long kib = read_mem_available_kib();
    if (kib <= 0)
        return DEFAULT_SIZE_FLOOR;
    int threads = thread_count();
    if (threads <= 0)
        threads = 1;
    double per_thread = (double)kib * 1024.0 * 0.6 / threads;
    size_t s = (size_t)per_thread;
    if (s > DEFAULT_SIZE_CAP)  s = DEFAULT_SIZE_CAP;
    if (s < DEFAULT_SIZE_FLOOR) s = DEFAULT_SIZE_FLOOR;
    return s;
}

/* Log-uniform random block length in [MIN_BLOCK, MAX_BLOCK]: pick the
 * exponent uniformly so every decade (L1/L2/L3/DRAM scale) is equally
 * likely, then the mantissa uniformly inside it. Result is 64-byte
 * aligned (whole cache lines).
 */
static size_t random_block_len(void)
{
    /* exponents: 64=2^6 .. 2^21 → 16 buckets for the log scale */
    int bucket = (int)(random32() % 16);          /* 0..15 */
    size_t len = (size_t)1 << (6 + bucket);
    if (len > MAX_BLOCK)
        len = MAX_BLOCK;
    return len;
}

/* ---- the NEON read-modify-write transform under test ----
 * block[i] ^= rotl64(block[i] + K, 13)  — per 128-bit vector: a
 * load, an integer add, a rotate, a xor and a store, so both the LSU
 * and the vector ALU participate. The scalar golden below repeats the
 * SAME arithmetic independently (different instruction path).
 */
static constexpr uint64_t TWEAK_K = 0x9E3779B97F4A7C15ULL;

#ifdef __aarch64__
static void transform_block_neon(uint8_t *p, size_t len)
{
    const uint64x2_t vk = vdupq_n_u64(TWEAK_K);
    uint64_t *w = reinterpret_cast<uint64_t *>(p);
    size_t vec_count = len / 16;
    for (size_t i = 0; i < vec_count; ++i) {
        uint64x2_t v = vld1q_u64(&w[2 * i]);
        uint64x2_t t = vaddq_u64(v, vk);
        /* rotate each lane left by 13: t = (t << 13) | (t >> 51) */
        uint64x2_t sh  = vshlq_n_u64(t, 13);
        uint64x2_t shr = vshrq_n_u64(t, 51);
        uint64x2_t rot = vorrq_u64(sh, shr);
        vst1q_u64(&w[2 * i], veorq_u64(v, rot));
    }
}
#endif

static void transform_block_scalar(uint8_t *p, size_t len)
{
    uint64_t *w = reinterpret_cast<uint64_t *>(p);
    size_t n = len / 8;
    for (size_t i = 0; i < n; ++i) {
        uint64_t v = w[i];
        uint64_t t = v + TWEAK_K;
        w[i] = v ^ ((t << 13) | (t >> 51));
    }
}

/* ---- test lifecycle ---- */

static int random_access_sweep_init(struct test *test)
{
#ifdef __aarch64__
    auto *data = new SweepData;
    if (!data)
        return EXIT_FAILURE;

    /* knobs */
    int64_t size_knob = get_testspecific_knob_value_int(test, "size_mb", 0);
    data->region_size = (size_knob > 0)
                       ? (size_t)size_knob * (1u << 20)
                       : default_region_bytes();
    /* address/value mode 旋钮同样只能在 init 读取（见 SweepData 注释） */
    {
        int64_t m = get_testspecific_knob_value_int(test, "mode", 0);
        if (m == 1)
            data->addr_mode = ADDR_SEQ;
    }
    {
        int64_t v = get_testspecific_knob_value_int(test, "valmode", 0);
        if (v == 1)
            data->val_mode = VAL_PATTERN;
        else if (v == 2)
            data->val_mode = VAL_SPECIALS;
    }
    if (data->region_size > DEFAULT_SIZE_CAP)
        data->region_size = DEFAULT_SIZE_CAP;
    if (data->region_size < MAX_BLOCK * 2)
        data->region_size = MAX_BLOCK * 2;    /* must fit one max block + slack */

    int ncpus = num_cpus();
    if (ncpus <= 0)
        ncpus = 1;
    data->regions.resize(ncpus);
    for (int i = 0; i < ncpus; ++i) {
        RegionInfo &r = data->regions[i];
        r.size = data->region_size;
        r.base = static_cast<uint8_t *>(
            mmap(nullptr, r.size, PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0));
        if (r.base == MAP_FAILED) {
            int err = errno;
            /* unmap what we already mapped */
            for (int j = 0; j < i; ++j)
                munmap(data->regions[j].base, data->regions[j].size);
            log_skip(TestResourceIssueSkipCategory,
                     "random_access_sweep: mmap of %zu bytes failed: %s",
                     r.size, strerror(err));
            delete data;
            return EXIT_SKIP;
        }
#ifdef MADV_HUGEPAGE
        /* THP=always would give this anyway on most distros; ask explicitly
         * so the sweep also covers huge-page TLB pressure when available. */
        madvise(r.base, r.size, MADV_HUGEPAGE);
#endif
    }

    test->data = data;
    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM64 NEON required for random_access_sweep");
    return EXIT_SKIP;
#endif
}

static int random_access_sweep_run(struct test *test, int cpu)
{
#ifdef __aarch64__
    auto *data = static_cast<SweepData *>(test->data);
    if (cpu < 0 || static_cast<size_t>(cpu) >= data->regions.size())
        cpu = 0;   /* defensive; framework always passes a valid cpu */
    RegionInfo &region = data->regions[cpu];
    uint8_t *base = region.base;
    const size_t size = region.size;

    /* knobs 已在 init 缓存到 data（本函数为 per-cpu worker 线程，
     * 禁止调用 get_testspecific_knob_value_int） */
    AddrMode addr_mode = data->addr_mode;
    ValMode val_mode = data->val_mode;

    /* Sequential-mode cursor, 64-byte stepped so every cache line is
     * eventually visited in-order (address control for attribution). */
    uint64_t seq_cursor = 0;

    /* Golden scratch: reused across iterations; one per thread. */
    std::vector<uint8_t> golden(MAX_BLOCK);

    do {
        /* 1. random offset + random block length (64-byte aligned) */
        size_t len = random_block_len();
        size_t span = size - len;
        size_t off;
        if (addr_mode == ADDR_SEQ) {
            off = (size_t)(seq_cursor % (span + 1));
            seq_cursor += len;
        } else {
            off = ((size_t)random64() % (span / MIN_BLOCK + 1)) * MIN_BLOCK;
        }

        uint8_t *block = base + off;

        /* 2. fill the block (per valmode) */
        switch (val_mode) {
        case VAL_RANDOM:
            memset_random(block, len);
            break;
        case VAL_PATTERN: {
            uint64_t *w = reinterpret_cast<uint64_t *>(block);
            for (size_t i = 0; i < len / 8; ++i)
                w[i] = (i & 1) ? 0x5555555555555555ULL
                               : 0xAAAAAAAAAAAAAAAAULL;
            break;
        }
        case VAL_SPECIALS: {
            uint64_t *w = reinterpret_cast<uint64_t *>(block);
            for (size_t i = 0; i < len / 8; ++i)
                w[i] = SPECIALS_TABLE[i % 8];
            break;
        }
        }

        /* 3. same-iteration golden: snapshot, transform with the
         *    independent scalar path, compare against the NEON result
         *    (reproduce-ironclad: golden and result derive from the
         *    same fill, computed this iteration by this thread). */
        memcpy(golden.data(), block, len);

        transform_block_neon(block, len);
        transform_block_scalar(golden.data(), len);

        if (memcmp(block, golden.data(), len) != 0) {
            size_t first_bad = 0;
            while (first_bad < len && block[first_bad] == golden.data()[first_bad])
                ++first_bad;
            report_fail_msg("random_access_sweep: transform mismatch at "
                            "offset %zu+%zu (len %zu) byte %zu: "
                            "got %02X want %02X",
                            off, first_bad, len, first_bad,
                            block[first_bad], golden.data()[first_bad]);
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#else
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM64 NEON required for random_access_sweep");
    return EXIT_SKIP;
#endif
}

static int random_access_sweep_cleanup(struct test *test)
{
#ifdef __aarch64__
    auto *data = static_cast<SweepData *>(test->data);
    if (data) {
        for (RegionInfo &r : data->regions) {
            if (r.base && r.base != MAP_FAILED)
                munmap(r.base, r.size);
        }
        delete data;
        test->data = nullptr;
    }
#else
    (void)test;
#endif
    return EXIT_SUCCESS;
}

DECLARE_TEST(random_access_sweep,
             "Random-access sweep over a large mmap: random offset + log-uniform block length (64B..2MB), NEON RMW transform verified byte-exact against a same-iteration scalar golden")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = random_access_sweep_init,
    .test_run = random_access_sweep_run,
    .test_cleanup = random_access_sweep_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
