# 项目内 ninja 构建并行度上限（nproc×50%）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 sdcshield 仓库内建立统一构建入口 `scripts/build.sh`，把 ninja 默认并行度从"核数+2"限制为"nproc×50%（下限 1）"，并让仓库所有本地构建脚本与文档全部走该入口。

**Architecture:** 新增一个薄 wrapper 脚本 `scripts/build.sh`（默认注入 `-C builddir -j nproc/2`，显式 `-j`/`SDC_BUILD_JOBS` 可覆盖，make jobserver 让路）；三个仓库构建脚本改为调用它；活文档（CLAUDE.md/README/运行手册等 13 处）的 `ninja -C builddir` 统一替换为 `./scripts/build.sh`。不改任何 C/C++/meson 代码，不改 CI（GitHub Actions 隔离 runner）。

**Tech Stack:** POSIX 兼容 bash 脚本；ninja ≥1.8；无新依赖。

**Spec:** 无独立 spec 文档。需求为用户 2026-10-21 指令："在本项目中做限制"——ninja 默认并行度 = CPU 核数的 50%。背景论证见下节（本会话实测）。

## 背景与论证（2026-10-21 本机实测）

- 上游 ninja 默认并行度 = 逻辑 CPU 数 + 2（2011-02-01 commit `0d0f186` 引入 `GuessParallelism()`，0/1 核→2、2 核→3、≥3 核→N+2，15 年未变；源码无注释解释 +2 动机）。
- 本机（共享服务器，鲲鹏 920）：`nproc`=96 → ninja 默认 `-j98`。98 路并行 g++（Eigen 重模板，单进程峰值数百 MB）内存+CPU 双峰值打满，会拖垮同机在跑的 192-CPU 压测与其他用户。
- `NINJAFLAGS` 实测只认 `-v` 不认 `-j`（3 组计时实验），环境变量方案不可行 → 必须 wrapper。
- meson 无默认 -j 选项、build.ninja 无法表达默认并行度 → 项目级只能靠"统一入口脚本 + 文档规范"。
- 用户级 `~/.local/bin/ninja` wrapper（本会话早前已装）与本方案正交组合：`build.sh` 注入显式 `-j` 后 wrapper 直接透传，无双重干预。

## Global Constraints

- 50% 定义：`nproc`（调用环境可见逻辑核数）/2，**下限 1**（`-j 0` 在 ninja 里 = 无限并行，必须钳位）。
- 并行度优先级：显式 `-j` 参数 > `SDC_BUILD_JOBS` 环境变量 > nproc/2。
- make jobserver 场景（`MAKEFLAGS` 含 jobserver）不注入 `-j`（ninja ≥1.12 自动用 jobserver 池，显式 -j 会禁用它）。
- **x86-64 不受影响**（CLAUDE.md 规则 5）：纯脚本+文档改动，无 arch 代码；x86 路径同样可用该入口（nproc 自适应）。
- **CI（`.github/workflows/`、`.github/actions/`）不改**：GitHub Actions 隔离 runner 非共享服务器，capping 只会拖慢 PR 验证（4 核 runner 会变 -j2）。`codeql.yaml` 已自用 `-j2`（内存原因），不动。
- 历史归档不改写：`docs/superpowers/plans|output/` 旧文件、`docs/cases/`、`docs/SDC_STRESS_CONFIG_CN.md:290`（历史验证记录）、纯散文提及（`scripts/lts-stability/README.md:70`、`deploy_81machine.sh:150`、`sdc_machine_scan.sh:146`）。
- 每个 commit：`git commit -s`（DCO，Signed-off-by: wangxu <wangxumarshall@qq.com>，最后一行，无 Co-Authored-By），先 `git branch --show-current` 确认在 `feat/build-jobs-cap`，逐个 verify→commit→push，绝不碰 `main`。
- 工作区有他人未跟踪文件（tests/、t2.pid 等）：**只 add 本任务明确列出的文件**，严禁 `git add -A`。

## Review Focus

1. **nproc=1（单核机/taskset 限制）→ j=0 → `-j 0` = 无限并行**：必须钳位到 1。测试：`taskset -c 0` + 4 任务 scratch 目录 → ~4s（Task 1 Step 6）。
2. **`SDC_BUILD_JOBS` 非数字**：必须显式报错 exit 2，不能静默当 0 或字符串传入 ninja。测试：Task 1 Step 7。
3. **显式 `-j` 被覆盖**：必须原样透传。测试：Task 1 Step 5（~1s 对照）。
4. **用户 `-C` 与注入 `-C` 冲突**：用户显式 `-C` 时不重复注入。测试：Task 1 Steps 3-5 全部用 `-C /tmp/...` 跑通即证明。
5. **jobserver 场景注入 -j**：会破坏 `make -jN` 内嵌 ninja 的池化。测试：Task 1 Step 8。

---

### Task 1: `scripts/build.sh`——受限并行度统一构建入口

**Files:**
- Create: `scripts/build.sh`（可执行，`chmod +x`）
- Create: `docs/superpowers/plans/2026-10-21-build-jobs-cap.md`（本文件，先入库）

**Interfaces:**
- Consumes: PATH 中的 `ninja`（≥1.8）、`nproc`。
- Produces: `scripts/build.sh [ninja参数...]`——Task 2 三个脚本以 `build.sh -C <dir>` 调用；Task 3 文档以 `./scripts/build.sh` 引用。退出码 = ninja 退出码（参数错误时 2）。

- [ ] **Step 1: 建分支并提交 plan 文档**

```bash
cd /home/sdc/wangxu/code-review/sdcshield
git branch --show-current        # 必须先确认；当前在 main
git checkout -b feat/build-jobs-cap
git add docs/superpowers/plans/2026-10-21-build-jobs-cap.md
git commit -s -m "docs(plan): 构建并行度上限(nproc×50%)统一入口 plan 入库"
git push -u origin feat/build-jobs-cap
```

- [ ] **Step 2: 写 `scripts/build.sh`（完整内容如下）**

```bash
#!/bin/bash
# build.sh — SDCShield 规范构建入口（并行度上限保护）
#
# 为什么存在: 上游 ninja 默认并行度 = 逻辑 CPU 数 + 2（2011-02-01 ninja commit
# 0d0f186 的 GuessParallelism(): 0/1 核→2、2 核→3、≥3 核→N+2, 沿用至今, 源码无
# 注释解释动机）。本仓库主要在共享服务器（鲲鹏 920, 96 逻辑核）上构建与压测,
# 98 路并行 g++（Eigen 重模板, 单进程峰值数百 MB）内存/CPU 双峰值会打满机器,
# 拖垮同机在跑的压测。故项目内构建统一走本入口, 默认并行度 = nproc × 50%。
#
# 用法（在仓库根目录执行; builddir 需先 meson setup, 见 README）:
#   ./scripts/build.sh                 # 等价 ninja -C builddir -j $(( $(nproc)/2 ))
#   ./scripts/build.sh unittests       # 追加 ninja 目标/参数
#   ./scripts/build.sh -C other-dir    # 自定义构建目录（显式 -C 时不重复注入）
#   ./scripts/build.sh -j 8            # 显式 -j 优先
#
# 并行度优先级: 显式 -j 参数 > SDC_BUILD_JOBS 环境变量 > nproc/2（下限 1;
# -j 0 在 ninja 里 = 无限并行, 故必须钳位）。make jobserver 场景（MAKEFLAGS 含
# jobserver, 如 make -jN 内嵌调用）让路给 jobserver 池, 不注入 -j。
# CI（GitHub Actions 隔离 runner）与历史归档文档不经本入口, 不受影响。

set -euo pipefail

inject_C=1
inject_j=1
for a in "$@"; do
    case "$a" in
    -C|-C*) inject_C=0 ;;
    -j*|--jobs=*) inject_j=0 ;;
    esac
done

# ninja >= 1.12 检测到 MAKEFLAGS 带 jobserver 时自动用其池; 显式 -j 会禁用
case "${MAKEFLAGS:-}" in
*jobserver*) inject_j=0 ;;
esac

if [ "$inject_j" = 1 ]; then
    j="${SDC_BUILD_JOBS:-}"
    if [ -z "$j" ]; then
        j=$(( $(nproc 2>/dev/null || echo 4) / 2 ))
    fi
    case "$j" in
    ''|*[!0-9]*) echo "scripts/build.sh: 无效并行度 '$j'（SDC_BUILD_JOBS 需为正整数）" >&2; exit 2 ;;
    esac
    [ "$j" -ge 1 ] || j=1
    set -- -j "$j" "$@"
fi

if [ "$inject_C" = 1 ]; then
    set -- -C builddir "$@"
fi

exec ninja "$@"
```

```bash
chmod +x scripts/build.sh
bash -n scripts/build.sh && echo "syntax OK"
```

- [ ] **Step 3: 搭建计时测试场（96 个 sleep-1 任务 + 4 任务小场）**

```bash
mkdir -p /tmp/buildsh-test && cd /tmp/buildsh-test
{ echo 'rule sleep1'; echo '  command = sleep 1'; for i in $(seq 0 95); do echo "build t$i: sleep1"; done; } > build.ninja
mkdir -p /tmp/buildsh-test-small && cd /tmp/buildsh-test-small
{ echo 'rule sleep1'; echo '  command = sleep 1'; for i in $(seq 0 3); do echo "build s$i: sleep1"; done; } > build.ninja
```

（输出文件永不生成 → 每次全量重跑，可反复计时。）

- [ ] **Step 4: 默认并行度 = 48（96 任务 → 2 波 → ~2s）**

```bash
cd /home/sdc/wangxu/code-review/sdcshield
time ./scripts/build.sh -C /tmp/buildsh-test
```
Expected: `real ~2.0-2.4s`（96/48=2 波）；对照 `time ninja -C /tmp/buildsh-test -j 96` 应为 `~1.0-1.4s`（证明 2s 确实来自 -j48 封顶）。

- [ ] **Step 5: 显式 -j 透传（Review Focus 3/4）**

```bash
time ./scripts/build.sh -C /tmp/buildsh-test -j 96
```
Expected: `real ~1.0-1.4s`（显式 -j 覆盖默认 48；同时证明用户 -C 被尊重、无双重注入）。

- [ ] **Step 6: SDC_BUILD_JOBS=32（96 任务 → 3 波 → ~3s）**

```bash
time env SDC_BUILD_JOBS=32 ./scripts/build.sh -C /tmp/buildsh-test
```
Expected: `real ~3.0-3.4s`。

- [ ] **Step 7: 单核钳位与非法值（Review Focus 1/2）**

```bash
time taskset -c 0 ./scripts/build.sh -C /tmp/buildsh-test-small   # nproc=1 → -j1 → 4 波
env SDC_BUILD_JOBS=abc ./scripts/build.sh -C /tmp/buildsh-test-small; echo "exit=$?"
```
Expected: 第一条 `real ~4s`（-j1，绝不许 `-j 0`）；第二条 stderr 出现 `无效并行度 'abc'` 且 `exit=2`。

- [ ] **Step 8: jobserver 让路（Review Focus 5）**

```bash
time env MAKEFLAGS='--jobserver-auth=3,4 -j' ./scripts/build.sh -C /tmp/buildsh-test
```
Expected: ninja 打印 jobserver 相关 warning（pipe 协议不支持→忽略）后正常构建，`real ~1.0-1.4s`（未注入 -j，走 ninja 自身默认 98）。若该版本 warning 行为不同，如实记录输出并分析，不得略过。

- [ ] **Step 9: 真实 builddir 增量构建**

```bash
./scripts/build.sh
```
Expected: 进入 builddir，`no work to do` 或少量重编，**零新告警**。粘贴真实输出。

- [ ] **Step 10: Commit**

```bash
git add scripts/build.sh
git commit -s -m "feat(build): scripts/build.sh 统一构建入口, ninja 默认并行度 nproc/2

上游 ninja 默认 = 核数+2(2011-02-01 0d0f186 GuessParallelism, 15 年未变),
96 核共享服务器上 -j98 的 Eigen 重模板编译内存/CPU 双峰值会拖垮在跑压测。
默认注入 -C builddir -j nproc/2(下限 1); 显式 -j > SDC_BUILD_JOBS > nproc/2;
make jobserver 让路。NINJAFLAGS 实测不认 -j, 故必须 wrapper。"
git push
```

### Task 2: 三个仓库构建脚本接入 build.sh

**Files:**
- Modify: `scripts/build-fuzzing.sh:26`
- Modify: `scripts/offline-build/build.sh:38-39`
- Modify: `scripts/offline-build/container-build.sh:331-332`

**Interfaces:**
- Consumes: Task 1 的 `scripts/build.sh -C <dir>` 契约（显式 -C 时不重复注入 -C builddir；显式 -j 透传）。

- [ ] **Step 1: `scripts/build-fuzzing.sh`（afl 模糊构建）**

在第 22 行 `target_dir=$1` 前加 script_dir 解析，替换第 26 行：

```bash
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
```
旧: `CC=afl-gcc CXX=afl-g++ ninja -C $target_dir`
新:
```bash
# 统一走受限并行度入口（默认 -j = nproc/2; 显式 -j/SDC_BUILD_JOBS 可覆盖）
CC=afl-gcc CXX=afl-g++ "$script_dir/build.sh" -C "$target_dir"
```

- [ ] **Step 2: `scripts/offline-build/build.sh`（宿主构建+自验证）**

旧 38-39 行:
```bash
echo "==> ninja..."
ninja -C "$BUILD"
```
新:
```bash
echo "==> ninja..."
# 统一走受限并行度入口（默认 -j = nproc/2, 共享服务器保护; 显式 -j/SDC_BUILD_JOBS 可覆盖）
"$SCRIPT_DIR/../build.sh" -C "$BUILD"
```
（`SCRIPT_DIR` 已在 13 行定义 = scripts/offline-build。）

- [ ] **Step 3: `scripts/offline-build/container-build.sh`（容器内构建）**

旧 331-332 行:
```bash
echo "===== [4/5] ninja ====="
ninja -C "$BUILD/builddir"
```
新:
```bash
echo "===== [4/5] ninja ====="
# 统一走受限并行度入口（默认 -j = nproc/2; 容器内 nproc 即容器可见核数）
"$SRCW/scripts/build.sh" -C "$BUILD/builddir"
```
（`SRCW="$BUILD/src"` 于 175 行定义，`cp -a "$SRC"` 时 scripts/ 一并拷入容器。）

- [ ] **Step 4: 语法检查三脚本**

```bash
bash -n scripts/build-fuzzing.sh && bash -n scripts/offline-build/build.sh && bash -n scripts/offline-build/container-build.sh && echo "syntax OK"
```

- [ ] **Step 5: 端到端跑 `scripts/offline-build/build.sh`（真实输出）**

```bash
./scripts/offline-build/build.sh
```
Expected: 打印 openEuler 版本 → meson reconfigure → ninja（经 build.sh, 增量）→ `测试数: ~440` → zstd19 单线程出现 `exit`/`result` 行（pass）。全程零新告警。粘贴关键行。

- [ ] **Step 6: 边界诚实声明（build-fuzzing / container-build 无法本机端到端）**

`build-fuzzing.sh` 需 afl-gcc（本机未装，脚本 17-20 行会提前退出）；`container-build.sh` 需 podman 全容器重建（负载过重）。两者验证边界 = `bash -n` + 逐行核对改动（改动仅为把 ninja 调用换成 build.sh 同参调用）。在 commit message 与最终报告中如实声明。

- [ ] **Step 7: Commit**

```bash
git add scripts/build-fuzzing.sh scripts/offline-build/build.sh scripts/offline-build/container-build.sh
git commit -s -m "build: 仓库构建脚本统一接入 scripts/build.sh 并行度上限

build-fuzzing / offline-build build.sh / container-build 三处 ninja 调用改为
受限入口。offline-build/build.sh 已端到端实测; 另两处因 afl-gcc/podman 依赖
本机不可用, 验证边界为 bash -n + 逐行核对（同参替换, 无行为差异除 -j 封顶）。"
git push
```

### Task 3: 活文档统一改用 ./scripts/build.sh

**Files（13 处，全部是机械替换 `ninja -C builddir` → `./scripts/build.sh`，保持其余文字不变）:**

| # | 文件:行 | 说明 |
|---|---|---|
| 1 | `CLAUDE.md:15` | 规则 2(a) 自验证命令 |
| 2 | `CLAUDE.md:62` | Build & run 代码块（加一行注释说明默认 -j） |
| 3 | `README.md:156` | 快速开始 |
| 4 | `README.md:242` | openssl 验证行 |
| 5 | `README.md:264` | 首次构建顺序 |
| 6 | `docs/research/usage.md:11,15` | 两处 |
| 7 | `docs/research/adding-tests.md:55,177` | 两处 |
| 8 | `.github/PULL_REQUEST_TEMPLATE.md:7` | 贡献者本地验证清单 |
| 9 | `docs/build-deploy-pr-ci/agent-full-automation.md:9` | 验证门描述 |
| 10 | `docs/build-deploy-pr-ci/offline-build-dependencies.md:137,153` | 两处 |
| 11 | `docs/sdc-excite-reproduce/operations-runbook.md:476` | 路径 A 构建命令 |
| 12 | `docs/sdc-excite-reproduce/sdc-excite-reproduce-7x24.md:1631` | 路径 B 构建命令 |
| 13 | `scripts/sdc-excite-reproduce/NEW_BOARD_ONBOARDING.md:39` | 新板入职 |

**Interfaces:**
- Consumes: Task 1 的 `./scripts/build.sh` 命令形态。

- [ ] **Step 1: 逐文件替换（Edit 精确匹配，先 Read 再改）**

CLAUDE.md:62 代码块替换示例（其余 12 处只换命令本身）:

旧:
```
ninja -C builddir
```
新:
```
./scripts/build.sh      # ninja -j = nproc/2 by default (shared-server cap)
```

CLAUDE.md:15: `` `ninja -C builddir` clean`` → `` `./scripts/build.sh` clean``

- [ ] **Step 2: README 构建节加 1 行说明（在快速开始代码块后）**

```markdown
> 构建统一走 `./scripts/build.sh`：默认 `-j` = `nproc`/2（共享服务器保护；ninja 上游默认是核数+2）。显式 `-j` 或 `SDC_BUILD_JOBS` 可覆盖，详见 `scripts/build.sh` 头注释。
```

- [ ] **Step 3: grep 清扫验证**

```bash
git grep -n 'ninja -C builddir' -- CLAUDE.md README.md docs/research/ docs/build-deploy-pr-ci/ docs/sdc-excite-reproduce/ scripts/sdc-excite-reproduce/NEW_BOARD_ONBOARDING.md .github/PULL_REQUEST_TEMPLATE.md
```
Expected: **零输出**（历史归档 docs/superpowers/、docs/cases/、SDC_STRESS_CONFIG_CN 不在清单内，允许保留）。

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md README.md docs/research/usage.md docs/research/adding-tests.md .github/PULL_REQUEST_TEMPLATE.md docs/build-deploy-pr-ci/agent-full-automation.md docs/build-deploy-pr-ci/offline-build-dependencies.md docs/sdc-excite-reproduce/operations-runbook.md docs/sdc-excite-reproduce/sdc-excite-reproduce-7x24.md scripts/sdc-excite-reproduce/NEW_BOARD_ONBOARDING.md
git commit -s -m "docs: 构建文档统一改用 ./scripts/build.sh（并行度上限 nproc/2）

CLAUDE.md/README/usage/adding-tests/PR 模板/自动化设计/离线依赖/运维手册/
7x24 手册/新板入职共 13 处活文档; 历史归档(plans/output/cases/压力配置
历史记录)与 CI(隔离 runner)不改。"
git push
```

### Task 4: 收尾全量验证（CLAUDE.md 规则 2）

**Files:** 无新改动（只验证）。

- [ ] **Step 1: 强制真实编译一个 TU**

```bash
touch tests/cpu/compression/zstd19.cpp   # 若路径不同, git grep -l zstd19 tests/ 找到
./scripts/build.sh 2>&1 | tail -5
```
Expected: 重编 zstd19 相关目标并链接，**零告警**。粘贴真实输出。

- [ ] **Step 2: 回归**

```bash
./builddir/sdcshield -e zstd19 -t 2000 -n 1 2>&1 | tail -5
```
Expected: `exit: pass`。

- [ ] **Step 3: x86-64 非回归（inspection）**

```bash
git diff main --stat
```
Expected: 只有 scripts/ 与 docs/ 与计划文档，无任何 C/C++/meson/x86 代码 → 架构无关，规则 5 满足。

- [ ] **Step 4: 最终推送确认**

```bash
git log --oneline main..feat/build-jobs-cap
git status        # 他人未跟踪文件原样保留, 未被 add
git push
```

---

## Self-Review

1. **Spec 覆盖**：需求"项目内 ninja 默认并行度 = 核数 50%" → Task 1 建入口（默认值+优先级+钳位）、Task 2 仓库脚本接入、Task 3 文档规范传播、Task 4 全量验证。CI 排除已在 Global Constraints 论证。无缺口。
2. **占位符扫描**：无 TBD/TODO；build.sh 全文给出；三脚本给出精确旧行/新行；文档 13 处逐一列出。
3. **类型/命名一致性**：`SDC_BUILD_JOBS`、`inject_C/inject_j`、`script_dir/SCRIPT_DIR/SRCW` 各任务内一致；Task 2 调用形态 `build.sh -C <dir>` 与 Task 1 契约一致。
4. **Review Focus**：5 条均在 Task 1 Steps 5-8 有对应测试。
