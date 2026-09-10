# RGB 少量缺帧取证

详细结论见 [排查记录](../../04_docs/rgb-input-gap-investigation-20260910.md)。

- `sdk-input-frames.csv`、`sdk-input-probe.txt`：52d2 的 300 秒纯 SDK 实验，不是正式 frames.csv 格式。
- `sdk-v1-tests.txt`、`sdk-v2-tests.txt`：两个 SDK 配置的原生测试日志，均为同一套 11 项测试。
- `recording.json`：六路 180 秒录制及交付结果。
- `recording-analysis.json`：全部 CSV 与 12 个视频全帧解码结果。
- `findings.json`：已对应上的编码缺帧、纯采集统计以及尚未定位的边界。
- `deployment.json`：仅两台诊断版部署及回退二进制信息。

本批有一个真实 RGB 源 ID 缺口，不把 `complete` 当成零缺帧。
没有保存到异常原始图像，也没有伪造历史缺帧的具体原因。
