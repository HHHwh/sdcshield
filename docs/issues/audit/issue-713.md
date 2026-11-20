# issue #713 解决方案文档：T11 显式 knob 警告机制半迁移态

> 状态：**活缺陷（可维护性）**，未修复。audit 于 2026-11-13 立案属实；快道处置
> 已核实并拆分 T11b 迁移路线（见 issue 评论），本文件将该路线固化为可执行方案。
> 本文只出方案，不含修复代码。所有行号基于本分支
> `role/pr-issue-triage/issues-audit`（基底 727cc8d3，2026-11-18）实测，复查方式见 §5。

## 0. 问题一句话

eigen 族用例的显式 `-O`/env 维度 knob 存在**两条分叉路径**：SVD 模板族 10 个用例
已迁入联合层 `sdc_joint_dim_slice` 的 T11 早退（打 `[内存越界]`/`[欠压力]`
预期内存警告），其余 gemm/sparse/SVE 伴生体等 **29 个源文件**仍在各自 init 里
`if (v > 0) return v;` 调用方短路（无任何警告）。同一优先级链
「显式 > quick > 自适应」两条实现并存，后续维护改一条漏一条。

## 1. 现状核实（全部锚点在本分支实测在位）

| audit 原引 | 本分支实测 | 内容 |
|---|---|---|
| `framework/sdc_stress_config.h:827-851` T11 块尾注 | `framework/sdc_stress_config.h:1434-1484`（块）、**`:1451-1453`**（尾注） | 尾注自认「未迁移调用方(gemm/sparse/sve 族的调用方短路)不经此处, 显式路径仍无警告; 采纳本机制只需删掉调用方短路」 |
| `tests/cpu/eigen_gemm/gemm_double_dynamic_square.cpp:76-80` | `tests/cpu/eigen_gemm/gemm_double_dynamic_square.cpp:61-72` | `gemm_mdim`：`:69-71` `if (v > 0) { *is_explicit = true; return (int)v; }` 在联合求解之前自行短路 |
| `README.md:502` | `README.md:489` | 「本用例显式路径暂无警告——T11b 将统一路由」 |

仓库自身四处披露该已知边界（T11b 收尾时须逐处改写，见 §3 U9）：

- `CLAUDE.md:157-162`（"gemm/sparse/sve callers still short-circuit … no warning yet (T11b follow-up; 10/47 eigen cases covered …)"）
- `README.md:489`（eigen_svd_cdouble_sve 行内括注）
- `docs/architecture.md:293-297`（同 CLAUDE.md 内容的英文版）
- 已删 plan `docs/superpowers/plans/2026-11-17-eigen-ci-timeout-fix.md`（`727cc8d3` 于 2026-11-18 删除整目录，**git 历史 `727cc8d3^` 可取回**；其 :269 附近原文「部分 GEMM 族走调用方短路无 T11 警告，为已知 T11b 跟进项」——注意 issue 评论所引 `31f6a99e^` 恢复点取不到该文件，它死于第二个清理 commit）

调用方侧显式通道读取面（本分支 grep `sdc_knob_int_clamped|sdc_knob_or_env_clamped`
于 `tests/cpu/eigen_gemm/` + `tests/cpu/eigen_svd/` + `tests/cpu/eigen_sparse/` 实测
**29 个文件**，含 1 个共享头）：eigen_gemm 17 件 + eigen_sparse 1 件 +
eigen_svd 10 件伴生体 + `sandstone_eigen_common.h`；另有
`gemm_double_ab_split.cpp:75` 裸 `getenv("GEMM_K_DIM")`（无 `-O` knob 管道）。
与 issue 评论「29 文件 + ab_split 裸 env」一致。

## 2. 根因

**机制设计**（`framework/sdc_stress_config.h`）：T11（P2b）给联合求解加了显式
knob 早退——`sdc_joint_dim_slice` 在 ARM64 分支开头（`:1454-1483`）经
`sdc_joint_knob_value`（cache 优先）+ `sdc_env_knob_int_clamped` 兜底读取显式
值，命中即按模型口径
`预期合计 = 全机线程 × c_bytes × n² + slices × shared`（`:1464-1465`）与全局
四源预算比对，超预算打 `[内存越界]`（`:1470`）、不足一半打 `[欠压力]`
（`:1475`），然后**原值返回**（`:1481`）。该早退位于退化区间/quick 早退**之前**
（`:1448`），语义「显式 > 一切自动收缩」。

**已迁移路径**（覆盖的 10 个 SVD 模板族用例）：
`tests/cpu/eigen_svd/sandstone_eigen_common.h:191-217` `calibrated_auto_dim` 被
**无条件调用**（`:196-197` 注释自认「现被无条件调用(含显式 knob 命中时)」），
显式值由 `sdc_joint_dim_slice` 的 T11 早退处理；`[压力映射]` 单次日志等价契约
钉在 `:201-203`。

**未迁移路径**（同一优先级链的另一条分叉）——按形态分五类：

| 类 | 代表锚点 | 短路形态 |
|---|---|---|
| A. gemm NEON 调用方短路 | `gemm_double_dynamic_square.cpp:69-71` | `if (v > 0) { *is_explicit = true; return (int)v; }` —— 早于联合求解，无警告 |
| B. max_arm（NEON+SVE 双档） | `gemm_double_max_arm.cpp:472-473`（`_sve2` 伴生体 `gemm_double_max_arm_sve2.cpp:480-481` 同型；另 `:359-363` preinit 侧同型） | `if (knob > 0) { dim = (int)knob; }` 早于 `sdc_joint_dim_slice(:488)` |
| C. SVE/SVE2 伴生体 | `gemm_double_dynamic_square_sve.cpp:45-52`（knob 读取位于 `arm64_require_sve_vl` **之后**）；SVD 伴生体 `svd_cdouble_sve.cpp:350,491` 等 10 件 | 同 A 形态，但与 tier pin 顺序耦合 |
| D. 裸 env 无 knob 管道 | `gemm_double_ab_split.cpp:75` | `getenv("GEMM_K_DIM")`，T11 的 env 兜底按 `test->id` 命名（`sdc_stress_config.h:1456-1457`），不覆盖该历史通道名 |
| E. eigen_sparse | `eigen_sparse.cpp:96-98` | knob+env 手工读取后自行返回 |

**定性**：这不是行为缺陷而是 T11 分期交付停在半程——尾注（`:1451-1453`）本身
就是迁移指南，四处文档披露如实。audit 定 Minor/可维护性正确：风险在于两条路径
并存期间，任何对早退顺序/查询序列/警告口径的修改（如 #488 is_explicit 契约、
quick 模式、`[压力映射]` 等价性）都要在两处同步，漏一处即语义漂移且无测试可拦。

## 3. 建议修复路径（T11b：U1→U9 逐单元，每单元独立 PR）

原则：**每单元一个 commit/PR**（rule 1），**显式值命中时的行为必须与迁移前逐字
等价**（原值返回、查询序列等价、`[压力映射]` 仍只打一次），迁移是搬运不是重设计。

### U1 框架使能（其余单元的前提）
`sdc_joint_dim_slice` 已能读显式值（`:1455-1457`），缺的是向调用方暴露
「本次返回值来自显式通道」的探测（gemm NEON 族 `gemm_mdim` 的 `is_explicit`
出参契约，issue #488：bad_alloc 分流「显式值不可降档」）。改
`framework/sdc_stress_config.h`：给 T11 早退块增加出参（如
`struct sdc_joint_args` 加 `bool *explicit_hit`，或独立 helper
`sdc_joint_knob_explicit(test, knob, lo, hi)`），早退块 `kv > 0` 时置位。
**不动既有 10 个 covered 调用方**。单独成 commit，本机可完整验证（见 §4）。

### U2 gemm NEON 4 件（`gemm_double_dynamic_square` ×3 + `double14`）
删 `gemm_double_dynamic_square.cpp:69-71` 的 v>0 短路：调用方先经 U1 探测置
`is_explicit`，再无条件走联合层（T11 早退原值返回 + 打警告）。保留
`sdc_gemm_init_shared` 的 bad_alloc 分流契约（`gemm_double_dynamic_square.cpp:58-59`
头注：显式值不可降档）。`double14.cpp` 同型收口。

### U3 gemm SVE/SVE2 伴生体 8 件
`_sve`/`_sve2`（dynamic_square、double14、float_dynamic_square 系）。这些文件
knob 读取位于 `arm64_require_sve_vl` 之后（`gemm_double_dynamic_square_sve.cpp:45-52`）
——迁移后联合层调用本身已在 tier gate 之后，顺序天然保持；逐件核对
「显式值仍只在 tier 校验通过后才被接受」。

### U4 eigen_sparse ×1
`eigen_sparse.cpp:96-98` 同 U2 形态收口。

### U5 max_arm ×2（最难）
`gemm_double_max_arm.cpp:472-473`、`gemm_double_max_arm_sve2.cpp:480-481`（及
preinit 侧 `:359-363`）。knob 已织入 duration 模型（issue #352），且 PR#384
检视契约钉死「读取通道经 `sdc_knob_or_env_clamped` 单一实现」（`:356-357`
头注自认）。迁移时读取移入联合层 T11 早退（`sdc_joint_knob_value` cache 优先 +
env 兜底），须证明查询序列与单一实现等价——预init 与 init 两处读取点都要收，
不能只收 `test_init` 侧。

### U6 ab_split ×1
`gemm_double_ab_split.cpp:75` 先补 `-O` knob 管道（现裸 `GEMM_K_DIM`），再路由。
env 通道名需显式裁定：T11 env 兜底按 `test->id` 命名（`eigen_gemm_double_ab_split.dim`），
`GEMM_K_DIM` 是历史名——迁移 PR 必须要么在早退块补认历史名，要么声明性废弃
（deprecation 日志），二选一并写入提交信息。

### U7 SVD SVE 伴生体 10 件
`svd_{cdouble,double,double2,float,fvectors}_sve/_sve2` 共 10 件（如
`svd_cdouble_sve.cpp:350,491`）。**必须经共享 body/dispatcher 头收口**
（`tests/cpu/eigen_svd/sandstone_eigen_sve_dispatch.h`），一处改 body/模板，
勿 10 份散改。

### U8 `_dump` ×3（先裁定，后动手）
`gemm_{double,float,cdouble}_dynamic_square_dump`。先裁定 K 维语义是否纳入
`[内存越界]`/`[欠压力]` 覆盖域（K 维不是 n² 内存主项）；裁定不纳入则在文档
披露其豁免理由，纳入则同 U2。

### U9 文档收口（末单元）
四处披露随迁移逐单元收敛、末单元统一改写：`framework/sdc_stress_config.h:1451-1453`
尾注删除、`CLAUDE.md:157-162`、`README.md:489`、`docs/architecture.md:293-297`。
新计划文档照常落 `docs/superpowers/plans/`（该目录已被 `727cc8d3` 清空，新建即可）。

## 4. 验证边界

**通用断言（每单元必验）**：
1. `./scripts/build.sh` clean、零新警告（本机 GCC 10.3.1 需
   `meson setup builddir --buildtype=release -Dcpp_std=gnu++20` 覆盖，记忆实测项）。
2. 行为等价三断言：显式 `-O <test>.mdim=<v>` 命中时返回值逐字等价（=v）、
   `[压力映射]` 日志仍只打一次（covered 先例契约
   `sandstone_eigen_common.h:201-203`）、is_explicit 契约不破坏（gemm 族）。
3. 回归：`-e zstd19 -t 2000 -n 1` → `exit: pass`。

**本机（no-SVE Kunpeng 920，CPU part 0xd01，96 核）可原生验证的单元**：
U1、U2、U4、U6、U8、U9（NEON/裸 C++ 用例）——显式 `-O` 超预算值跑一次应见
`[内存越界]`、取值下限应见 `[欠压力]`，引用真实输出入 PR。
U3/U5/U7 涉 `_sve`/`_sve2`/SVE dispatch 的单元本机 **SKIP 路径都不具备**
（no-SVE 主机连 tier gate 都到不了），按 CLAUDE.md rule 2 gem5-fi 条款在
`/home/sdc/wangxu/gem5-fi`（CHAOS/SE，同源双内核 `workloads/sdcshield/`）以
`--sve_vl {1,2,4,8}` 跑受影响 tier，引用 tier/checksum/`SDCSHIELD_FAILS=0`
真实输出；显式 knob 警告在 SE 模式下经 stdout 断言其出现。

**收尾断言（U9 后全量）**：§1 表中 29 文件清单上 `if (v > 0) … return` 短路
形态归零（grep 复查）；`-e eigen_gemm_double_dynamic_square -O mdim=4096 -n 1`
（或 gem5 等价腿）出现 `[内存越界]` 警告；四处文档披露无残留「暂无警告」字样。

## 5. 引用复查清单

```console
sed -n '1434,1484p' framework/sdc_stress_config.h          # T11 块 + :1451-1453 尾注
sed -n '61,72p'  tests/cpu/eigen_gemm/gemm_double_dynamic_square.cpp
sed -n '472,488p' tests/cpu/eigen_gemm/gemm_double_max_arm.cpp
sed -n '45,52p'  tests/cpu/eigen_gemm/gemm_double_dynamic_square_sve.cpp
sed -n '96,98p'  tests/cpu/eigen_sparse/eigen_sparse.cpp
sed -n '75p'     tests/cpu/eigen_gemm/gemm_double_ab_split.cpp
sed -n '191,217p' tests/cpu/eigen_svd/sandstone_eigen_common.h
git show 727cc8d3^:docs/superpowers/plans/2026-11-17-eigen-ci-timeout-fix.md | sed -n '265,272p'
gh api repos/wangxumarshall/sdcshield/issues/713           # 原文 + U1-U9 拆分评论
```

（`gh issue view 713` 在本仓会因 Projects-classic GraphQL 弃用告警失败，用
REST `gh api` 取原文——本 mission 全程如此。）
