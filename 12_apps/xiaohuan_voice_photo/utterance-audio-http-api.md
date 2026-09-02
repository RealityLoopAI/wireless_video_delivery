# 小环整句语音 HTTP 接收接口

## 1. 功能

`orangepi5pro-d12a4719` 不再持续发送 RTP/Opus 音频。设备只在以下流程中发送用户
说出的完整语句：

1. 用户说“你好小环”。
2. 设备播放“我在”。
3. “我在”播放结束后，设备等待用户开始说话。
4. 连续静音 600ms 后判定用户说完，单句最长 60 秒。
5. 如果本地识别为拍照指令，播放“叮”并执行原有三连拍，不发送该段音频。
6. 其他内容或本地识别为空时，播放“登”，随后异步发送完整 WAV。

发送音频保留约 200ms 语音前缓冲和 300ms 语音后缓冲。原有文本转语音接口
`POST http://192.168.66.133:18082/api/tts/speak` 保持不变。

## 2. 接口

```text
接收设备: macOS
接收地址: 192.168.66.113
协议: HTTP/1.1
端口: 50020/TCP
方法: POST
路径: /api/audio
Content-Type: audio/wav
```

HTTP 请求体只包含 WAV 文件完整字节，不使用 JSON、multipart 或旁路元数据。

WAV 固定格式：

```text
编码: PCM signed 16-bit little-endian
采样率: 16000Hz
声道: 单声道
最长时长: 约 60 秒
最大请求体: 4MiB
```

接收端返回任意 `2xx` 状态码即视为成功。参考脚本成功时返回：

```json
{
  "accepted": true,
  "filename": "20260730_221800_123456_a1b2c3d4.wav",
  "duration_seconds": 3.2
}
```

发送失败或超时后最多重试 3 次。上传在后台执行，不阻塞下一次唤醒。发送队列容量
为 8 段，队满时丢弃最旧语音。成功后发送端不保留本地 WAV。

## 3. macOS 接收脚本

把 `xiaohuan_audio_receiver.py` 放在 Mac 桌面，然后运行：

```bash
python3 ~/Desktop/xiaohuan_audio_receiver.py
```

默认监听：

```text
http://0.0.0.0:50020/api/audio
```

默认保存到：

```text
~/Desktop/xiaohuan_received_audio/YYYY-MM-DD/*.wav
```

自定义目录：

```bash
python3 ~/Desktop/xiaohuan_audio_receiver.py \
  --port 50020 \
  --output-dir ~/Desktop/model_audio_input
```

macOS 首次运行时如果弹出防火墙提示，需要允许 Python 接收局域网连接。当前接口没有
令牌认证，只应在可信局域网中使用，不要映射到公网。

## 4. 健康检查

在 Mac 本机执行：

```bash
curl -sS http://127.0.0.1:50020/healthz
```

正常返回：

```json
{
  "ok": true,
  "service": "xiaohuan_audio_receiver",
  "received": 0,
  "failures": 0
}
```

从其他局域网设备检查：

```bash
curl -sS http://192.168.66.113:50020/healthz
```

## 5. 手工发送测试

```bash
curl -sS -X POST http://192.168.66.113:50020/api/audio \
  -H 'Content-Type: audio/wav' \
  --data-binary @test.wav
```

如果返回 `202` 且目标目录出现 WAV，说明接口正常。

## 6. 时序与并发

- “叮”和“登”都在用户说完之后播放。
- 拍照顺序是：用户说完 -> “叮”播放完成 -> 三连拍。
- 普通语音顺序是：用户说完 -> “登”播放完成 -> WAV 进入后台发送队列。
- 用户会话进行期间，外部文本 TTS 任务只入队，不会插播到用户录音中。
- 普通语音进入发送队列后立即恢复唤醒监听，不等待网络上传完成。
- 下游接口未启动不会影响本地唤醒和拍照，失败语音在重试结束后丢弃并写入日志。
