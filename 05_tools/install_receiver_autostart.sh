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
"$VENV/bin/python" -m pip install -r "$WEB_DIR/requirements.txt" >/dev/null

systemctl --user stop gwv3-web-monitor.service gwv3-gemini-receiver.service gwv3-receiver-log-rotate.timer >/dev/null 2>&1 || true
systemctl --user reset-failed gwv3-web-monitor.service gwv3-gemini-receiver.service gwv3-receiver-log-rotate.timer >/dev/null 2>&1 || true

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
MemoryHigh=3G
MemoryMax=4G
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
