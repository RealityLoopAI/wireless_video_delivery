# Documentation Index

更新时间：2026-09-02

本目录只保存当前主线的长期维护文档。文件名统一使用英文，正文使用中文。历史排查记录已从工作区删除；需要追溯时使用 Git 历史，不把历史现象当作当前行为。

## Reading Order

| Order | Document | Purpose |
| --- | --- | --- |
| 1 | [overview.md](overview.md) | 项目目标、角色和能力边界 |
| 2 | [architecture.md](architecture.md) | 当前架构、线程和模块边界 |
| 3 | [data-pipeline.md](data-pipeline.md) | RGB、Depth、时间戳、预览和文件链路 |
| 4 | [deployment.md](deployment.md) | 构建、部署、启停和现场检查 |
| 5 | [configuration.md](configuration.md) | 配置文件、设备配置清单和约束 |
| 6 | [api-reference.md](api-reference.md) | 端口、协议、REST API 和数据字段 |
| 7 | [recording-and-nas.md](recording-and-nas.md) | 录制状态机、分片、fMP4、原子发布和 NAS |
| 8 | [clock-sync.md](clock-sync.md) | chrony、CLOCK_SYNC、全局时间轴和下游对齐 |
| 9 | [audio-and-controls.md](audio-and-controls.md) | 音频、语音拍照、TTS、GPIO 按键和 LED |
| 10 | [troubleshooting.md](troubleshooting.md) | 按现象排查相机、网络、预览、录制和同步 |
| 11 | [testing-and-release.md](testing-and-release.md) | 自动测试、长测、版本核对和发布流程 |
| 12 | [roadmap.md](roadmap.md) | 已知上限与后续演进方向 |

## Sources Of Truth

判断当前行为时按以下优先级：

1. `main` 分支当前源码与测试。
2. `06_configs/` 中实际选择的设备配置。
3. 本目录当前文档。
4. Git 历史，仅用于解释过去为什么修改。

现场设备可能尚未部署最新提交。排障时必须同时核对运行服务的 `build_commit`、`build_source_hash` 和 `build_dirty`，不能只看本地仓库。

## Documentation Rules

- 一个主题只保留一份正式文档；不再新增按日期命名的阶段总结。
- 已解决问题更新到相应正式文档的“故障模式”或“设计理由”中。
- 测试结果进入测试脚本或提交说明，不在 `08_reports/` 堆积 Markdown。
- `08_reports/` 仅是兼容现有服务的运行日志根目录，日志由 `.gitignore` 排除。
- 设备密码、Wi-Fi 密码、SSH 凭据、私有 NAS 路径和原始录制数据禁止进入仓库。
- 外部接口、配置、端口或文件格式变更时，必须同步更新文档与兼容性测试。
