# 接收端深度暂存修复验收证据

源码 `7681bb7539c0`，Receiver source hash `bd83e3d7b26999ab`。
完整说明在 `04_docs/rgbd-recording-gap-fix-acceptance-20260910.md`。

- `first-regression-failures.txt`：首轮回归的真实失败记录，未删除。
- `ctest-after.txt`：修正坏包校验次序及测试发布等待后，34/34 全套通过。
- `release-repeat.txt`：正式提交二进制，新增深度暂存测试重复三次通过。
- `deployment.json`：实际替换的旧/新二进制散列、配置散列与回退路径。
- `recording.json`：六路 300 秒脚本验收，质量为 complete；实际窗口约 299.008 秒。
- `csv-analysis.json`：逐帧源 ID、全局时间、时钟标记与视频索引检查。
- `recording-analysis.json`：完整解码结束后生成的最终逐帧分析。
- `runtime.jsonl`：约两秒一次的运行快照，不是所有网络延迟的完整采样。
- `sender-*.json`：北京时间 11:51:59 至 11:56:59 的发送端日志与计数。
- `receiver-runtime.txt`：对应时段接收端到达延迟警告。
- `post-recording-status.json`：停止后六路在线及 NAS 队列状态。

独立端口/临时目录回归命令：

```sh
ctest --test-dir /tmp/wvd-prestart-build --output-on-failure
ctest --test-dir /tmp/wvd-prestart-build -R receiver_prestart_depth_integration --repeat until-fail:3 --output-on-failure
```

实录使用 `05_tools/run_deployment_acceptance.py` 的当前会话保护版本：

```sh
python3 /tmp/wvd-run-rgb-acceptance.py --record-seconds 300 --finalize-timeout 180 --config /etc/gwv3/receiver.json --require-complete-recording --output /tmp/wvd-prestart-acceptance.json
```

该命令会真实开始和停止录制，只能在确认空闲后主动使用，不要在用户录制中执行。
正式数据保留于六个相机的 `2026-09-10/115159/`，没有清理测试录像。
