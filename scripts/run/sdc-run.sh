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
#     --t DUR       每用例每核时长，透传 -t（缺省 5s；裸数字=毫秒，
#                   框架语义——建议带 s 后缀）
#     --par N       并行实例数（缺省 1；实例按簇交错调度避免同簇双载，
#                   仅加速，每核归因不变——同核绝不双实例）
#     --quality N   质量档（缺省 0=PROD+BETA，arm64_sdc 等 BETA 用例需要）
#     --retest N    框架级失败重测（缺省 3，透传 --retest-on-failure）
#     --out DIR     输出根目录（缺省 ./sdc-run-scan-<MMDD-HHMMSS>/）
#     -- <框架参数> 双划线后的一切原样透传给 sdcshield（如 -s AES 换 RNG 引擎、
#                  -O eigen_svd_double.mdim=3072 调用例参数、--test-list-randomize
#                  随机序）。不得传 -e/--cpuset/-o/-t/-Y/--quality 等破坏编排的
#                  选项——那会污染逐核归因。
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
usage() { sed -n '3,33p' "$0" | sed 's/^# \{0,1\}//'; }

# 本机敏感集（Kunpeng 920 无 SVE——sve* 用例不在集内；名字已对
# --quality=0 --list-tests 核实。P1 向量FMA/矩阵 + P2 ARM64 SDC 专项 + P4 位扩散）
DEFAULT_TESTS="eigen_gemm_double_dynamic_square,eigen_svd_double,openblas_dgemm,sleef_neon,fma,power_virus_dit,arm64_sdc,ooo_dep_chain_arm,lsu_store_forward_arm,neon_add,zstd19,openssl_sha"

[ $# -ge 1 ] || { usage; exit 2; }
CMD="$1"; shift
case "$CMD" in
    scan) ;;
    -h|--help|help) usage; exit 0 ;;
    *) echo "FATAL: 未知子命令 '$CMD'（可用: scan）" >&2; exit 2 ;;
esac

BIN_OVERRIDE=""; CORES_SPEC="all"; TESTS="$DEFAULT_TESTS"
T_PER="5s"; PAR=1; QUALITY=0; RETEST=3; OUT=""; PASSTHRU=()
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
        --)        shift; PASSTHRU=("$@"); break ;;
        -h|--help) usage; exit 0 ;;
        *) echo "FATAL: scan 未知选项 '$1'（框架参数请放在 -- 之后）" >&2; exit 2 ;;
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
MISSING=""
while IFS= read -r t; do
    [ -n "$t" ] || continue
    grep -qx "$t" "$TMP_LIST" || MISSING="$MISSING $t"
done < <(printf '%s\n' "$TESTS" | tr ',' '\n')
[ -z "$MISSING" ] || { echo "FATAL: 以下用例不在 --list-tests（--quality=$QUALITY）:$MISSING" >&2; exit 2; }
N_TESTS=$(printf '%s\n' "$TESTS" | tr ',' '\n' | grep -c .)

# ---------------- 核列表展开（all=仅在线核，离线核报错） ----------------
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
ORDERED=$(while read -r c; do echo "$(cluster_of "$c") $c"; done <<< "$CORES" | python3 -c "
import sys
from collections import defaultdict
# 输入: 每行 '<簇代表核> <核>'；输出: 簇交错序（第 r 轮取每簇第 r 个核
# → 前 N_簇 个启动实例互不同簇；PAR ≤ 簇数时同簇绝不并行）
g = defaultdict(list)
for l in sys.stdin:
    p = l.split()
    if len(p) == 2: g[int(p[0])].append(int(p[1]))
rows = [sorted(v) for _, v in sorted(g.items())]
out = []
if rows:
    for r in range(max(len(x) for x in rows)):
        for row in rows:
            if len(row) > r: out.append(row[r])
print('\n'.join(map(str, out)))
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
        ${PASSTHRU[@]+"${PASSTHRU[@]}"} \
        -e "$TESTS" -o "$OUT/core_${core}.yaml" \
        > "$OUT/core_${core}.stdout" 2>&1
}
PIDS=()
cleanup() {  # Ctrl-C: 杀子进程，已完成核照常出矩阵（不丢已采数据）
    trap - INT TERM
    for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    build_matrix || true
    generate_report || true
    log "被中断：部分矩阵已落盘 $OUT/matrix.tsv"
    exit 130
}
trap cleanup INT TERM

# ---------------- 并行调度（已用核集合防重叠；单核失败不中断全局） ----------------
DONE=0
for core in $ORDERED; do
    while [ "$(jobs -rp | wc -l)" -ge "$PAR" ]; do wait -n || true; done
    run_one "$core" & PIDS+=($!)
    DONE=$((DONE + 1))
    log "核 $core 启动（$DONE/$N_CORES）"
done
wait || true

# ---------------- 矩阵 ----------------
build_matrix() {
    # 解析规则（实测 YAML 结构——同名测试多数据集条目，如 fma×33）：
    #   任一条目 fail → fail；全条目 skip → skip；否则 pass。
    # mce_check（框架自动追加的 EDAC 检查）作为信息行附在矩阵末尾。
    python3 - "$OUT" "$TESTS" > "$OUT/matrix.tsv" <<'PY'
import sys, glob, re
outdir, tests = sys.argv[1], sys.argv[2].split(",")
cores = []
for f in glob.glob(outdir + "/core_*.yaml"):
    m = re.search(r"core_(\d+)\.yaml$", f)
    if m: cores.append(int(m.group(1)))
cores.sort()
entries = {}  # entries[test][core] = [result, ...]
for c in cores:
    cur = None
    for line in open("%s/core_%d.yaml" % (outdir, c), errors="replace"):
        m = re.match(r"^- test: (\S+)", line)
        if m: cur = m.group(1); entries.setdefault(cur, {}).setdefault(c, [])
        m = re.match(r"^  result: (\S+)", line)
        if m and cur: entries[cur][c].append(m.group(1).rstrip(","))
def verdict(vals):
    if not vals: return "-"
    if "fail" in vals: return "fail"
    if all(v == "skip" for v in vals): return "skip"
    return "pass"
print("test\t" + "\t".join(map(str, cores)))
for t in tests + (["mce_check"] if "mce_check" in entries else []):
    row = [verdict(entries.get(t, {}).get(c)) for c in cores]
    print(t + "\t" + "\t".join(row))
PY
}
build_matrix

# ---------------- 确认复测（失败对 ×1，静默邻居单跑） ----------------
FAIL_PAIRS=$(python3 - "$OUT" "$TESTS" <<'PY'
import sys
outdir, tests = sys.argv[1], sys.argv[2].split(",")
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
            ${PASSTHRU[@]+"${PASSTHRU[@]}"} \
            -e "$test" -o "$OUT/confirm_${core}_${test}.yaml" > /dev/null 2>&1 || true
        CR=$(grep -m1 "^  result:" "$OUT/confirm_${core}_${test}.yaml" 2>/dev/null | awk '{print $2}' || true)
        if [ "$CR" = "fail" ]; then CLASS="持续(复现)"; else CLASS="瞬态(未复现)"; fi
        CONFIRM_MD="$CONFIRM_MD
| $core | $test | ${CR:-无结果} | $CLASS |"
        log "确认核 $core $test → ${CR:-无结果}（$CLASS）"
    done <<< "$FAIL_PAIRS"
    RC=1
fi

# ---------------- report.md ----------------
generate_report() {
    python3 - "$OUT" > "$OUT/report.md" <<'PY'
import sys, datetime
outdir = sys.argv[1]
m = open(outdir + "/matrix.tsv").read().splitlines()
header = m[0].split("\t")[1:]
print("# SDC 逐核定位扫描报告\n")
print("- 生成: %s" % datetime.datetime.now().isoformat(timespec="seconds"))
print("- 核数: %d；矩阵见 matrix.tsv\n" % len(header))
stats = {}
for line in m[1:]:
    for r in line.split("\t")[1:]:
        stats[r] = stats.get(r, 0) + 1
print("## 结果统计\n")
for k in sorted(stats): print("- %s: %d" % (k, stats[k]))
PY
    if [ -n "${CONFIRM_MD:-}" ]; then
        printf '\n## 确认复测（失败对单跑复测）\n\n| 核 | 用例 | 复测结果 | 分类 |\n|---|---|---|---|\n%s\n' \
            "$CONFIRM_MD" >> "$OUT/report.md"
    fi
}
generate_report
log "scan 完成: 矩阵=$OUT/matrix.tsv 报告=$OUT/report.md 退出码=$RC"
exit $RC
