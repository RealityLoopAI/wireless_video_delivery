# 设备初始化作业模板

更新时间：2026-09-03  
适用项目：`wireless_video_delivery`  
文档性质：初始化人员作业参考模板，交付前应由项目负责人补齐方括号内容

## 1. 目标与边界

本流程用于把已安装受支持 Linux 系统的设备初始化为以下角色之一：

- Sender：连接 Orbbec 相机，采集并发送 RGB/Depth。
- Receiver：接收、预览、录制并把文件搬运到 NAS。
- NAS：提供 SMB 存储和局域网自动发现。

初始化完成后应达到：

1. 设备接入客户局域网后通过 DHCP 获取地址，不使用实验室固定 IP。
2. Sender 自动发现唯一 Receiver，不要求现场填写 Receiver 地址。
3. Receiver 自动发现并挂载唯一配套 NAS。
4. Receiver 和 NAS 断线时保留本地数据，恢复后自动补传。
5. 断电恢复后服务自动启动，但视频录制必须由网页或按键重新手动开始。
6. 互联网中断时，除在线 TTS 外，采集、预览、录制、按键和 NAS 补传仍可工作。
7. 设备不启用自动软件更新。

初始化人员不要修改媒体协议、相机档位、曝光算法、文件格式或端口。配置与设备不匹配时应停止部署并联系项目负责人。

## 2. 当前项目默认值

以下内容已经按当前主线填写，定稿时只需核对，不应由初始化人员临时决定：

| 项目 | 当前标准值 |
| --- | --- |
| 项目版本 | GWV3 |
| 代码仓库 | `https://github.com/RealityLoopAI/wireless_video_delivery.git` |
| 正式分支 | `main`；交付时仍需冻结具体 commit |
| 现场网络 | 全设备 DHCP；Receiver/NAS 有线，Sender 优先 5 GHz Wi-Fi |
| 正式现场拓扑 | 1 台 Receiver + 1 台配套 NAS + 动态数量 Sender |
| Receiver 系统用户 | `loop` |
| Receiver 仓库路径 | `/home/loop/Desktop/wireless_video_delivery` |
| Receiver 配置 | `06_configs/receiver_loop.json` |
| Receiver 本地暂存 | `/home/loop/recording_staging` |
| Receiver NAS 挂载点 | `/home/loop/Desktop/nas` |
| Web Monitor | `http://<receiver-ip>:8080`，受信任局域网内无需令牌 |
| NAS SMB share | `video_database` |
| NAS SMB 用户 | `fzxl`，密码单独交付 |
| 视频切片 | 900 秒，即 15 分钟 |
| RGB 文件 | H.264 fragmented MP4，即 fMP4 |
| Depth 文件 | FFV1 MKV |
| 帧索引 | `frames.csv` |
| 完成标记 | `recording_ready.json` |
| Receiver 发现 | UDP `50009` |
| 主媒体 | TCP `50010` |
| 状态 | UDP `50011` |
| CLOCK_SYNC | UDP `50012` |
| Receiver 本地管理口 | HTTP `127.0.0.1:18080` |
| NAS 发现 | UDP `50008` |
| 自动更新 | 禁止 |

当前相机档位基线：

| Sender/相机系列 | Linux 用户 | RGB | Depth | 备注 |
| --- | --- | --- | --- | --- |
| LubanCat RK3576 + Gemini 305 | `cat` | `1280x800@30 MJPG` | `320x200@30 Y16` | Orbbec SDK `2.8.6`；MPP 硬编 |
| OrangePi 5 Pro + Gemini 305 | `orangepi` | `1280x800@30 MJPG` | `320x200@30 Y16` | 设备配置决定曝光与方向 |
| OrangePi 5 Pro + SV1301S | `orangepi` | `1920x1080@30 MJPG` | `320x200@30 Y12` | MPP 硬编 |
| 本机 RK3588 + SV1301S | `linaro` | `1920x1080@30 MJPG` | `320x200@30 Y12` | 当前正式单相机配置 |

上表是当前配置基线，不代表相机 SDK 声称支持的全部档位。初始化人员不得为了通过测试自行降低分辨率或帧率。

## 3. 初始化工单

开始前由负责人填写：

| 项目 | 填写内容 |
| --- | --- |
| 工单编号 | `[填写]` |
| 初始化日期 | `[实际作业日期；本模板日期为 2026-09-03]` |
| 操作人员 | `[填写]` |
| 设备角色 | `Sender / Receiver / NAS` |
| 设备型号 | `[填写]` |
| 资产编号 | `[填写]` |
| 主机名 | `[填写]` |
| Linux 用户名 | `[填写]` |
| 仓库绝对路径 | `[必须与 systemd service 一致]` |
| 发布 commit | `[40 位 Git commit，必须由负责人提供]` |
| Sender ID | `[Sender 填写，其他角色填 N/A]` |
| Camera ID | `[Sender 填写，通常为 cam01]` |
| 相机型号 | `[填写]` |
| Sender 配置文件 | `[例如 06_configs/sender_lubancat-xxxx_gemini305.json]` |
| Sender service | `[例如 gwv3-gemini-sender-lubancat-xxxx.service]` |
| NAS SMB share | `video_database` |
| 备注 | `[填写]` |

密码不写入本工单、Git 仓库、聊天记录或截图。密码由负责人通过受控方式单独提供。

## 4. 通用准备

### 4.1 硬件

- 使用项目指定电源，不使用临时充电器替代。
- Sender 的相机接入已确认的 USB 3.x 端口。
- Receiver 和 NAS 使用有线网络。
- Sender 可使用 Wi-Fi；优先连接 5 GHz，信号和频段必须实测。
- 初始化阶段准备显示器、键鼠和可访问 Git 仓库的网络。

### 4.2 系统检查

```bash
hostnamectl
uname -a
uname -m
ip -br address
ip route
df -h
timedatectl
```

验收要求：

- Sender 为项目批准的 ARM64 镜像；Receiver 为项目批准的 x86_64 镜像。
- 根分区空间充足，Receiver 本地暂存盘不得是 FAT、NTFS 或 NAS 挂载目录。
- 默认路由有效，DNS 可用。
- 时区可显示为 `Asia/Hong_Kong` 或等价 UTC+8；协议时间戳仍使用 Unix epoch microseconds。

### 4.3 安装发布版本

仓库路径由工单和 systemd service 决定。不要自行改变已有设备路径：

```bash
export GWV3_ROOT="<repository-absolute-path>"
git clone https://github.com/RealityLoopAI/wireless_video_delivery.git "$GWV3_ROOT"
cd "$GWV3_ROOT"
git fetch --all --prune
git checkout --detach <release-commit>
git status --short
git rev-parse HEAD
```

也可以使用负责人提供的离线 release bundle。无论采用哪种方式，必须满足：

- `git rev-parse HEAD` 与工单中的发布 commit 完全相同。
- `git status --short` 没有输出。
- 不从个人临时分支部署，不在设备上执行自动更新。

## 5. NAS 初始化

### 5.1 NAS 管理界面

1. 启用 SMB 服务。
2. 创建工单指定的 share，当前默认名为 `video_database`。
3. 创建项目专用 SMB 用户，并授予该 share 读写权限。
4. 确认 NAS 防火墙允许同一局域网访问 SMB 和 UDP `50008`。
5. 开启 SSH 仅用于初始化和维护；是否长期保留由负责人决定。

### 5.2 安装自动发现服务

在 NAS 的仓库根目录执行：

```bash
sudo ./05_tools/install_nas_discovery_beacon.sh video_database
```

检查：

```bash
systemctl is-enabled gwv3-nas-discovery.service
systemctl is-active gwv3-nas-discovery.service
systemctl --no-pager --full status gwv3-nas-discovery.service
```

三个结果应分别为 `enabled`、`active`，状态中不应持续重启。不得在 beacon 配置中保存 SMB 密码。

## 6. Receiver 初始化

当前标准 Receiver 用户名为 `loop`。如果用户名不是 `loop`，不能直接使用生产配置，必须先由研发生成对应路径和 UID/GID 的配置。

### 6.1 安装依赖

```bash
sudo apt-get update
sudo apt-get install -y \
  git cmake build-essential pkg-config \
  libjsoncpp-dev zlib1g-dev \
  ffmpeg python3 python3-venv python3-pip \
  cifs-utils curl chrony
```

### 6.2 检查 Receiver 配置

使用：

```text
06_configs/receiver_loop.json
```

重点确认：

- `nas_root` 是 `/home/loop/Desktop/nas`。
- `recording_staging.root` 是 Receiver 本地磁盘上的 `/home/loop/recording_staging`。
- `recording_staging.enabled=true`。
- `rgb_output_mode=fragmented_mp4`。
- `segment_seconds=900`。
- `receiver_discovery.enabled=true`。
- `nas_auto_mount.enabled=true`。

禁止把 `recording_staging.root` 放到 `nas_root` 内。

### 6.3 写入 NAS 凭据

凭据由负责人现场输入，不复制到仓库：

```bash
sudo install -d -m 0755 /etc/gwv3
sudoedit /etc/gwv3/nas-credentials
sudo chmod 0600 /etc/gwv3/nas-credentials
```

文件格式：

```text
username=<SMB 用户名>
password=<SMB 密码>
```

### 6.4 安装服务

```bash
cd /home/loop/Desktop/wireless_video_delivery
sudo ./05_tools/install_receiver_network_tuning.sh
sudo ./05_tools/install_receiver_nas_auto_mount.sh 06_configs/receiver_loop.json
./05_tools/install_receiver_autostart.sh 06_configs/receiver_loop.json
sudo loginctl enable-linger loop
sudo ./05_tools/setup_receiver_chrony_server.sh <customer-lan-cidr>
```

`<customer-lan-cidr>` 示例为 `192.168.1.0/24`，必须按现场网段填写，不能直接照抄示例。

### 6.5 Receiver 验收

```bash
systemctl is-active gwv3-nas-auto-mount.service
systemctl is-active gwv3-receiver-network-tuning.service
systemctl --user is-active gwv3-gemini-receiver.service
systemctl --user is-active gwv3-recording-uploader.service
systemctl --user is-active gwv3-web-monitor.service
systemctl --user is-active gwv3-photo-uploader.service
systemctl --user is-active gwv3-audio-archive.service
mountpoint /home/loop/Desktop/nas
test -w /home/loop/Desktop/nas
curl -fsS http://127.0.0.1:18080/api/status
```

查找 Receiver 当前 DHCP 地址：

```bash
hostname -I
```

在同一局域网另一台电脑访问：

```text
http://<receiver-ip>:8080
```

页面默认无需访问令牌。不要把 `8080` 端口暴露到公网。

## 7. Sender 初始化

当前仓库已有的正式设备对照如下。初始化已有设备时必须整行匹配；新增设备必须由研发先增加独立配置和 service。

| Sender ID | 用户 | 仓库路径 | 配置文件 | systemd service |
| --- | --- | --- | --- | --- |
| `rk3588-ubuntu` | `linaro` | `/home/linaro/桌面/wireless_video_delivery` | `sender_rk3588-ubuntu_one_camera.json` | `gwv3-gemini-sender-rk3588-ubuntu.service` |
| `orangepi5pro-b439137c` | `orangepi` | `/home/orangepi/Downloads/wireless_video_delivery` | `sender_orangepi5pro-b439137c.json` | `gwv3-gemini-sender-orangepi5pro-b439137c.service` |
| `orangepi5pro-f022c4` | `orangepi` | `/home/orangepi/Downloads/wireless_video_delivery` | `sender_orangepi5pro-f022c4.json` | `gwv3-gemini-sender-orangepi5pro-f022c4.service` |
| `orangepi5pro-fe0e946c` | `orangepi` | `/home/orangepi/Desktop/wireless_video_delivery` | `sender_orangepi5pro-fe0e946c_gemini305.json` | `gwv3-gemini-sender-orangepi5pro-fe0e946c.service` |
| `orangepi5pro-fe0f7222` | `orangepi` | `/home/orangepi/Downloads/wireless_video_delivery` | `sender_orangepi5pro-fe0f7222.json` | `gwv3-gemini-sender-orangepi5pro-fe0f7222.service` |
| `lubancat-4df661d7` | `cat` | `/home/cat/wireless_video_delivery` | `sender_lubancat-4df661d7_gemini305.json` | `gwv3-gemini-sender-lubancat-4df661d7.service` |
| `lubancat-52d2ef0c` | `cat` | `/home/cat/wireless_video_delivery` | `sender_lubancat-52d2ef0c_gemini305.json` | `gwv3-gemini-sender-lubancat-52d2ef0c.service` |
| `lubancat-e8cc0cb3` | `cat` | `/home/cat/wireless_video_delivery` | `sender_lubancat-e8cc0cb3_gemini305.json` | `gwv3-gemini-sender-lubancat-e8cc0cb3.service` |

### 7.1 必须先取得设备专用资料

负责人必须提供：

- 唯一 `sender_id`。
- 对应 `06_configs/<sender-config>.json`。
- 对应 `05_tools/systemd/<sender-service>.service`。
- 对应板卡的 Orbbec SDK 版本与目录。
- 相机型号、RGB/Depth 档位、方向和曝光策略。

仓库中没有该设备的配置或 service 时，初始化人员不得复制其他设备文件后自行改 ID。

### 7.2 安装依赖与 SDK

基础编译依赖：

```bash
sudo apt-get update
sudo apt-get install -y \
  git cmake build-essential pkg-config \
  libopencv-dev libjsoncpp-dev zlib1g-dev liblz4-dev libjpeg-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-tools chrony curl
```

把负责人提供的 Orbbec SDK 放入约定目录，具体规则见：

```text
11_third_party/README.md
```

Gemini 305 在当前 RK3576 生产镜像使用 Orbbec SDK `2.8.6`。不同板卡和镜像不能盲目复用该结论。

### 7.3 检查硬件与编码器

```bash
lsusb -t
lsusb
gst-inspect-1.0 mpph264enc
gst-inspect-1.0 mppjpegdec
```

要求：

- 相机实际枚举在 `5000M` 或更高的 USB 3.x 链路。
- `mpph264enc`、`mppjpegdec` 可被 GStreamer 正常加载。
- 只看到插件文件存在不算通过；出现 GStreamer ABI blacklist 必须先处理。

### 7.4 构建与测试

```bash
cd "$GWV3_ROOT"
ORBBEC_SDK_ROOT=<approved-sdk-path> cmake -S . -B 12_build \
  -DGWV3_BUILD_RECEIVER=OFF \
  -DGWV3_BUILD_SENDER=ON \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build 12_build -j2
ctest --test-dir 12_build --output-on-failure
./12_build/bin/gemini_sender \
  --config 06_configs/<sender-config>.json \
  --validate-config
./05_tools/sender_preflight.sh 06_configs/<sender-config>.json
```

任何一步失败都不能继续安装自启动。

### 7.5 时间与网络

Sender 自动发现 Receiver 的媒体地址，但 chrony 仍需一个可解析的 Receiver 地址。优先使用现场可稳定解析的 Receiver 主机名；没有可靠本地 DNS/mDNS 时，部署完成后按 Receiver 当前 DHCP 地址执行：

```bash
sudo ./05_tools/setup_sender_chrony_client.sh <receiver-ip-or-hostname>
sudo ./05_tools/install_sender_wifi_tuning.sh
```

如果 Receiver DHCP 地址以后变化且主机名无法解析，必须重新执行 chrony 客户端配置。媒体自动发现和 CLOCK_SYNC 不受该限制，但系统 NTP 状态会受影响。

Wi-Fi 检查：

```bash
nmcli -f GENERAL.CONNECTION,GENERAL.STATE,IP4.ADDRESS dev show
iw dev
iw dev wlan0 link
```

要求优先为 5 GHz、信号稳定、无持续高重传。初始化文档不得写死客户 SSID 或 BSSID。

### 7.6 安装 Sender 自启动

```bash
sudo install -m 0644 \
  05_tools/systemd/<sender-service>.service \
  /etc/systemd/system/<sender-service>.service
sudo systemctl daemon-reload
sudo systemctl enable --now <sender-service>.service
```

确认 service 使用的用户名、仓库路径和配置文件与工单一致：

```bash
systemctl cat <sender-service>.service
systemctl is-enabled <sender-service>.service
systemctl is-active <sender-service>.service
./05_tools/status_sender.sh 06_configs/<sender-config>.json
journalctl -u <sender-service>.service -n 100 --no-pager
```

单相机设备允许只按相机型号热插拔；一台 Sender 连接多台同型号相机时必须按序列号或稳定 UID 绑定。

语音、音频、GPIO 按键和 LED 属于设备选配项，只在工单明确要求时按 `04_docs/audio-and-controls.md` 安装，不能默认复制到所有 Sender。

## 8. 全系统联合验收

### 8.1 通电与发现

1. 先启动交换机/路由器和 NAS。
2. 启动 Receiver。
3. 启动所有 Sender。
4. 等待最多 2 分钟。
5. 打开 `http://<receiver-ip>:8080`。

每路应满足：

- `online=true`、`media_live=true`、`status_live=true`。
- RGB 和 Depth 档位与工单一致。
- 稳定运行时输入/发送帧率接近目标值；30 FPS 档位通常应稳定在约 29 至 31 FPS。
- `clock_sync_valid=true`。
- `sender_id + camera_id` 在系统内唯一。
- 预览方向、亮度和颜色符合样张。
- Receiver `recording_start_ready=true`。
- NAS 状态 `ready=true`，uploader 无历史积压。

### 8.2 短录制

1. 在网页点击“全部开始录制”。
2. 连续录制至少 60 秒。
3. 点击“全部停止录制”。
4. 等待 `record_finalize_outstanding_segments=0`。
5. 等待 `recording_uploader.pending_segments=0`。
6. 在 NAS 每个相机目录中确认出现 `*recording_ready.json`。

每路目录至少应包含：

```text
rgb.mp4
depth.mkv
frames.csv
meta.json
calibration.json
ffmpeg.log
recording_ready.json
```

音频只对配置了麦克风的 Sender 做要求。fMP4 的 `nb_frames=N/A` 本身不是失败，必须结合解码、时长、CSV 和 ready marker 判断。

检查工具：

```bash
ffprobe -v error <rgb.mp4>
ffprobe -v error <depth.mkv>
./05_tools/analyze_segment_fps.py <segment-directory>
```

`recording_ready.json` 应满足：

- `ready=true`。
- RGB/Depth 时间戳无倒退。
- 没有不可解释的长帧间隔。
- `recording_quality_status` 为 `complete`；若为 `partial`，必须记录具体 reason 并由负责人决定是否放行。

### 8.3 故障恢复抽测

至少完成：

1. Sender 重启后 2 分钟内自动恢复画面。
2. 相机重新插入后 sender service 不需要人工启动。
3. NAS 短时断开时禁止新录制，已有本地数据不被删除；NAS 恢复后自动补传。
4. Receiver 重启后服务和网页自动恢复，但不自动开始视频录制。
5. DHCP 地址变化后 Sender 重新发现 Receiver，Receiver 重新发现 NAS。

## 9. 常见失败与停止条件

| 现象 | 首要检查 | 处理原则 |
| --- | --- | --- |
| 网页打不开 | Receiver IP、`gwv3-web-monitor`、8080 端口 | 不修改 C++ admin 绑定地址 |
| 页面无 Sender | Wi-Fi、sender service、Receiver discovery UDP 50009 | 不先写死 Receiver IP |
| Sender active 但无画面 | `sender_preflight`、相机 SDK、USB、watchdog 日志 | `active` 不等于采集成功 |
| FPS 明显不足 | USB 速率、相机档位、温度、Wi-Fi、CPU | 不允许擅自降低档位 |
| NAS 未挂载 | NAS beacon、SMB、凭据、mount manager | 不把普通空目录当 NAS 使用 |
| 无法开始录制 | `recording_start_ready` 和 block reason | 不绕过存储门禁 |
| 停止后文件未出现 | finalizer、uploader、NAS pending | 不强杀 Receiver/uploader |
| `recording_quality_status=partial` | ready marker 的 reason、CSV、ffprobe | 不标记为验收通过 |
| `clock_sync_valid=false` | chrony、UDP 50012、网络抖动 | 不用于多机时间对齐验收 |

出现以下任一情况必须停止交付：

- 设备没有负责人指定的配置或 service。
- Sender ID/Camera ID 与其他设备重复。
- 运行二进制 `build_dirty=true`，或 commit/source hash 与发布记录不一致。
- 相机只能在 USB 2.0 枚举，而工单要求 USB 3.x。
- 短录制无法解码、无法拖动、缺少 CSV/ready marker，或 NAS 有未解释积压。
- 服务持续重启、设备过热降频、Wi-Fi 长时间高延迟或大量重传。

## 10. 交付记录

| 验收项 | 结果 | 证据/备注 |
| --- | --- | --- |
| 发布 commit 正确、工作区干净 | `通过/失败` | `[填写]` |
| DHCP 与默认路由正常 | `通过/失败` | `[填写]` |
| NAS beacon active | `通过/失败/N/A` | `[填写]` |
| Receiver 全部服务 active | `通过/失败/N/A` | `[填写]` |
| NAS 自动挂载、可写 | `通过/失败/N/A` | `[填写]` |
| Sender SDK 与硬编可用 | `通过/失败/N/A` | `[填写]` |
| Sender 自启动与热插拔恢复 | `通过/失败/N/A` | `[填写]` |
| RGB/Depth 档位正确 | `通过/失败/N/A` | `[填写]` |
| FPS、温度与网络稳定 | `通过/失败` | `[填写]` |
| CLOCK_SYNC 有效 | `通过/失败/N/A` | `[填写]` |
| 60 秒录制完整 | `通过/失败` | `[NAS 相对路径]` |
| 断电/断网恢复抽测 | `通过/失败` | `[填写]` |
| 网页无需令牌且局域网可访问 | `通过/失败/N/A` | `[填写]` |

操作人员签字：`[填写]`  
复核人员签字：`[填写]`  
交付日期：`[填写]`

## 11. 负责人定稿前应修改的内容

1. 把工单示例替换为公司的实际资产字段和签字流程。
2. 明确每种板卡的标准系统镜像、用户名、SDK 包和恢复包位置。
3. 为每个产品 SKU 列出唯一配置、service、相机档位、曝光和选配功能。
4. 明确客户网络的开放端口、VLAN、是否支持本地 DNS/mDNS。
5. 明确 NAS 型号、share、容量门槛、账号发放和 SSH 保留策略。
6. 指定正式发布 commit、回退 commit、验收样张和允许的性能范围。
7. 删除初始化人员不需要执行的选配章节，不要保留模糊的可选命令。
