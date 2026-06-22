# Gemini305 Depth UDP 分片与 PLZ4 实验记录

日期：2026-06-22

## 1. 实验目标

本轮验证两个方向：

1. Depth 是否可以脱离 TCP，独立走 UDP 分片通道。
2. RGB 主码流和 Depth 主码流是否可以都走 UDP。
3. Depth 是否可以通过分块并行 LZ4，也就是 `plz4`，降低单帧压缩等待时间。

本轮测试设备：

```text
sender:   orangepi5pro-d12a4719 / 192.168.66.133
receiver: fz-Standard-PC-i440FX-PIIX-1996 / 192.168.66.196
camera:   Orbbec Gemini305
RGB:      1280x800@30 MJPG -> H.264 6Mbps
Depth:    1280x800@30 Y16
Wi-Fi:    SSID 666666, 5GHz, PHY about 433Mbps
```

## 2. 本轮代码能力

发送端新增：

- `media_udp` 配置块。
- 可按流选择 UDP：
  - `media_udp.rgb_enabled`
  - `media_udp.depth_enabled`
- `send_media_udp()`，复用现有 `GUP1` 分片头发送完整媒体包。
- `plz4` Depth 压缩格式：一帧 Depth 切为多个块，各块独立 LZ4 压缩，接收端重组后恢复为原始 `uint16` Depth。

接收端新增：

- `media_udp_enabled/media_udp_port` 配置。
- 独立 `media_udp_loop()` 监听 `50013/udp`。
- media UDP 只接受主媒体流 `rgb` 和 `depth_raw`。
- preview UDP 仍只接受 `rgb_preview`，避免主媒体和网页预览混流。
- `plz4` 解码后继续进入原 `handle_media_packet()`，落盘和 CSV 路径不分叉。

## 3. 实测结果

### 3.1 RGB + Depth 都走 UDP，Depth 使用 `plz4 + 2mm`

配置：

```json
{
  "media_udp": {
    "enabled": true,
    "rgb_enabled": true,
    "depth_enabled": true,
    "port": 50013,
    "mtu_bytes": 1200
  },
  "depth_transport": {
    "compression": "plz4",
    "quantization_step_mm": 2
  }
}
```

早期 10 秒窗口：

```text
active_tcp: 0
rgb_fps:   29.98
depth_fps: 28.69
rgb_mbps:  5.99
depth_mbps: 224.41
```

后续 10 秒窗口：

```text
active_tcp: 0
rgb_fps:   29.78
depth_fps: 23.78
rgb_mbps:  5.97
depth_mbps: 180.20
```

发送端现象：

```text
TCP Send-Q: none, because main media did not use TCP
rgb_send_failures: 0
depth_send_failures: 0
depth_compress_avg_ms: about 29-33ms
depth_send_avg_ms: about 6-13ms
sender CPU: about 300%
```

结论：

```text
RGB 和 Depth 主码流都可以走 UDP，链路能跑通。
但是 Depth 有明显的接收端有效帧损失。
发送端没有 send failure，不代表接收端完整收到，因为 UDP 不确认、不重传。
```

### 3.2 RGB 走 TCP，Depth 单独走 UDP，Depth 使用 `lz4 + 2mm`

配置：

```json
{
  "media_udp": {
    "enabled": true,
    "rgb_enabled": false,
    "depth_enabled": true,
    "port": 50013,
    "mtu_bytes": 1400
  },
  "depth_transport": {
    "compression": "lz4",
    "quantization_step_mm": 2
  }
}
```

10 秒窗口：

```text
active_tcp: 1
rgb_fps:   30.08
depth_fps: 23.99
rgb_mbps:  6.05
depth_mbps: 203.25
```

结论：

```text
Depth 独立 UDP 可以避免 TCP 队头阻塞，但无法保证每帧完整到达。
在当前 200Mbps 级 Depth 码率下，单靠普通 UDP 分片不够稳。
```

### 3.3 回到 TCP，Depth 使用 `lz4 + 2mm`

10 秒窗口：

```text
active_tcp: 1
rgb_fps:   30.48
depth_fps: 21.59
rgb_mbps:  6.08
depth_mbps: 172.44
TCP Send-Q: about 24.6MB
```

发送端出现：

```text
bulk media TCP packet dropped under backpressure
```

结论：

```text
TCP 可以保证有序可靠，但在当前 Wi-Fi 抖动和 200Mbps 级 Depth 下会堆队列。
队列堆满后发送端主动丢 bulk depth 包，Depth 有效帧率也会下降。
```

## 4. 结论

本轮验证结论：

```text
RGB/Depth 都走 UDP 技术上可行。
Depth 独立 UDP 技术上可行。
但是当前“完整媒体包 UDP 分片 + 无重传/无 FEC”的方案还不能保证 1280x800@30 Depth 完整落盘。
```

核心原因：

- Depth 单帧压缩后仍约 0.8-1.0MB。
- `1200` 字节 MTU 下，一帧 Depth 需要拆成数百个 UDP 包。
- 任意一个分片丢失，接收端就不能重组该帧，只能丢整帧。
- Wi-Fi 下高 PPS 和高吞吐同时存在，丢少量 UDP 分片很正常。
- `plz4` 当前用每帧临时并发任务实现，压缩延迟仍约 29-33ms，没有明显优于单块 LZ4。

## 5. 后续建议

如果继续走 UDP 方向，不能只做“裸 UDP 分片”，建议下一步按优先级做：

1. 接收端增加 UDP 分片丢失统计：按 frame_id/sequence 统计缺片率、丢帧率、乱序和超时。
2. 发送端给 UDP 分片做 pacing，避免一帧几百个包瞬间打满 Wi-Fi 驱动队列。
3. Depth UDP 增加小窗口重传或 FEC，否则无法保证完整 30fps 落盘。
4. RGB 主码流如果长期走 UDP，应改成 H.264/RTP NAL-aware 分片，不能一直用完整媒体包粗暴分片。
5. `plz4` 如果继续保留，应改成持久线程池，避免每帧创建异步任务。
6. 真正要稳定 1280x800@30，仍建议先降低 Depth 码率，例如更有效的 Depth 专用编码、8-16mm 量化、ROI 或有线链路。
