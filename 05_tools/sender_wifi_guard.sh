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
  printf 'iface=%s connection=%s required_ssid=%s min_freq_mhz=%s wifi_powersave=%s' \
    "$(gemini_sender_wifi_iface)" \
    "${GEMINI_SENDER_WIFI_CONNECTION:-未配置}" \
    "${GEMINI_SENDER_WIFI_REQUIRED_SSID:-未配置}" \
    "${GEMINI_SENDER_WIFI_MIN_FREQ_MHZ:-未配置}" \
    "${GEMINI_SENDER_WIFI_DISABLE_POWERSAVE:-1}"
}

gemini_sender_wifi_disable_powersave() {
  local iface connection configured
  iface="$(gemini_sender_wifi_iface)"
  if [[ "${GEMINI_SENDER_WIFI_DISABLE_POWERSAVE:-1}" == "0" ]]; then
    return 0
  fi

  if command -v iw >/dev/null 2>&1; then
    if iw dev "$iface" get power_save 2>/dev/null | grep -qi 'off'; then
      return 0
    fi
    iw dev "$iface" set power_save off >/dev/null 2>&1 || true
    if iw dev "$iface" get power_save 2>/dev/null | grep -qi 'off'; then
      return 0
    fi
    if command -v sudo >/dev/null 2>&1; then
      sudo -n iw dev "$iface" set power_save off >/dev/null 2>&1 || true
      if iw dev "$iface" get power_save 2>/dev/null | grep -qi 'off'; then
        return 0
      fi
    fi
  fi

  if command -v nmcli >/dev/null 2>&1; then
    connection="$(nmcli -g GENERAL.CONNECTION device show "$iface" 2>/dev/null | head -n 1 || true)"
    if [[ -n "$connection" && "$connection" != "--" ]]; then
      configured="$(nmcli -g 802-11-wireless.powersave connection show "$connection" 2>/dev/null | head -n 1 || true)"
      if [[ "$configured" != "2" && "$configured" != "disable" ]]; then
        nmcli connection modify "$connection" 802-11-wireless.powersave 2 >/dev/null 2>&1 || true
      fi
    fi
  fi
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

gemini_sender_wifi_visible_freq_for_ssid() {
  local ssid="$1"
  local iface visible_ssid visible_freq freq
  iface="$(gemini_sender_wifi_iface)"
  if [[ -z "$ssid" ]] || ! command -v nmcli >/dev/null 2>&1; then
    return 1
  fi

  local best_freq=0
  while IFS=: read -r visible_ssid visible_freq _; do
    if [[ "$visible_ssid" != "$ssid" ]]; then
      continue
    fi
    freq="${visible_freq%% *}"
    if [[ "$freq" =~ ^[0-9]+$ ]] && (( freq > best_freq )); then
      best_freq="$freq"
    fi
  done < <(nmcli --escape no -t -f SSID,FREQ dev wifi list ifname "$iface" 2>/dev/null || true)
  if (( best_freq > 0 )); then
    printf '%s\n' "$best_freq"
    return 0
  fi
  return 1
}

gemini_sender_wifi_find_visible_saved_connection() {
  local iface min_freq required_ssid connection fields type autoconnect priority ssid freq
  local best_connection="" best_priority=-999999 best_freq=0
  iface="$(gemini_sender_wifi_iface)"
  min_freq="${GEMINI_SENDER_WIFI_MIN_FREQ_MHZ:-0}"
  required_ssid="${GEMINI_SENDER_WIFI_REQUIRED_SSID:-}"

  if ! command -v nmcli >/dev/null 2>&1; then
    return 1
  fi
  if ! [[ "$min_freq" =~ ^[0-9]+$ ]]; then
    return 1
  fi

  while IFS= read -r connection; do
    [[ -n "$connection" ]] || continue
    mapfile -t fields < <(nmcli -g connection.type,connection.autoconnect,connection.autoconnect-priority,802-11-wireless.ssid connection show "$connection" 2>/dev/null || true)
    type="${fields[0]:-}"
    autoconnect="${fields[1]:-}"
    priority="${fields[2]:-0}"
    ssid="${fields[3]:-}"

    [[ "$type" == "802-11-wireless" ]] || continue
    [[ "$autoconnect" == "yes" ]] || continue
    [[ -n "$ssid" ]] || continue
    if [[ -n "$required_ssid" && "$ssid" != "$required_ssid" ]]; then
      continue
    fi
    if ! [[ "$priority" =~ ^-?[0-9]+$ ]]; then
      priority=0
    fi
    freq="$(gemini_sender_wifi_visible_freq_for_ssid "$ssid" || true)"
    [[ "$freq" =~ ^[0-9]+$ ]] || continue
    (( freq >= min_freq )) || continue

    if [[ -z "$best_connection" ]] || (( priority > best_priority )) || \
      { (( priority == best_priority )) && (( freq > best_freq )); }; then
      best_connection="$connection"
      best_priority="$priority"
      best_freq="$freq"
    fi
  done < <(nmcli -g NAME connection show 2>/dev/null || true)

  [[ -n "$best_connection" ]] || return 1
  printf '%s\n' "$best_connection"
}

gemini_sender_wifi_connect_if_configured() {
  local connection iface min_freq
  connection="${GEMINI_SENDER_WIFI_CONNECTION:-}"
  iface="$(gemini_sender_wifi_iface)"
  min_freq="${GEMINI_SENDER_WIFI_MIN_FREQ_MHZ:-0}"
  GEMINI_SENDER_WIFI_LAST_ERROR=""

  if ! gemini_sender_wifi_required; then
    gemini_sender_wifi_disable_powersave
    return 0
  fi
  if gemini_sender_wifi_check_policy; then
    gemini_sender_wifi_disable_powersave
    return 0
  fi
  if ! command -v nmcli >/dev/null 2>&1; then
    GEMINI_SENDER_WIFI_LAST_ERROR="未找到 nmcli，无法切换 Wi-Fi 连接"
    return 1
  fi
  if [[ -z "$connection" ]]; then
    connection="$(gemini_sender_wifi_find_visible_saved_connection || true)"
    if [[ -z "$connection" ]]; then
      GEMINI_SENDER_WIFI_LAST_ERROR="当前 Wi-Fi 不满足策略，且未找到已保存、可见并满足频率要求的 Wi-Fi 连接"
      return 1
    fi
  fi
  if [[ "$min_freq" =~ ^[0-9]+$ ]] && (( min_freq >= 5000 )); then
    nmcli connection modify "$connection" 802-11-wireless.band a >/dev/null 2>&1 || true
  fi
  if ! nmcli connection up "$connection" ifname "$iface" >/dev/null 2>&1; then
    if gemini_sender_wifi_check_policy; then
      gemini_sender_wifi_disable_powersave
      return 0
    fi
    GEMINI_SENDER_WIFI_LAST_ERROR="无法在 $iface 上启用 Wi-Fi 连接 $connection"
    return 1
  fi
  gemini_sender_wifi_disable_powersave
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
