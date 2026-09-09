# 2026-09-09 缺录修复证据

此目录按用户要求归档已经筛选的测试证据，不包含视频、完整配置、账号密码、SDK 二进制或完整 status 日志。
这里只保存有限复现实验与验收输出，不持续向 Git 写运行日志。
详细结论见 [修复报告](../../04_docs/mpp-keyframe-recovery-20260909.md)。

| 文件前缀 | 条件和结论 |
| --- | --- |
| `vendor-long-*` | 52d2 换驱动后的第一轮 920 秒；ab 丢帧、本机首帧晚，严格失败；全视频解码正常不等于完整 |
| `balanced-ap-short` | 换 AP 后短测；启动时仅五台在线，不能替代六路预先在线验收 |
| `balanced-ap-long-*` | 第二轮 920 秒；12 个分片 complete，24 视频解码/CSV 一致；e8 仍有 2 个 RGB 源 ID 缺口 |
| `c7423c6-deployed-short*` | 六台统一主程序后 120 秒；52d2、b439 首帧迟到，严格失败 |
| `mpp-baseline-tests.txt` | 相同固定源码、不加 sync-point 补丁，重复关键帧请求失败 |
| `mpp-patched-tests.txt` | 本机加补丁后 JPEG 单路、双分支、BGR 三模式通过 |
| `plugin-deployment.json` | 六机插件原生测试、运行 maps、二进制和配置摘要；不代表 8 小时验收 |
| `mpp-syncpoint-long*` | 补丁全部部署后的 920 秒，12 片 complete；3 个 RGB 源 ID 缺口，52d2 存在无效 clock 区间，详见报告 |

`*-analysis.json` 按 CSV 表头分析 global 倒退/重复、源 ID 缺口、最大间隔和切片边界。
`*-decode.json` 还包含每个完整视频的 ffmpeg 解码错误、解码帧数和 CSV 一致性。
`ready=true` 仅表示交付就绪；必须另外查看质量状态、缺口与解码，失败批次没有删除。

最后一批的在线等待探针因另一会话开始而终止，随后明确按原会话目录离线验证。
不要把 `validation_kind=offline_session_files` 的结果写成原探针正常退出；
`window_end_to_last_nas_finalize_ms` 也不等于精确的点击结束到 NAS 完成时间。
