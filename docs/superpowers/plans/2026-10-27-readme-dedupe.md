# README.md 冗余精简(第二轮)实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在第一轮(过时修正,PR #300)基础上,删除 README.md 中的冗余内容——重复段落、巨型单元格、验证过程细节——把 566 行压到 ~480 行,信息密度提升而不丢失任何操作性事实。

**Architecture:** 纯文档精简。冗余判定原则:①同一事实出现两次以上→留一处;②验证过程记录(多少秒×多少 seed 的会话细节)→压缩为一句结论+指针(CLAUDE.md/docs 有完整口径);③单用例说明书级细节→概括一句;④合规性警示(旧二进制判读)→保留但压缩。

**Tech Stack:** Markdown;验证 = grep 无事实丢失关键项 + 纯文档变更(二进制不受影响,第一轮构建/回归口径仍有效)。

**Spec:** 本计划自身;前置 = `2026-10-27-readme-refactor.md`(第一轮,已 merge PR #300)。

## Global Constraints

- 单 commit、`git commit -s` 末行唯一 `Signed-off-by: wangxu <wangxumarshall@qq.com>`、特性分支。
- **不删除的内容**:所有命令、所有 knob 域、所有链接、旧二进制判读警示(合规价值)、优先序依据(文献引用)、检出通道口径(eigen 盲区披露——诚实边界,必须保留)。
- 判读规则与机制说明保留;只删过程性/重复性文字。

## 冗余清单(逐项)

| # | 位置 | 冗余类型 | 处置 |
|---|---|---|---|
| R1 | 「结果判读」eigen 段(~600 字) | 验证过程细节(3×240s×3 seed、k=3.30~3.39、24.9×、300s 宽限推导) | 压缩为结论+一句验证概括;**保留**:修复机制、满核挂=SDC 嫌疑、检出通道盲区披露 |
| R2 | 旧二进制判读警示块(~400 字) | 重复解释两次"旧口径" | 压缩;保留 KNOWN_FLAKY 恢复命令 |
| R3 | Vendored 节 isa-l 段 | 与同节表格逐句重复 | 删整段 |
| R4 | 检测能力表「ARM64 SDC 专项」单元格(~1100 字) | 每用例说明书 | 压缩为域级概括(每用例一句以内),细节指向 `-l` 输出 |
| R5 | 检测能力表「SDC 激发增强套件」单元格(~550 字) | 同上+框架诊断细节 | 压缩,保留设计依据一句话 |
| R6 | 「能力门控的诚实跳过」段(~400 字) | gem5 实证过程细节 | 压缩,保留流数=lane 数机制+W 调度定维 |
| R7 | 「运维入口」节(~300 字) | 与「一键最大程度激发 SDC」节重复(excite.sh 旗标讲两遍) | 压缩为细分脚本一句话+runbook 链接 |
| R8 | 模式 E 代码块(12 行) | 9 测试 12 knob 全展开 | 精简为代表性 4-5 行+说明 |
| R9 | memcpy_rewr「参考实测」(~200 字) | 过程数据 | 压缩为结论行 |
| R10 | OpenBLAS 锁设计段 | 论证细节 | 保留结论(单线程+锁=防假阳),压缩实证过程 |

---

### Task 1: 精简「一行命令」节(R1/R2)

- [ ] 压缩 eigen 判读段:保留修复机制/判读规则/检出通道盲区;验证细节(多机多 seed 数据)缩为一句。
- [ ] 压缩旧二进制警示:保留恢复命令与"仅旧二进制适用"边界。

### Task 2: 精简 vendored 与战役模式节(R3/R8/R10)

- [ ] 删 isa-l 重复段;压缩 OpenBLAS 锁设计论证;模式 E 代码块减半。

### Task 3: 精简检测能力表与门控段(R4/R5/R6/R9)

- [ ] 压缩两个巨型单元格为域级概括;能力门控段保留机制删过程;memcpy_rewr 实测压缩。

### Task 4: 精简「运维入口」节(R7)

- [ ] 删与上节重复的 excite.sh 用法,保留细分脚本指引+runbook 链接。

### Task 5: 校验 + 提交

- [ ] grep 校验:命令/knob/链接/KNOWN_FLAKY/盲区披露全部仍在;`wc -l` 报告行数变化。
- [ ] 切分支 `docs/readme-dedupe-2026-10-27`,单 commit 推送,开 PR。
