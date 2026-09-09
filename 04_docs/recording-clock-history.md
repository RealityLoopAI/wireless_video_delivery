# Recording Gaps And Clock History Review

日期：2026-09-09。所有现场时间为北京时间。

## 1. 结论与上线状态

本次复核对象是两台 LubanCat 的 `2026-09-09/100445` 批次。
**问题仍然存在，不能宣布整个系统已经根治或适合无损长录。**

已完成代码修复并自动验证：

1. 接收端用当前 drift 模型外推数分钟前的帧，造成 `global_timestamp_us` 倒退。
2. 同一帧两次读取可变 clock model，模型诊断列与实际映射可能不一致。
3. 发送端在编码输出和网络消费处共用丢帧保护，提前丢弃后续可发送帧，并可能遗留 FIFO 时间记录。

没有改曝光、分辨率、编码 pipeline、TCP wire format、相机身份、pair_id 或 NAS 原文件。
没有更换生产二进制、重启生产服务、切换 Wi-Fi、停止当前录制。
正在录制的 sender 仍为 `b031e8ee2aed`，receiver 仍为 `ad80eb33f0b8`。

## 2. 原始证据

NAS 相对路径：

```text
lubancat-52d2ef0c_cam01/2026-09-09/100445/
lubancat-e8cc0cb3_cam01/2026-09-09/100445/
```

读取了两路完整 `frames.csv`、`meta.json`、`recording_ready.json`，以及 receiver
10:04 至 10:25 的日志。本次没有重新全量解码 MP4/MKV，不声称完成了画面内容检查。
原始 CSV 和日志证据保留在控制机 `/home/linaro/文档/recording-gap-20260909/`，不提交视频和大日志到 Git。

| 指标 | 52d2ef0c RGB | e8cc0cb3 RGB |
|---|---:|---:|
| 已录视频索引行数 | 15991 | 26627 |
| 首尾 source frame_id 跨度 | 27021 | 27020 |
| 首尾之间缺失的源帧编号数 | 11030 | 393 |
| 相邻 global 时间戳倒退 | 6 | 0 |
| 最大 global 间隔 | 1.898997 s | 1.032476 s |
| 入发送队列到接收，P50 | 72.271 s | 5.769 s |
| 入发送队列到接收，P95 | 350.421 s | 29.883 s |
| 入发送队列到接收，最大值 | 371.920 s | 31.184 s |

源帧跨度不是固定的 27000：相机实际帧率和首尾边界有小幅差异。
按用户的 900 秒、30 FPS 标称口径，52d2 覆盖率仍为 59.23%。

52d2 Depth 有 26743 帧、7 次相邻 global 倒退；其设备时间、system time 都没有倒退。
e8cc Depth 有 27023 帧，源帧编号跨度恰为 27023，设备时间最大间隔约 33.309 ms。
e8cc 的 12 处 RGB 大缺口均伴随源帧编号跳号，其中两处跳 31，原始 system 间隔也约 1.033 s。
因此缺帧不是仅由 CSV 的 global 换算造成，也不能用平均 FPS 掩盖。

注意：meta 的 out-of-order 计数与“相邻 CSV 行倒退次数”口径不同。
`add_recording_window_summary()` 比较的是之前的最大值，倒退后尚未追平的多个帧都会计数，
所以 52d2 的 meta RGB 12 次与相邻行 6 次并不矛盾。

## 3. 时间戳倒退的已确认原因

六次 RGB 倒退处 `frame_id` 均只增加 1。以下列均单调：

```text
timestamp_us
frame_system_timestamp_us
sender_capture_host_timestamp_us
sender_packet_queued_timestamp_us
receiver_receive_timestamp_us
```

最明显的一处：

```text
frame_id:                    1648996 -> 1648997
frame_system_timestamp_us:   1788919888952023 -> 1788919888985626  (+33603 us)
global_timestamp_us:         10:11:29.038677 -> 10:11:28.948488  (-90189 us)
sender_offset_us:            14611 -> 13845
sender_drift_ppm:            -200 -> 138.313
receiver_receive_timestamp:  约 10:17:38.630 -> 10:17:38.640
```

旧 `ClockSyncManager::get_global_timestamp_us()` 使用：

```text
global = frame_system + latest_offset
       + (frame_system - latest_last_sync) * latest_drift / 1000000
```

一帧晚到约六分钟，此处 elapsed 就是约负六分钟。短窗口测得的 drift 变化几百 ppm，
会被放大成超过一个帧周期的 global 修正跳变。原始系统时间没有跳，此时归因于 chrony
跳时或相机上电计数器回退没有数据依据。

接收函数原来还先 `get_model()`、再 `get_global_timestamp_us()`，后者内部再次取模型。
这是可以由代码确认的并发一致性缺陷，但不能仅凭现有 CSV 断言这六次都命中了该竞态。

修复使用 `ClockSyncTimeline`：保存历史分段，连续且有界地修正 offset，已经映射的范围不再改写；
一次 `map_timestamp()` 返回 global 与模型快照。详情及新诊断列见 [clock-sync.md](clock-sync.md)。
不使用 `last_global + 1`、排序 CSV、重复帧或改原始采集时间来掩盖异常。

## 4. 缺帧与积压的已知和未知

52d2 前几分钟每完整分钟约 1801/1802 帧，之后接收延迟从毫秒上升到几十秒、数分钟。
10:11 至 10:15 的若干分钟只有约 309 至 471 帧。
e8cc 缺口集中在 10:13、10:14、10:16，前后帧实际到 receiver 时普遍已晚约 30 秒。

两路的连接 session 在这段日志中没有替换。12:00、12:16 现场状态还显示：

- 52d2 输入约 30 FPS，但发送帧率显著不足，RGB 和 Depth 到达时可晚数分钟。
- sender 持续上报 `rgb queue overwrite; waiting for next keyframe`，52d2 还出现 encoder lag reset。
- receiver 录制队列为 0，`record_backpressure_waits=0`、`record_write_errors=0`。
- receiver 本地 staging 剩余约 87%，finalizer 没有失败。
- NAS uploader `failed_attempts=0`；活动文件仍在增量搬运，不能把活动镜像差值当作已完成目录丢失。

这支持“接收前积压与 sender 丢帧”的判断，不支持把当前缺口直接归为 NAS 收尾失败。
但是两台设备在 `TP-LINK_5G_215E` 的 NAT 后，接收端看到的来源均为 `192.168.1.159`。
本次 SSH 到 LAN 地址 `.0.105/.0.109` 超时，未取得这个批次的 sender 原始日志、
无线重传增量、发送 socket 队列和 USB 内核日志。
**无线吞吐抖动、sender 排队、USB/SDK 间歇问题各占多少，仍不能精确归因。**

### 已修复的发送端放大因素

`publish_rgb_encoded_units()` 原来在 `resolve_rgb_encode_timing()` 之前执行网络关键帧保护。
一个 recovery IDR 尚在发送队列时，随后编码完成的 P 帧会在生产端被提前丢弃。
实际网络线程又对队列做同一次保护，扩大了网络异常导致的内容缺口。
当编码输出不能精确按 PTS 命中而走 FIFO 时，提前退出还会遗留旧 timing 项。

现在生产端只标记 recovery IDR，仍消费每个编码输出的 timing；
是否缺少参考帧、是否必须丢弃 P 帧由真正按顺序发送的消费线程决定。
只有 IDR 真正发送成功才解除保护，TCP 半包续传与有限队列上限保持不变。

这修复不必要的额外丢帧，不会把有限缓冲变成无限可靠存储。
链路长期小于产出速率时仍会溢出；本次没有通过降分辨率、降低目标 FPS 或无限加 RAM 来掩盖它。

## 5. 自动验证

实际执行：

| 验证 | 结果 |
|---|---|
| 新历史映射回归测试运行于未修复算法 | 失败，复现相邻帧倒退 |
| 修复后的 receiver ARM64 编译及历史映射单测 | 通过 |
| x86 receiver 全套 CTest，临时端口及临时目录 | 33/33 通过 |
| ARM64 sender 编译，SDK v1.10.27 | 通过 |
| ARM64 sender 全套 CTest | 8/8 通过 |
| 八小时模拟时钟历史，Depth 当前、RGB 落后六分钟 | 通过，映射不被新模型改写、历史有界 |
| 真实 UDP report + TCP RGBD + 最终 CSV 集成测试 | 通过，三个 RGB 视频索引和三个 Depth 时间匹配 |
| 900 帧生产端入队策略与消费端 IDR 恢复测试 | 通过 |

八小时是模拟时间推进，不是八小时实机录制。x86 测试副本位于
`/home/loop/wvd-clock-history-20260909/`，没有监听生产 50010/50011/50012。
ARM 主机没有 ffmpeg，媒体验证实际在 x86 的隔离副本执行。
sender 初次 CTest 两个 target 未构建，补构建后重新运行才得到 8/8。

没有完成：SDK v2.8.6 的两台 LubanCat 原机编译部署、无线限速压力下实拍验证、连续长录及内容级校准。

## 6. 旧数据处理

不修改已交付目录，不将 `partial` 改成 `complete`。
只在旁路分析里试算 `frame_system_timestamp_us + sender_offset_us`，52d2 两个流的倒退都变成 0，
但 RGB 最大缺口仍约 1.899 秒，缺的源帧也不会回来。
这个试算进一步支持 drift 外推导致倒退，但旧 CSV 缺少完整历史模型，不能宣称重新获得准确校准时间。
下游须保留原始文件和修订说明，将旧数据的局部时间修复与真正的缺帧分开处理。

## 7. 安全上线与回退

1. 等现场停止录制，确认收尾和补传状态；不得重启清队列后把“恢复在线”当作无损恢复。
2. 保留当前 sender `b031e8ee2aed` 与 receiver `ad80eb33f0b8` 的二进制及配置备份。
3. receiver 只替换二进制；sender 在各自 SDK/架构上编译。固定身份、曝光、profile、服务参数保持不变。
4. 新 receiver 重启后历史为空，先观察有效 report 和队列追平，再手动开始新的测试录制。
5. 至少检查多次 15 分钟切片和持续录制：逐路帧数、源帧编号缺口、global 倒退、最大间隔、
   queue-to-receiver P95/最大值、队列覆盖/guard drop、末尾缺失、ready/quality、CSV 与解码帧数。
6. 实测 RGB/Depth 顺序到达应映射一致，模拟正常输入时映射不能凭空生成新回退。
7. 若链路仍持续积压，先取 sender socket/Wi-Fi/USB 增量日志，单独确定吞吐瓶颈，不能仅继续放大缓冲。
8. 新版异常时停止测试录制再恢复备份二进制。新增 CSV 列可按表头忽略，不回写历史 NAS 文件。

与昨日修复的关系：停止尾帧补收、NAT 控制回复修复仍保留；昨日短测通过不代表已覆盖本次
数分钟积压的场景，详见 [尾帧补收验收](recording-tail-drain-20260908.md)。
