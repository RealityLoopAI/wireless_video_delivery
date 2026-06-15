#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-}"
PID_FILE="$ROOT_DIR/12_build/sender.pid"
CHILD_PID_FILE="$ROOT_DIR/12_build/sender_child.pid"
LOG_FILE="$ROOT_DIR/08_reports/sender_logs/sender.log"

config_from_cmdline() {
  local pid="$1"
  local candidate=""
  local prev=""

  [[ -r "/proc/$pid/cmdline" ]] || return 1

  while IFS= read -r arg; do
    if [[ "$prev" == "--config" ]]; then
      printf '%s\n' "$arg"
      return 0
    fi
    if [[ "$arg" == *.json ]]; then
      candidate="$arg"
    fi
    prev="$arg"
  done < <(tr '\0' '\n' < "/proc/$pid/cmdline")

  [[ -n "$candidate" ]] || return 1
  printf '%s\n' "$candidate"
}

if [[ -z "$CONFIG" ]]; then
  if [[ -f "$CHILD_PID_FILE" ]] && kill -0 "$(cat "$CHILD_PID_FILE")" 2>/dev/null; then
    CONFIG="$(config_from_cmdline "$(cat "$CHILD_PID_FILE")" || true)"
  fi
  if [[ -z "$CONFIG" ]] && [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
    CONFIG="$(config_from_cmdline "$(cat "$PID_FILE")" || true)"
  fi
  CONFIG="${CONFIG:-$ROOT_DIR/06_configs/sender_rk3588-01_one_camera.json}"
fi

"$ROOT_DIR/05_tools/sender_preflight.sh" "$CONFIG" "状态查看" "config" || true
echo

if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
  PID="$(cat "$PID_FILE")"
  echo "发送端守护运行中，watchdog PID=$PID"
  if [[ -r "/proc/$PID/cmdline" ]]; then
    echo -n "守护命令："
    tr '\0' ' ' < "/proc/$PID/cmdline"
    echo
  fi
  if [[ -f "$CHILD_PID_FILE" ]] && kill -0 "$(cat "$CHILD_PID_FILE")" 2>/dev/null; then
    CHILD_PID="$(cat "$CHILD_PID_FILE")"
    echo "发送端子进程运行中，PID=$CHILD_PID"
    if [[ -r "/proc/$CHILD_PID/cmdline" ]]; then
      echo -n "发送命令："
      tr '\0' ' ' < "/proc/$CHILD_PID/cmdline"
      echo
    fi
  else
    echo "发送端子进程暂未运行，守护进程会继续尝试重启"
  fi
else
  echo "发送端未运行"
fi

if command -v ss >/dev/null 2>&1; then
  echo "媒体连接："
  ss -tnp 2>/dev/null | grep -E '(:50010|gemini_sender)' || echo "  未发现当前 TCP 媒体连接"
fi

if [[ -f "$LOG_FILE" ]]; then
  echo "最近日志："
  tail -n 20 "$LOG_FILE"
else
  echo "暂无发送端日志"
fi
