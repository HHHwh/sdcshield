# ci-multi-os-fix: 修复 Multi-OS Verify（run 44456953718）的两类 CI 红

## Intent

Multi-OS Verify（workflow_dispatch，最新 main 3633cd85）全线红，分两类，都是
PR #814「feat/migrate-opendcdiag-tests」合并进 main 后引入的回归：① 20.03-LTS
全族（5 个 job）在编译期挂——`tests/cpu/misc/adaptive_mem_sdc.cpp:828` 用了
`HWCAP2_SVE2` 却没 `#ifndef` guard，20.03 的 4.19-era 头文件没定义它；②
22.03/24.03 全族（10 个 job）在「Verify (full parameter sweep)」挂——全量扫描
`crash=2`（2 个测试崩溃，0 data-fail，0 soft-fail），疑似 814 新增用例族。本 mission
修这两类，让 Multi-OS Verify 复绿。

## Done when

- 20.03 build 通过：adaptive_mem_sdc.cpp 的 `HWCAP2_SVE2` 加 `#ifndef` guard，且 814 新增代码里无其它未 guard 的 20.03-incompatible `HWCAP2_*`/arch 宏
- 定位出 `crash=2` 的两个测试名 + 各自根因（引真实崩溃现场/复现）
- 两处 crash 已修复（或按 red line 诚实 skip 并说明不可在本机/CI 运行的理由）
- 本机构建 + 受影响用例 + 回归 `zstd19 -t 2000 -n 1` → `exit: pass`（rule 2 真实输出）

## Out of scope

- 不碰 x86-64 参考路径（rule 5）；不改 CI 工作流本身（只改测试/源码让现有 workflow 能绿）
- 不 merge 任何 PR、不 resolve review thread、不 push main（改动落到本 mission 分支 → 一个 MR）
- 若 crash 根因是 pre-existing（非 814 引入），记录并把边界如实写进报告，不在本 mission 强行扩scope

## Decisions for you

1. **crash 定位范围未定**：CI 的 grep 把崩溃现场过滤掉了（`crash=2` 只剩判定行，无测试名）。
   两个 builder 会在本地（本机 no-SVE 鲲鹏 920 aarch64，可跑非 SVE 用例）复现 + 必要时拉 CI
   artifact 取证。**Recommended: 先按 814 新增用例族（L3_cache_920 / arm-0102 / misc
   adaptive 三处）+ 本地全量扫描复现**，找不到再退回 CI 侧重跑；若 crashed 测试真在本机
   不可运行（SVE/SME 门控），按 gem5-fi 规则或诚实声明边界。approve 即采纳。
2. **模型**：2 个 builder + 1 个 reviewer，沿用 bypass-permissions 默认。

## How the work is split

两类失败解耦在文件上：20.03 编译错只涉及 `adaptive_mem_sdc.cpp` 一个文件（+可能同族的
未 guard 宏，扫描发现再扩），crash 定位涉及 814 新增的测试目录 `L3_cache_920/` 与
`arm-0102/`。两 role 文件不重叠，且并行（crash-hunt 不依赖 hwcap-guard——22.03/24.03
本身编译通过）。若 crash-hunt 发现崩溃在它 touches 之外的文件，按流程报 blocked 触发
计划更新。

```yaml
troupe: 1
goal: "修掉 Multi-OS Verify run 44456953718 的两类红：20.03 的 HWCAP2_SVE2 编译错 + 22.03/24.03 的 2 个崩溃测试"
why: "814 合并进 main 后 Multi-OS Verify 全红（编译 5 job + verify 10 job），需复绿"
workflow: troupe-mission
base: mission/ci-multi-os-fix
profiles:
  claude-build:
    agent: claude
    model: sonnet
    effort: high
    flags: [--dangerously-skip-permissions]
    text: workflow:builder
roles:
  hwcap-guard:
    use: claude-build
    goal: "修 20.03 编译错：adaptive_mem_sdc.cpp 的 HWCAP2_SVE2 加 #ifndef guard，并扫除 814 新增代码的同类未 guard 宏"
    touches: [tests/cpu/misc/adaptive_mem_sdc.cpp]
  crash-hunt:
    use: claude-build
    goal: "定位并修 22.03/24.03 的 crash=2：两个崩溃测试名 + 根因 + 修复（或诚实 skip）"
    touches: [tests/cpu/L3_cache_920/, tests/cpu/arm-0102/]
  review:
    agent: claude
    model: opus
    effort: high
    flags: [--dangerously-skip-permissions]
    goal: "Review each branch as it lands"
    touches: []
    reviews: [hwcap-guard, crash-hunt]
    text: workflow:reviewer
```

## Role: hwcap-guard

1. 读 `tests/cpu/misc/adaptive_mem_sdc.cpp`（`origin/main` 上 814 新增文件，当前 mission 分支已有），
   定位 `:828` 的 `if (hwc2 & HWCAP2_SVE2)`：在 `#include <asm/hwcap.h>`（:62）附近加
   `#ifndef HWCAP2_SVE2` 的 fallback（参照仓库既有先例 `framework/arm64_vl.h`、
   `tests/cpu/arm64/sme_fmopa_stress_arm.cpp` 的 guard 写法，定义为 0 或直接 guard 使用点），
   保证 20.03（4.19-era headers，无 `HWCAP2_SVE2`）能编译、22.03+ 行为不变。
2. 系统性扫描 814 带来的其它新增文件（至少 `tests/cpu/L3_cache_920/`、`tests/cpu/misc/`、
   `tests/cpu/arm-0102/`）里是否有未 guard 的 `HWCAP2_*`（SVE2/SME/MTE/SVEAES/SVEBITPERM…）
   或其它 20.03 头文件缺的宏——**只读扫描**；发现的每一处若不在你 touches 内，写 `status:
   blocked` 报告文件与宏名，等计划扩 scope（不自己越界改）。
3. rule 2 自证（100% 真实）：本机构建（`-Dcpp_std=gnu++20`，GCC 10 无 gnu++23）干净零告警；
   `adaptive_mem_sdc` 用例跑真实输出；回归 `zstd19 -t 2000 -n 1` → `exit: pass`；x86 靠检视。
   诚实边界：本机无 20.03 容器，无法真机复验 20.03 头文件——guard 正确性以「宏在旧头未定义
   则 fallback=0，语义 = 无 SVE2」的代码逻辑保证，如实写明。

## Role: crash-hunt

目标：把 CI 的 `crash=2` 落到具体测试名 + 根因 + 修复。

1. 先取证崩溃现场（CI grep 已过滤）：(a) 拉 run 44456953718 的 `verify-*.tar` artifact（
   `gh run download 44456953718`）找 per-test 输出/崩溃栈；(b) 本机构建后跑 814 新增用例族
   （`tests/cpu/L3_cache_920/`、`arm-0102/`、`misc` 的 adaptive/fp64 件）逐个找崩溃；
   (c) 若 CI 主机是 SVE1 而本机 no-SVE，无法本地复现的 SVE 崩溃走 gem5-fi 或诚实声明边界。
2. 定位每个崩溃的根因（读崩溃栈/框架 forkfd 输出/`report_fail` 路径），修复（`git commit -s`）。
   若根因在 `tests/cpu/misc/adaptive_mem_sdc.cpp`（hwcap-guard 的文件）或其它你 touches
   之外的文件，写 `status: blocked` 报告文件+行号，不越界。
3. rule 2 自证：构建干净；每个崩溃测试加真实通过输出；回归 `zstd19 -t 2000 -n 1` → pass；
   x86 靠检视。修不了就按 red line 诚实 skip（`log_skip` + "to be implemented (placeholder)"）
   并把理由写进报告，**绝不**用「SKIP 不变」当修复证据。

## Role: review

审 `hwcap-guard`、`crash-hunt` 两个分支。额外要查：guard 是否与 `arm64_vl.h` 先例等价（不是
自创魔法数字）、扫描是否真覆盖了 814 新增文件、崩溃修复是否引了真实崩溃现场（不是「应该好了」）、
skip 理由是否诚实、`zstd19` 回归输出是否真实。按 reviewer 规则写 `.troupe/reviews/<role>.md`
并给 PASS / FAIL 汇总。