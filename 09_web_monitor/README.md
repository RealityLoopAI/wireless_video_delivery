# Web Monitor

Web Monitor 是 receiver 管理 API 的 FastAPI 网页和局域网 REST 代理。它显示状态和预览并转发控制请求，但不拥有录制状态，也不直接写 NAS。

## Endpoints

| Address | Scope | Purpose |
| --- | --- | --- |
| `http://<receiver-ip>:8080/` | LAN | 网页监控 |
| `http://<receiver-ip>:8080/api/status` | LAN | 聚合状态 |
| `http://127.0.0.1:18080/api/status` | receiver only | C++ admin 原始状态 |

`18080` 必须保持 loopback。外部程序统一调用 `8080`，完整接口见 [api-reference.md](../04_docs/api-reference.md)。

## Runtime

页面入口：

```text
09_web_monitor/static/index.html
```

依赖：

```text
09_web_monitor/requirements.txt
```

推荐由 receiver 启动脚本统一管理：

```bash
./05_tools/start_receiver.sh 06_configs/receiver_loop.json
./05_tools/status_receiver.sh 06_configs/receiver_loop.json
```

脚本从所选 receiver 配置读取 `web_bind_ip`、`web_port` 和 `admin_port`，不要在前端代码中硬编码现场地址。

## Behavior

- 页面可查看 sender/camera 在线状态、RGB/Depth 预览、录制状态和关键诊断指标。
- RGB/Depth 图片接口是最新帧快照，允许丢帧；主 H.264 接口需要客户端持续读取。
- admin 短时超时时，只读 status 可使用有限缓存；写操作不会用缓存伪造成功。
- 页面显示时间使用北京时间 UTC+8、精确到秒；原始 `*_timestamp_us` 保持 Unix epoch microseconds。
- Web 预览卡顿或消失不等于正式录制失败，必须结合 receiver 队列、媒体 age 和 NAS 完成标记判断。

## Security

当前接口面向受信任的采集局域网，默认不要求访问令牌。不要把 `8080` 映射到公网；需要跨网络访问时，应在外部网关增加认证和 TLS，而不是直接开放 receiver。
