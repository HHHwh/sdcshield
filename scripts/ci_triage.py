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
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
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
    """受监控 workflow 名单——调用时读 env(测试可 monkeypatch.setenv)。"""
    raw = os.environ.get("CITRIAGE_WORKFLOWS",
                         "Multi-OS Verify,Sanitizer,CodeQL,Security")
    return [w.strip() for w in raw.split(",") if w.strip()]


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

def find_issue_by_run(run_id: int) -> Optional[int]:
    """按幂等标记搜既有 open issue → issue 号;无匹配返回 None。

    `gh search issues --state open "ci-triage:run" --limit 50`(只搜 open:
    CLOSED 的标记 issue 意味着人工已接管/关闭,不重建——调用方自行决定跳过
    语义)。命中判据是 body 内含完整标记 `<!-- ci-triage:run:<id> -->`,
    而非标题匹配——标题会被人工改写,body 标记是稳定锚。
    """
    slug = repo_slug()
    res = gh_json([
        "search", "issues", "--repo", slug,
        "--state", "open",
        "--json", "number,title,body",
        "--limit", "50",
        "ci-triage:run",
    ])
    if not isinstance(res, list):
        raise GhError("gh search issues 返回非 list")
    marker = RUN_MARKER_FMT.format(run_id=run_id)
    for it in res:
        if marker in (it.get("body") or ""):
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
    if len(lines) > LOG_EXCERPT_MAX_LINES:
        lines = lines[:LOG_EXCERPT_MAX_LINES]
        lines.append(f"…(已截断,原 {len(lines)}+ 行)")
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
      - 否则: 找既有 issue(标记匹配,含 state 外人工已开的)→ 复用;无则建
        → DETECTED → ISSUED。

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
        # 幂等创建: 先搜标记(含 state.json 之外的既有 issue——双源对账)
        issue_no = find_issue_by_run(run_id)
        if issue_no is None:
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
