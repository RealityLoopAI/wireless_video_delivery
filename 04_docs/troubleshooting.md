# 故障排查手册

更新时间：2026-09-03

本文档把历史问题压缩成按现象排查的步骤。排查时先确认“当前实际运行状态”，不要只看某个服务是否 active。

## 1. 总体排查顺序

先按这个顺序看：

1. 接收端是否启动。
2. Web Monitor 是否可访问。
3. 发送端是否启动。
4. 接收端 `/api/status` 是否看到目标 `sender_id` / `camera_id`。
5. RGB/Depth packet 计数是否增长。
6. 录制目录是否生成文件。
7. `frames.csv` 是否持续写入。
8. 网络、CPU、内存、USB 是否有明显瓶颈。

常用命令：

```bash
./05_tools/status_receiver.sh
./05_tools/status_sender.sh
curl -s http://<receiver_ip>:8080/api/status
```

## 2. Web 页面打不开

可能原因：

1. Web Monitor 没启动。
2. 端口 8080 被占用。
3. 接收端 IP 访问错了。
4. 防火墙或网络隔离。
5. Web 服务启动了，但 C++ 接收端管理 API 不可用。

处理：

```bash
./05_tools/status_receiver.sh
```

重点看：

1. `gwv3-web-monitor.service` 是否运行。
2. `gwv3-gemini-receiver.service` 是否运行。
3. 8080 和 18080 是否监听。
4. Web 日志里是否有 `receiver admin unavailable`。
5. 浏览器访问的是 Web 端口 `8080`，不是仅限本机的 C++ admin 端口 `18080`。

Web Monitor 默认无需登录；不要为了远程访问而把 C++ admin 端口暴露到局域网。

## 3. Web 打开了但没有发送端

可能原因：

1. 发送端没启动。
2. 发送端卡在 Wi-Fi guard、预检或相机启动。
3. UDP 50009 广播被网络隔离，且配置中的兜底 IP/主机名也不可达。

生产 Sender 默认通过 UDP 50009 自动发现 Receiver，并让媒体、状态、预览和 CLOCK_SYNC 共用同一动态目标。`receiver.ip` 只作为发现失败时的兜底；若网络禁止客户端广播或启用了 AP isolation，需要放通同一二层网络内的 UDP 50009，或提供可解析的兜底主机名。发现失败只会进入重连，不会让采集进程异常退出。
4. 状态 UDP 50011 不通。
5. `sender_id` / `camera_id` 改了，页面还在看旧 key。

处理：

在发送端看：

```bash
./05_tools/status_sender.sh
./05_tools/sender_preflight.sh
```

在接收端看：

```bash
curl -s http://<receiver_ip>:8080/api/status
```

注意：systemd active 不代表采集子进程一定已经跑起来。要看 watchdog、预检、相机枚举和日志。

## 4. 有发送端但没有预览

可能原因：

1. 预览目标选错了。
2. RGB 或 Depth packet 没有增长。
3. Web 预览流被关闭或降级。
4. 接收端预览解码失败。
5. 浏览器缓存旧状态。
6. 发送端改过身份，Web 主预览仍指向旧 key。

处理：

1. 刷新页面。
2. 在 `/api/status` 确认目标 `sender_id` / `camera_id`。
3. 检查 RGB/Depth packet 计数和 age。
4. 手动设置主预览目标：

```bash
curl -X POST 'http://<receiver_ip>:8080/api/preview/main-target?sender_id=<sender_id>&camera_id=<camera_id>'
```

如果正式录制文件正常，只是预览异常，优先按预览链路排查，不要直接判断主链路损坏。

## 5. 发送端看似在跑但接收端没有数据

可能原因：

1. Wi-Fi 不在目标频段或连接不稳定。
2. 媒体 TCP 50010 不通。
3. 状态 UDP 50011 通但媒体 TCP 不通。
4. 相机采集线程卡住。
5. 编码器启动失败。
6. 发送端队列被回压压住。

处理：

1. 看 `status_sender.sh` 的实际帧率、码率、send failure。
2. 看发送端日志里的 `media send failed`、`encoder init failed`、`camera disconnected`。
3. 看接收端 `/api/status` 的 packet 和 byte 是否增长。
4. 降低 RGB 码率或关闭不必要预览做对照。
5. 确认 5GHz Wi-Fi 和链路质量。

## 6. 录制失败或没有文件

可能原因：

1. NAS 未挂载、不可写或被错误挂载到普通本地目录。
2. NAS 隐藏写入目录和正式目录不在同一文件系统。
3. 当前写入文件系统达到保留水位。
4. 录制命令没有到达接收端。
5. 相机没有在线。
6. `storage_key` 或文件名前缀包含非法字符。

处理：

1. 检查 `nas_root` 是真实挂载点并且可写；确认 `.gwv3_direct_inprogress` 可创建目录。
2. Web 或 CLI 开始录制后看 `/api/status` 的 recording 状态。
3. 录制中看 `<nas_root>/.gwv3_direct_inprogress/`，交付后看正式相机/date/time 路径。
4. 查看 `ffmpeg.log` 和接收端日志。
5. 看 `/api/status` 的 `recording_state`、`record_accepting`、`record_finalizing`、`record_write_errors`、队列字节数和 finalizer 计数。
6. 当前生产 staging 模式还要检查 `recording_staging.root`、uploader 和 capture queue。

## 7. `rgb.mp4` 打不开或收尾失败

普通 MP4 在异常退出时可能缺少尾部索引。当前生产输出为 fMP4，录制开始即写初始化信息，运行中写 `moof` 分片，正常关闭补 `mfra`；不使用会触发长文件全扫描的 `global_sidx`。这提高异常可恢复性，但仍不意味着断电时最后一个活动分片一定完整。

当前结论：

1. RGB 直接在 NAS 隐藏目录写 fragmented MP4，关闭后检查 `moov+moof+mfra` 并原子发布。
2. `write_debug_h264=false` 时不双写 H.264 调试旁路。
3. 自动切片等待 SPS/PPS/IDR 后切换 writer；旧 writer 后台关闭，不阻塞当前录制。
4. sender 离线超过 `idle_finalize_ms` 时关闭旧分片；重连在首个可解码 RGB 前不发布 Depth-only 小段。
5. 文件带前缀时 ready marker 也带同一前缀，以 `meta.recording_ready_file` 为准。
6. 当前生产 staging 模式使用 uploader/capture queue；只有显式回退到直写配置时才不看这些字段。

排查：

1. 看 `ffmpeg.log`。
2. 看 `meta.json` 的 closed 状态和帧数。
3. 看 `frames.csv` 中 `rgb_recorded=1` 的数量。
4. 检查 NAS 挂载、可用空间和 `.gwv3_direct_inprogress` 中是否有滞留目录。
5. 长时录制后若 `frame_id`、`frame_system_timestamp_us`、`global_timestamp_us` 同时大幅倒退，检查 `media_ingress_superseded_sessions` 和 `media_ingress_stale_packets`。这通常是 sender 重连时新旧 TCP 会话重叠，不是 clock offset 正常漂移。
6. `record_prequeue_peak_delay_ms` 高说明入队前有重处理；`record_queue_peak_wait_ms` 高则优先排查 NAS 或写盘能力。
7. 若多路 prequeue delay 同时跃增，检查 receiver 全局锁内是否发生磁盘 I/O，并核对运行 `build_source_hash`。
8. 若出现少量 Depth-only 小段，查看 `segment_prestart_depth_drops` 和是否在首个可解码 RGB 前错误建段。
9. 若历史 `media TCP connect failed` 仍显示，但媒体 age 很小且 FPS 正常，区分当前状态和未清理历史错误。
10. finalizer 持续数十秒且 `ffprobe` 在读取整段大 fMP4，说明运行的仍是旧版全文件扫描路径；核对组件版本。
11. 最终 fMP4 应有 `moov`、`moof`、尾部 `mfra`，且 ready marker 声明 `rgb_container_format=fragmented_mp4`。
12. 按 uploader 的 pending、active phase、capture queue 与发布日志排查；不得手工伪造 ready marker。

## 8. 帧率低或画面卡

先区分三种帧率：

1. 相机 SDK 输入帧率。
2. 发送端实际发送帧率。
3. 接收端实际录制帧率。

不要只看播放器显示的标称 fps。

录制后分析：

```bash
./05_tools/analyze_segment_fps.py <segment_dir>
```

常见原因：

1. Wi-Fi 带宽不足。
2. RGB 码率过高。
3. Depth 分辨率过高。
4. zlib 压缩耗时过高。
5. TCP 回压。
6. 接收端写 NAS 变慢。
7. Web 预览占用资源。
8. USB 带宽或供电问题。
9. 深度压缩模式和设备性能不匹配。

处理方向：

1. 降低 RGB 码率。
2. 根据数据精度要求选择更合适的 Depth 压缩模式或降低 Depth profile。
3. 关闭或降低 Web RGB 预览。
4. 确认 5GHz 链路质量。
5. 检查接收端 CPU、内存和磁盘写入。
6. 检查发送端 USB 和编码器日志。

## 9. RGB 和 Depth 对不上

可能原因：

1. RGB 和 Depth 本来就是两条独立流。
2. 下游错误假设第 N 个 RGB 等于第 N 个 Depth。
3. 下游没有优先使用 `global_timestamp_us`，也没有在无 clock model 时回退 `frame_system_timestamp_us`。
4. 发送端时间未同步。
5. 双路相机物理连接或 Depth remap 配置错误。

处理：

1. 保留 RGB 和 Depth 各自原始帧号。
2. 按 `global_timestamp_us` 做最近邻匹配；`clock_sync_valid=0` 时回退 `frame_system_timestamp_us`。
3. 输出 pair 时保留 `pair_delta_ms`。
4. 检查 `frames.csv` 的 `rgb_system_timestamp_us`、`depth_system_timestamp_us` 和 `pair_delta_us`。
5. 如果是双路物理视角交叉，检查 `swap_depth_between_cameras`。

## 10. 多相机 RGB 视频不同步

历史根因：

1. 本机预览看起来同步，不代表编码输出和 MP4 文件同步。
2. 旧逻辑可能把当前采集帧时间戳套到编码器输出的旧画面上。
3. MP4 内部时间轴不是跨相机同步依据。

当前处理原则：

1. 用 `global_timestamp_us` 做多路对齐优先字段；`clock_sync_valid=0` 时再用 `frame_system_timestamp_us`。
2. 用 `rgb_video_frame_index` 映射 `rgb.mp4` 解码帧。
3. 不再依赖独立 `rgb_recorded_frames.csv` 作为正式格式。
4. 下游必须等待录制收尾，只读取最终发布的 `frames.csv`。
5. 必须筛选 `stream_type=rgb && rgb_recorded=1`，禁止用 RGB 行号或 `len(rows)` 推算 MP4 帧号。
6. 对齐前运行 `05_tools/sync_input_guard.py`；检查失败时应终止任务，不能降级读取实时 CSV。

若某一路画面稳定提前整数帧，而 `global_timestamp_us` 本身正常，优先比较同步输出中的 `video_index` 与最终 CSV 的 `rgb_video_frame_index`。录制收尾前的 RGB 包日志可能包含尚未进入 MP4 的起始 P 帧，直接按行号编号会把更晚的画面绑定到更早的时间戳。

## 11. Depth 伪彩颜色变化

伪彩 Depth 是预览派生结果，不是正式深度数据。

颜色变化常见原因：

1. 伪彩映射范围变化。
2. 不同相机或不同场景深度范围不同。
3. Web 预览使用的固定/自动范围不同。
4. 浏览器或页面刷新看到的是不同时间点。

判断正式数据是否正常，应看：

1. `depth.mkv` 是否写入。
2. `frames.csv` 的 Depth 帧率和时间戳。
3. `calibration.json` 的 `depth_scale`。
4. 需要时用离线工具读取原始 `uint16` 深度值。

## 12. 相机枚举失败

可能原因：

1. USB 线、接口或供电问题。
2. USB autosuspend。
3. SDK 版本或架构不匹配。
4. Gemini305 必须使用支持该型号的 Orbbec SDK v2；当前 OrangePi 路线使用 SDK 2.8.6 + LibUVC。
5. `device_model`、`serial_number` 或 `uid` 配置不匹配。

处理：

1. 重新插拔相机。
2. 检查 `sender_preflight.sh`。
3. 检查 Orbbec SDK 是否能枚举设备。
4. 检查启动日志中的 `configured_model/device_model`、`configured_serial/device_serial` 和 `uid`。
5. 对 RK3588 USB 相关问题，运行对应 USB 准备脚本。

如果相机能枚举但运行中 RGB/Depth 同时停流：

1. 检查内核日志是否有 `usb_submit_urb returned -19`、`reset SuperSpeed USB device` 或 `USB disconnect`。
2. 检查 SDK 日志是否有 `setXu failed`、`bad magic` 或 `did not claim interface`。
3. 用 `ldd 12_build/bin/gemini_sender` 和 `readlink -f libOrbbecSDK.so.2` 核对真实加载版本，不能只看 SDK 目录名。
4. Gemini305 固件 1.0.70 应配合真实的 SDK 2.8.6；性能日志不应每秒读取 UVC XU 属性。
5. 当前发送端默认在 5 分钟内出现 3 次相机停流时退出子进程，由 `sender_watchdog.sh` 重建整个 SDK context。
6. 若更新 SDK 后仍出现真实 `USB disconnect`，再检查短 USB3 线、其他 USB3 根端口或独立供电 Hub。

单相机设备需要允许同型号相机互换时，保留精确 `device_model` 并删除 `serial_number`/`uid`。同一发送端连接多台同型号相机时不要这样做，应继续严格绑定序列号或稳定 USB UID。

## 13. 时间显示、CLOCK_SYNC 或时间戳不对

先区分：

1. 页面显示时间。
2. 系统时间。
3. `*_timestamp_us` 原始字段。
4. `global_timestamp_us` 统一时间轴字段。

要求：

1. 桌面和 Web 显示北京时间到秒。
2. CSV/API 仍写 Unix epoch microseconds。
3. 发送端和接收端保持时间同步。
4. CLOCK_SYNC 有有效 offset 时，多发送端离线对齐优先使用 `global_timestamp_us`。

检查：

```bash
date
timedatectl
chronyc tracking
curl -s http://<receiver_ip>:8080/api/status
```

如果时间同步异常，先修 chrony / 系统时间，再看 CLOCK_SYNC 模型。若 `clock_sync_valid=false` 或 delay 长期很大，先排查 50012 UDP、网络抖动和 receiver 负载，再判断跨设备对齐。若仅 `clock_report_stale=true`，但 `clock_last_probe_receiver_us` 持续更新且 `clock_probe_count` 增长，说明双向测时仍存活，不应把它误判成模型中断。

当前 CLOCK_SYNC 会过滤高延迟、负延迟、超大 offset 跳变和过期模型，并使用低延迟样本中位数、平滑 offset 和受限 drift。它只能统一软件时间轴，不能保证不同相机同时曝光；内容级偏移仍需离线标定或硬件触发。

## 14. 网络和设备压力怎么看

发送端重点看：

1. RGB sent fps / Mbps。
2. Depth sent fps / Mbps。
3. RGB preview fps / Mbps。
4. send failure。
5. zlib 压缩耗时。
6. 编码耗时。
7. CPU、内存、温度。

接收端重点看：

1. 每路 packet 和 byte 是否增长。
2. 录制帧率。
3. NAS 写入是否阻塞。
4. ffmpeg 是否报错。
5. Web 预览刷新是否拖慢。
6. CPU、内存、磁盘 IO。

网络重点看：

1. 频段是否 5GHz。
2. 信号质量。
3. 实际吞吐。
4. 丢包和重传迹象。
5. 多发送端同时运行时总码率是否接近链路上限。

## 15. 历史问题如何追溯

主线不再保留按日期堆积的报告。需要复核旧问题时使用 `git log --all -- <path>`、`git show <commit>` 和 GitHub 提交记录。结论回写当前文档，证据保留在提交和自动测试中。
