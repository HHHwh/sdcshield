# 类型SDC 捕获战役（满载激发 + sdc-run scan 逐核定位 + 定型）实施计划

> **For agentic workers:** 本计划为**运维/战役型**计划（非代码实现），由当前会话 native 执行
> （长时后台进程 + 定时巡检），不适用 subagent 逐任务分发。步骤用 `- [ ]` 跟踪。

**Goal:** 在本机（96 核 Kunpeng 920 / 29Gi RAM）上以最大化激发强度运行 SDC 联合战役，
用 `sdc-run.sh` 的 scan/path 命令做逐核定位与通路归因，记录一切异常，直至捕获自然 SDC
并完成类型定型（触发条件、位模式、核局部性、复现性、分类）。

**Architecture:** 三段漏斗（v5 方法学）：满载联合激发+检测（stress-ng full/excite 全核 +
SDCShield 定向检测长跑）→ 触发后失败分类协议（同 seed 复测×3 冷/热 → -n 1 →
transient / full_core_only / sdc_suspect）→ 逐核定位（scan --keep-bg）与通路归因
（path + 定向用例）。全程台账记录，异常即冻结现场。

**Tech Stack:** `sdc-stressng`（stress-ng 0.22.01-sdc.1 fork，`scripts/sdc-run.sh` 六模式
+ `sdc-scan.sh`）；`sdcshield` builddir（704cd1c3，484 用例）；`scripts/run/sdc_detect.sh`
（优先序轮转）。

**Spec:** 用户 /goal（2026-10-16，五条要求）；方法学
`docs/sdc-excite-reproduce/sdc-excite-reproduce-7x24.md`（v5 五段闭环）；
运行手册 `../sdc-stressng/docs/runbook.zh-CN.md`（全部命令已实跑核验）。

## 术语解释（类型SDC）

"类型SDC" = 捕获自然 SDC 事件后完成**分型定型**：①核/执行单元/通路定位 ②位模式
（xor 单比特/多比特/位位置）③触发条件（负载组合/并发/温度/时序）④复现性
（seed 固定复现率、冷热差异）⑤分类（transient / full_core_only / sdc_suspect /
软件误报 / 数值非确定性）。模板见本计划"定型模板"节。

## Global Constraints

- **不做 systemd service**（用户规则：SDC 检测/激发只按需进程运行，nohup 前台/后台进程）
- **不伤害共存负载**：gem5-fi-ding 战役（~6 核 + ~1Gi）为另一会话活跃工作，禁杀；
  内存保留 ≥2.5Gi MemAvailable 余量，OOM 偏好杀本战役进程（本方进程 oom_score_adj≥500）
- **内存"拉满"工程口径**：工作集逼近可用容量但不进 swap 不 OOM；watchdog 监控
  MemAvailable/swap，越限自动降档（阶梯 512→768→1024 维度）
- **电压/频率不可控为硬件边界**（Kunpeng 920 无 cpufreq；BMC 调压平台不可用）——
  如实记录，以负载强度/温度累积（preheat）替代
- **单补丁单提交**、`git commit -s`、只推 feature 分支；战役产物（台账/日志）放
  `~/sdc-campaigns/campaign-20261016_type-sdc/`（仓外），仓内只落计划/结论文档
- **所有结论附实测输出**；"未检出"必须附运行时长与等效试验数（v5 §1.3 口径）
- 机器拓扑事实：96 核（0-95，无 SMT），NUMA 0-47/48-95，无 SVE（SVE/SME 用例预期
  SKIP，非缺陷）；每核 L1 64K/L2 512K，4×L3 24M

## Review Focus（最可能坑人的五类，逐条有对策）

1. **软件误报当 SDC**（10-13 adox 前车之鉴）→ 一切 FAIL 先走分类协议（同 seed 复测×3），
   确定性复现=软件嫌疑，查该用例近期 commit
2. **shield 侧早退无结果**（今晨 run6-v2b 只留头部日志）→ Phase 0 冒烟必须确认
   shield-cmd 全链产出 yaml；TMPDIR/XDG_RUNTIME_DIR 按昨夜 v2 方案
3. **swap 拖慢/OOM** → 阶梯标定 + 10s 采样 watchdog + 自动降档
4. **长跑脚本自身死掉无人知** → nohup + 心跳文件 + 定时巡检（CronCreate ~23min）
5. **单核隔离消灭复现**（CPU179 教训）→ scan 必带 --keep-bg 48；复测保留 aggressor 环境

---

### Task 0: 就绪检查与冒烟（今晚 ~21:00，~20min）

**Files:** 建 `~/sdc-campaigns/campaign-20261016_type-sdc/`（ledger.md、env 快照）

- [x] 系统快照：`nproc`、`free -h`、`cat /proc/loadavg`、`sensors 2>/dev/null | head`、
      `cat /sys/devices/system/cpu/online`、dmesg 近期 RAS 行、gem5 进程清单（PID/内存）
- [x] sdc_detect 冒烟：`cd ~/wangxu/code-review/sdcshield && env SDC_BIN=./builddir/sdcshield
      bash scripts/run/sdc_detect.sh --smoke` → 预期 35s 内跑完、0 fail
- [x] shield-cmd-v2 全链冒烟（今晨疑点复现检验）：复制 shield-cmd-v2.sh 改 `-T 3m`，
      设 TMPDIR 指向战役 tmp/，nohup 跑完 → **必须产出 yaml 且含 test 结果**；
      若复现"只留头部"，先修此问题（查 forkfd/shm 报错）再进 Phase 1
- [x] sdc-run.sh 冒烟：`cd ~/wangxu/code-review/sdc-stressng && NG=./stress-ng
      ./scripts/sdc-run.sh scan -t 5 --keep-bg 48 -o <战役dir>/scan-smoke` →
      预期逐核产出 core_*.yaml（5s/核 × 96 ≈ 10min，可 -c 0-3 只冒烟 4 核）
- [x] 写 ledger.md 头部（环境基线、共存负载、战役参数）

### Task 1: 内存最大化标定（~40min）

**目标：** "根据当前内存容量最大化所有用例参数"——实证选出 shield 最大稳定档

- [x] 三档阶梯（512 / 768 / 1024 维度族，即昨夜 v1/v2 参数之间），每档
      `nohup ./builddir/sdcshield -f no -T 8m -t 30s -n 24 <用例集> <档位 -O> -Y` +
      同跑 stress-ng full `-t 480`（真实联合环境），每 10s 采
      `/proc/meminfo`（MemAvailable/SwapFree）
- [x] 判据：全程 MemAvailable ≥2.5Gi、SwapUsed 不增、无 OOM-killer 行 → 该档"稳定"
- [x] 产出 `max_params.sh`（最大稳定档的 shield 命令，固化到战役目录）
- [x] stress-ng 侧内存拉满检查：确认 full 模式 addrspace 峰值 RSS（昨夜实测 7.5Gi）；
      若 sdc-run.sh 有内存预算 env 可调则按剩余内存上调（读 sdc-run.sh 源确认）

### Task 2: 基线逐核定位 scan（用户指定，~2h）

- [x] `cd ~/wangxu/code-review/sdc-stressng && NG=./stress-ng nohup ./scripts/sdc-run.sh
      scan -t 60 --keep-bg 48 -o <战役dir>/scan-baseline -t 60` →
      96 核 × 60s + 开销 ≈ 1.7h；期间 shield 不跑（避免争抢），keep-bg 48 提供
      全机并发环境
- [x] 产物核验：`summary.txt`（扫描核数/可疑核数）、`suspects.txt`（若 rc=2 有可疑核）、
      96 份 core_N.yaml；rc 期望 0（无可疑核）
- [x] 若出可疑核：直接跳 Task 4 触发协议（对可疑核定向复测 + path）

### Task 3: 主战役——满载联合激发+检测（过夜 ~8h，多样性三窗口）

- [x] **窗口 A（~4h）**：`NG=./stress-ng nohup ./scripts/sdc-run.sh full -t 14400
      --preheat 10 --sdcshield <战役dir>/max_params.sh -o <战役dir>/full-night1`
      （cpu/fma/operand-var/addrspace/varyload 全核 + di/dt 阶跃 + shield 最大档定向检测，
      -F 首错即停）
- [x] **窗口 B（~2h）**：`NG=./stress-ng nohup ./scripts/sdc-run.sh excite -t 7200
      --preheat 10 --sdcshield <战役dir>/max_params.sh -o <战役dir>/excite-night1`
      （11 站轮换最广通路覆盖：+armcrypto/memrate/vm-rand-offset/llccross/ooopress）
- [x] **窗口 C（~2h）**：`env SDC_BIN=<abs> nohup bash scripts/run/sdc_detect.sh`
      （shield 全核自载，484 用例优先序轮转，P1 向量FMA 优先——SEVI >92% SDC 由 FMA 贡献）
- [x] 串联编排：写 `night1.sh` 顺序跑 A→B→C（nohup 包裹），每窗口间 2min 冷却
- [x] 巡检（CronCreate ~23min）：心跳（A_full.log 增长）、MemAvailable、swap、温度、
      dmesg RAS、shield yaml 是否产出；异常（fail/早退/OOM）→ Task 4

### Task 4: 触发响应协议（任一 FAIL/可疑核时执行）

- [x] **冻结现场**：不停负载（CPU179 教训），立即快照 dmesg、`/proc/interrupts`、
      温度、loadavg、gem5 共存状态、stress-ng/shield 日志尾部到
      `<战役dir>/events/<时间戳>/`
- [x] **失败分类协议**（v5 §漏斗）：同参数同 seed 复测 ×3（冷态=直接重跑；
      热态=preheat 10m 后重跑）→ 全复现=确定性（软件嫌疑，查用例 commit 与
      golden 构造）；部分复现/仅负载下复现 → `-n 1` 单核复测 →
      分类 transient / full_core_only / **sdc_suspect**
- [x] **逐核定位**：`NG=./stress-ng ./scripts/sdc-run.sh scan -t 120 --keep-bg 48
      -c <嫌疑核范围> -o <战役dir>/scan-trigger1`
- [x] **通路归因**：`NG=./stress-ng ./scripts/sdc-run.sh path -t 600 -c <嫌疑核>
      -o <战役dir>/path-trigger1`（sve2 预期 SKIP 无 SVE；ls64/crc32 按硬件探测）
      + shield 定向：`-e <失败用例族> --random-seed <原seed> --cpuset <嫌疑核>`
- [x] **台账登记**：时间/用例/核/seed/位模式(xor)/ttd/负载/温度/复现性/分类

### Task 5: 分析、定型与报告（每窗口结束 + 战役收尾）

- [x] ledger 汇总：异常事件全列表 + 分类结论 + 置信度
- [x] 若捕获 SDC：按定型模板出 `<战役dir>/SDC_TYPE_REPORT.md`（仓内同步
      docs/cases/ 条目）；若零事件：按 v5 §9.6 给出核心小时数与置信上界
      （"k=0 时给上界而非 0% 风险"）
- [x] 仓内文档：progress-log 追加条目；本计划勾选完成项；commit -s 推 feature 分支
- [x] 向用户报告：结果先行，触发条件/表现特征/下一步建议

### Task 6: 策略迭代（未捕获时逐夜优化）

- [ ] 维度/时长/窗口配比调整（-t 30s 快轮转 vs 长驻留深测：seed 扫描扩覆盖、
      固定 seed 长驻留扩时间覆盖——v5 核心判断，两者都要）
- [ ] 用例集调整：向撞出相位敏感的用例族倾斜（neon_rot/gather/mesh 已在）；
      可加 P4 位扩散族（zstd/crc32/arm_crypto）窗口
- [ ] 温度轴：白天满载累积热环境（自然热残池）后再跑敏感窗口

---

## 定型模板（捕获 SDC 时逐项填写）

| 维度 | 记录项 |
|---|---|
| 定位 | 核号 / 执行单元候选（ALU/FPU/向量/LSU/一致性）/ 通路（scan+path+用例映射）|
| 位模式 | xor 值、翻转比特数、比特位置、lane、跨 byte 模式 |
| 触发条件 | aggressor 负载组合、并发度（-n）、工作集、温度、频率状态、ttd、相位 |
| 复现性 | 同 seed 复现率 x/3、冷/热差、单核 vs 满核、跨日衰减 |
| 分类 | transient / full_core_only / sdc_suspect / 软件误报（附证据链）|
| 证据 | yaml + 日志 + 现场快照哈希 + dmesg/RAS + topology |

## 自检结论（self-review）

- 五条 /goal 要求 → Task 覆盖：①激发强度（T1 内存最大化+T3 三窗口+preheat）②scan
  逐核定位（T2 基线+T4 触发态，path 归因）③多方法（full/excite/detect/参数阶梯/温度累积；
  电压频率硬件不可控已如实标注）④异常记录分析（T4 协议+ledger+定型模板）⑤迭代至捕获
  （T6）+ 全核全内存拉满（T1 判据+watchdog）✓
- 无占位符；所有命令可直接执行；预期输出已注明
- 执行方式：native（当前会话 + 后台进程 + CronCreate 巡检），理由：长时运维型战役，
  监控-响应闭环需会话内上下文
