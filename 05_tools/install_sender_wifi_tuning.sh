#!/usr/bin/env bash
set -euo pipefail

if ((EUID != 0)); then
  echo "run with sudo: sudo $0" >&2
  exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APPLY_SOURCE="$ROOT_DIR/05_tools/apply_sender_wifi_tuning.sh"
UNIT_SOURCE="$ROOT_DIR/06_configs/gwv3-sender-wifi-tuning.service"
TEMPLATE_SOURCE="$ROOT_DIR/06_configs/gwv3-sender-wifi-tuning@.service"
DISPATCHER_SOURCE="$ROOT_DIR/05_tools/sender_wifi_dispatcher.sh"

for source in "$APPLY_SOURCE" "$UNIT_SOURCE" "$TEMPLATE_SOURCE" "$DISPATCHER_SOURCE"; do
  if [[ ! -f "$source" ]]; then
    printf 'missing Wi-Fi tuning deployment file: %s\n' "$source" >&2
    exit 1
  fi
done

install -m 0755 "$APPLY_SOURCE" /usr/local/sbin/gwv3-apply-sender-wifi-tuning
install -m 0644 "$UNIT_SOURCE" /etc/systemd/system/gwv3-sender-wifi-tuning.service
install -m 0644 "$TEMPLATE_SOURCE" /etc/systemd/system/gwv3-sender-wifi-tuning@.service
install -d -m 0755 /etc/NetworkManager/dispatcher.d
install -o root -g root -m 0755 "$DISPATCHER_SOURCE" /etc/NetworkManager/dispatcher.d/90-gwv3-sender-wifi-tuning

systemctl daemon-reload
systemctl enable gwv3-sender-wifi-tuning.service
systemctl restart gwv3-sender-wifi-tuning.service
systemctl --no-pager --full status gwv3-sender-wifi-tuning.service
