#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

RECEIVER_DIR="${DELIVERY_ROOT}/03_receiver_linux"
TEMPLATE_CONFIG="${DELIVERY_ROOT}/06_configs/receiver.linux.default.json"
RUNTIME_DIR="${DELIVERY_ROOT}/07_samples/runtime/receiver_linux"
RUNTIME_CONFIG="${RUNTIME_DIR}/receiver.runtime.json"
PID_FILE="${RUNTIME_DIR}/receiver.pid"
STDOUT_LOG="${RUNTIME_DIR}/receiver_stdout.log"
STDERR_LOG="${RUNTIME_DIR}/receiver_stderr.log"

LISTEN_PORT="${1:-5600}"

if [[ ! -f "${TEMPLATE_CONFIG}" ]]; then
  echo "[receiver-linux] ERROR: 模板配置不存在: ${TEMPLATE_CONFIG}" >&2
  exit 2
fi

ensure_dir "${RUNTIME_DIR}"

if [[ -f "${PID_FILE}" ]]; then
  old_pid="$(cat "${PID_FILE}" 2>/dev/null || true)"
  if is_pid_running "${old_pid}"; then
    echo "[receiver-linux] 已在运行, PID=${old_pid}"
    echo "[receiver-linux] 查看状态: ${SCRIPT_DIR}/status_receiver_linux.sh"
    exit 0
  fi
fi

cp -f "${TEMPLATE_CONFIG}" "${RUNTIME_CONFIG}"
write_json_with_overrides "${RUNTIME_CONFIG}" "${RUNTIME_CONFIG}" "network.listen_port" "${LISTEN_PORT}"

PYTHON_BIN="$(default_python_bin)"

(
  cd "${RECEIVER_DIR}"
  PYTHONPATH="${RECEIVER_DIR}${PYTHONPATH:+:${PYTHONPATH}}" \
  nohup "${PYTHON_BIN}" run_receiver.py --config "${RUNTIME_CONFIG}" \
    > "${STDOUT_LOG}" 2> "${STDERR_LOG}" &
  echo $! > "${PID_FILE}"
)

sleep 2
pid="$(cat "${PID_FILE}" 2>/dev/null || true)"
if ! is_pid_running "${pid}"; then
  echo "[receiver-linux] ERROR: 进程启动失败，请看日志 ${STDERR_LOG}" >&2
  tail -n 40 "${STDERR_LOG}" 2>/dev/null || true
  exit 3
fi

echo "[receiver-linux] 已启动"
echo "[receiver-linux] PID: ${pid}"
echo "[receiver-linux] 监听端口: ${LISTEN_PORT}"
echo "[receiver-linux] 配置: ${RUNTIME_CONFIG}"
echo "[receiver-linux] 日志: ${STDERR_LOG}"
