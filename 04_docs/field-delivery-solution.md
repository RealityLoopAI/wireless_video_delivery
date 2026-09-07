# 现场交付方案与问题答复

更新时间：2026-09-07  
适用版本：`field-v2026.09.07.5` 及对应 `main` 版本

本文回答现场交付前已经提出的关键问题：设备换到新网络后如何工作、IP 是否需要固定、能否一键部署、Sender 如何找到 Receiver、NAS 如何恢复、断网和断电后会发生什么。

本文讲清方案和边界，不代替现场操作步骤。现场开关机、预览和录制请直接看 [operator-manual.md](operator-manual.md)。

## 1. 先给结论

| 问题 | 当前答案 |
| --- | --- |
| 现场需要固定 IP 吗 | 不需要。Sender、Receiver 和 NAS 都可以使用 DHCP。 |
| Receiver 地址改变后要逐台改 Sender 吗 | 不需要。Sender 会通过 UDP 自动发现 Receiver，配置里的地址只是兜底。 |
| Sender 能否做到开箱即用 | 可以，但必须在出厂前装好系统、项目、相机 SDK、配置和自启动服务，并预存现场 Wi-Fi。 |
| 一台全新的空设备能否零操作自动部署 | 不能。空设备至少需要先安装批准的 Ubuntu 镜像、连接网络，然后运行一次部署脚本。 |
| 现场只准备 Wi-Fi 可以吗 | 对已经初始化的 Sender 可以；如果 Wi-Fi 没有提前写入，现场人员仍需先连接 Wi-Fi。 |
| Receiver 和 NAS 是否只插网线即可 | 出厂初始化完成后可以。首次初始化 Receiver 时仍需录入一次 NAS 的 SMB 地址、share、账号和密码。 |
| NAS 地址改变后 Receiver 能否恢复 | 可以。Receiver 会使用已记录目标、MAC 扫描或可选 beacon 重新定位并挂载。 |
| NAS 断线时会丢掉已经录到 Receiver 的数据吗 | 不会立即丢失。数据保留在 Receiver 本地 staging，NAS 恢复后自动补传。 |
| NAS 断线时能否继续开始新录制 | 默认不能。系统会阻止新录制，避免本地磁盘被无边界占满。 |
| 录制中 NAS 断线怎么办 | 当前录制继续写 Receiver 本地盘；本地空间达到保护线时会安全停止。 |
| 断电恢复后会自动继续录像吗 | 不会。服务会恢复并补传断电前已经落地的数据，但必须由人员重新开始录像。 |
| 没有互联网还能工作吗 | 可以。采集、传输、预览、录制、按键、固定提示音、CLOCK_SYNC 和 NAS 补传都在局域网内工作。在线 TTS 可能不可用。 |
| 系统会自动更新吗 | 不会。当前明确禁止现场设备自动更新。 |
| 相机数量需要提前写死吗 | 不需要。Receiver 接受动态数量的 Sender；实际稳定路数仍受 Wi-Fi、Receiver 和 NAS 吞吐限制。 |
| 网页需要账号或令牌吗 | 当前受信任采集局域网内不需要，地址为 `http://<receiver-ip>:8080`。 |

## 2. 系统里每台设备负责什么

```text
Orbbec 相机
  -> Sender：采集 RGB/Depth、编码、打时间戳、发送
  -> Receiver：接收、网页预览、录制、本地可靠暂存
  -> NAS：保存最终交付文件
```

设备分工：

1. **Sender**：ARM 设备，连接相机，通常使用 5 GHz Wi-Fi。
2. **Receiver**：x86 Linux 电脑，建议使用网线，提供网页和录制控制。
3. **NAS**：绿联 NAS，建议使用网线，只提供 SMB 存储，不运行项目主程序。
4. **操作电脑**：与 Receiver 在可互相访问的局域网，通过浏览器操作。

正式现场按“一台 Receiver、一台配套 NAS、若干 Sender”设计。测试环境可以有多台 Receiver，但正式使用时不要让多台 Receiver 同时争抢同一批 Sender。

## 3. 推荐的交付方式

### 3.1 推荐方式：出厂前完成初始化

这是现场最省事的方式。出厂前完成：

1. Sender 安装批准的 Ubuntu ARM64 镜像。
2. 安装项目冻结版本、对应 Orbbec SDK 和硬件编码插件。
3. 自动识别板卡与相机型号，生成稳定且唯一的 `sender_id`。
4. 写入 `/etc/gwv3/sender.json`，启用相机热插拔和 Receiver 自动发现。
5. 安装并启用 `gwv3-gemini-sender.service`。
6. 配置 Chrony、CLOCK_SYNC、关闭 Wi-Fi 省电。
7. 需要时安装按键、LED、音频、语音和拍照服务。
8. Receiver 安装接收端、Web、NAS 自动挂载、uploader 和自启动服务。
9. 完成一次重启和 30 秒全路录制验收。

现场人员只需要接线、开机、让 Sender 连入 Wi-Fi、查 Receiver IP，然后打开网页。

### 3.2 备用方式：现场给空设备一键部署

适用于已经安装批准系统、但还没有项目环境的设备。它不是裸机装系统工具。

在线安装 Sender：

```bash
curl -fsSLO https://raw.githubusercontent.com/RealityLoopAI/wireless_video_delivery/field-v2026.09.07.5/05_tools/bootstrap_online.sh
chmod +x bootstrap_online.sh
./bootstrap_online.sh --role sender
```

在线安装 Receiver：

```bash
./bootstrap_online.sh --role receiver
```

脚本完成依赖、SDK、构建、配置、服务、自启动和开机验收。未知板卡或未知相机不会被脚本猜测配置，而是停止并要求人工确认。

无互联网时使用预先制作的离线包，入口仍然是：

```bash
./install.sh --role sender
./install.sh --role receiver
```

## 4. IP 改变后为什么仍能工作

### 4.1 Sender 找 Receiver

Sender 不把某个实验室 IP 当成唯一地址。发现顺序如下：

1. 通过局域网 UDP `50009` 广播查找 Receiver。
2. 尝试 `gwv3-receiver.local`。
3. 使用配置中的 fallback 地址兜底。
4. 找到 Receiver 后保存稳定 `receiver_id` 和最新 IP。
5. Receiver 地址变化时，媒体、状态、预览和 CLOCK_SYNC 一起切换到新目标。

因此正常情况下无需逐台 SSH 修改 IP。

自动发现成立需要：

- Sender 和 Receiver 位于可互相访问的局域网；
- AP 没有开启客户端隔离；
- UDP `50009` 没有被防火墙拦截；
- 正式现场只有一台目标 Receiver。

跨 VLAN、访客 Wi-Fi或禁止广播的网络不能只依赖自动发现，需要网络管理员放通或配置可解析的兜底主机名。

### 4.2 Receiver 找 NAS

绿联 NAS 不要求安装项目代码。首次部署 Receiver 时输入一次：

- NAS 当时的 IP 或主机名；
- SMB share；
- SMB 用户名；
- SMB 密码。

Receiver 将密码保存在本机 root-only 文件中，将 NAS 身份和 MAC 单独持久化。以后旧地址失效时，自动挂载服务会重新查找并恢复挂载。

“自动发现 NAS”不等于不需要首次授权。SMB 账号和密码不能通过广播自动获取，也不能写进 Git 仓库。

## 5. 录制和 NAS 的真实数据路径

当前正式路径不是收到数据后直接裸写最终 NAS 目录：

```text
相机数据
  -> Receiver 每路可靠录制队列
  -> Receiver 本地 staging
  -> uploader 增量搬运
  -> NAS 隐藏目录
  -> 校验并原子发布
  -> 最终目录 + recording_ready.json
```

这样设计的原因：

1. NAS 短时抖动不会立即打断当前录制。
2. 下游不会看到半写入文件并误认为录像完成。
3. 停止录制后容器收尾和 NAS 发布可在后台完成。
4. NAS 恢复后可以自动补传 Receiver 本地积压。

重要边界：

- “点击停止成功”只表示停止边界已接受，不代表 NAS 已完成发布。
- 最终目录出现有效 `recording_ready.json` 才表示该分片可交付。
- NAS 平均写入速度如果长期低于媒体生成速度，本地 backlog 仍会增长；多 worker 不能突破物理带宽。
- 本地空间不足时，系统会拒绝新录制或安全停止，不会自动删除已有原始数据。

## 6. 开机、断网和断电行为

| 事件 | 系统行为 | 是否需要人工操作 |
| --- | --- | --- |
| 正常开机 | Receiver、Web、Sender 和辅助服务自动启动 | 打开网页并检查相机即可 |
| Sender Wi-Fi 较慢 | watchdog 等待网络并重试 | 通常不需要 |
| Receiver 晚开机 | Sender 持续后台发现并重连 | 通常不需要 |
| 相机重新插入 | Sender 热插拔扫描并恢复；持续零帧时 watchdog 重建进程 | 等待恢复；失败再联系维护 |
| NAS 短时断线 | 当前数据保留在 Receiver 本地，uploader 暂停 | 恢复 NAS 网络即可 |
| Receiver 重启 | 修复和补传已落地数据，不自动继续旧录像 | 重新手动开始录制 |
| Sender 重启 | 采集和发送自动恢复，断线期间形成可检测缺口 | 检查画面后决定是否重录 |
| 整套断电 | 已完整发布分片保留；最后活动分片可能需要恢复审计 | 恢复后重新开始录制 |
| 互联网中断 | 在线 TTS 受影响，主采集链路不受影响 | 无需停止采集 |

系统保证“保留真实状态并可检测缺口”，不伪造断电或断网期间不存在的帧。

## 7. 当前方案不承诺什么

1. 不承诺不同相机同时曝光。`global_timestamp_us` 是软件统一时间轴，不是硬件同步信号。
2. 不承诺 Wi-Fi 在任何客户网络都能承载任意路数。必须做现场吞吐和丢包验收。
3. 不承诺 Sender 断网期间缓存完整 RGB-D。该时段会形成明确缺口。
4. 不承诺录制停止瞬间 NAS 已经可见全部文件。发布需要完成收尾、补传和校验。
5. 不承诺未知板卡、未知相机或任意 Linux 镜像可以自动适配。
6. 不承诺异常断电时最后一个正在写的分片完整。
7. 不允许现场自动升级。版本升级必须由维护人员明确执行并保留回退点。

## 8. 交付验收标准

不能只看网页打开或 systemd 显示 `active`。完整交付至少满足：

1. Receiver 和 NAS 接网线，Sender 接 5 GHz Wi-Fi。
2. 整机冷启动后两分钟内网页可访问。
3. 网页显示预期数量相机在线，RGB/Depth 数据持续更新。
4. 每台 Sender 的相机在 USB 3.x 上运行，分辨率和帧率符合批准配置。
5. Chrony 正常，CLOCK_SYNC 有效。
6. 执行一次不少于 30 秒的全路录制。
7. 停止后所有路完成收尾，NAS uploader backlog 清空。
8. 每个最终目录存在 `recording_ready.json`。
9. RGB/Depth 可读取，RGB 可拖动，`frames.csv` 持续且可解析。
10. 模拟 NAS 临时断线后，Receiver 本地保留数据，恢复后自动补传。
11. 断电重启后不会自动开始新录像。

自动验收入口：

```bash
sudo cat /var/lib/gwv3/deployment-report.txt
./05_tools/gwv3_doctor.sh sender /etc/gwv3/sender.json
./05_tools/gwv3_doctor.sh receiver /etc/gwv3/receiver.json
```

Receiver 空闲且允许触发测试录像时：

```bash
python3 05_tools/run_deployment_acceptance.py \
  --admin http://127.0.0.1:18080 \
  --record-seconds 30 \
  --output 08_reports/deployment-acceptance.json
```

## 9. 谁负责什么

| 角色 | 负责内容 | 不应擅自处理 |
| --- | --- | --- |
| 出厂初始化人员 | 安装冻结版本、配置服务和网络、执行验收 | 修改相机档位、协议和业务参数 |
| 现场运维 | 接线、开关机、检查网页、开始/停止录制、确认数据完成 | SSH 改代码、重装 SDK、手工伪造完成标记 |
| 开发维护人员 | 诊断日志、升级版本、修复服务、恢复异常分片 | 在录制中直接重启或覆盖数据 |
| 下游工程师 | 只消费完成目录，按 CSV 表头和时间戳处理 | 读取隐藏目录、用 CSV 行号代替视频帧索引 |

## 10. 文档入口

- 现场日常操作：[operator-manual.md](operator-manual.md)
- 一键部署：[one-click-deployment.md](one-click-deployment.md)
- 初始化工单：[device-initialization.md](device-initialization.md)
- 录制与 NAS：[recording-and-nas.md](recording-and-nas.md)
- 故障排查：[troubleshooting.md](troubleshooting.md)
- 接口与数据格式：[api-reference.md](api-reference.md)
