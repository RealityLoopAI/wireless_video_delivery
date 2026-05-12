# Gemini Wireless Video v3 Sender Handoff

This package is for receiver-side integration with the current sender implementation.

2026-05-11 update: the sender H.264 pipeline now forces `NV12` before Rockchip `mpph264enc`. This fixes the issue where receivers only saw AUD/SPS/PPS header NALs without decodable picture frames. RGB packets now set `flags.bit0` when the payload contains an IDR frame.

2026-05-11 depth update: sender and receiver now support optional `zlib` compression for Depth packets. The sender default is `06_configs/sender_orangepi5pro-01_depth_zlib.json`; keep `06_configs/sender_orangepi5pro-01.json` only as a raw Depth compatibility profile.

2026-05-11 quality update: the sender default RGB profile is `1920x1080@30` with H.264 target bitrate `12000000` bps. Depth remains `640x400@30` with `zlib`.

2026-05-12 startup update: sender startup/status scripts now print the active config, receiver endpoint, RGB/Depth profiles, encoder, Depth compression, preview mode, route, Wi-Fi link, and validation result before starting.

2026-05-12 performance update: sender RGB now uses the camera MJPG payload directly through Rockchip `mppjpegdec` before `mpph264enc`, avoiding the old software JPEG decode path for transmission. On the current Orange Pi 5 Pro + Orbbec SV1301S_U3 unit, the requested RGB profile is still `1920x1080@30`, but measured RGB output is about `19-21fps` even in RGB-only probing; Depth remains about `30fps`. Receiver recording and preview should use measured sender frame cadence rather than assuming 30 RGB frames per second.

Read first:

```text
04_docs/04_发送端交付与接收端对接说明_v3.md
04_docs/03_中间传输数据格式_v3.md
06_configs/sender_orangepi5pro-01_depth_zlib.json
06_configs/sender_orangepi5pro-01.json
03_common_core/include/gwv3_common/protocol.hpp
```

Current sender endpoint:

```text
sender_id: orangepi5pro-01
camera_id: cam01
receiver_ip: 192.168.1.107
status: UDP 50011
media: TCP 50010
```

The receiver should listen on UDP `50011` for JSON status messages and TCP `50010` for binary media packets.
