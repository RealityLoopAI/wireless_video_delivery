#!/usr/bin/env bash
set -euo pipefail

ROLE="${1:-}"
RUN_USER="${2:-}"
CONFIG="${3:-}"
ROOT_DIR="${4:-}"
[[ "$ROLE" =~ ^(sender|receiver)$ ]] || { echo "invalid role" >&2; exit 2; }
id "$RUN_USER" >/dev/null 2>&1 || { echo "invalid run user" >&2; exit 2; }
[[ -f "$CONFIG" && -d "$ROOT_DIR" ]] || { echo "config or repository missing" >&2; exit 2; }
((EUID == 0)) || { echo "run with sudo" >&2; exit 1; }

install -d -m 0755 /usr/local/lib/gwv3 /var/lib/gwv3
install -m 0755 "$ROOT_DIR/05_tools/gwv3_post_install_verify.sh" \
  /usr/local/lib/gwv3/gwv3_post_install_verify.sh
touch /var/lib/gwv3/post-install-pending
chmod 0644 /var/lib/gwv3/post-install-pending

cat > /etc/systemd/system/gwv3-post-install-verify.service <<EOF
[Unit]
Description=GWV3 one-time post-install verification
After=network-online.target gwv3-gemini-sender.service gwv3-nas-auto-mount.service
Wants=network-online.target
ConditionPathExists=/var/lib/gwv3/post-install-pending

[Service]
Type=oneshot
ExecStart=/usr/local/lib/gwv3/gwv3_post_install_verify.sh $ROLE $RUN_USER $CONFIG $ROOT_DIR
TimeoutStartSec=1200

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable gwv3-post-install-verify.service
