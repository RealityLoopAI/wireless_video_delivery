# 存储保护故障分类、限时恢复与时钟质量报告

## 修复范围

对应 9 月 14 日上午批次全局停止和 complete 含无效时钟记录两项问题。
本次没有修改原始 CSV/录像、时间戳、相机配置、TCP 协议或正式发布标记的消费规则。

## 存储保护

之前所有检查失败统一返回 false，并报 free space 不足；上层又通过错误文案包含 free space 判断是否全局安全停止。

现在使用结构化检查结果和 RecordingStorageError 异常类型：

| 原因代码 | 含义 | 录制工作线程处理 |
| --- | --- | --- |
| local_low_space / local_low_space_percent | 本地实际剩余容量低于配置 | 立即失败并进入原安全停止流程 |
| local_space_query_failed | 本地文件系统容量查询失败 | 立即失败 |
| shared_nas_low_space | 配套共享卷的容量报告低于预留 | 立即失败，不使用旧成功值覆盖 |
| nas_snapshot_wrong_mount | 快照不是目标 NAS 挂载 | 立即失败 |
| nas_snapshot_unreadable / invalid | 快照不可读或格式无效 | 限时重试 |
| nas_snapshot_unavailable | NAS 状态不 ready 或没有可用容量值 | 限时重试 |
| nas_snapshot_stale / future | 测量时间过期或在未来 | 限时重试 |

新录制仍要求当前容量检查通过，不使用重试放行新任务。已进入录制写入的工作线程在暂态检查失败后，每 50ms 重查，最多约 2 秒；保留当前工作包且不继续写盘。其他待写包仍进入原有有界队列，没有增加无界内存缓存。恢复后继续；期限结束仍失败则明确记录原因后安全停止。

这是工作线程内限时等待，不在媒体 socket 读线程或管理 API 中 sleep。系统调用自身的异常阻塞并不受该 2 秒时钟强制中断；共享 NAS 容量来自本地快照，未在这个函数新增网络 stat。

日志包含快照时间、检查时间、可用字节及预留字节等证据。管理状态的 recording_storage 新增 check_reason 和 snapshot_retryable。保留现有 hard_limit 行为用于兼容：检查未知也会阻止新录制，不能只凭 hard_limit 推断磁盘实际满。

真实低容量时仍跳过尾帧补收，防止把宿主机共享盘写满。此次不声称所有存储故障后都可补齐尾帧；超过重试期限的未知状态也仍会停止。原故障边界和尾缺继续如实记录。

## 时钟质量字段

在最终合并 frames.csv 时，仅对保留下来的已录制 RGB 和 Depth 行计数，新字段进入 meta.json 与 recording_ready.json，并由 NAS uploader 透传：

- recording_quality_scope：frame_coverage_and_continuity。
- clock_quality_status：valid / invalid / unknown。
- rgb_clock_frames、depth_clock_frames：被统计的记录行数。
- rgb_clock_invalid_frames、depth_clock_invalid_frames：明确 clock_sync_valid=0 的行数。
- rgb_clock_unknown_frames、depth_clock_unknown_frames：字段缺失或无法识别的行数。

任意明确无效行使 clock_quality_status=invalid；没有无效但有未知行或没有记录时为 unknown；其余为 valid。该字段仅代表 CSV 中有效性标志的统计，不保证画面内容硬同步。原 recording_quality_status/recording_complete 不改变含义，不强行修改 clock_sync_valid。

历史目录不回写，新字段不存在时下游应视为未提供该项评估，不能默认为 valid。

## 测试内容

- 启动拒绝：低容量、陈旧、未来、错误挂载、无效 JSON、丢失快照。
- 运行中：暂停写入并恢复快照后继续处理，核验暂停期间的 60 个 Depth 源编号均在最终 CSV。
- 恢复后再注入低容量，仍须触发全局安全停止，防止缓存旧成功状态掩盖真正故障。
- 持续陈旧快照：超过重试期限后安全停止，错误原因必须是 nas_snapshot_stale。
- 管理 API 在工作线程重试时仍可访问。
- 实际录像测试检查新增时钟字段计数，uploader 检查字段透传。

首次使用本机旧二进制的基线测试失败；本机 ARM 编译新代码成功，但本机缺 ffmpeg，媒体集成验证移至接收端隔离端口/临时目录。测试曾暴露错误文案变更造成故障分类漏判，已经加入专门异常类型修复，不将失败轮次写成通过。

## 尚未完成

发送端持久化缓存、接收确认与去重、百秒断线补传和迟到帧跨切片归属仍未实现。无线驱动/空口的长时间不可用、rk3588 历史源帧缺口也不能由此次修改宣称已根治。两项修复之外的升级需独立故障注入与兼容测试，不混入这一版已验证范围。
