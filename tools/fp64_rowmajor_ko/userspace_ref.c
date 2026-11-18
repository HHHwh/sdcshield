/*
 * userspace_ref.c — 验证参考实现 (oracle)
 *
 * 来源: sandstone 测试 neon_rot_ldr_at_top_rowmajor_fp64_dump.cpp 的
 * 用户态 C 转换, 保留原始 NEON 单寄存器访存路径:
 *     load_vec:  "ldr %q0, [%1]"   (ldr qN, [addr], 16B)
 *     store_vec: vst1q_u64()       (str qN, [addr], 16B)
 * 用于与 ldp/stp 内核模块版本 (fp64_rowmajor_ldp_stp.c) 做逐位结果比对。
 *
 * 与内核模块保持同步的约定:
 *   - 同一 xorshift64* 随机源 (同一种子 => 逐位相同的 srcA/srcB/scratch);
 *   - 相同的 FNV-1a 64 哈希输出格式 (六个缓冲):
 *     "HASH srcA=.. srcB=.. scratch=.. expected=.. temp=.. dst=.."
 *   - 失败消息含 "miscompare" 关键字 (保持族测试 grep 约定)。
 *
 * 用法: ./userspace_ref [seed]     # 缺省 0x12345678DEADBEEF
 */

#define _GNU_SOURCE /* sched_getcpu */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#else
#error "userspace_ref requires an AArch64 build"
#endif

#define FP64_COUNT        1024
#define FP64_SWEEPS       256
#define FP64_DEFAULT_SEED 0x12345678DEADBEEFULL

struct fp64_ctx {
    uint64x2_t *srcA;
    uint64x2_t *srcB;
    uint64x2_t *scratch;
    uint64x2_t *expected;
};

/* ===== 原始路径: ldr q 单寄存器加载 / vst1q_u64 (str q) 存储 ===== */

static inline void store_vec(void *addr, uint64x2_t val)
{
    vst1q_u64((uint64_t *)addr, val);
}

static inline uint64x2_t load_vec(const void *addr)
{
    uint64x2_t res;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

/* FP64 rotation, slot order mirrors the integer parent: 0=fadd 1=fsub
 * 2=fmul 3=fmax. Reinterpretations are compile-time no-ops. */
static inline uint64x2_t fp64_rot(int slot, uint64x2_t a, uint64x2_t b)
{
    const float64x2_t fa = vreinterpretq_f64_u64(a);
    const float64x2_t fb = vreinterpretq_f64_u64(b);
    float64x2_t fres;
    switch (slot) {
    case 0: fres = vaddq_f64(fa, fb); break;
    case 1: fres = vsubq_f64(fa, fb); break;
    case 2: fres = vmulq_f64(fa, fb); break;
    default: fres = vmaxq_f64(fa, fb); break;
    }
    return vreinterpretq_u64_f64(fres);
}

/* ===== 与内核模块完全一致的 xorshift64* 随机源 ===== */

static uint64_t rng_state;

static void rng_seed_init(uint64_t s)
{
    rng_state = s ? s : 0x9E3779B97F4A7C15ULL;
}

static uint64_t rng_next(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static void memset_random(void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t off = 0;
    while (off + sizeof(uint64_t) <= len) {
        uint64_t v = rng_next();
        memcpy(p + off, &v, sizeof(v));
        off += sizeof(v);
    }
    if (off < len) {
        uint64_t v = rng_next();
        memcpy(p + off, &v, len - off);
    }
}

/* ===== 与内核模块完全一致的 FNV-1a 64 哈希 ===== */

static uint64_t fnv1a64(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint64_t h = 0xcbf29ce484222325ULL;
    while (len--) {
        h ^= *p++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* ===== 冷路径失败报告 (原 report_fail_msg 消息体, 文本不变) ===== */
__attribute__((noinline)) static void miscompare_report(
        const struct fp64_ctx *ctx, const uint64x2_t *temp,
        const uint64x2_t *dst, uint64_t sweep)
{
    const int cpu = sched_getcpu();
    static int g_seq; /* cold-only: unique seq across calls */
    const int seq = __atomic_fetch_add(&g_seq, 1, __ATOMIC_RELAXED);
    (void)seq;

    int bad[8];
    int nbad = 0;
    int total_bad = 0;
    for (int i = 0; i < FP64_COUNT; i++) {
        if (memcmp(&dst[i], &ctx->expected[i], sizeof(uint64x2_t)) != 0) {
            total_bad++;
            if (nbad < 8)
                bad[nbad++] = i;
        }
    }

    char msg[900];
    int off = snprintf(msg, sizeof(msg),
                       "fp64_rowmajor data miscompare: cpu=%d sweep=%lu nbad=%d bad=",
                       cpu, (unsigned long)sweep, total_bad);
    for (int k = 0; k < nbad && k < 2 && off > 0 && off < (int)sizeof(msg) - 260; k++) {
        const int i = bad[k];
        const uint64_t *A = (const uint64_t *)&ctx->srcA[i];
        const uint64_t *B = (const uint64_t *)&ctx->srcB[i];
        const uint64_t *T = (const uint64_t *)&temp[i];
        const uint64_t *G = (const uint64_t *)&dst[i];
        const uint64_t *E = (const uint64_t *)&ctx->expected[i];
        off += snprintf(msg + off, sizeof(msg) - (size_t)off,
                        " | #%d s=%d A=%016lx,%016lx B=%016lx,%016lx T=%016lx,%016lx G=%016lx,%016lx E=%016lx,%016lx X=%016lx,%016lx",
                        i, i % 4, A[0], A[1], B[0], B[1], T[0], T[1], G[0], G[1], E[0], E[1],
                        G[0] ^ E[0], G[1] ^ E[1]);
    }
    fprintf(stderr, "%s\n", msg);
}

static void *xalloc(size_t size)
{
    void *p = NULL;
    if (posix_memalign(&p, 64, size) != 0)
        return NULL;
    memset(p, 0, size);
    return p;
}

int main(int argc, char **argv)
{
    uint64_t seed = (argc > 1) ? strtoull(argv[1], NULL, 0) : FP64_DEFAULT_SEED;
    rng_seed_init(seed);

    struct fp64_ctx ctx;
    ctx.srcA     = xalloc(FP64_COUNT * sizeof(uint64x2_t));
    ctx.srcB     = xalloc(FP64_COUNT * sizeof(uint64x2_t));
    ctx.scratch  = xalloc(FP64_COUNT * sizeof(uint64x2_t));
    ctx.expected = xalloc(FP64_COUNT * sizeof(uint64x2_t));
    uint64x2_t *temp = xalloc(FP64_COUNT * sizeof(uint64x2_t));
    uint64x2_t *dst  = xalloc(FP64_COUNT * sizeof(uint64x2_t));
    if (!ctx.srcA || !ctx.srcB || !ctx.scratch || !ctx.expected || !temp || !dst) {
        fprintf(stderr, "userspace_ref: alloc failed\n");
        return 2;
    }

    memset_random(ctx.srcA,    FP64_COUNT * sizeof(uint64x2_t));
    memset_random(ctx.srcB,    FP64_COUNT * sizeof(uint64x2_t));
    memset_random(ctx.scratch, FP64_COUNT * sizeof(uint64x2_t));

    /* golden: same FP64 rotating composition, computed on the same NEON
     * units — 原始形态: 直接数组下标访问 (编译为 ldr q / str q) */
    for (int i = 0; i < FP64_COUNT; i++)
        ctx.expected[i] = fp64_rot(i % 4, ctx.srcA[i], ctx.srcB[i]);

    int failing = 0;
    uint64_t sweep = 0;
    for (int iter = 0; iter < FP64_SWEEPS; iter++) {
        const int ascending = (sweep & 1) == 0;
        for (int n = 0; n < FP64_COUNT; n++) {
            const int i = ascending ? n : (FP64_COUNT - 1 - n);

            uint64x2_t unused_x = load_vec(&ctx.scratch[i]); /* ldr q */
            (void)unused_x;

            uint64x2_t a = load_vec(&ctx.srcA[i]);            /* ldr q */
            uint64x2_t b = load_vec(&ctx.srcB[i]);            /* ldr q */
            uint64x2_t res = fp64_rot(i % 4, a, b);

            store_vec(&temp[i], res);                         /* str q */
            store_vec(&dst[i], res);                          /* str q */
        }
        sweep++;

        if (memcmp(dst, ctx.expected, FP64_COUNT * sizeof(uint64x2_t)) != 0) {
            /* COLD failure branch ONLY */
            miscompare_report(&ctx, temp, dst, sweep);
            failing++;
        }
    }

    printf("HASH srcA=%016llx srcB=%016llx scratch=%016llx expected=%016llx temp=%016llx dst=%016llx\n",
           (unsigned long long)fnv1a64(ctx.srcA, FP64_COUNT * sizeof(uint64x2_t)),
           (unsigned long long)fnv1a64(ctx.srcB, FP64_COUNT * sizeof(uint64x2_t)),
           (unsigned long long)fnv1a64(ctx.scratch, FP64_COUNT * sizeof(uint64x2_t)),
           (unsigned long long)fnv1a64(ctx.expected, FP64_COUNT * sizeof(uint64x2_t)),
           (unsigned long long)fnv1a64(temp, FP64_COUNT * sizeof(uint64x2_t)),
           (unsigned long long)fnv1a64(dst, FP64_COUNT * sizeof(uint64x2_t)));
    printf("%s cpu=%d sweeps=%d seed=0x%016llx\n",
           failing ? "FAIL" : "PASS", sched_getcpu(), FP64_SWEEPS,
           (unsigned long long)seed);

    free(dst);
    free(temp);
    free(ctx.expected);
    free(ctx.scratch);
    free(ctx.srcB);
    free(ctx.srcA);
    return failing ? 1 : 0;
}
