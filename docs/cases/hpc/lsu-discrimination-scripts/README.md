# §7.4 load/store 通路判别脚本(CORE145 SDC 诊断)

本目录脚本为 `HPC-CORE145-SDC-DIAGNOSIS-REPORT.md` §7.4( LSU 假说的微架构判别)的全部数据来源。运行前提:日志文件位于 `/tmp/hpc_log.txt`(即 `Linsheng 10.39.0.1_2026-09-11_6_42_28.log` 的本地副本)。除 `f9norm2.pl`(纯位运算自包含)与 `lsu_sides.pl`(结论打印)外,其余脚本以日志路径为第一参数或内含 `/tmp/hpc_log.txt` 路径。

| 脚本 | 对应报告结论 | 关键输出 |
|---|---|---|
| `f8divisor2.pl` | §7.4.1 判别一前置 | FAIL9 col234: exact-2^-15 count=600, not-exact=0 |
| `f9expdiv2.pl` | §7.4.1 判别一 | actual exp == golden exp - 15: ok=600 bad=0;golden 类 0x10:307/0x11:292/0x01:1 |
| `f9xorfinal.pl` | §7.4.1 判别一(决定性) | 类 0x00:293/0x01:307;固定 XOR 预测 +17/+15 分裂;实测统一 +15;307 一致/293 矛盾;尾数/符号 bad=0 |
| `f9c0ex.pl` | §7.4.1 判别一(类 0x00 进位实例) | 0x3e8+15=0x3f7(进位)vs XOR→0x3f9(+17) |
| `f9norm.pl` | §7.4.1 判别二 | z=0.22652905637574997;max|x|² 占比 0.077636699… |
| `f9norm2.pl` | §7.4.1 判别三 | 0x3fc XOR 0x3de = 0x022,popcount=2 |
| `lsu_divisor3.pl` | §7.4.2(FAIL8) | col38 k=24145.4913385 (2.67e-10);col39 k=0.248376594746 (2.13e-10);col11 k=5997.17513441 (2.11e-06) |
| `lsu_sides.pl` | §7.4.3 | P(12/12 golden 侧)= 0.000244 = 1/4096 |
| `lsu_gemmcount.pl` | §7.4.4 | 同窗(10:41–10:51)GEMM 299/0、sparse 9/0、svd 家族 167/0、svd_sve 41/12/1 |
| `lsu_summary_data.pl` | §7.4 汇总 | [1]-[4] 证据块 |

注意:`f9norm2.pl` 与 `lsu_sides.pl` 无输入参数;`f9norm2.pl` 输出中 "one-element corruption bound |k-1| <= ~0.078" 为判别二的界。`f9norm.pl` 的旧版 popcount 行(打印 "0 bits")为已知笔误,以 `f9norm2.pl` 的显式逐位循环结果(popcount=2)为准——该修正已在报告中注明脚本来源为 `f9norm2.pl`。

*所有输出均可在保留原始日志副本的环境中复现;报告中的每一个数值都对应上述某个脚本的一次实际运行。*
