# LubanCat 0282f88a 画质修复实施记录

设备为发送端 `192.168.1.91` / `lubancat-0282f88a_cam02`，接收端 `192.168.1.196`。截至 2026-09-24，接收端完整亮度范围标记和发送端 10 ms 自动曝光上限均已部署；MPP 二进制和插件沿用另一组已经部署的现场版本，本任务没有替换它们。

## 已部署配置

接收端 `/etc/gwv3/receiver.json` 的 `rgb_h264_full_range_camera_keys` 新增 `lubancat-0282f88a_cam02`，保留原有 `orangepi5pro-d12a4719_cam01`，其余配置值不变。修改前确认录制、尾帧收集、分段收尾和交付队列均为空；原子替换配置后重启 `gwv3-gemini-receiver.service`。API `/api/config` 已读回两个相机键，目标相机 `/api/status` 的 `rgb_h264_full_range=true`。

- 接收端原配置 SHA256：`4a4c99db3ea6ce01db08b91fd00733691378587a0f705af152ddc3a68ce38d3c`。
- 接收端新配置 SHA256：`147eb4a4e0eec5a76fd2e302787e39e3f8fa3b81a982e3b653f9fa6050baf2bc`。
- 接收端备份：`/var/backups/gwv3-quality-20260924/receiver.before.json`。
- 仓库现场快照：[receiver.json](../06_configs/deployment/lubancat-0282f88a-quality-20260924/receiver.json)。迁移时必须按相机键合并，不能整份覆盖其他现场。

发送端 `/etc/gwv3/sender.json` 保留 `auto_exposure=true`，为 `cam02` 新增 `color_controls.max_exposure=100`。Gemini 305 固件 1.0.70、OrbbecSDK 2.8.6 标准 `OB_SENSOR_COLOR` 路径的 API 单位为 100 微秒，因此该值对应 10 ms。独立属性读回和后续逐帧元数据均为 100；5 分钟录像各采样窗的曝光元数据保持 100，自动曝光保持开启，增益随场景变化。

- 发送端原配置 SHA256：`03e6681b39dd7b3b88f073adfbc93754546669c81d2148f249e4cb4db1b2352d`。
- 发送端新配置 SHA256：`fef0c34f6906d530921e66de084bd2ba7a46e3817d50ab1571548b9f07649204`。
- 发送端备份：`/var/backups/gwv3-quality-20260924/sender.before.json`；现场快照：[sender.json](../06_configs/deployment/lubancat-0282f88a-quality-20260924/sender.json)。
- 配置和读回证据：`E:\chat\sender-comparison-20260924\implementation\sender-evidence\sender.before.json`、`sender.after.json`、`sender-runtime-after.json`、`sender-start-after.log`、`sender-evidence-manifest.json`。
- 单位依据：`E:\chat\sender-comparison-20260924\diagnosis\implementation\exposure-unit\Gemini305曝光单位核查.md`。

`getIntPropertyRange()` 仅换算 `max/def`，不换算 `cur/min/step`；`cur=30158, def=301` 是混合单位结构，不能据此把配置值改成 10000。项目字段 `rgb_exposure_us` 当前仍保存 Gemini 305 的原始 100 微秒刻度值，本次没有修改传输协议或 CSV 字段语义。

## 处理链与 MPP

接收端处理链为 `h264_metadata=video_full_range_flag=1:matrix_coefficients=6`，录像采用 `-copyts -c:v copy`。离线复核 `210354` 和 `205145` 两段原录像：670 帧和 2359 帧的帧/包时间、容器时长及逐帧原始 YUV 哈希不变，完整解码无错误；NAS 原件 SHA256 前后一致。三张原始 JPEG 对照 MSE 从 `142.185/129.780/128.040` 降至 `20.944/14.276/17.888`。证据为 `E:\chat\sender-comparison-20260924\diagnosis\implementation\full-bsf-validation.json`。

独占 MPP 重复关键帧对照中，原系统插件 `6070c2dd8cc8ad2db8209ffafa1d5a902b70580e537dd27e2a6780efd30b8f1a` 的 10 次请求有 8 次超过阈值；现场当前插件 `640637d13e82de03ca386fd15c03a84ed036babd1415e9bfc6d6934a76002a0d` 为 0/10 失败，全部在请求同帧产生 IDR、下一帧取得输出。当前插件由另一组部署，本任务仅验证并保留，没有用本任务的备用构建替换。

硬件测试输入为 640×480@30 双路合成 JPEG，主码率 12 Mbps，判定要求 IDR 延迟不超过 3 帧、取得输出不超过 4 帧；现场生产主码率 2 Mbps，另由下面三段真实录制覆盖。现场 sender 的 build commit 为 unknown，不能宣称当前仓库分支可复现另一组的二进制。

生产程序实际路径为 `/home/cat/wireless_video_delivery/12_build/bin/gemini_sender`，SHA256 为 `fc59625a9cda00941c4532ccd6703077986b836f275339d290441a2b64084d18`，内含 `sender_source_hash=f1a27f542274a25e`。运行进程 maps 证实实际加载 `/opt/gwv3/plugins/mpp-syncpoint-ca829e0/640637d13e82/libgstrockchipmpp.so`。证据见 `E:\chat\sender-comparison-20260924\implementation\mpp\RESULTS.md` 和 `mpp\exclusive-evidence\logs\exclusive-summary.json`、`resume-production-binary.txt`、`deployed-plugin-sha256.txt`。

收尾核查时 sender、语音、receiver、音频归档、录像上传和照片上传服务均为 active/running，`NRestarts=0`，TTS ready；配置、生产二进制和实际加载插件哈希与上文一致。receiver 二进制 `/home/loop/wireless_video_delivery-field-v2026.09.07.1/12_build/bin/gemini_receiver` 的 SHA256 为 `2a28f1c73fae10ffcd9a977cd083d6e9e7ea094a66c1a9c05685e6ba1b7bcdbb`。证据为 `E:\chat\sender-comparison-20260924\implementation\sender-post-test\post-test\health.txt` 和 `receiver-post-test\post-test\health.txt`。

## 录制与语音验证状态

两次短录制和一次 5 分钟录制均已完成并交付 NAS。5 分钟录像有效窗口为 304.04 秒，仅有一个分片；这是用户确认的验收时长，本轮不测试 900 秒跨切片。validator 对三段均以退出码 0 完成，RGB、depth、audio 全媒体解码、逐帧计数、覆盖率、连续性、PTS 映射、曝光上限和文件稳定性检查全部通过，质量阈值未放宽。

仓库保留[验收数据摘要](../06_configs/deployment/lubancat-0282f88a-quality-20260924/acceptance-summary.json)。其中 `recording_validation_passed=true`，但因下述语音确认超时，`full_plan_acceptance_passed=false`。

| 录像 | 时长 | RGB / depth 帧 | RGB / depth 最大间隔 | RGB / depth 跨度差 | audio 解码帧 | 结论 |
|---|---:|---:|---:|---:|---:|---|
| `222445` | 21.050 s | 632 / 632 | 47.251 / 49.941 ms | 1.045 ms | 1052 | 全部通过 |
| `222632` | 21.018 s | 631 / 631 | 46.430 / 46.458 ms | 0.165 ms | 1051 | 全部通过 |
| `222813` | 304.040 s | 9128 / 9128 | 47.764 / 46.587 ms | 0.427 ms | 15202 | 全部通过 |

长段音频预期/收到/解码均为 `15202/15202/15202`，无 late、duplicate、silence 或 no-input，所有包时钟同步有效；`audio.opus`、`audio_meta.json`、`audio_timing.csv` 哈希匹配。音频事件包含 13 组 wake response 和 12 组 photo cue，start/end 成对。完整结果见 `E:\chat\sender-comparison-20260924\diagnosis\implementation\validation\本次全部录像验收汇总.md`、`.json` 和 `long05m-222813\compact-final.json`。

录制期间队列曾短暂积压，不能描述为全程为零：`record_queue_peak_bytes=48429825`，采样到的 `record_queue_oldest_age_ms` 峰值 2844 ms，depth receive delay 峰值 565832 us；收尾时队列归零且 write errors 为 0。原始 RGB/depth 最大帧间隔仍不超过 48 ms，源帧缺失、重复和回退均为 0，因此该积压没有造成三段验收失败。

状态中的 `last_error="rgb encoder output lag reset"` 对应 22:24:02、首次短录制开始前的 sender 启动阶段；5 分钟录像期间 sender 日志没有再次出现该错误。画面限制仍存在：640×480 分辨率没有改变，快速转动时仍能看到运动模糊，手腕局部高亮也未完全消除。10 ms 上限和 full-range 标记改善曝光拖影及范围解释，但不承诺所有运动画面完全清晰或消除全部高亮。视觉证据为 `E:\chat\sender-comparison-20260924\implementation\motion-contact-sheet.jpg`。

- 录制控制及元数据：`E:\chat\sender-comparison-20260924\implementation\short-01.json`、`short-02.json`、`five-minute.json`。
- 最终校验：`E:\chat\sender-comparison-20260924\diagnosis\implementation\validation\本次全部录像验收汇总.md`、`.json`、`long05m-222813\compact-final.json`。

真实语音链路已经识别“你好小环”和“拍照”，并完成拍照入 NAS。5 分钟用户互动在 22:30:03–22:32:40 触发 12 组三连拍，共 36 张 JPEG，最终全部进入接收端 NAS；receiver 的重启后累计完成数为 39，另含 22:02:57 的一组三连拍。互动测试中有 3 组共 9 张因接收端从照片入队到本地暂存完成耗时约 43–104 秒（包含排队等待）而超过客户端 30 秒确认期限。同期 `sar` 显示本地虚拟盘拥塞时 await 约 41.8 ms，对照约 2.62 ms，支持本地存储拥塞方向；现有证据不能把延迟归因到某一次 `fsync`。该语音拍照确认时限问题尚未修复，不能宣称批量验收全部通过，也不能把确认超时等同于照片丢失。照片位于 `/home/loop/Desktop/nas/voice_photos/lubancat-0282f88a_cam02/2026-09-24/`，真实识别与三连拍链路证据见 `E:\chat\sender-comparison-20260924\implementation\sender-post-test\post-test\voice-live-validation.log`、`E:\chat\sender-comparison-20260924\deployment\final-evidence\voice-runtime-tail.txt` 和 `photo-check.json`。

七组 Linux 软件回归已通过：电源 sender 范围、电源按键、录制按键、LED、语音拍照、TTS 和音频混音。它们是模拟设备接口的软件回归，不能替代上述真实链路和全解码结果。

## 回退

回退前先确认目标 sender 没有录制、待开始、尾帧收集或分段收尾，并保存当前配置和服务状态。重启接收端会影响所有相机，因此还须确认所有相机及交付队列均已空闲。

1. 接收端仅从 `rgb_h264_full_range_camera_keys` 删除 `lubancat-0282f88a_cam02`，保留其他相机键；仅当当前配置与本次部署后版本完全一致时，才可恢复 `/var/backups/gwv3-quality-20260924/receiver.before.json`。重启 `gwv3-gemini-receiver.service` 后核对 `/api/config` 和 `/api/status`。
2. 发送端撤销本次 `cam02.color_controls.max_exposure` 改动，保留 `auto_exposure=true`、`zlib`、Wi-Fi 优先级 drop-in 及其他人的有效修改；仅当当前配置仍与 SHA256 `fef0c34f…` 一致时，才可恢复上述 sender 备份。仅删除 JSON 字段不保证相机已写入的上限自动恢复：需在独占相机时将实际属性恢复为部署前独立读回的 301（30.1 ms），核对读回，再启动 sender 并验证自动曝光与逐帧元数据。
3. 不回退、删除或替换生产二进制 `fc59625a…`、现场插件 `640637d1…` 或其服务插件路径；这些属于另一组部署，不是本任务的回退范围。
4. 历史录像和 JPEG 均为验收证据，不删除、不覆盖。回退后重新做一次 sender 范围短录制，确认只影响目标相机且没有遗留交付任务。
