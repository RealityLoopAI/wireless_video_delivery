# Web Monitor

Web Monitor 是接收端管理 API 的 FastAPI 网页和 REST 代理。完整说明见 [../04_docs/04_部署与运行手册.md](../04_docs/04_部署与运行手册.md) 和 [../04_docs/05_接口与数据格式参考.md](../04_docs/05_接口与数据格式参考.md)。

前端页面：

```text
09_web_monitor/static/index.html
```

默认接收端管理 API：

```text
http://127.0.0.1:18080
```

默认 Web 监听：

```text
http://0.0.0.0:8080
```

接收端启动脚本会从下面配置读取 `web_bind_ip`、`web_port` 和 `admin_port`：

```text
06_configs/receiver_ubuntu-01.json
```

本机访问地址：

```text
http://127.0.0.1:8080
```

时间显示规则：

```text
Readable timestamps in the page are shown as Beijing time UTC+8 with second precision.
Raw API fields such as *_timestamp_us and last_*_us remain Unix epoch microseconds.
```
