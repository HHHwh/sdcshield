# adaptive_l3_sdc 实施计划（2026-11-18）

> 用户指令：上一个自适应（adaptive_mem_sdc，misc/）基于 L1D 紧时序域；
> 这次**基于 L3_cache_920/ 文件夹内测试用例的负载模式**写新的自适应
> 用例——方法源 = /home/sdc/wangxu/sdcshield/tests/cpu/L3_cache_920。
> 依据 [[adaptive-midload]]：中间负载 = ldr 与 str 之间任何保功能可插入
> 操作；组合空间不得被本机（core-179）负结果框死。

**Goal**: 新测试 `adaptive_l3_sdc`（tests/cpu/L3_cache_920/）=
adaptive_mem_sdc 的运行时模型 + L3 域负载模式组合空间。

## 组合空间：7 模式 × 3 数据通路 = 21 组合

| 模式 | 方法源（文件夹内） | 校验点 |
|---|---|---|
| SEQ_READ | mesh read 族 | 共享 4MB + golden sum 每遍重读 |
| RAND_READ | llc_randomized 族 | 动态 L3 窗 + 转发对 + 周期稠密扫 |
| STRIDE_READ | llc_uniform 族 | 128B 步长 + 稠密扫 |
| WRITE | write_only 两遍式 | 写满+golden → 延迟重读复算 |
| COPY | memcpy 族 | 动态 L3 块 + 预污染 + memcmp |
| RMW | llcstress 族 | counts 影子 + 延迟复验扫 |
| FETCH | l3_execute 族 | civac+ivau 重取指 + init 快照 |

× 数据通路 {SCALAR, NEON(u32x4), FP64(f64x2, 0.5 倍数 k 空间精确和)}。

## 运行时模型（照抄 adaptive_mem_sdc 机制，env 前缀 ADAPTIVE_L3_SDC_）

shm 槽位协调 fork 循环子进程 / 墙钟切片轮转 / MODE=combat|test /
VERIFY 轮询 / COVFILE 轨迹 / COMBO 锁定 / ORDER 覆盖 / fail 报告带
组合标签 `[pattern/datapath/几何]`。kPriorityOrder 初值=表序（未标定，
各机自校准——本机预期多零！！！=负空间数据，他机可能是最快臂）。

## 验证门

构建零新警告；`-l` 注册；VERIFY 全 21 组合零失配；逐模式短跑 pass；
zstd19 回归；x86 untouched（aarch64 门控）。完成后本会话审阅提交推送。
