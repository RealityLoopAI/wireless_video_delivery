#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/12_build"
CONFIG="${1:-$ROOT_DIR/06_configs/receiver_ubuntu-01.json}"
RECEIVER_UNIT="gwv3-gemini-receiver.service"
WEB_UNIT="gwv3-web-monitor.service"
UPLOADER_UNIT="gwv3-recording-uploader.service"

read_config_field() {
  python3 - "$CONFIG" "$1" "$2" <<'PY'
import json
import sys

config_path, key, fallback = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    with open(config_path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    print(data.get(key, fallback))
except Exception:
    print(fallback)
PY
}

ADMIN_PORT="$(read_config_field admin_port 18080)"

systemd_user_available() {
  command -v systemctl >/dev/null 2>&1 && systemctl --user status >/dev/null 2>&1
}

unit_main_pid() {
  systemctl --user show "$1" --property=MainPID --value 2>/dev/null | awk '{print $1}'
}

show_pid() {
  local name="$1"
  local file="$2"
  local unit="${3:-}"
  if [[ -n "$unit" ]] && systemd_user_available && systemctl --user is-active --quiet "$unit" 2>/dev/null; then
    local pid
    pid="$(unit_main_pid "$unit")"
    [[ "$pid" =~ ^[0-9]+$ ]] && ((pid > 0)) && echo "$pid" > "$file"
    echo "$name 运行中，PID=${pid:-unknown}，systemd=$unit"
  elif [[ -f "$file" ]] && kill -0 "$(cat "$file")" 2>/dev/null; then
    echo "$name 运行中，PID=$(cat "$file")"
  else
    echo "$name 未运行"
  fi
}

show_pid "C++ 接收端" "$BUILD_DIR/receiver.pid" "$RECEIVER_UNIT"
show_pid "Web 监控" "$BUILD_DIR/web_monitor.pid" "$WEB_UNIT"
if systemd_user_available && systemctl --user is-active --quiet "$UPLOADER_UNIT" 2>/dev/null; then
  echo "录制上传器运行中，PID=$(unit_main_pid "$UPLOADER_UNIT")，systemd=$UPLOADER_UNIT"
else
  echo "录制上传器未运行"
fi

ADMIN_PORT="$ADMIN_PORT" python3 - <<'PY'
import json
import os
import urllib.request

try:
    admin_port = os.environ.get("ADMIN_PORT", "18080")
    with urllib.request.urlopen(f"http://127.0.0.1:{admin_port}/api/status", timeout=2) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    print("接收端状态：")
    print(json.dumps(data, ensure_ascii=False, indent=2))
except Exception as exc:
    print(f"接收端管理接口不可用：{exc}")
PY

LOG="$ROOT_DIR/08_reports/receiver_logs/receiver.log"
if [[ -f "$LOG" ]]; then
  echo "最近日志："
  tail -n 20 "$LOG"
fi
