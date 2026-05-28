#!/usr/bin/env bash
set -u

write_sysfs() {
  local path="$1"
  local value="$2"
  [[ -e "$path" ]] || return 0
  if [[ -w "$path" ]]; then
    printf '%s\n' "$value" >"$path" 2>/dev/null || true
  else
    printf '%s\n' "$value" | sudo -n tee "$path" >/dev/null 2>&1 || true
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
