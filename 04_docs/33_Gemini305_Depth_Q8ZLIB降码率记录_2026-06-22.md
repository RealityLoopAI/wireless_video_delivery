# Gemini305 Depth Q8ZLIB 降码率记录

日期：2026-06-22

## 1. 背景

Orange Pi Gemini305 当前运行档位：

```text
sender:   orangepi5pro-d12a4719 / 192.168.66.133
receiver: fz-Standard-PC-i440FX-PIIX-1996 / 192.168.66.196
camera:   Orbbec Gemini305 / USB3.2
RGB:      1280x800@30 MJPG -> H.264 6Mbps
Depth:    1280x800@30 Y16
media:    TCP 50010
```

`lz4` 量化到 `32mm`、`64mm` 后，短时间可以接近 30fps，但 Wi-Fi/TCP 吞吐波动时仍会把发送端 TCP Send-Q 顶到约 `24MB`，触发：

```text
bulk media TCP packet dropped under backpressure
```

因此继续保留 TCP 主媒体链路，但把 Depth 改成更低码率的深度专用编码。

## 2. 本次代码改动

发送端新增两种 Depth 压缩：

```text
q8lz4
q8zlib
pq8zlib
```

编码流程：

```text
1. 采集到 SDK Depth 原始 uint16 payload。
2. 按 quantization_step_mm 换算成 raw_step。
3. 每个非零深度值四舍五入到 raw_step 对应的 8-bit index，0 仍表示无效深度。
4. q8lz4 使用 LZ4 压缩 8-bit index 图。
5. q8zlib 使用 zlib 压缩 8-bit index 图。
6. pq8zlib 把 8-bit index 图切成最多 4 块，并行 zlib 压缩。
7. q8lz4/q8zlib payload 内部写入 16 字节小头：magic/version/raw_step/sample_count/compressed_size。
8. pq8zlib payload 写入 20 字节总头和每块 12 字节 chunk 表，接收端按 chunk 表并行解压后再还原 uint16。
```

接收端新增对应解码：

```text
1. 按 magic 识别 Q8L1、Q8Z1 或 PQ8Z。
2. 解压出 8-bit index 图。
3. 按 raw_step 还原为 uint16 little-endian Depth payload。
4. 后续落盘、预览、CSV 仍走原来的 depth_u16 路径。
```

因此接收端对外的文件格式没有改成 8-bit；下游仍读取解码后的 `uint16` Depth。

## 3. 实测对比

### 3.1 `q8lz4 + 64mm`

结果：

```text
depth_sent_fps:      mostly about 29-30fps
depth_mbps:          about 95-106Mbps
single depth packet: about 420-580KB
TCP Send-Q:          can still rise to about 24MB
depth failures:      still appears during Wi-Fi/TCP backpressure
sender CPU:          about 217%
```

结论：

```text
q8lz4 比 16-bit LZ4 低，但仍不够稳。
```

### 3.2 `q8zlib + 64mm`

结果：

```text
depth_sent_fps:      about 30fps in normal windows
depth_mbps:          about 74-79Mbps
compress time:       about 32-34ms/frame
TCP Send-Q:          can still reach about 24MB during longer observation
depth failures:      appears in short bursts under backpressure
sender CPU:          about 240%
```

结论：

```text
q8zlib + 64mm 已明显降码率，但现场链路波动时仍有短时积压。
```

### 3.3 `q8zlib + 128mm`

结果：

```text
rgb_input_fps:        about 29.8-30.9fps
depth_input_fps:      about 28.9-30.9fps
rgb_sent_packets_s:   about 29.8-30.9fps
depth_sent_fps:       about 29.8-30.9fps
rgb_mbps:             about 6Mbps
rgb_preview_mbps:     about 0.1-0.4Mbps
depth_mbps:           about 61-63Mbps
depth_compress_ms:    about 30-32ms/frame
depth_send_failures:  0 in the 30s verification window
TCP Send-Q:           about 144KB at the end of the window
sender CPU:           gemini_sender about 247%, system still about 60% idle
receiver CPU:         gemini_receiver about 60%
```

结论：

```text
q8zlib + 128mm 是上一版现场稳定低码率配置。
相比 q8zlib + 64mm，Depth 码率从约 76Mbps 降到约 61Mbps，TCP 队列明显下降。
```

### 3.4 `pq8zlib + 128mm`

结果：

```text
rgb_input_fps:        about 29.8-30.8fps
depth_input_fps:      about 29.0-30.9fps
rgb_sent_packets_s:   about 29.8-30.8fps
depth_sent_fps:       about 29-30fps
rgb_mbps:             about 6Mbps
rgb_preview_mbps:     about 0.1-0.4Mbps
depth_mbps:           about 36-53Mbps, latest recheck about 44-48Mbps
depth_compress_ms:    about 20-24ms/frame
depth_send_failures:  0 in 30s and 60s verification windows
TCP Send-Q:           0 on both sender and receiver during the final window
sender CPU:           gemini_sender about 325-381%, system still about 50-55% idle
receiver CPU:         gemini_receiver about 50-70%
```

结论：

```text
pq8zlib + 128mm 是当前 Orange Pi Gemini305 的正式低码率配置。
它的深度数值量化精度与 q8zlib + 128mm 相同，只把 zlib 压缩改为分块并行。
相比 q8zlib + 128mm，Depth 压缩耗时从约 30-32ms 降到约 20-24ms，链路观察窗口内 TCP 队列为 0。
```

## 4. 当前正式配置

当前 `06_configs/sender_orangepi5pro-d12a4719_gemini305.json`：

```json
{
  "media_udp": {
    "enabled": false,
    "rgb_enabled": false,
    "depth_enabled": false
  },
  "depth_transport": {
    "compression": "pq8zlib",
    "quantization_step_mm": 128
  }
}
```

说明：

```text
主 RGB/Depth 媒体链路仍走 TCP。
Depth 采集仍是 1280x800@30 Y16。
接收端落盘前会解码回 uint16 Depth。
```

## 5. 精度影响

`quantization_step_mm=128` 表示非零深度按约 `128mm` 档位还原，理论最大四舍五入误差约 `64mm`。这不会改变 RGBD 的像素对齐关系和时间戳关系，但会降低深度数值精度。

如果下游更看重深度数值精度，可退回：

```text
q8zlib + 64mm
```

但当前现场已经观察到 `64mm` 在链路波动时仍可能触发 TCP 回压丢 Depth。若必须同时满足更小量化误差和稳定 30fps，需要继续优化网络侧或实现更强的 Depth 专用编码。

## 6. 现场部署备注

Orange Pi 发送端需要使用 Orbbec SDK v2 构建。当前设备上 SDK v2 实体来自：

```text
/home/orangepi/Desktop/SDK与依赖/pyorbbecsdk/
```

发送端工程内 SDK v2 目录使用符号链接指向该 SDK 的 `include` 和 `install/lib`，systemd 环境变量保持：

```text
ORBBEC_SDK_ROOT=/home/orangepi/Desktop/wireless_video_delivery/11_third_party/orbbec/linux_arm64/OrbbecSDK_v2.8.6
```

部署后确认二进制链接 `libOrbbecSDK.so.2`，避免误用旧 SDK v1 或带本机路径 RUNPATH 的二进制。
