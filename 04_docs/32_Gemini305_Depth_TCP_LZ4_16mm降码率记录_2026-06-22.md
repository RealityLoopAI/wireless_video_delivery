# Gemini305 Depth TCP LZ4 16mm 降码率记录

日期：2026-06-22

## 1. 背景

当前 Orange Pi Gemini305 发送端恢复主媒体 TCP 后，Depth `1280x800@30 Y16` 在 `lz4 + 2mm` 下仍会产生约 `150-230Mbps` 级别的发送压力。现场 Wi-Fi 虽然是 5GHz，但 TCP Send-Q 会积压到约 `24MB`，触发发送端 bulk depth 回压保护：

```text
bulk media TCP packet dropped under backpressure
```

这会导致 Depth 实际发送帧率下降，RGB 虽然仍能接近 30fps，但 TCP 队列长期处在危险区。

## 2. 本轮测试配置

测试设备：

```text
sender:   orangepi5pro-d12a4719 / 192.168.66.133
receiver: fz-Standard-PC-i440FX-PIIX-1996 / 192.168.66.196
camera:   Orbbec Gemini305
network:  SSID 666666, 5GHz, signal about -40 dBm, tx bitrate about 390Mbps
RGB:      1280x800@30 MJPG -> H.264 6Mbps
Depth:    1280x800@30 Y16
media:    TCP 50010
```

## 3. 对比结果

### 3.1 `lz4 + 4mm`

结果：

```text
depth_mbps:       about 150-203Mbps
depth_sent_fps:   about 22-28fps
TCP Send-Q:       about 24MB
depth failures:   still present
```

结论：

```text
4mm 量化不够，仍会触发 TCP 回压丢 Depth。
```

### 3.2 `rvl + 4mm`

结果：

```text
depth_mbps:       about 67-75Mbps
depth_sent_fps:   about 9-10fps
TCP Send-Q:       below 1MB
depth failures:   0
compress time:    about 31-35ms/frame
```

结论：

```text
RVL 码率明显更低，但当前实现压缩耗时接近或超过 30fps 单帧预算。
发送端深度压缩队列保留最新帧、丢弃旧帧，所以有效 Depth 发送只有约 10fps。
现阶段不适合作为 1280x800@30 正式运行配置。
```

### 3.3 `lz4 + 8mm`

结果：

```text
depth_mbps:       about 175-177Mbps
depth_sent_fps:   about 30fps
TCP Send-Q:       about 0.27MB
depth failures:   0
```

结论：

```text
8mm 可以恢复 30fps 和低 TCP 队列，但码率下降还不够明显。
```

### 3.4 `lz4 + 16mm`

当前部署结果：

```text
rgb_input_fps:        about 29.9-30.9fps
depth_input_fps:      about 29.8-30.9fps
rgb_sent_packets_s:   about 29.9-30.9fps
depth_sent_fps:       about 29.8-30.9fps
rgb_mbps:             about 6Mbps
depth_mbps:           about 151-176Mbps
depth_compress_ms:    about 25-30ms/frame
depth_send_failures:  0
TCP Send-Q:           below 1MB
sender CPU:           gemini_sender about 230%
receiver CPU:         gemini_receiver about 28%
```

结论：

```text
16mm 是当前现场较稳的折中点。
相比 2mm/4mm，TCP 队列不再长期顶满，Depth 能维持约 30fps。
代价是 Depth 数值被按 16mm step 量化，理论最大量化误差约 8mm。
```

## 4. 当前正式运行配置

当前 `06_configs/sender_orangepi5pro-d12a4719_gemini305.json` 使用：

```json
{
  "media_udp": {
    "enabled": false,
    "rgb_enabled": false,
    "depth_enabled": false
  },
  "depth_transport": {
    "compression": "lz4",
    "quantization_step_mm": 16
  }
}
```

说明：

```text
主 RGB/Depth 媒体链路继续走 TCP。
低码率 Web RGB preview 仍可走 UDP 50012。
Depth UDP 和 plz4 实验开关保留在代码中，但当前不作为正式运行配置。
```

## 5. 后续建议

如果 16mm 量化对下游 RGBD 对齐或可视化仍可接受，当前建议先保持该配置跑完整录制测试。

如果下游认为 16mm 误差过大，可退到 `8mm`，但需要继续观察 Wi-Fi 抖动时 TCP 队列是否再次积压。

如果必须同时满足更低误差和更低码率，需要继续做 Depth 专用编码优化：

```text
1. 修复/重测 qdelta，目标是比 LZ4 更低码率、比 RVL 更低 CPU。
2. 将 RVL 或 qdelta 改成持久线程池或分块并行，避免单帧压缩阻塞。
3. 增加 Depth 编码前的 ROI/有效深度范围裁剪。
4. 在 UDP 方向补齐分片丢失统计、pacing、FEC 或小窗口重传。
```
