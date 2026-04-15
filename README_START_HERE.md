# Wireless Video Delivery Bundle

这个目录是最终交付包，按下面顺序使用即可。

## 先看这 4 条命令（最傻瓜）

在交付目录执行：

```bash
bash start_receiver_linux.sh
bash start_sender.sh 192.168.1.105
bash status.sh
bash stop.sh
```

如果你不想记命令，直接运行：

```bash
bash one_click.sh
```

## 目录说明

- `01_sender_linux/`: Linux 发送端代码工程
- `02_receiver_windows/`: Windows 接收端代码工程
- `03_receiver_linux/`: Linux 接收端代码工程
- `04_docs/`: 产品手册、快速开始、技术路线、运维说明
- `05_tools/`: 一键检查与启动脚本
- `06_configs/`: 正式配置基线
- `07_samples/`: 运行时日志与临时配置输出目录
- `08_reports/`: 测试验收与边界说明

## Linux 快速启动

1. Linux 接收端（推荐先启动）  
`bash start_receiver_linux.sh`

2. Linux 发送端（传接收端IP）  
`bash start_sender.sh 192.168.1.105`

3. 查看状态  
`bash status.sh`

4. 停止  
`bash stop.sh`

## Windows 接收端

在 PowerShell 里执行：

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\start_receiver_windows.ps1
.\status_receiver_windows.ps1
```

更傻瓜方式（推荐）：

```powershell
.\one_click_windows.cmd
```

## 接收端IP改了怎么办

优先用傻瓜启动脚本直接传 IP，不用改 JSON：

`bash start_sender.sh <新的接收端IP>`

如果手改配置，改这里：

`06_configs/sender.default.json` 的 `network.remote_ip`
