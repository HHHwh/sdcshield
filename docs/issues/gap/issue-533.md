# Issue #533 解决方案文档：ci-triage main 遗留红两档

- Issue：[#533](https://github.com/wangxumarshall/sdcshield/issues/533)（2026-11-08 立案，来源 PR #532 终检归因）
- 本文核查日期：2026-11-18（核查基线 = main `727cc8d3`；所有行号、run id、commit 均为当日实测/实查）

## 一、结论先行

**两档现状均已不可观察——被一个更早阶段的第三档（lint/codespell 全红）掩盖；该第三档已于
2026-11-17 在 main 修复（`681e8631`），但此后没有任何 PR 上下文 run 发生，两档原始红是否
仍在，处于「未观察、未验证」状态。** 在立案时点（11-08）确曾双红；11-09 起 Build binary 档
已连续转绿（环境性 flake 的实证），Multi-version×3 档持续红到 11-10（此后被掩盖）。
同族兄弟档 Multi-OS Verify（main 夜间 schedule，#429 系）已于 11-17 根修转绿。

## 二、现状核查（2026-11-18 实测证据链）

### 2.1 立案时点签名（11-08，run 42479086021，PR #532 时代）

- **Build binary for CPU (Linux)**：失败 step = `run tests quickly`（本仓
  `.github/workflows/pr.yaml:115`）。日志实证（GHA job log 摘录）：

  ```text
  command-line: 'sdcshield --quick -n4 --retest-on-failure=0 --disable eigen_svd_double ... -o quick.yaml'
  tests:
  - test: low_load_cache_pressure
    exit: invalid
  ##[error]Process completed with exit code 2.
  ```

  与 issue 正文「low_load_cache_pressure{,_neon}/neon_memcpy_cache 族环境性 crash」一致。

- **Multi-version ×3**（20.03-LTS / 22.03-SP3 / 24.03-SP3 三个 matrix 腿全部失败）：
  失败 step = `Build image + source + smoke-verify (corner three)`（`pr.yaml:192`）。
  日志实证：构建阶段全过（`[3/5] meson setup`→`[4/5] ninja`→`[5/5] 功能验证`→
  `容器内构建完成` 均打印），随后 `RESULT: FAIL openEuler-22.03LTS_SP3 (source build)`
  （发出点 `scripts/offline-build/build-all.sh:151-153`，即 `container-build.sh` 非零退出）。
  「功能验证」的内容是 `--list-tests >100` 闸门 + `-e zstd19 -t 2000 -n 1`
  （`scripts/offline-build/container-build.sh:336-341`）；smoke 闸门本体
  （`verify-built-pristine.sh:64-71`，list-tests>100 + zstd19 `exit: pass`）在其后，未到达。
  容器内详细输出在 GHA 日志中被折叠，**失败的具体行（too-few-tests 还是 zstd19 非 pass）
  未取证**——与 issue「未取证」的自认一致。

### 2.2 演化时间线（run 级实测）

| 窗口（UTC） | Build binary 档 | Multi-version×3 档 | 备注 |
|---|---|---|---|
| 11-08 11:37–12:15 | **红**（low_load_cache_pressure exit: invalid） | **红**（×3） | #533 签名；42476562930/42479086021/42479361188 |
| 11-09 01:28 | 红（42545765482） | 红 | |
| 11-09 02:05 → 11-10 08:19 | **连续绿**（42548981618…42781695122 共 8 个 run，全部 success） | **持续红**（×3，全部走到 smoke step） | tier-a 不再复现——「环境性」定性成立 |
| 11-10 → 11-17 03:14 | **skipped**（0s） | **skipped**（0s） | 第三档掩盖：lint/codespell 全红（含 docs-only PR #629），两 job `needs: [lint, git-sanity]`（`pr.yaml:63,172`）不启动 |
| 11-17 03:38 后 | 无 run | 无 run | 最后一次 pr.yaml run = 44008722743（PR #815，codespell 红） |

### 2.3 第三档（掩盖者）根因与修复——已闭环

- 症状：11-10 起所有 PR 的 lint job 在 `Run codespell` step（`pr.yaml:34-36`，`pipx install
  codespell && codespell`）以 exit 65 挂掉，包括不可能引入拼写问题的 docs-only PR #629。
- 根因：检入工件 `tools/fp64_rowmajor_ko/verify.log:47` 的 `parm`（内核 modinfo 字段名）被
  codespell 2.4.3 字典命中。**本机已复现**：codespell 2.4.3（本机版本 = CI pipx 装的 latest）
  在 PR #815 head 树上实跑输出 `./tools/fp64_rowmajor_ko/verify.log:47: parm ==> param, pram,
  parma`、exit 65；lint job checkout 的是 `github.sha`（PR head，`pr.yaml:25-29`），每个 PR
  head 都含 main 的该文件 → 全红。
- 修复：main `681e8631`（2026-11-17 19:18 +0800，"codespell 排除 fp64_rowmajor_ko/verify.log
  运行时日志工件 — 11-10 起 lint 全红解锁"）在 `.codespellrc` skip 列表加
  `*fp64_rowmajor_ko/verify.log`。**本机实跑当前 main 树：0 findings，exit 0。**
- 同日 main 还有 Multi-OS Verify 族根修系列（`0c36ac75`/`336ed169`/`f256f1f6` full-log 契约、
  `d7c93c4a` verify-params 矩阵 `-O mdim=200` 钉维、`d50ceee0` 阶段超时 900→3600s +
  stage_rc 残卷硬闸 + acl_gemm_sve 补钉）。Multi-OS Verify（main schedule）最新 run
  44084195392（11-17 12:33 UTC 完成）**绿**——#429 系问题闭环。

## 三、根因（文件:行号汇总）

1. **掩盖档（已修）**：`tools/fp64_rowmajor_ko/verify.log:47`（触发词）× `.github/workflows/pr.yaml:34-36`
   （codespell 全树扫描、无版本锁定）→ 修复 commit `681e8631`。
2. **tier-a quick-suite crash（11-08 红、11-09 起不复现）**：测试本体
   `tests/cpu/neon_memcpy_cache/low_load_cache_pressure.cpp`（及 `_neon` 孪生）在 hosted
   runner 上 `exit: invalid`；本机 pass。无修复落地（该文件 11-09 后最后一次相关改动是
   `c51a739b`，非针对此 crash）。quick step 的 SIGXCPU 类已知问题另案已修
   （`pr.yaml:117-127` ulimit -St 600，#369），与本 crash 不同类（exit code 2 非 24）。
3. **tier-b Multi-version smoke（11-08→11-10 持续红）**：`scripts/offline-build/build-all.sh:151-153`
   报出的 `container-build.sh` 非零退出；功能验证内容在 `container-build.sh:336-341`；
   具体失败行未取证（容器内输出被 GHA 折叠）。是否与 11-17 Multi-OS 族根修同根，未判定。

## 四、建议修复路径

按序执行，第一步是唯一解锁动作：

1. **重新暴露两档（必做先行）**：让任一 open PR 的 head 含 `681e8631` 之后的 main
   （rebase 或 merge main 进 PR 分支后 push），触发 pull_request run。lint 预期绿
   （本机 codespell 已验证 main 树干净），`Build binary` 与 `Multi-version×3` 两 job 随之
   恢复执行——这一步把「未观察」变成「可观察」。
2. **tier-a 若复现**：二选一。
   - 快道：把 `low_load_cache_pressure`、`low_load_cache_pressure_neon` 加入 quick step
     的 hosted-runner 禁用清单（`pr.yaml:136-143`，现有 `--disable 'mesh_upi*'` 等先例；
     注释引用 #533 与「本机 pass、hosted-runner 崩」定性）。禁用是坦诚的资源边界声明，
     不是修复——夜间全量（Multi-OS Verify all-quality）仍覆盖它们。
   - 根修：拿到 crash 现场。runner 无 GDB（bats 日志实证 `crash backtrace # skip No GDB
     in this installation`），需在 PR 临时加 coredump 上传（`ulimit -c unlimited` +
     actions/upload-artifact）或在本机以等价资源约束（`-n 4`、cgroup cpu 限额、与 runner
     同规格 4 vCPU）复跑 `--quick -n4` 逼出 exit: invalid 的信号/退出码，再定位。
3. **tier-b 若复现**：先取证再修。当前 `container-build.sh` 的容器内输出在 GHA 折叠，
   建议在其 `podman run` 调用处加 `--log-level=debug` 或把 inner script 输出 tee 到
   step summary；拿到具体失败行后按三候选（quick-suite 同因 / `--list-tests` 退出码 /
   镜像因素）归因。若与 11-17 已修的 verify-params/超时/紧凑日志同根，直接验证收敛；
   若独立，按证据修 `verify-built-pristine.sh`/镜像。
4. **归因机制沉淀（流程项）**：`scripts/task_loop.py:842`（`scan_red_runs`）只扫 main 分支
   schedule/push 事件的 4 个受监控 workflow（`task_loop.py:71`、`194-196`：Multi-OS
   Verify/Sanitizer/CodeQL/Security）——**PR 上下文的红不在其监控域**，#533 式「终检归因
   不计 PR 自身」目前是人工判断。建议把该边界写进 `docs/build-deploy-pr-ci/ci-triage.md`
   （PR 红 = PR-loop 职责，main 红 = task-loop 职责），避免下一个 PR 期 CI 红再被误归因。

## 五、验证边界

| 项 | 验证方式 | 状态 |
|---|---|---|
| codespell 档根因 | 本机 codespell 2.4.3 实跑 PR #815 head（exit 65、单条 finding）与当前 main（0 findings） | **已验证（真实命令输出）** |
| codespell 档 CI 侧转绿 | 需任一含 `681e8631` 的 PR head 触发 run，lint job 绿 | 未验证（无 run 发生）——预期绿 |
| tier-a 现状 | 需同上 run 的 `run tests quickly` step 结果 | 未观察。历史证据：11-09→11-10 连续 8 run 绿 |
| tier-b 现状 | 需同上 run 的 Multi-version×3 结果 | 未观察。历史证据：11-08→11-10 全红；容器内失败行未取证 |
| Multi-OS Verify（#429 族，main） | run 44084195392 绿（11-17 12:33 UTC） | 已验证（真实 run 记录） |
| Multi-version 档本机复现 | 本机无 podman/skopeo（实测 `which podman` 为空），离线容器链无法本机跑 | **本机边界**：只能在 GHA hosted arm64 runner 或装有 podman 的主机验证 |

诚实边界声明：截至本文，两档原始红的「是否仍存在」没有新证据；本文全部结论基于历史 run
日志、main 当前树与本机 codespell 复现，不含任何对未发生 run 的推断性判定。
