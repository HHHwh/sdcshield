# HPL 全节点压测 & SDC 检测方案(cn23154 / 鲲鹏 608 核)

## 目的
用 HPL 大规模矩阵计算把 CPU 压满,通过残差校验(`PASSED/FAILED`)捕捉
SDC(Silent Data Corruption,静默数据损坏)故障。

## 硬件/软件架构(实测)
- CPU: 2× HiSilicon 0xd22(鲲鹏),304 核/socket,共 **608 核**,无 SMT
- 频率: 定频 2.0GHz(userspace governor,无睿频)
- ISA: **SVE2 + SME**(smef64f64 硬件矩阵引擎,KML sme/kblas 内核使用)
- NUMA: 16 个计算域(每域 38 核 + ~32GB DDR),另有 16 个远端内存节点
  (4GB/个,距离 61-91,推测 HBM/CXL 扩展内存)
- 内存: 565GB
- OS: Kylin V10 (Jasmine), kernel 5.10.0-285
- 关键内核参数: `isolcpus=nohz,domain,managed_irq` 摘除 592 个计算核,
  **进程默认 affinity 只有 16 个管家核** → 必须 `taskset -c 0-607`
- MPI: HyperMPI (Open MPI 4.1.6rc4)
- BLAS: 华为 KML(libkml_rt 调度 → sme/kblas、sve512/libklapack_full)
- 运行时依赖: libgfortran.so.5(计算节点系统盘没有,从登录节点拷贝到
  共享目录 `gf_libs/` 加入 LD_LIBRARY_PATH)

## HPL 配置(压满 608 核)
```
N  = 216000     # 373GB 矩阵,占可用内存 ~78%,不 OOM
NB = 192        # 实测最优(兼容 SME 内核分块)
P×Q = 19 × 32   # 608 = 19×32;NUMA 对齐:每个域恰好 2 个完整进程列
PMAP = 1        # Column-major,rank 与 NUMA 域连续对应
BLAS_NUM_THREADS=1, OMP_NUM_THREADS=1   # 纯 MPI,一 rank 一核,无超订
```

## 提交与运行
- 目录: `/home/share/suke/hpl-full/`
  - `HPL.dat` 压测配置
  - `run_full.sh` 循环执行脚本(每轮校验,FAILED/异常退出 → `sdc_alerts.log`)
  - `job.sh` dsub 脚本(`-rpn 608` 占整机)
- 提交: `dsub -s /home/share/suke/hpl-full/job.sh`
- 监控: `dattach -c "top -bn1|head -6" <jobid>`、`tail job_stdout.log`
- SDC 告警: `cat sdc_alerts.log`(出现 FAILED 残差或非零退出码即记录)

## SDC 判据
- HPL 残差校验 FAILED(`||Ax-b||` 超阈值 16.0)→ 强 SDC 信号
- 进程崩溃/非零退出 → 弱信号(可能是 MPI/内存问题,需复跑定位)
- 建议连续跑数小时至数天积累统计

## 调试记录(608 rank 大规模启动的三个坑,2026-09-11)
1. **HWLOC sysfs 扫描串行化**:HyperMPI(OpenMPI 4.1.6)内嵌 hwloc 在每个 rank
   的 MPI_Init 里扫 `/sys/bus/cpu/devices/cpu*/cache|topology`(608 CPU × ~15 文件)。
   608 rank 并发 → kernfs 全局锁 → 全体 D 状态、CPU 90% idle。
   修复:空闲节点上 `lstopo-no-graphics --of xml --allow all --no-io --no-angles`
   生成 topo.xml,运行时 `HWLOC_XMLFILE` 直接加载。
2. **HWLOC_XMLFILE + OMPI bind 冲突**:XML 加载的拓扑 is_thissystem=0,
   OMPI 拒绝绑定("does NOT support binding")。
   修复:`mpirun --bind-to none` + 每 rank 经 wrapper 脚本
   `taskset -c $OMPI_COMM_WORLD_RANK ./xhpl` 内核级钉核(还能穿过 isolcpus)。
3. **staging 目录**:608 rank 并发 open NFS 上的库文件也会串行化,一切运行时
   文件(xhpl、topo.xml、wrapper、HPL.dat、libgfortran)先 cp 到 /tmp(tmpfs)。
   注意 wrapper 脚本也要 stage,否则 mpirun 找不到可执行文件(报 SIGILL/132)。
