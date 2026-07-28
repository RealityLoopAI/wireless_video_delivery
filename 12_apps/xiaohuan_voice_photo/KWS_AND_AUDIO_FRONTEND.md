# 唤醒识别后续方案说明

## 当前默认方案

默认运行 `vosk_wake.py`：

- WebRTC VAD 判断人声。
- ffmpeg 实时语音前端做高通、低通和 FFT 降噪。
- Vosk 小语法识别 `你好 小环 / 您好 小环 / 你好 / 您好 / 小环 / [unk]`。
- 支持 `你好` 和 `小环` 分段命中。
- 低质量片段先过滤，不再全部送进 Vosk。

这个方案已经能在当前设备上长期运行，CPU 低，且不需要录制个人模板。

## sherpa-onnx KWS 候选方案

`sherpa_kws.py` 已改成候选 KWS 后端：

- 默认关键词文件：`keywords_nihao_xiaohuan_variants.txt`
- 默认回复：`response_wozai_tts_default.wav`
- 默认采样率：`16000`
- 默认阈值：`0.20`

测试音频 `debug_wake_nihao_xiaohuan.wav` 上已能检出 `你好小环`。可用下面命令前台试跑：

```bash
./start_sherpa_kws_no_record.sh
```

当前不把它设为默认，是因为 Vosk 方案已经经过更长时间实机运行；sherpa 还需要更多真人距离、噪声、音量测试。

## AEC / NS / AGC 情况

本机当前没有 `pipewire-pulse`，`pactl` 不能连接，因此不能直接加载 Pulse/PipeWire 的 `module-echo-cancel` 虚拟源。

尝试安装 Python 的 WebRTC/Speex 音频处理包时也失败：

- `webrtc-audio-processing` 在 ARM 上编译到了 x86 SSE 头文件 `xmmintrin.h`。
- `speexdsp` 缺少系统级 `speex/speex_echo.h` 开发头。

所以当前已落地的是 ffmpeg 实时音频前端，默认滤波：

```text
highpass=f=80,lowpass=f=7600,afftdn=nf=-30
```

另有可选 AGC 滤波：

```bash
python3 vosk_wake.py listen --audio-filter voice-agc
```

实测 AGC 会放大当前设备底噪，因此不作为默认。默认前端能覆盖基础降噪，但不等同于真正 AEC。真正 AEC 需要拿到扬声器参考信号，建议后续二选一：

- 安装并启用 `pipewire-pulse`，用 echo-cancel 虚拟源作为录音设备。
- 安装系统级 `libspeexdsp-dev` 或可用的 WebRTC Audio Processing 库，再把扬声器参考流接入 Python 前端。

## systemd 常驻服务

已提供用户服务：

```bash
./install_wake_service.sh
systemctl --user status xiaohuan-wake.service
journalctl --user -u xiaohuan-wake.service -f
tail -f wake_runtime.log
```

服务会自动重启。安装脚本会尝试开启 linger；如果系统权限不允许，服务仍可在当前用户登录会话中运行。
