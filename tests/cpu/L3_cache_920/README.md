# L3_cache_920 —— 鲲鹏 920 L3 检测精选（2026-11-17 L3 审计裁决）

本目录收纳经代码逻辑审计、在本机（鲲鹏 920，分布式 L3 = **24MB/24 核簇**，
框架 cache-info 与 sysfs 双源一致）真正具备 **L3 数据完整性检测能力** 的用例。

## 收录判据（两条件缺一不可）

1. **工作集为 L3 尺度**：动态探测簇 L3（容量边界语义，随宿主机自适应）
2. **校验点经存储层级重读比对**：memcmp 全量重读——而非 store-buffer/L1
   立即转发回读（后者校验的是 LSU 转发路径，L3 无校验点）

## 收录用例

**memcpy 对（L3 尺度复制路径）**：

| 用例 | 机制 |
|---|---|
| `memcpy_l3_cache_size` | L3 尺度块 memcpy（libc 路径）+ 目标预污染 + memcmp 全量重读校验 |
| `memcpy_l3_cache_size_neon` | 同几何，NEON vld1q/vst1q 流（绕过 libc IFUNC，保 NEON 数据通路归因）+ memcmp 校验 |

两者共享三步 L3 探测（device_info → sysconf → 8MB 兜底），窗口 1.5×L3，
随机块长 L3/8..1×L3，随机 64B 对齐偏移。

**mesh read 族六件（L3 读路径，2026-11-17 用户指令扩收）**：

| 用例 | 机制 |
|---|---|
| `mesh_upi_avx2_read_L3_int` | 共享 4MB 数组顺序 NEON 读 + sum/golden 全量重读校验 |
| `mesh_upi_sse_read_L3_int` | 同上（sse 名继承 x86，实跑 NEON 128 位） |
| `mesh_upi_avx512_asymm_read_L3_int` | 同上 + 16 元素块级 store→reload 一致性副校验 |
| `mesh_upi_avx2_read_only_int` | 共享 4MB 顺序读 + sum/golden 校验 |
| `mesh_upi_sse_read_only_int` | 同上 |
| `mesh_upi_avx512_asymm_read_only_int` | 同上 + 块级一致性副校验 |

几何：4MB **共享**数组 ∈ (L2 512KB, 簇 L3 24MB) → 稳态 L3 驻留，且
驻留不随线程数退化（共享单份，异于 write_only 的每线程扩容）。校验
语义：L3 驻留行任何位错改变 sum → 检出；理论盲区为补偿性双错（概率
可忽略）。x86 上这些 TU 走 #else 干净 SKIP（注册面不变）。

## 2026-11-17 八件套审计落选者（及落选原因）

| 用例 | 落选原因 |
|---|---|
| `llc_randomized_load` / `llc_uniform_load` | L3 激励真（24MB 容量边界）但校验 = store→asm-ldr 立即回读 = LSU 转发，L3 无校验点；data[] 载入翻转与寄存器值自洽不可见 |
| `llcstress` | 64MB>24MB 压力真，校验对 = 立即回读 = 转发 |
| `l3_execute` | ic ivau 重取指由 L2（PoU）供数——取指路径检测器，非 L3 |
| `mesh_upi_avx2_write_only_int` | vst1q→立即 vld1q 回读 = 转发；写流量穿 L3 但无校验点 |
| `mesh_upi_sse_write_only_int` / `mesh_upi_avx512_asymm_write_only_int` | 4KB 协作工作集，纯 L1/L2 域（描述的 L1/L2/L3 指跨核一致性路径） |

## 未收录的合格者

（无——mesh read 族六件已于 2026-11-17 扩收完毕；write_only 三件与
llc/l3_execute 五件见上表落选原因。）

## SVE 机注意

本目录为 NEON 机（920）精选；SVE 孪生（如 `mesh_upi_sve_read_L3_int`）
在 `tests/cpu/sve/` 按其自身组织。L3 尺寸探测随宿主机自适应，无需改码。
