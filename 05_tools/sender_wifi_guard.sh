#!/usr/bin/env bash

gemini_sender_wifi_iface() {
  printf '%s' "${GEMINI_SENDER_WIFI_IFACE:-wlan0}"
}

gemini_sender_wifi_apply_default_policy() {
  local default_connection="${1:-}"
  local default_min_freq="${2:-}"

  if [[ -n "$default_connection" ]]; then
    if [[ -z "${GEMINI_SENDER_WIFI_CONNECTION+x}" ]]; then
      export GEMINI_SENDER_WIFI_CONNECTION="$default_connection"
    fi
    if [[ -z "${GEMINI_SENDER_WIFI_REQUIRED_SSID+x}" ]]; then
      export GEMINI_SENDER_WIFI_REQUIRED_SSID="$default_connection"
    fi
  fi
  if [[ -n "$default_min_freq" && -z "${GEMINI_SENDER_WIFI_MIN_FREQ_MHZ+x}" ]]; then
    export GEMINI_SENDER_WIFI_MIN_FREQ_MHZ="$default_min_freq"
  fi
}

gemini_sender_wifi_apply_repo_defaults() {
  local default_connection="${GEMINI_SENDER_DEFAULT_WIFI_CONNECTION:-}"
  local default_min_freq="${GEMINI_SENDER_DEFAULT_WIFI_MIN_FREQ_MHZ:-5000}"

  gemini_sender_wifi_apply_default_policy "$default_connection" "$default_min_freq"
}

gemini_sender_wifi_required() {
  [[ -n "${GEMINI_SENDER_WIFI_CONNECTION:-}" \
    || -n "${GEMINI_SENDER_WIFI_REQUIRED_SSID:-}" \
    || -n "${GEMINI_SENDER_WIFI_MIN_FREQ_MHZ:-}" ]]
}

gemini_sender_wifi_policy_summary() {
  printf 'iface=%s connection=%s required_ssid=%s min_freq_mhz=%s' \
    "$(gemini_sender_wifi_iface)" \
    "${GEMINI_SENDER_WIFI_CONNECTION:-未配置}" \
    "${GEMINI_SENDER_WIFI_REQUIRED_SSID:-未配置}" \
    "${GEMINI_SENDER_WIFI_MIN_FREQ_MHZ:-未配置}"
}

gemini_sender_wifi_current_link() {
  local iface
  iface="$(gemini_sender_wifi_iface)"
  if command -v iw >/dev/null 2>&1; then
    iw dev "$iface" link 2>/dev/null || true
    return
  fi
  if command -v nmcli >/dev/null 2>&1; then
    nmcli --escape no -t -f ACTIVE,SSID,FREQ,DEVICE dev wifi list ifname "$iface" 2>/dev/null \
      | awk -F: -v iface="$iface" '$1 == "yes" && $4 == iface {print "SSID: " $2; print "\tfreq: " $3; exit}'
  fi
}

gemini_sender_wifi_current_ssid() {
  local ssid_escaped
  ssid_escaped="$(gemini_sender_wifi_current_link | sed -n 's/^[[:space:]]*SSID: //p' | head -n 1)"
  printf '%b' "$ssid_escaped"
}

gemini_sender_wifi_current_freq() {
  gemini_sender_wifi_current_link | awk '/^[[:space:]]*freq:/ {print $2; exit}'
}

gemini_sender_wifi_connect_if_configured() {
  local connection iface
  connection="${GEMINI_SENDER_WIFI_CONNECTION:-}"
  iface="$(gemini_sender_wifi_iface)"
  GEMINI_SENDER_WIFI_LAST_ERROR=""

  if [[ -z "$connection" ]]; then
    return 0
  fi
  if gemini_sender_wifi_check_policy; then
    return 0
  fi
  if ! command -v nmcli >/dev/null 2>&1; then
    GEMINI_SENDER_WIFI_LAST_ERROR="未找到 nmcli，无法切换到 Wi-Fi 连接 $connection"
    return 1
  fi
  if ! nmcli connection up "$connection" ifname "$iface" >/dev/null 2>&1; then
    if gemini_sender_wifi_check_policy; then
      return 0
    fi
    GEMINI_SENDER_WIFI_LAST_ERROR="无法在 $iface 上启用 Wi-Fi 连接 $connection"
    return 1
  fi
}

gemini_sender_wifi_check_policy() {
  local iface link ssid freq min_freq required_ssid
  iface="$(gemini_sender_wifi_iface)"
  required_ssid="${GEMINI_SENDER_WIFI_REQUIRED_SSID:-}"
  min_freq="${GEMINI_SENDER_WIFI_MIN_FREQ_MHZ:-}"
  GEMINI_SENDER_WIFI_LAST_ERROR=""

  if ! gemini_sender_wifi_required; then
    return 0
  fi
  if ! command -v iw >/dev/null 2>&1 && ! command -v nmcli >/dev/null 2>&1; then
    GEMINI_SENDER_WIFI_LAST_ERROR="未找到 iw/nmcli，无法检查 Wi-Fi 链路"
    return 1
  fi

  link="$(gemini_sender_wifi_current_link)"
  if [[ -z "$link" || "$link" == *"Not connected."* ]]; then
    GEMINI_SENDER_WIFI_LAST_ERROR="$iface 未连接 Wi-Fi"
    return 1
  fi

  ssid="$(gemini_sender_wifi_current_ssid)"
  if [[ -n "$required_ssid" && "$ssid" != "$required_ssid" ]]; then
    GEMINI_SENDER_WIFI_LAST_ERROR="$iface 当前 SSID 为 ${ssid:-未知}，要求 $required_ssid"
    return 1
  fi

  if [[ -n "$min_freq" ]]; then
    if ! [[ "$min_freq" =~ ^[0-9]+$ ]]; then
      GEMINI_SENDER_WIFI_LAST_ERROR="GEMINI_SENDER_WIFI_MIN_FREQ_MHZ 必须是数字，当前为 $min_freq"
      return 1
    fi
    freq="$(gemini_sender_wifi_current_freq)"
    if ! [[ "$freq" =~ ^[0-9]+$ ]]; then
      GEMINI_SENDER_WIFI_LAST_ERROR="无法读取 $iface 当前 Wi-Fi 频率"
      return 1
    fi
    if (( freq < min_freq )); then
      GEMINI_SENDER_WIFI_LAST_ERROR="$iface 当前频率 ${freq}MHz，低于要求 ${min_freq}MHz"
      return 1
    fi
  fi
}
