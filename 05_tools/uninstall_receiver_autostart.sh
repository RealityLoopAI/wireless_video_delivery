#!/usr/bin/env bash
set -euo pipefail

UNIT_DIR="$HOME/.config/systemd/user"
UNITS=(
  gwv3-web-monitor.service
  gwv3-gemini-receiver.service
  gwv3-recording-uploader.service
  gwv3-audio-archive.service
  gwv3-receiver-log-rotate.timer
  gwv3-receiver-log-rotate.service
)

if command -v systemctl >/dev/null 2>&1 && systemctl --user status >/dev/null 2>&1; then
  systemctl --user disable --now "${UNITS[@]}" >/dev/null 2>&1 || true
fi

for unit in "${UNITS[@]}"; do
  rm -f "$UNIT_DIR/$unit"
done

if command -v systemctl >/dev/null 2>&1 && systemctl --user status >/dev/null 2>&1; then
  systemctl --user daemon-reload
fi

echo "接收端自启动服务已卸载。"
