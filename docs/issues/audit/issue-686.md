# issue #686 解决方案文档：验证矩阵 128 档缺失（gem5-fi 档位覆盖缺口）

> 状态：**文档层 moot + 实质验证缺口残留**。audit 于 2026-11-13 立案属实；快道
> 核实（issue 评论）确认引文 100% 准确，但锚点 plan 已被 `31f6a99e`（2026-11-17
> 冗余清理）删除，文档缺陷无存活修复面；**实质残留**——`eigen_gemm_double_max_arm`
> （及 `eigen_svd_cdouble_sve`）的 sizing 探针在 **128 档**至今无任何真实运行
> 证据。本文给出闭合该缺口的验证方案。所有行号基于本分支
> `role/pr-issue-triage/issues-audit`（基底 727cc8d3）实测，复查方式见 §5。

## 0. 问题一句话

plan 验证矩阵自认「本任务触及四档 init 路径」（128/256/512/1024），验证却只覆盖
三档——256 本机原生 + 512/1024 gem5 `--sve_vl 4/8`——**128 档的 init/solve_once
分派与 sizing 探针路径从未被任何真实运行触及**，CLAUDE.md rule 2 的 gem5-fi
档位覆盖要求存在缺口。

## 1. 现状核实

### 1.1 历史引文逐字属实（git 历史 `31f6a99e^` 取回，audit 行号系早期修订版）

| audit 原引 | `31f6a99e^` 终版实测 | 原文 |
|---|---|---|
| plan:601 | `docs/superpowers/plans/2026-10-27-eigen-p0-p3-fix.md:620`（gemm 任务验证矩阵） | 「gem5-fi `--sve_vl 4/8` 两档 pass（**本任务触及四档 init 路径**）」 |
| plan:593 | 同文件 `:611-612`（svd Task 5 Step 4） | 「以 `--sve_vl 4/8`（512/1024）……（256 档本机已原生覆盖）」 |

执行记录同样属实：`e481496e`（2026-10-28，gemm sizing 迁 child）实跑
`--sve_vl 2/4/8` 三档（比 plan 多跑了 256，仍无 128）；`bb0e148a`（2026-10-27，
svd_cdouble_sve 迁 child）只 `--sve_vl 4`（checksum 50ff99225080c21c）与
`--sve_vl 8`（诚实 SKIP）。两 commit 均无 `--sve_vl 1` 或本机降档 128 的输出。

### 1.2 代码锚点（现行形态）

- tier 分派已从手写 switch 收口进生成宏：`tests/cpu/eigen_svd/sandstone_eigen_sve_dispatch.h:79`（`pfx##_tier_select` 的 `case 128:`）与 `:96`（`case 128: return pfx##_vl128::solve_once(n);`）——audit 所引旧形态
  「`gemm_double_max_arm.cpp:210` `case 128: return gemm_vl128::solve_once(n)`」
  已由 `EIGEN_SVE_DEFINE_DISPATCHERS` 取代（`gemm_double_max_arm.cpp:445-447`
  注释自认「the tier whitelist this TU used to spell out by hand」）。
  **128 档在白名单内、可编译、可分派——缺口不在可达性，在证据。**
- sizing 探针：`tests/cpu/eigen_gemm/gemm_double_max_arm.cpp:488`
  `dim = sdc_joint_dim_slice(test, "mdim", &ja, gemm_solve_once)`；探针
  「executes that tier's fixed-width packets on THIS thread」（`:443-444`），
  即 init 的联合求解探针跑的正是当前 pin tier 的 packet 代码——128 档跑的是
  `_vl128` 编译体的 `solve_once`，与 256/512/1024 档是**四份不同指令流**。
  `eigen_svd_cdouble_sve` 同款（`bb0e148a` 迁移，sizing_probe 经
  `sandstone_eigen_common.h:217`）。

### 1.3 moot 性与残留（issue 评论核实 + 本分支复核）

- **文档层 moot**：锚点 plan 随 `31f6a99e`（2026-11-17）整目录清理出库
  （CLAUDE.md 文档地图记载恢复点 `31f6a99e^`）；当前 main 存活面 grep
  「触及四档」「sve_vl 4/8」为零。文档缺陷无修复面，**不复活历史工件打补丁**。
- **实质残留成立**：后续工作未闭合 128 档探针——issue #410（2026-11-01）gem5
  四档验证的是 ptranspose scatter payload；2026-11-13 sve2 companion 的 FS 腿
  是 `eigen_svd_double_sve2` VL=128（dispatcher 路径，非本 issue 两个用例）。
  两者都不是 `eigen_gemm_double_max_arm` / `eigen_svd_cdouble_sve` 自身 sizing
  探针在 128 档的运行。

## 2. 根因

历史 plan（已删，`:611-620`）在写验证矩阵时把「本机 SVE max=256」默认为档位
下界：**128 档被隐含排除在「触及四档」的验证义务之外**——矩阵列了 512/1024
（gem5 补）而把 256 留给本机原生，唯独没有回答「128 档怎么验」。rule 2 的原文
要求「在受影响的 VL/SVL tiers 引用真实运行输出」，四档 init 路径全部在受影响
范围内（`:96` 四份 per-tier 编译体，128 档有独立的 `_vl128::solve_once` 指令流），
plan 只验 2+1 档即自认完成——**档位覆盖矩阵与触及面声明不一致**，是计划文档的
验收判据自我放松，非代码缺陷。

## 3. 建议修复路径

文档已 moot，唯一有意义的动作是**补 128 档验证并归档**（issue 评论建议另立
验证任务，非文档补丁）：

1. **立验证任务**（task-loop 标准道或会话手动），目标两用例：
   `eigen_gemm_double_max_arm`、`eigen_svd_cdouble_sve`。
2. **优先本机降档**（若执行机是 SVE 主机）：
   `SDCSHIELD_ARM64_SPEC_MAX_VL=128 ./builddir/sdcshield -e eigen_gemm_double_max_arm -t 5000`
   ——该 env 只降不升、框架 in-process pin（CLAUDE.md 记载 `PR_SVE_SET_VL`
   不经 `execve` 存活，外部 pin-then-exec 无效，该 env 即为此设计），
   期望：`[联合求解]` 日志可见、`exit: pass`、readback VL=128。
3. **gem5-fi 路径**（SVE 主机不可得时；本 mission 主机即属此类，见 §4）：
   `/home/sdc/wangxu/gem5-fi`（**注意**：plan:20 写的 `operator/gem5-fi` 是
   已失效旧路径，spec §8 勘误同认，本机真实路径以实测为准）CHAOS/SE 同源
   双内核 `workloads/sdcshield/`，`--sve_vl 1`（=128 bit）跑两用例，
   期望：`exit: pass`、tier readback 128、checksum、`SDCSHIELD_FAILS=0`。
4. **结果归档**：真实输出贴回 issue #686 评论收口该单；归档行落验证任务的
   新计划文档，照常落 `docs/superpowers/plans/`（新建即可，不复活已删
   plan——`docs/research/` 整目录已随 `31f6a99e`（2026-11-17）清出本分支
   基底，本文初稿误引的 progress-log 通道在该时点不存在）。若 128 档出现
   异常（fail/错误 sizing），则升级为代码缺陷另立 fix-PR——那正是这条
   从未验过的腿该抓的东西。

## 4. 验证边界

- **本 mission 主机（no-SVE Kunpeng 920，CPU part 0xd01，96 核，Features 无
  sve）**：`-e` 显式注册也只到 honest-SKIP（`ARM64_REQUIRES_SVE` 门控），
  **任何档位都不可原生验证**——方案 2 的「SVE 主机降档」在本机不可行，方案 3
  gem5-fi 是本机唯一可行路径（`/home/sdc/wangxu/gem5-fi` 的 `gem5.opt` 可用，
  2026-10-26 实测勘误）。
- **gem5 边界如实声明**：SE 模式验证的是功能正确性 + sizing 探针路径
  （init 联合求解 + per-tier packet 分派），`-t` 时长语义与 native 不同
  （仿真速），预期产物是 checksum/`SDCSHIELD_FAILS=0`/tier readback 而非
  时长数字；本机降档（若换 SVE 主机执行）则同时具备时长口径。
- 验收断言：两条用例各有一条 128 档真实运行输出（tier/checksum/FAILS=0），
  `[联合求解]` 探针日志在 128 档出现——即 `sandstone_eigen_sve_dispatch.h:96`
  `gemm_vl128::solve_once` 路径首次获得实测证据。

## 5. 引用复查清单

```console
git show 31f6a99e^:docs/superpowers/plans/2026-10-27-eigen-p0-p3-fix.md | sed -n '605,622p'
sed -n '79,96p'  tests/cpu/eigen_svd/sandstone_eigen_sve_dispatch.h
sed -n '443,495p' tests/cpu/eigen_gemm/gemm_double_max_arm.cpp   # tier contract + :488 探针
sed -n '216,217p' tests/cpu/eigen_svd/sandstone_eigen_common.h   # sizing_probe 入联合层
git log --oneline -1 e481496e; git log --oneline -1 bb0e148a
gh api repos/wangxumarshall/sdcshield/issues/686                 # 原文 + 快道核实评论
```
