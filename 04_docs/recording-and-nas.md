# Recording And NAS

更新时间：2026-09-10

本文是录制、切片、文件收尾和 NAS 发布的唯一正式说明。

## Production Path

当前 `receiver_loop.json` 使用本地 staging + NAS uploader 模式：

```text
media packet
  -> per-camera reliable record queue
  -> receiver segment writer
  -> <recording_staging.root>/<segment>
  -> close RGB/Depth/CSV metadata and local ready marker
  -> uploader hidden NAS capture queue
  -> NAS finalization and atomic publication
  -> final camera/date/time directory
  -> recording_ready.json
```

RGB 本地直接写完整 fMP4，停止时不做整文件重封装。uploader 可以在录制期间增量镜像，结束后只补齐尾部、验证并发布；NAS 断线时本地录制继续，恢复后补传。

## Recording State Machine

| State | Meaning |
| --- | --- |
| `idle` | 未录制，可以开始 |
| `starting` | 已接受请求，等待统一起点和可解码 RGB 边界 |
| `recording` | 正在接收并写入有效窗口 |
| `faulted` | 本次会话因存储或一致性错误整体终止 |

`start-all` 为所有当前在线相机生成同一个 `recording_session_id` 与 `recording_window_start_global_us`。发送端提前请求 IDR，接收端只把统一起点后的有效帧计入数据集窗口。

`stop-all` 立即设置统一的 `recording_window_end_global_us`。活动 writer 与入口分离后，关闭容器、合并索引、写元数据和发布目录由后台 finalizer 完成，因此下一次录制不需要等待所有 NAS `fsync` 结束。

分离 writer 之前，先等待各主媒体流越过该结束边界，继续保留窗口内的迟到帧，
默认最多等待 120 秒。此等待不阻塞停止 API，但同相机的新录制需要等待旧任务分离；
不能将“开始请求已接受”理解成“新任务已经写入”。超时会如实标记 `partial`。
这一步不等于 NAS 搬运，也不能找回 sender 已丢弃的帧。

## Segment Rotation

默认每 900 秒切片。切片边界按会话全局时间轴计算，而不是每路 writer 各自启动后计时：

```text
window_start + N * segment_seconds
```

边界前 receiver 请求定时关键帧，sender 按 CLOCK_SYNC offset 换算到本机采集时间。新段从可独立解码的 SPS/PPS/IDR 开始。网络抖动或相机相位可能带来约一帧边界差异，因此“文件开始时间相同”不等于内容硬同步；下游仍按 `global_timestamp_us` 做最近邻匹配。

切片轮转的完整性要求：

- 上一段和下一段都有明确全局窗口。
- `frames.csv` 保留落盘视频索引和窗口有效标记。
- 不因为后台 finalizer 慢而停止当前 writer；达到 finalizer 上限时延后轮转，不静默丢弃当前数据。
- 掉线重连加入当前全局 segment index，不重新建立独立 15 分钟周期。

## File Formats

| File | Meaning |
| --- | --- |
| `rgb.mp4` | H.264 fragmented MP4，含 `moov`、`moof`、`mfra` |
| `depth.mkv` | FFV1 封装的 `uint16` Depth |
| `frames.csv` | 包、帧、时间戳、录制索引和窗口字段 |
| `meta.json` | 会话、分片、帧数、实际速率和状态 |
| `calibration.json` | RGBD 内参、外参、Depth scale |
| `audio.opus` | 有真实音频 RTP 输入时的任务音频 |
| `audio_timing.csv` / `audio_meta.json` | 音频时间轴与质量信息 |
| `recording_ready.json` | 该最终目录可交付的唯一完成标记 |

fMP4 可由 VLC、mpv、ffplay 和 FFmpeg 系列工具直接播放与定位。极旧播放器如果不支持 fragmented MP4，可在非生产环境切换 `conventional_mp4`，代价是停止后整文件重封装时间显著增加。

## RGB Capture Timeline

2026-09-10 新增 RGB 采集时间轴封装。现场是否启用必须核对部署版本，不能据本文推断已上线。
`meta.json` 包含 `rgb_timestamp_mode=capture_global_vfr_v1` 的新分片使用：

```text
RGB MP4 sample PTS (us) = global_timestamp_us - rgb_pts_origin_global_us
```

帧间缺口和首帧晚到偏移会保留；不再把所有已收到帧连续排列成固定 30fps。
最后一帧只保留一个标称帧周期，不凭空补齐尾部缺录。播放器可能停留在上一帧，
不代表该缺口真的采集到了画面。正式对齐仍应通过 CSV 及有效性字段选择真实帧。

实现仅在 receiver 内部用 NUT 将逐帧 PTS 传给 FFmpeg，再无重编码封装到 MP4，
不修改 TCP 媒体协议。普通 MP4 收尾与调试恢复也保留时间戳。
`write_debug_h264=true` 时额外生成 `rgb_recovery.nut`，用于带时间轴恢复；默认不开启该副本。
接收端编译新增 `libavformat-dev`、`libavcodec-dev`、`libavutil-dev` 依赖。

旧分片不会自动改写；Depth 当前仍使用原有封装路径，不能用 Depth 播放进度替代 CSV 时间。
本修复不恢复网络或采集已丢失的帧，也不改变 CLOCK_SYNC 的可信度判定。
详细验证与回退见 [RGB Recording PTS](rgb-recording-pts-vfr-20260910.md)。

## Atomic Visibility

下游只扫描正式目录，并且只消费存在且内容有效的 `recording_ready.json` 的分片。

`.gwv3_direct_inprogress` 中的目录尚未发布，可能仍在写入或等待恢复。接收端重启后只自动发布已经有合法 ready 元数据的完整目录；不完整目录留给人工审计，不伪装成成功录像。

同名正式目录不会被覆盖。隐藏目录和正式目录必须位于同一挂载文件系统，否则原子 rename 不成立，receiver 拒绝开始分片。

## Backpressure And Capacity

录制可靠性优先级高于预览：

1. 每路相机有独立可靠录制队列。
2. Web 预览允许丢旧帧，不能反压录制。
3. 接收端还有全局队列字节上限和磁盘保留水位。
4. 存储故障触发整次 session `faulted`，不在同一 session 中悄悄续录。

本地 staging 隔离 NAS 短时抖动，但没有消除 NAS 或 SMB 的物理吞吐上限。NAS 长期写入低于媒体生成速度时，backlog 仍会增长。持续录制必须监控：

```text
record_queue_packets
record_queue_bytes
record_queue_oldest_age_ms
record_backpressure_waits
record_write_errors
record_finalize_outstanding_segments
recording_delivery_ready
```

## Direct NAS Compatibility Mode

`recording_staging.enabled=false` 保留为受控环境下的直接 NAS 兼容模式。它依赖 NAS 全程在线，不能满足现场断线后继续本地录制的要求。

生产 staging 模式要求本地平均写入和 NAS 平均搬运都追得上媒体生成速度。若长期写入速度不足，backlog 一定会累积；增加 worker 只能改善并行度，不能突破磁盘、网络或 SMB 的总带宽。

## Task Audio

视频任务音频按同一 `recording_session_id`、`segment_window_start_global_us` 和结束窗口生成，并复制到每个相机分片目录。静音也是有效 PCM/Opus 内容，应保留；只有完全没有收到音频 RTP 包时才不生成 `audio.opus`，同时写 `quality_status=no_input`。

下游先检查 `audio_ready.json`，再按 `audio_meta.json` 中的 `complete`、`partial` 或 `no_input` 决定是否进入训练集。为保持时间轴连续而补的静音包会记录在 `silence_packets`，不能冒充真实采集包。

## Acceptance Checks

停止后依次确认：

1. receiver 的 record queue 和 finalizer 计数归零。
2. 最终目录出现 `recording_ready.json`。
3. `ffprobe` 能解析 RGB 和 Depth；RGB 可以 seek。
4. `frames.csv` 中 `rgb_video_frame_index` 连续且可映射到实际解码帧。
5. 多路 `recording_session_id`、窗口起止和 segment index 一致。
6. 运行 `sync_input_guard.py --verify-video-frames` 与 `analyze_segment_fps.py`。

异常断电只能保证已完成原子发布的分片。正在写入的最后一个隐藏分片可能需要恢复或丢弃，这不是正常停止路径的“封装失败”。
