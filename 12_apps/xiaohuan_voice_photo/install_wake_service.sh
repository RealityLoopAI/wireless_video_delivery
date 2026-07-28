#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNIT_NAME="xiaohuan-wake.service"
USER_UNIT_DIR="$HOME/.config/systemd/user"

mkdir -p "$USER_UNIT_DIR"
install -m 0644 "$ROOT_DIR/systemd/$UNIT_NAME" "$USER_UNIT_DIR/$UNIT_NAME"
systemctl --user daemon-reload
systemctl --user enable --now "$UNIT_NAME"

if command -v loginctl >/dev/null 2>&1; then
  loginctl enable-linger "$USER" 2>/dev/null || true
fi

systemctl --user --no-pager status "$UNIT_NAME"
