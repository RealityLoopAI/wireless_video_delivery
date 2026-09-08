# TCP 抗回压修复上线记录

日期：2026-09-08，北京时间 UTC+8。

## 范围与版本

用户确认停止录制后，本次在 16:33 完成以下两端更新：

| 设备 | 更新前运行版本 | 更新后运行版本 |
| --- | --- | --- |
| rk3588-ubuntu，本机发送端 | aca150471515 | b031e8ee2aed |
| 192.168.1.196，接收端 | aca150471515 | b031e8ee2aed |

`b031e8ee2aed219f6f9c98ce5c0a2a8efb72e744` 包含 `ccd3981` TCP 抗回压修复。先前代码虽然提交，但生产进程一直是 12:11 启动的旧程序，所以 14:20 的录制仍受旧超时断连策略影响。

本次没有合入独立审查分支，没有更新其他五个发送端，也没有替用户开始生产录制。其他发送端自动重连到了新接收端，但其发送端保留重试能力不能视为已经升级。

## 修改的实际行为

- 启用录制缓冲的 RGB/Depth TCP 主媒体，在暂时回压时保留同一包、已发送字节偏移和连接，稍后继续发送。
- 单次发送操作约 100 ms 让出控制；保留重试不直接触发丢帧和等待关键帧保护。
- 接收端不再把一次约 2 秒的读取超时等同于连接关闭，保留半包等待后续数据。
- 预览解码器运行状态查询不再等待 FFmpeg 写管道的互斥锁。
- 状态增加 RGB、Depth 独立的 receive age/delay，便于分辨心跳在线、单流停顿与数据积压。
- 真正断连、目标变化或超过约定等待上限仍然进入恢复流程，不保证无限网络中断不丢帧。

没有修改 TCP 媒体封装、GStreamer pipeline、RGBD 配对、CSV 时间戳语义、相机曝光、白平衡、分辨率或压缩配置。

## 发布与回退准备

两端均使用对应架构本机编译，未将 ARM 可执行文件复制给 x86。

本机：

- 构建目录：`/home/linaro/wvd-reorg-20260902/12_build_sender_rollout`。
- 安装位置：`/home/linaro/wvd-reorg-20260902/12_build/bin/gemini_sender`。
- 旧程序和配置备份：`/home/linaro/.local/state/gwv3/rollout-backups/clock-backpressure-20260908`。
- 保持 Orbbec SDK v1.10.27，不切换 SDK。

接收端：

- 独立源码与构建目录：`/home/loop/wvd-releases/clock-backpressure-b031e8e`。
- 安装位置：`/home/loop/wireless_video_delivery-field-v2026.09.07.1/12_build/bin/gemini_receiver`。
- 旧程序、配置和 Web 文件备份：`/home/loop/wvd-rollout-backups/clock-backpressure-20260908`。
- 原服务、工作目录与 Web 工作区保持不变，只替换核心可执行文件。不能仅根据旧工作区 HEAD 推断运行二进制版本；发布源码以上述独立目录为准。

上线前通过管理 API 确认六路全部停止录制，录制队列、活动写入、收尾任务均为 0。新程序先复制为旁路文件，再替换正式路径并启动服务。接收端启动检查失败时，部署流程会恢复旧二进制。

已核对实际运行进程的 `/proc/<pid>/exe` SHA-256，而不仅是磁盘路径：

| 程序 | 更新后 SHA-256 |
| --- | --- |
| gemini_sender | 20bcf5072d6338c842da2760af2834a9b86a4ba1f38b511fc92233521080950d |
| gemini_receiver | 2d8468d9f8f3e94246d63dcef2b957fa07e1f32febbc3a54f378a13f56378231 |

两端配置与备份逐字节一致。接收端 `server.py`、`static/index.html`、Web README 与备份逐字节一致，保留原有本地修改。

## 测试记录

发送端使用 `transport_backpressure_test`，随机 loopback 端口，连续两个测试包总计 8,388,796 字节：

| 接收方停读时长 | 结果 | 最大单次发送调用耗时 |
| --- | --- | --- |
| 3 秒 | 同一连接续传，逐字节一致 | 100.265 ms |
| 6 秒 | 同一连接续传，逐字节一致 | 100.230 ms |
| 10 秒 | 同一连接续传，逐字节一致 | 100.191 ms |

接收端测试在独立 network namespace 中运行，仅打开 loopback；临时 staging/NAS 目录均在测试目录下，没有向现场 NAS 写测试数据。

- 半包续读断言通过：包头和负载各停顿 2.5 秒后仍收到一个完整包，管理 API 保持响应。
- `receiver_staging_pipeline_integration` 通过，使用实际 FFmpeg/ffprobe 验证测试录制、收尾及发布路径，不是因缺少 FFmpeg 跳过。
- 首次 `receiver_hardening_integration` 在畸形深度包断言失败，未将该次运行记为通过。测试未等待前序录制收尾与计划起点，且将两个错误包放进同一路录制队列，第二个包可能受第一个错误的队列清理影响。
- 对该测试补充等待实际录制起点，并把两个畸形格式分配给独立测试相机，分别要求产生写入拒绝计数。仅调整测试，不再次修改或重启线上 C++ 程序。
- 修正后 `receiver_hardening_integration` 通过，耗时 34.94 秒；`receiver_staging_pipeline_integration` 最近一次通过耗时 9.69 秒。两项为分别执行后的结果，不是宣称本次运行了全部仓库测试。
- `python3 -m py_compile 10_tests/test_receiver_hardening.py` 与 `git diff --check` 通过。

## 上线后观察

16:35 至 16:36，连续约 60 秒，按接收端包计数增量计算：

| 相机 | 实际 RGB 接收 packets/s | 实际 Depth 接收 packets/s |
| --- | --- | --- |
| rk3588-ubuntu_cam01 | 29.98 | 30.06 |
| orangepi5pro-ab748372_cam01 | 30.01 | 30.05 |
| orangepi5pro-b439137c_cam02 | 29.98 | 30.05 |
| orangepi5pro-fe0f7222_cam01 | 30.01 | 30.05 |
| lubancat-e8cc0cb3_cam01 | 30.43 | 30.03 |
| lubancat-52d2ef0c_cam01 | 32.50 | 31.85 |

重连后存在积压释放，接收 packets/s 暂时高于 30 不代表提高了相机采集档位。该表是传输到达统计，不是正式文件完整性验收。

- 六路均收到 RGB 与 Depth。
- 本机每 10 秒采样的 RGB、Depth receive age 最大均约 30 ms。
- 六路发送失败计数在观察窗口内没有增长。
- 队列没有积压，收尾失败计数为 0。
- 录制始终保持 `idle`。未借上线操作自动开始录制。

补充：16:42 最后复核时，现场已于 16:37:07 开始新一轮六路录制。该开始操作不是本次部署或隔离测试发起的。本次仅继续只读检查：六路 `segment_active=true`，录制队列为 0，写入错误和收尾失败均为 0，`recording_faulted=false`。以上 `idle` 描述仅指 16:35 至 16:36 的观察窗口；发现新录制后没有再重启或更改配置。

## 回退方法

先停止录制并等待收尾，再在对应设备执行。配置没有改动，无需覆盖配置。

本机：

```bash
sudo systemctl stop gwv3-gemini-sender.service
install -m 755 /home/linaro/.local/state/gwv3/rollout-backups/clock-backpressure-20260908/gemini_sender.before /home/linaro/wvd-reorg-20260902/12_build/bin/gemini_sender.rollback
mv -T /home/linaro/wvd-reorg-20260902/12_build/bin/gemini_sender.rollback /home/linaro/wvd-reorg-20260902/12_build/bin/gemini_sender
sudo systemctl start gwv3-gemini-sender.service
```

接收端，以原服务用户执行：

```bash
systemctl --user stop gwv3-gemini-receiver.service
install -m 755 /home/loop/wvd-rollout-backups/clock-backpressure-20260908/gemini_receiver.before /home/loop/wireless_video_delivery-field-v2026.09.07.1/12_build/bin/gemini_receiver.rollback
mv -T /home/loop/wireless_video_delivery-field-v2026.09.07.1/12_build/bin/gemini_receiver.rollback /home/loop/wireless_video_delivery-field-v2026.09.07.1/12_build/bin/gemini_receiver
systemctl --user start gwv3-gemini-receiver.service
```

回退后必须再次检查运行版本、双流接收和录制状态；旧版本也会重新带回本次修复的断流风险。

## 未验证与限制

- 本次没有进行新的真实相机长时间录制，也没有对现场 AP 故意注入断网。
- 测试通过不能保证无限回压、队列耗尽、真实 TCP 断开或设备断电时不丢帧。
- 原有 14:20 缺录无法靠本次上线恢复。NAS 的 `ready=true` 仍只表示完成交付，不能覆盖 `partial` 质量状态。
- 其他五个发送端仍需按各自架构、SDK 和现场登录路径分别安排升级，不得称为“全设备已经部署”。
- 相机校准参数读取告警不在本次修改范围，不能将本次传输恢复当作内外参已验证。
