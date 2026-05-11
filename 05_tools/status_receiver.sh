#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/12_build"
CONFIG="${1:-$ROOT_DIR/06_configs/receiver_ubuntu-01.json}"

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

show_pid() {
  local name="$1"
  local file="$2"
  if [[ -f "$file" ]] && kill -0 "$(cat "$file")" 2>/dev/null; then
    echo "$name 运行中，PID=$(cat "$file")"
  else
    echo "$name 未运行"
  fi
}

show_pid "C++ 接收端" "$BUILD_DIR/receiver.pid"
show_pid "Web 监控" "$BUILD_DIR/web_monitor.pid"

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
