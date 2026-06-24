#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/12_build/bin/gemini_sender"
CONFIG="${1:-${GEMINI_SENDER_CONFIG:-$ROOT_DIR/06_configs/sender_rk3588-01_one_camera.json}}"
PID_FILE="$ROOT_DIR/12_build/sender.pid"
source "$ROOT_DIR/05_tools/sender_wifi_guard.sh"
gemini_sender_wifi_apply_repo_defaults

resolve_sender_sdk_lib() {
  local linked_lib=""
  linked_lib="$(ldd "$BIN" 2>/dev/null | awk '/libOrbbecSDK/ {print $3; exit}')"
  if [[ -n "$linked_lib" && -f "$linked_lib" ]]; then
    dirname "$linked_lib"
    return
  fi

  local candidate
  for candidate in \
    "${ORBBEC_SDK_ROOT:-}/lib" \
    "$ROOT_DIR/11_third_party/orbbec/linux_arm64/OrbbecSDK_v2.8.6/lib" \
    "$ROOT_DIR/11_third_party/orbbec/linux_arm64/OrbbecSDK_C_C++_v1.10.27_20250925_0549823_linux_arm64_release/OrbbecSDK_v1.10.27/SDK/lib"; do
    if [[ -n "$candidate" && -d "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return
    fi
  done
}

"$ROOT_DIR/05_tools/sender_preflight.sh" "$CONFIG" "前台启动" "config"

if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
  echo "发送端已经在运行，PID=$(cat "$PID_FILE")"
  echo "先关闭原来的发送端终端，或执行：$ROOT_DIR/05_tools/stop_sender.sh"
  exit 0
fi

mkdir -p "$ROOT_DIR/12_build" "$ROOT_DIR/08_reports/sender_logs"
cd "$ROOT_DIR"
SDK_LIB="${GEMINI_SENDER_SDK_LIB:-$(resolve_sender_sdk_lib)}"

child_pid=""
cleanup() {
  if [[ -n "$child_pid" ]] && kill -0 "$child_pid" 2>/dev/null; then
    kill "$child_pid" 2>/dev/null || true
    wait "$child_pid" 2>/dev/null || true
  fi
  rm -f "$PID_FILE"
}
trap cleanup INT TERM HUP EXIT

echo
echo "启动 Gemini 发送端。关闭这个终端窗口即可停止发送端。"
echo "日志：$ROOT_DIR/08_reports/sender_logs/sender.log"
echo

DISPLAY="${DISPLAY:-:1}" LD_LIBRARY_PATH="$SDK_LIB:${LD_LIBRARY_PATH:-}" "$BIN" --config "$CONFIG" &
child_pid="$!"
echo "$child_pid" > "$PID_FILE"
wait "$child_pid"
child_pid=""
