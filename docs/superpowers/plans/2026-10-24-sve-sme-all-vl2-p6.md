# SVE/SME 全 VL 二期 P6（存量迁移）实施计划

> **Status（2026-10-24）：已实施并收官（本文档自此为计划存档 + 收官实测修订）**——实施分支 feat/p6-allvl（Task 4/5 收官 ac5b1e6b/a9c1f08c，终审修复轮 7e464454），交付 PR #288 待人工 merge（合并顺序见下）。Tasks 已按实际执行勾选并附 commit 佐证。下方「For agentic workers」逐任务执行指令仅存档保留，**勿据此重跑迁移**——任何重执行前必须先核对 PR #288 合并状态与 Task 5 全仓活调用审计结果。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 140 个仍手写 `getauxval` 探测的测试文件（基线字符串级命中 141，其中 sleef_sve 1 处为注释级提及、非活调用）批量迁移到框架层 `arm64_require_sve_vl/sme_svl`，**tests/ 范围内**「独立 SVE/VL 与 SME 探测样板」清零；**复合能力门除外**（SVE2/BTI/F32MM/SVEBF16 等 helper 不覆盖的测试语义合取谓词系设计保留，实测仅剩 sve512_fmmla_arm/sve2_cross_precision_arm 两处——见变换规范第 2 条分流门控与 Task 5 设计保留清单；后续审计不得将保留门判为清零失败，更不得为「清零」字面删除这些保护新指令的能力检查）。spec §12.4 验收口径：`tests/` 下 grep 审计、框架自身实现除外——`framework/` 下 arm64_vl.h/cpuid_internal.h/arm64_cpuid.h/selftest.cpp/simd-arm.conf 共 5 处 getauxval+HWCAP 命中系能力探测实现本身，不在清零面。同时处理 P1 遗留的单位约定重估。

**Architecture:** commit 粒度：一目录（或同族）一 commit——CLAUDE.md 规则 1 的 unit（each feature/bug/porting point）在**机械同构迁移**语境解释为一个迁移批，判定条件：同一变换规范逐字适用 + 零语义决策 + 每文件变换可被审计命令机检；三条件齐备才可合批，含语义决策的改动（Task 4 单位重估、先存缺陷修复）仍逐单元独立 commit。spec §6 的「按目录分批」裁定的是构建+回归节奏，one-patch-per-unit 按上述粒度诠释执行。该诠释的实际执行记录见 PR #288（29+1 commit——**作为执行记录引用，不作为先例**：该分支由本计划驱动，以其收官形态为本计划背书是循环论证）；若机械批粒度随 #288 获人工 merge 认可，CLAUDE.md 规则 1 应在 P6 收口同步补注该例外的判定条件（文档同步义务，防「机械同构」被泛化为任意打包的口子）。P3/P4 批量代理模式复用。批次调度（对账 spec §6 + 执行实录）：spec §6 字面要求「阶段内 work item 串行（验证 → commit → push）」，实际执行为**三批并行**——批 A（sve/case 51 + kreg 11 + vector2 5 + fma 10 = 77 文件）直落 feat/p6-allvl 主线，批 B（mesh/arithmetic/mem/fma2）、批 C（arm64/eigen/meson）各走独立分支 p6-batch-b/p6-batch-c（worktree 隔离），收官时 9a72140a→c5bc95b3 串行 merge 回主线（`git log --first-parent 680307c4..feat/p6-allvl` 可复核）。该并行系**计划自载的调度裁定**（137cb4a8 先于首批提交 19 分钟载明「三批并行」），**非 spec 修订授权的例外**，如实记录偏差边界：①三批文件集目录级互斥、零共享文件；②批内每个 work item 仍严格「构建+抽样 skip+zstd19 → commit」串行不跳步（spec §6 的 per-item 生命周期未破坏）；③集成时点串行 merge + 收官全量活调用审计（a9c1f08c）收口。偏差的最终追认点 = PR #288 人工 merge 门；人审不认可则后续阶段恢复 §6 字面串行。

**Spec:** `docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md` §6 P6 / §12.4 / §5（API 契约）。

## 迁移面（基线声明 + 数字口径，2026-10-24 实测）

**基线 = P1-P5 全部合入后**（实施分支 feat/p6-allvl 基于 P5 尖端 680307c4 构建；合并顺序 #277→#283→#284→#286→#280（任意时点）→#288）。基线上实测 **141 文件**字符串级命中 getauxval：sve/case 51、sve/mesh 17、sve/arithmetic 16、arm64 12、sve/kreg 11、sve/fma 10、sve/mem 9、sve/fma2 8、sve/vector2 5、sve/eigen 1、sleef 1——其中 **140 为活调用迁移面**（sleef_sve 已随 P5 迁移，命中为注释提及，活调用分类时排除）。

**数字口径（三种，勿混用）**：①**基线活调用面 = 140 文件**（上列组成去掉 sleef）；②**本 PR 基点（main = 505eeaff）字符串级直测 = 169 文件**（与①不同口径：①活调用级、②字符串级；等式按字符串级成立——**169 = 141（基线字符串级）+ 28（main 有而基线无）**）——28 = arm64 +10（sve512 家族 9 + libomp_ctx_arm，均 P3 已迁）、ipsec +17（P4 已迁）、eigen_svd +1（svd_cdouble_sve，P5 已迁），均系 P3-P5 迁移交付物尚未合入 main，**不在 P6 面内**（main ipsec 共 24 cpp、17 含 getauxval 手探，另 7 个 EVP-only 本就无 SVE 探测）；③**实际执行改动面（680307c4..a9c1f08c，tests/ 全文件）= 142** = 141 个 .cpp（140 门控迁移 + 1 个相邻清理 mesh_upi_sve_wide_asymm_read_L3_int.cpp）+ 1 个 meson.build（门控分流注释同步）；.cpp 按目录：case 51、mesh 18（17 迁移 + 1 相邻清理）、arithmetic 16、arm64 12、kreg 11、fma 10、mem 9、fma2 8、vector2 5、eigen 1；全文件口径 arm64 = 12 .cpp + meson = 13、sleef = 0。实施以实时 grep 重锁面：mesh 多触及 1 个相邻文件、sleef 不触及。

## 变换规范（canonical，源自 P3-P5 已验证形态）

对每个含 `getauxval(AT_HWCAP)` SVE 门控的文件：
1. include 块：`<sys/auxv.h>`/`<asm/hwcap.h>` → `<device/cpu/arm64/arm64_vl.h>`（保留 arm_sve.h；**若文件还探测其他 HWCAP 位如 SVE2/BTI 则保留原 include**）
2. 门控：**仅替换独立的 SVE/VL 子条件**——`getauxval(AT_HWCAP) & HWCAP_SVE` 块 + 其后的 VL 读取/检查 → `const long svl = arm64_require_sve_vl(test, 128, 1024); if (svl < 0) return EXIT_SKIP;`；**复合门**（同文件另有 SVE2/BTI/F32MM 等 HWCAP/HWCAP2 谓词，如 sve512_fmmla/sve2_cross_precision）**不得整条件替换**：保留这些谓词、其本地 skip 分支与非 AArch64 分支，只把纯 SVE 能力 + VL 范围段交给 helper（分流门控，实际执行形态 54b46920/ca4dde86）——事后审计不能替代本约束，SVE-only 主机不得因整条件替换绕过额外能力检查执行不支持的指令。+ 既有 lanes 换算改 `svl/8|svl/4`（**保留原文件已有的范围检查语义——若原文件无范围检查（纯 A 类），补 [2,16]/[4,32] 检查**）
3. run 侧漂移守卫：A 类文件多无守卫——补齐（svcntd/svcntw 对 init 记录值；128 编译 TU 用 arm64_sve_vl_bytes() 防 GCC 折叠——P5 实证教训）
4. SKIP 语义：aarch64 门控的文案/类别统一为 arm64_vl.h canonical（框架签发，正是收敛目的）；非 aarch64 `#else` 分支、资源错误类、占位类保留本地处理，逐个列入 Task 5 例外清单
5. SME 契约（sme_* 文件）：`arm64_require_sme_svl`（HWCAP2_SME 能力 + 128..1024-bit SVL 一次签发）；lanes 换算 svl_bytes/4（f32）、/8（f64）；`smstart sm` → `smstart za` → … → `smstop za` → `smstop sm` 配对生命周期必须保留（P2 裁决形态）

**VL 上限口径**：上限 1024 = 战役四档 gem5 验证口径（spec §7/D1），与 P3-P5 已验证形态一致（实际执行 176 处 `arm64_require_sve_vl` + 3 处 `arm64_require_sme_svl` 调用统一 128..1024，feat/p6-allvl @ a9c1f08c 实测）；`framework/selftest.cpp` 的 128..2048 是 helper 自检语境（全架构域验收），非测试门控口径。A 类文件由无上限收敛为 128..1024 属设计内语义收敛——>1024 档未经本战役验证，诚实 SKIP（文案含实测值）优于未验证执行。

**不做的**（明确出界）：不改几何/不改 golden/不改压力语义——P6 是纯探测层迁移；凡发现文件有 VL 相关缺陷（越界/单位错）——**单独 commit + 报告**，不混入机械批。

**验证口径（CLAUDE.md 规则 2 + spec §12.1/§12.3）**：本机（0xd01，无 SVE/SME）只能验证构建零告警 + SKIP 路径 + selftest——这是**本地边界，不是功能验证**；迁移含 lanes 换算 svl/8|svl/4、漂移守卫、范围检查补齐等**功能行为变更**，功能验收 = gem5-fi 四档（128/256/512/1024-bit）× 16 run 全绿（`SDCSHIELD_FAILS=0`、random checksum 互异，spec §12.1 首条）；zstd19 回归系非 SVE 测试，与迁移面无关联。**执行状态（诚实记录）**：P6 三批的 gem5 补证因 429 预算中断改主 agent 后台自跑（progress-log 2026-10-24 P6 条目），截至 2026-10-25 未见落盘记录——PR #288 人工 merge 门须按 spec §12.1 核验此项或由人工明示豁免边界；不得以「构建 + SKIP 干净」替代功能验证（CLAUDE.md PR #242 先例）。

## 任务分批

- [x] **Task 1: 批 A（sve/case 51 + sve/kreg 11 + sve/vector2 5 + sve/fma 10 = 77 文件）**——worktree 隔离代理；按子目录 4 个 commit；每 commit 后构建+抽样 skip+zstd19（已执行：c4eb1546/281c0934/76de9242/0427199f，批 A 净删 279 行）
- [x] **Task 2: 批 B（sve/mesh 17 + sve/arithmetic 16 + sve/mem 9 + sve/fma2 8 = 50 迁移 + 1 相邻清理）**——同上 4 commit（已执行：db48e384/7789db97/b62c721a/0b5e7530；mesh 实际触及 18 文件 = 17 门控迁移 + mesh_upi_sve_wide_asymm_read_L3_int 相邻清理）
- [x] **Task 3: 批 C（arm64 12 + sve/eigen 1 = 13 迁移 + meson 注释同步 = 全文件口径 14）**——同上（已执行：arm64 单文件 commit e9d7f175/f816919d/20404c89/dedf830e/a0f28d12/cc5d1476/d8e6f625 + 分流门控 54b46920/ca4dde86 + sme 三文件 f45e2df0/4cae4b60/faf3c980 + a39dd077 + meson dc4b0121；**sleef 不在面内**——基线计数中的 sleef 1 为注释级命中）；arm64 目录含 SME 门控文件（实时核对：遗留的用 require_sme_svl）；**含 P3 评审点名的 6 个遗留手探文件**（stencil_axis/nt_reload/fcmla/pred_ops/fpcr_cartesian/power_virus_dit_sve）
- [x] **Task 4: 单位约定重估（P1 遗留 minor 的兑现）**：`arm64_vl.h` 的 require(bits)/pin(bytes) 不对称——评估改 require 返回 bits 或参数改 bytes；**裁定权在证据**：迁移 140 文件后的实际调用形态统计（多数要 lanes=svl/4 即 bytes 除法——若 bytes 口径调用面更广则保持，仅强化注释；反之改签名）。一个 commit（含头文件+selftest/已迁文件的调用点同步）。**若裁定翻转签名**：该 commit 须含全部调用点逐处单位重核对（P2 Critical-1 军规：除数逐处注释 + release 生效范围检查）+ 全量构建 + 抽样回归；头文件契约文档同步。（实际执行=维持现行约定，ac5b1e6b 入档：可复现口径 tests/ 内 `arm64_require_sve_vl` 176 + `arm64_require_sme_svl` 3 处代码行调用、lanes 换算 svl/8|svl/4 90 行，另有 framework/selftest 的 pin/自检调用——改签名需全部调用点逐处重核对，收益边际故不改。）
- [x] **Task 5: 验收审计 + 文档**：grep 审计——两段式、**无文件级 AND 预过滤**（凡含 getauxval 的文件必入分类清单——防能力位经宏展开/数值位间接引用的探测文件在「须同时字面含 getauxval 与 HWCAP_SVE/HWCAP2」的预过滤下整体逃逸）：`grep -rlZ 'getauxval' tests/ | xargs -0 -n1 -r grep -nH 'getauxval\|HWCAP'`（逐文件列**所有** getauxval/HWCAP 命中行——覆盖两行式/同行式探测与 AT_HWCAP2/SME；人工分类活调用 vs 注释/docblock/meson 提及）；期望结果：**活调用仅剩设计保留清单**（如 fmmla/cross_precision 的 SVE2 HWCAP2 复合门，逐个列理由；arm64_vl.h 自身在 framework/ 不属 tests/ 审计范围）；另补迁移前后 log_skip 行 diff 审计——每处被移除的本地 log_skip 须由 arm64_require_* 采纳或列入例外清单；progress-log P6 条目；PR 正文（分批清单+审计输出）。（执行佐证 feat/p6-allvl @ a9c1f08c：字符串命中 14 文件，活调用仅 sve512_fmmla_arm.cpp:165 与 sve2_cross_precision_arm.cpp:208 两处设计保留。）

## Review Focus

1. **机械批的语义漂移**：迁移改变 skip 分类/文案/时序（require 在能力缺失时 log_skip 的类别是否与原手探一致）。
2. **单位换算错**（P2 Critical-1 永久军规）：svl/8 vs /4 每文件核对（批代理 grep 审计 + 抽样细读）。
3. **多 HWCAP 位文件的 include 误删**：探测 SVE2/BTI 等的文件保留 sys/auxv。
4. **GCC 折叠陷阱**（P5 实证）：128 编译 TU 的漂移守卫必须 rdvl 型。
5. **范围检查补齐的正确性**：A 类文件原无范围检查——补的 [2,16]/[4,32] 与其几何匹配（f32 文件 [4,32]）。

## Self-Review

- Spec §6 P6 全覆盖（分批+审计+单位重估）；§12.4 验收口径落实 Task 5；§12.1 gem5 功能验收与 CLAUDE.md 规则 2 落实「验证口径」段（P6 实际执行留有 gem5 补证缺口，PR #288 merge 门核验）。无占位符（批清单以实时 grep 为准是环境事实）。风险：140 文件批代理的长任务——分 3 代理并行 + 每批内子目录 commit 降低爆炸半径。
