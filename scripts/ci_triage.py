#!/usr/bin/env python3
# ci_triage.py — 兼容薄壳(过渡期,PR-1a): 全部职责已搬至 task_loop.py。
#
# SPDX-License-Identifier: Apache-2.0
#
# 必须用**模块别名**而非 `from task_loop import *`: 测试做
# `import ci_triage as ct` 后 `monkeypatch.setattr(ct, "gh_json", ...)`
# ——星号 re-export 会产生两份命名空间,monkeypatch 打在壳模块上、
# task_loop 内部调用看不见,31 个用例全部失效。别名让 ct **就是**
# task_loop 模块对象,补丁直接生效。PR-5(cron 切换并观察后)删除本壳。
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import task_loop  # noqa: E402

sys.modules["ci_triage"] = task_loop

if __name__ == "__main__":
    sys.exit(task_loop.main())
