# Multi-OS Verify eigen 9 用例 CI timeout 修复（-O 维度钉小）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 Multi-OS Verify 全量参数扫描中 9 个 eigen 用例在 4 核 CI runner 的 `--timeout=60s` 预算内真实 pass（当前 `result: timed out` = 零验证覆盖），**零源码改动**（用户明确要求避免改源码），仅改 `scripts/gha/verify-params.py` 注入显式 `-O` 维度。

**Architecture:** 根因是两个预算口径错位：CI 的 `--timeout=60s` 是每测试**整体墙钟**上限（`framework/sandstone.cpp:179` `test_timeout()`：显式 override 时直接取该值，覆盖默认 max(5×-t+30s, 300s)），而 eigen 家族自适应 sizing（`sdc_joint_dim_slice`）的时间目标是**每迭代 200s**（`run_budget_ms=200000`，sparse 为 144000，按生产 300s 硬看门狗口径标定）。4 核 runner 上 `-n 1`（AR=1）联合求解选出的大维度**单迭代就超 60s**。修复 = verify-params.py 的 COMMON 注入 9 个显式 `-O <test>.<knob>=<cap>`——走框架 T11 显式 knob 早退（`framework/sdc_stress_config.h:1380`）完全旁路 sizing 与试解探针，档位按 run 44011713221 五个 24.03 镜像的 CI 实测试解幂律取 ≥4× 裕度。COMMON 同时覆盖 all-quality(-n1) 与 all-PROD n1/4/8 两个超时现场。

**Tech Stack:** Python3 纯 stdlib（verify-params.py 现有契约不变）；sdcshield 现有 `-O/--test-option` 机制（可重复：`framework/sandstone_opts.cpp:479` `add_to_map_as_vec`；`set_knob_from_key_value_string` 按字符串存 map，未运行的测试的 knob 是惰性条目，不报错）与 T11 knob 早退。

**Spec:** 本计划 Background 节即根因证据链（2026-11-17 会话从 run 44011713221 五镜像 artifact 实证），无独立 spec 文档。

## Background: 根因证据链（已实证，勿重复调查）

- 现场：run 44011713221（main @ 98286caa，2026-11-17）全部 **5 个 24.03 镜像**（LTS/SP1/SP2/SP3/SP4）的 allquality.yaml 超时集合**逐字一致**：`eigen_svd_jacobi`、`eigen_svd_jacobi_double`、`eigen_svd_jacobi_cdouble`、`eigen_svd_jacobi_fvectors`、`eigen_sparse`、`eigen_gemm_float_dynamic_square`(+`_dump`)、`eigen_gemm_cdouble_dynamic_square`(+`_dump`)，全部 `result: timed out`、`loop-count: 0`、`E> Child did not exit, sending signal SIGQUIT`（soft，被 `--ignore-timeout` 容忍不红，但 CI 上零覆盖）。20.03/22.03 十镜像红在 **Build** 步骤（#661 `-march=armv9-a+sve2`，另案，不在本计划范围）。
- **逐用例 sizing 现场**（24.03-LTS-SP3 allquality，`-n 1`，`[联合求解]` 告警行）：

| 测试 | knob(clamp 区间) | CI 自适应选出 | 选出机制 | CI 试解幂律 | 单迭代耗时估算 |
|---|---|---|---|---|---|
| eigen_svd_jacobi | mdim [64,2048] | 1934 | 时间受限 n_run | 256→176.9ms, 512→1552.6ms, k=3.13 | ~200s（按设计） |
| eigen_svd_jacobi_double | mdim [64,2048] | 1646 | n_run | 256→303.6ms, 512→2629.5ms, k=3.11 | ~200s |
| eigen_svd_jacobi_cdouble | mdim [64,2048] | 1019 | n_run | 128→165.6ms, 256→1406.1ms, k=3.09 | ~200s |
| eigen_svd_jacobi_fvectors | mdim [64,2048] | 1932 | n_run | 256→176.8ms, 512→1553.5ms, k=3.14 | ~200s |
| eigen_sparse | n [64,8192] | 8192 | hi 封顶 | 1024→104.7ms, 2048→761.1ms, k=3.00 | ~97s（构造+golden 解+DUT 解） |
| eigen_gemm_float_dynamic_square(+dump) | mdim [16,8192] | 8192 | hi 封顶 | 2048→424.8ms, 4096→3245.0ms, k=3.00 | ~54s |
| eigen_gemm_cdouble_dynamic_square(+dump) | mdim [16,4096] | 4096 | hi 封顶 | 1024→403.1ms, 2048→3105.7ms, k=3.00 | ~50s（实测 60s 内 loop-count 0，utime 59.3s） |

- **机制解读**：4 个 Jacobi 是**时间受限档**——`n_run` 的设计目标就是每迭代 200s（`run_budget_ms / (ar×2)`，`-n 1` 时 AR=1），**任何主机上 `--timeout=60s` 都必死**（设计口径 vs CI 口径错位，非 sizing bug）；sparse/float-gemm/cdouble-gemm 是 **hi 封顶**——4 核 runner 内存充裕（n_mem 13019-35435）但每解成本在 hi 档已达 25-48s，首迭代（构造/golden + DUT 两次求解）> 60s。
- `--max-test-loop-count 3` + `-t 5000`：`test_time_condition()`（`framework/sandstone_run.cpp:633`）在 3 次内循环或 5s 墙钟先到即退——**pass 只需 preamble(构造+golden)+首个 DUT 迭代 < 60s**。
- `-O` 机制：可重复（vector option）；`set_knob_from_key_value_string`（`framework/test_knobs.cpp:183`）仅存字符串，阶段里未运行/被 `--disable` 的测试的 knob 惰性无害；显式值经 `sdc_knob_int_clamped` 钳到各测试 [lo,hi]（下表 cap 全部在区间内）；T11 早退打 `[欠压力]` 预期内存警告（`c_bytes×n² < 50% 预算`——cap 档必然触发），这是框架诚实披露，属预期日志噪声。
- prod_n1 阶段同款超时（4 个 gemm + 3 个 openblas）；prod_n4/n8 阶段这 4 个 gemm auto 档 23s 通过。openblas_n1 三例与 acl_gemm_sve_n4 一例是**范围外遗留**（见 Review Focus 5），本计划只修任务界定的 9 个 eigen。
- 旁证（run 43863891250，20.03 job）：20.03 试解（1024→420.9ms, 2048→3294.0ms）与 24.03 慢 ~7%——cap 裕度覆盖编译器代差。

## 修复设计：pin 档位表（核心决策，逐项给出依据）

档位原则：按 CI 实测试解幂律外推，**首迭代（preamble+DUT1）≤ ~13s、全程 ≤ ~13s**，对 60s 预算 ≥4.5× 墙钟裕度；对超时档（hi/n_run）单解成本 **8-32× 缩小**；全部落在 knob clamp 区间；与历史硬编码同数量级（jacobi 256 / sparse 256 / gemm 221 的老 CI 曾全绿）。

| `-O` 键 | cap | 依据（CI 试解外推单解耗时@cap） | 预计 CI 全程 |
|---|---|---|---|
| eigen_svd_jacobi.mdim | 512 | 1.55s/SVD；首迭代 2 解+fill≈3.1s | ~6-7s |
| eigen_svd_jacobi_double.mdim | 512 | 2.63s/SVD；首迭代≈5.3s | ~9s |
| eigen_svd_jacobi_cdouble.mdim | 256 | 1.41s/SVD；首迭代≈2.8s | ~5-6s |
| eigen_svd_jacobi_fvectors.mdim | 512 | 1.55s/SVD；首迭代≈3.1s | ~6-7s |
| eigen_sparse.n | 4096 | 6.1s/解；构造+golden+DUT≈12.5s | ~13-19s |
| eigen_gemm_float_dynamic_square.mdim | 4096 | fill+1乘 3.2s；init 1乘+preamble 2乘+DUT 1乘 | ~10-13s |
| eigen_gemm_float_dynamic_square_dump.mdim | 4096 | 同上（dump 变体 pass 路径无额外开销） | ~10-13s |
| eigen_gemm_cdouble_dynamic_square.mdim | 2048 | fill+1乘 3.1s | ~10-13s |
| eigen_gemm_cdouble_dynamic_square_dump.mdim | 2048 | 同上 | ~10-13s |

实现形态：`verify-params.py` 顶部（`CLOUD_DISABLE` 之后）加 `EIGEN_CI_DIMS` dict + 展开为 `EIGEN_DIM_PINS`（`["-O", "<key>=<cap>", ...]`），`COMMON` 追加 `+ EIGEN_DIM_PINS`；注释块写根因一行版 + 指向本 plan。COMMON 被 allquality 与全部 prod_n 阶段共用——一处注入覆盖两个超时现场；smoke（zstd19 only）与 openblas mdim 扫谱（自带 -O）不经 COMMON，零影响。

## Global Constraints

- **零源码改动**（用户指示；rule 5 自然满足）：只改 `scripts/gha/verify-params.py` + 新增本 plan 文档；不碰任何 `.cpp/.h`、workflow YAML、`--full-log` 相关行（#815 已合并修复，勿动）。
- One patch per unit：plan 文档一个 commit（`docs(plan): ...`，先例 0d1c0191），verify-params.py 修复一个 commit。
- verify-params.py 保持纯 stdlib（容器内无 PyYAML 契约）。
- DCO：`git commit -s`，末行 `Signed-off-by: wangxu <wangxumarshall@qq.com>`，其后无内容、无 Co-Authored-By。
- 分支 `fix/eigen-ci-timeout`（从 main @ 98286caa 切出）；push 该分支后 `gh pr create` 到 main；**不 merge 自己的 PR**；提交前 `git branch --show-current`。
- 自验证 100% 真实：改前本机复现 + 改后真实输出 + 回归 `-e zstd19 -t 2000 -n 1` → `exit: pass`；x86 非回归按检查（verify-params.py 仅 multi-os-verify.yml 消费，arm64 container 矩阵；`-O` 机制 arch 无关）。

## Review Focus

1. **cap 出框架 clamp 区间会被静默钳改**（`sdc_knob_int_clamped` 钳到 [lo,hi]，不报错）→ 上表 9 个 cap 均已人工核对在区间内；改后验证步骤用 YAML 中 `[欠压力] ... 显式 <cap>` 警告行断言实际生效值逐字等于表值。
2. **COMMON 共享使 prod_n4/n8 阶段也被钉小**（现 auto 档 23s pass → 钉后 ~13s pass）→ CI 软覆盖口径下可接受（多线程档仍真跑真比对，只是维度恒定）；PR 正文披露该口径变化。若审阅者认为 n4/n8 应保留 auto，可改为只在 allquality 与 n1 注入——本计划选 COMMON 统一（简单、一处覆盖 prod_n1 同款超时）。
3. **[欠压力] 警告是预期噪声**（显式 knob 的框架诚实披露，cap 档必触发）→ 不是错误、不进门禁（summarize 只认 fail/crash）；PR 披露，防止后续维护者误当 bug。
4. **本机 128 核 ≠ CI 4 核的验证边界**（gem5-fi 规则不适用：无芯片能力门控，9 个用例本机全部原生可跑）→ 显式 knob 旁路 sizing 后，CI 与本机的唯一差异是**固定维度下的单解速度**，CI 侧预测直接锚定 CI 实测试解数据（≥4.5× 裕度）；本机改前/改后复现证机制；**终验 = 合并后次日 nightly run 5 个 24.03 镜像**，PR 正文如实写明。
5. **范围外遗留需披露**（不修、防"修好了 CI"误读）：prod_n1 的 openblas_dgemm/zgemm/cgemm 三例同型超时（auto sizing 大维度；mdim 扫谱阶段 64/256/512 全 pass 证 knob 通道可治，建议后续同法钉小）；prod_n4 的 acl_gemm_sve（无 SVE runner 上未按预期 SKIP 而是超时，疑似缺 arm64 门控，需单独查证）；20.03/22.03 Build 红（#661 另案）。

---

### Task 1: 建分支 + plan 文档落盘

**Files:**
- Create: `docs/superpowers/plans/2026-11-17-eigen-ci-timeout-fix.md`（本文件）
- 无代码改动

**Interfaces:**
- Produces: 分支 `fix/eigen-ci-timeout`（基于 main @ 98286caa）；本 plan 作为 Task 2 的档位表依据。

- [ ] **Step 1: 建分支并确认**

Run: `git -C /home/sdc/wangxu/sdcshield-sve2 checkout -b fix/eigen-ci-timeout && git -C /home/sdc/wangxu/sdcshield-sve2 branch --show-current`
Expected: `fix/eigen-ci-timeout`（工作树干净；若脏先核对属主再决定 stash）。

- [ ] **Step 2: 提交 plan 文档**

```bash
git add docs/superpowers/plans/2026-11-17-eigen-ci-timeout-fix.md
git commit -s -m "docs(plan): multi-os verify eigen 9 用例 60s 超时根因与 -O 钉维修复计划"
```
Expected: commit 末行 `Signed-off-by: wangxu <wangxumarshall@qq.com>`（`git log -1 --format=%B` 核对）。

### Task 2: verify-params.py 注入 EIGEN_CI_DIMS（修复本体）

**Files:**
- Modify: `scripts/gha/verify-params.py`（`CLOUD_DISABLE` 定义之后 ~:73 加 `EIGEN_CI_DIMS`+`EIGEN_DIM_PINS`；`COMMON` 列表 ~:223 追加 `+ EIGEN_DIM_PINS`；COMMON 注释块补一行指向）

**Interfaces:**
- Consumes: `builddir/sdcshield`（main 树已建好；`-O`/`--test-list-file` 现有 CLI 机制）。
- Produces: all-quality(-n1) 与 all-PROD n1/4/8 阶段的命令行各带 9 对 `-O` 参数；9 个 eigen 用例维度恒定（旁路 sizing 与试解探针）。

- [ ] **Step 1: 改前本机复现（rule 2 取证）**

先写测试清单 `/tmp/eigen9.txt`（9 行：`eigen_svd_jacobi` / `eigen_svd_jacobi_double` / `eigen_svd_jacobi_cdouble` / `eigen_svd_jacobi_fvectors` / `eigen_sparse` / `eigen_gemm_float_dynamic_square` / `eigen_gemm_float_dynamic_square_dump` / `eigen_gemm_cdouble_dynamic_square` / `eigen_gemm_cdouble_dynamic_square_dump`），然后按 CI allquality 阶段**逐字同款参数**本机跑一遍（无 pin）：

```bash
builddir/sdcshield --quality=-1 -n 1 -t 5000 -vvv --full-log --retest-on-failure=0 \
  --max-test-loop-count 3 --timeout=60s --max-messages 0 --ignore-timeout \
  --disable mesh_upi* --disable memcpy_rewr --test-list-file /tmp/eigen9.txt \
  -o /tmp/eigen-repro-before.yaml
```

Expected（预期 ~10-12 min，后台跑）：9 个条目 `result: timed out`。4 个 Jacobi 由设计性质保证必超时（时间受限档 ~200s/迭代 vs 60s 墙钟，与主机无关）；5 个 hi 封顶件在本机同样走 hi 封顶（`MemAvailable≈56GiB` → n_mem 远超 hi）→ 预期同超时；若个别在本机通过（单核速度差异），如实记录并在 PR 披露——CI 侧证据（5 镜像一致超时）独立成立。同时从 YAML 引每个用例的 `[联合求解] ... → <dim>` 行对照 Background 表（本机选出维度应与 CI 同为 hi/n_run 档；n_run 因本机测速而异属预期）。

- [ ] **Step 2: 施改**

在 `CLOUD_DISABLE = [...]` 之后加（档位表与根因见本 plan；注释写根因一行版）：

```python
# CI 维度钉小(run 44011713221 根因, 见 docs/superpowers/plans/2026-11-17-eigen-ci-timeout-fix.md):
# --timeout=60s 是每测试整体墙钟预算, 而 eigen 自适应 sizing 的设计目标是每迭代
# 200s(sparse 144s, 按 300s 硬看门狗标定) → 4 核 CI runner 上联合求解选出的大维度
# 单迭代即超 60s: Jacobi 4 件套走时间受限档 n_run≈1019-1934(设计 200s/迭代, 任何
# 主机必死), sparse/float-gemm 被 hi 封顶 8192, cdouble-gemm 封顶 4096。显式 -O
# 钉维走 knob 早退完全旁路 sizing 与试解探针; 档位按 CI 实测试解幂律(k≈3.0-3.14)
# 取 ≥4.5× 墙钟裕度。[欠压力] 警告是显式 knob 的框架诚实披露, 属预期噪声。
EIGEN_CI_DIMS = {
    "eigen_svd_jacobi.mdim": 512,
    "eigen_svd_jacobi_double.mdim": 512,
    "eigen_svd_jacobi_cdouble.mdim": 256,
    "eigen_svd_jacobi_fvectors.mdim": 512,
    "eigen_sparse.n": 4096,
    "eigen_gemm_float_dynamic_square.mdim": 4096,
    "eigen_gemm_float_dynamic_square_dump.mdim": 4096,
    "eigen_gemm_cdouble_dynamic_square.mdim": 2048,
    "eigen_gemm_cdouble_dynamic_square_dump.mdim": 2048,
}
EIGEN_DIM_PINS = []
for _k, _v in EIGEN_CI_DIMS.items():
    EIGEN_DIM_PINS += ["-O", f"{_k}={_v}"]
```

`COMMON` 改为 `COMMON = [ ...原样... ] + EIGEN_DIM_PINS`，并在 COMMON 注释块补一行：「+ EIGEN_DIM_PINS: 上述 9 用例 CI 钉维(根因/档位依据见 EIGEN_CI_DIMS 注释与 plan 文档)」。

- [ ] **Step 3: 脚本级验证**

Run: `python3 -m py_compile scripts/gha/verify-params.py` → 无输出（语法过）。
Run: `python3 -c "import sys; sys.path.insert(0, 'scripts/gha'); import verify-params"` 不合法（模块名带连字符），改用 importlib：

```bash
python3 -c "
import importlib.util
spec = importlib.util.spec_from_file_location('vp', 'scripts/gha/verify-params.py')
vp = importlib.util.module_from_spec(spec); spec.loader.exec_module(vp)
assert len(vp.EIGEN_DIM_PINS) == 18, len(vp.EIGEN_DIM_PINS)
assert all(x in vp.COMMON for x in vp.EIGEN_DIM_PINS)
print('COMMON pins OK:', ' '.join(vp.EIGEN_DIM_PINS[::2]))"
```

Expected: `COMMON pins OK: <9 个 -O 键串>`。

- [ ] **Step 4: 用例级改后验证（CI 口径同款命令 + pin）**

同一命令加 9 对 `-O`（即 verify-params.py 修改后实际构造出的 argv）：

```bash
builddir/sdcshield --quality=-1 -n 1 -t 5000 -vvv --full-log --retest-on-failure=0 \
  --max-test-loop-count 3 --timeout=60s --max-messages 0 --ignore-timeout \
  --disable mesh_upi* --disable memcpy_rewr \
  -O eigen_svd_jacobi.mdim=512 -O eigen_svd_jacobi_double.mdim=512 \
  -O eigen_svd_jacobi_cdouble.mdim=256 -O eigen_svd_jacobi_fvectors.mdim=512 \
  -O eigen_sparse.n=4096 -O eigen_gemm_float_dynamic_square.mdim=4096 \
  -O eigen_gemm_float_dynamic_square_dump.mdim=4096 \
  -O eigen_gemm_cdouble_dynamic_square.mdim=2048 \
  -O eigen_gemm_cdouble_dynamic_square_dump.mdim=2048 \
  --test-list-file /tmp/eigen9.txt -o /tmp/eigen-repro-after.yaml
```

Expected: 9 个条目全部 `result: pass` 且 `test-runtime` 本机实测记录（引每个数值）；YAML 内每用例含 `[欠压力] <test>.<knob>: 显式 <cap> ...` 行——逐字断言 9 个实际生效维度 == 档位表（防 clamp 静默改值）。另断言 YAML 中**无** `[时间校准]`/`[联合求解]` 行（旁路 sizing 的直接证据）。

- [ ] **Step 5: 端到端 + 回归**

Run: `python3 -u scripts/gha/verify-params.py --bin $(pwd)/builddir/sdcshield --out /tmp/vp-smoke --smoke`
Expected: 末行 `RESULT: PASS`（脚本整体不破；smoke 不经 COMMON，纯完整性哨兵）。
Run: `builddir/sdcshield -e zstd19 -t 2000 -n 1`（末行 grep `exit:`）
Expected: `exit: pass`（回归）。
x86 非回归（检查）：`grep -rn verify-params .github/` 仅 multi-os-verify.yml（arm64 container）消费；零 C++/workflow 改动。

- [ ] **Step 6: Commit**

```bash
git add scripts/gha/verify-params.py
git commit -s -m "fix(gha): verify-params.py 为 9 个 eigen 用例注入 -O 钉维 — 60s/测试 CI 预算 vs 200s/迭代 sizing 设计口径错位"
```
提交说明正文含：根因两行（60s 墙钟 vs 200s 迭代设计）、5 镜像一致超时证据、档位依据、改前/改后本机实测引文、[欠压力] 噪声披露、验证边界（终验 = 合并后 nightly）。

### Task 3: 推送 + PR + 披露

- [ ] **Step 1:** `git push -u origin fix/eigen-ci-timeout`（沙箱走 HTTPS origin；push 前 `git branch --show-current` 确认）。
- [ ] **Step 2:** `gh pr create` 到 main，标题 `fix(gha): eigen 9 用例 CI 60s timeout — -O 钉维(零源码改动)`。正文必含：
  - 根因：逐用例表（Background 表精简版）+ 机制（test_timeout override vs run_budget_ms 200s 设计；Jacobi 时间受限档任何主机必死 / 5 个 hi 封顶件单迭代 25-97s）；
  - 实证：run 44011713221 5 镜像一致超时集合 + 本机改前 9×timed out / 改后 9×pass 实测 runtime 引文 + `[欠压力] 显式 <cap>` 生效断言；
  - 口径披露：prod_n4/n8 的 4 个 gemm 也被钉小（auto 23s → ~13s，软覆盖可接受）；[欠压力] 警告为预期噪声；
  - 范围外遗留：openblas_n1 三例（同型，建议后续同法）、acl_gemm_sve_n4（疑缺 SVE 门控需查证）、#661 Build 红另案；
  - 验证边界：本机 128 核 vs CI 4 核——pin 旁路 sizing 后唯一差异是固定维度单解速度，CI 预测锚定 CI 实测试解数据（≥4.5× 裕度）；**终验 = 合并后次日 nightly 5 个 24.03 镜像 allquality 9 例全 pass**；
  - 修后 PR CI（pr.yaml 自身）预期绿。
- [ ] **Step 3:** 报告用户：根因结论（逐用例）、最终方案、每 commit 验证输出引文、PR 链接、未尽事项与边界。
