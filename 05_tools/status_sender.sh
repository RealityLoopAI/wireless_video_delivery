#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PID_FILE="$ROOT_DIR/12_build/sender.pid"
LOG_FILE="$ROOT_DIR/08_reports/sender_logs/sender.log"

if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
  echo "发送端运行中，PID=$(cat "$PID_FILE")"
else
  echo "发送端未运行"
fi

if [[ -f "$LOG_FILE" ]]; then
  echo "最近日志："
  tail -n 20 "$LOG_FILE"
else
  echo "暂无发送端日志"
fi
