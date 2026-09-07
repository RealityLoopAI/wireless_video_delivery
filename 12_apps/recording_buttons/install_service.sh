#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEVICE_ID="${1:-lubancat-52d2ef0c}"
UNIT_NAME="gwv3-recording-buttons.service"
POWER_UNIT_NAME="gwv3-power-button.service"
LED_UNIT_NAME="gwv3-recording-led.service"
USER_UNIT_DIR="$HOME/.config/systemd/user"
BUTTON_CONFIG_SOURCE="${GWV3_BUTTON_CONFIG:-$ROOT_DIR/config_${DEVICE_ID}.json}"
POWER_CONFIG_SOURCE="${GWV3_POWER_CONFIG:-$ROOT_DIR/config_${DEVICE_ID}_power.json}"
ENABLE_BUTTONS="${GWV3_ENABLE_BUTTONS:-1}"
ENABLE_POWER="${GWV3_ENABLE_POWER:-1}"
ENABLE_LED="${GWV3_ENABLE_LED:-1}"

if [[ ! "$DEVICE_ID" =~ ^[A-Za-z0-9._-]+$ ]]; then
  echo "invalid device id: $DEVICE_ID" >&2
  exit 2
fi
if [[ "$ENABLE_BUTTONS" == 1 || "$ENABLE_LED" == 1 ]] && [[ ! -f "$BUTTON_CONFIG_SOURCE" ]]; then
  echo "missing recording button/LED config for device id: $DEVICE_ID" >&2
  exit 2
fi
if [[ "$ENABLE_POWER" == 1 && ! -f "$POWER_CONFIG_SOURCE" ]]; then
  echo "missing power button config for device id: $DEVICE_ID" >&2
  exit 2
fi

render_unit() {
  local source="$1" target="$2" temporary escaped
  temporary="$(mktemp)"
  escaped="${ROOT_DIR//&/\\&}"
  sed "s|@APP_DIR@|$escaped|g" "$source" > "$temporary"
  if grep -q '@APP_DIR@' "$temporary"; then
    rm -f "$temporary"
    echo "failed to render $(basename "$source")" >&2
    exit 1
  fi
  install -m 0644 "$temporary" "$target"
  rm -f "$temporary"
}

if [[ "$ENABLE_BUTTONS" == 1 || "$ENABLE_LED" == 1 ]]; then
  install -m 0644 "$BUTTON_CONFIG_SOURCE" "$ROOT_DIR/config_lubancat-local.json"
fi
if [[ "$ENABLE_POWER" == 1 ]]; then
  install -m 0644 "$POWER_CONFIG_SOURCE" "$ROOT_DIR/config_lubancat-local_power.json"
fi

if [[ "$ENABLE_BUTTONS" == 1 ]]; then
  mkdir -p "$USER_UNIT_DIR"
  render_unit "$ROOT_DIR/systemd/$UNIT_NAME" "$USER_UNIT_DIR/$UNIT_NAME"
  chmod +x "$ROOT_DIR/recording_button_service.py"
  systemctl --user daemon-reload
  systemctl --user enable --now "$UNIT_NAME"
  if command -v loginctl >/dev/null 2>&1; then
    loginctl enable-linger "$USER" 2>/dev/null || true
  fi
  systemctl --user --no-pager status "$UNIT_NAME"
fi

escaped="${ROOT_DIR//&/\\&}"
if [[ "$ENABLE_POWER" == 1 || "$ENABLE_LED" == 1 ]]; then
  sudo install -d -m 0755 /etc/systemd/system /etc/systemd/logind.conf.d
fi
if [[ "$ENABLE_POWER" == 1 ]]; then
  power_tmp="$(mktemp)"
  sed "s|@APP_DIR@|$escaped|g" "$ROOT_DIR/systemd/$POWER_UNIT_NAME" > "$power_tmp"
  sudo install -m 0644 "$power_tmp" "/etc/systemd/system/$POWER_UNIT_NAME"
  rm -f "$power_tmp"
  sudo install -m 0644 "$ROOT_DIR/systemd/90-gwv3-power-key.conf" \
    /etc/systemd/logind.conf.d/90-gwv3-power-key.conf
  chmod +x "$ROOT_DIR/power_button_service.py"
fi
if [[ "$ENABLE_LED" == 1 ]]; then
  led_tmp="$(mktemp)"
  sed "s|@APP_DIR@|$escaped|g" "$ROOT_DIR/systemd/$LED_UNIT_NAME" > "$led_tmp"
  sudo install -m 0644 "$led_tmp" "/etc/systemd/system/$LED_UNIT_NAME"
  rm -f "$led_tmp"
  chmod +x "$ROOT_DIR/recording_led_service.py"
fi
if [[ "$ENABLE_POWER" == 1 || "$ENABLE_LED" == 1 ]]; then
  sudo systemctl daemon-reload
fi
if [[ "$ENABLE_POWER" == 1 ]]; then
  sudo systemctl enable --now "$POWER_UNIT_NAME"
  sudo systemctl --no-pager status "$POWER_UNIT_NAME"
fi
if [[ "$ENABLE_LED" == 1 ]]; then
  sudo systemctl enable --now "$LED_UNIT_NAME"
  sudo systemctl --no-pager status "$LED_UNIT_NAME"
fi

echo "recording controls installed for device_id=$DEVICE_ID"
if [[ "$ENABLE_POWER" == 1 ]]; then
  echo "HandlePowerKey=ignore is installed; reboot once if logind did not already use it."
fi
