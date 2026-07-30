# 133 语音播报 HTTP 接口

## 服务地址

```text
设备: orangepi5pro-d12a4719
IP: 192.168.66.133
端口: 18082/TCP
接口: POST /api/tts/speak
Content-Type: application/json
```

该接口只面向当前可信局域网，不校验令牌、来源 IP 或调用方身份，也不配置
CORS。不要把 `18082/TCP` 映射到公网。

## 提交播报

请求体必须同时包含 `request_id` 和 `text`：

```json
{
  "request_id": "dataset-job-20260729-0001",
  "text": "数据采集已经完成，camera 01 当前帧率为 30 FPS。"
}
```

约束：

- `request_id`：非空字符串，最多 128 个字符；调用方应保证唯一。
- `text`：非空字符串，最多 500 个字符。
- 支持中文、数字、英文单词和英文缩写混合输入。
- 音量和语速由 133 固定配置，调用方不能覆盖。

成功入队返回 HTTP `202`：

```json
{
  "accepted": true,
  "request_id": "dataset-job-20260729-0001",
  "task_id": "bdf531f18f2a48cdb51f8ad7f2a837ce",
  "queue_position": 1,
  "duplicate": false
}
```

`accepted=true` 只表示任务已经进入内存队列，不表示已经播放完毕。当前没有任务
完成状态查询接口。

## 幂等和队列

- 同一进程生命周期内，相同 `request_id` 只播放一次。
- 重复提交仍返回 `202` 和原 `task_id`，同时返回 `duplicate=true`。
- 已完成任务重复提交时 `queue_position=0`。
- 远程文本、唤醒回复和拍照回复共用一个严格 FIFO 队列。
- 队列最多保留 100 个未完成任务；满队列返回 HTTP `429`，已经受理的任务不会被
  丢弃。
- 服务重启后，队列和幂等记录清空。

队满响应：

```json
{
  "accepted": false,
  "error": "queue_full"
}
```

## 调用示例

Linux/macOS：

```bash
curl -sS -X POST http://192.168.66.133:18082/api/tts/speak \
  -H 'Content-Type: application/json' \
  -d '{"request_id":"demo-001","text":"数据采集已经完成，camera 01 正常。"}'
```

Python 标准库：

```python
import json
from urllib.request import Request, urlopen

payload = {
    "request_id": "demo-002",
    "text": "请检查设备 RGBD 连接状态。",
}
request = Request(
    "http://192.168.66.133:18082/api/tts/speak",
    data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urlopen(request, timeout=2) as response:
    print(json.loads(response.read().decode("utf-8")))
```

## 健康检查

```bash
curl -sS http://192.168.66.133:18082/healthz
```

正常示例：

```json
{
  "ok": true,
  "service": "xiaohuan_speech",
  "tts_ready": true,
  "tts_backend": "edge-tts:zh-CN-XiaoyiNeural+offline-fallback",
  "queue_depth": 0,
  "queue_capacity": 100
}
```

## 播放和麦克风行为

- 133 默认使用联网 Edge TTS `zh-CN-XiaoyiNeural` 音色；待播文本会发送给
  Edge TTS 服务进行合成。
- Edge 请求失败或 4 秒内未完成时自动回退到离线 eSpeak NG；失败后 30 秒内的新任务
  直接使用离线回退，避免反复等待网络超时。
- 相同文本在当前进程内命中有界 PCM 缓存后不再访问网络；服务重启后缓存清空。
- 文本按句切分；首句合成后立即开始播放，后续句子继续生成。
- 播放期间停止 USB 麦克风采集，所以 Vosk 唤醒、拍照命令识别和 RTP/Opus 麦克风
  推流会同时暂停。
- 队列暂时清空后等待 200 ms，再重新打开麦克风并恢复识别和 RTP。
- 音箱打开或播放失败时重试 5 秒；仍失败则丢弃当前任务并继续下一条。
- 当前局域网实测 `Xiaoyi` 首次合成完整短句约需 1.4～2.4 秒；接口入队响应不等待
  合成。离线回退音色比神经 TTS 更机械。

## 运维

服务由用户级 systemd 管理：

```bash
systemctl --user status xiaohuan-wake.service
systemctl --user restart xiaohuan-wake.service
tail -f /home/orangepi/Desktop/new_experiment_2026-07-02/wake_runtime.log
```

关键环境变量：

```text
XIAOHUAN_TTS_HTTP_ENABLED=1
XIAOHUAN_TTS_HTTP_BIND=0.0.0.0
XIAOHUAN_TTS_HTTP_PORT=18082
XIAOHUAN_TTS_BACKEND=edge
XIAOHUAN_TTS_EDGE_VOICE=zh-CN-XiaoyiNeural
XIAOHUAN_TTS_EDGE_TIMEOUT_SECONDS=4
XIAOHUAN_TTS_EDGE_CACHE_ENTRIES=64
XIAOHUAN_TTS_SPEAKER_RETRY_SECONDS=5
XIAOHUAN_TTS_RESUME_DELAY_SECONDS=0.2
```

后端依赖：

```bash
sudo apt-get install -y espeak-ng ffmpeg
python3 -m pip install --user edge-tts
```

`XIAOHUAN_TTS_BACKEND=espeak` 可强制完全离线运行。可选 sherpa 中英模型可通过
`./download_tts_model.sh` 安装。
