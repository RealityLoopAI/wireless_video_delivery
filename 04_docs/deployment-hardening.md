# Deployment Hardening

本文说明设备部署、断电重启和现场验收中已确认的故障模式，以及当前主线用什么机制防止复发。具体初始化命令见 [device-initialization.md](device-initialization.md)，现场 DHCP 与 NAS 行为见 [field-deployment.md](field-deployment.md)。

## Confirmed Root Causes

| Symptom | Root cause | Current control |
| --- | --- | --- |
| Sender 进程存活但相机零帧后不能自愈 | watchdog 过去读取 `sender_stdout.log`，应用实际写 `sender.log` | watchdog 从生效配置解析真实应用日志，并跟踪日志轮转后的 inode |
| 校时后 watchdog 错误重启或超时异常 | 健康超时过去使用可跳变的系统墙钟 | 健康、启动和 Wi-Fi 宽限计时使用 `/proc/uptime` 单调时间 |
| 重启后连上 2.4 GHz，Sender 一直不启动 | 系统允许 2.4 GHz 自动连接，watchdog 又要求至少 5 GHz；未指定连接名时旧逻辑不会主动选择合格网络 | Wi-Fi guard 从已保存且可见的连接中选择 5 GHz，双频配置明确限定 A 频段，并关闭省电 |
| 时间戳突然前跳或倒退 | Chrony 与 `ntpdate`、`sntp`、`date -s` 或其他 NTP 服务同时改墙钟 | 安装时审计并停用冲突服务和 cron；Chrony 只允许启动阶段 step，运行阶段 slewing；Sender 启动前做一次有上限的收敛等待 |
| 换现场后仍访问旧 Receiver | IP 分散在配置、systemd、按键、LED 和音频归档参数中 | 正式安装只生成 `/etc/gwv3/sender.json`；媒体自动发现，按键/LED/关机逻辑读取同一持久化目标，音频归档启动时解析该目标 |
| 同一设备存在多套 Sender service | 每增加设备就复制一份写死用户名、路径和配置的 unit | 正式部署只启用 `gwv3-gemini-sender.service`，运行用户和路径来自 root 管理的 `/etc/gwv3/sender.env` |
| 现场修改无法确认或回退 | 仓库 dirty、detached HEAD、模板与生效配置混用 | 安装器默认拒绝 dirty worktree，记录 commit 与配置 SHA-256，安装前备份并支持自动/手动回退 |
| 只看到 `active` 就认为交付完成 | 进程、按钮和网页状态不能证明文件完整 | `gwv3_doctor.sh` 做只读整机诊断；显式短录制验收检查每路启动、收尾、错误计数和 NAS 搬运清空 |

## Release And Installation Contract

正式 Sender 只允许从已提交的仓库版本安装：

```bash
sudo ./05_tools/install_device.sh sender \
  --config 06_configs/<sender-config>.json \
  --run-user <linux-user> \
  --receiver-fallback <receiver-ip-or-hostname> \
  --chrony-server <receiver-ip-or-hostname>
```

安装过程执行以下事务：

1. 检查 Git 状态，构建并校验二进制和设备配置。
2. 备份原配置、发布记录、Chrony 配置、unit、launcher、doctor 和 Sender service 启停状态。
3. 生成唯一生效配置 `/etc/gwv3/sender.json`。
4. 写入 `/etc/gwv3/release.json`，记录 commit、配置哈希、来源配置和安装时间。
5. 清理冲突校时机制，安装通用 service，停用旧 Sender service。
6. 任一步失败时自动恢复安装前文件和 service 状态。

手动回退最近一次安装：

```bash
sudo ./05_tools/rollback_sender_install.sh
```

指定备份回退：

```bash
sudo ./05_tools/rollback_sender_install.sh /var/backups/gwv3/sender-<id>
```

## Boot And Recovery Behavior

| Event | Expected behavior |
| --- | --- |
| 冷启动时网络较慢 | systemd 等待 NetworkManager；watchdog 持续选择满足策略的 Wi-Fi 并重试，不需要人工重启服务 |
| Receiver 晚启动或 DHCP 地址变化 | Sender 保持采集/重连，通过 UDP 发现更新共享 Receiver 目标 |
| 短时 Wi-Fi 中断 | 宽限期内保留 Sender 子进程和可靠媒体队列；恢复后 TCP 重连并继续发送 |
| 相机拔出再插入 | hotplug 逻辑重新枚举；持续零帧或卡在启动状态时 watchdog 重启子进程 |
| NAS 暂时不可用 | Receiver 本地 staging 保留数据并禁止不安全的新录制；NAS 恢复后自动补传 |
| Sender 断电重启 | service、相机和传输自动恢复；断电前内存中的媒体不可恢复，时间轴会有可检测缺口 |
| Receiver 断电重启 | 修复并补传已落地数据，恢复网页和接收服务；不会自动恢复断电前的录制任务 |

设备或 Receiver 重启期间无法保证当前录像无缝连续。现有产品策略是保留已写数据、明确产生新分段和缺口、恢复后等待现场人员重新开始录制，而不是伪造连续时间轴。

## Acceptance Evidence

部署后先运行只读诊断：

```bash
./05_tools/gwv3_doctor.sh sender /etc/gwv3/sender.json
```

诊断必须核对：发布 commit、dirty 状态、配置哈希、Chrony、冲突校时机制、service、自启动、USB 3.x、5 GHz、关闭 Wi-Fi 省电、Receiver 路由和最新采集 FPS。

Receiver 空闲且现场允许明确触发录制时，再执行：

```bash
python3 05_tools/run_deployment_acceptance.py \
  --admin http://127.0.0.1:18080 \
  --record-seconds 60 \
  --output 08_reports/deployment-acceptance.json
```

脚本拒绝干扰已有录制，只对开始时在线的相机验收。通过条件包括：每路进入录制、停止后 finalizer 清空、录制队列清空、NAS uploader 无 pending/active、每路完成分片计数增加、写错误不增加且相机仍在线。

自动脚本不能替代最终文件抽检。交付前仍需对 NAS 中带 `recording_ready.json` 的目录执行 `ffprobe`、CSV 单调性和实际播放检查，并完成一次整机断电重启与网络恢复抽测。

## Remaining Boundaries

- 软件时间统一不能替代相机硬件曝光同步；多机内容级对齐仍需使用 `global_timestamp_us` 加离线内容补偿。
- Sender 不保存断网期间的完整 RGB-D 数据，局域网长时间中断会形成缺口。
- 音频归档目标在语音服务启动时解析；Receiver 地址在服务运行期间变化时，应重启语音服务或等待设备重启。
- 配置模板仍可包含实验室 fallback；正式生效地址只由安装参数写入 `/etc/gwv3/sender.json`，初始化人员不得搜索源码批量替换 IP。
- 5 GHz 可见不等于无线质量合格；高重传、弱信号和 AP 空口拥塞仍需现场网络验收。
