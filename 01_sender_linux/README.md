# Gemini Wireless Video

基于树莓派 5 与奥比中光 Gemini 相机的单路 RGB 无线视频传输工程。

目标链路：

`Gemini RGB -> H.264 编码 -> RTP/UDP -> 接收端解码 -> 实时显示`

## 目录

- `wireless_video/`: 核心代码
- `config/`: 示例配置
- `run_sender.py`: 发送端入口
- `run_receiver.py`: 接收端入口
- `tests/`: 基础单元测试

## 依赖

- `pyorbbecsdk`
- `opencv-python`
- `numpy`
- `av`

## 快速开始

发送端：

```bash
conda activate orbbec_env
cd /home/berry/orbbec/pyorbbecsdk-main/gemini_wireless_video
python run_sender.py --config config/sender.json
```

接收端：

```bash
conda activate orbbec_env
cd /home/berry/orbbec/pyorbbecsdk-main/gemini_wireless_video
python run_receiver.py --config config/receiver.json
```

## 说明

- 当前版本只处理单个 Gemini 相机的 RGB 视频流。
- 队列采用实时优先策略，满时丢弃最旧帧。
- 编码默认使用 `libx264` 低延迟配置。
- 发送端和接收端均带有基础状态统计与自动恢复逻辑。

## Systemd 自启动

发送端可配置为树莓派开机自启。

环境文件：

`/home/berry/orbbec/pyorbbecsdk-main/gemini_wireless_video/systemd/gemini-wireless-sender.env`

默认指向：

`/home/berry/orbbec/pyorbbecsdk-main/gemini_wireless_video/config/sender.json`

安装服务：

```bash
cd /home/berry/orbbec/pyorbbecsdk-main/gemini_wireless_video
sudo bash systemd/install_sender_service.sh
```

启动与查看状态：

```bash
sudo systemctl start gemini-wireless-sender.service
sudo systemctl status gemini-wireless-sender.service
sudo journalctl -u gemini-wireless-sender.service -f
```

设置开机自启后，重启设备会自动拉起发送端。

停止并取消安装：

```bash
sudo bash systemd/uninstall_sender_service.sh
```
