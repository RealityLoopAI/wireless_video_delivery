# 接收端 Web RGB 预览 FFmpeg 泄漏与卡顿根治归档

## 现象

`orangepi5pro-d12a4719/cam01` 的网页 RGB 主预览曾连续卡住十余秒。故障期间发送端 RGB/Depth 采集及主媒体发送仍保持约 30 FPS，没有坏帧、编码积压、发送失败或相机超时，因此问题不在相机采集和主录制链路。

## 现场证据

- 2026-07-16 18:23:19 至 18:23:37，接收端因拿不到新预览帧，每秒向 d12a4719 请求一次 RGB 强制关键帧。
- 每次强制关键帧均在约 31-79 ms 内由发送端完成，但网页仍反复重试。
- 接收端进程当时占用约 2.6 GB 内存，达到 3.0 GB memory high 附近，只剩约 337 MB 可用内存。
- receiver cgroup 中累积了 73 个 `ffmpeg` 预览解码进程和 1179 个任务，其中包含大量长期休眠进程和僵尸进程。
- GDB 线程栈确认预览清理线程阻塞在：

```text
RgbPreviewDecoder::stop()
  -> std::thread::join()
  -> 等待旧 reader 线程 2714100
```

- reader 线程则永久阻塞在 `RgbPreviewDecoder::read_loop()` 的管道 `read()`。Linux 中由另一线程关闭描述符不保证唤醒已阻塞的 `read()`；旧实现又依赖 FFmpeg 关闭全部管道写端后产生 EOF，因此清理线程可能永久停止工作。
- 旧实现使用 `pipe()` 后再调用 `fcntl(FD_CLOEXEC)`，并发启动多个预览 decoder 时还存在描述符继承竞态，会增加其他子进程继续持有旧管道端的风险。

## 修复

修改 `02_receiver_linux/src/main.cpp`：

1. 使用 `pipe2(O_CLOEXEC)` 原子创建预览管道；仅在系统不支持时回退到 `pipe()` 加 `FD_CLOEXEC`。
2. 在 `posix_spawn` file actions 中显式关闭完成 `dup2` 后的原始管道端。
3. 将 FFmpeg stdout 读取端设为非阻塞，reader 使用 100 ms 有界 `poll()`，并检查 decoder 的停止标记。
4. `stop()` 不再先关闭 reader 正在使用的 stdout 描述符；先停止写入、终止并回收 FFmpeg、等待 reader 有界退出，最后关闭 stdout。
5. 发现 decoder 已失活或写入失败时立即移入异步清理队列，不再保留带旧 JPEG 的失活 decoder，也不在媒体线程中同步执行旧 decoder 的停止流程。
6. 新增预览 decoder 生命周期回归测试：两台虚拟相机反复切换主预览并持续输入 H.264，验证 JPEG 持续刷新、FFmpeg 子进程最终回收到预期数量，并验证 receiver 可在 8 秒内正常退出。

本次没有改动媒体协议、发送端采集、RGB/Depth 配对、时间戳、录制队列或落盘格式。

## 部署与验证

- 接收端：`fz@192.168.66.196`
- 源码备份：`02_receiver_linux/src/main.cpp.before_preview_cleanup_20260716_1833`
- 二进制备份：`12_build/bin/gemini_receiver.before_preview_cleanup_20260716_1833`
- ARM 本地编译成功，`ctest` 2/2 通过。
- x86 接收端编译成功，`ctest` 2/2 通过，包含新增 decoder 生命周期测试。
- 线上自动轮换五路主画面 100 次后，FFmpeg 数量稳定为 5，没有继续增长。
- d12a4719 连续请求 50 次主 RGB 预览均返回有效 JPEG，大小约 4.3-12.2 KB，预览帧年龄保持在 29-71 ms。
- receiver 新版本完整 restart 耗时 0.65 秒；旧版本因清理线程卡死无法正常退出。
- 重启后接收端内存约 134-145 MB，任务数约 106-132；五路相机全部在线，媒体帧年龄小于约 110 ms，录制写入错误为 0。

## 结论

本次 d12a4719 网页 RGB 卡顿是接收端预览 decoder 生命周期泄漏导致的资源耗尽，不是 d12a4719 相机、RGB 180 度翻转、主媒体网络或录制数据掉帧。修复后 decoder 停止流程有明确时间上限，反复切换预览不会继续累积 FFmpeg 进程。
