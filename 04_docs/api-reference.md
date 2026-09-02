# API And Data Format Reference

更新时间：2026-09-02

本文是当前对外端口、媒体协议、REST API 和落盘字段的查表文档。配置字段见 [configuration.md](configuration.md)。

## Ports

| Port | Protocol | Direction | Purpose |
| --- | --- | --- | --- |
| 50010 | TCP | sender -> receiver | RGB、Depth、低码率预览和 MJPEG 快照 |
| 50011 | UDP | sender <-> receiver | 状态、事件和控制 |
| 50012 | UDP | sender <-> receiver | CLOCK_SYNC probe/response |
| 50013 | UDP | optional | 实验媒体 UDP，生产默认关闭 |
| 50014 | UDP | optional | 实验预览 UDP，生产默认关闭 |
| 18080 | HTTP | receiver loopback | C++ admin API |
| 8080 | HTTP | LAN client -> receiver | Web Monitor 与公开 REST 代理 |
| 18083 | HTTP | receiver loopback | 音频归档 admin API |
| 50130 | UDP | audio sender -> receiver | 音频 timing/control report |

音频 RTP 端口按 sender 配置分配，必须唯一。不要把 18080 暴露到局域网或公网；远程调用统一使用 8080。

## Identity

| Field | Rule |
| --- | --- |
| `sender_id` | 1-64 位 ASCII 字母、数字、`_`、`-` |
| `camera_id` | 同上，在同一 sender 内唯一 |
| `camera_key` | `<sender_id>_<camera_id>` |
| `camera_name` | 显示/存储别名，不改变原始身份 |
| `camera_file_prefix` | 文件名前缀，不改变身份 |

## Media Packet V2

TCP 媒体包为：

```text
fixed header (134 bytes)
sender_id bytes
camera_id bytes
codec_or_compression bytes
payload bytes
```

所有整数使用 little-endian。固定头关键字段：

| Field | Meaning |
| --- | --- |
| magic | `GWV3` / `0x33565747` |
| version | `2` |
| stream_type | RGB、Depth、preview 或 snapshot |
| flags | key frame、drop、system time、诊断和方向标记 |
| frame_id | 当前流内帧号，重启后可以归零 |
| timestamp_us | 相机/SDK 原始帧时间 |
| system_timestamp_us | sender 在采集处绑定的系统时间 |
| pair_id | sender 内 RGBD 配对标识 |
| width / height | 当前流尺寸 |
| payload_size | 编码/压缩 payload 大小 |
| uncompressed_size | Depth 解压后大小 |
| rgb_exposure_us / rgb_gain | SDK 读回的曝光诊断 |
| sender_*_timestamp_us | 采集、绑定、编码和入队阶段诊断时间 |

完整布局以 `03_common_core/include/gwv3_common/protocol.hpp` 为唯一源码定义。解析器必须检查 magic、版本、header size、字符串长度、尺寸、payload 上限和整数溢出。

## Stream Types

| Value | Name | Payload |
| --- | --- | --- |
| 1 | `rgb` | Annex-B H.264 主码流 |
| 2 | `depth_raw` | 原始或配置压缩后的 `uint16` Depth |
| 3 | `rgb_preview` | 低码率 H.264 预览 |
| 4 | `rgb_snapshot` | 带 request ID 的单帧 MJPEG |

Depth 的 `codec_or_compression` 可为 `none`、`zlib`、`qdelta`、`pq12zlib`、`q8lz4`、`pq8zlib` 或 `pq8lz4`。量化模式必须同时读取实际 `quantization_step_mm`，不能只按文件扩展名推断精度。

## Status Messages

UDP 50011 使用 JSON，`protocol_version` 当前为 `3.0`。常见 `message_type`：

```text
sender_hello
camera_announce
heartbeat
camera_offline
clock_sync_report
event
control
```

状态消息携带 sender/camera 身份、运行版本、FPS、码率、队列、相机属性、时间同步和错误计数。实际字段以 sender `base_message()` 和 `GET /api/status` 为准，消费者必须容忍新增字段。

## Clock Sync Messages

Probe：

```json
{
  "protocol_version": "3.0",
  "message_type": "clock_sync_probe",
  "sender_id": "example-sender",
  "sequence": 123,
  "t1_sender_send_us": 1710000000000000
}
```

Response：

```json
{
  "protocol_version": "3.0",
  "message_type": "clock_sync_response",
  "sender_id": "example-sender",
  "sequence": 123,
  "t1_sender_send_us": 1710000000000000,
  "t2_receiver_recv_us": 1710000000001200,
  "t3_receiver_send_us": 1710000000001300
}
```

sender 在本地记录 t4 并通过 heartbeat/report 上报 `clock_sync_valid`、`clock_offset_us`、`clock_delay_us`、`clock_drift_ppm` 与 `clock_last_sync_us`。详细语义见 [clock-sync.md](clock-sync.md)。

## REST Base URL

局域网调用：

```text
http://<receiver-ip>:8080
```

Web Monitor 对受信任采集局域网默认无 token。它代理 loopback admin API，并对短时 admin 超时提供有限的只读 status cache；写操作绝不会用缓存伪造成功。

## Status And Configuration

```http
GET /api/status
GET /api/config
```

`/api/status` 包含：

- sender/camera 在线状态、profile、FPS、Mbps 和媒体 age；
- 录制状态、session、窗口、队列、finalizer 和交付状态；
- CLOCK_SYNC model；
- Web preview 与 H.264 stream 状态；
- build commit/source hash/dirty；
- 音频摘要与最近错误。

## Recording

```http
POST /api/record/start-all[?file_prefix=...]
POST /api/record/stop-all
POST /api/record/start-sender?sender_id=...
POST /api/record/stop-sender?sender_id=...
POST /api/record/start?sender_id=...&camera_id=...[&file_prefix=...]
POST /api/record/stop?sender_id=...&camera_id=...
```

成功响应包含 `ok=true`。全局开始还返回统一 `recording_session_id`、`recording_start_us` 和可能的 `start_pending`。停止返回 `recording_end_global_us`；该响应表示停止边界已接受，不等于所有容器已经完成 NAS 发布。

GPIO 录制按键调用 sender 级接口，只控制该物理 sender 的全部相机。

## Camera Naming

```http
POST /api/camera/name?sender_id=...&camera_id=...&camera_name=...
POST /api/camera/prefix?sender_id=...&camera_id=...&prefix=...
POST /api/storage/prefix?prefix=...
```

名称和前缀只能使用安全文件名字符，不能改变原始 sender/camera ID。

## Preview Images

```http
GET  /api/preview/rgb?sender_id=...&camera_id=...
GET  /api/preview/rgb-main?sender_id=...&camera_id=...
GET  /api/preview/depth?sender_id=...&camera_id=...
POST /api/preview/main-target?sender_id=...&camera_id=...
```

这些接口返回当前最新图像快照，只用于监控。客户端应设置 no-cache 并允许 404/503，不能因预览暂时不可用判定正式录制失败。

## Live H.264 Frames

```http
GET /api/preview/rgb-h264-frames
    ?sender_id=...
    &camera_id=...
    &quality=preview|main
    &metadata=legacy|global
```

- `quality=preview`：低码率预览流。
- `quality=main`：复用原始 H.264 主码流，不重编码，适合下游临时获取全画质 30 FPS。
- `metadata=legacy`：GWHP v1，40 字节头。
- `metadata=global`：GWHP v2，48 字节头，包含 global timestamp。

每个输出单元为 `GWHP header + Annex-B payload`，little-endian：

| Offset | Type | Field |
| --- | --- | --- |
| 0 | 4 bytes | `GWHP` |
| 4 | u16 | version |
| 6 | u16 | header_size，必须按此定位 payload |
| 8 | u32 | payload_size |
| 12 | u32 | flags: bit0 key, bit1 config, bit2 clock valid |
| 16 | u32 | width |
| 20 | u32 | height |
| 24 | u64 | sender frame timestamp |
| 32 | u64 | stream sequence |
| 40 | u64 | `global_timestamp_us`，仅 v2 |

每路建立独立 HTTP 连接。客户端必须持续读取并自行解码 H.264，不能积压后慢速消费；达到并发上限或预览尚未准备时返回 503。主码流会增加 receiver 到下游的总网络带宽，但不增加 sender 编码负担。

## Audio Control

```http
GET  /api/audio/status
POST /api/audio/start-all
POST /api/audio/stop-all
POST /api/audio/start-sender?sender_id=...
POST /api/audio/stop-sender?sender_id=...
```

这些接口代理音频归档 admin 服务。音频归档是否开机自动开始由配置和服务状态决定，与视频录制按钮分离。

## RGB Snapshot Result

语音服务通过本地请求文件提交 `request_id`、`burst_id`、序号和目标采集时间。接收端持久化成功后，通过状态控制消息返回：

```json
{
  "message_type": "control",
  "control": "rgb_snapshot_result",
  "sender_id": "example-sender",
  "camera_id": "cam01",
  "request_id": "request-01of03",
  "burst_id": "request",
  "burst_index": 1,
  "burst_count": 3,
  "ok": true,
  "status": "captured",
  "orientation_applied_degrees": 180,
  "image_path": "<receiver-local-staging-path>"
}
```

`captured` 表示 receiver 本地可靠副本完成，不表示照片已发布到 NAS。

## Frames CSV

旧列不删除，新字段追加。下游关键列：

| Field | Meaning |
| --- | --- |
| `stream_type` | RGB 或 Depth |
| `frame_id` / `timestamp_us` | 原始流帧号与 SDK 时间 |
| `frame_system_timestamp_us` | sender 采集绑定时间 |
| `receiver_receive_timestamp_us` | receiver 完整收包时间 |
| `clock_sync_valid` | global time 模型是否有效 |
| `sender_offset_us` / `sender_delay_us` / `sender_drift_ppm` | 该行 clock model |
| `global_timestamp_us` | 统一时间轴 |
| `rgb_recorded` | RGB 是否进入视频 |
| `rgb_video_frame_index` | RGB 在 MP4 解码帧序号 |
| `recording_window_valid` | 是否处于数据集有效窗口 |
| `recording_session_id` | 多路统一会话 |
| `segment_window_*_global_us` | 逻辑切片窗口 |

消费者必须按 header 名称解析 CSV，不能依赖固定列号。

## Ready Contract

最终录像目录只有在存在有效 `recording_ready.json` 时可消费。隐藏目录、`.inprogress` 文件和仅含 `recording_staged.json` 的目录都不是正式交付。

任务音频先检查 `audio_ready.json` 和 `audio_meta.json`。照片最终目录位于 `voice_photos/<camera-key>/...`，只保留完成发布的 JPG。

## Compatibility

- REST 和 CSV 允许新增字段，客户端应忽略未知字段。
- 不删除旧字段，不覆盖原始 timestamp。
- H.264 客户端必须读取 `header_size`，兼容 GWHP v1/v2。
- `frame_id` 不是跨重启全局序号。
- 网页预览、实时主码流和正式录制是不同交付语义。
