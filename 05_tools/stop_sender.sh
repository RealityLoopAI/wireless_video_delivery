#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PID_FILE="$ROOT_DIR/12_build/sender.pid"

if [[ ! -f "$PID_FILE" ]]; then
  echo "发送端未运行：没有 PID 文件"
  exit 0
fi

PID="$(cat "$PID_FILE")"
if kill -0 "$PID" 2>/dev/null; then
  kill "$PID"
  for _ in {1..20}; do
    if ! kill -0 "$PID" 2>/dev/null; then
      break
    fi
    sleep 0.2
  done
  if kill -0 "$PID" 2>/dev/null; then
    echo "发送端停止超时，尝试强制停止 PID=$PID"
    kill -9 "$PID" 2>/dev/null || true
  fi
  echo "发送端已停止"
else
  echo "发送端未运行：PID=$PID 不存在"
fi

rm -f "$PID_FILE"
