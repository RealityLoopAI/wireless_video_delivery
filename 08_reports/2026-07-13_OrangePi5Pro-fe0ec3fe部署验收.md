# Orange Pi 5 Pro fe0ec3fe 部署验收

日期：2026-07-13

## 设备与版本

- 主机名：`orangepi5pro-fe0ec3fe`
- 现场 IP：`192.168.66.112`
- 板卡：Orange Pi 5 Pro，RK3588S，8 GB RAM
- 系统：Ubuntu 22.04.5，内核 `6.1.43-rockchip-rk3588`
- sender ID：`orangepi5pro-fe0ec3fe`
- 源码提交：`32685387207a`
- 源码哈希：`c47ab0733a437d27`
- 配置：`06_configs/sender_orangepi5pro-fe0ec3fe.json`

该系统镜像的 `/etc/machine-id` 与旧 Orange Pi 重复，因此没有使用 machine-id 作为对外身份。本次使用 WLAN MAC 后缀生成并固定唯一 sender ID，同时将主机名改为同名值。

## 相机与档位

- SDK 识别名称：`SV1301S_U3`
- 序列号：`AY2T7410103`
- 连接：USB 3.0
- RGB：`1920x1080@30 MJPG`
- Depth：`320x200@30 Y12`
- RGB 编码：Rockchip MPP H.264，配置码率 `12 Mbps`
- Depth 压缩：zlib 无损
- 曝光：手动 `312`
- RGB gain：`60`
- 白平衡：自动

原始 SDK 测试中，自动曝光会让 RGB 降到约 20 fps。切换为手动曝光 `312` 后，RGB 和 Depth 均恢复到约 30 fps。

## 部署项

1. 补齐 `cmake` 和 `liblz4-dev`，保留板厂锁定的 GStreamer/MPP 软件栈。
2. 部署 Orbbec SDK v1.10.27，并在目标板本机编译 sender。
3. 安装 Orbbec 官方 libusb udev 规则和 USB 禁止自动休眠规则。
4. chrony 固定使用接收端 `192.168.66.196` 作为时间源。
5. 安装并启用 `/etc/systemd/system/gwv3-gemini-sender.service`。
6. systemd 以 `orangepi` 用户运行 sender/watchdog，启动前 USB 准备命令以 root 权限运行。

## MPP 启动崩溃根因

首次将 systemd sender 以 root 用户运行时，`mpph264enc` 在第一帧崩溃。独立 `mpi_enc_test` 也可复现，因此与工程采集、编码封装或网络无关。

`strace` 对比结果：

- root 运行时，旧版 MPP 选择 `/dev/dma_heap/system-uncached-dma32`；该节点在当前板厂内核中不存在，库随后空指针崩溃。
- `orangepi` 运行时，MPP 正确选择 `/dev/dri/card0` DRM allocator，`mpi_enc_test` 和 GStreamer MPP 编码都正常。

该现象与 Rockchip MPP issue #356 记录的 DMA heap allocator 失败一致：

https://github.com/rockchip-linux/mpp/issues/356

最终修复是以非 root 用户运行媒体进程，不创建伪造 DMA heap 节点，也不修改现有媒体协议和编码管线。

## 验收结果

- systemd：`active (running)`，修复后 `NRestarts=0`
- RGB 采集/发送：约 `29.9-30 fps`
- Depth 采集/发送：约 `29.9-30 fps`
- RGB 主码率：典型约 `12 Mbps`
- Depth 码率：画面相关，典型约 `6.5 Mbps`
- 坏 JPEG：0
- RGB/Depth send failure：0
- 设备 CPU idle：约 `88%`
- 内存可用：约 `7.0 GiB`
- Wi-Fi：5 GHz，信号约 `-37 dBm`，发送链路速率约 `433 Mbps`
- TCP RGB/Depth 发送队列：0
- chrony：已同步到接收端，系统时钟修正量约 `0.05 ms`
- CLOCK_SYNC：healthy，offset 典型约 `-0.1 ms`，delay 约 `1.2-1.5 ms`
- 接收端：`orangepi5pro-fe0ec3fe_cam01` 的 status/media 均在线
- Web RGB 预览：按需流返回 `X-GWV3-Rgb-Stream: preview`，解析分辨率 `640x360`

本次没有自动开始正式录制，避免干扰现场正在进行的录制任务。
