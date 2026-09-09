#!/usr/bin/env bash
set -euo pipefail

IFACE="${1:-}"
ACTION="${2:-}"
case "$ACTION" in
  up|reapply) ;;
  *) exit 0 ;;
esac
[[ "$IFACE" =~ ^[a-zA-Z0-9_][a-zA-Z0-9_.:-]{0,14}$ ]] || exit 0

# Do not wait for driver ioctls in NetworkManager's dispatcher.
if ! UNIT="$(systemd-escape --template=gwv3-sender-wifi-tuning@.service -- "$IFACE")" \
    || ! systemctl --no-block start "$UNIT"; then
  printf 'sender_wifi_tuning could not schedule iface=%s action=%s\n' "$IFACE" "$ACTION" >&2
fi
exit 0
