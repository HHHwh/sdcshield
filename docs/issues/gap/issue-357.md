# Issue #357 解决方案文档：mesh 家族大缓冲退化为只写不校验的空转 pass

- Issue：[#357](https://github.com/wangxumarshall/sdcshield/issues/357)（2026-11-17 立案，源自 PR #308 四轮检视意见 #16 后的处置轮实测）
- 本文核查日期：2026-11-18（核查基线 = main `727cc8d3`；行号、run/commit 为当日实查）

## 一、结论先行

**病理机制全链路在框架源码中核实无误；族内已出现三个层级的部分修复（4 文件全修、若干
文件半修），但 46 文件中 42 个仍处「45% 物存 sizing + 等待耐心不足 + 无诚实 SKIP」的
原始退化形态——族级根治（issue 的三条候选方向）未做。** issue 的实测退化边界
（缓冲 ≳1 MiB 即空转）与其机制归因（40 次 auto-fracture 迭代预算，与 -t 无关）均被
当前源码结构证实。

## 二、机制核实（框架侧，文件:行号）

等待方耐心被钉死的完整因果链（全部当前 main 实查）：

1. `framework/sandstone_run.cpp:634-645` —— `test_time_condition()` 每次调用
   `inner_loop_count++`（每线程独立计数，`:640`），并在
   `max_loop_count_exceeded()`（`:614-627`，判据在 `:621`）中与全局
   `shmem->current_max_loop_count` 比较，超限即返回 0。
2. `framework/sandstone_run.cpp:2057-2060` —— 未设 `fracture_loop_count` 的测试
   （mesh 全族如此）走 auto-fracture：`current_max_loop_count = 40`。
3. `framework/sandstone_run.cpp:2037` + `:2092-2094` —— 预算翻倍仅发生在
   `now < first_iteration_target`（= setup 时刻 + **10 ms**）窗口内；mesh 一轮
   填充远超 10 ms，翻倍永不发生 → 预算恒 40。
4. mesh 等待自旋每（N 次）自旋调一次 `test_time_condition` → 等待耐心 =
   40 × N 次自旋：
   - **每自旋都调**（最劣，如 `tests/cpu/mesh/mesh_upi_sse_sym.cpp:226-229`：条件
     表达式内直调）→ 耐心 ~40 自旋（µs 级）；
   - **1024 一档**（如 `tests/cpu/sve/mesh/mesh_upi_sve_asymm_int.cpp:216-219`，
     相位 1 等待写者填充）→ 耐心 ~40×1024 自旋（亚毫秒）；
   - **131072 一档**（已修文件，如 `mesh_upi_avx512_sym.cpp:285-294`）→ 耐心
     ~40×131072 自旋（≈0.1–0.3 s）。
5. 耐心耗尽 → `goto out_of_budget` / `break` 跳过整轮校验 → 多数文件
   `return EXIT_SUCCESS`（如 `mesh_upi_sve_asymm_int.cpp:322-323`）——零校验记 PASS。
   写者填充循环无时间检查是设计使然（逐块调用会以同样机制截断填充并触发 fracture
   重启、覆盖归零——issue 已论证这是行为回归而非修复，不作为修法）。

绑定预算的不是 `-t` 墙钟而是迭代预算语义——issue 用 1 MiB（填充时长远小于 -t 5 s 却
仍 0 条校验线）证伪了墙钟绑定，机制链与该实证自洽。

## 三、族级现状盘点（46 文件全查，2026-11-18）

46 = `tests/cpu/mesh/` 28 + `tests/cpu/sve/mesh/` 18（PR #308 的 17 文件已 merge 在
后者）。三维度逐文件 grep 实查（sizing：`sdc_target_workset_bytes`/`sdc_budget_bytes`
四源 vs `sysinfo` 45% 物存；节流：131072 / 1024 残留 / 每自旋；VA：`validated_any`
诚实 SKIP）：

| 状态 | 文件（数） | 说明 |
|---|---|---|
| **全修**（4src sizing + 131072 + VA） | avx512_sym、avx_sym、avx512_symm_f64（3） | #325/#331/#358 修复线：默认缓冲 = 四源安全预算 45% ÷（2×线程×slice）clamp [64 MiB, 1 GiB]（`mesh_upi_avx512_sym.cpp:96-146`，`:116` `sdc_budget_bytes(900)`）；等待 131072 一档（`:285-294`）；零校验轮诚实 SKIP（`:222`、`:393-400`） |
| **VA-only**（45% sizing + 每自旋 + VA） | sse_sym（1） | 已有诚实 SKIP（`:158`）与整数化 sum 校验（#332 线，`:146`、`:217-222`、`:241-273`），但 sizing 仍 45% 物存（`:59-73`，`:70` `total/10*9/2`）、等待仍每自旋查预算（`:226-229`）——大缓冲下耐心 µs 级即耗尽，VA 把空转 pass 变诚实 SKIP，**校验仍不会发生** |
| **半修**（4src sizing，节流/VA 各异） | SVE mesh 其余 15（含 sve_sym/sve_symm_int/sve_wide_sym 的 131072、sve_asymm_int 相位 1 的 1024 残留 `:216-219`、asymm_write/read_only 等的每自旋） | PR #308 后 sizing 全面上四源；等待侧仅部分文件升级 131072；全部无 VA |
| **原始形态**（45% sizing，节流各异，无 VA） | x86 mesh 其余 22（avx2_*、avx512_asymm*、sse_* 等） | 1024 残留：avx2_asymm_int、avx512_asymm_int、sse_asymm_int 等 5 文件；其余每自旋或无等待环 |

注：read_only/write_only/read_L3 类单角色文件无跨线程等待环，「等待耐心」病理不适用，
但「整轮时长脱离 -t/预算约束 → 战役外部 timeout 误报 Timed Out」的姊妹问题
（#325 已修三文件、sse_sym 头注 `:40-56` 留档）对大缓冲仍成立。

## 四、根因

1. **框架侧（机制）**：`framework/sandstone_run.cpp:2037`（10 ms 翻倍窗）+ `:2057-2060`
   （预算 40）+ `:621`（每调用计数判停）——自旋等待复用迭代预算语义，把「等待时长」
   错误地绑到一个为「工作量」设计的预算上。
2. **测试侧（放大器）**：mesh 族等待环把 `test_time_condition` 当超时探测用
   （`mesh_upi_sve_asymm_int.cpp:210-219` 注释自认「inner_loop_count 语义是轮次而非
   自旋」）；9b7ee76c knob-ize 引入的 45% 物存 sizing（仍存于 25 文件）使默认缓冲
   远超耐心覆盖范围。
3. **记账侧（违例）**：零校验轮 `return EXIT_SUCCESS`（rule 6 精神违例，#358 已立案
   已关但只修了 4 个文件）。

## 五、建议修复路径

按 issue 三候选方向 + 已落地先例，给族级根治的执行序：

1. **等待侧改纯墙钟 deadline（方向 1 的根治半）**：等待环不再调
   `test_time_condition`，改为 `MonotonicTimePoint` deadline 自旋（deadline = min(本测试
   `current_test_endtime` 剩余, 填充时长上界)）——等待不是工作量，不该烧迭代预算。
   需要一个框架层小 API（如 `test_wait_deadline_remaining()`，读
   `sApp->shmem->current_test_endtime` 即可，避免 46 文件各自摸内部状态）。过渡兼容：
   保留 131072 一档节流的文件可最后迁移（它们的耐心 0.1–0.3 s 已覆盖 ≤1 GiB 填充，
   是当前最大事实上的「能校验」面）。
2. **sizing 时间感知（方向 2）**：把 avx512_sym 已落的「读流量总量恒定」口径
   （`mesh_upi_avx512_sym.cpp:100-146`）作为族标准推广到 25 个 45% 物存文件——它们与
   sym 族同读模式（每线程全量读），四源预算 ÷ 线程 × slice 的 clamp 语义可直接复制
   （孪生同步注释 `:56-59` 已预告「#357 族级四源迁移时应合并进
   `sdc_target_workset_bytes` 体系（届时连 SVE mesh 18 文件一并收口）」——执行它）。
   更进一步按 -t 反推（`sdc_calibrated_dim` 幂律拟合先例）列为可选加值项，不阻塞。
3. **增量校验（方向 3，长期）**：写者分段完成 → 读者分段校验，消除「整轮完成才校验」
   耦合。改动面大（协议改写），建议独立 PR，且只在 1/2 落地后仍有语义需求的文件做。
4. **VA 兜底（rule 6 地板，先行做）**：42 个无 VA 文件统一补 `validated_any` 诚实
   SKIP（`mesh_upi_avx512_sym.cpp:222,393-400` 模板，#358 先例）——这是把「空转 pass」
   至少变成「可观测的零校验」的最低成本动作，不依赖 1–3 的设计决策。
5. **节奏**：每文件一 commit（rule 1）；22.03/20.03 容器编译注意（老 gcc 对
   `optimize("O0")` 属性等已有先例处理，照抄 `mesh_upi_sve_asymm_int.cpp:124-132` 的
   ICE 规避注释）。

## 六、验证边界

1. **病理复现（基线）**：issue 的实测表（16 KiB 有校验线 / 1 MiB 起 0 条 / 默认 45%
   ≈27.5 GiB 填充 >150 s）为本 issue 立案证据；其测量机为 128 核/61 GiB/SVE-256。
   **本文核查机为 96 核/313 GiB/no-SVE**——SVE mesh 18 文件本机诚实 SKIP，无法本机
   复现 SVE 侧；x86 命名但实为 NEON 实现的文件（avx512_sym/sse_sym 等）本机可跑，
   差异化对照（1 MiB 缓冲下 sse_sym 校验线消失 vs avx512_sym 校验线在）见下。
   [实验结果见附录 A]
2. **修复后验证**：`SDC_MESH_BUFFER_BYTES=1073741824 ./builddir/sdcshield -e <mesh> -n 2
   -t 5000 -vv` 下读者校验线出现且多轮；默认 sizing 的 `-t 5` 内完成至少一轮完整
   「填充→校验」（无 Timed Out）；零校验轮结局为 SKIP 而非 PASS。
3. **回归红线**：`-e zstd19 -t 2000 -n 1` → `exit: pass`；`./scripts/build.sh`
   零新告警；quick 模式（CI 冒烟 16 MiB）行为不变。
4. **x86 非回归**：mesh 文件全部 `#ifdef __aarch64__` NEON/SVE 路径 + 占位 SKIP
   （`mesh_upi_avx512_sym.cpp:415-424`），x86-64 侧检视确认不动。
5. **框架 API 改动**（路径 1 若做）：`test_time_condition` 语义不动（46 文件外全仓
   依赖），新增 API 走独立 commit + unittests/selftests 全绿。
6. 诚实边界：本文未实现任何修复代码；族级盘点基于结构 grep + 抽样精读（sse_sym、
   avx512_sym、sve_asymm_int 全文精读，其余按模式分类），个别文件的等待环形态若与
   分类不符，以文件实读为准。

## 附录 A：本机差异化对照实验（2026-11-18，96 核 / 313 GiB / no-SVE 主机）

本 worktree 全新构建（gcc 10.3.1 需 `-Dcpp_std=gnu++20` 覆盖默认 gnu++23；构建零告警；
回归 `-e zstd19 -t 2000 -n 1` → `exit: pass`）。实测结果：

1. **病理主对象（SVE mesh 18 文件，env 尺寸缓冲）本机不可执行**：
   `-e mesh_upi_sve_asymm_int -n 2 -t 2000 -o x.yaml` →
   `result: skip, skip-category: CpuNotSupported, skip-reason: 'test compiled with sve'`
   （诚实 SKIP）。issue 的实测表（16 KiB 有校验线 / ≥1 MiB 零校验线）来自 SVE-256 主机，
   本机无法复现，gem5-fi 或 SVE 主机为验证边界。
2. **x86 命名的 asymm NEON 孪生不具病理面**：`mesh_upi_sse_asymm_int` 等为编译期静态
   小缓冲（`TOTAL_ELEMENTS 1024` = 4 KiB，`tests/cpu/mesh/mesh_upi_sse_asymm_int.cpp:13-17`），
   无 env knob——缓冲永不超耐心，读者恒校验。即 #357 的「大缓冲」退化面集中在
   PR #308 knob-ize 后的 SVE mesh（含 45% 物存默认 sizing）。
3. **sym 骨架在 -n 2 下自愈（补充发现）**：`mesh_upi_sse_sym`（等待仍每自旋查预算、
   45% 物存 sizing、有 VA）在显式 `SDC_MESH_BUFFER_BYTES` = 16 KiB / 1 MiB / 64 MiB /
   256 MiB / 1 GiB 五档下全部 `exit: pass`（1 GiB 档单 fracture 迭代 5.37 s 完整跑满
   -t，含完整校验轮）。机理：sym 是全线程动态认领填充，等待方只需覆盖**尾部负载偏斜**
   （µs–ms 级）而非整段填充——40 次预算的耐心对它够用。**issue 的「≳1 MiB 即空转」
   边界因此精确化为：单写者 asymm 骨架（读者等整段填充）+ 高线程数下的屏障孵化偏斜
   （"1024 一档在 40 次预算下覆盖不足"，`mesh_upi_sve_asymm_int.cpp:296-299` 注释）；
   sym 骨架低线程数不在病理面内。** 这不弱化 issue——SVE mesh 的 asymm/wide_asymm
   14 文件正是 env 尺寸 + 单写者/分布式写者骨架，病理面与 issue 所述一致。
4. **VA 观察点有效性**：`mesh_upi_sse_sym` 的 `validated_any` 诚实 SKIP 是本机可用的
   「零校验轮」探针（本次实验未触发 SKIP，与第 3 条自愈一致）；SVE mesh 42 文件中
   无 VA 者仍以 `EXIT_SUCCESS` 记账零校验轮（`mesh_upi_sve_asymm_int.cpp:213-214`）。
