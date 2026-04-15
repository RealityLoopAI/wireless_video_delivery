# 交接说明

## 本次交付包含

- 三套工程目录（发送端 / Linux接收端 / Windows接收端）
- 一键检查与启动脚本（`05_tools`）
- 配置基线（`06_configs`）
- 文档与报告（`04_docs`, `08_reports`）

## 关键注意点

- Windows 接收端包来自 `gemini_wireless_video (2)` 分支
- Linux 接收端包来自 `gemini_wireless_video` 主线
- 若后续要合并为一套统一接收端，建议以 Linux 主线的稳定性改动为基准回灌 Windows 分支

## 推荐后续任务

1. 增加硬件解码路径并做并发回归  
2. 统一 Linux/Windows 接收端代码分支  
3. 增加 CI 自动检查（依赖、语法、单测）
