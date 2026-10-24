# SVE/SME 全 VL 二期 P3（sve512 家族×10）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** sve512 家族 10 文件从 512-bit 单值门控改为全 VL（128..1024-bit）动态适配：8 个浅改（门控/漂移守卫/文案）+ xlswave Phase A 深重构（bt 参数化）+ libomp 栈缓冲修复，gem5-fi 三 kernel 48 run 四档实证先行。

**Architecture:** 双仓流水：gem5-fi kernel 批次先行（含 issue #182 的 VL=128 golden 失配疑点清除）→ sdcshield 逐文件重构（自 `origin/feat/arm64-vl-api` 终头 `ec65712b` 切出，含 bot 评审修复）→ 文档收尾。

**Tech Stack:** SVE sizeless intrinsics（svmla 链/gather-scatter/stencil 双向量）、arm64_vl.h（PR #277）、gem5-fi（--sve_vl {1,2,4,8}）。

**Spec:** `docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md` §6 P3 / §7 / §8 / §9（已在 main）。

## Global Constraints

- **单位军规（P2 Critical-1 教训）**：`arm64_require_sve_vl` 返回**字节**；f64 lanes = svl/8（∈[2,16]）、f32 lanes = svl/4（∈[4,32]）；每处换算必须带单位注释 + release 生效的显式范围检查（assert 在 NDEBUG 下失效）。
- **issue #182 风险前置**：512 门控的历史缘由是 GH runner（VL=128）上 golden 实测失配——**kernel 四档实证必须在门控放宽之前全绿**，否则该疑点未清，不得改门控。
- 分支：`feat/sve512-allvl` 自 `ec65712b`（`origin/feat/arm64-vl-api` 终头）切出——PR #277 已合入 main（505eeaff），`git diff ec65712b 505eeaff` 为空、两树相同；964eba30 是其祖先（pr-loop bot 的 include 规范化等修复已包含，勿按 964eba30 checkout——会落在缺少 ec65712b 所合 main 内容的基点上）；后续阶段自 main 切出。测试 include 用尖括号 `<device/cpu/arm64/arm64_vl.h>`（bot 规范化后的形态）。
- 本机 0xd01 无 SVE：只验证构建零告警 + SKIP 路径 + 回归；功能结论以 gem5 四档为准（规则 2b）。验证 subagent 隔离并行。
- 构建入口 `./scripts/build.sh`；回归 `-e zstd19 -t 2000 -n 1` → `exit: pass`；DCO `git commit -s` 末行 wangxu 签名；一文件一 commit；x86 零改动。
- gem5-fi 分支：`feat/sve512-dual` 从 `feat/fmopa-za-dual`（ebe08632）堆叠；Makefile 选择性暂存纪律（他人 PR244 hunks 不捆绑）。

## 探索结论基线（2026-10-24 评估代理，file:line 已核）

- **8/10 文件几何已 svcnt 化**（sizeless + 动态 vector + svcnt 变量），512-ism 全为文案；编译本就无 -msve-vector-bits（arm64_sve_lib 真 VLA）。
- **xlswave**（:60 B_TILE=16、:102/:107 `n+8`、:253-254 `result+8` 越界写、:256 闭式 96.0×(jj+1)）与 **libomp**（:236 `micro[8]` 栈缓冲，:241 svst1 写 svcntd 个 double）是仅有的承重 512-ism。
- kernel 聚类：**A** 串行 FMLA 链（f64/f32 × 有限带/特殊值 × 平流/块 slab，6 文件 + libomp ACTIVE 窗同形）；**B** gather/scatter（_arm 1-D 带标签 / _svd 2-D 块 whilelt，xlswave Phase B 与 _svd 逐字同 kernel）；**C** stencil 双向量（xlswave Phase A，**全 VL kernel 已存在**：gem5-fi `feat/sve-sme-all-vl` @ 7bed7f0e 的 stencil_axis_dual.c，勿取工作树旧版）。
- 共性缺口：10 文件均无 run 侧 VL 漂移守卫；手写 getauxval；文案与门控矛盾。

## Review Focus

1. **issue #182 疑点**：VL=128 golden 失配复现与否——kernel 四档通道 A 是判据（Task 1）。
2. **单位换算错**（P2 Critical-1 同类）：svl/8 vs /4 vs /32 混淆——每文件显式范围检查 + kernel 映射一致。
3. **xlswave 越界**：result+8 模式在非 512 档的写越界/漏检——bt 参数化后 result 按 lanes 分隔 + 上限缓冲。
4. **libomp 栈溢出**：micro[8] @1024-bit 写 16 double——尺寸上限修复。
5. **块 slab 寻址**（_svd 变体）：步距 CHAIN_STEPS×lanes 随档位变化的正确性——kernel 含块 slab 模式。
6. **门控放宽后 skip 文案/文档与实态矛盾**（meson 注释/README/docblock）。

---

### Task 1: gem5-fi kernel 批次（3 kernel，48 run）

**Files:**
- Create（gem5-fi）: `workloads/sdcshield/sve512_chain_dual.c`、`workloads/sdcshield/sve512_gather_dual.c`
- cherry-pick: `7bed7f0e` 的全 VL `stencil_axis_dual.c`（`git show 7bed7f0e:workloads/sdcshield/stencil_axis_dual.c` 取版本覆盖工作树旧版）
- Modify: Makefile（+2 目标）+ findings.md（实录）

**Interfaces:**
- Produces: ①四档全绿实录（含 issue #182 疑点清除声明）；②单位映射锚（lanes=bytes/8|/4）；③与 Task 2-5 同源的 kernel 形态。

- [ ] **Step 1: 分支**：`git checkout -b feat/sve512-dual`（自 feat/fmopa-za-dual @ ebe08632）。
- [ ] **Step 2: 写 `sve512_chain_dual.c`**（聚类 A，单次运行覆盖 4 模式，as-built 口径）：
  - `rdsvl→svl_bytes`，`lanes = svl_bytes/8`（f64），范围 [2,16] 否则 exit 3
  - 模式段（一次 main 内顺序执行，各自打 checksum）：
    - `chain_f64`：CHAIN_STEPS=512 步 `svmla_f64_x` 串行链（双流 ld1，步距=lanes×8 寄存器化），标量 `fma` 同序 golden，逐 lane 字节比对；操作数 |x|≤2 有限带（固定/随机两态）
    - `chain_f64_slab`（**独立模式，as-built——非并入 chain_f64**）：19×19 slab 网格、每 slab 双流 STEPS×lanes doubles 背靠背（_svd 布局），按 `((row*19+col)*(2*STEPS*lanes))` 块步距取代表块跑同形链（验证 _svd 变体的 slab 寻址随档位缩放）
    - `chain_f32`：同形态 `svmla_f32_x`，lanes_w=svl_bytes/4，指数带 0x3FE/0x3FF 的 f32 版
    - `special_f64`：64 步链 + 每步随机抽 IEEE 特殊值表（NaN/±Inf/有限），类别比对 golden（NaN 类内、±Inf 同号、有限字节精确——fpu_special_sve_dual.c 的比对函数可直接借形；**golden 与 kernel 累加器起始值必须对称**——e0eba9cd 记录 Important-1：golden 起始 1.0 vs hw 累加器 0.0 的不对称曾因 NaN 掩盖而漏检，已另行修复+重验，实现时勿复现该不对称）
  - 输出：`mode=<name> lanes=N FAILS=n`（每模式一行）+ 末行总 `SDCSHIELD_FAILS` 与全局 FNV checksum
- [ ] **Step 3: 写 `sve512_gather_dual.c`**（聚类 B，2 模式）：
  - `1d_tag`：Fisher-Yates 置换索引表（gn=64×lanes 动态）+ `svld1_gather_u64index_f64` + `svmla` + `svst1_scatter_u64index` + mantissa 低 20 位索引标签断言（镜像 gather_scatter_arm 的 check_read_offsets）
  - `2d_block`：300×300 固定工作区 + 16×16 块内置换 + `svwhilelt_b64(off, 90000)` 尾（镜像 gather_scatter_svd；90000 与 VL 无关）
  - 各模式标量 golden + 逐元素比对 + checksum
- [ ] **Step 4: 取 7bed7f0e 的 stencil_axis_dual.c** 覆盖工作树（`git show 7bed7f0e:workloads/sdcshield/stencil_axis_dual.c > workloads/sdcshield/stencil_axis_dual.c`），Makefile 确认目标存在。
- [ ] **Step 5: Makefile +2 gcc 目标**（`-march=armv9-a+sve`，无 SME 助记符），选择性暂存纪律。
- [ ] **Step 6: 派验证代理**：3 kernel × 4 档 × {fixed 1, random 1..3} = 48 run（`--sve_vl $T`）；判据：各模式 lanes 逐档 2/4/8/16（f64）与 4/8/16/32（f32）、`SDCSHIELD_FAILS=0`、同档 fixed/random checksum 一致性如实记录；checksum 的输入态/档位差异**按模式分判，不作统一通过条件**——值依赖模式（chain 族：checksum 是输入值的函数）不同输入态、不同档位各异，如实记录；结果与输入无关模式（gather 的 dst 终态：scatter 使每个地址的终值与 lane 分块和置换无关，FNV 按地址序聚合——源码核实）同档一致系设计属性，如实记录、不作判据——**勿改成"不同 random seed 的 checksum 互异"判据**（对 gather 同样假失败：其 dst 终态与置换无关，seed 互异不成立）。**spec §7 回归锚条款**：stencil 复用 7bed7f0e 的 16 run，其 512-bit 档四态 checksum（da6f0aba/610e3ce5/05c9f28e/641c22fa）与旧版逐位一致（回归锚成立）；chain/gather 为新写 kernel、无旧版可锚——按 P2（PR #279）"无回归锚（诚实声明）"先例声明 N/A，勿伪造锚。**PR #279 Erratum#3 教训转移**：取证脚本取退出码用 `PIPESTATUS[0]`（管道 `$?` 取到的是末端 grep 的退出码，gem5 崩溃可被误记为 exit=0）；kernel 逐 lane 比对 memcpy 化（消严格别名 UB）。**若 128-bit 档任何模式 golden 失配 → issue #182 疑点未清，停止并如实上报（不改数据迁就）**。findings.md 实录 + 选择性暂存提交推送。
- [ ] **Step 7: 报告裁决**：48 run 表 + issue #182 清除声明 + checksum 表。

### Task 2: 浅改六文件（chain/special × arm/_svd）

**Files（各一 commit）:** `sve512_f64_chain_arm.cpp`（门控 :166-172、vl_d :176）、`sve512_f64_chain_svd.cpp`（:149/:159）、`sve512_f32_chain_arm.cpp`（:150/:160）、`sve512_f32_chain_svd.cpp`（:129/:138）、`sve512_f64_special_arm.cpp`（:168/:178）、`sve512_f64_special_svd.cpp`（:138/:147）

**Interfaces:**
- Consumes: `arm64_require_sve_vl(test, 128, 1024) -> long`（字节；<0 = EXIT_SKIP(-255) 且已 log_skip——非裸 -1，见 arm64_vl.h 头注（裸 -1 会触发框架 "Operation not permitted" 误报）；检测到 `svl < 0` 直接 `return EXIT_SKIP`）；Task 1 的映射锚。

- [ ] **Step 1: 切分支**：`git checkout -b feat/sve512-allvl origin/feat/arm64-vl-api`（其头 = ec65712b，与 main（505eeaff）两树相同；后续阶段直接自 main 切出）
- [ ] **Step 2: 每文件六段编辑（f64_chain_arm 为范本，其余按行号表套用）**：

① include 块（`#ifdef __aarch64__` 内、arm_sve.h 旁）加：
```cpp
#include <device/cpu/arm64/arm64_vl.h>
```
② 门控替换（f64 族范本，f32 族 `/8`→`/4`、范围 [4,32]）：
```cpp
/* OLD（:164-172 整块）:
 *   本测试族为 512-bit VL 设计并在 HiSilicon 0xd22 上验证;GH 托管 SVE
 *   runner(运行时 VL=128)上实测 golden 失配(issue #182)。
 *   if (svcntb() != 64) { log_skip(... 512-bit VL only ...); return EXIT_SKIP; }
 * NEW: */
/* All-VL Phase 2 P3: the 512-bit single-value gate widens to a
   128..1024-bit range check via the framework layer. The geometry has
   been svcnt-dynamic all along (only this gate pinned one tier); the
   historical VL=128 golden mismatch (issue #182, hosted CI runner) is
   cleared by the four-tier dual-kernel evidence (gem5-fi
   feat/sve512-dual, 48 runs). arm64_require_sve_vl returns BYTES —
   f64 lanes = svl/8 (range-check below, release-effective). */
const long svl = arm64_require_sve_vl(test, 128, 1024);
if (svl < 0)
    return EXIT_SKIP;
```
③ vl_d 赋值 + 范围检查：
```cpp
data->vl_d = (int)(svl / 8);          /* f64: bytes/8; f32: bytes/4 */
if (data->vl_d < 2 || data->vl_d > 16) {   /* f32: [4,32] */
    log_error("%s: f64 lanes %d outside [2,16] — unit drift (svl %ld bytes)",
              test->id, data->vl_d, svl);
    return EXIT_FAILURE;
}
```
④ run 侧漂移守卫（`auto *data = static_cast<...>` 取回后第一件事）：
```cpp
/* All-VL policy #3: fail closed on init/run VL drift. */
if ((int)svcntd() != data->vl_d) {    /* f32 文件用 svcntw() */
    log_error("%s: VL drifted between init (%d lanes) and run (%d lanes)",
              test->id, data->vl_d, (int)svcntd());
    report_fail_msg("%s: SVE VL drifted between init and run", test->id);
}
```
⑤ 文案同步：docblock 的 512-bit 表述（各文件 :8-:21 域）、占位符消息 "SVE-512"、fail 消息 "SVE-512 f64 FMLA chain"、DECLARE 描述、门控上方中文注释块——统一改为 "all-VL (128..1024-bit via arm64_require_sve_vl)" 口径；`CHAIN_STEPS/SPECIAL_CHAIN_STEPS` 是链深语义，注释声明与 VL 无关；⑥ 所删手探块的 log_skip 文案（"requires SVE (e.g. HiSilicon 0xd22, 512-bit VL)"）随块删除一并消除，不留 512-bit 残留。
⑥ 删除 init 首段 getauxval(AT_HWCAP) 手探块（范本 f64_chain_arm :154-162；全族同型块在各文件 init 首段——Task 2 六文件约 :120-:157 域，gather_scatter_svd :103、gather_scatter_arm :134、xlswave :140、libomp :335 同型）：`arm64_require_sve_vl` 已内置能力探测（无 HWCAP_SVE → log_skip + 返回 EXIT_SKIP）与范围检查，手探块整块冗余——spec §12 验收#4 "手写探测样板清零"的 P3 主体（as-built e0eba9cd：10 文件 getauxval 计数全为 0；sve512 家族不属 P4/P5/P6 任何后续阶段，P3 不删则该验收无主体可完成）。

- [ ] **Step 3: 逐文件验证 + commit**：每文件 `./scripts/build.sh`（TU 零告警）→ `-e <test> -t 2000 -n 1 -v`（`result: skip`，require 文案或框架门如实记录——arm64_sve_lib 为 +sve TU，框架编译特性门先触发为预期）→ commit（一文件一 commit，正文引用四档锚）→ 下一文件。六文件完成后 push。

### Task 3: gather_scatter 双文件

**Files:** `sve512_gather_scatter_arm.cpp`（门控 :144）、`sve512_gather_scatter_svd.cpp`（:112）

- [ ] 同 Task 2 六段编辑（两文件几何已全动态：_arm 的 gn=64×vl_d/:156、_svd 的 svwhilelt 尾/:83；含 ⑥ 删 getauxval 手探块——_arm :134、_svd :103）；_arm 顺带更新 :76-78 "512-entry gather table" 注释为档位相关表述（gn 随 lanes）；_svd 的 docblock :23-25 本就记录多 VL 设计理由，保留。各一 commit。

### Task 4: libomp_ctx_arm.cpp（含栈缓冲修复）

**Files:** `libomp_ctx_arm.cpp`（门控 :344、micro :236）

- [ ] Task 2 六段编辑（vl_d :354，f64 映射，含 ⑥ 删 getauxval 手探块 :335；**本文件状态指针是 `ctx->` 而非范本的 `data->`**——:354 `ctx->vl_d`，套用范本③④时同步替换）**加**：
```cpp
/* OLD (:236): double micro[8];
 * NEW: SVE store writes svcntd() doubles; the require gate caps VL at
 * 1024-bit = 16 f64 lanes — the pre-P3 micro[8] was exactly full at
 * 512-bit and overflowed only at 1024-bit (16 into 8); [16] covers
 * the whole gated range. */
double micro[16];
```
- [ ] 一 commit（正文记录栈溢出风险的消除）。

### Task 5: xlswave_ctx_arm.cpp Phase A 深重构

**Files:** `sve512_xlswave_ctx_arm.cpp`（B_TILE :60、axis kernel :86-110、src 尺寸 :162、result 存储 :253-254、闭式 golden :256、门控 :149、s_base :165）

- [ ] **以 `sve512_stencil_axis_arm.cpp`（同 Phase A 几何的已验证先例，其注释 :99-105 记录四档实证）为模板**：
  1. 门控/vl_d/漂移守卫/include/删 getauxval 手探块（:140）同 Task 2（f64 映射）
  2. `constexpr size_t B_TILE = 16` → 删除；运行时 `bt = 2 * lanes`（lanes=svl/8，bt∈[4,32]，kTileMax=32 上限）
  3. `axis_generic_16x2` 内 `n + 8` → `n + lanes`、`off = r*stride*B_TILE` → `r*stride*bt`（函数签名加 lanes/bt 参数）
  4. `src.resize(extent * stride * B_TILE)` → `* bt`；tile 填充值模式随 bt（镜像 stencil_axis 的 `(i % bt + 1)` 模式）
  5. `result[B_TILE]` → `result[32]`（上限）；`svst1(result)`/`svst1(result + 8)` → `result + lanes`（第二向量在 tile 内的偏移）
  6. 闭式 `expected = 96.0*(jj+1)` → **改为标量 golden**（镜像 stencil_axis 的做法）：按 bt/coeff/坐标以同序标量重算 expected 数组（as-built 在 run 期逐次重算）。**理由勘正（as-built e0eba9cd 实证）**：闭式在各档均与标量参考相等——它绑定的是 `i%bt+1` 填充模式而非 512-bit 对齐；替换为标量 golden 仍正确（抗填充模式变化），但"闭式与 tile 值绑定、bt 化后不再可靠"的原陈述失实，勿沿用
  7. Phase B 与 canary 区不动（已 VL 无关）；docblock 四档引用补 gem5-fi feat/sve512-dual 的 stencil 16 run
  8. `data->s_base = data->src.data() + coord * stride * B_TILE` → `* data->bt`（:165——原七处清单遗漏的第 8 处，B_TILE 引用仅靠"删除 constexpr 后编译报错"兜底发现，此处显式列入）
- [ ] 验证 + 一 commit。**本文件是 P3 唯一"几何重构"。验证边界声明（显式）**：Task 1 Step 4 的 stencil_axis_dual 现成 16 run 是**kernel 族级锚**——覆盖轴 kernel 几何本身（bt 参数化、`n+lanes`、`result+lanes`、slab 步距随档缩放）；xlswave 特有转录层（标量 golden 数组、result[32] 上限存储、`i%bt+1` 填充模式、s_base 偏移）**不在 kernel 覆盖内**——由构建零告警 + 评审 + 512-bit 档语义同一性（bt=16 ≡ 旧 B_TILE=16）+ 本机无 SVE 的 SKIP 边界保障，如实记录、不冒称四档已验。

### Task 6: 文档收尾 + PR

- [ ] progress-log P3 条目（真实 run 数据）；`tests/cpu/arm64/meson.build` 的 sve512 相关注释同步（若含 512-only 表述）；README 的 sve512 家族表述核对（grep 'sve512\|512-bit'）
- [ ] 全量验证矩阵（构建零告警 / 10 测试 `-e` 全部干净 skip / zstd19 `-t 2000 -n 1` → pass / **mce_check**：`-e mce_check -t 2000 -n 1` → pass——CLAUDE.md 规则 6，真实 EDAC 后端测试必须 pass，不得以 SKIP 代替）
- [ ] push + PR 正文（48 run 表 + issue #182 清除声明 + 聚类 rationale + 堆叠关系）

---

## Self-Review 记录

- **Spec 覆盖**：§6 P3 全四要素（范围检查✓Task2-5/geometric svcnt 化✓Task5/孪生对逐文件✓/dual kernel≈5-6→聚类为 3（A 单 kernel **四模式**：chain_f64 / chain_f64_slab 独立 / chain_f32 / special_f64，as-built + B 两模式 + C 复用现成））；**Ruling（检视后改写）：聚类验证的 scoping 及其边界**——同 kernel 形态 = 同验证目标，但**未覆盖组合显式点名**：`sve512_f64_special_svd`（特殊值×块 slab）与 `sve512_f32_chain_svd`（f32×块 slab）的组合寻址无 kernel 模式直接覆盖（chain_f64_slab 用有限带操作数），其正确性由"两维分别经四档实证"的推理支撑——**残留风险与触发条件**：真 SVE 主机非 512 档首现失败时，首要怀疑上述两份 _svd 文件的组合寻址；§7 协议 48 run（C 复用 16 run 重验，含 512-bit 回归锚——7bed7f0e 的 512-bit 档四态 checksum 与旧版逐位一致；chain/gather 新 kernel 无旧版可锚，按 P2（PR #279）"无回归锚（诚实声明）"先例声明 N/A）；§8 golden 表内三行（标量同序/同内核/闭式→标量）；§9 SKIP 语义（require 内置）。
- **占位符**：Task 2 的六段编辑以真实范本代码给出，其余文件按行号表套用（同构机械变换，探索已逐文件核验 512-ism 全为文案）；**三处套用偏差显式标注**（均编译期报错、非静默错误，但与"逐文件核验"的精度声明一并如实记录）：f32 族变量是 `vl_w`/`svcntw()` 而非范本③④通篇的 `vl_d`/`svcntd()`（f32_chain_arm :160 `data->vl_w = svcntw();`）、libomp 状态指针是 `ctx->` 而非 `data->`（:354）、xlswave 的 `s_base` 计算含 `B_TILE` 引用（:165，Task 5 第 8 处显式列入）。Task 5 以 in-repo 先例文件为模板并列八处具体改动。无 TBD。
- **类型一致性**：`arm64_require_sve_vl` 签名与 PR #277 一致；lanes 单位注释处处标注（P2 教训）。
- **Review Focus 归属**：1→Task 1 Step 6；2→各 Task 范围检查；3→Task 5.5；4→Task 4；5→Task 1 Step 2 chain_f64_slab 模式；6→Task 2⑤⑥/Task 6。
