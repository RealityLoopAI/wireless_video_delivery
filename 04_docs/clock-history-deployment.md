# Clock History Rollout Acceptance

日期：2026-09-09，时间为北京时间。对应 [问题复核](recording-clock-history.md)。

## 结论

用户授权“上线吧”后，已部署媒体核心版本 `9d2ef259766c477a06020cb75c89d282fd0499b1`。
范围为当时在线的六台 sender 和一台 receiver，不代表离线或其他接收集群也已升级。
**部署完成，但 52d2 的 RGB 接收前积压再次出现，不能宣布整个系统的长录缺口已经根治。**

## 部署范围

| 节点 | 维护时 SSH 地址 | SDK | 原机 CTest | 新进程 PID |
|---|---|---|---|---:|
| receiver | `loop@192.168.1.196` | 不适用，x86_64 | 33/33 | 394362 |
| rk3588-ubuntu_cam01 | 本机，`192.168.5.4` | v1.10.27 | 8/8 | 1517592 |
| orangepi5pro-b439137c_cam02 | `orangepi@192.168.1.125` | v1.10.27 | 8/8 | 1952157 |
| orangepi5pro-ab748372_cam01 | `orangepi@192.168.1.147` | v1.10.27 | 8/8 | 3705874 |
| orangepi5pro-fe0f7222_cam01 | `orangepi@192.168.5.5` | v1.10.27 | 8/8 | 1958394 |
| lubancat-52d2ef0c_cam01 | `cat@192.168.0.105` | v2.8.6 | 8/8 | 2239108 |
| lubancat-e8cc0cb3_cam01 | `cat@192.168.0.109` | v2.8.6 | 8/8 | 2181435 |

LubanCat 位于 `TP-LINK_5G_215E` 的 NAT 后，receiver 看到的共同来源是 `192.168.1.159`，
不能将它当作两台机器各自的 SSH 地址。维护时暂时停止本机 sender、切到该 AP 操作，
随后恢复本机原 `88888888` Wi-Fi 和 sender；自动恢复安全 timer 已撤销。

各机使用干净的同一源码归档，各自 `RelWithDebInfo` 编译。SDK 从原进程已加载的库路径确认，
没有用 SDK v1 二进制覆盖 v2 相机。LubanCat 编译时的 Mali ELF 警告属于既有 vendor 库，
没有替换图形驱动或 SDK。运行状态 API 的 receiver/sender build 均为 `9d2ef259766c`、dirty=false。

曝光、gain、白平衡、profile、相机身份、网络配置、服务启动参数保持不变；
上线前后配置 SHA256 一致。只替换生产二进制，生产目录的旧 Git HEAD 不作为运行版本依据。
独立发行源码在每台机器的 `~/wvd-releases/clock-history-9d2ef25`。

## 已上线的修复

1. 用按采集时间查询的历史 offset 映射，替代用当前短窗口 drift 外推数分钟前帧的做法。
2. global 和诊断字段使用同一个加锁模型快照，保留原始采集时间，新增历史映射诊断列。
3. sender 编码输出不再提前执行网络 P 帧丢弃，统一由实际发送消费者按 IDR 顺序恢复；
   每个编码输出仍消费自己的 timing，避免额外缺帧和 FIFO 时间关联残留。

未修改 TCP 格式、编码 pipeline、RGBD pairing、NAS 原始目录或既有 CSV 字段。
没有通过调整 `last_global + 1`、重复帧、降档位、降低帧率或清理旧录像来制造通过结果。

## 第一次实机短录制

六路同时开始/停止；session `1788932926366985`，NAS 相对路径均为
`<camera_key>/2026-09-09/134847/`。脚本请求录制 60 秒，实际统一采集窗口约 59.005 秒，
因为当时脚本计时包含了约 1 秒的计划启动等待，不应以固定 1800 帧判定本批完整性。

| 相机 | RGB / Depth 帧数 | RGB 最大间隔 | Depth 最大间隔 |
|---|---:|---:|---:|
| 52d2ef0c_cam01 | 1771 / 1771 | 50.122 ms | 40.768 ms |
| e8cc0cb3_cam01 | 1771 / 1770 | 37.991 ms | 36.608 ms |
| ab748372_cam01 | 1770 / 1775 | 36.316 ms | 41.986 ms |
| b439137c_cam02 | 1768 / 1775 | 64.121 ms | 42.142 ms |
| fe0f7222_cam01 | 1769 / 1773 | 36.520 ms | 44.570 ms |
| rk3588-ubuntu_cam01 | 1767 / 1772 | 67.981 ms | 43.433 ms |

六路结果：

- RGB/Depth 相邻 `global_timestamp_us` 倒退、重复及超过 500 ms 的缺口均为 0。
- 首尾之间的 source frame_id 缺号均为 0；不意味着捕获回调之前绝对无丢帧。
- 所有行 clock 有效、`clock_mapping_version=1`；三个新诊断列均存在。
- 12 个视频逐一 `ffprobe -count_frames` 和 `ffmpeg -f null` 全量解码，均无报错，帧数匹配 CSV。
- 六个最终目录 `recording_ready.json` 的 ready=true、质量 complete，`meta.json` 同样为 complete。
- 停止窗口至最终 NAS ready 元数据时间约 8.1 至 15.1 秒。这是本批短测，不可线性外推 15 分钟。

### 验收脚本误判及修复

第一次脚本返回失败，原始失败 JSON 保留。它只看 recording_state=idle、队列=0、uploader pending=0，
未考虑 stop 已返回、尾帧补收仍在运行、finalizer 尚未登记的间隙，过早判断没有完成文件。
随后现场等待并直接核查正式 NAS 目录，证实本批已完整交付。

已给 `test_deployment_acceptance.py` 增加延迟补收模拟：旧脚本复现失败，修复后通过。
新等待条件检查各相机尾帧/收尾状态，并要求各相机的 completed 计数超过录制前基线，
再检查全局队列、finalizer 和搬运状态。报告新增 stop 请求时间及等待交付状态耗时。
这只是测试工具修复，不需要再次重启 C++ 数据面，也不把旧失败报告改写成成功。

## 第二次实机短录制

session `1788933163085270`，正式目录 `<camera_key>/2026-09-09/135244/`。
修复后的验收工具返回成功，停止请求至 delivery 状态就绪约 **40.6 秒**，其中包含等待 52d2 的网络尾帧。
这不是单纯的 NAS 文件复制耗时。实际采集窗口仍约 59 秒，起始安排和生效窗口以 CSV/元数据为准。

| 相机 | RGB / Depth 帧数 | RGB 最大间隔 | Depth 最大间隔 |
|---|---:|---:|---:|
| 52d2ef0c_cam01 | 1770 / 1770 | 52.026 ms | 48.426 ms |
| e8cc0cb3_cam01 | 1771 / 1770 | 36.852 ms | 36.871 ms |
| ab748372_cam01 | 1769 / 1774 | 36.363 ms | 42.369 ms |
| b439137c_cam02 | 1769 / 1774 | 64.182 ms | 42.078 ms |
| fe0f7222_cam01 | 1770 / 1775 | 36.383 ms | 44.565 ms |
| rk3588-ubuntu_cam01 | 1763 / 1766 | 64.028 ms | 68.462 ms |

12 个视频再次全量解码无报错，帧数匹配 CSV、RGB 文件索引连续。
六路 RGB/Depth 相邻 global 倒退、重复、超过 500 ms 的间隔、首尾之间 source frame_id 缺号均为 0。
本批 52d2 RGB 到达延迟 P95=34.798 秒、最大=36.420 秒，但尾帧补收最终保留了这些迟到帧。
e8cc RGB 到达延迟最大约 1.31 秒。这些是传输到达延迟，不是画面采集时间间隔。

**额外同步边界：52d2 的 Depth 有 294 行 `clock_sync_valid=0`**，当时回退到原始 sender system time。
其余流本批无无效行。不能把未倒退、文件完整或脚本 ok=true 等同于全部行已精确校准。
下游须检查 clock_sync_valid；无效区间保留但不得当成已校准的高精度训练样本。
历史模型对超过外推时间限制的数据主动降级，不在本次上线中放宽限制来伪造有效同步。

### 质量统计遗留问题

本批六个最终目录 ready=true，但只有五路 quality=complete；52d2 为 partial，理由是
`RGB/depth duration drift exceeds 500 ms`。其 `rgb_media_duration_us=94906143`，
`depth_media_duration_us=58976157`，而实际两个容器时长都是 59 秒。

已从代码确认：`StreamRecordStats::add()` 记录的是收包处理时的 `packet_local_us=now_us()`，
`media_duration_seconds()` 使用 first/last_local_us 差值，质量检查再拿两路结果做时长差比较。
RGB 迟到约 36 秒时，统计把网络补收耗时混入“媒体时长”。这是独立于本次 clock 历史映射的既有问题，
不能从这条 partial 原因反推画面真的相差 36 秒。真正的帧缺口仍以 CSV、源 frame_id 和解码结果核查。

本次没有改已交付文件或放宽质量阈值。后续应将采集覆盖、容器时长、接收耗时分开统计并补回归测试，
保留旧字段语义或明确兼容迁移；同时必须处理上述无效 clock 行，不能只修质量标签。

## 仍存在的链路风险

维护前实际读取到：

- 52d2 相机 USB 为 5000M，温度约 50 至 54 摄氏度，不能将当时问题直接归因为 USB2 或过热。
- 52d2 RGB TCP Send-Q 为 65,815,944 字节，约 62.8 MiB；配置 send_buffer_bytes=33,554,432。
- e8cc 同配置项为 65,536。大内核发送缓冲容许延迟隐藏积累，本次保留配置，没有擅自调小。
- 52d2 当时采集约 30 FPS、RGB 发送一度约 10 FPS，旧日志反复出现关键帧保护恢复丢弃。
- 52d2/e8cc RSSI 约 -47/-46 dBm，但信号强不等于持续有效吞吐有保证。

第二次短录制中 52d2 的 RGB 到达延迟又上升到约 34 秒，随后观察到约 45 秒；Depth 当时接近实时。
receiver 录制队列为 0、record_write_errors=0，无 NAS pending 积压，CPU load 约 0.91/1.28/1.05，
内存可用约 8.4 GiB。此时不能将等待尾帧归因为 NAS 上传吞吐。
接收侧 TCP Recv-Q 为 0，旧积压在接收前，但仅凭接收侧 `ss` 不能得出 sender 当时重传率。

后续本次上线观察中，52d2 RGB 到达延迟进一步达到约 185 秒、Depth 约 28 秒，RGB 发送约 12 FPS；
ab748372/b439137c 也出现数秒到达延迟。其余节点该次快照仍近实时。
因此短测交付通过不能作为当前全系统稳定或可以直接无人值守长录的结论。

52d2 clock offset 在抖动时达到约 8.5 ms，软件 clock 有效不等于实测误差小于 5 ms。
当前仍需继续单独定位无线/NAT 路径、sender 内核发送队列和长期吞吐不足；
不能靠每次重启清队列或无限增大缓冲宣称根治。

原 10:04 批次的两台 LubanCat sender 日志已轮转，能访问机器后也没有找回该时段完整原始日志。
本次观测不应被写成当时故障每一秒的已证实原因。

## 证据与回退

各设备备份：`~/wvd-rollout-backups/clock-history-20260909/`，包含上线前二进制、配置、服务定义和 manifest。
本机归档：`/home/linaro/文档/clock-history-rollout-20260909/`。
manifest 记录旧/新 binary SHA256、配置 SHA256、目标 commit 和新 PID；配置备份可能含敏感信息，禁止提交到 Git。
原始失败报告、两批短测报告、CSV 分析、解码结果、运行快照及原机测试日志均保留，不删除测试录像。

回退须先手动停止录制、等尾帧/收尾完成，再停对应 service，将备份的 `gemini_sender.before`
或 `gemini_receiver.before` 复制到临时文件并原子替换原 binary，然后启动原服务。
receiver 为 loop 用户服务，sender 为系统服务；服务名和绝对路径以该机 manifest 为准，
不要对所有机器套用同一个 SDK 或服务名。回退后用 `/proc/<pid>/exe` 校验二进制，检查收流。

本次未做多次 15 分钟切片、2 小时/8 小时实拍长录、受控丢包限速测试、跨机内容级校准或音频专项验收。
旧 NAS 缺失的帧无法由本次软件上线补回。
