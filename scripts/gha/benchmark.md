# 跨 OS 基准采集设计说明

脚本本体见 [`benchmark.sh`](benchmark.sh)（`bash benchmark.sh <bin> <outdir>`），本文说明口径与依据。

## 为什么用固定 loop 数而不是固定墙钟

GHA 的 `ubuntu-24.04-arm` 是**共享 aarch64 云实例**（AWS Graviton 系），不是用户的 kunpeng920 实机。
若用固定墙钟（`-t 30s`），同一测试在不同 OS 上会因 CPU 频率/邻居负载差异跑到**不同迭代数**，
跨 OS 不可比。改用 `--max-test-loop-count N` 让每个 OS 上每个测试跑**恰好 N 次 main loop**，
工作量一致，只比"完成同样工作量的墙钟"（YAML `test-runtime`，秒）。

## 口径声明（诚实边界）

- **相对近似值**：报告的是同架构、同工作量下每个测试的墙钟，作为跨 openEuler 版本
  （gcc-7/10/12、glibc 2.28/2.34/2.38、内核差异）的性能**相对**参考。
- **非绝对对比**：不与 x86、也不与用户本地 kunpeng920 实机做绝对对比（宿主不同，结果不可复现）。
- **固定环境**：所有 OS 跑在同一类 runner 上，已隔离"机器差异"这一最大混淆因子。

## 基准测试集（三系列交集，2026-09-17 探针验证 20.03/22.03/24.03 皆有）

| test | 检测域 | loop 数 |
|---|---|---|
| openblas_dgemm | NEON FMA GEMM（double） | 2 |
| openblas_sgemm | NEON FMA GEMM（float） | 2 |
| zstd | 压缩-解压（默认档） | 1 |
| zlib | 压缩-解压（zlib 默认档） | 1 |
| fma | FMA 算术 | 2 |
| crc32 | CRC32 指令 | 2 |
| pocketfft_fft | 复数 FFT | 2 |
| memcpy_l2_cache_size | L2 带宽 memcpy | 2 |
| eigen_gemm_double_dynamic_square | Eigen GEMM | 2 |
| gmp_bignum | 大整数 | 2 |
| openssl_sha | SHA-256/384/512 | 2 |

> 选**默认压缩档** `zstd`(~80s/loop)/`zlib`(~177s/loop)而非最高档 `zstd19`(700s+)/`zlib1`(168s)，
> 保证基准时长可控；单 loop 值 2026-09-17 本地 kunpeng920 实机量取，GHA 云实例会更快。

## 输出

`<outdir>/benchmark.tsv`：首行表头 `test<TAB>wall_seconds<TAB>loop_count` 是三方契约
（`benchmark.sh` 写、`benchmark-gate.py` 与 `benchmark-to-json.py` 校验，格式演进须三处
同步）；数据行 `wall_seconds` 三态：

| 值 | 含义 | 闸门（[`benchmark-gate.py`](benchmark-gate.py)，verify 与 benchmark-trend 共用） |
|---|---|---|
| 有限数值 | 数据行（目标测试 `result: pass` 且取到它自己的 `test-runtime`，`float` 可解析且 `math.isfinite`） | 进 JSON/趋势 |
| `skip` | 运行时跳过（`result: skip`） | `::warning::` 点名，数据点缺失 |
| `fail` | 在列但未产出可用数据（进程失败 / 测试 fail / 字段缺失 / 非有限值） | verify 红 |

失败现场留档：每测试 `stdout/stderr` 存 `<outdir>/<t>.log`、运行 YAML 存
`<outdir>/<t>.yaml`（均随 artifact 上传），失败摘要（退出码 + 末尾 15 行）同时进
`benchmark.log`。`--list-tests` 捕获的有效性 = 退出码 0 且非空且 **BENCH_TESTS 交集
完整**（`grep -qxF` 字面匹配；测试名含正则元字符时 `-x` 的 BRE 解释会误判定）；
任一不满足即重试一次，连续失败中止采集（stderr 留档 `list-tests.err`）——run
40177906937（2026-10-25）24.03-LTS-SP2 的 `memcpy_l2_cache_size` 曾因该捕获一次性
部分输出被误记「缺席」（同 job allquality 全量扫描里该测试存在且 pass，此前两天同
OS 同测试均为正常数值；该次 rc 已不可考，`rc=0` 部分输出变体无法被「rc≠0 或空表」
拦截，故补交集完整性校验）。BENCH_TESTS 声明为跨三系列交集，正常应全部在列（3
run × 15 OS = 495 行实证：唯一一次缺席就是上述事故）——缺席即异常，中止而非降级
`skip`。

此基准 tsv 仍上传进 artifact，但不再进最终 report summary（最终 summary 由
`report-summary.py` 生成「用例 × 版本」结果矩阵，见 `report-summary.py` 注释）。