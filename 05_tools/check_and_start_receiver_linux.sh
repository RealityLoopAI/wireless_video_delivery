#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"${SCRIPT_DIR}/check_receiver_linux_env.sh"
"${SCRIPT_DIR}/start_receiver_linux_easy.sh" "${@}"
