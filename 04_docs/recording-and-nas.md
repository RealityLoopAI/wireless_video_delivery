# Recording And NAS

更新时间：2026-09-02

本文是录制、切片、文件收尾和 NAS 发布的唯一正式说明。

## Production Path

当前 `receiver_loop.json` 使用直接 NAS 模式：

```text
media packet
  -> per-camera reliable record queue
  -> receiver segment writer
  -> <nas_root>/.gwv3_direct_inprogress/<segment>
  -> close RGB/Depth/CSV metadata
  -> fsync files and directory
  -> same-filesystem atomic rename
  -> final camera/date/time directory
  -> recording_ready.json
```

这条链路没有“先上传到 NAS，再从 NAS 读回重封装，再写回 NAS”的生产步骤。RGB 直接交付完整 fMP4，因此停止时不再整文件重封装。

## Recording State Machine

| State | Meaning |
| --- | --- |
| `idle` | 未录制，可以开始 |
| `starting` | 已接受请求，等待统一起点和可解码 RGB 边界 |
| `recording` | 正在接收并写入有效窗口 |
| `faulted` | 本次会话因存储或一致性错误整体终止 |

`start-all` 为所有当前在线相机生成同一个 `recording_session_id` 与 `recording_window_start_global_us`。发送端提前请求 IDR，接收端只把统一起点后的有效帧计入数据集窗口。

`stop-all` 立即设置统一的 `recording_window_end_global_us`。活动 writer 与入口分离后，关闭容器、合并索引、写元数据和发布目录由后台 finalizer 完成，因此下一次录制不需要等待所有 NAS `fsync` 结束。

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

直接 NAS 模式消除了本地 staging 的持续搬运积压，但没有消除 NAS 或 SMB 的物理吞吐上限。NAS 写延迟超过实时数据生成速度时，record queue 仍会增长。持续录制必须监控：

```text
record_queue_packets
record_queue_bytes
record_queue_oldest_age_ms
record_backpressure_waits
record_write_errors
record_finalize_outstanding_segments
recording_delivery_ready
```

## Fallback Staging Mode

`recording_staging.enabled=true` 是 NAS 不适合实时直写时的回退模式：先写接收端本地盘，再由 `recording_uploader.py` 可靠复制到 NAS capture queue 并原子发布。

它能隔离 NAS 短时抖动，但要求本地平均读写和 NAS 平均写入都追得上媒体生成速度。若长期写入速度不足，backlog 一定会累积；增加 worker 只能改善并行度，不能突破磁盘、无线或 SMB 的总带宽。

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
