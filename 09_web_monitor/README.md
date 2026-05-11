# Web Monitor

FastAPI wrapper for the receiver admin API.

Frontend page:

```text
09_web_monitor/static/index.html
```

Default receiver admin endpoint:

```text
http://127.0.0.1:18080
```

Default web endpoint:

```text
http://0.0.0.0:8080
```

The receiver startup script reads `web_bind_ip`, `web_port`, and `admin_port` from:

```text
06_configs/receiver_ubuntu-01.json
```

Normal local URL:

```text
http://127.0.0.1:8080
```
