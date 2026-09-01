#!/usr/bin/env bash
set -euo pipefail

if ((EUID != 0)); then
  echo "receiver network tuning requires root" >&2
  exit 1
fi

SYSCTL_CONFIG="${GWV3_RECEIVER_SYSCTL_CONFIG:-/etc/sysctl.d/99-gwv3-receiver-network.conf}"
IFACE="${1:-${GWV3_RECEIVER_NETWORK_IFACE:-}}"

if [[ -f "$SYSCTL_CONFIG" ]]; then
  sysctl --load "$SYSCTL_CONFIG" >/dev/null
fi

if [[ -z "$IFACE" ]]; then
  IFACE="$(ip -o route show default 2>/dev/null | awk '{print $5; exit}')"
fi
if [[ -z "$IFACE" || ! -d "/sys/class/net/$IFACE" ]]; then
  echo "receiver network interface not found: ${IFACE:-unset}" >&2
  exit 1
fi

mapfile -t RX_QUEUES < <(find "/sys/class/net/$IFACE/queues" -maxdepth 1 -type d -name 'rx-*' | sort)
if ((${#RX_QUEUES[@]} == 0)); then
  echo "receiver network interface has no RX queues: $IFACE" >&2
  exit 1
fi

RPS_MASK="${GWV3_RECEIVER_RPS_CPUS:-}"
if [[ -z "$RPS_MASK" ]]; then
  RPS_MASK="$(python3 - <<'PY'
import os

count = max(1, os.cpu_count() or 1)
mask = (1 << count) - 1
if count > 1:
    mask &= ~1
groups = []
while mask:
    groups.append(f"{mask & 0xffffffff:08x}")
    mask >>= 32
print(",".join(reversed(groups)).lstrip("0") or "1")
PY
)"
fi

TOTAL_FLOW_ENTRIES="$(sysctl -n net.core.rps_sock_flow_entries 2>/dev/null || echo 0)"
if [[ ! "$TOTAL_FLOW_ENTRIES" =~ ^[0-9]+$ ]] || ((TOTAL_FLOW_ENTRIES < 1)); then
  TOTAL_FLOW_ENTRIES=32768
  sysctl -w "net.core.rps_sock_flow_entries=$TOTAL_FLOW_ENTRIES" >/dev/null
fi
FLOW_ENTRIES_PER_QUEUE=$((TOTAL_FLOW_ENTRIES / ${#RX_QUEUES[@]}))
((FLOW_ENTRIES_PER_QUEUE > 0)) || FLOW_ENTRIES_PER_QUEUE=1

for queue in "${RX_QUEUES[@]}"; do
  printf '%s\n' "$RPS_MASK" > "$queue/rps_cpus"
  printf '%s\n' "$FLOW_ENTRIES_PER_QUEUE" > "$queue/rps_flow_cnt"
done

printf 'receiver_network_tuning iface=%s rx_queues=%d rps_cpus=%s rps_flow_cnt=%d backlog=%s budget=%s budget_usecs=%s rmem_max=%s wmem_max=%s\n' \
  "$IFACE" \
  "${#RX_QUEUES[@]}" \
  "$RPS_MASK" \
  "$FLOW_ENTRIES_PER_QUEUE" \
  "$(sysctl -n net.core.netdev_max_backlog)" \
  "$(sysctl -n net.core.netdev_budget)" \
  "$(sysctl -n net.core.netdev_budget_usecs)" \
  "$(sysctl -n net.core.rmem_max)" \
  "$(sysctl -n net.core.wmem_max)"
