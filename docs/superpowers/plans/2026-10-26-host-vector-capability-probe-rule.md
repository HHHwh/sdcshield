# 主机向量能力「先探测、不假设」规则 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 CLAUDE.md 里「本机无 SVE/SME → SVE/SME 只能 gem5-fi」这条**写死机器身份**的规则，改成「先现场探测本机芯片的 SVE/SME 支持情况与详细规格；本机支持的档位必须本机原生验证，不支持的档位才走 gem5-fi」，并清除 `0xd01`/`96 核`/`128 核`/`192 核` 之类与实机可能冲突的硬编码身份。

**Architecture:** 纯文档改动，不碰代码。做法是**把"能力事实"从文档里撤出、换成"探测程序 + 判定规则"**：CLAUDE.md 承载规则与探测命令；`docs/architecture.md` 同步同一断言；两份日期化的 spec/plan 作为历史快照只加一行更正注记，不改写其结论。

**Tech Stack:** Markdown；AArch64 运行时探测（`/proc/cpuinfo`、sysfs MIDR、`getauxval` HWCAP、`prctl`、`rdvl`）。

**Spec:** 本 plan 由 2026-10-26 的实机实测直接驱动（证据见下「实测证据」节），无独立 spec 文档；证据原文见本文件节末。

## Global Constraints

- Reply in Chinese；文档正文沿用仓库既有语言分布（CLAUDE.md 主体英文、夹杂中文注释；不改语言风格）。
- CLAUDE.md 是**给任意一台机器上的 agent 读的**：新文本不得再出现任何具体机器的 part 号 / 核数 / 主机名作为「本机」的定义。
- 历史产物（`docs/superpowers/plans|specs/` 下带日期的文档）**不改写原结论**，只追加更正注记。
- 提交规则：`git commit -s`，末行 `Signed-off-by: wangxu <wangxumarshall@qq.com>`，其后无任何内容；先 `git branch --show-current` 确认不在 `main`；一文件一 commit。
- 不新增脚本、不改代码、不改 README（用户明确要求「只改文档」且范围限定在 CLAUDE.md + 三处过时断言）。

## 实测证据（本 plan 全部结论的来源，2026-10-26 本机）

```console
$ cat /sys/devices/system/cpu/cpu0/regs/identification/midr_el1
0x00000000480fd020                     # implementer 0x48 HiSilicon / part 0xd02 / rev 0
$ grep -m1 Features /proc/cpuinfo
Features : fp asimd ... sve asimdfhm ... svei8mm svef32mm svef64mm svebf16 i8mm bf16 dgh rng ecv   # 无 sme
$ nproc; lscpu | grep -E '^CPU\(s\)|On-line'
127
CPU(s): 128 ; On-line CPU(s) list: 0-121,123-127 ; Off-line: 122
$ prctl(PR_SVE_GET_VL) → 32 bytes ／ rdvl #1 → 32 bytes      # 默认 VL=256-bit，真执行
$ SET_VL {16B→16B, 32B→32B, 64B/128B/256B/… → 32B}          # 硬件支持 {128,256}-bit，可 pin 到 128
$ HWCAP2=0x000000000009ff81 → HWCAP2_SVE2=0, HWCAP2_SME=0
$ 指令实测：add/fmla.d/fmla.h/fmls/ld1d/tbl/ptrue/addvl → EXEC OK
            smmla/ummla/bfdot/bfmmla/fmmla(z.s)/fmmla(z.d) → EXEC OK
            smulh z.b / sqrshl p/m / bdep / bext → SIGILL      # SVE2 与 BITPERM 确实没有
            SMSTART / mrs smidr_el1 → SIGILL                   # SME 软件不可见
$ /boot/config-$(uname -r) 无 CONFIG_ARM64_SME；/proc/sys/abi/ 无 sme_default_vector_length
$ ./builddir/sdcshield -e sve_rot_ldr_at_top -t 2000 -n 1 → exit: pass   # 原生 SVE 测试可跑
```

**诚实边界（必须写进文档）**：本机只能证明"软件层面不可见 SME"——`ID_AA64PFR1_EL1` 读得 `0x20`（SME 字段 `bits[27:24]=0b0000`，按规范字面＝已实现，但本世代核很可能是 RES0），且 `CONFIG_ARM64_SME=n` 时 EL0 的 `SMSTART` 必然 SIGILL。**两者都不构成硬件判据**，硬件层面需 `CONFIG_ARM64_SME=y` 内核复测。文档措辞必须区分"软件不可见"与"硬件不存在"。

## File Structure

| 文件 | 责任 | 动作 |
|---|---|---|
| `CLAUDE.md` | agent 的操作规则；第 2 条（gem5-fi 条款）+ 平台怪癖节 | 改写规则、撤出机器身份 |
| `docs/architecture.md` | 框架/构建的活文档（documentation map 指向它）；含同一过时断言 | 同步为"按机探测" |
| `docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md` | 2026-10-23 日期化设计快照 | 仅追加更正注记 |
| `docs/superpowers/plans/2026-10-23-sve-sme-all-vl2-p0-p1.md` | 2026-10-23 日期化计划快照 | 仅追加更正注记 |

---

### Task 1: CLAUDE.md 规则 2 —— 探测优先的验证路径

**Files:**
- Modify: `CLAUDE.md:15-31`（Non-negotiable rules 第 2 条尾部 gem5-fi 条款）
- Modify: `CLAUDE.md:3-9`（文件头 "ported to ARM64 (Kunpeng 920 / ARMv8.1+)"）

**Interfaces:**
- Consumes: 无（首个任务）
- Produces: 规则编号 `2` 的稳定文本；Task 3 会以「CLAUDE.md 第 2 条」指代它

- [ ] **Step 1: 切分支（不在 main 上提交）**

```bash
cd /home/sdc/wangxu/sdcshield
git branch --show-current          # 期望 main
git checkout -b docs/host-vector-probe-rule
git branch --show-current          # 期望 docs/host-vector-probe-rule
```

- [ ] **Step 2: 替换规则 2 的 gem5-fi 条款**

把 `CLAUDE.md` 中**从** `**gem5-fi rule when the host chip cannot run it**:` **到** `\`sme_za_tileview_dual.c\` at SVL 128/256/512/1024-bit).` 的整段，替换为下面这段（注意：**不要**在编号列表内嵌代码栅栏，用行内 `code`）：

```markdown
   **Probe the host before assuming anything** — the lab runs several ARM64
   hosts that differ in part number, core count and vector capability
   (SVE/SVE2/SME, VL/SVL tiers). Never quote a part number, a core count or
   "this machine has/hasn't X" from memory or from another host; measure
   THIS machine first (MIDR = implementer/part/rev, HWCAP-derived features
   incl. `sve`/`sve2`/`sme`, online CPUs, and the *real* per-thread vector
   length — HWCAP presence is not proof an instruction executes, so probe
   the instruction itself where it matters; an unsupported one raises
   SIGILL):
   `cat /sys/devices/system/cpu/cpu0/regs/identification/midr_el1`,
   `grep -m1 Features /proc/cpuinfo`, `nproc` + `lscpu | grep On-line`,
   `prctl(PR_SVE_GET_VL)` / `rdvl` for VL, `mrs smidr_el1` / `SMSTART` for
   SME, and `./builddir/sdcshield --dump-cpu-info` (features + SVE VL line).
   - **Capability present on this host → verify natively here** with a real
     run and real output. SKIP-only is never acceptable evidence for a
     functional change.
   - **Capability absent on this host → gem5-fi** (`/home/sdc/wangxu/code-review/gem5-fi`,
     CHAOS/SE, same-source dual kernel under `workloads/sdcshield/`) at the
     affected VL/SVL tiers, quoting real run output (tier, checksum,
     `SDCSHIELD_FAILS=0`); if gem5 validation is genuinely impossible, state
     that verification boundary honestly instead.
   - Never pass off "SKIP reason unchanged" as the verification of a
     functional change (PR #242 precedent: input-formula fix verified by
     `sme_za_tileview_dual.c` at SVL 128/256/512/1024-bit).
```

- [ ] **Step 3: 改文件头，撤出芯片身份**

`CLAUDE.md:7` 的 `ported to ARM64 (Kunpeng 920 / ARMv8.1+); x86-64 is the untouched reference` 改为：

```markdown
to ARM64 (ARMv8.1+; the lab hosts differ in part number, core count and
vector capability — probe each machine, see rule 2); x86-64 is the untouched
reference
```

- [ ] **Step 4: 验证**

```bash
cd /home/sdc/wangxu/sdcshield
grep -n "gem5-fi rule\|0xd01\|96 CPUs\|96-CPU\|Kunpeng 920" CLAUDE.md   # 期望：无输出（退出码 1）
grep -n "Probe the host before assuming anything" CLAUDE.md             # 期望：命中 1 行
```

再把新文本里点名的命令**真跑一遍**并核对输出与文档一致：

```bash
cat /sys/devices/system/cpu/cpu0/regs/identification/midr_el1
grep -m1 Features /proc/cpuinfo
./builddir/sdcshield --dump-cpu-info | head -1
```
期望：MIDR 打印 `0x00000000480fd020`；Features 行含 `sve` 且不含 `sme`；`--dump-cpu-info` 首行打印
`Detected CPU: …; family-model-stepping (hex): 48-d02-00; CPU features: …`。
（若 `--dump-cpu-info` 首行没有特征串或名字为 `cortex x3b`，**以命令实测输出为准**，不要为了让文档好看而改文案——
`cortex x3b` 是 `CPU_ARCHITECTURES` 的**特征位匹配名**（`framework/device/cpu/simd-arm.conf:139`），不是 MIDR 身份。）

- [ ] **Step 5: Commit**

```bash
cd /home/sdc/wangxu/sdcshield
git add CLAUDE.md
git commit -s -m "docs(claude): 规则 2 改为「先探测本机 SVE/SME 规格」——支持的档位本机原生验证，不支持的才走 gem5-fi；撤出写死的芯片身份"
git log -1 --format='%s%n%n%b' | tail -3
```
期望：末行恰为 `Signed-off-by: wangxu <wangxumarshall@qq.com>`，其后无内容。

---

### Task 2: CLAUDE.md 平台怪癖节 —— 按机探测，删除身份硬编码

**Files:**
- Modify: `CLAUDE.md:103-121`（`## Known platform quirks …` 整节）

**Interfaces:**
- Consumes: Task 1 产出的规则 2 文本（本节引用「rule 2」）
- Produces: 一个不含机器身份的怪癖节；Task 3 的 architecture.md 文案与之措辞一致

- [ ] **Step 1: 替换标题与前言**

`## Known platform quirks (Kunpeng 920, openEuler 24.03 SP3)` 改为：

```markdown
## Known platform quirks (probe per host — this section binds no machine)
```

并在其后加一行前言：

```markdown
这些条目来自**多台**开发机的实测记录；条目本身可能只对其中一部分成立。
换机先按 rule 2 探测，与实测冲突时以本机实测为准，并顺手更新本节。
```

- [ ] **Step 2: 逐条去身份化**

- `cluster_id` 条：在 `firmware/ACPI-PPTT artifact, read as-is.` 前补 `(observed on a Kunpeng-family board; read as-is wherever it appears)` —— 保留结论，去掉"这就是本机"的暗示。
- cpufreq/thermal 条：把 `No \`cpufreq\` → …skip gracefully; no CPU \`thermal_zone*\` zones → thermal throttle is a no-op here.` 改为：

```markdown
- cpufreq/thermal **因机而异**：有的板有 `cppc_cpufreq`（如 400M–2.9GHz 独立 policy），
  有的完全没有。先看 `ls /sys/devices/system/cpu/cpu0/cpufreq` 与
  `ls /sys/class/thermal/`：缺 `cpufreq` 时 `--vary-frequency`/
  `--vary-uncore-frequency` 优雅 SKIP；缺 CPU `thermal_zone*` 时 thermal
  throttle 是 no-op。不是缺陷。
```

- eigen flakiness 条：把括号内的 `(ULP-level diffs vs strict memcmp; the original "192 CPUs" records are from the cortex x3b machine — see the SVE rule below, this host is the 96-CPU 0xd01)` 改为：

```markdown
(ULP-level diffs vs strict memcmp under full-system multithreading; the
   larger the core count, the more likely — machine-independent, `-n 1`
   always passes. Eigen/large-core-count limitation, not a port defect.)
```

- SVE 条：整条（`- SVE runtime-VL rule: this host (0xd01, Kunpeng 920, 96 CPUs) has NO SVE/SME — …` 到 `…\`sleef_sve\` stays at 128 by design.`）替换为：

```markdown
- **Vector capability (SVE/SVE2/SME, VL/SVL tiers) is per-host — probe it,
  never assume it.** Measured lab hosts differ widely: some expose SVE with
  a pinnable VL and no SVE2/SME, some expose the full SVE2 set + SME/SME2 at
  a wider VL. Verification path follows rule 2: tiers the host supports are
  verified natively here; tiers it lacks go to gem5-fi
  (`--sve_vl/--sme_vl {1,2,4,8}` = 128/256/512/1024-bit). `tests_sve`
  compiles at fixed 128-bit, so on SVE hosts with VL≠128 the eigen SVD test
  needs a VL-pinning launcher (`scripts/eigen-sve-double/` precedent; the
  external `/tmp/vlrun` tool no longer exists); `sleef_sve` stays at 128 by
  design.
- **SME caveat**: "HWCAP2_SME=0 / `SMSTART` raises SIGILL" only proves SME is
  *not enabled by this kernel* — with `CONFIG_ARM64_SME=n` (check
  `/boot/config-$(uname -r)`; the SME sysctl
  `/proc/sys/abi/sme_default_vector_length` will also be absent) EL0 `SMSTART`
  traps regardless of silicon. A kernel-level "absent" is not a hardware
  verdict; say "software-invisible" and state the boundary.
```

- [ ] **Step 3: 验证**

```bash
cd /home/sdc/wangxu/sdcshield
grep -n "0xd01\|0xd02\|96 CPUs\|96-CPU\|128 CPUs\|192 CPUs\|cortex x3b\|Kunpeng 920" CLAUDE.md
```
期望：**无输出**（退出码 1）。
再用与事实核对的反向检查：

```bash
grep -n "probe per host" CLAUDE.md; grep -n "SME caveat" CLAUDE.md
```
期望：各命中 1 行。

- [ ] **Step 4: Commit**

```bash
cd /home/sdc/wangxu/sdcshield
git add CLAUDE.md
git commit -s -m "docs(claude): 平台怪癖节去身份化——改为按机探测，并补 SME『软件不可见≠硬件不存在』的判定边界"
```

---

### Task 3: docs/architecture.md 同步同一断言

**Files:**
- Modify: `docs/architecture.md`（"Eigen 5.0 SVE double packets" 节末尾 + SVE/SME 探测段）

**Interfaces:**
- Consumes: Task 2 的措辞（SVE/SME 判定边界）
- Produces: 与 CLAUDE.md 完全一致的事实陈述；无下游任务

- [ ] **Step 1: 打出待改区域，确认行号**

```bash
cd /home/sdc/wangxu/sdcshield
grep -n "this host is VL=256\|cortex x3b\|awaits an SVE/SME host\|no SVE at all" docs/architecture.md
sed -n '100,125p;170,185p' docs/architecture.md
```

- [ ] **Step 2: 改写 "Eigen 5.0 SVE double packets" 节末的运行时 VL 段**

把 `The earlier "this host is VL=256" record was written on the cortex x3b machine; the current host (0xd01, Kunpeng 920) has no SVE at all, so the SVD SVE test cleanly skips here — on SVE hosts with VL≠128 it needs a ...` 整句，改为：

```markdown
Whether a host has SVE at all is per-machine: probe it (CLAUDE.md rule 2)
instead of assuming — on a host without SVE the SVD SVE test cleanly skips,
and on SVE hosts with VL≠128 it needs a `prctl(PR_SVE_SET_VL, 16)` pin (see
`scripts/eigen-sve-double/` and `arm64_sve_pin_vl()`).
```

- [ ] **Step 3: 改写探测段的"awaits an SVE/SME host"表述**

把 `…so their runtime verification still awaits an SVE/SME host (the probes and SKIP paths do run on this host).` 改为：

```markdown
…so their runtime verification executes wherever the capability probes pass
(probe the host — CLAUDE.md rule 2; the probes and SKIP paths run everywhere).
```

- [ ] **Step 4: 验证**

```bash
cd /home/sdc/wangxu/sdcshield
grep -n "0xd01\|cortex x3b\|no SVE at all\|awaits an SVE/SME host" docs/architecture.md   # 期望：无输出
grep -n "CLAUDE.md rule 2" docs/architecture.md                                          # 期望：命中 ≥1 行
```
并复核该文件里"本机"相关的其余陈述不再有写死的 part 号/核数：

```bash
grep -n "this host\|本机" docs/architecture.md | head -20     # 人工逐条确认无机器身份断言
```

- [ ] **Step 5: Commit**

```bash
cd /home/sdc/wangxu/sdcshield
git add docs/architecture.md
git commit -s -m "docs(architecture): 同步『按机探测 SVE/SME』——删除 0xd01 无 SVE 的过时断言"
```

---

### Task 4: 两份日期化 spec/plan 追加更正注记（不改原结论）

**Files:**
- Modify: `docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md:69`（`「host is VL=256」是另一台机器（cortex x3b，…` 处）
- Modify: `docs/superpowers/plans/2026-10-23-sve-sme-all-vl2-p0-p1.md:215`（引用 CLAUDE.md 旧 SVE 规则处）

**Interfaces:**
- Consumes: Task 1/2 的规则文本
- Produces: 无

- [ ] **Step 1: 定位两处**

```bash
cd /home/sdc/wangxu/sdcshield
sed -n '66,72p' docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md
sed -n '213,217p' docs/superpowers/plans/2026-10-23-sve-sme-all-vl2-p0-p1.md
```

- [ ] **Step 2: 各追加一行更正注记（保持原文不动）**

两处**紧邻其后**各插入：

```markdown
> **2026-10-26 更正**：本机实测 MIDR `0x480fd020`（part `0xd02`）**有 SVE**，
> 默认 VL=256-bit（`rdvl`=32B），可 pin 到 128-bit；HWCAP2 无 `SVE2`/`SME`
> （SVE2 指令实测 SIGILL；SME 仅"软件不可见"，见 CLAUDE.md 第 2 条与平台怪癖节）。
> 因此"host is VL=256 属于另一台机器"的断言不成立，机器身份一律现场探测。
> 本注记只更正事实，不改写当日结论。
```

- [ ] **Step 3: 验证**

```bash
cd /home/sdc/wangxu/sdcshield
grep -rn "2026-10-26 更正" docs/superpowers/ | wc -l    # 期望：2
git diff --stat
```
期望：两份文件各 +6 行左右，**无原有行被删改**（`git diff` 中不出现 `-` 开头的正文行，除空行对齐外）。

- [ ] **Step 4: Commit & push**

```bash
cd /home/sdc/wangxu/sdcshield
git add docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md docs/superpowers/plans/2026-10-23-sve-sme-all-vl2-p0-p1.md
git commit -s -m "docs(plans): 2026-10-23 SVE/SME 设计快照补更正注记——本机有 SVE-256，机器身份改现场探测"
git push -u origin docs/host-vector-probe-rule
git log --oneline -4
```

---

### Task 5: 全仓一致性终检（不产生新提交）

**Files:** 无

**Interfaces:**
- Consumes: Task 1–4 的全部产物
- Produces: 结论（供人工决定是否开 PR）

- [ ] **Step 1: 全仓扫写死的机器身份**

```bash
cd /home/sdc/wangxu/sdcshield
grep -rn "this host (0xd01\|96 CPUs\|96-CPU\|no SVE at all\|has NO SVE" CLAUDE.md docs/**/*.md 2>/dev/null
```
期望：无输出。**注意范围**：`docs/cases/**`、`docs/cpu/arm64/kungpeng/*.md`、`docs/paper/ref-paper-experiment-note/*` 里的
`0xd22`/`0xd01`/`cn23154` 是**具体被测机的案例记录**（正确用法），**不得**改动——本规则只禁"用某台机器代表本机"。

- [ ] **Step 2: 交叉链接核对**

```bash
cd /home/sdc/wangxu/sdcshield
grep -n "rule 2" CLAUDE.md docs/architecture.md
grep -n "sve_vl\|sme_vl" CLAUDE.md docs/architecture.md
```
期望：规则编号、`--sve_vl/--sme_vl {1,2,4,8}` = 128/256/512/1024-bit 三处含义一致。

- [ ] **Step 3: 汇报**

向用户汇报：改动文件、4 个 commit 的 hash、`git diff --stat`，以及"是否开 PR"由人决定（仓库规则：永不自动 merge）。

---

## Self-Review

**1. Spec coverage**（对照用户两条要求）：
- 「先检查芯片 SVE/SME 支持情况和详细规格；支持的本机测试，不支持走 gem5-fi」→ Task 1 Step 2（规则正文）+ Task 2 Step 2（SVE 条）。
- 「移除 `0xd01`/96 核 与本机 `0xd02`/128 核 的不一致」→ Task 1 Step 3 + Task 2 Step 2（删净 `0xd01`/`96 CPUs`/`Kunpeng 920`/`cortex x3b` 断言）+ Task 3（architecture.md 同源断言）+ Task 4（历史快照注记）。
- 用户追加决策「只改文档」→ Global Constraints 明列不新增脚本/不改代码；「一并修正三处」→ Task 3、Task 4。

**2. Placeholder scan**：无 TBD/TODO；每个 Step 均为可执行命令或**完整替换文本**（替换文本逐字给出，不含"类似上文"）。

**3. Type consistency**：规则编号统一为 `rule 2`／`第 2 条`；VL 档位记号统一为 `--sve_vl/--sme_vl {1,2,4,8}` = 128/256/512/1024-bit；术语统一为「supported tier → 本机原生验证」「absent tier → gem5-fi」；SME 一律表述为"软件不可见（software-invisible）"而非"不存在"。
