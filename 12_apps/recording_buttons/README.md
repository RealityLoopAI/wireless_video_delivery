# Physical Recording Controls

该应用把支持底板上的物理按键映射为“只控制本 sender”的录制操作，并可选管理电源键和录制状态 LED。Linux 启动前的 Recovery/Maskrom 功能不受影响。

## Behavior

| Input | Hold | Result | Cue |
| --- | --- | --- | --- |
| RECOVERY / SARADC channel 1 | 1 秒 | 开始该 `sender_id` 当前在线的全部相机 | `ding` |
| MASKROM / SARADC channel 0 | 1 秒 | 停止该 `sender_id` 的全部相机 | `deng` |
| ON/OFF / RK805 `KEY_POWER` | 5 秒 | 先安全停止本 sender 录制，再请求关机 | `ding` + 100 ms + `deng` |
| Linux boot | 一次 | 语音服务就绪后提示开机 | `deng` + 100 ms + `ding` |

短按、两键同时长按和未释放的重复触发都会忽略。动作前先查询 receiver：目标状态已经满足、receiver 不可达或状态正在转换时，不执行也不播放成功提示。有效动作先把提示音加入本地高优先级队列，再发送 sender 级命令并重试一次。

录制键只影响配置中的 `sender_id`，不能开始或停止其他发送端。服务通过 receiver 的局域网 Web API `8080` 调用；loopback 管理口 `18080` 不对外开放。

## Services

| Service | Privilege | Responsibility |
| --- | --- | --- |
| `gwv3-recording-buttons.service` | user | SARADC 录制/停止键 |
| `gwv3-power-button.service` | root | 5 秒电源键策略 |
| `gwv3-recording-led.service` | root | 开机常亮、录制闪烁 |

`systemd-logind` 使用 `HandlePowerKey=ignore`，避免短按绕过 5 秒策略。关机流程等待当前语音结束后播放组合提示，再执行系统关机。

已配置 LED 的底板使用 `GPIO4_C3_D`（`gpiochip4` line `19`，active high）：sender 空闲或 receiver 状态短时不可用时常亮，本 sender 录制时每 500 ms 翻转。HTTP 轮询在独立线程中运行，短时超时不会暂停闪烁；最后有效状态最多保留 5 秒。

## Install

在应用目录执行，设备 ID 必须有对应的 `config_<device-id>.json` 和 `config_<device-id>_power.json`：

```bash
cd 12_apps/recording_buttons
./install_service.sh <device-id>
```

安装器把选定配置复制为被忽略的本地 runtime 文件，使共享 systemd unit 不写死其他发送端身份。它会启用用户录制键服务、root 电源键服务和 LED 服务。

## Probe

在安装前读取 SARADC 阈值：

```bash
python3 recording_button_service.py \
  --config config_<device-id>.json --probe
```

释放值应高于 `released_above`，按下值应低于 `pressed_below`。自制底板或硬件版本改变后必须重新探测，不能直接沿用另一块板的阈值。

## Verification

```bash
systemctl --user is-active gwv3-recording-buttons.service
journalctl --user -u gwv3-recording-buttons.service -f
sudo systemctl is-active gwv3-power-button.service
sudo systemctl is-active gwv3-recording-led.service
curl -sS http://<receiver-ip>:8080/api/status
curl -sS http://127.0.0.1:18082/healthz
```

语音健康响应的 `cue_names` 应包含 `ding`、`deng`、`startup` 和 `shutdown`。提示音提交接口只监听 loopback，局域网客户端不能直接让设备播放控制提示。

## Failure Boundary

- receiver 查询失败时不缓存延迟操作，避免网络恢复后意外开始录制。
- 提示音或 LED 失败不得阻止核心 sender 采集与发送。
- 服务配置中的 `sender_id` 必须与实际 sender 配置一致。
- 电源服务和 LED 服务需要 root；用户服务不能假设自己有 GPIO 或关机权限。
