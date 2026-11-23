# ci-multi-os-green: 让 Multi-OS Verify 全绿并把 15 个 tarball 发成 GitHub Release

## Intent

Multi-OS Verify 最近一次失败 run 44456953718 是打在**旧 commit 3633cd8** 上的，红了 15 个
job——5×20.03 的 `HWCAP2_SVE2` 编译错 + 10×22.03/24.03 的 `crash=2`。两个修复已经合入 main
（`f0375ab6` 补 HWCAP2_SVE2 fallback、`63f1b3c2` 修 low_load_cache_pressure 两 FP64 变体），
但**从未被一次验证跑确认**。本 mission：触发/监控全量跑，若仍有红就定位+修复，直到 15 个
verify job + report + benchmark-trend 全绿，再走 workflow_dispatch 的 publish_release 把 15 个
built/ tarball 发成 GitHub Release。容器镜像不重建（已确认：只发 Release）。

## Done when

- 一次完整 full 跑（引 run ID）上 15 个 verify job 全部 pass，且 report、benchmark-trend 绿。
- 若首跑红：每个失败 job 落实到具体测试名/编译错 + 根因，逐处修复，重新触发直至绿（每处引真实崩溃/编译现场）。
- GitHub Release 发布成功：release job 绿，15 个 built-* tarball 挂上 release（引 tag/URL）。
- rule 2 自证：本机构建（`-Dcpp_std=gnu++20`，GCC 10 无 gnu++23）干净零新告警；受影响用例真实输出 pass；`zstd19 -t 2000 -n 1` → `exit: pass`。

## Out of scope

- 不重建 ghcr 容器镜像（已确认，发布只走 GitHub Release 的 15 个 tarball）。
- 不改 x86-64 参考路径（rule 5）；不在本 mission 动容器镜像构建脚本 images/。
- 不 merge 非本 mission 的 PR、不 resolve 他人 review thread。

## Decisions for you

1. **角色切分**：失败范围在首跑结果出来前未知，**不能按文件预分并行**，故用单 builder
   （`ci-fix`，谁失败修谁）+ 单 reviewer。**Recommended: 单 builder 串行**，因为残 bug
   未知、且修一处就重跑一次的串行闭环天然串行；并行拆分会空转。
2. **修复是否允许落在 CI 工作流自身**：本 mission 目标就是让这套 CI 绿+发布，所以
   `multi-os-verify.yml` / `scripts/gha/` 自身缺陷（零诊断死亡、误判红等）也是修复对象
   （上一 mission 明确不碰工作流，本 mission 放开）。**Recommended: 允许。**

## How the work is split

一段串行闭环：`ci-fix` 监控跑 → 红则定位+修复 → rule 2 自证 → 提交；审查后合回 mission 分支，
重新触发，直到绿；最后走 Release。因为残 bug 的位置未知，`ci-fix` 的 `touches` 覆盖「测试源码
+ 框架 + workflow + gha 脚本」这一整片修复面，且无并行角色与之冲突；`review` 改零文件、只审。

```yaml
troupe: 1
goal: "让 Multi-OS Verify 15 个 verify job + report + benchmark-trend 全绿，并把 15 个 built/ tarball 发成 GitHub Release"
why: "f0375ab6（HWCAP2_SVE2 编译错）+ 63f1b3c2（crash=2）合入 main 后从未被验证跑确认，需复绿并发布"
workflow: troupe-mission
base: mission/ci-multi-os-green
profiles:
  claude-build:
    agent: claude
    model: sonnet
    effort: high
    flags: [--permission-mode, bypassPermissions]
    text: workflow:builder
roles:
  ci-fix:
    use: claude-build
    goal: "监控 run 44683247314（及后续重跑），红则定位每个失败 job 的测试名/根因并修复源码或工作流，rule 2 自证后提交"
    touches: [tests/, framework/, .github/workflows/multi-os-verify.yml, scripts/gha/]
  review:
    agent: claude
    model: opus
    effort: high
    flags: [--permission-mode, bypassPermissions]
    goal: "Review each branch as it lands"
    touches: []
    reviews: [ci-fix]
    text: workflow:reviewer
```

## Role: ci-fix

目标：把 Multi-OS Verify 复绿。它修的是「残 bug」，不是预先已知的两处（那两处已在 main）。

1. **监控** run 44683247314（workflow_dispatch、full、main 上已触发）。等它结束就用
   `gh run view <id> --json jobs` 看每个 job 结论，红 job 用
   `gh api repos/wangxumarshall/sdcshield/actions/jobs/<jobid>/logs` 拉真实错误现场、
   需要时 `gh run download <id>` 拉 verify-* artifact 取证。
2. **全绿**：直接报结果——绿 run ID + 15 job 结论逐行列出，`status: done`，不改一个文件。
3. **有红**：对每个红 job，分清是「编译错 / 测试失败 / crash / 工作流脚本零诊断死亡」，
   落到具体测试名 + 根因（引真实现场），修到 `touches` 覆盖面内，`git commit -s`（DCO 签名，
   不带 Co-Authored-By）。若要改的文件不在 `touches` 内 → 写 `status: blocked` 说明文件+原因，
   不越界；等计划扩 scope。
4. **rule 2 自证（100% 真实）**：`./scripts/build.sh`（本机需 `-Dcpp_std=gnu++20`）干净零新
   告警；受影响用例真跑并贴输出；`./builddir/sdcshield -e zstd19 -t 2000 -n 1` → `exit: pass`；
   x86-64 靠检视。**诚实边界**：本机 no-SVE（Kunpeng 920，GCC 10），若红是 SVE/SVE2 相关
   （GitHub arm runner 有 SVE VL=128），本机复现不了 → 走 gem5-fi（`/home/sdc/wangxu/gem5-fi`）
   或如实声明边界，**绝不**用「SKIP 不变」当修复证据。

## Role: review

按 reviewer 规则审 `ci-fix` 每个分支，额外查：

- 修复是否引了**真实**现场（崩溃栈 / 编译错误原文），不是「应该好了」。
- rule 2 自证输出是否真实：构建零新告警、受影响用例输出、`zstd19 exit: pass`。
- 改动是否真的局限在 `touches`（尤其 x86-64 路径与 images/ 不得动）。
- 若 ci-fix 走了诚实 skip / gem5，skip 理由是否诚实、不越界。
- 汇总给 PASS / FAIL，PASS 才合。
