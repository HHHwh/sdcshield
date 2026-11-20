# pr-issue-triage: 全量处置 open PR + 为 open issue 出方案文档

## Intent

把当前 9 个 open PR（7 个 WIP 特性 PR + 2 个 docs-only PR）全量按序过一遍：
逐个 triage 已有 bot 检视 findings，修掉真实问题并 push 回各自分支，使它们到
「检视通过、CI 复绿、待合并」；再为 9 个 open issue 各产出一份解决方案文档
（根因 + 建议修复路径 + 验证边界），不写修复代码。合并遵守仓库红线：**用户非
docs PR 永不自动 merge**，docs-only PR 走快道但同样经你批准。

## Done when

- 9 个 PR 各有一份处置结论（merge-ready / 已修复 / 阻塞原因），真实 finding 已修复并 push 回对应分支、CI 复绿（或如实记录未复绿原因）
- 2 个 docs-only PR（628、629）可合并（快道）；7 个 WIP 特性 PR 处于「检视通过、待你人工合并」状态，未触发任何自动 merge
- 9 个 issue 各有一份解决方案文档，含根因 + 建议修复路径 + 验证边界，落于 `docs/issues/` 下
- mission MR（PR 处置报告 + 方案文档）已合并到 main

## Out of scope

- 不自动 merge 任何非 docs PR（红线）；不 resolve 现有 review threads（禁止）
- 不实现 issue 的修复代码（只出方案文档）
- 不改动 x86-64 参考架构（rule 5）；不把 PR 的代码改动合入 mission 分支（只 push 回 PR 自己的分支）

## Decisions for you

1. **角色模型分配**：5 个 builder 用 `sonnet` + `effort: high`，1 个 reviewer 用
   `opus` + `effort: high`，全部 `--permission-mode acceptEdits`。
   **Recommended: 接受**，因为量大体裁是 triage 与文档，sonnet 足够快，reviewer
   换 opus 做终检。approve 即采纳；改模型是 change。
2. **docs-only PR 的合并动作**：628/629 无 `WIP:` 前缀、CI 绿，属快道。
   **Recommended: 本 mission 内经你在 mr-ok / merged gate 批准后由我 `gh pr merge
   --squash`**；备选是留给 nightly pipeline 当夜合。7 个 WIP 特性 PR 只到
   merge-ready，永不自动 merge。

## How the work is split

PR 按文件耦合切：4 个 NEON PR 同族归 `pr-neon`；3 个大特性 PR（march/migrate/
nodepool 三个独立子系统）归 `pr-feature` 按序过；2 个 docs PR 归 `pr-docs`。issue
按主题切两半：4 个 `[audit]` 口径 issue 归 `issues-audit`，5 个 ci-triage /
verification-gap / mesh 缺口 issue 归 `issues-gap`。所有 role 互不共享文件
（各自写自己的报告/文档目录），故全部并行，无 `depends_on`。reviewer 逐个审。
PR role 的「修复」push 回 PR 自己的分支（不落 mission 分支），mission 分支只
承载报告 + 方案文档。

```yaml
troupe: 1
goal: "全量 triage 并修复 9 个 open PR 至 merge-ready、为 9 个 open issue 产出解决方案文档，汇总为一个 MR"
why: "9 个 open PR 均无 approving review 且带未处置 bot findings，9 个 issue 无方案沉淀"
workflow: troupe-mission
base: mission/pr-issue-triage
profiles:
  claude-build:
    agent: claude
    model: sonnet
    effort: high
    flags: [--dangerously-skip-permissions]
    text: workflow:builder
roles:
  pr-neon:
    use: claude-build
    goal: "triage 并修复 4 个 NEON PR（660/643/627/625）至 merge-ready，写报告"
    touches: [docs/missions/pr-issue-triage/reports/pr-neon.md]
  pr-feature:
    use: claude-build
    goal: "按序 triage 并修复 3 个大特性 PR（518/814/695）至 merge-ready，写报告"
    touches: [docs/missions/pr-issue-triage/reports/pr-feature.md]
  pr-docs:
    use: claude-build
    goal: "triage 2 个 docs-only PR（629/628），修真实 finding，写报告"
    touches: [docs/missions/pr-issue-triage/reports/pr-docs.md]
  issues-audit:
    use: claude-build
    goal: "为 4 个 audit issue（713/686/655/645）各写一份解决方案文档"
    touches: [docs/issues/audit/]
  issues-gap:
    use: claude-build
    goal: "为 5 个缺口 issue（533/376/375/357/332）各写一份解决方案文档"
    touches: [docs/issues/gap/]
  review:
    agent: claude
    model: opus
    effort: high
    flags: [--dangerously-skip-permissions]
    goal: "Review each branch as it lands"
    touches: []
    reviews: [pr-neon, pr-feature, pr-docs, issues-audit, issues-gap]
    text: workflow:reviewer
```

## Role: pr-neon

处理 4 个 NEON PR，按 660 → 643 → 627 → 625 顺序逐个过。对每个 PR：

1. 读 diff 与既有 bot 检视 findings：`gh pr view <n> --comments`（本地 claude 检视
   `local-claude-review`、codex、pr-agent、CodeRabbit），区分真实缺陷 / 噪音 /
   already-fixed。
2. 对真实 finding，在 PR 自己的 head 分支上修复：`git fetch origin` 后检出
   `<head>` 分支，`git commit -s`（尾行 `Signed-off-by: wangxu
   <wangxumarshall@qq.com>`，无 Co-Authored-By），`git push origin HEAD:<head>`。
3. 自证（rule 2，100% 真实）：`./scripts/build.sh` 干净零新告警；跑受影响用例并
   引真实输出；回归 `-e zstd19 -t 2000 -n 1` → `exit: pass`；x86 非回归靠检视。
   若本机芯片跑不了（SVE/SME/VL 档位缺 HWCAP），按 gem5-fi 规则在
   `/home/sdc/wangxu/gem5-fi` 验证，否则如实声明验证边界，不得把「SKIP 不变」
   当功能验证。
4. 不 merge、不 resolve review thread、不改 `WIP:` 标题。把你的处置结论写进
   `docs/missions/pr-issue-triage/reports/pr-neon.md`（每 PR 一段：verdict +
   fixed findings + CI 状态 + 验证输出摘录 + 阻塞原因若有），并在 `.troupe/result.md`
   写 `status: done`。

## Role: pr-feature

处理 3 个大特性 PR，按 518 → 814 → 695 顺序。步骤与 `pr-neon` 相同（triage →
修复 push 回 head 分支 → rule 2 自证 → 报告）。三个 PR 分属不同子系统（march
tiers / opendcdiag 迁移 / nodepool），互不共享文件，按序逐个完整做完再下一个。
报告写进 `docs/missions/pr-issue-triage/reports/pr-feature.md`。不 merge、不
resolve thread、不改 `WIP:` 标题。

## Role: pr-docs

处理 2 个 docs-only PR（629 readme-deep-verify、628 agent-full-automation-cn-render-fix）。
它们无 `WIP:` 前缀、属快道。triage 既有 findings，修真实问题 push 回 head 分支，
rule 2 自证（docs 改动验证 = 构建通过 + 渲染/表述核对 + 引用链接/行号准确）。
报告写进 `docs/missions/pr-issue-triage/reports/pr-docs.md`，标明是否可快道合并。
你自己不执行 merge（由 orchestrator 在 gate 批准后 `gh pr merge --squash`）。

## Role: issues-audit

为 4 个 audit issue 各写一份解决方案文档，落 `docs/issues/audit/issue-<n>.md`：
713（T11 显式 knob 半迁移态）、686（验证矩阵 128 档缺失）、655（GEMM sizing 超
预算 8.8%）、645（证据矩阵口径偷换）。每份：读 `gh issue view <n>` 全文 + 定位涉
及代码/文档，写清「根因（引文件:行号）→ 建议修复路径（具体到改哪里怎么改）→ 验
证边界（怎么证明修好、本机/gem5 边界）」。只出文档，不写修复代码。引用必须精确、
可复查。

## Role: issues-gap

为 5 个缺口 issue 各写一份解决方案文档，落 `docs/issues/gap/issue-<n>.md`：533
（ci-triage main 遗留红两档）、376（sve512/power_virus 终态-only 漏检）、375
（聚合/容差校验族 KAT 锚缺失）、357（mesh 只写不校验空转）、332（mesh 读阶段自拷
贝零检出）。格式同 `issues-audit`：根因（文件:行号）→ 修复路径 → 验证边界。只出
文档。

## Role: review

逐个审 5 个 builder 分支，按 landing 顺序。builder 未把 PR 代码合入 mission 分支，
故你要做的额外检查：对 PR 报告分支，用 `gh pr view <n>` / `gh pr checks <n>` 复核
报告里写的 CI 状态、push 的修复 commit 是否真在 PR head 分支上、rule 2 验证输出是
否真实引用了跑出来的结果（不是「should work」）。对 issue 文档分支，核对根因引用
的文件:行号是否真实存在、修复路径是否可执行。把结论写 `.troupe/reviews/<role>.md`
并在 result 里给 PASS 或 FAIL 汇总。
