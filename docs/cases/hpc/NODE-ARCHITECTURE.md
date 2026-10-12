# 计算节点 cn23154 软硬件架构与 CPU 微架构深度研究

> 写于 2026-09-11,实测于 NSCC 集群计算节点 cn23154(经 login01 → dattach)。
> 探测方法:lscpu / /proc / /sys + ID 寄存器直读(MRS)+ 自编译微基准(延迟/带宽/FLOPS)。

## 1. 节点身份与整机

| 项目 | 值 |
|---|---|
| 节点名 | cn23154(集群拓扑:G11-0;集群共 7 排机柜 A–G、~104 橱、总核 ~1432 万) |
| 主板/整机 | 华为 S920X00 / TaiShan 服务器(BIOS 32.80, 2026-06-17) |
| CPU 型号 | HiSilicon part **0xd22**(调度器 modelName "72F5")——未公开发布的新一代 Taishan 核 |
| 封装 | 2 路(socket)× 304 核 = **608 核**,无 SMT |
| 主频 | 2.0 GHz 定频(cppc_cpufreq + userspace governor;范围 1200–2000 MHz) |
| MIDR | 0x00000000480fd220(implementer=0x48 HiSilicon, part=0xd22, rev=0) |
| 内存 | 565 GB 总量:16 个计算 NUMA 节点各 ~31–33 GB + 16 个无 CPU 内存节点各 4 GB |
| OS | Kylin V10 (Jasmine),内核 5.10.0-285.5.v2403.ky10p.aarch64 |
| 页粒度 | **仅 64KB**(TGran4=0xf;PAGESIZE 实测 4096 是内核兼容层) |
| CPU 隔离 | isolcpus + nohz_full 覆盖 608 核,每 NUMA 留末核(37,75,…,607)处理中断 |

## 2. CPU 微架构

### 2.1 核心特征(ARMv9)

- **SVE 512-bit 向量**(prctl PR_SVE_GET_VL 实测 64 字节;ZFR0 SVEver=1, f32mm/f64mm/BF16=1)
- **SME + SME2**(SMFR0: f64f64=1, b16f32=1, f32f32=1, i8i32=1;fa64=0)
- ISA 扩展:AES+PMULL、SHA1/2/512、SHA3、SM3/SM4(国密)、CRC32、LSE、LRCPC2/3、FCMA、JSCVT、SVE2 全集(sveaes/svesha3/svesm4)、i8mm、bf16、RPRES、WFXT
- PFR0: RAS=1, DIT=1, CSV2/3=1, AMU=1;PFR1: RNDR=0, BTI=0, MTE=0
- PMUv3(6 通用计数器)+ SPE(ARM Statistical Profiling Extension)

### 2.2 流水线吞吐(自测 FMA 微基准,单核 @2.0GHz)

| 数据通路 | 实测 | flop/cycle | 说明 |
|---|---|---|---|
| 标量 FMADD 8 链 | 8.0 Gflop/s | **4** | 2 条标量 FMA/周期 |
| NEON128 8×2lane | 13.5 Gflop/s | ~6.75 | 向量 FMA 2/周期(128b) |
| SVE512 8×8lane | 27.2 Gflop/s(下限) | ~13.6 | 理论 2×512b FMA/cyc=16;初版基准被编译器削链,待复测 |

**单核理论峰值:64 flop/cyc(SVE512 FP64 FMA)→ 128 Gflop/s;608 核节点理论 ~77.8 Tflop/s(FP64)。**

### 2.3 缓存层级(每核私有,SoC 无 LLC)

| 层级 | 容量/组织 | 实测延迟(指针追踪) |
|---|---|---|
| L1D | 32KB,8 路,64B 行 | ~5.0 ns(≈10 cyc) |
| L1I | 32KB,4 路,64B 行(CTR_EL0=0x9444c004,ERG=64B) | — |
| L2 | **768KB,12 路**,统一,私有 | ~8.7 ns(≈17 cyc) |
| L3 | **不存在**(sysfs 无 index3,CLIDR 陷阱) | — |
| SCN/网状远端 | 8–16MB 呈 45–90 ns | ~45–90 ns |
| 远程 NUMA 内存 | 256MB+ 工作集 | **~130 ns** |

NUMA 距离矩阵(node0 视角):本地 10;同 socket 内 20/30/40(子簇层级);跨 socket 61/71/81/91。

### 2.4 NUMA 拓扑(32 节点)

- 16 个"计算"节点:node0–15(每节点 38 核 + ~31–33 GB),socket0=0–7,socket1=8–15
- 16 个无 CPU 节点:node16–31(各 4 GB),距离 61–91 → 疑似跨 socket 挂载的近存/CXL 扩展内存

### 2.5 其他系统寄存器实测

- DCZIDR=0x4(DC ZVA 64B);CNTFRQ=100 MHz
- MMFR0 PARange 解码受限(EL0 仅见部分位),大页 64KB 强制
- 分支预测:待节点空闲后补测容量曲线(bpbench)

## 3. 互连与 I/O

| 项目 | 值 |
|---|---|
| 片间互连 | HCCS 类,跨 socket NUMA 距离 61–91 |
| 计算 NIC | **8× 200GbE RoCE**(hns3,roceroh0–7,MTU 4096),聚合 **1.6 Tb/s**;socket0/1 各 4 |
| 管理 NIC | 2× 10GE(01:00.0 Huawei 0222)+ bond |
| PCIe | Gen4 Root Complex;每 socket 多组 Root Port |
| 本地盘 | 登录节点 2× 894GB SAS(MegaRAID 3508);计算节点无本地大数据盘 |
| 并行 FS | 华为 dtfs:/home/share 2.5PB、/eapp_test_0{1,2} 各 5PB;另 NFS 管理面 |
| UnCore PMU | 171 个 perf 设备:8 SCCL×(4 DDRC+4 HHA+16 UC)、14 SICL L3(hisi_sicl3_pa/h60pa)、SPE、PCIe PTT |

## 4. 集群与软件栈

- 调度器:华为 **Donau**(dsub/djob/dattach/dkill/dtopo/dnode),队列 q_Test_20260903
- 登录节点:login01(S920X00,2×64 核 Kunpeng-920 0xd01,502GB,100GbE RoCE ×2)
- 编译器:GCC 10.3.1(节点/登录)、BiSheng 3.1(登录)、clang 12(openEuler)
- MPI:HMPI(华为自研);perf 5.10 可用

## 5. 对 HPL/优化的启示

1. **无 LLC、私有 768KB L2** → 分块贴 L2:NB ~192–256,P×Q 小、BCAST 维持面板在 L2
2. **SVE512** → BiSheng `-march=armv9-a+sve512` 可获 2× 向量宽 vs 920 的 256b
3. **16+16 NUMA** → numactl 绑核时排除无 CPU 节点;跨 socket 距离高,P×Q 布局要同 socket 优先
4. **64KB 页强制 + THP 默认开** → 无需显式 hugetlbfs
5. **8×200G RoCE** → MPI 绑 NUMA 本地 roceroh;拓扑感知 rankfile
6. 定频 2.0 GHz(userspace governor 锁 MAX),无变频噪声,测量稳定

## 6. 复现命令(登录节点)

```bash
# 节点详情
dnode -l cn23154; dnode --frequency cn23154
# 进入节点(需 RUNNING job)
djob; dattach -c 'lscpu' <jobid>
# 微基准(已编译,在 /home/share/suke/archprobe/)
dattach -c '/home/share/suke/archprobe/latbench' <jobid>   # 缓存延迟曲线
dattach -c '/home/share/suke/archprobe/bwbench 0 512' <jobid> # 单核带宽
dattach -c '/home/share/suke/archprobe/membw_multi' <jobid>   # 聚合带宽
dattach -c '/home/share/suke/archprobe/flopbench' <jobid>     # 峰值 FLOPS
dattach -c '/home/share/suke/archprobe/archprobe' <jobid>     # ID 寄存器
```

## 7. 未决项

- [ ] SVE512 峰值复测(修正版 flopbench 已写好,等节点空闲)
- [ ] 分支预测器容量(bpbench,中断过)
- [ ] node16–31 无 CPU 内存节点性质确认(CXL?HBM?)——需 ddrc PMU 或 dmesg(root)
