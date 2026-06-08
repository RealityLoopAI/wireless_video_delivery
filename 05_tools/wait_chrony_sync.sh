#!/usr/bin/env bash
set -euo pipefail

MAX_CORRECTION_S="${1:-${GEMINI_CHRONY_MAX_CORRECTION_S:-0.010}}"
TRIES="${GEMINI_CHRONY_WAITSYNC_TRIES:-60}"
MAX_SKEW_PPM="${GEMINI_CHRONY_MAX_SKEW_PPM:-100}"
INTERVAL_S="${GEMINI_CHRONY_WAITSYNC_INTERVAL_S:-1}"

if ! command -v chronyc >/dev/null 2>&1; then
  echo "chronyc not found; install chrony first" >&2
  exit 20
fi

echo "waiting for chrony sync: max_correction_s=$MAX_CORRECTION_S tries=$TRIES interval_s=$INTERVAL_S"
chronyc waitsync "$TRIES" "$MAX_CORRECTION_S" "$MAX_SKEW_PPM" "$INTERVAL_S"

tracking="$(chronyc tracking)"
echo "$tracking"

if ! grep -q 'Leap status[[:space:]]*:[[:space:]]*Normal' <<< "$tracking"; then
  echo "chrony leap status is not Normal" >&2
  exit 21
fi

echo "chrony sync is ready"
