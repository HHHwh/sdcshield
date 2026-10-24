/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mmu_split_tlb_sve
 * @parblock
 * MMU / TLB / Page-Table-Walker SDC stress — the coverage-weakest unit (MMU
 * 20%). The existing mmu_stress_arm covers forced TLB-miss (MADV_DONTNEED)
 * and DTLB thrash, but leaves the MMU's two hardest SDC surfaces
 * unexercised:
 *
 *   1. The **translation-coherence / alias** surface: one physical page
 *      visible through several virtual addresses (page-color / ASID aliasing).
 *      A store through alias A must be observable through alias B; a fault in
 *      the STLB / snoop / coherency between aliased translations silently
 *      breaks this. mmu_stress_arm never aliased a page, so this path (shared
 *      page, divergent VAs) was never touched.
 *   2. The **dual-TLB-lookup per single instruction** surface at a boundary
 *      that is *also* a cache-line boundary, not just a page boundary. mmu_stress_arm
 *      straddles the 4K page boundary at page_end-4 (8B access, 4B in each page)
 *      — two TLB lookups, one cache line each (the access fits inside a 64B
 *      line, so no cache-line split). It never placed the straddling access on a
 *      cache-line boundary too, so the MMU<->LSU split-access interaction at
 *      a *line* boundary inside a *page* boundary was never hit.
 *
 * This test adds exactly those two surfaces, plus a large-ASID pressure sweep:
 *
 *   - **Page-color aliasing**: back a single 4K anonymous page with a memfd,
 *     mmap it N times at distinct VAs (MAP_SHARED, same fd, offset 0). Every
 *     alias must see writes made through any other alias — byte-exact. We
 *     write a high-Hamming golden through alias 0 and read it back through
 *     every other alias; any mismatch is an MMU/translation-coherency SDC.
 *   - **Dual-lookup at a combined line+page boundary**: a single 8B unaligned
 *     access whose address is page_end-4 sits 4B in each page; on Kunpeng 920
 *     a 4K page is 64 cache lines of 64B, so page_end-4 is also line_end-4
 *     only when page_end aligns to a line — which it always does (4K = 64*64B).
 *     To additionally stress the *cross-cache-line split* path independently
 *     of the page boundary, we also place a 16B access at line_end-8 (8B in
 *     each of two lines) fully *inside* one page, forcing the LSU/MMU
 *     split-access path with a single TLB lookup. Both are byte-exact checked.
 *   - **Large-ASID TLB pressure**: a working set of ~8192 pages (32 MB,
 *     ~128x a typical DTLB) swept with a stride that touches one cache line
 *     per page; the sweep evicts resident TLB entries and replays the page
 *     table walker, stressing the PTW state machine under alias pressure.
 *
 * ARM64-native (inline-asm str/ldr + linux mmap/memfd); on non-aarch64 it
 * returns a clean EXIT_SKIP. Wired into the arm64 subdir (aarch64-only guard),
 * so x86-64 is untouched.
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"
#include <cstdint>
#include <cinttypes>
#include <cstring>
#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif
#include <cerrno>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>

// Number of distinct virtual aliases of the one physical page. Enough aliases
// to keep several TLB entries pointing at the same physical frame concurrently
// (the aliasing/coherency surface only manifests with >=2 concurrent
// translations of the same frame).
static constexpr int NUM_ALIASES = 8;

// High-Hamming golden table (same construction as mmu_stress_arm / operand
// space: successive entries maximise toggling in the data-path gates).
static const uint64_t GOLDEN_TABLE[] = {
    0x0000000000000000ULL,
    0xFFFFFFFFFFFFFFFFULL,
    0xAAAAAAAAAAAAAAAAULL,
    0x5555555555555555ULL,
    0x0F0F0F0F0F0F0F0FULL,
    0xF0F0F0F0F0F0F0F0ULL,
};
static constexpr size_t GOLDEN_TABLE_SIZE =
    sizeof(GOLDEN_TABLE) / sizeof(GOLDEN_TABLE[0]);

#if defined(__aarch64__)
struct MmuSplitTlbSveData {
    size_t pressure_pages;   /* 每线程 TLB 压力扫描页数(init 定容, 线程共享) */
};

/* 压力扫描页数(原 8192 页 = 4K 页 × 32MB/线程跨度) — 统一优先级链:
 *   1. -O mmu_split_tlb_sve.pressure_pages=N / 配置文件同名条目
 *   2. 环境变量 SDC_MMU_SPLIT_TLB_SVE_PRESSURE_PAGES
 *   3. 自适应: 每线程一个 pressure_pages 页的私有 mmap 压力跨度,
 *      页数 = 60% 物理内存(2 个大内存消费者均摊) / (线程数 × 4KB),
 *      除后 clamp [8192, 1M]。NUM_ALIASES / GOLDEN_TABLE 不动。 */
static size_t mmu_split_tlb_sve_pressure_pages(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "pressure_pages", 0, 8192, 1 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("mmu_split_tlb_sve", "pressure_pages", &v, 8192, 1 << 20);
    if (v > 0)
        return (size_t)v;
    uint64_t budget = sdc_target_workset_bytes(600, 2, nullptr);
    int64_t pages = (int64_t)(budget / ((uint64_t)thread_count() * 4096));
    if (pages < 8192)
        pages = 8192;
    if (pages > (1 << 20))
        pages = 1 << 20;
    return (size_t)pages;
}
#endif

#if defined(__aarch64__)
// ARM64 inline-asm 64-bit load/store — keeps the access type visible to the
// toolchain (real str/ldr micro-ops through the LSU/MMU).
static inline void mmu_store64(void *addr, uint64_t val)
{
    __asm__ volatile("str %1, [%0]" : : "r"(addr), "r"(val) : "memory");
}
static inline uint64_t mmu_load64(const void *addr)
{
    uint64_t res;
    __asm__ volatile("ldr %0, [%1]" : "=r"(res) : "r"(addr) : "memory");
    return res;
}
// 128-bit (16B) load/store pair — a single instruction moving 16B; when the
// address is line_end-8 it straddles two 64B cache lines, exercising the
// LSU/MMU split-access path for a *single* translation.
static inline void mmu_store128_sve(void *addr, uint64_t lo, uint64_t hi)
{ uint64_t p[2] = {lo, hi}; svbool_t pg = svwhilelt_b64(0, 2);
  svst1_u64(pg, (uint64_t *)addr, svld1_u64(pg, p)); }
static inline uint64_t mmu_load128_lo(const void *addr)
{
    uint64_t lo, hi;
    __asm__ volatile("ldp %0, %1, [%2]" : "=r"(lo), "=r"(hi) : "r"(addr) : "memory");
    return lo;
}
static inline uint64_t mmu_load128_hi(const void *addr)
{
    uint64_t lo, hi;
    __asm__ volatile("ldp %0, %1, [%2]" : "=r"(lo), "=r"(hi) : "r"(addr) : "memory");
    return hi;
}
#else
static inline void mmu_store64(void *addr, uint64_t val)
{
    std::memcpy(addr, &val, sizeof(val));
}
static inline uint64_t mmu_load64(const void *addr)
{
    uint64_t v;
    std::memcpy(&v, addr, sizeof(v));
    return v;
}
static inline void mmu_store128_sve(void *addr, uint64_t lo, uint64_t hi)
{
    uint64_t pair[2] = {lo, hi};
    std::memcpy(addr, pair, sizeof(pair));
}
static inline uint64_t mmu_load128_lo(const void *addr)
{
    uint64_t pair[2];
    std::memcpy(pair, addr, sizeof(pair));
    return pair[0];
}
static inline uint64_t mmu_load128_hi(const void *addr)
{
    uint64_t pair[2];
    std::memcpy(pair, addr, sizeof(pair));
    return pair[1];
}
#endif

static int mmu_split_tlb_sve_init(struct test *test)
{
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
     * layer: arm64_require_sve_vl does the SVE capability check
     * + 128..1024-bit range gate in one call (skip reason
     * framework-issued). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
    auto *data = static_cast<MmuSplitTlbSveData *>(malloc(sizeof(MmuSplitTlbSveData)));
    if (!data)
        return EXIT_FAILURE;
    data->pressure_pages = mmu_split_tlb_sve_pressure_pages(test);
    test->data = data;
#endif
    (void)test;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__

// Create N distinct virtual mappings of one 4K anonymous physical page using a
// memfd as the backing store. Returns the alias pointers in `out` and the
// memfd fd via `out_fd` (caller closes it after munmap). Returns 0 on success.
static int make_aliased_page(void *out[NUM_ALIASES], int *out_fd, size_t page_size)
{
    int fd = (int)syscall(SYS_memfd_create, "mmu_split_tlb", 0U);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)page_size) != 0) {
        close(fd);
        return -1;
    }
    for (int i = 0; i < NUM_ALIASES; ++i) {
        void *p = mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            for (int j = 0; j < i; ++j)
                munmap(out[j], page_size);
            close(fd);
            return -1;
        }
        out[i] = p;
    }
    *out_fd = fd;
    return 0;
}

static int mmu_split_tlb_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MmuSplitTlbSveData *>(test->data);
    const size_t pressure_pages = data->pressure_pages;

    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    size_t page_size = (size_t)ps;

    void *aliases[NUM_ALIASES] = {};
    int memfd = -1;
    if (make_aliased_page(aliases, &memfd, page_size) != 0) {
        log_skip(OSResourceIssueSkipCategory,
                 "mmu_split_tlb_sve: aliased-page setup failed: %s",
                 strerror(errno));
        return EXIT_SKIP;
    }

    // Large-ASID pressure region: per-thread private so no cross-thread
    // madvise races (mirrors mmu_stress_arm's lesson).
    size_t pressure_size = pressure_pages * page_size;
    uint8_t *pressure = static_cast<uint8_t *>(
        mmap(nullptr, pressure_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (pressure == MAP_FAILED) {
        for (int i = 0; i < NUM_ALIASES; ++i)
            munmap(aliases[i], page_size);
        close(memfd);
        log_skip(OSResourceIssueSkipCategory,
                 "mmu_split_tlb_sve: pressure mmap failed: %s",
                 strerror(errno));
        return EXIT_SKIP;
    }

    do {
        bool all_passed = true;

        // ---- (1) Page-color aliasing. Write a distinct golden through alias 0
        // each cycle, then read it back through every other alias. Any
        // mismatch is an MMU/translation-coherency (STLB/snoop/alias) SDC.
        // Cycle the golden so the data-path gates also toggle across cycles.
        static uint64_t golden_cycle = 0;
        uint64_t golden = GOLDEN_TABLE[golden_cycle % GOLDEN_TABLE_SIZE];
        golden_cycle = (golden_cycle + 1) % GOLDEN_TABLE_SIZE;

        mmu_store64(aliases[0], golden);
        // Read back through alias 0 itself first (sanity), then every alias.
        for (int a = 0; a < NUM_ALIASES; ++a) {
            uint64_t rd = mmu_load64(aliases[a]);
            if (rd != golden) {
                log_warning("mmu_split_tlb_sve: alias %d read 0x%016" PRIx64
                            " want 0x%016" PRIx64 " (alias-coherency SDC)",
                            a, rd, golden);
                all_passed = false;
                break;
            }
        }
        if (!all_passed) {
            munmap(pressure, pressure_size);
            for (int i = 0; i < NUM_ALIASES; ++i)
                munmap(aliases[i], page_size);
            close(memfd);
            report_fail_msg("mmu_split_tlb_sve: page-alias translation SDC detected");
            return EXIT_FAILURE;
        }

        // Also alias a 16B value, to exercise 128-bit translation coherence
        // (a single stp/ldp through alias 0, read through others).
        uint64_t g_lo = GOLDEN_TABLE[(golden_cycle + 1) % GOLDEN_TABLE_SIZE];
        uint64_t g_hi = GOLDEN_TABLE[(golden_cycle + 3) % GOLDEN_TABLE_SIZE];
        mmu_store128_sve(aliases[0], g_lo, g_hi);
        for (int a = 1; a < NUM_ALIASES; ++a) {
            uint64_t rlo = mmu_load128_lo(aliases[a]);
            uint64_t rhi = mmu_load128_hi(aliases[a]);
            if (rlo != g_lo || rhi != g_hi) {
                log_warning("mmu_split_tlb_sve: alias %d 128B mismatch "
                            "lo=0x%016" PRIx64 "/0x%016" PRIx64 " hi=0x%016" PRIx64
                            "/0x%016" PRIx64, a, rlo, g_lo, rhi, g_hi);
                all_passed = false;
                break;
            }
        }
        if (!all_passed) {
            munmap(pressure, pressure_size);
            for (int i = 0; i < NUM_ALIASES; ++i)
                munmap(aliases[i], page_size);
            close(memfd);
            report_fail_msg("mmu_split_tlb_sve: page-alias 128B translation SDC detected");
            return EXIT_FAILURE;
        }

        // ---- (2) Dual-TLB-lookup per single instruction at a page boundary.
        // Place an 8B access at page_end-4 of alias 0: 4B in this page, 4B in
        // the next page of the SAME memfd mapping. A single ldr/str of those
        // 8B forces two TLB lookups from one instruction. (We map the memfd
        // at 2*page_size so the "next page" is resident too.)
        // To make alias 0 2 pages, we re-map: simpler — use the pressure
        // region's first two contiguous pages for the cross-page probe (they
        // are guaranteed contiguous and resident-able).
        uint8_t *page0 = pressure;
        uint8_t *page1 = pressure + page_size;
        uint8_t *cross = page0 + page_size - 4; // 4B in page0, 4B in page1
        uint64_t crossv = GOLDEN_TABLE[(golden_cycle + 2) % GOLDEN_TABLE_SIZE]
                          ^ GOLDEN_TABLE[(golden_cycle + 4) % GOLDEN_TABLE_SIZE];
        mmu_store64(cross, crossv);
        uint64_t crd = mmu_load64(cross);
        if (crd != crossv) {
            log_warning("mmu_split_tlb_sve: cross-page 8B at %p mismatch "
                        "0x%016" PRIx64 " want 0x%016" PRIx64,
                        (void*)cross, crd, crossv);
            all_passed = false;
        }

        // ---- (3) Cross-cache-line split access INSIDE one page (single TLB
        // lookup, two cache lines). Place a 16B access at line_end-8 inside
        // page1: 8B in one 64B line, 8B in the next. A single stp/ldp straddles
        // the line boundary -> the LSU/MMU split-access path.
        // page1 begins at pressure+page_size (page-aligned). line_end-8 of the
        // first line of page1 is at page1 + 64 - 8 = page1 + 56.
        uint8_t *line_cross = page1 + 56; // 8B in line0, 8B in line1 of page1
        uint64_t lc_lo = GOLDEN_TABLE[(golden_cycle + 1) % GOLDEN_TABLE_SIZE];
        uint64_t lc_hi = GOLDEN_TABLE[(golden_cycle + 5) % GOLDEN_TABLE_SIZE];
        mmu_store128_sve(line_cross, lc_lo, lc_hi);
        uint64_t lc_rlo = mmu_load128_lo(line_cross);
        uint64_t lc_rhi = mmu_load128_hi(line_cross);
        if (lc_rlo != lc_lo || lc_rhi != lc_hi) {
            log_warning("mmu_split_tlb_sve: cross-line 16B at %p mismatch "
                        "lo=0x%016" PRIx64 "/0x%016" PRIx64 " hi=0x%016" PRIx64
                        "/0x%016" PRIx64, (void*)line_cross, lc_rlo, lc_lo,
                        lc_rhi, lc_hi);
            all_passed = false;
        }

        // ---- (4) Large-ASID TLB pressure sweep. Touch one cache line per
        // page across the whole pressure region; the working set far exceeds
        // DTLB capacity, so every access evicts a resident entry and replays
        // the page-table walker — real PTW torture, run under alias pressure.
        for (size_t p = 0; p < pressure_pages; ++p) {
            uint64_t sv = GOLDEN_TABLE[p % GOLDEN_TABLE_SIZE]
                          ^ (p * 0x9E3779B97F4A7C15ULL);
            mmu_store64(pressure + p * page_size, sv);
        }
        for (size_t p = 0; p < pressure_pages; ++p) {
            uint64_t sv = GOLDEN_TABLE[p % GOLDEN_TABLE_SIZE]
                          ^ (p * 0x9E3779B97F4A7C15ULL);
            uint64_t srd = mmu_load64(pressure + p * page_size);
            if (srd != sv) {
                log_warning("mmu_split_tlb_sve: pressure page %zu mismatch "
                            "0x%016" PRIx64 " want 0x%016" PRIx64, p, srd, sv);
                all_passed = false;
                break;
            }
        }

        if (!all_passed) {
            munmap(pressure, pressure_size);
            for (int i = 0; i < NUM_ALIASES; ++i)
                munmap(aliases[i], page_size);
            close(memfd);
            report_fail_msg("mmu_split_tlb_sve: MMU/TLB/PTW SDC detected");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    munmap(pressure, pressure_size);
    for (int i = 0; i < NUM_ALIASES; ++i)
        munmap(aliases[i], page_size);
    close(memfd);
    return EXIT_SUCCESS;
}
#else
static int mmu_split_tlb_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 MMU/TLB split-access "
             "(inline-asm str/ldr/stp/ldp + memfd aliasing) required");
    return EXIT_SKIP;
}
#endif

static int mmu_split_tlb_sve_finish(struct test *test)
{
#if defined(__aarch64__)
    if (test->data) {
        free(test->data);
        test->data = nullptr;
    }
#else
    (void)test;
#endif
    return EXIT_SUCCESS;
}

DECLARE_TEST(mmu_split_tlb_sve,
             "MMU/TLB/Page-Table-Walker SDC stress: page-color aliasing "
             "(one physical page, many VAs), dual-TLB-lookup cross-page and "
             "cross-cache-line split accesses, and large-ASID pressure sweep "
             "to torture the ARM64 translation path")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mmu_split_tlb_sve_init,
    .test_run = mmu_split_tlb_sve_run,
    .test_cleanup = mmu_split_tlb_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
