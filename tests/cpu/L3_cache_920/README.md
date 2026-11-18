# L3_cache_920 —— 鲲鹏 920 L3 检测精选（2026-11-17/18 L3 审计 + 能力改造 + element-exact 收口）

本目录收纳在本机（鲲鹏 920，分布式 L3 = **24MB/24 核簇**，框架 cache-info
与 sysfs 双源一致）真正具备 **L3 数据/指令完整性检测能力** 的用例。
2026-11-18 能力改造（用户指令："原版是 x86 语义，改成本机能检测 L3"）后，
审计 14 件全部合格入夹；同日 element-exact 收口（检视裁决）将全部校验点
统一为**逐元素精确比对**。

## 收录判据（两条件缺一不可）

1. **工作集为 L3 尺度**（动态探测簇 L3，或 4MB 共享数组 ∈ (L2 512KB, 簇 L3 24MB)）
2. **校验点经存储层级重读、逐元素精确比对**（element-exact golden 副本
   逐 lane/逐字比较、延迟复验、跨核读者全量重读——而非 store-buffer
   立即转发回读，**也非 sum/reduce 聚合**：仓库铁律禁止聚合校验，
   保和类损坏（置换/对换/steering）对和值不可见、对逐元素比对可见）

## 收录用例（30 件 = 标量 14 + NEON 孪生 5 + FP64 孪生 10 + adaptive 1，2026-11-18 统一归位 + 复制体退役）

**复制路径（2）**——L3 尺度块 memcpy + memcmp 全量重读：
- `memcpy_l3_cache_size`（libc 路径）、`memcpy_l3_cache_size_neon`（vld1q/vst1q 流）

**读路径（10）**——共享 4MB 数组顺序读 + element-exact golden 全量重读：
- 标量 6：`mesh_upi_{avx2,sse}_read_L3_int`、`mesh_upi_{avx2,sse}_read_only_int`、
  `mesh_upi_avx512_asymm_read_{L3,only}_int`（asymm 版含块级 reload 副校验）
- FP64 4：`mesh_upi_avx2_read_L3_neon_fp64`、`mesh_upi_avx512_asymm_read_L3_neon_fp64`、
  `llc_randomized_load_neon_fp64`、`llc_uniform_load_neon_fp64`（后两件归随机/步长组，
  此处按数据通路计）——**原 12 件 FP64 read/write 扩展矩阵中 4 件
  （avx2_read_only / sse_read_L3 / sse_read_only / avx512_asymm_read_only 的
  neon_fp64 版）经 2026-11-18 检视退役**：与保留件逐字节相同（diff 实证仅
  测试名行不同），零边际检出能力——保留每对代表件，复制体不保留。

**写路径（6，2026-11-18 改造 + element-exact 收口）**：
- `mesh_upi_avx2_write_only_int`——两遍式：写满 4MB/线程（同步双写 golden 副本）
  → 延迟全量重读**逐元素比对**
- `mesh_upi_sse_write_only_int`、`mesh_upi_avx512_asymm_write_only_int`——扩容 4MB
  共享（原 4KB），写点双写 golden，读者全量重读逐元素比对
  （原差分 sum 协议已退役）
- FP64 3：`mesh_upi_{avx2,sse,avx512_asymm}_write_only_neon_fp64`（同双写模式）

**指令路径（3，2026-11-18 改造）**：
- `l3_execute`——dc civac 逐出 L2（到 PoC）→ ic ivau 重取指由 **L3 供数**，init 快照位级比对
- `l3_execute_neon` / `l3_execute_neon_fp64`——同机制，NEON/FP64 计算核

**随机/步长压力 + L3 校验（7，2026-11-18 改造 + element-exact 收口）**：
- `llcstress`（+`_neon`、`_neon_fp64`）——64MB 窗随机压力 + 每 65536 存储的
  全槽位延迟复验（影子比对，slot 粒度）
- `llc_randomized_load` / `llc_uniform_load`（各 +`_neon`、`_neon_fp64`）——
  24MB 动态窗压力 + 周期稠密 element-exact golden 扫（逐元素比对）

**自适应编队（1）**：
- `adaptive_l3_sdc`——21-combo 轮换底盘（seq/rand/stride/read/write/copy/rmw/fetch
  × scalar/NEON/FP64），引擎层与上述父测试语义同步（element-exact）

## 改造前审计落选原因（存档；"改造手段"列为 2026-11-17 首轮，后续 element-exact 收口统一升级为 golden 副本逐元素比对）

| 原落选者 | 改造前缺陷 | 首轮改造手段 → element-exact 收口 |
|---|---|---|
| llc_randomized/uniform_load | data[] 载入翻转被寄存器洗白；check[] 校验=LSU 转发 | 稠密扫算 → golden 副本逐元素比对 |
| llcstress | store→立即回读 = store-buffer 转发 | 延迟复验扫（影子比对，本就 slot 粒度精确） |
| l3_execute | ic ivau 只到 PoU=L2，热 worker 恒驻 L2 | dc civac 清+失效到 PoC，逐出 L2 落驻 L3 切片 |
| mesh_upi_avx2_write_only | vst1q→立即 vld1q 回读 = 转发 | 两遍式延迟重读（sum）→ golden 副本逐元素比对 |
| mesh_upi_{sse,avx512_asymm}_write_only | 4KB 工作集纯 L1 域 | 扩容 4MB 共享（差分 sum）→ 写点双写 golden 逐元素比对 |

## 诚实边界

- element-exact golden 比对的残留风险 = golden 副本与被测数据**共模损坏**
  （同一物理缺陷同时翻转两份副本的对应位）——与全仓 byte-identical memcmp
  黄金模式同款边界；golden 副本与数据在不同缓存行/不同分配，共模概率极低
- llcstress 延迟复验中刚重写的尾部槽位停留 L2（混合服务，源码注记）
- write_only avx2 版簇内并发 >6 线程时聚合超切片（流式为主，源码注记）
- SVE 孪生不随迁（`tests/cpu/sve/mesh/` 按其自身组织）
- **NEON/FP64 孪生已统一迁入本目录（2026-11-18 用户指令）+ 4 件复制体
  退役（同日检视裁决，见上）**：现存 NEON 孪生 5 件
  （`memcpy_l3_cache_size_neon`、`llc_{randomized,uniform}_load_neon`、
  `llcstress_neon`、`l3_execute_neon`）+ FP64 孪生 10 件
  （`l3_execute_neon_fp64`、`llcstress_neon_fp64`、
  `memcpy_l3_cache_size_neon_fp64`、`llc_{randomized,uniform}_load_neon_fp64`、
  `mesh_upi_{avx2_read_L3,avx512_asymm_read_L3,avx2,sse,avx512_asymm}_write_only*`
  的 neon_fp64 5 件）。标量 ↔ NEON ↔ FP64 三种数据通路 × 读/写/复制/指令
  四路径——30 件全在本目录（adaptive_l3_sdc 另计）

## x86-64

注册面与测试 ID 不变（mesh 族 x86 走 `#else` 干净 SKIP；退役的 4 件
FP64 复制体从注册面移除——它们从未提供独立检测面）。
