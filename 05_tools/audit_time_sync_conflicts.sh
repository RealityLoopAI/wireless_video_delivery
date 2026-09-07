#!/usr/bin/env bash
set -euo pipefail

MODE="check"
if [[ "${1:-}" == "--fix" ]]; then
  MODE="fix"
elif [[ -n "${1:-}" && "${1:-}" != "--check" ]]; then
  echo "usage: $0 [--check|--fix]" >&2
  exit 2
fi

SUDO=()
if [[ "$EUID" -ne 0 ]]; then
  SUDO=(sudo -n)
fi

CONFLICT_UNITS=(
  systemd-timesyncd.service
  ntp.service
  ntpd.service
  ntpsec.service
  openntpd.service
)
CRON_PATTERN='(^|[[:space:]/])(ntpdate|sntp)([[:space:]]|$)|(^|[[:space:]])date[[:space:]].*(-s|--set)([[:space:]]|=)'
timestamp="$(date '+%Y%m%d-%H%M%S')"
found=0

unit_exists() {
  systemctl list-unit-files "$1" --no-legend 2>/dev/null | grep -q .
}

unit_conflicts() {
  local unit="$1"
  unit_exists "$unit" || return 1
  systemctl is-active --quiet "$unit" 2>/dev/null \
    || systemctl is-enabled --quiet "$unit" 2>/dev/null
}

for unit in "${CONFLICT_UNITS[@]}"; do
  if ! unit_conflicts "$unit"; then
    continue
  fi
  found=1
  echo "time-sync conflict: $unit is active or enabled"
  if [[ "$MODE" == "fix" ]]; then
    "${SUDO[@]}" systemctl disable --now "$unit" >/dev/null 2>&1 || true
    "${SUDO[@]}" systemctl mask "$unit" >/dev/null 2>&1 || true
    echo "disabled and masked: $unit"
  fi
done

cron_files=()
while IFS= read -r path; do
  [[ -n "$path" ]] && cron_files+=("$path")
done < <(find /etc/cron.d /var/spool/cron/crontabs -maxdepth 1 -type f 2>/dev/null | sort)
[[ -f /etc/crontab ]] && cron_files+=(/etc/crontab)

for path in "${cron_files[@]}"; do
  matches="$("${SUDO[@]}" awk -v pattern="$CRON_PATTERN" '
    $0 !~ /^[[:space:]]*#/ && $0 ~ pattern {print NR ":" $0}
  ' "$path" 2>/dev/null || true)"
  [[ -n "$matches" ]] || continue
  found=1
  echo "time-sync conflict in $path:"
  printf '%s\n' "$matches" | sed 's/^/  /'
  if [[ "$MODE" == "fix" ]]; then
    backup="${path}.gwv3-backup.${timestamp}"
    "${SUDO[@]}" cp -a "$path" "$backup"
    tmp="$(mktemp)"
    "${SUDO[@]}" awk -v pattern="$CRON_PATTERN" '
      $0 !~ /^[[:space:]]*#/ && $0 ~ pattern {
        print "# disabled by gwv3 time-sync audit: " $0
        next
      }
      {print}
    ' "$path" > "$tmp"
    "${SUDO[@]}" tee "$path" < "$tmp" >/dev/null
    rm -f "$tmp"
    echo "disabled cron entries; backup=$backup"
  fi
done

custom_units="$("${SUDO[@]}" grep -RIlE \
  '^[[:space:]]*Exec(Start|StartPre)=.*(^|[[:space:]/])(ntpdate|sntp)([[:space:]]|$)|^[[:space:]]*Exec(Start|StartPre)=.*date[[:space:]].*(-s|--set)' \
  /etc/systemd/system 2>/dev/null || true)"
if [[ -n "$custom_units" ]]; then
  found=1
  echo "custom systemd time-setting commands require manual review:"
  printf '%s\n' "$custom_units" | sed 's/^/  /'
fi

if [[ "$MODE" == "fix" ]]; then
  remaining=0
  for unit in "${CONFLICT_UNITS[@]}"; do
    unit_conflicts "$unit" && remaining=1
  done
  for path in "${cron_files[@]}"; do
    if "${SUDO[@]}" awk -v pattern="$CRON_PATTERN" \
      '$0 !~ /^[[:space:]]*#/ && $0 ~ pattern {found=1} END {exit !found}' \
      "$path" 2>/dev/null; then
      remaining=1
    fi
  done
  if (( remaining == 0 )) && [[ -z "$custom_units" ]]; then
    echo "time-sync audit: only chrony remains"
    exit 0
  fi
  echo "time-sync audit: unresolved conflicts remain" >&2
  exit 1
fi

if (( found == 0 )); then
  echo "time-sync audit: no conflicts found"
  exit 0
fi
echo "time-sync audit: conflicts found; rerun with --fix after review" >&2
exit 1
