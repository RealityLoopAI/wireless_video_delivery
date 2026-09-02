# Xiaohuan TTS HTTP API

该接口让局域网程序向启用 Xiaohuan 语音服务的发送端提交中文或中英混合文本。服务默认使用 Edge TTS `zh-CN-XiaoyiNeural`，固定回复使用同音色预生成音频。

## Endpoint

```http
POST http://<sender-ip>:18082/api/tts/speak
Content-Type: application/json
```

接口只面向受信任局域网，不校验 token 或调用方身份。不要把 `18082/TCP` 映射到公网。

## Request

```json
{
  "request_id": "dataset-job-0001",
  "text": "数据采集已经完成，camera 01 当前帧率为 30 FPS。"
}
```

| Field | Constraint |
| --- | --- |
| `request_id` | 非空字符串，最多 128 字符；调用方生成唯一值 |
| `text` | 非空字符串，最多 500 字符；支持中文、数字和英文 |

音色、语速和音量由设备配置统一控制，调用方不能在单次请求中覆盖。

成功入队返回 HTTP `202`：

```json
{
  "accepted": true,
  "request_id": "dataset-job-0001",
  "task_id": "bdf531f18f2a48cdb51f8ad7f2a837ce",
  "queue_position": 1,
  "duplicate": false
}
```

`accepted=true` 只表示进入内存队列，不表示已经合成或播放完毕。当前没有任务完成查询接口。

## Queue And Idempotency

- 同一进程生命周期内，相同 `request_id` 只播放一次。
- 重复请求返回原 `task_id` 和 `duplicate=true`；已完成项的 `queue_position=0`。
- 远程文本按 FIFO 排队；唤醒回复、拍照“叮”和转发“登”使用更高优先级。
- 用户唤醒会话进行中，远程文本可以入队但不能插播。
- 默认最多保留 100 个未完成任务；队满返回 HTTP `429` 和 `queue_full`。
- 服务重启会清空内存队列和幂等记录，已经生成的磁盘语音缓存仍保留。

## Examples

curl：

```bash
curl -sS -X POST http://<sender-ip>:18082/api/tts/speak \
  -H 'Content-Type: application/json' \
  -d '{"request_id":"demo-001","text":"数据采集已经完成。"}'
```

Python 标准库：

```python
import json
from urllib.request import Request, urlopen

payload = {"request_id": "demo-002", "text": "请检查设备连接状态。"}
request = Request(
    "http://<sender-ip>:18082/api/tts/speak",
    data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
    headers={"Content-Type": "application/json"},
    method="POST",
)
with urlopen(request, timeout=2) as response:
    print(json.loads(response.read().decode("utf-8")))
```

## Health

```bash
curl -sS http://<sender-ip>:18082/healthz
```

关键字段包括 `ok`、`tts_ready`、`tts_backend`、`queue_depth`、`queue_capacity` 和本地 `cue_names`。`ok=true` 只证明服务可响应；首次未缓存文本仍依赖 Edge TTS 网络。

## Synthesis And Playback

- 首次文本发送到 Edge TTS；请求失败或默认 4 秒超时后记录失败，不自动切换成不同音色。
- 失败后进入退避窗口，避免网络故障时反复阻塞队列。
- 成功结果写入内存和 `$HOME/.cache/xiaohuan/edge_tts`；默认磁盘上限 256 MB。
- 长文本按句切分，首句准备后开始播放，后续句子继续合成。
- 音箱打开或播放失败会在有限时间内重试；超时子进程被回收，不能永久卡住播放队列。
- 固定“我在”和“叮/登”是本地音频，不依赖临时联网。

播放期间的麦克风行为由 `XIAOHUAN_CAPTURE_PLAYBACK_MODE` 决定：`keep` 保持采集并丢弃识别 PCM，`restart` 临时释放后快速重建。具体选择由设备 profile 决定，不能假设所有 USB 声卡都需要同一种模式。

## Operations

```bash
systemctl --user status xiaohuan-wake.service
systemctl --user restart xiaohuan-wake.service
journalctl --user -u xiaohuan-wake.service -f
```

主要环境变量：

```text
XIAOHUAN_TTS_HTTP_ENABLED=1
XIAOHUAN_TTS_HTTP_BIND=0.0.0.0
XIAOHUAN_TTS_HTTP_PORT=18082
XIAOHUAN_TTS_BACKEND=edge
XIAOHUAN_TTS_EDGE_VOICE=zh-CN-XiaoyiNeural
XIAOHUAN_TTS_EDGE_TIMEOUT_SECONDS=4
XIAOHUAN_TTS_EDGE_CACHE_DIR=$HOME/.cache/xiaohuan/edge_tts
XIAOHUAN_TTS_EDGE_CACHE_MAX_MB=256
XIAOHUAN_TTS_SPEAKER_RETRY_SECONDS=15
XIAOHUAN_TTS_RESUME_DELAY_SECONDS=0.2
XIAOHUAN_ECHO_TAIL_SECONDS=0.03
XIAOHUAN_CAPTURE_PLAYBACK_MODE=keep|restart
```

安装与声卡恢复见 [README.md](README.md)，音频前端设计见 [kws-and-audio-frontend.md](kws-and-audio-frontend.md)。
