# 52d2 正式媒体缓冲扩容

## 实际修改与部署

2026-09-14 15:13，只对 `lubancat-52d2ef0c` 部署。
`recording_buffer.rgb_frames_per_slot` 与 `depth_frames_per_slot` 从 900 改为 1800。
按 30 fps，帧数上限由名义 30 秒提高到 60 秒。

现场配置先拉回，仅修改这两个字段，使用现场二进制 `--validate-config` 验证通过。
保留所有现场曝光、旋转、网络等设置，没有用仓库整份配置覆盖现场。
确认 Receiver 为 faulted、各路 recording=false、无收尾后，重启该 sender。
15:13:21 日志确认两个上限均为 1800。程序二进制未替换，其他 sender 未扩容。

## 不改变的边界

- 正式发送队列缓存编码/压缩后的媒体包，不扩大 RGB 原始采集或 SDK 帧池。
- 每个正式流 slot 仍有 256 MiB 上限，帧数和字节数先触及哪项，就以哪项为准。
- RGB 与 Depth 两个发送 slot 的队列载荷上限合计 512 MiB，不是整个进程的内存上限；还有在途包、编码器、压缩队列等。
- Depth 压缩前队列仍为 60 帧，预览仍走最新帧策略，不给预览增加一分钟积压。
- 不改变 TCP socket 缓冲、相机档位、编码参数、帧时间戳或 CSV 字段。
- Receiver 现场没有覆盖 stop_drain_timeout，当前二进制对应源码默认 120 秒；media_recovery_grace_ms 也是 120 秒。本轮不修改接收端。
- Receiver 停止录制补收与 sender 进程退出不是一回事。sender 退出的 graceful drain 仍为 10 秒，重启/断电不保证保住一分钟队列。

这是提高短时停顿容忍度，不是每帧必达保证。恢复后的吞吐需高于实时产生速度才能追赶；如果只是相等，积压不会消失。字节上限、长期带宽不足、源 JPEG 损坏仍会造成缺帧。

## 验证

新增 `10_tests/test_sender_media_buffer.py`：编译实际 LatestMediaQueue 类，周边媒体类型用轻量 stub，避免依赖 SDK。没有复制队列逻辑。

- 停止消费并发布 1350 帧，相当于 30fps 下 45 秒逻辑积压，RGB/Depth 均保持 FIFO，无覆盖。
- 达到 1800 帧后仍按现有策略覆盖旧帧，边界测试通过。
- 256 MiB 字节上限、预览只保留最新帧、stop 后拒绝发布测试通过。
- 此测试不等待真实 45 秒、不分配等量图像内存，也不模拟真实无线传输，不能当成 45 秒断网实录测试。
- 已加入 CTest 的 sender_media_buffer 项，1/1 通过，耗时 1.04 秒。首次因原构建 BUILD_TESTING=OFF 未发现测试，不计为通过；启用测试并重新配置后执行了该项。

上线采样：RGB/Depth 输入约 30fps；Receiver 上 RGB 延迟 52.585ms、Depth 24.208ms；设备内存已用约 396 MiB、可用约 7407 MiB。只是短时样本，不代表长测结果。API 的 last_error 当时保留 rgb encoder output lag reset，不能把上线状态写成从未有过告警。

Receiver 仍为此前的 faulted 状态，本轮没有清除录制故障、自动开录或做新 NAS 长录制。后续应在录制保护原因确认后，测试实际积压、恢复排空以及停止后的完整帧覆盖。

## 回退

现场原配置备份：
`/home/cat/wireless_video_delivery_release/06_configs/sender_lubancat-52d2ef0c_gemini305.json.before-buffer60-20260914`。

确认无录制及收尾后，将现场这两个值恢复 900 并重启
`gwv3-gemini-sender-lubancat-52d2ef0c.service`。优先只回退两个字段，避免覆盖之后的其他配置变更。
