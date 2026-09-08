# 停止尾帧修复与 52d2 网络排查

日期：2026-09-08，UTC+08:00。

## 已确认的两个问题

1. `b031e8ee2aed` 的停止入口立即关闭录制接收开关，只等待 Receiver 内部写盘队列。
   采集于停止之前、停止之后才到达的帧会漏出刚才的任务。已用隔离 TCP fixture 复现。
2. `lubancat-52d2ef0c` 有独立的接收前积压。历史 `155420/160920` 的会话替换、
   采集时间、入队时间及 CSV 缺口见 [原始诊断](recording-gap-recovery.md#10-52d2ef0c-午后批次复核与停止缺尾风险)。
   停止修复不能解释或恢复连续录制时已经丢失的 88/111 秒缺口。

## 代码改动

- 新增可独立单测的 `RecordingTailDrain`，保存固定结束时间、每流进度和单调时钟期限。
- all/sender/camera 三种停止范围均进入有界收尾；首个录制包尚未到达时也保留原窗口。
- RGB 与 Depth 分别越过结束时间才结束等待；预览、心跳、空写盘队列不能代替该判据。
- 将旧任务 session、文件前缀和时间窗单独冻结，避免立即重开把尾帧误归到新任务。
- 截止后主帧只推进进度，不写入旧任务；超前接收时钟超过 500ms 的异常时间不推进进度。
- 迟到帧和已排队帧仍按采集时间正常切片，不因点击停止而禁用轮转。
- 等待超时、进程退出、存储故障均能退出等待；未收齐水位时向质量原因加入 `tail drain`。
- 新增状态字段用于区分等网络尾帧与后续封装/NAS 上传。

线程约束：尾帧状态由 Receiver 主互斥锁保护；后台 close worker 等待时不持锁，
停止 API 和媒体接收线程不睡眠。最终关闭录制入口后，仍按原逻辑等已接纳的写盘任务完成。

没有改动 TCP 封装、编码器、相机档位/曝光、CSV 原字段、时间戳语义或 NAS 发布契约。

## 行为边界

- 默认最多等 `recording_stop_drain_timeout_ms=120000`，不是每次固定增加两分钟。
- 新开始可以排队，但同一相机的实际新任务要等旧尾帧排空和 writer 分离，不承诺无间隙重开。
- 判据依赖每路 TCP 内的有序帧及有效采集时间。越过停止边界不等于所有历史帧都曾到达。
- 缺帧质量检查仍然保留；只有 `recording_ready.json` 的最终目录才可交付，ready 不代表 complete。
- 持续带宽不足、有限队列溢出、断电和真实 TCP 失败仍可能丢帧。
  本补丁没有实现 sender 持久缓存、逐帧落盘 ACK 或断电重放。

## 测试与部署验收

自动测试包括：三种停止范围、RGB/Depth 不同延迟、重复停止、立即重开、缺 Depth 超时、
RGB-only、首帧在途、等待中退出。使用生成 H.264、独立端口和临时 NAS 目录，核对最终 CSV
帧号、结束边界、任务隔离以及 `partial` 原因，不用平均帧率替代逐帧检查。

运行方式：

```bash
cmake --build <receiver-build> -j2
ctest --test-dir <receiver-build> --output-on-failure
```

Receiver 的网络发现测试需允许广播；隔离网络 namespace 时除启用 loopback，还需配置
带广播地址的测试 IPv4 接口。不能把因测试网络禁止广播而失败的用例删除或跳过。

现场应先确认没有录制，再备份生产二进制及配置，原生 x86 构建后原子替换并重启 Receiver。
本次模块只在 Receiver 生效，Sender 保持已部署的 `b031e8ee2aed` 续传版本，不重复重启无关节点。
重启后核对运行 commit、六路实际收包、CLOCK_SYNC、NAS 队列及一次短录制的最终 CSV。

## 网络诊断注意事项

52d2 本次实查相机为 USB 5000M，RGB 1280x800@30、Depth 320x200@30；
CPU 温度约 53~54.5°C，可用内存约 7.3GB，采集约 30fps、RGB 编码小于 1ms。
Wi-Fi 是 `rtw_8821cu`，省电已关闭，USB autosuspend 已关闭，不能把重复关闭省电描述为修复。

原 TP-LINK AP 下，RGB TCP Send-Q 约 65.8MB，Depth 约 6.3MB；
到接收端 ping 平均 184.7ms，到本 AP 平均 147.3ms。Receiver 收包队列基本为空，CPU 未满。
换 `666666` 5GHz 后初始 ping 平均 4.6ms，但持续媒体负载再次恶化，
120 秒实测仅约 1.27 RGB / 3.36 Depth packets/s，RGB 帧龄达到约 296 秒。
因此这个 AP 对照未通过，不能只报最开始的低 ping。

停止 sender 后还发现 FIN-WAIT-1 中约 40MB + 63.5MB 未发送数据；
当时独立 iperf3 限速 16Mbps 测试仅收到约 0.122Mbps。这个结果带有残留传输竞争，
不能当作干净空载的 AP 容量测试。当前内核不支持试用的 `fq_codel`，没有安装或持久化未知驱动。

Linux 官方资料指出无线设备在驱动和硬件中还有聚合队列，因此信号强、PHY 速率高、
应用发送成功均不能证明端到端低延迟：[Wireless device queues](https://wireless.docs.kernel.org/en/latest/en/developers/bufferbloat.html)。

## 回退

只在无录制时操作：恢复备份的 Receiver 二进制并重启原服务；配置原文件保持不变。
也可显式将 `recording_stop_drain_timeout_ms` 设为 `0`，但这会恢复已确认的停止漏尾风险。
网络试验按设备备份的 NetworkManager 配置回退，不删除其他已存 Wi-Fi 或改变 DHCP 地址分配。

本文测试与现场结果将在实际发布、短录完成后补充，不能将上述验收步骤当作已完成结果。
