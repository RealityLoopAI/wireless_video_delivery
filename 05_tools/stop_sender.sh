#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PID_FILE="$ROOT_DIR/12_build/sender.pid"
CHILD_PID_FILE="$ROOT_DIR/12_build/sender_child.pid"
BIN="$ROOT_DIR/12_build/bin/gemini_sender"

stop_orphan_children() {
  local orphan_pids
  orphan_pids="$(pgrep -f "^$BIN --config " 2>/dev/null || true)"
  if [[ -n "$orphan_pids" ]]; then
    echo "$orphan_pids" | xargs -r kill 2>/dev/null || true
    echo "已请求停止残留发送端子进程：$orphan_pids"
  fi
}

if [[ ! -f "$PID_FILE" ]]; then
  if [[ -f "$CHILD_PID_FILE" ]] && kill -0 "$(cat "$CHILD_PID_FILE")" 2>/dev/null; then
    kill "$(cat "$CHILD_PID_FILE")" 2>/dev/null || true
    rm -f "$CHILD_PID_FILE"
    echo "发送端子进程已停止"
  else
    stop_orphan_children
    echo "发送端未运行：没有 PID 文件"
  fi
  exit 0
fi

PID="$(cat "$PID_FILE")"
if kill -0 "$PID" 2>/dev/null; then
  if [[ -f "$CHILD_PID_FILE" ]] && kill -0 "$(cat "$CHILD_PID_FILE")" 2>/dev/null; then
    kill "$(cat "$CHILD_PID_FILE")" 2>/dev/null || true
  fi
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

stop_orphan_children
rm -f "$PID_FILE" "$CHILD_PID_FILE"
