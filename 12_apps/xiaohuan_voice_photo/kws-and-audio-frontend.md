# KWS And Audio Frontend

本文说明关键词唤醒的默认实现、音频前端边界和可选后端。应用总览见 [README.md](README.md)。

## Default Pipeline

正式默认后端为 `vosk_wake.py`：

```text
ALSA capture
  -> ffmpeg high-pass / low-pass / FFT denoise
  -> WebRTC VAD (RMS fallback)
  -> dynamic noise gate
  -> constrained Vosk grammar
  -> wake/session state machine
```

主要规则：

- 采样率为 16 kHz、单声道、PCM signed 16-bit。
- 默认麦克风捕获增益为 62%，硬件 AGC 关闭；设备 profile 可覆盖，例如带硬件 AEC 的声卡可使用不同 mixer 值。
- 只有完整、顺序正确且没有额外词的“你好小环 / 您好小环”触发严格唤醒。
- 默认关闭跨片段拼接；无静音结尾或超过 3.2 秒的连续声音不参与唤醒匹配。
- 低质量片段在进入 Vosk 前被过滤，避免持续噪声增加误唤醒。
- 唤醒后拍照命令使用独立近音规则，不会放宽常驻唤醒条件。

默认 ffmpeg 滤波：

```text
highpass=f=80,lowpass=f=7600,afftdn=nf=-30
```

`voice-agc` 是诊断选项，不是默认。软件或硬件 AGC 在持续背景噪声下可能把噪声抬到 VAD 区间，必须实机验证后才能启用。

## Playback And Capture

所有固定回复、提示音和 HTTP TTS 共用串行优先级队列，避免多个 `aplay` 同时占用声卡。

`capture_playback_mode` 决定播放时怎样处理麦克风：

| Mode | Behavior | Use case |
| --- | --- | --- |
| `keep` | 保持 ALSA capture/RTP 管线，播放期间 drain 并丢弃识别 PCM | 默认；声卡全双工稳定时恢复最快 |
| `restart` | 播放前释放 capture，播放后快速重建并沿用噪声阈值 | 单 TT Hub 或录放并发不稳定的设备 |

两种模式都会在播放时暂停关键词判定。`restart` 只重建音频采集，不应重启 RGB-D sender。没有扬声器参考信号时，当前滤波不等于真正 AEC；播放尾音由 `echo_tail_seconds` 和会话门控处理。

## Device Recovery

- 服务按稳定 ALSA card name 匹配录音和播放设备，不固定 `hw:N,0`。
- USB 重枚举后重新解析 card，并重新应用 mixer 捕获增益与 AGC。
- 读取超时或短读先在进程内重建 capture；持续失败后退出，由 systemd 重启。
- 某些带硬件 AEC 的声卡会输出合法数字静音，设备 profile 可把 `zero_audio_restart_seconds` 设为 `0`，但真实 ALSA 读取错误仍触发恢复。
- udev 规则阻止 PulseAudio 自动抢占已匹配的 USB 音频型号；不影响其他声卡。

## Alternative Backends

`sherpa_kws.py` 是候选 KWS 后端：

```bash
./start_sherpa_kws_no_record.sh
```

它能使用自定义关键词，但尚未替代经过更长时间实机运行的 Vosk 默认路径。`wake_word.py` 是早期个人模板匹配原型，仅用于硬件验证。

代码保留 `espeak` 和 sherpa TTS 诊断后端；需要统一音色的正式设备使用 Edge TTS 和预生成固定回复，不在运行时自动切换音色。

## AEC Boundary

真正 AEC 需要同时获得麦克风信号和扬声器参考信号。当前 ALSA 独占路径若没有参考信号，只能依靠半双工、尾窗和降噪降低回授。后续若引入 PipeWire/Pulse echo-cancel 或 WebRTC/Speex DSP，必须验证 ARM 架构依赖、端到端延迟、RTP 归档语义和热插拔恢复。

## Verification

```bash
arecord -l
aplay -l
systemctl --user status xiaohuan-wake.service
journalctl --user -u xiaohuan-wake.service -f
python3 vosk_wake.py file <test.wav>
```

验收至少覆盖静音、背景声、不同说话人、远近距离、唤醒后立即说命令、播放期间回授、USB 重插和持续运行。
