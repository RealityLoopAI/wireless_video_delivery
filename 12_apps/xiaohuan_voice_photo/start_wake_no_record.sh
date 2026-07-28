#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

echo "wake listener starting; log: $ROOT_DIR/wake_runtime.log"
exec "$ROOT_DIR/run_wake_service.sh"
