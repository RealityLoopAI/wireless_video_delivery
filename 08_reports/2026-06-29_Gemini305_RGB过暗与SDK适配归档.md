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

2. V4L2/UVC 层的 `brightness/gamma` 对实际 RGB 画面有效，可用于快速验证问题方向。
   - 手动执行 `v4l2-ctl -d /dev/video8 --set-ctrl=brightness=40,gamma=400` 后，预览灰度均值提升到约 `84`。
   - 但 `gamma=400` 暗部提升过多，画面会发灰，不适合作为默认值。

3. Gemini305 需要 sender 链接 Orbbec SDK v2.8.6。
   - `orbbec_depth_probe` 链接 v2.8.6，可识别 Gemini305。
   - `gemini_sender` 链接 v1.10.27 时，`queryDeviceList()` 返回 0，日志表现为 `no Orbbec device found`。
   - 重新使用 `ORBBEC_SDK_ROOT=.../OrbbecSDK_v2.8.6` 配置并编译 sender 后，相机可正常启动。

## 本次修复

- sender 正式接入 Orbbec SDK color controls：
  - `auto_white_balance`
  - `white_balance`
  - `brightness`
  - `contrast`
  - `saturation`
  - `gamma`

- 已移除 watchdog 启动后 V4L2 补偿脚本，避免绕开 SDK 造成配置来源不清晰。

- 修改 `06_configs/sender_orangepi5pro-d12a4719_gemini305.json`：
  - 保持手动曝光：`auto_exposure=false`
  - 固定 `exposure=312`
  - 固定 `gain=80`
  - 增加 `brightness=20`
  - 增加 `gamma=300`

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

- SDK 控制读回：
  - `brightness=20`
  - `gamma=300`

- 说明：
  - `brightness=40,gamma=400` 可快速提亮，但画面发灰。
  - 当前默认回落到 `brightness=20,gamma=300`，优先保持对比度和自然观感。

## 后续注意

- Gemini305 设备不要使用 v1.10.27 SDK 编译 sender。
- `brightness/gamma` 已接入 Orbbec SDK color property，不再依赖固定 `/dev/video8`。
