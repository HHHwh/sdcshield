# 已解决：mesh_upi_avx512_symm_int 竞态伪影（原"CPU27 SDC 候选"重新分类）

> 2026-10-16 初判"SDC 候选"（见下文存档），**当日经簇锚定对照实验推翻**——本文件保留完整推理链与推翻过程，作为假阳性鉴别的范本。
> 状态：**非 SDC——mesh_upi_avx512_symm_int 测试自身竞态**；机器无异常。

## 一、推翻证据（时序）

1. **13:46 簇锚定 dwell（增强取证二进制，未修竞态）**：目标簇 24-27 与**健康对照簇 0-3 均 8/8 轮失败**（每轮 4 分钟内即败，iteration 7-35）。硬件不可能双簇同型复现——测试缺陷实锤。
2. **取证输出实证跨迭代数据混合**：失败线程 `expected=0`（reset 后/未写状态）+ 首读见旧数据（block 级 delta）+ 重读=写侧真值（数组最终自洽）——读到了跨迭代混合数据，且快照取到零值状态。
3. **单核/短时验证的盲区**：2 核 × 3s 与 20s 短量子全部 pass（竞态需要特定交错+较长运行才现形）——这也是初判"SDC 候选"时误排除竞态的原因：当时的协议门控推演（read_done==total 门死 reset）存在漏洞，实证优先于推演。

## 二、原初判存档（2026-10-16 06:57 事件，供方法论复盘）

- 事件：L3 全核轮转中 `mesh_upi_avx512_symm_int` Thread 27（CPU 25/27 区域）单线程 `read_sum=8019260, expected_sum=0, consistent=1`，重试即过。
- 初判推理：①125 正确读者洗清共享数组→损坏局部于单线程计算通路；②协议门控推演排除竞态；③同 run 的 sm3sm4 全核 fail（大 data_size 软件缺陷）与 mesh_upi_sse_sym crash（OOM 伪影，journal 07:11:38 实证）形态正交。
- **推理链缺陷复盘**：②的门控推演未考虑"框架按调度量子重跑测试（每量子独立 test_init/td，实测 120s 内 101 个条目）+ 量子边界时间条件切断 + 屏障判定与快照非原子"的组合；①的"读者洗清"论证在**跨迭代混合读**场景失效（不同读者读的是不同迭代的数据，不构成互相洗清）。**教训：单事件定性必须先做可复现性对照（对照簇/定向重压），推演只能排序假设不能定案。**

## 三、测试竞态的修复

- 取证增强（06fadfb1）：写侧逐块真值 + 失败二次读 + 块级定位——本案例正是靠它拿到"首读≠重读=写侧"的混合读实证。
- **v2 协议重设计已合入（13b13850）**：静态块分区 + 双感知反转屏障（B1 写后/B2 读后）+ 累计和差分，三项结构性消除竞态（无计数器清零、无胜者选举、无套圈窗口）。验证：双簇 2×600s 饱和驻留 4/4 pass（同形状旧码 8/8 假 FAIL）+ 32 核 L3 形态 pass + 单核退化 pass + zstd19 回归 pass。v1 generation 方案因驻留形态死锁已回滚（4 worker 自旋 + 主 futex 等待，机制未完全定位——v2 不依赖该理解，结构性消除）。
- **其余 18 个 mesh 族文件共享同款旧模式**（`grep -l "expected_sum = td->global_sum.load" tests/cpu/mesh/`），asymm 变体用 `allocated_blocks` 屏障——待按 v2 范式逐个推广（独立 patch）。

## 四、对 SDC 猎捕的净结论

**本机（TaiShan 2280 参考机）至 2026-10-16 无确认自然 SDC 事件**。全部异常已定性：
| 异常 | 定性 |
|---|---|
| mesh_upi_avx512_symm_int 单核 fail（06:57） | **测试竞态伪影**（本文件） |
| openssl_sm3sm4 全核 fail（08:09，L2 谱系同败） | 大 data_size（≥16M 病理变慢+失配）软件缺陷，另行排查 |
| mesh_upi_sse_sym crash（07:15） | OOM 伪影（journal 07:11:38 oom-kill task=control） |
| eigen_svd/eigen_sparse 偶发 | 已知 Eigen 全核多线程限制（CLAUDE.md） |

数据坐标不变：`~/sdc-excite-reproduce/logs/20261016/060357-l3_fixed_c1.yaml`（1.87GB，fail 块 line 7162069）、`cluster_anchored_logs/round{1-4}_{target,control}.yaml`（8/8 复现）、`/tmp/trace-run.yaml`（89465 行插桩 trace，101 条目模型实证）。
