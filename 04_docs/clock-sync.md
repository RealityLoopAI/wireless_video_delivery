# Clock Sync And Frame Alignment

更新时间：2026-09-09

本文描述仓库的新实现。2026-09-09 本次修改尚未替换正在录制的现场进程，
验证及上线边界见 [缺录与时间映射复核](recording-clock-history.md)。

当前同步目标是 dataset-grade 软件统一时间轴，不是传感器曝光级硬同步。

## Clock Layers

系统同时使用三层时间：

1. 相机/SDK `timestamp_us`：设备上电后的相机时间或 SDK 原始时间，保留用于设备诊断。
2. sender `system_timestamp_us`：采集回调取得帧后尽早绑定的 sender Unix epoch 微秒时间。
3. receiver `global_timestamp_us`：sender 系统时间通过 CLOCK_SYNC 模型映射到 receiver 时间轴后的值。

`global_timestamp_us` 的基准是 receiver 系统时钟。所有设备仍应运行 chrony；CLOCK_SYNC 用于运行时测量和补偿，不替代基础授时。

## Four-Timestamp Exchange

sender 每 2 秒通过 UDP 50012 发送 probe，receiver 收到后立即记录 `t2` 并在回复前记录 `t3`；sender 收到回复时记录 `t4`：

```text
t1 = sender send
t2 = receiver receive
t3 = receiver send
t4 = sender receive

offset = ((t2 - t1) + (t3 - t4)) / 2
delay  = (t4 - t1) - (t3 - t2)
```

offset 的方向为 `receiver_clock - sender_clock`。

sender 丢弃身份/sequence 不匹配、负 delay、delay 超过 100 ms、非陈旧模型下单次 offset 跳变超过 50 ms 的样本。最近窗口先取低 delay 的一半样本，再对 offset 取中位数并做 0.8/0.2 平滑；drift 使用窗口回归估计并限制在合理范围。

## Receiver Model

sender 通过 heartbeat/report 上报 offset、delay、drift 和最后同步时间。receiver 只有在该 sender 已从同一来源 IP 完成 probe 后才接受 report，避免错误设备污染模型。

receiver 保留每个 sender 的分段历史，按 **采集系统时间** 查找，而不是按帧到达时的
最新 heartbeat 取模型。同一帧的 global time 与 CSV 模型信息从一次加锁查询取得。

映射公式：

```text
global = sender_system_timestamp + applied_offset(sender_system_timestamp)
```

第一份模型采用常量 offset。后续模型在旧映射上连续衔接，目标为新测得的 offset，
至少用 2 秒、最多每秒修正 1 ms 的速度接近目标，到达目标后保持。
新分段不会覆盖已经分配过的时间范围，RGB/Depth 即使到达时间不同也使用同一历史映射。
窗口估计的 `drift_ppm` 仍上报，但不再向过去数分钟无界外推。

历史每 sender 最多 4096 段，相同 `last_sync_us` 的重复 heartbeat 不新增段。
缺少历史、历史已淘汰、向未来预测超过 `model_timeout_ms`、有效模型超时或数值越界时，
不伪造有效同步。初次模型之前只允许 `model_timeout_ms` 范围内的常量 offset 近似。
历史只在进程内保留，receiver 重启不恢复历史。

`clock_sync_valid=true` 表示有可用映射，不代表已经实测达到 5 ms，也不代表传感器硬同步。
较大的初始误差需要 chrony 和收敛预热；无线延迟不对称仍会影响 offset 的准确度。

若模型无效或包缺少 sender system timestamp，则不丢包：

```text
clock_sync_valid = false
global_timestamp_us = system_timestamp_us if present else timestamp_us
```

receiver 还拒绝与本机接收时间相差超过安全范围的异常 global candidate。原始时间戳永不覆盖。

## Frames CSV

下游至少读取：

```text
sender_id
camera_id
frame_id
timestamp_us
frame_system_timestamp_us
receiver_receive_timestamp_us
clock_sync_valid
sender_offset_us
sender_delay_us
sender_drift_ppm
global_timestamp_us
rgb_recorded
rgb_video_frame_index
recording_window_valid
recording_session_id
```

新版保留全部旧字段，另增加以下诊断列，继续按表头解析：

- `clock_model_reference_timestamp_us`：该历史分段所用报告的 sender `last_sync_us`。
- `clock_applied_offset_us`：本帧实际使用的修正，有效时严格等于 `global_timestamp_us - sender_system_timestamp_us`。
- `clock_mapping_version`：`1` 表示历史分段加有界平滑；旧文件没有此列。

`sender_offset_us`、`sender_delay_us`、`sender_drift_ppm` 是该历史分段的测量信息，
不是最新状态页的值。平滑期间 `sender_offset_us` 与 `clock_applied_offset_us` 可以不同。
下游直接使用有效的 `global_timestamp_us`，不要再自行叠加 offset 或 drift。
若 `abs(sender_offset_us - clock_applied_offset_us)` 超过任务允许的对齐误差，
应判定尚未收敛，不把这一段当作已经校准的多机数据。

RGB 视频帧必须通过 `rgb_video_frame_index` 映射到 CSV，不能假设 CSV 第 N 行就是视频第 N 帧。

## Downstream Alignment

多相机 RGB 对齐流程：

1. 只读取同一 `recording_session_id` 和逻辑 segment window。
2. 过滤 `stream_type=rgb`、`rgb_recorded=1`、`recording_window_valid=1`。
3. 优先使用 `clock_sync_valid=1` 的 `global_timestamp_us`。
4. 对每个目标时刻做一对一最近邻匹配，设定最大时间差；不要重复使用同一源帧。
5. 遇到 global timestamp 倒退、突跳、重复或长空洞时切断连续窗口并报错，不要拉长视频时长填补。
6. 保留原始时间、匹配 delta、来源 frame ID 和视频索引，便于回溯。

可先执行：

```bash
python3 05_tools/sync_input_guard.py --inputs <segment-a> <segment-b> --verify-video-frames
python3 05_tools/build_rgb_sync_manifest.py --help
```

## Why Content Can Still Be Misaligned

全局时间轴一致不代表两个传感器同一时刻曝光。无同步线的 USB 相机各自自由运行，30 FPS 的相位可天然相差接近一帧；自动曝光还可能改变有效曝光中心和运动模糊。USB 调度、SDK 缓冲、H.264 重排序、重连和错误的 CSV 到视频映射也会放大误差。

这些误差不会简单地因为“曝光参数不同”每天累计几秒。出现持续增长的秒级内容差通常意味着：

- 某路 timestamp 不是采集时刻或发生倒退/跳变；
- 下游按 CSV 行号而不是 `rgb_video_frame_index` 取帧；
- 某路丢帧后仍按固定 30 FPS 序号对齐；
- clock model 长时间失效却仍把 fallback 时间当作有效 global time；
- 不同录制 session 或 segment window 被拼在一起。

## Content Calibration

在硬件不可改的条件下，可对每对相机拍摄共同的快速事件或闪光序列，估计固定内容偏移，再叠加到 `global_timestamp_us` 的最近邻匹配上。固定补偿只有在以下条件稳定时才可沿用：

- 相机、USB 口、采集 profile、曝光模式和 SDK 缓冲策略不变；
- 系统无重启、重连或明显丢帧事件；
- 定期控制点证明残差没有漂移。

建议每次开机、换相机/线缆/profile、重连后重新标定，并在长录制中周期性放入可观测同步事件。若要保证画面内容级硬同步，必须使用支持外部 trigger/sync 的相机及硬件同步线；CLOCK_SYNC 无法替代。
