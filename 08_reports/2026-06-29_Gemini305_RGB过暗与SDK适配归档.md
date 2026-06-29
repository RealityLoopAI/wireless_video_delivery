# Gemini305 RGB 过暗与 SDK 适配归档

日期：2026-06-29

## 现象

- 设备：`orangepi5pro-d12a4719`
- 相机：`cam01`，Orbbec Gemini 305，序列号 `CV2R46P00091`
- 配置为手动曝光后，RGB 画面仍明显偏暗。

## 排查结论

1. SDK 曝光/增益可以设置并读回，但对当前画面亮度提升不明显。
   - `exposure=312, gain=80` 时预览灰度均值约 `16-17`。
   - 临时提高到 `exposure=624, gain=120` 后画面亮度变化仍很小。

2. V4L2/UVC 层的 `brightness/gamma` 对实际 RGB 画面有效。
   - 手动执行 `v4l2-ctl -d /dev/video8 --set-ctrl=brightness=40,gamma=400` 后，预览灰度均值提升到约 `84`。

3. Gemini305 需要 sender 链接 Orbbec SDK v2.8.6。
   - `orbbec_depth_probe` 链接 v2.8.6，可识别 Gemini305。
   - `gemini_sender` 链接 v1.10.27 时，`queryDeviceList()` 返回 0，日志表现为 `no Orbbec device found`。
   - 重新使用 `ORBBEC_SDK_ROOT=.../OrbbecSDK_v2.8.6` 配置并编译 sender 后，相机可正常启动。

## 本次修复

- 新增 `05_tools/apply_sender_v4l2_controls.sh`：
  - 从 sender 配置中的 `color_controls.brightness/gamma` 读取 V4L2 控制值。
  - 自动查找 Orbbec/Gemini 的有效 `/dev/video*` 节点。
  - 仅在节点支持目标 control 时应用，失败不影响 sender 主链路。

- 修改 `05_tools/sender_watchdog.sh`：
  - 每次 sender 子进程启动后异步重试应用 V4L2 控制。
  - watchdog 自动重启后也会重新补齐亮度配置。

- 修改 `06_configs/sender_orangepi5pro-d12a4719_gemini305.json`：
  - 保持手动曝光：`auto_exposure=false`
  - 固定 `exposure=312`
  - 固定 `gain=80`
  - 增加 `brightness=40`
  - 增加 `gamma=400`

- 修改 `01_sender_linux/CMakeLists.txt`：
  - 默认优先使用 Orbbec SDK v2.8.6。
  - 若本地没有 v2.8.6，再回退到 v1.10.27。

## 验证结果

- `gemini_sender` 远端链接库：
  - `libOrbbecSDK.so.2 => .../OrbbecSDK_v2.8.6/lib/libOrbbecSDK.so.2`

- 远端 sender 状态：
  - RGB：`1280x800@30`
  - Depth：`320x200@30`
  - `rgb_input_fps` 稳定约 `29.9-30.0`
  - `depth_input_fps` 稳定约 `29.9-30.0`
  - `rgb_exposure=312`
  - `rgb_gain=80`

- V4L2 控制读回：
  - `/dev/video8 brightness=40`
  - `/dev/video8 gamma=400`

- 接收端主预览抓图：
  - 尺寸：`640x400`
  - 灰度均值：`83.3`
  - 灰度中位数：`77`
  - p95：`110`

## 后续注意

- Gemini305 设备不要使用 v1.10.27 SDK 编译 sender。
- `brightness/gamma` 当前是 V4L2/UVC 启动补偿项，不是 Orbbec SDK color property。
- 如果后续更换 Gemini305 的 USB 枚举节点，脚本会自动重新扫描 Orbbec/Gemini video node，不依赖固定 `/dev/video8`。
