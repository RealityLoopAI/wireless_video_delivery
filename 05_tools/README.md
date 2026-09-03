# Operations And Data Tools

`05_tools` 保存构建启动、systemd 安装、状态检查、网络调优和录制数据分析工具。除特别说明外，命令从仓库根目录执行。

## Service Lifecycle

| Tool | Purpose |
| --- | --- |
| `start_receiver.sh [config]` | 构建并启动 receiver、Web Monitor 和所需辅助服务 |
| `status_receiver.sh [config]` | 只读查看 receiver、Web、录制队列和近期日志 |
| `stop_receiver.sh [config]` | 请求安全停止录制并停止 receiver 相关服务 |
| `start_sender.sh [config]` | 预检并由 watchdog 启动已构建的 sender |
| `start_sender_preview.sh [config]` | 以本机 OpenCV 预览模式启动 sender |
| `status_sender.sh` | 只读查看 watchdog、sender 进程、socket 和近期日志 |
| `stop_sender.sh` | 停止 sender watchdog 和子进程 |
| `run_sender_foreground.sh <config>` | 前台运行，适合现场调试 |

启动脚本可能重新构建并改变运行进程。录制期间优先使用 `status_*`，不要把启动脚本当健康检查。

## Deployment And Hardware

| Tool | Purpose |
| --- | --- |
| `install_receiver_autostart.sh` | 安装 receiver 用户级自启动服务 |
| `install_receiver_nas_auto_mount.sh` | 安装 Receiver 的 NAS 自动发现、挂载和健康检查服务 |
| `install_nas_discovery_beacon.sh` | 在配套 NAS 安装局域网发现 beacon |
| `install_receiver_network_tuning.sh` | 安装 receiver 收包队列/RPS 调优 |
| `install_sender_wifi_tuning.sh` | 为匹配驱动安装发送队列限制 |
| `sender_preflight.sh [config]` | 检查配置、SDK、编码器、网络和设备条件 |
| `prepare_rk3588_usb.sh` | 应用 RK3588 USB 现场准备项 |
| `orbbec_runtime_guard.sh` | 检查 Orbbec 运行时依赖 |
| `orbbec_fps_probe.cpp` | 独立探测相机 profile 实际 FPS |
| `orbbec_depth_probe.cpp` | 独立探测 Depth 数据 |

网络与 USB 工具可能需要 `sudo`。执行前先阅读脚本，并确认目标网卡、驱动和板型匹配。

## Time And Recording

| Tool | Purpose |
| --- | --- |
| `setup_receiver_chrony_server.sh` | 配置 receiver chrony 基准 |
| `setup_sender_chrony_client.sh` | 配置 sender 向 receiver 校时 |
| `wait_chrony_sync.sh` | 等待并验证 chrony 状态 |
| `gwv3_receiver_cli.py` | 调用接收端录制控制 API |
| `recording_uploader.py` | 生产 staging 模式的 NAS 增量搬运和原子发布器 |
| `audio_archive_receiver.py` | 接收和切分持续音频归档 |
| `photo_uploader.py` | 原子发布语音快照到 NAS |

## Analysis And Export

| Tool | Purpose |
| --- | --- |
| `analyze_segment_fps.py` | 分析单段媒体/CSV 帧率和完整性 |
| `sync_input_guard.py` | 多机对齐前检查输入完整性与时间字段 |
| `analyze_rgb_timestamp_sync.py` | 分析多路 RGB 时间差 |
| `build_rgb_sync_manifest.py` | 生成 RGB 对齐清单 |
| `align_depth_to_rgb.py` | 基于标定参数生成 RGB 坐标系 Depth |
| `export_orbbec_delivery.py` | 导出下游兼容目录 |
| `audit_silent_audio_archive.py` | 区分静音音频与无数据缺失 |
| `depth_compression_bench.cpp` | 离线比较 Depth 压缩模式 |

这些工具不会替代 `recording_ready.json` 完整性门槛。涉及重写或导出的命令应先输出到新目录，避免覆盖唯一原始数据。

完整流程见 [deployment.md](../04_docs/deployment.md)、[testing-and-release.md](../04_docs/testing-and-release.md) 和 [troubleshooting.md](../04_docs/troubleshooting.md)。
