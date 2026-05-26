#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/12_build/bin/gemini_sender"
CONFIG="${1:-$ROOT_DIR/06_configs/sender_orangepi5pro-01_depth_zlib.json}"
MODE="${2:-启动}"
PREVIEW_MODE="${3:-config}"
SDK_LIB="$ROOT_DIR/11_third_party/orbbec/linux_arm64/OrbbecSDK_C_C++_v1.10.27_20250925_0549823_linux_arm64_release/OrbbecSDK_v1.10.27/SDK/lib"
SDK_CONFIG="$ROOT_DIR/12_build/bin/OrbbecSDKConfig_v1.0.xml"

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
import socket
import sys


def _read_first_line(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return f.readline().strip()
    except OSError:
        return ""


def _valid_mac(value):
    return bool(re.fullmatch(r"[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}", value or "")) and value.lower().replace(":", "") != "000000000000"


def _mac_for_interface(name):
    value = _read_first_line(f"/sys/class/net/{name}/address")
    return value if _valid_mac(value) else ""


def _default_route_interface():
    try:
        with open("/proc/net/route", "r", encoding="utf-8") as f:
            next(f, None)
            for line in f:
                fields = line.split()
                if len(fields) >= 2 and fields[1] == "00000000":
                    return fields[0]
    except OSError:
        return ""
    return ""


def _first_available_mac():
    route_iface = _default_route_interface()
    if route_iface:
        mac = _mac_for_interface(route_iface)
        if mac:
            return mac

    try:
        names = sorted(os.listdir("/sys/class/net"))
    except OSError:
        names = []
    for name in names:
        if name == "lo":
            continue
        mac = _mac_for_interface(name)
        if mac:
            return mac
    return ""


def _sanitize_protocol_part(value):
    value = re.sub(r"[^A-Za-z0-9_-]", "-", value or "").strip("-").lower()
    return value or "sender"


def _derive_auto_sender_id():
    mac = _first_available_mac()
    suffix = mac.lower().replace(":", "")[-8:] if mac else ""
    if not suffix:
        machine_id = re.sub(r"[^0-9A-Fa-f]", "", _read_first_line("/etc/machine-id")).lower()
        suffix = machine_id[-8:] if machine_id else ""
    if not suffix:
        raise SystemExit("无法自动生成 sender_id：没有可用网卡 MAC 或 /etc/machine-id")

    prefix = _sanitize_protocol_part(socket.gethostname())
    max_len = 64
    if len(prefix) + 1 + len(suffix) > max_len:
        prefix = prefix[: max_len - len(suffix) - 1]
    return f"{prefix}-{suffix}"

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
preview = cfg.get("preview") or {}

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

sender_id = cfg.get("sender_id", "")
if sender_id == "auto":
    sender_id = _derive_auto_sender_id()

values = {
    "SENDER_ID": sender_id,
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
    "PREVIEW_ENABLED": preview.get("enabled", ""),
    "PREVIEW_FPS": preview.get("fps", ""),
    "LOG_DIRECTORY": (cfg.get("logging") or {}).get("directory", ""),
    "LOG_MAX_BYTES": (cfg.get("logging") or {}).get("max_bytes", ""),
}

camera_summaries = []
for item in cams:
    rgb_item = item.get("rgb_profile") or {}
    depth_item = item.get("depth_profile") or {}
    enc_item = item.get("rgb_encoding") or {}
    depth_transport_item = item.get("depth_transport") or {}
    camera_summaries.append(
        "{camera_id} serial={serial} rgb={rgb_w}x{rgb_h}@{rgb_fps} {rgb_fmt}->{codec}/{encoder} depth={depth_w}x{depth_h}@{depth_fps} {depth_fmt} compression={compression}".format(
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
gst-inspect-1.0 "$RGB_ENCODER" >/dev/null 2>&1 || fail "未找到硬件编码插件 $RGB_ENCODER"

validate_output="$(
  LD_LIBRARY_PATH="$SDK_LIB:${LD_LIBRARY_PATH:-}" "$BIN" --config "$CONFIG" --validate-config 2>&1
)" || fail "$validate_output"

if ! lsusb | grep -q '2bc5:'; then
  fail "未发现 Orbbec USB 设备。请检查相机 USB 连接"
fi

route_output="$(ip route get "$RECEIVER_IP" 2>/dev/null || true)"
[[ -n "$route_output" ]] || fail "无法找到到接收端 $RECEIVER_IP 的路由"

echo "发送端${MODE}参数："
echo "  config: $CONFIG"
echo "  sender_id: $SENDER_ID  version: ${SENDER_VERSION:-未指定}"
echo "  receiver: $RECEIVER_IP  media/${MEDIA_PROTOCOL}=$MEDIA_PORT  status/${STATUS_PROTOCOL}=$STATUS_PORT"
echo "  reconnect: connect_timeout=${CONNECT_TIMEOUT_MS:-未指定}ms  interval=${RECONNECT_INTERVAL_MS:-未指定}ms"
echo "  heartbeat: ${HEARTBEAT_INTERVAL_MS:-未指定}ms"
echo "  camera_count: $CAMERA_COUNT"
echo "  cameras: $CAMERA_SUMMARY"
echo "  RGB: ${RGB_WIDTH}x${RGB_HEIGHT}@${RGB_FPS} ${RGB_FORMAT} -> ${RGB_CODEC}/${RGB_ENCODER} ${RGB_BITRATE}bps"
echo "  Depth: ${DEPTH_WIDTH}x${DEPTH_HEIGHT}@${DEPTH_FPS} ${DEPTH_FORMAT} compression=${DEPTH_COMPRESSION}"
echo "  config_preview: enabled=${PREVIEW_ENABLED} fps=${PREVIEW_FPS}"
case "$PREVIEW_MODE" in
  no-preview)
    echo "  launch_preview: disabled by --no-preview"
    ;;
  *)
    echo "  launch_preview: follows config"
    ;;
esac
echo "  log: ${LOG_DIRECTORY:-未指定} max_bytes=${LOG_MAX_BYTES:-未指定}"
echo "  route: $route_output"

if echo "$route_output" | grep -q ' dev wlan0 '; then
  iw dev wlan0 link 2>/dev/null | sed 's/^/  wifi: /' || true
fi

echo "  validate: ok"
