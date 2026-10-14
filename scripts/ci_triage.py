#!/usr/bin/env python3
# ci_triage.py — CI 无人值守分诊引擎(cron 驱动,pr_loop 的姊妹工具)。
#
# 定位: 周期扫描目标分支 CI(workflow run)失败 → 开 issue 跟踪 → 派 agent
# 自动修复(fix 分支 → PR → pr_loop 闭环)。与 pr_loop.py 互补: pr_loop 管
# 「已存在 PR 的检视-修复循环」,本工具管「CI 失败 → 修复 PR 的自动产生」。
# 设计规格见 .superpowers/sdd/2026-10-13-unattended-ci-triage/。
#
# 子命令(本任务仅注册骨架,行为在后续任务实现):
#   scan      扫描受监控分支的 CI 失败并更新状态(Task 2)
#   status    打印当前状态总览(Task 3)
#   dry-run   只读演练: 展示将执行的动作而不实际执行(Task 4)
#
# 状态存储: ~/.local/state/sdcshield-ci-triage/state.json(原子写: 先写
# state.json.tmp 再 os.replace;env CITRIAGE_STATE_DIR 重定向,测试用)。
#
# 依赖: Python 3.11 标准库,零第三方运行时依赖;gh CLI 凭据全权交给 gh
# (与 pr_loop.py 同策略,本脚本不管理 token)。
#
# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass
from typing import Optional

# 兼容两种启动方式: 仓库根 `python3 scripts/ci_triage.py` 与 scripts/ 目录
# 内引用。后续任务复用 pr_loop.py 的 gh 封装时,`import pr_loop` 需 scripts/
# 在 sys.path(与测试文件的 sys.path.insert 同理)。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# ============ 常量(监控范围与节奏,spec §2/§14) ============

# 受监控的 CI workflow 名单(env CITRIAGE_WORKFLOWS 逗号分隔覆盖,plan §14)
WORKFLOWS = [w for w in os.environ.get("CITRIAGE_WORKFLOWS",
             "Multi-OS Verify,Sanitizer,CodeQL,Security").split(",") if w]
# 只处理这两类事件触发的 run(避免拉取全部历史 push 的噪音)
EVENTS = {"schedule", "push"}
# 受监控分支: main 的 CI 红 = 仓库红线,修复优先级最高
MONITORED_BRANCH = "main"
# 单个修复 agent 的最长运行时间(秒)——超时视为失败,计数升级
AGENT_TIMEOUT_SEC = 3600
# 回看窗口(天): 每轮扫描只看最近 N 天的 run,避免翻陈年历史
LOOKBACK_DAYS = 3

# 状态目录与状态文件(env CITRIAGE_STATE_DIR 重定向)。导入时快照供外部
# 引用(状态文件名恒为 state.json);StateStore 构造时再按当前 env 解析——
# 测试经 monkeypatch.setenv 重定向须在构造后生效,不能用导入时的快照。
STATE_DIR = os.path.expanduser(
    os.environ.get("CITRIAGE_STATE_DIR", "~/.local/state/sdcshield-ci-triage"))
STATE_FILE_NAME = "state.json"

# state 顶层结构: {"entries": [...], "auth_failure_streak": 0}
# auth_failure_streak 是 gh 认证失败的熔断计数,跨条目累计(spec §7)
DEFAULT_STATE = {"entries": [], "auth_failure_streak": 0}


# ============ 状态条目 ============

@dataclass
class Entry:
    """单个失败 workflow run 的跟踪条目(state.json 的 entries[] 元素)。

    生命周期: 发现失败(attempts=1) → 派 agent 修复 → fix_branch/PR 跟踪 →
    修复合入或放弃。字段经 to_dict/from_dict 与 JSON 往返等值。
    """
    workflow: str                       # workflow 名(如 "Multi-OS Verify")
    run_id: int                         # GitHub Actions run id
    status: str                         # 条目状态机(open/fixing/fixed/abandoned…)
    issue_number: Optional[int]         # 跟踪 issue 编号(未开 = None)
    fix_branch: str                     # 修复分支名(未建 = 空串)
    pr_number: Optional[int]            # 修复 PR 编号(未开 = None)
    attempts: int                       # 修复尝试次数(含进行中)
    rounds: int                         # pr_loop 检视-修复循环轮数
    agent_pid: Optional[int]            # 修复 agent 进程 PID(未派 = None)
    agent_started_at: Optional[str]     # agent 启动时间(ISO8601,超时判据)
    agent_log: Optional[str]            # agent 日志文件路径(失败现场留存)
    last_dispatched_head: Optional[str] # 最近一次派 agent 时的 head SHA(幂等)
    extra_runs: list[int]               # 同 workflow 后续失败 run id
    created_at: str                     # 条目创建时间(ISO8601)
    updated_at: str                     # 最近更新时间(ISO8601)

    # 注: 全部字段显式必填(含 extra_runs)——不做隐式默认,构造方须给全 16 字段。

    def to_dict(self) -> dict:
        """state.json 序列化形态。"""
        return {
            "workflow": self.workflow,
            "run_id": self.run_id,
            "status": self.status,
            "issue_number": self.issue_number,
            "fix_branch": self.fix_branch,
            "pr_number": self.pr_number,
            "attempts": self.attempts,
            "rounds": self.rounds,
            "agent_pid": self.agent_pid,
            "agent_started_at": self.agent_started_at,
            "agent_log": self.agent_log,
            "last_dispatched_head": self.last_dispatched_head,
            "extra_runs": list(self.extra_runs),
            "created_at": self.created_at,
            "updated_at": self.updated_at,
        }

    @classmethod
    def from_dict(cls, d: dict) -> "Entry":
        """从 state.json 还原类型化 Entry。"""
        return cls(
            workflow=d["workflow"],
            run_id=d["run_id"],
            status=d["status"],
            issue_number=d["issue_number"],
            fix_branch=d["fix_branch"],
            pr_number=d["pr_number"],
            attempts=d["attempts"],
            rounds=d["rounds"],
            agent_pid=d["agent_pid"],
            agent_started_at=d["agent_started_at"],
            agent_log=d["agent_log"],
            last_dispatched_head=d["last_dispatched_head"],
            extra_runs=list(d["extra_runs"]),
            created_at=d["created_at"],
            updated_at=d["updated_at"],
        )


# ============ 状态存储(原子写) ============

class StateStore:
    """state.json 的读写门面。

    路径在构造时按 CITRIAGE_STATE_DIR 解析(测试经 env 重定向 tmp_path)。
    save 原子性: 写 state.json.tmp → os.replace 原子替换——中断(进程被杀/
    磁盘满)时旧 state.json 完整可读,绝不留半份 JSON;失败路径清理 tmp,
    复跑 load 不会被 tmp 残留污染(load 只读 state.json,不碰 tmp)。
    """

    def __init__(self, state_dir: Optional[str] = None):
        d = state_dir if state_dir is not None else os.environ.get(
            "CITRIAGE_STATE_DIR", STATE_DIR)
        self.state_dir = os.path.expanduser(d)
        self.state_file = os.path.join(self.state_dir, STATE_FILE_NAME)
        self.state_tmp = self.state_file + ".tmp"

    def load(self) -> dict:
        """读 state.json;不存在时返回初始空状态(首跑语义)。

        只读 state.json——tmp 是 save 的中间产物,绝不在 load 路径出现。
        """
        try:
            with open(self.state_file, "r", encoding="utf-8") as f:
                return json.load(f)
        except FileNotFoundError:
            return {"entries": [], "auth_failure_streak": 0}

    def save(self, state: dict) -> None:
        """原子写: tmp → os.replace。替换失败时清理 tmp,旧文件不动。"""
        os.makedirs(self.state_dir, exist_ok=True)
        try:
            with open(self.state_tmp, "w", encoding="utf-8") as f:
                json.dump(state, f, ensure_ascii=False, indent=2)
                f.write("\n")
                f.flush()
                os.fsync(f.fileno())
            os.replace(self.state_tmp, self.state_file)
        except BaseException:
            # 中断现场清理: 保证「复跑 load 不读到 tmp」的不变式
            try:
                os.unlink(self.state_tmp)
            except FileNotFoundError:
                pass
            raise

    @staticmethod
    def entries(state: dict) -> list[Entry]:
        """state["entries"] → 类型化 Entry 列表。"""
        return [Entry.from_dict(d) for d in state.get("entries", [])]


# ============ 子命令(Task 2/3/4 实现,本任务仅占位) ============

def cmd_scan(args: argparse.Namespace) -> int:
    """扫描受监控分支的 CI 失败并更新状态。"""
    raise NotImplementedError("to be implemented in Task 2")


def cmd_status(args: argparse.Namespace) -> int:
    """打印当前状态总览。"""
    raise NotImplementedError("to be implemented in Task 3")


def cmd_dry_run(args: argparse.Namespace) -> int:
    """只读演练: 展示将执行的动作而不实际执行。"""
    raise NotImplementedError("to be implemented in Task 4")


# ============ 入口 ============

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="ci_triage.py",
        description="CI 无人值守分诊引擎(cron 驱动,pr_loop 的姊妹工具)。"
                    "周期扫描 main 分支 CI 失败 → 开 issue → 派 agent 修复 → "
                    "PR 闭环。规格见 .superpowers/sdd/2026-10-13-unattended-ci-triage/。")
    sub = p.add_subparsers(dest="cmd", required=True)

    ps = sub.add_parser("scan", help="扫描受监控分支的 CI 失败并更新状态(Task 2)")
    ps.set_defaults(func=cmd_scan)

    pt = sub.add_parser("status", help="打印当前状态总览(Task 3)")
    pt.set_defaults(func=cmd_status)

    pd = sub.add_parser("dry-run", help="只读演练: 展示将执行的动作而不实际执行(Task 4)")
    pd.set_defaults(func=cmd_dry_run)
    return p


def main(argv: Optional[list] = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
