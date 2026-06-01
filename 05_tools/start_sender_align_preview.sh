#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="$ROOT_DIR/06_configs/sender_rk3588-01_two_cameras_align.json"

exec "$ROOT_DIR/05_tools/start_sender_preview.sh" "$CONFIG"
