# SVE/SME 全 VL 二期 P5（sleef/eigen×8）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 spike 证据分流：**sleef_sve + sve_rot×5 真迁移**（TU/库双 VLA 就绪、三态探针零告警）到真 VLA 库；**eigen×2（gemm/svd）自 pin 回退**（Eigen packet 定长、VLA 语言级不可编译——两次 spike 独立确证）。

**Architecture:** gem5-fi sleef dual kernel（链 vendored SLEEF 静态库，same-kernel golden + 跨档逐位一致 + 128 回归锚）四档 16-run 先行 → sdcshield 迁移（meson 库移动 + 注释重写）+ eigen×2 自 pin 接入 → 文档收尾。

**Spec:** `docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md` §6 P5 / §11.1（(a)/(c) 分流——(a) 对 sleef 成立、(c) 对 Eigen 成立，均有 spike 证据）。

**Spike 证据基线**（2026-10-24 双 spike，报告全文见执行会话账本与 gem5-fi 侧记录）：
- sleef：TU 零 128-ism 构造（sizeless + svcnt 步进 + whilelt 尾）；vendored SLEEF 3.9.0 纯 VLA（反汇编实证 cntd/cntp/rdvl、无 ptrue_pat）；三态编译（128/256/无 flag）全零告警；历史 256 回归最可能系实验污染（H1：整体翻 flag 连 Eigen 定长 TU 重编的堆损坏被归因 sleef）。
- Eigen：`arm_sve_vector_bits(EIGEN_ARM64_SVE_VL)` 定长 + PacketXcd struct 成员/NumTraits sizeof 语言级死结；七类编译期消费点；自 pin 回退有完整基础设施（arm64_sve_pin_vl + require）。

## Global Constraints

- 分支：`feat/sleef-eigen-allvl` 堆叠 `feat/ipsec-allvl`；gem5-fi `feat/sleef-dual` 堆叠 `feat/sha-dual`。
- gem5 sleef kernel 依赖 vendored SLEEF 静态库（主 checkout `third-party/sleef/install/lib/libsleef.a`，gitignored——kernel Makefile 用绝对路径引用 + 注释声明依赖，不把二进制入仓）。
- 单位军规 / 验证 subagent 隔离 / 一文件一 commit / DCO / x86 零改动，同前各阶段。

## Review Focus

1. **sleef 跨档逐位一致**：sin/cos/exp/log 逐 lane 纯函数 ⇒ 四档 golden 缓冲逐字节一致——kernel 判据核心（若不一致即历史回归真身，单变量定位）。
2. **128 回归锚**：迁移后 128 档 checksum 与迁移前（128 编译产物）一致。
3. **Eigen 自 pin 语义**：init 里 pin(16) + 返回值核对 + run 侧 fail-closed；pin 失败（内核限制）诚实 SKIP 文案。
4. **meson 库移动的依赖链**：sleef_dep 跟随新库；tests_set_sve 剩余成员（eigen×2）不破坏。
5. **sve_rot×5 同批**：探针已证 VLA 三态全净——迁移零风险项。

---

### Task 1: gem5-fi `sleef_vla_dual.c` + 16 run

- [ ] kernel：链 `libsleef.a`；四函数组（Sleef_sindx_u10sve/cosdx/expdx/logdx + u35 二档）；输入 nelems=1024（fixed 确定性/random xs32）；**same-kernel golden**（init 期同函数算）+ 逐字节比对 + FNV；**跨档一致性检查**：golden 缓冲在四档间逐字节相等（判据 1——kernel 内直接断言并将结果入 FAILS）；输出 `mode=sleef_vla lanes=N cross-tier-match=yes/no`。Makefile 目标注明 libsleef.a 绝对路径依赖。
- [ ] 16 run（四档 × {fixed, random×3}）隔离代理；判据：FAILS=0、四档 golden 逐位一致、128 档为回归锚基准。

### Task 2: sdcshield sleef_sve + sve_rot×5 迁移（每文件一 commit）

- [ ] `tests/cpu/meson.build`：`sleef/sleef_sve.cpp` 移出 tests_set_sve → 新真 VLA 静态库（`-march=armv8.2-a+sve` + sleef_dep + Eigen 宏**不带**）；sve_rot×5 同批移入；tests_set_sve 仅剩 eigen×2（注释更新）。
- [ ] sleef_sve.cpp 注释重写（128-lock 段落 → VLA 语义 + 四档实证引用）；门控换 `arm64_require_sve_vl`（现行 HWCAP 手探 → 框架层）+ run 侧漂移守卫。
- [ ] sve_rot×5：同 P3 五段式（其 docblock 的 P5 迁移预排注释兑现）。
- [ ] 每文件构建零告警 + skip + zstd19。

### Task 3: eigen×2 自 pin 接入（每文件一 commit）

- [ ] `gemm_double_max_arm.cpp` / `svd_cdouble_sve.cpp`：init 里 `arm64_require_sve_vl(test, 128, 1024)` 后 `arm64_sve_pin_vl(16)`（编译档 128）+ 返回值核对（≠16 → 诚实 SKIP "cannot pin to the compile-time VL (kernel restriction)"）+ docblock 记 spike 裁定与语言级死结引用；run 侧 fail-closed（`svcntb() != 16 → fail`）。**消灭对已丢失的外部 /tmp/vlrun launcher 的依赖**。
- [ ] 每文件构建零告警 + skip + zstd19。

### Task 4: 文档收尾 + PR

- [ ] meson/README/progress-log（P5 条目：双 spike 结论、分流裁定、16-run 实录、Eigen 死结档案引用）；PR 正文（含 spec §11.1 三选一的最终归属表）。

## Self-Review

- Spec §6 P5 全覆盖（sleef 迁移/回退分流——按 spike 证据而非原设想的统一路径；原"真 VLA 重构"对 sleef 成立对 Eigen 不可达，spec §11.1 预设的撞墙回退路径被行使）。Review Focus 归属：1/2→Task 1；3→Task 3；4→Task 2；5→Task 2。无占位符；kernel 的 libsleef.a 依赖是环境事实非 TBD。
