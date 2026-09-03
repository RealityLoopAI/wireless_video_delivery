# Gemini Sender

发送端负责 Orbbec RGB-D 采集、采集时间绑定、RGB H.264 编码、Depth 压缩、媒体发送、状态上报、CLOCK_SYNC、预览、热插拔和恢复。它不写 NAS，也不决定 receiver 的录制窗口。

## Source Layout

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

## Build And Test

发送端必须在目标 ARM64 设备上使用该设备实际 Orbbec SDK 与 GStreamer/MPP ABI 编译：

```bash
ORBBEC_SDK_ROOT=/path/to/OrbbecSDK cmake -S . -B 12_build_sender \
  -DGWV3_BUILD_RECEIVER=OFF \
  -DGWV3_BUILD_SENDER=ON \
  -DBUILD_TESTING=ON
cmake --build 12_build_sender -j2
ctest --test-dir 12_build_sender --output-on-failure
```

## Run

```bash
./12_build_sender/bin/gemini_sender --config 06_configs/<sender-config>.json
./12_build_sender/bin/gemini_sender --config 06_configs/<sender-config>.json --validate-only
```

长期运行使用仓库根目录脚本；脚本要求 `12_build/bin/gemini_sender` 已在本机生成：

```bash
./05_tools/start_sender.sh 06_configs/<sender-config>.json
./05_tools/status_sender.sh 06_configs/<sender-config>.json
./05_tools/stop_sender.sh
```

## Operational Contract

- 帧系统时间在采集回调附近绑定，编码和网络线程不能重新取时间替换它。
- 采集、编码、Depth 压缩、主媒体和预览使用有界队列；预览允许丢旧帧，录制主链路优先。
- 相机或网络恢复由 watchdog/hotplug 状态机处理，不能因 CLOCK_SYNC、预览或语音应用失败阻塞采集。
- Receiver 默认通过 UDP 50009 自动发现；媒体、状态、预览和 CLOCK_SYNC 共享同一个动态目标，固定 `receiver.ip` 仅作兜底。
- 多设备部署必须保持唯一 `sender_id`，多相机设备还必须保持稳定 `camera_id` 映射。

设备配置和身份规则见 [configuration.md](../04_docs/configuration.md)，数据链路见 [data-pipeline.md](../04_docs/data-pipeline.md)。
