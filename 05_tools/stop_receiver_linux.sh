#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

RUNTIME_DIR="${DELIVERY_ROOT}/07_samples/runtime/receiver_linux"
PID_FILE="${RUNTIME_DIR}/receiver.pid"

if [[ ! -f "${PID_FILE}" ]]; then
  echo "[receiver-linux] 未找到PID文件，视为已停止"
  exit 0
fi

pid="$(cat "${PID_FILE}" 2>/dev/null || true)"
if ! is_pid_running "${pid}"; then
  echo "[receiver-linux] 进程不存在，清理PID文件"
  rm -f "${PID_FILE}"
  exit 0
fi

kill -INT "${pid}" >/dev/null 2>&1 || true
sleep 2

if is_pid_running "${pid}"; then
  kill -TERM "${pid}" >/dev/null 2>&1 || true
  sleep 1
fi

if is_pid_running "${pid}"; then
  kill -KILL "${pid}" >/dev/null 2>&1 || true
fi

rm -f "${PID_FILE}"
echo "[receiver-linux] 已停止 (PID=${pid})"
