#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROLE=""
RUN_USER="${SUDO_USER:-$USER}"
NON_INTERACTIVE=0
SKIP_PACKAGES=0
NO_REBOOT=0
ALLOW_DIRTY=0
OFFLINE_CACHE="${GWV3_OFFLINE_CACHE:-$ROOT_DIR/11_third_party/cache}"
RECEIVER_HOST=""
SENDER_ID=""
ROTATION=auto
BOARD=""
CAMERA=""
NAS_HOST=""
NAS_SHARE=""
NAS_USER=""
NAS_PASSWORD_FILE=""
WIFI_SSIDS=""
WIFI_PASSWORD_FILE=""
CREDENTIALS_TEMP=""

cleanup() {
  [[ -z "$CREDENTIALS_TEMP" ]] || rm -f "$CREDENTIALS_TEMP"
}
trap cleanup EXIT

usage() {
  cat <<'EOF'
GWV3 one-click deployment

usage: ./05_tools/bootstrap.sh --role sender|receiver [options]

Common:
  --run-user USER             Account that runs GWV3 services
  --offline-cache PATH        SDK/package cache from an offline bundle
  --wifi-ssids A,B            Additional saved Wi-Fi SSIDs (optional)
  --wifi-password-file PATH   Password for additional SSIDs (mode 600 recommended)
  --non-interactive           Never prompt; all required values must be supplied
  --skip-packages             Do not install apt dependencies
  --no-reboot                 Finish without the one-time reboot
  --allow-dirty               Development only: deploy an uncommitted worktree

Sender:
  --receiver-host HOST        Manual fallback when UDP/mDNS discovery is unavailable
  --sender-id ID              Preserve an approved existing ID; otherwise auto-generate
  --rotation auto|0|180       Keep profile orientation or explicitly override it
  --board NAME                Manual unknown-hardware override
  --camera NAME               Manual unknown-camera override

Receiver:
  --nas-host HOST             Initial NAS IPv4 address or hostname
  --nas-share SHARE           SMB share name
  --nas-user USER             SMB account
  --nas-password-file PATH    File containing only the SMB password
EOF
}

while (($#)); do
  case "$1" in
    --role) ROLE="${2:-}"; shift 2 ;;
    --run-user) RUN_USER="${2:-}"; shift 2 ;;
    --offline-cache) OFFLINE_CACHE="${2:-}"; shift 2 ;;
    --wifi-ssids) WIFI_SSIDS="${2:-}"; shift 2 ;;
    --wifi-password-file) WIFI_PASSWORD_FILE="${2:-}"; shift 2 ;;
    --receiver-host) RECEIVER_HOST="${2:-}"; shift 2 ;;
    --sender-id) SENDER_ID="${2:-}"; shift 2 ;;
    --rotation) ROTATION="${2:-}"; shift 2 ;;
    --board) BOARD="${2:-}"; shift 2 ;;
    --camera) CAMERA="${2:-}"; shift 2 ;;
    --nas-host) NAS_HOST="${2:-}"; shift 2 ;;
    --nas-share) NAS_SHARE="${2:-}"; shift 2 ;;
    --nas-user) NAS_USER="${2:-}"; shift 2 ;;
    --nas-password-file) NAS_PASSWORD_FILE="${2:-}"; shift 2 ;;
    --non-interactive) NON_INTERACTIVE=1; shift ;;
    --skip-packages) SKIP_PACKAGES=1; shift ;;
    --no-reboot) NO_REBOOT=1; shift ;;
    --allow-dirty) ALLOW_DIRTY=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

fail() { echo "GWV3 deployment failed: $1" >&2; exit 1; }
prompt() {
  local variable="$1" message="$2" secret="${3:-0}" value
  [[ -z "${!variable}" ]] || return 0
  ((NON_INTERACTIVE == 0)) || fail "$variable is required in non-interactive mode"
  if ((secret)); then read -r -s -p "$message: " value; printf '\n'; else read -r -p "$message: " value; fi
  [[ -n "$value" ]] || fail "$variable cannot be empty"
  printf -v "$variable" '%s' "$value"
}
as_root() { if ((EUID == 0)); then "$@"; else sudo "$@"; fi; }
as_user() {
  if ((EUID == 0)); then
    runuser -u "$RUN_USER" -- env HOME="$RUN_HOME" XDG_RUNTIME_DIR="/run/user/$(id -u "$RUN_USER")" "$@"
  else
    "$@"
  fi
}

if [[ -z "$ROLE" ]]; then prompt ROLE "Device role (sender/receiver)"; fi
[[ "$ROLE" == sender || "$ROLE" == receiver ]] || fail "--role must be sender or receiver"
[[ "$ROTATION" == auto || "$ROTATION" == 0 || "$ROTATION" == 180 ]] || fail "invalid rotation"
id "$RUN_USER" >/dev/null 2>&1 || fail "Linux user does not exist: $RUN_USER"
[[ "$RUN_USER" != root ]] || fail "GWV3 services must run under a regular Linux account"
if ((EUID != 0)) && [[ "$(id -un)" != "$RUN_USER" ]]; then
  fail "run as $RUN_USER or invoke through sudo from that account"
fi
RUN_HOME="$(getent passwd "$RUN_USER" | cut -d: -f6)"
[[ -d "$RUN_HOME" ]] || fail "invalid home for $RUN_USER"

OS_ID="$(. /etc/os-release; printf '%s' "$ID")"
OS_VERSION="$(. /etc/os-release; printf '%s' "$VERSION_ID")"
ARCH="$(uname -m)"
if [[ "$ROLE" == sender ]]; then
  [[ "$OS_ID" == ubuntu && "$OS_VERSION" == 22.04 && "$ARCH" == aarch64 ]] \
    || fail "supported Sender baseline is Ubuntu 22.04 arm64; found $OS_ID $OS_VERSION $ARCH"
else
  [[ "$OS_ID" == ubuntu && "$OS_VERSION" == 24.04 && "$ARCH" == x86_64 ]] \
    || fail "supported Receiver baseline is Ubuntu 24.04 x86_64; found $OS_ID $OS_VERSION $ARCH"
fi

install_packages() {
  local packages=()
  mapfile -t packages < <(python3 - "$ROOT_DIR/06_configs/deployment/package-manifest.json" "$ROLE" <<'PY'
import json, sys
manifest=json.load(open(sys.argv[1], encoding="utf-8"))
for package in manifest["common"] + manifest["roles"][sys.argv[2]]:
    print(package)
PY
)
  if compgen -G "$OFFLINE_CACHE/apt/*.deb" >/dev/null; then
    as_root apt-get install -y "$OFFLINE_CACHE"/apt/*.deb
  else
    as_root apt-get update
    as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y "${packages[@]}"
  fi
}

disable_automatic_updates() {
  local temporary
  temporary="$(mktemp)"
  cat > "$temporary" <<'EOF'
APT::Periodic::Enable "0";
APT::Periodic::Update-Package-Lists "0";
APT::Periodic::Unattended-Upgrade "0";
EOF
  as_root install -m 0644 "$temporary" /etc/apt/apt.conf.d/20auto-upgrades
  rm -f "$temporary"
  as_root systemctl disable --now unattended-upgrades.service apt-daily.timer apt-daily-upgrade.timer >/dev/null 2>&1 || true
}

configure_wifi_profiles() {
  command -v nmcli >/dev/null 2>&1 || return 0
  local active password="" ssid profile priority=80
  active="$(nmcli -t -f NAME connection show --active 2>/dev/null | head -n1 || true)"
  if [[ -n "$active" ]]; then
    as_root nmcli connection modify "$active" connection.autoconnect yes 802-11-wireless.powersave 2 || true
  fi
  [[ -n "$WIFI_SSIDS" ]] || return 0
  [[ -r "$WIFI_PASSWORD_FILE" ]] || fail "--wifi-password-file is required with --wifi-ssids"
  IFS= read -r password < "$WIFI_PASSWORD_FILE"
  [[ -n "$password" ]] || fail "Wi-Fi password file is empty"
  IFS=',' read -ra requested <<< "$WIFI_SSIDS"
  for ssid in "${requested[@]}"; do
    [[ -n "$ssid" ]] || continue
    profile="gwv3-$ssid"
    if ! nmcli -g NAME connection show | grep -Fxq "$profile"; then
      as_root nmcli connection add type wifi ifname '*' con-name "$profile" ssid "$ssid"
    fi
    as_root nmcli connection modify "$profile" connection.autoconnect yes \
      connection.autoconnect-priority "$priority" 802-11-wireless-security.key-mgmt wpa-psk \
      802-11-wireless-security.psk "$password" 802-11-wireless.powersave 2
    priority=$((priority - 1))
  done
}

if ((SKIP_PACKAGES == 0)); then install_packages; fi
disable_automatic_updates
as_root systemctl enable --now avahi-daemon.service >/dev/null 2>&1 || true
configure_wifi_profiles

if [[ "$ROLE" == sender ]]; then
  DETECTION="$(python3 "$ROOT_DIR/05_tools/gwv3_provision.py" detect --json)"
  DETECTED_BOARD="$(jq -r .board <<<"$DETECTION")"
  DETECTED_CAMERA="$(jq -r .camera.family <<<"$DETECTION")"
  [[ -n "$BOARD" ]] || BOARD="$DETECTED_BOARD"
  [[ -n "$CAMERA" ]] || CAMERA="$DETECTED_CAMERA"
  if [[ "$BOARD" == unknown ]]; then prompt BOARD "Unknown board; enter rk3588, orangepi5pro, or lubancat"; fi
  if [[ "$CAMERA" == unknown || "$CAMERA" == none ]]; then prompt CAMERA "Unknown camera; enter sv1301s or gemini305"; fi

  if [[ -z "$RECEIVER_HOST" ]]; then
    DISCOVERY="$(python3 "$ROOT_DIR/05_tools/gwv3_provision.py" discover-receiver --timeout-ms 2000 2>/dev/null || true)"
    if [[ -n "$DISCOVERY" ]]; then RECEIVER_HOST="$(jq -r .host <<<"$DISCOVERY")"; fi
  fi
  if [[ -z "$RECEIVER_HOST" ]] && getent hosts gwv3-receiver.local >/dev/null 2>&1; then
    RECEIVER_HOST=gwv3-receiver.local
  fi
  prompt RECEIVER_HOST "Receiver IP or hostname"

  if [[ -z "$SENDER_ID" ]]; then
    SENDER_ID="$(python3 "$ROOT_DIR/05_tools/gwv3_provision.py" identity --board "$BOARD" | jq -r .sender_id)"
  fi
  PROFILE_JSON="$(python3 - "$ROOT_DIR/06_configs/deployment/sender-profiles.json" "$BOARD" "$CAMERA" <<'PY'
import json, sys
profiles=json.load(open(sys.argv[1], encoding="utf-8"))["profiles"]
match=[p for p in profiles if p["board"] == sys.argv[2] and p["camera"] == sys.argv[3]]
if len(match) != 1: raise SystemExit(2)
print(json.dumps(match[0]))
PY
)" || fail "no approved profile for board=$BOARD camera=$CAMERA"
  SDK_GENERATION="$(jq -r .sdk <<<"$PROFILE_JSON")"
  CONFIG_TEMP="$(mktemp)"
  python3 "$ROOT_DIR/05_tools/gwv3_provision.py" sender-config --root "$ROOT_DIR" \
    --profiles "$ROOT_DIR/06_configs/deployment/sender-profiles.json" --output "$CONFIG_TEMP" \
    --sender-id "$SENDER_ID" --rotation "$ROTATION" --receiver-host "$RECEIVER_HOST" \
    --board "$BOARD" --camera "$CAMERA" >/dev/null
  SDK_ROOT="$(as_root "$ROOT_DIR/05_tools/install_orbbec_sdk.sh" --generation "$SDK_GENERATION" --cache-dir "$OFFLINE_CACHE")"
  INSTALL_ARGS=(--config "$CONFIG_TEMP" --run-user "$RUN_USER" --receiver-fallback "$RECEIVER_HOST" --chrony-server "$RECEIVER_HOST" --sdk-root "$SDK_ROOT")
  ((ALLOW_DIRTY == 0)) || INSTALL_ARGS+=(--allow-dirty)
  as_root "$ROOT_DIR/05_tools/install_sender_service.sh" "${INSTALL_ARGS[@]}"
  rm -f "$CONFIG_TEMP"
  as_root "$ROOT_DIR/05_tools/install_sender_wifi_tuning.sh"
  as_root "$ROOT_DIR/05_tools/install_chrony_receiver_watch.sh" "$RUN_USER" "$RECEIVER_HOST" "$ROOT_DIR"
  BUTTON_CAPABLE="$(jq -r .recording_buttons_available <<<"$DETECTION")"
  POWER_CAPABLE="$(jq -r .power_button_available <<<"$DETECTION")"
  LED_CAPABLE="$(jq -r .recording_led_available <<<"$DETECTION")"
  if [[ "$BOARD" == lubancat && ( "$BUTTON_CAPABLE" == true || "$POWER_CAPABLE" == true || "$LED_CAPABLE" == true ) ]]; then
    BUTTON_TEMP="$(mktemp)"
    POWER_TEMP="$(mktemp)"
    python3 "$ROOT_DIR/05_tools/gwv3_provision.py" button-config --root "$ROOT_DIR" \
      --sender-id "$SENDER_ID" --button-output "$BUTTON_TEMP" --power-output "$POWER_TEMP"
    chmod 0644 "$BUTTON_TEMP" "$POWER_TEMP"
    as_user env GWV3_BUTTON_CONFIG="$BUTTON_TEMP" GWV3_POWER_CONFIG="$POWER_TEMP" \
      GWV3_ENABLE_BUTTONS="$([[ "$BUTTON_CAPABLE" == true ]] && echo 1 || echo 0)" \
      GWV3_ENABLE_POWER="$([[ "$POWER_CAPABLE" == true ]] && echo 1 || echo 0)" \
      GWV3_ENABLE_LED="$([[ "$LED_CAPABLE" == true ]] && echo 1 || echo 0)" \
      "$ROOT_DIR/12_apps/recording_buttons/install_service.sh" "$SENDER_ID"
    rm -f "$BUTTON_TEMP" "$POWER_TEMP"
  fi
  if [[ "$(jq -r .audio.supported_pair <<<"$DETECTION")" == true ]]; then
    as_user "$ROOT_DIR/05_tools/install_optional_voice.sh" --cache-dir "$OFFLINE_CACHE"
  fi
  as_root loginctl enable-linger "$RUN_USER"
  as_root hostnamectl set-hostname "$SENDER_ID"
  as_root "$ROOT_DIR/05_tools/install_post_boot_verify.sh" sender "$RUN_USER" /etc/gwv3/sender.json "$ROOT_DIR"
else
  prompt NAS_HOST "NAS IP or hostname"
  prompt NAS_SHARE "NAS SMB share"
  prompt NAS_USER "NAS SMB username"
  NAS_PASSWORD=""
  if [[ -n "$NAS_PASSWORD_FILE" ]]; then
    [[ -r "$NAS_PASSWORD_FILE" ]] || fail "NAS password file is unreadable"
    IFS= read -r NAS_PASSWORD < "$NAS_PASSWORD_FILE"
  else
    prompt NAS_PASSWORD "NAS SMB password" 1
  fi
  [[ -n "$NAS_PASSWORD" ]] || fail "NAS password is empty"

  CONFIG_TEMP="$(mktemp)"
  python3 "$ROOT_DIR/05_tools/gwv3_provision.py" receiver-config \
    --template "$ROOT_DIR/06_configs/receiver_loop.json" --output "$CONFIG_TEMP" \
    --home "$RUN_HOME" --uid "$(id -u "$RUN_USER")" --gid "$(id -g "$RUN_USER")" >/dev/null
  as_root install -d -m 0755 /etc/gwv3 /var/lib/gwv3
  as_root install -m 0644 "$CONFIG_TEMP" /etc/gwv3/receiver.json
  rm -f "$CONFIG_TEMP"
  AUDIO_CONFIG_TEMP="$(mktemp)"
  python3 "$ROOT_DIR/05_tools/gwv3_provision.py" relocate-json \
    --template "$ROOT_DIR/06_configs/audio_archive_receiver_loop.json" \
    --output "$AUDIO_CONFIG_TEMP" --old-prefix /home/loop --new-prefix "$RUN_HOME" >/dev/null
  as_root install -m 0644 "$AUDIO_CONFIG_TEMP" /etc/gwv3/audio_archive_receiver.json
  rm -f "$AUDIO_CONFIG_TEMP"
  CREDENTIALS_TEMP="$(mktemp)"
  printf 'username=%s\npassword=%s\n' "$NAS_USER" "$NAS_PASSWORD" > "$CREDENTIALS_TEMP"
  chmod 0600 "$CREDENTIALS_TEMP"
  as_root install -m 0600 "$CREDENTIALS_TEMP" /etc/gwv3/nas-credentials
  rm -f "$CREDENTIALS_TEMP"
  unset NAS_PASSWORD
  TARGET_TEMP="$(mktemp)"
  python3 "$ROOT_DIR/05_tools/gwv3_provision.py" nas-target --output "$TARGET_TEMP" \
    --host "$NAS_HOST" --share "$NAS_SHARE" >/dev/null
  as_root install -m 0600 "$TARGET_TEMP" /var/lib/gwv3/nas-target.json
  rm -f "$TARGET_TEMP"

  as_root "$ROOT_DIR/05_tools/setup_receiver_chrony_server.sh" auto-private
  as_root "$ROOT_DIR/05_tools/install_receiver_network_tuning.sh"
  as_root "$ROOT_DIR/05_tools/install_receiver_nas_auto_mount.sh" /etc/gwv3/receiver.json
  RECEIVER_INSTALL_ENV=(GWV3_AUDIO_ARCHIVE_CONFIG=/etc/gwv3/audio_archive_receiver.json)
  if compgen -G "$OFFLINE_CACHE/pip/*" >/dev/null; then
    RECEIVER_INSTALL_ENV+=(GWV3_PIP_CACHE="$OFFLINE_CACHE/pip")
  fi
  as_user env "${RECEIVER_INSTALL_ENV[@]}" \
    "$ROOT_DIR/05_tools/install_receiver_autostart.sh" /etc/gwv3/receiver.json
  as_root loginctl enable-linger "$RUN_USER"
  as_root hostnamectl set-hostname gwv3-receiver
  as_root "$ROOT_DIR/05_tools/install_post_boot_verify.sh" receiver "$RUN_USER" /etc/gwv3/receiver.json "$ROOT_DIR"
fi

echo "GWV3 deployment installed. Post-boot report: /var/lib/gwv3/deployment-report.txt"
if ((NO_REBOOT == 1)); then
  echo "Reboot skipped. Run: sudo systemctl start gwv3-post-install-verify.service"
else
  echo "Rebooting once to verify startup and hardware recovery..."
  as_root systemctl reboot
fi
