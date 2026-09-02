# Gemini Receiver

接收端 C++ 服务负责媒体入口、状态聚合、CLOCK_SYNC、预览派生、录制队列、分片和 NAS 原子发布。完整说明见 [deployment.md](../04_docs/deployment.md)、[recording-and-nas.md](../04_docs/recording-and-nas.md) 和 [api-reference.md](../04_docs/api-reference.md)。

核心入口：

```text
TCP 50010          media packets
UDP 50011          sender status/control
UDP 50012          CLOCK_SYNC
HTTP 127.0.0.1:18080 admin API
```

当前生产录制直接写 `<nas_root>/.gwv3_direct_inprogress`，关闭完整 fMP4/FFV1/CSV 后在同一 NAS 文件系统原子发布。`recording_staging.enabled=true` 时才启用本地 staging 和独立 uploader 回退链路。

`main.cpp` 只保留进程入口；`application.cpp` 负责应用生命周期，私有实现按职责位于 `src/detail/`。CLOCK_SYNC 是独立编译模块。

构建：

```bash
cmake -S . -B 12_build_receiver \
  -DGWV3_BUILD_RECEIVER=ON \
  -DGWV3_BUILD_SENDER=OFF \
  -DBUILD_TESTING=ON
cmake --build 12_build_receiver -j2
ctest --test-dir 12_build_receiver --output-on-failure
```

运行：

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
