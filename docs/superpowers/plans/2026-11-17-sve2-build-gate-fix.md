# SVE2 Companion 双编译 march 降级（Multi-OS Verify Build 红根修）+ codespell lint 解锁 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复 main 分支 Multi-OS Verify 的 Build 步骤回归（PR #661 的 SVE2 companion 双编译用
`-march=armv9-a+sve2`，armv9-a 架构名需 GCC 11+，20.03/22.03 的 gcc-10 镜像全死），方法为把
march 降级为 `armv8.2-a+sve2`（SVE2 自 GCC 10 起即作为 Armv8-A 扩展可编，armv9-a 不再是
必要条件）；并解锁自 2026-11-10 起全红的 codespell lint（`tools/fp64_rowmajor_ko/verify.log`
是 checked-in 运行时日志工件，须在 .codespellrc 排除，不改日志本身）。

**Architecture:** 不动编译图结构、不加编译器探针、不摘库——只把 meson 双编译表里 `_sve2` 行
的 march 字符串从 `armv9-a+sve2` 改为 `armv8.2-a+sve2`，并把所有引用该 march 口径的证据面
（14 个 TU 描述串/头注、writing_tests.md 配方、CLAUDE.md、README）同步修订。诚实边界
（companion 编译产物零 SVE2-only 指令、SVE1 子集）以本机 gcc-12 双 march A/B objdump 审计
实证延续。codespell 单元仅改 .codespellrc 的 skip 列表。

**Tech Stack:** meson/ninja、GCC 12.3.1（本机）、GCC 10.3.1-10.oe2203（免 root 从
openEuler 22.03 源 `dnf download` + rpm2cpio 解包，与 CI 22.03 镜像同包同版）、objdump/as、
codespell。

**Spec:** 本任务派发书（2026-11-17）+ 协调者方向反转指令（2026-11-17：不做编译器守门，
改为 march 降级，gcc-10 也要支持 SVE2）。

## 方向反转记录（防止后人困惑）

初版设计是"meson 编译器能力探针（`cpp.has_argument('-march=armv9-a+sve2')`）守门，gcc-10
镜像上不编 SVE2 companion 库、`-e` 报 Cannot find matching tests"。用户 2026-11-17 反转：gcc-10
也要支持 SVE2。探针路径**未写任何代码即被放弃**（零沉没成本），本计划为反转后口径。
（arm64/meson.build 中 FMMLA/SVE2/SME 三库的既有 `cpp.has_argument` 守门不受影响——它们
march 是 `armv9-a+sve2+bf16+f32mm+f64mm`/`armv8.2-a+sve+f32mm+f64mm`，与 eigen companion 无关，
守门是它们既有的优雅降级机制。）

## 现场与根因（已取证，勿重复调查）

1. **Build 红**：main @ 98286caa 起（PR #661，2026-11-17 00:57 合入）10/15 镜像每日红在
   Build 步骤。现场：run 44011713221 的 job 133800036385 等，`cc1plus: error: unknown value
   'armv9-a+sve2' for '-march'`。20.03/22.03 镜像 gcc-10 系；本机 gcc-12.3.1 能编 → 本地从不红。
   #661 的多版本构建验证从未跑过（11-10 起 codespell lint 红 skip 了 pr.yaml 的 Multi-version
   build 矩阵，盲区）。
2. **codespell 红**：`./tools/fp64_rowmajor_ko/verify.log:47: parm ==> param, pram, parma`
   （4d2031e1 于 11-10 引入）。该行是内核模块 `modinfo` 的 `parm:` 字段名（运行时验证日志
   工件），非源码拼写错误；日志是证据，**禁止编辑日志本身**。
3. **真 gcc-10.3.1 双向实证（2026-11-17 本机取证，openEuler 22.03 源同包同版）**：

```
$ /tmp/gcc10/root/usr/bin/gcc --version | head -1
gcc (GCC) 10.3.1

$ gcc-10 -c -march=armv9-a+sve2 t.cc          # ← 复现 CI Build 死因，报错逐字一致
cc1plus: error: unknown value 'armv9-a+sve2' for '-march'
cc1plus: note: valid arguments are: armv8-a armv8.1-a armv8.2-a armv8.3-a armv8.4-a armv8.5-a armv8.6-a native
exit=1                                          # 注记列明 base 名止于 armv8.6-a：armv9-a 需 GCC 11+

$ gcc-10 -c -march=armv8.2-a+sve2 t.cc         # ← 修复口径，同一编译器接受
exit=0

$ gcc-10 -c -march=armv8.2-a+sve2 -msve-vector-bits={128,256,512,1024} t.cc   # 四档全过
vector-bits=128: OK / 256: OK / 512: OK / 1024: OK

$ gcc-10 -c -march=armv8.2-a+sve2 -msve-vector-bits=128 t_acle.cc   # <arm_sve.h> ACLE 路径
ACLE compile: OK, exit=0

$ g++ -c -march=armv8.2-a+sve2 -msve-vector-bits=128 t.cc   # 本机 gcc-12 同口径也接受
gcc-12 leg: OK, exit=0
```

   文档事实（多源一致）：SVE2 自 GCC 10 起即以 `-march=armv8-a+sve2` 形式支持（GCC 把 SVE2
   建模为 Armv8-A 的扩展）；`armv9-a` base 架构名 GCC 11 才认识。上引 gcc-10 实测与之互证。
4. **CI 现场口径**（multi-os-verify.yml:103-137）：22.03/20.03 用 `-Dcpp_std=gnu++20` +
   `CXXFLAGS="-DOPENEULER_{22,20}_03 -include framework/compat/cpp23_polyfill.h -include
   unistd.h -isystem framework/compat"`；24.03 用 `gnu++23` 不注入。gcc-10 腿须复刻此口径。
5. **诚实边界先例**：#661 收尾（b1b5fcca）objdump 审计门 = "40 个 sve2 目标库 SVE2-only
   助记符全零（uniq -c = '40 0'）"；设计文档已随分支清理不在仓（writing_tests.md 引用的
   specs/2026-11-12-eigen-sve2-companion-design.md 为悬空引用，不在本计划范围）。
6. **工作树异物**：`scripts/gha/verify-params.py` 有本会话之外的未提交本地改动
   （MATRIX_CI_DIM 200→128 等，节流单元的工作）。**禁止动它、禁止把它卷进任何 commit。**
   它是 Python CI 脚本，不影响 ninja 产物与本计划验证。同理不动：d7c93c4a（eigen 钉维，
   已合）、scripts/task_loop*。

## Global Constraints

- CLAUDE.md rule 1：一单元一 commit；顺序验证→commit→push。
- CLAUDE.md rule 2：100% 实证，commit message 引真实验证输出；诚实声明验证边界（本机无
  podman/无 24.03 外镜像容器，10 镜像 Build 终验 = 今夜 nightly 真容器跑）。
- CLAUDE.md rule 3：`git commit -s`（末行 `Signed-off-by: wangxu <wangxumarshall@qq.com>`，
  其后无内容、无 Co-Authored-By）；提交前 `git branch --show-current` 确认 main。
- 用户 2026-11-17 授权直推 main 免 PR（本任务派发书原文）；push 走 HTTPS（沙箱 SSH 被拒，
  见 memory）。
- CLAUDE.md rule 5：x86-64 untouched——march 串在 aarch64 guard 块内，x86 树不触（目检）。
- CLAUDE.md rule 6/7：文档同步（新口径："SVE2 companion 用 armv8.2-a+sve2 编译，GCC 10+ 可编；
  armv9-a 不再是必要条件"）。
- 日志工件不可改：verify.log 只在 .codespellrc 排除，绝不编辑。

## Review Focus

1. **march 降级改变 gcc-12 代码生成**（例如 armv9-a 基线带入 armv8.5+ 特性）→ 任务 2 的
   A/B objdump 40/40 逐字节 diff 捕获；若非全等须逐差分析是否 SVE2-only/边界外指令。
2. **gcc-10 只接受裸 march 但拒组合**（-msve-vector-bits 某档/ACLE 头）→ 已四档+ACLE
   实测全过（见上引输出）；再加 body TU 真编译兜底（任务 3）。
3. **残留 armv9-a+sve2 口径**（docs/TU 描述串漏改 → 描述串与实编译不符 = 谎言）→ grep 门
   （任务 1 Step 6）：eigen companion 语境零残留；允许残留清单 = arm64/meson.build 与
   paper docs 的 tests_arm64_sve2（FMMLA 库，march 含扩展后缀，非本单元）+ 历史计划文档。
4. **描述串变化被误当"行为变化"**：`-e <test>_sve2` 的 result/skip-category/exit 与
   skip reason 不变，仅描述串文本如实更新（本身就是修复的一部分）→ 任务 4 Step 3
   逐字对比 skip 行。
5. **codespell skip 过宽**（排除整个 tools/ 目录会漏扫真源码 .c/.sh）→ 只排 verify.log
   一个文件；排除后全仓重扫仍须零发现（其余拼写域无回归）。

---

### Task 0: 本计划入库

**Files:** Create: `docs/superpowers/plans/2026-11-17-sve2-build-gate-fix.md`（本文件）

- [x] Step 1: 取证（已完成，见"现场与根因"节）
- [ ] Step 2: `git add docs/superpowers/plans/2026-11-17-sve2-build-gate-fix.md && git commit -s -m "docs(plan): ..."`（先例：e689a571 先于 d7c93c4a）

### Task 1: march 降级 + 证据面同步

**Files:**
- Modify: `tests/cpu/meson.build:1285,1353-1356`（march 串 + 双目标注释）
- Modify: 14 个 TU 的 DECLARE_TEST 描述串（`tests/cpu/eigen_svd/svd_{cdouble,double,double2,float,fvectors}_sve2.cpp`、
  `tests/cpu/eigen_svd_jacobi/svd_{cdouble,double,float,fvectors}_sve2.cpp`、
  `tests/cpu/eigen_gemm/gemm_{double_max_arm,double_dynamic_square,float_dynamic_square,cdouble_dynamic_square,double14}_sve2.cpp`）
- Modify: `tests/cpu/eigen_gemm/gemm_double_max_arm_sve2.cpp:52`（头注 "-march=armv9-a+sve2"）
- Modify: `docs/coding/writing_tests.md:804,810`
- Modify: `CLAUDE.md:192`（SVE2 companion 双目标口径）
- Modify: `README.md:369`（eigen 表行 march 口径）

**Interfaces:** 无代码接口变化；唯一事实变化 = `_sve2` 行 march 字符串
`'armv9-a+sve2'` → `'armv8.2-a+sve2'`（`eigen_sve_body_specs` 表 foreach tgt 列表第二行），
库符号/namespace/dispatcher/运行时门控全部不动。

- [ ] Step 1: **先存 A/B 基线**（改动前）：`meson setup --reconfigure builddir`；对 40 个
  `builddir/tests/cpu/libtests_eigen_<pfx>_sve2_vl<t>.a` 逐库
  `objdump -d` 存 `/tmp/sve2audit/disasm_armv9/<lib>.txt`（改动前 march=armv9-a+sve2 的指令流基线）。
- [ ] Step 2: meson.build 改 march 串 + 两处注释（1285 行 "compiled for the armv9-a+sve2
  target"、1354 行 "sve2 adds the armv9-a+sve2 target" → armv8.2-a+sve2，并在 tgt 列表注释
  加 GCC 10/11 事实与 2026-11-17 回归一句）。
- [ ] Step 3: 14 个 TU 描述串 "body compiled for the armv9-a+sve2 target" →
  "body compiled for the armv8.2-a+sve2 target"（sed 可批量：`grep -rl 'armv9-a+sve2' tests/cpu/eigen_*/`）；
  max_arm 头注同理。
- [ ] Step 4: writing_tests.md / CLAUDE.md / README.md 口径同步（内容见 Review Focus 3；
  CLAUDE.md 补一句 gcc-10 现场与降级动机）。
- [ ] Step 5: `meson setup --reconfigure builddir`（meson 改动必须 reconfigure）。
- [ ] Step 6: grep 门：`grep -rn "armv9-a+sve2" tests/cpu/eigen_svd tests/cpu/eigen_svd_jacobi tests/cpu/eigen_gemm tests/cpu/meson.build docs/coding CLAUDE.md README.md` → 期望 0 命中；
  `git status` 确认未卷入 verify-params.py。

### Task 2: 双 march 等价性审计（gcc-12，rule 2 主腿）

**Files:** 无源码改动；产物在 /tmp/sve2audit/。

- [ ] Step 1: `./scripts/build.sh`（或先 `ninja -C builddir` 40 个 sve2 目标再全量）：干净、
  零新警告（全日志唯一 warning 须仍为预存豁免项）。
- [ ] Step 2: 40 库新指令流 `objdump -d` 存 `/tmp/sve2audit/disasm_armv82/<lib>.txt`；
  `diff -r` A/B → 期望 40/40 逐字节一致（符号名两版相同，diff 即指令流 diff）。
  若有差异：逐差分析指令是否 SVE2-only/边界外（不得含糊放行）。
- [ ] Step 3: 诚实边界直接复核（两道）：
  (a) **SVE1-twin 助记符直方图对比**：每个 family×tier，
  `objdump -d` 提取助记符（地址后首 token）`sort | uniq -c`，sve2 库 vs 对应 sve 库
  diff → 期望 40/40 全等（= 新 march 未引入任何 sve 库没有的助记符）；
  (b) **SVE1 反汇编器重汇编门**：40 库全部指令行（`objdump -d` 指令段）拼接为 .s，
  前置 `.arch armv8.2-a+sve`，`as` 汇编 → 期望通过（= 每条指令都在 SVE1-only march 下
  可编码，零 SVE2-only）；个别 objdump 回译语法噪声 → 逐条人工核对助记符并如实报告。
- [ ] Step 4: 引用真实输出入 commit message（库数、diff 结果、(a)(b) 结论）。

### Task 3: gcc-10 真机腿（与 CI 22.03 同包同版）

**Files:** 无源码改动；工具链在 /tmp/gcc10/。

- [ ] Step 1: 用解包 gcc-10 + oe22 libstdc++ 头（`-nostdinc++ -isystem`）复刻 CI 口径
  （`-std=gnu++20`、polyfill `-include`、vendored eigen5 `-isystem`、framework `-I`、
  新 march + 某档 vector-bits + EigenSVE2 宏），真编译一个 body TU
  （如 `tests/cpu/eigen_svd/svd_double_sve_body.cpp`，vl128 组合）→ 期望成功出 .o；
  同口径换回 `-march=armv9-a+sve2` → 期望复现 CI 报错原文。
- [ ] Step 2: 如该腿因宿主头文件混搭（24.03 glibc 头 × 22.03 libstdc++）出现与本单元无关的
  噪声：如实声明边界——gcc-10 裸 march/四档/ACLE/报错复现四证已在取证节，终验为今夜
  nightly 10 镜像 Build 真容器跑（boundary statement，不许含糊）。

### Task 4: 本机行为非回归（rule 2）

- [ ] Step 1: `./builddir/sdcshield --list-tests | wc -l` = 565；
  `--quality=-1 --list-tests | wc -l` = 683；两口径 `_sve2` 命中 = 0（本机 SVE1 口径不变）。
- [ ] Step 2: tests section 实例机判计数 = 701（readelf `tests` section 大小 / sizeof(struct
  test)=176，先例 b1b5fcca 方法）→ 14 个 `_sve2` 仍全量注册。
- [ ] Step 3: `-e eigen_svd_double_sve2 -t 5 -n 1 -v`（及 3 个 Jacobi 系加 `--quality=-1`）
  → result: skip / skip-category: CpuNotSupported / skip-reason "SVE2 required (HWCAP2_SVE2
  absent)…" / exit: pass，与改动前逐字一致（仅描述串文本如实更新）。
- [ ] Step 4: 回归：`-e zstd19 -t 2000 -n 1` → `exit: pass`。
- [ ] Step 5: x86 非回归（目检）：march 串/TU 均在 aarch64 guard 内，构建图对 x86 零变化。

### Task 5: codespell 单元（独立 commit）

**Files:** Modify: `.codespellrc`（skip 列表追加 `*fp64_rowmajor_ko/verify.log`，注释写明
"checked-in 运行时验证日志工件（kernel modinfo `parm:` 字段），非源码拼写域；日志为证据不可改"）。

- [ ] Step 1: 改 .codespellrc（只排该文件，不排目录——tools/fp64_rowmajor_ko/ 内有真源码）。
- [ ] Step 2: `/home/sdc/.local/bin/codespell` → 期望零输出零发现（其余拼写域无回归）。
- [ ] Step 3: commit -s + push。

### Task 6: 提交与直推（用户授权直推 main 免 PR）

- [ ] Step 1: commit 1（已含 Task 0 的 plan commit 则顺延编号）：docs(plan)。
- [ ] Step 2: commit 2：march 降级单元（Task 1-4 改动 + 引真实验证输出）→ push origin main。
- [ ] Step 3: commit 3：codespell 单元（Task 5）→ push origin main。
- [ ] Step 4: 每个 commit 前 `git branch --show-current` = main；每个 commit 前
  `git status` 确认 verify-params.py 未被卷入。
- [ ] Step 5: push 用 HTTPS（`git remote` 为 SSH 形式时以 https URL 显式 push）。

## 验证边界声明（诚实口径）

本机无 podman、无 20.03/22.03 容器。gcc-10 证据链 = 同源同版 rpm 解包工具链（裸 march 双向、
四档 vector-bits、ACLE 头、body TU 真编译、CI 报错逐字复现），加上 CI 11-03→11-10 窗口
Multi-version 矩阵对同 body 源 + SVE1 行全绿的历史实证（说明 gcc-10 对 SVE1 口径全部接受，
唯一差异点即 march 串）。**10 镜像 Build 的端到端终验 = 今夜 12:00 nightly pipeline 真容器**，
本计划交付时如实标注此边界，不以本机证据冒充容器验证。
