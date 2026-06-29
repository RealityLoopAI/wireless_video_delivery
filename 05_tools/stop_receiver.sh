#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/12_build"
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
    systemctl --user stop "$unit"
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
      if ((${#descendants[@]} > 0)); then
        kill "${descendants[@]}" 2>/dev/null || true
        sleep 0.5
        kill -9 "${descendants[@]}" 2>/dev/null || true
      fi
      kill -9 "$pid" 2>/dev/null || true
    elif ((${#descendants[@]} > 0)); then
      kill "${descendants[@]}" 2>/dev/null || true
      sleep 0.5
      kill -9 "${descendants[@]}" 2>/dev/null || true
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
  kill -9 "${pids[@]}" 2>/dev/null || true
  echo "$name 残留进程已清理：${pids[*]}"
}

stop_unit_if_active "Web 监控" "$WEB_UNIT" "$BUILD_DIR/web_monitor.pid" || stop_pid_file "Web 监控" "$BUILD_DIR/web_monitor.pid"
stop_matching_processes "Web 监控" "$ROOT_DIR/09_web_monitor/.venv/bin/python -m uvicorn server:app"
stop_unit_if_active "C++ 接收端" "$RECEIVER_UNIT" "$BUILD_DIR/receiver.pid" || stop_pid_file "C++ 接收端" "$BUILD_DIR/receiver.pid" 1
stop_matching_processes "C++ 接收端" "$ROOT_DIR/12_build/bin/gemini_receiver --config"
