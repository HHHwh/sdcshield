# SVE/SME 全 VL 二期 P2（SME fmopa_za 收尾）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `sme_fmopa_za_arm` 从"从未可用"修成全 VL（128/256/512/1024-bit）动态适配的 SME 测试——arm64_vl.h API 接入、几何 cols 化、轴向语义实证裁决、gem5-fi 语义裁决 kernel 四档验证、writing_tests.md VL 政策节交付。

**Architecture:** 双仓流水（PR #242 先例）：gem5-fi 语义裁决 kernel 先行（轴向判别 + za-only SMSTART 合法性 + 四档 16 run 全绿）→ 同源落地 sdcshield 测试重构（堆叠在 feat/arm64-vl-api 之上，消费 arm64_require_sme_svl）→ writing_tests.md 政策节 + 文档收尾。

**Tech Stack:** C++（SME inline asm, GCC 12.3 无 arm_sme.h intrinsics）、gem5-fi（CHAOS/SE, `--sme_vl {1,2,4,8}`）、arm64_vl.h（PR #277）。

**Spec:** `docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md` §5/§7/§8/§9（已在 main，PR #271 已合并）。

## Global Constraints

- 依赖排序：**测试分支 `feat/sme-fmopa-za-allvl` 从 `feat/arm64-vl-api`（PR #277，未合并）切出**（堆叠 PR）；若 #277 先合并，rebase 到 main（无冲突预期：两 PR 文件集不相交）。
- 本机 0xd01 无 SVE/SME：测试只能验证构建零告警 + SKIP 路径；**全部功能结论以 gem5 四档真实 run 为准**（规则 2b）。gem5-fi 真实路径 `/home/sdc/wangxu/code-review/gem5-fi`。
- 构建入口 `./scripts/build.sh`；回归 `./builddir/sdcshield -e zstd19 -t 2000 -n 1` → `exit: pass`；meson 改动须 `meson setup --reconfigure builddir`。
- DCO：`git commit -s` 末行 `Signed-off-by: wangxu <wangxumarshall@qq.com>`，无 Co-Authored-By；永不提交 main；一单元一 patch。
- x86-64 零改动；SME 测试全部在 `#if defined(__aarch64__)` 内。
- **无回归锚（诚实声明）**：原测试从未在任何硬件产生过有效输出（见下"原状三重缺陷"），spec §7 的回归锚机制不适用——验证锚 = kernel 通道 A 在四档对精确整数 golden 的逐字节一致。
- 验证按用户指令由隔离 subagent 并行执行，主 agent 不阻塞；报告回来前不得引用其结论。

## 原状三重缺陷（P2 的真实起点，2026-10-23 全文核验）

`tests/cpu/arm64/sme_fmopa_za_arm.cpp`（262 行）从未在任何硬件运行过（host 无 SME、无 dual kernel、PR #242 未纳入），全文核验发现三重独立缺陷：

1. **`rdsvl %0, 0`（:153）imm=0**：RDSVL 语义 = SVL_bytes × imm（`sme_fmopa_dual.c:75` 的 `#1` 返回字节数为实证旁证）→ imm=0 恒返 0 → `tile_cols=0` → golden_row0 为空向量、run 侧 `st1w` 向 0 长缓冲写 SVL/32 个 float——真实 SME 硬件上**堆溢出**。
2. **asm 步距 `add %[a], %[a], #16`（:95-96）单位错**：#16 = 16 **字节** = 4 float，而 golden 按 16 **float**/步（`b_stream[s*16+c]` :121、流长 `MOPA_STEPS*16` :158）寻址——asm 与 golden 几何在**任何** SVL 下都不一致。
3. **`mova z2.s, p0/m, za0v.s[w12, 0]`（:100）轴向与自家 docblock 矛盾**：docblock :41-42 称"row slice 0 … ZA[0][c]"（行），golden 按行建模；但 tileview 探针（PR #242 四档实证）确立 **za0v = 列视图**（"za0v.d[r,0]=列 r 转置"）。轴向由 kernel 实证裁决（Task 1），测试 golden 随裁决结果落地。

另：`smstart za`（:88，无 `smstart sm`）能否执行 FMOPA 未经任何验证（stress 孪生用 sm+za 双切）——kernel 一并裁决。

## Review Focus

1. **mova za0v.s 轴向裁决错误 → golden 比错轴**。测试：Task 1 kernel 双 golden 判别（readout 同时对比行/列两个 golden，运行证据裁决并记录）。
2. **za-only SMSTART 下 FMOPA 非法 → SIGILL**。测试：Task 1 四档 run（崩溃即证据；应急方案 = 改 sm+za 双切，stress 先例）。
3. **步距单位错（#16 字节 vs float）**。测试：Task 1 通道 A 四档逐字节一致 + Task 2 一步自检。
4. **SVL 读取/API 误用（cols=0 缺陷类）**。测试：Task 2 require 助手 + 范围检查 + run 侧 fail-closed 断言。
5. **gem5 F4 双舍入假阳性**。测试：精确整数操作数（积与和均精确可表示，两种舍入重合）——Task 1 设计不变式 + 四档 FAILS=0。
6. **cols=32（1024-bit 档）缓冲**：流按 steps×cols 动态分配，无 16 硬编码。测试：Task 2 尺寸代码 + 四档跑。

---

### Task 1: gem5-fi 语义裁决 kernel `sme_fmopa_za_dual.c` + 四档 16 run

**Files:**
- Create（gem5-fi 仓）: `workloads/sdcshield/sme_fmopa_za_dual.c`
- Modify（gem5-fi 仓）: `workloads/sdcshield/Makefile`（追加 gcc 目标）、`findings.md`（实录）

**Interfaces:**
- Produces: ①轴向结论（ROW/COL，四档一致）；②za-only SMSTART 合法性结论；③四档 × {fixed, random×3} 全绿实录（`SDCSHIELD_FAILS=0` + checksum 表）；④与 Task 2 同源的 asm 形态。Task 2 的 golden 公式、smstart 形态、步距寄存器化全部以本任务裁决为准。

- [ ] **Step 1: gem5-fi 仓分支与身份**

```bash
cd /home/sdc/wangxu/code-review/gem5-fi
git config user.name "wangxu" && git config user.email "wangxumarshall@qq.com"   # 若已配置则跳过
git checkout -b feat/fmopa-za-dual    # 基于 main @ 9dbbbbba
```
注意：工作树有他人未提交的 CHAOS/ 与 Makefile PR244 目标——**选择性暂存**（P1 Task 8 先例），不碰他人改动。

- [ ] **Step 2: 写 kernel**

`workloads/sdcshield/sme_fmopa_za_dual.c`（完整内容）：

```c
/* sme_fmopa_za_dual.c — semantics-settling dual-channel kernel for the
 * all-VL Phase 2 P2 rework of sdcshield test sme_fmopa_za_arm.
 * Mirrors the REWORKED test asm (same-source): smstart za; zero {za};
 * 256-step FMOPA za0.s chain over per-step operand windows; mova
 * za0v.s[w12,0] readout; st1w. Settles the original test's two
 * never-validated choices empirically:
 *   (a) mova za0v.s[0,0] axis — tileview .d probe says COLUMN 0, the
 *       test docblock claimed ROW 0; both goldens computed, the run
 *       decides (byte-exact, per lane);
 *   (b) smstart za-only (no sm) sufficing for FMOPA — stress twin uses
 *       sm+za; a gem5 crash here is the evidence to switch.
 * Operands: exact small integers, window layout [s*cols + i]; every
 * product and accumulate is exactly representable in f32, so gem5's
 * fplib double-rounding (F4) coincides with real-hardware single
 * rounding — no F4 adaptation needed.
 * Channels: A = axis discrimination + byte-exact vs the winning golden;
 *           B = FNV-1a over the readout bits.
 * argv: [fixed|random] [seed]; exit 0 / 1(g_fails) / 3(unsupported SVL).
 * Build: gcc -static -O2 -march=armv9-a+sve -Wall -Wextra (no SME
 * mnemonics beyond .inst-free plain asm — wait, FMOPA/SMSTART ARE SME
 * mnemonics: build with clang if available, else gcc -march=armv9-a+sve+sme
 * — mirror the Makefile's existing dual-kernel flags).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define COLS_MAX 32        /* SVL=1024-bit -> 32 f32 lanes */
#define STEPS    256       /* the test's MOPA_STEPS (SVL-independent) */

static uint32_t xs_state;
static uint32_t xs32(void)
{
    xs_state ^= xs_state << 13;
    xs_state ^= xs_state >> 17;
    xs_state ^= xs_state << 5;
    return xs_state;
}

static unsigned g_fails = 0;
static uint64_t fnv = 1469598103934665603ULL;
static void fnv_u32(uint32_t v)
{
    fnv ^= v & 0xFF;         fnv *= 1099511628211ULL;
    fnv ^= (v >> 8) & 0xFF;  fnv *= 1099511628211ULL;
    fnv ^= (v >> 16) & 0xFF; fnv *= 1099511628211ULL;
    fnv ^= (v >> 24) & 0xFF; fnv *= 1099511628211ULL;
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "fixed";
    const uint32_t seed = (argc > 2) ? (uint32_t)strtoul(argv[2], 0, 0) : 1;
    const int randomize = (strcmp(mode, "random") == 0);
    xs_state = seed ? seed : 1;

    int64_t svl_bytes = 0;
    asm volatile("rdsvl %0, #1" : "=r"(svl_bytes));
    const int cols = (int)(svl_bytes / 4);
    if (cols < 4 || cols > COLS_MAX) {
        fprintf(stderr, "unsupported SVL %ld bytes\n", (long)svl_bytes);
        return 3;
    }

    /* Exact-integer operand windows, layout [s*cols + i]. Row 0 and
       column 0 readouts differ by construction (a/b patterns differ). */
    static float a[STEPS * COLS_MAX], b[STEPS * COLS_MAX];
    for (int s = 0; s < STEPS; ++s)
        for (int i = 0; i < cols; ++i) {
            const size_t si = (size_t)s * cols + i;
            if (randomize) {
                a[si] = (float)((xs32() % 7) + 1);
                b[si] = (float)((xs32() % 5) + 1);
            } else {
                a[si] = (float)((si % 7) + 1);
                b[si] = (float)((si % 5) + 1);
            }
        }

    /* Both candidate goldens, exact integer accumulation. */
    static float g_row[COLS_MAX], g_col[COLS_MAX];
    for (int c = 0; c < cols; ++c) g_row[c] = g_col[c] = 0.0f;
    for (int s = 0; s < STEPS; ++s)
        for (int x = 0; x < cols; ++x) {
            const size_t s0 = (size_t)s * cols;
            /* row 0: ZA[0][c] = sum z0[0]*z1[c] */
            g_row[x] += a[s0] * b[s0 + x];
            /* column 0: ZA[r][0] = sum z0[r]*z1[0] */
            g_col[x] += a[s0 + x] * b[s0];
        }

    /* Same-source mirror of the reworked test asm. stride is a register
       operand (bytes = cols * 4); the f32 element size stays implicit. */
    static float out[COLS_MAX];
    {
        const float *pa = a, *pb = b;
        float *po = out;
        long steps = STEPS;
        const size_t stride = (size_t)cols * sizeof(float);
        asm volatile(
            "smstart sm\n\t"
            "smstart za\n\t"
            "ptrue p0.s\n\t"
            "zero {za}\n\t"
            "mov w12, wzr\n\t"
            "1:\n\t"
            "ld1w {z0.s}, p0/z, [%[a]]\n\t"
            "ld1w {z1.s}, p0/z, [%[b]]\n\t"
            "add %[a], %[a], %[s]\n\t"
            "add %[b], %[b], %[s]\n\t"
            "fmopa za0.s, p0/m, p0/m, z0.s, z1.s\n\t"
            "subs %[n], %[n], #1\n\t"
            "b.ne 1b\n\t"
            "mova z2.s, p0/m, za0v.s[w12, 0]\n\t"
            "st1w {z2.s}, p0, [%[o]]\n\t"
            "smstop za\n\t"
            "smstop sm\n\t"
            : [a] "+r"(pa), [b] "+r"(pb), [n] "+r"(steps)
            : [o] "r"(po), [s] "r"(stride)
            : "z0", "z1", "z2", "p0", "w12", "memory", "cc");
        /* 注: 输出 %[a]/%[b]/%[n] 在输入 [s]/[o] 读尽前已写,GCC 语义上理论
           正确形为 "+&r";保持 "+r" 系与 ebe08632 已验证 kernel 及仓库惯例
           (sme_fmopa_stress_arm / za_tileview)逐字同源,统一硬化留待后续 PR
           (三处 SME asm 一并改 "+&r" 并重跑 16-run)。 */
    }

    int row_ok = 1, col_ok = 1;
    for (int i = 0; i < cols; ++i) {
        uint32_t ob, rb, cb;
        memcpy(&ob, &out[i], 4);   /* no aliasing UB; byte-exact per lane */
        memcpy(&rb, &g_row[i], 4);
        memcpy(&cb, &g_col[i], 4);
        fnv_u32(ob);
        if (ob != rb) row_ok = 0;
        if (ob != cb) col_ok = 0;
    }
    if (row_ok && col_ok) {
        /* row0 == col0 at this seed: patterns degenerated — rerun with a
           different seed (the operator notes it); treat as failure here. */
        fprintf(stderr, "row/col goldens identical — seed %u degenerate, "
                        "rerun with another seed\n", seed);
        return 1;
    }
    if (!row_ok && !col_ok) {
        for (int i = 0; i < cols && i < 4; ++i)
            fprintf(stderr, "mismatch @%d hw=%.1f row=%.1f col=%.1f\n",
                    i, (double)out[i], (double)g_row[i], (double)g_col[i]);
        ++g_fails;
    }

    const char *axis = row_ok ? "ROW" : (col_ok ? "COLUMN" : "UNKNOWN");
    printf("sme_fmopa_za_dual: svl=%ld cols=%d axis=%s\n",
           (long)svl_bytes, cols, axis);
    printf("SDCSHIELD_FAILS=%u\nchecksum=%016llx\n",
           g_fails, (unsigned long long)fnv);
    return g_fails ? 1 : 0;
}
```
注：头部 build 注释里对编译器的不确定段落删掉，按 Makefile 实际 flags 写（见 Step 3）。`fmopa`/`smstart`/`mova {za…}` 是 SME 助记符 → 需 `+sme` march；本机无 clang，gcc `-march=armv9-a+sve+sme` 可汇编（`tests_arm64_sme` 库同款）。

- [ ] **Step 3: Makefile 目标**

按 P1 Task 8 的选择性暂存纪律，索引版 Makefile = HEAD + 本单元：
```make
# all-VL Phase 2 P2: semantics-settling kernel for the sme_fmopa_za_arm
# rework (needs +sme mnemonics -> gcc, same shape as the clang SME kernels):
sme_fmopa_za_dual: sme_fmopa_za_dual.c
	$(GCC) -static -O2 -march=armv9-a+sve+sme -Wall -Wextra -o $@ $< -lm
```
并把 `sme_fmopa_za_dual` 加入 `all:` 与 `clean:`。`make sme_fmopa_za_dual` 必须 -Wall -Wextra 零告警。

- [ ] **Step 4: 四档 × {fixed, random×3} = 16 run**

```bash
G5=/home/sdc/wangxu/code-review/gem5-fi/build/ARM/gem5.opt
overall=0
for T in 1 2 4 8; do
  for ARGS in "fixed 1" "random 1" "random 2" "random 3"; do
    timeout 600 $G5 configs/se/arm_chaos.py --cpu O3 \
      --cmd workloads/sdcshield/sme_fmopa_za_dual --sme_vl $T --sve_vl $T \
      --workload_args "$ARGS" 2>&1 | tee /tmp/fza_sve${T}_$(echo $ARGS | tr ' ' '_').log \
      | grep -E 'sme_fmopa_za_dual|SDCSHIELD_FAILS|checksum|mismatch'
    rc=${PIPESTATUS[0]}      # timeout/gem5 的真实退出码,不是 grep 的
    echo "tier=$T args='$ARGS' exit=$rc"
    [ "$rc" -ne 0 ] && overall=1
  done
done
exit $overall
```
Expected 逐档：`cols=4/8/16/32`（=128/256/512/1024-bit）；**axis= 四档一致**（记录 ROW 还是 COLUMN——这是 Task 2 golden 公式的依据）；全部 `SDCSHIELD_FAILS=0`；random 各 seed checksum 互异、fixed/random 可比即记录；**exit≠0 或 axis=UNKNOWN 的档位不得计入全绿、不得作为 Task 2 golden 依据**。若 gem5 在 smstart/fmopa 处崩溃（SIGILL 类）→ za-only 非法，改 `smstart sm`+`smstart za` / `smstop za`+`smstop sm`（stress 先例 :121-122/:150-151），重跑全部 16 run 并在 findings.md 记录裁决过程。若 axis 判别失败（两 golden 均不匹配）→ 看 mismatch dump，如实上报，不改数据迁就。

- [ ] **Step 5: findings.md + 提交推送**

findings.md 尾部：日期、16 run 表（tier/mode/cols/axis/checksum/FAILS/exit）、za-only 裁决、F4 豁免依据（精确整数）、PR 交叉引用。然后选择性暂存三文件、`git commit -s`（正文含 16 run 实证与轴向结论）、`git push -u origin feat/fmopa-za-dual`。

### Task 2: `sme_fmopa_za_arm.cpp` 全 VL 重构（堆叠分支）

**Files:**
- Modify: `tests/cpu/arm64/sme_fmopa_za_arm.cpp`（全文重构 init/asm/golden/run/docblock）

**Interfaces:**
- Consumes: `arm64_require_sme_svl(struct test *, long min_bits, long max_bits) -> long`（>0 = SVL 字节；<0 = EXIT_SKIP(-255) 且已 log_skip——非裸 -1，见 arm64_vl.h 头注，直接 return；PR #277，`#include <device/cpu/arm64/arm64_vl.h>`）；Task 1 的 axis 裁决（**COLUMN**，ebe08632 四档实证）与 smstart 形态结论（**sm+za 双切强制**，za-only 首条 FMOPA 即陷阱）。
- Produces: SME 侧全部达标（spec §6 P2 出口）。

- [ ] **Step 1: 堆叠分支**

```bash
git checkout feat/arm64-vl-api && git checkout -b feat/sme-fmopa-za-allvl
```

- [ ] **Step 2: 重构（以下为完整新实现，替换对应函数/段落）**

`fmopa_chain_asm` ——步距寄存器化（原 `#16` 缺陷修正）：
```cpp
static void fmopa_chain_asm(const float *a_stream, const float *b_stream,
                            size_t steps, float *out_vec, size_t cols)
{
    const size_t stride = cols * sizeof(float);   /* window bytes, runtime */
    __asm__ volatile(
        "smstart sm\n\t"   /* Task 1 裁决: sm+za 双切强制——za-only 下首条 FMOPA
                              即陷阱(FMOPA 需 SVCR.SM=1),四档实证 ebe08632 */
        "smstart za\n\t"
        "ptrue p0.s\n\t"
        "zero {za}\n\t"
        "mov w12, wzr\n\t"
        "1:\n\t"
        "ld1w {z0.s}, p0/z, [%[a]]\n\t"
        "ld1w {z1.s}, p0/z, [%[b]]\n\t"
        "add %[a], %[a], %[s]\n\t"
        "add %[b], %[b], %[s]\n\t"
        "fmopa za0.s, p0/m, p0/m, z0.s, z1.s\n\t"
        "subs %[n], %[n], #1\n\t"
        "b.ne 1b\n\t"
        "mova z2.s, p0/m, za0v.s[w12, 0]\n\t"
        "st1w {z2.s}, p0, [%[o]]\n\t"
        "smstop za\n\t"
        "smstop sm\n\t"
        : [a] "+r"(a_stream), [b] "+r"(b_stream), [n] "+r"(steps)
        : [o] "r"(out_vec), [s] "r"(stride)
        : "z0", "z1", "z2", "p0", "w12", "memory", "cc");
    /* 注: 输出 %[a]/%[b]/%[n] 在输入 [s]/[o] 读尽前已写,理论正确形 "+&r";
       保持 "+r" 与 ebe08632 已验证形态及仓库惯例逐字同源,统一硬化留待后续 PR。 */
}
```

`fmopa_golden` ——窗口几何 + 裁决轴向（以 COLUMN 为例；ROW 则对调两处下标，Task 1 结论为准）：
```cpp
static std::vector<float> fmopa_golden(const std::vector<float> &a_stream,
                                       const std::vector<float> &b_stream,
                                       size_t steps, size_t cols)
{
    /* Per Task 1's axis verdict: mova za0v.s[w12,0] w12=0 reads COLUMN 0 —
       out[r] = ZA[r][0] = sum_s z0[r]*z1[0] = sum_s a[s*cols+r]*b[s*cols]. */
    std::vector<float> out(cols, 0.0f);
    for (size_t s = 0; s < steps; ++s) {
        const size_t s0 = s * cols;
        for (size_t r = 0; r < cols; ++r)
            out[r] += a_stream[s0 + r] * b_stream[s0];  /* exact integers */
    }
    return out;
}
```

`init` ——require API + 范围检查 + 窗口流（替换 :128-174 整个函数体）：
```cpp
static int sme_fmopa_za_arm_init(struct test *test)
{
#ifndef __aarch64__
    (void)test;
    return EXIT_SUCCESS;
#else
    const long svl = arm64_require_sme_svl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;   /* helper returns EXIT_SKIP(-255), not a bare -1;
                               log_skip already emitted (capability or range) */

    try {
        auto data = std::make_unique<SmeFmopaData>();
        /* arm64_require_sme_svl returns SVL BYTES (arm64_vl.h: bits/8);
           cols = f32 lanes = SVL_bits/32 = svl_bytes/4 — same as the kernel. */
        data->tile_cols = (size_t)svl / 4;
        if (data->tile_cols < 4 || data->tile_cols > 32) {
            log_error("sme_fmopa_za_arm: cols %zu outside [4,32] — unit "
                      "drift (svl %ld bytes)", data->tile_cols, svl);
            return EXIT_FAILURE;   /* assert() is compiled out under NDEBUG */
        }

        /* Exact-integer operand windows [s*cols + i] (Task 1 geometry). */
        std::vector<float> a_stream(MOPA_STEPS * data->tile_cols);
        std::vector<float> b_stream(MOPA_STEPS * data->tile_cols);
        for (size_t si = 0; si < a_stream.size(); ++si) {
            a_stream[si] = (float)((si % 7) + 1);
            b_stream[si] = (float)((si % 5) + 1);
        }
        data->golden_readout = fmopa_golden(a_stream, b_stream, MOPA_STEPS,
                                            data->tile_cols);
        data->a_stream = a_stream;      /* struct 增两字段, 免 run 重建 */
        data->b_stream = b_stream;
        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sme_fmopa_za_arm init: %s", e.what());
        return EXIT_SKIP;
    }
#endif
}
```
`SmeFmopaData` 增字段 `std::vector<float> a_stream, b_stream;`；`golden_row0` 改名 **`golden_readout`**（与 38faa1b3 实现一致——原名暗示行 0，与 Task 1 裁决的 COLUMN 轴向矛盾）。

`run` ——fail-closed 断言 + 一步自检 cols 化（替换 :176-233 函数体；流直接用 init 存的，不再重建）：
```cpp
static int sme_fmopa_za_arm_run(struct test *test, int cpu)
{
    (void)cpu;
#ifndef __aarch64__
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 SME required for "
             "sme_fmopa_za_arm");
    return EXIT_SKIP;
#else
    auto *d = static_cast<SmeFmopaData *>(test->data);
    const size_t cols = d->tile_cols;

    if ((size_t)arm64_sme_svl_bytes() / 4 != cols) {
        log_error("sme_fmopa_za_arm: SVL drifted between init (%zu cols) "
                  "and run (%ld cols)", cols, arm64_sme_svl_bytes() / 4);
        report_fail_msg("sme_fmopa_za_arm: SVL drifted between init and run");
    }

    /* One-step self-check (golden-vs-hardware on a single FMOPA; 走同一
       fmopa_chain_asm 硬件路径,失配不预设分类——golden 缺陷与真实 SDC
       在此处不可区分,统一按未分类硬件/金标准失配上报)。 */
    {
        std::vector<float> one_a(cols, 2.0f), one_b(cols);
        for (size_t j = 0; j < cols; ++j)
            one_b[j] = (j & 1) ? 1.0f : 3.0f;
        std::vector<float> expect = fmopa_golden(one_a, one_b, 1, cols);
        std::vector<float> got(cols, -1.0f);
        fmopa_chain_asm(one_a.data(), one_b.data(), 1, got.data(), cols);
        if (memcmp(expect.data(), got.data(), cols * sizeof(float)) != 0) {
            report_fail_msg("sme_fmopa_za_arm: golden tile-mapping "
                            "self-check failed — golden/asm mismatch or "
                            "hardware fault (unclassified)");
        }
    }

    do {
        std::vector<float> out(cols, -1.0f);
        fmopa_chain_asm(d->a_stream.data(), d->b_stream.data(), MOPA_STEPS,
                        out.data(), cols);
        if (memcmp(out.data(), d->golden_readout.data(),
                   cols * sizeof(float)) != 0) {
            for (size_t c = 0; c < cols; ++c)
                if (out[c] != d->golden_readout[c]) {
                    uint32_t g, a;
                    memcpy(&g, &d->golden_readout[c], 4);
                    memcpy(&a, &out[c], 4);
                    log_warning("sme_fmopa_za: col %zu golden=0x%08" PRIx32
                                " actual=0x%08" PRIx32, c, g, a);
                    break;
                }
            report_fail_msg("sme_fmopa_za_arm: SME FMOPA/ZA outer-"
                            "product SDC detected");
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#endif
}
```
（比较改 memcmp 字节级整块比较 + 首失配定位日志——spec §8 军规口径。）

**验证边界诚实声明（P2 评审裁决补记）**：本计划的验证架构存在结构性盲区——gem5 只执行 Task 1 kernel，测试粘合层（init 流尺寸/golden 组装/run 缓冲）在本机与 gem5 均零执行覆盖。缓解防线：① 测试 fixed 模式与 kernel 逐字节几何同构（操作数公式 `(si%7)+1/(si%5)+1`、`MOPA_STEPS=256`、golden 公式三者与 kernel 一致），16-run 证据经同构锚定直达测试 golden；② run 侧一步自检使真机首跑即校验 golden 组装；③ init 的 cols∈[4,32] 显式检查兜住单位漂移。此声明源自 PR #279 五 bot 检视（local-claude 意见），Self-Review 原文"范围检查 ✓"据实勘正为"bits 区间 + 派生 cols∈[4,32] 双重检查"。

头部 includes：`#ifdef __aarch64__` 块内 `<sys/auxv.h>/<asm/hwcap.h>` 换成 `#include <device/cpu/arm64/arm64_vl.h>`（getauxval 手探不再需要）；docblock 重写：三重缺陷修复记载、Task 1 轴向/SMSTART 裁决引用（gem5 run 命令形态 + 16 run 表）、"first functional form"诚实声明。

- [ ] **Step 3: 构建 + SKIP 路径 + 回归**

```bash
./scripts/build.sh
./builddir/sdcshield -e sme_fmopa_za_arm -t 2000 -n 1 -v   # 预期 result: skip (require 助手文案)
./builddir/sdcshield -e zstd19 -t 2000 -n 1
```
Expected：构建零告警；skip-reason 含 'ARM64 SME not available; sme_fmopa_za_arm exercises the SME datapath'（require 助手能力分支文案）；zstd19 pass。

- [ ] **Step 4: 提交推送**

单 commit（一个单元 = 一个测试的完整重构），正文引用 Task 1 的 16 run 轴向/SMSTART 裁决与本机 SKIP/构建实录。

### Task 3: writing_tests.md VL 政策节（spec 0c6c9c89 归属交付）

**Files:**
- Modify: `docs/coding/writing_tests.md`（`## Additional information` :641 之前插入）

- [ ] **Step 1: 插入政策节**

```markdown
## SVE/SME vector-length policy (all-VL Phase 2)

Tests that exercise SVE/SME must adapt to the runtime vector length
instead of assuming one tier:

- **Probe via the framework layer**: `#include
  <device/cpu/arm64/arm64_vl.h>` and call
  `arm64_require_sve_vl(test, min_bits, max_bits)` /
  `arm64_require_sme_svl(...)` from `test_init`; a negative return
  (EXIT_SKIP, -255 — deliberately not a bare -1, see arm64_vl.h) means
  the helper already logged the skip — `return EXIT_SKIP` immediately. Do
  not hand-write getauxval probes: `device_has_feature()` is
  short-circuited by compile-time `__ARM_FEATURE_*` macros in +sve/+sme
  TUs (docs/architecture.md).
- **Range check, never single-value match**: gate on
  `min_bits <= VL <= max_bits` (128..1024-bit covers all four tiers); an
  out-of-range VL skips with the measured value in the message. A test
  that only runs at exactly one VL must say why in its docblock.
- **Fail-closed on the run side**: re-check the VL in `test_run` and
  `report_fail` on drift from the value recorded at init — never
  continue with stale geometry.
- **Geometry scales, element sizes stay immediate** (the PR #242
  invariant): size buffers by the helper's return (or svcnt/SVL) at
  init, registerize strides and loop bounds in inline asm, keep only
  element-size immediates (e.g. `sizeof(float)` multiplies, `lsl #2`).
- **Golden comparison is byte-exact per lane/element**: memcmp /
  memcmp_or_fail against a golden recomputed for the actual VL;
  sum/xor/reduce cross-lane aggregation is forbidden (it can cancel a
  flipped bit).
```

- [ ] **Step 2: 构建（纯文档 no-op）+ 提交**

与 Task 2 同分支、独立 commit（不同单元）。

### Task 4: 文档收尾 + PR

**Files:**
- Modify: `docs/research/progress-log.md`（尾部 P2 条目）
- PR：`feat/sme-fmopa-za-allvl` → main（正文含 Task 1 四档实录表 + Task 2/3 说明）

- [ ] **Step 1: progress-log P2 条目**（真实数据：轴向裁决、SMSTART 裁决、16 run 表、本机 SKIP/构建实录、三重缺陷修复记载、无回归锚声明）
- [ ] **Step 2: 全量验证矩阵**（构建零告警 / `-e sme_fmopa_za_arm -v` skip 文案 / zstd19 / `--dump-cpu-info` 不变）
- [ ] **Step 3: 提交推送 + PR**（若 #277 已合并先 rebase main）；PR 正文交叉引用 gem5-fi `feat/fmopa-za-dual` 与 PR #277。

---

## Erratum（2026-10-24，PR #279 五 bot 检视闭环）

1. **`/32` 单位错误（Critical）**：原模板 init/run 两处 `svl / 32` 把
   arm64_require_sme_svl 返回的 **SVL 字节**当作位处理，差 8×——128-bit 档
   cols=0（空 golden + memcmp 长度 0 恒过的**无操作假 PASS** + st1w 向空缓冲
   写的堆溢出），256-1024-bit 档前缀一致假 PASS + 越界写。实现分支
   `feat/sme-fmopa-za-allvl` 提交 38faa1b3 已在代码侧修复（`/32→/4` + 显式
   cols∈[4,32] 检查），本计划模板本节同步修正，并指出根因：**P2 计划模板
   自相矛盾**（Interfaces 说明写"字节"，代码块写 `/32`）。
2. **Task 1 两项裁决已落定**（原模板为未决占位）：**axis=COLUMN**
   （`za0v.s[w12,0]` 读列 0）；**sm+za 双切强制**（za-only 下首条 FMOPA 即
   陷阱，FMOPA 需 SVCR.SM=1）——证据 = gem5-fi `feat/fmopa-za-dual @
   ebe08632` 四档 16 run 全绿。kernel 代码块已按裁决更新。
3. **取证脚本取码修正**：原 `exit=$?` 取的是管道末 grep 的退出码而非
   gem5 的（gem5 崩溃但有匹配行 → 假 exit=0 记录）；改 `PIPESTATUS[0]` +
   overall 汇总。kernel 输出补 `checksum=` 标签（原裸十六进制行被自家
   grep 过滤式排除）、axis 双失配时输出 UNKNOWN 而非武断 COLUMN、
   fnv/比对 memcpy 化（消严格别名 UB、字节级 per-lane）。
4. **跳过返回值契约勘误**：Interfaces 与 Task 3 政策原文"-1 = 已
   log_skip"系错误——实际契约是 EXIT_SKIP(-255)（arm64_vl.h 头注明确
   "not a bare -1"）。两处已改，Task 3 文本将进入 writing_tests.md 持久
   指南，此处勘误杠杆最高。
5. 评审出处：PR #279 五 bot 检视（coderabbit 6 条 / local-claude 6 条 /
   pr-agent、codex、greptile 限额类），处置明细见 PR 描述。自检失配
   "test bug, not SDC" 分类措辞中性化（自检走同一硬件路径，无独立 oracle
   依据支撑分类断言）。

## Self-Review 记录

- **Spec 覆盖**：§6 P2 四要素（新 API ✓Task2/bits 区间 + 派生 cols∈[4,32] 双重检查 ✓/`#16` 寄存器化 ✓/dual kernel ✓Task1）；§7 协议落在 Task 1（16 run + 无回归锚的诚实替代：通道 A 四档 golden 逐字节一致）；§8 SME fmopa_za 行（标量累加 ✓——精确整数）；§9 SKIP 语义（require 助手内置）；§10 writing_tests.md（0c6c9c89 归属 ✓Task3）。✓
- **占位符**：Task 2 中两处"[Task 1 裁决]"是**跨任务数据依赖**（轴向/SMSTART 由实证决定，非 TBD——Task 1 产出明确定义）；其余无占位。✓
- **类型一致性**：`fmopa_chain_asm(const float*, const float*, size_t, float*, size_t)` 与 golden/init/run 调用一致；`arm64_require_sme_svl/ arm64_sme_svl_bytes` 签名与 PR #277 一致。✓
- **Review Focus 归属**：1/2→Task 1 Step 4；3→Task 1 通道 A + Task 2 自检；4→Task 2 Step 2/3；5→Task 1 设计不变式 + 16 run；6→Task 2 尺寸 + 四档 cols=32。✓
