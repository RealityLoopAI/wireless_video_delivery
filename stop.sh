#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

bash "${ROOT_DIR}/05_tools/stop_sender.sh" || true
bash "${ROOT_DIR}/05_tools/stop_receiver_linux.sh" || true
