#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

RUNTIME_DIR="${DELIVERY_ROOT}/07_samples/runtime/sender"
PID_FILE="${RUNTIME_DIR}/sender.pid"
STDERR_LOG="${RUNTIME_DIR}/sender_stderr.log"
RUNTIME_CONFIG="${RUNTIME_DIR}/sender.runtime.json"

pid=""
if [[ -f "${PID_FILE}" ]]; then
  pid="$(cat "${PID_FILE}" 2>/dev/null || true)"
fi

if is_pid_running "${pid}"; then
  echo "[sender] status: RUNNING (PID=${pid})"
else
  echo "[sender] status: STOPPED"
fi

if [[ -f "${RUNTIME_CONFIG}" ]]; then
  echo "[sender] runtime config: ${RUNTIME_CONFIG}"
fi

if [[ -f "${STDERR_LOG}" ]]; then
  last_state="$(grep -E "state=" "${STDERR_LOG}" | tail -n 1 || true)"
  if [[ -n "${last_state}" ]]; then
    echo "[sender] last state: ${last_state}"
  fi
  echo "[sender] recent log:"
  tail -n 8 "${STDERR_LOG}" || true
else
  echo "[sender] log: <missing>"
fi
