# Configuration

更新时间：2026-09-03

本文说明配置文件的职责、选择方法和修改约束。配置文件不包含密码；现场凭据由设备本地或受控密码管理保存。

## Receiver Configurations

| File | Purpose |
| --- | --- |
| `receiver_loop.json` | 当前 `loop` 接收端生产配置，使用 DHCP、Receiver/NAS 自动发现和本地 staging |
| `receiver_ubuntu-01.json` | 旧接收端兼容配置，保留用于回退和迁移 |
| `receiver_runtime_state.json` | 显示名称、文件前缀等持久化状态初始值 |
| `audio_archive_receiver_loop.json` | 当前接收端音频归档配置 |
| `audio_archive_receiver.json` | 旧接收端音频归档兼容配置 |

`start_receiver.sh` 和 `install_receiver_autostart.sh` 在使用 `receiver_loop.json` 时会自动选择
`audio_archive_receiver_loop.json`。其他接收端仍使用兼容配置，也可以通过
`GWV3_AUDIO_ARCHIVE_CONFIG` 显式指定音频归档配置。

生产接收端当前关键值：

```text
receiver discovery UDP:     50009
media TCP:                  50010
status UDP:                 50011
clock sync UDP:             50012
admin HTTP loopback:        18080
segment_seconds:            900
recording_staging.enabled:  true
rgb_output_mode:            fragmented_mp4
```

`recording_staging.enabled=true` 表示媒体先写 Receiver 本地可靠暂存目录，再由 uploader 搬运并在 NAS 原子发布。该模式用于隔离 NAS 短时断线和 DHCP 地址变化。`nas_auto_mount` 控制自动发现、挂载状态文件和新录制门禁。

## Sender Configurations

正式设备配置：

| Device family | Configuration |
| --- | --- |
| local RK3588 | `sender_rk3588-ubuntu_one_camera.json`、`sender_rk3588-01_two_cameras.json` |
| OrangePi SV1301S | `sender_orangepi5pro-ab748372.json`、`sender_orangepi5pro-b439137c.json`、`sender_orangepi5pro-f022c4.json`、`sender_orangepi5pro-fe0ec3fe.json`、`sender_orangepi5pro-fe0f7222.json` |
| OrangePi Gemini 305 | `sender_orangepi5pro-d12a4719_gemini305.json`、`sender_orangepi5pro-fe0e946c_gemini305.json` |
| LubanCat Gemini 305 | `sender_lubancat-4df661d7_gemini305.json`、`sender_lubancat-52d2ef0c_gemini305.json`、`sender_lubancat-e8cc0cb3_gemini305.json` |

保留的实验/回退配置：

| Configuration | Purpose |
| --- | --- |
| `sender_orangepi5pro-01*.json` | 自动身份和基础模板 |
| `sender_orangepi5pro-133_four_rgb_v4l2.json` | 单机多路纯 RGB V4L2 测试 |
| `sender_orangepi5pro-f022c4_rtp_only.json` | 独立 RTP-only 回退实验 |
| `sender_rk3588-01_cam02_*.json` | 高 Depth 档位和单路诊断 |
| `sender_rk3588-01_two_cameras_swapped_test.json` | 身份映射诊断，不用于生产 |

这些文件包含曾部署设备和离线设备的稳定身份，不因设备暂时离线而删除。systemd 服务的 `ExecStart` 才决定该设备实际使用哪个文件。

## Sender Fields

| Field | Meaning |
| --- | --- |
| `sender_id` | 稳定发送端身份；生产设备优先固定值 |
| `receiver.ip` | 自动发现失败时的接收端兜底地址 |
| `receiver_discovery.*` | Receiver 自动发现端口、周期、黏性超时和上次目标状态文件 |
| `receiver.media_port` / `status_port` | 媒体与状态端口 |
| `clock_sync.*` | probe 地址、端口、周期、超时和过滤窗口 |
| `transport.media_protocol` | 当前正式值为 `tcp` |
| `web_rgb_preview.enabled` | 是否产生网页低码率预览流 |
| `hotplug.enabled` | 是否扫描同型号热插拔相机 |
| `cameras[].camera_id` | 该 sender 内稳定相机身份 |
| `cameras[].device_model` | 可接受的 SDK 型号 |
| `cameras[].serial_number` / `uid` | 可选的严格物理身份或 USB 端口约束 |
| `cameras[].rgb_profile` | RGB 宽、高、帧率和格式 |
| `cameras[].depth_profile` | Depth 宽、高、帧率和格式 |
| `cameras[].rgb_encoding` | H.264 编码器、码率和关键帧设置 |
| `cameras[].depth_transport` | Depth 压缩和量化精度 |
| `cameras[].color_controls` | 原生自动/手动曝光、增益、白平衡等 |
| `cameras[].adaptive_exposure` | 软件测光和手动曝光闭环 |
| `cameras[].rotation_degrees` | RGB/Depth 软件方向，允许 0/90/180/270 |

## Identity Rules

1. 接收端唯一 key 固定为 `<sender_id>_<camera_id>`。
2. 单相机设备可以只按 `device_model` 接受同型号替换相机。
3. 一台设备连接多台同型号相机时必须使用 `serial_number` 或稳定 `uid`，否则不能保证 camera ID。
4. `sender_id=auto` 只适合模板或已确认硬件机器标识唯一的设备。
5. 显示名称、文件前缀和 DHCP 地址不能替代身份。

## Exposure Rules

- `auto_exposure=true` 时不要同时把手动 exposure 当作实时生效值。
- `adaptive_exposure.enabled=true` 时相机原生 AE 必须关闭，由软件闭环写手动 exposure/gain。
- 为保持 30 FPS，软件曝光上限不能超过该型号 30 FPS 的实机边界。
- Gemini 305 与 SV1301S 的属性范围、曝光单位和画面响应不同，不能复制同一个数值后假设观感一致。
- 修改曝光、白平衡、旋转或 profile 后，必须读回 SDK 属性并做实际帧率与录制画面验证。

## Validation

发送端配置验证：

```bash
./12_build/bin/gemini_sender --config 06_configs/<sender-config>.json --validate-only
```

接收端配置在进程启动时严格校验。上线前还应执行：

```bash
./05_tools/sender_preflight.sh
./05_tools/status_sender.sh
./05_tools/status_receiver.sh
```

配置改动必须与对应设备 service 一起审查，防止“仓库改对了，但服务仍引用另一个配置”。
