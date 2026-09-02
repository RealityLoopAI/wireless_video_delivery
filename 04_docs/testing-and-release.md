# Testing And Release

更新时间：2026-09-02

## Branch Policy

`main` 是唯一长期分支和生产基线。功能开发使用短期分支或独立 worktree，测试通过后 fast-forward 或合并进入 `main`，随后删除临时分支。设备专用长期分支、报告分支和归档分支不再保留。

生产设备只部署已推送 commit。禁止直接从 dirty worktree 构建并长期运行。

## Build

Receiver：

```bash
cmake -S . -B 12_build_receiver \
  -DGWV3_BUILD_RECEIVER=ON \
  -DGWV3_BUILD_SENDER=OFF \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build 12_build_receiver -j2
```

Sender 在目标 ARM64 设备本机编译：

```bash
ORBBEC_SDK_ROOT=/path/to/OrbbecSDK cmake -S . -B 12_build_sender \
  -DGWV3_BUILD_RECEIVER=OFF \
  -DGWV3_BUILD_SENDER=ON \
  -DBUILD_TESTING=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build 12_build_sender -j2
```

OrangePi、LubanCat 和本机可能使用不同 Orbbec SDK 与 GStreamer ABI，不能从另一架构复制二进制代替本机编译。

## Automated Tests

```bash
ctest --test-dir 12_build_receiver --output-on-failure
ctest --test-dir 12_build_sender --output-on-failure
```

重点覆盖：

- sender 配置、曝光控制、关键帧和媒体恢复 guard；
- receiver 协议 hardening、录制队列、direct NAS、staging uploader 与故障注入；
- Web 低延迟预览；
- CLOCK_SYNC 输入与 RGB sync manifest；
- 语音拍照、TTS、音频采集恢复、全天归档、GPIO 按键和 LED。

测试脚本不得连接生产端口、停止生产 service 或写正式 NAS 目录。并行测试使用临时目录、随机端口和模拟 sender。

## Pre-Deployment Gate

1. 工作区无非预期改动。
2. 双端目标编译通过，所有 CTest 通过。
3. 所有 JSON 配置可解析，sender 配置验证通过。
4. 不包含密码、令牌、大媒体、SDK 二进制或运行日志。
5. 文档链接有效，接口和配置变更已同步更新。
6. `main` 已推送且 GitHub CI/检查没有失败。

## Device Deployment

对每台设备分别：

1. 拉取同一个 `main` commit。
2. 使用该设备本地 SDK 和插件重新构建。
3. 验证 service 引用的配置文件与预期一致。
4. 验证 `sender_id`、`camera_id`、receiver 地址和端口。
5. 重启服务并核对 status 中的 commit、source hash、dirty=false。
6. 观察至少 2 分钟 FPS、码率、媒体 age、Send-Q、温度和重连计数。

接收端部署时先停止新录制请求，等待当前录制完成和 finalizer 归零，再替换二进制。发送端分批重启，避免所有路同时离线。

## Recording Acceptance

短测至少覆盖：

- 全部开始、统一窗口、全部停止、立即再次开始；
- 自动 15 分钟切片边界；
- sender 重连后恢复到当前 session；
- NAS 短时抖动、receiver 退出与恢复；
- 每路 RGB/Depth 解码、seek、帧率、CSV 索引和 ready marker。

长测基线为目标路数连续至少 2 小时；承诺 8 小时连续录制前应实际完成 8 小时压力测试。测试中定期记录 receiver 队列、NAS 延迟、TCP 重传、sender FPS、媒体 age、温度和 CLOCK_SYNC 状态。

任何会导致媒体协议、分片、收尾或队列语义变化的修复，都必须从头重新计时长测，不能把修复前后的时间拼成一次通过。

## Release Evidence

发布提交应包含：

- 改动目的和兼容性说明；
- 执行过的构建/测试命令和结果；
- 未测试的现场条件；
- 是否需要重启、配置迁移或设备分批部署。

运行状态中的版本字段：

```text
build_commit
build_dirty
build_source_hash
```

组件源码哈希包含 `.cpp`、`.h`、`.hpp`、`.inl` 和 CMake 文件。仅 commit 相同但 source hash 不同，说明运行二进制并非同一源码构建。

## Rollback

回退到上一个已验证 `main` commit 后，仍必须在目标架构本机重编译。配置或落盘格式已经迁移时，先确认旧版本能读取当前状态文件和隐藏 in-progress 目录；否则保留新版本做恢复工具，不要直接删除现场数据。
