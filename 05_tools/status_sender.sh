#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-$ROOT_DIR/06_configs/sender_rk3588-01_one_camera.json}"
PID_FILE="$ROOT_DIR/12_build/sender.pid"
CHILD_PID_FILE="$ROOT_DIR/12_build/sender_child.pid"
LOG_FILE="$ROOT_DIR/08_reports/sender_logs/sender.log"

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
  echo "媒体 TCP 连接："
  ss -tnp 2>/dev/null | grep -E '(:50010|gemini_sender)' || echo "  未发现当前 TCP 媒体连接"
  echo "媒体 UDP socket："
  ss -aunp 2>/dev/null | grep -E '(gemini_sender|:50012|:50013)' || echo "  未发现当前 UDP 媒体 socket"
fi

if [[ -f "$LOG_FILE" ]]; then
  echo "最近日志："
  tail -n 20 "$LOG_FILE"
else
  echo "暂无发送端日志"
fi
