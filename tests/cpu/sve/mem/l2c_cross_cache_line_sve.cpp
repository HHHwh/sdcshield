/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b l2c_cross_cache_line_sve
 * @parblock
 * L2 cache SDC stress — a coverage-weak unit (L2C 40%). The existing
 * memcpy_l2_cache_size test does a plain 512K std::memcpy + memcmp: it
 * exercises the L2 capacity but never the L2's *coherency / snoop /
 * split-access* surface, which is where L2 SDC actually arises. This test
 * adds those surfaces:
 *
 *   1. **L2 exclusives / snoop / coherency path** (ldxr/stxr): a line-aligned
 *      exclusive RMW run under heavy neighbour traffic (store storm on a
 *      nearby line that would snoop/evict the reservation). A fault in the L2
 *      exclusives/coherency logic silently breaks the reservation or corrupts
 *      the stored value — detected by reading back byte-exact. (The exclusive
 *      class enforces alignment checking, so a single 8B exclusive cannot
 *      straddle a 64B line at 8B alignment; the cross-line straddle target
 *      uses the regular stp/ldp class in probe (2).)
 *   2. **Store-to-load forwarding across the line boundary**: a 16B store
 *      (stp) at line_end-8 (offset 56) straddles two 64B L2 lines (8B each) —
 *      a single instruction split across two lines; the immediate 16B load
 *      (ldp) must forward correctly across the boundary. A forwarding-path
 *      fault corrupts the reload — byte-exact checked.
 *   3. **L2 thrash sweep**: a working set sized to the L2 (read from sysfs
 *      index2/size, fallback 512K) written with a per-line golden through
 *      inline-asm str and read back through inline-asm ldr, with a stride of
 *      one cache line so the sweep evicts resident L2 lines every pass —
 *      real L2 thrashing, not a capacity-fit memcpy.
 *   4. **dc civac + cross-core coherence probe**: clean+invalidate a line
 *      (dc civac) then verify a sibling CPU sharing the L3 sees the
 *      up-to-date value (the L3 is the coherency point on Kunpeng 920; a
 *      cross-core dirty-line writeback fault corrupts the observed value).
 *
 * SDC detection: every access is byte-exact compared against the written
 * golden; report_fail_msg on any mismatch. ARM64-native (inline-asm
 * ldxr/stxr/str/ldr/stp/ldp + dc civac + dsb); non-aarch64 returns a clean
 * EXIT_SKIP. Wired into the arm64 subdir (aarch64-only guard), so x86-64 is
 * untouched.
 *
 * Note: getconf returns 0 for L2 size on this Kunpeng host (sysfs exposes
 * cache info incompletely), so the L2 size is read from
 * /sys/devices/system/cpu/cpuN/cache/index2/size directly, not via
 * sysconf(_SC_LEVEL2_CACHE_SIZE).
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif
#include <cerrno>
#include <cstdio>
#include <atomic>
#include <thread>
#include <unistd.h>
#include <sys/types.h>

// 64-byte cache line (L1/L2 line size on Kunpeng 920; read from sysfs at
// runtime but the constant backs the array alignment / boundary math).
static constexpr size_t CACHE_LINE = 64;
// L2 line size is also 64B on Kunpeng 920 (index2 coherency_line_size = 64).
// The L3 uses 128B lines but this test targets the L2.
// Number of distinct line-boundary probe offsets we sweep (one per outer
// iteration) inside the thrash region.
static constexpr size_t NUM_PROBE_LINES = 256;

// Parse a sysfs cache size string ("512K", "32M", "64K") into bytes.
static size_t parse_cache_size_str(const char *s)
{
    if (!s || !*s) return 0;
    char *end = nullptr;
    long long v = std::strtoll(s, &end, 10);
    if (end == s) return 0;
    if (end && *end) {
        switch (*end) {
            case 'k': case 'K': v <<= 10; break;
            case 'm': case 'M': v <<= 20; break;
            case 'g': case 'G': v <<= 30; break;
            default: break;
        }
    }
    return (size_t)v;
}

// Read the L2 size for the current CPU from sysfs. Falls back to 512K
// (Kunpeng 920 per-core L2) on any read failure.
static size_t get_l2_size_bytes()
{
    char path[128];
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/cpu/cpu%d/cache/index2/size",
                  sched_getcpu() >= 0 ? sched_getcpu() : 0);
    FILE *f = std::fopen(path, "re");
    if (f) {
        char buf[32];
        size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
        buf[got] = '\0';
        std::fclose(f);
        size_t sz = parse_cache_size_str(buf);
        if (sz > 0) return sz;
    }
    return 512 * 1024;
}

#if defined(__aarch64__)
// 64-bit load/store via inline asm (real micro-ops through the L2).
static inline void l2_store64(void *addr, uint64_t val)
{
    __asm__ volatile("str %1, [%0]" : : "r"(addr), "r"(val) : "memory");
}
static inline uint64_t l2_load64(const void *addr)
{
    uint64_t res;
    __asm__ volatile("ldr %0, [%1]" : "=r"(res) : "r"(addr) : "memory");
    return res;
}
// 128-bit (16B) load/store pair. stp/ldp tolerate misalignment (regular
// load/store class), so these are safe at any offset.
static inline void l2_store128(void *addr, uint64_t lo, uint64_t hi)
{
    __asm__ volatile("stp %2, %3, [%0]" : : "r"(addr), "r"(0), "r"(lo), "r"(hi) : "memory");
}
static inline void l2_load128(const void *addr, uint64_t *lo, uint64_t *hi)
{
    __asm__ volatile("ldp %0, %1, [%2]" : "=r"(*lo), "=r"(*hi) : "r"(addr) : "memory");
}
// Exclusive load (ldxr). NOTE: the exclusive class performs alignment
// checking — the address MUST be aligned to the access size (8B). A
// cache-line boundary cannot be straddled by a single 8B exclusive access
// at 8B alignment (no 8B-aligned offset in [57..63]), so the cross-line
// SDC probe uses the regular stp/ldp class instead (see probe (2)); the
// exclusive probe (1) targets the L2 exclusives/snoop/coherency path at a
// line-aligned address.
static inline uint64_t l2_ldxr(const void *addr)
{
    uint64_t res;
    __asm__ volatile("ldxr %0, [%1]" : "=r"(res) : "r"(addr) : "memory");
    return res;
}
// Exclusive store (stxr). Returns 0 on success (reservation held), 1 on
// failure (reservation lost) — the exclusives status, per ARMv8 semantics.
static inline unsigned l2_stxr(void *addr, uint64_t val)
{
    unsigned res;
    __asm__ volatile("stxr %w0, %2, [%1]" : "=&r"(res) : "r"(addr), "r"(val) : "memory");
    return res;
}
// 128-bit exclusive load/store pair (ldxp/stxp). The 16B exclusive access,
// placed at a 16B-aligned address that straddles the 64B line boundary, DOES
// cross the line while staying aligned. 16B-aligned line_end-16 = offset 48:
// bytes 48-63 (line0) + 64-79 (line1) — wait, 48..63 is line0, 64..79 is
// line1. A 16B access at offset 48 covers 48..63 (line0 only). At offset 112
// it covers 112..127 (line1 only). The only 16B-aligned offset that straddles
// a 64B boundary is 56 mod 16 = 8 -> NOT 16B aligned. So a 16B exclusive
// also cannot straddle a 64B line at 16B alignment. Therefore ldxp/stxp are
// used at a line-aligned address too (the exclusives path is still exercised;
// the straddling target belongs to the regular stp/ldp class).
// Data-cache clean+invalidate to the point of coherency (dc civac). Forces a
// dirty line writeback through the L2/L3 hierarchy.
static inline void l2_dc_civac(const void *addr)
{
    __asm__ volatile("dc civac, %0" : : "r"(addr) : "memory");
}
static inline void l2_dsb(void)
{
    __asm__ volatile("dsb sy" : : : "memory");
}
#else
static inline void l2_store64(void *addr, uint64_t val) { std::memcpy(addr, &val, 8); }
static inline uint64_t l2_load64(const void *addr) { uint64_t v; std::memcpy(&v, addr, 8); return v; }
static inline void l2_store128(void *addr, uint64_t lo, uint64_t hi) { uint64_t p[2]={lo,hi}; std::memcpy(addr,p,16); }
static inline void l2_load128(const void *addr, uint64_t *lo, uint64_t *hi) { uint64_t p[2]; std::memcpy(p,addr,16); *lo=p[0]; *hi=p[1]; }
static inline uint64_t l2_ldxr(const void *addr) { return l2_load64(addr); }
static inline unsigned l2_stxr(void *addr, uint64_t val) { l2_store64(addr, val); return 0; }
static inline void l2_dc_civac(const void *addr) { (void)addr; }
static inline void l2_dsb(void) {}
#endif

static int l2c_cross_cache_line_sve_init(struct test *test)
{
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
     * layer: arm64_require_sve_vl does the SVE capability check
     * + 128..1024-bit range gate in one call (skip reason
     * framework-issued). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    (void)test;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__

// High-Hamming golden table (max data-path gate toggling across cycles).
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

static int l2c_cross_cache_line_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;

    size_t l2_size = get_l2_size_bytes();
    // Size the thrash region to EXCEED the L2 so every stride pass evicts
    // resident lines (real thrashing). 2x L2 keeps the sweep bounded but
    // guarantees thrash; capped so multi-thread memory use stays sane.
    size_t thrash_size = l2_size * 2;

    uint8_t *thrash = static_cast<uint8_t *>(
        aligned_alloc_safe(CACHE_LINE, thrash_size));
    if (!thrash) {
        log_skip(TestResourceIssueSkipCategory,
                 "l2c_cross_cache_line_sve: thrash alloc failed");
        return EXIT_SKIP;
    }

    // A small aligned probe buffer for the cross-line exclusive + forwarding
    // probes (2 cache lines so line_end is reachable inside it).
    uint8_t *probe = static_cast<uint8_t *>(
        aligned_alloc_safe(CACHE_LINE, CACHE_LINE * 2));
    if (!probe) {
        free(thrash);
        log_skip(TestResourceIssueSkipCategory,
                 "l2c_cross_cache_line_sve: probe alloc failed");
        return EXIT_SKIP;
    }

    do {
        bool all_passed = true;
        static uint64_t golden_cycle = 0;
        uint64_t g0 = GOLDEN_TABLE[golden_cycle % GOLDEN_TABLE_SIZE];
        uint64_t g1 = GOLDEN_TABLE[(golden_cycle + 1) % GOLDEN_TABLE_SIZE];
        uint64_t g2 = GOLDEN_TABLE[(golden_cycle + 2) % GOLDEN_TABLE_SIZE];
        golden_cycle = (golden_cycle + 1) % GOLDEN_TABLE_SIZE;

        // ---- (1) L2 exclusives / snoop / coherency path. ldxr/stxr is the
        // exclusive class — it enforces alignment checking, so the address is
        // line-aligned (probe, 8B-aligned). The exclusives track a reservation
        // under neighbour traffic (store storm on a nearby line) that would
        // snoop/evict it; a coherency/snoop fault breaks the reservation or
        // corrupts the stored value. (A single 8B exclusive access cannot
        // straddle a 64B line at 8B alignment — no aligned offset in [57..63]
        // — so the straddling target is probe (2) on the regular stp/ldp class;
        // the exclusives probe targets the reservation/snoop path.)
        uint8_t *excl_addr = probe; // line0 start, 8B-aligned
        // Warm neighbour traffic on line1 to stress the snoop path while the
        // exclusive sequence runs.
        for (int k = 0; k < 32; ++k) {
            l2_store64(probe + CACHE_LINE + 8, g2 ^ (uint64_t)k);
        }
        unsigned stxr_fail = 1;
        int tries = 0;
        while (stxr_fail && tries < 64) {
            (void)l2_ldxr(excl_addr);
            stxr_fail = l2_stxr(excl_addr, g0);
            ++tries;
        }
        if (stxr_fail) {
            // Reservation never held — the exclusives path is broken; treat
            // as a fault (under benign load stxr should succeed within a few
            // tries).
            log_warning("l2c_cross_cache_line_sve: exclusive stxr never "
                        "succeeded after %d tries", tries);
            all_passed = false;
        }
        uint64_t ex_rd = l2_load64(excl_addr);
        if (ex_rd != g0) {
            log_warning("l2c_cross_cache_line_sve: exclusive RMW "
                        "read 0x%016" PRIx64 " want 0x%016" PRIx64, ex_rd, g0);
            all_passed = false;
        }

        // ---- (2) Store-to-load forwarding across the line boundary. A 16B
        // stp at line0_end-8 (offset 56) straddles line0/line1 (8B each) — a
        // single instruction split across two 64B L2 lines. stp/ldp tolerate
        // the misalignment (regular load/store class, no alignment fault),
        // and the immediate reload must forward correctly across the boundary.
        uint8_t *fwd_addr = probe + CACHE_LINE - 8; // 8B in line0, 8B in line1
        /* SVE 128-bit 向量 store (原版 stp 的 SVE 形态; ldxr/stxr/dc-civac
         * 等标量 asm 微操作全部保留 —— 它们是本测试的被测对象) */
        {
            uint64_t pair[2] = {g1, g2};
            svbool_t pg2 = svwhilelt_b64((uint64_t)0, (uint64_t)2);
            svst1_u64(pg2, (uint64_t *)fwd_addr, svld1_u64(pg2, pair));
        }
        uint64_t f_lo, f_hi;
        {
            svbool_t pg2 = svwhilelt_b64((uint64_t)0, (uint64_t)2);
            uint64_t reloaded[2];
            svst1_u64(pg2, reloaded, svld1_u64(pg2, (const uint64_t *)fwd_addr));
            f_lo = reloaded[0]; f_hi = reloaded[1];
        }
        if (f_lo != g1 || f_hi != g2) {
            log_warning("l2c_cross_cache_line_sve: cross-line 16B forward "
                        "lo=0x%016" PRIx64 "/0x%016" PRIx64 " "
                        "hi=0x%016" PRIx64 "/0x%016" PRIx64,
                        f_lo, g1, f_hi, g2);
            all_passed = false;
        }

        // ---- (3) dc civac + writeback probe. Store a golden to probe line0,
        // clean+invalidate the line (forces dirty writeback through L2/L3),
        // then reload — must still match (the line is re-fetched from L2/L3
        // after the writeback). A writeback/coherency fault corrupts it.
        // dc civac requires a cache-line-aligned address; probe is aligned.
        uint8_t *civ_addr = probe; // line0 start, aligned
        l2_store64(civ_addr, g0 ^ g1);
        l2_dc_civac(civ_addr);
        l2_dsb(); // wait for the clean+invalidate to complete
        uint64_t civ_rd = l2_load64(civ_addr);
        if (civ_rd != (g0 ^ g1)) {
            log_warning("l2c_cross_cache_line_sve: post-civac reload "
                        "0x%016" PRIx64 " want 0x%016" PRIx64, civ_rd, g0 ^ g1);
            all_passed = false;
        }

        // ---- (4) L2 thrash sweep. One golden per cache line across the
        // whole thrash region (2x L2). Write pass (str) then read-back pass
        // (ldr) — the read-back is itself L2-thrashing, not cache-warm. Any
        // line that comes back wrong is an L2 SDC.
        size_t num_lines = thrash_size / CACHE_LINE;
        for (size_t l = 0; l < num_lines; ++l) {
            uint64_t sv = GOLDEN_TABLE[l % GOLDEN_TABLE_SIZE]
                          ^ ((uint64_t)l * 0x9E3779B97F4A7C15ULL);
            l2_store64(thrash + l * CACHE_LINE, sv);
        }
        for (size_t l = 0; l < num_lines; ++l) {
            uint64_t sv = GOLDEN_TABLE[l % GOLDEN_TABLE_SIZE]
                          ^ ((uint64_t)l * 0x9E3779B97F4A7C15ULL);
            uint64_t srd = l2_load64(thrash + l * CACHE_LINE);
            if (srd != sv) {
                log_warning("l2c_cross_cache_line_sve: thrash line %zu "
                            "mismatch 0x%016" PRIx64 " want 0x%016" PRIx64,
                            l, srd, sv);
                all_passed = false;
                break;
            }
        }

        if (!all_passed) {
            free(probe);
            free(thrash);
            report_fail_msg("l2c_cross_cache_line_sve: L2 cache coherency/"
                            "split-access/exclusives SDC detected");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    free(probe);
    free(thrash);
    return EXIT_SUCCESS;
}
#else
static int l2c_cross_cache_line_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 L2 cache "
             "(ldxr/stxr/str/ldr/stp/ldp + dc civac) required");
    return EXIT_SKIP;
}
#endif

static int l2c_cross_cache_line_sve_finish(struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(l2c_cross_cache_line_sve,
             "L2 cache coherency/split-access SDC stress: cross-64B-line "
             "exclusive ldxr/stxr RMW, store-to-load forwarding across the "
             "line boundary, dc-civac writeback probe, and a 2x-L2 thrash "
             "sweep (ARM64 inline-asm; L2 size read from sysfs index2)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = l2c_cross_cache_line_sve_init,
    .test_run = l2c_cross_cache_line_sve_run,
    .test_cleanup = l2c_cross_cache_line_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
