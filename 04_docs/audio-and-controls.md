# Audio And Device Controls

更新时间：2026-09-02

音频和 GPIO 属于可选发送端能力，不改变 RGB-D 主媒体协议。实现位于 `12_apps/xiaohuan_voice_photo/`、`05_tools/audio_archive_receiver.py` 和相关 systemd 配置。

## Audio Paths

当前存在三条相互独立的路径：

| Path | Purpose | Lifetime |
| --- | --- | --- |
| 实时 RTP/Opus | 下游实时监听 | 服务启停控制，可独立于视频录制 |
| 全天音频归档 | NAS 连续音频分片 | 音频归档服务启停控制 |
| 录制任务音频 | 与视频 session/window 对齐 | 跟随视频录制任务 |

音频发送使用 48 kHz、单声道、Opus、20 ms RTP 包。每个 sender 使用唯一 UDP 端口与 SSRC。端口以 `audio_archive_receiver_loop.json` 和设备 systemd drop-in 为准，不在业务代码中按 IP 推断。

发送端周期上报音频 timing anchor；receiver 根据 CLOCK_SYNC 模型生成音频全局时间。丢包时任务音频可补静音保持连续时间轴，但 `received_packets`、`silence_packets`、`outage_intervals` 和质量状态必须如实记录。

## Wake And Photo Flow

本机关键词唤醒和“拍照”命令只用于本机交互：

```text
microphone
  -> local wake word / speech recognition
  -> play response or cue
  -> write snapshot request
  -> sender captures next complete MJPEG frame
  -> independent reliable snapshot queue
  -> receiver local durable staging
  -> captured response
  -> photo uploader atomic NAS publish
```

一次拍照命令默认生成 3 张，目标间隔 0.2 秒，共用 `burst_id` 和一个 NAS 目录。识别后播放提示只代表请求已受理；最终成功以 receiver 回执和 NAS 文件为准。

相机配置有旋转时，照片走独立 JPEG 旋转 worker，避免阻塞采集线程。NAS 不可用时照片留在本地 staging 重试，不能阻塞主媒体。

## TTS

设备可通过本地 HTTP API 接收短中文文本并排队播放。成功语义是 `accepted`，不是“已经播完”。语音唤醒回复和 HTTP 文本应使用同一引擎、voice、rate 与缓存策略，以避免音色不一致。

具体请求格式见 [TTS HTTP API](../12_apps/xiaohuan_voice_photo/tts-http-api.md)。Edge TTS 依赖网络，缓存命中快、首次合成存在网络延迟；离线引擎只作为网络不可用时的降级，不保证音色一致。

播报期间默认暂停或门控麦克风上行，防止音箱回授进入识别和远程音频。恢复采集必须由连续失败二次确认，不能因为一次超时就重建整个 USB 音频链路。

## Utterance Forwarding

可选模式在唤醒回复后等待用户说完：

- 识别为拍照：播放“叮”，执行拍照，不转发语音。
- 其他内容：播放“登”，把完整 WAV 通过 HTTP 发送给下游。
- 提示音在判定语句结束后播放。

具体接口见 [utterance audio HTTP API](../12_apps/xiaohuan_voice_photo/utterance-audio-http-api.md)。实时 RTP 和 utterance forwarding 可分别启停，不应让远端不可达阻塞本地唤醒线程。

## Recording Buttons

带物理按键的 sender 使用 sender 级录制 API：

- 长按录制键约 1 秒：开始该 sender 当前在线的全部相机。
- 长按停止键约 1 秒：停止该 sender 的全部相机。
- receiver 不可达、正在转换状态或请求失败时忽略并不播放成功提示音。
- 成功后立即播放不同提示音；按键去抖和单次长按只触发一次。

电源键长按约 5 秒，等待当前语音结束后播放关机组合提示，再请求系统关机。不能用录制按键误触发全系统其他 sender。

## Recording LED

配置 LED 的设备遵循：

- 开机并且服务正常：常亮。
- 本 sender 任一路处于录制：闪烁。
- 服务停止或关机完成：熄灭。

GPIO 编号和极性必须来自该板卡 service/drop-in，不能把某台底板的引脚硬编码到通用 Python 模块。

## Device Discovery

USB 声卡编号会在重插、重启或换端口后改变。服务应按稳定 card name/match 解析 ALSA capture/playback 设备，而不是永久写死 `hw:3,0`。排障顺序：

```bash
arecord -l
aplay -l
arecord -D <resolved-device> -f S16_LE -r 16000 -c 1 -d 3 /tmp/test.wav
aplay -D <resolved-device> /tmp/test.wav
systemctl --user status xiaohuan-wake.service
```

不能同时让 PulseAudio/PipeWire 和独占 ALSA 服务占用同一个 capture endpoint。设备断电后应由 systemd 重启与稳定设备匹配恢复，不依赖人工重新插拔。

## Failure Boundaries

- 网络发送失败不能重启相机 sender 或整个语音服务。
- 音频采集失败重建仅影响 capture 子进程。
- TTS、RTP、录音归档和拍照分别使用有界队列。
- 提示音与 TTS 必须串行混音，禁止并发访问音箱造成截断。
- 没有音频包与“收到静音音频”是两种状态，归档时不能混淆。
