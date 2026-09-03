# Gemini Receiver

接收端 C++ 服务负责媒体入口、状态聚合、CLOCK_SYNC、预览派生、可靠录制队列、统一分片和 NAS 原子发布。它是录制状态和数据交付状态的唯一权威；Web Monitor 只是代理和界面。

## Network Boundary

核心入口：

```text
UDP 50009          Receiver discovery
TCP 50010          media packets
UDP 50011          sender status/control
UDP 50012          CLOCK_SYNC
HTTP 127.0.0.1:18080 admin API
```

外部客户端只访问 `8080`。`18080` 必须保留为 loopback，不能直接暴露给采集局域网。

## Recording Boundary

当前生产录制先写 Receiver 本地 staging，再由独立 uploader 向 NAS 增量搬运并原子发布。NAS 暂时不可用时已有录制继续留存在本地；NAS 恢复后自动补传。新录制必须同时满足 NAS 已挂载和本地空间门槛。

`main.cpp` 只保留进程入口；`application.cpp` 负责应用生命周期，私有实现按职责位于 `src/detail/`。CLOCK_SYNC 是独立编译模块。

## Build And Test

```bash
cmake -S . -B 12_build_receiver \
  -DGWV3_BUILD_RECEIVER=ON \
  -DGWV3_BUILD_SENDER=OFF \
  -DBUILD_TESTING=ON
cmake --build 12_build_receiver -j2
ctest --test-dir 12_build_receiver --output-on-failure
```

## Run

```bash
./12_build_receiver/bin/gemini_receiver --config 06_configs/receiver_loop.json
```

运维：

```bash
./05_tools/start_receiver.sh 06_configs/receiver_loop.json
./05_tools/status_receiver.sh 06_configs/receiver_loop.json
./05_tools/stop_receiver.sh 06_configs/receiver_loop.json
```

正式数据只消费带 `recording_ready.json` 的最终目录。活动隐藏目录、网页预览和 admin 状态缓存都不能替代录制完整性验收。

完整说明见 [deployment.md](../04_docs/deployment.md)、[recording-and-nas.md](../04_docs/recording-and-nas.md) 和 [api-reference.md](../04_docs/api-reference.md)。
