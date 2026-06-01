# Orbbec 兼容交付导出说明

更新时间：2026-05-29

本文档说明如何把接收端现有 segment 导出成下游 Orbbec 多相机方案更容易读取的目录结构。

## 1. 设计原则

实时录制主链路保持不变：

- RGB 仍然保存为 `rgb.mp4`，接收端不重编码。
- Depth 仍然保存为 `depth.mkv`，FFV1 / `gray16le` 无损。
- `frames.csv`、`meta.json` 和 `calibration.json` 仍然作为接收端原始索引、元数据和空间标定来源。
- 导出文件名里的 fps 来自主文件探测结果，通常接近 `meta.json` 的 `rgb_record_fps` / `depth_record_fps`。
- 如果下游需要 RGB 坐标系下的 Depth，可在录制完成后手动运行 `05_tools/run_align_depth_to_rgb.sh` 生成 `depth_aligned_to_rgb.mkv`。

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
  /home/fz/Desktop/nas/orangepi5pro-b439137c_cam02/2026-05-11/162546 \
  --overwrite
```

默认输出在 segment 目录内：

```text
<segment>/orbbec_delivery/<session_name>/
```

也可以指定输出根目录：

```bash
python3 05_tools/export_orbbec_delivery.py \
  /home/fz/Desktop/nas/orangepi5pro-b439137c_cam02/2026-05-11/162546 \
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

## 5. 可选 RGB 对齐深度

接收端实时录制阶段只保存原始 Depth 母版，不自动生成 `depth_aligned_to_rgb`。如果需要空间对齐，可在录制完成后手动运行：

```bash
05_tools/run_align_depth_to_rgb.sh /path/to/segment_dir
```

输出：

```text
depth_aligned_to_rgb.mkv
depth_aligned_to_rgb.json
```

该脚本使用 `calibration.json` 中的 RGB/Depth 内参、畸变参数和 `d2c_transform`。输出分辨率与 RGB 相同，像素格式仍为 `gray16le`，深度值单位沿用 `depth_profile.depth_scale`。成功后会更新 `calibration.json.aligned_depth`。脚本会拒绝内参尺寸与当前视频/profile 不一致、D2C 外参缺失或全 0 的数据；如果 SDK 只给出某个分辨率组合的完整标定，应使用该已标定组合录制或先补齐目标分辨率对应的标定。

当前 RK3588 现场已验证的空间对齐采集组合是 `RGB 640x480 + Depth 640x400`。发送端应使用 `06_configs/sender_rk3588-01_two_cameras_align.json`，并在录制前运行 `05_tools/check_sender_alignment_ready.sh` 确认接收端状态中 `calibration_available=true`。默认高画质 `RGB 1920x1080 + Depth 640x400` 不作为 aligned depth 交付录制规格。

## 6. `depth_frames.csv`

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

## 7. 接收端 CSV 增量字段

从本版本开始，接收端 `frames.csv` 在旧字段后追加：

```csv
packet_system_timestamp_us,rgb_system_timestamp_us,depth_system_timestamp_us,frame_id,timestamp_us,frame_system_timestamp_us,codec_or_compression
```

旧字段顺序保持不变，老解析逻辑不会受到影响。

新增字段说明：

- `frame_id`：当前行所属流自己的帧号。
- `timestamp_us`：当前行所属流自己的设备/SDK timestamp。
- `frame_system_timestamp_us`：当前行所属流自己的发送端 system timestamp，推荐作为跨设备/跨相机对齐入口。
- `codec_or_compression`：当前行媒体包的编码或压缩方式。
