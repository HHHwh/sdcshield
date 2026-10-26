# SVE/SME 全 VL 二期 P0+P1 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 落地 all-VL Phase 2 的 P0（5 项真相快修）与 P1（框架层 `arm64_vl.h` VL/SVL API + selftest + dump-cpu-info + gem5 四档验证 + 文档），为 P2-P6 铺路。

**Architecture:** 框架优先（spec D3）：新建 `framework/device/cpu/arm64/arm64_vl.h` 单头文件（纯查询 + 测试流助手两层，`.inst` 定数编码保证任何 aarch64 TU 可用），P0 五项独立小 PR，P1 单分支三提交 + gem5-fi 跨仓探针。

**Tech Stack:** C/C++（GCC 12/clang, meson/ninja）、Linux prctl/getauxval、gem5-fi（CHAOS/SE，`/home/sdc/wangxu/code-review/gem5-fi`）。

**Spec:** `docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md`（本计划从 spec 立论，执行者须同时读两者）

## Global Constraints

- 本机 = 鲲鹏 920（CPU part 0xd01，96 核）**无 SVE/SME**：只能验证构建零告警 + SKIP 路径 + selftest；SVE/SME 功能结论必须来自 gem5 四档真实 run 输出（CLAUDE.md 规则 2b / AGENTS.md:26-32）。gem5-fi 真实路径 `/home/sdc/wangxu/code-review/gem5-fi`（CLAUDE.md 现写的 `gem5-fi-fuzz` 是过时的，P0-4 修）。
- 首次构建：`PKG_CONFIG_PATH=./third-party/eigen5 meson setup builddir --buildtype=release && ./scripts/build.sh`（并行度 nproc/2 由脚本管）；meson 文件改动后须 `meson setup --reconfigure builddir`。
- 每任务回归：`./builddir/sdcshield -e zstd19 -t 2000 -n 1` → `exit: pass`。
- DCO：`git commit -s`，最后一行 `Signed-off-by: wangxu <wangxumarshall@qq.com>`，其后无任何内容、无 Co-Authored-By。仓库本地已配 `user.name/user.email`（若新 shell 报身份警告，先 `git config user.name "wangxu" && git config user.email "wangxumarshall@qq.com"`）。
- 永不直接提交 main：每任务 `git branch --show-current` 确认后从 main 切独立分支；验证 → commit → push。
- x86-64 零改动（additive only）；未实现功能 EXIT_SKIP + "to be implemented" 文案（placeholder honesty）。
- P0 五项各自独立分支/PR；P1 在 `feat/arm64-vl-api` 单分支分提交。

## Review Focus

1. **rdvl/rdsvl `.inst` 编码错误** → 执行错误指令（UNDEF/垃圾 VL）。测试：Task 6 Step 1 汇编器推导复核（`0x04bf5020`/`0x04bf5820`）+ Task 8 gem5 四档 `rdvl == 16×T` 实测。
2. **无 SVE 硬件上执行 rdvl → SIGILL**。测试：Task 6 的 selftest 在本机（无 SVE）跑通即能力守卫生效的活体证据。
3. **require 助手 skip 通道类别/文案错** → 覆盖缺失被掩盖。测试：Task 6 `selftest_arm64_vl_require_sve` 在本机输出干净 skip（CpuNotSupportedSkipCategory）。
4. **pin 语义误用（返回值/线程继承假设错）** → size-specific TU 静默跑错 VL。测试：Task 6 pin 往返断言 + 本机 `-EINVAL` 断言 + Task 8 gem5 prctl 通道如实记录。
5. **x86 构建破坏**（头文件被非 arm64 TU 引入）。测试：全任务 release 构建绿 + `arm64_vl.h` 全文件 `#if defined(__aarch64__)` 守卫 + `#else` 桩（构造性 inspection）。
6. **未初始化 sizeless 向量在 -O2 -Werror 再爆**（mla 漏网同类）。测试：Task 1 与 Task 6 的 release 全量构建零告警。

---

### Task 1: P0-1 mla 探针补 svundef 初始化（c8ac06fe 漏网）

**Files:**
- Modify: `tests/cpu/sve/case/neon_rot_ldr_at_top_mla_sve.cpp:117` 和 `:163`

**Interfaces:**
- Consumes: 无（独立修复）
- Produces: 无下游依赖

背景：c8ac06fe（2026-10-22）修了 14 个文件的 `'svuint64_t res;' → '= svundef_u64()'`，commit message 声称 "mla_sve 无此模式跳过"——**该声明不实**：mla 以 `svuint32_t` 形态存在同一模式（117 行 init、163 行 run，均紧邻 `switch (i % 4)` 无 default）。当前 release 构建未触发告警（GCC 的 maybe-uninitialized 分析未在 u32 路径命中），本修复是同类一致性补齐，`svundef` 编译为空指令、零运行时语义变化。

- [ ] **Step 1: 修改两处声明**

`tests/cpu/sve/case/neon_rot_ldr_at_top_mla_sve.cpp:117`：
```cpp
        svuint32_t res = svundef_u32();
```
（替换 `        svuint32_t res;`，init 内、`switch (i % 4)` 之前）

`:163` 同样替换（run 的 TEST_LOOP 内）：
```cpp
            svuint32_t res = svundef_u32();
```

- [ ] **Step 2: 从 main 切分支并构建**

```bash
git checkout main && git checkout -b fix/sve-mla-svundef-init
./scripts/build.sh    # 无 builddir 时先: PKG_CONFIG_PATH=./third-party/eigen5 meson setup builddir --buildtype=release
```
Expected: 构建零失败零新告警。

- [ ] **Step 3: 验证 SKIP 路径不变 + 回归**

```bash
./builddir/sdcshield -e neon_rot_ldr_at_top_mla_sve -t 2000 -n 1
./builddir/sdcshield -e zstd19 -t 2000 -n 1
```
Expected: 前者干净 skip（本机无 SVE，框架 "test compiled with sve" 门）；zstd19 `exit: pass`。

- [ ] **Step 4: 提交推送 + PR**

```bash
git add tests/cpu/sve/case/neon_rot_ldr_at_top_mla_sve.cpp
git commit -s -m "fix(sve-case): neon_rot_ldr_at_top_mla_sve 补 svundef 初始化 (c8ac06fe 漏网)

c8ac06fe 声称 mla_sve 无 switch 裸声明模式, 实为 u32 形态存在于
init(117)/run(163); 当前未触发 -Werror 但属同类隐患, 补齐一致性。
svundef 编译为空指令, 零运行时语义变化。"
git push -u origin fix/sve-mla-svundef-init
```
PR 标题同 commit 主题；正文引用上面验证输出。PR 描述末尾加：`🤖 Generated with [Claude Code](https://claude.com/claude-code)`

---

### Task 2: P0-2 sve_rot×5 构建注释与 docblock 真相化

**Files:**
- Modify: `tests/cpu/meson.build:445-451`（注释块）
- Modify: `tests/cpu/sve/sve_rot_ldr_at_top.cpp`、`sve_rot_ldr_at_top_rowmajor.cpp`、`sve_rot_ldr_at_top_rowmajor_k5inter_rand.cpp`、`sve_rot_ldr_at_top_rowmajor_k7pattern.cpp`、`sve_rot_ldr_at_top_rowmajor_k3res.cpp`（各 docblock 中 "true VLA" 声明）

**Interfaces:** 无依赖、无下游。

事实：5 文件 docblock 与 meson 注释均声称 "true VLA — no -msve-vector-bits"，实际注册进 `tests_set_sve`，编译带 `-msve-vector-bits=128`（`tests/cpu/meson.build:990`）。真实迁移（进真 VLA 库 + gem5 四档验证）排期在 spec P5；本任务只让文档说真话。

- [ ] **Step 1: 改 meson 注释块**

`tests/cpu/meson.build:445-451` 整块替换为：
```meson
# ARM64-only SVE SDC probes: SVE/SVE2 ports of the core-179 NEON attribution
# family (tests/cpu/misc/neon_rot_ldr_at_top*.cpp). They are registered in
# tests_set_sve, which compiles them with -march=armv8.2-a+sve
# -msve-vector-bits=128 (see the tests_sve build block below): fixed 128-bit
# SVE semantics, guaranteed correct only at runtime VL == 128 (the
# size-specific rule in docs/architecture.md); on VL != 128 silicon their
# behavior is in the same undefined class documented for
# eigen_gemm/gemm_double_max_arm.cpp:40-47. The framework auto-skips them on
# non-SVE hardware via compiler_minimum_device (framework/sandstone_run.cpp).
# The Eigen namespace-rename macros on that library are inert for these
# files (they never include Eigen). Scheduled: migrate to a true-VLA library
# in all-VL Phase 2 P5 (docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md
# §6). x86-64 untouched.
```

- [ ] **Step 2: 改 5 个 docblock**

`grep -n 'true VLA' tests/cpu/sve/sve_rot_ldr_at_top*.cpp` 逐处将：
```
     *     unchanged on 128b/256b/512b hardware (true VLA, no
     *     -msve-vector-bits);
```
替换为：
```
     *     unchanged on 128b/256b/512b hardware (VLA by design; NOTE:
     *     currently compiled in tests_set_sve with -msve-vector-bits=128,
     *     so guaranteed correct only at runtime VL == 128 until the
     *     all-VL Phase 2 P5 migration — see
     *     docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md §6);
```
（5 文件措辞统一；行折行按各文件实际宽度对齐）

- [ ] **Step 3: reconfigure + 构建 + 回归**

```bash
git checkout main && git checkout -b docs/sve-rot-vl-reality
meson setup --reconfigure builddir && ./scripts/build.sh
./builddir/sdcshield -e zstd19 -t 2000 -n 1
```
Expected: 构建绿；zstd19 pass（纯注释/文档改动，无行为变化，无需 gem5）。

- [ ] **Step 4: 提交推送 + PR**

```bash
git add tests/cpu/meson.build tests/cpu/sve/sve_rot_ldr_at_top*.cpp
git commit -s -m "docs(sve): sve_rot_ldr_at_top×5 构建归属真相化 (实编 -msve-vector-bits=128)

docblock/meson 注释声称 true VLA, 实际注册 tests_set_sve 带 128-bit
固定 flag(meson.build:990); 真迁移排期 all-VL Phase 2 P5。纯文档,
零行为变化。" 
git push -u origin docs/sve-rot-vl-reality
```

---

### Task 3: P0-3 孤儿文件状态注释

**Files:**
- Modify: `tests/cpu/arm64/meson.build:139-147`（tests_arm64_sve 库注释块尾部追加一行）

**Interfaces:** 无。

事实：`tests/cpu/arm64/sve512_fma_arm.cpp`（550 行，commit ea879f68 2026-10-11 "sve512" 单文件提交）从未进入任何 sources 列表。处置：不删（P3 全 VL 战役自然接管接线+验证），先在构建文件里把状态写明。

- [ ] **Step 1: 注释追加**

在 `tests/cpu/arm64/meson.build` 的 `# SVE full-vector-length (sve512_fma family)...` 注释块末尾（`arm64_sve_lib = static_library(` 之前）追加：
```meson
    # NOTE: sve512_fma_arm.cpp is committed (ea879f68) but deliberately NOT
    # wired into any sources list yet — wiring + all-VL validation is
    # scheduled for all-VL Phase 2 P3
    # (docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md §6).
```

- [ ] **Step 2: 构建 + 提交推送 + PR**

```bash
git checkout main && git checkout -b docs/sve512-fma-orphan-note
meson setup --reconfigure builddir && ./scripts/build.sh
git add tests/cpu/arm64/meson.build
git commit -s -m "docs(build): 标注 sve512_fma_arm.cpp 孤儿状态 (未接线, 排期 P3)"
git push -u origin docs/sve512-fma-orphan-note
```

---

### Task 4: P0-4 CLAUDE.md 环境真相修正

**Files:**
- Modify: `CLAUDE.md`（两处：规则 2b 的 gem5 路径；Known platform quirks 的 SVE 条目）

**Interfaces:** 无。

- [ ] **Step 1: 修 gem5 路径**

CLAUDE.md 规则 2b 中：
```
`/home/sdc/wangxu/gem5-fi-fuzz`
```
替换为：
```
`/home/sdc/wangxu/code-review/gem5-fi`
```

- [ ] **Step 2: 修过时的 host VL 陈述**

Known platform quirks 中：
```
- SVE runtime-VL rule: host is VL=256 while `tests_sve` compiles at 128 → the eigen SVD SVE test needs the VL-pinning launcher in `scripts/eigen-sve-double/`; `sleef_sve` stays at 128 by design.
```
替换为：
```
- SVE runtime-VL rule: this host (0xd01, Kunpeng 920, 96 CPUs) has NO SVE/SME — all SVE/SME functional verification is gem5-fi only (`--sve_vl/--sme_vl {1,2,4,8}` = 128/256/512/1024-bit). Earlier "host is VL=256" records refer to a different machine (cortex x3b, docs/architecture.md:130). `tests_sve` still compiles at fixed 128-bit; on SVE hosts with VL≠128 the eigen SVD test needs a VL-pinning launcher (`scripts/eigen-sve-double/` precedent; the external `/tmp/vlrun` tool no longer exists); `sleef_sve` stays at 128 by design.
```

> **2026-10-26 更正（本步执行时未生效，事后修正）**：上面这段替换文本**不要照抄**——
> 本机实测 MIDR `0x480fd020`（part `0xd02`）**有 SVE**，默认 VL=256-bit（`rdvl`=32B），
> 可 pin 到 128-bit；HWCAP2 无 `SVE2`/`SME`（SVE2 指令实测 SIGILL；SME 仅"软件不可见"）。
> 现行规则见 CLAUDE.md 第 2 条"Probe the host before assuming anything"与平台怪癖节
> （改为按机探测：支持的档位本机原生验证，不支持的才走 gem5-fi）。本注记只更正事实，
> 不改写当日结论。

- [ ] **Step 3: 提交推送 + PR**

```bash
git checkout main && git checkout -b docs/claude-md-env-truth
git add CLAUDE.md
git commit -s -m "docs(claude): 修正 gem5-fi 实际路径与本机无 SVE 事实 (0xd01)"
git push -u origin docs/claude-md-env-truth
```

---

### Task 5: P0-5 progress-log 补 PR #242 条目

**Files:**
- Modify: `docs/research/progress-log.md`（在 `## 2026-10-16/17` 条目之前插入，即 510 行"检测战役"bullet 之后）

**Interfaces:** 无。

- [ ] **Step 1: 插入条目**

```markdown
## 2026-10-15（PR #242：SVE/SME 全 VL 规格化一期——三测试四档实证）

- **合并**：merge `2b016d9c`（"WIP: feat/sve-sme-all-vl"）；计划载体
  `docs/superpowers/plans/2026-10-13-sve-sme-all-vl.md`（本条目补记——此前 progress-log 漏记）。
- **三测试全 VL 化**：`sve512_stencil_axis_arm`（bt=2×svcntd()，[4,32]）、
  `sme_fmopa_stress_arm`（n=svl/4，[4,32]，run 侧 assert）、
  `sme_za_tileview_arm`（n=svl/8，[2,16]，kTileNMax=16）。
- **gem5-fi 四档实证**：dual kernel 四档（128/256/512/1024）× {fixed,
  random×3} 共 48 run 全绿（findings.md `4f88821a` 记录）；512-bit 回归锚
  逐位一致（za_tileview `e9e2be2a`、fmopa_stress `892e4ba8`、stencil
  `610e3ce5`）。kernel 归属：fmopa_dual 已合 main（`9dbbbbba`）；
  za_tileview/stencil 全 VL 版在 `feat/sve-sme-all-vl`（`b75c7a31`）未合回。
- **review 修正固化**：范围检查替代单值门控；run 侧 fail-closed assert；
  golden 逐 lane 字节级 memcmp 禁跨 lane 聚合。
- **诚实边界**：当年 run 在 operator 工作仓完成（本机不可复现）；五相位
  za_tileview dual kernel 不在本机 checkout（缺口记 all-VL Phase 2 spec §7）。
- **承接**：all-VL Phase 2 spec
  `docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md`（P0-P6）。
```

- [ ] **Step 2: 提交推送 + PR**

```bash
git checkout main && git checkout -b docs/progress-log-pr242
git add docs/research/progress-log.md
git commit -s -m "docs(progress): 补记 PR #242 (all-VL 一期) 条目与诚实边界"
git push -u origin docs/progress-log-pr242
```

---

### Task 6: P1-1 框架层 arm64_vl.h + 两个 selftest

**Files:**
- Create: `framework/device/cpu/arm64/arm64_vl.h`
- Modify: `framework/selftest.cpp`（run 函数插在 `selftest_sme_sigill_probe_run` 之后 ~941 行；注册插在 2850 行 `#endif` 之后、`};` 之前；includes 加在文件头部 arm64 守卫内）

**Interfaces:**
- Produces（P2-P6 与 Task 7/8/9 依赖）:
  - `#include <device/cpu/arm64/arm64_vl.h>`（框架根在 include path，先例 `<device/cpu/topology_cpu.h>`）
  - `bool arm64_cpu_has_sve(void)` / `bool arm64_cpu_has_sve2(void)` / `bool arm64_cpu_has_sme(void)`
  - `long arm64_sve_vl_bytes(void)` / `long arm64_sme_svl_bytes(void)`（字节；无能力返回 0）
  - `long arm64_sve_pin_vl(long bytes)` / `long arm64_sme_pin_vl(long bytes)`（返回生效字节；失败 -errno）
  - `long arm64_require_sve_vl(struct test *, long min_bits, long max_bits)` / `long arm64_require_sme_svl(struct test *, long min_bits, long max_bits)`（>0 = VL 字节；-1 = 已 log_skip）

- [ ] **Step 1: 复核指令编码（防推导漂移）**

```bash
printf '.arch armv8.2-a+sve\n.text\nrdvl x0, #1\n' > /tmp/vl1.s && gcc -c /tmp/vl1.s -o /tmp/vl1.o && objdump -d /tmp/vl1.o | tail -2
printf '.arch armv9-a+sve+sme\n.text\nrdsvl x0, #1\n' > /tmp/vl2.s && gcc -c /tmp/vl2.s -o /tmp/vl2.o && objdump -d /tmp/vl2.o | tail -2
```
Expected: `04bf5020  rdvl x0, #1` 与 `04bf5820  rdsvl x0, #1`。若不同，以本次输出为准更新头文件常量并记录。

- [ ] **Step 2: 先写 selftest（失败态）**

`framework/selftest.cpp` 头部（与其他 arm64 include 同区）加：
```cpp
#if defined(__aarch64__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif
```
`selftest_sme_sigill_probe_run`（~941 行）之后加 Task 6 代码块 A（见下）。2850 行 `#endif` 后加代码块 B（注册）。

**代码块 A（run 函数）：**
```cpp
#if defined(__aarch64__)
// All-VL Phase 2 P1: arm64_vl.h API invariants. On SVE-less hosts (this
// Kunpeng 920) queries must report absence (false/0, pin -EINVAL) and agree
// with getauxval ground truth; on SVE hosts VL must be a 16-byte multiple in
// [16,256]. Functional SVE behavior is validated in gem5-fi
// (workloads/sdcshield/vl_api_probe.c, four tiers).
static int selftest_arm64_vl_api_run(struct test *test, int cpu)
{
    (void)cpu;

    if (arm64_cpu_has_sve() != !!(getauxval(AT_HWCAP) & HWCAP_SVE))
        report_fail_msg("%s: arm64_cpu_has_sve() disagrees with getauxval(AT_HWCAP)", test->id);
    if (arm64_cpu_has_sve2() != !!(getauxval(AT_HWCAP2) & HWCAP2_SVE2))
        report_fail_msg("%s: arm64_cpu_has_sve2() disagrees with getauxval(AT_HWCAP2)", test->id);
    if (arm64_cpu_has_sme() != !!(getauxval(AT_HWCAP2) & HWCAP2_SME))
        report_fail_msg("%s: arm64_cpu_has_sme() disagrees with getauxval(AT_HWCAP2)", test->id);

    long vl = arm64_sve_vl_bytes();
    if (arm64_cpu_has_sve()) {
        if (vl < 16 || vl > 256 || (vl % 16) != 0)
            report_fail_msg("%s: SVE VL %ld bytes is not 16-aligned in [16,256]", test->id, vl);
    } else if (vl != 0) {
        report_fail_msg("%s: arm64_sve_vl_bytes() returned %ld without SVE", test->id, vl);
    }

    long svl = arm64_sme_svl_bytes();
    if (arm64_cpu_has_sme()) {
        if (svl < 16 || svl > 256 || (svl % 16) != 0)
            report_fail_msg("%s: SME SVL %ld bytes is not 16-aligned in [16,256]", test->id, svl);
    } else if (svl != 0) {
        report_fail_msg("%s: arm64_sme_svl_bytes() returned %ld without SME", test->id, svl);
    }

    if (arm64_cpu_has_sve()) {
        long applied = arm64_sve_pin_vl(vl);
        if (applied != vl)
            report_fail_msg("%s: pin to current VL (%ld) applied %ld", test->id, vl, applied);
    } else if (arm64_sve_pin_vl(16) != -EINVAL) {
        report_fail_msg("%s: arm64_sve_pin_vl() succeeded on an SVE-less host", test->id);
    }

    return EXIT_SUCCESS;
}

// The require-helper path: on SVE-less hosts the framework must report a
// clean EXIT_SKIP from init (never a crash); on SVE hosts run re-checks the
// VL recorded at init (fail-closed against init/run drift).
static int selftest_arm64_vl_require_sve_init(struct test *test)
{
    long vl = arm64_require_sve_vl(test, 128, 1024);
    if (vl < 0)
        return EXIT_SKIP; /* log_skip already emitted with the reason */
    test->data = (void *)(uintptr_t)vl;
    return EXIT_SUCCESS;
}

static int selftest_arm64_vl_require_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    long vl_at_init = (long)(uintptr_t)test->data;
    if (arm64_sve_vl_bytes() != vl_at_init)
        report_fail_msg("%s: SVE VL drifted between init (%ld) and run (%ld)",
                        test->id, vl_at_init, arm64_sve_vl_bytes());
    return EXIT_SUCCESS;
}
#endif
```

**代码块 B（注册，插在 2850 行 `#endif` 之后、`};` 之前）：**
```cpp
#if defined(__aarch64__)
{
    .id = "selftest_arm64_vl_api",
    .description = "arm64_vl.h API invariants (capability/VL consistency, pin round-trip)",
    .groups = DECLARE_TEST_GROUPS(&group_test_hw_features),
    .test_run = selftest_arm64_vl_api_run,
    .desired_duration = -1,
    .quality_level = TEST_QUALITY_PROD,
},
{
    .id = "selftest_arm64_vl_require_sve",
    .description = "arm64_require_sve_vl() clean-skip/run path (skips cleanly on SVE-less hosts)",
    .groups = DECLARE_TEST_GROUPS(&group_test_hw_features),
    .test_init = selftest_arm64_vl_require_sve_init,
    .test_run = selftest_arm64_vl_require_sve_run,
    .desired_duration = -1,
    .quality_level = TEST_QUALITY_PROD,
},
#endif
```

- [ ] **Step 3: 构建确认失败态**

```bash
git checkout main && git checkout -b feat/arm64-vl-api
./scripts/build.sh
```
Expected: **失败**——`framework/device/cpu/arm64/arm64_vl.h` 不存在（include error）。这就是本任务的"红灯"。

- [ ] **Step 4: 写 arm64_vl.h（绿灯实现）**

创建 `framework/device/cpu/arm64/arm64_vl.h`（版权头镜像同目录 `arm64_cpuid.h` 的 SPDX 块）：
```c
#ifndef SANDSTONE_ARM64_VL_H
#define SANDSTONE_ARM64_VL_H

/*
 * ARM64 SVE/SME vector-length (VL/SVL) layer — all-VL Phase 2 (P1).
 * Spec: docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md §5.
 *
 * Layer 1 (pure queries): usable from any aarch64 TU regardless of -march —
 *   rdvl/rdsvl are .inst-encoded (same compiler-independence trick as
 *   selftest.cpp's smstart probe); getauxval/prctl are syscalls.
 * Layer 2 (test-flow helpers): capability check + VL range check + log_skip
 *   in one call, replacing per-test getauxval boilerplate.
 *
 * Hard boundary: VL/SVL are per-thread runtime quantities, deliberately NOT
 * part of the process-wide device_features bitmask. Self-pinning is meant
 * for test_init, before worker threads are created (new threads inherit the
 * creator's VL).
 */

#include <stdbool.h>
#include <sandstone.h>

#if defined(__aarch64__)
#include <errno.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <asm/hwcap.h>

/* prctl numbers/masks, for build environments whose linux/prctl.h predates
 * kernel 5.19 (SME). Values per uapi linux/prctl.h. */
#ifndef PR_SVE_SET_VL
#define PR_SVE_SET_VL 50
#endif
#ifndef PR_SVE_GET_VL
#define PR_SVE_GET_VL 51
#endif
#ifndef PR_SVE_VL_LEN_MASK
#define PR_SVE_VL_LEN_MASK 0xffff
#endif
#ifndef PR_SME_SET_VL
#define PR_SME_SET_VL 63
#endif
#ifndef PR_SME_GET_VL
#define PR_SME_GET_VL 64
#endif
#ifndef PR_SME_VL_LEN_MASK
#define PR_SME_VL_LEN_MASK 0xffff
#endif

/* ---- capability probes: honest runtime, immune to the compile-time
 * short-circuit of device_has_feature() in +sve/+sme-compiled TUs ---- */

static inline bool arm64_cpu_has_sve(void)
{
    return (getauxval(AT_HWCAP) & HWCAP_SVE) != 0;
}

static inline bool arm64_cpu_has_sve2(void)
{
    return (getauxval(AT_HWCAP2) & HWCAP2_SVE2) != 0;
}

static inline bool arm64_cpu_has_sme(void)
{
    return (getauxval(AT_HWCAP2) & HWCAP2_SME) != 0;
}

/* ---- current-thread vector lengths (bytes; 0 without the feature) ---- */

/* rdvl x0, #1 == .inst 0x04bf5020 (derived via as -march=armv8.2-a+sve +
 * objdump, 2026-10-23). The destination is pinned to x0 by the local
 * register binding; the encoding's Rd field must stay 0. */
static inline long arm64_sve_vl_bytes(void)
{
    if (!arm64_cpu_has_sve())
        return 0;
    register long vl __asm__("x0");
    asm volatile(".inst 0x04bf5020" : "=r"(vl));
    return vl;
}

/* rdsvl x0, #1 == .inst 0x04bf5820 (derived via as -march=armv9-a+sve+sme). */
static inline long arm64_sme_svl_bytes(void)
{
    if (!arm64_cpu_has_sme())
        return 0;
    register long vl __asm__("x0");
    asm volatile(".inst 0x04bf5820" : "=r"(vl));
    return vl;
}

/* ---- self-pinning (size-specific TUs; call from test_init) ---- */

static inline long arm64_sve_pin_vl(long bytes)
{
    if (!arm64_cpu_has_sve())
        return -EINVAL;
    int rc = prctl(PR_SVE_SET_VL, (unsigned long)(bytes & PR_SVE_VL_LEN_MASK));
    if (rc < 0)
        return -errno;
    return rc & PR_SVE_VL_LEN_MASK;
}

static inline long arm64_sme_pin_vl(long bytes)
{
    if (!arm64_cpu_has_sme())
        return -EINVAL;
    int rc = prctl(PR_SME_SET_VL, (unsigned long)(bytes & PR_SME_VL_LEN_MASK));
    if (rc < 0)
        return -errno;
    return rc & PR_SME_VL_LEN_MASK;
}

/* ---- test-flow helpers ---- */

static inline long arm64_require_sve_vl(struct test *test, long min_bits, long max_bits)
{
    if (!arm64_cpu_has_sve()) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SVE not available; %s exercises the SVE datapath", test->id);
        return -1;
    }
    long bits = arm64_sve_vl_bytes() * 8;
    if (bits < min_bits || bits > max_bits) {
        log_skip(CpuNotSupportedSkipCategory,
                 "%s: SVE vector length %ld bits is outside the supported %ld..%ld-bit range",
                 test->id, bits, min_bits, max_bits);
        return -1;
    }
    return bits / 8;
}

static inline long arm64_require_sme_svl(struct test *test, long min_bits, long max_bits)
{
    if (!arm64_cpu_has_sme()) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SME not available; %s exercises the SME datapath", test->id);
        return -1;
    }
    long bits = arm64_sme_svl_bytes() * 8;
    if (bits < min_bits || bits > max_bits) {
        log_skip(CpuNotSupportedSkipCategory,
                 "%s: SME streaming vector length %ld bits is outside the supported %ld..%ld-bit range",
                 test->id, bits, min_bits, max_bits);
        return -1;
    }
    return bits / 8;
}

#else /* !__aarch64__: include-safety stubs; x86-64 untouched by design. */

static inline bool arm64_cpu_has_sve(void)   { return false; }
static inline bool arm64_cpu_has_sve2(void)  { return false; }
static inline bool arm64_cpu_has_sme(void)   { return false; }
static inline long arm64_sve_vl_bytes(void)  { return 0; }
static inline long arm64_sme_svl_bytes(void) { return 0; }
static inline long arm64_sve_pin_vl(long bytes)  { (void)bytes; return -1; }
static inline long arm64_sme_pin_vl(long bytes)  { (void)bytes; return -1; }
static inline long arm64_require_sve_vl(struct test *test, long min_bits, long max_bits)
{
    (void)test; (void)min_bits; (void)max_bits; return -1;
}
static inline long arm64_require_sme_svl(struct test *test, long min_bits, long max_bits)
{
    (void)test; (void)min_bits; (void)max_bits; return -1;
}

#endif /* __aarch64__ */
#endif /* SANDSTONE_ARM64_VL_H */
```

- [ ] **Step 5: 构建绿灯 + selftest 实跑**

```bash
./scripts/build.sh
./builddir/sdcshield --selftests 2>&1 | grep -iA2 'selftest_arm64_vl' ; echo "exit=$?"
```
Expected: 构建零告警；`selftest_arm64_vl_api` pass；`selftest_arm64_vl_require_sve` 干净 skip（CpuNotSupportedSkipCategory，本机无 SVE）。若 `--selftests` 整体退出码受 negative 组影响非零，如实记录输出并只以本两用例行为为准。

- [ ] **Step 6: 全量回归 + 提交**

```bash
./builddir/sdcshield -e zstd19 -t 2000 -n 1
git add framework/device/cpu/arm64/arm64_vl.h framework/selftest.cpp
git commit -s -m "feat(arm64): 框架层 VL/SVL API (arm64_vl.h) + selftest

all-VL Phase 2 P1: 能力探测(getauxval, 免编译期宏短路)/VL 查询(rdvl
rdsvl .inst 定数 0x04bf5020/0x04bf5820)/自 pin(prctl)/测试流助手
(范围检查+log_skip 一行)。VL 不进 device_features(per-thread 语义)。
本机(无 SVE)验证: selftest_arm64_vl_api pass + require_sve 干净 skip;
功能四档验证见 gem5-fi vl_api_probe(后续提交)。"
git push -u origin feat/arm64-vl-api
```

---

### Task 7: P1-2 dump-cpu-info 输出 VL/SVL

**Files:**
- Modify: `framework/device/cpu/cpu_device.cpp`（`dump_device_info()` 内，"Detected CPU" printf 之后、`# CPU` 表头之前；文件头 include 区加 `#include "arm64/arm64_vl.h"`）

**Interfaces:**
- Consumes: Task 6 的 `arm64_cpu_has_sve/sme`、`arm64_sve/sme_svl_bytes`

- [ ] **Step 1: 插入输出块**

在 `dump_device_info()` 的 `device_features_to_string(device_features).c_str());` 行之后插入：
```cpp
#if defined(__aarch64__)
    /* Per-thread vector lengths, distinct from the process-wide feature
       bits above; see docs/architecture.md "SVE/SME vector-length layer". */
    if (arm64_cpu_has_sve())
        printf("SVE vector length: %ld bits (%ld bytes)\n",
               arm64_sve_vl_bytes() * 8, arm64_sve_vl_bytes());
    else
        printf("SVE vector length: n/a (SVE not present)\n");
    if (arm64_cpu_has_sme())
        printf("SME streaming vector length: %ld bits (%ld bytes)\n",
               arm64_sme_svl_bytes() * 8, arm64_sme_svl_bytes());
    else
        printf("SME streaming vector length: n/a (SME not present)\n");
#endif
```
include 区加（与现有 include 并列）：
```cpp
#include "arm64/arm64_vl.h"
```

- [ ] **Step 2: 构建实跑**

```bash
./scripts/build.sh
./builddir/sdcshield --dump-cpu-info | head -10
```
Expected（本机）：`SVE vector length: n/a (SVE not present)` + `SME streaming vector length: n/a (SME not present)` 出现在 Detected CPU 行之后。

- [ ] **Step 3: 回归 + 提交**

```bash
./builddir/sdcshield -e zstd19 -t 2000 -n 1
git add framework/device/cpu/cpu_device.cpp
git commit -s -m "feat(arm64): --dump-cpu-info 输出 SVE VL / SME SVL (含缺席显式标注)"
git push origin feat/arm64-vl-api
```

---

### Task 8: P1-3 gem5-fi vl_api_probe 四档验证（跨仓）

**Files:**
- Create（gem5-fi 仓）: `workloads/sdcshield/vl_api_probe.c` + Makefile 追加目标
- Modify（gem5-fi 仓）: `findings.md`（尾部记录）

**Interfaces:**
- Consumes: Task 6 的 `.inst` 编码与查询逻辑（同源镜像）
- Produces: 四档真实 run 输出（P1 PR 与 progress-log 的功能验证证据，规则 2b）

- [ ] **Step 1: gem5-fi 仓切分支**

```bash
cd /home/sdc/wangxu/code-review/gem5-fi && git branch --show-current   # 预期 main @ 9dbbbbba
git checkout -b feat/vl-api-probe
```

- [ ] **Step 2: 写探针内核**

`workloads/sdcshield/vl_api_probe.c`：
```c
/*
 * vl_api_probe — all-VL Phase 2 P1: validates the sdcshield framework
 * arm64_vl.h query layer (rdvl/rdsvl .inst encodings + getauxval/prctl)
 * in gem5 at all four tiers. Same-source mirror of
 * framework/device/cpu/arm64/arm64_vl.h (encodings 0x04bf5020/0x04bf5820).
 * Spec: docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md §7.
 *
 * Channel A: sanity (VL is 16-byte multiple in [16,256]) → exit 0 / 3.
 * Channel B: FNV-1a 16-hex checksum over reported values.
 * argv: [fixed|random] [seed] — deterministic probe; fixed and random must
 * yield identical checksums per tier (checked by comparing run records).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/prctl.h>
#include <asm/hwcap.h>

static uint64_t fnv1a(uint64_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ULL; }
    return h;
}

static long rdvl_bytes(void)
{
    register long vl __asm__("x0");
    asm volatile(".inst 0x04bf5020" : "=r"(vl)); /* rdvl x0, #1 */
    return vl;
}

static long rdsvl_bytes(void)
{
    register long vl __asm__("x0");
    asm volatile(".inst 0x04bf5820" : "=r"(vl)); /* rdsvl x0, #1 */
    return vl;
}

int main(int argc, char **argv)
{
    int mode_fixed = (argc < 2 || strcmp(argv[1], "fixed") == 0);
    unsigned long hw = getauxval(AT_HWCAP), hw2 = getauxval(AT_HWCAP2);
    long vl = rdvl_bytes(), svl = rdsvl_bytes();
    int psg = prctl(PR_SVE_GET_VL), pmg = prctl(PR_SME_GET_VL);

    printf("vl_api_probe: mode=%s hwcap_sve=%d hwcap2_sme=%d rdvl=%ld rdsvl=%ld "
           "prctl_sve_get=%d prctl_sme_get=%d\n",
           mode_fixed ? "fixed" : "random", !!(hw & HWCAP_SVE), !!(hw2 & HWCAP2_SME),
           vl, svl, psg, pmg);

    int bad = 0;
    if (vl < 16 || vl > 256 || (vl % 16)) { printf("FAIL: rdvl %ld\n", vl); bad = 1; }
    if (svl < 16 || svl > 256 || (svl % 16)) { printf("FAIL: rdsvl %ld\n", svl); bad = 1; }

    uint64_t h = 0xcbf29ce484222325ULL;
    h = fnv1a(h, &vl, sizeof vl); h = fnv1a(h, &svl, sizeof svl);
    h = fnv1a(h, &psg, sizeof psg); h = fnv1a(h, &pmg, sizeof pmg);
    printf("checksum=%016llx SDCSHIELD_FAILS=%d\n", (unsigned long long)h, bad);
    return bad ? 3 : 0;
}
```
Makefile 追加（镜像现有 dual 目标的 clang `-static -O2 -march=armv9-a+sve+sme` 形态）：
```make
vl_api_probe: vl_api_probe.c
	clang -static -O2 -march=armv9-a+sve+sme -o $@ $<
```

- [ ] **Step 3: 四档 × 双模式 8 run**

```bash
make vl_api_probe
G5=/home/sdc/wangxu/code-review/gem5-fi/build/ARM/gem5.opt
for T in 1 2 4 8; do
  for MODE in "fixed 1" "random 2"; do
    $G5 configs/se/arm_chaos.py --cpu O3 --cmd workloads/sdcshield/vl_api_probe \
        --sve_vl $T --sme_vl $T --workload_args "$MODE" 2>&1 | grep -E 'vl_api_probe|checksum|SDCSHIELD_FAILS'
  done
done
```
Expected（逐档）：`rdvl=<16×T>` 且 `rdsvl=<16×T>`（T=1/2/4/8 → 16/32/64/128 字节 = 128/256/512/1024-bit）；全部 `SDCSHIELD_FAILS=0`；同档 fixed/random checksum 相同、跨档互异。prctl 两个 GET 值如实记录（gem5 SE 对 prctl 的支持是诚实边界，不做判定）。每 run 数分钟（O3 仿真），耐心等完，不提前下结论。

- [ ] **Step 4: findings.md 记录 + 提交推送**

findings.md 尾部追加：日期、8 run 的档位/mode/checksum/FAILS 表、prctl 通道观察、结论。然后：
```bash
git add workloads/sdcshield/vl_api_probe.c workloads/sdcshield/Makefile findings.md
git commit -s -m "feat(sdcshield): vl_api_probe 探针, 四档验证 arm64_vl.h 查询层"
git push -u origin feat/vl-api-probe
cd /home/sdc/wangxu/code-review/sdcshield-sve
```

---

### Task 9: P1-4 文档同步 + P1 PR

**Files:**
- Modify: `docs/architecture.md`（80 行 "Sysdeps Layering" 之前插入新节）
- Modify: `docs/research/adding-tests.md:119-120` 与 `:151-153`
- Modify: `README.md`（--dump-cpu-info 输出说明处）
- Modify: `docs/research/progress-log.md`（尾部 P0+P1 战役条目）

**Interfaces:**
- Consumes: Task 6/7/8 的成品与真实验证输出

- [ ] **Step 1: architecture.md 新节**

在 `## Sysdeps Layering`（82 行）之前插入：
```markdown
## SVE/SME vector-length layer (arm64_vl.h)

`framework/device/cpu/arm64/arm64_vl.h` is the single entry point for
per-thread SVE vector length (VL) and SME streaming vector length (SVL):

- capability probes (`arm64_cpu_has_sve/sve2/sme`) — honest runtime
  `getauxval(AT_HWCAP/HWCAP2)` checks that, unlike `device_has_feature()`,
  are not short-circuited by compile-time `__ARM_FEATURE_*` macros in
  +sve/+sme-compiled TUs;
- current-thread VL/SVL queries (`arm64_sve_vl_bytes` /
  `arm64_sme_svl_bytes`) — rdvl/rdsvl via `.inst`, so any aarch64 TU can
  use them without a +sve march;
- self-pinning (`arm64_sve_pin_vl` / `arm64_sme_pin_vl`, prctl) for
  size-specific TUs, to be called from test_init before worker threads
  are created (new threads inherit the creator's VL);
- test-flow helpers (`arm64_require_sve_vl` / `arm64_require_sme_svl`)
  folding capability check + range check + log_skip into one call.

VL/SVL are deliberately **not** part of `device_features`: they are
per-thread runtime quantities, while feature bits are process-wide.
Validated at all four tiers (128/256/512/1024-bit) in gem5-fi
(`workloads/sdcshield/vl_api_probe.c`).
```

- [ ] **Step 2: adding-tests.md 政策改写**

`:119-120` 的 bullet：
```markdown
- `tests_arm64_sve`（同文件）：`-march=armv8.2-a+sve`——SVE 全长向量族（每测试 init 里探
  HWCAP_SVE，无 SVE 硬件干净 skip）
```
改为：
```markdown
- `tests_arm64_sve`（同文件）：`-march=armv8.2-a+sve`——SVE 全长向量族（每测试 init 里
  `#include <device/cpu/arm64/arm64_vl.h>` 后调 `arm64_require_sve_vl(test, min_bits,
  max_bits)` 一行完成能力+范围探测，无 SVE 硬件干净 skip；返回 -1 直接
  `return EXIT_SKIP`）
```
`:151-153` 决策树的 SVE 分支：
```
需要 SVE（<arm_sve.h> / svmla_*）？
  → tests/cpu/arm64/ 源文件 + 加进 tests_arm64_sve 库（+sve march）
     且 init 里必须探 HWCAP_SVE → EXIT_SKIP（鲲鹏 920 无 SVE）
```
改为：
```
需要 SVE（<arm_sve.h> / svmla_*）？
  → tests/cpu/arm64/ 源文件 + 加进 tests_arm64_sve 库（+sve march）
     且 init 里必须 arm64_require_sve_vl() → -1 即 EXIT_SKIP（本机 0xd01 无 SVE）
```

- [ ] **Step 3: README --dump-cpu-info 说明**

`grep -n 'dump-cpu-info' README.md` 定位输出示例处，在示例中补两行：
```
SVE vector length: n/a (SVE not present)
SME streaming vector length: n/a (SME not present)
```
并注明：有 SVE/SME 时输出实测 bits/bytes（per-thread 运行量，与 feature 位图独立）。

- [ ] **Step 4: progress-log 战役条目**

progress-log 尾部追加（引用真实输出，不写预计值）：
```markdown
## 2026-10-23（all-VL Phase 2 P0+P1：真相快修 + 框架 VL/SVL API）

- **P0**：mla svundef 漏网补齐 / sve_rot×5 构建真相 / 孤儿标注 /
  CLAUDE.md 环境修正 / PR #242 补记——各独立 PR。
- **P1**：arm64_vl.h（能力/VL 查询/自 pin/require 助手）+ selftest×2 +
  --dump-cpu-info 输出。本机（0xd01 无 SVE）：selftest_arm64_vl_api pass、
  require_sve 干净 skip、dump 输出 n/a 两行、release 零告警、zstd19 pass。
- **gem5 四档**：vl_api_probe 8 run（四档 × fixed/random）rdvl/rdsvl =
  16/32/64/128 字节全对、SDCSHIELD_FAILS=0、（粘贴各档 checksum）。
  prctl 通道观察：（如实粘贴）。
- **承接**：P2（SME 收尾）起各阶段另行计划。
```

- [ ] **Step 5: 构建 + 提交 + PR**

```bash
./scripts/build.sh && ./builddir/sdcshield -e zstd19 -t 2000 -n 1
git add docs/architecture.md docs/research/adding-tests.md README.md docs/research/progress-log.md
git commit -s -m "docs(all-vl2): P1 文档同步 (architecture/adding-tests/README/progress-log)"
git push origin feat/arm64-vl-api
```
创建 PR（`feat/arm64-vl-api` → main）：标题 `feat(arm64): 框架层 VL/SVL API + selftest + dump-cpu-info (all-VL Phase 2 P1)`，正文含本机验证输出、gem5 四档表格（Task 8 实录）、spec/plan 链接、gem5-fi 分支交叉引用。PR 描述末尾：`🤖 Generated with [Claude Code](https://claude.com/claude-code)`

---

## Self-Review 记录

- **Spec 覆盖**：spec §6 P0 五项 → Task 1-5；P1 四项（API/selftest、dump-cpu-info、docs、gem5 验证）→ Task 6-9。§7 协议在 Task 8 落地（P1 是确定性 API 探针，无 golden/回归锚概念，checksum 的同档 fixed/random 一致性替代）。§5 三硬边界在 Task 6 实现（VL 不进 device_features、pin 限 init、selftest）。✓
- **占位符扫描**：无 TBD/TODO 型步骤；编码值、代码、命令、预期输出全部具体。✓
- **类型一致性**：`arm64_require_sve_vl(struct test *, long, long) -> long` 在 Task 6 定义、Task 9 文档引用一致；`.inst` 常量在 Task 6/8 一致（0x04bf5020/0x04bf5820）。✓
- **Review Focus 测试归属**：条目 1→Task 6 Step1+Task 8；2→Task 6 Step5；3→Task 6 Step5；4→Task 6 Step5+Task 8；5→全任务构建；6→Task 1/6 构建。✓
