# Gemini Receiver

首版接收端由一个 C++ 服务负责核心数据面：

1. UDP `50011` 接收状态 JSON。
2. TCP `50010` 接收媒体二进制 packet。
3. `127.0.0.1:18080` 暴露本地管理 HTTP API。
4. 录制时写入 NAS 根目录，默认 `/home/fz/Desktop/nas`。
5. RGB 调用 `ffmpeg` 封装为 `rgb.mp4`，同时保留 `rgb_debug.h264`。
6. Depth 调用 `ffmpeg` 封装为 `depth.mkv + FFV1`，同时保留 `depth_debug.raw`。
7. `frames.csv` 记录媒体包索引，并追加发送端 system timestamp 字段用于后续对齐。

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
POST /api/record/stop?sender_id=...&camera_id=...
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
