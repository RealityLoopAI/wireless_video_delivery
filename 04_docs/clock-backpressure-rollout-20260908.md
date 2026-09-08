# TCP 抗回压修复上线记录

日期：2026-09-08，北京时间 UTC+8。

最新状态：17:24 完成全体在线设备复核，六个 sender 与 receiver 均运行 `b031e8ee2aed`。下文 16:33 的两端上线记录保留作为历史；五台远端补齐情况及尚未通过的稳定性项目见「全体发送端补齐与复核」。上线完成不等于六路长录验收通过。

17:36 后续诊断补充：52d2ef0c 的积压在原连接上追平，17:32 至 17:34 实收 RGB/Depth 均约 30 FPS；但独立测试确认当前停止操作不会补收仍在网络中的停止前采集帧。两份午后缺录及该未修复风险详见 [录制缺帧说明第 10 节](recording-gap-recovery.md#10-52d2ef0c-午后批次复核与停止缺尾风险)。

## 范围与版本

用户确认停止录制后，本次在 16:33 完成以下两端更新：

| 设备 | 更新前运行版本 | 更新后运行版本 |
| --- | --- | --- |
| rk3588-ubuntu，本机发送端 | aca150471515 | b031e8ee2aed |
| 192.168.1.196，接收端 | aca150471515 | b031e8ee2aed |

`b031e8ee2aed219f6f9c98ce5c0a2a8efb72e744` 包含 `ccd3981` TCP 抗回压修复。先前代码虽然提交，但生产进程一直是 12:11 启动的旧程序，所以 14:20 的录制仍受旧超时断连策略影响。

16:33 这一阶段没有合入独立审查分支，没有更新其他五个发送端，也没有替用户开始生产录制。其他发送端当时自动重连到了新接收端，但其发送端保留重试能力不能视为当时已经升级。随后用户要求全部升级，执行情况见后文。

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

## 全体发送端补齐与复核

### 操作边界

用户要求「都升级吧」后，先只读检查并准备发布。最初仍有六路录制，直到 16:56:22 确认 receiver 为 `idle`、录制队列与收尾任务为 0 后才开始逐台替换。没有调用生产录制的开始或停止接口。

所有新程序来自干净提交 `b031e8ee2aed219f6f9c98ce5c0a2a8efb72e744`，在各发送端独立目录 `/home/<user>/wvd-releases/clock-backpressure-b031e8e` 本机编译，没有覆盖现场工作区的源码改动。原账号、systemd 服务名、相机身份、分辨率、旋转、曝光、白平衡、压缩参数和音频程序保持原样。

本次在线范围为六个 sender，不包含已关机或不在本次 receiver 状态列表中的历史设备。

| sender_id | 本次确认的 SSH 地址 | SDK | 最终运行版本 |
| --- | --- | --- | --- |
| rk3588-ubuntu | 本机，192.168.5.4 | 1.10.27 | b031e8ee2aed |
| orangepi5pro-ab748372 | 192.168.1.147 | 1.10.27 | b031e8ee2aed |
| orangepi5pro-b439137c | 192.168.1.125 | 1.10.27 | b031e8ee2aed |
| orangepi5pro-fe0f7222 | 192.168.5.5 | 1.10.27 | b031e8ee2aed |
| lubancat-52d2ef0c | 192.168.0.105 | 2.8.6 | b031e8ee2aed |
| lubancat-e8cc0cb3 | 192.168.0.109 | 2.8.6 | b031e8ee2aed |

表中 IP 仅为此次现场观测，不是相机身份或固定部署配置。两台 LubanCat 位于 `TP-LINK_5G_215E` 的 NAT 后，receiver 看到的来源均为 `192.168.1.159`。本机临时停发并切入该 AP 才完成 SSH 部署，保持 SSH host key 校验，没有修改路由器。17:14:39 已恢复本机原来的 `88888888` 网络与 sender；自动恢复安全计时器已关闭，未增加新的开机任务。

### 编译、启动与回退材料

三个 Orange Pi 使用 SDK 1.10.27 构建。两个 LubanCat 使用 SDK 2.8.6，运行时 SONAME 为 `libOrbbecSDK.so.2`，不能因名称不是 `.so.2.8` 就误判 SDK 版本。LubanCat 链接期间出现 vendor `libmali.so.1` 的 `.dynsym` 警告，构建仍成功，随后实际启动并使用 MPP 编码；本次没有修改 Mali 驱动或库。

五台均保留以下目录，权限为 700：

- Orange Pi：`/home/orangepi/wvd-rollout-backups/fleet-clock-backpressure-20260908`。
- LubanCat：`/home/cat/wvd-rollout-backups/fleet-clock-backpressure-20260908`。

每份包含 `gemini_sender.before`、`sender.json.before`、`sender_watchdog.sh.before` 与 `manifest.json`。manifest 记录原路径、配置哈希、旧程序哈希、新程序哈希和实际运行 PID。LubanCat 另保留现场 tracked diff，不将可能包含现场凭据的原始备份提交到 Git。

三个 Orange Pi 和 52d2ef0c 安装发布版 watchdog；e8cc0cb3 原 watchdog 含现场 Wi-Fi 恢复改动，保留该脚本，只在实际采集启动前补入有界 chrony 等待，使用其原有 `LOG_FILE` 变量。执行 `bash -n` 后才替换，未用整份新脚本覆盖现场定制。

实际运行 `/proc/<pid>/exe` SHA-256 校验：

| 设备 | SHA-256 |
| --- | --- |
| ab748372、b439137c、fe0f7222 | 2edc7f04a41b3a0cb598b7011dbd8c068d4bbf2ee3f8fa2dea544dcc928b2327 |
| 52d2ef0c | c4fc4dd3f4314386b14dd10a1a2e04cd56b1be894535a33349f36404556a7e1b |
| e8cc0cb3 | 1fc0acec1afb06870be6c875a538f3e5bc7d4def89da94d581b467d88587023b |

六路 sender 的运行 `build_source_hash` 均为 `367b292522d2194d`。不同构建目录或 SDK 的二进制哈希可以不同，不能要求 ARM 与 x86、SDK v1 与 v2 的程序逐字节相同。

需要回退时，先停止该 sender 对应录制并等待收尾，然后停止 manifest 指定的服务，将备份的二进制和 watchdog 先复制到各自旁路文件，再原子替换正式文件，启动同一服务。三个 Orange Pi 使用 `gwv3-gemini-sender.service`；LubanCat 必须使用各自 `gwv3-gemini-sender-lubancat-<id>.service`，不要误启未使用的通用服务。不要直接覆盖此后可能被现场修改的配置。

### 五台原生停读续传测试

每台运行独立 loopback 的 `transport_backpressure_test`，接收方停读 6 秒，连续两个测试包合计 8,388,796 字节。不使用生产端口或录制目录。

| 设备 | 保留重试次数 | 最大单次 send 调用 | 同一连接且逐字节一致 |
| --- | --- | --- | --- |
| b439137c | 95 | 100.312 ms | 通过 |
| ab748372 | 95 | 100.211 ms | 通过 |
| fe0f7222 | 95 | 100.286 ms | 通过 |
| 52d2ef0c | 94 | 101.502 ms | 通过 |
| e8cc0cb3 | 94 | 101.418 ms | 通过 |

这是五台实际执行的字节续传测试，不是新一轮全仓回归，也不是六路无线加 NAS 长时间录制测试。

### 全系统实收观察与未通过项

使用 receiver `GET /api/status`，每 10 秒只读采样一次，按累计 `rgb_packets`、`depth_packets` 的增量除以单调时钟间隔统计。两个窗口分别为 17:22:33 至 17:23:33，以及 17:23:53 至 17:24:53，均约 60 秒。

| 相机 | 第一窗口 RGB / Depth packets/s | 第二窗口 RGB / Depth packets/s |
| --- | --- | --- |
| rk3588-ubuntu_cam01 | 30.00 / 30.08 | 30.00 / 30.06 |
| orangepi5pro-ab748372_cam01 | 30.01 / 30.08 | 29.98 / 30.06 |
| orangepi5pro-b439137c_cam02 | 30.01 / 30.08 | 29.99 / 30.04 |
| orangepi5pro-fe0f7222_cam01 | 30.00 / 30.08 | 30.00 / 30.06 |
| lubancat-e8cc0cb3_cam01 | 29.98 / 30.01 | 29.96 / 30.00 |
| lubancat-52d2ef0c_cam01 | 24.58 / 28.18 | 22.71 / 28.93 |

共同确认：

- 六路所有采样点均为 live，且 clock model 有效；采样点中的最大绝对 offset 为 2,298 us。这不等于已经测量或保证画面内容级同步误差。
- 两窗口发送失败、Depth 丢帧、接收端 RGB 重连恢复、等待关键帧丢弃和录制写入错误计数均无增长。
- 两窗口 recording 均为 `idle`。录制队列、收尾任务、NAS pending segments/bytes 均为 0，NAS mount ready，接收端 staging 可用空间约 240.8 GB。
- 本机及三个 Orange Pi 两窗口 RGB 丢弃计数无增长，实收约 30 FPS。

不能忽略的剩余问题：

1. **52d2ef0c 尚未通过实时稳定性复核。** 采集/发送心跳约 30 FPS，但两个窗口 receiver 实收 RGB 均低于 30，第二窗口某采样点 RGB receive age 为 784 ms。receiver 日志 17:24:12 记录 `capture_to_receiver_us=18508008`，即采集到接收约 18.5 秒；Depth 同时约 4.6 秒。检查 TCP 时 receiver Recv-Q 为 0，没有录制队列阻塞，未看到该会话新增重连。这些证据指向该设备接收前链路存在积压，尚不能仅凭 receiver 数据判定是 Wi-Fi、sender 排队还是其他原因。保留续传避免主动断连，但不能增加链路带宽；缺乏发送端 socket/无线增量证据前，不声称根因已修复。
2. **e8cc0cb3 仍有少量 RGB 丢帧。** 两个窗口分别新增 2、3 帧 RGB 丢弃，`last_error` 为 `corrupt rgb mjpeg frame dropped`。52d2ef0c 也留有同类历史告警，但两窗口该计数没有增加。旧版本已存在 JPEG SOI/EOI 完整性检查，因此不能把告警文字出现本身当成新增缺陷证据；仍需采集相机原始 MJPEG、USB 与 sender 日志核对具体原因。本次没有放宽坏帧校验来掩盖问题。

17:27:29 最后补查：六路仍运行目标版本且 live，receiver 仍为 `idle`，NAS 无待交付任务。但 52d2ef0c 的 `rgb_receive_delay_us` 已达到 57,036,156（约 57 秒），Depth 当时为 35,257 us；e8cc0cb3 的累计 RGB 丢弃达到 27 帧。52d2ef0c 的 RGB 积压风险尚在扩大，不能依据 clock model 有效、在线或无 send failure 就建议直接做稳定长录。其余五路当时 RGB 采集至接收估计延迟约 12 至 60 ms。本次未通过重启清队列掩盖该问题，也未将旧缺录风险版本重新作为正式修复版。

结论：**六个发送端和接收端版本已统一、启动与续传用例通过；六路长录稳定性尚未全部验收通过。** 下一次正式验收需单独处理上述两台风险，再做真实相机短录/长录，复核最终 CSV、实际视频帧数和质量状态；不能只凭本次无发送失败就宣称不缺录。

## 未验证与限制

- 本次没有进行新的真实相机长时间录制，也没有对现场 AP 故意注入断网。
- 测试通过不能保证无限回压、队列耗尽、真实 TCP 断开或设备断电时不丢帧。
- 原有 14:20 缺录无法靠本次上线恢复。NAS 的 `ready=true` 仍只表示完成交付，不能覆盖 `partial` 质量状态。
- 本次在线六路已全部升级；未上线历史设备不在本次覆盖范围。52d2ef0c 积压与 e8cc0cb3 坏帧仍需独立诊断，见上文。
- 相机校准参数读取告警不在本次修改范围，不能将本次传输恢复当作内外参已验证。
