# RGB PTS Live Validation

日期：2026-09-10，接收端版本 `bad0096fc0b1`。

## 范围

经用户要求，在原录制空闲后启动六路实机短录制，持续控制时间 120 秒。
统一录制窗口为 14:52:25.065047 至 14:54:24.075224，共 119.010177 秒；
控制时间与窗口差约一秒来自统一起点的启动提前量，不能把本次直接当作 120 秒有效窗口。
会话 ID：`1789023144065047`。
NAS：`<sender_id>_<camera_id>/2026-09-10/145225`，全部文件保留。

中途两次检查六路均在录制、录制队列为零、写入错误为零。
停止后六路最终目录均 ready 且质量标记 complete，NAS 待搬运清零。
停止请求至交付状态完成约 **21.842 秒**。该时间不包含之后离线读取文件做审计的耗时。

## 逐帧结果

| 相机 | RGB 帧数 | 最大 RGB 间隔 | 校时无效 RGB 帧数 | MP4 与 CSV 最大 PTS 差 |
| --- | ---: | ---: | ---: | ---: |
| 52d2ef0c | 3573 | 49.893 ms | 143 | 209 us |
| e8cc0cb3 | 3573 | 46.416 ms | 0 | 733 us |
| ab748372 | 3570 | 64.061 ms | 0 | 968 us |
| b439137c cam02 | 3571 | 59.937 ms | 0 | 702 us |
| fe0f7222 | 3571 | 64.367 ms | 0 | 773 us |
| rk3588 cam01 | 3567 | 67.919 ms | 0 | 595 us |

六路 RGB 视频 packet 数与 CSV 的 `rgb_recorded=1` 数完全一致，落盘索引连续。
所有 RGB/Depth CSV 均无超过 500ms 的间隔，无全局时间戳倒退。
六路 MP4 时长约 119.012 至 119.017 秒，与本次窗口接近。
已按逐帧 PTS 核对，而非仅凭总时长判断；误差均小于 1ms。
名义 30fps 覆盖率约 99.91% 至 100.08%，这不是丢帧率，实际相机频率和窗口边界会影响该比值。

## 仍存在的问题

52d2 有 143 个 RGB 帧及 143 个 Depth 帧校时无效，时间范围为：

```text
frame_system_timestamp_us: 1789023165393680 .. 1789023170123840
clock_model_reference_timestamp_us: 1789023155373192
model age: 10.020488 .. 14.750648 seconds
```

模型超过 10 秒有效期，仍保留旧映射但如实标记无效。
发送端同期 14:52:37 和 14:52:48 有 `clock_sync probe timeout or invalid response`，
14:52:36 起多次 `media TCP retaining pending packet`。
证据支持链路短时受阻与校时更新中断；尚不能仅凭这些日志区分无线重传、AP 排队或其他网络原因。
这次缓存保留并最终补齐，未出现秒级采集时间缺口；不保证更久拥塞也能完整恢复。

因此 **RGB 容器时间轴修复通过本次短测，但跨设备校时可靠性不能判定完全通过**。
`complete/ready` 表示录制发布完整，不代表每帧跨设备时间精度都可信。
下游仍应检查逐帧 `clock_sync_valid`；未做画面内容级同步标定或长时间切片测试。

## 解码验证说明

初次 `ffmpeg -f null` 验证中，解码进程返回 0，但空输出 muxer 出现重复 DTS 告警，
严格脚本因 stderr 非空而失败。原始失败结果已保留，不把该轮记为通过。
VFR 检查改用 `-fps_mode passthrough -enc_time_base 1:1000000`，保留微秒输出时间基，
然后重新对 RGB 和 Depth 做全文件解码，最终结果见 `audit.json` 的 `decodes`。
复跑六路 RGB 与六路 Depth 共 12 个视频，全部返回 0 且 stderr 为空，审计脚本退出 0。
此修改仅用于离线测试命令，未改录像或生产程序。

## 证据

- [录制启停与发布验收](../08_reports/rgb-pts-live-validation-20260910/acceptance.json)
- [逐帧和解码结果](../08_reports/rgb-pts-live-validation-20260910/audit.json)
- [初次空输出时间基告警](../08_reports/rgb-pts-live-validation-20260910/audit-initial-null-timebase.json)
- [52d2 同期发送日志](../08_reports/rgb-pts-live-validation-20260910/sender-52d2.log)
- [只读审计脚本](../08_reports/rgb-pts-live-validation-20260910/audit.py)
