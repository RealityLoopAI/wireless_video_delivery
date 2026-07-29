#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_FILE="$ROOT_DIR/wake_runtime.log"
MAX_LOG_BYTES="${XIAOHUAN_WAKE_MAX_LOG_BYTES:-5242880}"
MAX_LOG_FILES="${XIAOHUAN_WAKE_MAX_LOG_FILES:-5}"
DEVICE_WAIT_SECONDS="${XIAOHUAN_DEVICE_WAIT_SECONDS:-300}"
DEVICE_WAIT_INTERVAL_SECONDS="${XIAOHUAN_DEVICE_WAIT_INTERVAL_SECONDS:-2}"
ALLOW_DEVICE_FALLBACK="${XIAOHUAN_ALLOW_DEVICE_FALLBACK:-0}"
DEFAULT_RECORD_DEVICE="plughw:4,0"
DEFAULT_PLAYBACK_DEVICE="plughw:3,0"
RECORD_CARD_MATCH="${XIAOHUAN_RECORD_CARD_MATCH:-USB PnP Sound Device}"
PLAYBACK_CARD_MATCH="${XIAOHUAN_PLAYBACK_CARD_MATCH:-USB2.0 Device}"
AUDIO_STREAM_ENABLED="${XIAOHUAN_AUDIO_STREAM_ENABLED:-0}"
AUDIO_STREAM_HOST="${XIAOHUAN_AUDIO_STREAM_HOST:-127.0.0.1}"
AUDIO_STREAM_PORT="${XIAOHUAN_AUDIO_STREAM_PORT:-50020}"
AUDIO_STREAM_SAMPLE_RATE="${XIAOHUAN_AUDIO_STREAM_SAMPLE_RATE:-48000}"
AUDIO_STREAM_BITRATE="${XIAOHUAN_AUDIO_STREAM_BITRATE:-64000}"

cd "$ROOT_DIR"

rotate_log_if_needed() {
  if [[ ! -f "$LOG_FILE" ]]; then
    return
  fi
  local size
  size="$(wc -c < "$LOG_FILE" 2>/dev/null || echo 0)"
  if (( size < MAX_LOG_BYTES )); then
    return
  fi

  local stamp
  stamp="$(date +%Y%m%d_%H%M%S)"
  mv "$LOG_FILE" "$LOG_FILE.$stamp"
  find "$ROOT_DIR" -maxdepth 1 -type f -name 'wake_runtime.log.*' -printf '%T@ %p\n' \
    | sort -nr \
    | awk -v keep="$MAX_LOG_FILES" 'NR > keep {print $2}' \
    | xargs -r rm -f
}

rotate_log_if_needed

resolve_alsa_card_by_match() {
  local pattern="$1"
  awk -v pattern="$pattern" '
    /^[[:space:]]*[0-9]+[[:space:]]+\[/ {
      card=$1
      line=$0
      desc=""
      if (getline nextline > 0) {
        desc=nextline
      }
      text=line " " desc
      if (index(text, pattern) > 0) {
        print card
        exit
      }
    }
  ' /proc/asound/cards 2>/dev/null || true
}

resolve_alsa_device() {
  local explicit="$1"
  local match="$2"
  local fallback="$3"
  if [[ -n "$explicit" ]]; then
    printf '%s\n' "$explicit"
    return
  fi
  local card
  card="$(resolve_alsa_card_by_match "$match")"
  if [[ -n "$card" ]]; then
    printf 'plughw:%s,0\n' "$card"
  elif [[ "$ALLOW_DEVICE_FALLBACK" == "1" ]]; then
    printf '%s\n' "$fallback"
  else
    return 1
  fi
}

wait_for_audio_devices() {
  local start_ts now_ts elapsed
  start_ts="$(date +%s)"
  while true; do
    if RECORD_DEVICE="$(resolve_alsa_device "${XIAOHUAN_RECORD_DEVICE:-}" "$RECORD_CARD_MATCH" "$DEFAULT_RECORD_DEVICE")" \
      && PLAYBACK_DEVICE="$(resolve_alsa_device "${XIAOHUAN_PLAYBACK_DEVICE:-}" "$PLAYBACK_CARD_MATCH" "$DEFAULT_PLAYBACK_DEVICE")"; then
      return 0
    fi

    now_ts="$(date +%s)"
    elapsed=$((now_ts - start_ts))
    if (( elapsed >= DEVICE_WAIT_SECONDS )); then
      echo "audio device wait timed out after ${elapsed}s record_match=$RECORD_CARD_MATCH playback_match=$PLAYBACK_CARD_MATCH"
      return 1
    fi
    echo "waiting for audio devices elapsed=${elapsed}s record_match=$RECORD_CARD_MATCH playback_match=$PLAYBACK_CARD_MATCH"
    sleep "$DEVICE_WAIT_INTERVAL_SECONDS"
  done
}

{
  echo
  echo "==== xiaohuan wake service start $(date -Is) ===="
  wait_for_audio_devices
  echo "resolved_record_device=$RECORD_DEVICE match=$RECORD_CARD_MATCH"
  echo "resolved_playback_device=$PLAYBACK_DEVICE match=$PLAYBACK_CARD_MATCH"
  AUDIO_STREAM_ARGS=(--no-audio-stream)
  if [[ "$AUDIO_STREAM_ENABLED" == "1" ]]; then
    AUDIO_STREAM_ARGS=(
      --audio-stream
      --audio-stream-host "$AUDIO_STREAM_HOST"
      --audio-stream-port "$AUDIO_STREAM_PORT"
      --audio-stream-sample-rate "$AUDIO_STREAM_SAMPLE_RATE"
      --audio-stream-bitrate "$AUDIO_STREAM_BITRATE"
    )
  fi
  exec python3 -u vosk_wake.py listen \
    --record-device "$RECORD_DEVICE" \
    --playback-device "$PLAYBACK_DEVICE" \
    "${AUDIO_STREAM_ARGS[@]}" \
    --no-barge-in
} >> "$LOG_FILE" 2>&1
