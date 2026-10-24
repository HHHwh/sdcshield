# SVE/SME 全 VL 二期 P6（存量迁移）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 141 个仍手写 `getauxval` 探测的测试文件批量迁移到框架层 `arm64_require_sve_vl/sme_svl`，全仓探测样板清零（spec §12.4 验收：grep 审计口径），同时处理 P1 遗留的单位约定重估。

**Architecture:** 按目录分批（spec §6 既定裁定：每批 build+回归，一目录批一 commit，非一文件一 commit——141 文件的逐文件粒度不具可操作性），P3/P4 批量代理模式复用；三批并行（worktree 隔离）。

**Spec:** `docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md` §6 P6 / §12.4 / §5（API 契约）。

## 迁移面（2026-10-24 实测 grep）

141 文件含 getauxval：sve/case 51、sve/mesh 17、sve/arithmetic 16、arm64 12、sve/kreg 11、sve/fma 10、sve/mem 9、sve/fma2 8、sve/vector2 5、sve/eigen 1、sleef 1（已迁 P5？——grep 时点差异，实施时以实时 grep 为准）。另 7 个 EVP-only ipsec 文件无 SVE 探测（不在面内）。

## 变换规范（canonical，源自 P3-P5 已验证形态）

对每个含 `getauxval(AT_HWCAP)` SVE 门控的文件：
1. include 块：`<sys/auxv.h>`/`<asm/hwcap.h>` → `<device/cpu/arm64/arm64_vl.h>`（保留 arm_sve.h；**若文件还探测其他 HWCAP 位如 SVE2/BTI 则保留原 include**）
2. 门控：`getauxval(AT_HWCAP) & HWCAP_SVE` 块 + 其后的 VL 读取/检查 → `const long svl = arm64_require_sve_vl(test, 128, 1024); if (svl < 0) return EXIT_SKIP;` + 既有 lanes 换算改 `svl/8|svl/4`（**保留原文件已有的范围检查语义——若原文件无范围检查（纯 A 类），补 [2,16]/[4,32] 检查**）
3. run 侧漂移守卫：A 类文件多无守卫——补齐（svcntd/svcntw 对 init 记录值；128 编译 TU 用 arm64_sve_vl_bytes() 防 GCC 折叠——P5 实证教训）
4. SKIP 文案保持原义（能力缺失类），仅探测机制换框架层

**不做的**（明确出界）：不改几何/不改 golden/不改压力语义——P6 是纯探测层迁移；凡发现文件有 VL 相关缺陷（越界/单位错）——**单独 commit + 报告**，不混入机械批。

## 任务分批

- [ ] **Task 1: 批 A（sve/case 51 + sve/kreg 11 + sve/vector2 5 + sve/fma 10）**——worktree 隔离代理；按子目录 4 个 commit；每 commit 后构建+抽样 skip+zstd19
- [ ] **Task 2: 批 B（sve/mesh 17 + sve/arithmetic 16 + sve/mem 9 + sve/fma2 8）**——同上 4 commit
- [ ] **Task 3: 批 C（arm64 12 + sve/eigen 1 + sleef 1）**——同上；arm64 目录含 SME 门控文件（sme_* 已在 P2 迁移？——实时核对：已迁的跳过，遗留的用 require_sme_svl）；**含 P3 评审点名的 6 个遗留手探文件**（stencil_axis/nt_reload/fcmla/pred_ops/fpcr_cartesian/power_virus_dit_sve）
- [ ] **Task 4: 单位约定重估（P1 遗留 minor 的兑现）**：`arm64_vl.h` 的 require(bits)/pin(bytes) 不对称——评估改 require 返回 bits 或参数改 bytes；**裁定权在证据**：迁移 141 文件后的实际调用形态统计（多数要 lanes=svl/4 即 bytes 除法——若 bytes 口径调用面更广则保持，仅强化注释；反之改签名）。一个 commit（含头文件+selftest/已迁文件的调用点同步）。
- [ ] **Task 5: 验收审计 + 文档**：grep 审计（`grep -rn 'getauxval.*HWCAP_SVE\b' tests/` 应只剩 arm64_vl.h 自身与保留多位的文件——逐个列出并核理由）；progress-log P6 条目；PR 正文（分批清单+审计输出）。

## Review Focus

1. **机械批的语义漂移**：迁移改变 skip 分类/文案/时序（require 在能力缺失时 log_skip 的类别是否与原手探一致）。
2. **单位换算错**（P2 Critical-1 永久军规）：svl/8 vs /4 每文件核对（批代理 grep 审计 + 抽样细读）。
3. **多 HWCAP 位文件的 include 误删**：探测 SVE2/BTI 等的文件保留 sys/auxv。
5. **GCC 折叠陷阱**（P5 实证）：128 编译 TU 的漂移守卫必须 rdvl 型。
6. **范围检查补齐的正确性**：A 类文件原无范围检查——补的 [2,16]/[4,32] 与其几何匹配（f32 文件 [4,32]）。

## Self-Review

- Spec §6 P6 全覆盖（分批+审计+单位重估）；§12.4 验收口径落实 Task 5。无占位符（批清单以实时 grep 为准是环境事实）。风险：141 文件批代理的长任务——分 3 代理并行 + 每批内子目录 commit 降低爆炸半径。
