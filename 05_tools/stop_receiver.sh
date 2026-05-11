#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/12_build"

stop_pid_file() {
  local name="$1"
  local file="$2"
  if [[ ! -f "$file" ]]; then
    echo "$name 未运行：没有 PID 文件"
    return
  fi
  local pid
  pid="$(cat "$file")"
  if kill -0 "$pid" 2>/dev/null; then
    kill "$pid"
    for _ in {1..30}; do
      if ! kill -0 "$pid" 2>/dev/null; then
        break
      fi
      sleep 0.2
    done
    if kill -0 "$pid" 2>/dev/null; then
      echo "$name 停止超时，强制停止 PID=$pid"
      kill -9 "$pid" 2>/dev/null || true
    fi
    echo "$name 已停止"
  else
    echo "$name 未运行：PID=$pid 不存在"
  fi
  rm -f "$file"
}

stop_pid_file "Web 监控" "$BUILD_DIR/web_monitor.pid"
stop_pid_file "C++ 接收端" "$BUILD_DIR/receiver.pid"
