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
    
    # 检查测试结果（框架输出为小写 "result: fail"，须大小写不敏感匹配）
    if grep -Eqi 'result: *fail' $LOG_FILE; then
        echo "⚠️ 核心 $core 测试出现失败！请检查日志：$LOG_FILE"
        # 可选：将失败核心记录到文件中
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
