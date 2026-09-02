# Gemini Sender

发送端负责 Orbbec RGB-D 采集、采集时间绑定、RGB H.264 编码、Depth 压缩、媒体发送、状态上报、CLOCK_SYNC、预览、热插拔和恢复。

源码结构：

```text
include/gwv3_sender/        public component interfaces
src/main.cpp                process entry only
src/application.cpp         application lifecycle and implementation assembly
src/detail/runtime.inl      queues, runtime state and arguments
src/detail/camera_support.inl
src/detail/depth_compression.inl
src/detail/preview_control.inl
src/detail/camera_capture.inl
src/detail/hotplug_and_run.inl
```

`src/detail/*.inl` 是同一 application translation unit 的私有实现分区，不是对外头文件。拆分保持原有定义顺序和匿名命名空间，组件源码哈希会覆盖这些文件。

发送端必须在目标 ARM64 设备上使用该设备实际 Orbbec SDK 与 GStreamer/MPP ABI 编译：

```bash
ORBBEC_SDK_ROOT=/path/to/OrbbecSDK cmake -S . -B 12_build_sender \
  -DGWV3_BUILD_RECEIVER=OFF \
  -DGWV3_BUILD_SENDER=ON \
  -DBUILD_TESTING=ON
cmake --build 12_build_sender -j2
ctest --test-dir 12_build_sender --output-on-failure
```

运行：

```bash
./12_build_sender/bin/gemini_sender --config 06_configs/<sender-config>.json
./12_build_sender/bin/gemini_sender --config 06_configs/<sender-config>.json --validate-only
```

设备配置和身份规则见 [configuration.md](../04_docs/configuration.md)，数据链路见 [data-pipeline.md](../04_docs/data-pipeline.md)。
