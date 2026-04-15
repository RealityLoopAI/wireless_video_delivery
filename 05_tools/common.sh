#!/usr/bin/env bash
set -euo pipefail

TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DELIVERY_ROOT="$(cd "${TOOLS_DIR}/.." && pwd)"

default_python_bin() {
  if [[ -x "/home/berry/miniconda3/envs/orbbec_env/bin/python" ]]; then
    printf "%s\n" "/home/berry/miniconda3/envs/orbbec_env/bin/python"
    return 0
  fi
  if command -v python3 >/dev/null 2>&1; then
    command -v python3
    return 0
  fi
  if command -v python >/dev/null 2>&1; then
    command -v python
    return 0
  fi
  return 1
}

ensure_dir() {
  local target="$1"
  mkdir -p "${target}"
}

is_pid_running() {
  local pid="$1"
  if [[ -z "${pid}" ]]; then
    return 1
  fi
  if [[ ! "${pid}" =~ ^[0-9]+$ ]]; then
    return 1
  fi
  kill -0 "${pid}" >/dev/null 2>&1
}

write_json_with_overrides() {
  local template_json="$1"
  local output_json="$2"
  local key_path="$3"
  local value="$4"
  local pybin
  pybin="$(default_python_bin)"
  "${pybin}" - "$template_json" "$output_json" "$key_path" "$value" <<'PY'
import json
import sys

template_path, output_path, key_path, value = sys.argv[1:]
with open(template_path, "r", encoding="utf-8") as f:
    data = json.load(f)

cursor = data
keys = key_path.split(".")
for part in keys[:-1]:
    if part not in cursor or not isinstance(cursor[part], dict):
        cursor[part] = {}
    cursor = cursor[part]
leaf = keys[-1]

if value.isdigit():
    cursor[leaf] = int(value)
else:
    lowered = value.lower()
    if lowered == "true":
        cursor[leaf] = True
    elif lowered == "false":
        cursor[leaf] = False
    else:
        cursor[leaf] = value

with open(output_path, "w", encoding="utf-8") as f:
    json.dump(data, f, ensure_ascii=False, indent=2)
PY
}
