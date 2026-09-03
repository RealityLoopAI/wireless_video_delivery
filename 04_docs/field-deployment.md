# Field Deployment

更新时间：2026-09-03

本文面向出厂部署和维护人员，说明现场 DHCP 网络下的自动发现、NAS 容错和验收边界。它不是现场操作手册。

## Delivery Model

- Sender、Receiver 和配套 NAS 在出厂前安装完成，不做现场自动更新。
- Sender 由现场人员在设备桌面配置客户 Wi-Fi；Receiver 与 NAS 使用有线网络。
- 所有设备使用 DHCP，避免把实验室固定地址带到客户网络造成冲突。
- 现场只有一台 Receiver 和一台配套 NAS；测试网络允许存在多台 Receiver。
- Web 地址仍为 `http://<receiver-ip>:8080`，由现场人员查询 Receiver 当前地址。

## Receiver Discovery

Receiver 在 UDP `50009` 监听 `receiver_discovery_request`。Sender 每秒向各活动网卡的定向广播地址、有限广播地址和配置中的兜底地址发送请求，并使用响应包来源 IP 作为目标地址。

所有 Sender 传输实例共享一个线程安全目标：

```text
Receiver discovery
  -> shared ReceiverTarget
  -> status UDP
  -> media TCP / optional media UDP
  -> Web preview transport
  -> CLOCK_SYNC
```

切换目标时，已有媒体 TCP 主动关闭并连接新地址。Sender 将最后一次发现成功的 `receiver_id + IP` 原子保存到：

```text
$XDG_STATE_HOME/gwv3/receiver_target.json
或 ~/.local/state/gwv3/receiver_target.json
```

选择顺序：

1. 当前 Receiver 仍响应或媒体连接最近成功时保持不变。
2. 当前 Receiver 超过 `sticky_timeout_ms` 不可达后，选择已发现的其他 Receiver。
3. 没有发现响应时，使用配置文件 `receiver.ip` 兜底，并继续后台发现。

正式现场只有一台 Receiver，因此无需人工配对。测试环境存在多台时，持久化身份可避免正常运行中随机串台。

## NAS Discovery And Mount

NAS 运行 `nas_discovery_beacon.py`，在 UDP `50008` 返回稳定 `nas_id` 和 SMB share 名。响应只包含发现信息，不传输账号密码。

Receiver 的系统级 `nas_mount_manager.py`：

1. 自动发现唯一配套 NAS。
2. 使用本机 `/etc/gwv3/nas-credentials` 挂载 CIFS。
3. 持续执行有超时的写探针。
4. 将健康状态原子写入 `/run/gwv3/nas-mount-status.json`。
5. NAS DHCP 地址变化或旧挂载持续失效时，重新发现并挂载。

凭据文件只存在 Receiver 本机，权限必须为 `0600`，禁止提交 GitHub。

安装 NAS beacon：

```bash
sudo ./05_tools/install_nas_discovery_beacon.sh <smb-share-name>
```

安装 Receiver 自动挂载服务：

```bash
sudo ./05_tools/install_receiver_nas_auto_mount.sh 06_configs/receiver_loop.json
```

安装脚本会把同一 `nas_root` 的旧固定 IP `/etc/fstab` 项备份到 `/etc/fstab.gwv3-before-nas-discovery` 后移除，避免旧 automount 与自动发现服务竞争；若存在旧 `/etc/gwv3-nas-credentials`，会以 `0600` 权限迁移到新路径。只能在停止录制且 finalizer 已清空时执行这次迁移。

## Recording Failure Policy

生产配置使用本地 staging：

```text
camera packets
  -> Receiver per-camera reliable queue
  -> Receiver local staging
  -> recording_uploader
  -> NAS hidden capture queue
  -> atomic publication
```

- Receiver 启动时 NAS 不可用：服务和预览继续运行，但禁止开始新录制。
- 录制开始后 NAS 中断：当前录制继续写 Receiver 本地盘，NAS 恢复后自动补传。
- 本地空间低于 20%：进入容量警告/搬运压力状态。
- 本地空间低于 10%或低于绝对保留值 20 GiB：拒绝新录制；活动录制达到硬限制时安全停止，不删除未上传数据。
- 异常断电重启：恢复和补传已留存分片，但不自动恢复视频录制。
- Sender 到 Receiver 的局域网中断：保持现有策略，只自动重连，不在 Sender 缓存 RGB-D；断线期间在时间轴中形成可检测缺口。

## Offline Boundary

互联网中断时，采集、传输、录制、NAS、CLOCK_SYNC、固定提示音、按键和拍照继续工作。任意文本在线 TTS 允许不可用。软件更新只由维护人员主动执行，并保留可回退版本。

## Factory Acceptance

每套设备出厂前至少验证：

1. DHCP 地址变化后，Sender 在 2 分钟内重新发现 Receiver。
2. 测试网络有两台 Receiver 时，Sender 保持上次成功目标。
3. NAS 未启动时网页可访问、预览可用、开始录制被阻止。
4. 录制中断开 NAS，Receiver 继续生成本地分片且 uploader 不向未挂载目录写数据。
5. NAS 恢复或 DHCP 地址变化后自动挂载并清空 backlog。
6. 本地空间门槛同时按百分比和绝对字节生效。
7. 断电后只修复补传旧数据，不自动开始新视频录制。
8. 网络正常、相机已连接时，整机通电后 2 分钟内可预览、可录制。

当前网页不提供重启服务、恢复出厂或相机参数修改入口。单路相机异常不会阻止其他在线相机录制。
