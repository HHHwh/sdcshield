#!/bin/bash
# ============================================================
# 循环 18 次，每次执行 sdcshield -T 30m --strict-runtime
# 每次运行后检查结果：
#   - 全 PASS → 删除该次日志
#   - 有 FAIL → 保留日志并立即中止循环
# ============================================================

OPENDCDIAG="./builddir/sdcshield"
LOG_DIR="./logs"
TOTAL=20
CPU_LIST=$(seq -s, 32 63)   # 生成 "32,33,...,63"

mkdir -p "$LOG_DIR"

echo "========== 开始循环测试（共 $TOTAL 次，每次 10 分钟）=========="
echo "日志目录: $LOG_DIR"
echo "开始时间: $(date)"
echo ""

for ((i=1; i<=TOTAL; i++)); do
    LOG_FILE="$LOG_DIR/run_${i}.log"
    echo -n "第 $i 次运行（10 分钟）... "

    # 执行测试（使用正确的时间参数）
    taskset -c $CPU_LIST $OPENDCDIAG -T 10m --strict-runtime -o "$LOG_FILE" &> /dev/null
    RUN_RC=$?

    # 检查执行是否成功（退出码非0则报错）
    if [ $RUN_RC -ne 0 ]; then
        echo -e "\033[31m❌ 执行失败（退出码 $RUN_RC）\033[0m"
        echo "日志文件: $LOG_FILE"
        # 保留日志供检查，退出循环
        exit 1
    fi

    # 完整判定（忽略大小写）：先看有无 fail/crash/中断类结果，再核对结果数非零且全 pass
    # （fail 块内部含独立 "seed:" 行，只统计 "result:" 行，避免误计）
    RESULT_CNT=$(grep -Eci "result: *(pass|fail|crash|skip|timed out|interrupted|operating system error)" "$LOG_FILE")
    PASS_CNT=$(grep -c "result: pass" "$LOG_FILE")
    if grep -Eqi "result: *(fail|crash)" "$LOG_FILE"; then
        echo -e "\033[31m❌ FAIL\033[0m"
        echo ""
        echo "========== 测试中止：第 $i 次运行出现失败 =========="
        echo "日志文件: $LOG_FILE"
        echo "失败信息（最近一次 FAIL 上下文）："
        grep -B 5 -A 5 -i "result: fail" "$LOG_FILE" | tail -20
        echo "=================================================="
        exit 1   # 退出脚本，保留日志
    elif [ "$RESULT_CNT" -eq 0 ] || [ "$PASS_CNT" -ne "$RESULT_CNT" ]; then
        # 零 result 行（崩溃/被杀/零执行）或存在 pass 之外的软失败结果——一律视为失败，保留日志
        echo -e "\033[31m⚠️ 结果不完整或异常（pass=$PASS_CNT/结果数=$RESULT_CNT）——日志保留: $LOG_FILE\033[0m"
        exit 1
    else
        echo -e "\033[32m✅ PASS\033[0m"
        rm -f "$LOG_FILE"   # 删除本次确认通过（结果齐全且全 pass）的日志
    fi

    # 每 5 次输出一次进度
    if [ $((i % 5)) -eq 0 ]; then
        echo "已完成 $i / $TOTAL 次，当前时间: $(date)"
    fi
done

echo ""
echo "========== 所有 $TOTAL 次运行全部通过！ =========="
echo "结束时间: $(date)"
echo "所有 PASS 的日志已自动删除。"
