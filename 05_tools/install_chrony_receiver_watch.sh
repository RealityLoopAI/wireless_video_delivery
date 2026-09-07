#!/usr/bin/env bash
set -euo pipefail

RUN_USER="${1:-}"
FALLBACK="${2:-}"
ROOT_DIR="${3:-}"
((EUID == 0)) || { echo "run with sudo" >&2; exit 1; }
id "$RUN_USER" >/dev/null 2>&1 || { echo "invalid run user" >&2; exit 2; }
[[ -n "$FALLBACK" && -d "$ROOT_DIR" ]] || { echo "fallback and repository root are required" >&2; exit 2; }
HOME_DIR="$(getent passwd "$RUN_USER" | cut -d: -f6)"
STATE="$HOME_DIR/.local/state/gwv3/receiver_target.json"

install -d -m 0755 /usr/local/lib/gwv3
install -m 0755 "$ROOT_DIR/05_tools/chrony_receiver_watch.py" /usr/local/lib/gwv3/
cat > /etc/systemd/system/gwv3-chrony-receiver-watch.service <<EOF
[Unit]
Description=GWV3 discovered receiver Chrony target
After=network-online.target chrony.service
Wants=network-online.target chrony.service

[Service]
Type=simple
ExecStart=/usr/bin/python3 /usr/local/lib/gwv3/chrony_receiver_watch.py --state $STATE --fallback $FALLBACK --setup $ROOT_DIR/05_tools/setup_sender_chrony_client.sh
Restart=always
RestartSec=5
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=read-only
ReadWritePaths=/etc/chrony /var/lib/gwv3

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now gwv3-chrony-receiver-watch.service
