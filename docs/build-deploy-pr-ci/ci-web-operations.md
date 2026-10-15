# CI 网页操作清单(GitHub Settings 侧)

代码侧无法完成的 CI 配置,集中在本文档。每项标注一次性/持续。

## nightly 失败通知(GitHub 原生,已选定方案)

> 背景:multi-os-verify 每日 cron 无人值守,默认挂了只在 Actions 页变红。

1. 个人 **Settings → Notifications → Actions**:勾选 **Workflow failures**
   (scheduled workflow 失败只通知最近编辑该 workflow 文件的用户——保持
   workflow 由本人维护即可收到)
2. 仓库页 **Watch → Custom**:勾选 **Actions**(Issues/PRs 按需)

一次性操作。生效后 nightly 失败推送邮件。

## 分支保护 + required checks(把「一补丁一 PR」变硬门)

1. **Settings → Branches → Add branch ruleset** → target: `main`
2. **Require a pull request before merging**(DCO/历史检查已由 `git-sanity` 承担;人工提交须 Signed-off-by,Copilot Autofix 机器提交豁免——双标记:GitHub 提交者+bot trailer,命中打 notice 可审计)
3. **Require status checks to pass**:勾选
   - `lint`
   - `git-sanity`
   - `Build binary for CPU (Linux) (GCC-arm64, -arm, unittests, selftests)`
   - `Security / zizmor (workflow security)`
   - (名称以 Actions 实际显示为准;`multi-version` 因 docs-only PR 会 skip,
     建议不设为 required——skipped 状态视具体平台策略而定,观测期先不挂)
4. **Do not allow bypassing**:单人仓可不勾,保留紧急直推能力

一次性操作。注意:main 当前的既有失败(见 progress-log)修绿后再挂
required checks,否则所有 PR 被卡。

## CodeQL / Security 页确认

1. **Settings → Code security**:
   - Code scanning:应显示 CodeQL(advanced setup = `codeql.yaml`)
   - Dependabot alerts / secret scanning / zizmor(SARIF)按页面提示启用(免费)
2. Dependabot:首个 PR 约 1 周内到达(`.github/dependabot.yml`,周一档)

## gh-pages 基准趋势

nightly 跑完后 `benchmark-trend` job 自动维护 gh-pages 分支;
趋势页 = `https://wangxumarshall.github.io/sdcshield/dev/bench/`
(benchmark-action 默认布局)。首次 merge 后手动 workflow_dispatch 一次
(smoke 也行,benchmark step 无条件执行)验证分支创建。

## 已知事项

- CodeQL 的 arm64 runner 支持若在 CI 报不支持,二选一:放弃 CodeQL /
  例外允许 x86 runner 仅做分析注入(产物不发布)——勿静默改回。
- gitleaks 若报 fork 历史(OpenDCDiag)遗留泄漏:立即处理泄漏本身
  (rotate 凭据),非仅修 workflow。
