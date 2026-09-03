#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-$ROOT_DIR/06_configs/receiver_loop.json}"
BUILD_DIR="$ROOT_DIR/12_build"
BIN="$BUILD_DIR/bin/gemini_receiver"
RECEIVER_PID="$BUILD_DIR/receiver.pid"
WEB_PID="$BUILD_DIR/web_monitor.pid"
UPLOADER_PID="$BUILD_DIR/recording_uploader.pid"
AUDIO_ARCHIVE_PID="$BUILD_DIR/audio_archive.pid"
RECEIVER_STDOUT="$ROOT_DIR/08_reports/receiver_logs/receiver_stdout.log"
WEB_STDOUT="$ROOT_DIR/08_reports/receiver_logs/web_stdout.log"
WEB_DIR="$ROOT_DIR/09_web_monitor"
VENV="$WEB_DIR/.venv"
RECEIVER_UNIT="gwv3-gemini-receiver.service"
WEB_UNIT="gwv3-web-monitor.service"
UPLOADER_UNIT="gwv3-recording-uploader.service"
AUDIO_ARCHIVE_UNIT="gwv3-audio-archive.service"
DEFAULT_AUDIO_ARCHIVE_CONFIG="$ROOT_DIR/06_configs/audio_archive_receiver.json"
if [[ "$(basename "$CONFIG")" == "receiver_loop.json" ]]; then
  DEFAULT_AUDIO_ARCHIVE_CONFIG="$ROOT_DIR/06_configs/audio_archive_receiver_loop.json"
fi
AUDIO_ARCHIVE_CONFIG="${GWV3_AUDIO_ARCHIVE_CONFIG:-$DEFAULT_AUDIO_ARCHIVE_CONFIG}"
MAX_LOG_BYTES=$((256 * 1024 * 1024))

fail() {
  echo "接收端启动失败：$1" >&2
  exit 1
}

[[ -f "$CONFIG" ]] || fail "配置文件不存在 $CONFIG"
[[ -f "$AUDIO_ARCHIVE_CONFIG" ]] || fail "音频归档配置不存在 $AUDIO_ARCHIVE_CONFIG"
command -v cmake >/dev/null 2>&1 || fail "未找到 cmake，请先安装 cmake"
command -v g++ >/dev/null 2>&1 || fail "未找到 g++，请先安装编译器"
command -v ffmpeg >/dev/null 2>&1 || fail "未找到 ffmpeg，无法封装 rgb.mp4/depth.mkv"
command -v python3 >/dev/null 2>&1 || fail "未找到 python3"

read_config_field() {
  python3 - "$CONFIG" "$1" "$2" <<'PY'
import json
import sys

config_path, key, fallback = sys.argv[1], sys.argv[2], sys.argv[3]
try:
    with open(config_path, "r", encoding="utf-8") as handle:
        data = json.load(handle)
    print(data.get(key, fallback))
except Exception:
    print(fallback)
PY
}

ADMIN_PORT="$(read_config_field admin_port 18080)"
WEB_BIND_IP="$(read_config_field web_bind_ip 0.0.0.0)"
WEB_PORT="$(read_config_field web_port 8080)"
WEB_DISPLAY_HOST="$WEB_BIND_IP"
if [[ "$WEB_DISPLAY_HOST" == "0.0.0.0" ]]; then
  WEB_DISPLAY_HOST="127.0.0.1"
fi

mkdir -p "$BUILD_DIR" "$ROOT_DIR/08_reports/receiver_logs"

shell_quote() {
  printf '%q' "$1"
}

rotate_log_if_large() {
  local path="$1"
  [[ -f "$path" ]] || return 0
  local size
  size="$(stat -c '%s' "$path" 2>/dev/null || echo 0)"
  if ((size > MAX_LOG_BYTES)); then
    local archive_dir="$ROOT_DIR/08_reports/receiver_logs/archive"
    local stamp
    stamp="$(date +%Y%m%d_%H%M%S)"
    mkdir -p "$archive_dir"
    mv "$path" "$archive_dir/$(basename "$path").$stamp"
    : > "$path"
  fi
}

systemd_user_available() {
  command -v systemd-run >/dev/null 2>&1 && systemctl --user status >/dev/null 2>&1
}

unit_active() {
  systemctl --user is-active --quiet "$1" 2>/dev/null
}

unit_installed() {
  systemctl --user cat "$1" >/dev/null 2>&1
}

unit_main_pid() {
  systemctl --user show "$1" --property=MainPID --value 2>/dev/null | awk '{print $1}'
}

write_unit_pid() {
  local unit="$1"
  local pid_file="$2"
  local pid
  for _ in {1..30}; do
    pid="$(unit_main_pid "$unit")"
    if [[ "$pid" =~ ^[0-9]+$ ]] && ((pid > 0)); then
      echo "$pid" > "$pid_file"
      return 0
    fi
    sleep 0.2
  done
  return 1
}

start_systemd_unit() {
  local name="$1"
  local unit="$2"
  local pid_file="$3"
  local workdir="$4"
  local command="$5"
  if unit_active "$unit"; then
    write_unit_pid "$unit" "$pid_file" || true
    echo "$name 已经运行，PID=$(cat "$pid_file" 2>/dev/null || echo unknown)"
    return 0
  fi
  systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
  systemd-run --user --unit="$unit" --collect \
    --property=Restart=on-failure \
    --property=RestartSec=2 \
    --property=WorkingDirectory="$workdir" \
    /usr/bin/env bash -lc "$command" >/dev/null
  write_unit_pid "$unit" "$pid_file" || fail "$name 已启动但无法读取 systemd MainPID"
  echo "$name 已启动，PID=$(cat "$pid_file")"
}

start_installed_unit() {
  local name="$1"
  local unit="$2"
  local pid_file="$3"
  systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
  systemctl --user start "$unit"
  write_unit_pid "$unit" "$pid_file" || fail "$name 已启动但无法读取 systemd MainPID"
  echo "$name 已启动，PID=$(cat "$pid_file")"
}

start_legacy_background() {
  local name="$1"
  local pid_file="$2"
  local workdir="$3"
  shift 3
  cd "$workdir"
  setsid nohup "$@" &
  echo "$!" > "$pid_file"
  echo "$name 已启动，PID=$(cat "$pid_file")"
}

rotate_log_if_large "$RECEIVER_STDOUT"
rotate_log_if_large "$WEB_STDOUT"
rotate_log_if_large "$ROOT_DIR/08_reports/receiver_logs/receiver.log"
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DGWV3_BUILD_RECEIVER=ON -DGWV3_BUILD_SENDER=OFF >/dev/null
cmake --build "$BUILD_DIR" -j"$(nproc)" >/dev/null
[[ -x "$BIN" ]] || fail "接收端可执行文件不存在 $BIN"

if [[ ! -x "$VENV/bin/python" ]]; then
  python3 -m venv "$VENV"
fi
REQUIREMENTS_HASH="$(sha256sum "$WEB_DIR/requirements.txt" | awk '{print $1}')"
REQUIREMENTS_STAMP="$VENV/.requirements.sha256"
if [[ ! -f "$REQUIREMENTS_STAMP" ]] || [[ "$(<"$REQUIREMENTS_STAMP")" != "$REQUIREMENTS_HASH" ]]; then
  "$VENV/bin/python" -m pip install -r "$WEB_DIR/requirements.txt" >/dev/null
  echo "$REQUIREMENTS_HASH" > "$REQUIREMENTS_STAMP"
fi

if systemd_user_available && unit_active "$RECEIVER_UNIT"; then
  write_unit_pid "$RECEIVER_UNIT" "$RECEIVER_PID" || true
  echo "C++ 接收端已经运行，PID=$(cat "$RECEIVER_PID" 2>/dev/null || echo unknown)"
elif [[ -f "$RECEIVER_PID" ]] && kill -0 "$(cat "$RECEIVER_PID")" 2>/dev/null; then
  echo "C++ 接收端已经运行，PID=$(cat "$RECEIVER_PID")"
else
  if systemd_user_available; then
    if unit_installed "$RECEIVER_UNIT"; then
      start_installed_unit "C++ 接收端" "$RECEIVER_UNIT" "$RECEIVER_PID"
    else
      start_systemd_unit "C++ 接收端" "$RECEIVER_UNIT" "$RECEIVER_PID" "$ROOT_DIR" \
        "exec $(shell_quote "$BIN") --config $(shell_quote "$CONFIG") >>$(shell_quote "$RECEIVER_STDOUT") 2>&1"
    fi
  else
    start_legacy_background "C++ 接收端" "$RECEIVER_PID" "$ROOT_DIR" "$BIN" --config "$CONFIG" >>"$RECEIVER_STDOUT" 2>&1
  fi
fi

if systemd_user_available && unit_active "$UPLOADER_UNIT"; then
  write_unit_pid "$UPLOADER_UNIT" "$UPLOADER_PID" || true
  echo "录制上传器已经运行，PID=$(cat "$UPLOADER_PID" 2>/dev/null || echo unknown)"
elif [[ -f "$UPLOADER_PID" ]] && kill -0 "$(cat "$UPLOADER_PID")" 2>/dev/null; then
  echo "录制上传器已经运行，PID=$(cat "$UPLOADER_PID")"
else
  if systemd_user_available; then
    if unit_installed "$UPLOADER_UNIT"; then
      start_installed_unit "录制上传器" "$UPLOADER_UNIT" "$UPLOADER_PID"
    else
      start_systemd_unit "录制上传器" "$UPLOADER_UNIT" "$UPLOADER_PID" "$ROOT_DIR" \
        "exec /usr/bin/python3 $(shell_quote "$ROOT_DIR/05_tools/recording_uploader.py") --config $(shell_quote "$CONFIG") >>$(shell_quote "$ROOT_DIR/08_reports/receiver_logs/recording_uploader.log") 2>&1"
    fi
  else
    start_legacy_background "录制上传器" "$UPLOADER_PID" "$ROOT_DIR" /usr/bin/python3 \
      "$ROOT_DIR/05_tools/recording_uploader.py" --config "$CONFIG" \
      >>"$ROOT_DIR/08_reports/receiver_logs/recording_uploader.log" 2>&1
  fi
fi

if systemd_user_available && unit_active "$AUDIO_ARCHIVE_UNIT"; then
  write_unit_pid "$AUDIO_ARCHIVE_UNIT" "$AUDIO_ARCHIVE_PID" || true
  echo "音频归档已经运行，PID=$(cat "$AUDIO_ARCHIVE_PID" 2>/dev/null || echo unknown)"
elif [[ -f "$AUDIO_ARCHIVE_PID" ]] && kill -0 "$(cat "$AUDIO_ARCHIVE_PID")" 2>/dev/null; then
  echo "音频归档已经运行，PID=$(cat "$AUDIO_ARCHIVE_PID")"
else
  if systemd_user_available; then
    if unit_installed "$AUDIO_ARCHIVE_UNIT"; then
      start_installed_unit "音频归档" "$AUDIO_ARCHIVE_UNIT" "$AUDIO_ARCHIVE_PID"
    else
      start_systemd_unit "音频归档" "$AUDIO_ARCHIVE_UNIT" "$AUDIO_ARCHIVE_PID" "$ROOT_DIR" \
        "exec /usr/bin/python3 $(shell_quote "$ROOT_DIR/05_tools/audio_archive_receiver.py") --config $(shell_quote "$AUDIO_ARCHIVE_CONFIG") >>$(shell_quote "$ROOT_DIR/08_reports/receiver_logs/audio_archive.log") 2>&1"
    fi
  else
    start_legacy_background "音频归档" "$AUDIO_ARCHIVE_PID" "$ROOT_DIR" /usr/bin/python3 \
      "$ROOT_DIR/05_tools/audio_archive_receiver.py" --config "$AUDIO_ARCHIVE_CONFIG" \
      >>"$ROOT_DIR/08_reports/receiver_logs/audio_archive.log" 2>&1
  fi
fi

if systemd_user_available && unit_active "$WEB_UNIT"; then
  write_unit_pid "$WEB_UNIT" "$WEB_PID" || true
  echo "Web 监控已经运行，PID=$(cat "$WEB_PID" 2>/dev/null || echo unknown)"
elif [[ -f "$WEB_PID" ]] && kill -0 "$(cat "$WEB_PID")" 2>/dev/null; then
  echo "Web 监控已经运行，PID=$(cat "$WEB_PID")"
else
  if systemd_user_available; then
    if unit_installed "$WEB_UNIT"; then
      start_installed_unit "Web 监控" "$WEB_UNIT" "$WEB_PID"
    else
      start_systemd_unit "Web 监控" "$WEB_UNIT" "$WEB_PID" "$WEB_DIR" \
        "export GWV3_RECEIVER_ADMIN=$(shell_quote "http://127.0.0.1:$ADMIN_PORT") GWV3_AUDIO_ARCHIVE_ADMIN=http://127.0.0.1:18083; exec $(shell_quote "$VENV/bin/python") -m uvicorn server:app --host $(shell_quote "$WEB_BIND_IP") --port $(shell_quote "$WEB_PORT") --no-access-log >>$(shell_quote "$WEB_STDOUT") 2>&1"
    fi
  else
    cd "$WEB_DIR"
    GWV3_RECEIVER_ADMIN="http://127.0.0.1:$ADMIN_PORT" GWV3_AUDIO_ARCHIVE_ADMIN="http://127.0.0.1:18083" setsid nohup "$VENV/bin/python" -m uvicorn server:app --host "$WEB_BIND_IP" --port "$WEB_PORT" --no-access-log >>"$WEB_STDOUT" 2>&1 &
    echo "$!" > "$WEB_PID"
  fi
  echo "Web 监控已启动，PID=$(cat "$WEB_PID")，地址：http://$WEB_DISPLAY_HOST:$WEB_PORT"
fi
