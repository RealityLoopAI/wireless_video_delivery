# 外部设备 REST/Web 接口使用手册 v3

更新时间：2026-05-14

本文档只说明外部设备如何调用接收端已暴露的 Web/REST 接口，不包含接收端安装、启动、停止和运维排查内容。

## 1. 访问地址

接收端 Web/REST 服务监听：

```text
0.0.0.0:8080
```

本机访问：

```text
http://127.0.0.1:8080/
```

局域网外部设备访问：

```text
http://<receiver_ip>:8080/
```

当前现场示例：

```text
http://192.168.66.196:8080/
```

注意：

- `<receiver_ip>` 以接收端机器当前网卡 IP 为准，网络切换后可能变化。
- 当前接口没有鉴权，只建议在可信局域网内使用。
- C++ 管理接口 `127.0.0.1:18080` 只绑定本机，外部设备应调用 `8080` 的 Web/REST 代理接口。

## 2. 能力范围

外部设备当前可以做：

- 查看接收端状态。
- 查看接收端配置。
- 获取 RGB 最新预览图。
- 获取 Depth 最新伪彩预览图。
- 开始全部相机录制。
- 停止全部相机录制。
- 开始指定相机录制。
- 停止指定相机录制。

外部设备当前不能做：

- 修改发送端相机参数。
- 修改曝光、分辨率、码率等采集/编码参数。
- 查询历史录像列表。
- 下载录像文件。
- 删除录像文件。
- 用户登录、鉴权和权限隔离。

## 3. 接口总览

| Method | Path | 说明 |
|---|---|---|
| `GET` | `/` | Web 监控页面。 |
| `GET` | `/api/status` | 查询接收端和相机状态。 |
| `GET` | `/api/config` | 查询接收端配置。 |
| `GET` | `/api/preview/rgb` | 获取指定相机 RGB 最新预览图。 |
| `GET` | `/api/preview/depth` | 获取指定相机 Depth 最新伪彩预览图。 |
| `POST` | `/api/record/start-all` | 开始全部相机录制。 |
| `POST` | `/api/record/stop-all` | 停止全部相机录制。 |
| `POST` | `/api/record/start` | 开始指定相机录制。 |
| `POST` | `/api/record/stop` | 停止指定相机录制。 |

## 4. 状态接口

### 4.1 查询状态

```http
GET /api/status
```

示例：

```bash
curl "http://192.168.66.196:8080/api/status"
```

返回示例：

```json
{
  "running": true,
  "recording_all": false,
  "nas_root": "/home/fz/Desktop/nas",
  "media_port": 50010,
  "status_port": 50011,
  "admin_port": 18080,
  "cameras": [
    {
      "sender_id": "orangepi5pro-01",
      "camera_id": "cam01",
      "camera_key": "orangepi5pro-01_cam01",
      "online": true,
      "recording": false,
      "segment_active": false,
      "segment_dir": "",
      "last_status_us": 1778739951620992,
      "last_media_us": 1778739951987716,
      "rgb_packets": 4483,
      "depth_packets": 4607,
      "rgb_bytes": 224296038,
      "depth_bytes": 253751391,
      "rgb_preview_available": true,
      "rgb_preview_width": 640,
      "rgb_preview_height": 360,
      "rgb_preview_us": 1778739951964507,
      "depth_preview_available": true,
      "depth_preview_width": 320,
      "depth_preview_height": 200,
      "depth_preview_us": 1778739951987716,
      "last_error": ""
    }
  ]
}
```

主要字段：

| 字段 | 说明 |
|---|---|
| `running` | 接收端 C++ 服务是否运行。 |
| `recording_all` | 是否处于全局录制状态。 |
| `nas_root` | 录像保存根目录。 |
| `cameras` | 当前接收端已发现的相机列表。 |
| `sender_id` | 发送端 ID。 |
| `camera_id` | 相机 ID。 |
| `camera_key` | `<sender_id>_<camera_id>`。 |
| `online` | 相机是否在线。 |
| `recording` | 该相机是否被请求录制。 |
| `segment_active` | 该相机当前是否已有录制切片正在写入。 |
| `segment_dir` | 当前录制切片目录；未录制时为空。 |
| `last_status_us` | 最近一次收到发送端状态心跳的时间，Unix epoch microseconds。 |
| `last_media_us` | 最近一次收到媒体包的时间，Unix epoch microseconds。 |
| `rgb_packets` / `depth_packets` | 接收端累计收到的 RGB/Depth 媒体包数量。 |
| `rgb_bytes` / `depth_bytes` | 接收端累计收到的 RGB/Depth payload 字节数。 |
| `rgb_preview_available` | 是否有 RGB 预览图可取。 |
| `rgb_preview_width` / `rgb_preview_height` | RGB 预览图尺寸。 |
| `depth_preview_available` | 是否有 Depth 伪彩预览图可取。 |
| `depth_preview_width` / `depth_preview_height` | Depth 预览图尺寸。 |
| `last_error` | 最近状态事件或错误信息。 |

判断实时流是否在更新：

- 连续调用 `/api/status`。
- 如果 `rgb_packets` / `depth_packets` 持续增加，说明媒体流正在进入接收端。
- 如果 `last_status_us` 持续增加但媒体包不增加，说明发送端心跳在线但媒体流可能异常。
- 如果 `last_status_us` 和 `last_media_us` 都长时间不变，说明发送端可能未连接或网络中断。

## 5. 配置接口

### 5.1 查询配置

```http
GET /api/config
```

示例：

```bash
curl "http://192.168.66.196:8080/api/config"
```

典型返回字段：

```json
{
  "media_bind_ip": "0.0.0.0",
  "media_port": 50010,
  "status_bind_ip": "0.0.0.0",
  "status_port": 50011,
  "admin_bind_ip": "127.0.0.1",
  "admin_port": 18080,
  "nas_root": "/home/fz/Desktop/nas"
}
```

说明：

- 该接口只读。
- 外部设备不应直接访问 `admin_port`，应走 `8080` Web/REST 接口。

## 6. 预览接口

当前预览不是 RTSP/WebRTC/HLS 视频流，而是“最新预览图”接口。客户端高频请求这些图片，即可形成近实时画面。

### 6.1 RGB 预览图

```http
GET /api/preview/rgb?sender_id=<sender_id>&camera_id=<camera_id>
```

示例：

```bash
curl \
  "http://192.168.66.196:8080/api/preview/rgb?sender_id=orangepi5pro-01&camera_id=cam01" \
  -o rgb_preview.jpg
```

返回：

```text
Content-Type: image/jpeg
Body: JPEG 图片字节
```

说明：

- 当前 RGB 源流是 H.264，接收端解码后输出预览 JPEG。
- 当前常见预览尺寸为 `640x360`。
- 预览尺寸不代表录像原始分辨率。录像 RGB 可为 `1920x1080`。

### 6.2 Depth 伪彩预览图

```http
GET /api/preview/depth?sender_id=<sender_id>&camera_id=<camera_id>
```

示例：

```bash
curl \
  "http://192.168.66.196:8080/api/preview/depth?sender_id=orangepi5pro-01&camera_id=cam01" \
  -o depth_preview.bmp
```

返回：

```text
Content-Type: image/bmp
Body: BMP 图片字节
```

说明：

- Depth 原始数据是 `uint16`。
- 该接口返回的是伪彩预览图，不是原始 Depth 数据。
- 当前常见预览尺寸为 `320x200`。
- 录像保存的 Depth 主文件为 `depth.mkv`，像素格式为 `gray16le`。

### 6.3 浏览器内直接显示预览

RGB：

```text
http://192.168.66.196:8080/api/preview/rgb?sender_id=orangepi5pro-01&camera_id=cam01
```

Depth：

```text
http://192.168.66.196:8080/api/preview/depth?sender_id=orangepi5pro-01&camera_id=cam01
```

客户端如果要做实时预览，可以每隔 `100ms ~ 300ms` 请求一次，并在 URL 后追加时间戳避免缓存：

```text
http://192.168.66.196:8080/api/preview/rgb?sender_id=orangepi5pro-01&camera_id=cam01&t=1778739951
```

## 7. 录制控制接口

### 7.1 开始全部相机录制

```http
POST /api/record/start-all
```

示例：

```bash
curl -X POST "http://192.168.66.196:8080/api/record/start-all"
```

返回示例：

```json
{
  "ok": true,
  "recording_all": true
}
```

### 7.2 停止全部相机录制

```http
POST /api/record/stop-all
```

示例：

```bash
curl -X POST "http://192.168.66.196:8080/api/record/stop-all"
```

返回示例：

```json
{
  "ok": true,
  "recording_all": false
}
```

### 7.3 开始指定相机录制

```http
POST /api/record/start?sender_id=<sender_id>&camera_id=<camera_id>
```

示例：

```bash
curl -X POST \
  "http://192.168.66.196:8080/api/record/start?sender_id=orangepi5pro-01&camera_id=cam01"
```

返回示例：

```json
{
  "ok": true
}
```

说明：

- `sender_id` 和 `camera_id` 可从 `/api/status` 的 `cameras` 列表读取。
- 如果相机尚未被接收端发现，会返回错误。

### 7.4 停止指定相机录制

```http
POST /api/record/stop?sender_id=<sender_id>&camera_id=<camera_id>
```

示例：

```bash
curl -X POST \
  "http://192.168.66.196:8080/api/record/stop?sender_id=orangepi5pro-01&camera_id=cam01"
```

返回示例：

```json
{
  "ok": true
}
```

## 8. 典型调用流程

### 8.1 查看在线相机

```bash
curl "http://192.168.66.196:8080/api/status"
```

从返回的 `cameras` 中读取：

```text
sender_id = orangepi5pro-01
camera_id = cam01
```

### 8.2 检查画面是否可预览

```bash
curl \
  "http://192.168.66.196:8080/api/preview/rgb?sender_id=orangepi5pro-01&camera_id=cam01" \
  -o rgb_preview.jpg
```

```bash
curl \
  "http://192.168.66.196:8080/api/preview/depth?sender_id=orangepi5pro-01&camera_id=cam01" \
  -o depth_preview.bmp
```

### 8.3 开始录制

```bash
curl -X POST \
  "http://192.168.66.196:8080/api/record/start?sender_id=orangepi5pro-01&camera_id=cam01"
```

### 8.4 确认录制目录

```bash
curl "http://192.168.66.196:8080/api/status"
```

查看对应相机：

```json
{
  "recording": true,
  "segment_active": true,
  "segment_dir": "/home/fz/Desktop/nas/orangepi5pro-01_cam01/2026-05-14/142343"
}
```

### 8.5 停止录制

```bash
curl -X POST \
  "http://192.168.66.196:8080/api/record/stop?sender_id=orangepi5pro-01&camera_id=cam01"
```

## 9. Python 调用示例

```python
import time
import requests

BASE = "http://192.168.66.196:8080"
SENDER_ID = "orangepi5pro-01"
CAMERA_ID = "cam01"

status = requests.get(f"{BASE}/api/status", timeout=3).json()
print(status["cameras"])

requests.post(
    f"{BASE}/api/record/start",
    params={"sender_id": SENDER_ID, "camera_id": CAMERA_ID},
    timeout=3,
)

time.sleep(10)

requests.post(
    f"{BASE}/api/record/stop",
    params={"sender_id": SENDER_ID, "camera_id": CAMERA_ID},
    timeout=3,
)
```

保存预览图：

```python
import requests

BASE = "http://192.168.66.196:8080"
params = {"sender_id": "orangepi5pro-01", "camera_id": "cam01"}

rgb = requests.get(f"{BASE}/api/preview/rgb", params=params, timeout=3)
rgb.raise_for_status()
open("rgb_preview.jpg", "wb").write(rgb.content)

depth = requests.get(f"{BASE}/api/preview/depth", params=params, timeout=3)
depth.raise_for_status()
open("depth_preview.bmp", "wb").write(depth.content)
```

## 10. 错误和限制

### 10.1 常见 HTTP 状态

| 状态码 | 含义 |
|---:|---|
| `200` | 请求成功。 |
| `404` | 预览不可用，或相机 ID 不存在。 |
| `502` | Web 代理无法访问本机 C++ 管理接口。 |

### 10.2 常见问题

页面能打开但画面不动：

- 调用 `/api/status`。
- 检查 `rgb_packets` / `depth_packets` 是否增长。
- 如果不增长，说明发送端媒体流没有进入接收端。

`/api/preview/rgb` 返回 404：

- 发送端可能还没输出可解码 H.264 帧。
- 接收端刚启动时可能需要等待新的关键帧。
- 先检查 `/api/status` 中 `rgb_preview_available`。

`/api/preview/depth` 返回 404：

- 发送端可能没有发送 Depth。
- 先检查 `/api/status` 中 `depth_preview_available`。

开始录制后 `segment_active` 仍为 `false`：

- 录制请求已记录，但还没有媒体包进来。
- 等发送端开始发送 RGB/Depth 后，segment 才会真正创建并写入。

### 10.3 安全限制

当前接口没有鉴权。不要直接暴露到公网。如果必须跨网访问，应先加 VPN、反向代理鉴权或防火墙白名单。
