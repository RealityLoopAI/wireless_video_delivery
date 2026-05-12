# Orbbec 兼容交付导出说明

更新时间：2026-05-12

本文档说明如何把接收端现有 segment 导出成下游 Orbbec 多相机方案更容易读取的目录结构。

## 1. 设计原则

实时录制主链路保持不变：

- RGB 仍然保存为 `rgb.mp4`，接收端不重编码。
- Depth 仍然保存为 `depth.mkv`，FFV1 / `gray16le` 无损。
- `frames.csv` 和 `meta.json` 仍然作为接收端原始索引和元数据。
- 导出文件名里的 fps 来自主文件探测结果，通常接近 `meta.json` 的 `rgb_record_fps` / `depth_record_fps`。

新增的是离线交付层：

```text
GWV3 segment -> Orbbec-compatible delivery folder
```

这样既不影响实时录制稳定性，又能给下游提供他们熟悉的 `session.json`、相机级 JSON、Depth CSV 和预览视频。

## 2. 导出命令

工具路径：

```bash
05_tools/export_orbbec_delivery.py
```

示例：

```bash
python3 05_tools/export_orbbec_delivery.py \
  /home/fz/Desktop/nas/orangepi5pro-01_cam01/2026-05-11/162546 \
  --overwrite
```

默认输出在 segment 目录内：

```text
<segment>/orbbec_delivery/<session_name>/
```

也可以指定输出根目录：

```bash
python3 05_tools/export_orbbec_delivery.py \
  /home/fz/Desktop/nas/orangepi5pro-01_cam01/2026-05-11/162546 \
  -o /home/fz/Desktop/nas/orbbec_exports \
  --overwrite
```

## 3. 导出目录结构

默认导出：

```text
<output_dir>/<session_name>/
├── session.json
├── camera_00_<serial>_rgb_<W>x<H>_<fps>fps.mp4
├── camera_00_<serial>_rgb_<W>x<H>_<fps>fps.json
├── camera_00_<serial>_depth_<W>x<H>_<fps>fps.mkv
├── camera_00_<serial>_depth_frames.csv
└── camera_00_<serial>_depth_preview_<fps>fps.mp4
```

说明：

- RGB 文件来自原始 `rgb.mp4`。
- Depth 文件来自原始 `depth.mkv`。
- 默认用硬链接安装媒体文件，跨文件系统失败时自动复制。
- `depth_preview` 是从 `depth.mkv` 离线生成的伪彩色 MP4，仅用于人工检查。

## 4. 可选 Depth PNG

如果下游明确要求逐帧 `uint16 PNG`，可以打开：

```bash
python3 05_tools/export_orbbec_delivery.py \
  <segment_dir> \
  --export-depth-png \
  --overwrite
```

会额外生成：

```text
camera_00_<serial>_depth_png/
├── camera_00_depth_00000000.png
├── camera_00_depth_00000001.png
└── ...
```

不建议默认开启，因为逐帧 PNG 会制造大量小文件，对 NAS 和 IO 压力明显更高。

## 5. `depth_frames.csv`

导出的 CSV 采用下游方案类似字段：

```csv
frame_index,timeline_frame_index,timeline_offset_frames,depth_file,depth_video_frame_index,video_pts_sec,planned_host_epoch,planned_host_utc,emit_host_epoch,emit_host_utc,source_host_epoch,source_host_utc,source_monotonic_sec,device_timestamp_us,depth_source_seq,duplicated,placeholder,depth_width,depth_height,depth_value_scale,min_depth_mm,max_depth_mm,valid_pixel_ratio,write_start_epoch,write_end_epoch,write_latency_ms,write_ok,error,receiver_local_time_us,source_system_timestamp_us,payload_size
```

其中：

- `depth_file` 默认指向 depth MKV 文件。
- `depth_video_frame_index` 表示该行对应 `depth.mkv` 中的第几帧。
- 如果启用 `--export-depth-png`，`depth_file` 改为对应 PNG 相对路径。
- `emit_host_epoch` 来自接收端写入 `frames.csv` 的本地时间。
- `device_timestamp_us` 来自发送端 SDK depth timestamp。
- `source_system_timestamp_us` 来自协议头里的发送端 system timestamp。老 segment 没有该列时会留空。

## 6. 接收端 CSV 增量字段

从本版本开始，接收端 `frames.csv` 在旧字段后追加：

```csv
packet_system_timestamp_us,rgb_system_timestamp_us,depth_system_timestamp_us
```

旧字段顺序保持不变，老解析逻辑不会受到影响。新增字段用于后续更可靠地做 RGBD 时间线对齐。
