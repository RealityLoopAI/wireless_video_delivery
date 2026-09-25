# cam02 局部高亮修复（2026-09-25）

## 问题与实测选择

Gemini 305（CV275610004S，固件 1.0.70）原先已开启原生自动曝光，曝光上限为 10 ms，但 `DEVICE_AE_REFERENCE` 实测为 0（Depth）。接收端 full-range 补标已生效；本次在编码前的原始 JPEG 中仍能重现白纸杯杯沿与杯内阴影消失，不能靠再次修改 MP4 色彩范围解决。

固定相机和室内场景，比较四组，各取 7 张原始 JPEG 和 3 张 848×530 Y16 深度图。只缩短曝光上限到 5 ms 时，原生 AE 将增益原始值从 33 提到 60，高亮改善很小。切换为 Color 测光（reference=1）后，杯沿及杯内明暗分界恢复；再限制为 5 ms，增益读回为 32。

| 测光 / 曝光上限 | 近杯 Y≥230 | 近杯高端众数面积 | 小杯 Y≥230 | 暗椅背 Y 中位数 | 深度非零率 |
|---|---:|---:|---:|---:|---:|
| Depth / 10 ms（原配置） | 89.43% | 68.15% | 76.96% | 51.97 | 87.65% |
| Depth / 5 ms | 85.92% | 60.80% | 75.17% | 49.62 | 86.95% |
| Color / 10 ms | 2.11% | 3.37% | 26.75% | 51.27 | 88.69% |
| Color / 5 ms（本次选择） | 0.67% | 2.63% | 20.86% | 51.41 | 88.08% |

亮度为 JPEG 解码 RGB 的 BT.601 加权码值，表格为逐帧指标的均值；不是线性光强。纯白表面的平台本身不证明传感器饱和，结论同时依据固定杯口阴影/边缘重新可见。深度非零率只用于检验明显退化，三张样本不证明测距精度提高。窗口依然有大片高亮平台，不能声称保住了窗外细节。

原始照片与同场景对照保存在操作机 `E:\chat\sender-comparison-20260925\highlight`；仓库保存配置、固定 ROI、重复帧统计、读回和验收证据，见 [部署快照](../06_configs/deployment/lubancat-0282f88a-highlight-20260925/manifest.json)。

## 部署

- 发送端：192.168.1.91，相机 `lubancat-0282f88a_cam02`。
- `/etc/gwv3/sender.json` 仅将 `color_controls.max_exposure` 从 100 改为 50；该 SDK 的标准 COLOR API 和 RGB metadata 单位为 100 µs，50 对应 5 ms。保留 `auto_exposure=true`。
- 新增独立工具 `05_tools/orbbec_ae_reference_setup.cpp`，每次 systemd 启动发送服务前按明确序列号设置 `DEVICE_AE_REFERENCE=1`，检查支持、范围和独立读回，不开启图像流。
- drop-in：`/etc/systemd/system/gwv3-gemini-sender.service.d/70-color-ae-reference.conf`。使用 `timeout --kill-after=2s 15s`，失败使本次服务启动失败，避免静默运行错误测光设置。
- 工具安装到 `/opt/gwv3/camera-controls/ae-reference-9f3b179f9e53/ae_reference_setup`，动态链接现场 SDK 2.8.6。现有 sender 二进制、MPP 插件、Wi-Fi 配置和 depth zlib 保留。
- 接收端无新增修改。质量阈值保持覆盖率 ≥98%，缺口/跨度差 ≤500 ms。

此 drop-in 保证 **systemd 服务启动**时应用设置；watchdog 内部重启子进程和进程内部相机重连不会重跑 ExecStartPre。已验证 SDK 关闭、重新打开及启动流不重置 reference。未将此结果宣称为相机断电永久保存；单独热拔插相机后应重启发送服务重新应用设置。

## 构建与检查

在发送端使用其 SDK：

```sh
SDK=/home/cat/wireless_video_delivery_new0923/11_third_party/orbbec/linux_arm64/OrbbecSDK_v2.8.6
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread \
  -I"$SDK/include" 05_tools/orbbec_ae_reference_setup.cpp \
  -L"$SDK/lib" -Wl,-rpath,"$SDK/lib" -lOrbbecSDK -o ae_reference_setup
```

原生编译通过。无参数、缺失参数、无效 reference、重复 reference 均在创建 SDK Context 前拒绝；`--help` 成功。不存在的序列号返回非零，未改设备；正确序列号在不开流状态下写入 1 并读回一致。随后独立探针重新打开相机，在开流前和开流后均读回 1。

SDK 的 COLOR 与 DEPTH 控制在此设备标准模式下共享入口。本机实际使用 LibUVC；RGB 帧曝光/增益/AE 元数据来自 RGB 帧自己的 UVC metadata，不是深度 getter 的显示别名。因此同时保存 RGB 成像变化和深度样本，不能仅凭 API 写成功推断画质。

## 正式服务验证与录像

10:07:41 的正式服务 JPEG 与原场景构图一致，近杯 Y≥230 为 0.65%，右杯为 7.58%；暗椅背 Y 中位数为 52.19（原配置 51.97）。启动日志独立读回 reference=1。此后相机在约 10:08:44 被移动，未对新构图套用旧 ROI。

录像 `lubancat-0282f88a_cam02/2026-09-25/100811` 持续 312.057602 秒，接收端 `recording_ready.json` 判为 complete。

| 流 | 有效帧数 | 覆盖率 | 首帧延迟 | 尾帧延迟 | 最大帧间隔 |
|---|---:|---:|---:|---:|---:|
| RGB | 9364 | 100.0243% | 22.985 ms | 32.429 ms | 66.906 ms |
| Depth | 9368 | 100.0670% | 25.380 ms | 30.762 ms | 49.171 ms |

两路采集跨度差 0.728 ms。覆盖率按有效帧数 / 名义 30fps / 录制窗口计算，略超过 100% 来自实际帧周期差异。全部 9364 个 RGB 记录的 AE=1、曝光 raw=50（5 ms），增益 raw=31–42；没有用降低亮度后的图像冒充采集参数生效。

独立 ffprobe 全帧计数与 ffmpeg 严格完整解码均通过：RGB 9364 帧、Depth 9368 帧、音频 15603 帧/包，解码错误输出均为零。RGB 为 H.264 640×480，`color_range=pc`、`color_space=smpte170m`；CSV 录像帧索引连续且与解码帧数一致，相对 PTS 残差为零。音频归档清单、哈希、包数、时钟及解码检查通过。完整验收结果为 `passed=false`，唯一失败项是下述 `rgb.source_ids_contiguous`。

**严格无缺帧检查未全通过：** RGB 源帧号缺失 6750、8367、8782、9126，共 4 个单帧；深度源帧号无缺失、重复或回退。发送端日志在相同帧号标记 `stage=capture_input`，对应时刻没有编码重置或发送失败。可定位为进入编码前已出现的采集输入跳号，但尚未区分 USB、固件、SDK 或采集调度原因；不能据此断言与曝光调整无关。现有 ≥98% / ≤500 ms 质量门槛通过，额外的零源帧缺失要求未通过，没有放宽阈值或掩盖此项。

动态抽样看到了人员走动及相机移动，但没有确认按要求完成手部静止/移动和遮光恢复的配合测试；不据此声称各种光照与运动场景已全部验收。

## 回退

先停止本相机录像并等待收尾，再执行：

```sh
sudo bash -s <<'ROLLBACK'
set -eu
test -r /var/backups/gwv3-highlight-20260925/sender.before.json
systemctl stop gwv3-gemini-sender.service
timeout --kill-after=2s 15s \
  /opt/gwv3/camera-controls/ae-reference-9f3b179f9e53/ae_reference_setup \
  --serial CV275610004S --reference 0
install -m 0644 /var/backups/gwv3-highlight-20260925/sender.before.json /etc/gwv3/sender.json
runuser -u cat -- test -r /etc/gwv3/sender.json
rm /etc/systemd/system/gwv3-gemini-sender.service.d/70-color-ae-reference.conf
systemctl daemon-reload
systemctl start gwv3-gemini-sender.service
ROLLBACK
```

只移除 drop-in 不会主动把仍通电相机的 reference 改回 0，因此回退命令明确包含设置 0。
