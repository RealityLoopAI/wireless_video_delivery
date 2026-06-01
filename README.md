# Gemini Wireless Video v3

## 1. 项目定位

本目录是无线 RGBD 采集与录制系统 3.0 的工程骨架。
3.0 目标是使用 C++ 重构发送端和接收端，实现全 Linux 环境下的多路 Gemini RGB + Depth 采集、传输、网页预览和 NAS 录制。

当前目录已包含发送端、接收端、Web 监控和控制脚本源码；发送端需要按 `11_third_party/README.md` 放置 Orbbec ARM64 SDK 后编译运行。

## 2. 当前状态

已完成：

1. v3 工程目录分类。
2. 需求 3.0 总文档。
3. 发送端需求文档。
4. 接收端需求文档。
5. 中间传输数据格式文档。
6. 第三方 SDK 放置说明。
7. RK3588/香橙派 5 Pro 发送端 C++ 实现，包含本地预览、自动曝光配置、坏 MJPEG 帧过滤、TCP 背压丢包保护/自动重连、采集/媒体发送停滞自恢复和 watchdog 自动重启。
8. Ubuntu 接收端 C++ 核心接收与录制服务。
9. FastAPI Web/REST 监控服务。
10. 接收端 CLI 控制工具。
11. 发送端和接收端一键启动、停止、状态脚本。
12. 接收端 Orbbec 兼容交付导出工具。
13. 发送端一键脚本预检：配置、SDK、相机、GStreamer 编码器、接收端路由、Wi-Fi 链路信息、USB/TCP 缓冲告警和当前设备 5GHz Wi-Fi guard。

源码仓库默认不包含或尚未完成：

1. Orbbec SDK 实体库文件（GitHub 源码交付默认不包含）。
2. 长时间稳定性测试报告。
3. 接收端历史回放、文件下载、权限体系。
4. 发送端软件编码 fallback 的实测验证。

## 3. 目录说明

```text
01_sender_linux/
```

发送端工程目录，包含树莓派 5、香橙派等 Linux ARM/ARM64 设备上运行的 C++ 采集和发送程序。

```text
02_receiver_linux/
```

接收端 C++ 工程目录。负责 UDP/TCP 接收、录制控制、本地管理 HTTP 和 NAS 写入。

```text
03_common_core/
```

公共核心目录，包含发送端和接收端共用的数据结构、协议定义、配置读取、日志、时间戳等代码。

```text
04_docs/
```

项目文档目录。当前最重要的需求和分工文档都在这里。

```text
05_tools/
```

工具目录，包含启动、停止、状态、预检、导出和调试工具。

```text
06_configs/
```

配置目录，包含发送端和接收端 JSON 配置模板。

```text
07_samples/
```

样例目录，用于放录制目录样例、配置样例或小规模测试样例。

```text
08_reports/
```

报告目录，用于放测试报告、联调记录、问题总结、验收记录和运行日志。

```text
09_web_monitor/
```

网页监控目录。当前使用 FastAPI 提供 Web 页面和 REST 代理。

```text
10_tests/
```

测试目录，用于放单元测试、集成测试和功能验证脚本。

```text
11_third_party/
```

第三方依赖说明目录。Orbbec SDK、FFmpeg、GStreamer、WebRTC 等依赖的放置规则见该目录下的 README。

```text
12_build/
```

构建输出目录，CMake 生成的临时文件和可执行文件放在这里。

## 4. 关键文档

建议按以下顺序阅读：

1. `04_docs/00_文档索引_v3.md`
2. `04_docs/需求3.0.md`
3. `04_docs/03_中间传输数据格式_v3.md`
4. `04_docs/08_接收端运行使用手册_v3.md`
5. `04_docs/06_接收端当前存储格式说明_v3.md`
6. `04_docs/07_Orbbec交付导出说明_v3.md`
7. `04_docs/06_发送端运行使用手册_v3.md`

发送端默认配置当前使用固定 `sender_id=orangepi5pro-66-133`，接收端地址 `192.168.66.196`。RK3588 基础配置为 `cam01` / `cam02`，RGB 为 `1920x1080@30 MJPG -> H.264 12Mbps`，Depth 为 `640x400@30 y16 -> zlib`，本地预览按配置启用。发送端运行中每 2 秒扫描 Orbbec 设备，发现配置外新设备时自动分配 `cam03` / `cam04` 并按同规格发送；动态热插拔最多同时发送 4 路，超出设备会被忽略并上报 warning event。4 路是发送端逻辑上限，实际稳定路数取决于当前无线链路和接收端吞吐。媒体 TCP 发送使用非阻塞背压保护：短暂拥塞时优先丢弃尚未写入 socket 的完整媒体包并保留连接；连续背压丢包后会主动关闭 media socket 并重连，避免发送计数长期卡在 0。如果采集正常但媒体连续发不出去，发送端会主动退出并交给 watchdog 重启。

如需让接收端录制完成后运行脚本生成 RGB 坐标系下的 `depth_aligned_to_rgb.mkv`，发送端应使用对齐采集配置 `06_configs/sender_rk3588-01_two_cameras_align.json`，规格为 `RGB 640x480@30 + Depth 640x400@30`。启动入口为 `05_tools/start_sender_align.sh` 或 `05_tools/start_sender_align_preview.sh`；录制前运行 `05_tools/check_sender_alignment_ready.sh`，确认接收端状态中目标相机 `calibration_available=true` 且 announce 尺寸匹配。

2026-05-29 当前现场容量结论：这台 RK3588 发送端本机算力按实测约可支撑 5-6 路同规格采集/编码/发送处理，但在当前 Wi-Fi 与接收端组合下，端到端稳定上限是 2 路满规格；3 路可运行但 TCP Send-Q 会接近 4 MiB 上限并出现 backpressure 丢包，4 路仅表示发送端逻辑支持，不建议在当前无线链路下作为稳定配置。

发送端开发人员重点阅读：

1. `04_docs/01_发送端需求_v3.md`
2. `04_docs/03_中间传输数据格式_v3.md`
3. `04_docs/04_发送端交付与接收端对接说明_v3.md`
4. `04_docs/06_发送端运行使用手册_v3.md`
5. `11_third_party/README.md`

接收端开发人员重点阅读：

1. `04_docs/02_接收端需求_v3.md`
2. `04_docs/03_中间传输数据格式_v3.md`
3. `04_docs/08_接收端运行使用手册_v3.md`
4. `04_docs/06_接收端当前存储格式说明_v3.md`
5. `04_docs/07_Orbbec交付导出说明_v3.md`

双方共同对齐：

1. `04_docs/03_中间传输数据格式_v3.md`

## 5. Orbbec SDK 状态

GitHub 源码交付默认不内置 Orbbec SDK 实体文件。本机运行发送端时，应按下方路径放置 ARM64 SDK；当前这台发送端已按 `linux_arm64` 路径放置 SDK。

已确认 Orbbec SDK v1.10.27 release 中有 Linux ARM64 包，可作为发送端候选 SDK：

```text
https://github.com/orbbec/OrbbecSDK/releases/tag/v1.10.27
```

本项目按架构分开放置：

```text
11_third_party/orbbec/linux_arm64/
11_third_party/orbbec/linux_x64/
```

规则：

1. `linux_arm64` 给树莓派 5 / 香橙派发送端使用。
2. `linux_x64` 给 Ubuntu 24.04 x86_64 接收端或开发机参考使用。
3. 不能把 x64 SDK 当作发送端 SDK。

具体放置规则见：

```text
11_third_party/README.md
```

## 6. 打包交付说明

如果当前阶段要把本目录发给其他开发人员，请至少包含：

1. 本 README。
2. `04_docs/` 全部文档。
3. `11_third_party/README.md`。
4. 当前一级目录结构。

如果要连 SDK 一起打包，需要先确认是否允许分发 SDK 实体，并把 ARM64 和 x64 SDK 按架构分别放入 `11_third_party/orbbec/` 下。

## 7. 发送端快速启动

当前 RK3588 发送端默认配置：

```text
06_configs/sender_rk3588-01_two_cameras.json
```

RGB/Depth 对齐采集配置：

```text
06_configs/sender_rk3588-01_two_cameras_align.json
```

默认规格：

```text
sender_id: orangepi5pro-66-133
camera_id: cam01 / cam02
receiver_ip: 192.168.66.196
RGB: 1920x1080@30 H.264 12Mbps
Depth: 640x400@30 zlib
hotplug: scan every 2s, auto cam03/cam04, max active cameras 4
field capacity: stable 2 full-spec streams on current Wi-Fi/receiver; 3 streams run with backpressure drops
transport.send_buffer_bytes: 4194304
Wi-Fi guard: 土著拯救器-5G, min 5000MHz
desktop idle-delay: 3600s
dual full spec tuning: usbfs_memory_mb >= 256, net.core.wmem_max >= 4194304
```

对齐采集规格：

```text
RGB: 640x480@30 H.264 4Mbps
Depth: 640x400@30 zlib
receiver-side alignment: run receiver/downstream script after recording
pre-recording check: ./05_tools/check_sender_alignment_ready.sh
```

固定 `sender_id` 必须和设备名称对应，避免接收端把多台设备的数据混在一起。
实际运行参数和系统缓冲告警以 `./05_tools/sender_preflight.sh` 和 `./05_tools/status_sender.sh` 输出为准。

当前桌面熄屏时间设置为 3600 秒，可通过 `./05_tools/set_desktop_screen_timeout.sh 3600` 重新应用。

启动：

```bash
./05_tools/start_sender.sh
```

带本地预览启动：

```bash
./05_tools/start_sender_preview.sh
```

切换到 RGB/Depth 对齐采集模式：

```bash
./05_tools/stop_sender.sh
./05_tools/start_sender_align_preview.sh
./05_tools/check_sender_alignment_ready.sh
```

查看状态：

```bash
./05_tools/status_sender.sh
```

停止：

```bash
./05_tools/stop_sender.sh
```

详细说明见：

```text
04_docs/06_发送端运行使用手册_v3.md
```

## 8. 接收端快速启动

当前接收端默认写入：

```text
/home/fz/Desktop/nas
```

启动：

```bash
./05_tools/start_receiver.sh
```

查看状态：

```bash
./05_tools/status_receiver.sh
```

停止：

```bash
./05_tools/stop_receiver.sh
```

Web 页面：

```text
http://127.0.0.1:8080
```

命令行控制：

```bash
./05_tools/gwv3_receiver_cli.py status
./05_tools/gwv3_receiver_cli.py start-all
./05_tools/gwv3_receiver_cli.py stop-all
```

Orbbec 兼容交付导出：

```bash
./05_tools/export_orbbec_delivery.py <segment_dir> --overwrite
```

详细说明见：

```text
04_docs/08_接收端运行使用手册_v3.md
04_docs/07_Orbbec交付导出说明_v3.md
```
