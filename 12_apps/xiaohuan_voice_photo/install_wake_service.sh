#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNIT_NAME="xiaohuan-wake.service"
USER_UNIT_DIR="$HOME/.config/systemd/user"
UDEV_RULE_NAME="90-xiaohuan-usb-audio-exclusive.rules"
UDEV_RULE_SOURCE="$ROOT_DIR/systemd/$UDEV_RULE_NAME"
UDEV_RULE_TARGET="/etc/udev/rules.d/$UDEV_RULE_NAME"

install_system_file() {
  local mode="$1"
  local source="$2"
  local target="$3"
  if (( EUID == 0 )); then
    install -D -m "$mode" "$source" "$target"
  elif command -v sudo >/dev/null 2>&1; then
    sudo install -D -m "$mode" "$source" "$target"
  else
    echo "sudo is required to install $target" >&2
    return 1
  fi
}

mkdir -p "$USER_UNIT_DIR"
install -m 0644 "$ROOT_DIR/systemd/$UNIT_NAME" "$USER_UNIT_DIR/$UNIT_NAME"
install_system_file 0644 "$UDEV_RULE_SOURCE" "$UDEV_RULE_TARGET"
if command -v udevadm >/dev/null 2>&1; then
  if (( EUID == 0 )); then
    udevadm control --reload-rules
  else
    sudo udevadm control --reload-rules
  fi
fi
systemctl --user daemon-reload
systemctl --user enable --now "$UNIT_NAME"

if command -v loginctl >/dev/null 2>&1; then
  loginctl enable-linger "$USER" 2>/dev/null || true
fi

systemctl --user --no-pager status "$UNIT_NAME"
