# L3_cache_920 —— 鲲鹏 920 L3 检测精选（2026-11-17/18 L3 审计 + 能力改造）

本目录收纳在本机（鲲鹏 920，分布式 L3 = **24MB/24 核簇**，框架 cache-info
与 sysfs 双源一致）真正具备 **L3 数据/指令完整性检测能力** 的用例。
2026-11-18 能力改造（用户指令："原版是 x86 语义，改成本机能检测 L3"）后，
审计 14 件全部合格入夹。

## 收录判据（两条件缺一不可）

1. **工作集为 L3 尺度**（动态探测簇 L3，或 4MB 共享数组 ∈ (L2 512KB, 簇 L3 24MB)）
2. **校验点经存储层级重读比对**（memcmp/sum 稠密重读、延迟复验、跨核读者
   全量重读——而非 store-buffer 立即转发回读）

## 收录用例（15 件 = 审计 14 件 + NEON memcpy 孪生）

**复制路径（2）**——L3 尺度块 memcpy + memcmp 全量重读：
- `memcpy_l3_cache_size`（libc 路径）、`memcpy_l3_cache_size_neon`（vld1q/vst1q 流）

**读路径（6）**——共享 4MB 数组顺序读 + sum/golden 全量重读：
- `mesh_upi_{avx2,sse}_read_L3_int`、`mesh_upi_avx512_asymm_read_L3_int`（+块级 reload 副校验）
- `mesh_upi_{avx2,sse}_read_only_int`、`mesh_upi_avx512_asymm_read_only_int`

**写路径（3，2026-11-18 改造）**：
- `mesh_upi_avx2_write_only_int`——两遍式：写满 4MB/线程 + golden sum → 延迟全量重读复算
- `mesh_upi_sse_write_only_int`、`mesh_upi_avx512_asymm_write_only_int`——扩容 4MB 共享（原 4KB），既有读者差分 sum 协议经存储层级

**指令路径（1，2026-11-18 改造）**：
- `l3_execute`——dc civac 逐出 L2（到 PoC）→ ic ivau 重取指由 **L3 供数**，init 快照位级比对

**随机/步长压力 + L3 校验（3，2026-11-18 改造）**：
- `llcstress`——64MB 窗随机压力 + 每 65536 存储的全槽位延迟复验（影子比对）
- `llc_randomized_load` / `llc_uniform_load`——24MB 动态窗压力 + 周期稠密黄金扫算

## 改造前审计落选原因（存档）

| 原落选者 | 改造前缺陷 | 改造手段 |
|---|---|---|
| llc_randomized/uniform_load | data[] 载入翻转被寄存器洗白；check[] 校验=LSU 转发 | 稠密黄金扫算（data[] 永不再写，golden_sum init 定标） |
| llcstress | store→立即回读 = store-buffer 转发 | 延迟复验扫（间隔 65536 随机游走后影子比对） |
| l3_execute | ic ivau 只到 PoU=L2，热 worker 恒驻 L2 | dc civac 清+失效到 PoC，逐出 L2 落驻 L3 切片 |
| mesh_upi_avx2_write_only | vst1q→立即 vld1q 回读 = 转发 | 两遍式延迟重读 |
| mesh_upi_{sse,avx512_asymm}_write_only | 4KB 工作集纯 L1 域 | 扩容 4MB 共享（协议逐句保留，节流档距随尺度放大，补 fracture=-1） |

## 诚实边界

- sum/扫算类校验理论盲区 = 补偿性双错（概率可忽略，read 族同款）
- llcstress 延迟复验中刚重写的尾部槽位停留 L2（混合服务，源码注记）
- write_only avx2 版簇内并发 >6 线程时聚合超切片（流式为主，源码注记）
- SVE 孪生不随迁（`tests/cpu/sve/mesh/` 按其自身组织）
- arm_add/fp64 下的同名 NEON/FP64 孪生（`llc_{randomized,uniform}_load_neon`、`llcstress_neon{,_fp64}`、`l3_execute_neon{,_fp64}` 共 6 件）已于 2026-11-18 **同步传导能力改造**（civac 逐出 L2 / 延迟复验扫 + STRIDE_V 512 / 动态 L3 窗 + 稠密向量黄金扫算），留在其家族目录（arm_add NEON pass / fp64 数据通路族）组织

## x86-64

全部 15 件注册面与测试 ID 不变；mesh 族 x86 走 `#else` 干净 SKIP。
