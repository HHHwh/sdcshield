# 2026-10-10 monitor 联锁传感器名板侧适配（THERMAL_SENSORS / FAN_SENSORS）

## 背景

sdc1-01-02 机（Yangtze R240K V2，192C，SN 71006932A034）入役核查发现
`sdc_monitor.sh` 的两个安全联锁读取参考机（TaiShan 2280）的固定 SDR 传感器名，
在该板全部落空：

| 联锁 | 固定名 | R240K V2 实际命名 | 后果 |
|---|---|---|---|
| 热联锁（PAUSE 95 / RESUME 90 / KILL 100） | `CPU1/2 Core Rem` | `CPU1_TEMP`..`CPU4_TEMP` | `maxt` 恒 0 → **联锁全盲**，永不 PAUSE/KILL |
| 风扇联锁（缺失/0rpm=异常，3 采样 PAUSE） | `FAN2/FAN3 Speed` | `FAN1-4 F/R Speed`（8 个） | 读空=缺失 → monitor 启动 3 周期内**永久 fan PAUSE**，战役无法运行 |

v3 发现式列集（monitor.csv 数据列）本就板侧自适应；缺陷仅在联锁段的固定名。
该板 CPU1 空载 80°C / BMC 门限 105°C、且 SEL 有越限史——恰恰是最需要热联锁
真实生效的板（用户已知悉热态并批准带保守联锁推进）。

## 方案（默认行为不变）

新增两个 env 可配项（monitor 启动时读取；systemd drop-in `Environment=` 或
campaign.env source 均可提供）：

- `THERMAL_SENSORS`：冒号分隔的 CPU 温度传感器名（`sdr_raw` 正则语义），
  取值并入 `maxt`，参与全部既有阈值逻辑（88 快采样 / PAUSE / RESUME / KILL）。
  默认空 = 不新增，参考板 c1/c2 路径原样保留。
- `FAN_SENSORS`：冒号分隔的风扇传感器名，任一缺失/0rpm 视为异常（保持
  "缺失即异常"的安全方向），连续 3 采样 PAUSE 语义不变。
  默认 `FAN2 Speed:FAN3 Speed` = 参考板现行为；**置空 = 显式关闭风扇联锁**
  （无风扇读数板卡记录在案的选择，不是静默失效）。

冒号分隔是为了容纳含空格的传感器名（`FAN2 F Speed`）。快照段的 c1/c2/f2/f3
等固定名读取保持原样（纯展示字段，不影响联锁；该板真实数据经 v3 发现式列集
完整入 monitor.csv）。

## 部署（R240K V2）

systemd drop-in（install.sh 重装单元文件不覆盖 drop-in 目录，比 sed 注入
单元文件更稳——后者在 onboarding 第 4 步 PMU 注入处已知会随重装丢失）：

```
/etc/systemd/system/sdc-monitor.service.d/r240k-v2-sensors.conf
  [Service]
  Environment="THERMAL_SENSORS=CPU1_TEMP:CPU2_TEMP:CPU3_TEMP:CPU4_TEMP"
  Environment="FAN_SENSORS=FAN1 F Speed:FAN1 R Speed:FAN2 F Speed:FAN2 R Speed:FAN3 F Speed:FAN3 R Speed:FAN4 F Speed:FAN4 R Speed"
```

## 验证

1. `bash -n`
2. `MON_SDR_FILE` / `MON_SELFTEST` 测试钩子（不打真 BMC）：
   - T1 本板假 SDR（CPU1_TEMP=96 + 健康风扇）+ 配置名 → thermal PAUSE 触发
   - T2 同输入、默认 env（不配 THERMAL_SENSORS）→ 无 PAUSE（复现修复前的盲缺陷）
   - T3 假 SDR 缺全部风扇名 + 配置 FAN_SENSORS → 连续 3 周期后 fan PAUSE
     （`timeout 130` 跑 3 个采样周期，无 MON_SELFTEST；SEL 段按真 root 通道
     只读一次，无副作用）
3. 真机一次 `ipmitool sdr list` 核对 `sdr_val` 对 `CPU1_TEMP`/`FAN2 F Speed`
   的实际取值（确认 drop-in 配置名与 sdr list 输出命名一致）
4. 回归：`-e zstd19 -t 2000 -n 1` → `exit: pass`

## 安全边界

- 自测假 SDR 温度用 96°C（< KILL 线 100、单周期 hot=1）——绝不触发 KILL 分支
  的 `pkill`（当时可能有真实 sdcshield/stress-ng 在跑）。
- 默认值与参考板逐位等价：不配 env 时 c1/c2 路径与 FAN2/FAN3 行为零变化。
