# adaptive_l3_sdc 21 臂锁定标定扫收官档（2026-11-18）

## 战役概况

- **目的**：`adaptive_l3_sdc`（L3 域自适应 21 组合用例）在本机的逐组合检出基线标定——kPriorityOrder 的本机数据源（对齐 11-14 adaptive_mem_sdc 30 组合全扫方法论，600s/臂 TTF 可比口径）
- **方法**：每臂 600s，`ADAPTIVE_L3_SDC_COMBO=N` 锁定单组合；掩码 96-189（94 核，**179 在场**，190/191 排除）；auto seed；一次一组合（独占铁律）；bin `48ca0790f328`（已备份 `bin_sdcshield_backup`）
- **时间线**：11:38 起跑 → 15:06:32 收官（21/21 臂全齐），**全程零宕机**（boot 仍 2026-11-16 17:42）
- **环境记录**：`env.log`（3.5h 电压/占用，钉 0-7）

## 结果总表（21 臂）

| 臂 | 组合 | fail | | 臂 | 组合 | fail |
|---|---|---|---|---|---|---|
| c0 | seq_read/scalar | **0！！！** | | c11 | write/fp64 | **0！！！** |
| c1 | seq_read/neon | **0！！！** | | c12 | copy/scalar | **0！！！** |
| c2 | seq_read/fp64 | **0！！！** | | c13 | copy/neon | **0！！！** |
| c3 | rand_read/scalar | **0！！！** | | c14 | copy/fp64 | **0！！！** |
| c4 | rand_read/neon | **0！！！** | | c15 | rmw/scalar | **0！！！** |
| c5 | rand_read/fp64 | **0！！！** | | c16 | rmw/neon | **0！！！** |
| c6 | stride_read/scalar | **0！！！** | | c17 | rmw/fp64 | **0！！！** |
| c7 | stride_read/neon | **0！！！** | | c18 | fetch/scalar | **0！！！** |
| c8 | stride_read/fp64 | **0！！！** | | c19 | fetch/neon | **0！！！** |
| c9 | write/scalar | **0！！！** | | c20 | fetch/fp64 | **0！！！** |
| c10 | write/neon | **0！！！** | | | | |

**汇总**：TOTAL_FAILS = **0！！！**（双法交叉：21×`exit: pass` + 全库零 `miscompare` 字样）。

## 结论

1. **本机 L3 域 21 组合全部零检出！！！**——与全部历史证据一致（evict/L2 路径 30min 0-fail、movbe_l2 三法交叉 0、cache 变体全零）：**core-179 的触发域 = L1D/转发紧时序（2 源 load + ALU 链 + str→ldr ≤2cyc），L3 容量域全负空间**。21 臂把这条域边界又钉死了一环。
2. **kPriorityOrder 本机标定完成**：零 TTF 信号 → 表序恒等（0→20）即本机序——无检出机器上任何序等价，不改码。
3. **其他机器各自标定**：L3 域组合在他机可能是最快臂（用户实证：他机存在 4×ldr+1×str 触发）。标定方法 = 本档同法：`ADAPTIVE_L3_SDC_COMBO=<c>` 逐臂 600s + 按首检 TTF 排序，`ADAPTIVE_L3_SDC_ORDER` env 即可覆盖轮转序，无需改码。
4. **用例健康度**：3.5h×94 核满载零 crash、零假阳性、600s 臂步进精确——运行时底盘（shm 槽位轮转/fork 循环/auto-fracture 子进程短命再 fork）实战验证通过。

## 与 adaptive_mem_sdc（L1D 域）的分界

| | adaptive_mem_sdc | adaptive_l3_sdc |
|---|---|---|
| 域 | L1D/转发紧时序（小缓冲 1024 元素） | L3 容量域（24MB 动态窗/4MB 共享/64MB 窗） |
| 本机（core-179） | 15/30 组合有检出，全 179 | 21/21 组合零检出！！！ |
| 组合轴 | dtype×ldr×str×载入族（30） | 访问模式×数据通路（21） |
| 优先序 | 实测 TTF 序（11-14 全扫） | 表序恒等（本机标定 11-18） |

两用例互补：一个钉 L1D 转发域（本机实锤），一个扫 L3 容量域（他机候选）——机器上哪种域有缺陷，对应的自适应用例负责把它找出来。

## 数据与工件

- 原始日志：`/home/sdc/adaptive_l3_sweep21/c00..c20/run.log`（含 series_summary.txt）
- 环境记录：同目录 `env.log`；二进制备份：`bin_sdcshield_backup`（sha256 `48ca0790f328`）
- 断点续跑：`run_sweep.sh`（DONE 标记跳过已完成臂）
