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

	/* policy 销毁（设备移除 / cpufreq_online 失败路径，cpufreq_policy_free
	 * 前广播）时精确回收 qos 请求，避免请求悬挂于已释放 constraints（UAF）；
	 * 同时复位 qos_active，使同 CPU 重建 policy 时 CREATE_POLICY 重新挂载
	 * （评审 I3）。 */
	if (event == CPUFREQ_REMOVE_POLICY && policy->cpu == target_cpu) {
		mutex_lock(&state_lock);
		if (qos_active) {
			freq_qos_remove_request(&qos_req);
			qos_active = false;
			pr_info(LOGTAG "policy for CPU%u destroyed, qos request removed\n",
				target_cpu);
		}
		mutex_unlock(&state_lock);
		return NOTIFY_DONE;
	}

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

/* 换算状态只读暴露：脚本门0 据此拒绝"cap_perf=0（kprobe 钳位未武装）"态的
 * 上线——加载期换算失败时 CREATE_POLICY 兜底重试晚于驱动 init slam，
 * 首次上线的时刻②仅 qos 在场、kprobe 直通，不能放行（评审 I1）。 */
module_param(cap_perf, uint, 0444);
MODULE_PARM_DESC(cap_perf, "cap in CPPC perf units (0 = conversion pending, kprobe clamp inert)");

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

	/* 迟加载兜底：本启动 target 已在线（policy 已存在）→ 直接挂 qos；
	 * 失败即拒载并清理已注册项（spec §4.5，评审 I4） */
	policy = cpufreq_cpu_get(target_cpu);
	if (policy) {
		mutex_lock(&state_lock);
		if (!qos_active) {
			ret = freq_qos_add_request(&policy->constraints, &qos_req,
						   FREQ_QOS_MAX, qos_value());
			if (ret < 0) {
				mutex_unlock(&state_lock);
				cpufreq_cpu_put(policy);
				pr_err(LOGTAG "late-bind freq_qos_add_request failed: %d, refusing load\n",
				       ret);
				goto err_late_bind;
			}
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

err_late_bind:
	cpufreq_unregister_notifier(&policy_nb, CPUFREQ_POLICY_NOTIFIER);
	unregister_kprobe(&kp_cppc_set_perf);
	return ret;
}

static void __exit cpu122_capfreq_exit(void)
{
	/* 先注销 notifier/kprobe（blocking 注销会等待在途回调返回），再复查
	 * qos：CREATE_POLICY 若在"早前移除之后、notifier 注销之前"的窗口内
	 * 重新挂载，此处补刀回收——关闭 rmmod 与并发上线竞态的 UAF 窗口
	 * （评审 I2）。 */
	cpufreq_unregister_notifier(&policy_nb, CPUFREQ_POLICY_NOTIFIER);
	unregister_kprobe(&kp_cppc_set_perf);
	mutex_lock(&state_lock);
	if (qos_active) {
		freq_qos_remove_request(&qos_req);
		qos_active = false;
	}
	mutex_unlock(&state_lock);
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
