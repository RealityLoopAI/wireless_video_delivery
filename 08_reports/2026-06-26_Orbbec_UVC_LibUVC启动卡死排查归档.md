# Orbbec UVC/LibUVC 启动卡死排查归档

时间：2026-06-26 13:30-13:50

## 问题

本机 RK3588 发送端启动后，`cam01` 长时间 0fps，receiver 端显示 `camera startup pending`。相机可被 Orbbec SDK 枚举，深度档位也能列出，但正式 pipeline 无法出帧。

## 排查结论

问题不是 USB 带宽，也不是 `1280x720 RGB + 1280x800 depth` 档位不支持。真正原因是 SDK 配置使用 `LinuxUVCBackend=LibUVC`，在当前 Orbbec `SV1301S_U3` 设备上会导致 RGB UVC 接口被解绑并卡死。

直接证据：

- `v4l2-ctl` 使用 kernel `uvcvideo` 可正常采集 RGB `1280x720@30 MJPG`。
- 改成 `LinuxUVCBackend=V4L2` 后，SDK color-only 与 RGBD 探针均恢复 30fps。
- 正式 sender 恢复后，receiver 侧 `rk3588-ubuntu_cam01 live=true`。

## 已落地修复

- 固定仓库根目录 SDK 配置为 `V4L2` 后端。
- 启动脚本和 watchdog 启动前自动同步 SDK 配置。
- 启动脚本和 watchdog 启动前自动恢复 Orbbec UVC 的 `uvcvideo` 绑定。
- 避免 `frame_aggregate_mode=disable` 时继续调用 aggregate API。
- 曝光/增益控制后移到 pipeline 成功启动之后。
- 增强 `orbbec_fps_probe`，用于后续复测指定 RGB/depth 档位。

## 当前运行状态

- 本机 `cam01`：`1280x720@30 RGB MJPG`，`1280x800@30 depth Y12`。
- 实测发送：RGB 约 `12 Mbps`，depth 约 `22-23 Mbps`。
- Wi-Fi：5GHz，tx bitrate 约 `433 Mbps`，当前发送流量约 `39 Mbps`。
- CPU：sender 约 `140%`，系统总 idle 约 `70%`。
- `cam02`：未插入，离线属于预期。

## 后续建议

- 如果后续再换 Orbbec 型号，先用 `orbbec_fps_probe` 串行验证 RGB-only 与 RGBD 组合，避免多个探针并行占用 USB。
- 如果需要双路同时恢复，先确认第二路 RGB UVC 是否也正确绑定 `uvcvideo`，再启动 sender。
