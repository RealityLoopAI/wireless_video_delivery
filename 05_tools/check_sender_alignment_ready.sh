#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-$ROOT_DIR/06_configs/sender_rk3588-01_two_cameras_align.json}"

python3 - "$CONFIG" <<'PY'
import json
import os
import sys
import urllib.request
from pathlib import Path

config_path = Path(sys.argv[1])
with config_path.open("r", encoding="utf-8") as f:
    cfg = json.load(f)

sender_id = str(cfg.get("sender_id") or "")
receiver = cfg.get("receiver") or {}
receiver_ip = str(receiver.get("ip") or "")
cameras = cfg.get("cameras") or []
if not sender_id or not receiver_ip or not cameras:
    raise SystemExit("配置缺少 sender_id、receiver.ip 或 cameras")

urls = []
if os.environ.get("GEMINI_RECEIVER_STATUS_URL"):
    urls.append(os.environ["GEMINI_RECEIVER_STATUS_URL"])
urls.append(f"http://{receiver_ip}:8080/api/status")

status = None
last_error = ""
for url in urls:
    try:
        with urllib.request.urlopen(url, timeout=3) as resp:
            status = json.load(resp)
        break
    except Exception as exc:
        last_error = f"{url}: {exc}"

if not isinstance(status, dict):
    raise SystemExit(f"无法读取接收端状态：{last_error}")

by_key = {cam.get("camera_key"): cam for cam in status.get("cameras", []) if isinstance(cam, dict)}
failed = False
print(f"对齐录制就绪检查：sender_id={sender_id} receiver={receiver_ip}")
print(f"接收端 active_media_clients={status.get('active_media_clients')}")

for item in cameras:
    camera_id = str(item.get("camera_id") or "")
    key = f"{sender_id}_{camera_id}"
    rgb = item.get("rgb_profile") or {}
    depth = item.get("depth_profile") or {}
    expected = {
        "announce_rgb_width": int(rgb.get("width") or 0),
        "announce_rgb_height": int(rgb.get("height") or 0),
        "announce_depth_width": int(depth.get("width") or 0),
        "announce_depth_height": int(depth.get("height") or 0),
    }
    cam = by_key.get(key)
    reasons = []
    if not cam:
        reasons.append("接收端状态中未找到该 camera_key")
    else:
        if not cam.get("live"):
            reasons.append("live=false")
        if not cam.get("announce_live"):
            reasons.append("announce_live=false")
        if not cam.get("calibration_available"):
            reasons.append("calibration_available=false")
        for field, value in expected.items():
            if int(cam.get(field) or 0) != value:
                reasons.append(f"{field}={cam.get(field)} expected={value}")

    if reasons:
        failed = True
        print(f"FAIL {key}: " + "; ".join(reasons))
    else:
        print(
            f"OK   {key}: RGB {expected['announce_rgb_width']}x{expected['announce_rgb_height']} "
            f"Depth {expected['announce_depth_width']}x{expected['announce_depth_height']} "
            f"media_age_ms={cam.get('media_age_ms')} status_age_ms={cam.get('status_age_ms')}"
        )

extra = []
for cam in status.get("cameras", []):
    if not isinstance(cam, dict) or cam.get("sender_id") != sender_id:
        continue
    key = cam.get("camera_key")
    if key not in {f"{sender_id}_{str(item.get('camera_id') or '')}" for item in cameras} and cam.get("live"):
        extra.append(str(key))
if extra:
    print("提示：接收端还有同一 sender_id 的动态热插拔相机在线：" + ", ".join(extra))

if failed:
    raise SystemExit(1)
PY
