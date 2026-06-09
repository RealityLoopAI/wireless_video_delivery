#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/12_build/bin/gemini_sender"
CONFIG="${1:-$ROOT_DIR/06_configs/sender_rk3588-01_one_camera.json}"
MODE="${2:-preview}"
PID_FILE="$ROOT_DIR/12_build/sender.pid"
CHILD_PID_FILE="$ROOT_DIR/12_build/sender_child.pid"
LOG_FILE="$ROOT_DIR/08_reports/sender_logs/sender_stdout.log"
SDK_LIB="$ROOT_DIR/11_third_party/orbbec/linux_arm64/OrbbecSDK_C_C++_v1.10.27_20250925_0549823_linux_arm64_release/OrbbecSDK_v1.10.27/SDK/lib"
RESTART_DELAY_SECONDS="${GEMINI_SENDER_RESTART_DELAY_SECONDS:-3}"
USB_MISSING_GRACE_SECONDS="${GEMINI_SENDER_USB_MISSING_GRACE_SECONDS:-10}"
DISPLAY_VALUE="${GEMINI_SENDER_DISPLAY:-${DISPLAY:-:1}}"
source "$ROOT_DIR/05_tools/sender_wifi_guard.sh"
gemini_sender_wifi_apply_repo_defaults

mkdir -p "$ROOT_DIR/12_build" "$ROOT_DIR/08_reports/sender_logs"
cd "$ROOT_DIR"

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

log_watchdog "watchdog started mode=$MODE config=$CONFIG pid=$$ wifi_guard=$(gemini_sender_wifi_policy_summary)"

monitor_child_health() {
  local pid="$1"
  while kill -0 "$pid" 2>/dev/null; do
    if ! lsusb | grep -q '2bc5:'; then
      sleep "$USB_MISSING_GRACE_SECONDS"
      if kill -0 "$pid" 2>/dev/null && ! lsusb | grep -q '2bc5:'; then
        log_watchdog "sender child pid=$pid still running with no Orbbec USB after ${USB_MISSING_GRACE_SECONDS}s; killing for restart"
        kill "$pid" 2>/dev/null || true
        return
      fi
    fi
    if gemini_sender_wifi_required && ! gemini_sender_wifi_check_policy; then
      local reason="$GEMINI_SENDER_WIFI_LAST_ERROR"
      log_watchdog "wifi guard failed while sender child pid=$pid: $reason; attempting repair"
      if gemini_sender_wifi_connect_if_configured; then
        sleep 2
        if gemini_sender_wifi_check_policy; then
          log_watchdog "wifi guard repaired link; killing sender child pid=$pid to rebuild media TCP"
        else
          log_watchdog "wifi guard still failing after repair: $GEMINI_SENDER_WIFI_LAST_ERROR; killing sender child pid=$pid"
        fi
      else
        log_watchdog "wifi guard repair command failed: $GEMINI_SENDER_WIFI_LAST_ERROR; killing sender child pid=$pid"
      fi
      kill "$pid" 2>/dev/null || true
      return
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

  if [[ "$MODE" == "no-preview" ]]; then
    LD_LIBRARY_PATH="$SDK_LIB:${LD_LIBRARY_PATH:-}" "$BIN" --config "$CONFIG" --no-preview &
  else
    DISPLAY="$DISPLAY_VALUE" LD_LIBRARY_PATH="$SDK_LIB:${LD_LIBRARY_PATH:-}" "$BIN" --config "$CONFIG" &
  fi

  child_pid="$!"
  echo "$child_pid" > "$CHILD_PID_FILE"
  log_watchdog "sender child started pid=$child_pid"
  monitor_child_health "$child_pid" &
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
