# CLOCK_SYNC 模块实现归档

日期：2026-06-29

## 背景

现有系统已经能在单 sender 内保留 RGB/Depth 的帧时间戳、系统时间戳、`frame_id` 和 `pair_id`，但多 sender 数据集生成时缺少统一时间轴。下游需要在 receiver 侧获得可直接用于跨 sender 对齐的 `global_timestamp_us`。

## 本次实现

### sender

新增：

```text
01_sender_linux/include/gwv3_sender/clock_sync_client.hpp
01_sender_linux/src/clock_sync_client.cpp
```

实现：

1. 独立线程通过 UDP 向 receiver 发送 `clock_sync_probe`。
2. 收到 `clock_sync_response` 后记录本机 `t4`。
3. 使用 NTP 四时间戳算法计算 `offset_us` 和 `delay_us`。
4. 过滤异常样本。
5. 平滑 offset 并估计 drift。
6. heartbeat 上报 `clock_sync_valid`、`clock_offset_us`、`clock_delay_us`、`clock_drift_ppm`、`clock_last_sync_us`。

### receiver

新增：

```text
02_receiver_linux/include/gwv3_receiver/clock_sync_manager.hpp
02_receiver_linux/src/clock_sync_manager.cpp
```

实现：

1. 独立线程监听 clock sync UDP 端口。
2. 收到 probe 后记录 receiver `t2` 和 `t3`，立即回包。
3. 从 sender heartbeat 更新每个 `sender_id` 的 clock model。
4. 为每个 media packet 计算并保存 `global_timestamp_us`。
5. `/api/status` 输出 clock sync 状态。
6. `frames.csv` 追加 clock sync 相关字段。

## 关键实现选择

`offset_us` 的定义是 receiver 系统时钟减 sender 系统时钟，因此 `global_timestamp_us` 使用：

```text
packet.system_timestamp_us + offset_us
```

如果 `system_timestamp_us` 缺失，才退回 `packet.timestamp_us`。

这样保留了相机/SDK 原始 `timestamp_us`，同时给下游提供统一 receiver 时间轴。

## 降级策略

1. `clock_sync.enabled=false` 时，系统继续运行。
2. sender 收不到 response 时，采集和传输不受影响，只使用最近有效 offset 或保持无效状态。
3. receiver 没有某个 sender 的有效 clock model 时，不丢 media packet，`clock_sync_valid=false`。
4. clock model 默认 10 秒超时，超时后只标记无效，不影响录制。

## 配置变更

已在 `06_configs` 的 sender 示例配置中加入：

```json
"clock_sync": {
  "enabled": true,
  "receiver_ip": "192.168.66.196",
  "port": 50012,
  "interval_ms": 2000,
  "timeout_ms": 100,
  "max_delay_us": 100000,
  "sample_window": 10
}
```

已在 receiver 示例配置中加入：

```json
"clock_sync": {
  "enabled": true,
  "bind_ip": "0.0.0.0",
  "port": 50012,
  "model_timeout_ms": 10000
}
```

## 验证结果

已完成：

```text
receiver-only CMake build: pass
sender-only CMake build: pass
```

后续现场运行时建议观察：

```text
clock_sync response sent sender_id=xxx sequence=xxx
clock_sync sender_id=xxx offset_us=xxx delay_us=xxx drift_ppm=xxx samples=xxx healthy=true
clock_sync updated sender_id=xxx offset_us=xxx delay_us=xxx drift_ppm=xxx samples=xxx valid=true
```

下游优先使用：

```text
frames.csv global_timestamp_us
```

同时保留以下字段用于排查：

```text
sender_timestamp_us
sender_system_timestamp_us
receiver_receive_timestamp_us
sender_offset_us
sender_delay_us
sender_drift_ppm
```
