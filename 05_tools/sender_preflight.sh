#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/12_build/bin/gemini_sender"
CONFIG="${1:-$ROOT_DIR/06_configs/sender_rk3588-01_one_camera.json}"
MODE="${2:-启动}"
PREVIEW_MODE="${3:-config}"
SDK_LIB="$ROOT_DIR/11_third_party/orbbec/linux_arm64/OrbbecSDK_C_C++_v1.10.27_20250925_0549823_linux_arm64_release/OrbbecSDK_v1.10.27/SDK/lib"
SDK_CONFIG="$ROOT_DIR/12_build/bin/OrbbecSDKConfig_v1.0.xml"
source "$ROOT_DIR/05_tools/sender_wifi_guard.sh"

fail() {
  echo "发送端${MODE}检查失败：$1" >&2
  exit 1
}

[[ -x "$BIN" ]] || fail "未找到可执行文件 $BIN，请先执行 cmake 构建"
[[ -f "$CONFIG" ]] || fail "配置文件不存在 $CONFIG"
[[ -d "$SDK_LIB" ]] || fail "Orbbec SDK 动态库目录不存在 $SDK_LIB"
[[ -f "$SDK_LIB/libOrbbecSDK.so" ]] || fail "Orbbec SDK 动态库不存在"
[[ -f "$SDK_CONFIG" ]] || fail "Orbbec SDK 运行配置不存在 $SDK_CONFIG"
command -v python3 >/dev/null 2>&1 || fail "未找到 python3，无法解析配置"
command -v gst-inspect-1.0 >/dev/null 2>&1 || fail "GStreamer 未安装"
command -v ip >/dev/null 2>&1 || fail "未找到 ip 命令，无法检查到接收端的路由"
command -v lsusb >/dev/null 2>&1 || fail "未找到 lsusb，无法检查相机 USB 设备"

config_exports="$(
  python3 - "$CONFIG" <<'PY'
import json
import os
import re
import shlex
import sys

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as f:
    cfg = json.load(f)

cams = cfg.get("cameras") or []
if not cams:
    raise SystemExit("配置中没有 cameras")
cam = cams[0]
receiver = cfg.get("receiver") or {}
top_transport = cfg.get("transport") or {}
rgb = cam.get("rgb_profile") or {}
depth = cam.get("depth_profile") or {}
encoding = cam.get("rgb_encoding") or {}
transport = cam.get("depth_transport") or {}
color_controls = cam.get("color_controls") or {}
preview = cfg.get("preview") or {}
hotplug = cfg.get("hotplug") or {}

required = {
    "sender_id": cfg.get("sender_id", ""),
    "camera_id": cam.get("camera_id", ""),
    "receiver_ip": receiver.get("ip", ""),
    "media_port": receiver.get("media_port", ""),
    "status_port": receiver.get("status_port", ""),
}
missing = [k for k, v in required.items() if v in ("", None)]
if missing:
    raise SystemExit("配置缺少字段: " + ", ".join(missing))

def _sanitize_protocol_part(value):
    value = "".join(ch.lower() if ch.isalnum() or ch in "_-" else "-" for ch in value)
    value = value.strip("-")
    return value or "sender"

def _read_first_line(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return handle.readline().strip()
    except Exception:
        return ""

def _default_route_interface():
    try:
        with open("/proc/net/route", "r", encoding="utf-8") as handle:
            next(handle, None)
            for line in handle:
                fields = line.split()
                if len(fields) >= 2 and fields[1] == "00000000":
                    return fields[0]
    except Exception:
        pass
    return ""

def _valid_mac(value):
    return bool(re.fullmatch(r"[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}", value)) and value.replace(":", "") != "000000000000"

def _mac_for_interface(name):
    value = _read_first_line(f"/sys/class/net/{name}/address")
    return value if _valid_mac(value) else ""

def _first_available_mac():
    route_iface = _default_route_interface()
    if route_iface:
        mac = _mac_for_interface(route_iface)
        if mac:
            return mac
    try:
        names = sorted(os.listdir("/sys/class/net"))
    except Exception:
        names = []
    for name in names:
        if name == "lo":
            continue
        mac = _mac_for_interface(name)
        if mac:
            return mac
    return ""

def _machine_id_suffix():
    value = "".join(ch.lower() for ch in _read_first_line("/etc/machine-id") if ch.lower() in "0123456789abcdef")
    return value[-8:] if value else ""

def _derive_auto_sender_id():
    mac = _first_available_mac()
    suffix = mac.replace(":", "").lower()[-8:] if mac else _machine_id_suffix()
    if not suffix:
        return "auto"
    prefix = _sanitize_protocol_part(os.uname().nodename)
    if len(prefix) + 1 + len(suffix) > 64:
        prefix = prefix[:64 - len(suffix) - 1]
    return f"{prefix}-{suffix}"

sender_id = cfg.get("sender_id", "")
effective_sender_id = _derive_auto_sender_id() if sender_id == "auto" else sender_id

values = {
    "SENDER_ID": effective_sender_id,
    "CONFIG_SENDER_ID": sender_id,
    "SENDER_VERSION": cfg.get("sender_version", ""),
    "CAMERA_COUNT": len(cams),
    "CAMERA_ID": cam.get("camera_id", ""),
    "CAMERA_SERIAL": cam.get("serial_number", ""),
    "RECEIVER_IP": receiver.get("ip", ""),
    "MEDIA_PORT": receiver.get("media_port", ""),
    "STATUS_PORT": receiver.get("status_port", ""),
    "MEDIA_PROTOCOL": top_transport.get("media_protocol", "tcp"),
    "STATUS_PROTOCOL": top_transport.get("status_protocol", "udp"),
    "CONNECT_TIMEOUT_MS": top_transport.get("connect_timeout_ms", ""),
    "SEND_TIMEOUT_MS": top_transport.get("send_timeout_ms", ""),
    "SEND_BUFFER_BYTES": top_transport.get("send_buffer_bytes", ""),
    "RECONNECT_INTERVAL_MS": top_transport.get("reconnect_interval_ms", ""),
    "HEARTBEAT_INTERVAL_MS": cfg.get("heartbeat_interval_ms", ""),
    "RGB_WIDTH": rgb.get("width", ""),
    "RGB_HEIGHT": rgb.get("height", ""),
    "RGB_FPS": rgb.get("fps", ""),
    "RGB_FORMAT": rgb.get("format", ""),
    "RGB_CODEC": encoding.get("codec", ""),
    "RGB_ENCODER": encoding.get("gstreamer_encoder", ""),
    "RGB_BITRATE": encoding.get("bitrate_bps", ""),
    "DEPTH_WIDTH": depth.get("width", ""),
    "DEPTH_HEIGHT": depth.get("height", ""),
    "DEPTH_FPS": depth.get("fps", ""),
    "DEPTH_FORMAT": depth.get("format", ""),
    "DEPTH_COMPRESSION": transport.get("compression", ""),
    "COLOR_CONTROLS": " ".join(f"{key}={str(value).lower() if isinstance(value, bool) else value}" for key, value in color_controls.items()) or "未配置",
    "PREVIEW_ENABLED": preview.get("enabled", ""),
    "PREVIEW_FPS": preview.get("fps", ""),
    "HOTPLUG_ENABLED": hotplug.get("enabled", True),
    "LOG_DIRECTORY": (cfg.get("logging") or {}).get("directory", ""),
    "LOG_MAX_BYTES": (cfg.get("logging") or {}).get("max_bytes", ""),
}

camera_summaries = []
for item in cams:
    rgb_item = item.get("rgb_profile") or {}
    depth_item = item.get("depth_profile") or {}
    enc_item = item.get("rgb_encoding") or {}
    depth_transport_item = item.get("depth_transport") or {}
    controls_item = item.get("color_controls") or {}
    controls_summary = ",".join(
        f"{key}={str(value).lower() if isinstance(value, bool) else value}" for key, value in controls_item.items()
    ) or "未配置"
    camera_summaries.append(
        "{camera_id} serial={serial} rgb={rgb_w}x{rgb_h}@{rgb_fps} {rgb_fmt}->{codec}/{encoder} depth={depth_w}x{depth_h}@{depth_fps} {depth_fmt} compression={compression} color_controls={controls}".format(
            camera_id=item.get("camera_id", ""),
            serial=item.get("serial_number", "") or "未指定",
            rgb_w=rgb_item.get("width", ""),
            rgb_h=rgb_item.get("height", ""),
            rgb_fps=rgb_item.get("fps", ""),
            rgb_fmt=rgb_item.get("format", ""),
            codec=enc_item.get("codec", ""),
            encoder=enc_item.get("gstreamer_encoder", ""),
            depth_w=depth_item.get("width", ""),
            depth_h=depth_item.get("height", ""),
            depth_fps=depth_item.get("fps", ""),
            depth_fmt=depth_item.get("format", ""),
            compression=depth_transport_item.get("compression", ""),
            controls=controls_summary,
        )
    )
values["CAMERA_SUMMARY"] = " | ".join(camera_summaries)

for key, value in values.items():
    if isinstance(value, bool):
        value = str(value).lower()
    print(f"{key}={shlex.quote(str(value))}")
PY
)" || fail "$config_exports"

eval "$config_exports"

[[ -n "${RGB_ENCODER:-}" ]] || fail "配置中没有 rgb_encoding.gstreamer_encoder"
rockchipmpp_features="$(gst-inspect-1.0 rockchipmpp 2>/dev/null || true)"
if ! printf '%s\n' "$rockchipmpp_features" | grep -Fq "$RGB_ENCODER:"; then
  fail "未找到硬件编码插件 $RGB_ENCODER"
fi
if [[ "$RGB_FORMAT" == "mjpg" ]] && ! printf '%s\n' "$rockchipmpp_features" | grep -Fq 'mppjpegdec:'; then
  fail "RGB MJPG 直通需要 mppjpegdec，但当前 GStreamer 未发现该插件"
fi

validate_output="$(
  LD_LIBRARY_PATH="$SDK_LIB:${LD_LIBRARY_PATH:-}" "$BIN" --config "$CONFIG" --validate-config 2>&1
)" || fail "$validate_output"

USB_WARNING=""
if ! lsusb | grep -q '2bc5:'; then
  if [[ "${GEMINI_SENDER_REQUIRE_USB:-1}" == "0" ]]; then
    USB_WARNING="未发现 Orbbec USB 设备；守护进程会等待相机重新枚举后重试"
  else
    fail "未发现 Orbbec USB 设备。请检查相机 USB 连接"
  fi
fi

USBFS_MEMORY_MB=""
TCP_WMEM_MAX=""
TCP_WMEM_DEFAULT=""
if [[ -r /sys/module/usbcore/parameters/usbfs_memory_mb ]]; then
  USBFS_MEMORY_MB="$(cat /sys/module/usbcore/parameters/usbfs_memory_mb 2>/dev/null || true)"
fi
if [[ -r /proc/sys/net/core/wmem_max ]]; then
  TCP_WMEM_MAX="$(cat /proc/sys/net/core/wmem_max 2>/dev/null || true)"
fi
if [[ -r /proc/sys/net/core/wmem_default ]]; then
  TCP_WMEM_DEFAULT="$(cat /proc/sys/net/core/wmem_default 2>/dev/null || true)"
fi

if gemini_sender_wifi_required; then
  gemini_sender_wifi_connect_if_configured || fail "$GEMINI_SENDER_WIFI_LAST_ERROR"
  gemini_sender_wifi_check_policy || fail "$GEMINI_SENDER_WIFI_LAST_ERROR"
fi

route_output="$(ip route get "$RECEIVER_IP" 2>/dev/null || true)"
[[ -n "$route_output" ]] || fail "无法找到到接收端 $RECEIVER_IP 的路由"

receiver_status_warning="$(
  python3 - "$RECEIVER_IP" "$SENDER_ID" <<'PY' 2>/dev/null || true
import json
import os
import sys
import urllib.request

receiver_ip = sys.argv[1]
sender_id = sys.argv[2]
urls = []
if os.environ.get("GEMINI_RECEIVER_STATUS_URL"):
    urls.append(os.environ["GEMINI_RECEIVER_STATUS_URL"])
urls.append(f"http://{receiver_ip}:8080/api/status")

status = None
for url in urls:
    try:
        with urllib.request.urlopen(url, timeout=1.5) as resp:
            status = json.load(resp)
        break
    except Exception:
        continue

if not isinstance(status, dict):
    raise SystemExit(0)

active_clients = status.get("active_media_clients")
if isinstance(active_clients, int) and active_clients > 1:
    print(f"receiver currently has {active_clients} active media clients; current one-stream delivery expectation may be impacted")

conflicts = []
for cam in status.get("cameras", []):
    if cam.get("sender_id") != sender_id:
        continue
    if not (cam.get("status_live") or cam.get("media_live")):
        continue
    key = cam.get("camera_key") or f"{cam.get('sender_id', '')}_{cam.get('camera_id', '')}"
    status_age = cam.get("status_age_ms", -1)
    media_age = cam.get("media_age_ms", -1)
    conflicts.append(f"{key}(status_live={cam.get('status_live')},media_live={cam.get('media_live')},status_age_ms={status_age},media_age_ms={media_age})")

if conflicts:
    print("receiver already has live cameras with this sender_id; expected only if this sender is already running, otherwise it is an ID conflict: " + ", ".join(conflicts))
PY
)"

echo "发送端${MODE}参数："
echo "  config: $CONFIG"
echo "  sender_id: $SENDER_ID  version: ${SENDER_VERSION:-未指定}"
if [[ "${CONFIG_SENDER_ID:-}" == "auto" ]]; then
  echo "  config_sender_id: auto"
fi
echo "  receiver: $RECEIVER_IP  media/${MEDIA_PROTOCOL}=$MEDIA_PORT  status/${STATUS_PROTOCOL}=$STATUS_PORT"
echo "  transport: connect_timeout=${CONNECT_TIMEOUT_MS:-未指定}ms  send_timeout=${SEND_TIMEOUT_MS:-未指定}ms  send_buffer=${SEND_BUFFER_BYTES:-未指定}B  reconnect_interval=${RECONNECT_INTERVAL_MS:-未指定}ms"
echo "  heartbeat: ${HEARTBEAT_INTERVAL_MS:-未指定}ms"
echo "  camera_count: $CAMERA_COUNT"
echo "  cameras: $CAMERA_SUMMARY"
echo "  RGB: ${RGB_WIDTH}x${RGB_HEIGHT}@${RGB_FPS} ${RGB_FORMAT} -> ${RGB_CODEC}/${RGB_ENCODER} ${RGB_BITRATE}bps"
echo "  Depth: ${DEPTH_WIDTH}x${DEPTH_HEIGHT}@${DEPTH_FPS} ${DEPTH_FORMAT} compression=${DEPTH_COMPRESSION}"
echo "  color_controls: ${COLOR_CONTROLS:-未配置}"
echo "  config_preview: enabled=${PREVIEW_ENABLED} fps=${PREVIEW_FPS}"
echo "  hotplug: enabled=${HOTPLUG_ENABLED:-true}"
case "$PREVIEW_MODE" in
  no-preview)
    echo "  launch_preview: disabled by --no-preview"
    ;;
  *)
    echo "  launch_preview: follows config"
    ;;
esac
echo "  log: ${LOG_DIRECTORY:-未指定} max_bytes=${LOG_MAX_BYTES:-未指定}"
if gemini_sender_wifi_required; then
  echo "  wifi_guard: $(gemini_sender_wifi_policy_summary)"
fi
echo "  route: $route_output"

if echo "$route_output" | grep -q ' dev wlan0 '; then
  iw dev wlan0 link 2>/dev/null | sed 's/^/  wifi: /' || true
fi
if [[ -n "$receiver_status_warning" ]]; then
  while IFS= read -r line; do
    [[ -n "$line" ]] && echo "  warning: $line"
  done <<< "$receiver_status_warning"
fi
if [[ -n "$USB_WARNING" ]]; then
  echo "  usb: $USB_WARNING"
fi
if [[ -n "$USBFS_MEMORY_MB" ]]; then
  echo "  usbfs_memory_mb: $USBFS_MEMORY_MB"
  if [[ "$USBFS_MEMORY_MB" =~ ^[0-9]+$ ]] && (( USBFS_MEMORY_MB < 256 )); then
    echo "  warning: usbfs_memory_mb 低于多路满规格建议值 256，Orbbec SDK 可能无法分配 USB 传输缓冲"
  fi
fi
if [[ -n "$TCP_WMEM_MAX" ]]; then
  echo "  tcp_wmem: default=${TCP_WMEM_DEFAULT:-unknown} max=$TCP_WMEM_MAX"
  if [[ "$TCP_WMEM_MAX" =~ ^[0-9]+$ ]] && (( TCP_WMEM_MAX < 4194304 )); then
    echo "  warning: net.core.wmem_max 低于配置发送缓冲，多路或最高 Depth 档发送可能出现 TCP 背压丢帧"
  fi
fi

echo "  validate: ok"
