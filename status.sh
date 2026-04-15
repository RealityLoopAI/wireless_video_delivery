#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==== Receiver (Linux) ===="
bash "${ROOT_DIR}/05_tools/status_receiver_linux.sh" || true
echo
echo "==== Sender ===="
bash "${ROOT_DIR}/05_tools/status_sender.sh" || true
