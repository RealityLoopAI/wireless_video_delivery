#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-$ROOT_DIR/06_configs/receiver_loop.json}"
INSTALL_DIR="/usr/local/lib/gwv3"
UNIT_PATH="/etc/systemd/system/gwv3-nas-auto-mount.service"

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  echo "Run with sudo: sudo $0 [receiver-config]" >&2
  exit 1
fi
[[ -f "$CONFIG" ]] || { echo "Receiver config not found: $CONFIG" >&2; exit 1; }
[[ -f /etc/gwv3/nas-credentials ]] || {
  echo "Missing /etc/gwv3/nas-credentials (must contain username= and password=, mode 600)" >&2
  exit 1
}
command -v mount.cifs >/dev/null 2>&1 || {
  echo "mount.cifs is required; install cifs-utils before factory delivery" >&2
  exit 1
}

install -d -m 0755 "$INSTALL_DIR" /etc/gwv3 /var/lib/gwv3 /run/gwv3
install -m 0755 "$ROOT_DIR/05_tools/nas_mount_manager.py" "$INSTALL_DIR/nas_mount_manager.py"
chmod 0600 /etc/gwv3/nas-credentials

cat > "$UNIT_PATH" <<EOF
[Unit]
Description=GWV3 supplied NAS discovery and mount manager
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/python3 $INSTALL_DIR/nas_mount_manager.py --config $CONFIG
Restart=always
RestartSec=2
RuntimeDirectory=gwv3
StateDirectory=gwv3
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now gwv3-nas-auto-mount.service
systemctl --no-pager --full status gwv3-nas-auto-mount.service
