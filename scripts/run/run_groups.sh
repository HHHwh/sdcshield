#!/bin/bash
# ============================================================
# 分四组并行测试 15 个特定测试用例（每组 32 个核心）
# 核心列表显式写出，避免解析问题
# 用法： ./run_15_tests_groups_explicit.sh [每个用例时间]  例如：./run_15_tests_groups_explicit.sh 1m
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

LOG_DIR="fail_15_tests"
mkdir -p "$LOG_DIR"

# 15 个测试用例名称（完全匹配 --list）
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
enabled_list=$(IFS=,; echo "${tests[*]}")

# 四组核心列表（显式写出）
group1="0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31"
group2="32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63"
group3="64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95"
group4="96,97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127"

groups=("$group1" "$group2" "$group3" "$group4")

# 颜色
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

echo "========== 分组运行 15 个测试（每个用例 ${TEST_TIME}）=========="
echo "日志目录: $LOG_DIR"
echo "开始时间: $(date)"
echo ""

for cpuset in "${groups[@]}"; do
    LOG_FILE="$LOG_DIR/group_$(echo $cpuset | cut -d',' -f1)-$(echo $cpuset | rev | cut -d',' -f1).log"
    echo -n "组 (核心 $cpuset) 测试中 ... "

    timeout -k 10 $((TOTAL_SEC + 10)) ./builddir/sdcshield \
        --cpuset="$cpuset" \
        --enable="$enabled_list" \
        -t "$TEST_TIME" \
        -T "$TOTAL_TIME" \
        --strict-runtime \
        -o "$LOG_FILE" 2>&1

    if grep -Eqi "result: *(fail|crash)" "$LOG_FILE"; then
        echo -e "${RED}❌ FAIL${NC}"
        echo "  日志文件: $LOG_FILE"
        echo "  最后失败信息："
        grep -B 5 -A 5 "result: fail" "$LOG_FILE" | grep -E "data\[0..7\]|data_ok|consistent|crc[0-9]*=" | tail -10
    elif grep -q "result: pass" "$LOG_FILE"; then
        echo -e "${GREEN}✅ PASS${NC}"
        rm -f "$LOG_FILE"
    else
        echo -e "${RED}⚠️ 无 result 行（崩溃/被杀/零执行？）——日志保留: $LOG_FILE${NC}"
    fi
    echo ""
done

echo "结束时间: $(date)"
rm -f sdcshield-*.yaml
