# CPU122 上线即封顶（原子降频）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付 `cpu122-capfreq` 内核模块 + `cpu122-online-offline.sh` v2，使 CPU122 上线时频率封顶窗口（时刻②③）归零，模块未就绪时脚本拒绝上线。

**Architecture:** 模块双拦截——kprobe `cppc_set_perf` pre-handler 钳位 desired_perf（覆盖驱动 init 的 2.9G slam）+ FREQ_QOS_MAX 请求挂 policy->constraints（跨热插拔存活，governor 稳态封顶）。脚本 v2 加门0（模块就绪）/门1（编号漂移预检）/后验（MPIDR+频率读数，不符自动下线）+ `封顶` 子命令（在线切档，免上下线）。

**Tech Stack:** Linux 内核模块（GPL 导出符号：kprobes/freq_qos/cppc/ACPI CPPC，已在 openEuler 6.6.0-159.4.x `Module.symvers` 逐一确认）、Bash、fake-sysfs 行为测试。

**Spec:** `docs/superpowers/specs/2026-10-10-cpu122-online-atomic-freqcap-design.md`（本计划从 spec 出发，执行者须同时读两者）

## Global Constraints

- 全程中文；仓内文档中文；**内核 printk 消息一律 ASCII**（netconsole/0101 工具链兼容，前缀 `cpu122-capfreq: ` 可 grep）。
- DCO：`git commit -s`，最后一行 `Signed-off-by: wangxu <wangxumarshall@qq.com>`，其后无任何内容、**无 Co-Authored-By**。
- 分支 `feat/cpu122-online-atomic-freqcap`（已建好并已推送 spec 提交 `dcae1ce3`）；每任务一提交，验证通过后 push，绝不 push main。
- 验证 100% 真实：模块本机验证 = 零警告构建（本机 firestorm 无 sudo、无坏核、无 cpufreq，运行时验证属板上 V1–V6，用户执行）；脚本验证 = fake-sysfs 行为测试 + `bash -n`。禁止宣称"应该能跑"。
- 本工作只触碰 `docs/cases/sdc1-01-04-core122/`、`docs/superpowers/`、`docs/research/progress-log.md`——零接触测试框架/meson/x86。
- 模块仅 aarch64（编译期 `#error` 守卫）；板上内核 `6.6.0-159.4.13.167`，本机构建验证用同系列 `6.6.0-159.4.3.154` kernel-devel（`/lib/modules/$(uname -r)/build` 存在，已确认）。
- 构建产物（*.o/*.ko/*.mod 等）不得入库：模块目录带 `.gitignore`。

## Review Focus

1. **cap_khz=0（模块停用态）后直接上线 = 裸奔** → 门0 必须拒绝（T3.2 钉死）；`封顶 0` 是显式解锁通道但必须高危警告（T2.5e）。
2. **固件重屏蔽 → 逻辑编号漂移后照常上线** → 门1 present/offline/拓扑三重预检（T3.4/T3.5/T3.6）+ 后验1 MPIDR 不符自动下线（T3.8/T3.10）。
3. **模块失载（内核升级忘重编、加载失败）** → `/sys/module/cpu122_capfreq` 缺失 → 门0 拒绝（T3.1）；板上 conf 自动加载是前提（README SOP 钉死）。
4. **qos 静默失效**（`freq_qos_add_request` 失败，CREATE_POLICY 返回值被 core 忽略）→ 模块 dmesg ERROR 留痕 + 脚本后验2 读数兜底并自动下线（T3.9）。
5. **CREATE_POLICY 回调内挂 qos 的死锁疑虑**（回调持 policy->rwsem）→ 已核实 v6.6 `cpufreq_notifier_max` 为 `schedule_work` 异步（`/tmp/k66/cpufreq.c`，计划撰写时源码核实）；模块代码注释中保留该依据，板上 V1 实证。

---

### Task 1: 模块 cpu122-capfreq 完整交付

**Files:**
- Create: `docs/cases/sdc1-01-04-core122/cpu122-capfreq/cpu122_capfreq.c`
- Create: `docs/cases/sdc1-01-04-core122/cpu122-capfreq/Makefile`
- Create: `docs/cases/sdc1-01-04-core122/cpu122-capfreq/README.md`
- Create: `docs/cases/sdc1-01-04-core122/cpu122-capfreq/deploy/modules-load.d-cpu122-capfreq.conf`
- Create: `docs/cases/sdc1-01-04-core122/cpu122-capfreq/deploy/modprobe.d-cpu122-capfreq.conf`
- Create: `docs/cases/sdc1-01-04-core122/cpu122-capfreq/.gitignore`

**Interfaces:**
- Consumes: 内核导出符号 `cppc_set_perf`/`cppc_get_perf_caps`/`cppc_khz_to_perf`（`<acpi/cppc_acpi.h>`）、`freq_qos_add_request`/`freq_qos_update_request`/`freq_qos_remove_request`（`<linux/pm_qos.h>`）、`register_kprobe`/`unregister_kprobe`（`<linux/kprobes.h>`）、`cpufreq_register_notifier`/`cpufreq_cpu_get`/`cpufreq_cpu_put`（`<linux/cpufreq.h>`）、`CPUFREQ_POLICY_NOTIFIER`/`CPUFREQ_CREATE_POLICY` 常量。
- Produces（Task 2/3 脚本依赖的稳定接口）: 模块名 `cpu122_capfreq`；参数 `/sys/module/cpu122_capfreq/parameters/cap_khz`（0644 可写，kHz，0=停用）与 `target_cpu`（0444）；dmesg 前缀 `cpu122-capfreq: `。

- [ ] **Step 1: 写 `cpu122_capfreq.c`（完整代码如下，一字不落的实现）**

```c
// SPDX-License-Identifier: GPL-2.0
/*
 * cpu122_capfreq — 坏核（sdc1-01-04 逻辑 CPU122）上线即封顶
 *
 * 消除 online → userspace cap 竞态。根因（v6.6 源码级，见设计文档）：
 *  1) KOBJ_ONLINE uevent 在 write() 返回前才广播，用户态封顶必然迟到；
 *  2) cppc_cpufreq_cpu_init() 在 CPU 上线路径内（CPUHP_AP_ONLINE_DYN，由
 *     CPU122 自身执行，早于 governor/notifier）直接 cppc_set_perf(desired=
 *     highest_perf = 2.9GHz boost) —— 满频是内核亲手写上去的；
 *  3) 用户态无跨 offline/online 限值持久化通道。
 *
 * 双拦截：
 *  机制一（时刻③ governor 稳态）: FREQ_QOS_MAX 请求挂 target_cpu 的
 *    policy->constraints；cpufreq_set_policy() 以 freq_qos 聚合值为权威
 *    policy->max；policy 结构跨热插拔存活 → 一次挂载，后续上线自动生效。
 *    qos 变更经 policy 自带 notifier 触发 policy->update work —— 该 work
 *    为 schedule_work 异步（v6.6 drivers/cpufreq/cpufreq.c
 *    cpufreq_notifier_max），故在 CREATE_POLICY 回调（持 policy->rwsem）
 *    内挂 qos 不会死锁。
 *  机制二（时刻② init slam）: kprobe cppc_set_perf pre-handler 改写
 *    target_cpu 的 desired_perf/max_perf。init slam / set_target /
 *    fast_switch / set_boost 全部经由 cppc_set_perf，单点覆盖。
 *
 * 设计文档: docs/superpowers/specs/2026-10-10-cpu122-online-atomic-freqcap-design.md
 * 运行手册: 同目录 README.md（V1 好核演练 → V3 真实上线）
 */
#ifndef __aarch64__
#error "cpu122_capfreq: kprobe 参数寄存器布局仅适配 aarch64（AAPCS64 x0/x1）"
#endif

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/mutex.h>
#include <linux/cpufreq.h>
#include <linux/pm_qos.h>
#include <linux/cpu.h>
#include <linux/smp.h>
#include <acpi/cppc_acpi.h>

#define LOGTAG "cpu122-capfreq: "

static unsigned int target_cpu = 122;
module_param(target_cpu, uint, 0444);
MODULE_PARM_DESC(target_cpu, "logical CPU to cap (default 122, the faulty core)");

static unsigned int cap_khz = 1450000;

static DEFINE_MUTEX(state_lock);
static struct freq_qos_request qos_req;
static bool qos_active;
static struct cppc_perf_caps perf_caps;
static bool caps_valid;
static unsigned int cap_perf;      /* 0 = 未换算或停用，kprobe 直通 */
static unsigned long clamp_count;

static unsigned int qos_value(void)
{
	return cap_khz ? cap_khz : FREQ_QOS_MAX_DEFAULT_VALUE;
}

static void recompute_cap_perf(void)
{
	if (!caps_valid || cap_khz == 0) {
		cap_perf = 0;
		return;
	}
	cap_perf = cppc_khz_to_perf(&perf_caps, cap_khz);
}

/* ---------- 机制二：kprobe clamp（时刻②） ----------
 * kprobe pre-handler 为原子上下文（禁睡）：只做单字读写与限速打印，不取锁。
 * cap_perf/target_cpu 竞读的后果仅为换档瞬间一次直通，可接受。
 */
static int cppc_set_perf_pre(struct kprobe *p, struct pt_regs *regs)
{
	int cpu = (int)regs->regs[0];
	struct cppc_perf_ctrls *ctrls = (struct cppc_perf_ctrls *)regs->regs[1];
	bool clamped = false;

	if (cpu != (int)target_cpu || cap_perf == 0 || !ctrls)
		return 0;

	if (ctrls->desired_perf > cap_perf) {
		ctrls->desired_perf = cap_perf;
		clamped = true;
	}
	if (ctrls->max_perf > cap_perf) {
		ctrls->max_perf = cap_perf;
		clamped = true;
	}
	if (clamped) {
		clamp_count++;
		pr_info_ratelimited(LOGTAG "CPU%u cppc_set_perf clamped to perf %u (count %lu)\n",
				    target_cpu, cap_perf, clamp_count);
	}
	return 0;
}

static struct kprobe kp_cppc_set_perf = {
	.symbol_name = "cppc_set_perf",
	.pre_handler = cppc_set_perf_pre,
};

/* ---------- 机制一：freq_qos（时刻③） ---------- */
static int policy_event(struct notifier_block *nb, unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;

	if (event != CPUFREQ_CREATE_POLICY || policy->cpu != target_cpu)
		return NOTIFY_DONE;

	mutex_lock(&state_lock);
	/* caps 兜底：加载期换算失败的场合，此刻 CPU 已在线，重试一次 */
	if (!caps_valid && cap_khz) {
		if (cppc_get_perf_caps(target_cpu, &perf_caps) == 0) {
			caps_valid = true;
			recompute_cap_perf();
			pr_info(LOGTAG "caps converted at CREATE_POLICY: %u kHz -> perf %u\n",
				cap_khz, cap_perf);
		} else {
			pr_err(LOGTAG "cppc_get_perf_caps(CPU%u) still failing: kprobe clamp INERT (qos unaffected)\n",
			       target_cpu);
		}
	}
	if (qos_active) {
		pr_warn(LOGTAG "CPU%u policy recreated while qos active, skip re-add\n",
			target_cpu);
		mutex_unlock(&state_lock);
		return NOTIFY_DONE;
	}
	if (freq_qos_add_request(&policy->constraints, &qos_req,
				 FREQ_QOS_MAX, qos_value()) < 0) {
		/* CREATE_POLICY 返回值被 cpufreq core 忽略，无法中止上线；
		 * kprobe 仍值守，cpu122-online-offline.sh 后验2（频率读数）兜底拦截 */
		pr_err(LOGTAG "freq_qos_add_request(CPU%u) FAILED: qos UNARMED\n",
		       target_cpu);
		mutex_unlock(&state_lock);
		return NOTIFY_DONE;
	}
	qos_active = true;
	pr_info(LOGTAG "FREQ_QOS_MAX=%u kHz attached to CPU%u policy (survives hotplug)\n",
		qos_value(), target_cpu);
	mutex_unlock(&state_lock);
	return NOTIFY_OK;
}

static struct notifier_block policy_nb = {
	.notifier_call = policy_event,
};

/* ---------- 参数 cap_khz 运行时可写（改档入口，免上下线） ---------- */
static int cap_khz_set(const char *val, const struct kernel_param *kp)
{
	unsigned int old = cap_khz;
	int ret;

	ret = param_set_uint(val, kp);
	if (ret)
		return ret;

	mutex_lock(&state_lock);
	recompute_cap_perf();
	if (qos_active && freq_qos_update_request(&qos_req, qos_value()) < 0)
		pr_err(LOGTAG "freq_qos_update_request FAILED (constraint may be stale)\n");
	pr_info(LOGTAG "cap_khz %u -> %u%s\n", old, cap_khz,
		cap_khz > 2000000 ? " (DANGER: above 2.0G experiment tier)" : "");
	mutex_unlock(&state_lock);
	return 0;
}

static const struct kernel_param_ops cap_khz_ops = {
	.set = cap_khz_set,
	.get = param_get_uint,
};
module_param_cb(cap_khz, &cap_khz_ops, &cap_khz, 0644);
MODULE_PARM_DESC(cap_khz, "freq cap in kHz (0=disabled, default 1450000, runtime-writable)");

static int __init cpu122_capfreq_init(void)
{
	struct cpufreq_policy *policy;
	int ret;

	if (!cpu_possible(target_cpu)) {
		pr_err(LOGTAG "target CPU%u not possible on this system\n", target_cpu);
		return -EINVAL;
	}

	mutex_lock(&state_lock);
	if (cppc_get_perf_caps(target_cpu, &perf_caps) == 0) {
		caps_valid = true;
		recompute_cap_perf();
		pr_info(LOGTAG "caps: highest=%u nominal=%u (perf); cap %u kHz -> perf %u\n",
			perf_caps.highest_perf, perf_caps.nominal_perf, cap_khz, cap_perf);
	} else {
		/* 离线 CPU 的 ACPI 命名空间数据通常可读；失败不阻断加载：
		 * qos 不依赖换算，kprobe 钳位待 CREATE_POLICY 兜底重试 */
		pr_warn(LOGTAG "cppc_get_perf_caps(CPU%u) failed at load: kprobe clamp deferred\n",
			target_cpu);
	}
	mutex_unlock(&state_lock);

	ret = register_kprobe(&kp_cppc_set_perf);
	if (ret) {
		pr_err(LOGTAG "register_kprobe(cppc_set_perf) failed: %d, refusing load\n", ret);
		return ret;
	}
	ret = cpufreq_register_notifier(&policy_nb, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err(LOGTAG "cpufreq_register_notifier failed: %d, refusing load\n", ret);
		unregister_kprobe(&kp_cppc_set_perf);
		return ret;
	}

	/* 迟加载兜底：本启动 target 已在线（policy 已存在）→ 直接挂 qos */
	policy = cpufreq_cpu_get(target_cpu);
	if (policy) {
		mutex_lock(&state_lock);
		if (!qos_active &&
		    freq_qos_add_request(&policy->constraints, &qos_req,
					 FREQ_QOS_MAX, qos_value()) >= 0) {
			qos_active = true;
			pr_info(LOGTAG "late-bind: FREQ_QOS_MAX=%u kHz (CPU%u already online)\n",
				qos_value(), target_cpu);
		}
		mutex_unlock(&state_lock);
		cpufreq_cpu_put(policy);
	}

	pr_info(LOGTAG "armed: target=CPU%u cap=%u kHz kprobe=%s qos=%s\n",
		target_cpu, cap_khz,
		cap_perf ? "ok" : "pending-perf-conv",
		qos_active ? "ok" : "pending-policy");
	return 0;
}

static void __exit cpu122_capfreq_exit(void)
{
	mutex_lock(&state_lock);
	if (qos_active)
		freq_qos_remove_request(&qos_req);
	mutex_unlock(&state_lock);
	cpufreq_unregister_notifier(&policy_nb, CPUFREQ_POLICY_NOTIFIER);
	unregister_kprobe(&kp_cppc_set_perf);
	pr_info(LOGTAG "disarmed%s (clamp_count=%lu)\n",
		cpu_online(target_cpu) ? " - WARNING: target online, no cap protection now" : "",
		clamp_count);
}

module_init(cpu122_capfreq_init);
module_exit(cpu122_capfreq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("wangxu <wangxumarshall@qq.com>");
MODULE_DESCRIPTION("atomic per-CPU freq cap for faulty core CPU122 (kprobe + freq_qos)");
MODULE_VERSION("1.0");
```

- [ ] **Step 2: 写 `Makefile`、`.gitignore`、`deploy/` 两个 conf**

`Makefile`：

```make
# cpu122_capfreq — 依赖目标内核的 kernel-devel（板上: 6.6.0-159.4.13.167）
# 内核升级后必须在此重编（SOP 见 README.md），脚本门0 会拦截 vermagic 不匹配的失载。
obj-m += cpu122_capfreq.o

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean
```

`.gitignore`：

```
*.o
*.ko
*.mod
*.mod.c
*.symvers
*.order
.tmp_versions/
.cache.mk
```

`deploy/modules-load.d-cpu122-capfreq.conf`：

```
cpu122_capfreq
```

`deploy/modprobe.d-cpu122-capfreq.conf`：

```
options cpu122_capfreq target_cpu=122 cap_khz=1450000
```

- [ ] **Step 3: 写 `README.md`（完整内容）**

```markdown
# cpu122-capfreq — CPU122（坏核）上线即封顶内核模块

## 为什么需要它：三时刻分析

`echo 1 > cpu122/online` 的 write() 返回前，CPU122 已完整上线并满频暴露。
频率失控分三时刻（v6.6 源码级分析，详见
`docs/superpowers/specs/2026-10-10-cpu122-online-atomic-freqcap-design.md`）：

| 时刻 | 内容 | 处置 |
|---|---|---|
| ① PSCI 热启动 → cpufreq step 前 | 固件域频率 | 模块不可达（残余，历史实证低风险）；长期走 BIOS/BMC per-core 限频 |
| ② 驱动 init slam | `cppc_cpufreq_cpu_init()` 在上线路径内直接 `cppc_set_perf(desired=highest_perf=2.9G)` | **kprobe 钳位：首次 CPPC 写入即封顶** |
| ③ governor 稳态 | `policy->max` 由 freq_qos 聚合（cpufreq_set_policy 权威源） | **FREQ_QOS_MAX 挂 policy，跨热插拔存活** |

udev cap 脚本架构性必输：KOBJ_ONLINE uevent 在 write() 返回前才广播，
一切用户态触发源都在风险窗口之后（实测上线后 ~10ms 即死）。

## 机制

- **kprobe `cppc_set_perf` pre-handler**（aarch64：x0=cpu, x1=perf_ctrls）：
  `target_cpu` 的 desired_perf/max_perf > cap_perf 时改写为 cap_perf。
  init slam / set_target / fast_switch / set_boost 全部经由 cppc_set_perf，
  单点覆盖。
- **freq_qos FREQ_QOS_MAX 请求**：挂 policy->constraints；policy 结构跨
  热插拔 offline/online 存活 → 一次挂载、后续上线自动生效；qos 变更经
  policy 自带 notifier（schedule_work 异步）自动传导 governor，
  **改档无需上下线**。
- 失败即拒载：kprobe / cpufreq notifier 注册失败 → 模块加载失败
  （宁可不上线，不可裸奔上线；脚本门0 拦截失载）。
- qos 挂载失败（CREATE_POLICY 返回值被 core 忽略，无法中止上线）→
  dmesg `qos UNARMED` ERROR + 脚本后验2 频率读数兜底并自动下线。

## 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `target_cpu` | 122 | 受保护逻辑 CPU（V1 演练时用 123） |
| `cap_khz` | 1450000 | kHz 上限。运行时可写：`echo 2000000 > /sys/module/cpu122_capfreq/parameters/cap_khz`，即时生效。0 = 停用（kprobe 直通 + qos 失效） |

## 构建（板上执行；内核升级后必须重复）

```console
# 1. 安装当前内核的 kernel-devel（以 159.4.13.167 为例）
sudo dnf install "kernel-devel-$(uname -r)"
# 2. 构建（在 /home/sdc/cpu122-capfreq/，与仓内正本同步）
make
# 3. 确认零警告零错误，查看 vermagic
modinfo cpu122_capfreq.ko | grep -E 'vermagic|filename'
```

## 自动加载（部署一次性执行）

```console
sudo cp deploy/modules-load.d-cpu122-capfreq.conf /etc/modules-load.d/cpu122-capfreq.conf
sudo cp deploy/modprobe.d-cpu122-capfreq.conf    /etc/modprobe.d/cpu122-capfreq.conf
sudo modprobe cpu122_capfreq        # 立即加载（等价于重启后的自动加载）
dmesg | grep 'cpu122-capfreq'       # 应见 armed: ... kprobe=ok qos=ok/pending-policy
```

## 验证手册（V1–V6，先无风险后真实；结论回写 progress-log）

**V1 好核演练（不碰坏核，target_cpu=123）**

```console
sudo insmod cpu122_capfreq.ko target_cpu=123 cap_khz=1450000
echo 0 > /sys/devices/system/cpu/cpu123/online     # 下线好核 123
echo 1 > /sys/devices/system/cpu/cpu123/online     # 上线：模块应在上线路径内封顶
dmesg | tail -5                                    # 期望 FREQ_QOS_MAX=... attached + clamped
cat /sys/devices/system/cpu/cpu123/cpufreq/scaling_max_freq   # 期望 1450000
cat /sys/devices/system/cpu/cpu123/cpufreq/scaling_cur_freq   # 期望 ≤ 1450000（+取整容差）
echo 2000000 > /sys/module/cpu122_capfreq/parameters/cap_khz  # 切档
cat /sys/devices/system/cpu/cpu123/cpufreq/scaling_max_freq   # 期望自动跟随 2000000
echo 0 > /sys/devices/system/cpu/cpu123/online && echo 1 > /sys/devices/system/cpu/cpu123/online
cat /sys/devices/system/cpu/cpu123/cpufreq/scaling_max_freq   # 复用 policy 路径仍 2000000
sudo rmmod cpu122_capfreq
```

**V2 vermagic 门**：换内核后不重编 → 重启后 `lsmod | grep cpu122_capfreq` 为空 →
脚本 `上线` 被门0 拒绝。

**V3 真实上线**：`modprobe cpu122_capfreq`（conf 默认 target_cpu=122）→
`sudo /home/sdc/cpu122-online-offline.sh 上线` → 存活 + 门0/门1/后验全过 +
dmesg clamp 证据留存（同步 0101）。

**V4 切档**：`sudo /home/sdc/cpu122-online-offline.sh 封顶 2000000` → 读数跟随；
`封顶 1450000` 回落。

**V5 回归**：122 离线态模块值守无副作用（无 policy 时模块静默）；
postboot-check 行为不变；`-e zstd19 -t 2000 -n 1` 回归 pass。

**V6 部署清理**：检索并清除板上可能残留的 udev cap 尝试规则
（`grep -rn 'scaling_max_freq' /etc/udev /usr/lib/udev/rules.d /run/udev`）；
**注意保留** `SUBSYSTEM=="cpu", ACTION=="add"` 防自动上线注释（两者无关）。

## 卸载与安全边界

- `rmmod` 前确认 target 离线；在线卸载 = 无封顶保护（dmesg 会警告）。
- `封顶 0`（cap_khz=0）= 显式解锁，仅限满频 A/B 实验，风险自担。
- 时刻①（PSCI 热启动早期段）残余风险：固件域，本模块不可达；上线时机错峰
  （系统 idle、kdump 武装）+ 每加电会话一次上线纪律缓解。
```

- [ ] **Step 4: 本机构建验证（零警告为门）**

```console
cd docs/cases/sdc1-01-04-core122/cpu122-capfreq
make 2>&1 | tee /tmp/capfreq-build.log
```

预期：`CC [M] cpu122_capfreq.o` → `LD [M] cpu122_capfreq.ko`，**无任何 warning/error**（Makefile 走内核默认 -Wall；若出现警告必须修复后重建，禁止带警告提交）。然后：

```console
modinfo cpu122_capfreq.ko | grep -E 'vermagic|description'
make clean
```

预期 vermagic = `6.6.0-159.4.3.154.oe2403sp4.aarch64 SMP preempt mod_unload modversions aarch64`（本机系列，板上重编为 159.4.13.167）。`make clean` 后 `git status` 确认无构建产物入库。

- [ ] **Step 5: 本机加载冒烟（root 口令已在会话中提供；凭据不落盘）**

以 `sudo -S` 从 stdin 传口令执行（口令只出现在命令中，不写入任何文件/提交/文档）：

```console
printf '%s\n' '<口令>' | sudo -S insmod cpu122_capfreq.ko target_cpu=0 cap_khz=1450000
dmesg | tail -3
printf '%s\n' '<口令>' | sudo -S rmmod cpu122_capfreq
dmesg | tail -1
```

预期（本机无 CPPC caps、无 cpufreq policy，属正常降级路径）：`armed: target=CPU0 ... kprobe=pending-perf-conv qos=pending-policy` → `disarmed (clamp_count=0)`，加载/卸载干净。**若 sudo 因故不可用，在提交信息中如实注明"本机仅构建验证，运行时验证待板上 V1"**。

- [ ] **Step 6: 提交并推送**

```bash
git add docs/cases/sdc1-01-04-core122/cpu122-capfreq/
git commit -s -m "feat: cpu122-capfreq 内核模块——CPU122 上线即封顶

kprobe cppc_set_perf 钳位（覆盖 init slam/set_target/set_boost）+
freq_qos FREQ_QOS_MAX 挂 policy（跨热插拔存活，governor 稳态封顶）。
失败即拒载；qos 失效时 dmesg ERROR + 脚本后验兜底。
本机零警告构建通过；运行时验证 V1-V6 手册见 README。

Signed-off-by: wangxu <wangxumarshall@qq.com>"
git push
```

---

### Task 2: 脚本测试基建 + `封顶` 子命令（TDD）

**Files:**
- Create: `docs/cases/sdc1-01-04-core122/cpu122-capfreq/tests/run_tests.sh`
- Modify: `docs/cases/sdc1-01-04-core122/cpu122-online-offline.sh`

**Interfaces:**
- Consumes: Task 1 的 `/sys/module/cpu122_capfreq/parameters/cap_khz`（0644）。
- Produces（Task 3 复用）: 脚本 env 钩子 `CPU_SYSFS`/`MOD_SYSFS`/`CPU122_CAPFREQ_TEST_MODE`；函数 `module_ready`（成功时置全局 `MOD_CAP`）；常量 `CAP_HARD_MAX=2900000`、`CAP_WARN_MAX=2000000`；测试基建 `new_env`/`mod_unload`/`mod_setcap`/`run_script`/`assert_rc`/`assert_has`/`assert_file`。

- [ ] **Step 1: 写测试 `tests/run_tests.sh`（基建 + T2 用例，完整代码）**

```bash
#!/bin/bash
# =============================================================================
# tests/run_tests.sh — cpu122-online-offline.sh v2 行为测试（fake sysfs，无真实硬件）
# 原理: CPU_SYSFS/MOD_SYSFS 重定向到临时假树 + PATH 前置假 journalctl/dmesg
#       + CPU122_CAPFREQ_TEST_MODE=1 跳过 root 检查与准备1/2（不触碰真实 grub/udev）
# 运行: bash tests/run_tests.sh     退出码 0=全过
# =============================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPT=$(cd "$HERE/.." && pwd)/../cpu122-online-offline.sh

PASS=0 FAIL=0 FAILED_NAMES=""

ok()  { PASS=$((PASS+1)); echo "  PASS: $*"; }
bad() { FAIL=$((FAIL+1)); FAILED_NAMES="$FAILED_NAMES
  FAIL: $*"; echo "  FAIL: $*"; }

ENVROOT=""

new_env() {                     # 每个 case 一个干净假树
    ENVROOT=$(mktemp -d /tmp/cpu122test.XXXXXX)
    local c=$ENVROOT/cpu
    mkdir -p "$ENVROOT/fakebin" "$ENVROOT/journal" \
            "$c/cpu121/topology" "$c/cpu122/topology" "$c/cpu122/cpufreq" "$c/cpu123/topology"
    # 全局 CPU 状态（happy 默认，个别 case 覆写）
    echo "0-127"          > "$c/present"
    echo "0-121,123-127"  > "$c/online"
    echo "122"            > "$c/offline"
    # cpu121/122/123 拓扑（同簇、core_id 递增；122 位于 121 与 123 之间）
    for n in 121 122 123; do echo 1 > "$c/cpu$n/topology/physical_package_id"; done
    echo 290 > "$c/cpu121/topology/core_id"
    echo 292 > "$c/cpu122/topology/core_id"
    echo 294 > "$c/cpu123/topology/core_id"
    echo 1 > "$c/cpu121/online"; echo 1 > "$c/cpu123/online"
    # cpu122 离线 + cpufreq 读数（模块武装后的期望值，个别 case 覆写）
    echo 0        > "$c/cpu122/online"
    echo 1450000  > "$c/cpu122/cpufreq/scaling_max_freq"
    echo 1450000  > "$c/cpu122/cpufreq/scaling_cur_freq"
    # 假 journalctl / dmesg
    printf '#!/bin/bash\ncat "%s/journal/boot.log"\n' "$ENVROOT" > "$ENVROOT/fakebin/journalctl"
    printf '#!/bin/bash\ncat "%s/journal/dmesg.log" 2>/dev/null\n' "$ENVROOT" > "$ENVROOT/fakebin/dmesg"
    chmod +x "$ENVROOT/fakebin/"*
    echo "localhost kernel: CPU122: Booted secondary processor 0x0900060200" > "$ENVROOT/journal/boot.log"
    echo "cpu122-capfreq: armed: target=CPU122 cap=1450000 kHz" > "$ENVROOT/journal/dmesg.log"
    # 模块默认已加载（cap=1450000）；"未加载" case 用 mod_unload 覆写
    mkdir -p "$ENVROOT/mod/params"
    echo 1450000 > "$ENVROOT/mod/params/cap_khz"
}

mod_unload() { rm -rf "$ENVROOT/mod"; }
mod_setcap()  { echo "$1" > "$ENVROOT/mod/params/cap_khz"; }

run_script() {                 # $@ = 脚本参数
    CPU122_CAPFREQ_TEST_MODE=1 \
    CPU_SYSFS="$ENVROOT/cpu" \
    MOD_SYSFS="$ENVROOT/mod/params" \
    PATH="$ENVROOT/fakebin:$PATH" \
    bash "$SCRIPT" "$@"
}

assert_rc()   { [ "$1" -eq "$2" ] && ok "$3" || bad "$3 (rc=$1 期望 $2)"; }
assert_has()  { printf '%s' "$2" | grep -q -- "$3" && ok "$1" || bad "$1"; }
assert_file() { [ "$(cat "$1" 2>/dev/null)" = "$2" ] && ok "$3" \
                || bad "$3 ($(cat "$1" 2>/dev/null) ≠ $2)"; }

# ============================== T2 用例 ==============================

echo "== T2.1 下线基线（验证测试基建与既有路径不回归）=="
new_env
echo 1 > "$ENVROOT/cpu/cpu122/online"          # 122 当前在线
out=$(run_script 下线); rc=$?
assert_rc $rc 0 "T2.1 下线 rc=0"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T2.1 online 文件写 0"
assert_has "$out" "下线成功" "T2.1 输出含 下线成功"
rm -rf "$ENVROOT"

echo "== T2.2 封顶·模块未加载 → 拒绝 =="
new_env; mod_unload
out=$(run_script 封顶 2000000); rc=$?
assert_rc $rc 1 "T2.2 rc=1"
assert_has "$out" "未加载" "T2.2 提示模块未加载"
rm -rf "$ENVROOT"

echo "== T2.3 封顶 2000000（122 离线）→ 写参数 + 提示下次上线生效 =="
new_env
out=$(run_script 封顶 2000000); rc=$?
assert_rc $rc 0 "T2.3 rc=0"
assert_file "$ENVROOT/mod/params/cap_khz" 2000000 "T2.3 cap_khz 写入 2000000"
assert_has "$out" "下次上线" "T2.3 提示下次上线路径内生效"
rm -rf "$ENVROOT"

echo "== T2.4 封顶 2000000（122 在线）→ 读数跟随 =="
new_env; echo 1 > "$ENVROOT/cpu/cpu122/online"
echo 2000000 > "$ENVROOT/cpu/cpu122/cpufreq/scaling_max_freq"   # 桩：模块传导后的读数
out=$(run_script 封顶 2000000); rc=$?
assert_rc $rc 0 "T2.4 rc=0"
assert_has "$out" "在线验证" "T2.4 在线验证输出"
rm -rf "$ENVROOT"

echo "== T2.5 封顶参数校验 =="
new_env
run_script 封顶 >/dev/null;         assert_rc $? 1 "T2.5a 缺 kHz → rc=1"
run_script 封顶 abc >/dev/null;     assert_rc $? 1 "T2.5b 非数字 → rc=1"
run_script 封顶 3100000 >/dev/null; assert_rc $? 1 "T2.5c 超 2.9G → rc=1"
out=$(run_script 封顶 2900000);     assert_rc $? 0 "T2.5d 2900000 高危放行 rc=0"
assert_has "$out" "高危" "T2.5d 高危警告"
out=$(run_script 封顶 0);           assert_rc $? 0 "T2.5e 0=解锁放行 rc=0"
assert_has "$out" "解锁" "T2.5e 解锁警告"
rm -rf "$ENVROOT"

echo "======================================"
echo "PASS=$PASS FAIL=$FAIL"
[ $FAIL -eq 0 ] || { echo "$FAILED_NAMES"; exit 1; }
```

- [ ] **Step 2: 运行测试，确认按预期失败（红）**

```console
cd docs/cases/sdc1-01-04-core122/cpu122-capfreq && bash tests/run_tests.sh; echo "exit=$?"
```

预期：T2.1 失败（当前脚本无 TEST_MODE，`exec sudo` 不可用导致 rc≠0 或 online 未写 0）；T2.2–T2.5 全部失败（当前脚本不识 `封顶`，usage 退出 1，T2.3/T2.4/T2.5d/T2.5e 期望 0 却得 1）。记录实际输出。

- [ ] **Step 3: 改脚本——env 钩子、TEST_MODE、参数解析、封顶子命令**

对 `cpu122-online-offline.sh` 做以下修改（保留准备1/2/3 函数不动）：

**(a) 变量块（原 `BAD_CORE=122` 到 `UDEV_CPU_ONLINE_RE=...` 之间）改为：**

```bash
BAD_CORE=122
# 测试钩子: TEST_MODE 下 CPU_SYSFS/MOD_SYSFS 可指向假树（仅测试使用，板上勿设）
CPU_SYSFS=${CPU_SYSFS:-/sys/devices/system/cpu}
MOD_SYSFS=${MOD_SYSFS:-/sys/module/cpu122_capfreq/parameters}
GRUB_DEFAULT=/etc/default/grub
CAP_HARD_MAX=2900000     # 板上物理最高频（kHz）
CAP_WARN_MAX=2000000     # 超出即高危警告（2.0G 实验档以上）
```

（`UDEV_CPU_ONLINE_RE` 原行保留在其后。）

**(b) `usage()` 替换为：**

```bash
usage() {
    cat <<EOF
用法: $0 {上线|on|1|下线|off|0}          坏核上下线
      $0 {封顶|cap} <kHz>               在线改频率上限（免上下线, 0=解锁）
  上线|on|1   -> 门0(模块就绪)+门1(编号预检) -> echo 1 -> 后验(MPIDR+频率)
  下线|off|0  -> echo 0 -> cpu122/online
  封顶|cap    -> 写 /sys/module/cpu122_capfreq/parameters/cap_khz
参数强制显式指定，没有默认值。
示例:
  sudo $0 上线            # 三门齐过后上线（封顶由模块在上线路径内完成）
  sudo $0 下线            # CPU122 下线
  sudo $0 封顶 2000000    # 切 2.0G 档（在线生效, 无需上下线）
  sudo $0 封顶 0          # 解锁（仅限满频 A/B, 高危）
EOF
}
```

**(c) 参数解析块（原 `if [ $# -ne 1 ]...` 到 `esac`）替换为：**

```bash
# ---------- 参数解析（先于 root 检查，无参数/错误参数直接报用法） ----------
if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    usage
    exit 1
fi
case "$1" in
    上线|on|1)  [ $# -eq 1 ] || { usage; exit 1; }
                ACT=1; ACT_TXT="上线" ;;
    下线|off|0) [ $# -eq 1 ] || { usage; exit 1; }
                ACT=0; ACT_TXT="下线" ;;
    封顶|cap)   ACT=2; ACT_TXT="封顶"
                [ $# -eq 2 ] || { usage; exit 1; }
                case "$2" in ''|*[!0-9]*) usage; exit 1 ;; esac
                CAP_ARG=$2 ;;
    -h|--help)  usage; exit 0 ;;
    *)          usage; exit 1 ;;
esac
```

**(d) root 检查块改为（加 TEST_MODE 分支）：**

```bash
# ---------- root 权限（sysfs / udev / grub 写操作均需要） ----------
if [ "${CPU122_CAPFREQ_TEST_MODE:-}" = "1" ]; then
    log "TEST MODE: 跳过 root 检查与准备1/2（仅 fake sysfs 测试用）"
elif [ "$(id -u)" -ne 0 ]; then
    log "需要 root 权限，通过 sudo 重新执行..."
    exec sudo bash "$0" "$@"
    die "sudo 不可用，请以 root 手动运行"
fi
```

**(e) 主流程块（原 `log "===== 准备工作 ====="` 到文件尾）替换为：**

```bash
# ============================== 主流程 ==============================
log "===== 准备工作 ====="
if [ "${CPU122_CAPFREQ_TEST_MODE:-}" != "1" ]; then
    block_udev_autoonline
    ensure_grub_params
else
    log "TEST MODE: 准备1/2 已跳过"
fi
online_all_except_bad

# ---------- 门0: 模块就绪检查（上线/封顶共用；成功时置全局 MOD_CAP） ----------
module_ready() {
    local p="$MOD_SYSFS/cap_khz"
    if [ ! -d "${MOD_SYSFS%/*}" ]; then
        log "门0: 模块 cpu122_capfreq 未加载 — 拒绝（裸奔上线封顶无从谈起）"
        log "门0: 排查: lsmod | grep cpu122_capfreq; dmesg | grep cpu122-capfreq; 内核升级后需按模块 README 重编"
        return 1
    fi
    if [ ! -r "$p" ]; then
        log "门0: 参数文件 $p 不可读 — 拒绝"
        return 1
    fi
    MOD_CAP=$(cat "$p" 2>/dev/null)
    case "$MOD_CAP" in ''|*[!0-9]*)
        log "门0: cap_khz 非法值 '$MOD_CAP' — 拒绝"; return 1 ;;
    esac
    return 0
}

case "$ACT" in
2)  # ---- 封顶: 在线改模块频率上限（qos 自动传导, 无需上下线） ----
    log "===== 主功能: 封顶 CPU$BAD_CORE @ ${CAP_ARG}kHz ====="
    module_ready || die "封顶需要模块就绪"
    if [ "$CAP_ARG" -gt "$CAP_HARD_MAX" ]; then
        die "封顶值 $CAP_ARG 超出物理最高频 $CAP_HARD_MAX"
    fi
    [ "$CAP_ARG" -gt "$CAP_WARN_MAX" ] \
        && log "高危警告: 封顶 $CAP_ARG 超出 2.0G 实验档（坏核满频曾 <4s 致死）"
    [ "$CAP_ARG" -eq 0 ] \
        && log "警告: 封顶 0 = 解除限制（kprobe 直通 + qos 失效）— 仅限满频 A/B 且风险自担"
    err=$( { echo "$CAP_ARG" > "$MOD_SYSFS/cap_khz"; } 2>&1 ) \
        || die "写 cap_khz 失败: $err"
    MOD_CAP=$(cat "$MOD_SYSFS/cap_khz")
    [ "$MOD_CAP" = "$CAP_ARG" ] || die "cap_khz 回读不符: $MOD_CAP ≠ $CAP_ARG"
    log "封顶 $CAP_ARG kHz 已生效"
    if [ "$(cat "$CPU_SYSFS/cpu$BAD_CORE/online" 2>/dev/null)" = "1" ]; then
        sleep 0.2    # qos notifier 经 schedule_work 异步生效
        fmax=$(cat "$CPU_SYSFS/cpu$BAD_CORE/cpufreq/scaling_max_freq" 2>/dev/null)
        if [ -n "$fmax" ] && [ "$fmax" -gt $((MOD_CAP + 1000)) ]; then
            log "警告: 在线读数 scaling_max_freq=$fmax 未跟随 cap=$MOD_CAP（qos 疑未挂载, 检查 dmesg 'cpu122-capfreq'）"
        else
            log "在线验证: scaling_max_freq=$fmax ≤ cap=$MOD_CAP ✓"
        fi
    else
        log "CPU$BAD_CORE 当前离线 — cap 将在下次上线路径内生效"
    fi
    ;;
1)  # ---- 上线 ----（Task 3 完整实现; 本任务先保留既有主功能行为）
    log "===== 主功能: CPU$BAD_CORE 【上线】 (online=1) ====="
    err=$( { echo 1 > "$CPU_SYSFS/cpu$BAD_CORE/online"; } 2>&1 ) \
        || die "CPU$BAD_CORE 上线失败: $err"
    cur=$(cat "$CPU_SYSFS/cpu$BAD_CORE/online")
    [ "$cur" = "1" ] || die "CPU$BAD_CORE 状态验证失败: online=$cur（期望 1）"
    log "CPU$BAD_CORE 上线成功，当前 online=$cur"
    ;;
0)  # ---- 下线 ----
    log "===== 主功能: CPU$BAD_CORE 【下线】 (online=0) ====="
    err=$( { echo 0 > "$CPU_SYSFS/cpu$BAD_CORE/online"; } 2>&1 ) \
        || die "CPU$BAD_CORE 下线失败: $err"
    cur=$(cat "$CPU_SYSFS/cpu$BAD_CORE/online")
    [ "$cur" = "0" ] || die "CPU$BAD_CORE 状态验证失败: online=$cur（期望 0）"
    log "CPU$BAD_CORE 下线成功，当前 online=$cur"
    ;;
esac

# ---------- 最终状态 ----------
off=$(cat "$CPU_SYSFS/offline" 2>/dev/null)
log "online : $(cat "$CPU_SYSFS/online")"
log "offline: ${off:-（空）}"
log "完成。"
```

- [ ] **Step 4: 跑测试到全绿**

```console
bash tests/run_tests.sh; echo "exit=$?"
bash -n ../cpu122-online-offline.sh && echo "syntax OK"
```

预期：`PASS=17 FAIL=0`（T2.1×3 + T2.2×2 + T2.3×3 + T2.4×2 + T2.5×7 = 17 断言，全部 PASS，exit=0），`syntax OK`。若有失败，修复脚本（不是改测试）直到全绿。

- [ ] **Step 5: 提交并推送**

```bash
git add docs/cases/sdc1-01-04-core122/cpu122-capfreq/tests/run_tests.sh \
        docs/cases/sdc1-01-04-core122/cpu122-online-offline.sh
git commit -s -m "feat: 脚本 v2 测试基建 + 封顶子命令

fake sysfs/假 journalctl 行为测试框架（TEST_MODE 跳过 root 与准备1/2）;
封顶 = 在线写模块 cap_khz, qos 自动传导免上下线, 0=显式解锁高危警告。
T2.1-T2.5 全绿, bash -n 通过。

Signed-off-by: wangxu <wangxumarshall@qq.com>"
git push
```

---

### Task 3: 上线三门 + 后验 + 下线微调 + 头注释（TDD）

**Files:**
- Modify: `docs/cases/sdc1-01-04-core122/cpu122-online-offline.sh`
- Modify: `docs/cases/sdc1-01-04-core122/cpu122-capfreq/tests/run_tests.sh`（追加 T3 用例）

**Interfaces:**
- Consumes: Task 2 的 `module_ready`/`MOD_CAP`/`CAP_HARD_MAX`/`CAP_WARN_MAX`、测试基建。
- Produces: `cap_sanity_for_online`（上线前 cap 边界）、`drift_preflight`（门1）、`postonline_verify`（后验，MPIDR 不符/读数越界自动下线）；MPIDR 期望常量 `0x0900060200`。

- [ ] **Step 1: 在 `tests/run_tests.sh` 的 `echo "======================================"` 汇总行之前追加 T3 用例**

```bash
# ============================== T3 用例 ==============================

echo "== T3.1 上线·模块未加载 → 拒绝且不写 online =="
new_env; mod_unload
out=$(run_script 上线); rc=$?
assert_rc $rc 1 "T3.1 rc=1"
assert_has "$out" "门0" "T3.1 门0 拦截输出"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T3.1 online 保持 0（未裸奔上线）"
rm -rf "$ENVROOT"

echo "== T3.2 上线·cap=0 → 拒绝 =="
new_env; mod_setcap 0
out=$(run_script 上线); rc=$?
assert_rc $rc 1 "T3.2 rc=1"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T3.2 online 保持 0"
rm -rf "$ENVROOT"

echo "== T3.3 上线·cap 越界与高危档 =="
new_env; mod_setcap 3100000
run_script 上线 >/dev/null; assert_rc $? 1 "T3.3a cap=3100000 拒绝"
rm -rf "$ENVROOT"
new_env; mod_setcap 2500000
echo 2500000 > "$ENVROOT/cpu/cpu122/cpufreq/scaling_max_freq"
echo 2500000 > "$ENVROOT/cpu/cpu122/cpufreq/scaling_cur_freq"
out=$(run_script 上线); rc=$?
assert_rc $rc 0 "T3.3b cap=2500000 高危警告后放行"
assert_has "$out" "高危" "T3.3b 高危警告输出"
rm -rf "$ENVROOT"

echo "== T3.4 门1·present 漂移 → 拒绝 =="
new_env; echo "0-126" > "$ENVROOT/cpu/present"
out=$(run_script 上线); rc=$?
assert_rc $rc 1 "T3.4 present=0-126 拒绝"
assert_has "$out" "门1" "T3.4 门1 拦截输出"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T3.4 online 保持 0"
rm -rf "$ENVROOT"

echo "== T3.5 门1·offline 异常 → 拒绝 =="
new_env; echo "121,122" > "$ENVROOT/cpu/offline"
run_script 上线 >/dev/null; assert_rc $? 1 "T3.5 offline≠122 拒绝"
rm -rf "$ENVROOT"

echo "== T3.6 门1·拓扑漂移 → 拒绝 =="
new_env; echo 7 > "$ENVROOT/cpu/cpu122/topology/physical_package_id"
run_script 上线 >/dev/null; assert_rc $? 1 "T3.6 拓扑不符拒绝"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T3.6 online 保持 0"
rm -rf "$ENVROOT"

echo "== T3.7 上线 happy path =="
new_env
out=$(run_script 上线); rc=$?
assert_rc $rc 0 "T3.7 rc=0"
assert_file "$ENVROOT/cpu/cpu122/online" 1 "T3.7 online 写 1"
assert_has "$out" "0x0900060200" "T3.7 MPIDR 核对输出"
assert_has "$out" "后验2" "T3.7 频率后验输出"
rm -rf "$ENVROOT"

echo "== T3.8 后验1·MPIDR 不符 → 自动下线 =="
new_env
echo "localhost kernel: CPU122: Booted secondary processor 0x0800060200" > "$ENVROOT/journal/boot.log"
out=$(run_script 上线); rc=$?
assert_rc $rc 1 "T3.8 rc=1"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T3.8 自动下线（online 回 0）"
assert_has "$out" "MPIDR" "T3.8 MPIDR 报错输出"
rm -rf "$ENVROOT"

echo "== T3.9 后验2·qos 疑失效（max 越界）→ 自动下线 =="
new_env
echo 2900000 > "$ENVROOT/cpu/cpu122/cpufreq/scaling_max_freq"
out=$(run_script 上线); rc=$?
assert_rc $rc 1 "T3.9 rc=1"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T3.9 自动下线"
assert_has "$out" "qos 疑失效" "T3.9 qos 失效提示"
rm -rf "$ENVROOT"

echo "== T3.10 后验1·journal 无 Booted 行 → 自动下线 =="
new_env
echo "unrelated line" > "$ENVROOT/journal/boot.log"
run_script 上线 >/dev/null; assert_rc $? 1 "T3.10 无 Booted 行 rc=1"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T3.10 自动下线"
rm -rf "$ENVROOT"
```

- [ ] **Step 2: 运行，确认 T3 按预期失败（红）**

```console
bash tests/run_tests.sh; echo "exit=$?"
```

预期：T2 段全绿；T3.1/T3.2/T3.4/T3.5/T3.6/T3.8/T3.9/T3.10 失败（当前上线路径无门无后验，假树上直接上线成功 rc=0），T3.3a 失败（未拦截 3100000），T3.3b/T3.7 可能通过（rc 恰为 0 但缺高危/后验输出则 assert_has 失败）。记录实际输出。

- [ ] **Step 3: 脚本实现门0 上线边界 + 门1 + 后验，替换上线 case**

**(a) 在 `module_ready()` 函数之后追加三个函数：**

```bash
# ---------- 门0 附加上线边界: cap 必须 >0 且 ≤ 物理上限; >2.0G 高危警告 ----------
cap_sanity_for_online() {
    if [ "$MOD_CAP" -eq 0 ]; then
        log "门0: cap_khz=0（模块停用态）— 拒绝上线（如需满频 A/B, 先执行: $0 封顶 2900000 显式解锁）"
        return 1
    fi
    if [ "$MOD_CAP" -gt "$CAP_HARD_MAX" ]; then
        log "门0: cap_khz=$MOD_CAP 超出物理最高频 $CAP_HARD_MAX — 拒绝"
        return 1
    fi
    [ "$MOD_CAP" -gt "$CAP_WARN_MAX" ] \
        && log "门0: 高危警告: cap_khz=$MOD_CAP 超出 2.0G 实验档（坏核满频曾 <4s 致死）"
    return 0
}

# ---------- 门1: 编号漂移预检（防固件重屏蔽后 122 不再是坏核） ----------
drift_preflight() {
    local present offline pkg121 pkg122 pkg123 core121 core122 core123
    present=$(cat "$CPU_SYSFS/present" 2>/dev/null)
    offline=$(cat "$CPU_SYSFS/offline" 2>/dev/null)
    if [ "$present" != "0-127" ]; then
        log "门1: present='$present' ≠ 0-127（固件呈现变化, 逻辑编号可能漂移）— 拒绝上线"
        return 1
    fi
    if [ "$offline" != "122" ]; then
        log "门1: offline='$offline' ≠ 122 — 拒绝上线"
        return 1
    fi
    pkg121=$(cat "$CPU_SYSFS/cpu121/topology/physical_package_id" 2>/dev/null)
    pkg122=$(cat "$CPU_SYSFS/cpu122/topology/physical_package_id" 2>/dev/null)
    pkg123=$(cat "$CPU_SYSFS/cpu123/topology/physical_package_id" 2>/dev/null)
    core121=$(cat "$CPU_SYSFS/cpu121/topology/core_id" 2>/dev/null)
    core122=$(cat "$CPU_SYSFS/cpu122/topology/core_id" 2>/dev/null)
    core123=$(cat "$CPU_SYSFS/cpu123/topology/core_id" 2>/dev/null)
    if [ -z "$pkg121" ] || [ -z "$pkg122" ] || [ -z "$pkg123" ] \
       || [ -z "$core121" ] || [ -z "$core122" ] || [ -z "$core123" ]; then
        log "门1: 拓扑文件缺失 — 拒绝上线"
        return 1
    fi
    if [ "$pkg121" != "$pkg122" ] || [ "$pkg122" != "$pkg123" ] \
       || [ "$core121" -ge "$core122" ] || [ "$core122" -ge "$core123" ]; then
        log "门1: cpu121/122/123 拓扑不符同簇序（pkg $pkg121/$pkg122/$pkg123, core $core121/$core122/$core123）— 编号漂移嫌疑, 拒绝上线"
        return 1
    fi
    log "门1: present/offline/拓扑预检通过（121-123 同簇, core_id $core121 < $core122 < $core123）"
    return 0
}

# ---------- 后验: MPIDR 核对 + 频率读数（不符自动下线） ----------
BAD_MPIDR=0x0900060200
postonline_verify() {
    local mpidr fmax fcur try
    # 后验1: 本启动日志中 CPU122 的 MPIDR 必须是坏核物理 ID（空结果小重试, journald 摄录延迟）
    mpidr=""
    for try in 1 2 3; do
        mpidr=$(journalctl -k -b 0 --no-pager 2>/dev/null \
                | grep "CPU$BAD_CORE: Booted secondary processor" | tail -n1)
        [ -n "$mpidr" ] && break
        sleep 0.3
    done
    if [ -z "$mpidr" ]; then
        log "后验1: journalctl 未见 'CPU$BAD_CORE: Booted' — 自动下线并中止"
        echo 0 > "$CPU_SYSFS/cpu$BAD_CORE/online" 2>/dev/null
        return 1
    fi
    if ! printf '%s' "$mpidr" | grep -q "$BAD_MPIDR"; then
        log "后验1: MPIDR 不符（期望 $BAD_MPIDR, 实际: $mpidr）— 编号漂移! 自动下线并中止"
        echo 0 > "$CPU_SYSFS/cpu$BAD_CORE/online" 2>/dev/null
        return 1
    fi
    log "后验1: MPIDR 核对通过（$mpidr）"
    # 后验2: 频率读数必须 ≤ cap（max 恰等 qos 值; cur 允许取整/过渡容差）
    fmax=$(cat "$CPU_SYSFS/cpu$BAD_CORE/cpufreq/scaling_max_freq" 2>/dev/null)
    fcur=$(cat "$CPU_SYSFS/cpu$BAD_CORE/cpufreq/scaling_cur_freq" 2>/dev/null)
    if [ -z "$fmax" ] || [ "$fmax" -gt $((MOD_CAP + 1000)) ]; then
        log "后验2: scaling_max_freq='$fmax' 超出 cap=$MOD_CAP — qos 疑失效, 自动下线并中止"
        echo 0 > "$CPU_SYSFS/cpu$BAD_CORE/online" 2>/dev/null
        return 1
    fi
    if [ -n "$fcur" ] && [ "$fcur" -gt $((MOD_CAP + 50000)) ]; then
        log "后验2: scaling_cur_freq='$fcur' 超出 cap+50MHz — 当前频率越界, 自动下线并中止"
        echo 0 > "$CPU_SYSFS/cpu$BAD_CORE/online" 2>/dev/null
        return 1
    fi
    log "后验2: 频率读数通过（max=$fmax cur=$fcur ≤ cap=$MOD_CAP）"
    log "后验2(信息): $(dmesg 2>/dev/null | grep 'cpu122-capfreq' | tail -n2 | tr '\n' ' ')"
    return 0
}
```

**(b) `case "$ACT"` 的 `1)` 上线分支替换为：**

```bash
1)  # ---- 上线: 门0+门1 → 上线（封顶由模块在上线路径内完成）→ 后验 ----
    module_ready            || die "模块未就绪, 拒绝上线（绝不裸奔）"
    cap_sanity_for_online   || die "cap 参数不合法, 拒绝上线"
    drift_preflight         || die "编号漂移预检未过, 拒绝上线"
    log "===== 主功能: CPU$BAD_CORE 【上线】 (online=1, cap=$MOD_CAP kHz 由模块在上线路径内实施) ====="
    err=$( { echo 1 > "$CPU_SYSFS/cpu$BAD_CORE/online"; } 2>&1 ) \
        || die "CPU$BAD_CORE 上线失败: $err"
    cur=$(cat "$CPU_SYSFS/cpu$BAD_CORE/online")
    [ "$cur" = "1" ] || die "CPU$BAD_CORE 状态验证失败: online=$cur（期望 1）"
    log "CPU$BAD_CORE 上线成功, online=1"
    postonline_verify       || die "后验未过, CPU$BAD_CORE 已自动下线, 中止"
    ;;
```

（横幅 `log` 引用 `MOD_CAP`，故置于三门全过之后——`module_ready` 成功才有 `MOD_CAP`。）

**(c) 下线分支末尾追加一行（在 `log "CPU$BAD_CORE 下线成功..."` 之后）：**

```bash
    log "提示: 模块封顶仍在值守, 下次上线自动生效（重启后由 modules-load.d 自动加载）"
```

**(d) 头部注释块（第 1–33 行）整体替换为：**

```bash
#!/bin/bash
# =============================================================================
# cpu122-online-offline.sh — CPU122（坏核）上线/下线/封顶控制 v2
#
# 用法:
#   sudo ./cpu122-online-offline.sh {上线|on|1|下线|off|0}
#   sudo ./cpu122-online-offline.sh {封顶|cap} <kHz>     # 0=解锁（高危）
#
# v2 安全模型（2026-10-10, 上线即封顶体系）:
#   前提    内核模块 cpu122_capfreq 已加载（kprobe cppc_set_perf 钳位 +
#           freq_qos MAX 挂 policy 跨热插拔存活）——满频 slam 在写硬件前即被
#           钳位, 封顶不再依赖用户态"看到 online=1 再降频"（那条链路架构性
#           必输: uevent 在 write() 返回前才广播, 实测上线后 ~10ms 即死）。
#   门0     模块未加载/cap 异常 → 拒绝上线/封顶（绝不裸奔上线）;
#           cap=0 拒绝上线; cap>2.9G 拒绝; cap>2.0G 高危警告放行。
#   门1     present≠0-127 / offline≠122 / cpu121-123 拓扑不符同簇序
#           → 编号漂移嫌疑, 拒绝上线（跨启动以 MPIDR 为准, 见配置总账 §6.4）。
#   后验1   journalctl 核对 CPU122 MPIDR == 0x0900060200, 不符自动下线。
#   后验2   scaling_max_freq/scaling_cur_freq ≤ cap, 越界（qos 疑失效）自动下线。
#   封顶    写模块 cap_khz, qos 自动传导, 在线切档免上下线（1.45G↔2.0G）。
#
# 每次执行前自动完成三项准备工作（均幂等, 已就绪则跳过）:   #  ← 原 v1 注释
#   准备1  检索并注释 udev 的 CPU 自动上线规则（防 maxcpus 失效）
#   准备2  GRUB 确保 maxcpus=122 panic=30（含 grubby 生效层同步）
#   准备3  上线除 CPU122 外的全部核
#
# 设计文档: docs/superpowers/specs/2026-10-10-cpu122-online-atomic-freqcap-design.md
# 模块正本: docs/cases/sdc1-01-04-core122/cpu122-capfreq/（构建/SOP/验证手册）
# =============================================================================
```

- [ ] **Step 4: 跑测试到全绿**

```console
bash tests/run_tests.sh; echo "exit=$?"
bash -n ../cpu122-online-offline.sh && echo "syntax OK"
```

预期：T2 + T3 全部断言 PASS（exit=0）。失败则修脚本（不是改测试）。

- [ ] **Step 5: 提交并推送**

```bash
git add docs/cases/sdc1-01-04-core122/cpu122-online-offline.sh \
        docs/cases/sdc1-01-04-core122/cpu122-capfreq/tests/run_tests.sh
git commit -s -m "feat: 脚本 v2 上线三门+后验——模块未就绪/编号漂移/MPIDR 不符自动拒绝

门0 模块就绪与 cap 边界（cap=0 拒绝, >2.9G 拒绝, >2.0G 高危警告）;
门1 present/offline/topology 编号漂移预检; 后验1 MPIDR==0x0900060200
不符自动下线; 后验2 频率读数越界（qos 疑失效）自动下线。
T2+T3 共 16 用例 43 断言全绿, bash -n 通过。

Signed-off-by: wangxu <wangxumarshall@qq.com>"
git push
```

---

### Task 4: 配置总账增补

**Files:**
- Modify: `docs/cases/sdc1-01-04-core122/SDC1-01-04-CPU122-BOOT-ISOLATION-CONFIG.md`

**Interfaces:**
- Consumes: Task 1–3 的最终形态（模块路径、脚本 v2 行为）。
- Produces: 与板上实态一致的配置总账（spec §3.4 文档同步义务）。

- [ ] **Step 1: 五处增补（精确内容如下）**

**(a) 文件头 blockquote 变更记录追加一行**（在现有 `> **变更记录**: 2026-10-09 深夜 vmcore...` 行之后）：

```markdown
> **变更记录**: 2026-10-10 新增上线即封顶体系——内核模块 `cpu122-capfreq`（kprobe+freq_qos 双拦截，时刻②③窗口归零）+ 脚本 v2 三门/后验/封顶子命令；设计见 `docs/superpowers/specs/2026-10-10-cpu122-online-atomic-freqcap-design.md`，模块正本 `docs/cases/sdc1-01-04-core122/cpu122-capfreq/`。
```

**(b) §0 一页结论的"规则"段**，将「实验需要坏核时，走 `cpu122-online-offline.sh` 受控通道（上线 + 降频封顶 + MPIDR 核对），重启后自动回到隔离态。」替换为：

```markdown
实验需要坏核时，走 `cpu122-online-offline.sh` v2 受控通道（模块 `cpu122-capfreq` 上线即封顶 + 门0/门1 预检 + MPIDR/频率后验，2026-10-10 起），重启后自动回到隔离态。
```

**(c) §4.1 末尾追加小节：**

```markdown
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
```

**(d) §4.3 末尾追加一段：**

```markdown
- **封顶机制 v2（2026-10-10 起）**：模块 `cpu122-capfreq` 在 CPU 上线路径内
  完成"首次 CPPC 写入即封顶 + governor 稳态封顶"，`scaling_max_freq` 的用户
  态补写不再是安全边界（降级为后验读数）；封顶跨 offline/online 自动保持，
  1.45G↔2.0G 切档走 `cpu122-online-offline.sh 封顶 <kHz>`，免上下线。
```

**(e) §6.6 末尾追加一段：**

```markdown
2026-10-10 起：模块 `cpu122-capfreq` 使"意外上线后满频裸奔"窗口从
用户态封顶链路（50–500ms，实测致死）缩小到 PSCI 热启动早期段（固件域，
历史实证低风险）；若模块被 `封顶 0` 显式解锁或卸载，恢复裸奔态——脚本
门0 会拒绝 cap=0 态的上线，但已在线期间的风险自担。
```

**(f) §8 文件—职责速查表追加三行**（表格末尾）：

```markdown
| `docs/cases/sdc1-01-04-core122/cpu122-capfreq/`（板上 `/home/sdc/cpu122-capfreq/`） | 上线即封顶内核模块正本（c/Makefile/README/deploy/tests） | 实验 |
| `/etc/modules-load.d/cpu122-capfreq.conf` + `/etc/modprobe.d/cpu122-capfreq.conf` | 模块开机自动加载与默认参数（target_cpu=122 cap_khz=1450000） | 实验 |
| `docs/superpowers/specs/2026-10-10-cpu122-online-atomic-freqcap-design.md` | 上线即封顶设计（三时刻/双拦截/验证 V1–V6/已排除方案） | 实验 |
```

- [ ] **Step 2: 核对一致性**

通读修改后的 §0/§4.1/§4.3/§6.6/§8，确认：与脚本 v2 实际行为逐条一致（门0/门1/后验/封顶参数边界）、无残留"上线后需手动封顶"旧表述（§4.3 首条"每次上线后必须重新封顶"保留为历史结论但已被 v2 段落覆盖说明——若读起来矛盾，在该句后加"（v2 起由模块在上线路径内自动完成）"）。

- [ ] **Step 3: 提交并推送**

```bash
git add docs/cases/sdc1-01-04-core122/SDC1-01-04-CPU122-BOOT-ISOLATION-CONFIG.md
git commit -s -m "docs: 配置总账增补上线即封顶体系（模块/三门/后验/封顶子命令）

§0/§4.1/§4.3/§6.6/§8 同步 v2 实态; 变更记录补 2026-10-10 行。

Signed-off-by: wangxu <wangxumarshall@qq.com>"
git push
```

---

### Task 5: progress-log 台账 + 收尾回归

**Files:**
- Modify: `docs/research/progress-log.md`

**Interfaces:**
- Consumes: Task 1–4 的全部产出与真实验证输出（构建日志、测试计数、提交哈希）。
- Produces: 台账条目（板上 V1–V6 待办挂账）。

- [ ] **Step 1: 文件末尾追加条目（沿用底部追加惯例）**

```markdown
## 2026-10-10（会话：CPU122 上线即封顶——满频致死竞态根因与模块方案）

### 现象与根因（v6.6 源码级, 逐条核实）
- 现象: 脚本上线 CPU122 后 ~10ms 整机卡死——uevent 未出、udev cap 脚本未跑,
  "先上线后降频"架构性必输。
- 根因三条:
  1) KOBJ_ONLINE uevent 在 write() 返回前才广播 → 用户态一切触发源都在
     风险窗口之后;
  2) cppc_cpufreq_cpu_init() 在 CPUHP_AP_ONLINE_DYN（上线路径内, CPU122
     自身执行, 早于 governor）直接 cppc_set_perf(desired=highest_perf
     =2.9G boost)——满频是内核亲手写上去的;
  3) 用户态无跨 offline/online 限值持久化通道（per-cpu policy 实证 +
     sysfs 离线不存在 + 无 freq_qos 接口）。
- 关键核实: cpufreq_notifier_max 为 schedule_work 异步 → CREATE_POLICY
  回调内挂 freq_qos 无死锁; 全部依赖符号在 6.6.0-159.4.x Module.symvers
  确认导出。

### 产出（分支 feat/cpu122-online-atomic-freqcap）
- 方案 A（用户裁定）: 模块 cpu122-capfreq——kprobe cppc_set_perf 钳位
  （时刻②归零）+ freq_qos MAX 挂 policy 跨热插拔存活（时刻③归零）。
  spec: docs/superpowers/specs/2026-10-10-cpu122-online-atomic-freqcap-design.md
- 仓内正本: docs/cases/sdc1-01-04-core122/cpu122-capfreq/（含 deploy/tests）
  + 脚本 v2（门0/门1/后验/封顶）+ 配置总账增补。
- 实证: 本机零警告构建通过（vermagic 159.4.3.154）; 行为测试 <N> 用例
  全绿（fake sysfs/假 journalctl, TDD 红→绿）; <M> 个提交逐一推送。

### 残余与缓解
- 时刻①（PSCI 热启动早期段, 固件域）不可归零: 历史全部死亡发生于负载期或
  满频稳态, 无一例在热启动早期段; 缓解 = 会话一次上线 + 错峰; 治本走
  BIOS/BMC per-core 限频（长期项）。

### 下一步
- [ ] 板上部署（kernel-devel 重编 + deploy conf 安装）→ V1 好核 123 演练
      → V3 真实上线, 结论回写本台账
- [ ] V6: 检索清除板上残留 udev cap 规则（保留防自动上线注释）
- [ ] 内核升级 SOP 增补"先重编模块再谈上线"（脚本门0 强制）
```

（`<N>`/`<M>` 执行时以实际数字填写；构建/测试输出贴关键行。）

- [ ] **Step 2: 收尾回归（全量真实验证）**

```console
cd docs/cases/sdc1-01-04-core122/cpu122-capfreq && bash tests/run_tests.sh; echo "exit=$?"
bash -n ../cpu122-online-offline.sh && echo "syntax OK"
cd ../../.. && git status --short && git log --oneline origin/main..HEAD
```

预期：测试 exit=0 全绿；`syntax OK`；`git status` 干净（无构建产物/临时文件）；分支上共 6 个提交（spec + 5 任务），全部已推送（`git log origin/main..HEAD` 与本地一致）。

- [ ] **Step 3: 提交并推送**

```bash
git add docs/research/progress-log.md
git commit -s -m "docs: progress-log 记录上线即封顶会话（根因/产出/板上 V1-V6 待办）

Signed-off-by: wangxu <wangxumarshall@qq.com>"
git push
```
