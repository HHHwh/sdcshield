# SVE/SME 全 VL 二期 P4（ipsec HMAC×17 流数参数化）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 17 个 HMAC 测试从固定 8/4 流 + VL≥256 门控改为按运行时 VL 参数化流数（VL=128 可跑 4/2 流，512/1024 升 16/8、32/16 流），共享内核/HMAC 头参数化，gem5-fi 3 个 SHA dual kernel 48-run 四档实证先行。

**Architecture:** 双仓流水：gem5-fi 3 kernel（sha1/256/512 族，参数化流数 + 标量参考）先行 → 共享头参数化（`sve_sha_kernels.h` 3 内核 + `sve_hmac.h` 5 包装）→ 17 测试模板批改（P3 批量代理先例）→ verify.cpp 同步 → 文档。

**Tech Stack:** SVE intrinsics（sha 内核本就 lane 无关——谓词自动缩放，参数化只动 C 层数组维度与循环边界）、arm64_vl.h、gem5-fi。

**Spec:** `docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md` §6 P4 / §7 / §8 / §9。

## Global Constraints

- **单位军规**：`arm64_require_sve_vl` 返回字节；32-bit 族流数 = svl/4（∈[4,32]），64-bit 族 = svl/8（∈[2,16]）；每处换算带单位注释 + release 生效范围检查。
- **两个既有缺陷必须随本阶段修复**（评估代理实证）：
  1. **门控无上界（8.2）**：`svcntw()<8 || svcntd()<4` 只查下界——VL≥512 硬件上 `svst1(ptrue, W[t])` 会向 8 宽行写 16/32 lane（**越界栈写 + 串行污染 W[t+1]**）。全 VL 化的数组定维（STREAMS_MAX）+ require 范围门同时消除。
  2. **data_size knob vs 2048B 栈缓冲（8.4）**：knob 允许 [1024, 16MB] 而 `full[N][2048]` 每流仅 2048B——`data_size > ~1975`（32-bit 族）/`~1903`（64-bit 族）即溢出。修法：hmac 头内加容量断言（fail-loudly）+ 测试 knob 上限钳到容量内 + docblock 说明。
- 分支：`feat/ipsec-allvl` 堆叠于 P3 完成后的 `feat/sve512-allvl`（两阶段同栈依次合并）；gem5-fi `feat/sha-dual` 堆叠于 `feat/sve512-dual`。
- 验证 subagent 隔离；一文件一 commit；DCO；x86 零改动；本机只验构建+skip+回归。

## 探索结论基线（2026-10-24 评估代理）

- 内核 SVE 向量代码 **lane 数无关**（谓词自动缩放、零跨 lane 指令、消息调度按列独立——W 表 `[轮][流]` 行主序，lane=流）；流数硬编码全在 **C 层**：内核签名 3 处 + HMAC 包装 5 处 + 测试模板 17 份。
- 3 类内核模式：SHA-1（`sha1_compress_x8` :262，W[80][8] u32）/ SHA-256 族（`sha256_compress_xN` :59，W[64][8]；sha224=换 IV+截 7 字）/ SHA-512 族（`sha512_compress_x4` :177，W[80][4] u64；sha384=换 IV+截 6 字）。**3 个 dual kernel 覆盖 17 测试全部 SHA 计算**。
- golden 是同内核 init 期预计算（自洽），流数参数化后对应关系自动保持；lane 0 恒为原始密文不变式不受影响。
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

### Task 1: gem5-fi 3 个 SHA dual kernel + 48 run

**Files（gem5-fi）:** `workloads/sdcshield/sve_sha1_dual.c`、`sve_sha256_dual.c`、`sve_sha512_dual.c`（gcc `-march=armv9-a+sve`）

- [ ] 每个 kernel 结构（同源镜像 `sve_sha_kernels.h` 的参数化形态）：
  - `svl_bytes`（rdsvl 或 svcntb×8）→ 流数 n（sha1/256: svl/4∈[4,32]；sha512: svl/8∈[2,16]），越界 exit 3
  - **标量参考**：FIPS 标准标量 SHA-1/256/512 实现（K 常量表从 sdcshield `sve_sha_kernels.h` 同源复制，禁手抄错）；对 n 条消息各算一遍摘要
  - **向量路径**：镜像内核的 W 表 `[ROUNDS][STREAMS_MAX]` + `svptrue` + `svld1/svst1` 逐轮往返 + 轮函数（sha1 的 ROTL1、sha256/512 的 σ/Σ 三异或移位组合）——从内核头逐字移植（同源）
  - 消息：n 条、每条 2 块（覆盖 nblocks>1 路径）+ 大端长度域；fixed 模式确定性内容、random 模式 xs32
  - 通道 A：逐流逐字节摘要比对；通道 B：FNV-1a over 全部摘要位
  - 输出 `mode=sha1 streams=N FAILS=n` 等行
- [ ] Makefile +3 gcc 目标（选择性暂存纪律）
- [ ] 验证代理：3 kernel × 4 档 × {fixed 1, random 1..3} = 48 run；判据：流数逐档 4/8/16/32 与 2/4/8/16、FAILS=0、checksum 表；128-bit 档（4/2 流）全绿 = "VL=128 可跑"命题实证；**同时**跑一条 HMAC 路径（sha256 kernel 内嵌 ipad/opad 两块外层——覆盖 hmac 层的块组装/大端计数）

### Task 2: `sve_sha_kernels.h` 参数化（一 commit）

- [ ] 3 个内核函数：签名 `[N]` 维度 → 运行时 `int nstreams` 参数 + `STREAMS_MAX_F32=32`/`STREAMS_MAX_F64=16` 定维（`W[80][STREAMS_MAX_F32]` 等）；标量填表 `s<nstreams`；**向量代码零改动**（谓词/加载存储本就 lane 无关——评估实证）；输出 svst1 的 pg 自动只写激活 lane
- [ ] 前置条件注释（:56-58/:175-176 "要求 svcntw()>=8"）改为 "nstreams must equal svcntw()/svcntd() at the caller"（**调谐约定**：调用方传 lanes，不是自由 N——向量通路按 VL 动作）
- [ ] :259-261 陈旧注释修正（sha1 段的"4 条 64-bit"残留）
- [ ] 验证：构建零告警 + verify.cpp 暂时性编译检查（Task 5 前其行为可能变——见 Task 5）

### Task 3: `sve_hmac.h` 参数化 + 容量守卫（一 commit）

- [ ] 5 个 hmac 函数：`[8]/[4]` 维度 → `nstreams` 参数 + STREAMS_MAX 定维；`s<nstreams` 循环；调用内核传 nstreams
- [ ] **容量守卫（8.4 修复）**：每函数入口 `if (msg_len > capacity) { /* fail-loudly: log_error + 返回错误码或 abort */ }`——签名加返回值或保留 void+abort？**裁定：改为返回 int（0/-1），17 个测试的调用点检查**（调用点在 Task 4 一并改）；容量 = 2048−64−9（32-bit）/2048−128−17（64-bit）与现有注释一致
- [ ] 验证：构建 + zstd19（此时 17 测试还没改——**中间态必须可构建**：保持旧签名兼容或同 commit 内完成 Task 4 的调用点——**裁定：Task 3+4 顺序不可倒，Task 3 先行改头并保留旧签名包装（deprecated 内联转发）或 Task 3/4 合并为一个 PR 内两个 commit 依次落地、构建在 Task 4 后全绿**——采用后者：Task 3 commit 后允许 17 测试暂用旧签名编译失败？不行。**最终裁定：Task 3 的头参数化直接同步改 17 个测试的调用点属于同一单元的机械部分，但一文件一 commit 军规……折中：Task 3 头部改动 + 最小调用点适配（传 svcntw()）合并为一个 commit（头+17 文件的机械适配），流数门控与 knob 修复是 Task 4 的独立 commit**）

### Task 4: 17 测试批改（批量代理，P3 先例）

- [ ] 每文件五段式（P3 范本同构）：include 换 `arm64_vl.h`；双门控 → `require(128,1024)` + `nstreams = svl/4`（或 /8）+ 显式范围检查；`NSTREAM` 宏与 `golden_mac[NSTREAM][…]` → 运行时 nstreams（数组按 STREAMS_MAX 定维或 vector）；run 侧漂移守卫；docblock "Eight lanes"→"N = svcntw()/svcntd() lanes (all-VL)" + meson :296-306 注释同步
- [ ] data_size knob 上限：`sdc_knob_int_clamped(test,"data_size",0,1024,容量)`（8.4 闭环）+ hmac 返回值检查
- [ ] 每文件构建 + skip + commit

### Task 5: `scripts/sve-sha-verify/verify.cpp` 同步

- [ ] 比较循环与 `msgs[8]` → `svcntw()/svcntd()` 运行时流数；构建验证（本机无 SVE 只能编译——**诚实边界：5200 向量的 OpenSSL 重验需 SVE 硬件，记录为待办**）
- [ ] 一 commit

### Task 6: 文档收尾 + PR

- [ ] README:409 的 VL≥256 政策段改写（全 VL 口径）；progress-log P4 条目（48 run 实录 + 8.2/8.4 修复记载）；PR 正文（聚类 rationale + 三重防线）

---

## Self-Review 记录

- **Spec 覆盖**：§6 P4 全要素（流数参数化 ✓Task2-4/基于现成内核头 ✓/≈3 dual kernel ✓Task1）；§7 48 run；§8 golden 同内核自洽（评估实证机制自动保持）+ kernel 通道 A 逐流比对；§9 require 内置。
- **Task 3/4 的 commit 边界裁定**（头参数化与 17 调用点的原子性）：头+机械适配一 commit（同一编译单元闭环），门控/knob/文案为独立 commit——防中间态构建断裂。
- **占位符**：无 TBD；kernel 的标量参考明确 K 常量同源复制纪律。
- **Review Focus 归属**：1/2→Task 1 四档 + Task 2 定维；3→Task 3 守卫 + Task 4 knob；4→Task 1 HMAC 路径；5→Task 5；6→Task 1 通道 A。
