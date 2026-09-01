#!/usr/bin/env bash
set -euo pipefail

if ((EUID != 0)); then
  echo "sender Wi-Fi tuning requires root" >&2
  exit 1
fi

IFACE="${1:-${GWV3_SENDER_WIFI_IFACE:-wlan0}}"
QUEUE_LIMIT="${GWV3_SENDER_WIFI_QUEUE_LIMIT:-128}"

if [[ ! -d "/sys/class/net/$IFACE" ]]; then
  echo "sender Wi-Fi interface not found: $IFACE" >&2
  exit 1
fi
if [[ ! "$QUEUE_LIMIT" =~ ^[0-9]+$ ]] || ((QUEUE_LIMIT < 16 || QUEUE_LIMIT > 1024)); then
  echo "invalid sender Wi-Fi queue limit: $QUEUE_LIMIT" >&2
  exit 1
fi

DRIVER="$(ethtool -i "$IFACE" 2>/dev/null | awk '/^driver:/ {print $2; exit}')"
if [[ "$DRIVER" != "rtw_8821cu" ]]; then
  printf 'sender_wifi_tuning iface=%s driver=%s action=unchanged\n' "$IFACE" "${DRIVER:-unknown}"
  exit 0
fi

if command -v iw >/dev/null 2>&1; then
  iw dev "$IFACE" set power_save off >/dev/null 2>&1 || true
fi

tc qdisc replace dev "$IFACE" root pfifo limit "$QUEUE_LIMIT"

printf 'sender_wifi_tuning iface=%s driver=%s qdisc=pfifo queue_limit=%s power_save=' \
  "$IFACE" "$DRIVER" "$QUEUE_LIMIT"
if command -v iw >/dev/null 2>&1; then
  iw dev "$IFACE" get power_save 2>/dev/null | sed -n 's/^Power save: //p'
else
  printf 'managed-by-NetworkManager\n'
fi
