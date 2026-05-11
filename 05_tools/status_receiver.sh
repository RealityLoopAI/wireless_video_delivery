#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/12_build"

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

python3 - <<'PY'
import json
import urllib.request

try:
    with urllib.request.urlopen("http://127.0.0.1:18080/api/status", timeout=2) as resp:
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
