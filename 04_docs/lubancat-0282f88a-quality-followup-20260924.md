# cam02 画质、音量与编码器复查

发送端为 `192.168.1.91`，相机 `lubancat-0282f88a_cam02`；2026-09-24 晚间跟进。用户要求继续处理模糊、局部过曝与小音量，编码器先讨论。

## 已实施：扬声器播放音量

现场 ALSA `audio` 卡的 PCM 左右声道为 80/120，即 67%、-20 dB，未静音。麦克风采集是 100%、0 dB。已有语音启动配置写了 PCM 100%，但当前硬件实际值已变为 67%；本次没有证据确定是谁或哪个过程改变了它。

本次把 PCM 左右声道设为 108/120，即 90%、-6 dB，播放链增益相对现场值提高 14 dB。为统一后续语音启动时的音量，新增用户服务 drop-in：

`/home/cat/.config/systemd/user/xiaohuan-wake.service.d/zzzz-playback-volume.conf`

内容为 `Environment=XIAOHUAN_PCM_PLAYBACK_LEVEL=90%`，快照见 [zzzz-playback-volume.conf](../06_configs/deployment/lubancat-0282f88a-quality-followup-20260924/zzzz-playback-volume.conf)。执行 `systemctl --user daemon-reload` 并直接应用 ALSA 音量，语音进程持续运行。重启持久性依据是现有 `run_wake_service.sh` 在启动时读取该变量并调用 `amixer`；本次没有为了测试音量而重启设备。

复查硬件读数仍为 90%、-6 dB，TTS health 为 ready。测试句“语音音量已经调大。这是音量测试。”的任务 `fdf8104f1b9b481ebe898c284bd06db2` 日志为 `success=true`。用户对实际听感的确认待回复；软件播放成功不代表已测量现场声压或音箱失真。

回退到本次修改前的即时音量：`amixer -c audio sset PCM 80 unmute`；移除上述本次新增 drop-in 后 daemon-reload 可恢复原来的启动配置（原配置启动值为 100%，不是此次观察到的 67%）。不要把二者混为一谈。

## 画质：需要可见场景继续验证

上一轮 10 ms 上限及完整范围声明已生效，但运动模糊和局部高亮尚未完全解决。本次 23:01:53 从正在运行的 sender 取得相机原始 JPEG，画面几乎全黑；当前实时元数据仍为原生 AE=1、曝光 raw=100（10 ms）、gain=248。可能是镜头遮挡或现场很暗，不能凭此画面选择高光阈值或判断运动改善。

已请用户将镜头对准有光桌面，同时包含白纸、深色物体和手，并保持位置稳定。本次尚未再次部署画质参数，候选验证路线为：

1. 在同场景对照原生 AE 的 10 ms 与 5 ms 上限，读取实际曝光和增益，比较移动边缘拖影、暗部噪声与高光截断。
2. G305 原生最大增益控制不支持。若减曝光后增益补回、局部高光仍截断，先做少量手动曝光/增益对照，再评估已有软件自动曝光对曝光和增益的联合约束。
3. 软件自动曝光需要关闭相机原生 AE。G305 标准模式的 COLOR AE/GAIN 与底层深度属性有别名关系，因此还要验证深度有效像素及画质，不能只看 RGB。
4. 先确定问题是否已经存在于相机原始 JPEG。亮度、gamma、锐化和增加 H.264 码率无法恢复相机已经截断或运动积分丢失的细节。

这不是已验收的修复配置。原始照片：`Z:\voice_photos\lubancat-0282f88a_cam02\2026-09-24\23-01-53\20260924_230153.jpg`。本地副本及亮度统计：`E:\chat\sender-comparison-20260924\quality-followup\baseline-original.jpg`、`baseline-image-metrics.json`。

## 编码器：实际输出与状态文字需要区分

本次只读核查中，生产进程持续输出约 30 fps、2 Mbps，`rgb_encode_avg_ms` 约 0.25–0.42 ms；编码重置与发送失败的每秒计数为 0。进程 PID 237538 映射已验证的 `libgstrockchipmpp.so`，并持有 `/dev/mpp_service` 句柄。此前 304.04 秒生产录像有 9,128 个可完整解码的 H.264 帧。现有证据不支持“主 H.264 编码器一直未工作”。

可能涉及的不同提示：

- `rgb encoder output lag reset`：此前 22:24:02 启动阶段确有一次 1.475 秒延迟并重置。此次开始查询时 `last_error` 为空，后续查询又出现该字符串；但 22:57–23:08 的 sender 日志没有新增重置，最新日志仍约 30 fps 且重置计数为 0。该状态和当前正常输出可以同时存在，符合旧错误文字由后续心跳重新带入的机制；不能把该字符串单独当作实时停工检测。
- `adaptive_ae=false`：表示可选的软件自动曝光关闭，相机原生 `rgb_ae=1` 仍开启，和编码器开关不同。
- 网页 `RGB 解码错误`：来自浏览器 `VideoDecoder`，需要检查浏览器解码路径。
- `VP8 unsupported`：此前启动日志有这条，但紧接 H.264 MPP 配置成功；不能把 VP8 提示直接用于判断 H.264。
- 心跳中的 `hardware_encoder` 是初始化标志，当前 receiver status 未转发该字段；字段缺失本身不能作为 false。

已询问用户看到的具体原文、位置和时间，回复后按同一时间的输入/输出帧率、包计数、错误日志继续讨论。本轮没有因未明确的提示更换编码器。

## 证据

`E:\chat\sender-comparison-20260924\quality-followup\evidence\` 保存修改前后 ALSA 状态、语音 unit/drop-in、TTS 完成日志、服务健康、sender 日志及原配置；归档为同级 `evidence.tar.gz`。旧版完整录像验收记录仍保留，不能把本次待完成的画质检查记成已经通过。
