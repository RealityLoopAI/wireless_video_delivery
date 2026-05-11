# Gemini Wireless Video v3 Sender Handoff

This package is for receiver-side integration with the current sender implementation.

2026-05-11 update: the sender H.264 pipeline now forces `NV12` before Rockchip `mpph264enc`. This fixes the issue where receivers only saw AUD/SPS/PPS header NALs without decodable picture frames. RGB packets now set `flags.bit0` when the payload contains an IDR frame.

Read first:

```text
04_docs/04_发送端交付与接收端对接说明_v3.md
04_docs/03_中间传输数据格式_v3.md
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
