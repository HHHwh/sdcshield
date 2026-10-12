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
- 失败即拒载：kprobe / cpufreq notifier 注册失败、迟加载（late-bind）qos
  添加失败 → 模块加载失败（宁可不上线，不可裸奔上线；脚本门0 拦截失载）。
- qos 挂载失败于运行期（CREATE_POLICY 返回值被 core 忽略，无法中止上线）→
  dmesg `qos UNARMED` ERROR + 脚本后验2 频率读数兜底并自动下线。
- policy 销毁（CPUFREQ_REMOVE_POLICY）时精确回收 qos 请求并复位状态，
  同 CPU 重建 policy 时自动重挂——无悬挂 UAF、无"skip re-add 失保护"窗口。
- `cap_perf` 只读参数暴露 kHz→perf 换算状态；脚本门0 拒绝 `cap_perf=0`
  （kprobe 钳位未武装，首次 slam 将直通）态的上线。

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
