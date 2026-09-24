# LubanCat 0282f88a 画质修复实施记录

设备为发送端 `192.168.1.91` / `lubancat-0282f88a_cam02`，接收端 `192.168.1.196`。本记录区分已经部署的改动与尚未完成的硬件验收；不能将下面的曝光参数和 MPP 构建产物视为已部署。

## 已部署：接收端完整亮度范围

在 `/etc/gwv3/receiver.json` 的 `rgb_h264_full_range_camera_keys` 中追加 `lubancat-0282f88a_cam02`，其余配置值不变。原有相机 `orangepi5pro-d12a4719_cam01` 保留。修改前确认所有相机无录制、待开始、尾帧收集或分段收尾，且无待交付数据；原子替换配置后重启用户服务 `gwv3-gemini-receiver.service`。

API `/api/config` 已读回两个相机键，目标相机 `/api/status` 的 `rgb_h264_full_range=true`。接收、音频归档和 Web 服务均为 active，音频发送端白名单和 900 秒切片设置保留。

- 原配置 SHA256：`4a4c99db3ea6ce01db08b91fd00733691378587a0f705af152ddc3a68ce38d3c`。
- 新配置 SHA256：`147eb4a4e0eec5a76fd2e302787e39e3f8fa3b81a982e3b653f9fa6050baf2bc`。
- 接收端备份：`/var/backups/gwv3-quality-20260924/receiver.before.json`。
- 新配置快照：[receiver.json](../06_configs/deployment/lubancat-0282f88a-quality-20260924/receiver.json)。该文件是现场记录；迁移时按相机键合并，不整份覆盖其他现场。

正式处理链为 `h264_metadata=video_full_range_flag=1:matrix_coefficients=6`，录像仍采用 `-copyts -c:v copy`。离线复核 `210354` 和 `205145` 两段原录像，分别为 670 帧和 2359 帧：全部帧/包时间信息、容器时长及逐帧原始 YUV 哈希不变，完整解码无错误。三张原始 JPEG 的对照 MSE 为 `142.185→20.944`、`129.780→14.276`、`128.040→17.888`。补标副本只写本地诊断目录，NAS 原件 SHA256 前后一致。

## 已确认、待实机部署：曝光上限

相机原生自动曝光已开启。Gemini 305 固件为 1.0.70，SDK 为 2.8.6；AE 最长曝光属性支持读写。官方 v2.8.6 提交 `63994096ce6a0c07a235500802ce5379490404b7` 的 `G305ColorAePropertyAccessor` 对彩色曝光及 AE 上限执行写入乘 100、独立读取除 100。因此标准 `OB_SENSOR_COLOR` 模式下，10 ms 目标对应 `color_controls.max_exposure=100`，保留 `auto_exposure=true`。

`getIntPropertyRange()` 只换算 `max/def`，未换算 `cur/min/step`；探针输出 `cur=30158, def=301` 是混合单位结构，不能据此把设置值改成 10000。Gemini 305 的彩色 SCR 元数据单位为 100 微秒，历史曝光元数据 301 对应约 30.1 ms。仓库现有字段 `rgb_exposure_us` 仍保存原始元数据，本次没有改传输协议或 CSV 字段语义。

部署前还必须独立读取 `getIntProperty()`，设置后核对读回值及逐帧曝光元数据，并做静止/运动场景对照；若不支持或不生效，保留原配置并报告。当前尚未写入此参数。

源码依据：[G305PropertyAccessors.cpp](https://github.com/orbbec/OrbbecSDK_v2/blob/63994096ce6a0c07a235500802ce5379490404b7/src/device/gemini305/G305PropertyAccessors.cpp#L219-L278)、[G305MetadataParser.hpp](https://github.com/orbbec/OrbbecSDK_v2/blob/63994096ce6a0c07a235500802ce5379490404b7/src/device/gemini305/G305MetadataParser.hpp#L270-L293)。

## MPP 准备与当前协调阻塞

在发送端 `/home/cat/wvd-mpp-syncpoint-20260924` 隔离构建了固定公开源码 `ca829e0dc1d814df01711a5796e7510812af9e85` 的无补丁和补丁版本，以及 640×480@30 的重复关键帧测试。未替换系统插件，也未添加服务插件路径。

| 插件 | SHA256 |
|---|---|
| 原系统 | `6070c2dd8cc8ad2db8209ffafa1d5a902b70580e537dd27e2a6780efd30b8f1a` |
| 固定源码无补丁 | `bf09e24a73a75d0f6e014bb20752d5ce76b063056eb01241b5f66cae0c35907f` |
| 固定源码补丁 | `d73a4ec49869c4e1f3553e1ccc7096cf825c9ee708629807976c08901d6ff054` |

本任务停止发送服务以安排测试后，该服务在 21:55:24 被另一操作启动；同时发现非本任务的 `/var/tmp/cam02-complete-20260924/source` 编译进程。因此已向用户请求协调独占窗口，暂停发送端硬件测试和部署。一轮生产服务 active 时的原插件观察为 10 次请求中 8 次失败，不作为独占性能验收；补丁尚未通过本设备硬件验收。

恢复实施时，先重新读取设备的主程序、配置和服务 drop-in，保留其他操作的有效改动。核查时发送配置 SHA256 为 `03e6681b39dd7b3b88f073adfbc93754546669c81d2148f249e4cb4db1b2352d`，深度压缩为 `zlib`，已有 `90-cam02-wifi-priority.conf`；不得用早期快照覆盖这些设置。

## 验证状态与回退

当前七组 Linux 软件回归通过：电源 sender 范围、电源按键、录制按键、LED、语音拍照、TTS 和音频混音。测试在接收端隔离目录运行，模拟设备接口，不代表本轮已重新完成真实语音唤醒或物理按键操作。

多次短录制、至少 20 分钟跨切片实录、RGB/depth/audio 全帧校验及曝光场景对照尚未执行。完整性要求仍为两路覆盖率至少 98%，首尾缺口及内部间隔不超过 500 ms，两路采集跨度差不超过 500 ms；未放宽阈值。

接收端回退：重新核对空闲状态与当前配置差异，仅撤销本次新增相机键；若配置从部署后没有其他变化，也可恢复上述 root 备份，再重启该用户接收服务。不要恢复整个旧部署目录或覆盖之后其他人的改动。历史录像原件未修改，发送端目前无本任务已部署的新曝光参数或插件需要回退。
