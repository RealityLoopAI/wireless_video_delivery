#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/12_build/bin/gemini_sender"
CONFIG="${1:-$ROOT_DIR/06_configs/sender_orangepi5pro-01_depth_zlib.json}"
PID_FILE="$ROOT_DIR/12_build/sender.pid"
STDOUT_LOG="$ROOT_DIR/08_reports/sender_logs/sender_stdout.log"
SDK_LIB="$ROOT_DIR/11_third_party/orbbec/linux_arm64/OrbbecSDK_C_C++_v1.10.27_20250925_0549823_linux_arm64_release/OrbbecSDK_v1.10.27/SDK/lib"

fail() {
  echo "发送端启动失败：$1" >&2
  exit 1
}

"$ROOT_DIR/05_tools/sender_preflight.sh" "$CONFIG" "后台启动" "no-preview"

if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
  echo "发送端已经在运行，PID=$(cat "$PID_FILE")"
  exit 0
fi

mkdir -p "$ROOT_DIR/12_build" "$ROOT_DIR/08_reports/sender_logs"
cd "$ROOT_DIR"
LD_LIBRARY_PATH="$SDK_LIB:${LD_LIBRARY_PATH:-}" setsid -f "$BIN" --config "$CONFIG" --no-preview >>"$STDOUT_LOG" 2>&1 < /dev/null
sleep 0.5
pgrep -n -f "$BIN --config $CONFIG" > "$PID_FILE" || fail "发送端进程没有成功保持运行，请查看 $STDOUT_LOG"
echo "发送端已启动，PID=$(cat "$PID_FILE")"
echo "日志：$ROOT_DIR/08_reports/sender_logs/sender.log"
