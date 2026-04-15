#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

SENDER_DIR="${DELIVERY_ROOT}/01_sender_linux"
TEMPLATE_CONFIG="${DELIVERY_ROOT}/06_configs/sender.default.json"
RUNTIME_DIR="${DELIVERY_ROOT}/07_samples/runtime/sender"
RUNTIME_CONFIG="${RUNTIME_DIR}/sender.runtime.json"
PID_FILE="${RUNTIME_DIR}/sender.pid"
STDOUT_LOG="${RUNTIME_DIR}/sender_stdout.log"
STDERR_LOG="${RUNTIME_DIR}/sender_stderr.log"

RECEIVER_IP="${1:-}"
RECEIVER_PORT="${2:-5600}"

if [[ -z "${RECEIVER_IP}" ]]; then
  read -r -p "请输入接收端IP (例如 192.168.1.105): " RECEIVER_IP
fi

if [[ -z "${RECEIVER_IP}" ]]; then
  echo "[sender] ERROR: 接收端IP不能为空" >&2
  exit 2
fi

if [[ ! -f "${TEMPLATE_CONFIG}" ]]; then
  echo "[sender] ERROR: 模板配置不存在: ${TEMPLATE_CONFIG}" >&2
  exit 2
fi

ensure_dir "${RUNTIME_DIR}"

if [[ -f "${PID_FILE}" ]]; then
  old_pid="$(cat "${PID_FILE}" 2>/dev/null || true)"
  if is_pid_running "${old_pid}"; then
    echo "[sender] 已在运行, PID=${old_pid}"
    echo "[sender] 查看状态: ${SCRIPT_DIR}/status_sender.sh"
    exit 0
  fi
fi

write_json_with_overrides "${TEMPLATE_CONFIG}" "${RUNTIME_CONFIG}" "network.remote_ip" "${RECEIVER_IP}"
write_json_with_overrides "${RUNTIME_CONFIG}" "${RUNTIME_CONFIG}" "network.remote_port" "${RECEIVER_PORT}"

PYTHON_BIN="$(default_python_bin)"

(
  cd "${SENDER_DIR}"
  PYTHONPATH="${SENDER_DIR}${PYTHONPATH:+:${PYTHONPATH}}" \
  nohup "${PYTHON_BIN}" run_sender.py --config "${RUNTIME_CONFIG}" \
    > "${STDOUT_LOG}" 2> "${STDERR_LOG}" &
  echo $! > "${PID_FILE}"
)

sleep 2
pid="$(cat "${PID_FILE}" 2>/dev/null || true)"
if ! is_pid_running "${pid}"; then
  echo "[sender] ERROR: 进程启动失败，请看日志 ${STDERR_LOG}" >&2
  tail -n 40 "${STDERR_LOG}" 2>/dev/null || true
  exit 3
fi

echo "[sender] 已启动"
echo "[sender] PID: ${pid}"
echo "[sender] 接收端: ${RECEIVER_IP}:${RECEIVER_PORT}"
echo "[sender] 配置: ${RUNTIME_CONFIG}"
echo "[sender] 日志: ${STDERR_LOG}"
