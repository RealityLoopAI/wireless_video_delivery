# 部署与运维手册

## 1. 环境要求

- Python `3.9+`（推荐 `orbbec_env`）
- 依赖：`av`、`opencv-python`、`numpy`、`pyorbbecsdk`
- 网络：发送端与接收端互通，默认 UDP 端口 `5600`

## 2. 关键配置

发送端：

- 文件：`06_configs/sender.default.json`
- 核心项：`network.remote_ip`、`network.remote_port`

接收端（Linux）：

- 文件：`06_configs/receiver.linux.default.json`
- 核心项：`network.listen_ip`、`network.listen_port`

接收端（Windows）：

- 文件：`06_configs/receiver.windows.default.json`
- 核心项：`network.listen_ip`、`network.listen_port`

## 3. 日志与运行状态

发送端日志目录：

`07_samples/runtime/sender/`

接收端日志目录（Linux）：

`07_samples/runtime/receiver_linux/`

Windows 接收端日志：

`02_receiver_windows/Log/`

状态脚本：

- `bash 05_tools/status_sender.sh`
- `bash 05_tools/status_receiver_linux.sh`
- `.\05_tools\status_receiver_windows.ps1`

## 4. 常见故障排查

### 启动失败

1. 先执行环境检查脚本，确认依赖可导入。  
2. 查看 `stderr` 日志最后 50 行。  
3. 确认端口是否被占用、IP 是否可达。

### 接收端黑屏

1. 确认接收端先启动。  
2. 确认发送端目标 IP 正确。  
3. 检查双方是否在同网段，防火墙是否放行 UDP 5600。

### 帧率偏低

1. 先看发送端 `fps_in/fps_out`。  
2. 若 `fps_in` 高而 `fps_out` 低，重点看编码/CPU 压力。  
3. 多路并发时，优先评估接收端解码能力。
