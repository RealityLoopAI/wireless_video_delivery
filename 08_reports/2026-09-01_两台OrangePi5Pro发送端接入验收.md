# 两台 Orange Pi 5 Pro 发送端接入验收

日期：2026-09-01

## 1. 接入设备

本次新增两台 RK3588S Orange Pi 5 Pro，均连接一台 `SV1301S_U3` RGB-D 相机。

| sender ID | 部署时管理地址 | camera ID | 相机 RGB USB 序列号 | 内存 |
| --- | --- | --- | --- | --- |
| `orangepi5pro-fe0f7222` | `192.168.1.116` | `cam01` | `AY2MC3100Z8` | 8 GB |
| `orangepi5pro-b439137c` | `192.168.0.100` | `cam02` | `AY2MC31010W` | 16 GB |

地址由 DHCP 分配，运行身份以 sender ID 为准，不依赖固定 IP。

## 2. 发现的问题

### 2.1 克隆镜像身份冲突

`192.168.1.116` 的镜像仍使用旧主机名和 sender ID `orangepi5pro-fe0ec3fe`，两台新设备的 `/etc/machine-id` 也与旧镜像相同。继续运行会导致状态、预览和录制目录被错误归到已有发送端。

处理结果：

- 按实际 WLAN MAC 后缀为 `.116` 分配 `orangepi5pro-fe0f7222`。
- 保留 `.125` 实际 WLAN MAC 对应的 `orangepi5pro-b439137c`。
- 修正两台主机名并重新生成互不相同的 32 位 machine ID。

### 2.2 接收端地址和系统时间过期

设备旧配置仍指向 `192.168.66.196`，系统时间停留在 2026-08-27，比当前接收端慢约五天。

处理结果：

- 媒体、状态和 CLOCK_SYNC 均改为当前接收端 `192.168.1.196`。
- chrony 固定以接收端为时间源并执行首次 step 校时。
- 重启后两台均自动恢复到 stratum 3，CLOCK_SYNC 模型有效。

### 2.3 旧程序和服务状态不一致

`.116` 运行的是旧源码，`.125` 虽然残留 `b439137c` 服务但没有启动。`.116` 还存在 root 所有的 Orbbec SDK 日志目录，普通 sender 进程无法写日志。

处理结果：

- 两台均部署提交 `7af411a` 对应源码并在设备本机编译。
- 配置校验通过后安装并启用 `gwv3-gemini-sender.service`。
- 修正 `.116` 的 SDK 日志目录所有权。
- 实机重启验证服务、chrony、USB 和 Wi-Fi 均能自动恢复。

## 3. 运行配置

两台都使用：

```text
RGB:   1920x1080 @ 30 FPS, MJPG capture, MPP H.264, 12 Mbps
Depth: 320x200 @ 30 FPS, Y12, zlib
Media: TCP 50010
Status: UDP 50011
Clock sync: UDP 50012
Web preview: on demand, 640x360, 30 FPS, 500 kbps
Exposure: manual 312, gain 0
White balance: automatic
```

相机以 `device_model=SV1301S_U3` 绑定，不严格绑定 USB 序列号。

## 4. 无线分流

部署后两个 5 GHz AP 各承载四台发送端：

```text
666666 / 5 GHz:
  orangepi5pro-f022c4
  lubancat-4df661d7
  lubancat-52d2ef0c
  orangepi5pro-fe0f7222

TP-LINK_5G_215E / 5 GHz:
  rk3588-ubuntu
  orangepi5pro-ab748372
  lubancat-e8cc0cb3
  orangepi5pro-b439137c
```

两台新增设备均固定到对应 5 GHz BSSID，并关闭 Wi-Fi 省电。部署时链路实测：

- `fe0f7222`：信号约 -36 dBm，20 次 ping 0% 丢包，平均 13.3 ms。
- `b439137c`：信号约 -51 dBm，20 次 ping 0% 丢包，平均 7.7 ms。

## 5. 验收结果

重启后连续 60 秒接收端计数：

| sender | RGB 增量 | Depth 增量 | RGB/Depth 丢帧 | 发送失败 | CLOCK_SYNC offset | delay |
| --- | ---: | ---: | --- | --- | ---: | ---: |
| `b439137c` | 1804 | 1809 | 0 / 0 | 0 / 0 | 7 us | 1630 us |
| `fe0f7222` | 1806 | 1808 | 0 / 0 | 0 / 0 | 187 us | 2923 us |

两路 RGB/Depth 均约 30 FPS，接收端状态、媒体和 RGB/Depth 预览均为 live，录制队列为 0。

## 6. 已知边界

SDK 对 `1920x1080 RGB + 320x200 Depth` 组合持续报告没有匹配的 calibration profile，接收端因此显示 `calibration_available=false`。这不影响原始 RGB/Depth 采集、发送和落盘，但该档位不能直接宣称已具备 SDK 几何对齐参数；下游若需要像素级 RGBD 对齐，需要使用相机支持标定参数的 profile 组合或单独标定。
