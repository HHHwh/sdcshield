/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_gather_scatter_arm
 * @parblock
 * SVE full-vector-length gather/scatter SDC stress — standalone extraction
 * of workload 4 from sve512_fma_arm, extended with explicit read-offset
 * verification and gather-variant coverage (SEVI ASPLOS'26: 76% of
 * memory-class SDC is a wrong-offset read — valid data from the wrong
 * address, never a crash, input-level repro ~0 — so the offset itself
 * must be asserted, not just the value).
 *
 * Drives the SVE memory pipeline at the CPU's runtime vector length:
 *   svld1_gather_u64index_f64   — gather f64 elements at element-index
 *                                 granularity from a permuted index table
 *   svmla_f64_x                 — FMA-transform each gathered element
 *                                 (v = 1.5 + v*v)
 *   svst1_scatter_u64index_f64  — scatter results to a scratch region at
 *                                 the same element indices
 *
 * Explicit read-offset assertion: the source is encoded so the value
 * reveals the index it came from — the low IDX_TAG_BITS of the mantissa
 * literally carry the element index while sign, exponent and the upper
 * mantissa carry per-run fresh entropy (the plain 0x3FF0..|i encoding
 * put every value within ~1e-16 of 1.0 and gutted the datapath
 * switching this stress test exists to exercise — caught by the Codex
 * review of PR #233). After every gather, each lane's raw loaded value
 * is tag-decoded and checked against idx[lane] BEFORE the FMA — a
 * wrong-offset read produces a decodable-but-wrong index and fails
 * immediately.
 *
 * Gather variants (round-robin): u64index (scalar base + u64 element
 * indices — the original), u64base (per-lane base addresses in a vector,
 * svld1_gather_u64base_f64), and a wide-stride variant walking a
 * dedicated 2 MiB backing table (larger than L1D and L2) with element
 * stride 64 and a per-round 512 KiB window rotation, so every gather is
 * a genuine L1 miss into L2/L3 — ITHICA MemDiv: forcing different
 * memory-hierarchy paths exposes faults homogeneous traffic never sees.
 *
 * The scatter destination is byte-compared against a scalar golden that
 * indexes through the SAME indirection (a permutation does NOT commute
 * with the scatter, so the golden must map idx[i] -> dst, not linearly).
 *
 * All-VL 128..1024-bit via arm64_require_sve_vl (all-VL Phase 2 P3);
 * the pre-P3 512-bit single-value gate and its issue #182 caveat are
 * cleared by the four-tier dual-kernel evidence in gem5-fi
 * feat/sve512-dual. test_init gates via the framework layer
 * (HWCAP_SVE capability + range check in one call, no SVE instruction
 * executes before the probe), returning a clean EXIT_SKIP on SVE-less
 * CPUs; test_run re-checks the VL and fails closed on drift.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <cmath>
#include <algorithm>  // std::fill: libc++ does not pull <algorithm> transitively (libstdc++ does)
#include <memory>
#include <vector>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

static inline uint64_t splitmix64(uint64_t x)
{
    uint64_t z = (x + 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Index-revealing encoding v2: the low 20 mantissa bits are the element
// index tag (the wrong-offset assertion decodes exactly these bits), wide
// enough for both the gather table (gn = 64 x lanes, i.e. 128..1024
// entries across the 128..1024-bit VL tiers) and the 256K-entry stride
// table below — IDX_TAG_MASK 0xFFFFF covers every tier.
static constexpr uint64_t IDX_TAG_MASK = 0xFFFFFULL;

// Wide-stride backing table: 256K f64 = 2 MiB — larger than the 64 KiB L1D
// (and than the 512 KiB per-core L2) of the target parts, so the stride
// walk genuinely traverses L1 -> L2/L3 (ITHICA MemDiv).
static constexpr size_t STRIDE_ELEMS = 256 * 1024;

struct SveGatherScatterData {
    size_t vl_d;
    std::vector<uint64_t> gather_src;  // 只读共享
    std::vector<uint64_t> gather_idx;  // 只读共享(置换索引表)
    std::vector<uint64_t> stride_src;  // 只读共享(2 MiB 宽步距后备表)
    // 注意: scatter_dst / scatter_golden 是每线程每迭代的可变 scratch,不能放共享 data ——
    // -n>1 时框架对每个 worker 线程共享同一个 test->data,并发改写同一块 scatter 缓冲会
    // 相互踩踏(实测 -n 4 时上半 lane 读到 0x0:某线程的 std::fill 清掉了另一线程正在比对的
    // lane)。改为 test_run 内栈上分配,与 sve512_f64_chain_arm 的 hw/gold 同模式。
};

#ifdef __aarch64__

static void gather_scatter_golden(const SveGatherScatterData *d, uint64_t *golden_out)
{
    for (size_t i = 0; i < d->gather_idx.size(); ++i) {
        double v;
        memcpy(&v, &d->gather_src[d->gather_idx[i]], 8);
        v = std::fma(v, v, 1.5);
        memcpy(&golden_out[d->gather_idx[i]], &v, 8);
    }
}

static void gather_scatter_hw(const SveGatherScatterData *d, uint64_t *dst_out)
{
    const size_t n = d->gather_idx.size();
    const size_t lanes = d->vl_d;
    const svbool_t pg = svptrue_b64();
    const svfloat64_t one_half = svdup_f64(1.5);
    for (size_t off = 0; off < n; off += lanes) {
        svuint64_t vidx = svld1_u64(pg, &d->gather_idx[off]);
        svfloat64_t v = svld1_gather_u64index_f64(
            pg, reinterpret_cast<const double *>(d->gather_src.data()), vidx);
        v = svmla_f64_x(pg, one_half, v, v);
        svst1_scatter_u64index_f64(
            pg, reinterpret_cast<double *>(dst_out), vidx, v);
    }
}

static void report_f64_lane_mismatch(const char *tag, size_t lane,
                                     uint64_t golden, uint64_t actual)
{
    log_warning("%s: lane %zu golden=0x%016" PRIx64 " "
                "actual=0x%016" PRIx64 " xor=0x%016" PRIx64,
                tag, lane, golden, actual, golden ^ actual);
}

static int sve512_gather_scatter_arm_init(struct test *test)
{
    /* All-VL Phase 2 P3: capability + 128..1024-bit range check in one
       framework-layer call (the geometry has been svcnt-dynamic all
       along; only the old single-value gate pinned one tier). The
       historical VL=128 golden mismatch on a hosted CI runner
       (issue #182) is cleared by the four-tier dual-kernel evidence
       (gem5-fi feat/sve512-dual, 48 runs). arm64_require_sve_vl
       returns the VL in BYTES — f64 lanes = svl/8, range-checked
       below (release-effective, NDEBUG-proof). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    try {
        auto data = std::make_unique<SveGatherScatterData>();
        data->vl_d = (size_t)svl / 8;   /* f64 lanes = SVL bytes / 8 */
        if (data->vl_d < 2 || data->vl_d > 16) {
            log_error("sve512_gather_scatter_arm: f64 lanes %zu outside "
                      "[2,16] — unit drift (svl %ld bytes)",
                      data->vl_d, svl);
            return EXIT_FAILURE;
        }

        const size_t gn = 64 * data->vl_d;
        data->gather_src.resize(gn);
        data->gather_idx.resize(gn);
        // Index-revealing encoding v2 (Codex review, PR #233): the low
        // IDX_TAG_BITS of the mantissa carry the element index (the
        // wrong-offset assertion decodes exactly these), while sign,
        // exponent and the upper mantissa carry per-run fresh entropy
        // from the framework RNG (H12': -s reproducible). The exponent
        // stays in the [0.5, 2) band so the FMA transform v = 1.5 + v*v
        // can never overflow and the byte-exact golden stays valid — and
        // the operands keep the bit-switching activity the v1 encoding
        // (quiet [1,2) doubles 1e-16 apart) had destroyed. The old
        // seeds_f64 table became dead code under v1 and is gone.
        for (size_t i = 0; i < gn; ++i) {
            uint64_t sign = (random64() & 1ULL) << 63;
            uint64_t exp = 0x3FEULL + (random64() & 1ULL);          // [0.5, 2)
            uint64_t entropy = (random64() & 0xFFFFFFFFULL) << 20;  // mant [51:20]
            data->gather_src[i] = sign | (exp << 52) | entropy |
                                  (uint64_t)(i & IDX_TAG_MASK);
            data->gather_idx[i] = i;
        }
        /* Fisher-Yates shuffle driven by the framework RNG (per-run
         * fresh permutation; was a fixed splitmix permutation). */
        for (size_t i = gn - 1; i > 0; --i) {
            size_t j = (size_t)(random64() % (i + 1));
            uint64_t t = data->gather_idx[i];
            data->gather_idx[i] = data->gather_idx[j];
            data->gather_idx[j] = t;
        }

        /* Wide-stride backing table (Codex review, PR #233): the stride
         * variant used to index the 4 KiB gather_src, where (i*64) % 512
         * touches only eight L1-resident lines — no L1 miss ever
         * happened. A dedicated 2 MiB table (STRIDE_ELEMS) exceeds L1D
         * and L2, so the stride walk genuinely traverses the hierarchy.
         * Same index-revealing encoding v2 (256K < 2^20 tag width). */
        data->stride_src.resize(STRIDE_ELEMS);
        for (size_t i = 0; i < STRIDE_ELEMS; ++i) {
            uint64_t sign = (random64() & 1ULL) << 63;
            uint64_t exp = 0x3FEULL + (random64() & 1ULL);          // [0.5, 2)
            uint64_t entropy = (random64() & 0xFFFFFFFFULL) << 20;  // mant [51:20]
            data->stride_src[i] = sign | (exp << 52) | entropy |
                                  (uint64_t)(i & IDX_TAG_MASK);
        }

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sve512_gather_scatter_arm init: %s", e.what());
        return EXIT_SKIP;
    }
}

// ---------------------------------------------------------------------------
// Explicit read-offset assertion (SEVI: 76% of memory-class SDC is a
// wrong-offset read): the value encoding carries its own index, so a lane
// that read the wrong address decodes to the wrong index.
// ---------------------------------------------------------------------------
static bool check_read_offsets(const SveGatherScatterData *d,
                               const uint64_t *raw,
                               const uint64_t *idx, size_t n)
{
    (void)d;
    bool ok = true;
    for (size_t i = 0; i < n; ++i) {
        uint64_t decoded = raw[i] & IDX_TAG_MASK;
        if (decoded != (idx[i] & IDX_TAG_MASK)) {
            log_warning("sve512_gather_scatter: wrong-offset read at slot %zu: "
                        "expected index %llu, value decodes to %llu",
                        i, (unsigned long long)idx[i],
                        (unsigned long long)decoded);
            ok = false;
        }
    }
    return ok;
}

// Wide-stride index table: element stride 64 (512 B) over a backing table
// larger than L1, with a per-round rotating base. The rotation advances by
// 64K elements (512 KiB) — more than one round's 256 KiB window — so
// consecutive rounds touch disjoint line sets and every gather is a
// genuine L1 miss instead of re-hitting the previous round's lines.
static void build_stride_idx(std::vector<uint64_t> &idx, size_t n,
                             size_t span, size_t base)
{
    idx.resize(n);
    for (size_t i = 0; i < n; ++i)
        idx[i] = (base + i * 64) % span;
}

static int sve512_gather_scatter_arm_run(struct test *test, int cpu)
{
    (void)cpu;
    const auto *d = static_cast<SveGatherScatterData *>(test->data);

    /* All-VL policy #3: fail closed on init/run VL drift. */
    if (svcntd() != d->vl_d) {
        log_error("sve512_gather_scatter_arm: VL drifted between init (%zu "
                  "lanes) and run (%u lanes)", d->vl_d, (unsigned)svcntd());
        report_fail_msg("sve512_gather_scatter_arm: SVE VL drifted between "
                        "init and run");
    }

    const size_t gn = d->gather_idx.size();
    const size_t lanes = d->vl_d;

    // u64base variant operand: per-lane base addresses into src (computed
    // once from the read-only shared tables — no -n>1 hazard).
    std::vector<uint64_t> base_addrs(gn);
    for (size_t i = 0; i < gn; ++i)
        base_addrs[i] = reinterpret_cast<uintptr_t>(&d->gather_src[d->gather_idx[i]]);

    // wide-stride variant table (rebuilt every round with a rotating base)
    std::vector<uint64_t> stride_idx;
    size_t stride_base = 0;

    // 每线程每迭代的栈上 scratch(dst/golden 可写),不共享 —— 规避 -n>1 并发踩踏
    // (与 sve512_f64_chain_arm 的 hw/gold 同模式)。
    std::vector<uint64_t> scatter_dst(gn, 0);
    std::vector<uint64_t> scatter_golden(gn, 0);

    do {
        bool all_passed = true;

        // ---- variant 1: u64index (original) + explicit offset check ----
        {
            std::vector<uint64_t> raw(gn);
            const svbool_t pg = svptrue_b64();
            for (size_t off = 0; off < gn; off += lanes) {
                svuint64_t vidx = svld1_u64(pg, &d->gather_idx[off]);
                svfloat64_t v = svld1_gather_u64index_f64(
                    pg, reinterpret_cast<const double *>(d->gather_src.data()), vidx);
                svst1_f64(pg, reinterpret_cast<double *>(&raw[off]), v);
            }
            if (!check_read_offsets(d, raw.data(), d->gather_idx.data(), gn))
                all_passed = false;
        }

        // ---- variant 2: u64base (vector base addresses) ----
        {
            std::vector<uint64_t> raw(gn);
            const svbool_t pg = svptrue_b64();
            for (size_t off = 0; off < gn; off += lanes) {
                svuint64_t vbase = svld1_u64(pg, &base_addrs[off]);
                svfloat64_t v = svld1_gather_u64base_f64(pg, vbase);
                svst1_f64(pg, reinterpret_cast<double *>(&raw[off]), v);
            }
            if (!check_read_offsets(d, raw.data(), d->gather_idx.data(), gn))
                all_passed = false;
        }

        // ---- variant 3: wide stride (genuine L1-miss gather path) ----
        {
            build_stride_idx(stride_idx, gn, d->stride_src.size(), stride_base);
            stride_base = (stride_base + 64 * 1024) % d->stride_src.size();
            std::vector<uint64_t> raw(gn);
            const svbool_t pg = svptrue_b64();
            for (size_t off = 0; off < gn; off += lanes) {
                svuint64_t vidx = svld1_u64(pg, &stride_idx[off]);
                svfloat64_t v = svld1_gather_u64index_f64(
                    pg, reinterpret_cast<const double *>(d->stride_src.data()), vidx);
                svst1_f64(pg, reinterpret_cast<double *>(&raw[off]), v);
            }
            if (!check_read_offsets(d, raw.data(), stride_idx.data(), gn))
                all_passed = false;
        }

        // ---- original FMA+scatter round-trip (value-level check) ----
        gather_scatter_hw(d, scatter_dst.data());
        gather_scatter_golden(d, scatter_golden.data());
        for (size_t i = 0; i < gn; ++i) {
            if (scatter_dst[i] != scatter_golden[i]) {
                report_f64_lane_mismatch("sve512_gather_scatter", i,
                                         scatter_golden[i],
                                         scatter_dst[i]);
                all_passed = false;
            }
        }

        if (!all_passed) {
            report_fail_msg(
                "sve512_gather_scatter_arm: SVE gather/scatter SDC detected "
                "(wrong-offset read or scatter corruption)");
            return EXIT_FAILURE;
        }

        std::fill(scatter_dst.begin(), scatter_dst.end(), 0);
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int sve512_gather_scatter_arm_cleanup(struct test *test)
{
    delete static_cast<SveGatherScatterData *>(test->data);
    return EXIT_SUCCESS;
}

#else  /* !__aarch64__ */

static int sve512_gather_scatter_arm_init(struct test *test)
{
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "sve512_gather_scatter_arm requires aarch64 SVE");
    return EXIT_SKIP;
}
static int sve512_gather_scatter_arm_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    return EXIT_SKIP;
}
static int sve512_gather_scatter_arm_cleanup(struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}

#endif /* __aarch64__ */

DECLARE_TEST(sve512_gather_scatter_arm,
             "SVE full-vector-length gather/scatter SDC stress: "
             "svld1_gather_u64index_f64 + svmla_f64_x + "
             "svst1_scatter_u64index_f64 round-trip over a permuted index "
             "table (all-VL 128..1024-bit via arm64_require_sve_vl), "
             "byte-exact compared against a scalar golden")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_gather_scatter_arm_init,
    .test_run = sve512_gather_scatter_arm_run,
    .test_cleanup = sve512_gather_scatter_arm_cleanup,
    .fracture_loop_count = 0,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
