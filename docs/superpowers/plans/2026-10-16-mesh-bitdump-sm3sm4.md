# mesh 位级取证增强 + sm3sm4 大 data_size 缺陷排查 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans（本会话原生执行——用户已批准三项下一步）。

**Goal:** (1) mesh 族 sum 校验 fail 路径增加二次读取取证（复发时拿到块级定位+瞬态性自证）；(2) 排查 openssl_sm3sm4 大 data_size 全核散布失配机理并修复或明确排除。

**Architecture:** (1) 失败时二次读数组重求和——sum2==expected 即由失败线程自身洗清数组（不再依赖 125 读者论证），sum2 仍≠则数组/内存侧问题；同时打印首块级差异定位。(2) sm3sm4 从失配形态（首 16 字节后起始、~74% 散布）反推实现路径，复现→定位→修复。

**Tech Stack:** C++20/NEON（mesh）、框架 `report_fail_msg`/`log_skip` 约定、meson/ninja 构建。

**Spec:** 案例 `docs/cases/sdc-2026-10-16-core27-mesh-upi/`；本计划自含。

## Global Constraints

- 分支 `feat/sdc-run-scan` 续作；`git commit -s` DCO；一 patch 一单元。
- 提交前：`ninja -C builddir` 零新警告；受影响行为真实运行输出；zstd19 回归 `-t 2000 -n 1` exit: pass。
- x86-64 不触碰（改动仅 ARM64 路径文件）；占位诚实；文档同步。
- mesh 改动不改变通过/失败判定语义（只增加失败时的证据输出）——判定逻辑变更会污染历史对照。

---

### Task 1: mesh_upi_avx512_symm_int 失败路径二次读取取证

**Files:** Modify: `tests/cpu/mesh/mesh_upi_avx512_symm_int.cpp`（仅此一个测试先行——族级推广视本测试模式验证后另立 patch）。

- [ ] **Step 1**: 读当前 fail 路径（已知 L142-166 附近），在 `report_fail_msg` 前插入：
  - 二次全量读循环（同首读的 vld1q_s32 累加）得 `read_sum2`；
  - 打印 `re-read_sum=%lu (expected=%lu, first=%lu)`: sum2==expected ⇒ "transient datapath fault (array exonerated by re-read)"，否则 "array/memory side suspect"；
  - 块级定位：二次读时按块累计 block_sum2，与首读… 首读无块和——改为：二次读中输出与 expected 的偏差分块扫描不可行（无逐块期望）。**简化**：二次读只输出总 sum2 + 前 4 个与首读值不同的…不可得。**最终设计**：仅 sum2 对比（自证瞬态）+ 打印 data[] 首 16 值已存在。位级元素定位需首读逐块留和——首读循环里维护 `uint64_t block_sums[NUM_BLOCKS]`（每 16 元素一组，内存开销 NUM_BLOCKS×8B 可忽略），失败时二次读逐块对比，打印首个差异块号+该块 16 值（二次读的"真值"）+两版块和。
- [ ] **Step 2**: `ninja -C builddir` 零警告；`--cpuset=0 -t 3s -e mesh_upi_avx512_symm_int` pass（正常路径不受扰）。
- [ ] **Step 3**: 人工构造失败验证取证输出（临时把 expected_sum 判定反转跑一次——验证后还原；或读代码路径走查）。诚实要求：无法自然复现 SDC，取证路径验证用走查+构造。
- [ ] **Step 4**: zstd19 回归；提交 `tests(mesh): avx512_symm_int 失败路径二次读取+块级取证`。

### Task 2: sm3sm4 大 data_size 失配机理排查

**Files:** 调查 `tests/cpu/crypto/openssl_sm3sm4.cpp`（路径待定位）± 修复。

- [ ] **Step 1**: 定位测试源与 data_size 旋钮；理解 25.5MB/线程自适应（600‰）下首 16 字节后 ~74% 散布失配的实现路径（SM3 digest/SM4 cipher 的缓冲组织）。
- [ ] **Step 2**: 单核小尺寸复现矩阵：`--cpuset=0 -t 10s -O openssl_sm3sm4.data_size=<16K,1M,24M> -e openssl_sm3sm4`——确认失配是否 data_size 阈值触发（预期：小尺寸 pass、大尺寸 fail）。
- [ ] **Step 3**: 定位根因（候选：偏移/计数 32 位截断、块边界 stride、缓冲重用未清零、超大尺寸下 golden 生成 vs 消费不一致）并修复；若为设计限制则按占位诚实原则 log_skip 大尺寸档并文档化。
- [ ] **Step 4**: 回归（小/中/大三档真实输出）+ 提交。

### Task 3: 定向确认与驻留（已并行启动）

- [ ] scan-confirm-cpu27（簇 24-27 × 10 mesh 用例 × 60s）结果分析；
- [ ] 接 `run_cluster_anchored.sh -C 24-27 -e <NEON mesh 集> -t 1800 -r 4`（目标簇 vs 对照簇，判持续/瞬态+节律）；
- [ ] Task 1 增强二进制就绪后，驻留轮换用增强二进制（复发即得位级证据）。

## Self-Review
1. 覆盖：用户批准的三项 → Task 1(增强)/Task 2(sm3sm4)/Task 3(确认+驻留)。✓
2. 无占位：Task 1 Step 1 含最终设计（块和数组+二次读对比）。✓
3. 命名一致：read_sum2/block_sums。✓
4. Review Focus 对应：增强不改判定语义（Global Constraints 明示）；构造失败验证取证路径（Task 1 Step 3）。✓
