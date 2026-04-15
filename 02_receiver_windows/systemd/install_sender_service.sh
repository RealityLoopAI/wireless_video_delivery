#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
SERVICE_NAME="gemini-wireless-sender.service"
SOURCE_UNIT="${PROJECT_DIR}/systemd/${SERVICE_NAME}"
TARGET_UNIT="/etc/systemd/system/${SERVICE_NAME}"

if [[ "${EUID}" -ne 0 ]]; then
  echo "Please run with sudo."
  exit 1
fi

cp "${SOURCE_UNIT}" "${TARGET_UNIT}"
chmod 644 "${TARGET_UNIT}"

systemctl daemon-reload
systemctl enable "${SERVICE_NAME}"

echo "Installed ${SERVICE_NAME}"
echo "Edit ${PROJECT_DIR}/systemd/gemini-wireless-sender.env if needed."
echo "Start with: sudo systemctl start ${SERVICE_NAME}"
echo "Status with: sudo systemctl status ${SERVICE_NAME}"

