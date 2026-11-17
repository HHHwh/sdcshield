/*  neon_fma_gemm_flush_arm.cpp — 归因纯净的 NEON FMA 矩阵压测 + DC CIVAC 写回覆盖。
 *
 * 靶点（AGENTS 优先级第一位）：NEON f64 乘法器阵列与 FMA 累加器——FMA 是现场
 * 向量 SDC 的第一贡献者（>75% [05]）。现有矩阵套件要么隔库层（eigen_gemm /
 * acl_gemm，指令成分由库决定），要么是 SVE 矩阵指令（FMMLA/FMMLA2）或 SME
 * （FMOPA）——128-bit 纯手写 NEON FMA 矩阵形态此前为零，本测试补该空档。
 *
 * 计算核：手写 vfmaq_f64 双行分块 GEMM（448x448 f64 行主序，64 行一个块行，
 * 每 k 步 16 条 FMA），无库依赖、无自动向量化参与——失效可直接归因到 NEON
 * f64 FMA 数据通路本身。
 *
 * 内存层级覆盖（结合 DC CIVAC 覆盖 L3 与 DDR）：
 *   - 工作集：A+B+golden 4.8MB 共享只读 + C/D 3.2MB/线程私有（run 内分配，
 *     见 run 函数注释——共享会导致全核满载下污染写与冷回读验证竞态）。
 *     GEMM 热数据驻留 L3，每迭代 B 全阵读（1.6MB）与 C/D 块写跨越
 *     L2（1.25MB），产生真实 L2<->L3 流量；
 *   - 每迭代对结果块逐行 dc civac + dsb sy，强制脏行写回 PoC（内存）并作废，
 *     随后冷回读与标量独立 golden 字节级比对——该腿专测写回路径（热读验证
 *     结构性不可达的盲区）与行重填路径；C/D 双镜像缓冲各自独立冲刷验证。
 *
 * Golden（issue #373 范式）：test_init 用标量 std::fma 按与计算核相同的 k 序、
 * 相同的单次融合舍入独立重算全 A*B；no-tree-vectorize + noinline 钉死标量性，
 * 可过 scripts/check-scalar-golden.sh 机器门禁。操作数位型全部来自框架
 * 64-bit RNG（-s seed 可重放），取有限带 [0.5, 2)（指数仅 0x3FE/0x3FF），
 * 乘积 <= 4、k=448 累加 <= 1792，不会溢出到 Inf/NaN。
 *
 * ld/st 指令负载账（每迭代，Y = 块行字节数 = 64 x 448 x 8 = 224KB）：
 *   污染写 st 2Y（C/D 块，轮转常量位型，兼反懒惰复制与边际模式激活）
 *   GEMM  ld A 块 Y + ld B 全阵 7Y + 镜像写 st 2Y（同一累加器同时写 C 与 D）
 *   冷回读验证 ld 2Y（CIVAC 后必然 miss，自写回落点重填）
 *   => ld:st = 10Y:4Y = 5:2。GEMM「2 操作数读 : 1 结果写」是计算形态固有
 *   形状；镜像写 + 污染写把 store 占比从纯 GEMM 的 11%（8:1）提到 29%——
 *   不塞稀释计算的填充复制腿时，这是该形态能达到的最接近 1:1 的均衡。
 *
 * 失败现场：逐元素位型比对，失配报告首个 (row, col) + computed/expected
 * 64-bit 位型 + 失配总数。
 *
 * 内存足迹：4.8MB 共享（A+B+golden，init 一次）+ 3.2MB/线程（C/D 私有），
 * 128 核全量约 415MB、192 核约 620MB（低于同目录 memcpy_l3_cache_size_neon
 * 的 168MB/线程一个量级）。归入 neon_memcpy_cache 分类
 * （tests/cpu/meson.build aarch64 守卫块），便于按分类整组执行；
 * 仅 aarch64 构建，x86-64 不受影响。
 */
#include <sandstone.h>
#include <arm_neon.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int N_DIM = 448;          /* 矩阵阶：7 x 64，行字节数 3584 = 56 行 */
constexpr int TILE  = 64;           /* 块行高度（行）；N_DIM 必须被 TILE 与 16 整除 */
constexpr size_t ELEM_COUNT = (size_t)N_DIM * N_DIM;
constexpr size_t BUF_BYTES  = ELEM_COUNT * sizeof(double);  /* 1,605,632，64 的倍数 */

struct NeonFmaGemmFlushData {
    double *A;       /* 左操作数（init 期随机，运行期只读，线程间共享安全） */
    double *B;       /* 右操作数（同上） */
    double *golden;  /* 标量独立重算的 A*B（issue #373，运行期只读） */
};

static inline double bits_to_f64(uint64_t bits)
{
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

static inline uint64_t f64_to_bits(double d)
{
    uint64_t b;
    memcpy(&b, &d, sizeof(b));
    return b;
}

static double *alloc_buf(void)
{
    /* BUF_BYTES 是 64 的倍数，满足 aligned_alloc 的对齐整除要求 */
    return static_cast<double *>(aligned_alloc(64, BUF_BYTES));
}

/* 操作数填充：符号位 + 指数 {0x3FE, 0x3FF}（值域 [0.5, 2)）+ 52 位全随机尾数。
 * 尾数来自框架 64-bit RNG——高翻转密度且 -s seed 可重放。 */
static void fill_operands(double *M)
{
    for (size_t i = 0; i < ELEM_COUNT; ++i) {
        uint64_t r0 = random64();
        uint64_t r1 = random64();
        uint64_t mantissa = r0 & 0x000FFFFFFFFFFFFFULL;
        uint64_t exp = 0x3FEULL + (r1 & 1ULL);      /* [0.5,1) 或 [1,2) */
        uint64_t sign = (r1 >> 1) & 1ULL;
        uint64_t bits = (sign << 63) | (exp << 52) | mantissa;
        memcpy(&M[i], &bits, sizeof(bits));
    }
}

/* 标量独立 golden：与计算核相同的 i/j/k 遍历序、相同的单次融合舍入
 * （std::fma 对应 vfmaq_f64 的单次舍入语义），逐位可复现。 */
#pragma GCC push_options
#pragma GCC optimize("no-tree-vectorize")
[[gnu::noinline]]
static void scalar_gemm_golden(const double *A, const double *B, double *G)
{
    for (int i = 0; i < N_DIM; ++i) {
        for (int j = 0; j < N_DIM; ++j) {
            double acc = 0.0;
            for (int k = 0; k < N_DIM; ++k)
                acc = std::fma(A[(size_t)i * N_DIM + k],
                               B[(size_t)k * N_DIM + j], acc);
            G[(size_t)i * N_DIM + j] = acc;
        }
    }
}
#pragma GCC pop_options

/* 污染写：块行区域先写成轮转常量位型——反懒惰（GEMM 若漏写任何 2 元素段，
 * 污染位型会残留被比对抓到），同时「长常量保持后翻转」激活边际缺陷。 */
static void pollute_tile_rows(double *P, int r0, double pattern)
{
    float64x2_t v = vdupq_n_f64(pattern);
    for (int i = r0; i < r0 + TILE; ++i)
        for (int j = 0; j < N_DIM; j += 2)
            vst1q_f64(P + (size_t)i * N_DIM + j, v);
}

/* 计算核：块行 r0 的 C/D 镜像写入。每 k 步 16 条 vfmaq_f64（两行 x 8 个
 * 2 元素累加器），k 序与标量 golden 完全一致。 */
static void gemm_block_row(const double *A, const double *B,
                           double *C, double *D, int r0)
{
    for (int ii = r0; ii < r0 + TILE; ii += 2) {
        const double *arow0 = A + (size_t)ii * N_DIM;
        const double *arow1 = A + (size_t)(ii + 1) * N_DIM;
        for (int jj = 0; jj < N_DIM; jj += 16) {
            float64x2_t c0 = vdupq_n_f64(0.0), c1 = vdupq_n_f64(0.0);
            float64x2_t c2 = vdupq_n_f64(0.0), c3 = vdupq_n_f64(0.0);
            float64x2_t c4 = vdupq_n_f64(0.0), c5 = vdupq_n_f64(0.0);
            float64x2_t c6 = vdupq_n_f64(0.0), c7 = vdupq_n_f64(0.0);
            float64x2_t e0 = vdupq_n_f64(0.0), e1 = vdupq_n_f64(0.0);
            float64x2_t e2 = vdupq_n_f64(0.0), e3 = vdupq_n_f64(0.0);
            float64x2_t e4 = vdupq_n_f64(0.0), e5 = vdupq_n_f64(0.0);
            float64x2_t e6 = vdupq_n_f64(0.0), e7 = vdupq_n_f64(0.0);
            for (int k = 0; k < N_DIM; ++k) {
                float64x2_t va0 = vdupq_n_f64(arow0[k]);
                float64x2_t va1 = vdupq_n_f64(arow1[k]);
                const double *brow = B + (size_t)k * N_DIM + jj;
                float64x2_t b0 = vld1q_f64(brow + 0);
                float64x2_t b1 = vld1q_f64(brow + 2);
                float64x2_t b2 = vld1q_f64(brow + 4);
                float64x2_t b3 = vld1q_f64(brow + 6);
                float64x2_t b4 = vld1q_f64(brow + 8);
                float64x2_t b5 = vld1q_f64(brow + 10);
                float64x2_t b6 = vld1q_f64(brow + 12);
                float64x2_t b7 = vld1q_f64(brow + 14);
                c0 = vfmaq_f64(c0, va0, b0); c1 = vfmaq_f64(c1, va0, b1);
                c2 = vfmaq_f64(c2, va0, b2); c3 = vfmaq_f64(c3, va0, b3);
                c4 = vfmaq_f64(c4, va0, b4); c5 = vfmaq_f64(c5, va0, b5);
                c6 = vfmaq_f64(c6, va0, b6); c7 = vfmaq_f64(c7, va0, b7);
                e0 = vfmaq_f64(e0, va1, b0); e1 = vfmaq_f64(e1, va1, b1);
                e2 = vfmaq_f64(e2, va1, b2); e3 = vfmaq_f64(e3, va1, b3);
                e4 = vfmaq_f64(e4, va1, b4); e5 = vfmaq_f64(e5, va1, b5);
                e6 = vfmaq_f64(e6, va1, b6); e7 = vfmaq_f64(e7, va1, b7);
            }
            double *crow0 = C + (size_t)ii * N_DIM + jj;
            double *crow1 = C + (size_t)(ii + 1) * N_DIM + jj;
            double *drow0 = D + (size_t)ii * N_DIM + jj;
            double *drow1 = D + (size_t)(ii + 1) * N_DIM + jj;
            vst1q_f64(crow0 + 0, c0);  vst1q_f64(crow0 + 2, c1);
            vst1q_f64(crow0 + 4, c2);  vst1q_f64(crow0 + 6, c3);
            vst1q_f64(crow0 + 8, c4);  vst1q_f64(crow0 + 10, c5);
            vst1q_f64(crow0 + 12, c6); vst1q_f64(crow0 + 14, c7);
            vst1q_f64(crow1 + 0, e0);  vst1q_f64(crow1 + 2, e1);
            vst1q_f64(crow1 + 4, e2);  vst1q_f64(crow1 + 6, e3);
            vst1q_f64(crow1 + 8, e4);  vst1q_f64(crow1 + 10, e5);
            vst1q_f64(crow1 + 12, e6); vst1q_f64(crow1 + 14, e7);
            /* 镜像写：同一累加器内容再写一份到 D——store 流量加倍，
             * 且为第二条独立冲刷/验证臂供数 */
            vst1q_f64(drow0 + 0, c0);  vst1q_f64(drow0 + 2, c1);
            vst1q_f64(drow0 + 4, c2);  vst1q_f64(drow0 + 6, c3);
            vst1q_f64(drow0 + 8, c4);  vst1q_f64(drow0 + 10, c5);
            vst1q_f64(drow0 + 12, c6); vst1q_f64(drow0 + 14, c7);
            vst1q_f64(drow1 + 0, e0);  vst1q_f64(drow1 + 2, e1);
            vst1q_f64(drow1 + 4, e2);  vst1q_f64(drow1 + 6, e3);
            vst1q_f64(drow1 + 8, e4);  vst1q_f64(drow1 + 10, e5);
            vst1q_f64(drow1 + 12, e6); vst1q_f64(drow1 + 14, e7);
        }
    }
}

static inline void dc_civac_line(const void *addr)
{
    __asm__ volatile("dc civac, %0" : : "r"(addr) : "memory");
}

/* 块行逐 64B 行 clean+invalidate 到 PoC（脏行强制写回内存，缓存行作废），
 * 末尾 dsb sy 保证全部写回完成——随后的读必然冷缺失、自写回落点重填。 */
static void flush_tile_rows(double *P, int r0)
{
    char *base = reinterpret_cast<char *>(P);
    for (int i = r0; i < r0 + TILE; ++i)
        for (size_t off = 0; off < (size_t)N_DIM * sizeof(double); off += 64)
            dc_civac_line(base + (size_t)i * N_DIM * sizeof(double) + off);
    __asm__ volatile("dsb sy" : : : "memory");
}

struct MismatchInfo {
    int row;
    int col;
    uint64_t computed_bits;
    uint64_t expected_bits;
    size_t count;
};

/* 冷回读验证：位型级（字节级）比对，浮点零容差。全扫以统计失配总数，
 * 首个失配留存现场。 */
static bool verify_tile_rows(const double *P, const double *G, int r0,
                             MismatchInfo *mi)
{
    bool ok = true;
    for (int i = r0; i < r0 + TILE; ++i) {
        for (int j = 0; j < N_DIM; ++j) {
            size_t idx = (size_t)i * N_DIM + j;
            uint64_t cb = f64_to_bits(P[idx]);
            uint64_t gb = f64_to_bits(G[idx]);
            if (cb != gb) {
                if (ok) {   /* 首个失配：留存现场 */
                    mi->row = i;
                    mi->col = j;
                    mi->computed_bits = cb;
                    mi->expected_bits = gb;
                }
                mi->count++;
                ok = false;
            }
        }
    }
    return ok;
}

} /* anonymous namespace */

static int neon_fma_gemm_flush_arm_init(struct test *test)
{
    auto *d = new NeonFmaGemmFlushData();
    d->A = alloc_buf();
    d->B = alloc_buf();
    d->golden = alloc_buf();
    if (!d->A || !d->B || !d->golden) {
        free(d->A); free(d->B); free(d->golden);
        delete d;
        return EXIT_FAILURE;
    }

    fill_operands(d->A);
    fill_operands(d->B);
    /* 独立标量路径重算 golden——绝不经过被测的 NEON 单元（issue #373） */
    scalar_gemm_golden(d->A, d->B, d->golden);

    test->data = d;
    return EXIT_SUCCESS;
}

static int neon_fma_gemm_flush_arm_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<NeonFmaGemmFlushData *>(test->data);

    /* C/D 每线程私有：全核满载时每核一个并发线程，若共享同一对缓冲，
     * 线程间的污染写会与彼此已完成块的冷回读验证竞态，验证读到的是
     * 别人刚写入的污染位型（确定性假失败，2026-11-17 全核实测）。
     * run 顶部一次性分配、各出口释放（memcpy_l1d_size_neon 惯例；
     * fracture_loop_count = -1 保证单次 run 覆盖全部时长）。 */
    double *C = alloc_buf();
    double *D = alloc_buf();
    if (!C || !D) {
        free(C); free(D);
        return EXIT_FAILURE;
    }

    /* 污染位型轮转表：全 0 / 全 1 / 交替两种——「长时间保持同值后翻转」
     * 的边际缺陷激活模式（AGENTS 项 2）。位型经 bit-cast 进 f64 寄存器，
     * 仅作背景，不参与数值语义。 */
    static const uint64_t POLLUTE_BITS[4] = {
        0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
        0x5A5A5A5A5A5A5A5AULL, 0xA5A5A5A5A5A5A5A5ULL,
    };

    do {
        /* 每迭代随机块行（7 选 1，行起点 3584B 为 64 的倍数，行对齐保持） */
        const int r0 = TILE * (int)(random32() % (N_DIM / TILE));
        const double pattern = bits_to_f64(POLLUTE_BITS[random32() % 4]);

        pollute_tile_rows(C, r0, pattern);
        pollute_tile_rows(D, r0, pattern);

        gemm_block_row(d->A, d->B, C, D, r0);

        /* 臂 1：C 写回 + 冷回读验证 */
        MismatchInfo mi = {};
        flush_tile_rows(C, r0);
        if (!verify_tile_rows(C, d->golden, r0, &mi)) {
            report_fail_msg("neon_fma_gemm_flush_arm: C-arm writeback-reload "
                            "mismatch (row %d, col %d, computed 0x%016llx, "
                            "expected 0x%016llx, total mismatches %zu, block row %d)",
                            mi.row, mi.col,
                            (unsigned long long)mi.computed_bits,
                            (unsigned long long)mi.expected_bits,
                            mi.count, r0);
            free(C); free(D);
            return EXIT_FAILURE;
        }

        /* 臂 2：D 镜像缓冲独立写回 + 冷回读验证 */
        flush_tile_rows(D, r0);
        if (!verify_tile_rows(D, d->golden, r0, &mi)) {
            report_fail_msg("neon_fma_gemm_flush_arm: D-arm writeback-reload "
                            "mismatch (row %d, col %d, computed 0x%016llx, "
                            "expected 0x%016llx, total mismatches %zu, block row %d)",
                            mi.row, mi.col,
                            (unsigned long long)mi.computed_bits,
                            (unsigned long long)mi.expected_bits,
                            mi.count, r0);
            free(C); free(D);
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));

    free(C); free(D);
    return EXIT_SUCCESS;
}

static int neon_fma_gemm_flush_arm_cleanup(struct test *test)
{
    auto *d = static_cast<NeonFmaGemmFlushData *>(test->data);
    if (d) {
        free(d->A); free(d->B); free(d->golden);
        delete d;
        test->data = nullptr;
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_fma_gemm_flush_arm,
             "Attribution-pure NEON FMA GEMM stress with dc-civac writeback "
             "coverage: hand-written 448x448 f64 vfmaq block-row kernel "
             "(16 FMA/k-step, mirror-written to two buffers), per-iteration "
             "dc civac + dsb writeback-to-PoC of both result tiles followed "
             "by cold-reload byte-exact verify vs a scalar fma golden "
             "(issue #373, same k-order single rounding) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = neon_fma_gemm_flush_arm_init,
    .test_run = neon_fma_gemm_flush_arm_run,
    .test_cleanup = neon_fma_gemm_flush_arm_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
