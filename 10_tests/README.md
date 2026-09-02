# Tests

`10_tests` 包含 C++ 单元测试、Python 测试和无需真实相机/NAS 的集成测试。正式发布要求见 [testing-and-release.md](../04_docs/testing-and-release.md)。

## Run

Receiver 测试：

```bash
cmake -S . -B 12_build_test_receiver \
  -DGWV3_BUILD_RECEIVER=ON \
  -DGWV3_BUILD_SENDER=OFF \
  -DBUILD_TESTING=ON
cmake --build 12_build_test_receiver -j2
ctest --test-dir 12_build_test_receiver --output-on-failure
```

Sender 测试需要目标 ARM64 设备及匹配的 Orbbec SDK/GStreamer ABI：

```bash
ORBBEC_SDK_ROOT=/path/to/OrbbecSDK cmake -S . -B 12_build_test_sender \
  -DGWV3_BUILD_RECEIVER=OFF \
  -DGWV3_BUILD_SENDER=ON \
  -DBUILD_TESTING=ON
cmake --build 12_build_test_sender -j2
ctest --test-dir 12_build_test_sender --output-on-failure
```

## Coverage Areas

- sender 配置、曝光控制、关键帧调度和传输恢复。
- receiver 输入校验、录制队列、fMP4/NAS 发布和并发上传。
- CLOCK_SYNC 输入检查与 RGB 同步清单。
- Web 低延迟预览。
- 音频归档、静音审计、语音 TTS、整句转发和拍照链路。
- GPIO 录制按钮、电源按钮和状态 LED。

测试必须使用临时目录和本地端口，不得写生产 NAS、修改生产配置或启停现场服务。需要相机、真实网络或长时间录制的验证属于部署验收，必须在自动测试通过后单独执行并记录运行版本。
