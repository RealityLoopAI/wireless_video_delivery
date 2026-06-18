# wireless_video_delivery

`wireless_video_delivery` 是一套面向多路 Orbbec Gemini RGBD 相机的无线采集、传输、网页预览和录制工程。

仓库目标是把发送端、接收端、Web Monitor、配置模板、运行脚本和交付文档放在同一个工程里，方便在 Linux 设备上部署、联调和继续开发。README 只说明项目是什么、怎么进入、哪些边界需要遵守；设备现场配置、历史问题和测试数据统一放在 `04_docs/` 和 `08_reports/` 中维护。

## 1. 项目边界

当前工程覆盖：

1. Linux ARM/ARM64 发送端采集 Orbbec RGB 和 Depth。
2. RGB 编码、Depth 压缩、媒体传输和状态上报。
3. Linux x86_64 接收端接收、状态管理、录制控制和 NAS 落盘。
4. FastAPI Web Monitor 页面和 REST 控制接口。
5. 启动、停止、状态、预检、导出和现场维护脚本。
6. Orbbec 兼容交付导出说明和下游同步字段说明。

当前实现的网络入口：

```text
media:   TCP 50010
status:  UDP 50011
manage:  HTTP 8080
```

历史方案文档里提到的 UDP/RTP、SRT、WebRTC 等仍属于候选或对比路线。除非对应代码和配置已经落地，不要把它们当作当前默认传输实现。

## 2. 系统组成

典型链路如下：

```text
Orbbec Gemini camera
  -> sender_linux
  -> TCP/UDP network
  -> receiver_linux
  -> Web Monitor / REST
  -> NAS recordings / Orbbec-compatible export
```

发送端关注采集、编码、压缩、网络发送、本地预览和自恢复。

接收端关注多发送端接入、媒体解包、录制起停、Web 状态、落盘目录和导出。

公共核心关注协议结构、配置读取、时间戳字段、日志和跨端共享数据结构。

## 3. 当前状态

已具备的主要能力：

1. 工程目录和 CMake 构建骨架。
2. RK3588 / Orange Pi / Raspberry Pi 等 Linux 发送端代码路径。
3. RGB H.264 发送、Depth zlib 发送和基础本地预览。
4. 发送端预检、watchdog、自恢复和运行状态脚本。
5. Ubuntu 接收端核心服务、录制控制和状态接口。
6. FastAPI Web Monitor 监控页面。
7. Orbbec 兼容交付导出工具和存储格式说明。
8. 多发送端唯一 key、时间戳字段和下游同步文档。
9. 网络背压、坏帧、MP4 封装、网页预览和时间戳问题的归档文档。

源码仓库默认不包含：

1. Orbbec SDK 实体库文件。
2. 现场私有网络配置、密码或设备私有凭据。
3. NAS 上的录制数据。
4. 长时间稳定性验收报告的原始日志。

## 4. 目录结构

```text
01_sender_linux/
```

发送端 C++ 工程。运行在树莓派、香橙派、RK3588 等 Linux ARM/ARM64 设备上，负责相机采集、RGB 编码、Depth 压缩和网络发送。

```text
02_receiver_linux/
```

接收端 C++ 工程。负责媒体接收、状态聚合、录制控制、本地 HTTP 管理接口和 NAS 写入。

```text
03_common_core/
```

发送端和接收端共用代码，包括协议定义、配置读取、日志、时间戳和基础工具。

```text
04_docs/
```

工程文档目录。需求、运行手册、协议契约、时间戳同步、多发送端约束、故障归档和技术评审都在这里。

```text
05_tools/
```

启动、停止、状态、预检、导出、桌面设置和调试脚本。

```text
06_configs/
```

配置模板目录。发送端、接收端和现场测试配置都应通过这里的 JSON 文件管理。

```text
07_samples/
```

样例目录，用于放样例配置、小规模录制样例或离线测试材料。

```text
08_reports/
```

测试报告、联调记录、问题总结、验收记录和运行日志归档。

```text
09_web_monitor/
```

Web Monitor 服务。当前使用 FastAPI 提供网页和 REST 代理。

```text
10_tests/
```

测试目录，用于单元测试、集成测试和功能验证脚本。

```text
11_third_party/
```

第三方依赖说明目录。Orbbec SDK 放置规则见 `11_third_party/README.md`。

```text
12_build/
```

本地构建输出目录。CMake 临时文件和可执行文件放在这里，不应作为源码交付重点。

## 5. 快速入口

首次接手先读：

1. `04_docs/00_文档索引_v3.md`
2. `04_docs/需求3.0.md`
3. `04_docs/03_中间传输数据格式_v3.md`
4. `04_docs/27_现场运行约束与配置归档_2026-06-18.md`
5. `04_docs/28_跨设备文档审查与融合说明_2026-06-18.md`

接收端运行入口：

```bash
./05_tools/start_receiver.sh
./05_tools/status_receiver.sh
./05_tools/stop_receiver.sh
```

发送端运行入口：

```bash
./05_tools/start_sender.sh
./05_tools/start_sender_preview.sh
./05_tools/status_sender.sh
./05_tools/stop_sender.sh
```

实际使用哪个配置文件，应以对应设备的运行手册、交接文档和当前分支配置为准，不应从 README 里复制临时现场参数。

## 6. 配置原则

多发送端环境中，接收端使用 `<sender_id>_<camera_id>` 作为唯一相机 key。这个 key 会影响网页预览、录制控制、存储目录和下游同步，不能为了临时网段、当前 IP 或调试方便随意改名。

配置文件中的设备身份、相机绑定、接收端地址、RGB/Depth profile、码率、预览开关和热插拔策略都属于运行配置。默认配置可以随交付目标调整，但设备身份和相机 key 的改动必须先检查是否会与其他发送端冲突。

时间显示和时间戳字段分开处理：

1. 桌面和 Web Monitor 的可读系统时间按北京时间 UTC+8 显示到秒。
2. 协议、API、CSV 和落盘字段中的 `*_timestamp_us` 保持 Unix epoch microseconds 原始值。
3. RGB/Depth 和多发送端对齐以原始微秒时间戳和同步方案为准，不以页面显示时间为准。

详细约束见：

1. `04_docs/11_多发送端时间戳同步与对齐方案_v3.md`
2. `04_docs/15_时间戳字段统一与下游同步改造说明_2026-06-10.md`
3. `04_docs/27_现场运行约束与配置归档_2026-06-18.md`

## 7. 开发入口

发送端开发重点读：

1. `04_docs/01_发送端需求_v3.md`
2. `04_docs/04_发送端交付与接收端对接说明_v3.md`
3. `04_docs/06_发送端运行使用手册_v3.md`
4. `04_docs/10_发送端帧率排查与修复说明_v3.md`
5. `11_third_party/README.md`

接收端和 Web 开发重点读：

1. `04_docs/02_接收端需求_v3.md`
2. `04_docs/05_接收端落地说明_v3.md`
3. `04_docs/08_接收端运行使用手册_v3.md`
4. `04_docs/09_外部设备REST接口使用手册_v3.md`
5. `09_web_monitor/README.md`

协议、存储和下游同步重点读：

1. `04_docs/03_中间传输数据格式_v3.md`
2. `04_docs/06_接收端当前存储格式说明_v3.md`
3. `04_docs/07_Orbbec交付导出说明_v3.md`
4. `04_docs/16_RGB编码输出时间戳与下游视频索引同步整改说明_2026-06-10.md`
5. `04_docs/26_RGB全链路时间戳诊断字段说明_2026-06-17.md`

全链路审查和交接重点读：

1. `04_docs/22_全链路工程审查报告_2026-06-16.md`
2. `04_docs/24_当前上下文交接摘要_2026-06-16.md`
3. `README_RECEIVER_HANDOFF.md`

## 8. Orbbec SDK

GitHub 源码交付默认不内置 Orbbec SDK 实体文件。发送端编译运行前，需要按架构把 SDK 放到指定目录：

```text
11_third_party/orbbec/linux_arm64/
11_third_party/orbbec/linux_x64/
```

规则：

1. `linux_arm64` 给树莓派、香橙派和 RK3588 发送端使用。
2. `linux_x64` 给 Ubuntu x86_64 接收端或开发机参考使用。
3. 不能把 x64 SDK 当作 ARM64 发送端 SDK。

具体放置规则见 `11_third_party/README.md`。

## 9. 交付说明

源码交付至少包含：

1. `README.md`
2. `04_docs/`
3. `05_tools/`
4. `06_configs/`
5. `11_third_party/README.md`
6. 发送端、接收端、公共核心和 Web Monitor 源码目录

如果要连 SDK 一起交付，需要先确认 SDK 分发许可，并按 `11_third_party/README.md` 的架构目录放置。现场密码、私有网络信息、NAS 数据和本地临时备份文件不应进入 GitHub 仓库。
