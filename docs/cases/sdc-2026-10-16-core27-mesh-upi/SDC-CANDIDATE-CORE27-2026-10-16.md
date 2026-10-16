# SDC 候选事件：CPU 27 单核瞬态计算通路损坏（mesh_upi_avx512_symm_int）

> 首次捕获：2026-10-16 06:57:17 (+08:00)，TaiShan 2280 参考机（Kunpeng 920, 128 核/126 在线, NEON-only）
> 战役：7×24 full 模式 cycle 1 L3 多样性轮转（`060357-l3_fixed_c1.yaml`，1.87GB）
> 事件目录：`~/sdc-excite-reproduce/events/20261016-102251-l3_fixed_c1-rc143/`（SIGTERM 收尾触发 end-of-phase 取证落盘；fail 本身发生于 06:57，非停止伪影）

## 一、事件五要素

| 要素 | 内容 |
|---|---|
| **核定位** | **CPU 27**（NUMA0，簇 24-27，module 3234）；cpu-mask 单 X；失败消息 "Thread 27" |
| **用例** | `mesh_upi_avx512_symm_int`（NEON int32 跨核 L1/L2 load/store mesh，sum 校验；激发增强套件 H14'/P17 改造版） |
| **形态/位模式** | `read_sum=8019260, expected_sum=0, consistent=1`——求和偏差 +8019260，store→reload 一致性检查通过。iteration 11，time-to-fail 75.275s。**无位级转储**（测试只打印聚合和，损坏元素/位不可恢复——已知局限） |
| **触发条件** | 满载（L3 全核 + 热扫描 8 实例 + CPU 回填 + 内存伴层）、内存压力活跃（swap in/out）、温度带 65-67°C、seed `LCG:1607519520` |
| **复现节律** | 瞬态：框架立即重试（iteration 1, retry: true）**pass**；该测试本机首次执行即现此事件；单事件 |

原始证据（yaml 13342 行区段，line 7162069 起）：

```
E> Failed at ../tests/cpu/mesh/mesh_upi_avx512_symm_int.cpp:162: Sum mismatch or consistency failure
mesh_upi_avx512_symm_int: Thread 27, data[0..15]=(-115058,670200,...), read_sum=8019260, expected_sum=0, consistent=1, result=FAIL
```

## 二、定性推理链（为什么这是 SDC 候选而非伪影）

1. **数组被 125 个正确读者洗清**：所有 126 线程读**同一共享数组** `td->data`。若损坏在数组/内存页/swap 拷贝，所有读者求和同错。实测仅 Thread 27 偏差 → 损坏**严格局部于 Thread 27 的计算通路**（vld1q_s32 载入返回 / 寄存器堆 / 整数加法之一）。此论证同时排除内核页管理（swap/THP 拷贝损坏）假说。
2. **consistent=1 佐证时序**：一致性检查比较的是 栈 vals→store_buf→重载 路径（损坏数据进入校验缓冲之前已错但自洽）→ 损坏在首次 `vld1q_s32(td->data)` 载入或求和算术，非存储器内容。
3. **协议竞态被门控排除**（源码逐路径分析，`tests/cpu/mesh/mesh_upi_avx512_symm_int.cpp`）：
   - reset 需 `read_done==126`，而 Thread 27 失败路径在 `read_done++` 之前 `return EXIT_FAILURE` → Thread 27 读取期间数组冻结，无跨迭代数据混合；
   - 快照（`expected_sum = global_sum.load(seq_cst)`）先于读循环，且 global_sum.fetch_add → write_done.fetch_add 与读取侧的 seq_cst 全序保证快照完整；
   - 写屏障时间退出路径需要"到时仍有块未写"，将导致**多数读者**失配而非单线程——与观测不符。
4. **同 run 同型事件的对照排除**：同相位 sm3sm4 全核 fail（19M/25.5M 散布失配、双引擎双负载水平均败、L2 谱系已败）= 确定性软件失败（数据规模相关测试缺陷候选，另行排查）；mesh_upi_sse_sym crash = 07:11:38 内核 OOM-killer 杀 `control` 辅助进程（journal 实证）= 内存超订伪影。两者均与本事件形态正交。

**分类**：类型 SDC 候选（单核瞬态、静默、无 crash、重试即过）——与机队 CPU179 家谱（LSU load-return 旁路）形状最接近；非 cn23154 形态（该签名为 SEGV + VA[55:48] 高位 + SVE）。

## 三、已知局限（诚实边界）

- 单事件；无第二次观测，节律未知。
- 测试证据只含聚合和：损坏的元素/位不可恢复。（增强建议：fail 路径 dump 首个失配元素与期望值——见"下一步"）
- 硬件瞬态 vs 极罕见软件时序（seq_cst 假设依赖工具链正确性）不能 100% 分辨；置信度基于推理链强度而非绝对证明。

## 四、触发时系统工况（事件上下文）

- L3 固定序全核轮转（-T 21600s -t 60s --strict-runtime，--disable eigen_svd_double,eigen_sparse）
- 并行：热扫描 8 实例（par=8，eigen 波次）、CPU 回填 matrixprod、内存伴层（目标带 [6.5,8]GiB avail，实测谷底 185MB-2G 波动，swap 活跃）
- 温度 63-67°C（acpitz），联锁阈值 90°C 未触及
- 框架旗标：--on-crash=context --retest-on-failure=3 --temperature-threshold=95000

## 五、下一步（未执行——压测已于 2026-10-16 10:22 被用户指令停止）

1. **定向确认**（需批准恢复压测）：`bash scripts/run/sdc-run.sh scan --cores 24-27 --tests mesh_upi_avx512_symm_int,mesh_upi_avx512_sym,mesh_upi_sse_symm_int --t 60s --par 4` + 簇 24-27 饱和长驻留（run_cluster_anchored.sh -C 24-27 -e <mesh 族> -t 1800）
2. **测试增强**（仓库工作）：mesh 族 fail 路径打印首个失配元素的实际值/期望值/索引（位模式取证能力）
3. **排除 sm3sm4**：openssl_sm3sm4 大 data_size（≈25.5MB/线程自适应）失配机理排查（首 16 字节后散布失配的块边界形态），修复前从 SDC 事件计数中排除
4. 长时暴露继续收集第二事件（节律/占空比统计）

## 六、数据坐标

- 主证据：`~/sdc-excite-reproduce/logs/20261016/060357-l3_fixed_c1.yaml` line 7162069-7162602（fail+retry-pass 全块）
- 事件取证：`~/sdc-excite-reproduce/events/20261016-102251-l3_fixed_c1-rc143/`
- 对照（sm3sm4 L2 谱系败）：`~/sdc-excite-reproduce/logs/spectrum_c1_files/sdc_spectrum_20261016_020100/crypto.yaml`
- OOM 伪影实证：`journalctl -k` 2026-10-16 07:11:38 oom-kill task=control
- 静默基线（CPU 27 全过）：`~/sdc-run/scan-baseline-1015-2359/matrix.tsv`；满载热扫（CPU 27 全过，但热扫不含 mesh 族）：`~/sdc-run/scan-hot-1016-0154/matrix.tsv`
