#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

SENDER_DIR="${DELIVERY_ROOT}/01_sender_linux"
CONFIG_FILE="${1:-${DELIVERY_ROOT}/06_configs/sender.default.json}"
PYTHON_BIN="$(default_python_bin)"

echo "[sender] project: ${SENDER_DIR}"
echo "[sender] config:  ${CONFIG_FILE}"
echo "[sender] python:  ${PYTHON_BIN}"

if [[ ! -f "${CONFIG_FILE}" ]]; then
  echo "[sender] ERROR: config not found: ${CONFIG_FILE}" >&2
  exit 2
fi

PYTHONPATH="${SENDER_DIR}${PYTHONPATH:+:${PYTHONPATH}}" \
"${PYTHON_BIN}" - <<'PY'
import importlib
mods = ["av", "cv2", "numpy", "wireless_video.sender_app"]
failed = []
for m in mods:
    try:
        importlib.import_module(m)
    except Exception as e:
        failed.append((m, str(e)))
if failed:
    print("[sender] ERROR: import check failed")
    for name, err in failed:
        print(f"  - {name}: {err}")
    raise SystemExit(3)
print("[sender] OK: dependency import passed")
PY

"${PYTHON_BIN}" - "${CONFIG_FILE}" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1], "r", encoding="utf-8"))
ip = cfg.get("network", {}).get("remote_ip", "")
port = cfg.get("network", {}).get("remote_port", "")
w = cfg.get("camera", {}).get("width", "")
h = cfg.get("camera", {}).get("height", "")
fps = cfg.get("camera", {}).get("fps", "")
print(f"[sender] target: {ip}:{port}")
print(f"[sender] capture: {w}x{h}@{fps}")
PY

echo "[sender] environment check passed"
