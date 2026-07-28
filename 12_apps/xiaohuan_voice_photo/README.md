# 语音关键词唤醒实验

目标：当麦克风听到“你好小环”时，通过音箱播放“我在，有什么可以帮到您的”。

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

## 1. 通用离线 ASR 方案

当前优先使用 Vosk 中文小模型做离线识别，不需要录“你好小环”关键词模板，也不会把持续监听音频保存成 WAV。

监听流程是“实时语音前端 + WebRTC VAD + 动态音量门控 + 唤醒词小语法”：

- 先用 ffmpeg 实时语音前端做高通、低通和 FFT 降噪。
- 先按当前环境噪声自动校准音量阈值，运行中继续慢速更新噪声底。
- 优先使用 `webrtcvad` 判断人声；如果依赖不可用，自动回退到 RMS 音量门控。
- 片段识别时只使用 `你好 小环 / 您好 小环 / 你好 / 您好 / 小环 / [unk]` 小语法，不跑完整中文听写搜索。
- 支持分段命中：如果 `你好/您好` 和 `小环` 被切成相邻片段，只要在短时间窗口内出现，也算唤醒。
- 低质量片段会先跳过，不再全部送进 Vosk。
- 检测到唤醒后播放固定回复，并在回复播放结束后的尾音窗口内忽略麦克风输入，避免自激误识别。
- 支持语义打断固定回复：播放“我在，有什么可以帮到您的”时仍监听拍照命令；只有识别到“拍照 / 拍张照 / 照相”后才停止当前回复并执行拍照，避免音箱回声按音量误触发。

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
录音: 默认匹配 USB PnP Sound Device，可用 XIAOHUAN_RECORD_DEVICE 或 XIAOHUAN_RECORD_CARD_MATCH 覆盖
播放: 默认匹配 USB2.0 Device，可用 XIAOHUAN_PLAYBACK_DEVICE 或 XIAOHUAN_PLAYBACK_CARD_MATCH 覆盖
```

热插拔策略：

```text
启动等待: run_wake_service.sh 默认等待目标录音/播放设备最多 300 秒
等待间隔: XIAOHUAN_DEVICE_WAIT_INTERVAL_SECONDS，默认 2 秒
等待超时: XIAOHUAN_DEVICE_WAIT_SECONDS，默认 300 秒
旧卡号回退: 默认关闭，只有设置 XIAOHUAN_ALLOW_DEVICE_FALLBACK=1 才回退 plughw 默认值
运行中恢复: 连续 12 秒录音 RMS <= 0.0005 时退出，交给 systemd Restart=always 自动重启重绑设备
```

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
echo_tail_seconds: 0.3
zero_audio_rms: 0.0005
zero_audio_restart_seconds: 12.0
barge_in: true
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

## 2. sherpa-onnx KWS 方案

已下载 sherpa-onnx 中文 KWS 模型，并提供候选脚本：

```bash
./start_sherpa_kws_no_record.sh
```

当前测试音频中，sherpa KWS 对自定义词“你好小环”可以检出；默认运行仍使用 Vosk，因为 Vosk 方案已经经过更长时间实机运行。

## 3. 模板匹配方案

`wake_word.py` 是最早的模板匹配硬件验证原型，需要录关键词模板。它只用于验证麦克风和音箱链路，不作为正式方案；后续不要再走这条路径。

## 4. 固定回复

当前固定回复使用 `response_wozai_tts_default.wav`，内容为“我在，有什么可以帮到您的”。该文件由 `edge-tts` 生成，当前音量为轻提示音量，实测能听到。

如需真正 TTS，可以后续接入本地中文 TTS 或在线 TTS 生成“我在”。

## 5. 唤醒后拍照

当前语音服务在检测到“你好小环 / 您好小环”后，会先播放固定回复“我在，有什么可以帮到您的”。固定回复播放期间也会监听“拍照 / 拍张照 / 照相”；如果识别到拍照命令，会停止当前固定回复并向 RGBD 发送端写入本地拍照请求。固定回复自然播放结束后也会等待约 0.3 秒尾音，并默认开启 8 秒命令窗口。

正式运行时，语音服务不直接打开 Orbbec 相机，避免和视频发送端抢 SDK 设备。发送端取得下一帧完整的相机 MJPEG 后，经独立可靠 TCP 通道发送给接收端。方向正常时保留原始 JPEG 有效字节；配置 RGB 软件旋转 180 度时，在独立快照线程中校正方向，不阻塞采集线程。接收端先把 JPG 和完整性清单原子写入本地 staging；本地 `fsync` 成功后返回 `captured`。语音服务默认一次写入同一 `burst_id`、带目标采集时间的 3 个请求，目标间隔 0.2 秒，三张都确认后播放“好的，已拍照”。NAS 写入由独立上传服务异步完成，NAS 短时不可用不会阻塞拍照确认，也不影响 RGBD 录制链路。

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
photo_result_timeout_seconds: 5.0
photo_result_retention_seconds: 86400.0
photo_burst_count: 3
photo_burst_interval_seconds: 0.2
photo_camera_id: cam01
photo_response_wav: response_photo_done.wav
```

## 6. 文件识别测试

```bash
python3 vosk_wake.py file debug_nihao_xiaohuan_live.wav
python3 sherpa_kws.py file debug_nihao_xiaohuan_live.wav
```
