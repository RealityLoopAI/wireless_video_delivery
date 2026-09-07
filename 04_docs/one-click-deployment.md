# One-Click Deployment

更新时间：2026-09-07

本文是新设备初始化的唯一入口说明。部署脚本面向已经安装项目批准 Ubuntu 镜像的空设备；NAS 使用绿联系统，只配置 SMB，不在 NAS 上安装本项目。

## 1. Supported Baselines

| Role | Hardware | Operating system | Architecture |
| --- | --- | --- | --- |
| Sender | RK3588 / Orange Pi 5 Pro / RK3576 LubanCat | Ubuntu 22.04 | arm64 |
| Receiver | PC | Ubuntu 24.04 | x86_64 |

脚本不设置统一的磁盘容量下限。Receiver 仍按配置中的剩余空间百分比和保留空间做运行时保护：空间不足时拒绝新录制，保留已有文件并等待 NAS 恢复，不能用“删旧数据”换取继续录制。

当前自动识别的组合：

| Board | Camera | SDK | Configuration source |
| --- | --- | --- | --- |
| RK3588 | SV1301S | Orbbec SDK 1.10.27 | `sender_rk3588-ubuntu_one_camera.json` |
| Orange Pi 5 Pro | SV1301S | Orbbec SDK 1.10.27 | `sender_orangepi5pro-fe0f7222.json` |
| Orange Pi 5 Pro | Gemini 305 | Orbbec SDK 2.8.6 | `sender_orangepi5pro-d12a4719_gemini305.json` |
| RK3576 LubanCat | Gemini 305 | Orbbec SDK 2.8.6 | `sender_lubancat-e8cc0cb3_gemini305.json` |

配置选择只按板型和相机型号，不按相机序列号绑定。未知板卡或相机不会猜测配置，交互模式会要求人工选择，非交互模式直接失败。

## 2. Online Installation

联网空设备执行：

```bash
curl -fsSLO https://raw.githubusercontent.com/RealityLoopAI/wireless_video_delivery/field-v2026.09.07.4/05_tools/bootstrap_online.sh
chmod +x bootstrap_online.sh
./bootstrap_online.sh --role sender
```

Receiver 使用：

```bash
./bootstrap_online.sh --role receiver
```

在线入口只克隆冻结的 release tag，不会更新已经存在的安装目录。以后升级必须由维护人员显式执行另一个发布流程。

## 3. Offline Installation

离线包必须在与目标相同的系统和架构上生成：

```bash
sudo apt-get install -y apt-rdepends jq
./05_tools/build_offline_bundle.sh sender /path/to/usb
./05_tools/build_offline_bundle.sh receiver /path/to/usb
```

把对应压缩包解压到 U 盘。目标设备运行包内同一个入口：

```bash
./install.sh --role sender
# 或
./install.sh --role receiver
```

离线包包含冻结源码、apt 依赖、Python wheel、官方 Orbbec SDK；Sender 包还包含 Vosk 中文模型。每个下载资产都由清单固定版本，Orbbec SDK 和语音模型在安装前校验 SHA256。

## 4. Sender Flow

Sender 现场只需要先连接一个可用 Wi-Fi，然后运行安装器。安装器自动完成：

1. 检查 Ubuntu 22.04 arm64。
2. 从设备树、USB 和 ALSA 识别板卡、相机、USB 速率与选配硬件。
3. 从硬件序列或永久 MAC 生成稳定、唯一的 `sender_id`，并同步设置主机名。
4. 通过 UDP `50009` 自动发现 Receiver；广播不可达时尝试 `gwv3-receiver.local`，最后才询问人工兜底地址。
5. 按批准 profile 生成 `/etc/gwv3/sender.json`，删除相机序列号/UID 严格绑定并启用热插拔。
6. 安装正确代际的官方 Orbbec SDK，编译并安装唯一的 `gwv3-gemini-sender.service`。
7. 配置 Chrony、CLOCK_SYNC、Wi-Fi 省电关闭和 Receiver 地址变化跟随。
8. LubanCat 检测到项目 ADC/GPIO 后自动生成按键、电源键与 LED 配置；检测到批准的 USB 麦克风/音箱组合后安装离线唤醒、拍照和 TTS 服务。

默认保留 profile 里的相机方向。确需覆盖时使用：

```bash
./05_tools/bootstrap.sh --role sender --rotation 180
```

需要保留已批准的历史 Sender ID 时使用 `--sender-id`。新增设备不要手工复制其他设备配置文件。

多个备用 Wi-Fi 可预先保存，密码放在本机临时文件中，不写命令行历史或 Git：

```bash
chmod 600 /path/to/wifi-password
./05_tools/bootstrap.sh --role sender \
  --wifi-ssids 'site-ap-a,site-ap-b' \
  --wifi-password-file /path/to/wifi-password
```

安装器不会为了 RSSI 波动频繁切 AP。NetworkManager 只在当前连接失效后，从已保存且可见的 profile 自动恢复。

## 5. Receiver Flow

Receiver 接网线并运行安装器。首次部署只询问：

- NAS 当前 IP 或主机名；
- SMB share；
- SMB 用户名和密码。

安装器自动完成：

1. 检查 Ubuntu 24.04 x86_64。
2. 按实际用户 home、UID 和 GID 生成 `/etc/gwv3/receiver.json` 与音频配置。
3. 将 SMB 凭据写入仅 root 可读的 `/etc/gwv3/nas-credentials`，不会写入仓库或日志。
4. 记录 NAS 地址和同网段可见的物理 MAC；以后 DHCP 地址失效时通过 beacon 或 MAC 扫描重新定位。
5. 安装 Receiver、Web、上传器、照片、音频、NAS 挂载、网络调优和日志轮转服务。
6. 启用 Receiver Chrony 基准和 mDNS 主机名 `gwv3-receiver.local`。
7. NAS 中断时继续保留本地已录数据，恢复后自动补传；空间达到保护线时停止接受新录制。

NAS 不需要项目脚本。必须在绿联管理界面启用 SMB、建立唯一 share，并给项目账号读写权限。

## 6. Reboot And Acceptance

安装结束默认重启一次。启动后 `gwv3-post-install-verify.service` 会生成：

```text
/var/lib/gwv3/deployment-report.json
/var/lib/gwv3/deployment-report.txt
```

Sender 验证服务、相机、USB、帧率、网络、Chrony 和 CLOCK_SYNC。Receiver 在存在在线相机时执行 30 秒全路录制，验证启动、视频/CSV、收尾和 NAS 搬运；成功后只按该次 `recording_session_id` 删除测试目录。没有在线 Sender 时会明确记录 `SKIPPED`，不把空系统误判为录制成功。

调试时可以跳过重启：

```bash
./05_tools/bootstrap.sh --role sender --no-reboot
sudo systemctl start gwv3-post-install-verify.service
```

查看状态：

```bash
sudo cat /var/lib/gwv3/deployment-report.txt
./05_tools/gwv3_doctor.sh sender /etc/gwv3/sender.json
./05_tools/gwv3_doctor.sh receiver /etc/gwv3/receiver.json
```

## 7. Failure Rules

- Sender 安装是事务式的，失败时使用 `/var/backups/gwv3/sender-*` 自动恢复旧配置和服务。
- 已有 `$HOME/wireless_video_delivery` 时，在线/离线入口都拒绝自动覆盖。
- SDK、模型或离线包校验失败时停止安装，不使用未知文件继续运行。
- Receiver/NAS 不可达不会删除本地录制。
- 断电重启只修复和补传中断前的数据，不自动开始新录制。
- 互联网中断只影响首次下载和在线 Edge TTS；采集、录制、固定提示音、离线唤醒和 NAS 补传不依赖互联网。

## 8. Non-Interactive Example

自动化工装应把密码放在权限为 `0600` 的临时文件中：

```bash
./05_tools/bootstrap.sh --role receiver --non-interactive --no-reboot \
  --nas-host 192.168.1.89 \
  --nas-share video_database \
  --nas-user capture \
  --nas-password-file /run/secrets/nas-password
```

密码文件由工装在部署后删除。禁止使用命令行明文密码参数。
