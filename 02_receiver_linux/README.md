# Gemini Receiver

首版接收端由一个 C++ 服务负责核心数据面：

1. UDP `50011` 接收状态 JSON。
2. TCP `50010` 接收媒体二进制 packet。
3. `127.0.0.1:18080` 暴露本地管理 HTTP API。
4. 录制时写入 NAS 根目录，默认 `/home/fz/Desktop/nas`。
5. RGB 调用 `ffmpeg` 按实测到达帧率封装为 fragmented `rgb.mp4`，降低 NAS 收尾阶段缺 `moov` 的风险；录制期间同步写 `rgb_debug.h264` 作为 MP4 修复旁路，成功校验后默认删除，只有配置开启时长期保留。
6. Depth 调用 `ffmpeg` 按实测到达帧率封装为 `depth.mkv + FFV1`；`depth_debug.raw` 仅在配置开启时保留。
7. `frames.csv` 记录媒体包索引，并追加统一的当前帧字段 `frame_id` / `timestamp_us` / `frame_system_timestamp_us` / `codec_or_compression` 用于后续对齐。
8. `meta.json` 记录编码、分辨率、请求帧率、实际帧率、帧数和 `rgb_record_fps` / `depth_record_fps`。
9. Web/REST 可持久化设置相机自命名和单路文件名前缀；录制停止返回整次录制任务的 `recording_start_us`。

构建：

```bash
cmake -S . -B 12_build -DGWV3_BUILD_RECEIVER=ON -DGWV3_BUILD_SENDER=OFF
cmake --build 12_build -j4
```

运行：

```bash
./12_build/bin/gemini_receiver --config 06_configs/receiver_ubuntu-01.json
```

管理接口：

```text
GET  /api/status
GET  /api/config
POST /api/record/start-all
POST /api/record/stop-all
POST /api/record/start?sender_id=...&camera_id=...
POST /api/record/start?sender_id=...&camera_id=...&file_prefix=...
POST /api/record/stop?sender_id=...&camera_id=...
POST /api/camera/name?sender_id=...&camera_id=...&camera_name=...
POST /api/camera/prefix?sender_id=...&camera_id=...&prefix=...
```

一键运行：

```bash
./05_tools/start_receiver.sh
./05_tools/status_receiver.sh
./05_tools/stop_receiver.sh
```

Orbbec 兼容交付导出：

```bash
./05_tools/export_orbbec_delivery.py <segment_dir> --overwrite
```
