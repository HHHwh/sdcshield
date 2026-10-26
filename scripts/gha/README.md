# scripts/gha — GitHub Actions 多 OS 验证脚本

[`.github/workflows/multi-os-verify.yml`](../../.github/workflows/multi-os-verify.yml) 的配套脚本。
全部**纯 stdlib**（无 PyYAML / 无第三方依赖），因为三个系列的 openEuler 构建镜像 RPM 树都没有 PyYAML。

## 文件

| 文件 | 作用 |
|---|---|
| `verify-params.py` | 全量参数功能测试（`--quality=-1` 全覆盖、`-n 1/4/8` 三档、openblas `mdim` 扫谱、selftests 正负集），末行 `RESULT: PASS|FAIL` |
| `benchmark.sh` | 固定 `--max-test-loop-count` 采集跨 OS 基准 → `benchmark.tsv`（`wall_seconds` 三态：数值/`skip`(运行时跳过)/`fail`(在列未出数据)；`--list-tests` 捕获须 BENCH_TESTS 交集完整否则重试/中止；失败现场按测试留档 `.yaml`/`.log`） |
| `benchmark-gate.py` | benchmark.tsv 状态分类单一权威（verify/trend 共用）：ok=可 float 解析、skip=缺席或运行时跳过、fail=在列但未出数据 |
| `benchmark.md` | 基准口径说明（为何固定 loop 数、诚实边界） |
| `report-summary.py` | `report` job 汇总 15 份 `allquality.yaml` 成「用例 × 版本」结果矩阵（有基线时每格附耗时 Δ） |
| `package-built.sh` | 把 CI 构建的二进制打成自包含 built/ tarball（bin+libs+脚本+元数据），上传为 artifact |

## report-summary.py（最终 report 输出）

`report` job 下载 15 份 `allquality.yaml`（每镜像 `--quality=-1` 全质量级跑一轮的产物，含全部
PROD+BETA+SKIP 用例），生成一张 **「用例 × 版本」矩阵**写入 job summary：

- 行 = 测试用例（15 版本求并集，~290 行），列 = 15 个 OS 版本（20.03/22.03/24.03 × LTS+SP1~SP4）。
- 每格 = `<结果态>[<耗时>s]`：
  - `PASS[1.23s]` 执行正确
  - `FAIL[0.10s]` 报错
  - `SKIP[0.00s]` 跳过
  - `TIMEOUT[60.0s]` 超时、`CRASH[0.0s]` 崩溃、`OSERR[..]` 系统错误、`INTERRUPTED[..]` 中断、`INVALID[..]` 无效
  - 空 = 该版本未编译出该用例（如 22.03/20.03 无 `sleef_neon`）
- **耗时 Δ（可选 `--previous DIR`）**：`report` job 先用 `gh run download` 拉上次成功
  全量运行的 `verify-*` artifact（候选 = 本 workflow 最近 8 个 success run，新到旧排除
  自身；接受第一个目录树里含 `allquality.yaml` 的——smoke 运行只有 `smoke_zstd19.yaml`，
  天然被排除；7 天 retention 之外的下载失败自动试下一个），再以 `--previous
  prev-all-results` 调用本脚本。此时每格升级为 `<结果态>[<耗时>s (±Δs)]`，
  Δ = 相对上次成功全量运行**同用例同版本**的耗时变化（增加为正，**仅对比耗时**，
  结果态 PASS/FAIL/... 本身不参与对比）。当前有而基线无的用例（基线版本未编译出）
  维持旧格式；基线缺失（7 天内无成功全量运行）时打印说明行、降级为不带 Δ 的旧格式。
  不带 `--previous` 时输出与旧版**逐字节一致**（pr.yaml 的 quick summary 无参复用本脚本）。
- 矩阵尾部附一张「结果态统计（单元格计数）」表。
- 纯 stdlib，逐行流式解析（每份 ~1.4MB × 15 = ~20MB，不整文件入内存）。

> 取代旧的 `benchmark-summary.py`（原「跨 OS 基准墙钟对比表」）。`benchmark.sh` 采集的
> `benchmark.tsv` 仍上传进 artifact，只是不再进最终 summary（如需可另起一个 step 展示）。

## verify-params.py 验证矩阵

| 阶段 | 内容 | 判定 |
|---|---|---|
| 枚举 | `--list-tests` / `--quality=-1` / `--selftests` | 计数 >100 |
| all-quality(-n1) | `--quality=-1 -n 1 -t 5000` 全质量级跑一轮 | 0 fail 且 0 crash |
| all-PROD -n {1,4,8} | 多线程并发档，`--disable` eigen 数值类 | 0 fail 且 0 crash |
| selftest @positive | `--selftests --quick -e @positive` | 退出 0 |
| selftest 负面集 | 逐条 `selftest_*fail/abort/sig*` | 非零退出且非 insn 崩溃 |
| openblas mdim | `-O <id>.mdim={64,256,512}` × 3 gemm | 0 fail 且 0 crash |

**用法**：
```bash
# 容器内，镜像已装依赖 + 已 git clone + 已构建 builddir/sdcshield
python3 scripts/gha/verify-params.py --bin "$(pwd)/builddir/sdcshield" --out "$(pwd)/results"
# 快档
python3 scripts/gha/verify-params.py --bin ... --out ... --smoke
```

**已知省略（诚实，非缺陷）**：`sleef_neon`/`sleef_sve` 在无 cmake 的镜像里优雅缺席；`eigen` 数值类在
`-n>1` 档被 `--disable`（规避 CLAUDE.md 记录的 192 核 ULP flakiness），在 all-quality(-n1) 单线程档覆盖。

## benchmark.sh 用法

```bash
bash scripts/gha/benchmark.sh "$(pwd)/builddir/sdcshield" "$(pwd)/benchmark"
# → benchmark/benchmark.tsv: test<TAB>wall_seconds<TAB>loop_count
#   wall_seconds ∈ { 数值, skip(运行时跳过), fail(在列未出数据) }
#   (--list-tests 捕获无效/交集不完整 → 重试一次,连续失败中止)
# → 每测试 benchmark/<t>.yaml|<t>.log 留档;闸门判定用 benchmark-gate.py
```

固定 `--max-test-loop-count`（同工作量墙钟对比），只取三系列交集测试。口径与边界见 `benchmark.md`。
## package-built.sh（每次 CI 构建后的自包含产物）

workflow 的 `Package built/` step 调用：把刚构建的 `builddir/sdcshield` 打成与本地
podman 链路 `third-party/rpms/.../built/` 结构一致的 tarball，上传为 artifact
`built-<series>-<sp>`（保留 90 天）。随后 `Package smoke-verify` step 解开 tarball、
只设 `LD_LIBRARY_PATH=libs` 实跑（list-tests >100 + zstd19 exit:pass 双断言）——
容器内形态的 `verify-built-pristine.sh smoke`，产物闸门不静默降级。

```bash
# 容器内（原生 container 模式，ldd/strip 直跑，无需 podman）
bash scripts/gha/package-built.sh builddir dist/openEuler-24.03LTS_SP3 24.03 SP3
# → dist/openEuler-24.03LTS_SP3/sdcshield-openEuler-24.03LTS_SP3-<sha8>.tar.gz (~4MB)
```

产物结构：`sdcshield`（stripped）+ `libs/` + `run-sdcshield.sh` + `MANIFEST.tsv` +
`VERSION` + `BUILD-HASH`。

**libs 收集规则**（与本地 `package-built-artifacts.sh` 判定对齐）：
- 20.03：ldd 输出的 `/opt/openEuler/*` toolset 库全拷（`cp -L` 解引用，soname 与
  real file 双名字落盘）；
- 22.03/24.03：恒拷 `libatomic.so.1`（最小 KIWI 目标容器缺，镜像 `/usr/lib64/` 有，
  与本地从 LTS RPM 提取同源同效）；
- 其余（libc/libm/libz/libzstd/libgmp/系统 libstdc++）视为目标机自带不拷；
- ACL 已改 vendored 静态链接（`-l:libarm_compute-core.a`），二进制 ldd 不依赖
  `libarm_compute.so` —— 本地 built/libs 里该文件是老方案遗留死重，本脚本不拷。

**BUILD-HASH 等价锚点**：公式与 `build-all.sh compute_build_hash` 完全一致（源码树
`git ls-tree` 哈希 + `container-build.sh` 哈希 + cpp_std/macro + 镜像 input-hash +
series-sp 拼串 sha256）。同 commit 时 GHA 产物与本地 built/ 的 BUILD-HASH 相等——
两条链路等价的机器可查证明（已实测双向验证）。

**与本地 `package-built-artifacts.sh` 的分工**：本地版跑在构建宿主上，需要 podman
包装进目标镜像做 ldd 探测、从 RPM 树提取 libatomic；GHA 版跑在镜像**内部**（原生
container 模式），ldd/strip 直跑、libatomic 从镜像文件系统直拷，无需 podman 与
RPM 树（GHA 不 checkout rpms 子模块）。产物结构与元数据格式一致。
