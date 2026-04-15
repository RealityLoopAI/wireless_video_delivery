#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/common.sh"

RECEIVER_DIR="${DELIVERY_ROOT}/03_receiver_linux"
CONFIG_FILE="${1:-${DELIVERY_ROOT}/06_configs/receiver.linux.default.json}"
PYTHON_BIN="$(default_python_bin)"

echo "[receiver-linux] project: ${RECEIVER_DIR}"
echo "[receiver-linux] config:  ${CONFIG_FILE}"
echo "[receiver-linux] python:  ${PYTHON_BIN}"

if [[ ! -f "${CONFIG_FILE}" ]]; then
  echo "[receiver-linux] ERROR: config not found: ${CONFIG_FILE}" >&2
  exit 2
fi

PYTHONPATH="${RECEIVER_DIR}${PYTHONPATH:+:${PYTHONPATH}}" \
"${PYTHON_BIN}" - <<'PY'
import importlib
mods = ["av", "cv2", "numpy", "wireless_video.receiver_app"]
failed = []
for m in mods:
    try:
        importlib.import_module(m)
    except Exception as e:
        failed.append((m, str(e)))
if failed:
    print("[receiver-linux] ERROR: import check failed")
    for name, err in failed:
        print(f"  - {name}: {err}")
    raise SystemExit(3)
print("[receiver-linux] OK: dependency import passed")
PY

"${PYTHON_BIN}" - "${CONFIG_FILE}" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1], "r", encoding="utf-8"))
ip = cfg.get("network", {}).get("listen_ip", "")
port = cfg.get("network", {}).get("listen_port", "")
print(f"[receiver-linux] listen: {ip}:{port}")
PY

echo "[receiver-linux] environment check passed"
