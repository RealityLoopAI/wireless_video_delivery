# Device Information API

更新时间：2026-09-08

## 1. 用途

局域网内的程序通过 Receiver 查询所有 Sender 的设备信息，无需逐台访问 Sender，也无需登录或提供访问令牌。

当前返回的核心信息包括：

- Sender ID 和主机名；
- Wi-Fi 接口 `wlan0` 的永久 MAC；
- Sender 当前系统日期、时间和时区；
- 在线状态、相机列表、来源 IP 和信息新鲜度。

该接口只传递设备状态，不影响 RGB、Depth、录制或 CLOCK_SYNC 链路。

## 2. 地址

基础地址：

```text
http://<receiver-ip>:8080
```

当前测试环境示例：

```text
http://192.168.1.196:8080
```

查询全部设备：

```http
GET /api/device-info
```

按 Sender ID 查询：

```http
GET /api/device-info?sender_id=lubancat-e8cc0cb3
```

按永久 MAC 查询，MAC 大小写不敏感：

```http
GET /api/device-info?mac=80:9d:65:d8:2f:4a
```

接口无需鉴权，支持浏览器跨域 GET。只应在受信任的采集局域网中开放，不应直接暴露到公网。

## 3. 返回示例

```json
{
  "ok": true,
  "protocol_version": "1.0",
  "generated_at": "2026-09-08T12:15:30+08:00",
  "receiver_admin_stale": false,
  "device_count": 1,
  "devices": [
    {
      "sender_id": "lubancat-e8cc0cb3",
      "device_info_available": true,
      "device_info_version": 1,
      "host_name": "lubancat",
      "wifi_interface": "wlan0",
      "wifi_permanent_mac": "80:9d:65:d8:2f:4a",
      "mac_is_permanent": true,
      "mac_source": "ethtool_perm_addr",
      "device_date": "2026-09-08",
      "device_time": "2026-09-08T12:15:29+08:00",
      "device_system_time_us": 1788840929000000,
      "timezone": "Asia/Hong_Kong",
      "device_info_received_us": 1788840929100000,
      "device_info_age_ms": 100,
      "online": true,
      "camera_ids": ["cam01"],
      "source_ips": ["192.168.1.159"]
    }
  ]
}
```

## 4. 字段说明

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `generated_at` | string | Receiver 生成本次 HTTP 响应的时间 |
| `receiver_admin_stale` | bool | `true` 表示使用了 Receiver 的短时状态缓存 |
| `device_count` | integer | 本次过滤后返回的 Sender 数量 |
| `sender_id` | string | Sender 的稳定业务标识 |
| `device_info_available` | bool | Sender 是否已经上报新版设备信息 |
| `host_name` | string | Sender Linux 主机名 |
| `wifi_interface` | string | 当前固定为 `wlan0` |
| `wifi_permanent_mac` | string | 小写、冒号分隔的 Wi-Fi MAC；无法读取时为空字符串 |
| `mac_is_permanent` | bool | 是否由永久地址接口确认，调用方必须检查此字段 |
| `mac_source` | string | MAC 的实际读取来源，见下表 |
| `device_date` | string | Sender 当前系统日期，格式 `YYYY-MM-DD` |
| `device_time` | string | Sender 当前系统时间，ISO 8601，包含 UTC 偏移 |
| `device_system_time_us` | integer | Sender 系统 Unix 时间，单位微秒 |
| `timezone` | string | Sender 时区，例如 `Asia/Hong_Kong` |
| `device_info_received_us` | integer | Receiver 收到这份设备信息的本机 Unix 时间，单位微秒 |
| `device_info_age_ms` | integer/null | 信息距本次查询的年龄；未收到新版信息时为 `null` |
| `online` | bool | 该 Sender 是否至少有一路相机被 Receiver 判定在线 |
| `camera_ids` | array | 此 Sender 当前已知的相机 ID |
| `source_ips` | array | Receiver 观察到的 Sender 来源 IP，可能经过 AP/NAT |

`mac_source` 取值：

| 值 | 含义 |
| --- | --- |
| `sysfs_perm_address` | 内核 sysfs 明确提供永久地址 |
| `ethtool_perm_addr` | 通过内核 `ETHTOOL_GPERMADDR` ioctl 读取永久地址 |
| `sysfs_current_address_fallback` | 驱动不支持永久地址查询，回退为当前 MAC；此时 `mac_is_permanent=false` |
| `unavailable` | 无法获得有效 MAC |

## 5. 调用示例

命令行：

```bash
curl -sS http://192.168.1.196:8080/api/device-info
```

Python：

```python
import requests

response = requests.get(
    "http://192.168.1.196:8080/api/device-info",
    timeout=3,
)
response.raise_for_status()

for device in response.json()["devices"]:
    if not device["device_info_available"]:
        continue
    print(
        device["sender_id"],
        device["wifi_permanent_mac"],
        device["device_date"],
        device["online"],
    )
```

浏览器 JavaScript：

```javascript
const response = await fetch("http://192.168.1.196:8080/api/device-info");
if (!response.ok) throw new Error(`HTTP ${response.status}`);
const data = await response.json();
console.log(data.devices);
```

## 6. 调用规则

1. 设备唯一业务身份仍使用 `sender_id`，不要只依赖 MAC。
2. 需要永久 MAC 时，必须同时确认 `mac_is_permanent=true`。
3. `device_date` 来自 Sender，不是 Receiver；判断是否实时还要检查 `device_info_age_ms`。
4. 推荐查询周期不快于 5 秒。设备信息来自约 1 秒一次的 heartbeat，没有必要高频轮询。
5. Sender 离线后 Receiver 可保留最后一次值，并返回 `online=false`。
6. 按条件没有匹配设备时返回 HTTP 200、`device_count=0` 和空 `devices` 数组。
7. Receiver 状态接口及短时缓存均不可用时返回 HTTP 502，调用方应稍后重试。

## 7. Sender 上报字段

Sender 在现有 UDP heartbeat 中附加以下字段，不新增端口：

```json
{
  "device_info_version": 1,
  "host_name": "lubancat",
  "wifi_interface": "wlan0",
  "wifi_permanent_mac": "80:9d:65:d8:2f:4a",
  "mac_is_permanent": true,
  "mac_source": "ethtool_perm_addr",
  "device_date": "2026-09-08",
  "device_time": "2026-09-08T12:15:29+08:00",
  "device_system_time_us": 1788840929000000,
  "timezone": "Asia/Hong_Kong"
}
```

MAC 在 Sender 进程启动后读取并缓存；日期和时间在每条 heartbeat 生成，因此跨天运行时会自动更新。
