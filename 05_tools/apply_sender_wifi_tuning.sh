#!/usr/bin/env bash
set -euo pipefail

sender_wifi_driver_supported() {
  [[ "$1" == "rtw_8821cu" || "$1" == "rtl8821cu" ]]
}

sender_wifi_ensure_queue() {
  local iface="$1" limit="$2"
  # Reapplying a live profile must not purge an already-correct queue.
  if tc -j qdisc show dev "$iface" | python3 -c '
import json, sys
try:
    queues = json.load(sys.stdin)
    matched = any(q.get("root") is True and q.get("kind") == "pfifo"
                  and q.get("options", {}).get("limit") == int(sys.argv[1]) for q in queues)
except (ValueError, TypeError, AttributeError):
    matched = False
sys.exit(0 if matched else 1)
' "$limit"; then
    return 0
  fi
  tc qdisc replace dev "$iface" root pfifo limit "$limit"
}

sender_wifi_tuning_main() {
  if ((EUID != 0)); then
    echo "sender Wi-Fi tuning requires root" >&2
    return 1
  fi
  local iface="${1:-${GWV3_SENDER_WIFI_IFACE:-wlan0}}"
  local limit="${GWV3_SENDER_WIFI_QUEUE_LIMIT:-128}" driver lock_fd
  if [[ ! "$iface" =~ ^[a-zA-Z0-9_][a-zA-Z0-9_.:-]{0,14}$ ]]; then
    echo "invalid sender Wi-Fi interface: $iface" >&2
    return 1
  fi
  if [[ ! -d "/sys/class/net/$iface" ]]; then
    printf 'sender_wifi_tuning iface=%s action=interface-absent\n' "$iface"
    return 0
  fi
  if [[ ! "$limit" =~ ^[0-9]+$ ]] || ((limit < 16 || limit > 1024)); then
    echo "invalid sender Wi-Fi queue limit: $limit" >&2
    return 1
  fi
  driver="$(ethtool -i "$iface" 2>/dev/null | awk '/^driver:/ {print $2; exit}')"
  if ! sender_wifi_driver_supported "$driver"; then
    printf 'sender_wifi_tuning iface=%s driver=%s action=unchanged\n' "$iface" "${driver:-unknown}"
    return 0
  fi
  umask 077
  exec {lock_fd}>"/run/gwv3-sender-wifi-$iface.lock"
  flock -w 2 "$lock_fd" || return 1
  if command -v iw >/dev/null 2>&1; then
    iw dev "$iface" set power_save off >/dev/null 2>&1 || true
  fi
  sender_wifi_ensure_queue "$iface" "$limit"
  printf 'sender_wifi_tuning iface=%s driver=%s qdisc=pfifo queue_limit=%s power_save=' \
    "$iface" "$driver" "$limit"
  if command -v iw >/dev/null 2>&1; then
    iw dev "$iface" get power_save 2>/dev/null | sed -n 's/^Power save: //p'
  else
    printf 'managed-by-NetworkManager\n'
  fi
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  sender_wifi_tuning_main "$@"
fi
