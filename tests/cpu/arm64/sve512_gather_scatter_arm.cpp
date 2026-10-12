/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_gather_scatter_arm
 * @parblock
 * SVE full-vector-length gather/scatter SDC stress — standalone extraction
 * of workload 4 from sve512_fma_arm.
 *
 * Drives the SVE memory pipeline at the CPU's runtime vector length:
 *   svld1_gather_u64index_f64   — gather f64 elements at element-index
 *                                 granularity from a permuted index table
 *   svmla_f64_x                 — FMA-transform each gathered element
 *                                 (v = 1.5 + v*v)
 *   svst1_scatter_u64index_f64  — scatter results to a scratch region at
 *                                 the same element indices
 *
 * The scatter destination is byte-compared against a scalar golden that
 * indexes through the SAME indirection (a permutation does NOT commute
 * with the scatter, so the golden must map idx[i] -> dst, not linearly).
 *
 * test_init probes HWCAP_SVE via getauxval (no SVE instruction executes
 * before the probe), returning a clean EXIT_SKIP on SVE-less CPUs.
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
#include <sys/auxv.h>
#include <asm/hwcap.h>
#include <arm_sve.h>
#endif

static inline uint64_t splitmix64(uint64_t x)
{
    uint64_t z = (x + 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct SveGatherScatterData {
    size_t vl_d;
    std::vector<uint64_t> seeds_f64;   // 只读共享
    std::vector<uint64_t> gather_src;  // 只读共享
    std::vector<uint64_t> gather_idx;  // 只读共享(置换索引表)
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
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SVE not available on this CPU; "
                 "sve512_gather_scatter_arm requires SVE");
        return EXIT_SKIP;
    }

    /* 本测试族为 512-bit VL 设计并在 HiSilicon 0xd22 上验证;GH 托管 SVE
     * runner(运行时 VL=128)上实测 golden 失配(issue #182)。 */
    if (svcntb() != 64) {
        log_skip(CpuNotSupportedSkipCategory,
                 "SVE vector length is not 512-bit on this CPU; the sve512 "
                 "family is designed and validated at 512-bit VL only "
                 "(HiSilicon 0xd22, see issue #182)");
        return EXIT_SKIP;
    }

    try {
        auto data = std::make_unique<SveGatherScatterData>();
        data->vl_d = svcntd();

        /* randomization hardening H12': seeds from the framework RNG
         * (per-run fresh, -s reproducible) instead of fixed splitmix. */
        data->seeds_f64.resize(data->vl_d);
        for (size_t lane = 0; lane < data->vl_d; ++lane) {
            data->seeds_f64[lane] = 0x3FF0000000000000ULL |
                (random64() & 0x000FFFFFFFFFFFFFULL);
        }

        const size_t gn = 64 * data->vl_d;
        data->gather_src.resize(gn);
        data->gather_idx.resize(gn);
        for (size_t i = 0; i < gn; ++i) {
            data->gather_src[i] = data->seeds_f64[i % data->vl_d];
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

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sve512_gather_scatter_arm init: %s", e.what());
        return EXIT_SKIP;
    }
}

static int sve512_gather_scatter_arm_run(struct test *test, int cpu)
{
    (void)cpu;
    const auto *d = static_cast<SveGatherScatterData *>(test->data);
    const size_t gn = d->gather_idx.size();

    // 每线程每迭代的栈上 scratch(dst/golden 可写),不共享 —— 规避 -n>1 并发踩踏
    // (与 sve512_f64_chain_arm 的 hw/gold 同模式)。
    std::vector<uint64_t> scatter_dst(gn, 0);
    std::vector<uint64_t> scatter_golden(gn, 0);

    do {
        bool all_passed = true;

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
                "sve512_gather_scatter_arm: SVE-512 gather/scatter SDC detected");
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
             "table, byte-exact compared against a scalar golden")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_gather_scatter_arm_init,
    .test_run = sve512_gather_scatter_arm_run,
    .test_cleanup = sve512_gather_scatter_arm_cleanup,
    .fracture_loop_count = -1,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
