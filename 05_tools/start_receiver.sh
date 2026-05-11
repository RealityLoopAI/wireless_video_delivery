#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-$ROOT_DIR/06_configs/receiver_ubuntu-01.json}"
BUILD_DIR="$ROOT_DIR/12_build"
BIN="$BUILD_DIR/bin/gemini_receiver"
RECEIVER_PID="$BUILD_DIR/receiver.pid"
WEB_PID="$BUILD_DIR/web_monitor.pid"
RECEIVER_STDOUT="$ROOT_DIR/08_reports/receiver_logs/receiver_stdout.log"
WEB_STDOUT="$ROOT_DIR/08_reports/receiver_logs/web_stdout.log"
WEB_DIR="$ROOT_DIR/09_web_monitor"
VENV="$WEB_DIR/.venv"

fail() {
  echo "接收端启动失败：$1" >&2
  exit 1
}

[[ -f "$CONFIG" ]] || fail "配置文件不存在 $CONFIG"
command -v cmake >/dev/null 2>&1 || fail "未找到 cmake，请先安装 cmake"
command -v g++ >/dev/null 2>&1 || fail "未找到 g++，请先安装编译器"
command -v ffmpeg >/dev/null 2>&1 || fail "未找到 ffmpeg，无法封装 rgb.mp4/depth.mkv"
command -v python3 >/dev/null 2>&1 || fail "未找到 python3"

mkdir -p "$BUILD_DIR" "$ROOT_DIR/08_reports/receiver_logs"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DGWV3_BUILD_RECEIVER=ON -DGWV3_BUILD_SENDER=OFF >/dev/null
cmake --build "$BUILD_DIR" -j"$(nproc)" >/dev/null
[[ -x "$BIN" ]] || fail "接收端可执行文件不存在 $BIN"

if [[ ! -x "$VENV/bin/python" ]]; then
  python3 -m venv "$VENV"
fi
"$VENV/bin/python" -m pip install -r "$WEB_DIR/requirements.txt" >/dev/null

if [[ -f "$RECEIVER_PID" ]] && kill -0 "$(cat "$RECEIVER_PID")" 2>/dev/null; then
  echo "C++ 接收端已经运行，PID=$(cat "$RECEIVER_PID")"
else
  cd "$ROOT_DIR"
  setsid nohup "$BIN" --config "$CONFIG" >>"$RECEIVER_STDOUT" 2>&1 &
  echo "$!" > "$RECEIVER_PID"
  echo "C++ 接收端已启动，PID=$(cat "$RECEIVER_PID")"
fi

if [[ -f "$WEB_PID" ]] && kill -0 "$(cat "$WEB_PID")" 2>/dev/null; then
  echo "Web 监控已经运行，PID=$(cat "$WEB_PID")"
else
  cd "$WEB_DIR"
  GWV3_RECEIVER_ADMIN="http://127.0.0.1:18080" setsid nohup "$VENV/bin/python" -m uvicorn server:app --host 0.0.0.0 --port 8080 >>"$WEB_STDOUT" 2>&1 &
  echo "$!" > "$WEB_PID"
  echo "Web 监控已启动，PID=$(cat "$WEB_PID")，地址：http://127.0.0.1:8080"
fi
