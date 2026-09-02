# Configuration Inventory

本目录保存可提交的 receiver、sender、音频和网络调优配置。设备清单与字段说明见 [configuration.md](../04_docs/configuration.md)。

规则：

- 设备暂时离线不删除其稳定 sender 配置。
- 正式 service 必须明确引用一个设备配置，不能依赖目录中“最新”文件。
- 名称含 `test`、`rtp_only`、`four_rgb` 或特殊 profile 的文件只用于明确实验，不作为默认生产配置。
- 修改 receiver 地址、相机身份、profile、曝光、旋转、端口或录制模式后，必须运行配置验证并同步检查 systemd service。
- 禁止写入密码、Wi-Fi PSK、SSH 凭据和私有 token。
