# 快速开始

## Linux 到 Linux

在交付包根目录执行：

```bash
bash 05_tools/check_and_start_receiver_linux.sh
bash 05_tools/check_and_start_sender.sh 192.168.1.105
bash 05_tools/status_receiver_linux.sh
bash 05_tools/status_sender.sh
```

停止：

```bash
bash 05_tools/stop_sender.sh
bash 05_tools/stop_receiver_linux.sh
```

## Linux 发送到 Windows 接收

Windows PowerShell：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\05_tools\check_and_start_receiver_windows.ps1
.\05_tools\status_receiver_windows.ps1
```

Linux 发送端：

```bash
bash 05_tools/check_and_start_sender.sh 192.168.1.105
```

## 接收端换 IP

只需要重新执行发送端启动命令，传新的 IP 即可：

```bash
bash 05_tools/start_sender_easy.sh <new_receiver_ip>
```
