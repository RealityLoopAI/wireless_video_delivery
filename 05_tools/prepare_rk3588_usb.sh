#!/usr/bin/env bash
set -u

write_sysfs() {
  local path="$1"
  local value="$2"
  [[ -e "$path" ]] || return 0
  if [[ -r "$path" ]] && [[ "$(<"$path")" == "$value" ]]; then
    return 0
  fi
  if [[ -w "$path" ]]; then
    if ! printf '%s\n' "$value" >"$path" 2>/dev/null; then
      printf 'prepare warning: failed to write %s=%s\n' "$path" "$value" >&2
    fi
  else
    if ! printf '%s\n' "$value" | sudo -n tee "$path" >/dev/null 2>&1; then
      printf 'prepare warning: need root permission to write %s=%s\n' "$path" "$value" >&2
    fi
  fi
}

warn_if_less_than() {
  local path="$1"
  local minimum="$2"
  local label="$3"
  [[ -r "$path" ]] || return 0
  local value
  value="$(<"$path")"
  [[ "$value" =~ ^[0-9]+$ ]] || return 0
  if (( value < minimum )); then
    printf 'prepare warning: %s is %s, expected at least %s\n' "$label" "$value" "$minimum" >&2
  fi
}

pin_usb_power() {
  local device="$1"
  write_sysfs "$device/power/control" on
  write_sysfs "$device/power/autosuspend" -1
}

otg_mode="/sys/devices/platform/fd5d0000.syscon/fd5d0000.syscon:usb2-phy@0/otg_mode"
if [[ -e "$otg_mode" ]] && ! grep -qx host "$otg_mode" 2>/dev/null; then
  write_sysfs "$otg_mode" host
  sleep 1
fi

write_sysfs /sys/module/usbcore/parameters/usbfs_memory_mb 256
write_sysfs /sys/module/usbcore/parameters/autosuspend -1
write_sysfs /proc/sys/net/core/wmem_max 33554432
write_sysfs /proc/sys/net/core/wmem_default 4194304
warn_if_less_than /sys/module/usbcore/parameters/usbfs_memory_mb 256 usbfs_memory_mb
warn_if_less_than /proc/sys/net/core/wmem_max 4194304 net.core.wmem_max

for device in /sys/bus/usb/devices/*; do
  [[ -f "$device/idVendor" ]] || continue
  vendor="$(<"$device/idVendor")"
  product="$(<"$device/idProduct")"
  class="$(<"$device/bDeviceClass")"

  # Orbbec cameras, the UGREEN HID receiver, and all hubs stay awake.
  if [[ "$vendor" == "2bc5" || "$vendor:$product" == "2b89:0043" || "$class" == "09" ]]; then
    pin_usb_power "$device"
  fi
done
