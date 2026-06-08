#!/usr/bin/env bash
set -euo pipefail

ALLOW_CIDR="${1:-${GEMINI_CHRONY_ALLOW_CIDR:-192.168.66.0/24}}"
CONF="/etc/chrony/chrony.conf"
BEGIN_MARK="# BEGIN GEMINI_WIRELESS_VIDEO_CHRONY_SERVER"
END_MARK="# END GEMINI_WIRELESS_VIDEO_CHRONY_SERVER"

SUDO=()
if [[ "${EUID}" -ne 0 ]]; then
  SUDO=(sudo)
fi

if ! command -v chronyc >/dev/null 2>&1; then
  export DEBIAN_FRONTEND=noninteractive
  "${SUDO[@]}" apt-get update
  "${SUDO[@]}" apt-get install -y chrony
fi

if systemctl list-unit-files systemd-timesyncd.service >/dev/null 2>&1; then
  "${SUDO[@]}" systemctl disable --now systemd-timesyncd.service 2>/dev/null || true
fi

timestamp="$(date '+%Y%m%d-%H%M%S')"
"${SUDO[@]}" cp "$CONF" "${CONF}.bak.${timestamp}"

tmp="$(mktemp)"
awk -v begin="$BEGIN_MARK" -v end="$END_MARK" '
  $0 == begin {skip=1; next}
  $0 == end {skip=0; next}
  !skip {print}
' "$CONF" > "$tmp"

cat >> "$tmp" <<EOF

$BEGIN_MARK
# Gemini Wireless Video receiver time source.
# All senders should sync to this receiver before RGB capture starts.
allow $ALLOW_CIDR
local stratum 10
makestep 0.1 -1
rtcsync
$END_MARK
EOF

"${SUDO[@]}" install -m 0644 "$tmp" "$CONF"
rm -f "$tmp"

"${SUDO[@]}" systemctl enable --now chrony
"${SUDO[@]}" systemctl restart chrony

echo "chrony receiver server configured"
echo "allowed_cidr=$ALLOW_CIDR"
echo
chronyc sources -v || true
echo
chronyc tracking || true
