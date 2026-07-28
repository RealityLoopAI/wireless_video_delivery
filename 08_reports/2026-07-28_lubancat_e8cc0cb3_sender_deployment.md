# LubanCat-3IO RK3576 Sender 部署记录

日期：2026-07-28

## 1. 设备身份

```text
deployment IP: 192.168.66.74
hostname: lubancat-e8cc0cb3
sender_id: lubancat-e8cc0cb3
camera_id: cam01
board: EmbedFire LubanCat-3IO
SoC: RK3576
board serial: 3b07dab9e8cc0cb3
architecture: aarch64
memory: 8 GB
OS: Ubuntu 22.04.5
kernel: 6.1.99-rk3576
```

相机：

```text
model: Orbbec Gemini 305
serial: CV2R46P00091
firmware: 1.0.70
USB ID: 2bc5:0840
SDK connection: USB3.2
```

本次板卡沿用了上一台设备的 USB Wi-Fi 网卡和相机，因此部署时的 MAC、DHCP IP
和相机序列号均与上一台相同。为避免接收端键和历史数据冲突，`sender_id` 使用
RK3576 板卡序列号后 8 位，不使用可移动 Wi-Fi 网卡的 MAC。

## 2. 部署内容

配置文件：

```text
06_configs/sender_lubancat-e8cc0cb3_gemini305.json
```

systemd 服务：

```text
05_tools/systemd/gwv3-gemini-sender-lubancat-e8cc0cb3.service
```

采集与传输：

```text
RGB: 1280x800@30 MJPG -> mpph264enc, target 6 Mbps
Depth: 320x200@30 Y16 -> pq8zlib, quantization 10 mm
media: TCP 192.168.66.196:50010
status: UDP 192.168.66.196:50011
clock sync: UDP 192.168.66.196:50012
RGB exposure: manual 312
RGB gain: 16
white balance: auto
camera binding: model only
hotplug scan: enabled
```

服务已设置为开机自启动，Wi-Fi 省电已关闭，chrony 已配置为以接收端为时间源。

## 3. 依赖适配

Gemini 305 使用 Orbbec 官方 SDK 2.8.6 ARM64 包：

```text
OrbbecSDK_v2.8.6_202604271452_6399409_linux_arm64.tar.gz
SHA256: a052221d4bdea6afb2f8b338bcd6e635afffcebbacab1483422b986e680fb441
```

RK3576 使用经过生产设备验证的 GStreamer 1.20 Rockchip MPP 插件：

```text
/home/cat/wireless_video_runtime/gstreamer-1.0/libgstrockchipmpp.so
SHA256: 2d6bfb9e17f12dc6439109b794a43a808930b9a5dec1b546a30ed7c03a706d56
```

`mppjpegdec`、`mpph264enc` 和 1280x800 H.264 实际硬件 pipeline 均已通过。

## 4. 验证结果

构建与单元检查：

```text
sender ARM64 Release build: passed
sender_config_validation: 1/1 passed
```

SDK 原始采集进入稳定阶段后：

```text
RGB: about 30.02 FPS
Depth: about 30.02 FPS
startup timeouts: 3
steady-state timeouts: 0
```

发送端完整链路在相机物理断开前：

```text
RGB input/send: about 29.9-30.0 FPS
Depth input/send: about 29.9-30.0 FPS
RGB hardware encode: about 0.35-0.57 ms/frame
RGB send failures during steady state: 0
Depth send failures during steady state: 0
CLOCK_SYNC valid: true
clock offset: about -0.24 ms
clock network delay: about 3.46 ms
receiver calibration available: true
```

## 5. 现场 USB 事件

2026-07-28 14:53:15 内核记录：

```text
usb 2-1: USB disconnect, device number 2
```

事件发生后相机从 `lsusb` 完全消失，SDK 报告 device deactivated/disconnected。
因此随后接收端帧率下降不是编码、网络或接收端性能问题。发送端服务仍保持启用，
watchdog 和热插拔扫描会在相机重新枚举后恢复采集。

14:54:29 相机重新以 SuperSpeed 枚举为 USB device 3，发送端未重新部署，
自动恢复 RGB/Depth 约 30 FPS，证明自启动 watchdog 和热插拔恢复链路生效。

## 6. 当前局域网限制

部署验证期间，接收端在不录制时 CPU 和内存仍有充足余量，但其他发送端已持续向
接收端输入约 70.6 Mbps。新发送端相机采集保持 30 FPS 时，媒体 TCP 偶发大队列和
发送失败；同时测得：

```text
new sender -> receiver ping: average about 321 ms while sender stopped
deployment host -> receiver ping: average about 127 ms
```

因此当前波动属于共享 AP / 无线链路拥塞，不是 RK3576 采集或硬件编码性能不足。
部署没有降低采集档位或帧率。
