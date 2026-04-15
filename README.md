# Wireless Video Delivery 产品手册

本工程是一套面向交付与现场部署的无线视频传输方案，用于将奥比中光 Gemini 相机的实时 RGB 画面从 Linux 发送端，通过 Wi-Fi 局域网传输到 Linux 或 Windows 接收端，并在接收端完成解码与显示。

当前交付目标是：**单路无线视频稳定传输、跨平台接收、支持一键检查与启动、便于现场实施与运维。**

链路流程如下：

`Gemini 相机 -> Linux 发送端采集/编码 -> RTP/UDP -> Wi-Fi 网络 -> Linux/Windows 接收端解码 -> 实时显示`

## 1. 产品定位

这套工程适合以下场景：

- 将 Gemini 相机画面从边缘设备无线传回接收主机
- 在同一局域网内快速搭建单路低延迟视频链路
- 现场演示、设备联调、轻量化视频回传
- 作为后续多路接入、硬件解码、平台化接入的基础版本

当前版本的核心特点：

- 单路实时优先，队列满时主动丢旧帧，优先保证画面“跟得上”
- 发送端支持状态统计、异常检测与自动重连
- 接收端支持 Linux 与 Windows 两种部署路径
- 已提供交付包级别的启动、停止、状态检查与配置基线

## 2. 支持的部署形态

### 方案 A：Linux 发送端 -> Linux 接收端

适合研发联调、实验室压测、同构环境部署。

### 方案 B：Linux 发送端 -> Windows 接收端

适合现场落地、普通 PC 接收显示、交付演示。

## 3. 交付内容

仓库目录说明如下：

- `01_sender_linux/`: Linux 发送端工程，负责相机采集、H.264 编码、RTP/UDP 发送
- `02_receiver_windows/`: Windows 接收端工程，负责接收、解码、显示，并提供 PowerShell 启停脚本
- `03_receiver_linux/`: Linux 接收端工程，负责接收、解码、显示
- `04_docs/`: 补充文档，包括产品手册、快速开始、技术路线、部署与运维说明
- `05_tools/`: 一键检查、启动、停止、状态查看脚本
- `06_configs/`: 交付配置基线
- `07_samples/`: Linux 侧运行时配置、PID、日志输出目录
- `08_reports/`: 测试验收、容量边界、交接说明
- 根目录脚本: 面向交付使用的快捷入口，屏蔽底层工程细节

推荐的阅读顺序：

1. 先看本 `README.md`
2. 需要快速上手时看 [04_docs/02_quick_start.md](04_docs/02_quick_start.md)
3. 需要做现场部署或排障时看 [04_docs/03_deploy_ops_manual.md](04_docs/03_deploy_ops_manual.md)
4. 需要了解实现路线时看 [04_docs/04_technical_route.md](04_docs/04_technical_route.md)

## 4. 系统能力概览

当前版本已经具备以下能力：

- 单个 Gemini 相机 RGB 视频采集
- H.264 低延迟编码
- RTP over UDP 发送与接收
- 接收端实时显示
- 有界队列与丢旧帧策略
- 运行状态统计
- 基础异常检测与自动恢复
- Linux 发送端 `systemd` 开机自启能力

## 5. 环境要求

### 发送端（Linux）

- Ubuntu 24.04 或同类 Linux 环境
- Python `3.9+`
- 推荐 Conda 环境：`orbbec_env`
- 相机 SDK：`pyorbbecsdk`
- 依赖：`av`、`opencv-python`、`numpy`
- 相机与发送端设备正确连接，并具备 USB 访问权限

### 接收端（Linux）

- Python `3.9+`
- 依赖：`av`、`opencv-python`、`numpy`

### 接收端（Windows）

- Windows PowerShell
- Python 可用，或安装 `uv` 以自动创建 `.venv` 并安装依赖
- 依赖：`av`、`opencv-python`、`numpy`

### 网络要求

- 发送端与接收端位于同一局域网并互通
- 默认使用 UDP 端口 `5600`
- 推荐使用 5GHz Wi-Fi

## 6. 默认配置基线

当前交付基线位于 `06_configs/`：

- `sender.default.json`: 发送端正式配置
- `sender.usb2_stable.json`: USB2 或弱性能场景基线
- `sender.weaknet.json`: 弱网调优基线
- `receiver.linux.default.json`: Linux 接收端基线
- `receiver.linux.weaknet.json`: Linux 接收端弱网基线
- `receiver.windows.default.json`: Windows 接收端基线

当前默认发送端参数：

- 分辨率：`1920x1080`
- 目标帧率：`30 fps`
- 编码：`H.264`
- 目标码率：`5000 kbps`
- 发送端口：`5600/UDP`

常用关键字段：

- 发送端：`network.remote_ip`、`network.remote_port`
- 接收端：`network.listen_ip`、`network.listen_port`
- 抖动缓冲：`network.jitter_ms`
- 编码参数：`codec.bitrate_kbps`、`codec.gop`

## 7. 最短使用路径

### Linux 到 Linux

在仓库根目录执行：

```bash
bash start_receiver_linux.sh
bash start_sender.sh 192.168.1.105
bash status.sh
bash stop.sh
```

说明：

- 先启动接收端，再启动发送端
- `192.168.1.105` 替换为接收端设备的实际 IP
- `status.sh` 会同时查看 Linux 接收端和发送端状态
- `stop.sh` 会停止 Linux 接收端与发送端
- 如果使用 Windows 接收端，请改用 `status_receiver_windows.ps1` 和 `stop_receiver_windows.ps1`

### Linux 发送到 Windows 接收

Windows 接收端在 PowerShell 中执行：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\start_receiver_windows.ps1
.\status_receiver_windows.ps1
```

或者直接使用一键菜单：

```powershell
.\one_click_windows.cmd
```

Linux 发送端执行：

```bash
bash start_sender.sh 192.168.1.105
```

## 8. 一键脚本说明

根目录脚本是交付入口，底层会调用 `05_tools/` 中的检查与启动逻辑。

Linux 侧：

- `bash start_receiver_linux.sh`: 检查 Linux 接收端环境并启动
- `bash start_sender.sh <receiver_ip>`: 检查发送端环境并启动
- `bash status.sh`: 查看 Linux 接收端和发送端状态
- `bash stop.sh`: 停止 Linux 接收端和发送端
- `bash one_click.sh`: Linux 一键菜单

Windows 侧：

- `.\start_receiver_windows.ps1`: 检查 Windows 接收端环境并启动
- `.\status_receiver_windows.ps1`: 查看 Windows 接收端状态
- `.\stop_receiver_windows.ps1`: 停止 Windows 接收端
- `.\one_click_windows.cmd`: Windows 一键菜单入口

## 9. 日常运行与运维

### 接收端 IP 变化

通常**不需要改接收端配置文件**，只需重新启动发送端并传入新的接收端 IP：

```bash
bash start_sender.sh <new_receiver_ip>
```

如需手工改配置，修改：

`06_configs/sender.default.json` 中的 `network.remote_ip`

### 日志与运行状态

Linux 发送端运行目录：

- `07_samples/runtime/sender/`
- 关键文件：`sender.runtime.json`、`sender.pid`、`sender_stdout.log`、`sender_stderr.log`

Linux 接收端运行目录：

- `07_samples/runtime/receiver_linux/`
- 关键文件：`receiver.runtime.json`、`receiver.pid`、`receiver_stdout.log`、`receiver_stderr.log`

Windows 接收端运行目录：

- `02_receiver_windows/`
- 关键文件：`receiver.runtime.json`、`receiver.pid`
- 日志目录：`02_receiver_windows/Log/`

状态查看命令：

- `bash 05_tools/status_sender.sh`
- `bash 05_tools/status_receiver_linux.sh`
- `.\05_tools\status_receiver_windows.ps1`

正常状态日志常见字段：

- `state=RUNNING`: 当前链路处于运行状态
- `fps_in`: 采集帧率
- `fps_out`: 输出/显示帧率统计
- `bitrate`: 当前估算码率
- `latency`: 估算端到端延迟
- `lost`: 累计丢包数
- `reconnect`: 累计异常重连次数

## 10. 验收结论与性能观察

根据现有交付报告，当前版本已达到以下结果：

- 发送端与接收端可稳定建立链路
- 长时间运行场景下可持续工作
- 支持 Linux / Windows 两种接收部署路径
- 已具备进入发布与现场部署阶段的条件

当前常用配置与观察结果：

- 常用链路配置：`1920x1080@30`
- 编码码率：约 `4~5 Mbps`
- 已记录的本机回环平均延迟：约 `93ms ~ 100ms`
- 在 5GHz 网络下，网络通常不是第一瓶颈

需要注意：

- 自动曝光场景下，帧率可能随曝光策略动态变化
- 当前实测帧率可能受相机链路、USB 模式、CPU 负载等因素影响
- 文档中曾记录过 `USB2.0` 链路下实际帧率低于目标帧率的情况

## 11. 当前边界与已知限制

当前版本边界如下：

- 单发送实例仅支持 `1` 个 Gemini 相机
- 多设备接入同一接收主机时，推荐稳定规划为 `5` 台并发
- 可冲刺上限约 `6` 台，`7+` 台进入明显不稳定区间
- 当前接收端核心为**软件解码**，不是硬件解码
- 多路并发时，首要瓶颈通常在接收端解码与渲染能力

## 12. 常见问题排查

### 启动失败

按下面顺序排查：

1. 先执行对应环境检查脚本，确认依赖可以导入
2. 查看 `stderr` 日志最后 50 行
3. 确认配置文件存在，IP 正确，端口未被占用

### 接收端黑屏或无画面

优先检查：

1. 接收端是否先于发送端启动
2. 发送端目标 IP 是否填写正确
3. 双方是否在同一网段
4. 防火墙是否放行 UDP `5600`

### 帧率偏低或偶发卡顿

优先检查：

1. `fps_in` 与 `fps_out` 是否明显不一致
2. 相机是否工作在 `USB3.0` 而不是 `USB2.0`
3. 发送端编码压力、接收端解码压力是否过大
4. 无线网络是否存在抖动

### 相机权限问题

如果发送端报 USB 访问权限问题，可按发送端执行手册安装 udev 规则并重新插拔相机。详见：

- [01_sender_linux/执行手册.md](01_sender_linux/执行手册.md)

## 13. 进阶使用

### 发送端 `systemd` 开机自启

Linux 发送端支持 `systemd` 开机自启，相关文件位于：

- `01_sender_linux/systemd/`

典型安装方式详见：

- [01_sender_linux/README.md](01_sender_linux/README.md)
- [01_sender_linux/执行手册.md](01_sender_linux/执行手册.md)

### 工程级运行入口

如果不是走交付包根目录脚本，也可以分别进入工程目录手工执行：

- 发送端入口：`01_sender_linux/run_sender.py`
- Linux 接收端入口：`03_receiver_linux/run_receiver.py`
- Windows 接收端入口：`02_receiver_windows/run_receiver.py`

## 14. 补充文档

- [04_docs/01_product_manual.md](04_docs/01_product_manual.md)
- [04_docs/02_quick_start.md](04_docs/02_quick_start.md)
- [04_docs/03_deploy_ops_manual.md](04_docs/03_deploy_ops_manual.md)
- [04_docs/04_technical_route.md](04_docs/04_technical_route.md)
- [06_configs/README.md](06_configs/README.md)
- [08_reports/01_test_acceptance.md](08_reports/01_test_acceptance.md)
- [08_reports/02_boundary_limits.md](08_reports/02_boundary_limits.md)
- [08_reports/03_handoff.md](08_reports/03_handoff.md)

## 15. 版本结论

当前版本已经完成“**单路 Gemini 无线视频传输 + Linux/Windows 跨平台接收 + 交付级启动运维脚本**”这一阶段目标，适合作为现场部署版本与后续升级的基础版本。

后续若继续演进，优先建议：

1. 增加接收端硬件解码路径
2. 统一 Linux/Windows 接收端代码分支
3. 推进多相机架构与并发回归测试
4. 增加 CI 自动检查与自动化验收
