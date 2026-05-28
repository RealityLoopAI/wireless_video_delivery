#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/12_build/bin/gemini_sender"
WATCHDOG="$ROOT_DIR/05_tools/sender_watchdog.sh"
CONFIG="${1:-$ROOT_DIR/06_configs/sender_rk3588-01_two_cameras.json}"
PID_FILE="$ROOT_DIR/12_build/sender.pid"
CHILD_PID_FILE="$ROOT_DIR/12_build/sender_child.pid"
STDOUT_LOG="$ROOT_DIR/08_reports/sender_logs/sender_stdout.log"
SDK_LIB="$ROOT_DIR/11_third_party/orbbec/linux_arm64/OrbbecSDK_C_C++_v1.10.27_20250925_0549823_linux_arm64_release/OrbbecSDK_v1.10.27/SDK/lib"

fail() {
  echo "发送端启动失败：$1" >&2
  exit 1
}

prepare_usb_for_orbbec() {
  "$ROOT_DIR/05_tools/prepare_rk3588_usb.sh" || true
}

prepare_usb_for_orbbec
GEMINI_SENDER_REQUIRE_USB=0 "$ROOT_DIR/05_tools/sender_preflight.sh" "$CONFIG" "后台启动" "no-preview"

if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
  echo "发送端已经在运行，PID=$(cat "$PID_FILE")"
  exit 0
fi

mkdir -p "$ROOT_DIR/12_build" "$ROOT_DIR/08_reports/sender_logs"
cd "$ROOT_DIR"
chmod +x "$WATCHDOG"
rm -f "$CHILD_PID_FILE"
setsid -f "$WATCHDOG" "$CONFIG" "no-preview" >>"$STDOUT_LOG" 2>&1 < /dev/null
for _ in {1..20}; do
  if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
    break
  fi
  sleep 0.1
done
if [[ ! -f "$PID_FILE" ]] || ! kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
  fail "发送端守护进程没有成功保持运行，请查看 $STDOUT_LOG"
fi
echo "发送端守护已启动，watchdog PID=$(cat "$PID_FILE")"
if [[ -f "$CHILD_PID_FILE" ]]; then
  echo "当前发送端子进程 PID=$(cat "$CHILD_PID_FILE")"
fi
echo "日志：$ROOT_DIR/08_reports/sender_logs/sender.log"
