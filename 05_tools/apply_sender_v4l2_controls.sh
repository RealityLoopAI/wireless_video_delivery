#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-$ROOT_DIR/06_configs/sender_rk3588-01_one_camera.json}"

if [[ ! -f "$CONFIG" ]]; then
  echo "v4l2 controls skipped: config not found: $CONFIG"
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "v4l2 controls skipped: python3 not found"
  exit 1
fi

requests="$(
  python3 - "$CONFIG" <<'PY'
import json
import sys

path = sys.argv[1]
with open(path, "r", encoding="utf-8") as handle:
    cfg = json.load(handle)

control_map = {
    "brightness": "brightness",
    "contrast": "contrast",
    "saturation": "saturation",
    "gamma": "gamma",
}

for cam in cfg.get("cameras") or []:
    controls = cam.get("color_controls") or {}
    pairs = []
    for cfg_key, v4l2_key in control_map.items():
        value = controls.get(cfg_key)
        if type(value) is int:
            pairs.append(f"{v4l2_key}={value}")
    if pairs:
        print(
            "\t".join(
                [
                    str(cam.get("camera_id") or ""),
                    str(cam.get("device_model") or ""),
                    ",".join(pairs),
                ]
            )
        )
PY
)"

if [[ -z "$requests" ]]; then
  exit 0
fi

if ! command -v v4l2-ctl >/dev/null 2>&1; then
  echo "v4l2 controls skipped: v4l2-ctl not found"
  exit 1
fi

candidate_video_nodes() {
  local model="$1"
  if [[ -n "$model" ]]; then
    v4l2-ctl --list-devices 2>/dev/null | awk -v model="$model" '
        /^[^[:space:]].*:$/ { capture = index($0, model) > 0; next }
        capture && /^[[:space:]]+\/dev\/video[0-9]+/ { print $1 }
      '
    return 0
  fi
  v4l2-ctl --list-devices 2>/dev/null | awk '
    /^[^[:space:]].*:$/ { capture = ($0 ~ /Orbbec|Gemini/); next }
    capture && /^[[:space:]]+\/dev\/video[0-9]+/ { print $1 }
  '
}

applied=0
requested=0
while IFS=$'\t' read -r camera_id model controls; do
  [[ -n "$controls" ]] || continue
  requested=$((requested + 1))
  mapfile -t nodes < <(candidate_video_nodes "$model")
  if [[ "${#nodes[@]}" -eq 0 ]]; then
    echo "v4l2 controls pending camera_id=${camera_id:-unknown} model=${model:-unknown}: video node not found"
    continue
  fi

  IFS=',' read -r -a pairs <<< "$controls"
  for node in "${nodes[@]}"; do
    ctrl_list="$(v4l2-ctl -d "$node" --list-ctrls 2>/dev/null || true)"
    supported=()
    for pair in "${pairs[@]}"; do
      name="${pair%%=*}"
      if grep -Eq "^[[:space:]]*${name}[[:space:]]" <<< "$ctrl_list"; then
        supported+=("$pair")
      fi
    done
    if [[ "${#supported[@]}" -eq 0 ]]; then
      continue
    fi

    set_arg="$(IFS=','; printf '%s' "${supported[*]}")"
    if v4l2-ctl -d "$node" --set-ctrl="$set_arg"; then
      echo "v4l2 controls applied camera_id=${camera_id:-unknown} node=$node controls=$set_arg"
      applied=$((applied + 1))
      break
    fi
    echo "v4l2 controls failed camera_id=${camera_id:-unknown} node=$node controls=$set_arg"
  done
done <<< "$requests"

if [[ "$requested" -gt 0 && "$applied" -eq 0 ]]; then
  exit 1
fi
