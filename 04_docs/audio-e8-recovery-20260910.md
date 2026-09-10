# e8 音频零输入排查与恢复

日期：2026-09-10。以下时间均为北京时间。

## 结论

`lubancat-e8cc0cb3` 的 USB 麦克风可以采集，语音进程也有 RTP 包输出，但归档目标仍为旧网段 `192.168.66.196:50032`。实际接收端已是 `192.168.1.196`，导致接收端零输入。不是 NAS 上传后丢失 `audio.opus`。

本次只修复 e8 独立语音服务的站点配置，未修改媒体协议、相机参数、音频编码、静音留存语义，未重启 sender 视频服务、receiver 或正在进行的视频录制。

## 证据

- 用户指定的 NAS 目录：`audio/lubancat-e8cc0cb3/2026-09-09/144500`。
- `audio_meta.json` 与 `audio_ready.json` 均记录 `received_packets=0`、`quality_status=no_input`、`audio_valid=false`。预期 45,000 包，最长无输入 900,000 ms。仅有元数据和时序文件，没有音频文件。
- 当前接收端 e8 计数也为 0，`input_outage=true`，无 timing anchor。不是把正常静音误判为无输入。
- e8 `arecord -l` 识别 `SM15 M1 USB audio`，采集端点由语音服务占用；运行日志持续有 RMS/VAD 测量。
- 服务从 `/home/cat/Desktop/new_experiment_2026-07-02/run_wake_service.sh` 启动。实际进程参数为 `--audio-archive-host 192.168.66.196`。
- 13:55:56 发送端抓包：每约 20 ms 向 `192.168.66.196:50032` 发出一个 92 字节 UDP payload。接收端实际地址是 `192.168.1.196`。
- 视频发现状态文件已记录新接收端，但该桌面旧实验脚本没有接入该发现结果。升级视频程序并不等于升级独立语音服务。

历史片段确认为零输入；错误目标地址与本次复现、恢复效果一致。没有对 9 月 9 日全天保留的网络抓包，不能把当前抓包表述为历史全天的逐包证据。

## 已部署修复

在 e8 新增用户服务 drop-in：

```text
/home/cat/.config/systemd/user/xiaohuan-wake.service.d/zzz-audio-receiver.conf
```

内容见 [部署配置副本](../08_reports/audio-e8-recovery-20260910/zzz-audio-receiver.conf)。仅覆盖 `XIAOHUAN_AUDIO_ARCHIVE_HOST=192.168.1.196`，保留每设备端口 50032、SSRC 3322710864、timing 50130、control 50131、USB 声卡匹配和唤醒参数。

13:56:17 执行用户服务 `daemon-reload` 和 `restart xiaohuan-wake.service`。配置持久保存，重启后仍生效。视频服务启动时间仍是 11:16:34，`NRestarts=0`。

这是当前站点地址修复，不等于完成动态迁网适配。以后更换接收端地址，仍需同步检查语音服务的实际目标，不能仅检查视频发现成功。

## 验收

- 恢复后 e8 `received_packets` 从 0 增至 1,030、4,804、5,678，`input_outage=false`，`timing_anchor_available=true`，`clock_sync_valid=true`。
- 未出现格式错误、SSRC 不匹配或 payload type 不匹配。
- 13:59:01 抓包已改为 `192.168.1.196:50032`，仍为约 20 ms 一包。
- 独立录音生成 `audio.opus.inprogress`，视频目录 `lubancat-e8cc0cb3_cam01/2026-09-10/135314` 生成 `.audio.opus.inprogress`。
- 视频随录音频 `ffprobe` 识别为 Opus、48 kHz、单声道；对其当时可读的前 180 秒运行 `ffmpeg -f null -`，退出码 0。它包含修复前缺口的补静音，不代表整段原始声音完整。
- HTTP `/healthz` 返回 `ok=true`、`tts_ready=true`、队列 0。未安排现场人员实测唤醒、听音及拍照，不能据此声明这些交互端到端均已验收。
- 约 129.986 秒连续采样新增 6,499 包，即 49.998 包/秒；无新增 outage、无新增重建请求。原始状态见 [状态采样](../08_reports/audio-e8-recovery-20260910/status-samples.json)。
- 14:00 自然切片发布完成：NAS `audio/lubancat-e8cc0cb3/2026-09-10/134500/audio.opus` 大小 1,048,337 字节，Ogg/Opus、48 kHz、单声道、容器时长 900.0065 秒。直接从 NAS 对整文件运行 `ffmpeg -f null -`，退出码 0、无解码报错。
- 此跨修复片段收到 10,960/45,000 包，修复前无输入 680.8 秒，状态仍为 `partial`、`audio_valid=false`。容器的 900 秒包含缺口补静音，不能当作 900 秒真实声音。见 [meta](../08_reports/audio-e8-recovery-20260910/recovered-audio-meta.json)、[ready](../08_reports/audio-e8-recovery-20260910/recovered-audio-ready.json)。
- 14:00 新片段当前已写入的时序行显示每秒 50/50 包、无补静音；该完整 15 分钟窗口尚未结束，未宣称已完成整窗或全天长测。
- 当前音频上传 pending 为 0，`last_error` 为空；累计 `failed_uploads=1` 在此次前后未增长，不能表述成历史从未出错。

## 数据影响与边界

历史零输入数据无法恢复真实声音，不伪造音频、不清除缺录标记。修复跨越的当前分片仍应如实保留前半段无输入状态；后续完整分片才适合评估持续完整率。

e8 下游实时监听目标仍是旧 `192.168.66.32:50032`。该路径独立于接收端归档，未擅自切换到未经确认的下游地址。接收端配置中的离线 d12a、4df 音频路径也没有在本次修改。

批量补查历史目录时，SMB 读取 `2026-09-09/013000/audio_meta.json` 曾返回 `EBUSY`，因此未完成全部 60 段及 17 个视频目录的独立复核。指定的 `144500` 和本次新发布文件读取、解码正常；该单次历史读取错误不能解释已经由收包计数确认的零输入，也未在本次对 NAS 做重挂载或修复。

## 防复发与回退

迁网/部署验收必须分别核查视频、独立语音进程和接收端音频：

1. `systemctl --user cat xiaohuan-wake.service` 确认实际启动目录、所有 drop-in 和最终地址；不能只看源码模板。
2. 查看运行日志的 `audio_archive_target`，确认与本次选定 receiver 一致。`auto` 只有在运行脚本和 resolver 确实支持时才可用。
3. 采样接收端 `http://127.0.0.1:18083/api/status`，确认收包增量约 50 包/秒、无 outage、有效 timing anchor。服务 `active` 和 `sendto` 成功不代表 receiver 收到了 UDP。
4. 等待自然切片，确认 NAS `audio_ready.json`、非零 `received_packets` 和可解码 `audio.opus`；视频录制还要检查视频目录中的随录音频。
5. 不要通过重复重启麦克风解决错误网络目标；零输入告警需要区分设备离线、采集故障和目标地址错误。

回退只涉及新增 drop-in：移走 `zzz-audio-receiver.conf` 后重新加载、重启语音服务即可恢复原配置。但原地址在当前站点仍不可用，故不建议回退。没有覆盖旧配置文件，也没有修改旧录制文件。
