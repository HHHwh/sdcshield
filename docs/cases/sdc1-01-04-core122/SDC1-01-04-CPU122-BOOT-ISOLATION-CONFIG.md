# sdc1-01-04 单板 core122 启动隔离与全场景捕获体系——配置总览

> **文档日期**: 2026-10-09（验证于当日 22:15 启动实例，全部输出为本机实测采集）
> **受测单板**: sdc1-01-04（京东云 J353 G3 / BC83AMDA01-7270Z，BIOS 32.83，2×64 核 Kunpeng 920 / TaiShan v120，MIDR `0x480fd020`，128 CPU / 4 NUMA / 无 SMT，openEuler 24.03 SP4，kernel `6.6.0-159.4.13.167.oe2403sp4.aarch64`，IP 172.168.234.165/enp23s0f0）
> **故障核**: 物理核 MPIDR `0x0900060200`（节点 0x09 / cluster 06 / core 2）= 128 核全在场时的**逻辑 CPU122**（NUMA node3、socket 1，与 cpu120/121/123 同簇，core_id 缺口 292 即其位置）
> **本文定位**: 研究前提的配置总账——把"启动期隔离 core122 + 启动后上线 123–127 + core122 永久下线"这一规则所依赖的**全部系统配置**（GRUB、udev、systemd 启动脚本、sysctl、看门狗、kdump、netconsole、SEL/vmcore 归档、实验上下线通道）统一成一份可核查、可复用、可回退的文档。微架构级故障分析见同目录《SDC1-01-04-CPU122-MICROARCH-DIAGNOSIS-REPORT.md》。
> **命名注**: 用户口径中偶称 "sdc1-02-04"；本仓全部证据（本目录名、既有报告标题）均为 **sdc1-01-04**，本文统一按 sdc1-01-04 撰写。
> **变更记录**: 2026-10-09 深夜 vmcore 转储目录由 `/home/vmcore` 迁移至 `/home/sdc/vmcore`（同卷 `mv` 原子改名，**数据原样保留**）；`/etc/kdump.conf`（`path sdc/vmcore`）、vmcore-sync、postboot-check 及 vmcore-project 正本/README/runbook/install-all 同步更新，kdump 已重启并验证 operational。
> **变更记录**: 2026-10-10 新增上线即封顶体系——内核模块 `cpu122-capfreq`（kprobe+freq_qos 双拦截，时刻②③窗口归零）+ 脚本 v2 三门/后验/封顶子命令；设计见 `docs/superpowers/specs/2026-10-10-cpu122-online-atomic-freqcap-design.md`，模块正本 `docs/cases/sdc1-01-04-core122/cpu122-capfreq/`。

---

## 0. 一页结论

**规则**：内核以 `maxcpus=122` 启动（只拉起逻辑 CPU0–121，坏核 122 从不执行一条启动代码）→ systemd `cpu-good-online.service` 在 multi-user 阶段把 CPU123–127 热插拔上线 → **终态 127 核在线、逻辑 CPU122（MPIDR 0x0900060200）永久下线**。实验需要坏核时，走 `cpu122-online-offline.sh` v2 受控通道（模块 `cpu122-capfreq` 上线即封顶 + 门0/门1 预检 + MPIDR/频率后验，2026-10-10 起），重启后自动回到隔离态。

**当前实态（2026-10-09 22:15 启动实例实测）**：

| 项 | 值 |
|---|---|
| `/proc/cmdline` 尾部 | `… console=tty0 maxcpus=122 panic=30` |
| `online` | `0-121,123-127`（127 核） |
| `offline` | `122` |
| `present` / `possible` | `0-127` / `0-127`（128 核全在场，固件未屏蔽） |
| `isolated` / `nohz_full` | 空 / 空（纯热插拔隔离，非调度隔离） |
| 内核日志 | `smp: Brought up 4 nodes, 122 CPUs`、`SMP: Total of 122 processors activated.` |
| 坏核物理 ID | `0x0900060200` 在本次启动 journalctl 中**零出现**（从未引导）；`CPU123: Booted secondary processor 0x0900060300`（22:15:38，由上线服务触发） |
| 上线服务 | `cpu-good-online.service` 本次 22:15:38–39 执行，`status=0/SUCCESS` |

---

## 1. 背景：为什么必须启动隔离

1. **水银核心**：core122（MPIDR 0x0900060200）存在核内数据通路"有效数据错行替换"缺陷——损坏值是本应流经该核的其他合法数据（printk 文本、指令编码、SWAR 常量），ECC 完全无感、RAS 全静默。9-17 三次 128-CPU 启动均在 19~34s 内崩溃（首错核 3/3 落在 CPU122），加电会话内几乎必然无法进入操作系统。证据链见微架构诊断报告 §0–§2。
2. **9-22 夜间事件链**（固件掩码失效后）：21:36 起平台恢复呈现 128 CPU（原始编号回归，logical 122 = 坏核）→ 22:29 坏核在线 @2.9GHz 时五候选稳定性检查**未起跑即整机关机** → 23:07/23:29 两次非正常重启 → 23:40 启动后 23:41 人工 `echo 0 > cpu122/online` 下线坏核 → 稳定。这直接催生了 9-22 深夜的 `maxcpus=122` + `cpu-good-online.service` 配置。
3. **研究需求**：研究 SDC 根因、找稳定复现用例，前提是**操作系统必须能稳定启动并长期存活**（127 好核可用），坏核仅在受控实验窗口按需上线。启动期隔离同时满足两者：坏核不参与启动（绕开"启动暴露峰"崩溃窗口），其余 123–127 启动后补齐。
4. **逻辑编号陷阱（重要）**：逻辑 CPU 号 ↔ 物理核的映射取决于"固件呈现了哪些核"。9-17~9-22 期间固件曾屏蔽坏核（127-CPU 状态，logical 122 = 健康的旧 123 = 0x0900060300）；9-22 21:36 起恢复 128-CPU 原始编号。**`maxcpus=122` 隔离的正是"128 核全在场时的逻辑 122"**——固件状态一旦再变，隔离对象就会漂移。因此全流程有一条铁律：**跨启动引用 CPU 编号必须以 MPIDR 为准**，每次重启后用 `journalctl -k -b 0 | grep -E 'CPU12[23]: Booted'` 核对映射（详见 §6.4）。

---

## 2. 隔离机制：两阶段启动

### 2.1 为什么选 `maxcpus`（而不是 nr_cpus / isolcpus）

| 参数 | 语义 | 对本板是否适用 |
|---|---|---|
| **`maxcpus=122`** ✔ | SMP 初始化只拉起前 122 个逻辑 CPU（0–121）；122–127 起始为 offline 但**保留完整热插拔能力**（`possible` 仍为 0-127，sysfs 设备齐全），可事后逐个上线 | **选用**。坏核 122 从不执行任何代码，123–127 可启动后补齐 |
| `nr_cpus=N` | **硬截断** `possible`：内核只为前 N 个 CPU 分配结构，超出的核连 sysfs `cpuN/online` 都不存在，无法事后上线 | ✘ 弃用。且曾出过事故：09-24 本机曾出现"`/etc/default/grub` 正确但生效层残留 `nr_cpus=12`"（`cpu122-online-offline.sh` 注释记载），故脚本会主动清除 `nr_cpus=` |
| `isolcpus=…` | CPU **仍然在线**、仍执行启动与内核代码，只是被移出调度域 | ✘ 弃用。坏核是"启动即崩"，必须让它彻底不上电运行 |

本机为无 SMT 平台（`Thread(s) per core: 1`），逻辑 CPU 号与物理核一一对应；隔离**不依赖** cpuset/cgroup（`Cpus_allowed_list` 仍为 `0-127`——亲和掩码包含 offline 核的位，调度器只是永远不会往 offline 核上放任务）。

### 2.2 启动时序（2026-10-09 本次启动实测）

```
22:15:02  加电/内核起点（uptime -s）
22:15:19  smp: Bringing up secondary CPUs ...
          smp: Brought up 4 nodes, 122 CPUs          ← maxcpus=122 生效：仅 CPU0-121
          SMP: Total of 122 processors activated.
          （journalctl 共 121 条 "Booted secondary processor"（CPU1-121）；
            CPU0 为引导核，对应 "Booting Linux on physical CPU 0x0300000000"）
          此刻 offline = 122-127，坏核 0x0900060200 零出现
22:15:38  systemd 进入 multi-user 阶段，cpu-good-online.service 启动
          （After=systemd-udevd.service）
22:15:38  CPU123: Booted secondary processor 0x0900060300  ← 热插拔上线 123-127
22:15:39  cpu-good-online.service: Deactivated successfully.（exit 0）
终态      online = 0-121,123-127（127 核）；offline = 122；nproc = 127
```

### 2.3 与隔离并行的第二使命：全场景崩溃捕获（六层体系）

隔离保证"稳"，捕获体系保证"一旦死（实验期坏核上线导致的必然风险）必留证据"。二者同在 9-22/9-23 夜部署，正本在 `/home/sdc/vmcore-project/`（git 管理，`configs/` 为部署源），架构为六层纵深：

| 层 | 机制 | 状态 |
|---|---|---|
| L1 检测触发 | sysctl panic 全家桶（§3.4） | 在值守 |
| L2 转储执行 | kdump → /home/sdc/vmcore（§3.6） | 在值守 |
| L3 兜底复位 | SBSA 看门狗 + systemd `RuntimeWatchdogSec=900`（§3.5） | 在值守 |
| L4 复位取证 | ramoops 保留内存 | **已退役**（本平台热复位清擦 DDR，§3.9） |
| L5 实时直播 | netconsole UDP 6666 → 收集机 0101（§3.7） | 在值守 |
| L6 带外记录 | ipmitool SEL/SDR 每小时快照 + vmcore 异地同步（§3.8） | 在值守 |

部署拓扑：生产机 172.168.234.165 ⇄ 收集机 **0101**（172.168.177.97，同型单板；UDP 6666 收 console 镜像、`/home/vmcore-backup/` 存转储副本、`/home/sel-archive/` 存 SEL 归档）。验证 V1–V10 全 PASS（在 0101 上执行，生产机零破坏），明细见 `vmcore-project/logs/test-summary.md`。

---

## 3. 配置逐项明细

### 3.1 GRUB（启动参数：源头层 + 生效层 + 运行层）

**源头层 `/etc/default/grub`**（mtime 2026-09-22 23:52）——`grub2-mkconfig` 的输入，改这里可持久：

```console
$ grep GRUB_CMDLINE_LINUX /etc/default/grub
GRUB_CMDLINE_LINUX="rd.lvm.lv=openeuler/root rd.lvm.lv=openeuler/swap cgroup_disable=files apparmor=0 crashkernel=1024M,high smmu.bypassdev=0x1000:0x17 smmu.bypassdev=0x1000:0x15 arm64.nopauth nospectre_bhb console=tty0 maxcpus=122 panic=30"
```

参数分工：`maxcpus=122` 是隔离核心参数；`panic=30` 是早期启动阶段（sysctl 生效前）的 panic 自动重启；`crashkernel=1024M,high` 为 kdump 预留（实测 `/sys/kernel/kexec_crash_size` = 1207959552 B = **1152 MiB**，含高位 1024M + 低位补充窗口）；其余为平台常规项（`cpu122-online-offline.sh` 只增删 `maxcpus`/`panic`/`nr_cpus` 三类，不触碰其他参数）。

**生效层 `/boot/efi/EFI/openEuler/grub.cfg`**（`/etc/grub2-efi.cfg` 符号链接指向它）——**三个启动条目（当前内核、旧内核、rescue）全部携带 `maxcpus=122 panic=30`**：

```console
# grep -n maxcpus /boot/efi/EFI/openEuler/grub.cfg   （root）
133:  linux /vmlinuz-6.6.0-159.4.13.167.… root=/dev/mapper/openeuler-root ro … console=tty0 maxcpus=122 panic=30
150:  linux /vmlinuz-6.6.0-159.4.3.154.…  root=/dev/mapper/openeuler-root ro … console=tty0 maxcpus=122 panic=30
166:  linux /vmlinuz-0-rescue-60f5ed…     root=/dev/mapper/openeuler-root ro … console=tty0 maxcpus=122 panic=30
```

选单状态：`GRUB_DEFAULT=saved` + grubenv `saved_entry=openEuler (6.6.0-159.4.13.167.oe2403sp4.aarch64) 24.03 (LTS-SP4)`（`/boot/grub2/grubenv` 与 `/boot/efi/EFI/openEuler/grubenv` 内容一致）。

**注意**：本机 **不使用 BLS**——`/boot/loader/entries/` 为空目录，启动项直接以 menuentry 形式写在 grub.cfg；`grubby` 经 `/etc/grub2-efi.cfg` 操作同一文件。`/etc/kernel/cmdline` 不存在。

**运行层 `/proc/cmdline`**（真实生效证据）：

```console
$ cat /proc/cmdline
BOOT_IMAGE=/vmlinuz-6.6.0-159.4.13.167.oe2403sp4.aarch64 root=/dev/mapper/openeuler-root ro rd.lvm.lv=openeuler/root rd.lvm.lv=openeuler/swap cgroup_disable=files apparmor=0 crashkernel=1024M,high smmu.bypassdev=0x1000:0x17 smmu.bypassdev=0x1000:0x15 arm64.nopauth nospectre_bhb console=tty0 maxcpus=122 panic=30
```

### 3.2 udev（防自动上线：屏蔽出厂 CPU 自动上线规则）

openEuler 出厂 `/usr/lib/udev/rules.d/40-openEuler.rules` 含一条**CPU 自动上线规则**——udevd 启动冷插拔时会把所有 offline 核（含坏核 122）全部拉起，**令 maxcpus 失效**：

```console
$ grep -n 'SUBSYSTEM=="cpu"' /usr/lib/udev/rules.d/40-openEuler.rules
4:#SUBSYSTEM=="cpu", ACTION=="add", TEST=="online", ATTR{online}=="0", ATTR{online}="1"
```

**当前该行已被注释**（第 4 行行首 `#`；文件 mtime 2026-10-09 21:56——本次启动前刚维护过）。配套事实：

- `/etc/udev/rules.d/` 为空（无自定义规则，也无旧方案残留的整文件覆盖）；`/run/udev/rules.d/` 不存在；全部规则目录中 `ATTR{online}` 仅上述已注释行一处命中。
- **维护机制**：`cpu122-online-offline.sh` 的"准备1"每次运行都会动态检索 `/etc` `/run` `/usr/lib` 三个规则目录的全部 `*.rules`，把激活状态的 CPU 自动上线规则**原地注释**（正则覆盖 `SUBSYSTEM` 行首与 `ACTION=="add"` 行首两种写法，幂等），复核无残留后 `udevadm control --reload`。不做 `/etc` 整文件覆盖（避免 udev 升级后快照过时、遮蔽新版出厂规则）。
- **风险**：`/usr/lib` 出厂文件中的注释会随 **udev 包升级被还原**（脚本与本文 §6.2 均提示），升级后必须重跑脚本核查或人工再注释。

### 3.3 systemd 启动脚本（上线 123–127）

`/etc/systemd/system/cpu-good-online.service`（mtime 2026-09-22 23:54，enabled，`multi-user.target.wants/` 内），**完整内容**：

```ini
[Unit]
Description=Online good CPUs 123-127 (cpu122 faulty, stays offline)
After=systemd-udevd.service

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'for c in 123 124 125 126 127; do echo 1 > /sys/devices/system/cpu/cpu$c/online 2>/dev/null || true; done'

[Install]
WantedBy=multi-user.target
```

要点：**白名单式硬编码 123–127**（不是"上线全部 offline 核"——那会把 122 也拉起来）；单个上线失败被 `|| true` 吞掉不影响其余核；排在 `systemd-udevd` 之后。本次启动执行记录：

```console
$ systemctl status cpu-good-online.service --no-pager
○ cpu-good-online.service - Online good CPUs 123-127 (cpu122 faulty, stays offline)
     Loaded: loaded (/etc/systemd/system/cpu-good-online.service; enabled; preset: disabled)
     Active: inactive (dead) since Fri 2026-10-09 22:15:39 CST; 28min ago
    Process: 3144 ExecStart=/bin/sh -c for c in 123 124 125 126 127; … (code=exited, status=0/SUCCESS)
Oct 09 22:15:38 localhost systemd[1]: Starting Online good CPUs 123-127 …
Oct 09 22:15:39 localhost systemd[1]: cpu-good-online.service: Deactivated successfully.
```

### 3.4 sysctl：panic 全家桶（L1 检测触发层）

两个自定义文件（`/etc/sysctl.d/`，按文件名字典序加载，**后者覆盖前者**；`99-sysctl.conf` 为发行版默认项，含 `kernel.dmesg_restrict=1`）：

`/etc/sysctl.d/99-sdc-study.conf`（mtime 2026-09-23 00:00）：

```ini
kernel.softlockup_panic = 1
kernel.hardlockup_panic = 1
kernel.nmi_watchdog     = 1
kernel.panic_on_oops    = 1
kernel.panic_on_warn    = 1
kernel.panic            = 30
```

`/etc/sysctl.d/99-vmcore-capture.conf`（mtime 2026-09-23 01:48，**正本 `~/vmcore-project/configs/99-vmcore-capture.conf`**，修改请改正本）：

```ini
# L1 检测触发层：任何卡死征兆立即 panic（kdump 接管）
kernel.softlockup_panic = 1
kernel.hardlockup_panic = 1
kernel.panic_on_oops = 1
kernel.hung_task_panic = 1
kernel.hung_task_timeout_secs = 60
kernel.panic_on_rcu_stall = 1
kernel.softlockup_all_cpu_backtrace = 1
kernel.hardlockup_all_cpu_backtrace = 1
kernel.panic = 10
kernel.panic_print = 0x7f
kernel.panic_on_warn = 0
kernel.printk = 7 4 1 7
kernel.sysrq = 1
```

**运行时实测值**：`panic=10`、`panic_on_oops=1`、`softlockup_panic=1`、`hardlockup_panic=1`、`hung_task_panic=1`、`panic_print=127(0x7f)`、`panic_on_warn=0`、`nmi_watchdog=1`。语义分工：**早期启动阶段**（systemd-sysctl 生效前）用 grub 的 `panic=30`；**进入系统后** panic 超时为 10s（`99-vmcore-capture` 字典序在后，覆盖 `99-sdc-study` 的 30 与 `panic_on_warn=1`）。设计意图：软/硬死锁、oops、D 状态堆积 60s、RCU stall——任何卡死征兆立即 panic，交给 kdump 转储后 `failure_action reboot` 自愈。

### 3.5 SBSA 看门狗（L3 兜底复位层）

`/etc/systemd/system.conf`：`RuntimeWatchdogSec=900`（PID1 喂狗，15 分钟）。实测 `/sys/class/watchdog/watchdog0` = **SBSA Generic Watchdog，state=active**。平台实测结论（vmcore-project）：`ipmi_watchdog` 与 `sbsa_gwdt` misc 设备互斥，方案固定 SBSA；systemd v255 不认 `RuntimeWatchdogDevice` 键，用默认设备即可；转储 37s 完成、900s 武装下不被狗截断（V9）。

### 3.6 kdump（L2 转储执行层）

- **预留**：cmdline `crashkernel=1024M,high`，实测 `kexec_crash_size` = 1152 MiB。
- **`/etc/sysconfig/kdump`**（有效项）：

```ini
KDUMP_COMMANDLINE_REMOVE="hugepages hugepagesz slub_debug quiet"
KDUMP_COMMANDLINE_APPEND="irqpoll nr_cpus=1 reset_devices cgroup_disable=memory udev.children-max=2 panic=10 swiotlb=noforce novmcoredd numa=off console=ttyAMA0"
```

  capture 内核 `nr_cpus=1`（单核转储，天然不碰坏核）。
- **`/etc/kdump.conf`**（有效项）：`ext4 /dev/mapper/openeuler-home` + `path sdc/vmcore`（转储落 `/home/sdc/vmcore`，home 卷 910G；根分区可用 52G < 未压缩估算 64G 故弃用；**2026-10-09 由 `/home/vmcore` 迁入**）；`core_collector "makedumpfile -l --message-level 1 -d 31"`；`failure_action reboot`（转储失败也重启自愈）。
- **状态**：`kdump.service` enabled+active，`/sys/kernel/kexec_crash_loaded` = 1；kdump initramfs 本次启动自动重建（`initramfs-…kdump.img` mtime 2026-10-09 22:16）。
- **`kdump-early.service` 不存在于本系统**（`systemctl is-enabled` → not-found；root 历史中曾尝试启用，openEuler 24.03 未提供该单元）——initramfs 阶段崩溃由 netconsole（L5）兜底。
- **实战记录**：`/home/sdc/vmcore/` 下已有三次真实捕获——`127.0.0.1-2026-09-29-11:48:53/`、`127.0.0.1-2026-09-30-08:32:11/`、`127.0.0.1-2026-10-09-11:17:53/`（各含 `vmcore` + `vmcore-dmesg.txt`），分别对应 9-29/9-30 movbe-c-122 实验与 10-09 全核批次 TEST #13 窗口的崩溃形态死亡（#12 于 11:11:21 结束，11:17:53 触发 `Internal error: Oops: 0000000096000004` 后 kdump 正常接管）。

### 3.7 netconsole（L5 实时直播层）

- **`/etc/modprobe.d/netconsole.conf`**（mtime 2026-09-23 00:54）：

```ini
options netconsole netconsole=6666@172.168.234.165/enp23s0f0,6666@172.168.177.97/b0:4f:a6:57:75:ba
```

  本机（172.168.234.165/enp23s0f0）内核日志经 UDP **6666** 实时流往收集机 0101（172.168.177.97，MAC b0:4f:a6:57:75:ba），0101 侧加时间戳落 `/var/log/host-console.log`。
- **`netconsole.service`**：oneshot `modprobe netconsole`，`Wants/After=network-online.target`，enabled+active（lsmod 确认模块已加载）。panic 输出秒级抵达收集机（V8 实测 2281+ 行）。
- **本地接收端（休眠保留）**：`console-recv2.socket`（UDP **6667**，sockets.target.wants，active 监听）+ socket 激活的 `console-recv2.service`（`/usr/local/bin/console-recv.py`，数据报到才运行）→ `/var/log/testm-console.log`。其角色是 V1–V10 验证期 0101 console 的反向镜像留证；验证结束后 0101 已回退为纯收集机，**该日志自 2026-09-24 21:10 起不再增长**，接收端仅保持监听（无害）。

### 3.8 SEL 快照与 vmcore 异地同步（L6 + 同步层，timer 驱动）

| 单元（均 enabled） | 触发 | 动作（脚本） |
|---|---|---|
| `sel-collect.timer` → `.service` | `OnCalendar=hourly` + `OnBootSec=5min` + Persistent | `/usr/local/bin/sel-collect.sh`：`ipmitool sel elist` + `sdr elist` → `/var/log/sel/sel-<ts>.eline`、`sdr-<ts>.txt`（30 天留存）→ scp 至 `root@172.168.177.97:/home/sel-archive/` |
| `vmcore-sync.timer` → `.service` | `OnCalendar=daily` + `OnBootSec=30min` + Persistent | `/usr/local/bin/vmcore-sync.sh`：`flock /run/vmcore-sync.lock` 后 `rsync -a /home/sdc/vmcore/ → root@172.168.177.97:/home/vmcore-backup/<host>/`（幂等） |
| `pstore-archive.service` | multi-user 一次性 | `/usr/local/bin/pstore-archive.sh`：挂载 pstore → 归档至 `/var/lib/systemd-pstore/<ts>` → rsync 至 0101（**L4 已退役，见 §3.9，属留档代码**） |

本次启动实证：SEL 快照 22:20:06 落盘（OnBootSec=5min ✓），vmcore-sync 22:45:24 执行（OnBootSec=30min ✓）。SEL 的研究价值：坏核 SDC 属"有效数据错行替换"，**BMC 侧 RAS 全静默**——SEL 快照正是"零纠正错误"这一负证据的定期取证。

### 3.9 pstore/ramoops（L4，已退役，留档说明）

本平台**热复位清擦 DDR**（干净重启亦丢数据），ramoops 类保留内存取证不可用，2026-09-23 退役；复位取证职责由 L5 镜像 + L6 SEL 承担。`pstore-archive.service`/`postboot-check.sh` 中对应段落保留只为结构完整（postboot-check 输出 `INFO L4 ramoops retired on this platform (warm reset scrubs RAM)`）。复位归因（无 vmcore 场景）用三角推断：意外重启 + 无 vmcore + 镜像静默 → 看门狗/人工/掉电，详见 `vmcore-project/docs/runbook.md`。

### 3.10 六层自检：`/usr/local/bin/postboot-check.sh`（手动）

正本 `~/vmcore-project/configs/postboot-check.sh`（mtime 2026-09-23 03:18）。输出 PASS/FAIL/PENDING 三态，任一 FAIL 退出码 1；**逐层核对**：L1 六个 panic sysctl 非零 → L2 kdump active + `kexec_crash_loaded=1` → L4 退役说明 → L5 netconsole active → L3 watchdog0 active → L6 两个 timer active，**外加硬约束巡检**：

```bash
[ "$(cat /sys/devices/system/cpu/cpu122/online 2>/dev/null)" = 0 ] \
  && P 'cpu122 offline (constraint)' || F 'cpu122 ONLINE - VIOLATION'
```

另提示 `/home/sdc/vmcore`、`/var/lib/systemd/pstore` 有无待分析产出物；结果 tee 到 `/tmp/vmcore-postboot-<ts>.txt`（tmpfs，重启即失，仅作当场留证）。**未发现任何自动化触发**（无 systemd 单元/cron 引用）——属人工巡检工具，建议每次开机后手动执行一次。

### 3.11 排查过的"不存在"项（负面结论，均为实测）

| 项 | 结论 |
|---|---|
| `/etc/udev/rules.d/` | 空（无自定义 udev 规则；隔离不依赖 /etc 侧规则） |
| BLS 条目 `/boot/loader/entries/` | 空目录；`/etc/kernel/cmdline` 不存在——参数全部在 grub.cfg menuentry |
| `isolcpus` / `nohz_full` / `nr_cpus` | cmdline 中均无；`/sys/devices/system/cpu/isolated`、`nohz_full` 均为空 |
| `/etc/rc.local` | 原样模板（仅 `touch /var/lock/subsys/local`），无 CPU 逻辑 |
| root crontab / `/etc/cron.d/` | root 无 crontab；cron.d 仅发行版条目（0hourly/dailyjobs/mdcheck/timezone.cron）——**周期任务全部走 systemd timer** |
| `kdump-early.service` | not-found（不存在） |
| `systemtap.service` | enabled 但 `/etc/systemtap/script.d/` 为空——不跑任何脚本（预留） |
| tuned | `throughput-performance`（发行版档位，不改 CPU 在线状态） |
| 自动守卫 | **不存在**任何守护进程监控/纠正 cpu122 意外上线（postboot-check 只检测不纠正） |

---

## 4. 实验运行规则（研究前提的受控使用方式）

### 4.1 `cpu122-online-offline.sh`——坏核上下线的唯一正门

`/home/sdc/cpu122-online-offline.sh`，用法 `sudo ./cpu122-online-offline.sh {上线|on|1|下线|off|0}` 或 `sudo ./cpu122-online-offline.sh {封顶|cap} <kHz>`（参数强制显式指定）。**每次执行前自动完成三项幂等准备**：

1. **准备1（udev 防线）**：检索并注释全部规则目录中激活状态的 CPU 自动上线规则（§3.2），复核无残留后 `udevadm control --reload`；检测旧方案残留（`/etc/udev/rules.d/40-openEuler.rules` 整文件覆盖）并告警。
2. **准备2（GRUB 防线）**：确保 `/etc/default/grub` 的 `GRUB_CMDLINE_LINUX` 含 `maxcpus=122 panic=30`——先清除历史残留的 `maxcpus=`/`panic=`/`nr_cpus=`（nr_cpus 硬截断 possible，会破坏"128 核可见"设计；09-24 曾发生生效层残留 `nr_cpus=12` 的事故）再追加；仅在改动过时 `grub2-mkconfig -o /boot/efi/EFI/openEuler/grub.cfg`；随后用 `grubby --info=ALL` 逐条目核对生效层，不一致则 `grubby --update-kernel=ALL --remove-args="nr_cpus" --args="maxcpus=122 panic=30"`；最后对 `/proc/cmdline` 做一致性提示（运行中内核需重启才生效）。
3. **准备3（补齐好核）**：上线除 122 外的全部 offline 核（0..最大核号，逐个 `echo 1`，失败即中止）。

**主功能**：`echo <0|1> > /sys/devices/system/cpu/cpu122/online` 并回读验证（v1 曾在收尾提醒手动降额封顶——v2 起封顶由模块在上线路径内自动完成，该提示已废弃；重启后回离线的语义不变）。

**v2 上线即封顶（2026-10-10 起，模块 `cpu122-capfreq`）**：坏核满频致死竞态
（上线后 ~10ms 即死，uevent 未出、用户态封顶架构性迟到）由内核模块归零——
kprobe `cppc_set_perf` 钳位（驱动 init 的 2.9G slam 在写硬件前被改写）+
freq_qos MAX 挂 policy（跨热插拔存活，governor 稳态封顶）。脚本 v2 流程：
门0（模块加载 + cap 边界：0 拒绝、>2.9G 拒绝、>2.0G 高危警告）→ 门1
（present=0-127、offline=122、cpu121-123 同簇序——编号漂移即拒）→ 上线
（封顶在上线路径内完成）→ 后验1（journalctl MPIDR==0x0900060200，不符自动
下线）→ 后验2（scaling_max_freq/cur ≤ cap，qos 疑失效自动下线）。**切档
免上下线**：`封顶 2000000` 在线生效（qos 自动传导），每加电会话只需一次
上线。残余：时刻①（PSCI 热启动早期段，固件域）不可归零，靠会话一次上线
纪律 + 错峰缓解；长期治本走 BIOS/BMC per-core 限频。

### 4.2 狩猎/批次脚本的安全预检（实验侧的第二道闸）

实验脚本（`/home/sdc/root-xupeng/sdcshield/scripts/run/`，代表为 `run-sdc-hunt-30min-all128.sh` 狩猎 v3 与 `run-freq2000-allcore-batch-20261009.sh`）在跑任何负载前强制预检，**任一不过即拒跑**：

- **MPIDR 核对**：`journalctl -k -b 0 | grep "CPU122: Booted secondary processor"` 必须命中 `0x0900060200`——编号漂移即中止（任务规则：跨启动以 MPIDR 为准）；
- **降频封顶**：`cpu122/cpufreq/scaling_max_freq` ≤ 容差（1.45GHz 档容差 1500000；2.0GHz 档要求恰为 2000000）——**每个测试前复查，封顶丢失即中止**（2.9GHz 满频负载曾两度带崩整机）；
- **核态**：122 在线（狩猎就是冲它来的）+ 全核 0–127 在线（或 `nproc`=128）；
- 其他：二进制/清单存在、`/home` 磁盘余量、断点续跑、死亡嫌疑隔离（STATUS.txt 的 START 无 END = 机器死亡窗口，该测试进 `death-suspects.txt` 人工裁决）；
- **死亡容忍设计**：STATUS/summary 全部 `sync`（fsync）即时落盘，机器若崩已完成数据不丢；START-END 标记即死亡取证。

### 4.3 降频封顶协议与频率依赖结论

- 封顶机制：本板 cpufreq 驱动为 **cppc_cpufreq**（每核独立 policy，`performance` governor，可用连续范围 400000–2900000 kHz）——注意仓库 CLAUDE.md 的"No cpufreq"平台注记描述的是另一台 192-CPU 单板，**本板 cpufreq 可用**。
- **cpu122 离线时其 cpufreq 目录不存在**——封顶只在在线期间存在，**每次上线后必须重新封顶**（狩猎脚本预检正是为此；v2 起由模块 `cpu122-capfreq` 在上线路径内自动完成）。
- 频率依赖 A/B（2026-09-24，同二进制同种子同核）：2.9GHz 满频单核 `zstd19` 60s 两连崩（SIGSEGV，<4s）；1.45GHz 同种子精确重放 60s 184/184 迭代全 pass，整机存活——**坏核故障是频率依赖的**（时序/电压裕量劣化形态）。此后狩猎 v3 以 122@1.45GHz + 其余 127 核@2.9GHz 全核跑完 116 项×30min（67.6h）；10-08 起频率研究改 2.0GHz 档（40 项×15min 全净），10-09 按用户裁定改为全核心复跑。台账见 `docs_xu/2026-10-08-cpu122-frequency-sdc-study.md`。
- **封顶机制 v2（2026-10-10 起）**：模块 `cpu122-capfreq` 在 CPU 上线路径内完成"首次 CPPC 写入即封顶 + governor 稳态封顶"，`scaling_max_freq` 的用户态补写不再是安全边界（降级为后验读数）；封顶跨 offline/online 自动保持，1.45G↔2.0G 切档走 `cpu122-online-offline.sh 封顶 <kHz>`，免上下线。

### 4.4 机器死亡后的恢复流程（狩猎脚本头注释原文规则）

```
⚠️ 机器死亡后恢复流程（grub 带 maxcpus=122，重启后 122-127 默认 offline）：
    root 逐个上线：for c in $(seq 122 127); do echo 1 > /sys/devices/system/cpu$c/online; done
    root 重新封顶：echo 1450000 > /sys/devices/system/cpu/cpu122/cpufreq/scaling_max_freq
    然后重新运行脚本——已完成测试自动跳过；死亡窗口测试已在隔离名单，需人工决定是否重跑。
```

（注：该流程含 122——因为狩猎实验需要它在线；不跑实验的普通恢复只需 123–127，`cpu-good-online.service` 已自动完成。取证顺序：0101 的 host-console.log → `/home/sdc/vmcore/` 最新 vmcore-dmesg → crash 深析 → SEL。）

### 4.5 默认态与实验态对比

| | 默认态（每次重启后） | 实验态（受控窗口内） |
|---|---|---|
| cpu122 | **offline**（maxcpus=122 保证） | online（`cpu122-online-offline.sh 上线`） |
| 123–127 | online（cpu-good-online.service） | online |
| 122 频率 | —（离线无 cpufreq） | **封顶**（1.45GHz / 2.0GHz 档，performance） |
| 122 MPIDR 核对 | `0x0900060200` 零出现于启动日志 | 上线后 journalctl 核对 `CPU122: Booted … 0x0900060200` |
| 风险等级 | 坏核不上电，整机稳 | 随时可能整机死亡/崩溃——按"死亡容忍"设计实验（fsync 台账、kdump、netconsole 就位） |

---

## 5. 生效验证手册（命令 + 本机真实输出）

以下命令除标注 root 外均可直接以 sdc 用户执行；输出均采集自 2026-10-09 22:15 启动实例。

**① 启动参数三层核对**：

```console
$ cat /proc/cmdline
BOOT_IMAGE=/vmlinuz-6.6.0-159.4.13.167.oe2403sp4.aarch64 root=/dev/mapper/openeuler-root ro … console=tty0 maxcpus=122 panic=30

$ grep maxcpus /etc/default/grub
GRUB_CMDLINE_LINUX="… console=tty0 maxcpus=122 panic=30"

# root：
# grep -n maxcpus /boot/efi/EFI/openEuler/grub.cfg     ← 应 3 行（当前/旧/rescue 条目）全含 maxcpus=122
# cat /boot/efi/EFI/openEuler/grubenv                   ← saved_entry 指向预期内核
```

**② CPU 状态**：

```console
$ for f in online offline present possible; do echo "$f = $(cat /sys/devices/system/cpu/$f)"; done
online = 0-121,123-127
offline = 122
present = 0-127
possible = 0-127
$ cat /sys/devices/system/cpu/cpu122/online    # ← 必须为 0
0
$ nproc
127
```

**③ 内核日志（SMP 数量 + MPIDR 核对，最硬的证据）**：

```console
$ journalctl -b -k --no-pager | grep -E 'smp: Brought up|SMP: Total'
Oct 09 22:15:19 localhost kernel: smp: Brought up 4 nodes, 122 CPUs
Oct 09 22:15:19 localhost kernel: SMP: Total of 122 processors activated.
$ journalctl -b -k --no-pager | grep -c 'Booted secondary processor'
126                                    # = 121（启动期 CPU1-121）+ 5（服务上线 123-127）
$ journalctl -b -k --no-pager | grep '0x0900060200'
（空输出 = 坏核物理 ID 从未引导，隔离生效）
$ journalctl -b -k --no-pager | grep -E 'CPU12[23]: Booted'
… CPU120: Booted secondary processor 0x0900060000 …   22:15:19
… CPU121: Booted secondary processor 0x0900060100 …   22:15:19
… CPU123: Booted secondary processor 0x0900060300 …   22:15:38（← cpu-good-online 触发）
```

**④ 上线服务**：`systemctl status cpu-good-online.service`（见 §3.3，本次 22:15:38–39 exit 0）。

**⑤ 六层自检**：`sudo /usr/local/bin/postboot-check.sh` —— 全 PASS 即健康（含 `cpu122 offline (constraint)` 一行；若出现 `cpu122 ONLINE - VIOLATION` 立即处置）。

**⑥ udev 防线核查（root 或直接 grep，世界可读）**：

```console
$ grep -rn 'ATTR{online}' /usr/lib/udev/rules.d/ /run/udev/rules.d/ /etc/udev/rules.d/ 2>/dev/null
/usr/lib/udev/rules.d/40-openEuler.rules:4:#SUBSYSTEM=="cpu", … ATTR{online}="1"
# ↑ 必须且仅此一行，且行首带 #（已注释）。若出现未注释的激活行 = maxcpus 防线被击穿，立即按 §6.2 处置。
```

**⑦ 捕获层速查**：`systemctl is-active kdump netconsole`、`cat /sys/kernel/kexec_crash_loaded`（=1）、`cat /sys/class/watchdog/watchdog0/state`（=active）、`systemctl list-timers sel-collect.timer vmcore-sync.timer`、`ls -lt /home/sdc/vmcore/`（有无新转储）。

---

## 6. 风险、红线与运维注意

### 6.1 红线（承 vmcore-project §7，实验例外见 6.6）

1. **生产机绝不主动触发崩溃/复位**：`echo c/b/o > /proc/sysrq-trigger`、`insmod lockup-test.ko`、`kexec -e`、`reboot` 仅限用户明示授权。
2. **core122 保持下线**：启动参数 `maxcpus=122` 原样保留，默认禁写 `online=1`。
3. **凭据不落盘**：收集机 0101 的 root 密码曾在对话中暴露过，应尽快轮换；本文不含任何口令。

### 6.2 udev 包升级会还原出厂规则（最高频风险）

`/usr/lib/udev/rules.d/40-openEuler.rules` 属 udev RPM——**包升级会把已注释的 CPU 自动上线规则还原为激活**，udevd 冷插拔随即拉起全部 offline 核（含 122），maxcpus 防线失效。处置：每次 udev 升级（`dnf update` 后）重跑 `cpu122-online-offline.sh`（准备1 自动复核）或人工注释 + `udevadm control --reload`；本文件 mtime 2026-10-09 21:56 说明该防线需要周期性人工维护（本次启动前刚维护过）。

### 6.3 内核升级后的复核

`/etc/default/grub` 是持久源头，但**新安装内核生成的新启动条目必须复核**（新条目参数由 grubby/kernel-install 生成，历史上有"源头正确、生效层残留旧值"的事故——09-24 的 `nr_cpus=12` 残留）。升级后执行 §5①（三层核对），不一致时用 `grubby --update-kernel=ALL --args="maxcpus=122 panic=30" --remove-args="nr_cpus"` 一键同步（`cpu122-online-offline.sh` 准备2 亦自动完成）。

### 6.4 逻辑编号漂移（架构性风险）

`maxcpus=122` 隔离的是"**128 核全在场时的逻辑 122**"。固件若再次屏蔽坏核（如 9-17 的 127-CPU 状态），编号整体前移、logical 122 变成健康核——此时隔离错对象（虽不致崩，但白丢一个好核且实验目标丢失）；固件状态切换（BIOS/BMC 维护、意外失效）都可能触发。**铁律：跨启动以 MPIDR 为准**，每次重启后 `journalctl -k -b 0 | grep -E 'CPU12[23]: Booted'` 核对 `122 ↔ 0x0900060200` 映射（狩猎脚本已内置该预检）。

### 6.5 `cpu-good-online.service` 硬编码 123–127

核数变化（固件屏蔽/BIOS 配置改动 → possible 变化）时该服务不适配：多出的核不会上线（`2>/dev/null || true` 静默吞掉不存在的核），需人工修改服务内核号清单。这是**白名单设计的代价**（换取绝不误上线 122 的确定性）。

### 6.6 意外上线的后果与恢复

无自动守卫：122 被误上线（udev 规则还原、误操作、异常热插拔事件）后**不会被自动纠正**，且离线前其 cpufreq 封顶不存在（新上线默认满频 2.9GHz——最危险状态）。恢复：`echo 0 > /sys/devices/system/cpu/cpu122/online` 或直接重启（maxcpus 保证回到隔离态）。若实验授权上线，必须同步完成封顶（§4.3）——这正是 vmcore-project 红线 #2 的实验例外通道：**用户明示授权 + 降频封顶 + MPIDR 核对 + 死亡容忍设计**。

2026-10-10 起：模块 `cpu122-capfreq` 使"意外上线后满频裸奔"窗口从用户态封顶链路（50–500ms，实测致死）缩小到 PSCI 热启动早期段（固件域，历史实证低风险）；若模块被 `封顶 0` 显式解锁或卸载，恢复裸奔态——脚本门0 会拒绝 cap=0 态的上线，但已在线期间的风险自担。

### 6.7 其他注意

- panic 超时是**两阶段**语义：早期启动 30s（grub）、进系统后 10s（sysctl）——解读"崩溃后多久自愈"时注意区分。
- `kdump-early` 不存在：initramfs 阶段（kdump 尚未 armed）的崩溃只有 netconsole（L5）+ BMC SOL 带外日志可查（诊断报告的取证来源即 BMC SOL）。
- SEL 时间戳走 BMC 时钟，与 OS 时间可能偏差十余分钟，跨源比对注意。
- 周期性维护项清单：udev 升级（§6.2）、内核升级（§6.3）、重启后 MPIDR 核对（§6.4）、开机跑一次 postboot-check（§3.10）。

---

## 7. 配置时间线（溯源，依据文件 mtime 与 root/sdc shell 历史）

| 时间 | 事件 |
|---|---|
| 2026-09-17 | 三次 128-CPU 启动崩溃（首错核均 CPU122）；随后固件自动屏蔽坏核 → 127-CPU 干净启动（Boot D） |
| 2026-09-18 | 微架构诊断报告成文；内核升级窗口准备 |
| 2026-09-22 20:46–21:36 | 内核升级（159.4.3.154 → 159.4.13.167）；固件掩码失效，128 CPU 原始编号回归 |
| 2026-09-22 22:29–23:40 | 坏核在线 @2.9GHz：五候选检查未起跑即关机、两次非正常重启；23:41 人工下线坏核后稳定 |
| 2026-09-22 23:52–23:54 | **`/etc/default/grub` 写入 `maxcpus=122 panic=30` + `grub2-mkconfig`；创建并启用 `cpu-good-online.service`**（root 历史实证） |
| 2026-09-23 00:00–03:18 | vmcore-project 六层捕获体系部署（V1–V10 在 0101 验证全 PASS）：`99-sdc-study.conf`、`netconsole.conf`、`99-vmcore-capture.conf`、sel/vmcore/pstore 单元与脚本、`postboot-check.sh`；sdc 历史实证 `echo 1450000 | sudo tee …/scaling_max_freq` 降额与 `sudo vim 40-openEuler.rules` 注释 |
| 2026-09-24 | 12:06 起 maxcpus=122 + 122@1.45GHz 启动稳定；频率依赖 A/B 确立；16:08 狩猎 v3（116×30min 全核）开跑（→09-27，67.6h） |
| 2026-09-29 / 09-30 / 10-09 | movbe-c-122 与频率研究实验三次崩溃形态死亡 → 三次真实 kdump 捕获（现均存于 `/home/sdc/vmcore/`） |
| 2026-10-08 → 10-09 | 频率研究 @2.0GHz（单核 40 项全净 → 10-09 按用户裁定改全核心复跑） |
| 2026-10-09 21:56 | `40-openEuler.rules` CPU 自动上线规则（再）注释——防线临启维护 |
| 2026-10-09 22:15 | 本次启动：122 核激活 → 123–127 上线 → 终态 127 核在线、122 离线（§0 快照） |

---

## 8. 文件—职责速查表

| 路径 | 角色 | 层 |
|---|---|---|
| `/etc/default/grub` | 启动参数**源头**（`maxcpus=122 panic=30`） | 隔离 |
| `/boot/efi/EFI/openEuler/grub.cfg`（←`/etc/grub2-efi.cfg`） | 启动参数**生效层**（3 条目全覆盖） | 隔离 |
| `/boot/{grub2,efi/EFI/openEuler}/grubenv` | `saved_entry`（默认启动项） | 隔离 |
| `/usr/lib/udev/rules.d/40-openEuler.rules:4` | 出厂 CPU 自动上线规则（**已注释**，防 maxcpus 失效） | 隔离 |
| `/etc/systemd/system/cpu-good-online.service` | 启动后上线 CPU123–127（白名单） | 隔离 |
| `/etc/sysctl.d/99-sdc-study.conf`、`99-vmcore-capture.conf` | panic 全家桶（卡死→panic） | L1 |
| `/etc/sysconfig/kdump`、`/etc/kdump.conf` | 转储内核参数与落盘策略（→`/home/sdc/vmcore`） | L2 |
| `/etc/systemd/system.conf`（`RuntimeWatchdogSec=900`） | SBSA 看门狗喂狗周期 | L3 |
| `/etc/modprobe.d/netconsole.conf`、`/etc/systemd/system/netconsole.service` | 内核日志实时流向 0101 | L5 |
| `console-recv2.{socket,service}`、`/usr/local/bin/console-recv.py` | 本地 UDP 6667 接收端（休眠留档） | L5 |
| `sel-collect.{service,timer}`、`/usr/local/bin/sel-collect.sh` | SEL/SDR 每小时快照 + 异地归档 | L6 |
| `vmcore-sync.{service,timer}`、`/usr/local/bin/vmcore-sync.sh` | vmcore 每日/开机后同步 0101 | 同步 |
| `pstore-archive.service`、`/usr/local/bin/pstore-archive.sh` | pstore 归档（L4 退役留档） | — |
| `/usr/local/bin/postboot-check.sh` | 六层自检 + cpu122 离线硬约束（**手动**） | 巡检 |
| `/home/sdc/cpu122-online-offline.sh` | 坏核受控上下线正门（三准备） | 实验 |
| `/home/sdc/vmcore-project/`（`configs/` 为正本，git 管理） | 捕获体系部署源 + 设计/runbook/测试报告 | 全局 |
| `/home/sdc/root-xupeng/sdcshield/scripts/run/*.sh` | 狩猎/频率批次脚本（MPIDR/封顶预检） | 实验 |
| `/home/sdc/vmcore/` | kdump 转储落点（2026-10-09 由 `/home/vmcore` 迁入；已有 09-29、09-30、10-09 三次实战捕获） | L2 |
| `docs/cases/sdc1-01-04-core122/cpu122-capfreq/`（板上 `/home/sdc/cpu122-capfreq/`） | 上线即封顶内核模块正本（c/Makefile/README/deploy/tests） | 实验 |
| `/etc/modules-load.d/cpu122-capfreq.conf` + `/etc/modprobe.d/cpu122-capfreq.conf` | 模块开机自动加载与默认参数（target_cpu=122 cap_khz=1450000） | 实验 |
| `docs/superpowers/specs/2026-10-10-cpu122-online-atomic-freqcap-design.md` | 上线即封顶设计（三时刻/双拦截/验证 V1–V6/已排除方案） | 实验 |

---
