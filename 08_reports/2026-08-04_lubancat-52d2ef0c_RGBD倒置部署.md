# lubancat-52d2ef0c RGBD 倒置部署

日期：2026-08-04

## 需求

`lubancat-52d2ef0c` 的 Gemini 305 相机采用倒置安装，方向处理应与 `orangepi5pro-d12a4719` 一致。这里的“相机倒置”同时包括 RGB 和 Depth，不是仅旋转 RGB 预览。

## 配置

正式配置 `06_configs/sender_lubancat-52d2ef0c_gemini305.json` 的 `cam01` 新增：

```json
"rotation_degrees": 180
```

现有 sender 对 Gemini 305 的实现为：

- RGB：软件旋转 180 度，因为该固件接受 Color Rotation 属性但不改变 MJPEG 内容。
- Depth：SDK 硬件旋转 180 度。
- 同一 `rotation_degrees` 同时驱动两路，避免 RGB/Depth 方向不一致。

## 部署与验证

- 设备：`lubancat-52d2ef0c`（192.168.66.74）。
- 远端部署前备份：`/home/cat/deployment_backups/rgb_rotation_20260804_151110`。
- 同步当前 sender 源码及 common protocol 后，在设备本机完成 Release 构建。
- `adaptive_exposure_controller_test`、`sender_config_validation` 均通过。
- 服务日志确认：`degrees=180 color=software depth=hardware`。
- RGB `1280x800@30 MJPG`、Depth `320x200@30`，持续观测均约 29.8..30.0fps。
- 观测窗口内 RGB/Depth 发送失败为 0；启动阶段曾丢弃 1 个损坏 MJPEG，随后各周期计数恢复为 0。
- 接收端 RGB 预览人工复核方向正常；Depth 方向由 SDK 属性回读和启动日志确认。

## 结论

当前设备已经按整路 RGBD 倒置部署并运行，方向配置会随服务重启和相机热插拔重新应用。
