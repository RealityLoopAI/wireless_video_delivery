# OrangePi 5 Pro fe0e946c 部署验收

## 设备身份

- 部署日期：2026-07-15
- 设备地址：`192.168.66.64`
- SSH 用户：`orangepi`
- 网卡 MAC：`8c:cd:fe:0e:94:6c`
- 主机名：`orangepi5pro-fe0e946c`
- sender_id：`orangepi5pro-fe0e946c`
- machine-id：`fbf9b28eb3554282914bffc2b966c5c5`

设备来自 d12 镜像，初始 hostname 和 machine-id 与
`orangepi5pro-d12a4719` 重复。部署时已重新生成 machine-id，并使用 MAC
派生的固定主机名和 sender_id，避免接收端、日志和局域网名称发生冲突。

## 部署内容

- 项目目录：`/home/orangepi/Desktop/wireless_video_delivery`
- Orbbec SDK：`OrbbecSDK_v2.8.6`（ARM64）
- 配置文件：`06_configs/sender_orangepi5pro-fe0e946c_gemini305.json`
- sender 服务：`gemini-sender.service`
- USB 初始化服务：`rk3588-usb-prepare.service`
- 两项服务均已设置开机自启动并验证重启后自动运行。
- 已安装 Orbbec udev 规则和 RK3588 USB 初始化脚本。
- chrony 已固定使用接收端 `192.168.66.196` 作为时间源。
- 已安装 CMake 3.22.1，并完成 `gemini_sender` 本机增量构建验证。
- 已关闭 Wi-Fi 省电。
- 已启用相机热插拔扫描；固定 `cam01` 同时保留断线自动重连。
- 已禁用克隆镜像中无实际用途且端口冲突的 `dnsmasq.service`。

新机发送端源码与 d12 及本地主分支 `c676f08` 的关键源文件校验一致，
运行二进制与 d12 的校验一致。

## 相机与发送配置

该设备按 d12 的 Gemini305 单路 RGBD 参数部署：

- 型号绑定：`Orbbec Gemini 305`，不严格绑定序列号。
- RGB：目标 `1280x800@30 MJPG`。
- Depth：目标 `320x200@30 Y16`。
- RGB 编码：RK MPP H.264，目标码率 6 Mbps。
- Depth：`pq8zlib`，量化步长 10 mm。
- 曝光：手动，exposure 312，gain 16。
- 白平衡：自动。
- CLOCK_SYNC、TCP 主媒体和 Web RGB preview 均已启用。

部署验收时相机序列号为 `CV27561000C7`。接收端已识别
`orangepi5pro-fe0e946c_cam01`，状态、媒体和 CLOCK_SYNC 均在线。

## 验收结果与待处理项

- 接收端 clock offset 约 `-1.7 ms`，clock delay 约 `1.7 ms`，模型有效。
- Wi-Fi 连接在 5 GHz，验收时信号约 `-39 dBm`，协商速率约 433 Mbps。
- 主机状态为 `running`，无 failed systemd unit。
- 配置校验、动态库解析、重启恢复和本机编译均通过。

当前相机物理链路只协商到 `480M / USB 2.1`；d12 对照机为
`5000M / USB 3.2`。因此 SDK 无法匹配目标档位，自动回退到
`848x530@10 YUYV + 848x530@10 Y16`。重启后仍为 USB 2.1，说明不是 sender
配置或服务初始化问题。保持目标配置不降档；需要重新插拔 USB3 接头、改用
USB3 数据线或更换到 SuperSpeed 端口，直到 `lsusb -t` 显示相机位于 Bus 06、
速率为 `5000M`，随后 sender 会通过重连机制恢复目标 30 fps 档位。

## 热插拔复测

后续复测已将 `hotplug.enabled` 改为 `true`。相机曾在 `Bus 06` 以
`5000M / SuperSpeed` 枚举，证明端口选择正确，但随后内核连续报告
`device not accepting address, error -71`、xHCI setup timeout，并将设备从总线
移除。停止 sender 后软重置 xHCI 控制器仍得到同样错误，因此该次掉线发生在
USB 枚举层，不是 SDK 热插拔扫描导致。sender 服务保持运行并持续重连；需要
重新插紧两端接头、替换 USB3 数据线或排查相机供电，直至 `lsusb -t` 能持续
显示 `Bus 06 ... 5000M`。
