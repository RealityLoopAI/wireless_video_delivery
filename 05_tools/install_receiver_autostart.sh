#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-$ROOT_DIR/06_configs/receiver_ubuntu-01.json}"
BUILD_DIR="$ROOT_DIR/12_build"
BIN="$BUILD_DIR/bin/gemini_receiver"
WEB_DIR="$ROOT_DIR/09_web_monitor"
VENV="$WEB_DIR/.venv"
UNIT_DIR="$HOME/.config/systemd/user"
RECEIVER_UNIT="$UNIT_DIR/gwv3-gemini-receiver.service"
WEB_UNIT="$UNIT_DIR/gwv3-web-monitor.service"
LOG_ROTATE_SERVICE="$UNIT_DIR/gwv3-receiver-log-rotate.service"
LOG_ROTATE_TIMER="$UNIT_DIR/gwv3-receiver-log-rotate.timer"
RECEIVER_PID="$BUILD_DIR/receiver.pid"
WEB_PID="$BUILD_DIR/web_monitor.pid"

fail() {
  echo "接收端自启动安装失败：$1" >&2
  exit 1
}

collect_descendants() {
  local pid="$1"
  local child
  while read -r child; do
    [[ -z "$child" ]] && continue
    collect_descendants "$child"
    echo "$child"
  done < <(pgrep -P "$pid" 2>/dev/null || true)
}

stop_matching_processes() {
  local name="$1"
  local pattern="$2"
  local force_kill="${3:-1}"
  local pids=() descendants=() pid
  mapfile -t pids < <(pgrep -f "$pattern" 2>/dev/null || true)
  if ((${#pids[@]} == 0)); then
    return 0
  fi
  for pid in "${pids[@]}"; do
    mapfile -t descendants < <(collect_descendants "$pid")
    if ((${#descendants[@]} > 0)); then
      kill "${descendants[@]}" 2>/dev/null || true
    fi
  done
  kill "${pids[@]}" 2>/dev/null || true
  for _ in {1..20}; do
    local alive=0
    for pid in "${pids[@]}"; do
      if kill -0 "$pid" 2>/dev/null; then
        alive=1
        break
      fi
    done
    ((alive == 0)) && break
    sleep 0.2
  done
  if [[ "$force_kill" == "1" ]]; then
    kill -9 "${pids[@]}" 2>/dev/null || true
  else
    local still_alive=()
    for pid in "${pids[@]}"; do
      if kill -0 "$pid" 2>/dev/null; then
        still_alive+=("$pid")
      fi
    done
    if ((${#still_alive[@]} > 0)); then
      echo "$name 残留进程仍在运行，已跳过 SIGKILL 以避免打断录制收尾：${still_alive[*]}"
      return 1
    fi
  fi
  echo "$name 残留进程已清理：${pids[*]}"
}

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

status_is_idle() {
  python3 -c '
import json
import sys

try:
    data = json.load(sys.stdin)
except Exception:
    sys.exit(1)

busy = bool(data.get("recording_all"))
if int(data.get("record_finalize_outstanding_segments") or 0) > 0:
    busy = True
for cam in data.get("cameras", []):
    if cam.get("recording") or cam.get("segment_active") or cam.get("segment_finalizing") or cam.get("record_finalizing"):
        busy = True
    if int(cam.get("record_queue_packets") or 0) > 0 or int(cam.get("record_active_writes") or 0) > 0:
        busy = True
sys.exit(1 if busy else 0)
'
}

request_receiver_record_stop() {
  command -v curl >/dev/null 2>&1 || return 0
  local admin_port
  admin_port="$(read_config_field admin_port 18080)"
  local base_url="http://127.0.0.1:${admin_port}"
  if ! curl -fsS --max-time 3 -X POST "$base_url/api/record/stop-all" >/dev/null 2>&1; then
    return 0
  fi
  echo "已请求接收端停止录制，等待切片收尾..."
  for _ in {1..900}; do
    local status
    status="$(curl -fsS --max-time 3 "$base_url/api/status" 2>/dev/null || true)"
    if [[ -n "$status" ]] && status_is_idle <<<"$status"; then
      echo "接收端录制已空闲"
      return 0
    fi
    sleep 1
  done
  fail "接收端录制收尾等待超时，已避免 SIGKILL；请确认录制停止后再安装自启动"
}

[[ -f "$CONFIG" ]] || fail "配置文件不存在 $CONFIG"
command -v systemctl >/dev/null 2>&1 || fail "未找到 systemctl"
systemctl --user status >/dev/null 2>&1 || fail "当前用户 systemd 不可用"
command -v cmake >/dev/null 2>&1 || fail "未找到 cmake"
command -v g++ >/dev/null 2>&1 || fail "未找到 g++"
command -v ffmpeg >/dev/null 2>&1 || fail "未找到 ffmpeg"
command -v python3 >/dev/null 2>&1 || fail "未找到 python3"

ADMIN_PORT="$(read_config_field admin_port 18080)"
WEB_BIND_IP="$(read_config_field web_bind_ip 0.0.0.0)"
WEB_PORT="$(read_config_field web_port 8080)"
RECEIVER_STDOUT="$ROOT_DIR/08_reports/receiver_logs/receiver_stdout.log"
WEB_STDOUT="$ROOT_DIR/08_reports/receiver_logs/web_stdout.log"

mkdir -p "$BUILD_DIR" "$ROOT_DIR/08_reports/receiver_logs" "$UNIT_DIR"
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

systemctl --user stop gwv3-web-monitor.service gwv3-receiver-log-rotate.timer >/dev/null 2>&1 || true
request_receiver_record_stop
systemctl --user stop gwv3-gemini-receiver.service >/dev/null 2>&1 || true
systemctl --user reset-failed gwv3-web-monitor.service gwv3-gemini-receiver.service gwv3-receiver-log-rotate.timer >/dev/null 2>&1 || true
stop_matching_processes "Web 监控" "$VENV/bin/python -m uvicorn server:app"
stop_matching_processes "C++ 接收端" "gemini_receiver .*--config" 0
rm -f "$RECEIVER_PID" "$WEB_PID"

cat > "$RECEIVER_UNIT" <<EOF
[Unit]
Description=Gemini Wireless Video v3 Receiver
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=$ROOT_DIR
ExecStart=$BIN --config $CONFIG
Restart=on-failure
RestartSec=2
KillMode=mixed
TimeoutStopSec=900s
SendSIGKILL=no
MemoryHigh=3G
MemoryMax=4G
ManagedOOMPreference=avoid
StandardOutput=append:$RECEIVER_STDOUT
StandardError=append:$RECEIVER_STDOUT

[Install]
WantedBy=default.target
EOF

cat > "$WEB_UNIT" <<EOF
[Unit]
Description=Gemini Wireless Video v3 Web Monitor
After=gwv3-gemini-receiver.service
Wants=gwv3-gemini-receiver.service

[Service]
Type=simple
WorkingDirectory=$WEB_DIR
Environment=GWV3_RECEIVER_ADMIN=http://127.0.0.1:$ADMIN_PORT
ExecStart=$VENV/bin/python -m uvicorn server:app --host $WEB_BIND_IP --port $WEB_PORT --no-access-log
Restart=on-failure
RestartSec=2
KillMode=mixed
TimeoutStopSec=5s
SendSIGKILL=yes
MemoryHigh=384M
MemoryMax=512M
StandardOutput=append:$WEB_STDOUT
StandardError=append:$WEB_STDOUT

[Install]
WantedBy=default.target
EOF
chmod 600 "$WEB_UNIT"

cat > "$LOG_ROTATE_SERVICE" <<EOF
[Unit]
Description=Rotate Gemini receiver logs

[Service]
Type=oneshot
ExecStart=$ROOT_DIR/05_tools/rotate_receiver_logs.sh
EOF

cat > "$LOG_ROTATE_TIMER" <<EOF
[Unit]
Description=Run Gemini receiver log rotation periodically

[Timer]
OnBootSec=5min
OnUnitActiveSec=10min
Persistent=true

[Install]
WantedBy=timers.target
EOF

systemctl --user daemon-reload
systemctl --user enable gwv3-gemini-receiver.service gwv3-web-monitor.service gwv3-receiver-log-rotate.timer >/dev/null
systemctl --user restart gwv3-gemini-receiver.service
systemctl --user restart gwv3-web-monitor.service
systemctl --user start gwv3-receiver-log-rotate.timer

for item in "gwv3-gemini-receiver.service:$RECEIVER_PID" "gwv3-web-monitor.service:$WEB_PID"; do
  unit="${item%%:*}"
  pid_file="${item#*:}"
  pid="$(systemctl --user show "$unit" --property=MainPID --value 2>/dev/null || true)"
  if [[ "$pid" =~ ^[0-9]+$ ]] && ((pid > 0)); then
    echo "$pid" > "$pid_file"
  fi
done

echo "接收端自启动已安装并启动。"
echo "Web 地址：http://127.0.0.1:$WEB_PORT"

if command -v loginctl >/dev/null 2>&1; then
  linger="$(loginctl show-user "$USER" -p Linger --value 2>/dev/null || echo no)"
  if [[ "$linger" != "yes" ]]; then
    echo "提示：当前 Linger=$linger。若需要开机后未登录也自动启动，请执行："
    echo "  sudo loginctl enable-linger $USER"
  fi
fi
