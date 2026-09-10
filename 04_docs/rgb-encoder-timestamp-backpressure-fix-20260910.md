# RGB 编码时间戳与短时拥塞修复（2026-09-10）

## 结论与证据边界

本轮在实际编码封装类上复现并修复两项软件缺陷，不改相机档位、曝光、TCP 协议、
RGBD 配对或 CSV 字段，也不填充重复帧掩盖缺口。

1. 正式 RGB 路径使用 `leaky=downstream` 和 `appsink drop=true`，短暂停顿会静默丢帧。
   同一测试提交 8 帧，旧版单路 BGR/JPEG 仅返回 3 帧，旧版双输出返回 4 帧。
2. `jpegparse` 接到带帧率的完整 JPEG 后，会从起始时间按固定周期递增输出 PTS。
   原输入是逐帧采集时间，含实际间隔抖动或缺口，不能按理想 30 FPS 重建。
   仅修队列时测试曾得到 8 个输出，但有 4 个 PTS 不再等于输入。

第二项会干扰 `resolve_rgb_encode_timing()` 的最近时间匹配，造成帧身份和内容错配、
未匹配项移除及后续丢帧保护。`rgb_encode_unmatched_inputs` **不是硬件编码器丢帧计数**：
它既可能来自真正的输出缺失，也可能来自输出 PTS 被重写。
此前 52d2 frame 1402、fe0f frame 6513 的日志只确认“没有匹配”，
不能据此断言对应图像没有编码出来。历史日志不足以给每一个缺口单独定案。

## 官方实现核查

两台问题设备的 `gst-inspect-1.0 jpegparse` 均显示 GStreamer 1.20.3。
同版本的 `gst_jpeg_parse_pre_push_frame()` 将输出时间戳赋为 `next_ts`，
然后按 `duration` 递增；已有 `next_ts` 时不会逐帧重新读取新的输入采集时间。

- [GStreamer 1.20.3 jpegparse 源码](https://github.com/GStreamer/gstreamer/blob/1.20.3/subprojects/gst-plugins-bad/gst/jpegformat/gstjpegparse.c)
- [queue 官方说明](https://gstreamer.freedesktop.org/documentation/coreelements/queue.html)
- [appsrc 官方说明](https://gstreamer.freedesktop.org/documentation/app/appsrc.html)
- [appsink 官方说明](https://gstreamer.freedesktop.org/documentation/app/appsink.html)

这些源码与实验共同证明软件风险，不等于已证明全部历史 USB、SDK 或网络缺帧的原因。

## 修改内容

### 完整 JPEG 直接解码

SDK/V4L2 已经按帧交付完整 JPEG；去除 `jpegparse`，明确设置
`image/jpeg,parsed=true,width=...,height=...,framerate=...`，直接交给 `mppjpegdec`/`jpegdec`。
维持输入 PTS、H.264 AU 打包与输出匹配逻辑。既有 JPEG 首尾及可选解码校验不放宽。
测试输入包括末尾零填充；不是将未校验的任意字节流宣称为完整 JPEG。

### 正式流与预览分开设置缓冲

- 正式输入 queue：4 帧，`leaky=no`，字节和时间维度的隐含限制显式关闭。
- 双输出的正式分支 queue：4 帧，`leaky=no`。
- 正式 appsink：8 帧，`drop=false`。
- 独立预览明确传入 `GstH264QueuePolicy::Preview`；其队列和 appsink 仍允许丢旧帧。
- 双输出预览仍独立使用可丢帧队列，不因预览拥塞阻塞正式分支。

### 避免保帧后死锁或无限增长

`encode_*()` 在同一调用线程推输入、取输出，因此不能简单将所有元素改成阻塞。
appsrc 保持 `block=false`，应用在入队前检查容量：不超过 8 个排队 buffer、16 MiB，
单帧也不超过 16 MiB。旧 GStreamer 无帧数查询属性时保留字节上限。
满时先取已编码输出释放压力，最多等待 100ms；超时明确抛出“input not submitted”。
不隐式丢弃已接收输入，等待期间已取出的输出保留至后续成功调用。

上述容量是队列上限，不是固定增加的延迟；空闲时照常立即处理。
持续低于采集速度或硬件卡死时仍会报错，不能用有限内存保证任意长拥塞零丢帧。
原有 500ms 编码滞后恢复保护及外层看门狗没有移除。

## 自动验证

新增 `10_tests/test_gst_encoder_backpressure.cpp`，调用生产编码类，
用 pad gate 确定性模拟暂停，不依赖随机 CPU 压力。

覆盖单路 BGR、单路 JPEG、双输出三条路径，各有四种场景：

- 短时暂停后完整返回全部输入 PTS；
- 输入时间含抖动及 100ms 间断，输出仍保留逐帧原时间；
- 持续拥塞触发有界超时，已接收帧仍可排空；
- 独立预览可以丢帧，双输出预览暂停时正式流仍完整。

本机完整测试 23/23，两台不同 SDK 的原机构建也各通过 23/23。
本机 ASan/UBSan 编码专项 12/12 通过（未做泄漏检测）。
52d2、fe0f 各通过 12 项 MPP 回归及生产分辨率的关键帧压力测试；
其余四台原机分别通过 BGR/JPEG 硬件 PTS 测试。六台均已运行 `8c174858d674`。

第一轮六路 300 秒录制：12 个 RGB/Depth 视频完整解码、帧数与 CSV 一致，
内部源帧号连续、无全局时间回退，但 52d2/e8 深度缺头，**整体验收不通过**。
进一步定位到接收端等待首个 RGB 时丢深度，修复与复测见
[接收端深度缺头修复](recording-prestart-depth-fix-20260910.md)。
原始测试、失败记录、部署散列及录制分析在 `08_reports/rgb-encoder-fix-20260910/`。

## 未解决事项与回退

少量原始 JPEG 被完整性检查拒绝的问题仍需保存实际坏载荷定位，不能通过放宽校验修复。
无硬件同步的相机也不会因为保留 PTS 就达到曝光内容级硬同步。

上线前确认 Receiver 空闲，保留目标机原二进制及 MPP 插件覆盖配置；按 SDK/架构原生构建，
复用同运行环境候选时仍逐台做硬件测试，确认上报 commit、RGBD 帧率和时钟模型。
异常时恢复本轮二进制备份，重启对应服务。
不要更改原相机身份、USB 后端或用旧 checkout 的 HEAD 冒充运行二进制版本。
