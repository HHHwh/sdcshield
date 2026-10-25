#!/usr/bin/env python3
# benchmark-to-json.py — benchmark.tsv → github-action-benchmark custom JSON。
#
# 输入:benchmark.sh 产出的 benchmark.tsv(列:test\twall_seconds\tloop_count;
#   无数据行的测试 wall_seconds 为 skip(缺席或运行时跳过)或 fail(在列但未
#   产出可用数据))。
# 输出:[{"name": <test>, "value": <wall_seconds>, "unit": "s"}, ...](skip/fail
#   等一切非数值行剔除;闸门侧的状态判别见 benchmark-gate.py, 口径一致)。
# 供 benchmark-action/github-action-benchmark 的 tool: custom 消费,趋势线只取
# 固定基准 SP(24.03-LTS-SP3),跨 commit 可比。
#
# SPDX-License-Identifier: Apache-2.0
import json
import sys

if len(sys.argv) != 3:
    sys.exit(f"usage: {sys.argv[0]} <benchmark.tsv> <out.json>")

tsv_path, out_path = sys.argv[1], sys.argv[2]
entries = []
with open(tsv_path, encoding="utf-8") as f:
    f.readline()  # header: test\twall_seconds\tloop_count
    for line in f:
        parts = line.rstrip("\n").split("\t")
        if len(parts) != 3:
            continue
        name, wall, _loop = parts
        try:
            value = float(wall)
        except ValueError:
            continue  # skip 行(该 OS 无此测试)
        entries.append({"name": name, "value": value, "unit": "s"})

with open(out_path, "w", encoding="utf-8") as f:
    json.dump(entries, f, indent=2)
    f.write("\n")
print(f"benchmark json written: {out_path} ({len(entries)} entries)")
