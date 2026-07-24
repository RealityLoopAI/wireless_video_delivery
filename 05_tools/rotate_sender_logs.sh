#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR="${GWV3_LOG_DIR:-$ROOT_DIR/08_reports/sender_logs}"
ARCHIVE_DIR="$LOG_DIR/archive"
MAX_LOG_BYTES="${GWV3_MAX_LOG_BYTES:-268435456}"
MAX_ARCHIVE_DAYS="${GWV3_MAX_ARCHIVE_DAYS:-30}"

mkdir -p "$LOG_DIR" "$ARCHIVE_DIR"

rotate_one() {
  local path="$1"
  [[ -f "$path" ]] || return 0
  local size
  size="$(stat -c '%s' "$path" 2>/dev/null || echo 0)"
  if ((size <= MAX_LOG_BYTES)); then
    return 0
  fi

  local stamp archived
  stamp="$(date +%Y%m%d_%H%M%S)"
  archived="$ARCHIVE_DIR/$(basename "$path").$stamp"
  cp --reflink=auto --preserve=timestamps "$path" "$archived"
  truncate -s 0 "$path"
  gzip -f "$archived" 2>/dev/null || true
}

rotate_one "$LOG_DIR/sender.log"
rotate_one "$LOG_DIR/sender_stdout.log"

find "$ARCHIVE_DIR" -type f -mtime +"$MAX_ARCHIVE_DAYS" -delete 2>/dev/null || true
