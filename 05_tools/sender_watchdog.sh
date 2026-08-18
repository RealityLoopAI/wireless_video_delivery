#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/12_build/bin/gemini_sender"
CONFIG="${1:-$ROOT_DIR/06_configs/sender_rk3588-01_one_camera.json}"
MODE="${2:-preview}"
PID_FILE="$ROOT_DIR/12_build/sender.pid"
CHILD_PID_FILE="$ROOT_DIR/12_build/sender_child.pid"
LOCK_FILE="$ROOT_DIR/12_build/sender.lock"
LOG_FILE="$ROOT_DIR/08_reports/sender_logs/sender_stdout.log"
RESTART_DELAY_SECONDS="${GEMINI_SENDER_RESTART_DELAY_SECONDS:-3}"
USB_MISSING_GRACE_SECONDS="${GEMINI_SENDER_USB_MISSING_GRACE_SECONDS:-10}"
ZERO_FPS_GRACE_SECONDS="${GEMINI_SENDER_ZERO_FPS_GRACE_SECONDS:-20}"
ZERO_FPS_RESTART_SAMPLES="${GEMINI_SENDER_ZERO_FPS_RESTART_SAMPLES:-15}"
STARTUP_PENDING_GRACE_SECONDS="${GEMINI_SENDER_STARTUP_PENDING_GRACE_SECONDS:-25}"
WIFI_OUTAGE_GRACE_SECONDS="${GEMINI_SENDER_WIFI_OUTAGE_GRACE_SECONDS:-45}"
DISPLAY_VALUE="${GEMINI_SENDER_DISPLAY:-${DISPLAY:-:1}}"
if ! [[ "$WIFI_OUTAGE_GRACE_SECONDS" =~ ^[0-9]+$ ]] || [[ "$WIFI_OUTAGE_GRACE_SECONDS" -lt 5 ]]; then
  WIFI_OUTAGE_GRACE_SECONDS=45
fi
source "$ROOT_DIR/05_tools/sender_wifi_guard.sh"
source "$ROOT_DIR/05_tools/orbbec_runtime_guard.sh"
gemini_sender_wifi_apply_repo_defaults

resolve_sender_sdk_lib() {
  local linked_lib=""
  linked_lib="$(ldd "$BIN" 2>/dev/null | awk '/libOrbbecSDK/ {print $3; exit}')"
  if [[ -n "$linked_lib" && -f "$linked_lib" ]]; then
    dirname "$linked_lib"
    return
  fi

  local candidate
  for candidate in \
    "$ROOT_DIR/11_third_party/orbbec/linux_arm64/OrbbecSDK_v2.8.6/lib" \
    "$ROOT_DIR/11_third_party/orbbec/linux_arm64/OrbbecSDK_C_C++_v1.10.27_20250925_0549823_linux_arm64_release/OrbbecSDK_v1.10.27/SDK/lib"; do
    if [[ -d "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return
    fi
  done
}

SDK_LIB="${GEMINI_SENDER_SDK_LIB:-$(resolve_sender_sdk_lib)}"

mkdir -p "$ROOT_DIR/12_build" "$ROOT_DIR/08_reports/sender_logs"
"$ROOT_DIR/05_tools/rotate_sender_logs.sh" || true
cd "$ROOT_DIR"

exec 9>"$LOCK_FILE"
if ! flock -n 9; then
  printf '%s [WATCHDOG] another sender watchdog is already running; exiting pid=%s config=%s\n' \
    "$(date '+%Y-%m-%d %H:%M:%S')" "$$" "$CONFIG" >> "$LOG_FILE"
  exit 0
fi

echo "$$" > "$PID_FILE"
child_pid=""
monitor_pid=""
stopping=0

log_watchdog() {
  printf '%s [WATCHDOG] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOG_FILE"
}

cleanup() {
  stopping=1
  if [[ -n "${monitor_pid:-}" ]] && kill -0 "$monitor_pid" 2>/dev/null; then
    kill "$monitor_pid" 2>/dev/null || true
    wait "$monitor_pid" 2>/dev/null || true
  fi
  if [[ -n "${child_pid:-}" ]] && kill -0 "$child_pid" 2>/dev/null; then
    kill "$child_pid" 2>/dev/null || true
    wait "$child_pid" 2>/dev/null || true
  fi
  rm -f "$PID_FILE" "$CHILD_PID_FILE"
}

trap cleanup INT TERM HUP EXIT

log_watchdog "watchdog started mode=$MODE config=$CONFIG pid=$$ sdk_lib=${SDK_LIB:-auto} wifi_guard=$(gemini_sender_wifi_policy_summary)"

monitor_child_health() {
  local pid="$1"
  local initial_log_offset="${2:-0}"
  local child_started_at
  local log_offset="$initial_log_offset"
  local current_size=0
  local new_bytes=0
  local wifi_outage_started_at=0
  local wifi_outage_last_log_at=0
  local wifi_outage_elapsed=0
  child_started_at="$(date +%s)"
  declare -A camera_started=()
  declare -A startup_attempt_at=()
  declare -A zero_fps_samples=()

  check_sender_perf_health() {
    local now elapsed line camera_id
    now="$(date +%s)"
    elapsed=$((now - child_started_at))
    current_size="$(wc -c < "$LOG_FILE" 2>/dev/null || echo 0)"
    if [[ "$current_size" -lt "$log_offset" ]]; then
      log_offset=0
    fi
    if [[ "$current_size" -le "$log_offset" ]]; then
      return 0
    fi

    new_bytes=$((current_size - log_offset))
    log_offset="$current_size"
    while IFS= read -r line; do
      if [[ "$line" =~ camera\ started\ camera_id=([^[:space:]]+) ]]; then
        camera_id="${BASH_REMATCH[1]}"
        camera_started["$camera_id"]=1
        unset "startup_attempt_at[$camera_id]"
        zero_fps_samples["$camera_id"]=0
        continue
      fi
      if [[ "$line" =~ camera\ reconnect\ attempt\ camera_id=([^[:space:]]+) ]]; then
        camera_id="${BASH_REMATCH[1]}"
        startup_attempt_at["$camera_id"]="$now"
        continue
      fi
      if [[ "$line" =~ camera\ reconnect\ failed\ camera_id=([^[:space:]]+) ]]; then
        camera_id="${BASH_REMATCH[1]}"
        unset "startup_attempt_at[$camera_id]"
        continue
      fi
      if [[ "$line" =~ camera\ disconnected\ camera_id=([^[:space:]]+) ]]; then
        camera_id="${BASH_REMATCH[1]}"
        unset "camera_started[$camera_id]"
        unset "startup_attempt_at[$camera_id]"
        zero_fps_samples["$camera_id"]=0
        continue
      fi
      if [[ ! "$line" =~ perf\ camera_id=([^[:space:]]+) ]]; then
        continue
      fi
      camera_id="${BASH_REMATCH[1]}"
      if [[ -z "${camera_started[$camera_id]:-}" ]]; then
        continue
      fi

      if [[ "$line" == *"rgb_input_fps=0.00 depth_input_fps=0.00"* \
        && "$line" == *"rgb_sent_packets_s=0.00"* \
        && "$line" == *"depth_sent_fps=0.00"* ]]; then
        if [[ "$elapsed" -lt "$ZERO_FPS_GRACE_SECONDS" ]]; then
          continue
        fi
        zero_fps_samples["$camera_id"]=$(( ${zero_fps_samples[$camera_id]:-0} + 1 ))
        if [[ "${zero_fps_samples[$camera_id]}" -ge "$ZERO_FPS_RESTART_SAMPLES" ]]; then
          log_watchdog "sender child pid=$pid camera_id=$camera_id has ${zero_fps_samples[$camera_id]} consecutive zero-fps perf samples after ${elapsed}s; killing for restart"
          kill "$pid" 2>/dev/null || true
          return 1
        fi
      else
        zero_fps_samples["$camera_id"]=0
      fi
    done < <(tail -c "$new_bytes" "$LOG_FILE" 2>/dev/null || true)

    for camera_id in "${!startup_attempt_at[@]}"; do
      if [[ $((now - startup_attempt_at[$camera_id])) -ge "$STARTUP_PENDING_GRACE_SECONDS" ]]; then
        log_watchdog "sender child pid=$pid camera_id=$camera_id startup pending for $((now - startup_attempt_at[$camera_id]))s; killing for restart"
        kill "$pid" 2>/dev/null || true
        return 1
      fi
    done
  }

  while kill -0 "$pid" 2>/dev/null; do
    if ! check_sender_perf_health; then
      return
    fi
    if ! lsusb | grep -q '2bc5:'; then
      sleep "$USB_MISSING_GRACE_SECONDS"
      if kill -0 "$pid" 2>/dev/null && ! lsusb | grep -q '2bc5:'; then
        log_watchdog "sender child pid=$pid still running with no Orbbec USB after ${USB_MISSING_GRACE_SECONDS}s; killing for restart"
        kill "$pid" 2>/dev/null || true
        return
      fi
    fi
    if gemini_sender_wifi_required; then
      if gemini_sender_wifi_check_policy; then
        if [[ "$wifi_outage_started_at" -gt 0 ]]; then
          wifi_outage_elapsed=$(( $(date +%s) - wifi_outage_started_at ))
          log_watchdog "wifi link recovered after ${wifi_outage_elapsed}s; preserving sender child pid=$pid for media queue replay"
          wifi_outage_started_at=0
          wifi_outage_last_log_at=0
        fi
      else
        local reason="$GEMINI_SENDER_WIFI_LAST_ERROR"
        local now
        now="$(date +%s)"
        if [[ "$wifi_outage_started_at" -eq 0 ]]; then
          wifi_outage_started_at="$now"
          wifi_outage_last_log_at="$now"
          log_watchdog "wifi guard detected transient outage while sender child pid=$pid: $reason; preserving capture and media queues for ${WIFI_OUTAGE_GRACE_SECONDS}s"
        fi
        wifi_outage_elapsed=$((now - wifi_outage_started_at))
        if [[ "$wifi_outage_elapsed" -lt "$WIFI_OUTAGE_GRACE_SECONDS" ]]; then
          if [[ $((now - wifi_outage_last_log_at)) -ge 10 ]]; then
            log_watchdog "wifi outage still within grace child_pid=$pid elapsed_s=$wifi_outage_elapsed reason=$reason"
            wifi_outage_last_log_at="$now"
          fi
          sleep 2
          continue
        fi

        log_watchdog "wifi outage grace expired child_pid=$pid elapsed_s=$wifi_outage_elapsed reason=$reason; attempting repair"
        if gemini_sender_wifi_connect_if_configured; then
          sleep 2
          if gemini_sender_wifi_check_policy; then
            log_watchdog "wifi guard repaired link; preserving sender child pid=$pid for media TCP reconnect"
            wifi_outage_started_at=0
            wifi_outage_last_log_at=0
            continue
          fi
          log_watchdog "wifi guard still failing after repair: $GEMINI_SENDER_WIFI_LAST_ERROR; killing sender child pid=$pid"
        else
          log_watchdog "wifi guard repair command failed: $GEMINI_SENDER_WIFI_LAST_ERROR; killing sender child pid=$pid"
        fi
        kill "$pid" 2>/dev/null || true
        return
      fi
    fi
    sleep 2
  done
}

while [[ "$stopping" -eq 0 ]]; do
  if gemini_sender_wifi_required; then
    if ! gemini_sender_wifi_connect_if_configured; then
      log_watchdog "wifi guard connect failed before child start: $GEMINI_SENDER_WIFI_LAST_ERROR; retrying in ${RESTART_DELAY_SECONDS}s"
      sleep "$RESTART_DELAY_SECONDS"
      continue
    fi
    if ! gemini_sender_wifi_check_policy; then
      log_watchdog "wifi guard check failed before child start: $GEMINI_SENDER_WIFI_LAST_ERROR; retrying in ${RESTART_DELAY_SECONDS}s"
      sleep "$RESTART_DELAY_SECONDS"
      continue
    fi
  fi

  gemini_sender_orbbec_prepare_runtime

  child_log_offset="$(wc -c < "$LOG_FILE" 2>/dev/null || echo 0)"
  if [[ "$MODE" == "no-preview" ]]; then
    LD_LIBRARY_PATH="${SDK_LIB:+$SDK_LIB:}${LD_LIBRARY_PATH:-}" "$BIN" --config "$CONFIG" --no-preview &
  elif [[ "$MODE" == "no-local-preview" ]]; then
    LD_LIBRARY_PATH="${SDK_LIB:+$SDK_LIB:}${LD_LIBRARY_PATH:-}" "$BIN" --config "$CONFIG" --no-local-preview &
  else
    DISPLAY="$DISPLAY_VALUE" LD_LIBRARY_PATH="${SDK_LIB:+$SDK_LIB:}${LD_LIBRARY_PATH:-}" "$BIN" --config "$CONFIG" &
  fi

  child_pid="$!"
  echo "$child_pid" > "$CHILD_PID_FILE"
  log_watchdog "sender child started pid=$child_pid"
  monitor_child_health "$child_pid" "$child_log_offset" &
  monitor_pid="$!"

  set +e
  wait "$child_pid"
  exit_code="$?"
  set -e
  if [[ -n "${monitor_pid:-}" ]] && kill -0 "$monitor_pid" 2>/dev/null; then
    kill "$monitor_pid" 2>/dev/null || true
    wait "$monitor_pid" 2>/dev/null || true
  fi
  monitor_pid=""
  child_pid=""
  rm -f "$CHILD_PID_FILE"

  if [[ "$stopping" -ne 0 ]]; then
    break
  fi

  log_watchdog "sender child exited code=$exit_code; restarting in ${RESTART_DELAY_SECONDS}s"
  sleep "$RESTART_DELAY_SECONDS"
done
