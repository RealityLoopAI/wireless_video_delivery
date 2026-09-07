#!/usr/bin/env bash
set -euo pipefail

SERVER_IP="${1:-${GEMINI_CHRONY_SERVER_IP:-}}"
CONF="/etc/chrony/chrony.conf"
BEGIN_MARK="# BEGIN GEMINI_WIRELESS_VIDEO_CHRONY_CLIENT"
END_MARK="# END GEMINI_WIRELESS_VIDEO_CHRONY_CLIENT"

SUDO=()
if [[ "${EUID}" -ne 0 ]]; then
  SUDO=(sudo)
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -z "$SERVER_IP" ]]; then
  echo "usage: $0 <receiver-ip-or-hostname>" >&2
  exit 2
fi

if ! command -v chronyc >/dev/null 2>&1; then
  export DEBIAN_FRONTEND=noninteractive
  "${SUDO[@]}" apt-get update
  "${SUDO[@]}" apt-get install -y chrony
fi

"${SUDO[@]}" "$SCRIPT_DIR/audit_time_sync_conflicts.sh" --fix

timestamp="$(date '+%Y%m%d-%H%M%S')"
"${SUDO[@]}" cp "$CONF" "${CONF}.bak.${timestamp}"

tmp="$(mktemp)"
awk -v begin="$BEGIN_MARK" -v end="$END_MARK" '
  $0 == begin {skip=1; next}
  $0 == end {skip=0; next}
  skip {next}
  /^[[:space:]]*(server|pool|peer)[[:space:]]+/ {
    print "# disabled by gemini_sender_chrony_client: " $0
    next
  }
  {print}
' "$CONF" > "$tmp"

cat >> "$tmp" <<EOF

$BEGIN_MARK
# Gemini Wireless Video sender time source.
# Keep every sender on the same receiver clock for RGB timestamp matching.
server $SERVER_IP iburst prefer minpoll 3 maxpoll 4
makestep 0.05 3
rtcsync
$END_MARK
EOF

"${SUDO[@]}" install -m 0644 "$tmp" "$CONF"
rm -f "$tmp"

"${SUDO[@]}" systemctl enable --now chrony
"${SUDO[@]}" systemctl restart chrony
"${SUDO[@]}" chronyc makestep || true

echo "chrony sender client configured"
echo "server_ip=$SERVER_IP"
echo
chronyc sources -v || true
echo
chronyc tracking || true
echo
"$SCRIPT_DIR/wait_chrony_sync.sh" "${GEMINI_CHRONY_MAX_CORRECTION_S:-0.010}" || true
