# lubancat-52d2ef0c 录制状态 LED 归档

日期：2026-08-19

## 硬件确认

- LED 接到 RK3576 `GPIO4_C3_D`。
- Linux GPIO 映射为 `gpiochip4` line `19`，传统全局编号为 `147`。
- 现场空闲电平读取为 `1` 且 LED 点亮，因此配置为高电平有效。
- 该管脚原设备树复用为 HDMI TX SDA，但没有 GPIO consumer 占用；LED 服务通过 `libgpiod` 请求后切换为 GPIO 输出并持续持有。

## 行为

- `lubancat-52d2ef0c` 任一路相机处于录制状态：LED 每 `500 ms` 翻转一次。
- 未录制：LED 熄灭。
- HTTP 状态查询在独立线程运行，不会阻塞 LED 闪烁。
- 短暂超时沿用最近一次有效状态，超过 `5 s` 仍无有效状态则熄灭，避免误报录制。
- 服务退出时主动熄灭 LED 并释放 GPIO。

## 实现

- 新增 `recording_led_service.py`，独立于按键线程运行。
- 新增 root systemd 单元 `gwv3-recording-led.service`，只放行 `/dev/gpiochip4`。
- 状态来源为接收端 `/api/status`，仅匹配 `sender_id=lubancat-52d2ef0c`。
- LED 配置保存在 `config_lubancat-52d2ef0c.json`。
