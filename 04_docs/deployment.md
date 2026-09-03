# 部署与运行手册

更新时间：2026-09-03

本文档说明当前主线如何部署、启动、停止、检查状态和录制。密码、私有凭据和现场临时备份不写入仓库文档。

## 1. 当前实现基准

当前主线按以下角色运行：

1. 发送端：Linux ARM/ARM64，连接 Orbbec Gemini 相机。
2. 接收端：Linux x86_64，运行 C++ 接收端和 Web Monitor。
3. NAS：由接收端挂载为本地目录。
4. Web Monitor：默认 8080 端口。

正式文档以仓库代码和配置模板为准。现场设备正在跑的状态可以作为部署参考，但不等于主线默认配置。

## 2. 接收端部署

### 2.1 依赖

接收端需要：

```text
Linux x86_64
cmake
g++
ffmpeg
python3
python3-venv
```

Web Monitor 依赖位于：

```text
09_web_monitor/requirements.txt
```

### 2.2 配置文件

当前生产接收端配置：

```text
06_configs/receiver_loop.json
```

关键字段：

```text
media_port: 50010
status_port: 50011
clock_sync.port: 50012
admin_port: 18080
web_port: 8080
nas_root: 接收端挂载后的 NAS 根目录
receiver_discovery.port: Sender 自动发现端口，默认 UDP 50009
nas_auto_mount: 配套 NAS 自动发现、挂载健康状态和录制启动门禁
recording_staging.enabled: true 为当前生产本地 staging 模式
recording_staging.root: receiver 本地可靠暂存目录
recording_staging.direct_publish_hidden_directory: 直写模式的 NAS 隐藏写入目录
recording_staging.rgb_output_mode: fragmented_mp4 直接交付或 conventional_mp4 兼容重封装
segment_seconds: 单段切片时长
state_path: Web/REST 持久化状态文件
```

当前生产配置要求 `recording_staging.root` 位于 Receiver 本地 ext4/xfs，不能等于或位于 `nas_root` 内。`min_free_disk_mb` 和 `min_free_disk_percent` 同时保护本地暂存盘。NAS 自动挂载和出厂验收见 [field-deployment.md](field-deployment.md)。

### 2.3 启动、停止、状态

启动：

```bash
./05_tools/start_receiver.sh
```

停止：

```bash
./05_tools/stop_receiver.sh
```

查看状态：

```bash
./05_tools/status_receiver.sh
```

使用指定配置：

```bash
./05_tools/start_receiver.sh 06_configs/receiver_loop.json
./05_tools/status_receiver.sh 06_configs/receiver_loop.json
```

启动脚本会编译接收端、准备 Web Monitor venv，并优先通过 systemd 用户服务启动 receiver、uploader 和 Web Monitor。NAS 不可用时 uploader 暂停，Receiver 仍可预览，但新录制由存储门禁阻止。构建产物位于当前构建目录的 `bin/`，隔离测试构建不会覆盖正式 `12_build/bin/`。

### 2.4 自启动

首次部署或接收端网卡配置变更后，安装内核收包队列与 RPS 调优：

```bash
sudo ./05_tools/install_receiver_network_tuning.sh
```

该服务会自动选择默认路由接口，持久化加载
`06_configs/99-gwv3-receiver-network.conf`，并在每次开机后恢复 RX 队列 RPS 配置。

安装接收端用户服务：

```bash
./05_tools/install_receiver_autostart.sh 06_configs/receiver_loop.json
```

出厂时还必须安装 NAS 自动挂载服务；它需要本机已有权限为 `0600` 的 `/etc/gwv3/nas-credentials`：

```bash
sudo ./05_tools/install_receiver_nas_auto_mount.sh 06_configs/receiver_loop.json
```

卸载：

```bash
./05_tools/uninstall_receiver_autostart.sh
```

安装后涉及：

```text
gwv3-gemini-receiver.service
gwv3-nas-auto-mount.service       # 系统级
gwv3-receiver-network-tuning.service
gwv3-recording-uploader.service
gwv3-web-monitor.service
gwv3-receiver-log-rotate.timer
```

如果要求未登录桌面也能自启动，需要按现场用户启用 linger。

## 3. Web Monitor

默认访问：

```text
http://<receiver_ip>:8080
```

Web Monitor 面向受信任的采集局域网，默认无需访问令牌。接收端 C++ 管理口仍强制绑定 `127.0.0.1`，外部控制只能经过 Web Monitor。不要将 Web 端口直接暴露到公网。

页面能力：

1. 查看发送端和相机在线状态。
2. 查看 RGB 和 Depth 预览。
3. 切换主预览目标。
4. 全局开始/停止录制。
5. 单路开始/停止录制。
6. 设置相机显示名称和文件名前缀。

Web Monitor 调用接收端本地管理 API。真正的状态和录制控制在 C++ 接收端。

## 4. 发送端部署

### 4.1 依赖

发送端需要：

```text
Linux ARM64 / aarch64
Orbbec SDK ARM64
GStreamer
Rockchip MPP GStreamer 插件 mpph264enc
OpenCV
jsoncpp
zlib
libjpeg
```

重新编译通常还需要：

```text
cmake
g++
pkg-config
libopencv-dev
libjsoncpp-dev
libgstreamer1.0-dev
libgstreamer-plugins-base1.0-dev
zlib1g-dev
liblz4-dev
libjpeg-dev
```

检查硬件编码器：

```bash
gst-inspect-1.0 mpph264enc
```

### 4.1.1 LubanCat-3IO / RK3576

使用 `rtw_8821cu` USB Wi-Fi 的 LubanCat 需要安装发送队列限制，避免持续媒体上行时产生数百毫秒的驱动排队：

```bash
sudo ./05_tools/install_sender_wifi_tuning.sh
```

安装脚本仅对 `rtw_8821cu` 驱动应用 `pfifo limit 128`，其他无线驱动保持不变。服务会在开机联网后、sender 启动前恢复配置。

RK3576 镜像需要额外核对 GStreamer 插件 ABI，不能只检查
`libgstrockchipmpp.so` 文件是否存在。典型错误组合是：

```text
GStreamer runtime: 1.20
Rockchip MPP plugin build ABI: 1.22
```

这种组合会把 `libgstrockchipmpp.so` 加入 GStreamer blacklist，表现为文件存在，
但 `gst-inspect-1.0 mpph264enc` 返回 `No such element or plugin`。

处理方式是保留系统插件不动，把与目标 runtime ABI 一致的 Rockchip MPP 插件放入
设备专用运行目录：

```text
<runtime-root>/gstreamer-1.0/libgstrockchipmpp.so
```

systemd 服务通过下面的环境变量优先加载该插件：

```text
GST_PLUGIN_PATH_1_0=<runtime-root>/gstreamer-1.0
```

部署后必须同时验证 `mppjpegdec`、`mpph264enc` 和实际硬件 pipeline，不能用
`gst-inspect` 单项结果代替编码测试。

### 4.2 Orbbec SDK

仓库不内置 Orbbec SDK 实体文件。SDK 放置规则见：

[../11_third_party/README.md](../11_third_party/README.md)

发送端 ARM64 SDK 应放在 `11_third_party/orbbec/linux_arm64/` 下的约定目录。

Gemini 305（USB ID `2bc5:0840`）在当前 RK3576 现场镜像上使用
Orbbec SDK `2.8.6`。SDK `1.10.27` 在该设备上即使以 root 运行也无法枚举相机，
不能把 USB 枚举成功误判为 SDK 已兼容。

### 4.3 配置文件

发送端配置位于 `06_configs/`。设备与配置清单只在
[configuration.md](configuration.md) 维护；部署时从清单选择目标设备文件，不按文件修改时间猜测。

关键字段：

```text
sender_id
receiver.ip
receiver_discovery
receiver.media_port
receiver.status_port
clock_sync
cameras[].camera_id
cameras[].device_model
cameras[].serial_number
cameras[].uid
cameras[].rgb_profile
cameras[].depth_profile
cameras[].rgb_encoding
cameras[].depth_transport
web_rgb_preview
hotplug
```

单相机发送端可只配置 `device_model`，此时接受该 SDK 型号下的任意物理序列号，适合相机在相同型号设备之间互换。`serial_number` 或 `uid` 非空时仍作为更严格的身份约束，`device_model` 同时作为型号校验。多相机发送端尤其是多台同型号相机必须继续配置 `serial_number` 或 `uid`，否则无法稳定区分 `camera_id`。

### 4.4 发送端身份规则

多发送端环境中必须保证：

1. 每台发送端有稳定 `sender_id`。
2. 每路相机有稳定 `camera_id`。
3. `<sender_id>_<camera_id>` 不能和其他在线设备冲突。
4. 不要因为仓库模板、当前 IP 或临时调试随意改身份。
5. 如果只是想改变网页显示或文件命名，使用相机自命名和文件名前缀。

`sender_id` 可以写固定值，也可以写 `auto` 由机器信息派生。复制配置到另一台设备前，必须检查身份是否会冲突。

不要只用 USB Wi-Fi 网卡的 MAC 地址生成固定 `sender_id`。网卡可能在板卡之间移动，
克隆镜像也可能生成重复 MAC。优先使用板卡序列号、不可移动网卡地址或受控资产编号；
更换 Wi-Fi 网卡或相机时不应改变板卡身份。

### 4.5 构建、启动、停止和状态

在每台目标 ARM64 设备本机构建，输出目录必须与运维脚本约定一致：

```bash
ORBBEC_SDK_ROOT=/path/to/OrbbecSDK cmake -S . -B 12_build \
  -DGWV3_BUILD_RECEIVER=OFF \
  -DGWV3_BUILD_SENDER=ON \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build 12_build -j2
ctest --test-dir 12_build --output-on-failure
```

`start_sender.sh` 不执行编译。它先检查 `12_build/bin/gemini_sender`、SDK、插件、配置、路由和 USB，再由 watchdog 启动子进程。

启动：

```bash
./05_tools/start_sender.sh 06_configs/<sender-config>.json
```

带本地预览启动：

```bash
./05_tools/start_sender_preview.sh 06_configs/<sender-config>.json
```

查看状态：

```bash
./05_tools/status_sender.sh 06_configs/<sender-config>.json
```

停止：

```bash
./05_tools/stop_sender.sh
```

前台调试：

```bash
./05_tools/run_sender_foreground.sh 06_configs/<sender-config>.json
```

预检：

```bash
./05_tools/sender_preflight.sh 06_configs/<sender-config>.json
```

## 5. 时间同步和时间显示

系统有两类时间：

1. 给人看的系统时间。
2. 给程序和下游用的微秒时间戳。

要求：

1. 桌面和 Web Monitor 显示北京时间 UTC+8，精确到秒。
2. 协议、API、CSV 和文件中的 `*_timestamp_us` 保持 Unix epoch microseconds。
3. 发送端和接收端应保持系统时间同步。
4. CLOCK_SYNC 作为运行时偏移估计，不能替代 chrony，也不能实现硬件级曝光同步。

相关脚本：

```bash
./05_tools/setup_receiver_chrony_server.sh
./05_tools/setup_sender_chrony_client.sh
./05_tools/wait_chrony_sync.sh
./05_tools/set_desktop_screen_timeout.sh
```

## 6. 录制流程

### 6.1 Web 录制

1. 打开 Web Monitor。
2. 确认相机在线、预览可见、状态无明显错误。
3. 点击全部录制或单路录制。
4. 观察录制状态和保存目录。
5. 停止录制。
6. writer 分离后可以开始下一次录制；旧段后台关闭期间查看 `record_finalize_outstanding_segments`，不要把“可重新开始”和“已可交付”混为一谈。
7. 等待最终目录出现 `recording_ready.json`，并确认 record queue 与 finalizer 归零。
8. 生产 fMP4 模式应看到 `rgb_container_format=fragmented_mp4`，不应出现整文件重封装。
9. 当前生产 staging 模式还需要等待 `recording_uploader.pending_segments=0` 和本地缓存清零。
10. 多机对齐前运行 `./05_tools/sync_input_guard.py --inputs <各路录制目录> --verify-video-frames`。

### 6.2 CLI 录制

查看状态：

```bash
./05_tools/gwv3_receiver_cli.py status
```

全局开始：

```bash
./05_tools/gwv3_receiver_cli.py start-all
```

全局停止：

```bash
./05_tools/gwv3_receiver_cli.py stop-all
```

单路开始：

```bash
./05_tools/gwv3_receiver_cli.py start <sender_id> <camera_id>
```

单路停止：

```bash
./05_tools/gwv3_receiver_cli.py stop <sender_id> <camera_id>
```

## 7. 录制后检查

录制完成后先检查文件是否齐全：

```text
rgb.mp4
depth.mkv
frames.csv
meta.json
calibration.json
ffmpeg.log
recording_ready.json
```

分析帧率：

```bash
./05_tools/analyze_segment_fps.py <segment_dir>
```

导出 Orbbec 兼容目录：

```bash
./05_tools/export_orbbec_delivery.py <segment_dir> --overwrite
```

生成 RGB 坐标系下的 aligned depth：

```bash
python3 05_tools/align_depth_to_rgb.py <segment_dir>
```

## 8. Wi-Fi Guard

发送端 Wi-Fi guard 默认应检查链路是否处于 5GHz 或满足最低频率要求，不应在仓库默认脚本中写死现场 SSID。

只有某次测试明确要求固定到某个 AP 时，才通过 systemd drop-in 或临时环境变量指定连接名。

如果 systemd 显示服务 active 但接收端没有画面，要检查 watchdog 是否卡在 Wi-Fi guard、预检或相机启动阶段，不能只看 active 状态。

## 9. 运行前检查清单

接收端：

1. NAS 挂载目录存在、是挂载点且可写；直写隐藏目录与正式目录位于同一文件系统。
2. `ffmpeg` 可用；当前生产 staging 模式要求 `gwv3-recording-uploader.service` active。
3. `50010`、`50011`、`8080` 未被错误占用。
4. 接收端服务和 Web Monitor 都已启动。
5. Web Monitor 能访问。
6. 使用 `build_commit` / `build_dirty` / `build_source_hash` 确认实际运行版本；`build_source_hash` 是组件级哈希，所有 sender 应一致，receiver 单独核对。

发送端：

1. Orbbec SDK 路径正确。
2. 相机能被 SDK 枚举。
3. `mpph264enc` 可用。
4. 配置里的接收端 IP 和端口正确。
5. `sender_id` / `camera_id` 不冲突。
6. Wi-Fi 处于目标频段。
7. 预检通过。

## 10. 安全和提交规则

可以提交：

1. 文档。
2. 配置模板。
3. 脚本。
4. 源码。

不应提交：

1. 密码。
2. 私有凭据。
3. NAS 原始录制数据。
4. 本地临时备份包。
5. 与当前主线无关的现场私有配置。
