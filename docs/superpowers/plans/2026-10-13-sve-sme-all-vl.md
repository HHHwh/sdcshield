# SVE/SME 全规格自适应（128–1024-bit）+ gem5 四档验证 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `sve512_stencil_axis_arm`、`sme_fmopa_stress_arm`、`sme_za_tileview_arm` 从 512-bit 硬编码改为 **VL/SVL 运行时自适应**（128/256/512/1024-bit 全规格），每步设计先在 gem5（本机无 SVE/SME 硬件）四档实证，再落 sdcshield 测试源码。`fpu_special_values_sve` 已 VL 自适应（`svcntw()/svcntd()` 运行时取 lane，缓冲上限 1024-bit）无需改动。

**Architecture:** 双仓流水：gem5-fi 仓（`feat/sve-sme-fi` 分支）先把三个 dual kernel 的 asm/几何全规格化并在 gem5 四档（`--sve_vl/--sme_vl 1/2/4/8`）跑 J1' 全绿 —— asm 参数化设计的实证载体；sdcshield 仓（`feat/sdc-seed-randomization` 分支）随后采用**同源 asm 模式**修改三个测试（skip 条件删除、几何/立即数运行时化），本机验证构建零警告 + skip 路径。

**Tech Stack:** sdcshield C++20/clang SME asm（`tests/cpu/arm64/`）；gem5-fi 双通道 kernel（clang `-march=armv9-a+sve+sme -ffp-contract=off`）+ `arm_chaos.py --sve_vl/--sme_vl`（Task 1 已建）。

**Spec:** 用户指令（2026-10-13 resume）："检测用例支持 sve 和 sme 所有规格版本，并在 gem5 中验证通过（因为物理本机不支持）"。种子随机化已由 PR #237/`6a94d48f` 完成，本计划不重复。

## Global Constraints

- **一补丁一单元**（两仓各自的 CLAUDE.md）：每 Task = 一个 commit；验证→commit→push。
- sdcshield DCO：`git commit -s` 尾行 `Signed-off-by: wangxu <wangxumarshall@qq.com>`；push feature 分支（`feat/sdc-seed-randomization`），绝不 push main。
- gem5-fi 分支 `feat/sve-sme-fi`，其 verify 纪律同前。
- **asm 参数化不变式**：元素大小立即数保留（f32 `lsl #2`、f64 `lsl #3`——与 VL 无关）；**几何立即数寄存器化**（循环边界/行距的 lane 数）；谓词 `ptrue` 任意 SVL 全真无需改。
- **golden 比对不变式**（PR #242 review coderabbit）：Task 4-6 的动态范围只改变参与比较的 lane/元素数量；每个 lane/元素必须与对应 golden/expected 使用 `memcmp` 或 `memcmp_or_fail` 做字节级独立比较，禁止 `sum`、`xor`、`reduce` 等跨 lane 聚合（单 lane SDC 不得被合并结果掩盖——AGENTS.md 检视重点 3）。
- **诚实边界**：sdcshield 测试的运行时（非 skip）行为本机不可验证（无 SVE/SME）；gem5 验证对象是**同源逻辑的 dual kernel**（asm 模式与测试完全一致），此等价性写入提交信息与文档。
- gem5 环境固定：`source ~/gem5-deps/env.sh`；`first_clock` 单位是 Cycles。
- 数值契约不因规格化改变：fmopa |a|,|b|<0.5625（64 步和 <21）；stencil 1/8 网格 <2^53；za 单射精确整数（PR #237 已论证，全规格下 n 变化不破坏证明——单射性 `(i*n+j)` 对任意 n≥2 成立、和上界按 n 缩放仍 <21/<2^53）。

---

### Task 1: gem5 kernel `sme_fmopa_dual` 全 SVL 化

**Files:** Modify `~/wangxu/operator/gem5-fi/workloads/sdcshield/sme_fmopa_dual.c`（asm 寄存器化 + N_F32 动态化）

**Interfaces:** Produces: 参数化 asm 模板（x15=lanes 运行时；`lsl #2` f32 恒定）——Task 4 的 sdcshield 测试直接复用此模式。

- [ ] Step 1: 修改：`N_F32` 由编译常量改运行时 `n = svl_bytes/4`（保留 K_FMOPA=64 与 SVL 无关）；asm 中 `lsl #6` → `mul x10, x9, x15; add x10, x1, x10, lsl #2`（x15=n）；`cmp x9, #16` → `cmp x9, x15`；a/b/golden/tile 数组按 n 动态（`static float a[64*32]` 上限 1024-bit=32 lanes）
- [ ] Step 2: 编译 `make -C workloads/sdcshield sme_fmopa_dual`（fp-contract=off 保持）
- [ ] Step 3: gem5 四档验证：`--sme_vl {1,2,4,8}` × {fixed, random 1..3} 全部 `SDCSHIELD_FAILS=0`，且 random 各 seed 各档 checksum 互异（四档 × 3 seed = 12 值全异）
- [ ] Step 4: commit（gem5-fi）+ push

### Task 2: gem5 kernel `sme_za_tileview_dual` 全 SVL 化

**Files:** Modify `~/wangxu/operator/gem5-fi/workloads/sdcshield/sme_za_tileview_dual.c`

**Interfaces:** Produces: `mov x8, <n>` + `cmp x9, x8` 模式（Task 5 复用）。

- [ ] Step 1: `N_D` → 运行时 `n = svl_bytes/8`；asm 的 `cmp x9, #8` → `cmp x9, x8`（x8 从操作数装入 n）；`mov x8, #8` 删除（改为参数）；`lsl #3`（f64）保留；数组上限 16（1024-bit）；golden 排列循环 n 化（`(i+1)%n` 对 2 的幂 SVL 成立）
- [ ] Step 2-4: 同 Task 1（编译 → 四档 × 模式验证 → commit + push）

### Task 3: gem5 kernel `stencil_axis_dual` 全 VL 化

**Files:** Modify `~/wangxu/operator/gem5-fi/workloads/sdcshield/stencil_axis_dual.c`

**Interfaces:** Produces: `bt = 2*svcntd()` 双向量几何 + `result + svcntd()` 存储偏移（Task 6 复用）。

- [ ] Step 1: `B_TILE` 常量 16 → 运行时 `bt = 2 * svcntd()`（res0/res1 各 svcntd() lanes；VL=128→4 … 1024→32）；src 上限 `[56*64*32]`；`svst1(ptrue, result + 8, res1)` → `result + svcntd()`；expected/比较循环 bt 化；`s_base` 偏移 `coord*stride*bt`
- [ ] Step 2-4: 同 Task 1（注意 `--sve_vl` 四档；stencil 的 run kernel `svmla_f64_m` 谓词形式任意 VL 自适应 ✓）

### Task 4: sdcshield `sme_fmopa_stress_arm` 全 SVL 化

**Files:** Modify `~/wangxu/operator/sdcshield/tests/cpu/arm64/sme_fmopa_stress_arm.cpp`

- [ ] Step 1: skip 条件 `svl_bytes != kTileN*4` → 改为支持范围检查：`n = svl_bytes/4`，`n < 4 || n > kTileNMax(32)` 时 skip（缓冲区上限对应 SVL=1024-bit，超出范围的 SVL 不得进入 run——实施修正，PR #242 review coderabbit）；`kTileN` 常量 → `n = svl_bytes/4` 运行时；asm 同 Task 1 模式寄存器化（`x15=n`、`mul+ lsl #2`、`cmp x9, x15`）；init 的 golden 三重循环与 vec_count 按 n 动态（上限 32 lanes）
- [ ] Step 2: 本机验证：`ninja -C builddir` 零警告；`-e sme_fmopa_stress_arm -t 2000 -n 1` → skip（本机无 SME，`ARM64 SME not available`——路径不变）
- [ ] Step 3: commit -s（注明 asm 模式与 Task 1 gem5 实证同源）+ push

### Task 5: sdcshield `sme_za_tileview_arm` 全 SVL 化

**Files:** Modify `~/wangxu/operator/sdcshield/tests/cpu/arm64/sme_za_tileview_arm.cpp`

- [ ] Step 1: skip `svl_bytes != kTileN*8` → 改为支持范围检查：`n = svl_bytes/8`，`n < 2 || n > kTileNMax(16)` 时 skip（实施修正，PR #242 review coderabbit）；asm `cmp x9, #8` → `cmp x9, x8`、`mov x8, #8` → 参数化（同 Task 2）；golden 排列 n 化；数组上限 16
- [ ] Step 2-3: 同 Task 4（构建 + skip 验证 + commit + push）

### Task 6: sdcshield `sve512_stencil_axis_arm` 全 VL 化

**Files:** Modify `~/wangxu/operator/sdcshield/tests/cpu/arm64/sve512_stencil_axis_arm.cpp`

- [ ] Step 1: `svcntb() != 64` / `svcntd() != 8` 两个固定宽度 skip → 改为支持范围检查：`bt = 2*svcntd()`，`bt < 4 || bt > kTileMax(32)` 时 skip（缓冲区上限对应 VL=1024-bit，实施修正，PR #242 review coderabbit）；`B_TILE` → `bt = 2*svcntd()`；`sve512_stencil_axis_data` 的 src 上限 32/lane；run 的 `result+8` → `result+svcntd()`；expected/golden 循环 bt 化（同 Task 3 模式）；注释更新（512-bit 设计历史 + 全 VL 支持 + gem5 四档实证引用）
- [ ] Step 2-3: 本机构建 + skip 验证（`test compiled with sve` 路径不变）+ commit -s + push

### Task 7: 文档同步 + 汇总

- [ ] Step 1: grep sdcshield 仓内 "512-bit only"/"512-bit VL only" 相关表述（stencil 描述、docs），更新为全规格表述；gem5-fi findings.md 补一行全规格化记录
- [ ] Step 2: 汇报：gem5 四档验证表（12+ checksum 值）+ 测试修改清单 + 诚实边界（kernel 等价验证、真机待验）

## Self-Review

- 用户要求全覆盖：4 测试中 3 个需全规格化（fpu_special 已自适应——计划明确说明）；gem5 四档验证在 T1-T3；测试源码 T4-T6；文档 T7 ✓
- asm 不变式明确（元素立即数保留、几何寄存器化）；数值契约跨规格的证明边界写明 ✓
- 无占位符；每步有命令与期望 ✓
