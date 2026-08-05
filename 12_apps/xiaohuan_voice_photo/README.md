# 语音关键词唤醒实验

目标：当麦克风听到“你好小环”时，通过音箱播放“我在”。

当前硬件默认使用：

```text
record_device: 优先按 USB PnP Sound Device 自动解析，找不到时等待热插拔恢复
playback_device: 优先按 USB2.0 Device 自动解析，找不到时等待热插拔恢复
sample_rate: 16000
```

模型文件不随仓库提交。正式运行前需要把 Vosk 中文模型放到：

```text
models/vosk-model-small-cn-0.22/
```

动态文本播报默认使用 Edge TTS `zh-CN-XiaoyiNeural`，需要 `ffmpeg` 和
`edge-tts`：

```bash
sudo apt-get install -y ffmpeg
python3 -m pip install --user edge-tts
```

## 1. 通用离线 ASR 方案

当前优先使用 Vosk 中文小模型做离线识别，不需要录“你好小环”关键词模板，也不会把持续监听音频保存成 WAV。

监听流程是“实时语音前端 + WebRTC VAD + 动态音量门控 + 唤醒词小语法”：

- 先用 ffmpeg 实时语音前端做高通、低通和 FFT 降噪。
- 裸 PCM 输入关闭 ffmpeg 默认探测缓冲，缩短 USB 音频重建后的首帧等待。
- 先按当前环境噪声自动校准音量阈值，运行中继续慢速更新噪声底。
- 优先使用 `webrtcvad` 判断人声；如果依赖不可用，自动回退到 RMS 音量门控。
- 片段识别时只使用 `你好 小环 / 您好 小环 / 你好 / 您好 / 小环 / [unk]` 小语法，不跑完整中文听写搜索。
- 支持分段命中：如果 `你好/您好` 和 `小环` 被切成相邻片段，只要在短时间窗口内出现，也算唤醒。
- 低质量片段会先跳过，不再全部送进 Vosk。
- 检测到唤醒后把固定回复放入统一播放队列。
- 当前硬件没有播放参考信号可供 AEC 使用。通用默认 `capture_playback_mode=keep` 会在播报期间保持采集，识别 PCM 由 drain 丢弃；133 的 USB 麦克风和音箱位于同一个不稳定的单 TT Hub，因此正式 drop-in 使用 `restart` 半双工模式：播放前释放麦克风端点，播放后快速重开、沿用已有噪声阈值且不重复 1.5 秒标定。
- 拍照小语法覆盖“拍照 / 拍张照 / 拍一张照片 / 帮我拍照 / 照相”等说法，并兼容 Vosk 常见的“拍 [unk] / [unk] 照 / 牌照”结果。针对部分说话人把 `pai zhao` 说成 `pai zao` 的情况，命令窗口还会识别“拍/排/牌/派 + 照/早/造/澡/灶/遭”的近音组合；该兼容不会作用于常驻唤醒阶段。
- 唤醒回复结束后等待用户开口 8 秒；一旦开口，连续静音 0.6 秒判定说完，单句最长 60 秒。命令音频保留约 0.2 秒前缓冲和 0.3 秒后缓冲。
- 拍照命令会先播放“叮”再执行三连拍；其他内容或本地识别为空时先播放“登”，再把完整 WAV 异步发送给下游。

启动前台监听：

```bash
./start_wake_no_record.sh
```

长期后台运行：

```bash
./start_wake_background.sh
tail -f wake_runtime.log
./stop_wake_background.sh
```

`run_wake_service.sh` 会在 `wake_runtime.log` 超过 5MB 时自动轮转，默认保留最近 5 份历史日志。

检测到“你好小环 / 您好小环”时，播放 `response_wozai_tts_default.wav`。

当前硬件映射：

```text
录音: 默认匹配 USB PnP Sound Device，并解析为稳定的 plughw:CARD=... 名称；可用 XIAOHUAN_RECORD_DEVICE 或 XIAOHUAN_RECORD_CARD_MATCH 覆盖
播放: 默认匹配 USB2.0 Device，并解析为稳定的 plughw:CARD=... 名称；可用 XIAOHUAN_PLAYBACK_DEVICE 或 XIAOHUAN_PLAYBACK_CARD_MATCH 覆盖
```

热插拔策略：

```text
启动等待: run_wake_service.sh 默认等待目标录音/播放设备最多 300 秒
等待间隔: XIAOHUAN_DEVICE_WAIT_INTERVAL_SECONDS，默认 2 秒
等待超时: XIAOHUAN_DEVICE_WAIT_SECONDS，默认 300 秒
旧卡号回退: 默认关闭，只有设置 XIAOHUAN_ALLOW_DEVICE_FALLBACK=1 才回退 plughw 默认值
运行中恢复: 读取超时、短读或设备断开后，先在原进程内重建采集管道，默认重试 20 秒
最终降级: 20 秒仍无法恢复或连续 12 秒录音 RMS <= 0.0005 时退出，由 systemd 重新解析声卡名称
```

`install_wake_service.sh` 还会安装
`90-xiaohuan-usb-audio-exclusive.rules`。该 udev 规则让当前 USB 麦克风和 USB
音箱由本程序通过 ALSA 独占，禁止 PulseAudio 在 Hub 重枚举后自动探测并短时占用
PCM 设备。规则不影响板载声卡或其他 USB 音频型号。

当前关键默认参数：

```text
audio_filter: voice
webrtc_vad_mode: 2
webrtc_voice_ratio: 0.55
webrtc_voice_energy_margin: 0.003
decode_min_rms: 0.024
decode_noise_margin: 0.008
split_wake_window_seconds: 2.4
noise_update_alpha: 0.03
playback_ignore_seconds: 0.3
echo_tail_seconds: 0.03
tts_http_port: 18082
tts_backend: edge
tts_edge_voice: zh-CN-XiaoyiNeural
tts_edge_timeout_seconds: 4.0
tts_edge_cache_dir: ~/.cache/xiaohuan/edge_tts
tts_edge_cache_max_mb: 256
tts_queue_capacity: 100
tts_max_text_chars: 500
tts_speaker_retry_seconds: 15.0
tts_resume_delay_seconds: 0.2
utterance_forward_url: http://192.168.66.113:50020/api/audio
utterance_forward_queue: 8
utterance_forward_retries: 3
command_start_timeout_seconds: 8.0
command_end_silence_seconds: 0.6
command_max_speech_seconds: 60.0
command_pre_roll_seconds: 0.2
command_tail_seconds: 0.3
audio_read_timeout_seconds: 2.0
audio_recovery_seconds: 20.0
audio_recovery_interval_seconds: 0.5
capture_playback_mode: keep（通用默认）；restart（133 正式配置）
zero_audio_rms: 0.0005
zero_audio_restart_seconds: 12.0
barge_in: false
barge_in_ignore_seconds: 0.45
barge_in_min_rms: 0.035
barge_in_noise_margin: 0.020
barge_in_noise_scale: 4.0
barge_in_voice_ratio: 0.70
barge_in_min_chunks: 3
```

长期服务安装：

```bash
./install_wake_service.sh
systemctl --user status xiaohuan-wake.service
tail -f wake_runtime.log
```

更多 KWS 候选后端、AEC/NS/AGC 条件说明见 `KWS_AND_AUDIO_FRONTEND.md`。

为降低无人说唤醒词时的误触发，正式服务使用严格唤醒模式：

- 只接受完整、顺序正确且没有额外词的“你好小环 / 您好小环”；
- 默认关闭“你好”和“小环”的跨片段拼接；
- 没有正常静音结尾或超过 3.2 秒的连续声音不参与唤醒；
- 启动时按声卡名称解析录音设备，将 USB 麦克风固定为 62% 捕获增益并关闭硬件 AGC。

这些限制只作用于常驻唤醒阶段，不改变唤醒后的拍照近音匹配。可通过
`XIAOHUAN_MIC_CAPTURE_LEVEL` 和 `XIAOHUAN_MIC_AGC` 覆盖麦克风参数。
`XIAOHUAN_PCM_PLAYBACK_LEVEL` 可选覆盖音箱 `PCM` 音量；留空时不改动原有音量。

133 换用同一张声卡同时录放音的 `SM15 M1 USB audio` 时，将
`systemd/xiaohuan-wake-sm15-m1.conf` 安装到
`~/.config/systemd/user/xiaohuan-wake.service.d/sm15-m1.conf`，再执行
`systemctl --user daemon-reload` 和 `systemctl --user restart xiaohuan-wake.service`。
该配置禁用低电平重启看门狗，避免声卡 AEC 输出数字静音时被误判为断流；
`audio_read_timeout_seconds` 仍用于检测真实读取超时。

## 2. sherpa-onnx KWS 方案

已下载 sherpa-onnx 中文 KWS 模型，并提供候选脚本：

```bash
./start_sherpa_kws_no_record.sh
```

当前测试音频中，sherpa KWS 对自定义词“你好小环”可以检出；默认运行仍使用 Vosk，因为 Vosk 方案已经经过更长时间实机运行。

## 3. 模板匹配方案

`wake_word.py` 是最早的模板匹配硬件验证原型，需要录关键词模板。它只用于验证麦克风和音箱链路，不作为正式方案；后续不要再走这条路径。

## 4. 固定回复与动态 TTS

唤醒固定回复 `response_wozai_tts_default.wav` 使用 Edge TTS
`zh-CN-XiaoyiNeural` 提前生成并裁除首尾静音。拍照和普通语音分别使用本地短提示音
`cue_photo_ding.wav` 与 `cue_forward_deng.wav`，触发时均不依赖网络。历史文件
`response_photo_done.wav` 只为旧流程兼容保留，正式流程不再播放“好的，已拍照”。

`192.168.66.133` 上的外部动态文本默认使用 Edge TTS
`zh-CN-XiaoyiNeural`。首次遇到的文本需要联网合成；成功结果写入
`~/.cache/xiaohuan/edge_tts`，相同文本在进程重启后仍可复用。磁盘缓存上限为
256 MB，超出后按最久未使用顺序清理。

Edge 请求失败或 4 秒超时时，该条动态播报失败并写日志，不切换到 eSpeak，从而保证
所有成功播放的语音音色一致。失败后进入 30 秒退避期，避免连续请求反复等待网络超时。
HTTP 接口已经返回的 `accepted` 仅表示任务入队，不表示合成成功。Edge 模式会把待播
文本发送给云端服务。

代码仍保留手工诊断用的 `espeak` 和 `sherpa` 后端，但 133 的正式配置不得启用它们，
否则不能保证统一音色。

## 5. 唤醒后拍照

当前语音服务在检测到“你好小环 / 您好小环”后，会完整播放预生成的固定回复“我在”。
133 播放前释放麦克风 USB 端点，播完等待 30ms 防回声尾窗，再快速重开麦克风并沿用
已有噪声标定。设备等待用户开口 8 秒；开始说话后不再受 8 秒窗口限制，而是录到
0.6 秒尾静音或 60 秒上限。识别到拍照指令后先完整播放“叮”，再向 RGBD 发送端写入
原有三连拍拍照请求。其他语音播放“登”后进入 HTTP WAV 后台发送队列。

正式运行时，语音服务不直接打开 Orbbec 相机，避免和视频发送端抢 SDK 设备。发送端取得下一帧完整的相机 MJPEG 后，经独立可靠 TCP 通道发送给接收端。方向正常时保留原始 JPEG 有效字节；配置 RGB 软件旋转 180 度时，在独立快照线程中校正方向，不阻塞采集线程。接收端先把 JPG 和完整性清单原子写入本地 staging；本地 `fsync` 成功后返回 `captured`。语音服务默认一次写入同一 `burst_id`、带目标采集时间的 3 个请求，目标间隔 0.2 秒，并在后台收集三张回执。即时语音表示“拍照命令已受理”，不表示三张已经持久化；最终结果以 `captured` 日志和 NAS 文件为准。连续触发的三连拍任务会串行执行，回执使用 30 秒总等待窗口，避免并发挤满接收端快照队列。NAS 写入由独立上传服务异步完成，NAS 短时不可用不会阻塞语音反馈，也不影响 RGBD 录制链路。

默认请求目录：

```text
/tmp/gemini_rgb_snapshot_requests
```

默认结果目录：

```text
/tmp/gemini_rgb_snapshot_results
```

接收端本地 staging：

```text
/home/fz/recording_staging/.gwv3_photo_queue/<request_id>/
```

最终 NAS 目录只发布 JPG，不写旁路 JSON：

```text
/home/fz/Desktop/nas/voice_photos/<sender_id>_<camera_id>/YYYY-MM-DD/HH-MM-SS/YYYYMMDD_HHMMSS.jpg
```

一次三连拍以第一张实际选中 RGB 帧的 `frame_system_timestamp_us` 确定目录和文件名前缀，三张使用 `.jpg`、`_001.jpg`、`_002.jpg`；后两张跨秒时仍保存在该目录。目标秒目录已被另一轮拍照占用时，整组改用 `HH-MM-SS_001` 目录。目录时间不是语音识别完成时间或 NAS 上传时间。

相关默认参数：

```text
post_wake_command_seconds: 8.0
photo_request_dir: /tmp/gemini_rgb_snapshot_requests
photo_result_dir: /tmp/gemini_rgb_snapshot_results
photo_result_timeout_seconds: 30.0
photo_result_retention_seconds: 86400.0
photo_burst_count: 3
photo_burst_interval_seconds: 0.2
photo_camera_id: cam01
photo_cue_wav: cue_photo_ding.wav
forward_cue_wav: cue_forward_deng.wav
photo_decode_min_seconds: 0.45
photo_decode_min_rms: 0.016
command_end_silence_seconds: 0.6
command_max_speech_seconds: 60.0
```

## 6. 局域网文本播报

`192.168.66.133` 默认监听：

```text
POST http://192.168.66.133:18082/api/tts/speak
```

请求必须包含 `request_id` 和 `text`。重复 `request_id` 不会重复播放。接口返回
`accepted` 只表示已经入队。唤醒后的用户会话进行期间，远程文本只排队不插播；固定
回复和“叮/登”本地提示音优先于远程文本。完整协议、错误码和调用示例见
`TTS_HTTP_API.md`。

## 7. 唤醒后整句 WAV 发送

133 正式配置不再持续发送 RTP/Opus。只有一次唤醒会话中的非拍照语音会发送到：

```text
POST http://192.168.66.113:50020/api/audio
Content-Type: audio/wav
Body: 16000Hz / mono / PCM signed 16-bit WAV
```

可用环境变量：

```text
XIAOHUAN_AUDIO_STREAM_ENABLED=0
XIAOHUAN_UTTERANCE_FORWARD_ENABLED=1
XIAOHUAN_UTTERANCE_FORWARD_URL=http://192.168.66.113:50020/api/audio
XIAOHUAN_UTTERANCE_FORWARD_QUEUE=8
XIAOHUAN_UTTERANCE_FORWARD_TIMEOUT_SECONDS=10
XIAOHUAN_UTTERANCE_FORWARD_RETRIES=3
XIAOHUAN_UTTERANCE_FORWARD_RETRY_DELAY_SECONDS=0.5
```

HTTP 请求体不附带 JSON 或 multipart 元数据。任意 `2xx` 响应视为成功；失败最多重试
3 次。队列满时丢弃最旧语音，上传成功后发送端不保留本地 WAV。参考 macOS 接收脚本
为 `05_tools/xiaohuan_audio_receiver.py`，完整接口见 `UTTERANCE_AUDIO_HTTP_API.md`。

旧 RTP/Opus 参数和实现仅保留用于兼容诊断，133 的正式 systemd profile
`systemd/xiaohuan-wake-utterance-forward.conf` 明确关闭该链路。

## 8. 文件识别测试

```bash
python3 vosk_wake.py file debug_nihao_xiaohuan_live.wav
python3 sherpa_kws.py file debug_nihao_xiaohuan_live.wav
```
