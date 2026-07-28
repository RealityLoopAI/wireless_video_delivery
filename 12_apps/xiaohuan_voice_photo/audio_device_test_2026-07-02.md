# USB 音箱麦克风一体设备测试记录 2026-07-02

## 设备枚举

USB 层可见设备：

```text
08bb:2902 Texas Instruments PCM2902 Audio Codec
8087:1024 Intel Corp. USB2.0 Device
```

ALSA 设备：

```text
card 3: Device [USB2.0 Device]
  USB ID: 8087:1024
  Playback: hw:3,0, stereo, S16_LE, 48000 Hz
  Capture:  hw:3,0, mono,   S16_LE, 48000 Hz

card 4: Device_1 [USB PnP Sound Device]
  USB ID: 08bb:2902
  Capture:  hw:4,0, mono,   S16_LE, 44100/48000 Hz
  Playback: not exposed by ALSA
```

## 测试文件

```text
mic_card3_usb2_5s.wav
mic_card4_pcm2902_5s.wav
speaker_test_1khz_1p5s.wav
```

## 录音测试

`hw:3,0` 录音：

```text
format: PCM S16_LE, mono, 48000 Hz
duration: 5.00s
Peak level: -1.17 dB
RMS level: -34.48 dB
result: 有明显输入信号
```

`hw:4,0` 录音：

```text
format: PCM S16_LE, mono, 48000 Hz
duration: 5.00s
Peak level: -45.20 dB
RMS level: -56.86 dB
result: 输入很弱，更像环境底噪或非主麦克风
```

## 播放测试

通过 `hw:3,0` 播放 1.5 秒 1kHz 测试音，命令正常完成。

```text
aplay -D hw:3,0 speaker_test_1khz_1p5s.wav
```

## 当前建议

后续实验优先使用：

```text
播放设备: hw:3,0
录音设备: hw:3,0
采样率: 48000 Hz
格式: S16_LE
输入通道: mono
输出通道: stereo
```
