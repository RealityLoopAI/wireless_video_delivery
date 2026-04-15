#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

"${SCRIPT_DIR}/check_sender_env.sh"
"${SCRIPT_DIR}/start_sender_easy.sh" "${@}"
