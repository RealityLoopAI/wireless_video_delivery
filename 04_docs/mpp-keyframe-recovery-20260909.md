# 录制缺口修复：MPP 重复关键帧请求与六机复验

日期：2026-09-09，北京时间。先区分缺陷修复、实际部署和验证边界。
本报告承接 [TCP/时钟修复](recording-backlog-fix.md) 与
[无线驱动对照](rtl8821cu-recording-recovery-20260909.md)，不是把所有历史缺录归为同一原因。

## 1. 本轮定位到的原因

| 环节 | 证据 | 处理 |
| --- | --- | --- |
| 52d2 无线上行 | 关闭相机后纯 TCP 仍只有约 1.9~4.8Mbps，低于媒体需求；换驱动组合后目标 20Mbps 短测实收 14.94Mbps | 仅该设备定向替换 8821cu 驱动组合，保留原模块，重启验证通过 |
| ab 所在 AP 链路 | 原 AP 上采集约 30FPS，但约 30 秒队列溢出；源 ID 缺口与 sender 丢弃数一致 | 迁到已有的另一 5GHz AP，保留原网络备用，不降低相机档位 |
| 隐藏 TCP 缓存 | 大 Send-Q 掩盖实际积压并拖延尾帧 | 有界未发送缓存和原有应用队列反压，不丢半包、不篡改原帧 |
| 历史时钟 | 同一采集时刻受处理时最新模型过期影响，可能切回未加 offset 的时间 | 按采集时刻选历史模型；holdover 明确标无效，原始列不改 |
| 采集时长统计 | 网络尾帧等待被混入媒体时长 | 分开记录采集跨度和接收跨度，不清除真实 partial |
| 关键帧调度 | 帧绝对时间不能直接作为 GStreamer running-time；旧插件还遗漏实际输出关键帧标志 | 采集侧到期触发立即请求；本轮补齐 MPP 输出 sync-point |

## 2. 为什么此前修复仍未完全通过

主程序 `c7423c61789f` 修正了请求时间和无线重连调优，但六路 120 秒测试
`174353` 中，52d2 起始 RGB 晚约 642ms，b439 晚约 895ms，严格验收仍失败。
只有一次强制 IDR 的测试不足以发现问题，因此增加十次重复请求的硬件回归。

人工帧测试使用固定内容、30FPS、绝对帧 PTS；第 43 帧首次请求，之后每 47 帧请求一次，
避免请求恰好与自然 GOP 对齐。检查实际 H.264 IDR NAL、原 PTS 对应的帧号，以及取到输出的时刻。
超过请求后 3 帧才成为 IDR，或超过 4 帧才取得输出，均失败；未完成请求也失败。
不以日志中出现“已请求”代替真实编码结果。

原系统插件和相同固定源码的无补丁版本均可复现：首次请求生效，后续请求等自然 GOP。
固定源码无补丁的单路测试 10 次请求失败 8 次，双分支失败 9 次。
GStreamer debug 显示实际 IDR 输出的 `sync point` 仍为 0，随后出现
`Not requesting another key unit`。这不是相机不出帧，也不是 NAS 丢包。

## 3. 根因和补丁

GStreamer 基类要从编码插件获知输出帧是否为同步点，才能完成待处理的强制关键帧事件。
旧 `gstmppenc.c` 在 `gst_video_encoder_finish_frame()` 前没有设置该标志，
因此第一次 pending 请求没有正常完成，后续立即请求可能被抑制。
依据为 [GStreamer 1.20.3 源码](https://raw.githubusercontent.com/GStreamer/gstreamer/1.20.3/subprojects/gst-plugins-base/gst-libs/gst/video/gstvideoencoder.c)
与 [官方关键帧事件说明](https://gstreamer.freedesktop.org/documentation/additional/design/keyframe-force.html)，
并由上述真实 MPP 输出测试验证。

补丁从实际输出 MPP packet 的 `KEY_OUTPUT_INTRA` 获取结果，只在确认 intra 输出时设置
`GST_VIDEO_CODEC_FRAME_SET_SYNC_POINT`，否则清除该标志。
不根据“曾请求关键帧”猜测结果，不将 P 帧伪装成 IDR，不修改 PTS、相机曝光、GOP 和主媒体协议。

源码固定在 [Rockchip 插件开发分支](https://github.com/JeffyCN/mirrors/tree/gstreamer-rockchip) 的
`ca829e0dc1d814df01711a5796e7510812af9e85`。最新分支需要现场 MPP 头文件中不存在的
`MppSysCfg`，构建失败后没有部署。采用兼容的固定版本后，对同版本做有/无补丁对照，
而非将整个驱动栈升级产生的效果都归为这几行修改。

## 4. 原生编译与硬件测试

| 设备 | 人工测试输入 | JPEG 单路 / JPEG 双分支 / BGR | 部署 |
| --- | --- | --- | --- |
| rk3588-ubuntu | 1920x1080@30 | 每模式 10/10 请求通过 | 已加载独立插件 |
| orangepi5pro-ab748372 | 1920x1080@30 | 每模式 10/10 通过 | 已加载独立插件 |
| orangepi5pro-b439137c | 1920x1080@30 | 每模式 10/10 通过 | 已加载独立插件 |
| orangepi5pro-fe0f7222 | 1920x1080@30 | 每模式 10/10 通过 | 已加载独立插件 |
| lubancat-52d2ef0c | 1280x800@30 | 每模式 10/10 通过 | 已加载独立插件 |
| lubancat-e8cc0cb3 | 1280x800@30 | 每模式 10/10 通过 | 已加载独立插件 |

**未通过项也保留**：两台 LubanCat 额外的 1920x1080 双分支测试输出落后，
生产 sender 停止后依然未达本测试时限。不能只归咎于同时运行的采集负载，也不能宣称
RK3576 的 1080p 双分支已通过。本次两台实际相机是 1280x800，复测该档位通过，
没有为通过测试而调低生产档位。实际 52d2 为单路 JPEG 编码，e8 包含 RGB 软件翻转路径。

本机通用 sender CTest 10/10 通过。另在本机运行现有 receiver 测试清单返回 30/32，
两项集成测试因 PATH 没有 `ffmpeg` 报错；部分已有用例可能自行跳过媒体子场景，
这一结果不作为完整 receiver 验收。随后在 x86 receiver 上实际补验
`receiver_clock_history_integration`、`receiver_tail_drain_integration`、
`receiver_status_reply_integration`，3/3 通过，耗时约 30.80 秒。
随后还在 x86 receiver 重跑完整现有回归，33/33 通过，耗时约 125.67 秒。
这 33 项运行的是该机已部署版本的测试；本次新增加的验收会话保护另在本机
`deployment_acceptance_integration` 中实际通过，不混成同一套构建版本。

## 5. 部署和版本核验

六台 sender 主程序均为 `c7423c61789f`，receiver 仍为 `90da5c8bcb94`。
本轮新增的是服务专用插件，不因为报告提交产生新的主程序构建号。
逐机核验运行进程 `/proc/<pid>/maps`、插件 SHA256、主程序 SHA256、配置 SHA256。
插件安装期间主程序与配置 SHA256 不变；构建产物各机原生生成，不跨 SDK 复制二进制。

- 独立插件：`/opt/gwv3/plugins/mpp-syncpoint-ca829e0/<sha-prefix>/libgstrockchipmpp.so`。
- systemd drop-in：`/etc/systemd/system/<sender-service>.d/60-mpp-sync-point.conf`。
- 仅该服务设置 `GST_PLUGIN_PATH_1_0`，原 `/usr/lib/.../libgstrockchipmpp.so` 保留。
- root 备份：`/var/lib/gwv3/rollbacks/mpp-syncpoint-20260909/<sender-service>/`。
- 原生构建与测试：各机 `~/wvd-mpp-syncpoint-20260909/manifest.json`。
- 脱敏部署汇总：`08_reports/recording-recovery-20260909/plugin-deployment.json`。

新增 `05_tools/build_patched_mpp_plugin.sh` 只构建，不安装或重启。
必须提供独立、干净、指定提交的源码工作树；依赖按现场版本使用。
`GWV3_TEST_MPP_HARDWARE` 默认 OFF，防止 CI/无 MPP 的 x86 主机误跑硬件测试。
测试宽高可用 `GWV3_TEST_MPP_WIDTH` / `GWV3_TEST_MPP_HEIGHT` 指定，不能冒充生产配置。

```sh
# 在已配置好 SDK/GStreamer 的独立 sender 测试构建目录中启用；不要在录制中跑额外编码压力。
cmake -S . -B TEST_BUILD -DGWV3_TEST_MPP_HARDWARE=ON \
  -DGWV3_TEST_MPP_WIDTH=1280 -DGWV3_TEST_MPP_HEIGHT=800
cmake --build TEST_BUILD --target mpp_keyframe_repeat_test -j1
GST_PLUGIN_PATH_1_0=/opt/gwv3/plugins/mpp-syncpoint-ca829e0/ACTUAL_SHA_PREFIX \
  ctest --test-dir TEST_BUILD -L hardware --output-on-failure
```

## 6. 六路跨 15 分钟切片复验

本轮六台均在线并核验插件后，启动 920 秒实录，覆盖原有 900 秒切片。
每 5 秒观察帧率、丢帧增量、接收延迟、时钟和录制队列。
会话 `1788948524730137`，NAS 目录为 `2026-09-09/180845` 和 `182345`。
12 个分片均 `ready=true`、`recording_quality_status=complete`，逐帧 CSV 检查如下。

| 相机 | 首片 RGB / Depth 帧数 | RGB / Depth 源 ID 缺数 | 超过 500ms 的间隔 | global 倒退 |
| --- | --- | --- | --- | --- |
| 52d2ef0c | 27018 / 27011 | 2 / 0 | 0 | 0 |
| e8cc0cb3 | 27020 / 27015 | 0 / 0 | 0 | 0 |
| ab748372 | 27004 / 27059 | 0 / 0 | 0 | 0 |
| b439137c | 27007 / 27059 | 0 / 0 | 0 | 0 |
| fe0f7222 | 27002 / 27059 | 1 / 0 | 0 | 0 |
| rk3588-ubuntu | 26987 / 27061 | 0 / 0 | 0 | 0 |

所有 RGB CSV 视频索引连续。六机 RGB/Depth 跨片的源 ID 均增加 1，
时间差约 28.17~44.00ms。正常源时钟采集频率与名义 30FPS 有微小差别，
不能只拿帧数除以 27000 判断内部缺帧；同时核对源 ID 和实际采集跨度。

52d2 的一帧在 18:22:34 被拒绝：`frame_id=26043`、139588 字节、
`missing jpeg soi/eoi marker`。另一个源 ID 缺口及 fe0f 的单帧缺口仍需按原始输入追踪，
不能全部归为坏 JPEG。没有通过放宽完整性检查或补重复帧消除这些计数。

无线在中途再次发生抖动：52d2 到 AP 的 5 次 ping 平均约 683ms、最大 1688ms，
e8 平均约 540ms、最大 929ms；当时信号约 -49/-45dBm。
CSV 实测 52d2 RGB 最大到达滞后约 14.34 秒，e8 约 7.48 秒，
但没有转化为秒级采集缺口。**这是缓冲/补传保留了数据，不是无线抖动已经消失。**
52d2 首片 RGB 786 行、Depth 783 行 `clock_sync_valid=0`，采用历史 holdover 估计；
其他本批流没有无效 clock 行。全局时间连续不等于这些区间重新获得校准精度，
下游不可把无效区间当作已验证的 5ms 内容同步数据。

18:24:32 出现另一会话 `1788949472377433` 的开始操作，本测试工具没有发起该次开始。
原探针一直等待“全局 idle”，因此手动终止了**仅等待的探针进程**，没有停止新会话。
最终以原会话的明确 12 个目录只读验收；报告 `validation_kind=offline_session_files`，
`stop_to_delivery_status_ms=null`，不伪造原探针成功退出或精确点击到交付耗时。
从该会话元数据窗口结束到最后 NAS finalized 时间约 28.10 秒，是另一种统计口径。

24 个视频均已用低优先级 ffmpeg 全帧解码完成，无解码错误，解码帧数与 CSV 一致。
证据为 `08_reports/recording-recovery-20260909/mpp-syncpoint-long-decode.json`。
没有修改原录制文件，也没有因为新会话出现就跳过原批次的逐帧验证。

### 验收工具的会话保护

新增两种模拟测试：测试停止前被新会话替换，以及停止后收尾期间有人开始新会话。
旧工具重现等待全局空闲超时；新工具在开始确认、主动停止前、收尾等待和异常清理前，
检查 `recording_session_id`，发现变化便失败退出并保留证据，不向已识别的新会话发送停止。
模拟服务验证停止调用次数和新会话保持运行，修复后测试通过。

这是客户端安全检查，不是服务端原子条件停止：查询和停止请求之间仍有并发窗口。
正式验收仍应约定独占控制；多人并发控制需要未来增加服务端会话条件参数，
本次没有偷偷修改控制协议来假装已解决这个竞态。

## 7. 回退与剩余边界

先结束录制并等待尾帧/交付完成。只撤销该服务的 `60-mpp-sync-point.conf`；
如果 root 备份存在 `dropin.before`，恢复原件而非直接删除。
执行 `systemctl daemon-reload` 并重启对应 sender，再从运行进程 maps 确认加载原插件。
不要删除整个 service drop-in 目录，也不要覆盖系统插件或其他现场配置。
无线驱动回退单独按无线恢复报告执行，不能在唯一 Wi-Fi SSH 通道上直接卸载驱动。

连续录制仍受相机真实输入完整性、无线可用吞吐、设备供电与散热约束。
已出现过独立 JPEG 不完整帧，不能靠补 EOI、重复帧或改时间戳修复真实内容。
`complete` 是现行质量阈值结论，不等于零缺帧；小于 500ms 的缺口也必须统计。
本轮跨片测试不能替代 8 小时或断电/多日漂移验收，更不能恢复历史已经缺失的帧。

另外核查了两台 SDK v2 的采集后端：生产目录旧 `OrbbecSDKConfig_v1.0.xml` 中虽然写着
V4L2，但当前 SDK v2.8.6 使用 `OrbbecSDKConfig.xml`，其 Gemini305 的 Auto 默认是 LibUVC；
运行进程实际打开 `/dev/bus/usb/...`，没有 `/dev/video*` 数据句柄。
52d2 的 `usbfs_memory_mb` 已为 256，并非仍处于很小的缺省缓冲。
[官方性能说明](https://orbbec.github.io/OrbbecSDK_v2/docs/tutorial/performance_tuning.html)
将切换 V4L2 列为可测试选项，但同时说明兼容性与重新插拔要求。
本次没有把这个后端差异直接认定为三帧缺口的根因，也没有未经 RGBD/时间戳实测就全机切换。
后续原始输入对照应使用真正生效的 SDK v2 配置，而不是继续修改这个旧文件。
