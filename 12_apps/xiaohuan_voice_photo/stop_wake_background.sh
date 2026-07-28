#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PID_FILE="$ROOT_DIR/wake_runtime.pid"

if [[ ! -s "$PID_FILE" ]]; then
  pkill -f "run_wake_service.sh" 2>/dev/null || true
  pkill -f "python3 -u vosk_wake.py listen" 2>/dev/null || true
  echo "wake listener stopped"
  exit 0
fi

PID="$(cat "$PID_FILE")"
if [[ -n "$PID" ]] && kill -0 "$PID" 2>/dev/null; then
  kill "$PID" 2>/dev/null || true
  sleep 1
  if kill -0 "$PID" 2>/dev/null; then
    kill -9 "$PID" 2>/dev/null || true
  fi
fi

pkill -P "$PID" 2>/dev/null || true
rm -f "$PID_FILE"
echo "wake listener stopped"
