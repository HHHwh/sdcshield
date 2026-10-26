#!/bin/bash
# benchmark.sh — 在 openEuler 容器内采集跨 OS 基准(固定 loop 数,同工作量墙钟对比)。
#
# 背景/口径见 scripts/gha/benchmark.md。这里只做采集:
#   bash benchmark.sh <bin> <outdir>
#     <bin>    容器内 sdcshield 绝对路径(如 /__w/.../builddir/sdcshield)
#     <outdir> 输出目录(挂载回 host)
# 产出:<outdir>/benchmark.tsv,首行表头 test<TAB>wall_seconds<TAB>loop_count 是
#   三方契约(benchmark.sh 写 / benchmark-gate.py 与 benchmark-to-json.py 校验),
#   格式演进(加列/改列序)必须三处同步。
#   wall_seconds 三态(消费方闸门见 benchmark-gate.py):
#     数值   数据行(目标测试 result: pass 且取到它自己的 test-runtime)
#     skip   运行时跳过(result: skip)——良性无数据点
#     fail   在列但未产出可用数据(进程失败/测试 fail/字段缺失)——交闸门红
#   失败现场:每测试输出留档 <outdir>/<t>.log、YAML 留档 <outdir>/<t>.yaml
#   (随 artifact 上传), 摘要(退出码+末尾输出)同时打进本脚本 stdout
#   (→ workflow 的 benchmark.log)。
#
# 设计:共享 CI runner 跑不了确定性 assert(云实例非用户实机);固定墙钟在不同 OS 上会跑到
# 不同迭代数,不可比。改用 --max-test-loop-count N 让每个 OS 每个测试跑恰好 N 次 main loop,
# 工作量一致,只比"完成同样工作量的墙钟"。结果口径是跨 openEuler 版本的相对近似值。
#
# SPDX-License-Identifier: Apache-2.0
set -uo pipefail

BIN="${1:?usage: benchmark.sh <bin> <outdir>}"
OUTDIR="${2:?usage: benchmark.sh <bin> <outdir>}"
mkdir -p "$OUTDIR"

# 跨三系列交集测试(20.03/22.03/24.03 皆有;sleef/isal 是 24.03 独占,不进主表避免 N/A 泛滥)。
# 选默认压缩档 zstd/zlib(单 loop ~80s/~177s),而非最高档 zstd19/zlib1(700s+/168s+),保证 bounded。
BENCH_TESTS=(
  openblas_dgemm openblas_sgemm zstd zlib fma crc32 pocketfft_fft \
  memcpy_l2_cache_size eigen_gemm_double_dynamic_square gmp_bignum openssl_sha
)
LONG_LOOP=1      # 单 loop 较长的测试(zstd/zlib 压缩往返)降为 1 次
DEFAULT_LOOP=2   # 其余 2 次(openblas 单 loop ~54s → 2 loop ~2min)

# 先取一次 test 列表(避免每个测试都重新枚举)。这里曾是静默退化点:stderr 被
# 2>/dev/null 吞掉、部分输出被当真——run 40177906937 的 24.03-LTS-SP2 上一次
# 瞬态失败使 11 个基准中恰 1 个(memcpy_l2_cache_size)没匹配上、被记成「缺席
# skip」(同 job 更早的 allquality 全量扫描里该测试存在且 pass;10-23/10-24 同
# OS 同测试均为正常数值——既非崩溃也非缺席;该次 rc 已不可考,rc=0 的部分输出
# 变体无法被「rc≠0 或空表」拦截)。BENCH_TESTS 声明为跨三系列交集,正常应全部
# 在列(3 run × 15 OS = 495 行实证:唯一一次缺席就是上述事故)——故捕获有效性
# = rc=0 且非空且交集完整;任一不满足即重试一次,连续失败中止采集(退出码交
# verify 闸门红,现场在 list-tests.err)。
LIST_ERR="$OUTDIR/list-tests.err"
capture_list() {
  lrc=0
  LIST="$("$BIN" --list-tests 2>"$LIST_ERR")" || lrc=$?
}
list_valid() {
  # grep -qxF:字面整行匹配。-x 不等于字面匹配,grep -qx 'a.c' 会误命中 'abc'
  # (BRE 元字符);测试名当前全是 C 标识符,但名单演进后元字符名会造成
  # 缺席/在场判定静默出错,-F 关闭模式解释。
  [ "$lrc" -eq 0 ] && [ -n "$LIST" ] || return 1
  local t
  for t in "${BENCH_TESTS[@]}"; do
    printf '%s\n' "$LIST" | grep -qxF -- "$t" || return 1
  done
}
capture_list
if ! list_valid; then
  echo "benchmark: --list-tests invalid (rc=$lrc; empty or BENCH_TESTS not all listed); stderr attempt#1:" >&2
  cat "$LIST_ERR" >&2
  echo "benchmark: retrying --list-tests once ..." >&2
  capture_list
  if ! list_valid; then
    echo "benchmark: ABORT — --list-tests failed/incomplete twice (rc=$lrc); stderr attempt#2:" >&2
    cat "$LIST_ERR" >&2
    exit 1
  fi
fi

OUT="$OUTDIR/benchmark.tsv"
: > "$OUT"
printf 'test\twall_seconds\tloop_count\n' >> "$OUT"

for t in "${BENCH_TESTS[@]}"; do
  if ! printf '%s\n' "$LIST" | grep -qxF -- "$t"; then
    # 不可达防御:list_valid 已保证 BENCH_TESTS 全部在列。若真到这(校验逻辑
    # 被改坏),按 fail 落表交闸门红——绝不静默降级 skip(缺席=异常,见上)。
    printf '%s\tfail\t0\n' "$t" >> "$OUT"
    echo "benchmark: $t: absent from --list-tests despite validated capture (logic error)" >&2
    continue
  fi
  LOOP=$DEFAULT_LOOP
  case "$t" in
    zstd|zlib) LOOP=$LONG_LOOP ;;
  esac
  YAML="$OUTDIR/$t.yaml"
  LOG="$OUTDIR/$t.log"
  rm -f "$YAML" "$LOG"
  # 每测试输出留档:进程级失败(loader 错误/被杀/崩溃前的部分输出)不再 >/dev/null 吞掉
  rc=0
  "$BIN" --max-test-loop-count "$LOOP" --retest-on-failure=0 \
          -n 1 -e "$t" -o "$YAML" >"$LOG" 2>&1 || rc=$?
  # 只取目标测试自己的 result/test-runtime:-e 单测运行会附带伴随段(mce_check),
  # 原 grep|head -1 取首个匹配依赖「目标段恰好在前」的隐式顺序。每个 "- test: "
  # 行都重置匹配状态:目标段缺字段/被截断时不跨段读伴随段(否则 RES/RT 会窃取
  # 下一测试段的数据——错误数值写进目标行且闸门无从识别)。
  RES=$(awk -v t="$t" '/^- test: / {in_t=($0 == "- test: " t)}
                       in_t && /^  result:/ {print $2; exit}' "$YAML" 2>/dev/null)
  RT=$(awk -v t="$t" '/^- test: / {in_t=($0 == "- test: " t)}
                      in_t && /^  test-runtime:/ {print $2; exit}' "$YAML" 2>/dev/null)
  if [ "$rc" -ne 0 ]; then
    printf '%s\tfail\t%s\n' "$t" "$LOOP" >> "$OUT"
    echo "benchmark: $t: FAILED rc=$rc; tail of $t.yaml:"
    tail -n 15 "$YAML" 2>/dev/null
    echo "benchmark: $t: tail of $t.log:"
    tail -n 15 "$LOG" 2>/dev/null
    continue
  fi
  case "$RES" in
    pass)
      if [ -n "$RT" ]; then
        # RT 原样入表(含潜在的格式漂移);闸门按 float 口径统一判别
        printf '%s\t%s\t%s\n' "$t" "$RT" "$LOOP" >> "$OUT"
      else
        printf '%s\tfail\t%s\n' "$t" "$LOOP" >> "$OUT"
        echo "benchmark: $t: result=pass but no test-runtime — YAML tail:"
        tail -n 15 "$YAML" 2>/dev/null
        echo "benchmark: $t: tail of $t.log:"
        tail -n 15 "$LOG" 2>/dev/null
      fi
      ;;
    skip)
      printf '%s\tskip\t%s\n' "$t" "$LOOP" >> "$OUT"
      echo "benchmark: $t: runtime skip (no data point)"
      ;;
    *)
      printf '%s\tfail\t%s\n' "$t" "$LOOP" >> "$OUT"
      echo "benchmark: $t: result='${RES:-<missing>}' (not pass/skip) — YAML tail:"
      tail -n 15 "$YAML" 2>/dev/null
      echo "benchmark: $t: tail of $t.log:"
      tail -n 15 "$LOG" 2>/dev/null
      ;;
  esac
done

echo "benchmark written: $OUT"
cat "$OUT"
