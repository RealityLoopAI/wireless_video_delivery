# Gemini Receiver

接收端由一个 C++ 服务负责核心数据面。完整部署说明见 [../04_docs/04_部署与运行手册.md](../04_docs/04_部署与运行手册.md)，接口和数据格式见 [../04_docs/05_接口与数据格式参考.md](../04_docs/05_接口与数据格式参考.md)。

1. UDP `50011` 接收状态 JSON。
2. TCP `50010` 接收媒体二进制 packet。
3. UDP `50012` 处理 CLOCK_SYNC probe/response，并维护每个 sender 的 offset/delay/drift 模型。
4. `127.0.0.1:18080` 暴露本地管理 HTTP API。
5. 录制时先写 receiver 本地 staging；独立 uploader 校验后原子发布到 NAS，默认 `/home/fz/Desktop/nas`。
6. RGB 调用 `ffmpeg` 按实测到达帧率封装为 fragmented `rgb.mp4`；`rgb_debug.h264` 只有 `write_debug_h264=true` 时才写入。
7. Depth 调用 `ffmpeg` 按实测到达帧率封装为 `depth.mkv + FFV1`；`depth_debug.raw` 仅在配置开启时保留。
8. `start-all` 为所有相机分配同一 `recording_session_id` 和未来全局起点，并提前 1 秒收集 H.264 可解码预滚。下游只使用 `recording_window_valid=1` 的行，不把 MP4 第 0 帧当作跨相机统一起点。
9. `meta.json` 记录编码、分辨率、请求帧率、实际帧率、帧数和 `rgb_record_fps` / `depth_record_fps`。
10. Web/REST 可持久化设置相机自命名和单路文件名前缀；录制停止返回整次录制任务的 `recording_start_us`。
11. `preview_enabled=false` 可关闭接收端 RGB/Depth 预览解码与伪彩生成；`preview_enabled=true` 时预览也按客户端请求触发，未打开网页或未访问预览接口时不主动解码/生成伪彩。多设备录制压力较高时建议关闭预览，仅保留采集、传输和落盘。
12. 停止录制时先分离旧 writer；MP4/CSV 收尾和 NAS 发布在后台继续。收尾期间可以立即再次点击开始，分离未完成时请求会以 `start_pending=true` 排队并自动启动。
13. `gwv3-recording-uploader.service` 与 receiver 分离运行；NAS 临时不可用时保留本地 backlog，上传恢复不阻塞媒体线程。

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
POST /api/storage/prefix?prefix=...
POST /api/preview/main-target?sender_id=...&camera_id=...
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
