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
MIC_CAPTURE_LEVEL="${XIAOHUAN_MIC_CAPTURE_LEVEL:-62%}"
MIC_AGC="${XIAOHUAN_MIC_AGC:-off}"
WAKE_DECODE_MIN_RMS="${XIAOHUAN_WAKE_DECODE_MIN_RMS:-0.016}"
DECODE_NOISE_MARGIN="${XIAOHUAN_DECODE_NOISE_MARGIN:-0.006}"
WAKE_RESPONSE_WAV="${XIAOHUAN_WAKE_RESPONSE_WAV:-$ROOT_DIR/response_wozai_tts_fast.wav}"
ECHO_TAIL_SECONDS="${XIAOHUAN_ECHO_TAIL_SECONDS:-0.10}"
TTS_HTTP_ENABLED="${XIAOHUAN_TTS_HTTP_ENABLED:-1}"
TTS_HTTP_BIND="${XIAOHUAN_TTS_HTTP_BIND:-0.0.0.0}"
TTS_HTTP_PORT="${XIAOHUAN_TTS_HTTP_PORT:-18082}"
TTS_BACKEND="${XIAOHUAN_TTS_BACKEND:-espeak}"
TTS_MODEL_DIR="${XIAOHUAN_TTS_MODEL_DIR:-$ROOT_DIR/models/vits-melo-tts-zh_en}"
TTS_NUM_THREADS="${XIAOHUAN_TTS_NUM_THREADS:-4}"
TTS_EDGE_VOICE="${XIAOHUAN_TTS_EDGE_VOICE:-zh-CN-XiaoyiNeural}"
TTS_EDGE_TIMEOUT_SECONDS="${XIAOHUAN_TTS_EDGE_TIMEOUT_SECONDS:-4}"
TTS_EDGE_CACHE_ENTRIES="${XIAOHUAN_TTS_EDGE_CACHE_ENTRIES:-64}"
TTS_SPEAKER_RETRY_SECONDS="${XIAOHUAN_TTS_SPEAKER_RETRY_SECONDS:-5}"
TTS_RESUME_DELAY_SECONDS="${XIAOHUAN_TTS_RESUME_DELAY_SECONDS:-0.2}"

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

configure_record_mixer() {
  if ! command -v amixer >/dev/null 2>&1; then
    echo "amixer unavailable; record mixer configuration skipped"
    return
  fi

  local device_suffix card
  device_suffix="${RECORD_DEVICE#*:}"
  card="${device_suffix%%,*}"
  if [[ ! "$card" =~ ^[0-9]+$ ]]; then
    echo "record mixer configuration skipped for non-numeric device=$RECORD_DEVICE"
    return
  fi

  if amixer -c "$card" sget "Mic" >/dev/null 2>&1; then
    amixer -q -c "$card" sset "Mic" "$MIC_CAPTURE_LEVEL" unmute
    echo "record mic level applied card=$card level=$MIC_CAPTURE_LEVEL"
  fi
  if amixer -c "$card" sget "Auto Gain Control" >/dev/null 2>&1; then
    amixer -q -c "$card" sset "Auto Gain Control" "$MIC_AGC"
    echo "record AGC applied card=$card state=$MIC_AGC"
  fi
}

{
  echo
  echo "==== xiaohuan wake service start $(date -Is) ===="
  wait_for_audio_devices
  echo "resolved_record_device=$RECORD_DEVICE match=$RECORD_CARD_MATCH"
  echo "resolved_playback_device=$PLAYBACK_DEVICE match=$PLAYBACK_CARD_MATCH"
  configure_record_mixer
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
  TTS_HTTP_ARGS=(--no-tts-http)
  if [[ "$TTS_HTTP_ENABLED" == "1" ]]; then
    TTS_HTTP_ARGS=(
      --tts-http
      --tts-http-bind "$TTS_HTTP_BIND"
      --tts-http-port "$TTS_HTTP_PORT"
    )
  fi
  exec python3 -u vosk_wake.py listen \
    --record-device "$RECORD_DEVICE" \
    --playback-device "$PLAYBACK_DEVICE" \
    "${TTS_HTTP_ARGS[@]}" \
    --tts-backend "$TTS_BACKEND" \
    --tts-model-dir "$TTS_MODEL_DIR" \
    --tts-num-threads "$TTS_NUM_THREADS" \
    --tts-edge-voice "$TTS_EDGE_VOICE" \
    --tts-edge-timeout-seconds "$TTS_EDGE_TIMEOUT_SECONDS" \
    --tts-edge-cache-entries "$TTS_EDGE_CACHE_ENTRIES" \
    --tts-speaker-retry-seconds "$TTS_SPEAKER_RETRY_SECONDS" \
    --tts-resume-delay-seconds "$TTS_RESUME_DELAY_SECONDS" \
    --response-wav "$WAKE_RESPONSE_WAV" \
    --echo-tail-seconds "$ECHO_TAIL_SECONDS" \
    "${AUDIO_STREAM_ARGS[@]}" \
    --decode-min-rms "$WAKE_DECODE_MIN_RMS" \
    --decode-noise-margin "$DECODE_NOISE_MARGIN" \
    --no-allow-split-wake \
    --wake-require-end-silence \
    --wake-decode-max-seconds 3.2 \
    --no-barge-in
} >> "$LOG_FILE" 2>&1
