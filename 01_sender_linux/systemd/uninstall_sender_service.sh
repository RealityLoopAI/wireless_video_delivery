#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="gemini-wireless-sender.service"
TARGET_UNIT="/etc/systemd/system/${SERVICE_NAME}"

if [[ "${EUID}" -ne 0 ]]; then
  echo "Please run with sudo."
  exit 1
fi

systemctl disable --now "${SERVICE_NAME}" || true
rm -f "${TARGET_UNIT}"
systemctl daemon-reload

echo "Removed ${SERVICE_NAME}"

