# Documentation Index

更新时间：2026-09-02

本目录只保存当前 `main` 的长期维护文档。文件名使用英文，正文使用中文。历史现象通过 Git 历史追溯，不把旧报告当作当前行为。

## By Task

| Task | Document |
| --- | --- |
| 第一次理解系统 | [overview.md](overview.md) |
| 理解进程、线程和模块边界 | [architecture.md](architecture.md) |
| 追踪 RGB、Depth、时间戳和预览链路 | [data-pipeline.md](data-pipeline.md) |
| 构建、部署、启停和现场检查 | [deployment.md](deployment.md) |
| 选择或修改设备配置 | [configuration.md](configuration.md) |
| 调用端口、REST、视频流和读取文件 | [api-reference.md](api-reference.md) |
| 理解分片、fMP4、完成标记和 NAS | [recording-and-nas.md](recording-and-nas.md) |
| 使用 chrony、CLOCK_SYNC 和统一时间轴 | [clock-sync.md](clock-sync.md) |
| 部署音频、语音拍照、按键和 LED | [audio-and-controls.md](audio-and-controls.md) |
| 按现象诊断相机、网络、预览和录制 | [troubleshooting.md](troubleshooting.md) |
| 运行测试、长测和发布 | [testing-and-release.md](testing-and-release.md) |
| 了解已知边界和后续方向 | [roadmap.md](roadmap.md) |

## By Component

| Component | Local documentation |
| --- | --- |
| Sender | [../01_sender_linux/README.md](../01_sender_linux/README.md) |
| Receiver | [../02_receiver_linux/README.md](../02_receiver_linux/README.md) |
| Common protocol | [../03_common_core/README.md](../03_common_core/README.md) |
| Operations tools | [../05_tools/README.md](../05_tools/README.md) |
| Configurations | [../06_configs/README.md](../06_configs/README.md) |
| Web Monitor | [../09_web_monitor/README.md](../09_web_monitor/README.md) |
| Tests | [../10_tests/README.md](../10_tests/README.md) |
| Third-party dependencies | [../11_third_party/README.md](../11_third_party/README.md) |
| Optional applications | [../12_apps/README.md](../12_apps/README.md) |

## Sources Of Truth

判断当前行为时按以下优先级：

1. `main` 当前源码、协议定义和自动测试。
2. 目标设备 systemd 服务实际引用的 `06_configs/` 配置。
3. 本目录文档。
4. Git 历史，仅用于解释设计演变。

现场设备可能尚未部署最新提交。排障时同时核对运行服务的 `build_commit`、`build_source_hash` 和 `build_dirty`，不能只看开发机仓库。

## Ownership

| Change | Documents that must be reviewed |
| --- | --- |
| 端口、协议头、状态或 REST 字段 | `api-reference.md`、`data-pipeline.md` |
| 录制文件、分片、完成标记或 NAS 路径 | `recording-and-nas.md`、`api-reference.md` |
| 配置字段或设备 profile | `configuration.md`、对应组件 README |
| 服务、启动命令或部署依赖 | `deployment.md`、`05_tools/README.md` |
| 时间戳或对齐规则 | `clock-sync.md`、`data-pipeline.md` |
| 语音、音频、拍照、GPIO | `audio-and-controls.md`、`12_apps/` 对应 README |

## Documentation Rules

- 一个主题只保留一份正式说明；其他文档链接到它，不复制整段规则。
- 已解决问题进入对应文档的设计理由或故障模式，不新增按日期命名的总结。
- 命令默认从仓库根目录执行；需要切换目录时必须明确写出。
- 使用 `<receiver-ip>`、`<sender-config>` 等占位符，不在通用说明中固化现场凭据或 DHCP 地址。
- 测试结果进入自动测试、issue 或提交说明；`08_reports/` 只保存被忽略的运行日志。
- 外部接口、配置、端口或文件格式改变时，代码、测试和文档必须同一次提交更新。
