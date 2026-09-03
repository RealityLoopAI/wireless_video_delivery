#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-$ROOT_DIR/06_configs/receiver_loop.json}"
INSTALL_DIR="/usr/local/lib/gwv3"
UNIT_PATH="/etc/systemd/system/gwv3-nas-auto-mount.service"
LEGACY_CREDENTIALS="/etc/gwv3-nas-credentials"

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  echo "Run with sudo: sudo $0 [receiver-config]" >&2
  exit 1
fi
[[ -f "$CONFIG" ]] || { echo "Receiver config not found: $CONFIG" >&2; exit 1; }
NAS_ROOT="$(python3 - "$CONFIG" <<'PY'
import json
import os
import sys

with open(sys.argv[1], "r", encoding="utf-8") as source:
    config = json.load(source)
path = os.path.abspath(os.path.expanduser(str(config.get("nas_root") or "")))
if not path or path == "/":
    raise SystemExit("receiver nas_root must be a non-root absolute path")
print(path)
PY
)"
install -d -m 0755 /etc/gwv3
if [[ ! -f /etc/gwv3/nas-credentials && -f "$LEGACY_CREDENTIALS" ]]; then
  install -m 0600 "$LEGACY_CREDENTIALS" /etc/gwv3/nas-credentials
fi
[[ -f /etc/gwv3/nas-credentials ]] || {
  echo "Missing /etc/gwv3/nas-credentials (must contain username= and password=, mode 600)" >&2
  exit 1
}
command -v mount.cifs >/dev/null 2>&1 || {
  echo "mount.cifs is required; install cifs-utils before factory delivery" >&2
  exit 1
}

MOUNT_UNIT="$(systemd-escape --path --suffix=mount "$NAS_ROOT")"
AUTOMOUNT_UNIT="$(systemd-escape --path --suffix=automount "$NAS_ROOT")"
systemctl stop "$MOUNT_UNIT" "$AUTOMOUNT_UNIT" >/dev/null 2>&1 || true

# A fixed-IP fstab entry would race the discovery manager after every boot.
python3 - "$NAS_ROOT" <<'PY'
from pathlib import Path
import shutil
import sys

fstab = Path("/etc/fstab")
mount_point = sys.argv[1]
lines = fstab.read_text(encoding="utf-8").splitlines(keepends=True)
kept = []
removed = []
for line in lines:
    stripped = line.strip()
    fields = stripped.split()
    if stripped and not stripped.startswith("#") and len(fields) >= 3 and fields[1] == mount_point:
        removed.append(line)
    else:
        kept.append(line)
if removed:
    backup = Path("/etc/fstab.gwv3-before-nas-discovery")
    if not backup.exists():
        shutil.copy2(fstab, backup)
    temporary = Path("/etc/.fstab.gwv3.tmp")
    temporary.write_text("".join(kept), encoding="utf-8")
    temporary.chmod(fstab.stat().st_mode & 0o777)
    temporary.replace(fstab)
    print(f"Removed {len(removed)} legacy fstab entry for {mount_point}; backup: {backup}")
PY

install -d -m 0755 "$INSTALL_DIR" /var/lib/gwv3 /run/gwv3 "$NAS_ROOT"
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

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable gwv3-nas-auto-mount.service
systemctl restart gwv3-nas-auto-mount.service
systemctl --no-pager --full status gwv3-nas-auto-mount.service
