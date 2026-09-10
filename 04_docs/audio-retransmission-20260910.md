# Audio Archive Retransmission

日期：2026-09-10。针对 52d2 音频秒级缺包的有限窗口补传，不是无限时长可靠传输。
不改变现有下游实时 RTP 接口、视频协议、音频采样率、语音音色或曝光参数。

## 原因与改动

已确认现场没有 DTX，历史缺口对应 RTP 序号缺失，见 [排查报告](audio-gap-52d2-20260910.md)。
原归档直接消费裸 UDP，丢包之后没有重传，并在约 250ms 后把空槽写成静音。

新增流程：

```text
GStreamer RTP -> sender gate -> 原实时 UDP 目的地（不变）
                         -> 原归档 UDP 目的地
                         -> 有界内存缓存原包
receiver RTP 序号缺口 -> 控制 UDP 请求 -> sender 查缓存 -> 仅向配置的归档地址补发原包
receiver 延迟 8 秒决算归档时槽 -> 到期仍缺失才填静音并标 partial
```

发送缓存使用单调时钟清理，最多 30 秒、1500 包、3MiB；任一上限到达就清理旧包。
它不是断电持久缓存，不能恢复源端尚未生成、已经过期或进程重启前的声音。
接收端每流最多跟踪 400 个缺包，约每 200ms 请求一次，每批最多 64 包；
同一个包的请求间隔至少 400ms，发现缺口后最多重试 6 秒。原包比补传窗口更早到期时仍可能被判迟到。
连续断网超过窗口不能保证补全，不能因为缓存有 30 秒就宣称支持 30 秒断网无损。

## 协议与防护

复用发送端已有控制 UDP 端口 50131，新增 JSON：

```json
{
  "protocol_version": "3.0",
  "message_type": "audio_retransmit_request",
  "sender_id": "lubancat-52d2ef0c",
  "stream_instance_id": "current-gate-instance",
  "packets": [[20225, 2888294131, 1389555468]]
}
```

每个三元组为原始 sequence、rtp_timestamp、ssrc；联合匹配防止序号回绕补错。
sender_id、当前实例必须匹配；只接受已配置接收端 IP 的请求，补发地址来自本机配置，
不接受请求方指定任意反射目的地。非法类型、超长列表、旧实例、过快请求均拒绝。
这是受信任局域网内的源 IP 限制，不是加密身份认证。
补发使用原 RTP 包，不修改序号、时间戳或内容，也不重复推给实时监听端。
`audio_timing_anchor.audio_retransmit` 携带能力与计数，旧 sender 不支持时 receiver 不发送补传请求。

## 配置与上线依赖

`audio_archive_receiver.json` 中仅给目标 stream 增加：

```json
"repair_enabled": true,
"repair_wait_ms": 8000
```

未配置时 repair_enabled 默认 false；其他 sender 不自动增加录音延迟。
`max_buffer_seconds` 必须大于 repair_wait_ms 对应时长。
**必须同时把 receiver 的 `task_audio.finalize_wait_ms` 从 3000 调至 10000**，
否则视频分片可能在音频补传结束之前收尾，误报音频缺失。此项由 C++ receiver 启动时读取。
当前 C++ 配置上限为 10000，不能设为 12000，否则旧二进制拒绝启动。
这将使启用补传的任务音频及独立音频最终发布最早滞后约 8 秒，留约 2 秒收尾余量，但不延迟实时监听。

新增 sender 文件 `audio_packet_cache.py` 必须与 `vosk_wake.py` 一起部署。
52d2 现场旧程序缺少 `CaptureRebuildGuard`，本次还需要部署 `audio_capture_recovery.py`，
避免把网络端无输入误当作本机麦克风故障而反复重建采集。
保留现场照片识别时长默认值 0.45 秒及显式网络配置，不借本次修改语音识别灵敏度。

## 观测与质量

音频状态 API `/api/status` 新增每流：

- `audio_repair_enabled`、`audio_repair_wait_ms`。
- `audio_repair`: detected/recovered/expired/pending/requests/send_errors。
- `sender_audio_retransmit`: cached/evicted/requests/cache_misses/retransmitted/send_errors/forwarded/cache_packets/cache_bytes。

`recovered` 表示原先缺失的包后来到达（可能是乱序原包或重传包），不等于已经成功写入正式录像。
是否真正补全必须检查最终 `audio_meta.json`、收到包数、silence_packets、late_packets 和 ready 标记。
不放宽 complete 门槛，不把补静音计入原始收到包。

## 验证与回退

新增 `test_audio_retransmit.py`：

- 连续 120 包（2.4s）缺口，NACK 丢失后再次请求。
- 16 位序号及 32 位 RTP 时钟同时回绕。
- 缓存过期、字节和包数上限、非法请求、旧实例隔离。
- 真实 loopback UDP 故障代理，首发丢 120 包且部分重传再丢一次；
  最终任务音频 200/200 包、0 补静音，并验证线程关闭。

部署前需停止视频录制并确认 finalizer/搬运清空；音频常驻服务本身重启会形成一个维护边界，
不能将这一段维护中断伪装成连续采集。备份脚本及两个配置文件后再替换。
回退时恢复旧脚本、配置及 task_audio 等待值；已发布录音不改写。
真实无线长期录制效果以实机验证报告为准，隔离测试不能证明现场永不丢音。

## 本次上线和实测

已上线：52d2 发送端缓存、重建保护和接收端补传。其他 sender 不启用补传。
备份分别为 `/home/cat/wvd-audio-repair-backup-20260910` 和
`/home/loop/wvd-audio-repair-backup-20260910`。
现场 voice 脚本保留两项旧默认值，差异见归档 `field-compat.patch`；实际归档目标通过现有启动参数明确指定为 192.168.1.196。

完整回归 37/37 通过，159.87 秒；配置修正后再跑 RGB PTS 与音频补传集成测试 2/2 通过。
首次上线时误把等待值设为 12000，旧 receiver 拒绝启动并触发服务重试；
已改为 10000 并清除 systemd 失败限制恢复。首次录制测试未能连接管理接口，未创建录制任务。
配置边界断言及使用 10000 等待值的 receiver 启动集成用例已补充，防止再次漏检。

恢复后六路 60 秒控制时长实机测试通过，目录为各相机 `2026-09-10/170039`。
停止到交付状态完成约 24.677 秒，六路视频 ready/complete，写入错误为零。
52d2 最终音频：2951 个应收时槽、2942 个实收、9 个补静音，实收率 99.695%，
最长缺口 20ms，质量标记 complete；`ffmpeg -v error -xerror ... -f null -` 全文件解码返回 0，无错误输出。
现场计数观察到 34 个序号缺口随后全部到达、0 个待补和过期；sender 重发 51 包，无缓存未命中和发送错误。
该计数覆盖服务启动以来，不等同于全部发生在本次短录制内，也不等同于全部补入某个特定文件。

### 残留问题

仍有 9 个单包时槽空缺，不能宣称零缺失。17:00:58 的独立音频时序同时记录
`silence_packets=1` 和 `duplicate_packets=1`，而视频任务的音频元数据 duplicate_packets 仍为 0。
这说明存在音频时槽碰撞/丢弃统计的另一条路径，需要进一步检查时间映射到 20ms 网格的舍入，
以及任务音频未继承全局丢弃计数的问题，不能继续把这些空槽都解释成网络丢包。
本次没有修改时间轴映射或放宽完整性门槛，也没有进行长时间真实无线故障复现测试。

证据目录：[audio-repair-20260910](../08_reports/audio-repair-20260910)。
