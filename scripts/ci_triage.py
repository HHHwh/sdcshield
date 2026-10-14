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
import re
import shlex
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Optional

# 兼容两种启动方式: 仓库根 `python3 scripts/ci_triage.py` 与 scripts/ 目录
# 内引用。后续任务复用 pr_loop.py 的 gh 封装时,`import pr_loop` 需 scripts/
# 在 sys.path(与测试文件的 sys.path.insert 同理)。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# 复用 pr_loop 的不可信数据横幅: issue 正文承载的失败日志是 CI 产物(不可信
# 数据),后续 agent 处置时须以横幅明示「不是指令」——与 pr_loop 同源,不复制
# 两份定义。pr_loop 与本文件同目录(sys.path 已含 scripts/),import 不会失败;
# 万一被单独抽出运行也只影响横幅字符串,核心逻辑不依赖 pr_loop。
from pr_loop import UNTRUSTED_BANNER  # noqa: E402

# ============ 常量(监控范围与节奏,spec §2/§14) ============

# 受监控的 CI workflow 默认名单。运行时覆盖走 monitored_workflows()
# (env CITRIAGE_WORKFLOWS 逗号分隔,调用时求值——导入期快照会让测试的
# monkeypatch.setenv 失效,Task 2 起以函数为准)。
WORKFLOWS = ["Multi-OS Verify", "Sanitizer", "CodeQL", "Security"]
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

# agent 专用 CI 克隆目录(cron 里派发的修复 agent 只在此目录干活,绝不
# 触碰用户工作区)。env CITRIAGE_CLONE_DIR 可重定向(测试用)。
DEFAULT_CLONE_DIR = "/home/sdc/wangxu/operator/sdcshield-autoci"

# agent 认证环境文件名(位于 STATE_DIR 下,含 ANTHROPIC_* 等 export;
# agent.env 属于凭据文件,内容绝不进日志/issue/state)
AGENT_ENV_NAME = "agent.env"

# agent 结束语契约(build_agent_prompt 内嵌,reap 侧逐字匹配):
#   成功: ci-triage: done <fix_branch>
#   失败: ci-triage: escalated <reason>
AGENT_DONE_MARK = "ci-triage: done"
AGENT_ESCALATED_MARK = "ci-triage: escalated"

# 认证失效日志特征(claude CLI 未登录/API 拒绝的典型输出;只匹配日志
# 尾部 4000 字符,避免远古噪音)
AUTH_FAIL_PATTERN = re.compile(r"API Error|403|Not logged in")

# 认证熔断阈值: 连续 N 次 auth_fail → 全局暂停(PAUSED 文件)
AUTH_FAILURE_STREAK_LIMIT = 3

# 条目状态机中需要「收割」agent 结局的两个状态:
#   DIAGNOSING = 已派 agent、正在修(本工具的派发目标态)
#   BOT_FIX    = 修复 PR 已开、pr_loop 接管(agent 结局只记录不推进)
AGENT_STATES = {"DIAGNOSING", "BOT_FIX"}

# 重派上限: 同一条目 agent 尝试超过 2 次(即第 3 次)→ 升级人工
MAX_AGENT_ATTEMPTS = 2

# state 顶层结构: {"entries": [...], "auth_failure_streak": 0}
# auth_failure_streak 是 gh 认证失败的熔断计数,跨条目累计(spec §7)
DEFAULT_STATE = {"entries": [], "auth_failure_streak": 0}

# 条目状态机中「活跃」的判定: 非 DONE/ESCALATED 即活跃(spec §5 同根因收敛)
TERMINAL_STATUSES = {"DONE", "ESCALATED"}

# gh 子进程超时(秒),与 pr_loop.GH_TIMEOUT 一致
GH_TIMEOUT_SEC = 45
# gh run list 单次拉取条数上限(60 条 ≈ 4 个 workflow × 3 天 × 多次/天)
RUN_LIST_LIMIT = 60
# issue 正文里失败日志摘录的最大行数(防超长正文)
LOG_EXCERPT_MAX_LINES = 200
# 幂等标记: issue 正文内嵌 <!-- ci-triage:run:<databaseId> -->,重建前先搜
RUN_MARKER_FMT = "<!-- ci-triage:run:{run_id} -->"


def monitored_workflows() -> list[str]:
    """受监控 workflow 名单——调用时读 env(测试可 monkeypatch.setenv)。

    env 存在但为空串(或全空白项,过滤后为空)视同未设置 → 回落默认 4 个
    ——否则空名单会让扫描静默返回零红 run,无人值守失明(检视 Important#2)。
    """
    raw = os.environ.get("CITRIAGE_WORKFLOWS") or \
        "Multi-OS Verify,Sanitizer,CodeQL,Security"
    workflows = [w.strip() for w in raw.split(",") if w.strip()]
    return workflows or ["Multi-OS Verify", "Sanitizer", "CodeQL", "Security"]


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


# ============ gh CLI 封装(凭据全权交给 gh,与 pr_loop 同策略) ============

class GhError(RuntimeError):
    """gh CLI 调用失败(未认证/网络/端点不存在等)。"""


def gh_json(args: list[str], input_text: Optional[str] = None):
    """运行 `gh <args...>` → 解析 JSON 输出。失败抛 GhError。

    input_text 给定时写入子进程 stdin(gh issue create --body-file - 的
    正文通道——避免超长正文/特殊字符经 argv 传递,也规避 shell 注入面)。
    timeout 45s 与 pr_loop.GH_TIMEOUT 一致;非零退出统一转 GhError。
    """
    try:
        proc = subprocess.run(["gh", *args], capture_output=True, text=True,
                              timeout=GH_TIMEOUT_SEC, input=input_text)
    except FileNotFoundError:
        raise GhError("找不到 gh CLI——请安装并认证: gh auth login")
    except subprocess.TimeoutExpired:
        raise GhError(f"gh {' '.join(args[:3])}… 超时({GH_TIMEOUT_SEC}s)")
    if proc.returncode != 0:
        err = " ".join(proc.stderr.strip().split())[:300]
        raise GhError(f"gh {' '.join(args[:3])}… 失败"
                      f"(exit {proc.returncode}): {err}")
    out = proc.stdout.strip()
    if not out:
        return None    # 部分子命令(如 issue comment)成功时无输出
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        return out     # 非输出 JSON 的子命令(run view --log-failed)原样返回


def repo_slug() -> str:
    """目标仓库 owner/repo。env CITRIAGE_REPO 覆盖,否则取 origin remote。"""
    env_slug = os.environ.get("CITRIAGE_REPO")
    if env_slug:
        return env_slug.strip()
    try:
        proc = subprocess.run(["git", "remote", "get-url", "origin"],
                              capture_output=True, text=True,
                              timeout=GH_TIMEOUT_SEC)
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        raise GhError(f"解析 origin remote 失败: {exc}")
    if proc.returncode != 0 or not proc.stdout.strip():
        raise GhError("git remote get-url origin 无输出"
                      "——可用 env CITRIAGE_REPO=owner/repo 指定")
    # 归一化两种形态: git@…:owner/repo.git 与 https://host/owner/repo.git
    url = proc.stdout.strip()
    m = re.search(r"[:/]([^/:]+/[^/]+?)(?:\.git)?$", url)
    if not m:
        raise GhError(f"无法从 origin URL 解析 owner/repo: {url}")
    return m.group(1)


# ============ 红扫描(gh run list → 四维过滤) ============

def _parse_created_at(s: str) -> Optional[datetime]:
    """GitHub createdAt(ISO8601,含 Z)→ aware datetime;解析失败返回 None。"""
    try:
        return datetime.fromisoformat(s.replace("Z", "+00:00"))
    except ValueError:
        return None


def scan_red_runs(lookback_days: int = LOOKBACK_DAYS) -> list[dict]:
    """扫描 main 分支最近 lookback_days 天的红 run(受监控 workflow × 事件)。

    gh run list 一次拉足(60 条)后在 Python 侧过滤: workflow 名 ∈ 监控名单、
    event ∈ {schedule, push}、分支 = main、conclusion = failure、
    createdAt 落在回看窗内。分支过滤双保险: gh 参数 --branch main 锚定
    之外,Python 侧对 headBranch(若返回)再判一次。
    """
    slug = repo_slug()
    cutoff = datetime.now(timezone.utc) - timedelta(days=lookback_days)
    runs = gh_json([
        "run", "list", "--repo", slug,
        "--branch", MONITORED_BRANCH,
        "--limit", str(RUN_LIST_LIMIT),
        "--json",
        "databaseId,workflowName,event,conclusion,headBranch,headSha,createdAt,url",
    ])
    if not isinstance(runs, list):
        raise GhError("gh run list 返回非 list")
    workflows = set(monitored_workflows())
    red = []
    for r in runs:
        if r.get("workflowName") not in workflows:
            continue
        if r.get("event") not in EVENTS:
            continue
        if r.get("conclusion") != "failure":
            continue
        # 分支过滤: run list --json 的 headBranch 若返回则须为 main
        # (gh --branch 参数已限,这里是双保险——fake/缓存场景可能混入他分支)
        if r.get("headBranch", MONITORED_BRANCH) != MONITORED_BRANCH:
            continue
        created = _parse_created_at(r.get("createdAt", ""))
        if created is None or created < cutoff:
            continue
        red.append(r)
    return red


# ============ issue 幂等查找 / 创建 / 收敛评论 ============

def _search_marker_issues(state: str, run_id: int) -> Optional[list]:
    """`gh search issues --state <state> "ci-triage:run"` → 结果列表。"""
    res = gh_json([
        "search", "issues", "--repo", repo_slug(),
        "--state", state,
        "--json", "number,title,body",
        "--limit", "50",
        "ci-triage:run",
    ])
    if not isinstance(res, list):
        raise GhError("gh search issues 返回非 list")
    marker = RUN_MARKER_FMT.format(run_id=run_id)
    return [it for it in res if marker in (it.get("body") or "")]


def find_issue_by_run(run_id: int) -> Optional[int]:
    """按幂等标记搜既有 open issue → issue 号;无匹配返回 None。

    只搜 open: 命中判据是 body 内含完整标记
    `<!-- ci-triage:run:<id> -->`,而非标题匹配——标题会被人工改写,
    body 标记是稳定锚。CLOSED 的标记 issue(人工接管信号)不在此返回,
    调用方经 find_closed_marker_issue 另行判定。
    """
    for it in _search_marker_issues("open", run_id):
        return int(it["number"])
    return None


def find_closed_marker_issue(run_id: int) -> Optional[int]:
    """搜 CLOSED issue 中的幂等标记 → issue 号;无匹配返回 None。

    CLOSED 且携带标记 = 人工已显式接管并关闭该 run 的跟踪(spec §8
    人工接管信号)——分诊必须跳过该 run,绝不重建人工关闭的 issue。
    """
    for it in _search_marker_issues("closed", run_id):
        return int(it["number"])
    return None


def _failed_jobs_and_log(run: dict) -> str:
    """`gh run view <id> --log-failed` 摘录(截断 LOG_EXCERPT_MAX_LINES 行)。

    失败日志是不可信 CI 产物——只进 issue 正文(带 UNTRUSTED_BANNER),绝不
    参与本工具的任何控制流判断。截断上限 200 行防超长正文撑爆 issue。
    """
    try:
        out = gh_json(["run", "view", str(run["databaseId"]),
                       "--repo", repo_slug(), "--log-failed"])
    except GhError as exc:
        return f"(拉取失败日志出错: {exc})"
    lines = out.splitlines() if isinstance(out, str) else [str(out)]
    total = len(lines)
    if total > LOG_EXCERPT_MAX_LINES:
        lines = lines[:LOG_EXCERPT_MAX_LINES]
        lines.append(f"…(已截断,原 {total} 行)")
    return "\n".join(lines)


def build_issue_title(run: dict) -> str:
    """issue 标题: `[ci-triage] <Workflow> 红：run <id>（<YYYY-MM-DD UTC>）`。"""
    created = _parse_created_at(run.get("createdAt", "")) or \
        datetime.now(timezone.utc)
    date = created.strftime("%Y-%m-%d")
    return (f"[ci-triage] {run['workflowName']} 红：run "
            f"{run['databaseId']}（{date} UTC）")


def build_issue_body(run: dict) -> str:
    """issue 正文: run 链接 + head SHA + 失败日志摘录 + 幂等标记 + 横幅。

    标记 `<!-- ci-triage:run:<id> -->` 是 find_issue_by_run 的稳定锚,
    必须整体逐字保留(HTML 注释在渲染时不可见,不干扰阅读)。
    """
    marker = RUN_MARKER_FMT.format(run_id=run["databaseId"])
    log_excerpt = _failed_jobs_and_log(run)
    return (
        f"{marker}\n\n"
        f"CI 红 run: {run.get('url', '(无 URL)')}\n\n"
        f"- Workflow: {run['workflowName']}\n"
        f"- Event: {run.get('event', '?')}\n"
        f"- Head SHA: {run.get('headSha', '?')}\n"
        f"- Created: {run.get('createdAt', '?')}\n\n"
        f"### 失败日志摘录(gh run view --log-failed,截断 200 行)\n\n"
        f"```\n{log_excerpt}\n```\n\n"
        f"---\n{UNTRUSTED_BANNER}\n"
    )


def create_issue(run: dict) -> int:
    """建跟踪 issue(正文经 stdin)→ 返回 issue 号。"""
    url = gh_json(["issue", "create", "--repo", repo_slug(),
                   "--title", build_issue_title(run),
                   "--body-file", "-"],
                  input_text=build_issue_body(run))
    # gh issue create 输出 issue URL: https://…/issues/<n>
    m = re.search(r"/issues/(\d+)", str(url))
    if not m:
        raise GhError(f"gh issue create 输出无 issue 号: {url}")
    return int(m.group(1))


def comment_on_issue(issue_number: int, body: str) -> None:
    """在既有 issue 追加一条评论(同根因收敛时挂新 run 链接)。"""
    gh_json(["issue", "comment", str(issue_number),
             "--repo", repo_slug(), "--body", body])


def _now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# ============ 分诊: 新红 run → DETECTED →(建 issue)→ ISSUED ============

def triage_new_red_runs(state: dict, runs: list[dict],
                        dry_run: bool = False) -> list[Entry]:
    """把一批红 run 并入 state,返回本批新建的 Entry 列表。

    与 gh 解耦(gh 调用由 scan_red_runs/find_issue_by_run 等承担,本函数只
    经模块级助手,测试可整体替换):
      - run 已有条目(run_id 主键) → 跳过(幂等);
      - 同 workflow 已有活跃条目(status 非 DONE/ESCALATED) → 同根因收敛:
        挂 extra_runs + 在既有 issue 上追加一条评论,不开新 issue、不派发;
      - 否则: 找既有 open issue(标记匹配,含 state 外人工已开的)→ 复用;
        open 无命中但 CLOSED issue 携带标记 → 人工接管信号,跳过不重建;
        全无 → 建 issue → DETECTED → ISSUED。

    dry_run=True: 只打印将执行的动作,零 gh 写、零 state 变更。
    """
    new_entries: list[Entry] = []
    entries = StateStore.entries(state)
    for run in runs:
        run_id = int(run["databaseId"])
        workflow = run["workflowName"]

        # 幂等: 该 run 已被跟踪(state 主键) → 跳过
        if any(e.run_id == run_id for e in entries):
            print(f"[skip] run {run_id} 已在跟踪(state 命中)")
            continue
        # 已收敛过该 run 的活跃条目(extra_runs 命中) → 跳过
        if any(run_id in e.extra_runs for e in entries):
            print(f"[skip] run {run_id} 已收敛进活跃条目(extra_runs 命中)")
            continue

        # 同根因收敛: 同 workflow 活跃条目 → 挂 extra_runs + 一条评论
        active = next((e for e in entries
                       if e.workflow == workflow
                       and e.status not in TERMINAL_STATUSES), None)
        if active is not None:
            _merge_into_active(state, entries, active, run, dry_run)
            continue

        # 新根因: 建条目(DETECTED)→ 复用或创建 issue → ISSUED
        entry = Entry(
            workflow=workflow, run_id=run_id, status="DETECTED",
            issue_number=None, fix_branch="", pr_number=None,
            attempts=0, rounds=0, agent_pid=None, agent_started_at=None,
            agent_log=None, last_dispatched_head=None, extra_runs=[],
            created_at=_now_iso(), updated_at=_now_iso(),
        )
        if dry_run:
            print(f"[dry-run] 将为 {workflow} run {run_id} 开 issue: "
                  f"标题「{build_issue_title(run)}」")
            # 预演也须模拟收敛语义: 条目入内存 state(不落盘、零 gh 写),
            # 后续同 workflow run 才会正确预演为「收敛」而非再开 issue
            entries.append(entry)
            state["entries"] = [e.to_dict() for e in entries]
            new_entries.append(entry)
            continue
        # 幂等创建: 先搜 open 标记(含 state.json 之外的既有 issue——双源
        # 对账);open 无命中再查 CLOSED 标记——命中 = 人工已接管关闭该 run
        # (spec §8 人工接管信号),跳过不重建(检视 Important#1)。
        issue_no = find_issue_by_run(run_id)
        if issue_no is None:
            closed_no = find_closed_marker_issue(run_id)
            if closed_no is not None:
                print(f"[skip] run {run_id} 的标记 issue #{closed_no} 已被"
                      f"人工关闭(接管信号)——不重建,跳过该 run")
                continue
            issue_no = create_issue(run)
        else:
            print(f"[reuse] run {run_id} 复用既有 issue #{issue_no}")
        entry.issue_number = issue_no
        entry.status = "ISSUED"
        entry.updated_at = _now_iso()
        entries.append(entry)
        state["entries"] = [e.to_dict() for e in entries]
        new_entries.append(entry)
        print(f"[issue] {workflow} run {run_id} → issue #{issue_no}")
    return new_entries


def _merge_into_active(state: dict, entries: list[Entry], active: Entry,
                       run: dict, dry_run: bool) -> None:
    """同根因收敛: 新 run 挂活跃条目 extra_runs,并在既有 issue 追加评论。"""
    run_id = int(run["databaseId"])
    if dry_run:
        issue_ref = (f"#{active.issue_number}"
                     if active.issue_number is not None else "(条目 issue 创建后)")
        print(f"[dry-run] {run['workflowName']} run {run_id} 将收敛进条目 "
              f"run {active.run_id}(extra_runs),评论 issue {issue_ref}")
        return
    active.extra_runs.append(run_id)
    active.updated_at = _now_iso()
    body = (f"同 workflow 新红 run(收敛,不开新 issue): "
            f"{run.get('url', f'run {run_id}')}\n\n"
            f"- Head SHA: {run.get('headSha', '?')}\n"
            f"- Created: {run.get('createdAt', '?')}\n\n"
            f"{RUN_MARKER_FMT.format(run_id=run_id)}\n\n{UNTRUSTED_BANNER}")
    if active.issue_number is not None:
        comment_on_issue(active.issue_number, body)
    state["entries"] = [e.to_dict() for e in entries]
    print(f"[merge] {run['workflowName']} run {run_id} → 条目 run "
          f"{active.run_id}.extra_runs,评论 issue #{active.issue_number}")


# ============ agent 派发与生命周期(Task 3) ============

def _resolve_state_dir() -> str:
    """当前生效的状态目录(调用时读 env——测试 monkeypatch.setenv 生效)。"""
    return os.path.expanduser(
        os.environ.get("CITRIAGE_STATE_DIR", STATE_DIR))


def _agent_log_path(run_id: int) -> str:
    """agent 日志文件路径: <STATE_DIR>/agent-<runid>-<时间戳>.log。

    每次派发独立文件(含秒级时间戳),重派不覆盖上一轮的失败现场。
    """
    ts = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return os.path.join(_resolve_state_dir(), f"agent-{run_id}-{ts}.log")


def build_agent_prompt(entry: Entry, run_detail: dict) -> str:
    """构造派发给修复 agent 的中文 prompt。

    内嵌常量任务契约(复现→诊断→修复→验证→commit -s→push→贴报告)、
    5 条红线、不可信数据横幅、分层复现口径提示、git 单树纪律与结束语
    契约——全部逐字内嵌,不依赖运行时状态。
    """
    fix_branch = entry.fix_branch or f"fix/ci-{entry.run_id}"
    return f"""你是 SDCShield 仓库的 CI 修复 agent,工作目录是仓库的 CI 专用克隆(不是任何用户的工作区)。

## 红线(违反任何一条即整体失败)
1. 绝不推 main——修复只进 {fix_branch} 分支,main 只读。
2. 绝不 merge——不合并任何分支/PR,合并是人的职责。
3. 绝不 resolve——不把 issue/PR 标记 resolved/closed,只回帖报告。
4. 绝不碰用户工作区——你的 cwd 之外(尤其 /home/sdc/wangxu/operator/sdcshield 主工作区)一律只读、最好不进。
5. 未复现不修——复现是修复的前提;复现不出就如实报告 escalated,绝不猜测式修改。

## 任务(修复 CI 红 run {entry.run_id})
- Workflow: {run_detail.get('workflowName', entry.workflow)}
- Run: {run_detail.get('url', f'(run {entry.run_id})')}
- Head SHA: {run_detail.get('headSha', '?')}
- 跟踪 issue: #{entry.issue_number}(最终报告贴在该 issue 下)
- 拉失败日志: gh run view {entry.run_id} --log-failed

## 工作契约(严格按序)
1. 复现: 先读仓库规则(CLAUDE.md、AGENTS.md),按下述分层口径复现失败。
2. 诊断: 定位根因——是代码缺陷、构建环境、还是测试假设过期。
3. 修复: 最小修复,一个补丁一个单元,遵循仓库代码风格。
4. 验证(CLAUDE.md 规则 2): 修复后必须真实自证——构建零新警告、受影响
   行为跑通并引用真实输出、回归 `-e <受影响测试> -t 2000 -n 1` exit: pass;
   不得以「应该能过」替代验证。
5. 提交: git commit -s(DCO: Signed-off-by 最后一行);绝不捆绑多个单元。
6. 推送: push 到 {fix_branch} 分支(绝不推 main)。
7. 报告: 把修复报告(根因、修法、验证证据)贴到 issue #{entry.issue_number}。

## 分层复现口径
- Sanitizer 红: 在本机克隆里按同 sanitizer 配置重跑(能直接复现)。
- Multi-OS Verify 红: 优先在本机容器/同环境复现;确实不可行时基于
  失败日志与代码路径做日志级诊断,并在报告里如实声明验证边界。
- CodeQL / Security 告警: 无需运行复现——按告警定位到具体代码行,
  判定真/误报,真报走修复,误报给出依据。

## git 单树纪律
从 origin/main 起: `git checkout -B {fix_branch} origin/main`,全程只在此
分支提交;结束时工作区回到干净的 main(`git checkout main && git status`
须 clean),不留任何游离改动。

{UNTRUSTED_BANNER}

## 结束语契约(最后一行,逐字)
- 成功推分支并贴完报告后,输出一行: `ci-triage: done {fix_branch}`
- 未能完成(复现不出/超预算/无法修复)时,输出一行: `ci-triage: escalated <原因>`
"""


def run_background(cmd: str, cwd: str, log: str) -> int:
    """后台派生子进程的薄封装(模块级,测试 monkeypatch 的唯一子进程出口)。

    Popen + shell=True + start_new_session(脱离 cron 会话,工具退出后
    agent 继续跑)+ stdout/stderr 追加重定向到 log。返回子进程 PID。
    """
    os.makedirs(os.path.dirname(log), exist_ok=True)
    with open(log, "ab") as lf:
        proc = subprocess.Popen(
            cmd, shell=True, cwd=cwd, stdout=lf, stderr=lf,
            stdin=subprocess.DEVNULL, start_new_session=True)
    return proc.pid


def agent_alive(pid: Optional[int]) -> bool:
    """PID 探活: os.kill(pid, 0)。进程不存在 → False;无权限(非本组
    进程,如别的用户的进程)→ 视为活着(True)——宁可多判活也不误杀。"""
    if pid is None:
        return False
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def has_running_agent(state: dict) -> bool:
    """单飞判定: 任一条目持有活 PID 且状态 ∈ {DIAGNOSING, BOT_FIX}。"""
    for e in StateStore.entries(state):
        if e.status in AGENT_STATES and agent_alive(e.agent_pid):
            return True
    return False


def dispatch_agent(entry: Entry, state: dict,
                   dry_run: bool = False) -> Optional[int]:
    """派修复 agent(nohup 后台)→ 返回 PID;单飞冲突返回 None。

    命令形态:
      nohup bash -c 'source <STATE_DIR>/agent.env && timeout <T> claude
      -p <shlex 引用的 prompt> --dangerously-skip-permissions'
      >> <log> 2>&1 & echo $!
    - cwd = agent 专用克隆目录(CITRIAGE_CLONE_DIR,默认 autoci 目录),
      绝不在用户工作区执行;
    - 单飞: has_running_agent(state) 为真时拒绝派发(返回 None);
    - dry_run: 只打印意图,零 spawn,返回 None。
    """
    if has_running_agent(state):
        print(f"[single-flight] 已有 agent 在跑,拒绝为 run {entry.run_id} 派发")
        return None
    fix_branch = entry.fix_branch or f"fix/ci-{entry.run_id}"
    prompt = build_agent_prompt(entry, {"workflowName": entry.workflow})
    log = _agent_log_path(entry.run_id)
    clone_dir = os.environ.get("CITRIAGE_CLONE_DIR", DEFAULT_CLONE_DIR)
    env_file = os.path.join(_resolve_state_dir(), AGENT_ENV_NAME)
    inner = (f"source {shlex.quote(env_file)} && "
             f"timeout {AGENT_TIMEOUT_SEC} claude -p {shlex.quote(prompt)} "
             f"--dangerously-skip-permissions")
    cmd = (f"nohup bash -c {shlex.quote(inner)} >> {shlex.quote(log)} 2>&1 "
           f"& echo $!")
    if dry_run:
        print(f"[dry-run] 将派 agent 修 run {entry.run_id} → {fix_branch},"
              f"日志 {log},工作目录 {clone_dir}")
        return None
    pid = run_background(cmd, cwd=clone_dir, log=log)
    entry.agent_pid = pid
    entry.agent_log = log
    entry.agent_started_at = _now_iso()
    entry.fix_branch = fix_branch
    entry.attempts += 1
    entry.updated_at = _now_iso()
    print(f"[dispatch] run {entry.run_id} → agent PID {pid},分支 "
          f"{fix_branch},日志 {log}")
    return pid


def check_agent_outcome(entry: Entry) -> str:
    """判定 agent 结局: "running"|"success"|"auth_fail"|"dead"|"timeout"。

    判定顺序(先具体后兜底):
      timeout   — started_at + AGENT_TIMEOUT_SEC 已过(超时强杀优先判);
      auth_fail — 日志尾部 4000 字符匹配 API Error|403|Not logged in;
      success   — 日志含结束语契约 `ci-triage: done`;
      dead      — PID 已死且无上述特征;
      running   — 其余(PID 活着、尚无结论特征)。
    """
    started = _parse_created_at(entry.agent_started_at or "")
    if started is not None and \
            datetime.now(timezone.utc) - started > timedelta(
                seconds=AGENT_TIMEOUT_SEC):
        return "timeout"
    tail = ""
    if entry.agent_log:
        try:
            with open(entry.agent_log, "rb") as f:
                f.seek(0, os.SEEK_END)
                size = f.tell()
                f.seek(max(0, size - 4000))
                tail = f.read().decode("utf-8", errors="replace")
        except OSError:
            tail = ""    # 日志缺失/不可读: 视为无特征,交给后续判档
    if AUTH_FAIL_PATTERN.search(tail):
        return "auth_fail"
    if AGENT_DONE_MARK in tail:
        return "success"
    if not agent_alive(entry.agent_pid):
        return "dead"
    return "running"


def _log_tail(path: Optional[str], limit: int = 800) -> str:
    """升级评论用的日志尾部摘录(失败现场留存;读不到给占位说明)。"""
    if not path or not os.path.exists(path):
        return "(日志缺失)"
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()[-limit:].strip() or "(日志为空)"
    except OSError:
        return "(日志读取失败)"


def _escalate(entry: Entry, state: dict, reason: str,
              next_step: str, dry_run: bool) -> None:
    """条目升级人工: 状态 ESCALATED + 一条 @wangxumarshall 评论。"""
    entry.status = "ESCALATED"
    entry.agent_pid = None           # 归还单飞配额(无活 agent 了)
    entry.updated_at = _now_iso()
    body = (f"@wangxumarshall ci-triage 升级：{reason}。"
            f"证据：{_log_tail(entry.agent_log)}。"
            f"建议人工介入：{next_step}。")
    if dry_run:
        print(f"[dry-run] 将评论 issue #{entry.issue_number}: {body[:120]}…")
        return
    if entry.issue_number is not None:
        comment_on_issue(entry.issue_number, body)
    print(f"[escalate] run {entry.run_id} → ESCALATED({reason})")


def reap_or_advance(state: dict, dry_run: bool = False) -> None:
    """收割 agent 结局并推进状态机(就地更新 state dict)。

    - DIAGNOSING 条目按 check_agent_outcome 推进:
        success → gh pr list 查 fix 分支 PR → 命中: pr_number + PR_OPEN
                  (auth_failure_streak 归零——证明凭据活着);未命中: 走
                  attempts 路径(可能 agent 谎报,重派或升级);
        auth_fail → streak+1;达 3 → 写 PAUSED 文件 + 升级评论(全局熔断,
                    连续认证失效说明凭据过期,继续派发只会空转);
        dead/timeout → attempts+1;≤2 重派(回到 DIAGNOSING 由调用方
                  dispatch);>2 升级人工。
    - BOT_FIX 条目: 结局只记录在条目上,状态不变(pr_loop 已接管)。
    - 单飞语义: 处理中的 agent 未退场(PID 活)不影响本函数——只收割
      已结束的结局。
    """
    entries = StateStore.entries(state)
    for entry in entries:
        if entry.status not in AGENT_STATES:
            continue
        if entry.agent_pid is None and entry.status == "DIAGNOSING":
            continue          # 尚未派发,无结局可收割
        outcome = check_agent_outcome(entry)
        if outcome == "running":
            continue
        entry.updated_at = _now_iso()
        if entry.status == "BOT_FIX":
            # pr_loop 接管期: 结局只记录,不推进状态机
            print(f"[reap] run {entry.run_id}(BOT_FIX) agent 结局: {outcome}")
            continue

        if outcome == "success":
            prs = gh_json(["pr", "list", "--repo", repo_slug(),
                           "--head", entry.fix_branch, "--state", "open",
                           "--json", "number,headRefName,url"])
            if isinstance(prs, list) and prs:
                pr = prs[0]
                entry.pr_number = int(pr["number"])
                entry.status = "PR_OPEN"
                entry.agent_pid = None    # 归还单飞配额
                state["auth_failure_streak"] = 0    # gh 凭据活着,归零
                print(f"[reap] run {entry.run_id}: agent 成功 → PR "
                      f"#{entry.pr_number}({entry.fix_branch})")
            else:
                # 日志称 done 但查无 PR——按失败重走 attempts 路径
                print(f"[reap] run {entry.run_id}: 日志称 done 但无 open PR,"
                      f"按失败处理")
                _requeue_or_escalate(entry, state, "agent 报 done 但无 PR",
                                     "核对 agent 日志与远端分支", dry_run)
        elif outcome == "auth_fail":
            state["auth_failure_streak"] = \
                state.get("auth_failure_streak", 0) + 1
            if state["auth_failure_streak"] >= AUTH_FAILURE_STREAK_LIMIT:
                if not dry_run:
                    Path(_resolve_state_dir(), "PAUSED").touch()
                _escalate(entry, state, "agent 认证失效，请更新 agent.env"
                          " 后删除 PAUSED",
                          "更新 agent.env 凭据后删除 PAUSED 文件恢复自动派发",
                          dry_run)
            else:
                # 未达阈值: 清 PID 结束本轮收割(该结局只计一次),
                # 条目保持 DIAGNOSING——但 agent.env 未修好前重派必再
                # auth_fail,继续烧凭据配额,故不立即重派,等下一轮/熔断
                entry.agent_pid = None
                print(f"[reap] run {entry.run_id}: agent 认证失效"
                      f"(streak {state['auth_failure_streak']}"
                      f"/{AUTH_FAILURE_STREAK_LIMIT}),不重派等下一轮")
        else:   # dead / timeout
            entry.attempts += 1
            _requeue_or_escalate(entry, state,
                                 "agent " + ("超时" if outcome == "timeout"
                                             else "进程死亡"),
                                 "人工查看 agent 日志定位卡点", dry_run)
    # 序列化回 state: 必须用本轮持有的同一批 Entry 对象——重新从
    # state["entries"] 解析会拿到未修改的旧 dict,丢掉全部推进
    state["entries"] = [e.to_dict() for e in entries]


def _requeue_or_escalate(entry: Entry, state: dict, reason: str,
                         next_step: str, dry_run: bool) -> None:
    """attempts 路径: ≤MAX_AGENT_ATTEMPTS 保持 DIAGNOSING 待重派;
    超限升级人工。"""
    if entry.attempts <= MAX_AGENT_ATTEMPTS:
        entry.agent_pid = None
        print(f"[requeue] run {entry.run_id}: attempts {entry.attempts}"
              f"/{MAX_AGENT_ATTEMPTS},待下一轮重派({reason})")
    else:
        _escalate(entry, state, reason, next_step, dry_run)


# ============ 子命令(Task 2/3/4 实现,本任务仅占位) ============

def cmd_scan(args: argparse.Namespace) -> int:
    """扫描受监控分支的 CI 失败并更新状态(Task 2: 红扫描 + 分诊落 state)。"""
    store = StateStore()
    state = store.load()
    try:
        runs = scan_red_runs()
    except GhError as exc:
        print(f"扫描失败: {exc}", file=sys.stderr)
        return 1
    print(f"扫描到 {len(runs)} 个红 run(监控 {MONITORED_BRANCH} 分支,"
          f"回看 {LOOKBACK_DAYS} 天)")
    triage_new_red_runs(state, runs, dry_run=bool(getattr(args, "dry_run", False)))
    store.save(state)
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    """打印当前状态总览。"""
    raise NotImplementedError("to be implemented in Task 3")


def cmd_dry_run(args: argparse.Namespace) -> int:
    """只读演练: 扫描 + 分诊全部走 dry_run,零 gh 写、零 state 变更。

    Task 2 交付扫描与分诊的 dry-run 主体(真实 gh 读: run list/issue 搜
    索);Task 4 将扩展为含派发预演的完整演练。state 不落盘——复跑无痕。
    """
    state = StateStore().load()   # 只读: 只为幂等判断,不 save
    try:
        runs = scan_red_runs()
    except GhError as exc:
        print(f"扫描失败: {exc}", file=sys.stderr)
        return 1
    print(f"[dry-run] 扫描到 {len(runs)} 个红 run(监控 {MONITORED_BRANCH} 分支,"
          f"回看 {LOOKBACK_DAYS} 天,零 gh 写)")
    for run in runs:
        print(f"[dry-run] 红 run: [{run['workflowName']}] run "
              f"{run['databaseId']} ({run.get('createdAt', '?')}) "
              f"{run.get('url', '')}")
    triage_new_red_runs(state, runs, dry_run=True)
    return 0


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
