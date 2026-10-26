#!/usr/bin/env python3
# benchmark-gate.py — benchmark.tsv 状态分类的单一权威(single source of truth)。
#
# 背景:verify 步骤与 benchmark-trend 消费步骤原先各自在 YAML 里内联 awk 判定
# ($2 != "skip" 即 ok), 三份拷贝口径漂移——闸门按「非 skip」计数, 而消费方
# benchmark-to-json.py 按「float 可解析」剔除, 非 skip 非数值的行可同时骗过
# 两道闸门(空 JSON 流入 gh-pages 的威胁模型换壳重现)。本脚本把分类收敛到
# 一处, 两个步骤共用同一口径:
#
#   ok   = wall_seconds 为正的有限数值(float 可解析、math.isfinite 且 >0,
#          与 benchmark-to-json.py 完全一致; 墙钟 ≤0 与 nan/inf 同属不可用
#          数据——物理上不可能的值不做趋势基线)
#   skip = "skip"——运行时跳过(result: skip; 缺席在采集侧 benchmark.sh 已
#          升级为中止/不产 TSV), 良性, 无数据点
#   fail = "fail"——在列但未产出可用数据(进程失败/测试 fail/字段缺失), 以及
#          一切非 skip 非正有限数值的行(格式漂移/nan/inf/非正墙钟, 按消费方
#          口径视为坏行)
#
# 用法: benchmark-gate.py {verify|trend} <benchmark.tsv>
#   verify 角色: ok=0(采集整体废掉)或 fail>0 → ::error:: + exit 1;
#               skip>0 → ::warning:: 点名(数据点缺失, 可观测不红);
#               行完备性:BENCH_TESTS(从 benchmark.sh 解析, 单一权威)中任一
#               测试无行 → ::error:: + exit 1——benchmark.sh 无 set -e 且
#               printf 写入错误不检查, 磁盘写满/截断可留下「有 ok 行但缺行」
#               的 TSV 且 exit 0, 闸门是行完备性的最后防线
#   trend  角色: ok=0 → ::warning:: + exit 1(不产 JSON、不更新 gh-pages);
#               ok>0 → exit 0(fail/skip 行不进 JSON, 数据点缺失由 verify 报;
#               行完备性报警同样归 verify——缺行只意味着当日无新数据点,
#               已发布的历史点不受影响, trend 不越位)
# TSV 缺失/不可读/非 UTF-8: 两角色都按「无数据」处理(verify 红 / trend 不
# 发布), 打 ::warning:: 说明具体原因, 不抛异常不红趋势 job(趋势更新非闸门);
# verify 的 ::error:: 走专属归因(采集在写表前中止/文件损坏), 不落入「表头
# 漂移/行截断」——那是对不存在文件的失真归因。
# 表头契约:首行必须是 test\twall_seconds\tloop_count(与 benchmark.sh 写入方
# 和 benchmark-to-json.py 一致)。盲跳首行的旧写法在格式演进(加列/改列序/丢
# 表头)时会把首行真实数据静默吃掉——数据点无声丢失;表头不符按格式漂移处理
# (verify 红 / trend 不发布), 强制三处同步演进。
#
# SPDX-License-Identifier: Apache-2.0
import math
import re
import sys
from pathlib import Path

EXPECTED_HEADER = "test\twall_seconds\tloop_count"


def expected_tests():
    """TSV 行完备性的期望名单:从同目录 benchmark.sh 的 BENCH_TESTS 数组解析。

    名单从 producer 本体解析而非在 gate 里复制第二份——单一权威, 名单演进
    (加/删基准)自动跟随。解析失败返回 None:此时 benchmark.sh 自身也无法运行
    (数组语法坏), 采集根本产不出 TSV → ok=0 红, 不存在静默漏洞, 故仅告警
    跳过完备性校验而不红。
    """
    try:
        text = (Path(__file__).resolve().parent / "benchmark.sh").read_text(encoding="utf-8")
    except OSError:
        return None
    # 兼容多行(原生态: ") 独立成行, 行尾 \ 续行)与单行两种数组书写;
    # 数组元素是 C 标识符测试名, 不含 ")", 首个 ")" 即数组收尾
    m = re.search(r"^BENCH_TESTS=\((.*?)\)", text, re.S | re.M)
    if not m:
        return None
    # bash 数组字面量:空白分隔;行尾续行符 \<newline> 先归一为空白
    names = m.group(1).replace("\\\n", " ").split()
    return names or None


def classify(path):
    """→ (ok, skips, fails, header_ok, readable)。header_ok=False 表示表头
    不符契约(含空文件/丢表头/格式演进);readable=False 表示文件缺失/不可读/
    非 UTF-8(已打 ::warning:: 给出具体原因), 两者都由调用方按角色决定
    红/不发布——但 readable=False 走专属归因文案, 不落入「表头漂移/行
    截断」:那是给「文件在但内容坏」的归因, 文件压根不存在时是失真归因
    (典型根因:benchmark.sh --list-tests 双败 ABORT 在写 TSV 之前退出)。"""
    ok, skips, fails = [], [], []
    header_ok = False
    readable = True
    try:
        with open(path, encoding="utf-8") as f:
            first = f.readline().rstrip("\n")
            header_ok = first == EXPECTED_HEADER
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if len(parts) == 1 and parts[0] == "":
                    continue  # 空行
                if len(parts) != 3:
                    # 残行/多列:消费方(benchmark-to-json.py 按 len==3 且 float
                    # 双重过滤)会整行丢弃, 数据点无声丢失——按坏行报警
                    fails.append(parts[0] if parts else "?")
                    continue
                name, wall, _loop = parts
                if wall == "skip":
                    skips.append(name)
                elif wall == "fail":
                    fails.append(name)
                else:
                    try:
                        value = float(wall)
                    except ValueError:
                        fails.append(name)  # 非 skip 非数值:消费方口径下的坏行
                    else:
                        if math.isfinite(value) and value > 0:
                            ok.append(name)
                        else:
                            # nan/inf/1e999→inf:float 可解析, 但 RFC 8259 无法表示,
                            # json.dump 默认写出 NaN/Infinity 非法 JSON, 下游 strict
                            # 解析(JS JSON.parse / benchmark-action)必挂——按格式
                            # 漂移(坏行)红, 而非放行染红 trend job。
                            # ≤0:墙钟为非正量纲(计时回拨/解析错位), 物理上不可能
                            # 的值不做趋势基线(alert-threshold 对非正基线行为未
                            # 定义)——同属不可用数据, 按 bad 行红
                            fails.append(name)
    except (OSError, UnicodeDecodeError) as err:
        # UnicodeDecodeError 是 ValueError 子类, 不在 OSError 之下——非 UTF-8
        # 字节的 TSV(传输损坏/磁盘错)若不显式接住会裸 traceback, verify 丢
        # 失全部分类诊断(只剩堆栈), 违背本脚本「不抛异常」的书面契约
        print(f"::warning::benchmark-gate: cannot read/decode {path} ({err})")
        readable = False
    return ok, skips, fails, header_ok, readable


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("verify", "trend"):
        sys.exit(f"usage: {sys.argv[0]} {{verify|trend}} <benchmark.tsv>")
    role, path = sys.argv[1], sys.argv[2]
    ok, skips, fails, header_ok, readable = classify(path)
    print(f"benchmark-gate[{role}]: ok={len(ok)} skip={len(skips)} "
          f"fail={len(fails)} header={'ok' if header_ok else 'BAD'}")
    if role == "verify":
        rc = 0
        if not readable:
            # 文件缺失/不可读/非 UTF-8:专属归因(具体原因见 classify 打的
            # ::warning::)。不走下方「文件在但内容坏」的表头/缺行文案——对
            # 不存在的文件那是失真归因;典型根因是 benchmark.sh --list-tests
            # 双败 ABORT(在写 TSV 之前 exit 1, 真因现场在上方 benchmark.log
            # 的 ABORT 行与 artifact 的 list-tests.err)。
            print("::error::benchmark.tsv 缺失/不可读/非 UTF-8(原因见上 ::warning::)"
                  "——采集在写表前中止(benchmark.sh ABORT)或文件损坏/未产出")
            rc = 1
        else:
            if not header_ok:
                print("::error::benchmark.tsv 表头不符契约(预期 'test\\twall_seconds\\t"
                      "loop_count')——格式漂移/丢表头会吃掉首行数据, 须同步 benchmark.sh/"
                      "benchmark-gate.py/benchmark-to-json.py 三处")
                rc = 1
            if not ok:
                print(f"::error::benchmark.tsv 无数据行(ok=0, skip={len(skips)}, "
                      f"fail={len(fails)})——采集整体废掉或基准全部失败(见上方 benchmark.log)")
                rc = 1
            if fails:
                print(f"::error::benchmark 基准失败 {len(fails)} 行: {' '.join(fails)}"
                      "——在列但未产出可用数据, 现场见 benchmark.log 与 artifact 的 "
                      "benchmark/<test>.yaml|.log")
                rc = 1
            expected = expected_tests()
            if expected is None:
                print("::warning::benchmark-gate: cannot parse BENCH_TESTS from benchmark.sh"
                      " — TSV 行完备性校验跳过(状态分类判定不受影响)")
            else:
                present = set(ok) | set(skips) | set(fails)
                missing = [t for t in expected if t not in present]
                if missing:
                    print(f"::error::benchmark.tsv 缺 {len(missing)} 行(预期在列但无记录): "
                          f"{' '.join(missing)}——producer 写入失败/截断, 数据点无声丢失")
                    rc = 1
            if skips:
                print(f"::warning::benchmark 无数据点 {len(skips)} 行(运行时跳过): "
                      f"{' '.join(skips)}——对应趋势数据点将缺失")
        sys.exit(rc)
    # trend 角色:消费侧拦截, 不产 JSON 不 push;报警归 verify 侧, 不重复红
    if not readable:
        sys.exit(1)  # 已打 ::warning::(原因);不产 JSON 不 push
    if not header_ok:
        print("::warning::benchmark.tsv 表头不符契约——不更新 gh-pages 趋势")
        sys.exit(1)
    if not ok:
        print(f"::warning::benchmark.tsv 无数据行(ok=0;verify 闸门已红或已告警)"
              "——不更新 gh-pages 趋势")
        sys.exit(1)
    sys.exit(0)


if __name__ == "__main__":
    main()
