# SVE/SME 全 VL 二期 P4（ipsec HMAC×17 流数参数化）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 17 个 HMAC 测试从固定 8/4 流 + VL≥256 门控改为按运行时 VL 参数化流数（VL=128 可跑 4/2 流，512/1024 升 16/8、32/16 流），共享内核/HMAC 头参数化，gem5-fi SHA dual kernel（单文件三族）48 族-run 四档实证先行。

**Architecture:** 双仓流水：gem5-fi SHA dual kernel（单文件内 sha1/256/512 三族，参数化流数 + 标量参考）先行 → 共享头参数化（`sve_sha_kernels.h` 3 内核 + `sve_hmac.h` 5 包装）→ 17 测试模板批改（P3 批量代理先例）→ verify.cpp 同步 → 文档。

**Tech Stack:** SVE intrinsics（sha 内核本就 lane 无关——谓词自动缩放，参数化只动 C 层数组维度与循环边界）、arm64_vl.h、gem5-fi。

**Spec:** `docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md` §6 P4 / §7 / §8 / §9。

## Global Constraints

- **单位军规**：`arm64_require_sve_vl` 返回字节；32-bit 族流数 = svl/4（∈[4,32]），64-bit 族 = svl/8（∈[2,16]）；每处换算带单位注释 + release 生效范围检查。
- **两个既有缺陷必须随本阶段修复**（评估代理实证）：
  1. **门控无上界（8.2）**：`svcntw()<8 || svcntd()<4` 只查下界——VL≥512 硬件上 `svst1(ptrue, W[t])` 会向 8 宽行写 16/32 lane（**越界栈写 + 串行污染 W[t+1]**）。全 VL 化的数组定维（STREAMS_MAX）+ require 范围门同时消除。
  2. **data_size knob vs 2048B 栈缓冲（8.4）**：knob 允许 [1024, 16MB] 而 `full[N][2048]` 每流仅 2048B——`data_size > ~1975`（32-bit 族）/`~1903`（64-bit 族）即溢出。修法：hmac 头内加容量断言（fail-loudly）+ 测试 knob 上限钳到容量内 + docblock 说明。
- 分支：`feat/ipsec-allvl` 堆叠于 P3 完成后的 `feat/sve512-allvl`（两阶段同栈依次合并）；gem5-fi `feat/sha-dual` 堆叠于 `feat/sve512-dual`。
- 验证 subagent 隔离；一单元一 commit（机械适配可作同一单元——AGENTS.md 规则 4"一个补丁一个单元"的单元口径）；DCO；x86 零改动；本机只验构建+skip+回归。

## 探索结论基线（2026-10-24 评估代理）

- 内核 SVE 向量代码 **lane 数无关**（谓词自动缩放、零跨 lane 指令、消息调度按列独立——W 表 `[轮][流]` 行主序，lane=流）；流数硬编码全在 **C 层**：内核签名 3 处 + HMAC 包装 5 处 + 测试模板 17 份。
- 3 类内核模式：SHA-1（`sha1_compress_x8` :262，`W[80][8]` u32）/ SHA-256 族（`sha256_compress_xN` :59，`W[64][8]`；sha224=换 IV+截 7 字）/ SHA-512 族（`sha512_compress_x4` :177，`W[80][4]` u64；sha384=换 IV+截 6 字）。**3 个 dual kernel 覆盖 17 测试全部 SHA 计算**。
- golden 在同一内核的初始化阶段预计算（自洽），流数参数化后对应关系自动保持；lane 0 恒为原始密文不变式不受影响。
- `scripts/sve-sha-verify/verify.cpp` 直 include 内核头，5200 向量对 OpenSSL——其 8/4 流比较循环需同步参数化。
- 陈旧注释：`sve_sha_kernels.h:259-261`（sha1 段写"4 条 64-bit"复制残留）、17 个测试 docblock "Eight lanes"（4 流文件实为 4 lane）。

## Review Focus

1. **流数-数组维度失配**（8.2 同类）：参数化后任何 `svst1` 行宽 < 激活 lane 数即越界——STREAMS_MAX 定维 + require 范围 + 显式检查三重防。
2. **W 表跨流污染**：参数化后 W 行宽必须 ≥ svcntw()/svcntd()——kernel 四档通道 A 是判据。
3. **HMAC 容量溢出**（8.4）：data_size 上限与 full[] 容量的闭环。
4. **大端计数器/padding**：与流数无关（评估实证），但参数化重构不得触碰——kernel 含多流 HMAC 路径覆盖。
5. **verify.cpp 漂移**：验证器与内核头同步参数化，否则 5200 向量失效。
6. **golden 流对应**：init/run 同参数化函数下第 s 行即第 s 流——kernel 通道 A 逐流比对。

---

### Task 1: gem5-fi SHA dual kernel（单文件三族）+ 48 族-run

**Files（gem5-fi）:** `workloads/sdcshield/sve_sha_dual.c`（**as-built 单文件**：一次调用内依次跑 SHA-1/256/512 三族——原计划为三个文件 `sve_sha1_dual.c`/`sve_sha256_dual.c`/`sve_sha512_dual.c`，实施裁定合并，见 Erratum ②；gcc `-march=armv9-a+sve`）

- [ ] 每个 kernel 结构（同源镜像 `sve_sha_kernels.h` 的参数化形态）：
  - `svl_bytes`（rdsvl 或 svcntb，均返回**字节数**——原计划误写"svcntb×8"，系把字节当位的单位错误，见 Erratum ①；`arm64_require_sve_vl` 参数为 bit、成功返回值为 byte——调用方直接用返回值算流数）→ 流数 n（sha1/256: svl/4∈[4,32]；sha512: svl/8∈[2,16]），越界 exit 3
  - **标量参考**：FIPS 标准标量 SHA-1/256/512 实现（K 常量表从 sdcshield `sve_sha_kernels.h` 同源复制，禁手抄错）；对 n 条消息各算一遍摘要
  - **向量路径**：镜像内核的 W 表 `[ROUNDS][STREAMS_MAX]` + `svptrue` + `svld1/svst1` 逐轮往返 + 轮函数（sha1 的 ROTL1、sha256/512 的 σ/Σ 三异或移位组合）——从内核头逐字移植（同源）
  - 消息：n 条、每条 2 块（覆盖 nblocks>1 路径）+ 大端长度域；fixed 模式确定性内容、random 模式 xs32
  - 通道 A：逐流逐字节摘要比对；通道 B：FNV-1a over 全部摘要位
  - 输出 `mode=sha1 streams=N FAILS=n` 等行
- [ ] Makefile gcc 目标（as-built 单文件单目标——原计划 +3 目标随三文件合并为 1；选择性暂存纪律）
- [ ] 验证代理（as-built 矩阵）：单 kernel 二进制 × 4 档 × {fixed seed=1, random seed=1..3} = **16 次调用 × 3 族 = 48 族-run**（原计划"3 kernel × 4 档 = 48 run"，单文件合并后一次调用跑三族，见 Erratum ②）；**每次记录字段**：tier/VL 档位、args（mode+seed）、n32/n64 流数、逐族 FAILS、`SDCSHIELD_FAILS=0`、16-hex checksum、exit；判据：流数逐档 4/8/16/32（32-bit 族）与 2/4/8/16（64-bit 族）、逐族 FAILS=0、random 各 seed 的 checksum 互异（seed 确实生效）；128-bit 档（4/2 流）全绿 = "VL=128 可跑"命题实证；**同时**跑一条 HMAC 路径（sha256 kernel 内嵌 ipad/opad 两块外层——覆盖 hmac 层的块组装/大端计数）
- [ ] 复跑要求（Erratum 待办，见文末 Erratum ③）：① HMAC 独立参考扩到 64-bit 族——原 HMAC 路径仅 sha256 内嵌（32-bit 族），而 64-bit 族恰是差异最大处（128B 块/17B 长度域）；② 容量边界 run：msg_len 1975/1976（32-bit 族）、1903/1904（64-bit 族）——断言边界内返回 0、超界**在 memcpy 前返回 -1** 且不改写输出缓冲区；③ 复跑须直 include 参数化后的真头（`verify.cpp:11` 直 include `sve_sha_kernels.h` 先例 + gem5-fi `workloads/sdcshield/` 同步为子模块/同源文件），替代手工镜像移植——as-built 首轮 16/16 FAIL（kernel 标量 golden 两缺陷：`&sw[0][s]` 布局误读 + mini-HMAC 连续读越界）已实证镜像法的脆弱性

### Task 2: `sve_sha_kernels.h` 参数化（与 Task 3 头改动同一 commit，见 Task 3 裁定）

- [ ] 3 个内核函数（as-built 87a81939）：数组定维 `[8]/[4]` → `SVE_SHA_STREAMS_MAX_32=32`/`SVE_SHA_STREAMS_MAX_64=16`（1024-bit 上限；原计划误写宏名 `STREAMS_MAX_F32/F64`，见 Erratum ②），`W[64][SVE_SHA_STREAMS_MAX_32]`、`W[80][SVE_SHA_STREAMS_MAX_64]` 等；**流数不做函数参数——函数内部 `n = svcntw()/svcntd()` 推导**（n≡lane 数是自然律：向量通路按 VL 动作，n 与 lane 数不存在失配自由度，调用点最小化；原计划的 `int nstreams` 参数形态被实施否决，见 Erratum ②）；标量填表 `s<n`；**向量代码零改动**（谓词/加载存储本就 lane 无关——评估实证）
- [ ] **内存安全三层防线**（替换原"前置条件注释改为 nstreams must equal svcntw()/svcntd()"方案）：① `SVE_SHA_STREAMS_MAX_32/64` 定维（W 行宽覆盖 1024-bit 档）；② 调用方 `arm64_require_sve_vl(test, 128, 1024)` 范围门 + `nstreams∈[4,32]/[2,16]` 显式检查；③ 内核内部 `n≡svcntw()/svcntd()` 推导（无失配自由度）。**勘误**：原计划"输出 svst1 的 pg 自动只写激活 lane"是错误安全断言——pg 是 `svptrue_b32()/svptrue_b64()`，写满**全部** VL lane（正是 8.2 溢出机理，与 Global Constraints 自相矛盾），已删除；前置条件注释按 as-built 改为 "streams = svcntw()/svcntd() at the caller's runtime VL（调用方 128..1024-bit 门控）"。**已知边界**：内核头自身无 assert——直 include 者需自守（verify.cpp 以 exit-3 VL 范围门自守）
- [ ] :259-261 陈旧注释修正（sha1 段的"4 条 64-bit"残留）
- [ ] 验证：主仓构建零告警。**预期编译断裂（计划内中间态）**：Task 2 改内核签名/定维后至 Task 5 前，`verify.cpp` 以旧签名调用 5 处压缩函数（`sha256_compress_x8(st, mp, …)` 等）且 `st[8][8]` 等为固定定维，与新头不兼容、**必然编译失败**——verify.cpp 的编译验证归属 Task 5，Task 2/3/4 不对其做编译检查（原计划"暂时性编译检查（其行为可能变）"表述不准确，编译断裂是确定性的）

### Task 3: `sve_hmac.h` 参数化 + 容量守卫（头改动 + 17 调用点适配一 commit，as-built 87a81939）

- [ ] 5 个 hmac 函数（as-built 87a81939，与内核头同形态）：`[8]/[4]` 维度 → `SVE_SHA_STREAMS_MAX_32/64` 定维；签名 void → **返回 int（0/-1）**，同样**不做 nstreams 参数**——函数内部 `n = svcntw()/svcntd()` 推导（调用内核无需传流数）；`s<n` 循环
- [ ] **容量守卫（8.4 修复，as-built 形态）**：① msg 侧——`padded = ((body+9+63)/64)*64`（32-bit 族）`/((body+17+127)/128)*128`（64-bit 族）计算后、`memcpy` 进 `full[][2048]` 前 `if (padded > 2048) return -1;`（等价容量边界 msg_len 1975/1903，见 Task 4）；② key 侧（as-built 5e344c86 评审轮补入）——`if (key_len > 64/128) return -1;`（ipad/opad XOR 循环越界防护，与 msg 守卫对称）；17 个测试的调用点检查 `!=0` → log_error + report_fail_msg（调用点随 Task 3 头+适配 commit 一并改）
- [ ] **commit 边界裁定（最终）**：头部参数化（Task 2 内核头 + Task 3 HMAC 头）+ 17 个测试的最小调用点适配合并为**一个 commit**——依据 AGENTS.md 规则 4"一个补丁一个单元"的**单元**口径：头改签名而 17 文件仍用旧签名，则中间 commit 不可构建，故消费者适合同单元落地；as-built 87a81939 实证（头 2 文件 + 17 测试，19 files 一 commit）：适配内容 = hmac 返回值检查（golden init/frozen replay/re-roll 三处同式 log_error + report_fail_msg）+ `compute_golden` 加 `test` 形参，`out[][20]` 等按摘要长度定维、经 C 数组形参退化与测试旧 `NSTREAM=8` 定维兼容——门控/nstreams 填充/数组重定维/knob 钳制随后续 Task 4 逐文件 commit。验证：构建零告警 + skip 干净 + zstd19 pass。（原文本三轮方案推导已清理，见 Erratum ②）
- [ ] **中间态边界注记（诚实入档，不改写历史）**：上述头+调用点 commit 落地时 17 测试的门控/数组定维仍为旧形态（Task 4 才改）——在 VL≥512 硬件上该中间态 commit **不安全-before-and-after**：8.2 缺陷（旧门控 `svcntw()<8||svcntd()<4` 只查下界，放行 VL≥512）在改动前同样存在，本 commit 只改变故障模式（越界读指针数组 vs 越界写 W 行），未扩大暴露面；本机验证边界 = 构建 + skip + zstd19（无 SVE 硬件，运行时越界不可见），VL≥512 档安全性由**终态** gem5 四档 16-run 实证收口（512/1024 档 W 定宽 32/16 无越界）

### Task 4: 17 测试批改（批量代理，P3 先例）

- [ ] 每文件五段式（P3 范本同构）：include 换 `arm64_vl.h`；双门控 → `require(128,1024)` + `nstreams = svl/4`（或 /8）+ 显式范围检查；`NSTREAM` 宏与 `golden_mac[NSTREAM][…]` → 运行时 nstreams（数组按 STREAMS_MAX 定维或 vector）；run 侧漂移守卫；docblock "Eight lanes"→"N = svcntw()/svcntd() lanes (all-VL)" + meson :296-306 注释同步
- [ ] data_size knob 上限——**三通道全钳**（as-built，8.4 闭环；原计划只钳 `-O` 旋钮一条路径，环境变量/自适应默认两通道仍 16MB 会触发容量守卫致 17 测试默认全红，local-claude Critical 指出）：① `-O` 旋钮 `sdc_knob_int_clamped(test,"data_size",0,1024,上限)`；② 环境变量 `sdc_env_knob_int_clamped(<test>, "data_size", &v, 1024, 上限)`；③ 自适应默认 `sdc_auto_linear_count(…, 1024, 上限, …)`——三条取值路径**全部**钳到上限 1900（32-bit 族）/1830（64-bit 族）+ hmac 返回值检查（调用点 `!=0` → log_error + report_fail_msg）。容量边界 = 2048−64−9=1975（32-bit 族）/2048−128−17=1903（64-bit 族，padded 恰好 2048 的 msg_len 上界）；钳制值 1900/1830 在边界内、留 75/73 字节余量
- [ ] 每文件构建 + skip + commit

### Task 5: `scripts/sve-sha-verify/verify.cpp` 同步

- [ ] 范围（as-built 扩写——原仅"比较循环与 `msgs[8]`"）：**五段全部按流分配的数组**（SHA-256/SHA-224/SHA-512/SHA-384/SHA-1）——`msgs`/`mp`/`padded`/`st`（含 SHA-224、SHA-512 段的 `msgs[8][256]`、`msgs[4][1024]`、`st[8][4]` 等）首维按 `SVE_SHA_STREAMS_MAX_32/64` 定维（固定 `[8]/[4]` 首维在 VL≥512 档流数超维即越界读写）；初始化/哈希/比较循环只处理前 `n = svcntw()/svcntd()` 条流；入口加 **exit-3 VL 范围门**（`svcntw()∈[4,SVE_SHA_STREAMS_MAX_32]` 且 `svcntd()∈[2,SVE_SHA_STREAMS_MAX_64]`，镜像 gem5 内核守卫——as-built 5e344c86 评审轮补入，防超 1024-bit VL 写溢定宽数组）；构建验证（本机无 SVE 只能编译——**诚实边界：5200 向量的 OpenSSL 重验需 SVE 硬件，记录为待办**，见 Erratum ③）
- [ ] 一 commit

### Task 6: 文档收尾 + PR

- [ ] README:409 的 VL≥256 政策段改写（全 VL 口径）；progress-log P4 条目（48 族-run 实录，字段同 Task 1：tier/VL、args、n32/n64 流数、逐族 FAILS、`SDCSHIELD_FAILS=0`、checksum、exit + 8.2/8.4 修复记载）；PR 正文（聚类 rationale + 三重防线）

---

## Erratum（2026-10-24，PR #282 五 bot 检视闭环）

1. **`svcntb×8` 单位错误（Critical）**：Task 1 原模板 `svl_bytes`（rdsvl 或 svcntb×8）把字节当位——`svcntb()`/`rdsvl` 本就返回字节数，×8 后 `svl/4` 在 128-bit 档会得 32 流（正确为 4），全档差 8×。与 P2 计划的 `/32` 单位错误（PR #279，d5fb43c1 Erratum ①）同类，根因同样是**计划模板单位自相矛盾**（Global Constraints 写"返回字节"、代码模板按位换算）；实现侧 `svcntw()/svcntd()` 直取 lane 数未犯此错。本节已修正并补 `arm64_require_sve_vl` 参数 bit/返回 byte 的契约说明。
2. **as-built 四项偏离**（计划形态 vs 实现分支 `feat/ipsec-allvl`/PR #284 @ 5e344c86，正文已按 as-built 回写）：① 内核/HMAC **流数不做函数参数**（原计划 `int nstreams` 参数）——函数内部 `n = svcntw()/svcntd()` 推导，消除 n 与 lane 数失配的自由度；② 宏名 `SVE_SHA_STREAMS_MAX_32/64`（原计划 `STREAMS_MAX_F32/F64`）；③ gem5 kernel **单文件** `sve_sha_dual.c` 一次调用三族（原计划三文件），验证矩阵随之 16 次调用 × 3 族 = 48 族-run；④ data_size knob **三通道全钳** 1900/1830（原计划只钳 `-O` 旋钮，环境变量/自适应默认两通道仍 16MB——local-claude Critical：默认配置下 17 测试必触发容量守卫全红，且本机 build+skip 与 gem5 镜像 kernel 验证均不可见）。另：Task 3 原三轮"不行？/折中"裁定推导已清理为最终裁定。
3. **验证边界诚实记载**：① HMAC **容量守卫未做执行验证**——1975/1976、1903/1904 边界 run 是复跑待办（Task 1 复跑要求②）；② HMAC 独立参考仅覆盖 32-bit 族（sha256 内嵌路径），**64-bit 族（128B 块/17B 长度域）无独立参考**——复跑待办①；③ gem5 首轮实证走**手工镜像移植**而非直 include 真头（首轮 16/16 FAIL 正是镜像法脆弱性的实证），复跑须直 include 参数化真头（Task 1 复跑要求③）；④ verify.cpp 5200 向量 OpenSSL 重验需 SVE 硬件——as-built 后续已在 SVE 硬件完成 39000 向量四档（128..1024-bit）交叉验证全绿（5e344c86 评审轮），此待办已收口；⑤ Task 3 头+调用点中间态 commit 在 VL≥512 档不安全-before-and-after（8.2 缺陷同在、故障模式改变），本机验证边界=构建+skip+zstd19，VL≥512 安全性由终态四档实证收口——诚实入档不改写历史。
4. **评审出处**：PR #282 五 bot 检视——coderabbit 7 条 actionable + 2 条 nitpick（单位错误/判据字段/前置条件强制/knob 边界/commit 边界/verify.cpp 数组/中文术语+MD052）、local-claude 6 条（knob 三通道 Critical/中间态 VL≥512/`pg 只写激活 lane` 错误断言/镜像验证 vs 真头/裁定推导清理/verify.cpp 编译断裂）、codex/greptile/pr-agent 配额类无独立发现。处置明细见 PR 描述（采纳 12 + 变通 3）。
5. **证据坐标**：gem5-fi 远端 `feat/sha-dual @ 955f766d`（workloads/sdcshield/）`findings.md` 两轮实录——首轮 16/16 FAIL（kernel 标量 golden 两缺陷：`&sw[0][s]` 把 `[word][stream]` 布局当连续状态、mini-HMAC 两独立块被 xN 连续读越界，验证代理依规止步未提交）→ 修复后同矩阵 16/16 全绿（`SDCSHIELD_FAILS=0`、逐档流数 4/8/16/32 与 2/4/8/16）；sdcshield 侧 as-built 见 `feat/ipsec-allvl`（87a81939 头+17 文件参数化、PR #284 @ 5e344c86 评审修复轮）。

## Self-Review 记录

- **Spec 覆盖**：§6 P4 全要素（流数参数化 ✓Task2-4/基于现成内核头 ✓/≈3 dual kernel ✓Task1——as-built 单文件三族）；§7 48 族-run（16 次调用 × 3 族，记录字段齐备）；§8 golden 同内核自洽（评估实证机制自动保持）+ kernel 通道 A 逐流比对；§9 require 内置。
- **Task 3/4 的 commit 边界裁定**（头参数化与 17 调用点的原子性）：头+机械适配一 commit（AGENTS.md 规则 4 单元口径：同一编译单元闭环，as-built 87a81939 实证），门控/knob/文案为独立 commit——防中间态构建断裂；中间态 VL≥512 边界诚实注记（不安全-before-and-after，终态四档实证收口）。
- **占位符**：无 TBD；kernel 的标量参考明确 K 常量同源复制纪律；复跑三项（HMAC 64-bit 独立参考/容量边界 run/直 include 真头）以 Erratum 待办显式入档，非隐式 TBD。
- **Review Focus 归属**：1/2→Task 1 四档 + Task 2 三层防线（STREAMS_MAX 定维/require+显式范围/内部推导）；3→Task 3 守卫 + Task 4 三通道钳制；4→Task 1 HMAC 路径（64-bit 族独立参考为复跑待办）；5→Task 5（五段定维+exit-3 门，编译断裂归属已明确）；6→Task 1 通道 A。
