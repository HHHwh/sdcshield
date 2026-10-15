**变更说明**

<!-- 这个 PR 做什么、为什么;一个 PR 只做一件事(一 patch 一单元) -->

**自验证清单(CLAUDE.md 规则 2,100% 实证)**

- [ ] `ninja -C builddir` 干净构建,零新增告警
- [ ] 实际运行受影响行为,引用真实输出
- [ ] 回归:`./builddir/sdcshield -e zstd19 -t 2000 -n 1` → `exit: pass`
- [ ] x86-64 非回归(检查确认;共享代码改动必查)
- [ ] ARM64 未实现项以 `EXIT_SKIP` + placeholder 理由跳过,无假通过
- [ ] 新增 tests/ 用例使用框架 RNG(`random32`/`random64`/`memset_random`/`frandom*`),无自建 RNG(mt19937/random_device/rand);固定 pattern 已加 `R3-justified:` 论证

**提交卫生**

- [ ] 每个 feature/bug/移植点独立 commit,无捆绑
- [ ] DCO:`git commit -s`(Signed-off-by 为最后一行)
- [ ] 大颗粒度修改已同步 README.md 与 docs/ 对应文档
