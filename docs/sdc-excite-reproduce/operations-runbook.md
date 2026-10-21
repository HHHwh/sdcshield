# sdc-excite-reproduce 运维手册（Operations Runbook）

> 版本：M5（2026-09-27）。本手册是 v5 方案 `sdc-excite-reproduce-7x24.md` **§16 运行手册的
> as-built 落地版**——目标态条目与实装的差异一律如实标注，不虚称。部署手册（从零装起）
> 是 `scripts/sdc-excite-reproduce/NEW_BOARD_ONBOARDING.md`；本手册覆盖**运行期**操作。
> 数据根缺省 `~/sdc-excite-reproduce`（`$SDC_EXCITE_REPRODUCE_DIR` 可覆盖）。
> 运行实态锚点：参考机 TaiShan 2280（128 核），9 服务 2026-09-26 部署、2026-09-27 00:17
> 整体重启后连续运行。**本手册 §4 巡检模板取代 v4 遗留的 `scripts/campaign` 模板
> （该路径随 M0 命名迁移已不存在，不再使用）。**

## 0. v5 §16 覆盖对照（验收核对单）

| v5 §16 操作项 | 本手册落点 | as-built 状态 |
|---|---|---|
| §16.1 Preflight 就绪检查（10 项） | 附录 A.1 十项表 | ✅ 落地（NTP 不可用→双源时间戳等缺口如实标注） |
| §16.2 Phase 1 全量画像 | 附录 A.2 | ✅ collect_inventory v2（capabilities.env + known_faults + 想采但采不到清单） |
| §16.3 Phase 2 构建与冒烟 | 附录 A.3 | ✅ 架构分支 + 预构建/源码两路径 + zstd19 冒烟门 |
| §16.4 启动与正常运行 | §1 九服务清单与启停顺序 | ✅ 眼睛先于负载（远端观测面〔研究〕未部署，本地 spool 为事实来源） |
| §16.5 发生 mismatch | §1.5 事件链 + §7 复现排班 | ✅ handle_failure→eventd→RED→ring 固化→repro_queue 全链 |
| §16.6 hang/panic/失联 | §2 BLACK 恢复 + §1.4 重启语义 | ✅ Restart=always + 阶段超时判别 + BLACK 人工确认 |
| §16.7 停止与恢复 | §1.3 停止顺序 + §1.4 | ✅ 反序停止 + state.json 断点 + restore 幂等 |

---

## 1. 九服务清单与启停顺序（v5 §16.4/§16.7）

### 1.1 服务清单（全部 `Restart=always` + `RestartSec=10` + `StartLimitIntervalSec=0`）

| # | 服务 | 单元 | 用户 | 职责 | 健康判据 |
|---|---|---|---|---|---|
| 1 | 驱动 | `sdc-excite-reproduce.service` | sdc | L0-L5 阶段调度、失败分类协议、提取式取证、台账、断点续跑 | `state.json` phase 推进；`driver.log` 有行 |
| 2 | 监控 | `sdc-monitor.service` | **root** | 60s OS/BMC 采样、温度/风扇/内存/磁盘/SEL 联锁、快照代理、exposure 快照 | `monitor.csv` 末行新鲜；`bmc_ok=1` |
| 3 | 逐核采集器 | `sdc-collector@percore.service` | **root** | percore.csv（逐核占用+实测频率）+ 频点驻留直方图 | `collector_self.csv` 丢样=0 |
| 4 | PMU 采集器 | `sdc-collector@pmu.service` | **root** | pmu_core/pmu_uncore.csv（perf 计数模式 @20s + 组轮换 + coverage 列） | 同上；`percent_covered` 可见 |
| 5 | RAS 采集器 | `sdc-collector@ras.service` | **root** | ras_edac.csv + journal RAS 流 + spurious canary（dmesg 降级路径）+ rasdaemon 监护 | 同上 |
| 6 | 特权助手 | `sdc-root-helper.service` | **root** | allowlist 特权动作（perf burst / cpu online-offline / restore）+ nonce + 审计 | `spool/root_helper_audit.jsonl` 可追加 |
| 7 | 事件产线 | `sdc-eventd.service` | sdc | 五源尾随 → canonical 事件流 `spool/events.jsonl`（档案全量保留） | `events.jsonl` 增长；`tail_offsets.json` 推进 |
| 8 | 状态机 | `sdc-controller.service` | sdc | 五态规则机 + append-only 决策账本 + cmd/ 请求分派 | 账本每 2s 一行 heartbeat |
| 9 | 热窗 | `sdc-ring.service` | sdc | 120s 滚动热窗；red/black 事件固化 `events/<id>/ring_window/` | `spool/ring_offset.json` 推进 |

一键概览：`bash scripts/sdc-excite-reproduce/status.sh`（只读，任意用户）；
九服务活性：`systemctl is-active sdc-{excite-reproduce,monitor,collector@percore,collector@pmu,collector@ras,root-helper,eventd,controller,ring}`。

### 1.2 启动顺序（原则：**眼睛先于负载，消费者先于生产者**）

首启全序（NEW_BOARD_ONBOARDING.md 第 3-5 步的浓缩，顺序不可换）：

```bash
su -c "bash scripts/sdc-excite-reproduce/install.sh"        # 0) 装单元+logrotate（enable 不 start）
su -c "systemctl start sdc-monitor"                          # 1) 联锁眼睛先就位
timeout 900 bash scripts/sdc-excite-reproduce/sdc-excite-reproduce.sh smoke   # 2) 10min 冒烟（手动，不动 systemd）
su -c "systemctl start sdc-excite-reproduce"                 # 3) 冒烟过后正式 7×24
su -c "systemctl enable --now sdc-collector@percore sdc-collector@pmu sdc-collector@ras"   # 4) 三采集器（@pmu 先做 PMU_CORE_COUNTERS 预算注入，见 onboarding 第 4 步第 2 点）
su -c "systemctl start sdc-root-helper sdc-eventd"           # 5) M2 四守护：特权通道+事件产线
sleep 10                                                     # 6) 等 eventd 首轮回填（≥3 轮 2s 轮询）
bash scripts/sdc-excite-reproduce/m2_controller_baseline.sh  # 7) controller 消费基线（见 1.2.1）
bash scripts/sdc-excite-reproduce/m2_ring_baseline.sh        # 8) ring 消费基线（同理）
touch ~/sdc-excite-reproduce/spool/root_helper_audit.jsonl   # 9) 审计通道预建（空文件）
su -c "systemctl start sdc-controller sdc-ring"              # 10) 状态机+热窗最后上线
```

**1.2.1 消费基线为什么必须**（M2 部署核心教训，实测依据）：eventd 首启把五源全部历史
回填 events.jsonl（本机实测 193 条，含 2×sdc_mismatch(red) + 25×interlock(black)）；
controller/ring 若无基线以 offset=0 直接启动，会把历史喂进状态机 → 参考板直接终态
BLACK，并对**陈旧事件分派特权动作**（perf burst 真触发）——安全事故。基线脚本把消费
offset 预置到回填末尾（字节单位，复用 `tail_file` 单一实现），只消费部署时刻之后的
实时事件；events.jsonl 档案不删不改，全史可 `replay`。

整体重启（如维护后）：`su -c "systemctl start sdc-monitor sdc-collector@{percore,pmu,ras} sdc-root-helper sdc-eventd sdc-excite-reproduce sdc-controller sdc-ring"`
——已部署过的机器不需要重做基线（状态文件在，controller 重启不丢 RED/BLACK、不重放
旧决策；ring 重扫护栏防覆盖既有固化证据）。

### 1.3 停止顺序（启动的反序：负载先撤，眼睛最后）

```bash
su -c "systemctl stop sdc-excite-reproduce"    # 1) 负载源（断点保留 state.json，重启续跑）
su -c "systemctl stop sdc-controller sdc-ring" # 2) 消费者
su -c "systemctl stop sdc-eventd"              # 3) 事件产线
su -c "systemctl stop sdc-root-helper"         # 4) 特权通道
su -c "systemctl stop sdc-monitor"             # 5) 联锁眼睛（负载已撤才可停）
su -c "systemctl stop sdc-collector@percore sdc-collector@pmu sdc-collector@ras"   # 6) 采集器（独立观察者，也可保留常驻）
```

日常停止（只停战役对）用 `su -c "bash scripts/sdc-excite-reproduce/stop.sh"`（等价
上述 1+5）。v5 §16.7 的 restore unit 语义 as-built 由 root-helper 承担：hotplug 改写
前快照 online 集（`spool/pre_state_online.txt`，不覆盖——restore 恒回最初全集；运维
有意变更 online 集后须手删该文件重锚），governor/IRQ 同理读回验证（d13 演练守卫）。
停止后收尾：跑一次 §5 报告生成 + 核对 `daily_summary.log`——capsule 自带 `SHA256SUMS`。

### 1.4 崩溃/失联自愈语义（v5 §16.6）

- 任一服务崩溃 systemd 自动拉起（`Restart=always` + `StartLimitIntervalSec=0`——永不
  放弃）；驱动从 `state.json` 断点续跑，冷机首轮恢复自动补做断点周期。
- **禁止自动回到高压力**：联锁 PAUSE 粘性类（disk/SEL Critical）须人工 `rm PAUSE`；
  状态机 BLACK 是终态（§2），恢复必经人工确认——这是设计不是缺陷。
- panic/系统失效：事件链 `journal panic 行 → eventd red → interlock black → controller
  BLACK + verify.request`；驱动阶段超时判别集 `{124, 137, 143}`
  （timeout/SIGKILL/SIGTERM，d07 演练实测覆盖）。
- 本机 BMC in-band 是唯一通道（管理网隔离实测；串口/远端 watchdog 不可用，如实标注）——
  失联取证靠 BMC 带外 `sel`/`sdr` 快照与 kdump/pstore 归档，恢复后先归档再续跑。

### 1.5 事件链速查（v5 §16.5 as-built 五步）

```
驱动 handle_failure（提取式取证 yaml_extract/stdout_summary/context + 定向复测×3
  + 台账行 + enqueue_repro + snapshot.request——战役不中断）
→ sdc-eventd：sdc_mismatch 事件（red）入 spool/events.jsonl
→ sdc-controller：RED（freeze_ring 自动 + snapshot_root + enqueue_reproduction + hold_profile）
→ sdc-ring：120s 窗口固化 events/<event_id>/ring_window/（[-60s,+60s] 取证）
→ spool/repro_queue/ → 低负载时段人工消费（§7：门禁七项 → capsule + 概率复测）
```

UE（EDAC 不可纠正错误）是**用户决策点**：即时告警 + 事件目录 + 台账行，不自动 PAUSE
（v5 §8.6 黑/红分级 as-built 语义，d05 演练守卫）。

---

## 2. BLACK 状态人工确认与恢复（状态机恢复入口）

### 2.1 BLACK 语义——粘性是设计

- 唯一合法入口：`interlock_action` 且 `severity=black`（monitor 的 `ALERT PAUSE:` /
  热升级 KILL 行 → eventd 定级 black）→ 规则 `safety_interlock` → **black + verify_only**
  （只读校验请求 `cmd/verify.request`，BLACK 态只允许验证类动作）。
- `transitions.black = {}`——**终态**：RESUME（green）事件到达只记 `ignored=true` 决策行，
  状态不动；controller 重启从 `spool/controller_state.json` 恢复，**重启不清 BLACK**。
- 为什么粘性：BLACK 表示"安全联锁曾以最高级别介入"（热 KILL/系统失效/断链 fail-safe）。
  自动翻绿 = 机器未经人确认就回到压力状态，违背 v5 §16.6"禁止自动直接回到高压力阶段"。
- 驱动负载由 **PAUSE 标志**（联锁）控制、状态机由 **controller** 记录——两者独立：
  thermal/fan/mem 类 PAUSE 自动恢复（RESUME 行），controller 仍留 BLACK 等人工收敛。

**真实案例（参考机，2026-09-26/27）**：18:24:37 首次 `green→black`（safety_interlock，
verify_only 已分派）；2026-09-27 08:13 与 08:32 两次 CPU 101-102°C → 热升级 KILL + PAUSE
→ 08:13:49/08:32:49 RESUME（thermal 自动恢复）；controller 至今 `state=black`，其后全部
interlock 事件 `from=black to=black ignored=true`（账本可查）；`cmd/verify.done.<epoch>`
在案（驱动消费闭环）。——教科书式的"负载已恢复、状态机等人工"形态。

### 2.2 恢复前置检查单（逐项确认后才可执行 2.3）

| # | 检查 | 命令/判据 | 不过时 |
|---|---|---|---|
| 1 | BLACK 根因在案 | `grep -v heartbeat ~/sdc-excite-reproduce/spool/controller_ledger.jsonl \| grep -B2 '"to": "black"' \| head`；对照 `monitor/alerts.log` 的 PAUSE/KILL 行 | 先完成事件分诊（§4.3 路由），未定性的 BLACK 不恢复 |
| 2 | PAUSE 标志已清 | `ls ~/sdc-excite-reproduce/PAUSE` 不存在；若在：thermal/fan/mem 应已自动 RESUME，残留的 disk/SEL 粘性 PAUSE 须先处置根因再 `rm` | 联锁未解除前不恢复压力 |
| 3 | 温度回常态 | `tail -1 monitor/monitor.csv` 温度列 ≤ 90°C（恢复线）；`ipmitool sdr type Temperature` 无越限 | 热根因未消退即恢复会二次 KILL |
| 4 | BMC 通路正常 | monitor.csv `bmc_ok=1`；`monitor/sel_events/` 无新增 Critical | 断链期恢复 = 联锁失明 |
| 5 | verify/snapshot 请求已终态化 | `ls cmd/*.request` 为空（`.done.`/`.rejected.` 尾缀是终态产物，非垃圾） | 滞留请求先按 §1.5 消费或确认驱动已消费 |
| 6 | 磁盘水位 < 85% | `df -h ~/sdc-excite-reproduce` | 高水位叠加压力有停日志风险（§8） |
| 7 | 人工干预记录 | BLACK 根因、处置、恢复决定写入台账/事件目录（E2 干预记录——报告三段分级的"事实"段来源） | 无记录的恢复在证据链上是缺口 |

### 2.3 恢复操作（controller_state 重置 + 重启）

```bash
su -c "systemctl stop sdc-controller"
bash scripts/sdc-excite-reproduce/m2_controller_baseline.sh --force   # state=green + offset 预置到当前档案末尾
su -c "systemctl start sdc-controller"
# 验收：cat ~/sdc-excite-reproduce/spool/controller_state.json → "state": "green"
#       tail controller_ledger.jsonl → heartbeat from=green；此后新事件正常决策
```

**为什么用 `--force` 基线而不是裸 `rm controller_state.json`**：状态文件同时存
`state` 与**消费 offset**。裸 `rm` 后重启 = state 回 green 但 offset=0 → controller
把 events.jsonl **全部历史**（含当初触发 BLACK 的旧行）重放进状态机 → 立即重入
BLACK 并可能对陈旧事件分派特权动作（M2 部署教训，见 §1.2.1）。`--force` 的语义
即"删除状态 + 安全地把 offset 重置到现在"——这是 `rm controller_state.json`
恢复意图的**正确操作化**。ring 侧无需动（BLACK 固化证据是资产，保留）。

---

## 3. 季度演练排期（drill，v5 §17.2 验收基准）

### 3.1 排期与一键执行

- **节奏**：每季度首周一次全量；此外**触发式加练**——9 服务单元文件/联锁代码/规则表
  （rules_m2.json）任一变更后、新单板入役前、大版本 sdcshield 更换后。
- 一键（15 类 ≈5 分钟；2026-09-29 `SDC_DRILLS_FULL=1 pytest test_drills.py`
  实测 9 passed in 267.76s——含 d15；2026-09-27 14 类时 8 passed in 275.51s）：

```bash
bash scripts/sdc-excite-reproduce/drills/drill_all.sh          # 退出码 0 = 15/15 PASS
#   子集：--only d01,d05（或 DRILL_ONLY 环境变量）；产物：--out DIR（默认 drills/out/）
#   产物：drills/out/<类名>/root/（隔离根证据）+ drill_all.log + summary.txt
```

15 类 = v5 §17.2 清单 13 项（CE/UE 拆分 d04/d05）+ restore（d13）+ kdump 只读探针（d15，
M5 终审 Minor #2 补 panic/kdump 中 kdump 半项）：合成 mismatch（d01）/
测试 bug（d02）/spurious（d03）/CE（d04）/UE（d05）/panic（d06）/runner hang（d07）/
BMC 超时（d08）/collector 崩溃（d09）/网络断开（d10）/磁盘满（d11）/时间跳变（d12）/
温度越限（d14）/kdump 只读探针（d15）/restore 失败（d13）。**安全边界**：全部在隔离
数据根 + 测试进程上（drill_lib 真实根形态守卫：DRILL_OUT 已含 spool/+driver.log 即拒）
——绝不注入真实 spool、绝不 kill 真实服务、绝不真断网/真满盘/真写 sysfs、绝不打真 BMC；
d15 三项（kdumpctl status/crashkernel 预留/转储位可写）为只读探针（真 kdumpctl 非只读，
演练用假二进制回显；`[ -w ]`=access(2) 判定，绝不写 /var/crash 内容）。

### 3.2 pytest 门控（SDC_DRILLS_FULL）

`tools/telemetry/tests/test_drills.py` 默认**跳过**全量 15 类（留给 drill_all 本体承担，
避免日常回归被 ~5 分钟演练拖慢）；`SDC_DRILLS_FULL=1` 开启（CI/人工门控）。日常回归
只跑子集断言（d01/d02/d03/d14/d15 + 守卫 + 编排格式）。

### 3.3 新演练类准入流程（六步）

1. 隔离根构造 + 注入→观测，**≥2 条断言**（复用 `drill_lib.sh` 的 `drill_init` /
   `assert_*` / `jassert` / 假二进制注入点模式）；
2. 头注释登记 v5 §17.2 清单映射（哪一项的 as-built 承接）；
3. 加入 `drill_all.sh` 的 `ALL_DRILLS` 数组（restore 类收尾的排序惯例）；
4. `test_drills.py` 补子集用例（默认跑、非 SDC_DRILLS_FULL 档）；
5. 安全边界自查：对照 §3.1 红线逐条过（真 root 动作只读探针除外）；
6. 一次 `drill_all.sh` 全量回归 15+N/15+N PASS 后合入。

---

## 4. 巡检模板（4h 节奏 + 2-5 行汇报格式；取代 v4 scripts/campaign 模板）

### 4.1 巡检项（按序 ~2 分钟，全部只读）

| # | 项 | 命令 | 正常态 |
|---|---|---|---|
| 1 | 九服务活性 | §1.1 一键 is-active | 9/9 active |
| 2 | 状态机 | `cat ~/sdc-excite-reproduce/spool/controller_state.json` | state=green；black → §2 流程 |
| 3 | 决策账本活体 | `tail -1 spool/controller_ledger.jsonl` | 2s 内 heartbeat |
| 4 | PAUSE 标志 | `ls ~/sdc-excite-reproduce/PAUSE` | 不存在；在 → 读内容（哪类联锁） |
| 5 | 最新告警 | `tail -5 monitor/alerts.log` | 无新 PAUSE/KILL/风暴行 |
| 6 | 战役进度 | `bash scripts/sdc-excite-reproduce/status.sh` | phase 推进、cycle 递增 |
| 7 | 磁盘水位 | `df -h ~/sdc-excite-reproduce` | < 85%（§8） |
| 8 | 丢样 | `tail -3 monitor/collector_self.csv` | samples_dropped=0 |
| 9 | 新事件 | `tail -3 events/ledger.csv` | 对照上次巡检；新行 → §4.3 |
| 10 | 复现队列 | `ls spool/repro_queue/ \| wc -l` | 0；堆积 → §7 排班消费 |
| 11 | SEL 异态 | `grep -h Critical monitor/sel_events/*.txt \| tail -3` | 无新增断言 |

### 4.2 汇报格式（2-5 行，固定骨架）

```
[巡检 YYYY-MM-DD HH:MM] 服务 9/9 active｜controller=green｜driver cycle=N <phase>｜无 PAUSE｜磁盘 XX%｜新事件 K｜repro_queue M｜丢样 0
异常：（无 / 一行一事：现象+数据+初判）
处置：（无 / 已做/将做什么+谁做）
下一步：（无 / 需用户决策点/待办）
```

异常行必须带数据（温度值/rc/水位数），不带数据的"看起来正常"不写。BLACK/RED/
新 SDC 候选事件**单独起事件分诊**（§4.3），不塞进巡检行。

### 4.3 异常分诊路由

| 现象 | 路由 |
|---|---|
| controller=black | §2 前置检查单 → 人工确认恢复 |
| 新 ledger 失败行（rc∈{1,137}） | 等驱动复测分类完成 → 事件目录 classification.txt → §7 消费排班；竞态三探针按 onboarding M2 段 |
| PAUSE 持续 ≥30min（thermal/fan/mem 未自愈） | 查风道/负载/传感器；§2 表 3-4 项 |
| SEL Critical 新增 | 粘性 PAUSE——调查后人工 `rm PAUSE`；known_faults 白名单豁免仅离散断言告警 |
| repro_queue 堆积 >0 | §7（低负载时段消费） |
| collector 丢样 >0 或服务反复重启 | `journalctl -u <unit> --since -1h` + collector_self.csv；PMU coverage 骤降同查 |

---

## 5. 报告生成 SOP（sdc_report，M4 退出标准载体）

### 5.1 命令（只读数据根，与运行中 9 服务完全共存）

```bash
python3 tools/analysis/sdc_report.py --data-root ~/sdc-excite-reproduce -o /tmp/report.md
# 单事件深挖：--event-id <id>；五段：概述/位形态/时序/假设排序/建议
```

### 5.2 guard 红线（报告尾部自检段必须 0 违例——违例是 bug，修报告不是删自检）

源出 brief 四违例、as-built 拆为 **5 类行级扫描**：①无分母比率（百分数而无 `n=`/分母）；
②无区间发生率（「率」有分母而无 `[` 区间/上界/CI）；③零率表述（k=0 必须写
「上界 X（95% 精确）；rule of three ≈ 3/n」，不写 0 率）；④「PMU 异常」×「SDC」同现
而无「证据非判据」限定；⑤「根因已定位」无 E2/E3/E4 证据等级前缀。

### 5.3 排除与降级语义（诚实纪律）

- **污染事件排除**：台账 note 含「测试污染」/竞态关键词/定性非 SDC → INVALID，
  **不入 SDC 率分母但单独列出**（不静默丢弃）——分母口径与 `sdc_eventd` 同源。
- **暴露量**：`spool/exposure.json` 最后有效快照供 `rate_table` 双口径分母（迭代
  Clopper-Pearson / 核时 Poisson）；缺文件/坏行/字段 null → 「暴露量不可得，仅事件
  计数」，不得据此作零率或"无 SDC"声明。**as-built 现状**：exposure.json 尚未产出
  （monitor 待重启，§9.2）且驱动日汇总行缺 loop-count token（§9.1）——迭代口径
  如实降级，核时口径已有效。
- **三段分级**：报告正文事实/推断/假设三段不得混写；无 E2（单变量反事实干预）以上
  只可用「相关/候选」措辞。

---

## 6. capsule 重放 SOP（run.sh）

capsule 位于 `spool/repro_capsules/<event_id>/`（消费产出，§7）；自带 `SHA256SUMS`
全文件校验与 `scripts/run.sh`。

### 6.1 预检（先 --check-only，永不直接发射）

```bash
bash ~/sdc-excite-reproduce/spool/repro_capsules/<event_id>/scripts/run.sh --check-only
```

预检五项（manifest 冻结 vs 重放机实测）：内核 release / governor / online 集 /
拓扑指纹（逐核 package-cluster-core-die + NUMA cpulist 的规范化哈希）/ BMC 热状态
允许区间（`["ambient","+10C"]` 形式，无数值基线时记注记不强拒）。另做二进制哈希
核验——`binaries/` 只放 sha256+路径引用**不复制二进制**（500MB 级），`$SDC_BIN`
可指向本机一致二进制。

### 6.2 退出码与失败处置表

| 退出码 | 含义 | 处置 |
|---|---|---|
| 0 | 预检通过（--check-only 即止） | 可进入重放（去 --check-only，victim 先起、aggressor 并发、rc 归并） |
| 2 | manifest 预检拒绝（stderr 列具体项） | 见下逐项 |
| 3 | 二进制哈希不符 | `SDC_BIN=<一致二进制路径>` 重跑；不一致即拒绝——不同构建不可比 |
| 4 | 缺 manifest.json | capsule 损坏，从 repro_capsules 上游/归档重新取 |

预检拒绝逐项：**拓扑指纹不符** = 不同机器或固件/热插变更——诚实拒绝，不属可修错误，
换原机或记录环境差异后放弃重放；**online 集不符** = 有核下线，先 `restore`（root-helper）
回快照全集再验；**governor 不符** = 按 manifest 记录值切回再验；**热状态超区间** =
等冷却到 ambient+容差内再验（ipmitool 不可读时记注记不强拒——降级不装死）。

### 6.3 重放纪律

重放是真实受界发射：预算受 manifest limits 约束；与运行中战役同机时**先巡检 §4.1
确认无 PAUSE、低负载**再发射；结果 yaml 落 capsule `replay/`，与原事件位形态
（actual/expected/xor）对照才算复现声明。

---

## 7. repro_queue 消费排班（--from-queue --once）

### 7.1 看板与节奏

`ls ~/sdc-excite-reproduce/spool/repro_queue/`（文件名 epoch-ns 数值序即入队序）——
**目录即看板，堆积即待办**。产出端是驱动 handle_failure 钩子（战役运行中自动入队）；
消费端是**按需 CLI（裁定不部署常驻守护）**，排班归人：

- 常规：每日一次低负载时段（参考机选夜间/深驻留间隙）消费到空；
- 事件驱动：巡检发现堆积且当日无消费窗口 → 优先安排；
- 队列消费**不自动追新**：新事件入队等下一窗口（复测是有界负载，不无理由重跑）。

### 7.2 消费命令与预算

```bash
python3 tools/excite/sdc_reproducer.py --from-queue --once --data-root ~/sdc-excite-reproduce
# 逐事件：from_event（失败 test/seed 锚定 detecting 核构造最小 victim profile）
#   → 真实性门禁七项（含健康核对照真实发射）→（过）capsule 打包 + 概率复测 3 次
# 结果：spool/repro_done/<队列文件名>（processed/gate_rejected/invalid/error/already_done）
```

预算（真实发射，非模拟）：健康核对照 120s + 概率复测 3 次（复测总预算
`QUEUE_EVENT_BUDGET_S=300s`、per-trial 截断）+ capsule 打包——**单事件最坏 ~7min**。
前置：`MemAvailable ≥ 6GB`（内存门，低于则事件跳过并如实记录）。排班规则：一次
窗口消费件数 × 7min ≤ 低负载窗；复用 retest_guarded 护栏（前置门 6GB + 看门狗 2.5GB）。

### 7.3 done 状态判读

`processed`（复测 k/n + Wilson CI + capsule 路径）｜`gate_rejected`（拒因入案**不重试**——
证据不足/竞态定案/健康核亦复现等，转 test_bug 或补证据后重新入队）｜`invalid`（毒丸
JSON 原文截留）｜`error`（from_event 响亮拒绝：无种子/无锚点）｜`already_done`（崩溃
恢复幂等）。

---

## 8. 磁盘水位与轮转策略

### 8.1 水位联锁（monitor 执行，campaign.env 可覆盖）

| 水位 | 动作 | 语义 |
|---|---|---|
| ≥ 85%（DISK_WARN_PCT） | 每小时首分钟 `ALERT 磁盘水位` | 告警（yellow）——巡检响应清理 |
| ≥ 95%（DISK_STOP_PCT） | `PAUSE: disk ...（停新日志）` | 停新日志 + 驱动 PAUSE；**粘性**——清理水位后人工 `rm PAUSE`（§2.2 表 2 同则） |

### 8.2 轮转矩阵（什么转、什么绝不转）

| 数据 | 策略 |
|---|---|
| `logs/*.out`、`driver.log`、`daily_summary.log`、`stressng.log` | logrotate：**daily + rotate 14（14 天保留）+ compress + delaycompress + copytruncate**（`/etc/logrotate.d/sdc-excite-reproduce`） |
| `logs/YYYYMMDD/*.yaml`（取证 YAML） | 驱动日汇总时 gzip 昨日及更早（实测比 ≈12×）；归档后**永久保留**，不入 logrotate |
| `events/`（台账+事件目录+ring_window）、`monitor/`（CSV/告警/SEL）、`spool/`（事件档案/账本/审计/capsule/done） | **证据目录永不轮转删除**（v5 §3.4：事件证据单独保留） |
| `spool/exposure.json` | **不轮转**（追加式 JSONL 档案：轮转会切断 report"最后有效快照"的消费连续性，且属证据纪律；体积 ~每 10min 一行，可忽略） |

### 8.3 容量预算与实况

as-built 实测外推 raw ≈75MB/天（pmu_core 66 + uncore 6 + percore 2.4 + self 0.5 +
ras/日志 <1；v5 §5.6）。参考机实况（2026-09-27）：数据根 7.6G、/home 水位 67%——
余量充足；81 机入役前按 §16.1 表 4 预留 ≥50GB。

---

## 9. 欠账与部署重启待办清单（T1 评审移交 + M5 收敛清单）

### 9.1 驱动 daily_summary 补 loop-count token（T2 已实施，待重启生效）

- **原欠账**：`daily_summary()`（`sdc-excite-reproduce.sh`）的 driver.log 日汇总行只有
  「日汇总完成（历史 YAML 已压缩）」，无 loop-count 累计 → monitor 的
  `write_exposure` 解析不到 token → `exposure.json` 的 `valid_iterations_today`
  **如实为 null**（source 注记 `unavailable(...)`），报告迭代口径降级（§5.3）。
- **修法（T2 as-built，`feat/sdc-excite-reproduce-m5-tail`）**：`daily_summary()`
  统计**今日** `logs/YYYYMMDD/` 下逐文件 `loop-count:` 行数之和 `N` 与文件数 `M`，
  把 `loop-count 日累计: <N>（YAML 文件数 M）` 写入 driver.log 日汇总行
  （`日汇总完成 loop-count 日累计: N（YAML 文件数 M）（历史 YAML 已压缩）`），
  同时写入 `daily_summary.log` 块。monitor 解析器兼容 token 形
  `loop[-_]count(?:\s*日累计)?[:=：]?\s*<N>`（旧档 `loop-count=N` 与 T2 `loop-count 日累计: N`），
  旧档无 token 仍降级 null。
- **口径注记**：`N` = 今日 YAML 里 `loop-count:` 行数合计（= 各线程 main-loop 记录数，
  非 loop 次数值之和；框架仅 `-vvv` 下产该行，无则计 0）。压缩只作用于昨日及更早，
  故今日文件多为 `.yaml`（未压）——实现同时计 `.yaml` 与 `.yaml.gz`，避免恒 0 假真值。
  日汇总行**不含 cycle token**，`exposure.json` 的 `cycle` 仍为 null（既有状态，未在本单元扩围）。
- **生效链**：补丁合入 → 驱动重启（§9.2）→ 次个日汇总点 → monitor 聚合真值 →
  report 迭代分母 `n=` 从「不可得」转真值。

### 9.2 M5 脚本改动的重启生效清单（部署动作）

| 改动 | commit | 生效条件 | 现状 |
|---|---|---|---|
| monitor `write_exposure`（exposure.json 生产） | 208bbcff（2026-09-27 12:49） | **重启 sdc-monitor.service** | 运行进程 00:17 启动=旧代码；**待重启**。建议低负载时段执行（BMC 轮询中断 <1 周期；联锁语义不变） |
| 驱动 daily_summary 补 loop-count token（§9.1，T2） | 本单元提交 | **重启驱动（sdc-excite-reproduce.service）** | 运行进程=旧代码；**待重启**——重启后次个日汇总点写 token。另：M3 的 enqueue_repro/consume_verify_request 接线已于 09-27 00:17 重启生效（`cmd/verify.done.*` 在案实证） |
| tools/analysis（T2 机读 hint/标签接续） | 5ad565a5 | 无（离线工具零部署，按需调用即新代码） | 已生效 |
| drills / deploy_81machine.sh | f2b551fb..f51dc0a4 | 无（按需脚本） | 已生效 |
| systemd 单元加固（NoNewPrivileges+PrivateTmp，5 模板） | befbef7c | **install.sh 重装单元 + 重启对应服务** | 模板已合入；/etc/systemd/ 现行单元未动，**待用户重启窗口** |

### 9.3 其他移交待办（按优先级）

1. **81 机风扇联锁用户决策**（T4 移交）：TG225 B1 的 FAN3 恒 0rpm 会使战役持续
   fan PAUSE（模拟量联锁不查 known_faults）——选项 A 维持联锁只采集 / B 授权后加
   豁免代码路径（独立补丁单元）/ C 先修传感器。**入役前必须决策，不得静默绕过**。
2. ~~**systemd 单元沙箱加固**（M2 评审 Minor 移交）~~ **已闭环**（befbef7c）：核实五个
   服务实际读写面后，模板仅加 `NoNewPrivileges=yes` + `PrivateTmp=yes`（只收紧写面；
   root-helper 写 sysfs cpu online/offline、monitor 读 /dev/ipmi0/sysfs/journalctl、
   三采集器读写数据根均不受影响；经核这五服务不用 /tmp，spool/状态全在数据根）。
   `ProtectSystem=full`/`ProtectHome` 未纳入（ProtectHome 会切断用户级服务对 /home
   数据根的写；root-helper 写 sysfs 亦须保留）。`systemd-analyze verify` 五个渲染后
   模板无本单元诊断。生效条件：重装单元（install.sh）→ 重启服务，属用户决策窗口。
3. **status.sh 呈现扩展**（v5 单元 14 未完部分）：纳入九服务活性/controller 状态/
  repro_queue 看板（§4.1 巡检项目前靠手工组合命令）。
4. **README 运维段引用**：仓库 README 尚无 sdc-excite-reproduce 运维入口——本手册
   合入后按 docs 同步纪律补引用行。
5. **m2 基线脚本在 install.sh 重装后的重做**：install.sh 覆盖单元文件不改 spool 状态
   （基线状态文件在即拒），但 `sdc-collector@.service` 重装后 M1 的 PMU_CORE_COUNTERS
   注入行需重做（onboarding 第 4 步第 2 点）。

---

## 附录 A：部署前三查（v5 §16.1-16.3 as-built 对照）

### A.1 Preflight 十项就绪检查（任一"否"必须明示影响，不得静默跳过）

| # | v5 §16.1 项 | as-built 方法与判据 |
|---|---|---|
| 1 | 裸金属 | `systemd-detect-virt` = `none` |
| 2 | 权限 | `sudo -n true` 或 root 密码（经 stdin 传 su，不落盘）；提权自动探测 root > sudo > 降级（缺口记 00_gaps.txt） |
| 3 | BMC/IPMI 可达 | `ipmitool mc info`；不可达 → OS 侧降级 + 温度联锁只剩 acpitz（如实记录） |
| 4 | 磁盘空间 | 数据目录所在盘预留 ≥50G；85%/95% 联锁见 §8.1 |
| 5 | 时间同步 | `timedatectl`；本机 NTP 不可用（UDP/123 阻断实测）→ **双源时间戳**（系统 + BMC `sel time`；事件带五元时间，`sdc_time.py` 锚定残差可见） |
| 6 | 散热与供电基线 | `ipmitool sdr type Fan`、`sel list \| tail -50` 无 Critical；空载基线距 Tjmax ≥30°C |
| 7 | 无生产负载 | 专用机确认（共存则逐核隔离结果有噪声，如实标注） |
| 8 | 网络与包管理 | 记录联网与否 → 预构建 rpms（A.3 路径 A）或离线/容器构建（路径 B） |
| 9 | 二进制与配置 hash | resolve 冻结含二进制 sha256（`sdc_profile.py` 单一权威）；capsule/门禁均按哈希核验 |
| 10 | 时间映射/PMU/kdump/restore + L0 自检 | `sdc_time` 锚点 + `capabilities.env` 的 PMU_CORE_COUNTERS 实测值 + kdump 状态 + restore 幂等（d13）+ L0 冒烟 `zstd19 -n 1 -t 2000` exit: pass + 10min smoke |

### A.2 全量画像（collect_inventory v2）

`scripts/sdc-excite-reproduce/collect_inventory.sh docs/superpowers/inventory/`（幂等；
三种提权方式见 onboarding 第 1 步）。产物 16 文件 + `capabilities.env`（核数/cpufreq
与频率源/governor 响应实验结论/PMU 拓扑+计数器预算/EDAC 拓扑/轨集/EINJ-BERT-HEST
存在性/OEM 调压结论/cpu online 可写性）+ `known_faults` 注册表。**必须输出**：机器
档案表 `00_MACHINE_PROFILE.md` + 「想采但采不到」清单 `00_gaps.txt`（不得留空假设）。
部署后用同脚本刷新数据根 capabilities.env（onboarding 第 4 步第 1 点）。
**内存专项基线**（区分 CPU SDC vs 内存 SDC）：压测前/后各跑一次
`stress-ng --vm N --vm-bytes 70% --verify`（或离线 memtest）用于归因排除。

### A.3 工具获取与构建（架构分支先行）

- **架构分支**：`uname -m` = aarch64 → sdcshield（基准平台）；x86-64 → 上游 OpenDCDiag
  官方构建优先（sdcshield x86 路径仅交叉验证参考）；任何架构配 stress-ng `--verify`
  + rasdaemon 补充层。
- **路径 A 预构建**：`third-party/rpms/` 按 openEuler LTS/SP——**SP 必须精确匹配**
  （错配 → glibc 降级死结，安装脚本拦截）；或 Actions MultiOS Verify 自包含 tarball。
- **路径 B 源码**：vendored 依赖 `third-party/{openssl,openblas,sleef,isa-l,acl}/build.sh`
  （幂等）→ `PKG_CONFIG_PATH=./third-party/eigen5 meson setup builddir --buildtype=release
  && ./scripts/build.sh`；离线/旧版本走 `scripts/offline-build/container-build.sh`。
- **冒烟门槛**（过后才准正式战役）：`-e zstd19 -n 1 -t 2000` → exit: pass。
