#!/usr/bin/env bash

gemini_sender_orbbec_runtime_config_source() {
  local root_dir="${ROOT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
  local candidate

  for candidate in \
    "$root_dir/OrbbecSDKConfig_v1.0.xml" \
    "$root_dir/12_build/bin/OrbbecSDKConfig_v1.0.xml" \
    "$root_dir/11_third_party/orbbec/linux_arm64/OrbbecSDK_C_C++_v1.10.27_20250925_0549823_linux_arm64_release/OrbbecSDK_v1.10.27/SDK/config/OrbbecSDKConfig_v1.0.xml" \
    "$root_dir/11_third_party/orbbec/linux_arm64/OrbbecSDK_v2.8.6/config/OrbbecSDKConfig_v1.0.xml"; do
    if [[ -f "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  return 1
}

gemini_sender_orbbec_sync_runtime_config() {
  local root_dir="${ROOT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
  local source_config
  local runtime_config="$root_dir/OrbbecSDKConfig_v1.0.xml"

  if ! source_config="$(gemini_sender_orbbec_runtime_config_source)"; then
    printf '%s [ORBBEC_GUARD] no OrbbecSDKConfig_v1.0.xml source found\n' "$(date '+%Y-%m-%d %H:%M:%S')" >&2
    return 0
  fi
  if [[ "$source_config" != "$runtime_config" ]]; then
    cp -f "$source_config" "$runtime_config"
  fi
}

gemini_sender_orbbec_rebind_uvc() {
  local driver_dir="/sys/bus/usb/drivers/uvcvideo"
  local bind_path="$driver_dir/bind"
  local iface dev vendor class iface_name

  if [[ ! -e "$driver_dir" ]] && sudo -n true 2>/dev/null; then
    sudo modprobe uvcvideo 2>/dev/null || true
  fi
  [[ -e "$bind_path" ]] || return 0

  shopt -s nullglob
  for iface in /sys/bus/usb/devices/*:*; do
    dev="${iface%:*}"
    [[ -f "$dev/idVendor" && -f "$iface/bInterfaceClass" ]] || continue
    vendor="$(<"$dev/idVendor")"
    class="$(<"$iface/bInterfaceClass")"
    [[ "$vendor" == "2bc5" && "$class" == "0e" ]] || continue
    [[ ! -e "$iface/driver" ]] || continue

    iface_name="${iface##*/}"
    if [[ -w "$bind_path" ]]; then
      printf '%s' "$iface_name" >"$bind_path" 2>/dev/null || true
    elif sudo -n true 2>/dev/null; then
      printf '%s' "$iface_name" | sudo tee "$bind_path" >/dev/null 2>&1 || true
    else
      printf '%s [ORBBEC_GUARD] Orbbec UVC interface is unbound but sudo is unavailable iface=%s\n' \
        "$(date '+%Y-%m-%d %H:%M:%S')" "$iface_name" >&2
    fi
  done
  shopt -u nullglob
}

gemini_sender_orbbec_prepare_runtime() {
  gemini_sender_orbbec_sync_runtime_config
  gemini_sender_orbbec_rebind_uvc
}
