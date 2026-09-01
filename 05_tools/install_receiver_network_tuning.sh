#!/usr/bin/env bash
set -euo pipefail

if ((EUID != 0)); then
  echo "run with sudo: sudo $0" >&2
  exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SYSCTL_SOURCE="$ROOT_DIR/06_configs/99-gwv3-receiver-network.conf"
APPLY_SOURCE="$ROOT_DIR/05_tools/apply_receiver_network_tuning.sh"
UNIT_SOURCE="$ROOT_DIR/06_configs/gwv3-receiver-network-tuning.service"

install -m 0644 "$SYSCTL_SOURCE" /etc/sysctl.d/99-gwv3-receiver-network.conf
install -m 0755 "$APPLY_SOURCE" /usr/local/sbin/gwv3-apply-receiver-network-tuning
install -m 0644 "$UNIT_SOURCE" /etc/systemd/system/gwv3-receiver-network-tuning.service

systemctl daemon-reload
systemctl enable gwv3-receiver-network-tuning.service
systemctl restart gwv3-receiver-network-tuning.service
systemctl --no-pager --full status gwv3-receiver-network-tuning.service
