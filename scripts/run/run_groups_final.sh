#!/bin/bash
# ============================================================
# 分四组并行测试 15 个特定测试用例（每组 32 个核心）
# 核心列表用 seq 动态生成，--enable 每个测试单独指定
# 所有输出重定向到日志，终端保持干净
# 用法： ./run_groups_final.sh [每个用例时间]  例如：./run_groups_final.sh 1m
# 默认每个用例 1 分钟
# ============================================================

# 清理旧日志
rm -f sdcshield-*.yaml

TEST_TIME="${1:-1m}"
# 解析时间
NUM=$(echo "$TEST_TIME" | grep -o '^[0-9]*')
UNIT=$(echo "$TEST_TIME" | grep -o '[a-zA-Z]*$')
[ -z "$NUM" ] && NUM=1 && UNIT="m"
case "$UNIT" in
    s) TOTAL_SEC=$((NUM * 2)) ;;
    m) TOTAL_SEC=$((NUM * 2 * 60)) ;;
    h) TOTAL_SEC=$((NUM * 2 * 3600)) ;;
    *) TOTAL_SEC=$((NUM * 2)) ;;
esac
TOTAL_TIME="${TOTAL_SEC}s"
TIMEOUT_SEC=$((TOTAL_SEC + 10))

LOG_DIR="fail_groups_final"
mkdir -p "$LOG_DIR"

# 15 个测试用例
tests=(
    memcpy_shuffle_v2
    memcpy_shuffle
    isal_crc_iscsi
    isal_crc_ieee
    isal_crc64_jones_norm
    isal_crc64_iso_refl
    isal_crc64_iso_norm
    isal_crc64_ecma182_refl
    isal_crc64_ecma182_norm
    isal_crc32_gzip
    isal_crc_t10dif
    isal_crc64_jones_refl
    crc32_fixed_shuffled
    crc32_fixed
    crc32
)

# 四组核心范围
groups=("0-31" "32-63" "64-95" "96-127")

# 颜色
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

echo "========== 分组运行 15 个测试（每个用例 ${TEST_TIME}）=========="
echo "日志目录: $LOG_DIR"
echo "开始时间: $(date)"
echo ""

for group_range in "${groups[@]}"; do
    start=$(echo $group_range | cut -d'-' -f1)
    end=$(echo $group_range | cut -d'-' -f2)
    cpuset=$(seq -s, $start $end)
    LOG_FILE="$LOG_DIR/group_${group_range}.log"
    echo -n "组 ${group_range} 测试中 ... "

    # 构建 --enable 参数（每个测试单独一个）
    ENABLE_ARGS=""
    for test in "${tests[@]}"; do
        ENABLE_ARGS="$ENABLE_ARGS --enable=$test"
    done

    # 执行命令，将所有输出重定向到日志文件，终端无输出
    timeout -k 10 "$TIMEOUT_SEC" ./builddir/sdcshield \
        --cpuset="$cpuset" \
        $ENABLE_ARGS \
        -t "$TEST_TIME" \
        -T "$TOTAL_TIME" \
        --strict-runtime \
        -o "$LOG_FILE" >> "$LOG_FILE" 2>&1
    RC=$?

    # 完整判定：命令退出码非 0 / fail/crash / timed out/interrupted/OS error 等异常结果
    # / 预期用例未全部出结果，均不通过；仅整次运行明确通过且无异常时才删日志
    # （fail 块内部含独立 "seed:" 行，只统计 "result:" 行，避免误计）
    PASS_CNT=$(grep -c "result: pass" "$LOG_FILE")
    RESULT_CNT=$(grep -Ec "result: *(pass|fail|crash|skip|timed out|interrupted|operating system error)" "$LOG_FILE")
    EXPECTED=${#tests[@]}
    if grep -Eqi "result: *(fail|crash)" "$LOG_FILE"; then
        echo -e "${RED}❌ FAIL${NC}"
        echo "  日志文件: $LOG_FILE"
        echo "  最后失败信息："
        grep -B 5 -A 5 "result: fail" "$LOG_FILE" | grep -E "data\[0..7\]|data_ok|consistent|crc[0-9]*=" | tail -10
    elif [ $RC -ne 0 ]; then
        echo -e "${RED}❌ FAIL（命令退出码 $RC）——日志保留: $LOG_FILE${NC}"
    elif grep -Eqi "result: *(timed out|interrupted|operating system error)" "$LOG_FILE"; then
        # 混合结果（如 pass + timeout 中断）同样是异常信号，不得当作通过删除日志
        echo -e "${RED}❌ FAIL（存在 timed out/interrupted/OS error 结果）——日志保留: $LOG_FILE${NC}"
    elif [ "$RESULT_CNT" -lt "$EXPECTED" ] || [ "$PASS_CNT" -lt "$EXPECTED" ]; then
        # 组内预期 ${EXPECTED} 个用例未全部出结果或未全部 pass（未执行/被中断/零结果）
        echo -e "${RED}⚠️ 结果不完整（pass=$PASS_CNT/结果数=$RESULT_CNT，预期 $EXPECTED）——日志保留: $LOG_FILE${NC}"
    else
        echo -e "${GREEN}✅ PASS${NC}"
        rm -f "$LOG_FILE"
    fi
    echo ""
done

echo "结束时间: $(date)"
rm -f sdcshield-*.yaml
