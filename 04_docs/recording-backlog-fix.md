# 录制积压与时钟降级修复

日期：2026-09-09。前序证据见 [上次部署验收](clock-history-deployment.md)。

## 已定位的缺陷

1. `52d2` 原 `send_buffer_bytes=33554432`，实测每条媒体 TCP Send-Q 接近 63 MiB。
   大内核缓存让发送调用暂时成功，但不代表数据已到接收端。网络持续吞吐不足时，延迟可积累数分钟。
   这会推迟尾帧到达，并最终触发应用队列覆盖和 H.264 关键帧恢复。
2. 时钟历史查询原来还受当前报告是否过期的影响。同一采集时刻的 Depth 先到、RGB 晚到时，
   可能一个加 offset、另一个回退原始时间；正 offset 失效时还可能造成 global 倒退。
3. `StreamRecordStats` 用收包墙钟跨度计算媒体时长、实际 FPS 和重定时目标。
   相同采集时段的 RGB 迟到约 36 秒，会错误增加 RGB 时长，产生 duration drift 的 partial 标记。
   这不是说所有 partial 都是假警报，缺帧、首尾缺失仍要独立检查。

当次 52d2 温度约 51 C、USB 5000M、receiver 录制队列和 NAS 待上传队列均为空。
这些证据不支持把当时延迟直接归为过热、USB2 或 NAS 写入瓶颈，也不能追溯证明旧录像每个缺口的原因。

## 代码修复

### 有界 TCP 未发送缓存

新增 `transport.tcp_notsent_lowat_bytes`，默认 65536，范围 0 至 4194304，0 可禁用。
Linux 支持时设置 `TCP_NOTSENT_LOWAT`，让未发数据尽早反压到原有有界应用队列。
内核不支持时记录 transport error 并继续原有链路，不影响启动。它限制的是未发送字节阈值，
不是 TCP 全部在途数据的硬上限，不能用它保证 Send-Q 永远低于 64 KiB。

52d2 仓库配置同时把 `send_buffer_bytes` 调到 65536。其他相机档位、曝光、编码和 TCP 协议不变。
完整包的分段重试仍使用原连接和原字节序列，不通过丢弃半个包、反复断连来降低队列。
该修复防止缓存掩盖问题，不能凭空增加无线吞吐；持续可用吞吐低于采集码率时仍然有丢帧风险。
Linux 行为参考 [官方网络参数文档](https://docs.kernel.org/networking/ip-sysctl.html)。

### 历史时钟与 holdover

`clock_mapping_version=2`：按帧采集时间所在的历史 segment 判断有效性，而非按处理时最新报告判断。
模型过期但仍有历史估计时，继续连续、有界的 offset 映射，同时明确 `clock_sync_valid=0`。
新报告不会重写已映射区间。不存在历史模型、过早超出保留范围、缺少 sender 系统时间或超出安全范围时，
仍降级使用原始时间并标记无效。没有改原始时间列，也没有把每帧时间硬钳成上一帧加 1。

兼容注意：v1 的无效行可能直接使用 sender 原始时间；v2 的无效行可能是 holdover 估计。
下游仍读取 `global_timestamp_us`，但必须同时保留 `clock_sync_valid`、`clock_mapping_version`、
`clock_applied_offset_us` 和 `clock_model_reference_timestamp_us`。
不能把无效区间当作已校准样本，不能从有效标志推导内容硬同步或小于 5 ms 的实测精度。

### 采集时长与接收耗时分离

`recording_quality_version=2`：原 `rgb_media_duration_us`、`depth_media_duration_us` 保留，
修正为有效采集窗口内首尾 global 的跨度；新增 `rgb_receive_duration_us`、
`depth_receive_duration_us` 单独记录到达跨度。实际 FPS、重定时目标也改用采集跨度。
`recording_uploader.py` 将版本和接收耗时一起保留到 NAS 最终 ready 标记。

兼容注意：没有版本字段的旧元数据，其 media_duration 可能混入网络等待时间，不能与 v2 直接比较。
采集跨度不等于精确容器时长，后者仍需 ffprobe；跨度也不能证明中间无缺口。
正式交付规则未变：只消费带 `recording_ready.json` 的最终目录，并另行检查质量、CSV 和视频解码。
不重写历史 NAS 文件、不清除 partial、不填重复帧或降低质量阈值。

## 回归测试

- 32 MiB 发送缓存、两包共 8 MiB、接收端暂停 6 秒：旧实现不能触发预期反压，修复后通过。
  验证分段重试有界、同连接传输、字节完整。
- 正 offset 90 ms 后模型超时：旧实现回退原始时间造成倒退，新实现连续 holdover 并标记无效。
  覆盖新报告到达后迟到帧历史不变、当前模型超时但历史采集有效的情况。
- 真实 UDP/TCP 注入历史模型和迟到 RGBD，检查最终 CSV 的版本、offset、有效标记和视频帧索引。
- 相同采集时间的 RGB 尾部延迟 800 ms：旧实现错报时长差，新实现保持采集时长、FPS、重定时一致，
  单独报告接收耗时差，不掩盖延迟。
- uploader 测试检查新字段完整进入最终 ready 标记。

测试执行结果、实机网络和上线版本以后续部署验收记录为准，不能把增加测试等同于已完成长录验收。

## 发布与回退

先确保没有用户录制及尾帧收尾，再备份各机当前二进制、配置、服务定义。
在各架构按实际加载的 SDK 编译、测试；receiver 同步更新 uploader，发送端保留其相机配置。
核对运行进程 binary SHA256 和 build commit，而不是仅查看生产目录 Git HEAD。
回退同样先结束录制和收尾，原子替换备份二进制及对应 uploader；52d2 网络配置单独回退。
不要把 SDK v1 编译产物部署到 v2 相机设备。
