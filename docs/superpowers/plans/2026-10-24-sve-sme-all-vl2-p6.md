# SVE/SME 全 VL 二期 P6（存量迁移）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 141 个仍手写 `getauxval` 探测的测试文件批量迁移到框架层 `arm64_require_sve_vl/sme_svl`，全仓探测样板清零（spec §12.4 验收：grep 审计口径），同时处理 P1 遗留的单位约定重估。

**Architecture:** commit 粒度：一目录（或同族）一 commit——CLAUDE.md 规则 1 的 unit 在机械同构迁移语境解释为一个迁移批，141 文件逐文件提交不具可操作性（执行先例：feat/p6-allvl 29+1 commit）；spec §6 的「按目录分批」裁定的是构建+回归节奏，one-patch-per-unit 按上述粒度诠释执行。P3/P4 批量代理模式复用；三批并行（worktree 隔离）。

**Spec:** `docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md` §6 P6 / §12.4 / §5（API 契约）。

## 迁移面（基线声明 + 数字口径，2026-10-24 实测）

**基线 = P1-P5 全部合入后**（实施分支 feat/p6-allvl 基于 P5 尖端构建；合并顺序 #277→#283→#284→#286→#280（任意时点）→#288）。基线上实测 **141 文件**命中 getauxval+HWCAP：sve/case 51、sve/mesh 17、sve/arithmetic 16、arm64 12、sve/kreg 11、sve/fma 10、sve/mem 9、sve/fma2 8、sve/vector2 5、sve/eigen 1、sleef 1（sleef_sve 已随 P5 迁移——grep 命中为注释提及，活调用分类时排除）。

**数字口径**：本 PR 基点（main）直测为 **169 文件**——多出的 28 = arm64 +10（main 22 vs 基线 12，P3 迁移的 sve512 家族）、ipsec +17（P4 交付物）、eigen_svd +1（svd_cdouble_sve，P5 交付物），均系 P3-P5 交付物尚未合入 main，**不在 P6 面内**（另 7 个 EVP-only ipsec 文件本就无 SVE 探测）。两数不矛盾：P6 面以基线树锁定，**实施以实时 grep 重锁面**——实际执行 **142 文件**（feat/p6-allvl @ a9c1f08c，PR #288 待 merge：mesh 实为 18、arm64 实为 13、sleef 不在面内）。

## 变换规范（canonical，源自 P3-P5 已验证形态）

对每个含 `getauxval(AT_HWCAP)` SVE 门控的文件：
1. include 块：`<sys/auxv.h>`/`<asm/hwcap.h>` → `<device/cpu/arm64/arm64_vl.h>`（保留 arm_sve.h；**若文件还探测其他 HWCAP 位如 SVE2/BTI 则保留原 include**）
2. 门控：`getauxval(AT_HWCAP) & HWCAP_SVE` 块 + 其后的 VL 读取/检查 → `const long svl = arm64_require_sve_vl(test, 128, 1024); if (svl < 0) return EXIT_SKIP;` + 既有 lanes 换算改 `svl/8|svl/4`（**保留原文件已有的范围检查语义——若原文件无范围检查（纯 A 类），补 [2,16]/[4,32] 检查**）
3. run 侧漂移守卫：A 类文件多无守卫——补齐（svcntd/svcntw 对 init 记录值；128 编译 TU 用 arm64_sve_vl_bytes() 防 GCC 折叠——P5 实证教训）
4. SKIP 语义：aarch64 门控的文案/类别统一为 arm64_vl.h canonical（框架签发，正是收敛目的）；非 aarch64 `#else` 分支、资源错误类、占位类保留本地处理，逐个列入 Task 5 例外清单
5. SME 契约（sme_* 文件）：`arm64_require_sme_svl`（HWCAP2_SME 能力 + 128..1024-bit SVL 一次签发）；lanes 换算 svl_bytes/4（f32）、/8（f64）；`smstart sm` → `smstart za` → … → `smstop za` → `smstop sm` 配对生命周期必须保留（P2 裁决形态）

**VL 上限口径**：上限 1024 = 战役四档 gem5 验证口径（spec §7/D1），与 P3-P5 已验证形态一致（实际执行 176 处 `arm64_require_sve_vl` + 3 处 `arm64_require_sme_svl` 调用统一 128..1024，feat/p6-allvl @ a9c1f08c 实测）；`framework/selftest.cpp` 的 128..2048 是 helper 自检语境（全架构域验收），非测试门控口径。A 类文件由无上限收敛为 128..1024 属设计内语义收敛——>1024 档未经本战役验证，诚实 SKIP（文案含实测值）优于未验证执行。

**不做的**（明确出界）：不改几何/不改 golden/不改压力语义——P6 是纯探测层迁移；凡发现文件有 VL 相关缺陷（越界/单位错）——**单独 commit + 报告**，不混入机械批。

## 任务分批

- [ ] **Task 1: 批 A（sve/case 51 + sve/kreg 11 + sve/vector2 5 + sve/fma 10）**——worktree 隔离代理；按子目录 4 个 commit；每 commit 后构建+抽样 skip+zstd19
- [ ] **Task 2: 批 B（sve/mesh 17 + sve/arithmetic 16 + sve/mem 9 + sve/fma2 8）**——同上 4 commit
- [ ] **Task 3: 批 C（arm64 12 + sve/eigen 1 + sleef 1）**——同上；arm64 目录含 SME 门控文件（sme_* 已在 P2 迁移？——实时核对：已迁的跳过，遗留的用 require_sme_svl）；**含 P3 评审点名的 6 个遗留手探文件**（stencil_axis/nt_reload/fcmla/pred_ops/fpcr_cartesian/power_virus_dit_sve）
- [ ] **Task 4: 单位约定重估（P1 遗留 minor 的兑现）**：`arm64_vl.h` 的 require(bits)/pin(bytes) 不对称——评估改 require 返回 bits 或参数改 bytes；**裁定权在证据**：迁移 141 文件后的实际调用形态统计（多数要 lanes=svl/4 即 bytes 除法——若 bytes 口径调用面更广则保持，仅强化注释；反之改签名）。一个 commit（含头文件+selftest/已迁文件的调用点同步）。**若裁定翻转签名**：该 commit 须含全部调用点逐处单位重核对（P2 Critical-1 军规：除数逐处注释 + release 生效范围检查）+ 全量构建 + 抽样回归；头文件契约文档同步。（实际执行=维持现行约定，ac5b1e6b：298 调用面统计，改签名收益边际故不改。）
- [ ] **Task 5: 验收审计 + 文档**：grep 审计——文件级两段式，两行式/同行式探测与 SME/SVE2 的 AT_HWCAP2 全覆盖：`grep -rlZ 'getauxval' tests/ | xargs -0 grep -lZ 'HWCAP_SVE\|HWCAP2' | xargs -0 -n1 -r grep -nH 'getauxval\|HWCAP_SVE\|HWCAP2'`（逐文件列命中行，人工分类活调用 vs 注释/docblock/meson 提及）；期望结果：**活调用仅剩设计保留清单**（如 fmmla/cross_precision 的 SVE2 HWCAP2 复合门，逐个列理由；arm64_vl.h 自身在 framework/ 不属 tests/ 审计范围）；另补迁移前后 log_skip 行 diff 审计——每处被移除的本地 log_skip 须由 arm64_require_* 采纳或列入例外清单；progress-log P6 条目；PR 正文（分批清单+审计输出）。（执行佐证 feat/p6-allvl @ a9c1f08c：字符串命中 14 文件，活调用仅 sve512_fmmla_arm.cpp:165 与 sve2_cross_precision_arm.cpp:208 两处设计保留。）

## Review Focus

1. **机械批的语义漂移**：迁移改变 skip 分类/文案/时序（require 在能力缺失时 log_skip 的类别是否与原手探一致）。
2. **单位换算错**（P2 Critical-1 永久军规）：svl/8 vs /4 每文件核对（批代理 grep 审计 + 抽样细读）。
3. **多 HWCAP 位文件的 include 误删**：探测 SVE2/BTI 等的文件保留 sys/auxv。
4. **GCC 折叠陷阱**（P5 实证）：128 编译 TU 的漂移守卫必须 rdvl 型。
5. **范围检查补齐的正确性**：A 类文件原无范围检查——补的 [2,16]/[4,32] 与其几何匹配（f32 文件 [4,32]）。

## Self-Review

- Spec §6 P6 全覆盖（分批+审计+单位重估）；§12.4 验收口径落实 Task 5。无占位符（批清单以实时 grep 为准是环境事实）。风险：141 文件批代理的长任务——分 3 代理并行 + 每批内子目录 commit 降低爆炸半径。
