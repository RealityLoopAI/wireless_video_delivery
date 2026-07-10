#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/12_build"
CONFIG="${1:-$ROOT_DIR/06_configs/receiver_ubuntu-01.json}"
RECEIVER_UNIT="gwv3-gemini-receiver.service"
WEB_UNIT="gwv3-web-monitor.service"

systemd_user_available() {
  command -v systemctl >/dev/null 2>&1 && systemctl --user status >/dev/null 2>&1
}

stop_unit_if_active() {
  local name="$1"
  local unit="$2"
  local file="$3"
  if systemd_user_available && systemctl --user is-active --quiet "$unit" 2>/dev/null; then
    if ! systemctl --user stop "$unit"; then
      echo "$name systemd 停止请求未正常完成，继续检查残留进程"
    fi
    systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
    rm -f "$file"
    echo "$name 已停止"
    return 0
  fi
  return 1
}

collect_descendants() {
  local pid="$1"
  local child
  while read -r child; do
    [[ -z "$child" ]] && continue
    collect_descendants "$child"
    echo "$child"
  done < <(pgrep -P "$pid" 2>/dev/null || true)
}

stop_pid_file() {
  local name="$1"
  local file="$2"
  local kill_tree="${3:-0}"
  local force_kill="${4:-1}"
  if [[ ! -f "$file" ]]; then
    echo "$name 未运行：没有 PID 文件"
    return
  fi
  local pid
  pid="$(cat "$file")"
  if kill -0 "$pid" 2>/dev/null; then
    local descendants=()
    if [[ "$kill_tree" == "1" ]]; then
      mapfile -t descendants < <(collect_descendants "$pid")
    fi
    kill "$pid"
    for _ in {1..30}; do
      if ! kill -0 "$pid" 2>/dev/null; then
        break
      fi
      sleep 0.2
    done
    if kill -0 "$pid" 2>/dev/null; then
      echo "$name 停止超时，强制停止 PID=$pid"
      if [[ "$force_kill" == "1" ]]; then
        if ((${#descendants[@]} > 0)); then
          kill "${descendants[@]}" 2>/dev/null || true
          sleep 0.5
          kill -9 "${descendants[@]}" 2>/dev/null || true
        fi
        kill -9 "$pid" 2>/dev/null || true
      else
        echo "$name 仍在运行，已跳过 SIGKILL 以避免打断录制收尾"
      fi
    elif ((${#descendants[@]} > 0)); then
      kill "${descendants[@]}" 2>/dev/null || true
      sleep 0.5
      if [[ "$force_kill" == "1" ]]; then
        kill -9 "${descendants[@]}" 2>/dev/null || true
      fi
    fi
    echo "$name 已停止"
  else
    echo "$name 未运行：PID=$pid 不存在"
  fi
  rm -f "$file"
}

stop_matching_processes() {
  local name="$1"
  local pattern="$2"
  local force_kill="${3:-1}"
  mapfile -t pids < <(pgrep -f "$pattern" 2>/dev/null || true)
  if ((${#pids[@]} == 0)); then
    return 0
  fi
  kill "${pids[@]}" 2>/dev/null || true
  for _ in {1..20}; do
    local alive=0
    for pid in "${pids[@]}"; do
      if kill -0 "$pid" 2>/dev/null; then
        alive=1
        break
      fi
    done
    ((alive == 0)) && break
    sleep 0.2
  done
  if [[ "$force_kill" == "1" ]]; then
    kill -9 "${pids[@]}" 2>/dev/null || true
  else
    local still_alive=()
    for pid in "${pids[@]}"; do
      if kill -0 "$pid" 2>/dev/null; then
        still_alive+=("$pid")
      fi
    done
    if ((${#still_alive[@]} > 0)); then
      echo "$name 残留进程仍在运行，已跳过 SIGKILL 以避免打断录制收尾：${still_alive[*]}"
      return 0
    fi
  fi
  echo "$name 残留进程已清理：${pids[*]}"
}

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

status_is_idle() {
  python3 -c '
import json
import sys

try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(1)

busy = bool(data.get("recording_all"))
for cam in data.get("cameras", []):
    if cam.get("recording") or cam.get("segment_active") or cam.get("segment_finalizing") or cam.get("record_finalizing"):
        busy = True
    if int(cam.get("record_queue_packets") or 0) > 0 or int(cam.get("record_active_writes") or 0) > 0:
        busy = True
sys.exit(1 if busy else 0)
'
}

request_receiver_record_stop() {
  command -v curl >/dev/null 2>&1 || return 0
  command -v python3 >/dev/null 2>&1 || return 0
  local admin_port
  admin_port="$(read_config_field admin_port 18080)"
  local base_url="http://127.0.0.1:${admin_port}"
  if ! curl -fsS --max-time 3 -X POST "$base_url/api/record/stop-all" >/dev/null 2>&1; then
    return 0
  fi
  echo "已请求接收端停止录制，等待切片收尾..."
  for _ in {1..120}; do
    local status
    status="$(curl -fsS --max-time 3 "$base_url/api/status" 2>/dev/null || true)"
    if [[ -n "$status" ]] && status_is_idle <<<"$status"; then
      echo "接收端录制已空闲"
      return 0
    fi
    sleep 1
  done
  echo "接收端录制收尾等待超时，后续停止将避免 SIGKILL"
}

stop_unit_if_active "Web 监控" "$WEB_UNIT" "$BUILD_DIR/web_monitor.pid" || stop_pid_file "Web 监控" "$BUILD_DIR/web_monitor.pid"
stop_matching_processes "Web 监控" "$ROOT_DIR/09_web_monitor/.venv/bin/python -m uvicorn server:app"
request_receiver_record_stop
stop_unit_if_active "C++ 接收端" "$RECEIVER_UNIT" "$BUILD_DIR/receiver.pid" || stop_pid_file "C++ 接收端" "$BUILD_DIR/receiver.pid" 1 0
stop_matching_processes "C++ 接收端" "$ROOT_DIR/12_build/bin/gemini_receiver --config" 0
