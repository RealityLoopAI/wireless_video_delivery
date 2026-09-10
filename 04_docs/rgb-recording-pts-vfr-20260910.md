# RGB Recording PTS Repair

日期：2026-09-10。状态：代码修复及隔离测试完成；现场停止录制后，14:50:35 已上线接收端。

## 问题和原因

13:11:20 批次的 52d2 与 e8 分别记录 11,266 和 15,551 个 RGB 帧，
MP4 时长分别为 375.533333 秒和 518.366667 秒，等于帧数除以 30，
而录制窗口为 643.418316 秒。原录制命令把无时间戳的 H.264 裸流交给 FFmpeg，
使用输入 `-r` 和 `+genpts` 重建时间轴，未将 CSV 的采集间隔传入容器。
这解释播放时长压缩，不解释真实帧为何没有到达。

既有逐帧审计发现源帧号缺失、编码后到接收端排队较长，以及超过有效期的校时模型。
AP 分配已调整；调整后的首个 900 秒分片两路各收到 27,020 个 RGB 帧，
源帧号无缺失、校时无效数为零。这只是一个窗口的改善，不代表无线故障永久消失。
详见 [原始全链路审计](recording-timeline-audit-20260910.md)。

## 修改内容

1. 新增 `TimestampedH264Writer`，每个录制 writer 独立维护状态，使用 FFmpeg
   libavformat 的 NUT 封装，将每个 H.264 AU 的微秒 PTS 传给原 FFmpeg 子进程。
2. PTS 为该帧 `global_timestamp_us` 减去本分片全局起点。无全局值时才走既有
   system/local 降级；不修改 CSV 原始字段、不把校时无效伪装成有效。
3. MP4 使用 `-copyts`、`delay_moov` 并关闭自动负时间平移，保留首帧偏移及中途缺口。
   普通 MP4 收尾和 uploader 重封装同步保留时间戳。
4. 调试恢复副本改用带时间戳的 `rgb_recovery.nut` 恢复 MP4，避免恢复后又回到固定帧率。
   只有开启 `write_debug_h264` 才有这份额外副本，原调试 H.264 仍保留。
5. 元数据新增 `rgb_timestamp_mode=capture_global_vfr_v1` 和
   `rgb_pts_origin_global_us`；`rgb_retime_scale=1.0`。
6. 拒绝负值、重复或倒退 PTS、一个时间戳对应多个 AU，以及缺少独立 DTS 的 B 帧，
   不静默编造播放时间。当前发送端为无 B 帧、AU 对齐输出。

不改变 Sender、TCP 协议、RGBD 配对、CSV 字段或 NAS ready 契约。无 RGB 重编码。
内部存在一次带 FFmpeg padding 的压缩包复制，不宣称零拷贝。
Depth 的 CFR 路径未在本次修改，仍必须按 CSV 对齐。

## 自动验证

测试在 receiver 的独立临时目录与隔离端口运行，未重启生产服务。

| 验证 | 结果 |
| --- | --- |
| 原生产二进制跑新增 PTS 用例 | 按预期失败，输出 0、0.033333、0.066667、0.1、0.133333 秒 |
| 新封装同一输入 | 保留 0.2、0.233333、0.266666、1.266666、1.299999 秒，误差小于 1ms |
| fMP4、普通 MP4、FFmpeg 故障后的 NUT 恢复、uploader 重封装 | 全部通过逐帧 PTS、帧数、解码校验 |
| 时间戳倒退、多 AU、B 帧、输出失败、重复关闭、正常 I/P 序列 | 单元测试通过 |
| receiver 全套 CTest | 36/36 通过；再次完整复跑 36/36，146.53 秒 |
| ARM64 receiver 编译 | 通过 |
| ARM64 writer ASAN/UBSAN，启用 leak detection | 通过，未报告错误 |

ARM 本机无 FFmpeg 命令行程序，因此未声称本机完成端到端媒体测试。
x86 接收端新增安装开发库；安装前确认没有升级已有运行库，也未因此重启服务。
长时间真实多机录制验收尚未执行。
完整复跑记录：[CTest log](../08_reports/rgb-recording-pts-vfr-20260910/ctest.log)。

## 下游如何使用

先检查正式目录的 `recording_ready.json`，再按 CSV 表头解析，
通过 `rgb_video_frame_index` 关联实际图像，用 `global_timestamp_us` 匹配跨设备帧。
新格式 MP4 的 sample PTS 加 `rgb_pts_origin_global_us` 可还原该图像的全局时间。
不要用 `frame_index / 30` 表示真实采集时间，也不要用平均 FPS 证明连续性。
`clock_sync_valid=false` 仍应标为低可信数据；本修复不提升其时钟精度。
旧录制保持原样，可离线依据 CSV 重建时间轴，但无法恢复不存在的图像。

## 上线与回退

本次实际部署：接收端 `192.168.1.196`，二进制版本 `bad0096fc0b1`，
`build_dirty=false`，源码指纹 `2291f2d3163a3bb3`。
发布前再次确认 idle、finalizer=0、NAS 待发布=0；上线后六路 RGB/Depth 全部恢复接收，
六路 `clock_sync_valid=true`，未自动启动录制。
干净源码保留于 `/home/loop/wvd-pts-release-bad0096`，
旧二进制和 uploader 保留于 `/home/loop/wvd-backup-rgb-pts-bad0096`。
现场 uploader 只合并了三行 FFmpeg 参数，保留现场既有未提交修改。
正式版本另跑 RGB PTS 集成测试通过，耗时 9.47 秒。
这不等于已完成新版真实相机长时间录制验收。

1. 等现场停止当前录制，确认 finalizer 和待发布任务状态，保留原二进制与配置。
2. 使用包含本提交的干净源码在 x86 接收端编译，确认版本信息。
3. 替换接收端二进制。现场 uploader 有未合并修改时，只合并本次时间戳参数，禁止整文件覆盖。
4. 重启接收端后短录制，检查 sample PTS 与 CSV、帧数、解码、音视频关系及 NAS ready。
5. 如失败，停止新录制并保存异常分片，恢复原二进制和对应 uploader 参数。
   新旧数据通过元数据模式区别；不批量改写已有 NAS 数据。

待验证：真实设备长测、无线重新拥塞时覆盖率、模型失效比例、断电恢复及 NAS 积压。
本次不能承诺所有掉帧和跨机内容同步问题已根治。

## 官方参考

- [FFmpeg MOV/MP4 muxer](https://ffmpeg.org/ffmpeg-formats.html#mov_002c-mp4_002c-ismv)
- [FFmpeg muxing example](https://ffmpeg.org/doxygen/trunk/muxing_8c-example.html)
