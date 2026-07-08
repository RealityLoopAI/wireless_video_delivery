# 2026-07-07 clock_sync 重启恢复修复

## 现象

`orangepi5pro-d12a4719` 重启后媒体链路恢复正常，RGB/Depth 包持续以约 30fps 增长，但 receiver 状态中的 `clock_offset_us` 长时间停在 `-572178us`，`clock_drift_ppm` 约 `8284.97`。

## 原因

设备刚重启时 chrony 尚未完全收敛，clock_sync 客户端先接受了一次错误 offset。随后系统时间恢复正常，新样本和旧 offset 差值超过 50ms，被“offset 跳变过滤”持续拒绝。

同时 heartbeat 原先直接上报 `state.valid`，即使 clock_sync 已经超时没有新样本，receiver 仍会继续把旧 offset 视为有效，并用于 `global_timestamp_us`。

## 修复

- sender heartbeat 改为只有 `clock_sync->healthy()` 且状态有效时才上报 `clock_sync_valid=true`。
- clock_sync 客户端在旧同步样本过期或本机系统时间发生回拨/跳变后，允许接受新的大 offset 样本并重置样本窗口，避免永久卡在启动早期的坏 offset。

## 影响

该改动不影响 RGB/Depth 采集、编码、媒体 TCP 主链路和录制格式。clock_sync 不健康时 receiver 会降级为 `clock_sync_valid=false`，避免继续使用过期 offset 生成错误的全局时间轴。
