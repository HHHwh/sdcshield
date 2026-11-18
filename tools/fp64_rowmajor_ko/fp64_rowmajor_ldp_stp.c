// SPDX-License-Identifier: GPL-2.0
/*
 * fp64_rowmajor_ldp_stp.c — neon_rot_ldr_at_top_rowmajor_fp64 的
 * ldp/stp 内核模块 (ko) 版本
 *
 * 改造说明 (相对原 sandstone 测试 neon_rot_ldr_at_top_rowmajor_fp64_dump):
 *
 * 1. 指令替换 — 原 NEON 单寄存器访存全部改为双寄存器对:
 *        load_vec:  "ldr %q0, [%1]"    (ldr qN, [addr], 16B)
 *        store_vec: vst1q_u64()        (str qN, [addr], 16B)
 *    =>  load_vec:  "ldp %0,%1,[%2]"   (ldp xA,xB, [addr], 8B+8B)
 *        store_vec: "stp %0,%1,[%2]"   (stp xA,xB, [addr], 8B+8B)
 *    语义等价性: 小端 AArch64 下 ldr q 的 v.d[0]/v.d[1] 通道分别对应
 *    16 字节窗口的前/后 8 字节, 与 ldp xA,xB 的装入顺序一一对应;
 *    stp 与 vst1q_u64 同理。计算逻辑 (fp64_rot 旋转 slot 合成) 未变。
 *
 * 2. 内核化约束:
 *    - NEON/FPSIMD: arm64 内核使用 NEON 必须处于 kernel_neon_begin/end
 *      区间; 所有 V 寄存器操作限制在两个 noinline 函数内
 *      (fp64_golden_init / fp64_run_sweeps), 便于反汇编核验;
 *    - 编译: 内核全局 -mgeneral-regs-only, 通过 CFLAGS_REMOVE 移除
 *      (与内核 lib/xor-neon.o 同一做法, 见 Makefile);
 *    - 内存: __get_free_pages 页对齐分配 (满足原 64B 对齐要求),
 *      GFP_KERNEL|__GFP_ZERO;
 *    - 数据转储: 内核态不直接写文件, 原 movbe_log 二进制 dump 改为
 *      dmesg 报告 — 逐 sweep memcmp, 失败时冷路径打印 nbad 与前 2 个
 *      差异元素的 A/B/T/G/E/X (格式与原 report_fail_msg 一致, 保留
 *      "miscompare" 关键字以兼容 grep);
 *    - 验证接口: 与用户态参考 (userspace_ref.c) 共用同一 xorshift64*
 *      种子与 FNV-1a 哈希, 输出 "HASH srcA=.. srcB=.. scratch=..
 *      expected=.. temp=.. dst=.." 供逐位一致性比对。
 *
 * 3. 模块参数: seed (ullong, 默认 0x12345678DEADBEEF)
 *    用法: insmod fp64_rowmajor_ldp_stp.ko seed=0x12345678DEADBEEF
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/atomic.h>
#include <linux/smp.h>
#include <linux/utsname.h>
#include <asm/neon.h>
/* 内核规范的 NEON 内建函数包装头 (统一 u64/s64 与 arm_neon.h 的类型),
 * 见 arch/arm64/include/asm/neon-intrinsics.h */
#include <asm/neon-intrinsics.h>

#define FP64_COUNT        1024
#define FP64_SWEEPS       256
#define FP64_BUF_SZ       (FP64_COUNT * sizeof(uint64x2_t))
#define FP64_DEFAULT_SEED 0x12345678DEADBEEFULL

static u64 seed = FP64_DEFAULT_SEED;
module_param(seed, ullong, 0444);
MODULE_PARM_DESC(seed, "xorshift64* seed for srcA/srcB/scratch (same value as userspace_ref argv[1])");

struct fp64_ctx {
    uint64x2_t *srcA;
    uint64x2_t *srcB;
    uint64x2_t *scratch;
    uint64x2_t *expected;
};

/* ===== ldp/stp 双寄存器加载存储 (等价替换 ldr q / str q) =====
 * 小端 AArch64: ldp xA,xB,[addr] 依次装入 16 字节的前/后 8 字节,
 * 与 ldr q 的 v.d[0]/v.d[1] 通道一一对应; stp 同理。 */

static inline void store_vec(void *addr, uint64x2_t val)
{
    const u64 lo = vgetq_lane_u64(val, 0);
    const u64 hi = vgetq_lane_u64(val, 1);
    __asm__ volatile ("stp %0, %1, [%2]" : : "r"(lo), "r"(hi), "r"(addr) : "memory");
}

static inline uint64x2_t load_vec(const void *addr)
{
    u64 lo, hi;
    __asm__ volatile ("ldp %0, %1, [%2]" : "=r"(lo), "=r"(hi) : "r"(addr) : "memory");
    return vcombine_u64(vcreate_u64(lo), vcreate_u64(hi));
}

/* FP64 rotation, slot order mirrors the integer parent: 0=fadd 1=fsub
 * 2=fmul 3=fmax. 与原版完全一致, 未做任何改动。 */
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

/* ===== 与用户态参考完全一致的 xorshift64* 随机源 ===== */

static u64 rng_state;

static void rng_seed_init(u64 s)
{
    rng_state = s ? s : 0x9E3779B97F4A7C15ULL;
}

static u64 rng_next(void)
{
    u64 x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static void memset_random(void *buf, size_t len)
{
    u8 *p = (u8 *)buf;
    size_t off = 0;
    while (off + sizeof(u64) <= len) {
        u64 v = rng_next();
        memcpy(p + off, &v, sizeof(v));
        off += sizeof(v);
    }
    if (off < len) {
        u64 v = rng_next();
        memcpy(p + off, &v, len - off);
    }
}

/* ===== 与用户态参考完全一致的 FNV-1a 64 哈希 ===== */

static u64 fnv1a64(const void *buf, size_t len)
{
    const u8 *p = (const u8 *)buf;
    u64 h = 0xcbf29ce484222325ULL;
    while (len--) {
        h ^= *p++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* ===== 缓冲分配: 页对齐 (满足原 64B 对齐要求) ===== */

static void *buf_alloc(void)
{
    return (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
                                    get_order(FP64_BUF_SZ));
}

static void buf_free(void *p)
{
    if (p)
        free_pages((unsigned long)p, get_order(FP64_BUF_SZ));
}

/* ===== 冷路径失败报告 (dmesg, 消息体与原 report_fail_msg 一致) ===== */

static noinline void fp64_miscompare_report(
        const struct fp64_ctx *ctx, const uint64x2_t *temp,
        const uint64x2_t *dst, u64 sweep)
{
    /* cpu is NOT a parameter: fetch it cold — the actual core residency
     * at report time (raw_smp_processor_id: 冷路径允许在可抢占上下文). */
    const int cpu = raw_smp_processor_id();
    static atomic_t g_seq = ATOMIC_INIT(0); /* cold-only: unique seq */
    const int seq = atomic_inc_return(&g_seq) - 1;
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

    /* compact per-element summary:
     * A/B = feeding sources, T = 1st store, G = dst (got), E = golden,
     * X = G^E per lane. Two elements in the message. */
    char msg[900];
    int off = snprintf(msg, sizeof(msg),
                       "fp64_rowmajor data miscompare: cpu=%d sweep=%lu nbad=%d bad=",
                       cpu, (unsigned long)sweep, total_bad);
    for (int k = 0; k < nbad && k < 2 && off > 0 && off < (int)sizeof(msg) - 260; k++) {
        const int i = bad[k];
        const u64 *A = (const u64 *)&ctx->srcA[i];
        const u64 *B = (const u64 *)&ctx->srcB[i];
        const u64 *T = (const u64 *)&temp[i];
        const u64 *G = (const u64 *)&dst[i];
        const u64 *E = (const u64 *)&ctx->expected[i];
        off += snprintf(msg + off, sizeof(msg) - (size_t)off,
                        " | #%d s=%d A=%016llx,%016llx B=%016llx,%016llx T=%016llx,%016llx G=%016llx,%016llx E=%016llx,%016llx X=%016llx,%016llx",
                        i, i % 4,
                        (unsigned long long)A[0], (unsigned long long)A[1],
                        (unsigned long long)B[0], (unsigned long long)B[1],
                        (unsigned long long)T[0], (unsigned long long)T[1],
                        (unsigned long long)G[0], (unsigned long long)G[1],
                        (unsigned long long)E[0], (unsigned long long)E[1],
                        (unsigned long long)(G[0] ^ E[0]),
                        (unsigned long long)(G[1] ^ E[1]));
    }
    pr_err("%s\n", msg);
}

/* ===== 黄金值计算 + 热循环: 所有 NEON 使用限制在这两个 noinline
 * 函数内 (kernel_neon_begin/end 保护), 便于反汇编核验 ===== */

static noinline void fp64_golden_init(struct fp64_ctx *ctx)
{
    kernel_neon_begin();
    for (int i = 0; i < FP64_COUNT; i++) {
        const uint64x2_t a = load_vec(&ctx->srcA[i]);        /* ldp */
        const uint64x2_t b = load_vec(&ctx->srcB[i]);        /* ldp */
        store_vec(&ctx->expected[i], fp64_rot(i % 4, a, b)); /* stp */
    }
    kernel_neon_end();
}

/* 返回失败 sweep 数 (<0 = 错误); 打印 HASH 与 PASS/FAIL 行 */
static noinline int fp64_run_sweeps(struct fp64_ctx *ctx)
{
    uint64x2_t *temp = buf_alloc();
    uint64x2_t *dst = buf_alloc();
    int failing = 0;
    u64 sweep = 0;

    if (!temp || !dst)
        goto nomem;

    for (int iter = 0; iter < FP64_SWEEPS; iter++) {
        const bool ascending = (sweep & 1) == 0;

        kernel_neon_begin();
        for (int n = 0; n < FP64_COUNT; n++) {
            const int i = ascending ? n : (FP64_COUNT - 1 - n);

            const uint64x2_t unused_x = load_vec(&ctx->scratch[i]); /* ldp */
            (void)unused_x;

            const uint64x2_t a = load_vec(&ctx->srcA[i]);           /* ldp */
            const uint64x2_t b = load_vec(&ctx->srcB[i]);           /* ldp */
            const uint64x2_t res = fp64_rot(i % 4, a, b);

            store_vec(&temp[i], res);                               /* stp */
            store_vec(&dst[i], res);                                /* stp */
        }
        kernel_neon_end();
        sweep++;

        if (memcmp(dst, ctx->expected, FP64_BUF_SZ) != 0) {
            /* ---- COLD failure branch ONLY: all report work lives in
             * the out-of-line helper above. ---- */
            fp64_miscompare_report(ctx, temp, dst, sweep);
            failing++;
        }
    }

    pr_info("HASH srcA=%016llx srcB=%016llx scratch=%016llx expected=%016llx temp=%016llx dst=%016llx\n",
            (unsigned long long)fnv1a64(ctx->srcA, FP64_BUF_SZ),
            (unsigned long long)fnv1a64(ctx->srcB, FP64_BUF_SZ),
            (unsigned long long)fnv1a64(ctx->scratch, FP64_BUF_SZ),
            (unsigned long long)fnv1a64(ctx->expected, FP64_BUF_SZ),
            (unsigned long long)fnv1a64(temp, FP64_BUF_SZ),
            (unsigned long long)fnv1a64(dst, FP64_BUF_SZ));
    pr_info("%s cpu=%d sweeps=%d seed=0x%016llx\n",
            failing ? "FAIL" : "PASS", raw_smp_processor_id(), FP64_SWEEPS,
            (unsigned long long)seed);

    buf_free(temp);
    buf_free(dst);
    return failing;

nomem:
    buf_free(temp);
    buf_free(dst);
    return -ENOMEM;
}

static int __init fp64_rowmajor_ldp_stp_init(void)
{
    struct fp64_ctx ctx = { NULL, NULL, NULL, NULL };
    int ret;

    rng_seed_init(seed);
    pr_info("init: kernel=%s cpu=%d count=%d sweeps=%d seed=0x%016llx\n",
            init_utsname()->release, raw_smp_processor_id(),
            FP64_COUNT, FP64_SWEEPS, (unsigned long long)seed);

    ctx.srcA     = buf_alloc();
    ctx.srcB     = buf_alloc();
    ctx.scratch  = buf_alloc();
    ctx.expected = buf_alloc();
    if (!ctx.srcA || !ctx.srcB || !ctx.scratch || !ctx.expected) {
        ret = -ENOMEM;
        goto out;
    }

    memset_random(ctx.srcA, FP64_BUF_SZ);
    memset_random(ctx.srcB, FP64_BUF_SZ);
    memset_random(ctx.scratch, FP64_BUF_SZ);

    fp64_golden_init(&ctx);
    ret = fp64_run_sweeps(&ctx);

out:
    buf_free(ctx.srcA);
    buf_free(ctx.srcB);
    buf_free(ctx.scratch);
    buf_free(ctx.expected);
    return ret < 0 ? ret : 0;
}

static void __exit fp64_rowmajor_ldp_stp_exit(void)
{
    pr_info("unloaded\n");
}

module_init(fp64_rowmajor_ldp_stp_init);
module_exit(fp64_rowmajor_ldp_stp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("sdc");
MODULE_DESCRIPTION("ldp/stp variant of neon_rot_ldr_at_top_rowmajor_fp64 (fp64 row-major rotating add/sub/mul/max), kernel-module form with dmesg miscompare reporting");
MODULE_VERSION("1.0");
