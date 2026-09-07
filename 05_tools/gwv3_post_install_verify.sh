#!/usr/bin/env bash
set -uo pipefail

ROLE="${1:-}"
RUN_USER="${2:-}"
CONFIG="${3:-}"
ROOT_DIR="${4:-}"
STATE_DIR=/var/lib/gwv3
REPORT_JSON="$STATE_DIR/deployment-report.json"
REPORT_TEXT="$STATE_DIR/deployment-report.txt"
PENDING="$STATE_DIR/post-install-pending"
mkdir -p "$STATE_DIR"

timestamp="$(date --iso-8601=seconds)"
status=failed
reason=""
doctor_output=""
acceptance_output=""

if [[ ! "$ROLE" =~ ^(sender|receiver)$ ]]; then
  reason="invalid role: $ROLE"
elif [[ ! -x "$ROOT_DIR/05_tools/gwv3_doctor.sh" ]]; then
  reason="repository tools are unavailable: $ROOT_DIR"
else
  for _ in {1..180}; do
    if [[ "$ROLE" == sender ]] && systemctl is-active --quiet gwv3-gemini-sender.service; then
      sleep 25
      break
    fi
    if [[ "$ROLE" == receiver ]] && curl -fsS --max-time 2 http://127.0.0.1:18080/api/status >/dev/null 2>&1; then
      break
    fi
    sleep 2
  done
  if [[ "$ROLE" == receiver ]]; then
    run_uid="$(id -u "$RUN_USER")"
    doctor_output="$(runuser -u "$RUN_USER" -- env XDG_RUNTIME_DIR="/run/user/$run_uid" \
      "$ROOT_DIR/05_tools/gwv3_doctor.sh" "$ROLE" "$CONFIG" 2>&1)"
  else
    doctor_output="$($ROOT_DIR/05_tools/gwv3_doctor.sh "$ROLE" "$CONFIG" 2>&1)"
  fi
  doctor_status=$?
  if ((doctor_status >= 2)) || [[ "$doctor_output" != *"SUMMARY "* ]]; then
    reason="gwv3-doctor reported a failure"
  elif [[ "$ROLE" == receiver ]]; then
    live_count="$(curl -fsS --max-time 3 http://127.0.0.1:18080/api/status 2>/dev/null \
      | python3 -c 'import json,sys; print(sum(1 for c in json.load(sys.stdin).get("cameras",[]) if c.get("live")))' \
      2>/dev/null || echo 0)"
    if [[ "$live_count" =~ ^[0-9]+$ ]] && ((live_count > 0)); then
      acceptance_output="$(python3 "$ROOT_DIR/05_tools/run_deployment_acceptance.py" \
        --admin http://127.0.0.1:18080 --record-seconds 30 --config "$CONFIG" \
        --cleanup-on-success --output "$STATE_DIR/deployment-acceptance.json" 2>&1)"
      acceptance_status=$?
      if ((acceptance_status != 0)); then
        reason="30-second receiver acceptance recording failed"
      else
        status=passed
      fi
    else
      acceptance_output="SKIPPED: no live camera was present after boot; run run_deployment_acceptance.py when Senders are online"
      status=passed
    fi
  else
    status=passed
  fi
fi

python3 - "$REPORT_JSON" "$ROLE" "$RUN_USER" "$CONFIG" "$timestamp" "$status" "$reason" <<'PY'
import json
import pathlib
import sys

path, role, user, config, timestamp, status, reason = sys.argv[1:]
pathlib.Path(path).write_text(
    json.dumps(
        {
            "schema": "gwv3_deployment_report_v1",
            "role": role,
            "run_user": user,
            "config": config,
            "checked_at": timestamp,
            "status": status,
            "reason": reason,
        },
        indent=2,
        sort_keys=True,
    )
    + "\n",
    encoding="utf-8",
)
PY
{
  printf 'GWV3 deployment verification\n'
  printf 'role=%s status=%s checked_at=%s\n' "$ROLE" "$status" "$timestamp"
  [[ -z "$reason" ]] || printf 'reason=%s\n' "$reason"
  printf '\n%s\n' "$doctor_output"
  [[ -z "$acceptance_output" ]] || printf '\nAcceptance:\n%s\n' "$acceptance_output"
} > "$REPORT_TEXT"
chmod 0644 "$REPORT_JSON" "$REPORT_TEXT"
rm -f "$PENDING"
if [[ "$status" == passed ]]; then
  exit 0
fi
exit 1
