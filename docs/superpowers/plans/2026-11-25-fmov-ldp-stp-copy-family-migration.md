# fmov LDP/STP 拷贝探针族 10 件回迁接入 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把研究仓 `~/wangxu/sdcshield/tests/cpu/fmov/` 的 10 个测试件（asm 5 件 + intrinsics 5 件，`diff -r` 已证与本地工作树**逐字节一致**）接入本仓构建，按 rule 1 每件一提交回迁、验证、推送；终态 fmov/meson.build 与研究仓逐字节一致；README/CLAUDE.md 计数链同步。

**Architecture:** `tests/cpu/meson.build` 加一行 `subdir('fmov')`（fmov/meson.build 自带 aarch64 守卫，x86-64 零改动，抄 arm64/ 自门控模式）→ 按研究仓提交序逐件回迁（每件 = 文件 + meson 行 + rule 2 四项验证 + `commit -s` + push）→ 末件后 docs 计数链同步提交。10 件 = 346 套件 SIMDFPLoadStoreMemLoopCpy 谱系端口（P05/P06/P08/P13）+ adaptive_ldp_stp 自适应引擎 ×（asm / intrinsics 受控编码判别）双臂。与 misc/ 的 `fmov_train*` 族（跨堆 FMOV 消费链）为**不同测试族**：全仓 DECLARE_TEST ID 零重复（grep 已验证）、shm 前缀独立——**misc/ 无需删除任何件**（用户条件指令的前提不成立）。

**Tech Stack:** C++20/C17, GCC, meson/ninja, NEON armv8.1-a 纯基线（无 SVE/SVE2/crypto 门控）——本机 0102（192 核无 SVE 鲲鹏 920，即原验证机）全件可原生验证，**gem5-fi rule 不适用**。

**Spec:** 本文件即 spec；源 = 研究仓提交链 `2c984f8a → 485d1423 → 1c7bee0c → 64ab8b3a → 8b148a23(+77d13d47 已折叠入文件) → c419ce61 → 3d70c4ec → c566ab97 → afac25ab → 2e0bafea`（分支 feat/migrate-opendcdiag-tests）。

## 线程模型（用户问题 1 的结论，实现者须知）

框架层：每测试 fork 子进程，per-CPU worker 线程并发调 `test_run(test, cpu)`（docs/architecture.md）→ **10 件默认全为全核多线程**，`-n 1` 即单线程。

- **固定配方 8 件**（p05/p06/p08/p13 × asm/intr）：线程间零协调——init 在子进程内一次性建共享只读 master/golden，run 里各 worker 用 per-thread 工作缓冲独立跑相同 sweep，天然全核安全。
- **adaptive 2 件**：额外跨进程/跨线程 POSIX shm 协调——combat 模式同一 30min 墙钟窗口内全 worker/全子进程锁**同一组合**（全核同步激发同配方）；`VERIFY=1` round-robin 分工，192 worker > 54/27 组合，一次短跑覆盖全部金身；`MODE=test` 逐组合巡测。依赖框架默认 auto-fracture 再 fork 换组合（故无 fracture_loop_count 覆盖）。p05/p06/p08/p13 件显式 `-1` 永不 fracture。

## 部署方式（用户问题 2 的结论）

构建接线（唯一缺口）→ `meson setup --reconfigure` + `./scripts/build.sh` → 10 件自动注册 PROD（无 spec 过滤门控，任何 920 可跑）→ 默认集 0102 预期 521→531。板级部署 = 标准 sdcshield 二进制拷板；adaptive 件战役脚本按 env 契约（`ADAPTIVE_LDP_STP_KEY/VERIFY/COVFILE/COMBO/MODE/TEST_SLICE`，intr 件前缀 `_INTR`）驱动，首失败由脚本 SIGKILL 主进程，KEY 对象归脚本清理。

## Global Constraints

- **rule 1**: 每件一提交，顺序 verify → commit → push，绝不打包；多件清单顺序推进。
- **rule 2** 每提交四项：(a) 构建零**新**警告（基线既有 3 条 stl_vector 假阳性不变）；(b) 引用真实运行输出，禁止 "should work"；(c) `./builddir/sdcshield -e zstd19 -t 2000 -n 1` → `exit: pass`；(d) x86 非回归靠检视并在 commit message 注明（自门控 subdir，x86 文件零触碰）。
- **rule 3**: 全部提交在 `feat/fmov-ldp-stp-copy-family`（从 main 切出），push 该分支，**永不 push main**；每次提交前 `git branch --show-current`。
- **DCO**: 尾行精确 `Signed-off-by: wangxu <wangxumarshall@qq.com>`，其后无任何内容、无 Co-Authored-By。`git commit -s` 后核对尾行（先例 d4142409：Author=wangxumarshall 而 S-o-b=wangxu，不符则 `--amend` 修正）。
- **meson 演进**: 中间每提交只引用已提交文件（否则 configure 失败）；**终态 = 当前磁盘版 fmov/meson.build**（与研究仓逐字节一致，作为唯一权威蓝图，中间态从它裁剪）。
- **commit message**: `feat(arm64): fmov/<件名> 回迁——<要点>（研究仓 <hash>）`，正文含验证实录（构建/运行/gate/回归计数真实输出）。
- **基线**（本机 0102，2026-11-25 实测）: PROD 521 / q0 630 / q-1 634；每件落地逐 +1。

## Review Focus

1. **meson 中间态引用未提交文件** → configure 直接红。Pin：每提交步骤 2 的干净构建（Task 1–10）。
2. **intrinsics 臂被 GCC 折叠成 `bl memcpy`** → 与既有 28 件 memcpy 舰队重复、编码判别性归零（研究仓实测单调与交替循环均被折叠，flag 后为真实逐元素 ldr/str 火车）。Pin：每个 `_intr` 件 objdump 其 run 函数确认无 memcpy 调用（Task 6–10 步骤 3）。
3. **标量金身向量化泄漏**（issue #373 范式违约）→ Pin：`scripts/check-scalar-golden.sh builddir/sdcshield <金身符号>` 每件过门（Task 1–10 步骤 4；p08 两臂无金身计算、跳过）。
4. **adaptive 双引擎 shm 前缀碰撞/对象泄漏** → asm/intr 前缀独立（`adaptive_ldp_stp_ppid*` vs `adaptive_ldp_stp_intr_ppid*`）。Pin：双引擎并行短跑后 `/dev/shm` 两前缀对象并存不串扰（Task 10 步骤 5）。
5. **计数链断裂** → Pin：每提交 `--list-tests` 精确 +1；Task 11 三档终测。

---

### Task 1: 分支 + 目录接线 + ldp_stp_cpy_p08（研究仓 2c984f8a）

**Files:**
- Create: `tests/cpu/fmov/ldp_stp_cpy_p08.cpp`（已在磁盘，git add）
- Create: `tests/cpu/fmov/meson.build`（首版：从当前磁盘版裁剪——asm lib 完整结构但 `fmov_tests` 仅 `'ldp_stp_cpy_p08.cpp'` 一行，无 intr 块；头注释保留）
- Modify: `tests/cpu/meson.build`（`subdir('sve')` 行后加注释块 + `subdir('fmov')`；注释注明自门控 aarch64、346 套件谱系、x86 零改动，风格抄 arm64/ 注释块）

**Interfaces:**
- Produces: `subdir('fmov')` 接线（后续任务不再动 tests/cpu/meson.build）；fmov/meson.build asm lib 块（Task 2–5 往 `fmov_tests` 列表追加行）。

- [ ] **Step 1: 切分支** — `git checkout -b feat/fmov-ldp-stp-copy-family && git branch --show-current`（预期输出分支名）
- [ ] **Step 2: 裁剪 fmov/meson.build 首版 + 加 subdir('fmov')** — 按上方 Files 编辑两文件
- [ ] **Step 3: 构建零新警告** — `meson setup --reconfigure builddir && ./scripts/build.sh`；预期：成功，警告数 = 基线（3 条既有假阳性）
- [ ] **Step 4: 运行验证（引用真实输出）** — `./builddir/sdcshield -e ldp_stp_cpy_p08 -t 3000 -n 1` → pass；`-e ldp_stp_cpy_p08 -t 3000`（192 核满载）→ pass
- [ ] **Step 5: 回归** — `./builddir/sdcshield -e zstd19 -t 2000 -n 1` → `exit: pass`
- [ ] **Step 6: 计数** — `./builddir/sdcshield --list-tests | grep -c '^[a-z]'` → **522**（521+1）
- [ ] **Step 7: commit + push** — `git add tests/cpu/fmov/ldp_stp_cpy_p08.cpp tests/cpu/fmov/meson.build tests/cpu/meson.build && git commit -s`（核对 DCO 尾行）`&& git push -u origin feat/fmov-ldp-stp-copy-family`

### 标准回迁配方（Task 2–10 共用；各任务仅列差异行）

- [ ] **Step 1**: fmov/meson.build 对应 lib 的 `files()` 列表追加该件一行（Task 6 同时逐字引入 intr lib 块，自当前磁盘版拷入，含 `-fno-tree-loop-distribute-patterns` 及其注释）
- [ ] **Step 2**: `meson setup --reconfigure builddir && ./scripts/build.sh` → 零新警告
- [ ] **Step 3**: 运行该件 `-e <id> -t 3000 -n 1` 与满载各一次 → pass，引用真实输出；`_intr` 件另做 objdump 判别（Review Focus 2：`objdump -d builddir/sdcshield` 定位该件 run 函数，确认无 `bl.*memcpy`）
- [ ] **Step 4**: gate（Review Focus 3）— `scripts/check-scalar-golden.sh builddir/sdcshield <该件金身符号>` → pass
- [ ] **Step 5**: 回归 `-e zstd19 -t 2000 -n 1` → `exit: pass`
- [ ] **Step 6**: `--list-tests` 精确 +1（Task 1 后基线递增）
- [ ] **Step 7**: `git branch --show-current` → `git add` 两文件 → `git commit -s`（标题含研究仓 hash，正文含验证实录）→ `git push`

### Task 2–5（asm 臂余件，差异行）

| 任务 | 件（研究仓 hash） | gate 金身符号 | 备注 |
|---|---|---|---|
| Task 2 | `ldp_stp_cpy_p13`（485d1423） | `ldp_stp_cpy_p13_init` | L2 3×128KB 双源 EXT #8 |
| Task 3 | `ldp_stp_cpy_p05`（1c7bee0c） | `ldp_stp_cpy_p05_init` | 100 轮标量 fold 内联于 init 覆盖 |
| Task 4 | `ldp_stp_cpy_p06`（64ab8b3a） | `ldp_stp_cpy_p06_init` | 5a_a5 字常量填充（防 init 向量化） |
| Task 5 | `adaptive_ldp_stp`（8b148a23+77d13d47） | `ald_scalar_golden` | 运行面加强（见下） |

**Task 5 运行面加强（替换配方 Step 3）：**
- [ ] 默认 combat 短跑：`./builddir/sdcshield -e adaptive_ldp_stp -t 3000` → pass（全 worker 锁 combo#0：ldp/stp l2 s2 alurot）
- [ ] verify 金身覆盖：`ADAPTIVE_LDP_STP_VERIFY=1 ./builddir/sdcshield -e adaptive_ldp_stp -t 3000` → pass（192 worker round-robin 覆盖 54 组合）
- [ ] 组合锁定抽查：`ADAPTIVE_LDP_STP_COMBO=<k> ./builddir/sdcshield -e adaptive_ldp_stp -t 3000 -n 4`（k 取 27：ldr/str 中段组合）→ pass
- [ ] COVFILE：`ADAPTIVE_LDP_STP_COVFILE=/tmp/ald.cov ADAPTIVE_LDP_STP_VERIFY=1 ... -t 3000` 后 `cat /tmp/ald.cov` 有 `<iso> <pid> verify <combo>` 行

### Task 6–10（intrinsics 臂，差异行）

| 任务 | 件（研究仓 hash） | gate 金身符号 | 备注 |
|---|---|---|---|
| Task 6 | `ldp_stp_cpy_p08_intr`（c419ce61） | 无（纯拷贝无金身计算） | meson 引入 intr lib 块；objdump 无 memcpy |
| Task 7 | `ldp_stp_cpy_p13_intr`（3d70c4ec） | `ldp_stp_cpy_p13_intr_init` | objdump |
| Task 8 | `ldp_stp_cpy_p05_intr`（c566ab97） | `ldp_stp_cpy_p05_intr_init` | objdump |
| Task 9 | `ldp_stp_cpy_p06_intr`（afac25ab） | `ldp_stp_cpy_p06_intr_init` | objdump |
| Task 10 | `adaptive_ldp_stp_intr`（2e0bafea） | `aldi_scalar_golden` | 运行面加强（见下） |

**Task 10 运行面加强（替换配方 Step 3，env 前缀 `_INTR`、27 组合）：**
- [ ] 默认 combat / `ADAPTIVE_LDP_STP_INTR_VERIFY=1` / `ADAPTIVE_LDP_STP_INTR_COMBO=<k>`（k 取 13）各短跑 pass
- [ ] **双引擎并行**（Review Focus 4）：`./builddir/sdcshield -e adaptive_ldp_stp,adaptive_ldp_stp_intr -t 5000` → 双 pass；跑前跑后 `ls /dev/shm/ | grep adaptive_ldp_stp` 观察两前缀对象并存、无第三前缀串扰（战役期对象随子进程亡而亡，ppid 对象由收割器处理——如实记录所见，不虚构）

### Task 11: docs 计数链同步（单独 docs commit，23b6a352 先例）

**Files:**
- Modify: `README.md`「测试用例与检测能力」主段（~348 行链文）+ footnote（~394）
- Modify: `CLAUDE.md` `--list-tests` 注释块（追加 2026-11-25 实测链目）

- [ ] **Step 1: 三档终测**（本机 0102 实测，预期 531/640/644，**以实测为准**）：`--list-tests` / `--quality=0` / `--quality=-1` 三档计数，引用真实输出
- [ ] **Step 2: 全族短跑**：`-e p05,p05_intr,p06,p06_intr,p08,p08_intr,p13,p13_intr -t 5000` 满载 → 全 pass（引用输出）
- [ ] **Step 3: 编辑 README 链文** — 追加：fmov 目录 10 件（346 套件 SIMDFPLoadStoreMemLoopCpy 谱系 P05/P06/P08/P13 × asm/intr 受控编码判别对 + adaptive_ldp_stp 双引擎 54/27 组合），研究仓 2c984f8a…2e0bafea 逐字节回迁，0102 主机 2026-11-25 实测三档计数
- [ ] **Step 4: 编辑 CLAUDE.md 注释块** — 同步链目（与 README 口径一致）
- [ ] **Step 5: commit + push** — `docs: sync counts after fmov/ 10-test migration (+10, 2026-11-25 measured)`

## Self-Review 记录

- 覆盖：10 件 = Task 1–10；接线 = Task 1；docs = Task 11；用户三条问题在「线程模型/部署方式/Architecture」作答。无缺口。
- 步骤可执行性：配方 + 差异表使每步唯一可写；meson 中间态裁剪规则唯一（从磁盘终态裁剪）。
- 类型一致：gate 符号、env 前缀、研究仓 hash 与源核对一致（`aldi_scalar_golden` 见 adaptive_ldp_stp_intr.cpp:279）。
- Review Focus 5 项均 pin 到任务步骤。
- 比例：件代码已存在（逐字节），本计划纯为接线/验证/提交编舞，无代码转写。
