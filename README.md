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
7. RK3588/香橙派 5 Pro 发送端 C++ 实现，包含本地预览、曝光/增益配置、坏 MJPEG 帧过滤、TCP 背压丢包保护/自动重连、采集/媒体发送停滞自恢复和 watchdog 自动重启。
8. Ubuntu 接收端 C++ 核心接收与录制服务。
9. FastAPI Web/REST 监控服务。
10. 接收端 CLI 控制工具。
11. 发送端和接收端一键启动、停止、状态脚本。
12. 接收端 Orbbec 兼容交付导出工具。
13. 发送端一键脚本预检：配置、SDK、相机、GStreamer 编码器、接收端路由、Wi-Fi 链路信息、USB/TCP 缓冲告警和当前设备 5GHz Wi-Fi guard。
14. 接收端 systemd 用户服务自启动、异常自动重启、日志轮转和 Web 访问日志降噪。

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

发送端默认配置当前使用固定 `sender_id=rk3588-ubuntu`，接收端地址 `192.168.66.196`。当前交付预期收敛为一路 RGBD 稳定发送，默认入口使用 `06_configs/sender_rk3588-01_one_camera.json`。默认一路规格为 `cam02 / AY2MC31010W`，RGB `1920x1080@30 MJPG -> H.264 12Mbps`，Depth `320x200@30 y12 -> uint16 depth frame -> zlib`，本地预览按配置启用。发送端仍保留多路和热插拔能力，发现配置外新设备时可自动分配 `cam03` / `cam04` 并按同规格发送；但这属于扩展/实验能力，不作为当前默认运行目标。媒体 TCP 发送使用非阻塞背压保护：短暂拥塞时优先丢弃尚未写入 socket 的完整媒体包并保留连接；连续背压丢包后会主动关闭 media socket 并重连，避免发送计数长期卡在 0。如果采集正常但媒体连续发不出去，发送端会主动退出并交给 watchdog 重启。

2026-06-05 Orange Pi 5 Pro 现场单路配置必须保留历史相机 key：`sender_id=auto` 派生为 `orangepi5pro-b439137c`，`camera_id=cam02`，配置文件为 `06_configs/sender_orangepi5pro-01_depth_zlib.json`，热插拔关闭。运行规格应跟随当前默认交付档，Depth 使用 `320x200@30 y12`；不能跟随仓库默认乱改的是身份和设备绑定字段。不要为了适配本机 IP 临时改成 `orangepi5pro-66-206_cam01`；这样会改变接收端 key，网页主预览或录制控制如果还选中旧 key，就会表现为“看不到预览/像是没发”。排查时先看接收端 `/api/status` 中 `orangepi5pro-b439137c_cam02` 的 `rgb_packets/depth_packets` 是否增长，并用 `POST /api/preview/main-target?sender_id=orangepi5pro-b439137c&camera_id=cam02` 把主预览切回本机。

2026-06-05 使用 `05_tools/orbbec_depth_probe.cpp` 对现场 `SV1301S_U3 / AY2MC31010W` 枚举确认：该相机最低 Depth profile 为 `320x200@5 y11/y12`，最高 Depth profile 为 `1280x800@30 y11/y12`。当前默认交付使用 `320x200@30 y12`，在保留 Depth 实时性的同时降低 USB、zlib、网络和接收端压力；`06_configs/sender_rk3588-01_cam02_depth_max.json` 提供单路最高 Depth 测试配置，用于容量验证，不作为默认交付配置。

2026-06-05 默认 Depth 档运行时，发送端会按 `depth_profile.fps` 对 Depth 压缩和媒体发送限流。运行状态判断以 `depth_sent_fps` 和 `depth_mbps` 为准，`depth_input_fps` 仅表示 SDK 输入到发送端的实际帧率。

2026-06-05 RK3588 双路现场配置为 `06_configs/sender_rk3588-01_two_cameras.json`，固定 `sender_id=rk3588-ubuntu`，接收端为 `192.168.66.196`。当前两路绑定为 `cam01 / AY2M54302ZH / uid=10-1.2` 和 `cam02 / AY2MC3100Z8 / uid=8-1.4.2`，RGB 保持 `1920x1080@30 H.264 12Mbps`，Depth 使用 `320x200@30 y12 -> zlib`，RGB 手动曝光使用 `auto_exposure=false, exposure=300, gain=50`，这是现场按 30fps 约束调整后的中等增益档。现场发现 Orbbec SDK 返回的两路 Depth 物理视角与 RGB 交叉，因此该配置启用 `swap_depth_between_cameras=true`：只交换 Depth 的发送和本地预览归属，RGB 仍按各自 `camera_id` 发送。验证时同时看发送端本地预览标签、接收端 `/api/status` 中 `rk3588-ubuntu_cam01/cam02` 的包计数，以及接收端 RGB/Depth 四宫格截图；不要只看网页主预览当前选中的旧 key。

2026-06-05 使用最高 Depth 单路配置断开前最后 60 个有效 `perf` 采样确认：RGB/Depth 输入与发送均约 30fps，RGB 网络约 12.06Mbps，Depth zlib 网络约 62.45Mbps，总网络约 74.51Mbps；Depth USB 输入约 492.38Mbps，Depth zlib 平均约 15.78ms/帧，RGB/Depth send failure 均为 0。该结果依赖当时 5G Wi-Fi 链路和接收端吞吐正常，仍只作为单路容量和画质验证；默认交付已改为 `320x200@30 y12`。

2026-06-05 当前现场交付预期：默认运行一路 RGBD，降低对 Wi-Fi 上行、USB、中断和远程操作通道的抢占。2026-05-29 历史容量测试表明，这台 RK3588 本机算力按实测约可支撑 5-6 路同规格处理，但在当前 Wi-Fi 与接收端组合下，多路会先受无线链路或接收端吞吐限制；3 路曾出现 TCP Send-Q 接近 4 MiB 和 backpressure 丢包。因此 2 路以上只作为手动指定配置的扩展/实验场景，不作为当前默认交付预期。

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
06_configs/sender_rk3588-01_one_camera.json
```

默认规格：

```text
sender_id: rk3588-ubuntu
camera_id: cam02
receiver_ip: 192.168.66.196
RGB: 1920x1080@30 H.264 12Mbps
Depth: 320x200@30 y12 -> zlib
current expectation: stable 1 full-spec RGBD stream
hotplug: retained as extension capability, not default delivery target
transport.send_buffer_bytes: 4194304
Wi-Fi guard: 土著拯救器-5G, min 5000MHz
desktop idle-delay: 3600s
desktop clock seconds: enabled
monitor time display: Beijing time UTC+8, second precision
multi-stream tuning: usbfs_memory_mb >= 256, net.core.wmem_max >= 4194304
```

最高 Depth 单路测试配置：

```text
06_configs/sender_rk3588-01_cam02_depth_max.json
RGB: 1920x1080@30 H.264 12Mbps
Depth: 1280x800@30 y12 -> zlib
2026-06-05实测: RGB约30fps, Depth约30fps, 总网络约74.5Mbps, send failure为0
用途: 单路容量和画质验证；默认交付保持 320x200@30 y12
```

多发送端同时接入时，接收端以 `<sender_id>_<camera_id>` 作为唯一相机 key。固定 `sender_id` 必须和物理设备一一对应，不能为了本机 IP、当前网段或临时调试随意改名；同一个 key 被两台发送端复用会导致预览、录制控制和存储目录混写或互相覆盖。改动任何 `sender_id` / `camera_id` 前，先查接收端 `/api/status` 是否已有同名 live key，并以 `sender_preflight.sh` 的 ID conflict 提示为准。
实际运行参数和系统缓冲告警以 `./05_tools/sender_preflight.sh` 和 `./05_tools/status_sender.sh` 输出为准。

当前桌面熄屏时间设置为 3600 秒，可通过 `./05_tools/set_desktop_screen_timeout.sh 3600` 重新应用。现场桌面时钟需要显示到秒，当前用户应保持 `org.gnome.desktop.interface clock-show-seconds=true`。Web Monitor 页面上的系统时间、Last media、Last status 统一按北京时间 UTC+8 显示到秒；接口和落盘中的 `*_timestamp_us` / `last_*_us` 仍保留 Unix epoch microseconds 原始值，用于跨流对齐。

启动：

```bash
./05_tools/start_sender.sh
```

带本地预览启动：

```bash
./05_tools/start_sender_preview.sh
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
