#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

exec python3 -u sherpa_kws.py \
  --keywords-file keywords_nihao_xiaohuan_variants.txt \
  --keywords-threshold 0.20 \
  listen \
  --record-device plughw:4,0 \
  --playback-device plughw:3,0
