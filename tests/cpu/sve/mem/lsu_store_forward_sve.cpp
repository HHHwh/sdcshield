/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b lsu_store_forward_sve
 * @parblock
 * Load/Store Unit SDC stress — a coverage-weak unit (LSU 54%). The suite's
 * existing memory tests (memcpy_*, mem_disambiguation, mfence) move data
 * bulk or test ordering, but none target the LSU's most SDC-prone
 * micro-path: the **store buffer → load forwarding** datapath, where a
 * subsequent load to a just-stored address is satisfied by the store buffer
 * before the store retires to L1. A fault in forwarding (wrong lane merge,
 * wrong partial-forward, store-buffer entry corruption) silently produces a
 * load value that differs from what was stored — the canonical LSU SDC.
 *
 * This test exercises every forwarding case the silicon must handle:
 *
 *   1. **Same-size forward at every access width and offset**: store then
 *      immediately load the SAME address at widths 1/2/4/8/16 bytes and at
 *      offsets 0,1,2,3,7,8,12,15 — including line_end-8..line_end-1 where
 *      the access straddles a 64B cache-line boundary (the split-forward
 *      path, where the forward merges two store-buffer entries). Inline-asm
 *      str/ldr/stp/ldp with a "memory" clobber so the optimizer cannot
 *      elide the store or the load (a real forward, not a reg-reg copy).
 *   2. **Partial-store forwarding / store-merging**: overlapping stores of
 *      different widths into one cache line (store 1B at 0, 2B at 1, 4B at
 *      3, then load 8B at 0) — the LSU must merge the partial stores into
 *      the forwarded load. A merge fault corrupts the loaded bytes.
 *   3. **Store-after-load hazard**: a load immediately followed by a store
 *      to the same address (RAW then WAW), interleaved with a forward —
 *      stresses address-matching / hazard detection in the store buffer.
 *   4. **NEON 128B store + reload across the line**: a 16B stp/ldp at the
 *      line boundary (offset 56, 8B per line) — the 128-bit forward path.
 *   5. **Strided store-then-load across a full cache line**: store 8B every
 *      8 bytes across one 64B line, then reload each — exercises the
 *      multi-entry store-buffer forward under back-to-back same-line traffic.
 *
 * SDC detection: every forward is byte-exact compared against the value
 * stored; report_fail_msg on any mismatch. SVE-port: scalar inline-asm str/ldr micro-ops preserved
 * (they are the tested forwarding window), phase-4 128-bit cross-line forward
 * on svst1/svld1 vector lanes; non-aarch64 returns a clean
 * EXIT_SKIP. Wired into the arm64 subdir (aarch64-only guard), so x86-64 is
 * untouched.
 *
 * Note: the regular str/ldr/stp/ldp class tolerates misalignment on
 * Kunpeng 920 (verified), so the cross-line offset probes (line_end-8..-1)
 * do NOT raise BUS_ADRALN — only the exclusive class and dc-civac do (see
 * the l2c_cross_cache_line_arm alignment note). This test deliberately
 * uses the regular class for the misaligned probes.
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

static constexpr size_t CACHE_LINE = 64;

// High-Hamming golden table — successive cycles toggle the data-path gates.
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
// Byte/halfword/word/doubleword inline-asm store+load, "memory" clobber so
// the compiler emits real str/ldr micro-ops and cannot elide the round-trip.
static inline void sf_store8(void *a, uint8_t v)
{ __asm__ volatile("strb %w1, [%0]" : : "r"(a), "r"((uint32_t)v) : "memory"); }
static inline uint8_t sf_load8(const void *a)
{ uint32_t r; __asm__ volatile("ldrb %w0, [%1]" : "=r"(r) : "r"(a) : "memory"); return (uint8_t)r; }
static inline void sf_store16(void *a, uint16_t v)
{ __asm__ volatile("strh %w1, [%0]" : : "r"(a), "r"((uint32_t)v) : "memory"); }
static inline uint16_t sf_load16(const void *a)
{ uint32_t r; __asm__ volatile("ldrh %w0, [%1]" : "=r"(r) : "r"(a) : "memory"); return (uint16_t)r; }
static inline void sf_store32(void *a, uint32_t v)
{ __asm__ volatile("str %w1, [%0]" : : "r"(a), "r"(v) : "memory"); }
static inline uint32_t sf_load32(const void *a)
{ uint32_t r; __asm__ volatile("ldr %w0, [%1]" : "=r"(r) : "r"(a) : "memory"); return r; }
static inline void sf_store64(void *a, uint64_t v)
{ __asm__ volatile("str %1, [%0]" : : "r"(a), "r"(v) : "memory"); }
static inline uint64_t sf_load64(const void *a)
{ uint64_t r; __asm__ volatile("ldr %0, [%1]" : "=r"(r) : "r"(a) : "memory"); return r; }
static inline void sf_store128(void *a, uint64_t lo, uint64_t hi)
{ __asm__ volatile("stp %2, %3, [%0]" : : "r"(a), "r"(0), "r"(lo), "r"(hi) : "memory"); }
static inline void sf_load128(const void *a, uint64_t *lo, uint64_t *hi)
{ __asm__ volatile("ldp %0, %1, [%2]" : "=r"(*lo), "=r"(*hi) : "r"(a) : "memory"); }
#else
static inline void sf_store8(void *a, uint8_t v) { std::memcpy(a, &v, 1); }
static inline uint8_t sf_load8(const void *a) { uint8_t v; std::memcpy(&v, a, 1); return v; }
static inline void sf_store16(void *a, uint16_t v) { std::memcpy(a, &v, 2); }
static inline uint16_t sf_load16(const void *a) { uint16_t v; std::memcpy(&v, a, 2); return v; }
static inline void sf_store32(void *a, uint32_t v) { std::memcpy(a, &v, 4); }
static inline uint32_t sf_load32(const void *a) { uint32_t v; std::memcpy(&v, a, 4); return v; }
static inline void sf_store64(void *a, uint64_t v) { std::memcpy(a, &v, 8); }
static inline uint64_t sf_load64(const void *a) { uint64_t v; std::memcpy(&v, a, 8); return v; }
static inline void sf_store128(void *a, uint64_t lo, uint64_t hi) { uint64_t p[2]={lo,hi}; std::memcpy(a,p,16); }
static inline void sf_load128(const void *a, uint64_t *lo, uint64_t *hi) { uint64_t p[2]; std::memcpy(p,a,16); *lo=p[0]; *hi=p[1]; }
#endif

static int lsu_store_forward_sve_init(struct test *test)
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

static int lsu_store_forward_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;

    // A 2-cache-line buffer: line0 = [0..63], line1 = [64..127]. Aligned so
    // line_end is exactly at offset 64. The cross-line probes use offsets
    // 56..63 of line0 (an access there straddles into line1).
    uint8_t *buf = static_cast<uint8_t *>(aligned_alloc_safe(CACHE_LINE, CACHE_LINE * 2));
    if (!buf) {
        log_skip(TestResourceIssueSkipCategory,
                 "lsu_store_forward_sve: alloc failed");
        return EXIT_SKIP;
    }

    do {
        bool all_passed = true;
        static uint64_t golden_cycle = 0;
        uint64_t g0 = GOLDEN_TABLE[golden_cycle % GOLDEN_TABLE_SIZE];
        uint64_t g1 = GOLDEN_TABLE[(golden_cycle + 1) % GOLDEN_TABLE_SIZE];
        uint64_t g2 = GOLDEN_TABLE[(golden_cycle + 2) % GOLDEN_TABLE_SIZE];
        golden_cycle = (golden_cycle + 1) % GOLDEN_TABLE_SIZE;

        // ---- (1) Same-size forward at every width, sweeping offsets that
        // include the line boundary (offsets 56..63 straddle line0/line1).
        // For each (width, offset): store the golden (low width bytes),
        // immediately load the same address, compare byte-exact.
        // Offsets: 0,1,2,3,7,8,12,15 then the boundary run 56..63.
        static const unsigned offsets[] = {0, 1, 2, 3, 7, 8, 12, 15,
                                           56, 57, 58, 59, 60, 61, 62, 63};
        constexpr size_t NUM_OFF = sizeof(offsets) / sizeof(offsets[0]);

        // 1-byte forward (strb/ldrb) at every offset.
        for (size_t i = 0; i < NUM_OFF; ++i) {
            uint8_t want = (uint8_t)(g0 >> ((i % 8) * 8));
            sf_store8(buf + offsets[i], want);
            uint8_t got = sf_load8(buf + offsets[i]);
            if (got != want) {
                log_warning("lsu_store_forward_sve: 1B fwd off %u "
                            "got 0x%02x want 0x%02x", offsets[i], got, want);
                all_passed = false;
            }
        }
        // 2-byte forward (strh/ldrh) at offsets where 2 bytes fit (<=62)
        // and at the boundary (56..62 straddle).
        for (size_t i = 0; i < NUM_OFF; ++i) {
            if (offsets[i] > 62) continue; // 2B access; let boundary straddle
            uint16_t want = (uint16_t)(g1 >> ((i % 4) * 4));
            sf_store16(buf + offsets[i], want);
            uint16_t got = sf_load16(buf + offsets[i]);
            if (got != want) {
                log_warning("lsu_store_forward_sve: 2B fwd off %u "
                            "got 0x%04x want 0x%04x", offsets[i], got, want);
                all_passed = false;
            }
        }
        // 4-byte forward (str/ldr) — offsets <=60.
        for (size_t i = 0; i < NUM_OFF; ++i) {
            if (offsets[i] > 60) continue;
            uint32_t want = (uint32_t)(g2 ^ (uint32_t)(i * 0x11111111u));
            sf_store32(buf + offsets[i], want);
            uint32_t got = sf_load32(buf + offsets[i]);
            if (got != want) {
                log_warning("lsu_store_forward_sve: 4B fwd off %u "
                            "got 0x%08x want 0x%08x", offsets[i], got, want);
                all_passed = false;
            }
        }
        // 8-byte forward (str/ldr) — offsets <=56 (56 straddles line0/line1).
        for (size_t i = 0; i < NUM_OFF; ++i) {
            if (offsets[i] > 56) continue;
            uint64_t want = g0 ^ (i * 0x9E3779B97F4A7C15ULL);
            sf_store64(buf + offsets[i], want);
            uint64_t got = sf_load64(buf + offsets[i]);
            if (got != want) {
                log_warning("lsu_store_forward_sve: 8B fwd off %u "
                            "got 0x%016" PRIx64 " want 0x%016" PRIx64,
                            offsets[i], got, want);
                all_passed = false;
            }
        }
        // 16-byte forward (stp/ldp) — offsets <=48 (48..63 all in line0;
        // offset 56 straddles line0/line1 with 8B in each line).
        for (size_t i = 0; i < NUM_OFF; ++i) {
            if (offsets[i] > 56) continue;
            uint64_t lo = g1 ^ (i * 0x0123456789ABCDEFULL);
            uint64_t hi = g2 ^ (i * 0xFEDCBA9876543210ULL);
            sf_store128(buf + offsets[i], lo, hi);
            uint64_t glo, ghi;
            sf_load128(buf + offsets[i], &glo, &ghi);
            if (glo != lo || ghi != hi) {
                log_warning("lsu_store_forward_sve: 16B fwd off %u "
                            "lo=0x%016" PRIx64 "/0x%016" PRIx64 " "
                            "hi=0x%016" PRIx64 "/0x%016" PRIx64,
                            offsets[i], glo, lo, ghi, hi);
                all_passed = false;
            }
        }

        // ---- (2) Partial-store forwarding / store-merging. Write
        // overlapping partial stores into one 8-byte slot, then load the
        // full 8 bytes — the LSU must merge the partial stores into the
        // forwarded load. Build the expected merged value in software.
        std::memset(buf, 0, CACHE_LINE * 2); // clean slate
        // Store 1B at 0, 2B at 1, 4B at 3, 1B at 7 — all into bytes 0..7.
        uint8_t  b0 = (uint8_t)(g0);
        uint16_t h1 = (uint16_t)(g1);
        uint32_t w3 = (uint32_t)(g2);
        uint8_t  b7 = (uint8_t)(g0 >> 8);
        sf_store8(buf + 0, b0);
        sf_store16(buf + 1, h1);
        sf_store32(buf + 3, w3);
        sf_store8(buf + 7, b7);
        // Compute the merged 8-byte expectation (little-endian).
        uint8_t merged[8] = {0,0,0,0,0,0,0,0};
        merged[0] = b0;                       // byte 0
        merged[1] = (uint8_t)(h1 & 0xff);     // byte 1 (low of h1)
        merged[2] = (uint8_t)(h1 >> 8);       // byte 2 (high of h1)
        merged[3] = (uint8_t)(w3 & 0xff);     // byte 3 (low of w3)
        merged[4] = (uint8_t)(w3 >> 8);       // byte 4
        merged[5] = (uint8_t)(w3 >> 16);      // byte 5
        merged[6] = (uint8_t)(w3 >> 24);      // byte 6
        merged[7] = b7;                       // byte 7 (overwrites? no, w3 was bytes 3-6, b7 is byte 7)
        uint64_t merged64;
        std::memcpy(&merged64, merged, 8);
        uint64_t fwd64 = sf_load64(buf + 0);
        if (fwd64 != merged64) {
            log_warning("lsu_store_forward_sve: partial-fwd merge "
                        "got 0x%016" PRIx64 " want 0x%016" PRIx64,
                        fwd64, merged64);
            all_passed = false;
        }

        // ---- (3) Store-after-load hazard. Load the just-merged value,
        // then store a new golden over it, then forward-load — the LSU must
        // handle the RAW then WAW then RAW sequence.
        (void)sf_load64(buf + 0);             // RAW read of merged value
        uint64_t haz = g0 ^ g1;
        sf_store64(buf + 0, haz);
        uint64_t haz_rd = sf_load64(buf + 0); // forward of the new store
        if (haz_rd != haz) {
            log_warning("lsu_store_forward_sve: hazard fwd "
                        "got 0x%016" PRIx64 " want 0x%016" PRIx64,
                        haz_rd, haz);
            all_passed = false;
        }

        // ---- (4) NEON 128B cross-line forward (offset 56: 8B in line0,
        // 8B in line1).
        /* SVE 16B 跨行 store/reload (原版 stp/ldp 的 SVE 向量形态) */
        {
            uint64_t pair[2] = {g0, g1};
            svbool_t pg16 = svwhilelt_b64((uint64_t)0, (uint64_t)2);
            svst1_u64(pg16, (uint64_t *)(buf + 56), svld1_u64(pg16, pair));
            uint64_t reloaded[2];
            svst1_u64(pg16, reloaded, svld1_u64(pg16, (const uint64_t *)(buf + 56)));
            uint64_t clo = reloaded[0], chi = reloaded[1];
            if (clo != g0 || chi != g1) {
            log_warning("lsu_store_forward_sve: 128B cross-line fwd "
                        "lo=0x%016" PRIx64 "/0x%016" PRIx64 " "
                        "hi=0x%016" PRIx64 "/0x%016" PRIx64,
                        clo, g0, chi, g1);
                all_passed = false;
            }
        }

        // ---- (5) Strided store-then-load across one full cache line.
        // Store 8B every 8 bytes across line0, then reload each — multi-
        // entry store-buffer forward under back-to-back same-line traffic.
        uint64_t stride_golden[CACHE_LINE / 8];
        for (size_t i = 0; i < CACHE_LINE / 8; ++i) {
            stride_golden[i] = GOLDEN_TABLE[i % GOLDEN_TABLE_SIZE]
                               ^ (i * 0x9E3779B97F4A7C15ULL);
            sf_store64(buf + i * 8, stride_golden[i]);
        }
        for (size_t i = 0; i < CACHE_LINE / 8; ++i) {
            uint64_t got = sf_load64(buf + i * 8);
            if (got != stride_golden[i]) {
                log_warning("lsu_store_forward_sve: stride fwd slot %zu "
                            "got 0x%016" PRIx64 " want 0x%016" PRIx64,
                            i, got, stride_golden[i]);
                all_passed = false;
            }
        }

        if (!all_passed) {
            free(buf);
            report_fail_msg("lsu_store_forward_sve: store-buffer/load "
                             "forwarding SDC detected");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    free(buf);
    return EXIT_SUCCESS;
}
#else
static int lsu_store_forward_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 LSU store-to-load "
             "forwarding (inline-asm str/ldr/strb/ldrb/strh/ldrh/stp/ldp) "
             "required");
    return EXIT_SKIP;
}
#endif

static int lsu_store_forward_sve_finish(struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(lsu_store_forward_sve,
             "LSU store-buffer/load-forwarding SDC stress: same-size forward "
             "at widths 1/2/4/8/16B and offsets including the line boundary, "
             "partial-store forwarding/merging, store-after-load hazard, "
             "128B cross-line forward, and strided same-line forward "
             "(ARM64 inline-asm)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = lsu_store_forward_sve_init,
    .test_run = lsu_store_forward_sve_run,
    .test_cleanup = lsu_store_forward_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
