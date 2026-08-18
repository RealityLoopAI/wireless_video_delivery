#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNIT_NAME="gwv3-recording-buttons.service"
POWER_UNIT_NAME="gwv3-power-button.service"
USER_UNIT_DIR="$HOME/.config/systemd/user"

mkdir -p "$USER_UNIT_DIR"
install -m 0644 "$ROOT_DIR/systemd/$UNIT_NAME" "$USER_UNIT_DIR/$UNIT_NAME"
chmod +x "$ROOT_DIR/recording_button_service.py"
systemctl --user daemon-reload
systemctl --user enable --now "$UNIT_NAME"
if command -v loginctl >/dev/null 2>&1; then
  loginctl enable-linger "$USER" 2>/dev/null || true
fi
systemctl --user --no-pager status "$UNIT_NAME"

sudo install -d -m 0755 /etc/systemd/system /etc/systemd/logind.conf.d
sudo install -m 0644 \
  "$ROOT_DIR/systemd/$POWER_UNIT_NAME" \
  "/etc/systemd/system/$POWER_UNIT_NAME"
sudo install -m 0644 \
  "$ROOT_DIR/systemd/90-gwv3-power-key.conf" \
  /etc/systemd/logind.conf.d/90-gwv3-power-key.conf
chmod +x "$ROOT_DIR/power_button_service.py"
sudo systemctl daemon-reload
sudo systemctl enable --now "$POWER_UNIT_NAME"
sudo systemctl --no-pager status "$POWER_UNIT_NAME"

echo "HandlePowerKey=ignore is installed; reboot once if logind did not already use it."
