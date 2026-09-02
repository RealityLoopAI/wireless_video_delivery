# Optional Device Applications

`12_apps` 保存建立在 sender/receiver 核心链路之上的可选设备功能。核心 RGB-D 采集和录制不依赖这些应用。

| Application | Purpose | Documentation |
| --- | --- | --- |
| `recording_buttons` | 物理按键控制本 sender 录制、电源键策略和录制 LED | [recording_buttons/README.md](recording_buttons/README.md) |
| `xiaohuan_voice_photo` | 离线关键词唤醒、语音提示、HTTP TTS、整句音频转发和三连拍 | [xiaohuan_voice_photo/README.md](xiaohuan_voice_photo/README.md) |

## Boundary

- 可选应用通过本地文件请求、loopback API 或 receiver 的局域网 REST API 使用核心能力。
- 应用故障不得阻塞相机采集、主媒体发送、receiver 录制或 NAS 收尾。
- systemd 服务、声卡/GPIO 选择和设备身份由目标设备配置决定，不能复制其他设备的本地 runtime 配置。
- 安装前先阅读应用 README；部署后分别检查应用服务和核心 sender 服务。

跨应用的音频与控制设计见 [audio-and-controls.md](../04_docs/audio-and-controls.md)。
