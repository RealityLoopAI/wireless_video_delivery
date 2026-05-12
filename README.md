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
7. 香橙派 5 Pro 发送端首版 C++ 实现。
8. Ubuntu 接收端 C++ 核心接收与录制服务。
9. FastAPI Web/REST 监控服务。
10. 接收端 CLI 控制工具。
11. 发送端和接收端一键启动、停止、状态脚本。
12. 接收端 Orbbec 兼容交付导出工具。
13. 发送端一键脚本预检：配置、SDK、相机、GStreamer 编码器、接收端路由和 Wi-Fi 链路信息。

暂未内置：

1. Orbbec SDK 实体库文件。
2. 长时间稳定性测试报告。
3. 接收端历史回放、文件下载、权限体系。
4. 发送端软件编码 fallback 的实测验证。

## 3. 目录说明

```text
01_sender_linux/
```

发送端工程目录。后续放树莓派 5、香橙派上运行的 C++ 采集和发送程序。

```text
02_receiver_linux/
```

接收端 C++ 工程目录。负责 UDP/TCP 接收、录制控制、本地管理 HTTP 和 NAS 写入。

```text
03_common_core/
```

公共核心目录。后续放发送端和接收端共用的数据结构、协议定义、配置读取、日志、时间戳等代码。

```text
04_docs/
```

项目文档目录。当前最重要的需求和分工文档都在这里。

```text
05_tools/
```

工具目录。后续放环境检查、网络检查、调试工具等。

```text
06_configs/
```

配置目录。后续放发送端和接收端 JSON 配置模板。

```text
07_samples/
```

样例目录。后续放录制目录样例、配置样例或小规模测试样例。

```text
08_reports/
```

报告目录。后续放测试报告、联调记录、问题总结和验收记录。

```text
09_web_monitor/
```

网页监控目录。当前使用 FastAPI 提供 Web 页面和 REST 代理。

```text
10_tests/
```

测试目录。后续放单元测试、集成测试和功能验证脚本。

```text
11_third_party/
```

第三方依赖说明目录。Orbbec SDK、FFmpeg、GStreamer、WebRTC 等依赖的放置规则见该目录下的 README。

```text
12_build/
```

构建输出目录。后续 CMake 生成的临时文件和可执行文件可放这里。

## 4. 关键文档

建议按以下顺序阅读：

1. `04_docs/00_文档索引_v3.md`
2. `04_docs/需求3.0.md`
3. `04_docs/03_中间传输数据格式_v3.md`
4. `04_docs/08_接收端运行使用手册_v3.md`
5. `04_docs/06_接收端当前存储格式说明_v3.md`
6. `04_docs/07_Orbbec交付导出说明_v3.md`
7. `04_docs/06_发送端运行使用手册_v3.md`

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

当前 v3 工程没有直接内置 Orbbec SDK 实体文件。

已确认 Orbbec SDK v1.10.27 release 中有 Linux ARM64 包，可作为发送端候选 SDK：

```text
https://github.com/orbbec/OrbbecSDK/releases/tag/v1.10.27
```

本项目建议后续按架构分开放置：

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

当前发送端默认配置：

```text
06_configs/sender_orangepi5pro-01_depth_zlib.json
```

默认规格：

```text
sender_id: orangepi5pro-01
camera_id: cam01
receiver_ip: 192.168.1.107
RGB target: 1920x1080@30 H.264 12Mbps
Depth: 640x400@30 zlib
```

当前 Orange Pi 5 Pro + Orbbec SV1301S_U3 实测：Depth 基本可到 30fps，RGB 1080p MJPG 即使单独打开也约 19-21fps。配置中的 `30fps` 是请求 profile 和编码时间基准，接收端应以发送端心跳和日志里的实测帧率为准。

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
