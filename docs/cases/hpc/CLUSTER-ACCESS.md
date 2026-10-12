# 计算集群访问指南(NSCC 登录节点 + 计算节点)

> 写给 Claude:如何在 NSCC 集群上定位自己、进入计算节点并直接操作。
> 写于 2026-09-11,实测验证。本地 Windows 副本见 agentreach 仓库 docs/CLUSTER-ACCESS.md。

## 0. 你在哪里

当前 session 通过 reach 连接远程目标:**Bash 工具不在本地 Windows 执行,而在登录节点 login01 上执行**。
不要用 Read/Write/Edit 等本地文件工具操作远程文件。远程文件用 shell 命令访问。

## 1. 环境拓扑

```
本地 Windows (Documents\agentreach 仓库,reach.exe)
    │   reach 拦截 Bash,经 SSH 免密连接(ssh_config 别名 nscc)
    ▼
登录节点 nscc = 10.39.0.1 (login01, aarch64, Kylin V10)
    │   dattach -c '命令' <jobid>
    ▼
计算节点 cn23154 = 10.36.181.114 (608 核, 565GB 内存)
```

- 登录后默认工作目录:`/home/share/suke`(家目录是 /home/share/suke,不是 /home/suke)
- `/home/share` 是登录/计算节点共享 NFS(2.5PB)

## 2. 本地启动方式

```powershell
cd C:\Users\ubuntu\Documents\agentreach
.\reach.exe nscc claude
```

`~/.ssh/config` 已有 `nscc` 别名(10.39.0.1,用户 suke,密钥 id_ed25519 免密)。
sshd 旧版本每次连接打印 post-quantum 警告横幅,可忽略。
若 reach 启动卡 2 分钟,是 seam 探测超时,本地代码已修复(probe.go 的 CLAUDE_CONFIG_DIR)。

## 3. 进入计算节点

### 3.1 先查 RUNNING job

```bash
djob    # 看 STATE 和 EXEC_NODES 列
```

已有 RUNNING、EXEC_NODES 为 cn23154 的 job → 直接用其 jobid 走 3.3,
**不要重复提交**(节点被独占,新 job 只会 PENDING)。

### 3.2 没有则提交

交互式 job 需要持续 TTY,Claude 的独立命令连接维持不了。二选一:

```bash
dsub -rpn 608 -q q_Test_20260903 -nl cn23154 -I bash        # 用户终端手动保持
dsub -rpn 608 -q q_Test_20260903 -nl cn23154 'sleep infinity'  # 长驻后台 job
```

然后 `djob` 查 jobid。

### 3.3 在计算节点执行命令

```bash
dattach -c '命令' <jobid>
```

输出开头有 `<<<attaching to the node cn23154 of job ...>>>`,即确认在计算节点。

## 4. 特性与限制

- `dattach -c` 无持久 shell,cd/export 不跨命令;用绝对路径
- 文件持久(共享 NFS);计算节点上 `nohup ... &` 随 job 存活
- 用户 TTY 的 bash 和 dattach 互不可见对方环境
- dsub 的 `Replicas_per_node needs to work with replica...` 警告无害
- 每条命令约 1-2 秒开销(SSH 重连 + dattach)

## 5. 关键路径

- 家目录:`/home/share/suke`;项目:`/home/share/suke/wangxu/sdcshield-main`
- 暂存:`/work_hdd/scratch`(100PB);软件区:`/work_ssd/software`
- 架构:**aarch64 鲲鹏**

## 6. 速查

```bash
djob / djob -l <jobid> / dattach -c 'cmd' <jobid> / dkill <jobid>
```
