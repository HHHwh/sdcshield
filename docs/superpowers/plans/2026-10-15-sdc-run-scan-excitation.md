# sdc-run.sh scan 逐核定位 + 本机 SDC 激发加压战役 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 创建 `scripts/run/sdc-run.sh` 统一入口（`scan` 子命令做逐核 SDC 定位分析），并在本机（TaiShan 2280, Kunpeng 920, 128 核/126 在线, NEON-only 无 SVE, 无 root）启动多手段 SDC 激发加压战役，持续记录分析异常直至捕获类型 SDC 完整信息。

**Architecture:** 双轨制——(A) 定位轨：`sdc-run.sh scan` 对每个核单独运行 SDC 敏感用例集（单核 `--cpuset` 绑定 + 静默邻居），产出 core×test 矩阵与异常报告，失败对自动确认复测区分瞬态/持续；(B) 激发轨：复用仓库既有 7×24 战役引擎 `sdc-excite-reproduce.sh full`（L0–L5 阶段状态机 + stress-ng 伴层 + 热联锁），机器空闲时先跑基线 scan，再启动长驻战役。

**Tech Stack:** Bash（仓库脚本惯例：fail-loud、usage 写文件头）、python3（矩阵解析）、sdcshield 框架（`--cpuset/-Y/--quality/--retest-on-failure/--on-crash`）、nohup 后台长驻进程。

**Spec:** 本计划即 spec（目标来自 /goal 指令，无独立 spec 文档）。相关背景：`docs/sdc-excite-reproduce/operations-runbook.md`（M5 运维手册）、`scripts/run/sdc_detect.sh`（二进制发现与优先桶惯例）、`scripts/run/run_cluster_anchored.sh`（簇锚定编排）、memory `cn23154-numa3-va-path-fault`（机队故障签名）。

## Global Constraints

- 分支 `feat/sdc-run-scan`（从 main 切出），绝不直接提交 main；`git commit -s`，末行 `Signed-off-by: wangxu <wangxumarshall@qq.com>`，无 Co-Authored-By。
- 一个 patch 一个单元；提交前 100% 实证（真实运行输出，非"应该可以"）。
- fail-loud 与占位诚实：二进制缺失/用例名缺失/参数错误必须报错退出，绝不静默空转。
- 本机事实（已实证，2026-10-15）：无 SVE（`lscpu` Flags 无 sve）→ sve* 用例不在默认集；无 root（sudo -n 失败）→ 电压/调频/122,123 上线杠杆不可用，以负载多样性+并发档位+时长驻留补偿并如实记录；cpufreq 存在但锁 performance@2.6GHz；thermal zones = acpitz ×2（50.8/48.2°C）；126 在线核（122,123 离线）。
- `--cpuset` 与 `-n` 不兼容（`--help` 明示）——单核定位用 `--cpuset=<N>`（单 cpu 集合即单线程），绝不同时传 `-n`。
- 框架标志已实证存在：`--retest-on-failure=N`、`--fatal-skips`、`--on-crash=context`、`-Y`(yaml)。
- 默认测试集 12 个名字已对 `./builddir/sdcshield --quality=0 --list-tests` 核实存在。

## Review Focus

- **skip 行解析**：SVE 用例在本机必 skip，YAML 中 skip 的表示（`result: skip` + 原因行）必须被矩阵解析器正确归类为 skip 而非 fail/缺失——冒烟步骤显式包含一个 sve 用例验证。
- **并行实例核重叠**：`--par>1` 时两个实例若绑到同一核会互相污染归因——簇交错调度 + 已用核集合防重。
- **Ctrl-C 半途矩阵**：长扫被中断时已完成核的 YAML 仍在——trap 后仍生成部分矩阵，绝不丢已采数据。
- **离线核进入 --cores all**：`all` 必须只含在线核（122,123 离线），从 `/sys/devices/system/cpu/online` 展开而非 present。
- **duration 带 s 后缀**：`-t` 接受 `5s`/`10` 两种写法，透传时保持原样（框架自身解析），不要自行剥离后缀。

---

### Task 1: Preflight——构建新鲜度与回归门

**Files:** 无新文件（纯验证）。

**Interfaces:**
- Produces: 确认 `./builddir/sdcshield` 与 main 一致可用；为后续任务提供可信二进制。

- [ ] **Step 1: 构建新鲜度**

Run: `cd /home/sdc/wangxu/sdcshield && ninja -C builddir 2>&1 | tail -3`
Expected: `ninja: no work to do` 或重链成功零新警告（若重编译则须重跑 Step 2）。

- [ ] **Step 2: zstd19 回归门（CLAUDE.md 规则 2c）**

Run: `./builddir/sdcshield -e zstd19 -t 2000 -n 1 -o /tmp/preflight-zstd19.yaml -q; echo "exit=$?"; tail -2 /tmp/preflight-zstd19.yaml`
Expected: `exit=0`，末行 `exit: pass`。

- [ ] **Step 3: 单核 cpuset 行为验证（scan 的运行形态）**

Run: `./builddir/sdcshield --cpuset=7 -t 3 -Y -q -e fma -o /tmp/preflight-core7.yaml; echo "exit=$?"; grep -E "^- test:|^  result:" /tmp/preflight-core7.yaml | head -4`
Expected: `exit=0`，fma `result: pass`（单核绑定形态可用）。

### Task 2: 实现 scripts/run/sdc-run.sh（scan 子命令）

**Files:**
- Create: `scripts/run/sdc-run.sh`（可执行）

**Interfaces:**
- Consumes: sdcshield 二进制（`--cpuset=<N> -t <dur> -Y --quality=<n> --retest-on-failure=<n> --on-crash=context --on-hang=kill --ignore-timeout --ignore-os-errors -e <list> -o <file>`）。
- Produces: `scan` 子命令；输出 `<out>/core_<N>.yaml`、`<out>/matrix.tsv`、`<out>/report.md`、`<out>/scan.log`。退出码 0=全 pass/skip（无 fail），1=存在 fail（含确认复测后分类），2=用法/环境错误。

- [ ] **Step 1: 写完整脚本**（代码如下，一次成型；惯例对齐 `sdc_detect.sh`：usage 在文件头注释、二进制发现 fail-loud）

```bash
#!/bin/bash
# ============================================================
# sdc-run.sh — SDC 压测统一入口（scan：逐核定位分析）
#
# 子命令:
#   scan   逐核定位：对 --cores 的每个核单独运行 SDC 敏感用例集
#          （--cpuset=<core> 单核绑定，邻居静默），产出 core×test
#          结果矩阵与异常报告；失败对自动确认复测区分 瞬态/持续。
#
# 用法:
#   bash scripts/run/sdc-run.sh scan [选项]
#     --bin PATH    sdcshield 二进制（缺省: $SDC_BIN → ./builddir/sdcshield
#                   → ./builddir-gcc/sdcshield，找不到 fail-loud 退出）
#     --cores SPEC  核列表: all（缺省=全部在线核）| 0-31 | 0,4,8 | 0-31,64
#     --tests LIST  逗号分隔用例名（缺省 DEFAULT_TESTS；全部须存在于
#                   --list-tests，缺失 fail-loud 列名退出）
#     --t DUR       每用例每核时长，透传 -t（缺省 5s）
#     --par N       并行实例数（缺省 1；实例按簇交错调度避免同簇双载，
#                   仅加速，每核归因不变——同核绝不双实例）
#     --quality N   质量档（缺省 0=PROD+BETA，arm64_sdc 等 BETA 用例需要）
#     --retest N    框架级失败重测（缺省 3，透传 --retest-on-failure）
#     --out DIR     输出根目录（缺省 ./sdc-run-scan-<MMDD-HHMMSS>/）
#
# 产出:
#   <out>/core_<N>.yaml            每核完整 YAML（-Y）
#   <out>/matrix.tsv               core × test 矩阵（pass/fail/skip）
#   <out>/report.md                摘要+异常+确认复测分类
#   <out>/scan.log                 进度日志（每核一行）
#
# 退出码: 0=无 fail；1=存在 fail（含确认分类）；2=用法/环境错误
# 设计依据: docs/superpowers/plans/2026-10-15-sdc-run-scan-excitation.md
# ============================================================
set -euo pipefail
usage() { sed -n '3,32p' "$0" | sed 's/^# \{0,1\}//'; }

DEFAULT_TESTS="eigen_gemm_double_dynamic_square,eigen_svd_double,openblas_dgemm,sleef_neon,fma,power_virus_dit,arm64_sdc,ooo_dep_chain_arm,lsu_store_forward_arm,neon_add,zstd19,openssl_sha"

[ $# -ge 1 ] || { usage; exit 2; }
CMD="$1"; shift
case "$CMD" in
    scan) ;;
    -h|--help|help) usage; exit 0 ;;
    *) echo "FATAL: 未知子命令 '$CMD'（可用: scan）" >&2; exit 2 ;;
esac

BIN_OVERRIDE=""; CORES_SPEC="all"; TESTS="$DEFAULT_TESTS"
T_PER="5s"; PAR=1; QUALITY=0; RETEST=3; OUT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --bin)     BIN_OVERRIDE="$2"; shift 2 ;;
        --cores)   CORES_SPEC="$2"; shift 2 ;;
        --tests)   TESTS="$2"; shift 2 ;;
        --t)       T_PER="$2"; shift 2 ;;
        --par)     PAR="$2"; shift 2 ;;
        --quality) QUALITY="$2"; shift 2 ;;
        --retest)  RETEST="$2"; shift 2 ;;
        --out)     OUT="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "FATAL: scan 未知选项 '$1'" >&2; exit 2 ;;
    esac
done
[ "$PAR" -ge 1 ] 2>/dev/null || { echo "FATAL: --par 须为 >=1 整数" >&2; exit 2; }
[ "$RETEST" -ge 0 ] 2>/dev/null || { echo "FATAL: --retest 须为 >=0 整数" >&2; exit 2; }

# ---------------- 二进制发现（fail-loud，与 sdc_detect.sh 同约定） ----------------
if [ -n "$BIN_OVERRIDE" ]; then BIN="$BIN_OVERRIDE"
else
    BIN="${SDC_BIN:-}"
    for c in ./builddir/sdcshield ./builddir-gcc/sdcshield; do
        [ -x "$c" ] && BIN="$c" && break
    done
fi
[ -n "$BIN" ] && [ -x "$BIN" ] || {
    echo "FATAL: 未找到可执行的 sdcshield（--bin/SDC_BIN/./builddir/./builddir-gcc 均未命中）" >&2
    exit 2; }

# ---------------- 用例名存在性校验（fail-loud） ----------------
TMP_LIST=$(mktemp); trap 'rm -f "$TMP_LIST"' EXIT
"$BIN" --quality="$QUALITY" --list-tests > "$TMP_LIST" 2>/dev/null \
    || { echo "FATAL: --list-tests 执行失败" >&2; exit 2; }
MISSING=$(printf '%s\n' "$TESTS" | tr ',' '\n' | while read -r t; do
    [ -n "$t" ] && ! grep -qx "$t" "$TMP_LIST" && echo "$t"
done)
[ -z "$MISSING" ] || { echo "FATAL: 以下用例不在 --list-tests（--quality=$QUALITY）: $(echo $MISSING | tr '\n' ' ')" >&2; exit 2; }
N_TESTS=$(printf '%s\n' "$TESTS" | tr ',' '\n' | grep -c .)

# ---------------- 核列表展开（all=仅在线核） ----------------
expand_spec() {  # "0-3,7" → 每行一核
    python3 - "$1" <<'PY'
import sys
spec = sys.argv[1].strip()
if spec == "all":
    spec = open("/sys/devices/system/cpu/online").read().strip()
out = []
for part in spec.split(","):
    part = part.strip()
    if not part: continue
    if "-" in part:
        a, b = part.split("-"); out += list(range(int(a), int(b) + 1))
    else:
        out.append(int(part))
online = set()
for part in open("/sys/devices/system/cpu/online").read().strip().split(","):
    if "-" in part:
        a, b = part.split("-"); online.update(range(int(a), int(b) + 1))
    else: online.add(int(part))
bad = [c for c in out if c not in online]
if bad:
    sys.stderr.write("FATAL: 核 %s 不在线\n" % ",".join(map(str, sorted(set(bad))))); sys.exit(1)
seen = set()
for c in out:
    if c not in seen: seen.add(c); print(c)
PY
}
CORES=$(expand_spec "$CORES_SPEC") || exit 2
N_CORES=$(echo "$CORES" | wc -l)

# ---------------- 簇交错重排（--par>1 时各实例分散到不同簇） ----------------
cluster_of() {  # 核→簇代表核（cluster_cpus_list 首核；缺失回退 die，再回退 4 核均匀切）
    local c="$1" cl
    for f in cluster_cpus_list die_cpus_list; do
        cl=$(cat "/sys/devices/system/cpu/cpu$c/$f" 2>/dev/null) || continue
        echo "${cl%%[-,]*}"; return
    done
    echo $(( (10#$c / 4) * 4 ))
}
ORDERED=$(while read -r c; do echo "$(cluster_of "$c") $c"; done <<< "$CORES" \
    | sort -k1,1n -k2,2n | awk '{print $2}' | python3 -c "
import sys
rows = [l.split() for l in sys.stdin if l.split()]  # 已按簇排序: 簇1的核..簇2的核..
# 轮转取行: 同一轮次内每簇至多取一个 → 前 PAR 个实例必不同簇
cols = max(1, ${PAR})
out, i = [], 0
for r in range(cols):
    for row in rows:
        if len(row) > r: out.append(row[r])
print('\n'.join(out))
")

# ---------------- 输出目录 ----------------
[ -n "$OUT" ] || OUT="./sdc-run-scan-$(date +%m%d-%H%M%S)"
mkdir -p "$OUT"
LOG="$OUT/scan.log"
log() { printf '[%s] %s\n' "$(date '+%F %T')" "$*" | tee -a "$LOG"; }
log "scan 开始: cores=$N_CORES tests=$N_TESTS t=$T_PER par=$PAR quality=$QUALITY bin=$BIN out=$OUT"
log "测试集: $TESTS"

# ---------------- 单核执行 ----------------
run_one() {
    local core="$1"
    "$BIN" --cpuset="$core" -t "$T_PER" -Y \
        --quality="$QUALITY" --retest-on-failure="$RETEST" \
        --on-crash=context --on-hang=kill --ignore-timeout --ignore-os-errors \
        -e "$TESTS" -o "$OUT/core_${core}.yaml" \
        > "$OUT/core_${core}.stdout" 2>&1
}
PIDS=()
cleanup() {  # Ctrl-C: 杀子进程，已完成核照常出矩阵（不丢已采数据）
    trap - INT TERM
    for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    build_matrix; generate_report; exit 130
}
trap cleanup INT TERM

# ---------------- 并行调度（已用核集合防重叠） ----------------
DONE=0; STARTED=()
for core in $ORDERED; do
    while [ "$(jobs -rp | wc -l)" -ge "$PAR" ]; do wait -n; done
    run_one "$core" & PIDS+=($!)
    STARTED+=("$core")
    log "核 $core 启动（$((++DONE))/$N_CORES）"
done
wait

# ---------------- 矩阵与报告 ----------------
build_matrix() {
    python3 - "$OUT" "$TESTS" > "$OUT/matrix.tsv" <<'PY'
import sys, glob, re
outdir, tests = sys.argv[1], sys.argv[2].split(",")
cores = []
for f in glob.glob(outdir + "/core_*.yaml"):
    m = re.search(r"core_(\d+)\.yaml$", f)
    if m: cores.append(int(m.group(1)))
cores.sort()
res = {}  # res[test][core] = 'pass'|'fail'|'skip'
for c in cores:
    cur = None
    for line in open("%s/core_%d.yaml" % (outdir, c), errors="replace"):
        m = re.match(r"^- test: (\S+)", line)
        if m: cur = m.group(1); res.setdefault(cur, {})[c] = "no-result"
        m = re.match(r"^  result: (\S+)", line)
        if m and cur: res[cur][c] = m.group(1).rstrip(",")
print("test\t" + "\t".join(map(str, cores)))
for t in tests:
    row = [res.get(t, {}).get(c, "-") for c in cores]
    print(t + "\t" + "\t".join(row))
PY
}
build_matrix

# ---------------- 确认复测（失败对 ×1，静默邻居单跑） ----------------
FAIL_PAIRS=$(python3 - "$OUT" "$TESTS" <<'PY'
import sys, re
outdir, tests = sys.argv[1], sys.argv[2].split(",")
for t in tests:
    for line in open(outdir + "/matrix.tsv"):
        if line.startswith(t + "\t"):
            cells = line.rstrip("\n").split("\t")[1:]
            cores = line.split("\t")[0]  # placeholder, real header parse below
            break
header = None
for line in open(outdir + "/matrix.tsv"):
    if line.startswith("test\t"):
        header = [int(x) for x in line.rstrip("\n").split("\t")[1:]]
        continue
    parts = line.rstrip("\n").split("\t")
    t, cells = parts[0], parts[1:]
    if t not in tests: continue
    for c, r in zip(header, cells):
        if r == "fail":
            print("%s %s" % (c, t))
PY
)
CONFIRM_MD=""
RC=0
if [ -n "$FAIL_PAIRS" ]; then
    log "确认复测 $(echo "$FAIL_PAIRS" | wc -l) 个失败对（单跑 ×1）"
    while read -r core test; do
        [ -z "$test" ] && continue
        "$BIN" --cpuset="$core" -t "$T_PER" -Y --quality="$QUALITY" \
            -o "$OUT/confirm_${core}_${test}.yaml" > /dev/null 2>&1 || true
        CR=$(grep -m1 "^  result:" "$OUT/confirm_${core}_${test}.yaml" | awk '{print $2}' || true)
        if [ "$CR" = "fail" ]; then CLASS="持续(复现)"; else CLASS="瞬态(未复现)"; fi
        CONFIRM_MD="$CONFIRM_MD
| $core | $test | $CR | $CLASS |"
        log "确认核 $core $test → $CR（$CLASS）"
    done <<< "$FAIL_PAIRS"
    RC=1
fi

# ---------------- report.md ----------------
generate_report() {
    python3 - "$OUT" > "$OUT/report.md" <<'PY'
import sys, re, datetime
outdir = sys.argv[1]
m = open(outdir + "/matrix.tsv").read().splitlines()
header = m[0].split("\t")[1:]
print("# SDC 逐核定位扫描报告\n")
print("- 生成: %s" % datetime.datetime.now().isoformat(timespec="seconds"))
print("- 核数: %d；矩阵见 matrix.tsv\n" % len(header))
stats = {}
for line in m[1:]:
    p = line.split("\t")
    for r in p[1:]:
        stats[r] = stats.get(r, 0) + 1
print("## 结果统计\n")
for k in sorted(stats): print("- %s: %d" % (k, stats[k]))
PY
    if [ -n "$CONFIRM_MD" ]; then
        printf '\n## 确认复测（失败对单跑复测）\n\n| 核 | 用例 | 复测结果 | 分类 |\n|---|---|---|---|\n%s\n' \
            "$CONFIRM_MD" >> "$OUT/report.md"
    fi
}
generate_report
log "scan 完成: 矩阵=$OUT/matrix.tsv 报告=$OUT/report.md 退出码=$RC"
exit $RC
```

注意两点实现细节：(a) `cleanup` 中调用的 `build_matrix/generate_report` 定义在其后——bash 函数调用在运行时解析，trap 触发时定义已就位，合法；(b) 确认复测段落里 `CR` 的 grep 从每用例 yaml 取首个 `result:`——确认跑 `-e` 单用例，首个即该用例结果。

- [ ] **Step 2: 语法检查**

Run: `bash -n scripts/run/sdc-run.sh && echo SYNTAX-OK`
Expected: `SYNTAX-OK`。

### Task 3: 冒烟验证 scan（含 skip 形态与并行防重叠）

**Files:**
- Test: 临时输出目录（/tmp/sdc-run-smoke/）

- [ ] **Step 1: 冒烟——2 核 × 默认集 + 1 个必 skip 的 sve 用例 × 3s**

Run: `bash scripts/run/sdc-run.sh scan --cores 0,4 --t 3s --tests fma,zstd19,fma_sve --out /tmp/sdc-run-smoke; echo "rc=$?"`
Expected: `rc=0`；`matrix.tsv` 2 核 × 3 用例；fma/zstd19=pass，fma_sve=skip（无 SVE 主机预期 skip——验证 skip 解析正确）；report.md 统计行含 skip 计数。

- [ ] **Step 2: 冒烟——fail-loud 两条路径**

Run: `bash scripts/run/sdc-run.sh scan --tests fma,no_such_test --cores 0 --t 1s --out /tmp/x1; echo "rc=$?"` 与 `bash scripts/run/sdc-run.sh scan --cores 122 --t 1s --out /tmp/x2; echo "rc=$?"`
Expected: 均退出 rc=2，错误信息分别列出缺失用例名 / 离线核 122。

- [ ] **Step 3: 冒烟——--par=4 簇交错与总时长合理性**

Run: `bash scripts/run/sdc-run.sh scan --cores 0,1,2,3,4,5,6,7 --t 2s --tests fma --par 4 --out /tmp/sdc-run-par; echo "rc=$?"; cat /tmp/sdc-run-par/scan.log | head -10`
Expected: rc=0；scan.log 中核启动顺序为簇交错（0,4 先于 1,5——同簇不并行）；8 核总时长 ≈ 单核 2× 而非 8×。

### Task 4: 提交（feature 分支 + DCO）

- [ ] **Step 1: 切分支并提交**

```bash
cd /home/sdc/wangxu/sdcshield
git branch --show-current   # 确认不在 main；在则 git switch -c feat/sdc-run-scan
git add scripts/run/sdc-run.sh
git commit -s -m "feat(run): sdc-run.sh 统一入口——scan 逐核定位分析（core×test 矩阵+确认复测）"
```

验证：`git log -1 --format=%B` 末行是 `Signed-off-by: wangxu <wangxumarshall@qq.com>`；`git status` 干净（脚本文件外无裹挟）。push 到远端 feature 分支。

### Task 5: 基线全核 scan（定位轨，机器空闲时）

- [ ] **Step 1: 后台启动 126 核基线扫描**

Run（Bash run_in_background）: `cd /home/sdc/wangxu/sdcshield && bash scripts/run/sdc-run.sh scan --cores all --t 10s --par 32 --out ~/sdc-run/scan-baseline-$(date +%m%d-%H%M) 2>&1`
Expected: 126 核 × 12 用例 × 10s ÷ par32 ≈ 15–30 min 完成；产出完整矩阵。启动后先确认前几个核 YAML 正常生成。

- [ ] **Step 2: 基线结果分析**

Run: `cat ~/sdc-run/scan-baseline-*/report.md; grep -c fail ~/sdc-run/scan-baseline-*/matrix.tsv || true`
Expected: 绝大多数 pass + 少量预期 skip；任何 fail 进入 Task 7 分析循环（eigen 类在单核绑定下不应 fail——CLAUDE.md 记载 eigen 偶发仅在全系统多线程下）。

### Task 6: 启动 7×24 激发战役（激发轨，CPU+内存双满载）

**Interfaces:**
- Consumes: `scripts/sdc-excite-reproduce/sdc-excite-reproduce.sh full`（L0–L5 状态机；campaign.env 已存在：热联锁 95°C、stress-ng 伴层 124-127、L3 禁用 eigen_svd_double/eigen_sparse——全核多线程已知误报源）。
- 用户追加要求（2026-10-15）：**CPU 与内存都必须满载**——CPU 伴层之外加内存饱和伴层，且以实测占用为准（不为"应该满"放行）。

- [ ] **Step 0: 内存饱和伴层设计**

实测 `free -g` 总量与 MemAvailable，按 `MemTotal × 0.85` 起步（留 OOM 余量），用 `stress-ng --vm 1 --vm-bytes <size> --vm-method all --page-in` 常驻触碰页（不是只分配不触碰）；与 CPU 伴层并列跑。若战役引擎的用例本身吃满内存（memcpy/mdim 类），伴层按剩余空闲内存自适应。验证命令：`free -g`（used ≥ 85%）+ `vmstat 1 3`（wa/cs 正常，无 swap thrash）。
- [ ] **Step 1: 归档 smoke 状态，干净起 full**

Run: `mv ~/sdc-excite-reproduce/state.json ~/sdc-excite-reproduce/state.json.smoke-20261009.bak`（先 `cat` 确认内容为 smoke cycle4 残留——不盲删）。
Run（run_in_background）: `cd /home/sdc/wangxu/sdcshield && nohup bash scripts/sdc-excite-reproduce/sdc-excite-reproduce.sh full >> ~/sdc-excite-reproduce/driver.log 2>&1`
Expected: driver.log 出现 `[cycle 1]` full 模式行（T_COLD=30m 等 full 时长表）；campaign.env 沿用不重生成。

- [ ] **Step 2: 15 min 后健康检查**

Run: `tail -20 ~/sdc-excite-reproduce/driver.log; ls ~/sdc-excite-reproduce/logs/ | tail -5; cat /sys/class/thermal/thermal_zone*/temp`
Expected: 阶段推进、温度 < 联锁阈值 95°C、无 fail/crash 文件。

### Task 7: 监控-分析-迭代循环（直至捕获类型 SDC）

- [ ] **Step 1: 会话内定时巡检**（CronCreate，每 ~30 min，非整点分）: 检查 driver.log 尾部 + logs/ 新增 fail/crash + 基线/战役 YAML 中 `result: fail` 行 + 温度；任何异常 → 立即进入分析（哪个核、哪个用例、何种位模式、与机队签名对照：cn23154 VA[55:48]/簇锚定、CPU179 store→reload、CPU122 mercurial）。
- [ ] **Step 2: 异常处置协议**: fail 出现 → (a) 该核×用例单跑确认复测 ×3（持续 vs 瞬态）；(b) `sdc-run.sh scan --cores <疑核±簇邻> --tests <用例> --t 60` 定向驻留；(c) 簇饱和对照 `run_cluster_anchored.sh -C <簇> -e <NEON 用例集> -t 1800`（默认 sve512 集在本机全 skip，必须 -e 覆盖为 NEON 集）；(d) 提取位模式（YAML 的 mismatch 详情）判型。
- [ ] **Step 3: 策略迭代**: 每轮巡检后依据覆盖缺口调整——用例多样性（P1–P6 桶轮换）、时长档（60s→驻留 30min）、温度档（负载自然升温，无 root 无法主动调压，如实记录该杠杆缺失）、占用档（单核/簇/全机 × stress-ng 伴层）。
- [ ] **Step 4: 捕获判据**: 拿到「核定位 + 用例 + 位模式 + 触发条件（占用/温度/时长）+ 复现节律」五要素齐全的一次自然 SDC 事件，写入 docs/cases/ 案例文件，目标达成。

### Task 8: 文档同步（大颗粒度则更）

- [ ] **Step 1**: 检查 README.md 是否有 scripts 索引段落；有则加一行 sdc-run.sh scan 用法（引用 `--help`）。若 docs/research/progress-log.md 存在会话恢复约定，追加本次战役锚点（数据目录、分支、PID）。

---

## Self-Review

1. **Spec coverage**: /goal 五条要求 → 需求1(激发强度+定向) Task 6/7；需求2(sdc-run.sh scan 逐核定位) Task 2–5；需求3(多手段: 负载/温度/电压/用例参数) Task 6–7（电压杠杆无 root 不可用，已在 Global Constraints 如实记录并以并发档位补偿）；需求4(记录分析异常) Task 5 Step2 + Task 7 Step1–2；需求5(迭代至捕获) Task 7 Step3–4。✓
2. **Placeholder scan**: 无 TBD/TODO；Task 2 给出全量代码；Task 7 的巡检命令在执行时由 CronCreate prompt 固化。✓
3. **Type consistency**: `run_one/build_matrix/generate_report/cleanup` 命名一致；matrix.tsv 首行 `test\t<cores>` 约定在生成与消费两处一致（确认复测段按 `test\t` 头解析）。✓
4. **Review Focus 测试归属**: skip 解析→Task 3 Step 1（显式含 fma_sve）；核重叠→Task 3 Step 3（簇交错顺序断言）；中断矩阵→Task 2 trap + Task 5 长跑中实证；离线核→Task 3 Step 2（--cores 122 必须报错）；duration 后缀→透传不剥（Task 2 代码 `-t "$T_PER"` 原样）。✓
