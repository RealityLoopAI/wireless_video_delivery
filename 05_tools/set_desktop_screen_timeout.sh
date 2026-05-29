#!/usr/bin/env bash
set -euo pipefail

TIMEOUT_SECONDS="${1:-${GEMINI_DESKTOP_IDLE_DELAY_SECONDS:-3600}}"

if ! [[ "$TIMEOUT_SECONDS" =~ ^[0-9]+$ ]]; then
  echo "usage: $0 [idle-delay-seconds]" >&2
  exit 2
fi

if ! command -v gsettings >/dev/null 2>&1; then
  echo "gsettings is required to update the desktop screen timeout" >&2
  exit 1
fi

gsettings set org.gnome.desktop.session idle-delay "$TIMEOUT_SECONDS"
gsettings set org.gnome.desktop.screensaver lock-delay 0

echo "desktop idle-delay=$(gsettings get org.gnome.desktop.session idle-delay)"
echo "screensaver lock-delay=$(gsettings get org.gnome.desktop.screensaver lock-delay)"
