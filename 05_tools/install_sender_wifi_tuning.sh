#!/usr/bin/env bash
set -euo pipefail

if ((EUID != 0)); then
  echo "run with sudo: sudo $0" >&2
  exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APPLY_SOURCE="$ROOT_DIR/05_tools/apply_sender_wifi_tuning.sh"
UNIT_SOURCE="$ROOT_DIR/06_configs/gwv3-sender-wifi-tuning.service"

install -m 0755 "$APPLY_SOURCE" /usr/local/sbin/gwv3-apply-sender-wifi-tuning
install -m 0644 "$UNIT_SOURCE" /etc/systemd/system/gwv3-sender-wifi-tuning.service

systemctl daemon-reload
systemctl enable gwv3-sender-wifi-tuning.service
systemctl restart gwv3-sender-wifi-tuning.service
systemctl --no-pager --full status gwv3-sender-wifi-tuning.service
