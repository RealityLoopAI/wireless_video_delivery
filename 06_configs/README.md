# 配置基线说明

- `sender.default.json`: 发送端正式配置基线
- `sender.usb2_stable.json`: USB2/弱性能场景基线
- `sender.weaknet.json`: 弱网调优基线
- `receiver.linux.default.json`: Linux 接收端基线
- `receiver.linux.weaknet.json`: Linux 接收端弱网基线
- `receiver.windows.default.json`: Windows 接收端基线

接收端 IP 变化通常不需要改接收配置，改发送端目标即可：

`network.remote_ip`
