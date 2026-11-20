# issue #655 解决方案文档：GEMM sizing 超预算 8.8%（RSS 表系数欠测）

> 状态：**无活缺陷（滞后重复审计）**。audit 于 2026-11-11 立案，证据快照是
> `e481496e`（2026-10-28 00:33）的验证记录；所指缺陷在**同日数小时内**已由
> `09c94fc6`（04:49 重标定）、`73b6918e`（10:02 T10 bad_alloc 防护）、
> `fd2f9bcd`（反馈撕裂窗口闭合）全部修复并均为现分支祖先。快道核实（issue
> 评论）已逐条对照确认；本文固化「已修复状态的现行证据 + 残余风险面评估 +
> 若需复核的验证方法」。行号基于本分支 `role/pr-issue-triage/issues-audit`
> （基底 727cc8d3）实测，复查方式见 §5。

## 0. 问题一句话

audit 的两条主张——(1) RSS 系数 `SDC_RSS_GEMM_F64=0.0290`（T4−T1 差分口径）
在 sizing 天花板处欠测每线程真实足迹，N=3364 全核实测峰值 43.31 GiB 超模型
39.8 GiB 达 +8.8%，吃掉 0.9×MemAvailable 安全储备大半；(2) worker 期
make_data/run 分配无 bad_alloc→SKIP 防护，运行期内存下降时 OOM-kill 裸奔
——**两条均已被同日后续 commit 修复**，现行代码无该缺陷。

## 1. 现状核实（audit 主张 → 现行代码对照）

| #655 主张 | 现行状态（本分支实测） | 修复 commit |
|---|---|---|
| 系数 0.0290 欠测 8.8% | **0.0290→0.0315 已重标定**：`framework/sdc_stress_config.h:1150`（表值）+ `:1143-1148`（证据链注释） | `09c94fc6`（2026-10-28 04:49） |
| preinit 引用点字面量随表重标定陈旧 | 引用点全走表函数：`tests/cpu/eigen_gemm/gemm_double_max_arm.cpp:338`（`sdc_rss_table_c(SDC_RSS_GEMM_F64)`）；`:348` 的字面量 `0.0315` 是 issue #489 哨兵路径（`c_kb<=0` 无表/越界时的 `+inf` UB 防御）有意退守，`:343-347` 注释明文其异常路径属性 | `1570de49` + `7ca9bb68` |
| worker 期无 bad_alloc→SKIP，OOM-kill 裸奔 | **T10 已落地**：`tests/cpu/eigen_gemm/gemm_double_max_arm_body.cpp:93-129` 分配/求解/reroll 三段窄 catch（仅 `std::bad_alloc`，catch 分别在 `:100/:110/:124`）→ `log_skip` + `EXIT_SKIP`（线程级资源性失败不打红）；比对段恒在 catch 外（`:115` 注释，SDC 误配必须打红） | `73b6918e`（2026-10-28 10:02） |
| 「仅 sizing 探针包了 try/catch，make_data/run 期分配仍在 catch 外」 | run 期大额分配（3 矩阵 + reroll 乘积 x）全部入 catch（上行）；make_data 仅 `new gemm_sve_data` 4B int 盒（body `:53-60`），T10 契约明文「本 TU 的 init 不持有矩阵, 无 init 降级点」（`:89-91`）——4B 分配失败即全进程内存墙，非本 issue 证据所及 | `73b6918e` |

**数学复算（本文独立代入，公式 = 表注释口径 `n² × c_KB × 全机线程 / 2²⁰`）**：

- 旧系数@N=3364：11,316,496 × 0.0290 KB × 127 ÷ 2²⁰ = **39.75 GiB**（精确落
  39.8 GiB 预算，audit 所述「模型精确落预算而实测 43.31 GiB」成立）；
- 新系数@N=3364：11,316,496 × 0.0315 KB × 127 ÷ 2²⁰ = **43.17 GiB**，vs 实测
  43.31 GiB 偏差 **+0.33%**（issue 评论算得 43.09 GiB，同 <0.5% 量级；无论
  哪个口径，原 +8.8% 系统性超模型已消除）；
- 新 sizing：预算 39.75 GiB 反解 n = √(41,679,360 ÷ (127 × 0.0315)) ≈
  **3228**——与 `09c94fc6` 验证记录「n_mem 3364→3228，预测=预算≈39.75 GiB」
  一致，0.9×MemAvailable 安全储备恢复。

## 2. 根因（现行代码注释即根因记录，引文件:行号）

- **系数欠测**：差分标定口径 `(child@T4 − child@T1)/3`（`sdc_stress_config.h:1124-1125`）
  只测 n² 斜率增量，**漏计 run 期第 4 个驻留矩阵（reroll 乘积 x）**及非 n²
  缩放驻留（Eigen packing buffer、探针高水位、分配器 slack，`:1126-1132`）。
  解析主体应为 4 矩阵 × 8B = **0.03125 KB/n²/线程**（`:1144-1145`），旧值
  0.0290 恰漏一个矩阵位（3×8B=0.024 + 打包残留 ≈ 0.029），天花板处欠测
  +8.8%（`:1145-1146`：N=3364 实测 43.31 GiB vs 模型 39.8 GiB）。
- **防护缺位**：`e481496e` 窗口只给 sizing 探针包了 catch（当时的
  `try { sdc_joint_dim_slice(...) } catch(...)` 退守 MDIM_MIN），run 期
  `l_lhs/l_rhs/l_prod/x` 分配裸露——Spec P2 已自认「SVD/GEMM 模板 test_run
  无 bad_alloc catch → malloc SIGKILL/abort」。

两层根因均在同日修复：系数换 0.0315（含探针高水位口径，T6 实测隐含 0.0316、
T7 反馈 eff 推 `c_fb≈32.8B`，`:1146-1147`）；run 期三段窄 catch + 比对段豁免。
另两条结构性防线把残差继续压住：VmHWM 反馈闭环（`:1163` 起，`fd2f9bcd` 修
写侧扣 fork 基线/读侧 4× 锚上钳，表值只是初始锚、跨机 ±5% 级残差由反馈吸收，
`:1132-1135`）与 preinit n_hi 上界精算（`max_arm.cpp:338-350`，同表同预算）。

## 3. 建议修复路径

**无需新修复**——不为修而修。两条主张的修复已落地且证据链完整。可选动作只有
两类，均非必需：

1. **残余面监视**（建议随下次触碰该族时顺带做，不单开 PR）：
   - `:348` 哨兵退守字面量会在下次表重标定时陈旧——`7ca9bb68`（#489）已
     裁定该路径有意保留（仅无表/越界异常路径受影响，健康路径逐值不变），
     注释在案，**不改**；
   - Eigen 版本升级需重标定整表（`:1135-1136` 注释明文）——升级 PR 的
     checklist 项。
2. **issue 收口动作**：按滞后重复审计归档——close 时引用本文 §1 对照表 +
  §2 数学复算即可，不需要任何代码/文档 diff。

## 4. 验证边界

- **本 mission 主机（no-SVE Kunpeng 920，CPU part 0xd01，96 核）**：
  `eigen_gemm_double_max_arm` 带 `ARM64_REQUIRES_SVE` 门控，`-e` 显式注册
  也只到 honest-SKIP——**本机无法原生复核 sizing 数值面**（43.31 GiB 级
  实测需 SVE 主机全核跑 + 峰值子进程 RSS 采样，参照机为 127C/61G 单节点）。
- **可离地完成的验证（本文已做）**：数学复算（§1，纯代入表值与公式，
  n_mem 3228 反解与 `09c94fc6` 验证记录吻合）+ 现行代码锚点逐条核实（§1 表）。
- **若必须在真机复核**（超出本 issue 需要）：在 SVE 主机跑
  `-e eigen_gemm_double_max_arm -t 3000` 全核，断言 `[联合求解]` n_mem
  ≤ 表模型反解值、全跑峰值 RSS 不超 650‰ 预算；反馈口径断言 `[反馈] eff`
  落在 1.3% 级残差（`09c94fc6` 记录 +1.3%）。**gem5-fi 在此不适用**：
  SE/FS 仿真不产生真实进程 RSS，RSS 系数的标定与验证只认真机差分/VmHWM
  口径——仿真腿只能验证 T10 catch 的功能行为（OOM 注入），不能验证 GiB 数。
- **T10 防护的行为验证**（若单独验证）：真机或 FS 腿注入内存压力，断言
  `log_skip(memory: ...)` + 线程级 SKIP 而非 SIGKILL；比对段豁免契约
  （SDC 误配仍打红）不动。

## 5. 引用复查清单

```console
sed -n '1123,1151p' framework/sdc_stress_config.h   # 口径注释 + :1143-1148 重标定证据链 + :1150 表值
sed -n '338,350p'  tests/cpu/eigen_gemm/gemm_double_max_arm.cpp   # 表函数引用 + #489 哨兵退守
sed -n '93,129p'   tests/cpu/eigen_gemm/gemm_double_max_arm_body.cpp  # T10 三段窄 catch
sed -n '53,60p'    tests/cpu/eigen_gemm/gemm_double_max_arm_body.cpp  # make_data 4B 盒
git log --oneline -1 09c94fc6; git log --oneline -1 73b6918e
git log --oneline -1 1570de49; git log --oneline -1 7ca9bb68; git log --oneline -1 fd2f9bcd
gh api repos/wangxumarshall/sdcshield/issues/655      # 原文 + 快道核实评论（数学复核段）
```
