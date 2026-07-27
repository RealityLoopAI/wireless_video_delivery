# NAS 优先 capture 与普通 MP4 双阶段发布归档

日期：2026-07-27

## 1. 问题

旧 uploader 必须在 receiver 本地 staging 上完成整个 RGB fragmented MP4 到普通 MP4 的无损重封装，再复制全部结果到 NAS。长切片会让本地唯一副本和 staging 空间持续被占用；停止录制后虽然 receiver 已能快速接受下一次开始，但“数据安全到 NAS”和“播放器兼容 MP4 已交付”没有分层，现场只能等待完整后处理。

本次目标是优先保证持续录制：

1. receiver 实时录制和 TCP media 主链路不改。
2. closed capture 尽快可靠写入 NAS，随后释放本地空间。
3. 普通 MP4 后处理继续后台串行执行。
4. 任何中断都不能提前发布 ready，也不能删除唯一副本。

## 2. 新数据路径

```text
receiver local staging
  -> closed fMP4 + Depth MKV + frames.csv + meta
  -> recording_staged.json
  -> NAS/.gwv3_capture_queue/.gwv3-uploading-*
  -> 文件 fsync + 清单校验
  -> recording_capture_ready.json
  -> 原子进入稳定 capture 目录
  -> 删除本地副本
  -> NAS capture 上无损 remux 普通 MP4
  -> recording_nas_finalized.json
  -> 写发布日志和 pending ready
  -> 移动到最终相对路径
  -> 清理内部标记
  -> 原子生成 recording_ready.json
```

正式 `recording_ready.json` 绝不会出现在隐藏 capture queue。下游递归扫描 NAS 时也不会把 capture 或 remux 中间态当成完成数据。

## 3. 崩溃恢复

发布前在 capture queue 根目录写 `.gwv3-publish-*.json`，记录 capture 目录、最终相对路径、正式 ready 文件名和完整 ready payload。

覆盖以下中断窗口：

1. capture 复制中断：本地 staged 数据保留，隐藏临时目录下次清理重试。
2. NAS remux 失败：NAS 原始 fMP4 capture 保留，不回退成损坏的普通 MP4。
3. remux 完成、目录移动前退出：内部 finalized 标记保留，下次直接重试发布，不重复破坏数据。
4. 目录已移动、正式 ready 尚未生成时退出：启动后按 journal 定位最终目录，完成内部标记清理，再把 pending ready 原子改名为正式 ready。
5. 正式 ready 已生成、journal 尚未删除时退出：校验身份后只做幂等清理。

## 4. CIFS 兼容问题

首次生产短测发现：

```text
PermissionError: [Errno 13] Permission denied:
NAS/.gwv3_capture_queue/<capture> -> NAS/<camera>/<date>/<time>
```

目录、父目录和账户权限均正常。最小复现实验证明，CIFS 可以跨父目录 rename 普通目录，但目录内只要存在仍被进程打开的 `.gwv3_uploader.lock`，同一次 rename 就返回 `EACCES`。在 ext4 临时目录上的自动测试不会触发这一差异。

修复：

1. NAS capture 锁移动到 capture queue 根目录，命名为 `.gwv3-lock-<capture>`。
2. 被移动目录内部不再包含打开的文件。
3. 发布前清理旧版本遗留的目录内锁标记。
4. 锁描述符关闭后再删除根目录锁文件。

修复后原先积压的五路 capture 全部幂等发布，未重新录制、未丢数据。

## 5. 状态与调度

uploader 状态升级为 `gwv3_recording_uploader_status_v2`：

```text
pipeline_mode=nas_first_fmp4_finalize
local_pending_segments/bytes
nas_finalize_pending_segments/bytes
publish_recovery_journals
captured_segments
last_capture_success_us
pending_segments/bytes
```

语义：

1. `local_pending_segments=0`：本地 closed 分片已被 NAS capture 安全接管。
2. 总 `pending_segments=0`：普通 MP4、最终目录和正式 ready 全部完成。

生产配置：

```text
upload_bandwidth_limit_mbps=240
quiet_before_segment_finalize_ms=10000
pause_record_queue_bytes=8388608
pause_record_queue_oldest_age_ms=500
```

后台 capture/remux 在 receiver finalizer 活跃、录制队列达到 8 MiB、任一路最老包等待达到 500 ms，或进入切片边界前 10 秒时暂停，实时录制优先。

## 6. 自动测试

通过：

1. `recording_uploader_fault_injection`
2. `receiver_staging_pipeline_integration`
3. `receiver_hardening_integration`
4. `sync_input_guard_unit`
5. Python `py_compile`
6. NAS 不可用、本地唯一副本保留。
7. capture 复制中断恢复。
8. NAS remux 失败保留原始 fMP4。
9. remux 后、目录移动前中断恢复。
10. 目录移动后、正式 ready 前中断恢复。
11. capture queue 不暴露正式 ready。
12. prefixed marker 和路径越界拒绝。

## 7. 生产短测

### 第一轮兼容测试

五路约 112 秒录制：

1. 五个最终目录均发布 ready。
2. RGB MP4 顶层 box 均为 `ftyp/free/mdat/moov`，无 `moof`。
3. RGB 时长约 111.600 至 112.367 秒。
4. Depth 时长约 111.966 至 112.566 秒。
5. `sync_input_guard --verify-video-frames` 全部通过。
6. 五路 RGB 实际解码帧数分别为 3369、3371、3371、3367、3348，均与 CSV 的正式视频索引一致。

该轮在定位 CIFS 锁期间主动停过 uploader，因此不用于评价最终发布耗时。

### 干净时延测试

五路约 12 秒录制，约 106.3 MB：

1. `stop-all` HTTP 响应：0.7 ms。
2. receiver 本地 finalizer 归零：停止后 0.225 秒。
3. 五路 NAS capture 全部完成、本地 backlog 归零：停止后 8.073 秒。
4. 五路普通 MP4 和正式 ready 全部完成：停止后 16.996 秒。
5. uploader `failed_attempts=0`、总 backlog=0、发布 journal=0。
6. 五路 RGB 实际解码帧数分别为 389、389、390、388、387，均与 CSV 索引一致。
7. 五路在线发送约 29.9 至 30.9 fps，receiver 录制队列为 0、写错误为 0。

## 8. 当前结论与边界

短测已证明新路径能把“停止后可继续录制”“数据已安全到 NAS”和“普通 MP4 已交付”拆开，并能在 CIFS 上正确恢复发布。它显著缩短本地 staging 占用时间，普通 MP4 后处理不再阻止下一次录制。

按用户要求，本次没有执行两小时以上长测。正式替代 2026-07-25 的两小时验收结论前，仍需补一轮至少两小时、最好四小时的五路连续录制，覆盖多个 15 分钟自动切片，并核对：

1. 每个切片前后可拼接性和帧数。
2. capture 生产速率长期高于媒体生成速率。
3. NAS finalize backlog 不跨多个切片持续增长。
4. staging 空间、receiver 队列、CIFS 错误和服务重启计数。
5. 停止后最后一组切片的 capture 与最终 ready 时间。

部署前备份保留在接收端：

```text
05_tools/recording_uploader.py.pre_nas_first_20260727
06_configs/receiver_ubuntu-01.json.pre_nas_first_20260727
```
