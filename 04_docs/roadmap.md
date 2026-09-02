# Roadmap

更新时间：2026-09-02

本文只记录尚未完成或仍需规模验证的方向。已实现能力写入对应正式文档，不在这里重复列为规划。

## Current Baseline

主线已具备多设备 RGB-D 采集、H.264/Depth 压缩传输、独立状态和 CLOCK_SYNC、统一录制窗口、fMP4/FFV1 分片、直接 NAS 原子发布、网页预览、全画质 H.264 下游接口、音频归档、语音拍照、GPIO 控制、热插拔恢复和自动测试。

当前主要上限不是缺少基本功能，而是无线、USB、NAS 和无硬件同步条件下的可证明稳定性。

## Priority 1: Production Observability

- 为每路建立明确的 SLO：采集 FPS、发送 FPS、媒体 age、队列 age、重传、clock delay 和存储延迟。
- 在 Web 状态区分采集故障、媒体故障、预览故障、录制故障和历史错误。
- 统一生成每段健康报告，自动判定 ready、可解码、可 seek、帧索引一致、同步窗口有效和音频质量。
- 对 NAS 隐藏目录滞留、record queue 增长、CLOCK_SYNC 失效和 Wi-Fi 排队设置告警。

验收标准：不看聊天记录和现场经验，也能从状态与日志定位故障环节。

## Priority 2: Sustained Recording Capacity

- 在实际目标路数下完成连续 8 小时测试，覆盖至少 32 次 15 分钟轮转。
- 同时测试正常停止、立即重启录制、sender 短时掉线、receiver 重启恢复和 NAS 抖动。
- 测出 NAS 直写在并发回放、文件扫描和 SMB 客户端访问下的安全余量。
- 明确 5/6/7 路的生产码率预算，而不是只按 AP 理论吞吐判断。

若平均输入长期高于 NAS 或网络的可持续吞吐，任何队列都会最终积压。根治只能是降低数据量、增加独立链路/存储带宽或分接收端，不能仅靠加大内存。

## Priority 3: Transport Isolation

当前主媒体仍使用 TCP。候选升级按风险从低到高：

| Candidate | Benefit | Cost |
| --- | --- | --- |
| RGB/Depth 独立 TCP 连接 | 消除同路 RGB 与大 Depth 包的队头阻塞 | 连接和会话管理增加 |
| 每相机独立 TCP 入口 | 故障隔离更清晰 | 接收端端口/连接治理 |
| SRT | 抖动、拥塞和丢包恢复更成熟 | 集成、时延和参数复杂 |
| RTP/UDP | 低时延、适合预览 | 正式 Depth 完整性与重组要求高 |

协议升级前必须先定义丢包语义、重连边界、时间戳、录制可靠性与降级；不能只用短时低延迟作为验收。

## Priority 4: Wireless And Hardware

- 固定 5 GHz 频段/BSSID、关闭省电并保留 LubanCat 厂商驱动队列限制。
- 按真实空口占用、重传和终端能力分配 AP，不按设备数量平均分。
- 为接收端 vNIC/RX 多队列和宿主网络做专项验证。
- 新硬件优先提供独立 USB 3.x root port、有线千兆/2.5G 和稳定散热。

增加 AP 只有在 AP 空口是瓶颈且 AP 之间有独立信道、接收端上行足够时有效。发送板 USB Wi-Fi 驱动、天线、电源或共享 USB root hub 有问题时，加 AP 不能根治。

## Priority 5: Time And Content Synchronization

- 录制前对 chrony、CLOCK_SYNC delay、model age 和全局时间单调性做门禁。
- 每段自动输出跨 sender 最近邻 delta 分布和异常窗口。
- 开发内容同步事件的校准与周期复核工具，估计固定相机相位偏移。
- 有硬件条件时评估支持外部 trigger/sync 的相机与同步线。

软件 clock sync 可以建立统一时钟轴，但不能控制各传感器曝光。严格内容级同步的最终路线仍是硬件触发。

## Priority 6: Code Boundaries

当前入口已拆为应用生命周期和职责化私有实现单元。下一步仅在接口稳定且有测试时继续把以下边界转为独立 translation unit：

- receiver recording/finalization；
- receiver preview and H.264 fan-out；
- sender camera adapter；
- sender Depth codec；
- status schema builder。

目标是减少编译耦合和审查范围，不做改变行为的大规模重写。

## Release Policy

- `main` 是唯一长期和生产分支。
- 设备差异进入配置、service 或小型适配模块，不建立永久设备分支。
- 新实验使用短期分支和隔离 worktree，合并后删除。
- 每次数据格式、API、配置或落盘语义变化都更新正式文档和自动测试。
- 不在主线保存密码、原始录像、运行日志和按日期命名的试错报告。
