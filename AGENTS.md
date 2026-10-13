# AGENTS.md

本文件为 AI 代码检视代理提供 SDCShield 仓库级指令：任何变更在合入前，都必须通过下列规则与「SDC 激发能力」检视。

## 仓库概览

SDCShield 是 Intel OpenDCDiag 的 ARM64 移植版，用于检测**静默数据损坏（SDC, Silent Data
Corruption）**——即计算不崩溃、但产生错误比特。其检测原理是：压测目标计算单元（FMA、SIMD、
压缩、SVD、crypto），再与预先算好的 golden 值做**字节级比较**。

- **参考架构**：x86-64（**未改动**，作为移植正确性的对照基线）。
- **目标架构**：ARM64，面向 Kunpeng 920 / ARMv8.1+。
- **比对原则**：golden 比较是字节级 `memcmp`，浮点结果必须可复现。

## 关键规则

1. **golden 比对必须字节级、且浮点可复现**：比对使用 `memcmp`（或封装 `memcmp_or_fail`）。
   浮点输出必须逐位可复现——框架已在 `sandstone_data.cpp` 统一 quiet SNaN，以保证 FP16/BF16
   的 NaN 位型跨架构一致。任何引入架构相关浮点差异的改动都视为缺陷。
2. **x86-64 非回归（增量式移植）**：移植必须附加式实现，使用 `#elif __aarch64__` 与按架构的
   meson 守卫；只有在**确实共享**的代码路径上，才可把 `#ifdef __x86_64__` 扩展为同时包含
   `__aarch64__`。禁止无理由地改写 x86-64 已有逻辑。
3. **诚实性——禁止假通过**：ARM64 上尚未实现的功能，必须从 `test_init` 返回 `EXIT_SKIP`，
   且跳过理由为 `"to be implemented (placeholder): <缺失内容>"`；**禁止**用 `EXIT_SUCCESS`
   伪装通过（no-op 测试报 pass 是 bug）。`mce_check` 在 ARM64 是真实的 EDAC 后端测试，**必须通过**。
4. **一个补丁一个单元**：每个特性 / 修复 / 移植点单独提交，**不得捆绑**多个单元。提交遵循 DCO：
   `git commit -s`，`Signed-off-by` 为最后一行，其后不得再有内容。
5. **文档同步**：大颗粒度修改必须同步更新 `README.md` 与 `docs/` 下对应文档，确保内容 100% 准确。

## 代码检视重点：SDC 激发能力

SDC 检视的独特之处在于——不仅看代码「对不对」，更看它**是否真的能把计算单元逼出错比特**。
评审时请逐条核查以下 6 项：

1. **激发强度与覆盖**
   - 是否真正压到目标计算单元（FMA / SIMD / 压缩 / SVD / crypto），而非只调用一次即返回？
   - 数据宽度是否覆盖目标位宽（如 128/256/512-bit 向量、FP16/BF16/FP32/FP64）？
   - 数据模式是否多样（随机、全 0/全 1、边界值、NaN/Inf、极值），以避免被单一模式掩盖的缺陷？
   - `TEST_LOOP` 的定时压力是否充足，能否在给定时长内持续打满目标单元？

2. **golden 比对**
   - 是否使用字节级 `memcmp` 而非近似/容差比较？
   - 浮点是否逐位可复现（SNaN/NaN 位型跨架构一致，依赖框架在 `sandstone_data.cpp` 的 quiet 处理）？

3. **正确性基线（x86-64 非回归）**
   - 是否以 `#elif __aarch64__` / 按架构 meson 守卫的方式增量添加，未破坏 x86-64 参考路径？
   - 是否**不当扩大** `#ifdef __x86_64__`？只有在代码真正跨架构共享时才允许扩展。

4. **诚实性**
   - ARM64 未实现项是否返回 `EXIT_SKIP` 且理由为 `"to be implemented (placeholder): ..."`？
   - 是否存在以 `EXIT_SUCCESS` 假通过的 no-op 测试？

5. **架构相关缺陷（SVE/NEON VL）**
   - 是否存在与向量长度（VL）相关的越界读写或误报？
   - 注意：CI runner 的 VL=128 会掩盖 512-bit VL 下的缺陷，需人工确认更高 VL 下的正确性。

6. **失败现场留存**
   - 失败路径是否保留冷路径的 bit dump、sum-bit、failing-vector 记录，以便复现与定位？
   - 是否因过早返回而丢弃了足以定位故障的现场信息？

## 延伸阅读

- [`CLAUDE.md`](CLAUDE.md) — 仓库协作与工程规则总纲。
- [`docs/architecture.md`](docs/architecture.md) — 框架内部（fork 生命周期、质量门、特性探测、sysdeps、RAS、RNG）。
- [`docs/misc/writing_tests.md`](docs/misc/writing_tests.md) — 测试编写指南。
- [`docs/misc/coding_style_guide.md`](docs/misc/coding_style_guide.md) — 代码风格（C++20/C17，无警告）。
- [`README.md`](README.md) — 快速上手、构建与部署。
