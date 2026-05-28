#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PID_FILE="$ROOT_DIR/12_build/sender.pid"
CHILD_PID_FILE="$ROOT_DIR/12_build/sender_child.pid"
BIN="$ROOT_DIR/12_build/bin/gemini_sender"

stop_pids() {
  local label="$1"
  shift
  local pids=("$@")
  local alive=()

  if [[ "${#pids[@]}" -eq 0 ]]; then
    return
  fi

  kill "${pids[@]}" 2>/dev/null || true
  for _ in {1..20}; do
    alive=()
    for pid in "${pids[@]}"; do
      if kill -0 "$pid" 2>/dev/null; then
        alive+=("$pid")
      fi
    done
    if [[ "${#alive[@]}" -eq 0 ]]; then
      return
    fi
    sleep 0.2
  done

  echo "${label}停止超时，尝试强制停止 PID=${alive[*]}"
  kill -9 "${alive[@]}" 2>/dev/null || true
}

stop_orphan_children() {
  local orphan_pids=()
  mapfile -t orphan_pids < <(pgrep -f "^$BIN --config " 2>/dev/null || true)
  if [[ "${#orphan_pids[@]}" -gt 0 ]]; then
    stop_pids "残留发送端子进程" "${orphan_pids[@]}"
    echo "已停止残留发送端子进程：${orphan_pids[*]}"
  fi
}

if [[ ! -f "$PID_FILE" ]]; then
  if [[ -f "$CHILD_PID_FILE" ]] && kill -0 "$(cat "$CHILD_PID_FILE")" 2>/dev/null; then
    stop_pids "发送端子进程" "$(cat "$CHILD_PID_FILE")"
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
    stop_pids "发送端子进程" "$(cat "$CHILD_PID_FILE")"
  fi
  stop_pids "发送端守护进程" "$PID"
  echo "发送端已停止"
else
  echo "发送端未运行：PID=$PID 不存在"
fi

stop_orphan_children
rm -f "$PID_FILE" "$CHILD_PID_FILE"
