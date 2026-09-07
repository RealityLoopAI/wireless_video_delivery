#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CACHE_DIR="${GWV3_OFFLINE_CACHE:-$ROOT_DIR/11_third_party/cache}"
MANIFEST="$ROOT_DIR/06_configs/deployment/asset-manifest.json"
APP_DIR="$ROOT_DIR/12_apps/xiaohuan_voice_photo"

while (($#)); do
  case "$1" in
    --cache-dir) CACHE_DIR="${2:-}"; shift 2 ;;
    -h|--help) echo "usage: $0 [--cache-dir PATH]"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

((EUID != 0)) || { echo "run this installer as the Sender service user, without sudo" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required" >&2; exit 1; }
command -v unzip >/dev/null 2>&1 || { echo "unzip is required" >&2; exit 1; }

mapfile -t ASSET < <(python3 - "$MANIFEST" <<'PY'
import json, sys
asset=json.load(open(sys.argv[1], encoding="utf-8"))["assets"]["vosk_small_cn_0_22"]
for key in ("archive_name", "url", "sha256", "directory"):
    print(asset[key])
PY
)
ARCHIVE_NAME="${ASSET[0]}"
URL="${ASSET[1]}"
SHA256="${ASSET[2]}"
DIRECTORY="${ASSET[3]}"
ARCHIVE="$CACHE_DIR/$ARCHIVE_NAME"
MODEL_DIR="$APP_DIR/models/$DIRECTORY"

mkdir -p "$CACHE_DIR" "$APP_DIR/models"
if [[ ! -s "$MODEL_DIR/am/final.mdl" ]]; then
  if [[ ! -f "$ARCHIVE" ]]; then
    command -v curl >/dev/null 2>&1 || { echo "voice model is not cached and curl is unavailable" >&2; exit 1; }
    curl --fail --location --retry 5 --retry-delay 2 --output "$ARCHIVE.part" "$URL"
    mv "$ARCHIVE.part" "$ARCHIVE"
  fi
  printf '%s  %s\n' "$SHA256" "$ARCHIVE" | sha256sum --check --status \
    || { echo "voice model checksum mismatch: $ARCHIVE" >&2; exit 1; }
  temporary="$(mktemp -d "$APP_DIR/models/.extract.XXXXXX")"
  trap 'rm -rf "$temporary"' EXIT
  unzip -q "$ARCHIVE" -d "$temporary"
  [[ -s "$temporary/$DIRECTORY/am/final.mdl" ]] || { echo "voice model archive is incomplete" >&2; exit 1; }
  rm -rf "$MODEL_DIR"
  mv "$temporary/$DIRECTORY" "$MODEL_DIR"
fi

if compgen -G "$CACHE_DIR/pip/*.whl" >/dev/null; then
  python3 -m pip install --user --no-index --find-links "$CACHE_DIR/pip" vosk webrtcvad edge-tts
else
  python3 -m pip install --user vosk webrtcvad edge-tts
fi

"$APP_DIR/install_wake_service.sh"
if grep -qi 'SM15 M1 USB audio' /proc/asound/cards 2>/dev/null; then
  mkdir -p "$HOME/.config/systemd/user/xiaohuan-wake.service.d"
  install -m 0644 "$APP_DIR/systemd/xiaohuan-wake-sm15-m1.conf" \
    "$HOME/.config/systemd/user/xiaohuan-wake.service.d/sm15-m1.conf"
  systemctl --user daemon-reload
  systemctl --user restart xiaohuan-wake.service
fi
echo "optional voice/photo service installed"
