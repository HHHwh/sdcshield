# Plan: pr_loop 瞬态容错与离线自检（PR #245）

> 回溯补记：本特性由 PR#242 偶发 HTTP 500 事故触发、按四 bot 检视意见逐轮演进
> （greptile PR#245 建议补齐计划文档）。范围、决策与验证步骤按实际发生过程记录，
> 供后续维护者有据可查。

## 背景与问题

- **事故**：2026-10-14，`pulls/242/reviews` 偶发 HTTP 500，`pr_loop.py status` 总览
  单点失败直接 exit 2，其余 open PR 的总览信息全部丢失。
- **根因**：gh 调用无重试、status 总览无逐 PR 容错。

## 范围

1. `scripts/pr_loop.py`：gh 调用瞬态容错（重试）+ status 总览逐 PR 降级。
2. `scripts/pr_loop.py`：`selftest` 子命令——离线自检（fake gh 故障注入）。
3. 文档同步：`docs/misc/pr-loop.md` + 双端 twin
   （`.claude/commands/pr-loop.md`、`.trae/rules/pr-loop.md`）。

不涉及：wait / collect 的行为变化；bot-coverage.yml；x86-64 测试代码。

## 设计决策

| 决策 | 依据 |
|---|---|
| 重试策略：总尝试 ≤ 3 次（含首次），线性退避 1s / 2s | 瞬态 5xx / 网络抖动通常秒级自愈；更长退避让 agent 会话卡顿 |
| HTTP 4xx 与非 JSON 响应**不重试**（首次即抛） | 4xx 是确定性错误（权限 / 不存在 / 参数），重试只白等退避（greptile PR#245 R1） |
| status 总览逐 PR 降级，全失败才 exit 2 | 单 PR 拉取失败不应毁掉整个总览；诚实标注「数据拉取失败，已跳过」 |
| selftest 用 fake gh（纯 shell，PATH 首位注入），不用 pytest | 仓库 CI 不跑 tools/ pytest；pr_loop 是 stdlib-only 自闭环脚本，零新依赖 |
| fake gh **全固定数据端点，不转调真 gh** | 「离线自检」承诺必须兑现——转调真 gh 则未认证 / 断网即失败（greptile/coderabbit PR#245 R2） |
| fake 未知路径一律 404 | 契约漂移哨兵：selftest 场景调用未配置 API 路径时立即失败（覆盖边界：四个场景只跑 status） |
| fake 临时路径一律 `shlex.quote` | TMPDIR 含空格时未引用路径分词（greptile PR#245 R2） |
| fake 路径 case 先特异后宽泛 | `commits/` 前缀放前会吞并 `check-runs` 路径，JSON 形状错配（实证踩坑） |

## 实施步骤（实际发生顺序）

1. `gh_api` / `resolve_slug` 统一 `_gh_once` + 重试循环（70167ada）。
2. status 总览逐 PR try/except 降级（70167ada）。
3. 三处文档同步「瞬态容错」语义（70167ada）。
4. 4xx 快速失败 + 重试文案精确化 + selftest 子命令 v1（fe15fb80）。
5. fake gh 全离线固定数据重构 + shlex.quote + 双 PR 降级断言（6c69fa3d）。
6. selftest 设计与 fake-gh 约定写入 docs/misc/pr-loop.md §3（3c4d139f）。
7. 哨兵范围措辞收敛 + fake commit SHA 精确匹配（089e04d7）。
8. `fail_once_once` 场景双 PR 全展示断言 + 文档量词（本提交）。

## 验证步骤

- `python3 scripts/pr_loop.py selftest` 四场景全 PASS——必须在
  `env -u GH_TOKEN -u GITHUB_PERSONAL_ACCESS_TOKEN` 下运行（离线兑现）：
  ① `fail_once_once`：重试恢复，status exit 0，双 PR 全展示、无跳过标注；
  ② `api_404_always`：快速失败，仅 1 次 api 调用；
  ③ `api_5xx_always`：重试耗尽，3 次尝试；
  ④ `pr1_ok_pr2_fail`：#1 完整展示、#2 标注「数据拉取失败，已跳过」、exit 0。
- TMPDIR 带空格（`"/tmp/pr loop space"`）同样 4/4 PASS。
- 真实仓库 `status` 总览回归 exit 0（3 open PR 完整）。
- `python3 -m py_compile scripts/pr_loop.py`；`ninja -C builddir` 零新警告。

## 经验总结

- fake gh 里 `command -v gh` 会解析回 fake 自身——真 gh 路径不可在 fake 内探测，
  干脆不转调（全固定数据）反而更简单、更诚实。
- shell `case` 某分支 `exit 1` 会短路其后追加的行——fake 的计数逻辑须放脚本头部。
- 「离线」「全覆盖」这类承诺写进文档前先用最恶劣环境（清凭据 / 空格路径）实测。
