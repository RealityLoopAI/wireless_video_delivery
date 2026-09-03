#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHARE="${1:-recordings}"
INSTALL_DIR="/usr/local/lib/gwv3"
CONFIG_PATH="/etc/gwv3/nas-beacon.json"
UNIT_PATH="/etc/systemd/system/gwv3-nas-discovery.service"

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  echo "Run with sudo: sudo $0 [smb-share-name]" >&2
  exit 1
fi
if [[ ! "$SHARE" =~ ^[A-Za-z0-9_$.\-]{1,80}$ ]]; then
  echo "Invalid SMB share name" >&2
  exit 1
fi

install -d -m 0755 "$INSTALL_DIR" /etc/gwv3
install -m 0755 "$ROOT_DIR/05_tools/nas_discovery_beacon.py" "$INSTALL_DIR/nas_discovery_beacon.py"
if [[ ! -f "$CONFIG_PATH" ]]; then
  cat > "$CONFIG_PATH" <<EOF
{
  "enabled": true,
  "nas_id": "auto",
  "bind_ip": "0.0.0.0",
  "port": 50008,
  "share": "$SHARE"
}
EOF
  chmod 0644 "$CONFIG_PATH"
fi

cat > "$UNIT_PATH" <<EOF
[Unit]
Description=GWV3 supplied NAS discovery beacon
After=network-online.target smb.service smbd.service
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/python3 $INSTALL_DIR/nas_discovery_beacon.py --config $CONFIG_PATH
Restart=always
RestartSec=2
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now gwv3-nas-discovery.service
systemctl --no-pager --full status gwv3-nas-discovery.service
