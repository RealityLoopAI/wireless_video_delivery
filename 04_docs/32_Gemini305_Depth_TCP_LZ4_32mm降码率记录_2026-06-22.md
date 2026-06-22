# Gemini305 Depth TCP LZ4 32mm 降码率记录

日期：2026-06-22

追加结论：本文保留 `lz4` 量化试验过程。后续长时间观察确认 `lz4 + 32mm/64mm` 仍可能在 Wi-Fi/TCP 波动时触发回压，当前正式配置已改为 `q8zlib + 128mm`。最终配置和验收数据见 `04_docs/33_Gemini305_Depth_Q8ZLIB降码率记录_2026-06-22.md`。

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

短窗口结果：

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
16mm 短窗口可以恢复约 30fps。
但后续更长窗口观察到 TCP Send-Q 又涨到约 23MB，并再次出现 Depth 回压丢包。
因此 16mm 不是当前现场足够稳的最终档。
```

### 3.5 `lz4 + 32mm`

短窗口结果：

```text
rgb_input_fps:        about 29.8-31.0fps
depth_input_fps:      about 28.8-31.0fps
rgb_sent_packets_s:   about 29.8-31.0fps
depth_sent_fps:       mostly about 29.8-31.0fps
rgb_mbps:             about 6Mbps
depth_mbps:           about 125-143Mbps
depth_compress_ms:    about 25-29ms/frame
depth_send_failures:  0
TCP Send-Q:           about 0.3-0.6MB
sender CPU:           gemini_sender about 230%
receiver:             active_media_clients=1, media_live=true
```

结论：

```text
32mm 在短窗口内比 16mm 更稳。
但后续更长观察仍出现 TCP Send-Q 爬升和 Depth 回压风险，因此不再作为当前正式运行配置。
代价是 Depth 数值被按 32mm step 量化，理论最大量化误差约 16mm。
```

## 4. 历史运行配置

该阶段曾在 `06_configs/sender_orangepi5pro-d12a4719_gemini305.json` 使用：

```json
{
  "media_udp": {
    "enabled": false,
    "rgb_enabled": false,
    "depth_enabled": false
  },
  "depth_transport": {
    "compression": "lz4",
    "quantization_step_mm": 32
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

当前不再建议继续使用 `lz4 + 32mm` 作为正式配置。最终部署已改为 `q8zlib + 128mm`。

如果下游认为 32mm 误差过大，可退到 `16mm` 或 `8mm`，但需要继续观察 Wi-Fi 抖动时 TCP 队列是否再次积压。当前现场已经观察到 `16mm` 长一点运行仍会回压。

如果必须同时满足更低误差和更低码率，需要继续做 Depth 专用编码优化：

```text
1. 修复/重测 qdelta，目标是比 LZ4 更低码率、比 RVL 更低 CPU。
2. 将 RVL 或 qdelta 改成持久线程池或分块并行，避免单帧压缩阻塞。
3. 增加 Depth 编码前的 ROI/有效深度范围裁剪。
4. 在 UDP 方向补齐分片丢失统计、pacing、FEC 或小窗口重传。
```
