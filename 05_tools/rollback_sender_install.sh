#!/usr/bin/env bash
set -euo pipefail

BACKUP_DIR="${1:-}"
if (( EUID != 0 )); then
  echo "run this rollback with sudo" >&2
  exit 1
fi
if [[ -z "$BACKUP_DIR" ]]; then
  BACKUP_DIR="$(find /var/backups/gwv3 -mindepth 1 -maxdepth 1 -type d -name 'sender-*' -printf '%T@ %p\n' 2>/dev/null \
    | sort -nr | awk 'NR==1 {$1=""; sub(/^ /, ""); print}')"
fi
[[ -n "$BACKUP_DIR" && -d "$BACKUP_DIR" ]] || {
  echo "sender backup directory was not found" >&2
  exit 1
}
[[ -f "$BACKUP_DIR/existing-paths.txt" ]] || {
  echo "invalid sender backup: missing existing-paths.txt" >&2
  exit 1
}

systemctl disable --now gwv3-gemini-sender.service >/dev/null 2>&1 || true

TRACKED_PATHS=(
  /etc/gwv3/sender.json
  /etc/gwv3/sender.env
  /etc/gwv3/release.json
  /etc/systemd/system/gwv3-gemini-sender.service
  /usr/local/sbin/gwv3-sender-service-launcher
  /usr/local/sbin/gwv3-doctor
  /etc/chrony/chrony.conf
)
for path in "${TRACKED_PATHS[@]}"; do
  if grep -Fxq "$path" "$BACKUP_DIR/existing-paths.txt"; then
    source_path="$BACKUP_DIR/$(basename "$path")"
    [[ -e "$source_path" ]] || {
      echo "backup payload missing: $source_path" >&2
      exit 1
    }
    cp -a "$source_path" "$path"
  else
    rm -f "$path"
  fi
done

systemctl daemon-reload
if [[ -s "$BACKUP_DIR/service-state.tsv" ]]; then
  while IFS=$'\t' read -r unit enabled_state active_state; do
    [[ -n "$unit" ]] || continue
    case "$enabled_state" in
      enabled|enabled-runtime)
        systemctl unmask "$unit" >/dev/null 2>&1 || true
        systemctl enable "$unit" >/dev/null 2>&1 || true
        ;;
      disabled)
        systemctl unmask "$unit" >/dev/null 2>&1 || true
        systemctl disable "$unit" >/dev/null 2>&1 || true
        ;;
      masked|masked-runtime)
        systemctl mask "$unit" >/dev/null 2>&1 || true
        ;;
    esac
    case "$active_state" in
      active|activating|reloading)
        systemctl restart "$unit" >/dev/null 2>&1 || true
        ;;
      *)
        systemctl stop "$unit" >/dev/null 2>&1 || true
        ;;
    esac
  done < "$BACKUP_DIR/service-state.tsv"
elif [[ -s "$BACKUP_DIR/legacy-services.txt" ]]; then
  # Compatibility with backups produced by the first installer revision.
  while IFS= read -r unit; do
    [[ -n "$unit" ]] || continue
    systemctl enable "$unit" >/dev/null 2>&1 || true
    systemctl restart "$unit" >/dev/null 2>&1 || true
  done < "$BACKUP_DIR/legacy-services.txt"
fi
if systemctl list-unit-files chrony.service --no-legend 2>/dev/null | grep -q .; then
  systemctl restart chrony.service
fi

echo "sender installation rolled back from $BACKUP_DIR"
