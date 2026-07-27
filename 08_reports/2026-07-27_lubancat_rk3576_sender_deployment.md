# LubanCat-3IO RK3576 Sender 部署记录

日期：2026-07-27

## 1. 设备身份

```text
IP: 192.168.66.74
hostname: lubancat
sender_id: lubancat-52d2ef0c
camera_id: cam01
board: EmbedFire LubanCat-3IO
SoC: RK3576
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

## 2. 部署配置

配置文件：

```text
06_configs/sender_lubancat-52d2ef0c_gemini305.json
```

systemd 服务：

```text
05_tools/systemd/gwv3-gemini-sender-lubancat-52d2ef0c.service
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

服务已经设置为开机自启动。启动前会把 `usbfs_memory_mb` 调到 256、关闭 USB
autosuspend，并把 `net.core.wmem_max` 调到 32 MB。

## 3. 适配问题与处理

### 3.1 GStreamer ABI 不一致

目标镜像自带的 `gstreamer1.0-rockchip1` 文件按 GStreamer 1.22 ABI 构建，
系统实际运行 GStreamer 1.20.3，因此 MPP 插件被 blacklist。

没有替换系统 GStreamer。部署使用现有生产 Orange Pi 上已验证的 GStreamer
1.20 兼容插件，并通过独立目录加载：

```text
/home/cat/wireless_video_runtime/gstreamer-1.0/libgstrockchipmpp.so
SHA256: 2d6bfb9e17f12dc6439109b794a43a808930b9a5dec1b546a30ed7c03a706d56
```

`mppjpegdec`、`mpph264enc` 以及 1280x800 硬件编码 pipeline 均已实际运行通过。

### 3.2 Orbbec SDK 版本

SDK `1.10.27` 无法枚举 Gemini 305，安装 udev 规则和 root 运行均不能改变结果。
改用现有 Gemini 305 生产发送端上的 ARM64 SDK `2.8.6` 后，相机立即正确枚举。

### 3.3 APT 索引损坏

初始系统的 Ubuntu Translation 索引损坏，导致 `apt-get update` 解析失败。
删除单个损坏索引并使用 `Acquire::Languages=none` 重建缓存后，依赖安装正常完成。

## 4. 验证结果

SDK 原始采集 12 秒测试进入稳定阶段后：

```text
RGB: about 30.02 FPS
Depth: about 30.02 FPS
wait timeouts: 2 during startup, 0 after warm-up
```

接收端 20.034 秒连续计数：

```text
RGB packets: 601, 29.999 packets/s
Depth packets: 601, 29.999 packets/s
RGB media bitrate: 6.009 Mbps
Depth media bitrate: 8.053 Mbps
RGB send failures: 0
Depth send failures: 0
```

运行资源：

```text
sender CPU: about 135% of one core, about 1.35 / 8 cores
sender RSS: about 97 MB
maximum observed temperature: 67.5 C
Wi-Fi: 5 GHz, 5320 MHz, signal about -44 dBm
systemd restarts: 0
fatal sender events: 0
```

时间同步：

```text
chrony: synchronized to receiver
CLOCK_SYNC valid: true
offset: about 0.7-1.3 ms after convergence
network delay: about 1.7-1.9 ms
```

接收端已经确认：

```text
camera online: true
media live: true
status live: true
calibration available: true
RGB preview: available after on-demand decoder warm-up
Depth preview: available
```

启动阶段检测到并丢弃过 1 个损坏的 RGB MJPEG 帧；之后连续计数达到 30 FPS，
未再出现发送失败或服务重启。按当前数据判断属于启动瞬态，不是持续坏帧。
