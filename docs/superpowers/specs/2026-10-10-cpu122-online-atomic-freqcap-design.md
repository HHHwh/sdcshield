# CPU122 上线即封顶（原子降频）设计

> **日期**: 2026-10-10
> **受测板**: sdc1-01-04（128 核 Kunpeng 920，坏核 = 逻辑 CPU122 = MPIDR `0x0900060200`）
> **问题来源**: `cpu122-online-offline.sh 上线` 后坏核满频致死——上线后约 10ms，online uevent 尚未离开内核、udev 未收到事件、cap 脚本未开始运行，整机已卡死
> **上游文档**: `docs/cases/sdc1-01-04-core122/SDC1-01-04-CPU122-BOOT-ISOLATION-CONFIG.md`（§4.1 实验通道、§4.3 降频封顶协议）
> **决策记录**: 2026-10-10 用户裁定采用方案 A（部署 out-of-tree 内核模块归零封顶窗口）

---

## 1. 问题与根因

### 1.1 竞态表象

CPU122 频率依赖实证（配置总账 §4.3）：2.9GHz 满频负载 <4s 即崩；1.45GHz 同种子 60s×184 迭代全 pass。因此每次上线必须封顶。而封顶动作（写 `scaling_max_freq`）发生在上线之后——窗口内坏核满频裸奔，实测 10ms 内整机卡死。

### 1.2 根因（openEuler 6.6 / 上游 v6.6 源码级，三条事实）

**事实一：uevent 触发源全部位于风险窗口结束之后。**
`echo 1 > online` 的 `write()` 路径：`store_online → device_online → bus->online`（完整热插拔，CPU122 跑完全部 cpuhp 状态）→ `kobject_uevent(KOBJ_ONLINE)` → write 返回。死亡发生在 write() 内部热插拔阶段 = uevent 发出之前。udev cap 链路（uevent → udevd epoll → fork worker → exec 脚本 → 写 sysfs，累计 50–500ms）**架构性必输**，与实现快慢无关。一切"观察到 online=1 再封顶"的用户态形态（udev、轮询、脚本串行 echo）同理。

**事实二：内核在 online 路径内主动把坏核打到最高频率。**
`drivers/cpufreq/cppc_cpufreq.c` `cppc_cpufreq_cpu_init()`（经 `cpufreq_online()` 挂在 `CPUHP_AP_ONLINE_DYN` 槽位、在 `echo 1 > online` 同步路径内、由 CPU122 自身执行，且早于一切 governor/notifier）：

```c
policy->cur = cppc_cpufreq_perf_to_khz(cpu_data, caps->highest_perf);
cpu_data->perf_ctrls.desired_perf = caps->highest_perf;   /* = 2.9GHz boost */
ret = cppc_set_perf(cpu, &cpu_data->perf_ctrls);          /* 直接写 CPPC 硬件 */
```

风险窗口不是"没来得及降下来"，而是驱动 init 亲手写上去的。

**事实三：用户态不存在跨 offline/online 的限值持久化通道。**

- cppc_cpufreq 无 `.online` 回调 → 复用 policy 路径仍走完整 `->init` → `policy->max` 重置 + 再次 slam（`cpufreq.c` `cpufreq_online()` 的 `else` 分支）；
- `scaling_max_freq` sysfs 节点离线时不存在（板上实证，配置总账 §4.3）；
- per-cpu policy（板上 A/B 实证：122@1.45G 与同簇 120–123@2.9G 并行运行）→ 无"借同簇好核保持限值"通路；
- freq_qos 请求挂在 `policy->constraints` 上、policy 结构热插拔 offline 不释放（`__cpufreq_offline` 不 free，仅 `cpufreq_policy_free` 于设备移除时调用）→ **qos 约束客观上可跨热插拔存活，但用户态无 freq_qos 接口**。

**结论：纯用户态无解，封顶必须进入上线路径本身（内核侧）。**

### 1.3 频率失控三时刻分解

| 时刻 | 内容 | 频率决定者 | 覆盖手段 |
|---|---|---|---|
| ① | PSCI 热启动入口 → `CPUHP_AP_CPUFREQ_ONLINE` 之前的早期 cpuhp 状态 | 固件 | 内核侧不可达；残余（§7） |
| ② | 驱动 init slam：`cppc_set_perf(desired=highest)` | 驱动 | kprobe `cppc_set_perf` pre-handler 改写参数 → **归零** |
| ③ | governor 稳态：`policy->max` 由 `cpufreq_set_policy()` 中 `freq_qos_read_value(FREQ_QOS_MAX)` 聚合得出（权威源） | governor | 模块挂 FREQ_QOS_MAX 请求 → **归零** |

依据（`cpufreq.c` v6.6）：
- `cpufreq_online()`：`driver->init()`（slam）→ `CPUFREQ_CREATE_POLICY` notifier（仅新建 policy 时发出）→ `cpufreq_init_policy()` → `cpufreq_set_policy()` → governor 启动/限频；
- `cpufreq_set_policy()`：`new_data.max = freq_qos_read_value(&policy->constraints, FREQ_QOS_MAX)` → `policy->max` 由 qos 决定 → governor（performance）按 `policy->max` 请求目标频率；
- policy 创建时注册 `cpufreq_notifier_max` 于自身 constraints（`freq_qos_add_notifier(FREQ_QOS_MAX, ...)`）→ **qos 请求变更自动触发 policy 更新与 governor 重限频**（改档无需上下线）。

依赖符号已在受测板内核系列（`6.6.0-159.4.x`，本机 kernel-devel `Module.symvers`）全部确认导出：`freq_qos_add_request`/`freq_qos_update_request`/`freq_qos_remove_request`、`cppc_set_perf`/`cppc_get_perf_caps`/`cppc_khz_to_perf`、`register_kprobe`/`unregister_kprobe`、`cpufreq_register_notifier`、`cpufreq_cpu_get`。

---

## 2. 目标与非目标

**目标**
1. CPU122 上线时，时刻②③的 >cap 频率暴露窗口为零：首次 CPPC 写入即封顶、governor 稳态永不超过 cap；
2. 封顶跨 offline/online 自动保持（一次挂载终身有效），上线从高危操作变为有守卫的例行操作；
3. 1.45GHz↔2.0GHz 实验切档在线完成（改模块参数），热插拔循环归零；
4. 模块未就绪（未加载/vermagic 不匹配/参数异常）时，脚本拒绝上线——不可裸奔上线；
5. 全程可观测（dmesg 记录挂载/clamp/改档，读数可验证）。

**非目标**
- 时刻①（PSCI 热启动早期段）归零——固件域，本设计仅记录残余风险（§7），长期走 BIOS/BMC per-core 限频（厂商通道，另行推进）；
- 不改动 127 个好核的任何行为（模块对无 policy 的 CPU 完全静默）；
- 不修改 maxcpus=122 隔离体系、udev 防自动上线、六层捕获体系——完全叠加兼容；
- 不做 DKMS/自动重编——内核升级时由 SOP+脚本 vermagic 检查强制人工重编（板上内核升级本就是人工受控流程）。

---

## 3. 方案总览

```
时刻①  PSCI热启动→cpufreq step   残余（实证低风险：历史死亡均发生于负载期或满频稳态，无一例在热启动早期段）
时刻②  init slam desired=2.9G   → kprobe cppc_set_perf pre-handler clamp 【归零】
时刻③  governor 稳态             → freq_qos MAX 请求（挂 policy，跨热插拔存活）【归零】
协议层  上线频次                 → 会话一次上线纪律 + 切档免上下线（热插拔次数最小化）
```

四个交付物：
1. 内核模块 `cpu122-capfreq`（仓内正本 `docs/cases/sdc1-01-04-core122/cpu122-capfreq/`，板上部署 `/home/sdc/cpu122-capfreq/`）；
2. 脚本 `cpu122-online-offline.sh` v2（同一目录双正本同步）；
3. 板上开机自动加载配置（modules-load.d + modprobe.d）；
4. 文档同步（配置总账、模块 README、progress-log）。

---

## 4. 模块设计 `cpu122-capfreq`

### 4.1 参数（module_param，0644）

| 参数 | 默认 | 语义 |
|---|---|---|
| `target_cpu` | 122 | 保护对象（逻辑 CPU 号）。验证期可用好核（如 123）演练 |
| `cap_khz` | 1450000 | 频率上限（kHz）。0 = 停用 clamp 与 qos；写新值即时生效并自动传导 |

`cap_khz` 写入路径：param store 回调 → 重算 `cap_perf` → `freq_qos_update_request()` → policy 自带 qos notifier 自动触发 `handle_update` → governor 重限频。**切档（1.45G↔2.0G）一次 echo，无需上下线。**

### 4.2 机制一：freq_qos 约束（覆盖时刻③）

- `cpufreq_register_notifier(CPUFREQ_POLICY_NOTIFIER)` 监听 `CPUFREQ_CREATE_POLICY`；
- 回调中 `policy->cpu == target_cpu` 时 `freq_qos_add_request(&policy->constraints, &req, FREQ_QOS_MAX, cap_khz)`；
- 首次上线（本启动 122 从未在线 → policy 新建 → CREATE_POLICY 触发）即挂载；
- policy 结构跨热插拔存活（§1.2 事实三）→ **后续每次 offline/online 约束自动在场**：re-online 的 `cpufreq_init_policy → cpufreq_set_policy` 读 qos 得 cap → governor 直接以 cap 启动。

### 4.3 机制二：kprobe clamp（覆盖时刻②）

- kprobe 注册于 `cppc_set_perf(int cpu, struct cppc_perf_ctrls *perf_ctrls)`（GPL 导出、跨模块调用不内联）；
- pre-handler（arm64：arg0=x0=cpu，arg1=x1=perf_ctrls）：`cpu == target_cpu && cap 生效` 时，`desired_perf > cap_perf` → 改写为 `cap_perf`；`max_perf > cap_perf` 同样 clamp（防御性）；
- 覆盖全部三条写 CPPC desired 的路径：驱动 init slam、governor set_target/fast_switch、`set_boost`；
- `cap_perf` 换算：模块加载时 `cppc_get_perf_caps(target_cpu, &caps)` + `cppc_khz_to_perf(&caps, cap_khz)`（对离线 CPU 可用——ACPI 命名空间数据不依赖在线状态；若失败则延迟到 CREATE_POLICY 回调时重试，此时 CPU 已在线，失败即按 §4.5 拒绝后续动作并 dmesg 报错）；
- clamp 命中记 dmesg（限速打印）+ 累计计数。

### 4.4 迟加载兜底

insmod 时若 target_cpu 的 policy 已存在（本启动已上线过、模块后加载）→ `cpufreq_cpu_get(target_cpu)` 直接对现存 policy 挂 qos（freq_qos 变更经 policy notifier 自动生效，无需额外触发）。

### 4.5 失败语义（宁可不上线，不可裸奔上线）

- kprobe 注册失败（符号不存在/kprobe 黑名单）、qos 添加失败、caps 换算失败且无兜底 → **模块加载失败**（exit 路径清理已注册项）；
- 脚本侧 vermagic/加载状态检查与之配合（§5），构成双重门。

### 4.6 可观测性

- 挂载：`cpu122-capfreq: target=CPU122 cap=1450000kHz (perf N/M), kprobe+qos armed`；
- 每次上线 clamp：`cpu122-capfreq: CPU122 online: clamped desired_perf H→N (count K)`；
- 改档：`cpu122-capfreq: cap_khz 1450000→2000000, qos updated`；
- `/sys/module/cpu122_capfreq/parameters/{target_cpu,cap_khz}` 可读。

### 4.7 边界与语义细节

- `cap_khz=0`：qos 请求移除/失效 + kprobe 直通——留作 2.9G 满频 A/B 实验的显式解锁通道（dmesg 警告）；
- 用户态写 `scaling_max_freq` > cap：经 `cpufreq_set_policy` 的 qos 聚合被钳到 cap（这正是 thermal cooling 的同款力学），无冲突；
- 用户态写 `scaling_min_freq` > cap：driver verify 拒绝（min>max），属预期行为，脚本不去动 min；
- 模块卸载：移除 qos/kprobe；若 target_cpu 在线则 dmesg 警告"裸奔"。默认不建议卸载；
- 对 `target_cpu` 以外的 CPU：notifier 回调不命中即返回，kprobe 不命中即放行，零开销零干扰。

---

## 5. 脚本 `cpu122-online-offline.sh` v2

### 5.1 保留项

准备1（udev 防自动上线）、准备2（GRUB maxcpus=122 panic=30）、准备3（补齐好核）原样保留；`下线` 流程不变（追加提示：模块封顶仍在值守）。

### 5.2 `上线` 新流程

```
门0  模块就绪检查（任一不过即拒绝上线，绝不降级为裸奔上线）:
     - lsmod 含 cpu122_capfreq
     - modinfo vermagic 与 uname -r 一致（内核升级后忘重编即拦截）
     - /sys/module/cpu122_capfreq/parameters/cap_khz sane: 0 < cap ≤ 2900000；> 2000000 打印高危警告
门1  编号漂移预检（上线前）:
     - present 恰为 0-127、offline 恰为 122
     - cpu122 topology 与 121/123 同 physical_package_id 且 core_id 介于两者之间（cluster 06 缺口 292 位置）
主操作  echo 1 > cpu122/online          ← 封顶由模块在上线路径内完成（时刻②③归零）
后验1  journalctl -k -b 0 | grep "CPU122: Booted secondary processor" 必须命中 0x0900060200；
       不符 → 立即 echo 0 > cpu122/online 下线 + die（编号漂移防护，承配置总账 §6.4 铁律）
后验2  cat cpu122/cpufreq/scaling_max_freq ≤ cap（cppc 换算取整容差 ±10kHz）
       cat cpu122/cpufreq/scaling_cur_freq ≤ cap + 容差
       dmesg 尾部应见 clamp 记录（信息项，不作为硬门槛——qos 路径可能先于 clamp 生效）
状态  全量落日志（沿用现有 log 风格）
```

### 5.3 新子命令 `封顶 <kHz>`（别名 `cap`）

- 写 `/sys/module/cpu122_capfreq/parameters/cap_khz` → 回读 → 验证 `scaling_max_freq`/`scaling_cur_freq` 自动跟随（qos notifier 自动传导，无需上下线）；
- 用途：1.45G↔2.0G 实验切档、A/B 满频实验的显式解锁（cap=2900000）与恢复。

### 5.4 用法面

```
sudo ./cpu122-online-offline.sh {上线|on|1|下线|off|0|封顶|cap} [kHz]
```

---

## 6. 部署设计

### 6.1 板上落点

| 路径 | 内容 |
|---|---|
| `/home/sdc/cpu122-capfreq/` | `cpu122_capfreq.c` + `Makefile` + `README.md`（与仓内正本同步） |
| `/etc/modules-load.d/cpu122-capfreq.conf` | `cpu122_capfreq`（开机自动加载） |
| `/etc/modprobe.d/cpu122-capfreq.conf` | `options cpu122_capfreq target_cpu=122 cap_khz=1450000` |

构建依赖：板上 `kernel-devel-$(uname -r)`（当前 159.4.13.167）。`make` → `insmod`/`modprobe`。

### 6.2 清理项

部署时检索并清除板上可能残留的 udev cap 尝试规则（`ACTION=="online"` 写 `scaling_max_freq` 类）——该方案已判定架构性必输，保留只会误导。**注意区分**：`SUBSYSTEM=="cpu", ACTION=="add"` 的防自动上线注释必须原样保留（准备1 的防线，两者无关）。

### 6.3 内核升级 SOP 增补

内核升级后：安装新 `kernel-devel` → `/home/sdc/cpu122-capfreq/` 重编 → 重启后确认模块自动加载（`lsmod` + dmesg armed 行）→ 方可上线。脚本门0 的 vermagic 检查是该流程的强制执行者。

---

## 7. 残余风险与缓解

| 风险 | 评估 | 缓解 |
|---|---|---|
| 时刻①（PSCI 热启动早期段，固件域频率） | 实证低风险：9-24 以来所有成功上线（狩猎 v3 116 项、频率研究 40+ 项）均穿越此段；全部已知死亡发生于负载期或满频稳态（含本轮 udev-cap 失败案例，位于时刻②③暴露段） | 上线时机错峰（系统 idle、kdump 武装、无实验负载）；每次会话仅上线一次（§8 协议）；长期走 BIOS/BMC per-core 限频归零 |
| kprobe 对 `cppc_set_perf` 的假设失效（符号内联/黑名单） | 低（GPL 导出、跨模块调用）；加载即验证，失败拒载 | 门0 拦截，不存在静默失效 |
| 内核升级后模块未重编 | vermagic 不匹配 | 门0 强制拦截 + SOP |
| qos 换算取整偏差 | `cppc_khz_to_perf` 线性换算，误差 ≤1 perf 档 | 后验容差 ±10kHz；实际频率以 `scaling_cur_freq` 复核 |
| 编号漂移（固件再屏蔽坏核） | 架构性既有风险（配置总账 §6.4） | 门1 预检 + 后验1 MPIDR 核对 + 自动下线 |

---

## 8. 运行协议变更（配套纪律）

1. **会话一次上线**：每加电会话内 CPU122 只做一次上线（如需在线，上线后保持——模块保证其稳态 ≤cap，长期在线从"高危"降为"受控"）。重启天然回隔离态，无需主动下线；
2. **切档免上下线**：实验间频率切换一律走 `封顶` 子命令；
3. 默认 cap=1.45GHz；2.0GHz 档实验显式 `封顶 2000000`；满频 A/B 需 `封顶 2900000`（脚本高危警告，实验记录留痕）。

---

## 9. 验证方案（板上执行，先无风险后真实）

**V1 好核演练（不碰坏核）**：`insmod cpu122_capfreq.ko target_cpu=123 cap_khz=1450000` → offline/online cpu123 循环 ≥2 次 → 验证：首次上线 dmesg qos 挂载行 + clamp 行；二次上线（policy 复用路径）约束仍在；`scaling_max_freq`=1450000、`scaling_cur_freq`≤cap；改 `cap_khz=2000000` → 读数自动跟随。卸载。
**V2 vermagic 门**：`insmod` 旧编模块于新内核 → 拒载；脚本 `上线` → 门0 拒绝。
**V3 真实上线**：`target_cpu=122` 重载 → 脚本 `上线` → 存活 + 后验全过 + dmesg 证据留存（同步至 0101）。
**V4 切档**：`封顶 2000000` → 读数跟随；`封顶 1450000` 回落。
**V5 回归**：122 离线态模块值守（无 policy 静默）；`postboot-check` 行为不变；`-e zstd19 -t 2000 -n 1` 回归 pass（脚本改动不触及测试框架，此项确认无意外耦合）。
**V6 文档核对**：配置总账 §4.1/§4.3/§6.6/§8 与板上实态一致。

验证结论全部回写 `docs/research/progress-log.md` 与模块 README。

---

## 10. 仓库落点与提交划分

仓内（`docs/cases/sdc1-01-04-core122/`）：

| 提交 | 内容 | 单元 |
|---|---|---|
| 1 | `docs/superpowers/specs/2026-10-10-cpu122-online-atomic-freqcap-design.md`（本文） | 设计文档 |
| 2 | `cpu122-capfreq/cpu122_capfreq.c` + `Makefile` + `README.md` | 模块 |
| 3 | `cpu122-online-offline.sh` v2（门0/门1/后验/封顶子命令） | 脚本 |
| 4 | `SDC1-01-04-CPU122-BOOT-ISOLATION-CONFIG.md` 增补（§4.1/§4.3/§6.6/§8） | 配置总账 |
| 5 | `docs/research/progress-log.md` 条目 | 台账 |

板上部署与 V1–V6 验证由用户在受测板执行（本机为 firestorm 开发机，无 122 坏核，仅能做语法/静态验证；模块编译验证依赖板上 kernel-devel）。

---

## 附：已排除的替代方案（决策依据）

| 方案 | 排除原因 |
|---|---|
| B：内核源码补丁（cppc_cpufreq.c per-cpu cap） | 效果同 A，但每次 openEuler 内核升级需重打补丁；模块仅重编。团队 9-22 刚经历内核升级，维护成本差异显著 |
| C：纯用户态（会话一次上线 + µs 级 C 双写 + 错峰） | 时刻②③仍裸奔，每次上线是概率赌博；仅作为协议纪律并入本设计（§8） |
| udev cap 规则 | 架构性必输（§1.2 事实一），且实测已死 |
| 借同簇好核 policy 保限值 | per-cpu policy（A/B 实证），无此通路 |
| thermal cooling (cpufreq_cooling) 持久 qos | 注册要求 policy 先存在（首次上线仍竞态），且机制远重于 150 行模块 |
| BIOS/BMC 固件级限频 | 唯一能覆盖时刻①的治本方案，但依赖厂商周期，作为长期项并行推进，不阻塞本设计 |
