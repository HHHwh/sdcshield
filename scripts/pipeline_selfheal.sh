#!/bin/bash
# pipeline_selfheal.sh — @reboot 自愈: 机器重启落在 23:00–09:00 窗口内则重新
# 拉起 pipeline(锁语义在 task_loop.py pipeline 内部: 已在跑则秒退,双保险)。
# 本机频繁重启是常态(2026-11-12 当天 13h 内 5 次 boot),spec §2 崩溃语义配套件。
set -u
HOUR=$(date +%H); HOUR=${HOUR#0}
if [ "$HOUR" -ge 23 ] || [ "$HOUR" -lt 9 ]; then
  mkdir -p "$HOME/.local/state/sdcshield-task-loop"
  cd /home/sdc/wangxu/operator/sdcshield-autoci || exit 0
  exec timeout 39600 /usr/bin/python3 scripts/task_loop.py pipeline \
    >> "$HOME/.local/state/sdcshield-task-loop/pipeline.log" 2>&1
fi
exit 0
