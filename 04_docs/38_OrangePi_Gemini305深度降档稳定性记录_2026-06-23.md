# OrangePi Gemini305 深度降档稳定性记录

日期：2026-06-23

## 背景

OrangePi `orangepi5pro-d12a4719` 连接 Gemini305 后，原始运行档位为：

```text
RGB:   1280x800@30 MJPG -> H.264, 约 6Mbps
Depth: 1280x800@30 y16 -> pq8zlib, quantization_step_mm=10
```

该档位在发送端采集侧可接近 30fps，但深度压缩后码率长期处在 `50-70Mbps`，在 Wi-Fi 链路波动时会快速填满 TCP 发送队列。现场表现为：

- OrangePi 发送端 `Send-Q` 堆到约 `33MB`。
- `media TCP packet dropped under backpressure` 反复出现。
- RGB 非 IDR 帧被 keyframe guard 丢弃，RGB 有时掉到 0-几 fps。
- 接收端网页 RGB 延迟、卡顿或短时不刷新。

切换到更强的 5GHz AP 后，短时间能恢复满帧，但队列仍会继续增长，说明根因不是单纯信号强弱，而是当前深度码率缺少长期吞吐余量。

## 本次处理

将 OrangePi Gemini305 的深度档位降为：

```text
Depth: 320x200@30 y16 -> pq8zlib, quantization_step_mm=10
```

RGB 保持不变：

```text
RGB: 1280x800@30 MJPG -> H.264, 约 6Mbps
```

设备本地配置文件：

```text
/home/orangepi/Desktop/wireless_video_delivery/06_configs/sender_orangepi5pro-d12a4719_gemini305.json
```

降档前自动备份：

```text
/home/orangepi/Desktop/wireless_video_delivery/06_configs/sender_orangepi5pro-d12a4719_gemini305.json.bak_20260623_175812
```

仓库同步配置：

```text
06_configs/sender_orangepi5pro-d12a4719_gemini305.json
```

## 验证结果

发送端启动确认：

```text
camera started camera_id=cam01 color=1280x800@30 format=mjpg depth=320x200@30 format=y16 connection=USB3.2
```

发送端日志中，深度链路变化如下：

```text
降档前 depth_usb_mbps 约 490Mbps，depth_mbps 约 50-70Mbps，depth_compress_avg_ms 约 15-23ms
降档后 depth_usb_mbps 约 30.6Mbps，depth_mbps 约 7.1-7.6Mbps，depth_compress_avg_ms 约 3-5ms
```

接收端 10 秒采样：

```text
orangepi5pro-d12a4719_cam01 depth=320x200 age_ms=25
RGB:   30.0fps / 6.0Mbps
Depth: 30.0fps / 7.3Mbps
```

OrangePi 媒体 TCP 队列：

```text
Send-Q: 0
```

## 结论

对当前现场网络和接收端压力来说，OrangePi Gemini305 的 `1280x800@30` 深度档位过重。降到 `320x200@30` 后，RGB 和 Depth 均恢复 30fps，深度码率从 `60-70Mbps` 级别降到约 `7Mbps`，TCP 回压消失。

当前建议将该配置作为 OrangePi Gemini305 的稳定默认档位。若后续需要恢复高分辨率深度，应先完成更强的深度专用压缩或给该发送端提供独占 AP/有线链路余量，再进行 A/B 验收。

## 回退方式

如需回退到高深度档，修改配置中的 `depth_profile`：

```json
"depth_profile": {
  "width": 1280,
  "height": 800,
  "fps": 30,
  "format": "y16"
}
```

回退后必须重新观察：

- `depth_mbps`
- `ss -tin` 中媒体 TCP `Send-Q`
- `media TCP packet dropped under backpressure`
- RGB 是否仍能稳定 30fps
