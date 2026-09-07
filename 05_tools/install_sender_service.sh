#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG_SOURCE=""
RUN_USER="${SUDO_USER:-}"
RECEIVER_FALLBACK=""
CHRONY_SERVER=""
START_SERVICE=1
BUILD_SENDER=1
RUN_PREFLIGHT=1
ALLOW_DIRTY=0
BACKUP_DIR=""
INSTALL_COMMITTED=0

usage() {
  cat <<'EOF'
usage: sudo ./05_tools/install_sender_service.sh --config PATH [options]

Options:
  --run-user USER           Linux account that owns the camera process
  --receiver-fallback HOST  Override receiver.ip and clock_sync.receiver_ip
  --chrony-server HOST      Chrony source; defaults to the receiver fallback
  --no-start                Install and enable without starting the service
  --skip-build              Reuse the existing sender binary
  --skip-preflight          Skip hardware/runtime preflight
  --allow-dirty             Permit installation from a dirty Git worktree
EOF
}

while (($#)); do
  case "$1" in
    --config)
      CONFIG_SOURCE="${2:-}"
      shift 2
      ;;
    --run-user)
      RUN_USER="${2:-}"
      shift 2
      ;;
    --receiver-fallback)
      RECEIVER_FALLBACK="${2:-}"
      shift 2
      ;;
    --chrony-server)
      CHRONY_SERVER="${2:-}"
      shift 2
      ;;
    --no-start)
      START_SERVICE=0
      shift
      ;;
    --skip-build)
      BUILD_SENDER=0
      shift
      ;;
    --skip-preflight)
      RUN_PREFLIGHT=0
      shift
      ;;
    --allow-dirty)
      ALLOW_DIRTY=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

fail() {
  echo "sender installation failed: $1" >&2
  exit 1
}

rollback_on_exit() {
  local status=$?
  trap - EXIT
  if (( status != 0 && INSTALL_COMMITTED == 0 )) && [[ -n "$BACKUP_DIR" && -d "$BACKUP_DIR" ]]; then
    echo "sender installation failed; restoring the pre-install state from $BACKUP_DIR" >&2
    set +e
    "$ROOT_DIR/05_tools/rollback_sender_install.sh" "$BACKUP_DIR" >&2
    rollback_status=$?
    set -e
    if (( rollback_status != 0 )); then
      echo "automatic rollback failed; run sudo $ROOT_DIR/05_tools/rollback_sender_install.sh $BACKUP_DIR" >&2
    fi
  fi
  exit "$status"
}

trap rollback_on_exit EXIT

(( EUID == 0 )) || fail "run this installer with sudo"
[[ -n "$CONFIG_SOURCE" ]] || fail "--config is required"
CONFIG_SOURCE="$(realpath "$CONFIG_SOURCE")"
[[ -f "$CONFIG_SOURCE" ]] || fail "config does not exist: $CONFIG_SOURCE"
if [[ -z "$RUN_USER" ]]; then
  RUN_USER="$(stat -c '%U' "$ROOT_DIR")"
fi
id "$RUN_USER" >/dev/null 2>&1 || fail "Linux user does not exist: $RUN_USER"
RUN_HOME="$(getent passwd "$RUN_USER" | cut -d: -f6)"
[[ -n "$RUN_HOME" && -d "$RUN_HOME" ]] || fail "home directory is invalid for $RUN_USER"
command -v python3 >/dev/null 2>&1 || fail "python3 is required"
command -v systemctl >/dev/null 2>&1 || fail "systemctl is required"
command -v runuser >/dev/null 2>&1 || fail "runuser is required"

if git -C "$ROOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  COMMIT="$(git -C "$ROOT_DIR" rev-parse HEAD)"
  if [[ -n "$(git -C "$ROOT_DIR" status --porcelain)" && "$ALLOW_DIRTY" -ne 1 ]]; then
    fail "Git worktree is dirty; commit the release or use --allow-dirty for development only"
  fi
else
  COMMIT="unversioned"
fi

run_as_sender() {
  if [[ "$RUN_USER" == "root" ]]; then
    "$@"
  else
    /usr/sbin/runuser -u "$RUN_USER" -- "$@"
  fi
}

if (( BUILD_SENDER == 1 )); then
  run_as_sender cmake -S "$ROOT_DIR" -B "$ROOT_DIR/12_build" \
    -DGWV3_BUILD_RECEIVER=OFF -DGWV3_BUILD_SENDER=ON \
    -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
  run_as_sender cmake --build "$ROOT_DIR/12_build" -j2
fi
BIN="$ROOT_DIR/12_build/bin/gemini_sender"
[[ -x "$BIN" ]] || fail "sender binary is missing: $BIN"

install -d -m 0755 /etc/gwv3 /var/backups/gwv3
timestamp="$(date '+%Y%m%d-%H%M%S')"
BACKUP_DIR="$(mktemp -d "/var/backups/gwv3/sender-$timestamp.XXXX")"
chmod 0700 "$BACKUP_DIR"
TRACKED_PATHS=(
  /etc/gwv3/sender.json
  /etc/gwv3/sender.env
  /etc/gwv3/release.json
  /etc/systemd/system/gwv3-gemini-sender.service
  /usr/local/sbin/gwv3-sender-service-launcher
  /usr/local/sbin/gwv3-doctor
  /etc/chrony/chrony.conf
)
: > "$BACKUP_DIR/existing-paths.txt"
for path in "${TRACKED_PATHS[@]}"; do
  if [[ -e "$path" ]]; then
    printf '%s\n' "$path" >> "$BACKUP_DIR/existing-paths.txt"
    cp -a "$path" "$BACKUP_DIR/"
  fi
done

OLD_SENDER_UNITS=()
declare -A seen_sender_units=()
while IFS= read -r unit; do
  [[ -n "$unit" && -z "${seen_sender_units[$unit]:-}" ]] || continue
  OLD_SENDER_UNITS+=("$unit")
  seen_sender_units["$unit"]=1
done < <(
  {
    systemctl list-unit-files --type=service --no-legend 2>/dev/null \
      | awk '$1 ~ /^gwv3-gemini-sender(-.+)?\.service$/ {print $1}'
    systemctl list-unit-files gemini-sender.service --no-legend 2>/dev/null \
      | awk '{print $1}'
  } | sort -u
)
: > "$BACKUP_DIR/service-state.tsv"
for unit in "${OLD_SENDER_UNITS[@]}"; do
  enabled_state="$(systemctl is-enabled "$unit" 2>/dev/null || true)"
  active_state="$(systemctl is-active "$unit" 2>/dev/null || true)"
  printf '%s\t%s\t%s\n' "$unit" "${enabled_state:-unknown}" "${active_state:-unknown}" \
    >> "$BACKUP_DIR/service-state.tsv"
done

CONFIG_TMP="$(mktemp)"
python3 - "$CONFIG_SOURCE" "$CONFIG_TMP" "$RECEIVER_FALLBACK" <<'PY'
import json
import sys

source, destination, receiver = sys.argv[1:]
with open(source, "r", encoding="utf-8") as handle:
    config = json.load(handle)
if receiver:
    config.setdefault("receiver", {})["ip"] = receiver
    config.setdefault("clock_sync", {})["receiver_ip"] = receiver
with open(destination, "w", encoding="utf-8") as handle:
    json.dump(config, handle, ensure_ascii=True, indent=2)
    handle.write("\n")
PY
install -m 0644 "$CONFIG_TMP" /etc/gwv3/sender.json
rm -f "$CONFIG_TMP"

if [[ -z "$CHRONY_SERVER" ]]; then
  CHRONY_SERVER="$(python3 - /etc/gwv3/sender.json <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as handle:
    print((json.load(handle).get("receiver") or {}).get("ip") or "")
PY
)"
fi
[[ -n "$CHRONY_SERVER" ]] || fail "a Chrony server or receiver fallback is required"

"$BIN" --config /etc/gwv3/sender.json --validate-config
if (( RUN_PREFLIGHT == 1 )); then
  run_as_sender env GEMINI_SENDER_REQUIRE_USB=1 \
    "$ROOT_DIR/05_tools/sender_preflight.sh" /etc/gwv3/sender.json "安装" "no-local-preview"
fi

shell_assignment() {
  printf '%s=%q\n' "$1" "$2"
}
ENV_TMP="$(mktemp)"
{
  shell_assignment GWV3_ROOT "$ROOT_DIR"
  shell_assignment GWV3_CONFIG /etc/gwv3/sender.json
  shell_assignment GWV3_RUN_USER "$RUN_USER"
  shell_assignment GWV3_HOME "$RUN_HOME"
  shell_assignment GWV3_MODE no-local-preview
} > "$ENV_TMP"
install -m 0644 "$ENV_TMP" /etc/gwv3/sender.env
rm -f "$ENV_TMP"

install -m 0755 "$ROOT_DIR/05_tools/gwv3_sender_service_launcher.sh" \
  /usr/local/sbin/gwv3-sender-service-launcher
install -m 0755 "$ROOT_DIR/05_tools/gwv3_doctor.sh" /usr/local/sbin/gwv3-doctor
install -m 0644 "$ROOT_DIR/05_tools/systemd/gwv3-gemini-sender.service" \
  /etc/systemd/system/gwv3-gemini-sender.service

python3 - "$ROOT_DIR" "$CONFIG_SOURCE" "$COMMIT" <<'PY'
import datetime
import hashlib
import json
import pathlib
import sys

root, config_source, commit = sys.argv[1:]
config_bytes = pathlib.Path("/etc/gwv3/sender.json").read_bytes()
record = {
    "role": "sender",
    "commit": commit,
    "repository_root": root,
    "source_config": config_source,
    "effective_config": "/etc/gwv3/sender.json",
    "config_sha256": hashlib.sha256(config_bytes).hexdigest(),
    "installed_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
}
pathlib.Path("/etc/gwv3/release.json").write_text(
    json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8"
)
PY
chmod 0644 /etc/gwv3/release.json

if systemctl list-unit-files NetworkManager-wait-online.service --no-legend 2>/dev/null | grep -q .; then
  systemctl unmask NetworkManager-wait-online.service >/dev/null 2>&1 || true
  systemctl enable NetworkManager-wait-online.service >/dev/null 2>&1 || true
fi
"$ROOT_DIR/05_tools/setup_sender_chrony_client.sh" "$CHRONY_SERVER"

for legacy in "${OLD_SENDER_UNITS[@]}"; do
  [[ "$legacy" == "gwv3-gemini-sender.service" ]] && continue
  systemctl disable --now "$legacy" >/dev/null 2>&1 || true
done

systemctl daemon-reload
systemctl enable gwv3-gemini-sender.service
if (( START_SERVICE == 1 )); then
  systemctl restart gwv3-gemini-sender.service
  systemctl --no-pager --full status gwv3-gemini-sender.service
else
  systemctl stop gwv3-gemini-sender.service >/dev/null 2>&1 || true
fi

INSTALL_COMMITTED=1
trap - EXIT

echo "sender installation complete"
echo "  service: gwv3-gemini-sender.service"
echo "  config: /etc/gwv3/sender.json"
echo "  release: /etc/gwv3/release.json"
echo "  backup: $BACKUP_DIR"
