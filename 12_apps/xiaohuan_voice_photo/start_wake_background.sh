#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PID_FILE="$ROOT_DIR/wake_runtime.pid"
LOG_FILE="$ROOT_DIR/wake_runtime.log"

if [[ -s "$PID_FILE" ]]; then
  OLD_PID="$(cat "$PID_FILE" 2>/dev/null || true)"
  if [[ -n "$OLD_PID" ]] && kill -0 "$OLD_PID" 2>/dev/null; then
    echo "wake listener already running, pid=$OLD_PID"
    exit 0
  fi
fi

cd "$ROOT_DIR"
setsid "$ROOT_DIR/run_wake_service.sh" > /dev/null 2>&1 < /dev/null &

PID="$!"
echo "$PID" > "$PID_FILE"
echo "wake listener started, pid=$PID"
echo "log: $LOG_FILE"
