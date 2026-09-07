#!/usr/bin/env bash
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROLE="${1:-auto}"
CONFIG="${2:-}"
PASS_COUNT=0
WARN_COUNT=0
FAIL_COUNT=0

usage() {
  echo "usage: $0 [auto|sender|receiver|nas] [config]"
}

pass() {
  PASS_COUNT=$((PASS_COUNT + 1))
  printf 'PASS  %s\n' "$*"
}

warn() {
  WARN_COUNT=$((WARN_COUNT + 1))
  printf 'WARN  %s\n' "$*"
}

fail() {
  FAIL_COUNT=$((FAIL_COUNT + 1))
  printf 'FAIL  %s\n' "$*"
}

json_value() {
  local path="$1" dotted="$2"
  python3 - "$path" "$dotted" <<'PY' 2>/dev/null
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as handle:
    value = json.load(handle)
for key in sys.argv[2].split("."):
    value = value[key]
if isinstance(value, bool):
    print("true" if value else "false")
else:
    print(value)
PY
}

if [[ -r /etc/gwv3/release.json ]]; then
  installed_root="$(json_value /etc/gwv3/release.json repository_root || true)"
  if [[ -n "$installed_root" && -d "$installed_root" ]]; then
    ROOT_DIR="$installed_root"
  fi
fi

if [[ "$ROLE" == "-h" || "$ROLE" == "--help" ]]; then
  usage
  exit 0
fi
if [[ "$ROLE" == "auto" ]]; then
  if [[ -r /etc/gwv3/release.json ]]; then
    ROLE="$(json_value /etc/gwv3/release.json role || true)"
  elif systemctl list-unit-files gwv3-gemini-sender.service --no-legend 2>/dev/null | grep -q .; then
    ROLE="sender"
  elif systemctl --user list-unit-files gwv3-gemini-receiver.service --no-legend 2>/dev/null | grep -q .; then
    ROLE="receiver"
  elif systemctl list-unit-files gwv3-nas-discovery.service --no-legend 2>/dev/null | grep -q .; then
    ROLE="nas"
  else
    echo "cannot infer device role; specify sender, receiver, or nas" >&2
    exit 2
  fi
fi
if [[ "$ROLE" != "sender" && "$ROLE" != "receiver" && "$ROLE" != "nas" ]]; then
  usage >&2
  exit 2
fi

echo "GWV3 doctor role=$ROLE host=$(hostname) time=$(date --iso-8601=seconds)"
echo

if [[ -r /etc/gwv3/release.json ]]; then
  release_commit="$(json_value /etc/gwv3/release.json commit || true)"
  release_hash="$(json_value /etc/gwv3/release.json config_sha256 || true)"
  release_root="$(json_value /etc/gwv3/release.json repository_root || true)"
  [[ -n "$release_commit" ]] && pass "release metadata commit=$release_commit" \
    || warn "release metadata does not contain a commit"
  if [[ -n "$release_root" ]] && git -C "$release_root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    current_commit="$(git -C "$release_root" rev-parse HEAD 2>/dev/null || true)"
    if [[ "$current_commit" == "$release_commit" ]]; then
      pass "repository HEAD matches the installed release"
    else
      fail "repository HEAD=$current_commit differs from installed commit=$release_commit"
    fi
    if [[ -z "$(git -C "$release_root" status --porcelain 2>/dev/null)" ]]; then
      pass "installed repository worktree is clean"
    else
      warn "installed repository worktree has uncommitted changes"
    fi
  else
    fail "installed repository is unavailable: ${release_root:-unset}"
  fi
else
  warn "release metadata is absent: /etc/gwv3/release.json"
fi

available_kib="$(awk '/MemAvailable:/ {print $2}' /proc/meminfo 2>/dev/null || true)"
if [[ "$available_kib" =~ ^[0-9]+$ ]] && (( available_kib >= 262144 )); then
  pass "available memory=$((available_kib / 1024)) MiB"
else
  fail "available memory is below 256 MiB"
fi
root_free_kib="$(df -Pk / 2>/dev/null | awk 'NR==2 {print $4}')"
if [[ "$root_free_kib" =~ ^[0-9]+$ ]] && (( root_free_kib >= 1048576 )); then
  pass "root filesystem free=$((root_free_kib / 1024)) MiB"
else
  fail "root filesystem has less than 1 GiB free"
fi

if command -v chronyc >/dev/null 2>&1 && systemctl is-active --quiet chrony.service; then
  leap="$(chronyc tracking 2>/dev/null | awk -F: '/Leap status/ {gsub(/^[ \t]+/, "", $2); print $2}')"
  if [[ "$leap" == "Normal" ]]; then
    pass "chrony is active and synchronized"
  else
    warn "chrony is active but Leap status is ${leap:-unknown}"
  fi
  if chronyc waitsync 1 0.010 100 0.1 >/dev/null 2>&1; then
    pass "chrony correction is within 10 ms"
  else
    fail "chrony correction is not within 10 ms"
  fi
else
  fail "chrony is not active"
fi
if "$ROOT_DIR/05_tools/audit_time_sync_conflicts.sh" --check >/tmp/gwv3-doctor-time-audit.$$ 2>&1; then
  pass "no competing time synchronization mechanism"
else
  fail "competing time synchronization mechanism detected"
  sed 's/^/      /' /tmp/gwv3-doctor-time-audit.$$
fi
rm -f /tmp/gwv3-doctor-time-audit.$$

if [[ "$ROLE" == "sender" ]]; then
  [[ -n "$CONFIG" ]] || CONFIG=/etc/gwv3/sender.json
  if [[ -r "$CONFIG" ]] && python3 -m json.tool "$CONFIG" >/dev/null 2>&1; then
    pass "sender config is valid JSON: $CONFIG"
    if [[ -n "${release_hash:-}" ]]; then
      current_hash="$(sha256sum "$CONFIG" | awk '{print $1}')"
      if [[ "$current_hash" == "$release_hash" ]]; then
        pass "effective config matches release metadata"
      else
        fail "effective config differs from the installed release metadata"
      fi
    fi
  else
    fail "sender config is missing or invalid: $CONFIG"
  fi

  if [[ -x "$ROOT_DIR/12_build/bin/gemini_sender" ]] \
    && "$ROOT_DIR/12_build/bin/gemini_sender" --config "$CONFIG" --validate-config >/dev/null 2>&1; then
    pass "sender binary accepts the effective config"
  else
    fail "sender binary/config validation failed"
  fi

  service_exec="$(systemctl show gwv3-gemini-sender.service -p ExecStart --value 2>/dev/null || true)"
  if [[ "$service_exec" == *"/usr/local/sbin/gwv3-sender-service-launcher --run"* ]]; then
    pass "sender service uses the generic launcher"
  else
    warn "sender service is a legacy device-specific unit"
  fi
  if systemctl is-enabled --quiet gwv3-gemini-sender.service 2>/dev/null; then
    pass "sender service is enabled"
  else
    warn "generic sender service is not enabled; this may be a legacy deployment"
  fi
  if systemctl is-active --quiet gwv3-gemini-sender.service 2>/dev/null \
    || systemctl list-units --state=running --no-legend 2>/dev/null | grep -qE 'gemini-sender|gwv3-gemini-sender'; then
    pass "sender service is active"
  else
    fail "sender service is not active"
  fi

  if lsusb 2>/dev/null | grep -q '2bc5:'; then
    pass "Orbbec USB device is present"
    if lsusb -t 2>/dev/null | grep -q '5000M\|10000M\|20000M'; then
      pass "a USB 3.x bus is active"
    else
      warn "Orbbec is present but no active USB 3.x bus was found"
    fi
  else
    fail "Orbbec USB device is absent"
  fi

  route_target="$(json_value "$CONFIG" receiver.ip || true)"
  route_line="$(ip route get "$route_target" 2>/dev/null | head -n1 || true)"
  if [[ -n "$route_line" ]]; then
    pass "receiver fallback is routable: $route_line"
  else
    warn "receiver fallback is not currently routable: ${route_target:-unset}"
  fi
  if [[ "$route_line" == *" dev wlan"* ]]; then
    iface="$(awk '{for(i=1;i<=NF;i++) if($i=="dev") {print $(i+1); exit}}' <<<"$route_line")"
    freq="$(iw dev "$iface" link 2>/dev/null | awk '/freq:/ {print $2; exit}')"
    if [[ "$freq" =~ ^[0-9]+$ ]] && (( freq >= 5000 )); then
      pass "$iface is connected on 5 GHz (${freq} MHz)"
    else
      fail "$iface is not connected on the required 5 GHz band"
    fi
    power_save="$(iw dev "$iface" get power_save 2>/dev/null | awk '{print $3}')"
    [[ "$power_save" == "off" ]] && pass "$iface power saving is off" \
      || warn "$iface power saving is not confirmed off"
  fi

  sender_log="$(json_value "$CONFIG" logging.directory || true)"
  [[ "$sender_log" == /* ]] || sender_log="$ROOT_DIR/${sender_log:-08_reports/sender_logs}"
  sender_log="$sender_log/sender.log"
  perf_line="$(grep 'perf camera_id=' "$sender_log" 2>/dev/null | tail -n1 || true)"
  if [[ -n "$perf_line" ]]; then
    rgb_fps="$(sed -n 's/.*rgb_input_fps=\([0-9.]*\).*/\1/p' <<<"$perf_line")"
    depth_fps="$(sed -n 's/.*depth_input_fps=\([0-9.]*\).*/\1/p' <<<"$perf_line")"
    if python3 - "$rgb_fps" "$depth_fps" <<'PY' 2>/dev/null
import sys
sys.exit(0 if max(float(sys.argv[1]), float(sys.argv[2])) > 1.0 else 1)
PY
    then
      pass "latest capture health rgb_fps=$rgb_fps depth_fps=$depth_fps"
    else
      fail "latest capture health reports zero FPS"
    fi
  else
    fail "no sender perf record found in $sender_log"
  fi
  clock_line="$(grep 'clock_sync sender_id=' "$sender_log" 2>/dev/null | tail -n1 || true)"
  if [[ "$clock_line" == *"healthy=true"* ]]; then
    clock_offset="$(sed -n 's/.*offset_us=\(-\{0,1\}[0-9]*\).*/\1/p' <<<"$clock_line")"
    clock_delay="$(sed -n 's/.*delay_us=\(-\{0,1\}[0-9]*\).*/\1/p' <<<"$clock_line")"
    pass "CLOCK_SYNC is healthy offset_us=${clock_offset:-unknown} delay_us=${clock_delay:-unknown}"
  elif [[ -n "$clock_line" ]]; then
    fail "latest CLOCK_SYNC record is unhealthy"
  else
    warn "no CLOCK_SYNC health record found in $sender_log"
  fi

  sender_home="$HOME"
  if [[ -r /etc/gwv3/sender.env ]]; then
    sender_home="$(bash -c 'source /etc/gwv3/sender.env; printf "%s" "${GWV3_HOME:-}"' 2>/dev/null || true)"
    [[ -n "$sender_home" ]] || sender_home="$HOME"
  fi
  target_state="${XDG_STATE_HOME:-$sender_home/.local/state}/gwv3/receiver_target.json"
  if [[ -r "$target_state" ]]; then
    target_host="$(json_value "$target_state" receiver_host || json_value "$target_state" host || true)"
    pass "receiver discovery state is present target=${target_host:-unknown}"
  else
    warn "receiver discovery has not persisted a target for this user"
  fi
elif [[ "$ROLE" == "receiver" ]]; then
  [[ -n "$CONFIG" ]] || CONFIG="$ROOT_DIR/06_configs/receiver_loop.json"
  if [[ -r "$CONFIG" ]] && python3 -m json.tool "$CONFIG" >/dev/null 2>&1; then
    pass "receiver config is valid JSON: $CONFIG"
  else
    fail "receiver config is missing or invalid: $CONFIG"
  fi
  for unit in gwv3-gemini-receiver.service gwv3-web-monitor.service gwv3-recording-uploader.service; do
    if systemctl --user is-active --quiet "$unit" 2>/dev/null; then
      pass "$unit is active"
    else
      fail "$unit is not active"
    fi
  done
  admin_port="$(json_value "$CONFIG" admin_port || true)"
  status="$(curl -fsS --max-time 3 "http://127.0.0.1:${admin_port:-18080}/api/status" 2>/dev/null || true)"
  if [[ -n "$status" ]]; then
    pass "receiver admin API responds"
    if python3 -c 'import json,sys; d=json.load(sys.stdin); sys.exit(0 if int(d.get("record_queue_total_bytes") or 0) == 0 else 1)' <<<"$status"; then
      pass "receiver recording queue is empty"
    else
      warn "receiver recording queue is not empty"
    fi
  else
    fail "receiver admin API is unavailable"
  fi
  nas_root="$(json_value "$CONFIG" nas_root || true)"
  if [[ -n "$nas_root" ]] && mountpoint -q "$nas_root" && [[ -w "$nas_root" ]]; then
    pass "NAS mount is writable: $nas_root"
  else
    warn "NAS mount is not currently writable: ${nas_root:-unset}"
  fi
else
  if systemctl is-active --quiet smbd.service 2>/dev/null; then
    pass "SMB service is active"
  else
    fail "SMB service is not active"
  fi
  if systemctl is-enabled --quiet gwv3-nas-discovery.service 2>/dev/null \
    && systemctl is-active --quiet gwv3-nas-discovery.service 2>/dev/null; then
    pass "NAS discovery service is enabled and active"
  else
    fail "NAS discovery service is not enabled and active"
  fi
  if [[ -r /etc/gwv3/nas-beacon.json ]] && python3 -m json.tool /etc/gwv3/nas-beacon.json >/dev/null 2>&1; then
    pass "NAS discovery config is valid"
  else
    fail "NAS discovery config is missing or invalid"
  fi
fi

echo
echo "SUMMARY pass=$PASS_COUNT warn=$WARN_COUNT fail=$FAIL_COUNT"
if (( FAIL_COUNT > 0 )); then
  exit 2
fi
if (( WARN_COUNT > 0 )); then
  exit 1
fi
exit 0
