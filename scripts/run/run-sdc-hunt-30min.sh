#!/bin/bash
# SDC 狩猎长时窗检测：逐个运行我们扩展的 SVE 测试，每个 30 分钟
# 运行规则（用户 2026-09-21 指定；2026-10-13 审查修复 D-009 调整第 3 条）：
#   1. 每个测试用例逐个运行 30 分钟（全核，含 122）
#   2. 每运行完一个：检查该测试的日志——最终结果 fail 或日志中存在 SDC 记录（fail/miscompare 等）
#      （双保险：结果层面出现 SDC + 日志层面留有 SDC 记录）
#   3. 有 SDC → 保留日志；无 SDC → 日志同样保留（不删）——skip/异常不可静默吞掉，
#      保留全部日志供事后审计（D-009：原"无 SDC 删日志"规则改为全保留）
#   4. 运行下一个
# 用法: nohup bash run-sdc-hunt-30min.sh <测试清单文件> &
#   清单文件默认 docs/docs_xu/sve-test-inventory.txt（116 个，每行一个测试 ID）

set -u

REPO=/home/sdc/root-xupeng/sdcshield
BIN=$REPO/builddir/sdcshield
LIST_FILE=${1:-$REPO/docs/docs_xu/sve-test-inventory.txt}
OUT_DIR=$REPO/sdc_hunt_logs
PER_TEST_MS=1800000        # 30 分钟
DURATION="30m"

mkdir -p "$OUT_DIR"
RESULTS_CSV=$OUT_DIR/summary.csv
echo "test_id,start_time,end_time,result,has_sdc_log,log_path" > "$RESULTS_CSV"

TOTAL=$(grep -c . "$LIST_FILE")
echo "=============================================="
echo " SDC 狩猎长时窗检测启动"
echo " 开始时间: $(date '+%Y-%m-%d %H:%M:%S')"
echo " 测试清单: $LIST_FILE ($TOTAL 个测试)"
echo " 每测试时长: $DURATION (全核含 122)"
echo " 日志目录: $OUT_DIR"
echo " 预计总时长: 约 $(( TOTAL * 31 / 60 )) 小时 ($TOTAL × ~31min)"
echo "=============================================="

IDX=0
for T in $(grep . "$LIST_FILE"); do
    IDX=$((IDX+1))
    START=$(date '+%Y-%m-%d %H:%M:%S')
    echo "[$IDX/$TOTAL] $START  开始: $T  (预计结束 $(date -d "+30 minutes" '+%H:%M'))"

    LOG=$OUT_DIR/${T}.yaml

    # 运行测试: 全核(含 122), 30 分钟, 日志显式命名
    # --fatal-skips: skip 也算失败(诚实, D-009 兑现注释承诺); 不加 -F, 让所有测试跑完
    "$BIN" -e "$T" -t "$PER_TEST_MS" --fatal-skips -o "$LOG" > /dev/null 2>&1
    RC=$?

    END=$(date '+%Y-%m-%d %H:%M:%S')

    # ---- SDC 双重判定 ----
    # 条件A: 进程退出码非 0 (fail/crash)
    # 条件B: 日志中存在失败记录 (result: fail / crash / miscompare / data-miscompare / report_fail 文本)
    RESULT="pass"
    [ $RC -ne 0 ] && RESULT="nonzero_exit($RC)"

    HAS_SDC=0
    if [ -f "$LOG" ]; then
        # 结果层面的 fail/crash
        if grep -qE "result: (fail|crash)" "$LOG"; then HAS_SDC=1; fi
        # 日志层面的 SDC 记录(miscompare / 数据错配 / E> Failed)
        if grep -qE "data-miscompare|E> Failed|miscompare" "$LOG"; then HAS_SDC=1; fi
        # 最终 exit 行
        if grep -qE "^exit: (fail|crash)" "$LOG"; then HAS_SDC=1; fi
    fi

    # 红队修复: RC=0 但日志含 SDC 记录时 result 列不可写 pass——否则同一行
    # "pass,1" 自相矛盾, "grep ,pass," 统计会误判; 记为 sdc_log。
    # RC!=0 时保留 nonzero_exit(rc) 不覆盖(退出码信息更有价值)
    [ "$HAS_SDC" -eq 1 ] && [ $RC -eq 0 ] && RESULT="sdc_log"

    # 双保险: 结果 fail **或** 日志含 SDC 记录 → 标记信号；无信号 → pass。
    # 无论有无信号，日志一律保留（D-009：skip/零执行不可静默吞掉，事后可审计）
    if [ "$HAS_SDC" -eq 1 ] || [ $RC -ne 0 ]; then
        echo "  [$IDX/$TOTAL] $END  ★★ $T: SDC/失败信号 (exit_rc=$RC, sdc_log=$HAS_SDC) → 日志保留: $LOG"
        echo "$T,$START,$END,$RESULT,$HAS_SDC,$LOG" >> "$RESULTS_CSV"
    else
        echo "  [$IDX/$TOTAL] $END  ✓ $T: 无 SDC → 日志保留: $LOG"
        echo "$T,$START,$END,pass,0,$LOG" >> "$RESULTS_CSV"
    fi
done

echo "=============================================="
echo " SDC 狩猎长时窗检测全部完成"
echo " 结束时间: $(date '+%Y-%m-%d %H:%M:%S')"
echo " 汇总: $RESULTS_CSV"
echo " 有 SDC 信号的条目:"
# tail rc 恒 0 使 || 永不触发——补 grep . 让"全 pass 无信号"时也能打出 (无)
grep -vE ",pass,0," "$RESULTS_CSV" | tail -n +2 | grep . || echo "  (无)"
echo "=============================================="
