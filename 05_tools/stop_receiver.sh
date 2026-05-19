#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/12_build"

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

stop_pid_file "Web 监控" "$BUILD_DIR/web_monitor.pid"
stop_pid_file "C++ 接收端" "$BUILD_DIR/receiver.pid" 1
