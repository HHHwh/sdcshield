#!/bin/bash

# 设定每个核心的测试时间（每个用例2分钟，总时间40分钟）
TEST_TIME_PER_CORE="20s"
TOTAL_TIME="7m"
OUTPUT_DIR="core_test_results"

# 创建结果目录
mkdir -p $OUTPUT_DIR

# 循环所有核心 (0-127)
for core in {87..127}; do
    echo "========== 正在测试核心 $core =========="
    LOG_FILE="$OUTPUT_DIR/sdcshield_core_${core}.log"

    # 运行单核心测试
    ./builddir/sdcshield -t $TEST_TIME_PER_CORE -T $TOTAL_TIME --strict-runtime --cpuset=$core -o $LOG_FILE
    RC=$?

    # 完整判定（框架输出为小写 "result: fail"，须大小写不敏感匹配）：
    #   fail/crash → 失败；非零退出码 → 异常；零 result 行 → 异常；
    #   只有「结果数非零且全为 pass」才算通过（skip 宽松收集、不计失败）
    # （fail 块内部含独立 "seed:" 行，只统计 "result:" 行，避免误计）
    RESULT_CNT=$(grep -Eci 'result: *(pass|fail|crash|skip|timed out|interrupted|operating system error)' $LOG_FILE)
    PASS_CNT=$(grep -c 'result: pass' $LOG_FILE)
    if grep -Eqi 'result: *(fail|crash)' $LOG_FILE; then
        echo "⚠️ 核心 $core 测试出现失败！请检查日志：$LOG_FILE"
        # 可选：将失败核心记录到文件中
        echo "core_$core" >> $OUTPUT_DIR/failed_cores.txt
    elif [ $RC -ne 0 ]; then
        echo "⚠️ 核心 $core 测试异常退出（rc=$RC）！请检查日志：$LOG_FILE"
        echo "core_$core" >> $OUTPUT_DIR/failed_cores.txt
    elif [ "$RESULT_CNT" -eq 0 ]; then
        echo "⚠️ 核心 $core 无任何 result 行（崩溃/被杀/零执行）！请检查日志：$LOG_FILE"
        echo "core_$core" >> $OUTPUT_DIR/failed_cores.txt
    elif [ "$PASS_CNT" -ne "$RESULT_CNT" ]; then
        echo "⚠️ 核心 $core 存在 pass 之外的异常结果（pass=$PASS_CNT/结果数=$RESULT_CNT）！请检查日志：$LOG_FILE"
        echo "core_$core" >> $OUTPUT_DIR/failed_cores.txt
    else
        echo "✅ 核心 $core 测试通过"
    fi

    echo ""
    # 可选：暂停一下，避免系统过热（可调整）
    # sleep 1
done

echo "所有核心测试完成！"
echo "失败核心列表（如有）："
cat $OUTPUT_DIR/failed_cores.txt 2>/dev/null || echo "无失败核心，全部通过！"
