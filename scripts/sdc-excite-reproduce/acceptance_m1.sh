#!/bin/bash
# acceptance_m1.sh — M1 退出标准核对（v5 §14.1：24h 采集无关键丢样 + 质量字段可见 + 开销达标）
# 用法: bash acceptance_m1.sh <运行小时数，默认 24>   # 冒烟口径传 0.5，正式门传 24
# 七项门：① 三采集器 active ② 丢样<0.1%（周期口径，逐采集器）③ percore 新鲜度
#         ④ PMU percent_covered 可见 ⑤ 运行时长（容差 2%）⑥ A/B 常态开销
#         ⑦ 逐采集器最大间隙 <3×period_s（采集死区捕获）
# ⑥ 为 T2 裁定的双档协议：复测 median 下降 ≤3% PASS / 3%-8% WARN（受环境噪声限制，
# 需静默窗口复测——如实降级路径）/ >8% FAIL；输出复测 median±MAD；跑前查静默窗口
# （pgrep sdcshield 非 0 → 记警告不中断）。锚点与复测 t-ms 不同（10s vs 30s）→
# 按 loops/s 归一后比较，直接比 loop_total 不可比。
# 退出码：0 = PASS（含 WARN 时输出明确标注，WARN 项需静默窗口复测跟进）；1 = FAIL。
set -u
cd "$(dirname "$0")/../.."                              # 仓库根（ab_baseline 相对路径）
HOURS="${1:-24}"
ROOT="${SDC_EXCITE_REPRODUCE_DIR:-/home/sdc/sdc-excite-reproduce}"
MON="$ROOT/monitor"
FAIL=0; WARNS=0
need() { echo "✗ $1"; FAIL=1; }
ok()   { echo "✓ $1"; }
warn() { echo "⚠ $1"; WARNS=$((WARNS+1)); }
have() { [ -s "$1" ] || { need "缺 $1"; return 1; }; }

# 1) 三采集器 active
for c in percore pmu ras; do
    [ "$(systemctl is-active sdc-collector@$c)" = active ] && ok "sdc-collector@$c active" \
        || need "sdc-collector@$c 非 active"
done

# 2) 丢样：按周期口径对所有采集器统一——collector_self.csv 每行=一周期，
#    失败率 = 该 collector 的失败周期数 / 该 collector 的行数，阈值 0.1%
#    （单位语义：周期失败率。samples_dropped 是进程内累计计数、重启归零——
#     相邻回退处分段，各段末值之和=失败周期总数；pmu 的行级丢失（某核某窗口
#     缺行）不在此口径，由门 7 间隙兜住。空文件（只有列头）exit 2 给明确文案）
if have "$MON/collector_self.csv"; then
python3 - "$MON/collector_self.csv" <<'EOF'
import csv, sys
from collections import defaultdict
rows = list(csv.DictReader(open(sys.argv[1])))
if not rows:
    print("collector_self.csv 无数据（只有列头）"); sys.exit(2)
per = defaultdict(list)
for r in rows:
    per[r["collector"]].append(int(r["samples_dropped"]))
fail = False
for c in ("percore", "pmu", "ras"):                # 采集器整体缺席也是盲区
    if c not in per:
        print(f"{c}: 无数据行"); fail = True
for c in sorted(per):
    drops = per[c]
    total, prev = 0, drops[0]                      # 累计计数器：回退=新进程段
    for d in drops[1:]:
        if d < prev:
            total += prev
        prev = d
    total += prev
    print(f"{c}: 失败周期 {total}/{len(drops)} = {total/len(drops)*100:.4f}%")
    fail |= total / len(drops) >= 0.001
sys.exit(1 if fail else 0)
EOF
case $? in
    0) ok "丢样 <0.1%（周期口径，逐采集器）";;
    2) need "collector_self.csv 无数据（只有列头）——丢样无法核验";;
    *) need "丢样超标（周期失败率 ≥0.1%，见上明细）";;
esac
fi

# 3) 数据新鲜度：percore.csv 末行 ts 距今 < 180s 固定界
if have "$MON/percore.csv"; then
LAST=$(tail -1 "$MON/percore.csv" | cut -d, -f1)
if LAST_TS=$(date -d "$LAST" +%s 2>/dev/null); then
    AGE=$(( $(date +%s) - LAST_TS ))
    [ "$AGE" -lt 180 ] && ok "percore 新鲜 (${AGE}s)" || need "percore 停更 ${AGE}s"
else
    need "percore.csv 末行 ts 不可解析: '${LAST}'"
fi
fi

# 4) PMU 质量字段可见：pmu_core.csv 列头含 percent_covered（multiplex 诚实呈现）
if have "$MON/pmu_core.csv"; then
head -1 "$MON/pmu_core.csv" | grep -q percent_covered && ok "PMU 质量字段可见" \
    || need "pmu_core.csv 缺 percent_covered"
fi

# 5) 时长门：运行满 HOURS 小时（collector_self.csv 最早最晚 ts 差，容差 2%）；
#    只有列头无数据行 → exit 2 给明确文案（而非 IndexError traceback）
if have "$MON/collector_self.csv"; then
python3 - "$MON/collector_self.csv" "$HOURS" <<'EOF'
import csv, sys, datetime
rows = list(csv.DictReader(open(sys.argv[1])))
if not rows:
    print("collector_self.csv 无数据（只有列头）"); sys.exit(2)
t0 = datetime.datetime.strptime(rows[0]["ts"], "%Y-%m-%d %H:%M:%S")
t1 = datetime.datetime.strptime(rows[-1]["ts"], "%Y-%m-%d %H:%M:%S")
print(f"数据跨度 {(t1-t0).total_seconds()/3600:.2f}h（要求 ≥{float(sys.argv[2])*0.98:.2f}h）")
sys.exit(0 if (t1 - t0).total_seconds() >= float(sys.argv[2]) * 3600 * 0.98 else 1)
EOF
case $? in
    0) ok "运行 ≥${HOURS}h";;
    2) need "collector_self.csv 无数据（只有列头）——时长无法核验";;
    *) need "运行时长不足（或冒烟口径传 0.5 再验）";;
esac
fi

# 6) A/B 常态开销复测（双档协议）——锚点=采集器全停基线（Task 2），本测=采集器常态运行
NS=$(pgrep -c sdcshield 2>/dev/null || true)
if [ -n "$NS" ] && [ "$NS" != "0" ]; then
    warn "静默窗口不净: pgrep sdcshield=$NS（共享机器噪声，如实记录不中断）"
fi
if python3 tools/telemetry/ab_baseline.py --runs 15 --t-ms 30000 --out /tmp/ab_m1.json; then
python3 - "$ROOT/monitor/ab_baseline.json" /tmp/ab_m1.json <<'EOF'
import json, statistics, sys
A, B = (json.load(open(p)) for p in sys.argv[1:3])
if A["config"]["n"] != B["config"]["n"]:
    print(f"A/B 配置不一致: 锚点 n={A['config']['n']} vs 复测 n={B['config']['n']}（不可比）")
    sys.exit(4)
def rates(doc):                                    # loop_total → loops/s（跨 t-ms 归一）
    t = doc["config"]["t_ms"] / 1000.0
    return [r["loop_total"] / t for r in doc["runs"]]
ra, rb = rates(A), rates(B)
ma, mb = statistics.median(ra), statistics.median(rb)
mada = statistics.median([abs(x - ma) for x in ra])
madb = statistics.median([abs(x - mb) for x in rb])
drop = (ma - mb) / ma * 100
print(f"锚点  median {ma:.2f} loops/s (MAD ±{mada:.2f} = {mada/ma*100:.1f}%, runs={len(ra)}, t_ms={A['config']['t_ms']})")
print(f"复测  median {mb:.2f} loops/s (MAD ±{madb:.2f} = {madb/mb*100:.1f}%, runs={len(rb)}, t_ms={B['config']['t_ms']})")
print(f"常态开销: median 下降 {drop:+.2f}%")
if drop <= 3.0:
    print("A/B 判定: PASS（下降 ≤3%）"); sys.exit(0)
if drop <= 8.0:
    print("A/B 判定: WARN（下降 3%-8%）——受环境噪声限制，需静默窗口复测"); sys.exit(3)
print("A/B 判定: FAIL（下降 >8%）——查采集器 CPU 占用"); sys.exit(4)
EOF
case $? in
    0) ok "A/B 常态开销 ≤3%（PASS）";;
    3) warn "A/B 常态开销 3%-8%（WARN）——受环境噪声限制，需静默窗口复测";;
    *) need "A/B 常态开销 >8% 或复测异常（FAIL）——查采集器 CPU 占用/锚点文件";;
esac
else
    need "A/B 复测执行失败（ab_baseline.py 非 0 退出）"
fi

# 7) 逐采集器最大间隙：collector_self.csv 按 collector 分组、ts 升序，相邻 ts 间隙
#    max < 3×该 collector 的 period_s（period_s 列在文件里）——捕获"中途死亡数小时
#    被 systemd 拉活"的死区：门 1 只看当下 active、门 3 只看末行新鲜度，都看不见
#    历史空洞；重启后列头相同则无缝续写，唯有时间轴间隙留痕
if have "$MON/collector_self.csv"; then
# GAP_SINCE（可选，'YYYY-MM-DD HH:MM:SS'）：间隙扫描的下界。计划维护窗
# （如 M1 A/B 锚点抓取需停/起采集器）会在时间轴留下真实间隙，全史扫描会把
# 它当死区误报；设 GAP_SINCE=维护重启时刻即把验收窗起算点移到维护后。
# 证据数据不动，只限定扫描范围；默认空 = 全史扫描（原行为）。
GAP_SINCE="${GAP_SINCE:-}" python3 - "$MON/collector_self.csv" <<'EOF'
import csv, datetime, os, sys
from collections import defaultdict
FMT = "%Y-%m-%d %H:%M:%S"
since = os.environ.get("GAP_SINCE", "").strip()
t_since = datetime.datetime.strptime(since, FMT) if since else None
rows = list(csv.DictReader(open(sys.argv[1])))
if not rows:
    print("collector_self.csv 无数据（只有列头）"); sys.exit(2)
if t_since is not None:
    n_all = len(rows)
    rows = [r for r in rows if datetime.datetime.strptime(r["ts"], FMT) >= t_since]
    print(f"（间隙扫描窗：自 {since} 起，{len(rows)}/{n_all} 行在窗内——GAP_SINCE 限定）")
per = defaultdict(list)
for r in rows:
    per[r["collector"]].append(r)
fail = False
for c in sorted(per):
    rs = sorted(per[c], key=lambda r: r["ts"])
    if len(rs) < 2:
        print(f"{c}: 仅 {len(rs)} 行，无相邻间隙可查"); continue
    worst, bound = None, 0.0
    for a, b in zip(rs, rs[1:]):
        gap = (datetime.datetime.strptime(b["ts"], FMT)
               - datetime.datetime.strptime(a["ts"], FMT)).total_seconds()
        if worst is None or gap > worst:
            worst, bound = gap, 3 * float(a["period_s"])
    print(f"{c}: max gap {worst:.0f}s（period {rs[0]['period_s']}s，界 {bound:.0f}s）")
    fail |= worst >= bound
sys.exit(1 if fail else 0)
EOF
case $? in
    0) ok "逐采集器最大间隙 < 3×period_s（无采集死区）";;
    2) need "collector_self.csv 无数据（只有列头）——间隙无法核验";;
    *) need "存在采集死区（相邻周期间隙 ≥3×period_s，见上明细）";;
esac
fi

if [ "$FAIL" = 0 ] && [ "$WARNS" = 0 ]; then
    echo "M1 验收 PASS"; exit 0
elif [ "$FAIL" = 0 ]; then
    echo "M1 验收 PASS（含 ${WARNS} 项 WARN——见上，WARN 项需静默窗口复测后闭环）"; exit 0
else
    echo "M1 验收 FAIL"; exit 1
fi
