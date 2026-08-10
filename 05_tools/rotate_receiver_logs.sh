#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG_DIR="${GWV3_LOG_DIR:-$ROOT_DIR/08_reports/receiver_logs}"
ARCHIVE_DIR="$LOG_DIR/archive"
MAX_LOG_BYTES="${GWV3_MAX_LOG_BYTES:-67108864}"
MAX_ARCHIVE_DAYS="${GWV3_MAX_ARCHIVE_DAYS:-14}"

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
  # Services keep stdout log descriptors open. Copy-and-truncate preserves the
  # inode they are writing to; renaming would leave new logs in the archive.
  cp --reflink=auto --preserve=timestamps "$path" "$archived"
  truncate -s 0 "$path"
  gzip -f "$archived" 2>/dev/null || true
}

rotate_one "$LOG_DIR/receiver.log"
rotate_one "$LOG_DIR/receiver_stdout.log"
rotate_one "$LOG_DIR/web_stdout.log"
rotate_one "$LOG_DIR/recording_uploader.log"
rotate_one "$LOG_DIR/photo_uploader.log"

find "$ARCHIVE_DIR" -type f -mtime +"$MAX_ARCHIVE_DAYS" -delete 2>/dev/null || true
