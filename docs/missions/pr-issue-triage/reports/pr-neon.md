# pr-issue-triage: pr-neon 角色报告

**日期**: 2026-11-18　**角色**: pr-neon（4 个 NEON PR triage 至 merge-ready）
**工作机**: 本机 = openEuler 22.03 / GCC 10.3.1 / 鲲鹏 96 核（无 SVE；`uname -r` 5.10.0-60.139.0.166.oe2203.aarch64）

**总结论**：4 个 PR 全部处置完毕。3 个原本 CONFLICTING 的（660/627/625）已在各自 head
分支合并 origin/main 解决冲突并推送，4 个 PR 现 **merge=MERGEABLE**；全部 bot findings
逐条 triage（真实缺陷修复 / already-fixed / 噪音 三分），本轮新发现并修复 3 处同型
「整数兄弟覆盖 FP64 单元」虚假覆盖承诺残留（643 已由作者修复；627/625 各含一处由本轮
修复），660 的 4 条未处置 findings 全部闭环（含本机 GCC 10 A/B 实证与捕获期跟踪 issue
#843 的建立）。未 merge、未 resolve 任何 review thread、未动 `WIP:` 标题（按任务边界）。

---

## PR #660 — fix/memcpy1-neon-stderr-20261112 → **merge-ready**（CONFLICTING → MERGEABLE/CLEAN）

**verdict**: 7 条 local-claude-review findings 全部处置闭环；冲突解决；CI 绿。
新增提交：merge `d09286cf`（usage.md 迁址 README）+ 处置提交 `6b547d46`，head 已推送。

**findings 逐条处置**（评审锚点：前 4 条挂当前 head，后 3 条挂 de17d8d5）：

| # | finding | 判定 | 处置 |
|---|---|---|---|
| 1 | 崩溃机制未定案 + 注释「observed 142 MB」定案口吻与 coredump 记录（4294261107/58KB 映射）矛盾 | 真实 | 12:54 coredump（/tmp/core_m1n.dump）已不存在，机制永久无法定案。探针实证（本机 64KB 页）：`ROUND_UP_TO_OLD(4294261107)=4294311936`（≈4GiB，**不**截断——宏截断解释不了该现场）；`OLD(4.14GiB)=150339584`（143.4MiB，宏截断自洽现场）。三处注释（sandstone_page.h / logging.cpp / 单测）如实化：如实披露两支候选现场矛盾 + pread 头读取对两支均健壮（EOF 容忍 + 实读字节数注记） |
| 2 | LogMessagesFile 全量 mmap 巨型 memfd + mmap 失败 exit(EX_OSERR) 杀全程序 | already-fixed | a1fa5477 pread 重写已消除（当前 head print_child_stderr_common 无 mmap 路径，逐行核实） |
| 3 | 捕获期无上限（实测 356GB/3min），plan 承诺「另开 issue」无跟踪实体 | 真实 | **建立 issue #843**；plan 文档与 logging.cpp 注释挂引 |
| 4 | meson native 池注入声称修复 GCC 10 unittests 构建失败但零实证 | 真实（本机可实证） | **本机恰为 GCC 10.3.1 无 `<barrier>`**（探测块触发）→ A/B 实证：摘除注入 → `ninja unittests` 死于 `topology_cpu.h:16: fatal error: barrier: No such file or directory`；带注入 → 零告警构建 + `RoundUpToPagePast4GiB` PASS。meson 注释补记该实证，「observed 2026-11-12」声称就此坐实 |
| 5 | usage.md 未同步 stderr 截断语义 | already-fixed → 冲突迁移 | de17d8d5 已修；但 main 31f6a99e 删除了整个 docs/research/ → 合并时语义迁 README「结果判读」第 5 条（注记格式按当前代码实际输出如实化） |
| 6 | pread EINTR 短读静默丢尾无注记 | already-fixed | ec47d836 重试循环 + `total > nread` 截断注记覆盖所有短读形态；逐行核实 |
| 7 | pread 失败静默丢弃 + 不可逆截断 memfd 无痕迹 | already-fixed | ec47d836 read_error 留痕（`[stderr dump unreadable: ...]`）；逐行核实 |

**冲突解决**：`docs/research/usage.md` UD（main 删除整个 docs/research/，PR 修改该文件）
→ 接受删除、语义迁 README（merge commit `d09286cf`，说明在提交信息）。

**CI**（新 head 6b547d46）：auto-pr **pass**、pr-agent **pass**、CodeRabbit skip
（<10 stars 仓不自动检视）。

**验证实录**（rule 2，GCC 10.3.1，`-Dcpp_std=gnu++20` + meson.build 探测块自动注入
polyfill——该路径本机首次完整实证）：
- `./scripts/build.sh` 全新构建 exit 0；唯一告警 `stacksize.cpp:5 "_XOPEN_SOURCE"
  redefined` 为 main 既有 polyfill 注入路径产物（用与主二进制相同的 host 池 flags 独立
  编译该 TU 复现，与本 PR 改动无关）→ 零新告警
- `-e memcpy1_neon -t 30000 -n 1` / 全核 96 线程 → 均 `exit: pass`，日志 975 字节零打印
- `-e zstd19 -t 2000 -n 1` → `exit: pass`
- pread 钳制 E2E（scratch 载体 1.2MB stderr，验证后移除）：`--full-log` 与 compact 默认
  下失败路径均转储 `[stderr truncated: 1228800 bytes total, first 1048576 bytes
  shown]`，父进程存活
- unittests A/B（finding 4）+ `--gtest_filter=PageSizeTest.*` 5/5 PASS（含
  RoundUpToPagePast4GiB——此前仅 CI 可跑，本机 gtest 齐备直接实测）
- x86 非回归：logging.cpp/sandstone_page.h arch 中立、meson 仅 native 池、memcpy1_neon
  仅 aarch64 构建面（by inspection）

---

## PR #643 — feat/neon-rot-ldp → **merge-ready**（原本即 MERGEABLE/CLEAN，零新提交）

**verdict**: 2 条 findings 均已由作者 2026-11-17 提交正确修复（0f0b283d + a13a75c4），
本轮复核修复质量 + 全门禁本机实测，**无需任何改动**。

| # | finding | 判定 | 处置 |
|---|---|---|---|
| 1 | [Important] docstring「整数兄弟的标量黄金覆盖 FP64 单元」虚假覆盖承诺 | already-fixed（质量已核实） | 0f0b283d：现明确披露「FP64 向量单元盲区 NOT covered elsewhere——整数兄弟只执行整数向量 ALU、零 FP64 指令，家族内无任何独立黄金覆盖」，与探针证据一致，如实到位 |
| 2 | [Minor] plan Task 3 Step 2 残留「标量黄金」陈旧门禁文本 | already-fixed | a13a75c4：改「同路径 NEON 重算，非标量黄金」，与 Task 1 探针裁决及实现一致 |

第二轮检视（2026-11-17 09:33，针对当前 head a13a75c4）**零 findings**；codex 的
failed/未处置 state 挂旧 head 9e20dd9c（其 inline findings 因用量限制未产出，无实质
未处置项）；PR-Agent「No major issues」。

**CI**: 全绿（CodeQL/analyze/zizmor×2/auto-pr/bot-coverage/gitleaks/osv-scanner/pr-agent）。

**验证实录**（rule 2，本机全量新构建 exit 0；两条告警 stacksize `_XOPEN_SOURCE` +
mesh_upi_sse_sym 格式均为 main 侧既有、与本 PR 无关）：
- `-e neon_rot_ldp -t 5000 -n 1` → `exit: pass`；全核 → `exit: pass`；
  `-s 2 -t 5000 -n 1` → `exit: pass`（换 seed 黄金可复现）
- objdump pair 形态门禁（ko verify 同款）：run 函数 `ldp×9 / stp×7 / ldr q=0 /
  str q=0`，fadd/fsub/fmul/fmax v.2d 四槽全在；golden 循环 `ldp×8 / stp×6 /
  ldr q=0 / str q=0`（同路径 NEON 重算与实现相符）
- `-e zstd19 -t 2000 -n 1` → `exit: pass`；`--list-tests` 注册恰 1 条
- x86 非回归：meson 注册在 `host_machine.cpu_family() == 'aarch64'` 块内（by inspection）

---

## PR #627 — feat/neon-rot-ldr-rand-ldp-stp → **merge-ready**（CONFLICTING → MERGEABLE/CLEAN）

**verdict**: 两轮检视（11-10 轮 11 条、11-17 09:50 轮 5 条）作者已自行修复 16 条（逐条
核实在位）；本轮核验 09:50 轮 5 条修复全部到位；**新发现 1 条真实残留**（56c62837 修复
独立性声明时引入的同型虚假覆盖承诺）已修复；add/add 冲突解决；ko 全 22 门禁本机真跑
全过。新增提交：merge `f8c3fde8` + `ff90fbe2`（覆盖承诺如实化）+ `bfab2f52`（合并
复活 verify.log 的退跟踪恢复）。

**findings 处置**：
- 11-10 轮 11 条（同路径 golden/insmod 退码吞 SDC 检出/定长 256 次仅 1.6ms/单核无
  per-CPU/FNV 哈希非字节级/-W 无 -Werror/反汇编门禁窄/verify.log 入库/死代码 g_seq/
  lineage 失实/缺 README/5.10 构建）：already-fixed——作者 2026-11-17 06:14–06:47 的
  11 个提交，本轮逐条核实在位（标量独立 golden 且 FMAXNM 语义对齐 C fmax、退码透传、
  sweeps/duration_ms 定时、per-CPU、门禁强化、退跟踪、删死代码、lineage+README）
- 11-17 09:50 轮 5 条：already-fixed（11:15–11:21 的 5 个提交）——`nproc` → `nproc
  --all` 对齐 for_each_online_cpu、FPSIMD 收容改按寄存器类（V/D/S/Q/H/B）、定时 2s
  路径入 verify.sh 门禁 [6/6]、`flush_work` 逐项代替 `flush_workqueue(system_wq)`、
  golden 独立性声明收窄
- **本轮新发现**：56c62837 的替换句「该缺陷类由整数 slot 父测试（的标量整数 golden）
  覆盖」为虚假覆盖承诺（整数父测试零 FP64 指令、对 FP64 ALU 故障零检出，与 #643 检视
  同轮证伪的断言同型）→ 三处（README/模块头注/golden 函数注）如实化为「本家族内无
  任何独立黄金覆盖」，`ff90fbe2`
- **合并副产品**：add/add 三方合并把 8fb57210 已退跟踪的 verify.log 复活回跟踪集 →
  `bfab2f52` 恢复退跟踪（`git ls-files` 复核 6 文件，无 verify.log）

**冲突解决**：4 文件 add/add（main 侧 #626 合入的「add ko」与本分支首提交 9b48f00a
**逐字节相同**（`git diff` 实证为空），本分支自该点推进 13 个修复提交）→ 四文件全取
本分支侧（--ours），即「main 现状 + 本 PR 修复」的精确合成（merge commit `f8c3fde8`）。

**CI**（新 head bfab2f52）：auto-pr **pass**、pr-agent **pass**。

**验证实录**（rule 2，本机内核 5.10.0-60.139 + 内核头在位 + sudo 可用）：
- `make` 从 head 源码重编 `.ko` 零告警；`sudo bash verify.sh` → **PASS=22 / FAIL=0**，
  关键门禁实录：`[PASS] userspace_ref 编译 (-Werror 零告警零报错)`、`[PASS] FPSIMD
  收容 (FP/NEON 寄存器仅出现在 fp64_golden_init/fp64_run_sweeps 内)`、`[PASS] 指令
  替换完整 (数据通路无 ldr q/str q, 已用 ldp/stp)`、**定时持续路径**（sweeps=0
  duration_ms=2000）**96 在线 CPU 全 PASS、每核 ≥285733 sweeps、反 no-op 门禁过**、
  `nproc --all` 对齐 96=96、无 Oops/BUG、rmmod 干净
- x86 非回归：PR 仅新增 tools/fp64_rowmajor_ko/ 子树（内核模块工具），不触框架/测试
  构建面（by inspection）

---

## PR #625 — feat/neon-fp64-tests-20261109 → **merge-ready**（CONFLICTING → MERGEABLE）

**verdict**: 六轮检视（11-10×2、11-11×3、11-17 09:55）作者已自行修复绝大多数（f4f5c9f9
守卫/45c17137 NULL 判空/4fb6ad99 标量 golden/5d1988de fopen 报错/d22d121b pid 唯一
文件名/bac8a3b6+1e206872 NaN 比对两轮拉锯后收敛/634fc0da 覆盖声明收窄/545f77db 计数
链）；本轮核验发现 **5 条真实残留**并全部修复（5 提交），4 处合并冲突解决，全验证后
推送（head 47547f5e）。

**findings 残留处置（本轮修复，提交哈希）**：

| # | finding（来源轮） | 判定 | 处置（提交） |
|---|---|---|---|
| R1 | [Important] L2 检测自造链绕过框架 device_info——与基测试同机给出 512KiB vs 1.25MB 两个「L2 尺寸」；探测落空静默缩水无告警（11-11 00:08） | 真实残留 | detect_l2_bytes 优先序改为与基测试 memcpy_l2_cache_size 同源（device_info[].cache[1] → sysconf → sysfs → 256KiB），兜底 log_warning 留痕（`8d8173b4`） |
| R2 | [Minor] special_values_check 只比对 lanes[0]，lane 1 全测试零校验点（11-11 两轮） | 真实残留 | 逐 lane 双校验（vdupq 同输入两 lane 必须一致；NaN 类别 + 非 NaN 字节级，日志带 lane 号）（`69a1df21`） |
| R3 | [Minor] FMA 尾循环 `dst[i]=scalar_reference(dst[i])` 恒真自比 + 生产不可达（11-11 两轮） | 真实残留 | 尾循环改 d-form NEON（vld1_f64/vfma_f64/vst1_f64——FMLA d-form，留在被测数据通路族上），tail_self_check 从恒真变为与标量 golden 的真独立比对；注明生产 count 恒为 8 倍数、尾路径由 kTailCount=7 自检覆盖（`aebd5c90`） |
| R4 | [Minor] dump 落 CWD 相对 movbe_log 弱于兄弟 /var/tmp panic 持久盘约定（11-11 两轮）+ docstring「skipped silently」过时（5d1988de 已加 log_error） | 真实残留（文档面） | docstring 如实化：保留 movbe_log（在树 dump 家族约定，sve_rot_* 8 例同款 movbe_log/gps_rowmajor/、dualcmp 消费该布局）+ 如实披露 trade-off（panic×tmpfs CWD 丢 dump 风险、建议持久盘 CWD 运行战役）+「静默」改「log_error 留痕」。**未**改目录（两约定在树并存，约定争议留 maintainer 裁量——如实披露代替翻烙饼）（`e94e09bb`） |
| R5 | 634fc0da 收窄声明时引入「whole-unit 故障类由 sibling scalar-integer 族填上」虚假覆盖断言 | 真实残留（本轮新发现，与 643/627 同型） | golden 注释如实化：「该整单元故障类在本家族内无任何独立黄金覆盖——标量整数兄弟只执行整数向量 ALU、零 FP64 指令，对其零检出；为已披露的家族级盲区，非已委派覆盖」（`47547f5e`） |

其余 findings 判定：守卫不完整、NULL 判空、同路径 golden、fopen 静默、文件名碰撞、
plan checkbox 失实（README 361 行已含该测试，checkbox 与事实相符）、README 571/689
外推（脚注如实标注「算术外推未在 0xd02 重测」）——均已由作者提交处置或如实披露。
16:43 轮「NaN 类别比对触线非字节级红线」与 09:55 轮「字节级比对绑实现定义 NaN
payload 会假阳性 FAIL」两次**相反**指控，1e206872 收敛为「非 NaN 字节级 + NaN 类别 +
实现定义性披露」——两轮指控的交集成立，终态可辩护。

**冲突解决**（4 处，merge commit `eaa9da2e`）：
- README/CLAUDE.md 计数链：main 的 2026-11-17 无 SVE 实测（458/566/570）为基 + 本分支
  +2 非门控 PROD 增量句（0xd01 2026-11-10 实测 459/567/571 如实保留）；表 571/689 为
  本分支算术外推（脚注如实标注）；main 的 SVE2 配套与 WHILEWR 诚实边界注记全文保留
- .gitignore：双方新增并集（/run-logs/ + docs/superpowers/），run-logs 注释如实化
  （其原引 progress-log 已被 main 31f6a99e 清理出库）
- docs/research/progress-log.md：接受 main 整目录删除；本分支 soak/战役台账增补仅存
  git 历史（证据链以 docs/superpowers/plans/ 两计划文档为准）

**验证实录**（rule 2，GCC 10.3.1 本机）：
- 全量新构建 exit 0；R1–R5 逐提交增量编译零告警；最终态 `./scripts/build.sh` 干净通过
- `-e memcpy_l2_cache_size_neon_fp64 -t 5000 -n 1` / 全核 96 线程 → 均 `exit: pass`
  （special_values_check 与 tail_self_check 在 init 期执行——pass 即 14×14 特殊值
  矩阵两 lane 全过 + d-form 尾路径与标量 golden 字节一致）
- `-e neon_rot_ldr_at_top_rowmajor_fp64_dump -t 5000 -n 1` / 全核 → 均 `exit: pass`
- `-e zstd19 -t 2000 -n 1` → `exit: pass`
- x86 非回归：两文件注册均在 aarch64 守卫块内（arm64/meson.build 整目录守卫 +
  tests/cpu meson aarch64 块，by inspection）
- 如实记录：提交过程一度在注释里写入 `dump*/` 序列提前闭合块注释致编译失败，当即修复
  后重建通过（最终态已全量验证）

**CI**（新 head 47547f5e）：auto-pr **pass**（9s）、pr-agent 运行中、CodeRabbit skip。

---

## 横向发现（供 orchestrator/人裁量）

1. **同型虚假覆盖承诺三连**：「FP64 向量/算术单元故障由整数兄弟（标量整数 golden）
   覆盖」在 #643（作者自修 0f0b283d）、#627（56c62837 引入、本轮修 ff90fbe2）、
   #625（634fc0da 引入、本轮修 47547f5e）三处独立出现——整数 NEON 用例零 FP64 指令
   是架构事实，该盲区为家族级、无任何独立黄金覆盖。已三处如实化，但**家族级结论值得
   入 docs**（如 README 检测能力表的盲区披露），未越权代做。
2. **660 残留特性**：stderr 转储钳制按 fracturing 回合计（每回合 ≤1MiB），回合数无界
   则日志总量无界；compact 日志（main 新增，默认开）折叠 pass 条目时不转储子进程
   stderr——失控测试若不失败则其 stderr 不入日志。两者均为 main 侧设计语义，记录非
   缺陷。
3. **CLAUDE.md doc map 引用已删文件**：main 31f6a99e 删除 docs/research/ 后，main 的
   CLAUDE.md 文档地图仍指向 docs/research/usage.md / adding-tests.md / progress-log.md
   （3 处）。pre-existing，未越权修（CLAUDE.md 不在本角色 touches）。
4. **issue #843**（本轮建立）：子进程 stderr 捕获期无上限——memfd 可至数百 GB（实测
   356GB/3min），整机 OOM 风险；PR #660 仅修转储侧。建议排期捕获期钳制。
