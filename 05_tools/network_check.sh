#!/usr/bin/env bash
set -euo pipefail

TARGET_IP="${1:-}"

echo "[network] local ipv4:"
ip -4 -o addr show | awk '{print "  - " $2 ": " $4}'

echo "[network] default route:"
ip route | head -n 3

if [[ -n "${TARGET_IP}" ]]; then
  echo "[network] ping ${TARGET_IP}:"
  ping -c 3 "${TARGET_IP}" || true
fi
