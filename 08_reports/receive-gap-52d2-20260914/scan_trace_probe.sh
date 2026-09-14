#!/bin/bash
set -euo pipefail
base=/sys/kernel/debug/tracing/instances/gwv3_scan_diag
mkdir "$base"
cleanup() {
  printf 0 > "$base/tracing_on"
  printf 0 > "$base/events/cfg80211/rdev_scan/enable"
  printf 0 > "$base/events/cfg80211/cfg80211_scan_done/enable"
  rmdir "$base"
}
trap cleanup EXIT
printf 0 > "$base/tracing_on"
printf 64 > "$base/buffer_size_kb"
printf 1 > "$base/events/cfg80211/rdev_scan/enable"
printf 1 > "$base/events/cfg80211/cfg80211_scan_done/enable"
printf 1 > "$base/tracing_on"
date -Iseconds
cat /proc/uptime
sleep 90
printf 0 > "$base/tracing_on"
cat "$base/trace"
date -Iseconds
