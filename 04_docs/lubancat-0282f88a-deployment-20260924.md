# LubanCat 0282f88a 部署快照（2026-09-24）

本快照记录发送端 `192.168.1.91` 与接收端 `192.168.1.196` 已验证的语音、按键和录像音频部署状态。发送端为 LubanCat RK3576，有效身份为 `lubancat-0282f88a`；相机为 Orbbec Gemini 305，`camera_id=cam02`，序列号 `CV275610004S`。接收端 Web API 使用 TCP 8080。

快照配置位于 [`06_configs/deployment/lubancat-0282f88a-20260924/`](../06_configs/deployment/lubancat-0282f88a-20260924/)。它们用于核对和恢复本设备的增量配置，不是可覆盖任意现场的完整环境镜像。仓库保留源码、配置和本文的验收摘要，不包含现场录音、照片或设备日志。

## 配置文件与落点

以下路径相对快照目录；`~` 指对应设备上实际运行用户服务的用户目录。

| 快照文件 | 安装位置或用途 |
|---|---|
| `sender/sender.json` | 发送端实际相机配置，现场为 `/etc/gwv3/sender.json`；恢复前核对相机序列号、接收地址与现有运行参数 |
| `sender/device.conf` | `~/.config/systemd/user/xiaohuan-wake.service.d/device.conf`；拍照身份、TTS 与设备级默认值 |
| `sender/sm15-m1.conf` | 同目录；SM15 M1 同卡录放音及数字静音处理 |
| `sender/zz-audio-archive.conf` | 同目录；最终生效的 RTP/Opus 与归档参数 |
| `sender/power-key-inhibit.conf` | `/etc/systemd/system/gwv3-power-button.service.d/power-key-inhibit.conf`；接管电源键，并启动电源控制程序 |
| `receiver/receiver.json` | 接收主服务配置，现场为 `/etc/gwv3/receiver.json`；本次关键增量为 `task_audio.sender_ids` |
| `receiver/audio_archive_receiver.json` | 音频归档配置，现场为 `/etc/gwv3/audio_archive_receiver.json`；本次关键增量为 `streams` 中的新 sender |

录制键与电源键的设备配置分别为 [`config_lubancat-0282f88a.json`](../12_apps/recording_buttons/config_lubancat-0282f88a.json) 和 [`config_lubancat-0282f88a_power.json`](../12_apps/recording_buttons/config_lubancat-0282f88a_power.json)。安装器将选定配置生成本地 `config_lubancat-local*.json`。设备快照中的 `sender_id=auto` 若保留，仍应核对运行态身份实际为 `lubancat-0282f88a`，不能据此把设备身份复制到另一块主板。

## 相机、按键与启动行为

相机原生自动曝光已开启：`color_controls.auto_exposure=true`，SDK 写入后回读为 true，连续 60 条实际帧元数据的 `rgb_ae` 均为 1，并观察到曝光或增益变化。软件 `adaptive_exposure` 保持关闭；它与原生 AE 是互斥策略。本次没有改采集档位或另行启用软件曝光闭环。

| 控件 | 最终行为 |
|---|---|
| RECOVERY，SARADC channel 1 | 长按 1 秒，开始本 sender 当前在线相机录制 |
| MASKROM，SARADC channel 0 | 长按 1 秒，停止本 sender 录制 |
| RK805 电源键 | 长按 5 秒，先停止本 sender 录制，等待关机提示音，再执行系统关机 |
| 录制 LED | `gpiochip0`、line 24（GPIO0_D0）、低电平有效；空闲常亮，录制时每 500 ms 翻转 |

录制键调用 sender 级 `start-sender` / `stop-sender`。电源配置也明确包含 `sender_id=lubancat-0282f88a`；目标缺失或状态过期时不会退回全局停止。为兼容原有安装，只有未配置 sender_id 的旧电源配置仍保留 stop-all 行为。

电源服务新增 `shutdown_audio_wait_seconds`，默认 30 秒，配置值必须为有限正数。提示音 HTTP 持续返回 `queued` 时，独立单调时钟总期限到达后继续关机；单次 HTTP 超时和轮询间隔按剩余时间收缩。HTTP 不可用仍按原 `shutdown_audio_failure_seconds=3` 处理，正常排队超过 3 秒仍允许等到播放完成。新字段放在 `ServiceConfig` 末尾，已有构造和未新增该字段的配置仍可使用默认值。

语音服务的 systemd 模板使用 `WorkingDirectory=@APP_DIR@`，渲染为实际绝对路径时不加值外层引号；本设备 Ubuntu 22.04 / systemd 249 曾将带引号的值判为非绝对路径。`ExecStart` 保留合法的命令路径引号。Linux 安装与启动入口使用 LF 换行并具有执行权限，安装后校验渲染出的 unit。

录制键运行于用户服务；电源与 LED 运行于系统服务。电源服务以 root 运行，允许写入其 `/run/gwv3-recording-buttons` 运行目录；LED 的设备白名单允许 `/dev/gpiochip0`，使用 gpiod v1 Python 接口。语音和相机 sender 共用 `/tmp/gemini_rgb_snapshot_requests`、`/tmp/gemini_rgb_snapshot_results`；恢复时不能把这两个进程的拍照目录隔离到各自的 PrivateTmp。

## 语音模型与依赖

使用官方中文模型 `vosk-model-small-cn-0.22`，下载来源为 [Alpha Cephei 官方模型地址](https://alphacephei.com/vosk/models/vosk-model-small-cn-0.22.zip)。本次官方压缩包共 43,898,754 字节，ZIP CRC 验证通过；实际计算的 SHA-256 与仓库 [`asset-manifest.json`](../06_configs/deployment/asset-manifest.json) 一致：

```text
3af8b0e7e0f835ae9d414ce5df580237a3cfb08d586c9fbbb0f7ff29ad5b14ba
```

模型解压位置为 `<部署目录>/12_apps/xiaohuan_voice_photo/models/vosk-model-small-cn-0.22/`，其中应存在 `am/final.mdl`。恢复时从官方来源或已验证的离线缓存安装，模型压缩包与运行模型不作为本次配置快照提交。

本次实机安装并通过依赖检查的直接 Python 依赖为：

| 包 | 版本 |
|---|---|
| numpy | 1.26.4 |
| vosk | 0.3.45 |
| webrtcvad-wheels | 2.0.14（导入名为 `webrtcvad`） |
| edge-tts | 7.2.8 |

还需要设备上的 ALSA 工具、ffmpeg，以及 RTP/Opus 路径使用的 GStreamer 工具和插件。安装到实际用户服务可见的 Python 环境，并检查架构、Python 版本及依赖解析结果。动态 TTS 使用 Edge 后端；固定唤醒回复和控制提示音使用本地音频资源。整句 WAV HTTP 转发保持关闭，本次不部署语音问答后端。

SM15 M1 的录音与播放均按 `SM15 M1 USB audio` 声卡名称解析，避免依赖易变化的数字卡号。SM15 配置将 Mic/PCM 设置为 100%，关闭硬件 AGC，并将 `XIAOHUAN_ZERO_AUDIO_RESTART_SECONDS=0`，避免声卡 AEC 输出数字静音时误触发重建；真实读取超时仍走恢复逻辑。

## 音频归档与覆盖顺序

语音用户服务的三个 drop-in 按文件名字典序加载：`device.conf` → `sm15-m1.conf` → `zz-audio-archive.conf`。相同 Environment 变量由后加载的赋值覆盖。`device.conf` 中保留的音频默认关闭值会由最后一个文件开启；只复制前两份文件无法恢复录像音频。恢复时检查全部现存 drop-in 和 `systemctl --user cat/show` 的有效配置，避免其他更晚加载的文件再次覆盖。

最终音频参数如下：

| 项目 | 值 |
|---|---|
| sender_id | `lubancat-0282f88a` |
| 归档 RTP 目标 | `192.168.1.196:50034/UDP` |
| SSRC | `42137738` |
| Payload type / Opus 比特率 | `111` / `32000 bit/s` |
| 采样率 / 声道 | `48000 Hz` / 单声道 |
| timing 目标 | 接收端 UDP 50130 |
| 控制端口 | 发送端 UDP 50131 |
| 主 RTP 目标 | `127.0.0.1:50020/UDP` |
| 播音期间采集 | `PAUSE_DURING_PLAYBACK=0`、`CONTINUOUS_LISTEN_DURING_PLAYBACK=1`、`CAPTURE_PLAYBACK_MODE=keep` |

`XIAOHUAN_AUDIO_STREAM_ENABLED` 与 `XIAOHUAN_AUDIO_ARCHIVE_STREAM_ENABLED` 均需为 1。主 RTP 分支留在本机回环，归档分支发往接收端；程序分别发送两份，不做目标地址去重，因此不能把两者都设成 `.196:50034`。回环 UDP 50020 也不是整句 WAV 的 HTTP 接收服务。

**接收端两份配置必须同时核对并合并：**

1. 在实际音频归档配置的 `streams` 中按 sender_id 合并本设备的 port、SSRC、PT、sample_rate、control_port，保留其他流并检查 sender_id 和端口唯一性。
2. 在实际接收主服务配置的 `task_audio.sender_ids` 白名单中追加本 sender，保留其他成员。仅增加 stream 不足以生成录像内的 `audio.opus`，主程序仍可能返回 `sender has no configured audio input`。

`task_audio.sender_ids` 在接收主服务启动时加载，不支持热重载；音频归档 streams 也在归档服务启动时加载。合并配置后，在所有相机已停止录制且没有待结束分段的窗口重启相应服务；若运行态白名单已包含本设备，则无需因该名单重复重启。此次新增白名单实际重启了接收主服务和音频归档服务，未重启录制上传服务。确认运行态 `/api/config` 的 `task_audio_sender_ids` 包含本设备，并检查 `/api/audio/status` 的收包、SSRC/PT 匹配与时钟锚点。

## 配置快照之外的系统设置

这些改动不会仅靠复制 JSON 自动恢复，应按对应设备当前状态逐项核对：

- 用户服务持久启动：发送端和接收端运行用户的 linger 已开启。语音、录制按钮为用户服务；电源、LED 为系统服务，不能混用安装目录和 service manager。
- GNOME 电源键动作：`org.gnome.settings-daemon.plugins.power` 的 `power-button-action` 设为 `nothing`。
- logind：安装 `/etc/systemd/logind.conf.d/90-gwv3-power-key.conf`，内容为 `[Login]` 下 `HandlePowerKey=ignore`。
- 电源键即时接管：`power-key-inhibit.conf` 先清空原 `ExecStart`，再通过 `systemd-inhibit --what=handle-power-key --mode=block` 启动电源服务，已在现场生效。恢复到其他部署目录时必须调整其中 Python 程序和配置的绝对路径。
- 原厂服务：本设备 `lbc-test.service` 已禁用，避免原厂 DDR 测试与正式按键逻辑竞争；不要将此设备专用动作推广到无关服务。
- PulseAudio：只对 `alsa_card.usb-MV-SILICON_SM15_M1_USB_audio_20190808-00` 执行 `pactl set-card-profile … off`，释放 SM15 的 ALSA 访问。没有禁用整台机器的 PulseAudio 服务；恢复时先匹配真实声卡名称，并保留其他声卡配置。
- udev：安装仓库 `90-xiaohuan-usb-audio-exclusive.rules` 到 `/etc/udev/rules.d/` 并重新加载规则，通过 `PULSE_IGNORE=1` 避免匹配的 USB 音频设备被重新探测抢占。规则加载与已有 PulseAudio profile 状态是两项独立设置。

## 恢复顺序与合并边界

先确认正在运行服务的 ExecStart、实际配置路径、部署目录、硬件身份和现有 drop-in，备份拟改文件及相关系统设置。以本快照为对照，合并本设备所需字段；不得整份覆盖另一现场的 receiver 配置、NAS/暂存目录、相机清单、白名单或音频 streams。

准备模型与依赖、渲染后的 unit、设备按键配置及三份语音 drop-in，再在设备空闲窗口启用或重启受影响的服务。设备特定的 SM15 和语音身份 drop-in 并非由 `install_wake_service.sh` 自动全部安装；恢复时要显式核对。系统配置与用户配置分别执行对应 manager 的 daemon-reload。涉及接收白名单与 streams 的改动必须按上一节同步落地和加载。

恢复后分别验证服务状态、最终 Environment、TTS 健康、有效 sender/camera 身份、接收白名单、持续收包及一段本 sender 的实际录像音频。回退同样只恢复本次触及的文件、声卡 profile 和桌面/系统设置；JSON 配置快照不包含模型安装、Python 环境或全部系统状态。

## 实机验收结果与剩余限制

2026-09-24（北京时间）现场确认语音、录制按钮、电源、LED 均 enabled、active/running，核验时 `NRestarts=0`。电源 10 个测试函数、sender scope 4 项、录制按钮、LED 3 项、photo burst、TTS、mixer 回归均通过；LED 测试采用设备实际 GPIO0 默认版本。已记录 RECOVERY/MASKROM 的 ADC 事件及本 sender `started_count=1` / `stopped_count=1`，重复开始被忽略。未执行真实关机或整机重启后的自启动验收。

真实语音链路在 20:16:49 识别“你好小环”并回复“我在”，20:16:52 识别“拍照”，随后 cam02 的三张照片成功落盘到接收端 NAS。该证据证明本次完整操作成功，不作为不同噪声条件下识别率的统计结论。

音频两端配置同步后，对本 sender 进行一次短录制，NAS 在 `lubancat-0282f88a_cam02/2026-09-24/204126/` 生成 `audio.opus`。其元数据为：

- `audio_valid=true`、`quality_status=complete`。
- `received_packets=553`、`expected_packets=553`，完整率 100%。
- `audio_duration_us=11060000`，即 11.06 秒；Opus/Ogg、单声道、48 kHz，文件 45,234 字节。
- `silence_packets=0`、`late_packets=0`、`duplicate_packets=0`、`longest_no_input_ms=0`；`silence_packets` 表示补缺包的静音填充数量，不表示现场必然有人说话。
- SSRC/PT 不匹配计数为 0，时钟同步和时间锚点有效；ffprobe 与完整 ffmpeg 解码通过。

仍有两个与本次语音和音频接通独立的运行限制：深度请求档位 `320×200` 回退到实际 `848×530`，原始像素数据量约为期望的 7 倍；视频运行仍出现 `rgb queue overwrite; waiting for next keyframe`，LED 状态 HTTP 查询偶有超时。尚未证明深度回退是全部网络积压的原因，本次没有修复采集档位或完成长时间视频/深度性能验收。11.06 秒音频完整性结果不代表这些问题已经消除。
